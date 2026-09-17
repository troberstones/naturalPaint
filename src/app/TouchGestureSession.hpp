#pragma once

#include <optional>
#include <utility>

#include "app/AppState.hpp"      // CanvasView
#include "app/TouchGesture.hpp"  // TrackpadTouchPoint, TwoTouchDelta
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// app/TouchGestureSession -- a two-finger gesture held across frames as a
// CHAIN OF ONE-FRAME ANCHOR SOLVES: given last frame's two touch points and
// this frame's, find the canvas point that was under last frame's midpoint and
// solve zoom, rotation and pan together so that same canvas point lands under
// this frame's midpoint. Scale comes from the ratio of the fingers'
// separation, rotation from the change in the angle between them, and
// translation is not a separate term at all -- it falls out of the same solve.
//
// **Why this replaces the previous "absolute from a gesture-start baseline"
// model.** That model was sound for zoom and rotate, but the touch pair's own
// translation was not part of its solve: `TwoTouchDelta::panDx/panDy` measure
// the midpoint's motion SINCE THE GESTURE BEGAN, and the caller added that
// absolute quantity to `view.panX/panY` with `+=` on every frame. So the whole
// accumulated translation was re-applied once per frame for as long as the
// gesture lasted -- the view kept sliding while the fingers were held still,
// which is the "swimmy, uncontrollable" feel reported on iOS. Folding pan into
// the anchor solve removes the separate additive step that could compound, and
// states the intended behaviour directly: whatever was under your fingers a
// moment ago is still under your fingers now.
//
// Frame-to-frame rather than from a fixed baseline is also what makes each
// frame start from where the view ACTUALLY is, so a clamp (zoom hitting its
// limit) or any other external change to the view is absorbed rather than
// fought by a stale baseline.
enum class TouchAnchor {
  // A touchscreen: the fingers are literally over the pixels they are
  // touching, so the midpoint between them IS a screen position and the
  // anchor is the real one the reference behaviour describes.
  TouchMidpoint,
  // A trackpad: an INDIRECT device whose surface has no correspondence to any
  // screen location (`app/TouchGesture.hpp`'s own header makes this argument
  // at length), so there is no honest absolute midpoint. Only the RELATIVE
  // motion is physical; the cursor is the one screen-meaningful reference
  // point available, so the anchor sits there and is carried by the
  // midpoint's frame-to-frame travel.
  Cursor,
};

class TouchGestureSession {
 public:
  // `touchesScreen`: the current two-touch pair, ALREADY converted by the
  // caller from its device's own normalised units into screen coordinates --
  // the same units as `ImGui::GetIO().MousePos`, which is what makes the
  // anchor solve's arithmetic dimensionally honest. On a touchscreen that
  // conversion is exact (finger coordinates are normalised to the window); on
  // a trackpad it is `normalizedPosition * deviceSize`, whose absolute origin
  // is meaningless but whose DIFFERENCES are real screen-space travel -- which
  // is all `TouchAnchor::Cursor` uses them for. `std::nullopt` when fewer or
  // more than two touches are down right now.
  //
  // `view`: read for its CURRENT zoom/rotation/pan every frame (this model has
  // no gesture-start baseline to read instead) and WRITTEN with the solved
  // zoom/rotation/pan; untouched when no gesture is active.
  //
  // `canvasCenter`/`paintOrigin`/`avail`/`tex`/`cursorScreen`: this frame's own
  // canvas layout and cursor, exactly what `app/ZoomAndSize.hpp`'s
  // `panForAnchoredZoomRotateTo()` takes -- `ui/MacPaintUI.cpp` already
  // computes all of these for its own `ViewTransform`, so this reads them
  // rather than re-deriving a second copy.
  //
  // `panSign`: +1 to follow the fingers, -1 to oppose them (macOS's
  // traditional-scrolling preference). A touchscreen always follows the
  // finger, so it always passes +1 -- there is no such preference for a
  // direct device. Kept a plain runtime argument rather than a compile-time
  // branch so `--selftest` exercises the shape that actually ships.
  void update(std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> touchesScreen,
              CanvasView& view, Vec2 canvasCenter, Vec2 paintOrigin, Vec2 avail, Vec2 tex,
              Vec2 cursorScreen, TouchAnchor anchor = TouchAnchor::Cursor,
              float panSign = 1.0f) noexcept;

  // True while a two-touch gesture is being tracked -- the rising edge frame
  // included, which applies no transform (there is no previous frame to solve
  // against yet).
  bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
  // Last frame's touch pair, in screen units. The whole of this class's
  // memory: there is no gesture-start snapshot, by design.
  TrackpadTouchPoint prevA_{};
  TrackpadTouchPoint prevB_{};
};

}  // namespace np
