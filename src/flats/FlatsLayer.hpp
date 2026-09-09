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
// IT (PRD N1) -- or, when a layer below it is marked as the flatting
// REFERENCE, against exactly the marked layers (`flatsSourceLayers()` below)
// -- encoded to display sRGB first (PRD N10: ink extraction is a perceptual
// threshold and runs in the domain the artist judged it in). The evaluation
// is cached per layer, keyed on the content hash of the layer's own
// `FlatsContent` combined with a signature of THE LAYERS IT READ, so it runs
// when a parameter, a repair or the line art changes and at no other time --
// never per frame. That cache is the reason this file exists apart
// from core/VectorRaster: a vector rasterisation costs milliseconds and can
// be redone on a cache miss every composite; a rubber-sheet segmentation of a
// 2K plate costs seconds and cannot.
//
// The signature is built from what is cheap to read
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
//
// **And the source set is what decides which strokes count at all.** With
// nothing marked, a stroke on ANY layer below pays that cadence -- a colour
// rough, a note, a second drawing. Marking the line art means a stroke
// anywhere else changes no part of the signature and costs nothing.
// Measured on a 1024x1024 fixture with a rough between the art and the Flats
// layer: five dabs on the rough cost 228 ms of re-segmentation unmarked, and
// 0 ms with the art marked.

namespace np {

// Whether `doc.layers[index]` is a Flats layer this file evaluates.
bool flatsLayerEvaluable(const Document& doc, size_t index) noexcept;

// ==========================================================================
// WHAT a flats evaluation reads (the reference layer)
// ==========================================================================
//
// A Flats layer segments the drawing under it. "Under it" defaulted to the
// composite of every layer below, and still does -- but a layer may now be
// marked as the flatting REFERENCE (`core/Layer.hpp`'s `flatsReference`), and
// when any layer below the Flats layer is marked, exactly those are read
// instead.
//
// **This is a performance feature as much as a correctness one.** The
// evaluation cache is keyed on a signature of everything the evaluation read,
// so with the default source a colour rough, a note or a second drawing under
// the inks does not merely pollute the segmentation -- every edit to any of
// them re-runs it, and one evaluation is ~215 ms on a 1024x1024 document.
// Naming the line art removes that whole class of invalidation: painting on a
// layer that is not the reference now costs nothing at all.
std::vector<size_t> flatsSourceLayers(const Document& doc, size_t index);

// What the Paint Bucket's `Flats` mode segments when it bakes pixels onto an
// ordinary layer, which is a different question because there is no Flats
// layer to be "beneath".
//
// `ExcludeTarget` is the default and it fixes a real defect: the bake used to
// segment the WHOLE composite, the target layer included, so the second fill
// on a layer saw the first fill's pixels as line art and segmented a different
// set of regions.
enum class FlatsBakeSource { AllLayers, ExcludeTarget, ReferenceLayer };
struct FlatsBakeSourceRow { FlatsBakeSource mode; const char* label; const char* tip; };
inline constexpr size_t kFlatsBakeSourceCount = 3;
extern const FlatsBakeSourceRow kFlatsBakeSources[kFlatsBakeSourceCount];
std::vector<size_t> flatsBakeSourceLayers(const Document& doc, size_t targetIndex,
                                          FlatsBakeSource mode);

// The staleness key for an explicit set of source layers, and their composite
// as display-encoded straight RGBA8 (w*h*4). Both take the set rather than an
// index precisely so that the key covers what was READ and nothing else.
uint64_t flatsSourceSignature(const Document& doc, const std::vector<size_t>& layers);
std::vector<uint8_t> flatsSourceRgba8(const Document& doc, const std::vector<size_t>& layers);

// The evaluation of the Flats layer at `index`, from the cache or freshly
// computed. Never null for an evaluable layer; null otherwise.
std::shared_ptr<const FlatEvaluation> flatsEvaluateLayer(const Document& doc, size_t index);

// The evaluation of the Flats layer at `index` **from the cache only**, or
// null when the cache holds nothing current for it. NEVER computes.
//
// This is the read a per-frame UI wants, and the distinction is not
// pedantry. `flatsContentHash()` covers `FlatParams`, so every frame of a
// parameter drag is a fresh hash and therefore a cache miss -- and a miss in
// `flatsEvaluateLayer()` runs a whole rubber-sheet segmentation. A panel that
// merely wants to print "153 fills" beside the slider being dragged would
// pay that per frame, for a number, while the drag is deliberately not even
// recorded until release. So the panel peeks instead, and draws "--" for the
// handful of frames the drag is in flight.
std::shared_ptr<const FlatEvaluation> flatsPeekEvaluation(const Document& doc, size_t index);

// An evaluation of `content` over an explicit set of source layers, for a
// layer that is NOT a Flats layer -- the bucket's `Flats` mode on an RGB
// layer (ADR-0009). Cached the same way, under `cacheKey`, which is the
// target layer's id. The caller resolves the set with
// `flatsBakeSourceLayers()`; it is passed in rather than derived here
// because the bake's three source modes are a user choice, not a property of
// the document.
std::shared_ptr<const FlatEvaluation> flatsEvaluateSource(const Document& doc, uint64_t cacheKey,
                                                          const std::vector<size_t>& layers,
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
