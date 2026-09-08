// color/LutBake per-op kernel: Threshold. WGSL port of ops/ToneOps.cpp's
// applyThreshold() -- a luma-based mono split, not three independently
// clipping channels, compared in the shaper domain for Posterize's reason.
// "At or above -> white" is the inclusive side, matching that function's `>=`.
//#include "include/shaper.wgsl"

@group(0) @binding(0) var<uniform> P : vec4<f32>;  // x = threshold, y = amount, zw unused
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

// ops/PointOps.hpp's kRec709LumaWeights, restated here for the same reason
// every other kernel restates its constants: a shader cannot include the
// header, and the cross-check in app/selftest/LutBake.cpp is what holds the
// two copies to the same values.
const kLumaWeights = vec3<f32>(0.2126, 0.7152, 0.0722);

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let linearIn = vec3<f32>(shaperDecode(shapedIn.r), shaperDecode(shapedIn.g), shaperDecode(shapedIn.b));

  let luma = dot(linearIn, kLumaWeights);
  let shapedLuma = shaperEncode(luma);
  var bw = 0.0;
  if (shapedLuma >= P.x) { bw = 1.0; }
  let linearOut = mix(linearIn, vec3<f32>(bw, bw, bw), P.y);

  let shapedOut = vec3<f32>(shaperEncode(linearOut.r), shaperEncode(linearOut.g), shaperEncode(linearOut.b));
  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
