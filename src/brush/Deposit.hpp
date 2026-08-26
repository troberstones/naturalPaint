#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "brush/StrokePath.hpp"
#include "core/Pigment.hpp"
#include "core/SelectionMask.hpp"
#include "core/Tile.hpp"

// brush/Deposit -- **what happens to a dab.**
//
// ==========================================================================
// 0. What this delivers, what it does not, and what is still owed
// ==========================================================================
//
// Twelve Phase 5 steps deep, no stroke had ever reached a `Layer`. `sim::
// PaintSim` owns one dense `RGBA8Unorm` canvas texture and nothing wrote
// `Layer::pigmentTiles` outside `--selftest`, so `Mix` (PRD C3, **P0**, and
// the reason this application exists) was asserted and never witnessed, and
// `core/History` had never seen a stroke.
//
// **This module is the cheap interim, not the designed fix.** The designed fix
// is the GPU->CPU solver readback in `scratchpad/design-stroke-bridge.md` (983
// lines, 12 steps): the fluid solver runs, its pigment field is read back in
// f16 and *that* is what lands in a Pigment layer, so what the document holds
// is what the water actually did. What is built here instead is the direct
// path -- **the same dab stream `brush/StrokePath` already emits, deposited
// into a Pigment layer's tiles on the CPU, with no solver in the loop at all.**
//
// So, plainly:
//
//   Delivered here          A stroke reaches a Layer. A Pigment layer holds
//                           hand-painted content. `Mix` is visible. A stroke
//                           is one undo step. The dirty-tile path carries an
//                           in-progress stroke to the screen.
//
//   NOT delivered here      Any fluid behaviour whatsoever: no water, no
//                           diffusion, no edge darkening, no granulation, no
//                           paper tooth, no wet-in-wet. A dab deposited here
//                           is a stamp with a falloff, and it stays exactly
//                           where it was stamped forever. Watercolour and oil
//                           still live only in `sim::PaintSim`'s texture and
//                           still never reach a `Layer`.
//
//   Still owed              The solver readback. Until it lands, the medium a
//                           user can *keep* in a document is "flat pigment",
//                           and the medium they can *see* simulated is one
//                           they cannot save. That is the whole of the gap and
//                           it is not narrowed by anything in this file.
//
// **`sim::PaintSim::readbackCanvas()` is deliberately not reused**, and it is
// the shortcut a reader will reach for first: it already returns the canvas as
// pixels, so "just write those into the layer" looks like the bridge for free.
// It is 8-bit and display-referred (`RGBA8Unorm`, `sim/PaintSim.cpp`'s canvas
// target), which violates PRD B6 outright -- a document part must hold
// scene-referred linear data, and 8 bits of sRGB cannot round-trip through the
// f16 tile it would be written into. It also has no latent in it at all, so
// everything a Pigment layer exists for (`Mix`, PRD C3) would have to be
// re-invented from RGB by `rgbToLatent()`, whose decomposition docs/ui.md §3.3
// calls "plausible rather than true". Nothing in this file calls it.
//
// ==========================================================================
// 1. What one dab does to one texel
// ==========================================================================
//
// A dab is a centre, a radius, a falloff and a pigment. Per covered texel it
// **adds mass and mixes latent**, and the rule is exactly:
//
//     dm  = flow * coverage(|texel_centre - dab_centre| / radius) * sel
//     w   = dm / (m + dm)                       // (m + dm == 0 -> w = 1)
//     z'  = lerp(z, z_brush, w)                 // == (z*m + z_brush*dm)/(m+dm)
//     m'  = min(m + dm, kMaxMass * sel)         // ... never below m; see §4
//
// where `sel` is the active selection's coverage at that texel, 1.0 when there
// is no selection. §4 is the whole argument for why it appears in **both** the
// first line and the last one, and what happens to a feathered edge when it
// appears only in the first.
//
// Three things in those four lines are decisions rather than algebra.
//
// **(i) The latent blend is mass-weighted, and it is written as a lerp rather
// than as the quotient.** `(z*m + z_brush*dm) / (m + dm)` and `lerp(z,
// z_brush, dm/(m+dm))` are the same number in exact arithmetic and are *not*
// the same number in floating point. The lerp form is chosen because it is
// exact at the endpoint that matters: on empty paper `m == 0` gives `w = dm /
// dm`, which is exactly `1.0f` for any finite non-zero `dm`, and `std::lerp(a,
// b, 1)` is specified to return `b` exactly. The quotient form computes
// `z_brush*dm/dm`, two roundings that need not come back to `z_brush`. That
// exactness is what makes the hue invariant below assertable at **zero**
// tolerance instead of at an f16 tolerance.
//
// **(ii) `m + dm == 0` yields the brush's latent, and that is the limit, not a
// convention.** `depositDab()` never calls the rule with `dm == 0` -- a texel
// whose coverage is zero is skipped and not rewritten at all, which is also
// what keeps a dab's reported footprint equal to what it changed (§3). So the
// singular case is reachable only by a direct call to `depositTexel()`. It is
// still defined, because a function with an undefined input is a bug waiting
// for a caller: as `dm -> 0+` with `m == 0`, `w = dm/dm -> 1`, so the limit is
// `z_brush`, and the limit is what the function returns. The alternative --
// leaving the destination latent untouched -- is what a mass-0 texel would
// then carry forever: a stale hue at zero coverage, which PRD F10's eraser
// (mass down, "leaving the Latent untouched") deliberately creates and which
// the *next* deposit must not be biased by. `w = 1` is the answer that erases
// that bias.
//
// **(iii) Mass saturates; the mixing weight does not.** `core/Composite`
// projects a Pigment texel as `(latentToRgb(latent) * mass, mass)` -- **mass
// IS the layer's alpha** -- so a mass above 1 is not a bright texel, it is a
// document with alpha 1.4 in it, which no compositor in this codebase or any
// other has a meaning for. The stored mass is therefore capped at
// `kMaxMass == 1` -- or at `kMaxMass * sel` where a selection is only partly
// engaged, which §4 derives and which is the *same* cap with the paper's
// capacity scaled rather than a second mechanism. The *weight* `w`
// deliberately uses the uncapped `dm`, so a
// texel already at full mass keeps taking on the brush's hue as more paint
// goes down (`w = dm/(1+dm) > 0`) instead of freezing at the first colour that
// happened to saturate it. Capping `dm` instead -- "the paper can only hold so
// much" -- would make an opaque area permanently un-repaintable, which is
// wrong for paint and wrong for every application that has ever shipped a
// brush.
//
// **The invariant that justifies storing latents at all.** Two half-mass dabs
// of one pigment must equal one full-mass dab of it -- the deposit must be
// *idempotent in hue*. It is, exactly: with `z == z_brush`, `lerp(z, z, w)`
// returns `z` for every `w`, so repeated deposits of one pigment cannot walk
// the stored latent anywhere, at any mass, in any order. `--selftest` asserts
// that at **zero** tolerance over a whole dab footprint, and asserts the mass
// half at the derived f16 bound (splitting a dab rounds the intermediate mass
// through binary16 twice instead of once; the latent does not round at all,
// because it does not change).
//
// **The invariant's other half, which `--selftest` found rather than confirmed.**
// Below saturation the rule is a *running mass-weighted mean*: after any
// sequence of deposits, `z = sum(z_i * dm_i) / sum(dm_i)`. So it is
// **order-independent** -- two pigments laid down in either order, in any
// number of instalments, give the identical latent. That is ADR-0003's rule
// (deposition depends on distance travelled, never on how many events that
// distance was divided into) holding in *hue* as well as in mass, and it was
// not designed in; the first draft of this header asserted the opposite and
// the test refused it.
//
// Order-dependence appears at **saturation**, and only there: once `m` is
// capped at `kMaxMass` the denominator stops growing, so a later deposit
// carries more weight than an earlier one of the same size. A mass of blue
// then half a mass of red is a different colour from half a mass of red then a
// mass of blue -- which is the behaviour paint has, and it arrives from the
// cap rather than from anything added for it. `--selftest` asserts both halves.
//
// ==========================================================================
// 2. Why the falloff is what it is
// ==========================================================================
//
// `coverage()` is 1 inside a flat core of `hardness * radius`, falls by a
// smoothstep to 0 at `radius`, and is **exactly zero at and beyond `radius`**.
// The last clause is not cosmetic: it is what makes a dab's footprint a
// bounded, checkable set (§3). A Gaussian, the other obvious profile, has no
// zero anywhere, so its footprint is either unbounded or truncated at an
// arbitrary sigma count -- and a truncated Gaussian has a visible step at the
// truncation radius, which is exactly the seam this module cannot afford.
//
// Smoothstep rather than a linear ramp because a linear ramp is C0: its
// derivative jumps at the core edge and at the rim, and overlapping dabs at
// 0.25-radius spacing turn those two circles of curvature discontinuity into
// visible banding along a stroke. `hardness == 1` degenerates to a hard disc
// with no division at all, which is the `DryBrush` end of the range.
//
// ==========================================================================
// 2b. The tip is an ellipse, and why that arrived this late
// ==========================================================================
//
// `roundness` and `angle` have been on `BrushState` since the BRUSH EDITOR
// panel was built, `DynamicTarget::Roundness` and `DynamicTarget::Angle` have
// been two of the DYNAMICS matrix's twelve columns for just as long, the
// `Flat Wash` preset in `brush/Library.cpp` sets them to 0.28 and 35 deg with
// a comment saying they are "what roundness and angle are FOR", and
// `io/AbrBrushes` imports Photoshop's `Rndn` and `Angl` straight into them.
//
// **None of it reached a dab.** `brushTipFor()` dropped both fields on the
// floor, `BrushTip` had nowhere to put them, and this function computed a
// circle. Every one of those five surfaces was telling a user that two
// sliders shape the brush, and painting with them produced the identical
// round mark -- which is the flavour of wrong that is worse than a missing
// feature, because a missing control cannot be *believed*. It was found by
// building the dab preview (`app/DabPreview`): a preview that draws what the
// deposit actually does drew a circle for the flat wash brush, and there is
// no honest way to draw the ellipse in a preview that the deposit will not
// then paint.
//
// So the offset is mapped into the tip's own frame before the radial test:
//
//     u =  dx*cos(angle) + dy*sin(angle)     // along the MAJOR axis
//     v = -dx*sin(angle) + dy*cos(angle)     // along the MINOR axis
//     v /= roundness                          // minor semi-axis = roundness*r
//
// and `coverage` is then §2's profile of `sqrt(u*u + v*v) / radius` exactly
// as before. So `roundness` is the minor/major axis *ratio* it has always
// claimed to be (`radius` stays the semi-MAJOR axis, never the minor), and
// `angle` is the major axis's rotation in degrees, measured from +x toward
// +y -- which, y being down in canvas space, is the clockwise direction on
// screen, and is the same sense Photoshop's `Angl` uses.
//
// **A round tip takes neither branch.** `roundness == 1` and `angle == 0` are
// tested for and skipped, so `d2` is the bit-identical float it was before
// this mapping existed. That is not an optimisation: it is what makes the
// change invisible to every dab this application has ever deposited, to
// `--pigment-stroke-demo` (which builds a default `BrushTip`), and therefore
// to the `canvas` golden reference. A version that always ran the rotation
// would move every existing stroke by a last-bit rounding, which is exactly
// the kind of diff nobody can tell from a real regression.
//
// **`dabPixelBounds()` is deliberately NOT tightened for the ellipse.** The
// minor semi-axis is `roundness * radius <= radius`, so an elliptical tip is
// *inscribed* in the circle the bounds already describe, and §3's fact 1 --
// every texel this module changes lies inside `dabPixelBounds()` -- survives
// unchanged. A rotated ellipse's true bounding box is
// `sqrt((r cos a)^2 + (r*rn sin a)^2)` by `sqrt((r sin a)^2 + (r*rn cos a)^2)`,
// which is tighter, and computing it would buy a shorter scan over texels
// whose coverage is zero and which are therefore never written. §3's *tile*
// set stays tight regardless, because a tile is reported at the moment its
// first texel is written and not from the bounds.
//
// **The floor on `roundness` is 0.01 and it is `io/AbrBrushes.cpp`'s**, which
// already clamps an imported `Rndn` there; the same number is applied here so
// an import cannot produce a tip this function has to defend against. Below
// it the division amplifies `dy` by more than 100x for no visible gain -- at
// the widest radius the UI offers, 200 px, a roundness of 0.01 is a minor
// axis of 2 px, already a hairline -- and at zero it would divide by zero.
//
// ==========================================================================
// 2c. A sampled tip is a second SOURCE of shape, not a second falloff
// ==========================================================================
//
// `io/AbrBrushes.cpp` reads Photoshop's `samp` block -- a UUID-keyed
// greyscale bitmap per brush, usually PackBits-compressed -- and now attaches
// the decoded pixels to a preset as `BrushTipBitmap`. This section is about
// what `dabCoverage()` does with one, and the governing decision is that it
// is a REPLACEMENT for §2's radial profile, not an addition to it: a bitmap
// tip's coverage comes entirely from its own pixels, so `hardness` (which has
// no meaning for a mark that was scanned rather than computed) is ignored
// outright when `BrushTip::bitmap` is set, exactly as the round branch below
// ignores `roundness`/`angle` when they are at their identity.
//
// **The mapping keeps two things a user already turns**, applied in the same
// order and with the same sign convention as the procedural ellipse, so a
// brush that happens to carry both a bitmap and a Photoshop `Rndn`/`Angl`
// (most of Kyle Webster's inkers do not; one of twelve in the pack this was
// built against does) is not a special case:
//
//   1. The query offset `(dx, dy)` is rotated by `angle` and its second axis
//      divided by `roundness`, **identically** to §2b's `u`/`v` -- so a
//      bitmap tip squashes and rotates the same way a circle does, rather
//      than needing a second rotation convention nobody would notice differs
//      from the first until a brush imported with both looked wrong.
//   2. The rotated, squashed `(u, v)` is then a coordinate in an **isotropic
//      frame where the tip's own bounding square is `[-radius, radius]` on
//      each axis** -- and that square is mapped onto the bitmap's native
//      `width x height` rectangle, independently per axis, so a landscape
//      sample and a portrait one both fill their tip circle along their own
//      long side. `radius` therefore means the same thing it always has --
//      the semi-MAJOR axis, `BrushTip::roundness`'s own comment -- for a
//      bitmap tip too: whichever of `width`/`height` is larger maps onto
//      `radius` exactly, and the other maps onto a shorter half-extent in
//      proportion.
//
// A point that rotates and squashes onto bitmap coordinates outside
// `[0,width] x [0,height]` has **zero** coverage -- there is no "outside the
// disc" test for a bitmap tip because the bitmap's own rectangle already is
// one, unlike the round/elliptical branch's explicit `d2 < r2` gate. That
// matters for `dabPixelBounds()`: a non-square bitmap's un-rotated footprint
// is already a rectangle inscribed in the `radius`-square (by construction,
// per point 2 above), but a ROTATED one is not -- a `120 x 93` sample turned
// 45 degrees needs more horizontal reach than its own un-rotated width. So
// `dabPixelBounds()` computes a bitmap tip's half-extents from the standard
// axis-aligned-bounding-box-of-a-rotated-rectangle formula
// (`|w cos| + |h sin|`, `|w sin| + |h cos|`) rather than reusing `radius`
// symmetrically -- the one place this feature could not simply inherit §2b's
// "bounds stay a `radius`-square, tightening is not worth it" argument,
// because for the procedural ellipse that square is provably a superset
// (roundness only ever shrinks the minor axis) and for a rotated rectangle it
// provably is not.
//
// **The greyscale-to-coverage polarity was measured, not assumed.** A
// scanned brush tip is intuitively "dark mark on light paper", which reads as
// "0 is the ink" -- and that is backwards for a Photoshop `.abr`. Rendered as
// ASCII art, the smallest sample in Kyle Webster's Runny Inkers pack (a round
// dot, 14x14) is a solid block of value-255 texels inside the circle and
// value-0 outside it: **255 is full coverage, 0 is none**, the same sense as
// every other coverage value this module produces. Guessing the opposite
// would have painted every imported sampled brush as its own photographic
// negative -- a solid dot importing as a ring -- and nothing short of this
// check would have said so, because an inverted mask is still a plausible-
// looking brush shape.
//
// **Sampling is bilinear**, because a sampled tip is drawn at whatever
// `radius` the SIZE dynamics resolve to this dab, almost never the sample's
// own native pixel dimensions, and nearest-neighbour resampling of a hand-
// scanned mark magnifies badly (a soft charcoal edge turns into visible
// stair-steps). The four texels a query point falls between are clamped to
// the bitmap's own edge rather than treated as transparent outside it, which
// is what keeps the mapped rectangle's own border crisp instead of feathering
// it by half a texel for free.
//
// ==========================================================================
// 2d. Dual Brush: a second tip, composited into the same one number
// ==========================================================================
//
// Photoshop's Dual Brush stamps a SECOND tip through the first, combined by a
// blend mode -- Multiply and Overlay are the two `io/AbrBrushes.cpp` reads and
// the two `dabCoverage()` implements. It is most of why an ink brush reads
// granular rather than smooth: the second tip is what breaks up the first
// tip's edge, and until this section existed `io/AbrBrushes.cpp` could only
// detect that a preset wanted one and say it could not be honoured.
//
// **The representation is a nested `BrushTip`, and the obvious question is
// recursion: can a dual brush have a dual brush?** Photoshop's own Dual Brush
// panel has no such control -- there is nowhere in the UI to pick a THIRD
// tip -- but `BrushTip::dualTip` is a plain field on an untrusted-file-derived
// struct, and nothing in the C++ type stops a second level from being built.
// The answer is **no, enforced structurally, not by convention**: the coverage
// function that reads a tip's shape (`singleTipCoverage()`, brush/Deposit.cpp)
// never looks at that tip's own `.dualTip` at all. `dabCoverage()` calls it
// once on `tip` and, when `tip.dualTip` is set, once more on `*tip.dualTip` --
// and that second call runs the identical function, which still does not
// consult `.dualTip`. So even a `BrushTip` built with two or three levels of
// nesting composites as if it had exactly one; the deeper levels are inert
// data, never visited by the one function that would need to visit them to
// matter. `io/AbrBrushes.cpp`'s own reader repeats the same discipline on the
// way in: the helper that reads a `Brsh`-shaped tip object never looks for a
// nested `dualBrush` key on the object it is given, so a hand-crafted `.abr`
// that puts a `dualBrush` inside a `dualBrush` cannot make one arrive here
// either. Two independent refusals of the same recursion, at import time and
// at composite time, because only one of them running is a promise and this
// is not a place for a promise.
//
// **Only shape fields are read from the nested tip.** `dabCoverage()`'s helper
// reads `radius`/`roundness`/`angle`/`hardness`/`bitmap` off `*tip.dualTip`,
// exactly as it reads them off `tip` itself -- so a dual tip squashes, rotates
// and samples a bitmap by the identical rules §2b and §2c already state, with
// no second convention to keep in sync. Nothing else on the nested `BrushTip`
// is read for compositing: not `pigment`, `linearRgb`, `flow`, `opacity` (a
// dual brush loads no colour of its own in Photoshop, it only reshapes the
// mark) and not `spacing`/`scatter` (see below).
//
// **The two tips are sampled at the SAME offset, `(dx, dy)` from one shared
// dab centre.** That is exact for `Cnt 1` with no scatter -- Photoshop stamps
// the second tip once, centred on the first, in that configuration, and this
// is the identical answer. It is an approximation for anything else: a real
// Dual Brush also has its OWN spacing, its own scatter and its own count
// (`useScatter`/`Cnt `/`bothAxes`/`countDynamics`/`scatterDynamics` on the
// descriptor), which would stamp the second tip several times per dab of the
// first, jittered around it. None of that reaches this function -- there is
// no per-dab loop here to multiply, `depositDab()` calls `dabCoverage()`
// exactly once per texel per dab regardless of what the tip carries -- so
// `io/AbrBrushes.cpp` reads those five keys only far enough to say, per
// brush, that this gap exists (`AbrImportResult::dualBrushCadenceNotHonoured`)
// rather than silently painting a smoother mark than Photoshop's Count and
// Scatter would. Landing shape compositing correctly and saying plainly what
// is not yet landed was chosen over landing all four pieces half working.
//
// **Combining is per-texel, on the coverage SCALAR, not on any RGBA notion of
// blending.** A coverage value is already the one number `depositDab()` folds
// into `deltaMass`, so Multiply (`base * second`) and Overlay (the standard
// two-branch formula, base as the "bottom" layer since it is the primary
// tip's own shape and the second tip is what Photoshop describes as blending
// ONTO it) are defined directly on that scalar in [0,1] rather than routed
// through `core/Blend.hpp`'s pixel/layer blend modes, which combine four-
// channel premultiplied colour and have no notion of a bare coverage float.
//
// **The zero-base short-circuit is what keeps `dabPixelBounds()` correct
// without widening it for a larger second tip.** Both formulas give exactly
// `0` when `base == 0` (Multiply trivially; Overlay's `base < 0.5` branch is
// `2 * base * second`, which is `0` for any `second` when `base == 0`), so a
// dual tip with a LARGER radius than the primary can never make a dab paint
// outside the primary tip's own disc -- `dabCoverage()` returns exactly `0`
// there rather than computing an unreachable nonzero value. `dabPixelBounds()`
// therefore needs no dual-brush case at all: it already reports every texel
// where the PRIMARY tip's own coverage can be nonzero (§3, fact 1), and that
// is now also every texel where the COMBINED coverage can be nonzero. The
// implementation makes this exact rather than approximate: `dabCoverage()`
// returns the base tip's own coverage function's `0.0f` literal without ever
// evaluating the second tip, so a texel outside the primary's disc costs
// nothing extra even when the second tip is a bitmap.
//
// ==========================================================================
// 3. Which tiles a dab touches, and why the set is complete
// ==========================================================================
//
// A dab near a tile boundary covers up to four tiles, and **a missed tile is a
// stroke with a visible seam that nothing will ever repair** -- no later pass
// revisits a tile that was not reported. Completeness rests on two facts, in
// this order:
//
//   1. Every texel this module changes lies inside `dabPixelBounds()`. That
//      holds because `coverage()` returns exactly `0.0f` for every offset with
//      `dx*dx + dy*dy >= radius*radius`, tested by the squared comparison
//      before any square root or division happens, and the bounds are the
//      integer texel rectangle containing that disc.
//   2. Every tile holding such a texel is reported, because the tile is
//      reported at the moment its **first** changed texel is written -- the
//      same `if (tile == nullptr)` that fetches it. There is no separate
//      "which tiles did I touch" calculation that could disagree with the
//      writes; reporting and writing are the same branch.
//
// The second fact is also what keeps the set *tight*. Reporting the bounding
// box's tiles instead would be safe and would cost a **224 KiB allocation per
// tile that the dab clipped but never wrote** -- a dab one texel inside a tile
// corner would allocate three empty tiles and hand three empty tiles to the
// incremental composite and to every history entry from then on.
//
// `--selftest` does not take either fact on trust: it brute-force scans the
// whole layer before and after a deposit and asserts the reported set is
// exactly the set of tiles whose bytes changed, including a dab centred on the
// corner where four tiles meet.
//
// ==========================================================================
// 4. The selection bounds the deposit (PRD E1, P0) -- and `kMaxMass` alone
//    does NOT bound it
// ==========================================================================
//
// "Every deposit and every op respects the active selection", and until this
// section existed **this was the one deposit in the build that did not**.
// `depositDab()` took no `Selection` at all, so `app/StrokeSession`'s pigment
// branch could not pass one while the RGB branch on the line directly above it
// passed `doc_->selection` explicitly: painting natural media on a Pigment
// layer -- the layer kind `Document::createBlank()` actually makes -- went
// straight through the marching ants. PRD E1 is **P0**.
//
// **The two nulls, which are opposites and are both reachable.**
// `core/SelectionMask.hpp` states the rule and this loop obeys it: a null
// `Selection*` is "no restriction anywhere" and weighs 1.0; a **null tile**
// inside an engaged selection is "selects nothing here" and weighs 0.0. That
// header requires every hoisted per-texel loop to own its own copy of the first
// branch (a hash lookup per texel is not affordable, so the tile is fetched
// once per tile and `selectionTileCoverage()` is called per texel), and warns
// that a perturbation inverting one copy leaves the others right. This is one
// such loop; `--selftest` drives both nulls through it.
//
// **The selection enters twice, and the second one is what makes it a bound.**
// `brush/RgbDeposit` §4 found the same thing on the other storage, but the
// argument there rests on a per-stroke accumulator and **this route has none**
// -- its ceiling is `kMaxMass`, a property of the paper. So the question "does
// the walk-through problem exist here too?" has to be answered from what
// `depositTexel()` actually does, and the answer is **yes, and it arrives
// faster than in RGB**:
//
//   * Gate only the rate, `dm = flow * cov * sel`, and mass still accumulates
//     **linearly** toward `kMaxMass`: after N dabs a texel holds
//     `min(N * flow * cov * sel, 1)`. For any `sel > 0` that reaches 1 exactly,
//     in `ceil(1 / (flow*cov*sel))` dabs. At the shipped defaults -- `flow`
//     0.35 (`BrushTip::flow`), `cov` 1 under the tip's flat core -- a
//     **half**-selected texel saturates in **6 dabs**, and at the 0.25-radius
//     default spacing six dabs is **1.5 radii of travel**. One ordinary pass of
//     the brush across a feathered selection edge therefore makes it fully
//     opaque: the feather is not softened, it is deleted.
//   * That is strictly worse than the RGB route's version of the same defect.
//     There `A' = A + w(1-A)` approaches the ceiling asymptotically and never
//     arrives, so the edge degrades gradually with scrubbing; here the cap is
//     hit exactly, and quickly, by a stroke drawn at ordinary speed.
//   * So **`kMaxMass` does not already handle it.** It bounds the mass at 1
//     regardless of coverage, which is precisely the bound that ignores the
//     selection.
//
// The fix is the one the shape of this route already suggests: `kMaxMass` is
// the *paper's* capacity, so scale the paper rather than inventing a stroke
// ceiling the pigment route deliberately does not have.
//
//     dm  = flow * cov * sel                 // one dab lays `sel` of what it would
//     cap = kMaxMass * sel                   // and NO number of dabs goes past it
//
// with two clauses that are not decoration:
//
//   * **`cap` is never lowered below the mass already stored.** A deposit must
//     never *remove* paint. A texel holding mass 0.9 from an earlier unselected
//     stroke, dabbed again through a half-engaged selection, keeps its 0.9 --
//     it simply gains nothing. Without this clause the brush would be an eraser
//     wherever the selection is thinner than the paint underneath it, which is
//     the single most alarming way a selection gate can be wrong.
//   * **`cap` is never raised above `kMaxMass`.** `sel <= 1`, so this can only
//     bite when a caller hands `depositTexel()` a destination already over the
//     cap; clamping at the point of storage keeps the invariant a property of
//     the *document* rather than of one reader, which is the same discipline
//     `kMaxMass`'s own comment states.
//
// At `sel == 1` the two clauses collapse to `min(m + dm, kMaxMass)` **bit for
// bit** -- `max(m, kMaxMass)` is `kMaxMass` for every legal `m` -- so a
// document with no selection, `--pigment-stroke-demo` and the `canvas` golden
// reference all deposit the identical floats they did before this section
// existed. That exactness is deliberate and is what makes the gate free.
//
// **What the cap does NOT bound, and why that is right.** The mixing weight `w`
// still uses the uncapped `dm` (§1(iii)), so hue keeps moving on a texel that
// has reached its selection-scaled cap, exactly as it does on one that has
// reached `kMaxMass`. A half-selected texel can therefore be repainted to any
// colour -- it just cannot be made more *present* than half. That is what a
// coverage mask means: the selection says how much of this edit lands here, not
// which colour it is allowed to be. Bounding the hue as well would need the
// pre-stroke latent remembered per texel, which is `brush/RgbErase` §2's
// rejected `A0` store wearing a different hat.
//
// **Order-independence moves with the cap, it does not break.** §1's running
// mass-weighted mean is order-independent *below saturation* and order-
// dependent at it; a partial selection simply lowers where saturation is. Two
// pigments laid through a half-engaged selection commute up to mass 0.5 and
// stop commuting above it, for the identical reason and by the identical
// mechanism.
//
// **Repeated separate strokes.** The cap is on the *stored mass*, not on a
// stroke, so unlike the RGB route's ceiling it does not reset at pen-up: a
// second pass through a half-selected texel cannot lift it past 0.5 either.
// That is a stronger guarantee than the RGB route gives (`brush/RgbDeposit` §4
// records that two passes there reach 0.5 then 0.75), and it is stronger
// because it costs nothing here -- there is no accumulator to reset. It is also
// the more defensible reading of a coverage mask, and it is what
// `clearThroughSelection(PigmentTileStore&)` already implies by scaling mass
// alone.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `History`, no `OpenDocument`.** This module is
// `core/`-only and takes a `PigmentTileStore&`: it is the arithmetic of
// deposition and nothing else. The stroke lifecycle -- pen-down, live
// feedback, one undo step at pen-up, and which tool routes here at all --
// is `app/StrokeSession`, because it needs the document record and the
// history that `app/` owns, and a `brush/` -> `app/` include edge would be
// upside down (`color/ core/ ops/ app/ io/ ui/ gfx/ paint/ sim/ brush/` are
// directory groupings, never namespaces, but they still have a direction).
//
// **No pressure, tilt, jitter, texture or scatter.** `BrushTip` is what the
// deposit reads. `app/AppState`'s `BrushState` already turns pressure into a
// radius and a flow multiplier for the solver path; a second copy of that
// mapping here would be a second place for it to drift.
namespace np {

// The cap on stored mass, and therefore on a Pigment layer's alpha.
//
// 1.0 rather than "uncapped, clamp at the composite": `core/Composite` reads
// `mass` straight into the alpha channel of a premultiplied texel, and every
// consumer downstream of it (the blend table, `layerCoverage()`'s product,
// io/Export's quantization) assumes alpha is in [0,1]. Clamping at the point
// of storage means the invariant is true of the *document* and not merely of
// one reader of it -- and it is the same discipline `MaskTile::readCoverage()`
// already applies at its own boundary.
inline constexpr float kMaxMass = 1.0f;

// The narrowest tip §2b will draw. `io/AbrBrushes.cpp`'s own clamp on an
// imported `Rndn`, restated here so the deposit is defended at the point of
// use and not only at the one importer that happens to share the number.
inline constexpr float kMinRoundness = 0.01f;

// A sampled tip's decoded pixels -- §2c. Built once by `io/AbrBrushes.cpp`
// from a `.abr`'s `samp` block and shared from there on, never copied: a
// preset carrying one is duplicated by `presetFromBrush()`, applied to the
// live brush by `applyPresetToBrush()` and read into a fresh `BrushTip` by
// `app/StrokeSession::brushTipFor()` on every dab of every stroke, and a deep
// copy at any one of those points would be the per-dab allocation
// CONTEXT.md's *Lightweight* exists to refuse. Immutable once built, which is
// what makes sharing it safe without a lock: nothing in this codebase ever
// mutates a `BrushTipBitmap` after `io/AbrBrushes.cpp` returns it.
struct BrushTipBitmap {
  int32_t width = 0;
  int32_t height = 0;

