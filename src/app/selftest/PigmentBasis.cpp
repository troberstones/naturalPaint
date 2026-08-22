#include "app/selftest/Support.hpp"

namespace np {

// ==========================================================================
// Phase 5 step 15 -- the pigment basis stamped in the document and the file,
// and the export warnings that never reached a caller.
// ==========================================================================
bool runPigmentBasisTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Which backend set was compiled in, following runNpaintFormatTest()'s own
  // precedent and PLAN.md §1.5 ("an unexercised build option is not a seam"):
  // every assertion below runs in both configurations and states the correct
  // answer for the one it is in. The OFF build's answer for the file half is
  // that `saveNpaint()`/`loadNpaint()` refuse by name, which is asserted here
  // rather than compiled out.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto writePigment = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                         const Latent& z, float mass) {
    PigmentTileStore& tiles = *doc.layers[layerIndex].pigmentTiles;
    const PixelCoord at{x, y};
    PigmentTexel t;
    t.latent = z;
    t.mass = mass;
    tiles.getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), t);
  };
  auto readPigment = [](const Document& doc, size_t layerIndex, int32_t x,
                        int32_t y) -> PigmentTexel {
    const PixelCoord at{x, y};
    const PigmentTile* tile = doc.layers[layerIndex].pigmentTiles->find(tileCoordAt(at));
    return tile ? tile->readTexel(tileLocalOffset(at)) : PigmentTexel{};
  };
  // Every component exactly representable in binary16, so the round trip below
  // can be asserted at zero tolerance without borrowing paint/MixboxLut (and
  // therefore without needing the LUT file to be present).
  const Latent zExact{{0.5f, 0.25f, 0.125f}, {0.0625f, -0.125f, 0.75f}};

  // --- The file comparator, and why it cannot be a plain memcmp -----------
  //
  // **Measured here, 2026-08-21, and it is a fact about this format nothing in
  // the codebase had written down: a `.npaint` is not byte-reproducible.**
  // OpenEXR stamps a `capDate` header attribute -- "YYYY:MM:DD HH:MM:SS", wall
  // clock, one per part -- so two saves of the identical document one second
  // apart differ in three places. Everything else is deterministic: with those
  // three fields masked, two saves of the same document from the same binary
  // are byte-for-byte equal, which is asserted below rather than assumed.
  //
  // So "byte-identical" for this format means "identical once capDate is
  // masked", and the masking is done by pattern rather than by offset because
  // the offsets move with the header.
  auto readFile = [](const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  auto sameFileBytes = [&](const char* a, const char* b) {
    const std::string ba = readFile(a), bb = readFile(b);
    return !ba.empty() && maskCapDates(ba) == maskCapDates(bb);
  };

  // ======================================================================
  // (1) The field on the document. Both builds; no file is involved.
  // ======================================================================

  check(std::string(kPigmentBasisMixbox) == "mixbox-v1" &&
            Document{}.pigmentBasis == kPigmentBasisMixbox &&
            Document::createBlank(4, 4, WorkingSpace{}).pigmentBasis == kPigmentBasisMixbox,
        "basis: every document this build makes declares \"mixbox-v1\" -- the default is on "
        "the member, so createBlank() and a bare Document cannot disagree");

  // Pointer equality, not string equality. String equality would still pass if
  // io/NpaintFile.hpp went back to spelling the literal a second time, which is
  // exactly the drift the alias exists to prevent: two constants that must
  // agree and are only checked by eye.
  check(static_cast<const void*>(kNpaintPigmentBasis) ==
            static_cast<const void*>(kPigmentBasisMixbox),
        "basis: io/NpaintFile's kNpaintPigmentBasis IS core/Document's constant -- the same "
        "object, not a second literal that happens to match today");

  // The placement cost, measured rather than asserted from memory: the string
  // fits in libc++'s short-string buffer, so `core::History`'s per-entry
  // `Document` copy does not allocate for this member. Deterministic (a
  // property of this standard library and this literal), hence no [measured].
  const size_t ssoCapacity = std::string().capacity();
  std::printf("    basis: \"%s\" is %zu bytes; std::string SSO capacity is %zu\n",
              kPigmentBasisMixbox, std::strlen(kPigmentBasisMixbox), ssoCapacity);
  check(std::strlen(kPigmentBasisMixbox) <= ssoCapacity,
        "basis: the default value is short-string-optimised, so putting it on Document costs "
        "a history entry sizeof(std::string) and zero allocations");

  // It travels with the pixels through undo, which is the whole argument for
  // putting it on Document rather than on app::OpenDocument. `core::History`
  // holds each entry's Document by value, so this needs no support anywhere in
  // History -- but "needs no code" is exactly the kind of claim that stops
  // being true silently.
  {
    History h;
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    doc.pigmentBasis = "km2-v1";
    h.begin("opened", doc);
    Document edited = doc;
    edited.pigmentBasis = kPigmentBasisMixbox;
    h.record("converted the basis", edited);
    const Document* back = h.undo();
    check(back != nullptr && back->pigmentBasis == "km2-v1",
          "basis: undo restores it with the tiles it describes -- on app::OpenDocument it "
          "would sit outside every history entry and undo could not reach it");
    const Document* fwd = h.redo();
    check(fwd != nullptr && fwd->pigmentBasis == kPigmentBasisMixbox,
          "basis: and redo puts it back, so it is ordinary document state and not a special "
          "case anywhere in core/History");
  }

  // ======================================================================
  // (2) The file. Stamped from the document, read back onto the document.
  // ======================================================================

  const char* kPlain = "selftest_basis_plain.npaint";
  const char* kPlainAgain = "selftest_basis_plain2.npaint";
  const char* kViaCarry = "selftest_basis_viacarry.npaint";
  const char* kOtherBasis = "selftest_basis_other.npaint";
  const char* kForeign = "selftest_basis_foreign.npaint";
  const char* kForeignAgain = "selftest_basis_foreign2.npaint";
  const char* kPigment = "selftest_basis_pigment.npaint";
  const char* kPigmentAgain = "selftest_basis_pigment2.npaint";
  for (const char* p : {kPlain, kPlainAgain, kViaCarry, kOtherBasis, kForeign, kForeignAgain,
                        kPigment, kPigmentAgain})
    std::remove(p);

  // "A document with nothing unusual in it" -- two RGB layers, a name, an
  // opacity, a blend, a few written texels. Deliberately ordinary: this is the
  // fixture the byte-identity claim below is made about.
  auto plainDocument = [&]() {
    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    doc.layers[0].opacity = 0.75f;
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    second.blend = "multiply";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 0.5f);
    return doc;
  };

  {
    const Document doc = plainDocument();
    const NpaintSaveResult saved = saveNpaint(doc, kPlain);
    check(saved.ok == kOiioBuild,
          kOiioBuild ? "basis: an ordinary document saves"
                     : "basis: an ordinary document's save refuses -- no OpenImageIO in this "
                       "build, which is the OFF build's correct answer, not a skipped test");
    if (!kOiioBuild) {
      check(contains(saved.error, ".npaint") && contains(saved.error, "NP_USE_OIIO"),
            "basis: and the refusal names the format and the cmake option that enables it, "
            "so the whole basis body below is a refusal against a real entry point");
      const NpaintLoadResult loaded = loadNpaint(kPlain);
      check(!loaded.ok && contains(loaded.error, ".npaint") &&
                contains(loaded.error, "NP_USE_OIIO"),
            "basis: and so does the read direction");
    }
  }

  if (kOiioBuild) {
    // --- The stamp round-trips, on the carry AND on the document ---------
    {
      const Document doc = plainDocument();
      const NpaintLoadResult back = loadNpaint(kPlain);
      check(back.ok && back.carry.basis == kPigmentBasisMixbox &&
                back.document.pigmentBasis == kPigmentBasisMixbox,
            "basis: np:basis comes back on BOTH -- the carry records what the file said, the "
            "document records what its own latents are (PRD C8)");
      check(back.ok && back.warnings.empty(),
            "basis: and this build's own basis produces no warning");
      check(back.ok && back.document.pigmentBasis == doc.pigmentBasis,
            "basis: so a save/load leaves the document's basis where it started");
    }

    // --- Byte-identity: extending the format, not altering it ------------
    //
    // The value written now comes from `doc.pigmentBasis` instead of from
    // io/NpaintFile's constant. If that moved a single byte of an ordinary
    // document's file, the step would have changed the format rather than
    // extended it. Proven three ways, because each alone is weak:
    //
    //   * two saves of the same document are identical (the comparator is
    //     stable, and the capDate masking above is sufficient);
    //   * the document-sourced write and a carry-sourced write of the *same*
    //     string are identical (the new source of the string changes nothing);
    //   * a same-length *different* basis is NOT identical (the comparator can
    //     actually see the basis bytes, so the two passes above are not
    //     vacuous).
    {
      const Document doc = plainDocument();
      saveNpaint(doc, kPlainAgain);
      check(sameFileBytes(kPlain, kPlainAgain),
            "basis: two saves of one document are byte-identical once OpenEXR's per-part "
            "capDate is masked -- the format is deterministic apart from that clock");

      NpaintCarry sameBasis;
      sameBasis.basis = kPigmentBasisMixbox;
      const NpaintSaveResult viaCarry = saveNpaint(doc, kViaCarry, {}, &sameBasis);
      check(viaCarry.ok && sameFileBytes(kPlain, kViaCarry),
            "basis: writing the basis FROM THE DOCUMENT produces the same bytes as writing it "
            "from a carry -- an ordinary document's file is untouched by this step");

      NpaintCarry otherBasis;
      otherBasis.basis = "mixbox-v2";  // same length, so only the value differs
      const NpaintSaveResult viaOther = saveNpaint(doc, kOtherBasis, {}, &otherBasis);
      check(viaOther.ok && !sameFileBytes(kPlain, kOtherBasis),
            "basis: and a different basis of the SAME LENGTH does change the bytes -- the "
            "comparator can see the attribute, so the two claims above are not vacuous");
    }

    // --- A basis this build cannot interpret -----------------------------
    //
    // Built by handing saveNpaint() a carry, which is the only way to
    // manufacture one: nothing in this build writes a basis other than its own.
    {
      NpaintCarry foreign;
      foreign.basis = "km2-v1";
      const Document doc = plainDocument();
      check(saveNpaint(doc, kForeign, {}, &foreign).ok,
            "basis: a foreign basis can be written at all -- an RGB-only document carries one "
            "through untouched (PRD I10), which is what makes the case below reachable");

      const NpaintLoadResult back = loadNpaint(kForeign);
      check(back.ok, "basis: a file declaring a basis this build has never heard of LOADS -- "
                     "refusing would make a document unopenable over a label");
      if (back.ok) {
        check(back.document.pigmentBasis == "km2-v1" && back.carry.basis == "km2-v1",
              "basis: and the document keeps that string verbatim rather than being "
              "relabelled with this build's");
        bool named = false;
        for (const std::string& w : back.warnings)
          if (contains(w, "km2-v1") && contains(w, kPigmentBasisMixbox)) named = true;
        check(named,
              "basis: warned by name, naming both the file's basis and this build's -- loud, "
              "not silent, and not fatal");

        // The requirement in its own words: "a file written by some future
        // build with a different basis round-trips its own basis rather than
        // being silently relabelled by this one". Saved with **no carry at
        // all**, so the document is the only thing the value can have come
        // from -- which is precisely what a constant could not do.
        const NpaintSaveResult again = saveNpaint(back.document, kForeignAgain);
        check(again.ok, "basis: a foreign-basis document saves back out with no carry");
        const NpaintLoadResult third = loadNpaint(kForeignAgain);
        check(third.ok && third.document.pigmentBasis == "km2-v1" &&
                  third.carry.basis == "km2-v1",
              "basis: still \"km2-v1\" after a second generation written from the DOCUMENT "
              "alone -- the constant is gone from the write path");
      }
    }

    // --- Latents in a foreign basis: the case the step actually decides ---
    //
    // The trap this replaces: between step 3 and step 15 the refusal compared
    // the carry against this build's *constant*, so a document loaded from a
    // foreign-basis file with real Pigment layers opened, accepted edits, and
    // could never be written back -- app/Journal's crash checkpoint included,
    // since that checkpoint is a saveNpaint(). Both readings are exercised
    // here, the built one and the rejected one, rather than only the built one.
    {
      Document doc = Document::createBlank(64, 64, WorkingSpace{});
      doc.layers[0] = makePigmentLayer("wash");
      writePigment(doc, 0, 1, 1, zExact, 0.5f);
      doc.pigmentBasis = "km2-v1";

      NpaintCarry agreeing;
      agreeing.basis = "km2-v1";
      const NpaintSaveResult saved = saveNpaint(doc, kPigment, {}, &agreeing);
      check(saved.ok,
            "basis: a document whose Pigment latents and whose source file agree on a foreign "
            "basis SAVES -- it is coherent, and refusing it would trap the user's work behind "
            "a label");

      // The rejected reading, run beside the built one: compare the carry
      // against this build's constant instead of against the document, which
      // is what the code did before this step.
      Document relabelled = doc;
      relabelled.pigmentBasis = kPigmentBasisMixbox;
      const NpaintSaveResult mixed = saveNpaint(relabelled, kPigmentAgain, {}, &agreeing);
      check(!mixed.ok && contains(mixed.error, "km2-v1") &&
                contains(mixed.error, kPigmentBasisMixbox),
            "basis: but a document claiming THIS build's basis for latents whose file says "
            "km2-v1 is still refused by name -- one file, two bases, no honest label");

      const NpaintLoadResult back = loadNpaint(kPigment);
      check(back.ok && back.document.pigmentBasis == "km2-v1",
            "basis: the saved foreign-basis Pigment document reads back declaring km2-v1");
      if (back.ok && back.document.layers.size() == 1 &&
          back.document.layers[0].pigmentTiles.has_value()) {
        const PigmentTexel got = readPigment(back.document, 0, 1, 1);
        check(got.latent == zExact && got.mass == 0.5f,
              "basis: with its latents bit-identical at zero tolerance -- the label travelled "
              "and the numbers were never reinterpreted");
      } else {
        check(false, "basis: with its latents bit-identical at zero tolerance -- the label "
                     "travelled and the numbers were never reinterpreted");
      }
    }

    // --- An empty basis is refused, not written -------------------------
    {
      Document doc = Document::createBlank(8, 8, WorkingSpace{});
      doc.pigmentBasis.clear();
      const NpaintSaveResult refused = saveNpaint(doc, kPigmentAgain);
      check(!refused.ok && contains(refused.error, "pigmentBasis") &&
                contains(refused.error, kPigmentBasisMixbox),
            "basis: an empty basis is refused by name -- this OpenImageIO drops empty string "
            "attributes, so the file would declare no basis rather than an unknown one");
    }
  }

  for (const char* p : {kPlain, kPlainAgain, kViaCarry, kOtherBasis, kForeign, kForeignAgain,
                        kPigment, kPigmentAgain})
    std::remove(p);

  // ======================================================================
  // (3) The export warnings that never reached a caller.
  // ======================================================================
  //
  // Adjacent debt fixed in the same step, and nothing to do with the basis:
  // `exportDocumentWithRequest()` called the one-argument
  // `flattenDocumentToLinear()`, so `ExportResult::warnings` was **always
  // empty** from it -- every Export As, and every item io/ExportStates drove
  // through it, reported no warnings whatever the document held.
  // io/ExportStates.hpp had the gap written down and deferred. No OpenImageIO
  // anywhere in this block: PNG is stb-backed in both configurations.
  {
    auto docWithApproximatedBlend = [&](const char* blend) {
      Document doc = Document::createBlank(8, 8, WorkingSpace{});
      Layer top;
      top.kind = LayerKind::RGB;
      top.rgbTiles.emplace();
      top.name = "Line pass";
      top.blend = blend;
      doc.layers.push_back(std::move(top));
      writeStraight(doc, 0, 1, 1, 0.5f, 0.0f, 0.0f, 1.0f);
      writeStraight(doc, 1, 1, 1, 0.0f, 0.0f, 0.5f, 1.0f);
      return doc;
    };

    ExportRequest png8;
    png8.format = ImageFormat::Png;
    png8.targetSpace = ExportTargetSpace::Rec709Srgb;
    png8.bitDepth = ExportBitDepth::UInt8;

    const ExportResult plain = exportDocumentWithRequest(plainDocument(), png8);
    check(plain.ok && plain.warnings.empty(),
          "export warnings: a document this build composites faithfully still exports with no warnings "
          "-- the negative control, so the next assertion is about the document and not about "
          "the plumbing");

    const ExportResult odd = exportDocumentWithRequest(docWithApproximatedBlend("linear-burn"),
                                                       png8);
    check(odd.ok && !odd.warnings.empty() && contains(odd.warnings[0], "linear-burn") &&
              contains(odd.warnings[0], "Line pass"),
          "export warnings: and a blend this build only approximates now REACHES the caller through "
          "exportDocumentWithRequest(), naming the layer and the value (was always empty)");

    // Carried on a refusal too, which is io/Export's `exportDocument()` own
    // rule and its own reason: fixing the rejected request must not be how you
    // find out the composite was approximate.
    ExportRequest tooBig = png8;
    tooBig.resize.mode = ExportResizeMode::Percent;
    tooBig.resize.percent = 400.0f;
    const ExportResult refused =
        exportDocumentWithRequest(docWithApproximatedBlend("linear-burn"), tooBig);
    check(!refused.ok && !refused.warnings.empty() &&
              contains(refused.warnings[0], "linear-burn"),
          "export warnings: and on a REFUSED request too -- a user who fixes the resize and re-exports "
          "must not learn about the approximation only then");

    // The other entry point was already correct and must stay so: this step
    // fixed one caller, not the flattener.
    const ExportResult direct = exportDocument(docWithApproximatedBlend("linear-burn"),
                                               ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt8);
    check(direct.ok && direct.warnings.size() == odd.warnings.size() &&
              !direct.warnings.empty() && direct.warnings[0] == odd.warnings[0],
          "export warnings: exportDocument() reported this all along, and the two paths now say the "
          "same thing about the same document");
  }

  std::printf("[selftest] pigment basis %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
