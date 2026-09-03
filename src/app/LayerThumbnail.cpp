#include "app/LayerThumbnail.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/Mask.hpp"
#include "core/Tile.hpp"

namespace np {
namespace {

// A float in [0,1] to a byte, rounded rather than truncated. Truncation puts
// 1.0 on 254 for anything short of exactly 255.0f and is the reason a "white"
// thumbnail can read one notch grey next to a chrome white; the round is what
// makes the assertions in `--selftest` exact numbers rather than ranges.
uint8_t toByte(float v) noexcept {
  if (!(v > 0.0f)) return 0;  // also catches NaN, the same guard shape
                              // `maskCoverageClamp()` and `layerCoverage()` use
  if (v >= 1.0f) return 255;
  return static_cast<uint8_t>(std::lround(v * 255.0f));
}

// The letterbox: the largest `docW x docH`-shaped rect that fits in the cell.
// Never zero-sized for a document with area, because a 1x4096 document would
// otherwise round to a width of 0 and vanish -- a thumbnail of one column is
// still a true statement about the document, an empty square is not.
void fitRect(int32_t docW, int32_t docH, int cell, int& xOut, int& yOut, int& wOut, int& hOut) {
  if (docW <= 0 || docH <= 0) {
    xOut = yOut = wOut = hOut = 0;
    return;
  }
  const double scale = std::min(static_cast<double>(cell) / static_cast<double>(docW),
                                static_cast<double>(cell) / static_cast<double>(docH));
  wOut = std::max(1, std::min(cell, static_cast<int>(std::lround(docW * scale))));
  hOut = std::max(1, std::min(cell, static_cast<int>(std::lround(docH * scale))));
  xOut = (cell - wOut) / 2;
  yOut = (cell - hOut) / 2;
}

// The `s`-th of `kThumbSupersample` sample positions inside `[lo, hi)`, at the
// centre of its own sub-interval so the set is symmetric about the footprint's
// middle. Clamped into `[lo, hi-1]` because a footprint narrower than the
// sample count legitimately repeats a source texel -- a 24 px thumbnail of an
// 8 px document is four samples of the same texel, which is the right answer
// and not a degenerate case to refuse.
int32_t sampleAt(int32_t lo, int32_t hi, int s) noexcept {
  const int32_t span = hi - lo;
  if (span <= 0) return lo;
  const int32_t off = static_cast<int32_t>((static_cast<int64_t>(2 * s + 1) * span) /
                                           (2 * kThumbSupersample));
  return lo + std::min(off, span - 1);
}

LayerThumbnail emptyThumb() {
  LayerThumbnail t;
  t.rgba.assign(static_cast<size_t>(kLayerThumbPx) * kLayerThumbPx * 4, 0);
  return t;
}

}  // namespace

LayerThumbnail layerContentThumbnail(const Document& doc, size_t layerIndex) {
  LayerThumbnail out = emptyThumb();
  if (layerIndex >= doc.layers.size()) return out;
  const Layer& layer = doc.layers[layerIndex];
  // One question, asked with the compositor's own predicate rather than by
  // re-listing the kinds that have stores. `core/Composite.hpp` exports
  // `layerHoldsPixels()` precisely so that "a layer the walk would composite
  // from" cannot become two different ideas in two files, and a thumbnail is
  // exactly that question asked by a panel.
  if (!layerHoldsPixels(layer)) return out;
  fitRect(doc.width, doc.height, kLayerThumbPx, out.x, out.y, out.w, out.h);
  if (out.w == 0 || out.h == 0) return out;

  const TileStore* rgb = layer.rgbTiles.has_value() ? &*layer.rgbTiles : nullptr;
  const PigmentTileStore* pig = layer.pigmentTiles.has_value() ? &*layer.pigmentTiles : nullptr;

  for (int oy = 0; oy < out.h; ++oy) {
    // The source rows this output row stands for: `[sy0, sy1)`, the same
    // half-open convention on both axes so adjacent output texels neither
    // overlap nor leave a gap.
    const int32_t sy0 = static_cast<int32_t>((static_cast<int64_t>(oy) * doc.height) / out.h);
    const int32_t sy1 = static_cast<int32_t>((static_cast<int64_t>(oy + 1) * doc.height) / out.h);
    for (int ox = 0; ox < out.w; ++ox) {
      const int32_t sx0 = static_cast<int32_t>((static_cast<int64_t>(ox) * doc.width) / out.w);
      const int32_t sx1 = static_cast<int32_t>((static_cast<int64_t>(ox + 1) * doc.width) / out.w);

      // Averaged PREMULTIPLIED, un-premultiplied once at the end -- §2. A
      // texel the layer has never been painted on contributes an honest four
      // zeroes, which is what makes the average over a half-covered edge come
      // out at half coverage rather than at full coverage of a dimmer colour.
      std::array<double, 4> acc{0.0, 0.0, 0.0, 0.0};
      int taken = 0;
      for (int sj = 0; sj < kThumbSupersample; ++sj) {
        const int32_t y = sampleAt(sy0, sy1, sj);
        for (int si = 0; si < kThumbSupersample; ++si) {
          const int32_t x = sampleAt(sx0, sx1, si);
          const PixelCoord at{x, y};
          const TileCoord coord = tileCoordAt(at);
          const PixelCoord local = tileLocalOffset(at);
          ++taken;
          if (rgb != nullptr) {
            const Tile* tile = rgb->find(coord);
            if (tile == nullptr) continue;  // absent means transparent black
            const std::array<float, 4> px = tile->readPixel(local);
            for (int c = 0; c < 4; ++c) acc[c] += px[c];
          } else if (pig != nullptr) {
            const PigmentTile* tile = pig->find(coord);
            if (tile == nullptr) continue;
            // The compositor's own latent -> premultiplied RGBA projection, not
            // a second one. A Pigment texel holds a Mixbox `Latent` scaled by
            // mass (core/Pigment.hpp); anything else here would be a thumbnail
            // of a different picture from the one the canvas shows.
            const std::array<float, 4> px = projectPigmentTexel(tile->readTexel(local));
            for (int c = 0; c < 4; ++c) acc[c] += px[c];
          }
        }
      }
      out.samples += static_cast<size_t>(taken);
      if (taken == 0) continue;

      const double inv = 1.0 / static_cast<double>(taken);
      const float a = static_cast<float>(acc[3] * inv);
      const size_t o = (static_cast<size_t>(out.y + oy) * kLayerThumbPx +
                        static_cast<size_t>(out.x + ox)) *
                       4;
      if (!(a > 0.0f)) {
        // Nothing here. All four bytes stay zero -- writing a colour at alpha 0
        // would be the malformed texel `brush/RgbErase.hpp` §1 warns about,
        // arriving in a thumbnail instead of a tile.
        continue;
      }
      // §1: the three colour channels are sRGB-encoded, the alpha is not,
      // because alpha is a coverage and a coverage is never gamma-encoded.
      for (int c = 0; c < 3; ++c) {
        const float straight = static_cast<float>(acc[c] * inv) / a;
        out.rgba[o + static_cast<size_t>(c)] = toByte(srgbEncode(straight));
      }
      out.rgba[o + 3] = toByte(a);
    }
  }
  return out;
}

LayerThumbnail layerMaskThumbnail(const Document& doc, size_t layerIndex) {
  LayerThumbnail out = emptyThumb();
  if (layerIndex >= doc.layers.size()) return out;
  const Layer& layer = doc.layers[layerIndex];
  if (!layer.mask.has_value()) return out;  // absent, which is not "reveals all"
  fitRect(doc.width, doc.height, kLayerThumbPx, out.x, out.y, out.w, out.h);
  if (out.w == 0 || out.h == 0) return out;
  const MaskTileStore& mask = *layer.mask;

  for (int oy = 0; oy < out.h; ++oy) {
    const int32_t sy0 = static_cast<int32_t>((static_cast<int64_t>(oy) * doc.height) / out.h);
    const int32_t sy1 = static_cast<int32_t>((static_cast<int64_t>(oy + 1) * doc.height) / out.h);
    for (int ox = 0; ox < out.w; ++ox) {
      const int32_t sx0 = static_cast<int32_t>((static_cast<int64_t>(ox) * doc.width) / out.w);
      const int32_t sx1 = static_cast<int32_t>((static_cast<int64_t>(ox + 1) * doc.width) / out.w);

      double acc = 0.0;
      int taken = 0;
      for (int sj = 0; sj < kThumbSupersample; ++sj) {
        const int32_t y = sampleAt(sy0, sy1, sj);
        for (int si = 0; si < kThumbSupersample; ++si) {
          const int32_t x = sampleAt(sx0, sx1, si);
          const PixelCoord at{x, y};
          const MaskTile* tile = mask.find(tileCoordAt(at));
          // **An unallocated mask tile means 1.0, not 0.0** (core/Mask.hpp),
          // which is the opposite reading from an absent content tile above.
          // Getting this branch the wrong way round would draw every
          // freshly-added mask as solid black -- "discovered by the user as a
          // black layer", which is the failure that header designed out.
          acc += tile == nullptr ? 1.0 : static_cast<double>(tile->readCoverage(
                                             tileLocalOffset(at)));
          ++taken;
        }
      }
      out.samples += static_cast<size_t>(taken);
      if (taken == 0) continue;

      // §1: **no encode.** A mask sample is an opacity, so 0.5 is byte 128.
      const uint8_t g = toByte(static_cast<float>(acc / static_cast<double>(taken)));
      const size_t o = (static_cast<size_t>(out.y + oy) * kLayerThumbPx +
                        static_cast<size_t>(out.x + ox)) *
                       4;
      out.rgba[o + 0] = g;
      out.rgba[o + 1] = g;
      out.rgba[o + 2] = g;
      out.rgba[o + 3] = 255;  // opaque: the letterbox margin is the only
                              // transparent part of a mask thumbnail
    }
  }
  return out;
}

const LayerThumbnailCache::Row& LayerThumbnailCache::rowFor(const Document& doc, size_t layerIndex,
                                                           uint64_t documentId,
                                                           uint64_t revision) {
  // §4. The whole map goes, not one entry: `revision` is document-wide and
  // cannot say which layer moved, and a reorder changes which layer an index
  // names without changing any layer at all.
  if (!primed_ || documentId != documentId_ || revision != revision_) {
    rows_.clear();
    documentId_ = documentId;
    revision_ = revision;
    primed_ = true;
  }
  const auto it = rows_.find(layerIndex);
  if (it != rows_.end()) return it->second;

  Row row;
  row.content = layerContentThumbnail(doc, layerIndex);
  row.mask = layerMaskThumbnail(doc, layerIndex);
  ++builds_;
  return rows_.emplace(layerIndex, std::move(row)).first->second;
}

}  // namespace np
