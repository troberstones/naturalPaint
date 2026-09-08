#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/PathRaster.hpp"
#include "core/TileStore.hpp"
#include "core/VectorShape.hpp"

// core/VectorRaster -- how a `LayerKind::Vector` layer reaches the compositor.
//
// ==========================================================================
// 1. Why the raster is not stored on the layer
// ==========================================================================
//
// The tempting design is to keep the rasterised result in the layer's own
// `rgbTiles` and let `layerHoldsPixels()` (core/Composite.cpp) return true for
// Vector, so the whole existing RGB path picks it up unchanged. It was tried
// on paper and rejected on three measured grounds, recorded here because the
// idea is attractive enough to be re-proposed:
//
//  1. **`layerHoldsPixels()` is the guard, not the dispatch.** core/Composite
//     re-derives `kind == RGB && rgbTiles` inline at its own walk, and so do
//     core/Probe (seven sites) and ui/TransformPreviewTexture. Flipping the
//     guard would enable a layer the dispatch still skips.
//  2. **Two of those sites would then be undefined behaviour.** Both the
//     opaque-floor pre-warm and the clip fold read
//     `if (!layerHoldsPixels(l)) continue; if (kind == RGB) ...rgbTiles...
//     else ...*l.pigmentTiles...` -- a Vector layer passes the guard, fails
//     the RGB test, and dereferences a disengaged `std::optional`.
//  3. **History would collapse.** core/History holds a whole `Document` by
//     value and is affordable only because unchanged tiles are shared. A
//     geometry edit re-rasterises the *whole path footprint*, so consecutive
//     entries would share nothing: roughly 32 MiB per undo step for a
//     full 2048^2 layer against a 128 MiB budget, i.e. about four steps, and
//     `enforceBudget()`'s cheap early-out would fail permanently so that every
//     `amend()` -- every mouse-move of a drag -- ran a full O(all-slots) scan.
//
// So the layer stores geometry, which is kilobytes, and the raster lives
// outside the document entirely.
//
// ==========================================================================
// 2. Materialisation, and why it is a view rather than a fourth branch
// ==========================================================================
//
// `MaterializedDocument` hands the compositor a `Document` in which every
// Vector layer has been replaced by an ordinary RGB layer whose `rgbTiles` is
// the cached raster. core/Merge.cpp already builds a local sub-`Document` and
// composites that, so the pattern is established rather than invented.
//
// Two properties make this cheap and safe:
//
//   * **Zero cost when there is nothing to do.** A document with no Vector
//     layer is not copied at all; `get()` returns the original. Every
//     existing caller therefore pays nothing.
//   * **The copy is shallow.** `TileStore` shares its tiles, so rewriting the
//     layer vector copies slot maps, not pixels -- the same class of copy
//     ui/DocumentTexture already makes once per frame.
//
// The result is that `core/Composite` needs no Vector branch, and neither do
// Probe, Merge, Export or the opaque-floor logic. **A materialised document is
// for compositing only and must never be saved**: its Vector layers claim to
// be RGB, and writing that out would turn editable geometry into pixels.
//
// ==========================================================================
// 3. Cache invalidation is by content hash, not by a counter
// ==========================================================================
//
// The cache is keyed by `vectorContentHash()` (core/VectorShape.hpp), not by a
// revision counter that mutation sites must remember to bump. The failure mode
// of a counter is a mutation that forgets it, and the symptom is a stale
// raster -- an edit that silently does not appear. Hashing makes the question
// answerable from the data. See core/VectorShape.hpp for the cost argument.
namespace np {

// Paint one layer's shapes into a fresh tile store, in linear premultiplied
// rgba16float -- the same representation an RGB layer holds, so the result is
// interchangeable with one.
//
// Shapes paint bottom to top, and within a shape the fill paints before the
// stroke, which is SVG's order and also the only order that makes a stroke
// read as an outline rather than as a band under the fill.
//
// `width`/`height` clip the result to the document. Geometry outside is not
// rasterised at all rather than being allocated and discarded.
TileStore rasterizeVectorLayer(const std::vector<VectorShape>& shapes, int32_t width,
                               int32_t height);

// One raster per Vector layer, keyed by that layer's id and content hash.
//
// Exactly one entry per layer, never a history of them: a layer only ever
// needs its current appearance, so the cache is bounded by the number of
// Vector layers rather than by how many edits have happened. That is the
// property that keeps this from becoming the memory problem section 1
// rejected.
class VectorRasterCache {
 public:
  // The cached raster for `layerId` if it was built from exactly `hash`.
  // Null on a miss, including when the layer is present at a different hash --
  // a stale entry is never returned, which is the whole point of section 3.
  std::shared_ptr<const TileStore> lookup(uint64_t layerId, uint64_t hash) const;

