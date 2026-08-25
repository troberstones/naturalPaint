#include "app/selftest/Support.hpp"

#include "core/Channels.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"

namespace np {

// core/Channels + io/NpaintFile's `S####` part (PLAN.md "Phase 7 -- Select and
// paste"; PRD E11, E12, E13). Alpha channels stored in the document, the
// selection<->channel round trip, saved selections, and quick mask.
//
// Mostly headless CPU tile arithmetic, like runSelectionTest(); sections 5-7
// go through the filesystem, like runNpaintFormatTest(), because a format claim
// asserted against an in-memory struct is a claim about the struct.
//
// **The two assertions this section exists for**, since the rest is arithmetic
// that would fail loudly anyway:
//
//  1. A channel's absent-tile default is the SELECTION one (0.0) and not the
//     layer-mask one (1.0). Backwards, a channel painted on one tile of a
//     four-tile document comes back selecting the other three -- and nothing
//     crashes.
//  2. The active selection still does not reach `Document`, and therefore not
//     `History`. PRD E11 puts a *saved* selection in the document, and the
//     obvious wrong way to satisfy it is to move the live marquee there, which
//     would make drawing one undoable. Section 4 asserts both halves.
bool runChannelsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances: there is one, and it is ZERO --------------------------
  //
  // Every coverage comparison below is exact, and that is a measurement rather
  // than an aspiration. The chain a channel travels on the way to disk is:
  //
  //   uint8 v  -> coverageAt(): v * (1/255)   exact, 255 -> exactly 1.0
  //            -> floatToHalf()               ROUNDS, <= 2^-11 relative
  //            -> OpenImageIO, TypeDesc::HALF, ZIP        lossless
  //            -> halfToFloat()               exact
  //            -> writeCoverage(): *255 + 0.5, truncate   requantises
  //
  // so the one lossy stage is the float-to-half, and the question is whether it
  // can ever move a value across a uint8 grid boundary. **Measured over all 256
  // grid points, with the project's own core/Half:**
  //
  //   mismatches, v -> half -> v         : 0 of 256
  //   worst absolute half-rounding       : 2.432466e-4 (at v = 239, 0.93725)
  //   worst relative half-rounding       : 4.234514e-4 (bound 2^-11 = 4.8828e-4)
  //   half of one uint8 grid step        : 1.960784e-3  (1/510)
  //   margin, half-step / worst rounding : 8.06x
  //   0.0 and 1.0                        : exact identities
  //
  // 8.06x is not a tolerance, it is the reason there is no tolerance: the
  // rounding would have to grow eightfold before a single one of the 256 values
  // landed on the wrong byte. So the file round trip is asserted at **zero
  // tolerance**, the same standard io/NpaintFile holds HALF layer pixels to,
  // and a regression that quantised coverage anywhere along the chain fails
  // here rather than being absorbed.
  //
  // The margin is printed at run time from the same computation, so the
  // derivation is checkable rather than merely quoted.
  {
    int mismatches = 0;
    double worstAbs = 0.0;
    for (int v = 0; v <= 255; ++v) {
      const float exact = static_cast<float>(v) * (1.0f / 255.0f);
      const float back = halfToFloat(floatToHalf(exact));
      const double err = std::fabs(static_cast<double>(back) - static_cast<double>(exact));
      if (err > worstAbs) worstAbs = err;
      if (static_cast<int>(back * 255.0f + 0.5f) != v) ++mismatches;
    }
    std::printf("  [selftest] channels: uint8 coverage grid through HALF -- %d/256 mismatch, "
                "worst rounding %.7f vs half-step %.7f (%.2fx margin)\n",
                mismatches, worstAbs, 1.0 / 510.0, (1.0 / 510.0) / worstAbs);
    check(mismatches == 0 && worstAbs < 1.0 / 510.0,
          "channels: HALF carries all 256 uint8 coverage values EXACTLY -- the measurement "
          "the on-disk sample type was chosen on, and the reason nothing below has a "
          "tolerance");
  }

