#include "core/SelectionMask.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace np {
namespace {

// The overlap, in one axis, between the texel spanning [t, t+1) and the
// requested span [lo, hi). Returns 0..1.
//
// Coverage is separable for an axis-aligned rectangle -- the covered area of a
// unit square is the product of its two axis overlaps -- which is what keeps
// `selectRectangle()` exact rather than sampled. It stops being separable for
// an ellipse or a lasso, and those will need their own answer rather than a
// generalisation of this one.
float axisOverlap(int32_t t, float lo, float hi) noexcept {
  const float left = std::max(static_cast<float>(t), lo);
  const float right = std::min(static_cast<float>(t) + 1.0f, hi);
  return right > left ? right - left : 0.0f;
}

}  // namespace

float selectionCoverageAt(const Selection* selection, PixelCoord docTexel) noexcept {
  // The common case, and the whole reason this function exists: no selection
  // is NOT "select nothing", it is "no restriction". See the header.
  if (selection == nullptr) return 1.0f;
  const SelectionTile* tile = selection->tiles.find(tileCoordAt(docTexel));
  return selectionTileCoverage(tile, tileLocalOffset(docTexel));
}

bool selectionSelectsNothing(const Selection& selection) noexcept {
  for (const auto& [coord, tile] : selection.tiles) {
    (void)coord;
    if (!tile.selectsNothing()) return false;
  }
  return true;
}

Selection selectRectangle(float x0, float y0, float x1, float y1) {
  Selection out;
  // Normalise rather than refuse: a marquee dragged up-and-left gives x1 < x0,
  // and that is a rectangle, not an error.
  if (x1 < x0) std::swap(x0, x1);
  if (y1 < y0) std::swap(y0, y1);
  // Degenerate is empty, and empty means "selects nothing" -- deliberately not
  // "selects everything", which is what returning a default-constructed
  // Selection would mean if callers read it as absent. It is engaged with zero
  // tiles, and `selectionSelectsNothing()` says so.
  if (!(x1 > x0) || !(y1 > y0)) return out;

  // The half-open texel range the rectangle can touch at all. `floor(x0)` is
  // the first texel whose span [t, t+1) can overlap, and `ceil(x1)` is one
  // past the last.
  const int32_t tx0 = static_cast<int32_t>(std::floor(x0));
  const int32_t ty0 = static_cast<int32_t>(std::floor(y0));
  const int32_t tx1 = static_cast<int32_t>(std::ceil(x1));
  const int32_t ty1 = static_cast<int32_t>(std::ceil(y1));

  for (int32_t y = ty0; y < ty1; ++y) {
    const float yCov = axisOverlap(y, y0, y1);
    if (yCov <= 0.0f) continue;
    for (int32_t x = tx0; x < tx1; ++x) {
      const float cov = axisOverlap(x, x0, x1) * yCov;
      // Skip zero rather than writing it: a texel at zero coverage is
      // indistinguishable from an absent one, and writing it would allocate a
      // 16 KiB tile to store "not selected", which is what an absent tile
      // already says for free.
      if (cov <= 0.0f) continue;
      const PixelCoord doc{x, y};
      out.tiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), cov);
    }
  }
  return out;
}

size_t clearThroughSelection(TileStore& tiles, const Selection* selection) {
  size_t changed = 0;

  // The coordinates are collected first rather than walked directly, because
  // the loop below takes write handles through findForWrite(), and a
  // copy-on-write unshare is a mutation of the very map being iterated.
  std::vector<TileCoord> coords;
  coords.reserve(tiles.occupiedTileCount());
  for (const auto& [coord, tile] : tiles) {
    (void)tile;
    coords.push_back(coord);
  }

  for (const TileCoord coord : coords) {
    // Fast path, and it is the one that matters: a selection with no tile
    // here covers none of it, so there is nothing to clear and -- crucially --
    // no reason to unshare a copy-on-write tile. Without this, clearing
    // through a small marquee would deep-copy every tile in the document.
    const SelectionTile* selTile =
        selection != nullptr ? selection->tiles.find(coord) : nullptr;
    if (selection != nullptr && selTile == nullptr) continue;

    Tile* dst = tiles.findForWrite(coord);
    if (dst == nullptr) continue;

    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const PixelCoord local{lx, ly};
        const float cov =
            selection == nullptr ? 1.0f : selectionTileCoverage(selTile, local);
        if (cov <= 0.0f) continue;

        const std::array<float, 4> before = dst->readPixel(local);
        // Premultiplied, so all four channels scale together and the hole is
        // correct with no un-premultiply bracket. See the header on why doing
        // this to straight alpha instead is where fringing comes from.
        const float keep = 1.0f - cov;
        const std::array<float, 4> after{before[0] * keep, before[1] * keep,
                                         before[2] * keep, before[3] * keep};
        if (after != before) {
          dst->writePixel(local, after);
          ++changed;
        }
      }
    }

    // **A tile cleared to nothing is NOT dropped here.** core/TileStore has no
    // erase(), by omission rather than by decision as far as this file can
    // tell, and adding one to a type every store in the project is an alias of
    // does not belong in the same change as the selection store. So a
    // full-coverage clear leaves 128 KiB of zeros resident per tile.
    //
    // A cost, not a defect: an all-zero tile composites to nothing (core's
    // premultiplied zero is transparent black) and io/NpaintFile already
    // declines to write one, so the document is correct and the file is
    // correct. Only resident memory is worse than it needs to be, and PRD M5
    // is the requirement that will eventually care.
  }
  return changed;
}

}  // namespace np
