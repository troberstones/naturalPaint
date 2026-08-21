#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/AtelierTheme.hpp"

namespace np {

// PLAN.md Phase 5 step 14 -- the two-tab split (PRD **A5**, P1) and the
// residency rule that makes it legal (PRD **A6**, P0).
//
// The two are one section because they are one mechanism. ui/DocumentTexture
// held **one** texture keyed on `(id, revision, width, height)`, so two
// visible documents would have missed that key *alternately* and recomposited
// and re-uploaded the whole canvas twice a frame. §4 below runs that -- the
// rejected alternative, on the same two documents, in the same loop -- and
// prints both upload counts, so the cap is justified by a measurement rather
// than by a citation.
//
// Headless for §1 and §2 (pure geometry and pure session logic; no ImGui
// context is created anywhere here). §3 onwards need the GPU, because PRD A6
// is a claim about texture bytes and this section answers it in bytes.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// nothing here touches a file format, so `kOiioBuild` does not appear.
// Writes no files.
bool runDocumentResidencyTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // -----------------------------------------------------------------------
  // 1. The split, as arithmetic
  // -----------------------------------------------------------------------
  //
  // The property worth a test is the one the design diagram asserts only
  // implicitly, and it is ui/AtelierChrome's own layout test's property one
  // level down: **the panes and the rule between them tile the canvas
  // region** -- every pixel in exactly one of the three, no gap, no overlap.
  // A split can satisfy every named dimension and still leave a seam, and a
  // seam in the *canvas* shows the surround through the middle of a painting.
  std::printf("  -- 1. the split tiles the canvas region --\n");

  auto tiles = [](const AtelierPanes& p, const AtelierRect& canvas, bool columns) {
    if (p.count != 2) return false;
    if (columns) {
      const bool sameBand = p.pane[0].y == canvas.y && p.pane[1].y == canvas.y &&
                            p.pane[0].h == canvas.h && p.pane[1].h == canvas.h &&
                            p.divider.y == canvas.y && p.divider.h == canvas.h;
      const bool abut = p.pane[0].x == canvas.x && p.divider.x == p.pane[0].right() &&
                        p.pane[1].x == p.divider.right() &&
                        p.pane[1].right() == canvas.right();
      const bool exact = p.pane[0].w + p.divider.w + p.pane[1].w == canvas.w;
      return sameBand && abut && exact;
    }
    const bool sameColumn = p.pane[0].x == canvas.x && p.pane[1].x == canvas.x &&
                            p.pane[0].w == canvas.w && p.pane[1].w == canvas.w &&
                            p.divider.x == canvas.x && p.divider.w == canvas.w;
    const bool abut = p.pane[0].y == canvas.y && p.divider.y == p.pane[0].bottom() &&
                      p.pane[1].y == p.divider.bottom() &&
                      p.pane[1].bottom() == canvas.bottom();
    const bool exact = p.pane[0].h + p.divider.h + p.pane[1].h == canvas.h;
    return sameColumn && abut && exact;
  };

