#include "core/SelectionOps.hpp"

#include <algorithm>
#include <vector>

namespace np {
namespace {

// Build one output tile from the two inputs, either of which may be absent.
//
// Absent means zero coverage, which is what `selectionTileCoverage()` already
// says, so no branch is needed here beyond the one it does internally. Returns
// false when every texel came out zero -- the caller then declines to store
// the tile, which is how the "no tile is entirely zero" invariant survives an
// operation that can erase one.
bool buildCombinedTile(const SelectionTile* a, const SelectionTile* b, SelectionCombine op,
                       SelectionTile& out) {
  bool any = false;
  for (int32_t ly = 0; ly < kTileSize; ++ly) {
    for (int32_t lx = 0; lx < kTileSize; ++lx) {
      const PixelCoord local{lx, ly};
      const float cov = combineCoverage(selectionTileCoverage(a, local),
                                        selectionTileCoverage(b, local), op);
      if (cov <= 0.0f) continue;
      out.writeCoverage(local, cov);
      any = true;
    }
  }
  return any;
}

// Every tile coordinate that could hold a non-zero result, for each rule.
//
// This is the whole of the sparsity argument, in one place rather than spread
// across three loops that could disagree: subtract and intersect can only ever
// *reduce*, so neither can produce coverage where `base` had none, and
// intersect additionally cannot produce it where `addend` had none.
std::vector<TileCoord> candidateTiles(const Selection& base, const Selection& addend,
                                      SelectionCombine op) {
  std::vector<TileCoord> coords;
  for (const auto& [coord, tile] : base.tiles) {
    (void)tile;
    // Intersect: a base tile with no addend tile under it contributes nothing,
    // so it is not a candidate at all and its 16 KiB is never touched.
    if (op == SelectionCombine::Intersect && addend.tiles.find(coord) == nullptr) continue;
    coords.push_back(coord);
  }
  if (op == SelectionCombine::Add) {
    for (const auto& [coord, tile] : addend.tiles) {
      (void)tile;
      // Skip the ones already contributed by `base`, or every shared tile
      // would be computed twice and stored twice.
      if (base.tiles.find(coord) != nullptr) continue;
      coords.push_back(coord);
    }
  }
  return coords;
}

}  // namespace

SelectionCombine selectionCombineFromModifiers(bool shift, bool alt) noexcept {
  if (shift && alt) return SelectionCombine::Intersect;
  if (shift) return SelectionCombine::Add;
  if (alt) return SelectionCombine::Subtract;
  return SelectionCombine::Replace;
}

float combineCoverage(float base, float addend, SelectionCombine op) noexcept {
  switch (op) {
    case SelectionCombine::Replace:
      return addend;
    case SelectionCombine::Add:
      return std::max(base, addend);
    case SelectionCombine::Subtract:
      // Intersect-with-complement, written out. The header's De Morgan claim
      // is this line: `min(a, 1 - b)` is exactly `Intersect(a, Invert(b))`.
      return std::min(base, 1.0f - addend);
    case SelectionCombine::Intersect:
      return std::min(base, addend);
  }
  // Unreachable for a valid enumerator. Returning `base` rather than 0 keeps a
  // corrupted op from silently deselecting the document.
  return base;
}

Selection combineSelections(const Selection& base, const Selection& addend,
                            SelectionCombine op) {
  // Replace ignores `base` by definition, and copying `addend` is both the
  // correct answer and cheaper than walking it texel by texel to arrive at the
  // same one.
  if (op == SelectionCombine::Replace) return addend;

  Selection out;
  for (const TileCoord coord : candidateTiles(base, addend, op)) {
    const SelectionTile* a = base.tiles.find(coord);
    const SelectionTile* b = addend.tiles.find(coord);
    SelectionTile tile;
    if (!buildCombinedTile(a, b, op, tile)) continue;
    out.tiles.getOrCreate(coord) = tile;
  }
  return out;
}

Selection invertSelection(const Selection& selection, int32_t width, int32_t height) {
  Selection out;
  if (width <= 0 || height <= 0) return out;

  // Every tile the document touches, including the partial ones at the right
  // and bottom edges. The complement is dense, so this is the real extent of
  // the work and the header says so out loud.
  const TileCoord first = tileCoordAt(PixelCoord{0, 0});
  const TileCoord last = tileCoordAt(PixelCoord{width - 1, height - 1});

  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const SelectionTile* src = selection.tiles.find(coord);
      const PixelCoord origin = tileOrigin(coord);

      SelectionTile tile;
      bool any = false;
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        const int32_t docY = origin.y + ly;
        // Outside the document stays unselected. Without this the edge tiles
        // of a document whose size is not a multiple of 128 would come back
        // fully selected in the margin -- coverage over texels that do not
        // exist, which every consumer would then dutifully walk.
        if (docY < 0 || docY >= height) continue;
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const int32_t docX = origin.x + lx;
          if (docX < 0 || docX >= width) continue;
          const PixelCoord local{lx, ly};
          const float cov = 1.0f - selectionTileCoverage(src, local);
          if (cov <= 0.0f) continue;
          tile.writeCoverage(local, cov);
          any = true;
        }
      }
      // A tile the old selection covered completely inverts to nothing, and a
      // fully-selected document inverts to no tiles at all -- which is
      // `selectionSelectsNothing()`, the honest answer, rather than an empty
      // selection that would read as "no restriction".
      if (any) out.tiles.getOrCreate(coord) = tile;
    }
  }
  return out;
}

}  // namespace np
