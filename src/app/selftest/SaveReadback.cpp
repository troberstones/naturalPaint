#include "app/selftest/Support.hpp"

#include "app/CloseDecision.hpp"

namespace np {

// ============================================================================
// Reachability audit D5 / PRD I13 (P1) -- "Saves are read back and
// structurally verified before the original leaves memory." See
// io/NpaintFile.hpp's NpaintSaveOptions::verifyReadback and
// verifyNpaintRoundTrip() for the design argument; this section is the proof
// it holds against real files rather than against constants.
//
// **The headline finding this step surfaced, stated where the assertions
// prove it rather than only in a comment: `saveNpaint()` writes in place.**
// With `verifyReadback` unset -- every caller before this step, and
// app/Journal.cpp's crash checkpoint still today -- `oiioWriteMultiPartExr()`
// opens `path` directly, and its own failure path removes `path` outright
// ("never leave a partial document behind"). A write that dies partway does
// not merely fail to add a new version; it deletes whatever good version was
// already there. Section D proves the mechanism -- a forced write failure
// against a file with a known-good previous version -- and section A proves
// the fix: with `verifyReadback` set, the same failure cannot reach `path` at
// all, because nothing is ever opened there until a verified replacement is
// ready to be renamed in.
//
// Covered, in order:
//
//  A. A good save through the real path (saveDocumentAs(), which now forces
//     verification on for every explicit save) verifies, is marked clean,
//     and the close guard stops asking about it. The temp file is gone --
//     renamed, not abandoned. And the one deliberately-allowed difference --
//     an all-zero tile allocated in memory coming back *absent* -- is proven
//     rather than merely excluded: the tile really does disappear, and
//     verifyNpaintRoundTrip() still calls the file good, because it is the
//     decoded *value* the rule protects, not tile presence.
//  B. The assertion a hash cannot make: two different, individually valid,
//     fully intact documents saved to the same path. verifyNpaintRoundTrip()
//     catches document A's expectations held against document B's bytes --
//     there is no corruption anywhere for a hash to trip over, only a file
//     that is not the one that was asked for.
//  C. A truncated file -- real corruption, on disk, after a good save.
//     verifyNpaintRoundTrip() refuses it; a byte-level read (what
//     app/Journal.cpp's own hash check actually does) reads every remaining
//     byte successfully and would report nothing wrong.
//  D. A forced write failure at the exact moment `saveNpaint()` would open
//     its temp file, against a path that already holds a known-good file.
//     The good file's size is unchanged byte for byte -- the failure never
//     reached it.
//  E. A forced rename failure *after* a successful write and a successful
//     verify -- the fourth and last place this can go wrong. Whatever was at
//     the destination stays exactly what it was.
//  F. The same guarantee one layer up, through saveDocumentAs(): a save
//     failure -- any save failure, because the function does not and should
//     not distinguish "the write failed" from "the write succeeded but did
//     not verify", the same one-question shape app/CloseDecision.hpp's
//     `closeNeedsDecision()` already uses -- leaves the document dirty, the
//     in-memory edit untouched, and the close guard still asking about it.
//  G. A regression case for a real defect writing this section caught: a
//     carried, foreign pigment basis on an RGB-only document is a legitimate
//     save (PRD I10), not a corruption, and must not fail its own
//     verification just because it does not match the document's own
//     default label.
//
// Gated on oiioBackendCompiledIn() and prints a skip line rather than going
// quiet if it is false, matching runOpenAnyFileTest()'s and
// runNpaintFormatTest()'s own convention (io/NpaintFile needs OpenImageIO for
// every entry point this section calls) -- PLAN.md 1.5's "an unexercised
// build option is not a seam" (.claude/AGENT-BRIEF.md rule 2). In practice
// this build always has it: the top-level CMakeLists.txt now defines
// NP_USE_OIIO unconditionally and NP_USE_OIIO=OFF is "no longer supported",
// so the branch below is not reachable from a build this project currently
// produces -- kept anyway, at the cost of one `if`, because that is exactly
// the situation the rule is written for.
// ============================================================================
bool runSaveReadbackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  if (!oiioBackendCompiledIn()) {
    std::printf("  %-58s %s\n", "save readback: skipped, this build has no .npaint writer",
                "n/a");
    std::printf("[selftest] save readback PASS\n");
    return true;
  }

