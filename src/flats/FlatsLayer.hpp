#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/Document.hpp"
#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "flats/Model.hpp"

// flats/FlatsLayer -- how a `LayerKind::Flats` layer reaches the compositor,
// and how the paint bucket reaches a flat. ADR-0009.
//
// A Flats layer evaluates against THE COMPOSITE OF THE VISIBLE LAYERS BENEATH
// IT (PRD N1), encoded to display sRGB first (PRD N10: ink extraction is a
// perceptual threshold and runs in the domain the artist judged it in). The
// evaluation is cached per layer, keyed on the content hash of the layer's
// own `FlatsContent` combined with a signature of what lies beneath, so it
// runs when a parameter, a repair or the line art changes and at no other
// time -- never per frame. That cache is the reason this file exists apart
// from core/VectorRaster: a vector rasterisation costs milliseconds and can
// be redone on a cache miss every composite; a rubber-sheet segmentation of a
// 2K plate costs seconds and cannot.
//
// The signature of the layers beneath is built from what is cheap to read
// without touching a pixel: each layer's id, kind, visibility, opacity,
// blend, clip flag, and -- for the raster kinds -- the identity of every
// occupied tile slot. core/TileStore's copy-on-write barrier replaces a
// slot's pointer whenever a shared tile is written, and every history
// snapshot shares every tile, so the first stroke after any recorded edit
// changes the signature. What the signature does NOT see is an in-place
// write to a tile that no snapshot shares -- the later dabs of one stroke --
// so a Flats layer re-flats at the START of each stroke on the line art and
// again when the edit is recorded, not on every dab. That is the intended
// cadence for an operation of this cost.

namespace np {

// Whether `doc.layers[index]` is a Flats layer this file evaluates.
bool flatsLayerEvaluable(const Document& doc, size_t index) noexcept;

// A signature of the visible layers beneath `index` -- see above.
uint64_t flatsBeneathSignature(const Document& doc, size_t index);

// The composite beneath `index` as display-encoded straight RGBA8, w*h*4.
std::vector<uint8_t> flatsBeneathRgba8(const Document& doc, size_t index);

// The evaluation of the Flats layer at `index`, from the cache or freshly
// computed. Never null for an evaluable layer; null otherwise.
std::shared_ptr<const FlatEvaluation> flatsEvaluateLayer(const Document& doc, size_t index);

// An evaluation of `content` against the composite beneath `index` for a
// layer that is NOT a Flats layer -- the bucket's `Flats` mode on an RGB
// layer (ADR-0009). Cached the same way, keyed on the target layer's id.
// `index == doc.layers.size()` segments the WHOLE composite ("sample all
// layers"), which is what the bake wants: the line art may be on the very
// layer being filled, or above it.
std::shared_ptr<const FlatEvaluation> flatsEvaluateBeneath(const Document& doc, size_t index,
                                                           const FlatsContent& content);

// The evaluation painted into linear, premultiplied tiles -- what the
// materialise loop hands the compositor in place of the layer.
TileStore flatsRasterize(const FlatEvaluation& e);

// The cached tiles for the Flats layer at `index`, built on a miss.
std::shared_ptr<const TileStore> flatsLayerTiles(const Document& doc, size_t index);

// Drop cache entries for layers no longer in `doc`.
void flatsForgetLayersNotIn(const Document& doc);
// Drop everything (a document closed, or a test wanting a cold cache).
void flatsForgetAll();
// Entries resident, for the memory panel and for --selftest.
size_t flatsCacheEntryCount() noexcept;

// The fill under a click as a coverage selection over its labels: the
// bucket's region in `Flats` mode. Empty when `fillId` is 0.
Selection flatsFillSelection(const FlatEvaluation& e, int fillId);

}  // namespace np
