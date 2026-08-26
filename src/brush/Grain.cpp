#include "brush/Grain.hpp"

#include <algorithm>

#include "brush/Dynamics.hpp"  // splitmix64() -- reused, not re-derived; see header §1.

namespace np {
namespace {

// Wraps `v` into `[0, period)` for a positive `period`, correctly for a
// negative `v` -- `%` alone would return a negative remainder for one, which
// is not a valid array/lattice index and would make `x` and `x - periodX`
// hash to two DIFFERENT wrapped coordinates instead of the identical one the
// header's "tiles exactly" claim requires.
int32_t wrapIndex(int32_t v, int32_t period) noexcept {
  const int32_t m = v % period;
  return m < 0 ? m + period : m;
}

}  // namespace

float grainHeightAt(const GrainParams& params, int32_t x, int32_t y) noexcept {
  const int32_t periodX = std::max(params.periodX, 1);
  const int32_t periodY = std::max(params.periodY, 1);
  const int32_t ix = wrapIndex(x, periodX);
  const int32_t iy = wrapIndex(y, periodY);

  // The wrapped lattice coordinate, packed into one 64-bit word exactly as
  // `strokeSeedFromStart()` packs two floats (brush/Dynamics.cpp) -- two
  // 32-bit halves, one hash. `static_cast<uint32_t>` on a non-negative
  // `int32_t` is value-preserving, so this is a lossless pack, not a
  // truncation.
  const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
                          static_cast<uint32_t>(iy);
  const uint64_t h = splitmix64(packed);

  // Top 24 bits -> a uniform draw in [0,1), `dynamicRandomDraw()`'s own
  // construction (brush/Dynamics.cpp): a float's mantissa holds 24 bits
  // (23 explicit plus the implicit leading one) with no rounding, so this can
  // never round up to the excluded 1.0f the way dividing a wider integer
  // could.
  const float unit = static_cast<float>(h >> 40) / static_cast<float>(1u << 24);
  return unit * std::max(params.depth, 0.0f);
}

float grainOverlayFraction(float P, float S, float O1, float G) noexcept {
  const float raw = P * S * O1 - G;
  if (!(raw > 0.0f)) return 0.0f;  // also catches NaN, `depositTexel()`'s own idiom
  return raw < 1.0f ? raw : 1.0f;
}

float grainCoverageAt(const GrainParams& params, float coverage, int32_t x, int32_t y) noexcept {
  // Checked FIRST, before `grainHeightAt()` or `grainOverlayFraction()` runs
  // at all -- header §`GrainParams::enabled`'s own comment: this is what
  // makes "grain off is a no-op" a claim about the code path, not merely
  // about the numbers it produces.
  if (!params.enabled) return coverage;
  const float G = grainHeightAt(params, x, y);
  return grainOverlayFraction(coverage, params.strength, 1.0f, G);
}

bool grainParamsEqual(const GrainParams& a, const GrainParams& b) noexcept {
  return a.enabled == b.enabled && a.periodX == b.periodX && a.periodY == b.periodY &&
         a.depth == b.depth && a.strength == b.strength;
}

}  // namespace np