  {
    const AtelierBands bands = atelierLayout(0.0f, 0.0f, 1600.0f, 1000.0f, true);
    const AtelierRect canvas = bands.canvas;

    const AtelierPanes single = atelierSplitPanes(canvas, AtelierSplit::Single);
    check(single.count == 1 && single.pane[0].x == canvas.x && single.pane[0].w == canvas.w &&
              single.pane[0].h == canvas.h && single.divider.empty(),
          "Single: one pane, the whole canvas region, no divider");

    const AtelierPanes cols = atelierSplitPanes(canvas, AtelierSplit::Columns);
    check(tiles(cols, canvas, true), "Columns: two panes + a 2 px rule tile the canvas exactly");
    check(cols.divider.w == kRuleThickness && cols.divider.h == canvas.h,
          "Columns: the rule is the chrome's own 2 px, full height");
    check(cols.pane[0].x < cols.pane[1].x, "Columns: pane 0 is the left one");

    const AtelierPanes rows = atelierSplitPanes(canvas, AtelierSplit::Rows);
    check(tiles(rows, canvas, false), "Rows: two panes + a 2 px rule tile the canvas exactly");
    check(rows.pane[0].y < rows.pane[1].y, "Rows: pane 0 is the top one");

    // The halves cannot both be exactly half of an odd remainder, so the
    // question is only where the odd pixel goes -- and the answer has to be
    // "into a pane", never "into a seam".
    const AtelierRect odd{11.0f, 7.0f, 1001.0f, 707.0f};
    check(tiles(atelierSplitPanes(odd, AtelierSplit::Columns), odd, true),
          "Columns: an odd width still tiles exactly (the spare pixel is in a pane)");
    check(tiles(atelierSplitPanes(odd, AtelierSplit::Rows), odd, false),
          "Rows: an odd height still tiles exactly");

    // Below two usable panes the split collapses rather than producing
    // slivers. The threshold is the navigator's own box plus its inset --
    // the width at which the canvas region has already given up on showing
    // its overlay.
    check(kMinPaneW == kNavigatorMaxW + 2.0f * kNavigatorInset &&
              kMinPaneH == kNavigatorMaxH + 2.0f * kNavigatorInset,
          "the minimum pane is the navigator box plus its inset, not a guess");
    const AtelierRect narrow{0.0f, 0.0f, kMinPaneW * 2.0f, 900.0f};
    check(atelierSplitPanes(narrow, AtelierSplit::Columns).count == 1,
          "Columns: a canvas one rule short of two minimum panes stays single");
    const AtelierRect wide{0.0f, 0.0f, kMinPaneW * 2.0f + kRuleThickness, 900.0f};
    check(atelierSplitPanes(wide, AtelierSplit::Columns).count == 2,
          "Columns: exactly two minimum panes plus the rule does split");
    const AtelierRect shortRect{0.0f, 0.0f, 1400.0f, kMinPaneH * 2.0f};
    check(atelierSplitPanes(shortRect, AtelierSplit::Rows).count == 1,
          "Rows: a canvas too short for two minimum panes stays single");
    check(atelierSplitPanes(AtelierRect{}, AtelierSplit::Columns).count == 1,
          "an empty canvas region cannot be split");
  }

  // -----------------------------------------------------------------------
  // 2. Which document each pane shows
  // -----------------------------------------------------------------------
  //
  // The rule ui/AtelierChrome.hpp states: the focused pane always holds the
  // session's active document, so focusing the other pane *swaps* rather than
  // pointing the panels somewhere else. Everything here is that rule plus the
  // repairs a session can force which a click never can.
  std::printf("  -- 2. the focused pane is the session's active document --\n");

