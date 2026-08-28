// Two-constant Kubelka-Munk latent -> RGB projection, linked in when
// NP_USE_MIXBOX is off (see gfx/ShaderLoader.cpp, which resolves
// composite.wgsl's basis-neutral `//#include "include/pigment_basis.wgsl"` to
// this file or to include/mixbox.wgsl depending on the C++ build flag).
//
// This is the GPU mirror of core/Pigment.cpp's NP_USE_MIXBOX=OFF branch of
// `latentToRgb()` -- see that file's header comment for the physics
// (Kubelka & Munk 1948's two-constant reflectance formula, applied per RGB
// channel rather than spectrally) and for why it needs no LUT, no fitted
// polynomial and nothing from third_party/mixbox: the formula is closed-form.
// **The two implementations must stay numerically identical** the same way
// mixboxEvalPolynomial() and core::pigmentPolynomialRgb() are required to --
// `--selftest`'s pigment-basis section checks the GPU composite against the
// CPU projection for a fixed set of latents in this build too.
//
// `PigmentLatent` (include/common.wgsl) carries the same six floats as the
// Mixbox basis, reinterpreted: `c` is absorption (K) per RGB channel, `res`
// is scattering (S) per RGB channel. See include/common.wgsl's own comment on
// why that makes the fluid solver's existing linear advection of these two
// textures the correct two-constant mixing rule (K_mix = sum ci*Ki, S_mix =
// sum ci*Si) with no change to the solver itself.
fn pigmentLatentToRgb(latent: PigmentLatent) -> vec3<f32> {
  // S must be positive for K/S to mean anything; a latent that was never
  // deposited (mass 0, before the caller's own divide-by-mass) or that
  // erasure has driven negative both land here, and 1e-4 keeps the divide
  // and the sqrt below finite rather than propagating a NaN into the frame.
  let S = max(latent.res, vec3<f32>(1e-4));
  let ks = max(latent.c / S, vec3<f32>(0.0));

  // Kubelka 1948's closed-form reflectance of an optically-thick layer from
  // its absorption/scattering ratio, `R = 1 + (K/S) - sqrt((K/S)^2 + 2(K/S))`,
  // rationalised to `1 / (1 + K/S + sqrt((K/S)^2 + 2(K/S)))` for the same
  // reason core/Pigment.cpp's CPU version is: the un-rationalised form
  // subtracts two nearly-equal large numbers for a saturated pigment's
  // near-zero channel and loses most of f32's mantissa doing it -- see that
  // file's comment for the measured example. Applied per channel.
  let r = vec3<f32>(1.0) / (vec3<f32>(1.0) + ks + sqrt(ks * ks + 2.0 * ks));
  return clamp(r, vec3<f32>(0.0), vec3<f32>(1.0));
}
