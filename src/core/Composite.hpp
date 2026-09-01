#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/OpStack.hpp"
#include "core/Pigment.hpp"

// core/Composite (PLAN.md "Phase 5 -- Stack it", step 1: "Multiple layers in
// `Document`, with reorder, visibility, lock, opacity"; step 2 moved the blend
// arithmetic out to core/Blend and left the document walk here; step 3 gave
// the walk Pigment layers, the latent -> RGB projection, the per-layer op
// stack and `Mix`; step 4 gave every kind a per-layer mask; step 5 made
// Adjustment layers transform the composite below; step 9 gave the walk
// clipping masks).
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
//                  nothing of its own. Built at step 9 -- see §§12-17 -- and
//                  still not conflated with this one: §16 is the whole of what
//                  a layer's own mask and its clip do to each other, and they
//                  act at different points and on different operands.
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
// `core::applyOpsPremultiplied()` (core/OpStack.hpp) is exactly that bracket
// and already owns it, so this walk hands it the accumulator's texel through
// `gradedPremultiplied()`, the same function every other layer kind's grade
// goes through. There is one un-premultiply-for-grading in *this walk*, not
// two.
//
// **Two implementations of that same bracket exist in the binary, not one.**
// `ops::applyPointOpsPremultiplied()` (PRD B4, ops/PointOps.hpp) was the
// original, evaluating its op list through a stored `std::vector<PointOp>`
// of `std::function` closures -- one indirect call per op per texel. Every
// self-test that exercises the bracket directly (ops/PointOps' own,
// color/LutBake's GPU-vs-CPU reference) still uses it unchanged, and its
// contract is identical: same guard, same un-premultiply/re-premultiply
// arithmetic, same never-touches-alpha rule. Only this walk stopped calling
// it, on docs/architecture-review.md's P0-5 finding: that indirect call is
// exactly wrong for a per-texel hot path evaluating a pipeline that is
// constant for the whole layer. `core::applyOpsPremultiplied()` keeps the
// identical bracket but evaluates each `core::Op` via `applyOpDirect()`'s
// switch instead -- same six formulas, same order, a closed dispatch instead
// of an open one. app/selftest/GradeDispatch.cpp checks the two agree.
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
//                beneath is a **clipping mask** (PRD C9), a different feature
//                with its own row marker (`ADJUSTMENT · CLIPPED` in
//                docs/ui.md §3.2) -- built at PLAN.md step 9 and still not
//                conflated with this: an adjustment layer with
//                `clipped == false` behaves exactly as this section
//                describes, and one with `clipped == true` is composited by
//                §14's group instead of by this branch at all.
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
// ==========================================================================
// Phase 5 step 9 -- Clipping masks
// ==========================================================================
//
// PRD C9 (**P0**): "a layer or group is clipped by the alpha of the layer
// below it". PRD C4's "layers behave as in Photoshop". docs/ui.md §3.2 already
// assumed it -- `ADJUSTMENT · CLIPPED` is one of its own example rows.
//
// One bool on the layer (`Layer::clipped`); everything else about a clip is a
// property of where the layer sits, derived here by `clipRuns()`.
//
// --- 12. A RUN clips to ONE base, and never to itself ---------------------
//
// **The base is the nearest layer below that is not itself clipped.** Three
// clipped layers over one base all clip to that base's alpha; they do not
// progressively erode one another, and the second clipped layer is *not* the
// third one's base. In `clipRuns()` that is three lines -- the running `base`
// index is updated only when a layer is **not** clipped -- and it is called
// out here because the cumulative reading is the single most common
// clipping-mask bug and both readings look right in a one-clipped-layer
// screenshot. `--selftest` separates them numerically: under the cumulative
// reading a stack of three clipped layers over a half-alpha base would land at
// 0.5^3 = 0.125 coverage; under this one it lands at 0.5, and the two numbers
// are printed side by side.
//
// A clipped layer whose nearest non-clipped layer below **holds no pixels** --
// an Adjustment layer, a Media/Strokes/Text/Flats placeholder, or an RGB layer
// whose tile store is absent -- has no base either. "The alpha of the layer
// below" is not a quantity such a layer has. It is *not* resolved by searching
// further down, because that would clip to something that is not the layer
// below, which is not what PRD C9 says.
//
// --- 13. Which alpha, and where the group lands: the two-part answer -------
//
// **(i) The alpha that clips is the base's EFFECTIVE alpha** -- its stored
// alpha, after its own op stack, times its own opacity and its own mask
// sample. Concretely it is the alpha of the exact premultiplied texel the walk
// would have blended into the accumulator had nothing been clipped to it,
// which is why the code needs no separate notion of "the clip alpha" at all.
//
// Two consequences worth stating because they are the reason for choosing the
// effective alpha over the stored one: **hiding the base hides the whole
// clipping group** (its coverage is 0, so the group's is), and **a mask on the
// base masks the group**, which is what a user who has just masked a base
// expects and is Photoshop's behaviour. The op stack is in that list too but
// changes nothing: ops/PointOps' committed set never touches alpha, so a grade
// on the base cannot move the clip boundary -- asserted rather than trusted.
//
// **(ii) The clipping group composites INTERNALLY first, and then lands on the
// backdrop through the base's blend mode and the base's coverage.** It does
// not composite each clipped layer onto the backdrop independently. The two
// readings are observably different whenever the base is partly transparent or
// its blend is not `over`, and `--selftest` prints both rather than only the
// chosen one. Three reasons for this one:
//
//   * **Only this reading makes "clipped by the alpha of the layer below"
//     literally true.** A clipping group's coverage is *exactly* the base's;
//     clipping can never add coverage to the document. Under the independent
//     reading it can: an opaque clipped layer masked to a half-alpha base and
//     composited `over` gives 0.5 + 0.5*0.5 = 0.75 coverage, i.e. the clipped
//     layer has made the base *more* opaque than it was. That is not a
//     clipping mask.
//   * **A clipped layer must not paint on the backdrop.** Under the
//     independent reading, where the base is semi-transparent the clipped
//     layer lands partly on the base and partly on whatever shows through it,
//     so a clipped highlight bleeds onto the layers below. §16's fixture shows
//     the two answers.
//   * **It gives the base's blend mode and opacity one meaning instead of
//     two.** A `multiply` base with a clipped layer over it multiplies *the
//     result* into the backdrop, which is what "the base is how this group
//     meets the document" means.
//
// The arithmetic, per texel, and it needs **no offscreen buffer** -- which is
// the prediction core/Layer.hpp made and this step falsified. Writing `S` for
// the base's own final premultiplied source texel (graded, coverage applied):
//
//     open   g = (S.rgb / S.a, 1)          // the base's straight colour,
//                                          // treated as an opaque backdrop
//     fold   g = blendPixel(mode_m, src_m, g),  g.a := 1   for each member m,
//                                                          bottom to top
//     close  out = (g.rgb * S.a, S.a)      // the base's alpha, restored
//
// and `out` then meets the accumulator through the **base's** blend mode. The
// open/close bracket is the same shape `applyPointOpsPremultiplied()` already
// uses for grading, for the same reason: a blend mode is defined on straight
// colour against a backdrop, and inside a clipping group the backdrop is the
// base considered opaque. Forcing `g.a` back to 1.0f after every fold makes
// "a clipping group's coverage is the base's" true by construction rather than
// by trusting that `as + 1*(1-as)` rounds to exactly 1.
//
// **The bracket is opened lazily, and that is a correctness property rather
// than an optimisation.** `(S.rgb / S.a) * S.a` is a divide and a multiply and
// is therefore not the identity on every float -- **16.1% of one swept binade
// of premultiplied components come back one ulp out**, measured and printed by
// `--selftest` rather than asserted here. So a group whose members contribute
// nothing at a texel -- an unpainted texel of a clipped layer, a member masked
// to 0, a member at opacity 0 -- must not go through it at all.
// It does not: the group is opened by the first member that actually
// contributes, so such a texel is **bit-identical** to the same document with
// the clip flags cleared. `--selftest` asserts that by `memcmp`.
//
// Where `S.a <= 0` the group is never opened either, and nothing needs to
// guard the division separately: a base with no coverage clips everything away
// to nothing, which is PRD C9 read literally.
//
// --- 14. A clipped Adjustment layer sees its BASE, not the composite -------
//
// This is the most common real use of clipping and it is the one interaction
// most likely to be quietly wrong, because §8 made `adjustedPremultiplied()`
// act on "the composite below" -- the whole accumulator. A clipped adjustment
// layer is a **member**, so the "below" it is handed is the group accumulator
// `g`, not the document accumulator, and its scope is therefore the base and
// only the base. Nothing changed in `adjustedPremultiplied()` itself; what
// changed is which four floats it is handed.
//
// Two details fall out and both are worth stating. `g.a` is exactly 1.0f
// inside a group, so the function's own un-premultiply is a division by one
// (exact) and its `below.a <= 0` early-out is unreachable there -- a clipped
// adjustment layer is the one place in this codebase where that bracket is
// exact for every input. And its own opacity and mask keep meaning what §10
// says they mean: how much of the adjustment applies, not where.
//
// --- 15. `Mix` and a clip are mutually exclusive, decided in one place -----
//
// `Mix` composites two Pigment layers **as one unit** (§2). A clip makes the
// layer below the alpha that decides where the layer above **shows**. Both are
// relationships with the same neighbour, they are not the same relationship,
// and a document can ask for both. The resolution is a single added condition
// in `blendModeAvailableForLayer()` -- PRD L5's existing predicate -- so that
// the dropdown, `core::setLayerBlend()` and `mixPairing()` cannot disagree:
//
//   **no mixed pair forms if either half is `clipped`.**
//
// Each of the three arrangements it rules out is ruled out for its own reason:
//
//   upper half clipped   the layer would be asking the layer beneath it both
//                        to be its mixing partner and to be the alpha it is
//                        masked by. There is no composite that is both.
//   lower half clipped   that layer is a *member* of a clipping run whose base
//                        is further down. Consuming it into a pair with a
//                        layer outside the run would composite it outside its
//                        own group, and the clip would silently stop applying
//                        -- the worst of the three outcomes, because nothing
//                        on screen would say so.
//   a clipped layer
//   between a pair       [P0, C1(clipped), P2(mix)] -- P2's "layer beneath" is
//                        a member of P0's run, which is the previous case.
//
// The layer that loses its pair is composited as `over` and warned about by
// name with the specific reason, which is the contract §7 has applied to a
// `mix` PRD L5 does not permit since step 3 -- not a new one. It is **not** a
// refusal: `np:blend` and `np:clipped` are both carried verbatim from a file
// (PRD I10), so a document can arrive holding the combination and must still
// composite and still save.
//
// A mixed pair is, however, a perfectly good clip **base**: [P0, P1(mix),
// C2(clipped)] clips C2 to the *pair's* alpha, because the pair is one unit
// and one unit is what a base is. That needs no special case at all -- `S`
// above is `mixedPairTexel()`'s output for such a base.
//
// --- 16. A layer's own mask and its clip are different operators -----------
//
// **Both apply, and they are not the same thing applied twice.** The layer's
// own mask (and opacity) scale its source *inside* the group, at the fold; the
// clip scales the group's *result*, at the close. So on colour they multiply
// -- a clipped layer at mask 0.5 over a base of alpha 0.5 lands its colour at
// 0.25 -- but on **coverage they do not**: the group's alpha is 0.5, the
// base's, not 0.25. That is exactly what separates a clip from a mask, and
// `--selftest` prints the four combinations of {mask 1, 0.5} x {base alpha 1,
// 0.5} side by side with the two alphas visibly different.
//
// The base's own mask and opacity are a different case again: they are a
// single scalar on the whole group, so multiplying them into the clip alpha
// and fading the finished group are the same arithmetic, and the suite asserts
// the equality on dyadic fixtures rather than assuming that a float product
// re-associates.
//
// --- 17. The bottom layer, and what a clip costs --------------------------
//
//   the bottom layer  **cannot be clipped** -- there is nothing below it.
//                     `core::setLayerClipped()` refuses index 0 by name and
//                     with the document's layer count, and `core::moveLayer()`
//                     refuses a move that would put a clipped layer there.
//                     But a *file* can carry the flag (PRD I10), so the
//                     composite answers for one too: it is composited
//                     **unclipped**, and warned about by name with its index
//                     -- never silently ignored, and never dropped. Dropping
//                     it would let a one-bit attribute be the thing that makes
//                     a layer's pixels vanish, which is the failure §7 refuses
//                     for an unimplementable blend for exactly the same
//                     reason.
//   groups            PRD C9 says "a layer **or group**". This build has no
//                     `LayerKind::Group` and creates no groups (`Layer::parent`
//                     is still carried and still never acted on), so the
//                     group half of C9 is **not built here** and is not
//                     pretended to be. What this step did establish is that it
//                     will not need the offscreen buffer core/Layer.hpp
//                     predicted: a clipping group is folded per texel inside
//                     the base's own tile walk.
//   cost              A clipping run is walked over **the base's occupied
//                     tiles only**, because outside them the base's alpha is 0
//                     and the group contributes exactly nothing. So a clipped
//                     layer's own tiles are never visited for their own sake,
//                     and clipping is the one feature in this walk that can
//                     only make it cheaper. A document with no clipped layer
//                     pays one `clipRuns()` pass -- three vectors, no tile
//                     access -- and takes byte-for-byte the path it took
//                     before this step.
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
//   - **Groups.** `Layer::parent` is still carried and never acted on: this
//     build creates no groups. This bullet used to add "and honouring a parent
//     link means compositing a group's members into an offscreen buffer first,
//     which is step 9's machinery" -- step 9 has landed and that turned out to
//     be wrong twice over (§17): clipping needed no offscreen buffer, and
//     groups are still unbuilt. Clipping masks themselves *are* here, at
//     §§12-17, and still share no code with the per-layer mask of §5.
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

