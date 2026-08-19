// color/LutBake per-op kernel: Levels (PLAN.md Phase 3 step 4). WGSL port of
// ops/PointOps.cpp's applyLevelsChannel() -- same formula, same internal
// clamp-t-to-[0,1]-before-pow() guard (required by math, not policy: pow()
// on a negative base with a fractional exponent is NaN), same epsilon.
//
// General per-op-kernel shape (shared by every kernel here except Curves,
// see lut_op_curves.wgsl for why it differs): read one shaper-domain texel
// from the current ping-pong LUT texture, shaperDecode to scene-linear,
// apply this op's own linear-domain math, shaperEncode back to shaper
// domain, clamp to [0,1] -- color/LutBake's own deferred-clamp decision,
// see color/Shaper.hpp's header comment -- and write to the other
// ping-pong texture.
//#include "include/shaper.wgsl"

// 15 floats (5 params x 3 channels) packed into 4 vec4<f32> slots -- one
// float (whiteOut.w) unused. Must match color/LutBake.cpp's LevelsUniform
// byte for byte.
struct LevelsUniform {
  chR: vec4<f32>,       // blackIn, whiteIn, gamma, blackOut
  chG: vec4<f32>,
  chB: vec4<f32>,
  whiteOut: vec4<f32>,  // whiteOutR, whiteOutG, whiteOutB, unused
}

@group(0) @binding(0) var<uniform> P : LevelsUniform;
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

const kLevelsEpsilon: f32 = 1e-6;

fn applyLevelsChannelGpu(input: f32, blackIn: f32, whiteIn: f32, gamma: f32,
                         blackOut: f32, whiteOut: f32) -> f32 {
  let range = max(whiteIn - blackIn, kLevelsEpsilon);
  var t = (input - blackIn) / range;
  t = clamp(t, 0.0, 1.0);
  t = pow(t, 1.0 / gamma);
  return t * (whiteOut - blackOut) + blackOut;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;
  let linearIn = vec3<f32>(shaperDecode(shapedIn.r), shaperDecode(shapedIn.g), shaperDecode(shapedIn.b));

  let linearOut = vec3<f32>(
      applyLevelsChannelGpu(linearIn.r, P.chR.x, P.chR.y, P.chR.z, P.chR.w, P.whiteOut.x),
      applyLevelsChannelGpu(linearIn.g, P.chG.x, P.chG.y, P.chG.z, P.chG.w, P.whiteOut.y),
      applyLevelsChannelGpu(linearIn.b, P.chB.x, P.chB.y, P.chB.z, P.chB.w, P.whiteOut.z));

  let shapedOut = vec3<f32>(shaperEncode(linearOut.r), shaperEncode(linearOut.g), shaperEncode(linearOut.b));
  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
