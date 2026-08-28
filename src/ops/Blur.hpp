#pragma once

#include <cstdint>
#include <vector>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/Blur (PLAN.md "Phase 6 -- Filter and transform it": "Gaussian/box blur
// through the mip pyramid"; DESIGN-imaging.md "Class B -- parametric spatial
// ops -> live passes with ROI").
//
// Blur is the first real consumer of ops/Roi, and the reason phase 6 is
// ordered the way it is: highpass is `src - blur(src)`, unsharp is a highpass,
// selection feather (PRD E4, ops/Feather) is a blur of coverage, and
// make-tileable's gradient removal is a blur of the image divided out of
// itself. One kernel, four features.
//
// ==========================================================================
// The domain. Linear light, premultiplied alpha, and nothing else.
// ==========================================================================
//
// PRD principle 2: "every operation that averages pixels is defined on linear
// light". A blur is nothing but a weighted average, so this file runs on
// core/TileStore's storage exactly as it is -- **linear, premultiplied,
// rgba16float** -- and applies no transfer function in either direction.
//
// Both halves of that are load-bearing and both are the trap PLAN.md warns
// about for this phase, from opposite ends:
//
//   **Not the shaper domain.** Blurring display-encoded values averages the
//   *codes*, not the light. A 50/50 blur of linear 0.0 and 1.0 is linear 0.5;
//   blurred in sRGB it is code 128, which decodes to linear 0.214. That is the
//   "muddy blurs and gradients" DESIGN-imaging.md names as the actual reason
//   other editors' defaults look wrong, and it is a factor of 2.3 on a
//   mid-tone, not a subtlety. Note that this is the exact *opposite* of the
//   other domain rule in the same phase -- add-noise runs in the shaper
//   domain, because fixed-amplitude noise in linear light is invisible in
//   shadow and enormous in highlight. Two neighbouring ops, two opposite
//   answers, which is why each says which one it is out loud.
//
//   **Premultiplied, not straight.** Under premultiplied alpha a texel's RGB
//   is already weighted by how much of it is there, so a weighted average of
//   RGB and a weighted average of A stay consistent with each other and the
//   result is the correct convolution of "light present" and "coverage
//   present" independently. Averaging *straight* RGB gives a fully transparent
//   texel's colour the same vote as an opaque one's, so whatever arbitrary RGB
//   sits behind alpha = 0 bleeds into the visible result -- the black or white
//   halo around every blurred cut-out. ops/Resample.hpp makes the same
//   argument in the other direction (it takes straight input and
//   premultiplies internally); here no conversion is needed at all, which is
//   the point of the working space being premultiplied in the first place.
//
//   Because the store is premultiplied, blurring past the edge of the painted
//   region fades to **transparent**, not to black: RGB and A fall together, so
//   un-premultiplying the result still gives the original hue. --selftest
//   asserts exactly that at the edge of an opaque square, because it is the
//   assertion that would fail if anyone "fixed" the edge handling by clamping.
//
// ==========================================================================
// Separable: two 1-D passes, and what the intermediate costs
// ==========================================================================
//
// A 2-D Gaussian is the outer product of two 1-D Gaussians, so convolving with
// a (2a+1)x(2a+1) kernel is exactly convolving rows then columns with the
// (2a+1)-tap 1-D one. A box kernel factors the same way. That takes the cost
// per texel from (2a+1)^2 multiply-adds to 2(2a+1):
//
//     sigma = 8   (a = 32)    4225 taps -> 130     32x cheaper
//     sigma = 200 (a = 800)   2.56M    -> 3202     801x cheaper
//
// **What it costs instead is one full-size intermediate plane.** The
// horizontal pass cannot write into its own input (a texel's neighbours must
// still hold their *unblurred* values when it is their turn), so `blurPlane()`
// allocates `width * height * channels` floats of scratch -- 16 bytes per texel
// for RGBA -- and holds it for the duration of the call.
//
// Rejected: keeping only a rolling window of `2a+1` horizontally-blurred rows
// instead of the whole plane, which is the standard memory optimisation and
// would cut the scratch from `h` rows to `2a+1`. It is not taken because in
// this file's actual call pattern the scratch is never the dominant term --
// the gathered source plane (below) is at least as large and is resident for
// the same span -- so the saving is at most a third of the peak, bought with a
// modular-indexed ring buffer that makes the pass much harder to read and to
// check against a direct convolution. It is the right change to make if and
// when a profile says the blur is memory-bound; it is not a change to make
// speculatively.
//
// ==========================================================================
// The apron, and why a blur that reads only its own tile is broken
// ==========================================================================
//
// This is the failure mode the whole file is designed against. A blur output
// texel one texel inside a tile's left edge reads source texels `a` texels
// further left, which are in the *neighbouring tile*. A blur that treats the
// tile as its world -- clamping at the edge, or worse, treating outside as
// zero -- produces a value that depends on which tile the texel happens to
// live in. The visible result is a grid of seams at every 128-texel boundary,
// faint enough to survive review and impossible to explain afterwards.
//
// So `blurTiles()` never reads a tile in isolation. It asks ops/Roi for the
// source rectangle the requested output needs -- `roiBackward(blurRoiOp(p),
// outRect)`, an outward dilation by exactly the kernel's reach -- gathers that
// whole rectangle into one flat plane, and blurs it as a single image. The
// apron is `a` texels on all four sides because that is precisely how far the
// kernel reaches and not one texel more; a smaller apron is the seam bug and a
// larger one is wasted fetch.
//
// **--selftest asserts the property directly, and at the strongest available
// strength**: a blur computed in separate per-region calls across a tile
// boundary is **bit-identical** to the same blur computed in one call, and
// every stored half is **exactly** the reference float -- computed on one
// large hand-gathered plane -- rounded to half, over all 131 072 channel
// values checked. Measured, not hoped for -- the split and
// single paths gather differently sized planes, but every tap outside the
// smaller plane contributes exactly `weight * 0.0f`, which changes a float sum
// not at all, so the two sums agree to the last bit. The selftest checks the
// ragged case too (a split at texel 77, nowhere near a tile edge, over a
// deliberately sparse store with a missing tile in the middle), which is
// likewise bit-identical.
//
// And the failure it is guarding against is not marginal: blurring each tile
// as if it were the whole world, at sigma = 6, was measured to be **wrong by
// 0.467** at the tile boundary -- nearly half of full scale -- while being
// exactly right in the tile interior. That is the shape of the bug in one
// number: invisible in the middle of every tile, enormous on a grid of lines
// every 128 texels. --selftest computes that naive path on purpose and asserts
// it differs, so that the seam assertion is proved to be *sensitive* rather
// than merely satisfied.
//
// The apron is also what makes the memory cost non-obvious, so here it is in
// numbers. Gathering the source for a single 128x128 tile costs
// `(128 + 2a)^2 * channels * 4` bytes, and `blurPlane` holds a second plane of
// the same size:
//
//     sigma = 8   (a = 32)    192 x 192    x2 =   1.1 MiB
//     sigma = 32  (a = 128)   384 x 384    x2 =   4.5 MiB
//     sigma = 200 (a = 800)  1728 x 1728   x2 =  91.1 MiB   <-- per tile
//
// That last row is DESIGN-imaging.md's argument for descending the mip
// pyramid, as an actual number: "a sigma = 200 px Gaussian at full res is
// absurd; downsample, blur small, upsample". **This file does not descend the
// pyramid** -- it is the honest full-resolution kernel, and the mip path is a
// separate optimisation that has to compose a downscale, a blur at the coarse
// level and an upscale, whose combined ROI crosses a scale boundary that
// `RoiOp` deliberately cannot express (ops/Roi.hpp says why). Wiring the mip
// descent in before the exact kernel exists would leave nothing to check the
// approximation against.
//
// The apron cost per tile is also why `blurTiles()` takes a **rectangle**
// rather than a tile coordinate: the apron is paid once per call, so blurring
// a 1024x1024 region (64 tiles) in one call at sigma = 8 gathers 1088x1088 --
// 36.1 MiB for both planes, allocated once -- where 64 separate per-tile calls
// peak at only 1.1 MiB but touch 72.0 MiB in total. Peak resident memory
// trades against total traffic, and the caller is the only one that knows
// which of the two it is short of.
//
// ==========================================================================
// The accumulator is f32, and the store's f16 is not good enough
// ==========================================================================
//
// The store is `rgba16float` and PLAN.md warns that "a 200px Gaussian
// accumulating in f16 loses low bits". It does, and the loss is not subtle.
//
// Half has 11 bits of significand, so the spacing of representable values just
// below 1.0 is 2^-11 = 4.88e-4. A sigma = 200 kernel is 1601 taps whose peak
// weight is 1/(sigma*sqrt(2*pi)) = 0.002. Adding a term of order 0.002 into a
// running sum of order 1.0 in half precision therefore moves the accumulator
// by about four representable steps, and once the sum passes 1.0 most taps
// round away entirely. The error is not the random walk of `sqrt(N)*eps` that
// a well-conditioned f16 sum would give -- it is a systematic truncation with
// a preferred sign.
//
// **Measured, not argued.** The same 1-D pass over a unit step, with the same
// kernel, differing only in the accumulator's type, against a double-precision
// reference (max absolute error; the sigma = 8 row is there to show the effect
// is a function of kernel width, not a constant):
//
//     accumulator     sigma = 8 (65 taps)     sigma = 200 (1601 taps)
//     float               7.63e-08                 4.64e-07
//     half                5.44e-04                 2.09e-02
//
// The half column is the interesting one. At sigma = 200 it is 2.1% of full
// scale -- a visible banded error, 86x the f16 *store's* own worst rounding of
// 2.44e-4 -- and it is 45 000x worse than the float column. Even at sigma = 8
// the half accumulator has already spent the store's entire error budget twice
// over before the value is written.
//
// So: **accumulate in `float`, store in half.** Not double either -- the float
// accumulator's own error is 4.64e-07 at sigma = 200, which is 500x below the
// 2.44e-4 the half store quantises to on the way out, so a double accumulator
// would buy precision the destination provably cannot hold and cost twice the
// scratch. The storage format is the error floor for this operation and the
// accumulator's only job is to stay comfortably under it.
//
// (The store's floor itself is measured rather than assumed: the worst
// round-trip error of `floatToHalf`/`halfToFloat` over 200 000 values in
// [0, 1] is 2.441e-04, which is 2^-12 -- half an ulp just below 1.0, exactly
// as the format predicts.)
//
// ==========================================================================
// The kernel
// ==========================================================================
//
// **Gaussian: box-integrated, not point-sampled.** Tap `i` is the integral of
// the continuous Gaussian over the texel's own footprint,
// `Phi((i+0.5)/sigma) - Phi((i-0.5)/sigma)`, evaluated with `std::erf`. A
// texel represents an *area*, not a point, so this is the correct discrete
// kernel; point-sampling `exp(-i^2/2sigma^2)` and renormalising is the common
// shortcut, and it converges to the same thing for wide kernels while being
// badly wrong for narrow ones, where most of the curve's mass falls between
// the taps. Measured difference between the two kernels applied to a unit
// step, max absolute:
//
//     sigma = 0.5   5.19e-02        sigma = 2   2.40e-03
//     sigma = 1     8.25e-03        sigma = 8   1.57e-04
//
// Read against the store's 2.44e-4 floor: point sampling is a visible error
// for every sigma below about 8, and narrow is exactly where a feather radius
// of one or two texels lives (ops/Feather). The erf costs one call per tap at
// kernel-build time -- 1601 of them for the worst case above, once per blur,
// against millions of multiply-adds -- so the accurate form is free.
//
// **Truncation at 4 sigma.** Renormalising hides the missing tail's *mass* but
// not its *shape*. Measured error of a truncated-and-renormalised kernel
// against an 8-sigma reference on a unit step, max absolute:
//
//     truncation    sigma = 2    sigma = 8    sigma = 64
//     3 sigma        5.77e-04     1.10e-03     1.32e-03
//     4 sigma        1.07e-05     2.43e-05     3.06e-05
//     5 sigma        7.60e-08     2.07e-07     2.75e-07
//
// 4 sigma is chosen because it is the first row that lands **below the f16
// store's own 2.44e-4 rounding**: at 4 sigma a wider kernel could not change a
// single stored value, and at the more usual 3 sigma it demonstrably could
// (1.1e-3 is 4.5 quantisation steps). 5 sigma is another 25% of apron for an
// error already two orders of magnitude below what the store can represent, so
// it buys nothing. The cost of 4 over 3 is a 33% wider apron, which is a 78%
// larger gather plane in 2-D -- a real price, paid so that "this blur is exact
// to the storage format" is true rather than nearly true.
//
// **Weights are normalised to sum to 1 in double**, so a constant field blurs
// to itself and a blur cannot change a flat region's brightness. Measured: a
// constant 0.5 field through both kernels comes back at exactly 0.5, max
// deviation 0.0, and --selftest asserts DC preservation directly.
//
// **Box: exact, integer radius, sliding window.** Kernel width `2r+1`, every
// weight `1/(2r+1)`, computed with a running sum so the cost is O(1) per texel
// regardless of radius rather than O(r).
//
// The running sum is kept in `double`, and the honest version of that decision
// is that f32 would also have been fine *today*: measured against a direct
// per-texel summation over a 2048-texel row at r = 64, an f32 running sum
// drifts by 1.39e-06 and a double one by exactly 0. 1.39e-06 is 175x below the
// store's 2.44e-4 floor, so it would not have changed a stored value. Double
// is chosen anyway because the f32 drift scales with the **row length**, which
// is the gather rectangle's width and therefore grows without bound as callers
// blur larger regions, while the store's floor does not move -- so f32 would
// be a number that has to be re-measured every time a caller changes its
// request size. One scalar per line removes the question instead of parking
// it.
//
// Separability itself is measured too: the two-pass f32 result against a
// direct 2-D convolution in double, same kernel, sigma = 6, differs by at most
// 2.41e-07 -- an order of magnitude below the f32 accumulator's own error and
// three below the store's, which is what "separable is exact, not an
// approximation" means in practice.
//
// Box blur is offered as its own filter rather than as three-passes-approximate
// -a-Gaussian. Rejected, explicitly: the triple-box approximation is the
// classic fast Gaussian and it is genuinely close (the errors are around 3% of
// peak), but this kernel feeds `highpass = src - blur(src)`, where the
// difference of two nearly-equal images amplifies exactly the kernel's shape
// error, and it feeds PRD E4's feather, where the shape error becomes a
// visible ripple in a selection edge the user is trying to control. An exact
// Gaussian at 801 taps is affordable because it is separable; the
// approximation exists to avoid a cost this file does not have.
namespace np {

enum class BlurKind {
  Gaussian,  // `sigma` in texels
  Box,       // `boxRadius` in texels; kernel width 2r+1, all weights equal
};

struct BlurParams {
  BlurKind kind = BlurKind::Gaussian;