  // Row-major, top-to-bottom, one byte per texel, size `width * height`.
  // **255 is full coverage, 0 is none** -- the same sense as `dabCoverage()`'s
  // own return value, and §2c records the ASCII-art check against a real
  // sample that pinned this polarity down rather than assumed it.
  std::vector<uint8_t> alpha;
};

// How a Dual Brush's second tip combines with the first (§2d), Photoshop's
// `BlnM` on the `dualBrush` descriptor. Only these two: `io/AbrBrushes.cpp`
// reads every other `BlnM` value it recognises as UNSUPPORTED rather than
// guessing (`AbrImportResult::dualBrushUnsupportedBlend`), so this enum never
// needs an "other" member -- a `BrushTip::dualTip` is never set for a blend
// mode this build cannot compute.
enum class DualBrushBlend {
  Multiply,
  Overlay,
};

// One stamp of the brush tip: its shape, its load, and what it is loaded with.
//
// `spacing` is carried here rather than left to the caller because it belongs
// to the tip -- `brush/StrokePath` wants `spacing * radius` in pixels and
// nothing else in a stroke knows both numbers. It is in units of the radius,
// the same convention and the same 0.25 default `app/AppState`'s `BrushState`
// already uses (ADR-0003: deposition depends on distance travelled, never on
// time or event count).
struct BrushTip {
  // Pixels. Coverage is exactly zero at and beyond this distance from the
  // dab centre; a radius of 0 or less deposits nothing at all.
  float radius = 24.0f;

