#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"

// core/Composite (PLAN.md "Phase 5 -- Stack it", step 1: "Multiple layers in
// `Document`, with reorder, visibility, lock, opacity"; step 2 moved the blend
// arithmetic out to core/Blend and left the document walk here).
//
// --- Why this module exists at all ----------------------------------------
//
// Visibility and opacity are not properties a layer can *have* in isolation;
// they are properties of how a layer meets the layers under it. Before this
// step `Layer::visible` and `Layer::opacity` were inert values with no
// consumer (core/Layer.hpp said so outright), and the only thing that reduced
// a Document to an image -- io/Export's `flattenDocumentToLinear()` -- was a
// **plain sum** over every RGB layer, correct only under the invariant "at
// most one layer holds painted content at a given point". That invariant is
// what this step destroys, so the sum had to be replaced by real compositing
// in the same step that makes stacking possible. There was never a version of
// this step that did not include a compositor.
//
// --- What is here, and what deliberately is not ---------------------------
//
// **The document walk, and nothing else.** Step 1 shipped `compositeOver()`
// from this file as a standalone two-pixel function with no Document, no Layer
// and no loop in it, and said in these words: "when `core/Blend` lands,
// `compositeOver()` moves there verbatim and this file keeps only the document
// walk, dispatching per layer through whatever enumeration step 2 chooses."
// **That is exactly what happened.** `compositeOver()` is now declared in
// core/Blend.hpp with its body unchanged, along with `blendPixel()` (the
// per-mode dispatch), `blendIsImplemented()` (which moved from here) and the
// `BlendMode` enumeration. This header re-exports none of them; it includes
// core/Blend.hpp so its existing callers keep compiling, which is the one
// concession to churn made here.
//
// What this file still owns is the part that is about a *Document*: walk the
// layers bottom to top, skip the kinds that hold no pixels, apply
// `visible`/`opacity` as coverage, resolve each layer's blend name to a mode
// once per layer rather than once per texel, and collect the warnings a name
// this build cannot honour produces.
//
// Not here, on purpose:
//   - The blend arithmetic (core/Blend).
//   - Layer masks (step 4), clipping masks (step 9), adjustment layers
//     (step 5) and groups. `Layer::parent` is still carried and never acted
//     on: this build creates no groups, and honouring a parent link means
//     compositing a group's members into an offscreen buffer first, which is
//     step 9's machinery, not this step's.
//   - Pigment/Media layers. They own no tile storage yet (core/Layer.hpp), so
//     they contribute nothing and are skipped exactly as before. That is also
//     why a `Mix` layer never reaches the arithmetic: PRD L5 restricts `Mix`
//     to a Pigment layer over a Pigment layer, and neither holds a texel.
//
// --- Linear light, premultiplied -----------------------------------------
//
// Both are the codebase's existing invariants, not new choices here.
// core/Tile stores premultiplied ("associated") rgba16float in the working
// space (DESIGN-imaging.md §2), io/ImageIO premultiplies on import, and
// core/Probe / io/Export un-premultiply at their read boundaries. Compositing
// happens *inside* that boundary: the whole walk below is premultiplied, and
// exactly one un-premultiply happens at the very end, in the caller. That
// ordering is the same one core/Probe.cpp's `sumPremultipliedBox()` already
// argues for at length -- an alpha-0 texel must contribute "no colour", not
// "black at full weight", and only premultiplied arithmetic gives that for
// free.
namespace np {

// The scalar a layer's premultiplied texels are multiplied by before it is
// composited: `layer.opacity` clamped to [0,1], or exactly 0.0f when the
// layer is hidden.
//
// **Opacity is a coverage multiplier, not an alpha replacement**, and on
// premultiplied values that distinction is what makes it a single multiply:
// scaling colour-times-alpha and alpha by the same factor is exactly "the
// same colour, covering less". A layer at opacity 0.5 whose texel alpha is
// 0.5 therefore contributes effective alpha 0.25 -- the two compose, neither
// overrides the other. `--selftest` asserts that composition rather than
// leaving it to be inferred.
//
// Clamps rather than refuses, because `Layer::opacity` is a public member of
// a plain aggregate with no mutator to validate in (core/Layer.hpp), so an
// out-of-range value can exist in memory even though `core/LayerOps`' setter
// refuses one and io/NpaintFile refuses to save one. `!(o > 0)` rather than
// `std::max` so a NaN lands on 0 instead of propagating through the whole
// canvas.
float layerCoverage(const Layer& layer) noexcept;

// **A layer whose blend this build cannot composite is composited as `over`
// and warned about by name -- never silently.** Two cases reach here and the
// sentence distinguishes them, because the answer to "when will this work" is
// different:
//
//   * a name outside `BlendMode` entirely (a newer build's "linear-burn"), and
//   * `mix`, which this build knows the name of and cannot apply, because
//     `Mix` lerps *latents* and no layer stores one until PLAN.md Phase 5
//     step 3's Pigment tiles (core/Blend.hpp's `mixLatents()` argues it).
//
// The alternative considered and rejected for both was refusing the document
// outright. Refusing is wrong here for a specific reason rather than a general
// preference: io/NpaintFile deliberately carries an unrecognised `np:blend`
// value through a load/save untouched (PRD I10, and core/Layer.hpp's whole
// argument for the member being a string), and the loader already tells the
// user a newer build's document "was opened anyway". If the compositor
// refused, that carefully preserved value would become the thing that makes
// the document unsaveable -- because part 0 is regenerated on **every** save
// (PRD I12), so no save could complete. A preserved attribute that bricks the
// file it was preserved in is worse than an approximate composite that says it
// is approximate.
//
// So the contract is: the pixels are an approximation, and every boundary that
// turns them into a durable artefact reports it. `saveNpaint()` surfaces the
// sentence in `NpaintSaveResult::warnings` (whose own doc comment is exactly
// this case: "the save went ahead, and the caller is told precisely what about
// it is approximate"), and `exportDocument()` in `ExportResult::warnings`.
//
// Names the layer by index, by its user-facing name when it has one, and by
// the blend it asked for -- the io/Export refusal style, applied to a warning.
std::string unimplementedBlendWarning(size_t layerIndex, const Layer& layer);

// Composites every RGB-kind layer of `doc` **bottom to top** into one
// premultiplied, linear-light RGBA buffer of `doc.width * doc.height * 4`
// floats, row-major, top-to-bottom, no padding.
//
// **Bottom to top is `doc.layers` front to back**, and that is fixed by the
// file format rather than chosen here: docs/document-format.md says "Part
// order is layer order, bottom to top, after part 0", and io/NpaintFile writes
// `doc.layers[0]` as the first layer part. `core::Document`'s own member
// comment has said "ordered, bottom-to-top" since Phase 2. The layers *panel*
// shows the same vector top-first (app/LayerPanel.hpp owns that one mapping),
// which is the conventional presentation and the reason the two must be stated
// together rather than each assuming the other.
//
// Returns an empty vector for a non-positive canvas. A document with no
// layers, or no RGB-kind layer, is not an error -- it composites to fully
// transparent black.
//
// Each layer's `blend` is resolved to a `BlendMode` **once per layer**, not
// once per texel: the name is a `std::string` and a string comparison inside
// the inner loop would be the one place this walk could plausibly get slow.
//
// Only the tiles that exist are walked (core/TileStore's own begin()/end()),
// never a grid across the canvas, so an empty or sparsely painted document
// costs nothing per unpainted tile. Skipping a layer's missing tile is exactly
// equivalent to compositing a transparent one, per core/Blend.hpp's
// transparent-source identity -- which holds for **every** mode, not only
// `over`, so this remains a cost saving and not an approximation whatever the
// layer's blend is.
//
// Content outside the canvas rectangle is clipped away, matching what
// io/Export's flattener has always done: a composite is of the document's
// canvas, not of its content's bounding box.
//
// `warningsOut`, when non-null, gains one `unimplementedBlendWarning()`
// sentence per layer whose blend this build could not honour. It is appended
// to, never cleared, so a caller can collect from several stages.
std::vector<float> compositeDocumentPremultiplied(
    const Document& doc, std::vector<std::string>* warningsOut = nullptr);

}  // namespace np
