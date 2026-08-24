#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"

// ops/FloodFill -- the magic wand (PRD E3) and the paint bucket (PRD D25),
// which are **one algorithm with two outputs** and are built here as one.
//
// PLAN.md separates them: the bucket is Phase 6 ("the tool-shaped ops that need
// the same fill machinery ... paint bucket with tolerance and fill-all-similar")
// and the wand is Phase 7 ("magic wand as a **CPU** flood fill paging through
// the tile store"). Building them in their own phases would have produced two
// implementations of "is this the same colour", in different files, written
// months apart -- and the failure mode of that is not a crash. It is a bucket
// that fills a slightly different region than the wand it was previewed with,
// which no test written against either one alone can see. So the traversal, the
// tolerance metric and the coverage ramp live here once, and the two tools are
// two call sites:
//
//   wand   -- floodFillSelection() -> Selection, installed on the Document
//             (combined through core/SelectionOps by the usual modifiers).
//   bucket -- floodFillSelection() -> Selection, immediately consumed by
//             fillThroughSelection(). There is no second pixel-writing path.
//
// ==========================================================================
// 1. What "similar colour" means, and why it is NOT measured in linear light
// ==========================================================================
//
// The working space is linear, premultiplied `rgba16float` (DESIGN-imaging.md
// §2). A tolerance is a single number the user types once and expects to mean
// the same thing everywhere in the picture, and **a tolerance measured on
// linear values does not**. Measured through the shipped
// `floodFillDistanceBetween()`, not asserted -- with the default tolerance
// below, the linear-light span this file accepts around a grey seed is:
//
//     seed linear 0.0000  ->  accepts linear [-0.014444, 0.014444]  width 0.0289
//     seed linear 0.1800  ->  accepts linear [ 0.092270, 0.303409]  width 0.2111
//     seed linear 0.5000  ->  accepts linear [ 0.330185, 0.712147]  width 0.3820
//     seed linear 1.0000  ->  accepts linear [ 0.737910, 1.309616]  width 0.5717
//
// a factor of **19.8** between the darkest seed and the lightest -- or **18.1x**
// comparing only the one-sided widths that stay inside [0, 1] (0.014444 above
// black against 0.262090 below white, which is the pair `--selftest` asserts,
// since it needs no HDR headroom to be reproducible). Run the other way -- fix
// a linear width and ask what it looks like -- a linear tolerance of 0.05 is
// **5.7** sRGB code values wide against a white seed and **63.2** against
// black. One number cannot be both. A user who set a tolerance that behaved on
// a sky would find it swallowing an entire shadow, and there is no setting that
// fixes it, because the problem is the domain and not the value.
//
// **So the distance is measured on display-encoded values**: each channel goes
// through `color/Space`'s `srgbEncode()` before the difference is taken. Three
// reasons, in order of weight:
//
//   1. It is the domain the user is looking at. `ui/CanvasQuad` presents the
//      canvas through exactly this curve, and `core/Probe`'s eyedropper reports
//      exactly this curve's output. The wand answers "do these two look the
//      same to me", and "look" is a display-referred question -- so it must be
//      asked in the display-referred domain, not the storage one.
//   2. It makes the tolerance number mean what every user already thinks it
//      means. Photoshop's tolerance is 0..255 on 8-bit display-encoded data;
//      `kFloodDefaultTolerance` below is literally its default, converted.
//   3. It is the same domain rule this codebase already applies elsewhere, and
//      for the same reason: PRD §D's callout that "**add noise must be applied
//      in the shaper domain, not linear** -- fixed-amplitude Gaussian noise in
//      linear light is invisible in shadow and enormous in highlight", and
//      ADR-0004's decision to author curves in a log domain. A fixed-amplitude
//      *tolerance* in linear light fails in exactly the same direction.
//
// **Rejected: the ACEScct shaper domain** (`color/Shaper.hpp`), which is the
// other non-linear domain already in this build and which would have handled
// HDR values above 1.0 more gracefully. It is the *grading* domain -- its job
// is to spread scene-linear headroom across a LUT's [0,1] index, and its toe is
// shaped for that, not for perceptual uniformity. Encoding to it would make the
// wand's tolerance disagree with the eyedropper and the screen, which is the
// one disagreement this decision exists to prevent. The cost of the rejection
// is stated below in §4.
//
// **Rejected: a perceptual difference formula (CIE dE76/dE2000, OkLab).** Those
// are genuinely better answers to "do these look the same", and if this build
// had an RGB<->XYZ matrix the argument would be live. It does not --
// `color/Space.hpp` says so outright ("*not* an RGB<->XYZ matrix: deriving one
// is a job for whichever later step actually needs to adapt primaries") -- so
// adopting one here would mean landing chromatic adaptation and a gamut policy
// as a side effect of building a paint bucket. Deferred, not dismissed: it
// changes `floodFillDistance()` alone, and nothing else in this file.
//
// --- Alpha is part of the distance, and is NOT encoded -------------------
//
// Two reasons alpha cannot be dropped. First, this store is premultiplied, so
// un-premultiplying a transparent texel gives {0,0,0,0} (core/Premultiply.hpp
// defines it that way deliberately) -- which is *indistinguishable from opaque
// black* on RGB alone. A wand that ignored alpha would treat the empty canvas
// and a black shape as the same colour. Second, a soft edge is a real boundary
// and the user expects the wand to stop at it.
//
// It is compared **linearly**, without a transfer function, because alpha is
// opacity and not light -- the identical policy `ops/PointOps.hpp` and
// `color/Space.hpp` already state for the transfer functions ("never see alpha,
// never touch it -- alpha is opacity, not light"). Pushing a blend weight
// through a curve designed for photons is a category error, not a refinement.
//
// --- Chebyshev, not Euclidean --------------------------------------------
//
// The distance is `max` over the four terms, not their root-sum-square. Under a
// Euclidean norm a tolerance of `t` admits a per-channel difference of `t` when
// only one channel moves but only `t/2` when all four do, so the number's
// meaning would depend on the *direction* of the colour difference -- and the
// user has no way to know which case they are in. `max` gives the number one
// meaning: **every channel is within the tolerance**, which is also what makes
// it comparable to Photoshop's.
//
// ==========================================================================
// 2. Antialiased coverage, and where the ramp width comes from
// ==========================================================================
//
// PRD E2 is P0 and says selections store "antialiased *coverage*, not a
// bitmask". A threshold on a distance is a bitmask, so the accepted band has a
// ramp at its outer edge: coverage is 1.0 out to `tolerance - edgeBand`, falls
// linearly to 0.0 at `tolerance`, and is 0.0 beyond.
//
// Note what the ramp does NOT do: it does not extend the region. A texel is
// reachable by the traversal exactly when its coverage is above zero, i.e.
// exactly when it is within `tolerance` -- so **the set of texels the fill
// reaches is identical whether `edgeBand` is zero or default**. The band
// weights the boundary; it never moves it. `--selftest` asserts that directly,
// because the alternative (a soft band that also grows the selection) would
// make the antialiasing setting silently change which region gets filled.
//
// One caveat on that, stated because it is the sort of thing found later and
// mistaken for a bug: the *stored* answer is uint8, so a coverage below
// 0.5/255 rounds to zero and the outermost ~0.2 % of the ramp comes back
// unselected even though the traversal walked it. That is the coverage store's
// resolution, not a boundary this file moved, and it is one part in five
// hundred of a band that is itself a fraction of the tolerance.
//
// `edgeBand == 0` is a legitimate setting and gives a hard in/out answer -- it
// is Photoshop's "Anti-alias" checkbox, unticked. PRD E2 is satisfied by the
// store being *able* to hold partial coverage and by the default producing it,
// not by forbidding a user from asking for hard edges.
//
// **Rejected: supersampling the source.** That is how `selectRectangle()` gets
// its antialiasing (it has an analytic rectangle and integrates the true
// covered area), and it is the better mechanism *when there is a shape to
// integrate*. Here there is not: the input is texels, and there is nothing
// between two of them to sample. Any "subtexel" answer would be an
// interpolation invented by this file and presented as measured coverage.
//
// --- kFloodDefaultEdgeBand is derived, not chosen ------------------------
//
// The band's width is bounded from below by a real quantisation, and the
// default sits exactly on that bound. Two grids are involved:
//
//   * the **source**, `rgba16float`. Sweeping every representable half in
//     [0, 1] and encoding each through `srgbEncode()`, the largest gap between
//     two *adjacent* representable values, in the encoded metric this file
//     measures distance in, is **3.21507e-4** -- occurring at linear 0.5, where
//     binary16's exponent steps from 2^-2 to 2^-1 and the spacing doubles to
//     2^-11 while the encode curve's slope has not yet flattened to match.
//   * the **destination**, `core/SelectionMask`'s uint8 coverage: 256 levels.
//
// A ramp narrower than 255 source steps therefore cannot reach all 256 coverage
// levels no matter what the picture contains -- it is a bitmask with a few
// extra rungs, wearing the word "antialiased". 255 x 3.21507e-4 = **0.0819844**,
// which is the constant below. Verified rather than merely derived: sweeping
// the half grid through the real ramp yields 255-256 distinct uint8 coverage
// levels at every seed luminance tried (0.0, 0.02, 0.18, 0.5, 0.75, 1.0), and
// the criterion is not vacuous -- at half this band a white seed yields 165
// levels, and at a tenth of it, 33. `--selftest` re-measures both ends so the
// derivation is checkable in the binary rather than only in this comment.
//
// This is a **floor**, and the honest statement of what it is not: it is not a
// claim that 0.0819844 is the prettiest edge. Anything wider is a taste
// question a UI slider may answer; anything narrower is provably claiming
// precision the pipeline cannot deliver. The default is the floor because a
// default should be the defensible end of a range, not a point in the middle of
// one that someone liked.
//
// ==========================================================================
// 3. Two reaches, one predicate -- and only one of them is a flood fill
// ==========================================================================
//
// PRD D25 asks for "contiguous fill with tolerance, **and** fill-all-similar",
// and those are not the same shape of computation:
//
//   Contiguous -- a real flood fill. Scanline/span based, 4-connected, paging
//     through the `TileStore` a tile at a time. Cost is proportional to the
//     region found, not to the document.
//   Global     -- not a traversal at all. It is a whole-document predicate
//     pass: every texel that satisfies the same test, connected or not. Cost is
//     proportional to the document (see the transparent-seed note in §4).
//
// They share `floodFillCoverage()` and nothing else. Sharing the *traversal*
// instead -- "global is a flood fill with connectivity switched off" -- would
// have meant a span walker running over a document-sized frontier to arrive at
// an answer a flat loop gives directly, and would have put the predicate behind
// a branch inside the hot path of the mode that does not need it.
//
// --- Why scanline, and why 4-connected -----------------------------------
//
// **Scanline, not per-texel 4-way recursion.** A recursive flood fill of a
// 4096x4096 region recurses up to sixteen million frames deep and dies on the
// stack before it dies on time; the explicit-stack per-texel variant survives
// but queues one entry per texel. The span form pushes one entry per *run*, so
// a solid region costs one entry per scanline rather than one per texel.
// `--selftest` floods a 400x400 region (160 000 texels) specifically to fail if
// this is ever rewritten recursively.
//
// **4-connected, not 8.** An 8-connected fill leaks through single-texel
// diagonal gaps -- the exact structure that a hand-drawn outline, a hairline
// vector stroke, or any diagonally-resampled edge is full of -- and the leak is
// invisible until the whole background floods. 4-connected is what Photoshop
// and GIMP both do, and the failure it produces instead (a diagonal hairline
// that has to be clicked on both sides) is the recoverable one.
//
// ==========================================================================
// 4. What this deliberately does not do
// ==========================================================================
//
//  * **It samples one `TileStore`, not a Document.** "Sample all layers" is a
//    real bucket/wand option, and the caller supplies it by flattening --
//    `io/Export.hpp`'s `flattenDocumentToLinear()` already exists for exactly
//    that shape of question. Taking a `Document` here would mean this file
//    owning a compositing decision that `core/Composite` already owns.
//  * **It does not sample a `PigmentTileStore`.** A pigment texel is a straight
//    Mixbox latent plus a mass (core/Pigment.hpp), and "similar colour" there
//    is a question about latents, not about sRGB-encoded RGB. Answering it by
//    projecting through `latentToRgb()` first would work and would also be a
//    silent, unstated choice about what similarity means in Kubelka-Munk space.
//    Named here rather than discovered by whoever wands a pigment layer.
//  * **It does not restrict itself to the existing selection.** Photoshop's
//    wand searches only inside the active selection when combining. Doing that
//    here would confuse two things: what the tool *found* and how it *combines*
//    -- and combining is `core/SelectionOps`' job, which already has the four
//    rules and the modifier table. A caller that wants the restriction
//    intersects afterwards.
//  * **A seed above 1.0 gets a coarser ramp than the derivation promises.** The
//    band was derived over the half grid in [0, 1]; above 1.0 the grid coarsens
//    (the largest encoded step over [0, 4] is 5.72801e-4, at linear 2.0) faster
//    than the encode curve flattens, so an HDR highlight yields fewer distinct
//    coverage levels than 256. Stated rather than fixed: widening the default
//    band to cover the HDR case would coarsen the ordinary case, which is
//    almost every case.
//  * **The distance costs three `pow()` calls per candidate texel.** The seed's
//    three encodes are hoisted into `FloodFillReference`; the candidate's are
//    not, because it is different every time. This is a user-initiated one-shot
//    op and not a per-frame cost, so it is not optimised speculatively. The
//    optimisation available if a profile ever asks for one is a 65 536-entry
//    table over the half bit patterns -- and the reason it is not already here
//    is that un-premultiplying moves the value off the half grid, so the table
//    would only serve the fully-opaque case.
namespace np {

// Photoshop's default wand/bucket tolerance, in the units Photoshop states it
// in: 32 out of 255, on display-encoded data. It is quoted rather than invented
// precisely because §1's whole argument is that the number should mean what a
// user coming from another editor already believes it means -- picking a
// different default would break that on the one setting they are most likely to
// leave alone.
inline constexpr float kFloodDefaultTolerance = 32.0f / 255.0f;

// The narrowest coverage ramp that is not a lie: 255 x the largest encoded gap
// between adjacent representable halfs in [0, 1] (3.21507e-4, at linear 0.5).
// §2 derives it and `--selftest` re-measures both this number and the level
// count it buys. Also the default, because the defensible end of a range is a
// better default than a point in the middle of one.
inline constexpr float kFloodEdgeBandFloor = 0.0819844f;
inline constexpr float kFloodDefaultEdgeBand = kFloodEdgeBandFloor;

// Contiguous is a flood fill; Global is a whole-document predicate pass. §3 is
// why they are one enum and two implementations rather than one implementation
// with connectivity disabled.
enum class FloodFillReach {
  Contiguous,  // PRD D25's "contiguous fill", PRD E3's wand with Contiguous ticked
  Global,      // PRD D25's "fill all similar"
};

struct FloodFillParams {
  // Maximum display-encoded per-channel difference, Chebyshev. 0 accepts only
  // an exact match; values above 1 accept most of the picture.
  float tolerance = kFloodDefaultTolerance;

