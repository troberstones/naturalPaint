#include "app/selftest/Support.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/Parallel.hpp"
#include "ops/Blur.hpp"
#include "ops/Filters.hpp"
#include "ops/Roi.hpp"

// docs/architecture-review.md P0-3 ("zero multithreading in an application
// built on an embarrassingly parallel data structure"): core/Parallel.hpp,
// the whole threading layer, and its two consumers -- ops/Blur.cpp's
// blurTiles() and ops/Filters.cpp's scatterAligned()/gatherBlurredPlane()
// (which highpassTiles(), unsharpMaskTiles(), sharpenTiles() and
// localContrastTiles() all hang off). core/Composite.cpp is out of scope --
// docs/architecture-review.md P0-4 is a separate, concurrent restructuring of
// that file, and touching it here would collide with it.
//
// Three things carry this section, in order:
//
//  1. **core::parallelFor itself.** Below the measured grain it runs serial;
//     at or above it, dispatch_apply. Both branches are asserted to compute
//     the exact same values for the exact same body -- the primitive's own
//     determinism claim, checked before trusting anything built on it.
//
//  2. **The strongest claim available, at the op level.** docs/
//     architecture-review.md's own instruction: run a real filter both
//     serially and in parallel over the *same* fixture and diff the output
//     tiles with memcmp, not a tolerance. Neither blurTiles() nor
//     highpassTiles() takes a grain parameter to force one path or the
//     other directly, so this section uses the property every existing
//     "seam" section in app/selftest/Blur.cpp and app/selftest/Filters.cpp
//     already proves exhaustively -- that both functions produce a request-
//     size-independent result -- to get two genuinely different code paths
//     out of the *same* production entry points: a small request (write and
//     gather tile counts both under the grain, so core::parallelFor's own
//     serial branch runs) and a large request covering it (both counts at
//     or above the grain, so the dispatch_apply branch runs). If
//     parallelizing either op's tile loop introduced a defect -- a race on
//     `TileStoreOf::getOrCreate`, a wrong tile-index computation after
//     flattening `ty`/`tx`, anything -- the two would disagree over the
//     region they share, and nothing before this section would have caught
//     it (the existing seam sections predate this change and never varied
//     request size across the grain boundary on purpose).
//
//  3. **Guarding against vacuity.** A comparison between two requests that
//     both happen to fall on the same side of the grain would still pass,
//     for the wrong reason -- it would just be proving the seam property
//     again. So both requests' write-tile AND gather-tile counts are
//     computed independently (via the same public ops/Roi functions the
//     ops themselves call, `roiTileRange`/`roiBackward`/`blurRoiOp`) and
//     checked against `kParallelForDefaultGrain` before the bit-identity
//     comparison is trusted to mean anything.
//
// Also here: the `[measured]` wall-clock numbers docs/architecture-review.md
// asks for -- blur and highpass at 1024^2 and 2048^2, serial vs parallel --
// printed rather than `check()`-gated, matching this suite's own documented
// policy on wall-clock figures (see e.g. runTransformPreviewTextureTest()).
// The "serial" timing again has no direct lever on the production function,
// so it is measured by tiling the identical canvas rectangle into one
// blurTiles()/highpassTiles() call per output tile, summing their wall time,
// against the same rectangle in one call. **This is not a hermetic
// single-threaded baseline and the code says so rather than overclaiming
// one**: each per-tile call's WRITE loop is forced serial (exactly 1 tile,
// well under the grain), but its GATHER loop is not -- dilating a *full*
// tile's boundary by any positive apron spills into the neighbouring tile on
// every side, so even a one-output-tile request needs a 3x3 = 9-tile gather
// neighbourhood at this section's sigma=3 (12px apron), which is already
// over the grain of 8. So "serial" here means "the whole rectangle requested
// one output tile at a time," which is a fair proxy for how an un-
// parallelized caller would actually drive these ops (part 2's bit-identity
// proof above is the one that needs a genuinely all-serial request, and it
// uses a smaller, inset rectangle for exactly that reason). Both timings go
// through the real op, not a reimplementation of it.
namespace np {

namespace {

// splitmix64's finalizer, the same generator app/selftest/Blur.cpp's
// blurTestNoise() uses and for the same reason: a deterministic field with
// no <random> implementation-defined behaviour to worry about matching
// across a bit-identity comparison.
float parallelTestNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// A deterministic, premultiplied, opaque field over a `tilesPerSide` x
// `tilesPerSide` block of tiles anchored at the origin -- big enough that a
// request covering most of it clears the grain by a wide margin in both the
// write loop and the (apron-dilated) gather loop.
TileStore parallelTestField(int32_t tilesPerSide) {
  TileStore tiles;
  uint64_t counter = 0;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty) {
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = parallelTestNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
  return tiles;
}

// Bit-exact comparison over every TEXEL `r` covers -- deliberately not a
// whole-tile memcmp. `smallRect` below is a sub-tile inset (see its own
// comment for why), so the tile it lives in holds real blurred content only
// inside that inset and untouched zero elsewhere; a whole-tile memcmp
// against a request that DID cover that tile's outer margin (`largeRect`)
// would compare bytes neither request was ever asked to agree on and fail
// for a reason that has nothing to do with parallelizing anything. Comparing
// texel-by-texel, only across `r`, is the comparison that actually answers
// "did the serial and parallel paths compute the same request the same
// way" -- raw half-word equality per channel, not readPixel()'s float
// decode, so this cannot be fooled by two halves that happen to decode
// within tolerance of each other. An absent tile on one side and present on
// the other, for a texel `r` claims, is a mismatch, not a skip.
bool tilesBitIdentical(const TileStore& a, const TileStore& b, const PixelRect& r) {
  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const PixelCoord doc{x, y};
      const Tile* ta = a.find(tileCoordAt(doc));
      const Tile* tb = b.find(tileCoordAt(doc));
      if ((ta == nullptr) != (tb == nullptr)) return false;
      if (ta == nullptr) continue;
      const PixelCoord local = tileLocalOffset(doc);
      const size_t base =
          (static_cast<size_t>(local.y) * static_cast<size_t>(kTileSize) +
           static_cast<size_t>(local.x)) *
          static_cast<size_t>(Tile::kChannels);
      if (std::memcmp(ta->data() + base, tb->data() + base,
                      static_cast<size_t>(Tile::kChannels) * sizeof(uint16_t)) != 0) {
        return false;
      }
    }
  }
  return true;
}

