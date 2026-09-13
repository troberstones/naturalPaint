#pragma once

#include <cstdint>
#include <vector>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/LensBlur -- aperture-shaped bokeh (docs/operations.md §2.2, P2: "Lens
// blur -- aperture-shaped bokeh -- the most expensive filter here", and
// §2.3's "~12,000 taps per pixel by brute force" at radius 64).
//
// The aperture is a regular polygon (3-8 blades) or a circle (0 blades)
// inscribed in `radius` texels, convolved as a FLAT (equal-weight) kernel --
// what "aperture-shaped" means: every texel the shape covers contributes
// equally, none outside it contributes at all. Convolution happens directly
// in the tile store's native linear-light premultiplied space -- there is no
// separate "linear light" step to take, because that is what this format
// already is (core/TileStore.hpp's own header); a store whose native
// representation were sRGB would need one and this file would not compile
// without it.
//
// **Why this is not the O(radius^2) brute force the doc warns about.** A
// convex aperture intersects any horizontal line in one contiguous run of
// texels (`lensApertureRowSpans()` below derives each run's bounds once, in
// closed form, from the aperture's half-plane/circle definition). Summing a
// contiguous run through a per-row prefix-sum table is then O(1) regardless
// of the run's width, which turns the whole convolution into O(radius) rows
// per output texel instead of O(radius^2) texels -- the scanline
// decomposition docs/operations.md §2.3 asks for. `--selftest` cross-checks
// the result against a structurally independent, genuinely O(radius^2)
// reference at a small radius, matching `ops/Filters.hpp`'s own motion-blur
// precedent.
namespace np {

// Texels, inclusive of the identity at 0. Capped at the doc's own worked
// example -- docs/operations.md §2.3 names radius 64 as the brute-force cost
// this filter exists to beat; past it, the fast path's own advantage only
// grows, but a preview built around a larger cap would be committing to a
// number nobody has measured.
inline constexpr int32_t kLensBlurMaxRadius = 64;

struct LensBlurParams {
  // 0 = circle. 3-8 = a regular polygon with that many straight edges
  // ("blades"), inscribed in a circle of the same `radius`.
  int32_t bladeCount = 0;
  float bladeRotationRadians = 0.0f;

  // 0 is the exact identity (the aperture is a single texel); the general
  // path is never reached, so `bladeCount`/`bladeRotationRadians` are
  // unvalidated and irrelevant at `radius == 0`, the same convention
  // ops/Filters.hpp's zero-strength filters already use.
  int32_t radius = 0;

  // Specular highlight bloom: a source texel whose Rec. 709 luma (of its own
  // PREMULTIPLIED colour, ops/Filters.hpp §7's convention) exceeds this
  // threshold has its RGB (not alpha -- a highlight getting brighter does not
  // become more opaque) scaled by `1 + highlightBoost` before the aperture
  // convolves it. `highlightBoost == 0` disables this entirely regardless of
  // `highlightThreshold`, which is what makes "energy conserved without the
  // specular boost" a meaningful thing for `--selftest` to assert: the
  // convolution itself is a normalised (sum-to-1) average, so with no boost
  // applied upstream of it, total energy in the interior (away from any edge
  // the aperture would otherwise read past) is preserved.
  float highlightThreshold = 1.0f;
  float highlightBoost = 0.0f;
};

// False for `radius` outside `[0, kLensBlurMaxRadius]`, a `bladeCount` that
// is neither 0 nor in `[3, 8]`, a non-finite rotation/threshold, or a
// negative `highlightBoost` (which would dim rather than bloom -- a
// different feature nobody asked for).
bool lensBlurParamsValid(const LensBlurParams& p) noexcept;

// `RoiOp{}` at `radius == 0`; otherwise `roiDilateOp(radius)` -- the aperture
// is isotropic by construction, exactly like a blur's.
RoiOp lensBlurRoiOp(const LensBlurParams& p) noexcept;

// One row of the aperture's discrete shape: the inclusive texel-offset range
// `[loX, hiX]` at vertical offset `dy` from the aperture's own centre, or
// `valid == false` for a row the aperture does not reach at all. Exposed
// (rather than kept file-local) so `--selftest` can assert its texel COUNT
// against the continuous regular-polygon area formula independently of
// running the filter -- see ops/LensBlur.hpp's own file comment on why that
// is a stronger proof than comparing rendered pixels alone.
struct LensApertureRow {
  bool valid = false;
  int32_t loX = 0;
  int32_t hiX = 0;
};

// One entry per `dy` in `[-radius, radius]`, in that order (index `i`
// corresponds to `dy = i - radius`). `bladeCount`/`bladeRotationRadians` are
// read; `radius` must be `> 0` (the caller is expected to have already taken
// the `radius == 0` identity path).
std::vector<LensApertureRow> lensApertureRowSpans(int32_t bladeCount, float bladeRotationRadians,
                                                  int32_t radius);

// The discrete aperture's own texel count -- the sum of each row's width.
// This is the divisor the engine normalises by, and the number
// `--selftest` compares against the continuous polygon-area formula
// `(1/2) * N * radius^2 * sin(2*pi/N)` within a discretisation tolerance.
int64_t lensApertureTexelCount(int32_t bladeCount, float bladeRotationRadians, int32_t radius);

bool lensBlurTiles(const TileStore& src, const PixelRect& outRect, const LensBlurParams& p,
                   TileStore* dst);

}  // namespace np
