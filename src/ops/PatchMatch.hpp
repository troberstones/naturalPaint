#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/Roi.hpp"

// ops/PatchMatch -- PRD D7's second half: texture-synthesis fill for a
// selected hole, Barnes et al. 2009's randomized nearest-neighbour-field
// search (random init, propagation, random search) inside a coarse-to-fine
// pyramid, with voting to reconstruct the hole from the resulting field.
//
// **Not ops/Inpaint.** That file's diffusion is a weighted average -- smooth,
// and honest about being smooth. This file copies whole patches of real
// texture, so a wall of brick or a stretch of grain comes back looking like
// brick or grain rather than like a blur of it. The cost is that it is a
// search, not a closed-form solve: two runs with different seeds can pick
// different equally-good patches, so a caller that wants the SAME fill twice
// must keep the seed.
//
// **This is destructive and seeded, not a cached op** (2026-09-13 owner
// decision, overriding PLAN.md phase 9's original "cached class-D op" plan).
// It commits pixels like any other filter; an action replay reproduces the
// result byte for byte because the seed is recorded, not because the answer
// is cached.
//
// ==========================================================================
// The hole threshold is 0.5, not ops/Inpaint's "> 0"
// ==========================================================================
//
// A diffusion fill blends a feathered rim texel's coverage back in afterward
// (app/FilterOps.hpp's composite), so treating any nonzero coverage as "wrong
// data" costs it nothing -- the rim's own value never has to be a real
// picture. A texture patch cannot be partially copied without destroying the
// high-frequency content the whole op exists to preserve, so this file has to
// pick a side at the rim instead of blending one: a texel at or below 0.5
// coverage is treated as genuine SOURCE data (its own value, real and usable,
// both to match against and to copy from); above 0.5 it is the HOLE. The
// composite step still blends the result back by the selection's true
// coverage afterward, so a soft edge still fades -- what changes is only
// which side of 0.5 a rim texel's OWN value comes from.
//
// ==========================================================================
// What "no source patch overlaps the hole" buys, and how it is kept exactly
// ==========================================================================
//
// A candidate source patch -- the (2*patchRadius+1)^2 window a hole texel's
// match points at -- is accepted only when every texel in that window is
// outside the hole. Inductively, every value ever read as "source" (during
// distance search, and during the final vote) is either an original known
// texel or a value already reconstructed from such sources, so nothing this
// file writes is a texel copied from itself. `patchMatchTiles()`'s optional
// `nnfOut` exposes the accepted field so a caller -- a selftest, in
// particular -- can check this from outside rather than trust the search
// that produced it.
namespace np {

inline constexpr int32_t kPatchMatchMaxPatchRadius = 12;
inline constexpr int32_t kPatchMatchMaxIterations = 16;
inline constexpr int32_t kPatchMatchMaxPyramidLevels = 6;
inline constexpr int32_t kPatchMatchMaxSourceMargin = 2048;

// The hole's bounding box cap, in texels. Named rather than derived: past
// this the search rectangle (the hole's bbox dilated by `sourceMargin`) is a
// genuinely large fraction of a real document, and this op is a search over
// it, not a closed-form pass -- "Select All, then Content-Aware Fill" is a
// request to synthesise a canvas from nothing, which this file refuses by
// name rather than grinding through slowly. 300000 texels comfortably covers
// the 200x200 (40000) case PRD D7 is measured against, with headroom for a
// wide scratch or a seam-heal band.
inline constexpr int64_t kPatchMatchMaxHoleTexels = 300000;

struct PatchMatchParams {
  // Coverage > 0.5 is the hole; see this header's own section above for why
  // the threshold differs from ops/Inpaint's. A borrowed pointer, valid for
  // the call and not retained. `nullptr` means no hole, refused exactly as
  // ops/Inpaint.hpp's inverted default is.
  const Selection* hole = nullptr;

  // Texels a source patch's CENTRE may not land on, in addition to the hole
  // itself -- e.g. seam heal keeps a fill from re-sourcing the other band
  // while it is being repaired at the same time. `nullptr` excludes nothing.
  const Selection* excludeSource = nullptr;

  // Patch size is `2*patchRadius+1` on a side.
  int32_t patchRadius = 3;
  // PatchMatch iterations (propagate + random search) run per pyramid level.
  int32_t iterations = 5;
  // Coarse-to-fine levels, coarsest first. Clamped internally to however many
  // the hole's own size actually supports before the patch stops fitting.
  int32_t pyramidLevels = 4;
  // How far outside the hole's bounding box a source patch may be centred.
  // Bounds the search (and the memory the search rectangle costs) on a huge
  // canvas around a small hole.
  int32_t sourceMargin = 256;
  // The whole reproducibility contract -- same seed, same inputs, bit-
  // identical output, via one fixed named PRNG (this file's own
  // splitmix64-based hash, matching ops/Filters.cpp's `filterRandomUniform`
  // in spirit). No threading: the scan order is fixed, so there is nothing
  // for a second thread's schedule to perturb.
  uint64_t seed = 0;
};

bool patchMatchParamsValid(const PatchMatchParams& p) noexcept;

// The tightest rectangle containing the hole, or `std::nullopt` when there is
// none. Mirrors ops/Inpaint.hpp's `inpaintHoleBounds()`.
std::optional<PixelRect> patchMatchHoleBounds(const PatchMatchParams& p);

// One accepted match: `target` (a hole texel, in document space) and
// `source` (the centre of the patch its value was reconstructed from, also
// document space). Populated only for the FINEST pyramid level actually run
// -- the level whose vote produced the texels `patchMatchTiles()` writes.
struct PatchMatchNnfEntry {
  PixelCoord target;
  PixelCoord source;
};

// Fills `p.hole` in `src` and writes the result into `dst`, exactly as
// ops/Inpaint.hpp's `inpaintTiles()` does: reads only texels outside the
// hole, writes only texels inside it, leaves every tile the hole does not
// reach absent from `dst`.
//
// `nnfOut`, when non-null, is cleared and filled with the finest level's
// accepted (target, source) pairs -- see this header's section on what that
// buys a caller that wants to check the "never sources from the hole"
// property independently rather than trust this function's own bookkeeping.
//
// Returns false and writes nothing when: `dst` is null or aliases `src`;
// `patchMatchParamsValid(p)` is false; `outRect` is empty or does not contain
// the hole's bounds (this op cannot be evaluated in pieces, for the identical
// reason ops/Inpaint.hpp section 4 gives); the hole's bounding box exceeds
// `kPatchMatchMaxHoleTexels`; or the search rectangle (hole bbox dilated by
// `sourceMargin`, clamped to `outRect`) contains not one valid source patch.
bool patchMatchTiles(const TileStore& src, const PixelRect& outRect, const PatchMatchParams& p,
                     TileStore* dst, std::vector<PatchMatchNnfEntry>* nnfOut = nullptr);

}  // namespace np
