#include "app/selftest/Support.hpp"

#include "app/PathOps.hpp"

namespace np {

// app/PathOps -- the PATHS panel's verbs (docs/path-editing-plan.md section
// 1), plus the two `core/Path` primitives promoted for them.
//
// Headless and GPU-free. Writes no files. Touches no `ui/` file and no
// `AppState`: every case below builds `std::vector<VectorShape>` and a
// `PathSelection` directly, exactly as `app/selftest/PenTool.cpp` does.
//
// **What this section is written to catch, stated up front because it shaped
// the assertions.** Three of these verbs have a failure mode that a
// count-based check sails straight past:
//
//   * `reverseSubPath()` without the handle swap produces a subpath with the
//     same anchor count, the same anchor positions and the same bounds -- and
//     a different curve. Section 1 asserts handle POSITIONS.
//   * `Join` with the wrong endpoint normalisation produces the right anchor
//     count in an order that makes the path cross itself. Section 4 asserts
//     the anchor ORDER for all four endpoint combinations, by position.
//   * `splitSegmentAt()` with an arithmetic slip still inserts one anchor in
//     the right place -- and moves the curve. Section 6 asserts the curve is
//     UNCHANGED by evaluating both halves against the original cubic.
bool runPathOpsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; };
  auto ptNear = [&](PathPoint a, PathPoint b, float tol = 1e-4f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };
  auto anchor = [](PathPoint pt, PathPoint in, PathPoint out) {
    Anchor a;
    a.pt = pt;
    a.in = in;
    a.out = out;
    return a;
  };

  // One shape holding one open subpath through `pts`, with every handle set
  // to a value DISTINCT from its own anchor, so a verb that loses or
  // mis-assigns a handle cannot hide behind `in == out == pt`.
  auto lineShape = [&](uint64_t id, std::vector<PathPoint> pts) {
    VectorShape s;
    s.id = id;
    SubPath sub;
    sub.closed = false;
    for (const PathPoint& p : pts)
      sub.anchors.push_back(anchor(p, PathPoint{p.x - 1.0f, p.y}, PathPoint{p.x + 1.0f, p.y}));
    s.path.subpaths.push_back(sub);
    return s;
  };

  auto componentSel = [](std::vector<ComponentRef> refs) {
    PathSelection sel;
    sel.mode = PathSelectMode::Component;
    sel.components = std::move(refs);
    return sel;
  };
  auto shapeSel = [](std::vector<uint64_t> ids) {
    PathSelection sel;
    sel.mode = PathSelectMode::Shape;
    sel.shapes = std::move(ids);
    return sel;
  };

  uint64_t nextId = 100;

  // =======================================================================
  // 1. reverseSubPath() -- the handle swap, which the order flip is not
  // =======================================================================
  //
  // REQUIRED (docs/path-editing-plan.md section 1.2): `in` controls the
  // segment ARRIVING at an anchor and `out` the one LEAVING it, so reversing
  // direction swaps them. Asserting only the anchor ORDER here would pass
  // against `std::reverse(anchors)` alone -- which renders a visibly
  // different curve. Sabotage: delete the swap loop in core/Path.cpp and
  // exactly these two lines must redden.
  {
    SubPath sub;
    sub.anchors = {anchor({0, 0}, {-1, 0}, {1, 0}), anchor({10, 0}, {9, 0}, {11, 0}),
                   anchor({20, 0}, {19, 0}, {21, 0})};
    reverseSubPath(sub);
    check(ptNear(sub.anchors[0].pt, {20, 0}) && ptNear(sub.anchors[2].pt, {0, 0}),
          "reverseSubPath(): anchor order flips");
    check(ptNear(sub.anchors[0].in, {21, 0}) && ptNear(sub.anchors[0].out, {19, 0}),
          "reverseSubPath(): the reversed head's handles SWAP (in<->out), not merely travel "
          "with their anchor -- the half a plain std::reverse would get wrong");
    check(ptNear(sub.anchors[1].in, {11, 0}) && ptNear(sub.anchors[1].out, {9, 0}),
          "reverseSubPath(): an interior anchor's handles swap too");

    SubPath lone;
    lone.anchors = {anchor({5, 5}, {4, 5}, {6, 5})};
    reverseSubPath(lone);
    check(ptNear(lone.anchors[0].in, {4, 5}) && ptNear(lone.anchors[0].out, {6, 5}),
          "reverseSubPath(): a one-anchor subpath is left alone -- no direction to reverse");
  }

  // =======================================================================
  // 2. fitAnchorTangent() -- one implementation, and it is opposite through
  //    the point by construction
  // =======================================================================
  {
    SubPath sub;
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({10, 0}, {10, 0}, {10, 0}),
                   anchor({20, 10}, {20, 10}, {20, 10})};
    fitAnchorTangent(&sub, 1, /*closed=*/false);
    // m = (next - prev)/2 = ((20,10)-(0,0))/2 = (10,5); handles are pt +/- m/3.
    check(ptNear(sub.anchors[1].out, {10.0f + 10.0f / 3.0f, 5.0f / 3.0f}) &&
              ptNear(sub.anchors[1].in, {10.0f - 10.0f / 3.0f, -5.0f / 3.0f}),
          "fitAnchorTangent(): uniform Catmull-Rom tangent, pt +/- m/3");
    check(sub.anchors[1].smooth, "fitAnchorTangent(): sets smooth");
    // The property `smooth = true` is supposed to MEAN: the two handles are
    // exactly opposite through the point. Checked as a midpoint identity
    // rather than by re-deriving the formula, so it tests the claim and not
    // the arithmetic a second time.
    const PathPoint mid{(sub.anchors[1].in.x + sub.anchors[1].out.x) * 0.5f,
                        (sub.anchors[1].in.y + sub.anchors[1].out.y) * 0.5f};
    check(ptNear(mid, sub.anchors[1].pt),
          "fitAnchorTangent(): in and out are opposite through pt -- the geometric content "
          "of `smooth`, asserted rather than assumed");
  }

  // =======================================================================
  // 3. CLOSE / OPEN / REVERSE, and the mixed-selection rule
  // =======================================================================
  {
    std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {10, 10}})};
    PathSelection sel = shapeSel({1});

    check(pathOpCanRun(PathOp::Close, shapes, sel) == PathOpRefusal::None,
          "CLOSE: runs on an open subpath");
    PathOpResult r = runPathOp(PathOp::Close, &shapes, &nextId, sel);
    check(r.changed && shapes[0].path.subpaths[0].closed, "CLOSE: sets closed");
    check(pathOpCanRun(PathOp::Close, shapes, sel) == PathOpRefusal::AlreadyClosed,
          "CLOSE: refuses AlreadyClosed the second time -- the greying the panel reads");

    // OPEN cuts BEFORE the selected anchor, by rotating it to index 0. No
    // anchor is duplicated (docs/path-editing-plan.md section 1: a repeated
    // vertex is a second representation of the same path).
    PathSelection cut = componentSel({ComponentRef{1, 0, 1, AnchorPart::Point}});
    r = runPathOp(PathOp::Open, &shapes, &nextId, cut);
    check(r.changed && !shapes[0].path.subpaths[0].closed, "OPEN: clears closed");
    check(shapes[0].path.subpaths[0].anchors.size() == 3,
          "OPEN: does not duplicate the cut anchor -- three anchors in, three out");
    check(ptNear(shapes[0].path.subpaths[0].anchors[0].pt, {10, 0}),
          "OPEN: the selected anchor becomes the new start, so the break falls before it");

    // A mixed selection closes what it can and leaves the rest, rather than
    // refusing the whole gesture because one member was already done.
    shapes.push_back(lineShape(2, {{0, 20}, {10, 20}, {10, 30}}));
    shapes[0].path.subpaths[0].closed = true;
    PathSelection both = shapeSel({1, 2});
    check(pathOpCanRun(PathOp::Close, shapes, both) == PathOpRefusal::None,
          "CLOSE: a mixed selection (one closed, one open) is not refused");
    r = runPathOp(PathOp::Close, &shapes, &nextId, both);
    check(r.changed && shapes[1].path.subpaths[0].closed,
          "CLOSE: closes the eligible member of a mixed selection");

    // A degenerate subpath has no segment to close, and the refusal says so
    // rather than reporting the wrong reason.
    std::vector<VectorShape> lone = {lineShape(9, {{1, 1}})};
    check(pathOpCanRun(PathOp::Close, lone, shapeSel({9})) == PathOpRefusal::DegenerateSubPath,
          "CLOSE: a one-anchor subpath refuses DegenerateSubPath, not AlreadyClosed");
  }

  // =======================================================================
  // 4. JOIN -- REQUIRED: all four endpoint combinations, asserted by ORDER
  // =======================================================================
  //
  // The four cases differ only in which end of each subpath was selected, and
  // the implementation normalises them with at most one reversal per side.
  // A normalisation slip yields the correct anchor COUNT in an order that
  // makes the joined path double back on itself -- invisible to a count, so
  // every case below reads the resulting positions in sequence.
  {
    // A: (0,0)-(10,0)   B: (20,0)-(30,0). Joining A's tail to B's head must
    // give 0,10,20,30 -- and each of the other three combinations must give
    // the sequence that puts the two SELECTED anchors adjacent in the middle.
    auto joinCase = [&](uint32_t aAnchor, uint32_t bAnchor, std::vector<PathPoint> want,
                        const char* what) {
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                         lineShape(2, {{20, 0}, {30, 0}})};
      PathSelection sel = componentSel({ComponentRef{1, 0, aAnchor, AnchorPart::Point},
                                        ComponentRef{2, 0, bAnchor, AnchorPart::Point}});
      const PathOpResult r = runPathOp(PathOp::Join, &shapes, &nextId, sel);
      if (!r.changed || shapes.size() != 1) return false;
      const SubPath& sub = shapes[0].path.subpaths[0];
      if (sub.anchors.size() != want.size()) return false;
      for (size_t i = 0; i < want.size(); ++i)
        if (!ptNear(sub.anchors[i].pt, want[i])) return false;
      (void)what;
      return true;
    };
    check(joinCase(1, 0, {{0, 0}, {10, 0}, {20, 0}, {30, 0}}, "tail-head"),
          "JOIN: tail-to-head appends in order (0,10,20,30)");
    check(joinCase(0, 0, {{10, 0}, {0, 0}, {20, 0}, {30, 0}}, "head-head"),
          "JOIN: head-to-head reverses the FIRST path (10,0,20,30)");
    check(joinCase(1, 1, {{0, 0}, {10, 0}, {30, 0}, {20, 0}}, "tail-tail"),
          "JOIN: tail-to-tail reverses the SECOND path (0,10,30,20)");
    check(joinCase(0, 1, {{10, 0}, {0, 0}, {30, 0}, {20, 0}}, "head-tail"),
          "JOIN: head-to-tail reverses BOTH (10,0,30,20)");

    // Two ends of ONE subpath is a close, not a concatenation -- and it must
    // not duplicate an anchor into the seam.
    {
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {10, 10}})};
      PathSelection sel = componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                        ComponentRef{1, 0, 2, AnchorPart::Point}});
      const PathOpResult r = runPathOp(PathOp::Join, &shapes, &nextId, sel);
      check(r.changed && shapes[0].path.subpaths[0].closed &&
                shapes[0].path.subpaths[0].anchors.size() == 3,
            "JOIN: the two ends of one subpath close it, adding no anchor");
      check(shapes.size() == 1 && r.erasedShapes.empty(),
            "JOIN: closing one subpath erases no shape");
    }

    // Cross-shape join consumes the second shape -- and reports the discard
    // only when the style was actually different.
    {
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                         lineShape(2, {{20, 0}, {30, 0}})};
      shapes[0].stroke.on = true;
      shapes[0].stroke.rgba = {1, 0, 0, 1};
      shapes[1].stroke.on = true;
      shapes[1].stroke.rgba = {0, 0, 1, 1};  // a DIFFERENT blue
      PathSelection sel = componentSel({ComponentRef{1, 0, 1, AnchorPart::Point},
                                        ComponentRef{2, 0, 0, AnchorPart::Point}});
      const PathOpResult r = runPathOp(PathOp::Join, &shapes, &nextId, sel);
      check(shapes.size() == 1 && shapes[0].id == 1,
            "JOIN: across shapes, the earlier shape survives");
      check(shapes[0].stroke.rgba[0] == 1.0f,
            "JOIN: the survivor keeps ITS paint, not the donor's");
      check(r.erasedShapes.size() == 1 && r.erasedShapes[0] == 2,
            "JOIN: the consumed shape's id is reported for selection pruning");
      check(r.discardedShapeStyle,
            "JOIN: discardedShapeStyle is raised when the donor's paint actually differed");
    }
    {
      // Same geometry, IDENTICAL styles: nothing was lost, so the panel must
      // not be told to warn. A flag that fires every time is a flag nobody
      // reads.
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                         lineShape(2, {{20, 0}, {30, 0}})};
      PathSelection sel = componentSel({ComponentRef{1, 0, 1, AnchorPart::Point},
                                        ComponentRef{2, 0, 0, AnchorPart::Point}});
      const PathOpResult r = runPathOp(PathOp::Join, &shapes, &nextId, sel);
      check(r.changed && !r.discardedShapeStyle,
            "JOIN: identical styles do not raise discardedShapeStyle");
    }

    // The refusals, in the order the header specifies them.
    {
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {20, 0}})};
      check(pathOpCanRun(PathOp::Join, shapes, shapeSel({1})) == PathOpRefusal::WrongSelectMode,
            "JOIN: Shape mode refuses WrongSelectMode -- mode before arity, so the user is "
            "told the useful thing");
      check(pathOpCanRun(PathOp::Join, shapes,
                         componentSel({ComponentRef{1, 0, 0, AnchorPart::Point}})) ==
                PathOpRefusal::NeedsTwoAnchors,
            "JOIN: one anchor refuses NeedsTwoAnchors");
      check(pathOpCanRun(PathOp::Join, shapes,
                         componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                       ComponentRef{1, 0, 1, AnchorPart::Point}})) ==
                PathOpRefusal::NotAnEndpoint,
            "JOIN: an interior anchor refuses NotAnEndpoint");
      shapes[0].path.subpaths[0].closed = true;
      check(pathOpCanRun(PathOp::Join, shapes,
                         componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                       ComponentRef{1, 0, 2, AnchorPart::Point}})) ==
                PathOpRefusal::NotAnEndpoint,
            "JOIN: a CLOSED subpath has no loose end -- NotAnEndpoint");
    }

    // The three `part` values name the same knot, so a selection of one
    // anchor's point AND its two handles is still ONE anchor -- not three,
    // which would refuse NeedsTwoAnchors against a perfectly good join.
    {
      std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                         lineShape(2, {{20, 0}, {30, 0}})};
      PathSelection sel = componentSel({ComponentRef{1, 0, 1, AnchorPart::Point},
                                        ComponentRef{1, 0, 1, AnchorPart::InHandle},
                                        ComponentRef{1, 0, 1, AnchorPart::OutHandle},
                                        ComponentRef{2, 0, 0, AnchorPart::Point}});
      check(pathOpCanRun(PathOp::Join, shapes, sel) == PathOpRefusal::None,
            "JOIN: an anchor selected together with both its handles counts once");
    }
  }

  // =======================================================================
  // 5. SMOOTH / CORNER / BREAK -- three verbs, and CORNER is not BREAK
  // =======================================================================
  {
    std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {20, 10}})};
    PathSelection sel = componentSel({ComponentRef{1, 0, 1, AnchorPart::Point}});

    std::vector<VectorShape> corner = shapes;
    runPathOp(PathOp::Corner, &corner, &nextId, sel);
    const Anchor& ca = corner[0].path.subpaths[0].anchors[1];
    check(!ca.smooth && ptNear(ca.in, ca.pt) && ptNear(ca.out, ca.pt),
          "CORNER: clears smooth AND collapses both handles onto the point");

    std::vector<VectorShape> broke = shapes;
    runPathOp(PathOp::Break, &broke, &nextId, sel);
    const Anchor& ba = broke[0].path.subpaths[0].anchors[1];
    check(!ba.smooth && ptNear(ba.in, {9, 0}) && ptNear(ba.out, {11, 0}),
          "BREAK: clears smooth and leaves the handles exactly where they were -- the verb "
          "CORNER is not");

    std::vector<VectorShape> smoothed = shapes;
    runPathOp(PathOp::Smooth, &smoothed, &nextId, sel);
    const Anchor& sa = smoothed[0].path.subpaths[0].anchors[1];
    const PathPoint mid{(sa.in.x + sa.out.x) * 0.5f, (sa.in.y + sa.out.y) * 0.5f};
    check(sa.smooth && ptNear(mid, sa.pt),
          "SMOOTH: sets smooth and makes the handles opposite through the point");

    // Shape mode means every knot -- "smooth this path" is a sentence.
    std::vector<VectorShape> all = shapes;
    runPathOp(PathOp::Smooth, &all, &nextId, shapeSel({1}));
    check(all[0].path.subpaths[0].anchors[0].smooth &&
              all[0].path.subpaths[0].anchors[2].smooth,
          "SMOOTH: Shape mode smooths every anchor of the shape, not just a selected one");
  }

  // =======================================================================
  // 6. INSERT -- REQUIRED: the curve does not move
  // =======================================================================
  //
  // A de Casteljau split reproduces the original cubic exactly as two halves.
  // Asserting only "one more anchor, in the right slot" would pass against
  // an insertion that rounds the shape off -- which is what "add a point
  // here" must never do. So this evaluates the ORIGINAL cubic at several t
  // and checks the split path passes through the same places.
  {
    // A curve with real handles, not a straight line -- a line survives most
    // arithmetic slips and would prove nothing.
    VectorShape s;
    s.id = 1;
    SubPath sub;
    sub.closed = false;
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 30}), anchor({30, 0}, {30, 30}, {30, 0})};
    s.path.subpaths.push_back(sub);

    auto cubicAt = [](const PathPoint p[4], float t) {
      const float u = 1.0f - t;
      return PathPoint{u * u * u * p[0].x + 3 * u * u * t * p[1].x + 3 * u * t * t * p[2].x +
                           t * t * t * p[3].x,
                       u * u * u * p[0].y + 3 * u * u * t * p[1].y + 3 * u * t * t * p[2].y +
                           t * t * t * p[3].y};
    };
    PathPoint orig[4];
    subPathSegment(s.path.subpaths[0], 0, orig);
    const PathPoint atQuarter = cubicAt(orig, 0.25f);
    const PathPoint atHalf = cubicAt(orig, 0.5f);
    const PathPoint atThreeQ = cubicAt(orig, 0.75f);

    std::vector<VectorShape> shapes = {s};
    PathSelection sel = componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                      ComponentRef{1, 0, 1, AnchorPart::Point}});
    check(pathOpCanRun(PathOp::InsertAnchor, shapes, sel) == PathOpRefusal::None,
          "INSERT: two adjacent anchors name the segment between them");
    const PathOpResult r = runPathOp(PathOp::InsertAnchor, &shapes, &nextId, sel);
    const SubPath& out = shapes[0].path.subpaths[0];
    check(r.changed && out.anchors.size() == 3, "INSERT: adds exactly one anchor");
    check(ptNear(out.anchors[1].pt, atHalf),
          "INSERT: the new anchor lands ON the original curve at t=0.5");

    // The two halves must reproduce the original: the first half at t=0.5 is
    // the original at t=0.25, and the second half at t=0.5 is the original at
    // t=0.75. This is the assertion that fails if the split moves the shape.
    PathPoint firstHalf[4], secondHalf[4];
    subPathSegment(out, 0, firstHalf);
    subPathSegment(out, 1, secondHalf);
    check(ptNear(cubicAt(firstHalf, 0.5f), atQuarter, 1e-3f) &&
              ptNear(cubicAt(secondHalf, 0.5f), atThreeQ, 1e-3f),
          "INSERT: the curve is UNCHANGED -- both halves still pass through the original's "
          "own points, so inserting a point does not redraw the user's shape");

    // Non-adjacent anchors refuse rather than splitting some other segment.
    std::vector<VectorShape> three = {lineShape(1, {{0, 0}, {10, 0}, {20, 0}})};
    check(pathOpCanRun(PathOp::InsertAnchor, three,
                       componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                     ComponentRef{1, 0, 2, AnchorPart::Point}})) ==
              PathOpRefusal::NotAdjacent,
          "INSERT: non-adjacent anchors refuse NotAdjacent");
    // ...unless the subpath is CLOSED, where first and last ARE adjacent
    // across the implied closing segment.
    three[0].path.subpaths[0].closed = true;
    check(pathOpCanRun(PathOp::InsertAnchor, three,
                       componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                     ComponentRef{1, 0, 2, AnchorPart::Point}})) ==
              PathOpRefusal::None,
          "INSERT: on a CLOSED subpath the first and last anchors are adjacent across the "
          "closing segment");
    runPathOp(PathOp::InsertAnchor, &three, &nextId,
              componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                            ComponentRef{1, 0, 2, AnchorPart::Point}}));
    check(three[0].path.subpaths[0].anchors.size() == 4 &&
              ptNear(three[0].path.subpaths[0].anchors[3].pt, {10, 0}),
          "INSERT: the seam's new anchor goes at the END, not before anchor 0 -- inserting "
          "at the front would renumber every anchor");
  }

  // =======================================================================
  // 7. DELETE -- the cascade, and the one-anchor subpath that must survive
  // =======================================================================
  {
    // **FOUR anchors, deleting 0 and 2 -- and the fixture size is the
    // assertion.** With three anchors this case cannot fail: erasing
    // ascending runs the second erase at `end()`, which degenerates to a
    // `pop_back()` and lands on the same survivor the descending walk
    // reaches. A sabotage flipping the loop direction left the suite fully
    // green, which is what sent this fixture back for a fourth anchor.
    //
    //   descending (correct)  erase 2 -> [0,10,30]; erase 0 -> [10,30]
    //   ascending  (wrong)    erase 0 -> [10,20,30]; erase 2 -> [10,20]
    //
    // The two differ in the SECOND survivor, so that is what is read.
    std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {20, 0}, {30, 0}})};
    PathSelection sel = componentSel({ComponentRef{1, 0, 0, AnchorPart::Point},
                                      ComponentRef{1, 0, 2, AnchorPart::Point}});
    const PathOpResult r = runPathOp(PathOp::DeleteAnchor, &shapes, &nextId, sel);
    check(r.changed && shapes.size() == 1 &&
              shapes[0].path.subpaths[0].anchors.size() == 2 &&
              ptNear(shapes[0].path.subpaths[0].anchors[0].pt, {10, 0}) &&
              ptNear(shapes[0].path.subpaths[0].anchors[1].pt, {30, 0}),
          "DELETE: two anchors removed in one call leave the correct survivors -- the "
          "descending walk, on a fixture that can tell the two directions apart");
    check(r.erasedShapes.empty(), "DELETE: a shape that still has anchors is not erased");

    // A subpath left with ONE anchor survives: that is the state the Pen's
    // own first press creates, so erasing it here would make "place a point,
    // delete its neighbour" destroy the shape.
    const PathOpResult r2 = runPathOp(PathOp::DeleteAnchor, &shapes, &nextId,
                                      componentSel({ComponentRef{1, 0, 1, AnchorPart::Point}}));
    check(r2.changed && shapes.size() == 1 &&
              shapes[0].path.subpaths[0].anchors.size() == 1 && r2.erasedShapes.empty(),
          "DELETE: a subpath left with ONE anchor survives -- it is the state the Pen's own "
          "first press creates");

    // Emptying the last subpath erases the shape and reports its id.
    const PathOpResult r3 = runPathOp(PathOp::DeleteAnchor, &shapes, &nextId,
                                      componentSel({ComponentRef{1, 0, 0, AnchorPart::Point}}));
    check(r3.changed && shapes.empty() && r3.erasedShapes.size() == 1 &&
              r3.erasedShapes[0] == 1,
          "DELETE: the last anchor cascades subpath -> shape, and the erased id is reported "
          "so the caller can prune its selection");
  }

  // =======================================================================
  // 8. COMPOUND / RELEASE -- round trip, and the ids
  // =======================================================================
  {
    std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                       lineShape(2, {{0, 20}, {10, 20}}),
                                       lineShape(3, {{0, 40}, {10, 40}})};
    check(pathOpCanRun(PathOp::MakeCompound, shapes, shapeSel({1})) ==
              PathOpRefusal::NeedsTwoShapes,
          "COMPOUND: one shape refuses NeedsTwoShapes");
    const PathOpResult r = runPathOp(PathOp::MakeCompound, &shapes, &nextId, shapeSel({1, 2, 3}));
    check(r.changed && shapes.size() == 1 && shapes[0].id == 1 &&
              shapes[0].path.subpaths.size() == 3,
          "COMPOUND: three shapes become one shape of three subpaths, the earliest surviving");
    check(r.erasedShapes.size() == 2, "COMPOUND: both consumed ids are reported");
    // Subpath order follows the shapes' own order, so a compound is not a
    // reshuffle the user has to re-derive.
    check(ptNear(shapes[0].path.subpaths[1].anchors[0].pt, {0, 20}) &&
              ptNear(shapes[0].path.subpaths[2].anchors[0].pt, {0, 40}),
          "COMPOUND: subpaths keep the shapes' original order");

    const uint64_t before = nextId;
    const PathOpResult rr = runPathOp(PathOp::ReleaseCompound, &shapes, &nextId, shapeSel({1}));
    check(rr.changed && shapes.size() == 3, "RELEASE: three subpaths become three shapes");
    check(nextId == before + 2 && rr.createdShapes.size() == 2,
          "RELEASE: mints exactly two NEW ids from the layer's counter -- the original keeps "
          "its own, so no id is reused");
    check(shapes[0].id == 1 && shapes[1].id != shapes[2].id,
          "RELEASE: the released pieces get distinct ids");
    check(shapes[1].path.subpaths.size() == 1 && shapes[2].path.subpaths.size() == 1,
          "RELEASE: each piece carries exactly one subpath");
    check(pathOpCanRun(PathOp::ReleaseCompound, shapes, shapeSel({1})) ==
              PathOpRefusal::NotCompound,
          "RELEASE: a single-contour shape refuses NotCompound");
  }

  // =======================================================================
  // 9. Every refusal has a sentence, and every verb has an edit name
  // =======================================================================
  //
  // A switch that has grown an enumerator without a case falls through to a
  // default and says nothing, which is how a button ends up silently refusing
  // with a blank status line. Walked rather than spot-checked, so adding a
  // refusal without its sentence fails here rather than in front of a user.
  {
    const PathOpRefusal kRefusals[] = {
        PathOpRefusal::EmptySelection,  PathOpRefusal::WrongSelectMode,
        PathOpRefusal::NeedsTwoAnchors, PathOpRefusal::NotAnEndpoint,
        PathOpRefusal::AlreadyClosed,   PathOpRefusal::AlreadyOpen,
        PathOpRefusal::DegenerateSubPath, PathOpRefusal::NotAdjacent,
        PathOpRefusal::NeedsTwoShapes,  PathOpRefusal::NotCompound,
        PathOpRefusal::StaleSelection};
    bool allSaid = true;
    for (PathOpRefusal r : kRefusals)
      if (pathOpRefusalText(r) == nullptr || pathOpRefusalText(r)[0] == '\0') allSaid = false;
    check(allSaid, "every PathOpRefusal but None has a non-empty sentence");
    check(pathOpRefusalText(PathOpRefusal::None)[0] == '\0',
          "PathOpRefusal::None's sentence is empty, so a caller may display it "
          "unconditionally");

    const PathOp kOps[] = {PathOp::Close,        PathOp::Open,         PathOp::Join,
                           PathOp::Reverse,      PathOp::Smooth,       PathOp::Corner,
                           PathOp::Break,        PathOp::InsertAnchor, PathOp::DeleteAnchor,
                           PathOp::MakeCompound, PathOp::ReleaseCompound};
    bool allNamed = true;
    for (PathOp op : kOps) {
      const char* n = pathOpEditName(op);
      if (n == nullptr || n[0] == '\0') allNamed = false;
      // The undo entry's name is what HISTORY shows, and this build's
      // convention everywhere else is lower-case present tense.
      if (n != nullptr && n[0] >= 'A' && n[0] <= 'Z') allNamed = false;
    }
    check(allNamed, "every PathOp has a lower-case undo-entry name");

    // An empty selection refuses before anything else, for every verb --
    // including the ones whose own precondition would otherwise report
    // something more specific and more confusing.
    const std::vector<VectorShape> none;
    bool allEmpty = true;
    for (PathOp op : kOps)
      if (pathOpCanRun(op, none, PathSelection{}) != PathOpRefusal::EmptySelection)
        allEmpty = false;
    check(allEmpty, "every PathOp refuses EmptySelection first when nothing is selected");
  }

  // =======================================================================
  // 10. runPathOp() never corrupts on a refusal
  // =======================================================================
  //
  // The header promises a caller may run a verb without checking first. That
  // promise is only worth having if a refused verb leaves the geometry byte
  // for byte as it was -- checked through `vectorContentHash()`, which is the
  // build's own answer to "did this change?".
  {
    std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}, {20, 0}})};
    const uint64_t before = vectorContentHash(shapes);
    const PathOpResult r =
        runPathOp(PathOp::Join, &shapes, &nextId, shapeSel({1}));  // WrongSelectMode
    check(!r.changed && r.refusal == PathOpRefusal::WrongSelectMode &&
              vectorContentHash(shapes) == before,
          "a refused verb leaves the geometry untouched -- checked by content hash, not by "
          "eye");
  }

  return ok;
}

}  // namespace np