  namespace fs = std::filesystem;
  std::error_code ec;

  // All scratch state lives in one directory, removed at both ends of this
  // function -- app/selftest/DocumentLifecycle.cpp's own convention, and for
  // the same reason: several of the checks below plant a directory at a path
  // this build's writer wants to use, and a stale one from a previous failed
  // run must not silently change what a later run proves.
  const std::string dir = "selftest_savereadback";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const auto inDir = [&](const char* name) { return dir + "/" + std::string(name); };

  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto writePigment = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, const Latent& z,
                         float mass) {
    PigmentTileStore& tiles = *doc.layers[layerIndex].pigmentTiles;
    const PixelCoord at{x, y};
    PigmentTexel t;
    t.latent = z;
    t.mass = mass;
    tiles.getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), t);
  };

  // A fixture rich enough to exercise every property verifyNpaintRoundTrip()
  // claims to compare: two layer kinds, a mask, an alpha channel, distinct
  // per-layer metadata -- and one tile deliberately left allocated but never
  // painted, which is what section A's identity-tile assertion needs.
  auto buildFixture = [&]() {
    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Base";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.6f;
    doc.layers[0].visible = true;
    doc.layers[0].locked = true;
    writeStraight(doc, 0, 10, 10, 0.5f, 0.25f, 0.1f, 1.0f);
    // Allocated, never written: every word is the RGB identity (zero). Tile
    // (5,0) is document pixels [640,768) -- nowhere near the painted texel
    // above, so this is genuinely its own tile.
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{5, 0});

    Layer glaze;
    glaze.kind = LayerKind::Pigment;
    glaze.pigmentTiles.emplace();
    glaze.name = "Glaze";
    glaze.opacity = 0.85f;
    glaze.visible = true;
    doc.layers.push_back(std::move(glaze));
    writePigment(doc, 1, 20, 20, Latent{{0.2f, 0.3f, 0.1f}, {0.05f, 0.02f, 0.01f}}, 0.8f);

    AlphaChannel selection;
    selection.name = "Selection A";
    selection.tiles.getOrCreate(TileCoord{1, 1}).writeCoverage(PixelCoord{7, 7}, 1.0f);
    doc.channels.push_back(std::move(selection));

    return doc;
  };

  // --- A. the real path: saveDocumentAs() verifies, and marks clean --------
  std::printf("-- A. a good save verifies, and the document is marked clean --\n");
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = buildFixture();
    check(addLayerMask(doc.document, 1).ok, "fixture: the Glaze layer gets a mask");
    doc.document.layers[1].mask->getOrCreate(TileCoord{0, 0}).writeCoverage(PixelCoord{3, 3},
                                                                            0.4f);

    const std::string path = inDir("painting.npaint");
    const DocumentOpResult saved = saveDocumentAs(doc, path, {}, nullptr);
    check(saved.ok, "saveDocumentAs() with a good document succeeds");
    check(!doc.isDirty(), "...and the document is marked clean");
    check(!closeNeedsDecision(doc), "...so the close guard does not ask about it");

    const std::string tempSibling = inDir("painting.tmp.npaint");
    check(!fs::exists(tempSibling, ec),
          "the verified save's temporary file is gone -- renamed into place, not left behind");

    const NpaintLoadResult reloaded = loadNpaint(path);
    check(reloaded.ok && reloaded.document.layers.size() == 2 &&
              reloaded.document.layers[0].kind == LayerKind::RGB &&
              reloaded.document.layers[1].kind == LayerKind::Pigment,
          "the saved file reads back as the two-layer document that was saved");
    check(reloaded.ok && reloaded.document.layers[1].mask.has_value(),
          "...with its mask");
    check(reloaded.ok && reloaded.document.channels.size() == 1 &&
              reloaded.document.channels[0].name == "Selection A",
          "...and its alpha channel");

    // The one legitimate difference, proven rather than only excluded from
    // the comparison: an all-zero tile allocated in memory does NOT survive
    // as an allocated tile --
    check(reloaded.ok &&
              reloaded.document.layers[0].rgbTiles->find(TileCoord{5, 0}) == nullptr,
          "an all-zero tile that was allocated in memory reads back ABSENT -- io/NpaintFile's "
          "own drop-on-read rule, not data loss");
    // -- and verifyNpaintRoundTrip(), asked about the very same file, still
    // calls the save good, because presence is not the thing it protects;
    // the decoded value is, and an absent tile decodes to the same zero the
    // allocated one held.
    const NpaintVerifyResult reverified = verifyNpaintRoundTrip(doc.document, path);
    check(reverified.ok,
          "...and verifyNpaintRoundTrip() still calls this file good -- the identity-tile rule "
          "is asserted, not merely relied on to stay out of the way");
  }

  // --- B. the case a hash cannot see: a well-formed file, wrong document ---
  std::printf("-- B. a hash cannot catch a well-formed file holding the WRONG document --\n");
  {
    const Document docA = buildFixture();
    const Document docB = Document::createBlank(64, 64, WorkingSpace{});
    const std::string sharedPath = inDir("shared.npaint");

    // A perfectly ordinary, unverified save of document B -- the write this
    // module has always done, and there is nothing wrong with the bytes it
    // produces.
    const NpaintSaveResult savedB = saveNpaint(docB, sharedPath);
    check(savedB.ok, "fixture: document B saves cleanly to the shared path");
    const NpaintLoadResult reopenB = loadNpaint(sharedPath);
    check(reopenB.ok,
          "fixture: the file at that path is completely intact and readable -- nothing about "
          "it looks wrong, and a hash of it is well-defined and stable");

    const NpaintVerifyResult mismatch = verifyNpaintRoundTrip(docA, sharedPath);
    check(!mismatch.ok,
          "verifyNpaintRoundTrip() catches document A's expectations held against document B's "
          "intact, valid bytes -- the case a hash cannot see, because nothing here is "
          "corrupted");
    check(contains(mismatch.error, sharedPath.c_str()), "...and the message names the file");
  }

  // --- C. real corruption: truncation, and what a hash would have missed ---
  std::printf("-- C. a truncated file fails verification --\n");
  {
    const Document docC = buildFixture();
    const std::string truncPath = inDir("truncated.npaint");
    check(saveNpaint(docC, truncPath).ok, "fixture: a good file to truncate");

    const uintmax_t fullSize = fs::file_size(truncPath, ec);
    fs::resize_file(truncPath, fullSize / 2, ec);
    check(!ec && fullSize > 0, "fixture: the file was truncated to half its size");

    // What app/Journal.cpp's own hash check actually does: open the file and
    // read whatever bytes are there. A truncated file opens and reads to end
    // of file without a single I/O error -- fewer bytes, every one of them
    // readable -- which is exactly why a hash catches this case only if it
    // is compared against a hash recorded before the truncation, and proves
    // nothing about whether the file was ever structurally right to begin
    // with (section B).
    {
      std::ifstream probe(truncPath, std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(probe)),
                                    std::istreambuf_iterator<char>());
      check(!probe.bad() && !bytes.empty(),
            "a byte-level read of the truncated file completes with no I/O error -- there is "
            "nothing here for a hash-only check to trip over");
    }

    const NpaintVerifyResult truncResult = verifyNpaintRoundTrip(docC, truncPath);
    check(!truncResult.ok, "verifyNpaintRoundTrip() refuses the truncated file");
    check(contains(truncResult.error, truncPath.c_str()), "...and names it");
  }

  // --- D. a forced write failure leaves a known-good file untouched --------
  std::printf("-- D. a write failure through the verified path leaves the previous file alone "
              "--\n");
  {
    const Document good = buildFixture();
    const std::string protectedPath = inDir("protected.npaint");
    check(saveNpaint(good, protectedPath).ok, "fixture: a good file exists at the target path");
    const uintmax_t goodSize = fs::file_size(protectedPath, ec);
    check(goodSize > 0, "fixture: it has real content");

    // Force the write itself to fail, deterministically and without relying
    // on a permission bit (which would depend on whether this suite happens
    // to run as root): pre-occupy the exact temporary sibling saveNpaint()
    // computes for this path with a DIRECTORY. Opening a directory for
    // writing fails at the OS level on every platform this project targets,
    // well before OpenImageIO is even asked to do anything -- this mirrors
    // io/NpaintFile.cpp's own npaintTempSiblingPath() naming rule (the
    // stem, then ".tmp", then the original extension, in the same
    // directory), which is an internal, unexposed helper rather than a
    // seam a test can call -- so the three-line rule is repeated here
    // rather than exercised through an entry point that does not exist.
    const std::string collidingTemp = inDir("protected.tmp.npaint");
    fs::create_directory(collidingTemp, ec);
    check(!ec, "fixture: the colliding directory was created");

    NpaintSaveOptions verifyOpt;
    verifyOpt.verifyReadback = true;
    const NpaintSaveResult blocked = saveNpaint(good, protectedPath, verifyOpt);
    check(!blocked.ok && !blocked.verificationFailed,
          "the write itself refuses -- verificationFailed is false, because it never got that "
          "far");
    check(contains(blocked.error, protectedPath.c_str()), "...and the message names the target file");
    check(fs::file_size(protectedPath, ec) == goodSize,
          "...and the file that was already at that path is untouched, byte count and all");

    fs::remove_all(collidingTemp, ec);
  }

  // --- E. a forced rename failure also leaves the previous file alone ------
  std::printf("-- E. a rename failure after a successful verified write also leaves the "
              "destination alone --\n");
  {
    const Document good = buildFixture();
    const std::string renamePath = inDir("rename_blocked.npaint");
    // Occupies the FINAL destination -- not the temp sibling this time -- so
    // the write and the verify both succeed and only the rename fails.
    // Renaming any file onto an existing directory refuses on every
    // platform this project targets, empty or not.
    fs::create_directory(renamePath, ec);
    check(!ec, "fixture: the destination is occupied by a directory");

    NpaintSaveOptions verifyOpt;
    verifyOpt.verifyReadback = true;
    const NpaintSaveResult renameBlocked = saveNpaint(good, renamePath, verifyOpt);
    check(!renameBlocked.ok && !renameBlocked.verificationFailed,
          "the save refuses when the verified write cannot be put in place -- and this is not "
          "a verification failure either, so verificationFailed stays false");
    check(fs::is_directory(renamePath, ec),
          "...and whatever was at that path -- here, the directory -- is exactly what it was");

    fs::remove_all(renamePath, ec);
  }

  // --- F. the same guarantee one layer up: saveDocumentAs() and the close
  //        guard ------------------------------------------------------------
  std::printf("-- F. a failed save leaves the document dirty, and the close guard still asks "
              "--\n");
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = buildFixture();
    const std::string path = inDir("doc2.npaint");
    check(saveDocumentAs(doc, path, {}, nullptr).ok, "fixture: this document saves once, "
                                                     "cleanly");
    check(!doc.isDirty(), "...and starts this section clean");

    doc.document.layers[0].opacity = 0.4f;
    doc.recordEdit("nudge opacity");
    check(doc.isDirty(), "fixture: an edit makes it dirty again");

    // The same write-failure mechanism as section D, this time driven
    // through saveDocumentAs() -- proving the generic invariant rather than
    // a verification-specific one: saveDocumentAs() does not, and per
    // app/CloseDecision.hpp's own `closeNeedsDecision()` should not,
    // distinguish "the write failed" from "the write succeeded but did not
    // verify". Both are `!save.ok`, and saveDocumentAs()'s early return on
    // that is the whole of what keeps a failed save from being marked
    // clean -- so exercising it with a write failure exercises the exact
    // code a verification failure would also reach.
    const std::string tempSibling = inDir("doc2.tmp.npaint");
    fs::create_directory(tempSibling, ec);
    const DocumentOpResult saveFailed = saveDocumentAs(doc, path, {}, nullptr);
    check(!saveFailed.ok, "the save refuses");
    check(doc.isDirty(),
          "...the document stays dirty -- saveDocumentAs()'s early return on !save.ok is never "
          "reached by the code that clears it");
    check(closeNeedsDecision(doc), "...and the close guard still asks about it (PRD I11: a "
                                   "document whose save failed is still dirty)");
    check(contains(closeQuestion(doc), documentDisplayName(doc).c_str()),
          "...naming the document, the same way it would for any other unsaved edit");
    check(doc.document.layers[0].opacity == 0.4f,
          "...and the in-memory edit itself is untouched -- nothing about a failed save "
          "discards it");

    fs::remove_all(tempSibling, ec);
    const DocumentOpResult retried = saveDocumentAs(doc, path, {}, nullptr);
    check(retried.ok && !doc.isDirty(),
          "...and once the obstruction is gone, the SAME edit saves and cleans normally");
  }

  // --- G. a foreign carried pigment basis is not a false verification
  //        failure -----------------------------------------------------------
  //
  // A regression case for a real defect this section's own writing caught:
  // saveNpaint() stamps `np:basis` from `carry->basis` when it is non-empty
  // (io/NpaintFile.hpp's kNpaintPigmentBasis argues why -- a document loaded
  // from a foreign-basis file must round-trip that file's own claim, not this
  // build's), which is a legitimate, documented, NOT-refused case for an
  // RGB-only document (the mismatch refusal fires only when the document
  // holds Pigment layers). The first version of verifyNpaintRoundTrip()'s
  // caller compared the reload against `doc.pigmentBasis` unmodified, so
  // every such save -- correct, lossless, exactly what PRD I10 asks for --
  // would have failed its own new verification and been reported to the
  // user as broken. Fixed at the call site in saveNpaint(); this is what
  // would have caught it.
  std::printf("-- G. a foreign carried pigment basis on an RGB-only document verifies "
              "correctly, rather than failing against its own document's default --\n");
  {
    Document rgbOnly = Document::createBlank(32, 32, WorkingSpace{});
    check(rgbOnly.pigmentBasis == kPigmentBasisThisBuild,
          "fixture: an RGB-only document still carries this build's default basis label");

    NpaintCarry foreignBasis;
    // A basis this build has never heard of -- and which one that is depends
    // on NP_USE_MIXBOX, since core/Document.hpp's two names swap roles with
    // the flag. Spelling "km2-v1" here would make this an ordinary same-basis
    // save in the OFF build, where km2-v1 IS this build's own.
#if defined(NP_USE_MIXBOX)
    foreignBasis.basis = kPigmentBasisKm2;
#else
    foreignBasis.basis = kPigmentBasisMixbox;
#endif
    const std::string path = inDir("foreign_basis.npaint");

    NpaintSaveOptions verifyOpt;
    verifyOpt.verifyReadback = true;
    const NpaintSaveResult saved = saveNpaint(rgbOnly, path, verifyOpt, &foreignBasis);
    check(saved.ok,
          "the save succeeds and verifies -- a carried foreign basis on an RGB-only document "
          "is not a defect, and the verification agrees");

    const NpaintLoadResult reloaded = loadNpaint(path);
    check(reloaded.ok && reloaded.document.pigmentBasis == foreignBasis.basis,
          "...and the file genuinely carries the FOREIGN basis, exactly as PRD I10 requires -- "
          "this was never going to match `rgbOnly.pigmentBasis` verbatim, which is the whole "
          "point of the fix");
  }

  // --- Clean up --------------------------------------------------------------
  fs::remove_all(dir, ec);
  check(!fs::exists(dir, ec), "every scratch file this section wrote is removed");

  std::printf("[selftest] save readback %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
