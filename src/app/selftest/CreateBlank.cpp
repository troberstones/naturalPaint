#include "app/selftest/Support.hpp"

namespace np {

// Document::createBlank() (PLAN.md Phase 2 step 5, PRD C7). Separate
// function from runDocumentTest() above rather than folded into it: step 4
// shipped the Document/Layer *types*; step 5 is a distinct policy decision
// on top of them (what kind the blank layer is, whether it starts with any
// tiles allocated), same "one function per PLAN.md step" shape as
// runTileStoreTest()/runImageDecodeTest() elsewhere in this file.
//
// The size/working-space/layer-count/layer-kind checks are the ordinary
// half. The interesting half is occupiedTileCount() == 0 for a *large*
// canvas: a naive createBlank() that loops over width/height allocating a
// tile per (or per-region) would still pass every check at a small size,
// and only a large one (4096x4096 here) forces enough tiles that an
// accidental pre-allocation loop becomes visibly, immediately wrong rather
// than passing by coincidence.
bool runCreateBlankTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- small canvas: the ordinary happy path ---
  {
    WorkingSpace space;
    space.primaries = kRec709Primaries;
    const Document doc = Document::createBlank(64, 48, space);

    check(doc.width == 64 && doc.height == 48,
          "createBlank(64,48): Document holds the given width/height");
    check(doc.workingSpace.primaries.redX == kRec709Primaries.redX &&
              doc.workingSpace.primaries.whiteY == kRec709Primaries.whiteY,
          "createBlank(64,48): Document holds the given working space");
    check(doc.layers.size() == 1, "createBlank(64,48): layer list has exactly one entry");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "createBlank(64,48): the one layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value(),
            "createBlank(64,48): the RGB layer's tile storage is populated");
      if (doc.layers[0].rgbTiles)
        check(doc.layers[0].rgbTiles->occupiedTileCount() == 0,
              "createBlank(64,48): zero tiles allocated immediately after creation");
    }
  }

  // --- large canvas (4096x4096): the assertion that actually catches a
  // wrong implementation. Same checks, but at a size where "pre-allocate a
  // grid across the canvas" would be an obvious, large, non-zero tile
  // count rather than something a small-canvas test could miss by luck. ---
  {
    WorkingSpace space;
    space.primaries = kRec709Primaries;
    const Document doc = Document::createBlank(4096, 4096, space);

    check(doc.width == 4096 && doc.height == 4096,
          "createBlank(4096,4096): Document holds the given width/height");
    check(doc.layers.size() == 1,
          "createBlank(4096,4096): layer list has exactly one entry");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "createBlank(4096,4096): the one layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value(),
            "createBlank(4096,4096): the RGB layer's tile storage is populated");
      if (doc.layers[0].rgbTiles)
        check(doc.layers[0].rgbTiles->occupiedTileCount() == 0,
              "createBlank(4096,4096): zero tiles allocated despite the large canvas "
              "(PRD C2 -- memory tracks content, not canvas dimensions)");
    }
  }

  std::printf("[selftest] create blank %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