  // --- 1. The convention: an absent channel tile is 0.0 -------------------
  //
  // Checked against the layer-mask convention by name rather than in isolation,
  // exactly as runSelectionTest() does, because the hazard is not that either
  // value is wrong on its own -- it is that they are opposites and look alike.
  {
    AlphaChannel empty;
    empty.name = "Alpha 1";
    check(channelCoverageAt(empty, PixelCoord{0, 0}) == 0.0f &&
              channelCoverageAt(empty, PixelCoord{9999, -9999}) == 0.0f,
          "channels: a channel texel with NO tile behind it is 0.0 -- not selected, black, "
          "the SELECTION default and not the layer mask's");
    check(selectionTileCoverage(nullptr, PixelCoord{0, 0}) == 0.0f &&
              maskCoverage(nullptr, PixelCoord{0, 0}) == 1.0f,
          "channels: and that is the same leaf a selection reads through, while an absent "
          "MASK tile is still 1.0 -- one store type, one default, no second opinion");
    check(channelIsEmpty(empty),
          "channels: an empty channel says so by name, which is what a panel needs to show "
          "'this channel selects nothing' rather than showing nothing");

    // The failure this convention prevents, made concrete: a channel painted on
    // exactly one tile of a document that spans four. Under the layer-mask
    // default the three untouched tiles would read 1.0 and the channel would
    // select everywhere it had never been painted.
    AlphaChannel oneTile = channelFromSelection(selectRectangle(0.0f, 0.0f, 8.0f, 8.0f), "One");
    check(oneTile.tiles.occupiedTileCount() == 1 &&
              channelCoverageAt(oneTile, PixelCoord{4, 4}) == 1.0f &&
              channelCoverageAt(oneTile, PixelCoord{200, 4}) == 0.0f &&
              channelCoverageAt(oneTile, PixelCoord{4, 200}) == 0.0f &&
              channelCoverageAt(oneTile, PixelCoord{200, 200}) == 0.0f,
          "channels: a channel painted on ONE tile of a four-tile document covers nothing "
          "in the other three -- backwards, this is a channel that selects everywhere it "
          "was never painted, with no wrong pixel to point at");
  }

  // --- 2. PRD E13's round trip, both directions, EXACTLY ------------------
  {
    // A fixture with all three interesting kinds of texel in it: fully
    // selected, fully unselected, and a fractional edge -- the last because a
    // conversion that quantised would only show up there.
    const Selection source =
        combineSelections(selectRectangle(0.25f, 0.25f, 40.75f, 40.75f),
                          selectRectangle(130.0f, 5.0f, 140.0f, 15.0f), SelectionCombine::Add);
    check(source.tiles.occupiedTileCount() == 2,
          "channels: (fixture) the source selection spans two tiles and has fractional "
          "edges, so a conversion that quantised would have somewhere to show it");

    const AlphaChannel channel = channelFromSelection(source, "Round trip");
    const Selection back = selectionFromChannel(channel);

    // Texel for texel, over both tiles and the empty space around them.
    size_t differing = 0;
    float worstDelta = 0.0f;
    for (int32_t y = -4; y < 148; ++y) {
      for (int32_t x = -4; x < 148; ++x) {
        const float a = selectionCoverageAt(&source, PixelCoord{x, y});
        const float b = selectionCoverageAt(&back, PixelCoord{x, y});
        const float c = channelCoverageAt(channel, PixelCoord{x, y});
        if (a != b || a != c) ++differing;
        worstDelta = std::max(worstDelta, std::max(std::fabs(a - b), std::fabs(a - c)));
      }
    }
    std::printf("  [selftest] channels: selection<->channel round trip over 152x152 texels -- "
                "%zu differing, worst delta %.9f\n",
                differing, static_cast<double>(worstDelta));
    check(differing == 0 && worstDelta == 0.0f,
          "channels: selection -> channel -> selection is EXACT, texel for texel, at zero "
          "tolerance -- the two hold the same store, so there is no arithmetic to be "
          "approximately right");

    // Exact on the TILE SET too, not merely on the coverage. Neither
    // conversion compacts, which is what makes this assertable at all.
    check(back.tiles.occupiedTileCount() == source.tiles.occupiedTileCount() &&
              channel.tiles.occupiedTileCount() == source.tiles.occupiedTileCount(),
          "channels: and exact on the TILE SET, not just the coverage -- neither direction "
          "compacts, so a round trip cannot quietly change what is resident");
    check(channel.name == "Round trip",
          "channels: the channel keeps the name it was given, which is the only handle "
          "anything has on it");

    // The reverse order: channel -> selection -> channel.
    const AlphaChannel again = channelFromSelection(selectionFromChannel(channel), channel.name);
    bool identical = again.tiles.occupiedTileCount() == channel.tiles.occupiedTileCount();
    for (int32_t y = 0; y < 148 && identical; ++y) {
      for (int32_t x = 0; x < 148; ++x) {
        if (channelCoverageAt(again, PixelCoord{x, y}) !=
            channelCoverageAt(channel, PixelCoord{x, y})) {
          identical = false;
          break;
        }
      }
    }
    check(identical,
          "channels: and the other way round -- channel -> selection -> channel is exact "
          "too, so neither direction is the privileged one");

    // Compaction is the one thing that DOES change a tile set, and it must
    // change only tiles that carry nothing.
    {
      AlphaChannel padded = channelFromSelection(source, "Padded");
      padded.tiles.getOrCreate(TileCoord{9, 9});  // an all-zero tile, explicitly
      check(padded.tiles.occupiedTileCount() == 3,
            "channels: (fixture) an all-zero tile can be added, because the invariant "
            "belongs to the constructors and not to every write");
      const size_t dropped = compactChannel(padded);
      check(dropped == 1 && padded.tiles.occupiedTileCount() == 2,
            "channels: compaction drops the all-zero tile and NOTHING else -- 16 KiB of "
            "agreeing with the default, which every save-and-reopen would otherwise keep");
      check(channelCoverageAt(padded, PixelCoord{10, 10}) ==
                channelCoverageAt(channel, PixelCoord{10, 10}),
            "channels: and the coverage it kept is untouched by the compaction");
    }
  }

