#include "core/Histogram.hpp"

#include <algorithm>
#include <cmath>

#include "color/Space.hpp"

namespace np {
namespace {

// Rec.709 luma weights -- the exact three literal constants
// shaders/grayscale_blit.wgsl already hardcodes for its own grayscale
// preview pass, re-typed here rather than re-derived or re-rounded, so the
// histogram's Luma channel and the grayscale preview agree on what "luma"
// means.
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

// straight[i] = premultiplied[i] / a for the RGB channels. Mirrors
// Probe.cpp's own unpremultiply() helper's guard style (a <= 0 returns
// black) even though every call site in this file has already skipped
// alpha <= 0 texels before calling this -- keeping the guard here makes the
// helper safe on its own terms rather than relying on every future caller
// to remember to pre-check, the same defensive posture Probe.cpp's version
// takes.
std::array<float, 3> unpremultiplyRgb(const std::array<float, 4>& premultiplied) {
  const float a = premultiplied[3];
  if (a <= 0.0f) return {0.0f, 0.0f, 0.0f};
  return {premultiplied[0] / a, premultiplied[1] / a, premultiplied[2] / a};
}

// bin_index = clamp(floor(displayValue * binCount), 0, binCount - 1) -- the
// standard "outliers pile into the end bins" convention, since displayValue
// can land outside [0,1] (unclamped scene-linear working space; see
// Histogram.hpp's header comment).
int32_t binIndex(float displayValue, int32_t binCount) {
  const int32_t idx =
      static_cast<int32_t>(std::floor(displayValue * static_cast<float>(binCount)));
  return std::clamp(idx, 0, binCount - 1);
}

// Bins one allocated tile's overlap with [regionMin, regionMax) into
// `result`. `tileCoord`/`tile` come straight from the TileStore's own
// iteration (TileStore.hpp's "iterate only the tiles that exist") -- no
// find() needed, this already holds the Tile&.
void binTileRegion(const Tile& tile, TileCoord tileCoord, PixelCoord regionMin,
                    PixelCoord regionMax, int32_t binCount, HistogramResult& result) {
  const PixelCoord tileMin = tileOrigin(tileCoord);
  const PixelCoord tileMax{tileMin.x + kTileSize, tileMin.y + kTileSize};

  // Intersect the tile's document-space box with the requested region;
  // skip entirely on no overlap. This is the whole reason this function
  // walks the store's allocated tiles instead of the region's pixels: a
  // large, mostly-empty region never touches an unallocated tile's worth of
  // missing pixels.
  const int32_t xMin = std::max(tileMin.x, regionMin.x);
  const int32_t yMin = std::max(tileMin.y, regionMin.y);
  const int32_t xMax = std::min(tileMax.x, regionMax.x);
  const int32_t yMax = std::min(tileMax.y, regionMax.y);
  if (xMin >= xMax || yMin >= yMax) return;

  for (int32_t y = yMin; y < yMax; ++y) {
    for (int32_t x = xMin; x < xMax; ++x) {
      const PixelCoord local{x - tileMin.x, y - tileMin.y};
      const std::array<float, 4> premultiplied = tile.readPixel(local);

      // Skip fully-transparent texels entirely -- they increment no bin.
      // Unlike core::Probe's box-average (which sums premultiplied values
      // and un-premultiplies once, so an alpha-0 texel dilutes coverage
      // without darkening the reported colour -- see Probe.cpp's own header
      // comment), a histogram bins each pixel independently rather than
      // averaging into a box, so there is no "dilution vs. darkening"
      // tradeoff to solve here: a histogram simply shouldn't count "no
      // content" as a data point.
      if (premultiplied[3] <= 0.0f) continue;

      const std::array<float, 3> straight = unpremultiplyRgb(premultiplied);
      const float rDisplay = srgbEncode(straight[0]);
      const float gDisplay = srgbEncode(straight[1]);
      const float bDisplay = srgbEncode(straight[2]);
      // Luma is computed from the display-domain RGB above, not the linear
      // straight[] values, so all four histograms share one consistent
      // (display) domain.
      const float lumaDisplay = kLumaR * rDisplay + kLumaG * gDisplay + kLumaB * bDisplay;

      result.r[static_cast<size_t>(binIndex(rDisplay, binCount))] += 1;
      result.g[static_cast<size_t>(binIndex(gDisplay, binCount))] += 1;
      result.b[static_cast<size_t>(binIndex(bDisplay, binCount))] += 1;
      result.luma[static_cast<size_t>(binIndex(lumaDisplay, binCount))] += 1;
      ++result.sampleCount;
    }
  }
}

}  // namespace

HistogramResult computeHistogram(const Document& doc, const HistogramParams& params) {
  const int32_t binCount = params.binCount > 0 ? params.binCount : 1;

  HistogramResult result;
  result.r.assign(static_cast<size_t>(binCount), uint64_t{0});
  result.g.assign(static_cast<size_t>(binCount), uint64_t{0});
  result.b.assign(static_cast<size_t>(binCount), uint64_t{0});
  result.luma.assign(static_cast<size_t>(binCount), uint64_t{0});

  auto accumulate = [&](const Layer& layer) {
    if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) return;
    // Iterates exactly the tiles TileStore actually has allocated
    // (TileStore.hpp's begin()/end()), each intersected against the
    // requested region inside binTileRegion() -- never a per-pixel find()
    // over the whole region.
    for (const auto& [tileCoord, tile] : *layer.rgbTiles) {
      binTileRegion(tile, tileCoord, params.regionMin, params.regionMax, binCount, result);
    }
  };

  if (params.sampleAllLayers) {
    // Same plain-sum-across-layers reasoning as core::Probe's own
    // sampleAllLayers branch (Probe.cpp): today's Document invariant is at
    // most one populated RGB layer, so a plain sum is correct now and is
    // exactly where real per-layer compositing has to replace it once
    // multi-layer stacks exist -- see ProbeParams::sampleAllLayers's doc
    // comment (core/Probe.hpp) for the full reasoning, not re-litigated
    // here.
    for (const Layer& layer : doc.layers) accumulate(layer);
  } else if (params.activeLayerIndex >= 0 &&
             static_cast<size_t>(params.activeLayerIndex) < doc.layers.size()) {
    accumulate(doc.layers[static_cast<size_t>(params.activeLayerIndex)]);
  }

  return result;
}

}  // namespace np
