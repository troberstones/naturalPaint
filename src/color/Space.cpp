#include "color/Space.hpp"

#include <algorithm>
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

// Strictly greater than the ceiling, and strictly less than the floor -- a
// value *at* 1.0 is inside the display range, not over it, so a plain white
// foreground must not light up the panel's over-range badge. The floor half
// is here because the curves above mirror about zero rather than clipping
// (see their header), so a negative encoded value is a thing this pipeline
// can actually hold, and it dies at the same three destinations a value
// above white does.
bool exceedsDisplayRange(float encoded) noexcept {
  return encoded > kDisplayCeiling || encoded < kDisplayFloor;
}

bool exceedsDisplayRange(const std::array<float, 3>& encoded) noexcept {
  return exceedsDisplayRange(encoded[0]) || exceedsDisplayRange(encoded[1]) ||
         exceedsDisplayRange(encoded[2]);
}

// **Per channel, not a scale-to-fit.** Dividing the triple through by its
// largest channel would preserve the hue and is what a tone-mapper does;
// this is not a tone-mapper and must not become one. The destinations this
// serves are a swatch, a picker square and a pigment LUT, and each of them
// wants "the nearest colour I can represent" -- clipping per channel is the
// answer every one of them already assumed it was getting. Preserving hue
// instead would make the swatch a *different* colour from the one the
// numeric readout beside it prints, which is precisely the disagreement the
// readout exists to prevent.
std::array<float, 3> clampToDisplayRange(const std::array<float, 3>& encoded) noexcept {
  return {std::clamp(encoded[0], kDisplayFloor, kDisplayCeiling),
          std::clamp(encoded[1], kDisplayFloor, kDisplayCeiling),
          std::clamp(encoded[2], kDisplayFloor, kDisplayCeiling)};
}

}  // namespace np
