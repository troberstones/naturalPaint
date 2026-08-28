#pragma once

#include <array>

#include "core/Histogram.hpp"
#include "ops/PointOps.hpp"

// ops/AutoLevels (docs/operations.md §1.2: "Auto-tone", "Auto-contrast",
// "Auto-white-balance" -- all **P1**, plus a fourth, Equalize, this module
// adds for the same reason). §1.2's own words are the whole design brief:
//
//   "Auto-anything is a **parameter solver, not an op**: it inspects the
//   histogram, computes levels or balance parameters, and writes them into
//   an ordinary editable op. The user can then adjust what it chose, which
//   is the whole reason to build it that way."
//
// So nothing in this file is `rgb -> rgb`. Every function here is
// `HistogramResult -> parameters for an existing op` -- chiefly
// `std::array<LevelsParams, 3>` (ops/PointOps.hpp), the exact type
// `applyLevels()` already consumes, and once (Equalize) a `Curve`, the exact
// type `applyCurves()` already consumes. A caller hands the result to the
// existing op and the user can open the Levels/Curves dialog afterwards and
// see -- and edit -- precisely what the solver chose. No menu, no dialog, no
// core::OpStack wiring lives here; those are separate, later steps this
// module deliberately does not reach for.
//
// -- The domain crossing every solver below has to make ---------------------
// core::HistogramResult's bins are DISPLAY-encoded (sRGB): computeHistogram()
// (core/Histogram.cpp) bins `srgbEncode()` of each un-premultiplied texel,
// per core/Histogram.hpp's own header comment ("a histogram exists to be
// read visually... every mainstream image editor's histogram plots
// display-referred values"). LevelsParams::blackIn/whiteIn, by contrast, are
// SCENE-LINEAR -- ops/PointOps.hpp's own contract ("Operate on straight
// (non-premultiplied), scene-linear RGB"). A solver that fed a display-domain
// bin value straight into `blackIn`/`whiteIn` would be confidently wrong in
// exactly the way that still passes a self-consistent test (its own
// hand-computed expectations would inherit the same domain error) -- this
// was checked against Histogram.cpp directly, not assumed from prose:
//
//   bin_index = clamp(floor(displayValue * binCount), 0, binCount - 1)
//   (core/Histogram.cpp's binIndex(), read verbatim before writing this file)
//
// so bin `i` covers the half-open DISPLAY-domain interval
// `[i / binCount, (i+1) / binCount)`. Every solver below takes that
// interval's midpoint, `(i + 0.5) / binCount`, as the bin's representative
// display value (minimizes worst-case error against any true value that
// landed in the bin -- the standard histogram-midpoint convention), then
// un-does the encode Histogram.cpp applied via `srgbDecode()`
// (color/Space.hpp) to reach the scene-linear domain LevelsParams actually
// lives in. This conversion is centralised in one place (binToLinear(), in
// the .cpp) rather than repeated per solver, precisely so this domain
// crossing happens exactly once and is not a place a future edit could get
// backwards per-solver.
//
// Equalize's curve control points cross one domain further still: a `Curve`
// (ops/PointOps.hpp) is authored in color/Shaper's ACEScct log domain, not
// linear (ADR-0004) -- so its solver chains `srgbDecode()` (display -> linear)
// and then `shaperEncode()` (linear -> shaper) for both the x (input) and y
// (output) coordinate of every control point it emits. See solveEqualize()'s
// own doc comment below for the full chain.
//
// -- A named limitation inherited from core::HistogramResult, not created
// here -- core/Histogram.hpp documents at length that `computeHistogram()`
// does not composite: `sampleAllLayers` bins each layer independently (a
// pixel under two layers counts twice), ignores per-layer opacity/visibility/
// masks, and ignores Pigment layers outright. Every solver in this file
// inherits that limitation unchanged: an "auto" op run against a multi-layer
// stack's histogram inspects the same uncomposited counts a human reading the
// histogram panel would see, not a true flattened image. This is unlikely to
// matter for the single-RGB-layer documents this codebase's Document
// invariant produces today, but it is a real caveat for whenever that
// invariant changes, same as it already is for the histogram panel itself.
//
// -- Clip fraction, shared by all four solvers ------------------------------
// The conventional guard against a single stray hot/cold pixel (a sensor
// speck, a compression artifact) dragging a black/white point to an outlier
// instead of the image's real tonal range: ignore the outermost `clipFraction`
// of pixels at EACH tail independently (not `clipFraction` split between both
// tails -- this file's own explicit convention, stated here since "0.1%
// clipped" is ambiguous between the two readings in casual use) before
// locating a black or white point. 0.1% (0.001) is the conventional figure
// mainstream photo-editing tools default their own auto-tone/auto-contrast
// guards to; it is used here only as a familiar, defensible starting point,
// not reimplemented from any tool's internals. `clipFraction = 0` is
// well-defined: it clips nothing, so the black/white points land exactly at
// the histogram's true occupied extremes (see AutoLevelsParams's own doc
// comment for the exact "first/last bin whose cumulative count is nonzero"
// meaning of that case).
namespace np {

// Tuning shared by all four solvers below -- one struct, not four
// near-identical ones, since `clipFraction` is the only parameter any of
// them currently needs; a future solver-specific knob (if one ever arises)
// is a reason to split this, not a reason to anticipate the split now.
struct AutoLevelsParams {
  // Fraction of pixels (by count) to treat as clipped outliers at EACH tail
  // independently before a solver locates a black/white point or builds a
  // cumulative distribution -- see this header's top comment for why "each
  // tail independently" is the chosen (and stated) convention, and why 0.1%
  // is the default. Clamped internally to [0, 0.49]: a per-tail clip of 0.5
  // or more would discard at least half the pixels from EACH end, which for
  // any histogram leaves no bin uncontested by both tails at once -- see the
  // .cpp's clampClipFraction() for the exact reasoning.
  //
  // `clipFraction == 0`: no clipping. A solver's black/white point (or, for
  // Equalize, its curve's domain) lands exactly on the histogram's true
  // occupied extremes -- the lowest and highest bin with a nonzero count --
  // which is well-defined for any non-empty histogram and is deliberately
  // NOT special-cased away from the general clip-search below (a clip
  // threshold of exactly 0 pixels is just the ordinary case with an empty
  // clipped tail).
  float clipFraction = 0.001f;
};

// ---------------------------------------------------------------------
// 1. Auto tone (docs/operations.md §1.2: "per-channel black/white points
// from the histogram"). Each of R, G, B independently gets its own black
// and white point, found by clipping `tuning.clipFraction` of that
// channel's own pixels off each tail and mapping the remaining occupied
// range to [blackIn, whiteIn] (gamma/blackOut/whiteOut stay at their
// LevelsParams defaults -- neutral -- since this solver only ever answers
// "where do black and white sit", not "reshape the midtones" or "remap the
// output range").
//
// *** This is the one of the pair that CAN shift hue, and does so by
// design *** -- three independent per-channel stretches is exactly how a
// colour cast (say, a warm-toned scan where R's occupied range sits higher
// than B's) gets neutralised: each channel's own white point gets pulled
// down to its own bright end independently, which is precisely what
// realigns three unequal channel ranges into three equal ones. The
// side-effect is inseparable from the goal -- an operation whose entire
// job is "make each channel span the same range" cannot help but change
// the RELATIVE balance between channels, which is what hue is. A caller
// who wants tonal correction WITHOUT a hue shift wants solveAutoContrast()
// below instead.
std::array<LevelsParams, 3> solveAutoTone(const HistogramResult& histogram,
                                           const AutoLevelsParams& tuning = {});

// ---------------------------------------------------------------------
// 2. Auto contrast (docs/operations.md §1.2: "composite black/white
// points, so hue is preserved"). ONE black/white point pair, shared by all
// three channels -- computed from `histogram.luma` (the same Rec.709-
// weighted composite channel computeHistogram() already builds, see
// core/Histogram.hpp), not from pooling R+G+B's bins together (which would
// double/triple-count every pixel rather than reading one composite
// brightness per pixel).
//
// *** Hue-preserving BECAUSE the same pair goes to all three channels ***
// -- applying identical LevelsParams to R, G and B is exactly
// applyLevels()'s "composite" mode already documented in PointOps.hpp
// ("a 'composite' levels adjustment is just the caller passing the same
// LevelsParams for all three channels"): the SAME affine remap
// `t = (x - blackIn) / (whiteIn - blackIn)` runs on every channel, so a
// pixel's channel RATIOS (its hue and saturation, in the sense of relative
// channel proportions) are preserved exactly wherever `t` stays in the
// pre-clamp [0,1] region -- only the shared brightness axis moves, never
// the balance between channels. This is the direct structural contrast
// with solveAutoTone() above: same clip-and-stretch mechanism, but a
// SINGLE shared measurement (luma) in place of three independent
// per-channel ones is what keeps hue untouched.
std::array<LevelsParams, 3> solveAutoContrast(const HistogramResult& histogram,
                                               const AutoLevelsParams& tuning = {});

// ---------------------------------------------------------------------
// 3. Auto colour / auto white balance (docs/operations.md §1.2: "grey-world
// or brightest-neutral estimate"). This picks the GREY-WORLD estimate:
//
//   meanLinear[c] = the clipped mean of channel c's occupied scene-linear
//                   values (the clip-trimmed mean over the same
//                   clipFraction-bounded range solveAutoTone() searches,
//                   computed in LINEAR light -- a physical-light average is
//                   what grey-world theory actually reasons about, not a
//                   perceptual/display-domain one, so this is the one
//                   solver that averages `binToLinear()` values directly
//                   rather than only using them as endpoint(s))
//   grayTarget      = mean(meanLinear[0], meanLinear[1], meanLinear[2])
//   whiteIn[c]      = meanLinear[c] / grayTarget   (blackIn/gamma/blackOut/
//                     whiteOut left at their neutral LevelsParams defaults)
//
// With blackIn=0, gamma=1, blackOut=0, whiteOut=1, applyLevelsChannel()
// (ops/PointOps.hpp) reduces to `output = clamp(input / whiteIn, 0, 1)`,
// i.e. a pure multiply by `grayTarget / meanLinear[c]` in the unclamped
// region -- exactly the per-channel gain a grey-world corrector applies: a
// channel whose mean sits ABOVE the target gets a `whiteIn` ABOVE 1 (needs
// a brighter input to reach white, i.e. is suppressed), and a channel
// below target gets `whiteIn` below 1 (boosted) -- gain-only, no black-point
// shift, the same "correct the cast, leave exposure and shadows alone"
// posture a simple white-balance gain (not a full colour-balance curve)
// is expected to have.
//
// *** What this assumes, and when it is wrong *** -- grey-world theory's
// premise is that a scene's AVERAGE reflected colour, integrated over
// enough surfaces and enough of the frame, is neutral grey: real-world
// materials cover the spectrum broadly enough that their sum washes out to
// grey, so any large deviation from grey in the mean must be the light
// source's colour, not the scene's. This fails, predictably, whenever the
// frame is NOT a representative sample of "everything" -- a photo
// dominated by one true colour (a forest canopy, a red-rock canyon, a
// close-up of a blue wall) has a genuinely non-grey mean for legitimate
// content reasons, and grey-world will misread that as a cast and
// partially neutralise a scene that had no cast to begin with. (The other
// named estimate, brightest-neutral -- assume the single brightest region
// is a specular highlight or white surface and scale so IT reads neutral
// -- fails the opposite way: it needs a genuinely neutral bright object to
// exist in frame at all, and has nothing to anchor to on a scene with none,
// e.g. a sunset with no white/grey surface in it. Grey-world was chosen
// here because it degrades more gracefully on an arbitrary histogram with
// no scene knowledge -- it always has SOME mean to work from, where
// brightest-neutral can have no genuinely-neutral pixel at all -- not
// because it is unconditionally the better estimate.)
//
// Degenerate: a channel with zero occupied samples (impossible for a
// non-empty histogram unless that single channel is somehow entirely
// empty while another is not -- e.g. a hand-built test fixture) leaves
// that channel's LevelsParams at the neutral default rather than dividing
// by a zero mean; an entirely empty histogram (`grayTarget <= 0`, which is
// the same condition as "there is no data at all" for a histogram whose
// counts can never be negative) returns all-neutral LevelsParams
// unchanged -- see solveAutoColor()'s own definition for exactly which
// check performs each guard.
//
// *** The cost of expressing a gain as a white point: a BOOSTED channel
// clips its own highlights ***
// `LevelsParams` has no pure-gain field, so the correction rides on
// `whiteIn`, and `applyLevelsChannel()` clamps its normalised `t` to [0,1]
// (ops/PointOps.hpp explains why that clamp is a mathematical necessity
// there, not a policy). For a channel being SUPPRESSED (`whiteIn > 1`) that
// costs nothing -- the clamp only bites above 1.0, which is HDR headroom.
// For a channel being BOOSTED (`whiteIn < 1`, the blue channel under a warm
// cast, say) the clamp bites at `input == whiteIn`, which is *below* 1.0:
// every value above it saturates to white. So the brightest part of a
// boosted channel flattens, and hue shifts there in the opposite direction
// from the cast being corrected.
//
// This is real, it is visible on a bright image, and it is named here rather
// than left for a painter to discover, because nothing else in the call
// chain will mention it -- `applyLevels()` is doing exactly what Levels is
// supposed to do. Two ways out exist and neither is taken here: carry the
// gain on a `ChannelMixerParams` diagonal instead (no clamp anywhere, but
// the result is then not editable in the Levels dialog, which is the whole
// point of "a solver writes parameters into an ordinary editable op"), or
// scale all three channels so the largest gain lands at exactly 1.0 (no
// clipping, but it darkens the image, turning a white balance into an
// exposure change nobody asked for). Both are worse trades than a named
// limitation; the second especially, since a solver that silently changes
// exposure is harder to notice than one that clips.
std::array<LevelsParams, 3> solveAutoColor(const HistogramResult& histogram,
                                            const AutoLevelsParams& tuning = {});

// ---------------------------------------------------------------------
// 4. Equalize -- a histogram-derived cumulative distribution (standard
// histogram-equalization, e.g. Gonzalez & Woods' "Digital Image
// Processing"; not implemented from, or checked against, any commercial
// tool's own Equalize command). NOT expressible as LevelsParams -- a CDF is
// not an affine black/white/gamma remap in general -- so this returns a
// `Curve` (ops/PointOps.hpp's existing control-point type) instead, which
// keeps this file's governing rule intact: a Curve is exactly as editable
// afterwards, through the Curves dialog, as a LevelsParams is through the
// Levels dialog.
//
// One curve, built from `histogram.luma` (the composite channel), meant to
// be applied IDENTICALLY to R, G and B by the caller (`array<Curve,3>{c, c,
// c}`, the same "one measurement, replicated to all three channels" shape
// solveAutoContrast() above uses for the same reason: a shared curve moves
// the shared brightness axis only, preserving hue, where three
// independently-equalized channels would shift it exactly as
// solveAutoTone() does. A future "equalize channels independently" variant
// is a legitimate alternative this deliberately does not build.
//
// *** The classic formula, and the two extra domain conversions a Curve's
// control points need beyond what solveAutoContrast() above required ***
// Let `[loBin, hiBin]` be the clip-trimmed occupied range (same search as
// every other solver here), `countInRange` the total count within it, and
// `cdfMin = histogram.luma[loBin]` (guaranteed > 0 -- see the .cpp's
// computeClipIndices() doc comment for why the clip search can never land
// `loBin` on an empty bin). For a sampled bin `i` in that range:
//
//   cdf(i)     = sum of histogram.luma[loBin..i]
//   cdfNorm(i) = clamp((cdf(i) - cdfMin) / (countInRange - cdfMin), 0, 1)
//
// -- the textbook full-stretch equalization formula (subtracting `cdfMin`
// and dividing by `countInRange - cdfMin`, rather than the simpler
// `cdf(i) / countInRange`, is what makes the curve's low end actually
// reach display-output 0 instead of stopping at whatever fraction the
// first occupied bin alone represents). `cdfNorm(i)` is itself a DISPLAY-
// domain output value (a normalized position in [0,1], exactly the same
// domain histogram.luma's bins are already in) -- so building a control
// point needs `srgbDecode()` on BOTH coordinates (input display value
// `binCenterDisplay(i)` -> linear, and output display value `cdfNorm(i)`
// -> linear) followed by `shaperEncode()` on both linear results, since a
// Curve's (x, y) pair lives in color/Shaper's log domain, not linear
// (ADR-0004; ops/PointOps.hpp's Curves section). solveAutoTone() and
// solveAutoContrast() above only ever needed the first half of that chain
// (display -> linear) because LevelsParams stops at linear; a Curve goes
// one domain further.
//
// Points are sampled at up to `np::kMaxCurvePointsPerChannel`
// (color/LutBake.hpp) strictly-increasing bin indices spanning
// `[loBin, hiBin]` inclusive -- LutBake.hpp's own doc comment names this as
// the GPU kernel's fixed-size control-point buffer bound; a curve baked
// with more points than that is silently truncated, which is why this
// solver caps its OWN output at that bound rather than relying on some
// later stage to enforce it. `x` is strictly increasing (bin index ->
// display value -> linear -> shaper is monotonic at every step); `y` is
// non-decreasing (`cdf(i)` is a running sum of non-negative counts, so it
// never decreases as `i` increases, and every later domain conversion
// applied to it -- srgbDecode, shaperEncode -- is itself monotonic
// non-decreasing) -- so the emitted Curve is monotonically non-decreasing
// by construction, not merely in the fixtures this file happens to test.
//
// Degenerate: an empty histogram, or one whose entire occupied range
// collapses to a single bin after clipping (`hiBin <= loBin` -- nothing to
// redistribute; any two bins would need distinct display values to define
// a real CDF shape), returns an EMPTY Curve -- identity, per evalCurve()'s
// own "0 or 1 control points is identity" contract (ops/PointOps.hpp) --
// rather than fabricating a one-point curve or dividing by a zero-width
// range.
Curve solveEqualize(const HistogramResult& histogram, const AutoLevelsParams& tuning = {});

}  // namespace np