  // The fraction of the radius that is the flat, fully-covered core, in
  // [0,1]. 0 is a pure smoothstep from the centre; 1 is a hard disc.
  float hardness = 0.35f;

  // Minor/major axis ratio of an elliptical tip, in (0,1]; 1 is round. See
  // §2b. `radius` above is the semi-MAJOR axis whatever this holds, so
  // narrowing a tip never widens it.
  //
  // Defaulted to 1 so that every `BrushTip` built by naming its fields --
  // `--pigment-stroke-demo`, half of `--selftest`, this struct's own
  // aggregate initialisation -- is the exact circle it was before §2b.
  float roundness = 1.0f;

  // The major axis's rotation in degrees, from +x toward +y. Ignored, and not
  // merely ineffective, when `roundness == 1`: §2b's rotation branch is
  // skipped outright for a round tip, because rotating a circle is arithmetic
  // that can only introduce a rounding.
  float angle = 0.0f;

  // A sampled bitmap tip (§2c), or null for the procedural round/elliptical
  // profile `dabCoverage()` computes from `hardness`/`roundness`/`angle`
  // above. Shared, not owned -- see `BrushTipBitmap`'s own comment. `radius`,
  // `roundness` and `angle` still apply to a bitmap tip (§2c point 1); only
  // `hardness` does not, because a scanned mark has no core-to-rim falloff of
  // its own for a fraction to describe.
  std::shared_ptr<const BrushTipBitmap> bitmap;

