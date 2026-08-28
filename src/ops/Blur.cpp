#include "ops/Blur.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "core/Parallel.hpp"
#include "core/Tile.hpp"

// Attribution switches for the two optimisations below, each independently
// disableable so a build can isolate one from the other -- the same way
// src/CMakeLists.txt's P2-2 hardening comment attributes libc++ FAST mode
// against `-fstack-protector-strong` by building all four combinations.
// Neither is meant to be flipped by an ordinary build; both default on.
//
// **Flip them by editing the two `#define`s below. There is deliberately no
// CMake switch**, and the reason is worth recording because the obvious one
// is a trap. A `-D` on the CMake command line naming a cache variable that
// does not exist is accepted in silence -- CMake stores it and nothing reads
// it -- so a measurement driven that way produces a full set of plausible
// per-configuration numbers while every build is in fact the default one.
// That happened during this change's own review: four "attribution" builds
// driven by a `-D` for a variable that had already been removed all measured
// the same shipped binary, and the giveaway was only that the supposedly
// unoptimised configuration came out as fast as the optimised one. Editing
// the two lines below cannot fail that way, because getting it wrong is a
// compile error rather than a silent no-op.
//
// One thing makes editing them safe: `kBlurLineGrain` is `[[maybe_unused]]`,
// so the threading-off configuration still compiles under this project's
// `-Werror`. Without that it fails on an unused-constant warning -- and a
// build failure swallowed by a redirect is the second way to measure a stale
// binary. All four combinations build clean and run `--selftest` to 0 FAIL,
// so each one is asserted correct, not merely fast.
//
// Measured this way on this machine (M4 Max), `--selftest`'s T15 live-preview
// blur, which is the number PRD F3's budget is judged against:
//
//     configuration            1024^2      2048^2
//     neither                  207.5 ms    855.0 ms
//     SIMD only                 57.8 ms    231.3 ms   ( 3.6x /  3.7x)
//     threading only            22.0 ms     89.7 ms   ( 9.4x /  9.5x)
//     both (shipped)             9.1 ms     38.7 ms   (22.8x / 22.1x)
//
// Threading is the larger of the two, and they compose sub-multiplicatively
// (3.6 x 9.4 = 34, observed 23) -- past a point the loop stops being bound on
// the convolution arithmetic these two optimise and starts being bound on the
// gather/scatter and allocation around it.
#ifndef NP_BLUR_SIMD_CHANNELS
#define NP_BLUR_SIMD_CHANNELS 1
#endif
#ifndef NP_BLUR_THREADED
#define NP_BLUR_THREADED 1
#endif

