#include "app/selftest/Support.hpp"

#include "core/SelectionBoundary.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"

namespace np {

// ---------------------------------------------------------------------------
// core/SelectionBoundary -- the TRUE outline of a selection (PRD E6's marching
// ants; PLAN.md "Phase 7 -- Select and paste").
//
// **This section exists because a green suite once agreed with the bug.** The
// ants were drawn from `selectionBounds()`, which is exact for a rectangle and
// a bounding box for everything else, and while `selectRectangle()` was the
// only constructor that was harmless. PRD E3's lasso, polygon lasso and wand
// landed, and PRD E7's Shift-add landed, and nothing in the suite noticed that
// every selection had started drawing as a rectangle -- because every existing
// assertion was about the selection MODEL, which was right in every case.
// Four bug reports came back ("shift just draws another rectangle", "the lasso
// selects a rectangle", "the polygon lasso selects a rectangle", "the wand does
// nothing") and all four were the one picture.
//
// So the shape of this section is: **each assertion is one that a bounding box
// passes and a real boundary trace does not, or the reverse.** Section A is the
// case that already worked and must not regress. B, C and D are the three the
// bounding box gets wrong, and each is written so that a boundary function
// which quietly went back to returning bounds fails it. E pins the two answers
// that come out of core/SelectionMask.hpp's inverted coverage default rather
// than out of this file. F is the coverage threshold, which is where an
// antialiased edge is actually decided. G is the cache, structured so that a
// cache which never invalidates fails. H is what it costs, against PRD F3.
// ---------------------------------------------------------------------------
bool runSelectionBoundaryTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Writes one texel's coverage directly, so a fixture can be an arbitrary
  // shape rather than something a constructor happens to be able to make. An
  // L, a ring and a checkerboard step are not shapes any of PRD E3's tools
  // produce in one gesture, and they are exactly the shapes that separate a
  // boundary trace from a bounding box.
  auto paint = [](Selection& sel, int32_t x, int32_t y, float coverage) {
    const PixelCoord p{x, y};
    sel.tiles.getOrCreate(tileCoordAt(p)).writeCoverage(tileLocalOffset(p), coverage);
  };

  auto hasVertex = [](const SelectionBoundary& b, int32_t x, int32_t y) {
    for (const BoundaryContour& c : b.contours)
      for (const BoundaryVertex v : c.vertices)
        if (v.x == x && v.y == y) return true;
    return false;
  };

  // The bounding box of a whole boundary, in the same corner coordinates the
  // contours use. Several assertions below are of the form "the boundary is not
  // its own bounding box", and this is what they compare against.
  struct Box { int32_t x0, y0, x1, y1; bool any; };
  auto boxOf = [](const SelectionBoundary& b) {
    Box r{0, 0, 0, 0, false};
    for (const BoundaryContour& c : b.contours)
      for (const BoundaryVertex v : c.vertices) {
        if (!r.any) { r = Box{v.x, v.y, v.x, v.y, true}; continue; }
        r.x0 = std::min(r.x0, v.x); r.y0 = std::min(r.y0, v.y);
        r.x1 = std::max(r.x1, v.x); r.y1 = std::max(r.y1, v.y);
      }
    return r;
  };

  std::printf("  -- A. the rectangle: exactly its four sides, and nothing else --\n");

  // The case that already worked. `selectRectangle()` on whole numbers gives
  // coverage exactly 1.0 over an 8x6 block, so the answer is not a matter of
  // taste: 2*(8+6) = 28 unit crack edges, one closed contour, four corners.
  //
  // The unit-edge count is the assertion that pins the extraction rather than
  // merely sampling it -- a trace that emitted an edge twice, or that walked
  // 8-connected neighbours instead of 4, lands on a different number and there
  // is no tolerance to hide in.
  {
    const Selection sel = selectRectangle(2.0f, 3.0f, 10.0f, 9.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);
    check(b.unitEdgeCount == 28,
          "rect 8x6: exactly 2*(w+h) = 28 unit edges -- a different count means the trace "
          "double-counts a side or misses one");
    check(b.contours.size() == 1,
          "rect: one closed contour -- a rectangle has one boundary, and a second one would "
          "be an outline drawn twice over itself");
    check(b.contours.size() == 1 && b.contours[0].vertices.size() == 4,
          "rect: four vertices, not 28 -- collinear runs collapse, or a full-canvas outline "
          "costs thousands of line calls a frame");
    check(hasVertex(b, 2, 3) && hasVertex(b, 10, 3) && hasVertex(b, 10, 9) &&
              hasVertex(b, 2, 9),
          "rect: the four vertices ARE the four corners -- an outline offset by half a texel "
          "reads as a one-pixel registration error when zoomed in");
  }

