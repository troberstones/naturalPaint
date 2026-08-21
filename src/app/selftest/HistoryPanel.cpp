#include "app/selftest/Support.hpp"

namespace np {

bool runHistoryPanelTest() {
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
  // PLAN.md §1.5: an unexercised build option is not a seam. There is no
  // `#ifdef` around a single assertion in this section, and there is nothing
  // for one to guard: app/HistoryPanel reads a `core::History` and returns
  // strings, so every answer below is the same in both configurations. Said in
  // the output rather than assumed, so the claim is checkable from a log.
  std::printf(
      "[selftest] history panel: NP_USE_OIIO=%s -- every assertion in this section has the "
      "same correct answer in both configurations; nothing here reaches a file, an encoder "
      "or the GPU\n",
      kOiioBuild ? "ON" : "OFF");

  using Clock = std::chrono::steady_clock;
  auto seconds = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
  };

  // --- Tolerances -------------------------------------------------------
  //
  // Every correctness claim here is at **exactly zero tolerance**: a row's
  // serial, a row's state word, a refusal's presence and a cursor's position
  // are all discrete. The one inequality is PRD O3's timing ratio, whose bound
  // is derived at its point of use from the fact that a panel click's work
  // does not read the distance travelled, and which prints the measurement it
  // is asserted against.

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
    for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
      for (int32_t tx = 0; tx < kTilesPerSide; ++tx)
        std::memcpy(tiles.getOrCreate(TileCoord{tx, ty}).data(), base.data(),
                    Tile::kTexelCount * sizeof(uint16_t));
    return doc;
  };

  // The same deep copy the `history` section uses, and for the same reason:
  // without it every original tile has a holder outside the history, nothing
  // is ever attributable, and the eviction this section drives would silently
  // do nothing while every assertion still printed "pass".
  auto deepCopy = [](const Document& src) {
    Document d = src;
    unshareDocumentTiles(d);
    return d;
  };

  auto paintTile = [](Document& doc, int32_t tx, int32_t ty, float v) {
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty});
    for (int32_t k = 0; k < kTileSize; ++k)
      tile.writePixel(PixelCoord{k, k % kTileSize}, {v, 1.0f - v, v * 0.5f, 1.0f});
  };

  // A state's fingerprint as raw half words, never as a held `Document` --
  // step 7's `history` section records why in a comment, having written it the
  // other way first: a held copy is a holder outside the history and disarms
  // the eviction the test exists to drive.
  auto rawTile = [](const Document& d, int32_t tx, int32_t ty) {
    const Tile* t = d.layers[0].rgbTiles->find(TileCoord{tx, ty});
    std::vector<uint16_t> out(Tile::kTexelCount, 0);
    if (t) std::memcpy(out.data(), t->data(), Tile::kTexelCount * sizeof(uint16_t));
    return out;
  };
  auto sameRaw = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 2) == 0;
  };

  // The invariant app/HistoryPanel's binary search rests on, checked wherever
  // the list has just been mutated rather than once at the end.
  auto serialsAscend = [](const std::vector<HistoryEntry>& list) {
    for (size_t i = 1; i < list.size(); ++i)
      if (!(list[i - 1].serial < list[i].serial)) return false;
    return true;
  };

  // --- Part A: row order, and it is the OPPOSITE of app/LayerPanel's -------
  //
  // The one decision a reader coming from app/LayerPanel.hpp will want to
  // "fix". Both orders are asserted here, side by side, so fixing either one
  // fails this section.
  {
    History h;
    Document live = Document::createBlank(256, 256, WorkingSpace{});
    h.begin("opened", live);
    const char* kLabels[] = {"add layer", "duplicate", "opacity", "reorder"};
    for (const char* label : kLabels) {
      paintTile(live, 0, 0, 0.1f);
      h.record(label, live);
    }

    const std::vector<HistoryPanelRow> rows = historyPanelRows(h);
    check(rows.size() == 5 && rows.front().label == "opened" && rows.back().label == "reorder",
          "order: the history panel reads OLDEST at the top and newest at the bottom -- the "
          "baseline is row 0 and the newest edit is the last row");

    bool identity = true;
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].index != i || rows[i].serial != h.entries()[i].serial) identity = false;
    check(identity,
          "order: row N is entry N -- the panel order IS the model order, so there is no "
          "reversal in this file and no second index mapping to get wrong");

    // The contrast, made an assertion rather than a comment: the sibling panel
    // reverses, this one does not, and both are checked in the same breath.
    Document stack = Document::createBlank(64, 64, WorkingSpace{});
    stack.layers.push_back(makeRgbLayer("middle"));
    stack.layers.push_back(makeRgbLayer("top"));
    check(stack.layers.size() == 3 && layerIndexForPanelRow(0, 3) == 2 &&
              panelRowForLayerIndex(0, 3) == 2 && historyRowForSerial(h, rows[0].serial) == 0 &&
              historyRowForSerial(h, rows.back().serial) == 4,
          "order: **the two panels disagree on purpose** -- layers panel row 0 is model index "
          "2 (top of the stack first), history panel row 0 is model index 0 (oldest first)");
    std::printf(
        "[selftest] history panel: layers are a COMPOSITING order shown front-to-back "
        "(row 0 -> layer %zu of 3) and history is a TEMPORAL order shown as it happened "
        "(row 0 -> entry %zu of 5, '%s'); reversing either to match the other is the bug both "
        "headers exist to prevent\n",
        layerIndexForPanelRow(0, 3), historyRowForSerial(h, rows[0].serial),
        rows[0].label.c_str());

    check(historyRowText(rows[0]) == "opened \xC2\xB7 PAST" &&
              historyRowText(rows[4]) == "reorder \xC2\xB7 CURRENT",
          "order: a row reads its label and its state, separated by docs/ui.md's own middle "
          "dot -- and the label is core/LayerOps' editLabel, which is PRD O2's 'by the tool "
          "or op that produced them'");

    HistoryPanelRow blank;
    blank.state = HistoryRowState::Past;
    check(historyRowText(blank) == "(unlabelled edit) \xC2\xB7 PAST",
          "order: an entry with an empty label still gets readable row text -- a blank row is "
          "app/LayerPanel's 'Layer N' rule applied to the other panel");
  }

  // --- Part B: a row is keyed by serial, and the index is not identity ----
  {
    History h;
    Document live = Document::createBlank(256, 256, WorkingSpace{});
    h.begin("opened", live);
    check(serialsAscend(h.entries()), "serial: ascending after begin()");
    for (int i = 0; i < 6; ++i) {
      paintTile(live, i % 4, i / 4, 0.1f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    check(serialsAscend(h.entries()), "serial: ascending after six record()s");

    // Round trip, both directions, over every row.
    bool roundTrips = true;
    for (size_t row = 0; row < h.entries().size(); ++row) {
      const uint64_t s = historySerialForRow(h, row);
      if (s == 0 || historyRowForSerial(h, s) != row) roundTrips = false;
    }
    check(roundTrips,
          "serial: historyRowForSerial() and historySerialForRow() are exact inverses over "
          "every row -- the mapping this file owns, the way app/LayerPanel owns the reversal");

    check(historySerialForRow(h, 99) == 0 && historySerialForRow(h, h.entries().size()) == 0,
          "serial: an out-of-range row yields serial 0, which History never issues, so 'no "
          "row' cannot collide with a real state");
    check(historyRowForSerial(h, 0) == kNoHistoryRow &&
              historyRowForSerial(h, 999999) == kNoHistoryRow,
          "serial: a serial this history does not hold resolves to kNoHistoryRow rather than "
          "to a nearby row");

    // Truncation, then eviction: both mutate the list, neither may disorder it.
    h.jumpTo(2);
    paintTile(live, 7, 7, 0.9f);
    h.record("branch", live);
    check(serialsAscend(h.entries()) && h.truncatedEntryCount() == 4,
          "serial: ascending after a truncation, which erases from the NEW end");
    const size_t snapAt = h.takeSnapshot("snap", live);
    check(snapAt == 0 && serialsAscend(h.snapshots()) &&
              historyRowForSerial(h, h.snapshots()[0].serial) == kNoHistoryRow,
          "serial: a snapshot's serial is not a row on the linear list -- the two lists are "
          "searched apart, exactly as core/History keeps them apart");
    check(h.restoreSnapshot(0) != nullptr && serialsAscend(h.entries()),
          "serial: ascending after restoreSnapshot(), which records an ordinary edit");
  }

  // --- Part C: the stale row -- refused, never redirected -----------------
  //
  // The scenario core/History.hpp warns about in one sentence ("eviction
  // shifts every index down by one, which would silently repoint a panel row
  // or a pending 'jump here' action at a different state"), run for real: the
  // same fixture and the same budget the `history` section's eviction part
  // uses, so the six drops below are the number that section already proves.
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{4} * 1024 * 1024 * 1024);
    h.begin("opened", live);
    for (int32_t i = 0; i < 10; ++i) {
      paintTile(live, i, 0, 0.05f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }

    // The row a panel is holding when the budget bites: index 3, whose state
    // has NOT yet painted tile (3,0).
    constexpr size_t kStaleIndex = 3;
    const uint64_t staleSerial = historySerialForRow(h, kStaleIndex);
    const std::vector<uint16_t> staleFingerprint = rawTile(h.entries()[kStaleIndex].document, 3, 0);
    check(staleSerial != 0 && historyRowForSerial(h, staleSerial) == kStaleIndex,
          "stale: before the eviction, the row resolves to the state the user pointed at");

    h.jumpTo(10);
    h.setBudgetBytes(4 * sizeof(Tile));  // 512 KiB, the `history` section's own number
    check(h.droppedEntryCount() == 6 && h.entries().size() == 5 && serialsAscend(h.entries()),
          "stale: a 0.50 MiB budget dropped six states off the old end and left the list "
          "contiguous and still ascending in serial");

    // **What an index-keyed panel would have done**, spelled out rather than
    // asserted about in a comment: position 3 still exists and now names a
    // different state entirely.
    const uint64_t nowAtStaleIndex = historySerialForRow(h, kStaleIndex);
    const std::vector<uint16_t> nowFingerprint = rawTile(h.entries()[kStaleIndex].document, 3, 0);
    check(nowAtStaleIndex != 0 && nowAtStaleIndex != staleSerial &&
              !sameRaw(nowFingerprint, staleFingerprint),
          "stale: **row index 3 now holds a different state** -- a panel that stored the row "
          "NUMBER would have installed a different picture with nothing on screen to say so");
    std::printf(
        "[selftest] history panel: PRD O2's stale-row trap -- [measured] the budget dropped "
        "%zu states, so what was row %zu (serial %llu) is now serial %llu and a different "
        "picture; an index-keyed click lands there silently, the serial-keyed click refuses\n",
        h.droppedEntryCount(), kStaleIndex, static_cast<unsigned long long>(staleSerial),
        static_cast<unsigned long long>(nowAtStaleIndex));

    // The refusal itself.
    const size_t cursorBefore = h.cursor();
    const std::vector<uint16_t> onScreenBefore = rawTile(h.entries()[cursorBefore].document, 9, 0);
    check(historyRowForSerial(h, staleSerial) == kNoHistoryRow,
          "stale: the serial resolves to no row at all -- not to the nearest surviving one");

    const HistoryPanelClick refused = historyPanelClick(h, staleSerial);
    check(!refused.ok && refused.document == nullptr && refused.cursorMoves == 0 &&
              !refused.appendedEntry,
          "stale: the click is REFUSED -- no document, zero cursor moves, nothing appended");
    check(h.cursor() == cursorBefore && h.entries().size() == 5 &&
              sameRaw(rawTile(h.entries()[h.cursor()].document, 9, 0), onScreenBefore),
          "stale: and the state on screen is bit-identical afterwards -- the refusal really "
          "moved nothing, checked over raw half words rather than over the cursor alone");
    check(contains(refused.refusal, "refused rather than redirected") &&
              contains(refused.refusal, "6 states have been dropped") &&
              contains(refused.refusal, "0.50 MiB byte budget") &&
              contains(refused.refusal, "5 states remain"),
          "stale: the refusal names the numbers -- how many states went, the budget they went "
          "for, how many are left -- in History::budgetPressure()'s own tone");

    // And a surviving row still works, so the refusal is about that state and
    // not about the panel having given up.
    const uint64_t liveSerial = historySerialForRow(h, 1);
    const HistoryPanelClick good = historyPanelClick(h, liveSerial);
    check(good.ok && good.document != nullptr && good.cursorMoves == 1 && h.cursor() == 1 &&
              good.refusal.empty(),
          "stale: a row that DOES still exist clicks through in the same history -- the "
          "refusal is about the missing state, not about the panel refusing to work");
  }

  // --- Part D: the redo tail is visibly distinct --------------------------
  {
    History h;
    Document live = Document::createBlank(512, 512, WorkingSpace{});
    h.begin("opened", live);
    for (int i = 0; i < 5; ++i) {
      paintTile(live, i % 4, i / 4, 0.1f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    check(historyRedoTailNote(h).empty(),
          "tail: with the cursor at the newest state there is no redo tail and no note about "
          "one -- the common case says nothing");

    h.jumpTo(3);
    const std::vector<HistoryPanelRow> rows = historyPanelRows(h);
    bool states = rows.size() == 6;
    for (size_t i = 0; i < rows.size() && states; ++i) {
      const HistoryRowState want = i < 3   ? HistoryRowState::Past
                                   : i == 3 ? HistoryRowState::Current
                                            : HistoryRowState::Redoable;
      if (rows[i].state != want) states = false;
    }
    check(states,
          "tail: three rows before the cursor are PAST, the cursor's row is CURRENT, and the "
          "two after it are REDOABLE -- three states, not two");
    check(contains(historyRowText(rows[2]), "PAST") &&
              contains(historyRowText(rows[3]), "CURRENT") &&
              contains(historyRowText(rows[4]), "REDOABLE"),
          "tail: and the state is in the row TEXT, so what the next edit would destroy is "
          "legible to --selftest and not only to a screenshot's colour");

    const std::string note = historyRedoTailNote(h);
    check(contains(note, "2 states after this one can be redone") &&
              contains(note, "next edit discards them"),
          "tail: the note names how many states are ahead and says the next edit discards "
          "them -- the panel is not allowed to draw a branch without saying it is one");

    // Now make that edit, and watch the tail go.
    const uint64_t doomed = rows[5].serial;
    live = *h.jumpTo(3);
    paintTile(live, 7, 7, 0.9f);
    h.record("something else", live);
    const std::vector<HistoryPanelRow> after = historyPanelRows(h);
    check(after.size() == 5 && after.back().state == HistoryRowState::Current &&
              historyRedoTailNote(h).empty(),
          "tail: recording at a non-end cursor truncated exactly those two rows, and the note "
          "it warned with is gone because the branch is");
    const HistoryPanelClick gone = historyPanelClick(h, doomed);
    check(!gone.ok && gone.cursorMoves == 0 &&
              contains(gone.refusal, "2 states were truncated"),
          "tail: **and a click still holding one of those rows is refused with the count** -- "
          "the panel says the branch was abandoned rather than landing somewhere near it");
  }

  // --- Part E: PRD O3 -- ONE cursor move at any distance ------------------
  //
  // "Jumping back N entries costs one replay from the nearest keyframe, not N
  // replays." Two independent instruments, because a counter the click reports
  // about itself is not on its own a proof: the panel's own click path is
  // counted AND timed against the per-step walk an implementer would otherwise
  // have written, on the same 41-state history.
  {
    Document live = Document::createBlank(1024, 1024, WorkingSpace{});
    History h;  // the real default budget: 40 single-tile edits is 5 MiB, no eviction
    h.begin("baseline", live);
    for (int i = 0; i < 40; ++i) {
      paintTile(live, i % 8, i / 8, 0.02f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    check(h.entries().size() == 41 && h.cursor() == 40 && h.droppedEntryCount() == 0,
          "o3: a 41-state history with the cursor at the newest state and nothing evicted");

    const uint64_t sNewest = historySerialForRow(h, 40);
    const uint64_t sOneBack = historySerialForRow(h, 39);
    const uint64_t sFortyBack = historySerialForRow(h, 0);

    // The per-step walk, written out here the way Part E of the `history`
    // section writes out the naive eviction policy: this is the panel an
    // implementer gets by calling undo() until the cursor arrives.
    auto perStepWalk = [&](size_t targetRow) {
      size_t calls = 0;
      while (h.cursor() > targetRow) {
        h.undo();
        ++calls;
      }
      while (h.cursor() < targetRow) {
        h.redo();
        ++calls;
      }
      return calls;
    };

    const HistoryPanelClick one = historyPanelClick(h, sOneBack);
    check(one.ok && one.cursorMoves == 1 && h.cursor() == 39,
          "o3: a click one state back costs exactly one cursor move");
    h.jumpTo(40);
    const HistoryPanelClick forty = historyPanelClick(h, sFortyBack);
    check(forty.ok && forty.cursorMoves == 1 && h.cursor() == 0,
          "o3: **a click FORTY states back costs exactly one cursor move too** -- the same "
          "one, because every entry is a keyframe and the replay range is empty");

    // Same destination, same bytes, and the walk's call count for contrast.
    const std::vector<uint16_t> viaClick = rawTile(*h.jumpTo(0), 0, 0);
    h.jumpTo(40);
    const size_t walkCalls = perStepWalk(0);
    const std::vector<uint16_t> viaWalk = rawTile(h.entries()[h.cursor()].document, 0, 0);
    check(walkCalls == 40 && sameRaw(viaClick, viaWalk),
          "o3: the per-step walk reaches the SAME bytes in forty calls where the panel's "
          "click takes one -- so the saving is real work skipped, not a different answer");
    h.jumpTo(40);
    check(perStepWalk(39) == 1,
          "o3: and at distance one the walk costs one too -- the two paths are only allowed "
          "to differ in how the cost GROWS with N, which is the whole of PRD O3");

    // --- The timing -------------------------------------------------------
    //
    // PRD O3 is a claim about how cost GROWS with N, so what is measured is a
    // *ratio between distance 1 and distance 40* for each path, not two
    // absolute numbers that would only say which is faster on this machine.
    //
    //  * **The panel click's derived ratio is exactly 1.00.** A click is a
    //    `std::lower_bound` over the entry list plus one `History::jumpTo()`,
    //    and neither reads the distance travelled. The assertion lands at 1.4x
    //    that derived bound, per this project's rule, with the measurement's
    //    own noise floor measured beside it -- a second, independent best-of-5
    //    of the SAME distance-1 pair -- so a reader can see where the residual
    //    sits rather than take 1.4 on faith.
    //  * **The per-step walk's derived ratio is exactly 40** (forty `undo()`
    //    calls against one). The *observed* one lands on either side of 40,
    //    and legitimately: the distance-1 walk is a single ~1 ns call sitting
    //    at the edge of what a loop around `steady_clock` can resolve, so its
    //    denominator is the noisy term. Only the growth is being claimed, so
    //    the assertion is a lower bound at 10 -- four times *below* the derived
    //    value, which is the safe direction and survives either sign of that
    //    error.
    //
    // The residual above 1.00 in the click ratio is attributed rather than
    // waved at: the serial LOOKUP is not identical work at the two ends of the
    // list (a `HistoryEntry` is a large object, so a `lower_bound` landing at
    // position 0 touches different cache lines from one landing at position
    // 39), while the cursor move is one pointer write either way. The
    // lookup-only pair is therefore timed with no cursor move at all, and if
    // the gap lives there it shows up in those two numbers too.
    //
    // Each variant is timed over *pairs* -- (40 -> 39 -> 40) against
    // (40 -> 0 -> 40) -- so both numbers are two traversals and compare like
    // for like, and one click is far below steady_clock's resolution.
    constexpr int kClickReps = 100000;
    double clickNear = 1e9, clickFar = 1e9, clickControl = 1e9;
    double walkNear = 1e9, walkFar = 1e9;
    double lookupNear = 1e9, lookupFar = 1e9;
    size_t sink = 0, walkSink = 0, lookupSink = 0;
    for (int rep = 0; rep < 5; ++rep) {
      h.jumpTo(40);
      const auto t0 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        sink += historyPanelClick(h, sOneBack).cursorMoves;
        sink += historyPanelClick(h, sNewest).cursorMoves;
      }
      const auto t1 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        sink += historyPanelClick(h, sFortyBack).cursorMoves;
        sink += historyPanelClick(h, sNewest).cursorMoves;
      }
      const auto t2 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        sink += historyPanelClick(h, sOneBack).cursorMoves;
        sink += historyPanelClick(h, sNewest).cursorMoves;
      }
      const auto t3 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        walkSink += perStepWalk(39);
        walkSink += perStepWalk(40);
      }
      const auto t4 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        walkSink += perStepWalk(0);
        walkSink += perStepWalk(40);
      }
      const auto t5 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        lookupSink += historyRowForSerial(h, sOneBack);
        lookupSink += historyRowForSerial(h, sNewest);
      }
      const auto t6 = Clock::now();
      for (int i = 0; i < kClickReps; ++i) {
        lookupSink += historyRowForSerial(h, sFortyBack);
        lookupSink += historyRowForSerial(h, sNewest);
      }
      const auto t7 = Clock::now();
      clickNear = std::min(clickNear, seconds(t0, t1) / kClickReps);
      clickFar = std::min(clickFar, seconds(t1, t2) / kClickReps);
      clickControl = std::min(clickControl, seconds(t2, t3) / kClickReps);
      walkNear = std::min(walkNear, seconds(t3, t4) / kClickReps);
      walkFar = std::min(walkFar, seconds(t4, t5) / kClickReps);
      lookupNear = std::min(lookupNear, seconds(t5, t6) / kClickReps);
      lookupFar = std::min(lookupFar, seconds(t6, t7) / kClickReps);
    }
    check(sink == static_cast<size_t>(6) * kClickReps * 5 &&
              walkSink == static_cast<size_t>(2 + 80) * kClickReps * 5 &&
              lookupSink == static_cast<size_t>(39 + 40 + 0 + 40) * kClickReps * 5,
          "o3: every timed click, walk and lookup really ran -- the counts are the ones only "
          "an executed loop can produce, so none of the three was optimised away");

    const double clickRatio = clickNear > 0.0 ? clickFar / clickNear : 0.0;
    const double walkRatio = walkNear > 0.0 ? walkFar / walkNear : 0.0;
    const double lookupRatio = lookupNear > 0.0 ? lookupFar / lookupNear : 0.0;
    const double noiseFloor =
        clickNear > 0.0 ? std::max(clickControl / clickNear, clickNear / clickControl) : 0.0;
    std::printf(
        "[selftest] history panel: PRD O3 -- [measured] panel click %.1f ns at N=1 and %.1f ns "
        "at N=40, **ratio %.2f** against a derived 1.00 (noise floor %.2f, and the serial "
        "lookup alone accounts for %.2f of it); the per-step walk %.1f ns and %.1f ns, ratio "
        "%.1f around a derived 40 whose one-call denominator is at the clock's resolution\n",
        clickNear * 1e9, clickFar * 1e9, clickRatio, noiseFloor, lookupRatio, walkNear * 1e9,
        walkFar * 1e9, walkRatio);
    check(clickRatio > 0.0 && clickRatio <= 1.4,
          "o3: **the click costs the same at forty states as at one** -- inside 1.4x the "
          "derived bound of 1.0, which is what a click that never reads the distance must "
          "measure");
    check(walkRatio >= 10.0,
          "o3: **while the per-step walk's cost grows with N**, by the factor its forty calls "
          "predict -- that growth is the cost PRD O3 exists to forbid, and it is the one the "
          "panel does not pay");
  }

  // --- Part F: eviction and snapshots, both legible -----------------------
  {
    Document live = deepCopy(buildRealisticDocument());
    History h(size_t{4} * 1024 * 1024 * 1024);
    h.begin("opened", live);
    for (int32_t i = 0; i < 10; ++i) {
      paintTile(live, i, 0, 0.05f * static_cast<float>(i + 1));
      h.record("edit " + std::to_string(i), live);
    }
    check(historyDroppedNote(h).empty() && historySnapshotRows(h).empty(),
          "legible: with nothing evicted and no snapshot taken, the panel shows neither note "
          "-- a line about zero discarded states would be noise");

    const Document* at3 = h.jumpTo(3);
    const size_t snapIndex = h.takeSnapshot("before the risky bit", *at3);
    const std::vector<uint16_t> snapFingerprint = rawTile(*at3, 7, 0);
    check(snapIndex == 0 && historySnapshotRows(h).size() == 1 &&
              historyPanelRows(h).size() == 11,
          "legible: a snapshot appears in its OWN row list and adds no row to the linear "
          "list -- core/History's second list, presented as a second group");
    const HistorySnapshotRow snapRow = historySnapshotRows(h)[0];
    check(historySnapshotRowText(snapRow) == "before the risky bit \xC2\xB7 SNAPSHOT",
          "legible: and its row says SNAPSHOT rather than one of PAST/CURRENT/REDOABLE, "
          "because it has no cursor position at all");

    h.jumpTo(10);
    h.setBudgetBytes(2 * sizeof(Tile));
    const std::string note = historyDroppedNote(h);
    check(h.droppedEntryCount() == 10 && contains(note, "10 earlier states have been discarded") &&
              contains(note, "0.25 MiB byte budget") && contains(note, "refused rather than "
                                                                     "redirected"),
          "legible: **a user whose undo has stopped going back is told so, with numbers** -- "
          "how many states went, and the budget they went for");
    check(historyPanelRows(h).size() == 1 && historySnapshotRows(h).size() == 1 &&
              sameRaw(rawTile(h.snapshots()[0].document, 7, 0), snapFingerprint),
          "legible: PRD O4 on screen -- the eviction took every evictable row and the "
          "snapshot group still holds its state bit-exactly");

    // The two clicks are different actions, and each refuses the other's row.
    const HistoryPanelClick wrongWay = historyPanelClick(h, snapRow.serial);
    check(!wrongWay.ok && wrongWay.cursorMoves == 0 &&
              contains(wrongWay.refusal, "snapshot 1 of 1") &&
              contains(wrongWay.refusal, "no cursor position to move to"),
          "legible: clicking a snapshot as if it were a history row is refused by name -- a "
          "snapshot is not on the chain and there is no cursor position for it");
    const uint64_t entrySerial = historySerialForRow(h, 0);
    const HistoryPanelClick otherWay = historyPanelRestoreSnapshot(h, entrySerial);
    check(!otherWay.ok && !otherWay.appendedEntry &&
              contains(otherWay.refusal, "not a snapshot") &&
              contains(otherWay.refusal, "records no edit"),
          "legible: and restoring a history row is refused for the mirror reason -- one is a "
          "cursor move, the other is an edit, and conflating them is what a single click "
          "handler would have done");

    const size_t before = historyPanelRows(h).size();
    const HistoryPanelClick restored = historyPanelRestoreSnapshot(h, snapRow.serial);
    const std::vector<HistoryPanelRow> rowsNow = historyPanelRows(h);
    check(restored.ok && restored.appendedEntry && restored.cursorMoves == 1 &&
              rowsNow.size() == before + 1 && rowsNow.back().state == HistoryRowState::Current &&
              contains(rowsNow.back().label, "before the risky bit"),
          "legible: restoring appends ONE row at the bottom and puts the cursor on it -- "
          "core/History records it as an ordinary edit, so it is itself undoable and the list "
          "stays linear");
    check(sameRaw(rawTile(*restored.document, 7, 0), snapFingerprint),
          "legible: and the document it installed is the snapshotted state bit-exactly");

    check(h.dismissSnapshot(0) && historySnapshotRows(h).empty(),
          "legible: dismissing removes the group");
    const HistoryPanelClick afterDismiss = historyPanelRestoreSnapshot(h, snapRow.serial);
    check(!afterDismiss.ok && contains(afterDismiss.refusal, "0 snapshots are held") &&
              contains(afterDismiss.refusal, "exempt until dismissed"),
          "legible: restoring a dismissed snapshot is refused with the count and the reason "
          "-- 'until dismissed' (PRD O4) is exactly that, and nothing brings one back");
  }

  // --- Part G: the empty history, and the wiring -------------------------
  {
    History empty;
    check(historyPanelRows(empty).empty() && historySnapshotRows(empty).empty() &&
              historyDroppedNote(empty).empty() && historyRedoTailNote(empty).empty(),
          "wiring: a History with nothing in it draws nothing at all -- no rows, no notes");
    const HistoryPanelClick nothing = historyPanelClick(empty, 1);
    check(!nothing.ok && nothing.cursorMoves == 0 &&
              contains(nothing.refusal, "holds 0 states"),
          "wiring: and a click into it is refused with that number rather than dereferencing "
          "an empty list");

    // Through the real funnel, with the real labels.
    OpenDocument od = makeBlankOpenDocument(512, 512, WorkingSpace{}, "panel");
    check(historyPanelRows(od.history).size() == 1 &&
              historyPanelRows(od.history)[0].label == "new document" &&
              historyPanelRows(od.history)[0].state == HistoryRowState::Current,
          "wiring: a blank document's panel is one row reading 'new document', and it is the "
          "current one");

    const size_t layersBefore = od.document.layers.size();
    recordLayerEdit(od, addLayer(od.document, 1, Layer{}));
    recordLayerEdit(od, setLayerOpacity(od.document, 0, 0.5f));
    const std::vector<HistoryPanelRow> rows = historyPanelRows(od.history);
    check(rows.size() == 3 && rows[1].label == od.unsavedEdits[0] &&
              rows[2].label == od.unsavedEdits[1],
          "wiring: **the rows are named by the op that produced them** (PRD O2) -- they are "
          "core/LayerOps' own editLabel strings, the same ones PRD I11's refusals name, so "
          "the panel cannot grow a second vocabulary");

    // A click, applied the way ui/MacPaintUI applies it.
    const HistoryPanelClick back = historyPanelClick(od.history, rows[0].serial);
    check(back.ok && back.cursorMoves == 1 && back.document != nullptr &&
              back.document->layers.size() == layersBefore,
          "wiring: clicking the baseline row installs the document from before both edits, in "
          "one cursor move");
    od.document = *back.document;
    check(historyPanelRows(od.history)[0].state == HistoryRowState::Current &&
              historyPanelRows(od.history)[2].state == HistoryRowState::Redoable &&
              contains(historyRedoTailNote(od.history), "2 states"),
          "wiring: and the panel immediately reads the new cursor -- two rows became a redo "
          "branch, with the note that the next edit discards them");

    // The revert decision core/History.hpp defers to this step, answered.
    od.history.begin("revert to saved", od.document);
    const std::vector<HistoryPanelRow> afterRevert = historyPanelRows(od.history);
    check(afterRevert.size() == 1 && afterRevert[0].label == "revert to saved" &&
              afterRevert[0].state == HistoryRowState::Current && !od.history.canUndo(),
          "wiring: **revert stays un-undoable, and the panel is where that promise becomes "
          "visible** -- one row reading 'revert to saved' with nothing above it, which is "
          "what app/DocumentLifecycle's refusal already tells the user in words");
  }

  std::printf("[selftest] history panel %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// ==========================================================================
// PLAN.md Phase 5 step 9 -- Clipping masks (PRD C9)
// ==========================================================================


}  // namespace np