namespace np {

namespace {

// **FP contraction pinned OFF for every function in this file that does a
// multiply-then-add reduction, from here to the matching `contract(on)`
// below `boxLine4`.** Found, not assumed: `convolveLine`'s reduction
// (`acc += kernel[..] * in[..]`) is eligible for fused multiply-add under
// the default "contract within a statement" rule every C/C++ compiler
// applies without `-ffast-math`, and this project sets no flag that
// disables it. Under `-flto=thin` (src/CMakeLists.txt's P0-2), the compiler
// does not fuse it consistently -- a standalone `-O2` build (no LTO)
// compiles the SAME `convolveLine` source to a single fused `fmla` at every
// tap count tried, but the ThinLTO build fuses it only for short reductions
// (apron 0-1) and switches to a separate multiply-then-add (two roundings)
// once the tap count grows past a cost-model threshold ThinLTO's
// import/inlining pass apparently crosses somewhere around apron 8 -- found
// with a standalone repro comparing `-flto=thin` against a plain `-O2`
// build of the identical source, not by reading a manual. `convolveLine4`
// (the channel-vectorised fast path below) hits its own, INDEPENDENT
// threshold for the same decision, so above that gap in apron sizes the two
// functions fuse differently from each other -- not because either
// reordered anything, but because "fuse or don't" is a rounding-affecting
// choice the standard leaves to the compiler, and this build's LTO pass
// makes that choice per-function rather than per-file. Pinning contraction
// off removes the ambiguity outright: both functions always do the
// unfused, two-rounding multiply-then-add, so `convolveLine4` accumulating
// four channels in parallel is bit-identical to four calls of
// `convolveLine` **regardless of what either function's contraction
// decision would otherwise have been** -- checked directly by
// `ops/Blur.hpp`'s `blurSelfTestChannelVectorMatchesScalar` across a range
// of apron sizes spanning that threshold. The cost is one FMA fusion (a
// sub-ULP precision difference, and one fewer fused instruction per tap)
// given up in exchange for a rounding path that does not depend on LTO's
// internal cost model -- worth it for a file whose whole discipline is
// "checkable against a direct convolution," not "checkable until the next
// compiler upgrade changes an inlining heuristic."
#pragma clang fp contract(off)

// One 1-D convolution along a strided line, with **zero outside the line**.
//
// `stride` is in floats, so the same function serves both passes: a horizontal
// line has stride `channels` and a vertical one has stride `width * channels`,
// and neither pass needs its own copy of the loop. Writing it once is worth
// more than the vertical pass's worse cache behaviour costs -- the two passes
// have to agree exactly about clipping and normalisation or the separable
// result stops matching a direct 2-D convolution, and two copies of a clipped
// accumulation loop is precisely the kind of thing that drifts.
//
// The known cost of the shared form, named rather than hidden: the vertical
// pass strides by a whole row, so for a wide kernel every tap is a cache miss.
// The standard fix is to transpose between passes and run both horizontally.
// Not done here -- it needs a third full-size buffer or an in-place blocked
// transpose, and this file's first duty is to be checkable against a direct
// convolution. A profile is the thing that should change it.
//
// **The accumulator is `float`, deliberately, and ops/Blur.hpp's "the
// accumulator is f32" section carries the measurements.** Half loses the low
// bits of a wide kernel outright (2.09e-02 of error at sigma = 200, against
// 4.64e-07 for this float accumulator and 2.44e-04 for the store it is written
// to); double would buy precision the f16 store provably cannot hold.
void convolveLine(const float* in, int64_t inStride, int32_t n, const float* kernel,
                  int32_t apron, float* out, int64_t outStride) {
  for (int32_t i = 0; i < n; ++i) {
    const int32_t lo = std::max(0, i - apron);
    const int32_t hi = std::min(n - 1, i + apron);
    float acc = 0.0f;
    for (int32_t t = lo; t <= hi; ++t) {
      acc += kernel[t - i + apron] * in[static_cast<int64_t>(t) * inStride];
    }
    out[static_cast<int64_t>(i) * outStride] = acc;
  }
}

// The box kernel's O(1)-per-texel form: a sliding window sum, divided by the
// full width `2r+1` **whether or not the window is clipped**.
//
// That divisor is the whole subtlety. Dividing by the number of texels
// actually inside the line would be a "renormalise at the edge" box, which is
// a different operator -- it does not fade to zero at a boundary, so its
// output cannot match the Gaussian path's edge behaviour, cannot be gathered
// with an apron and cropped, and would reintroduce exactly the tile seam this
// file exists to prevent. Dividing by the constant width makes this a true
// convolution against a zero-extended signal, which is what `convolveLine`
// above does too.
//
// The running sum is kept in `double`, and ops/Blur.hpp's kernel section
// carries the measurement and the honest caveat: an f32 running sum drifts by
// 1.39e-06 over a 2048-texel row at r = 64, which is already 175x below the
// f16 store's floor and would have been acceptable *at that row length*. The
// drift scales with the row -- i.e. with the caller's gather rectangle -- and
// the store's floor does not, so a double accumulator removes a number that
// would otherwise need re-measuring every time a caller blurs a bigger region.
void boxLine(const float* in, int64_t inStride, int32_t n, int32_t radius, float* out,
             int64_t outStride) {
  const double invWidth = 1.0 / static_cast<double>(2 * radius + 1);
  double sum = 0.0;
  const int32_t primeHi = std::min(n - 1, radius);
  for (int32_t t = 0; t <= primeHi; ++t) sum += in[static_cast<int64_t>(t) * inStride];
  for (int32_t i = 0; i < n; ++i) {
    out[static_cast<int64_t>(i) * outStride] = static_cast<float>(sum * invWidth);
    const int32_t entering = i + 1 + radius;
    const int32_t leaving = i - radius;
    if (entering < n) sum += in[static_cast<int64_t>(entering) * inStride];
    if (leaving >= 0) sum -= in[static_cast<int64_t>(leaving) * inStride];
  }
}

// ==========================================================================
// The channel-vectorised fast path (4 channels: RGBA).
// ==========================================================================
//
// `convolveLine`/`boxLine` above are called once PER CHANNEL, with `stride ==
// channels` -- a 3-float hole between every load, the worst layout for a
// vector unit and the reason this loop never autovectorised. The four
// channels of one texel are contiguous in memory (`core/TileStore.hpp`'s
// `rgba16float`, unpacked to `float` on the way into this file), so the fix
// is to process a whole texel -- all four channels -- per tap instead of one
// channel at a time. That turns the strided gather into a plain 4-float load
// and gives the compiler 4-wide arithmetic to work with, for free.
//
// **Vectorised across CHANNELS, never across TAPS.** `convolveLine4` runs
// four independent scalar accumulators (`acc0..acc3`), one per channel, each
// summing its own channel's taps in exactly the loop order
// `convolveLine`/`boxLine` already use. No cross-channel and no cross-tap
// reduction happens -- reducing pairwise or as a tree across the KERNEL taps
// would reorder float additions and change the low bits, which is exactly
// what this file's bit-identity selftest assertions (app/selftest/Blur.cpp
// section 3: a blur split across a tile boundary is bit-identical to one
// computed in a single call) would catch. Accumulating four channels in
// parallel does not reorder anything WITHIN a channel, so this is
// bit-identical to calling the scalar, per-channel `convolveLine`/`boxLine`
// once per channel -- not an approximation of it, checked directly by
// app/selftest/BlurSimd.cpp against the scalar path on the same random data.
//
// `stride`/`outStride` here are in floats, same as the scalar functions
// above, and carry the same value at both call sites in `blurPlane` below --
// `channels` (4) for the horizontal pass, `rowStride` for the vertical one.
// The only change from the scalar call is that these no longer add a `+ c`
// channel offset and no longer loop over `c`: each call now walks every
// channel of the line at once.
//
// Guarded by `NP_BLUR_SIMD_CHANNELS` (default on) so a measurement build can
// isolate this optimisation from the row/column threading below -- see the
// file's top comment.
#if NP_BLUR_SIMD_CHANNELS
void convolveLine4(const float* in, int64_t stride, int32_t n, const float* kernel,
                   int32_t apron, float* out, int64_t outStride) {
  for (int32_t i = 0; i < n; ++i) {
    const int32_t lo = std::max(0, i - apron);
    const int32_t hi = std::min(n - 1, i + apron);
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    for (int32_t t = lo; t <= hi; ++t) {
      const float w = kernel[t - i + apron];
      const float* p = in + static_cast<int64_t>(t) * stride;
      acc0 += w * p[0];
      acc1 += w * p[1];
      acc2 += w * p[2];
      acc3 += w * p[3];
    }
    float* o = out + static_cast<int64_t>(i) * outStride;
    o[0] = acc0;
    o[1] = acc1;
    o[2] = acc2;
    o[3] = acc3;
  }
}

// Same restructure applied to the box's sliding-window sum. **Four `double`
// running sums, not four `float`** -- ops/Blur.cpp's `boxLine` comment above
// carries the measurement for why the scalar box keeps its sum in `double`,
// and that reasoning is per-channel and unaffected by processing four
// channels side by side; using `float` here to make the vectorisation look
// tidier would silently give up the precision the comment above just
// justified.
void boxLine4(const float* in, int64_t stride, int32_t n, int32_t radius, float* out,
             int64_t outStride) {
  const double invWidth = 1.0 / static_cast<double>(2 * radius + 1);
  double sum0 = 0.0, sum1 = 0.0, sum2 = 0.0, sum3 = 0.0;
  const int32_t primeHi = std::min(n - 1, radius);
  for (int32_t t = 0; t <= primeHi; ++t) {
    const float* p = in + static_cast<int64_t>(t) * stride;
    sum0 += p[0];
    sum1 += p[1];
    sum2 += p[2];
    sum3 += p[3];
  }
  for (int32_t i = 0; i < n; ++i) {
    float* o = out + static_cast<int64_t>(i) * outStride;
    o[0] = static_cast<float>(sum0 * invWidth);
    o[1] = static_cast<float>(sum1 * invWidth);
    o[2] = static_cast<float>(sum2 * invWidth);
    o[3] = static_cast<float>(sum3 * invWidth);
    const int32_t entering = i + 1 + radius;
    const int32_t leaving = i - radius;
    if (entering < n) {
      const float* pe = in + static_cast<int64_t>(entering) * stride;
      sum0 += pe[0];
      sum1 += pe[1];
      sum2 += pe[2];
      sum3 += pe[3];
    }
    if (leaving >= 0) {
      const float* pl = in + static_cast<int64_t>(leaving) * stride;
      sum0 -= pl[0];
      sum1 -= pl[1];
      sum2 -= pl[2];
      sum3 -= pl[3];
    }
  }
}
#endif  // NP_BLUR_SIMD_CHANNELS

// Restores the default (compiler's choice) for everything below --
// `contract(off)` above only needs to cover the two reductions it was
// pinned for.
#pragma clang fp contract(on)

// ==========================================================================
// Threading the row/column loop, and the grain used for it.
// ==========================================================================
//
// core/Parallel.hpp's `kParallelForDefaultGrain` (8) was measured against a
// TILE-sized work item -- a whole 128x128x4-float pass, ~6.2us serial. A
// single `convolveLine4`/`boxLine4` call over one row is a much smaller item
// in the cheapest case (e.g. a 128-wide row at apron 1 is ~150ns), so that
// grain is not assumed to transfer and was re-measured against this file's
// own loop shape rather than reused by default.
//
// Measured on this machine (Apple M4 Max), the real `core::parallelFor`
// mechanism (not a stand-in) over a synthetic row body of the same shape as
// `convolveLine4` -- best-of-15 (`dispatch_apply`'s pool is warm by the time
// production code reaches this loop, exactly as core/Parallel.hpp's own
// comment notes; a cold, unwarmed first call is measurably worse and is not
// the number a real run pays) -- across the realistic range from the
// cheapest row this file ever produces (128 wide, apron 1: a 130-texel-wide
// gather at a near-zero sigma) up to the widest a full-document blur uses:
//
//   shape                    row cost   breakeven n   speedup at n=height>=64
//   w=128,  apron=1 (cheap)    ~150ns    already >1x by n=1      1.1-2.5x
//   w=512,  apron=8 (medium)  ~2.6us     already >1x by n=1      2.0x+
//   w=2048, apron=32 (costly) ~62us      already >1x by n=1      8.1x
//
// Unlike the tile-shaped body core/Parallel.hpp measured, this row-shaped
// body shows no regime where parallelizing loses: `dispatch_apply`'s own
// overhead (core/Parallel.hpp cites 0.01-0.06us/task on this machine) is
// already below even the cheapest row's cost, so there is no floor to clear.
// **The honest conclusion is that 8 is fine, not that a smaller number is
// needed** -- `kParallelForDefaultGrain` is reused rather than replaced.
// (The n=1..7 cases are moot in practice: `blurPlane`'s row/column count is
// its plane's width or height, and every real caller's gather plane is at
// least `kTileSize` = 128 wide/tall, so production code always clears any
// grain in this range regardless of which single-digit number is chosen.)
[[maybe_unused]] constexpr size_t kBlurLineGrain = kParallelForDefaultGrain;

// Runs `body(i)` for `i` in `[0, n)`, either through `core::parallelFor` (the
// production path) or as a plain serial loop -- selectable at compile time
// via `NP_BLUR_THREADED` so a measurement build can isolate the threading
// half of this file's two optimisations from the SIMD half above. Default
// (both macros on) is the shipped behaviour.
template <class F>
void blurLineFor(size_t n, F&& body) {
#if NP_BLUR_THREADED
  parallelFor(n, kBlurLineGrain, std::forward<F>(body));
#else
  for (size_t i = 0; i < n; ++i) body(i);
#endif
}

}  // namespace

bool blurParamsValid(const BlurParams& p) noexcept {
  if (p.kind == BlurKind::Gaussian) return std::isfinite(p.sigma) && p.sigma >= 0.0f;
  return p.boxRadius >= 0;
}

int32_t blurApron(const BlurParams& p) noexcept {
  if (!blurParamsValid(p)) return 0;
  if (p.kind == BlurKind::Box) return p.boxRadius;
  if (p.sigma <= 0.0f) return 0;
  // ceil(4 * sigma). The truncation constant is measured, not chosen for
  // roundness -- ops/Blur.hpp's kernel section carries the numbers and the
  // reason 4 rather than the more usual 3 (at 3 sigma the truncation error is
  // larger than the f16 store's own quantisation step, so a wider kernel would
  // change stored values; at 4 sigma it is smaller, so it cannot).
  const double reach = std::ceil(4.0 * static_cast<double>(p.sigma));
  // Clamped into ops/Roi's coordinate range so an absurd sigma cannot produce
  // a margin that overflows a rectangle. The clamp is unreachable for any
  // sigma a document can justify; it exists so that "unreachable" does not
  // have to be argued from the caller's good behaviour.
  const double capped = std::min(reach, static_cast<double>(kRoiCoordLimit));
  return static_cast<int32_t>(capped);
}

RoiOp blurRoiOp(const BlurParams& p) noexcept { return roiDilateOp(blurApron(p)); }

std::vector<float> blurKernel(const BlurParams& p) {
  const int32_t apron = blurApron(p);
  const size_t taps = static_cast<size_t>(2 * apron + 1);
  std::vector<float> kernel(taps, 0.0f);
  if (apron == 0) {
    kernel[0] = 1.0f;
    return kernel;
  }

  std::vector<double> weights(taps, 0.0);
  if (p.kind == BlurKind::Box) {
    for (double& w : weights) w = 1.0;
  } else {
    // **Box-integrated, not point-sampled.** Tap i is the mass of the
    // continuous Gaussian falling inside texel i's own footprint
    // [i-0.5, i+0.5), which is what a texel means: an area, not a sample
    // point. Expressed through erf because that is what the normal CDF is:
    //   Phi(t) = 0.5 * (1 + erf(t / (sigma * sqrt(2))))
    // and the tap is Phi(i+0.5) - Phi(i-0.5). The 0.5 factors cancel in the
    // difference, so only the erf pair is evaluated.
    //
    // Rejected: exp(-i^2 / (2 sigma^2)) sampled at integer i and renormalised,
    // which is the shortcut in most implementations. It converges to the same
    // thing for wide kernels and is measurably wrong for narrow ones, where
    // most of the kernel's mass falls between the taps -- and narrow is
    // exactly where a feather radius of 1 or 2 texels lives.
    const double invScale = 1.0 / (static_cast<double>(p.sigma) * std::sqrt(2.0));
    for (int32_t i = -apron; i <= apron; ++i) {
      const double hi = std::erf((static_cast<double>(i) + 0.5) * invScale);
      const double lo = std::erf((static_cast<double>(i) - 0.5) * invScale);
      weights[static_cast<size_t>(i + apron)] = 0.5 * (hi - lo);
    }
  }

  // Normalised in double so the taps sum to 1 as exactly as the format allows.
  // This is what makes a blur preserve DC: a constant field must blur to
  // itself, or every flat region changes brightness with the radius and the
  // filter is unusable at any setting. The truncated tail is absorbed here,
  // which is why the truncation constant is chosen against the *shape* error
  // rather than the missing mass -- the missing mass is redistributed by this
  // division and never appears as a brightness shift.
  double sum = 0.0;
  for (const double w : weights) sum += w;
  const double inv = sum > 0.0 ? 1.0 / sum : 0.0;
  for (size_t i = 0; i < taps; ++i) kernel[i] = static_cast<float>(weights[i] * inv);
  return kernel;
}

void blurPlane(const float* src, int32_t width, int32_t height, int32_t channels,
               const BlurParams& p, float* dst) {
  if (src == nullptr || dst == nullptr || width <= 0 || height <= 0 || channels <= 0) return;

  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height) *
                        static_cast<size_t>(channels);
  const int32_t apron = blurApron(p);
  if (apron == 0 || !blurParamsValid(p)) {
    // Identity. A memmove rather than a convolution with a single unit tap, so
    // a zero-radius request cannot perturb a value by even one ulp -- the same
    // discipline ops/Resample takes for a 1:1 resize.
    if (dst != src) std::memmove(dst, src, texels * sizeof(float));
    return;
  }