// **Test-only.** Turns `compositeWalk()`'s opaque-floor early exit (see
// core/Composite.cpp's own section on it) off or back on; on by default.
// The only intended caller is app/selftest/OpaqueFloor.cpp, which needs to
// composite the SAME document both ways to prove the optimization changes
// nothing about the result -- nothing in the running application ever calls
// this. Not declared `noexcept` to match this header's style for a function
// with an observable side effect on process-wide state, and deliberately
// not thread-safe: `--selftest`'s sections run sequentially on one thread.
void setOpaqueFloorEnabledForTesting(bool enabled);

// **Test-only.** Overrides the grain `compositeWalk()`'s tile loops hand to
// `core::parallelFor()`. The only intended caller is
// app/selftest/CompositeParallel.cpp, which needs to force the walk down
// both the serial fallback (a grain larger than any tile count the test
// uses) and the genuinely-parallel path (a grain of 1) against the SAME
// document, to prove the two produce bit-identical results rather than
// merely trusting that they do. Nothing in the running application ever
// calls this; real callers get the measured default.
void setCompositeParallelGrainForTesting(size_t grain);

// --- Groups (PLAN.md Phase 5's C7/C12 follow-on; PRD C7) -------------------
//
// **The one decision this build makes about groups, stated once and made
// structural rather than implied**: a group here is **pass-through, not
// isolated**. There is no offscreen accumulator anywhere in this file for a
// group the way §§12-17 build one for a clipping run -- a group's members
// blend directly into the *document's* accumulator, against whatever sits
// beneath the group in the stack, each still under its own blend mode. What a
// group contributes is strictly less than an isolated group would: its own
// `visible`/`opacity` scale every member uniformly, nothing more.
//
// **Why pass-through and not isolated, argued rather than merely chosen.** An
// isolated group needs exactly the machinery §§12-17 built for a clipping
// run -- open an accumulator, fold every member into it under its own mode,
// close it back to premultiplied -- except the "base" a clipping run folds
// against is a real layer with real alpha, while an isolated group's base
// would be *transparent black*, which changes what several of that
// machinery's lazy-open guarantees mean (a clipping run's "no member
// contributes" case returns the base unchanged; an isolated group's has
// nothing to return unchanged *to*) and interacts with `Mix` pairs and nested
// clip runs inside the group in ways §§12-17 never had to answer because a
// clipping run's members are never composited against one another, only
// against the base. None of that is a small addition, and PRD C12 (P0, what
// this step actually owes) asks only that a group's "visibility, opacity...
// affect how its children composite" -- it does not ask for isolation, and
// nothing in this codebase's PRD demands non-`normal` blend modes compose
// differently inside a group than outside one. Pass-through delivers the
// P0 sentence exactly, at the cost of one multiply per layer, with no new
// accumulator and no new interaction to prove correct against every existing
// one (`Mix`, clipping, masks) at once. Isolation is a real, separate
// feature -- Photoshop's own "Pass Through" vs "Normal" group blend menu --
// and is not built here; if it is ever wanted, `LayerKind::Group` is where
// the choice belongs (a bool member, or a second kind), not a flag threaded
// through this file's walk, so that a file from a build with only one of the
// two behaviours cannot be misread as having the other.
//
// **Nesting is real**: a Group's own `Layer::parent` may itself name another
// Group, and `groupCoverage()` walks the whole ancestor chain, multiplying
// every ancestor's own coverage in. A three-deep nest at 50% each composites
// its members at 12.5% of what they would show top-level -- the same
// "coverage multiplies" rule opacity and a mask already follow at every other
// point in this file.
//
// **Cycle safety.** `Layer::parent` is a plain string on a plain aggregate
// (core/Layer.hpp), so nothing stops a hand-built `Document` -- or a `.npaint`
// this build did not write -- from describing a group that is its own
// ancestor. `groupCoverage()` walks with a bounded visit count (never more
// than `doc.layers.size()` hops) and returns 0.0f the moment it would revisit
// a group, which is the same choice `layerCoverage()` makes for a NaN
// opacity: hide rather than loop. `--selftest` constructs a two-group cycle
// directly (this build's own `GroupLayers` cannot create one -- see
// core/LayerSetOps.cpp) and asserts the walk terminates and reports 0.0f.
struct GroupAncestryResult {
  // Every ancestor Group's coverage, multiplied together; 1.0f for a
  // top-level layer (the empty product), and exactly 0.0f the instant a cycle
  // is detected -- see this section's cycle-safety paragraph.
  float coverage = 1.0f;
  // True the instant a `parent` chain revisits a group already on the walk.
  // Exposed (rather than folded silently into `coverage == 0.0f`, which a
  // hidden or zero-opacity ancestor also produces) so a caller that wants to
  // *say* a document is malformed, rather than merely render it dark, can.
  bool cyclic = false;
};

