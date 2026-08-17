// Ink, constituent transport — MoXi §5.4.
//
// The carbon particles ride the percolating water, then fix to the fibres. The
// accumulated ink (h) feeds back into the blocking factor, so a mark that has
// already dried impedes later flow through it — that feedback is what produces
// the pinned, ragged boundaries of Eastern ink painting rather than a smooth
// blob.
//
// Advection uses the same conservative donor-cell scheme as the other modes, so
// ink mass is exact.
//#include "include/common.wgsl"
//#include "include/donor.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;   // (u, v, rho, mask)
@group(0) @binding(2) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(4) var depCSrc  : texture_2d<f32>;
@group(0) @binding(5) var depRSrc  : texture_2d<f32>;
@group(0) @binding(6) var lbmCSrc  : texture_2d<f32>;
@group(0) @binding(7) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(8) var pigRDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(9) var depCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(10) var depRDst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(11) var lbmCDst : texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  // --- conservative advection of suspended ink ---
  var pigC = vec4<f32>(0.0);
  var pigR = vec4<f32>(0.0);
  for (var oy = -1; oy <= 1; oy = oy + 1) {
    for (var ox = -1; ox <= 1; ox = ox + 1) {
      let n = p + vec2<i32>(ox, oy);
      if (!inBounds(n, res)) { continue; }
      let wn = textureLoad(waterSrc, n, 0);
      var vn = vec2<f32>(0.0);
      if (wn.w > 0.5) { vn = wn.xy; }
      let w = donorWeight(vn, vec2<f32>(f32(-ox), f32(-oy)));
      if (w > 0.0) {
        pigC = pigC + w * textureLoad(pigCSrc, n, 0);
        pigR = pigR + w * textureLoad(pigRSrc, n, 0);
      }
    }
  }

  var depC = textureLoad(depCSrc, p, 0);
  var depR = textureLoad(depRSrc, p, 0);
  var lbmC = textureLoad(lbmCSrc, p, 0);

  let water = textureLoad(waterSrc, p, 0);
  let rho = water.z;

  // --- fixing to the fibres ---
  // Ink settles faster where the flow has slowed or the water is thin, which is
  // what strands pigment at the edge of a spreading mark.
  let speed = length(water.xy);
  // settleScale matters: unscaled this reached 2.75 and clamped to 1.0, fixing
  // every suspended particle to the fibres on the very first step. Ink that
  // deposits instantly cannot travel, so no amount of loosening the lattice
  // would have made a mark bleed.
  let settle = clamp(P.density * P.dt * P.settleScale
                     * (1.0 + 3.0 * exp(-8.0 * speed))
                     * select(1.0, 2.5, rho < 0.05), 0.0, 1.0);

  let down = pigC * settle;
  let downR = pigR * settle;
  pigC = pigC - down;
  pigR = pigR - downR;
  depC = depC + down;
  depR = depR + downR;

  // Accumulated ink feeds back into permeability.
  lbmC.z = min(lbmC.z + down.w, 4.0);

  textureStore(pigCDst, p, max(pigC, vec4<f32>(0.0)));
  textureStore(pigRDst, p, pigR);
  textureStore(depCDst, p, max(depC, vec4<f32>(0.0)));
  textureStore(depRDst, p, depR);
  textureStore(lbmCDst, p, lbmC);
}
