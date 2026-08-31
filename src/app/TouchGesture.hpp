#pragma once

#include <cstdint>

namespace np {

// app/TouchGesture -- item 4's raw-touch half: "search the web for best
// practices on trackpad pan/zoom/rotate and apply those fixes."
//
// **Why this exists at all, given app/WheelInput.hpp already ships a
// researched pinch/rotate/pan implementation.** That file's own gesture
// path goes through AppKit's HIGH-LEVEL, pre-classified gesture events
// (`NSEventTypeMagnify`/`NSEventTypeRotate`/two-finger `NSEventTypeScrollWheel`),
// each observed independently via `ui/MacTrackpadGestures.mm`'s local event
// monitor. Apple's own sample documentation for exactly this class of
// feature (the `LightTable` sample's `DualTouchTracker`) states directly:
// "the trackpad driver might switch its interpretation from one gesture to
// another, currently only for magnify and rotate gestures" -- i.e. AppKit
// itself, not this app's code, alternates between reporting ONLY magnify or
// ONLY rotate for one continuous two-finger motion that is actually doing
// both at once. That is the documented, platform-level cause of "it feels
// like gesture selection -- now zoom, now rotate around centre, now pan"
// rather than one continuous, combined transform.
//
// The fix Apple's own sample code uses is to bypass the classifier entirely
// and read the two fingers' RAW positions (`NSTouch`), deriving pan+zoom+
// rotate from their combined motion directly, every frame, rather than
// waiting for AppKit to decide which single gesture type this frame's
// sample belongs to. `ui/MacTrackpadTouch.hpp`/`.mm` is that capture layer;
// this file is its pure, headless-testable half -- the same split
// app/WheelInput.hpp already established, and for the identical reason
// (docs/reachability-audit.md F4: `--selftest` cannot reach an SDL/AppKit
// dispatch site directly).
//
// **What "pin to a point" can and cannot mean here.** A trackpad is an
// INDIRECT pointing device -- the surface under your fingers is physically
// separate from the screen, and `NSTouch.normalizedPosition` is a fraction
// of the TRACKPAD's own surface, with no defined correspondence to any
// screen or document location at all (unlike a touchscreen, where a finger
// literally sits over the pixel it is touching). So "pin two fingers to two
// points ON THE DOCUMENT" is not something a trackpad gesture can mean
// literally, no matter how the touches are read. What IS physically real
// and meaningful, touch-device-agnostic, is the RELATIVE motion between the
// two fingers: how far apart they have moved (-> zoom), how much the line
// between them has turned (-> rotate), and how their shared midpoint has
// slid (-> pan) -- exactly `DualTouchTracker`'s own `deltaSize`/`deltaOrigin`
// idea, generalised with rotation. Zoom and rotate are then anchored at the
// CURSOR (`ui/MacPaintUI.cpp`'s existing `applyZoomFactor`/
// `app/ZoomAndSize.hpp`'s `panForAnchoredZoomRotate()`), which is the one
// screen-meaningful reference point available -- the closest a trackpad can
// get to "pinned", and a real fix on its own: the EXISTING two-finger
// trackpad rotate pivots at the canvas's fixed centre regardless of where
// the cursor is, which this replaces.
//
// **Absolute from gesture-start, not incremental frame-to-frame.** Exactly
// `app/TransformSession`'s own `beginDrag()`/`updateDrag()` discipline
// (snapshot a baseline once, recompute FROM that baseline every frame) and
// for the identical reason: accumulating many small per-frame transforms
// compounds floating-point error over a gesture that can run for seconds,
// where solving once from the ORIGINAL two touch positions to the CURRENT
// two touch positions cannot drift no matter how long the gesture runs.
// `ui/MacTrackpadTouch.hpp` keeps the gesture-start touch pair fixed for
// this reason; `computeTwoTouchDelta()` below always takes both.

// One trackpad touch, matched by `identity` across frames -- Apple's own
// `NSTouch.identity`, opaque and stable for one finger's whole contact with
// the surface. `x`/`y` are ALREADY converted to this app's own y-DOWN screen
// convention by the caller (`ui/MacTrackpadTouch.mm`) -- `NSTouch.
// normalizedPosition` itself is y-UP (AppKit's general convention), and
// getting that flip right at the single point raw touches are read is what
// lets every function below share this app's one rotation-sign convention
// (`app/WheelInput.hpp`: clockwise-positive) with no second, disagreeing
// one. Otherwise dimensionless -- [0,1] within the trackpad's own surface,
// per Apple's documented contract for the field.
struct TrackpadTouchPoint {
  uint64_t identity = 0;
  float x = 0.0f;
  float y = 0.0f;
};

// The combined pan+zoom+rotate a touch pair's motion describes, from
// wherever that SAME pair started (gesture-begin) to now. `scale` is
// multiplicative (1.0 == no change); `rotationRadians` is clockwise-positive
// in this app's own y-down screen convention (see the struct above), zero
// for two touches that have not turned relative to each other; `panDx`/
// `panDy` are the shared midpoint's own motion, in the SAME dimensionless
// trackpad-normalized units the input points are -- the caller
// (`ui/MacPaintUI.cpp`) scales this to screen pixels and applies the
// system's natural/traditional scrolling preference (there is no such
// preference for pinch or rotate, only for a directional drag), neither of
// which is this pure function's concern.
struct TwoTouchDelta {
  float scale = 1.0f;
  float rotationRadians = 0.0f;
  float panDx = 0.0f;
  float panDy = 0.0f;
};

// Pure: two touches' START positions (captured once, at two-touch gesture
// begin) and the SAME two touches' CURRENT positions (already matched to
// `startA`/`startB` by `identity` -- this function does not itself do that
// matching, `ui/MacTrackpadTouch.mm` does, once, per `TrackpadTouchPoint`'s
// own header) -> the absolute pan+zoom+rotate delta described above.
//
// `scale` is the ratio of the two points' CURRENT distance apart to their
// START distance; a degenerate start (the two touches began at, or almost
// at, the same point -- `startDistance < kMinTouchSeparation`) returns
// `scale == 1.0f` rather than dividing by a near-zero denominator, the same
// "cannot divide by an unmeasurable baseline, so treat it as no scale
// change yet" posture `panForAnchoredZoom()`'s own zoom-clamp guards adopt
// elsewhere in this codebase. `rotationRadians` is the change in the angle
// of the line between the two touches, wrapped into (-pi, pi] via
// `app/WheelInput.hpp`'s own `wrapRotationRadians()` -- not a second,
// independently-typed wrap.
inline constexpr float kMinTouchSeparation = 0.02f;

TwoTouchDelta computeTwoTouchDelta(TrackpadTouchPoint startA, TrackpadTouchPoint startB,
                                   TrackpadTouchPoint curA, TrackpadTouchPoint curB) noexcept;

}  // namespace np