  // A Dual Brush's second tip (§2d), or null for every tip that has none --
  // every built-in, every hand-authored preset, and a `.abr` brush whose Dual
  // Brush is off or whose `BlnM` this build does not composite. Only its
  // SHAPE fields (`radius`/`hardness`/`roundness`/`angle`/`bitmap`) are read;
  // its own `pigment`/`flow`/`opacity`/`spacing`/`scatter`/`dualTip` are not
  // (§2d). **Never itself carries a non-null `dualTip`** -- not enforced by a
  // constructor check, because the function that would need to enforce it,
  // `dabCoverage()`'s shape helper, simply never reads that far, which is the
  // stronger guarantee (§2d's whole argument).
  std::shared_ptr<const BrushTip> dualTip;

  // How `dualTip`'s coverage combines with this tip's own (§2d). Meaningless
  // when `dualTip` is null.
  DualBrushBlend dualBlend = DualBrushBlend::Multiply;

  // Mass laid down per dab where coverage is 1. Not clamped to [0,1] here --
  // a flow above 1 is a legitimate "one dab saturates the paper" tip, and the
  // cap that matters is on the stored mass, not on the tip.
  float flow = 0.35f;

  // Arc-length dab spacing, in radii. `spacingPx()` is what StrokePath wants.
  float spacing = 0.25f;

