#include "brush/Taper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

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
// A ceiling on what one stroke's tail can become, so that no combination of a
// tiny minimum and a long ramp can walk forever.
constexpr size_t kMaxTailDabs = 4096;

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

void resampleTaperedTail(std::vector<StrokeDab>& tail, const BrushTaper& taperIn,
                         const BrushTaper& taperOut, float spacingPx) noexcept {
  if (!taperOut.on || taperOut.lengthPx <= 0.0f || spacingPx <= 0.0f || tail.size() < 2) return;

  // Arc length from each dab to the LAST one -- what the exit ramp is read at
  // -- and the stroke's own total, which the entry ramp is read against.
  std::vector<float> arc(tail.size(), 0.0f);
  for (size_t i = tail.size() - 1; i-- > 0;)
    arc[i] = arc[i + 1] + std::hypot(tail[i + 1].pos.x - tail[i].pos.x,
                                     tail[i + 1].pos.y - tail[i].pos.y);
  const float total = arc[0];
  if (total <= 0.0f) return;

  // The spacing wanted at a point, as a fraction of the full-size spacing.
  // Whichever ramp is thinning the tip more there decides, because the
  // coarser of the two is the one that would bead. The entry ramp only
  // reaches this at all on a stroke short enough for the two to overlap, and
  // on one of those it is what stops the start being coarsened back -- those
  // dabs were EMITTED at the entry ramp's own finer spacing.
  const auto wantFrac = [&](float arcFromEnd) {
    float f = taperMultiplier(arcFromEnd, taperOut);
    if (taperIn.on && taperIn.lengthPx > 0.0f)
      f = std::min(f, taperMultiplier(total - arcFromEnd, taperIn));
    return std::max(f, kMinSpacingFrac);
  };

  // Everything before the exit ramp keeps the dabs it was emitted with: the
  // ramp asks for nothing finer there, and re-walking it would throw away the
  // spacing the entry taper chose live, while the pen was down.
  size_t first = 0;
  while (first + 2 < tail.size() && arc[first + 1] > taperOut.lengthPx) ++first;
  std::vector<StrokeDab> out(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(first) + 1);

  // The rest of the polyline is re-walked so that the dabs TILE it exactly:
  // as many as the ramp asks for, evenly in "wanted spacings", with both ends
  // landing on real points. Two earlier shapes of this both banded:
  //
  //  * splitting each segment into `ceil(length / wanted)` pieces steps the
  //    density from one piece to two the moment the wanted spacing dips a
  //    hair below the spacing the dabs already have -- which is at the
  //    ramp's own start, where the dabs are still nearly full size and
  //    nothing about them absorbs the extra ink. A dark band exactly there.
  //  * stepping forward by the wanted spacing leaves a remainder at the lift
  //    point, and neither swallowing it (a gap up to twice what the tip
  //    wants) nor keeping it (two dabs almost on top of each other) is
  //    harmless.
  //
  // So the walk is done in PHASE -- the running count of wanted spacings --
  // rather than in px. The region holds `P` of them; `N` dabs are placed at
  // equal phase intervals, which puts the last one exactly on the lift point
  // and makes every gap the same small fraction of what its own position
  // wants. `N` rounds UP so no gap is ever coarser than wanted, with a slack
  // of a twentieth of a dab so that a stroke whose arc overshoots `P` by a
  // float hair does not buy a whole extra dab.
  const size_t last = tail.size() - 1;
  std::vector<float> cum(tail.size() - first, 0.0f);
  for (size_t i = first + 1; i <= last; ++i)
    cum[i - first] = cum[i - first - 1] + std::hypot(tail[i].pos.x - tail[i - 1].pos.x,
                                                     tail[i].pos.y - tail[i - 1].pos.y);
  const float regionArc = cum.back();
  if (regionArc <= 0.0f) return;

  // A point at `s` px along the region, as a dab: the bracketing dabs'
  // pressure and tilt interpolated, exactly as a split piece used to be.
  // `s` only ever moves forward, so the segment cursor does too -- a fresh
  // scan per dab would be quadratic in a long tail, on the pen-up frame.
  size_t cursor = 1;
  const auto at = [&](float s) {
    size_t& i = cursor;
    while (i + 1 < cum.size() && cum[i] < s) ++i;
    const float span = cum[i] - cum[i - 1];
    const float t = span > 0.0f ? std::clamp((s - cum[i - 1]) / span, 0.0f, 1.0f) : 1.0f;
    return lerpDab(tail[first + i - 1], tail[first + i], t);
  };
  // Wanted spacings per px at `s`, which is `regionArc - s` from the end.
  const auto density = [&](float s) { return 1.0f / (spacingPx * wantFrac(regionArc - s)); };

  constexpr float kPhaseStepPx = 0.25f;
  constexpr float kPhaseSlack = 0.05f;
  float phaseTotal = 0.0f;
  for (float s = 0.0f; s < regionArc;) {
    const float ds = std::min(kPhaseStepPx, regionArc - s);
    phaseTotal += ds * density(s + ds * 0.5f);
    s += ds;
  }
  const float n = std::ceil(phaseTotal - kPhaseSlack);
  if (!(n >= 1.0f) || n > static_cast<float>(kMaxTailDabs)) return;
  const size_t steps = static_cast<size_t>(n);

  float phase = 0.0f;
  size_t k = 1;
  for (float s = 0.0f; s < regionArc && k < steps;) {
    const float ds = std::min(kPhaseStepPx, regionArc - s);
    const float dp = ds * density(s + ds * 0.5f);
    while (k < steps && dp > 0.0f &&
           phase + dp >= static_cast<float>(k) * phaseTotal / n) {
      const float t = (static_cast<float>(k) * phaseTotal / n - phase) / dp;
      out.push_back(at(s + ds * std::clamp(t, 0.0f, 1.0f)));
      ++k;
    }
    phase += dp;
    s += ds;
  }

  out.push_back(tail.back());
  tail.swap(out);
}

}  // namespace np
