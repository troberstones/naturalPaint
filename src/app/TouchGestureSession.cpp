#include "app/TouchGestureSession.hpp"

#include <algorithm>
#include <cmath>

#include "app/WheelInput.hpp"   // wrapRotationRadians()
#include "app/ZoomAndSize.hpp"  // clampViewZoom(), panForAnchoredZoomRotateTo()

namespace np {

namespace {

Vec2 midpointOf(const TrackpadTouchPoint& a, const TrackpadTouchPoint& b) noexcept {
  return Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

float separationOf(const TrackpadTouchPoint& a, const TrackpadTouchPoint& b) noexcept {
  return std::hypot(b.x - a.x, b.y - a.y);
}

float angleOf(const TrackpadTouchPoint& a, const TrackpadTouchPoint& b) noexcept {
  return std::atan2(b.y - a.y, b.x - a.x);
}

// Below this the fingers are too close together for their separation to carry
// a usable scale ratio or angle -- the same "cannot divide by an unmeasurable
// baseline" posture `computeTwoTouchDelta()` already takes, in screen units
// here rather than normalised ones.
constexpr float kMinSeparationPx = 4.0f;

}  // namespace

void TouchGestureSession::update(
    std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> touchesScreen, CanvasView& view,
    Vec2 canvasCenter, Vec2 paintOrigin, Vec2 avail, Vec2 tex, Vec2 cursorScreen,
    TouchAnchor anchor, float panSign) noexcept {
  if (!touchesScreen.has_value()) {
    active_ = false;
    return;
  }

  const TrackpadTouchPoint curA = touchesScreen->first;
  const TrackpadTouchPoint curB = touchesScreen->second;

  if (!active_) {
    // Rising edge: there is no previous frame to solve against, so this frame
    // only records where the fingers are. Applying anything here would have to
    // invent a baseline.
    active_ = true;
    prevA_ = curA;
    prevB_ = curB;
    return;
  }

  const float prevSep = separationOf(prevA_, prevB_);
  const float curSep = separationOf(curA, curB);
  const Vec2 prevMid = midpointOf(prevA_, prevB_);
  const Vec2 curMid = midpointOf(curA, curB);

  // Scale and rotation are frame-to-frame RATIOS/DIFFERENCES, not quantities
  // measured from a baseline. A pair too close together to measure contributes
  // no scale and no rotation this frame, but its midpoint still pans -- a
  // degenerate separation says nothing about where the fingers are.
  const bool measurable = prevSep >= kMinSeparationPx && curSep >= kMinSeparationPx;
  const float scaleRatio = measurable ? curSep / prevSep : 1.0f;
  const float deltaRotation =
      measurable ? wrapRotationRadians(angleOf(curA, curB) - angleOf(prevA_, prevB_)) : 0.0f;

  CanvasView newView = view;
  newView.zoom = clampViewZoom(view.zoom * scaleRatio);
  newView.rotation = wrapRotationRadians(view.rotation + deltaRotation);

  // Where the canvas's own centre sits on screen under the view as it stands
  // RIGHT NOW -- recomputed each frame from the live view rather than carried
  // from a gesture-start snapshot, which is what makes each frame's solve
  // start from where the view actually is. Same algebra as the inverse in
  // `panForAnchoredZoomRotateTo()`: pivot = paintOrigin + margin + pan +
  // drawSize/2.
  const Vec2 drawSize{tex.x * view.zoom, tex.y * view.zoom};
  const float marginX = std::max(0.0f, (avail.x - drawSize.x) * 0.5f);
  const float marginY = std::max(0.0f, (avail.y - drawSize.y) * 0.5f);
  const Vec2 pivotScreenNow{paintOrigin.x + marginX + view.panX + drawSize.x * 0.5f,
                            paintOrigin.y + marginY + view.panY + drawSize.y * 0.5f};

  // The anchor, and where it has to end up. On a touchscreen this is literally
  // the previous midpoint moving to the current one. On a trackpad there is no
  // honest absolute midpoint (see `TouchAnchor`), so the cursor stands in as
  // the anchor and is carried by the midpoint's own frame-to-frame travel --
  // which keeps that path's existing cursor-anchored feel while still folding
  // the translation into this one solve.
  const Vec2 midTravel{(curMid.x - prevMid.x) * panSign, (curMid.y - prevMid.y) * panSign};
  const Vec2 anchorOld = anchor == TouchAnchor::TouchMidpoint ? prevMid : cursorScreen;
  const Vec2 anchorNew{anchorOld.x + midTravel.x, anchorOld.y + midTravel.y};

  const AnchoredPan pan =
      panForAnchoredZoomRotateTo(view, newView, canvasCenter, pivotScreenNow, anchorOld, anchorNew,
                                 paintOrigin, avail, tex);
  newView.panX = pan.panX;
  newView.panY = pan.panY;
  view = newView;

  prevA_ = curA;
  prevB_ = curB;
}

}  // namespace np
