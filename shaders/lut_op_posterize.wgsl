// color/LutBake per-op kernel: Posterize. WGSL port of ops/ToneOps.cpp's
// applyPosterize() -- quantise in the SHAPER domain, not in linear light,
// because linear is not perceptually uniform and equal-width linear bins put
// far too many levels in the highlights and far too few in the shadows. See
// ops/ToneOps.hpp for that argument in full.
//
// The two degenerate level counts are ports of that function's own decisions,
// not fresh ones: <= 0 is the identity, and 1 collapses everything to
// shaperDecode(0).
//#include "include/shaper.wgsl"

@group(0) @binding(0) var<uniform> P : vec4<f32>;  // x = levels (as a float), yzw unused
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let levels = i32(P.x);

  var result : vec3<f32>;
  if (levels <= 0) {
    result = clamp(shapedIn, vec3<f32>(0.0), vec3<f32>(1.0));
  } else if (levels == 1) {
    // shaperEncode(shaperDecode(0.0)) is 0.0 -- the op's output re-encoded.
    result = vec3<f32>(0.0, 0.0, 0.0);
  } else {
    // The quantisation happens directly on the shaped value, which is what
    // this LUT already holds -- so unlike every other kernel here there is no
    // decode/encode round trip, and that is a faithful port rather than a
    // shortcut: applyPosterize() encodes, quantises, decodes, and the bake
    // would immediately re-encode.
    let step = 1.0 / f32(levels - 1);
    result = clamp(round(shapedIn / step) * step, vec3<f32>(0.0), vec3<f32>(1.0));
  }
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
