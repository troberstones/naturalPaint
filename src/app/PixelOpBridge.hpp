#pragma once

#include <utility>

#include "app/DocumentLifecycle.hpp"
#include "app/FilterOps.hpp"
#include "app/StrokeSession.hpp"

// app/PixelOpBridge -- the two function templates every menu-driven pixel op
// in this application runs through, extracted from `app/FilterOps.cpp`'s
// anonymous namespace so that `app/AdjustmentOps` can share them rather than
// hand-copy them.
//
// **Why an extraction rather than a second copy.** `app/FilterOps.hpp`'s own
// header already argues the case for `previewX()` and `applyX()` sharing one
// implementation -- "a SECOND hand-written copy of 'run the engine, blend
// through the selection' is exactly how a preview and a commit end up
// computing two different answers". The Image > Adjustments menu is that same
// argument one level up: nineteen adjustment commands and seven filter
// commands ask the identical five questions -- which layer, does it refuse,
// what rectangle, how does the selection bound it, when is that one history
// entry recorded -- and the moment two files answer them separately, the two
// menus start disagreeing about what "the selected layer" means. They answer
// them here, once.
//
// Nothing in this header is new behaviour. It is `app/FilterOps.cpp`'s
// `computePixelFilter()`/`applyPixelFilter()` verbatim, moved; every comment
// they carried came with them, because every one of them is still the reason
// the line below it is written the way it is.
//
// **Header-only, and deliberately not a public API.** These are templates
// over an engine function and its params type, so they have to be visible at
// each call site; they live in `app/` rather than `ops/` because they know
// what an `OpenDocument`, a `Layer` and a `core::History` entry are, which is
// exactly the knowledge `ops/` is kept free of.
namespace np {

// The shape every menu pixel op shares: refuse by `PixelOpRefusal` before
// touching the engine, run it over the whole canvas rectangle
// (`app/FilterOps.hpp`'s own argument for why that is the correct rectangle
// and not merely the simple one), and composite the result back through the
// selection. `Engine` is any function with `ops/Blur`'s and `ops/Filters`'
// shared signature shape, `(const TileStore&, const PixelRect&, const
// Params&, TileStore*) -> bool` -- `blurTiles`, `unsharpMaskTiles`,
// `medianTiles`, and now `pointOpTiles` (ops/PointOpTiles.hpp), whose whole
// reason for matching that signature is to arrive here.
//
// **Split from `applyPixelFilter()` (docs/testing-issues.md T15)** so that a
// live preview and a commit are the SAME arithmetic rather than two
// implementations of it: this function computes the fully-composited result
// -- what the active layer would hold after the engine ran and
// `compositeFilterResult()` blended it through the selection -- and hands it
// back in `*previewOut` rather than writing it anywhere. `doc` is `const&`
// on purpose; nothing below can mutate it. `applyPixelFilter()` is the only
// place that takes what this returns and actually writes it, and that
// happens after this function has already returned, so there is exactly one
// line in this whole header where an op touches a live layer.
template <typename Engine, typename Params>
FilterOpResult computePixelFilter(const OpenDocument& doc, Engine engine, const Params& params,
                                  TileStore* previewOut) {
  FilterOpResult result;
  const Layer* target = activeLayerOf(doc);
  result.refusal = pixelOpRefusalFor(target);
  if (result.refusal != PixelOpRefusal::None) return result;

  // A copy, not a second reference to the live store -- see app/FilterOps.hpp's
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

  // Blend into a COPY of `original`, not into `target->rgbTiles` -- that copy
  // is what makes this function safe to call on a `const OpenDocument&`.
  // `compositeFilterResult()`'s own copy-on-write argument applies here
  // unchanged: `composed` starts by sharing every tile with `original` (an
  // O(tiles) refcount bump), and only the tiles the blend actually writes
  // fork a private copy, so an identity request or a fully-unselected canvas
  // costs neither an allocation nor a COW fork here either.
  TileStore composed = original;
  result.texelsChanged = compositeFilterResult(
      original, filtered, canvasRect, doc.selection.has_value() ? &*doc.selection : nullptr,
      composed);
  if (previewOut != nullptr && result.texelsChanged > 0) *previewOut = std::move(composed);
  return result;
}

template <typename Engine, typename Params>
FilterOpResult applyPixelFilter(OpenDocument& doc, Engine engine, const Params& params,
                                const char* editLabel) {
  TileStore composed;
  const FilterOpResult result = computePixelFilter(doc, engine, params, &composed);
  if (result.refusal != PixelOpRefusal::None || result.texelsChanged == 0) return result;

  // `computePixelFilter()` already did the work; this is the one line in the
  // file that makes it real. `target` is refetched (non-const this time)
  // rather than threaded through as an argument, so `computePixelFilter()`
  // never needs a mutable `Layer*` at all -- the const-correctness that lets
  // `previewX()` call it on a `const OpenDocument&`.
  Layer* target = activeLayerOf(doc);
  *target->rgbTiles = std::move(composed);
  doc.recordEdit(editLabel, EditKind::Content);
  return result;
}

}  // namespace np
