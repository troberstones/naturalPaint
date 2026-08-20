#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/Pigment.hpp"
#include "ops/PointOps.hpp"

// core/Composite (PLAN.md "Phase 5 -- Stack it", step 1: "Multiple layers in
// `Document`, with reorder, visibility, lock, opacity"; step 2 moved the blend
// arithmetic out to core/Blend and left the document walk here; step 3 gave
// the walk Pigment layers, the latent -> RGB projection, the per-layer op
// stack and `Mix`; step 4 gave every kind a per-layer mask).
//
// ==========================================================================
// Phase 5 step 3 -- Pigment layers, in the order the walk does them
// ==========================================================================
//
// --- 1. The projection, and where the op stack sits relative to it ---------
//
// A Pigment texel is a `Latent` plus a `mass` (core/Pigment.hpp). It becomes a
// premultiplied RGBA texel by
//
//     rgb   = latentToRgb(latent)          // straight, [0,1], no LUT needed
//     texel = (rgb * mass, mass)
//
// i.e. **mass is the Pigment layer's alpha**. That is not a convenience: PRD
// F10 says an eraser "reduces ... Mass on Pigment layers leaving the Latent
// untouched", so mass is already the coverage-like quantity, and giving it any
// other role would leave the layer with two.
//
// **The op stack runs after that projection, and that is the load-bearing
// sentence of PLAN.md's step 3.** `Layer::ops` is applied to the *projected*
// premultiplied RGBA, never to `c0..c2` or to the residual, so no grade ever
// reaches stored latents -- `--selftest` asserts the tile's raw half words are
// bit-identical across a grade rather than arguing it. The reason is
// DESIGN-imaging.md §3's own document invariant: "any op that is a linear
// combination of pixels stays valid in latent space, and any op that is not,
// is not", and its table puts levels, curves and every LUT in the second
// column. A curve applied to a pigment weight is not a graded colour, it is a
// different pigment -- and it would be irreversible, because the layer would
// no longer hold what the brush deposited.
//
// The stack applies to RGB layers at the same point (there the "projection" is
// just the tile read), because DESIGN-imaging.md §3's Layer diagram gives
// *every* layer an `ops` member. An **empty** stack is skipped outright rather
// than run as an identity: `applyPointOpsPremultiplied()` un-premultiplies and
// re-premultiplies, which is a divide and a multiply and therefore not
// bit-exact, and step 1's byte-identity regression boundary is asserted at
// zero tolerance. Every layer this build creates has an empty stack, and so
// does every layer in every `.npaint` written to date.
//
// Only `OpClass::PointA` entries are applied, because they are the only class
// with an implementation anywhere in this codebase (core/OpStack.hpp says so);
// a SpatialB/StrokeC/BakedD entry occupies its slot and contributes nothing.
//
// --- 2. `Mix`, and why it is a *pair* rather than an accumulator ----------
//
// PRD C3 (P0): "KM mixing between layers is the opt-in `Mix` blend mode". PRD
// L5: "`Mix` appears in the blend dropdown only between two Pigment layers",
// which docs/ui.md §3.4 states as "both the layer and the one beneath it".
//
// So `Mix` is defined on a **pair**: the layer, and the Pigment layer directly
// beneath it. `core::mixPairing()` (core/Blend) computes which layers pair
// with which, greedily from the bottom, and this walk composites a mixed pair
// as one unit -- the lower layer is *not* composited separately, because the
// mix replaces both of them rather than sitting over one.
//
// Concretely, per texel, with `low`/`up` the two Pigment texels:
//
//     t     = up.mass                        // the mixing weight is MASS
//     Lmix  = mixLatents(low.latent, up.latent, t)
//     mmix  = up.mass + low.mass*(1 - up.mass)   // coverage, union'd as `over`
//
// and the three projections that the coverages then combine (§3 below).
//
// **The weight is mass, not opacity**, and that is the whole of what makes
// PLAN.md's verify sentence work: an opaque blue over yellow is blue, because
// opaque paint covers; blue at mass 0.5 over yellow is green, because half a
// mass of blue pigment mixed with a mass of yellow pigment *is* green. That is
// DESIGN-imaging.md §3's own `latent_over·α + latent_under·(1−α)` with α the
// upper layer's coverage, and its own worked example ("gives green for
// blue-at-50%-over-yellow").
//
// **What is deliberately not built: chains.** Three Pigment layers, the top
// two both `mix`, pairs (0,1) and leaves the top one unpaired -- it is warned
// about by name and composited as `over`, the same contract this file has
// applied to an unimplementable blend since step 1. Making chains associative
// means carrying a *canvas-sized latent accumulator* through the walk (7
// floats per pixel, 117 MiB for a 2048x2048 document) so that a mixed result
// can itself be mixed into; and it means answering what a mid-chain layer's
// opacity fade -- which happens in projected RGB, because opacity is
// transparency -- does to a state that has to stay latent to be mixed again.
// Neither is a small question, neither is asked by PRD C3 or L5, and the pair
// costs **no extra memory at all**: both layers' tiles are read directly. A
// stated limit that warns beats an approximation that does not.
//
// --- 3. Opacity is transparency, on a mixed pair too ----------------------
//
// PRD C3 again: "Layer opacity means transparency on **every** kind". The trap
// this step had to avoid is that the obvious way to fade a `Mix` layer -- scale
// the mixing weight `t` by the layer's opacity -- turns opacity into *mass*:
// it would change the pigment mixture, so dragging opacity down would change
// the mixed colour's hue rather than let the backdrop show through. `t` is
// therefore untouched by opacity, and the fade happens on the projected RGBA.
//
// Writing `covLow`/`covUp` for the two layers' `layerCoverage()`, and
// `Plow`/`Pup`/`Pmix` for the three graded projections (lower alone, upper
// alone, and the mixed pair), the pair contributes
//
//   P =        covLow *      covUp  * Pmix
//     +        covLow * (1 - covUp) * Plow
//     + (1 -   covLow)*      covUp  * Pup
//
// -- each layer independently either participates or does not, weighted by its
// own coverage, and the fourth combination (neither) contributes nothing. The
// three corners are the three things PRD C3 requires and `--selftest` asserts
// each of them:
//
//   covUp  = 0  ->  covLow*Plow   : the mixing layer is *absent*, bit-for-bit
//                                   what the document composites to with that
//                                   layer deleted.
//   covLow = 0  ->  covUp*Pup     : hiding the lower layer leaves the upper
//                                   one visible and unmixed, rather than
//                                   blanking the pair.
//   both   = 1  ->  Pmix          : the full Kubelka-Munk mix.
//
// That form is not invented for `Mix`; it is what opacity already does. For
// `over`, `lerp(dst, over(src, dst), o)` is **algebraically identical** to
// `over(o*src, dst)` -- both come to `o*src + dst*(1 - o*src.a)` -- so "fade
// the layer's whole effect back toward the backdrop" and "scale the source's
// coverage" are the same operation wherever both are defined. `--selftest`
// asserts that identity numerically, because it is the argument for using the
// fade where only the fade is available.
//
// Two consequences worth stating: the stored `pig.m` is **never** written by
// opacity (asserted by `memcmp` of the tile's raw half words), and a Pigment
// layer's opacity behaves exactly like an RGB layer's, because for a non-mixed
// Pigment layer the walk multiplies the projected premultiplied texel by
// `layerCoverage()` and nothing else -- the identical line RGB layers take.
//
// --- 4. Every other combination, stated rather than left to be found ------
//
//   Pigment under `over`/plus/multiply/screen/min/max: projected first, then
//     handed to `blendPixel()` like any other premultiplied texel. A blend
//     mode is defined on colour, and after the projection a Pigment layer *is*
//     colour.
//   Pigment against an RGB layer: the same. No latent ever crosses an RGB
//     layer, which is DESIGN-imaging.md §3's "the stack is one-way" holding
//     automatically rather than by a rule -- this walk carries no latent in
//     its accumulator at all, only the pairwise read.
//   `Mix` where L5 does not hold (on an RGB layer, on the bottom layer, over a
//     non-Pigment layer, or as the upper half of a chain): composited as
//     `over`, warned by name, never silently, never refused. It cannot be set
//     through `core::setLayerBlend()` -- that refuses through the same
//     predicate -- so it can only arrive from a file, where PRD I10 requires
//     it be carried verbatim rather than coerced.
//   A Pigment layer with `pigmentTiles == nullopt`, or an RGB layer with
//     `rgbTiles == nullopt`: contributes nothing, exactly as an empty store
//     would.
//   Adjustment: **no longer skipped** -- see §8 below (PLAN.md Phase 5 step
//     5). It is the one kind that transforms the accumulator instead of
//     contributing to it.
//   Media/Strokes/Text/Flats: still skipped. None owns storage.
//
// ==========================================================================
// Phase 5 step 4 -- Layer masks
// ==========================================================================
//
// --- 5. A mask is per-texel opacity, and composes as a plain product ------
//
// PRD C4 (P0) lists "per-layer mask" among the things layers do; PRD C3 (P0)
// says what one *means*: "Layer opacity means transparency on **every** kind".
// A mask is that sentence made per texel, so the walk's rule is one line:
//
//     effective coverage at (x,y) = layerCoverage(layer) * mask(x,y)
//
// with `mask(x,y) == 1.0` wherever the layer has no mask, no mask tile, or a
// tile whose sample says 1.0 -- three different things in storage
// (core/Mask.hpp separates them) and the same thing here.
//
// **They compose as a plain product, and that is a claim rather than an
// assumption.** Opacity multiplies a premultiplied texel; so does a mask; and
// float multiplication is associative enough for the only property that
// matters -- `--selftest` asserts that a layer at opacity 0.5 under a 0.5 mask
// composites **byte-identically** to the same layer at opacity 0.25 with no
// mask, because both reach `contribute()` with the single scalar 0.25. The
// alternative form, "fade the layer's whole effect toward the backdrop by the
// mask", is the same value for the same reason §3 already gives for opacity:
// `lerp(dst, blend(src,dst), o)` and `blend(o*src, dst)` are algebraically
// identical, and `--selftest` measures the residual at exactly 0 across a
// range of mask values as well as opacities.
//
// A texel whose effective coverage is <= 0 is **skipped**, not multiplied by
// zero -- the same distinction the hidden-layer case makes above, and for the
// same reason (a stored NaN or signed zero would still perturb the
// accumulator). It is what makes an all-0.0 mask bit-for-bit identical to the
// layer being deleted.
//
// --- 6. The trap: a mask on a Pigment layer is not pigment mass -----------
//
// This is the same trap PRD C3 set for opacity in step 3, moved from
// per-layer to per-texel, and it has to be answered again because the obvious
// implementation is different here. A Pigment texel is `(latent, mass)` and
// `mass` is the coverage-like quantity, so "make this texel less visible"
// looks like "scale its mass". It is not:
//
//   * **On a lone Pigment layer the two are numerically indistinguishable**,
//     and that is why the distinction has to be argued rather than measured
//     here: `projectPigmentTexel()` returns `(latentToRgb(latent)*m, m)` and
//     `latentToRgb()` does not depend on `m` at all, so scaling `m` scales the
//     whole premultiplied vector exactly as scaling coverage does.
//   * **On a mixed pair they are wildly different**, because `Mix`'s weight
//     `t` *is* the upper layer's mass (§2). Halving the mass changes what
//     pigment mixture is being computed -- half a mass of blue into a mass of
//     yellow is **green** -- while halving the mask leaves the mixture alone
//     and fades the pair's whole contribution back toward the layer beneath,
//     which is a 50/50 *colour* blend of the mix and the backdrop, i.e. muddy.
//     `--selftest` prints both triples side by side.
//   * **PRD F10 already owns mass.** "Erase reduces alpha on RGB layers,
//     **Mass** on Pigment layers leaving the Latent untouched". A mask that
//     scaled mass would be a non-destructive eraser wearing a mask's name, and
//     the two are different features with different UI and different undo.
//
// So the walk hands `projectPigmentTexel()` the **stored** texel and applies
// the mask to what comes out, and on a mixed pair the mask modulates `covUp`
// and `covLow` while `t` is untouched. `--selftest` asserts the stored `pig.m`
// half words are bit-identical across a composite at any mask value, by
// memcmp, rather than arguing it.
//
// --- 7. Where the mask sits relative to everything else -------------------
//
//   the op stack   The mask applies **after** it, with opacity, because it is
//                  coverage and the stack grades colour. An empty stack is
//                  still skipped outright, so a masked layer with no ops takes
//                  the same bit-exact path an unmasked one does.
//   `visible`      Unchanged and independent: a hidden layer is skipped before
//                  any mask is read. A mask is not a way to hide a layer and
//                  the eye icon is not a mask.
//   blend mode     Unchanged. The mask scales the source texel; the mode then
//                  meets the backdrop with it. Alpha is still `over`'s under
//                  every mode, because scaling a premultiplied source scales
//                  its alpha with it.
//   `Mix`          §2's pairing is unaffected -- masks do not decide who pairs
//                  with whom, only how much each half covers.
//   clipping masks **A different feature** (PRD C9, PLAN.md Phase 5 step 9): a
//                  layer clipped by the *alpha of the layer below*, storing
//                  nothing of its own. Not built here, and not conflated.
//
// ==========================================================================
// Phase 5 step 5 -- Adjustment layers
// ==========================================================================
//
// PRD C5: "An Adjustment layer applies its op stack to the composite below
// it." PRD C1 lists Adjustment among the layer kinds (P0); PRD D18 makes every
// §D op re-editable in place, which is what a *layer* holding a stack is for.
//
// --- 8. It inverts the walk, and that is the whole of the feature ---------
//
// Every other layer kind is a *source*: it is projected, graded, scaled by its
// coverage and blended **onto** the accumulator. An Adjustment layer has no
// source. It holds no tiles of any kind -- no `rgbTiles`, no `pigmentTiles`,
// nothing to project -- and its entire content is `Layer::ops`. So it does not
// contribute; it **transforms what is already there**.
//
// **"The composite below" is the accumulator exactly as the walk finds it**
// when it reaches this layer's index: every layer at a lower index, already
// blended, in premultiplied linear light. That is the only reading the
// bottom-to-top walk makes available and it is also the right one -- PRD C5
// says "the composite below it", not "the layer below it", and an adjustment
// layer that saw only its immediate neighbour would be a *clipped* adjustment,
// which is PRD C9 and PLAN.md step 9 (see §11).
//
// --- 9. Straight vs premultiplied, and the guard that was not copied ------
//
// The op stack grades *colour*, and every op in ops/PointOps is contracted to
// operate on straight (non-premultiplied) scene-linear RGB. The accumulator is
// premultiplied. So the bracket is the familiar un-premultiply, grade,
// re-premultiply -- and it is **not written again here**:
// `applyPointOpsPremultiplied()` (PRD B4, ops/PointOps.hpp) is exactly that
// bracket and already owns it, so this walk hands it the accumulator's texel
// through `gradedPremultiplied()`, the same function every other layer kind's
// grade goes through. There is one un-premultiply-for-grading in this binary,
// not two.
//
// Its `a <= 0 -> {0,0,0,0}` guard is therefore inherited rather than
// duplicated, and this walk **never reaches it**: a texel whose accumulated
// alpha is <= 0 is skipped outright before the call. That is not an
// optimisation, it is the meaning of the operation. Where the composite below
// is empty there is no colour to grade -- an exposure of +2 stops on nothing
// is nothing -- and skipping says so exactly, where multiplying would depend
// on what a premultiplied texel with zero alpha and non-zero colour (which
// `plus` can produce) is taken to mean. **An adjustment layer over nothing is
// a bit-exact no-op**, and `--selftest` asserts it as a byte-identity claim.
//
// --- 10. Opacity and a mask mean "how much of the adjustment applies" ------
//
// A source layer's coverage says how much of *it* covers the backdrop. An
// adjustment layer has nothing to cover with, so the same scalar means how far
// the graded result is taken toward from the ungraded one:
//
//     effective = layerCoverage(layer) * mask(x,y)          // §5's product
//     out       = below + effective * (graded(below) - below)
//
// i.e. a lerp, per texel, in premultiplied space. That is the *same* identity
// §3 and §5 already rest on -- `lerp(dst, f(dst), o)` is how opacity is
// defined wherever both forms exist -- applied to a transform rather than to a
// source. The two ends are exact rather than nearly exact, and both matter:
//
//   effective == 0   the layer is **skipped**, so the accumulator is not
//                    written at all. Opacity 0, a hidden layer and an
//                    all-0.0 mask are each bit-for-bit the layer being
//                    deleted -- not "a lerp by zero", which is a multiply and
//                    an add and therefore not the identity on every float.
//   effective == 1   the graded value is **assigned**, not lerped to. `below +
//                    1.0f*(g - below)` is two correctly-rounded operations and
//                    is not `g`; a full-strength adjustment must be exactly
//                    what the op stack computed.
//
// An **empty** op stack is skipped outright as well, for the reason §1 already
// gives for every other kind: `gradedPremultiplied()` is the identity on an
// empty stack, but an adjustment layer with no ops must cost exactly nothing,
// and a `.npaint` written before this step carries no stack at all.
//
// --- 11. What it does to alpha, its blend, and its scope ------------------
//
//   alpha        **Unchanged, by construction.** This walk writes R, G and B
//                and never touches the accumulator's alpha at all. A grade is
//                a colour operation -- ops/PointOps' whole contract is that no
//                op in the committed P0 set sees alpha, let alone modifies it
//                -- and coverage is not colour. `--selftest` asserts the
//                accumulated alpha is bit-identical across an adjustment
//                layer at every opacity and under a mask.
//   blend mode   **Not honoured, and warned about by name.** A blend mode
//                combines a *source* with a backdrop, and an adjustment layer
//                has no source: the operand that would play `src` is the
//                backdrop itself. Photoshop defines the combination (a Curves
//                layer set to Multiply multiplies its own result against the
//                unadjusted composite); this build does not, because that is a
//                second semantic for `np:blend` and PLAN.md step 5 does not
//                ask for one. So the result is lerped in, `over`-style, and a
//                layer carrying anything but `normal` gets
//                `adjustmentLayerBlendWarning()` -- the same "approximate,
//                never silent" contract §7's unimplemented blend already has.
//   scope        **Everything below it in the stack**, and therefore the whole
//                canvas: an adjustment layer has no tiles, so there is no
//                sparse set to walk and no data window to clip to. That is
//                what PRD C5 asks for. Restricting it to just the layer
//                beneath is a **clipping mask** (PRD C9, PLAN.md step 9), a
//                different feature with a different UI and its own row marker
//                (`ADJUSTMENT · CLIPPED` in docs/ui.md §3.2) -- not built
//                here and not conflated, exactly as step 4 did not conflate a
//                layer mask with one.
//   groups       Unchanged: `Layer::parent` is still carried and never acted
//                on, so an adjustment layer inside a group would still affect
//                everything below it rather than the group. This build creates
//                no groups.
//   cost         O(canvas) per adjustment layer, unconditionally. A document
//                with no adjustment layer pays nothing -- the branch is not
//                taken -- but there is no ROI narrowing here; that is PLAN.md
//                phase 6's "`roi(rect) -> rect` per op, walked backwards" and
//                the hash-keyed tile cache that goes with it.
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
//   - The blend arithmetic (core/Blend), including `mixLatents()` itself --
//     this file owns which two texels get mixed and what happens to the
//     result, not the lerp.
//   - The latent -> RGB projection (core/Pigment).
//   - The mask *storage* and its value rules (core/Mask), which this file
//     reads and does not own.
//   - Clipping masks (step 9), adjustment layers (step 5) and groups.
//     `Layer::parent` is still carried and never acted on: this build creates
//     no groups, and honouring a parent link means compositing a group's
//     members into an offscreen buffer first, which is step 9's machinery,
//     not this step's. A **clipping** mask -- a layer clipped by the alpha of
//     the layer below (PRD C9) -- is a different feature from the per-layer
//     mask §5 describes, shares no code with it, and is not conflated here.
//   - Media layers. They will reuse the Pigment tile plus per-medium state
//     that has no home on `Layer` yet, so they are still skipped.
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

