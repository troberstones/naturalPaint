// Apply the pressure correction: make the velocity field divergence-free and
// fold the correction back into the stored pressure, keeping Curtis's coupling
// between p and the flow it drives.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var auxSrc   : texture_2d<f32>;
@group(0) @binding(3) var waterDst : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  var water = textureLoad(waterSrc, p, 0);
  let dp = textureLoad(auxSrc, p, 0).y;

  let dpL = textureLoad(auxSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).y;
  let dpR = textureLoad(auxSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).y;
  let dpD = textureLoad(auxSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).y;
  let dpU = textureLoad(auxSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).y;

  // NOTE: strokes still come out without the dark rim that FlowOutward is
  // supposed to produce. Two hypotheses were measured and neither was the cause:
  // the magnitude of the FlowOutward pressure nudge (raising it 50x changed
  // nothing visible), and over-projection here (relaxing to 0.55, on the grounds
  // that a free-surface shallow-water layer is not incompressible, also changed
  // nothing). Mean flow speed measures a healthy ~0.07 px/step, so it is not
  // that the water is standing still either. Still unresolved.
  //
  // Do NOT reintroduce `vel *= water.w` here. Scaling a freshly
  // divergence-free field by the wet mask makes it divergent again exactly at
  // the stroke boundary, and semi-Lagrangian advection then duplicates pigment
  // there. The mask is already a Dirichlet boundary condition on dp in
  // jacobi.wgsl, which is where it belongs.
  let vel = water.xy - 0.5 * vec2<f32>(dpR - dpL, dpU - dpD);

  // Do NOT fold dp into water.z. That channel is the physical water depth --
  // splat adds to it, capillary_flow absorbs from it, and FlowOutward lowers it
  // at the rim. dp is a pressure *correction* for the velocity solve only.
  // Adding it here drove the depth to zero within a single Jacobi iteration and
  // killed the pressure-driven flow entirely, which is why the paper-slope term
  // was the only thing moving water.
  textureStore(waterDst, p, vec4<f32>(vel, water.z, water.w));
}
