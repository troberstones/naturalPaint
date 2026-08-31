#include "app/selftest/Support.hpp"

#include "app/TouchGesture.hpp"

namespace np {

// item 4 ("trackpad interactions feel unnatural"): app/TouchGesture.hpp's
// pure half of the raw two-finger capture that replaces AppKit's own
// magnify/rotate gesture classifier for the canvas (see that header's own
// comment for the documented platform reason it alternates between the two
// mid-gesture, and why bypassing it needs raw NSTouch positions instead).
// `--selftest` cannot reach an NSTouch/AppKit dispatch site
// (docs/reachability-audit.md F4), so this exercises the geometry alone --
// every point below is a plain, dimensionless (x, y) pair, no AppKit
// involved.
bool runTouchGestureTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  std::printf("[selftest] touch gesture: two-touch pan+zoom+rotate geometry, no AppKit\n");

  // ==========================================================================
  // (a) No motion at all: identity delta.
  // ==========================================================================
  {
    const TrackpadTouchPoint a{1, 0.2f, 0.3f};
    const TrackpadTouchPoint b{2, 0.6f, 0.5f};
    const TwoTouchDelta d = computeTwoTouchDelta(a, b, a, b);
    check(near(d.scale, 1.0f, 1e-6f) && near(d.rotationRadians, 0.0f, 1e-6f) &&
              near(d.panDx, 0.0f, 1e-6f) && near(d.panDy, 0.0f, 1e-6f),
          "no motion: two touches unchanged from start is exactly the identity delta");
  }

  // ==========================================================================
  // (b) Pure zoom: touches move directly apart along the same line, midpoint
  // fixed. scale is the exact distance ratio; rotation and pan are both
  // zero -- a bug that coupled zoom into rotation or pan would show up here
  // specifically, since this case changes only ONE of the three quantities.
  // ==========================================================================
  {
    const TrackpadTouchPoint startA{1, 0.3f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.7f, 0.5f};  // 0.4 apart, midpoint (0.5, 0.5)
    const TrackpadTouchPoint curA{1, 0.2f, 0.5f};
    const TrackpadTouchPoint curB{2, 0.8f, 0.5f};  // 0.6 apart, SAME midpoint
    const TwoTouchDelta d = computeTwoTouchDelta(startA, startB, curA, curB);
    check(near(d.scale, 1.5f, 1e-4f), "pure zoom: scale is the exact distance ratio (0.6/0.4)");
    check(near(d.rotationRadians, 0.0f, 1e-4f) && near(d.panDx, 0.0f, 1e-4f) &&
              near(d.panDy, 0.0f, 1e-4f),
          "pure zoom: rotation and pan both stay exactly zero -- not coupled into a change "
          "that was only ever a distance change");
  }

  // ==========================================================================
  // (c) Pure rotate: touches swing 90 degrees about their shared, FIXED
  // midpoint -- distance apart and midpoint both unchanged, so scale and pan
  // must stay at their no-op values and only rotation may move.
  //
  // A at (0.4, 0.5), B at (0.6, 0.5): B is 0.1 to the RIGHT of the midpoint
  // (0.5, 0.5). After a 90-degree CLOCKWISE twist (this app's own y-down,
  // clockwise-positive convention -- app/WheelInput.hpp's own derivation),
  // "right of centre" sweeps to "below centre": B moves to (0.5, 0.6), A
  // (diametrically opposite) to (0.5, 0.4).
  // ==========================================================================
  {
    const TrackpadTouchPoint startA{1, 0.4f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.6f, 0.5f};
    const TrackpadTouchPoint curA{1, 0.5f, 0.4f};
    const TrackpadTouchPoint curB{2, 0.5f, 0.6f};
    const TwoTouchDelta d = computeTwoTouchDelta(startA, startB, curA, curB);
    check(near(d.scale, 1.0f, 1e-4f) && near(d.panDx, 0.0f, 1e-4f) && near(d.panDy, 0.0f, 1e-4f),
          "pure rotate: scale stays 1.0 and pan stays zero -- distance and midpoint both "
          "genuinely unchanged");
    check(near(d.rotationRadians, 1.5707963267948966f, 1e-3f),
          "pure rotate: +90 degrees (+pi/2) for a clockwise swing, matching "
          "app/WheelInput.hpp's own clockwise-positive view.rotation convention -- a sign "
          "regression here would silently reverse every trackpad-driven rotate");
  }

