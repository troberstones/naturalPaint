#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/RadialBlur -- Spin and Zoom (docs/operations.md §2.2, P2: "Radial / spin
// blur -- polar-space line integral", "Zoom blur -- same machinery as radial,
// different axis").
//
// Photoshop's own pairing: one dialog, one engine, two methods that differ
// only in which polar axis the samples walk. Spin integrates along the ARC
// through a texel about a centre (constant radius, varying angle); Zoom
// integrates along the RAY from that centre (constant angle, varying radius).
// Both share `ops/Filters.hpp`'s motion-blur shape -- an odd number of taps
// straddling the output texel's own position, box-averaged in the tile
// store's native premultiplied linear space, no un-premultiply anywhere, for
// the identical reason `motionBlurTiles()` needs none.
//
// **The centre is a parameter, not a computed default.** This engine does not
// know the canvas -- app/FilterOps.hpp supplies the centre the same way it
// supplies `canvasRectOf()` to `applyOffset()`/`applyLensCorrect()`.
namespace np {

enum class RadialBlurMethod { Spin, Zoom };

const char* radialBlurMethodName(RadialBlurMethod m) noexcept;
std::optional<RadialBlurMethod> radialBlurMethodFromName(std::string_view name) noexcept;

struct RadialBlurParams {
  RadialBlurMethod method = RadialBlurMethod::Spin;

  // Document-texel coordinates. Whatever position both methods hold fixed --
  // see `radialBlurTiles()`'s own comment for why that falls out of the
  // maths rather than needing a special case.
  float centerX = 0.0f;
  float centerY = 0.0f;

  // Spin: the total arc swept, in degrees. Zoom: the radial scale fraction
  // each tap reaches at its extreme. The taps run symmetrically about zero, so
  // a negative amount gives the same result as a positive one.
  // **0 is the exact identity for either method**
  // -- `radialBlurTiles()` short-circuits to a bit-exact copy rather than
  // let a zero sweep/scale reach the trig at all.
  float amount = 0.0f;

  // Half the tap count on each side of the output texel's own (zero-offset)
  // sample; total taps = `2*samples + 1`, `ops/Filters.hpp`'s motion-blur
  // convention restated so the centre offset (t=0) always lands exactly on
  // the untouched position.
  int32_t samples = 8;
};

// False for a non-finite centre/amount or `samples < 1`.
bool radialBlurParamsValid(const RadialBlurParams& p) noexcept;

// `RoiOp{}` (the exact identity) at `amount == 0`. Otherwise an ISOTROPIC
// margin -- same on all four sides, because unlike motion blur's fixed
// direction, a texel's displacement direction here depends on where it sits
// relative to the centre, which varies across `outRect`.
//
// Derivation: every tap displaces a texel at distance `r` from the centre by
// at most `r * |amount_scaled|` (Spin: half the swept angle in radians;
// Zoom: the scale fraction itself) -- Spin's bound is the chord-vs-arc
// inequality `2*sin(t/2) <= t` for all `t >= 0` (proof: `f(t) = t -
// 2*sin(t/2)` has `f(0) = 0` and `f'(t) = 1 - cos(t/2) >= 0`, so `f` never
// goes negative), Zoom's is immediate from `|scale - 1| <= |amount|`. `r`
// itself is bounded by the farthest of `outRect`'s four corners from the
// centre -- the maximum of a convex (distance-to-point) function over a
// rectangle is always at a corner, whether or not the centre lies inside it.
// One texel of bilinear safety is added on top, matching every dilation in
// this codebase that samples off the integer grid.
RoiOp radialBlurRoiOp(const RadialBlurParams& p, const PixelRect& outRect) noexcept;

// **The centre texel is a fixed point of both methods.** Not a special case:
// at `r == 0` every Spin sample is `centre + 0*(cos,sin)` and every Zoom
// sample is `centre + 0*scale`, regardless of `amount` or `samples`.
bool radialBlurTiles(const TileStore& src, const PixelRect& outRect, const RadialBlurParams& p,
                     TileStore* dst);

}  // namespace np
