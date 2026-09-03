#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "brush/Deposit.hpp"
#include "core/Layer.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/VectorShape.hpp"

// app/PathConsumers -- PRD J1/J2/J3/J4: the three things a user does WITH a
// path once it exists.
//
//   J2  path -> selection   `pathToSelection()`
//   J1/J4  fill the path    `fillPathIntoLayer()`
//   J3  stroke it with the current brush  `strokePathWithBrush()`
//
// ==========================================================================
// 1. The unit of input is a SHAPE LIST, not a `Path`, and that is what makes
//    Text work everywhere Vector does
// ==========================================================================
//
// Every function here takes `const std::vector<VectorShape>&`. That is not a
// generalisation for its own sake: it is the type `core/TextContent.hpp`'s
// `textContentToShapes()` returns, so a `LayerKind::Text` layer reaches all
// three consumers through exactly the same call a `LayerKind::Vector` layer
// does, with no `kind` branch anywhere below `pathConsumerShapes()`.
//
// core/TextContent.hpp section 1 states the claim ("text is not a second
// rendering path") and core/VectorRaster.hpp already made it true for
// compositing, through `layerRastersToTiles()`. `pathConsumerShapes()` is the
// same predicate reused rather than `kind == Vector || kind == Text` spelled
// out a fourth time -- core/VectorRaster.hpp's own comment says why: there
// were three non-adjacent sites there and adding the kind to two of them
// left the third silently wrong.
//
// A `Path`-taking overload was considered and rejected. A bare `Path` carries
// no `clip` and no `FillRule`-per-shape, so the fill consumer would need a
// second, paint-less entry point whose behaviour would drift from this one's,
// and a caller with a single path spells it `{VectorShape{path, ...}}`.
//
// ==========================================================================
// 2. No intermediate framebuffer, in any of the three
// ==========================================================================
//
// core/PathRaster's whole memory argument is that it emits coverage *spans*
// and never allocates a destination, because its four consumers each want a
// different one. Two of them are here, and each writes straight into its own:
//
//   * `pathToSelection()`  -> 8-bit coverage into a `SelectionTile`.
//   * `fillPathIntoLayer()` -> source-over into the target's rgba16float tiles.
//
// So this module deliberately does NOT call `rasterizeVectorLayer()`, even
// though that function already paints fills and strokes correctly and is
// already tested. It returns a whole `TileStore` covering the path's
// footprint, which would then be composited into the target -- a second full
// allocation of exactly the area core/PathRaster exists to avoid allocating.
// The span loop below is that function's `paintCoverage()` with the
// destination changed from "a fresh store" to "the layer's own", plus the
// active selection folded into the same multiply the clip already uses.
//
// The one buffer that IS allocated is a shape's `clip` coverage, and that is
// core/VectorRaster.cpp's own choice for the same reason: a clip has to be
// *queried* per texel of the shape, not streamed alongside it, so it cannot
// be a span callback. It is a `Selection` -- sparse, tiled, 8-bit -- for
// core/VectorRaster.cpp's stated reason, that a clip is precisely "sparse,
// tiled, antialiased coverage in [0,1]" and a second type for it would exist
// only to be converted into this one.
//
// ==========================================================================
// 3. A selection change is NOT a document edit
// ==========================================================================
//
// app/DocumentLifecycle.hpp is explicit, for PRD E4/E8/E9's refine family:
// `core::HistoryEntry` holds nothing but a `core::Document`, so folding a
// selection into core::History "would make the History panel show a step with
// no pixel change, and would make an ordinary pixel Undo silently revert a
// selection drawn afterwards".
//
// **That rule is enforced here by the type system rather than by a comment.**
// `PathSelectionResult` has no `editLabel` member and `pathToSelection()` is
// handed no `Document`, no `OpenDocument` and no `Layer` -- it cannot call
// `recordEdit()` because it has nothing to call it about. `PathFillResult`
// and `PathStrokeResult` DO carry an `editLabel`, non-empty exactly when the
// operation changed a texel, because those two are edits and must be
// recorded. The asymmetry between the three result types is the assertion.
//
// ==========================================================================
// 4. Refusals name what is wrong, and there are no silent no-ops
// ==========================================================================
//
// core/Merge.cpp's Adjustment refusal is the house template: a sentence that
// says what the layer *is* and what the user can do about it, not "operation
// unsupported". Every `ok == false` below carries one. The set:
//
//   * a source layer of a kind that holds no parametric geometry;
//   * a Text layer with nothing typed in it yet;
//   * an empty path (no shape encloses anything representable);
//   * a path entirely outside the canvas;
//   * a locked target layer;
//   * a target layer with no tile store of a kind this can write;
//   * an alpha-locked target, **for the fill only** -- see section 5;
//   * a tip with no radius, for the stroke.
//
// ==========================================================================
// 5. Alpha lock: refused by the fill, HONOURED by the stroke
// ==========================================================================
//
// These two answers differ because the question does.
//
// A **fill** puts colour where the path is, including where the layer is
// wholly transparent -- that is the entire operation. core/Layer.hpp's
// `alphaLocked` freezes exactly the quantity a fill exists to move, and there
// is no alpha-locked composite for a fill anywhere in this build to reach
// for. Inventing one here would be a different piece of work with its own
// argument to make, so the fill refuses by name and says so.
//
// A **stroke** has one already: `brush/RgbDeposit.hpp` section 4.5 derives
// the alpha-locked composite in full (`out.rgb = dst.rgb*(1-a) + ink*a*dst.a`,
// `out.a = dst.a`), `depositRgbTexel()` implements it, and
// `RgbStroke::begin()` latches the flag for it. Refusing here would make a
// built, derived and tested path unreachable through this one caller. So
// `strokePathWithBrush()` threads `target.alphaLocked` into `begin()` and the
// stroke paints colour without growing the shape -- which is what the flag
// means everywhere else in the application, and what --selftest asserts.
//
// The eraser is a separate matter and is not reachable from here at all:
// `brush/RgbErase` is a different module and `app/StrokeSession.cpp`'s
// `strokeRouteFor()` is what refuses it on an alpha-locked layer by name.
namespace np {

// The flattening tolerance every function here uses, in DOCUMENT texels.
//
// The same tenth of a texel core/VectorRaster.cpp uses and for its stated
// reason: below what the 1/255 coverage quantisation downstream can express,
// so tightening it buys nothing visible while costing segments quadratically
// (core/PathFlatten's count goes as 1/sqrt(tol)).
//
// **Deliberately not a parameter.** core/PathFlatten.hpp's tolerance is in
// the space the points are in and exists so an on-screen overlay at 8x zoom
// can ask for an eighth of it. Nothing here draws to a screen -- all three
// consumers write document texels -- so there is no zoom for a caller to
// track and no second correct answer to offer them.
inline constexpr float kPathConsumerTolerancePx = 0.1f;

// The geometry a layer contributes, whichever of the two parametric kinds it
// is. Section 1.
struct PathShapesResult {
  bool ok = false;
  std::string error;  // a sentence, when !ok
  std::vector<VectorShape> shapes;
};

// `layer.shapes` for a Vector layer, `textContentToShapes(layer.text)` for a
// Text one, and a refusal naming the kind for anything else.
//
// Gated on core/VectorRaster.hpp's `layerRastersToTiles()` rather than on a
// hand-written kind test -- section 1.
//
// Three distinguishable failures, because they are three different things for
// a user to do about it: a kind that has no geometry at all, a Text layer the
// shaper could not shape (invalid UTF-8, or a build with no shaper), and a
// Text layer that shaped fine but is empty because nobody has typed in it yet.
PathShapesResult pathConsumerShapes(const Layer& layer);

// --- PRD J2: path -> selection --------------------------------------------

// No `editLabel`, deliberately and permanently. Section 3.
struct PathSelectionResult {
  bool ok = false;
  std::string error;  // a sentence, when !ok
  // Install directly into `Document::selection`. **Do not call
  // `recordEdit()`** -- app/DocumentLifecycle.hpp lines 257-265.
  Selection selection;
  // Texels of `selection` that ended up non-zero. For --selftest and for a
  // caller that wants to tell "combined to nothing" from "did nothing".
  size_t selectedTexels = 0;
};

// Rasterise the shapes' enclosed area as coverage and combine it with `base`
// under `op`.
//
// `base` may be null, which is "no selection is installed" -- and note that
// under `Replace`, `Add` and `Intersect` this is NOT the same as passing a
// `selectAll()`: an absent selection means "no restriction" for editing
// (core/SelectionMask.hpp) but it is the empty *operand* for this algebra,
// which is what makes Shift-dragging a first path give that path rather than
// the whole canvas.
//
// `op` is a `SelectionCombine`, so a caller resolves the user's modifiers
// with `selectionCombineFromModifiers(shift, alt)` and this matches the
// modifier grammar of every other selection tool in the build. The combine
// itself is `combineSelections()`; nothing here re-implements the algebra.
//
// **PAINT IS IGNORED; GEOMETRY AND CLIP ARE NOT.** A shape with `fill.on ==
// false` still contributes its enclosed area -- a path encloses what it
// encloses whether or not anyone chose to paint it, which is what "make a
// selection from this path" means. A shape's `clip`, on the other hand, is
// honoured, so this and `fillPathIntoLayer()` cannot disagree about which
// texels a clipped shape covers.
//
// Overlapping shapes union with `max`, through `combineCoverage(...,
// SelectionCombine::Add)` -- core/SelectionOps.hpp's fuzzy-set rule, called
// rather than re-typed, so the within-call union and the with-`base` union
// are the same arithmetic.
PathSelectionResult pathToSelection(const std::vector<VectorShape>& shapes,
                                    const Selection* base, SelectionCombine op,
                                    int32_t width, int32_t height);

// --- PRD J1/J4: fill ------------------------------------------------------

struct PathFillResult {
  bool ok = false;
  std::string error;     // a sentence, when !ok
  std::string editLabel; // hand to recordEdit(); non-empty when a texel moved
  size_t texelsChanged = 0;
};

// Paint the shapes into `target`'s own tiles: each shape's fill, then its
// stroke, bottom shape to top -- SVG's order, and core/VectorRaster.cpp's,
// "the only order that makes a stroke read as an outline rather than as a
// band under the fill".
//
// `target` must be a `LayerKind::RGB` layer with engaged `rgbTiles`; anything
// else refuses by name. A Pigment layer is refused rather than approximated:
// its texel is a straight latent plus a mass and has no alpha channel a
// premultiplied source-over could write (core/Pigment.hpp), so filling one is
// a different arithmetic and not a parameter on this one.
//
// `selection` bounds the fill, PRD E1 -- null means no restriction, which is
// core/SelectionMask.hpp's convention and not its inverse. It multiplies the
// coverage exactly once, the same single multiply `fillThroughSelection()`
// (the paint bucket) makes, because a fill has no per-stroke accumulator for
// brush/RgbDeposit.hpp section 4's second entry to bound.
//
// Refuses an alpha-locked target by name -- section 5.
PathFillResult fillPathIntoLayer(Layer& target, const std::vector<VectorShape>& shapes,
                                 const Selection* selection, int32_t width, int32_t height);

// --- PRD J3: stroke with the current brush --------------------------------

struct PathStrokeResult {
  bool ok = false;
  std::string error;     // a sentence, when !ok
  std::string editLabel; // hand to recordEdit(); non-empty when a texel moved
  size_t dabs = 0;
  size_t texelsChanged = 0;
};

// Run `tip` along every subpath, as one stroke.
//
// **The dab emitter is `brush/StrokePath`, and there is no second one.** Each
// contour is flattened by core/PathFlatten and its points are fed to one
// `StrokePath` through `addPoint()`, closed by re-feeding the first point when
// the subpath is closed, and finished with `flush()`. `spacingPx` is
// `BrushTip::spacingPx()`, so this stroke lays dabs at the identical spatial
// spacing a hand-drawn one does (ADR-0003).
//
// **`flush()` is not optional here, and its job is bigger than it looks.**
// `addPoint()` lags one real sample behind by design, so it never walks the
// last segment of a contour; `flush()` is the only thing that does. Both it
// and the closing-point feed above therefore protect the SAME property -- that
// the stroke reaches the end of the path -- and dropping either leaves the
// walk short by one flattening step, which is `sqrt(4 * tol * L / 3)`, about
// 5 px on a 220 px edge at this file's tolerance. A brush wider than that gap
// hides it completely, which is why --selftest probes it with a deliberately
// fine tip.
//
// What `flush()` does NOT do from here is its single-click dab: that rule
// fires for a stroke whose samples never moved, and no such stroke can reach
// it, because `flattenPath()` drops every subpath with fewer than two anchors
// and `pathIsEmpty()` has already refused a path made only of those. A click
// that paints one dab is `app/StrokeSession`'s case, not this one's.
//
// Feeding a *flattened* polyline to a Catmull-Rom emitter does mean the dab
// centres follow a spline through the polyline's vertices rather than the
// original cubic. At `kPathConsumerTolerancePx` the vertices are within a
// tenth of a texel of the true curve and are dense, so the spline is within
// that of it too -- far below one dab of the softest tip. The alternative,
// evaluating the cubic directly at arc-length intervals, is a second dab
// emitter, which this file exists not to be.
//
// **All contours share ONE stroke**, hence one `RgbStroke` and one
// accumulator: "Stroke Path" is a single command, so two subpaths that cross
// must not compound past `tip.opacity` any more than a hand-drawn stroke
// crossing itself does (brush/RgbDeposit.hpp section 2). The `StrokePath`
// emitter itself IS reset per contour, because leftover arc length carried
// from one contour into the next would move every dab of the second --
// `StrokePath::reset()`'s own stated reason.
//
// Targets: `LayerKind::RGB` with `rgbTiles` (through `brush/RgbDeposit`'s
// `RgbStroke`, using `tip.linearRgb` and `tip.opacity`) and
// `LayerKind::Pigment` with `pigmentTiles` (through `brush/Deposit`'s
// `depositDabs()`, using `tip.pigment`). Both routes already exist and take
// the same arguments in the same order; neither is re-implemented here.
//
// `selection` bounds the deposit, PRD E1, and is passed straight through to
// whichever route -- brush/RgbDeposit.hpp section 4 is where the two-entry
// rule that makes it a bound rather than a speed limit lives.
//
// **Honours an alpha-locked RGB target rather than refusing it** -- section 5.
PathStrokeResult strokePathWithBrush(Layer& target, const std::vector<VectorShape>& shapes,
                                     const BrushTip& tip, const Selection* selection,
                                     int32_t width, int32_t height);

}  // namespace np
