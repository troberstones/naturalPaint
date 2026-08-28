#include "app/selftest/Support.hpp"

namespace np {

// ==========================================================================
// Phase 4 step 4 -- native `.npaint` save and load (multi-part tiled EXR).
// ==========================================================================
bool runNpaintFormatTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // There is exactly one number here, and most of this section deliberately
  // does not use it: the layer round trip is asserted at **zero tolerance**,
  // because the chain it travels has no rounding stage in it. A tile holds
  // `uint16_t` half words; buildLayerPart() memcpys them into the part
  // buffer; OpenImageIO writes them as TypeDesc::HALF; ZIP is lossless;
  // reading asks for TypeDesc::HALF back; unpackLayerPart() memcpys them
  // into a tile. No float appears anywhere along it. That is precisely
  // docs/document-format.md's own justification for EXR ("Working space is
  // `rgba16float` -> HALF channels -- byte-identical, no conversion"), and a
  // tolerance there would let a real regression through. It is also the same
  // claim, and the same reasoning, runFormatSupportTest() applies to the EXR
  // *export* round trip.
  //
  // The composite (part 0) is a different matter and is the one place a
  // tolerance is honest, because it is a regenerated product that genuinely
  // passes through float:
  //
  //   tile half -> readPixel (exact: every half is exactly a float)
  //     -> flatten's composite (PLAN.md Phase 5 step 1: `over`, honouring
  //        opacity). This used to be a *sum of a single contribution*, i.e.
  //        exact; it is now up to two roundings per channel -- the opacity
  //        multiply (`* 0.72f` on layer 0) and `over`'s multiply-add -- each
  //        <= 2^-24 relative
  //     -> un-premultiply, /a (rounds, <= 2^-24 relative)
  //     -> re-associate, *a (rounds, <= 2^-24 relative)
  //     -> float-to-half (rounds, <= 2^-11 relative -- binary16 has 10
  //        stored mantissa bits, so half an ulp is v * 2^-11)
  //     -> half-to-float on read (exact)
  //     -> un-premultiply for the DecodedImage contract, /a (<= 2^-24)
  //
  // The float roundings are ~8000x smaller than the half one and are kept in
  // the bound rather than waved away: relative error <= 2^-11 + 5*2^-24 =
  // 4.8830e-4 + 2.98e-7 = 4.8860e-4. The fixture's composite values are all
  // <= 1.0, so that is also the absolute bound. Landed 7.0e-4 -- 1.43x the
  // *derived bound*, not 1.43x the measurement, matching runLutBakeTest()'s
  // kResidualTol / runApplyPassTest()'s kApplyResidualTol / step 1's
  // kRoundTripTol16 discipline and runFormatSupportTest()'s own
  // kHalfEncodedTol, which is the same 2^-11 argument for the same reason.
  // The measured worst case is printed at run time so the derivation is
  // checkable rather than merely asserted.
  //
  // And for the *opaque* pixels the composite is additionally asserted
  // **exactly**: at alpha == 1.0 (exact in half) the two un-premultiplies
  // and the re-association are all by exactly 1.0, so the only surviving
  // stage is a float-to-half of a value that already is a half. Zero.
  //
  // Phase 5 step 1 nearly made that half of the claim vacuous and it is worth
  // saying why it did not. Compositing honours opacity, and layer 0 sits at
  // 0.72, so **none of layer 0's opaque pixels composites to alpha 1.0 any
  // more** -- 1.0 * 0.72 lands at 0.72. The pixel that keeps this assertion
  // meaningful is layer 1's opaque texel at (129,3), which nothing overlaps
  // and which sits at opacity 1.0, so it composites to alpha exactly 1.0 and
  // travels the whole multiply-by-one chain above. That is also why layer 1
  // had to be the visible one (see the fixture).
  constexpr float kCompositeTol = 7.0e-4f;

