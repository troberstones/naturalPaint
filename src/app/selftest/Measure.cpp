#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/AppState.hpp"
#include "app/MeasureLine.hpp"
#include "app/StrokeSession.hpp"
#include "app/ZoomAndSize.hpp"
#include "brush/Deposit.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The Measure tool (docs/ui.md section 2's `ruler` cell) -- app/MeasureLine.
//
// **The tool that writes nothing.** Every other built tool ends in a texel;
// this one ends in a sentence in the options bar. So there is no readback to
// assert on, no history entry to count and no tile to `memcmp` -- the whole
// of what this tool does is four numbers and a gate, and this file asserts on
// exactly those.
//
// **The method for the angle, which is the part worth being careful about.**
// `app/selftest/AngleConvention.cpp` exists because this build has been bitten
// by angle conventions, and its own opening states the rule this file obeys:
// *query the geometry, never read an angle number back out of the code under
// test*. A `measureReadout()` whose sign were mirrored, whose axes were
// swapped, or which were 90 degrees off would still produce a perfectly
// plausible DEGREE value, and an assertion of the form `angleDeg == 53.13`
// would pass on all three -- because 53.13 is what this file's own `atan2`
// says too, and both would be wrong together.
//
// So sections 3 and 4 below do what `AngleConvention.cpp` section 2 does:
// they take the angle `measureReadout()` reports, hand it to a `BrushTip` as
// `tip.angle`, and ask `dabCoverage()` -- an entirely separate piece of the
// build, with its own rotation and its own selftest pinning it -- whether that
// tip's elongated footprint actually reaches a point lying along the line that
// was measured. The query points are typed in by hand from the 3-4-5 unit
// vector (0.6, 0.8) and its rotations; not one of them is built from
// `cos(angleDeg)`. A wrong convention moves which query lands inside the
// footprint, which is a fact about geometry rather than about arithmetic.
//
// Headless and GPU-free: app/MeasureLine and brush/Deposit only.
// ---------------------------------------------------------------------------

namespace {

// The shape `app/selftest/AngleConvention.cpp` probes with, and deliberately
// the same numbers: elongated (roundness 0.2, minor semi-axis 5 px) so that a
// query 15 px off-centre is either comfortably inside the major-axis falloff
// or three times past the minor-axis rim -- never a near-miss whose result
// could be explained by the smoothstep's own float error. Restated here rather
// than shared because that file's constants are file-local to it, and a shared
// header for four floats would couple two sections that should be able to
// disagree.
constexpr float kRadius = 25.0f;
constexpr float kRoundness = 0.2f;
constexpr float kHardness = 0.3f;
constexpr float kProbeDist = 15.0f;
constexpr float kNonzeroFloor = 0.05f;

BrushTip tipAtAngle(float angleDeg) noexcept {
  BrushTip t;
  t.radius = kRadius;
  t.roundness = kRoundness;
  t.hardness = kHardness;
  t.angle = angleDeg;
  return t;
}

MeasureLine lineFrom(float x0, float y0, float x1, float y1) noexcept {
  MeasureLine l;
  l.active = true;
  l.documentId = 1u;
  l.x0 = x0;
  l.y0 = y0;
  l.x1 = x1;
  l.y1 = y1;
  return l;
}

}  // namespace

