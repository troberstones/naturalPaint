#pragma once

#include <optional>
#include <string_view>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/PolarRemap -- Polar Coordinates (docs/operations.md §3, class B: "remaps
// between polar and Cartesian, both directions"), long marked "future work"
// (docs/spec-vs-implementation.md §4).
//
// **Rect->Polar and Polar->Rect are the same code, forward/inverse formulae
// swapped** -- docs/operations.md's own words -- so this is one engine with a
// direction toggle, not two. Both directions walk DESTINATION pixels and ask
// where they came from (an inverse map, so the result cannot tear), and both
// treat the active rectangle -- `outRect`, always the whole canvas in
// practice (see `polarRemapRoiOp()`) -- as a square disc inscribed in it:
// centre at the rectangle's own centre, radius `0.5 * min(width, height)`.
// Photoshop's own dialog has no centre or radius control either; the frame
// IS the parameter.
//
// **The centre is a singularity, and only Polar->Rect has one.** A texel
// right at the destination's centre in that direction has radius 0, and
// EVERY angle maps to that same source row -- so a texel a few pixels away
// still resolves to a wildly different angle than its neighbour one pixel
// over, which is more angular detail than a single bilinear tap can resolve.
// `polarRemapTiles()` widens to a small rotational average within a few
// texels of the centre for exactly this reason (docs/operations.md: "needs
// prefiltering... not a point sample"). Rect->Polar has no such point: every
// destination texel there is an ordinary, well-behaved sample of Cartesian
// space.
//
// **The angular seam wraps.** Polar->Rect reads a source column that is
// periodic in the horizontal axis (angle 2*pi and angle 0 are the same
// column), and `polarRemapTiles()`'s own sampler wraps it rather than
// zero-padding at the edge the way every other filter in this family does --
// a plain edge-clamped sample would tear a visible seam down one radius.
namespace np {

enum class PolarRemapDirection { RectToPolar, PolarToRect };

const char* polarRemapDirectionName(PolarRemapDirection d) noexcept;
std::optional<PolarRemapDirection> polarRemapDirectionFromName(std::string_view name) noexcept;

struct PolarRemapParams {
  PolarRemapDirection direction = PolarRemapDirection::RectToPolar;
};

// **A bad ROI citizen by construction** (docs/operations.md's own phrase for
// this filter). Every current caller already passes `outRect` spanning the
// whole canvas (`app/PixelOpBridge.hpp`'s `computePixelFilter()` always
// does), and both directions can read from -- or write to -- anywhere within
// that same rectangle, so a one-texel dilation is exactly right for every
// real call: bilinear taps near the disc's own true edge (radius approaching
// `rMax`) reach one texel past `outRect`'s own boundary, the identical "one
// texel of bilinear safety" every isotropic-window op in this codebase adds
// (`roiDilateOp()`). A caller that asked for a genuinely smaller `outRect`
// would still need the WHOLE canvas underneath it: the angular wrap alone
// rules out a rectangular margin (a small destination rect near the seam
// needs two disjoint source columns, not a padded one), and this engine does
// not attempt to serve that case.
RoiOp polarRemapRoiOp(const PolarRemapParams& p, const PixelRect& outRect) noexcept;

bool polarRemapTiles(const TileStore& src, const PixelRect& outRect, const PolarRemapParams& p,
                     TileStore* dst);

}  // namespace np
