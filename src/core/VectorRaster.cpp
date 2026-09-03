#include "core/VectorRaster.hpp"

#include <algorithm>
#include <cmath>

#include "core/PathFlatten.hpp"
#include "core/PathStroke.hpp"
#include "core/SelectionMask.hpp"
#include "core/TextContent.hpp"

namespace np {
namespace {

// The flattening tolerance used when rasterising a layer into document texels.
//
// A tenth of a texel: below what the 1/255 coverage quantisation downstream
// can express, so tightening it further buys nothing visible while costing
// segments quadratically (core/PathFlatten's count goes as 1/sqrt(tol)).
// Deliberately NOT the tolerance an on-screen overlay should use -- an overlay
// at 8x zoom needs an eighth of this, and that is the caller's decision.
constexpr float kDocumentTolerancePx = 0.1f;

// Paint one already-built coverage source over the tile store, "over" in
// linear premultiplied space.
//
// `clip` may be null. When present its coverage multiplies the shape's, which
// is exactly what SVG's `clip-path` means and is why core/PathRaster emits
// spans rather than owning a destination: intersecting two coverages is a
// multiply over a row with no intermediate image.
void paintCoverage(TileStore& out, const Path& path, const std::array<float, 4>& straightRgba,
                   const Selection* clip, int32_t width, int32_t height,
                   PathRasterScratch& scratch) {
  const RasterClip clipRect = clipForPath(path, width, height);
  if (clipRect.x1 <= clipRect.x0 || clipRect.y1 <= clipRect.y0) return;

  const float r = straightRgba[0], g = straightRgba[1], b = straightRgba[2];
  const float a = straightRgba[3];
  if (!(a > 0.0f)) return;

  rasterizePath(path, kDocumentTolerancePx, clipRect, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    float c = cov[x - x0];
                    const PixelCoord at{x, y};
                    if (clip != nullptr) {
                      c *= selectionCoverageAt(clip, at);
                      if (!(c > 0.0f)) continue;
                    }
                    const float srcA = c * a;
                    if (!(srcA > 0.0f)) continue;

                    Tile& tile = out.getOrCreate(tileCoordAt(at));
                    const PixelCoord local = tileLocalOffset(at);
                    const std::array<float, 4> dst = tile.readPixel(local);
                    // Source-over, premultiplied. `straightRgba` is straight,
                    // so the source premultiplies here and nowhere else --
                    // core/VectorShape.hpp's stated convention.
                    const float inv = 1.0f - srcA;
                    tile.writePixel(local, {r * srcA + dst[0] * inv, g * srcA + dst[1] * inv,
                                            b * srcA + dst[2] * inv, srcA + dst[3] * inv});
                  }
                });
}

// A clip path as an 8-bit sparse coverage mask.
//
// `Selection` is reused rather than a new type invented: it is already
// "sparse, tiled, antialiased coverage in [0,1]", which is precisely a clip,
// and core/SelectionMask.hpp already argues that 8 bits is the right depth for
// exactly this job (Photoshop stores selections at 8 bits regardless of
// document depth). A clip edge therefore quantises to 1/255, which is the same
// step the selection tools have always produced.
Selection clipCoverage(const Path& path, int32_t width, int32_t height,
                       PathRasterScratch& scratch) {
  Selection sel;
  const RasterClip rect = clipForPath(path, width, height);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) return sel;
  rasterizePath(path, kDocumentTolerancePx, rect, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    const PixelCoord at{x, y};
                    sel.tiles.getOrCreate(tileCoordAt(at))
                        .writeCoverage(tileLocalOffset(at), cov[x - x0]);
                  }
                });
  return sel;
}

}  // namespace

TileStore rasterizeVectorLayer(const std::vector<VectorShape>& shapes, int32_t width,
                               int32_t height) {
  TileStore out;
  if (width <= 0 || height <= 0) return out;

  // One scratch for the whole layer: core/PathRaster's accumulators grow to
  // the widest clip they have seen and are then reused, so a layer of a
  // thousand shapes allocates them once.
  PathRasterScratch scratch;

  for (const VectorShape& shape : shapes) {
    std::optional<Selection> clip;
    if (shape.clip.has_value()) {
      clip = clipCoverage(*shape.clip, width, height, scratch);
      // An engaged clip with no coverage anywhere hides the shape entirely.
      // Distinct from no clip at all, exactly as core/SelectionMask.hpp
      // distinguishes an empty selection from an absent one -- and getting it
      // backwards here would make a clipped-to-nothing shape paint over
      // everything.
      if (clip->tiles.occupiedTileCount() == 0) continue;
    }
    const Selection* clipPtr = clip.has_value() ? &*clip : nullptr;

    // SVG's order, and the only one under which a stroke reads as an outline.
    if (shape.fill.on)
      paintCoverage(out, shape.path, shape.fill.rgba, clipPtr, width, height, scratch);

    if (shape.stroke.on && shape.strokeStyle.width > 0.0f) {
      const Path outline = strokePath(shape.path, shape.strokeStyle, kDocumentTolerancePx);
      if (!outline.subpaths.empty())
        paintCoverage(out, outline, shape.stroke.rgba, clipPtr, width, height, scratch);
    }
  }
  return out;
}