  // Fixture helpers, matching runExportTest()/runFormatSupportTest()'s: a
  // document holds *premultiplied* halfs, so a fixture writes straight
  // values through the same `rgb *= a` io/ImageIO.cpp performs on import,
  // and every precision claim is checked against the tile's own stored
  // value rather than the float literal that went in.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto storedWords = [](const Document& doc, size_t layerIndex,
                        TileCoord coord) -> const uint16_t* {
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(coord);
    return t ? t->data() : nullptr;
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };

  // A three-layer document with content in several different tiles (so the
  // per-part data window is genuinely a bounding box and not "the canvas"),
  // distinct metadata on every layer, and a translucent pixel as well as
  // opaque ones.
  auto buildFixture = []() {
    Document doc = Document::createBlank(400, 300, WorkingSpace{});
    doc.layers[0].name = "Line pass";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.72f;
    doc.layers[0].visible = true;
    doc.layers[0].locked = true;
    doc.layers[0].parent = "G0001";

    Layer mid;
    mid.kind = LayerKind::RGB;
    mid.rgbTiles.emplace();
    mid.name = "Flats";
    mid.blend = "normal";
    mid.opacity = 1.0f;
    // **Visible, and this moved here from `top` in Phase 5 step 1.** The
    // fixture needs exactly one layer with `np:visible == 0` to prove the
    // attribute round-trips as false rather than as absent, and it needs the
    // two-layers-at-one-pixel construction below to stay a *real* overlap so
    // the composite tolerance keeps bounding something. Those two demands
    // collided the moment a hidden layer started contributing nothing: this
    // layer owns half the overlap, so hiding it would have voided the
    // measurement silently. `top` -- which has no tiles at all and already
    // contributes nothing at opacity 0.0 -- carries the `false` instead, at no
    // cost to either property.
    mid.visible = true;
    mid.locked = false;
    mid.parent = "";
    doc.layers.push_back(std::move(mid));

    Layer top;
    top.kind = LayerKind::RGB;
    top.rgbTiles.emplace();
    top.name = "Layer 1";  // deliberately duplicated below -- names are not unique
    // **Changed from "screen" by PLAN.md Phase 5 step 2, and the change is
    // load-bearing rather than cosmetic.** This fixture's blend names carry
    // two claims at once: that `np:blend` round-trips *verbatim* (PRD I10),
    // and that a name this build cannot composite is reported at the save
    // boundary rather than silently approximated. Step 2 implements both
    // "multiply" and "screen", which would have quietly voided the second
    // claim -- the save would produce no warnings and the assertion below
    // would have to be deleted -- and weakened the first, since a name the
    // build recognises proves nothing about carrying one it does not.
    // "linear-burn" is outside core/Blend's set, so both claims are back.
    // Layer 0 keeps "multiply" so the round trip also covers a name that IS
    // in the set. No pixel changes: this layer has no tiles, is hidden and
    // sits at opacity 0, so it has never contributed to the composite.
    top.blend = "linear-burn";
    top.opacity = 0.0f;
    top.visible = false;  // see `mid.visible` above for why the false lives here
    top.locked = true;
    top.parent = "G0001";
    doc.layers.push_back(std::move(top));
    return doc;
  };

  Document fixture = buildFixture();
  // Layer 0: two well-separated tiles, so its data window spans a 3x2 tile
  // block with a hole in it -- the case a rectangular EXR data window cannot
  // encode sparsely and the reader's drop-all-zero-tiles rule handles.
  writeStraight(fixture, 0, 5, 7, 0.25f, 0.5f, 0.75f, 1.0f);
  writeStraight(fixture, 0, 300, 200, 1.0f, 0.0f, 0.5f, 1.0f);
  // A translucent texel, so the un-premultiply path is exercised on both
  // sides rather than only alpha == 1. Alpha is 0.3, NOT 0.5: a
  // power-of-two alpha makes the divide and the multiply back exact, which
  // would make the composite tolerance below vacuous -- it measured exactly
  // zero with 0.5 and never touched the term it exists to bound.
  writeStraight(fixture, 0, 6, 7, 0.8f, 0.4f, 0.2f, 0.3f);
  // Layer 1: one tile, far from layer 0's.
  writeStraight(fixture, 1, 129, 3, 0.125f, 0.875f, 0.0f, 1.0f);
  // Two layers contributing to the SAME pixel, with alphas that do not sum
  // to 1 and premultiplied values whose float sum is not itself a half.
  // This is the only construction in this fixture that makes the composite's
  // float->half stage actually round: everywhere else the un-premultiply and
  // re-association are by the same alpha and cancel exactly, which measured
  // 0.000e+00 and left the derived tolerance below bounding nothing.
  writeStraight(fixture, 0, 10, 10, 0.1f, 0.2f, 0.3f, 0.5f);
  writeStraight(fixture, 1, 10, 10, 0.7f, 0.6f, 0.5f, 0.25f);
  // Layer 2: no painted tiles at all -- the empty-layer edge case (EXR has
  // no zero-area part, so this is written as one all-zero tile and must come
  // back with zero tiles allocated, not one).

  // --- Request validation: identical in both build configurations --------
  //
  // Deliberately checked before anything that needs a backend. saveNpaint()
  // validates the request before it consults NP_USE_OIIO, so a malformed
  // request is refused the same way everywhere and --selftest can prove all
  // of PRD I11's refusals in the build with no OpenImageIO in it.
  {
    // PRD I7. The predicate first, since it is the public, backend-free half.
    std::string why;
    check(npaintCompressionIsLossy("dwaa", &why) && contains(why, "dwaa") &&
              contains(why, "lossy") && contains(why, "zip"),
          "I7: dwaa is refused by name, with the reason and an alternative");
    check(npaintCompressionIsLossy("DWAB", nullptr) && npaintCompressionIsLossy("b44", nullptr) &&
              npaintCompressionIsLossy("b44a", nullptr),
          "I7: dwab, b44 and b44a are lossy too, and the match is case-insensitive");
    check(npaintCompressionIsLossy("dwaa:45", &why) && contains(why, "dwaa"),
          "I7: the `name:level` form is caught -- `dwaa:45` cannot slip past");
    check(npaintCompressionIsLossy("pxr24", &why) && contains(why, "24 bits"),
          "I7: pxr24 is refused too (lossless only for half/integer, and a .npaint file can "
          "carry float parts forward)");
    // `&why` on the last one deliberately: the predicate must *clear* the
    // reason string when it answers "not lossy", or a caller reusing one
    // buffer across calls would report the previous refusal's wording.
    check(!npaintCompressionIsLossy("zip", nullptr) && !npaintCompressionIsLossy("piz", nullptr) &&
              !npaintCompressionIsLossy("zips", nullptr) &&
              !npaintCompressionIsLossy("rle", nullptr) &&
              !npaintCompressionIsLossy("none", &why) && why.empty(),
          "I7: the five lossless compressors are not refused, and the reason string is "
          "cleared for them");

    NpaintSaveOptions lossy;
    lossy.compression = "dwab:60";
    const NpaintSaveResult r = saveNpaint(fixture, "selftest_npaint_never.npaint", lossy);
    check(!r.ok && contains(r.error, "dwab") && contains(r.error, "lossy") &&
              contains(r.error, "PRD I7"),
          "I7: saveNpaint() itself refuses a lossy compressor by name (both builds)");
    std::FILE* f = std::fopen("selftest_npaint_never.npaint", "rb");
    check(f == nullptr, "I7: and nothing was written -- the refusal precedes the file");
    if (f) {
      std::fclose(f);
      std::remove("selftest_npaint_never.npaint");
    }

    NpaintSaveOptions unknown;
    unknown.compression = "squish9000";
    const NpaintSaveResult u = saveNpaint(fixture, "selftest_npaint_never.npaint", unknown);
    check(!u.ok && contains(u.error, "squish9000") && contains(u.error, "zip"),
          "I7: an unrecognised compressor is refused too -- an unknown name cannot be "
          "assumed lossless");

    Document zeroCanvas;
    const NpaintSaveResult z = saveNpaint(zeroCanvas, "selftest_npaint_never.npaint");
    check(!z.ok && contains(z.error, "no canvas"),
          "I11: a zero-area canvas is refused with a specific message");

    // Phase 5 step 3 made Pigment layers saveable (`pig.*`/`res.*` channels),
    // so the kind with no on-disk representation is now Media -- it needs
    // per-medium simulation state core::Layer has no member for. The claim
    // this assertion has always made is unchanged: a kind that cannot be
    // written is named by index, name and kind rather than dropped.
    Document withMedia = Document::createBlank(64, 64, WorkingSpace{});
    Layer media;
    media.kind = LayerKind::Media;
    media.name = "Wet wash";
    withMedia.layers.push_back(std::move(media));
    const NpaintSaveResult p = saveNpaint(withMedia, "selftest_npaint_never.npaint");
    check(!p.ok && contains(p.error, "layer 1") && contains(p.error, "Wet wash") &&
              contains(p.error, "Media") && contains(p.error, "np:simParams"),
          "I11: a layer kind with no on-disk representation is refused by index, name and "
          "kind -- naming exactly what would be lost, not degrading silently");

    // And the Pigment analogue of the malformed-RGB case below: the kind is
    // writable now, so the refusal that remains is a Pigment layer with no
    // storage at all, which core/Layer.hpp says cannot happen.
    Document pigmentNoTiles = Document::createBlank(64, 64, WorkingSpace{});
    Layer bare;
    bare.kind = LayerKind::Pigment;
    bare.name = "Bare";
    pigmentNoTiles.layers.push_back(std::move(bare));
    const NpaintSaveResult bp = saveNpaint(pigmentNoTiles, "selftest_npaint_never.npaint");
    check(!bp.ok && contains(bp.error, "layer 1") && contains(bp.error, "Bare") &&
              contains(bp.error, "pigmentTiles"),
          "I11: a Pigment layer with no tile storage is refused as malformed rather than "
          "written as an empty part");

    Document badOpacity = Document::createBlank(64, 64, WorkingSpace{});
    badOpacity.layers[0].opacity = 1.5f;
    const NpaintSaveResult o = saveNpaint(badOpacity, "selftest_npaint_never.npaint");
    check(!o.ok && contains(o.error, "opacity") && contains(o.error, "[0, 1]"),
          "I11: an out-of-range opacity is refused rather than written for readers to "
          "misinterpret");

    Document noTiles = Document::createBlank(64, 64, WorkingSpace{});
    noTiles.layers[0].rgbTiles.reset();
    const NpaintSaveResult n = saveNpaint(noTiles, "selftest_npaint_never.npaint");
    check(!n.ok && contains(n.error, "no tile storage"),
          "I11: an RGB layer with no tile storage is refused as malformed");

    // The two measured OpenImageIO limitations, refused by name. Both are
    // request-shape checks, so both fire in either build.
    NpaintCarry blobCarry;
    NpaintAttribute blob;
    blob.name = "np:futureOps";
    blob.type = NpaintAttribute::Type::Blob;
    blob.blobValue = {1, 2, 250, 4, 5};
    blobCarry.documentAttributes.push_back(blob);
    const NpaintSaveResult b =
        saveNpaint(fixture, "selftest_npaint_never.npaint", {}, &blobCarry);
    check(!b.ok && contains(b.error, "np:futureOps") && contains(b.error, "UINT8[n]") &&
              contains(b.error, "base64"),
          "measured: a UINT8[n] blob attribute is refused by name (OpenImageIO drops array "
          "attributes on write) with the string-encoding workaround named");

    NpaintCarry scanlineCarry;
    NpaintRawPart scanline;
    scanline.name = "X0001";
    scanline.width = scanline.height = 2;
    scanline.channelNames = {"a", "b"};
    scanline.sampleTypeName = "float";
    scanline.rawPixels.assign(2 * 2 * 2 * sizeof(float), 0);
    scanlineCarry.rawParts.push_back(scanline);
    const NpaintSaveResult sc =
        saveNpaint(fixture, "selftest_npaint_never.npaint", {}, &scanlineCarry);
    check(!sc.ok && contains(sc.error, "X0001") && contains(sc.error, "scanline"),
          "measured: a carried scanline part is refused by name (OpenEXR multi-part cannot "
          "mix scanline and tiled parts)");
  }

  // --- The save entry point succeeds with an OpenEXR writer ---------------
  {
    const NpaintSaveResult r = saveNpaint(fixture, "selftest_npaint_gate.npaint");
    check(r.ok, "the save entry point succeeds with an OpenEXR writer");
    std::remove("selftest_npaint_gate.npaint");
  }

  {
    const char* kPath = "selftest_npaint_roundtrip.npaint";
    std::remove(kPath);

    // --- The round trip ---------------------------------------------------
    const NpaintSaveResult saved = saveNpaint(fixture, kPath);
    check(saved.ok && saved.error.empty(), "save: a three-layer document writes without error");
    check(saved.partsWritten == 4,
          "save: four parts -- part 0's composite plus one per layer (PRD I4)");
    // **Changed by PLAN.md Phase 5 step 1, and narrowed by step 2.** This used
    // to assert `saved.warnings.empty()`. It cannot, and the reason is the
    // point rather than an inconvenience: this fixture's top layer carries a
    // blend name core/Blend does not implement, so part 0 -- which is
    // regenerated on every save (PRD I12) and is a real composite rather than
    // a sum -- is an approximation of what that layer means. The save still
    // goes ahead, because refusing would make a PRD I10-preserved blend name
    // the thing that stops the document being saved (core/Composite.hpp argues
    // that at length); it just says so. Asserting the *content* of the warning
    // rather than merely its count is what makes "never silently" checkable.
    //
    // Step 2 took this from two warnings to one: "multiply" is implemented
    // now, so only the deliberately-unknown "linear-burn" on the top layer
    // remains. See the fixture for why that layer's name was changed rather
    // than the assertion relaxed.
    {
      bool warnedUnknown = false;
      for (const std::string& w : saved.warnings)
        if (contains(w, "\"linear-burn\"") && contains(w, "Layer 1")) warnedUnknown = true;
      check(saved.warnings.size() == 1 && warnedUnknown,
            "save: the one layer whose blend this build cannot composite is named, with its "
            "blend, rather than silently composited as `over`");
    }

    // The file really is an OpenEXR file, checked in its own bytes rather
    // than by trusting the writer: magic 0x76 0x2f 0x31 0x01, then the
    // version int whose bit 12 (0x1000) is OpenEXR's multi-part flag.
    std::vector<uint8_t> head;
    if (std::FILE* f = std::fopen(kPath, "rb")) {
      uint8_t buf[8] = {};
      const size_t n = std::fread(buf, 1, sizeof(buf), f);
      head.assign(buf, buf + n);
      std::fclose(f);
    }
    check(head.size() == 8 && head[0] == 0x76 && head[1] == 0x2f && head[2] == 0x31 &&
              head[3] == 0x01,
          "save: the bytes on disk are an OpenEXR file (magic 0x76 0x2f 0x31 0x01)");
    check(head.size() == 8 && (head[5] & 0x10) != 0,
          "save: with OpenEXR's multi-part bit set in the version field -- multi-part is a "
          "property of the file, not of our reader");

    const NpaintLoadResult loaded = loadNpaint(kPath);
    check(loaded.ok && loaded.error.empty(), "load: reads back without error");
    check(loaded.warnings.empty(),
          "load: with no warnings at all -- a file this build wrote is a file it fully "
          "understands");
    check(loaded.document.width == 400 && loaded.document.height == 300,
          "load: the canvas comes back from the display window");
    check(loaded.document.layers.size() == 3, "load: all three layers come back");

    if (loaded.document.layers.size() == 3) {
      // --- Bit-exactness, at zero tolerance ------------------------------
      size_t comparedTiles = 0;
      size_t differingWords = 0;
      for (size_t li = 0; li < 3; ++li) {
        const TileStore& src = *fixture.layers[li].rgbTiles;
        const TileStore& dst = *loaded.document.layers[li].rgbTiles;
        for (const auto& [coord, tile] : src) {
          (void)tile;
          const uint16_t* a = storedWords(fixture, li, coord);
          const Tile* bt = dst.find(coord);
          if (!a || !bt) {
            differingWords += Tile::kTexelCount;
            continue;
          }
          ++comparedTiles;
          for (size_t w = 0; w < Tile::kTexelCount; ++w) {
            if (a[w] != bt->data()[w]) ++differingWords;
          }
        }
      }
      std::printf("    [measured] %zu tiles compared, %zu of %zu half words differ "
                  "(expected exactly 0)\n",
                  comparedTiles, differingWords, comparedTiles * Tile::kTexelCount);
      check(comparedTiles == 4 && differingWords == 0,
            "fidelity: every layer's tiles come back BIT-identical -- zero tolerance, the "
            "claim docs/document-format.md makes for HALF channels");

      check(loaded.document.layers[0].rgbTiles->occupiedTileCount() == 2 &&
                loaded.document.layers[1].rgbTiles->occupiedTileCount() == 2 &&
                loaded.document.layers[2].rgbTiles->occupiedTileCount() == 0,
            "fidelity: the sparse tile set survives -- including the hole inside layer 0's "
            "data window and the layer that has no tiles at all");

      // --- The seven per-layer np:* attributes ---------------------------
      const Layer& l0 = loaded.document.layers[0];
      const Layer& l1 = loaded.document.layers[1];
      const Layer& l2 = loaded.document.layers[2];
      check(l0.kind == LayerKind::RGB && l1.kind == LayerKind::RGB && l2.kind == LayerKind::RGB,
            "np:kind: every layer's kind round-trips");
      check(l0.name == "Line pass" && l1.name == "Flats" && l2.name == "Layer 1",
            "np:name: the user-facing name round-trips, and is allowed to duplicate -- the "
            "part id (L0001) is what has to be unique");
      check(l0.blend == "multiply" && l1.blend == "normal" && l2.blend == "linear-burn",
            "np:blend: the blend identity round-trips verbatim");
      check(l0.opacity == 0.72f && l1.opacity == 1.0f && l2.opacity == 0.0f,
            "np:opacity: exact float equality, including 0.0 (an absent attribute would "
            "read back as the 1.0 default and fail here)");
      check(l0.visible == true && l1.visible == true && l2.visible == false,
            "np:visible: round-trips, with false actually distinguishable from absent");
      check(l0.locked == true && l1.locked == false && l2.locked == true,
            "np:locked: round-trips");
      check(l0.parent == "G0001" && l1.parent == "" && l2.parent == "G0001",
            "np:parent: round-trips (and the empty case lands on the reader's own default, "
            "since OpenImageIO drops empty string attributes -- measured)");

      check(loaded.carry.layerPartNames.size() == 3 &&
                loaded.carry.layerPartNames[0] == "L0001" &&
                loaded.carry.layerPartNames[1] == "L0002" &&
                loaded.carry.layerPartNames[2] == "L0003",
            "part naming: stable synthetic ids in layer order, one-based, as "
            "docs/document-format.md requires");

      // --- Document-level attributes -------------------------------------
      check(loaded.carry.sourceVersion == kNpaintFormatVersion,
            "np:version: written and read back");
      check(loaded.carry.basis == kNpaintPigmentBasis, "np:basis: written and read back");
      const Primaries& p = loaded.document.workingSpace.primaries;
      check(p.redX == kRec709Primaries.redX && p.greenY == kRec709Primaries.greenY &&
                p.whiteX == kRec709Primaries.whiteX && p.whiteY == kRec709Primaries.whiteY,
            "I6: the working space's primaries survive through the standard EXR "
            "`chromaticities` attribute");
    }

    // --- PRD I12: part 0 is regenerated, never stale ---------------------
    {
      const DecodedImage expect = flattenDocumentToLinear(fixture);
      float worst = 0.0f;
      float worstOpaque = 0.0f;
      if (loaded.composite.valid() && expect.valid()) {
        for (uint32_t y = 0; y < expect.height; ++y) {
          for (uint32_t x = 0; x < expect.width; ++x) {
            const auto e = pixelOf(expect, x, y);
            const auto g = pixelOf(loaded.composite, x, y);
            for (int c = 0; c < 4; ++c) {
              const float d = std::fabs(e[c] - g[c]);
              worst = std::max(worst, d);
              if (e[3] == 1.0f || e[3] == 0.0f) worstOpaque = std::max(worstOpaque, d);
            }
          }
        }
      }
      std::printf("    [measured] composite vs. flattenDocumentToLinear: max residual = "
                  "%.3e (bound %.3e), and %.3e at alpha in {0,1} (expected exactly 0)\n",
                  worst, static_cast<double>(kCompositeTol) / 1.43, worstOpaque);
      check(loaded.composite.valid() && loaded.composite.width == 400 &&
                loaded.composite.height == 300,
            "I5b: part 0 is a full-canvas composite any EXR reader can show");
      check(worst <= kCompositeTol,
            "I5b: and it matches io/Export's flattener within the derived half tolerance");
      check(worstOpaque == 0.0f,
            "I5b: exactly, at every fully opaque or fully transparent pixel -- where the "
            "un-premultiply and re-association are both by exactly 1.0 and nothing rounds");

      // Now mutate a layer and save again. A composite that were merely
      // copied forward, or written once and left, would still show the old
      // pixel -- which is the failure docs/document-format.md §3.4 says
      // "nobody notices for months".
      const auto before = pixelOf(loaded.composite, 5, 7);
      writeStraight(fixture, 0, 5, 7, 0.0f, 1.0f, 0.25f, 1.0f);
      const NpaintSaveResult again = saveNpaint(fixture, kPath, {}, &loaded.carry);
      check(again.ok, "I12: the mutated document saves again over the same file");
      const NpaintLoadResult reloaded = loadNpaint(kPath);
      check(reloaded.ok, "I12: and reads back");
      if (reloaded.ok && reloaded.composite.valid()) {
        const auto after = pixelOf(reloaded.composite, 5, 7);
        const DecodedImage expect2 = flattenDocumentToLinear(fixture);
        const auto want = pixelOf(expect2, 5, 7);
        check(before[0] != after[0] || before[1] != after[1],
              "I12: the composite at the mutated pixel actually CHANGED -- a test that only "
              "checked part 0 exists would not catch a stale one");
        check(std::fabs(after[0] - want[0]) <= kCompositeTol &&
                  std::fabs(after[1] - want[1]) <= kCompositeTol &&
                  std::fabs(after[2] - want[2]) <= kCompositeTol,
              "I12: and it changed to match the NEW flattened document, not to something "
              "merely different");
        // And the layer tiles are still bit-exact after the second save, so
        // regenerating the composite did not disturb them.
        const uint16_t* src = storedWords(fixture, 0, TileCoord{0, 0});
        const Tile* dst = reloaded.document.layers[0].rgbTiles->find(TileCoord{0, 0});
        bool same = src && dst;
        if (same) {
          for (size_t w = 0; w < Tile::kTexelCount && same; ++w)
            same = src[w] == dst->data()[w];
        }
        check(same, "I12: and the layer tiles are still bit-exact after the second save");
      }
    }

    std::remove(kPath);
  }

  // --- PRD I10: unrecognised attributes and parts survive verbatim -------
  {
    const char* kPath = "selftest_npaint_carry.npaint";
    std::remove(kPath);

    // Everything here is deliberately something this build's code has no
    // knowledge of whatsoever: attribute names it never writes, on a part
    // whose np:kind it cannot hold, with channels it has never heard of.
    NpaintCarry carry;
    NpaintAttribute s;
    s.name = "np:futureComps";
    s.type = NpaintAttribute::Type::String;
    s.stringValue = "comp1:on,off,on|comp2:off,on,on";
    NpaintAttribute i;
    i.name = "np:futureRevision";
    i.type = NpaintAttribute::Type::Int;
    i.intValue = 1234567;
    NpaintAttribute fl;
    fl.name = "np:futureGamma";
    fl.type = NpaintAttribute::Type::Float;
    fl.floatValue = 2.4f;
    carry.documentAttributes = {s, i, fl};

    NpaintAttribute lm;
    lm.name = "np:futureMaskLink";
    lm.type = NpaintAttribute::Type::String;
    lm.stringValue = "M0007";
    carry.layerAttributes = {{lm}, {}, {}};

    // A future writer's Pigment layer: the exact case io/NpaintFile.hpp's
    // deferral list says must survive. `pig.c0` sorts before `pig.m`, which
    // matters -- OpenEXR stores channels in a sorted ChannelList, so a
    // fixture whose channel order is not already sorted would come back
    // reordered and the comparison would fail for a reason that has nothing
    // to do with preservation.
    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.x = 0;
    pigment.y = 0;
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    pigment.channelNames = {"pig.c0", "pig.m"};
    pigment.sampleTypeName = "float";
    {
      const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                              0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
      pigment.rawPixels.resize(sizeof(vals));
      std::memcpy(pigment.rawPixels.data(), vals, sizeof(vals));
    }
    NpaintAttribute pk;
    pk.name = "np:kind";
    pk.type = NpaintAttribute::Type::String;
    pk.stringValue = "Pigment";
    NpaintAttribute pm;
    pm.name = "np:medium";
    pm.type = NpaintAttribute::Type::String;
    pm.stringValue = "watercolour";
    pigment.attributes = {pk, pm};
    carry.rawParts = {pigment};

    // Interleaved on purpose: layer, then the foreign part, then layer.
    // Appending carried parts at the end would silently reorder the stack.
    carry.partOrder = {{NpaintPartSlot::Kind::Layer, 0},
                       {NpaintPartSlot::Kind::RawPart, 0},
                       {NpaintPartSlot::Kind::Layer, 1}};
    carry.layerPartNames = {"L0001", "L0003"};
    carry.basis = "future-basis-v9";

    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 1.0f);

    const NpaintSaveResult saved = saveNpaint(doc, kPath, {}, &carry);
    check(saved.ok && saved.partsWritten == 4,
          "I10: a document with one foreign part saves -- 4 parts (composite, 2 layers, the "
          "carried Pigment part)");

    const NpaintLoadResult back = loadNpaint(kPath);
    check(back.ok, "I10: and reads back");
    if (back.ok) {
      check(back.document.layers.size() == 2,
            "I10: the foreign part did NOT become a layer -- the reader is strict in its own "
            "disfavour rather than half-understanding it");
      bool warnedByName = false;
      for (const std::string& w : back.warnings) {
        if (w.find("L0002") != std::string::npos && w.find("Pigment") != std::string::npos)
          warnedByName = true;
      }
      check(warnedByName,
            "I10: and the load says so by name, naming the np:kind it could not hold");

      check(back.carry.documentAttributes.size() == 3,
            "I10: all three unrecognised document attributes came back");
      bool docAttrsExact = back.carry.documentAttributes.size() == 3;
      if (docAttrsExact) {
        // Order is EXR's own (sorted), so compare as a set by name.
        auto findByName = [&](const char* n) -> const NpaintAttribute* {
          for (const auto& a : back.carry.documentAttributes)
            if (a.name == n) return &a;
          return nullptr;
        };
        const NpaintAttribute* gs = findByName("np:futureComps");
        const NpaintAttribute* gi = findByName("np:futureRevision");
        const NpaintAttribute* gf = findByName("np:futureGamma");
        docAttrsExact = gs && gi && gf && *gs == s && *gi == i && *gf == fl;
      }
      check(docAttrsExact,
            "I10: each one byte-for-byte identical -- string, int and float, none of which "
            "this build's code knows anything about");

      check(back.carry.layerAttributes.size() == 2 &&
                back.carry.layerAttributes[0].size() == 1 &&
                back.carry.layerAttributes[0][0] == lm &&
                back.carry.layerAttributes[1].empty(),
            "I10: an unrecognised attribute on a *layer* part survives, attached to the "
            "right layer");

      check(back.carry.basis == "future-basis-v9",
            "I10: np:basis is preserved verbatim rather than overwritten with this build's");

      check(back.carry.rawParts.size() == 1 && back.carry.rawParts[0].name == "L0002" &&
                back.carry.rawParts[0].channelNames ==
                    std::vector<std::string>{"pig.c0", "pig.m"} &&
                back.carry.rawParts[0].sampleTypeName == "float" &&
                back.carry.rawParts[0].width == 4 && back.carry.rawParts[0].height == 2 &&
                back.carry.rawParts[0].rawPixels == pigment.rawPixels,
            "I10: the whole foreign part survives -- name, channel names, sample type, data "
            "window and every pixel byte");
      check(back.carry.rawParts.size() == 1 &&
                back.carry.rawParts[0].attributes.size() == 2,
            "I10: including both of its own np:* attributes");

      check(back.carry.partOrder.size() == 3 &&
                back.carry.partOrder[0].kind == NpaintPartSlot::Kind::Layer &&
                back.carry.partOrder[1].kind == NpaintPartSlot::Kind::RawPart &&
                back.carry.partOrder[2].kind == NpaintPartSlot::Kind::Layer,
            "I10: and it is still BETWEEN the two layers -- part order is layer order, so "
            "appending carried parts at the end would be data loss in the ordering");
      check(back.carry.layerPartNames.size() == 2 &&
                back.carry.layerPartNames[0] == "L0001" &&
                back.carry.layerPartNames[1] == "L0003",
            "I10: the layers kept their original part ids, so an np:parent link inside the "
            "foreign part still points where it did");

      // The second generation. One round trip proving preservation is a
      // weaker claim than it looks: it only shows the reader kept what the
      // writer had in hand. Saving the *loaded* carry and reading it again
      // proves the loop is closed.
      const char* kPath2 = "selftest_npaint_carry2.npaint";
      std::remove(kPath2);
      const NpaintSaveResult again = saveNpaint(back.document, kPath2, {}, &back.carry);
      check(again.ok && again.partsWritten == 4,
            "I10: the loaded carry saves straight back out");
      const NpaintLoadResult third = loadNpaint(kPath2);
      check(third.ok && third.carry.documentAttributes == back.carry.documentAttributes &&
                third.carry.layerAttributes == back.carry.layerAttributes &&
                third.carry.basis == back.carry.basis &&
                third.carry.rawParts.size() == 1 &&
                third.carry.rawParts[0].rawPixels == pigment.rawPixels &&
                third.carry.rawParts[0].attributes == back.carry.rawParts[0].attributes,
            "I10: and a SECOND generation is still identical -- an older build can open a "
            "newer document, edit it, save it, and destroy nothing");
      std::remove(kPath2);
    }
    std::remove(kPath);
  }

  // --- PRD I8: `.npaint` and `.exr` are the same container ----------------
  {
    const char* kExr = "selftest_npaint_as.exr";
    std::remove(kExr);
    const NpaintSaveResult saved = saveNpaint(fixture, kExr);
    check(saved.ok, "I8: the same document saves under a .exr name");
    const NpaintLoadResult back = loadNpaint(kExr);
    check(back.ok && back.document.layers.size() == 3,
          "I8: and reads back identically -- the writer is chosen by format name, never by "
          "the path's extension, so handoff really is a rename");
    std::remove(kExr);
  }

  // --- Alpha lock (`np:alphaLocked`): round-trips PER LAYER, and only costs
  //     a byte when true -------------------------------------------------
  //
  // `np:clipped`'s own two-part rule (this file's own writer comment states
  // it, and core/Layer.hpp's `alphaLocked` comment repeats it), checked the
  // way LayerMask.cpp's own byte-identity block checks the mask channel's: a
  // per-layer value round-trips, AND a document with the flag false
  // everywhere produces the SAME bytes as one that never had the attribute
  // at all -- measured against a real save, not assumed.
  {
    const char* kBare = "selftest_npaint_alphalock_bare.npaint";
    const char* kPath = "selftest_npaint_alphalock.npaint";
    const char* kAgain = "selftest_npaint_alphalock_again.npaint";
    for (const char* p : {kBare, kPath, kAgain}) std::remove(p);

    // Blanks OpenImageIO's `capDate` header attribute so two saves made in
    // different seconds compare equal on everything else -- LayerMask.cpp's
    // own helper, kept local rather than shared: Support.hpp has no home for
    // a file-comparison utility and every existing caller re-derives it in
    // place rather than reach for one that does not exist yet.
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

    Document doc = Document::createBlank(128, 128, WorkingSpace{});
    addLayer(doc, 1, makeRgbLayer("Locked alpha"));
    addLayer(doc, 2, makeRgbLayer("Plain"));
    writeStraight(doc, 0, 5, 5, 0.4f, 0.2f, 0.1f, 1.0f);
    writeStraight(doc, 1, 6, 6, 0.1f, 0.5f, 0.9f, 0.6f);
    writeStraight(doc, 2, 7, 7, 0.3f, 0.3f, 0.3f, 1.0f);

    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok, "np:alphaLocked: a three-layer document with no alpha-locked layer saves");

    doc.layers[1].alphaLocked = true;
    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok, "np:alphaLocked: the same document with layer 1 alpha-locked saves");

    const NpaintLoadResult back = loadNpaint(kPath);
    check(back.ok && back.document.layers.size() == 3 && !back.document.layers[0].alphaLocked &&
              back.document.layers[1].alphaLocked && !back.document.layers[2].alphaLocked,
          "np:alphaLocked: round-trips PER LAYER -- true on layer 1 only, false on the two "
          "either side of it, so this cannot pass on a value that round-tripped as a "
          "document-wide default instead of a per-layer attribute");

    check(bytesWithoutCapDate(kPath).size() > bytesWithoutCapDate(kBare).size(),
          "np:alphaLocked: the alpha-locked file really is BIGGER than the flag-free one, so "
          "the byte-identity check below is not passing because nothing was ever written");

    // The property docs/document-format.md requires and this task's own brief
    // repeats: clearing the flag again gives back the SAME bytes as a file
    // that never had it set.
    doc.layers[1].alphaLocked = false;
    const NpaintSaveResult again = saveNpaint(doc, kAgain);
    check(again.ok && bytesWithoutCapDate(kAgain) == bytesWithoutCapDate(kBare) &&
              !bytesWithoutCapDate(kBare).empty(),
          "np:alphaLocked: turning the flag back off gives a file BYTE-IDENTICAL to one that "
          "never had it -- `np:alphaLocked` is written only when true, so a document with the "
          "flag false everywhere (every `.npaint` this build wrote before this step, and any "
          "document saved after it with the flag never set) produces exactly the bytes it "
          "always did. This is the assertion sabotage (b) in this task's brief reddens.");

    for (const char* p : {kBare, kPath, kAgain}) std::remove(p);
  }

  // Scratch files: every path this section touches, removed unconditionally,
  // whether or not the assertion that created it passed.
  for (const char* p : {"selftest_npaint_never.npaint", "selftest_npaint_gate.npaint",
                        "selftest_npaint_roundtrip.npaint", "selftest_npaint_carry.npaint",
                        "selftest_npaint_carry2.npaint", "selftest_npaint_as.exr",
                        "selftest_npaint_alphalock_bare.npaint", "selftest_npaint_alphalock.npaint",
                        "selftest_npaint_alphalock_again.npaint"}) {
    std::remove(p);
  }

  std::printf("[selftest] npaint format %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