  // The smallest possible selection. Worth its own assertion because it is the
  // case where "collapse collinear runs" could plausibly collapse everything:
  // a one-texel square is four edges and four corners with no straight run at
  // all, and a trace that dropped it would make a single-texel wand click look
  // like a selection that did not happen.
  {
    Selection sel;
    paint(sel, 5, 5, 1.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);
    check(b.unitEdgeCount == 4 && b.contours.size() == 1 &&
              b.contours[0].vertices.size() == 4,
          "one texel: four edges, one contour, four vertices -- the smallest selection must "
          "still be drawn, not collapsed away");
  }

  std::printf("  -- B. a non-convex selection is NOT its bounding box --\n");

  // **The assertion this whole track exists for.** An L: a 4x4 block with the
  // bottom-right 2x2 removed, so the shape turns inward at (2, 2).
  //
  // Note what does NOT separate the two answers here: the L's perimeter is 16
  // unit edges and its bounding box's perimeter is also 16, because a
  // rectilinear shape with no true concavity in either axis has its box's
  // perimeter exactly. Every orthoconvex shape does. So the edge count is
  // useless for this case and the assertions are about the VERTICES -- the
  // concave corner must be there, and the box's own bottom-right corner must
  // not be.
  {
    Selection sel;
    for (int32_t y = 0; y < 4; ++y)
      for (int32_t x = 0; x < 4; ++x)
        if (!(x >= 2 && y >= 2)) paint(sel, x, y, 1.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);
    const Box box = boxOf(b);

    check(b.contours.size() == 1 && b.contours[0].vertices.size() == 6,
          "L-shape: six vertices -- a bounding box has four, so this is the count that tells "
          "a real trace from the box the ants used to draw");
    check(hasVertex(b, 2, 2),
          "L-shape: the boundary visits the CONCAVE corner (2,2) -- the one point no "
          "bounding box of this shape contains");
    check(!hasVertex(b, 4, 4),
          "L-shape: the boundary does NOT visit (4,4), the bounding box's own corner -- that "
          "corner is outside the selection and drawing it claims texels the edit will miss");
    check(box.x0 == 0 && box.y0 == 0 && box.x1 == 4 && box.y1 == 4,
          "L-shape: the outline still spans the full 0..4 extent -- the trace is tighter than "
          "the box without being smaller than the shape");
  }

  std::printf("  -- C. a hole gets its own contour --\n");

  // A ring: 5x5 with the centre texel removed. Two contours, and the inner one
  // is the whole point -- a trace that only finds outer boundaries loses the
  // hole silently, and what the user sees is a selection drawn as filled while
  // the edit leaves a hole in the middle of it. Nothing about the outer
  // contour is wrong in that failure, which is why it needs its own assertion.
  {
    Selection sel;
    for (int32_t y = 0; y < 5; ++y)
      for (int32_t x = 0; x < 5; ++x)
        if (!(x == 2 && y == 2)) paint(sel, x, y, 1.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);

    check(b.contours.size() == 2,
          "ring: TWO contours -- one contour means the hole was never traced, and a hole "
          "drawn as filled says the opposite of what the selection does");
    check(b.unitEdgeCount == 24,
          "ring: 20 outer + 4 inner = 24 unit edges -- the inner square's perimeter is "
          "present in the total, not merely in the contour count");

    // The inner contour identified by geometry rather than by index: it is the
    // one that lies strictly inside the outer extent. Identifying it by
    // position rather than by order is deliberate -- the extraction makes no
    // promise about which contour comes first, and a test that assumed one
    // would be asserting an implementation detail.
    size_t innerContours = 0;
    for (const BoundaryContour& c : b.contours) {
      bool strictlyInside = true;
      for (const BoundaryVertex v : c.vertices)
        if (v.x <= 0 || v.y <= 0 || v.x >= 5 || v.y >= 5) strictlyInside = false;
      if (strictlyInside) ++innerContours;
    }
    check(innerContours == 1,
          "ring: exactly one contour lies strictly inside the outer extent -- that is the "
          "hole, and it is what the marching ants have to draw round");
    check(hasVertex(b, 2, 2) && hasVertex(b, 3, 3),
          "ring: the hole's own corners (2,2) and (3,3) are on the boundary -- a hole traced "
          "at the wrong offset outlines the wrong texel");
  }