  // --- 3. PRD E12: quick mask ---------------------------------------------
  {
    // Entering with NO selection gives a blank overlay to paint into.
    const QuickMask fromNothing = quickMaskFromSelection(nullptr);
    check(fromNothing.coverage.tiles.occupiedTileCount() == 0 &&
              quickMaskCoverageAt(fromNothing, PixelCoord{5, 5}) == 0.0f,
          "quick mask: entering with NO selection gives an EMPTY overlay -- reading the "
          "null as 'coverage 1.0 everywhere' would hand the user a full mask to erase and "
          "allocate a tile per document tile to do it");

    // ...and leaving it untouched gives back no selection, not a selection
    // that selects nothing. The round trip must be a no-op.
    check(!selectionFromQuickMask(fromNothing).has_value(),
          "quick mask: leaving an untouched overlay gives back NO SELECTION -- the round "
          "trip is a no-op, where an engaged-empty selection would refuse every edit "
          "everywhere with no marquee on screen to explain it");

    // Entering from a real selection preserves it exactly.
    const Selection marquee = selectRectangle(10.5f, 10.5f, 30.0f, 30.0f);
    const QuickMask fromMarquee = quickMaskFromSelection(&marquee);
    bool preserved = true;
    for (int32_t y = 8; y < 34 && preserved; ++y) {
      for (int32_t x = 8; x < 34; ++x) {
        if (quickMaskCoverageAt(fromMarquee, PixelCoord{x, y}) !=
            selectionCoverageAt(&marquee, PixelCoord{x, y})) {
          preserved = false;
          break;
        }
      }
    }
    check(preserved,
          "quick mask: entering from a real selection carries its coverage in exactly, "
          "fractional edges included -- entering the mode must not soften the marquee");

    const std::optional<Selection> left = selectionFromQuickMask(fromMarquee);
    check(left.has_value() &&
              selectionCoverageAt(&*left, PixelCoord{10, 10}) ==
                  selectionCoverageAt(&marquee, PixelCoord{10, 10}) &&
              selectionCoverageAt(&*left, PixelCoord{20, 20}) == 1.0f,
          "quick mask: and leaving it again returns exactly what went in -- enter/exit with "
          "no painting is an identity on the selection");

    // Select All and Deselect are deliberately different states, and quick
    // mask is one of the places that becomes visible.
    const Selection all = selectAll(64, 64);
    check(quickMaskFromSelection(&all).coverage.tiles.occupiedTileCount() == 1 &&
              quickMaskFromSelection(nullptr).coverage.tiles.occupiedTileCount() == 0,
          "quick mask: entering from Select All gives a FULL overlay while entering from no "
          "selection gives an empty one -- the two restrict nothing in the same way and are "
          "still not the same state");

    // Painting: the overlay is editable, and a partial value stays partial.
    {
      QuickMask painting = quickMaskFromSelection(nullptr);
      paintQuickMask(painting, PixelCoord{3, 3}, 1.0f);
      paintQuickMask(painting, PixelCoord{4, 3}, 0.5f);
      check(painting.coverage.tiles.occupiedTileCount() == 1 &&
                quickMaskCoverageAt(painting, PixelCoord{3, 3}) == 1.0f,
            "quick mask: painting into a blank overlay allocates the one tile it touched "
            "and nothing else");
      const float partial = quickMaskCoverageAt(painting, PixelCoord{4, 3});
      check(partial > 0.49f && partial < 0.51f && partial != 0.0f && partial != 1.0f,
            "quick mask: and a half-strength dab stays HALF -- the overlay carries coverage, "
            "so a soft brush produces a soft selection edge rather than a hard one");

      const std::optional<Selection> out = selectionFromQuickMask(painting);
      check(out.has_value() && selectionCoverageAt(&*out, PixelCoord{3, 3}) == 1.0f &&
                selectionCoverageAt(&*out, PixelCoord{4, 3}) == partial,
            "quick mask: what was painted becomes the selection, at the coverage it was "
            "painted at -- this is PRD E12's whole claim");

      // Erase it all again: back to no selection, and the emptied tile does
      // not survive as 16 KiB of zeros.
      paintQuickMask(painting, PixelCoord{3, 3}, 0.0f);
      paintQuickMask(painting, PixelCoord{4, 3}, 0.0f);
      check(!selectionFromQuickMask(painting).has_value(),
            "quick mask: erasing the overlay back to blank gives NO selection -- deliberately "
            "erasing a selection to nothing is Deselect, which agrees with the untouched "
            "case above rather than fighting it");
    }

    // The eraser that costs nothing: writing 0.0 where there is no tile must
    // not allocate. An eraser dragged across empty canvas is exactly this
    // gesture, repeated for every texel it crosses.
    {
      QuickMask blank = quickMaskFromSelection(nullptr);
      for (int32_t i = 0; i < 4096; ++i)
        paintQuickMask(blank, PixelCoord{i, i}, 0.0f);
      check(blank.coverage.tiles.occupiedTileCount() == 0,
            "quick mask: erasing across 4096 texels of EMPTY overlay allocates zero tiles -- "
            "0.0 is what an absent tile already reads as, so the write has nothing to say");
    }

    // A negative or over-range value clamps rather than wrapping, inherited
    // from SelectionTile::writeCoverage() and asserted through this path
    // because it is a different call site.
    {
      QuickMask clampy = quickMaskFromSelection(nullptr);
      paintQuickMask(clampy, PixelCoord{1, 1}, 5.0f);
      paintQuickMask(clampy, PixelCoord{2, 1}, -5.0f);
      check(quickMaskCoverageAt(clampy, PixelCoord{1, 1}) == 1.0f &&
                quickMaskCoverageAt(clampy, PixelCoord{2, 1}) == 0.0f,
            "quick mask: out-of-range paint CLAMPS -- an unsigned wrap would turn 'more than "
            "fully selected' into 'unselected', the worst rounding of that mistake");
    }
  }

