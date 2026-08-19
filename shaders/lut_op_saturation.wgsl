// color/LutBake per-op kernel: Saturation (PLAN.md Phase 3 step 4). WGSL
// port of ops/PointOps.cpp's applySaturation() -- `luma = dot(rgb, weights);
// output = luma + (rgb - luma) * scale`. See lut_op_levels.wgsl's header
// comment for the shared decode/op/encode/clamp shape every kernel here
// follows.
//#include "include/shaper.wgsl"

@group(0) @binding(0) var<uniform> P : vec4<f32>;  // x = scale, yzw = lumaWeights
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let linearIn = vec3<f32>(shaperDecode(shapedIn.r), shaperDecode(shapedIn.g), shaperDecode(shapedIn.b));

  let weights = P.yzw;
  let luma = dot(linearIn, weights);
  let linearOut = vec3<f32>(luma, luma, luma) + (linearIn - vec3<f32>(luma, luma, luma)) * P.x;

  let shapedOut = vec3<f32>(shaperEncode(linearOut.r), shaperEncode(linearOut.g), shaperEncode(linearOut.b));
  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