  // Install a raster, replacing whatever that layer had.
  std::shared_ptr<const TileStore> store(uint64_t layerId, uint64_t hash, TileStore tiles);

  // Drop entries for layers no longer in `doc`, so deleting a Vector layer
  // does not leave its raster resident for the rest of the session.
  void forgetLayersNotIn(const Document& doc);

  size_t entryCount() const noexcept { return byLayer_.size(); }
  // Resident bytes across every cached raster, for the memory panel and for
  // --selftest to assert against rather than estimate.
  size_t residentBytes() const noexcept;

 private:
  struct Entry {
    uint64_t hash = 0;
    std::shared_ptr<const TileStore> tiles;
  };
  std::unordered_map<uint64_t, Entry> byLayer_;
};

// A compositing view of a document: see section 2.
//
// Holds a reference to `doc`, so it must not outlive it. `cache` may be null,
// in which case every Vector layer is rasterised afresh -- correct, and what
// a one-shot caller such as an export wants rather than growing a cache it
// will immediately discard.
class MaterializedDocument {
 public:
  MaterializedDocument(const Document& doc, VectorRasterCache* cache);

  const Document& get() const noexcept {
    return rewritten_.has_value() ? *rewritten_ : *original_;
  }

  // True when at least one Vector layer was replaced. False means `get()`
  // returned the original document untouched, and is the fast path every
  // existing caller takes.
  bool rewrote() const noexcept { return rewritten_.has_value(); }

 private:
  const Document* original_ = nullptr;
  std::optional<Document> rewritten_;
};

// Which layer kinds hold parametric content that this file turns into tiles:
// `Vector` (its `shapes`), since PLAN.md phase 14 `Text` (its `text`, via
// `textContentToShapes()`), and since phase 16 `Flats` (its `flats`, via
// flats/FlatsLayer's own cached evaluation -- see the materialise loop).
//
// **A predicate rather than `kind == Vector || kind == Text` spelled out at
// each site**, because there are THREE sites in this file and they are not
// adjacent: the materialise loop, `documentHasVectorLayers()`, and
// `VectorRasterCache::forgetLayersNotIn()`. Adding `Text` to the first two and
// missing the third leaks a raster for every deleted Text layer for the rest
// of the session, and nothing observable goes wrong until the memory panel is
// read -- the same class of silent partial fan-out `layerHoldsPixels()`'s own
// hand-copied duplicates caused, which section 1 above is about.
bool layerRastersToTiles(LayerKind kind) noexcept;

// Whether `doc` has any layer of a kind `layerRastersToTiles()` names. The
// one-line test every caller of `MaterializedDocument` implicitly makes;
// exposed because callers that only want to know "is there parametric content
// here" should not have to build a materialised view to find out.
//
// The name predates `Text` joining `Vector` and is kept because every caller
// spells it, and because what it actually answers -- "does materialising this
// document do anything?" -- has not changed.
bool documentHasVectorLayers(const Document& doc) noexcept;

}  // namespace np
