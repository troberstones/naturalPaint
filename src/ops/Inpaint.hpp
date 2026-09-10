#pragma once

#include <cstdint>
#include <optional>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/Inpaint -- PLAN.md "Phase 8 -- Repair it": "**diffusion inpaint
// (Telea)**", the first half of PRD D7 ("Inpaint: diffusion for scratches and
// dust; PatchMatch for textured regions", P1).
//
// The user paints a selection over a scratch, a dust speck or a boom mic and
// asks for it to go away. What comes back is a smooth continuation of what
// surrounded it, propagated inward from the hole's rim along the level sets of
// the distance to that rim -- A. Telea, "An Image Inpainting Technique Based
// on the Fast Marching Method", Journal of Graphics Tools 9(1), 2004.
//
// **PatchMatch is not here and is not a follow-on paragraph of this file.**
// D7's second half is a texture-synthesis op of an entirely different
// evaluation class -- PLAN.md phase 9 puts it with make-tileable, "as a cached
// class-D op with a Recompute button and a deterministic seed", because it is
// too slow to be a live pass and its answer is not a function of its inputs
// alone. Diffusion is a class-B/C spatial op like everything else in ops/, and
// the two share nothing but a menu neighbourhood. A hole with real texture in
// it will come back from this file smooth and slightly blurred; that is the
// honest limit of diffusion inpainting and the reason the other half exists.
//
// ==========================================================================
// 1. The selection is the HOLE, not a bound. This is the whole file.
// ==========================================================================
//
// Every other menu pixel op in this build treats the selection as a **bound**:
// run the engine over the canvas, then blend the result back wherever coverage
// is non-zero (app/FilterOps.hpp, "why the selection is honoured by
// COMPOSITING"). The engine itself never sees a `Selection` and does not need
// to -- a blur of a texel inside the selection reads that texel's own
// neighbourhood, selected or not.
//
// Inpaint inverts that. The selected texels are exactly the ones whose data is
// **wrong and must not be read**; the answer comes from *outside* the
// selection. So this is the one engine in ops/ that takes a `Selection` as a
// parameter rather than letting the bridge apply it afterwards, and
// `InpaintParams::hole` is that parameter.
//
// Two consequences, both of which are the kind of thing that produces a
// plausible-looking wrong image rather than a crash:
//
//   **`hole == nullptr` means "there is nothing to fill", NOT "the whole
//   canvas".** core/SelectionMask.hpp's central rule is that a null
//   `Selection*` means *no restriction* -- coverage 1.0 everywhere -- and
//   every other reader in this codebase is right to read it that way. Read
//   that way here it would mean "every texel in the document is a hole",
//   i.e. erase the layer and guess it back from the transparent surround.
//   So a null hole is a **refusal**, by name, and `inpaintParamsValid()`
//   says so. The same goes for an engaged selection that selects nothing
//   (`selectionSelectsNothing()`): there is no hole, so there is nothing to
//   do, and this file refuses rather than reporting a successful no-op.
//
//   **The engine's hole and the bridge's composite must be the SAME
//   selection.** `app/PixelOpBridge.hpp` blends this file's output back
//   through `doc.selection`; this file decides what to fill from
//   `params.hole`. If those two ever disagree -- a stale copy, a selection
//   edited between the preview and the commit -- the parts of the hole the
//   composite covers and the parts the engine filled stop lining up, and the
//   visible result is a *partly* repaired scratch, which reads as "the filter
//   is weak" rather than as "the wiring is wrong". `app/FilterOps.hpp`'s
//   `applyInpaint()`/`previewInpaint()` take a radius and nothing else, and
//   build `InpaintParams::hole` from `doc.selection` themselves, precisely so
//   there is no caller-side opportunity for the two to differ.
//
// **A texel is in the hole when its coverage is strictly greater than zero**,
// not when it is above some threshold. A feathered marquee's half-covered rim
// texel is half scratch, so its data is half wrong, so it is not source data
// -- it is filled like the rest of the hole and then the bridge's composite
// blends the fill back at that texel's own coverage, which is the graded edge
// the user asked for. Choosing `>= 0.5` instead would let the outer half of a
// feathered rim vote on the fill with exactly the pixels the user was pointing
// at, which is the failure mode this rule exists to avoid.
//
// ==========================================================================
// 2. The domain: linear light, premultiplied alpha, and a convex combination
// ==========================================================================
//
// ops/Blur.hpp's domain section applies here word for word and is not repeated
// -- a fill is a weighted average, so it runs on core/TileStore's storage as
// it is, **linear premultiplied rgba16float**, with no transfer function in
// either direction, and all four channels are averaged with one set of weights
// so RGB and A stay consistent with each other.
//
// What is worth adding is the property that follows from it. Every weight
// below is **strictly positive** and the result is divided by their sum, so a
// filled texel is a genuine **convex combination** of the texels it read.
// Inductively -- a filled texel becomes source data for the ones filled after
// it -- every value this file writes lies inside the per-channel range of the
// original known texels. That is not decoration:
//
//   - The premultiplied invariant `0 <= RGB <= A` is a set of linear
//     inequalities, so it is preserved exactly by a convex combination. An
//     inpaint cannot invent a texel that is more coloured than it is present,
//     which is the fringe every "just extrapolate the gradient" fill produces
//     at a soft edge.
//   - A flat field comes back flat. A hole in a constant colour fills with
//     that colour, because a convex combination of equal values is that value.
//   - There is no overshoot to clamp, so there is no clamp to hide a bug
//     behind. --selftest asserts the hull property directly, over every filled
//     texel of a two-tone fixture.
//
// **What is given up for it, named rather than hidden: Telea's gradient
// extrapolation term.** The paper's estimate is
// `I(p) = sum_q w(p,q) [ I(q) + grad I(q).(p-q) ] / sum_q w(p,q)`, and the
// bracketed first-order term is what lets a fill continue a ramp across a wide
// hole instead of flattening toward its rim's mean. It is deliberately absent
// here, because it is exactly the term that leaves the convex hull: on a
// premultiplied store an extrapolated RGB can exceed the extrapolated A, and
// the standard repair -- clamp the result -- silently discards the
// extrapolation the term was added for while leaving the code that computes it
// in place. So this file implements the weighted average and says so, rather
// than implementing the extrapolation and then neutering it. The named
// follow-up is a gradient term computed on **un-premultiplied** colour with
// alpha extrapolated separately, which is a different function with a
// different test, not an `if` inside this one.
//
// ==========================================================================
// 3. The algorithm: fast marching, boundary inward
// ==========================================================================
//
// Every texel is KNOWN (outside the hole), BAND (on the moving front) or
// UNKNOWN (hole, not yet filled). `T` is the distance from the hole's rim,
// computed by solving the eikonal equation `|grad T| = 1` with Sethian's fast
// marching method: a min-heap of front texels, and each pop finalises the
// texel with the smallest remaining `T` -- so texels are filled strictly in
// order of their distance from known data, which is what makes the result
// independent of scan order.
//
// `T` is solved at a texel from its two already-finalised orthogonal
// neighbours by the standard upwind quadratic (Telea eq. 4-5, four quadrants,
// smallest root taken); with only one finalised neighbour it degrades to
// `1 + T(neighbour)`, which is the correct one-sided answer and not a
// fallback.
//
// When a texel joins the band it is filled, once, from the texels within
// radius `eps` of it that are already KNOWN or BAND -- i.e. outside the hole
// or already filled -- with the paper's three weights multiplied together:
//
//     dir(p,q) = |(p-q) . N(p)| / |p-q|     N(p) = grad T(p), normalised
//     dst(p,q) = 1 / |p-q|^2
//     lev(p,q) = 1 / (1 + |T(p) - T(q)|)
//
// `dir` is the one that makes this an inpaint rather than a blur: it weights
// neighbours lying along the normal to the front -- the direction information
// is *arriving* from -- above neighbours lying along it, so a structure
// meeting the hole's rim continues into the hole instead of being averaged
// with everything else on the rim. `dst` is the usual inverse-square falloff.
// `lev` keeps the estimate close to the front's own level set.
//
// **These are the paper's forms, and they differ from OpenCV's widely-copied
// `INPAINT_TELEA` in two places** -- named because "matches the reference
// implementation" and "matches the published algorithm" are not the same
// claim, and the second one is the one this file makes. OpenCV uses
// `dst = 1/|p-q|^3` (its `VectorLength` returns the *squared* length, so its
// `1/(len*sqrt(len))` is an inverse cube) and leaves `dir` un-normalised by
// `|p-q|`, so its `dir` is a raw dot product that grows with distance rather
// than a cosine in [0,1]. Both are self-consistent and neither is what the
// paper writes.
//
// **`T` is signed.** `lev` compares `T(p)` inside the hole against `T(q)`,
// and for a `q` outside the hole a plain `T(q) = 0` would make every outside
// neighbour sit on the same level set no matter how far out it is -- which
// turns `lev` into a constant that cancels in the normalisation and quietly
// removes a third of the weighting. So the same fast march is run a second
// time with the roles swapped, outward from the rim, and outside texels carry
// **negative** `T`. Two marches, one solver.
//
// Two degenerate cases, handled rather than hoped away. `grad T` is exactly
// zero at a texel whose finalised neighbours all sit on one level set; there
// is no normal to weight by, so `dir` falls back to 1 (isotropic) rather than
// to zero, which would give the texel no source at all. And `dir` for a
// neighbour exactly perpendicular to the normal is zero, which would delete a
// legitimate source texel from the average, so it is floored at a small
// positive constant -- the same guard OpenCV applies for the same reason.
// Together these are what makes "the weight sum is strictly positive"
// (section 2's convexity argument) true rather than usually true.
//
// ==========================================================================
// 4. Why there is no `inpaintRoiOp()`
// ==========================================================================
//
// Every other spatial op here declares its region of interest as an ops/Roi
// `RoiOp` -- a fixed dilation, so a caller can ask "what source does this
// output rectangle need" and get an answer that does not depend on the
// pixels. `blurRoiOp()` is a dilation by the kernel's apron and that is the
// end of it.
//
// Inpaint has no such answer, and pretending otherwise is the tile-seam bug of
// ops/Blur.hpp wearing a new hat. The value at a hole texel depends on the
// texels at the rim of **its whole connected hole component**, which is a
// property of the selection and not of any margin: a one-texel hole needs a
// few texels around it, and a scratch 900 texels long needs data 900 texels
// away at the other end of it. A `RoiOp` cannot say that, so this file does
// not claim one.
//
// What it does instead is refuse to be split. `inpaintTiles()` requires
// `outRect` to **contain the entire hole** and returns false otherwise. That
// is a real restriction -- inpaint cannot be evaluated tile by tile the way a
// blur can, and a future tiled evaluator would have to work per hole component
// rather than per tile -- and it is stated as a refusal instead of being left
// as a trap, because the alternative is a function that returns a different
// answer depending on how the caller happened to carve up its request. The
// menu path passes the whole canvas rectangle (app/FilterOps.hpp says why that
// is the right rectangle for every op here, not merely the simple one), so it
// satisfies the requirement by construction.
//
// ==========================================================================
// 5. What it writes, and what it costs
// ==========================================================================
//
// **`inpaintTiles()` writes the hole and nothing else.** Unlike `blurTiles()`,
// which fills every tile of its output rectangle because a blur changes every
// texel it touches, an inpaint by definition changes nothing outside the hole,
// so allocating a canvas of tiles to copy them back unchanged would be pure
// waste. Tiles the hole does not reach are left absent in `dst`, which is
// exactly the case `compositeFilterResult()` already skips ("the engine wrote
// nothing at this tile at all"), and texels of a straddling tile that are
// outside the hole are left as `dst` already had them.
//
// The scratch is sized by the hole's **bounding box** dilated by `eps + 1`,
// not by the document: for each texel of that rectangle, 16 bytes of gathered
// RGBA, two floats of `T` (inward and outward) and three bytes of flags --
// 27 bytes per texel.
//
//     a 400 x 3 scratch, eps = 5      412 x 15       167 KiB
//     a 512 x 512 blemish, eps = 5    524 x 524      7.4 MiB
//     a whole 4096 x 4096 canvas      4108 x 4108    458 MiB
//
// The last row is the "Select All, then Inpaint" case, and it is a real cost
// rather than a theoretical one. It is left as a cost rather than a refusal
// because it is not a distinct failure: the hole's rim then lies outside the
// painted region, where core/TileStore has no tiles, which is transparent
// black under premultiplied alpha -- so a fully-selected layer inpaints toward
// transparent, fading rather than blackening, exactly as ops/Blur.hpp's edge
// behaviour does and for the identical reason. Filling a whole layer from its
// own emptiness is a strange thing to ask for, but it is not an inconsistent
// one, and special-casing it would put a second definition of "what is outside
// the hole" into a file whose entire subject is that question.
//
// Time is `O(N log N)` in the hole's texel count for the two marches, plus
// `(2*eps+1)^2` taps for each filled texel -- 121 at the default radius. A
// scratch is a few thousand texels, so this is a millisecond-scale op on the
// selections it is for, and a whole-canvas hole is neither its use case nor
// its budget.
namespace np {

// The largest `radius` a request may carry. The per-texel cost is
// `(2r+1)^2` taps, so this cap is 16 641 of them -- past the point where a
// diffusion fill is the wrong tool rather than a slow one, and far enough
// above any sane dial that it exists to bound a typo rather than to express
// a preference.
inline constexpr int32_t kInpaintMaxRadius = 64;

struct InpaintParams {
  // **The hole to fill, not a bound on where the result lands.** See section 1
  // -- `nullptr` means there is no hole and the request is refused, which is
  // the exact inverse of what a null `Selection*` means everywhere else.
  //
  // A borrowed pointer, valid for the duration of the call and not retained.
  const Selection* hole = nullptr;

