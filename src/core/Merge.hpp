#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"

// core/Merge (PLAN.md "Phase 5 -- Stack it", step 10: "The merge family --
// merge down, merge visible, stamp visible, flatten, and rasterise a
// parametric layer"; PRD C10 (P0): "Merge down, merge visible, **stamp
// visible** (merge visible to a new layer), flatten image"; PRD C11 (P1):
// "Rasterise a parametric layer -- Text, Adjustment, Strokes, Flats -- into
// pixels").
//
// ==========================================================================
// §1  Why these are here and not in core/LayerOps
// ==========================================================================
//
// Every operation in core/LayerOps moves, hides, renames or re-flags a layer.
// Not one of them reads a texel. Each of the five below **computes new
// pixels**, which is a different dependency (core/Composite, and through it
// core/Blend, core/Pigment and ops/PointOps) and a different failure mode: a
// reorder that is wrong is visibly wrong, a merge that is wrong is a picture
// that changed by a fraction of a bit and a set of tiles that can no longer be
// taken apart. They share core/LayerOps' `LayerOpResult` and its refusal
// idiom -- there is exactly one shape for "a layer operation refused, and here
// is the number" -- and nothing else.
//
// They also share nothing with `Bake`. PLAN.md is explicit that this step is
// "distinct from `Bake`, which flattens one layer's *op stack* and leaves the
// layer". Every operation here **destroys or creates a Layer**; `Bake` would
// destroy an `OpStack` and leave the layer where it was.
//
// ==========================================================================
// §2  There is one document walk, and this file does not contain a second one
// ==========================================================================
//
// The obvious way to write a merge is a loop over two layers' tiles that
// blends them. It is also the bug this whole file is arranged to avoid: that
// loop would be a **second** implementation of visibility, opacity, masks,
// blend-mode resolution, the Pigment projection, `Mix` pairing, per-layer op
// stacks, adjustment layers and clipping runs -- nine behaviours that
// core/Composite already implements once, and nine chances for the merged
// pixels to differ subtly from the pixels the merge was supposed to preserve.
//
// So **every operation here composites through
// `compositeDocumentPremultiplied()`**, by building a `Document` holding the
// layers the operation is about and handing it to that one walk:
//
//   merge down          a two-layer sub-document: the pair, alone.
//   merge visible       the document itself.
//   stamp visible       the document itself.
//   flatten             the document itself.
//   rasterise           layers [0 .. index], i.e. the adjustment layer and
//                       everything it transforms.
//
// Building a sub-document is cheap and it is cheap for a reason this phase
// already paid for: `core::TileStoreOf` is copy-on-write (PLAN.md Phase 5
// step 6), so `sub.layers.push_back(doc.layers[i])` shares every tile rather
// than copying it. A two-layer sub-document of a 32 MiB document costs the
// `unordered_map` nodes and nothing else; `--selftest` prints the number.
//
// **What it does cost is the composite buffer**, and that is the honest price
// of the reuse: `compositeDocumentPremultiplied()` returns
// `width * height * 4` floats, which is 64 MiB for a 2048x2048 canvas
// regardless of how few tiles the layers actually occupy. A hand-written
// per-tile merge would allocate only over the tiles it touched. That is the
// one thing the rejected alternative is better at, it is measured rather than
// waved away (`mergeCompositeBufferBytes()` below returns the number, and
// `--selftest` prints it beside the tile bytes the merge actually keeps), and
// it buys the property that makes a merge trustworthy: the merged pixels come
// out of the same function that drew the pixels on screen.
//
// The one exception is `mergeLayerDown()`'s **latent** path (§5), which never
// builds a composite buffer at all -- because it is not compositing.
//
// ==========================================================================
// §3  Canvas clipping: what a merge through the compositor discards
// ==========================================================================
//
// `compositeDocumentPremultiplied()` is of the document's **canvas**, not of
// its content's bounding box: "content outside the canvas rectangle is clipped
// away, matching what io/Export's flattener has always done". A merge that
// goes through it therefore **discards content outside the canvas**, which a
// layer's tiles can perfectly well hold (`tileCoordAt()` uses floor division
// exactly so a layer may extend past the origin in either direction).
//
// That is a real loss and it is reported rather than hidden: every operation
// below appends a warning naming the number of occupied tiles that fell
// entirely outside the canvas and the bytes they held. It is not refused,
// because the alternative -- a merge that refuses whenever any layer has ever
// been dragged past an edge -- would make the P0 operation unusable on exactly
// the documents that most need it, and because the pixels a merge produces are
// the pixels the user is looking at.
//
// The latent Pigment path (§5) does **not** clip to the canvas, because it
// walks tiles rather than a canvas buffer. The asymmetry is stated rather than
// smoothed over: it is a consequence of the two paths being different
// computations, not a policy.
//
// ==========================================================================
// §4  Merge down, and the exact conditions under which it preserves the
//     picture
// ==========================================================================
//
// This is the property that makes a merge trustworthy, so it is stated as an
// equation rather than as an intention. Write `B` for the composite of
// everything below the pair, `L` for the lower layer, `U` for the upper, and
// `M` for the merged layer. Merging is appearance-preserving exactly when
//
//     blend_U( blend_L(B, L), U )  ==  over(B, M)                    (*)
//
// and `M` is computed as `over(transparent, over(L, U))` -- the pair, alone.
//
// `over` is associative on premultiplied values, so (*) holds identically when
// both `blend_L` and `blend_U` are `over`. **It does not hold when either is
// anything else**, and not by a little: `multiply` combines its layer with the
// *whole backdrop*, so folding `U` into `L` changes the picture the moment
// anything at all sits below the pair. There is no merged layer that
// reproduces it, because the information the blend consumed (`B`) is not in
// the pair.
//
// So merge down **refuses a pair in which either layer's blend is not
// `normal`**, names the layer and the mode, and points at the two operations
// that *are* exact for such a layer -- Merge Visible and Flatten -- because
// they collapse the backdrop too and therefore have no `B` left to disagree
// about. Photoshop merges anyway and lets the picture change; this codebase's
// standing rule is the opposite one (core/Composite.hpp §7: never silently).
//
// Everything else about the pair is **baked, not refused**, and each is
// baked because the compositor already folds it into the pair's own pixels:
//
//   opacity        a coverage multiplier. `M`'s alpha carries it and `M`
//                  ends at opacity 1. Exact.
//   mask           coverage again, one more multiply (core/Composite.hpp §5).
//                  `M` ends with no mask, and the merge warns -- the mask was
//                  separable and is not any more.
//   op stack       the compositor runs it before it blends, so `M` holds the
//                  graded pixels and ends with an empty stack. The merge
//                  warns: a non-destructive grade became a destructive one,
//                  which is the whole content of `Bake` happening as a side
//                  effect of a different gesture.
//   clipping       `M` takes the lower layer's `clipped` flag. Two of the four
//                  arrangements are exact, one is refused, and §6 works
//                  through all four.
//
// And two are **refused** rather than baked, for reasons that are not about
// arithmetic:
//
//   a hidden layer     merging a hidden layer discards its pixels and changes
//                      nothing on screen -- the most silently destructive
//                      operation available. Refused by name. Opacity 0 is
//                      *not* refused, and the asymmetry is deliberate:
//                      opacity is a continuous appearance property that bakes
//                      exactly, visibility is a switch the user expects to be
//                      able to flip back.
//   a clip base with   if the layer directly above the pair is `clipped`, it
//   members above      is clipped by the upper layer's alpha -- and merging
//                      unions that alpha with the lower layer's, so the member
//                      above would show through where it used to be cut away.
//                      (*) fails one layer higher up than it is written.
//                      Refused, naming the layer above.
//
// ==========================================================================
// §5  Pigment: the one case where a merge can stay in latent space, and the
//     refusal everywhere else
// ==========================================================================
//
// PRD C3 (P0) is `Mix`, and `Mix` lives in latents; PRD F10's eraser "reduces
// ... Mass on Pigment layers leaving the Latent untouched". So a Pigment layer
// merged into RGB is not a Pigment layer that lost a little precision -- it is
// a layer that can no longer be mixed and can no longer be erased as paint.
// Falling back to RGB silently would spend a P0 feature to satisfy a P0
// requirement, which is not a trade this file is allowed to make quietly.
//
// **The `over` case has no latent answer at all**, and this is worth being
// precise about because "just mix them" is the tempting wrong move. `over`
// between two Pigment layers is a *glaze*: an upper film of paint at partial
// mass sitting on top of a lower one, whose appearance is the two reflectances
// seen in series. `Mix` is a *mixture*: one film of paint whose pigment
// weights are a convex combination of the two. They are different physical
// situations and they produce different colours -- that is exactly why PRD C3
// makes `Mix` **opt-in** rather than what pigment layers do by default. There
// is no `(latent, mass)` pair whose projection equals a glaze, because a
// single latent has no notion of two films at different depths. So merging an
// `over` pair of Pigment layers in latent space would not be lossy, it would
// be *wrong*.
//
// **The `Mix` case does have one, and it is exact.** core/Composite's mixed
// pair is, at full coverage and with empty op stacks,
//
//     Lmix = mixLatents(low.latent, up.latent, up.mass)
//     mmix = up.mass + low.mass * (1 - up.mass)
//
// and its projected texel is `projectPigmentTexel({Lmix, mmix})` -- the same
// function applied to the same two numbers that a single Pigment layer holding
// `(Lmix, mmix)` would be projected through. So a Pigment layer holding those
// values **is** the pair, to within the f16 the tile stores them in, and it is
// still a Pigment layer: it can be mixed again, erased as mass, and saved as
// latents.
//
// `mergeLayerDown()` therefore takes that path, and only that path, when every
// condition that makes the equality hold is met: both layers Pigment, the
// upper's blend `Mix`, the pair actually paired by `core::mixPairing()` in the
// real document, both at opacity 1, neither masked, both op stacks empty,
// neither clipped, and no `Mix` layer directly above whose pairing the merge
// would change. Each condition that fails produces its own sentence naming
// what to clear first. Anything else involving a Pigment layer is refused, and
// the refusal says which of C3 or F10 it is protecting.
//
// (Op stacks are a *condition* here rather than something baked, unlike the
// RGB path, and core/Composite.hpp §1 is the reason: "the op stack runs after
// that projection", so a graded pigment texel is not a pigment texel. Baking a
// curve into `c0..c2` would not be a grade of the colour, it would be a
// different pigment.)
//
// ==========================================================================
// §6  Merge down and clipping, all four arrangements
// ==========================================================================
//
//   neither clipped      an ordinary pair. `M` is unclipped.
//   upper clipped        the lower layer is the upper's base (it is the
//   only                 nearest non-clipped layer below, and it holds
//                        pixels -- the kinds that do not are already
//                        refused). The sub-document reproduces exactly that:
//                        base at 0, member at 1, and core/Composite's own
//                        clip-group bracket runs. `M` is unclipped, and its
//                        alpha is the base's alpha, which is what a clipped
//                        group's alpha is by construction
//                        (core/Composite.hpp §13).
//   both clipped         both are members of one run sharing a base further
//                        down. Inside the sub-document neither is clipped --
//                        they are folded as a plain pair -- and `M` carries
//                        `clipped`, so the run is one member shorter and
//                        otherwise identical. Exact by `over` associativity
//                        again: folding A then B into a group equals folding
//                        (B over A).
//   lower clipped only   the lower layer is inside a clip run and the upper
//                        is not. `M` must be one or the other, and either
//                        choice changes a picture: clipped, and the upper's
//                        pixels get cut away by a base they never touched;
//                        unclipped, and the lower's escape it. **Refused**,
//                        naming both layers.
//
// ==========================================================================
// §7  Merge visible, stamp visible, and why only one of them is
//     appearance-preserving
// ==========================================================================
//
// Both compute the same buffer -- the document's own composite, `C`. They
// differ in what they do with it, and the difference decides which invariant
// each one can claim.
//
//   **Merge visible** replaces every visible layer with one layer holding `C`,
//   placed at the index of the bottom-most visible layer, and leaves hidden
//   layers where they are. Hidden layers contribute nothing to a composite, so
//   the document's composite afterwards is `over(transparent, C) == C`.
//   **Appearance-preserving, exactly** (to §8's tolerance) -- and note that it
//   is preserving even for layers whose blend is `multiply` or whose `mix`
//   never paired, because the backdrop those blends consumed is inside `C`.
//   That is why merge down's blend refusal points here.
//
//   **Stamp visible** inserts a *new* top layer holding `C` and changes
//   nothing else. The document then composites to `over(C, C)`, which equals
//   `C` only where `C` is opaque. **Stamp visible is not appearance-preserving
//   and cannot be**; that is what "merge visible to a *new* layer" means, and
//   it is what every editor's stamp does. So the invariant it claims is the
//   one that is actually true and is worth as much: **the stamped layer, alone,
//   composites to what the whole document composited to.** Where `C` has any
//   texel with alpha strictly between 0 and 1, the merge warns and says how
//   many.
//
//   Locks follow from that difference and are not a separate policy: merge
//   visible destroys layers, so a locked visible layer refuses it; stamp
//   visible destroys nothing, so a locked layer does not obstruct it at all.
//
// ==========================================================================
// §8  The tolerance, derived
// ==========================================================================
//
// A merge cannot be bit-exact, and the reason is storage rather than
// arithmetic: the composite is computed in `float` and a `Layer` stores
// `half`. One f16 round trip is the whole of the error for merge visible,
// flatten and stamp, whose merged layer is composited over nothing.
//
// IEEE binary16 has a 10-bit stored significand, so a normal value's
// round-to-nearest relative error is at most 2^-11 = 4.883e-4, and the
// absolute error near the subnormal boundary is at most 2^-25 = 2.98e-8. So
// the bound this file's tests assert is
//
//     |after - before|  <=  2^-11 * |before|  +  2^-25
//
// with the second term covering values below the normal range. `--selftest`
// asserts it and prints the *measured* maximum, which for the fixtures there
// comes out an order of magnitude under the bound, because most of the
// fixture's texels are exactly representable.
//
// Merge **down** passes that error through one further `over` against the
// backdrop below. `over` is `s + d*(1-sa)` -- coefficients in [0,1] and no
// cancellation, since every term is non-negative on premultiplied values --
// so it cannot amplify the bound, and `--selftest` measures that it does not.
//
// The one place a merge *is* bit-exact is a document whose composite happens
// to be exactly representable in f16, which is what a fixture built from
// halves gives: `--selftest` builds one of those too and asserts 0.0.
//
// ==========================================================================
// §9  Flatten, and the background this build does not have
// ==========================================================================
//
// Photoshop's Flatten Image fills transparency with white, because its
// document model has a privileged opaque Background layer. **PRD C16 (P0)
// deletes that concept from this build on purpose**: "a new document's base
// layer is an ordinary layer with alpha -- there is no special locked
// Background", and the PRD calls it "a deliberate divergence".
//
// `core::Document` accordingly has no background colour to composite in, and
// inventing one here -- white? black? the paper the solver draws? -- would put
// back exactly the Background C16 removed, irreversibly, in the one operation
// named "flatten". So:
//
//   **flattenDocument() preserves alpha.** A fully transparent corner of the
//   canvas stays fully transparent.
//
// What distinguishes it from merge visible is then not the alpha but the
// **hidden layers**: flatten collapses the document to exactly one layer and
// discards them, merge visible leaves them alone. That is also Photoshop's
// real difference between the two commands, minus the background fill. The
// discard is reported by count.
//
// The place an opaque result is genuinely required is export to a format with
// no alpha channel, and io/Export already owns that decision at the encoder
// boundary (where it is undoable by re-exporting) rather than as a destructive
// edit to the document.
//
// ==========================================================================
// §10  Rasterise a parametric layer (PRD C11)
// ==========================================================================
//
// PRD C11 names four parametric kinds -- Text, Adjustment, Strokes, Flats.
// **Exactly one of them exists in this build**, and the other three are not
// "not implemented yet" in a way this function could paper over: core/Layer.hpp
// says they "hold no pixels of their own and structurally never will -- they'll
// eventually gain their own parameter-only members (a string+font, a Dab list,
// ...)", and none of those members exists. A Text layer in this build has no
// text. There is nothing to rasterise, and `rasteriseLayer()` says so by name
// rather than producing an empty layer.
//
// An **Adjustment** layer does have its content: `Layer::ops`. It holds no
// pixels because it *is* a transformation of what is beneath it (PRD C5), so
// "rasterise it into pixels" can only mean: evaluate it against the composite
// below and become that. Which is exactly `compositeDocumentPremultiplied()`
// over layers `[0 .. index]` -- the adjustment included -- so the mask, the
// clipping, the coverage and the `adjustedPremultiplied()` arithmetic are the
// compositor's, not a copy of them.
//
// **The layers below stay.** They are not consumed: rasterising is about one
// layer, and swallowing the stack would be flatten wearing a different name.
// The consequence is the same one stamp visible has (§7): the rasterised layer
// sits over the layers it was computed from, so the picture is preserved
// exactly where the composite below is opaque and warned about where it is
// not. An adjustment layer never changes alpha (`adjustedPremultiplied()`:
// "out.a = below.a, never touched"), so the alpha of the rasterised layer *is*
// the alpha of the composite below, and one buffer answers both questions.
//
// Refused: a non-Adjustment kind (named, with C11's list and what each of the
// other three still lacks), an Adjustment layer with an empty op stack (an
// exact no-op -- rasterising it produces a copy of the composite below, which
// is Stamp Visible under a misleading name), and an Adjustment layer at index
// 0 (nothing beneath it to evaluate against).
//
// ==========================================================================
// §11  Undo
// ==========================================================================
//
// Nothing here records anything -- `core/` does not depend on `app/`, exactly
// as core/LayerOps.hpp argues. Each function returns a `LayerOpResult` whose
// `editLabel` names what happened in the noun form `app::recordLayerEdit()`
// wants, and `app::applyLayerCommand()` is what turns it into an
// `EditKind::Structural` edit with a `core::History` entry. A merge that could
// not be undone would be a data-loss bug, so `--selftest` asserts the pre-merge
// stack comes back -- layer count, kinds, names and composited pixels.
namespace np {

// --- The cost of the reuse, so a caller can print it ----------------------

// Bytes of the intermediate premultiplied composite buffer a merge through
// core/Composite allocates for `doc`: `width * height * 4 * sizeof(float)`,
// and 0 for a non-positive canvas. Independent of how many tiles the layers
// occupy -- that is the point of quoting it. See §2.
size_t mergeCompositeBufferBytes(const Document& doc) noexcept;

// Occupied tiles of `layer` that lie entirely outside `doc`'s canvas and would
// therefore be discarded by a merge that goes through the compositor (§3).
// Counts across all of a layer's stores, exactly as `core::layerTileCount()`
// does.
size_t offCanvasTileCount(const Document& doc, const Layer& layer) noexcept;

// --- The shared constructor of a merged layer -----------------------------

// A new RGB layer holding `premultiplied` -- a canvas-sized premultiplied
// linear RGBA buffer as `compositeDocumentPremultiplied()` returns -- with
// this build's defaults: visible, opacity 1, blend `normal`, no mask, no ops,
// unclipped, unlocked.
//
// **Allocates a tile only where the buffer is not all zero**, so a merge of a
// sparsely painted document does not hand back a dense grid across the canvas.
// That is PRD C2 ("memory tracks content, not canvas dimensions") applied to
// the one operation most likely to violate it, and it is why this is a
// function rather than four copies of a fill loop.
//
// Returns a layer with an empty store for an empty or wrongly-sized buffer,
// which is what a non-positive canvas produces.
Layer layerFromPremultiplied(const Document& doc, const std::vector<float>& premultiplied,
                             std::string name);

// --- The five operations (PRD C10, C11) -----------------------------------
//
// Each returns core/LayerOps' `LayerOpResult`: `ok`, a refusal sentence naming
// the numbers, the `editLabel` a caller should record, and the index the
// result was left at. `warningsOut`, when non-null, is **appended** to and
// never cleared -- core/Composite's own convention, and for its reason (a
// caller collecting across stages passes one list through all of them).

// Merges layer `index` into layer `index - 1`, replacing both with one layer
// at `index - 1`. §§4-6 are the whole contract; the short version is that the
// pair must both be visible, unlocked, in the same group, blended `normal`
// (or be a `Mix`-paired Pigment pair, which takes the latent path), and not
// arranged so that some third layer's clip depends on the boundary being
// merged away.
//
// The merged layer keeps the **lower** layer's name, which is what every
// editor does and what makes a repeated merge-down read as one layer growing
// rather than a new one appearing each time.
LayerOpResult mergeLayerDown(Document& doc, size_t index,
                             std::vector<std::string>* warningsOut = nullptr);

// Replaces every visible layer with one layer holding the document's
// composite, at the index of the bottom-most visible layer. Hidden layers are
// untouched. Appearance-preserving to §8's tolerance; see §7.
//
// Refuses fewer than two visible layers (one visible layer has nothing to
// merge with, and collapsing it alone is a bake), a locked visible layer, and
// a visible set spanning more than one group.
LayerOpResult mergeVisibleLayers(Document& doc, std::vector<std::string>* warningsOut = nullptr);

// Inserts a **new** top layer holding the document's composite and changes
// nothing else -- PRD C10's "stamp visible (merge visible to a new layer)".
// Not appearance-preserving, deliberately; §7 says what it preserves instead.
//
// Refuses only an empty document and one with nothing visible. A lock does not
// obstruct it: it destroys nothing.
LayerOpResult stampVisibleLayers(Document& doc, std::vector<std::string>* warningsOut = nullptr);

// Collapses the whole document to exactly one layer holding its composite,
// discarding hidden layers. **Alpha is preserved**; §9 says why this build has
// no background to composite in and what that costs.
//
// Refuses an empty document and any locked layer -- flatten destroys every
// layer there is, so one locked layer is enough to refuse the whole operation.
LayerOpResult flattenDocument(Document& doc, std::vector<std::string>* warningsOut = nullptr);

// PRD C11: turns the parametric layer at `index` into pixels. Today that means
// an Adjustment layer and nothing else, and §10 says why that is a property of
// the other three kinds rather than of this function.
//
// The rasterised layer replaces the adjustment layer at `index`, keeps its
// name, and holds the composite of layers `[0 .. index]`. The layers below are
// untouched.
LayerOpResult rasteriseLayer(Document& doc, size_t index,
                             std::vector<std::string>* warningsOut = nullptr);

}  // namespace np
