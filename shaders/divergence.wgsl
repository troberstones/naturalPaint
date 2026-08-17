// Divergence of the velocity field, seeding the pressure-correction solve.
// Curtis §4.1 RelaxDivergence, recast as a Jacobi projection (Stam 1999) since
// the paper's Gauss-Seidel sweep does not map onto a GPU.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var auxDst   : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  let uL = textureLoad(waterSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).x;
  let uR = textureLoad(waterSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).x;
  let vD = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).y;
  let vU = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).y;

  let div = 0.5 * ((uR - uL) + (vU - vD));

  // x = divergence (constant through the solve), y = pressure correction.
  textureStore(auxDst, p, vec4<f32>(div, 0.0, 0.0, 0.0));
}
