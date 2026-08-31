#include "app/TouchGestureSession.hpp"

#include <algorithm>

#include "app/WheelInput.hpp"    // wrapRotationRadians()
#include "app/ZoomAndSize.hpp"   // clampViewZoom(), panForAnchoredZoomRotate()

namespace np {

void TouchGestureSession::update(
    std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> touches, CanvasView& view,
    Vec2 canvasCenter, Vec2 paintOrigin, Vec2 avail, Vec2 tex, Vec2 cursorScreen) noexcept {
  if (!touches.has_value()) {
    // Falling edge (or simply "not tracking two touches this frame"):
    // nothing to snapshot for a future gesture until a rising edge sees
    // exactly two again.
    active_ = false;
    lastDelta_ = TwoTouchDelta{};
    return;
  }

  const TrackpadTouchPoint curA = touches->first;
  const TrackpadTouchPoint curB = touches->second;

  if (!active_) {
    // Rising edge: snapshot the gesture's own fixed baseline -- the touch
    // pair's start positions, the view they started against, and where
    // that view's own centre sits on screen right now. Every subsequent
    // frame of this SAME gesture recomputes from this one snapshot, never
    // from last frame's output -- see the header comment on why.
    active_ = true;
    startA_ = curA;
    startB_ = curB;
    viewAtStart_ = view;
    const Vec2 drawSize{tex.x * viewAtStart_.zoom, tex.y * viewAtStart_.zoom};
    const float marginX = std::max(0.0f, (avail.x - drawSize.x) * 0.5f);
    const float marginY = std::max(0.0f, (avail.y - drawSize.y) * 0.5f);
    const Vec2 originAtStart{paintOrigin.x + marginX + viewAtStart_.panX,
                              paintOrigin.y + marginY + viewAtStart_.panY};
    pivotScreenAtStart_ = Vec2{originAtStart.x + drawSize.x * 0.5f,
                                originAtStart.y + drawSize.y * 0.5f};
    lastDelta_ = TwoTouchDelta{};
    return;
  }

  const TwoTouchDelta delta = computeTwoTouchDelta(startA_, startB_, curA, curB);
  lastDelta_ = delta;

  CanvasView newView = viewAtStart_;
  newView.zoom = clampViewZoom(viewAtStart_.zoom * delta.scale);
  newView.rotation = wrapRotationRadians(viewAtStart_.rotation + delta.rotationRadians);

  const AnchoredPan pan =
      panForAnchoredZoomRotate(viewAtStart_, newView, canvasCenter, pivotScreenAtStart_,
                                cursorScreen, paintOrigin, avail, tex);
  newView.panX = pan.panX;
  newView.panY = pan.panY;
  view = newView;
}

}  // namespace np
