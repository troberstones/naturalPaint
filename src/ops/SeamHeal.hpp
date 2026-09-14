#pragma once

#include <cstdint>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/SeamHeal -- PRD D8's missing piece: repair the discontinuity where a
// texture's opposite edges meet, make-tileable's last step after Offset (PLAN.md
// phase 9; app/FilterOps.hpp section on the two make-tileable ops explains
// why seam heal was not built alongside them).
//
// The canonical gesture is Offset By Half followed by a look at the seam now
// crossing the middle of the canvas; this op automates the "now fix it" step:
// offset by half so the seams cross the middle, build a hole mask of a band
// along both centre lines, ops/PatchMatch-fill it with sources drawn under
// WRAP addressing (a patch near the canvas edge may legitimately match one
// that wraps to the opposite edge -- the whole point of a tileable texture),
// offset back.
//
// **Destructive and seeded**, the identical 2026-09-13 decision
// ops/PatchMatch.hpp states for content-aware fill: this commits pixels, and
// a recorded seed is what makes a replay reproduce the same repair.
//
// The two offsets are pure addressing changes (ops/Filters.hpp section 4),
// so every texel OUTSIDE the two bands comes back bit-identical to `src` --
// stated here because it is the property that makes this op safe to run
// blind on a document nobody has looked at yet: the only pixels it can
// possibly change are the ones it names.
namespace np {

struct SeamHealParams {
  // The canvas, exactly as `OffsetParams::wrapRect` and
  // `LightingGradientParams::statsRect` are -- this file needs a modulus, and
  // `ops/` has no notion of "the document" to supply one.
  PixelRect wrapRect{};
  // Full width of each band, in texels, centred on the seam. 0 is refused --
  // a zero-width band is nothing to heal and reports so rather than running
  // a no-op PatchMatch search.
  int32_t bandWidth = 12;
  int32_t patchRadius = 3;
  int32_t iterations = 5;
  int32_t pyramidLevels = 4;
  uint64_t seed = 0;
};

bool seamHealParamsValid(const SeamHealParams& p) noexcept;

// The two seam lines' document coordinates for this `wrapRect` -- exactly
// `applyOffset()`'s `offsetByHalf()`, restated here so a caller (and a test)
// can name where the bands sit without re-deriving the floor division.
PixelCoord seamHealCentre(const PixelRect& wrapRect) noexcept;

// Fills the seam bands and writes the WHOLE `outRect` into `dst` -- unlike
// ops/Inpaint's and ops/PatchMatch's hole-only writes, because every texel
// here passes through two address changes even where its value does not, and
// this file leaves it to `compositeFilterResult()`'s own per-texel diff
// (app/FilterOps.hpp) to recognise "unchanged" and skip the allocation,
// exactly as a sigma-0 blur already does.
//
// `outRect` must equal `p.wrapRect` -- this op is defined over the whole
// canvas, not a piece of it (`ops/Filters.hpp`'s offset already refuses to be
// tiled for the identical reason). Returns false and writes nothing when
// `dst` is null or aliases `src`, `seamHealParamsValid(p)` is false,
// `outRect != p.wrapRect`, or the interior PatchMatch search refuses (the
// band exceeds `kPatchMatchMaxHoleTexels`, or no valid source patch exists).
bool seamHealTiles(const TileStore& src, const PixelRect& outRect, const SeamHealParams& p,
                   TileStore* dst);

}  // namespace np
