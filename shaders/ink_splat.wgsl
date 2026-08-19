// Ink, brush footprint — MoXi §5.2.
//
// The footprint is masked by max(1 - rho/lambda, m): already-wet paper resists
// taking more, which is what stops a second stroke over a wet mark from simply
// doubling the ink. lambda is the receptivity, m a floor so the brush always
// leaves something.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;   // (u, v, rho, mask)
@group(0) @binding(2) var lbmCSrc  : texture_2d<f32>;   // (f8, s, h, -)
@group(0) @binding(3) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(4) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(5) var lbmCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(6) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(7) var pigRDst  : texture_storage_2d<rgba32float, write>;

const BASE_MASK : f32 = 0.1;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var lbmC = textureLoad(lbmCSrc, p, 0);
  var pigC = textureLoad(pigCSrc, p, 0);
  var pigR = textureLoad(pigRSrc, p, 0);

  if (P.brushActive != 0u) {
    let d = distToSegment(vec2<f32>(p), P.brushA, P.brushB);
    let edge = mix(P.brushRadius, P.brushRadius * 0.15, P.brushHardness);
    let falloff = 1.0 - smoothstep(P.brushRadius - edge, P.brushRadius, d);

    if (falloff > 0.0) {
      let rho = textureLoad(waterSrc, p, 0).z;
      let mask = max(1.0 - rho / max(P.receptivity, 1e-3), BASE_MASK);
      // A dab deposits a fixed quantity -- not scaled by dt (ADR-0003):
      // deposition is a quantity per unit of brush travel, not a rate.
      let amount = falloff * mask;

      lbmC.y = lbmC.y + P.brushWater * amount;   // surface water s

      let m = P.brushPigment * amount;
      pigC = vec4<f32>(pigC.xyz + P.brushLatentC.xyz * m, pigC.w + m);
      pigR = vec4<f32>(pigR.xyz + P.brushLatentR.xyz * m, 0.0);
    }
  }

  textureStore(lbmCDst, p, lbmC);
  textureStore(pigCDst, p, pigC);
  textureStore(pigRDst, p, pigR);
}