std::shared_ptr<const TileStore> VectorRasterCache::lookup(uint64_t layerId,
                                                           uint64_t hash) const {
  const auto it = byLayer_.find(layerId);
  if (it == byLayer_.end()) return nullptr;
  if (it->second.hash != hash) return nullptr;  // stale: never hand it back
  return it->second.tiles;
}

std::shared_ptr<const TileStore> VectorRasterCache::store(uint64_t layerId, uint64_t hash,
                                                          TileStore tiles) {
  auto shared = std::make_shared<const TileStore>(std::move(tiles));
  byLayer_[layerId] = Entry{hash, shared};
  return shared;
}

void VectorRasterCache::forgetLayersNotIn(const Document& doc) {
  for (auto it = byLayer_.begin(); it != byLayer_.end();) {
    bool present = false;
    for (const Layer& l : doc.layers)
      if (layerRastersToTiles(l.kind) && l.id == it->first) {
        present = true;
        break;
      }
    it = present ? std::next(it) : byLayer_.erase(it);
  }
}

size_t VectorRasterCache::residentBytes() const noexcept {
  size_t total = 0;
  for (const auto& [id, entry] : byLayer_) {
    (void)id;
    if (entry.tiles) total += entry.tiles->occupiedTileCount() * sizeof(Tile);
  }
  return total;
}

bool layerRastersToTiles(LayerKind kind) noexcept {
  return kind == LayerKind::Vector || kind == LayerKind::Text;
}

bool documentHasVectorLayers(const Document& doc) noexcept {
  for (const Layer& l : doc.layers)
    if (layerRastersToTiles(l.kind)) return true;
  return false;
}

MaterializedDocument::MaterializedDocument(const Document& doc, VectorRasterCache* cache)
    : original_(&doc) {
  // The fast path every existing caller takes: no Vector layer, no copy.
  if (!documentHasVectorLayers(doc)) return;

  Document copy = doc;
  for (Layer& layer : copy.layers) {
    if (!layerRastersToTiles(layer.kind)) continue;

    // **Text takes the same path as Vector because it IS the same path.**
    // `textContentToShapes()` produces the identical `std::vector<VectorShape>`
    // a Vector layer stores, so the only per-kind work is deciding which
    // content to hash and where the shapes come from -- everything past these
    // four lines is shared, which is core/TextContent.hpp section 1's whole
    // claim made concrete.
    const bool isText = layer.kind == LayerKind::Text;
    const uint64_t hash =
        isText ? textContentHash(layer.text) : vectorContentHash(layer.shapes);
    std::shared_ptr<const TileStore> tiles =
        cache ? cache->lookup(layer.id, hash) : nullptr;
    if (!tiles) {
      // Shaping happens ONLY on a cache miss. A hit skips it entirely, which
      // matters more for text than for geometry: shaping is a CoreText call
      // and a per-frame one would be visible.
      const std::vector<VectorShape> shapes =
          isText ? textContentToShapes(layer.text) : layer.shapes;
      TileStore built = rasterizeVectorLayer(shapes, doc.width, doc.height);
      tiles = cache ? cache->store(layer.id, hash, std::move(built))
                    : std::make_shared<const TileStore>(std::move(built));
    }

    // Becomes an ordinary RGB layer for the compositor's purposes. Everything
    // else about the layer -- name, opacity, blend, visibility, mask, clip
    // flag, group tag, id -- is carried unchanged, because the compositor
    // reads all of it and a Vector layer must obey the same stack rules as
    // any other.
    //
    // **This document must never be saved.** These layers now claim to be
    // RGB, and writing that out would turn editable geometry into pixels.
    // io/NpaintFile is only ever handed the real document.
    layer.kind = LayerKind::RGB;
    layer.rgbTiles = *tiles;  // shares tiles; copies the slot map only
    layer.shapes.clear();
    layer.text = TextContent{};
  }
  rewritten_ = std::move(copy);
}

}  // namespace np
