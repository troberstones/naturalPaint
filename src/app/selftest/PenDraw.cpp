#include "app/selftest/Support.hpp"

#include "app/PenTool.hpp"

namespace np {

// app/PenTool section 9 -- Pen/Curve PLACEMENT: `pathEditBeginPen()`,
// `pathEditUpdate()`'s `PenExtend` arm, `pathEditHasOpenPath()` and
// `pathEditEndOpenPath()`. Before this, `PathDragKind::PenExtend` was a
// switch arm with no writer (app/PenTool.hpp's own account of it) -- Pen and
// Curve could select and drag geometry that already existed (an SVG import)
// but had no way to CREATE any. This is that writer's own test.
//
// Headless and GPU-free like every other section of this file: no ImGui, no
// `AppState`, building `std::vector<VectorShape>` directly and calling the
// same four transitions `ui/MacPaintUI.cpp`'s Pen/Curve canvas block calls.
bool runPenDrawTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; };
  auto ptNear = [&](PathPoint a, PathPoint b, float tol = 1e-4f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };

  // =======================================================================
  // 1. The first press on empty canvas creates a shape with one anchor
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;

    check(!pathEditHasOpenPath(st), "no open path before the first press");

    const PenPressResult r = pathEditBeginPen(&st, &shapes, &nextId, PathPoint{10, 20}, 4.0f,
                                              1,
                                              /*curveMode=*/false);
    check(r == PenPressResult::Placed, "the first press on empty canvas places");
    check(shapes.size() == 1, "one shape created");
    check(shapes.size() == 1 && shapes[0].path.subpaths.size() == 1 &&
              shapes[0].path.subpaths[0].anchors.size() == 1,
          "one subpath, one anchor");
    check(shapes.size() == 1 && shapes[0].path.subpaths[0].anchors.size() == 1 &&
              ptNear(shapes[0].path.subpaths[0].anchors[0].pt, PathPoint{10, 20}),
          "the anchor sits exactly at the press");
    check(nextId == 2, "the layer's nextShapeId counter advanced");
    check(pathEditHasOpenPath(st), "a path is now open");
    check(st.selection.mode == PathSelectMode::Component &&
              st.selection.components.size() == 1 && st.selection.components[0].anchor == 0,
          "the placed anchor is the selection (bullet 1: 'one anchor... selected')");

    // A plain click -- pen-up with no intervening pathEditUpdate() -- never
    // touches the tangent, so it leaves the corner the placement made.
    pathEditEnd(&st, shapes);
    const Anchor& a0 = shapes[0].path.subpaths[0].anchors[0];
    check(a0.smooth == false && ptNear(a0.in, PathPoint{10, 20}) && ptNear(a0.out, PathPoint{10, 20}),
          "Pen's plain click leaves a corner (in == out == pt, smooth == false)");
    check(pathEditHasOpenPath(st), "the path stays open after a plain click's pen-up");
  }

  // =======================================================================
  // 2. A drag before release sets a mirrored, smooth tangent (Pen)
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{0, 0}, 4.0f, 1, /*curveMode=*/false);
    check(st.drag == PathDragKind::PenExtend, "the placement press opens a PenExtend drag");

    const PathEditChange c1 = pathEditUpdate(&st, &shapes, PathPoint{30, 0});
    check(c1 == PathEditChange::EditContinued,
          "the drag amends the placement's own edit rather than opening a second one");
    const Anchor& a = shapes[0].path.subpaths[0].anchors[0];
    check(a.smooth == true, "a drag before release makes the anchor smooth");
    check(ptNear(a.out, PathPoint{30, 0}), "the OUT tangent follows the pointer");
    check(ptNear(a.in, PathPoint{-30, 0}), "the IN tangent mirrors it through the anchor");

    pathEditEnd(&st, shapes);
    check(st.drag == PathDragKind::None, "pen-up ends the tangent drag");
    check(pathEditHasOpenPath(st), "...without ending the placement session");
  }

  // =======================================================================
  // 3. Three presses give three anchors, in order, and a press on the open
  //    subpath's own first anchor closes it (bullet 1)
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    const PathPoint pts[3] = {{0, 0}, {50, 0}, {50, 50}};
    for (const PathPoint& p : pts) {
      pathEditBeginPen(&st, &shapes, &nextId, p, 4.0f, 1,
                       false);
      pathEditEnd(&st, shapes);
    }
    check(shapes.size() == 1, "still one shape -- presses extend it, not create new ones");
    {
      const SubPath& sub = shapes[0].path.subpaths[0];
      check(sub.anchors.size() == 3, "three anchors");
      check(sub.anchors.size() == 3 && ptNear(sub.anchors[0].pt, pts[0]) &&
                ptNear(sub.anchors[1].pt, pts[1]) && ptNear(sub.anchors[2].pt, pts[2]),
            "in press order");
      check(sub.closed == false, "not closed -- no press has landed on the first anchor yet");
    }

    const PenPressResult closed = pathEditBeginPen(&st, &shapes, &nextId, pts[0], 4.0f, 1, false);
    check(closed == PenPressResult::Closed, "a press on the first anchor closes");
    check(shapes[0].path.subpaths[0].closed == true, "the subpath is now closed");
    check(!pathEditHasOpenPath(st), "placement ends once the path closes");
    check(shapes[0].path.subpaths[0].anchors.size() == 3,
          "closing adds no anchor of its own -- still three");
  }

  // =======================================================================
  // 4. Escape (no drag live) ends placement, leaving what was placed
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{0, 0}, 4.0f, 1, false);
    pathEditEnd(&st, shapes);
    // Well clear of the first anchor's own gnomon (default reach 40 doc
    // units around its pivot, section 5's tier 1) -- a second press ON the
    // gnomon would be "existing geometry" (bullet 2), not a placement, which
    // is a different scenario from the one under test here.
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{200, 0}, 4.0f, 1, false);
    pathEditEnd(&st, shapes);
    check(pathEditHasOpenPath(st), "still open after two presses");

    // `ui/MacPaintUI.cpp` routes Escape here when no drag is live -- see this
    // header's own comment on `pathEditEndOpenPath()`.
    pathEditEndOpenPath(&st);
    check(!pathEditHasOpenPath(st), "Escape ends the open placement session");
    check(shapes[0].path.subpaths[0].anchors.size() == 2,
          "leaving exactly what was placed -- two anchors, neither removed");
    check(shapes[0].path.subpaths[0].closed == false, "and unclosed");
  }

  // =======================================================================
  // 5. Curve mode -- every anchor smooth, C1-continuous at interior anchors
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    // Not collinear, so a passing test cannot be an accident of the fixture
    // geometry: an interior anchor's fitted tangent has to actually come
    // from a real Catmull-Rom average of its neighbours.
    const PathPoint pts[4] = {{0, 0}, {40, 10}, {80, 0}, {120, 30}};
    size_t placedCount = 0;
    for (const PathPoint& p : pts) {
      const PenPressResult r = pathEditBeginPen(&st, &shapes, &nextId, p, 4.0f, 1, /*curveMode=*/true);
      if (r == PenPressResult::Placed) ++placedCount;
      pathEditEnd(&st, shapes);
    }
    check(placedCount == 4, "all four Curve presses report Placed");

    const SubPath& sub = shapes[0].path.subpaths[0];
    check(sub.anchors.size() == 4, "four anchors placed");

    bool allSmooth = true;
    for (const Anchor& a : sub.anchors)
      if (!a.smooth) allSmooth = false;
    check(allSmooth, "Curve marks every anchor smooth");

    // C1 continuity, asserted numerically rather than trusted from
    // `smooth == true` alone: at every INTERIOR anchor, `in` and `out` are
    // exactly opposite through `pt` -- the algebraic meaning of "the
    // incoming and outgoing tangent directions agree."
    bool interiorC1 = true;
    for (size_t i = 1; i + 1 < sub.anchors.size(); ++i) {
      const Anchor& a = sub.anchors[i];
      const PathPoint mid{(a.in.x + a.out.x) * 0.5f, (a.in.y + a.out.y) * 0.5f};
      if (!ptNear(mid, a.pt, 1e-3f)) interiorC1 = false;
    }
    check(interiorC1, "interior anchors: pt is the midpoint of in/out (C1 continuity)");

    // The formula itself, hand-computed for anchor 1 (interior, neighbours
    // anchor 0 and anchor 2): m = (next - prev) / 2, handle = pt +/- m/3 --
    // the uniform Catmull-Rom -> Bezier conversion app/PenTool.cpp's
    // `fitAnchorTangent()` implements.
    const PathPoint expectedM{(pts[2].x - pts[0].x) * 0.5f, (pts[2].y - pts[0].y) * 0.5f};
    const PathPoint expectedOut{pts[1].x + expectedM.x / 3.0f, pts[1].y + expectedM.y / 3.0f};
    check(ptNear(sub.anchors[1].out, expectedOut, 1e-3f),
          "anchor 1's OUT tangent matches the hand-computed Catmull-Rom fit");
  }

  // =======================================================================
  // 6. A press on OTHER existing geometry ends an open path and falls
  //    through to pathEditBegin()'s ordinary gestures (bullet 2)
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    // A second, already-existing shape (as if imported from an SVG), well
    // clear of where the open path below is placed.
    VectorShape existing;
    existing.id = 99;
    SubPath esub;
    esub.closed = true;
    Anchor ea;
    ea.pt = PathPoint{500, 500};
    ea.in = ea.pt;
    ea.out = ea.pt;
    esub.anchors.push_back(ea);
    existing.path.subpaths.push_back(esub);
    shapes.push_back(existing);

    uint64_t nextId = 100;
    PathEditState st;
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{0, 0}, 4.0f, 1, false);
    const uint64_t openShapeId = st.openPathShapeId;
    pathEditEnd(&st, shapes);
    check(pathEditHasOpenPath(st), "a path is open");

    const PenPressResult r = pathEditBeginPen(&st, &shapes, &nextId, PathPoint{500, 500}, 4.0f,
                                              1, false);
    // **This assertion is inverted from the one it replaces**, and the
    // inversion is the point of docs/path-editing-plan.md track C. It used to
    // read "a press on OTHER existing geometry falls through to
    // pathEditBegin()'s gestures" -- which is exactly the behaviour that made
    // the Pen the manipulator as well as the drawing tool. Those gestures are
    // `Tool::PathSelect`'s now.
    check(r == PenPressResult::Inert,
          "a press on other existing geometry is INERT -- the Pen does not select, drag or "
          "manipulate, it only places points");
    check(st.drag == PathDragKind::None,
          "...and starts no drag: the whole class of gestures the Pen used to inherit from "
          "pathEditBegin() is gone, not merely unreported");
    check(!pathEditHasOpenPath(st), "...and that press 'clicked away', ending the open path");

    const VectorShape* openShapeAfter = nullptr;
    for (const VectorShape& s : shapes)
      if (s.id == openShapeId) openShapeAfter = &s;
    check(openShapeAfter != nullptr && openShapeAfter->path.subpaths[0].anchors.size() == 1,
          "the open shape itself is untouched -- still one anchor");
  }

  // =======================================================================
  // 9. RESUME -- picking a path back up by its loose end
  // =======================================================================
  //
  // docs/path-editing-plan.md section 6, decided 2026-09-09. Without this, an
  // open path abandoned with Escape could never be continued, only redrawn.
  //
  // The assertion that matters is not "placement reopened" -- it is WHERE the
  // next anchor lands. Placement appends, so resuming from the FRONT of a
  // subpath has to reverse it first; a resume that reopened the session
  // without reversing would put the next point at the wrong end of the line,
  // which looks like the path jumping.
  {
    // Two anchors running left to right: (0,0) then (100,0).
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{0, 0}, 4.0f, 1, false);
    pathEditEnd(&st, shapes);
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{100, 0}, 4.0f, 1, false);
    pathEditEnd(&st, shapes);
    pathEditEndOpenPath(&st);
    check(!pathEditHasOpenPath(st), "the path is abandoned, as Escape leaves it");

    // --- resume from the TAIL: nothing to reverse -------------------------
    {
      std::vector<VectorShape> tailCase = shapes;
      PathEditState ts = st;
      const PenPressResult r =
          pathEditBeginPen(&ts, &tailCase, &nextId, PathPoint{100, 0}, 4.0f, 1, false);
      check(r == PenPressResult::Resumed,
            "RESUME: a press on the subpath's LAST anchor resumes without reversing, so "
            "nothing is recorded");
      check(pathEditHasOpenPath(ts), "RESUME: placement is open again");
      check(ptNear(tailCase[0].path.subpaths[0].anchors[0].pt, PathPoint{0, 0}),
            "RESUME: the anchor order is untouched when no reversal was needed");
      // The next press must extend at (100,0)'s end.
      pathEditEnd(&ts, tailCase);
      pathEditBeginPen(&ts, &tailCase, &nextId, PathPoint{200, 0}, 4.0f, 1, false);
      check(tailCase[0].path.subpaths[0].anchors.size() == 3 &&
                ptNear(tailCase[0].path.subpaths[0].anchors[2].pt, PathPoint{200, 0}),
            "RESUME: the next point extends from the end that was pressed");
    }

    // --- resume from the HEAD: the subpath reverses -----------------------
    {
      std::vector<VectorShape> headCase = shapes;
      PathEditState hs = st;
      const PenPressResult r =
          pathEditBeginPen(&hs, &headCase, &nextId, PathPoint{0, 0}, 4.0f, 1, false);
      check(r == PenPressResult::ResumedReversed,
            "RESUME: a press on the subpath's FIRST anchor reports ResumedReversed, so the "
            "caller records the reversal rather than leaving a silent geometry change");
      check(ptNear(headCase[0].path.subpaths[0].anchors[0].pt, PathPoint{100, 0}) &&
                ptNear(headCase[0].path.subpaths[0].anchors[1].pt, PathPoint{0, 0}),
            "RESUME: the subpath reversed, putting the pressed end at the back");
      pathEditEnd(&hs, headCase);
      pathEditBeginPen(&hs, &headCase, &nextId, PathPoint{-100, 0}, 4.0f, 1, false);
      check(headCase[0].path.subpaths[0].anchors.size() == 3 &&
                ptNear(headCase[0].path.subpaths[0].anchors[2].pt, PathPoint{-100, 0}),
            "RESUME: the next point extends from the pressed END, not from the other one -- "
            "the assertion the reversal exists for");
    }

    // --- a CLOSED subpath has no loose end --------------------------------
    {
      std::vector<VectorShape> closedCase = shapes;
      closedCase[0].path.subpaths[0].closed = true;
      PathEditState cs = st;
      const PenPressResult r =
          pathEditBeginPen(&cs, &closedCase, &nextId, PathPoint{0, 0}, 4.0f, 1, false);
      check(r == PenPressResult::Inert,
            "RESUME: a closed subpath has no loose end, so pressing its anchors is inert");
      check(!pathEditHasOpenPath(cs), "RESUME: ...and opens no placement session");
    }
  }

  // =======================================================================
  // 10. The Pen does not manipulate -- every gesture it used to inherit
  // =======================================================================
  //
  // Section 8 above covers the "clicked away" case. This one is the general
  // claim: the four hit kinds that `pathEditBegin()` turns into drags are all
  // inert under the Pen. Asserted per-KIND rather than once, because the
  // failure this guards is a single `case` arm creeping back into
  // `pathEditBeginPen()` -- which one assertion over one hit kind would not
  // see.
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 7;
    SubPath sub;
    sub.closed = true;  // closed: no loose ends, so nothing here can resume
    for (PathPoint p : {PathPoint{0, 0}, PathPoint{100, 0}, PathPoint{100, 100}}) {
      Anchor a;
      a.pt = p;
      a.in = PathPoint{p.x - 10, p.y};
      a.out = PathPoint{p.x + 10, p.y};
      shapes[0].path.subpaths.push_back(SubPath{});
      shapes[0].path.subpaths.back().anchors.push_back(a);
      shapes[0].path.subpaths.pop_back();
      sub.anchors.push_back(a);
    }
    shapes[0].path.subpaths.push_back(sub);

    // Select every anchor, so tangent handles are hit-testable too (they are
    // drawn, and tested, only for SELECTED anchors -- docs/vector-editing.md
    // section 3).
    uint64_t nextId = 50;
    PathEditState st;
    st.selection.mode = PathSelectMode::Component;
    for (uint32_t i = 0; i < 3; ++i)
      st.selection.components.push_back(ComponentRef{7, 0, i, AnchorPart::Point});

    struct Case {
      PathPoint at;
      const char* what;
    };
    const Case kCases[] = {
        {{100, 0}, "an anchor"},
        {{110, 0}, "a tangent handle"},
        {{50, 0}, "a path segment"},
    };
    for (const Case& c : kCases) {
      std::vector<VectorShape> copy = shapes;
      PathEditState ps = st;
      const uint64_t before = vectorContentHash(copy);
      const PenPressResult r =
          pathEditBeginPen(&ps, &copy, &nextId, c.at, 6.0f, 1, false);
      const bool inert = r == PenPressResult::Inert && ps.drag == PathDragKind::None &&
                         vectorContentHash(copy) == before;
      std::string label = "the Pen is INERT on ";
      label += c.what;
      label += " -- no drag, no geometry change (PathSelect's gesture, not the Pen's)";
      check(inert, label.c_str());
    }
  }

  return ok;
}

}  // namespace np