// The layer's **mask** sample at one document pixel, or exactly 1.0f when the
// layer has no mask, no mask tile there, or a sample that says 1.0 -- three
// different things in storage (core/Mask.hpp) and one thing here.
//
// Always in [0,1]: `MaskTile::readCoverage()` clamps, so a file's NaN or 1.5
// cannot reach the composite. §5 above is the whole of what this multiplies.
//
// Multiply it by `layerCoverage()` to get the layer's effective coverage at
// that pixel. That is exactly what core/Composite's walk and core/Probe both
// do; the walk hoists the tile lookup out of its texel loop and calls
// `core::maskCoverage()` -- this function's own leaf -- so the two cannot
// produce different answers.
float layerMaskCoverageAt(const Layer& layer, PixelCoord at) noexcept;

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
//
// `mixReason`, when non-empty, is the specific reason a `mix` layer could not
// be paired (it is not a Pigment layer, there is nothing beneath it, the layer
// beneath is not Pigment, or the layer beneath is already the lower half of
// another pair). It is computed by the walk, which knows the document, rather
// than guessed at here from a Layer alone.
std::string unimplementedBlendWarning(size_t layerIndex, const Layer& layer,
                                      const std::string& mixReason = {});

// The enabled `OpClass::PointA` entries of `ops`, in stack order, each already
// bound to its own params -- `core::OpStack::detectRuns()`'s output flattened,
// which is exactly what a per-texel grade needs and is resolved **once per
// layer** rather than per texel (building a `PointOp` allocates a closure).
//
// Returns an empty vector for an empty stack, and callers must treat empty as
// "do nothing at all" rather than "apply nothing": see this header's §1 on why
// running `applyPointOpsPremultiplied()` with no ops is not the identity.
std::vector<PointOp> layerPointOps(const OpStack& ops);

