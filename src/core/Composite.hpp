#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"

// core/Composite (PLAN.md "Phase 5 -- Stack it", step 1: "Multiple layers in
// `Document`, with reorder, visibility, lock, opacity").
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
// **`over` (Porter-Duff source-over), and nothing else.** PLAN.md Phase 5
// step 2 is `core/Blend` -- "the linear-safe set (over, plus, multiply,
// screen, min, max) and `Mix`, the KM latent lerp. Display-referred modes
// labelled as such (PRD B7)". That step owns the *set*; this one owns making
// one member of it real, because `over` is the only member whose absence
// makes `visible` and `opacity` meaningless.
//
// `compositeOver()` below is written as a standalone two-pixel function with
// no Document, no Layer and no loop in it, precisely so that step 2 can move
// it into `core/Blend` as the `over` entry of that set -- signature unchanged
// -- rather than re-deriving the arithmetic from a compositing loop it would
// first have to untangle. **When `core/Blend` lands, `compositeOver()` moves
// there verbatim and this file keeps only the document walk**, dispatching
// per layer through whatever enumeration step 2 chooses.
//
// Not here, on purpose:
//   - Any other blend mode. See `blendIsImplemented()` for what happens to a
//     layer that asks for one.
//   - A blend *enum*. `Layer::blend` stays a `std::string` -- core/Layer.hpp
//     argues that at length (PRD I10 value-level preservation is free with a
//     string, and step 2 owns the enumeration). Nothing here converts it.
//   - Layer masks (step 4), clipping masks (step 9), adjustment layers
//     (step 5) and groups. `Layer::parent` is still carried and never acted
//     on: this build creates no groups, and honouring a parent link means
//     compositing a group's members into an offscreen buffer first, which is
//     step 9's machinery, not this step's.
//   - Pigment/Media layers. They own no tile storage yet (core/Layer.hpp), so
//     they contribute nothing and are skipped exactly as before.
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

// `src` composited **over** `dst`, both premultiplied, linear-light RGBA.
//
//   out.rgb = src.rgb + dst.rgb * (1 - src.a)
//   out.a   = src.a   + dst.a   * (1 - src.a)
//
// `src` is the upper layer, `dst` the accumulated composite of everything
// beneath it. Argument order matches the way the operation is written and
// spoken ("src over dst"), which is worth more than matching a memcpy-style
// destination-first convention nothing else here uses.
//
// **The identity that makes this step's regression check possible**: when
// `src.a == 0` the result is exactly `dst` (`dst * (1 - 0)` is a
// multiplication by literal 1.0f, which is exact for every finite float), and
// when `dst` is all zeros the result is exactly `src`. So a document whose
// layers never overlap -- including every single-layer document -- composites
// to bit-identical output under this function and under the plain sum it
// replaces. That is asserted in `--selftest`, not merely reasoned about here.
//
// Not clamped. A working-space value may legitimately exceed 1.0 (color/
// Space.hpp: "whether to clamp is a display/export policy decision"), and
// io/Export already makes that decision at its own quantization step. Alpha
// above 1.0 is not defensively clamped either, because the only way to
// produce one is to write it into a tile by hand; `layerCoverage()` below
// clamps the one alpha multiplier this module actually introduces.
std::array<float, 4> compositeOver(const std::array<float, 4>& src,
                                   const std::array<float, 4>& dst) noexcept;

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

// Whether this build can honour `blend`. True for exactly one name today:
// core/Layer.hpp's `kDefaultBlendName` ("normal"), which is `over`.
//
// **A layer whose blend is anything else is composited as `over` and warned
// about by name -- never silently.** The alternative considered and rejected
// was refusing the document outright. Refusing is wrong here for a specific
// reason rather than a general preference: io/NpaintFile deliberately carries
// an unrecognised `np:blend` value through a load/save untouched (PRD I10, and
// core/Layer.hpp's whole argument for the member being a string), and the
// loader already tells the user a newer build's document "was opened anyway".
// If the compositor refused, that carefully preserved value would become the
// thing that makes the document unsaveable -- because part 0 is regenerated on
// **every** save (PRD I12), so no save could complete. A preserved attribute
// that bricks the file it was preserved in is worse than an approximate
// composite that says it is approximate.
//
// So the contract is: the pixels are an approximation, and every boundary that
// turns them into a durable artefact reports it. `saveNpaint()` surfaces the
// sentence in `NpaintSaveResult::warnings` (whose own doc comment is exactly
// this case: "the save went ahead, and the caller is told precisely what about
// it is approximate"), and `exportDocument()` in `ExportResult::warnings`.
bool blendIsImplemented(std::string_view blend) noexcept;

// The sentence `compositeDocumentPremultiplied()` emits for one such layer.
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
// Only the tiles that exist are walked (core/TileStore's own begin()/end()),
// never a grid across the canvas, so an empty or sparsely painted document
// costs nothing per unpainted tile. Skipping a layer's missing tile is exactly
// equivalent to compositing a transparent one, per `compositeOver()`'s
// identity above -- it is a cost saving, not an approximation.
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
