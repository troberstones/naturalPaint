#include "app/selftest/Support.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

#include "core/Parallel.hpp"
#include "ops/Blur.hpp"
#include "ops/Filters.hpp"
#include "ops/Roi.hpp"

// ops/Blur.cpp's two remaining CPU optimisations over `blurPlane()` itself
// (docs/architecture-review.md P0-3 threaded blurTiles()'s tile gather and
// scatter loops but deliberately left blurPlane()'s own arithmetic
// single-threaded and scalar -- that is this section's subject):
//
//   1. Threading the separable passes' row loop (horizontal) and column loop
//      (vertical) via core::parallelFor, one level below blurTiles()'s
//      already-threaded tile loops.
//   2. Vectorising across the four RGBA CHANNELS of one texel rather than
//      across the kernel's TAPS -- ops/Blur.cpp's file comment above
//      `convolveLine4`/`boxLine4` has the full argument for why the axis
//      matters: channel-vectorisation cannot reorder a single channel's own
//      float sum, so it is bit-identical to the scalar per-channel loop,
//      while tap-vectorisation would reorder additions and change the low
//      bits this file's separability/bit-identity assertions in
//      app/selftest/Blur.cpp depend on.
//
// This section's job is to prove the second claim directly (the first is
// already proven at the op level by app/selftest/Parallel.cpp's bit-identity
// comparison across the grain boundary, which exercises `blurPlane` through
// `blurTiles` either way) and to print the wall-clock numbers PRD F3 and
// docs/architecture-review.md ask for.
namespace np {

namespace {

// splitmix64's finalizer -- the same generator app/selftest/Blur.cpp's
// blurTestNoise() and app/selftest/Parallel.cpp's parallelTestNoise() use,
// and for the same reason: a deterministic field with no <random>
// implementation-defined behaviour for a bit-identity comparison to trip on.
float blurSimdNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// A deterministic, premultiplied, opaque field over a `tilesPerSide` x
// `tilesPerSide` block of tiles anchored at the origin, the same fixture
// shape app/selftest/Parallel.cpp uses so the wall-clock numbers below are
// directly comparable to that section's.
TileStore blurSimdField(int32_t tilesPerSide) {
  TileStore tiles;
  uint64_t counter = 0;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty) {
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = blurSimdNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
  return tiles;
}

double timeBlurTilesWhole(const TileStore& fixture, const PixelRect& rect,
                          const BlurParams& blur) {
  TileStore dst;
  const auto t0 = std::chrono::steady_clock::now();
  blurTiles(fixture, rect, blur, &dst);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double timeHighpassTilesWhole(const TileStore& fixture, const PixelRect& rect,
                              const BlurParams& blur) {
  TileStore dst;
  const auto t0 = std::chrono::steady_clock::now();
  highpassTiles(fixture, rect, blur, &dst);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

bool runBlurSimdTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // 1. The load-bearing claim: channel-vectorised == scalar per-channel,
  //    BIT FOR BIT, not merely close -- at the stride production code
  //    actually uses.
  // ==========================================================================
  //
  // **Why not just call `blurPlane` twice, once with channels=4 and once
  // with channels=1, and diff the results?** That was this section's first
  // draft, and it turned up a real ~1-ULP disagreement on about a third of
  // texels -- which looked exactly like the tap-axis reordering this file's
  // whole discipline exists to avoid, until `ops/Blur.hpp`'s
  // `blurSelfTestChannelVectorMatchesScalar` comment (and the standalone
  // repro that found it) traced it to something else entirely: the scalar
  // `convolveLine`, called through `channels=1`, runs at a CONTIGUOUS stride
  // and the compiler autovectorises it across TAPS there (separate
  // `fmul`/`fadd`, several roundings) -- a materially different rounding
  // path from `channels=4`'s real stride (4, strided, one `fmla` per tap).
  // The disagreement was between two rounding paths that were never claimed
  // to agree, not evidence the channel-vectorised path reordered anything.
  //
  // So this checks the actual claim, at the actual stride, via ops/Blur.hpp's
  // testing hook: `convolveLine4`/`boxLine4` (stride 4) against the
  // UNMODIFIED scalar `convolveLine`/`boxLine`, called four times at that
  // SAME stride 4 -- exactly what `blurPlane(channels=4, ...)` did before
  // this optimisation existed. `std::memcmp`, not a tolerance: with both
  // sides at the same stride there is no rounding-path difference left to
  // have a tolerance for.
  {
    for (int32_t apron : {0, 1, 8, 32, 130}) {  // 0: single tap; 130: wider
                                                // than any one gather tile
      check(blurSelfTestChannelVectorMatchesScalar(400, apron, 0xa11ceULL + static_cast<uint64_t>(apron)),
            "blur simd: convolveLine4 (channels=4, real stride) matches four calls of the "
            "unmodified scalar convolveLine at the SAME stride, BIT FOR BIT");
    }
    for (int32_t radius : {0, 1, 5, 64}) {
      check(blurSelfTestChannelVectorMatchesScalarBox(400, radius,
                                                       0xb0bULL + static_cast<uint64_t>(radius)),
            "blur simd: boxLine4 (channels=4, real stride) matches four calls of the "
            "unmodified scalar boxLine at the SAME stride, BIT FOR BIT");
    }
  }

  // ==========================================================================
  // 2. blurApron(0 (identity) and channels other than 1/4 still work -- the
  //    fast path is an ADDITIONAL branch, not a replacement that narrowed
  //    what this function accepts.
  // ==========================================================================
  {
    std::vector<float> three(static_cast<size_t>(9) * 9 * 3, 0.5f);
    std::vector<float> threeOut(three.size(), 0.0f);
    BlurParams p;
    p.sigma = 2.0f;
    blurPlane(three.data(), 9, 9, 3, p, threeOut.data());
    bool flat = true;
    const int32_t a = blurApron(p);
    for (int32_t y = a; y < 9 - a; ++y) {
      for (int32_t x = a; x < 9 - a; ++x) {
        for (int32_t c = 0; c < 3; ++c) {
          const size_t idx = (static_cast<size_t>(y) * 9 + static_cast<size_t>(x)) * 3 +
                             static_cast<size_t>(c);
          if (threeOut[idx] != 0.5f) flat = false;
        }
      }
    }
    check(flat,
          "blur simd: channels=3 (neither the scalar test's 1 nor the fast path's 4) still "
          "takes the generic scalar loop and blurs a flat field to itself -- the channels==4 "
          "branch is additive");
  }

  // ==========================================================================
  // 3. Wall-clock: blurTiles() and highpassTiles(), 1024^2 and 2048^2, at
  //    PRD F3's own preview sigma (8.0, ui/MacPaintUI.cpp's dialog default,
  //    the same sigma app/selftest/FilterMenu.cpp's preview-cost section
  //    times). Printed, not check()-gated -- wall-clock is this suite's own
  //    documented flake class (see e.g. app/selftest/Parallel.cpp).
  // ==========================================================================
  {
    const int32_t fixtureTilesPerSide = 20;  // 2560x2560px, matches
                                             // app/selftest/Parallel.cpp's
                                             // fixture so both sections'
                                             // numbers describe the same
                                             // amount of source content
    const TileStore fixture = blurSimdField(fixtureTilesPerSide);

    BlurParams blur;
    blur.kind = BlurKind::Gaussian;
    blur.sigma = 8.0f;  // ui/MacPaintUI.cpp's own dialog default

    for (const int32_t sizePx : {1024, 2048}) {
      const PixelRect rect{0, 0, sizePx, sizePx};
      const double blurMs = timeBlurTilesWhole(fixture, rect, blur);
      std::printf("  [measured] blur %dx%d (sigma=8, blurTiles whole-region): %.3f ms\n", sizePx,
                  sizePx, blurMs);
      const double highpassMs = timeHighpassTilesWhole(fixture, rect, blur);
      std::printf("  [measured] highpass %dx%d (sigma=8, highpassTiles whole-region): %.3f ms\n",
                  sizePx, sizePx, highpassMs);
    }
  }

  return ok;
}

}  // namespace np
