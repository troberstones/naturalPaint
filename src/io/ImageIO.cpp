#include "io/ImageIO.hpp"

#include <array>

#include "color/Space.hpp"
#include "core/Tile.hpp"

namespace np {

void writeDecodedImageIntoLayer(const DecodedImage& img, Layer& layer) {
  if (!img.valid() || !layer.rgbTiles.has_value()) return;

  TileStore& tiles = *layer.rgbTiles;
  for (uint32_t y = 0; y < img.height; ++y) {
    for (uint32_t x = 0; x < img.width; ++x) {
      const float* src = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
      const float a = src[3];
      // Premultiply: rgb *= a, alpha unchanged (DESIGN-imaging.md §2).
      const std::array<float, 4> premultiplied{src[0] * a, src[1] * a, src[2] * a, a};

      const PixelCoord doc{static_cast<int32_t>(x), static_cast<int32_t>(y)};
      tiles.getOrCreate(tileCoordAt(doc)).writePixel(tileLocalOffset(doc), premultiplied);
    }
  }
}

std::optional<Document> openImageAsDocument(const uint8_t* fileData, size_t fileSize,
                                             std::string* errorOut) {
  const DecodedImage img = decodeImageLinear(fileData, fileSize, errorOut);
  if (!img.valid()) return std::nullopt;

  Document doc = Document::createBlank(static_cast<int32_t>(img.width),
                                        static_cast<int32_t>(img.height), WorkingSpace{});
  writeDecodedImageIntoLayer(img, doc.layers[0]);
  return doc;
}

bool placeImageAsLayer(Document& doc, const DecodedImage& img) {
  if (!img.valid()) return false;

  // Same policy Document::createBlank() uses for its one layer -- reused,
  // not re-decided, here.
  Layer layer;
  layer.kind = LayerKind::RGB;
  layer.rgbTiles.emplace();
  writeDecodedImageIntoLayer(img, layer);

  doc.layers.push_back(std::move(layer));
  return true;
}

bool placeImageAsLayer(Document& doc, const uint8_t* fileData, size_t fileSize,
                        std::string* errorOut) {
  const DecodedImage img = decodeImageLinear(fileData, fileSize, errorOut);
  if (!img.valid()) return false;
  return placeImageAsLayer(doc, img);
}

}  // namespace np
