#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/SelectionMask.hpp"

// core/SelectionBoundary (PLAN.md "Phase 7 -- Select and paste"; PRD E6). The
// TRUE outline of a selection, in document texel space, as closed contours.
//
// This file exists because of a sentence core/SelectionMask.hpp wrote in
// advance and ui/MacPaintUI.cpp then repeated as an admission: the marching
// ants were drawn from `selectionBounds()`, "which is exact for a rectangular
// marquee and merely a bounding box for anything else". While `selectRectangle`
// was the only constructor that was approximate-but-harmless. Once the lasso,
// the polygon lasso and the wand landed -- and once PRD E7's Shift-add could
// put two disjoint islands in one selection -- it became actively wrong: every
// selection, whatever its shape, drew as a rectangle. Four separate bug reports
// ("shift just draws another rectangle", "the lasso selects a rectangle", "the
// polygon lasso selects a rectangle", "the wand does nothing") were all that one
// picture. The model was right in every case; only the outline lied.
//
// --- Crack edges, then chained into contours ------------------------------
//
// The extraction is a **crack-edge scan**: for every texel above the coverage
// threshold, an edge of the texel's unit square is a boundary edge exactly when
// the 4-neighbour across it is *not* above the threshold. The edges are then
// linked head-to-tail into closed loops, and collinear runs are collapsed to
// their turning points.
//
// **Rejected: a marching-squares contour walk on the coverage field.** It was
// the obvious alternative and it is a worse fit here for three reasons, each of
// which would have cost something visible:
//
//   * Marching squares interpolates the crossing point along each cell edge, so
//     its contour is *sub-texel* -- which sounds better and is not. What the
//     ants have to outline is the set of texels an edit will actually land in,
//     and that set has texel-aligned corners. A half-texel-offset outline reads
//     as a one-pixel registration error when zoomed in, and PRD E6's whole job
//     is to say where the selection is.
//   * Its saddle case (the two-diagonal cell) has to be resolved by a rule that
//     is invisible in the output, whereas the same ambiguity here is a vertex
//     where two contours touch at a corner and the rule is stated below and
//     asserted.
//   * "The boundary of a w x h rectangle is exactly 2*(w+h) unit edges" is a
//     statement a crack-edge scan can be *held to*, and it is the cheapest
//     regression guard there is for the case that already worked. Marching
//     squares would answer that question with four interpolated corners and no
//     way to tell a correct trace from a bounding box.
//
// What marching squares would have bought is a smoother outline on a feathered
// selection. It is not bought, deliberately: the ants are a boundary indicator,
// not a rendering of the coverage ramp, and PRD E8's feather is shown by the
// soft edit itself rather than by the dashes.
//
// **Why contours and not a loose bag of edges.** The ants animate: PRD E6's
// dashes crawl along the boundary, and a dash phase that restarted at every
// unit edge would not crawl at all, it would shimmer in place. Chaining the
// edges into ordered loops is what lets the drawing code run one arc-length
// accumulator along the whole outline. Collapsing collinear runs is the other
// half of the same argument -- a 2048-texel-wide selection is four line
// segments here, not 8192, which is the difference between an overlay that
// costs microseconds a frame and one that costs milliseconds.
//
// --- The coverage threshold is 0.5, and that is not a guess ---------------
//
// A selection is antialiased at its edge -- core/SelectionShapes rasterises
// exact covered area, ops/FloodFill's wand produces a tolerance ramp -- so
// "which coverage counts as inside?" changes where the line is drawn. Two
// candidates, and the evidence for the one chosen:
//
//   > 0.0   Every texel the antialiasing touched at all. This traces the OUTER
//           extent of the ramp, so the outline sits up to a texel outside the
//           edge the user perceives, and a wand with a wide tolerance ramp gets
//           an outline that visibly floats away from the region it selected.
//           It also makes the outline of a heavily feathered selection (PRD E8
//           grows the ramp deliberately) enormously longer than the shape.
//
//   >= 0.5  The half-coverage contour. This is what Photoshop draws, and its
//           own warning dialog says so out loud -- "no pixels are more than 50%
//           selected, the selection edges will not be visible" is a message
//           about *this exact threshold*, and it is the behaviour every painter
//           coming to this application already has in their hands.
//
// So 0.5 it is, checked against practice rather than reasoned from taste --
// the same standard core/SelectionMask.hpp applied when it chose uint8 storage.
//
// The comparison is `>=`, and **that choice cannot matter**, which is worth
// stating so nobody spends time on it: coverage is stored quantised to k/255,
// and 0.5 is not of that form (127/255 = 0.498039, 128/255 = 0.501961). No
// stored value lands on the threshold, so `>` and `>=` select the same texels.
//
// --- What lies outside the selection, and therefore what the canvas edge is -
//
// core/SelectionMask.hpp's central asymmetry decides this: **an absent
// selection tile means coverage 0.0** -- outside -- which is the inverse of a
// layer mask. There is no document extent in a `Selection` at all; the store is
// unbounded and simply holds nothing beyond what was selected. So a texel at
// x = 0 has a left neighbour at x = -1 whose coverage is 0, and that is a
// boundary edge.
//
// The consequence, which is the correct one: **Select All on a W x H document
// produces the canvas rectangle, not an empty boundary.** That is what the user
// must see -- core/SelectionMask.hpp's `selectAll()` exists precisely so that
// "the user asked for one and must be able to see it" -- and it falls out of
// the coverage convention rather than needing a special case. A version of this
// that clipped the scan to the document extent would draw nothing for Select
// All and would be indistinguishable from a bug in the tool.
//
// --- The one ambiguous vertex, and how it is resolved ---------------------
//
// Where two selected texels meet only at a corner (a checkerboard step), the
// shared grid vertex carries two outgoing edges and two incoming ones, and the
// walk has to pick a pairing. **The pairing chosen keeps the two texels in
// SEPARATE contours**, matching the 4-connectivity the edge test itself uses:
// diagonal texels are not neighbours anywhere else in this file, so they are
// not one region here either. The alternative pairing joins them into a single
// figure-eight loop. Both draw the identical set of line segments -- this is
// not a visual decision -- but it is a decision, and leaving it to hash order
// would make the contour count nondeterministic, which a test cannot assert
// against.
//
// Determinism generally: tiles are visited in sorted coordinate order rather
// than in `unordered_map` order, so the same selection always yields the same
// contours in the same order with the same starting vertices. That costs a sort
// of a few hundred tile coordinates and buys a golden image that does not
// depend on hash seeding.
namespace np {

// A corner of the document texel grid. **Integers, and exactly integers**: a
// texel spans [x, x+1) x [y, y+1), so every crack edge runs between two
// integer corners and there is no rounding anywhere in the extraction. The
// float conversion happens once, in the drawing code, on the way through the
// view transform.
struct BoundaryVertex {
  int32_t x = 0;
  int32_t y = 0;
};

inline bool operator==(BoundaryVertex a, BoundaryVertex b) noexcept {
  return a.x == b.x && a.y == b.y;
}
inline bool operator!=(BoundaryVertex a, BoundaryVertex b) noexcept { return !(a == b); }

// One closed loop of the boundary, as its turning points only -- consecutive
// vertices are always separated by an axis-aligned run, and **the last vertex
// joins the first**. A caller that draws this must close it itself; a caller
// that measures its perimeter must count the closing segment.
//
// A selection with a hole has two of these (an outer loop and an inner one),
// and two disjoint islands have two as well. Nothing here records which is
// which, because nothing needs to: the ants are drawn round every loop
// identically, and an inner loop the drawing code skipped would show a hole as
// filled.
struct BoundaryContour {
  std::vector<BoundaryVertex> vertices;
};

// The whole boundary of one selection.
struct SelectionBoundary {
  std::vector<BoundaryContour> contours;

