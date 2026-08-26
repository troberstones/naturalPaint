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
  return {wheelDx, wheelDy};
}

}  // namespace np
