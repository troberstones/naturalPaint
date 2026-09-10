#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/Document.hpp"
#include "core/StrokesContent.hpp"
#include "core/TileStore.hpp"

// brush/StrokesLayer -- how a `LayerKind::Strokes` layer reaches the
// compositor. PLAN.md phase 8; PRD C1, C11, D6.
//
// flats/FlatsLayer's job for the other derived kind, and deliberately its
// shape: an evaluation of a parameter member against WHAT LIES BENEATH THE
// LAYER, cached on a key that covers both halves, rasterised into tiles that
// core/VectorRaster's materialised view hands the compositor in place of the
// layer.
//
// **It lives in brush/ and not in core/ for one reason: `dabCoverage()`.** A
// stored dab's footprint has to be the footprint a painted dab has, or a
// Strokes layer would draw a second, subtly different brush. That function
// lives in brush/Deposit, brush/ already depends on core/, and core/ does not
// depend on brush/ -- so the evaluation comes here rather than dragging the
// dependency the other way. core/StrokesContent holds the model, which is
// what core/Layer.hpp needs and all it needs.
//
// ==========================================================================
// 1. Samples-only-from-below
// ==========================================================================
//
// A dab whose `source` is `DabColorSource::Below` reproduces the composite of the
// layers BENEATH its own layer -- indices `[0, index)`, in the document's own
// stack order -- sampled at the dab's centre plus its `sourceDx`/`sourceDy`.
// core/StrokesContent section 2 argues why "below" rather than "the whole
// document": the whole document includes this layer's own output, so
// evaluation would read its own result and have no fixed point.
//
// The consequence is PRD D6, and it is the property phase 8 is named for: add
// an Adjustment layer under a Strokes layer and the clone marks re-read the
// GRADED pixels on the next evaluation. Nothing about the dab records
// changes; they were never pixels.
//
// The below-composite is computed **only when some dab actually asks for
// it**. A layer of ordinary recorded paint (`DabColorSource::Ink` throughout) pays
// nothing at all for this section -- not the composite, and not the
// signature that would key it.
//
// ==========================================================================
// 2. Checkpoint tiles
// ==========================================================================
//
// PLAN.md phase 8 asks for these by name, and the sentence to read it against
// is "replaying every dab from the beginning must not be the only way to
// evaluate the layer". A stroke is hundreds of dabs; a drawing is tens of
// thousands. Replay is linear in the dab count and it happens on a cache
// miss, so without checkpoints the cost of adding one dab to a layer is the
// cost of every dab already on it.
//
// A checkpoint is the tile store as it stood after the first `n` dabs, plus
// `strokesPrefixHash(content, n)`. On a miss, the evaluation takes the LAST
// checkpoint whose prefix hash still matches -- i.e. whose first `n` dabs are
// still those dabs -- copies its tiles and replays only from there.
//
// **The copy is what makes this affordable, and it is a `TileStore` copy**:
// core/TileStore shares tiles copy-on-write, so a checkpoint costs the slot
// map and the tiles the replay after it actually writes into. It is the same
// economics core/History depends on to hold a whole `Document` per undo
// entry.
//
// **An edit in the MIDDLE of the list invalidates every checkpoint after
// it**, which is what the prefix hash detects and why the hash is over a
// prefix rather than over the whole content. Appending -- the common case, a
// stroke being recorded -- invalidates none. Deleting (PRD F11) invalidates
// from the first deleted dab onward, which is honest: those tiles were built
// over pixels that are no longer there.
//
// ==========================================================================
// 3. The cache key covers BOTH halves
// ==========================================================================
//
// `strokesContentHash()` for the records, and -- only when some dab samples
// from below -- `flatsSourceSignature()` for what lies beneath. The second is
// reused rather than re-implemented on purpose: it is the same question ("did
// anything I READ change?"), it already handles an invisible layer
// contributing its identity and nothing else, and a second copy of it would
// be a second thing to get wrong about copy-on-write tile identity. See
// flats/FlatsLayer.hpp for what the signature actually covers.
//
// **And that reuse brings flats/FlatsLayer's CADENCE with it, which is a
// property worth stating rather than discovering.** The signature is built
// from tile-slot IDENTITY, and core/TileStore's copy-on-write barrier
// replaces a slot's pointer only when a SHARED tile is written. So an
// in-place write to a tile no history snapshot shares -- the later dabs of
// one live stroke on a layer beneath -- does not move the signature, and a
// Strokes layer re-reads what is under it at the START of such a stroke and
// again when the edit is recorded, not on every dab. For a layer whose marks
// reproduce what is beneath them that is the intended cadence and not a bug:
// re-compositing every layer below on every dab of a stroke is exactly the
// per-frame cost this cache exists to refuse.