// Resolves `doc.layers[index]`'s full ancestor chain of groups. A layer with
// an empty `parent`, or one that does not resolve to any Group-kind layer in
// `doc` (a dangling tag -- e.g. from a group deleted since, or a hand-built
// fixture), has no ancestor there and the walk simply stops, exactly as an
// absent mask reads as 1.0 rather than as an error (core/Mask.hpp's own
// precedent for "absent means neutral").
GroupAncestryResult groupAncestry(const Document& doc, size_t index) noexcept;

// `groupAncestry(doc, index).coverage`, spelled out because it is the one
// field every call site below actually wants.
float groupCoverage(const Document& doc, size_t index) noexcept;

// `layerCoverage(doc.layers[index])`, scaled by every ancestor group's own
// coverage. **The one function the walk and the probe both call in place of
// the single-`Layer` overload above**, so "what a group's own visibility and
// opacity do to its children" has exactly one implementation rather than one
// per caller -- `mixedPairTexel()`'s own reason for existing, applied here.
float layerCoverage(const Document& doc, size_t index) noexcept;

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

// Whether a layer has an alpha of its own for something to be clipped by --
// i.e. whether the walk would ever composite a texel *from* it. Exactly the
// test `compositeWalk()` makes before it walks a layer's own tiles, exported
// (rather than kept file-local, which is where it lived before core/DirtyTiles
// needed the identical question) so that a clip base and a compositable layer
// cannot become two different ideas in two files. An Adjustment layer is false
// here by construction (it holds no tiles at all), which is §12's "the alpha
// of the layer below is not a quantity such a layer has".
bool layerHoldsPixels(const Layer& layer) noexcept;

