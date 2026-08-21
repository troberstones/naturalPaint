#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// core/LayerGeometry (PLAN.md "Phase 5 -- Stack it", step 11; PRD C13: "Align
// and distribute selected layers, to each other or to the canvas").
//
// ==========================================================================
// (1) WHY THIS FILE EXISTS: **a layer has no position, so aligning one means
//     moving its pixels**
// ==========================================================================
//
// core/LayerComp.hpp §1 wrote the finding down one step ago and refused a third
// of PRD C14 on it: `core::Layer` has no offset, origin or transform field of
// any kind, and tiles are keyed in **absolute document coordinates**
// (core/TileStore.hpp). Nothing about that changed here, and this step does not
// change it: **there is still no per-layer transform.**
//
// C13 is nevertheless deliverable, because align does not need a transform. It
// needs two things a layer's *pixels* can answer:
//
//   a **content bounding box** -- which document pixels this layer actually
//   occupies, so "its left edge" is a number;
//   a **translate** -- move those pixels by an integer offset.
//
// Both are here, both are real, and the second is what core/LayerComp.hpp said
// would have to exist first. What is still *not* here, and is refused by name
// rather than approximated, is a **transform**: rotate, scale, skew, or any
// translate by a fractional pixel. Every one of those resamples, and resampling
// is Phase 6 ("Filter and transform it") -- it needs a filter kernel choice, a
// premultiplied-alpha argument, and on a Pigment layer a decision about whether
// a latent triple may be interpolated at all (DESIGN-imaging.md §3: "any op
// that is a linear combination of pixels stays valid in latent space" -- a
// resample is, a filter with negative lobes is not). None of those is a field.
//
// So the honest boundary, stated once:
//
//   **An integer-pixel translate is exact and lossless. Anything else is a
//   transform, and this build has none.**
//
// `translateLayer()` takes `int32_t` deltas for that reason -- there is no
// float overload to round silently -- and `alignLayerSet()` rounds its own
// target to an integer and *reports the residual* rather than pretending the
// half pixel did not exist.
//
// ==========================================================================
// (2) WHY THE TRANSLATE IS LOSSLESS, structurally rather than carefully
// ==========================================================================
//
// It moves **raw half-float words**. `core::Tile`, `core::PigmentTile` and
// `core::MaskTile` each store `std::array<uint16_t, kTexelCount>` and each
// hands out `data()` for exactly this kind of bulk move ("HALF in, HALF out, no
// conversion", core/Pigment.hpp). Nothing here decodes a word to `float` and
// re-encodes it, so there is no rounding step that *could* lose a bit -- which
// is a stronger claim than "the error measured zero", and `--selftest` asserts
// it at **zero tolerance** on the words themselves, not on decoded values.
//
// Two consequences worth naming:
//
//   * It works unchanged for all three tile types, including the 7-channel
//     Pigment one. A latent triple is moved, never mixed, so a translate cannot
//     turn one pigment into another -- the failure mode DESIGN-imaging.md warns
//     about for every op that is *not* a linear combination of pixels.
//   * A tile that does not exist keeps meaning what its type says it means. The
//     gather fills a destination tile by **default-constructing** it and
//     overwriting only the texels a source tile actually covers, so a
//     `MaskTile` arriving from outside the layer's occupied set is 1.0
//     ("reveal", core/Mask.hpp's one non-zero default) rather than 0.0. Zeroing
//     it would have turned every mask translate into a partial erase.
//
// ==========================================================================
// (3) THE TWO PATHS, AND WHAT EACH COSTS
// ==========================================================================
//
//   **whole-tile** (both deltas exact multiples of `kTileSize`): a re-key of
//   the map, `TileStoreOf::rekeyTiles()`. **Zero bytes copied**, sharing
//   preserved. Align-to-canvas of a layer whose content starts on a tile
//   boundary lands here.
//
//   **sub-tile**: a gather. Each source tile's rows are scattered into the up
//   to four destination tiles they straddle, two `memcpy` runs per row at most.
//   Cost is the destination set, which is up to 4x the source set transiently
//   while it is built, and after pruning is at most 4x and usually about 2x.
//   **Destination tiles that came out exactly equal to a default-constructed
//   tile are dropped**, which is what keeps PRD C2 ("tiles allocate only where
//   content exists") true across a nudge -- an all-transparent tile and an
//   all-reveal mask tile are each indistinguishable from absent, so dropping
//   them is exact rather than approximate.
//
// `--selftest` prints both costs, measured.
//
// ==========================================================================
// (4) WHAT THE BOUNDING BOX INCLUDES, AND WHAT IT DOES NOT
// ==========================================================================
//
// **Content is non-zero alpha for an RGB layer and non-zero mass for a Pigment
// one**, tested on the stored half word (`word & 0x7fff`), so +0 and -0 are
// both empty and every other bit pattern -- including a denormal, an infinity
// and a NaN a file may carry -- is content. Those are the two channels that
// decide whether a texel contributes anything at all: values are premultiplied
// (core/Premultiply.hpp), so a zero-alpha RGB texel adds nothing through
// `over`, and mass 0 projects to fully transparent (core/Pigment.hpp).
//
// **The mask is not consulted.** A mask constrains what a layer *shows*, so an
// argument exists for intersecting with it -- but an engaged mask with no tiles
// reveals everywhere, and "everywhere" has no bounding box, so the intersection
// would be undefined in exactly the common case. The rule is therefore the
// simple one: bounds are the bounds of the layer's own pixels. The mask still
// **translates with the layer**, by the same delta, which is the property that
// actually matters -- a move that left the mask behind would slide content out
// from under its own coverage.
//
// **Bounds are not clipped to the canvas.** Tiles live in absolute document
// coordinates and content outside `[0,width) x [0,height)` genuinely exists --
// core/Merge already warns about it rather than pretending otherwise -- so a
// layer whose content hangs off the left edge has a negative `minX`, and
// aligning it to the canvas moves that content too.
//
// **A layer with no pixel storage has empty bounds**, not zero-sized bounds at
// the origin: an Adjustment layer occupies no document pixel, and reporting
// `0,0` would put it in every alignment as a point at the corner.
namespace np {

// A rectangle of document pixels, **inclusive** on both axes, or nothing.
//
// Inclusive rather than half-open because every producer here is a scan that
// finds a last occupied pixel, and the two conversions between the conventions
// are precisely where an off-by-one in an alignment hides. `empty` is a
// separate flag rather than an encodable degenerate rectangle so that "this
// layer has no content" cannot be mistaken for "this layer occupies one pixel
// at the origin".
struct LayerBounds {
  bool empty = true;
  int32_t minX = 0;
  int32_t minY = 0;
  int32_t maxX = 0;
  int32_t maxY = 0;

