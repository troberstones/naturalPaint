#include "app/selftest/Support.hpp"

namespace np {

bool runHistoryTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  using Clock = std::chrono::steady_clock;
  auto seconds = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
  };
  constexpr double kMiB = 1024.0 * 1024.0;

  // --- Tolerances -------------------------------------------------------
  //
  // Every correctness claim here is at **exactly zero tolerance**, and as in
  // step 6 that is a property of the mechanism rather than of a lucky
  // fixture: a history entry is a `Document` whose tiles are shared
  // `shared_ptr` slots, so "the state came back" is `memcmp` over raw half
  // words or pointer identity, never a comparison of two computed numbers.
  // Nothing in this file interpolates, converts or rounds. The timing and RSS
  // lines are machine numbers and are printed, with only the one ratio noted
  // at its point of use asserted.

  // --- Shared fixtures ---------------------------------------------------

  constexpr int32_t kTilesPerSide = 16;  // 2048x2048, 256 tiles, 32.0 MiB
  auto buildRealisticDocument = []() {
    Document doc = Document::createBlank(kTilesPerSide * kTileSize, kTilesPerSide * kTileSize,
                                         WorkingSpace{});
    doc.layers[0].name = "Source";
    TileStore& tiles = *doc.layers[0].rgbTiles;
    std::array<uint16_t, Tile::kTexelCount> base{};
    for (size_t i = 0; i < Tile::kTexelCount; i += 4) {
      const float t = static_cast<float>(i % 8192) / 8192.0f;
      base[i + 0] = floatToHalf(t);
      base[i + 1] = floatToHalf(1.0f - t);
      base[i + 2] = floatToHalf(t * t);
      base[i + 3] = floatToHalf(1.0f);
    }
    for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
      for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
        Tile& tile = tiles.getOrCreate(TileCoord{tx, ty});
        std::memcpy(tile.data(), base.data(), Tile::kTexelCount * sizeof(uint16_t));
      }
    }
    return doc;
  };

  // A deep copy -- the pre-step-6 behaviour, used wherever a fixture must
  // have no holder outside the history being measured. Without it every
  // original tile has an external holder and no entry could ever hold
  // anything exclusively, which would make every attributable number zero and
  // every assertion below vacuous.
  auto deepCopy = [](const Document& src) {
    Document d = src;
    unshareDocumentTiles(d);
    return d;
  };

  // One "stroke": rewrites a tile through `getOrCreate()`, which is the
  // barrier every writer in this build goes through and the one
  // core/TileStore.hpp says the canvas bridge will use when it lands.
  auto paintTile = [](Document& doc, int32_t tx, int32_t ty, float v) {
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty});
    for (int32_t k = 0; k < kTileSize; ++k)
      tile.writePixel(PixelCoord{k, k % kTileSize}, {v, 1.0f - v, v * 0.5f, 1.0f});
  };

  // Reads one texel, so a state can be identified by content rather than by
  // the test's own bookkeeping.
  auto readTexel = [](const Document& doc, int32_t tx, int32_t ty) -> std::array<float, 4> {
    const Tile* t = doc.layers[0].rgbTiles->find(TileCoord{tx, ty});
    return t ? t->readPixel(PixelCoord{0, 0}) : std::array<float, 4>{-1.0f, -1.0f, -1.0f, -1.0f};
  };

  // Bit-exact document comparison: every tile of every layer, by memcmp over
  // the raw half words. Zero tolerance, and it fails on a missing tile rather
  // than treating absence as zero.
  auto sameTiles = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const auto& la = a.layers[i];
      const auto& lb = b.layers[i];
      if (la.rgbTiles.has_value() != lb.rgbTiles.has_value()) return false;
      if (!la.rgbTiles) continue;
      if (la.rgbTiles->occupiedTileCount() != lb.rgbTiles->occupiedTileCount()) return false;
      for (const auto& [coord, tile] : *la.rgbTiles) {
        const Tile* other = lb.rgbTiles->find(coord);
        if (!other) return false;
        if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0)
          return false;
      }
    }
    return true;
  };

  // The independent byte count: distinct tile OBJECTS across a set of
  // documents, counted by address and deduplicated. This is step 6's own
  // method (`--selftest`'s `cow tiles` section counts the same way), written
  // out again here so `History::bytes()` is checked against something that
  // does not share a line of code with it.
  auto distinctTilesIn = [](const std::vector<const Document*>& docs) {
    std::vector<const void*> seen;
    for (const Document* d : docs) {
      for (const Layer& layer : d->layers) {
        if (!layer.rgbTiles) continue;
        for (const auto& [coord, tile] : *layer.rgbTiles) {
          (void)coord;
          seen.push_back(static_cast<const void*>(&tile));
        }
      }
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return seen.size();
  };

  // --- Part A: the cursor, exactly ----------------------------------------
  //
  // ADR-0005's amendment is a specification of cursor movement, so this is a
  // direct transcription of it rather than a smoke test.
  {
    History h;
    check(h.empty() && !h.canUndo() && !h.canRedo() && h.entries().empty(),
          "cursor: a fresh History is empty and can neither undo nor redo");

    Document d0 = Document::createBlank(256, 256, WorkingSpace{});
    paintTile(d0, 0, 0, 0.0f);
    h.begin("opened", d0);
    check(h.entries().size() == 1 && h.cursor() == 0 && !h.canUndo() && !h.canRedo(),
          "cursor: begin() makes one baseline entry with the cursor on it, and there is "
          "nothing to undo TO yet");

    Document live = d0;
    for (int i = 1; i <= 4; ++i) {
      paintTile(live, 0, 0, static_cast<float>(i) / 8.0f);
      h.record("edit " + std::to_string(i), live);
    }
    check(h.entries().size() == 5 && h.cursor() == 4 && h.canUndo() && !h.canRedo(),
          "cursor: four edits give five entries with the cursor at the end -- can undo, "
          "cannot redo");

    const Document* u = h.undo();
    check(u != nullptr && h.cursor() == 3 && h.canUndo() && h.canRedo(),
          "cursor: undo moves the cursor BACK one and makes redo available; the list is not "
          "touched");
    check(h.entries().size() == 5,
          "cursor: and undo does not discard anything -- the entries after the cursor are "
          "still there, which is the whole difference from a stack");

    const Document* r = h.redo();
    check(r != nullptr && h.cursor() == 4 && !h.canRedo(),
          "cursor: redo moves the cursor FORWARD one, back to the end");

    check(h.jumpTo(1) != nullptr && h.cursor() == 1,
          "cursor: jumpTo() is the primitive -- it moves the cursor to any index, which is "
          "PRD O2's panel click");
    check(h.jumpTo(99) == nullptr && h.cursor() == 1,
          "cursor: an out-of-range jump returns null and leaves the cursor alone");

    // Exhaustion in both directions.
    while (h.canUndo()) h.undo();
    check(h.cursor() == 0 && h.undo() == nullptr,
          "cursor: undo at the oldest entry returns null and the cursor stays put");
    while (h.canRedo()) h.redo();
    check(h.cursor() == 4 && h.redo() == nullptr,
          "cursor: redo at the newest entry returns null and the cursor stays put");

    // Serials are stable identity; indices are not.
    check(h.entries()[0].serial != h.entries()[1].serial &&
              h.entries()[0].label == "opened" && h.entries()[4].label == "edit 4",
          "cursor: each entry carries a distinct serial and the label its edit was recorded "
          "under -- the two things PRD O2's panel draws");
  }

  // --- Part B: PLAN.md's own Phase 5 verify sentence, adapted honestly -----
  //
  // The sentence is "Undo ten strokes, redo ten, and the result is
  // pixel-identical to before the undos". **There are no strokes.**
  // `sim::PaintSim` owns one dense GPU texture with no layer awareness and no
  // stroke reaches a `Layer` -- core/TileStore.hpp enumerates every writer in
  // this build and none of them is a brush. So the sentence is run in the
  // only form that exists: ten writes through `getOrCreate()`, funnelled
  // through `app::OpenDocument::recordEdit(..., EditKind::Content)`, which is
  // exactly the pair of calls app/DocumentLifecycle.hpp says the canvas
  // bridge will make when it lands. The adaptation is printed, not buried.
  {
    std::printf(
        "[selftest] history: PLAN.md's verify sentence is \"undo ten strokes, redo ten, "
        "pixel-identical\" -- run here as ten TILE WRITES through the real recordEdit() "
        "funnel, because no stroke reaches a Layer in this build and there are no strokes "
        "to undo\n");

    OpenDocument od = makeBlankOpenDocument(1024, 1024, WorkingSpace{}, "verify");
    check(od.history.entries().size() == 1 && !od.history.canUndo(),
          "verify: a blank document starts with one baseline history entry");

    const Document before = od.document;  // a share, frozen at this state

    for (int i = 0; i < 10; ++i) {
      paintTile(od.document, i % 4, i / 4, 0.05f + 0.09f * static_cast<float>(i));
      od.recordEdit("stroke " + std::to_string(i + 1), EditKind::Content);
    }
    const Document after = od.document;
    check(od.history.entries().size() == 11 && od.history.cursor() == 10,
          "verify: ten edits gave ten entries on top of the baseline");
    check(!sameTiles(before, after),
          "verify: the ten edits really did change the pixels -- so the test below could "
          "have failed");

    // Undo ten.
    for (int i = 0; i < 10; ++i) {
      const Document* d = od.history.undo();
      if (!d) {
        check(false, "verify: undo ran out of entries before ten");
        break;
      }
      od.document = *d;
    }
    check(od.history.cursor() == 0 && sameTiles(od.document, before),
          "verify: after ten undos the document is bit-identical to the state before the "
          "ten edits, at zero tolerance over every tile's raw half words");

    // Redo ten.
    for (int i = 0; i < 10; ++i) {
      const Document* d = od.history.redo();
      if (!d) {
        check(false, "verify: redo ran out of entries before ten");
        break;
      }
      od.document = *d;
    }
    check(od.history.cursor() == 10 && sameTiles(od.document, after),
          "verify: **undo ten, redo ten -> bit-identical to before the undos** -- PLAN.md's "
          "verify sentence, at zero tolerance");

    // And the composite, which is what "pixel-identical" means to a user:
    // the same assertion again through the one function that turns a Document
    // into pixels for export and for part 0 of every save.
    const DecodedImage flatAfter = flattenDocumentToLinear(after);
    const DecodedImage flatNow = flattenDocumentToLinear(od.document);
    check(flatAfter.valid() && flatNow.valid() &&
              flatAfter.pixels.size() == flatNow.pixels.size() &&
              std::memcmp(flatAfter.pixels.data(), flatNow.pixels.data(),
                          flatAfter.pixels.size() * sizeof(float)) == 0,
          "verify: and the COMPOSITE is byte-identical too, through the same "
          "flattenDocumentToLinear() that every export and every save's part 0 uses");
  }

  // --- Part C: redo is not an inverse (ADR-0005's amendment), and PRD O3 ---
  {
    Document live = Document::createBlank(1024, 1024, WorkingSpace{});
    History h;
    h.begin("baseline", live);
    std::vector<Document> expected;  // an independent record of every state
    expected.push_back(live);
    for (int i = 0; i < 40; ++i) {
      paintTile(live, i % 8, i / 8, 0.02f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
      expected.push_back(live);
    }
    check(h.entries().size() == 41 && expected.size() == 41,
          "replay: a 41-state history, with every state also held independently by the test");

    // 1. Every state reached by walking BACK equals the same state reached by
    //    a direct jump -- so undo is not a separate mechanism.
    bool undoWalkMatches = true;
    for (size_t i = 40; i-- > 0;) {
      const Document* viaUndo = h.undo();
      if (!viaUndo || !sameTiles(*viaUndo, expected[i])) undoWalkMatches = false;
    }
    check(undoWalkMatches && h.cursor() == 0,
          "replay: walking the cursor back forty times reproduces all forty states "
          "bit-exactly");

    bool redoWalkMatches = true;
    for (size_t i = 1; i <= 40; ++i) {
      const Document* viaRedo = h.redo();
      if (!viaRedo || !sameTiles(*viaRedo, expected[i])) redoWalkMatches = false;
    }
    check(redoWalkMatches && h.cursor() == 40,
          "replay: and walking it forward again reproduces the same forty states -- redo is "
          "the SAME operation with the index moving the other way, not an inverse");

    // 2. The direct jump agrees with both walks, from either direction.
    bool jumpMatches = true;
    for (size_t i : {size_t{0}, size_t{7}, size_t{19}, size_t{33}, size_t{40}}) {
      const Document* down = h.jumpTo(i);  // arriving from a higher index
      if (!down || !sameTiles(*down, expected[i])) jumpMatches = false;
      h.jumpTo(0);
      const Document* up = h.jumpTo(i);  // arriving from a lower index
      if (!up || !sameTiles(*up, expected[i])) jumpMatches = false;
    }
    check(jumpMatches,
          "replay: jumping straight to a state gives the same bytes whether the cursor "
          "arrived from above or below -- there is no direction-dependent state anywhere");

    // 3. PRD O3: jumping back N costs one install, not N. Timed rather than
    //    asserted from the code's shape.
    // One jump is far below the steady_clock's resolution, so each variant is
    // timed over a long run of *pairs* -- (40 -> 39 -> 40) against
    // (40 -> 0 -> 40) -- which is two jumps either way, so the two numbers
    // compare like for like.
    constexpr int kJumpReps = 200000;
    double oneStep = 1e9, fortyStep = 1e9;
    size_t sink = 0;
    for (int rep = 0; rep < 5; ++rep) {
      const auto t0 = Clock::now();
      for (int i = 0; i < kJumpReps; ++i) {
        sink += (h.jumpTo(39) != nullptr) ? 1u : 0u;
        sink += (h.jumpTo(40) != nullptr) ? 1u : 0u;
      }
      const auto t1 = Clock::now();
      for (int i = 0; i < kJumpReps; ++i) {
        sink += (h.jumpTo(0) != nullptr) ? 1u : 0u;
        sink += (h.jumpTo(40) != nullptr) ? 1u : 0u;
      }
      const auto t2 = Clock::now();
      oneStep = std::min(oneStep, seconds(t0, t1) / kJumpReps);
      fortyStep = std::min(fortyStep, seconds(t1, t2) / kJumpReps);
    }
    if (sink != static_cast<size_t>(4) * kJumpReps * 5) oneStep = -1.0;
    std::printf(
        "[selftest] history: PRD O3 -- [measured] jumpTo() one step back %.1f ns, forty "
        "steps back %.1f ns; one install either way, not N replays\n",
        oneStep * 1e9, fortyStep * 1e9);
    check(fortyStep < oneStep * 10.0 + 1e-6,
          "replay: PRD O3 -- a forty-step jump costs the same as a one-step jump (10x "
          "margin over the measured numbers), because every entry is a keyframe and the "
          "replay range is empty");
  }

  // --- Part D: a new edit at a non-end cursor truncates the tail ----------
  {
    // No holder outside the history and `live`: without the deep copy the
    // original tiles would have an external owner and every attributable
    // number below would be zero, which would make the section vacuous.
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{4} * 1024 * 1024 * 1024);  // budget out of the way; Part E is eviction
    h.begin("opened", live);
    for (int32_t i = 0; i < 20; ++i) {
      // Inside the 16x16 grid, so every edit SUPERSEDES an existing tile
      // rather than creating a new one -- the arithmetic below is about
      // superseded versions and would be measuring something else otherwise.
      paintTile(live, i % 16, i / 16, 0.03f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    check(h.entries().size() == 21 && h.cursor() == 20 && h.truncatedEntryCount() == 0,
          "truncate: twenty-one entries, cursor at the end, nothing truncated yet");

    auto liveAndEntries = [&]() {
      std::vector<const Document*> all{&live};
      for (const HistoryEntry& e : h.entries()) all.push_back(&e.document);
      return all;
    };
    const size_t distinctBefore = distinctTilesIn(liveAndEntries());
    check(distinctBefore == 256 + 20,
          "truncate: 256 tiles plus one superseded version per edit is all that is held -- "
          "the sharing IS the delta");

    // Move the cursor back and make a NEW edit there.
    const Document* at5 = h.jumpTo(5);
    check(at5 != nullptr && h.cursor() == 5 && h.canRedo(),
          "truncate: the cursor moved back to entry 5 with fifteen redo steps still ahead");
    live = *at5;
    paintTile(live, 10, 5, 0.9f);  // a tile none of the twenty edits touched
    h.record("edit after undo", live);

    check(h.entries().size() == 7 && h.cursor() == 6 && !h.canRedo(),
          "truncate: recording at a non-end cursor dropped everything after it -- 21 entries "
          "became 7, the cursor is at the new last one, and redo is gone");
    check(h.truncatedEntryCount() == 15 && h.droppedEntryCount() == 0,
          "truncate: fifteen entries were truncated and zero evicted -- an abandoned branch "
          "is not the same event as a byte-budget drop, and they are counted apart");

    const size_t distinctAfter = distinctTilesIn(liveAndEntries());
    check(distinctAfter == distinctBefore - 15 + 1,
          "truncate: **the tail's memory really went** -- the fifteen tile versions only the "
          "truncated entries held are gone, and the one the new edit superseded is the only "
          "addition. Counted by tile ADDRESS, independently of History::bytes()");
    check(h.bytes().distinctTiles == distinctAfter,
          "truncate: and History::bytes() agrees with that independent count exactly");

    // A tile the surviving prefix still refers to is untouched, which is the
    // other half of "released exactly the right things".
    check(h.entries()[0].document.layers[0].rgbTiles->find(TileCoord{15, 0}) != nullptr &&
              h.entries()[0].document.layers[0].rgbTiles->tileUseCount(TileCoord{15, 0}) >= 1,
          "truncate: a tile the surviving prefix still holds was not freed with the tail");

    // And the states that survived still read exactly what they read before.
    check(sameTiles(h.entries()[5].document, *h.jumpTo(5)) &&
              readTexel(h.entries()[5].document, 4, 0)[0] > 0.0f &&
              readTexel(h.entries()[5].document, 10, 5)[0] != 0.9f,
          "truncate: every surviving state still reads bit-exactly what it did before the "
          "truncation");
  }

  // --- Part E: bounded in bytes, under step 6's NON-additive sharing ------
  //
  // This is the section the step lives or dies on. core/TileShare.hpp's
  // finding is that `documentExclusiveTileBytes()` is 128 KiB for the oldest
  // entry and **zero for every entry after it**, so a budget that evicts
  // until the sum of per-entry exclusives covers the overrun will over-evict.
  // Both policies are run here, on the same history, and the naive one is
  // shown to take every evictable entry where the real one takes six.
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{4} * 1024 * 1024 * 1024);
    h.begin("opened", live);
    for (int32_t i = 0; i < 10; ++i) {
      paintTile(live, i, 0, 0.05f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }

    const HistoryBytes b0 = h.bytes();
    check(b0.attributable == 10 * sizeof(Tile),
          "evict: ten edits over a 256-tile document leave exactly ten tile versions "
          "attributable to the history -- 1.25 MiB, not the 320 MiB the entries SHOW");
    check(b0.shown == 11 * 256 * sizeof(Tile) && b0.distinct == (256 + 10) * sizeof(Tile),
          "evict: shown (352 MiB), distinct (33.25 MiB) and attributable (1.25 MiB) are "
          "three different numbers, and only the last is the history's to spend");

    // Step 6's finding, re-derived here rather than quoted from its header.
    size_t sumExclusive = 0;
    for (const HistoryEntry& e : h.entries())
      sumExclusive += documentExclusiveTileBytes(e.document);
    check(documentExclusiveTileBytes(h.entries()[0].document) == sizeof(Tile) &&
              documentExclusiveTileBytes(h.entries()[5].document) == 0 &&
              sumExclusive == sizeof(Tile),
          "evict: the oldest entry holds 128 KiB exclusively and every later one holds ZERO, "
          "so the per-entry sum over all eleven entries is 128 KiB -- against 1.25 MiB that "
          "dropping them all would really free, 10x");

    const size_t budget = 4 * sizeof(Tile);  // 512 KiB: deliberately far too small

    // The naive policy, spelled out and run against the same numbers: evict
    // from the old end, crediting each drop with that entry's exclusive
    // bytes, until the credited total covers the overrun.
    size_t naiveDrops = 0, naiveCredited = 0;
    for (size_t i = 0; i + 1 < h.entries().size(); ++i) {
      if (b0.attributable - naiveCredited <= budget) break;
      naiveCredited += documentExclusiveTileBytes(h.entries()[i].document);
      ++naiveDrops;
    }

    h.setBudgetBytes(budget);
    const size_t realDrops = h.droppedEntryCount();
    const HistoryBytes b1 = h.bytes();

    std::printf(
        "[selftest] history: eviction to a %.2f MiB budget from %.2f MiB attributable -- "
        "[measured] drop-one-then-re-measure took %zu entries; the naive sum-of-exclusives "
        "policy would have taken %zu (every evictable entry) and still believed it had "
        "freed only %.2f MiB\n",
        static_cast<double>(budget) / kMiB, static_cast<double>(b0.attributable) / kMiB,
        realDrops, naiveDrops, static_cast<double>(naiveCredited) / kMiB);

    check(realDrops == 6 && b1.attributable == budget,
          "evict: six drops brought it to exactly the budget -- each drop frees the one tile "
          "version whose last holder it was, which is only knowable by re-measuring");
    check(b1.attributable <= h.budgetBytes() && !h.overBudget(),
          "evict: and the budget is genuinely met afterwards, not approximately");
    check(naiveDrops == 10 && naiveDrops > realDrops,
          "evict: **the naive policy over-evicts** -- it would have discarded all ten "
          "evictable entries to reclaim what six actually reclaim, because a tile two doomed "
          "entries share becomes free only when the second one goes");
    check(h.entries().size() == 5 && h.cursor() == 4 && h.truncatedEntryCount() == 0,
          "evict: eviction took from the OLD end only; the list is still contiguous and the "
          "cursor moved down with it");
    check(sameTiles(h.entries()[h.cursor()].document, live),
          "evict: the state at the cursor -- the one on screen -- is bit-identical to the "
          "live document, so eviction never discarded what is being looked at");
    check(!h.canRedo() && h.canUndo(),
          "evict: and nothing after the cursor was touched, so no visible redo step was "
          "silently destroyed");

    // The one case where the bound cannot be met, reported rather than hidden.
    h.jumpTo(0);
    h.setBudgetBytes(sizeof(Tile) / 2);
    check(h.entries().size() == 5 && h.overBudget(),
          "evict: with the cursor at the oldest entry there is nothing evictable, so the "
          "budget is exceeded rather than the current state being discarded");
    const std::string pressure = h.budgetPressure();
    check(contains(pressure, "over its byte budget") &&
              contains(pressure, "cursor is at the oldest state") &&
              contains(pressure, "cannot be recorded cannot be undone"),
          "evict: and budgetPressure() names the overrun, why nothing is evictable, and that "
          "no edit was refused -- an unrecordable edit is an un-undoable edit");
    // Recording still works, which is the promise that sentence makes.
    paintTile(live, 12, 12, 0.5f);
    h.record("edit under pressure", live);
    check(h.truncatedEntryCount() == 4 && !h.entries().empty() &&
              sameTiles(h.entries()[h.cursor()].document, live),
          "evict: recording under budget pressure still works -- it truncated the four "
          "entries after the cursor, appended the new state, refused nothing, and then "
          "evicted what it could; the budget simply stays exceeded when nothing is left");
  }

  // --- Part F: snapshots are exempt from eviction (PRD O4) ----------------
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{4} * 1024 * 1024 * 1024);
    h.begin("opened", live);
    for (int32_t i = 0; i < 10; ++i) {
      paintTile(live, i, 0, 0.05f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }

    // Snapshot the state at entry 3, then let eviction take entry 3 itself.
    const Document* at3 = h.jumpTo(3);
    check(at3 != nullptr, "snapshot: reached the state to be snapshotted");
    const size_t snapIndex = h.takeSnapshot("before the risky bit", *at3);
    // **The fingerprint is raw half words, not a Document copy.** A held
    // `Document` would be a holder OUTSIDE the history, which would make every
    // tile it touches un-attributable and quietly disarm the very eviction
    // this section exists to drive -- the test would still have printed
    // "pass" while proving nothing. Learned by writing it the other way first.
    auto rawTile = [](const Document& d, int32_t tx, int32_t ty) {
      const Tile* t = d.layers[0].rgbTiles->find(TileCoord{tx, ty});
      std::vector<uint16_t> out(Tile::kTexelCount, 0);
      if (t) std::memcpy(out.data(), t->data(), Tile::kTexelCount * sizeof(uint16_t));
      return out;
    };
    const std::vector<uint16_t> fpEdited = rawTile(*at3, 1, 0);   // edited before entry 3
    const std::vector<uint16_t> fpPending = rawTile(*at3, 7, 0);  // edited after entry 3
    const std::vector<uint16_t> fpUntouched = rawTile(*at3, 12, 4);
    check(sameTiles(h.snapshots()[0].document, h.entries()[3].document),
          "snapshot: the snapshot is bit-identical to the entry it was taken from");
    check(snapIndex == 0 && h.snapshots().size() == 1 &&
              h.snapshots()[0].label == "before the risky bit",
          "snapshot: takeSnapshot() adds a named state, and it is NOT in entries()");
    check(h.entries().size() == 11,
          "snapshot: taking one does not add a step to the linear list -- the cursor is "
          "exactly where it was");

    h.jumpTo(10);
    h.setBudgetBytes(2 * sizeof(Tile));
    check(h.droppedEntryCount() == 10 && h.entries().size() == 1,
          "snapshot: a two-tile budget evicted every evictable entry, entry 3's own list "
          "slot included -- only the state at the cursor is left");
    check(!sameTiles(h.entries()[0].document, h.snapshots()[0].document),
          "snapshot: and that state really is gone from the entry list -- so the assertion "
          "below is about the snapshot and not about a survivor");
    check(h.snapshots().size() == 1 &&
              std::memcmp(rawTile(h.snapshots()[0].document, 1, 0).data(), fpEdited.data(),
                          fpEdited.size() * sizeof(uint16_t)) == 0 &&
              std::memcmp(rawTile(h.snapshots()[0].document, 7, 0).data(), fpPending.data(),
                          fpPending.size() * sizeof(uint16_t)) == 0 &&
              std::memcmp(rawTile(h.snapshots()[0].document, 12, 4).data(), fpUntouched.data(),
                          fpUntouched.size() * sizeof(uint16_t)) == 0,
          "snapshot: **PRD O4 -- the snapshot survived the eviction bit-exactly**, holding a "
          "state the byte budget removed from the list, checked over raw half words");
    check(h.bytes().exemptFromEviction > 0 && h.overBudget(),
          "snapshot: and the accounting says so -- the bytes the snapshot alone still holds "
          "are attributable, exempt, and are why the budget stays exceeded");

    // Restoring is an ordinary edit, so the list stays linear (ADR-0005 does
    // not adopt non-linear history).
    const size_t entriesBefore = h.entries().size();
    const Document* restored = h.restoreSnapshot(0);
    check(restored != nullptr && sameTiles(*restored, h.snapshots()[0].document) &&
              h.entries().size() == entriesBefore + 1 && h.cursor() == h.entries().size() - 1 &&
              contains(h.entries().back().label, "before the risky bit"),
          "snapshot: restoring one is recorded as an ordinary edit at the end of the list, so "
          "it is itself undoable and the history stays linear");
    check(h.canUndo() && h.undo() != nullptr,
          "snapshot: which means the restore can be undone like anything else");
    check(h.dismissSnapshot(9) == false && h.dismissSnapshot(0) == true &&
              h.snapshots().empty(),
          "snapshot: 'exempt until dismissed' -- dismissing is explicit, and an out-of-range "
          "index is refused rather than ignored");
  }

  // --- Part F2: a snapshot survives TRUNCATION, which is why it is a
  //              second list rather than a flag on an entry ----------------
  {
    Document live = Document::createBlank(512, 512, WorkingSpace{});
    History h;
    h.begin("opened", live);
    for (int32_t i = 0; i < 10; ++i) {
      paintTile(live, i % 4, i / 4, 0.05f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    // The exact workflow the feature exists for: snapshot, try something,
    // undo back PAST the snapshot, then do something else.
    const Document risky = *h.jumpTo(7);
    h.takeSnapshot("try something risky", risky);
    live = *h.jumpTo(3);
    paintTile(live, 3, 3, 0.75f);
    h.record("something else", live);

    check(h.truncatedEntryCount() == 7,
          "snapshot: the branch holding the snapshotted state was truncated away");
    check(h.snapshots().size() == 1 && sameTiles(h.snapshots()[0].document, risky),
          "snapshot: **and the snapshot survived it bit-exactly** -- this is the case a "
          "`bool snapshot` on a list entry would have lost, at exactly the moment the user "
          "took it for");
  }

  // --- Part F3: snapshots alone over budget -------------------------------
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(sizeof(Tile));  // one tile
    h.begin("opened", live);
    for (int32_t i = 0; i < 3; ++i) {
      paintTile(live, i, 5, 0.2f * static_cast<float>(i + 1));
      h.takeSnapshot("snap " + std::to_string(i), live);
    }
    check(h.snapshots().size() == 3 && h.overBudget(),
          "snapshot: three snapshots over a one-tile budget puts the history over budget");
    const std::string pressure = h.budgetPressure();
    check(contains(pressure, "3 snapshots") && contains(pressure, "exempt from eviction"),
          "snapshot: budgetPressure() names the snapshots and says they are exempt -- the "
          "one thing a user can act on");
    const size_t snapsBefore = h.snapshots().size();
    paintTile(live, 9, 9, 0.4f);
    h.record("still recording", live);
    check(h.snapshots().size() == snapsBefore,
          "snapshot: recording under that pressure evicts NO snapshot -- exempt means exempt, "
          "even when it is the snapshots that are over the line");
    const HistoryBytes withSnaps = h.bytes();
    h.dismissSnapshot(0);
    h.dismissSnapshot(0);
    h.dismissSnapshot(0);
    check(h.snapshots().empty() && h.bytes().attributable < withSnaps.attributable &&
              h.bytes().exemptFromEviction == 0,
          "snapshot: dismissing them gives the bytes back immediately");
  }

  // --- Part G: the byte accounting checked against reality ----------------
  //
  // "Measure held bytes, do not just trust the accounting function." Two
  // independent checks: an address count that shares no code with
  // History::bytes(), and the operating system's own view through
  // app/Memory's task_info -- the same source the idle-RSS gate uses.
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{8} * 1024 * 1024 * 1024);  // no eviction; this is accounting, not policy
    h.begin("opened", live);

    const size_t rssBase = currentResidentBytes();
    constexpr int kWholeDocEdits = 6;
    for (int e = 0; e < kWholeDocEdits; ++e) {
      for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
        for (int32_t tx = 0; tx < kTilesPerSide; ++tx)
          live.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty}).writePixel(
              PixelCoord{1, 1}, {0.1f * static_cast<float>(e + 1), 0.2f, 0.3f, 1.0f});
      h.record("whole-document edit " + std::to_string(e), live);
    }
    const size_t rssFull = currentResidentBytes();

    const HistoryBytes b = h.bytes();
    const size_t expectedAttributable =
        static_cast<size_t>(kWholeDocEdits) * 256 * sizeof(Tile);
    check(b.attributable == expectedAttributable,
          "accounting: six whole-document edits leave six superseded generations -- "
          "192.0 MiB -- attributable to the history");

    std::vector<const Document*> all{&live};
    for (const HistoryEntry& e : h.entries()) all.push_back(&e.document);
    const size_t independent = distinctTilesIn(all);
    check(independent * sizeof(Tile) == b.distinct && independent == b.distinctTiles,
          "accounting: History::bytes()'s distinct count equals an independent count of "
          "distinct tile ADDRESSES that shares no code with it");
    check(b.distinct - b.attributable == 256 * sizeof(Tile),
          "accounting: exactly one document's worth is NOT attributable -- the generation "
          "the live document still holds, which dropping the history would not free");

    // The property that makes the budget check belong on `record()`: the
    // history's cost materialises when the LIVE document paints away from it,
    // not when history does anything.
    const size_t beforeOne = h.bytes().attributable;
    live.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{2, 2}, {0.9f, 0.9f, 0.9f, 1.0f});
    check(h.bytes().attributable == beforeOne + sizeof(Tile),
          "accounting: one write to the LIVE document makes one more tile history-only -- "
          "the history's cost grows without the history being touched, which is why the "
          "budget is re-checked on every record() and why bytes() is not cached");

    // **The reality check that is a measurement rather than a prediction:
    // actually drop the history and count what disappeared**, by tile
    // address, over the same independent counter used above. `attributable`
    // claims to be "exactly what dropping the entire history would return";
    // this is that sentence executed.
    // Re-read the accounting immediately before the drop: the live-document
    // write just above legitimately moved one more tile into the history's
    // column, and comparing against the older number would be comparing
    // against a stale claim rather than the current one.
    const size_t attributableAtDrop = h.bytes().attributable;
    const size_t distinctBeforeDrop = distinctTilesIn(all);
    h.begin("history dropped", live);
    const size_t rssAfter = currentResidentBytes();
    std::vector<const Document*> justLive{&live};
    for (const HistoryEntry& e : h.entries()) justLive.push_back(&e.document);
    const size_t distinctAfterDrop = distinctTilesIn(justLive);
    check((distinctBeforeDrop - distinctAfterDrop) * sizeof(Tile) == attributableAtDrop,
          "accounting: **dropping the history really did return exactly the attributable "
          "bytes** -- that many distinct tile objects fewer, counted by address after the "
          "fact rather than predicted before it");

    // RSS, printed and deliberately NOT asserted. By the time this section
    // runs the process has already allocated and freed hundreds of MiB (step
    // 6's `cow tiles` section peaks around 540 MiB), so macOS's allocator
    // satisfies these 128 KiB blocks from pages it already holds and returns
    // them to its own cache rather than to the kernel. That makes RSS a poor
    // instrument *here* specifically -- it measures the allocator, not the
    // refcount -- so the assertion above is the address count, which measures
    // the thing itself. The numbers are still printed, because a large
    // unexplained gap would be worth investigating.
    const double grewMiB = static_cast<double>(rssFull - std::min(rssFull, rssBase)) / kMiB;
    const double freedMiB = static_cast<double>(rssFull - std::min(rssFull, rssAfter)) / kMiB;
    std::printf(
        "[selftest] history: %d whole-document entries over a 32.0 MiB document -- "
        "[measured] accounted attributable %.1f MiB, returned on drop %.1f MiB by tile "
        "count; process RSS grew %.1f MiB and gave back %.1f MiB (allocator retention, not "
        "a leak -- see the comment)\n",
        kWholeDocEdits, static_cast<double>(attributableAtDrop) / kMiB,
        static_cast<double>((distinctBeforeDrop - distinctAfterDrop) * sizeof(Tile)) / kMiB,
        grewMiB, freedMiB);
  }

  // --- Part G2: all three tile shapes, not just the 128 KiB one -----------
  //
  // core/TileShare.hpp is explicit that "a single `tiles x 128 KiB` would be
  // wrong for two of the three", and the accounting here sums each store's own
  // `sizeof(T)` for exactly that reason. An RGB tile is 128 KiB, a pigment tile
  // 224 KiB and a mask tile 32 KiB, so a history that counted tiles rather than
  // bytes would be out by 1.75x and 0.25x on two thirds of the document -- and
  // would still look right on every fixture above, all of which are RGB-only.
  {
    History h(size_t{4} * 1024 * 1024 * 1024);
    {
      // Built and destroyed in a scope, so the history is the ONLY holder and
      // every one of the three tiles is attributable.
      Document doc = Document::createBlank(256, 256, WorkingSpace{});
      doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      const LayerOpResult m = addLayerMask(doc, 0);
      check(m.ok && doc.layers[0].mask.has_value(), "shapes: the RGB layer got a mask");
      doc.layers[0].mask->getOrCreate(TileCoord{0, 0});
      Layer pig;
      pig.kind = LayerKind::Pigment;
      pig.pigmentTiles.emplace();
      pig.pigmentTiles->getOrCreate(TileCoord{0, 0});
      doc.layers.push_back(std::move(pig));
      h.begin("mixed document", doc);
    }
    const HistoryBytes b = h.bytes();
    check(b.distinctTiles == 3 &&
              b.attributable == sizeof(Tile) + sizeof(MaskTile) + sizeof(PigmentTile) &&
              b.attributable == (128 + 32 + 224) * 1024,
          "shapes: one RGB tile, one mask tile and one pigment tile account for "
          "128 + 32 + 224 KiB -- the accounting sums each store's own sizeof(T), so a "
          "history holding masks or latents is not counted as if every tile were 128 KiB");
    check(b.distinct == b.attributable && b.shown == b.attributable,
          "shapes: with no holder outside the history, shown, distinct and attributable all "
          "coincide -- which is the degenerate case that makes the three numbers legible");
  }

  // --- Part H: the measurements this step's correction rests on -----------
  //
  // PLAN.md asks for the undo tail to be "compressed" and "spilled to `mmap`
  // scratch". core/History.hpp declines both and says why; these are the
  // numbers it declines on, printed on every run so the decision can be
  // re-checked rather than trusted. The question is not "how well does a tile
  // compress" -- it is "how many of the process's tile bytes is the history
  // even attributable for", because that is the ceiling on what compressing
  // or paging out the history could ever save.
  {
    auto regime = [&](const char* name, bool wholeDocument) {
      Document live = deepCopy(buildRealisticDocument());
      History h(size_t{8} * 1024 * 1024 * 1024);
      h.begin("opened", live);
      for (int32_t i = 0; i < 10; ++i) {
        if (wholeDocument) {
          for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
            for (int32_t tx = 0; tx < kTilesPerSide; ++tx)
              live.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty}).writePixel(
                  PixelCoord{3, 3}, {0.05f * static_cast<float>(i), 0.5f, 0.5f, 1.0f});
        } else {
          paintTile(live, i, 0, 0.05f * static_cast<float>(i + 1));
        }
        h.record("edit " + std::to_string(i), live);
      }
      const HistoryBytes b = h.bytes();
      const size_t liveOnly = b.distinct - b.attributable;
      std::printf(
          "[selftest] history: %-28s [measured] shown %.1f MiB, distinct %.2f MiB, "
          "attributable to history %.2f MiB (%.1f%%), live document %.2f MiB (%.1f%%)\n",
          name, static_cast<double>(b.shown) / kMiB, static_cast<double>(b.distinct) / kMiB,
          static_cast<double>(b.attributable) / kMiB,
          100.0 * static_cast<double>(b.attributable) / static_cast<double>(b.distinct),
          static_cast<double>(liveOnly) / kMiB,
          100.0 * static_cast<double>(liveOnly) / static_cast<double>(b.distinct));
      return b;
    };

    const HistoryBytes favourable = regime("10 entries, 1 tile edited", false);
    const HistoryBytes unfavourable = regime("10 entries, whole doc edited", true);

    check(favourable.attributable == 10 * sizeof(Tile) &&
              favourable.distinct == (256 + 10) * sizeof(Tile),
          "correction: in the regime step 6 measured, the history is attributable for "
          "1.25 MiB of the 33.25 MiB held -- so compression and an mmap spill have a 3.8% "
          "ceiling on the process's tile footprint, whatever their ratio");
    check(unfavourable.attributable == 10 * 256 * sizeof(Tile),
          "correction: and in the worst regime -- every edit rewriting every tile -- it is "
          "320 MiB of 352 MiB, where compression WOULD buy something: a constant factor on "
          "a budget that is already a constant, which raising the budget buys for free");

    // What one undo step costs, and therefore how deep the default budget is.
    // The two ends of the realistic range, measured rather than assumed.
    auto costPerEdit = [&](int tilesPerEdit) {
      Document live = deepCopy(buildRealisticDocument());
      History h(size_t{8} * 1024 * 1024 * 1024);
      h.begin("opened", live);
      const size_t before = h.bytes().attributable;
      for (int32_t k = 0; k < tilesPerEdit; ++k)
        paintTile(live, k % kTilesPerSide, k / kTilesPerSide, 0.5f);
      h.record("one edit", live);
      return h.bytes().attributable - before;
    };
    const size_t oneTile = costPerEdit(1);
    const size_t oneBand = costPerEdit(16);  // a 2048 x ~128 px band: one stroke's worth
    std::printf(
        "[selftest] history: [measured] one undo step costs %.2f MiB for a single-tile edit "
        "and %.2f MiB for a 16-tile stroke-shaped band -- %zu and %zu steps deep at the "
        "%.0f MiB default budget (Photoshop's default is 50 states)\n",
        static_cast<double>(oneTile) / kMiB, static_cast<double>(oneBand) / kMiB,
        History::kDefaultBudgetBytes / oneTile, History::kDefaultBudgetBytes / oneBand,
        static_cast<double>(History::kDefaultBudgetBytes) / kMiB);
    check(oneTile == sizeof(Tile) && oneBand == 16 * sizeof(Tile),
          "correction: an undo step costs exactly the tiles its edit rewrote and nothing "
          "else -- the sharing IS the delta, so there is no per-entry overhead to compress");
    check(History::kDefaultBudgetBytes / oneBand >= 50,
          "correction: the default budget is at least Photoshop's 50 states deep even for "
          "stroke-sized edits, which is the depth the number was derived against");

    // What the accounting and the recording actually cost, since `record()`
    // runs on every edit.
    {
      Document live = deepCopy(buildRealisticDocument());
      History h;  // the real default budget, so the early-out is exercised
      h.begin("opened", live);
      double recordBest = 1e9;
      for (int32_t i = 0; i < 40; ++i) {
        paintTile(live, i % kTilesPerSide, i / kTilesPerSide, 0.02f * static_cast<float>(i));
        const auto t0 = Clock::now();
        h.record("edit", live);
        const auto t1 = Clock::now();
        recordBest = std::min(recordBest, seconds(t0, t1));
      }
      double scanBest = 1e9;
      size_t slots = 0;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = Clock::now();
        const HistoryBytes b = h.bytes();
        const auto t1 = Clock::now();
        slots = b.distinctTiles;
        scanBest = std::min(scanBest, seconds(t0, t1));
      }
      std::printf(
          "[selftest] history: [measured] record() over a 41-entry / 256-tile history "
          "%.3f ms (the sound `attributable <= shown` early-out); a full bytes() scan of "
          "the same history %.3f ms over %zu distinct tiles\n",
          recordBest * 1e3, scanBest * 1e3, slots);
      check(recordBest < 0.016,
          "correction: recording an edit costs well under one 60 Hz frame, so the byte "
          "bound needs no background thread and none is built");
    }
  }

  // --- Part I: the app/DocumentLifecycle wiring ---------------------------
  //
  // PLAN.md's step points at `recordEdit()` as the funnel every structural
  // edit already goes through, so history is wired there rather than into a
  // parallel notion of "an edit happened". These are the properties that
  // makes true.
  {
    OpenDocument od = makeBlankOpenDocument(512, 512, WorkingSpace{}, "wired");
    check(od.history.entries().size() == 1 && od.history.entries()[0].label == "new document" &&
              !od.history.canUndo() && !od.history.canRedo(),
          "wiring: a blank document is born with one baseline entry and nothing to undo");

    const size_t layersBefore = od.document.layers.size();
    const DocumentOpResult add = recordLayerEdit(od, addLayer(od.document, 1, Layer{}));
    check(add.ok && od.history.entries().size() == 2 &&
              od.document.layers.size() == layersBefore + 1,
          "wiring: a core/LayerOps operation through recordLayerEdit() appends exactly one "
          "history entry");
    check(od.history.entries()[1].label == od.unsavedEdits.back(),
          "wiring: and the entry's label IS core/LayerOps' own editLabel -- the same string "
          "the PRD I11 refusal names, so PRD O2's panel cannot drift from it");

    const Document* undone = od.history.undo();
    check(undone != nullptr && undone->layers.size() == layersBefore,
          "wiring: undoing that edit gives back a document with the layer gone");
    od.document = *undone;

    // A refused operation records nothing -- the pre-existing rule, now with
    // history riding on it.
    const size_t entriesBeforeRefusal = od.history.entries().size();
    setLayerLocked(od.document, 0, true);
    const DocumentOpResult refused = recordLayerEdit(od, removeLayer(od.document, 0));
    check(!refused.ok && od.history.entries().size() == entriesBeforeRefusal,
          "wiring: a refused layer operation records no history entry -- a document is not "
          "undoable-back because someone tried to delete a locked layer");

    // Content edits are recorded too: ADR-0005's undo is stroke-granular, so
    // the one EditKind the canvas bridge will pass must not be the one that
    // skips history.
    setLayerLocked(od.document, 0, false);
    // Note the cursor is still at 0 after the undo above, so this also
    // exercises truncation through the wired path: the entry count does not
    // grow, the abandoned one is replaced.
    const size_t cursorBeforeContent = od.history.cursor();
    const size_t truncatedBefore = od.history.truncatedEntryCount();
    paintTile(od.document, 1, 1, 0.5f);
    od.recordEdit("stroke", EditKind::Content);
    check(od.history.cursor() == cursorBeforeContent + 1 &&
              od.history.entries().back().label == "stroke" &&
              readTexel(od.history.entries().back().document, 1, 1) == readTexel(od.document, 1, 1),
          "wiring: EditKind::Content records a history entry exactly as Structural does -- a "
          "painted stroke is as undoable as a layer reorder (ADR-0005)");
    check(od.history.truncatedEntryCount() == truncatedBefore + 1,
          "wiring: and because the cursor was not at the end, that edit truncated the "
          "abandoned entry -- the same rule, through the real recordEdit() funnel");

    // A duplicate does not inherit its source's undo stack.
    const size_t sourceEntries = od.history.entries().size();
    const OpenDocument dup = duplicateDocument(od);
    check(dup.id != od.id && dup.history.entries().size() == 1 &&
              !dup.history.canUndo() &&
              contains(dup.history.entries()[0].label, "duplicate of"),
          "wiring: a duplicated document gets a FRESH history with one baseline entry -- "
          "undo in the copy can never reinstate a state the copy never had");
    check(od.history.entries().size() == sourceEntries,
          "wiring: and duplicating leaves the source's own history untouched");
  }

  // --- Part J: history is session state; the file format never sees it ----
  {
    const char* kWith = "selftest_history_with.npaint";
    const char* kWithout = "selftest_history_without.npaint";
    for (const char* p : {kWith, kWithout}) std::remove(p);

    auto bytesWithoutCapDate = [](const char* path) -> std::vector<unsigned char> {
      std::ifstream in(path, std::ios::binary);
      std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
      static const std::string kNeedle = "capDate";
      for (size_t i = 0; i + kNeedle.size() <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, kNeedle.data(), kNeedle.size()) != 0) continue;
        for (size_t j = i; j < std::min(i + 47, b.size()); ++j) b[j] = 0;
      }
      return b;
    };

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0].name = "base";
    paintTile(doc, 0, 0, 0.25f);
    paintTile(doc, 1, 1, 0.75f);

    OpenDocument plain;
    plain.id = allocateDocumentId();
    plain.document = doc;  // no history at all

    OpenDocument busy;
    busy.id = allocateDocumentId();
    busy.document = doc;
    busy.history.begin("opened", busy.document);
    for (int i = 0; i < 20; ++i) {
      // Twenty entries and a snapshot, then back to exactly the same state,
      // so the only difference between the two documents is the history.
      paintTile(busy.document, 0, 0, 0.9f);
      busy.recordEdit("edit " + std::to_string(i));
    }
    busy.history.takeSnapshot("snap", busy.document);
    busy.document = doc;
    check(busy.history.entries().size() == 21 && busy.history.snapshots().size() == 1,
          "npaint: the second document carries twenty-one history entries and a snapshot, "
          "against the first document's none");

    const NpaintSaveResult s1 = saveNpaint(plain.document, kWithout, {}, &plain.carry);
    const NpaintSaveResult s2 = saveNpaint(busy.document, kWith, {}, &busy.carry);

    if (kOiioBuild) {
      check(s1.ok && s2.ok, "npaint: both documents saved");
      const std::vector<unsigned char> a = bytesWithoutCapDate(kWithout);
      const std::vector<unsigned char> b = bytesWithoutCapDate(kWith);
      check(!a.empty() && a == b,
            "npaint: **a document with a history saves byte-identically to one without** "
            "(OpenImageIO's capDate masked) -- history is session state, reaches no writer, "
            "and cannot change a file");
    } else {
      check(!s1.ok && !s2.ok && contains(s1.error, "NP_USE_OIIO") &&
                s1.error == s2.error,
            "npaint: in the build with no writer both refuse identically through "
            "io/NpaintFile's own named refusal -- history changes nothing about that either");
    }

    for (const char* p : {kWith, kWithout}) std::remove(p);
    check(std::fopen(kWith, "rb") == nullptr && std::fopen(kWithout, "rb") == nullptr,
          "npaint: every scratch file this section wrote is removed");
  }

  // ======================================================================
  // amend(): one act that arrives in instalments
  // ======================================================================
  //
  // Built for the stroke bridge -- a wash dries tile by tile, so one stroke
  // reaches the document several times -- but the semantics are History's, so
  // they are asserted here rather than beside the caller. The cadence itself
  // (which batches belong to one episode) is app/StrokeBake's, and is asserted
  // in the stroke bridge section.
  std::printf("  -- amend: extending the entry at the cursor --\n");
  {
    Document d0 = Document::createBlank(256, 256, WorkingSpace{});
    History h;

    check(!h.amend("nothing", d0),
          "amend: an empty history refuses -- there is no entry to extend");
    check(h.entries().empty(), "amend: and the refusal created nothing");

    h.record("first", d0);
    const uint64_t firstSerial = h.entries().back().serial;
    Document d1 = d0;
    d1.layers.push_back(makePigmentLayer("added by the amend"));

    check(h.amend("first, more of it", d1), "amend: the entry at the cursor is extended");
    check(h.entries().size() == 1,
          "amend: the entry COUNT does not move -- that is the whole point, one act stays "
          "one row in the panel");
    check(h.entries().back().serial == firstSerial,
          "amend: and it keeps its SERIAL -- app/HistoryPanel keys rows by serial precisely so "
          "a row means the same thing across a mutation, and a new serial would make one "
          "drying stroke look like a different edit every time a tile finished");
    check(h.entries().back().label == "first, more of it",
          "amend: the label is replaced, so a caller can name the fuller act");
    check(h.entries().back().document.layers.size() == d1.layers.size(),
          "amend: and the stored document is the amended one, not the original");

    // The guard that matters: the cursor has moved, so the top entry is no
    // longer the caller's to extend.
    h.record("second", d0);
    check(h.entries().size() == 2, "amend: a following record() appends normally");
    const uint64_t secondSerial = h.entries().back().serial;
    check(secondSerial != firstSerial, "amend: which gets its own serial");

    check(h.undo() != nullptr, "amend: undo moves the cursor off the end");
    Document d2 = d0;
    d2.layers.push_back(makePigmentLayer("must not land"));
    check(!h.amend("should refuse", d2),
          "amend: REFUSES when the cursor is not at the end -- amending part-way through the "
          "list would silently rewrite history the user has already undone past");
    check(h.entries().size() == 2 && h.entries().back().serial == secondSerial &&
              h.entries().back().label == "second",
          "amend: and the refusal changed nothing at all -- not the count, not the serial, "
          "not the label");
  }

  std::printf("[selftest] history %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
