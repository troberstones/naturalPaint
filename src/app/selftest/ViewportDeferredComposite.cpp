#include "app/selftest/Support.hpp"

namespace np {

// ui/DocumentTexture.hpp decision 6: `DocumentTexture::viewFor()`'s optional
// `DocumentTextureViewport`. See app/SelfTest.hpp's declaration for the full
// list of what this proves; the section headers below repeat it split up.
bool runViewportDeferredCompositeTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(
        tileLocalOffset(at), premultiplied);
  };
  auto texelAt = [](const std::vector<uint16_t>& halves, int32_t w, int32_t x, int32_t y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return std::array<float, 4>{halfToFloat(halves[i + 0]), halfToFloat(halves[i + 1]),
                                halfToFloat(halves[i + 2]), halfToFloat(halves[i + 3])};
  };
  auto sameHalves = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(uint16_t)) == 0;
  };
  // A tile's own centre pixel -- the coordinate every probe below reads,
  // since it can never straddle a tile boundary regardless of tile size.
  auto tileCentre = [](int32_t tx, int32_t ty) {
    return PixelCoord{tx * kTileSize + kTileSize / 2, ty * kTileSize + kTileSize / 2};
  };

  // A 10x10-tile canvas (1280x1280): big enough that a small on-screen
  // viewport leaves the large majority of tiles off-screen, and that
  // the trickle budget does not clear the backlog in a single call -- both are
  // load-bearing for the sections below actually exercising deferral rather
  // than accidentally finishing in one shot.
  //
  // **Since the budget became a deadline (`kViewportTrickleBudgetMs`) rather
  // than a tile count, that second property is no longer free.** A 100-tile
  // one-layer canvas is cheap enough that the production budget would buy the
  // whole backlog in one call, and every deferral assertion below would then
  // pass vacuously -- green while testing nothing. So the fixtures that need
  // deferral call `setTrickleBudgetMs()` with a deliberately tiny deadline.
  // That is strictly better than the old arrangement, which depended on a
  // production constant happening to be smaller than this fixture and would
  // have silently stopped testing anything the moment someone raised it.
  constexpr int32_t kTilesAcross = 10;
  constexpr int32_t kCanvasSize = kTilesAcross * kTileSize;
  const std::array<float, 4> kProbeColour{0.5f, 0.25f, 0.125f, 1.0f};
  const std::array<float, 4> kEditColour{0.75f, 0.5f, 0.25f, 1.0f};

  auto fillEveryTile = [&](Document& doc, const std::array<float, 4>& colour) {
    for (int32_t ty = 0; ty < kTilesAcross; ++ty)
      for (int32_t tx = 0; tx < kTilesAcross; ++tx) {
        const PixelCoord c = tileCentre(tx, ty);
        writeRgb(doc, 0, c.x, c.y, colour);
      }
  };

  std::printf("  -- 1. a null viewport is byte-for-byte unaffected --\n");

  // -----------------------------------------------------------------------
  // 1. viewport == nullptr never defers, at every step of an edit sequence
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "null-vp");
    DocumentTexture dt;

    fillEveryTile(od.document, kProbeColour);
    od.recordEdit("fill", EditKind::Content);
    dt.viewFor(gpu, od);
    check(dt.pendingTiles() == 0, "null viewport: nothing pending after the first (full) call");
    check(sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document)),
          "null viewport: bit-identical to the oracle after the full call");

    writeRgb(od.document, 0, tileCentre(7, 3).x, tileCentre(7, 3).y, kEditColour);
    od.recordEdit("one tile", EditKind::Content);
    dt.viewFor(gpu, od);
    check(dt.pendingTiles() == 0, "null viewport: nothing pending after an incremental call");
    check(dt.incrementalUpdates() == 1 && dt.deferredUpdates() == 0,
          "null viewport: counted as an ordinary incremental update, never a deferral");
    check(sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document)),
          "null viewport: bit-identical after the incremental call too");
  }

  std::printf("  -- 2. a viewport covering the whole canvas takes the SAME fast paths --\n");

  // -----------------------------------------------------------------------
  // 2. A whole-canvas viewport is counter-for-counter identical to nullptr
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "whole-vp");
    DocumentTexture withNull;
    DocumentTexture withWhole;
    const DocumentTextureViewport whole{0, 0, kCanvasSize, kCanvasSize};

    auto step = [&](const char* label) {
      withNull.viewFor(gpu, od);
      withWhole.viewFor(gpu, od, nullptr, &whole);
      const bool countersMatch =
          withNull.fullRecomposites() == withWhole.fullRecomposites() &&
          withNull.incrementalUpdates() == withWhole.incrementalUpdates() &&
          withNull.emptyUpdates() == withWhole.emptyUpdates() &&
          withNull.lastDirtyTiles() == withWhole.lastDirtyTiles();
      check(countersMatch, label);
      check(withWhole.pendingTiles() == 0, "whole-canvas viewport: never defers anything");
      check(sameHalves(withNull.uploadedHalves(), withWhole.uploadedHalves()),
            "whole-canvas viewport: bit-identical to the nullptr instance");
    };

    fillEveryTile(od.document, kProbeColour);
    od.recordEdit("fill", EditKind::Content);
    step("whole-canvas viewport: full recompose counters match nullptr's");

    writeRgb(od.document, 0, tileCentre(2, 2).x, tileCentre(2, 2).y, kEditColour);
    od.recordEdit("one tile", EditKind::Content);
    step("whole-canvas viewport: incremental counters match nullptr's");

    od.recordEdit("rename only", EditKind::Structural);
    step("whole-canvas viewport: empty-update counters match nullptr's");
  }

  std::printf("  -- 3. priority: on-screen matches THIS call, off-screen genuinely does not --\n");

  // -----------------------------------------------------------------------
  // 3 + 6. Deferral is real (not a no-op), AND the deferred region is not
  //        silently lost when a later call answers with no viewport at all
  //        -- the snapshot-overwrite trap the header names.
  // -----------------------------------------------------------------------
  DocumentTexture dtBacklog;  // reused by section 6 below
  OpenDocument backlogDoc = makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "backlog");
  {
    fillEveryTile(backlogDoc.document, kProbeColour);
    backlogDoc.recordEdit("fill", EditKind::Content);

    // A small on-screen rectangle roughly centred on tile (4,4)-(5,5), well
    // clear (more than one tile's margin) of tile (9,9) at the far corner.
    const DocumentTextureViewport centre{550, 550, 700, 700};
    // A deadline small enough that the take pins to `kMinViewportTrickleTiles`
    // on this fixture, so "off-screen is genuinely deferred" is a fact about
    // the deferral machinery and not about how fast this machine happens to
    // composite four tiles.
    dtBacklog.setTrickleBudgetMs(0.000001);
    dtBacklog.viewFor(gpu, backlogDoc, nullptr, &centre);

    const std::vector<uint16_t> oracle = compositeDocumentStraightHalf(backlogDoc.document);
    const PixelCoord onScreen = tileCentre(4, 4);
    const PixelCoord farCorner = tileCentre(9, 9);  // last tile in raster order

    check(dtBacklog.pendingTiles() > 0,
          "priority: a small viewport on a 100-tile canvas leaves a real backlog");
    check(dtBacklog.deferredUpdates() == 1, "priority: counted as a deferred call");
    check(std::memcmp(texelAt(dtBacklog.uploadedHalves(), kCanvasSize, onScreen.x, onScreen.y)
                          .data(),
                      texelAt(oracle, kCanvasSize, onScreen.x, onScreen.y).data(),
                      sizeof(float) * 4) == 0,
          "priority: the on-screen tile is correct after ONE call");
    // The far corner is the LAST tile canvasTiles() visits (raster order),
    // so it is necessarily the last candidate the trickle budget would ever
    // reach -- with the take pinned to the floor (4) and ~84 off-screen tiles
    // here, it is not reached this call.
    check(std::memcmp(texelAt(dtBacklog.uploadedHalves(), kCanvasSize, farCorner.x, farCorner.y)
                          .data(),
                      texelAt(oracle, kCanvasSize, farCorner.x, farCorner.y).data(),
                      sizeof(float) * 4) != 0,
          "priority: the far off-screen tile is genuinely NOT yet caught up (not a no-op)");
  }

  std::printf("  -- 4. the snapshot-overwrite trap: switching to no viewport still converges --\n");

  {
    // Continues directly from section 3's `dtBacklog`/`backlogDoc`: a
    // backlog already exists and `backlogDoc` has NOT been edited since.
    // `snapshot_` was overwritten with the full document at the end of THAT
    // call regardless of what got deferred -- this is exactly the moment
    // the header's decision 6 says a naive design would lose the backlog,
    // because `documentDirtyTiles()` alone can no longer rediscover it.
    dtBacklog.viewFor(gpu, backlogDoc);  // no viewport this time, no edit either
    check(dtBacklog.pendingTiles() == 0,
          "trap: a subsequent no-viewport call fully absorbs the standing backlog");
    check(sameHalves(dtBacklog.uploadedHalves(), compositeDocumentStraightHalf(backlogDoc.document)),
          "trap: and the result is bit-identical to the oracle across the WHOLE canvas -- "
          "nothing was silently left stale");
  }

  std::printf("  -- 5. convergence: a PARKED viewport still counts the backlog down to zero --\n");

  // -----------------------------------------------------------------------
  // 5. Repeated calls against an unedited document, same restrictive
  //    viewport throughout -- the navigator-thumbnail-staleness case: the
  //    canvas itself is always correct where it matters (on screen), but the
  //    thumbnail (drawn from this same texture, decision-independent of the
  //    viewport) needs the WHOLE canvas to catch up eventually.
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "converge");
    fillEveryTile(od.document, kProbeColour);
    od.recordEdit("fill", EditKind::Content);

    DocumentTexture dt;
    // Same reason as section 3: pinned to the floor so convergence is being
    // measured, not the machine's speed.
    dt.setTrickleBudgetMs(0.000001);
    const DocumentTextureViewport centre{550, 550, 700, 700};
    const PixelCoord farCorner = tileCentre(9, 9);
    const std::vector<uint16_t> oracle = compositeDocumentStraightHalf(od.document);

    size_t previousPending = std::numeric_limits<size_t>::max();
    bool everMonotonic = true;
    bool convergedWithinBudget = false;
    // Enough calls that `kTilesAcross * kTilesAcross` tiles minus a small
    // on-screen viewport, at `kMinViewportTrickleTiles` a call, is guaranteed
    // to finish -- (100 / 4) is 25; 40 is a margin so a change to the floor
    // does not make this section flaky. The budget is pinned above, so this
    // is the slowest the adaptive path can ever be, not a typical rate.
    for (int call = 0; call < 40 && !convergedWithinBudget; ++call) {
      dt.viewFor(gpu, od, nullptr, &centre);
      if (dt.pendingTiles() > previousPending) everMonotonic = false;
      previousPending = dt.pendingTiles();
      if (dt.pendingTiles() == 0) convergedWithinBudget = true;
    }

    check(everMonotonic, "convergence: pendingTiles() never increases while parked");
    check(convergedWithinBudget, "convergence: pendingTiles() reaches zero within 20 calls");
    check(sameHalves(dt.uploadedHalves(), oracle),
          "convergence: once at zero, uploadedHalves() is bit-identical to the oracle");
    check(std::memcmp(texelAt(dt.uploadedHalves(), kCanvasSize, farCorner.x, farCorner.y).data(),
                      texelAt(oracle, kCanvasSize, farCorner.x, farCorner.y).data(),
                      sizeof(float) * 4) == 0,
          "convergence: including the tile section 3 proved was NOT caught up after one call");

    // The GPU texture itself, read back once -- the CPU mirror
    // (`uploadedHalves()`) matching the oracle is the load-bearing proof;
    // this confirms the actual upload landed the same bytes, the same way
    // section 7 of app/selftest/DocumentTexture.cpp's own GPU round trip
    // does for the un-deferred path.
    std::vector<float> readback;
    const bool read =
        readbackRGBA16FPadded(gpu, dt.texture(), static_cast<uint32_t>(kCanvasSize),
                              static_cast<uint32_t>(kCanvasSize), readback);
    bool gpuMatches = read && readback.size() == oracle.size();
    for (size_t i = 0; gpuMatches && i < oracle.size(); ++i)
      gpuMatches = gpuMatches && readback[i] == halfToFloat(oracle[i]);
    check(gpuMatches, "convergence: the GPU texture itself matches the oracle once converged");
  }

  std::printf("  -- 6. prompt scroll-into-view: a newly-visible tile is never throttled --\n");

  // -----------------------------------------------------------------------
  // 6. A tile that was off-screen and is now on-screen is caught up THIS
  //    call, unconditionally -- not merely "eventually, via the trickle".
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "scroll");
    fillEveryTile(od.document, kProbeColour);
    od.recordEdit("fill", EditKind::Content);

    DocumentTexture dt;
    const DocumentTextureViewport centre{550, 550, 700, 700};
    dt.viewFor(gpu, od, nullptr, &centre);
    const PixelCoord farCorner = tileCentre(9, 9);
    const std::vector<uint16_t> oracle = compositeDocumentStraightHalf(od.document);
    const bool staleBeforeScroll =
        std::memcmp(texelAt(dt.uploadedHalves(), kCanvasSize, farCorner.x, farCorner.y).data(),
                   texelAt(oracle, kCanvasSize, farCorner.x, farCorner.y).data(),
                   sizeof(float) * 4) != 0;
    check(staleBeforeScroll, "scroll: the far tile starts genuinely stale (setup for this section)");

    // "Scroll" the viewport to the far corner -- no document edit at all.
    const DocumentTextureViewport farViewport{kCanvasSize - 150, kCanvasSize - 150, kCanvasSize,
                                              kCanvasSize};
    dt.viewFor(gpu, od, nullptr, &farViewport);
    check(std::memcmp(texelAt(dt.uploadedHalves(), kCanvasSize, farCorner.x, farCorner.y).data(),
                      texelAt(oracle, kCanvasSize, farCorner.x, farCorner.y).data(),
                      sizeof(float) * 4) == 0,
          "scroll: the tile is caught up the VERY NEXT call once it is on screen -- "
          "not throttled by the trickle budget");
  }

  std::printf("  -- 7. performance (printed, not asserted): the trickle budget's own cost --\n");

  // -----------------------------------------------------------------------
  // 7. What kViewportTrickleBudget is set from: the same 40-layer/2048x2048
  //    synthetic app/selftest/OpaqueFloor.cpp's and
  //    app/selftest/CompositeParallel.cpp's own perf sections use, so the
  //    figures are directly comparable.
  // -----------------------------------------------------------------------
  {
    constexpr int32_t kBigW = 2048;
    constexpr int32_t kBigH = 2048;
    constexpr size_t kLayerCount = 40;
    OpenDocument od = makeBlankOpenDocument(kBigW, kBigH, WorkingSpace{}, "perf");
    for (size_t i = 0; i < kLayerCount; ++i) {
      if (i > 0) addLayer(od.document, od.document.layers.size(), makeRgbLayer("layer"));
      for (int32_t y = 0; y < kBigH; y += 3)
        for (int32_t x = 0; x < kBigW; x += 3)
          od.document.layers[i].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
              .writePixel(tileLocalOffset(PixelCoord{x, y}),
                         {0.1f * static_cast<float>(i % 7), 0.2f, 0.3f, 0.4f});
    }
    od.recordEdit("initial", EditKind::Content);

    DocumentTexture dt;
    dt.viewFor(gpu, od);  // warm: a real snapshot to diff against

    // core/DirtyTiles.hpp's own cost model (its `preferFullRecomposite()`
    // comment) is `S + n*t`: a per-CALL setup `S` (the pairing pass, the
    // clip pass, one `layerPointOps()` per layer -- all O(layers), none of
    // it per tile) plus a per-TILE marginal cost `t`. Dividing one call's
    // total time by its tile count conflates the two, and for a small
    // trickle budget `S` is most of what gets measured -- an earlier,
    // uncalibrated version of this section read anywhere from 0.72 to
    // 1.79 ms/"tile" across different budgets and different runs of the
    // SAME binary purely from that conflation, which is not a number a
    // constant should be sized from. So this calibrates `S` and `t`
    // separately, from two genuinely different tile counts on the identical
    // fixture, and reports both.
    auto touchTopLayerEveryNth = [&](int32_t stride, float alpha) {
      for (int32_t y = 0; y < kBigH; y += stride)
        for (int32_t x = 0; x < kBigW; x += stride)
          od.document.layers[kLayerCount - 1]
              .rgbTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
              .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.9f, 0.8f, 0.7f, alpha});
    };
    auto medianCallMsFor = [&](const DocumentTextureViewport& vp, int32_t stride,
                               size_t& tilesOut) {
      constexpr int kTrials = 7;
      std::vector<double> ms;
      float alpha = 0.6f;
      for (int trial = 0; trial < kTrials; ++trial) {
        touchTopLayerEveryNth(stride, alpha);
        alpha = alpha == 0.6f ? 0.5f : 0.6f;  // a genuinely different value each trial
        od.recordEdit("touch", EditKind::Content);
        dt.viewFor(gpu, od, nullptr, &vp);
        tilesOut = dt.lastDirtyTiles();
        ms.push_back(dt.lastUploadMs());
      }
      std::sort(ms.begin(), ms.end());
      return ms[ms.size() / 2];
    };

    // Point A: a viewport that covers the ONLY dirty tile (a stride large
    // enough to touch exactly one tile, near the canvas origin) -- an
    // incremental call with nothing to defer, n = 1, cost ~= S + t.
    size_t tilesA = 0;
    const DocumentTextureViewport singleTile{0, 0, kTileSize, kTileSize};
    const double msA = medianCallMsFor(singleTile, kBigW, tilesA);

    // Point B: a corner viewport on a fully-dirty canvas -- almost every one
    // of the 16x16 = 256 tiles is off-screen, so this call's `processSet` is
    // bounded by the trickle budget, cost ~= S + n*t at whatever n that
    // budget currently produces. Pinned to the floor so the fit has two
    // genuinely different tile counts to solve from; the adaptive take is
    // measured separately below, where its whole point is that it is NOT a
    // constant.
    size_t tilesB = 0;
    const DocumentTextureViewport corner{0, 0, 1, 1};
    dt.setTrickleBudgetMs(0.000001);
    const double msB = medianCallMsFor(corner, 3, tilesB);

    const double perTileMs =
        tilesB > tilesA ? (msB - msA) / static_cast<double>(tilesB - tilesA) : 0.0;
    const double setupMs = msA - perTileMs * static_cast<double>(tilesA);

    constexpr double kPenToPhotonMs = 20.0;  // PRD F3 (P0)
    std::printf("    [measured] point A: %zu tile(s), %.3f ms median -- point B: %zu tile(s), "
                "%.3f ms median -- fit: S (per-call setup) ~= %.3f ms, t (per-tile marginal) "
                "~= %.4f ms/tile\n",
                tilesA, msA, tilesB, msB, setupMs, perTileMs);
    check(tilesA >= 1 && tilesB > tilesA,
          "performance: the two calibration points give a solvable (n increases) fit");
    check(tilesB > 0 && tilesB <= kMinViewportTrickleTiles + 4,
          "performance: a pinned budget bounds the call by the floor, not the canvas");

    // --- What the deadline buys, on this fixture ---------------------------
    //
    // The old constant was 4 tiles for every document. The whole claim of
    // `kViewportTrickleBudgetMs` is that the take is derived from what a tile
    // actually costs HERE -- 40 layers, deliberately pessimistic -- so this
    // is where that claim is checked rather than asserted in a comment.
    dt.setTrickleBudgetMs(kViewportTrickleBudgetMs);
    size_t tilesAdaptive = 0;
    const double msAdaptive = medianCallMsFor(corner, 3, tilesAdaptive);

    std::printf("    [measured] budget %.1f ms on a %zu-layer %dx%d document -> learned rate "
                "%.4f ms/tile, take %zu tile(s), call %.2f ms (%.0f%% of budget, %.0f%% of PRD "
                "F3's %.0f ms)\n",
                dt.trickleBudgetMs(), kLayerCount, kBigW, kBigH, dt.trickleMsPerTile(),
                dt.lastTrickleTake(), msAdaptive, 100.0 * msAdaptive / dt.trickleBudgetMs(),
                100.0 * msAdaptive / kPenToPhotonMs, kPenToPhotonMs);

    // The floor is a floor. This is the assertion that would have caught the
    // adaptive path degenerating into "always the minimum", which is the way
    // this change could regress to the old behaviour while every other
    // assertion here still passed.
    check(dt.lastTrickleTake() >= kMinViewportTrickleTiles,
          "performance: the adaptive take never falls below the old fixed budget");
    check(dt.lastTrickleTake() <= kMaxViewportTrickleTiles,
          "performance: and never exceeds the stale-estimate cap");

    // The rate is learned, not assumed. A negative value means nothing was
    // ever observed, which would make every take the floor.
    check(dt.trickleMsPerTile() > 0.0,
          "performance: a per-tile rate was actually learned from real calls");

    // **The call's wall clock is PRINTED, not asserted**, and that correction
    // is worth recording. This section first asserted
    // `msAdaptive <= budget * 3 + S`, which passed on an idle machine and
    // failed the moment the suite ran beside a compile: the learned rate went
    // from 1.74 to 2.19 ms/tile and the call from 13.6 to 17.1 ms, purely
    // from load. That is a machine-speed claim wearing an assertion's
    // clothes -- the same mistake this file's own header warns about for the
    // `S`/`t` fit -- and on a 40-layer document at the floor the deadline is
    // arithmetically unmeetable anyway (`S` alone is ~3 ms and four tiles add
    // ~5 ms), so the assertion was never testing the budget in the first
    // place.
    //
    // What IS asserted instead is **monotonicity in the budget**, which is
    // timing-free and is the property a deadline actually has to have: a
    // smaller deadline must never buy more tiles. A take that ignored the
    // budget -- returning the cap, or a constant -- fails this on any machine
    // at any speed.
    const size_t takeAtFullBudget = dt.lastTrickleTake();
    dt.setTrickleBudgetMs(kViewportTrickleBudgetMs / 8.0);
    size_t tilesSmallBudget = 0;
    medianCallMsFor(corner, 3, tilesSmallBudget);
    const size_t takeAtSmallBudget = dt.lastTrickleTake();
    std::printf("    [measured] budget %.2f ms -> take %zu tile(s); budget %.2f ms -> take "
                "%zu tile(s)\n",
                kViewportTrickleBudgetMs, takeAtFullBudget, kViewportTrickleBudgetMs / 8.0,
                takeAtSmallBudget);
    // Weak HERE and known to be: on this 40-layer fixture both budgets pin to
    // the floor, so 4 <= 4 holds vacuously. It is still worth asserting as an
    // invariant, and section 8 runs the same comparison on the cheap fixture
    // where the two takes genuinely differ -- that is the one that
    // discriminates.
    check(takeAtSmallBudget <= takeAtFullBudget,
          "performance: an eighth of the deadline never buys MORE tiles (vacuous here -- "
          "section 8 is the discriminating case)");
    dt.setTrickleBudgetMs(kViewportTrickleBudgetMs);

    // And it must still be a *deferred* call -- if the take swallowed all 256
    // tiles, the two assertions above would be measuring a full recomposite
    // and the deadline would mean nothing.
    check(tilesAdaptive < static_cast<size_t>((kBigW / kTileSize) * (kBigH / kTileSize)),
          "performance: the adaptive call still defers rather than clearing the canvas");

    // Printed, not asserted -- a machine-speed claim, exactly like
    // app/selftest/OpaqueFloor.cpp's and app/selftest/CompositeParallel.cpp's
    // own perf sections. What IS asserted above is the shape the budget
    // guarantees regardless of machine: floor respected, cap respected, rate
    // learned, deadline the same order as the call, deferral still happening.
    // That is the part a faster machine must not be allowed to hide.
 
  }

  std::printf("  -- 8. the budget ADAPTS: a cheap document must buy more than the floor --\n");

  // -----------------------------------------------------------------------
  // 8. The assertion section 7 could not make.
  //
  // **Every assertion in section 7 passes when `trickleTake()` is sabotaged
  // to `return kMinViewportTrickleTiles;`.** That was checked by doing it,
  // not reasoned about -- and it is the whole point of this section. Section
  // 7's fixture is 40 layers at ~1.3 ms/tile, where a 4 ms deadline honestly
  // buys the floor and nothing more, so "take >= floor" and "take <= cap"
  // are both satisfied by the degenerate answer. A suite that only measured
  // there would go green on a build that had silently reverted to the fixed
  // budget this change exists to replace.
  //
  // So: the same machinery on a ONE-layer canvas, where a tile is cheap
  // enough that the deadline must buy substantially more. This is the
  // section that fails when the adaptive path stops adapting.
  // -----------------------------------------------------------------------
  {
    OpenDocument od =
        makeBlankOpenDocument(kCanvasSize, kCanvasSize, WorkingSpace{}, "adaptive");
    fillEveryTile(od.document, kProbeColour);
    od.recordEdit("fill", EditKind::Content);

    DocumentTexture dt;
    const DocumentTextureViewport corner{0, 0, 1, 1};

    // Several calls, so the moving average has something to converge on --
    // the first call always takes the floor (nothing observed yet), which is
    // by design and must not be mistaken for the answer.
    size_t take = 0;
    for (int call = 0; call < 6; ++call) {
      fillEveryTile(od.document, call % 2 == 0 ? kEditColour : kProbeColour);
      od.recordEdit("touch", EditKind::Content);
      dt.viewFor(gpu, od, nullptr, &corner);
      take = dt.lastTrickleTake();
    }

    std::printf("    [measured] 1-layer %dx%d, budget %.1f ms -> learned rate %.4f ms/tile, "
                "take %zu tile(s) (floor is %zu)\n",
                kCanvasSize, kCanvasSize, dt.trickleBudgetMs(), dt.trickleMsPerTile(), take,
                kMinViewportTrickleTiles);

    // The load-bearing one. A one-layer 128x128-tile composite runs far under
    // 4 ms / 4 tiles = 1 ms per tile on any machine this builds on, so the
    // budget must buy more than the floor -- and if it does not, the take is
    // no longer being derived from the deadline at all.
    check(take > kMinViewportTrickleTiles,
          "adaptive: a cheap one-layer document buys MORE than the floor (this is the "
          "assertion a reverted trickleTake() fails)");

    // Strictly more than the old fixed budget bought, stated as its own
    // assertion so the regression reads as what it is rather than as an
    // arithmetic coincidence about the floor's value.
    check(take > 4, "adaptive: and more than the old fixed budget of 4 tiles");

    // **Monotonicity in the budget, where it actually discriminates.** The
    // same comparison in section 7 is vacuous (both budgets pin to the floor
    // on 40 layers); here the full budget buys tens of tiles, so a take that
    // ignored the deadline -- a constant, or the cap -- fails this. Timing-
    // free: it compares two takes, never a clock, so machine load cannot
    // move it.
    const size_t takeFull = take;
    dt.setTrickleBudgetMs(kViewportTrickleBudgetMs / 8.0);
    fillEveryTile(od.document, kEditColour);
    od.recordEdit("touch", EditKind::Content);
    dt.viewFor(gpu, od, nullptr, &corner);
    const size_t takeEighth = dt.lastTrickleTake();
    std::printf("    [measured] 1-layer: budget %.2f ms -> take %zu, budget %.2f ms -> take %zu\n",
                kViewportTrickleBudgetMs, takeFull, kViewportTrickleBudgetMs / 8.0, takeEighth);
    check(takeEighth < takeFull,
          "adaptive: an eighth of the deadline buys STRICTLY fewer tiles (the take really "
          "is derived from the budget)");
    check(takeEighth >= kMinViewportTrickleTiles,
          "adaptive: ...but never below the floor, however small the deadline");
    dt.setTrickleBudgetMs(kViewportTrickleBudgetMs);

    // Correctness is not traded for the larger take: the same bit-identity
    // the incremental path owes everywhere else still holds once converged.
    dt.viewFor(gpu, od);  // no viewport: absorb whatever is left
    check(dt.pendingTiles() == 0, "adaptive: the backlog still converges to zero");
    check(sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document)),
          "adaptive: and the result is still bit-identical to the oracle");
  }

  return ok;
}

}  // namespace np
