#include "paint/Palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace np {
namespace {

// Identical polynomial to mixboxEvalPolynomial() in the WGSL; keep the two in
// step if either is ever touched.
std::array<float, 3> evalPolynomial(const std::array<float, 3>& c) {
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

}  // namespace

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

  const auto poly = evalPolynomial(z.c);
  z.res[0] = r - poly[0];
  z.res[1] = g - poly[1];
  z.res[2] = b - poly[2];
  return z;
}

std::array<float, 3> MixboxLut::latentToRgb(const Latent& z) const {
  auto rgb = evalPolynomial(z.c);
  for (int i = 0; i < 3; ++i) rgb[i] = std::clamp(rgb[i] + z.res[i], 0.0f, 1.0f);
  return rgb;
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