  std::printf("  -- D. two disjoint islands, which is what Shift-add produces --\n");

  // The user's reported bug in test form. Two rectangles far apart, combined
  // through the SAME path PRD E7's Shift-drag takes -- `combineSelections` with
  // `SelectionCombine::Add` -- rather than through a hand-built fixture, so
  // this fails if either the algebra or the trace regresses.
  //
  // A bounding box of this selection is one contour spanning 0..13, so the
  // numbers below are all ones the old drawing produced differently: it drew
  // one box of perimeter 52 where the truth is two squares of perimeter 12.
  {
    const Selection first = selectRectangle(0.0f, 0.0f, 3.0f, 3.0f);
    const Selection second = selectRectangle(10.0f, 10.0f, 13.0f, 13.0f);
    const Selection both = combineSelections(first, second, SelectionCombine::Add);
    const SelectionBoundary b = extractSelectionBoundary(both);

    check(b.contours.size() == 2,
          "shift-add: TWO contours, one per island -- one contour is the bounding box round "
          "both, which is exactly what 'shift just draws another rectangle' looked like");
    check(b.unitEdgeCount == 24,
          "shift-add: 12 + 12 = 24 unit edges, not the 52 of a box round both -- the outline "
          "measures the shapes and not the gap between them");
    check(!hasVertex(b, 0, 13) && !hasVertex(b, 13, 0),
          "shift-add: neither off-diagonal corner of the enclosing box is on the boundary -- "
          "those two points are the box, and nothing else");

    size_t nearOrigin = 0, farAway = 0;
    for (const BoundaryContour& c : b.contours) {
      bool allNear = true, allFar = true;
      for (const BoundaryVertex v : c.vertices) {
        if (v.x > 5 || v.y > 5) allNear = false;
        if (v.x < 5 || v.y < 5) allFar = false;
      }
      if (allNear) ++nearOrigin;
      if (allFar) ++farAway;
    }
    check(nearOrigin == 1 && farAway == 1,
          "shift-add: one contour is wholly around each island -- a single contour that "
          "stitched the two together would enclose the untouched space between them");
  }

  // The checkerboard step: two texels meeting only at a corner. The one
  // genuinely ambiguous vertex in the whole algorithm, and core/
  // SelectionBoundary.hpp states which way it is resolved -- two separate
  // contours, matching the 4-connectivity the edge test itself uses. Asserted
  // because the alternative pairing is equally drawable and would change the
  // contour count, and a decision nothing checks is a decision that drifts.
  {
    Selection sel;
    paint(sel, 0, 0, 1.0f);
    paint(sel, 1, 1, 1.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);
    check(b.contours.size() == 2 && b.unitEdgeCount == 8,
          "corner touch: two contours and eight edges -- diagonal texels are not neighbours "
          "under the 4-connected edge test, so they are not one region here either");
  }

  std::printf("  -- E. the two answers that come from the coverage default --\n");

  // Empty means empty. Both spellings of it: a selection with no tiles at all,
  // and a degenerate rectangle, which core/SelectionMask.hpp is careful to say
  // is "selects nothing" rather than "selects everything".
  {
    const Selection none;
    const SelectionBoundary b = extractSelectionBoundary(none);
    check(b.contours.empty() && b.unitEdgeCount == 0,
          "empty selection: no contours and no edges -- an outline round nothing is an "
          "outline round the whole canvas, which is the opposite claim");

    const Selection degenerate = selectRectangle(5.0f, 5.0f, 5.0f, 5.0f);
    const SelectionBoundary d = extractSelectionBoundary(degenerate);
    check(d.contours.empty() && d.unitEdgeCount == 0,
          "degenerate rectangle: no contours -- a marquee dragged to nothing draws nothing, "
          "not a canvas-wide outline");
  }

  // **Select All draws the canvas edge, and this is the one answer in the
  // section that is decided elsewhere.** core/SelectionMask.hpp's inverted
  // default is what settles it: a `Selection` has no document extent, an absent
  // tile means coverage 0.0, so the texel at x = 0 has an unselected neighbour
  // at x = -1 and that IS a boundary edge. The alternative -- treating the
  // document edge as "no boundary here" -- would make Select All draw nothing
  // and be indistinguishable from the command not working.
  {
    const Selection all = selectAll(64, 48);
    const SelectionBoundary b = extractSelectionBoundary(all);
    check(!b.empty(),
          "select all: the boundary is NOT empty -- outside the selection is coverage 0, so "
          "the canvas edge is a real edge and the user must see the ants they asked for");
    check(b.contours.size() == 1 && b.unitEdgeCount == 2 * (64 + 48),
          "select all 64x48: one contour of 2*(64+48) = 224 unit edges -- the canvas "
          "rectangle exactly");
    check(hasVertex(b, 0, 0) && hasVertex(b, 64, 0) && hasVertex(b, 64, 48) &&
              hasVertex(b, 0, 48),
          "select all: the four vertices are the canvas corners -- inset by one texel means "
          "the outline is being clipped somewhere it should not be");
  }

