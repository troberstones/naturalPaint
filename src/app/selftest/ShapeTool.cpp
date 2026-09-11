#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/AppState.hpp"  // the real `enum class Tool`, for toolCreatesShapes()
#include "app/ShapeTool.hpp"
#include "core/PathRaster.hpp"

namespace np {

// app/ShapeTool -- Tool::Shape's headless geometry and commit, per
// docs/ui.md §4a ("a Shape tool is a gesture that emits a `VectorShape` into
// the layer the Pen already edits"). Headless and GPU-free like
// app/selftest/PenDraw.cpp: no ImGui, no live document, `std::vector<
// VectorShape>` and a bare `PathEditState` built directly, calling the same
// functions `ui/MacPaintUI.cpp`'s canvas block calls.
bool runShapeToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; };
  auto ptNear = [&](PathPoint a, PathPoint b, float tol = 1e-3f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };
  const VectorStyle kStyle;

  // =======================================================================
  // 1. A plain click creates nothing, for every kind
  // =======================================================================
  {
    ShapeToolState tool;
    const ShapeKind kinds[] = {ShapeKind::Rectangle, ShapeKind::Ellipse, ShapeKind::RoundedRect,
                               ShapeKind::Polygon, ShapeKind::Line};
    bool allEmpty = true;
    for (ShapeKind k : kinds) {
      tool.kind = k;
      const VectorShape s = shapeToolGeometry(tool, PathPoint{5, 5}, PathPoint{5, 5}, false, false);
      if (!pathIsEmpty(s.path)) allEmpty = false;
    }
    check(allEmpty, "a zero-length drag (a plain click) produces an empty path for every kind");
  }

  // =======================================================================
  // 2. Rectangle: corners exactly where expected, with each modifier
  // =======================================================================
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::Rectangle;

    // Corner to corner, no modifiers: a 40x20 box from (10,10) to (50,30).
    const VectorShape plain = shapeToolGeometry(tool, PathPoint{10, 10}, PathPoint{50, 30}, false, false);
    check(plain.path.subpaths.size() == 1 && plain.path.subpaths[0].anchors.size() == 4 &&
              plain.path.subpaths[0].closed,
          "rectangle: four anchors, closed");
    if (plain.path.subpaths.size() == 1 && plain.path.subpaths[0].anchors.size() == 4) {
      const SubPath& sub = plain.path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, PathPoint{10, 10}) &&
                ptNear(sub.anchors[1].pt, PathPoint{50, 10}) &&
                ptNear(sub.anchors[2].pt, PathPoint{50, 30}) &&
                ptNear(sub.anchors[3].pt, PathPoint{10, 30}),
            "rectangle: corners at the drag's own box, no modifiers");
    }

    // Shift: the box becomes a 20x20 square anchored at the press point --
    // the LARGER of the two extents wins (dragBBox()'s own rule), so a
    // 40-wide, 20-tall drag becomes 40x40, not 20x20.
    const VectorShape square = shapeToolGeometry(tool, PathPoint{10, 10}, PathPoint{50, 30}, true, false);
    if (square.path.subpaths.size() == 1 && square.path.subpaths[0].anchors.size() == 4) {
      const SubPath& sub = square.path.subpaths[0];
      const float w = sub.anchors[1].pt.x - sub.anchors[0].pt.x;
      const float h = sub.anchors[3].pt.y - sub.anchors[0].pt.y;
      check(near(w, 40.0f) && near(h, 40.0f) && near(sub.anchors[0].pt.x, 10.0f) &&
                near(sub.anchors[0].pt.y, 10.0f),
            "rectangle + Shift: square sized to the larger extent, anchored at the press point");
    } else {
      check(false, "rectangle + Shift: square sized to the larger extent, anchored at the press point");
    }

    // Option: the press point is the CENTRE, so a (10,10)->(30,20) drag
    // (20 wide, 10 tall relative to the press) makes a 40x20 box centred on
    // (10,10): (-10,0) to (30,20).
    const VectorShape centered =
        shapeToolGeometry(tool, PathPoint{10, 10}, PathPoint{30, 20}, false, true);
    if (centered.path.subpaths.size() == 1 && centered.path.subpaths[0].anchors.size() == 4) {
      const SubPath& sub = centered.path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, PathPoint{-10, 0}) &&
                ptNear(sub.anchors[2].pt, PathPoint{30, 20}),
            "rectangle + Option: the press point is the box's centre");
    } else {
      check(false, "rectangle + Option: the press point is the box's centre");
    }

    // A one-axis drag (zero height) is degenerate, matching svgRectPath()'s
    // own "non-positive dimension -> nothing rendered" rule.
    const VectorShape flat = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{10, 0}, false, false);
    check(pathIsEmpty(flat.path), "rectangle: a one-axis drag (zero height) is empty");
  }

  // =======================================================================
  // 3. Ellipse: control points on the analytic curve
  // =======================================================================
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::Ellipse;
    // A 40x20 box from (0,0) to (40,20): centre (20,10), rx=20, ry=10.
    const VectorShape e = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{40, 20}, false, false);
    check(e.path.subpaths.size() == 1 && e.path.subpaths[0].anchors.size() == 4 &&
              e.path.subpaths[0].closed,
          "ellipse: four anchors (right/bottom/left/top), closed");
    if (e.path.subpaths.size() == 1 && e.path.subpaths[0].anchors.size() == 4) {
      const SubPath& sub = e.path.subpaths[0];
      // svgEllipsePath()'s own anchor order: right, bottom, left, top.
      check(ptNear(sub.anchors[0].pt, PathPoint{40, 10}) &&
                ptNear(sub.anchors[1].pt, PathPoint{20, 20}) &&
                ptNear(sub.anchors[2].pt, PathPoint{0, 10}) &&
                ptNear(sub.anchors[3].pt, PathPoint{20, 0}),
            "ellipse: the four cardinal anchors sit exactly on the analytic curve");
    }

    // Shift: a circle. A 40x20 drag becomes a 40-diameter circle centred on
    // the press point (0,0): (-20,0) to (20,40) in bounds terms, i.e. the
    // rightmost anchor sits at (20, 0).
    const VectorShape circle = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{40, 20}, true, false);
    if (circle.path.subpaths.size() == 1 && circle.path.subpaths[0].anchors.size() == 4) {
      const SubPath& sub = circle.path.subpaths[0];
      const float rx = std::fabs(sub.anchors[0].pt.x - sub.anchors[2].pt.x) * 0.5f;
      const float ry = std::fabs(sub.anchors[3].pt.y - sub.anchors[1].pt.y) * 0.5f;
      check(near(rx, ry), "ellipse + Shift: equal radii (a circle)");
      // Equal radii alone does not prove WHICH extent won -- a bug that
      // shrank the box to the SMALLER of the two drag extents would still
      // pass the check above (both radii still equal, just both wrong).
      // dragBBox()'s own rule is the larger extent, so the diameter here
      // must be 40 (the drag's width), not 20 (its height).
      check(near(rx, 20.0f) && near(ry, 20.0f),
            "ellipse + Shift: sized to the LARGER of the drag's two extents (diameter 40, "
            "not 20)");
    } else {
      check(false, "ellipse + Shift: equal radii (a circle)");
      check(false, "ellipse + Shift: sized to the LARGER of the drag's two extents (diameter 40, "
                   "not 20)");
    }
  }

  // =======================================================================
  // 4. Rounded rectangle: the corner radius param reaches the geometry
  // =======================================================================
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::RoundedRect;
    tool.cornerRadius = 5.0f;
    const VectorShape r = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{40, 20}, false, false);
    // svgRectPath()'s rounded-corner form is eight anchors; a plain
    // rectangle (radius 0, tested in section 2) is four -- the count itself
    // proves the radius reached the call.
    check(r.path.subpaths.size() == 1 && r.path.subpaths[0].anchors.size() == 8,
          "rounded rectangle: eight anchors once the corner radius is non-zero");

    // A radius of 0 degenerates back to a plain rectangle (four anchors),
    // svgRectPath()'s own `rx <= 0 && ry <= 0` branch.
    ShapeToolState sharp = tool;
    sharp.cornerRadius = 0.0f;
    const VectorShape r0 = shapeToolGeometry(sharp, PathPoint{0, 0}, PathPoint{40, 20}, false, false);
    check(r0.path.subpaths.size() == 1 && r0.path.subpaths[0].anchors.size() == 4,
          "rounded rectangle: a zero radius is a plain four-anchor rectangle");
  }

  // =======================================================================
  // 5. Polygon: side count, and Shift's "inscribe in a circle" meaning
  // =======================================================================
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::Polygon;
    tool.sides = 6;
    const VectorShape hex = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{20, 20}, false, false);
    check(hex.path.subpaths.size() == 1 && hex.path.subpaths[0].anchors.size() == 6 &&
              hex.path.subpaths[0].closed,
          "polygon: exactly `sides` anchors, closed");

    // Every vertex of a Shift-held (equal rx/ry) polygon sits the same
    // distance from the box's centre -- the analytic definition of
    // "inscribed in a circle." A DELIBERATELY non-square drag (40 wide, 20
    // tall): a square 20x20 drag would pass this same check even if the
    // implementation picked the SMALLER extent instead of the larger one
    // (`dragBBox()`'s own rule), since a square input makes "smaller" and
    // "larger" the same number.
    const VectorShape reg = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{40, 20}, true, false);
    if (reg.path.subpaths.size() == 1 && reg.path.subpaths[0].anchors.size() == 6) {
      const SubPath& sub = reg.path.subpaths[0];
      float minX = sub.anchors[0].pt.x, maxX = minX, minY = sub.anchors[0].pt.y, maxY = minY;
      for (const Anchor& a : sub.anchors) {
        minX = std::min(minX, a.pt.x);
        maxX = std::max(maxX, a.pt.x);
        minY = std::min(minY, a.pt.y);
        maxY = std::max(maxY, a.pt.y);
      }
      const float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
      float r0 = std::hypot(sub.anchors[0].pt.x - cx, sub.anchors[0].pt.y - cy);
      bool allEqual = true;
      for (const Anchor& a : sub.anchors)
        if (!near(std::hypot(a.pt.x - cx, a.pt.y - cy), r0, 0.01f)) allEqual = false;
      check(allEqual, "polygon + Shift: every vertex the same distance from centre (regular)");
      // And that shared radius is the LARGER extent's (20, half of the 40
      // wide drag), not the smaller one's (10, half of the 20 tall drag) --
      // the same magnitude check the ellipse's own Shift test makes, closing
      // the identical gap (an "equal radii" check alone cannot tell a
      // shrunk-but-still-regular polygon from a correctly sized one).
      check(near(r0, 20.0f, 0.05f),
            "polygon + Shift: the shared radius is the LARGER extent's half (20, not 10)");
    } else {
      check(false, "polygon + Shift: every vertex the same distance from centre (regular)");
      check(false, "polygon + Shift: the shared radius is the LARGER extent's half (20, not 10)");
    }

    // Fewer than 3 sides clamps to a triangle rather than degenerating.
    ShapeToolState two = tool;
    two.sides = 1;
    const VectorShape tri = shapeToolGeometry(two, PathPoint{0, 0}, PathPoint{20, 20}, false, false);
    check(tri.path.subpaths.size() == 1 && tri.path.subpaths[0].anchors.size() == 3,
          "polygon: a side count below 3 clamps to a triangle");
  }

  // =======================================================================
  // 6. Line: the open two-anchor path, and its own modifier meanings
  // =======================================================================
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::Line;
    const VectorShape l = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{10, 0}, false, false);
    check(l.path.subpaths.size() == 1 && l.path.subpaths[0].anchors.size() == 2 &&
              !l.path.subpaths[0].closed,
          "line: two anchors, open");
    if (l.path.subpaths.size() == 1 && l.path.subpaths[0].anchors.size() == 2) {
      check(ptNear(l.path.subpaths[0].anchors[0].pt, PathPoint{0, 0}) &&
                ptNear(l.path.subpaths[0].anchors[1].pt, PathPoint{10, 0}),
            "line: endpoints are exactly the press and the release, with no modifiers");
    }

    // Shift snaps a near-horizontal-but-not-quite drag to a true 45-degree
    // multiple (here, to due east) while preserving the drag's length.
    const VectorShape snapped =
        shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{10, 1}, true, false);
    if (snapped.path.subpaths.size() == 1 && snapped.path.subpaths[0].anchors.size() == 2) {
      const PathPoint end = snapped.path.subpaths[0].anchors[1].pt;
      check(near(end.y, 0.0f, 0.05f) && end.x > 9.9f,
            "line + Shift: snapped to the nearest 45 degrees, length preserved");
    } else {
      check(false, "line + Shift: snapped to the nearest 45 degrees, length preserved");
    }

    // A drag near 42 degrees is close to a true 45-degree multiple but
    // nowhere near a 90-degree one -- the case above (5.7 degrees off
    // horizontal) cannot tell "nearest 45" from "nearest 90" apart, since
    // both round it to the same due-east answer. This one can: snapping to
    // the nearest 90 would send it to due east (dy=0); snapping to the
    // nearest 45, the actual rule, sends it to the diagonal (dx == dy).
    const VectorShape diag = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{10, 9}, true, false);
    if (diag.path.subpaths.size() == 1 && diag.path.subpaths[0].anchors.size() == 2) {
      const PathPoint end = diag.path.subpaths[0].anchors[1].pt;
      check(end.x > 0.0f && near(end.x, end.y, 0.1f),
            "line + Shift: a near-diagonal drag snaps to the DIAGONAL (45 degrees), not to "
            "due east (a coarser 90-degree snap would also pass the near-horizontal case above)");
    } else {
      check(false,
            "line + Shift: a near-diagonal drag snaps to the DIAGONAL (45 degrees), not to "
            "due east (a coarser 90-degree snap would also pass the near-horizontal case above)");
    }

    // Option: the press point is the MIDPOINT, so a (0,0)->(10,0) drag
    // becomes a line from (-10,0) to (10,0).
    const VectorShape centered = shapeToolGeometry(tool, PathPoint{0, 0}, PathPoint{10, 0}, false, true);
    if (centered.path.subpaths.size() == 1 && centered.path.subpaths[0].anchors.size() == 2) {
      check(ptNear(centered.path.subpaths[0].anchors[0].pt, PathPoint{-10, 0}) &&
                ptNear(centered.path.subpaths[0].anchors[1].pt, PathPoint{10, 0}),
            "line + Option: the press point is the segment's midpoint");
    } else {
      check(false, "line + Option: the press point is the segment's midpoint");
    }
  }

  // =======================================================================
  // 7. Rasterised coverage of a rectangle equals the analytic area
  // =======================================================================
  //
  // The geometry function alone cannot catch a rasteriser that silently
  // disagrees with it (a winding-rule mismatch, an off-by-one in the span
  // walk) -- this closes that gap the way app/selftest/PathRaster.cpp's own
  // area checks do, over the exact shape this tool builds rather than a
  // hand-built fixture.
  {
    ShapeToolState tool;
    tool.kind = ShapeKind::Rectangle;
    const VectorShape r = shapeToolGeometry(tool, PathPoint{2, 2}, PathPoint{22, 12}, false, false);
    PathRasterScratch scratch;
    double coverage = 0.0;
    rasterizePath(r.path, 0.05f, RasterClip{0, 0, 32, 32}, scratch,
                 [&](int32_t, int32_t x0, int32_t x1, const float* cov) {
                   for (int32_t x = x0; x < x1; ++x) coverage += cov[x - x0];
                 });
    // 20 x 10 = 200 texels, straight-edged so the analytic and the
    // rasterised area should agree to well under one texel.
    check(std::fabs(coverage - 200.0) < 1.0,
          "rasterised coverage of a rectangle shape equals its analytic area (20x10 = 200)");
  }

  // =======================================================================
  // 8. Commit: one shape, and only when the drag was usable
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState pathEdit;
    ShapeToolState tool;
    tool.kind = ShapeKind::Rectangle;

    const ShapeCommitResult clicked =
        commitShapeTool(tool, PathPoint{5, 5}, PathPoint{5, 5}, false, false, kStyle, &shapes,
                        &nextId, &pathEdit);
    check(clicked == ShapeCommitResult::Empty && shapes.empty() && nextId == 1,
          "commit: a plain click commits nothing and does not advance the id counter");

    const ShapeCommitResult drawn = commitShapeTool(tool, PathPoint{0, 0}, PathPoint{10, 10}, false,
                                                    false, kStyle, &shapes, &nextId, &pathEdit);
    check(drawn == ShapeCommitResult::Committed && shapes.size() == 1 && nextId == 2,
          "commit: a real drag creates exactly one shape and advances the id once");
    check(shapes.size() == 1 && shapes[0].fill.on == kStyle.fill.on &&
              shapes[0].stroke.on == kStyle.stroke.on,
          "commit: the shape is stamped with the given style, not the invisible default "
          "(app/VectorStyle.hpp section 1's own defect, in the tool that would have repeated it)");
    check(pathEdit.selection.mode == PathSelectMode::Shape &&
              pathEdit.selection.shapes.size() == 1 && pathEdit.selection.shapes[0] == shapes[0].id,
          "commit: the new shape is left selected (Shape mode), for Path Select to pick up "
          "immediately");

    // A second commit creates a SECOND shape with a different id, and
    // replaces the selection rather than adding to it.
    const ShapeCommitResult drawn2 = commitShapeTool(tool, PathPoint{20, 0}, PathPoint{30, 10}, false,
                                                     false, kStyle, &shapes, &nextId, &pathEdit);
    check(drawn2 == ShapeCommitResult::Committed && shapes.size() == 2 &&
              shapes[1].id != shapes[0].id,
          "commit: a second draw is a second shape with a fresh id");
    check(pathEdit.selection.shapes.size() == 1 && pathEdit.selection.shapes[0] == shapes[1].id,
          "commit: selecting the new shape REPLACES the old selection, not adds to it");
  }

  // =======================================================================
  // 9. The tool-table term
  // =======================================================================
  {
    check(toolCreatesShapes(Tool::Shape), "toolCreatesShapes() is true for Tool::Shape");
    check(!toolCreatesShapes(Tool::Pen) && !toolCreatesShapes(Tool::PathSelect) &&
              !toolCreatesShapes(Tool::Curve),
          "toolCreatesShapes() is false for the other vector tools -- a widened predicate "
          "would hand their gestures to this one's geometry function");
  }

  // =======================================================================
  // 10. The KIND table has one row per ShapeKind, and no gaps
  // =======================================================================
  {
    check(kShapeKindCount == 5, "kShapeKinds has one row per ShapeKind");
    bool allNamed = true;
    for (size_t i = 0; i < kShapeKindCount; ++i)
      if (kShapeKinds[i].label == nullptr || kShapeKinds[i].label[0] == '\0') allNamed = false;
    check(allNamed, "every ShapeKind row has a non-empty label");
  }

  return ok;
}

}  // namespace np