// **A layer whose blend this build cannot composite is composited as `over`
// and warned about by name -- never silently.** Two cases reach here and the
// sentence distinguishes them, because the answer to "when will this work" is
// different:
//
//   * a name outside `BlendMode` entirely (a newer build's "dissolve"), and
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

// The enabled `OpClass::PointA` entries of `ops`, in stack order, as raw
// `core::Op` copies -- `core::OpStack::detectRuns()`'s run boundaries walked
// and flattened, but each entry copied verbatim rather than turned into a
// closure (docs/architecture-review.md P0-5: a per-pixel `applyOpDirect()`
// switch, core/OpStack.hpp, replaced the `std::vector<PointOp>` of
// `std::function` this used to build). Still resolved **once per layer**
// rather than per texel -- an `Op` carrying a `Curve` still copies a
// `std::vector` doing so, the same reason this was hoisted before.
//
// Returns an empty vector for an empty stack, and callers must treat empty as
// "do nothing at all" rather than "apply nothing": see this header's §1 on why
// running `applyOpsPremultiplied()` with no ops is not the identity.
std::vector<Op> layerPointOps(const OpStack& ops);

// `premultiplied` graded by `ops`, or **bit-identically** `premultiplied` when
// `ops` is empty. The one place this codebase decides that an empty op stack
// costs nothing and changes nothing.
std::array<float, 4> gradedPremultiplied(const std::array<float, 4>& premultiplied,
                                         const std::vector<Op>& ops);

