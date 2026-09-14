#include "ops/LensBlur.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/Parallel.hpp"
#include "ops/Filters.hpp"  // kFilterMaxLinear

namespace np {

namespace {

// Loose relative to `radius`'s integer scale (at most 64): a boundary edge
// this far from an exact integer is treated as landing ON the texel rather
// than being tipped one way or the other by float rounding of `cos`/`sin`.
constexpr double kApertureEpsilon = 1e-4;
constexpr double kPiD = 3.14159265358979323846;

// One row of the aperture, from its geometric definition directly (§ file
// comment: this is the closed form the O(radius) scanline decomposition
// exists to make possible). Circle: `tx^2 + dy^2 <= radius^2`. Polygon: a
// regular `bladeCount`-gon inscribed in `radius`, as `bladeCount` half-plane
// constraints through its apothem -- edge `k`'s outward normal bisects
// vertices `k` and `k+1`, at angle `rotation + (k+0.5) * (2*pi/bladeCount)`.
LensApertureRow rowSpanAt(int32_t bladeCount, float rotation, int32_t radius,
                         int32_t dy) noexcept {
  const double r = radius;
  if (bladeCount == 0) {
    const double disc = r * r - static_cast<double>(dy) * dy;
    if (disc < -kApertureEpsilon) return LensApertureRow{};
    const double half = std::sqrt(std::max(disc, 0.0));
    const int32_t hiX = static_cast<int32_t>(std::floor(half + kApertureEpsilon));
    return LensApertureRow{true, -hiX, hiX};
  }

  const double apothem = r * std::cos(kPiD / bladeCount);
  double lo = -1e30, hi = 1e30;
  for (int32_t k = 0; k < bladeCount; ++k) {
    const double phi =
        static_cast<double>(rotation) + (k + 0.5) * (2.0 * kPiD / bladeCount);
    const double nx = std::cos(phi);
    const double ny = std::sin(phi);
    const double rhs = apothem - ny * static_cast<double>(dy);
    if (std::fabs(nx) < kApertureEpsilon) {
      if (-rhs > kApertureEpsilon) return LensApertureRow{};  // nx*tx term vanishes: 0 <= rhs
      continue;
    }
    const double bound = rhs / nx;
    if (nx > 0.0) {
      hi = std::min(hi, bound);
    } else {
      lo = std::max(lo, bound);
    }
  }
  if (lo > hi + kApertureEpsilon) return LensApertureRow{};
  const int32_t loX = static_cast<int32_t>(std::ceil(lo - kApertureEpsilon));
  const int32_t hiX = static_cast<int32_t>(std::floor(hi + kApertureEpsilon));
  if (loX > hiX) return LensApertureRow{};
  return LensApertureRow{true, loX, hiX};
}

// Rec. 709 luma of PREMULTIPLIED RGB, ops/Filters.hpp §7's emboss convention
// restated: no un-premultiply anywhere in this file.
float luma(const std::array<float, 4>& t) noexcept {
  return 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
}

float clampStorable(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;
  return v > kFilterMaxLinear ? kFilterMaxLinear : v;
}

bool gatherRawPlane(const TileStore& src, const PixelRect& outRect, const RoiOp& roi,
                    PixelRect* need, std::vector<float>* plane) {
  *need = roiBackward(roi, outRect);
  const int32_t w = need->width();
  const int32_t h = need->height();
  if (w <= 0 || h <= 0) return false;

  plane->assign(static_cast<size_t>(w) * static_cast<size_t>(h) *
                    static_cast<size_t>(Tile::kChannels),
                0.0f);

  const TileRange gatherTiles = roiTileRange(*need);
  const int32_t tilesWide = gatherTiles.tilesWide();
  const PixelRect needRect = *need;
  float* const planeData = plane->data();
  parallelFor(static_cast<size_t>(gatherTiles.tileCount()), kParallelForDefaultGrain,
             [&](size_t i) {
               const int32_t tx = gatherTiles.x0 + static_cast<int32_t>(i) % tilesWide;
               const int32_t ty = gatherTiles.y0 + static_cast<int32_t>(i) / tilesWide;
               const TileCoord coord{tx, ty};
               const Tile* tile = src.find(coord);
               if (tile == nullptr) return;
               const PixelRect span = roiIntersect(roiTileRect(coord), needRect);
               const PixelCoord origin = tileOrigin(coord);
               for (int32_t y = span.y0; y < span.y1; ++y) {
                 for (int32_t x = span.x0; x < span.x1; ++x) {
                   const std::array<float, 4> rgba =
                       tile->readPixel(PixelCoord{x - origin.x, y - origin.y});
                   const size_t base =
                       (static_cast<size_t>(y - needRect.y0) * static_cast<size_t>(w) +
                        static_cast<size_t>(x - needRect.x0)) *
                       static_cast<size_t>(Tile::kChannels);
                   planeData[base + 0] = rgba[0];
                   planeData[base + 1] = rgba[1];
                   planeData[base + 2] = rgba[2];
                   planeData[base + 3] = rgba[3];
                 }
               }
             });
  return true;
}

}  // namespace

std::vector<LensApertureRow> lensApertureRowSpans(int32_t bladeCount, float bladeRotationRadians,
                                                  int32_t radius) {
  std::vector<LensApertureRow> rows;
  rows.reserve(static_cast<size_t>(2 * radius + 1));
  for (int32_t dy = -radius; dy <= radius; ++dy) {
    rows.push_back(rowSpanAt(bladeCount, bladeRotationRadians, radius, dy));
  }
  return rows;
}

int64_t lensApertureTexelCount(int32_t bladeCount, float bladeRotationRadians, int32_t radius) {
  int64_t total = 0;
  for (const LensApertureRow& row : lensApertureRowSpans(bladeCount, bladeRotationRadians, radius))
    if (row.valid) total += static_cast<int64_t>(row.hiX - row.loX + 1);
  return total;
}

bool lensBlurParamsValid(const LensBlurParams& p) noexcept {
  if (p.radius < 0 || p.radius > kLensBlurMaxRadius) return false;
  if (p.bladeCount != 0 && (p.bladeCount < 3 || p.bladeCount > 8)) return false;
  if (!std::isfinite(p.bladeRotationRadians) || !std::isfinite(p.highlightThreshold)) return false;
  if (!std::isfinite(p.highlightBoost) || p.highlightBoost < 0.0f) return false;
  return true;
}

RoiOp lensBlurRoiOp(const LensBlurParams& p) noexcept {
  if (p.radius == 0) return RoiOp{};
  return roiDilateOp(p.radius);
}

bool lensBlurTiles(const TileStore& src, const PixelRect& outRect, const LensBlurParams& p,
                   TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!lensBlurParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  if (p.radius == 0) {
    // Bit-exact identity: the aperture is a single texel, so there is
    // nothing this filter could change. Same short-circuit convention as
    // ops/Filters.hpp's zero-strength filters -- skip the gather entirely
    // rather than pay for a one-tap "convolution".
    const TileRange writeTiles = roiTileRange(outRect);
    for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
      for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
        const TileCoord coord{tx, ty};
        const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
        if (roiIsEmpty(span)) continue;
        const Tile* srcTile = src.find(coord);
        if (srcTile == nullptr) continue;
        Tile& outTile = dst->getOrCreate(coord);
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t y = span.y0; y < span.y1; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            const PixelCoord local{x - origin.x, y - origin.y};
            outTile.writePixel(local, srcTile->readPixel(local));
          }
        }
      }
    }
    return true;
  }

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherRawPlane(src, outRect, lensBlurRoiOp(p), &need, &plane)) return false;

  const int32_t w = need.width();
  const int32_t h = need.height();

  // Specular boost: a per-source-texel RGB scale, applied before the
  // aperture convolves it -- ops/LensBlur.hpp's own comment on why alpha is
  // untouched and why `highlightBoost == 0` leaves this a no-op.
  if (p.highlightBoost > 0.0f) {
    parallelFor(static_cast<size_t>(w) * static_cast<size_t>(h), kParallelForDefaultGrain,
               [&](size_t i) {
                 float* px = &plane[i * Tile::kChannels];
                 const std::array<float, 4> t{px[0], px[1], px[2], px[3]};
                 if (luma(t) > p.highlightThreshold) {
                   const float gain = 1.0f + p.highlightBoost;
                   px[0] *= gain;
                   px[1] *= gain;
                   px[2] *= gain;
                 }
               });
  }

  // Per-row EXCLUSIVE prefix sums: `prefix[row][x]` is the sum of texels
  // `[need.x0, need.x0 + x)` in that row. `prefix[row][b] - prefix[row][a]`
  // is then the sum of `[need.x0 + a, need.x0 + b)` in O(1) regardless of
  // width -- the whole reason this file is not the doc's brute force.
  std::vector<float> prefix(static_cast<size_t>(w + 1) * static_cast<size_t>(h) *
                            static_cast<size_t>(Tile::kChannels), 0.0f);
  parallelFor(static_cast<size_t>(h), kParallelForDefaultGrain, [&](size_t rowI) {
    const size_t rowBase = rowI * static_cast<size_t>(w) * Tile::kChannels;
    const size_t prefixRowBase = rowI * static_cast<size_t>(w + 1) * Tile::kChannels;
    for (int32_t c = 0; c < Tile::kChannels; ++c) prefix[prefixRowBase + static_cast<size_t>(c)] = 0.0f;
    for (int32_t x = 0; x < w; ++x) {
      for (int32_t c = 0; c < Tile::kChannels; ++c) {
        prefix[prefixRowBase + static_cast<size_t>(x + 1) * Tile::kChannels +
              static_cast<size_t>(c)] =
            prefix[prefixRowBase + static_cast<size_t>(x) * Tile::kChannels +
                  static_cast<size_t>(c)] +
            plane[rowBase + static_cast<size_t>(x) * Tile::kChannels + static_cast<size_t>(c)];
      }
    }
  });

  const std::vector<LensApertureRow> rows =
      lensApertureRowSpans(p.bladeCount, p.bladeRotationRadians, p.radius);
  const int64_t area = lensApertureTexelCount(p.bladeCount, p.bladeRotationRadians, p.radius);
  const float invArea = area > 0 ? 1.0f / static_cast<float>(area) : 0.0f;
  const int32_t radius = p.radius;

  const TileRange writeTiles = roiTileRange(outRect);
  struct ReservedTile {
    PixelRect span;
    PixelCoord origin;
    Tile* tile;
  };
  std::vector<ReservedTile> reserved;
  reserved.reserve(static_cast<size_t>(writeTiles.tileCount()));
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& tile = dst->getOrCreate(coord);
      reserved.push_back(ReservedTile{span, tileOrigin(coord), &tile});
    }
  }

  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t ri) {
    const ReservedTile& r = reserved[ri];
    for (int32_t y = r.span.y0; y < r.span.y1; ++y) {
      for (int32_t x = r.span.x0; x < r.span.x1; ++x) {
        std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
        for (int32_t i = 0; i < static_cast<int32_t>(rows.size()); ++i) {
          const LensApertureRow& row = rows[static_cast<size_t>(i)];
          if (!row.valid) continue;
          const int32_t dy = i - radius;
          const int32_t srcRow = (y + dy) - need.y0;
          const int32_t a = (x + row.loX) - need.x0;
          const int32_t b = (x + row.hiX) - need.x0 + 1;  // exclusive
          const size_t prefixRowBase =
              static_cast<size_t>(srcRow) * static_cast<size_t>(w + 1) * Tile::kChannels;
          for (int32_t c = 0; c < Tile::kChannels; ++c) {
            sum[static_cast<size_t>(c)] +=
                prefix[prefixRowBase + static_cast<size_t>(b) * Tile::kChannels +
                      static_cast<size_t>(c)] -
                prefix[prefixRowBase + static_cast<size_t>(a) * Tile::kChannels +
                      static_cast<size_t>(c)];
          }
        }
        const PixelCoord local{x - r.origin.x, y - r.origin.y};
        r.tile->writePixel(local, std::array<float, 4>{clampStorable(sum[0] * invArea),
                                                        clampStorable(sum[1] * invArea),
                                                        clampStorable(sum[2] * invArea),
                                                        clampStorable(sum[3] * invArea)});
      }
    }
  });
  return true;
}

}  // namespace np
