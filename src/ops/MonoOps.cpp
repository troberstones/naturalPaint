#include "ops/MonoOps.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// One lerp, shared by the two branches below (weight interpolation and, via
// gradientColorAt(), the colour ramp itself already does its own).
float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

}  // namespace

std::array<float, 3> applyBlackAndWhite(const std::array<float, 3>& rgb,
                                         const BlackAndWhiteParams& p) noexcept {
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float M = std::max({r, g, b});
  const float m = std::min({r, g, b});
  const float C = M - m;

  if (C == 0.0f) {
    // Achromatic: no hue, and the term any weight would scale is zero
    // regardless -- see MonoOps.hpp's doc comment.
    return {m, m, m};
  }

  // HSV's own published hue derivation (max/min/mod), reused as the sector
  // selector -- see MonoOps.hpp for why this is the right tool and not a
  // GPL/patent-adjacent one.
  float h;
  if (M == r) {
    h = std::fmod((g - b) / C, 6.0f);
  } else if (M == g) {
    h = (b - r) / C + 2.0f;
  } else {
    h = (r - g) / C + 4.0f;
  }
  if (h < 0.0f) h += 6.0f;
  if (h >= 6.0f) h -= 6.0f;

  int sector = static_cast<int>(std::floor(h));
  sector = std::clamp(sector, 0, 5);
  const float frac = h - static_cast<float>(sector);

  // {reds, yellows, greens, cyans, blues, magentas} -- exactly
  // docs/operations.md's own naming order, which is also the cyclic hue
  // order this construction assumes (0/60/120/180/240/300 degrees).
  const std::array<float, 6> w{p.reds, p.yellows, p.greens, p.cyans, p.blues, p.magentas};
  const float weight = lerp(w[static_cast<size_t>(sector)],
                             w[static_cast<size_t>((sector + 1) % 6)], frac);

  const float y = m + weight * C;
  return {y, y, y};
}

std::array<float, 3> applyGradientMap(const std::array<float, 3>& rgb,
                                       const GradientMapParams& p) noexcept {
  // (b) from MonoOps.hpp's doc comment: no colour stops means passthrough,
  // chosen so a default-constructed GradientMapParams is an exact identity
  // like every other op in this file family.
  if (p.stops.colorStops.empty()) {
    return rgb;
  }
  const float luma = computeLuma(rgb, p.lumaWeights);
  return gradientColorAt(p.stops, luma);
}

}  // namespace np