  // Tile-crossing, which is where a sparse store can go wrong invisibly. The
  // rectangle 120..140 straddles the boundary between four 128-texel tiles, so
  // every one of its four sides has texels whose 4-neighbour lives in a
  // different tile -- the case the extraction's apron exists for. A version
  // that read only within the tile would find spurious edges along the tile
  // seams and draw a grid over the selection.
  {
    const Selection sel = selectRectangle(120.0f, 120.0f, 140.0f, 140.0f);
    const SelectionBoundary b = extractSelectionBoundary(sel);
    check(b.contours.size() == 1 && b.unitEdgeCount == 2 * (20 + 20),
          "across four tiles: still one contour of 80 edges -- a seam between tiles is not a "
          "boundary, and a trace that thought so would draw the tile grid");
  }

  std::printf("  -- F. the coverage threshold, and where an antialiased edge lands --\n");

  // 0.5, and the evidence is in core/SelectionBoundary.hpp: it is the contour
  // Photoshop draws, and its own warning that a selection no more than half
  // covered anywhere will show no selection edges is a message about this exact
  // threshold. Here it is asserted in the form that message describes.
  {
    Selection faint;
    for (int32_t y = 0; y < 3; ++y)
      for (int32_t x = 0; x < 3; ++x) paint(faint, x, y, 0.4f);
    check(extractSelectionBoundary(faint).empty(),
          "coverage 0.4 everywhere: no boundary -- this is the state Photoshop warns about "
          "by name, and drawing ants round it would promise an edit that barely lands");

    Selection solidEnough;
    for (int32_t y = 0; y < 3; ++y)
      for (int32_t x = 0; x < 3; ++x) paint(solidEnough, x, y, 0.6f);
    check(extractSelectionBoundary(solidEnough).unitEdgeCount == 12,
          "coverage 0.6 everywhere: the full 3x3 outline -- the threshold weights which "
          "texels count, it does not soften the line it draws");
  }

  // The quantisation claim from the header, checked rather than asserted in
  // prose: coverage is stored as k/255, and 0.5 is not of that form
  // (127/255 = 0.498039, 128/255 = 0.501961), so no stored value can land ON
  // the threshold and `>` and `>=` cannot disagree. Both neighbouring levels
  // are exercised.
  {
    Selection above, below;
    paint(above, 0, 0, 0.5f);              // quantises to 128 -> 0.501961
    paint(below, 0, 0, 127.0f / 255.0f);   // quantises to 127 -> 0.498039
    const float aboveCov = above.tiles.find(tileCoordAt(PixelCoord{0, 0}))
                               ->coverageAt(PixelCoord{0, 0});
    const float belowCov = below.tiles.find(tileCoordAt(PixelCoord{0, 0}))
                               ->coverageAt(PixelCoord{0, 0});
    std::printf("  [measured] the two coverage levels straddling 0.5: %.6f and %.6f -- "
                "neither is 0.5,\n    so '>' and '>=' select the same texels\n",
                belowCov, aboveCov);
    check(aboveCov > 0.5f && belowCov < 0.5f,
          "quantisation: no stored coverage lands on 0.5 exactly -- which is why the "
          "comparison's strictness is not a decision anyone has to make");
    check(extractSelectionBoundary(above).unitEdgeCount == 4 &&
              extractSelectionBoundary(below).empty(),
          "quantisation: 128/255 is inside and 127/255 is outside -- the threshold splits "
          "the store's own levels and not a float that rounds either way");
  }

