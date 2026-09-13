#include "app/MoveTool.hpp"

#include "core/Clipboard.hpp"
#include "core/LayerOps.hpp"

namespace np {
namespace {

// This file's own copy of `app/TransformSession.cpp`'s private
// `compositeStoreOverRegion()` -- same twelve lines, same premultiplied-`over`
// algebra (`core/Premultiply.hpp`'s, applied to two already-final images, not
// a second resample), reimplemented here rather than shared because that
// function is private to a file this track was told not to touch, and
// neither core/ nor ops/ has a public "splice one TileStore over another"
// entry point (that file's own comment says so). `top` must already be
// positioned at `region` in absolute document coordinates -- the shape
// `transformRgbTiles()`'s output store is in.
void compositeRgbOverRegion(const TileStore& top, const DocumentRegion& region, TileStore* dst) {
  TransformImage below = imageFromTileStore(*dst, region.x, region.y, region.width, region.height);
  const TransformImage above = imageFromTileStore(top, region.x, region.y, region.width, region.height);
  TransformImage out;
  out.width = region.width;
  out.height = region.height;
  out.px.resize(below.px.size());
  for (size_t i = 0; i + 3 < out.px.size(); i += 4) {
    const float srcA = above.px[i + 3];
    for (int c = 0; c < 4; ++c) out.px[i + c] = above.px[i + c] + below.px[i + c] * (1.0f - srcA);
  }
  tileStoreFromImage(out, region.x, region.y, dst);
}

}  // namespace

bool toolMovesPixels(Tool tool) noexcept { return tool == Tool::Move; }

MoveTarget moveTargetFor(const OpenDocument& od) noexcept {
  // The header's section 2. `has_value()`, not "covers anything": an empty
  // selection is refused by name at begin time, which says more than
  // silently promoting the gesture to a whole-layer move would.
  return od.selection.has_value() ? MoveTarget::SelectionPixels : MoveTarget::WholeLayer;
}

TransformBeginResult beginMove(TransformSession& session, OpenDocument& od) {
  TransformBeginResult r;
  const std::optional<size_t> li = activeLayerIndex(od);
  if (!li) {
    r.error = "move refused: this document has no layer to move.";
    return r;
  }
  return moveTargetFor(od) == MoveTarget::SelectionPixels
             ? session.beginSelectionPixels(od, *od.selection, *li)
             : session.beginLayer(od, *li);
}

void setMoveTranslation(TransformSession& session, float dx, float dy) noexcept {
  session.setPending(transformTranslate(dx, dy));
}

TransformCommitResult nudgeMove(OpenDocument& od, float dx, float dy) {
  TransformSession session;
  const TransformBeginResult began = beginMove(session, od);
  if (!began.ok) {
    TransformCommitResult out;
    out.error = began.error;
    return out;
  }
  setMoveTranslation(session, dx, dy);
  return session.commit(od);
}

DuplicateMoveResult commitDuplicateMove(OpenDocument& od, float dx, float dy) {
  DuplicateMoveResult r;
  const std::optional<size_t> li = activeLayerIndex(od);
  if (!li) {
    r.error = "duplicate refused: this document has no layer to duplicate.";
    return r;
  }
  const Layer& active = od.document.layers[*li];
  if (active.locked) {
    r.error = "duplicate refused: this layer is locked. Unlock it first.";
    return r;
  }

  if (moveTargetFor(od) == MoveTarget::WholeLayer) {
    const LayerOpResult dup = duplicateLayer(od.document, *li);
    if (!dup.ok) {
      r.error = dup.error;
      return r;
    }
    const size_t newIdx = dup.index;
    const LayerTransformResult moved = transformLayer(
        od.document, newIdx, transformTranslate(dx, dy), DocumentTransformParams{});
    if (!moved.ok) {
      // Rolled back rather than left behind half-done: nothing was recorded
      // yet, so `od` must come back exactly as it went in.
      removeLayer(od.document, newIdx);
      r.error = moved.error;
      return r;
    }
    od.activeLayer = newIdx;
    od.recordEdit("duplicate and move layer", EditKind::Structural);
    r.ok = true;
    r.editLabel = "duplicate and move layer";
    return r;
  }

  // MoveTarget::SelectionPixels. Refused for Pigment here, by name, for the
  // identical reason app/TransformSession.hpp section 4 refuses its OWN
  // SelectionPixels target on Pigment: the splice below is a straight alpha
  // `over`, and that is physically wrong for two overlapping films of paint
  // (Kubelka-Munk mixing, core/Pigment.hpp).
  if (active.kind == LayerKind::Pigment) {
    r.error = "duplicate refused: a selection on a Pigment layer cannot be duplicated this "
              "way -- splicing pigment back is not a straight blend (see "
              "app/TransformSession.hpp section 4). Duplicate the whole layer instead.";
    return r;
  }
  if (!active.rgbTiles.has_value()) {
    r.error = "duplicate refused: this layer holds no pixels.";
    return r;
  }
  const Clipboard extracted = copyThroughSelection(active, &*od.selection);
  if (extracted.empty()) {
    r.error = "duplicate refused: the selection covers no pixels.";
    return r;
  }

  const DocumentRegion srcRegion = selectionContentRegion(*od.selection);
  const Mat3 translate = transformTranslate(dx, dy);
  const DocumentRegion dstRegion = transformedRegion(translate, srcRegion);
  const TransformParams params;
  TileStore moved;
  TransformReport report;
  std::string err;
  if (!transformRgbTiles(*extracted.rgbTiles, srcRegion, translate, dstRegion, params, &moved,
                         &report, &err)) {
    r.error = "duplicate refused: " + err;
    return r;
  }

  // Spliced onto the layer's OWN, still-untouched tiles -- never cut, which
  // is the whole difference from an ordinary Move (this header's section 6).
  Layer& layer = od.document.layers[*li];
  compositeRgbOverRegion(moved, dstRegion, &*layer.rgbTiles);

  // The selection follows the copy, matching TransformSession.hpp section 3's
  // rule for an ordinary move. Best-effort: a refusal here (an unlikely
  // non-invertible matrix -- translate never is) leaves the selection where
  // it was rather than blocking a splice that already succeeded.
  Selection movedSelection;
  TransformReport selReport;
  std::string selErr;
  if (transformSelectionCoverage(*od.selection, srcRegion, translate, dstRegion, params,
                                 &movedSelection, &selReport, &selErr)) {
    od.selection = std::move(movedSelection);
  }

  od.recordEdit("duplicate and move selection", EditKind::Structural);
  r.ok = true;
  r.editLabel = "duplicate and move selection";
  return r;
}

}  // namespace np
