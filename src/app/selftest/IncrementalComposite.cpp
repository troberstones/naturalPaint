#include "app/selftest/Support.hpp"

namespace np {

bool runIncrementalCompositeTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Why almost every assertion here is `memcmp` and not a tolerance -----
  //
  // The claim this section exists to make is **bit-identity**, not agreement:
  // an incremental composite has to produce the same half words a full
  // recomposite of the same document would, because anything else is a
  // rectangle of the canvas that is subtly wrong and that nothing will ever
  // repair. So the comparisons are `std::memcmp` over the raw storage, and
  // the timing lines are the only places a number is allowed to vary.
  //
  // Bit-identity is *achievable* rather than aspirational because
  // core/Composite has one walk with a tile filter and not two
  // implementations (core/Composite.hpp's region section derives it): the
  // accumulator is per texel and nothing reads a neighbour, so restricting
  // the tile set removes whole texels from the output and never a term from
  // any texel's arithmetic.

  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto writeMask = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float v) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].mask->getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), v);
  };
  auto writePigment = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                         const PigmentTexel& texel) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].pigmentTiles->getOrCreate(tileCoordAt(at))
        .writeTexel(tileLocalOffset(at), texel);
  };
  auto exposureOp = [](float stops) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = stops;
    return op;
  };
  auto sameHalves = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(uint16_t)) == 0;
  };
  auto sameFloats = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };

  std::printf("  -- 1. how a change is localised: copy-on-write slot identity --\n");

  // -----------------------------------------------------------------------
  // 1. Localisation
  // -----------------------------------------------------------------------
  {
    Document doc = Document::createBlank(512, 512, WorkingSpace{});
    writeRgb(doc, 0, 10, 10, {0.5f, 0.25f, 0.125f, 1.0f});

    // The snapshot. A plain copy IS the share (core/TileStore.hpp), so this
    // one line is the whole mechanism.
    const Document snap = doc;
    const TileCoord origin{0, 0};
    check(snap.layers[0].rgbTiles->find(origin) == doc.layers[0].rgbTiles->find(origin),
          "identity: a Document copy shares the tile -- one slot, one address");
    check(doc.layers[0].rgbTiles->tileUseCount(origin) == 2,
          "identity: which is exactly what puts the tile's use count at 2");

    // The write that must move the address. It is a *different* texel of the
    // *same* tile, so nothing but the barrier can make this detectable.
    writeRgb(doc, 0, 20, 20, {0.25f, 0.25f, 0.25f, 1.0f});
    check(snap.layers[0].rgbTiles->find(origin) != doc.layers[0].rgbTiles->find(origin),
          "identity: the barrier COPIED, so the address moved -- the whole proof");

    const DocumentDirtyTiles one = documentDirtyTiles(snap, doc);
    check(!one.everything && one.tiles.size() == 1 && one.tiles[0] == origin,
          "localise: one texel written -> exactly one dirty tile");

    // A tile that did not exist at snapshot time is caught by presence, not
    // by address -- the other half of the completeness argument.
    writeRgb(doc, 0, 300, 300, {1.0f, 1.0f, 1.0f, 1.0f});
    const DocumentDirtyTiles two = documentDirtyTiles(snap, doc);
    check(!two.everything && two.tiles.size() == 2 && two.tiles[0] == TileCoord{0, 0} &&
              two.tiles[1] == TileCoord{2, 2},
          "localise: a tile created after the snapshot is caught by presence");

    // Every store, not just the RGB one.
    Document masked = Document::createBlank(512, 512, WorkingSpace{});
    writeRgb(masked, 0, 10, 10, {1.0f, 1.0f, 1.0f, 1.0f});
    addLayerMask(masked, 0);
    const Document maskSnap = masked;
    writeMask(masked, 0, 400, 20, 0.5f);
    const DocumentDirtyTiles maskDirty = documentDirtyTiles(maskSnap, masked);
    check(!maskDirty.everything && maskDirty.tiles.size() == 1 &&
              maskDirty.tiles[0] == TileCoord{3, 0},
          "localise: a mask tile is diffed by the same rule as a colour tile");

    Document pig = Document::createBlank(512, 512, WorkingSpace{});
    addLayer(pig, pig.layers.size(), makePigmentLayer("p"));
    PigmentTexel t;
    t.latent.c = {0.25f, 0.5f, 0.125f};
    t.mass = 0.5f;
    writePigment(pig, 1, 260, 8, t);
    const Document pigSnap = pig;
    t.mass = 0.75f;
    writePigment(pig, 1, 264, 8, t);
    const DocumentDirtyTiles pigDirty = documentDirtyTiles(pigSnap, pig);
    check(!pigDirty.everything && pigDirty.tiles.size() == 1 &&
              pigDirty.tiles[0] == TileCoord{2, 0},
          "localise: and so is a pigment tile -- all three stores, one rule");
  }

  // -----------------------------------------------------------------------
  // 1b. What makes it complete, and the one thing that would defeat it
  // -----------------------------------------------------------------------
  //
  // core/DirtyTiles.hpp §2 proves completeness from the fact that the
  // snapshot is an owner. Both halves are demonstrated here: without an owner
  // the barrier writes in place and the address does *not* move, and a write
  // handle taken before the snapshot bypasses the barrier entirely.
  {
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    writeRgb(doc, 0, 5, 5, {1.0f, 0.0f, 0.0f, 1.0f});
    const Tile* before = doc.layers[0].rgbTiles->find(TileCoord{0, 0});
    writeRgb(doc, 0, 6, 6, {0.0f, 1.0f, 0.0f, 1.0f});
    check(doc.layers[0].rgbTiles->find(TileCoord{0, 0}) == before,
          "complete: with NO snapshot the barrier writes in place, address kept");
    check(doc.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 1,
          "complete: so holding the snapshot is what forces the copy, not luck");
  }
  {
    // **The one leak, demonstrated rather than described.** Take the write
    // handle, THEN copy, THEN write through it: the write lands in the tile
    // the copy shares, so the snapshot moves with the document and no address
    // changes. core/TileStore.hpp already states this caller rule; what is new
    // is that a missed dirty tile is now its consequence.
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    Tile& handle = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    handle.writePixel(PixelCoord{5, 5}, {1.0f, 0.0f, 0.0f, 1.0f});
    const Document snap = doc;
    const std::vector<uint16_t> uploaded = compositeDocumentStraightHalf(doc);

    handle.writePixel(PixelCoord{9, 9}, {0.0f, 0.0f, 1.0f, 1.0f});
    const DocumentDirtyTiles missed = documentDirtyTiles(snap, doc);
    check(missed.empty(), "leak: a write through a handle taken BEFORE the copy is invisible");
    check(!sameHalves(uploaded, compositeDocumentStraightHalf(doc)),
          "leak: while the picture really did change -- a texel nothing repairs");

    // The supported ordering, which is the only one this build produces:
    // ui/DocumentTexture takes the snapshot itself, after the composite, and
    // every writer in core/TileStore.hpp's enumeration holds its reference for
    // one texel or one tile fill.
    const Document snap2 = doc;
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{11, 11},
                                                                   {0.0f, 1.0f, 0.0f, 1.0f});
    check(documentDirtyTiles(snap2, doc).tiles.size() == 1,
          "leak: a handle taken AFTER it is seen -- which is every real path");
    std::printf("    the leak is core/TileStore.hpp's own caller rule (\"take the reference, "
                "write, then copy -- never the other order\"); ui/DocumentTexture closes it by "
                "taking the snapshot itself, inside viewFor(), after the composite\n");
  }

  std::printf("  -- 2. what is NOT tile-local, named one reason at a time --\n");

  // -----------------------------------------------------------------------
  // 2. Classification
  // -----------------------------------------------------------------------
  {
    auto baseDocument = [&]() {
      Document doc = Document::createBlank(512, 512, WorkingSpace{});
      writeRgb(doc, 0, 10, 10, {0.5f, 0.25f, 0.125f, 1.0f});
      addLayer(doc, doc.layers.size(), makeRgbLayer("upper"));
      writeRgb(doc, 1, 300, 300, {0.25f, 0.5f, 0.75f, 1.0f});
      return doc;
    };
    auto reasonAfter = [&](auto&& mutate) {
      Document doc = baseDocument();
      const Document snap = doc;
      mutate(doc);
      const DocumentDirtyTiles d = documentDirtyTiles(snap, doc);
      return d.everything ? d.reason : FullRecompositeReason::None;
    };
    auto dirtyAfter = [&](auto&& mutate) {
      Document doc = baseDocument();
      const Document snap = doc;
      mutate(doc);
      return documentDirtyTiles(snap, doc);
    };
    // Proved by patching, not asserted: take the old composite, recomposite
    // only the reported dirty tiles into it, and compare against a full
    // recomposite of `after`. Used throughout this section and by §3's own
    // reorder fixture below -- one implementation, so a change to this
    // section's story cannot leave it inconsistent with the one it was copied
    // from.
    auto patchedComposite = [&](const Document& before, const Document& after) {
      std::vector<float> buffer = compositeDocumentPremultiplied(before);
      const DocumentDirtyTiles d = documentDirtyTiles(before, after);
      CompositeRegion region;
      region.pixels = buffer.data();
      region.origin = PixelCoord{0, 0};
      region.width = after.width;
      region.height = after.height;
      compositeDocumentTilesPremultiplied(after, d.tiles, region, nullptr);
      return buffer;
    };

    // **Narrowed, not full**, for a layer that holds pixels and is not an
    // Adjustment layer: core/DirtyTiles.hpp §4's "no op reads a neighbour"
    // means visible/opacity/blend can only move texels inside that layer's
    // own tile footprint, and clipped only widens that to its clip base's too
    // (neither applies here -- layer 1 has no base). "upper" holds exactly one
    // painted texel at (300, 300), tile (2, 2), so that is the whole dirty set.
    const TileCoord upperTile{2, 2};
    {
      const DocumentDirtyTiles d = dirtyAfter([&](Document& d) { setLayerVisible(d, 1, false); });
      check(!d.everything && d.tiles.size() == 1 && d.tiles[0] == upperTile,
            "classify: visibility narrows to the layer's own tile, not full");
    }
    {
      const DocumentDirtyTiles d = dirtyAfter([&](Document& d) { setLayerOpacity(d, 1, 0.5f); });
      check(!d.everything && d.tiles.size() == 1 && d.tiles[0] == upperTile,
            "classify: opacity narrows to the layer's own tile, not full");
    }
    {
      const DocumentDirtyTiles d =
          dirtyAfter([&](Document& d) { setLayerBlend(d, 1, BlendMode::Multiply); });
      check(!d.everything && d.tiles.size() == 1 && d.tiles[0] == upperTile,
            "classify: a blend mode narrows to the layer's own tile, not full");
    }
    {
      // Clipped narrows to the layer's own tile UNION its clip base's --
      // here that is layer 0, whose own paint is at (10, 10), tile (0, 0).
      const DocumentDirtyTiles d = dirtyAfter([&](Document& d) { setLayerClipped(d, 1, true); });
      check(!d.everything && d.tiles.size() == 2 && d.tiles[0] == TileCoord{0, 0} &&
                d.tiles[1] == upperTile,
            "classify: a clip narrows to the layer's tile UNION its base's, not full");
    }

    // **The Adjustment-layer exception, still full.** Its op stack "reaches
    // everything below it" (§3), which is not its own tile footprint -- it
    // has none -- so none of the four narrowings above apply to it.
    {
      Document doc = baseDocument();
      addLayer(doc, doc.layers.size(), makeAdjustmentLayer("exposure"));
      const size_t adj = doc.layers.size() - 1;
      const Document snap = doc;
      setLayerVisible(doc, adj, false);
      const DocumentDirtyTiles d = documentDirtyTiles(snap, doc);
      check(d.everything && d.reason == FullRecompositeReason::LayerVisibilityChanged &&
                d.layerIndex == adj,
            "classify: an Adjustment layer's visibility stays full -- its stack has no tile of "
            "its own");
    }
    check(reasonAfter([&](Document& d) { d.layers[1].ops.add(exposureOp(1.0f)); }) ==
              FullRecompositeReason::LayerOpsChanged,
          "classify: an op stack -> full (an adjustment's reaches everything below)");
    check(reasonAfter([&](Document& d) { addLayerMask(d, 1); }) ==
              FullRecompositeReason::LayerMaskPresenceChanged,
          "classify: gaining a mask is not the same as editing one -> full");
    check(reasonAfter([&](Document& d) {
            addLayer(d, d.layers.size(), makeRgbLayer("third"));
          }) == FullRecompositeReason::LayerCountChanged,
          "classify: an added layer shifts every index -> full");
    check(reasonAfter([&](Document& d) { removeLayer(d, 1); }) ==
              FullRecompositeReason::LayerCountChanged,
          "classify: and so does a removed one -> full");
    check(reasonAfter([&](Document& d) { d.layers[1].kind = LayerKind::Pigment; }) ==
              FullRecompositeReason::LayerKindChanged,
          "classify: a kind change selects a different projection -> full");
    check(reasonAfter([&](Document& d) { d.width = 256; }) ==
              FullRecompositeReason::CanvasSizeChanged,
          "classify: a canvas resize decides the texture, not just its contents");
    check(reasonAfter([&](Document& d) { d.workingSpace.primaries.redX = 0.7f; }) ==
              FullRecompositeReason::WorkingSpaceChanged,
          "classify: a working space -> full, though nothing reads it today");
    // **No longer automatically full.** Before this narrowing landed, ANY
    // per-index property difference forced `whole()`, so a reorder that also
    // changes a property was indistinguishable from one that does not narrow
    // at all. Now it is exactly as narrow as the sum of its parts: layer 1's
    // blend changed (narrows to its own tile) and the reorder itself moved
    // storage identity at both indices (caught by the ordinary tile diff --
    // §3's very next fixture demonstrates that half on its own), so the union
    // is still tile-local, and proved bit-identical rather than merely sized.
    {
      Document doc = baseDocument();
      const Document snap = doc;
      setLayerBlend(doc, 1, BlendMode::Multiply);
      moveLayer(doc, 1, 0);
      const DocumentDirtyTiles d = documentDirtyTiles(snap, doc);
      check(!d.everything && !d.tiles.empty() && d.tiles.size() < canvasTileCount(doc),
            "classify: a reorder plus a property change is STILL tile-local, not full");
      check(sameFloats(patchedComposite(snap, doc), compositeDocumentPremultiplied(doc)),
            "classify: and patching only those tiles equals a full recomposite");
    }

    // The members the compositor provably never reads. Each is an *empty*
    // edit: the revision moves and nothing is recomposited at all.
    Document renamed = baseDocument();
    const Document renameSnap = renamed;
    setLayerName(renamed, 1, "a new name");
    setLayerLocked(renamed, 1, true);
    renamed.layers[1].parent = "L0001";
    check(documentDirtyTiles(renameSnap, renamed).empty(),
          "classify: name, locked and parent are NOT read -> zero dirty tiles");
    check(sameHalves(compositeDocumentStraightHalf(renameSnap),
                     compositeDocumentStraightHalf(renamed)),
          "classify: and the composite really is bit-identical across all three");

    // **The one caveat to "not compared".** No texel depends on `name`, but
    // the compositor puts it in a warning sentence, so an empty edit still has
    // to produce warnings -- with the *new* name. That is why
    // ui/DocumentTexture asks for them with an empty tile set rather than
    // skipping the walk outright, and it is the reason the exemption is
    // "no texel reads it" and not "nothing reads it".
    Document warned = baseDocument();
    warned.layers[1].blend = "dissolve";
    std::vector<std::string> beforeRename, afterRename;
    compositeDocumentTilesPremultiplied(warned, {}, CompositeRegion{}, &beforeRename);
    setLayerName(warned, 1, "a renamed layer");
    compositeDocumentTilesPremultiplied(warned, {}, CompositeRegion{}, &afterRename);
    check(!afterRename.empty() && afterRename != beforeRename &&
              afterRename[0].find("a renamed layer") != std::string::npos,
          "classify: a rename moves no texel but DOES move a warning sentence");

    // **The reorder that IS tile-local**, and the one a reader will doubt.
    // Two layers agreeing on every compared property are interchangeable
    // except for their tiles, so the per-index tile diff reports exactly the
    // coordinates where the two stacks differ. Proved by patching (the
    // `patchedComposite` defined above), not merely asserted.
    Document swapped = baseDocument();
    const Document swapSnap = swapped;
    moveLayer(swapped, 1, 0);
    const DocumentDirtyTiles swapDirty = documentDirtyTiles(swapSnap, swapped);
    check(!swapDirty.everything && swapDirty.tiles.size() == 2,
          "classify: a reorder of interchangeable layers IS tile-local -- 2 tiles");
    check(sameFloats(patchedComposite(swapSnap, swapped),
                     compositeDocumentPremultiplied(swapped)),
          "classify: and patching only those two tiles equals a full recomposite");

    // The conservative direction, as a check rather than a comment: the
    // reason names the layer, so a slow frame can say why it was slow. An
    // opacity change on an Adjustment layer is the fixture now, rather than
    // an ordinary RGB one -- the RGB case above narrows, and this is exactly
    // the case that still does not: an Adjustment layer's op stack reaches
    // everything below it, which no per-layer tile footprint can express.
    Document faded = baseDocument();
    addLayer(faded, faded.layers.size(), makeAdjustmentLayer("exposure"));
    const size_t fadedAdj = faded.layers.size() - 1;
    faded.layers[fadedAdj].ops.add(exposureOp(1.0f));
    const Document fadedSnap = faded;
    setLayerOpacity(faded, fadedAdj, 0.25f);
    const DocumentDirtyTiles fadedDirty = documentDirtyTiles(fadedSnap, faded);
    check(fadedDirty.everything && fadedDirty.layerIndex == fadedAdj &&
              fadedDirty.reason == FullRecompositeReason::LayerOpacityChanged &&
              fullRecompositeExplanation(fadedDirty.reason, fadedDirty.layerIndex)
                      .find("layer " + std::to_string(fadedAdj)) != std::string::npos,
          "classify: the reason names the layer that forced the full path");
    std::printf("    %s\n",
                fullRecompositeExplanation(fadedDirty.reason, fadedDirty.layerIndex).c_str());

    // -----------------------------------------------------------------------
    // 2b. Bit-identity of the narrowing, on OVERLAPPING layers, plus sabotage
    // -----------------------------------------------------------------------
    //
    // The classify checks above prove the *tile set*; they do not prove the
    // *pixels*, and a single-layer-per-tile fixture like `baseDocument()`
    // could not catch a dirty set that under-reports by one tile -- there is
    // nothing else at that coordinate for a missing tile to leave stale. This
    // fixture overlaps two RGB layers on purpose, so a dropped tile of the
    // upper layer leaves the LOWER layer's old, un-blended pixel showing
    // through where the two actually overlap.
    std::printf("  -- 2b. bit-identity of the narrowing, on overlapping layers --\n");
    auto overlapDocument = [&]() {
      Document doc = Document::createBlank(384, 384, WorkingSpace{});
      for (int32_t y = 0; y < 320; ++y)
        for (int32_t x = 0; x < 320; ++x) writeRgb(doc, 0, x, y, {0.6f, 0.3f, 0.1f, 0.8f});
      addLayer(doc, doc.layers.size(), makeRgbLayer("upper"));
      for (int32_t y = 64; y < 384; ++y)
        for (int32_t x = 64; x < 384; ++x) writeRgb(doc, 1, x, y, {0.15f, 0.55f, 0.85f, 0.5f});
      return doc;
    };

    auto proveBitIdentical = [&](auto&& mutate, const char* what) {
      Document doc = overlapDocument();
      const Document snap = doc;
      mutate(doc);
      check(sameFloats(patchedComposite(snap, doc), compositeDocumentPremultiplied(doc)), what);
    };

    proveBitIdentical(
        [&](Document& d) { setLayerVisible(d, 1, false); },
        "bit-identity: a visibility toggle, patched narrow, matches a full recomposite");
    proveBitIdentical(
        [&](Document& d) { setLayerOpacity(d, 1, 0.4f); },
        "bit-identity: an opacity change, patched narrow, matches a full recomposite");
    proveBitIdentical(
        [&](Document& d) { setLayerBlend(d, 1, BlendMode::Multiply); },
        "bit-identity: a blend change, patched narrow, matches a full recomposite");

    // --- The clip case, and its base -----------------------------------
    //
    // Toggling layer 1's clipped flag changes what layer 0 (its base) itself
    // renders wherever the two overlap, so the proof needs the base's own
    // tiles in the patch too, not just the member's.
    proveBitIdentical(
        [&](Document& d) { setLayerClipped(d, 1, true); },
        "bit-identity: clipping ON, patched narrow (member UNION base), matches a full "
        "recomposite");
    {
      Document doc = overlapDocument();
      setLayerClipped(doc, 1, true);
      const Document snap = doc;
      setLayerClipped(doc, 1, false);
      check(sameFloats(patchedComposite(snap, doc), compositeDocumentPremultiplied(doc)),
            "bit-identity: clipping OFF, patched narrow (member UNION base), matches a full "
            "recomposite");
    }

    // The base's own rendered pixel really does move when the flag flips
    // (within the member's own footprint here -- `overlapDocument()`'s two
    // layers happen to share the same tile span, so this fixture alone does
    // not distinguish "the base's tiles are necessary" from "they are a
    // redundant-but-harmless superset of the member's own"; sabotage (b)
    // below is the one that needs the base's footprint to genuinely differ
    // from the member's to say which).
    {
      Document unclipped = overlapDocument();
      Document clipped = unclipped;
      setLayerClipped(clipped, 1, true);
      check(!sameFloats(compositeDocumentPremultiplied(unclipped),
                        compositeDocumentPremultiplied(clipped)),
            "bit-identity: clipping really does change the base's own rendered pixels");
    }

    // --- Sabotage: an under-reported dirty set really does go wrong --------
    //
    // This codebase's established pattern for a correctness-critical check
    // (app/selftest/TransformSession.cpp's "SABOTAGE PROOF" sections,
    // app/selftest/TransformPreviewTexture.cpp's): construct the WRONG
    // computation explicitly, beside the right one, and prove it disagrees --
    // never just trust that a broken version would have been caught. Two ways
    // an implementation could under-report, one at a time.
    auto patchWith = [&](const Document& before, const Document& after,
                         const std::vector<TileCoord>& tiles) {
      std::vector<float> buffer = compositeDocumentPremultiplied(before);
      CompositeRegion region;
      region.pixels = buffer.data();
      region.origin = PixelCoord{0, 0};
      region.width = after.width;
      region.height = after.height;
      compositeDocumentTilesPremultiplied(after, tiles, region, nullptr);
      return buffer;
    };
    {
      // (a) drop one tile of the correct set -- e.g. an implementation that
      // only unioned the layer's AFTER tiles and not its BEFORE ones too.
      Document doc = overlapDocument();
      const Document snap = doc;
      setLayerOpacity(doc, 1, 0.4f);
      const DocumentDirtyTiles correct = documentDirtyTiles(snap, doc);
      check(!correct.everything && correct.tiles.size() > 1,
            "sabotage: the fixture has more than one dirty tile, so one can be dropped");
      const std::vector<TileCoord> oneShort(correct.tiles.begin(), correct.tiles.end() - 1);
      check(!sameFloats(patchWith(snap, doc, oneShort), compositeDocumentPremultiplied(doc)),
            "sabotage: an under-reported tile set (one short) is caught -- the bit-identity "
            "check above has teeth");
    }
    {
      // (b) drop the MEMBER's own tiles, keeping only the base's -- a fixture
      // where the member's footprint genuinely extends past the base's, so
      // some of its tiles are NEVER visited by the base's own walk (§17: "a
      // clipped layer's own tiles are never visited for their own sake").
      // Unclipped, the member composites independently over its WHOLE
      // footprint; clipped, it is invisible outside the base's -- so at those
      // tiles the member goes from painted to gone, and a patch that only
      // carries the base's tiles never touches them at all.
      Document doc = Document::createBlank(384, 384, WorkingSpace{});
      for (int32_t y = 0; y < 256; ++y)
        for (int32_t x = 0; x < 256; ++x) writeRgb(doc, 0, x, y, {0.6f, 0.3f, 0.1f, 0.8f});
      addLayer(doc, doc.layers.size(), makeRgbLayer("member"));
      for (int32_t y = 0; y < 384; ++y)
        for (int32_t x = 0; x < 384; ++x) writeRgb(doc, 1, x, y, {0.15f, 0.55f, 0.85f, 0.5f});
      const Document snap = doc;
      setLayerClipped(doc, 1, true);

      std::vector<TileCoord> baseOnly;
      if (doc.layers[0].rgbTiles)
        for (const auto& [coord, tile] : *doc.layers[0].rgbTiles) {
          (void)tile;
          baseOnly.push_back(coord);
        }
      check(baseOnly.size() < canvasTileCount(doc),
            "sabotage: the base's own footprint really is smaller than the member's");
      check(!sameFloats(patchWith(snap, doc, baseOnly), compositeDocumentPremultiplied(doc)),
            "sabotage: dropping the member's own tiles (keeping only the base's) is caught too");
    }

    // --- The clip TOPOLOGY ripple, proven as a classification too ----------
    //
    // core/Composite.hpp §12's `base` is a running variable that advances
    // only at a non-clipped layer, so un-clipping the MIDDLE of a run of
    // clipped layers re-parents every layer ABOVE it in that run to a new
    // base -- a layer that never appears in the before/after property diff at
    // all. `documentDirtyTiles()` answers this with a full recomposite
    // (pass 3a) rather than trying to compute the union precisely; proved
    // here as a classification, the same style as §2's own checks.
    {
      Document doc = Document::createBlank(384, 384, WorkingSpace{});
      for (int32_t y = 0; y < 128; ++y)
        for (int32_t x = 0; x < 384; ++x) writeRgb(doc, 0, x, y, {0.5f, 0.3f, 0.2f, 0.9f});
      addLayer(doc, doc.layers.size(), makeRgbLayer("mid"));
      for (int32_t y = 0; y < 128; ++y)
        for (int32_t x = 0; x < 384; ++x) writeRgb(doc, 1, x, y, {0.1f, 0.6f, 0.2f, 0.5f});
      setLayerClipped(doc, 1, true);
      addLayer(doc, doc.layers.size(), makeRgbLayer("top"));
      for (int32_t y = 0; y < 384; ++y)
        for (int32_t x = 0; x < 384; ++x) writeRgb(doc, 2, x, y, {0.2f, 0.2f, 0.8f, 0.7f});
      setLayerClipped(doc, 2, true);

      const Document snap = doc;
      setLayerClipped(doc, 1, false);  // un-clip "mid": "top" re-parents to it
      const DocumentDirtyTiles ripple = documentDirtyTiles(snap, doc);
      check(ripple.everything && ripple.reason == FullRecompositeReason::LayerClipChanged,
            "classify: un-clipping the middle of a run re-parents the layer above it -> full");
    }
  }

  std::printf("  -- 3. the region walk, tile by tile, against the full one --\n");

  // -----------------------------------------------------------------------
  // 3. Bit-identity of the region walk itself
  // -----------------------------------------------------------------------
  //
  // One document carrying every feature the walk has: a mixed Pigment pair, a
  // masked and faded RGB layer, a clipped layer over it, an adjustment layer
  // with its own mask and opacity, and a layer whose blend this build cannot
  // honour (so the walk produces warnings). Composited once whole, then once
  // **one tile at a time**, and the two buffers compared at zero tolerance.
  {
    Document doc = Document::createBlank(384, 256, WorkingSpace{});
    doc.layers.clear();

    addLayer(doc, 0, makePigmentLayer("lower pigment"));
    addLayer(doc, 1, makePigmentLayer("mixing pigment"));
    for (int32_t y = 0; y < 256; y += 5) {
      for (int32_t x = 0; x < 384; x += 5) {
        PigmentTexel low;
        low.latent.c = {0.75f, 0.5f, 0.25f};
        low.mass = 1.0f;
        writePigment(doc, 0, x, y, low);
        PigmentTexel up;
        up.latent.c = {0.125f, 0.25f, 0.875f};
        up.mass = 0.5f;
        if (((x + y) & 1) == 0) writePigment(doc, 1, x, y, up);
      }
    }
    setLayerBlend(doc, 1, BlendMode::Mix);
    check(doc.layers[1].blend != kDefaultBlendName, "region: the fixture's Mix pair really formed");

    addLayer(doc, 2, makeRgbLayer("masked and faded"));
    addLayerMask(doc, 2);
    for (int32_t y = 32; y < 224; ++y) {
      for (int32_t x = 32; x < 352; ++x) {
        writeRgb(doc, 2, x, y, {0.5f, 0.25f, 0.75f, 1.0f});
        writeMask(doc, 2, x, y, static_cast<float>(x % 128) / 128.0f);
      }
    }
    setLayerOpacity(doc, 2, 0.75f);

    addLayer(doc, 3, makeRgbLayer("clipped"));
    for (int32_t y = 0; y < 256; ++y)
      for (int32_t x = 0; x < 384; ++x) writeRgb(doc, 3, x, y, {0.0f, 0.25f, 0.0f, 0.25f});
    setLayerClipped(doc, 3, true);

    addLayer(doc, 4, makeAdjustmentLayer("exposure"));
    doc.layers[4].ops.add(exposureOp(0.5f));
    addLayerMask(doc, 4);
    for (int32_t y = 0; y < 256; ++y)
      for (int32_t x = 0; x < 384; ++x)
        writeMask(doc, 4, x, y, static_cast<float>(y % 64) / 64.0f);
    setLayerOpacity(doc, 4, 0.5f);

    addLayer(doc, 5, makeRgbLayer("a blend this build cannot honour"));
    writeRgb(doc, 5, 5, 5, {0.5f, 0.5f, 0.5f, 1.0f});
    doc.layers[5].blend = "dissolve";

    std::vector<std::string> fullWarnings;
    const std::vector<float> full = compositeDocumentPremultiplied(doc, &fullWarnings);
    check(!fullWarnings.empty(), "region: the fixture really does produce warnings");

    // Every canvas tile, composited on its own, into the same buffer shape.
    std::vector<float> assembled(full.size(), 0.0f);
    const std::vector<TileCoord> all = canvasTiles(doc);
    check(all.size() == canvasTileCount(doc) && all.size() == 3u * 2u,
          "region: a 384x256 canvas is 3x2 tiles, and both spellings agree");
    for (const TileCoord& coord : all) {
      CompositeRegion region;
      region.pixels = assembled.data();
      region.origin = PixelCoord{0, 0};
      region.width = doc.width;
      region.height = doc.height;
      compositeDocumentTilesPremultiplied(doc, {coord}, region, nullptr);
    }
    check(sameFloats(assembled, full),
          "region: tile-by-tile reassembles the full composite, BIT-EXACTLY");

    // The same, into a rectangle that is *not* the canvas -- a one-tile buffer
    // at that tile's own origin, which is the shape the incremental path uses.
    const TileCoord probe{1, 1};
    std::vector<float> oneTile(static_cast<size_t>(kTileSize) * kTileSize * 4, 0.0f);
    CompositeRegion small;
    small.pixels = oneTile.data();
    small.origin = tileOrigin(probe);
    small.width = kTileSize;
    small.height = kTileSize;
    compositeDocumentTilesPremultiplied(doc, {probe}, small, nullptr);
    bool offsetMatches = true;
    for (int32_t ty = 0; ty < kTileSize && offsetMatches; ++ty) {
      const size_t fullRow =
          (static_cast<size_t>(small.origin.y + ty) * static_cast<size_t>(doc.width) +
           static_cast<size_t>(small.origin.x)) *
          4u;
      const size_t tileRow = static_cast<size_t>(ty) * static_cast<size_t>(kTileSize) * 4u;
      offsetMatches = std::memcmp(&full[fullRow], &oneTile[tileRow],
                                  static_cast<size_t>(kTileSize) * 4u * sizeof(float)) == 0;
    }
    check(offsetMatches, "region: and into a buffer at the tile's own origin, too");

    // Warnings are a property of the document, not of which tiles were
    // visited. A cheap frame must not stop saying the composite is
    // approximate.
    std::vector<std::string> regionWarnings;
    compositeDocumentTilesPremultiplied(doc, {}, CompositeRegion{}, &regionWarnings);
    check(regionWarnings == fullWarnings,
          "region: an empty tile set still reports every warning a full walk does");
  }

  std::printf("  -- 4. ten edits end to end, each bit-identical to a full one --\n");

  // -----------------------------------------------------------------------
  // 4. The whole pipeline, over a sequence of edits
  // -----------------------------------------------------------------------
  //
  // One DocumentTexture, one document, a sequence of edits -- which is the
  // shape the running application has, and the shape that would expose a
  // dirty set complete for one edit and not for two in a row. After every
  // edit the object's uploaded half words are compared against a fresh full
  // composite of the same document at zero tolerance.
  {
    OpenDocument od = makeBlankOpenDocument(512, 512, WorkingSpace{}, "incremental");
    DocumentTexture dt;
    bool everyStepIdentical = true;

    auto step = [&](const char* label) {
      dt.viewFor(gpu, od);
      const std::vector<uint16_t> expected = compositeDocumentStraightHalf(od.document);
      const bool identical = sameHalves(dt.uploadedHalves(), expected);
      if (!identical) {
        everyStepIdentical = false;
        std::printf("    MISMATCH after \"%s\"\n", label);
      }
      return identical;
    };

    // 0. The first composite: nothing to be incremental against.
    writeRgb(od.document, 0, 10, 10, {0.5f, 0.25f, 0.125f, 1.0f});
    od.recordEdit("initial content", EditKind::Content);
    step("initial");
    check(dt.fullRecomposites() == 1 && dt.incrementalUpdates() == 0 &&
              dt.lastFullRecompositeReason() == FullRecompositeReason::NoPreviousComposite,
          "sequence: the first composite is full -- there is no snapshot yet");

    // 1. A single-tile paint.
    writeRgb(od.document, 0, 40, 40, {0.25f, 0.5f, 0.75f, 1.0f});
    od.recordEdit("one dab", EditKind::Content);
    const bool paintOk = step("single-tile paint");
    check(paintOk && dt.incrementalUpdates() == 1 && dt.lastDirtyTiles() == 1,
          "sequence: a single-tile paint -> 1 tile, bit-identical");

    // 2. A multi-tile edit -- a stroke crossing a tile boundary.
    for (int32_t x = 120; x < 200; ++x) writeRgb(od.document, 0, x, 64, {1.0f, 1.0f, 1.0f, 1.0f});
    od.recordEdit("a stroke across tiles", EditKind::Content);
    const bool strokeOk = step("multi-tile edit");
    check(strokeOk && dt.lastDirtyTiles() == 2,
          "sequence: a stroke across a tile boundary -> 2 tiles, bit-identical");

    // 2b. **Two dirty tiles in the same tile row with a gap between them.**
    // The upload groups tiles into bands sharing a tile row, and the columns
    // between two non-adjacent tiles of a band were never composited -- so an
    // implementation that packed or uploaded a whole band at once would write
    // scratch into those columns. On the GPU that is invisible (they are not
    // uploaded either), which is precisely what makes it worth a fixture.
    writeRgb(od.document, 0, 20, 200, {0.9f, 0.1f, 0.1f, 1.0f});   // tile (0,1)
    writeRgb(od.document, 0, 420, 200, {0.1f, 0.9f, 0.1f, 1.0f});  // tile (3,1)
    od.recordEdit("two tiles with a gap", EditKind::Content);
    const bool gapOk = step("gapped band");
    check(gapOk && dt.lastDirtyTiles() == 2 &&
              dt.lastUploadedTexels() == 2u * static_cast<uint64_t>(kTileSize) * kTileSize,
          "sequence: two tiles with two clean tiles between them, bit-identical");

    // 3. A layer property change whose layer covers the WHOLE canvas -- so
    //    narrowing to "that layer's own tiles" (core/DirtyTiles.cpp's new
    //    per-layer footprint) still lands on all 16 of 16 tiles, and
    //    `preferFullRecomposite()`'s own crossover (core/DirtyTiles.hpp §6)
    //    takes the full path anyway, because at that point the two cost the
    //    same and full is one upload instead of sixteen. The reason is reset
    //    to `None` when THAT is why -- DocumentTexture's own comment calls it
    //    "nothing was wrong; it was just cheaper" -- which is what
    //    distinguishes this from the Adjustment-layer case, whose reason
    //    stays `LayerOpacityChanged` because there the classifier itself
    //    could not narrow at all.
    addLayer(od.document, od.document.layers.size(), makeRgbLayer("upper"));
    for (int32_t y = 8; y < 500; ++y)
      for (int32_t x = 8; x < 500; ++x) writeRgb(od.document, 1, x, y, {0.2f, 0.4f, 0.6f, 1.0f});
    od.recordEdit("add a layer", EditKind::Structural);
    check(step("add a layer"), "sequence: an added layer, bit-identical");
    const uint64_t fullsBeforeOpacity = dt.fullRecomposites();
    setLayerOpacity(od.document, 1, 0.5f);
    od.recordEdit("opacity", EditKind::Structural);
    const bool opacityOk = step("layer property change");
    check(opacityOk && dt.fullRecomposites() == fullsBeforeOpacity + 1 &&
              dt.lastDirtyTiles() == canvasTileCount(od.document) &&
              dt.lastFullRecompositeReason() == FullRecompositeReason::None,
          "sequence: a property change over the WHOLE canvas takes the full path via the size "
          "crossover, not the classifier");

    // 4. A reorder of two layers whose compared properties are identical.
    setLayerOpacity(od.document, 1, 1.0f);
    od.recordEdit("back to opaque", EditKind::Structural);
    step("back to opaque");
    moveLayer(od.document, 1, 0);
    od.recordEdit("reorder", EditKind::Structural);
    check(step("reorder"), "sequence: a reorder of interchangeable layers, bit-identical");

    // 5. A removed layer.
    removeLayer(od.document, 0);
    od.recordEdit("remove a layer", EditKind::Structural);
    check(step("removed layer"), "sequence: a removed layer, bit-identical");

    // 6. A mask edit.
    addLayerMask(od.document, 0);
    od.recordEdit("add a mask", EditKind::Structural);
    step("add a mask");
    const uint64_t incBeforeMask = dt.incrementalUpdates();
    for (int32_t y = 260; y < 300; ++y)
      for (int32_t x = 260; x < 300; ++x) writeMask(od.document, 0, x, y, 0.25f);
    od.recordEdit("paint the mask", EditKind::Content);
    const bool maskOk = step("mask edit");
    check(maskOk && dt.incrementalUpdates() == incBeforeMask + 1 && dt.lastDirtyTiles() == 1,
          "sequence: a mask edit is tile-local -> 1 tile, bit-identical");

    // 7. A clipped run: the flag itself is tile-local too now (the member's
    //    own tiles UNION its base's -- here the member is freshly added and
    //    empty, so it is just the base's), and painting into the clipped
    //    layer afterwards stays tile-local as well, with the clip honoured
    //    inside the dirty tile either way.
    addLayer(od.document, od.document.layers.size(), makeRgbLayer("clipped"));
    setLayerClipped(od.document, 1, true);
    od.recordEdit("clip it", EditKind::Structural);
    check(step("clip flag"), "sequence: a clip flag, bit-identical");
    const uint64_t incBeforeClipPaint = dt.incrementalUpdates();
    for (int32_t y = 20; y < 100; ++y)
      for (int32_t x = 20; x < 100; ++x) writeRgb(od.document, 1, x, y, {0.0f, 0.75f, 0.0f, 1.0f});
    od.recordEdit("paint the clipped layer", EditKind::Content);
    const bool clipPaintOk = step("paint inside a clipped run");
    check(clipPaintOk && dt.incrementalUpdates() == incBeforeClipPaint + 1,
          "sequence: painting a clipped layer stays tile-local, bit-identical");

    // 8. An adjustment layer: its stack is full, but painting *under* it is
    //    tile-local and the adjustment must re-apply inside the dirty tile.
    addLayer(od.document, od.document.layers.size(), makeAdjustmentLayer("exposure"));
    od.document.layers[2].ops.add(exposureOp(1.0f));
    od.recordEdit("adjustment layer", EditKind::Structural);
    check(step("adjustment layer"), "sequence: an adjustment layer, bit-identical");
    const uint64_t incBeforeUnder = dt.incrementalUpdates();
    for (int32_t y = 400; y < 440; ++y)
      for (int32_t x = 400; x < 440; ++x) writeRgb(od.document, 0, x, y, {0.3f, 0.3f, 0.3f, 1.0f});
    od.recordEdit("paint under the adjustment", EditKind::Content);
    const bool underOk = step("paint under an adjustment layer");
    check(underOk && dt.incrementalUpdates() == incBeforeUnder + 1,
          "sequence: painting under an adjustment layer, bit-identical");

    // 9. An empty edit.
    const uint64_t emptiesBefore = dt.emptyUpdates();
    const uint64_t texelsBefore = dt.totalUploadedTexels();
    setLayerName(od.document, 0, "renamed");
    od.recordEdit("rename", EditKind::Structural);
    const bool emptyOk = step("empty edit");
    check(emptyOk && dt.emptyUpdates() == emptiesBefore + 1 &&
              dt.totalUploadedTexels() == texelsBefore,
          "sequence: a rename composites nothing and uploads nothing at all");

    // 10. **Undo and redo**, the one edit that replaces the whole document
    //     rather than mutating it. A history entry is a copy-on-write
    //     `Document` copy taken at an earlier revision, so restoring it puts
    //     the *older* tile objects back in the store -- addresses the snapshot
    //     does not hold, which is exactly what makes an undo visible to a diff
    //     that only ever compares identities. Worth a fixture because a naive
    //     "has anything been written since I last looked" scheme would answer
    //     no: an undo writes nothing at all.
    //
    //     The revision is bumped by hand here because that is what an undo
    //     does -- `recordEdit()` would append a history entry and truncate the
    //     redo branch, which is the opposite of what is wanted.
    for (int32_t y = 60; y < 100; ++y)
      for (int32_t x = 460; x < 500; ++x)
        writeRgb(od.document, 0, x, y, {0.8f, 0.1f, 0.4f, 1.0f});
    od.recordEdit("a dab to undo", EditKind::Content);
    check(step("a dab to undo"), "sequence: the dab an undo will take back");
    check(od.history.canUndo(), "sequence: and history really can take it back");
    if (const Document* undone = od.history.undo()) od.document = *undone;
    ++od.revision;
    const bool undoOk = step("undo");
    check(undoOk && dt.lastDirtyTiles() >= 1,
          "sequence: an undo restores older tiles and is seen, bit-identical");
    if (const Document* redone = od.history.redo()) od.document = *redone;
    ++od.revision;
    check(step("redo"), "sequence: and a redo, bit-identical");

    check(everyStepIdentical,
          "sequence: every edit above, bit-identical at zero tolerance");
    check(dt.fullRecomposites() + dt.incrementalUpdates() + dt.emptyUpdates() == dt.uploads(),
          "sequence: the three outcomes account for every key miss exactly");

    // --- The GPU, against the same buffer -------------------------------
    std::vector<float> readback;
    const bool read = readbackRGBA16FPadded(gpu, dt.texture(), 512, 512, readback);
    bool gpuMatches = read && readback.size() == dt.uploadedHalves().size();
    for (size_t i = 0; gpuMatches && i < dt.uploadedHalves().size(); ++i)
      gpuMatches = gpuMatches && readback[i] == halfToFloat(dt.uploadedHalves()[i]);
    check(gpuMatches, "gpu: after a sequence of sub-rectangle writes the texture is exact");

    // The sub-rectangle upload really was a sub-rectangle: one more edit, and
    // count what crossed the bus.
    const size_t at = (40u * 512u + 300u) * 4u;
    const uint16_t wasThere = dt.uploadedHalves()[at + 0];
    writeRgb(od.document, 0, 300, 40, {1.0f, 0.0f, 1.0f, 1.0f});
    od.recordEdit("one more dab", EditKind::Content);
    dt.viewFor(gpu, od);
    check(dt.lastDirtyTiles() == 1 &&
              dt.lastUploadedTexels() == static_cast<uint64_t>(kTileSize) * kTileSize,
          "gpu: one dirty tile uploads one tile of texels, not one canvas");
    std::vector<float> after;
    const bool readAgain = readbackRGBA16FPadded(gpu, dt.texture(), 512, 512, after);
    // Compared against the full composite rather than against the authored
    // colour: the texel is under a clipping run and an adjustment layer, so
    // what reaches the screen is not what was painted -- which is exactly the
    // thing an incremental update has to keep getting right.
    const std::vector<uint16_t> expectedNow = compositeDocumentStraightHalf(od.document);
    check(readAgain && dt.uploadedHalves()[at + 0] != wasThere &&
              after[at + 0] == halfToFloat(expectedNow[at + 0]) &&
              after[at + 1] == halfToFloat(expectedNow[at + 1]) &&
              after[at + 2] == halfToFloat(expectedNow[at + 2]),
          "gpu: and it landed at (300,40) -- the sub-rectangle origin is right");
    bool stillWhole = readAgain && after.size() == dt.uploadedHalves().size();
    for (size_t i = 0; stillWhole && i < after.size(); ++i)
      stillWhole = stillWhole && after[i] == halfToFloat(dt.uploadedHalves()[i]);
    check(stillWhole, "gpu: and the 245760 texels it did NOT write are unchanged");

    // The alignment observation, re-checked rather than re-derived: a tile row
    // at RGBA16Float is exactly four 256-byte units, so a packed tile upload
    // would satisfy the copy alignment even where this path does not need to.
    check(static_cast<uint32_t>(kTileSize) * 4u * sizeof(uint16_t) == 4u * 256u,
          "gpu: a 128-texel tile row is 1024 bytes = exactly 4 x 256");

    // Deliberately NOT marked `[measured]`: every number on this line is
    // decided by the fixed edit sequence above, so it is an assertion in
    // printed form and must show up in an additions-only diff if it moves.
    std::printf("    %llu key miss(es): %llu full, %llu incremental, %llu empty; "
                "%llu texels uploaded against the %llu a recomposite-everything build would "
                "have sent (%.1f%%)\n",
                static_cast<unsigned long long>(dt.uploads()),
                static_cast<unsigned long long>(dt.fullRecomposites()),
                static_cast<unsigned long long>(dt.incrementalUpdates()),
                static_cast<unsigned long long>(dt.emptyUpdates()),
                static_cast<unsigned long long>(dt.totalUploadedTexels()),
                static_cast<unsigned long long>(dt.uploads() * 512ull * 512ull),
                100.0 * static_cast<double>(dt.totalUploadedTexels()) /
                    static_cast<double>(dt.uploads() * 512ull * 512ull));
    std::printf("    canvas-proportional memory held for a 512x512 document: %.2f MiB (the f16 "
                "mirror plus the incremental-band float scratch plus the full-recomposite float "
                "scratch)\n",
                static_cast<double>(dt.residentBytes()) / (1024.0 * 1024.0));
    dt.release();
    check(dt.residentBytes() == 0,
          "gpu: release() drops the mirror -- no claim about a texture that is gone");
  }

  // -----------------------------------------------------------------------
  // 4b. The canvas edge, and content outside it
  // -----------------------------------------------------------------------
  //
  // A canvas whose size is not a multiple of 128 has a partial tile on two of
  // its edges, and a store may hold tiles that lie **entirely outside** the
  // canvas (nothing stops a layer being painted past the edge -- io/ImageIO
  // and a `.npaint` both can). Both reach the region walk and the upload,
  // where a tile's rectangle has to be clipped twice: to the canvas, and to
  // the band being composited. 300x200 gives a 44-texel-wide right edge and a
  // 72-texel-tall bottom edge, neither of them 256-byte aligned in any
  // spelling.
  {
    OpenDocument od = makeBlankOpenDocument(300, 200, WorkingSpace{}, "edges");
    writeRgb(od.document, 0, 5, 5, {0.5f, 0.25f, 0.125f, 1.0f});
    // Outside the canvas on both axes, in both directions.
    writeRgb(od.document, 0, -40, 20, {1.0f, 0.0f, 0.0f, 1.0f});
    writeRgb(od.document, 0, 600, 20, {0.0f, 1.0f, 0.0f, 1.0f});
    writeRgb(od.document, 0, 20, -40, {0.0f, 0.0f, 1.0f, 1.0f});
    od.recordEdit("content past the edge", EditKind::Content);
    DocumentTexture dt;
    dt.viewFor(gpu, od);
    check(sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document)),
          "edges: a 300x200 canvas with content past every edge, bit-identical");

    // Now an incremental edit into the two partial edge tiles.
    writeRgb(od.document, 0, 299, 199, {1.0f, 1.0f, 0.0f, 1.0f});  // the corner texel
    writeRgb(od.document, 0, 260, 10, {0.0f, 1.0f, 1.0f, 1.0f});   // the right edge tile
    od.recordEdit("paint the edge tiles", EditKind::Content);
    dt.viewFor(gpu, od);
    check(sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document)),
          "edges: and an incremental edit into the partial tiles, bit-identical");
    // 44 x 72 for the bottom-right corner tile plus 44 x 128 for the right
    // edge tile: the clip is applied to the upload rectangle as well, not just
    // to the composite.
    check(dt.lastDirtyTiles() == 2 && dt.lastUploadedTexels() == 44u * 72u + 44u * 128u,
          "edges: the partial tiles upload their clipped rectangles, not full ones");

    // An edit that touches ONLY a tile outside the canvas: the dirty set is
    // non-empty, the canvas is unchanged, and the upload must send nothing.
    const std::vector<uint16_t> before = dt.uploadedHalves();
    writeRgb(od.document, 0, 620, 30, {1.0f, 0.0f, 1.0f, 1.0f});
    od.recordEdit("paint outside the canvas", EditKind::Content);
    dt.viewFor(gpu, od);
    check(dt.lastDirtyTiles() == 1 && dt.lastUploadedTexels() == 0 &&
              sameHalves(before, dt.uploadedHalves()),
          "edges: a dirty tile wholly outside the canvas uploads nothing");

    std::vector<float> readback;
    const bool read = readbackRGBA16FPadded(gpu, dt.texture(), 300, 200, readback);
    bool matches = read && readback.size() == dt.uploadedHalves().size();
    for (size_t i = 0; matches && i < readback.size(); ++i)
      matches = matches && readback[i] == halfToFloat(dt.uploadedHalves()[i]);
    check(matches, "edges: and the GPU texture agrees at a 2400-byte row stride");
    dt.release();
  }

  std::printf("  -- 5. a photographable sequence, and the picture it makes --\n");

  // -----------------------------------------------------------------------
  // 5. The deliverable: a sequence a person can look at
  // -----------------------------------------------------------------------
  //
  // The same pipeline on a 1024x1024 document with content shaped so that the
  // difference between "the dirty tiles updated" and "the whole layer updated"
  // is visible rather than numerical. Every step is asserted bit-identical
  // here as well; the PNGs are written only when `NP_INCREMENTAL_PNG_DIR` is
  // set in the environment, so a plain `--selftest` writes no files.
  {
    const char* pngDir = std::getenv("NP_INCREMENTAL_PNG_DIR");
    constexpr int32_t kW = 1024;

    // Straight-alpha linear f16 over the canvas block's own paper colour,
    // sRGB-encoded to 16-bit -- i.e. what the screen shows, not what the
    // buffer holds. The paper and the blend are ui/MacPaintUI's own
    // IM_COL32(250,250,247) and ImGui's (SrcAlpha, OneMinusSrcAlpha).
    auto writePng = [&](const std::vector<uint16_t>& halves, const std::string& path) {
      const size_t texels = static_cast<size_t>(kW) * static_cast<size_t>(kW);
      std::vector<uint16_t> rgba(texels * 4, 0);
      const std::array<float, 3> paper{250.0f / 255.0f, 250.0f / 255.0f, 247.0f / 255.0f};
      for (size_t t = 0; t < texels; ++t) {
        const float a = halfToFloat(halves[t * 4 + 3]);
        for (size_t c = 0; c < 3; ++c) {
          const float over = halfToFloat(halves[t * 4 + c]) * a + paper[c] * (1.0f - a);
          const float encoded = std::clamp(srgbEncode(over), 0.0f, 1.0f);
          rgba[t * 4 + c] = static_cast<uint16_t>(encoded * 65535.0f + 0.5f);
        }
        rgba[t * 4 + 3] = 65535;
      }
      const std::vector<uint8_t> png = encodePng16(static_cast<uint32_t>(kW),
                                                   static_cast<uint32_t>(kW), rgba.data());
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(png.data()),
                static_cast<std::streamsize>(png.size()));
      return out.good() && !png.empty();
    };

    OpenDocument od = makeBlankOpenDocument(kW, kW, WorkingSpace{}, "photographable");
    auto fillRect = [&](size_t layerIndex, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        const std::array<float, 4>& straight) {
      const std::array<float, 4> pm{straight[0] * straight[3], straight[1] * straight[3],
                                    straight[2] * straight[3], straight[3]};
      for (int32_t y = y0; y < y1; ++y)
        for (int32_t x = x0; x < x1; ++x) writeRgb(od.document, layerIndex, x, y, pm);
    };

    fillRect(0, 96, 96, 544, 544, {0.10f, 0.52f, 0.74f, 1.0f});
    setLayerName(od.document, 0, "Cyan block");
    addLayer(od.document, od.document.layers.size(), makeRgbLayer("Magenta, Multiply"));
    fillRect(1, 352, 288, 928, 928, {0.86f, 0.16f, 0.42f, 1.0f});
    setLayerBlend(od.document, 1, BlendMode::Multiply);
    od.recordEdit("the fixture", EditKind::Content);

    DocumentTexture dt;
    size_t written = 0;
    bool allIdentical = true;
    auto shot = [&](const char* name) {
      dt.viewFor(gpu, od);
      allIdentical = allIdentical &&
                     sameHalves(dt.uploadedHalves(), compositeDocumentStraightHalf(od.document));
      const char* how = dt.lastDirtyTiles() == canvasTileCount(od.document) ? "full "
                        : dt.lastDirtyTiles() == 0                          ? "empty"
                                                                            : "incr ";
      std::printf("    %-26s %s, %4zu dirty tile(s), %8llu texel(s) uploaded\n", name, how,
                  dt.lastDirtyTiles(),
                  static_cast<unsigned long long>(dt.lastUploadedTexels()));
      if (pngDir != nullptr && writePng(dt.uploadedHalves(), std::string(pngDir) + "/" + name +
                                                                 ".png"))
        ++written;
    };

    shot("1-base");

    // A dab, inside one tile.
    for (int32_t y = 700; y < 740; ++y)
      for (int32_t x = 140; x < 180; ++x)
        writeRgb(od.document, 0, x, y, {0.95f, 0.80f, 0.10f, 1.0f});
    od.recordEdit("a dab", EditKind::Content);
    shot("2-one-tile-dab");

    // A stroke across five tiles.
    for (int32_t y = 810; y < 850; ++y)
      for (int32_t x = 140; x < 780; ++x)
        writeRgb(od.document, 0, x, y, {0.05f, 0.65f, 0.35f, 1.0f});
    od.recordEdit("a stroke", EditKind::Content);
    shot("3-multi-tile-stroke");

    // **The property change**: tile-local now, but to the WHOLE of the
    // magenta layer's own 36-tile footprint (a 6x6 block of the 8x8 canvas),
    // not to the two or three tiles the paint dab alone would touch. It
    // carries that paint edit in the same revision on purpose -- to show what
    // an even more optimistic reading (the dab's own tiny tile set standing
    // in for the whole edit) would have gotten wrong, immediately below.
    const Document beforeChange = od.document;
    for (int32_t y = 380; y < 420; ++y)
      for (int32_t x = 380; x < 420; ++x)
        writeRgb(od.document, 0, x, y, {0.99f, 0.97f, 0.30f, 1.0f});
    const Document afterPaintOnly = od.document;
    setLayerOpacity(od.document, 1, 0.35f);
    od.recordEdit("paint, and magenta opacity 0.35", EditKind::Structural);
    shot("4-layer-opacity");

    // --- The rejected reading, run beside the built one -----------------
    //
    // Treat the property change as if it were tile-local: patch only the tiles
    // the *paint* moved onto the previous composite. That is what an
    // optimistic classification produces, and it is a rectangle of the magenta
    // layer at its new opacity surrounded by the same layer at its old one --
    // a corner, permanently, because nothing ever revisits those tiles.
    {
      const DocumentDirtyTiles tileOnly = documentDirtyTiles(beforeChange, afterPaintOnly);
      std::vector<float> wrong = compositeDocumentPremultiplied(beforeChange);
      CompositeRegion region;
      region.pixels = wrong.data();
      region.origin = PixelCoord{0, 0};
      region.width = kW;
      region.height = kW;
      compositeDocumentTilesPremultiplied(od.document, tileOnly.tiles, region, nullptr);

      std::vector<uint16_t> wrongHalves(wrong.size());
      for (size_t t = 0; t * 4 + 3 < wrong.size(); ++t) {
        const std::array<float, 4> straight = unpremultiply(std::array<float, 4>{
            wrong[t * 4 + 0], wrong[t * 4 + 1], wrong[t * 4 + 2], wrong[t * 4 + 3]});
        for (size_t c = 0; c < 4; ++c) wrongHalves[t * 4 + c] = floatToHalf(straight[c]);
      }
      size_t differing = 0;
      for (size_t t = 0; t * 4 + 3 < wrongHalves.size(); ++t)
        if (std::memcmp(&wrongHalves[t * 4], &dt.uploadedHalves()[t * 4],
                        4 * sizeof(uint16_t)) != 0)
          ++differing;
      check(differing > 0,
            "picture: the tile-local reading of a property change really differs");
      std::printf("    the rejected reading (a property change treated as tile-local) leaves "
                  "%zu of %zu texels wrong -- %.1f%% of the canvas, and permanently, because "
                  "nothing revisits those tiles\n",
                  differing, static_cast<size_t>(kW) * kW,
                  100.0 * static_cast<double>(differing) /
                      static_cast<double>(static_cast<size_t>(kW) * kW));
      if (pngDir != nullptr &&
          writePng(wrongHalves, std::string(pngDir) + "/4b-if-it-were-tile-local.png"))
        ++written;
    }

    // And a visibility toggle, the other property change -- narrowed to the
    // same 36-tile footprint rather than the whole 64-tile canvas.
    setLayerVisible(od.document, 1, false);
    od.recordEdit("hide magenta", EditKind::Structural);
    shot("5-layer-hidden");

    // A rename: the revision moves and the picture must not.
    const std::vector<uint16_t> beforeRename = dt.uploadedHalves();
    setLayerName(od.document, 0, "Cyan block, renamed");
    od.recordEdit("rename", EditKind::Structural);
    shot("6-empty-edit");
    check(sameHalves(beforeRename, dt.uploadedHalves()),
          "picture: a rename leaves the uploaded bytes bit-identical");

    check(allIdentical,
          "picture: every step of the photographable sequence is bit-identical");
    if (pngDir != nullptr)
      std::printf("    NP_INCREMENTAL_PNG_DIR set: wrote %zu PNG(s) to %s\n", written, pngDir);
    dt.release();
  }

  std::printf("  -- 6. what it costs, and where incremental stops winning --\n");

  // -----------------------------------------------------------------------
  // 6. Cost, and the crossover
  // -----------------------------------------------------------------------
  {
    constexpr int32_t kSize = 1024;
    Document doc = Document::createBlank(kSize, kSize, WorkingSpace{});
    addLayer(doc, doc.layers.size(), makeRgbLayer("upper"));
    for (int32_t y = 0; y < kSize; y += 3) {
      for (int32_t x = 0; x < kSize; x += 3) {
        writeRgb(doc, 0, x, y, {0.25f, 0.5f, 0.75f, 1.0f});
        if (((x + y) & 1) == 0) writeRgb(doc, 1, x, y, {0.5f, 0.25f, 0.125f, 0.5f});
      }
    }
    const size_t tileCount = canvasTileCount(doc);

    std::vector<float> canvas(static_cast<size_t>(kSize) * kSize * 4, 0.0f);
    CompositeRegion whole;
    whole.pixels = canvas.data();
    whole.origin = PixelCoord{0, 0};
    whole.width = kSize;
    whole.height = kSize;

    auto timeTiles = [&](const std::vector<TileCoord>& tiles, int repeats) {
      const auto t0 = std::chrono::steady_clock::now();
      for (int r = 0; r < repeats; ++r)
        compositeDocumentTilesPremultiplied(doc, tiles, whole, nullptr);
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      return ms / static_cast<double>(repeats);
    };

    const auto f0 = std::chrono::steady_clock::now();
    const std::vector<float> full = compositeDocumentPremultiplied(doc);
    const double fullMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();

    std::vector<TileCoord> one{TileCoord{3, 3}};
    // A realistic edit: a dab whose footprint crosses a tile corner, which is
    // the common case for a round brush of any size on a 128-texel grid.
    std::vector<TileCoord> dab{TileCoord{3, 3}, TileCoord{4, 3}, TileCoord{3, 4},
                               TileCoord{4, 4}};
    std::vector<TileCoord> eight;
    for (int32_t i = 0; i < 8; ++i) eight.push_back(TileCoord{i % 8, 1});

    const double oneMs = timeTiles(one, 200);
    const double dabMs = timeTiles(dab, 100);
    const double eightMs = timeTiles(eight, 50);

    // The straight line through the 1-tile and 8-tile measurements: a
    // per-call setup plus a per-tile cost. The crossover is where that line
    // meets the full recomposite.
    const double perTileMs = (eightMs - oneMs) / 7.0;
    const double setupMs = oneMs - perTileMs;
    const double crossover = perTileMs > 0.0 ? (fullMs - setupMs) / perTileMs : 0.0;

    check(full.size() == canvas.size(), "cost: the timed buffers really are the same shape");
    check(oneMs < fullMs, "cost: one dirty tile is cheaper than the whole canvas -- the premise");
    check(dabMs < fullMs, "cost: and so is a four-tile dab");
    check(tileCount == 64u, "cost: a 1024x1024 canvas is 64 tiles of 128");

    std::printf("    [measured] composite only -- full %.3f ms; 1 tile %.4f ms; a 4-tile dab "
                "%.4f ms; 8 tiles %.4f ms\n", fullMs, oneMs, dabMs, eightMs);
    std::printf("    [measured] fitted %.4f ms setup + %.4f ms/tile -> the composite crossover "
                "is at %.1f dirty tiles of %zu (%.0f%% of the canvas)\n",
                setupMs, perTileMs, crossover, tileCount,
                100.0 * crossover / static_cast<double>(tileCount));
    std::printf("    [measured] the 4-tile dab is %.0fx cheaper than the full recomposite it "
                "replaces. PRD F3 (P0) allows 20 ms pen-to-photon for everything; the full "
                "recomposite alone spends %.0f%% of that, the dab %.1f%%\n",
                fullMs / std::max(dabMs, 1.0e-9), 100.0 * fullMs / 20.0, 100.0 * dabMs / 20.0);

    // --- The same question end to end, through the real object ------------
    //
    // The fit above is of the *composite* alone. What a frame actually pays is
    // composite + un-premultiply + f16 pack + upload, and the two paths differ
    // on the last two as well: a full recomposite packs a whole canvas and
    // issues one `wgpuQueueWriteTexture`, an incremental one packs a band per
    // tile row and issues one write per dirty tile. So the crossover that
    // matters is measured here rather than extrapolated from the composite.
    {
      OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, "cost");
      od.document = doc;
      od.recordEdit("fixture", EditKind::Content);
      DocumentTexture dt;
      dt.viewFor(gpu, od);  // the first one is full and is not timed

      auto timeIncremental = [&](int32_t tiles, int repeats) {
        double total = 0.0;
        for (int r = 0; r < repeats; ++r) {
          for (int32_t i = 0; i < tiles; ++i) {
            const PixelCoord o = tileOrigin(TileCoord{i % 8, i / 8});
            writeRgb(od.document, 0, o.x + 3 + r, o.y + 3, {0.4f, 0.4f, 0.4f, 1.0f});
          }
          od.recordEdit("dirty tiles", EditKind::Content);
          const auto t0 = std::chrono::steady_clock::now();
          dt.viewFor(gpu, od);
          total += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
        }
        return total / static_cast<double>(repeats);
      };
      // A property change is the production full path, so it is what gets
      // timed -- not a released texture, which would fold a texture creation
      // into the number.
      auto timeFull = [&](int repeats) {
        double total = 0.0;
        for (int r = 0; r < repeats; ++r) {
          setLayerOpacity(od.document, 0, 1.0f - 0.001f * static_cast<float>(r + 1));
          od.recordEdit("opacity", EditKind::Structural);
          const auto t0 = std::chrono::steady_clock::now();
          dt.viewFor(gpu, od);
          total += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
        }
        return total / static_cast<double>(repeats);
      };

      const double e2eOne = timeIncremental(1, 20);
      const double e2eFour = timeIncremental(4, 20);
      const double e2eSixteen = timeIncremental(16, 10);
      const double e2eFull = timeFull(10);
      const double e2ePerTile = (e2eSixteen - e2eOne) / 15.0;
      const double e2eCrossover = e2ePerTile > 0.0 ? (e2eFull - (e2eOne - e2ePerTile)) / e2ePerTile
                                                   : 0.0;
      check(e2eFour < e2eFull, "cost: end to end, a four-tile dab beats a full update");
      std::printf("    [measured] end to end (composite + pack + upload) -- full %.3f ms; "
                  "1 tile %.4f ms; 4 tiles %.4f ms; 16 tiles %.4f ms\n",
                  e2eFull, e2eOne, e2eFour, e2eSixteen);
      std::printf("    [measured] end-to-end crossover at %.1f dirty tiles of %zu (%.0f%% of the "
                  "canvas) -- so the crossover predicted at about a third of the canvas is not "
                  "there, and incremental is cheaper at every dirty count. The policy therefore "
                  "takes the full path only at %.0f%% of the canvas, where the two do the same "
                  "compositing work and full is one upload instead of %zu\n",
                  e2eCrossover, tileCount, 100.0 * e2eCrossover / static_cast<double>(tileCount),
                  100.0 * kFullRecompositeTileFraction, tileCount);
      std::printf("    [measured] what the rejected half-canvas threshold would have cost: an "
                  "extrapolated %.1f ms incremental against %.1f ms full at %zu dirty tiles\n",
                  (e2eOne - e2ePerTile) + e2ePerTile * static_cast<double>(tileCount / 2),
                  e2eFull, tileCount / 2);
      dt.release();
    }

    check(preferFullRecomposite(tileCount, tileCount),
          "cost: every tile dirty -> the policy takes the full path");
    check(!preferFullRecomposite(1, tileCount), "cost: one tile dirty -> incremental");
    check(!preferFullRecomposite(4, tileCount), "cost: a four-tile dab -> incremental");
    check(preferFullRecomposite(0, 0),
          "cost: a canvas with no tiles is full, not a divide by zero");
  }

  std::printf("[selftest] incremental composite %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
