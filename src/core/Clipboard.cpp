#include "core/Clipboard.hpp"

#include <vector>

namespace np {
namespace {

// Whether the selection covers every texel of `coord` -- the test that decides
// between sharing the tile and materialising a weighted copy of it.
//
// A null selection covers everything, so every tile is shareable. That is the
// full-document copy PRD M5 names, and it is the case that must cost nothing.
bool fullyCovers(const Selection* selection, TileCoord coord) {
  if (selection == nullptr) return true;
  const SelectionTile* tile = selection->tiles.find(coord);
  return tile != nullptr && tile->selectsAll();
}

// Whether the selection touches `coord` at all. An untouched tile is not in
// the copy, which is what keeps a small marquee from dragging a whole
// document's tiles along with it.
bool touches(const Selection* selection, TileCoord coord) {
  if (selection == nullptr) return true;
  const SelectionTile* tile = selection->tiles.find(coord);
  return tile != nullptr && !tile->selectsNothing();
}

}  // namespace

bool Clipboard::empty() const noexcept {
  const size_t rgb = rgbTiles.has_value() ? rgbTiles->occupiedTileCount() : 0;
  const size_t pig = pigmentTiles.has_value() ? pigmentTiles->occupiedTileCount() : 0;
  return rgb == 0 && pig == 0;
}

size_t Clipboard::sharedTileCount() const noexcept {
  size_t n = 0;
  if (rgbTiles.has_value()) n += rgbTiles->sharedTileCount();
  if (pigmentTiles.has_value()) n += pigmentTiles->sharedTileCount();
  return n;
}

size_t Clipboard::exclusiveBytes() const noexcept {
  size_t bytes = 0;
  if (rgbTiles.has_value()) bytes += rgbTiles->exclusiveTileBytes();
  if (pigmentTiles.has_value()) bytes += pigmentTiles->exclusiveTileBytes();
  return bytes;
}

Clipboard copyThroughSelection(const Layer& layer, const Selection* selection) {
  Clipboard clip;
  clip.kind = layer.kind;
  clip.sourceName = layer.name;

  if (layer.rgbTiles.has_value()) {
    const TileStore& src = *layer.rgbTiles;
    TileStore out;
    for (const auto& [coord, tile] : src) {
      (void)tile;
      if (!touches(selection, coord)) continue;

      if (fullyCovers(selection, coord)) {
        // PRD M5, the whole point: one refcount increment, no bytes. The
        // interior of any marquee larger than a tile lands here.
        out.shareTileFrom(src, coord);
        continue;
      }

      // The selection's edge crosses this tile, so its texels have to be
      // weighted and it cannot be shared. Only the perimeter pays this.
      const SelectionTile* selTile = selection->tiles.find(coord);
      const Tile* from = src.find(coord);
      if (from == nullptr) continue;
      Tile& to = out.getOrCreate(coord);
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const PixelCoord local{lx, ly};
          const float cov = selectionTileCoverage(selTile, local);
          if (cov <= 0.0f) continue;
          const std::array<float, 4> px = from->readPixel(local);
          // Premultiplied: all four channels scale together, so a
          // half-selected texel arrives half-present rather than half-dark.
          to.writePixel(local, {px[0] * cov, px[1] * cov, px[2] * cov, px[3] * cov});
        }
      }
    }
    if (out.occupiedTileCount() > 0) clip.rgbTiles = std::move(out);
  }

  if (layer.pigmentTiles.has_value()) {
    const PigmentTileStore& src = *layer.pigmentTiles;
    PigmentTileStore out;
    for (const auto& [coord, tile] : src) {
      (void)tile;
      if (!touches(selection, coord)) continue;

      if (fullyCovers(selection, coord)) {
        out.shareTileFrom(src, coord);
        continue;
      }

      const SelectionTile* selTile = selection->tiles.find(coord);
      const PigmentTile* from = src.find(coord);
      if (from == nullptr) continue;
      PigmentTile& to = out.getOrCreate(coord);
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const PixelCoord local{lx, ly};
          const float cov = selectionTileCoverage(selTile, local);
          if (cov <= 0.0f) continue;
          PigmentTexel t = from->readTexel(local);
          // **Mass only.** The latent is the pigment's identity and does not
          // fade with its quantity (PRD F10). See Clipboard.hpp on why this
          // is a different rule from the RGB branch above rather than an
          // inconsistency with it.
          t.mass *= cov;
          to.writeTexel(local, t);
        }
      }
    }
    if (out.occupiedTileCount() > 0) clip.pigmentTiles = std::move(out);
  }

  return clip;
}

Clipboard cutThroughSelection(Layer& layer, const Selection* selection) {
  // A locked layer refuses a destructive edit, the same routing rule
  // app/StrokeSession and app/StrokeBake follow. Returning empty rather than
  // throwing keeps the refusal in the same shape as "the selection covered
  // nothing", which is what a caller has to handle anyway.
  if (layer.locked) return Clipboard{};

  Clipboard clip = copyThroughSelection(layer, selection);
  // Nothing was taken, so nothing is removed. Without this a cut through an
  // empty selection would be a clear with extra steps -- destroying paint the
  // user did not select.
  if (clip.empty()) return clip;

  if (layer.rgbTiles.has_value()) clearThroughSelection(*layer.rgbTiles, selection);
  if (layer.pigmentTiles.has_value()) clearThroughSelection(*layer.pigmentTiles, selection);
  return clip;
}

std::optional<size_t> pasteAsLayer(Document& doc, const Clipboard& clip, size_t atIndex) {
  if (clip.empty()) return std::nullopt;
  if (atIndex > doc.layers.size()) return std::nullopt;

  Layer layer = clip.kind == LayerKind::Pigment ? makePigmentLayer(clip.sourceName)
                                                : makeRgbLayer(clip.sourceName);
  // Assigning the stores shares every tile with the clipboard (TileStoreOf's
  // copy constructor is the sharing one). So pasting the same payload five
  // times costs five refcount increments, and the first paint on any of them
  // unshares only the tile it touches.
  if (clip.rgbTiles.has_value()) layer.rgbTiles = *clip.rgbTiles;
  if (clip.pigmentTiles.has_value()) layer.pigmentTiles = *clip.pigmentTiles;

  const LayerOpResult r = addLayer(doc, atIndex, std::move(layer));
  if (!r.ok) return std::nullopt;
  return r.index;
}

std::optional<size_t> selectionToNewLayer(Document& doc, size_t srcIndex,
                                          const Selection* selection) {
  if (srcIndex >= doc.layers.size()) return std::nullopt;
  const Clipboard clip = copyThroughSelection(doc.layers[srcIndex], selection);
  if (clip.empty()) return std::nullopt;
  return pasteAsLayer(doc, clip, srcIndex + 1);
}

}  // namespace np