  // `DynamicTarget::Scatter`'s resolved magnitude, in radii -- an Add target
  // (brush/Dynamics.hpp), identity 0.0. **Not a position**: this is how FAR a
  // dab may be jittered from the stroke path, not which way. The direction is
  // drawn per dab (`app/StrokeSession`'s deposit loop), independently of
  // whatever fed this magnitude, so two dabs at the same scatter magnitude
  // land in different places -- a jitter with a fixed direction would just be
  // a second, smaller stroke offset from the first, not scatter.
  float scatter = 0.0f;

  // Which AXIS `scatter` above may displace a dab along -- Photoshop's own
  // Scatter panel "Both Axes" checkbox (docs/reachability-audit.md B5).
  // False (the default, and Photoshop's own default) confines the jitter to
  // the PERPENDICULAR of the stroke's own tangent, so a rougher line reads as
  // wider, not blurrier; true scatters isotropically, in a fresh direction
  // per dab. `app/StrokeSession`'s `applyPerDabScatter()` is what reads this.
  bool scatterBothAxes = false;

  // What the tip is loaded with. `paint/Palette`'s `MixboxLut::rgbToLatent()`
  // produces one from a colour; `core/Pigment`'s `latentToRgb()` projects it
  // back with no LUT at all, which is why a Pigment layer composites in a
  // build that never loaded the 512x512 texture.
  Latent pigment{};

