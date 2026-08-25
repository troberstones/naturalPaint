#include "app/selftest/Support.hpp"

#include "app/DabPreview.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "ui/AtelierTheme.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The BRUSH EDITOR's tip preview (app/DabPreview): **one dab, rasterised,
// exactly as the deposit will make it.**
//
// See app/DabPreview.hpp for every decision -- three pressures, one shared
// scale, the loaded colour over paper -- and brush/Deposit.hpp §2b for the
// elliptical tip this preview is the reason for. Two things are worth saying
// here, because they are what the section is *for*:
//
//   * **Section A is the whole point of the track.** A preview is only worth
//     drawing if it is the mark the brush makes, and "it calls the same
//     function" is a claim about source code that a refactor can quietly
//     falsify. So section A does not check that the preview calls
//     `dabCoverage()`; it runs a **real `depositDab()` into a real
//     `PigmentTileStore`** and compares the deposited tile against the
//     preview, texel by texel, over the whole footprint. A preview that grew
//     its own falloff would have to reproduce the deposit's to the last bit to
//     survive it, at which point it would not be a second falloff.
//
//   * **Section H's cache assertions are paired, always.** A cache that never
//     invalidates passes every count check written against a cache that is
//     asked once, and it passes every "did it redraw" check written without a
//     count. Each mutation below asserts BOTH that the rasterisation count
//     moved AND that the bytes handed back are different ones -- and the
//     converse, that a slider the image does not depend on moves neither.
// ---------------------------------------------------------------------------
bool runDabPreviewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerances ---------------------------------------------------------
  //
  // **Almost everything here is at exactly zero**, and that is not bravado: a
  // preview texel and a deposited texel are produced by the *same* three
  // function calls on the *same* float offsets (app/DabPreview.cpp's
  // `dabPreviewTexel()` is `depositDab()`'s inner loop verbatim), so any
  // difference at all is a difference in what the two compute, never a
  // difference in accuracy.
  //
  // The one exception is section A's comparison against a **stored** texel.
  // `PigmentTile` holds seven binary16 channels (core/Pigment.hpp), so the
  // deposit's float mass is rounded once on its way into the tile and the
  // preview's is not. binary16's 11-bit significand gives a round-to-nearest
  // relative error of at most 2^-11, plus an absolute floor of half a
  // subnormal ulp, 2^-25 -- the identical derivation runPigmentDepositTest()
  // and runRgbDepositTest() each state for the identical storage, restated
  // rather than shared because a tolerance borrowed without its derivation is
  // the one that later gets applied where it does not hold.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };

  // A latent with no LUT in sight: Mixbox's own yellow primary, exactly as
  // `--pigment-stroke-demo` builds one. `latentToRgb()` needs no LUT
  // (core/Pigment.hpp's opening), so nothing in this section depends on the
  // 512x512 PNG having loaded.
  Latent yellow;
  yellow.c = {0.0f, 1.0f, 0.0f};

  auto previewTip = [&](float radius, float hardness, float flow, float roundness,
                        float angle) {
    BrushTip t;
    t.radius = radius;
    t.hardness = hardness;
    t.flow = flow;
    t.roundness = roundness;
    t.angle = angle;
    t.pigment = yellow;
    return t;
  };
  auto sameTipThrice = [](const BrushTip& t) {
    return std::array<BrushTip, kDabPreviewCells>{t, t, t};
  };

  // ======================================================================
  std::printf("  -- A. the preview's dab IS the deposit's dab --\n");
  // ======================================================================
  //
  // Radius 24 so `dabPreviewScale()` returns exactly 1.0 (the fit radius is
  // 30) and one preview texel is one document pixel -- which is what makes a
  // texel-for-texel comparison against a real deposit meaningful at all. At
  // any other scale the two are sampling the same continuous dab at different
  // points, and "they agree" would need a resampling argument this section
  // deliberately does not want to have.
  {
    const BrushTip tip = previewTip(24.0f, 0.35f, 0.5f, 1.0f, 0.0f);
    check(dabPreviewScale(tip.radius) == 1.0f,
          "a 24 px tip previews 1:1, so a texel is a document pixel");

    // The dab centre sits on the boundary between texel 31 and texel 32 of the
    // cell, which is `dabPreviewOffset()`'s convention; the deposit is given
    // the identical centre so the two sample the identical points.
    PigmentTileStore store;
    const DepositCount count =
        depositDab(store, tip, Vec2{32.0f, 32.0f}, kDabPreviewCell, kDabPreviewCell, nullptr);
    check(count.texels > 0, "the reference deposit actually wrote texels");

    size_t compared = 0;
    size_t coverageMismatch = 0;
    size_t footprintMismatch = 0;
    size_t massMismatch = 0;
    size_t latentMismatch = 0;
    float worstMass = 0.0f;
    for (int y = 0; y < kDabPreviewCell; ++y) {
      for (int x = 0; x < kDabPreviewCell; ++x) {
        const DabPreviewOffset o = dabPreviewOffset(1.0f, 0, x, y);
        // 1. The mapping has no private copy of the falloff.
        if (dabPreviewCoverageAt(tip, 1.0f, 0, x, y) != dabCoverage(tip, o.dx, o.dy))
          ++coverageMismatch;

        const PigmentTexel got = dabPreviewTexel(tip, 1.0f, 0, x, y);
        const PixelCoord at{x, y};
        const PigmentTile* tile = store.find(tileCoordAt(at));
        const PigmentTexel want =
            tile != nullptr ? tile->readTexel(tileLocalOffset(at)) : PigmentTexel{};

        // 2. The FOOTPRINT agrees exactly: a texel the deposit left alone is a
        //    texel the preview draws as bare paper, and conversely. This is
        //    the half that catches an offset-by-one, which a mass comparison
        //    under a tolerance would let through at the rim where both are
        //    nearly zero.
        if ((got.mass > 0.0f) != (want.mass > 0.0f)) ++footprintMismatch;

        if (want.mass > 0.0f) {
          ++compared;
          if (!nearHalf(got.mass, want.mass)) ++massMismatch;
          worstMass = std::max(worstMass, std::fabs(got.mass - want.mass));
          // The latent is the brush's, unrounded on the preview side and
          // rounded once into the tile. Compared under the same binary16
          // bound, per component.
          for (int i = 0; i < 3; ++i)
            if (!nearHalf(got.latent.c[static_cast<size_t>(i)],
                          want.latent.c[static_cast<size_t>(i)]))
              ++latentMismatch;
        }
      }
    }
    check(coverageMismatch == 0,
          "preview coverage == dabCoverage() at every texel, zero tol");
    check(footprintMismatch == 0,
          "preview footprint == the deposited footprint, exactly");
    check(compared == count.texels,
          "and the compared set is the whole deposit, not a subset");
    check(massMismatch == 0, "preview mass == deposited mass within f16 storage");
    check(latentMismatch == 0, "preview latent == deposited latent within f16");
    std::printf("    worst mass difference over %zu texels: %.3e (bound %.3e) [measured]\n",
                compared, static_cast<double>(worstMass),
                static_cast<double>(kHalfRel + kHalfFloor));

    // The rejected alternative, run beside the built one: a preview that owned
    // its falloff. A linear ramp is the obvious hand-rolled one, and it is the
    // one brush/Deposit.hpp §2 explicitly rejected -- so this is what the
    // picture would have been wrong by, in the only unit that does not vary.
    float worstIfLinear = 0.0f;
    for (int x = 0; x < kDabPreviewCell; ++x) {
      const DabPreviewOffset o = dabPreviewOffset(1.0f, 0, x, kDabPreviewCell / 2);
      const float d = std::sqrt(o.dx * o.dx + o.dy * o.dy) / tip.radius;
      const float linear = d >= 1.0f ? 0.0f
                           : d <= tip.hardness
                               ? 1.0f
                               : 1.0f - (d - tip.hardness) / (1.0f - tip.hardness);
      worstIfLinear =
          std::max(worstIfLinear, std::fabs(linear - dabPreviewCoverageAt(tip, 1.0f, 0, x,
                                                                          kDabPreviewCell / 2)));
    }
    check(worstIfLinear > 0.05f,
          "a hand-rolled linear ramp would visibly disagree -- so this matters");
    std::printf("    a linear-ramp preview would differ by up to %.3f coverage\n",
                static_cast<double>(worstIfLinear));
  }

  // ======================================================================
  std::printf("  -- B. hardness changes the ramp, and by how much --\n");
  // ======================================================================
  //
  // Sampled at cell texel (44, 32), which is 12.5 px right of and 0.5 px below
  // the dab centre -- a distance of 12.510 px, i.e. **0.5212 of a 24 px
  // radius**, deliberately mid-ramp rather than near either end.
  //
  // The two answers are analytic, not measured:
  //   * hardness 0.90 -- 0.5212 is inside the flat core (0.90 of the radius),
  //     so coverage is exactly 1.
  //   * hardness 0.00 -- a pure smoothstep, u = 0.5212, so coverage is
  //     1 - u^2(3 - 2u) = 0.4682.
  // The difference is therefore **0.5318**, and the bound asserted is 0.45:
  // 15% below the derived value, which is orders of magnitude more slack than
  // any float wobble in a smoothstep needs and still enormously above the 0.0
  // a preview that ignored hardness would produce. This is the setting the bar
  // it replaces could not show at all.
  {
    const BrushTip hard = previewTip(24.0f, 0.90f, 1.0f, 1.0f, 0.0f);
    const BrushTip soft = previewTip(24.0f, 0.00f, 1.0f, 1.0f, 0.0f);
    const float ch = dabPreviewCoverageAt(hard, 1.0f, 0, 44, 32);
    const float cs = dabPreviewCoverageAt(soft, 1.0f, 0, 44, 32);
    check(ch == 1.0f, "at 0.52 r a hardness-0.90 tip is still in its flat core");
    check(cs > 0.40f && cs < 0.55f, "and a hardness-0.00 tip is mid-smoothstep there");
    check(ch - cs >= 0.45f, "hardness moves the ramp by >= 0.45 coverage there");
    std::printf("    hard %.4f, soft %.4f, difference %.4f (derived 0.5318)\n",
                static_cast<double>(ch), static_cast<double>(cs),
                static_cast<double>(ch - cs));
  }

  // ======================================================================
  std::printf("  -- C. roundness narrows the tip and angle turns it --\n");
  // ======================================================================
  //
  // **Only one of the brief's two directions is possible, and the reason is
  // structural rather than an omission.** brush/Deposit.hpp §2b keeps `radius`
  // as the semi-MAJOR axis, so an elliptical tip is *inscribed* in the round
  // tip of the same radius and can never cover a point the round one misses.
  // The pair that does bite in both directions is one ellipse against the
  // SAME ellipse rotated: at 0 deg it covers a point 90 deg does not, and at
  // 90 deg it covers a point 0 deg does not. A preview that ignored `angle`
  // would answer identically to both and fails two assertions, not one.
  //
  // Sample points, in cell texels, both 20.5 px from the dab centre on their
  // axis and both well inside a 24 px round tip:
  //   * `east`  = (52, 32) -- +20.5 px in x
  //   * `south` = (32, 52) -- +20.5 px in y
  // The minor semi-axis at roundness 0.30 is 7.2 px, so the off-axis point is
  // outside by nearly 3x -- not a boundary case a rounding could flip.
  {
    const BrushTip round = previewTip(24.0f, 0.35f, 1.0f, 1.0f, 0.0f);
    const BrushTip flat0 = previewTip(24.0f, 0.35f, 1.0f, 0.30f, 0.0f);
    const BrushTip flat90 = previewTip(24.0f, 0.35f, 1.0f, 0.30f, 90.0f);
    const BrushTip flat45 = previewTip(24.0f, 0.35f, 1.0f, 0.30f, 45.0f);
    auto cov = [&](const BrushTip& t, int x, int y) {
      return dabPreviewCoverageAt(t, 1.0f, 0, x, y);
    };

    check(cov(round, 52, 32) > 0.0f && cov(round, 32, 52) > 0.0f,
          "a round tip covers east and south alike");
    check(cov(flat0, 52, 32) > 0.0f, "a flat tip at 0 deg still covers east");
    check(cov(flat0, 32, 52) == 0.0f,
          "roundness 0.30 puts south outside the tip entirely");
    check(cov(flat90, 32, 52) > 0.0f, "the same tip at 90 deg covers south");
    check(cov(flat90, 52, 32) == 0.0f, "and no longer covers east -- angle turned it");
    // The diagonal, which no axis-aligned mistake can get right by accident:
    // at 45 deg the major axis runs down-right (y is down), so a point on that
    // diagonal is covered and its mirror across x is not.
    check(cov(flat45, 46, 46) > 0.0f, "at 45 deg the down-right diagonal is covered");
    check(cov(flat45, 46, 17) == 0.0f, "and the up-right diagonal is not");

    // The tip's rim is where §2b says it is: 24 px along the major axis, and
    // 0.30 x 24 = 7.2 px along the minor. Sampled just inside and just outside
    // each, so a preview that used `radius` for BOTH axes -- the plausible
    // mistake -- fails here.
    check(cov(flat0, 32 + 7, 32) > 0.0f && cov(flat0, 32, 32 + 7) == 0.0f,
          "the minor semi-axis is roundness x radius, not radius");
    check(dabPreviewTexel(flat0, 1.0f, 0, 52, 32).mass > 0.0f &&
              dabPreviewTexel(flat0, 1.0f, 0, 32, 52).mass == 0.0f,
          "and the rasterised texel agrees with the coverage");
  }

  // ======================================================================
  std::printf("  -- D. a round tip is centred and exactly symmetric --\n");
  // ======================================================================
  //
  // Exact equality, not a tolerance. The dab centre sits on a texel boundary,
  // so texel `p` and texel `63 - p` are at offsets that are exact float
  // negations of each other, and `dabCoverage()` squares them -- so two
  // mirrored samples must be the identical float or something is off by half
  // a texel. A preview centred half a texel wrong looks fine and is wrong.
  {
    const BrushTip round = previewTip(24.0f, 0.35f, 1.0f, 1.0f, 0.0f);
    size_t asymmetric = 0;
    size_t nonZero = 0;
    for (int y = 0; y < kDabPreviewCell; ++y) {
      for (int x = 0; x < kDabPreviewCell; ++x) {
        const float c = dabPreviewCoverageAt(round, 1.0f, 0, x, y);
        if (c > 0.0f) ++nonZero;
        if (c != dabPreviewCoverageAt(round, 1.0f, 0, kDabPreviewCell - 1 - x, y)) ++asymmetric;
        if (c != dabPreviewCoverageAt(round, 1.0f, 0, x, kDabPreviewCell - 1 - y)) ++asymmetric;
      }
    }
    check(nonZero > 0, "the round tip covers something at all");
    check(asymmetric == 0, "every mirrored pair matches at zero tolerance");

    // ...and the check is not vacuous, because a rotated ellipse breaks it.
    const BrushTip flat45 = previewTip(24.0f, 0.35f, 1.0f, 0.30f, 45.0f);
    size_t brokenMirrors = 0;
    for (int y = 0; y < kDabPreviewCell; ++y)
      for (int x = 0; x < kDabPreviewCell; ++x)
        if (dabPreviewCoverageAt(flat45, 1.0f, 0, x, y) !=
            dabPreviewCoverageAt(flat45, 1.0f, 0, kDabPreviewCell - 1 - x, y))
          ++brokenMirrors;
    check(brokenMirrors > 0,
          "a 45 deg tip is NOT left-right symmetric -- the mirror test bites");
  }

  // ======================================================================
  std::printf("  -- E. radius maps to the preview's extent --\n");
  // ======================================================================
  //
  // The assertion a fit-to-box preview cannot survive. Scanning row 32 (0.5 px
  // below the centre) for the rightmost covered texel:
  //
  //   radius  10  -> dx < sqrt(100 - 0.25) = 9.987, so texel 41 (dx 9.5)
  //   radius  24  -> dx < sqrt(576 - 0.25) = 23.995, so texel 55 (dx 23.5)
  //   radius 200  -> scale 200/30, so the rim lands at 29.996 TEXELS: texel 61
  //
  // The nearest rejected texel is a full pixel further out in each case, so
  // none of the three is a boundary a rounding could move. The third is the
  // fit path (§3), and 61 rather than 63 is `kDabPreviewFitRadius`'s two-texel
  // margin arriving as a number: the widest brush in the application still
  // does not touch its cell's edge.
  {
    auto rightmost = [&](float radius) {
      const std::array<BrushTip, kDabPreviewCells> tips =
          sameTipThrice(previewTip(radius, 0.35f, 1.0f, 1.0f, 0.0f));
      const float scale = dabPreviewScale(radius);
      int last = -1;
      for (int x = 0; x < kDabPreviewCell; ++x)
        if (dabPreviewCoverageAt(tips[0], scale, 0, x, kDabPreviewCell / 2) > 0.0f) last = x;
      return last;
    };
    const int r10 = rightmost(10.0f);
    const int r24 = rightmost(24.0f);
    const int r200 = rightmost(200.0f);
    check(r10 == 41, "a 10 px tip reaches texel 41 and stops");
    check(r24 == 55, "a 24 px tip reaches texel 55 -- 14 texels further");
    check(r10 != r24, "so the radius slider moves the mark, which is the point");
    check(r200 == 61, "and a 200 px tip is minified to fit, still short of the edge");
    check(r200 < kDabPreviewCell - 1, "no radius fills the cell edge to edge");
    std::printf("    rightmost covered texel: r10 %d, r24 %d, r200 %d (scale 1:%.2f)\n", r10,
                r24, r200, static_cast<double>(dabPreviewScale(200.0f)));

    // One scale for all three cells (§3), so the pressure cells stay
    // comparable. Built from three DIFFERENT radii, as a PRESSURE -> SIZE link
    // produces, and asserted to be the largest one's.
    std::array<BrushTip, kDabPreviewCells> mixed{previewTip(8.0f, 0.35f, 1.0f, 1.0f, 0.0f),
                                                 previewTip(40.0f, 0.35f, 1.0f, 1.0f, 0.0f),
                                                 previewTip(90.0f, 0.35f, 1.0f, 1.0f, 0.0f)};
    const DabPreviewImage img = rasteriseDabPreview(mixed);
    check(img.scale == dabPreviewScale(90.0f),
          "the scale comes from the LARGEST of the three tips");
    check(img.width == kDabPreviewWidth && img.height == kDabPreviewHeight,
          "the image is 3 x 64 px wide and 64 px tall, as 4a specifies");
  }

  // ======================================================================
  std::printf("  -- F. mass, not coverage: a loaded brush saturates --\n");
  // ======================================================================
  //
  // The argument for drawing the dab in the loaded colour rather than as a
  // grey coverage ramp (app/DabPreview.hpp §4). `depositTexel()` caps mass at
  // `kMaxMass`, so a flow of 2.0 lays a FLAT core where a flow of 0.5 lays a
  // ramp -- two brushes that differ in nothing a slider readout can show.
  //
  // Texels (32,32) and (42,32) have coverage 0.997 and 0.593 respectively at
  // radius 24 / hardness 0. At flow 2.0 both exceed the cap and store mass
  // exactly 1; at flow 0.5 they store 0.499 and 0.296.
  {
    const BrushTip loaded = previewTip(24.0f, 0.0f, 2.0f, 1.0f, 0.0f);
    const BrushTip thin = previewTip(24.0f, 0.0f, 0.5f, 1.0f, 0.0f);
    check(dabPreviewTexel(loaded, 1.0f, 0, 32, 32).mass == kMaxMass &&
              dabPreviewTexel(loaded, 1.0f, 0, 42, 32).mass == kMaxMass,
          "at flow 2.0 both samples are capped at kMaxMass");
    check(dabPreviewTexel(thin, 1.0f, 0, 32, 32).mass >
              dabPreviewTexel(thin, 1.0f, 0, 42, 32).mass,
          "at flow 0.5 the same two samples still differ");

    const DabPreviewImage flat = rasteriseDabPreview(sameTipThrice(loaded));
    const DabPreviewImage ramp = rasteriseDabPreview(sameTipThrice(thin));
    auto texelAt = [](const DabPreviewImage& img, int x, int y) {
      const size_t base =
          (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4u;
      return std::array<uint8_t, 4>{img.rgba[base], img.rgba[base + 1], img.rgba[base + 2],
                                    img.rgba[base + 3]};
    };
    check(texelAt(flat, 32, 32) == texelAt(flat, 42, 32),
          "so the saturated core rasterises FLAT, in bytes");
    check(texelAt(ramp, 32, 32) != texelAt(ramp, 42, 32),
          "and the unsaturated one does not -- visible only in colour");
  }

  // ======================================================================
  std::printf("  -- G. the ground is the design's paper, not the panel --\n");
  // ======================================================================
  //
  // app/DabPreview.cpp spells `ui/AtelierTheme.hpp`'s `kCanvasPaper` out as
  // three floats because `app/` does not include `ui/`. This is the guard that
  // duplicated constant needs: a theme revision that moved the token and not
  // the copy would leave the preview a shade off forever, and nothing else in
  // the build would notice.
  //
  // A one-byte tolerance, and it is a real one rather than slack: the paper
  // goes sRGB byte -> linear -> sRGB byte -> quantise, so one round trip
  // through a piecewise transfer function and one rounding stand between the
  // token and the pixel. A token that actually MOVED would move by far more
  // than one code.
  {
    const BrushTip tip = previewTip(24.0f, 0.35f, 1.0f, 1.0f, 0.0f);
    const DabPreviewImage img = rasteriseDabPreview(sameTipThrice(tip));
    const int paperR = static_cast<int>((kCanvasPaper >> 16) & 0xffu);
    const int paperG = static_cast<int>((kCanvasPaper >> 8) & 0xffu);
    const int paperB = static_cast<int>(kCanvasPaper & 0xffu);
    // Texel (0,0) is 31.5 px up and left of a 24 px dab, so it is bare ground.
    check(std::abs(static_cast<int>(img.rgba[0]) - paperR) <= 1 &&
              std::abs(static_cast<int>(img.rgba[1]) - paperG) <= 1 &&
              std::abs(static_cast<int>(img.rgba[2]) - paperB) <= 1,
          "an uncovered texel is kCanvasPaper, within one code");
    check(img.rgba[3] == 255u, "and the image is opaque -- it is paper, not a hole");
    std::printf("    ground %3u %3u %3u vs kCanvasPaper %3d %3d %3d\n", img.rgba[0],
                img.rgba[1], img.rgba[2], paperR, paperG, paperB);

    const size_t centre = (static_cast<size_t>(32) * static_cast<size_t>(kDabPreviewWidth) +
                           static_cast<size_t>(32)) *
                          4u;
    check(img.rgba[centre] != img.rgba[0] || img.rgba[centre + 1] != img.rgba[1] ||
              img.rgba[centre + 2] != img.rgba[2],
          "and the dab's centre is NOT paper -- paint was drawn");
  }

  // ======================================================================
  std::printf("  -- H. the cache refreshes, and only when it must --\n");
  // ======================================================================
  //
  // Every mutation below is a PAIR: the count moved AND the bytes are
  // different ones. Either alone is worthless. A cache that never invalidates
  // passes the second half's converse (bytes unchanged) trivially, and a cache
  // that rasterises every frame passes nothing here at all.
  {
    DabPreviewCache cache;
    const BrushTip base = previewTip(24.0f, 0.35f, 0.8f, 1.0f, 0.0f);
    const std::vector<uint8_t> first = cache.imageFor(sameTipThrice(base)).rgba;
    check(cache.rasterisations() == 1 && cache.hits() == 0,
          "the first ask rasterises; there was nothing to hit");
    const uint64_t gen1 = cache.generation();

    const std::vector<uint8_t> again = cache.imageFor(sameTipThrice(base)).rgba;
    check(cache.rasterisations() == 1 && cache.hits() == 1,
          "asking again with the same tip does not rasterise again");
    check(again == first && cache.generation() == gen1,
          "and hands back the same bytes and the same generation");

    auto mutated = [&](const char* what, const BrushTip& tip, uint64_t wantCount) {
      const std::vector<uint8_t> got = cache.imageFor(sameTipThrice(tip)).rgba;
      check(cache.rasterisations() == wantCount, what);
      check(got != first, "  ...and the image handed back actually changed");
      return got;
    };
    mutated("moving RADIUS re-rasterises", previewTip(30.0f, 0.35f, 0.8f, 1.0f, 0.0f), 2);
    mutated("moving HARDNESS re-rasterises", previewTip(24.0f, 0.90f, 0.8f, 1.0f, 0.0f), 3);
    mutated("moving ROUNDNESS re-rasterises", previewTip(24.0f, 0.35f, 0.8f, 0.4f, 0.0f), 4);
    mutated("moving FLOW re-rasterises", previewTip(24.0f, 0.35f, 1.6f, 1.0f, 0.0f), 5);
    // Angle on an ALREADY elliptical tip: on a round one it is a no-op by
    // construction (brush/Deposit.hpp §2b skips the rotation), so asserting a
    // changed image for a rotated circle would be asserting a bug.
    cache.imageFor(sameTipThrice(previewTip(24.0f, 0.35f, 0.8f, 0.4f, 0.0f)));
    const std::vector<uint8_t> at0 = cache.imageFor(sameTipThrice(
                                                       previewTip(24.0f, 0.35f, 0.8f, 0.4f, 0.0f)))
                                         .rgba;
    const uint64_t beforeAngle = cache.rasterisations();
    const std::vector<uint8_t> at60 =
        cache.imageFor(sameTipThrice(previewTip(24.0f, 0.35f, 0.8f, 0.4f, 60.0f))).rgba;
    check(cache.rasterisations() == beforeAngle + 1, "moving ANGLE re-rasterises");
    check(at60 != at0, "  ...and turns the tip in the image");

    // The converse, which is what keys the cache on exactly the fields the
    // image depends on: SPACING is a slider in the same TIP block and a
    // one-dab preview cannot show it, so moving it must NOT cost 12 288 texels.
    BrushTip spaced = previewTip(24.0f, 0.35f, 0.8f, 0.4f, 60.0f);
    spaced.spacing = 0.93f;
    spaced.opacity = 0.31f;
    const uint64_t beforeSpacing = cache.rasterisations();
    const std::vector<uint8_t> spacedImage = cache.imageFor(sameTipThrice(spaced)).rgba;
    check(cache.rasterisations() == beforeSpacing,
          "moving SPACING or OPACITY does not re-rasterise anything");
    check(spacedImage == at60, "  ...because the image cannot depend on either");

    // What the panel actually costs when it DOES have to redraw. Printed
    // rather than asserted -- it is a machine-dependent number, and the
    // assertion that matters is the cache's, above.
    const auto started = std::chrono::steady_clock::now();
    constexpr int kRuns = 200;
    for (int i = 0; i < kRuns; ++i) {
      BrushTip t = previewTip(20.0f + static_cast<float>(i) * 0.01f, 0.35f, 0.8f, 0.4f, 30.0f);
      cache.imageFor(sameTipThrice(t));
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    std::printf("    one rasterisation of %d texels: %.3f ms [measured]\n",
                kDabPreviewWidth * kDabPreviewHeight, ms / kRuns);
    std::printf("    cache: %llu rasterisation(s), %llu hit(s) [measured]\n",
                static_cast<unsigned long long>(cache.rasterisations()),
                static_cast<unsigned long long>(cache.hits()));
  }

  // ======================================================================
  std::printf("  -- I. the three cells are brushTipFor()'s, not the sliders' --\n");
  // ======================================================================
  //
  // A preview built from `BrushState::radius` directly would draw the same
  // three dabs for a brush with a PRESSURE -> SIZE link and one without, which
  // is the single thing app/DabPreview.hpp §2 says three cells exist to show.
  {
    MixboxLut noLut;
    BrushState brush;  // the default: defaultBrushLinks(), so PRESSURE -> SIZE
    const DynamicInputs live;
    const std::array<BrushTip, kDabPreviewCells> tips = dabPreviewTipsFor(brush, noLut, live);

    check(tips[0].radius < tips[1].radius && tips[1].radius < tips[2].radius,
          "the default brush's size link makes three different dabs");
    bool viaBrushTipFor = true;
    for (size_t i = 0; i < kDabPreviewCells; ++i)
      viaBrushTipFor = viaBrushTipFor &&
                       tips[i].radius == brushTipFor(brush, noLut, kDabPreviewPressures[i]).radius;
    check(viaBrushTipFor, "each cell is exactly brushTipFor() at its own pressure");
    check(tips[2].radius == brush.radius,
          "and the full-pressure cell is the slider's own radius");

    // Roundness and angle reach the tip at all -- brush/Deposit.hpp §2b's
    // whole subject. Before that they were dropped in `brushTipFor()` and this
    // assertion would have failed on a brush the UI showed as elliptical.
    brush.roundness = 0.42f;
    brush.angle = -70.0f;
    const std::array<BrushTip, kDabPreviewCells> shaped = dabPreviewTipsFor(brush, noLut, live);
    check(shaped[2].roundness == 0.42f && shaped[2].angle == -70.0f,
          "ROUNDNESS and ANGLE reach the tip -- they used to be dropped");

    // With every link removed the three cells must be byte-identical, which is
    // the converse: the cells differ ONLY because the dynamics say so.
    BrushState plain;
    plain.links.links.clear();
    const DabPreviewImage flatImg =
        rasteriseDabPreview(dabPreviewTipsFor(plain, noLut, live));
    bool cellsIdentical = true;
    for (int y = 0; y < kDabPreviewHeight && cellsIdentical; ++y)
      for (int lx = 0; lx < kDabPreviewCell && cellsIdentical; ++lx) {
        const size_t a = (static_cast<size_t>(y) * static_cast<size_t>(kDabPreviewWidth) +
                          static_cast<size_t>(lx)) *
                         4u;
        const size_t b = a + static_cast<size_t>(kDabPreviewCell) * 4u;
        const size_t c = a + static_cast<size_t>(2 * kDabPreviewCell) * 4u;
        for (int ch = 0; ch < 4; ++ch)
          cellsIdentical = cellsIdentical &&
                           flatImg.rgba[a + static_cast<size_t>(ch)] ==
                               flatImg.rgba[b + static_cast<size_t>(ch)] &&
                           flatImg.rgba[a + static_cast<size_t>(ch)] ==
                               flatImg.rgba[c + static_cast<size_t>(ch)];
      }
    check(cellsIdentical,
          "a brush with no links previews three identical dabs, exactly");
  }

  std::printf("[selftest] dab preview %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
