#include "app/selftest/Support.hpp"

namespace np {

// core/Document + core/Layer (PLAN.md Phase 2 step 4). See SelfTest.hpp for
// the full breakdown; in short: LayerKind's seven CONTEXT.md values
// round-trip through the name helpers that moved here from app/Keymap in
// this same step; a default Layer is Pigment-kinded with no RGB storage
// (only RGB is wired up -- "design for N, ship 1"); a Document holds what
// it's built with and starts with no layers (nothing here manufactures the
// one-layer document -- that's PLAN.md's next, separate step,
// Document::createBlank()); and a hand-built one-entry, RGB-kind layer list
// round-trips a pixel through core::TileStore exactly as runTileStoreTest()
// already proved TileStore itself does, just reached through Layer::rgbTiles
// this time.
bool runDocumentTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- LayerKind: all seven CONTEXT.md kinds round-trip name <-> kind ---
  const LayerKind allKinds[] = {LayerKind::Pigment,    LayerKind::RGB,   LayerKind::Media,
                                 LayerKind::Strokes,    LayerKind::Adjustment,
                                 LayerKind::Text,       LayerKind::Flats};
  bool allRoundTrip = true;
  for (LayerKind k : allKinds) {
    const auto back = layerKindFromName(layerKindName(k));
    if (!back || *back != k) allRoundTrip = false;
  }
  check(allRoundTrip, "all 7 CONTEXT.md LayerKind values round-trip through name<->kind");
  check(layerKindFromName("NotAKind") == std::nullopt,
        "layerKindFromName() rejects an unrecognized name");

  // --- Layer: default kind is Pigment (CONTEXT.md's domain default), and
  // only RGB actually gets tile storage wired up ---
  Layer defaultLayer;
  check(defaultLayer.kind == LayerKind::Pigment,
        "a default-constructed Layer's kind is Pigment (CONTEXT.md's domain default)");
  check(!defaultLayer.rgbTiles.has_value(),
        "a default (Pigment-kind) Layer has no RGB tile storage populated");

  // --- Document: holds what it's constructed with; layer list starts empty
  // (this step ships the types, not a document-creation policy) ---
  Document doc;
  doc.width = 64;
  doc.height = 48;
  doc.workingSpace.primaries = kRec709Primaries;

  check(doc.width == 64 && doc.height == 48, "Document holds its assigned width/height");
  check(doc.workingSpace.primaries.redX == kRec709Primaries.redX &&
            doc.workingSpace.primaries.whiteY == kRec709Primaries.whiteY,
        "Document holds its assigned working space");
  check(doc.layers.empty(), "a freshly constructed Document's layer list starts empty");

  // --- The "ship 1" case: one RGB-kind layer, added the way a future
  // createBlank()/place-image step would, exercising the round trip
  // PLAN.md's step asks for ---
  Layer rgbLayer;
  rgbLayer.kind = LayerKind::RGB;
  rgbLayer.rgbTiles.emplace();
  doc.layers.push_back(rgbLayer);

  check(doc.layers.size() == 1, "Document's layer list holds exactly one entry (ship 1)");
  check(doc.layers[0].kind == LayerKind::RGB, "the one layer's kind is RGB");
  check(doc.layers[0].rgbTiles.has_value(), "the RGB layer's tile storage is populated");

  if (doc.layers[0].rgbTiles) {
    TileStore& tiles = *doc.layers[0].rgbTiles;
    const std::array<float, 4> pixel{0.2f, 0.4f, 0.6f, 1.0f};
    const TileCoord coord{0, 0};
    tiles.getOrCreate(coord).writePixel(PixelCoord{10, 20}, pixel);

    const Tile* found = tiles.find(coord);
    check(found != nullptr, "a pixel written through the layer's TileStore is findable");
    if (found) {
      const auto rt = found->readPixel(PixelCoord{10, 20});
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "the layer's RGB tile storage round-trips a pixel via core::TileStore's API");
    }
  }

  std::printf("[selftest] document %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
