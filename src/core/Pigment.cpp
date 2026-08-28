#include "core/Pigment.hpp"

#include <algorithm>
#include <cmath>

namespace np {

#if defined(NP_USE_MIXBOX)

std::array<float, 3> pigmentPolynomialRgb(const std::array<float, 3>& c) noexcept {
  const float c0 = c[0], c1 = c[1], c2 = c[2];
  const float c3 = 1.0f - (c0 + c1 + c2);

  const float c00 = c0 * c0, c11 = c1 * c1, c22 = c2 * c2;
  const float c01 = c0 * c1, c02 = c0 * c2, c12 = c1 * c2, c33 = c3 * c3;

  struct W { float w; float r, g, b; };
  const W terms[] = {
      {c0 * c00,  0.07717053f,  0.02826978f,  0.24832992f},
      {c1 * c11,  0.95912302f,  0.80256528f,  0.03561839f},
      {c2 * c22,  0.74683774f,  0.04868586f,  0.00000000f},
      {c3 * c33,  0.99518138f,  0.99978149f,  0.99704802f},
      {c00 * c1,  0.04819146f,  0.83363781f,  0.32515377f},
      {c01 * c1, -0.68146950f,  1.46107803f,  1.06980936f},
      {c00 * c2,  0.27058419f, -0.15324870f,  1.98735057f},
      {c02 * c2,  0.80478189f,  0.67093710f,  0.18424500f},
      {c00 * c3, -0.35031003f,  1.37855826f,  3.68865000f},
      {c0 * c33,  1.05128046f,  1.97815239f,  2.82989073f},
      {c11 * c2,  3.21607125f,  0.81270228f,  1.03384539f},
      {c1 * c22,  2.78893374f,  0.41565549f, -0.04487295f},
      {c11 * c3,  3.02162577f,  2.55374103f,  0.32766114f},
      {c1 * c33,  2.95124691f,  2.81201112f,  1.17578442f},
      {c22 * c3,  2.82677043f,  0.79933038f,  1.81715262f},
      {c2 * c33,  2.99691099f,  1.22593053f,  1.80653661f},
      {c01 * c2,  1.87394106f,  2.05027182f, -0.29835996f},
      {c01 * c3,  2.56609566f,  7.03428198f,  0.62575374f},
      {c02 * c3,  4.08329484f, -1.40408358f,  2.14995522f},
      {c12 * c3,  6.00078678f,  2.55552042f,  1.90739502f},
  };

  std::array<float, 3> out{0.0f, 0.0f, 0.0f};
  for (const auto& t : terms) {
    out[0] += t.w * t.r;
    out[1] += t.w * t.g;
    out[2] += t.w * t.b;
  }
  return out;
}

std::array<float, 3> latentToRgb(const Latent& z) noexcept {
  auto rgb = pigmentPolynomialRgb(z.c);
  for (int i = 0; i < 3; ++i) rgb[i] = std::clamp(rgb[i] + z.res[i], 0.0f, 1.0f);
  return rgb;
}

#else  // !NP_USE_MIXBOX -- two-constant Kubelka-Munk, independent of Mixbox.
//
// docs/architecture-review.md finding P2-3 / ADR-0006: `NP_USE_MIXBOX=OFF` is
// supposed to build without any CC BY-NC encumbrance, and until this branch
// existed it did not -- the ON-only `pigmentPolynomialRgb()` above is a
// verbatim transcription of Mixbox's own fitted polynomial (its coefficients,
// not just the 512x512 LUT, are Secret Weapons' CC BY-NC-licensed work; see
// shaders/include/mixbox.wgsl's header), and nothing gated it. This is the
// real fallback: derived from the published, generic two-constant
// Kubelka-Munk reflectance theory (Kubelka, "New Contributions to the Optics
// of Intensely Light-Scattering Materials", J. Opt. Soc. Am. 38(5), 1948),
// which is closed-form and needs no fitted table, no measured pigment
// spectra taken from Mixbox, and nothing from Krita (GPL, deliberately not
// consulted) or the two Adobe patents this project is under instruction not
// to read.
//
// **What "two-constant" buys over the simpler single-constant Kubelka model**:
// single-constant KM tracks only the ratio K/S, which is enough to predict
// the reflectance of one layer over a matching substrate but cannot tell a
// pigment with strong hiding power (high scattering S, e.g. an opaque white
// gouache) from a transparent stain of the same hue (low S, e.g. a
// quinacridone glaze) -- both would have to carry the same K/S to look the
// same, so mixing either one into a third colour would move it identically,
// which is wrong. Two-constant theory tracks K and S *separately* and mixes
// them independently by concentration (`K_mix = sum ci*Ki`, `S_mix =
// sum ci*Si`), which is exactly what lets an opaque pigment "cover" and a
// transparent one merely "tint" when mixed with something else. This
// codebase already has a real, named place for that distinction --
// `paint::Pigment::staining` (`Physical pigment properties, following Curtis
// et al. 1997 Table 1`, paint/Palette.hpp) -- and paint/Palette.cpp's
// NP_USE_MIXBOX=OFF branch of `MixboxLut::rgbToLatent()` is what actually
// uses it; this function only needs to trust whatever K and S it is handed.
//
// **Storage convention for this basis** (the "third pigment basis" ADR-0006
// already named, `km2-v1`, mutually unreadable with `mixbox-v1` -- core/Document.hpp
// holds both spellings and picks between them): `Latent`
// still holds six floats with the same names, `c[0..2]` and `res[0..2]`, but
// they mean something different here than they do under Mixbox. `c[i]` is
// the mixture's absorption K in RGB channel i; `res[i]` is its scattering S
// in the same channel. This reinterpretation is deliberate and is the reason
// `mixLatents()` (core/Blend.cpp) needed **no change at all**: it is already
// a plain per-component `std::lerp`, and `K_mix = lerp(Ka, Kb, t)` /
// `S_mix = lerp(Sa, Sb, t)` at the same `t` *is* the two-constant mixing rule
// above -- concentration-weighted linear combination is what "two-constant"
// names. A fitted-polynomial basis and a closed-form-physics basis turning
// out to share one lerp function is not a coincidence: it is why Mixbox
// itself represents a mix as a latent-space lerp rather than an RGB one, and
// this fallback keeps that property instead of reinventing a worse one.
//
// **The projection below is the algebraic inverse of the classic KM formula**
// `K/S = (1-R)^2 / (2R)` for the reflectance R of an optically-thick layer,
// solved for R:
//
//   R = 1 + (K/S) - sqrt((K/S)^2 + 2(K/S))
//
// Applied per RGB channel -- a three-band simplification of the fully
// spectral theory, not a novel one: it is the same simplification Curtis et
// al. 1997 (already cited by paint::Pigment above) and most interactive
// digital-watercolour systems since have used, because a full spectral
// solver has no interactive budget and this project's `Latent` only carries
// three stored weights either way. `S` only ever enters as a *ratio* K/S at
// a single pick (see paint/Palette.cpp), so a single unmixed colour's round
// trip through `rgbToLatent()` then here is exact regardless of which S was
// assumed for it -- S cancels out of the ratio -- and only stops being exact
// once two differently-scattering latents have actually been mixed, which is
// the point: that divergence from a naive lerp *is* the physical effect this
// basis exists to produce.
std::array<float, 3> latentToRgb(const Latent& z) noexcept {
  std::array<float, 3> out{};
  for (int i = 0; i < 3; ++i) {
    // Guards, not tuning: `res[i]` (S) is 0 in a freshly-default-constructed
    // `Latent` (an untouched PigmentTile texel, mass 0 -- see this header's
    // own doc comment on why that default is never actually displayed), and
    // an erase or a bad mix can in principle drive `c[i]` (K) negative. Both
    // would otherwise send `ks` to +-infinity or the sqrt argument negative;
    // clamping keeps this total and finite the way the Mixbox branch's own
    // [0,1] clamp on its polynomial output is total and finite for any input.
    const float S = std::fmax(z.res[i], 1.0e-4f);
    const float ks = std::fmax(z.c[i] / S, 0.0f);
    // `1 + ks - sqrt(ks^2 + 2ks)`, but rationalised: for a saturated pigment
    // (or a picked colour with a near-zero channel, see paint/Palette.cpp's
    // clamp) `ks` runs into the tens, and subtracting two nearly-equal
    // large numbers in that form loses most of float32's mantissa --
    // measured, not assumed: at ks=4999 (a 1e-4 reflectance floor) the naive
    // form rounds to exactly 0 in float32, `1/(1+ks+sqrt(...))` recovers the
    // truthful ~1e-4. Multiplying by the conjugate `(1+ks+sqrt(ks^2+2ks))`
    // turns the subtraction into `[(1+ks)^2 - (ks^2+2ks)] = 1`, so the whole
    // expression is exactly `1 / (1+ks+sqrt(ks^2+2ks))` -- algebraically
    // identical, numerically stable because every term being summed is
    // positive.
    const float r = 1.0f / (1.0f + ks + std::sqrt(ks * ks + 2.0f * ks));
    out[i] = std::clamp(r, 0.0f, 1.0f);
  }
  return out;
}

#endif  // NP_USE_MIXBOX

}  // namespace np
