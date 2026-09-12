#include "core/Gradient.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

// core/Gradient -- implementation of the model half. Every decision a reader
// would want justified is in the header; this file carries only the notes that
// are about the code rather than about the design. The tile-store half lives
// in ops/Gradient.cpp.
namespace np {
namespace {

// Two pi, and its reciprocal, as the angular sweep's normaliser. Written out
// rather than pulled from <numbers> because the rest of this codebase does not
// use <numbers> and one file introducing it is churn, not consistency.
constexpr float kTwoPi = 6.283185307179586476925286766559f;

// The per-segment midpoint skew, shared by the colour and opacity ramps so the
// two cannot drift into different curve shapes for the same authored number.
//
// `t` is the already-normalised position within one segment, in [0, 1].
// Returns the reparameterised position, with `midpoint` sent to exactly 0.5.
//
// The clamp band is the interesting part. A midpoint of exactly 0 or 1 sends
// `ln(midpoint)` to -inf or 0 and the exponent to 0 or +inf, turning the
// segment into a step -- so both ends are pulled inside by a margin. 1e-3 is
// far below any value a UI can produce from a draggable diamond (a 256-px-wide
// gradient bar cannot express a midpoint finer than ~4e-3) while still being
// large enough that the exponent stays a finite, well-conditioned number:
// ln(0.5)/ln(1e-3) = 0.1003 at one end and ln(0.5)/ln(0.999) = 692.8 at the
// other, both of which `powf` handles exactly as an ordinary power.
float applyMidpointSkew(float t, float midpoint) noexcept {
  if (midpoint == 0.5f) return t;  // the overwhelmingly common case, exactly
  const float m = std::clamp(midpoint, 1e-3f, 1.0f - 1e-3f);
  // t is guaranteed in [0,1] by the caller, so no negative base reaches powf.
  return std::pow(t, std::log(0.5f) / std::log(m));
}

// Where `t` falls in a sorted stop list: the index of the last stop at or
// before `t`, plus the normalised position within the segment that follows.
//
// Returns `{index, localT}`. `index == stops.size() - 1` means `t` is at or
// past the final stop and `localT` is meaningless (the caller extrapolates
// flat). Linear scan rather than a binary search: stop lists are small -- a
// gradient with more than a dozen stops is pathological, and the branch
// predictor eats a scan of four -- and a scan has no off-by-one to get wrong.
template <class StopT>
std::pair<size_t, float> locateSegment(const std::vector<StopT>& stops, float t) noexcept {
  const size_t n = stops.size();
  if (t <= stops.front().position) return {0, 0.0f};
  for (size_t i = 0; i + 1 < n; ++i) {
    const float a = stops[i].position;
    const float b = stops[i + 1].position;
    if (t < b) {
      // Coincident stops (a == b) are legal and mean a hard edge; the span is
      // zero so there is nothing to interpolate across, and localT = 0 takes
      // the left stop. `t < b` above already sent everything at or past `b` to
      // a later segment, so the hard edge lands on the right side.
      const float span = b - a;
      return {i, span > 0.0f ? (t - a) / span : 0.0f};
    }
  }
  return {n - 1, 0.0f};
}

}  // namespace

void sortGradientStops(GradientStops& stops) {
  // Stable, so two stops the user dragged onto the same position keep the
  // order they were created in -- which is what decides which of them wins on
  // the left of the hard edge `locateSegment()` describes. An unstable sort
  // would make a coincident pair flip appearance for no user-visible reason.
  std::stable_sort(stops.colorStops.begin(), stops.colorStops.end(),
                   [](const ColorStop& a, const ColorStop& b) { return a.position < b.position; });
  std::stable_sort(
      stops.opacityStops.begin(), stops.opacityStops.end(),
      [](const OpacityStop& a, const OpacityStop& b) { return a.position < b.position; });
}

void reverseGradientStops(GradientStops& stops) {
  // One helper for both lists: they differ only in which payload field they
  // carry, and the position/midpoint bookkeeping -- the half worth getting
  // right -- is identical.
  auto flip = [](auto& list) {
    const size_t n = list.size();
    if (n == 0) return;
    // Midpoints first, read off the ORIGINAL order: segment i becomes segment
    // n-2-i, and its 50 % blend lands at 1 - m of the way along.
    std::vector<float> midpoints(n, 0.5f);
    for (size_t i = 0; i + 1 < n; ++i) midpoints[n - 2 - i] = 1.0f - list[i].midpoint;
    std::reverse(list.begin(), list.end());
    for (size_t i = 0; i < n; ++i) {
      list[i].position = 1.0f - list[i].position;
      list[i].midpoint = midpoints[i];
    }
  };
  flip(stops.colorStops);
  flip(stops.opacityStops);
}

float gradientParameterAt(const GradientGeometry& geometry, float px, float py) noexcept {
  const float dx = geometry.x1 - geometry.x0;
  const float dy = geometry.y1 - geometry.y0;
  const float vx = px - geometry.x0;
  const float vy = py - geometry.y0;

  // Degenerate drag: no direction, no length, no honest answer. Flat fill of
  // the first stop rather than NaN -- the header says why.
  const float lenSq = dx * dx + dy * dy;
  if (lenSq <= 0.0f) return 0.0f;

  float t = 0.0f;
  switch (geometry.kind) {
    case GradientKind::Linear:
      // Projection onto the drag, normalised by its length. The division is by
      // lenSq rather than len precisely so there is no sqrt here: (v.d)/|d|^2
      // is already the fraction, and the two-step form would cost a square
      // root per texel for the same number.
      t = (vx * dx + vy * dy) / lenSq;
      break;
    case GradientKind::Radial:
      t = std::sqrt((vx * vx + vy * vy) / lenSq);
      break;
    case GradientKind::Angular: {
      // Angle of the sample relative to the drag direction. `atan2` of the
      // difference would need an explicit wrap anyway, so the subtraction is
      // done on the angles and the wrap below handles both it and the branch
      // cut in one place.
      const float a = std::atan2(vy, vx) - std::atan2(dy, dx);
      t = a * (1.0f / kTwoPi);
      // Wrap into [0, 1). Angular ignores `spread` -- it is periodic by
      // construction and there is nothing outside the range to pad.
      t -= std::floor(t);
      return t;
    }
  }

  switch (geometry.spread) {
    case GradientSpread::Pad:
      return std::clamp(t, 0.0f, 1.0f);
    case GradientSpread::Repeat:
      return t - std::floor(t);
    case GradientSpread::Reflect: {
      // Triangle wave with period 2: fold [1, 2) back onto [1, 0).
      const float wrapped = t - 2.0f * std::floor(t * 0.5f);
      return wrapped <= 1.0f ? wrapped : 2.0f - wrapped;
    }
  }
  return t;
}

std::array<float, 3> gradientColorAt(const GradientStops& stops, float t) noexcept {
  const auto& cs = stops.colorStops;
  // Black rather than a signalling value: no render path ever asks (zero
  // colour stops paints nothing), so this exists only so a UI preview calling
  // the pure function on a half-built stop list gets a colour instead of
  // undefined behaviour.
  if (cs.empty()) return {0.0f, 0.0f, 0.0f};
  if (cs.size() == 1) return cs.front().color;

  const auto [i, localT] = locateSegment(cs, t);
  if (i + 1 >= cs.size()) return cs.back().color;  // flat extrapolation, right
  if (t <= cs.front().position) return cs.front().color;  // flat, left

  const float u = applyMidpointSkew(localT, cs[i].midpoint);
  const std::array<float, 3>& a = cs[i].color;
  const std::array<float, 3>& b = cs[i + 1].color;
  // The lerp, in LINEAR light, on STRAIGHT colour -- the two commitments the
  // header spends its length on, and they are both just this line being
  // reached with un-premultiplied linear values in `a` and `b`.
  //
  // Written as a + (b - a) * u rather than a*(1-u) + b*u: the first form is
  // exact at u == 0 and u == 1 for every float (a + 0 and a + (b - a)), so a
  // stop's own authored colour survives at its own position bit-exactly. The
  // second form is not exact at the endpoints in general.
  return {a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u};
}

float gradientOpacityAt(const GradientStops& stops, float t) noexcept {
  const auto& os = stops.opacityStops;
  // **The asymmetry.** No opacity stops means OPAQUE, not transparent. See
  // GradientStops in the header: an unauthored opacity ramp is the common case
  // and it must not make the gradient invisible.
  if (os.empty()) return 1.0f;
  if (os.size() == 1) return std::clamp(os.front().opacity, 0.0f, 1.0f);

  const auto [i, localT] = locateSegment(os, t);
  if (i + 1 >= os.size()) return std::clamp(os.back().opacity, 0.0f, 1.0f);
  if (t <= os.front().position) return std::clamp(os.front().opacity, 0.0f, 1.0f);

  const float u = applyMidpointSkew(localT, os[i].midpoint);
  const float a = os[i].opacity;
  const float b = os[i + 1].opacity;
  // Clamped after the lerp, not before: two in-range stops cannot produce an
  // out-of-range interpolant, so this only bites when the caller authored an
  // out-of-range opacity, and clamping the result is what keeps the stored
  // texel a well-formed premultiplied one.
  return std::clamp(a + (b - a) * u, 0.0f, 1.0f);
}

std::array<float, 4> gradientSampleStraight(const GradientStops& stops, float t) noexcept {
  const std::array<float, 3> rgb = gradientColorAt(stops, t);
  return {rgb[0], rgb[1], rgb[2], gradientOpacityAt(stops, t)};
}

}  // namespace np
