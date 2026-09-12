#include "brush/Taper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace np {

bool brushTaperEqual(const BrushTaper& a, const BrushTaper& b) noexcept {
  return a.on == b.on && a.lengthPx == b.lengthPx && a.minSizePct == b.minSizePct &&
         a.flow == b.flow;
}

float taperMultiplier(float distanceFromEndPx, const BrushTaper& taper) noexcept {
  if (!taper.on || taper.lengthPx <= 0.0f) return 1.0f;
  const float t = std::clamp(distanceFromEndPx / taper.lengthPx, 0.0f, 1.0f);
  const float s = t * t * (3.0f - 2.0f * t);  // smoothstep
  const float minFrac = std::clamp(taper.minSizePct, 0.0f, 100.0f) / 100.0f;
  return minFrac + (1.0f - minFrac) * s;
}

namespace {

// The same floor `StrokeSession::taperedSpacingPx()` puts under the entry
// taper's spacing: a ramp whose `minSizePct` is 0 ends at a point, and a
// spacing of literally zero is an unbounded number of dabs.
constexpr float kMinSpacingFrac = 0.05f;
// Per segment, so a pathological spacing cannot turn one tail into millions
// of dabs. A segment needing more than this is already finer than any tip
// this can be painting with.
constexpr int kMaxPiecesPerSegment = 32;

StrokeDab lerpDab(const StrokeDab& a, const StrokeDab& b, float t) noexcept {
  StrokeDab out = b;  // timestamp and any future field follow the later dab
  out.pos.x = a.pos.x + (b.pos.x - a.pos.x) * t;
  out.pos.y = a.pos.y + (b.pos.y - a.pos.y) * t;
  out.pressure = a.pressure + (b.pressure - a.pressure) * t;
  out.tilt = a.tilt + (b.tilt - a.tilt) * t;
  out.azimuth = a.azimuth + (b.azimuth - a.azimuth) * t;
  out.barrel = a.barrel + (b.barrel - a.barrel) * t;
  return out;
}

}  // namespace

void subdivideTaperedTail(std::vector<StrokeDab>& tail, const BrushTaper& taper,
                          float spacingPx) noexcept {
  if (!taper.on || taper.lengthPx <= 0.0f || spacingPx <= 0.0f || tail.size() < 2) return;

  // Arc length from each dab to the LAST one -- what the ramp is read at.
  std::vector<float> arc(tail.size(), 0.0f);
  for (size_t i = tail.size() - 1; i-- > 0;)
    arc[i] = arc[i + 1] + std::hypot(tail[i + 1].pos.x - tail[i].pos.x,
                                     tail[i + 1].pos.y - tail[i].pos.y);

  std::vector<StrokeDab> out;
  out.reserve(tail.size() * 2);
  for (size_t i = 0; i + 1 < tail.size(); ++i) {
    out.push_back(tail[i]);
    const float segLen = arc[i] - arc[i + 1];
    // The THIN end of the segment decides: it is the one that would dot.
    const float mul = std::max(std::min(taperMultiplier(arc[i], taper),
                                        taperMultiplier(arc[i + 1], taper)),
                               kMinSpacingFrac);
    const float want = spacingPx * mul;
    const int pieces = std::min(static_cast<int>(std::ceil(segLen / want)), kMaxPiecesPerSegment);
    for (int k = 1; k < pieces; ++k)
      out.push_back(lerpDab(tail[i], tail[i + 1], static_cast<float>(k) / static_cast<float>(pieces)));
  }
  out.push_back(tail.back());
  tail.swap(out);
}

}  // namespace np
