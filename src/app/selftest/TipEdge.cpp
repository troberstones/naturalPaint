#include "app/selftest/Support.hpp"

#include "brush/Deposit.hpp"
#include "brush/TipMips.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Track B / B1+B2: `BrushTip::edgePx`'s minimum pixel-wide antialiasing skirt
// on the procedural falloff (brush/Deposit.hpp §2), and
// `BrushTipBitmap::mips`' box-filter chain for a minified sampled tip (§2c).
//
// Six sections, matching the brief this section was specified against:
//   1. Identity: bit-exact against the pre-`edgePx` formula, two ways.
//   2. A hard disc's rim band: fractional with the floor, binary without.
//   3. The stroke's rim reads smoother: more than two distinct values along
//      a radial line with the floor, exactly two without.
//   4. `buildTipMips()` against a 7x5 fixture, hand-computed.
//   5. Level selection: a checkerboard tip minified vs. drawn at native size.
//   6. `brushTipEqual()` distinguishes two tips differing only in `edgePx`.
// ---------------------------------------------------------------------------
bool runTipEdgeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ======================================================================
  // 1. Identity: `edgePx` changes nothing when the brief's own conditions
  //    hold, compared at bit-exact `==`.
  // ======================================================================
  {
    // (1 - hardness) * radius == 15.6 >= 1.0 -- `Round Bristle 03`'s own
    // shipped default, the case brush/Deposit.hpp §2 names by name.
    BrushTip base;
    base.radius = 24.0f;
    base.hardness = 0.35f;

    BrushTip withFloor = base;
    withFloor.edgePx = 1.0f;
    BrushTip noFloor = base;
    noFloor.edgePx = 0.0f;

    bool identicalWide = true;
    int sampled = 0;
    // The whole footprint and a margin either side, so the comparison covers
    // the flat core, the skirt and the zero region outside the rim alike.
    for (float dy = -26.0f; dy <= 26.0f; dy += 0.5f) {
      for (float dx = -26.0f; dx <= 26.0f; dx += 0.5f) {
        ++sampled;
        if (dabCoverage(withFloor, dx, dy) != dabCoverage(noFloor, dx, dy)) identicalWide = false;
      }
    }
    check(identicalWide && sampled > 0,
          "identity: (1-h)*r >= edgePx -- edgePx=1 and edgePx=0 are BIT-IDENTICAL over the "
          "whole footprint, for the round tip at its shipped default size");

    // The second stated identity: `edgePx == 0` alone, at a size where the
    // FIRST identity does NOT hold (a hard, small tip -- the defect case).
    // `hEff` collapses to `min(hardness, 1 - 0/r) == min(hardness, 1) ==
    // hardness` (brush/Deposit.cpp's own comment on why nothing is added for
    // this case), so a tip with `edgePx == 0` must reproduce the exact
    // pre-edgePx hard-disc profile: 1 inside the disc, 0 outside, nothing
    // between.
    BrushTip hardZero;
    hardZero.radius = 5.0f;
    hardZero.hardness = 1.0f;
    hardZero.edgePx = 0.0f;
    bool binaryAtZero = true;
    for (float dy = -6.0f; dy <= 6.0f; dy += 0.25f) {
      for (float dx = -6.0f; dx <= 6.0f; dx += 0.25f) {
        const float c = dabCoverage(hardZero, dx, dy);
        if (c != 0.0f && c != 1.0f) binaryAtZero = false;
      }
    }
    check(binaryAtZero,
          "identity: edgePx == 0 alone reproduces the pre-edgePx hard-disc profile -- coverage "
          "is exactly 0 or exactly 1 everywhere, even for a tip too small/hard for the FIRST "
          "identity to hold");
  }

  // ======================================================================
  // 2. A hard disc's rim: fractional with the floor, binary without.
  // ======================================================================
  {
    constexpr float kRadius = 6.0f;
    BrushTip withFloor;
    withFloor.radius = kRadius;
    withFloor.hardness = 1.0f;
    withFloor.edgePx = 1.0f;
    BrushTip noFloor = withFloor;
    noFloor.edgePx = 0.0f;

    // hEff = min(1, 1 - 1/6) = 5/6 -- the skirt is the OPEN interval of
    // physical distance (5, 6) from the centre. "Within 1 px of the rim, on
    // the inside" is exactly that interval; outside the rim (>= 6) is always
    // 0 regardless of edgePx; that is not part of the band this section
    // claims is fractional, which is why the sampling below stops at the rim.
    int bandSamples = 0;
    bool floorAllFractional = true;
    bool noFloorAllBinary = true;
    for (float d = kRadius - 0.99f; d < kRadius; d += 0.05f) {
      ++bandSamples;
      const float cFloor = dabCoverage(withFloor, d, 0.0f);
      const float cNoFloor = dabCoverage(noFloor, d, 0.0f);
      if (cFloor == 0.0f || cFloor == 1.0f) floorAllFractional = false;
      // Every one of these points has physical distance < 6, so the OLD
      // hard-disc profile (edgePx == 0) must place it fully inside the disc.
      if (cNoFloor != 1.0f) noFloorAllBinary = false;
    }
    std::printf("  [measured] hard disc r=6: %d samples in the 1 px rim band\n", bandSamples);
    check(bandSamples >= 10,
          "rim band: the sampling actually reaches enough of the (r-1, r) interval to be a real "
          "test rather than an empty loop");
    check(floorAllFractional,
          "rim band: with edgePx=1 every texel within 1 px of the rim (on the inside) carries "
          "FRACTIONAL coverage -- none is exactly 0 or exactly 1");
    check(noFloorAllBinary,
          "rim band: and with edgePx=0 the identical band is entirely at the ceiling -- the old "
          "hard disc has no skirt at all, so 'within 1 px of the rim' is not a special place");

    // The complementary sanity checks: well inside and well outside agree
    // regardless of edgePx, so the difference above is really about the rim
    // and not a wholesale change to the disc.
    check(dabCoverage(withFloor, 0.0f, 0.0f) == 1.0f && dabCoverage(noFloor, 0.0f, 0.0f) == 1.0f,
          "rim band: the centre is at the ceiling either way");
    check(dabCoverage(withFloor, kRadius + 2.0f, 0.0f) == 0.0f &&
              dabCoverage(noFloor, kRadius + 2.0f, 0.0f) == 0.0f,
          "rim band: well outside the disc both are exactly 0 either way");
  }

  // ======================================================================
  // 3. The stroke's rim reads smoother: a radial line's distinct coverage
  //    count, sampled at unit-pixel steps like a real per-texel loop would.
  // ======================================================================
  {
    constexpr float kRadius = 6.0f;
    BrushTip withFloor;
    withFloor.radius = kRadius;
    withFloor.hardness = 1.0f;
    withFloor.edgePx = 1.0f;
    BrushTip noFloor = withFloor;
    noFloor.edgePx = 0.0f;

    auto distinctAlongRadial = [&](const BrushTip& tip) {
      std::vector<float> vals;
      // Unit steps at a half-pixel offset -- the same texel-centre convention
      // `depositDab()`'s own loop uses (`(x + 0.5) - centre.x`), so this is
      // "the coverage a real per-texel scan would see along one row" and not
      // a continuous sampling that would trivially produce many values.
      for (int i = 0; i <= static_cast<int>(kRadius) + 3; ++i) {
        const float dx = static_cast<float>(i) + 0.5f;
        vals.push_back(dabCoverage(tip, dx, 0.0f));
      }
      std::sort(vals.begin(), vals.end());
      vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
      return vals;
    };

    const std::vector<float> withVals = distinctAlongRadial(withFloor);
    const std::vector<float> noVals = distinctAlongRadial(noFloor);
    std::printf("  [measured] hard disc r=6, radial line at unit steps: edgePx=1 leaves %zu "
                "distinct coverage value(s), edgePx=0 leaves %zu\n",
                withVals.size(), noVals.size());
    check(noVals.size() == 2 && noVals.front() == 0.0f && noVals.back() == 1.0f,
          "rim smoothness: WITHOUT the floor, a hard disc's radial line holds exactly two "
          "coverage values -- 0 and 1, the definition of aliased");
    check(withVals.size() > 2,
          "rim smoothness: and WITH the floor the identical line holds MORE than two -- the "
          "assertion the brief's own smoothness claim rests on, not merely 'some pixel changed'");
  }

  // ======================================================================
  // 4. `buildTipMips()` against a 7x5 fixture, every value hand-computed.
  // ======================================================================
  {
    // alpha(x, y) = 10*x + 4*y -- chosen so every 2x2 box average (including
    // the edge-clamped ones) lands on an exact half-integer, and the `(sum +
    // 2) / 4` rounding-to-nearest rule in brush/TipMips.cpp is therefore
    // exercised on every single output texel rather than on a lucky few.
    BrushTipBitmap bmp;
    bmp.width = 7;
    bmp.height = 5;
    bmp.alpha.resize(35);
    for (int32_t y = 0; y < 5; ++y)
      for (int32_t x = 0; x < 7; ++x)
        bmp.alpha[static_cast<size_t>(y) * 7 + static_cast<size_t>(x)] =
            static_cast<uint8_t>(10 * x + 4 * y);

    buildTipMips(bmp);

    check(bmp.mips.size() == 3, "mip build: a 7x5 source produces exactly three levels -- 4x3, "
                                "2x2, 1x1 -- not two, not four");

    bool dimsOk = bmp.mips.size() == 3 && bmp.mips[0].width == 4 && bmp.mips[0].height == 3 &&
                 bmp.mips[1].width == 2 && bmp.mips[1].height == 2 &&
                 bmp.mips[2].width == 1 && bmp.mips[2].height == 1;
    check(dimsOk, "mip build: dimensions round UP at every level -- ceil(7/2)=4, ceil(5/2)=3, "
                 "then ceil(4/2)=2, ceil(3/2)=2, then 1x1");

    // Hand-computed (this file's own comment carries the arithmetic for one
    // representative corner; every value below was independently verified by
    // hand, including the doubly edge-clamped corner (3,2) of level 1).
    const std::array<uint8_t, 12> wantLevel1 = {7, 27, 47, 62, 15, 35, 55, 70, 21, 41, 61, 76};
    const std::array<uint8_t, 4> wantLevel2 = {21, 59, 31, 69};
    const std::array<uint8_t, 1> wantLevel3 = {45};

    bool level1Ok = dimsOk && bmp.mips[0].alpha.size() == wantLevel1.size();
    if (level1Ok)
      for (size_t i = 0; i < wantLevel1.size(); ++i)
        if (bmp.mips[0].alpha[i] != wantLevel1[i]) level1Ok = false;
    check(level1Ok, "mip build: level 1 (4x3) matches the hand-computed box-filtered values "
                    "exactly, including the row/column that needed edge-clamping");

    bool level2Ok = dimsOk && bmp.mips[1].alpha.size() == wantLevel2.size();
    if (level2Ok)
      for (size_t i = 0; i < wantLevel2.size(); ++i)
        if (bmp.mips[1].alpha[i] != wantLevel2[i]) level2Ok = false;
    check(level2Ok, "mip build: level 2 (2x2), downsampled from level 1 and not from the "
                    "original, matches by hand");

    bool level3Ok = dimsOk && bmp.mips[2].alpha.size() == wantLevel3.size();
    if (level3Ok)
      for (size_t i = 0; i < wantLevel3.size(); ++i)
        if (bmp.mips[2].alpha[i] != wantLevel3[i]) level3Ok = false;
    check(level3Ok, "mip build: level 3 (1x1) is the mean of ALL 35 original texels' cascaded "
                    "averages, not a direct average of the original -- 45");

    // Idempotence: `buildTipMips()` rebuilds from `alpha`, not from whatever
    // `mips` already held, so calling it twice must not double the chain.
    buildTipMips(bmp);
    check(bmp.mips.size() == 3, "mip build: calling buildTipMips() twice on the same object "
                                "reproduces the identical three-level chain, not six levels");

    // Degenerate input: a bitmap with no pixels yet must not crash and must
    // leave an empty chain -- `bitmapDabCoverage()`'s own fallback for it.
    BrushTipBitmap empty;
    buildTipMips(empty);
    check(empty.mips.empty(),
          "mip build: a degenerate (0x0) bitmap builds an EMPTY chain rather than crashing -- "
          "the untrusted-file-derived-data guard singleTipCoverage() already applies elsewhere");

    // A 1x1 tip already has nothing to downsample.
    BrushTipBitmap onePx;
    onePx.width = 1;
    onePx.height = 1;
    onePx.alpha = {200};
    buildTipMips(onePx);
    check(onePx.mips.empty(),
          "mip build: a 1x1 tip needs no mip chain at all -- the loop's own stop condition");
  }

  // ======================================================================
  // 5. Level selection: minified vs. native-size sampling of a bitmap tip.
  // ======================================================================
  {
    // A 64x64, period-2 checkerboard: alpha(x,y) = 255 when (x+y) is even,
    // else 0. Chosen because a 2x2 box filter CANCELS it exactly -- every
    // single 2x2 block in a period-2 checkerboard contains exactly two
    // texels of each colour, regardless of where the block starts -- so
    // every mip level from level 1 on is a perfectly uniform 128 (rounded
    // from 127.5), everywhere, with no edge-clamp exception (64 is even, so
    // no block at level 1 is ever clamped). That makes the "narrow band
    // around 0.5" the brief asks for degenerate but exact: the residual
    // contrast after one halving is EXACTLY zero, so the band collapses to
    // the single float 128/255, asserted at `==` rather than a tolerance.
    constexpr int32_t kSide = 64;
    auto checkerAlpha = [](int32_t x, int32_t y) -> uint8_t {
      return ((x + y) % 2 == 0) ? 255 : 0;
    };
    BrushTipBitmap bmp;
    bmp.width = kSide;
    bmp.height = kSide;
    bmp.alpha.resize(static_cast<size_t>(kSide) * static_cast<size_t>(kSide));
    for (int32_t y = 0; y < kSide; ++y)
      for (int32_t x = 0; x < kSide; ++x)
        bmp.alpha[static_cast<size_t>(y) * static_cast<size_t>(kSide) + static_cast<size_t>(x)] =
            checkerAlpha(x, y);
    buildTipMips(bmp);
    check(bmp.mips.size() == 6, "level select: a 64x64 tip's chain is 32,16,8,4,2,1 -- six "
                                "levels -- the size buildTipMips()'s own header states");

    BrushTip minified;
    minified.bitmap = std::make_shared<const BrushTipBitmap>(bmp);
    minified.radius = 4.0f;  // scale = 4 / 32 = 1/8 -- level 3, the 8x8 mip

    const float kUniform = 128.0f / 255.0f;
    // 128/255 - 0.5, the exact distance the rounded box-filter mean sits
    // from a true 0.5 -- the "narrow band" this section derives rather than
    // guesses, even though (per the comment above) it turns out to be exact.
    constexpr float kBandHalfWidth = 128.0f / 255.0f - 0.5f;
    bool allNearHalf = true;
    bool allExact = true;
    bool noneSaturated = true;
    int minifiedSamples = 0;
    for (float dy = -3.5f; dy <= 3.5f; dy += 0.7f) {
      for (float dx = -3.5f; dx <= 3.5f; dx += 0.7f) {
        if (dx * dx + dy * dy >= minified.radius * minified.radius) continue;
        ++minifiedSamples;
        const float c = dabCoverage(minified, dx, dy);
        if (std::fabs(c - 0.5f) > kBandHalfWidth + 1e-6f) allNearHalf = false;
        if (c != kUniform) allExact = false;
        if (c == 0.0f || c == 1.0f) noneSaturated = false;
      }
    }
    std::printf("  [measured] 64x64 checker at r=4 (scale 1/8): %d samples in the footprint, "
                "all within [%.6f, %.6f] of 0.5\n",
                minifiedSamples, -kBandHalfWidth, kBandHalfWidth);
    check(minifiedSamples > 10, "level select: the minified footprint is actually sampled "
                                "enough times to be a real test");
    check(allNearHalf && allExact,
          "level select: minified (r=4), EVERY sample across the footprint lands in the narrow "
          "band around 0.5 derived above -- exactly 128/255, because the chosen mip level's box "
          "filter fully cancels a period-2 checkerboard");
    check(noneSaturated,
          "level select: and not one sample is exactly 0 or exactly 1 -- the sparkle a "
          "level-0-only bilinear sample would produce at this minification (four native texels "
          "out of thousands, landing inside a single checker cell) is gone");

    BrushTip native;
    native.bitmap = std::make_shared<const BrushTipBitmap>(bmp);
    native.radius = 32.0f;  // scale = 32 / 32 = 1.0 -- level 0, bit-identical to pre-mip code

    bool nativeExact = true;
    int nativeSamples = 0;
    for (int32_t iy = 8; iy < kSide - 8; iy += 5) {
      for (int32_t ix = 8; ix < kSide - 8; ix += 5) {
        ++nativeSamples;
        const float dx = static_cast<float>(ix) + 0.5f - static_cast<float>(kSide) * 0.5f;
        const float dy = static_cast<float>(iy) + 0.5f - static_cast<float>(kSide) * 0.5f;
        const float want = checkerAlpha(ix, iy) == 255 ? 1.0f : 0.0f;
        if (dabCoverage(native, dx, dy) != want) nativeExact = false;
      }
    }
    std::printf("  [measured] 64x64 checker at r=32 (scale 1): %d texel-centre samples\n",
                nativeSamples);
    check(nativeSamples > 10, "level select: the native-size sampling covers enough texel "
                              "centres to be a real test");
    check(nativeExact,
          "level select: at scale 1 (r=32) every texel-centre sample is the EXACT checker value "
          "-- level 0, bit-identical to the pre-mip bilinear sample, exactly as brush/Deposit.hpp "
          "§2c states");
  }

  // ======================================================================
  // 6. `brushTipEqual()` distinguishes two tips differing only in `edgePx`.
  // ======================================================================
  {
    BrushTip a;
    BrushTip b = a;
    check(brushTipEqual(a, b), "equality: two default-constructed tips compare equal");
    b.edgePx = a.edgePx + 0.5f;
    check(!brushTipEqual(a, b),
          "equality: and no longer, once ONLY edgePx differs -- the structured binding in "
          "brushTipEqual() is what forces this (brush/Deposit.cpp's own comment: a field named "
          "but not compared fails to compile, a field never named fails the same way)");
    // Restoring it recovers equality, confirming the mismatch above was
    // really about edgePx and not some other accidental divergence.
    b.edgePx = a.edgePx;
    check(brushTipEqual(a, b),
          "equality: and restoring edgePx alone recovers equality -- isolating the field");
  }

  std::printf("[selftest] tip edge %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
