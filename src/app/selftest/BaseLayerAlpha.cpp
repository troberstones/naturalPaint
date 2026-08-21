#include "app/selftest/Support.hpp"

namespace np {

// PLAN.md Phase 2 step 14 / PRD C16: "the base layer is an ordinary layer
// with alpha, no locked Background." core/Layer.hpp has no
// background/locked concept at all -- every Layer, first or otherwise, is
// the same struct -- so the property holds by construction; this proves it
// rather than leaving it as an assumption. The two things a locked/special
// Background *would* do differently from an ordinary layer: refuse a
// non-opaque alpha, and be distinguishable from other layers by some flag.
// Neither exists here.
bool runBaseLayerAlphaTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  const Document doc = Document::createBlank(64, 64, WorkingSpace{});
  check(doc.layers.size() == 1, "createBlank()'s base layer is the document's only layer");

  Layer& base = const_cast<Document&>(doc).layers[0];
  check(base.rgbTiles.has_value(), "the base layer has ordinary RGB tile storage");
  if (base.rgbTiles) {
    // Fully transparent -- if the base layer were a locked/opaque
    // Background, this write would either be rejected or silently forced
    // back to alpha=1. Neither happens: TileStore::writePixel/readPixel
    // treat this layer exactly like any other.
    const std::array<float, 4> transparent{0.0f, 0.0f, 0.0f, 0.0f};
    base.rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{5, 5}, transparent);
    const auto rt = base.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5});
    check(nearf(rt[3], 0.0f, 0.001f),
          "the base layer's alpha channel writes and reads back 0 -- not clamped to opaque");

    // And an ordinary partial alpha, at the opposite end from the existing
    // io/ImageIO coverage's 128/255 and 64/255 fixtures, to show the whole
    // [0,1] range is honoured, not just "not fully transparent."
    const std::array<float, 4> partial{0.5f, 0.25f, 0.1f, 0.75f};
    base.rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{6, 6}, partial);
    const auto rt2 = base.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{6, 6});
    check(nearf(rt2[3], 0.75f, 0.01f),
          "the base layer's alpha channel round-trips an arbitrary partial value");
  }

  std::printf("[selftest] base layer alpha %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
