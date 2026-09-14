#include "ops/Gradient.hpp"

#include <algorithm>

#include "core/Blend.hpp"
#include "core/Tile.hpp"

// ops/Gradient -- implementation of the tile-store half. Every decision that a
// reader would want justified is in the header; this file carries only the
// notes that are about the code rather than about the design. The ramp model
// and its four pure evaluators are core/Gradient.cpp.
namespace np {

size_t renderGradient(TileStore& tiles, const GradientRegion& region,
                      const GradientGeometry& geometry, const GradientStops& stops,
                      const Selection* selection) {
  // No colour ramp, no gradient. The other empty case -- no opacity stops --
  // is handled inside gradientOpacityAt() as "opaque", and deliberately does
  // NOT come here.
  if (stops.colorStops.empty()) return 0;
  if (region.x1 <= region.x0 || region.y1 <= region.y0) return 0;

  size_t written = 0;

  // Walk tile by tile rather than texel by texel over the region, so both the
  // destination tile and the selection tile are looked up ONCE per tile
  // instead of once per texel. core/Composite does the same for the same
  // reason, and core/SelectionMask.hpp names this exact pattern as a hazard:
  //
  //   "A per-texel loop cannot afford a hash lookup per texel, so it hoists
  //    the tile out of the loop ... and then it owns the null-Selection branch
  //    itself. ... Any new hoisted loop must repeat the branch."
  //
  // This is such a loop, and `hasSelection` below is that repeated branch.
  // --selftest asserts the null case through renderGradient() itself and not
  // only through selectionCoverageAt(), which is the other half of what that
  // header asks for.
  const bool hasSelection = selection != nullptr;

  const int32_t tileX0 = floorDiv(region.x0, kTileSize);
  const int32_t tileY0 = floorDiv(region.y0, kTileSize);
  const int32_t tileX1 = floorDiv(region.x1 - 1, kTileSize);
  const int32_t tileY1 = floorDiv(region.y1 - 1, kTileSize);

  for (int32_t ty = tileY0; ty <= tileY1; ++ty) {
    for (int32_t tx = tileX0; tx <= tileX1; ++tx) {
      const TileCoord coord{tx, ty};

      // Hoisted selection lookup. When a selection exists and has no tile
      // here, coverage is 0.0 across the whole tile (core/SelectionMask.hpp:
      // an absent selection tile is OUTSIDE) -- so the tile contributes
      // nothing and, crucially, is skipped BEFORE any destination tile is
      // allocated. That is what keeps a small marquee on a blank 4K layer
      // costing the marquee's tiles rather than the document's.
      const SelectionTile* selTile = nullptr;
      if (hasSelection) {
        selTile = selection->tiles.find(coord);
        if (selTile == nullptr) continue;
      }

      // Clip the region to this tile, in document texels, half-open.
      const int32_t tileDocX = tx * kTileSize;
      const int32_t tileDocY = ty * kTileSize;
      const int32_t x0 = std::max(region.x0, tileDocX);
      const int32_t y0 = std::max(region.y0, tileDocY);
      const int32_t x1 = std::min(region.x1, tileDocX + kTileSize);
      const int32_t y1 = std::min(region.y1, tileDocY + kTileSize);

      // Lazily obtained on the first texel that actually contributes, so a
      // fully transparent stretch of the ramp does not allocate 128 KiB to
      // write zeros into. `getOrCreate()` is the copy-on-write barrier
      // (core/TileStore.hpp); the reference is held for the duration of this
      // one tile's fill and the store is never copied in between, which is
      // exactly the rule that header states.
      Tile* dstTile = nullptr;

      for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
          const PixelCoord local{x - tileDocX, y - tileDocY};

          // Coverage, through the hoisted tile. Null selection -> 1.0, the
          // branch this loop owns.
          const float coverage = hasSelection ? selTile->coverageAt(local) : 1.0f;
          if (coverage <= 0.0f) continue;

          // Texel CENTRES, not corners. A gradient whose handles sit on texel
          // corners must be symmetric about its own midpoint, and sampling at
          // integer positions puts the ramp half a texel off in a way that is
          // invisible on a 4096-px fill and glaring on an 8-px one.
          const float t = gradientParameterAt(geometry, static_cast<float>(x) + 0.5f,
                                              static_cast<float>(y) + 0.5f);
          const std::array<float, 4> straight = gradientSampleStraight(stops, t);

          // **The single premultiply**, and the single place coverage is
          // applied. Both fold into one scale factor because they mean the
          // same thing to a premultiplied texel: how present is this source
          // here. Scaling alpha alone and leaving RGB would leave RGB > alpha,
          // which is an over-bright premultiplied texel and the fringe in the
          // other direction (core/SelectionMask.hpp makes the same argument
          // for the clear).
          const float alpha = straight[3] * coverage;
          if (alpha <= 0.0f) continue;  // no contribution, no tile, no count

          const std::array<float, 4> src{straight[0] * alpha, straight[1] * alpha,
                                         straight[2] * alpha, alpha};

          if (dstTile == nullptr) dstTile = &tiles.getOrCreate(coord);
          // core/Blend's `over`, reused rather than re-derived: it is the one
          // formula in this codebase asserted at zero tolerance, and a second
          // hand-written copy of `cs + cb*(1-as)` is a second thing to get
          // wrong.
          dstTile->writePixel(local, compositeOver(src, dstTile->readPixel(local)));
          ++written;
        }
      }
    }
  }

  return written;
}

}  // namespace np
