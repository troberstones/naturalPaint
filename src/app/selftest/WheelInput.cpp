#include "app/selftest/Support.hpp"

#include "app/WheelInput.hpp"
#include "app/ZoomAndSize.hpp"

namespace np {

// track10/input: "make Mac trackpad input feel right". app/WheelInput.hpp's
// header comment carries the argument (what the vendored SDL 3.2.24 and
// ImGui's SDL3 backend actually hand this app, and why SDL3 cannot deliver
// pinch at all here); this is the check that the pure functions it factored
// the decision into still obey it. `--selftest` cannot reach the SDL/ImGui
// dispatch sites themselves (docs/reachability-audit.md F4), which is the
// whole reason the decision logic was pulled out of
// `ui/MacPaintUI.cpp`'s event handler into headless functions in the first
// place.
bool runWheelInputTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  std::printf("[selftest] wheel input: nothing here reaches SDL, ImGui or the GPU\n");

  // ==========================================================================
  // (a) wheelDeltaIsPrecise(): the notch/trackpad classifier.
  //
  // Derived from what SDL 3.2.24's vendored Cocoa backend actually does
  // (`Cocoa_HandleMouseWheel`, `src/video/cocoa/SDL_cocoamouse.m`): a
  // conventional wheel's delta is rounded to a whole number with
  // `SDL_ceil`/`SDL_floor` before the event is even posted; a trackpad or
  // Magic Mouse's is left as AppKit's own continuous fractional value. So
  // the property under test is exactly "whole numbers read as a notch,
  // anything else reads as precise" -- not a plausible-sounding threshold.
  // ==========================================================================
  {
    check(!wheelDeltaIsPrecise(0.0f), "precise: exact zero is not precise (nothing to classify)");
    check(!wheelDeltaIsPrecise(1.0f) && !wheelDeltaIsPrecise(-1.0f) &&
              !wheelDeltaIsPrecise(2.0f) && !wheelDeltaIsPrecise(-5.0f),
          "precise: whole-number deltas (what SDL rounds a conventional wheel to) "
          "read as a notch, not precise");
    check(wheelDeltaIsPrecise(0.1f) && wheelDeltaIsPrecise(-0.35f) && wheelDeltaIsPrecise(1.7f) &&
              wheelDeltaIsPrecise(0.03f),
          "precise: fractional deltas (what a trackpad/Magic Mouse actually sends) "
          "read as precise");
    // Just inside vs. just outside kWheelNotchEpsilon of a whole number --
    // the boundary the numerical-precision tolerance actually draws.
    check(!wheelDeltaIsPrecise(3.0f + kWheelNotchEpsilon * 0.5f),
          "precise: a delta within epsilon of a whole number is still a notch "
          "(float round-trip slop, not a real fractional sample)");
    check(wheelDeltaIsPrecise(3.0f + kWheelNotchEpsilon * 5.0f),
          "precise: comfortably past epsilon reads as precise");
  }

  // ==========================================================================
  // (b) wheelScrollPixels(): the required property -- "a precise-device
  // delta produces proportionally less scroll than a notch" -- checked as
  // the actual numeric comparison, at the SAME nominal delta magnitude, not
  // merely as a smaller total (which a smaller magnitude alone would also
  // produce and would prove nothing about the discount itself).
  // ==========================================================================
  {
    constexpr float kStep = 225.0f;  // controlsWheelScrollStep(900, 13) -- a real value
    check(near(wheelScrollPixels(1.0f, kStep), kStep, 1e-4f),
          "scroll: a whole-number (notch) delta of 1.0 gets the FULL step, unchanged "
          "from what this replaces");
    check(near(wheelScrollPixels(2.0f, kStep), 2.0f * kStep, 1e-4f),
          "scroll: a two-notch delta gets exactly twice the step (linear in the notch case)");
    const float precise = wheelScrollPixels(1.0f + kWheelNotchEpsilon * 5.0f, kStep);
    check(precise > 0.0f && precise < kStep,
          "scroll: a precise delta at essentially the SAME magnitude as one notch "
          "produces strictly LESS pixels than the notch does -- the required "
          "discount, not just a side effect of a smaller input");
    check(near(precise, (1.0f + kWheelNotchEpsilon * 5.0f) * kStep * kPreciseScrollFraction,
               1e-3f),
          "scroll: the precise case is exactly delta * step * kPreciseScrollFraction");
    check(wheelScrollPixels(0.0f, kStep) == 0.0f, "scroll: zero delta scrolls nothing");
    check(wheelScrollPixels(-1.0f, kStep) == -wheelScrollPixels(1.0f, kStep),
          "scroll: sign follows the delta (scrolling the other way undoes the same amount)");
    check(kPreciseScrollFraction > 0.0f && kPreciseScrollFraction < 1.0f,
          "scroll: the precise fraction is a genuine discount (strictly between 0 and 1), "
          "not accidentally 1.0 (no discount) or 0.0 (a dead trackpad)");
  }

