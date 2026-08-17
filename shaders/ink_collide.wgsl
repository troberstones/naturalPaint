// Ink, collision plus surface-to-fibre supply — MoXi §5.2 and §5.3.
//
// Curtis et al. couple their surface and capillary layers only loosely, and MoXi
// singles that out as the reason their model cannot grow an ink mark properly:
// the water supply from surface to fibres is never tracked, so a bigger puddle
// does not make a bigger blob. Here it is tracked explicitly:
//
//   phi = clamp(s, 0, pi - rho)     with pi = 1, the fibre capacity
//
// and phi is injected as an at-rest equilibrium so it adds density without
// spuriously adding momentum.
//#include "include/common.wgsl"
//#include "include/lbm.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var lbmASrc : texture_2d<f32>;
@group(0) @binding(2) var lbmBSrc : texture_2d<f32>;
@group(0) @binding(3) var lbmCSrc : texture_2d<f32>;
@group(0) @binding(4) var lbmADst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(5) var lbmBDst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(6) var lbmCDst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(7) var waterDst : texture_storage_2d<rgba16float, write>;

const FIBRE_CAPACITY : f32 = 1.0;   // pi

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  let a = textureLoad(lbmASrc, p, 0);
  let b = textureLoad(lbmBSrc, p, 0);
  var c = textureLoad(lbmCSrc, p, 0);

  var f = array<f32, 9>(a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x);

  var rho = 0.0;
  for (var i = 0; i < 9; i = i + 1) { rho = rho + f[i]; }

  // --- surface water seeps into the fibres ---
  let phi = clamp(c.y, 0.0, max(FIBRE_CAPACITY - rho, 0.0));
  if (phi > 0.0) {
    for (var i = 0; i < 9; i = i + 1) { f[i] = f[i] + LBM_W[i] * phi; }
    c.y = c.y - phi;
    rho = rho + phi;
  }

  // --- macroscopic velocity (rho0 = 1 in the incompressible model) ---
  var u = vec2<f32>(0.0);
  for (var i = 1; i < 9; i = i + 1) { u = u + f[i] * vec2<f32>(LBM_E[i]); }

  // --- collision toward equilibrium ---
  let w = clamp(P.omega, 0.1, 1.95);
  for (var i = 0; i < 9; i = i + 1) {
    f[i] = (1.0 - w) * f[i] + w * lbmEquilibrium(i, rho, u);
  }

  // --- uneven evaporation, biased by the paper grain (MoXi §5.3) ---
  let evap = P.evaporation * P.dt;
  if (evap > 0.0 && rho > 0.0) {
    let keep = max(1.0 - evap, 0.0);
    for (var i = 0; i < 9; i = i + 1) { f[i] = f[i] * keep; }
    rho = rho * keep;
  }

  textureStore(lbmADst, p, vec4<f32>(f[0], f[1], f[2], f[3]));
  textureStore(lbmBDst, p, vec4<f32>(f[4], f[5], f[6], f[7]));
  textureStore(lbmCDst, p, vec4<f32>(f[8], c.y, c.z, c.w));

  // Publish (u, v, rho, mask) so the pigment pass, the compositor and --diag can
  // all read the flow the same way they do for watercolour.
  let mask = select(0.0, 1.0, rho > 0.02);
  textureStore(waterDst, p, vec4<f32>(u, rho, mask));
}