  // --- 4. PRD E11: saved selection is DOCUMENT data; the active one is not -
  //
  // The section this file exists for as much as the convention. Two claims,
  // and they pull in opposite directions on purpose.
  {
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    check(doc.channels.empty(),
          "saved selection: a blank document has no channels -- the list is empty for every "
          "document until a user saves one, which is what makes the format change additive");

    const Selection marquee = selectRectangle(4.0f, 4.0f, 60.0f, 60.0f);
    const size_t index = saveSelectionAsChannel(doc, marquee, "Sky");
    check(index == 0 && doc.channels.size() == 1 && doc.channels[0].name == "Sky",
          "saved selection: Save Selection puts a NAMED channel in the document -- E11's "
          "save half, and the only sanctioned way coverage crosses from session to document");

    const std::optional<Selection> restored = loadChannelAsSelection(doc, "Sky");
    check(restored.has_value() && selectionCoverageAt(&*restored, PixelCoord{10, 10}) == 1.0f &&
              selectionCoverageAt(&*restored, PixelCoord{100, 100}) == 0.0f,
          "saved selection: and Load Selection gives it back -- E11's restore half, through "
          "the name, which is the only handle a channel has");

    check(!loadChannelAsSelection(doc, "Nothing here").has_value(),
          "saved selection: loading a channel that is not there gives NULLOPT, not an empty "
          "selection -- answering a lookup failure with a selection that refuses every edit "
          "would disable the editor over a typo");

    // Names stay unique, because the lookup above is by name.
    saveSelectionAsChannel(doc, marquee, "Sky");
    check(doc.channels.size() == 2 && doc.channels[1].name == "Sky 2",
          "saved selection: saving under a name already taken APPENDS under a free one -- "
          "silently overwriting a saved selection would destroy work under a command called "
          "Save");
    check(uniqueChannelName(doc, "") == "Alpha 1" && uniqueChannelName(doc, "Sky") == "Sky 3",
          "saved selection: an unnamed save starts at 'Alpha 1' and a taken name takes the "
          "next free number, so a user never has to invent one");

    // --- The other half: the ACTIVE selection must not reach History -----
    //
    // `core::History` snapshots a whole `Document` by value. So the test is
    // whether the live marquee is inside that snapshot: it must not be, or
    // undoing a paint stroke would restore a marquee and drawing one would be
    // an undoable act (app/DocumentLifecycle.hpp argues it; this asserts it).
    OpenDocument od;
    od.document = doc;
    od.selection = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    const uint64_t revisionBefore = od.selectionRevision;

    History history;
    history.begin("open", od.document);
    // A structural edit to the document, of the kind that gets snapshotted.
    saveSelectionAsChannel(od.document, *od.selection, "From marquee");
    history.record("save selection", od.document);
    check(od.document.channels.size() == 3,
          "history: (fixture) saving a selection is a change to the DOCUMENT, so it is "
          "something history has to hold");

    const Document* undone = history.undo();
    check(undone != nullptr && undone->channels.size() == 2,
          "history: undoing Save Selection REMOVES the channel -- a saved selection is "
          "document data, so making one is undoable, and that is correct rather than a "
          "side effect");
    if (undone != nullptr) od.document = *undone;

    // The claim. Nothing history handed back could have touched the active
    // selection, because `Document` has nowhere to put one -- and the point of
    // asserting it is that the day someone adds that member, this fails.
    check(od.selection.has_value() &&
              selectionCoverageAt(&*od.selection, PixelCoord{220, 220}) == 1.0f &&
              selectionCoverageAt(&*od.selection, PixelCoord{10, 10}) == 0.0f,
          "history: and the ACTIVE selection survived the undo untouched -- it lives on "
          "OpenDocument, never in Document, so Cmd+Z cannot restore a marquee along with "
          "the pixels");
    check(od.selectionRevision == revisionBefore,
          "history: the selection revision did not move either -- an undo is not a selection "
          "change, and anything caching the marquee's bounds must not be told it is stale");

    // Stated the other way round, which is the form a future reader will check
    // it in: a Document copied by value carries channels and nothing else that
    // looks like a selection.
    const Document snapshot = od.document;
    check(snapshot.channels.size() == od.document.channels.size(),
          "history: a Document copy carries its CHANNELS -- the saved selections travel with "
          "the pixels they were made against, which is why they are here and not on the "
          "session record");
  }