  // ==========================================================================
  // (c) smoothedScrollStep(): convergence. The property that matters is not
  // any one frame's number but that repeated frames drive the remainder to
  // zero WITHOUT crossing it (no oscillation) and without ever applying more
  // than what remained (no overshoot) -- checked by actually iterating the
  // recurrence, the same "run it and look" the rest of this suite already
  // does for FixedStep-style integrators, not by inspecting the formula.
  // ==========================================================================
  {
    float pending = 500.0f;
    const float dt = 1.0f / 60.0f;  // a typical frame time
    float totalApplied = 0.0f;
    bool everFlippedSign = false;
    bool everGrew = false;
    float prevAbs = std::fabs(pending);
    int frames = 0;
    // 300 frames at 60fps is 5s -- generously past this build's own
    // kScrollSmoothingTauSeconds (well under 0.1s), so failing to have
    // converged by then is a real defect, not an impatient test.
    while (std::fabs(pending) > 1e-3f && frames < 300) {
      const SmoothedStep s = smoothedScrollStep(pending, dt);
      totalApplied += s.appliedPx;
      const float curAbs = std::fabs(s.remainingPx);
      if (curAbs > prevAbs) everGrew = true;
      if ((pending > 0.0f && s.remainingPx < 0.0f) || (pending < 0.0f && s.remainingPx > 0.0f))
        everFlippedSign = true;
      prevAbs = curAbs;
      pending = s.remainingPx;
      ++frames;
    }
    check(frames < 300, "smoothing: converges to within 1e-3 px in well under 300 frames "
                        "(5s at 60fps) from a 500px pool");
    check(!everGrew, "smoothing: the remainder's magnitude never grows frame over frame "
                     "-- strictly non-increasing, which is what makes it converge");
    check(!everFlippedSign, "smoothing: the remainder never crosses zero and comes back "
                            "-- no oscillation");
    check(near(totalApplied, 500.0f, 0.5f),
          "smoothing: every applied fraction sums back to (essentially) the original "
          "pool -- smoothing redistributes IN TIME, it does not lose or invent pixels");
    std::printf("  smoothing: 500px pool settled in %d frames [measured]\n", frames);
  }
  {
    // Degenerate dt: must not stall. A zero or negative dt applies the whole
    // remainder at once rather than freezing the scroll position forever,
    // the same "never silently stall" rule controlsWheelScrollStep() already
    // follows for a degenerate window height.
    const SmoothedStep zero = smoothedScrollStep(120.0f, 0.0f);
    const SmoothedStep neg = smoothedScrollStep(120.0f, -1.0f);
    check(near(zero.appliedPx, 120.0f, 1e-6f) && zero.remainingPx == 0.0f,
          "smoothing: a zero dt applies the whole pool immediately rather than stalling");
    check(near(neg.appliedPx, 120.0f, 1e-6f) && neg.remainingPx == 0.0f,
          "smoothing: a negative dt (should never happen, but is not asserted against) "
          "does the same");
    const SmoothedStep zeroPending = smoothedScrollStep(0.0f, 1.0f / 60.0f);
    check(zeroPending.appliedPx == 0.0f && zeroPending.remainingPx == 0.0f,
          "smoothing: an empty pool stays empty");
  }

  // ==========================================================================
  // (d) zoomFactorForPinch(): Apple's own documented contract for
  // NSEvent.magnification -- "1.0 + magnification", transcribed rather than
  // invented.
  // ==========================================================================
  {
    check(near(zoomFactorForPinch(0.0f), 1.0f, 1e-6f),
          "pinch: zero magnification is a no-op factor (exactly 1.0)");
    check(near(zoomFactorForPinch(0.1f), 1.1f, 1e-6f),
          "pinch: a pinch-open sample of 0.1 gives a 1.1x factor (hand-computed)");
    check(near(zoomFactorForPinch(-0.2f), 0.8f, 1e-6f),
          "pinch: a pinch-close sample of -0.2 gives a 0.8x factor");
  }