  // **The same load, in the shape an RGB layer can hold**: STRAIGHT LINEAR
  // colour, for `brush/RgbDeposit`. Not a second colour -- `brushTipFor()`
  // derives this and `pigment` above from the one palette entry, in one place,
  // so a tip that paints one colour on a Pigment layer and a different one on
  // an RGB layer is not a thing that can be built. Exactly one of the two is
  // read per stroke, decided by `strokeRouteFor()`.
  //
  // Linear rather than the palette's display-referred sRGB because a document
  // part is scene-referred (DESIGN-imaging.md, PRD B6); brush/RgbDeposit.hpp §1
  // says what skipping the decode looks like, and it does not look like a
  // missing conversion.
  std::array<float, 3> linearRgb{0.0f, 0.0f, 0.0f};

  // **The ceiling ONE STROKE can reach**, in [0,1] -- distinct from `flow`
  // above, which is what one DAB lays down. brush/RgbDeposit.hpp §2 is the
  // whole argument for why these are two numbers and what applying opacity per
  // dab instead would do to a slow stroke.
  //
  // Read only by the RGB route. The pigment route has no equivalent today: its
  // ceiling is `kMaxMass`, a property of the paper rather than of the stroke,
  // and giving a pigment stroke a mass ceiling of its own is a decision about
  // what a half-loaded brush means physically, not a plumbing job.
  float opacity = 1.0f;

