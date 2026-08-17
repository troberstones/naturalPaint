// Curtis §4.1 FlowOutward — lower the pressure near the edge of the wet mask so
// water (and the pigment it carries) drifts to the rim and strands there.
// This single term is what produces watercolour's dark edges; without it strokes
// read as flat gouache.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var linSampler : sampler;
@group(0) @binding(2) var waterSrc : texture_2d<f32>;
@group(0) @binding(3) var waterDst : texture_storage_2d<rgba16float, write>;

// Two rings of bilinear taps approximate a wide Gaussian on M cheaply.
fn blurredMask(fp: vec2<f32>, res: vec2<u32>) -> f32 {
  var sum = textureSampleLevel(waterSrc, linSampler, toUV(fp, res), 0.0).w * 0.25;
  var wsum = 0.25;

  let r1 = 3.0;
  let r2 = 7.0;
  for (var i = 0u; i < 8u; i = i + 1u) {
    let a = f32(i) * 0.785398163;   // 2*pi/8
    let d = vec2<f32>(cos(a), sin(a));
    sum  = sum + textureSampleLevel(waterSrc, linSampler, toUV(fp + d * r1, res), 0.0).w * 0.0625;
    wsum = wsum + 0.0625;
    sum  = sum + textureSampleLevel(waterSrc, linSampler, toUV(fp + d * r2, res), 0.0).w * 0.0469;
    wsum = wsum + 0.0469;
  }
  return sum / wsum;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var water = textureLoad(waterSrc, p, 0);
  if (water.w >= 0.01) {
    let bm = blurredMask(vec2<f32>(p), P.resolution);
    water.z = water.z - P.edgeDarkening * (1.0 - bm) * water.w * P.dt;
    water.z = max(water.z, 0.0);
  }
  textureStore(waterDst, p, water);
}
