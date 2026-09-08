// color/LutBake per-op kernel: Invert. WGSL port of ops/ToneOps.cpp's
// applyInvert(). Two domains, exactly as that function has them: Linear is
// `1 - x` about a pivot of 1.0, Display encodes to sRGB, inverts there, and
// decodes back. Both are involutions for every real input, which is why
// neither clamps before the shared epilogue does. See lut_op_levels.wgsl's
// header comment for the decode/op/encode/clamp shape every kernel follows.
//#include "include/shaper.wgsl"
//#include "include/srgb.wgsl"

@group(0) @binding(0) var<uniform> P : vec4<f32>;  // x = amount, y = domain (0 linear, 1 display)
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let linearIn = vec3<f32>(shaperDecode(shapedIn.r), shaperDecode(shapedIn.g), shaperDecode(shapedIn.b));

  var inverted : vec3<f32>;
  if (P.y >= 0.5) {
    let e = vec3<f32>(srgbEncode(linearIn.r), srgbEncode(linearIn.g), srgbEncode(linearIn.b));
    let f = vec3<f32>(1.0) - e;
    inverted = vec3<f32>(srgbDecode(f.r), srgbDecode(f.g), srgbDecode(f.b));
  } else {
    inverted = vec3<f32>(1.0) - linearIn;
  }
  let linearOut = mix(linearIn, inverted, P.x);

  let shapedOut = vec3<f32>(shaperEncode(linearOut.r), shaperEncode(linearOut.g), shaperEncode(linearOut.b));
  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
