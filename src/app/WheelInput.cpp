#include "app/WheelInput.hpp"

#include <cmath>

namespace np {

bool wheelDeltaIsPrecise(float delta) noexcept {
  if (delta == 0.0f) return false;
  const float rounded = std::round(delta);
  return std::fabs(delta - rounded) >= kWheelNotchEpsilon;
}

float wheelScrollPixels(float delta, float notchStepPx) noexcept {
  if (delta == 0.0f) return 0.0f;
  const float fraction = wheelDeltaIsPrecise(delta) ? kPreciseScrollFraction : 1.0f;
  return delta * notchStepPx * fraction;
}

SmoothedStep smoothedScrollStep(float pendingPx, float dtSeconds) noexcept {
  if (pendingPx == 0.0f) return {0.0f, 0.0f};
  // A non-positive/degenerate dt (first frame, a paused frame clock under
  // --selftest driving this directly) applies the whole remainder at once
  // rather than dividing by zero or freezing forever -- the same "never
  // silently stall" posture `controlsWheelScrollStep()` already takes for a
  // degenerate window height.
  if (!(dtSeconds > 0.0f)) return {pendingPx, 0.0f};
  const float factor = 1.0f - std::exp(-dtSeconds / kScrollSmoothingTauSeconds);
  const float applied = pendingPx * factor;
  return {applied, pendingPx - applied};
}

float zoomFactorForPinch(float magnification) noexcept { return 1.0f + magnification; }

CanvasPanDelta canvasPanForPreciseWheel(float wheelDx, float wheelDy) noexcept {
  return {wheelDx * kCanvasPanSpeedFactor, wheelDy * kCanvasPanSpeedFactor};
}

namespace {
// 3.14159265358979323846f truncated to float32 precision -- not an M_PI
// dependency (not every toolchain/standard-library combination defines it),
// the same reason `app/selftest/ViewTransform.cpp`'s hand-computed 90-degree
// case above spells out `1.5707963267948966f` instead of `M_PI/2` too.
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
}  // namespace

float canvasRotationRadiansForTrackpad(float rotationDegrees) noexcept {
  // Negated: NSEvent.rotation is counterclockwise-positive (see this
  // function's header comment for the two corroborating sources), while
  // CanvasView::rotation is clockwise-positive on screen -- negating here is
  // what keeps "twist fingers clockwise" and "canvas turns clockwise"
  // agreeing with each other.
  return -rotationDegrees * kDegToRad;
}

float wrapRotationRadians(float radians) noexcept {
  // Wrap into (-pi, pi] via a single fmod plus a boundary fixup, rather than
  // a loop of +=/-= kTwoPi -- a loop would be O(n) in how far out of range
  // the input has drifted, which is exactly the unbounded-session case this
  // function exists to keep cheap.
  const float twoPi = 2.0f * kPi;
  float wrapped = std::fmod(radians + kPi, twoPi);
  if (wrapped <= 0.0f) wrapped += twoPi;  // std::fmod's result can be negative
  return wrapped - kPi;
}

}  // namespace np
