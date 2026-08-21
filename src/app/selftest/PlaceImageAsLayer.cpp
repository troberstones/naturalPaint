#include "app/selftest/Support.hpp"

namespace np {

bool runPlaceImageAsLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol8 = 0.01f;  // same magnitude as runImageIOTest()'s kTol8.

  // Same 2x2 fixture as runImageIOTest()'s first block -- two corners with
  // alpha < 1 so premultiplied and straight alpha are distinguishable.
  const uint8_t px[2 * 2 * 4] = {
      255, 255, 255, 255,  200, 40,  40,  128,
      0,   0,   0,   255,  100, 150, 200, 64,
  };
  std::vector<uint8_t> png;
  stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

  // --- placing into a Document::createBlank() document: the base layer
  // stays at index 0, untouched, and the placed image lands as a second
  // layer appended to the end (top of the stack) ------------------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    check(doc.layers.size() == 1, "placeImageAsLayer: fixture starts as a 1-layer document");

    std::string err;
    const bool placed = placeImageAsLayer(doc, png.data(), png.size(), &err);
    check(placed, "placeImageAsLayer: valid 2x2 PNG places successfully");
    check(err.empty(), "placeImageAsLayer: no error string on success");

    check(doc.layers.size() == 2,
          "placeImageAsLayer: document now has exactly two layers");
    if (doc.layers.size() == 2) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "placeImageAsLayer: original base layer at index 0 is still RGB-kind");
      check(doc.layers[0].rgbTiles.has_value() && doc.layers[0].rgbTiles->occupiedTileCount() == 0,
            "placeImageAsLayer: original base layer at index 0 is unchanged (still no tiles)");

      const Layer& placedLayer = doc.layers[1];
      check(placedLayer.kind == LayerKind::RGB,
            "placeImageAsLayer: the new layer (index 1, top of the stack) is RGB-kind");
      check(placedLayer.rgbTiles.has_value(),
            "placeImageAsLayer: the new layer's tile storage is populated");

      if (placedLayer.rgbTiles) {
        const TileStore& tiles = *placedLayer.rgbTiles;
        check(tiles.occupiedTileCount() == 1,
              "placeImageAsLayer: a 2x2 image occupies exactly one 128x128 tile");

        const Tile* tile = tiles.find(TileCoord{0, 0});
        check(tile != nullptr, "placeImageAsLayer: tile (0,0) exists in the new layer");
        if (tile) {
          // (1,0): (200, 40, 40, 128) -- alpha = 128/255 ~ 0.502.
          const auto tr = tile->readPixel(PixelCoord{1, 0});
          const float trA = 128 / 255.0f;
          const float trRLin = srgbDecode(200 / 255.0f);
          check(near(tr[0], trRLin * trA, kTol8) && near(tr[3], trA, kTol8),
                "placeImageAsLayer: placed pixel's rgb*a matches the source image "
                "(premultiplied), alpha unchanged");
          check(!near(tr[0], trRLin, 0.05f),
                "placeImageAsLayer: that pixel's red genuinely differs from the "
                "un-multiplied value -- proves premultiply ran, not just a copy");

          // Opaque corner: alpha = 1, so premultiplied == straight.
          const auto tl = tile->readPixel(PixelCoord{0, 0});
          check(near(tl[0], 1.0f, kTol8) && near(tl[3], 1.0f, kTol8),
                "placeImageAsLayer: opaque corner (alpha=1) premultiplies to itself");
        }
      }
    }
  }

  // --- placing into a Document that starts with zero layers: the function
  // doesn't assume index 0 already exists -- it only ever appends -------
  {
    Document doc;  // width/height/workingSpace default-constructed; layers empty.
    check(doc.layers.empty(), "placeImageAsLayer: fixture starts as a 0-layer document");

    const bool placed = placeImageAsLayer(doc, png.data(), png.size());
    check(placed, "placeImageAsLayer: places successfully into a 0-layer document");
    check(doc.layers.size() == 1,
          "placeImageAsLayer: a 0-layer document ends up with exactly one layer, at index 0");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "placeImageAsLayer: that layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value() &&
                doc.layers[0].rgbTiles->occupiedTileCount() == 1,
            "placeImageAsLayer: that layer's tiles hold the placed image");
    }
  }

  // --- the DecodedImage-taking overload, exercised directly (not only
  // reached through the file-bytes overload) ----------------------------
  {
    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid(), "placeImageAsLayer: DecodedImage fixture decodes");

    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    const bool placed = placeImageAsLayer(doc, img);
    check(placed, "placeImageAsLayer(DecodedImage): places successfully");
    check(doc.layers.size() == 2,
          "placeImageAsLayer(DecodedImage): document now has exactly two layers");
  }

  // --- clean failure: garbage bytes leave doc.layers completely
  // untouched, never a partially-inserted broken layer -------------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    const size_t layersBefore = doc.layers.size();

    const uint8_t garbage[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::string err;
    const bool placed = placeImageAsLayer(doc, garbage, sizeof(garbage), &err);
    check(!placed, "placeImageAsLayer: garbage bytes fail cleanly (returns false)");
    check(!err.empty(),
          "placeImageAsLayer: failure forwards decodeImageLinear()'s error string");
    check(doc.layers.size() == layersBefore,
          "placeImageAsLayer: doc.layers is completely untouched after a failed place");
  }

  // --- clean failure via the DecodedImage overload: an invalid image is
  // also a no-op, matching writeDecodedImageIntoLayer()'s own contract ---
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    const size_t layersBefore = doc.layers.size();

    const DecodedImage invalid;  // width == 0, valid() == false.
    const bool placed = placeImageAsLayer(doc, invalid);
    check(!placed, "placeImageAsLayer(DecodedImage): invalid image fails cleanly");
    check(doc.layers.size() == layersBefore,
          "placeImageAsLayer(DecodedImage): doc.layers is untouched after a failed place");
  }

  std::printf("[selftest] place image as layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
