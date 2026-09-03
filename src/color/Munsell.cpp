#include "color/Munsell.hpp"

#include "color/Space.hpp"

#include <cmath>

namespace np {
namespace {

using Mat3 = std::array<std::array<double, 3>, 3>;

std::array<double, 3> applyMat(const Mat3& m, const std::array<double, 3>& v) noexcept {
  return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
          m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
          m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Mat3 invert(const Mat3& m) noexcept {
  const double a = m[0][0], b = m[0][1], c = m[0][2];
  const double d = m[1][0], e = m[1][1], f = m[1][2];
  const double g = m[2][0], h = m[2][1], i = m[2][2];
  const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const double det = a * A + b * B + c * C;
  const double s = det == 0.0 ? 0.0 : 1.0 / det;
  return {{{A * s, (c * h - b * i) * s, (b * f - c * e) * s},
           {B * s, (a * i - c * g) * s, (c * d - a * f) * s},
           {C * s, (b * g - a * h) * s, (a * e - b * d) * s}}};
}

// XYZ of a chromaticity at unit luminance.
std::array<double, 3> xyzOf(double x, double y) noexcept {
  if (y == 0.0) return {0.0, 0.0, 0.0};
  return {x / y, 1.0, (1.0 - x - y) / y};
}

// The standard derivation: scale each primary's unit-luminance XYZ so the
// three together sum to the white point's XYZ at Y = 1.
Mat3 deriveRgbToXyz(const Primaries& p) noexcept {
  const auto r = xyzOf(static_cast<double>(p.redX), static_cast<double>(p.redY));
  const auto g = xyzOf(static_cast<double>(p.greenX), static_cast<double>(p.greenY));
  const auto b = xyzOf(static_cast<double>(p.blueX), static_cast<double>(p.blueY));
  const auto w = xyzOf(static_cast<double>(p.whiteX), static_cast<double>(p.whiteY));
  const Mat3 prim{{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  const auto s = applyMat(invert(prim), w);
  return {{{r[0] * s[0], g[0] * s[1], b[0] * s[2]},
           {r[1] * s[0], g[1] * s[1], b[1] * s[2]},
           {r[2] * s[0], g[2] * s[1], b[2] * s[2]}}};
}

const Mat3& rgbToXyz() noexcept {
  static const Mat3 m = deriveRgbToXyz(kRec709Primaries);
  return m;
}
const Mat3& xyzToRgb() noexcept {
  static const Mat3 m = invert(rgbToXyz());
  return m;
}

struct WhiteUv {
  double u = 0.0, v = 0.0;
};
const WhiteUv& whiteUv() noexcept {
  static const WhiteUv w = [] {
    const auto xyz = applyMat(rgbToXyz(), {1.0, 1.0, 1.0});
    const double d = xyz[0] + 15.0 * xyz[1] + 3.0 * xyz[2];
    if (d == 0.0) return WhiteUv{0.0, 0.0};
    return WhiteUv{4.0 * xyz[0] / d, 9.0 * xyz[1] / d};
  }();
  return w;
}

// CIE's own constants, written as the exact rationals the standard defines
// rather than the rounded 0.008856 / 903.3 that circulate -- the rounded pair
// is discontinuous at the breakpoint and this file's inverse is a bisection
// that would then have a step in it.
constexpr double kKappa = 24389.0 / 27.0;
constexpr double kEpsilon = 216.0 / 24389.0;

}  // namespace

double munsellValueToLuminanceFactor(double v) noexcept {
  return 1.1914 * v - 0.22533 * v * v + 0.23352 * v * v * v -
         0.020484 * v * v * v * v + 0.00081939 * v * v * v * v * v;
}

double luminanceFactorToMunsellValue(double yPercent) noexcept {
  if (yPercent <= 0.0) return 0.0;
  if (yPercent >= 100.0) return 10.0;
  double lo = 0.0, hi = 10.0;
  // 80 halvings of a range of 10 is far below double's resolution there; the
  // loop is a fixed count rather than an epsilon test so it cannot fail to
  // terminate on a denormal or a NaN input.
  for (int i = 0; i < 80; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (munsellValueToLuminanceFactor(mid) < yPercent) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

double lStarFromRelativeLuminance(double y) noexcept {
  return y > kEpsilon ? 116.0 * std::cbrt(y) - 16.0 : kKappa * y;
}

double munsellPageRowValue(int row, int steps) noexcept {
  if (steps <= 0) return 0.0;
  return 10.0 * static_cast<double>(row + 1) / static_cast<double>(steps + 1);
}

std::array<double, 3> lchUvToLinearRgb(double lStar, double chroma, double hueDeg) noexcept {
  if (lStar <= 0.0) return {0.0, 0.0, 0.0};
  const double h = hueDeg * 3.14159265358979323846 / 180.0;
  const double uStar = chroma * std::cos(h);
  const double vStar = chroma * std::sin(h);
  const WhiteUv& w = whiteUv();
  const double up = uStar / (13.0 * lStar) + w.u;
  const double vp = vStar / (13.0 * lStar) + w.v;
  // The inverse of L*, matching lStarFromRelativeLuminance() branch for
  // branch -- 8.0 is kKappa*kEpsilon, the L* at the breakpoint.
  const double y = lStar > 8.0 ? std::pow((lStar + 16.0) / 116.0, 3.0) : lStar / kKappa;
  if (vp == 0.0) return {0.0, 0.0, 0.0};
  const double x = y * 9.0 * up / (4.0 * vp);
  const double z = y * (12.0 - 3.0 * up - 20.0 * vp) / (4.0 * vp);
  return applyMat(xyzToRgb(), {x, y, z});
}

bool inLinearDisplayGamut(const std::array<double, 3>& linear) noexcept {
  // The tolerance is one part in a billion and exists only so the exact
  // gamut boundary -- which the bisection converges onto -- counts as inside
  // rather than flickering on the last bit.
  constexpr double kEps = 1e-9;
  for (double c : linear)
    if (!(c >= -kEps && c <= 1.0 + kEps)) return false;
  return true;
}

std::optional<std::array<float, 3>> munsellCellLinearRgb(double lStar, double chroma,
                                                         double hueDeg) noexcept {
  const auto rgb = lchUvToLinearRgb(lStar, chroma, hueDeg);
  if (!inLinearDisplayGamut(rgb)) return std::nullopt;
  // Clamped only against the 1e-9 slack above, so the returned triple is a
  // legal linear colour. This is not the clamp the header refuses: it moves
  // a value by at most 1e-9, where the refused one moves it by however far
  // out of gamut the request was.
  const auto clip = [](double c) {
    return static_cast<float>(c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c));
  };
  return std::array<float, 3>{clip(rgb[0]), clip(rgb[1]), clip(rgb[2])};
}

double maxInGamutChroma(double lStar, double hueDeg) noexcept {
  if (!inLinearDisplayGamut(lchUvToLinearRgb(lStar, 0.0, hueDeg))) return 0.0;
  // 400 is comfortably past any sRGB colour's CIELUV chroma (the largest is
  // ~180, at blue) and the bisection is exact enough that the bound only has
  // to be an over-estimate, not a tight one.
  double lo = 0.0, hi = 400.0;
  for (int i = 0; i < 60; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (inLinearDisplayGamut(lchUvToLinearRgb(lStar, mid, hueDeg))) lo = mid;
    else hi = mid;
  }
  return lo;
}

double pageChroma(int steps, double hueDeg) noexcept {
  double best = 0.0;
  for (int i = 0; i < steps; ++i) {
    const double v = munsellPageRowValue(i, steps);
    const double l = lStarFromRelativeLuminance(munsellValueToLuminanceFactor(v) / 100.0);
    const double c = maxInGamutChroma(l, hueDeg);
    if (c > best) best = c;
  }
  return best;
}

const std::array<std::array<double, 3>, 3>& rec709RgbToXyz() noexcept { return rgbToXyz(); }
const std::array<std::array<double, 3>, 3>& rec709XyzToRgb() noexcept { return xyzToRgb(); }
std::array<double, 2> rec709WhiteUv() noexcept {
  const WhiteUv& w = whiteUv();
  return {w.u, w.v};
}

}  // namespace np
