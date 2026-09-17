#include "ops/Gradient.hpp"

#include <algorithm>
#include <vector>

#include "core/Blend.hpp"
#include "core/Parallel.hpp"
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

  // Two phases, exactly ops/Blur.cpp's own shape and for the identical reason
  // (core/Parallel.hpp's file comment): `TileStoreOf::getOrCreate` mutates a
  // shared `unordered_map` and is not safe to call from two tiles at once, so
  // every `getOrCreate()` happens here, serially, before any tile's texels
  // are touched. The selection-tile-absent skip stays in THIS phase -- a
  // marquee on a blank 4K layer still costs only the marquee's tiles, not the
  // document's, exactly as before. What is no longer lazy is a destination
  // tile that the selection admits but whose whole span turns out fully
  // transparent (a spread-mode tail past the last opaque stop) -- reserved up
  // front rather than only on the first contributing texel, since deciding
  // that in advance would mean evaluating the ramp twice. Accepted rather
  // than kept lazy at the cost of staying single-threaded: a gradient drag
  // walks its own region every frame it moves (unlike a one-shot filter), and
  // on a document large enough for this to matter the per-frame cost of
  // walking it on one core is the more common complaint (see this function's
  // own header for the iPad report this fixes) -- see PSD's own "empty tile"
  // pitfall (docs, "40 fixtures missed a 6.2 GB allocation") before doing
  // this again somewhere the region cannot be kept small.
  struct ReservedTile {
    int32_t x0, y0, x1, y1;    // this tile's clipped span, document texels
    int32_t tileDocX, tileDocY;
    const SelectionTile* selTile;  // null when `!hasSelection`
    Tile* tile;
  };
  std::vector<ReservedTile> reserved;
  reserved.reserve(static_cast<size_t>(tileX1 - tileX0 + 1) *
                   static_cast<size_t>(tileY1 - tileY0 + 1));
  for (int32_t ty = tileY0; ty <= tileY1; ++ty) {
    for (int32_t tx = tileX0; tx <= tileX1; ++tx) {
      const TileCoord coord{tx, ty};
      const SelectionTile* selTile = nullptr;
      if (hasSelection) {
        selTile = selection->tiles.find(coord);
        if (selTile == nullptr) continue;
      }
      const int32_t tileDocX = tx * kTileSize;
      const int32_t tileDocY = ty * kTileSize;
      reserved.push_back(ReservedTile{
          std::max(region.x0, tileDocX), std::max(region.y0, tileDocY),
          std::min(region.x1, tileDocX + kTileSize), std::min(region.y1, tileDocY + kTileSize),
          tileDocX, tileDocY, selTile, &tiles.getOrCreate(coord)});
    }
  }

  // Phase 2: the pixel work, unchanged from what this loop always did except
  // for who runs which tile -- every `ReservedTile` names a distinct
  // `TileCoord` (the map cannot hold two slots at one), so no two parallel
  // iterations can write the same tile. Each iteration's own `written` count
  // is kept local and summed after: `size_t` is not atomic, and this loop's
  // whole per-texel body is cheap enough that an atomic increment per texel
  // would cost more than the loop itself.
  std::vector<size_t> writtenPerTile(reserved.size(), 0);
  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t i) {
    const ReservedTile& r = reserved[i];
    size_t localWritten = 0;
    for (int32_t y = r.y0; y < r.y1; ++y) {
      for (int32_t x = r.x0; x < r.x1; ++x) {
        const PixelCoord local{x - r.tileDocX, y - r.tileDocY};

        // Coverage, through the hoisted tile. Null selection -> 1.0, the
        // branch this loop owns.
        const float coverage = r.selTile != nullptr ? r.selTile->coverageAt(local) : 1.0f;
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
        if (alpha <= 0.0f) continue;  // no contribution, no count

        const std::array<float, 4> src{straight[0] * alpha, straight[1] * alpha,
                                       straight[2] * alpha, alpha};

        // core/Blend's `over`, reused rather than re-derived: it is the one
        // formula in this codebase asserted at zero tolerance, and a second
        // hand-written copy of `cs + cb*(1-as)` is a second thing to get
        // wrong.
        r.tile->writePixel(local, compositeOver(src, r.tile->readPixel(local)));
        ++localWritten;
      }
    }
    writtenPerTile[i] = localWritten;
  });

  for (const size_t w : writtenPerTile) written += w;
  return written;
}

}  // namespace np
