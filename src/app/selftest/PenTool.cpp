#include "app/selftest/Support.hpp"

#include "app/PenTool.hpp"
#include "ops/Transform.hpp"
#include "ui/AtelierChrome.hpp"  // toolImplemented(), toolHasCanvasHandler() -- read-only check

namespace np {

// app/PenTool: the headless core of Stage 4's vector editing
// (docs/vector-editing.md), written before any of this code and cited by
// section number throughout.
//
// Headless and GPU-free. Writes no files. Touches no `ui/` file, and does
// not exercise `AppState` at all -- `PathEditState` lives there but this
// section builds `PathSelection`/`std::vector<VectorShape>` directly, the
// same way app/selftest/MoveTool.cpp exercises app/MoveTool.hpp without a
// running application.
bool runPenToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; };
  auto ptNear = [&](PathPoint a, PathPoint b, float tol = 1e-4f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };

  // A subpath through `pts`, every handle set explicitly (never defaulted to
  // the anchor's own point) so a test's geometry says exactly what it means --
  // core/Path.hpp section 2's "handles are absolute" is the property under
  // test in section 2 below, and leaving them implicit here would hide it.
  auto anchor = [](PathPoint pt, PathPoint in, PathPoint out) {
    Anchor a;
    a.pt = pt;
    a.in = in;
    a.out = out;
    return a;
  };

  // =======================================================================
  // 1. shapePivot() -- nullopt tracks the centroid, a stored value does not
  // =======================================================================
  {
    // A 10x10 axis-aligned square, straight edges (handles coincide with
    // their own anchors, core/Path.hpp section 1), so `pathTightBounds()` is
    // exactly the anchor bounds and the expected centroid is hand-computable.
    auto square = [&](PathPoint c0, PathPoint c1, PathPoint c2, PathPoint c3) {
      VectorShape s;
      s.id = 1;
      SubPath sub;
      sub.closed = true;
      sub.anchors = {anchor(c0, c0, c0), anchor(c1, c1, c1), anchor(c2, c2, c2),
                     anchor(c3, c3, c3)};
      s.path.subpaths.push_back(sub);
      return s;
    };

    VectorShape unset = square({0, 0}, {10, 0}, {10, 10}, {0, 10});
    check(ptNear(shapePivot(unset), PathPoint{5, 5}),
          "shapePivot(): nullopt returns the tight-bounds centroid (a 10x10 square -> (5,5))");

    VectorShape placed = square({0, 0}, {10, 0}, {10, 10}, {0, 10});
    placed.pivot = PathPoint{5, 5};  // placed AT the centroid, deliberately
    check(ptNear(shapePivot(placed), PathPoint{5, 5}),
          "shapePivot(): a pivot placed at the centroid reads back the same value as nullopt "
          "-- so far indistinguishable, which is exactly why the next check is required");

    // Now edit both shapes' geometry the same way (stretch one corner from
    // (10,10) to (20,10)) and re-read. The nullopt shape's answer MUST
    // follow the new bounds; the placed one MUST NOT -- core/VectorShape.hpp's
    // whole point: "must survive the shape later being edited into a
    // different centroid."
    unset.path.subpaths[0].anchors[2] = anchor({20, 10}, {20, 10}, {20, 10});
    placed.path.subpaths[0].anchors[2] = anchor({20, 10}, {20, 10}, {20, 10});
    check(ptNear(shapePivot(unset), PathPoint{10, 5}),
          "shapePivot(): nullopt recomputes after the shape is edited (bounds now 0..20 x "
          "0..10 -> (10,5))");
    check(ptNear(shapePivot(placed), PathPoint{5, 5}),
          "shapePivot(): a USER-PLACED pivot stays put after the same edit -- distinct from "
          "nullopt even though both read (5,5) before the edit");
  }

  // =======================================================================
  // 2. componentPivot() -- mean of the selected anchors' points
  // =======================================================================
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 7;
    SubPath sub;
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({10, 0}, {10, 0}, {10, 0}),
                   anchor({10, 10}, {10, 10}, {10, 10})};
    shapes[0].path.subpaths.push_back(sub);

    // Select anchors 0 and 2, skipping 1 -- mean of (0,0) and (10,10) is
    // (5,5), which would be wrong if the skipped anchor were counted.
    std::vector<ComponentRef> sel = {ComponentRef{7, 0, 0, AnchorPart::Point},
                                      ComponentRef{7, 0, 2, AnchorPart::Point}};
    check(ptNear(componentPivot(shapes, sel), PathPoint{5, 5}),
          "componentPivot(): mean of the selected anchors' points, skipped anchors excluded");
    check(ptNear(componentPivot(shapes, {}), PathPoint{0, 0}),
          "componentPivot(): an empty selection returns the origin rather than dividing by "
          "zero");
  }

  // =======================================================================
  // 3. applyAffineToSelection() -- REQUIRED: an anchor's point and both its
  //    tangent handles move together, and an unselected neighbour's handle
  //    (even one that numerically points AT the selected anchor) does not.
  // =======================================================================
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 42;
    SubPath sub;
    sub.closed = false;
    // C2.in is set to EXACTLY C1.pt -- the concrete case docs/vector-editing
    // .md section 7 names: "the neighbour's handle points AT the selected
    // anchor and does not move." If applyAffineToSelection() ever scoped by
    // proximity instead of by selection membership, this is the value that
    // would betray it by moving when it must not.
    sub.anchors = {
        anchor(/*C0*/ {0, 0}, {0, 0}, {10, 0}),
        anchor(/*C1*/ {20, 0}, {15, 0}, {25, 0}),
        anchor(/*C2*/ {40, 0}, /*in points AT C1.pt*/ {20, 0}, {40, 0}),
    };
    shapes[0].path.subpaths.push_back(sub);

    PathSelection sel;
    sel.mode = PathSelectMode::Component;
    sel.components = {ComponentRef{42, 0, 1, AnchorPart::Point}};  // C1 only

    applyAffineToSelection(&shapes, sel, transformTranslate(100.0f, 50.0f));
    const SubPath& after = shapes[0].path.subpaths[0];

    check(ptNear(after.anchors[1].pt, {120, 50}) && ptNear(after.anchors[1].in, {115, 50}) &&
              ptNear(after.anchors[1].out, {125, 50}),
          "applyAffineToSelection() [component]: REQUIRED -- the selected anchor's point AND "
          "both its tangent handles move together, by hand-computed geometry");
    check(ptNear(after.anchors[0].pt, {0, 0}) && ptNear(after.anchors[0].in, {0, 0}) &&
              ptNear(after.anchors[0].out, {10, 0}),
          "applyAffineToSelection() [component]: the unselected C0 is untouched");
    check(ptNear(after.anchors[2].pt, {40, 0}) && ptNear(after.anchors[2].out, {40, 0}),
          "applyAffineToSelection() [component]: the unselected C2's own point and out-handle "
          "are untouched");
    check(ptNear(after.anchors[2].in, {20, 0}),
          "applyAffineToSelection() [component]: REQUIRED -- C2.in, which numerically pointed "
          "AT C1's old position, stays exactly there rather than following C1 -- it belongs to "
          "an anchor that was never in the selected set");
  }

  // Pivot asymmetry (docs/vector-editing.md section 1): Shape mode carries
  // the pivot, Component mode does not; a nullopt pivot is left alone either
  // way (there is nothing to transform, and the centroid it stands for
  // recomputes for free from the moved geometry).
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 9;
    shapes[0].pivot = PathPoint{10, 10};
    SubPath sub;
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({20, 20}, {20, 20}, {20, 20})};
    shapes[0].path.subpaths.push_back(sub);

    PathSelection shapeSel;
    shapeSel.mode = PathSelectMode::Shape;
    shapeSel.shapes = {9};
    applyAffineToSelection(&shapes, shapeSel, transformTranslate(5.0f, 5.0f));
    check(shapes[0].pivot.has_value() && ptNear(*shapes[0].pivot, {15, 15}),
          "applyAffineToSelection() [shape]: a stored pivot moves WITH the shape");

    PathSelection compSel;
    compSel.mode = PathSelectMode::Component;
    compSel.components = {ComponentRef{9, 0, 0, AnchorPart::Point}};
    applyAffineToSelection(&shapes, compSel, transformTranslate(5.0f, 5.0f));
    check(shapes[0].pivot.has_value() && ptNear(*shapes[0].pivot, {15, 15}),
          "applyAffineToSelection() [component]: the shape's stored pivot is NOT touched");

    std::vector<VectorShape> noPivotShapes(1);
    noPivotShapes[0].id = 11;
    SubPath sub2;
    sub2.anchors = {anchor({0, 0}, {0, 0}, {0, 0})};
    noPivotShapes[0].path.subpaths.push_back(sub2);
    PathSelection s2;
    s2.mode = PathSelectMode::Shape;
    s2.shapes = {11};
    applyAffineToSelection(&noPivotShapes, s2, transformTranslate(3.0f, 4.0f));
    check(!noPivotShapes[0].pivot.has_value(),
          "applyAffineToSelection() [shape]: a nullopt pivot is left alone -- nothing to "
          "transform, and shapePivot() will recompute the moved centroid on demand");
  }

  {
    VectorShape s;
    s.id = 3;
    setShapePivot(&s, PathPoint{1, 2});
    check(s.pivot.has_value() && ptNear(*s.pivot, {1, 2}),
          "setShapePivot(): sets the stored pivot and edits no geometry");
  }

  // =======================================================================
  // 4. Selection combine -- the four SelectionCombine rules, as SET
  //    operations, on both selection kinds (docs/vector-editing.md section 4)
  // =======================================================================
  {
    std::vector<uint64_t> shapesSel = {1, 2, 3};
    combineShapeSelection(&shapesSel, {3, 4}, SelectionCombine::Replace);
    check(shapesSel == std::vector<uint64_t>{3, 4}, "combineShapeSelection(): Replace");

    shapesSel = {1, 2, 3};
    combineShapeSelection(&shapesSel, {3, 4}, SelectionCombine::Add);
    check(shapesSel == std::vector<uint64_t>{1, 2, 3, 4}, "combineShapeSelection(): Add (union)");

    shapesSel = {1, 2, 3};
    combineShapeSelection(&shapesSel, {2, 3, 4}, SelectionCombine::Subtract);
    check(shapesSel == std::vector<uint64_t>{1}, "combineShapeSelection(): Subtract (difference)");

    shapesSel = {1, 2, 3};
    combineShapeSelection(&shapesSel, {2, 3, 4}, SelectionCombine::Intersect);
    check(shapesSel == std::vector<uint64_t>{2, 3}, "combineShapeSelection(): Intersect");

    shapesSel = {5, 5, 5};
    combineShapeSelection(&shapesSel, {5}, SelectionCombine::Add);
    check(shapesSel == std::vector<uint64_t>{5},
          "combineShapeSelection(): Add is idempotent -- a duplicated set unions to one entry, "
          "the same idempotence core/SelectionOps.hpp states for the coverage rules");

    const ComponentRef c1{1, 0, 0, AnchorPart::Point};
    const ComponentRef c2{1, 0, 1, AnchorPart::Point};
    const ComponentRef c3{2, 0, 0, AnchorPart::Point};
    check(c1 == c1 && !(c1 == c2), "ComponentRef::operator==: exact field equality");

    std::vector<ComponentRef> compSel = {c1, c2};
    combineComponentSelection(&compSel, {c2, c3}, SelectionCombine::Replace);
    check(compSel.size() == 2 &&
              std::find(compSel.begin(), compSel.end(), c2) != compSel.end() &&
              std::find(compSel.begin(), compSel.end(), c3) != compSel.end(),
          "combineComponentSelection(): Replace");

    compSel = {c1, c2};
    combineComponentSelection(&compSel, {c2, c3}, SelectionCombine::Add);
    check(compSel.size() == 3, "combineComponentSelection(): Add (union) has all three refs");

    compSel = {c1, c2};
    combineComponentSelection(&compSel, {c2}, SelectionCombine::Subtract);
    check(compSel.size() == 1 && compSel[0] == c1, "combineComponentSelection(): Subtract");

    compSel = {c1, c2};
    combineComponentSelection(&compSel, {c2, c3}, SelectionCombine::Intersect);
    check(compSel.size() == 1 && compSel[0] == c2, "combineComponentSelection(): Intersect");
  }

  // =======================================================================
  // 5. componentsInRect() / shapesIntersectingRect() -- boundary cases
  // =======================================================================
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 5;
    SubPath sub;
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({10, 0}, {10, 0}, {10, 0}),
                   anchor({10, 10}, {10, 10}, {10, 10})};
    shapes[0].path.subpaths.push_back(sub);

    const PathBounds exact{true, 0, 0, 10, 10};
    std::vector<ComponentRef> in = componentsInRect(shapes, exact);
    check(in.size() == 3, "componentsInRect(): a rect exactly on all three anchors' bounds "
                          "includes all of them -- inclusive edges");

    // All three anchors sit exactly on `exact`'s own boundary (two corners
    // and one edge-midpoint-by-construction), so shrinking the rect by a
    // hair on every side excludes all three -- pinning that a boundary point
    // is included via <= / >=, not a fuzzier "nearby" test.
    const PathBounds justInside{true, 0.001f, 0.001f, 9.999f, 9.999f};
    check(componentsInRect(shapes, justInside).empty(),
          "componentsInRect(): shrinking the rect by a hair off every side excludes all three "
          "boundary-sitting anchors");

    const PathBounds empty{false, 0, 0, 0, 0};
    check(componentsInRect(shapes, empty).empty(),
          "componentsInRect(): an invalid (empty) rect contains nothing");

    const PathBounds disjoint{true, 100, 100, 200, 200};
    check(shapesIntersectingRect(shapes, disjoint).empty(),
          "shapesIntersectingRect(): a rect nowhere near the shape's bounds finds nothing");

    const PathBounds overlapping{true, 5, 5, 15, 15};
    const std::vector<uint64_t> hitShapes = shapesIntersectingRect(shapes, overlapping);
    check(hitShapes.size() == 1 && hitShapes[0] == 5,
          "shapesIntersectingRect(): a rect overlapping the shape's control bounds finds it");
  }

  // =======================================================================
  // 5b. EVERY position the gnomon is DRAWN at is a position it can be HIT
  //     at -- the drawn/hit contract, and the reason `gnomonReachPx` is
  //     threaded through `pathEditBegin()` rather than defaulted.
  // =======================================================================
  //
  // `gnomonHandlePositions()`' own header says it is "what the caller should
  // read to draw the gnomon rather than a second, independently derived
  // geometry", and `ui/MacPaintUI.cpp`'s overlay now does exactly that. But
  // the overlay and the hit test are two separate call sites passing two
  // separate `reachPx` arguments, and until the overlay existed nothing in
  // the tree could tell whether they agreed: the gnomon was hit-tested and
  // never drawn, so a mismatch had no symptom.
  //
  // It has one now, and it is a bad one: a handle painted where nothing is
  // grabbable, or worse, a grab that starts a transform from a point with no
  // handle on it. So this asserts the contract directly.
  {
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 31;
    SubPath sub;
    sub.closed = true;
    // A real box, not a degenerate point: the corners must be four DISTINCT
    // positions or "every corner is hittable" is one assertion wearing four
    // hats.
    sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({100, 0}, {100, 0}, {100, 0}),
                   anchor({100, 80}, {100, 80}, {100, 80}), anchor({0, 80}, {0, 80}, {0, 80})};
    shapes[0].path.subpaths.push_back(sub);

    PathSelection sel;
    sel.mode = PathSelectMode::Shape;
    sel.shapes = {31};

    // Deliberately NOT `kDefaultGnomonReachPx`. The bug this section guards
    // against is one call site using the default while the other zoom-corrects,
    // and a test written at the default is green under exactly that bug.
    const float reach = 17.0f;
    const GnomonHandlePositions g = gnomonHandlePositions(shapes, sel, reach);
    check(g.valid, "gnomonHandlePositions(): a shape-mode selection of one shape has a gnomon");

    struct Spot { const char* name; PathPoint at; };
    const Spot spots[] = {
        {"free-move centre", g.center},   {"+X axis tip", g.axisXTip},
        {"+Y axis tip", g.axisYTip},      {"corner 0", g.corners[0]},
        {"corner 1", g.corners[1]},       {"corner 2", g.corners[2]},
        {"corner 3", g.corners[3]},
    };
    bool everySpotHits = true;
    const char* missed = "";
    for (const Spot& sp : spots) {
      // `pickRadiusPx` deliberately tiny: this must pass because the GNOMON
      // is there, not because a generous pick radius swept up the shape
      // underneath it.
      const PathHit h = hitTestPath(shapes, sel, sp.at, /*pickRadiusPx=*/0.5f,
                                    /*gnomonSuppressed=*/false,
                                    /*pivotMoveModeActive=*/false, reach);
      if (h.kind != PathHitKind::GnomonHandle) {
        everySpotHits = false;
        missed = sp.name;
      }
    }
    check(everySpotHits,
          "REQUIRED -- every position gnomonHandlePositions() reports is a GnomonHandle hit at "
          "the same reach, so the overlay cannot draw a handle nothing can grab");
    if (!everySpotHits) std::printf("    [selftest] first miss: %s\n", missed);

    // --- SABOTAGE PROOF: the assertion above is about the reach AGREEING,
    // not merely about the gnomon being large ---------------------------
    //
    // Run the identical loop with the hit test given a DIFFERENT reach --
    // which is precisely the defect (one call site zoom-corrects, the other
    // takes the default) -- and require that at least one drawn position now
    // misses. Without this, the check above would still pass if
    // `gnomonReachPx` were ignored entirely.
    {
      bool someSpotMissesAtWrongReach = false;
      for (const Spot& sp : spots) {
        const PathHit h = hitTestPath(shapes, sel, sp.at, 0.5f, false, false,
                                      /*a different reach=*/reach * 4.0f);
        if (h.kind != PathHitKind::GnomonHandle) someSpotMissesAtWrongReach = true;
      }
      check(someSpotMissesAtWrongReach,
            "SABOTAGE PROOF: with the hit test given a reach the positions were NOT computed "
            "at, at least one drawn handle stops being grabbable -- so the check above is "
            "testing the agreement and not just the gnomon's size");
    }

    // --- and the same contract through `pathEditBegin()`, which is the
    // function `ui/` actually calls -------------------------------------
    //
    // **This case exists because the two above did not catch the defect.**
    // They test `hitTestPath()` directly; the plumbing the overlay depends on
    // is `pathEditBegin()` FORWARDING its `gnomonReachPx` to it, and that
    // parameter was added in the same change as the overlay. Making
    // `pathEditBegin()` drop the argument and pass `kDefaultGnomonReachPx`
    // -- the exact defect, and the state this function was in before the
    // overlay existed -- left the suite entirely green. The sabotage found
    // that, reading the test did not, and the fix was to the ASSERTION.
    //
    // Asserted through `state.drag` rather than a return value: a gnomon hit
    // is `PathDragKind::Manipulator`, and no other tier produces it, so this
    // cannot pass by landing on the shape underneath.
    {
      PathEditState st{};
      st.selection = sel;
      const bool geometryDrag =
          pathEditBegin(&st, shapes, g.axisXTip, /*pickRadiusPx=*/0.5f,
                        /*gnomonSuppressed=*/false, SelectionCombine::Replace,
                        /*documentId=*/1, reach);
      check(geometryDrag && st.drag == PathDragKind::Manipulator,
            "REQUIRED -- pathEditBegin() FORWARDS gnomonReachPx: a pen-down on the +X axis tip "
            "computed at that reach starts a Manipulator drag, which is the only tier a gnomon "
            "hit produces");

      PathEditState wrong{};
      wrong.selection = sel;
      pathEditBegin(&wrong, shapes, g.axisXTip, 0.5f, false, SelectionCombine::Replace, 1,
                    /*a different reach=*/reach * 4.0f);
      check(wrong.drag != PathDragKind::Manipulator,
            "SABOTAGE PROOF: the same pen-down at a reach the tip was NOT computed at does NOT "
            "start a Manipulator drag -- so the check above proves the argument is read, not "
            "merely accepted");
    }
  }

  // =======================================================================
  // 6. Hit-test priority -- REQUIRED: exactly docs/vector-editing.md section
  //    3's order, and gnomonSuppressed makes the next tier reachable.
  // =======================================================================
  {
    // 6a. Gnomon beats an exactly-coincident anchor; suppressing the gnomon
    // makes that anchor reachable.
    std::vector<VectorShape> shapes(1);
    shapes[0].id = 21;
    shapes[0].pivot = PathPoint{50, 50};
    SubPath sub;
    sub.anchors = {anchor({50, 50}, {50, 50}, {50, 50})};
    shapes[0].path.subpaths.push_back(sub);

    PathSelection sel;
    sel.mode = PathSelectMode::Shape;
    sel.shapes = {21};

    const PathPoint at{50, 50};
    PathHit hit = hitTestPath(shapes, sel, at, /*pickRadiusPx=*/10.0f, /*gnomonSuppressed=*/false);
    check(hit.kind == PathHitKind::GnomonHandle,
          "hitTestPath(): REQUIRED -- the gnomon (tier 1) wins over an exactly-coincident "
          "anchor and pivot");

    hit = hitTestPath(shapes, sel, at, 10.0f, /*gnomonSuppressed=*/true);
    check(hit.kind == PathHitKind::Anchor && hit.shapeId == 21 && hit.component.anchor == 0,
          "hitTestPath(): REQUIRED -- gnomonSuppressed makes the NEXT tier down (the anchor) "
          "reachable at the same point");

    // 6b. The pivot marker (tier 2) beats the anchor (tier 3), but only
    // while pivot-move mode is active -- it sits at the same point as the
    // gnomon's own free-move handle, so it is only reachable once the
    // gnomon itself is out of the way.
    hit = hitTestPath(shapes, sel, at, 10.0f, /*gnomonSuppressed=*/true,
                       /*pivotMoveModeActive=*/true);
    check(hit.kind == PathHitKind::PivotMarker,
          "hitTestPath(): the pivot marker (tier 2) beats the anchor (tier 3) when pivot-move "
          "mode is active");
    hit = hitTestPath(shapes, sel, at, 10.0f, /*gnomonSuppressed=*/true,
                       /*pivotMoveModeActive=*/false);
    check(hit.kind == PathHitKind::Anchor,
          "hitTestPath(): the same point falls through to the anchor when pivot-move mode is "
          "NOT active");

    // 6c. Anchor (tier 3) beats tangent (tier 4): click exactly on a
    // selected anchor whose own out-handle also lies within the pick radius.
    std::vector<VectorShape> shapes2(1);
    shapes2[0].id = 22;
    SubPath sub2;
    sub2.closed = false;
    sub2.anchors = {anchor(/*B0*/ {0, 0}, {0, 0}, {5, 0}), anchor(/*B1*/ {100, 0}, {95, 0}, {100, 0})};
    shapes2[0].path.subpaths.push_back(sub2);

    PathSelection compSel;
    compSel.mode = PathSelectMode::Component;
    compSel.components = {ComponentRef{22, 0, 0, AnchorPart::Point}};  // B0 selected

    hit = hitTestPath(shapes2, compSel, PathPoint{0, 0}, /*pickRadiusPx=*/6.0f,
                       /*gnomonSuppressed=*/true);
    check(hit.kind == PathHitKind::Anchor && hit.component.anchor == 0,
          "hitTestPath(): the anchor (tier 3) beats its own tangent handle (tier 4) at "
          "distance 5, well inside the same 6-unit pick radius");

    // 6d. Tangent (tier 4) beats segment (tier 5): B0's straight handles
    // (0,0)-(5,0)-(95,0)-(100,0) are collinear, so the cubic IS the straight
    // line y=0 -- the segment passes through (5,0) exactly, the same point
    // as B0's own out-handle. With B0 selected, the tangent tier fires
    // first; with the selection empty, no tangent handles are drawn at all
    // (section 3: "drawn only for selected anchors") and the segment tier is
    // what answers instead.
    hit = hitTestPath(shapes2, compSel, PathPoint{5, 0}, /*pickRadiusPx=*/2.0f,
                       /*gnomonSuppressed=*/true);
    check(hit.kind == PathHitKind::Tangent && hit.component.part == AnchorPart::OutHandle,
          "hitTestPath(): the tangent (tier 4) beats the segment (tier 5) at a point that is "
          "exactly on both");

    PathSelection emptyCompSel;
    emptyCompSel.mode = PathSelectMode::Component;
    hit = hitTestPath(shapes2, emptyCompSel, PathPoint{5, 0}, 2.0f, /*gnomonSuppressed=*/true);
    check(hit.kind == PathHitKind::Segment && hit.shapeId == 22,
          "hitTestPath(): with no selected anchors there is no tangent to beat, and the same "
          "point falls through to the segment (tier 5)");

    // 6e. Nothing at all -> None, the marquee cue.
    hit = hitTestPath(shapes2, emptyCompSel, PathPoint{1000, 1000}, 2.0f,
                       /*gnomonSuppressed=*/true);
    check(hit.kind == PathHitKind::None,
          "hitTestPath(): a point nowhere near anything answers None -- the caller's cue to "
          "start a marquee");
  }

  // =======================================================================
  // 7. toolEditsPath() -- the gate, and pathToolPlacesAnchors() -- the split
  // =======================================================================
  //
  // **Two predicates asserted together, because one without the other is the
  // defect.** `toolEditsPath()` widened to three tools when
  // `Tool::PathSelect` landed; on its own that widening would have handed the
  // Pen's placement to Path Select and Path Select's manipulator to the Pen,
  // since the canvas block used to route on the HIT rather than the tool.
  // `pathToolPlacesAnchors()` is the routing split that makes the widening
  // safe, so a change that reverted either one alone must redden here.
  {
    bool exactlyThree = true;
    bool placesExactlyTwo = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const bool edits = (t == Tool::Pen || t == Tool::Curve || t == Tool::PathSelect);
      const bool places = (t == Tool::Pen || t == Tool::Curve);
      if (toolEditsPath(t) != edits) exactlyThree = false;
      if (pathToolPlacesAnchors(t) != places) placesExactlyTwo = false;
    }
    check(exactlyThree,
          "toolEditsPath(): true for exactly Pen, Curve and PathSelect -- the three tools "
          "that author or edit the same anchor model -- and false for every other Tool, "
          "Tool::Shape included");
    check(placesExactlyTwo,
          "pathToolPlacesAnchors(): true for exactly Pen and Curve -- PathSelect edits "
          "geometry and never creates an anchor");
    check(toolEditsPath(Tool::PathSelect) && !pathToolPlacesAnchors(Tool::PathSelect),
          "PathSelect is gated IN by toolEditsPath() and OUT by pathToolPlacesAnchors(): the "
          "two predicates disagreeing about it is exactly what makes the canvas block route "
          "it to the editing gestures rather than to placement");
    // Named explicitly, the way app/selftest/Eyedropper.cpp names Move
    // against toolPansView(): the tool most likely to be confused for this
    // one is Shape, which also produces vector geometry but is not gated by
    // this predicate because it does not go through anchor-level editing.
    check(!toolEditsPath(Tool::Shape) && !toolEditsPath(Tool::Text),
          "toolEditsPath(): Shape and Text are NOT path-editing tools, named explicitly since "
          "they are the two easiest to mistake for one");
    // **Wired, and the pairing is the assertion.** This predicate used to be
    // pinned as deliberately NOT in `toolHasCanvasHandler()`, because the
    // headless model landed before the canvas block that uses it. Both landed
    // together in one commit, and they have to: the eyedropper's tripwire (its
    // section 6) asserts `toolImplemented(t) == toolHasCanvasHandler(t)` for
    // every tool and separately asserts `toolNoHandlerException()` is empty, so
    // flipping either half alone turns the suite red -- and the tempting
    // repair is a row in the table asserted to have none.
    check(toolImplemented(Tool::Pen) && toolHasCanvasHandler(Tool::Pen),
          "toolEditsPath(): Tool::Pen is implemented AND has a canvas handler -- the two "
          "halves flipped together, which is the only way the tool tables stay consistent");
    check(toolImplemented(Tool::Curve) && toolHasCanvasHandler(Tool::Curve),
          "toolEditsPath(): so is Tool::Curve, which shares the gate and the flyout");
  }

  // =======================================================================
  // 8. pathEditSetSelectMode() -- the mode switch CARRIES the selection
  // =======================================================================
  //
  // The options bar's mode segment, and the only writer of
  // `PathSelection::mode`. What is asserted here is the property that makes
  // it a mode toggle rather than a deselect: switching does not throw the
  // user's selection away. Dropping it would look like a rounding bug at the
  // UI -- the shapes stay on screen, only their chrome vanishes -- which is
  // exactly the class of silent behaviour this suite exists to pin.
  {
    // Two shapes, so "carries the selection across" is distinguishable from
    // "selects everything": only shape 7 is ever selected below, and shape 8
    // must stay out of the result at every step.
    auto square = [&](uint64_t id, float x, float y) {
      VectorShape s;
      SubPath sub;
      sub.closed = true;
      const PathPoint pts[4] = {{x, y}, {x + 10, y}, {x + 10, y + 10}, {x, y + 10}};
      for (const PathPoint& p : pts) sub.anchors.push_back(anchor(p, p, p));
      s.path.subpaths.push_back(sub);
      s.id = id;
      return s;
    };
    const std::vector<VectorShape> shapes{square(7, 0, 0), square(8, 100, 100)};

    PathEditState st;
    st.selection.mode = PathSelectMode::Shape;
    st.selection.shapes = {7};

    pathEditSetSelectMode(&st, PathSelectMode::Component, shapes);
    check(st.selection.mode == PathSelectMode::Component,
          "pathEditSetSelectMode(): Shape -> Component switches the mode");
    check(st.selection.shapes.empty() && st.selection.components.size() == 4,
          "pathEditSetSelectMode(): ...carrying the selection across as every anchor of the "
          "shapes that were selected -- four, for the one selected square, and NOT the eight "
          "of both squares");
    bool allFromSeven = true;
    for (const ComponentRef& c : st.selection.components)
      if (c.shapeId != 7 || c.part != AnchorPart::Point) allFromSeven = false;
    check(allFromSeven,
          "pathEditSetSelectMode(): ...every carried component is an anchor POINT of shape 7, "
          "the shape that was selected -- not a handle, and not the unselected shape 8");
    check(ptNear(st.componentPivot, PathPoint{5, 5}),
          "pathEditSetSelectMode(): ...and the transient pivot is recomputed for the new "
          "component selection rather than left where the old mode put it");

    pathEditSetSelectMode(&st, PathSelectMode::Shape, shapes);
    check(st.selection.mode == PathSelectMode::Shape && st.selection.components.empty() &&
              st.selection.shapes.size() == 1 && st.selection.shapes[0] == 7,
          "pathEditSetSelectMode(): Component -> Shape carries back to the ONE shape those "
          "four anchors belong to -- de-duplicated, not one entry per anchor");

    // A live drag is abandoned, and it has to be: `shapesAtDragStart` was
    // captured against the OTHER mode's selection, so letting the next
    // `pathEditUpdate()` run would apply that affine to a selection the user
    // did not have at pen-down.
    st.selection.mode = PathSelectMode::Shape;
    st.selection.shapes = {7};
    const bool began =
        pathEditBegin(&st, shapes, PathPoint{5, 0}, 2.0f, /*gnomonSuppressed=*/true,
                      SelectionCombine::Replace, /*documentId=*/1);
    check(began && st.drag == PathDragKind::Manipulator,
          "pathEditSetSelectMode(): (setup) a press on a selected shape's segment starts a "
          "Manipulator drag");
    pathEditSetSelectMode(&st, PathSelectMode::Component, shapes);
    check(st.drag == PathDragKind::None && st.shapesAtDragStart.empty(),
          "pathEditSetSelectMode(): ...and switching mode mid-drag abandons it, snapshot and "
          "all -- the affine was captured against the other mode's selection");

    // Idempotence: setting the mode it is already in is a no-op, not a
    // selection round trip. Without this, an options bar that re-asserts the
    // current mode every frame -- which is what an ImGui segmented control
    // does -- would rebuild the selection on every frame of the session.
    st.selection.mode = PathSelectMode::Component;
    st.selection.components.clear();
    st.selection.components.push_back(ComponentRef{7, 0, 2, AnchorPart::Point});
    pathEditSetSelectMode(&st, PathSelectMode::Component, shapes);
    check(st.selection.components.size() == 1 && st.selection.components[0].anchor == 2,
          "pathEditSetSelectMode(): setting the mode already in effect leaves the selection "
          "exactly as it was -- an options bar re-asserting it every frame must not rebuild "
          "it every frame");
  }

  // =======================================================================
  // 9. gnomonHandleAffine() and the Manipulator dispatch -- REQUIRED: the
  //    scale corners and the rotate ring actually scale and rotate, per
  //    handle recorded at pathEditBegin() and read back by
  //    pathEditUpdate()'s Manipulator arm. Before this track,
  //    pathEditUpdate() applied `transformTranslate()` to EVERY Manipulator
  //    drag because nothing recorded which handle was pressed once the drag
  //    kind was set (`app/PenTool.cpp`, ca35341's line 864).
  // =======================================================================
  {
    // A 100x100 square, pivot explicitly at its centre (50,50) so the
    // expected geometry below is hand-computable independently of
    // shapePivot()'s own centroid arithmetic (section 1 already covers
    // that). Straight-edged (every handle coincides with its own anchor,
    // core/Path.hpp section 1) so `pathTightBounds()` is exactly the anchor
    // bounds -- 9h/9i/9j below read the gnomon's drawn corner and segment
    // positions, which a curved edge would move off the hand-computed
    // values. 9a2 below tests the tangent-follows-anchor property in
    // isolation, on a shape built for exactly that and nothing else, so an
    // intentionally skewed handle there cannot perturb these tests' bounds.
    auto square = [&]() {
      VectorShape s;
      s.id = 101;
      s.pivot = PathPoint{50, 50};
      SubPath sub;
      sub.closed = true;
      sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({100, 0}, {100, 0}, {100, 0}),
                     anchor({100, 100}, {100, 100}, {100, 100}),
                     anchor({0, 100}, {0, 100}, {0, 100})};
      s.path.subpaths.push_back(sub);
      return s;
    };

    PathSelection sel;
    sel.mode = PathSelectMode::Shape;
    sel.shapes = {101};
    const PathPoint pivot{50, 50};

    // Independent oracle for a scale: `pivot + factor * (p - pivot)`, plain
    // arithmetic rather than `Mat3`/`transformScaleAbout()`'s matrix
    // multiply -- an expectation built the same way the code under test
    // builds its answer could share its bug.
    auto scaledAbout = [](PathPoint p, PathPoint piv, float sx, float sy) {
      return PathPoint{piv.x + sx * (p.x - piv.x), piv.y + sy * (p.y - piv.y)};
    };

    // ---- 9a. Corner, free (no Shift): independent per-axis ratio --------
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};
      st.selection = sel;
      st.documentId = 1;
      st.drag = PathDragKind::Manipulator;
      st.gnomonHandle = GnomonPart::Corner;
      st.dragStart = PathPoint{0, 0};  // corner 0; offset from pivot (-50,-50)
      st.shapesAtDragStart = shapes;

      // current = (-100,0): offset (-150,-50) -> sx = -150/-50 = 3, sy = -50/-50 = 1.
      const PathEditChange change =
          pathEditUpdate(&st, &shapes, PathPoint{-100, 0}, /*shiftHeld=*/false);
      check(change == PathEditChange::EditBegan,
            "Manipulator/Corner: the first frame that moves the pointer opens the edit");

      const SubPath& sub = shapes[0].path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, scaledAbout({0, 0}, pivot, 3, 1)),
            "Manipulator/Corner (free): REQUIRED -- the dragged corner lands exactly on the "
            "pointer (sx=3, sy=1 from a (-100,0) drag)");
      check(ptNear(sub.anchors[2].pt, scaledAbout({100, 100}, pivot, 3, 1)),
            "Manipulator/Corner (free): the OPPOSITE corner scales by the same sx/sy about the "
            "same pivot");
      check(ptNear(sub.anchors[1].pt, scaledAbout({100, 0}, pivot, 3, 1)),
            "Manipulator/Corner (free): the third corner (100,0) scales by the same sx/sy "
            "about the same pivot");
      check(shapes[0].pivot.has_value() && ptNear(*shapes[0].pivot, pivot),
            "Manipulator/Corner (free): a Shape-mode scale about its OWN pivot leaves that "
            "pivot exactly where it was -- it is the centre, not a point being scaled");
    }

    // ---- 9a2. Tangent handles transform WITH their anchor -- isolated on a
    //          shape built for exactly this, since a skewed handle would
    //          otherwise perturb pathTightBounds() and the gnomon geometry
    //          9h/9i/9j below read.
    {
      std::vector<VectorShape> shapes(1);
      shapes[0].id = 303;
      shapes[0].pivot = PathPoint{0, 0};
      SubPath sub;
      // Point, in-handle and out-handle all distinct, so a scale that moved
      // the point but not its handles -- or by the wrong ratio -- has
      // something to betray it on (docs/vector-editing.md section 7: an
      // affine applies to all three of an anchor's points together).
      sub.anchors = {anchor(/*pt*/ {10, 10}, /*in*/ {8, 9}, /*out*/ {12, 11})};
      shapes[0].path.subpaths.push_back(sub);

      PathSelection sel303;
      sel303.mode = PathSelectMode::Shape;
      sel303.shapes = {303};
      const PathPoint pivot303{0, 0};

      PathEditState st{};
      st.selection = sel303;
      st.documentId = 1;
      st.drag = PathDragKind::Manipulator;
      st.gnomonHandle = GnomonPart::Corner;
      st.dragStart = PathPoint{10, 10};  // offset from pivot (0,0): (10,10)
      st.shapesAtDragStart = shapes;

      // current = (30,20): offset (30,20) -> sx = 3, sy = 2.
      pathEditUpdate(&st, &shapes, PathPoint{30, 20}, /*shiftHeld=*/false);
      const Anchor& a = shapes[0].path.subpaths[0].anchors[0];
      check(ptNear(a.pt, scaledAbout({10, 10}, pivot303, 3, 2)) &&
                ptNear(a.in, scaledAbout({8, 9}, pivot303, 3, 2)) &&
                ptNear(a.out, scaledAbout({12, 11}, pivot303, 3, 2)),
            "Manipulator/Corner: REQUIRED -- an anchor's point AND both its tangent handles "
            "move by the SAME affine (section 7: the affine applies to all three points "
            "together), exercised through the real Manipulator dispatch rather than "
            "applyAffineToSelection() directly");
    }

    // ---- 9b. Corner, Shift held: locked to the RADIAL distance ratio ----
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};
      st.selection = sel;
      st.documentId = 1;
      st.drag = PathDragKind::Manipulator;
      st.gnomonHandle = GnomonPart::Corner;
      st.dragStart = PathPoint{0, 0};
      st.shapesAtDragStart = shapes;

      // Same two points as 9a, Shift held this time: s = |current-pivot| /
      // |dragStart-pivot| = sqrt(150^2+50^2) / sqrt(50^2+50^2) = sqrt(5).
      const float s = std::sqrt(5.0f);
      pathEditUpdate(&st, &shapes, PathPoint{-100, 0}, /*shiftHeld=*/true);
      const SubPath& sub = shapes[0].path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, scaledAbout({0, 0}, pivot, s, s), 1e-3f),
            "Manipulator/Corner (Shift=uniform): REQUIRED -- BOTH axes take the radial "
            "distance ratio sqrt(5) rather than either axis' own free ratio (3 and 1) -- the "
            "dragged corner does NOT land exactly on the pointer under Shift, which is the "
            "tell that the lock is really engaged and not a no-op");
      check(ptNear(sub.anchors[2].pt, scaledAbout({100, 100}, pivot, s, s), 1e-3f),
            "Manipulator/Corner (Shift=uniform): the opposite corner scales by the same "
            "locked factor");
    }

    // ---- 9c. AxisX / AxisY: single-axis scale, the other axis untouched,
    //          and Shift changes nothing (there is only one axis to lock) --
    {
      std::vector<VectorShape> shapesX{square()};
      PathEditState stX{};
      stX.selection = sel;
      stX.documentId = 1;
      stX.drag = PathDragKind::Manipulator;
      stX.gnomonHandle = GnomonPart::AxisX;
      stX.dragStart = PathPoint{90, 50};  // +X axis tip at reach 40 from pivot
      stX.shapesAtDragStart = shapesX;
      // offset from pivot (40,0) -> current (130,50), offset (80,0) -> sx=2.
      pathEditUpdate(&stX, &shapesX, PathPoint{130, 50}, /*shiftHeld=*/false);
      const SubPath& subX = shapesX[0].path.subpaths[0];
      check(ptNear(subX.anchors[0].pt, scaledAbout({0, 0}, pivot, 2, 1)) &&
                ptNear(subX.anchors[2].pt, scaledAbout({100, 100}, pivot, 2, 1)),
            "Manipulator/AxisX: REQUIRED -- scales X only (sx=2) and leaves Y untouched "
            "(sy=1), about the pivot");

      std::vector<VectorShape> shapesY{square()};
      PathEditState stY{};
      stY.selection = sel;
      stY.documentId = 1;
      stY.drag = PathDragKind::Manipulator;
      stY.gnomonHandle = GnomonPart::AxisY;
      stY.dragStart = PathPoint{50, 90};
      stY.shapesAtDragStart = shapesY;
      pathEditUpdate(&stY, &shapesY, PathPoint{50, 130}, /*shiftHeld=*/false);
      const SubPath& subY = shapesY[0].path.subpaths[0];
      check(ptNear(subY.anchors[0].pt, scaledAbout({0, 0}, pivot, 1, 2)) &&
                ptNear(subY.anchors[2].pt, scaledAbout({100, 100}, pivot, 1, 2)),
            "Manipulator/AxisY: symmetric case -- scales Y only (sy=2), X untouched");

      std::vector<VectorShape> shapesShift{square()};
      PathEditState stShift{};
      stShift.selection = sel;
      stShift.documentId = 1;
      stShift.drag = PathDragKind::Manipulator;
      stShift.gnomonHandle = GnomonPart::AxisX;
      stShift.dragStart = PathPoint{90, 50};
      stShift.shapesAtDragStart = shapesShift;
      pathEditUpdate(&stShift, &shapesShift, PathPoint{130, 50}, /*shiftHeld=*/true);
      check(ptNear(shapesShift[0].path.subpaths[0].anchors[0].pt, scaledAbout({0, 0}, pivot, 2, 1)),
            "Manipulator/AxisX: Shift held changes nothing -- \"uniform\" names a relationship "
            "between two axes, and an axis handle only ever reads one");
    }

    // ---- 9d. Rotate, exact 90 degrees: exact anchor AND tangent mapping --
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};
      st.selection = sel;
      st.documentId = 1;
      st.drag = PathDragKind::Manipulator;
      st.gnomonHandle = GnomonPart::Rotate;
      st.dragStart = PathPoint{150, 50};  // pivot + (100,0): bearing 0
      st.shapesAtDragStart = shapes;
      // current = pivot + (0,100): bearing 90 -> delta = +90 degrees.
      pathEditUpdate(&st, &shapes, PathPoint{50, 150}, /*shiftHeld=*/false);
      const SubPath& sub = shapes[0].path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, {100, 0}) && ptNear(sub.anchors[1].pt, {100, 100}) &&
                ptNear(sub.anchors[2].pt, {0, 100}) && ptNear(sub.anchors[3].pt, {0, 0}),
            "Manipulator/Rotate (90 deg, exact): REQUIRED -- the square's four corners map "
            "(0,0)->(100,0)->(100,100)->(0,100)->(0,0), a cyclic quarter turn about the pivot");
      // Tangent handles rotating WITH their anchor is 9a2's assertion, on a
      // shape built for it; this one is kept straight-edged (section 7's
      // free case, in == out == pt) so as not to also perturb the corner
      // positions 9h/9i/9j read from `gnomonHandlePositions()`.
    }

    // ---- 9e. Rotate, Shift snap: 37 degrees snaps to the NEAREST 15-degree
    //          step (30, not 45 -- |37-30|=7 < |37-45|=8). Tested directly
    //          against gnomonHandleAffine(), the pure function, rather than
    //          through pixel positions on a drawn ring.
    {
      constexpr float kPi = 3.14159265358979323846f;
      const float rawDegrees = 37.0f;
      const PathPoint dragStart{pivot.x + 100.0f, pivot.y};  // bearing 0, radius 100
      const PathPoint current{pivot.x + 100.0f * std::cos(rawDegrees * kPi / 180.0f),
                              pivot.y + 100.0f * std::sin(rawDegrees * kPi / 180.0f)};

      const Mat3 free =
          gnomonHandleAffine(GnomonPart::Rotate, pivot, dragStart, current, /*shiftHeld=*/false);
      const Point2 freeMapped = mat3MapPoint(free, Point2{dragStart.x, dragStart.y});
      check(near(freeMapped.x, current.x, 1e-2f) && near(freeMapped.y, current.y, 1e-2f),
            "gnomonHandleAffine(Rotate, shiftHeld=false): the exact 37-degree bearing change "
            "maps dragStart's own radius onto `current` exactly (both built at radius 100)");

      const Mat3 snapped =
          gnomonHandleAffine(GnomonPart::Rotate, pivot, dragStart, current, /*shiftHeld=*/true);
      const Point2 snappedMapped = mat3MapPoint(snapped, Point2{dragStart.x, dragStart.y});
      const PathPoint expected30{pivot.x + 100.0f * std::cos(30.0f * kPi / 180.0f),
                                 pivot.y + 100.0f * std::sin(30.0f * kPi / 180.0f)};
      check(near(snappedMapped.x, expected30.x, 1e-2f) &&
                near(snappedMapped.y, expected30.y, 1e-2f),
            "gnomonHandleAffine(Rotate, shiftHeld=true): REQUIRED -- 37 degrees snaps to 30, "
            "the NEAREST 15-degree step (|37-30|=7 < |37-45|=8), not to 45");
    }

    // ---- 9f. Degenerate guards: pointer on the pivot must not produce NaN
    //          or collapse the geometry -- refuse the axis/handle instead.
    {
      const PathPoint onPivot = pivot;
      const PathPoint elsewhere{80, 65};
      const Point2 probe{123.0f, 45.0f};

      const Mat3 cornerDegenerate =
          gnomonHandleAffine(GnomonPart::Corner, pivot, onPivot, elsewhere, false);
      const Point2 mappedCorner = mat3MapPoint(cornerDegenerate, probe);
      check(std::isfinite(mappedCorner.x) && std::isfinite(mappedCorner.y) &&
                near(mappedCorner.x, probe.x) && near(mappedCorner.y, probe.y),
            "gnomonHandleAffine(Corner): REQUIRED -- dragStart exactly on the pivot (a "
            "zero-length reference vector) refuses BOTH axes rather than dividing by ~0 -- an "
            "arbitrary probe point maps to itself (identity), finite, not NaN");

      const Mat3 axisDegenerate =
          gnomonHandleAffine(GnomonPart::AxisX, pivot, onPivot, elsewhere, false);
      const Point2 mappedAxis = mat3MapPoint(axisDegenerate, probe);
      check(std::isfinite(mappedAxis.x) && near(mappedAxis.x, probe.x) &&
                near(mappedAxis.y, probe.y),
            "gnomonHandleAffine(AxisX): the same degenerate dragStart refuses its one axis too");

      const Mat3 rotateDegenerateStart =
          gnomonHandleAffine(GnomonPart::Rotate, pivot, onPivot, elsewhere, false);
      const Point2 mappedRotStart = mat3MapPoint(rotateDegenerateStart, probe);
      check(std::isfinite(mappedRotStart.x) && near(mappedRotStart.x, probe.x) &&
                near(mappedRotStart.y, probe.y),
            "gnomonHandleAffine(Rotate): REQUIRED -- dragStart on the pivot has no bearing to "
            "read off -- refuses the WHOLE handle (identity) rather than feed atan2(0,0) into "
            "a spurious rotation");

      const Mat3 rotateDegenerateCurrent =
          gnomonHandleAffine(GnomonPart::Rotate, pivot, elsewhere, onPivot, false);
      const Point2 mappedRotCur = mat3MapPoint(rotateDegenerateCurrent, probe);
      check(std::isfinite(mappedRotCur.x) && near(mappedRotCur.x, probe.x) &&
                near(mappedRotCur.y, probe.y),
            "gnomonHandleAffine(Rotate): the same refusal when CURRENT, not dragStart, is the "
            "one sitting on the pivot");
    }

    // ---- 9g. Component mode: the manipulator scales about the TRANSIENT
    //          componentPivot, which may be user-placed -- NOT a fresh
    //          recompute of the selected anchors' centroid (section 1).
    {
      std::vector<VectorShape> shapes(1);
      shapes[0].id = 202;
      SubPath sub;
      sub.anchors = {anchor({0, 0}, {0, 0}, {0, 0}), anchor({20, 20}, {20, 20}, {20, 20})};
      shapes[0].path.subpaths.push_back(sub);

      PathSelection compSel;
      compSel.mode = PathSelectMode::Component;
      compSel.components = {ComponentRef{202, 0, 0, AnchorPart::Point},
                            ComponentRef{202, 0, 1, AnchorPart::Point}};
      // The selection's own centroid would be (10,10) -- componentPivot()'s
      // ordinary answer, already covered by section 2's test. This simulates
      // the user having placed the transient pivot somewhere else entirely:
      // pathEditRefreshPivot() would never overwrite it, since that
      // function's whole job is leaving a user-placed pivot alone.
      PathEditState st{};
      st.selection = compSel;
      st.documentId = 1;
      st.componentPivot = PathPoint{0, 0};
      st.componentPivotIsUserPlaced = true;
      st.drag = PathDragKind::Manipulator;
      st.gnomonHandle = GnomonPart::Corner;
      st.dragStart = PathPoint{20, 20};  // offset from the PLACED pivot (0,0): (20,20)
      st.shapesAtDragStart = shapes;

      // current = (40,40): offset (40,40) -> sx = sy = 2, about pivot (0,0).
      pathEditUpdate(&st, &shapes, PathPoint{40, 40}, /*shiftHeld=*/false);
      const SubPath& after = shapes[0].path.subpaths[0];
      check(ptNear(after.anchors[1].pt, {40, 40}),
            "Manipulator/Corner [component]: REQUIRED -- the dragged anchor lands where the "
            "pointer is, scaled about the PLACED pivot (0,0)");
      check(ptNear(after.anchors[0].pt, {0, 0}),
            "Manipulator/Corner [component]: REQUIRED -- the pivot-side anchor (0,0) stays put "
            "-- it sits AT the placed pivot, so scaling about it moves it nowhere. Scaling "
            "about the selection's own centroid (10,10) instead would have moved this anchor "
            "to (-10,-10), which is the sabotage this assertion is built to catch");
    }

    // ---- 9h. Translate (the free-move centre) is UNCHANGED, bit for bit --
    //          reached both via a Segment click (never touches the gnomon)
    //          and via a direct hit on the gnomon's own centre handle.
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};  // nothing selected yet -- the segment click selects it

      const bool began = pathEditBegin(&st, shapes, PathPoint{50, 0}, /*pickRadiusPx=*/2.0f,
                                       /*gnomonSuppressed=*/true, SelectionCombine::Replace,
                                       /*documentId=*/1);
      check(began && st.drag == PathDragKind::Manipulator && st.gnomonHandle == GnomonPart::Center,
            "pathEditBegin(): a Segment click starts a Manipulator drag with gnomonHandle == "
            "Center -- the whole-shape translate, never touching the gnomon");

      pathEditUpdate(&st, &shapes, PathPoint{53, 7});
      check(ptNear(shapes[0].path.subpaths[0].anchors[0].pt, {3, 7}) &&
                ptNear(shapes[0].path.subpaths[0].anchors[2].pt, {103, 107}),
            "Manipulator/Center (via Segment): REQUIRED -- translate by (dx,dy)=(3,7), exactly "
            "today's only behaviour, bit for bit");
    }
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};
      st.selection = sel;
      const GnomonHandlePositions g = gnomonHandlePositions(shapes, sel, 40.0f);
      const bool began = pathEditBegin(&st, shapes, g.center, /*pickRadiusPx=*/0.5f,
                                       /*gnomonSuppressed=*/false, SelectionCombine::Replace,
                                       /*documentId=*/1, 40.0f);
      check(began && st.drag == PathDragKind::Manipulator && st.gnomonHandle == GnomonPart::Center,
            "pathEditBegin(): a press on the gnomon's own free-move handle also records "
            "gnomonHandle == Center");
      pathEditUpdate(&st, &shapes, PathPoint{g.center.x + 12.0f, g.center.y - 4.0f});
      check(ptNear(shapes[0].path.subpaths[0].anchors[0].pt, {12, -4}),
            "Manipulator/Center (via the gnomon handle): the same translate, reached through "
            "the real hit test this time rather than a hand-set drag state");
    }

    // ---- 9i. hitTestPath() names the SPECIFIC gnomon sub-part it hit, not
    //          merely "some gnomon handle" -- and the CLOSEST one wins.
    // =======================================================================
    {
      std::vector<VectorShape> shapes{square()};
      const GnomonHandlePositions g = gnomonHandlePositions(shapes, sel, 40.0f);

      auto partAt = [&](PathPoint at, float pickRadiusPx = 0.5f) {
        return hitTestPath(shapes, sel, at, pickRadiusPx, /*gnomonSuppressed=*/false,
                           /*pivotMoveModeActive=*/false, 40.0f)
            .gnomonPart;
      };
      check(partAt(g.center) == GnomonPart::Center, "hitTestPath(): names the centre Center");
      check(partAt(g.axisXTip) == GnomonPart::AxisX, "hitTestPath(): names the +X tip AxisX");
      check(partAt(g.axisYTip) == GnomonPart::AxisY, "hitTestPath(): names the +Y tip AxisY");
      check(partAt(g.corners[0]) == GnomonPart::Corner &&
                partAt(g.corners[1]) == GnomonPart::Corner &&
                partAt(g.corners[2]) == GnomonPart::Corner &&
                partAt(g.corners[3]) == GnomonPart::Corner,
            "hitTestPath(): names every one of the four corners Corner -- they share one "
            "value (GnomonPart's own comment: all four scale about the same pivot, so which "
            "one was grabbed does not change the math)");
      const PathPoint onRing{g.center.x + g.rotateRingRadius, g.center.y};
      check(partAt(onRing) == GnomonPart::Rotate, "hitTestPath(): a point exactly on the ring "
                                                  "names Rotate");
      // A wide pick radius that also reaches the centre and a corner (both
      // farther away): the CLOSEST candidate wins, not whichever `consider()`
      // call happened to run first or last -- pinning that identity tracks
      // distance rather than iteration order.
      check(partAt(onRing, /*pickRadiusPx=*/3.0f) == GnomonPart::Rotate,
            "hitTestPath(): REQUIRED -- with a pick radius wide enough to also reach farther "
            "handles, the CLOSEST one (the ring, at distance 0) is still what is reported");
    }

    // ---- 9j. End to end: a real pen-down on a drawn corner, through the
    //          real hit test, starts a REAL scale. (9a-9c above prove the
    //          dispatch function in isolation; this proves the wiring FROM
    //          an actual press, the same way section 5b's contract test
    //          proves gnomonHandlePositions() and hitTestPath() agree.)
    {
      std::vector<VectorShape> shapes{square()};
      PathEditState st{};
      st.selection = sel;
      const GnomonHandlePositions g = gnomonHandlePositions(shapes, sel, 40.0f);
      const bool began = pathEditBegin(&st, shapes, g.corners[0], /*pickRadiusPx=*/0.5f,
                                       /*gnomonSuppressed=*/false, SelectionCombine::Replace,
                                       /*documentId=*/1, 40.0f);
      check(began && st.drag == PathDragKind::Manipulator && st.gnomonHandle == GnomonPart::Corner,
            "pathEditBegin(): a press on a drawn scale corner records gnomonHandle == Corner");
      // corner[0] is (0,0) (bounds minX,minY); drag to (-100,0), as in 9a.
      pathEditUpdate(&st, &shapes, PathPoint{-100, 0});
      const SubPath& sub = shapes[0].path.subpaths[0];
      check(ptNear(sub.anchors[0].pt, scaledAbout({0, 0}, pivot, 3, 1)),
            "end to end: the dragged corner lands exactly where the pointer is");
      // **The dragged corner alone cannot tell a scale from a translate**:
      // by construction it lands on the pointer either way (translate:
      // old + (current - dragStart); scale: the whole point of a corner
      // handle). The OPPOSITE corner is what only a real scale moves
      // correctly -- a translate would leave it at (100,100) untouched by
      // the pivot, which is what makes this the assertion that actually
      // distinguishes the two dispatches, not the one just above.
      check(ptNear(sub.anchors[2].pt, scaledAbout({100, 100}, pivot, 3, 1)),
            "REQUIRED -- end to end, from a real pen-down on the drawn corner through a real "
            "drag: the OPPOSITE corner scales about the pivot -- proving this is a scale, not "
            "a translate that happens to also pass the corner-lands-on-the-pointer check above");
    }
  }

  return ok;
}

}  // namespace np
