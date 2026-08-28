#include "app/selftest/Support.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

#include "ops/Resample.hpp"
#include "ops/Transform.hpp"

// ops/Resample and ops/Transform, step 0 of the task that added this file:
// **measure before optimising**. Both are per-texel image walks that back
// Image Size, Canvas Size and the interactive transform tool -- paths where
// the user is watching -- and unlike ops/Blur.cpp and the adjustment-layer
// op chain, neither had a single `[measured]` line anywhere in this suite.
// This section is that measurement, kept permanent so the question "does
// this path matter" never again has to be answered by guessing.
//
// Sizes: 1024^2 and 2048^2 (the task's floor), plus a large upscale and a
// large downscale for ops/Transform (resampleAreaAverage refuses to upscale
// at all -- see ops/Resample.hpp's "downscale only" section -- so its own
// asymmetry is covered by a modest vs. a steep downscale instead). Each
// timing is the minimum of 3 runs, the standard way to suppress scheduler/
// thermal noise without hiding a real regression (the same rationale
// core/Parallel.hpp's own header cites for its throwaway benchmarks).
//
// Every number below is printed, not check()-gated, matching this suite's
// documented policy on wall-clock figures (core/Parallel.hpp, "the grain,
// measured rather than guessed"; app/selftest/Parallel.cpp).
namespace np {

namespace {

// splitmix64's finalizer -- the same generator app/selftest/Blur.cpp's
// blurTestNoise() and app/selftest/Parallel.cpp's parallelTestNoise() use,
// for the same reason: a deterministic field with no <random>
// implementation-defined behaviour to worry about.
float perfNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// A straight-alpha RGBA field for resampleAreaAverage(): fully opaque (alpha
// 1.0 everywhere) so the timing is not shaped by how much of the accumulator
// early-outs on zero weight -- ops/Resample.cpp's inner loop has no such
// early-out, but an opaque field is the common case (io/ExportAs resizing a
// flattened document) and is what a caller actually pays for.
std::vector<float> straightField(uint32_t w, uint32_t h) {
  std::vector<float> px(static_cast<size_t>(w) * h * 4u);
  uint64_t counter = 0;
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    const float r = perfNoise(counter++);
    const float g = perfNoise(counter++);
    const float b = perfNoise(counter++);
    px[i * 4 + 0] = r;
    px[i * 4 + 1] = g;
    px[i * 4 + 2] = b;
    px[i * 4 + 3] = 1.0f;
  }
  return px;
}

// A premultiplied RGBA field for transformImage(): same generator, opaque,
// so premultiplied == straight here and the field is a legitimate
// TransformImage without a conversion pass.
TransformImage premultipliedField(uint32_t w, uint32_t h) {
  TransformImage img;
  img.width = w;
  img.height = h;
  img.px = straightField(w, h);
  return img;
}

double timeMs(const std::function<void()>& body) {
  double best = -1.0;
  for (int rep = 0; rep < 3; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    body();
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (best < 0.0 || ms < best) best = ms;
  }
  return best;
}

}  // namespace

