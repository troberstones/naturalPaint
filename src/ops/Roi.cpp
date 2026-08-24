#include "ops/Roi.hpp"

#include <algorithm>

namespace np {

namespace {

// Every coordinate this file produces goes through here. Arithmetic is done in
// int64 and only then clamped back, so a dilation by a radius nobody sanity-
// checked cannot wrap a rectangle inside out -- see ops/Roi.hpp's "Coordinates"
// section for why a wrapped ROI is worse than a huge one (signed overflow is
// UB, and the observable symptom is an evaluator that fetches nothing and
// paints a black tile rather than one that is merely slow).
constexpr int32_t clampCoord(int64_t v) noexcept {
  const int64_t lo = -static_cast<int64_t>(kRoiCoordLimit);
  const int64_t hi = static_cast<int64_t>(kRoiCoordLimit);
  return static_cast<int32_t>(v < lo ? lo : (v > hi ? hi : v));
}

// Collapses any degenerate rectangle to the one canonical empty value, so
// "empty" has a single spelling. Callers still use roiIsEmpty() to *test*,
// because a rectangle built by hand elsewhere may be empty without being
// canonical.
constexpr PixelRect normalise(const PixelRect& r) noexcept {
  return roiIsEmpty(r) ? roiEmptyRect() : r;
}

}  // namespace

PixelRect roiIntersect(const PixelRect& a, const PixelRect& b) noexcept {
  return normalise(PixelRect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1),
                             std::min(a.y1, b.y1)});
}

PixelRect roiUnion(const PixelRect& a, const PixelRect& b) noexcept {
  // Empty is the identity rather than a rectangle at the origin, which is what
  // makes `fold(roiUnion, roiEmptyRect())` over a list of dirty rectangles
  // correct with no first-iteration special case. A plain min/max would pull
  // every union toward (0,0).
  if (roiIsEmpty(a)) return normalise(b);
  if (roiIsEmpty(b)) return normalise(a);
  return PixelRect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
                   std::max(a.y1, b.y1)};
}

bool roiContains(const PixelRect& outer, const PixelRect& inner) noexcept {
  if (roiIsEmpty(inner)) return true;  // no texel is needed, so none is missing
  if (roiIsEmpty(outer)) return false;
  return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 &&
         outer.y1 >= inner.y1;
}

PixelRect roiExpand(const PixelRect& r, int32_t left, int32_t right, int32_t up,
                    int32_t down) noexcept {
  // An empty rectangle expands to nothing. The tempting one-liner --
  // subtracting from x0 and adding to x1 unconditionally -- turns the
  // canonical empty {0,0,0,0} into a (left+right) x (up+down) rectangle
  // straddling the origin, which is how "this stack has nothing to redraw"
  // becomes "fetch the tiles around the document's top-left corner".
  if (roiIsEmpty(r)) return roiEmptyRect();
  return normalise(PixelRect{clampCoord(static_cast<int64_t>(r.x0) - left),
                             clampCoord(static_cast<int64_t>(r.y0) - up),
                             clampCoord(static_cast<int64_t>(r.x1) + right),
                             clampCoord(static_cast<int64_t>(r.y1) + down)});
}

PixelRect roiExpandUniform(const PixelRect& r, int32_t margin) noexcept {
  return roiExpand(r, margin, margin, margin, margin);
}

PixelRect roiTranslate(const PixelRect& r, int32_t dx, int32_t dy) noexcept {
  if (roiIsEmpty(r)) return roiEmptyRect();
  return PixelRect{clampCoord(static_cast<int64_t>(r.x0) + dx),
                   clampCoord(static_cast<int64_t>(r.y0) + dy),
                   clampCoord(static_cast<int64_t>(r.x1) + dx),
                   clampCoord(static_cast<int64_t>(r.y1) + dy)};
}

// Output texel `x` reads source `[x - dx - left, x - dx + right]`, so the
// source rectangle a whole output rectangle needs is the output shifted back by
// the translation and then dilated by the kernel's own reach. Half-open on the
// high side: the last output texel is `x1 - 1`, it reads up to
// `x1 - 1 - dx + right`, and the exclusive bound is one past that.
PixelRect roiBackward(const RoiOp& op, const PixelRect& want) noexcept {
  if (roiIsEmpty(want)) return roiEmptyRect();
  return roiExpand(roiTranslate(want, -op.dx, -op.dy), op.left, op.right, op.up, op.down);
}