  // ==========================================================================
  // (e) The zoom anchor property, for the PINCH path specifically: a spread
  // of magnifications, each composed with panForAnchoredZoom (the SAME
  // function ui/MacPaintUI.cpp's applyZoomFactor calls for every zoom
  // trigger), must keep the anchored document point fixed. This is not a
  // re-derivation of app/selftest/ZoomAndSize.cpp's own generic proof of
  // panForAnchoredZoom -- it is the specific claim that the PINCH gesture's
  // number reaches that function correctly.
  // ==========================================================================
  {
    const float paintOrigin = 10.0f, avail = 640.0f, tex = 320.0f;
    float zoom = 1.0f, panX = 0.0f;
    const float anchor = 220.0f;
    const float magnifications[] = {0.05f, 0.05f, -0.03f, 0.2f, -0.5f};
    bool anchorHeld = true;
    for (const float m : magnifications) {
      const float marginOld = std::max(0.0f, (avail - tex * zoom) * 0.5f);
      const float originOld = paintOrigin + marginOld + panX;
      const float canvasPtBefore = (anchor - originOld) / zoom;

      const float factor = zoomFactorForPinch(m);
      const float newZoom = clampViewZoom(zoom * factor);
      if (newZoom == zoom) continue;
      panX = panForAnchoredZoom(anchor, originOld, zoom, newZoom, paintOrigin, avail, tex);
      zoom = newZoom;

      const float marginNew = std::max(0.0f, (avail - tex * zoom) * 0.5f);
      const float originNew = paintOrigin + marginNew + panX;
      const float canvasPtAfter = (anchor - originNew) / zoom;
      if (!near(canvasPtAfter, canvasPtBefore, 1e-2f)) anchorHeld = false;
    }
    check(anchorHeld, "pinch zoom: a five-sample pinch gesture (open, open, close, open, "
                      "close) keeps the anchored document point fixed at every step, "
                      "through the SAME panForAnchoredZoom every other zoom trigger uses");
    check(zoom != 1.0f, "pinch zoom: the gesture actually changed the view (not a vacuous "
                        "pass because every sample happened to clamp away)");
  }

  // ==========================================================================
  // (f) canvasPanForPreciseWheel(): kCanvasPanSpeedFactor times the wheel
  // sample (track11/pan-rotate-reset's "trackpad panning is too slow" fix --
  // this function's own header comment carries the full derivation of why
  // that factor, not a units correction, is what changed), and -- the
  // property that still matters exactly as before -- NOT run through
  // kPreciseScrollFraction's discount. Unlike the panel scroll, a precise
  // wheel sample over the canvas is not competing against a "full notch"
  // step (a notch over the canvas zooms; see (d)/(e) above), so there is
  // nothing for it to be a fraction of.
  // ==========================================================================
  {
    const CanvasPanDelta d = canvasPanForPreciseWheel(3.0f, -2.0f);
    check(near(d.dx, 3.0f * kCanvasPanSpeedFactor, 1e-4f) &&
              near(d.dy, -2.0f * kCanvasPanSpeedFactor, 1e-4f),
          "canvas pan: kCanvasPanSpeedFactor times the wheel sample, hand-computed");
    check(kCanvasPanSpeedFactor > 1.0f,
          "canvas pan: the speed factor is a genuine SPEED-UP (> 1.0), not accidentally "
          "1.0 (no change from before this track) or < 1.0 (a further slowdown, the "
          "opposite of this track's brief)");
    check(!near(canvasPanForPreciseWheel(1.0f, 0.0f).dx, kPreciseScrollFraction, 1e-6f),
          "canvas pan: NOT discounted by kPreciseScrollFraction -- that constant is "
          "specific to competing against a panel's notch-sized step, which canvas "
          "pan never does");
    check(near(canvasPanForPreciseWheel(0.0f, 0.0f).dx, 0.0f, 1e-6f) &&
              near(canvasPanForPreciseWheel(0.0f, 0.0f).dy, 0.0f, 1e-6f),
          "canvas pan: a zero sample on both axes is a genuine no-op");
    // Sign follows the delta on both axes, independently -- a diagonal
    // trackpad swipe must not have one axis silently reflect the other.
    check(near(canvasPanForPreciseWheel(-5.0f, 7.0f).dx, -5.0f * kCanvasPanSpeedFactor, 1e-4f) &&
              near(canvasPanForPreciseWheel(-5.0f, 7.0f).dy, 7.0f * kCanvasPanSpeedFactor, 1e-4f),
          "canvas pan: sign follows each axis independently");
    // The "fixed screen-pixel factor, not scaled by zoom" property this
    // function's header comment argues for -- the SAME wheel sample must
    // produce the SAME pan delta regardless of what st.view.zoom happens to
    // be, since `canvasPanForPreciseWheel()` does not even take zoom as a
    // parameter. Asserted anyway, explicitly, so a future edit that tried to
    // thread a zoom argument through and scale by it would have to change
    // this test on purpose rather than by accident.
    const CanvasPanDelta atOneSample = canvasPanForPreciseWheel(4.0f, -1.5f);
    for (const float pretendZoom : {0.1f, 1.0f, 8.0f}) {
      (void)pretendZoom;  // canvasPanForPreciseWheel has no zoom parameter to vary
      const CanvasPanDelta again = canvasPanForPreciseWheel(4.0f, -1.5f);
      check(near(again.dx, atOneSample.dx, 1e-6f) && near(again.dy, atOneSample.dy, 1e-6f),
            "canvas pan: the SAME wheel sample gives the SAME screen-pixel pan delta "
            "every time -- zoom-independent, as ui/MacPaintUI.cpp's own screen-space "
            "panX/panY composition requires");
    }
  }

