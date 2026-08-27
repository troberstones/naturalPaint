#include "app/selftest/Support.hpp"

#include "core/LayerGeometry.hpp"
#include "core/LayerSetOps.hpp"

namespace np {

bool runLayerMultiSelectTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // PLAN.md §1.5. Everything except part J is pure -- the selection model, the
  // translate, align/distribute, links, labels and the panel filter reach no
  // file, no encoder and no GPU. Part J is the `.npaint` round trip.
  std::printf(
      "[selftest] layer multi-select: everything but part J reaches no file, no encoder "
      "and no GPU; part J is the `.npaint` round trip\n");

  // ======================================================================
  // The two verbs PRD C12 names that this step REFUSES, printed rather than
  // left in a header, because a P0 row three-fifths met is what a reader most
  // needs told outright.
  // ======================================================================
  std::printf("  C12 asks for \"move, transform, group, delete and set properties as a set\". "
              "BUILT: move (reorder AND translate), delete, group/ungroup, set properties "
              "(visible, locked, clipped, opacity, blend, colour label, link).\n");
  std::printf("  REFUSED -- **transform**: there is no geometric transform of a layer anywhere "
              "in this codebase and this step did not add one. What it added is an "
              "integer-pixel translate, the one case that needs no resampling. Rotate, scale, "
              "skew and sub-pixel offset each need a filter-kernel choice, a premultiplied-alpha "
              "rule and -- on a Pigment layer -- a decision about whether a latent triple may be "
              "interpolated at all (DESIGN-imaging.md 3). That is phase 6.\n");
  std::printf("  **group**: the MODEL is built, and NOTHING REACHES IT YET. This section used "
              "to print group as refused for want of a LayerKind::Group, a compositor, an "
              "honoured parent link and a channel-less part writer; all four now exist (see "
              "app/selftest/LayerGroup.cpp for the model, the composite integration and the "
              "`.npaint` round trip). But there is no LAYERS-panel gesture and no menu item "
              "that issues GroupLayers or UngroupLayers, so a user cannot group anything. "
              "Printing this as delivered would make it exactly the defect "
              "docs/reachability-audit.md is named after -- a whole subsystem built, tested "
              "and unreachable -- which is why C7 stays open in that document until the "
              "panel work lands.\n");
  std::printf("  PRD C13 is delivered in FULL rather than partially: align and distribute both "
              "move pixels, because core::Layer still has no position -- the same wall "
              "core/LayerComp.hpp hit at step 12. The two primitives it named as missing (a "
              "content bounding box and a translate) are built here.\n");