  // What the rejected threshold would have cost, measured rather than argued.
  //
  // The fixture is a 60x60 selection whose outer ten texels ramp from 0.1 up to
  // 1.0 -- the shape ops/FloodFill's tolerance ramp and PRD E8's feather both
  // produce, a *band* of fractional coverage rather than the single antialiased
  // texel a hard-edged shape has. Coverage reaches 0.5 four texels in, so the
  // half-coverage contour is the 52x52 square inset by four, while tracing any
  // non-zero coverage follows the outer extent of the ramp and gives the full
  // 60x60.
  //
  // Worth recording what does NOT separate them, because it is the fixture a
  // reader would reach for first: on an exact-area **ellipse** the two
  // thresholds come out identical (172 unit edges, extent 15,22..65,58 both
  // ways, measured), because a hard-edged shape's antialiasing is one texel
  // wide and its extreme texels are more than half covered anyway. The choice
  // of threshold is invisible there and unmistakable here, and it is here --
  // feathered edges and wand ramps -- that a user would notice.
  {
    Selection ramp;
    for (int32_t y = 0; y < 60; ++y)
      for (int32_t x = 0; x < 60; ++x) {
        const int32_t d = std::min(std::min(x, y), std::min(59 - x, 59 - y));
        paint(ramp, x, y, std::min(1.0f, (static_cast<float>(d) + 1.0f) / 10.0f));
      }
    const SelectionBoundary half = extractSelectionBoundary(ramp, 0.5f);
    const SelectionBoundary anyCoverage = extractSelectionBoundary(ramp, 1.0f / 255.0f);
    const Box halfBox = boxOf(half);
    const Box anyBox = boxOf(anyCoverage);
    std::printf("  [measured] a 10-texel coverage ramp round a 60x60 selection: the outline "
                "at\n    coverage >= 0.5 is %zu unit edges spanning %d,%d..%d,%d; at any "
                "non-zero coverage\n    it is %zu edges spanning %d,%d..%d,%d\n",
                half.unitEdgeCount, halfBox.x0, halfBox.y0, halfBox.x1, halfBox.y1,
                anyCoverage.unitEdgeCount, anyBox.x0, anyBox.y0, anyBox.x1, anyBox.y1);
    check(half.unitEdgeCount == 208 && anyCoverage.unitEdgeCount == 240,
          "threshold: 0.5 traces the 52x52 half-coverage contour, >0 traces the whole 60x60 "
          "ramp -- four texels apart on every side, which is not a subtlety");
    check(anyBox.x0 < halfBox.x0 && anyBox.y0 < halfBox.y0 && anyBox.x1 > halfBox.x1 &&
              anyBox.y1 > halfBox.y1,
          "threshold: the >0 outline strictly encloses the >=0.5 one -- so it floats outside "
          "the region a user reads as selected, which is why it was rejected");
  }

  std::printf("  -- G. the cache, and that it actually invalidates --\n");

  // PRD E6's ants animate, so the overlay is drawn every frame; section H is
  // why that cannot mean extracting every frame. A cache is therefore not
  // optional -- and **a cache that never invalidates passes every test that
  // only draws once**, which is why each assertion below checks BOTH that the
  // extraction count moved and that the boundary handed back is the new shape.
  // Either half alone can be satisfied by a broken cache: one by a cache that
  // recomputes and returns a stale copy, the other by a cache that returns the
  // right answer only because the fixture never changed.
  {
    SelectionBoundaryCache cache;
    const Selection one = selectRectangle(0.0f, 0.0f, 4.0f, 4.0f);

    const size_t firstContours = cache.boundaryFor(&one, /*documentId=*/7, /*revision=*/0)
                                     .contours.size();
    check(cache.extractionCount() == 1 && firstContours == 1,
          "cache: the first ask extracts once and gets the one contour a rectangle has");

    cache.boundaryFor(&one, 7, 0);
    cache.boundaryFor(&one, 7, 0);
    check(cache.extractionCount() == 1,
          "cache: repeating the same (document, revision) does NOT re-extract -- otherwise "
          "the ants cost ~6 ms of the frame's 20, every frame, forever");

    // The mutation, through the same `combineSelections` a Shift-drag runs.
    // The revision moves because `installSelection()` moves it, which is the
    // contract core/SelectionBoundary.hpp names.
    const Selection island = selectRectangle(20.0f, 20.0f, 24.0f, 24.0f);
    const Selection two = combineSelections(one, island, SelectionCombine::Add);
    const SelectionBoundary& after = cache.boundaryFor(&two, 7, /*revision=*/1);
    check(cache.extractionCount() == 2,
          "cache: a moved revision re-extracts -- a cache that never refreshes is invisible "
          "in any test that draws once and is a frozen outline to the user");
    check(after.contours.size() == 2,
          "cache: and the boundary handed back is the NEW one, two islands -- re-extracting "
          "into a stale copy would pass the count check above and still be wrong");

    // The two-tabs trap, which is the reason the key is not the revision alone:
    // revisions start at 0 per document, so two open documents sit at the same
    // number most of the time.
    const Selection ring = selectAll(8, 8);
    const SelectionBoundary& other = cache.boundaryFor(&ring, /*documentId=*/9, /*revision=*/1);
    check(cache.extractionCount() == 3 && other.unitEdgeCount == 2 * (8 + 8),
          "cache: a different document at the SAME revision re-extracts -- keyed on the "
          "revision alone, one tab would draw the other tab's outline");

    const SelectionBoundary& gone = cache.boundaryFor(nullptr, 9, /*revision=*/2);
    check(cache.extractionCount() == 4 && gone.empty(),
          "cache: deselecting gives an empty boundary -- no selection is not a selection of "
          "everything, and the ants must go away");

    cache.invalidate();
    cache.boundaryFor(nullptr, 9, 2);
    check(cache.extractionCount() == 5,
          "cache: invalidate() forces the next ask to re-extract even on an unmoved key -- "
          "the escape hatch for a selection mutated behind the revision's back");
  }