  // The one intermediate ops/Blur.hpp names the cost of: the horizontal pass
  // cannot write into its own input, because a texel's neighbours must still
  // hold unblurred values when their turn comes. `dst` is allowed to alias
  // `src` precisely because this buffer exists -- after the horizontal pass
  // the source is dead, so the vertical pass may write over it.
  std::vector<float> mid(texels, 0.0f);

  const int64_t rowStride = static_cast<int64_t>(width) * channels;
  float* const midData = mid.data();

  // Both passes below are threaded with `blurLineFor` (row count for the
  // horizontal pass, column count for the vertical one) -- every row/column
  // is independent of every other, the same "tile gather and scatter are
  // disjoint" argument `blurTiles()` already makes for its own loops, just
  // one level down at row/column granularity instead of tile granularity.
  // And every row/column takes the channel-vectorised `convolveLine4`/
  // `boxLine4` path when `channels == 4` (RGBA, `Tile::kChannels` --
  // `blurTiles()`'s only caller shape) and falls back to the scalar,
  // per-channel loop otherwise (`channels == 1`, ops/Feather's coverage
  // plane). See this file's top-of-namespace comment for why 4 is
  // vectorised across channels and not hardcoded as the only shape this
  // function accepts.
  if (p.kind == BlurKind::Box) {
    const int32_t r = p.boxRadius;
#if NP_BLUR_SIMD_CHANNELS
    if (channels == 4) {
      blurLineFor(static_cast<size_t>(height), [&](size_t yi) {
        const int32_t y = static_cast<int32_t>(yi);
        const float* in = src + static_cast<int64_t>(y) * rowStride;
        float* out = midData + static_cast<int64_t>(y) * rowStride;
        boxLine4(in, channels, width, r, out, channels);
      });
      blurLineFor(static_cast<size_t>(width), [&](size_t xi) {
        const int32_t x = static_cast<int32_t>(xi);
        const float* in = midData + static_cast<int64_t>(x) * channels;
        float* out = dst + static_cast<int64_t>(x) * channels;
        boxLine4(in, rowStride, height, r, out, rowStride);
      });
      return;
    }
#endif
    blurLineFor(static_cast<size_t>(height), [&](size_t yi) {
      const int32_t y = static_cast<int32_t>(yi);
      for (int32_t c = 0; c < channels; ++c) {
        const float* in = src + static_cast<int64_t>(y) * rowStride + c;
        float* out = midData + static_cast<int64_t>(y) * rowStride + c;
        boxLine(in, channels, width, r, out, channels);
      }
    });
    blurLineFor(static_cast<size_t>(width), [&](size_t xi) {
      const int32_t x = static_cast<int32_t>(xi);
      for (int32_t c = 0; c < channels; ++c) {
        const float* in = midData + static_cast<int64_t>(x) * channels + c;
        float* out = dst + static_cast<int64_t>(x) * channels + c;
        boxLine(in, rowStride, height, r, out, rowStride);
      }
    });
    return;
  }