bool runMeasureTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // =========================================================================
  // 1. Length is a length: a 3-4-5 triangle measures EXACTLY 5
  // =========================================================================
  //
  // Zero tolerance, and that is affordable rather than brave: 3, 4, 25 and 5
  // are all exactly representable in binary32 and `std::sqrt` is correctly
  // rounded by IEEE754, so `sqrt(9 + 16)` is 5.0f on the nose. An assertion
  // with a tolerance here would pass for an implementation that had, say,
  // dropped a term and then been "close enough" on this particular triangle.
  {
    const MeasureReadout r = measureReadout(lineFrom(0.0f, 0.0f, 3.0f, 4.0f));
    std::printf("  [measured] 3-4-5 drag: dx=%.4f dy=%.4f len=%.6f angle=%.4f deg\n",
                static_cast<double>(r.dx), static_cast<double>(r.dy),
                static_cast<double>(r.lengthPx), static_cast<double>(r.angleDeg));
    check(r.lengthPx == 5.0f,
          "measure: a 3-4-5 drag is exactly 5 document texels long -- zero tolerance, "
          "because every value in sqrt(9+16) is exact in binary32");
    check(r.dx == 3.0f && r.dy == 4.0f,
          "measure: dx and dy are the signed run and rise the options bar shows as W and H");
  }

  // =========================================================================
  // 2. It measures the DIFFERENCE of the endpoints, not either endpoint
  // =========================================================================
  //
  // The same 3-4-5, moved off the origin to (10, 20). An implementation that
  // measured `hypot(x1, y1)` -- treating the far end as a vector from the
  // document's corner, which is what happens the first time someone forgets
  // the subtraction -- would answer 5 for section 1 and 27.7 here, so this is
  // the assertion section 1 cannot make on its own.
  //
  // And the axis-aligned pair, which pins the convention's ORIGIN: a drag due
  // east is 0 degrees, and a drag due SOUTH (screen-down, +y) is +90 rather
  // than 270. That single number is what separates this build's
  // clockwise-on-screen sense (app/selftest/AngleConvention.cpp section 1,
  // derived from +y being down in every raster here) from the mathematical
  // convention, and it is asserted as a number here because the geometric
  // proof in sections 3 and 4 below cannot distinguish an origin shift of 180
  // degrees on a symmetric ellipse.
  {
    const MeasureReadout moved = measureReadout(lineFrom(10.0f, 20.0f, 13.0f, 24.0f));
    check(moved.lengthPx == 5.0f && moved.dx == 3.0f && moved.dy == 4.0f,
          "measure: the same 3-4-5 taken from (10,20) still measures 5 -- the length is of "
          "the difference of the endpoints, not of the far endpoint");

    const MeasureReadout east = measureReadout(lineFrom(4.0f, 7.0f, 14.0f, 7.0f));
    const MeasureReadout south = measureReadout(lineFrom(4.0f, 7.0f, 4.0f, 17.0f));
    std::printf("  [measured] due east: %.4f deg   due south (+y, screen-down): %.4f deg\n",
                static_cast<double>(east.angleDeg), static_cast<double>(south.angleDeg));
    check(std::fabs(east.angleDeg - 0.0f) < 1e-3f && east.lengthPx == 10.0f,
          "measure: a drag due east is 0 degrees and 10 texels -- the convention's origin "
          "is the +x axis");
    check(std::fabs(south.angleDeg - 90.0f) < 1e-3f,
          "measure: a drag SCREEN-DOWN is +90, not 270 -- positive is clockwise on screen, "
          "because +y is down (AngleConvention.cpp section 1's own derivation)");
  }

  // =========================================================================
  // 3. The reported angle FACES the measured line -- asked of dabCoverage()
  // =========================================================================
  //
  // The 3-4-5 again, so the unit vector (0.6, 0.8) and its perpendicular
  // (-0.8, 0.6) are exact and hide no rounding. `measureReadout()` is the only
  // function under test in this section; every query offset below is typed in
  // from those two pairs and never from `cos(r.angleDeg)`.
  {
    const MeasureReadout r = measureReadout(lineFrom(0.0f, 0.0f, 3.0f, 4.0f));
    const BrushTip tip = tipAtAngle(r.angleDeg);

    const float along = dabCoverage(tip, 0.6f * kProbeDist, 0.8f * kProbeDist);
    std::printf("  [measured] coverage ALONG the measured line: %.4f\n",
                static_cast<double>(along));
    check(along > kNonzeroFloor,
          "measure: a tip built from the REPORTED angle reaches 15px out along the line "
          "that was measured -- the number actually points where it claims");

    // Reflected about the x axis: (0.6, -0.8). A negated or CCW-positive
    // heading would put the footprint here instead of on the query above.
    const float mirrored = dabCoverage(tip, 0.6f * kProbeDist, -0.8f * kProbeDist);
    std::printf("  [measured] coverage on the MIRRORED line (0.6,-0.8): %.4f\n",
                static_cast<double>(mirrored));
    check(mirrored == 0.0f,
          "measure: and does NOT reach the mirror image of that point, at zero tolerance -- "
          "rules out a heading whose sign disagrees with the build's rotation sense");

    // Perpendicular: (-0.8, 0.6). Rules out the classic 90-degrees-off link.
    const float perp = dabCoverage(tip, -0.8f * kProbeDist, 0.6f * kProbeDist);
    std::printf("  [measured] coverage PERPENDICULAR to the measured line: %.4f\n",
                static_cast<double>(perp));
    check(perp == 0.0f,
          "measure: nor a point perpendicular to it -- rules out an angle that is 90 degrees "
          "off, which a plausible-looking degree value would not reveal");
  }

  // =========================================================================
  // 4. The degenerate drag: a click that never moved
  // =========================================================================
  //
  // It happens every time a user taps the canvas to see where the tool is, so
  // it must produce numbers rather than a `nan` in the options bar. Both
  // answers come out of `std::atan2(0, 0) == 0` with no branch --
  // app/MeasureLine.hpp §4 -- and the implementation this rules out is the
  // obvious-looking `acos(dx / lengthPx)`, which divides by zero here.
  //
  // `isfinite` is asserted separately from the values, because a NaN compares
  // false against EVERYTHING: `r.angleDeg == 0.0f` is false for a NaN, so the
  // value check alone would already redden -- but it would redden with a
  // message about the wrong thing, and a reader chasing it would be looking
  // for an off-by-something rather than for a division.
  {
    const MeasureReadout r = measureReadout(lineFrom(60.0f, 60.0f, 60.0f, 60.0f));
    std::printf("  [measured] zero-length click: len=%.6f angle=%.6f\n",
                static_cast<double>(r.lengthPx), static_cast<double>(r.angleDeg));
    check(std::isfinite(r.lengthPx) && std::isfinite(r.angleDeg),
          "measure: a zero-length click reports finite numbers -- not the NaN an "
          "acos(dx/length) implementation produces on exactly this gesture");
    check(r.lengthPx == 0.0f && r.angleDeg == 0.0f,
          "measure: and reports 0 and 0 specifically, which is atan2(0,0)'s own answer "
          "inherited rather than special-cased");
  }

  // =========================================================================
  // 5. The gesture is a gesture: the ruler stops following on pen-up
  // =========================================================================
  //
  // The three calls `ui/MacPaintUI.cpp`'s canvas block makes, in the order it
  // makes them. The last move is the assertion: the frame after the user lets
  // go is exactly the frame they move the pointer to the options bar to read
  // the numbers, and a ruler that kept tracking would rewrite the measurement
  // out from under them while they looked at it.
  {
    MeasureLine line;
    check(!line.active && !line.dragging,
          "measure: a fresh MeasureLine is inactive -- no ruler is drawn or reported "
          "until one is dragged");

    beginMeasureLine(line, 7u, 100.0f, 100.0f);
    check(line.active && line.dragging && measureReadout(line).lengthPx == 0.0f,
          "measure: pen-down puts BOTH ends at the pointer, so a click that never moves is "
          "already a well-formed zero-length line rather than one with a stale far end");

    updateMeasureLine(line, 103.0f, 104.0f);
    check(measureReadout(line).lengthPx == 5.0f,
          "measure: the far end follows the pointer while the button is held");

    endMeasureLine(line);
    updateMeasureLine(line, 500.0f, 500.0f);
    check(line.active && !line.dragging && measureReadout(line).lengthPx == 5.0f,
          "measure: after pen-up the line STAYS and stops following -- a pointer that "
          "wanders off while the user reads the numbers must not rewrite them");

    clearMeasureLine(line);
    check(!line.active && !measureLineAppliesTo(line, 7u),
          "measure: leaving the tool clears the ruler, and a cleared ruler is reported "
          "by nothing");
  }

  // =========================================================================
  // 6. A ruler belongs to the document it was taken on
  // =========================================================================
  //
  // The coordinates are texels of ONE document. A line dragged on a 4000 px
  // scan, still sitting in `AppState` while the user tabs to a 512 px sketch,
  // would otherwise report a length that has nothing to do with what is on
  // screen -- and a wrong number is worse than a wrong rectangle, because it
  // is a plausible integer rather than something visibly out of place. This
  // is the stale-`marqueeX0..Y1` defect ui/MacPaintUI.cpp's lasso branch was
  // written about, moved one panel over.
  {
    MeasureLine line;
    beginMeasureLine(line, 42u, 0.0f, 0.0f);
    updateMeasureLine(line, 3.0f, 4.0f);
    endMeasureLine(line);
    check(measureLineAppliesTo(line, 42u),
          "measure: a ruler is reported on the document it was taken on");
    check(!measureLineAppliesTo(line, 43u),
          "measure: and is NOT reported on any other open document -- its texels are that "
          "document's texels, and a length in the wrong document is a plausible lie");
  }

  // =========================================================================
  // 7. The seventh gate, added rather than borrowed
  // =========================================================================
  //
  // `Tool::Measure` shares the eyedropper's palette group and answers the same
  // `ToolCursor::Sample`, so the one-line way to make
  // `toolHasCanvasHandler(Tool::Measure)` go true is to add one name to
  // `toolSamplesCanvas()`. That is the failure this build already has an
  // assertion about for Zoom -- "wired by widening an existing predicate
  // rather than adding the next one" -- and here it is not merely stylistic:
  // `toolSamplesCanvas()` is the literal gate on the eyedropper's own canvas
  // block, so a Measure that satisfied it would have every ruler drag handed
  // to `applyEyedropperPick()` and would silently overwrite the foreground
  // colour. The pair of clauses below is what makes that shortcut fail.
  {
    check(toolMeasuresCanvas(Tool::Measure) && !toolSamplesCanvas(Tool::Measure),
          "measure: Measure passes its OWN gate and NOT the eyedropper's -- the assertion "
          "that fails if the tool is ever wired by widening toolSamplesCanvas()");
    check(!toolMeasuresCanvas(Tool::Eyedropper) && !toolMeasuresCanvas(Tool::Brush) &&
              !toolMeasuresCanvas(Tool::Hand) && !toolMeasuresCanvas(Tool::Marquee),
          "measure: and no other tool passes the measure gate -- it names exactly the one "
          "canvas block it gates");
    check(toolImplemented(Tool::Measure) && toolHasCanvasHandler(Tool::Measure),
          "measure: the palette cell is live and the completeness check agrees -- the "
          "flag and the handler shipped together");
    check(!toolBeginsStroke(Tool::Measure) && !toolWritesRgbPixels(Tool::Measure) &&
              !toolDrawsSelection(Tool::Measure) && !toolPansView(Tool::Measure) &&
              !toolZoomsView(Tool::Measure),
          "measure: and through NO other gate -- it deposits nothing, fills nothing, "
          "selects nothing and moves the view not at all");
  }

  std::printf("[selftest] measure %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
