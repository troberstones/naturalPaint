#include "app/FilterOps.hpp"

#include <array>

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

namespace {

// The shape every `applyX()` below shares: refuse by `PixelOpRefusal` before
// touching the engine, run it over the whole canvas rectangle (this header's
// own argument for why that is the correct rectangle and not merely the
// simple one), and composite the result back through the selection. `Engine`
// is one of `blurTiles`, `sharpenTiles`, `unsharpMaskTiles`, `addNoiseTiles`
// -- same signature shape, `(const TileStore&, const PixelRect&, const
// Params&, TileStore*) -> bool` -- so this is written once instead of copied
// four times with one line changed in each copy, which is precisely the
// "shares an implementation" failure the task's own selftest brief warns
// against; the four callers below still each name their own engine function
// and their own params type, so there is exactly one place they could
// silently start sharing a radius, and it is not this one.
template <typename Engine, typename Params>
FilterOpResult applyPixelFilter(OpenDocument& doc, Engine engine, const Params& params,
                                const char* editLabel) {
  FilterOpResult result;
  Layer* target = activeLayerOf(doc);
  result.refusal = pixelOpRefusalFor(target);
  if (result.refusal != PixelOpRefusal::None) return result;

  // A copy, not a second reference to the live store -- see this header's
  // "why the original is copied" section. `TileStoreOf`'s copy constructor
  // shares every tile (an O(tiles) refcount bump), so this costs nothing
  // proportional to the document's pixels.
  const TileStore original = *target->rgbTiles;
  const PixelRect canvasRect{0, 0, doc.document.width, doc.document.height};

  TileStore filtered;
  if (!engine(original, canvasRect, params, &filtered)) {
    // Every engine here refuses only for a reason the dialog should already
    // have prevented (invalid params, a null/aliased destination, an empty
    // rectangle) -- none of which is a `PixelOpRefusal`, so `result.refusal`
    // stays `None` and `texelsChanged` stays 0. The op is a no-op, not a
    // crash, which matches "the click is a click on the canvas" bucket's own
    // comment: a request the engine could not honour still records nothing
    // rather than corrupting the layer.
    return result;
  }

  result.texelsChanged = compositeFilterResult(
      original, filtered, canvasRect, doc.selection.has_value() ? &*doc.selection : nullptr,
      *target->rgbTiles);
  if (result.texelsChanged > 0) doc.recordEdit(editLabel, EditKind::Content);
  return result;
}

}  // namespace

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