  const std::vector<float> kernel = blurKernel(p);
#if NP_BLUR_SIMD_CHANNELS
  if (channels == 4) {
    const float* const kernelData = kernel.data();
    blurLineFor(static_cast<size_t>(height), [&](size_t yi) {
      const int32_t y = static_cast<int32_t>(yi);
      const float* in = src + static_cast<int64_t>(y) * rowStride;
      float* out = midData + static_cast<int64_t>(y) * rowStride;
      convolveLine4(in, channels, width, kernelData, apron, out, channels);
    });
    blurLineFor(static_cast<size_t>(width), [&](size_t xi) {
      const int32_t x = static_cast<int32_t>(xi);
      const float* in = midData + static_cast<int64_t>(x) * channels;
      float* out = dst + static_cast<int64_t>(x) * channels;
      convolveLine4(in, rowStride, height, kernelData, apron, out, rowStride);
    });
    return;
  }
#endif
  const float* const kernelData = kernel.data();
  blurLineFor(static_cast<size_t>(height), [&](size_t yi) {
    const int32_t y = static_cast<int32_t>(yi);
    for (int32_t c = 0; c < channels; ++c) {
      const float* in = src + static_cast<int64_t>(y) * rowStride + c;
      float* out = midData + static_cast<int64_t>(y) * rowStride + c;
      convolveLine(in, channels, width, kernelData, apron, out, channels);
    }
  });
  blurLineFor(static_cast<size_t>(width), [&](size_t xi) {
    const int32_t x = static_cast<int32_t>(xi);
    for (int32_t c = 0; c < channels; ++c) {
      const float* in = midData + static_cast<int64_t>(x) * channels + c;
      float* out = dst + static_cast<int64_t>(x) * channels + c;
      convolveLine(in, rowStride, height, kernelData, apron, out, rowStride);
    }
  });
}

