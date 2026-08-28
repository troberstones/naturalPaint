#include "brush/Grain.hpp"

#include <algorithm>
#include <cmath>

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

// One texel of a SAMPLED paper, at document coordinate (x, y), shaped by the
// panel's brightness / contrast / invert and scaled by `depth` -- so the
// return value is in `[0, depth]`, exactly as the procedural branch's is, and
// `grainOverlayFraction()` cannot tell which produced it.
//
// **Nearest-neighbour, not bilinear, and that is a choice.** The tip bitmap is
// sampled bilinearly (brush/Deposit.cpp) because it is a SHAPE and a jagged
// edge on a shape is visible as a jagged edge. Paper is a texture sampled at
// document resolution: interpolating it low-passes exactly the high-frequency
// tooth that makes it read as paper, and at `scale == 1` it is a 1:1 lookup
// where interpolation would do nothing but cost. A scale far from 1 will
// alias, and that is the case to revisit if one turns up -- the papers
// measured are 128 to 2016 px against brushes of tens of pixels, so the
// common direction is minification, where the honest fix is a mip chain
// rather than a bilinear tap.
float sampledHeightAt(const GrainParams& params, int32_t x, int32_t y) noexcept {
  const PaperField& f = *params.field;
  if (f.width <= 0 || f.height <= 0 || f.height8.empty()) return 0.0f;

  const float scale = (params.scale > 0.0f) ? params.scale : 1.0f;
  const int32_t sx = wrapIndex(static_cast<int32_t>(std::floor(static_cast<float>(x) / scale)),
                               f.width);
  const int32_t sy = wrapIndex(static_cast<int32_t>(std::floor(static_cast<float>(y) / scale)),
                               f.height);
  const size_t index = static_cast<size_t>(sy) * static_cast<size_t>(f.width) +
                       static_cast<size_t>(sx);
  if (index >= f.height8.size()) return 0.0f;

  float h = static_cast<float>(f.height8[index]) / 255.0f;

  // Brightness then contrast, both about the mid-grey a paper scan sits
  // around, then invert -- the order Photoshop's own panel lists them in.
  h = std::clamp(h + params.brightness, 0.0f, 1.0f);
  if (params.contrast != 0.0f)
    h = std::clamp((h - 0.5f) * (1.0f + params.contrast) + 0.5f, 0.0f, 1.0f);
  if (params.invert) h = 1.0f - h;

  return h * std::max(params.depth, 0.0f);
}

float grainHeightAt(const GrainParams& params, int32_t x, int32_t y) noexcept {
  // The sampled branch first, so the procedural code below is reached only
  // when there is genuinely no paper attached -- which keeps every
  // pre-existing brush on the identical path it has always taken.
  if (params.field != nullptr) return sampledHeightAt(params, x, y);

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
  // **Subtract keeps the original call, not an equivalent one.** It is the
  // default and the behaviour every existing brush has, and routing it through
  // `applyCoverageBlend()` would be the same arithmetic in a different order
  // of operations -- which is exactly the kind of "surely identical" change
  // that turns a bit-exact golden reference red for no reason anyone can name
  // afterwards.
  //
  // **`Height` joins it, and that is a correction rather than a convenience.**
  // brush/CoverageBlend.hpp says the two ids "resolve to the same formula";
  // routed through `applyCoverageBlend()` they would NOT, because that
  // function clamps `a` to [0,1] before subtracting and `grainOverlayFraction`
  // clamps only the result -- so any `strength` above 1 (which
  // `GrainParams::strength` explicitly permits, "a paper that makes a fully
  // loaded tip bite HARDER") would make Height and Subtract diverge while the
  // header claimed they could not. One call site for both is what makes the
  // claim checkable, and --selftest checks it over a grid that includes
  // `strength > 1` rather than taking it on the comment.
  if (params.blend == CoverageBlend::Subtract || params.blend == CoverageBlend::Height)
    return grainOverlayFraction(coverage, params.strength, 1.0f, G);
  return applyCoverageBlend(params.blend,
                            std::clamp(coverage * params.strength, 0.0f, 1.0f), G);
}

bool grainParamsEqual(const GrainParams& a, const GrainParams& b) noexcept {
  // `field` is compared by POINTER, not by content. Two presets holding the
  // same decoded paper hold the same `shared_ptr`, because the importer
  // resolves each pattern id once per file -- so pointer equality is the same
  // answer as content equality for every way a field can actually arrive, and
  // it does not walk a megapixel buffer on every EDITED-badge check.
  return a.enabled == b.enabled && a.periodX == b.periodX && a.periodY == b.periodY &&
         a.depth == b.depth && a.strength == b.strength && a.field == b.field &&
         a.scale == b.scale && a.invert == b.invert && a.brightness == b.brightness &&
         a.contrast == b.contrast && a.blend == b.blend;
}

}  // namespace np