  // ==========================================================================
  // (g) canvasRotationRadiansForTrackpad(): sign (this function's header
  // comment has the full derivation -- NSEvent.rotation counterclockwise-
  // positive, CanvasView::rotation clockwise-positive on screen, so the
  // mapping negates) and a zero-sample no-op.
  // ==========================================================================
  {
    check(near(canvasRotationRadiansForTrackpad(0.0f), 0.0f, 1e-6f),
          "trackpad rotate: a zero-degree sample is a genuine no-op");
    // 90 degrees counterclockwise (NSEvent.rotation == +90) must DECREASE
    // view.rotation (clockwise-positive) by pi/2 -- hand-computed, not just
    // "some negative number".
    const float kPiHalf = 1.5707963267948966f;
    check(near(canvasRotationRadiansForTrackpad(90.0f), -kPiHalf, 1e-4f),
          "trackpad rotate: +90 degrees (counterclockwise fingers) maps to -pi/2 "
          "(view.rotation moves counterclockwise on screen -- hand-computed)");
    check(near(canvasRotationRadiansForTrackpad(-45.0f), kPiHalf * 0.5f, 1e-4f),
          "trackpad rotate: -45 degrees (clockwise fingers) maps to +pi/4 (view.rotation "
          "moves clockwise on screen, the same sense the R+drag gesture already uses)");
    // Monotonic and sign-reversing: a bigger counterclockwise finger twist
    // must not produce a smaller (or same-signed) view.rotation change.
    check(canvasRotationRadiansForTrackpad(30.0f) < canvasRotationRadiansForTrackpad(10.0f) &&
              canvasRotationRadiansForTrackpad(10.0f) < canvasRotationRadiansForTrackpad(0.0f) &&
              canvasRotationRadiansForTrackpad(0.0f) < canvasRotationRadiansForTrackpad(-10.0f),
          "trackpad rotate: strictly monotonically DEcreasing in the input degrees -- "
          "the negation holds across the whole range, not just at the two hand-computed "
          "points above");
  }