  // ==========================================================================
  // (d) Pure pan: both touches translate identically. Distance and relative
  // angle both unchanged (rigid translation), so scale and rotation stay at
  // their no-op values; pan is the exact shared translation.
  // ==========================================================================
  {
    const TrackpadTouchPoint startA{1, 0.3f, 0.3f};
    const TrackpadTouchPoint startB{2, 0.5f, 0.4f};
    const TrackpadTouchPoint curA{1, 0.1f, 0.5f};   // both shifted by (-0.2, +0.2)
    const TrackpadTouchPoint curB{2, 0.3f, 0.6f};
    const TwoTouchDelta d = computeTwoTouchDelta(startA, startB, curA, curB);
    check(near(d.scale, 1.0f, 1e-4f) && near(d.rotationRadians, 0.0f, 1e-4f),
          "pure pan: scale and rotation stay at their no-op values for a rigid translation");
    check(near(d.panDx, -0.2f, 1e-4f) && near(d.panDy, 0.2f, 1e-4f),
          "pure pan: the shared midpoint's own motion, exactly (-0.2, +0.2)");
  }

  // ==========================================================================
  // (e) All three at once -- confirms the three quantities really are
  // independent (each is a different geometric measurement of the same two
  // points: distance ratio, angle change, midpoint shift), not merely
  // individually correct in isolation the way (b)/(c)/(d) each prove alone.
  // Start: A(0.4,0.5), B(0.6,0.5) -- 0.2 apart, midpoint (0.5,0.5). Current:
  // scaled 2x, rotated +90 clockwise, THEN the whole pair panned by
  // (0.1,-0.05): the rotated-and-scaled pair has A at (0.5,0.4), B at
  // (0.5,0.6) (0.4 apart, same derivation as (c) but at 2x distance -- wait,
  // 2x of 0.2 apart is 0.4 apart, i.e. 0.2 off-centre each, so A=(0.5,0.3),
  // B=(0.5,0.7)); panned: A=(0.6,0.25), B=(0.6,0.65).
  // ==========================================================================
  {
    const TrackpadTouchPoint startA{1, 0.4f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.6f, 0.5f};
    const TrackpadTouchPoint curA{1, 0.6f, 0.25f};
    const TrackpadTouchPoint curB{2, 0.6f, 0.65f};
    const TwoTouchDelta d = computeTwoTouchDelta(startA, startB, curA, curB);
    check(near(d.scale, 2.0f, 1e-3f), "combined: scale is 2.0, unaffected by the rotate+pan "
                                      "riding along with it");
    check(near(d.rotationRadians, 1.5707963267948966f, 1e-3f),
          "combined: rotation is still exactly +pi/2, unaffected by the scale+pan");
    check(near(d.panDx, 0.1f, 1e-3f) && near(d.panDy, -0.05f, 1e-3f),
          "combined: pan is still the exact shared-midpoint shift, unaffected by the "
          "scale+rotate riding along with it");
  }

  // ==========================================================================
  // (f) Degenerate start: the two touches began at (almost) the same point,
  // so the start distance is unmeasurable. `scale` must be the no-op 1.0,
  // not a division blow-up (inf/NaN from dividing by ~0) -- the same posture
  // this codebase's other anchor math takes on an unmeasurable baseline.
  // ==========================================================================
  {
    const TrackpadTouchPoint startA{1, 0.5f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.5001f, 0.5f};  // 1e-4 apart -- below kMinTouchSeparation
    const TrackpadTouchPoint curA{1, 0.3f, 0.5f};
    const TrackpadTouchPoint curB{2, 0.7f, 0.5f};
    const TwoTouchDelta d = computeTwoTouchDelta(startA, startB, curA, curB);
    check(near(d.scale, 1.0f, 1e-6f) && std::isfinite(d.scale),
          "degenerate start: an unmeasurable starting distance falls back to scale == 1.0, "
          "not an inf/NaN from dividing by ~0");
  }

  return ok;
}

}  // namespace np
