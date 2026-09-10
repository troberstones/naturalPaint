#include "app/FilterOps.hpp"

#include <array>

// `computePixelFilter()` and `applyPixelFilter()` used to live in this file's
// anonymous namespace. They moved to app/PixelOpBridge.hpp -- unchanged,
// comments and all -- when app/AdjustmentOps needed the same two functions;
// see that header for why sharing them is a correctness argument rather than
// a tidiness one. Every `applyX()`/`previewX()` below still calls them by the
// same names with the same arguments.
#include "app/PixelOpBridge.hpp"

namespace np {

size_t compositeFilterResult(const TileStore& original, const TileStore& filtered,
                             const PixelRect& rect, const Selection* selection,
                             TileStore& target) {
  size_t changed = 0;
  const TileRange tiles = roiTileRange(rect);
  for (int32_t ty = tiles.y0; ty < tiles.y1; ++ty) {
    for (int32_t tx = tiles.x0; tx < tiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      // The engine wrote nothing at this tile at all -- an identity request,
      // or a tile the requested rectangle did not reach -- so there is
      // nothing this tile could contribute, and asking `selectionCoverageAt`
      // about texels that already agree would still find nothing to write.
      const Tile* filteredTile = filtered.find(coord);
      if (filteredTile == nullptr) continue;

      const PixelRect window = roiIntersect(rect, roiTileRect(coord));
      if (roiIsEmpty(window)) continue;

      const Tile* originalTile = original.find(coord);
      const std::array<float, 4> kZero{0.0f, 0.0f, 0.0f, 0.0f};

      // Pass 1: does this tile need writing at all? Answering that WITHOUT
      // calling `getOrCreate()` is the whole point -- a tile the selection
      // excludes entirely, or one this filter left bit-identical everywhere
      // (sigma 0, amount 0, a noise amount of 0), must cost neither an
      // allocation nor a copy-on-write fork. `TileStoreOf::getOrCreate()`'s
      // own contract is the reason this has to be a separate pass rather
      // than a flag checked inside the write loop below: calling it once,
      // "to be safe", before knowing whether there is anything to write,
      // unshares (and for an absent tile, allocates) a tile this op never
      // actually touches.
      bool anyWrite = false;
      for (int32_t y = window.y0; y < window.y1 && !anyWrite; ++y) {
        for (int32_t x = window.x0; x < window.x1; ++x) {
          const PixelCoord doc{x, y};
          if (selectionCoverageAt(selection, doc) <= 0.0f) continue;
          const PixelCoord local = tileLocalOffset(doc);
          const std::array<float, 4> f = filteredTile->readPixel(local);
          const std::array<float, 4> o =
              originalTile != nullptr ? originalTile->readPixel(local) : kZero;
          if (f != o) {
            anyWrite = true;
            break;
          }
        }
      }
      if (!anyWrite) continue;

      // Pass 2: the actual write. `getOrCreate()` is the copy-on-write
      // barrier -- if this store's tile is shared with a `core::History`
      // snapshot (the ordinary case: the entry `recordEdit()` is about to
      // sit next to), this is where it forks a private copy. Texels this
      // loop does not touch keep whatever that copy already holds, which is
      // exactly the pre-edit value, because the fork is a byte-for-byte
      // clone.
      Tile& dst = target.getOrCreate(coord);
      for (int32_t y = window.y0; y < window.y1; ++y) {
        for (int32_t x = window.x0; x < window.x1; ++x) {
          const PixelCoord doc{x, y};
          const float cov = selectionCoverageAt(selection, doc);
          if (cov <= 0.0f) continue;
          const PixelCoord local = tileLocalOffset(doc);
          const std::array<float, 4> f = filteredTile->readPixel(local);
          const std::array<float, 4> o =
              originalTile != nullptr ? originalTile->readPixel(local) : kZero;

          // Full coverage writes the engine's own value rather than the
          // arithmetically-equal `o + 1.0f * (f - o)`: the two are not
          // guaranteed bit-identical in float (a lerp is not always exact at
          // its own endpoint), and the assertion this file exists to satisfy
          // is about the texels OUTSIDE the selection, not about
          // reintroducing a rounding difference at 1.0 that a plain
          // assignment would not have had.
          const std::array<float, 4> blended =
              cov >= 1.0f ? f
                          : std::array<float, 4>{o[0] + cov * (f[0] - o[0]),
                                                 o[1] + cov * (f[1] - o[1]),
                                                 o[2] + cov * (f[2] - o[2]),
                                                 o[3] + cov * (f[3] - o[3])};
          if (blended != o) {
            dst.writePixel(local, blended);
            ++changed;
          }
        }
      }
    }
  }
  return changed;
}


FilterOpResult applyGaussianBlur(OpenDocument& doc, float sigma) {
  BlurParams params;
  params.kind = BlurKind::Gaussian;
  params.sigma = sigma;
  return applyPixelFilter(doc, blurTiles, params, "gaussian blur");
}

FilterOpResult applySharpen(OpenDocument& doc, float strength) {
  const UnsharpParams params = sharpenParams(strength);
  return applyPixelFilter(doc, unsharpMaskTiles, params, "sharpen");
}

FilterOpResult applyUnsharpMask(OpenDocument& doc, const UnsharpParams& params) {
  return applyPixelFilter(doc, unsharpMaskTiles, params, "unsharp mask");
}

FilterOpResult applyAddNoise(OpenDocument& doc, const NoiseParams& params) {
  return applyPixelFilter(doc, addNoiseTiles, params, "add noise");
}

FilterOpResult applyEmboss(OpenDocument& doc, const EmbossParams& params) {
  return applyPixelFilter(doc, embossTiles, params, "emboss");
}

FilterOpResult applyMedian(OpenDocument& doc, const MedianParams& params) {
  return applyPixelFilter(doc, medianTiles, params, "median");
}

FilterOpResult applyMotionBlur(OpenDocument& doc, const MotionBlurParams& params) {
  return applyPixelFilter(doc, motionBlurTiles, params, "motion blur");
}

// See this header's own comment on `previewX()`: each one below is
// `applyX()`'s params-building preamble, feeding `computePixelFilter()`
// instead of `applyPixelFilter()` -- same engine, same params construction,
// so a preview cannot silently pick a different sigma/strength/params than
// the button next to it would commit.
FilterOpResult previewGaussianBlur(const OpenDocument& doc, float sigma, TileStore* previewOut) {
  BlurParams params;
  params.kind = BlurKind::Gaussian;
  params.sigma = sigma;
  return computePixelFilter(doc, blurTiles, params, previewOut);
}

FilterOpResult previewSharpen(const OpenDocument& doc, float strength, TileStore* previewOut) {
  const UnsharpParams params = sharpenParams(strength);
  return computePixelFilter(doc, unsharpMaskTiles, params, previewOut);
}

FilterOpResult previewUnsharpMask(const OpenDocument& doc, const UnsharpParams& params,
                                  TileStore* previewOut) {
  return computePixelFilter(doc, unsharpMaskTiles, params, previewOut);
}

FilterOpResult previewAddNoise(const OpenDocument& doc, const NoiseParams& params,
                               TileStore* previewOut) {
  return computePixelFilter(doc, addNoiseTiles, params, previewOut);
}

FilterOpResult previewEmboss(const OpenDocument& doc, const EmbossParams& params,
                             TileStore* previewOut) {
  return computePixelFilter(doc, embossTiles, params, previewOut);
}

FilterOpResult previewMedian(const OpenDocument& doc, const MedianParams& params,
                             TileStore* previewOut) {
  return computePixelFilter(doc, medianTiles, params, previewOut);
}

FilterOpResult previewMotionBlur(const OpenDocument& doc, const MotionBlurParams& params,
                                 TileStore* previewOut) {
  return computePixelFilter(doc, motionBlurTiles, params, previewOut);
}

// --------------------------------------------------------------------------
// PRD D8: the two make-tileable ops
// --------------------------------------------------------------------------

namespace {

// The canvas, which is what both of D8's ops need and neither engine can
// infer -- `ops/Filters.hpp` section 4's wrap modulus and section 10's
// statistics population. One function so the two ops cannot come to disagree
// about what "the document" means.
PixelRect canvasRectOf(const OpenDocument& doc) noexcept {
  return PixelRect{0, 0, doc.document.width, doc.document.height};
}

// Built once and used by both the preview and the commit of each op, for the
// reason app/FilterOps.hpp's `previewX()` section gives: a second copy of the
// params construction is how the two end up filtering with different numbers.
LightingGradientParams lightingGradientParamsFor(const OpenDocument& doc, float sigma) {
  LightingGradientParams params;
  params.sigma = sigma;
  params.statsRect = canvasRectOf(doc);
  return params;
}

OffsetParams offsetParamsFor(const OpenDocument& doc, const OffsetRequest& request) {
  OffsetParams params;
  params.dx = request.dx;
  params.dy = request.dy;
  params.edge = request.edge;
  params.wrapRect = canvasRectOf(doc);
  return params;
}

}  // namespace

FilterOpResult applyRemoveLightingGradient(OpenDocument& doc, float sigma) {
  return applyPixelFilter(doc, removeLightingGradientTiles,
                          lightingGradientParamsFor(doc, sigma), "remove lighting gradient");
}

FilterOpResult previewRemoveLightingGradient(const OpenDocument& doc, float sigma,
                                             TileStore* previewOut) {
  return computePixelFilter(doc, removeLightingGradientTiles,
                            lightingGradientParamsFor(doc, sigma), previewOut);
}

PixelCoord offsetByHalf(const OpenDocument& doc) noexcept {
  // `/ 2` on a non-negative extent, which is floor -- and the extent is
  // unsigned in the document, so there is no negative case for C's
  // round-toward-zero to differ on. Whole texels, because the op is an
  // addressing change: see this function's header comment.
  return PixelCoord{static_cast<int32_t>(doc.document.width / 2),
                    static_cast<int32_t>(doc.document.height / 2)};
}

PixelOpRefusal offsetRefusalFor(const OpenDocument& doc) noexcept {
  // The layer-shaped question first, the same one `computePixelFilter()` will
  // ask again on the way in -- asked here so the selection answer below can
  // never pre-empt "there is no layer at all", which has no fix a deselect
  // would help with.
  const PixelOpRefusal layer = pixelOpRefusalFor(activeLayerOf(doc));
  if (layer != PixelOpRefusal::None) return layer;
  if (doc.selection.has_value()) return PixelOpRefusal::SelectionActive;
  return PixelOpRefusal::None;
}

FilterOpResult applyOffset(OpenDocument& doc, const OffsetRequest& request) {
  FilterOpResult result;
  result.refusal = offsetRefusalFor(doc);
  if (result.refusal != PixelOpRefusal::None) return result;
  return applyPixelFilter(doc, offsetTiles, offsetParamsFor(doc, request), "offset");
}

FilterOpResult previewOffset(const OpenDocument& doc, const OffsetRequest& request,
                             TileStore* previewOut) {
  // The preview refuses on exactly the same question the commit does, rather
  // than quietly showing a torn document the button would then decline to
  // produce.
  FilterOpResult result;
  result.refusal = offsetRefusalFor(doc);
  if (result.refusal != PixelOpRefusal::None) return result;
  return computePixelFilter(doc, offsetTiles, offsetParamsFor(doc, request), previewOut);
}

DocumentOpOutcome applyImageSize(OpenDocument& doc, uint32_t width, uint32_t height,
                                 ResampleKernel kernel) {
  DocumentTransformParams params;
  params.pixels.kernel = kernel;
  Selection* selection = doc.selection.has_value() ? &*doc.selection : nullptr;
  const DocumentTransformResult r = resizeDocumentImage(doc.document, width, height, params,
                                                        selection);
  DocumentOpOutcome out;
  out.ok = r.ok;
  out.error = r.error;
  // `ok` is true for a request that left the extent unchanged too (D17's own
  // "must not perturb a value" rule for a 1:1 resize) -- comparing the
  // reported before/after is what tells "resized" apart from "asked to
  // resize to what it already was", the same distinction
  // `fillThroughSelection()`'s zero-texel return draws for the bucket.
  if (r.ok && (r.previousWidth != static_cast<int32_t>(width) ||
              r.previousHeight != static_cast<int32_t>(height))) {
    doc.recordEdit(r.editLabel, EditKind::Structural);
  }
  return out;
}

DocumentOpOutcome applyCanvasSize(OpenDocument& doc, uint32_t width, uint32_t height,
                                  CanvasAnchor anchor) {
  Selection* selection = doc.selection.has_value() ? &*doc.selection : nullptr;
  const DocumentTransformResult r =
      resizeDocumentCanvas(doc.document, width, height, anchor, selection);
  DocumentOpOutcome out;
  out.ok = r.ok;
  out.error = r.error;
  if (r.ok && (r.previousWidth != static_cast<int32_t>(width) ||
              r.previousHeight != static_cast<int32_t>(height))) {
    doc.recordEdit(r.editLabel, EditKind::Structural);
  }
  return out;
}

}  // namespace np