  // Width of the linear falloff at the outer edge of the band, in the same
  // units. Clamped to `tolerance` internally, so the seed texel is always at
  // exactly 1.0 coverage -- a setting that made the clicked texel partially
  // selected would be indefensible whatever the arithmetic said. 0 gives a hard
  // edge (Photoshop's Anti-alias unticked).
  float edgeBand = kFloodDefaultEdgeBand;

  FloodFillReach reach = FloodFillReach::Contiguous;
};

// The seed colour, with its three `srgbEncode()` calls already paid.
//
// Exposed rather than hidden inside the traversal because both reaches build one
// and `--selftest` compares against one; a second, retyped copy of "how the seed
// is prepared" is exactly the drift this file exists to prevent.
struct FloodFillReference {
  std::array<float, 3> encodedRgb{};  // straight RGB, display-encoded
  float alpha = 0.0f;                 // linear, deliberately not encoded (§1)
};

// From a **premultiplied** texel as `core::Tile` stores it. Un-premultiplies
// first (core/Premultiply.hpp's guard, so alpha <= 0 gives {0,0,0,0} and a
// never-written texel compares equal to a written-transparent one), then
// encodes RGB.
FloodFillReference floodFillReferenceFrom(const std::array<float, 4>& premultiplied) noexcept;

// Chebyshev distance between a prepared seed and a premultiplied candidate, in
// display-encoded units. This is the predicate both reaches share.
float floodFillDistance(const FloodFillReference& reference,
                        const std::array<float, 4>& candidatePremultiplied) noexcept;

// The two-colour form, for call sites and assertions that have two texels and no
// reason to hoist. Identical arithmetic -- it builds a reference and calls the
// function above, rather than repeating the metric.
//
// A separate NAME rather than an overload of `floodFillDistance`, and that is
// not style: `FloodFillReference` is an aggregate of a three-float array and a
// float, so a braced `{0, 0, 0, 1}` at the first argument is a viable
// initialiser for *both* parameter types and the call is ambiguous. Discovered
// by writing one, not predicted -- and an overload set a caller cannot use with
// the most natural spelling of a colour is worse than two names.
float floodFillDistanceBetween(const std::array<float, 4>& aPremultiplied,
                               const std::array<float, 4>& bPremultiplied) noexcept;

// Distance turned into selection coverage: 1.0 inside, a linear ramp across
// `edgeBand`, 0.0 outside. Returns exactly 1.0 at distance 0 for any parameters.
float floodFillCoverage(float distance, const FloodFillParams& params) noexcept;

// ---------------------------------------------------------------------------

// The wand, and the region half of the bucket.
//
// `source` is one layer's RGB tiles; `seed` is the clicked document texel;
// `width`/`height` bound the document. Texels outside those bounds are never
// selected and never traversed, **including texels that physically exist inside
// an edge tile** -- a 100-wide document's tile 0 holds 128 columns and the last
// 28 are not part of the picture. Missing tiles read as {0,0,0,0}, so a fill
// seeded on empty canvas floods across space no tile has been allocated for,
// which is correct and is how the bucket fills a blank layer.
//
// Returns a `Selection` whose coverage is the antialiased answer of §2. A seed
// outside the document, or a non-positive document size, gives a selection with
// **no tiles** -- `selectionSelectsNothing()`, engaged and empty, never the
// default-constructed-and-therefore-unrestricted state core/SelectionMask.hpp
// warns about.
Selection floodFillSelection(const TileStore& source, PixelCoord seed, int32_t width,
                             int32_t height, const FloodFillParams& params);

// The paint bucket's write, and PRD D26's fill: premultiplied source-over of one
// colour, weighted by the selection's coverage.
//
// **`selection` is a reference, not a pointer, and that is a decision.**
// Everywhere else in this codebase a null `Selection*` means "no restriction"
// (core/SelectionMask.hpp states it at length, and `clearThroughSelection()`
// honours it by clearing the whole layer). That convention is safe for a
// *clear*, which can only walk tiles that already exist, and unsafe for a
// *fill*, which allocates -- "fill everywhere with no bound" is an infinite
// plane of 128 KiB tiles. So this function does not accept the null case at
// all, and a caller who means "fill the layer" passes `selectAll(w, h)`, which
// is the same thing with the bound written down. Refusing to express the
// unbounded request is cheaper than getting it right.
//
// `straightLinearRgba` is **straight** (non-premultiplied) linear colour -- what
// a colour picker holds -- and is premultiplied here. `opacity` scales coverage;
// at 0 nothing is written and no tile is allocated.
//
// Per texel, with `w = coverage * opacity`:
//     out = src * a * w  +  dst * (1 - a * w)          (all four channels)
// which is ordinary premultiplied source-over of a source scaled by `w`. It is
// the same premultiply-correctness argument `clearThroughSelection()` makes in
// the other direction: because storage is associated, RGB and alpha scale
// together and a half-covered edge texel is half-present rather than
// half-brightness, so there is no fringe.
//
// Returns the number of texels whose stored value actually changed, so "filled
// nothing because the selection was empty" is distinguishable from "filled
// nothing because that colour was already there".
size_t fillThroughSelection(TileStore& tiles, const Selection& selection,
                            const std::array<float, 4>& straightLinearRgba,
                            float opacity = 1.0f);

}  // namespace np