namespace np {

// Whether `doc.layers[index]` is a Strokes layer this file evaluates.
bool strokesLayerEvaluable(const Document& doc, size_t index) noexcept;

// The layers a Strokes layer at `index` samples from: `[0, index)`. Section
// 1. A function rather than a loop at each call site because it is the one
// place the samples-only-from-below rule is written down as code, and
// `--selftest` asserts against it rather than against a re-derivation.
std::vector<size_t> strokesSourceLayers(const Document& doc, size_t index);

// The composite of `strokesSourceLayers()`, LINEAR and PREMULTIPLIED,
// `w * h * 4` floats -- the compositor's own output format, unconverted.
//
// Unlike flats/FlatsLayer's display-encoded sRGB8 equivalent, which encodes
// because ink extraction is a perceptual threshold judged in the display
// domain (PRD N10). A dab reproduces what is underneath it; converting to
// sRGB8 and back would quantise it to 8 bits for no reason at all.
std::vector<float> strokesSourceComposite(const Document& doc, size_t index);

// `content` composited into linear premultiplied tiles, in paint order.
//
// `below` is the buffer `strokesSourceComposite()` returns and may be empty,
// in which case every `DabColorSource::Below` dab contributes nothing -- which is
// the honest answer for a layer at the bottom of the stack, where there IS
// nothing beneath to reproduce, and the answer a one-shot caller with no
// document (a test, a fixture) gets.
//
// Pure: no cache, no document, no checkpoints. The cached, checkpointed path
// is `strokesLayerTiles()` below, which is implemented in terms of this
// function's per-dab step so the two can never disagree about what a dab
// looks like.
TileStore strokesRasterize(const StrokesContent& content, int width, int height,
                           const std::vector<float>& below);

// The cached tiles for the Strokes layer at `index`, built on a miss and
// resumed from the newest still-valid checkpoint (section 2). Never null for
// an evaluable layer; null otherwise.
std::shared_ptr<const TileStore> strokesLayerTiles(const Document& doc, size_t index);

// Drop cache entries for layers no longer in `doc`; drop everything (a
// document closed, or a test wanting a cold cache). flats/FlatsLayer's pair,
// with flats/FlatsLayer's contract.
void strokesForgetLayersNotIn(const Document& doc);
void strokesForgetAll();

// Entries resident, and how many checkpoints the entry for `layerId` holds.
//
// The second is for `--selftest` and it is the only way to see section 2 at
// all: a checkpointed replay and a from-scratch replay produce identical
// tiles by construction, so no assertion over PIXELS can tell whether
// checkpoints exist, are taken, or are correctly invalidated. This exposes
// the mechanism so it can be asserted directly. Zero for an unknown layer.
size_t strokesCacheEntryCount() noexcept;
size_t strokesCheckpointCount(uint64_t layerId) noexcept;

// How many dabs the LAST evaluation of `layerId` actually replayed, as
// opposed to how many the layer holds. `--selftest`'s handle on the claim
// section 2 makes: appending one dab to a long layer must replay a few, not
// all of them. Zero for an unknown layer.
size_t strokesLastReplayedDabs(uint64_t layerId) noexcept;

// How many dabs a checkpoint is taken every. Exposed because `--selftest`
// has to size a fixture against it, and a test that hard-codes 256 beside an
// implementation that says 512 asserts nothing.
size_t strokesCheckpointInterval() noexcept;

}  // namespace np