  {
    DocumentSession session;
    AtelierSplitState split;
    split.mode = AtelierSplit::Columns;

    AtelierPaneDocuments panes = atelierPaneDocuments(session, split);
    check(panes.count == 1 && panes.pane[0] == nullptr && split.companion == 0,
          "no document open: one pane, nothing in it, no companion");

    OpenDocument* a = session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "A"));
    panes = atelierPaneDocuments(session, split);
    check(panes.count == 1 && panes.pane[0] == a && split.companion == 0,
          "one document open: the split shows one pane, not an empty second");

    OpenDocument* b = session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "B"));
    // add() makes the new document active, so B is active and A is the
    // neighbour the companion rule picks.
    panes = atelierPaneDocuments(session, split);
    check(panes.count == 2, "two documents open: the split shows two panes");
    check(panes.pane[panes.focusedPane] == b && session.active() == b,
          "the focused pane holds the active document");
    check(panes.pane[1 - panes.focusedPane] == a && split.companion == a->id,
          "the companion is derived from tab order, not left empty");

    // Focus swaps the documents between the panes; the panes do not move.
    split.focusedPane = 1;
    panes = atelierPaneDocuments(session, split);
    check(panes.focusedPane == 1 && panes.pane[1] == b && panes.pane[0] == a,
          "focusing pane 1 puts the active document in pane 1, the companion in pane 0");

    OpenDocument* c = session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "C"));
    split.companion = a->id;
    split.focusedPane = 0;
    panes = atelierPaneDocuments(session, split);
    check(panes.pane[0] == c && panes.pane[1] == a,
          "a companion that is still open is kept, not re-derived every frame");

    // A closed companion is replaced rather than emptied -- a split that
    // silently became one pane on a close would read as a bug.
    std::string err;
    check(session.close(0, /*discardUnsavedChanges=*/false, &err),
          "closing the clean companion succeeds");
    panes = atelierPaneDocuments(session, split);
    check(panes.count == 2 && split.companion != 0 && split.companion != session.active()->id,
          "a closed companion is replaced from tab order, and the split survives");

    // A companion that has become the active document is the other repair: it
    // would otherwise show the same document twice.
    split.companion = session.active()->id;
    panes = atelierPaneDocuments(session, split);
    check(panes.pane[0] != panes.pane[1] && panes.count == 2,
          "a companion equal to the active document is repaired, never shown twice");

    // Down to one document the split shows one pane -- and the *mode* is kept,
    // so it comes back on its own when a second document does.
    check(session.close(0, false, &err) && session.count() == 1,
          "closing down to a single document succeeds");
    panes = atelierPaneDocuments(session, split);
    check(panes.count == 1 && split.mode == AtelierSplit::Columns,
          "one document left: one pane, but the arrangement is remembered");
    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "D"));
    check(atelierPaneDocuments(session, split).count == 2,
          "a second document returns and the remembered split reopens itself");
  }

  // -----------------------------------------------------------------------
  // 3. PRD A6, in bytes: at most two, and hidden documents hold none
  // -----------------------------------------------------------------------
  std::printf("  -- 3. residency: at most two visible documents hold textures --\n");

  // Small enough that twenty of them are cheap, large enough that the byte
  // arithmetic is not a rounding artefact: 256 x 256 x 8 bytes = 512 KiB each.
  constexpr int32_t kW = 256;
  constexpr int32_t kH = 256;
  constexpr size_t kBytesEach =
      static_cast<size_t>(kW) * static_cast<size_t>(kH) * DocumentTexture::kBytesPerTexel;
  constexpr size_t kTabs = 20;

  auto paint = [](OpenDocument& doc, float value, int32_t x, int32_t y) {
    const PixelCoord at{x, y};
    doc.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), std::array<float, 4>{value, value * 0.5f, 0.25f, 1.0f});
    doc.recordEdit("selftest paint", EditKind::Content);
  };

  DocumentSession session;
  for (size_t i = 0; i < kTabs; ++i) {
    OpenDocument* d = session.add(makeBlankOpenDocument(kW, kH, WorkingSpace{}, "tab"));
    // One texel each, so every document composites to something different and
    // a slot showing the wrong one would be caught by §5's memcmp rather than
    // passing on identical blanks.
    paint(*d, 0.25f + static_cast<float>(i) * 0.03125f, static_cast<int32_t>(i), 3);
  }

  DocumentTexturePool pool;
  check(kVisibleDocumentCap == 2, "PRD A6's cap is 2, in one named constant");
  check(pool.residentDocuments() == 0 && pool.gpuTextureBytes() == 0,
        "a pool nothing has been shown through holds zero bytes");

  OpenDocument* d0 = session.at(0);
  OpenDocument* d1 = session.at(1);
  OpenDocument* d2 = session.at(2);

  pool.viewFor(gpu, *d0);
  pool.viewFor(gpu, *d1);
  check(pool.residentDocuments() == 2, "two visible documents occupy two slots");
  check(pool.gpuTextureBytes() == 2 * kBytesEach,
        "two visible documents hold exactly two documents' worth of texture");
  check(pool.holds(d0->id) && pool.holds(d1->id), "both visible documents are resident");

  size_t hidden = 0;
  for (size_t i = 2; i < kTabs; ++i)
    if (!pool.holds(session.at(i)->id)) ++hidden;
  check(hidden == kTabs - 2, "every one of the other 18 open tabs holds no texture at all");

  // The third visible document is the whole point: it must re-point a slot,
  // not add one.
  const size_t bytesBefore = pool.gpuTextureBytes();
  pool.viewFor(gpu, *d2);
  check(pool.residentDocuments() == 2 && pool.gpuTextureBytes() == bytesBefore,
        "a third visible document costs no additional texture bytes");
  check(pool.evictions() == 1, "one slot was re-pointed, and it is counted");
  check(!pool.holds(d0->id) && pool.holds(d1->id) && pool.holds(d2->id),
        "the least recently visible document is the one that left");
  check(pool.retiredTextures() == 0,
        "re-pointing between documents of the same size retires no texture");

  std::printf("    [selftest] residency: %zu open tab(s), %zu resident, %.0f KiB of GPU "
              "texture (cap %zu x %.0f KiB)\n",
              kTabs, pool.residentDocuments(),
              static_cast<double>(pool.gpuTextureBytes()) / 1024.0, kVisibleDocumentCap,
              static_cast<double>(kBytesEach) / 1024.0);

  // -----------------------------------------------------------------------
  // 4. The rejected alternative, on the same two documents
  // -----------------------------------------------------------------------
  //
  // One `DocumentTexture` -- what this module held before this step -- driven
  // by two visible documents alternately. Every call is a key miss, because
  // the key carries the document id, so every call recomposites the whole
  // canvas and re-uploads it. The pool sees the same sequence.
  std::printf("  -- 4. one texture vs two slots, on the same alternating pair --\n");

  constexpr int kFrames = 8;  // 8 frames = 16 views of two documents
  DocumentTexture single;
  DocumentTexturePool paired;
  for (int f = 0; f < kFrames; ++f) {
    single.viewFor(gpu, *d0);
    single.viewFor(gpu, *d1);
    paired.viewFor(gpu, *d0);
    paired.viewFor(gpu, *d1);
  }

  const uint64_t singleUploads = single.uploads();
  const uint64_t pairedUploads = paired.uploads();
  check(singleUploads == 2 * kFrames,
        "one texture: every view of an alternating pair is a key miss");
  check(single.cacheHits() == 0, "one texture: the revision cache never answers");
  check(pairedUploads == 2, "two slots: one upload each, then the cache answers");
  check(paired.cacheHits() == 2 * kFrames - 2, "two slots: every later frame is free");
  check(single.totalUploadedTexels() ==
            static_cast<uint64_t>(kW) * static_cast<uint64_t>(kH) * 2u * kFrames,
        "one texture: the whole canvas is re-uploaded twice per frame");
  check(paired.totalUploadedTexels() ==
            static_cast<uint64_t>(kW) * static_cast<uint64_t>(kH) * 2u,
        "two slots: the canvas is uploaded once per document, ever");
  std::printf("    [selftest] alternating pair over %d frame(s): one texture %llu upload(s) / "
              "%llu texel(s); two slots %llu upload(s) / %llu texel(s) -- %llux fewer\n",
              kFrames, static_cast<unsigned long long>(singleUploads),
              static_cast<unsigned long long>(single.totalUploadedTexels()),
              static_cast<unsigned long long>(pairedUploads),
              static_cast<unsigned long long>(paired.totalUploadedTexels()),
              static_cast<unsigned long long>(singleUploads / pairedUploads));
  std::printf("    [selftest] alternating pair [measured] one texture %.2f ms total, "
              "two slots %.2f ms total\n",
              single.totalUploadMs(), paired.totalUploadMs());
  check(paired.totalUploadMs() <= single.totalUploadMs(),
        "the pool never costs more composite time than the single texture did");

  // -----------------------------------------------------------------------
  // 5. A re-pointed slot shows the right document, bit for bit
  // -----------------------------------------------------------------------
  //
  // The cap is only worth having if eviction is *correct*: a slot that has
  // held three documents must hold exactly the third one's pixels, with none
  // of the previous two left in the CPU mirror the incremental path writes
  // through.
  std::printf("  -- 5. eviction is correct, not merely bounded --\n");

  auto slotMatches = [&](const DocumentTexturePool& p, const OpenDocument& doc) {
    for (size_t i = 0; i < kVisibleDocumentCap; ++i) {
      if (p.slotDocument(i) != doc.id) continue;
      const std::vector<uint16_t> expected = compositeDocumentStraightHalf(doc.document);
      const std::vector<uint16_t>& got = p.slot(i).uploadedHalves();
      return got.size() == expected.size() &&
             std::memcmp(got.data(), expected.data(), expected.size() * sizeof(uint16_t)) == 0;
    }
    return false;
  };

  check(slotMatches(paired, *d0) && slotMatches(paired, *d1),
        "each slot holds its own document's composite, bit for bit");

  // d0 out, d2 in, d0 back: the slot has now carried three documents.
  paired.viewFor(gpu, *d2);
  check(!paired.holds(d0->id) && slotMatches(paired, *d2),
        "the re-pointed slot holds the incoming document's composite");
  paired.viewFor(gpu, *d0);
  check(slotMatches(paired, *d0),
        "a document that comes back is recomposited whole, not resumed from a stale mirror");
  check(paired.retiredTextures() == 0 && paired.retiredTextureBytes() == 0,
        "three documents through two slots retire nothing while the sizes match");

  // The one cost the cap does *not* remove, measured rather than argued: a
  // different-sized document through the same slot creates a texture, and the
  // old one is parked for ImGui's bind-group cache (ui/DocumentTexture.hpp,
  // decision 5).
  OpenDocument bigger = makeBlankOpenDocument(kW * 2, kH, WorkingSpace{}, "wide");
  paint(bigger, 0.5f, 5, 5);
  const size_t retiredBefore = paired.retiredTextures();
  paired.viewFor(gpu, bigger);
  check(paired.retiredTextures() == retiredBefore + 1,
        "a size change through a slot parks exactly one texture");
  check(paired.retiredTextureBytes() == kBytesEach,
        "the parked texture's bytes are reported, not silently held");
  check(paired.gpuTextureBytes() == kBytesEach + kBytesEach * 2,
        "the live figure follows the new size and still counts only two slots");

  // -----------------------------------------------------------------------
  // 6. PRD A8, and what this build can actually say about it
  // -----------------------------------------------------------------------
  //
  // ADR-0001's amendment: "A visible-but-unfocused document keeps stepping its
  // solver." **That is not reachable in this build, and saying so is the
  // honest answer rather than asserting a proxy for it.** `sim::PaintSim` is
  // one process-wide canvas constructed on the first stroke, with no
  // `DocumentId` on it and no per-document instance to step or freeze; a
  // stroke writes its dense texture and reaches no `Layer::rgbTiles` at all
  // (the stroke bridge is a later step). There is therefore no per-document
  // solver whose stepping could be observed, and no state in which one is
  // frozen.
  //
  // What *is* reachable is the compositing half of the same promise, and it is
  // the half the split can get wrong: an unfocused pane must keep showing a
  // document that changes, rather than freezing on the composite it had when
  // it lost focus. So that is what is checked.
  std::printf("  -- 6. an unfocused document keeps up to date --\n");

  DocumentTexturePool live;
  live.viewFor(gpu, *d0);  // focused
  live.viewFor(gpu, *d1);  // unfocused companion
  const uint64_t before = live.uploads();
  live.viewFor(gpu, *d0);
  live.viewFor(gpu, *d1);
  check(live.uploads() == before, "an unchanged pair costs no upload at all");
  paint(*d1, 0.75f, 40, 40);
  live.viewFor(gpu, *d0);
  live.viewFor(gpu, *d1);
  check(live.uploads() == before + 1,
        "an edit to the unfocused document recomposites it, and only it");
  check(slotMatches(live, *d1), "the unfocused pane shows the edit, not a frozen composite");

  // -----------------------------------------------------------------------
  // 7. PLAN.md Phase 5's Verify sentence, in measured bytes
  // -----------------------------------------------------------------------
  //
  // "Twenty open tabs cost kilobytes each. Two visible documents hold GPU
  // textures; hidden ones hold none."
  //
  // The first half is measured two ways, because RSS alone would not say
  // *why*: the structural size of the record, which is deterministic, and the
  // real resident-set delta of opening twenty of them, which is not.
  std::printf("  -- 7. PLAN.md Phase 5's Verify sentence, in bytes --\n");

  {
    size_t occupied = 0;
    for (size_t i = 0; i < kTabs; ++i)
      occupied += session.at(i)->document.layers[0].rgbTiles->occupiedTileCount();
    check(occupied == kTabs,
          "each tab holds exactly the one tile this section painted into it");

    const size_t recordBytes = sizeof(OpenDocument);
    check(recordBytes < 4096, "an OpenDocument record is under 4 KiB before any content");
    constexpr size_t kTileBytes = static_cast<size_t>(kTileSize) *
                                  static_cast<size_t>(kTileSize) *
                                  DocumentTexture::kBytesPerTexel;
    std::printf("    [selftest] tab cost: record %zu B, plus %zu KiB per tile that has "
                "content; a blank tab is the record alone\n",
                recordBytes, kTileBytes / 1024);

    // **Measured at 2000 tabs, not at 20**, and the reason is resolution
    // rather than ambition: `currentResidentBytes()` reports the task's RSS,
    // which moves a page at a time, and twenty records at a few hundred bytes
    // each do not fill one 16 KiB page -- the honest reading of twenty tabs is
    // "below the measurement's own granularity", which is true but says
    // nothing. Two thousand of them cost megabytes, the delta resolves, and
    // the Verify sentence's twenty is then arithmetic on a measured rate.
    constexpr size_t kManyTabs = 2000;
    DocumentSession blanks;
    const size_t rssBefore = currentResidentBytes();
    for (size_t i = 0; i < kManyTabs; ++i)
      blanks.add(makeBlankOpenDocument(2048, 2048, WorkingSpace{}, "blank"));
    const size_t rssAfter = currentResidentBytes();
    const double perTabBytes =
        rssAfter > rssBefore
            ? static_cast<double>(rssAfter - rssBefore) / static_cast<double>(kManyTabs)
            : 0.0;
    size_t blankTiles = 0;
    for (size_t i = 0; i < kManyTabs; ++i)
      blankTiles += blanks.at(i)->document.layers[0].rgbTiles->occupiedTileCount();
    check(blankTiles == 0,
          "two thousand blank 2048x2048 tabs allocate no tiles at all (PRD C2)");
    std::printf("    [selftest] blank 2048x2048 tabs [measured] %.0f B resident each over %zu "
                "tab(s), so twenty cost %.1f KiB in total\n",
                perTabBytes, kManyTabs, perTabBytes * static_cast<double>(kTabs) / 1024.0);
    // The bound is loose on purpose: this is the real RSS of the process and
    // the allocator is entitled to round. What it rules out is the failure
    // that matters -- a tab that costs megabytes because something sized
    // itself from the canvas rather than from the content. At 2048x2048 a tab
    // that did would be 32 MiB, so the bound has four orders of magnitude of
    // room and is still a real test.
    //
    // **Only the upper bound is asserted, and that is a fix rather than a
    // weakening.** This check used to also require `perTabBytes > 0.0`, which
    // made it fail whenever RSS did not grow across the loop -- the allocator
    // returning pages, or reclaim from an earlier section, both of which the
    // ternary above folds to 0.0 to avoid an unsigned underflow. That is a
    // process using *less* memory, which is the best possible outcome for the
    // property under test, being reported as a failure of it. It fired rarely
    // enough to look like noise and produced a real FAIL when it did.
    //
    // Growing by less than the measurement can resolve is not a result this
    // section is entitled to demand, so it is printed rather than asserted --
    // and the assertion that remains is the one with a failure behind it.
    if (perTabBytes == 0.0)
      std::printf("    [selftest] blank tabs: RSS did not grow measurably across %zu tabs, so "
                  "the per-tab cost is below this measurement's own resolution -- which is "
                  "the property passing, not the measurement failing\n",
                  kManyTabs);
    check(perTabBytes < 64.0 * 1024.0,
          "each blank tab costs kilobytes, not megabytes (bound: 64 KiB per tab)");

    // And the GPU half of the same sentence, on the same twenty tabs.
    check(pool.residentDocuments() == kVisibleDocumentCap &&
              pool.gpuTextureBytes() == kVisibleDocumentCap * kBytesEach,
          "twenty tabs, two visible: the GPU holds exactly two documents");
    // "hidden ones hold none", counted rather than asserted: how many of the
    // twenty tabs are named by a slot. Two, and the same two both times.
    size_t named = 0;
    for (size_t i = 0; i < kTabs; ++i)
      for (size_t sl = 0; sl < kVisibleDocumentCap; ++sl)
        if (pool.slotDocument(sl) == session.at(i)->id) ++named;
    check(named == kVisibleDocumentCap,
          "exactly two of the twenty tabs are named by a slot; the other 18 hold none");
  }

  std::printf("[selftest] document residency %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
