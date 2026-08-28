#include "ops/ColorOps.hpp"

#include <algorithm>
#include <cmath>

#include "color/Shaper.hpp"

namespace np {
namespace {

// Mirrors PointOps.cpp's kLevelsEpsilon pattern exactly -- a guard against a
// degenerate division, not a policy clamp. Shared by every op below that
// needs one.
constexpr float kColorOpsEpsilon = 1e-6f;

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// Rodrigues' rotation formula (standard axis-angle vector rotation): rotates
// `v` by `degrees` about the unit axis `k`. See ColorOps.hpp's Hue/
// Saturation/Lightness doc comment for why `k` is specifically
// normalize(kRec709LumaWeights) -- that choice is what makes this rotation
// leave computeLuma() invariant.
std::array<float, 3> rotateAroundAxis(const std::array<float, 3>& v, const std::array<float, 3>& k,
                                       float degrees) noexcept {
  const float theta = degrees * (3.14159265358979323846f / 180.0f);
  const float c = std::cos(theta);
  const float s = std::sin(theta);
  const std::array<float, 3> kxv = cross3(k, v);
  const float kdv = dot3(k, v);
  return {
      v[0] * c + kxv[0] * s + k[0] * kdv * (1.0f - c),
      v[1] * c + kxv[1] * s + k[1] * kdv * (1.0f - c),
      v[2] * c + kxv[2] * s + k[2] * kdv * (1.0f - c),
  };
}

// normalize(kRec709LumaWeights) -- the luma-preserving rotation axis every
// hue operation below shares. Computed once per call rather than hoisted to
// a file-scope constant: std::sqrt is not usable in a constexpr initializer
// the way this codebase's other shared constants (kLevelsEpsilon, etc.) are,
// and this is three multiplies and a sqrt, not a hot loop.
std::array<float, 3> lumaAxis() noexcept {
  const float norm = std::sqrt(dot3(kRec709LumaWeights, kRec709LumaWeights));
  return {kRec709LumaWeights[0] / norm, kRec709LumaWeights[1] / norm, kRec709LumaWeights[2] / norm};
}

}  // namespace

std::array<float, 3> applyHueSaturation(const std::array<float, 3>& rgb,
                                         const HueSaturationParams& p) noexcept {
  const std::array<float, 3> k = lumaAxis();
  std::array<float, 3> hueSatDone{};

  if (p.colorize) {
    // Colorize: fixed reference chroma direction (pure red's own deviation
    // from its own luma), rotated to the requested hue, scaled to the
    // requested saturation, and added to *this pixel's* own grey point --
    // see ColorOps.hpp's "Colorize" doc comment for why this keeps luma
    // exact rather than approximate.
    const float lumaRed = computeLuma({1.0f, 0.0f, 0.0f});
    const std::array<float, 3> refDev{1.0f - lumaRed, -lumaRed, -lumaRed};
    const std::array<float, 3> rotated = rotateAroundAxis(refDev, k, p.colorizeHueDegrees);
    const float lumaHere = computeLuma(rgb);
    hueSatDone = {
        lumaHere + rotated[0] * p.colorizeSaturation,
        lumaHere + rotated[1] * p.colorizeSaturation,
        lumaHere + rotated[2] * p.colorizeSaturation,
    };
  } else {
    // Ordinary hue rotation: rotate the deviation-from-own-luma, not the
    // raw triple, so `hueDegrees == 0` is an exact identity (rotateAroundAxis
    // by 0 degrees returns `dev` unchanged, cos(0)=1, sin(0)=0) with no
    // dependency on luma arithmetic round-tripping cleanly.
    const float luma = computeLuma(rgb);
    const std::array<float, 3> dev{rgb[0] - luma, rgb[1] - luma, rgb[2] - luma};
    const std::array<float, 3> rotatedDev = rotateAroundAxis(dev, k, p.hueDegrees);
    const std::array<float, 3> hueRotated{luma + rotatedDev[0], luma + rotatedDev[1],
                                           luma + rotatedDev[2]};
    // Saturation: literally applySaturation(), not a hand-rolled equivalent
    // -- see ColorOps.hpp's doc comment for why this must be the same call,
    // not merely numerically similar code.
    hueSatDone = applySaturation(hueRotated, SaturationParams{p.saturation, kRec709LumaWeights});
  }

  // Lightness: shaper-domain additive shift, skipped entirely at 0 rather
  // than relying on shaperDecode(shaperEncode(x)) == x to only float
  // tolerance -- mirrors applyCurves()'s identical documented reason.
  if (p.lightness == 0.0f) return hueSatDone;
  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) {
    const float shapedIn = shaperEncode(hueSatDone[static_cast<size_t>(c)]);
    const float shapedOut = shapedIn + p.lightness;
    out[static_cast<size_t>(c)] = shaperDecode(shapedOut);
  }
  return out;
}