// The margins swap sides here, and that is the entire content of this
// function. A change at source `x` is read by every output `x'` satisfying
// `x' - dx - left <= x <= x' - dx + right`, i.e. `x + dx - right <= x' <=
// x + dx + left`. So the op's LEFT reach dilates the forward rectangle's HIGH
// side and its RIGHT reach dilates the LOW side.
//
// Written out rather than expressed as "roiBackward with a negated op" because
// that is precisely the mistake: negating the offset is right, reflecting the
// margins is right, and doing only the first is a bug no symmetric kernel can
// reveal. ops/Roi.hpp says the same thing at the declaration; it is repeated
// here because this is the line someone will "simplify".
PixelRect roiForward(const RoiOp& op, const PixelRect& changed) noexcept {
  if (roiIsEmpty(changed)) return roiEmptyRect();
  return roiExpand(roiTranslate(changed, op.dx, op.dy), op.right, op.left, op.down, op.up);
}

RoiOp roiCompose(const RoiOp& first, const RoiOp& second) noexcept {
  // Backward through both is
  //   x0 - dx2 - l2 - dx1 - l1
  // so the composed offset is the sum of the offsets and the composed margin
  // is the sum of the margins, per side. That the algebra is this trivial is
  // the payoff for restricting RoiOp to dilate-and-translate; it is also why
  // an evaluator can fold a stack once and key a cache on the result rather
  // than re-walking a chain of callbacks per tile per frame.
  return RoiOp{clampCoord(static_cast<int64_t>(first.left) + second.left),
               clampCoord(static_cast<int64_t>(first.right) + second.right),
               clampCoord(static_cast<int64_t>(first.up) + second.up),
               clampCoord(static_cast<int64_t>(first.down) + second.down),
               clampCoord(static_cast<int64_t>(first.dx) + second.dx),
               clampCoord(static_cast<int64_t>(first.dy) + second.dy)};
}

PixelRect roiBackwardChain(const std::vector<RoiOp>& ops, const PixelRect& want) noexcept {
  // BACK TO FRONT. `ops.back()` is the op nearest the screen, so it is the
  // first one the request passes through on its way toward the source. A
  // forward loop here compiles, runs, and produces the right answer for every
  // stack whose ops all commute -- which is every stack made only of symmetric
  // blurs, i.e. every stack anyone writes a first test with.
  PixelRect rect = want;
  for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
    rect = roiBackward(*it, rect);
    if (roiIsEmpty(rect)) return roiEmptyRect();
  }
  return rect;
}

PixelRect roiForwardChain(const std::vector<RoiOp>& ops, const PixelRect& changed) noexcept {
  // FRONT TO BACK -- an edit enters at the source and propagates toward the
  // screen, which is the order the ops are stored in.
  PixelRect rect = changed;
  for (const RoiOp& op : ops) {
    rect = roiForward(op, rect);
    if (roiIsEmpty(rect)) return roiEmptyRect();
  }
  return rect;
}

RoiOp roiComposeChain(const std::vector<RoiOp>& ops) noexcept {
  // Order-independent in fact -- margins and offsets are commutative sums --
  // but written front-to-back so it reads as "apply ops[0], then ops[1], ..."
  // and so it stays correct if a scale factor is ever added to RoiOp, at which
  // point composition stops commuting.
  RoiOp folded{};
  for (const RoiOp& op : ops) folded = roiCompose(folded, op);
  return folded;
}

bool roiRoundTripContainsRequest(const RoiOp& op, const PixelRect& want) noexcept {
  return roiContains(roiForward(op, roiBackward(op, want)), want);
}

TileRange roiTileRange(const PixelRect& rect) noexcept {
  if (roiIsEmpty(rect)) return TileRange{0, 0, 0, 0};
  // floorDiv on the inclusive low corner, floorDiv on the *last* texel plus one
  // on the high corner. Going through `x1 - 1` rather than dividing the
  // exclusive bound is what keeps a rectangle ending exactly on a tile edge
  // from claiming the next tile: [0, 128) must be one tile, not two.
  const int32_t tx0 = floorDiv(rect.x0, kTileSize);
  const int32_t ty0 = floorDiv(rect.y0, kTileSize);
  const int32_t tx1 = floorDiv(rect.x1 - 1, kTileSize) + 1;
  const int32_t ty1 = floorDiv(rect.y1 - 1, kTileSize) + 1;
  return TileRange{tx0, ty0, tx1, ty1};
}

PixelRect roiTileRect(TileCoord tile) noexcept {
  const PixelCoord origin = tileOrigin(tile);
  return PixelRect{origin.x, origin.y, origin.x + kTileSize, origin.y + kTileSize};
}

PixelRect roiTileRangeRect(const TileRange& range) noexcept {
  if (range.tilesWide() <= 0 || range.tilesHigh() <= 0) return roiEmptyRect();
  return PixelRect{range.x0 * kTileSize, range.y0 * kTileSize, range.x1 * kTileSize,
                   range.y1 * kTileSize};
}

}  // namespace np
