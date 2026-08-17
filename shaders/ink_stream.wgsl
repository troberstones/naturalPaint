// Ink, streaming with half-way bounce-back — MoXi §5.3.1, eq. 5.
//
//   f_i(x, t+1) = kappa_i * f_opp(i)(x, t) + (1 - kappa_i) * f_i(x - e_i, t)
//
// kappa_i is the *average* of the blocking factors of the two linked sites, not
// the destination's alone. That symmetry is what keeps the blocking equal in
// both directions along a link and so conserves density and momentum.
//#include "include/common.wgsl"
//#include "include/lbm.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var lbmASrc : texture_2d<f32>;
@group(0) @binding(2) var lbmBSrc : texture_2d<f32>;
@group(0) @binding(3) var lbmCSrc : texture_2d<f32>;
@group(0) @binding(4) var paperTex : texture_2d<f32>;
@group(0) @binding(5) var lbmADst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(6) var lbmBDst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(7) var lbmCDst : texture_storage_2d<rgba32float, write>;

fn loadF(c: vec2<i32>) -> array<f32, 9> {
  let a = textureLoad(lbmASrc, c, 0);
  let b = textureLoad(lbmBSrc, c, 0);
  let d = textureLoad(lbmCSrc, c, 0);
  return array<f32, 9>(a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, d.x);
}

fn kappaAt(c: vec2<i32>) -> f32 {
  return lbmKappa(textureLoad(paperTex, c, 0).x, textureLoad(lbmCSrc, c, 0).z);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  let here = loadF(p);
  let kHere = kappaAt(p);

  var out : array<f32, 9>;
  out[0] = here[0];   // the rest population never streams

  for (var i = 1; i < 9; i = i + 1) {
    let up = p - LBM_E[i];              // upstream site for direction i
    let inside = inBounds(up, res);
    let src = select(p, up, inside);
    // A site at the grid edge has no upstream neighbour, so treat the wall as
    // fully blocking: everything bounces back.
    let kLink = select(1.0, (kHere + kappaAt(up)) * 0.5, inside);
    let streamed = loadF(src)[i];
    out[i] = kLink * here[LBM_OPP[i]] + (1.0 - kLink) * streamed;
  }

  var c = textureLoad(lbmCSrc, p, 0);
  textureStore(lbmADst, p, vec4<f32>(out[0], out[1], out[2], out[3]));
  textureStore(lbmBDst, p, vec4<f32>(out[4], out[5], out[6], out[7]));
  textureStore(lbmCDst, p, vec4<f32>(out[8], c.y, c.z, c.w));
}
