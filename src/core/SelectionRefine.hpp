#pragma once

#include <array>
#include <cstdint>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/FloodFill.hpp"

// core/SelectionRefine -- PRD E8 (grow and shrink through a distance
// transform) and PRD E9 (selection from colour range and from luminance
// range).
//
// Two requirements in one file because they are the two halves of "refine the
// selection I already have": E8 moves an existing boundary by a real number of
// texels, E9 builds a boundary out of the picture's own colour. Both hand back
// a `np::Selection`, so both compose through `core/SelectionOps`'
// `combineSelections()` and neither needs a bespoke combine rule.
//
// ==========================================================================
// Where this file sits, and the layering debt it takes on
// ==========================================================================
//
// `ops/Feather.hpp` argues at length that a selection operator built on the
// filter library belongs in `ops/`, because "core/ is the domain model and
// knows nothing about ops/". That argument is right and this file breaks it:
// §3 below reuses `ops/FloodFill`'s tolerance metric and `ops/PointOps`'
// luma, so `core/` gains an edge to `ops/`.
//
// It is taken deliberately and it is not a new edge -- `core/Composite.hpp`
// and `core/OpStack.hpp` already include `ops/PointOps.hpp`, for the same
// reason: the alternative is a second copy of a shared definition. The
// tiebreaker here is that the thing being reused is not arithmetic but a
// **decision about what "similar colour" means**, and `ops/FloodFill.hpp`'s
// own header names two disagreeing tolerance implementations as the failure it
// exists to prevent. A layering violation is visible in one `#include`; a
// second tolerance metric is invisible until a user notices that Colour Range
// and the magic wand select different pixels from the same click.
//
// What the honest alternative would have been: move the metric down into
// `core/` (it depends only on `color/Space`, which `core/` already uses) and
// leave `ops/FloodFill` as the traversal. That is the right shape and it is a
// change to a file this track does not own.
//
// ==========================================================================
// 1. Grow and shrink: why a distance transform and not iterated dilation
// ==========================================================================
//
// PRD E8 is unusually prescriptive -- "grow and shrink go through a distance
// transform, so the radius is a real number and antialiasing survives" -- and
// both clauses are consequences of the same fact: **dilation by a structuring
// element can only move an edge by whole texels, and can only decide a texel
// is in or out.**
//
// Iterated 4- or 8-neighbour dilation fails PRD E8 twice over. The radius
// quantises: three passes is three texels and there is no way to spell 2.5.
// And the coverage collapses, because the dilation of an antialiased edge has
// to answer "is this 0.3-covered texel a member?" with yes or no -- so a
// selection whose edge carried 256 levels comes back carrying two. That is not
// a subtle regression: it is PRD E2 ("selections store antialiased coverage,
// not a bitmask") being undone by the operator next to it. Measured in
// `--selftest`: growing an antialiased edge by zero through a *binary*
// distance field returns the boundary texel at coverage 0.0 where the input
// held 0.3, while the field this file builds returns it unchanged.
//
// The distance-transform formulation has neither problem, because it converts
// the selection into a real-valued field, adds a real number to it, and
// converts back:
//
//     phi(p)   = signed distance from texel centre p to the selection's edge,
//                positive inside
//     phi'(p)  = phi(p) + radius              (grow; shrink is radius < 0)
//     c'(p)    = clamp(phi'(p) + 0.5, 0, 1)
//
// The last line is the inverse of the first, and the pair is the whole reason
// antialiasing survives -- see §2.
//
// --- Felzenszwalb-Huttenlocher, and what was rejected --------------------
//
// The distance itself is the **exact** Euclidean distance to the seed set,
// computed by Felzenszwalb & Huttenlocher's two-pass squared-distance
// algorithm: a 1-D lower envelope of parabolas per column, then per row, O(n)
// per axis with no dependence on the radius.
//
// **Rejected: a chamfer / Borgefors approximation** (3-4 or 5-7-11 weights),
// the usual cheap stand-in. It is not cheaper here -- FH is already linear --
// and its error is *anisotropic*, which is the disqualifying part. Measured
// rather than cited, by chamfering a single seed and comparing every offset
// out to radius 100 against the true Euclidean distance: the 3-4 chamfer runs
// **+5.41 % long** near 18 degrees off an axis and **-5.72 % short** on the
// 45-degree diagonal, an 11.1 % spread. At a grow radius of 26.5 that is
// **2.949 texels** of contour position depending only on which way the edge
// happens to point -- a visibly octagonal blob. This file's exact transform
// lands the same contour to 0.0262 texels (§2), a factor of 112.
//
// An operator whose answer depends on the *direction* of the edge is exactly
// what the exact-area work in `core/SelectionShapes` was done to avoid.
//
// **Rejected: the fast marching method**, which would solve `|grad phi| = 1`
// directly and would in principle propagate the sub-texel offsets of §2
// exactly rather than approximately. It is O(n log n), it is a great deal more
// code, and its own accuracy on a straight edge is *worse* than an exact EDT's
// (it is a first-order upwind scheme; the EDT is exact by construction). It
// wins only where the sub-texel correction below is weakest, and the size of
// that weakness is measured rather than assumed -- see §2.
//
// ==========================================================================
// 2. The part that is not standard: keeping the antialiasing
// ==========================================================================
//
// A distance transform takes a **set**. A selection is not a set, it is a
// coverage field, and the naive move -- threshold at 0.5, transform, threshold
// back -- throws the antialiasing away before the transform ever runs. So the
// sub-texel position of the edge has to be carried into the seeding.
//
// **The coverage/distance relation.** For a straight edge crossing a texel,
// the covered area is exactly `clamp(t + 0.5, 0, 1)` where `t` is the signed
// distance from the texel centre to the edge. Inverted: a texel with
// **fractional** coverage `c` sits at signed distance `c - 0.5` from the edge,
// exactly. That is `selectionSignedDistanceFromCoverage()` below, and its
// inverse is `selectionCoverageFromSignedDistance()`. The two are declared
// together and named as inverses because the identity `grow(s, 0) == s` is
// *entirely* the statement that they are -- and that identity is the sharpest
// test this file has.
//
// A texel with saturated coverage (0 or 1) carries no sub-texel information at
// all; it only says the edge is at least half a texel away. So the seeding
// rule is:
//
//   * a texel with `0 < c < 1` **is a seed**, with offset `c - 0.5`;
//   * a texel with `c` saturated is a seed **only if some 4-neighbour is
//     saturated the other way** -- a genuine hard edge, whose contour lies on
//     the shared texel boundary, offset +/-0.5.
//
// The second clause's exclusion is the load-bearing half. A fully-covered
// texel next to a 0.3-covered one must NOT seed at +0.5: the true edge is 0.8
// away, and a +0.5 seed would win the minimum and pull the whole grown
// boundary half a texel inward. The fractional neighbour already knows the
// exact answer, so the saturated texel is required to keep quiet.
//
// **Propagation.** With offsets on the seeds, the field wanted is
// `min over seeds q of (|p - q| + offset(q))`, which is a lower envelope of
// *cones* and is not separable -- FH computes an envelope of parabolas. So the
// EDT is run unweighted, carrying its **feature transform** (the argmin, which
// FH's 1-D pass already knows: it is the surviving parabola's index), and the
// offset of the winning seed is added afterwards:
//
//     phi(p) = sign(p) * |p - q*(p)| + offset(q*(p))
//
// That is exact only when the seed nearest in plain distance is also the one
// minimising `|p-q| + offset(q)`, and **it frequently is not**. On a curved
// boundary the nearest seed is routinely one whose offset points the wrong
// way, while a seed a texel further along the contour with the opposite offset
// gives the better answer; measured on a disc of radius 20 grown by 6.5, the
// uncorrected field sits 0.24995 of coverage -- a quarter of a texel of contour
// position -- from the cone minimum, and it reads as a wobble.
//
// So the feature transform's answer is a **starting point**, and the true cone
// minimum is then taken over the seeds within three texels of it. Three is
// measured, not chosen: against a brute-force cone minimum over every seed, a
// window of 1 leaves up to 0.149 of coverage, a window of 2 up to 0.0194, and a
// window of 3 leaves 0.00196 -- which is 1/510, the uint8 store's own half-step
// -- at every grow radius from 2.5 to 30.5. Windows of 4, 6 and 8 are identical
// to 3. The implementation carries the table. The residual is therefore not a
// distance error at all any more; it is the store rounding.
//
// The refinement is confined to texels within 1.5 of the new contour, since
// coverage saturates outside that and the correction can move `phi` by at most
// 1.0. On the disc measured above that is 2.5 % of the plane.
//
// **Rejected: displacing each seed off-grid** to `q + offset(q) * n(q)` and
// running a vector distance transform (Gustavson & Strand's anti-aliased EDT).
// It removes the approximation above and is better on curved edges. It also
// needs a surface normal `n(q)`, estimated from the gradient of a coverage
// field that is quantised to 1/255 -- so on the shallow-gradient edges where
// it would help most, the normal is worst known. Deferred rather than
// dismissed: it changes the seeding and nothing else in this file.
//
// --- What the numbers are -------------------------------------------------
//
// All measured through the shipped functions, reading back through the uint8
// store, against a uint8 half-step of 1/510 = 1.961e-03:
//
//   * `grow(s, 0)` on an antialiased edge is an identity to **0.0 uint8
//     steps** -- bit-exact over a 30x30 window, because the boundary texel is
//     a seed with distance zero and the two conversions above are inverses.
//     The same edge put through a *binary* transform instead -- thresholded at
//     0.5 first, which is what a set-based operator must do -- comes back at
//     **0.000000** where the input held **0.301961**. That is the whole of PRD
//     E8's "antialiasing survives", in two numbers.
//   * A **fractional radius produces fractional coverage**: a hard-edged
//     marquee grown by 2.25 / 2.5 / 2.75 puts its new boundary texel at
//     **0.25098 / 0.50196 / 0.74902** -- the uint8 quantisations of 0.25, 0.5
//     and 0.75, which is the covered area of a texel the new contour crosses at
//     those offsets. Growing by an integer 2 or 3 gives 1.0 and 0.0 either
//     side: a hard edge stays hard, which is the control proving the
//     fractional answers are a moved edge and not a blur. Shrink is symmetric
//     at the same three values.
//   * **Accuracy and isotropy**, the claim the chamfer rejection rests on: a
//     disc of radius 20 grown by 6.5, against `selectEllipse()`'s analytic disc
//     of radius 26.5, gives a mean absolute coverage error of **0.00294** over
//     the 2293 affected texels and a maximum of **0.10980**. In aggregate --
//     the measure that exposes a directional bias, since an octagon and a
//     circle of the same 0.5-contour differ in area -- the grown selection's
//     area-implied radius is **26.5262** against the analytic **26.5001**, an
//     error of **0.0262 texels**. Split by direction -- the axis a chamfer
//     fails along -- the worst error within 10 degrees of an axis is
//     **0.02353** and within 10 degrees of a diagonal **0.07059**, so the two
//     directions a weighted-neighbour metric cannot keep together are within
//     0.05 of each other here. The 0.10980 maximum sits neither on an axis nor
//     on a diagonal (about 18 degrees off an axis): it is the straight-edge
//     coverage model meeting a curved boundary, not a direction-dependent
//     stretch, and it is 3.6x smaller than what the same disc costs a 3-4
//     chamfer in *radius* alone.
//   * The cone-minimum approximation, after the refinement above:
//     **0.00196** of coverage -- one uint8 half-step -- on a hard rectangle, on
//     an antialiased rectangle, and on the disc alike.
//
// Cost, on the same shapes: a disc of radius 20 grown by 6.5 takes **0.9 ms**
// and grown by 30.5 takes **1.4 ms**; a disc of radius 400 grown by 20 takes
// **18.1 ms** (44 tiles in, 52 out). One-shot, user-initiated, and not on any
// frame path.
//
// ==========================================================================
// 3. Colour range and luminance range (PRD E9)
// ==========================================================================
//
// Both are **whole-layer predicate passes with no connectivity** -- the
// difference from the magic wand is not the metric, it is that the wand walks
// and these do not. `ops/FloodFill.hpp` §3 already makes that distinction and
// already implements the non-walking half as `FloodFillReach::Global`.
//
// **The tolerance metric is not re-decided here.** `ops/FloodFill.hpp` §1
// settled it -- a display-encoded (sRGB) Chebyshev distance, after measuring
// an 18.1x asymmetry between a black seed and a white one in linear light --
// and `selectColourRange()` calls `floodFillDistance()` and
// `floodFillCoverage()` rather than restating any of it. `--selftest` asserts
// the consequence directly: colour range fed the colour under a texel returns
// **the identical selection** to a Global flood fill seeded on that texel,
// texel for texel. That assertion is the point of the whole arrangement; if
// someone ever writes a second metric here, it fails.
//
// What is genuinely different, and the only reason this is a separate entry
// point: **it takes a colour, not a seed coordinate**. Photoshop's Colour
// Range is driven by an eyedropper *and* by a swatch, and a function that can
// only be given a texel cannot serve the swatch. The colour is **straight**
// (un-premultiplied) linear RGBA -- what `core/Probe`'s `ProbeSample` holds
// and what `fillThroughSelection()` takes -- and is premultiplied here, at the
// one boundary, rather than at every call site.
//
// --- Luminance range: which luma, and in which domain ---------------------
//
// The weights are `ops/PointOps`' `computeLuma()` with its default
// `kRec709LumaWeights`, not a new set. That file already records why those
// three literals are what they are -- they are the exact constants
// `shaders/grayscale_blit.wgsl` hardcodes -- and a fourth copy of Rec.709
// would be a fourth thing to keep in step.
//
// **The weights are applied in linear light and the scalar result is encoded
// afterwards.** That order is a decision, and the alternative -- encode each
// channel, then weight -- is the video-engineering quantity Y', which is a
// different number. Two reasons for this order:
//
//   1. Luminance is defined as a weighted sum of *linear* light. Applying
//      those weights to display-encoded values is not an approximation of
//      luminance, it is a different function that happens to be close.
//   2. `computeLuma()`'s existing callers (saturation, grayscale) feed it
//      linear values, so weighting encoded ones here would mean this file's
//      "luminance" disagreeing with the grayscale operator's on the same
//      pixel.
//
// The **range endpoints are display-encoded**, for `ops/FloodFill.hpp` §1's
// reason unchanged: a range typed as 0.75..1.0 must mean "the top quarter of
// what I can see", and in linear light it would mean the top 3 %. Encoding one
// scalar is also cheaper than encoding three channels, which is the only place
// this operator is cheaper than colour range.
//
// --- Alpha ----------------------------------------------------------------
//
// Colour range gets alpha for free: it is the fourth term of
// `floodFillDistance()`'s Chebyshev max, so a transparent texel is far from an
// opaque one and blank canvas is not "black".
//
// Luminance range has no such term -- the user specified a luma band and
// nothing about opacity -- and ignoring alpha would be the bug
// `ops/FloodFill.hpp` names: an unwritten texel un-premultiplies to
// {0,0,0,0}, luminance 0, so "select the shadows" would select the whole empty
// canvas. So **coverage is multiplied by the texel's straight alpha**. A
// transparent texel is unselected whatever its nominal luma; a half-present
// one is half-selected, which is the same "half present, not half bright"
// rule premultiplied storage takes everywhere else in this codebase.
//
// **Rejected: a hard `alpha > 0` test.** It would make the antialiased outer
// texels of a shape fully selected, putting a bitmask edge on the output of an
// operator whose entire job is producing coverage.
//
// ==========================================================================
// 4. Cost, and what is deliberately not done
// ==========================================================================
//
// Grow/shrink materialises a **dense plane** over the input's tile extent
// expanded by `ceil(radius) + 1` texels. Counted rather than estimated, the
// buffers alive at the peak are coverage (float), the seed costs (double), the
// seed offsets (float), the transform's squared distances (double) and its two
// int32 feature indices, plus the column pass's own double and int32 that the
// row pass is still reading, plus the signed distance (float): **48 bytes per
// texel**. For a 256x256 marquee grown by 8 that is 282x282 -- 3.8 MiB,
// transient. For a Select All on 4096x4096 it is roughly 816 MiB, and that
// case is charged rather than hidden. Two things it is not:
//
//   * It is **not tileable**. A shrink by 500 depends on texels 500 away, so a
//     per-tile pass with an apron is the same allocation wearing a loop. The
//     available saving is horizontal *banding* with an apron of `radius + 1`,
//     which trades peak memory for re-transforming the aprons; not done,
//     because the operator is a user-initiated one-shot and the 4K Select All
//     case grows a selection that is already everything.
//   * There is **no document clip**, matching `featherSelection()` and for its
//     stated reason: a grow is well defined on an unbounded field and a
//     complement is not. Coverage may appear outside the canvas. A caller that
//     wants it clipped composes
//     `combineSelections(grown, selectAll(w, h), SelectionCombine::Intersect)`.
//
// Colour and luminance range cost one pass over the document's occupied tiles
// -- or over **every** tile, when the target matches the implicit empty texel,
// which is the same density `ops/FloodFill`'s `globalSimilar()` charges and
// for the same reason: if transparent black is in range then so is every texel
// no tile was ever allocated for, and that really is the answer.
namespace np {

// --- PRD E8: grow and shrink ----------------------------------------------

// Coverage of a texel whose centre lies at signed distance `phi` from a
// straight edge, positive inside: `clamp(phi + 0.5, 0, 1)`.
//
// Exposed as a named pair with its inverse below because the operator's
// central identity -- `growSelection(s, 0)` returns `s` -- is nothing but the
// statement that these two compose to identity on `[0, 1]`. A selftest that
// re-typed `c - 0.5` would be checking its own arithmetic instead.
float selectionCoverageFromSignedDistance(float phi) noexcept;

// The inverse, `c - 0.5`. **Exact for a straight edge and meaningless when `c`
// is saturated** -- coverage 0 or 1 says only that the edge is at least half a
// texel away, in an unknown direction. §2's seeding rule turns on that
// distinction, so it is stated on the function rather than left to the caller
// to remember.
float selectionSignedDistanceFromCoverage(float coverage) noexcept;

// PRD E8's grow and shrink, as one operator: `radius` in document texels,
// **positive grows and negative shrinks**.
//
// One function rather than two because there is exactly one sign in the
// implementation -- `phi + radius` -- and splitting it would create two places
// for that sign to be wrong. `shrinkSelection()` below is the readable
// spelling of a negative radius and is one line.
//
// A radius of zero returns the selection unchanged **to within the uint8 store
// and not merely approximately**; a non-finite radius does the same rather
// than producing a field of NaNs. An empty selection stays empty in both
// directions: there is no edge to move, and "grow nothing" is nothing, not
// everything.
//
// The result is not clipped to any document -- §4 says why -- and may occupy
// tiles the input did not. Tiles whose grown coverage quantises entirely to
// zero are dropped, preserving `core/SelectionMask`'s constructor invariant.
Selection growSelection(const Selection& selection, float radius);

// `growSelection(selection, -radius)`. Present so that a call site reads the
// way the menu item does; a negative `radius` here therefore grows, which is
// the same double-negative every editor's Contract dialog has.
Selection shrinkSelection(const Selection& selection, float radius);

// --- PRD E9: colour range and luminance range -----------------------------

// The tolerance and its ramp, in `ops/FloodFill`'s units and with its
// defaults, because they are the same numbers meaning the same thing (§3).
//
// Deliberately NOT a `FloodFillParams`: that struct also carries a
// `FloodFillReach`, and colour range has no connectivity to choose. A
// parameter a caller can set and this operator must ignore is worse than a
// second struct.
struct SelectionRangeParams {
  // Maximum display-encoded per-channel difference, Chebyshev over RGB plus
  // linear alpha. Photoshop's 32/255 by default, for the reason
  // `ops/FloodFill.hpp` gives: the number should mean what a user arriving
  // from another editor already believes it means.
  float tolerance = kFloodDefaultTolerance;

