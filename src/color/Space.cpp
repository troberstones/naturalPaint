#include "color/Space.hpp"

#include <cmath>

namespace np {
namespace {

// sRGB (IEC 61966-2-1) breakpoints: the toe is linear below this and a
// power curve above it, chosen so the two segments and their derivatives
// meet at the breakpoint.
constexpr float kSrgbLinearBreak = 0.0031308f;   // in linear
constexpr float kSrgbEncodedBreak = 0.04045f;    // in encoded (12.92 * break)
constexpr float kSrgbToeSlope = 12.92f;
constexpr float kSrgbGamma = 2.4f;
constexpr float kSrgbScale = 1.055f;
constexpr float kSrgbOffset = 0.055f;

// Rec.709 / BT.1886 OETF breakpoints. Same shape as sRGB's — linear toe,
// then a power segment — but different constants, hence a different curve.
constexpr float kRec709LinearBreak = 0.018f;              // in linear
constexpr float kRec709EncodedBreak = 4.5f * 0.018f;      // 0.081, in encoded
constexpr float kRec709ToeSlope = 4.5f;
constexpr float kRec709Gamma = 0.45f;
constexpr float kRec709Scale = 1.099f;
constexpr float kRec709Offset = 0.099f;

}  // namespace

float srgbEncode(float linear) {
  const float sign = linear < 0.0f ? -1.0f : 1.0f;
  const float x = std::fabs(linear);
  const float encoded = (x <= kSrgbLinearBreak)
                            ? x * kSrgbToeSlope
                            : kSrgbScale * std::pow(x, 1.0f / kSrgbGamma) - kSrgbOffset;
  return sign * encoded;
}

float srgbDecode(float encoded) {
  const float sign = encoded < 0.0f ? -1.0f : 1.0f;
  const float x = std::fabs(encoded);
  const float linear = (x <= kSrgbEncodedBreak)
                           ? x / kSrgbToeSlope
                           : std::pow((x + kSrgbOffset) / kSrgbScale, kSrgbGamma);
  return sign * linear;
}

float rec709Encode(float linear) {
  const float sign = linear < 0.0f ? -1.0f : 1.0f;
  const float x = std::fabs(linear);
  const float encoded = (x < kRec709LinearBreak)
                            ? x * kRec709ToeSlope
                            : kRec709Scale * std::pow(x, kRec709Gamma) - kRec709Offset;
  return sign * encoded;
}

float rec709Decode(float encoded) {
  const float sign = encoded < 0.0f ? -1.0f : 1.0f;
  const float x = std::fabs(encoded);
  const float linear = (x < kRec709EncodedBreak)
                           ? x / kRec709ToeSlope
                           : std::pow((x + kRec709Offset) / kRec709Scale, 1.0f / kRec709Gamma);
  return sign * linear;
}

}  // namespace np
