#include "app/selftest/Support.hpp"

#include "brush/StrokePath.hpp"

namespace np {
namespace {

// Feeds `n` samples along the straight line (x0,y) -> (x1,y) and flushes,
// returning every dab position the emitter produced. `n` stands in for stroke
// speed at a fixed sampling rate -- few samples over this distance is a fast
// stroke, many is a slow one -- which is what makes the coarse/fine pair below
// a speed-independence test and not merely a repeat.
std::vector<Vec2> walkLine(float x0, float x1, float y, int n, float spacingPx) {
  StrokePath path;
  path.reset();
  std::vector<Vec2> dabs;
  for (int i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(n - 1);
    path.addPoint(x0 + (x1 - x0) * t, y, spacingPx, dabs);
  }
  path.flush(spacingPx, dabs);
  return dabs;
}

bool closeTo(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

}  // namespace

// brush/StrokePath, driven directly rather than through a StrokeSession: the
// arc-length dab emitter is pure geometry, so the whole of it can be asserted
// with no GPU, no document and no PaintSim. `runStrokeSpeedTest()` is the
// closest existing coverage and it needs both -- it measures a real solver's
// throughput, not the emitter's output positions -- so nothing before this
// section ever looked at the dab stream itself.
//
// What it exists for: **a single click must lay one dab.** Before this change
// `flush()` returned immediately on a stroke with fewer than two samples, and
// a stroke with several coincident samples walked a zero-length curve, so
// every form of "click without dragging" deposited nothing at all. The other
// half of that requirement is that DRAGS DO NOT MOVE, which is why the
// moving-stroke assertions below check explicit dab coordinates rather than
// counting: a count alone cannot tell a preserved drag from one that gained an
// origin dab, and gaining an origin dab (seeding `leftover_ = spacingPx`) is
// the plausible over-reach this fix has to be pinned against.
bool runStrokePathTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] stroke path: a click lays one dab, and drags do not move\n");

  // ======================================================================
  // 1. The click: one sample, one dab, at the sample's own coordinates
  // ======================================================================
  {
    StrokePath path;
    path.reset();
    std::vector<Vec2> dabs;
    path.addPoint(37.5f, 91.25f, 10.0f, dabs);
    check(dabs.empty(), "click: addPoint() alone emits nothing (the walk still lags)");
    path.flush(10.0f, dabs);
    check(dabs.size() == 1, "click: one sample + flush emits exactly ONE dab");
    // Deliberately exact, not a tolerance: this dab is a copy of the sample,
    // not the result of any interpolation, so anything but bit equality means
    // the position came from somewhere else (a curve evaluation, an origin
    // constant) and the click is landing in the wrong place.
    check(dabs.size() == 1 && dabs[0].x == 37.5f && dabs[0].y == 91.25f,
          "click: the dab is at the sample's OWN coordinates, exactly");
  }

  // ======================================================================
  // 2. The click is spacing-independent -- it is not a walk with a small
  //    divisor. A spacing far larger than any distance in the stroke would
  //    starve any arc-length rule; this gesture has no arc length to divide.
  // ======================================================================
  {
    for (const float spacingPx : {0.5f, 10.0f, 5000.0f}) {
      StrokePath path;
      path.reset();
      std::vector<Vec2> dabs;
      path.addPoint(200.0f, 300.0f, spacingPx, dabs);
      path.flush(spacingPx, dabs);
      const bool one = dabs.size() == 1 && dabs[0].x == 200.0f && dabs[0].y == 300.0f;
      check(one, spacingPx > 1000.0f
                     ? "click: one dab even at a spacing larger than the canvas"
                     : (spacingPx < 1.0f ? "click: one dab at a sub-pixel spacing"
                                         : "click: one dab at an ordinary spacing"));
    }
  }

