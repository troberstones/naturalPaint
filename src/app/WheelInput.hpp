#pragma once

namespace np {

// app/WheelInput -- track10/input, "make Mac trackpad input feel right".
//
// **The bug, restated precisely.** Every wheel-driven control in this build
// (ui/MacPaintUI.cpp's controls-column scroll at the time this file was
// added, and the canvas's wheel-zoom beside it) reads exactly one number,
// `ImGui::GetIO().MouseWheel`, and multiplies it by a constant sized for ONE
// MOUSE NOTCH. A conventional wheel mouse delivers a handful of those a
// second, each meaning "the user made one deliberate click". A MacBook
// trackpad delivers dozens of FRACTIONAL deltas a second for the same single
// two-finger gesture, and every one of them was getting the same
// notch-sized multiplier -- which is both "much too fast" (many notches'
// worth of motion per second of finger travel) and "choppy" (each of those
// oversized jumps lands with a hard `SetScrollY`/pan write, no
// interpolation). This file factors the fix into pure, headless-testable
// functions, because docs/reachability-audit.md F4 already establishes that
// `--selftest` cannot reach an SDL or ImGui dispatch site to test the event
// handler directly.
//
// **How a notch is told apart from a trackpad sample, and where that fact
// comes from.** Not read off this build's own event handling -- it comes
// from the vendored SDL 3.2.24 itself (`third_party`'s CMake-fetched
// `_deps/sdl3-src`). Its Cocoa backend
// (`src/video/cocoa/SDL_cocoamouse.m::Cocoa_HandleMouseWheel`) already asks
// AppKit the question this app would otherwise have to ask a second time --
// `[event hasPreciseScrollingDeltas]` -- and answers it by ROUNDING:
//
//   if (![event hasPreciseScrollingDeltas]) {
//     if (x > 0) x = SDL_ceil(x); else if (x < 0) x = SDL_floor(x);
//     ... (same for y)
//   }
//   SDL_SendMouseWheel(..., x, y, direction);
//
// A conventional wheel (`hasPreciseScrollingDeltas == NO`) is forced to a
// whole number before SDL ever posts the event. A trackpad or Magic Mouse
// (`hasPreciseScrollingDeltas == YES`) is left as AppKit's own continuous,
// fractional value. That boolean itself is NOT re-exposed anywhere in
// `SDL_MouseWheelEvent` (its `integer_x`/`integer_y` fields, added in
// 3.2.12, are a running whole-tick ACCUMULATOR for compatibility with
// integer-only callers -- see `SDL_mouse.c`'s `SDL_SendMouseWheel` -- not a
// device-class flag), and ImGui's SDL3 backend
// (`imgui_impl_sdl3.cpp::ImGui_ImplSDL3_ProcessEvent`) forwards only the
// already-rounded-or-not `event.wheel.x`/`.y` into `io.AddMouseWheelEvent()`.
// So by the time this app reads `ImGui::GetIO().MouseWheel`, the distinction
// survives in exactly one place: whether the value is an exact whole number.
// `wheelDeltaIsPrecise()` below is that test, not a heuristic invented apart
// from what the vendored library actually does.
//
// **What SDL3 does NOT carry: pinch/magnification.** SDL2's
// `SDL_MULTIGESTURE` event is gone in SDL3 -- `SDL_events.h` in this same
// vendored copy explicitly reserves its old event-type values ("were the
// Gesture events from SDL2. Do not reuse these values!") rather than
// repurposing them, and the vendored Cocoa backend has no handler for
// `NSEventTypeMagnify` anywhere in `src/video/cocoa/`. A pinch gesture
// therefore cannot reach this app through SDL at all on this vendored
// version -- confirmed by reading the source, not inferred from an absence
// of a call site. `ui/MacTrackpadGestures.hpp`/`.mm` is the honest
// alternative the track brief asked for when that turns out to be true:
// a small, separate native `NSEvent` local monitor for
// `NSEventTypeMagnify`, built the same way `ui/MacNativeMenu.mm` already
// reaches past SDL for the menu bar. `zoomFactorForPinch()` below is the
// pure half of that path -- the arithmetic a magnification sample turns
// into, independent of how the sample was obtained.

// float32 has ~7 significant decimal digits, and a wheel delta this build
// ever sees is O(1)-O(10) in magnitude; 1e-4 is generous headroom above the
// ~1e-6 relative error such a value actually accumulates by the time it
// reaches this app (`event.wheel.x/y` assigned straight through by SDL, then
// summed by ImGui's own `io.MouseWheel += event.wheel_y` across however many
// events land in one frame -- an add per event, not a multiply, so error
// does not compound). This is a numerical-precision tolerance, not a "feel"
// constant -- it does not change what the classification MEANS.
inline constexpr float kWheelNotchEpsilon = 1e-4f;

// True if `delta` -- an `ImGui::GetIO().MouseWheel`/`MouseWheelH` value,
// already accumulated for the current frame exactly as ImGui reports it --
// came from a continuous, high-frequency pointing device (a trackpad, or a
// Magic Mouse, both of which answer YES to `hasPreciseScrollingDeltas`)
// rather than a conventional notched wheel. False for an exact zero: there
// is nothing to classify, and a caller combining this with a second axis
// (a two-finger scroll that is purely vertical reports `MouseWheelH ==
// 0.0f` exactly, which is technically a whole number) must not let a
// silent zero axis flip the decision.
//
// Not airtight in one documented direction: if several trackpad samples
// land in a single slow frame and happen to sum to an exact whole number,
// this reads as a notch for that one frame. Continuous values summing to
// exactly zero past the fractional part is a measure-zero event for real
// (non-integer) inputs, self-corrects the very next frame, and is a far
// smaller error than treating every trackpad sample as a full notch, which
// is the bug this file exists to fix.
bool wheelDeltaIsPrecise(float delta) noexcept;

// --- panel scroll: how far one wheel sample moves the scroll position -----

// The fraction of a full notch's step ONE UNIT of a precise/trackpad delta
// is worth. Reasoned, not measured -- this build cannot drive a physical
// trackpad from here (see this track's own report for what that leaves
// unverified). A notch is a single, discrete, deliberate action and keeps
// the FULL step (fraction 1.0, unchanged from before this file). A precise
// sample is one of many fired every second during one continuous gesture,
// so even at the SAME nominal magnitude as a notch it is deliberately
// discounted: total on-screen travel for a real gesture still comes from
// the SUM of many such samples (frequency does the rest), while a single
// oversized sample -- the case a fast flick can produce -- is kept from
// reading as one big notch-sized jump. `smoothedScrollStep()` below is the
// second, independent safety margin against exactly that case.
inline constexpr float kPreciseScrollFraction = 0.25f;

// Pure: `delta` wheel units -> pixels to move a panel whose full notch is
// `notchStepPx` (`app/ControlsLayout.hpp`'s `controlsWheelScrollStep()` at
// the one call site this exists for). Sign matches `delta`'s; the caller
// decides which way that pixel amount moves its own scroll position, the
// same as the raw `wheel * step` expression this replaces.
float wheelScrollPixels(float delta, float notchStepPx) noexcept;

// --- panel scroll: smoothing -----------------------------------------------

// Time constant of the exponential smoothing below, in seconds. Sourced
// from the one concrete momentum-scrolling constant this track's web
// research turned up rather than picked freely: Apple's own PastryKit
// library damps a coasting scroll by a factor of 0.95 every 16.7 ms
// (60 fps) animation tick. Converting that discrete per-tick factor to a
// continuous time constant via `tau = -dtTick / ln(factor)`:
//
//   tau = -0.016667 / ln(0.95) = -0.016667 / -0.051293 = 0.3250 s
//
// That is the timescale for a scroll that is already moving and coasting to
// a stop with no new input, which is a slower, longer decay than this file
// needs: here the "remaining" pool is topped up by fresh wheel deltas every
// frame during an active gesture, so is what a viewer actually watches is
// the SETTLING time behind live input, not a post-release coast. Used at
// 1/4 of Apple's own constant (still that same source, not a second freely
// chosen number) for a settle time about four times faster --
// short enough that panel scrolling does not visibly lag the finger, long
// enough that consecutive oversized notch-scaled samples (the case this
// file exists for) blend into motion instead of visibly jumping.
inline constexpr float kScrollSmoothingTauSeconds = 0.3250f / 4.0f;

// One frame of exponential smoothing for a pool of not-yet-applied scroll
// pixels. `pendingPx` is the caller's running remainder (positive or
// negative, carried across frames); `dtSeconds` is the frame's own
// `ImGui::GetIO().DeltaTime`. Returns the pixel amount to apply THIS frame
// and the remainder to carry into the next one, after the caller has added
// any new wheel-driven contribution for this frame to `pendingPx`.
//
// `applied = pendingPx * (1 - exp(-dtSeconds / tau))`: a first-order
// low-pass, frame-rate independent by construction (a fixed per-frame
// fraction would settle twice as fast in wall-clock time on a 120 Hz
// ProMotion display as on 60 Hz, which is the bug this formula avoids
// having of its own). `remainingPx` is `pendingPx` scaled by the SAME
// factor every frame regardless of its sign or magnitude, so it can only
// ever shrink toward zero -- it cannot cross zero, reverse sign, or grow,
// which is what makes this converge without oscillating or overshooting by
// construction rather than by tuning.
struct SmoothedStep {
  float appliedPx;
  float remainingPx;
};
SmoothedStep smoothedScrollStep(float pendingPx, float dtSeconds) noexcept;

// --- canvas: pinch magnification -> a zoom factor --------------------------

// Pure: one `NSEvent.magnification` sample -> the multiplicative factor
// `ui/MacPaintUI.cpp`'s existing `applyZoomFactor()` lambda expects.
// `1.0f + magnification`, not `magnification` alone or some other scale --
// this is Apple's own documented contract for the field (NSEvent.h:
// "the total is calculated as 1.0 + sum of magnification values"), the same
// way every native pinch-to-zoom handler on the platform consumes it, so
// this is a transcription of the device's contract rather than an invented
// mapping. magnification == 0 -> factor == 1 (a no-op sample changes
// nothing, matching every other zero-delta gesture in this file).
float zoomFactorForPinch(float magnification) noexcept;

// --- canvas: two-finger trackpad pan ---------------------------------------

// track11/pan-rotate-reset: "trackpad panning is too slow." Before changing
// the factor, this is what got traced -- the brief for this track assumed
// the path runs through `NSEvent.scrollingDeltaY`; it does not.
//
// **What the vendored SDL 3.2.24 Cocoa backend actually reads.**
// `Cocoa_HandleMouseWheel()` (`_deps/sdl3-src/src/video/cocoa/
// SDL_cocoamouse.m`) does `x = -[event deltaX]; y = [event deltaY];` --
// the LEGACY `deltaX`/`deltaY` accessors, not `scrollingDeltaX`/
// `scrollingDeltaY`. Apple's own shipped `NSEvent.h` (checked on this
// machine, both the current SDK and a mirrored 10.8 SDK) documents
// `scrollingDeltaX`/`Y` as "the preferred API ... When
// -hasPreciseScrollingDeltas returns YES, scroll by the returned value (in
// points)" -- but says nothing, in either SDK, about `deltaX`/`deltaY`
// carrying that same value for a precise device. That equivalence is NOT
// an Apple-documented contract; it is the near-universal assumption of the
// native-app ecosystem (e.g. a 2018 QEMU patch's own commit message,
// replacing `scrollingDeltaY` with `deltaY` for wider OS-version support:
// "does the same thing" -- one independent, corroborating data point, not
// a specification). This build cannot drive a physical trackpad to check
// it directly either. So: ASSUMED, not proven, and flagged as such.
//
// **What ImGui does with it.** `imgui_impl_sdl3.cpp`'s
// `ImGui_ImplSDL3_ProcessEvent()` forwards SDL's value straight into
// `io.AddMouseWheelEvent()` with only a sign flip on X (which cancels
// against the sign flip SDL's own backend already applied to `deltaX`, so
// the net magnitude reaching this app is untouched either way) -- no
// scaling anywhere in that path. Confirmed by reading both vendored
// sources directly, not inferred.
//
// **The conclusion this trace actually supports.** IF the assumed
// equivalence holds, one unit of `wheelDx`/`wheelDy` here is one POINT of
// "the amount AppKit itself would scroll a view's content by" (Apple's own
// wording) -- the exact same screen-space unit `ImGui::GetIO().MouseDelta`
// already uses for `panX`/`panY` a few lines below this comment's call
// site (`ui/MacPaintUI.cpp`'s middle-mouse/Hand-tool drag pan). So the
// PREVIOUS 1:1 mapping was not a units bug -- tracing it through confirms,
// rather than refutes, its own header's claim that it "matches the
// middle-mouse drag-pan rate for the same finger/cursor travel." Both really
// are the same physical unit, assuming the one thing this build cannot
// verify.
//
// **So why speed it up at all, and by how much.** A 1:1-with-AppKit's-own-
// content-scroll-rate mapping is exactly right for READING a scrollable
// document (Preview, Safari, Mail) -- fine control, no overshoot. It is not
// obviously right for NAVIGATING a 2D canvas, where covering the visible
// working area is the point and a trackpad's usable throw is small and
// physically bounded (a few hundred points before the fingers run off the
// pad and the gesture has to be released and restarted) in a way a
// mouse-drag's throw is not. That is a real, previously-unstated design
// difference between the two gestures the ORIGINAL comment's "matches the
// drag-pan rate" argument glossed over -- equal RATE per point is not equal
// THROUGHPUT per gesture when one input device's points-per-swipe is capped
// far lower than the other's. Nothing in the traced units gives a number for
// how much faster to make it, though -- that is a feel judgment, admitted as
// one, in the same spirit `kPreciseScrollFraction` above already admits it
// cannot be measured here. `kCanvasPanSpeedFactor` below is that admitted
// judgment call: a small enough multiple that a moderate flick cannot fling
// the canvas out of the window in one twitch, comfortably larger than 1.0 so
// the change is actually perceptible rather than lost in gesture-to-gesture
// noise. A person who CAN drive a physical trackpad from here should retune
// this one named constant; nothing else in `canvasPanForPreciseWheel()`
// needs to change to retune it.
inline constexpr float kCanvasPanSpeedFactor = 3.0f;

// Pure: one precise wheel sample (`wheelDx`, `wheelDy`, already the ImGui
// `MouseWheelH`/`MouseWheel` for this frame) -> the amount to add to
// `st.view.panX`/`panY`. `kCanvasPanSpeedFactor` times the raw delta,
// deliberately NOT run through `kPreciseScrollFraction` above: this is not
// competing against a notch-sized step the way the panel scroll is (a
// notched wheel over the canvas does something else entirely -- it zooms,
// see `zoomFactorForPinch()`'s header comment and `ui/MacPaintUI.cpp`'s
// existing wheel-zoom block), so there is no "full step" to be a fraction
// of.
//
// **Deliberately NOT scaled by `st.view.zoom`.** `panX`/`panY` are already
// screen-space pixels -- they are added directly to `origin`, the same
// screen coordinate the middle-mouse drag-pan's `MouseDelta` is added to a
// few lines below this function's one call site -- so a fixed swipe already
// moves a fixed number of SCREEN pixels at any zoom, exactly matching how
// the drag-pan (also unscaled by zoom) already behaves, and how every
// mainstream canvas/image editor's hand-tool panning behaves: the same
// swipe feels the same size on screen whether zoomed in or out. Scaling by
// 1/zoom instead (to hold DOCUMENT distance constant per swipe) would make
// the two pan gestures disagree with each other at any zoom besides 1.0x,
// which is the exact inconsistency the original 1:1 choice was trying to
// avoid in the first place -- and it would make panning at high zoom
// require MORE finger travel to cross the same visible screen distance,
// the opposite of "too slow"'s fix.
struct CanvasPanDelta {
  float dx;
  float dy;
};
CanvasPanDelta canvasPanForPreciseWheel(float wheelDx, float wheelDy) noexcept;

// --- canvas: two-finger trackpad rotate -------------------------------------

// Pure: one `NSEvent.rotation` sample (already in DEGREES, as Apple's own
// `NSEvent.h` documents it: "In degrees ... For NSEventTypeRotate, it is
// rotation on the track pad") -> the radians to ADD to `st.view.rotation`.
//
// **Sign.** Not stated in either AppKit header this track checked on this
// machine (current SDK and a mirrored 10.8 SDK both leave `rotation`'s
// doc comment silent on direction) -- so this is NOT transcribed from an
// Apple-documented contract the way `zoomFactorForPinch()`'s "1.0 +
// magnification" is. It is corroborated by an Apple Developer Forums
// explanation ("If the amount of rotation is positive, the direction is
// counterclockwise ... If negative, clockwise") and by a second,
// independent implementation (Hammerspoon's `libeventtap_event.m`, which
// documents the identical convention) -- two agreeing secondary sources,
// not a primary one, and not something this build can spin a physical
// trackpad to confirm.
//
// Given that (NSEvent.rotation counterclockwise-positive), the negation
// below is what makes the gesture feel like direct manipulation: hand-
// computing `ViewTransform`'s own rotation matrix (see that header's test
// case (b) -- zoom=2, mirrorX, rotation=+90deg, canvasCenter=(100,50),
// pivotScreen=(300,200) -- lands the transformed point ABOVE the pivot on
// screen for a POSITIVE `view.rotation`, i.e. a canvas point sweeps from
// "right" toward "up", which on a clock face (3 o'clock toward 12 o'clock,
// screen y increasing downward) is the CLOCKWISE direction as a person
// looking at the screen actually sees it) shows `CanvasView::rotation` is
// CLOCKWISE-positive on screen -- the opposite sense from
// `NSEvent.rotation`'s counterclockwise-positive. The existing `R`+drag
// rotate gesture (`ui/MacPaintUI.cpp`'s `rotating` block) independently
// confirms the same clockwise-positive sense the other direction: its
// `(v.x*d.y - v.y*d.x)/r2` cross-product term comes out positive for a
// mouse point sweeping clockwise around the pivot, and that positive value
// is added straight to `st.view.rotation` -- so "drag clockwise, canvas
// turns clockwise" already holds for the mouse gesture. Negating
// `NSEvent.rotation` here is what makes "twist your fingers clockwise,
// canvas turns clockwise" hold for the trackpad gesture too, the same feel
// through a second input device rather than a second, disagreeing one.
//
// magnitude == 0 -> 0 radians (a no-op sample changes nothing, matching
// every other zero-delta gesture in this file).
float canvasRotationRadiansForTrackpad(float rotationDegrees) noexcept;

// Pure: wraps `radians` into the canonical range (-pi, pi]. `sinf`/`cosf`
// (what `ViewTransform`'s matrix build actually calls) are periodic and so
// are mathematically correct for ANY float argument, but a rotation gesture
// fires many samples per second and `CanvasView::rotation`'s own comment
// calls it "arbitrary angle" with no existing wraparound -- left unbounded,
// repeated rotate gestures over a long session walk the stored float
// further and further from zero, and a huge float argument to `sinf`/`cosf`
// loses precision in its own internal range reduction (the classic
// "sin(1e8)" problem) well before it loses the ability to compile. Wrapping
// after every accumulation keeps the stored value, and therefore that
// range-reduction error, bounded for the life of the session -- a real
// hygiene fix, not merely a display nicety, since nothing here ever reads
// `view.rotation` in degrees for display.
float wrapRotationRadians(float radians) noexcept;

}  // namespace np
