// One-shot generation of the paper substrate.
//   .x  height h    — cold-press tooth; drives granulation and flow bias
//   .y  capacity c  — how much water these fibres can hold
//
// Slightly anisotropic noise, because real laid paper has a grain direction and
// perfectly isotropic tooth reads as digital.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var paperDst : texture_storage_2d<rgba16float, write>;

fn hash2(p: vec2<f32>) -> f32 {
  let h = dot(p, vec2<f32>(127.1, 311.7));
  return fract(sin(h) * 43758.5453123);
}

fn valueNoise(p: vec2<f32>) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let u = f * f * (3.0 - 2.0 * f);
  let a = hash2(i);
  let b = hash2(i + vec2<f32>(1.0, 0.0));
  let c = hash2(i + vec2<f32>(0.0, 1.0));
  let d = hash2(i + vec2<f32>(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn fbm(p: vec2<f32>) -> f32 {
  var sum = 0.0;
  var amp = 0.5;
  var freq = p;
  for (var i = 0; i < 5; i = i + 1) {
    sum = sum + amp * valueNoise(freq);
    freq = freq * 2.03;
    amp = amp * 0.5;
  }
  return sum;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  let uv = vec2<f32>(p) * 0.08;

  // Two scales: coarse tooth plus a stretched fine grain for the laid lines.
  let coarse = fbm(uv);
  let grain = fbm(vec2<f32>(uv.x * 3.1, uv.y * 0.55) + vec2<f32>(37.0, 11.0));

  var h = coarse * 0.72 + grain * 0.28;
  h = clamp((h - 0.25) * 1.9, 0.0, 1.0);

  // Thicker fibre bundles hold more water.
  let c = mix(0.55, 1.25, h);

  textureStore(paperDst, p, vec4<f32>(h, c, 0.0, 0.0));
}