  // ======================================================================
  // 3. A HELD click -- what a real click actually looks like. Both live
  //    routes in ui/MacPaintUI.cpp call addPoint() once per render frame for
  //    as long as the button is down, with no moved-since-last-frame guard,
  //    so a 50 ms click arrives as several IDENTICAL samples. This is the
  //    case a `numPts_ == 1` fix would have missed entirely.
  //
  //    It is also ADR-0003 restated for the click: 3 frames and 300 frames
  //    at one position must deposit the same amount, because deposition
  //    depends on distance travelled and neither of them travelled.
  // ======================================================================
  {
    for (const int frames : {2, 3, 30, 300}) {
      StrokePath path;
      path.reset();
      std::vector<Vec2> dabs;
      for (int i = 0; i < frames; ++i) path.addPoint(64.0f, 128.0f, 8.0f, dabs);
      check(dabs.empty(), "held click: no dab is emitted while the button is still down");
      path.flush(8.0f, dabs);
      check(dabs.size() == 1 && dabs[0].x == 64.0f && dabs[0].y == 128.0f,
            "held click: N identical samples still emit exactly one dab, at that point");
    }
  }

  // ======================================================================
  // 4. Zero samples: still nothing. The documented 0-sample contract, which
  //    the click fix must not turn into a dab out of nowhere -- a stroke the
  //    router refused, or a pen-up with no pen-down, ends this way.
  // ======================================================================
  {
    StrokePath path;
    path.reset();
    std::vector<Vec2> dabs;
    path.flush(10.0f, dabs);
    check(dabs.empty(), "no samples: flush() on an empty path emits nothing");
  }

  // ======================================================================
  // 5. THE MOVING STROKE, UNCHANGED. Five samples 20 px apart along y = 100,
  //    spacing 12 px. Collinear input keeps the extrapolated control points
  //    collinear (StrokePath.cpp's own `mirror()` comment), and the samples
  //    are evenly spaced, so the centripetal knots are uniform and the curve
  //    degenerates to the straight line with a linear parameterisation. The
  //    dab positions are therefore derivable by hand rather than recorded
  //    from the code's own output:
  //
  //      leftover_ starts at 0, so the first dab lands one FULL spacing in,
  //      at x = 12 -- x = 0, the stroke's origin texel, is never stamped --
  //      and every subsequent dab follows 12 px later: 12, 24, 36, 48, 60,
  //      72. The seventh would be at 84, past the path's 80 px, so flush()
  //      ends the stroke with 6 dabs and 8 px of unspent leftover.
  //
  //    Asserting those six numbers is what makes this section able to fail if
  //    the click fix reaches a drag. A count alone could not: seeding
  //    `leftover_ = spacingPx` (the "make every stroke stamp its origin"
  //    variant that was deliberately NOT implemented) yields 7 dabs starting
  //    at x = 0, and a stray click dab appended at flush() yields 7 ending at
  //    x = 80 -- both of which move these coordinates.
  // ======================================================================
  {
    const std::vector<Vec2> dabs = walkLine(0.0f, 80.0f, 100.0f, 5, 12.0f);
    check(dabs.size() == 6, "drag: 5 samples over 80 px at spacing 12 emit exactly 6 dabs");

    const float expectX[6] = {12.0f, 24.0f, 36.0f, 48.0f, 60.0f, 72.0f};
    bool positionsOk = dabs.size() == 6;
    for (size_t i = 0; i < dabs.size() && i < 6; ++i) {
      // 0.01 px: the walk is a 24-segment piecewise-linear approximation with
      // float accumulation, so the answer is not bit-exact, but it is two
      // orders of magnitude tighter than the 12 px spacing an off-by-one
      // seeding error would shift everything by.
      if (!closeTo(dabs[i].x, expectX[i], 0.01f) || !closeTo(dabs[i].y, 100.0f, 0.01f)) {
        positionsOk = false;
      }
    }
    check(positionsOk, "drag: every dab is at its derived position: 12,24,36,48,60,72");

    // Stated separately from the sweep above so the failure line names the
    // specific over-reach rather than "some coordinate moved". This is the
    // assertion that a leftover_ = spacingPx seed has to trip.
    check(!dabs.empty() && closeTo(dabs[0].x, 12.0f, 0.01f),
          "drag: the FIRST dab is one full spacing in, not at the origin");
    check(!dabs.empty() && closeTo(dabs.back().x, 72.0f, 0.01f),
          "drag: the LAST dab is 72, i.e. no click dab was appended at the end");
  }

