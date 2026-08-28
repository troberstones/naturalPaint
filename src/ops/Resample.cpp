#include "ops/Resample.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Parallel.hpp"
#include "core/Premultiply.hpp"

namespace np {
namespace {

// One axis's resampling plan: for each destination index, which contiguous
// run of source indices its footprint covers and with what weights.
//
// Precomputed once per axis rather than recomputed per row/column -- the
// weights depend only on the two extents, so computing them inside the pixel
// loop would repeat identical arithmetic srcHeight times.
//
// Weights are **normalised to sum to 1** per destination index. Normalising
// here rather than dividing by the footprint width at the end matters at the
// image's last destination texel, whose footprint can be clipped by
// floating-point rounding of `dstN * scale` against `srcN`: a plain
// divide-by-scale would then darken (or lighten) the final row or column by
// whatever fraction went missing, which is the classic one-pixel-dark-edge
// resize bug.
//
// They are kept in **double**, and that is load-bearing rather than
// cautious. Normalised float weights sum to 1 only to within about
// n * 6e-8, so an 8x reduction (64 weights per axis) leaves a fully opaque
// image with an alpha of ~0.999996 -- and io/Export refuses a JPEG or HDR
// export of anything whose alpha is below 1.0 by name. A resize would then
// have silently made an opaque document un-exportable to JPEG. In double the
// same sum lands within ~1e-14 of 1, which rounds to exactly 1.0f on the way
// into the float buffer, so a constant-valued region survives a downscale
// bit-exactly (--selftest asserts precisely that, at zero tolerance). The
// plan is a few thousand doubles at most -- it is per *axis*, not per pixel.
struct AxisPlan {
  std::vector<uint32_t> first;  // per destination index: first source index
  std::vector<uint32_t> count;  // per destination index: how many source indices
  std::vector<size_t> offset;   // per destination index: start in `weight`
  std::vector<double> weight;   // flattened, sums to 1 per destination index
};

AxisPlan buildAxisPlan(uint32_t srcN, uint32_t dstN) {
  AxisPlan plan;
  plan.first.resize(dstN);
  plan.count.resize(dstN);
  plan.offset.resize(dstN);

  const double scale = static_cast<double>(srcN) / static_cast<double>(dstN);
  // Every footprint is `scale` source texels wide, so the weight run is at
  // most ceil(scale) + 1 long (a footprint that starts and ends mid-texel
  // touches one partial texel at each end).
  plan.weight.reserve(static_cast<size_t>(dstN) *
                      (static_cast<size_t>(std::ceil(scale)) + 1u));

  for (uint32_t i = 0; i < dstN; ++i) {
    // The destination texel's footprint in source coordinates, as a
    // half-open interval. Source texel j covers [j, j+1).
    const double x0 = static_cast<double>(i) * scale;
    const double x1 = std::min(static_cast<double>(i + 1) * scale, static_cast<double>(srcN));

    uint32_t j0 = static_cast<uint32_t>(std::floor(x0));
    if (j0 >= srcN) j0 = srcN - 1;  // only reachable via rounding at the last texel
    uint32_t j1 = static_cast<uint32_t>(std::ceil(x1));
    if (j1 <= j0) j1 = j0 + 1;
    if (j1 > srcN) j1 = srcN;

    plan.first[i] = j0;
    plan.count[i] = j1 - j0;
    plan.offset[i] = plan.weight.size();

    double total = 0.0;
    const size_t base = plan.weight.size();
    for (uint32_t j = j0; j < j1; ++j) {
      // Overlap length between the footprint [x0,x1) and source texel
      // [j,j+1). This is the exact area weight -- what makes a non-integer
      // scale factor correct rather than approximately correct.
      const double overlap = std::min(x1, static_cast<double>(j + 1)) -
                             std::max(x0, static_cast<double>(j));
      const double w = overlap > 0.0 ? overlap : 0.0;
      total += w;
      plan.weight.push_back(w);
    }
    if (total > 0.0) {
      const double inv = 1.0 / total;
      for (size_t k = base; k < plan.weight.size(); ++k) plan.weight[k] *= inv;
    }
  }
  return plan;
}

bool fail(std::string* errorOut, std::vector<float>* out, std::string message) {
  if (out) out->clear();
  if (errorOut) *errorOut = std::move(message);
  return false;
}

}  // namespace

bool resampleAreaAverage(const float* src, uint32_t srcWidth, uint32_t srcHeight,
                         uint32_t dstWidth, uint32_t dstHeight, std::vector<float>* out,
                         std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) {
    if (errorOut)
      *errorOut = "resize refused: no destination buffer was supplied (internal error).";
    return false;
  }
  out->clear();
  if (src == nullptr)
    return fail(errorOut, out, "resize refused: no source pixels were supplied (internal error).");
  if (srcWidth == 0 || srcHeight == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "resize refused: the source image is %ux%u -- there is nothing to resample.",
                  srcWidth, srcHeight);
    return fail(errorOut, out, buf);
  }
  if (dstWidth == 0 || dstHeight == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "resize refused: the requested size is %ux%u; both dimensions must be at "
                  "least 1 pixel.",
                  dstWidth, dstHeight);
    return fail(errorOut, out, buf);
  }
  if (dstWidth > srcWidth || dstHeight > srcHeight) {
    // See ops/Resample.hpp's "downscale only" section. Named in full --
    // which axis grew, by how much, and what to do instead -- rather than a
    // bare "unsupported", the same refusal style io/Export.cpp uses.
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "resize refused: %ux%u -> %ux%u would enlarge the image (%s), and this build "
                  "resamples downwards only. Upscaling is decided entirely by the choice of "
                  "reconstruction filter (bilinear, bicubic, Catmull-Rom, Lanczos3), which is "
                  "PLAN.md phase 6's transform op to offer -- enlarging here would invent "
                  "detail with a filter nobody chose and label it as the document's. Pick a "
                  "size no larger than %ux%u.",
                  srcWidth, srcHeight, dstWidth, dstHeight,
                  (dstWidth > srcWidth && dstHeight > srcHeight) ? "both axes"
                  : (dstWidth > srcWidth)                        ? "the width"
                                                                 : "the height",
                  srcWidth, srcHeight);
    return fail(errorOut, out, buf);
  }

  const size_t dstSamples =
      static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4u;

  // 1:1 is not a resize. Copied verbatim so a "resize" that changes nothing
  // cannot perturb a value by the ulp that premultiplying and then
  // un-premultiplying by a non-power-of-two alpha would cost.
  if (dstWidth == srcWidth && dstHeight == srcHeight) {
    out->assign(src, src + dstSamples);
    return true;
  }

  const AxisPlan xPlan = buildAxisPlan(srcWidth, dstWidth);
  const AxisPlan yPlan = buildAxisPlan(srcHeight, dstHeight);

  // --- Horizontal pass: srcWidth x srcHeight -> dstWidth x srcHeight -------
  //
  // Premultiplication happens here, on the fly, so no second full-size buffer
  // is needed to hold a premultiplied copy of the source. The intermediate
  // stays premultiplied; un-premultiplication happens once, at the very end.
  //
  // Accumulated in double. Not superstition: a 4096-wide source reduced to
  // 64 sums 64 products per output sample, and the destination of an export
  // may be a 32-bit float file, so the accumulation should not be the
  // coarsest stage in the pipeline. The cost is confined to the accumulator
  // -- both buffers stay float.
  //
  // **Threaded over `y`, one row per task.** Every row reads only its own
  // slice of `src` and writes only its own slice of `rows` -- no row touches
  // another's memory, so this is the safe axis core/Parallel.hpp describes:
  // threading over independent rows preserves each output texel's own
  // accumulation order exactly (the `k` loop inside a row runs in the same
  // order it always did; only which CPU core runs which row's iteration is
  // new), so the result is bit-identical to the serial version by
  // construction. There is no `TileStoreOf` here -- `rows` and `out` are
  // flat `std::vector<float>` -- so none of core/Parallel.hpp's
  // `getOrCreate()` hazard applies; every row's destination slice already
  // exists (`rows` is sized once, above, before this loop starts).
  //
  // **Grain: `core::kParallelForDefaultGrain` (8), re-measured for this
  // shape rather than assumed.** core/Parallel.hpp's own 8 was measured
  // against a *tile*-sized work item (128x128x4 floats, ~6.2us/task
  // serial). A resample row is a different, much more variable size of
  // work: a throwaway benchmark (row width 256..4096, taps 3 and 9 --
  // spanning a mild 2x-ish downscale through a steep one, best-of-5,
  // `parallelFor(n, 1, ...)` to force the parallel branch at every `n`)
  // measured serial-per-row costs from ~0.4us (256-wide, 3 taps) up to
  // ~19us (4096-wide, 9 taps) -- i.e. some real rows are *cheaper* than the
  // tile floor 8 was tuned against, and some are pricier. The crossover
  // moved with it: ~n=2-3 for the expensive rows (already 4x-6x by n=8),
  // but the cheapest row shape never broke past ~1.0x even out to n=64 --
  // dispatch overhead is not costing anything there (consistent with this
  // header's own "0.01-0.06us/task" dispatch-overhead finding), there just
  // isn't enough work per row to show a win. That combination is exactly
  // why 8 stays the right choice rather than a lower one tuned to the
  // expensive rows: real `srcHeight`/`dstHeight` values worth optimising
  // for (1024, 2048, 4096) are always far larger than 8 regardless of which
  // row shape they are, so the grain only controls behaviour in the regime
  // where total wall-clock cost is already sub-millisecond and invisible
  // either way (a thumbnail-sized resize with under 8 rows). Measured, not
  // guessed -- and the number that was actually measured is 8.
  std::vector<float> rows(static_cast<size_t>(dstWidth) * static_cast<size_t>(srcHeight) * 4u);
  parallelFor(srcHeight, kParallelForDefaultGrain, [&](size_t yIdx) {
    const uint32_t y = static_cast<uint32_t>(yIdx);
    const float* srcRow = src + static_cast<size_t>(y) * srcWidth * 4u;
    float* dstRow = rows.data() + static_cast<size_t>(y) * dstWidth * 4u;
    for (uint32_t x = 0; x < dstWidth; ++x) {
      double acc[4] = {0.0, 0.0, 0.0, 0.0};
      const uint32_t j0 = xPlan.first[x];
      const uint32_t n = xPlan.count[x];
      const double* w = xPlan.weight.data() + xPlan.offset[x];
      for (uint32_t k = 0; k < n; ++k) {
        const float* p = srcRow + static_cast<size_t>(j0 + k) * 4u;
        const double weight = w[k];
        const double a = static_cast<double>(p[3]);
        acc[0] += weight * static_cast<double>(p[0]) * a;
        acc[1] += weight * static_cast<double>(p[1]) * a;
        acc[2] += weight * static_cast<double>(p[2]) * a;
        acc[3] += weight * a;
      }
      float* d = dstRow + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 4; ++c) d[c] = static_cast<float>(acc[c]);
    }
  });

  // --- Vertical pass: dstWidth x srcHeight -> dstWidth x dstHeight ---------
  // Threaded the same way and for the same reason: each destination row `y`
  // reads only `rows` (untouched by this pass, read-only here) and writes
  // only its own slice of `out`.
  out->resize(dstSamples);
  parallelFor(dstHeight, kParallelForDefaultGrain, [&](size_t yIdx) {
    const uint32_t y = static_cast<uint32_t>(yIdx);
    const uint32_t i0 = yPlan.first[y];
    const uint32_t n = yPlan.count[y];
    const double* w = yPlan.weight.data() + yPlan.offset[y];
    float* dstRow = out->data() + static_cast<size_t>(y) * dstWidth * 4u;
    for (uint32_t x = 0; x < dstWidth; ++x) {
      std::array<double, 4> acc{0.0, 0.0, 0.0, 0.0};
      for (uint32_t k = 0; k < n; ++k) {
        const float* p =
            rows.data() + (static_cast<size_t>(i0 + k) * dstWidth + x) * 4u;
        const double weight = w[k];
        // Vectorised across the four **channels**, not across taps: each
        // channel keeps its own independent double accumulation in its own
        // lane, so this stays bit-exact under SIMD (four parallel scalar
        // adds, not a reassociated horizontal sum). Reordering the `k`
        // (tap) loop instead would change which additions happen in which
        // order within a single channel's sum -- that is the reordering
        // core/Parallel.hpp and this file's own task both forbid.
        for (int c = 0; c < 4; ++c) acc[c] += weight * static_cast<double>(p[c]);
      }
      // core/Premultiply's shared `a <= 0 -> {0,0,0,0}` guard. The promotion
      // this call site's own comment said was "now due" for two steps has
      // happened; the header carries the argument, including why it is a
      // template. **`acc` stays double through the divide**: that is this
      // caller's whole reason for needing a T, and narrowing to float here
      // would undo the double-weight decision documented at the top of this
      // file. The narrowing happens once, on the way to storage, below.
      const std::array<double, 4> straight = unpremultiply(acc);
      float* d = dstRow + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 4; ++c) d[c] = static_cast<float>(straight[c]);
    }
  });
  return true;
}

}  // namespace np