  // Telea's `eps`: how far from a texel being filled its sources may lie, in
  // texels. Larger is smoother and slower; smaller follows the rim's detail
  // more closely and is more prone to streaking along the front's normals.
  //
  // 5 rather than OpenCV's 3, because the ops here run on a document-resolution
  // scratch rather than a thumbnail and because `dst`'s inverse-square falloff
  // already makes the far end of the window contribute little -- the widened
  // window buys stability at the rim for a cost the falloff has mostly already
  // discounted.
  int32_t radius = 5;
};

// False for a request nothing can be filled from: no hole at all
// (`hole == nullptr`, section 1's inverted default), a hole that selects
// nothing anywhere, or a radius outside `[1, kInpaintMaxRadius]`.
//
// Refused **by name** rather than clamped or silently treated as a no-op, for
// ops/Blur.hpp's stated reason: a bad parameter arriving at a filter is a
// caller bug, and the empty-selection case in particular is a question a user
// is entitled to an answer to ("inpaint what?"). `app/FilterOps.hpp` turns
// this into `PixelOpRefusal::NoSelection` before the engine is asked anything.
bool inpaintParamsValid(const InpaintParams& p) noexcept;

// The tightest document-texel rectangle containing every texel of the hole, or
// `std::nullopt` when there is no hole. This is the rectangle `outRect` must
// contain (section 4) and the one the scratch is sized from (section 5), so it
// is exposed rather than left internal: a caller that wants to know what an
// inpaint will cost, or a selftest that wants to check the containment rule
// without reimplementing it, asks here.
std::optional<PixelRect> inpaintHoleBounds(const InpaintParams& p);

// Fills `p.hole` in `src` and writes the result into `dst`.
//
// Reads only texels **outside** the hole (and texels of the hole it has
// already filled). Writes only texels **inside** the hole; every other texel
// of `dst`, and every tile the hole does not reach, is left exactly as it was
// -- see section 5.
//
// Tiles of `src` that do not exist read as transparent black, so a hole at the
// edge of the painted region fills toward transparent rather than toward
// black.
//
// Returns false and writes nothing when:
//   - `dst` is null, or `dst` is the same store as `src` (the march would read
//     texels it had already replaced -- ops/Blur.hpp's aliasing argument, and
//     here it would additionally make the fill depend on heap order);
//   - `inpaintParamsValid(p)` is false -- including the no-hole case;
//   - `outRect` is empty, or does not contain `inpaintHoleBounds(p)`
//     (section 4: this op cannot be evaluated in pieces);
//   - the hole's bounding box dilated by `radius + 1` would overflow ops/Roi's
//     coordinate limit.
bool inpaintTiles(const TileStore& src, const PixelRect& outRect, const InpaintParams& p,
                  TileStore* dst);

}  // namespace np
