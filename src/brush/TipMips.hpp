#pragma once

#include "brush/Deposit.hpp"

// brush/TipMips -- **the box-filter mip chain a sampled bitmap tip needs to
// minify without sparkling.**
//
// Track B / B2. `brush/Deposit.hpp`'s own §2c explains the defect this
// exists to fix (a bitmap tip drawn far smaller than its native pixel
// dimensions is bilinear-sampled from four texels out of the thousands an
// output texel actually spans, which is noise rather than an average) and
// the resolution (`BrushTipBitmap::mips`, sampled by `bitmapDabCoverage()`
// at a level chosen so one output texel never spans much more than the ~2
// source texels a bilinear sample can blend -- along the tip's MAJOR axis;
// along the minor axis of an elliptical tip it is up to `2 / roundness`, a
// limit brush/Deposit.hpp §2c states and measures rather than removes).
// This file is only the one
// function that BUILDS the chain -- `buildTipMips()` is called once, at
// every site that decodes a `BrushTipBitmap` for painting (`io/AbrBrushes.cpp`,
// `app/DabLibrary.cpp`), the same moment `alpha` itself is decoded and before
// the bitmap is shared out as `std::shared_ptr<const BrushTipBitmap>` --
// never later, and never on a tip already wrapped `const`, because §2c's own
// "immutable once built, safe to share lock-free" argument has to hold for
// `mips` too.
//
// **The filter is a 2x2 box, not a general resample.** A box filter is the
// exact right answer for what this chain is FOR: level k+1's texel is the
// unweighted mean of exactly the four level-k texels an output texel at that
// minification would otherwise sample a corner of, so the chain answers "what
// is the average coverage over this many source texels" rather than
// approximating some other reconstruction kernel. It is also the cheapest
// correct answer -- one add and one divide-by-4 per output texel, done once
// per tip rather than once per dab.
//
// **Dimensions round UP, never down, to a floor of `1x1`.** An odd source
// dimension (`7`, in the fixture `app/selftest/TipEdge.cpp` hand-checks)
// would lose a whole row or column of source data if the next level rounded
// down (`7/2 == 3`, dropping row/column 6 entirely); rounding up
// (`(7+1)/2 == 4`) instead means every level-k texel maps to a real,
// non-empty block of level-(k-1) texels, including the last one -- which is
// necessarily a 1x1 block when the source dimension was odd, not a 2x2 one,
// and that block's own edge-clamp (below) makes it the mean of one real
// texel repeated four times, i.e. that texel's own value, rather than an
// average diluted by a phantom neighbour.
//
// **Edge-clamped, not zero-padded, for a 2x2 source block that runs off the
// low-resolution level's own edge.** The alternative -- treating an
// out-of-range source texel as coverage 0 -- would darken every tip's own
// silhouette edge at every mip level, which is exactly the antialiasing
// defect §2c's bilinear sampling already avoids by clamping (its own
// comment: "keeps the mapped rectangle's own border crisp instead of
// feathering it by half a texel for free"). Clamping the box filter's
// sources the same way keeps that property true at every level, not only at
// level 0.
namespace np {

// Builds `bmp.mips` from `bmp.width`/`height`/`alpha` -- level 0 is `bmp`
// itself (not stored in `mips`), `mips[0]` a 2x2 box downsample of it,
// `mips[1]` a downsample of `mips[0]`, and so on down to a `1x1` level.
// `mips` is cleared and rebuilt from `bmp`'s CURRENT `alpha` every call, so
// calling this twice on the same object is idempotent rather than additive.
//
// A no-op (leaves `mips` empty) for a degenerate bitmap -- `width <= 0`,
// `height <= 0`, or `alpha.size()` not matching `width * height` -- the same
// guard `singleTipCoverage()` (brush/Deposit.cpp) already applies before
// trusting a `BrushTipBitmap`: a tip is untrusted-file-derived data by the
// time it reaches here, and an empty chain is `bitmapDabCoverage()`'s own
// "level 0 only" fallback, not a crash.
void buildTipMips(BrushTipBitmap& bmp);

}  // namespace np
