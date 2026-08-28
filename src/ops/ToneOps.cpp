#include "ops/ToneOps.hpp"

#include <algorithm>
#include <cmath>

#include "color/Shaper.hpp"
#include "color/Space.hpp"
#include "ops/PointOps.hpp"

namespace np {
namespace {

// Shared blend helper behind Invert and Threshold's `amount` field -- see
// ToneOps.hpp's header comment for why both structs carry it. `t == 0`
// early-returns `original` verbatim rather than computing
// `original + (effect - original) * 0`, so the identity default is exact
// bit-for-bit even if `effect` happens to be NaN/Inf for some pathological
// input (no arithmetic touches `original` at all in that case).
std::array<float, 3> blend(const std::array<float, 3>& original, const std::array<float, 3>& effect,
                            float t) noexcept {
  if (t == 0.0f) return original;
  return {original[0] + (effect[0] - original[0]) * t, original[1] + (effect[1] - original[1]) * t,
          original[2] + (effect[2] - original[2]) * t};
}

}  // namespace

float applyGainOffsetGammaChannel(float input, const GainOffsetGammaParams& p) noexcept {
  const float base = input * p.gain + p.offset;
  // gamma == 1 is pow(base, 1) == base exactly -- skip pow() (and the clamp
  // below) entirely rather than compute it, so the identity default holds
  // bit-for-bit even for a negative `base` (pow() with an exact integer
  // exponent of 1.0 would actually be fine on a negative base too, but
  // skipping it outright is what keeps this op's default free of any
  // dependence on that being true). See ToneOps.hpp's doc comment.
  if (p.gamma == 1.0f) return base;
  // Required by math, not policy: 1/gamma is a fractional exponent in
  // general, and pow() on a negative base with a fractional exponent is NaN.
  // gain/offset alone cannot make `base` negative in a way that matters (both
  // are total, order-preserving operations on their own), but a negative
  // `offset` combined with a non-1 `gamma` can. Clamping `base` here, not
  // this function's return value, mirrors PointOps.hpp's LevelsParams `t`
  // clamp exactly -- an internal value clamped out of mathematical
  // necessity, not this op's final output clamped as a policy choice.
  const float safeBase = std::max(base, 0.0f);
  return std::pow(safeBase, 1.0f / p.gamma);
}

std::array<float, 3> applyGainOffsetGamma(const std::array<float, 3>& rgb,
                                           const GainOffsetGammaParams& p) noexcept {
  return {applyGainOffsetGammaChannel(rgb[0], p), applyGainOffsetGammaChannel(rgb[1], p),
          applyGainOffsetGammaChannel(rgb[2], p)};
}

std::array<float, 3> applyInvert(const std::array<float, 3>& rgb, const InvertParams& p) noexcept {
  std::array<float, 3> inverted{};
  if (p.domain == InvertParams::Domain::Linear) {
    // pivot - x, pivot == 1.0 -- see ToneOps.hpp's doc comment for why this
    // pivot and why this is an involution (and therefore safe to leave
    // unclamped) for every real input, not just [0,1].
    inverted = {1.0f - rgb[0], 1.0f - rgb[1], 1.0f - rgb[2]};
  } else {
    // Display domain: encode, invert the encoded value, decode back.
    // srgbEncode/srgbDecode are themselves unclamped and exact inverses for
    // every real input (color/Space.hpp's own contract), which is what
    // keeps THIS branch an involution too -- see ToneOps.hpp's doc comment.
    inverted = {srgbDecode(1.0f - srgbEncode(rgb[0])), srgbDecode(1.0f - srgbEncode(rgb[1])),
                srgbDecode(1.0f - srgbEncode(rgb[2]))};
  }
  return blend(rgb, inverted, p.amount);
}

std::array<float, 3> applyPosterize(const std::array<float, 3>& rgb,
                                     const PosterizeParams& p) noexcept {
  // Degenerate level counts, decided and documented in ToneOps.hpp -- see
  // that comment for why these are not arbitrary special cases.
  if (p.levels <= 0) return rgb;
  if (p.levels == 1) {
    const float v = shaperDecode(0.0f);
    return {v, v, v};
  }

  const float step = 1.0f / static_cast<float>(p.levels - 1);
  std::array<float, 3> out{};
  for (size_t c = 0; c < 3; ++c) {
    const float shaped = shaperEncode(rgb[c]);
    const float q = std::round(shaped / step) * step;
    out[c] = shaperDecode(q);
  }
  return out;
}

std::array<float, 3> applyThreshold(const std::array<float, 3>& rgb,
                                     const ThresholdParams& p) noexcept {
  // computeLuma()/kRec709LumaWeights: PointOps.hpp's shared helper, reused
  // verbatim -- see ToneOps.hpp's doc comment for why Threshold is a
  // luma-based mono split rather than three independently-clipping
  // channels.
  const float luma = computeLuma(rgb);
  const float shaped = shaperEncode(luma);
  // "At/above -> white" is the inclusive side, per ToneOps.hpp's doc comment.
  const float bw = (shaped >= p.threshold) ? 1.0f : 0.0f;
  return blend(rgb, {bw, bw, bw}, p.amount);
}

}  // namespace np