  // Floored exactly as `ui/MacPaintUI`'s solver path floors it, so the two
  // stroke routes cannot emit dabs at different spacings from one tip.
  float spacingPx() const noexcept {
    const float px = spacing * radius;
    return px > 0.1f ? px : 0.1f;
  }
};

// The dab's coverage profile at an offset from its centre, in [0,1]. See §2
// for the profile, §2b for the ellipse and §2c for a sampled bitmap tip.
//
// Exactly 0.0f for every offset at or beyond the tip's rim, exactly 1.0f
// inside `tip.hardness` of the way to it, and a smoothstep between them --
// where "the rim" is the circle of `tip.radius` for a round tip and §2b's
// ellipse for any other -- **unless `tip.bitmap` is set**, in which case §2c's
// mapping and the bitmap's own pixels decide coverage outright and
// `tip.hardness` plays no part.
//
// **When `tip.dualTip` is set** (§2d), this is `tip`'s own coverage by the
// rule above, combined with `*tip.dualTip`'s coverage (by the SAME rule,
// applied to the nested tip) through `tip.dualBlend`. Still in [0,1], still
// exactly 0.0f wherever `tip`'s OWN coverage is 0.0f, which is what keeps
// `dabPixelBounds()` correct without a dual-brush case of its own.
//
// **This is the one function that decides what a dab looks like**, and it has
// exactly two callers by design: `depositDab()` below, and `app/DabPreview`,
// which shows a user what one dab will do. A preview with its own falloff
// would agree with this on the day it was written and drift the first time
// §2 changed -- and a preview that lies is worse than no preview, because a
// user who cannot trust it has to paint to find out anyway.
float dabCoverage(const BrushTip& tip, float dx, float dy) noexcept;

// The rule of §1, as a pure function of one texel, for the one reason a pure
// function earns its keep here: the invariants are about *this arithmetic*,
// so `--selftest` asserts them on this and not on a tile of it.
//
// Defined for every finite input, including `dst.mass + deltaMass == 0` (see
// §1(ii): the result takes `pigment` as its latent and keeps mass 0).
//
// `selection` is the active selection's coverage at this texel, in [0,1], and
// it is the **cap** half of §4 -- `deltaMass` is expected to have the *rate*
// half already folded in, exactly as `depositRgbTexel()` takes a `weight` with
// the selection in it and an `opacity` ceiling separately. Values outside [0,1]
// are clamped rather than trusted, because a grow/shrink or a boolean can land
// a hair outside and an unclamped one would lift the document's own mass cap.
//
// **Defaulted to 1.0, and that default is the identity rather than a way of
// leaving callers alone.** `core/SelectionMask.hpp`'s convention is that the
// *absence* of a selection means "no restriction", weight 1.0 -- so 1.0 here is
// the same statement, and §4 shows the arithmetic at 1.0 is bit-identical to
// the rule this function had before the parameter existed. The callers that
// take the default are the ones for which that is the truth: `app/DabPreview`
// draws one dab on empty paper with no document and therefore no selection, and
// the texel invariants in `--selftest` are claims about the mixing arithmetic
// and not about the gate. The *loop* below deliberately does not default it.
PigmentTexel depositTexel(const PigmentTexel& dst, const Latent& pigment, float deltaMass,
                          float selection = 1.0f) noexcept;

// The inclusive texel rectangle a dab centred at `centre` can change, clipped
// to `[0,canvasW) x [0,canvasH)`. Empty (`x1 < x0` or `y1 < y0`) when the dab
// falls entirely outside the canvas or has no radius.
//
// A texel is sampled at its **centre**, `(x + 0.5, y + 0.5)`, which is the
// convention that makes a dab centred exactly on a tile corner symmetric
// across all four of the tiles it lands on -- the boundary case §3 tests.
//
// **For a bitmap tip (§2c) the half-extents are not symmetric** when
// `tip.angle != 0`: a rotated non-square sample's axis-aligned bounding box is
// wider than its own un-rotated footprint, so this computes it from the
// rotated-rectangle formula rather than reusing `tip.radius` on both axes.
// Still an over-approximation and still uncorrected for `roundness`, for the
// same reason §2b gives for the ellipse: roundness only ever shrinks a
// bitmap tip's visible footprint below this box, never grows it.
//
// **Computed from `tip` alone, with no `tip.dualTip` case, deliberately.** §2d
// derives why: `dabCoverage()` is exactly 0 wherever `tip`'s OWN coverage is
// 0, for both Multiply and Overlay, regardless of the second tip's radius --
// so a dual tip strictly larger than the primary still cannot make a dab
// paint outside the box computed here. Widening this function for a second
// tip would be defending against a case that cannot occur.
struct PixelBounds {
  int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
  bool empty() const noexcept { return x1 < x0 || y1 < y0; }
};
PixelBounds dabPixelBounds(const BrushTip& tip, Vec2 centre, int32_t canvasW,
                           int32_t canvasH) noexcept;

// What one deposit changed. Counted, not estimated: `texels` is incremented at
// the write and `tiles` at the fetch.
struct DepositCount {
  size_t texels = 0;
  size_t tiles = 0;
};

// Deposits one dab into `store`, clipped to the canvas and gated by
// `selection` (nullptr means no restriction, §4).
//
// **Neither trailing parameter is defaulted, and that is deliberate.** The
// obvious shape for this change was `const Selection* selection = nullptr`
// after `touchedOut`, which would have left all nine existing call sites
// compiling untouched -- and that is exactly the objection to it: "nothing had
// to change" is indistinguishable from "nothing was checked", and the one thing
// this parameter must not be is easy to forget at a call site. The order is
// `RgbStroke::depositDab()`'s, argument for argument, so the two routes read
// identically and a reader moving between them cannot transpose the last two.
//
// Every tile it writes is appended to `touchedOut` when that is non-null,
// exactly once per dab, at the moment the tile is first written -- see §3 on
// why that is the same branch as the write. Duplicates across dabs are
// expected and are the caller's to fold (`sortUniqueTiles()`).
//
// `store.getOrCreate()` is what fetches the tile, so a tile shared with a
// history entry is copied here, once per dab that touches it, and the entry's
// copy is left exactly as it was. That is the whole of what makes one stroke
// cost its own tiles and not the layer's.
DepositCount depositDab(PigmentTileStore& store, const BrushTip& tip, Vec2 centre,
                        int32_t canvasW, int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

// Sorts ascending by (y, x) and removes duplicates, in place.
//
// That order is `documentDirtyTiles()`'s own, and it is load-bearing rather
// than tidy: `ui/DocumentTexture` uploads the dirty set one **tile band** at a
// time, a band being a maximal run sharing a tile row, so a set sorted any
// other way costs one upload per tile instead of one per row.
void sortUniqueTiles(std::vector<TileCoord>& tiles);

// What a whole stroke's worth of dabs changed.
struct StrokeDeposit {
  size_t dabs = 0;
  size_t texels = 0;
  // Sorted ascending by (y, x), unique. Ready for
  // `compositeDocumentTilesPremultiplied()`.
  std::vector<TileCoord> tiles;
};

// Deposits every dab in `dabs`, in order, gated by `selection` (§4). Order
// matters: §1's mixing rule is not commutative across two different pigments,
// and a stroke is a sequence.
StrokeDeposit depositDabs(PigmentTileStore& store, const BrushTip& tip,
                          const std::vector<Vec2>& dabs, int32_t canvasW, int32_t canvasH,
                          const Selection* selection);

}  // namespace np
