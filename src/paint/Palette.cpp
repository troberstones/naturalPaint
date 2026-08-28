#include "paint/Palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

// This is the one translation unit that compiles stb_image's actual function
// bodies (STB_IMAGE_IMPLEMENTATION may only be defined once in the whole
// binary) -- io/ImageDecode.cpp includes stb_image.h with no implementation
// macro and links against the bodies compiled in here.
//
// Was STBI_ONLY_PNG alone, back when this LUT load was the only stb_image
// caller. io/ImageDecode (PLAN.md Phase 2 step 6's decode half) needs
// JPEG/BMP/TGA too, and STBI_ONLY_x compiles out every *other* format's
// decoder within the one shared implementation -- so PNG-only here meant
// JPEG/BMP/TGA support literally didn't exist anywhere in the binary,
// regardless of what any other file included. Widened to the four formats
// this project actually needs; GIF/PSD/PIC/PNM/HDR stay excluded.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

namespace np {

#if defined(NP_USE_MIXBOX)

bool MixboxLut::load(const std::string& pngPath) {
  int w = 0, h = 0, comp = 0;
  unsigned char* px = stbi_load(pngPath.c_str(), &w, &h, &comp, 4);
  if (!px) {
    std::fprintf(stderr, "[mixbox] cannot load %s: %s\n", pngPath.c_str(),
                 stbi_failure_reason());
    return false;
  }
  if (w != static_cast<int>(kSize) || h != static_cast<int>(kSize)) {
    std::fprintf(stderr, "[mixbox] expected %ux%u LUT, got %dx%d\n", kSize,
                 kSize, w, h);
    stbi_image_free(px);
    return false;
  }
  data_.assign(px, px + static_cast<size_t>(w) * h * 4);
  stbi_image_free(px);
  return true;
}

bool MixboxLut::valid() const { return !data_.empty(); }

#else  // !NP_USE_MIXBOX

// The KM2 fallback (rgbToLatent() below) is closed-form and needs no file, so
// this is a deliberate no-op rather than a stub that still opens `pngPath` --
// which in this project's only caller (main.cpp) always names
// third_party/mixbox/shaders/mixbox_lut.png, and a build meant to carry none
// of Mixbox's CC BY-NC LUT should not read its bytes into memory anyway, even
// if nothing downstream would end up using them. Returns true, not false:
// main.cpp's "could not load, refuse to start" branch exists to catch a
// broken *Mixbox* install, and must not fire in a configuration that has no
// LUT to be missing.
bool MixboxLut::load(const std::string& pngPath) {
  (void)pngPath;
  return true;
}

// Unconditionally ready: there is no file dependency to be invalid about. See
// this class's header comment on why every existing `--selftest` site that
// constructs a deliberately-unloaded `MixboxLut` (grep `noLut`) still
// compiles and still means something under this basis -- it now exercises
// the real KM2 path instead of the ON-build's "LUT failed to load" fallback,
// and app/selftest/Eyedropper.cpp's own `#if` is the one place that
// distinction was assertion-visible enough to need branching.
bool MixboxLut::valid() const { return true; }

#endif  // NP_USE_MIXBOX

#if defined(NP_USE_MIXBOX)

std::array<float, 3> MixboxLut::fetch(float x, float y) const {
  // Bilinear, matching the linear-filtered textureSampleLevel in the WGSL.
  const float fx = std::clamp(x, 0.0f, kSize - 1.0f);
  const float fy = std::clamp(y, 0.0f, kSize - 1.0f);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const int x1 = std::min(x0 + 1, static_cast<int>(kSize) - 1);
  const int y1 = std::min(y0 + 1, static_cast<int>(kSize) - 1);
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);

  auto texel = [&](int px, int py, int ch) {
    return data_[(static_cast<size_t>(py) * kSize + px) * 4 + ch] / 255.0f;
  };

