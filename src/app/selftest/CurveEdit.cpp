#include "app/selftest/Support.hpp"

namespace np {

// PLAN.md Phase 3 step 8 ("Op-stack UI... and a curve widget operating in
// the shaper domain"). See SelfTest.hpp for the full breakdown. Pure CPU --
// app/CurveEdit.hpp has no ImGui/GPU/PaintSim involvement whatsoever.
bool runCurveEditTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto isAscendingX = [](const Curve& c) {
    for (size_t i = 1; i < c.size(); ++i)
      if (!(c[i - 1].x <= c[i].x)) return false;
    return true;
  };

  // --- curveToPlot() / plotToCurve(): the y-flip and the round trip ---
  {
    float px = 0.0f, py = 0.0f;
    curveToPlot(0.0f, 0.0f, 100.0f, px, py);
    check(nearf(px, 0.0f, 1e-4f) && nearf(py, 100.0f, 1e-4f),
          "curveToPlot: curve-space (0,0) (y-up) is plot-local (0,plotSize) "
          "(y-down) -- the bottom-left corner");

    curveToPlot(1.0f, 1.0f, 100.0f, px, py);
    check(nearf(px, 100.0f, 1e-4f) && nearf(py, 0.0f, 1e-4f),
          "curveToPlot: curve-space (1,1) is plot-local (plotSize,0) -- the top-right corner");

    curveToPlot(0.5f, 0.25f, 100.0f, px, py);
    check(nearf(px, 50.0f, 1e-4f) && nearf(py, 75.0f, 1e-4f),
          "curveToPlot: a hand-computed interior point (0.5,0.25) -> (50,75)");

    float cx = 0.0f, cy = 0.0f;
    plotToCurve(px, py, 100.0f, cx, cy);
    check(nearf(cx, 0.5f, 1e-4f) && nearf(cy, 0.25f, 1e-4f),
          "plotToCurve: inverts curveToPlot() exactly for the same point");

    curveToPlot(0.5f, 0.25f, 0.0f, px, py);
    check(nearf(px, 0.0f, 1e-4f) && nearf(py, 0.0f, 1e-4f),
          "curveToPlot: plotSize <= 0 returns (0,0) rather than dividing by zero");
    plotToCurve(10.0f, 10.0f, -5.0f, cx, cy);
    check(nearf(cx, 0.0f, 1e-4f) && nearf(cy, 0.0f, 1e-4f),
          "plotToCurve: plotSize <= 0 returns (0,0) rather than dividing by zero");
  }