  std::printf("  -- H. what it costs, against PRD F3's 20 ms --\n");

  // The realistic worst case: Select All on a 2048x2048 document. 256 tiles,
  // 4.19 M texels, an 8192-edge boundary -- every texel has to be looked at,
  // because coverage is not summarised anywhere.
  //
  // Best-of-five rather than a mean, the same convention runClippingMaskTest()
  // uses for its composite timings: the fastest run is the one least polluted
  // by whatever else the machine was doing.
  {
    const Selection all = selectAll(2048, 2048);
    double best = 1e30;
    size_t edges = 0;
    for (int i = 0; i < 5; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      const SelectionBoundary b = extractSelectionBoundary(all);
      const auto t1 = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
      edges = b.unitEdgeCount;
    }

    SelectionBoundaryCache cache;
    cache.boundaryFor(&all, 1, 0);
    constexpr int kHits = 2000;
    double cachedBest = 1e30;
    for (int i = 0; i < 5; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < kHits; ++k) cache.boundaryFor(&all, 1, 0);
      const auto t1 = std::chrono::steady_clock::now();
      cachedBest = std::min(
          cachedBest, std::chrono::duration<double, std::milli>(t1 - t0).count() / kHits);
    }

    std::printf("  [measured] full-canvas selection on 2048x2048 (%zu edges): extraction "
                "%.3f ms,\n    cached re-ask %.6f ms (%.0fx cheaper); PRD F3's whole frame is "
                "20 ms\n",
                edges, best, cachedBest, cachedBest > 0.0 ? best / cachedBest : 0.0);

    // **The bound is 20 ms and it is derived, not chosen for roundness.** The
    // measurement above is ~6 ms on this machine, so there is 3.3x of headroom
    // for a loaded build box -- and the implementation this replaced, which
    // asked `selectionCoverageAt()` for each of the four neighbours and so paid
    // a hash lookup per neighbour per texel, measures 47.6 ms on the identical
    // fixture and fails it. So the bound sits between the two by construction:
    // it cannot be met by the version that does not fit in a frame, and it is
    // not so tight that machine load flips it.
    check(best < 20.0,
          "cost: a full-canvas extraction fits inside one PRD F3 frame -- so changing a "
          "selection never drops a frame, however large the selection is");

    // And the ratio is what says the cache is necessary rather than tidy. The
    // measured ratio is four orders of magnitude (a key compare against a
    // 4.19 M texel walk); 1000x is asserted, leaving an order of magnitude of
    // slack, because anything near 1x means the memo is not memoising.
    check(cachedBest > 0.0 && best / cachedBest > 1000.0,
          "cost: a cached re-ask is >1000x cheaper than the extraction -- which is what makes "
          "an animated outline affordable at 120 fps");

    // The number the cache exists to avoid, stated as the assertion a future
    // "simplification" back to per-frame extraction would fail: 120 frames of
    // this would be over half a second of work per second of animation.
    std::printf("  [measured] extracting this every frame at 120 fps would cost %.0f%% of "
                "the CPU,\n    which is why ui/MacPaintUI asks the cache and not the "
                "extractor\n",
                best * 120.0 / 10.0);
  }

  // The section verdict every other section prints. Without it this file's
  // assertions are still counted -- `ok` reaches main.cpp's chain either way --
  // but a reader scanning the run for section names cannot see that it ran at
  // all, which is how a section that silently stopped being called gets missed.
  std::printf("[selftest] selection boundary %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
