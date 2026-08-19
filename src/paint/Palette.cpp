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