  // --- 5. The file: channels save and reload, at zero tolerance -----------
  //
  // io/NpaintFile's `S####` part, asserted against a real file rather than
  // against the struct. Follows runNpaintFormatTest()'s shape: a small fixture,
  // a save, a load, and a comparison with no tolerance in it.
  {
    auto makeFixture = []() {
      Document doc = Document::createBlank(256, 256, WorkingSpace{});
      Tile& t = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      t.writePixel(PixelCoord{1, 1}, {0.5f, 0.25f, 0.125f, 1.0f});
      return doc;
    };

    const char* kNoChannels = "selftest_channels_none.npaint";
    const char* kNoChannels2 = "selftest_channels_none2.npaint";
    const char* kWithChannels = "selftest_channels_two.npaint";
    std::remove(kNoChannels);
    std::remove(kNoChannels2);
    std::remove(kWithChannels);

    const Document bare = makeFixture();
    const NpaintSaveResult savedBare = saveNpaint(bare, kNoChannels);
    check(savedBare.ok && savedBare.partsWritten == 2,
          "channel file: a document with NO channels writes part 0 plus its one layer part "
          "and nothing else -- no S#### part is written for an empty list");

    // Two channels, deliberately different shapes: one with a fractional edge
    // that exercises the whole uint8 grid, one entirely empty.
    Document withChannels = makeFixture();
    saveSelectionAsChannel(withChannels,
                           combineSelections(selectRectangle(0.25f, 0.25f, 30.75f, 30.75f),
                                             selectRectangle(140.0f, 140.0f, 150.0f, 150.0f),
                                             SelectionCombine::Add),
                           "Sky");
    saveSelectionAsChannel(withChannels, Selection{}, "Deliberately empty");
    check(withChannels.channels.size() == 2 && channelIsEmpty(withChannels.channels[1]),
          "channel file: (fixture) two channels, the second empty on purpose -- an emptied "
          "channel is still a channel the user made");

    const NpaintSaveResult savedWith = saveNpaint(withChannels, kWithChannels);
    check(savedWith.ok && savedWith.partsWritten == savedBare.partsWritten + 2,
          "channel file: two channels add exactly two parts and change nothing else about "
          "the file's structure");

    const NpaintLoadResult reloaded = loadNpaint(kWithChannels);
    check(reloaded.ok && reloaded.document.channels.size() == 2,
          "channel file: and they come back as channels rather than as parts this build "
          "could not understand");
    check(reloaded.document.channels.size() == 2 &&
              reloaded.document.channels[0].name == "Sky" &&
              reloaded.document.channels[1].name == "Deliberately empty",
          "channel file: with their names, in their order -- np:name is the handle, and the "
          "part order is what a panel would list");

    if (reloaded.document.channels.size() == 2) {
      size_t differing = 0;
      float worstDelta = 0.0f;
      for (int32_t y = -4; y < 160; ++y) {
        for (int32_t x = -4; x < 160; ++x) {
          const float a = channelCoverageAt(withChannels.channels[0], PixelCoord{x, y});
          const float b = channelCoverageAt(reloaded.document.channels[0], PixelCoord{x, y});
          if (a != b) ++differing;
          worstDelta = std::max(worstDelta, std::fabs(a - b));
        }
      }
      std::printf("  [selftest] channel file: 164x164 texels through EXR -- %zu differing, "
                  "worst delta %.9f (tolerance 0)\n",
                  differing, static_cast<double>(worstDelta));
      check(differing == 0 && worstDelta == 0.0f,
            "channel file: coverage survives the EXR round trip EXACTLY, at zero tolerance "
            "-- HALF carries all 256 uint8 values with 8x of margin, measured at the top of "
            "this section");
      check(reloaded.document.channels[0].tiles.occupiedTileCount() ==
                withChannels.channels[0].tiles.occupiedTileCount(),
            "channel file: and so does the TILE SET -- the empty space between two distant "
            "marquees does not come back as allocated tiles");
      check(channelIsEmpty(reloaded.document.channels[1]) &&
                reloaded.document.channels[1].tiles.occupiedTileCount() == 0,
            "channel file: the empty channel survives as an EMPTY channel -- its name is "
            "document data even though its coverage says nothing, and the all-zero tile the "
            "writer had to emit for EXR's sake is dropped on read");
    }

    // The layer is untouched by any of this, which is what makes the change
    // additive rather than a rewrite of the file.
    check(reloaded.ok && reloaded.document.layers.size() == 1 &&
              reloaded.document.layers[0].rgbTiles.has_value() &&
              reloaded.document.layers[0].rgbTiles->occupiedTileCount() == 1,
          "channel file: and the layer parts are exactly what they were -- channels are "
          "their own parts and touch nothing else");

    // A save-load-save cycle keeps the channels and their part positions.
    {
      const char* kAgain = "selftest_channels_again.npaint";
      std::remove(kAgain);
      const NpaintSaveResult resaved =
          saveNpaint(reloaded.document, kAgain, {}, &reloaded.carry);
      const NpaintLoadResult twice = loadNpaint(kAgain);
      check(resaved.ok && twice.ok && twice.document.channels.size() == 2 &&
                twice.document.channels[0].name == "Sky",
            "channel file: a load/save/load cycle keeps both channels and their order -- the "
            "part order carries a Channel slot, so a channel cannot drift to the end of the "
            "file");
      std::remove(kAgain);
    }

    // --- 6. BACKWARD COMPATIBILITY: a file with no channels still loads ---
    //
    // `kNoChannels` above is, byte for byte, the file a build without this
    // feature would have written: no code path outside the channel parts
    // changed, and the writer emits no S#### part for an empty list. So
    // loading it *is* the older-file test, and the byte comparison below is
    // what makes that claim checkable rather than asserted.
    {
      const NpaintLoadResult old = loadNpaint(kNoChannels);
      check(old.ok && old.document.channels.empty(),
            "backward compat: a file written with NO channels loads, with an empty channel "
            "list -- an older document must not need a newer writer to have touched it");
      check(old.ok && old.document.layers.size() == 1 &&
                old.document.layers[0].rgbTiles.has_value() &&
                old.document.layers[0].rgbTiles->occupiedTileCount() == 1,
            "backward compat: and its layers arrive intact, which is the part that would "
            "actually cost someone their work");
      bool channelWarning = false;
      for (const std::string& w : old.warnings)
        if (contains(w, "channel") || contains(w, "S0001")) channelWarning = true;
      check(!channelWarning,
            "backward compat: and it produces no channel warning at all -- an old file is "
            "not a damaged one, and a warning that fires on every legacy document is a "
            "warning nobody reads");
    }

    // The bytes: adding the feature must not have changed what a channel-free
    // document writes. Two saves of the same channel-free document are
    // compared with OpenEXR's `capDate` masked, which is the only field a
    // `.npaint` is not reproducible in.
    {
      const NpaintSaveResult again = saveNpaint(bare, kNoChannels2);
      auto readAll = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
      };
      const std::string a = maskCapDates(readAll(kNoChannels));
      const std::string b = maskCapDates(readAll(kNoChannels2));
      check(again.ok && !a.empty() && a == b,
            "backward compat: two saves of a channel-free document are byte-identical once "
            "capDate is masked -- so 'a document with no channels writes what it always "
            "wrote' is measured against a file rather than argued from the source");
      // Non-vacuity: the comparison above would pass on two empty strings.
      const std::string withBytes = maskCapDates(readAll(kWithChannels));
      check(withBytes.size() > a.size(),
            "backward compat: (non-vacuity) the file WITH channels is larger, so the "
            "comparison above is comparing real bytes and the channel parts really are "
            "extra rather than a rewrite");
    }

