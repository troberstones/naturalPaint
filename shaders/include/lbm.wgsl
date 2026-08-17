// D2Q9 lattice Boltzmann shared definitions — Chu & Tai 2005 (MoXi), §4.
//
// The LBE models fluid at a mesoscopic level: nine particle distribution
// functions per site, streamed along the lattice vectors and then collided
// toward equilibrium. MoXi picks it over Navier-Stokes because it needs no
// Poisson solve, every operation is local (so it parallelises cleanly), and
// extra physics like variable permeability drops straight into the streaming
// step. Compressibility is not a concern here — ink through fibres is slow.
//
// Layout: lbmA = f0..f3, lbmB = f4..f7, lbmC = (f8, surface water s, ink
// accumulation h, unused).
//#include "include/common.wgsl"

const LBM_E = array<vec2<i32>, 9>(
  vec2<i32>( 0,  0),
  vec2<i32>( 1,  0), vec2<i32>( 0,  1), vec2<i32>(-1,  0), vec2<i32>( 0, -1),
  vec2<i32>( 1,  1), vec2<i32>(-1,  1), vec2<i32>(-1, -1), vec2<i32>( 1, -1)
);

// Index of the reversed lattice vector, for half-way bounce-back.
const LBM_OPP = array<i32, 9>(0, 3, 4, 1, 2, 7, 8, 5, 6);

const LBM_W = array<f32, 9>(
  0.44444444,
  0.11111111, 0.11111111, 0.11111111, 0.11111111,
  0.02777778, 0.02777778, 0.02777778, 0.02777778
);

// He & Luo's incompressible equilibrium, which minimises the compressibility
// error inherent to the LBE (MoXi eq. 2). rho0 is taken as 1.
fn lbmEquilibrium(i: i32, rho: f32, u: vec2<f32>) -> f32 {
  let e = vec2<f32>(LBM_E[i]);
  let eu = dot(e, u);
  return LBM_W[i] * (rho + 3.0 * eu + 4.5 * eu * eu - 1.5 * dot(u, u));
}

// MoXi eq. 6: the blocking factor that gives the paper its character. Grain
// stands in for voids in the fibres, glue for the viscosity artists add to limit
// spread, and accumulated ink for the way a laid-down mark impedes later flow.
// Alum is folded into the grain term rather than carried as its own texture.
fn lbmKappa(grain: f32, inkAccum: f32) -> f32 {
  return clamp(P.blocking + P.grainBlock * grain + P.glue + 0.35 * inkAccum,
               0.0, 0.98);
}