// The latent -> RGB projection of one Pigment texel, as a **premultiplied**
// linear-light RGBA texel: `(latentToRgb(latent) * mass, mass)`. See this
// header's §1 on why mass is the alpha.
std::array<float, 4> projectPigmentTexel(const PigmentTexel& texel) noexcept;

// One texel of a mixed Pigment pair, premultiplied, with both layers' op
// stacks and both layers' coverages already applied -- the `P` of this
// header's §3, in one function so that the flattener and the eyedropper cannot
// disagree about what a `Mix` layer looks like.
std::array<float, 4> mixedPairTexel(const PigmentTexel& lower,
                                    const std::vector<Op>& lowerOps, float lowerCoverage,
                                    const PigmentTexel& upper,
                                    const std::vector<Op>& upperOps, float upperCoverage);

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
                                           const std::vector<Op>& ops,
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

// --- Clipping masks (PLAN.md Phase 5 step 9; PRD C9) ----------------------

// Which layer clips which, resolved **once for a whole Document** so that
// core/Composite's walk and core/Probe cannot disagree about it -- the same
// shape, and the same reason, as `mixPairing()`.
//
// See this header's §12: the base of a clipped layer is the nearest layer
// below it that is **not itself clipped** and that actually holds pixels, so a
// run of consecutive clipped layers shares one base rather than eroding one
// another.
struct ClipRuns {
  // `members[b]` lists the layers clipped to base `b`, bottom-to-top. Empty
  // for every index in a document with no clipped layers, which is what makes
  // the whole feature cost nothing there.
  std::vector<std::vector<size_t>> members;
  // `clippedToBase[i]` is true when layer `i` is composited **by its base**,
  // as part of that base's group, and must therefore be skipped by the walk's
  // own per-layer iteration -- exactly as `MixPairing::consumedByAbove` marks
  // the lower half of a mixed pair.
  std::vector<bool> clippedToBase;
  // `clippedWithoutBase[i]` is true when layer `i` carries `clipped` and there
  // is nothing for it to clip to: it is the bottom layer, every layer below it
  // is also clipped, or the nearest non-clipped layer below holds no pixels.
  // Such a layer is composited **unclipped** and warned about; see §17.
  std::vector<bool> clippedWithoutBase;
  // True when any layer in the document carries `clipped`, whether or not it
  // found a base. Lets a caller take the pre-step-9 path verbatim.
  bool any = false;
};
ClipRuns clipRuns(const Document& doc);