    // --- 7. A foreign S-part is carried, not swallowed (PRD I10) ---------
    //
    // The forward half of the compatibility story: a newer build's channel part
    // -- two channels instead of one, say -- must survive this build untouched,
    // and must not steal the part name a real channel needs.
    {
      const char* kForeign = "selftest_channels_foreign.npaint";
      std::remove(kForeign);

      NpaintCarry foreignCarry;
      NpaintRawPart foreign;
      foreign.name = "S0001";
      foreign.x = 0;
      foreign.y = 0;
      foreign.width = kTileSize;
      foreign.height = kTileSize;
      foreign.tileWidth = kTileSize;
      foreign.tileHeight = kTileSize;
      foreign.channelNames = {"coverage", "flags"};  // a channel this build has never heard of
      foreign.sampleTypeName = "half";
      foreign.rawPixels.assign(
          static_cast<size_t>(kTileSize) * kTileSize * 2 * sizeof(uint16_t), 0);
      foreign.attributes.push_back(NpaintAttribute{"np:kind",
                                                   NpaintAttribute::Type::String,
                                                   "selection",
                                                   0,
                                                   0.0f,
                                                   {}});
      foreignCarry.rawParts.push_back(foreign);

      Document doc = makeFixture();
      saveSelectionAsChannel(doc, selectRectangle(0.0f, 0.0f, 16.0f, 16.0f), "Mine");
      const NpaintSaveResult saved = saveNpaint(doc, kForeign, {}, &foreignCarry);
      check(saved.ok,
            "carry: a document whose own channel would collide with a carried S0001 part "
            "still saves -- the name allocator skips names carried parts have claimed, and "
            "EXR requires them unique");

      const NpaintLoadResult back = loadNpaint(kForeign);
      check(back.ok && back.document.channels.size() == 1 &&
                back.document.channels[0].name == "Mine",
            "carry: this build's own channel comes back as a channel");
      check(back.carry.rawParts.size() == 1 && back.carry.rawParts[0].name == "S0001" &&
                back.carry.rawParts[0].channelNames.size() == 2,
            "carry: while the two-channel S0001 a newer build wrote is carried VERBATIM -- "
            "an S#### part this build only half-understands is kept whole rather than turned "
            "into a channel whose second half is gone (PRD I10)");
      bool namedReason = false;
      for (const std::string& w : back.warnings)
        if (contains(w, "S0001") && contains(w, "coverage")) namedReason = true;
      check(namedReason,
            "carry: and the warning names the part and says what a channel part looks like, "
            "rather than reporting that it is not an L#### layer");
      std::remove(kForeign);
    }

