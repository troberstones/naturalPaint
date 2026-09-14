#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/PasteCommands.hpp"
#include "core/Clipboard.hpp"
#include "core/Mask.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

namespace np {

// app/PasteCommands (PRD M9): Paste Into and Paste as New Document. (The
// Move tool's Option-drag duplicate is proven by `runMoveToolTest()` instead,
// app/selftest/MoveTool.cpp.)
//
// Pure and headless throughout, except the one OS-pasteboard smoke test
// section 3 names -- io/ClipboardImage.hpp's own header explains why the
// live pasteboard's CONTENT cannot be pinned in `--selftest`.
bool runPasteCommandsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t w, int32_t h,
                     std::array<float, 4> rgba) {
    for (int32_t y = y0; y < y0 + h; ++y)
      for (int32_t x = x0; x < x0 + w; ++x)
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), rgba);
  };

  // --- 1. Paste Into ---------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    // A 10x10 opaque red block at (2,2), copied whole (null selection) --
    // content bounds come back as exactly (2,2)+10x10 (ops/DocumentTransform's
    // scanStoreRegion() is a tight, texel-exact bounding box).
    fillRect(*od.document.layers[0].rgbTiles, 2, 2, 10, 10, {1.0f, 0.0f, 0.0f, 1.0f});
    od.recordEdit("fill fixture", EditKind::Content);
    const Clipboard clip = copyThroughSelection(od.document.layers[0], nullptr);
    check(!clip.empty(), "pasteinto: (fixture) the clipboard holds the red block");

    // Refusals first: no selection, then an empty clipboard.
    {
      const PasteIntoResult noSel = pasteInto(od, clip);
      check(!noSel.ok && noSel.error.find("nothing is selected") != std::string::npos,
            "pasteinto: refuses BY NAME with no active selection");
      const Clipboard empty;
      od.selection = selectRectangle(30.0f, 30.0f, 50.0f, 50.0f);
      const PasteIntoResult emptyClip = pasteInto(od, empty);
      check(!emptyClip.ok && emptyClip.error.find("empty") != std::string::npos,
            "pasteinto: refuses BY NAME with an empty clipboard");
    }

    // A 20x20 selection centred on (40,40); the 10x10 content centred on
    // (7,7). Centring the content on the selection's bounds should land it at
    // (35,35): 40 - 5 (half the content's own 10px width) = 35.
    const size_t layersBefore = od.document.layers.size();
    const PasteIntoResult r = pasteInto(od, clip);
    check(r.ok, "pasteinto: commits with a selection and clipboard content both present");
    check(od.document.layers.size() == layersBefore + 1,
          "pasteinto: exactly one new layer appears");
    check(r.layerIndex == od.activeLayer,
          "pasteinto: the new layer becomes active");

    Layer& pasted = od.document.layers[r.layerIndex];
    const DocumentRegion bounds = rgbContentRegion(*pasted.rgbTiles);
    check(bounds.x == 35 && bounds.y == 35 && bounds.width == 10u && bounds.height == 10u,
          "pasteinto: REQUIRED -- the pasted content is centred on the selection's bounds "
          "(30,30)-(50,50), landing at exactly (35,35)+10x10");

    check(pasted.mask.has_value(), "pasteinto: REQUIRED -- the new layer carries a mask");
    if (pasted.mask.has_value()) {
      // Sampled against selectionCoverageAt() directly, not against a second
      // hand-derived expectation -- "the mask equals the selection" is the
      // literal claim, so the oracle is the selection itself. The whole
      // fixture sits inside one 128-px tile, so every point below lands in
      // the ONE mask tile this command had to materialise regardless of
      // where the pasted content ended up.
      auto maskAt = [&](int32_t x, int32_t y) {
        const MaskTile* t = pasted.mask->find(tileCoordAt(PixelCoord{x, y}));
        return maskCoverage(t, tileLocalOffset(PixelCoord{x, y}));
      };
      auto selAt = [&](int32_t x, int32_t y) {
        return selectionCoverageAt(&*od.selection, PixelCoord{x, y});
      };
      bool everyPointAgrees = true;
      for (const auto& p : {std::pair{35, 35}, std::pair{49, 49}, std::pair{31, 48},
                            std::pair{10, 10}, std::pair{55, 55}}) {
        if (maskAt(p.first, p.second) != selAt(p.first, p.second)) everyPointAgrees = false;
      }
      check(everyPointAgrees,
            "pasteinto: REQUIRED -- the mask's coverage equals selectionCoverageAt() at every "
            "sampled point, inside the selection, inside the paste, and outside both");
    }
  }

  // --- 2. Paste as New Document: the pure builder -----------------------------
  {
    Document src = Document::createBlank(96, 96, WorkingSpace{});
    fillRect(*src.layers[0].rgbTiles, 20, 30, 8, 6, {0.0f, 1.0f, 0.0f, 1.0f});
    OpenDocument holder;
    holder.id = 1;
    holder.document = src;
    const Clipboard clip = copyThroughSelection(holder.document.layers[0], nullptr);
    check(!clip.empty(), "pasteasnew: (fixture) the clipboard holds the green block");

    check(!buildDocumentFromClipboard(Clipboard{}).has_value(),
          "pasteasnew: an empty clipboard builds nothing");

    const std::optional<OpenDocument> built = buildDocumentFromClipboard(clip);
    check(built.has_value(), "pasteasnew: a non-empty clipboard builds a document");
    if (built.has_value()) {
      check(built->document.width == 8 && built->document.height == 6,
            "pasteasnew: REQUIRED -- sized EXACTLY to the clipboard's own content bounds "
            "(8x6), not the source document's 96x96 canvas");
      check(built->document.layers.size() == 1,
            "pasteasnew: REQUIRED -- exactly ONE layer, not the blank placeholder plus the "
            "pasted one");
      // Shifted to the origin: what was at (20,30) in the source is now at
      // (0,0) in the new document.
      const TransformImage img =
          imageFromTileStore(*built->document.layers[0].rgbTiles, 0, 0, 8u, 6u);
      bool allGreen = true;
      for (size_t i = 0; i + 3 < img.px.size(); i += 4)
        if (img.px[i] != 0.0f || img.px[i + 1] != 1.0f || img.px[i + 2] != 0.0f ||
            img.px[i + 3] != 1.0f)
          allGreen = false;
      check(allGreen,
            "pasteasnew: REQUIRED -- every texel of the new document is the source block, "
            "bit-exact, shifted so its own top-left sits at the origin");
    }
  }

  // --- 3. Paste as New Document: internal-clipboard precedence ---------------
  //
  // With `st.clipboard` non-empty, the command must take the internal path
  // and never touch the OS pasteboard at all (PRD M8) -- proven by NOT
  // depending on the live pasteboard's state for this branch to be
  // deterministic, unlike section 4 below.
  {
    AppState st;
    Document src = Document::createBlank(48, 48, WorkingSpace{});
    fillRect(*src.layers[0].rgbTiles, 5, 5, 4, 4, {1.0f, 1.0f, 0.0f, 1.0f});
    OpenDocument holder;
    holder.id = 2;
    holder.document = src;
    st.clipboard = copyThroughSelection(holder.document.layers[0], nullptr);
    check(!st.clipboard.empty(), "pasteasnew: (fixture) the internal clipboard holds content");

    const size_t docsBefore = st.documents.count();
    const PasteAsNewDocumentResult r = pasteAsNewDocument(st);
    check(r.ok, "pasteasnew: commits from the internal clipboard");
    check(st.documents.count() == docsBefore + 1,
          "pasteasnew: exactly one document is opened");
    if (r.ok) {
      const OpenDocument* opened = st.documents.active();
      check(opened != nullptr && opened->document.width == 4 && opened->document.height == 4,
            "pasteasnew: REQUIRED -- the newly opened document is the ACTIVE one, and it is "
            "sized to the internal clipboard's own content (4x4), not any OS image");
    }
  }

  // --- 4. Paste as New Document: an empty everything refuses cleanly ---------
  //
  // The one branch this suite cannot fully pin: an EMPTY internal clipboard
  // falls back to the real OS pasteboard (io/ClipboardImage.hpp), whose
  // CONTENT --selftest does not control. What is asserted is what every
  // outcome must share regardless of what a machine's pasteboard happens to
  // hold right now: never a crash, and a refusal (if any) names a reason.
  {
    AppState st;
    check(st.clipboard.empty(), "pasteasnew: (fixture) the internal clipboard starts empty");
    const size_t docsBefore = st.documents.count();
    const PasteAsNewDocumentResult r = pasteAsNewDocument(st);
    if (r.ok) {
      check(st.documents.count() == docsBefore + 1,
            "pasteasnew: (live pasteboard held an image) exactly one document was opened");
    } else {
      check(!r.error.empty(),
            "pasteasnew: (live pasteboard held nothing usable) the refusal names a reason "
            "rather than failing silently");
    }
  }

  std::printf("[selftest] paste commands %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