// `premultiplied` graded by `ops`, or **bit-identically** `premultiplied` when
// `ops` is empty. The one place this codebase decides that an empty op stack
// costs nothing and changes nothing.
std::array<float, 4> gradedPremultiplied(const std::array<float, 4>& premultiplied,
                                         const std::vector<PointOp>& ops);

// The latent -> RGB projection of one Pigment texel, as a **premultiplied**
// linear-light RGBA texel: `(latentToRgb(latent) * mass, mass)`. See this
// header's §1 on why mass is the alpha.
std::array<float, 4> projectPigmentTexel(const PigmentTexel& texel) noexcept;

// One texel of a mixed Pigment pair, premultiplied, with both layers' op
// stacks and both layers' coverages already applied -- the `P` of this
// header's §3, in one function so that the flattener and the eyedropper cannot
// disagree about what a `Mix` layer looks like.
std::array<float, 4> mixedPairTexel(const PigmentTexel& lower,
                                    const std::vector<PointOp>& lowerOps, float lowerCoverage,
                                    const PigmentTexel& upper,
                                    const std::vector<PointOp>& upperOps, float upperCoverage);

// One texel of an **Adjustment** layer's effect: `below` -- the composite
// accumulated beneath it, premultiplied -- with `ops` applied and the result
// taken `effectiveCoverage` of the way there. This header's §§8-11 derive it;
// the summary is
//
//     out.rgb = below.rgb + effectiveCoverage * (graded(below).rgb - below.rgb)
//     out.a   = below.a                                        // never touched
//
// In one function so that the flattener and the eyedropper cannot disagree
// about what an adjustment layer looks like, exactly as `mixedPairTexel()` is.
//
// Returns `below` **bit-identically** -- the same four floats, not an
// arithmetically equal four -- in each of the three cases that mean "this
// layer does nothing here": an empty `ops`, an `effectiveCoverage` <= 0, and a
// `below` whose alpha is <= 0 (there is no colour under an adjustment layer to
// grade). At `effectiveCoverage >= 1` the graded value is assigned rather than
// lerped to, so a full-strength adjustment is exactly what the op stack
// computed. See §10 on why both ends have to be exact rather than merely
// close.
std::array<float, 4> adjustedPremultiplied(const std::array<float, 4>& below,
                                           const std::vector<PointOp>& ops,
                                           float effectiveCoverage);

// **An Adjustment layer carrying a blend mode other than `normal` has that
// mode ignored, and is warned about by name -- never silently.** See this
// header's §11: a blend combines a source with a backdrop and an adjustment
// layer has no source, so there is nothing for the mode to act on that would
// not be a second, invented meaning for `np:blend`.
//
// Named by index, by user-facing name when it has one, and by the blend it
// asked for -- the same sentence shape `unimplementedBlendWarning()` uses,
// because it is the same contract: the pixels are an approximation of what the
// document says, and every boundary that makes them durable reports it.
std::string adjustmentLayerBlendWarning(size_t layerIndex, const Layer& layer);

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
