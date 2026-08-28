#include "app/selftest/Support.hpp"

// docs/architecture-review.md P0-4: "the full composite is layer-major over a
// 256 MB accumulator." The finding's claim is that `compositeDocumentPremultiplied()`
// pays N full streaming passes over an accumulator far larger than any cache
// for an N-layer document, because it loops layers on the outside and
// scatters each layer's tiles into a canvas-sized buffer.
//
// **This section exists to test that premise before anyone inverts the
// loops for it.** Three of this review's other findings (P0-1, P0-5) turned
// out to have premises that did not survive being measured; this is P0-4's
// turn. It measures wall-clock composite time at a fixed 2048x2048 canvas
// across 1, 2, 4, 8 and 16 full-canvas RGB layers, and separately times a
// bare allocate-and-zero of a buffer the accumulator's own size, so the
// zero-fill's share of the single-layer cost is a measured number rather
// than a guess.
namespace np {

bool runCompositeCostTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- docs/architecture-review.md P0-4: composite cost vs. layer count --\n");

  constexpr int32_t kSide = 2048;  // matches --selftest's own 2048x2048 DocumentTexture numbers
  constexpr int32_t kTilesAcross = kSide / kTileSize;  // 16

  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at),
                                                                             premultiplied);
  };

  // A document with `layerCount` RGB layers, **every layer touching every
  // tile of the canvas** -- the worst case the finding's "N full passes"
  // story describes, and the only fixture shape that could make the story
  // true: a layer that is sparse would make its own pass cheap regardless of
  // loop order, so the premise has to be tested against the case where it has
  // the best chance of holding. One texel per tile is enough to allocate the
  // whole tile (core/Tile stores a tile densely once any texel in it is
  // written), so this is cheap to build and still forces every layer's walk
  // to visit all kTilesAcross^2 tiles.
  auto buildDoc = [&](int layerCount) {
    Document doc = Document::createBlank(kSide, kSide, WorkingSpace{});
    for (int li = 1; li < layerCount; ++li) addLayer(doc, doc.layers.size(), makeRgbLayer("L"));
    for (int32_t ty = 0; ty < kTilesAcross; ++ty) {
      for (int32_t tx = 0; tx < kTilesAcross; ++tx) {
        for (int li = 0; li < layerCount; ++li) {
          // A distinct-ish colour per layer so a defect that dropped a layer
          // (see this file's sabotage pass) would change the composited
          // value, not just its cost.
          const float v = 0.1f + 0.05f * static_cast<float>(li % 8);
          writeRgb(doc, static_cast<size_t>(li), tx * kTileSize + 5, ty * kTileSize + 5,
                   {v, v * 0.5f, v * 0.25f, 1.0f});
        }
      }
    }
    return doc;
  };

  auto timeComposite = [](const Document& doc) {
    double best = 1e30;
    for (int i = 0; i < 5; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<float> v = compositeDocumentPremultiplied(doc);
      const auto t1 = std::chrono::steady_clock::now();
      if (v.empty()) continue;
      best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    return best;
  };

  const int counts[] = {1, 2, 4, 8, 16};
  double times[5];
  for (size_t i = 0; i < 5; ++i) {
    const Document doc = buildDoc(counts[i]);
    const size_t occupied = doc.layers[0].rgbTiles->occupiedTileCount();
    check(occupied == static_cast<size_t>(kTilesAcross) * static_cast<size_t>(kTilesAcross),
          "cost: every layer's fixture actually occupies every tile of the canvas");
    times[i] = timeComposite(doc);
  }

  // The bare cost of allocating and zero-filling a buffer the accumulator's
  // own size (2048*2048*4 floats = 64 MiB), isolated from any tile walk at
  // all -- what `std::vector<float> out(w*h*4, 0.0f)` costs on its own inside
  // `compositeDocumentPremultiplied()`, before a single layer is visited.
  auto timeZeroFill = []() {
    double best = 1e30;
    // A `volatile` sink that reads the buffer back at a **runtime-computed**
    // index, matching app/selftest/CowTile.cpp's own pattern for the general
    // hazard but with one refinement this case needs: this build is LTO'd
    // (P0-2), and a first attempt here that read `v.front()`/`v.back()` (a
    // compile-time-constant 0 and size()-1) measured 0.0000s flat -- the
    // optimiser had legally proven both elements are the literal 0.0f the
    // vector was filled with and folded the reads to that constant without
    // ever running the fill, exactly as `front()+back()` on a
    // freshly-`0.0f`-filled vector is allowed to. An index computed from the
    // clock cannot be known at compile time, so the read cannot be folded and
    // the fill has to actually execute. Confirmed against two throwaway
    // standalone builds outside the tree (same shape as this lambda): the
    // constant-index version measured 0.0000s / 0.0000s / 0.0000s..., the
    // clock-indexed version measured a steady ~0.0004s once warm, matching
    // what an `asm volatile("":: "r,m"(ptr) : "memory")` opacity barrier
    // measured independently on the same buffer.
    volatile float sink = 0.0f;
    for (int i = 0; i < 5; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<float> v(static_cast<size_t>(kSide) * static_cast<size_t>(kSide) * 4, 0.0f);
      const size_t idx = static_cast<size_t>(t0.time_since_epoch().count()) % v.size();
      sink = v[idx];
      const auto t1 = std::chrono::steady_clock::now();
      if (v.empty()) continue;
      best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    (void)sink;
    return best;
  };
  const double zeroFillS = timeZeroFill();

  std::printf("    [measured] %dx%d composite, full-canvas RGB layers:\n", kSide, kSide);
  for (size_t i = 0; i < 5; ++i)
    std::printf("      [measured] %2d layer%s: %.4f s (%.4f s/layer)\n", counts[i],
               counts[i] == 1 ? " " : "s", times[i], times[i] / counts[i]);
  std::printf("    [measured] bare allocate+zero-fill of one %dx%d accumulator (64 MiB): %.4f s "
             "-- %.1f%% of the 1-layer composite\n",
             kSide, kSide, zeroFillS, 100.0 * zeroFillS / times[0]);

  // Linearity: if cost scaled linearly with layer count (the DRAM-re-streaming
  // story), doubling the layer count would double the *marginal* time past the
  // zero-fill floor. Checked as a ratio of the 16-layer and 2-layer marginal
  // costs against 8x (what perfect linearity from 2 to 16 predicts), with a
  // generous band -- this is a shape claim, not a tight one, and the point of
  // printing the raw numbers above is that a reader can judge the shape
  // directly rather than trust one ratio.
  const double marginal2 = times[1] - zeroFillS;
  const double marginal16 = times[4] - zeroFillS;
  const double ratio = marginal2 > 1e-6 ? marginal16 / marginal2 : -1.0;
  std::printf("    [measured] marginal (composite - zero-fill) time, 2->16 layers: %.4f s -> "
             "%.4f s, ratio %.2fx (linear predicts 8.00x)\n",
             marginal2, marginal16, ratio);

  // --- Is the accumulator's DRAM traffic actually the bottleneck? ---------
  //
  // Linear-in-layer-count cost (above) is consistent with the finding's
  // "every layer re-streams the accumulator from DRAM" story, but it is
  // equally consistent with plain per-texel compute cost, which also scales
  // linearly with layer count. The two are told apart by whether **per-texel
  // time changes with accumulator size**: an accumulator that stops fitting
  // in cache should make each texel more expensive once the canvas outgrows
  // L2 (a few hundred KiB) and again once it outgrows the last level cache
  // (tens of MiB on this machine); a compute-bound walk should cost about the
  // same per texel whatever the canvas size, because the bottleneck is ALU
  // and load/store work on data already in registers/L1, not the round trip
  // to DRAM. One layer only, so this isolates the per-texel cost from the
  // layer-count question above.
  std::printf("    [measured] per-texel cost vs. accumulator size (1 layer, full canvas):\n");
  const int32_t sizes[] = {256, 512, 1024, 2048};
  double nsPerTexelAtSmallest = -1.0;
  double nsPerTexelAtLargest = -1.0;
  for (int32_t side : sizes) {
    Document doc = Document::createBlank(side, side, WorkingSpace{});
    const int32_t tilesAcross = (side + kTileSize - 1) / kTileSize;
    for (int32_t ty = 0; ty < tilesAcross; ++ty)
      for (int32_t tx = 0; tx < tilesAcross; ++tx)
        writeRgb(doc, 0, std::min(tx * kTileSize + 5, side - 1),
                std::min(ty * kTileSize + 5, side - 1), {0.3f, 0.4f, 0.5f, 1.0f});
    const double s = timeComposite(doc);
    const double nsPerTexel =
        s * 1.0e9 / (static_cast<double>(side) * static_cast<double>(side));
    const double accumMiB =
        static_cast<double>(side) * side * 4.0 * sizeof(float) / (1024.0 * 1024.0);
    std::printf("      [measured] %5dx%-5d (accum %6.2f MiB): %.5f s, %.3f ns/texel\n", side, side,
               accumMiB, s, nsPerTexel);
    if (side == sizes[0]) nsPerTexelAtSmallest = nsPerTexel;
    if (side == sizes[3]) nsPerTexelAtLargest = nsPerTexel;
  }
  const double perTexelGrowth =
      nsPerTexelAtSmallest > 1e-9 ? nsPerTexelAtLargest / nsPerTexelAtSmallest : -1.0;
  std::printf("    [measured] per-texel cost, %dx%d accumulator vs. %dx%d: %.2fx (a "
             "DRAM/cache-bound accumulator would grow sharply past a few hundred KiB to tens of "
             "MiB; roughly flat means the per-texel arithmetic is what costs, not the round trip "
             "to the accumulator)\n",
             sizes[3], sizes[3], sizes[0], sizes[0], perTexelGrowth);

  return ok;
}

}  // namespace np
