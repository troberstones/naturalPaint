#include "app/selftest/Support.hpp"

#include "app/DabPreview.hpp"
#include "brush/Deposit.hpp"
#include "brush/Grain.hpp"
#include "brush/Library.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp): US 5,347,620's tiled
// grain field, `F = clamp(P*S*O1 - G, 0, 1)`, and the two call sites that
// apply it -- `depositDab()`'s inner loop and `app/DabPreview`'s
// `dabPreviewTexel()`. See brush/Grain.hpp's own header for the licensing
// accounting (what is the patent's, what is this file's own derivation) and
// for §3's argument that absolute-position keying is the whole feature.
// ---------------------------------------------------------------------------
bool runGrainTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // A few ULPs of 1.0 -- the only place this file compares two DIFFERENT
  // floating-point expressions that are equal in exact arithmetic (section C
  // below): `Fvalley - Fpeak` against `Gmax - Gmin`. Each side is at most two
  // correctly-rounded subtractions on magnitudes <= 1, so 4 ulps of 1.0 =
  // 4.768e-07 bounds it -- the identical derivation runPigmentDepositTest()
  // states for its own lerp-vs-quotient comparison.
  constexpr float kAlgebraTol = 4.7683716e-07f;
  // binary16 storage tolerance, restated (not shared) for
  // runPigmentDepositTest()'s own reason: a tolerance borrowed without its
  // derivation is the one that later gets applied where it does not hold.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };

  Latent yellow;
  yellow.c = {0.0f, 1.0f, 0.0f};

  // ======================================================================
  std::printf("  -- A. the field: deterministic, tiled, bounded, and not flat --\n");
  // ======================================================================
  {
    GrainParams p;
    p.enabled = true;  // grainHeightAt() itself does not read `enabled` --
                       // this field only matters to grainCoverageAt() -- but
                       // set for clarity, since a `GrainParams` describing a
                       // paper reads oddly with `enabled` left false.
    p.periodX = 24;
    p.periodY = 24;
    p.depth = 0.35f;

    // 1. Pure and deterministic: the same call, twice, bit for bit.
    const float a1 = grainHeightAt(p, 137, 42);
    const float a2 = grainHeightAt(p, 137, 42);
    check(a1 == a2, "grain/height: deterministic across repeated evaluation");

    // 2. Tiles exactly, on both axes, at several phases -- not merely at the
    //    origin, where an off-by-one in the wrap could hide.
    bool tilesX = true, tilesY = true;
    for (int32_t x = -3; x <= 3; ++x) {
      for (int32_t y = -3; y <= 3; ++y) {
        if (grainHeightAt(p, x, y) != grainHeightAt(p, x + p.periodX, y)) tilesX = false;
        if (grainHeightAt(p, x, y) != grainHeightAt(p, x, y + p.periodY)) tilesY = false;
      }
    }
    check(tilesX, "grain/height: tiles exactly -- x and x+periodX agree bit for bit");
    check(tilesY, "grain/height: tiles exactly -- y and y+periodY agree bit for bit");

    // 3. A negative coordinate wraps into the same bucket as its positive
    //    equivalent (`-1 mod 24 == 23`), not into a different one -- the
    //    specific failure mode of using `%` without correcting its sign.
    check(grainHeightAt(p, -1, 5) == grainHeightAt(p, p.periodX - 1, 5),
          "grain/height: a negative coordinate wraps to the correct positive bucket");

    // 4. Bounded in [0, depth] everywhere sampled, and genuinely NOT flat --
    //    both facts this same sweep establishes, and both needed by section C.
    float lo = 1e9f, hi = -1e9f;
    bool inRange = true;
    for (int32_t x = 0; x < p.periodX; ++x) {
      for (int32_t y = 0; y < p.periodY; ++y) {
        const float g = grainHeightAt(p, x, y);
        if (!(g >= 0.0f && g <= p.depth)) inRange = false;
        lo = std::min(lo, g);
        hi = std::max(hi, g);
      }
    }
    check(inRange, "grain/height: every sampled value lies in [0, depth]");
    check(hi > lo, "grain/height: the field is not constant across one full tile");
    std::printf("    [measured] one %dx%d tile: G ranges [%.4f, %.4f] (depth=%.2f)\n",
                p.periodX, p.periodY, static_cast<double>(lo), static_cast<double>(hi),
                static_cast<double>(p.depth));
  }

  // ======================================================================
  std::printf("  -- B. F = clamp(P*S*O1 - G, 0, 1): the clamp endpoints and the plain sum --\n");
  // ======================================================================
  {
    // raw <= 0: G at or above P*S*O1.
    check(grainOverlayFraction(0.4f, 1.0f, 1.0f, 0.4f) == 0.0f,
          "grain/formula: raw == 0 gives exactly 0");
    check(grainOverlayFraction(0.2f, 1.0f, 1.0f, 0.9f) == 0.0f,
          "grain/formula: raw < 0 clamps to exactly 0");
    // raw >= 1: a saturating tip well past a shallow valley.
    check(grainOverlayFraction(1.0f, 2.0f, 1.0f, 0.0f) == 1.0f,
          "grain/formula: raw == 1 gives exactly 1");
    check(grainOverlayFraction(2.0f, 2.0f, 1.0f, 0.0f) == 1.0f,
          "grain/formula: raw > 1 clamps to exactly 1");
    // Mid-range: P, S, O1 and G chosen as exact binary fractions (0.75, 0.25)
    // so `raw` is exact and the comparison needs no tolerance at all.
    check(grainOverlayFraction(0.75f, 1.0f, 1.0f, 0.25f) == 0.5f,
          "grain/formula: mid-range is the plain sum P*S*O1 - G, exactly");
    // S and O1 both actually multiply in, not just P alone.
    check(grainOverlayFraction(0.5f, 0.5f, 1.0f, 0.0f) == 0.25f,
          "grain/formula: S scales P before the subtraction");
    check(grainOverlayFraction(0.5f, 1.0f, 0.5f, 0.0f) == 0.25f,
          "grain/formula: O1 scales P before the subtraction");
  }

  // ======================================================================
  std::printf("  -- C. the whole point: a peak gets LESS coverage than a valley, quantified --\n");
  // ======================================================================
  {
    GrainParams p;
    p.enabled = true;
    p.periodX = 8;
    p.periodY = 8;
    p.depth = 0.35f;

    int32_t peakX = 0, peakY = 0, valleyX = 0, valleyY = 0;
    float gPeak = -1.0f, gValley = 2.0f;
    for (int32_t x = 0; x < p.periodX; ++x) {
      for (int32_t y = 0; y < p.periodY; ++y) {
        const float g = grainHeightAt(p, x, y);
        if (g > gPeak) { gPeak = g; peakX = x; peakY = y; }
        if (g < gValley) { gValley = g; valleyX = x; valleyY = y; }
      }
    }
    check(gPeak > gValley, "grain/peak-valley: the swept tile has a distinct peak and valley");

    // P chosen so `P - gPeak` and `P - gValley` both land strictly inside
    // (0,1) -- neither clamp of section B engages, which is what makes the
    // subtraction below an exact algebraic identity rather than a clamped
    // approximation of one.
    const float P = 0.6f;
    check(P - gPeak > 0.0f && P - gPeak < 1.0f && P - gValley > 0.0f && P - gValley < 1.0f,
          "grain/peak-valley: P is chosen so neither clamp engages (both raws in (0,1))");

    const float Fpeak = grainOverlayFraction(P, 1.0f, 1.0f, gPeak);
    const float Fvalley = grainOverlayFraction(P, 1.0f, 1.0f, gValley);
    check(Fpeak < Fvalley,
          "grain/peak-valley: the SAME pressure gives the peak texel LESS coverage");

    // Quantified, not just signed: with neither clamp engaged,
    // `Fvalley - Fpeak == (P-gValley) - (P-gPeak) == gPeak - gValley` exactly
    // in real arithmetic. Asserted to a few ulps rather than at zero because
    // the two sides are computed via two DIFFERENT sequences of roundings
    // (through `grainOverlayFraction()` twice, versus one direct subtraction).
    const float gap = Fvalley - Fpeak;
    const float wantGap = gPeak - gValley;
    check(std::fabs(gap - wantGap) <= kAlgebraTol,
          "grain/peak-valley: the coverage gap equals Gpeak-Gvalley (quantified, not just signed)");
    std::printf(
        "    [measured] peak (%d,%d) G=%.4f -> F=%.4f | valley (%d,%d) G=%.4f -> F=%.4f | "
        "gap=%.4f\n",
        peakX, peakY, static_cast<double>(gPeak), static_cast<double>(Fpeak), valleyX, valleyY,
        static_cast<double>(gValley), static_cast<double>(Fvalley), static_cast<double>(gap));
  }

  // ======================================================================
  std::printf("  -- D. grain OFF: a no-op, bit for bit, not merely numerically close --\n");
  // ======================================================================
  {
    // grainCoverageAt() itself, standalone: the disabled branch must return
    // its `coverage` argument utterly unchanged, for values that are not
    // otherwise special (0, 1, and one ordinary fraction).
    const GrainParams off;  // default-constructed: enabled == false
    check(grainCoverageAt(off, 0.0f, 12345, 6789) == 0.0f &&
              grainCoverageAt(off, 1.0f, 12345, 6789) == 1.0f &&
              grainCoverageAt(off, 0.37f, 12345, 6789) == 0.37f,
          "grain/off: grainCoverageAt() returns coverage bit-identical when disabled");

    // The whole deposit path: a dab through depositDab() with `tip.grain`
    // left at its default (off) must match a texel computed via the
    // PRE-GRAIN formula directly -- `dabCoverage()` and `depositTexel()`
    // with no `grainCoverageAt()` anywhere in the computation at all.
    //
    // Mass is compared at the f16 STORAGE bound, not at zero -- `want` is
    // computed in float32 and `got` is read back through `PigmentTile`'s
    // binary16 channels (runPigmentDepositTest()'s own `kHalfRel`/
    // `kHalfFloor` derivation, restated), which rounds regardless of grain.
    // That storage rounding is not what this section is testing; the bit-
    // exact claim -- no floating-point operation added at all when grain is
    // off -- is what the standalone `grainCoverageAt()` check just above
    // already proved, at true zero tolerance, before any store was involved.
    // Latent IS compared at zero tolerance: `yellow`'s channels (0 and 1)
    // are exact in binary16, so a first deposit onto empty paper (`w == 1`
    // exactly, brush/Deposit.hpp §1(i)) round-trips with no rounding at all.
    BrushTip tip;
    tip.radius = 30.0f;
    tip.hardness = 0.4f;
    tip.flow = 0.6f;
    tip.pigment = yellow;
    // tip.grain left default -- OFF.

    PigmentTileStore store;
    const Vec2 centre{60.0f, 60.0f};
    const DepositCount count = depositDab(store, tip, centre, 120, 120, nullptr, nullptr);
    check(count.texels > 0, "grain/off: the reference dab actually wrote texels");

    bool allMatch = true;
    size_t compared = 0;
    for (int32_t y = 40; y < 80 && allMatch; ++y) {
      for (int32_t x = 40; x < 80 && allMatch; ++x) {
        const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        const float cov = dabCoverage(tip, dx, dy);  // NO grainCoverageAt() call at all
        PigmentTexel want{};
        if (cov > 0.0f) want = depositTexel(PigmentTexel{}, tip.pigment, tip.flow * cov);
        const PixelCoord at{x, y};
        const PigmentTile* t = store.find(tileCoordAt(at));
        const PigmentTexel got = t != nullptr ? t->readTexel(tileLocalOffset(at)) : PigmentTexel{};
        ++compared;
        if (!nearHalf(got.mass, want.mass) || got.latent.c != want.latent.c) allMatch = false;
      }
    }
    check(compared == 40 * 40, "grain/off: compared every texel of the swept region");
    check(allMatch,
          "grain/off: depositDab() with grain off == the pre-grain formula (mass at the f16 "
          "bound, latent exact)");
  }

  // ======================================================================
  std::printf("  -- E. app/DabPreview agrees with a real depositDab(), grain ON --\n");
  // ======================================================================
  {
    // Radius <= kDabPreviewFitRadius (30) so dabPreviewScale() returns
    // exactly 1.0 -- one preview texel is one document pixel, the same
    // precondition runDabPreviewTest()'s own section A states for the
    // identical reason.
    BrushTip tip;
    tip.radius = 24.0f;
    tip.hardness = 0.35f;
    tip.flow = 0.5f;
    tip.pigment = yellow;
    tip.grain.enabled = true;
    tip.grain.periodX = 24;
    tip.grain.periodY = 24;
    tip.grain.depth = 0.4f;
    tip.grain.strength = 1.0f;

    check(dabPreviewScale(tip.radius) == 1.0f,
          "grain/preview-parity: a 24 px tip previews 1:1");

    // The identical geometry runDabPreviewTest() section A builds: a canvas
    // exactly kDabPreviewCell square, the dab centred at (32, 32) -- the same
    // point `dabPreviewOffset(scale=1, cell=0, ...)` centres cell 0 on. That
    // is what makes the deposit's absolute loop coordinate `(x, y)` and the
    // preview's absolute cell coordinate `(px, py)` the IDENTICAL integer for
    // every texel, which is the precondition brush/Deposit.hpp §2e's own
    // comment states for this comparison to mean anything.
    PigmentTileStore store;
    const DepositCount count = depositDab(store, tip, Vec2{32.0f, 32.0f}, kDabPreviewCell,
                                          kDabPreviewCell, nullptr, nullptr);
    check(count.texels > 0, "grain/preview-parity: the reference deposit actually wrote texels");

    // A grain-OFF deposit over the identical footprint, to prove this test
    // is not vacuously passing because grain happened to change nothing --
    // brush/Grain.hpp's own §GrainParams::depth comment sizes `depth` so an
    // ordinary soft tip's skirt does lose texels to a deep-enough valley.
    BrushTip ungrained = tip;
    ungrained.grain.enabled = false;
    PigmentTileStore refStore;
    const DepositCount refCount = depositDab(refStore, ungrained, Vec2{32.0f, 32.0f},
                                             kDabPreviewCell, kDabPreviewCell, nullptr, nullptr);
    check(count.texels < refCount.texels,
          "grain/preview-parity: grain actually skipped texels this dab would otherwise paint "
          "(not a vacuous comparison)");

    size_t compared = 0;
    size_t footprintMismatch = 0;
    size_t massMismatch = 0;
    size_t latentMismatch = 0;
    for (int y = 0; y < kDabPreviewCell; ++y) {
      for (int x = 0; x < kDabPreviewCell; ++x) {
        const PigmentTexel got = dabPreviewTexel(tip, 1.0f, /*cell=*/0, x, y);
        const PixelCoord at{x, y};
        const PigmentTile* t = store.find(tileCoordAt(at));
        const PigmentTexel want =
            t != nullptr ? t->readTexel(tileLocalOffset(at)) : PigmentTexel{};
        ++compared;
        if ((got.mass > 0.0f) != (want.mass > 0.0f)) ++footprintMismatch;
        if (!nearHalf(got.mass, want.mass)) ++massMismatch;
        if (got.mass > 0.0f && want.mass > 0.0f && got.latent.c != want.latent.c)
          ++latentMismatch;
      }
    }
    check(compared == kDabPreviewCell * kDabPreviewCell,
          "grain/preview-parity: compared every texel of the preview cell");
    check(footprintMismatch == 0,
          "grain/preview-parity: footprint (painted vs bare) agrees everywhere, grain ON");
    check(massMismatch == 0,
          "grain/preview-parity: mass agrees within the f16 storage bound, grain ON");
    check(latentMismatch == 0, "grain/preview-parity: latent agrees exactly, grain ON");
  }

  // ======================================================================
  std::printf("  -- F. paper, not wallpaper: absolute position, not dab-local offset --\n");
  // ======================================================================
  {
    GrainParams p;
    p.enabled = true;
    p.periodX = 24;
    p.periodY = 24;
    p.depth = 0.4f;
    p.strength = 1.2f;

    // Two absolute texels that share the SAME (dx, dy) offset from two
    // different dab centres 13 px apart (not a multiple of the 24 px
    // period), so a field keyed on the RELATIVE offset would hash the
    // identical input at both -- this is precisely what "a grain that moved
    // with the brush would be wallpaper" (brush/Grain.hpp §3) means, made
    // concrete as two integer coordinates.
    constexpr int32_t kOffsetX = 5;
    const int32_t ax = 80, ay = 80;
    const int32_t bx = 93, by = 80;  // 80 + 13
    check((bx - ax) % p.periodX != 0,
          "grain/absolute: the two centres are NOT a whole period apart (a real test, not a "
          "coincidental match)");

    const float gA = grainHeightAt(p, ax + kOffsetX, ay);
    const float gB = grainHeightAt(p, bx + kOffsetX, by);
    check(gA != gB,
          "grain/absolute: the SAME relative offset gets DIFFERENT grain at two absolute "
          "positions");

    // End to end: paint the identical tip at the two centres and confirm the
    // deposited MASS differs at that same relative offset -- proving the
    // effect reaches the paint, not only the standalone field function.
    BrushTip tip;
    tip.radius = 30.0f;
    tip.hardness = 0.4f;
    tip.flow = 0.6f;
    tip.pigment = yellow;
    tip.grain = p;

    PigmentTileStore storeA, storeB;
    depositDab(storeA, tip, Vec2{static_cast<float>(ax) + 0.5f, static_cast<float>(ay) + 0.5f},
              200, 200, nullptr, nullptr);
    depositDab(storeB, tip, Vec2{static_cast<float>(bx) + 0.5f, static_cast<float>(by) + 0.5f},
              200, 200, nullptr, nullptr);
    const auto readAt = [](const PigmentTileStore& store, int32_t x, int32_t y) -> PigmentTexel {
      const PixelCoord at{x, y};
      const PigmentTile* t = store.find(tileCoordAt(at));
      return t != nullptr ? t->readTexel(tileLocalOffset(at)) : PigmentTexel{};
    };
    const float massA = readAt(storeA, ax + kOffsetX, ay).mass;
    const float massB = readAt(storeB, bx + kOffsetX, by).mass;
    check(massA != massB,
          "grain/absolute: end to end, two strokes at different document positions deposit "
          "DIFFERENT mass at the identical relative offset");
    std::printf("    [measured] G(A)=%.4f G(B)=%.4f | mass(A)=%.4f mass(B)=%.4f\n",
                static_cast<double>(gA), static_cast<double>(gB), static_cast<double>(massA),
                static_cast<double>(massB));
  }

  // ======================================================================
  std::printf("  -- G. default and equality: what a brand-new BrushTip/BrushState carries --\n");
  // ======================================================================
  {
    check(!BrushTip{}.grain.enabled, "grain/default: a default-constructed BrushTip has grain OFF");
    check(!GrainParams{}.enabled, "grain/default: a default-constructed GrainParams is OFF");
    GrainParams a, b;
    check(grainParamsEqual(a, b), "grain/equal: two defaults compare equal");
    b.enabled = true;
    check(!grainParamsEqual(a, b), "grain/equal: enabled alone makes two GrainParams differ");
  }

  // ======================================================================
  std::printf("  -- H. the EDITED badge: presetMatches() sees a grain-only edit --\n");
  // ======================================================================
  {
    // Unlike `tipBitmap`/`dualTip` (brush/Library.hpp's own documented blind
    // spot for those two), `grain` HAS its own control -- the BRUSH EDITOR's
    // PAPER GRAIN section -- so it must be one of the fields `presetMatches()`
    // actually compares. Proven here rather than only asserted in that
    // function's own header comment: a preset and a "brush" that agree on
    // every OTHER field but grain must read as edited, not as matching.
    BrushPreset preset;  // every field at BrushPreset's own defaults
    // `preset.radius`/`hardness`/`spacing`/`roundness`/`angle` no longer
    // exist (Part 5 deleted the shadow scalars) -- `presetMatches()` now
    // compares projections of `preset.model.tip`, so a "compare a preset
    // against itself" fixture reads them from there instead.
    const bool matchesWhenIdentical =
        presetMatches(preset, preset.model.tip.diameterPx / 2.0f, preset.model.tip.hardness,
                     preset.model.tip.spacingPercent / 100.0f, preset.model.tip.roundness,
                     preset.model.tip.angleDeg, preset.load, preset.wetness, preset.links,
                     preset.grain);
    check(matchesWhenIdentical,
          "grain/edited-badge: identical grain (and everything else) reads as matching");

    GrainParams turnedOn = preset.grain;
    turnedOn.enabled = true;
    const bool matchesWhenGrainDiffers =
        presetMatches(preset, preset.model.tip.diameterPx / 2.0f, preset.model.tip.hardness,
                     preset.model.tip.spacingPercent / 100.0f, preset.model.tip.roundness,
                     preset.model.tip.angleDeg, preset.load, preset.wetness, preset.links,
                     turnedOn);
    check(!matchesWhenGrainDiffers,
          "grain/edited-badge: turning grain on, with every other field unchanged, reads as "
          "edited");
  }

  return ok;
}

}  // namespace np
