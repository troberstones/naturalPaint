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

// Pure: one precise wheel sample (`wheelDx`, `wheelDy`, already the ImGui
// `MouseWheelH`/`MouseWheel` for this frame) -> the amount to add to
// `st.view.panX`/`panY`. 1:1 with the raw delta, deliberately NOT run
// through `kPreciseScrollFraction` above: this is not competing against a
// notch-sized step the way the panel scroll is (a notched wheel over the
// canvas does something else entirely -- it zooms, see
// `zoomFactorForPinch()`'s header comment and `ui/MacPaintUI.cpp`'s
// existing wheel-zoom block), so there is no "full step" to be a fraction
// of; it is simply added the same per-frame way the existing middle-mouse
// drag-pan already adds `ImGui::GetIO().MouseDelta` directly to
// `panX`/`panY` a few lines below it, so the two pan gestures move the
// canvas at the same rate for the same finger/cursor travel.
struct CanvasPanDelta {
  float dx;
  float dy;
};
CanvasPanDelta canvasPanForPreciseWheel(float wheelDx, float wheelDy) noexcept;

}  // namespace np
