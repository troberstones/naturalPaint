#include "app/LayerThumbnail.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/Mask.hpp"
#include "core/PathRaster.hpp"
#include "core/PathStroke.hpp"
#include "core/TextContent.hpp"
#include "core/Tile.hpp"
#include "core/VectorRaster.hpp"
#include "core/VectorShape.hpp"

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

// ---------------------------------------------------------------------------
// §5: Vector and Text, the kinds with a picture and no tiles
// ---------------------------------------------------------------------------

// **This file's own question, and deliberately NOT `layerHoldsPixels()`.**
//
// That predicate answers "would the compositor's walk read tiles off this
// layer", and core/VectorRaster.hpp §1 records exactly what widening it costs:
// core/Composite.cpp, core/Merge.cpp, core/LayerOps.cpp and core/Probe.cpp each
// re-derive the same test inline, so flipping the shared one enables a layer
// those walks still skip -- and two of them go on to read `*l.pigmentTiles` for
// any layer that passed the guard and is not RGB, which for a Vector layer is a
// disengaged `std::optional` on every composite.
//
// The thumbnail asks something narrower -- "does this layer draw anything a
// 24 px picture could show" -- and Vector and Text answer yes to that while
// still answering no to the compositor's question. Two questions, two
// predicates, and the one line of overlap is cheaper than the class of bug that
// merging them creates.
bool layerHasThumbnailContent(const Layer& layer) noexcept {
  return layerHoldsPixels(layer) || layerRastersToTiles(layer.kind);
}

// Document texel space -> thumbnail cell space: exactly the letterbox, which is
// a scale and a translate and nothing else. `sx` and `sy` are separately
// derived so that the geometry lands on the SAME rect the tile sampler maps the
// document onto (`out.w` / `out.h` are rounded by `fitRect()`, so they differ
// from a single uniform scale by up to half a cell texel); they agree with each
// other to within that rounding, which is what makes a single stroke-width
// scale below honest.
struct CellMap {
  float sx = 1.0f;
  float sy = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;
};

PathPoint mapPoint(const CellMap& m, PathPoint p) noexcept {
  return PathPoint{p.x * m.sx + m.tx, p.y * m.sy + m.ty};
}

// One map over every point, with no special case for the handles -- which is
// the property core/Path.hpp §2 chose absolute handles to get, and the reason
// this is four lines rather than a linear-part-only branch someone forgets.
Path mapPath(const CellMap& m, const Path& in) {
  Path out = in;
  for (SubPath& sub : out.subpaths)
    for (Anchor& a : sub.anchors) {
      a.pt = mapPoint(m, a.pt);
      a.in = mapPoint(m, a.in);
      a.out = mapPoint(m, a.out);
    }
  return out;
}

// The flattening tolerance, in CELL texels. A tenth of one, which is
// core/VectorRaster.cpp's `kDocumentTolerancePx` reasoning applied in this
// space rather than that one: below what the 1/255 coverage quantisation can
// express, and the coordinates handed to the rasteriser are cell coordinates,
// so a document-space tolerance here would be 24/width of the intended one and
// would visibly facet a curve on any document wider than a few hundred texels.
constexpr float kCellTolerancePx = 0.1f;

// Rasterise `path` into the cell, handing `plot` a flat cell index and that
// texel's coverage. Every emitted texel is counted, which is what
// `LayerThumbnail::samples` reports for these kinds.
template <typename Plot>
void sweepCell(const Path& path, const RasterClip& clip, PathRasterScratch& scratch,
               size_t& emitted, Plot plot) {
  rasterizePath(path, kCellTolerancePx, clip, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x) {
                    plot(static_cast<size_t>(y) * kLayerThumbPx + static_cast<size_t>(x),
                         cov[x - x0]);
                    ++emitted;
                  }
                });
}

// Source-over, in LINEAR PREMULTIPLIED light -- core/VectorRaster.cpp's
// `paintCoverage()` at 1/100th the scale, and premultiplied for §2's reason:
// the un-premultiply happens once, at the very end, so a half-covered edge
// comes out at half coverage rather than at full coverage of a dimmer colour.
//
// `straight` is a `Paint::rgba`, which core/VectorShape.hpp defines as
// linear-light straight alpha -- the same space a `Tile` holds, so the encode
// at the end of `drawVectorContent()` is §1's single sRGB encode and not a
// second one.
void paintCell(std::vector<float>& cell, const Path& path, const std::array<float, 4>& straight,
               const std::vector<float>* clipCoverage, const RasterClip& clip,
               PathRasterScratch& scratch, size_t& emitted) {
  const float a = straight[3];
  if (!(a > 0.0f)) return;
  sweepCell(path, clip, scratch, emitted, [&](size_t i, float c) {
    if (clipCoverage != nullptr) c *= (*clipCoverage)[i];
    const float srcA = c * a;
    if (!(srcA > 0.0f)) return;
    const float inv = 1.0f - srcA;
    float* d = &cell[i * 4];
    d[0] = straight[0] * srcA + d[0] * inv;
    d[1] = straight[1] * srcA + d[1] * inv;
    d[2] = straight[2] * srcA + d[2] * inv;
    d[3] = srcA + d[3] * inv;
  });
}

