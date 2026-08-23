#include "app/selftest/Support.hpp"

#include <cmath>

#include "core/Clipboard.hpp"

namespace np {

// core/Clipboard (PLAN.md "Phase 7 -- Select and paste"; PRD M1, M3, M4, M5,
// M8). Pure CPU, no GPU.
//
// Two claims carry this section.
//
// **PRD M5** -- the clipboard is a copy-on-write tile REFERENCE, not a
// flattened buffer. The PRD calls this Lightweight rather than a convenience
// and names the number: a 4K copy is 68 MB, which PRD A5 forbids holding
// invisibly. So it is not enough that a copy be correct; it must cost
// refcounts for everything the selection covers whole, and bytes only for the
// tiles its edge crosses. Both halves are measured here.
//
// **The two weighting rules** -- an RGB texel is premultiplied, so coverage
// scales all four channels; a Pigment texel is a straight latent plus a mass
// that is the alpha analogue, so coverage scales MASS ALONE (PRD F10). The
// pigment assertions check the latent is bit-unchanged at partial coverage,
// because scaling it by reflex from the RGB path is the natural mistake and
// produces a half-copied red that has stopped being red.
bool runClipboardTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // A document whose one RGB layer is opaque red across four whole tiles plus
  // part of a fifth. 512x512 is 4x4 tiles of 128.
  auto makeDoc = []() {
    Document doc = Document::createBlank(512, 512, WorkingSpace{});
    doc.layers[0].name = "source";
    for (int32_t ty = 0; ty < 2; ++ty) {
      for (int32_t tx = 0; tx < 2; ++tx) {
        Tile& t = doc.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty});
        for (int32_t y = 0; y < kTileSize; ++y)
          for (int32_t x = 0; x < kTileSize; ++x)
            t.writePixel(PixelCoord{x, y}, {1.0f, 0.0f, 0.0f, 1.0f});
      }
    }
    return doc;
  };

  // --- 1. PRD M5: a full copy costs refcounts, not megabytes -------------
  {
    Document doc = makeDoc();
    const size_t sourceTiles = doc.layers[0].rgbTiles->occupiedTileCount();
    const Clipboard clip = copyThroughSelection(doc.layers[0], nullptr);
    std::printf("  [selftest] clipboard: whole-layer copy of %zu tiles -- %zu shared, %zu "
                "exclusive bytes\n",
                sourceTiles, clip.sharedTileCount(), clip.exclusiveBytes());
    check(!clip.empty() && clip.rgbTiles->occupiedTileCount() == sourceTiles,
          "M5: a null-selection copy takes the whole layer -- Select All is the default");
    check(clip.sharedTileCount() == sourceTiles,
          "M5: and EVERY tile is shared with the source, not copied -- this is the 4K case "
          "the requirement names, and it costs refcounts");
    check(clip.exclusiveBytes() == 0,
          "M5: so the clipboard's exclusive cost is ZERO bytes -- what the status bar would "
          "report, and why there is no 'Purge Clipboard' command to need");

    // The proof that sharing is real and not just a label: painting on the
    // source must not change the clipboard.
    doc.layers[0].rgbTiles->findForWrite(TileCoord{0, 0})
        ->writePixel(PixelCoord{5, 5}, {0.0f, 0.0f, 1.0f, 1.0f});
    check(clip.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[0] == 1.0f,
          "M5: writing to the SOURCE after the copy leaves the clipboard's red intact -- the "
          "copy-on-write barrier fired, so a shared tile is a snapshot and not an alias");
    check(clip.exclusiveBytes() > 0,
          "M5: and the clipboard now owns that one unshared tile outright, so the cost figure "
          "goes UP when the document diverges -- which is the honest thing for it to do");
  }

  // --- 2. PRD M5: a marquee pays for its perimeter, not its area ---------
  {
    Document doc = makeDoc();
    // Covers tile (0,0) whole and half of tile (1,0): [0,192) x [0,128).
    const Selection sel = selectRectangle(0.0f, 0.0f, 192.0f, 128.0f);
    const Clipboard clip = copyThroughSelection(doc.layers[0], &sel);
    std::printf("  [selftest] clipboard: marquee copy -- %zu tiles, %zu shared, %zu exclusive "
                "bytes (one tile is %zu)\n",
                clip.rgbTiles->occupiedTileCount(), clip.sharedTileCount(),
                clip.exclusiveBytes(), sizeof(Tile));
    check(clip.rgbTiles->occupiedTileCount() == 2,
          "M5: the copy holds exactly the two tiles the marquee touches, not the layer's four");
    check(clip.sharedTileCount() == 1,
          "M5: the FULLY covered tile is shared and the edge tile is not -- the interior of a "
          "marquee is free and only its boundary is paid for");
    check(clip.exclusiveBytes() == sizeof(Tile),
          "M5: so the whole cost of this copy is one tile, whatever the marquee's area");
    check(clip.rgbTiles->find(TileCoord{1, 0}) != nullptr &&
              clip.rgbTiles->find(TileCoord{1, 0})->readPixel(PixelCoord{10, 10})[3] == 1.0f,
          "M5: and the half-covered tile really carries its selected texels");
    check(clip.rgbTiles->find(TileCoord{1, 0})->readPixel(PixelCoord{100, 10})[3] == 0.0f,
          "M5: while its UNselected texels are empty -- the edge tile was weighted, not "
          "wholesale copied");
    check(clip.rgbTiles->find(TileCoord{0, 1}) == nullptr,
          "M5: a tile the marquee never touched is absent entirely, so a small selection on a "
          "big document does not drag the document along with it");
  }

  // --- 3. PRD M1: the copy is coverage-weighted --------------------------
  {
    Document doc = makeDoc();
    const Selection sel = selectRectangle(0.25f, 0.0f, 64.0f, 64.0f);
    const Clipboard clip = copyThroughSelection(doc.layers[0], &sel);
    const Tile* t = clip.rgbTiles->find(TileCoord{0, 0});
    const std::array<float, 4> edge = t->readPixel(PixelCoord{0, 0});
    std::printf("  [selftest] clipboard: 0.75-covered texel copied as rgba(%.4f, %.4f, %.4f, "
                "%.4f)\n",
                static_cast<double>(edge[0]), static_cast<double>(edge[1]),
                static_cast<double>(edge[2]), static_cast<double>(edge[3]));
    check(near(edge[3], 0.75f, 2.0f / 255.0f),
          "M1: a 0.75-covered texel is copied at 0.75 alpha -- the copy is weighted by "
          "coverage, so a feathered selection copies a feathered edge");
    check(near(edge[0], edge[3], 1e-4f),
          "M1: and its red falls WITH its alpha, because the store is premultiplied -- the "
          "same rule the clear follows, so a cut is self-consistent");
  }

  // --- 4. PRD M1: cut is copy then clear, and refuses when it must -------
  {
    Document doc = makeDoc();
    const Selection sel = selectRectangle(0.0f, 0.0f, 128.0f, 128.0f);
    const Clipboard clip = cutThroughSelection(doc.layers[0], &sel);
    check(!clip.empty() && clip.rgbTiles->occupiedTileCount() == 1,
          "M1: a cut yields the same payload a copy would");
    check(doc.layers[0].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[3] == 0.0f,
          "M1: and the source tile is now empty -- cut removed what it took");
    check(doc.layers[0].rgbTiles->find(TileCoord{1, 0})->readPixel(PixelCoord{5, 5})[3] == 1.0f,
          "M1: while the neighbouring tile outside the selection is untouched");
    check(clip.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[3] == 1.0f,
          "M1: and the CLIPBOARD still holds the paint the layer no longer has -- the clear "
          "unshared rather than emptying what the copy is holding");

    // An empty selection must not become a clear with extra steps.
    Document doc2 = makeDoc();
    Selection nothing;
    const Clipboard none = cutThroughSelection(doc2.layers[0], &nothing);
    check(none.empty(),
          "M1: a cut through an EMPTY selection yields nothing");
    check(doc2.layers[0].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[3] == 1.0f,
          "M1: and destroys nothing -- without that guard it would be a full clear, deleting "
          "paint the user never selected");

    Document doc3 = makeDoc();
    doc3.layers[0].locked = true;
    const Clipboard locked = cutThroughSelection(doc3.layers[0], nullptr);
    check(locked.empty() &&
              doc3.layers[0].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[3] ==
                  1.0f,
          "M1: a cut on a LOCKED layer refuses and touches nothing -- the routing rule "
          "app/StrokeSession and app/StrokeBake already follow for destructive edits");
  }

  // --- 5. PRD M3: paste creates a layer, in place ------------------------
  {
    Document doc = makeDoc();
    const Selection sel = selectRectangle(128.0f, 0.0f, 256.0f, 128.0f);
    const Clipboard clip = copyThroughSelection(doc.layers[0], &sel);
    const size_t before = doc.layers.size();
    const std::optional<size_t> at = pasteAsLayer(doc, clip, doc.layers.size());
    check(at.has_value() && doc.layers.size() == before + 1,
          "M3: paste creates a LAYER rather than stamping into the current one");
    if (at.has_value()) {
      const Layer& pasted = doc.layers[*at];
      check(pasted.rgbTiles.has_value() && pasted.rgbTiles->find(TileCoord{1, 0}) != nullptr,
            "M3: and the pasted tile keeps its ORIGINAL document coordinate -- paste in place "
            "is exact because tiles are keyed in document space, so there is nothing to "
            "offset and therefore nothing to resample");
      check(pasted.rgbTiles->find(TileCoord{0, 0}) == nullptr,
            "M3: it did not land at the origin, which is what an offset-to-zero paste would "
            "have done");
      const PixelCoord probe{130, 10};
      const Tile* pastedTile = pasted.rgbTiles->find(tileCoordAt(probe));
      check(pastedTile != nullptr &&
                pastedTile->readPixel(tileLocalOffset(probe))[0] == 1.0f,
            "M3: and the pasted pixel is bit-identical to the source it came from");
      check(pasted.name == "source",
            "M3: the pasted layer is named after where it came from, not 'Layer 2'");
      check(pasted.rgbTiles->sharedTileCount() == 1,
            "M5: pasting SHARES with the clipboard too, so pasting twice costs two refcounts "
            "and not two tiles");
    }
  }

  // --- 6. PRD M8: pigment latents survive, and mass alone is weighted ----
  {
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("pigment source");
    PigmentTile& src = doc.layers[0].pigmentTiles->getOrCreate(TileCoord{0, 0});
    PigmentTexel painted;
    painted.latent.c[0] = 0.25f;
    painted.latent.c[1] = 0.5f;
    painted.latent.c[2] = 0.75f;
    painted.mass = 1.0f;
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x) src.writeTexel(PixelCoord{x, y}, painted);

    // Fully covered: the tile is shared, so the latents are not merely equal,
    // they are the SAME BYTES. That is PRD M8's "never round-trips the
    // pasteboard" made structural -- there is no encode to lose them in.
    const Clipboard whole = copyThroughSelection(doc.layers[0], nullptr);
    check(whole.pigmentTiles.has_value() && whole.sharedTileCount() == 1,
          "M8: a pigment copy shares its tiles like any other -- no serialisation step exists "
          "for a latent to be lost in");
    const PigmentTexel back = whole.pigmentTiles->find(TileCoord{0, 0})->readTexel(
        PixelCoord{3, 3});
    check(back.latent.c[0] == painted.latent.c[0] && back.latent.c[1] == painted.latent.c[1] &&
              back.latent.c[2] == painted.latent.c[2] && back.mass == painted.mass,
          "M8: so the latent comes back EXACTLY -- copy-paste of pigment is lossless because "
          "it never leaves the internal representation");

    // Partially covered: mass scales, latent does NOT. The rule that differs
    // from the RGB path, and the one a reflex would get wrong.
    const Selection half = selectRectangle(0.5f, 0.0f, 64.0f, 64.0f);
    const Clipboard part = copyThroughSelection(doc.layers[0], &half);
    const PigmentTexel edge =
        part.pigmentTiles->find(TileCoord{0, 0})->readTexel(PixelCoord{0, 0});
    std::printf("  [selftest] clipboard: 0.5-covered pigment texel -- mass %.4f (was %.4f), "
                "latent (%.4f, %.4f, %.4f)\n",
                static_cast<double>(edge.mass), static_cast<double>(painted.mass),
                static_cast<double>(edge.latent.c[0]), static_cast<double>(edge.latent.c[1]),
                static_cast<double>(edge.latent.c[2]));
    check(near(edge.mass, 0.5f, 2.0f / 255.0f),
          "M1: a half-covered pigment texel copies at half MASS -- coverage weights how much "
          "pigment there is");
    check(edge.latent.c[0] == painted.latent.c[0] && edge.latent.c[1] == painted.latent.c[1] &&
              edge.latent.c[2] == painted.latent.c[2],
          "M8: and its LATENT is bit-unchanged -- PRD F10's rule. Scaling it too, by reflex "
          "from the premultiplied RGB path, would make a half-copied colour stop being that "
          "colour instead of being less of it");
  }

  // --- 7. PRD M4: selection -> new layer ---------------------------------
  {
    Document doc = makeDoc();
    const Selection sel = selectRectangle(0.0f, 0.0f, 128.0f, 128.0f);
    const size_t before = doc.layers.size();
    const std::optional<size_t> at = selectionToNewLayer(doc, 0, &sel);
    check(at.has_value() && *at == 1 && doc.layers.size() == before + 1,
          "M4: selection -> new layer lands the copy directly ABOVE its source");
    check(doc.layers[0].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5})[3] == 1.0f,
          "M4: and leaves the source intact -- it is a copy, not a cut (PRD M4 lists them "
          "separately and this is the non-destructive one)");
    Selection nothing;
    check(!selectionToNewLayer(doc, 0, &nothing).has_value(),
          "M4: an empty selection makes no layer, rather than an empty one nobody asked for");
    check(!selectionToNewLayer(doc, 99, &sel).has_value(),
          "M4: and an out-of-range source index refuses rather than reading past the stack");
  }

  std::printf("[selftest] clipboard %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