// Opens a clipping group on the base's own final premultiplied source texel:
// its **straight** colour, with alpha forced to exactly 1.0f, i.e. the base
// considered as an opaque backdrop for the members to blend against.
//
// Returns `{0,0,0,0}` when `basePremultiplied`'s alpha is not > 0. Callers
// must not open a group there and none does -- a base with no coverage clips
// its members away entirely (§13), so the division is unreachable rather than
// guarded twice.
//
// **Only ever called when a member actually contributes at this texel**, which
// is a correctness requirement and not an optimisation: `clipGroupClose()`
// does not exactly undo this, so a texel where the group turns out to be empty
// must never enter the bracket. §13 says why, and `--selftest` asserts the
// resulting byte-identity.
std::array<float, 4> clipGroupOpen(const std::array<float, 4>& basePremultiplied) noexcept;

// Folds one clipped member into the group: `src` graded by `ops`, scaled by
// `effectiveCoverage` (the member's own opacity times its own mask sample at
// this texel), then blended over `group` under the member's own `mode`.
//
// The group's alpha is **assigned** 1.0f rather than left as the blend's `ao`,
// so "a clipping group's coverage is exactly the base's" is true by
// construction; see §13. `over` against an opaque backdrop rounds to 1.0 in
// practice, and "in practice" is not what an invariant is made of.
//
// A member with nothing to contribute -- no tile at this texel, coverage <= 0
// -- must be skipped by the caller rather than folded with a zero, for the
// same reason every other skip in this file is a skip (§5).
std::array<float, 4> clipGroupFold(const std::array<float, 4>& group, BlendMode mode,
                                   std::array<float, 4> src, const std::vector<Op>& ops,
                                   float effectiveCoverage);