// The whole of §5, into `out`'s already-letterboxed rect.
void drawVectorContent(const Layer& layer, int32_t docW, int32_t docH, LayerThumbnail& out) {
  // **Text takes the Vector path because it IS the Vector path** --
  // core/TextContent.hpp §1, and core/VectorRaster.cpp's materialise loop makes
  // the same two-line decision for the same reason. A second branch below here
  // would be a second renderer, and `--selftest` asserts the two agree byte for
  // byte precisely so one cannot appear unnoticed.
  const std::vector<VectorShape> shapes =
      layer.kind == LayerKind::Text ? textContentToShapes(layer.text) : layer.shapes;
  // No shapes is not a failure and not a black square: a Vector layer one click
  // of NEW old, or a Text layer nobody has typed into yet, draws nothing, and
  // an empty letterbox is the true picture of that. (A shaping failure --
  // invalid UTF-8, or a build with no shaper -- lands here too, and drawing
  // nothing is the honest answer to "what does this text look like" when the
  // answer is unknown.)
  if (shapes.empty()) return;

  const CellMap m{static_cast<float>(out.w) / static_cast<float>(docW),
                  static_cast<float>(out.h) / static_cast<float>(docH),
                  static_cast<float>(out.x), static_cast<float>(out.y)};
  // A stroke is scaled by ONE number, because a stroke width is one number.
  // The geometric mean is the area-preserving choice and `sx`/`sy` differ only
  // by `fitRect()`'s rounding (under 5% on the worst aspect a 24 px cell can
  // letterbox), so the anisotropy this drops is well under the 1/255 the
  // coverage is quantised to.
  const float widthScale = std::sqrt(m.sx * m.sy);

  // Nothing outside the letterbox is ever emitted -- which is this space's
  // version of `clipForPath()` clipping to the document, and is what keeps the
  // margin transparent when geometry runs off the edge of the canvas.
  const RasterClip clip{out.x, out.y, out.x + out.w, out.y + out.h};

  constexpr size_t kCellTexels = static_cast<size_t>(kLayerThumbPx) * kLayerThumbPx;
  std::vector<float> cell(kCellTexels * 4, 0.0f);  // linear, premultiplied
  std::vector<float> clipCoverage;
  PathRasterScratch scratch;  // grows to the widest clip it sees: 24 floats

  for (const VectorShape& shape : shapes) {
    const std::vector<float>* clipPtr = nullptr;
    if (shape.clip.has_value()) {
      clipCoverage.assign(kCellTexels, 0.0f);
      sweepCell(mapPath(m, *shape.clip), clip, scratch, out.samples,
                [&](size_t i, float c) { clipCoverage[i] = c; });
      // An engaged clip with no coverage anywhere hides the shape entirely, and
      // is distinct from no clip at all -- core/VectorRaster.cpp says the same
      // thing in the same place, and getting it backwards makes a
      // clipped-to-nothing shape paint over everything.
      clipPtr = &clipCoverage;
    }

    // SVG's order, and core/VectorRaster.cpp's: fill, then stroke, or the
    // stroke reads as a band under the fill rather than as an outline.
    if (shape.fill.on)
      paintCell(cell, mapPath(m, shape.path), shape.fill.rgba, clipPtr, clip, scratch,
                out.samples);

    if (shape.stroke.on && shape.strokeStyle.width > 0.0f) {
      // Scale the path and the width together, then stroke -- rather than
      // stroking at document scale and mapping the outline, which would flatten
      // curves to a tenth of a DOCUMENT texel to draw a 24 px square and is the
      // one place this could accidentally cost O(document).
      StrokeStyle style = shape.strokeStyle;
      style.width *= widthScale;
      for (float& d : style.dashes) d *= widthScale;  // dashes are user units too
      style.dashOffset *= widthScale;
      const Path outline = strokePath(mapPath(m, shape.path), style, kCellTolerancePx);
      if (!outline.subpaths.empty())
        paintCell(cell, outline, shape.stroke.rgba, clipPtr, clip, scratch, out.samples);
    }
  }

  // §1 and §2, once, at the end: un-premultiply, sRGB-encode the three colour
  // channels, leave alpha alone because a coverage is never gamma-encoded.
  for (int oy = 0; oy < out.h; ++oy) {
    for (int ox = 0; ox < out.w; ++ox) {
      const size_t i =
          static_cast<size_t>(out.y + oy) * kLayerThumbPx + static_cast<size_t>(out.x + ox);
      const float a = cell[i * 4 + 3];
      if (!(a > 0.0f)) continue;  // all four bytes stay zero, never a colour at alpha 0
      for (int c = 0; c < 3; ++c) {
        const float straight = std::min(1.0f, cell[i * 4 + static_cast<size_t>(c)] / a);
        out.rgba[i * 4 + static_cast<size_t>(c)] = toByte(srgbEncode(straight));
      }
      out.rgba[i * 4 + 3] = toByte(a);
    }
  }
}

}  // namespace

LayerThumbnail layerContentThumbnail(const Document& doc, size_t layerIndex) {
  LayerThumbnail out = emptyThumb();
  if (layerIndex >= doc.layers.size()) return out;
  const Layer& layer = doc.layers[layerIndex];
  // The thumbnail's own question, not the compositor's -- see
  // `layerHasThumbnailContent()` above and §5 for why the two must stay apart.
  if (!layerHasThumbnailContent(layer)) return out;
  fitRect(doc.width, doc.height, kLayerThumbPx, out.x, out.y, out.w, out.h);
  if (out.w == 0 || out.h == 0) return out;

  // §5. A layer whose content is geometry is rasterised into the cell; there
  // are no tiles below this line to sample.
  if (layerRastersToTiles(layer.kind)) {
    drawVectorContent(layer, doc.width, doc.height, out);
    return out;
  }

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
