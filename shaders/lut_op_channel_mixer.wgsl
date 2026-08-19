// color/LutBake per-op kernel: Channel mixer (PLAN.md Phase 3 step 4). WGSL
// port of ops/PointOps.cpp's applyChannelMixer() -- `output[i] = dot(row_i,
// rgb) + offset_i`. See lut_op_levels.wgsl's header comment for the shared
// decode/op/encode/clamp shape every kernel here follows.
//#include "include/shaper.wgsl"

// Each row is {rWeight, gWeight, bWeight, offset} -- matches
// ops::ChannelMixerParams::matrix's own row shape exactly. Must match
// color/LutBake.cpp's ChannelMixerUniform byte for byte.
struct ChannelMixerUniform {
  row0: vec4<f32>,
  row1: vec4<f32>,
  row2: vec4<f32>,
}

@group(0) @binding(0) var<uniform> P : ChannelMixerUniform;
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let linearIn = vec3<f32>(shaperDecode(shapedIn.r), shaperDecode(shapedIn.g), shaperDecode(shapedIn.b));

  let linearOut = vec3<f32>(
      dot(P.row0.xyz, linearIn) + P.row0.w,
      dot(P.row1.xyz, linearIn) + P.row1.w,
      dot(P.row2.xyz, linearIn) + P.row2.w);

  let shapedOut = vec3<f32>(shaperEncode(linearOut.r), shaperEncode(linearOut.g), shaperEncode(linearOut.b));
  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