  // ==========================================================================
  // (h) wrapRotationRadians(): the canonical range is (-pi, pi], approached
  // from both directions, plus the in-range no-op case.
  // ==========================================================================
  {
    const float kPi = 3.14159265358979323846f;
    check(near(wrapRotationRadians(0.0f), 0.0f, 1e-4f),
          "wrap: zero is already canonical and stays zero");
    check(near(wrapRotationRadians(kPi * 0.5f), kPi * 0.5f, 1e-4f),
          "wrap: a value already inside (-pi, pi] passes through unchanged");
    check(near(wrapRotationRadians(kPi), kPi, 1e-3f),
          "wrap: +pi itself is canonical (the range's own closed end) and stays +pi");
    check(near(wrapRotationRadians(-kPi), kPi, 1e-3f),
          "wrap: -pi (excluded from the range) wraps to its equivalent +pi, not left as -pi");
    check(near(wrapRotationRadians(kPi + 0.1f), -kPi + 0.1f, 1e-3f),
          "wrap: just past +pi wraps around to just past -pi (positive overflow)");
    check(near(wrapRotationRadians(-kPi - 0.1f), kPi - 0.1f, 1e-3f),
          "wrap: just past -pi wraps around to just past +pi (negative overflow)");
    check(near(wrapRotationRadians(5.0f * kPi + 0.2f), -kPi + 0.2f, 1e-3f),
          "wrap: several full turns past +pi still lands in (-pi, pi], not merely one "
          "turn's worth -- the fmod-based implementation must not assume a single wrap "
          "is ever enough, which a naive one-shot +=/-= kTwoPi would");
    // Every wrapped output actually lands in the canonical range, swept
    // across a spread of inputs rather than a few hand-picked points.
    bool allInRange = true;
    for (float r = -20.0f; r <= 20.0f; r += 0.37f) {
      const float w = wrapRotationRadians(r);
      if (!(w > -kPi - 1e-3f && w <= kPi + 1e-3f)) allInRange = false;
    }
    check(allInRange, "wrap: every output across a wide sweep of inputs lands in "
                      "(-pi, pi], not merely the hand-picked cases above");
  }

  // --- Smoothing must actually SMOOTH -----------------------------------
  //
  // **Added because a sabotage that removed smoothing entirely left this
  // whole suite green.** Replacing `smoothedScrollStep()`'s body with
  // `return {pendingPx, 0.0f}` -- deliver the lot this frame, no smoothing
  // at all, exactly the choppiness the function exists to remove -- reddened
  // nothing. Every convergence property already asserted here holds
  // trivially for an instant drain: the remainder is 0, which is
  // non-increasing, never flips sign, and conserves mass perfectly.
  //
  // Those assertions are not wrong, they are just satisfiable two ways, and
  // the wrong way is the bug. What separates them is the one thing none of
  // them said out loud: a single pending amount must be delivered over
  // SEVERAL frames, so one frame's step is a strict fraction of what is
  // pending.
  {
    // 1/60 s at tau = 0.0813 s gives factor = 1 - exp(-0.205) ~= 0.185, so a
    // 100 px pending scroll should move ~18.5 px this frame and keep ~81.5.
    // Asserted as a BAND rather than a fitted constant: the claim is "a
    // sensible minority of the remainder", which is what makes it smoothing
    // rather than either a jump or a freeze. The band is wide enough that
    // retuning tau within any reasonable range keeps it green, and narrow
    // enough that both degenerate answers (1.0 and 0.0) fall outside.
    const SmoothedStep s = smoothedScrollStep(100.0f, 1.0f / 60.0f);
    const float fraction = s.appliedPx / 100.0f;
    check(fraction > 0.01f && fraction < 0.5f,
          "smoothing: one frame at 60 Hz delivers a strict MINORITY of what is pending -- not "
          "the whole thing (which is the unsmoothed jump this function replaces) and not "
          "nothing (which would freeze the panel)");
    check(near(s.appliedPx + s.remainingPx, 100.0f, 1e-3f) && s.remainingPx > 50.0f,
          "smoothing: and the rest is CARRIED, not dropped -- the majority of a 100 px scroll "
          "is still owed after one frame, which is the state an instant drain does not have");

    // Several frames must be needed to substantially finish. An instant drain
    // reaches ~0 remainder after one; this must not.
    float pending = 100.0f;
    int frames = 0;
    while (pending > 1.0f && frames < 1000) {
      pending = smoothedScrollStep(pending, 1.0f / 60.0f).remainingPx;
      ++frames;
    }
    check(frames > 3 && frames < 200,
          "smoothing: a 100 px scroll takes MANY frames to drain to under a pixel -- more than "
          "a handful, so it is visibly eased, and bounded, so it cannot crawl forever");
  }

  std::printf("[selftest] wheel input %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