// Times `blurTiles(fixture, rect, blur, &fresh)` as one call -- whatever
// code path core::parallelFor takes internally for `rect`'s tile counts.
double timeBlurWhole(const TileStore& fixture, const PixelRect& rect, const BlurParams& blur) {
  TileStore dst;
  const auto t0 = std::chrono::steady_clock::now();
  blurTiles(fixture, rect, blur, &dst);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Times the same rectangle as many individual blurTiles() calls, one per
// output tile -- each with a write-tile count of exactly 1 (well under the
// grain, so that loop's own branch is genuinely serial). The file comment
// above has the honest caveat about the gather loop. Same production
// function, same total pixels, a different call shape.
double timeBlurPerTile(const TileStore& fixture, const PixelRect& rect, const BlurParams& blur) {
  TileStore dst;
  const TileRange range = roiTileRange(rect);
  const auto t0 = std::chrono::steady_clock::now();
  for (int32_t ty = range.y0; ty < range.y1; ++ty) {
    for (int32_t tx = range.x0; tx < range.x1; ++tx) {
      const PixelRect tileRect = roiIntersect(roiTileRect(TileCoord{tx, ty}), rect);
      if (roiIsEmpty(tileRect)) continue;
      blurTiles(fixture, tileRect, blur, &dst);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double timeHighpassWhole(const TileStore& fixture, const PixelRect& rect, const BlurParams& blur) {
  TileStore dst;
  const auto t0 = std::chrono::steady_clock::now();
  highpassTiles(fixture, rect, blur, &dst);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double timeHighpassPerTile(const TileStore& fixture, const PixelRect& rect,
                           const BlurParams& blur) {
  TileStore dst;
  const TileRange range = roiTileRange(rect);
  const auto t0 = std::chrono::steady_clock::now();
  for (int32_t ty = range.y0; ty < range.y1; ++ty) {
    for (int32_t tx = range.x0; tx < range.x1; ++tx) {
      const PixelRect tileRect = roiIntersect(roiTileRect(TileCoord{tx, ty}), rect);
      if (roiIsEmpty(tileRect)) continue;
      highpassTiles(fixture, tileRect, blur, &dst);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

bool runParallelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  [selftest] core::parallelFor grain = %zu (core/Parallel.hpp; measured, see its "
              "own header comment for the two throwaway benchmarks this number came from -- "
              "empty-task dispatch_apply overhead was ~0.01-0.06us/task, and a cheap per-tile "
              "workload's serial-vs-parallel crossover measured at n=3-4 tasks, so 8 carries "
              "about 2x margin over the measured floor)\n",
              kParallelForDefaultGrain);

  // ==========================================================================
  // 1. core::parallelFor itself -- determinism across both branches.
  // ==========================================================================
  {
    constexpr size_t kSmallN = 4;
    std::array<uint64_t, kSmallN> smallOut{};
    parallelFor(kSmallN, kParallelForDefaultGrain, [&](size_t i) { smallOut[i] = i * i * i + 7; });
    bool smallCorrect = true;
    for (size_t i = 0; i < kSmallN; ++i) {
      if (smallOut[i] != i * i * i + 7) smallCorrect = false;
    }
    check(kSmallN < kParallelForDefaultGrain,
          "parallelFor: n=4 is below the grain -- this run took the serial branch");
    check(smallCorrect, "parallelFor: serial branch computes exactly f(i) for every i");

    constexpr size_t kLargeN = 4096;
    std::vector<uint64_t> largeOut(kLargeN, 0);
    std::vector<std::thread::id> threadIds(kLargeN);
    parallelFor(kLargeN, kParallelForDefaultGrain, [&](size_t i) {
      largeOut[i] = i * i * i + 7;
      threadIds[i] = std::this_thread::get_id();
    });
    bool largeCorrect = true;
    for (size_t i = 0; i < kLargeN; ++i) {
      if (largeOut[i] != i * i * i + 7) largeCorrect = false;
    }
    check(kLargeN >= kParallelForDefaultGrain,
          "parallelFor: n=4096 is at/above the grain -- this run took the dispatch_apply branch");
    check(largeCorrect,
          "parallelFor: dispatch_apply branch computes exactly f(i) for every i, same as serial");

    // Informational only -- NOT check()-gated. Which/how many OS threads
    // libdispatch actually hands the work to is a scheduler decision, not
    // this header's contract (parallelFor's correctness claim is about
    // VALUES, not about how many threads produced them), and gating a
    // build-green result on thread-count diversity would make this section
    // flake on a loaded or single-core CI sandbox for a property nothing
    // here actually needs.
    const std::unordered_set<std::thread::id> distinctThreads(threadIds.begin(), threadIds.end());
    std::printf("  [measured] parallelFor over n=%zu touched %zu distinct OS thread(s)\n", kLargeN,
                distinctThreads.size());
  }

  // ==========================================================================
  // 2. Op-level proof: blurTiles() and highpassTiles(), serial-path output
  //    vs parallel-path output, bit-identical over the same fixture.
  // ==========================================================================
  const int32_t fixtureTilesPerSide = 20;  // 2560x2560px, 400 tiles
  const int32_t fixturePx = fixtureTilesPerSide * kTileSize;
  const TileStore fixture = parallelTestField(fixtureTilesPerSide);

  BlurParams blur;
  blur.kind = BlurKind::Gaussian;
  blur.sigma = 3.0f;  // modest: keeps the small request's gather apron from
                      // spilling across enough extra tiles to accidentally
                      // cross the grain itself

  // Deep inside the fixture on both sides, so neither request's gather apron
  // runs off the fixture's edge (which would just read absent tiles as
  // zero, not wrong, but would make the tile counts below harder to reason
  // about by hand).
  const PixelRect largeRect{64, 64, fixturePx - 64, fixturePx - 64};

  // **Not a whole tile.** blurApron(sigma=3) is 12px, and dilating a
  // request by any positive apron always spills into the neighbouring tile
  // on every side it touches -- so a request spanning exactly one full tile
  // still needs a 3x3 gather range (9 tiles), which is already over this
  // header's grain of 8 and would make "small" request just as parallel as
  // "large". Inset 16px into the tile on every side (4px of margin beyond
  // the 12px apron) keeps the dilated gather rectangle entirely inside the
  // one tile it starts in, so both the write loop AND the gather loop see
  // exactly 1 tile -- genuinely below the grain, not accidentally close to
  // it.
  //
  // **Several probe tiles, not one, and the reason is a sabotage that got
  // through.** This section originally compared exactly one such inset rect,
  // in the tile at the fixture's centre. Sabotaging `parallelFor`'s
  // dispatch_apply branch to run only even indices -- a total break of the
  // parallel path -- left BOTH bit-identity assertions below green, because
  // the flattened index of that one tile (10*20 + 10 = 210) happens to be
  // even, so the one tile the comparison looked at was the one the sabotage
  // did not touch. It was caught only by app/selftest/Blur.cpp's older seam
  // assertions, which is detection by luck of a neighbour rather than by
  // this section doing its job. A single-tile probe cannot distinguish "the
  // parallel path is correct" from "the parallel path happened to be correct
  // on the one tile I sampled"; probing a run of ADJACENT tiles covers both
  // flat-index parities and, more generally, several different rows and
  // columns of the flattening -- which is what a wrong `%`/`/`, a dropped
  // task, or a skipped index actually perturbs.
  const int32_t tileOriginPx = fixturePx / 2;  // exact multiple of kTileSize
  const PixelRect smallRect{tileOriginPx + 16, tileOriginPx + 16, tileOriginPx + kTileSize - 16,
                            tileOriginPx + kTileSize - 16};
  // Four probes: (10,10), (11,10), (10,11), (11,11) in tile coordinates --
  // flat indices 210, 211, 230, 231 over the large request's 20-wide range,
  // so two even and two odd, and two distinct rows of the flattening.
  const std::array<PixelRect, 4> probeRects{
      smallRect,
      PixelRect{smallRect.x0 + kTileSize, smallRect.y0, smallRect.x1 + kTileSize, smallRect.y1},
      PixelRect{smallRect.x0, smallRect.y0 + kTileSize, smallRect.x1, smallRect.y1 + kTileSize},
      PixelRect{smallRect.x0 + kTileSize, smallRect.y0 + kTileSize, smallRect.x1 + kTileSize,
                smallRect.y1 + kTileSize},
  };

  // Vacuity guard: computed independently via the exact public ops/Roi
  // functions blurTiles()/highpassTiles() use internally to size their own
  // gather and write loops, so this is checking the real branch each
  // request takes, not assuming it.
  const int64_t largeWriteTiles = roiTileRange(largeRect).tileCount();
  const int64_t largeGatherTiles = roiTileRange(roiBackward(blurRoiOp(blur), largeRect)).tileCount();
  const int64_t smallWriteTiles = roiTileRange(smallRect).tileCount();
  const int64_t smallGatherTiles = roiTileRange(roiBackward(blurRoiOp(blur), smallRect)).tileCount();
  std::printf(
      "  [selftest] parallel proof tile counts (grain=%zu): large request %lld write / %lld "
      "gather tiles, small request %lld write / %lld gather tiles\n",
      kParallelForDefaultGrain, static_cast<long long>(largeWriteTiles),
      static_cast<long long>(largeGatherTiles), static_cast<long long>(smallWriteTiles),
      static_cast<long long>(smallGatherTiles));
  check(largeWriteTiles >= static_cast<int64_t>(kParallelForDefaultGrain) &&
            largeGatherTiles >= static_cast<int64_t>(kParallelForDefaultGrain),
        "parallel proof: the large request's write AND gather loops both clear the grain -- "
        "this run is genuinely parallel, not vacuously serial");
  check(smallWriteTiles < static_cast<int64_t>(kParallelForDefaultGrain) &&
            smallGatherTiles < static_cast<int64_t>(kParallelForDefaultGrain),
        "parallel proof: the small request's write AND gather loops both stay under the grain -- "
        "this run is genuinely serial, the baseline to compare against");

  // Every probe is checked for the grain property in its own right, not just
  // the first: an inset rect one tile over must still be a 1-write/1-gather
  // request, or the "serial baseline" it provides is not serial.
  bool everyProbeSerial = true;
  for (const PixelRect& probe : probeRects) {
    if (roiTileRange(probe).tileCount() >= static_cast<int64_t>(kParallelForDefaultGrain) ||
        roiTileRange(roiBackward(blurRoiOp(blur), probe)).tileCount() >=
            static_cast<int64_t>(kParallelForDefaultGrain)) {
      everyProbeSerial = false;
    }
  }
  check(everyProbeSerial,
        "parallel proof: all four probe rects stay under the grain in both loops, so each is a "
        "genuinely serial baseline and not just the first one");

  TileStore blurLarge;
  check(blurTiles(fixture, largeRect, blur, &blurLarge),
        "blur: large (parallel-path) request accepted");
  bool blurProbesAccepted = true, blurProbesIdentical = true;
  for (const PixelRect& probe : probeRects) {
    TileStore blurSmall;
    if (!blurTiles(fixture, probe, blur, &blurSmall)) blurProbesAccepted = false;
    if (!tilesBitIdentical(blurLarge, blurSmall, probe)) blurProbesIdentical = false;
  }
  check(blurProbesAccepted, "blur: all four small (serial-path) probe requests accepted");
  check(blurProbesIdentical,
        "blur: serial-path and parallel-path outputs are bit-identical over all four probes -- "
        "the strongest available proof that parallelizing blurTiles() introduced no defect");

  TileStore highpassLarge;
  check(highpassTiles(fixture, largeRect, blur, &highpassLarge),
        "highpass: large (parallel-path) request accepted");
  bool hpProbesAccepted = true, hpProbesIdentical = true;
  for (const PixelRect& probe : probeRects) {
    TileStore highpassSmall;
    if (!highpassTiles(fixture, probe, blur, &highpassSmall)) hpProbesAccepted = false;
    if (!tilesBitIdentical(highpassLarge, highpassSmall, probe)) hpProbesIdentical = false;
  }
  check(hpProbesAccepted, "highpass: all four small (serial-path) probe requests accepted");
  check(hpProbesIdentical,
        "highpass: serial-path and parallel-path outputs are bit-identical over all four probes "
        "-- proves scatterAligned()'s two-phase reservation introduced no defect");

  // ==========================================================================
  // 3. Measured wall-clock: blur and highpass, 1024^2 and 2048^2, serial vs
  //    parallel. Printed, not check()-gated -- wall-clock is this suite's
  //    own documented flake class.
  // ==========================================================================
  for (const int32_t sizePx : {1024, 2048}) {
    const PixelRect rect{0, 0, sizePx, sizePx};
    const double blurParallelMs = timeBlurWhole(fixture, rect, blur);
    const double blurSerialMs = timeBlurPerTile(fixture, rect, blur);
    std::printf(
        "  [measured] blur %dx%d: serial(per-tile) %.3f ms, parallel(whole) %.3f ms, %.2fx\n",
        sizePx, sizePx, blurSerialMs, blurParallelMs, blurSerialMs / blurParallelMs);

    const double highpassParallelMs = timeHighpassWhole(fixture, rect, blur);
    const double highpassSerialMs = timeHighpassPerTile(fixture, rect, blur);
    std::printf(
        "  [measured] highpass %dx%d: serial(per-tile) %.3f ms, parallel(whole) %.3f ms, %.2fx\n",
        sizePx, sizePx, highpassSerialMs, highpassParallelMs, highpassSerialMs / highpassParallelMs);
  }

  return ok;
}

}  // namespace np
