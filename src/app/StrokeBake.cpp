#include "app/StrokeBake.hpp"

#include <algorithm>
#include <cmath>

#include "core/PigmentBake.hpp"

namespace np {
namespace {

// The floor below which a solver texel is not worth a tile write. It is the
// same 1e-4 `shaders/composite.wgsl` uses to decide a texel is worth drawing,
// and that is the point: a texel the solver would not have painted must not
// become a texel the document holds.
constexpr float kBakeMassFloor = 1e-4f;

}  // namespace

size_t bakePigmentTileFrom(const float* depC, const float* depR, float absorption,
                           PigmentTile& out) {
  if (depC == nullptr || depR == nullptr) return 0;
  size_t written = 0;
  for (int32_t y = 0; y < kTileSize; ++y) {
    for (int32_t x = 0; x < kTileSize; ++x) {
      const size_t i = (static_cast<size_t>(y) * kTileSize + x) * 4;
      if (!(depC[i + 3] > kBakeMassFloor)) continue;
      const std::array<float, 4> c = {depC[i], depC[i + 1], depC[i + 2], depC[i + 3]};
      const std::array<float, 4> r = {depR[i], depR[i + 1], depR[i + 2], depR[i + 3]};
      out.writeTexel(PixelCoord{x, y}, projectSolverTexel(c, r, absorption));
      ++written;
    }
  }
  return written;
}

BakeResult bakePigmentTiles(const PaintSim& sim, Layer& layer, float absorption) {
  BakeResult result;
  if (!layer.pigmentTiles.has_value()) return result;

  const size_t n = sim.pigmentReadbackTileCount();
  for (size_t i = 0; i < n; ++i) {
    const float* depC = sim.pigmentReadbackDepC(i);
    const float* depR = sim.pigmentReadbackDepR(i);
    if (depC == nullptr || depR == nullptr) return result;  // readback not Ready

    // getOrCreate, not a fresh tile: a bake adds to what the layer already
    // holds. Writing into a copy and assigning would drop every texel this
    // bake did not touch, which is most of them.
    const TileCoord at{static_cast<int32_t>(sim.bridgeTileAt(i).x),
                       static_cast<int32_t>(sim.bridgeTileAt(i).y)};
    PigmentTile& tile = layer.pigmentTiles->getOrCreate(at);
    const size_t written = bakePigmentTileFrom(depC, depR, absorption, tile);
    result.texelsWritten += written;
    if (written == 0) {
      ++result.tilesEmpty;
    } else {
      ++result.tilesWritten;
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          result.peakCoverage = std::max(result.peakCoverage, tile.readTexel(PixelCoord{x, y}).mass);
    }
  }
  return result;
}

}  // namespace np