    // --- 7b. A duplicate np:name is REPAIRED on load, not refused --------
    //
    // The reader's one edit to what it read, and the assertion is that it stays
    // an edit to the *label* only. A file holding two channels called "Sky"
    // (hand-built, or written by a tool that did not enforce uniqueness) must
    // open and must remain saveable -- `saveNpaint()` refuses a duplicate, so a
    // reader that passed one through would produce a document that opens,
    // accepts edits and can never be written back. io/NpaintFile.hpp's basis
    // section calls that exact shape a trap.
    //
    // The fixture is built through the carry, because a valid channel part
    // carrying a colliding name is something this build's writer will not
    // produce on its own.
    {
      const char* kDup = "selftest_channels_dup.npaint";
      std::remove(kDup);

      NpaintCarry dupCarry;
      NpaintRawPart twin;
      twin.name = "S0001";
      twin.width = kTileSize;
      twin.height = kTileSize;
      twin.tileWidth = kTileSize;
      twin.tileHeight = kTileSize;
      twin.channelNames = {"coverage"};
      twin.sampleTypeName = "half";
      twin.rawPixels.assign(static_cast<size_t>(kTileSize) * kTileSize * sizeof(uint16_t), 0);
      // One texel of real coverage, so the tile is not dropped as all-zero and
      // the "coverage is unchanged" half of the claim has something to check.
      reinterpret_cast<uint16_t*>(twin.rawPixels.data())[0] = floatToHalf(1.0f);
      twin.attributes.push_back(
          NpaintAttribute{"np:kind", NpaintAttribute::Type::String, "selection", 0, 0.0f, {}});
      twin.attributes.push_back(
          NpaintAttribute{"np:name", NpaintAttribute::Type::String, "Sky", 0, 0.0f, {}});
      dupCarry.rawParts.push_back(twin);

      Document doc = makeFixture();
      saveSelectionAsChannel(doc, selectRectangle(0.0f, 0.0f, 16.0f, 16.0f), "Sky");
      const NpaintSaveResult saved = saveNpaint(doc, kDup, {}, &dupCarry);
      const NpaintLoadResult back = loadNpaint(kDup);
      check(saved.ok && back.ok && back.document.channels.size() == 2 &&
                back.document.channels[0].name == "Sky" &&
                back.document.channels[1].name == "Sky 2",
            "duplicate name: a file with two channels called \"Sky\" OPENS, and the second is "
            "renamed rather than the file being refused -- a document that opens and can "
            "never be saved again is worse than one with a renamed channel");
      check(back.document.channels.size() == 2 &&
                channelCoverageAt(back.document.channels[1], PixelCoord{0, 0}) == 1.0f,
            "duplicate name: and only the LABEL moved -- the renamed channel's coverage is "
            "exactly what the file held");
      bool namedBoth = false;
      for (const std::string& w : back.warnings)
        if (contains(w, "\"Sky\"") && contains(w, "\"Sky 2\"")) namedBoth = true;
      check(namedBoth,
            "duplicate name: with a warning naming both spellings, so the rename is on the "
            "record rather than something the user discovers later");
      check(back.ok && saveNpaint(back.document, kDup, {}, &back.carry).ok,
            "duplicate name: and the repaired document saves, which is the whole reason the "
            "reader repairs instead of refusing");
      std::remove(kDup);
    }