  // --- hitTestPoint(): exact/inside/outside radius, empty curve, tie-break ---
  {
    const Curve curve = {CurvePoint{0.2f, 0.3f}, CurvePoint{0.6f, 0.7f}};
    constexpr float plotSize = 200.0f;
    float p0x = 0.0f, p0y = 0.0f;
    curveToPlot(0.2f, 0.3f, plotSize, p0x, p0y);

    const auto exact = hitTestPoint(curve, p0x, p0y, plotSize, 5.0f);
    check(exact.has_value() && *exact == 0, "hitTestPoint: a query exactly on a point hits it");

    const auto inside = hitTestPoint(curve, p0x + 4.0f, p0y, plotSize, 5.0f);
    check(inside.has_value() && *inside == 0,
          "hitTestPoint: a query 4px from a point hits it within a 5px radius");

    const auto outside = hitTestPoint(curve, p0x + 10.0f, p0y, plotSize, 5.0f);
    check(!outside.has_value(),
          "hitTestPoint: a query 10px from every point misses within a 5px radius");

    check(!hitTestPoint(Curve{}, 0.0f, 0.0f, plotSize, 1000.0f).has_value(),
          "hitTestPoint: an empty curve always misses, regardless of radius");

    // Two points equidistant (40px) from the query -- (0.1,0.1) and
    // (0.9,0.1) on a 100px plot both sit 40px from plot-local x=50 at the
    // same y -- must resolve to the earlier index (index 0), the
    // documented tie-break.
    const Curve tie = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.9f, 0.1f}};
    const auto tieHit = hitTestPoint(tie, 50.0f, 90.0f, 100.0f, 40.0f);
    check(tieHit.has_value() && *tieHit == 0,
          "hitTestPoint: two equidistant points resolve to the earlier index");
  }

  // --- insertPoint(): scrambled insertion order stays sorted at every step ---
  {
    Curve curve;
    const size_t i0 = insertPoint(curve, 0.5f, 0.0f);
    check(i0 == 0 && isAscendingX(curve),
          "insertPoint: first point into an empty curve lands at index 0");
    const size_t i1 = insertPoint(curve, 0.1f, 0.0f);
    check(i1 == 0 && isAscendingX(curve),
          "insertPoint: a smaller x inserts before the existing point (index 0)");
    const size_t i2 = insertPoint(curve, 0.9f, 0.0f);
    check(i2 == 2 && isAscendingX(curve),
          "insertPoint: a larger x appends at the end (index 2)");
    const size_t i3 = insertPoint(curve, 0.3f, 0.0f);
    check(i3 == 1 && isAscendingX(curve),
          "insertPoint: an interior x inserts between its neighbours (index 1)");
    const size_t i4 = insertPoint(curve, 0.7f, 0.0f);
    check(i4 == 3 && isAscendingX(curve),
          "insertPoint: a second interior x still finds the correct sorted slot (index 3)");
    check(curve.size() == 5 && nearf(curve[0].x, 0.1f, 1e-4f) &&
              nearf(curve[1].x, 0.3f, 1e-4f) && nearf(curve[2].x, 0.5f, 1e-4f) &&
              nearf(curve[3].x, 0.7f, 1e-4f) && nearf(curve[4].x, 0.9f, 1e-4f),
          "insertPoint: five points inserted in scrambled order end up exactly "
          "{0.1,0.3,0.5,0.7,0.9}");

    Curve clampCurve;
    insertPoint(clampCurve, -1.0f, 5.0f);
    check(clampCurve.size() == 1 && nearf(clampCurve[0].x, 0.0f, 1e-4f) &&
              nearf(clampCurve[0].y, 1.0f, 1e-4f),
          "insertPoint: out-of-[0,1] input is clamped before insertion");
  }

  // --- movePoint(): crossing a neighbour re-sorts, a small move doesn't ---
  {
    Curve curve = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t newIdx = movePoint(curve, 0, 0.7f, 0.15f);
    check(newIdx == 1 && isAscendingX(curve),
          "movePoint: moving index 0 past its neighbour returns its new index (1)");
    check(nearf(curve[0].x, 0.5f, 1e-4f) && nearf(curve[0].y, 0.5f, 1e-4f),
          "movePoint: the point originally at index 1 is now found at index 0 "
          "(the opposite relative position)");
    check(nearf(curve[1].x, 0.7f, 1e-4f) && nearf(curve[1].y, 0.15f, 1e-4f),
          "movePoint: the moved point itself lands at its new (x,y)");
    check(nearf(curve[2].x, 0.9f, 1e-4f) && nearf(curve[2].y, 0.9f, 1e-4f),
          "movePoint: the point not involved in the crossing is untouched");

    Curve noCross = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t sameIdx = movePoint(noCross, 1, 0.55f, 0.6f);
    check(sameIdx == 1 && isAscendingX(noCross),
          "movePoint: a small move that crosses no neighbour keeps the same index");
    check(nearf(noCross[0].x, 0.1f, 1e-4f) && nearf(noCross[2].x, 0.9f, 1e-4f),
          "movePoint: ...and leaves the other two points' order untouched");

    Curve clampCurve = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t clampedIdx = movePoint(clampCurve, 2, -3.0f, 10.0f);
    check(clampedIdx == 0 && isAscendingX(clampCurve),
          "movePoint: an out-of-[0,1] target clamps before moving/re-sorting");
    check(nearf(clampCurve[0].x, 0.0f, 1e-4f) && nearf(clampCurve[0].y, 1.0f, 1e-4f),
          "movePoint: the clamped point lands at (0,1) as the new first entry");

    bool threw = false;
    try {
      movePoint(clampCurve, 99, 0.5f, 0.5f);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    check(threw, "movePoint: an out-of-range index throws std::out_of_range, matching "
                 "core::OpStack's own bounds-checked mutator convention");
  }

  // --- removePoint(): removes exactly the intended point, shifts the rest ---
  {
    Curve curve = {CurvePoint{0.1f, 0.0f}, CurvePoint{0.3f, 0.0f}, CurvePoint{0.5f, 0.0f},
                   CurvePoint{0.7f, 0.0f}};
    removePoint(curve, 1);
    check(curve.size() == 3 && nearf(curve[0].x, 0.1f, 1e-4f) && nearf(curve[1].x, 0.5f, 1e-4f) &&
              nearf(curve[2].x, 0.7f, 1e-4f),
          "removePoint: removes exactly the intended point, shifting later indices down by one");

    bool threw = false;
    try {
      removePoint(curve, 50);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    check(threw, "removePoint: an out-of-range index throws std::out_of_range");
  }

  // --- 0/1-point degenerate cases evalCurve() itself documents as identity
  // (ops/PointOps.hpp), checked both by curve size (this module's own
  // bookkeeping) and by feeding the CurveEdit-built curve straight into the
  // real evalCurve() as an oracle -- proving the two modules agree, not
  // just that CurveEdit's sizes look plausible. ---
  {
    Curve curve;
    check(curve.size() == 0 && nearf(evalCurve(curve, 0.37f), 0.37f, 1e-6f),
          "degenerate: an empty curve is evalCurve() identity");
    insertPoint(curve, 0.5f, 0.9f);
    check(curve.size() == 1 && nearf(evalCurve(curve, 0.37f), 0.37f, 1e-6f),
          "degenerate: a single-point curve (from insertPoint()) is still evalCurve() identity");
    removePoint(curve, 0);
    check(curve.size() == 0 && nearf(evalCurve(curve, 0.62f), 0.62f, 1e-6f),
          "degenerate: removePoint() back down to empty restores evalCurve() identity");
  }

  std::printf("[selftest] curve edit %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
