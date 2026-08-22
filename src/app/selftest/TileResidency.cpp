#include "app/selftest/Support.hpp"

namespace np {

bool runTileResidencyTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Residency is not a feature, it is a strategy: Eager is complete and
  // correct on its own, and the cached strategy below it is an addition.

  // --- Thresholds, derived from phase 1's measured latency ---------------
  //
  // There is no tolerance in this section: every pixel comparison below is at
  // **zero tolerance**, for exactly io/NpaintFile.hpp's reason. A cached
  // fetch asks OpenImageIO for TypeDesc::HALF and receives the file's own
  // half words; the eager path memcpy'd those same words out of the same
  // file. No float appears on either side, so a difference of one ulp is a
  // bug, not rounding. The claim holds and is asserted as equality of 128 KiB
  // memcmp, not as a norm.
  //
  // What does need deriving is how slow a fetch may be before the residency
  // costs more than it saves, and that comes from a measurement this project
  // already made rather than from a target invented here. PLAN.md's Findings
  // record phase 1.1's pen-to-photon baseline: **p50 12.1-12.4 ms, p99
  // 15.7-16.4 ms**, against PRD F3's < 20 ms. So one frame's worth of work is
  // ~12.1 ms at p50.
  //
  // The worst honest demand on the residency is redrawing a full viewport:
  // ADR-0001's amendment sizes that at 2560x1440, which is
  // ceil(2560/128) x ceil(1440/128) = 20 x 12 = **240 tiles**. For a viewport
  // refresh to fit inside one p50 frame and leave nothing for anything else,
  // a fetch must cost at most 12.1 ms / 240 = **50.4 us**.
  //
  // That is the bound. It is asserted against the *warm* fetch, which is the
  // steady state a second and subsequent frame see, and where the measured
  // cost is 3.1-4.5 us -- ~11x of headroom.
  constexpr double kWarmFetchBudgetUs = 50.4;
  //
  // The **cold** fetch is deliberately given a much looser ceiling, and the
  // reason is a finding rather than a convenience: the measured cold cost of
  // 49-62 us/tile is *at or just past* the 50.4 us bound, so a fully cold
  // 2560x1440 viewport does not fit in one frame. That is reported in the
  // output rather than asserted away, because it is true and it is what a
  // reader of this section needs to know. What is asserted is a regression
  // guard well clear of machine noise and of a cold filesystem cache: 500 us
  // is ~8x the measured value, and still 3x better than the 1549 us/tile that
  // the untiled-source path measured, which is the thing this number exists
  // to stay on the right side of.
  constexpr double kColdFetchCeilingUs = 500.0;

  // --- Fixture -----------------------------------------------------------
  //
  // 2048x2048: 16x16 = 256 tiles, 32.00 MiB of half words. Chosen so the
  // document is genuinely larger than the eviction budget the test sets
  // later, which is the only way eviction can be *proven* rather than
  // assumed, and large enough that "resident bytes" is a number worth
  // comparing.
  constexpr int32_t kCanvas = 2048;
  constexpr int32_t kTilesPerSide = kCanvas / kTileSize;  // 16
  constexpr size_t kTileBytes = sizeof(Tile);             // 128 KiB

