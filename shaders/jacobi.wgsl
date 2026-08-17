// One Jacobi iteration of the pressure-correction Poisson solve.
// Run ~20-40 times per frame; more iterations means water spreads more evenly
// and blooms read as rounder.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var auxSrc   : texture_2d<f32>;
@group(0) @binding(3) var auxDst   : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  let here = textureLoad(auxSrc, p, 0);
  let M = textureLoad(waterSrc, p, 0).w;

  // Outside the wet region the correction is pinned to zero — that Dirichlet
  // boundary is what makes the stroke edge behave like a meniscus.
  if (M < 0.01) {
    textureStore(auxDst, p, vec4<f32>(here.x, 0.0, 0.0, 0.0));
    return;
  }

  let dpL = textureLoad(auxSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).y;
  let dpR = textureLoad(auxSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).y;
  let dpD = textureLoad(auxSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).y;
  let dpU = textureLoad(auxSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).y;

  let dp = (dpL + dpR + dpD + dpU - here.x) * 0.25;

  textureStore(auxDst, p, vec4<f32>(here.x, dp, 0.0, 0.0));
}