  // Width of the linear falloff at the outer edge, same units. Clamped to
  // `tolerance` internally. Zero gives a hard in/out answer.
  float edgeBand = kFloodDefaultEdgeBand;
};

// PRD E9's colour range: every texel of `source` within `params.tolerance` of
// `targetStraightLinearRgba`, connected or not.
//
// The colour is **straight** (un-premultiplied) linear RGBA -- a
// `core::ProbeSample`'s `rgba`, or a picker's -- and is premultiplied here.
// Passing a premultiplied colour by mistake selects a darker band than
// intended rather than failing, which is why the convention is named on the
// parameter as well as in §3.
//
// `width`/`height` bound the document: texels outside are never selected,
// including texels that physically exist inside an edge tile. A non-positive
// size gives a selection with no tiles -- engaged and empty, never the
// unrestricted state.
//
// Fed the colour under a texel, this returns exactly what
// `floodFillSelection()` with `FloodFillReach::Global` returns for that seed.
// That is asserted, not assumed.
Selection selectColourRange(const TileStore& source,
                            const std::array<float, 4>& targetStraightLinearRgba,
                            int32_t width, int32_t height,
                            const SelectionRangeParams& params = {});

// PRD E9's luminance range: a band, not a tolerance around a sample.
//
// `low` and `high` are **display-encoded** Rec.709 luminance in [0, 1] -- the
// domain §3 argues for, and the domain `selectionLuminanceOf()` returns.
// `low > high` selects nothing (an empty band is empty, not inverted).
struct SelectionLuminanceRange {
  float low = 0.0f;
  float high = 1.0f;

  // Linear falloff outside each end of the band, in the same encoded units.
  // Shares `ops/FloodFill`'s derived floor, which is the narrowest ramp that
  // can reach all 256 coverage levels from an rgba16float source -- the
  // derivation is in that header and is about the two grids, not about
  // colour, so it transfers unchanged.
  float edgeBand = kFloodDefaultEdgeBand;
};

// Display-encoded Rec.709 luminance of one **premultiplied** texel: the
// un-premultiply guard, then `ops/PointOps`' `computeLuma()` on linear RGB,
// then one `srgbEncode()` of the scalar. §3 argues that order.
//
// Exposed so a UI readout, this operator and `--selftest` share one
// definition; a retyped dot product is how the three would drift.
float selectionLuminanceOf(const std::array<float, 4>& premultiplied) noexcept;

// Every texel of `source` whose luminance falls in `range`, connected or not,
// **weighted by the texel's straight alpha** so that empty canvas is not a
// shadow (§3). Same bounds and same empty-result rules as
// `selectColourRange()`.
Selection selectLuminanceRange(const TileStore& source, int32_t width, int32_t height,
                               const SelectionLuminanceRange& range = {});

}  // namespace np