bool blurTiles(const TileStore& src, const PixelRect& outRect, const BlurParams& p,
               TileStore* dst) {
  // Refusals by name. `dst == &src` is the one worth spelling out: gathering
  // from a store while scattering into it would read texels that had already
  // been replaced by blurred ones, which is a feedback filter rather than a
  // blur and which would produce a *plausible* image -- smoother in one
  // corner, progressively more so along the scan -- rather than an obvious
  // failure.
  if (dst == nullptr || dst == &src) return false;
  if (!blurParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  // **The whole point of ops/Roi, in one line.** The rectangle of source this
  // output needs is the output dilated by the kernel's reach -- walked
  // backwards, from what is wanted to what must be read. Everything below is
  // bookkeeping around this call.
  const PixelRect need = roiBackward(blurRoiOp(p), outRect);
  const int32_t w = need.width();
  const int32_t h = need.height();
  if (w <= 0 || h <= 0) return false;

  // Zero-initialised, which is exactly right: a tile core/TileStore does not
  // hold is transparent black under premultiplied alpha, so an absent tile
  // needs no special case at all -- it is simply not copied in. See
  // ops/Blur.hpp on why the result therefore fades to transparent rather than
  // to black at the edge of the painted region.
  const size_t planeFloats = static_cast<size_t>(w) * static_cast<size_t>(h) *
                             static_cast<size_t>(Tile::kChannels);
  std::vector<float> plane(planeFloats, 0.0f);

  // docs/architecture-review.md P0-3: safe to parallelize directly, no
  // reservation phase needed. This loop only reads `src` (a `const
  // TileStore&`, via `find()`) and writes into `plane`, and every tile's
  // `span` is disjoint from every other tile's -- that's what "tile" means.
  // `TileStoreOf::find()` never mutates the map, so concurrent calls from
  // multiple workers are ordinary concurrent reads of an unmodified
  // container. See core/Parallel.hpp for the one loop in this file (the
  // scatter below) that is NOT this simple.
  const TileRange gatherTiles = roiTileRange(need);
  {
    const int32_t tilesWide = gatherTiles.tilesWide();
    float* const planeData = plane.data();
    parallelFor(static_cast<size_t>(gatherTiles.tileCount()), kParallelForDefaultGrain,
               [&](size_t i) {
                 const int32_t tx = gatherTiles.x0 + static_cast<int32_t>(i) % tilesWide;
                 const int32_t ty = gatherTiles.y0 + static_cast<int32_t>(i) / tilesWide;
                 const TileCoord coord{tx, ty};
                 const Tile* tile = src.find(coord);
                 if (tile == nullptr) return;  // absent: already zero in `plane`
                 const PixelRect span = roiIntersect(roiTileRect(coord), need);
                 const PixelCoord origin = tileOrigin(coord);
                 for (int32_t y = span.y0; y < span.y1; ++y) {
                   for (int32_t x = span.x0; x < span.x1; ++x) {
                     const std::array<float, 4> rgba =
                         tile->readPixel(PixelCoord{x - origin.x, y - origin.y});
                     const size_t base =
                         (static_cast<size_t>(y - need.y0) * static_cast<size_t>(w) +
                          static_cast<size_t>(x - need.x0)) *
                         static_cast<size_t>(Tile::kChannels);
                     planeData[base + 0] = rgba[0];
                     planeData[base + 1] = rgba[1];
                     planeData[base + 2] = rgba[2];
                     planeData[base + 3] = rgba[3];
                   }
                 }
               });
  }

  // Aliased in and out: `blurPlane` holds its own intermediate, so this keeps
  // the peak at two planes rather than three. The numbers in ops/Blur.hpp's
  // apron section are for two.
  blurPlane(plane.data(), w, h, Tile::kChannels, p, plane.data());

  // docs/architecture-review.md P0-3, and the hazard core/Parallel.hpp names:
  // `TileStoreOf::getOrCreate` mutates `dst`'s unordered_map and is not safe
  // to call from two iterations at once. So this is two phases -- serially
  // reserve every destination tile the ROI touches (every `getOrCreate()`
  // call happens here, on one thread), then parallelize the pixel writes
  // over the now-stable slots. The pointers this reservation loop hands out
  // stay valid through the parallel phase below because `TileStoreOf::Slot`
  // is a `std::shared_ptr<T>` -- `getOrCreate()`'s `T&` addresses the tile
  // object the shared_ptr owns, not a map node, so nothing an insertion does
  // to the map's bucket layout can move it (core/TileStore.hpp, and
  // core/Parallel.hpp's file comment).
  const TileRange writeTiles = roiTileRange(outRect);
  struct ReservedTile {
    PixelRect span;
    PixelCoord origin;
    Tile* tile;
  };
  std::vector<ReservedTile> reserved;
  reserved.reserve(static_cast<size_t>(writeTiles.tileCount()));
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& tile = dst->getOrCreate(coord);
      reserved.push_back(ReservedTile{span, tileOrigin(coord), &tile});
    }
  }

  // Phase 2: the pixel work, unchanged from what this loop always did --
  // only who runs which iteration is new. Every `ReservedTile` names a
  // distinct tile (the map cannot hold two slots at one `TileCoord`), so no
  // two parallel iterations can write the same memory.
  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t i) {
    const ReservedTile& r = reserved[i];
    for (int32_t y = r.span.y0; y < r.span.y1; ++y) {
      for (int32_t x = r.span.x0; x < r.span.x1; ++x) {
        const size_t base = (static_cast<size_t>(y - need.y0) * static_cast<size_t>(w) +
                             static_cast<size_t>(x - need.x0)) *
                            static_cast<size_t>(Tile::kChannels);
        r.tile->writePixel(
            PixelCoord{x - r.origin.x, y - r.origin.y},
            {plane[base + 0], plane[base + 1], plane[base + 2], plane[base + 3]});
      }
    }
  });
  return true;
}

