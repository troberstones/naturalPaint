// Shallow-water velocity update — Curtis §4.1 UpdateVelocities, with the
// advection term done semi-Lagrangian (Stam 1999) rather than by the paper's
// explicit finite difference, which is only conditionally stable.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var linSampler : sampler;
@group(0) @binding(2) var waterSrc : texture_2d<f32>;
@group(0) @binding(3) var paperTex : texture_2d<f32>;
@group(0) @binding(4) var waterDst : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  let here = textureLoad(waterSrc, p, 0);
  let M = here.w;

  // Dry paper carries no flow.
  if (M < 0.01) {
    textureStore(waterDst, p, vec4<f32>(0.0, 0.0, here.z, M));
    return;
  }

  let res = P.resolution;
  let fp = vec2<f32>(p);

  // --- self-advection: trace backwards along the velocity field ---
  let back = fp - here.xy * P.dt;
  var vel = textureSampleLevel(waterSrc, linSampler, toUV(back, res), 0.0).xy;

  // --- paper height gradient: water runs downhill into the fibre valleys ---
  let hL = textureLoad(paperTex, clampCoord(p + vec2<i32>(-1, 0), res), 0).x;
  let hR = textureLoad(paperTex, clampCoord(p + vec2<i32>( 1, 0), res), 0).x;
  let hD = textureLoad(paperTex, clampCoord(p + vec2<i32>(0, -1), res), 0).x;
  let hU = textureLoad(paperTex, clampCoord(p + vec2<i32>(0,  1), res), 0).x;
  let gradH = vec2<f32>(hR - hL, hU - hD) * 0.5;
  vel = vel - P.paperSlope * gradH * P.dt;

  // --- pressure gradient drives flow out of the deposit site ---
  let pL = textureLoad(waterSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).z;
  let pR = textureLoad(waterSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).z;
  let pD = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).z;
  let pU = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).z;
  vel = vel - vec2<f32>(pR - pL, pU - pD) * 0.5 * P.dt;

  // --- viscosity (Laplacian smoothing) ---
  let vL = textureLoad(waterSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).xy;
  let vR = textureLoad(waterSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).xy;
  let vD = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).xy;
  let vU = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).xy;
  let lap = (vL + vR + vD + vU) * 0.25 - vel;
  vel = vel + P.viscosity * lap;

  // --- gravity: a tilted board makes a wet wash run ---
  // Scaled by film depth, which is what makes this behave like the real
  // technique rather than a global scroll: a standing puddle streaks downhill
  // while merely damp paper stays put, so you tilt while it is wet and lay the
  // board flat once it has soaked in. (A Nusselt film accelerates with depth;
  // this is the cheap monotonic stand-in for that.)
  let depth = clamp(here.z / max(P.maxFilm, 1e-3), 0.0, 1.0);
  vel = vel + P.tilt * depth * P.dt;

  // --- drag ---
  vel = vel * (1.0 - P.drag * P.dt);

  // Velocity outside the wet mask is already zeroed by the early-out above, so
  // there is no `vel *= M` here: scaling by a fractional mask would make the
  // field divergent at the stroke edge and semi-Lagrangian advection would
  // duplicate pigment there. See the note in project.wgsl.
  textureStore(waterDst, p, vec4<f32>(vel, here.z, M));
}