  // ======================================================================
  // 6. Speed independence across the click fix (ADR-0003). The same 80 px
  //    line fed as 5 coarse samples and as 41 fine ones -- a fast stroke and
  //    a slow one at the same sampling rate -- must lay dabs at the same
  //    places. This is the property `leftover_`'s carry across addPoint()
  //    calls exists for, and a click fix that touched the carry would break
  //    it here rather than anywhere a user would notice.
  // ======================================================================
  {
    const std::vector<Vec2> coarse = walkLine(0.0f, 80.0f, 100.0f, 5, 12.0f);
    const std::vector<Vec2> fine = walkLine(0.0f, 80.0f, 100.0f, 41, 12.0f);
    check(coarse.size() == fine.size(),
          "speed: 5 samples and 41 over the same line emit the same dab COUNT");
    bool sameSpots = coarse.size() == fine.size();
    for (size_t i = 0; i < coarse.size() && i < fine.size(); ++i) {
      if (!closeTo(coarse[i].x, fine[i].x, 0.05f) || !closeTo(coarse[i].y, fine[i].y, 0.05f)) {
        sameSpots = false;
      }
    }
    check(sameSpots, "speed: and at the same POSITIONS, dab for dab");
  }

  // ======================================================================
  // 7. flush() is still idempotent, and reset() still clears the click.
  //    The click branch is a second early return out of flush(); an early
  //    return that forgot to clear the point history would let a pen-up
  //    called twice (interrupted stroke, window blur mid-click) stamp twice.
  // ======================================================================
  {
    StrokePath path;
    path.reset();
    std::vector<Vec2> dabs;
    path.addPoint(10.0f, 20.0f, 6.0f, dabs);
    path.flush(6.0f, dabs);
    const size_t afterFirst = dabs.size();
    path.flush(6.0f, dabs);
    check(afterFirst == 1 && dabs.size() == 1,
          "click: a second flush() is a no-op -- the click cannot stamp twice");

    // Same for a drag, which reaches the OTHER early return.
    StrokePath drag;
    drag.reset();
    std::vector<Vec2> dragDabs;
    for (int i = 0; i < 5; ++i) drag.addPoint(20.0f * static_cast<float>(i), 50.0f, 12.0f, dragDabs);
    drag.flush(12.0f, dragDabs);
    const size_t dragCount = dragDabs.size();
    drag.flush(12.0f, dragDabs);
    check(dragCount == 6 && dragDabs.size() == dragCount,
          "drag: a second flush() is a no-op too");

    // reset() then flush(): a stroke abandoned rather than ended (the router
    // refusing it, a layer deleted mid-stroke) must not leave a click behind
    // for the next pen-up to emit.
    StrokePath abandoned;
    abandoned.reset();
    std::vector<Vec2> none;
    abandoned.addPoint(300.0f, 400.0f, 6.0f, none);
    abandoned.reset();
    abandoned.flush(6.0f, none);
    check(none.empty(), "click: reset() drops the pending click -- flush() after it emits nothing");
  }

  // ======================================================================
  // 8. The boundary between the two. A stroke that moved by any real amount
  //    is a drag and takes the drag path, even when the drag is shorter than
  //    one spacing and therefore still deposits nothing -- that is today's
  //    behaviour for a short drag and this change does not alter it. Stated
  //    as an assertion rather than left implicit because it is the exact
  //    edge a "slop budget" version of this fix would have moved, and a
  //    reader deciding to add one later should see it fail.
  // ======================================================================
  {
    StrokePath path;
    path.reset();
    std::vector<Vec2> dabs;
    for (int i = 0; i < 4; ++i) {
      path.addPoint(50.0f + 0.5f * static_cast<float>(i), 50.0f, 40.0f, dabs);
    }
    path.flush(40.0f, dabs);
    check(dabs.empty(),
          "boundary: a 1.5 px drag at spacing 40 is a DRAG and still emits nothing");
  }

  std::printf("[selftest] stroke-path %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
