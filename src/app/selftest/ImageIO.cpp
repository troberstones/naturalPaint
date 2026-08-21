#include "app/selftest/Support.hpp"

namespace np {

// io/ImageIO (PLAN.md Phase 2 step 6's remaining half). See SelfTest.hpp for
// the full breakdown; in short: openImageAsDocument() on a small PNG with
// alpha < 1 corners produces a right-sized, one-RGB-layer Document whose
// tiles hold rgb*a (not straight rgb) at those corners; a multi-tile-
// spanning image occupies exactly the tiles its footprint covers, not
// something tied to a larger nominal canvas (PRD C2); the lower-level
// writeDecodedImageIntoLayer() round-trips against a hand-built Layer on
// its own, independent of openImageAsDocument(); and corrupt/truncated
// bytes fail cleanly.
bool runImageIOTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // Byte-quantization tolerance, same magnitude as runImageDecodeTest()'s
  // kTol8 -- this fixture is 8-bit-per-channel, and every value checked
  // here passed through decodeImageLinear() first.
  constexpr float kTol8 = 0.01f;

  // --- openImageAsDocument: 2x2 PNG, two corners with alpha < 1 so
  // premultiplied and straight alpha are actually distinguishable ---------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  200, 40,  40,  128,
        0,   0,   0,   255,  100, 150, 200, 64,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    std::string err;
    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size(), &err);
    check(docOpt.has_value(), "openImageAsDocument: valid 2x2 PNG fixture decodes");

    if (docOpt) {
      const Document& doc = *docOpt;
      check(doc.width == 2 && doc.height == 2,
            "openImageAsDocument: Document is sized to the decoded image");
      check(doc.layers.size() == 1, "openImageAsDocument: Document has exactly one layer");
      if (doc.layers.size() == 1) {
        check(doc.layers[0].kind == LayerKind::RGB,
              "openImageAsDocument: the one layer is RGB-kind");
        check(doc.layers[0].rgbTiles.has_value(),
              "openImageAsDocument: the RGB layer's tile storage is populated");

        if (doc.layers[0].rgbTiles) {
          const TileStore& tiles = *doc.layers[0].rgbTiles;
          check(tiles.occupiedTileCount() == 1,
                "openImageAsDocument: a 2x2 image occupies exactly one 128x128 tile "
                "(PRD C2 -- memory tracks occupied tiles, not canvas dimensions)");

          const Tile* tile = tiles.find(TileCoord{0, 0});
          check(tile != nullptr, "openImageAsDocument: tile (0,0) exists");
          if (tile) {
            const auto tl = tile->readPixel(PixelCoord{0, 0});
            check(near(tl[0], 1.0f, kTol8) && near(tl[1], 1.0f, kTol8) &&
                      near(tl[2], 1.0f, kTol8) && near(tl[3], 1.0f, kTol8),
                  "openImageAsDocument: opaque white corner (alpha=1) premultiplies to itself");

            const auto bl = tile->readPixel(PixelCoord{0, 1});
            check(near(bl[0], 0.0f, kTol8) && near(bl[1], 0.0f, kTol8) &&
                      near(bl[2], 0.0f, kTol8) && near(bl[3], 1.0f, kTol8),
                  "openImageAsDocument: opaque black corner (alpha=1) premultiplies to itself");

            // (1,0): (200, 40, 40, 128) -- alpha = 128/255 ~ 0.502.
            const auto tr = tile->readPixel(PixelCoord{1, 0});
            const float trA = 128 / 255.0f;
            const float trRLin = srgbDecode(200 / 255.0f);
            const float trGLin = srgbDecode(40 / 255.0f);
            const float trBLin = srgbDecode(40 / 255.0f);
            check(near(tr[0], trRLin * trA, kTol8) && near(tr[1], trGLin * trA, kTol8) &&
                      near(tr[2], trBLin * trA, kTol8) && near(tr[3], trA, kTol8),
                  "openImageAsDocument: alpha=128/255 pixel stores rgb*a, alpha unchanged");
            check(!near(tr[0], trRLin, 0.05f),
                  "openImageAsDocument: that pixel's red genuinely differs from the "
                  "un-multiplied value -- proves premultiply ran, not just alpha passthrough");

            // (1,1): (100, 150, 200, 64) -- alpha = 64/255 ~ 0.251.
            const auto br = tile->readPixel(PixelCoord{1, 1});
            const float brA = 64 / 255.0f;
            const float brRLin = srgbDecode(100 / 255.0f);
            const float brGLin = srgbDecode(150 / 255.0f);
            const float brBLin = srgbDecode(200 / 255.0f);
            check(near(br[0], brRLin * brA, kTol8) && near(br[1], brGLin * brA, kTol8) &&
                      near(br[2], brBLin * brA, kTol8) && near(br[3], brA, kTol8),
                  "openImageAsDocument: alpha=64/255 pixel stores rgb*a, alpha unchanged");
            check(!near(br[2], brBLin, 0.05f),
                  "openImageAsDocument: that pixel's blue genuinely differs from the "
                  "un-multiplied value -- proves premultiply ran, not just alpha passthrough");
          }
        }
      }
    }
  }

  // --- occupied-tile count tracks the image's own footprint, not a larger
  // nominal canvas: a 140x140 opaque-grey image spans a 2x2 grid of
  // 128x128 tiles (tile (1,*) and (*,1) only exist because the image
  // crosses x=128/y=128), so this also exercises the multi-tile write path
  // runImageDecodeTest()'s 2x2 fixtures above never reach ---------------
  {
    constexpr int kSize = 140;
    std::vector<uint8_t> px(static_cast<size_t>(kSize) * kSize * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = 128;
      px[i + 1] = 128;
      px[i + 2] = 128;
      px[i + 3] = 255;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, kSize, kSize, 4, px.data(), kSize * 4);

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "openImageAsDocument: 140x140 PNG fixture decodes");
    if (docOpt && docOpt->layers.size() == 1 && docOpt->layers[0].rgbTiles) {
      check(docOpt->layers[0].rgbTiles->occupiedTileCount() == 4,
            "openImageAsDocument: a 140x140 image occupies exactly the 2x2=4 tiles its "
            "footprint spans, not a count tied to some other canvas size");
    }
  }

  // --- writeDecodedImageIntoLayer: separately callable against a
  // hand-built Layer, independent of openImageAsDocument -- this is the
  // exact reuse PLAN.md step 13 ("place an image as a layer") needs later
  // against a layer inside an already-open Document -----------------------
  {
    const uint8_t px[1 * 1 * 4] = {60, 120, 180, 90};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 4, px, 4);

    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid(), "writeDecodedImageIntoLayer: 1x1 fixture decodes");
    if (img.valid()) {
      Layer layer;
      layer.kind = LayerKind::RGB;
      layer.rgbTiles.emplace();
      writeDecodedImageIntoLayer(img, layer);

      check(layer.rgbTiles->occupiedTileCount() == 1,
            "writeDecodedImageIntoLayer: writing a 1x1 image allocates exactly one tile");
      const Tile* tile = layer.rgbTiles->find(TileCoord{0, 0});
      check(tile != nullptr, "writeDecodedImageIntoLayer: tile (0,0) exists after writing");
      if (tile) {
        const auto rt = tile->readPixel(PixelCoord{0, 0});
        const float a = 90 / 255.0f;
        check(near(rt[0], srgbDecode(60 / 255.0f) * a, kTol8) &&
                  near(rt[1], srgbDecode(120 / 255.0f) * a, kTol8) &&
                  near(rt[2], srgbDecode(180 / 255.0f) * a, kTol8) && near(rt[3], a, kTol8),
              "writeDecodedImageIntoLayer: pixel lands premultiplied in a hand-built Layer");
      }
    }

    // A Layer whose RGB tile storage isn't populated (the common case --
    // default kind is Pigment, per core/Layer.hpp) must be a safe no-op,
    // never a crash: this is the misuse case, not the intended call shape.
    Layer noRgbLayer;
    writeDecodedImageIntoLayer(img, noRgbLayer);
    check(!noRgbLayer.rgbTiles.has_value(),
          "writeDecodedImageIntoLayer: no-op against a Layer with no RGB tile storage");
  }

  // --- Failure handling: corrupt/truncated bytes fail cleanly, never a
  // bogus Document -----------------------------------------------------
  {
    const uint8_t garbage[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::string err;
    const std::optional<Document> docOpt =
        openImageAsDocument(garbage, sizeof(garbage), &err);
    check(!docOpt.has_value(), "openImageAsDocument: garbage bytes return std::nullopt");
    check(!err.empty(),
          "openImageAsDocument: failure forwards decodeImageLinear()'s error string");
  }
  {
    // A real PNG stream, cut off partway through -- exercises stb_image's
    // "ran out of bytes mid-decode" path, not just "never a PNG at all".
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        0,   0,   0,   255,  255, 255, 255, 255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);
    const size_t truncatedSize = png.size() / 2;

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), truncatedSize);
    check(!docOpt.has_value(),
          "openImageAsDocument: truncated PNG bytes return std::nullopt, not a bogus Document");
  }
  {
    const std::optional<Document> docOpt = openImageAsDocument(nullptr, 0);
    check(!docOpt.has_value(), "openImageAsDocument: null/empty input returns std::nullopt");
  }

  std::printf("[selftest] image io %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
