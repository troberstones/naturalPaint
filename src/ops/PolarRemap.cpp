#include "ops/PolarRemap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/Parallel.hpp"

namespace np {

namespace {

// A near-copy of ops/Filters.cpp's own gather/bilinear/scatter trio --
// ops/RadialBlur.cpp's own comment explains why a fourth caller does not yet
// earn pulling these into a shared header: doing so means editing that file,
// which is another track's.

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

// Zero-pads outside `need` on both axes -- Rect->Polar's own sampler, which
// reads ordinary (non-periodic) Cartesian source space.
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

// Wraps X across `need`'s own width (the polar source's periodic angle
// axis), zero-pads Y (its non-periodic radius axis) -- Polar->Rect's own
// sampler. ops/Pattern.hpp's `patternSourceTexel()` comment is the reason
// this is a euclidean wrap, spelled in floats: a truncating `fmod` indexes
// backwards off the front of the row for any `fx` left of `periodX0`, which a
// direction whose whole rectangle is `[-pi, pi)` reaches immediately.
//
// **The wrap period is `outRect`'s own width, not `need`'s.** `need` is
// dilated by one texel on every side (`polarRemapRoiOp()`) for the plain,
// non-wrapping sampler's bilinear safety at the disc's true edge; wrapping
// against that widened buffer would wrap one texel late and read a texel
// twice at the seam. The wrapped x always lands inside
// `[periodX0, periodX0 + periodWidth)` -- entirely within `need`'s own wider
// range -- so the buffer lookup below is never out of bounds.
std::array<float, 4> planeBilinearWrapX(const std::vector<float>& plane, const PixelRect& need,
                                        int32_t periodX0, int32_t periodWidth, float fx,
                                        float fy) {
  const int32_t bufWidth = need.width();
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  auto wrapX = [&](int32_t x) -> int32_t {
    int32_t rel = (x - periodX0) % periodWidth;
    if (rel < 0) rel += periodWidth;
    return periodX0 + rel;
  };
  auto at = [&](int32_t x, int32_t y) -> std::array<float, 4> {
    if (y < need.y0 || y >= need.y1) return {0.0f, 0.0f, 0.0f, 0.0f};
    const int32_t wx = wrapX(x);
    const size_t base = (static_cast<size_t>(y - need.y0) * static_cast<size_t>(bufWidth) +
                         static_cast<size_t>(wx - need.x0)) *
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

// Polar->Rect's pole. Within this many texels of the destination centre,
// every one-texel step swings the resolved angle by more than a bilinear tap
// can resolve (ops/PolarRemap.hpp's header comment derives why) -- widened to
// a rotational average of `kPoleSupersamples` taps at the SAME radius, faded
// linearly out to a single tap at the boundary so the switch is not a visible
// ring.
constexpr float kPoleSupersampleRadius = 2.0f;
constexpr int32_t kPoleSupersamples = 8;

}  // namespace

const char* polarRemapDirectionName(PolarRemapDirection d) noexcept {
  switch (d) {
    case PolarRemapDirection::RectToPolar: return "rect_to_polar";
    case PolarRemapDirection::PolarToRect: return "polar_to_rect";
  }
  return "rect_to_polar";
}

std::optional<PolarRemapDirection> polarRemapDirectionFromName(std::string_view name) noexcept {
  if (name == "rect_to_polar") return PolarRemapDirection::RectToPolar;
  if (name == "polar_to_rect") return PolarRemapDirection::PolarToRect;
  return std::nullopt;
}

RoiOp polarRemapRoiOp(const PolarRemapParams&, const PixelRect&) noexcept { return roiDilateOp(1); }

bool polarRemapTiles(const TileStore& src, const PixelRect& outRect, const PolarRemapParams& p,
                     TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (roiIsEmpty(outRect)) return false;

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherRawPlane(src, outRect, polarRemapRoiOp(p, outRect), &need, &plane)) return false;

  const float x0 = static_cast<float>(outRect.x0);
  const float y0 = static_cast<float>(outRect.y0);
  const float w = static_cast<float>(outRect.width());
  const float h = static_cast<float>(outRect.height());
  const float cx = x0 + 0.5f * w;
  const float cy = y0 + 0.5f * h;
  const float rMax = 0.5f * std::min(w, h);

  if (p.direction == PolarRemapDirection::RectToPolar) {
    // Dest (x, y) IS (angle, radius): x sweeps a full turn across the
    // rectangle's width, y sweeps radius 0..rMax across its height. No
    // singularity here -- every destination texel resolves to a distinct,
    // ordinary Cartesian point (ops/PolarRemap.hpp's header comment).
    scatterPlaneParallel(outRect, dst, [&](int32_t x, int32_t y) -> std::array<float, 4> {
      const float u = (static_cast<float>(x) - x0 + 0.5f) / w;
      const float v = (static_cast<float>(y) - y0 + 0.5f) / h;
      const float angle = u * 2.0f * kPi - kPi;
      const float radius = v * rMax;
      const float fx = cx + radius * std::cos(angle);
      const float fy = cy + radius * std::sin(angle);
      return planeBilinear(plane, need, fx, fy);
    });
    return true;
  }

  // Polar->Rect: dest (x, y) is a Cartesian point; its source is (angle,
  // radius) read back from the SAME layout Rect->Polar writes, so the two
  // are exact inverses of one another on the identical frame.
  scatterPlaneParallel(outRect, dst, [&](int32_t x, int32_t y) -> std::array<float, 4> {
    const float dx = static_cast<float>(x) + 0.5f - cx;
    const float dy = static_cast<float>(y) + 0.5f - cy;
    const float radius = std::sqrt(dx * dx + dy * dy);
    if (!(rMax > 0.0f) || radius > rMax) return {0.0f, 0.0f, 0.0f, 0.0f};

    auto sampleAt = [&](float ang, float r) -> std::array<float, 4> {
      const float u = (ang + kPi) / (2.0f * kPi);
      const float su = u * w + x0;
      const float sv = (r / rMax) * h + y0;
      return planeBilinearWrapX(plane, need, outRect.x0, outRect.width(), su, sv);
    };

    const float angle = std::atan2(dy, dx);
    if (radius >= kPoleSupersampleRadius) return sampleAt(angle, radius);

    std::array<float, 4> avg{0.0f, 0.0f, 0.0f, 0.0f};
    for (int32_t k = 0; k < kPoleSupersamples; ++k) {
      const float tapAngle = (static_cast<float>(k) / static_cast<float>(kPoleSupersamples)) *
                             2.0f * kPi - kPi;
      const std::array<float, 4> s = sampleAt(tapAngle, radius);
      avg[0] += s[0];
      avg[1] += s[1];
      avg[2] += s[2];
      avg[3] += s[3];
    }
    const float invN = 1.0f / static_cast<float>(kPoleSupersamples);
    avg = {avg[0] * invN, avg[1] * invN, avg[2] * invN, avg[3] * invN};

    const std::array<float, 4> single = sampleAt(angle, radius);
    const float t = radius / kPoleSupersampleRadius;  // 0 at the pole, 1 at the boundary
    return std::array<float, 4>{avg[0] + t * (single[0] - avg[0]),
                                avg[1] + t * (single[1] - avg[1]),
                                avg[2] + t * (single[2] - avg[2]),
                                avg[3] + t * (single[3] - avg[3])};
  });
  return true;
}

}  // namespace np