  std::array<float, 3> out{};
  for (int ch = 0; ch < 3; ++ch) {
    const float a = texel(x0, y0, ch) * (1 - tx) + texel(x1, y0, ch) * tx;
    const float b = texel(x0, y1, ch) * (1 - tx) + texel(x1, y1, ch) * tx;
    out[ch] = a * (1 - ty) + b * ty;
  }
  return out;
}

Latent MixboxLut::rgbToLatent(float r, float g, float b) const {
  Latent z{};
  if (!valid()) return z;

  r = std::clamp(r, 0.0f, 1.0f);
  g = std::clamp(g, 0.0f, 1.0f);
  b = std::clamp(b, 0.0f, 1.0f);

  const float x = r * 63.0f;
  const float y = g * 63.0f;
  const float zc = b * 63.0f;
  const float iz = std::floor(zc);

  auto tile = [&](float slice) {
    const float x0 = std::fmod(slice, 8.0f) * 64.0f;
    const float y0 = std::floor(slice / 8.0f) * 64.0f;
    return fetch(x0 + x, y0 + y);
  };

  const auto c0 = tile(iz);
  const auto c1 = tile(std::min(iz + 1.0f, 63.0f));
  const float t = zc - iz;

  for (int i = 0; i < 3; ++i) z.c[i] = c0[i] * (1 - t) + c1[i] * t;

  // The residual is *defined* as "what the polynomial did not reproduce", so
  // it must be computed with the very same polynomial `latentToRgb()` adds it
  // back to -- core/Pigment's, called here rather than re-transcribed. That
  // identity is what makes the round trip exact (measured 0.000e+00 in
  // --selftest) rather than LUT-quantisation-limited, and two copies of the
  // coefficients could not be relied on to give it.
  const auto poly = pigmentPolynomialRgb(z.c);
  z.res[0] = r - poly[0];
  z.res[1] = g - poly[1];
  z.res[2] = b - poly[2];
  return z;
}

#else  // !NP_USE_MIXBOX -- two-constant Kubelka-Munk, independent of Mixbox.
//
// The inverse of this projection is core/Pigment.cpp's NP_USE_MIXBOX=OFF
// `latentToRgb()`; see that file's header comment for the theory (Kubelka
// 1948), the storage convention (`c` = K per channel, `res` = S per channel)
// and why `mixLatents()` needed no change to remain correct for this basis.
// This is the forward half, and it needs the one thing the inverse does not:
// an estimate of **scattering S for a colour with no known pigment
// identity**. A picked #7f3f00 could be a thin burnt-sienna glaze or a thick
// opaque terra cotta gouache and the RGB triple alone cannot say which --
// there is no LUT of real paints to consult here, which is the entire point
// of this build existing.
//
// The choice made below is a physically-motivated heuristic, stated plainly
// as one rather than dressed up as measurement: a saturated colour is
// assumed more transparent (a stain's tinting strength usually trades away
// hiding power) and a desaturated/near-neutral colour is assumed more opaque
// (titanium white is the limiting case -- almost pure scattering, negligible
// absorption). `defaultPalette()`'s own `staining` column two screens down
// documents the same real-world correlation for the fourteen named pigments;
// this applies that intuition to a colour with no name attached. Neither
// `kOpaqueScatter` nor `kStainScatter` is a measured constant -- they set the
// *range* two pigments at the extremes of that column would plausibly sit
// in, in the same spirit `defaultPalette()`'s own density/staining/
// granulation values are "chosen to match how these paints actually behave
// on paper" rather than measured off a spectrophotometer.
Latent MixboxLut::rgbToLatent(float r, float g, float b) const {
  r = std::clamp(r, 0.0f, 1.0f);
  g = std::clamp(g, 0.0f, 1.0f);
  b = std::clamp(b, 0.0f, 1.0f);

  const float maxc = std::max({r, g, b});
  const float minc = std::min({r, g, b});
  const float sat = maxc > 1.0e-4f ? (maxc - minc) / maxc : 0.0f;

  constexpr float kOpaqueScatter = 1.6f;  // near-neutral, low-saturation end
  constexpr float kStainScatter = 0.35f;  // saturated, stain-like end
  const float scatter = kOpaqueScatter + (kStainScatter - kOpaqueScatter) * sat;

  const float rgb[3] = {r, g, b};
  Latent z{};
  for (int i = 0; i < 3; ++i) {
    // Kubelka's K/S from a single reflectance measurement of an
    // optically-thick layer (the picked colour, taken as the pigment's own
    // masstone). Floored well off 0 rather than at a bare division-by-zero
    // guard: `defaultPalette()`'s own darkest entry, Lamp Black, bottoms out
    // at 0.045, and the two nearest-to-zero primary channels among the
    // fourteen named pigments (Cadmium Yellow's blue, Phthalo Green's red)
    // are exact zeros standing in for "as absorbing as this pigment gets",
    // not a measurement of true zero reflectance -- no real pigment is a
    // perfect absorber. 0.03 keeps K/S -- and so `Latent::c` -- in a range
    // where `mixLatents()`'s lerp and every ulp-derived tolerance elsewhere
    // in `--selftest` that assumes "these are weights near [0,1]" (most of
    // them predate this basis and were derived for Mixbox's) stay
    // well-conditioned, instead of letting one near-black channel's K
    // dwarf every other term in a mix by three or four orders of magnitude.
    const float R = std::clamp(rgb[i], kKm2ReflectanceFloor, 1.0f - kKm2ReflectanceFloor);
    const float ks = (1.0f - R) * (1.0f - R) / (2.0f * R);
    z.c[i] = scatter * ks;  // K
    z.res[i] = scatter;     // S, replicated per channel -- see
                             // core/Pigment.cpp's comment on why that makes
                             // mixLatents()'s existing per-component lerp
                             // exactly the two-constant mixing rule.
  }
  return z;
}