  int32_t width() const noexcept { return empty ? 0 : maxX - minX + 1; }
  int32_t height() const noexcept { return empty ? 0 : maxY - minY + 1; }

  friend bool operator==(const LayerBounds&, const LayerBounds&) = default;
};

// The layer's occupied pixels. Section 4 states exactly what counts.
LayerBounds layerContentBounds(const Layer& layer);

// The union of two boxes; an empty box is the identity.
LayerBounds unionLayerBounds(const LayerBounds& a, const LayerBounds& b);

// The canvas as a box: `[0,width-1] x [0,height-1]`, empty when either
// dimension is zero (which is representable -- a `Document` default-constructs
// to 0x0).
LayerBounds documentCanvasBounds(const Document& doc);

// Moves the layer's pixels -- and its mask, by the same delta -- by an integer
// number of document pixels. Positive `dx` moves right, positive `dy` down,
// matching `TileCoord`'s own axes.
//
// **Refusals**, each with the numbers, in core/LayerOps' style:
//
//   an out-of-range index;
//   a **locked** layer -- moving a layer's pixels is the most content-y edit
//     there is, so `locked`'s stated scope ("a locked layer's content and its
//     own place in the stack are frozen", core/LayerOps.hpp) covers it without
//     needing a new rule;
//   a layer with **no pixel storage at all** -- an Adjustment layer, or one of
//     the inert kinds. There is nothing to move, and succeeding silently would
//     let an align report that it moved five layers when it moved four.
//
// A layer whose storage is engaged but **empty** is not refused: it succeeds
// and moves nothing, because "this layer could hold pixels and currently holds
// none" is a state a user reaches by adding a layer, and refusing it would make
// a fresh layer un-alignable for a reason that is about its history.
//
// `dx == dy == 0` succeeds and changes nothing, matching `moveLayer(from ==
// to)`: a no-op the user asked for is not an error.
LayerOpResult translateLayer(Document& doc, size_t index, int32_t dx, int32_t dy);

// --- The tile-level operation, exposed so `--selftest` can drive it directly
// on all three tile types without going through a `Layer` --------------------
//
// Returns a new store holding `in`'s content moved by (dx, dy) document pixels.
// Section 2 argues losslessness; section 3 the two paths and their costs.
template <class T>
TileStoreOf<T> translatedTileStore(const TileStoreOf<T>& in, int32_t dx, int32_t dy) {
  if (dx == 0 && dy == 0) return in;

  if (floorMod(dx, kTileSize) == 0 && floorMod(dy, kTileSize) == 0) {
    TileStoreOf<T> out = in;  // shares every slot; the re-key copies nothing
    out.rekeyTiles(floorDiv(dx, kTileSize), floorDiv(dy, kTileSize));
    return out;
  }

  // The gather. Scatter each source row into the (at most two) destination
  // tiles it straddles, one memcpy per run of same-destination texels.
  std::unordered_map<TileCoord, std::unique_ptr<T>> built;
  for (const auto& [sourceCoord, sourceTile] : in) {
    const PixelCoord origin = tileOrigin(sourceCoord);
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const int32_t docY = origin.y + ly + dy;
      const int32_t destTileY = floorDiv(docY, kTileSize);
      const int32_t destLocalY = floorMod(docY, kTileSize);
      int32_t lx = 0;
      while (lx < kTileSize) {
        const int32_t docX = origin.x + lx + dx;
        const int32_t destTileX = floorDiv(docX, kTileSize);
        const int32_t destLocalX = floorMod(docX, kTileSize);
        const int32_t run = std::min(kTileSize - lx, kTileSize - destLocalX);
        std::unique_ptr<T>& slot = built[TileCoord{destTileX, destTileY}];
        if (!slot) slot = std::make_unique<T>();
        std::memcpy(slot->data() + (static_cast<size_t>(destLocalY) * kTileSize +
                                    static_cast<size_t>(destLocalX)) *
                                       static_cast<size_t>(T::kChannels),
                    sourceTile.data() + (static_cast<size_t>(ly) * kTileSize +
                                         static_cast<size_t>(lx)) *
                                            static_cast<size_t>(T::kChannels),
                    sizeof(uint16_t) * static_cast<size_t>(run) *
                        static_cast<size_t>(T::kChannels));
        lx += run;
      }
    }
  }

  const T defaultTile{};
  TileStoreOf<T> out;
  for (const auto& entry : built) {
    if (std::memcmp(entry.second->data(), defaultTile.data(),
                    sizeof(uint16_t) * T::kTexelCount) == 0)
      continue;  // indistinguishable from absent -- see section 3
    out.getOrCreate(entry.first) = *entry.second;
  }
  return out;
}

}  // namespace np