  // Standard deviation in document texels. Gaussian only. Zero (or anything
  // below the point where the kernel is indistinguishable from a delta) is a
  // legal request and means "identity".
  float sigma = 0.0f;

  // Half-width in document texels. Box only. Zero means identity.
  int32_t boxRadius = 0;
};

// False for a request no kernel can be built from: a negative or non-finite
// sigma, a negative box radius. `blurTiles()` refuses these by name rather
// than clamping them, because a negative radius arriving at a filter is a
// caller bug and silently treating it as zero hides it.
bool blurParamsValid(const BlurParams& p) noexcept;

// How far the kernel reaches, in texels, on each side. **This is the apron**,
// and every other number in this file is downstream of it.
//
// Gaussian: `ceil(4 * sigma)`. Box: `boxRadius`. Zero for an identity request.
int32_t blurApron(const BlurParams& p) noexcept;

// The blur's ROI declaration, for ops/Roi's evaluator: a symmetric dilation by
// `blurApron()`, no translation. The one function that connects this file to
// the spine, and what `blurTiles()` itself uses to size its gather.
RoiOp blurRoiOp(const BlurParams& p) noexcept;

// The normalised 1-D kernel, `2 * blurApron(p) + 1` taps with the centre at
// index `blurApron(p)`. Exposed because a selftest that checks separability
// must convolve with the *same* weights the passes use rather than a retyped
// copy of the formula, and because highpass and unsharp will want to inspect
// it. An identity request returns a single tap of 1.0.
std::vector<float> blurKernel(const BlurParams& p);

// Separable blur of one interleaved float plane, in place-compatible form.
//
// `src` and `dst` each hold `width * height * channels` floats, row-major with
// no padding. **`dst` may alias `src`** -- the vertical pass reads only the
// internal intermediate, so the source is dead by the time anything is written
// -- and `blurTiles()` relies on that to hold two planes instead of three.
//
// **Texels outside the buffer are treated as zero**, which is transparent
// black under premultiplied alpha and therefore the correct implicit content
// of a tile that does not exist. The consequence, and the contract a caller
// must respect: only the region **inset by `blurApron(p)` on all four sides**
// is equal to the infinite-domain convolution. Everything within the apron of
// the buffer's own edge is a blur of `src` against implicit zeros, which is
// right when the signal really is zero out there (ops/Feather relies on this)
// and wrong when the caller merely cropped (`blurTiles()` gathers an apron so
// that it never is).
//
// A `channels` of 1 is the coverage-plane case ops/Feather uses; 4 is RGBA.
// Nothing here interprets the channels, which is the whole reason a
// premultiplied store can be blurred with the same code as a coverage mask.
void blurPlane(const float* src, int32_t width, int32_t height, int32_t channels,
               const BlurParams& p, float* dst);

// Blurs `src` into `dst` over the document rectangle `outRect`.
//
// Reads `roiBackward(blurRoiOp(p), outRect)` worth of `src` -- outRect plus the
// apron -- so the result is identical whether `outRect` is one tile, one tile's
// worth of an interior region, or the whole document. Tiles of `src` that do
// not exist read as transparent black.
//
// Writes only texels inside `outRect`. Texels of a boundary tile that fall
// outside `outRect` are left as `dst` already had them (zero for a tile this
// call allocated), so a caller can blur a region into an existing store without
// erasing its surroundings.
//
// Returns false and writes nothing when:
//   - `dst` is null, or `dst` is the same store as `src` (the gather would race
//     the scatter; a blur is not in-place at the store level);
//   - `blurParamsValid(p)` is false;
//   - `outRect` is empty.
// An identity request (zero apron) is a legal success and copies.
bool blurTiles(const TileStore& src, const PixelRect& outRect, const BlurParams& p,
               TileStore* dst);

// **Testing hook, not a production entry point.** True when `blurPlane`'s
// channel-vectorised fast path (`convolveLine4`/`boxLine4`, ops/Blur.cpp,
// `channels == 4`) is bit-for-bit identical to running the *old*, unmodified
// per-channel scalar path (`convolveLine`/`boxLine`) four times over the same
// deterministic random data -- run at the SAME stride (4 floats: one texel)
// production code actually uses for both, on `n` texels with kernel reach
// `apron`/box `radius`.
//
// **Why this can't be checked by calling `blurPlane` twice with `channels=4`
// and `channels=1` and diffing the results, the way app/selftest normally
// proves an equivalence.** `convolveLine`/`boxLine` are called through
// `blurPlane` at whatever stride `channels` makes them: 4 when `channels==4`
// (the real RGBA case, matching `convolveLine4`'s stride exactly), but 1 --
// contiguous -- when `channels==1` (ops/Feather's coverage plane). A
// **contiguous** stride is exactly the layout the scalar loop CAN
// autovectorise, and the compiler takes a materially different path for it
// on this toolchain: `convolveLine4`'s broadcast-multiply loop compiles to a
// single fused `fmla` (one rounding) per tap, while `convolveLine` at
// stride=1 compiles to an unrolled tap-tree of separate `fmul`/`fadd`
// (multiple roundings) -- so the two disagree by up to ~1 ULP on data where
// they are mathematically equal but were never claimed to share a rounding
// path. At stride=4 -- the shape both `convolveLine4` and every real
// `blurPlane(channels=4, ...)` caller actually use -- the scalar loop's
// strided/gathered access defeats that same autovectorisation (this file's
// whole reason for existing: "close to the worst possible layout for a
// vector unit"), so it reduces to the same single-accumulator `fmadd` shape
// `convolveLine4` uses per channel, and the two agree bit for bit. This
// function is the proof of that, run at the stride that is actually true
// rather than the stride that happens to be reachable through `channels=1`.
bool blurSelfTestChannelVectorMatchesScalar(int32_t n, int32_t apron, uint64_t seed);
bool blurSelfTestChannelVectorMatchesScalarBox(int32_t n, int32_t radius, uint64_t seed);

}  // namespace np
