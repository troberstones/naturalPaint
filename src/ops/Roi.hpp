#pragma once

#include <cstdint>
#include <vector>

#include "core/Tile.hpp"

// ops/Roi (PLAN.md "Phase 6 -- Filter and transform it": "ROI propagation
// (`roi(rect) -> rect` per op, walked backwards)"; DESIGN-imaging.md "Class B
// -- parametric spatial ops -> live passes with ROI").
//
// The design doc states the whole job in two sentences, and they are worth
// having in front of you while reading this file:
//
//   > To evaluate one tile through a blur of radius *r* you need input
//   > covering that tile expanded by *r*, and the expansion propagates back up
//   > the stack. Each op declares `roi(rect) -> rect`; the evaluator walks it
//   > backwards to decide which source tiles to fetch.
//
// This is the piece that makes a filter **tile-local**. Without it the only
// safe thing an evaluator can do is run every op over the whole document,
// because it has no way to know how far a pass reaches; with it, redrawing one
// 128x128 tile through `levels -> blur(8) -> curves` touches a 192x192 window
// of source and nothing else. Every other part of phase 6 -- the hash-keyed
// tile cache, live blur, highpass, unsharp -- is downstream of this file being
// right.
//
// ==========================================================================
// BACKWARDS. Which direction, said three times, because this is the bug.
// ==========================================================================
//
// There are two spatial questions an op stack can be asked, they have
// different answers, and confusing them is the classic failure here. Both live
// in this file, spelled so that a call site cannot say one and mean the other:
//
//   roiBackward(op, want)     "I WANT to produce this output rectangle. Which
//                              rectangle of my INPUT must I read?"
//                              --> The evaluator's question. Walks from the
//                                  requested output tile toward the source,
//                                  i.e. from the LAST op in the stack to the
//                                  FIRST, growing the rectangle as it goes.
//
//   roiForward(op, changed)   "This rectangle of my INPUT just CHANGED. Which
//                              rectangle of my OUTPUT is now stale?"
//                              --> The invalidation question. Walks from the
//                                  edit toward the screen, FIRST op to LAST.
//
// The reason this is worth a section rather than a sentence: **for a symmetric
// kernel the two functions return the same rectangle**, so a stack containing
// only blurs cannot tell them apart, and neither can a test written against
// only blurs. The first asymmetric op -- offset, motion blur, a one-sided
// morphological pass -- is where a stack that walked the wrong way starts
// fetching the wrong source and dropping a band of texels off one edge.
// `roiForward` below therefore *reflects* the kernel margins (the left margin
// dilates the high side of the forward rectangle, not the low side), which is
// not a sign flip and is not what a hurried reader would write. --selftest
// asserts the asymmetric case specifically, on an op no production code in
// this build has yet, precisely so the property is pinned before that op
// arrives.
//
// **What a test can and cannot currently prove about "backwards", stated so
// nobody mistakes a green suite for a proof.** Within the dilate-and-translate
// algebra below, composition *commutes* -- margins add, offsets add -- so
// `roiBackwardChain` walking the list forwards would return the identical
// rectangle for every stack that can be built today. The chain's direction is
// therefore genuinely unobservable right now, and --selftest says so rather
// than asserting a tautology; what it does assert is the pair of properties
// that are observable and that carry the same risk: `roiBackward` and
// `roiForward` differ for an asymmetric op, and the walk agrees with the fold.
// The moment `RoiOp` gains a scale factor (a resample or transform node),
// composition stops commuting and the chain's direction becomes testable --
// and that is the moment to add the test, not to discover the ordering.
//
// The safety direction is also worth naming, because ROI is not a symmetric
// contract: a `roiBackward` that returns **too large** a rectangle is merely
// slow, while one that returns too small silently produces wrong pixels near
// the edge of every tile. So everything here rounds outward, clamps outward,
// and never trims. `roiRoundTripContainsRequest()` states the corresponding
// invariant as a checkable predicate.
//
// ==========================================================================
// Why an op's ROI is a dilation plus a translation, and not a `std::function`
// ==========================================================================
//
// PLAN.md says `roi(rect) -> rect`, which reads like an arbitrary function
// pointer per op. `RoiOp` below is deliberately narrower: four non-negative
// per-side margins and one integer translation. Every class-B op this codebase
// has or has planned that is *not* a resampler is exactly that shape -- blur
// and highpass are symmetric dilations, unsharp is a blur's dilation, offset
// is a pure translation, motion blur is an asymmetric dilation, median and
// dust-and-scratches are symmetric dilations of their window.
//
// Three things the narrow form buys that an opaque function does not:
//
//   1. **Composition in closed form.** `roiCompose()` folds a whole run of ops
//      into one `RoiOp` -- margins add, offsets add -- so the evaluator can ask
//      "how far does this stack reach?" once and cache the answer, instead of
//      re-walking a chain of callbacks per tile per frame. With opaque
//      functions the only way to compose is to call them, and the only way to
//      cache is to memoise a rectangle you cannot key.
//
//   2. **The forward map exists.** The inverse of an arbitrary rect->rect
//      function is not computable; the inverse of a dilate-and-translate is,
//      and it is `roiForward`. An op stack needs both directions (see above),
//      and asking each op to supply two consistent functions is asking for the
//      pair to drift.
//
//   3. **It is inspectable.** "This stack reaches 812 texels" is a number a
//      memory budget, a progress estimate and a selftest can all read.
//      DESIGN-imaging.md's argument for descending the mip pyramid on wide
//      blurs is entirely an argument about that number's size.
//
// **What the narrow form cannot express, stated rather than discovered later:
// scale.** A resample or a transform maps a destination rectangle to a source
// rectangle by *multiplication*, and no margin-plus-offset composes with that.
// Two consequences. First, ops/Resample is deliberately not given a `RoiOp` --
// it is an export-path function today and phase 6's transform op will need a
// genuinely richer node type. Second, DESIGN-imaging.md's "descend the mip
// pyramid for large blurs" crosses a scale boundary, so a mip-descending blur
// is a *composite* of {downscale, blur, upscale} whose combined ROI has to be
// computed at the mip level and scaled back by hand, not folded through
// `roiCompose`. When that lands it should extend this type with a rational
// scale factor rather than quietly rounding a scaled rectangle into a margin.
//
// ==========================================================================
// Coordinates
// ==========================================================================
//
// `PixelRect` is **half-open** in document-texel space: `[x0, x1) x [y0, y1)`,
// the same convention core/SelectionMask's `SelectionBounds` already uses and
// the same one `selectRectangle(10, 20, 14, 23)` means by "four texels wide".
// Half-open is what makes an empty rectangle and a one-texel rectangle
// distinguishable, and what makes tile ranges compose without off-by-one
// corrections at every boundary.
//
// Coordinates are clamped to +/-`kRoiCoordLimit` (2^28) rather than to
// INT32_MAX. A blur with an absurd radius must not be able to overflow a
// rectangle into a *smaller* one -- signed overflow is UB and the observed
// behaviour of a wrapped ROI is an evaluator that fetches nothing and paints a
// black tile. 2^28 is 65536 times the widest document this application will
// ever open, so the clamp is unreachable in practice, and the choice of a
// value well below INT32_MAX is what leaves `width()` and `height()` unable to
// overflow either (2 * 2^28 < 2^31).
namespace np {

// A half-open document-texel rectangle: `[x0, x1) x [y0, y1)`.
//
// Any rectangle with `x1 <= x0 || y1 <= y0` is empty. The functions below
// normalise every empty result to `roiEmptyRect()` so that "empty" has one
// spelling and a caller can compare against it, but `roiIsEmpty()` is still
// the predicate to use -- a rectangle a caller built by hand may be empty
// without being canonical.
struct PixelRect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  constexpr int32_t width() const noexcept { return x1 > x0 ? x1 - x0 : 0; }
  constexpr int32_t height() const noexcept { return y1 > y0 ? y1 - y0 : 0; }

  friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

// A half-open range of TILE coordinates: `[x0, x1) x [y0, y1)` in tile units,
// not texels. What an evaluator actually iterates once ROI has told it how far
// a pass reaches.
struct TileRange {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  constexpr int32_t tilesWide() const noexcept { return x1 > x0 ? x1 - x0 : 0; }
  constexpr int32_t tilesHigh() const noexcept { return y1 > y0 ? y1 - y0 : 0; }
  constexpr int64_t tileCount() const noexcept {
    return static_cast<int64_t>(tilesWide()) * static_cast<int64_t>(tilesHigh());
  }

  friend bool operator==(const TileRange&, const TileRange&) = default;
};

// The outward clamp every coordinate here is held inside. See the header
// comment: far beyond any real document, far below INT32_MAX, so no dilation
// can wrap and no `width()` can overflow.
inline constexpr int32_t kRoiCoordLimit = 1 << 28;

// The canonical empty rectangle. Every function here that produces an empty
// result produces exactly this one.
constexpr PixelRect roiEmptyRect() noexcept { return PixelRect{0, 0, 0, 0}; }

constexpr bool roiIsEmpty(const PixelRect& r) noexcept { return r.x1 <= r.x0 || r.y1 <= r.y0; }

// Texels the rectangle contains. `int64_t` because a legitimately large ROI --
// a wide blur's apron around a full-document request -- can exceed 2^31 texels
// while still being a rectangle the caller wants a size for rather than a
// wrapped negative number.
constexpr int64_t roiTexelCount(const PixelRect& r) noexcept {
  return static_cast<int64_t>(r.width()) * static_cast<int64_t>(r.height());
}

// Set operations. `roiIntersect` normalises a disjoint pair to
// `roiEmptyRect()`; `roiUnion` treats empty as the identity, so folding a
// union over a list can start from `roiEmptyRect()` without a first-iteration
// special case.
PixelRect roiIntersect(const PixelRect& a, const PixelRect& b) noexcept;
PixelRect roiUnion(const PixelRect& a, const PixelRect& b) noexcept;

// True when every texel of `inner` is inside `outer`. An empty `inner` is
// contained by anything, including an empty `outer` -- "there is no texel I
// need that you do not have" is vacuously true, and that is the sense in which
// containment is used below (`roiRoundTripContainsRequest`).
bool roiContains(const PixelRect& outer, const PixelRect& inner) noexcept;

// Grows a rectangle by a per-side margin, clamped to +/-`kRoiCoordLimit`.
// Negative margins shrink and may empty the rectangle, which is normalised.
// An empty input stays empty: dilating "nothing" must not conjure a rectangle
// around the origin, which is what a naive `x0 -= left` on `{0,0,0,0}` does.
PixelRect roiExpand(const PixelRect& r, int32_t left, int32_t right, int32_t up,
                    int32_t down) noexcept;

// The symmetric case, which is every blur.
PixelRect roiExpandUniform(const PixelRect& r, int32_t margin) noexcept;

// Slides a rectangle. Empty stays empty, for the same reason as above.
PixelRect roiTranslate(const PixelRect& r, int32_t dx, int32_t dy) noexcept;

// One op's spatial reach, in the restricted dilate-and-translate form this
// header argues for.
//
// **Sign and side conventions, which are the whole meaning of the type:**
//
//   The op maps a source texel at `s` to an output texel at `s + (dx, dy)`.
//   So `dx = +5` means "this op shifts the picture five texels to the right",
//   which is what an Offset op's dialog says.
//
//   `left`/`right`/`up`/`down` are how far the op's kernel reaches **into the
//   source, from the output texel's own position, after the translation**.
//   Output texel `x` reads source `[x - dx - left, x - dx + right]`. `left`
//   therefore reaches toward smaller x and `up` toward smaller y, matching a
//   top-left origin (core/Tile.hpp's document space, y increasing downward).
//
//   All four margins must be >= 0. A negative margin would describe an op that
//   reads *less* than its own texel, which is not a thing, and would let
//   `roiCompose` produce a stack that under-fetches -- the one failure mode
//   this file exists to prevent. `roiOpIsValid()` says so and --selftest
//   checks it.
//
// The default is the identity: a point op reaches exactly its own texel.
// `RoiOp{}` is therefore the correct declaration for every class-A op, which
// is why there is no `roiPointOp()` factory -- the honest spelling is already
// the shortest one.
struct RoiOp {
  int32_t left = 0, right = 0, up = 0, down = 0;
  int32_t dx = 0, dy = 0;

