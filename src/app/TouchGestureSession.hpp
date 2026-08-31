#pragma once

#include <optional>
#include <utility>

#include "app/AppState.hpp"      // CanvasView
#include "app/TouchGesture.hpp"  // TrackpadTouchPoint, TwoTouchDelta
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// app/TouchGestureSession -- app/TouchGesture.hpp's pure delta math, held
// across a whole two-finger gesture the "absolute from gesture-start, not
// incremental" way that header commits to (see its own comment for why:
// the same drift argument as app/TransformSession's beginDrag()/
// updateDrag()). computeTwoTouchDelta() alone is stateless -- it needs
// SOMETHING to remember the touch pair's own starting positions, and the
// view/pivot geometry those started against, across many frames of one
// continuous gesture. This is that something.
//
// `update()` is meant to be called once per frame, unconditionally, with
// whatever ui/MacTrackpadTouch.hpp's poll function reports RIGHT NOW: a
// rising edge (was not tracking two touches, now is) snapshots gesture
// state; a falling edge (was tracking, now is not -- lifted fingers, or a
// third touch joined) clears it; two touches present on consecutive frames
// recompute the WHOLE view from that one fixed snapshot every time, never
// accumulating this frame's output onto last frame's.
//
// Deliberately NOT the touch-pan (`TwoTouchDelta::panDx`/`panDy`) -> screen
// pixels conversion, and deliberately NOT the system natural/traditional
// scrolling preference -- both are ui/MacTrackpadTouch.hpp's own concern
// (the physical trackpad's `deviceSize` and `NSUserDefaults`, neither of
// which this pure class has or needs), applied by the caller to `panDx`/
// `panDy` before folding the result into whatever this returns. What this
// class owns is only the zoom+rotate anchoring at the cursor -- the part
// app/ZoomAndSize.hpp's `panForAnchoredZoomRotate()` already does, driven
// from a fixed baseline instead of last frame's view.
class TouchGestureSession {
 public:
  // `touches`: the current two-touch pair (matched by identity, already
  // Y-flipped to this app's y-down convention -- see
  // app/TouchGesture.hpp's `TrackpadTouchPoint` comment), or `std::nullopt`
  // when fewer or more than two touches are down right now.
  //
  // `view`: read for its CURRENT zoom/rotation/pan on a rising edge (to
  // snapshot the gesture-start baseline) and WRITTEN with the new zoom/
  // rotation/pan on every frame a gesture is active; untouched when no
  // gesture is active.
  //
  // `canvasCenter`/`paintOrigin`/`avail`/`tex`/`cursorScreen`: this frame's
  // own canvas layout and anchor point, exactly what
  // app/ZoomAndSize.hpp's `panForAnchoredZoomRotate()` itself takes --
  // ui/MacPaintUI.cpp already computes all of these for its own
  // `ViewTransform` at the top of its canvas block, so this reads them
  // rather than re-deriving a second copy. Layout is assumed stable across
  // one gesture's lifetime (a resize mid-gesture is not defended against),
  // the same posture the existing scrubby-zoom drag already takes with
  // `io.MouseClickedPos[0]`.
  //
  // Writes the anchored zoom+rotate result into `view` and returns void --
  // the touch pair's own translation (`lastDelta().panDx`/`panDy`) is NOT
  // folded in here; the caller reads it via `lastDelta()`, converts it to
  // screen points (trackpad `deviceSize`) and the natural/traditional
  // scrolling preference, and adds it to `view.panX`/`view.panY` itself --
  // this class has no way to do that conversion (see the class comment
  // above). `view` is left untouched on any frame no gesture is active
  // (fewer/more than two touches).
  void update(std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> touches,
              CanvasView& view, Vec2 canvasCenter, Vec2 paintOrigin, Vec2 avail, Vec2 tex,
              Vec2 cursorScreen) noexcept;

  // The touch pair's raw `panDx`/`panDy` for THIS frame, already computed
  // against the gesture's own fixed starting positions -- exposed so the
  // caller can convert it to screen points (trackpad `deviceSize`) and
  // apply the scrolling-direction preference, then add it to what
  // `update()` already wrote. {0, 0} whenever `update()` itself returned
  // {0, 0} for the same reason (no gesture active this frame).
  TwoTouchDelta lastDelta() const noexcept { return lastDelta_; }

 private:
  bool active_ = false;
  TrackpadTouchPoint startA_{};
  TrackpadTouchPoint startB_{};
  CanvasView viewAtStart_{};
  Vec2 pivotScreenAtStart_{};
  TwoTouchDelta lastDelta_{};
};

}  // namespace np
