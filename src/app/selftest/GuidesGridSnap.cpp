#include "app/selftest/Support.hpp"

namespace np {

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// See SelfTest.hpp for the full scope note on why this covers app/Snapping.hpp's
// pure math and nothing UI-shaped.
bool runGuidesGridSnapTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto vecNear = [&](const std::vector<float>& a, const std::vector<float>& b, float tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!nearf(a[i], b[i], tol)) return false;
    return true;
  };

  // --- gridLinePositions() / isMajorGridLine(): hand-computable cases ---
  {
    const auto lines = gridLinePositions(100.0f, 4, 0.0f, 100.0f);
    check(vecNear(lines, {0.0f, 25.0f, 50.0f, 75.0f, 100.0f}, 1e-3f),
          "gridLinePositions: spacing=100/subdivisions=4 over [0,100] is exactly "
          "{0,25,50,75,100}");

    const auto majorsOnly = gridLinePositions(100.0f, 1, 0.0f, 250.0f);
    check(vecNear(majorsOnly, {0.0f, 100.0f, 200.0f}, 1e-3f),
          "gridLinePositions: subdivisions=1 returns major lines only, {0,100,200}");

    // The grid always anchors to document-space 0, not the queried range's
    // own start -- a range that doesn't begin at 0 still lands on the same
    // 0-based lattice.
    const auto offsetRange = gridLinePositions(50.0f, 2, 60.0f, 140.0f);
    check(vecNear(offsetRange, {75.0f, 100.0f, 125.0f}, 1e-3f),
          "gridLinePositions: [60,140] at spacing=50/subdivisions=2 (minor=25) is "
          "{75,100,125}, anchored at 0 rather than the range start");

    check(gridLinePositions(0.0f, 4, 0.0f, 100.0f).empty(),
          "gridLinePositions: non-positive spacing returns nothing");
    check(gridLinePositions(100.0f, 0, 0.0f, 100.0f).empty(),
          "gridLinePositions: non-positive subdivisions returns nothing");

    check(isMajorGridLine(200.0f, 100.0f) && isMajorGridLine(0.0f, 100.0f) &&
              !isMajorGridLine(225.0f, 100.0f),
          "isMajorGridLine: multiples of spacing are major, others are not");
  }

  // --- parseGuidePosition(): plain numbers, percentages, whitespace, junk ---
  {
    const auto px = parseGuidePosition("512", 1024.0f);
    check(px.has_value() && nearf(*px, 512.0f, 1e-3f), "parseGuidePosition: \"512\" -> 512 px");

    const auto pct = parseGuidePosition("50%", 1024.0f);
    check(pct.has_value() && nearf(*pct, 512.0f, 1e-3f),
          "parseGuidePosition: \"50%%\" of a 1024 px axis -> 512");

    const auto pctSmall = parseGuidePosition("25%", 800.0f);
    check(pctSmall.has_value() && nearf(*pctSmall, 200.0f, 1e-3f),
          "parseGuidePosition: \"25%%\" of an 800 px axis -> 200");

    const auto withSpace = parseGuidePosition("  128  ", 1024.0f);
    check(withSpace.has_value() && nearf(*withSpace, 128.0f, 1e-3f),
          "parseGuidePosition: surrounding whitespace is trimmed");

    check(!parseGuidePosition("nope", 1024.0f).has_value(),
          "parseGuidePosition: unparseable text returns nothing");
    check(!parseGuidePosition("", 1024.0f).has_value(),
          "parseGuidePosition: empty text returns nothing");
    check(!parseGuidePosition("%", 1024.0f).has_value(),
          "parseGuidePosition: a bare \"%%\" with no digits returns nothing");
  }

  // --- resolveSnap(): the function that actually matters (PRD Q6). Canvas
  // is 1000x800; one horizontal guide at y=310, one vertical guide at
  // x=690 (both deliberately off the grid lattice below, so a snap to
  // (690,310) can only be explained by the guides, not a coincidental grid
  // line); grid spacing=50, subdivisions=1 (minor lines at multiples of
  // 50); snap threshold 5 document px. ---
  {
    const std::vector<Guide> guides = {
        Guide{GuideOrientation::Horizontal, 310.0f},
        Guide{GuideOrientation::Vertical, 690.0f},
    };
    constexpr float canvasW = 1000.0f, canvasH = 800.0f;
    constexpr float spacing = 50.0f;
    constexpr int subdivisions = 1;
    constexpr float threshold = 5.0f;

    {
      // 3px from the vertical guide, 2px from the horizontal one -- both
      // well outside range of any grid line (nearest grid x to 693 is 700,
      // 7px away; nearest grid y to 308 is 300, 8px away -- neither
      // qualifies within the 5px threshold), so this isolates guide
      // snapping specifically.
      const SnapResult r = resolveSnap(Vec2{693.0f, 308.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(r.snappedX && r.snappedY && nearf(r.point.x, 690.0f, 1e-3f) &&
                nearf(r.point.y, 310.0f, 1e-3f),
            "resolveSnap: a point near both guides snaps exactly onto them");
    }

    {
      const SnapResult r = resolveSnap(Vec2{432.0f, 217.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(!r.snappedX && !r.snappedY && nearf(r.point.x, 432.0f, 1e-3f) &&
                nearf(r.point.y, 217.0f, 1e-3f),
            "resolveSnap: a point far from every guide, grid line and canvas edge is left "
            "unchanged");
    }

    {
      // Nearest grid x to 452 is 450 (2px); nearest grid y to 218 is 200
      // (18px, outside threshold) -- X should snap to the grid, Y should
      // not snap to anything (axes snap independently).
      const SnapResult r = resolveSnap(Vec2{452.0f, 218.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(r.snappedX && nearf(r.point.x, 450.0f, 1e-3f),
            "resolveSnap: a point near a grid line snaps to that grid line");
      check(!r.snappedY && nearf(r.point.y, 218.0f, 1e-3f),
            "resolveSnap: ...while that same point's Y, near nothing, is left untouched "
            "(axes snap independently)");
    }

    {
      const SnapResult topLeft = resolveSnap(Vec2{2.0f, 2.0f}, guides, spacing, subdivisions,
                                             canvasW, canvasH, threshold);
      check(topLeft.snappedX && topLeft.snappedY && nearf(topLeft.point.x, 0.0f, 1e-3f) &&
                nearf(topLeft.point.y, 0.0f, 1e-3f),
            "resolveSnap: near the top-left corner snaps to the canvas origin edges");

      const SnapResult bottomRight =
          resolveSnap(Vec2{canvasW - 3.0f, canvasH - 1.0f}, guides, spacing, subdivisions,
                     canvasW, canvasH, threshold);
      check(bottomRight.snappedX && bottomRight.snappedY &&
                nearf(bottomRight.point.x, canvasW, 1e-3f) &&
                nearf(bottomRight.point.y, canvasH, 1e-3f),
            "resolveSnap: near the bottom-right corner snaps to the canvas far edges");
    }

    {
      // The global snapping toggle (PRD Q6) is implemented by the caller
      // passing threshold 0 rather than a second code path -- confirm that
      // actually disables snapping outright, even exactly on a guide.
      const SnapResult r = resolveSnap(Vec2{690.0f, 310.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, 0.0f);
      check(!r.snappedX && !r.snappedY, "resolveSnap: a non-positive threshold snaps nothing");
    }
  }

  std::printf("[selftest] guides/grid/snap %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