// Closes the group: back to premultiplied, with the base's alpha restored
// unchanged. This is the step that makes the group "clipped by the alpha of
// the layer below" -- it is the only alpha the result can have.
std::array<float, 4> clipGroupClose(const std::array<float, 4>& group, float baseAlpha) noexcept;

// **A layer carrying `clipped` with nothing to clip to is composited
// unclipped, and warned about by name -- never silently, and never dropped.**
// See §17: `core::setLayerClipped()` and `core::moveLayer()` both refuse to
// create the state, but a `.npaint` can carry it (PRD I10), so the compositor
// has to answer for it, and the answer is the one §7 already gives for a blend
// this build cannot honour.
//
// Names the layer by index, by its user-facing name when it has one, and says
// which of the three reasons applies -- the layer count is in the sentence,
// the io/Export and core/LayerOps refusal style applied to a warning.
std::string clippedLayerWithoutBaseWarning(const Document& doc, size_t layerIndex);

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
// sentence per layer whose blend this build could not honour, one
// `adjustmentLayerBlendWarning()` per Adjustment layer carrying a blend mode,
// and one `clippedLayerWithoutBaseWarning()` per layer that asks to be clipped
// with nothing beneath it to clip to. It is appended to, never cleared, so a
// caller can collect from several stages.
std::vector<float> compositeDocumentPremultiplied(
    const Document& doc, std::vector<std::string>* warningsOut = nullptr);

