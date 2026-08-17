// Oil, stage 3 (IMPaSTo §4.1.1): conservative advection of the paint slab.
//
// Volume and pigment are advected with the same donor weights, so pigment stays
// consistent with the volume carrying it. Pigment is stored premultiplied by
// volume, which makes the transported quantity linear and therefore a
// Kubelka-Munk-correct mix on arrival.
//#include "include/common.wgsl"
//#include "include/donor.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(4) var waterDst : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(6) var pigRDst  : texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  var here = textureLoad(waterSrc, p, 0);

  var vol = 0.0;
  var accC = vec4<f32>(0.0);
  var accR = vec4<f32>(0.0);

  for (var oy = -1; oy <= 1; oy = oy + 1) {
    for (var ox = -1; ox <= 1; ox = ox + 1) {
      let n = p + vec2<i32>(ox, oy);
      if (!inBounds(n, res)) { continue; }
      let wn = textureLoad(waterSrc, n, 0);
      let w = donorWeight(wn.xy, vec2<f32>(f32(-ox), f32(-oy)));
      if (w > 0.0) {
        vol = vol + w * wn.z;
        accC = accC + w * textureLoad(pigCSrc, n, 0);
        accR = accR + w * textureLoad(pigRSrc, n, 0);
      }
    }
  }

  // --- levelling ---
  // Wet oil relaxes under its own surface tension. Not in IMPaSTo, but without
  // it the per-frame brush stamps leave periodic ridges in the height field that
  // the impasto lighting turns into regular arcs down the stroke. Volume and
  // pigment are smoothed with the same symmetric stencil, so the exchange is
  // conservative and pigment stays consistent with the volume carrying it.
  let lev = clamp(P.viscosity * P.dt, 0.0, 0.25);
  if (lev > 0.0) {
    var dVol = 0.0;
    var dC = vec4<f32>(0.0);
    var dR = vec4<f32>(0.0);
    let offs = array<vec2<i32>, 4>(
      vec2<i32>(-1, 0), vec2<i32>(1, 0), vec2<i32>(0, -1), vec2<i32>(0, 1)
    );
    for (var i = 0; i < 4; i = i + 1) {
      let q = clampCoord(p + offs[i], res);
      let wq = textureLoad(waterSrc, q, 0);
      dVol = dVol + (wq.z - here.z);
      dC = dC + (textureLoad(pigCSrc, q, 0) - textureLoad(pigCSrc, p, 0));
      dR = dR + (textureLoad(pigRSrc, q, 0) - textureLoad(pigRSrc, p, 0));
    }
    vol = vol + lev * 0.25 * dVol;
    accC = accC + lev * 0.25 * dC;
    accR = accR + lev * 0.25 * dR;
  }

  textureStore(waterDst, p, vec4<f32>(here.x, here.y, max(vol, 0.0), here.w));
  textureStore(pigCDst, p, max(accC, vec4<f32>(0.0)));
  textureStore(pigRDst, p, accR);
}
