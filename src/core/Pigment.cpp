#include "core/Pigment.hpp"

#include <algorithm>

namespace np {

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

}  // namespace np
