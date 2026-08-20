#pragma once

#include <cstddef>

#include "core/Document.hpp"
#include "core/Layer.hpp"

// core/TileShare (PLAN.md "Phase 5 -- Stack it", step 6: "COW tiles --
// copy-on-write with reference-counted history"; PRD A9, O1, O4).
//
// The mechanism is in core/TileStore.hpp -- a tile is a `std::shared_ptr<T>`
// slot, copying a store shares every tile, and the two barriers
// (`getOrCreate`, `findForWrite`) copy a shared tile before handing out a
// writable reference. That header carries the whole design argument.
//
// **This header is the other half of what step 6 owes step 7: the same two
// numbers, summed over a Layer and over a Document.** Step 7's history is a
// linear list with a cursor whose *eviction budget is in bytes* (PRD A9,
// "bounded in bytes, not steps"), and a history entry is a shared copy of a
// document. So the question it will ask on every edit is "what does this
// entry cost me", and the honest answer is not one number:
//
//   documentTileBytes(d)           what the entry *shows*: every tile it
//                                  refers to. Equal to what a deep copy would
//                                  have cost, and therefore the number to
//                                  quote when saying what copy-on-write
//                                  saved.
//   documentExclusiveTileBytes(d)  what dropping the entry would give *back*:
//                                  only the tiles no other document refers
//                                  to. This is the one a byte-budget must
//                                  spend, and using the first would make a
//                                  bounded history evict the whole tail to
//                                  reclaim nothing.
//
// A worked example, which is exactly the shape step 7 will see and which
// `--selftest`'s `cow tiles` section builds and measures: ten history entries
// over a 256-tile (32.0 MiB) document, one tile edited between each.
//
//   deep-copied, as before this step   11 x 32.0 MiB = 352.0 MiB
//   actually held                      256 + 10 tiles = 33.25 MiB   (10.6x)
//   documentTileBytes() per entry      32.0 MiB -- what it *shows*
//   documentExclusiveTileBytes()       128 KiB for the OLDEST entry, and
//                                      **zero for every entry after it**
//
// That last row is the one to read twice, because it is counter-intuitive and
// a byte budget written against the wrong intuition will free nothing. A
// middle entry holds no tile alone: the version of tile *k* it carries is
// also carried by every entry taken before the edit to tile *k*, and the
// version it carries of every *later*-edited tile is also carried by every
// entry after it. Only the ends of the chain hold anything exclusively.
//
// **So the exclusive number is a lower bound on a multi-entry eviction, not
// an additive one.** Dropping one entry returns exactly its exclusive bytes.
// Dropping k entries returns *at least* the sum, and in the example above it
// returns **1.25 MiB against a sum of 0.125 MiB** -- ten times more, because
// a tile two doomed entries shared becomes free only when the second goes.
// A budget that evicts until the sum of exclusives covers the overrun will
// over-evict; one that re-measures after each drop will not.
//
// --- What is deliberately not here ---------------------------------------
//
// **No history, no entry list, no cursor, no compression, no mmap spill.**
// All of that is step 7 and PLAN.md says so; building an entry type here
// would be inventing step 7's central decision as a side effect of this one,
// which is the same mistake io/TileResidency.hpp refused to make with Phase 4
// step 8's open-document record.
//
// **No dirty set.** app/Journal.hpp's deferral names this header's step as
// the natural home for one ("the write barrier that marks a tile dirty is the
// same barrier that copies it"), and it is right that the barrier is the
// place -- but a dirty set has to be *cleared* by something, and the thing
// that would clear it is a journal flush or a history commit, neither of
// which exists at a granularity finer than the document. A set nothing clears
// is a set that is always full. Unblocked by step 7's entry boundary, which
// is the natural clear point.
//
// **No `shareDocument()`.** Copying already shares -- `Document b = a;` is
// the share, `core::duplicateLayer` and `app::duplicateDocument` already say
// exactly that, and adding a second spelling would mean two ways to do one
// thing with no way to tell which a call site meant.
namespace np {

// --- Per-layer ------------------------------------------------------------

// Tiles this layer refers to, across all three of its stores (RGB *or*
// pigment -- at most one is engaged, per core/Layer.hpp's invariant -- plus
// the mask, which is orthogonal to both and counts separately).
inline size_t layerTileCount(const Layer& layer) noexcept {
  size_t n = 0;
  if (layer.rgbTiles) n += layer.rgbTiles->occupiedTileCount();
  if (layer.pigmentTiles) n += layer.pigmentTiles->occupiedTileCount();
  if (layer.mask) n += layer.mask->occupiedTileCount();
  return n;
}

// Bytes of tile payload this layer refers to, shared or not. Deliberately
// sums the three stores' own `tileBytes()` rather than multiplying a count by
// a size: a mask tile is 32 KiB, an RGB tile 128 KiB and a pigment tile
// 224 KiB, and a single "tiles x 128 KiB" would be wrong for two of the three
// (core/Mask.hpp, core/Pigment.hpp).
inline size_t layerTileBytes(const Layer& layer) noexcept {
  size_t bytes = 0;
  if (layer.rgbTiles) bytes += layer.rgbTiles->tileBytes();
  if (layer.pigmentTiles) bytes += layer.pigmentTiles->tileBytes();
  if (layer.mask) bytes += layer.mask->tileBytes();
  return bytes;
}

// Bytes no other store refers to: what destroying this layer would return.
inline size_t layerExclusiveTileBytes(const Layer& layer) noexcept {
  size_t bytes = 0;
  if (layer.rgbTiles) bytes += layer.rgbTiles->exclusiveTileBytes();
  if (layer.pigmentTiles) bytes += layer.pigmentTiles->exclusiveTileBytes();
  if (layer.mask) bytes += layer.mask->exclusiveTileBytes();
  return bytes;
}

inline size_t layerSharedTileCount(const Layer& layer) noexcept {
  size_t n = 0;
  if (layer.rgbTiles) n += layer.rgbTiles->sharedTileCount();
  if (layer.pigmentTiles) n += layer.pigmentTiles->sharedTileCount();
  if (layer.mask) n += layer.mask->sharedTileCount();
  return n;
}

// Makes every tile this layer refers to unique -- the explicit deep copy.
inline void unshareLayerTiles(Layer& layer) {
  if (layer.rgbTiles) layer.rgbTiles->unshareAll();
  if (layer.pigmentTiles) layer.pigmentTiles->unshareAll();
  if (layer.mask) layer.mask->unshareAll();
}

// --- Per-document ---------------------------------------------------------

inline size_t documentTileCount(const Document& doc) noexcept {
  size_t n = 0;
  for (const Layer& layer : doc.layers) n += layerTileCount(layer);
  return n;
}

inline size_t documentTileBytes(const Document& doc) noexcept {
  size_t bytes = 0;
  for (const Layer& layer : doc.layers) bytes += layerTileBytes(layer);
  return bytes;
}

inline size_t documentExclusiveTileBytes(const Document& doc) noexcept {
  size_t bytes = 0;
  for (const Layer& layer : doc.layers) bytes += layerExclusiveTileBytes(layer);
  return bytes;
}

inline size_t documentSharedTileCount(const Document& doc) noexcept {
  size_t n = 0;
  for (const Layer& layer : doc.layers) n += layerSharedTileCount(layer);
  return n;
}

// The deep copy, spelled out: `Document deep = source; unshareDocumentTiles(deep);`
//
// Two real uses, and neither is "because sharing is scary". (1) It is the
// pre-step-6 behaviour, so `--selftest` can time the old and the new cost in
// the same binary rather than quoting a number from a previous commit.
// (2) It is how a caller hands a document to a thread that will outlive the
// original's mutations *without* the conservative-copy rule in
// core/TileStore.hpp -- app/Journal's deferred background writer wants the
// shared copy, not this, but a future caller that wants isolation by value
// has one call to make.
inline void unshareDocumentTiles(Document& doc) {
  for (Layer& layer : doc.layers) unshareLayerTiles(layer);
}

}  // namespace np