  // --- Fixtures -------------------------------------------------------------
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex]
        .rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  // A solid rectangle of opaque content, in absolute document coordinates.
  auto fillRgb = [&](Document& doc, size_t layerIndex, int32_t x0, int32_t y0, int32_t x1,
                     int32_t y1, float v) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) writeRgb(doc, layerIndex, x, y, {v, v, v, 1.0f});
  };
  auto names = [](const Document& doc) {
    std::string s;
    for (const Layer& l : doc.layers) {
      if (!s.empty()) s += ",";
      s += l.name.empty() ? "?" : l.name;
    }
    return s;
  };
  // Five named layers, bottom to top, in one blank document.
  auto makeFive = []() {
    OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{});
    od.document.layers[0].name = "A";
    for (const char* n : {"B", "C", "D", "E"})
      addLayer(od.document, od.document.layers.size(), makeRgbLayer(n));
    od.recordEdit("fixture", EditKind::Structural);
    return od;
  };
  auto sel = [](std::vector<size_t> v) { return makeLayerSelection(std::move(v)); };

  // ======================================================================
  // Part A: the content bounding box -- the first of the two primitives
  //         core/LayerComp.hpp named as missing
  // ======================================================================
  {
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    check(layerContentBounds(doc.layers[0]).empty,
          "bounds: an engaged but empty store has EMPTY bounds");

    // Deliberately straddling a tile boundary in both axes: 100..200 crosses
    // x=128, so a bounding box that stopped at the first tile would be caught.
    fillRgb(doc, 0, 100, 30, 200, 40, 0.5f);
    const LayerBounds b = layerContentBounds(doc.layers[0]);
    check(!b.empty && b.minX == 100 && b.maxX == 200 && b.minY == 30 && b.maxY == 40,
          "bounds: exact to the pixel across a tile edge (100..200 crosses x=128)");
    check(b.width() == 101 && b.height() == 11,
          "bounds: inclusive on both axes, so width is maxX-minX+1");

    // Alpha is what counts, not the colour channels: a texel with colour and
    // zero alpha contributes nothing through `over` (values are premultiplied).
    Document colourOnly = Document::createBlank(64, 64, WorkingSpace{});
    writeRgb(colourOnly, 0, 5, 5, {0.9f, 0.9f, 0.9f, 0.0f});
    check(layerContentBounds(colourOnly.layers[0]).empty,
          "bounds: a zero-alpha texel is NOT content -- premultiplied, it adds nothing");

    // Not clipped to the canvas: tiles live in absolute document coordinates
    // and content can hang off the edge (core/Merge already warns about it).
    Document offCanvas = Document::createBlank(64, 64, WorkingSpace{});
    fillRgb(offCanvas, 0, -20, -10, -15, -5, 0.5f);
    const LayerBounds ob = layerContentBounds(offCanvas.layers[0]);
    check(!ob.empty && ob.minX == -20 && ob.minY == -10,
          "bounds: NOT clipped to the canvas -- content off the left edge keeps a negative minX");

    // A kind with no pixel storage occupies no document pixel at all. Reporting
    // 0,0 would put it into every alignment as a point at the corner.
    Document adj = Document::createBlank(64, 64, WorkingSpace{});
    addLayer(adj, 1, makeAdjustmentLayer("Grade"));
    check(layerContentBounds(adj.layers[1]).empty,
          "bounds: an Adjustment layer holds no pixels, so its bounds are empty, not 0,0");

    // A Pigment layer's content is mass, not alpha -- a different channel of a
    // 7-channel tile, so the scan is not accidentally sharing RGB's layout.
    Document pig = Document::createBlank(64, 64, WorkingSpace{});
    addLayer(pig, 1, makePigmentLayer("Pig"));
    {
      const PixelCoord at{9, 11};
      PigmentTexel t;
      t.latent.c[0] = 0.3f;
      t.mass = 0.75f;
      pig.layers[1].pigmentTiles->getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), t);
    }
    const LayerBounds pb = layerContentBounds(pig.layers[1]);
    check(!pb.empty && pb.minX == 9 && pb.maxX == 9 && pb.minY == 11 && pb.maxY == 11,
          "bounds: a Pigment layer's content is its MASS channel, at the right texel");
    {
      // ... and a latent with zero mass is not content: mass 0 projects to
      // fully transparent, so it would contribute nothing to a composite.
      Document massless = Document::createBlank(64, 64, WorkingSpace{});
      addLayer(massless, 1, makePigmentLayer("Pig"));
      const PixelCoord at{4, 4};
      PigmentTexel t;
      t.latent.c[1] = 0.9f;
      t.mass = 0.0f;
      massless.layers[1].pigmentTiles->getOrCreate(tileCoordAt(at))
          .writeTexel(tileLocalOffset(at), t);
      check(layerContentBounds(massless.layers[1]).empty,
            "bounds: a latent with zero mass is not content either");
    }
  }

  // ======================================================================
  // Part B: the translate -- the second missing primitive, and the claim
  //         that matters is LOSSLESSNESS AT ZERO TOLERANCE
  // ======================================================================
  {
    // The words themselves, not decoded values: a translate that moved raw half
    // words cannot round, and this is what proves it structurally.
    auto wordsAt = [](const Document& doc, size_t layer, int32_t x, int32_t y) {
      const PixelCoord at{x, y};
      std::array<uint16_t, 4> out{0, 0, 0, 0};
      const Tile* t = doc.layers[layer].rgbTiles->find(tileCoordAt(at));
      if (t == nullptr) return out;
      const PixelCoord local = tileLocalOffset(at);
      const uint16_t* w =
          t->data() +
          (static_cast<size_t>(local.y) * kTileSize + static_cast<size_t>(local.x)) * 4;
      out = {w[0], w[1], w[2], w[3]};
      return out;
    };

    Document doc = Document::createBlank(512, 512, WorkingSpace{});
    // Values chosen so no two texels share a word pattern, so a shift that
    // copied the wrong row would not accidentally compare equal.
    for (int32_t y = 60; y <= 200; ++y)
      for (int32_t x = 60; x <= 200; ++x)
        writeRgb(doc, 0, x, y,
                 {static_cast<float>(x) / 977.0f, static_cast<float>(y) / 991.0f,
                  static_cast<float>(x * y) / 199999.0f, 1.0f});
    const size_t tilesBefore = doc.layers[0].rgbTiles->occupiedTileCount();

    // --- sub-tile: the case a re-key cannot express ------------------------
    Document shifted = doc;
    const LayerOpResult sub = translateLayer(shifted, 0, 37, -19);
    check(sub.ok, "translate: a sub-tile shift succeeds");
    bool identical = true;
    size_t compared = 0;
    for (int32_t y = 60; y <= 200 && identical; ++y) {
      for (int32_t x = 60; x <= 200; ++x) {
        if (wordsAt(doc, 0, x, y) != wordsAt(shifted, 0, x + 37, y - 19)) {
          identical = false;
          break;
        }
        ++compared;
      }
    }
    check(identical && compared == 141 * 141,
          "translate: every one of 19881 texels is BIT-IDENTICAL after a sub-tile shift");
    std::printf("  translate: %zu texels compared as raw half words at ZERO tolerance -- the "
                "move is a memcpy of uint16 words, so there is no decode step that could round. "
                "Losslessness is structural, not measured luck.\n",
                compared);
    // The vacated region really is empty afterwards: a shift that copied
    // without clearing would pass the test above and still be wrong.
    check((wordsAt(shifted, 0, 60, 60) == std::array<uint16_t, 4>{0, 0, 0, 0}),
          "translate: and the vacated corner is empty -- content moved, not duplicated");

    const LayerBounds before = layerContentBounds(doc.layers[0]);
    const LayerBounds after = layerContentBounds(shifted.layers[0]);
    check(after.minX == before.minX + 37 && after.minY == before.minY - 19 &&
              after.width() == before.width() && after.height() == before.height(),
          "translate: the content bounding box moves by exactly the delta and keeps its size");

    // A round trip returns the original store bit for bit, which is the
    // strongest single statement of losslessness available.
    Document back = shifted;
    translateLayer(back, 0, -37, 19);
    bool roundTripped = back.layers[0].rgbTiles->occupiedTileCount() == tilesBefore;
    for (int32_t y = 60; y <= 200 && roundTripped; ++y)
      for (int32_t x = 60; x <= 200; ++x)
        if (wordsAt(doc, 0, x, y) != wordsAt(back, 0, x, y)) {
          roundTripped = false;
          break;
        }
    check(roundTripped,
          "translate: shifting by (37,-19) and back gives the ORIGINAL store, tile count too");

    // --- whole-tile: the path that copies nothing at all -------------------
    Document whole = doc;
    const size_t sharedBefore = whole.layers[0].rgbTiles->sharedTileCount();
    translateLayer(whole, 0, 2 * kTileSize, -kTileSize);
    const size_t sharedAfter = whole.layers[0].rgbTiles->sharedTileCount();
    check(whole.layers[0].rgbTiles->occupiedTileCount() == tilesBefore &&
              sharedAfter == sharedBefore && sharedAfter == tilesBefore,
          "translate: a whole-tile shift is a RE-KEY -- same tile count, every tile still "
          "shared with the source, so zero bytes were copied");
    bool wholeIdentical = true;
    for (int32_t y = 60; y <= 200 && wholeIdentical; ++y)
      for (int32_t x = 60; x <= 200; ++x)
        if (wordsAt(doc, 0, x, y) != wordsAt(whole, 0, x + 2 * kTileSize, y - kTileSize)) {
          wholeIdentical = false;
          break;
        }
    check(wholeIdentical, "translate: and the re-keyed content is bit-identical too");
    std::printf("  translate cost, measured: %zu source tiles (%zu KiB). Whole-tile path: **0 "
                "bytes copied**, all %zu tiles still shared with the source document. Sub-tile "
                "path: %zu destination tiles, %zu KiB, every byte of it copied -- that is the "
                "price of the one case a re-key cannot express.\n",
                tilesBefore, doc.layers[0].rgbTiles->tileBytes() / 1024, sharedAfter,
                shifted.layers[0].rgbTiles->occupiedTileCount(),
                shifted.layers[0].rgbTiles->tileBytes() / 1024);

    // --- the empty-tile prune ---------------------------------------------
    // Without it a gather would leave up to four tiles per source tile, three
    // of them holding nothing -- which is exactly the "memory tracks canvas,
    // not content" bug PRD C2 exists to prevent.
    Document sparse = Document::createBlank(512, 512, WorkingSpace{});
    writeRgb(sparse, 0, 64, 64, {0.5f, 0.5f, 0.5f, 1.0f});
    const size_t sparseBefore = sparse.layers[0].rgbTiles->occupiedTileCount();
    translateLayer(sparse, 0, 1, 1);
    check(sparseBefore == 1 && sparse.layers[0].rgbTiles->occupiedTileCount() == 1,
          "translate: all-default destination tiles are DROPPED -- one texel nudged by (1,1) "
          "still costs one tile, not four");

    // --- the mask moves with the layer, and its default is REVEAL ----------
    Document masked = Document::createBlank(512, 512, WorkingSpace{});
    fillRgb(masked, 0, 10, 10, 40, 40, 0.7f);
    addLayerMask(masked, 0);
    for (int32_t y = 10; y <= 40; ++y)
      for (int32_t x = 10; x <= 40; ++x) {
        const PixelCoord at{x, y};
        masked.layers[0].mask->getOrCreate(tileCoordAt(at))
            .writeCoverage(tileLocalOffset(at), 0.0f);
      }
    translateLayer(masked, 0, 25, 3);
    auto coverageAt = [&](const Document& d, int32_t x, int32_t y) {
      const PixelCoord at{x, y};
      const MaskTile* t = d.layers[0].mask->find(tileCoordAt(at));
      return t == nullptr ? 1.0f : t->readCoverage(tileLocalOffset(at));
    };
    check(coverageAt(masked, 35, 13) == 0.0f,
          "translate: the MASK moves with the layer -- the hidden patch is at the new position");
    check(coverageAt(masked, 10, 10) == 1.0f,
          "translate: and what moved in behind it reads 1.0 (reveal), not 0.0 -- a mask tile's "
          "default is the codebase's one non-zero default, and zeroing it would have made "
          "every mask translate a partial erase");

    // --- refusals, each with its numbers -----------------------------------
    Document refuse = Document::createBlank(64, 64, WorkingSpace{});
    fillRgb(refuse, 0, 1, 1, 4, 4, 0.5f);
    addLayer(refuse, 1, makeAdjustmentLayer("Grade"));
    setLayerLocked(refuse, 0, true);
    const LayerOpResult onLocked = translateLayer(refuse, 0, 1, 0);
    check(!onLocked.ok && contains(onLocked.error, "locked") && contains(onLocked.error, "layer 0"),
          "translate: refuses a LOCKED layer by name -- moving pixels is content");
    const LayerOpResult onAdjustment = translateLayer(refuse, 1, 1, 0);
    check(!onAdjustment.ok && contains(onAdjustment.error, "Adjustment") &&
              contains(onAdjustment.error, "no pixels"),
          "translate: refuses a layer that holds no pixels, naming the kind");
    const LayerOpResult outOfRange = translateLayer(refuse, 9, 1, 0);
    check(!outOfRange.ok && contains(outOfRange.error, "index 9") &&
              contains(outOfRange.error, "2 layer"),
          "translate: refuses an out-of-range index with both numbers");
  }

  // ======================================================================
  // Part C: what a multi-selection MEANS for each command -- ordering,
  //         all-or-nothing, and the classic delete bug run beside it
  // ======================================================================
  {
    // Delete {0,2,4} of A,B,C,D,E. Descending order is the whole point.
    OpenDocument od = makeFive();
    check(names(od.document) == "A,B,C,D,E", "set: the fixture is A,B,C,D,E bottom to top");
    const LayerSetOpResult del =
        applyLayerSetOp(od.document, LayerSetCommand::DeleteLayers, sel({0, 2, 4}));
    check(del.ok && names(od.document) == "B,D",
          "set delete {0,2,4}: the survivors are B and D -- by NAME, not by count");

    // The rejected alternative, implemented here rather than described: the
    // same three indices walked upwards, which is the bug the ordering rule
    // exists to prevent.
    Document naive = makeFive().document;
    for (const size_t i : {size_t{0}, size_t{2}, size_t{4}})
      if (i < naive.layers.size()) removeLayer(naive, i);
    check(names(naive) == "B,C,E",
          "set delete: the ascending walk leaves B,C,E -- it destroyed D, which was never "
          "named, and its third delete fell off the end of a stack that had shrunk under it");
    std::printf("  set delete {0,2,4} of A,B,C,D,E: descending leaves \"B,D\" (correct); the "
                "ascending walk an implementation without the rule would take leaves \"%s\" -- "
                "it deleted A and D (D was never named), and index 4 was out of range by then "
                "so the third delete silently did nothing at all. Two failures from one "
                "ordering choice.\n",
                names(naive).c_str());

    check(del.selection.size() == 1 && del.selection.indices[0] == 0,
          "set delete: the selection lands on the row that took the lowest deleted one's place");
  }
  {
    // All-or-nothing: one locked member refuses the whole set, and the document
    // is not merely "mostly" unchanged.
    OpenDocument od = makeFive();
    setLayerLocked(od.document, 2, true);
    const uint64_t rev = od.revision;
    const size_t entries = od.history.entries().size();
    const std::string before = names(od.document);
    const LayerSetEditResult r =
        applyLayerSetCommand(od, LayerSetCommand::DeleteLayers, sel({0, 2, 4}));
    check(!r.ok && contains(r.error, "locked") && contains(r.error, "layer 2"),
          "set delete with one LOCKED member: the whole set refuses, in core/LayerOps' own "
          "sentence naming that layer");
    check(names(od.document) == before && od.document.layers.size() == 5,
          "set delete refused: all five layers are still there -- no partial application");
    check(od.revision == rev && od.history.entries().size() == entries,
          "set delete refused: the revision did not move and no history entry was appended");
    check(r.selection == sel({0, 2, 4}), "set delete refused: the selection comes back unchanged");
  }
  {
    // Move Up over a set that contains the top layer.
    OpenDocument od = makeFive();
    const LayerSetOpResult top =
        applyLayerSetOp(od.document, LayerSetCommand::MoveLayersUp, sel({3, 4}));
    check(!top.ok && contains(top.error, "top of the stack") && contains(top.error, "'E'") &&
              names(od.document) == "A,B,C,D,E",
          "set move up including the TOP layer: refused whole, naming that layer rather than "
          "an index the user never typed");
    check(!layerSetCommandAvailable(od.document, LayerSetCommand::MoveLayersUp, sel({3, 4})),
          "and the command is unavailable for that selection, so the menu greys it first");

    const LayerSetOpResult up =
        applyLayerSetOp(od.document, LayerSetCommand::MoveLayersUp, sel({1, 2}));
    check(up.ok && names(od.document) == "A,D,B,C,E" && up.selection == sel({2, 3}),
          "set move up {1,2}: descending order keeps B and C adjacent and in order");

    OpenDocument od2 = makeFive();
    const LayerSetOpResult down =
        applyLayerSetOp(od2.document, LayerSetCommand::MoveLayersDown, sel({2, 3}));
    check(down.ok && names(od2.document) == "A,C,D,B,E" && down.selection == sel({1, 2}),
          "set move down {2,3}: ASCENDING order, the mirror of move up");
    const LayerSetOpResult bottom =
        applyLayerSetOp(od2.document, LayerSetCommand::MoveLayersDown, sel({0, 1}));
    check(!bottom.ok && contains(bottom.error, "bottom") && names(od2.document) == "A,C,D,B,E",
          "set move down including the BOTTOM layer: refused whole, nothing moved");
  }
  {
    // Duplicate {0,2}: the copies land at 1 and 4, and the selection follows
    // them -- the rule the single-selection Duplicate already follows.
    OpenDocument od = makeFive();
    const LayerSetOpResult dup =
        applyLayerSetOp(od.document, LayerSetCommand::DuplicateLayers, sel({0, 2}));
    check(dup.ok && names(od.document) == "A,A copy,B,C,C copy,D,E",
          "set duplicate {0,2}: descending again -- each copy lands directly above its source");
    check(dup.selection == sel({1, 4}),
          "set duplicate: the selection follows the COPIES, at the indices the shift produced");
  }
  {
    // Property writes as a set, and the one edit / one undo rule.
    OpenDocument od = makeFive();
    const uint64_t rev = od.revision;
    const size_t entries = od.history.entries().size();
    const LayerSetEditResult hide =
        applyLayerSetCommand(od, LayerSetCommand::HideLayers, sel({0, 2, 4}));
    check(hide.ok && !od.document.layers[0].visible && od.document.layers[1].visible &&
              !od.document.layers[2].visible && od.document.layers[3].visible &&
              !od.document.layers[4].visible,
          "set hide {0,2,4}: exactly those three, and no others");
    check(od.revision == rev + 1 && od.history.entries().size() == entries + 1,
          "set hide: THREE layers changed, ONE revision bump, ONE history entry");
    check(contains(od.history.entries().back().label, "hide 3 layers"),
          "set hide: and the history row names the set, not one of its members");

    const Document* undone = od.history.undo();
    check(undone != nullptr && undone->layers.size() == 5 && undone->layers[0].visible &&
              undone->layers[2].visible && undone->layers[4].visible,
          "set hide: ONE undo gives back all three -- not three undos, which would leave the "
          "user in a state they never saw");
  }
  {
    // Delete as one edit, undone as one edit.
    OpenDocument od = makeFive();
    const size_t entries = od.history.entries().size();
    applyLayerSetCommand(od, LayerSetCommand::DeleteLayers, sel({0, 2, 4}));
    check(od.history.entries().size() == entries + 1 &&
              contains(od.history.entries().back().label, "delete 3 layers"),
          "set delete: one history entry, labelled with the whole gesture");
    const Document* undone = od.history.undo();
    check(undone != nullptr && names(*undone) == "A,B,C,D,E",
          "set delete: one undo restores the entire pre-edit stack, in order");
  }
  {
    // The atomic trial's cost, measured against the deep copy it replaces.
    OpenDocument od = makeBlankOpenDocument(1024, 1024, WorkingSpace{});
    for (int32_t ty = 0; ty < 8; ++ty)
      for (int32_t tx = 0; tx < 8; ++tx)
        od.document.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty});
    const size_t tiles = od.document.layers[0].rgbTiles->occupiedTileCount();
    Document trial = od.document;  // exactly what applyLayerSetOp() makes
    const size_t shared = trial.layers[0].rgbTiles->sharedTileCount();
    const size_t exclusive = trial.layers[0].rgbTiles->exclusiveTileBytes();
    Document deep = od.document;
    deep.layers[0].rgbTiles->unshareAll();
    check(tiles == 64 && shared == tiles && exclusive == 0,
          "the atomic trial's copy shares every tile -- it costs no tile bytes at all");
    std::printf("  atomic trial, measured on a %zu-tile layer: the trial copy owns %zu tile "
                "bytes; the deep copy it replaces (unshareAll(), the pre-step-6 behaviour) owns "
                "%zu KiB. The rejected alternative -- predicting each refusal with a pre-flight "
                "predicate -- costs nothing at run time and a second copy of every refusal rule, "
                "which is how a UI comes to grey out what the model allows.\n",
                tiles, exclusive, deep.layers[0].rgbTiles->exclusiveTileBytes() / 1024);
  }

  // ======================================================================
  // Part D: links -- what one propagates, and what happens to a partner
  // ======================================================================
  {
    OpenDocument od = makeFive();
    const LayerSetEditResult link =
        applyLayerSetCommand(od, LayerSetCommand::LinkLayers, sel({0, 2}));
    check(link.ok && od.document.layers[0].linkGroup != 0 &&
              od.document.layers[0].linkGroup == od.document.layers[2].linkGroup &&
              od.document.layers[1].linkGroup == 0,
          "link {0,2}: both carry one non-zero group number and nothing else does");
    check(linkedLayers(od.document, 0) == std::vector<size_t>({0, 2}) &&
              layerIsLinked(od.document, 0) && !layerIsLinked(od.document, 1),
          "link: the relation is symmetric for free, because it is one shared number");

    const uint64_t group = od.document.layers[0].linkGroup;
    // Deleting one member: the survivor keeps its number and stops being
    // linked. No cleanup pass runs anywhere; the resolver applies the rule.
    OpenDocument afterDelete = od;
    applyLayerSetCommand(afterDelete, LayerSetCommand::DeleteLayers, sel({0}));
    const size_t survivor = 1;  // C, which was index 2
    check(afterDelete.document.layers.size() == 4 &&
              afterDelete.document.layers[survivor].name == "C" &&
              afterDelete.document.layers[survivor].linkGroup == group,
          "link: deleting one member leaves the survivor's group NUMBER untouched -- nothing "
          "walks the stack to repair anything");
    check(!layerIsLinked(afterDelete.document, survivor) &&
              linkedLayers(afterDelete.document, survivor) == std::vector<size_t>({survivor}),
          "link: and a group with fewer than two live members is NOT a link -- resolved, never "
          "repaired, so no caller ever sees a dangling id");
    const Document* undone = afterDelete.history.undo();
    check(undone != nullptr && undone->layers.size() == 5 && layerIsLinked(*undone, 0),
          "link: undo brings the partner back and the link with it, because the number was "
          "never destroyed");

    // What a link does NOT propagate.
    OpenDocument nonProp = od;
    applyLayerSetCommand(nonProp, LayerSetCommand::HideLayers, sel({0}));
    check(!nonProp.document.layers[0].visible && nonProp.document.layers[2].visible,
          "link: visibility does NOT propagate -- a link must not become a second, invisible "
          "selection");
    OpenDocument delProp = od;
    applyLayerSetCommand(delProp, LayerSetCommand::DeleteLayers, sel({0}));
    check(delProp.document.layers.size() == 4,
          "link: deletion does NOT propagate either -- one row highlighted, one layer gone");
  }

  // ======================================================================
  // Part E: align and distribute -- PRD C13, in pixels
  // ======================================================================
  {
    auto makeThree = []() {
      OpenDocument od = makeBlankOpenDocument(400, 400, WorkingSpace{});
      od.document.layers[0].name = "Low";
      addLayer(od.document, 1, makeRgbLayer("Mid"));
      addLayer(od.document, 2, makeRgbLayer("Top"));
      return od;
    };
    OpenDocument od = makeThree();
    fillRgb(od.document, 0, 10, 10, 29, 29, 0.6f);      // 20x20 at x=10
    fillRgb(od.document, 1, 100, 50, 139, 89, 0.6f);    // 40x40 at x=100
    fillRgb(od.document, 2, 200, 300, 209, 309, 0.6f);  // 10x10 at x=200

    OpenDocument left = od;
    const LayerSetEditResult l =
        applyLayerSetCommand(left, LayerSetCommand::AlignSelectionLeft, sel({0, 1, 2}));
    const LayerBounds b0 = layerContentBounds(left.document.layers[0]);
    const LayerBounds b1 = layerContentBounds(left.document.layers[1]);
    const LayerBounds b2 = layerContentBounds(left.document.layers[2]);
    check(l.ok && b0.minX == 10 && b1.minX == 10 && b2.minX == 10,
          "align left to selection: all three left edges land on the leftmost, x=10");
    check(b0.minY == 10 && b1.minY == 50 && b2.minY == 300 && b1.width() == 40,
          "align left: the other axis and every size are untouched -- a translate, not a fit");

    OpenDocument centred = od;
    const LayerSetEditResult cr =
        applyLayerSetCommand(centred, LayerSetCommand::AlignCanvasCenterX, sel({0, 1, 2}));
    bool allCentred = true;
    for (size_t i = 0; i < 3; ++i) {
      const LayerBounds b = layerContentBounds(centred.document.layers[i]);
      // The canvas centre of 0..399 is 199.5; an integer translate can only get
      // within half a pixel of it, which is exactly what is asserted.
      if (std::abs((b.minX + b.maxX) / 2.0 - 199.5) > 0.5) allCentred = false;
    }
    check(cr.ok && allCentred,
          "align horizontal centre to canvas: every layer's centre lands within half a pixel of "
          "the canvas centre -- the bound an INTEGER translate can achieve");
    check(cr.warnings.empty(),
          "align: three even-width layers on a canvas whose centre is also a half pixel need "
          "NO rounding, and no warning is invented for them");
    {
      // An odd-width layer, whose centre is a whole pixel while the canvas
      // centre of 0..399 is 199.5: the half pixel an integer translate cannot
      // reach. It is rounded and the residual is reported.
      OpenDocument odd = makeThree();
      fillRgb(odd.document, 0, 0, 0, 10, 10, 0.6f);    // 11 wide, centre 5.0
      fillRgb(odd.document, 1, 50, 0, 89, 10, 0.6f);   // 40 wide, centre 69.5
      const LayerSetEditResult rr =
          applyLayerSetCommand(odd, LayerSetCommand::AlignCanvasCenterX, sel({0, 1}));
      const LayerBounds ob = layerContentBounds(odd.document.layers[0]);
      check(rr.ok && std::abs((ob.minX + ob.maxX) / 2.0 - 199.5) == 0.5,
            "align: an odd-width layer lands exactly half a pixel off a half-pixel canvas "
            "centre -- the residual an integer translate cannot remove");
      check(!rr.warnings.empty() && contains(rr.warnings[0], "rounded") &&
                contains(rr.warnings[0], "0.5"),
            "align: and the rounding is REPORTED with the number rather than swallowed -- a "
            "sub-pixel move would resample, and this build has no transform");
    }

    // Distribute: equal centre spacing, ends fixed.
    OpenDocument dist = od;
    const LayerSetEditResult d =
        applyLayerSetCommand(dist, LayerSetCommand::DistributeHorizontally, sel({0, 1, 2}));
    const LayerBounds d0 = layerContentBounds(dist.document.layers[0]);
    const LayerBounds d1 = layerContentBounds(dist.document.layers[1]);
    const LayerBounds d2 = layerContentBounds(dist.document.layers[2]);
    const double c0 = (d0.minX + d0.maxX) / 2.0;
    const double c1 = (d1.minX + d1.maxX) / 2.0;
    const double c2 = (d2.minX + d2.maxX) / 2.0;
    check(d.ok && c0 == 19.5 && c2 == 204.5,
          "distribute horizontally: the two END units do not move");
    check(std::abs((c1 - c0) - (c2 - c1)) <= 1.0,
          "distribute: the middle unit's centre is equidistant from both ends, to within the "
          "one pixel an integer translate can resolve");
    std::printf("  distribute horizontally, measured: centres at %.1f, %.1f, %.1f -- gaps %.1f "
                "and %.1f, differing by %.1f px, which is the integer-translate residual and "
                "not an error.\n",
                c0, c1, c2, c1 - c0, c2 - c1, std::abs((c1 - c0) - (c2 - c1)));

    // A link group aligns as ONE unit: the two members keep their offset.
    OpenDocument linked = od;
    applyLayerSetOp(linked.document, LayerSetCommand::LinkLayers, sel({1, 2}));
    const LayerBounds pre1 = layerContentBounds(linked.document.layers[1]);
    const LayerBounds pre2 = layerContentBounds(linked.document.layers[2]);
    const int32_t offsetBefore = pre2.minX - pre1.minX;
    const LayerSetEditResult la =
        applyLayerSetCommand(linked, LayerSetCommand::AlignSelectionLeft, sel({0, 1}));
    const LayerBounds post1 = layerContentBounds(linked.document.layers[1]);
    const LayerBounds post2 = layerContentBounds(linked.document.layers[2]);
    check(la.ok && post2.minX - post1.minX == offsetBefore,
          "align: a LINK GROUP moves as one unit -- layer 2 was never selected, moved by the "
          "same delta, and the pair is exactly as far apart as before");
    check(post1.minX != pre1.minX,
          "align: and it really did move, so the assertion above is not vacuous");

    // Refusals.
    OpenDocument oneUnit = od;
    applyLayerSetOp(oneUnit.document, LayerSetCommand::LinkLayers, sel({0, 1}));
    const LayerSetOpResult single =
        applyLayerSetOp(oneUnit.document, LayerSetCommand::AlignSelectionLeft, sel({0, 1}));
    check(!single.ok && contains(single.error, "single unit"),
          "align to selection: a selection that is all one link group refuses -- there is "
          "nothing to align it TO, and it says so rather than doing nothing");
    OpenDocument withEmpty = makeThree();
    fillRgb(withEmpty.document, 0, 5, 5, 9, 9, 0.5f);
    const LayerSetOpResult emptyMember =
        applyLayerSetOp(withEmpty.document, LayerSetCommand::AlignSelectionLeft, sel({0, 1}));
    check(!emptyMember.ok && contains(emptyMember.error, "no content") &&
              contains(emptyMember.error, "layer 1"),
          "align: a member with no content at all refuses by name -- it has no edges");
    const LayerSetOpResult tooFew =
        applyLayerSetOp(od.document, LayerSetCommand::DistributeHorizontally, sel({0, 1}));
    check(!tooFew.ok && contains(tooFew.error, "at least 3"),
          "distribute: fewer than three units refuses, with the number needed");
  }

  // ======================================================================
  // Part F: colour labels
  // ======================================================================
  {
    OpenDocument od = makeFive();
    setLayerLocked(od.document, 1, true);
    const LayerSetEditResult r = applyLayerSetCommand(od, LayerSetCommand::LabelRed, sel({0, 1}));
    check(r.ok && od.document.layers[0].colorLabel == "red" &&
              od.document.layers[1].colorLabel == "red",
          "label red over a set: applied, INCLUDING to the locked member");
    std::printf("  a colour label and a link are settable on a LOCKED layer, deliberately: "
                "neither is part of how a layer looks, and marking a layer you have finished -- "
                "which is the commonest reason to lock one -- would otherwise be the one thing "
                "the lock forbade.\n");
    check(contains(layerRowSubLine(od.document.layers[0]), "RED"),
          "label: the panel row says RED, upper-cased as carried");
    check(layerRowSubLine(od.document.layers[2]) == "RGB \xC2\xB7 NORMAL \xC2\xB7 100%",
          "label: an UNLABELLED row is character-identical to what it said before this step");
    check(layerColorLabelSwatch("red").has_value() && !layerColorLabelSwatch("teal").has_value() &&
              !layerColorLabelSwatch(kNoLayerColorLabel).has_value(),
          "label: a name this build has no swatch for gets NO chip rather than a default one");
    Layer unknown;
    unknown.kind = LayerKind::RGB;
    unknown.colorLabel = "teal";
    check(contains(layerRowSubLine(unknown), "TEAL"),
          "label: ... and shows as its own text instead, so two unknown labels never look alike");
    const LayerSetEditResult clear =
        applyLayerSetCommand(od, LayerSetCommand::LabelNone, sel({0, 1}));
    check(clear.ok && od.document.layers[0].colorLabel.empty(),
          "label none clears it, and is a command like any other");
  }

  // ======================================================================
  // Part G: the panel filter -- and the hidden-row rule
  // ======================================================================
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    od.document.layers[0].name = "Sky";
    addLayer(od.document, 1, makeRgbLayer("Skin"));
    addLayer(od.document, 2, makePigmentLayer("Shadow"));
    addLayer(od.document, 3, makeAdjustmentLayer("Sky grade"));

    LayerFilter f;
    check(!f.active() && layersMatchingFilter(od.document, f).size() == 4,
          "filter: an inactive filter matches every layer, so no caller needs a special case");
    f.text = "sk";
    check(layersMatchingFilter(od.document, f) == std::vector<size_t>({0, 1, 3}),
          "filter by name: case-insensitive substring -- Sky, Skin and 'Sky grade'");
    f.text.clear();
    f.kind = LayerKind::Pigment;
    check(layersMatchingFilter(od.document, f) == std::vector<size_t>({2}),
          "filter by kind: exactly the Pigment layer");
    f.text = "sh";
    check(layersMatchingFilter(od.document, f) == std::vector<size_t>({2}),
          "filter: name and kind together are an AND, not an OR");

    // An unnamed layer's row reads "Layer N", and that is what a user searches.
    OpenDocument unnamed = makeBlankOpenDocument(64, 64, WorkingSpace{});
    addLayer(unnamed.document, 1, makeRgbLayer(""));
    LayerFilter byRow;
    byRow.text = "layer 2";
    check(layersMatchingFilter(unnamed.document, byRow) == std::vector<size_t>({1}),
          "filter: matches the row TITLE, so an unnamed layer is findable by the text it shows");

    // The rule that matters: a hidden row stays selected, and a command acts
    // only on what is visible.
    LayerFilter skin;
    skin.text = "skin";
    const LayerSelection selection = sel({0, 1});
    const LayerSelection visible = restrictSelectionToFilter(od.document, selection, skin);
    check(visible == sel({1}) && layersHiddenFromSelection(od.document, selection, skin) == 1,
          "filter: restriction drops the hidden member and reports that it dropped one");
    const LayerSetEditResult hidden =
        applyLayerSetCommand(od, LayerSetCommand::HideLayers, visible);
    check(hidden.ok && od.document.layers[0].visible && !od.document.layers[1].visible,
          "filter: Hide with a filter active leaves the FILTERED-OUT layer alone -- five rows on "
          "screen must never mean eight layers changed");
    check(selection == sel({0, 1}),
          "filter: and the selection itself is untouched, so clearing the box brings the hidden "
          "row back exactly as it was");
  }

  // ======================================================================
  // Part H: the command table, and the two views that walk it
  // ======================================================================
  {
    const std::vector<LayerSetCommand>& all = allLayerSetCommands();
    bool everyLabelled = true;
    for (const LayerSetCommand c : all)
      if (std::strcmp(layerSetCommandLabel(c), "?") == 0) everyLabelled = false;
    check(all.size() == 36 && everyLabelled,
          "every one of the 36 set commands has a menu label -- the LAYERS panel and the "
          "Layer > Selection menu both walk this one list (34, plus GroupLayers and "
          "UngroupLayers)");
    OpenDocument od = makeFive();
    check(!layerSetCommandAvailable(od.document, LayerSetCommand::DeleteLayers, LayerSelection{}) &&
              !layerSetCommandAvailable(od.document, LayerSetCommand::DeleteLayers, sel({9})),
          "availability: an empty selection and an out-of-range one offer nothing");
    check(!layerSetCommandAvailable(od.document, LayerSetCommand::LinkLayers, sel({0})) &&
              layerSetCommandAvailable(od.document, LayerSetCommand::LinkLayers, sel({0, 1})),
          "availability: Link needs two layers, by definition");
    check(!layerSetCommandAvailable(od.document, LayerSetCommand::ClipLayers, sel({0, 1})) &&
              layerSetCommandAvailable(od.document, LayerSetCommand::ClipLayers, sel({1, 2})),
          "availability: Clip is unavailable when the bottom layer is in the set");
    const LayerSetOpResult none =
        applyLayerSetOp(od.document, LayerSetCommand::DeleteLayers, LayerSelection{});
    check(!none.ok && contains(none.error, "no layers are selected"),
          "and an empty selection refuses with a sentence rather than doing nothing");

    // The selection type's own invariant.
    check(sel({3, 1, 1, 3, 0}).indices == std::vector<size_t>({0, 1, 3}),
          "makeLayerSelection sorts and de-duplicates, so a double shift-click cannot delete a "
          "layer twice");
    check(nextLinkGroupId(od.document) == 1,
          "nextLinkGroupId is one above the highest present -- safe here, unlike for Layer::id, "
          "because nothing outside Document::layers ever names a link group");
  }

  // ======================================================================
  // Part I: the two value-carrying set setters
  // ======================================================================
  {
    OpenDocument od = makeFive();
    const uint64_t rev = od.revision;
    const LayerSetEditResult op = applyLayerSetOpacity(od, sel({0, 2, 4}), 0.4f);
    check(op.ok && od.document.layers[0].opacity == 0.4f &&
              od.document.layers[1].opacity == 1.0f && od.document.layers[4].opacity == 0.4f,
          "set opacity over a set: exactly the members, and nothing else");
    check(od.revision == rev + 1, "set opacity: one revision bump for three layers");
    setLayerLocked(od.document, 2, true);
    const LayerSetEditResult refused = applyLayerSetOpacity(od, sel({0, 2}), 0.9f);
    check(!refused.ok && od.document.layers[0].opacity == 0.4f,
          "set opacity with a locked member: refused whole -- layer 0 keeps its old value");
    const LayerSetEditResult blend = applyLayerSetBlend(od, sel({3, 4}), BlendMode::Multiply);
    check(blend.ok && od.document.layers[3].blend == "multiply" &&
              od.document.layers[4].blend == "multiply",
          "set blend over a set, through core/LayerOps' own typed setter");
  }

  // ======================================================================
  // Part J: persistence -- a label that does not survive a reopen is a lie
  // ======================================================================
  {
    const char* kBare = "np_multiselect_bare.npaint";
    const char* kMarked = "np_multiselect_marked.npaint";
    const char* kCleared = "np_multiselect_cleared.npaint";
    for (const char* p : {kBare, kMarked, kCleared}) std::remove(p);

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
    doc.layers[0].name = "Base";
    writeRgb(doc, 0, 3, 4, {0.5f, 0.25f, 0.125f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("Over"));
    writeRgb(doc, 1, 3, 4, {0.0f, 0.0f, 0.5f, 0.5f});
    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok, "npaint: the unlabelled, unlinked fixture saves");

    Document marked = doc;
    applyLayerSetOp(marked, LayerSetCommand::LabelViolet, makeLayerSelection({0}));
    applyLayerSetOp(marked, LayerSetCommand::LinkLayers, makeLayerSelection({0, 1}));
    marked.layers[1].colorLabel = "teal";  // a name this build has no swatch for

    {
      const NpaintSaveResult saved = saveNpaint(marked, kMarked);
      check(saved.ok, "npaint: a document with a label and a link saves");
      const NpaintLoadResult back = loadNpaint(kMarked);
      check(back.ok && back.document.layers.size() == 2, "npaint: and it loads back clean");
      if (back.ok && back.document.layers.size() == 2) {
        check(back.document.layers[0].colorLabel == "violet" &&
                  back.document.layers[1].colorLabel == "teal",
              "npaint: the colour label ROUND-TRIPS, including a name this build has no swatch "
              "for -- a label that vanished on reopen would be a lie the moment the file opened");
        check(back.document.layers[0].linkGroup == marked.layers[0].linkGroup &&
                  back.document.layers[1].linkGroup == marked.layers[1].linkGroup &&
                  layerIsLinked(back.document, 0),
              "npaint: and so does the link, still resolving to a live pair after the reload");
      }

      // The regression boundary: a document with neither writes the bytes it
      // wrote before either attribute existed.
      Document cleared = back.ok ? back.document : marked;
      for (Layer& l : cleared.layers) {
        l.colorLabel.clear();
        l.linkGroup = 0;
      }
      const NpaintSaveResult again = saveNpaint(cleared, kCleared);
      const std::vector<unsigned char> bareBytes = bytesWithoutCapDate(kBare);
      const std::vector<unsigned char> clearedBytes = bytesWithoutCapDate(kCleared);
      const std::vector<unsigned char> markedBytes = bytesWithoutCapDate(kMarked);
      check(again.ok && !bareBytes.empty() && bareBytes == clearedBytes,
            "npaint: clearing every label and link gives back a file BYTE-IDENTICAL to one "
            "written before either attribute existed -- both are written only when set");
      check(markedBytes.size() > bareBytes.size() && markedBytes != bareBytes,
            "npaint: and the marked file really is bigger, so the comparator above is not "
            "passing because nothing was ever written");
      std::printf("  npaint: no labels/links %zu bytes; one label, one unknown label and a "
                  "two-member link %zu bytes (+%zu)\n",
                  bareBytes.size(), markedBytes.size(), markedBytes.size() - bareBytes.size());
    }

    // `np:link` is an int, so a group number this build could only truncate is
    // refused by name rather than written wrong. Unreachable through the
    // gestures -- `nextLinkGroupId()` counts up from 1 -- but `Layer` is a plain
    // aggregate, so the guard is asserted rather than assumed unreachable.
    Document huge = doc;
    huge.layers[0].linkGroup = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1;
    // The guard sits in `saveNpaint()`'s validation pass, which runs *before*
    // the NP_USE_OIIO gate -- so this is one of the assertions whose answer is
    // identical in both configurations, and the sentence is checked in both.
    const NpaintSaveResult tooBig = saveNpaint(huge, kMarked);
    check(!tooBig.ok && contains(tooBig.error, "np:link") &&
              contains(tooBig.error, "2147483647"),
          "npaint: a link group that does not fit np:link's int is REFUSED by name with the "
          "limit, never truncated -- truncation would silently link two layers that never were");
    for (const char* p : {kBare, kMarked, kCleared}) std::remove(p);
  }

  std::printf("[selftest] layer multi-select %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