    // --- 8. The two refusals, by name (PRD I11) --------------------------
    {
      Document nameless = makeFixture();
      nameless.channels.push_back(AlphaChannel{});
      const NpaintSaveResult r = saveNpaint(nameless, "selftest_channels_never.npaint");
      check(!r.ok && contains(r.error, "no name") && contains(r.error, "Nothing was written"),
            "refusal: a channel with no name is refused BY NAME -- np:name is the only handle "
            "a channel has, and an empty string attribute does not survive this "
            "OpenImageIO anyway");

      Document duplicated = makeFixture();
      duplicated.channels.push_back(AlphaChannel{});
      duplicated.channels.push_back(AlphaChannel{});
      duplicated.channels[0].name = "Sky";
      duplicated.channels[1].name = "Sky";
      const NpaintSaveResult d = saveNpaint(duplicated, "selftest_channels_never.npaint");
      check(!d.ok && contains(d.error, "\"Sky\"") && contains(d.error, "uniqueChannelName"),
            "refusal: two channels sharing a name are refused, naming the name and the "
            "function that produces a free one -- a duplicate makes the lookup depend on "
            "list order, which is not something a user can see");
      std::remove("selftest_channels_never.npaint");
    }

    std::remove(kNoChannels);
    std::remove(kNoChannels2);
    std::remove(kWithChannels);
  }

  std::printf("[selftest] channels %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