bool blurSelfTestChannelVectorMatchesScalar(int32_t n, int32_t apron, uint64_t seed) {
  if (n <= 0 || apron < 0) return false;
#if !NP_BLUR_SIMD_CHANNELS
  // Nothing to compare: this measurement build compiled the fast path out
  // (see this file's top-of-file `NP_BLUR_SIMD_CHANNELS` comment), so there
  // is no `convolveLine4` to check against the scalar path. Trivially true
  // rather than false -- absence of the thing under test is not a failure
  // of it.
  (void)n; (void)apron; (void)seed;
  return true;
#else
  // splitmix64's finalizer -- deterministic, no <random> implementation-
  // defined behaviour to worry about, same generator app/selftest's own
  // blur fixtures use.
  auto noise = [](uint64_t i) noexcept {
    uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
  };

  std::vector<float> kernel(static_cast<size_t>(2 * apron + 1));
  for (size_t i = 0; i < kernel.size(); ++i) kernel[i] = noise(seed + i + 1) * 0.1f;

  std::vector<float> interleaved(static_cast<size_t>(n) * 4);
  for (size_t i = 0; i < interleaved.size(); ++i) interleaved[i] = noise(seed + 1000 + i);

  std::vector<float> wide(interleaved.size(), 0.0f);
  convolveLine4(interleaved.data(), 4, n, kernel.data(), apron, wide.data(), 4);

  // The OLD path: the unmodified scalar convolveLine, called four times at
  // the SAME stride (4) -- exactly what `blurPlane(channels=4, ...)` did
  // before this file's channel-vectorised fast path existed.
  std::vector<float> scalarPath(interleaved.size(), 0.0f);
  for (int32_t c = 0; c < 4; ++c) {
    convolveLine(interleaved.data() + c, 4, n, kernel.data(), apron, scalarPath.data() + c, 4);
  }

  return std::memcmp(wide.data(), scalarPath.data(), wide.size() * sizeof(float)) == 0;
#endif  // NP_BLUR_SIMD_CHANNELS
}

bool blurSelfTestChannelVectorMatchesScalarBox(int32_t n, int32_t radius, uint64_t seed) {
  if (n <= 0 || radius < 0) return false;
#if !NP_BLUR_SIMD_CHANNELS
  (void)n; (void)radius; (void)seed;
  return true;
#else
  auto noise = [](uint64_t i) noexcept {
    uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
  };

  std::vector<float> interleaved(static_cast<size_t>(n) * 4);
  for (size_t i = 0; i < interleaved.size(); ++i) interleaved[i] = noise(seed + 2000 + i);

  std::vector<float> wide(interleaved.size(), 0.0f);
  boxLine4(interleaved.data(), 4, n, radius, wide.data(), 4);

  std::vector<float> scalarPath(interleaved.size(), 0.0f);
  for (int32_t c = 0; c < 4; ++c) {
    boxLine(interleaved.data() + c, 4, n, radius, scalarPath.data() + c, 4);
  }

  return std::memcmp(wide.data(), scalarPath.data(), wide.size() * sizeof(float)) == 0;
#endif  // NP_BLUR_SIMD_CHANNELS
}

}  // namespace np
