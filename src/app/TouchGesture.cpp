#include "app/TouchGesture.hpp"

#include <cmath>

#include "app/WheelInput.hpp"  // wrapRotationRadians()

namespace np {

TwoTouchDelta computeTwoTouchDelta(TrackpadTouchPoint startA, TrackpadTouchPoint startB,
                                   TrackpadTouchPoint curA, TrackpadTouchPoint curB) noexcept {
  const float startDx = startB.x - startA.x;
  const float startDy = startB.y - startA.y;
  const float startDist = std::sqrt(startDx * startDx + startDy * startDy);

  const float curDx = curB.x - curA.x;
  const float curDy = curB.y - curA.y;
  const float curDist = std::sqrt(curDx * curDx + curDy * curDy);

  TwoTouchDelta out;
  out.scale = startDist < kMinTouchSeparation ? 1.0f : curDist / startDist;
  out.rotationRadians =
      wrapRotationRadians(std::atan2(curDy, curDx) - std::atan2(startDy, startDx));
  out.panDx = ((curA.x + curB.x) - (startA.x + startB.x)) * 0.5f;
  out.panDy = ((curA.y + curB.y) - (startA.y + startB.y)) * 0.5f;
  return out;
}

}  // namespace np