  friend bool operator==(const RoiOp&, const RoiOp&) = default;
};

// A symmetric dilation by `margin` on all four sides -- blur, median, and
// every other isotropic window op. ops/Blur's `blurRoiOp()` is one call to
// this, and lives there because the radius-to-margin decision belongs next to
// the kernel that makes it.
constexpr RoiOp roiDilateOp(int32_t margin) noexcept {
  return RoiOp{margin, margin, margin, margin, 0, 0};
}

// A pure translation -- Offset (PLAN.md phase 6, "offset with wrap"), and the
// clone vector of DESIGN-imaging.md's class-C Strokes layer, whose ROI is
// "exactly one clone-vector offset".
constexpr RoiOp roiOffsetOp(int32_t dx, int32_t dy) noexcept {
  return RoiOp{0, 0, 0, 0, dx, dy};
}

constexpr bool roiOpIsValid(const RoiOp& op) noexcept {
  return op.left >= 0 && op.right >= 0 && op.up >= 0 && op.down >= 0;
}

// **The evaluator's direction.** Given the output rectangle you want, the
// input rectangle you must read.
//
// `need.x0 = want.x0 - dx - left`, `need.x1 = want.x1 - dx + right`, and the
// same in y. Empty in, empty out.
PixelRect roiBackward(const RoiOp& op, const PixelRect& want) noexcept;

// **The invalidation direction.** Given an input rectangle that changed, the
// output rectangle that is now stale.
//
// Note the margins **swap sides**: a change at source `x` is read by every
// output in `[x + dx - right, x + dx + left]`, so the op's `left` margin
// dilates the forward rectangle's HIGH side. That is not a sign flip of
// `roiBackward` and it is the detail an asymmetric op will expose. For a
// symmetric kernel the two agree, which is exactly why it goes unnoticed.
PixelRect roiForward(const RoiOp& op, const PixelRect& changed) noexcept;

// `first` then `second`, as a single op. Margins add, offsets add -- the
// closed-form composition the restricted `RoiOp` shape exists for.
//
// Order is "the order the pixels flow", matching `roiBackwardChain`'s vector:
// `roiCompose(a, b)` is the op that applies `a` and then feeds the result to
// `b`.
RoiOp roiCompose(const RoiOp& first, const RoiOp& second) noexcept;

// --- Chains ---------------------------------------------------------------
//
// **`ops[0]` is the op nearest the SOURCE and `ops.back()` is the op nearest
// the SCREEN**, i.e. the natural top-to-bottom reading order of a layer's op
// stack as core/OpStack stores it. Both chain walks take the vector in that
// same order and differ in which end they start from -- which is the entire
// content of "walked backwards", made a property of the function rather than
// of the caller's loop.

// Walks `ops` from **back to front**, growing `want` at each step. The
// rectangle of source texels an evaluator must fetch to produce `want` at the
// end of the stack.
//
// Equivalent to `roiBackward(roiComposeChain(ops), want)` and asserted to be
// so; both spellings exist because the fold is what a cache wants to key on
// and the walk is what a reader wants to see.
PixelRect roiBackwardChain(const std::vector<RoiOp>& ops, const PixelRect& want) noexcept;

// Walks `ops` from **front to back**. The rectangle of final output that a
// change to `changed` in the source invalidates.
PixelRect roiForwardChain(const std::vector<RoiOp>& ops, const PixelRect& changed) noexcept;

// The whole stack as one op. `roiComposeChain({})` is the identity `RoiOp{}`.
RoiOp roiComposeChain(const std::vector<RoiOp>& ops) noexcept;

// The invariant that makes an ROI implementation *safe* rather than merely
// self-consistent: everything you asked for is still inside what you get back
// after a round trip through both directions.
//
// `roiForward(op, roiBackward(op, want)) >= want`, containment, not equality
// -- the round trip is allowed to overshoot (an ROI that is too large is slow,
// one that is too small is wrong), and for a dilating op it always does, by
// `left + right` in each axis. Exposed as a predicate rather than left as
// prose because it is the one property a new op type must be checked against,
// and --selftest checks it over a table of ops including the asymmetric and
// translating ones.
bool roiRoundTripContainsRequest(const RoiOp& op, const PixelRect& want) noexcept;

// --- Tiles ----------------------------------------------------------------

// The half-open range of tiles that `rect` touches -- floor on the low corner,
// ceil on the high corner, so a rectangle overlapping a single texel of a tile
// includes that whole tile. Rounding outward, like everything else here.
//
// This is where ROI stops being arithmetic and becomes a fetch list. Empty
// rectangle -> empty range.
TileRange roiTileRange(const PixelRect& rect) noexcept;

// The document-texel rectangle one tile covers. The inverse of the "which tile
// is this texel in" direction core/Tile.hpp owns.
PixelRect roiTileRect(TileCoord tile) noexcept;

// The rectangle covering a whole tile range, so a caller that widened a fetch
// to tile granularity can say what it actually widened to.
PixelRect roiTileRangeRect(const TileRange& range) noexcept;

}  // namespace np
