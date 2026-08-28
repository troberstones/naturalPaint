#pragma once

#include <vector>

#include "core/TileStore.hpp"
#include "ops/PointOps.hpp"
#include "ops/Roi.hpp"

// ops/PointOpTiles -- the tile-level runner for ops/PointOps' pure
// `rgb -> rgb` functions, in the exact call shape `ops/Blur` and `ops/Filters`
// already use for the spatial ones:
//
//     (const TileStore& src, const PixelRect& outRect, const Params& p,
//      TileStore* out) -> bool
//
// That signature is not a coincidence and not a convenience -- it is the
// whole reason this file exists as its own module rather than as three more
// functions inside `ops/PointOps.cpp`. `app/PixelOpBridge.hpp`'s
// `computePixelFilter()`/`applyPixelFilter()` templates are written against
// exactly that shape, and they already carry every answer the Image >
// Adjustments menu needs and must not re-derive: which layer an op reaches,
// why it refuses, how a selection bounds it, when a tile forks copy-on-write,
// and how one op becomes one `core::History` entry. Matching the shape means
// an adjustment gets all of that by *calling the same template*, not by a
// second hand-written copy of it -- the identical argument
// `app/FilterOps.hpp` makes for why its own four filters share one wiring
// function.
//
// `ops/PointOps.hpp` keeps its stated purity: nothing there learns what a
// tile is. This module is the one place that knows both vocabularies.
//
// ==========================================================================
// Two properties a point op has that a spatial filter does not
// ==========================================================================
//
// **1. It never grows the layer.** `blurTiles()` allocates every tile across
// the requested rectangle, because a blur legitimately spreads paint into
// texels that held none (`app/FilterOps.hpp` names that cost explicitly). A
// point op cannot: `applyPointOpsPremultiplied()` maps `alpha <= 0` to
// `{0,0,0,0}` before any op runs, so a texel with no coverage comes out
// exactly as it went in, and a tile that does not exist would be created
// full of the value it already implies. So this runner iterates **the source
// store's allocated tiles**, intersected with `outRect` -- not the
// rectangle's tile range. A Levels adjustment on a 64x64 sketch inside a
// 8192x8192 canvas touches one tile, not sixteen thousand.
//
// This is a real behavioural commitment, not just an optimisation, and it is
// worth stating in the negative: an adjustment whose parameters map
// transparent black to something visible (Levels with `blackOut > 0`, an
// Invert, a Gradient Map whose t=0 stop is opaque white) still leaves empty
// canvas empty. That is correct -- those texels have zero alpha, and every
// op here is defined on colour, never on coverage -- and it matches what
// Photoshop's own Image > Adjustments do to a transparent region.
//
// **2. It needs no apron.** Every texel's output depends on that texel alone,
// so `outRect` is both the region read and the region written, and
// `ops/Roi.hpp`'s backward-propagation machinery has nothing to do: the
// `RoiOp` for any op in this file is the identity. The tile-seam invariant
// every spatial filter in `ops/Filters` has to earn is free here, and
// `--selftest` asserts it anyway -- cheaply, since the cost of proving it is
// two tiles rather than a padded gather.
namespace np {

// The parameter type `pointOpTiles()` takes: a run of ops/PointOps functions
// applied in list order, exactly what `applyPointOpsPremultiplied()` already
// consumes.
//
// **A `std::vector<PointOp>` rather than one `PointOp`**, because that is
// what the wrapper takes and because a run is the unit that will eventually
// be LUT-baked (ADR-0004, color/LutBake). A single adjustment passes a
// one-element vector; nothing about this runner changes when a caller
// composes several.
using PointOpRun = std::vector<PointOp>;

// Runs `ops` over every texel of `src` that lies inside `outRect` and inside
// an allocated tile, writing the result to `*out`.
//
// Returns false -- writing nothing -- for the three reasons every engine in
// `ops/Blur`/`ops/Filters` refuses, and for no others:
//   - `out` is null, or aliases `src` (an in-place run would read texels it
//     had already written; the callers all pass a distinct destination)
//   - `outRect` is empty
//   - `ops` is empty (an empty run is the identity, and returning false makes
//     `computePixelFilter()` treat it as the no-op it is rather than
//     allocating a full copy to prove nothing changed)
//
// `*out` is CLEARED on entry, not merged into, matching `blurTiles()`'s own
// contract -- the destination is this call's output, not an accumulator.
//
// **Alpha is copied, never computed.** Each texel goes through
// `applyPointOpsPremultiplied()` (ops/PointOps.hpp), which un-premultiplies,
// runs the ops on straight RGB, and re-premultiplies against the *original*
// alpha. No op in the family may modify alpha, and that header explains at
// length why an op that needs to does not belong behind the wrapper at all.
bool pointOpTiles(const TileStore& src, const PixelRect& outRect, const PointOpRun& ops,
                  TileStore* out);

}  // namespace np
