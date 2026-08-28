#include "ops/PointOpTiles.hpp"

#include <array>

namespace np {

bool pointOpTiles(const TileStore& src, const PixelRect& outRect, const PointOpRun& ops,
                  TileStore* out) {
  if (out == nullptr || out == &src) return false;
  if (roiIsEmpty(outRect)) return false;
  if (ops.empty()) return false;

  *out = TileStore{};

  // The source's allocated tiles, not `roiTileRange(outRect)` -- see this
  // module's header for why a point op cannot create coverage where there was
  // none, and therefore has no business allocating a tile that does not
  // already exist.
  for (const auto& [coord, tile] : src) {
    const PixelRect window = roiIntersect(outRect, roiTileRect(coord));
    if (roiIsEmpty(window)) continue;

    // Deferred until a texel actually differs, for `compositeFilterResult()`'s
    // own reason: an identity request (a neutral params struct, or a tile that
    // is entirely transparent) must cost no allocation at all. `dst` stays
    // null until the first real write.
    Tile* dst = nullptr;

    for (int32_t y = window.y0; y < window.y1; ++y) {
      for (int32_t x = window.x0; x < window.x1; ++x) {
        const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
        const std::array<float, 4> in = tile.readPixel(local);
        const std::array<float, 4> graded = applyPointOpsPremultiplied(in, ops);
        if (graded == in) continue;
        if (dst == nullptr) {
          dst = &out->getOrCreate(coord);
          // **The whole source tile first, then the graded texels over the
          // top.** Not an optimisation -- a correctness requirement, and the
          // one thing about this loop that is easy to get catastrophically
          // wrong. `compositeFilterResult()` (app/FilterOps.cpp) treats a tile
          // PRESENT in the engine's output as authoritative for every texel it
          // covers: it reads `filtered.readPixel(local)` at each address and
          // blends that value in. A freshly-created `Tile` is all zeroes, so
          // writing only the changed texels here would hand the compositor
          // transparent black for every texel this op left alone -- erasing
          // the untouched half of a partly-graded tile. Absent tiles are
          // skipped wholesale by that function, which is why the deferral
          // above is safe; a PARTIAL tile is not.
          *dst = tile;
        }
        dst->writePixel(local, graded);
      }
    }
  }
  return true;
}

}  // namespace np
