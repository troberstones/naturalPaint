#include "ops/Feather.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "ops/Roi.hpp"

namespace np {

float featherSigmaForRadius(float radius) noexcept {
  // ops/Feather.hpp's "Decision 2" is this line, and the measured table is
  // there. Half the radius, so the visible soft band is `radius` texels either
  // side of the original edge rather than about 2.5 times it.
  return radius * 0.5f;
}

BlurParams featherBlurParams(float radius) noexcept {
  BlurParams p;
  p.kind = BlurKind::Gaussian;
  p.sigma = featherSigmaForRadius(radius);
  return p;
}

Selection featherSelection(const Selection& selection, float radius) {
  if (!std::isfinite(radius) || radius <= 0.0f) return selection;

  const BlurParams params = featherBlurParams(radius);
  const int32_t apron = blurApron(params);
  if (apron <= 0) return selection;

  // The input's extent at TILE granularity, not `selectionBounds()`'s exact
  // texel bounds. Tile granularity is the cheaper of the two (O(tiles) against
  // O(tiles x texels), and core/SelectionMask.hpp warns that the exact form is
  // not for a hot path) and it is never wrong here, only occasionally
  // generous: a rectangle rounded out to whole tiles still contains every
  // non-zero texel, and rounding a *gather* outward is the safe direction --
  // ops/Roi.hpp's rule that too large is slow and too small is wrong.
  bool anyTile = false;
  TileRange content{};
  for (const auto& [coord, tile] : selection.tiles) {
    (void)tile;
    if (!anyTile) {
      content = TileRange{coord.x, coord.y, coord.x + 1, coord.y + 1};
      anyTile = true;
      continue;
    }
    content.x0 = std::min(content.x0, coord.x);
    content.y0 = std::min(content.y0, coord.y);
    content.x1 = std::max(content.x1, coord.x + 1);
    content.y1 = std::max(content.y1, coord.y + 1);
  }

  // An engaged selection with no tiles feathers to an engaged selection with
  // no tiles. Blurring zero is zero, so this is the arithmetic's own answer
  // rather than a shortcut -- but it is written out because the alternative
  // spelling, returning `selection` by copy, happens to be correct here too
  // and would stop being correct the moment anything else was added below.
  if (!anyTile) return Selection{};

  // The output rectangle: the input's tiles, expanded by the kernel's reach.
  // Coverage cannot be non-zero outside this, because everything outside the
  // input's tiles is exactly 0.0 (core/SelectionMask's inverted default) and a
  // convolution of zeros is zero. That is also why the plane below needs no
  // apron of its own -- `blurPlane()` treats outside-the-buffer as zero, which
  // is *true* here rather than merely convenient.
  const PixelRect out = roiExpandUniform(roiTileRangeRect(content), apron);
  const int32_t w = out.width();
  const int32_t h = out.height();
  if (w <= 0 || h <= 0) return Selection{};

  std::vector<float> plane(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);

  // Gathered by walking the STORE, not the tile range: the input is sparse and
  // a marquee shaped like an L would otherwise pay for the tiles in its notch.
  for (const auto& [coord, tile] : selection.tiles) {
    const PixelCoord origin = tileOrigin(coord);
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const int32_t y = origin.y + ly;
      float* row = plane.data() + static_cast<size_t>(y - out.y0) * static_cast<size_t>(w);
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        row[origin.x + lx - out.x0] = tile.coverageAt(PixelCoord{lx, ly});
      }
    }
  }

  // One channel, aliased in and out -- coverage is a scalar field and ops/Blur
  // interprets no channel, which is exactly why the same kernel serves a
  // premultiplied RGBA store and a uint8 coverage mask without a second
  // implementation.
  blurPlane(plane.data(), w, h, 1, params, plane.data());

  Selection result;
  const TileRange writeTiles = roiTileRange(out);
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), out);
      if (roiIsEmpty(span)) continue;
      const PixelCoord origin = tileOrigin(coord);

      SelectionTile tile;
      bool any = false;
      for (int32_t y = span.y0; y < span.y1; ++y) {
        const float* row = plane.data() + static_cast<size_t>(y - out.y0) * static_cast<size_t>(w);
        for (int32_t x = span.x0; x < span.x1; ++x) {
          const float cov = row[x - out.x0];
          if (cov <= 0.0f) continue;
          const PixelCoord local{x - origin.x, y - origin.y};
          tile.writeCoverage(local, cov);
          // `any` tracks what SURVIVED quantisation, not what was written: a
          // coverage of 1e-4 rounds to uint8 zero, and counting it as content
          // would keep a 16 KiB tile of zeros resident for every tile the
          // apron reaches into. This is the outermost-ring case ops/Feather.hpp
          // names, and it is the difference between a feather that grows the
          // store by `2 * radius` worth of tiles and one that grows it by
          // roughly `1.5 * radius`.
          if (tile.coverageAt(local) > 0.0f) any = true;
        }
      }
      // core/SelectionMask's constructor invariant: no stored tile is entirely
      // zero. Same drop rule core/SelectionOps' invert and intersect apply.
      if (any) result.tiles.getOrCreate(coord) = tile;
    }
  }
  return result;
}

}  // namespace np
