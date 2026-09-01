#include "app/selftest/Support.hpp"

#include "core/Parallel.hpp"

namespace np {

// core/Composite.cpp's tile-parallel walk: `compositeWalk()`'s four tile
// loops (RGB, Pigment, Mix-pair, Adjustment) now dispatch through
// `core::parallelFor()` (core/Parallel.hpp) instead of a plain range-for,
// one worker per tile. This section proves that change changed nothing
// about the RESULT -- only which thread computes it -- with the same
// bit-identity discipline as app/selftest/OpaqueFloor.cpp and
// app/selftest/IncrementalComposite.cpp: every comparison below is
// `std::memcmp`, never a tolerance.
//
// **The correctness hazard this landed alongside, not merely near.** Making
// this walk parallel-safe required a real fix, not just wrapping loops in
// `parallelFor()`: the clip-group fold used to rebind
// `maskTile`/`rgbTile`/`pigmentTile` pointers directly on the SHARED
// `members` vector, once per tile, read back by `foldClipGroup()`
// afterward -- correct only when tiles of one clip base are processed one
// at a time. Two tiles processed concurrently would both rebind the same
// shared fields, an ordering-dependent race. The fix
// (`BoundMember`/`bindMembersForTile()` in core/Composite.cpp) makes that
// binding a fresh, task-local vector every time instead. Section 3 below
// reproduces the ORIGINAL hazard in test-local code (never in production)
// to show the shape of bug this rewrite exists to rule out is real, not
// hypothetical.
bool runCompositeParallelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto writeRgbFull = [](Document& doc, size_t layerIndex, int32_t w, int32_t h,
                         const std::array<float, 4>& premultiplied) {
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex]
            .rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at), premultiplied);
      }
  };
  auto writePigmentFull = [](Document& doc, size_t layerIndex, int32_t w, int32_t h,
                             const PigmentTexel& texel) {
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex]
            .pigmentTiles->getOrCreate(tileCoordAt(at))
            .writeTexel(tileLocalOffset(at), texel);
      }
  };
  auto sameFloats = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };

  // A rich, wide fixture: RGB, Pigment, a clip base with a Multiply member
  // (exercises `bindMembersForTile()`/`foldClipGroup()`), a Mix pair, an
  // Adjustment layer, and a fully-opaque floor layer -- large enough
  // (2048x2048, 256 tiles) to cross any grain from 1 to a few hundred by a
  // wide margin, so a test that stayed under the serial fallback threshold
  // could not accidentally pass by never actually exercising the parallel
  // path.
  constexpr int32_t kW = 2048;
  constexpr int32_t kH = 2048;
  auto buildRichDocument = [&]() {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    writeRgbFull(doc, 0, kW, kH, {0.5f, 0.1f, 0.1f, 0.6f});
    addLayer(doc, doc.layers.size(), makeRgbLayer("clip-base"));
    writeRgbFull(doc, 1, kW, kH, {0.2f, 0.6f, 0.3f, 0.9f});
    addLayer(doc, doc.layers.size(), makeRgbLayer("clip-member"));
    writeRgbFull(doc, 2, kW, kH, {0.8f, 0.8f, 0.1f, 0.5f});
    setLayerBlend(doc, 2, BlendMode::Multiply);
    setLayerClipped(doc, 2, true);
    addLayer(doc, doc.layers.size(), makePigmentLayer("mix-lower"));
    PigmentTexel lo;
    lo.latent.c = {0.4f, 0.2f, 0.1f};
    lo.mass = 0.7f;
    writePigmentFull(doc, 3, kW, kH, lo);
    addLayer(doc, doc.layers.size(), makePigmentLayer("mix-upper"));
    PigmentTexel hi;
    hi.latent.c = {0.1f, 0.4f, 0.2f};
    hi.mass = 0.6f;
    writePigmentFull(doc, 4, kW, kH, hi);
    setLayerBlend(doc, 4, BlendMode::Mix);
    addLayer(doc, doc.layers.size(), makeAdjustmentLayer("adjust"));
    {
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = PointOpKind::Exposure;
      op.exposure.stops = 0.5f;
      doc.layers[5].ops.add(op);
    }
    addLayer(doc, doc.layers.size(), makeRgbLayer("floor"));
    writeRgbFull(doc, 6, kW, kH, {0.9f, 0.3f, 0.1f, 1.0f});  // opaque-floor candidate
    addLayer(doc, doc.layers.size(), makeRgbLayer("above-floor"));
    writeRgbFull(doc, 7, kW, kH, {0.1f, 0.2f, 0.9f, 0.4f});
    return doc;
  };

  std::printf("  -- 1. bit-identity: serial vs. parallel, same document --\n");

  // Forces the serial fallback (a grain larger than the tile count, so
  // `parallelFor()`'s own `n < grain` branch runs) and the genuinely
  // parallel path (grain 1, so every one of the 256 tiles dispatches
  // through `dispatch_apply`) against the SAME document, and compares the
  // premultiplied result byte for byte. A test that stayed under whatever
  // the DEFAULT grain is would never actually exercise `dispatch_apply` at
  // all -- forcing both ends explicitly is what makes this a proof about
  // the parallel path rather than a coincidence of the default.
  {
    const Document doc = buildRichDocument();
    setCompositeParallelGrainForTesting(1'000'000);  // forces serial: n < grain always
    const std::vector<float> serial = compositeDocumentPremultiplied(doc);
    setCompositeParallelGrainForTesting(1);  // forces parallel: every tile dispatches
    const std::vector<float> parallel = compositeDocumentPremultiplied(doc);
    check(sameFloats(serial, parallel),
          "rich fixture (RGB, Pigment, clip base+member, Mix pair, Adjustment, opaque floor): "
          "serial and parallel composites are bit-identical");
  }

  // The identical proof for the incremental (tile-restricted) path, which
  // is the one ui/DocumentTexture actually calls on every frame -- a
  // parallel bug that only showed up compositing the WHOLE canvas and not a
  // dirty subset would be a real gap in this section on its own.
  {
    const Document doc = buildRichDocument();
    std::vector<TileCoord> tiles;
    for (int32_t ty = 0; ty * kTileSize < kH; ++ty)
      for (int32_t tx = 0; tx * kTileSize < kW; ++tx)
        if ((tx + ty) % 2 == 0) tiles.push_back(TileCoord{tx, ty});  // half the tiles, a real subset
    check(tiles.size() > 8, "setup: the incremental fixture actually spans more than 8 tiles");

    auto compositeTiles = [&]() {
      std::vector<float> buffer(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4, 0.0f);
      CompositeRegion region;
      region.pixels = buffer.data();
      region.origin = PixelCoord{0, 0};
      region.width = kW;
      region.height = kH;
      compositeDocumentTilesPremultiplied(doc, tiles, region, nullptr);
      return buffer;
    };
    setCompositeParallelGrainForTesting(1'000'000);
    const std::vector<float> serial = compositeTiles();
    setCompositeParallelGrainForTesting(1);
    const std::vector<float> parallel = compositeTiles();
    check(sameFloats(serial, parallel),
          "incremental composite (half the canvas' tiles): serial and parallel are bit-identical");
  }

  std::printf("  -- 2. repeated parallel runs agree with each other and with serial --\n");

  // `dispatch_apply`'s own scheduling is not something this test controls,
  // so the strongest available proof that no worker races on shared state
  // is running the SAME parallel composite many times and checking every
  // run reaches the identical answer -- a race that depends on which
  // worker happens to run last would be expected to show up as
  // between-run disagreement somewhere across enough repetitions, on a
  // 256-tile document with 16 real cores to schedule across.
  {
    const Document doc = buildRichDocument();
    setCompositeParallelGrainForTesting(1'000'000);
    const std::vector<float> reference = compositeDocumentPremultiplied(doc);
    setCompositeParallelGrainForTesting(1);
    constexpr int kRepeats = 25;
    bool allMatch = true;
    for (int r = 0; r < kRepeats; ++r) {
      if (!sameFloats(compositeDocumentPremultiplied(doc), reference)) {
        allMatch = false;
        break;
      }
    }
    check(allMatch, "25 repeated parallel composites of the same document all agree with "
                    "each other and with the serial reference");
  }

  std::printf("  -- 3. sabotage: the exact hazard this rewrite fixed, reproduced outside "
              "production code --\n");

  // This does NOT patch core/Composite.cpp (which would leave a live defect
  // in the binary this same run's own --selftest has to pass). It
  // reproduces the SHAPE of the bug that used to exist there -- one shared,
  // mutable per-tile binding vector, rebound by whichever `parallelFor`
  // worker reaches a tile, read back afterward -- in a small, self-
  // contained harness local to this test, and shows that shape of bug is
  // real: repeated parallel runs of the NAIVE version disagree with the
  // serial reference, which is exactly the class of failure Section 1 and
  // 2 above exist to catch in the real implementation.
  {
    // A larger tile count (well beyond typical core counts) and a staggered
    // spin width (so workers are NOT all doing identical-length work, which
    // would let a lock-step scheduler accidentally keep them from ever
    // interleaving) both raise the odds this harness actually observes the
    // hazard, without changing what a "pass" means.
    constexpr int32_t kTiles = 4096;
    // The "shared, rebound-per-tile" state the old core/Composite.cpp used
    // to keep on `members`: a single slot, holding whichever tile index
    // last wrote it.
    int sharedBoundTileIndex = -1;
    std::vector<int> naiveResult(static_cast<size_t>(kTiles), -1);
    auto naiveCompositeOnce = [&]() {
      std::fill(naiveResult.begin(), naiveResult.end(), -1);
      parallelFor(static_cast<size_t>(kTiles), 1, [&](size_t idx) {
        // "Bind" the shared slot for this tile...
        sharedBoundTileIndex = static_cast<int>(idx);
        // ...do a little bit of unrelated work, exactly as the real
        // per-texel loop between a bind and a read used to (128x128
        // texels' worth of arithmetic, here stood in for by a spin), so
        // there is a real window for another worker to rebind the shared
        // slot before this one reads it back. The spin width is staggered
        // by `idx` so workers don't all finish in lockstep.
        volatile int spin = 0;
        const int spinFor = 2000 + static_cast<int>(idx % 4000);
        for (int s = 0; s < spinFor; ++s) spin += s;
        (void)spin;
        // ...then read the "binding" back, exactly as `foldClipGroup()`
        // used to read `members[k].rgbTile` after `bindMemberTiles()`.
        naiveResult[idx] = sharedBoundTileIndex;
      });
    };
    // The correct answer, independent of any race: tile `idx`'s own read
    // should see `idx`, because that is what THIS worker itself bound.
    std::vector<int> expected(static_cast<size_t>(kTiles));
    for (int i = 0; i < kTiles; ++i) expected[static_cast<size_t>(i)] = i;

    bool sawDisagreement = false;
    constexpr int kAttempts = 20;
    for (int attempt = 0; attempt < kAttempts && !sawDisagreement; ++attempt) {
      naiveCompositeOnce();
      if (naiveResult != expected) sawDisagreement = true;
    }
    // NOT gated into `ok`: reproducing a real data race within a fixed
    // number of attempts is inherently timing- and scheduler-dependent --
    // failing to reproduce it in a given run is not evidence the hazard is
    // fake, so treating non-reproduction as a hard FAIL would make this
    // whole suite flaky (sometimes red, sometimes green, for a reason that
    // has nothing to do with whether core/Composite.cpp is correct). The
    // actual, deterministic proof that the fix is correct is Sections 1 and
    // 2 above (bit-identical serial-vs-parallel, 25/25 repeated agreement).
    // This section is a best-effort, informational demonstration of WHY the
    // fix was necessary, printed either way.
    std::printf("  %-66s %s\n",
                "sabotage (informational, not gated): naive shared-rebind pattern vs. "
                "real parallelFor scheduling",
                sawDisagreement ? "reproduced" : "did not reproduce");
    if (sawDisagreement) {
      std::printf("    (confirmed: the naive shared-rebind pattern this rewrite replaced "
                  "really does disagree with the correct per-tile answer under real "
                  "parallelFor scheduling -- exactly the hazard BoundMember/"
                  "bindMembersForTile() in core/Composite.cpp exists to rule out)\n");
    } else {
      std::printf("    (did not reproduce in %d attempts on this run -- race reproduction is "
                  "inherently timing-dependent and this is NOT counted as a failure; "
                  "core/Composite.cpp's own fix -- BoundMember, a fresh task-local vector per "
                  "tile -- removes the shared mutable state this harness depends on to fail at "
                  "all, which is the structural argument Sections 1 and 2 above are the "
                  "bit-identical, repeatable proof of)\n",
                  kAttempts);
    }
  }

  std::printf("  -- 4. performance: the same 40-layer/2048x2048 synthetic, serial vs. parallel "
              "--\n");

  // The identical fixture app/selftest/OpaqueFloor.cpp's own perf section
  // uses (a near-full-canvas opaque layer near the top of a 40-layer
  // stack), so the two numbers are directly comparable: that section
  // measures the opaque floor alone (serial), this measures the
  // parallelization ON TOP of it.
  {
    constexpr int32_t kBigW = 2048;
    constexpr int32_t kBigH = 2048;
    constexpr size_t kLayerCount = 40;
    constexpr size_t kFloorAt = 32;
    Document big = Document::createBlank(kBigW, kBigH, WorkingSpace{});
    for (size_t i = 0; i < kLayerCount; ++i) {
      if (i > 0) addLayer(big, big.layers.size(), makeRgbLayer("layer"));
      writeRgbFull(big, i, kBigW, kBigH,
                  {0.1f * static_cast<float>(i % 7), 0.2f, 0.3f, i == kFloorAt ? 1.0f : 0.4f});
    }

    const int kReps = 5;
    setCompositeParallelGrainForTesting(1'000'000);  // serial: isolates the opaque floor alone
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) (void)compositeDocumentPremultiplied(big);
    auto t1 = std::chrono::steady_clock::now();
    const double serialMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;

    setCompositeParallelGrainForTesting(4);  // the shipped default
    auto t2 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) (void)compositeDocumentPremultiplied(big);
    auto t3 = std::chrono::steady_clock::now();
    const double parallelMs = std::chrono::duration<double, std::milli>(t3 - t2).count() / kReps;

    std::printf(
        "    %zu-layer %dx%d, opaque floor at index %zu of %zu: %.2f ms serial (opaque floor "
        "alone) -> %.2f ms with the parallel walk too (%.1fx)\n",
        kLayerCount, kBigW, kBigH, kFloorAt, kLayerCount, serialMs, parallelMs,
        parallelMs > 0.0 ? serialMs / parallelMs : 0.0);
  }

  setCompositeParallelGrainForTesting(4);  // restore the shipped default before returning

  std::printf("[selftest] composite parallel %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
