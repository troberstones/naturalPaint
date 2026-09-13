#include "ops/RadialBlur.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/Parallel.hpp"
#include "ops/Filters.hpp"  // kFilterMaxLinear

namespace np {

namespace {

// A near-copy of ops/Filters.cpp's own gather/bilinear/scatter trio --
// ops/Filters.cpp's own header explains why a third caller does not yet earn
// pulling these into a shared header: doing so means editing that file, which
// is another track's.

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

std::array<float, 4> planeBilinear(const std::vector<float>& plane, const PixelRect& need,
                                   float fx, float fy) {
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  auto at = [&](int32_t x, int32_t y) -> std::array<float, 4> {
    if (x < need.x0 || x >= need.x1 || y < need.y0 || y >= need.y1) {
      return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const size_t base = (static_cast<size_t>(y - need.y0) * static_cast<size_t>(need.width()) +
                         static_cast<size_t>(x - need.x0)) *
                        static_cast<size_t>(Tile::kChannels);
    return {plane[base + 0], plane[base + 1], plane[base + 2], plane[base + 3]};
  };
  const std::array<float, 4> p00 = at(x0, y0);
  const std::array<float, 4> p10 = at(x0 + 1, y0);
  const std::array<float, 4> p01 = at(x0, y0 + 1);
  const std::array<float, 4> p11 = at(x0 + 1, y0 + 1);
  std::array<float, 4> out{};
  for (int32_t c = 0; c < 4; ++c) {
    const size_t i = static_cast<size_t>(c);
    const float top = p00[i] + tx * (p10[i] - p00[i]);
    const float bottom = p01[i] + tx * (p11[i] - p01[i]);
    out[i] = top + ty * (bottom - top);
  }
  return out;
}

float clampStorable(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;
  return v > kFilterMaxLinear ? kFilterMaxLinear : v;
}

void scatterIdentity(const TileStore& src, const PixelRect& outRect, TileStore* dst) {
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
}

template <class Sample>
void scatterPlaneParallel(const PixelRect& outRect, TileStore* dst, Sample&& sample) {
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
  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t i) {
    const ReservedTile& r = reserved[i];
    for (int32_t y = r.span.y0; y < r.span.y1; ++y) {
      for (int32_t x = r.span.x0; x < r.span.x1; ++x) {
        r.tile->writePixel(PixelCoord{x - r.origin.x, y - r.origin.y}, sample(x, y));
      }
    }
  });
}

constexpr float kPi = 3.14159265358979323846f;

// The farthest `outRect` corner is from `(cx, cy)` -- see ops/RadialBlur.hpp's
// `radialBlurRoiOp()` comment for why a corner is always the worst case.
float farthestCornerDistance(const PixelRect& outRect, float cx, float cy) noexcept {
  const float xs[2] = {static_cast<float>(outRect.x0), static_cast<float>(outRect.x1)};
  const float ys[2] = {static_cast<float>(outRect.y0), static_cast<float>(outRect.y1)};
  float best = 0.0f;
  for (float x : xs) {
    for (float y : ys) {
      const float dx = x - cx;
      const float dy = y - cy;
      best = std::max(best, std::sqrt(dx * dx + dy * dy));
    }
  }
  return best;
}

}  // namespace

const char* radialBlurMethodName(RadialBlurMethod m) noexcept {
  switch (m) {
    case RadialBlurMethod::Spin: return "spin";
    case RadialBlurMethod::Zoom: return "zoom";
  }
  return "spin";
}

std::optional<RadialBlurMethod> radialBlurMethodFromName(std::string_view name) noexcept {
  if (name == "spin") return RadialBlurMethod::Spin;
  if (name == "zoom") return RadialBlurMethod::Zoom;
  return std::nullopt;
}

bool radialBlurParamsValid(const RadialBlurParams& p) noexcept {
  return std::isfinite(p.centerX) && std::isfinite(p.centerY) && std::isfinite(p.amount) &&
        p.samples >= 1;
}

RoiOp radialBlurRoiOp(const RadialBlurParams& p, const PixelRect& outRect) noexcept {
  if (!(p.amount != 0.0f)) return RoiOp{};
  const float rMax = farthestCornerDistance(outRect, p.centerX, p.centerY);
  const float reach = p.method == RadialBlurMethod::Spin
                          ? rMax * std::fabs(p.amount) * (kPi / 180.0f) * 0.5f
                          : rMax * std::fabs(p.amount);
  const int32_t margin = static_cast<int32_t>(std::ceil(reach)) + 1;
  return RoiOp{margin, margin, margin, margin, 0, 0};
}

bool radialBlurTiles(const TileStore& src, const PixelRect& outRect, const RadialBlurParams& p,
                     TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!radialBlurParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  if (!(p.amount != 0.0f)) {
    scatterIdentity(src, outRect, dst);
    return true;
  }

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherRawPlane(src, outRect, radialBlurRoiOp(p, outRect), &need, &plane)) return false;

  const int32_t samples = p.samples;
  const float invTaps = 1.0f / static_cast<float>(2 * samples + 1);
  const float cx = p.centerX;
  const float cy = p.centerY;
  const float amount = p.amount;

  if (p.method == RadialBlurMethod::Spin) {
    const float halfAngle = amount * (kPi / 180.0f) * 0.5f;
    scatterPlaneParallel(outRect, dst, [&](int32_t x, int32_t y) -> std::array<float, 4> {
      const float dx = static_cast<float>(x) - cx;
      const float dy = static_cast<float>(y) - cy;
      const float r = std::sqrt(dx * dx + dy * dy);
      const float theta0 = std::atan2(dy, dx);
      std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
      for (int32_t t = -samples; t <= samples; ++t) {
        const float frac = static_cast<float>(t) / static_cast<float>(samples);
        const float theta = theta0 + frac * halfAngle;
        const float fx = cx + r * std::cos(theta);
        const float fy = cy + r * std::sin(theta);
        const std::array<float, 4> s = planeBilinear(plane, need, fx, fy);
        sum[0] += s[0];
        sum[1] += s[1];
        sum[2] += s[2];
        sum[3] += s[3];
      }
      return std::array<float, 4>{clampStorable(sum[0] * invTaps), clampStorable(sum[1] * invTaps),
                                  clampStorable(sum[2] * invTaps), clampStorable(sum[3] * invTaps)};
    });
  } else {
    scatterPlaneParallel(outRect, dst, [&](int32_t x, int32_t y) -> std::array<float, 4> {
      const float dx = static_cast<float>(x) - cx;
      const float dy = static_cast<float>(y) - cy;
      std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
      for (int32_t t = -samples; t <= samples; ++t) {
        const float frac = static_cast<float>(t) / static_cast<float>(samples);
        const float scale = 1.0f + frac * amount;
        const float fx = cx + dx * scale;
        const float fy = cy + dy * scale;
        const std::array<float, 4> s = planeBilinear(plane, need, fx, fy);
        sum[0] += s[0];
        sum[1] += s[1];
        sum[2] += s[2];
        sum[3] += s[3];
      }
      return std::array<float, 4>{clampStorable(sum[0] * invTaps), clampStorable(sum[1] * invTaps),
                                  clampStorable(sum[2] * invTaps), clampStorable(sum[3] * invTaps)};
    });
  }
  return true;
}

}  // namespace np
