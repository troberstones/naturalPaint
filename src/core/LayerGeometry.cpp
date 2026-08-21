#include "core/LayerGeometry.hpp"

#include <string>

#include "core/Mask.hpp"
#include "core/Pigment.hpp"

namespace np {
namespace {

// The channel whose non-zero-ness makes a texel "content", per the header's
// section 4. Both happen to be index 3 -- alpha in an RGBA tile, mass in a
// (c0,c1,c2,mass,res0,res1,res2) pigment one -- but they are named separately
// because that is a coincidence of two independent layouts and a shared
// constant would make it look like a rule.
constexpr int32_t kRgbAlphaChannel = 3;
constexpr int32_t kPigmentMassChannel = 3;

// A half word carries content unless it is +0 or -0. Tested on the bits so a
// denormal, an infinity or a NaN a file may carry counts as content rather than
// being silently dropped by a comparison against 0.0f.
constexpr bool halfWordIsNonZero(uint16_t w) noexcept { return (w & 0x7fffu) != 0u; }

void growBounds(LayerBounds& b, int32_t x, int32_t y) {
  if (b.empty) {
    b.empty = false;
    b.minX = b.maxX = x;
    b.minY = b.maxY = y;
    return;
  }
  b.minX = std::min(b.minX, x);
  b.maxX = std::max(b.maxX, x);
  b.minY = std::min(b.minY, y);
  b.maxY = std::max(b.maxY, y);
}

// One tile's occupied sub-rectangle, folded into `b` in document coordinates.
template <class T>
void scanStore(const TileStoreOf<T>& store, int32_t channel, LayerBounds& b) {
  for (const auto& [coord, tile] : store) {
    const PixelCoord origin = tileOrigin(coord);
    const uint16_t* words = tile.data();
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const size_t at = (static_cast<size_t>(ly) * kTileSize + static_cast<size_t>(lx)) *
                              static_cast<size_t>(T::kChannels) +
                          static_cast<size_t>(channel);
        if (halfWordIsNonZero(words[at])) growBounds(b, origin.x + lx, origin.y + ly);
      }
    }
  }
}

std::string layerLabelFor(const Document& doc, size_t index) {
  const Layer& l = doc.layers[index];
  return "layer " + std::to_string(index) +
         (l.name.empty() ? std::string(" (unnamed)") : " ('" + l.name + "')");
}

}  // namespace

LayerBounds layerContentBounds(const Layer& layer) {
  LayerBounds b;
  if (layer.rgbTiles.has_value()) scanStore(*layer.rgbTiles, kRgbAlphaChannel, b);
  if (layer.pigmentTiles.has_value()) scanStore(*layer.pigmentTiles, kPigmentMassChannel, b);
  return b;
}

LayerBounds unionLayerBounds(const LayerBounds& a, const LayerBounds& b) {
  if (a.empty) return b;
  if (b.empty) return a;
  LayerBounds out;
  out.empty = false;
  out.minX = std::min(a.minX, b.minX);
  out.minY = std::min(a.minY, b.minY);
  out.maxX = std::max(a.maxX, b.maxX);
  out.maxY = std::max(a.maxY, b.maxY);
  return out;
}

LayerBounds documentCanvasBounds(const Document& doc) {
  LayerBounds b;
  if (doc.width <= 0 || doc.height <= 0) return b;
  b.empty = false;
  b.minX = 0;
  b.minY = 0;
  b.maxX = doc.width - 1;
  b.maxY = doc.height - 1;
  return b;
}

LayerOpResult translateLayer(Document& doc, size_t index, int32_t dx, int32_t dy) {
  LayerOpResult r;
  if (index >= doc.layers.size()) {
    r.error = "translate layer refused: index " + std::to_string(index) +
              " is out of range; this document has " + std::to_string(doc.layers.size()) +
              " layer(s). Nothing was changed.";
    return r;
  }
  Layer& layer = doc.layers[index];
  if (layer.locked) {
    r.error = "translate layer refused: " + layerLabelFor(doc, index) +
              " is locked, and a lock freezes a layer's content. Unlock it first. Nothing "
              "was changed.";
    return r;
  }
  if (!layer.rgbTiles.has_value() && !layer.pigmentTiles.has_value()) {
    r.error = "translate layer refused: " + layerLabelFor(doc, index) + " is a " +
              layerKindName(layer.kind) +
              " layer and holds no pixels, so there is nothing to move. Nothing was changed.";
    return r;
  }

  if (dx != 0 || dy != 0) {
    if (layer.rgbTiles.has_value())
      *layer.rgbTiles = translatedTileStore(*layer.rgbTiles, dx, dy);
    if (layer.pigmentTiles.has_value())
      *layer.pigmentTiles = translatedTileStore(*layer.pigmentTiles, dx, dy);
    // The mask moves with the layer, by the same delta -- see the header's
    // section 4. A mask left behind would slide content out from under its own
    // coverage, which is the one way a translate could change what a layer
    // shows rather than only where it is.
    if (layer.mask.has_value()) *layer.mask = translatedTileStore(*layer.mask, dx, dy);
  }

  r.ok = true;
  r.index = index;
  r.editLabel = "translate layer";
  return r;
}

}  // namespace np