#endif  // NP_USE_MIXBOX

const std::vector<Pigment>& defaultPalette() {
  //                                                       density staining granulation
  static const std::vector<Pigment> kPalette = {
      {"Cadmium Yellow",       {0.996f, 0.925f, 0.000f},   0.60f,  0.30f,  0.10f},
      {"Hansa Yellow",         {0.988f, 0.827f, 0.000f},   0.45f,  0.55f,  0.05f},
      {"Cadmium Orange",       {1.000f, 0.412f, 0.000f},   0.60f,  0.30f,  0.10f},
      {"Cadmium Red",          {1.000f, 0.153f, 0.008f},   0.65f,  0.25f,  0.15f},
      {"Quinacridone Magenta", {0.502f, 0.008f, 0.180f},   0.30f,  0.90f,  0.00f},
      {"Cobalt Violet",        {0.306f, 0.000f, 0.259f},   0.75f,  0.20f,  0.70f},
      {"Ultramarine Blue",     {0.098f, 0.000f, 0.349f},   0.55f,  0.30f,  0.65f},
      {"Cobalt Blue",          {0.000f, 0.129f, 0.522f},   0.70f,  0.25f,  0.55f},
      {"Phthalo Blue",         {0.051f, 0.106f, 0.267f},   0.25f,  0.95f,  0.00f},
      {"Phthalo Green",        {0.000f, 0.235f, 0.196f},   0.25f,  0.95f,  0.00f},
      {"Permanent Green",      {0.027f, 0.427f, 0.086f},   0.45f,  0.50f,  0.15f},
      {"Sap Green",            {0.420f, 0.580f, 0.016f},   0.40f,  0.60f,  0.10f},
      {"Burnt Sienna",         {0.482f, 0.282f, 0.000f},   0.70f,  0.25f,  0.45f},
      {"Lamp Black",           {0.045f, 0.045f, 0.050f},   0.55f,  0.60f,  0.20f},
  };
  return kPalette;
}

}  // namespace np