bool runResamplePerfTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "  [selftest] resample/transform perf: measuring against PRD F3's 20 ms "
      "pen-to-photon budget, and ops/Blur.cpp's ~232 ms (1024^2, from "
      "app/selftest/Parallel.cpp) as the other reference point\n");

  // ==========================================================================
  // resampleAreaAverage(): 1024^2 and 2048^2 at a modest 2x downscale, plus a
  // steep 8x downscale at each size to expose the horizontal-pass cost (which
  // scales with dstWidth * srcHeight, not dstWidth * dstHeight) separately
  // from the vertical pass.
  // ==========================================================================
  // 4096^2 added on top of the task's 1024^2/2048^2 floor: this codebase's
  // own "4K" canvas (docs/autoflats-migration.md, app/Journal.hpp) is
  // 4096x2160-4096x4096, and Image Size's downscale prefilter is exactly
  // resampleAreaAverage() run on a canvas at that scale.
  for (const uint32_t srcPx : {1024u, 2048u, 4096u}) {
    const std::vector<float> src = straightField(srcPx, srcPx);

    for (const uint32_t dstPx : {srcPx / 2u, srcPx / 8u}) {
      std::vector<float> out;
      std::string err;
      bool accepted = true;
      const double ms = timeMs([&]() {
        accepted = resampleAreaAverage(src.data(), srcPx, srcPx, dstPx, dstPx, &out, &err);
      });
      char what[128];
      std::snprintf(what, sizeof(what), "resampleAreaAverage %ux%u -> %ux%u accepted", srcPx,
                    srcPx, dstPx, dstPx);
      check(accepted, what);
      std::printf("  [measured] resampleAreaAverage %ux%u -> %ux%u (%.1fx downscale): %.3f ms\n",
                  srcPx, srcPx, dstPx, dstPx, static_cast<double>(srcPx) / dstPx, ms);
    }
  }

  // ==========================================================================
  // transformImage(): 1024^2 and 2048^2 for the common transform kinds --
  // a rotation (reconstruction only, no prefilter -- column norms are both
  // 1), an axis-aligned downscale (prefilter + reconstruction, PRD D17), and
  // a large upscale (reconstruction only, from a quarter-size source, which
  // is what "Image Size" going bigger actually runs). All at the default
  // Catmull-Rom kernel, which is what an interactive drag actually uses.
  // ==========================================================================
  TransformParams params;
  params.kernel = ResampleKernel::CatmullRom;

  for (const uint32_t px : {1024u, 2048u}) {
    const TransformImage field = premultipliedField(px, px);

    // Rotation: 17 degrees, off the exact-path snap, no minification (a
    // rotation's column norms are both 1) -- pure reconstruction-pass cost.
    {
      const Mat3 rot = transformRotateDegreesAbout(17.0f, Point2{px * 0.5f, px * 0.5f});
      TransformImage out;
      TransformReport report;
      std::string err;
      bool accepted = true;
      const double ms = timeMs([&]() {
        accepted = transformImage(field, rot, px, px, params, &out, &report, &err);
      });
      char what[128];
      std::snprintf(what, sizeof(what), "transformImage %ux%u rotate 17deg accepted", px, px);
      check(accepted, what);
      check(report.reconstructionPasses == 1 && !report.prefiltered,
            "transformImage rotate: exactly one reconstruction pass, no prefilter (as expected "
            "for a rotation)");
      std::printf("  [measured] transformImage %ux%u rotate 17deg (CatmullRom): %.3f ms\n", px,
                  px, ms);
    }

    // Downscale 4x: exercises the prefilter (resampleAreaAverage internally)
    // plus the one reconstruction pass PRD D16 allows.
    {
      const uint32_t dstPx = px / 4u;
      const Mat3 scale = transformScale(static_cast<float>(dstPx) / static_cast<float>(px),
                                        static_cast<float>(dstPx) / static_cast<float>(px));
      TransformImage out;
      TransformReport report;
      std::string err;
      bool accepted = true;
      const double ms = timeMs([&]() {
        accepted = transformImage(field, scale, dstPx, dstPx, params, &out, &report, &err);
      });
      char what[128];
      std::snprintf(what, sizeof(what), "transformImage %ux%u -> %ux%u downscale accepted", px,
                    px, dstPx, dstPx);
      check(accepted, what);
      std::printf(
          "  [measured] transformImage %ux%u -> %ux%u downscale (CatmullRom, prefiltered): "
          "%.3f ms\n",
          px, px, dstPx, dstPx, ms);
    }

    // Upscale 4x, from a quarter-size source up to `px`: the common "Image
    // Size" enlarge case, pure reconstruction (no prefilter -- magnification
    // never minifies either axis).
    {
      const uint32_t srcPx = px / 4u;
      const TransformImage small = premultipliedField(srcPx, srcPx);
      const Mat3 scale = transformScale(static_cast<float>(px) / static_cast<float>(srcPx),
                                        static_cast<float>(px) / static_cast<float>(srcPx));
      TransformImage out;
      TransformReport report;
      std::string err;
      bool accepted = true;
      const double ms = timeMs([&]() {
        accepted = transformImage(small, scale, px, px, params, &out, &report, &err);
      });
      char what[128];
      std::snprintf(what, sizeof(what), "transformImage %ux%u -> %ux%u upscale accepted", srcPx,
                    srcPx, px, px);
      check(accepted, what);
      check(!report.prefiltered, "transformImage upscale: no prefilter run (magnification only)");
      std::printf(
          "  [measured] transformImage %ux%u -> %ux%u upscale (CatmullRom): %.3f ms\n", srcPx,
          srcPx, px, px, ms);
    }
  }

  return ok;
}

}  // namespace np