// The same walk as `compositeDocumentPremultiplied()`, writing into a
// caller-owned buffer instead of returning a freshly allocated one --
// `compositeDocumentPremultiplied()` is in fact implemented in terms of this
// function with a local, empty `buffer`, so the two can never drift.
//
// For a one-shot caller (io/Export, `--selftest`) this costs exactly what
// `compositeDocumentPremultiplied()` always has. It exists for the caller
// that composites the *same document size* repeatedly and would otherwise
// pay a fresh `width * height * 4` allocate-and-zero -- for a large canvas,
// hundreds of MB -- on every single call: `buffer` is resized (discarding
// its old contents) only when its size no longer matches the canvas, which
// is the first call and every call after a canvas-size change; every other
// call reuses the existing allocation.
//
// **The zero-fill itself still happens on every call, reused buffer or
// not.** An untouched destination texel has to read transparent black
// afterward exactly as it does for `compositeDocumentPremultiplied()` (see
// that function's own comment on why), and this walk does not write every
// texel -- only the ones a layer's tiles actually cover -- so a stale
// non-zero texel left over from a *previous* call's content would otherwise
// leak through. Skipping only the allocator, via `std::fill` on an
// already-correctly-sized buffer, is the entire saving this function offers
// over the plain one.
void compositeDocumentPremultipliedInto(const Document& doc, std::vector<float>& buffer,
                                        std::vector<std::string>* warningsOut = nullptr);

// --- The same walk, restricted to a tile set (the incremental composite) ---
//
// **Why this is the same function and not a second compositor.** Everything
// §§1-17 above decides -- the pairing, the clip runs, the projection, the
// grade, the mask, the adjustment inversion -- is *unchanged* here. What
// changes is which tiles the three tile loops visit and where the accumulator
// texel lives. That is deliberate to the point of being the whole design:
// `compositeDocumentPremultiplied()` and this function are one walk behind one
// `#include`, so the incremental result cannot drift from the full one.
//
// **Bit-identity, and why it is structural rather than tested-into-existence.**
// The accumulator is per texel: every branch of the walk reads storage at a
// document coordinate and writes the accumulator at that same coordinate, and
// no op in this build reads a neighbour (core/DirtyTiles.hpp §4). So the value
// written at `p` is the same sequence of floating-point operations whether the
// walk visited one tile or every tile -- restricting the tile set removes
// whole texels from the output, never a term from any texel's arithmetic.
// `--selftest` asserts it by `memcmp` over the raw half words across ten kinds
// of edit anyway, because "structural" is an argument and the boundary is a
// number.
//
// The destination rectangle. `pixels` is `width * height * 4` floats,
// row-major, premultiplied linear RGBA, describing the document rectangle
// `[origin, origin + (width, height))`. A tile that falls outside it is
// clipped exactly as one falling outside the canvas is.
//
// **Not the whole canvas**, because the whole point is not to touch it: an
// eight-tile edit on a 2048x2048 document should cost eight tiles of memory
// traffic, and a caller that had to hand over a 64 MiB canvas-sized buffer
// would have paid the canvas anyway. `--selftest` measures the difference.
struct CompositeRegion {
  float* pixels = nullptr;
  PixelCoord origin{0, 0};
  int32_t width = 0;
  int32_t height = 0;
};

// Composites `tiles` of `doc` into `region`, **zeroing each tile's texels
// first** -- the accumulator has to start at transparent black for the same
// reason `compositeDocumentPremultiplied()`'s buffer does, and doing it here
// rather than in the caller keeps "an untouched texel is transparent black"
// in one place.
//
// Texels of `region` outside `tiles` are left exactly as the caller left them.
// A duplicate coordinate is harmless -- the filter is a set and the tile loops
// iterate the *stores*, so no tile is composited twice -- but every caller
// passes a set anyway (`documentDirtyTiles()` and `canvasTiles()` both return
// one), and a coordinate outside the canvas is clipped away exactly as a tile
// that straddles the canvas edge is.
//
// `warningsOut` behaves exactly as it does for the full walk, and produces the
// **same sentences**: every warning is emitted per layer, before any tile is
// visited, so a region composite reports an unimplementable blend, an
// Adjustment layer's ignored mode and a baseless clip just as a full one does.
// `--selftest` asserts the two lists match, because a user must not stop being
// told a document is approximate merely because a frame was cheap.
void compositeDocumentTilesPremultiplied(const Document& doc,
                                         const std::vector<TileCoord>& tiles,
                                         const CompositeRegion& region,
                                         std::vector<std::string>* warningsOut = nullptr);

}  // namespace np
