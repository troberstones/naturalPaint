#pragma once

#include <cstdint>

#include "core/SelectionMask.hpp"
#include "ops/Blur.hpp"

// ops/Feather -- PRD E4's feather, which is a blur of a selection's coverage.
//
// PLAN.md phase 7 puts feather in the selection chapter and says how:
// "**feather via phase 6's blur**". DESIGN-imaging.md agrees from the other
// side -- "Feather wants the blur from phase 6 ... so selections land naturally
// *after* the filter chapter". This file is the join, and it is deliberately
// small: everything numerical happens in ops/Blur, and what is left here is
// three decisions that are about *selections* rather than about blurring.
//
// It lives in its own translation unit rather than inside core/SelectionOps
// for a reason that outlives the merge convenience: core/ is the domain model
// and knows nothing about ops/ (core/SelectionOps.hpp includes only
// core/SelectionMask.hpp, and core/Composite, core/Histogram and the rest keep
// the same discipline). A feather that lived in core/SelectionOps.cpp would
// make the domain model depend on the filter library, in the same direction
// core/TileStore.hpp refuses to depend on io/. The dependency belongs this way
// round: ops/ knows about selections, core/ does not know about ops/.
//
// ==========================================================================
// Decision 1: the blur must read OUTSIDE the stored tiles, and that is the
// whole reason this is not two lines
// ==========================================================================
//
// core/SelectionMask.hpp's central rule: **an absent selection tile means
// coverage 0.0** -- the INVERSE of a layer mask, where an absent tile means
// reveal. So a selection's stored tiles are exactly the tiles it selects
// something in, and everything beyond them is genuinely unselected.
//
// Feathering an edge therefore has to produce coverage in tiles the input does
// not have. A hard-edged 64x64 marquee sitting inside one tile becomes, after
// a feather, a soft blob whose support is larger than the marquee -- and if
// the radius puts the edge near a tile boundary, larger than the tile. An
// implementation that walked only `selection.tiles` and blurred each in place
// would produce a feather with a hard outer cut-off exactly on the tile grid,
// which is the seam bug of ops/Blur wearing a different hat.
//
// So the output rectangle here is the input's tile extent **expanded by the
// blur's apron**, and the gather plane covers all of it. Nothing outside that
// can be non-zero, because everything outside the input's tiles is exactly 0
// and a convolution of zeros is zero -- which is also why this file can hand
// `blurPlane()` a buffer with no apron of its own and still get an exact
// answer everywhere in it. `blurPlane()`'s contract names this case: outside
// the buffer is treated as zero, which is *right* when the signal really is
// zero out there and wrong when the caller merely cropped. Here it really is.
//
// ==========================================================================
// Decision 2: radius -> sigma, so the number on the dialog means something
// ==========================================================================
//
// "Feather radius" is not a defined quantity -- Photoshop's mapping is
// undocumented and a Gaussian has no radius, only a sigma. So this is a
// choice, and it is **sigma = radius / 2**, chosen so that the soft band is
// `radius` texels either side of the original edge.
//
// Measured through this file itself, on a hard-edged 64x64 marquee whose left
// edge falls between texel 31 and texel 32, reading the uint8 store back:
//
//   radius  sigma  apron   cov(31)  cov(32)   at -radius  at +radius  first 0
//      2      1      4     0.30980  0.69020     0.0667      0.9922    1.75 * r
//      8      4     16     0.45098  0.54902     0.0314      0.9843    1.56 * r
//
// Four things to read out of that table:
//
//   **The pair straddling the original edge sums to exactly 1.0** at both
//   radii. That is the symmetric, DC-preserving kernel showing through: every
//   scrap of coverage the feather takes from inside the edge appears outside
//   it. A selection's total coverage is conserved, so a feather does not
//   quietly shrink or grow the area an edit will affect.
//
//   **The 50% contour therefore stays exactly on the original edge.**
//   Feathering softens a selection without moving it, which is what stops a
//   feathered mask from creeping every time the radius is adjusted.
//
//   **The transition is essentially complete within +/-radius** -- 3% coverage
//   remaining `radius` texels outside, 98% `radius` texels inside.
//
//   **Coverage reaches exactly zero at about 1.6 * radius, not at the apron's
//   2 * radius.** The last stretch of the Gaussian tail is below half a uint8
//   step and quantises away, which is what keeps the store from growing by the
//   full apron -- see the cost section.
//
// Rejected: sigma = radius, which is the other obvious mapping. It makes the
// dialog's number mean "one standard deviation", which is a statistician's
// quantity rather than a retoucher's -- the visible soft band would then be
// about 2.5 times the number typed, and every user's mental model of "feather
// 10" would be wrong by that factor.
//
// ==========================================================================
// Decision 3: no document bounds, and the caller clips
// ==========================================================================
//
// `featherSelection()` takes no width/height, unlike core/SelectionOps'
// `invertSelection()`. Coverage may therefore appear up to `2 * radius` texels
// outside the original selection's tiles, including outside the canvas.
//
// The two are not inconsistent. A **complement** is undefined without bounds
// -- "everything else" is infinite -- so invert has to be told where the
// document stops. A **blur** is perfectly well defined on an unbounded
// coverage field, and clipping it here would bake a canvas-edge decision into
// an operator that does not need one: a feathered Select All would come back
// with its outer half silently removed, which is not what the user asked for
// and is not recoverable afterwards.
//
// A caller that wants canvas clipping composes it, in one line and visibly:
//
//     combineSelections(featherSelection(s, r), selectAll(w, h),
//                       SelectionCombine::Intersect)
//
// ==========================================================================
// Cost
// ==========================================================================
//
// One float plane covering the input's tile extent plus `2 * radius` on each
// side, and one more of the same size inside `blurPlane()`. For a marquee
// spanning four tiles (256x256) at radius 8 that is 288x288 floats twice --
// 0.63 MiB, transient. The uint8 result is the usual 16 KiB per touched tile.
//
// Tiles whose feathered coverage quantises entirely to zero are **dropped**,
// not stored, which preserves core/SelectionMask's constructor invariant (no
// stored tile is entirely zero) and matters more here than elsewhere: the
// apron is 2 * radius, but coverage past about 1.6 * radius is below half of
// one uint8 step (1/510) and quantises to zero, so the outer ring of tiles the
// apron reaches into is dropped rather than stored as 16 KiB of "not
// selected". Measured: a 64x64 marquee living in one tile, feathered by 8,
// touches 9 tiles' worth of apron and stores 4.
//
// The accuracy this costs is exactly the store's own: a feather checked
// against a direct 2-D convolution of the same coverage field agrees to
// 1.957e-03, against a uint8 half-step of 1.961e-03. In other words the
// operator is exact and the uint8 grid is the entire error -- which is the
// same relationship ops/Blur has with its f16 store, and the reason
// core/SelectionMask's choice of 8 bits is the thing to argue with if this is
// ever not enough.
namespace np {

// The sigma this file feathers with, for a given radius. Exposed so that a UI
// preview and --selftest use the same mapping the operator does rather than a
// retyped copy of `radius / 2`.
float featherSigmaForRadius(float radius) noexcept;

// The full blur request, including the kind. Exposed for the same reason, and
// because `blurApron(featherBlurParams(r))` is how a caller finds out how far
// a feather will reach before paying for it.
BlurParams featherBlurParams(float radius) noexcept;

// PRD E4's feather: `selection`'s coverage, Gaussian-blurred.
//
// `radius` is in document texels. Zero, negative and non-finite radii return
// the selection unchanged -- a copy, which under core/TileStore's
// copy-on-write is a refcount per tile rather than the bytes, so "feather by
// 0" is genuinely free rather than merely fast.
//
// A selection that selects nothing feathers to a selection that selects
// nothing (not to "no selection", which means the opposite -- see
// core/SelectionMask.hpp). Blurring zero is zero, and the identity holds
// without a special case; it is asserted anyway, because that is the pair of
// states this store's whole hazard lives in.
//
// The result's coverage may extend outside the input's tiles and outside the
// canvas. See "Decision 3" above for why that is this function's answer and
// not its caller's problem to have prevented.
Selection featherSelection(const Selection& selection, float radius);

}  // namespace np