  // Total number of **unit-length** crack edges, before collinear collapse --
  // that is, the boundary's perimeter measured in texels. Kept because it is
  // the one number that pins the extraction exactly (a w x h rectangle is
  // 2*(w+h) and nothing else is), and because the collapsed vertex lists
  // deliberately throw it away.
  size_t unitEdgeCount = 0;

  bool empty() const noexcept { return contours.empty(); }
};

// The coverage at or above which a texel counts as inside the selection for
// drawing purposes. See the header comment for the evidence behind 0.5.
inline constexpr float kSelectionBoundaryCoverage = 0.5f;

// Extracts the boundary of `selection`.
//
// O(occupied tiles x texels), the same order as `selectionBounds()`, and for
// the same reason: coverage is not summarised anywhere, so finding the edge
// means looking at every texel that could be on it.
//
// **Measured, because the number is what decides whether the cache below is
// necessary**: a full-canvas selection on a 2048x2048 document (256 tiles,
// 4.19 M texels, an 8192-edge boundary) takes ~6 ms on this machine. That fits
// inside PRD F3's 20 ms frame once -- so changing a selection never drops a
// frame -- and does not fit inside it 120 times a second alongside a composite
// and a solver step. Hence `SelectionBoundaryCache`; --selftest re-runs the
// measurement rather than trusting this sentence.
//
// The honest worst case is far worse and is not a frame-rate problem: a wand
// result shaped like 1024 alternating one-texel stripes across the same
// document is 4.2 M boundary edges and ~350 ms, because a boundary that long
// is genuinely that much work to link. It is paid once, when the selection
// changes, on a gesture the user just made -- and a selection of that shape is
// pathological rather than realistic. Named so it is a known cost.
//
// An empty selection -- no tiles, or no texel above the threshold -- yields no
// contours and a `unitEdgeCount` of 0. That is not the same as "the whole
// canvas", and it is what a marquee dragged to nothing correctly draws.
SelectionBoundary extractSelectionBoundary(const Selection& selection,
                                           float threshold = kSelectionBoundaryCoverage);

// The boundary of the active selection, recomputed only when it changes.
//
// PRD E6's ants animate, so the overlay is drawn on **every** frame, at up to
// 120 fps under PRD F3's 20 ms budget -- and a full-canvas extraction does not
// fit in that budget more than once. So this memoises, keyed exactly the way
// `app::AppState::selectionBoundsCache` and `ui/DocumentTexture`'s composite
// cache are keyed, rather than inventing a third scheme:
//
//   **(document id, selection revision)**, and the document id is not
//   belt-and-braces. Revisions start at 0 per document, so two open tabs sit at
//   the same revision most of the time; keying on the revision alone would draw
//   one tab's outline over the other's canvas whenever the two numbers agreed,
//   which is the common case rather than the rare one. app/AppState.hpp already
//   states this for the bounds cache; the same trap is here.
//
// `ui/MacPaintUI`'s `installSelection()` is the single funnel that bumps
// `OpenDocument::selectionRevision`, so "the selection changed" and "the
// revision moved" are the same event by construction. A caller that mutates a
// `Selection` in place without moving the revision gets a stale outline, and
// that is the one way to break this -- which is why `--selftest` asserts the
// invalidation rather than assuming it, in a form a cache that never
// invalidates fails.
//
// Keyed on `uint64_t` rather than on `app::DocumentId` deliberately: core/ is
// the domain model and does not include app/ (core/TileStore.hpp's own
// argument, "core/ is the domain model and knows nothing about files"). The
// alias is a `uint64_t` and the caller passes it through.
class SelectionBoundaryCache {
 public:
  // The boundary for `selection` at `(documentId, selectionRevision)`. A null
  // `selection` is an absent selection, whose boundary is empty -- the same
  // "no selection is not an empty selection" distinction the rest of the
  // selection code makes, resolved here in the only way a drawing routine can
  // use: with no selection there is nothing to outline.
  //
  // The returned reference is valid until the next call.
  const SelectionBoundary& boundaryFor(const Selection* selection, uint64_t documentId,
                                       uint64_t selectionRevision);

  // How many times this cache has actually run the extraction. **The hook the
  // invalidation assertion hangs on**: a cache that never refreshes returns a
  // plausible-looking boundary forever and passes every test that draws once,
  // so --selftest checks both that this number moves when the selection changes
  // and that the boundary it hands back afterwards is the new one.
  size_t extractionCount() const noexcept { return extractions_; }

  // Forces the next call to re-extract, whatever the key says. For a caller
  // that has mutated a selection behind the revision's back -- nothing in this
  // build does, and the honest way to say that is to provide the escape hatch
  // rather than to pretend the situation cannot arise.
  void invalidate() noexcept { primed_ = false; }

 private:
  SelectionBoundary boundary_;
  uint64_t documentId_ = 0;
  uint64_t revision_ = 0;
  bool primed_ = false;
  size_t extractions_ = 0;
};

}  // namespace np