std::array<float, 3> applyVibrance(const std::array<float, 3>& rgb, const VibranceParams& p) noexcept {
  // kProtect is a deliberate design constant, not a caller-facing parameter
  // -- see ColorOps.hpp's doc comment for the monotonicity argument that
  // fixes it at exactly 0.5.
  constexpr float kProtect = 0.5f;

  const float maxC = std::max({rgb[0], rgb[1], rgb[2]});
  const float minC = std::min({rgb[0], rgb[1], rgb[2]});
  const float denom = std::max(std::fabs(maxC), kColorOpsEpsilon);
  const float saturationMeasure = (maxC - minC) / denom;

  const float scale = 1.0f + p.amount * (1.0f - kProtect * saturationMeasure);
  return applySaturation(rgb, SaturationParams{scale, p.lumaWeights});
}

namespace {

// The tonal-range tent functions -- see ColorOps.hpp's ColorBalance doc
// comment for why this exact shape is a partition of unity for every real
// `luma`, not just inside [0,1].
float tonalShadowWeight(float luma) noexcept { return std::clamp(1.0f - 2.0f * luma, 0.0f, 1.0f); }
float tonalHighlightWeight(float luma) noexcept { return std::clamp(2.0f * luma - 1.0f, 0.0f, 1.0f); }
float tonalMidtoneWeight(float luma) noexcept {
  return 1.0f - tonalShadowWeight(luma) - tonalHighlightWeight(luma);
}

}  // namespace

std::array<float, 3> applyColorBalance(const std::array<float, 3>& rgb,
                                        const ColorBalanceParams& p) noexcept {
  const float luma = computeLuma(rgb);
  const float wShadow = tonalShadowWeight(luma);
  const float wMidtone = tonalMidtoneWeight(luma);
  const float wHighlight = tonalHighlightWeight(luma);

  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) {
    const size_t i = static_cast<size_t>(c);
    const float offset = p.shadowsLift[i] * wShadow;
    const float gain = 1.0f + p.highlightsGain[i] * wHighlight;
    float gammaDenom = 1.0f + p.midtonesGamma[i] * wMidtone;
    // Required by math, not policy: guards `1/gammaDenom` against a
    // divide-by-zero, mirroring LevelsParams' own `whiteIn - blackIn`
    // epsilon guard.
    if (std::fabs(gammaDenom) < kColorOpsEpsilon) {
      gammaDenom = std::copysign(kColorOpsEpsilon, gammaDenom == 0.0f ? 1.0f : gammaDenom);
    }

    const float base = rgb[i] * gain + offset;
    if (gammaDenom == 1.0f) {
      // No gamma push applies at this pixel/channel (the default,
      // `midtonesGamma == 0`, always lands here) -- pass `base` through
      // unclamped so a default-constructed ColorBalanceParams stays an
      // exact identity for negative and HDR inputs alike.
      out[i] = base;
    } else {
      const float invPower = 1.0f / gammaDenom;
      // Required by math, not policy: pow() would see a negative base for
      // any base < 0, which is NaN in general for a fractional exponent --
      // the same LevelsParams necessity, scoped only to this branch.
      out[i] = std::pow(std::max(base, 0.0f), invPower);
    }
  }

  if (p.preserveLuminosity) {
    const float outLuma = computeLuma(out);
    if (outLuma > kColorOpsEpsilon) {
      const float f = luma / outLuma;
      out = {out[0] * f, out[1] * f, out[2] * f};
    }
  }
  return out;
}

std::array<float, 3> applyPhotoFilter(const std::array<float, 3>& rgb,
                                       const PhotoFilterParams& p) noexcept {
  std::array<float, 3> blended{};
  for (int c = 0; c < 3; ++c) {
    const size_t i = static_cast<size_t>(c);
    const float filtered = rgb[i] * p.color[i];
    blended[i] = rgb[i] + p.density * (filtered - rgb[i]);
  }

  if (p.preserveLuminosity) {
    const float lumaIn = computeLuma(rgb);
    const float lumaBlended = computeLuma(blended);
    if (lumaBlended > kColorOpsEpsilon) {
      const float f = lumaIn / lumaBlended;
      blended = {blended[0] * f, blended[1] * f, blended[2] * f};
    }
  }
  return blended;
}

}  // namespace np