  // Content is deterministic and every tile differs from every other, so a
  // comparison cannot pass by two tiles happening to be identical. Built by
  // filling one tile's worth of half words once and then stamping the tile's
  // own coordinate into a few texels, rather than calling floatToHalf 16.7
  // million times -- the fixture is not what is under test.
  auto buildFixture = [&]() {
    Document doc = Document::createBlank(kCanvas, kCanvas, WorkingSpace{});
    doc.layers[0].name = "Source";
    TileStore& tiles = *doc.layers[0].rgbTiles;

    std::array<uint16_t, Tile::kTexelCount> base{};
    for (size_t i = 0; i < Tile::kTexelCount; i += 4) {
      const float t = static_cast<float>(i % 8192) / 8192.0f;
      base[i + 0] = floatToHalf(t);
      base[i + 1] = floatToHalf(1.0f - t);
      base[i + 2] = floatToHalf(t * t);
      base[i + 3] = floatToHalf(1.0f);  // opaque: keeps the composite simple
    }
    for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
      for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
        Tile& tile = tiles.getOrCreate(TileCoord{tx, ty});
        std::memcpy(tile.data(), base.data(), Tile::kTexelCount * sizeof(uint16_t));
        // Stamp the coordinate, so no two tiles are byte-equal.
        for (int32_t k = 0; k < 8; ++k) {
          const float a = static_cast<float>(tx * 37 + ty * 11 + k) / 512.0f;
          tile.writePixel(PixelCoord{k, k}, {a, 1.0f - a, a * 0.5f, 1.0f});
        }
      }
    }
    return doc;
  };

  // --- Part A: the interface itself, identical in both configurations -----

  {
    // Eager residency is today's behaviour restated as one implementation of
    // the interface, so it is asserted the same way in both builds.
    TileStore store;
    store.getOrCreate(TileCoord{1, 1}).writePixel(PixelCoord{0, 0}, {0.5f, 0.25f, 0.125f, 1.0f});
    store.getOrCreate(TileCoord{4, 2});
    LayerResidency eager = LayerResidency::adoptEager(std::move(store));

    check(eager.mode() == TileResidencyMode::Eager && eager.source().path.empty(),
          "eager residency has no backing file -- the source was read in full at open");
    check(eager.ownedTileCount() == 2 && eager.residentBytes() == 2 * kTileBytes,
          "eager: every tile is ours, and resident bytes are exactly the tiles");

    const TileFetch present = eager.readTile(TileCoord{1, 1});
    check(present.status == TileFetchStatus::Owned && present.tile != nullptr,
          "eager: an existing tile reads back as Owned, not Clean -- in eager mode there is "
          "no such thing as a tile that came from the file and is not ours");
    check(std::fabs(present.tile->readPixel(PixelCoord{0, 0})[0] - 0.5f) < 1e-3f,
          "eager: and it is the tile that was put in");

    const TileFetch missing = eager.readTile(TileCoord{9, 9});
    check(missing.status == TileFetchStatus::Absent && missing.tile == nullptr,
          "eager: an unallocated tile is Absent -- the same answer TileStore::find() gives, "
          "and it means transparent black rather than an error");

    check(!eager.isOwned(TileCoord{9, 9}), "eager: and Absent is not Owned");
    std::string writeError;
    Tile* fresh = eager.tileForWrite(TileCoord{9, 9}, &writeError);
    check(fresh != nullptr && writeError.empty(),
          "eager: writing to an unallocated tile promotes it from transparent black, which "
          "is not a failure -- that is what an absent tile already means");
    check(eager.isOwned(TileCoord{9, 9}) && eager.ownedTileCount() == 3 &&
              eager.residentBytes() == 3 * kTileBytes,
          "eager: and it is now owned, and counted");
    check(eager.readTile(TileCoord{9, 9}).status == TileFetchStatus::Owned,
          "eager: a promoted tile reads back as Owned");
    check(eager.tileForWrite(TileCoord{9, 9}) == fresh && eager.ownedTileCount() == 3,
          "copy-on-FIRST-write: a second write returns the same tile and promotes nothing "
          "again");
  }

  {
    // npaintLayerTileSource() is pure index arithmetic over data io/NpaintFile
    // already produced, so it runs and asserts the same answers in both
    // builds. The carry is hand-built rather than loaded, precisely so the
    // OFF build can check the mapping too.
    NpaintCarry carry;
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::Layer, 0});
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::RawPart, 0});
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::Layer, 1});

    const std::optional<TileSourceRef> first = npaintLayerTileSource("doc.npaint", carry, 0);
    const std::optional<TileSourceRef> second = npaintLayerTileSource("doc.npaint", carry, 1);
    const std::optional<TileSourceRef> absent = npaintLayerTileSource("doc.npaint", carry, 7);

    check(first.has_value() && first->subimage == 1 && first->miplevel == 0 &&
              first->path == "doc.npaint",
          "npaintLayerTileSource: layer 0 is subimage 1 -- part 0 is the composite");
    check(second.has_value() && second->subimage == 3,
          "npaintLayerTileSource: a carried foreign part between the layers shifts the "
          "second layer to subimage 3, because subimages are file order and so is partOrder");
    check(!absent.has_value(),
          "npaintLayerTileSource: a layer with no part behind it returns nullopt rather than "
          "naming a subimage that does not hold it");
  }

  // --- Part B: the cached strategy ---------------------------------------

  const char* kDocPath = "selftest_residency_doc.npaint";
  const char* kCopyPath = "selftest_residency_copy.npaint";
  const char* kTruncPath = "selftest_residency_trunc.npaint";
  const char* kMutPath = "selftest_residency_mut.npaint";
  const char* kPngPath = "selftest_residency_untiled.png";
  for (const char* p : {kDocPath, kCopyPath, kTruncPath, kMutPath, kPngPath}) std::remove(p);

  {
    // Save the fixture, then load it back the eager way. That gives both a
    // real tiled file and the reference the cached path is compared against.
    Document fixture = buildFixture();
    const NpaintSaveResult saved = saveNpaint(fixture, kDocPath);
    check(saved.ok && saved.partsWritten == 2,
          "a 2048x2048 document saves as a tiled .npaint (composite + one layer part)");

    NpaintLoadResult eagerLoad = loadNpaint(kDocPath);
    check(eagerLoad.ok && eagerLoad.document.layers.size() == 1,
          "and loads back the eager way -- the reference every comparison below uses");

    if (saved.ok && eagerLoad.ok) {
      LayerResidency eager =
          LayerResidency::adoptEager(std::move(*eagerLoad.document.layers[0].rgbTiles));
      eagerLoad.document.layers[0].rgbTiles.emplace();

      const std::optional<TileSourceRef> ref =
          npaintLayerTileSource(kDocPath, eagerLoad.carry, 0);
      check(ref.has_value() && ref->subimage == 1,
            "the layer's pixels are subimage 1 of the .npaint, via the part order "
            "io/NpaintFile already recorded");

      LayerResidency cached;
      std::string openError;
      const bool opened =
          ref.has_value() &&
          openCachedLayerResidency(*ref, kTileCacheBudgetBytes, &cached, &openError);
      check(opened,
            "**docs/document-format.md §1's claim, tested**: our own .npaint opens as a "
            "cached residency -- the ImageCache serves OUR documents, not only imports");
      if (!opened) std::printf("    open error: %s\n", openError.c_str());

      if (opened) {
        check(cached.mode() == TileResidencyMode::Cached && cached.dataX() == 0 &&
                  cached.dataY() == 0 && cached.dataWidth() == kCanvas &&
                  cached.dataHeight() == kCanvas,
              "the cached residency's data window is the layer part's own, tile-aligned");

        // ---- The headline measurement --------------------------------------
        const size_t eagerBytes = eager.residentBytes();
        const size_t cachedBytes = cached.residentBytes();
        std::printf(
            "    resident in OUR memory, %dx%d document: eager %.2f MiB (%zu tiles) vs "
            "cached %.2f MiB (%zu tiles + 1 staging)\n",
            kCanvas, kCanvas, static_cast<double>(eagerBytes) / 1048576.0,
            eager.ownedTileCount(), static_cast<double>(cachedBytes) / 1048576.0,
            cached.ownedTileCount());
        check(eagerBytes == static_cast<size_t>(kTilesPerSide) * kTilesPerSide * kTileBytes,
              "eager residency holds the whole document: 256 tiles, 32.00 MiB");
        check(cachedBytes == kTileBytes && cached.ownedTileCount() == 0,
              "cached residency holds ONE staging tile and owns nothing -- an unmodified "
              "tile is not in our memory at all");

        // ---- Bit-identity, at zero tolerance --------------------------------
        size_t compared = 0, identical = 0, absentBoth = 0, mismatched = 0;
        for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
          for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
            const TileCoord coord{tx, ty};
            const Tile* eagerTile = eager.ownedTiles().find(coord);
            const TileFetch got = cached.readTile(coord);
            ++compared;
            if (eagerTile == nullptr) {
              // The eager reader drops all-zero tiles; the cache has no such
              // rule and returns the zeros the file holds. Equal content,
              // different representation -- asserted rather than glossed.
              const bool cachedIsZero =
                  got.status == TileFetchStatus::Clean &&
                  std::all_of(got.tile->data(), got.tile->data() + Tile::kTexelCount,
                              [](uint16_t w) { return w == 0; });
              if (got.status == TileFetchStatus::Absent || cachedIsZero) {
                ++absentBoth;
              } else {
                ++mismatched;
              }
              continue;
            }
            if (got.status == TileFetchStatus::Clean &&
                std::memcmp(eagerTile->data(), got.tile->data(),
                            Tile::kTexelCount * sizeof(uint16_t)) == 0) {
              ++identical;
            } else {
              ++mismatched;
            }
          }
        }
        std::printf("    tiles compared %zu: bit-identical %zu, both-empty %zu, mismatched %zu\n",
                    compared, identical, absentBoth, mismatched);
        check(compared == 256 && identical == 256 && mismatched == 0,
              "**every tile served from the cache is BIT-IDENTICAL to the eager path** -- "
              "zero tolerance, memcmp of all 128 KiB, because half words go in and half "
              "words come out with no conversion stage anywhere");
        check(cached.cleanFetchCount() == 256 && cached.promotionCount() == 0,
              "and reading 256 tiles promoted none of them -- readTile() never makes a tile "
              "ours");
        check(cached.residentBytes() == kTileBytes,
              "and our resident bytes did not move: 256 tiles were read, one staging tile "
              "was held");

        // ---- Cost of a cold fetch vs a resident one -------------------------
        //
        // Timed in-process over many iterations. No subprocess anywhere near
        // this loop: spawning one dominates the measurement, which is a trap
        // this project has already fallen into once.
        //
        // Best of three passes for both figures, not one: the two ceilings
        // below exist to catch a real regression in the fetch path, and a
        // single pass has no defence against an unrelated scheduler or
        // filesystem-cache stall landing inside it on a loaded machine.
        constexpr int kWarmIterations = 20000;
        double coldUs = std::numeric_limits<double>::max();
        double warmUs = std::numeric_limits<double>::max();
        for (int pass = 0; pass < 3; ++pass) {
          tileCacheInvalidate(kDocPath);
          auto t0 = std::chrono::steady_clock::now();
          for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
            for (int32_t tx = 0; tx < kTilesPerSide; ++tx) (void)cached.readTile(TileCoord{tx, ty});
          auto t1 = std::chrono::steady_clock::now();
          coldUs = std::min(
              coldUs, std::chrono::duration<double, std::micro>(t1 - t0).count() / 256.0);

          t0 = std::chrono::steady_clock::now();
          for (int i = 0; i < kWarmIterations; ++i) {
            (void)cached.readTile(TileCoord{i % kTilesPerSide, (i / kTilesPerSide) % kTilesPerSide});
          }
          t1 = std::chrono::steady_clock::now();
          warmUs = std::min(
              warmUs, std::chrono::duration<double, std::micro>(t1 - t0).count() / kWarmIterations);
        }

        std::printf(
            "    [measured] fetch cost: cold %.2f us/tile (256 tiles), warm %.3f us/tile (%d fetches); "
            "one 2560x1440 viewport = 240 tiles -> cold %.1f ms, warm %.2f ms against phase "
            "1's 12.1 ms p50 frame\n",
            coldUs, warmUs, kWarmIterations, coldUs * 240.0 / 1000.0, warmUs * 240.0 / 1000.0);
        check(warmUs < kWarmFetchBudgetUs,
              "a warm fetch is inside the derived per-tile interactive budget (12.1 ms p50 / "
              "240 viewport tiles = 50.4 us)");
        check(coldUs < kColdFetchCeilingUs,
              "a cold fetch stays far inside the regression ceiling -- and see the printed "
              "cold viewport figure, which does NOT fit in one frame");

        // ---- The cache's own accounting -------------------------------------
        TileCacheStats stats;
        const bool haveStats = tileCacheStatistics(&stats);
        check(haveStats, "OpenImageIO's own ImageCache statistics are readable");
        if (haveStats) {
          std::printf(
              "    ImageCache: %.2f MiB used of %.2f MiB budget; tiles created %d, current "
              "%d, peak %d; images total %.2f MiB uncompressed\n",
              static_cast<double>(stats.memoryUsedBytes) / 1048576.0,
              static_cast<double>(stats.budgetBytes) / 1048576.0, stats.tilesCreated,
              stats.tilesCurrent, stats.tilesPeak,
              static_cast<double>(stats.imageSizeBytes) / 1048576.0);
          check(stats.memoryUsedBytes > 0 && stats.tilesCurrent > 0,
                "the cache is genuinely holding the tiles our memory is not");
          check(stats.budgetBytes >= static_cast<int64_t>(kTileCacheBudgetBytes) - 1048576,
                "and it is holding them under the budget this module set");
        }

        // ---- Eviction, proven rather than assumed ---------------------------
        //
        // Shrink the budget below the document, sweep every tile, then ask
        // for the FIRST tile again. If `tiles_created` grows, that specific
        // tile was dropped and had to be re-read -- which is evidence, where
        // "current < created" alone would only be a hint.
        constexpr size_t kSmallBudget = 4ull * 1024 * 1024;
        check(tileCacheSetBudgetBytes(kSmallBudget), "the budget can be shrunk at run time");
        // Invalidating first is load-bearing, and it was found by the test
        // failing rather than by reading the documentation: **shrinking the
        // budget evicts nothing on its own.** OpenImageIO frees tiles when it
        // *adds* one that would exceed the budget, so with every tile of this
        // document already resident, a re-read is a hit and the first version
        // of this check measured 256 of 256 tiles still resident under a
        // 4 MiB budget. Making the sweep cold is what puts the cache in the
        // state where the budget is actually enforced -- and it is also the
        // honest state to measure, since a budget only ever bites while
        // reading something new.
        tileCacheInvalidate(kDocPath);
        for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
          for (int32_t tx = 0; tx < kTilesPerSide; ++tx) (void)cached.readTile(TileCoord{tx, ty});
        TileCacheStats sweep;
        tileCacheStatistics(&sweep);
        (void)cached.readTile(TileCoord{0, 0});
        TileCacheStats after;
        tileCacheStatistics(&after);
        std::printf(
            "    under a %.0f MiB budget: %.2f MiB held, %d tiles resident of %d created; "
            "re-reading tile (0,0) took created %d -> %d\n",
            static_cast<double>(kSmallBudget) / 1048576.0,
            static_cast<double>(sweep.memoryUsedBytes) / 1048576.0, sweep.tilesCurrent,
            sweep.tilesCreated, sweep.tilesCreated, after.tilesCreated);
        check(sweep.tilesCurrent < 256,
              "under a budget smaller than the document, fewer tiles are resident than the "
              "document has");
        check(after.tilesCreated > sweep.tilesCreated,
              "**eviction really happened**: re-reading an already-swept tile created a new "
              "cache tile, so that exact tile had been dropped");
        check(cached.residentBytes() == kTileBytes,
              "and none of that touched our own memory -- eviction is the cache's business");

        // ---- Copy-on-first-write, and paint surviving an eviction -----------
        const TileCoord painted{2, 3};
        std::string promoteError;
        Tile* owned = cached.tileForWrite(painted, &promoteError);
        check(owned != nullptr && promoteError.empty(),
              "copy-on-first-write: a clean tile promotes to an owned one");
        if (owned != nullptr) {
          const Tile* reference = eager.ownedTiles().find(painted);
          check(reference != nullptr &&
                    std::memcmp(owned->data(), reference->data(),
                                Tile::kTexelCount * sizeof(uint16_t)) == 0,
                "and the promoted tile starts as the FILE's pixels, bit for bit -- not as "
                "zeros, which would erase what the stroke was painted on top of");
          check(cached.isOwned(painted) && cached.ownedTileCount() == 1 &&
                    cached.residentBytes() == 2 * kTileBytes,
                "and it is now ours: one owned tile plus the staging tile");

          // Paint one distinctive texel.
          const PixelCoord brush{11, 13};
          owned->writePixel(brush, {0.875f, 0.0f, 0.375f, 1.0f});

          // Force the cache to churn hard enough that this tile's *clean*
          // copy is certainly gone, then read the tile back.
          for (int pass = 0; pass < 2; ++pass)
            for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
              for (int32_t tx = 0; tx < kTilesPerSide; ++tx)
                if (!(tx == painted.x && ty == painted.y))
                  (void)cached.readTile(TileCoord{tx, ty});
          tileCacheInvalidate(kDocPath);

          const TileFetch back = cached.readTile(painted);
          check(back.status == TileFetchStatus::Owned,
                "after eviction AND a full cache invalidation, the painted tile still reads "
                "as Owned -- it stopped being the cache's business at the moment it was "
                "written");
          bool paintSurvived = false, restIntact = false;
          if (back.tile != nullptr) {
            const std::array<float, 4> px = back.tile->readPixel(brush);
            paintSurvived = std::fabs(px[0] - 0.875f) < 1e-3f && px[1] == 0.0f &&
                            std::fabs(px[2] - 0.375f) < 1e-3f;
            if (reference != nullptr) {
              // Everything except the painted texel must still be the file's.
              const size_t brushIndex =
                  (static_cast<size_t>(brush.y) * kTileSize + brush.x) * Tile::kChannels;
              restIntact = std::memcmp(back.tile->data(), reference->data(),
                                       brushIndex * sizeof(uint16_t)) == 0 &&
                           std::memcmp(back.tile->data() + brushIndex + 4,
                                       reference->data() + brushIndex + 4,
                                       (Tile::kTexelCount - brushIndex - 4) * sizeof(uint16_t)) ==
                               0;
            }
          }
          check(paintSurvived,
                "**the paint survived the eviction** -- the cache is not a write-back cache "
                "and an owned tile is never re-fetched");
          check(restIntact,
                "and the rest of that tile is still the file's pixels, bit for bit, so the "
                "promotion copied rather than reconstructed");

          // An untouched neighbour is still clean and still correct after all
          // that churn, which is what proves the promotion was local.
          const TileCoord neighbour{3, 3};
          const TileFetch fresh = cached.readTile(neighbour);
          const Tile* neighbourRef = eager.ownedTiles().find(neighbour);
          check(fresh.status == TileFetchStatus::Clean && neighbourRef != nullptr &&
                    std::memcmp(fresh.tile->data(), neighbourRef->data(),
                                Tile::kTexelCount * sizeof(uint16_t)) == 0,
                "an untouched neighbour is still Clean and still bit-identical -- promoting "
                "one tile did not make the rest ours");
        }
        check(tileCacheSetBudgetBytes(kTileCacheBudgetBytes), "the budget is restored");

        // ---- Outside the data window ----------------------------------------
        const TileFetch beyond = cached.readTile(TileCoord{kTilesPerSide + 4, 0});
        check(beyond.status == TileFetchStatus::Absent && beyond.tile == nullptr,
              "a tile outside the data window is Absent, NOT a successful read of zeros -- "
              "measured, OpenImageIO returns success with a zero fill there, and serving "
              "that would turn 'the file says nothing' into 'the file says transparent'");
      }

      // ---- The composite part is cacheable too ------------------------------
      TileSourceRef compositeRef;
      compositeRef.path = kDocPath;
      compositeRef.subimage = 0;
      LayerResidency composite;
      std::string compositeError;
      check(openCachedLayerResidency(compositeRef, kTileCacheBudgetBytes, &composite,
                                     &compositeError),
            "part 0, the composite, opens as a cached residency as well -- every part this "
            "build writes is tiled HALF RGBA, so the whole container is cache-addressable");
    }

    // ---- An untiled source is refused, with the measurement in the message
    {
      std::vector<uint16_t> px(16 * 16 * 4, 0x8000);
      const std::vector<uint8_t> png = encodePng16(16, 16, px.data());
      FILE* f = std::fopen(kPngPath, "wb");
      if (f != nullptr) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
      }
      TileSourceRef ref;
      ref.path = kPngPath;
      LayerResidency residency;
      std::string error;
      const bool opened =
          openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
      check(!opened, "an untiled (scanline) source is refused for cached residency");
      check(contains(error, "scanline-stored") && contains(error, "Eager"),
            "and the refusal names the storage and points at Eager, rather than offering a "
            "mode that measured 1549 us per scattered cold tile against 49 us for a tiled "
            "one");
    }

    // ---- Missing file: fail loudly, and NEVER zero-fill a promotion --------
    {
      NpaintLoadResult src = loadNpaint(kDocPath);
      if (src.ok) {
        const NpaintSaveResult copy = saveNpaint(src.document, kCopyPath, {}, &src.carry);
        TileSourceRef ref;
        ref.path = kCopyPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string error;
        const bool opened =
            copy.ok && openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
        check(opened, "a copy of the document opens as a cached residency");
        if (opened) {
          check(residency.readTile(TileCoord{0, 0}).status == TileFetchStatus::Clean,
                "and serves a tile while the file is there");
          std::remove(kCopyPath);
          const TileFetch gone = residency.readTile(TileCoord{5, 5});
          check(gone.status == TileFetchStatus::Failed && gone.tile == nullptr &&
                    contains(gone.error, kCopyPath),
                "with the file removed, a fetch Fails by name and serves no pixels at all");

          std::string promoteError;
          Tile* promoted = residency.tileForWrite(TileCoord{6, 6}, &promoteError);
          check(promoted == nullptr && !promoteError.empty(),
                "**and a promotion refuses rather than starting from zeros** -- zero-filling "
                "here would mean one stroke silently erased the image underneath it");
          check(!residency.isOwned(TileCoord{6, 6}) && residency.ownedTileCount() == 0,
                "a refused promotion leaves nothing owned behind");
        }
        tileCacheInvalidate(kCopyPath);
      }
    }

    // ---- Truncated file ----------------------------------------------------
    {
      FILE* in = std::fopen(kDocPath, "rb");
      if (in != nullptr) {
        std::fseek(in, 0, SEEK_END);
        const long size = std::ftell(in);
        std::fseek(in, 0, SEEK_SET);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        const size_t read = std::fread(bytes.data(), 1, bytes.size(), in);
        std::fclose(in);
        // Keep the header (EXR headers and the tile offset table live at the
        // front) and drop most of the pixel data, so the open can succeed and
        // the fetch is what fails -- the more interesting of the two paths.
        FILE* out = std::fopen(kTruncPath, "wb");
        if (out != nullptr) {
          std::fwrite(bytes.data(), 1, read / 4, out);
          std::fclose(out);
        }
        TileSourceRef ref;
        ref.path = kTruncPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string openError;
        const bool opened =
            openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &openError);
        bool refusedSomewhere = !opened;
        std::string message = openError;
        if (opened) {
          // The last tile is certainly past the truncation point.
          const TileFetch got =
              residency.readTile(TileCoord{kTilesPerSide - 1, kTilesPerSide - 1});
          refusedSomewhere = got.status == TileFetchStatus::Failed && got.tile == nullptr;
          message = got.error;
        }
        check(refusedSomewhere && !message.empty(),
              "a truncated file is refused -- at open or at the first fetch past the "
              "truncation -- and never serves partial or zeroed pixels as if they were the "
              "file's");
        check(contains(message, kTruncPath),
              "and the refusal names the file rather than wearing OpenEXR's wording alone");
        tileCacheInvalidate(kTruncPath);
      }
    }

    // ---- Changed on disk after open ---------------------------------------
    {
      NpaintLoadResult src = loadNpaint(kDocPath);
      if (src.ok) {
        const NpaintSaveResult first = saveNpaint(src.document, kMutPath, {}, &src.carry);
        TileSourceRef ref;
        ref.path = kMutPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string error;
        const bool opened =
            first.ok && openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
        check(opened, "a document opens as a cached residency, and its identity is stamped");
        if (opened) {
          check(residency.readTile(TileCoord{1, 1}).status == TileFetchStatus::Clean,
                "and serves tiles from the file it stamped");
          // Rewrite the same path with a materially different document, so
          // both size and mtime move.
          Document replacement = Document::createBlank(kCanvas, kCanvas, WorkingSpace{});
          replacement.layers[0]
              .rgbTiles->getOrCreate(TileCoord{1, 1})
              .writePixel(PixelCoord{0, 0}, {1.0f, 1.0f, 1.0f, 1.0f});
          const NpaintSaveResult second = saveNpaint(replacement, kMutPath);
          check(second.ok, "the file is then rewritten underneath the open residency");
          const TileFetch stale = residency.readTile(TileCoord{1, 1});
          check(stale.status == TileFetchStatus::Failed && stale.tile == nullptr,
                "**the next fetch Fails rather than serving what the cache still holds** -- "
                "measured, OpenImageIO's cache does not notice a file changing and would "
                "have served the old pixels indefinitely");
          check(contains(stale.error, "changed on disk") && contains(stale.error, kMutPath),
                "and says so by name, so a stale document is a loud error rather than a "
                "quiet wrong picture");
        }
        tileCacheInvalidate(kMutPath);
      }
    }

    tileCacheInvalidate(kDocPath);
  }

  // Scratch files: every path this section touches, removed unconditionally,
  // whether or not the assertion that created it passed.
  for (const char* p : {kDocPath, kCopyPath, kTruncPath, kMutPath, kPngPath}) std::remove(p);

  std::printf("[selftest] tile residency %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
