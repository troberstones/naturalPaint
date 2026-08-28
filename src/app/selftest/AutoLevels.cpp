#include "app/selftest/Support.hpp"

#include "ops/AutoLevels.hpp"

namespace np {
namespace {

// A HistogramResult with all four channel vectors pre-sized to `binCount`
// and zeroed -- every test below builds its fixture by hand, per this
// module's own "you can build HistogramResult values by hand" allowance
// (core::HistogramResult's fields are all public), rather than compositing
// a real Document just to get a handful of specific bin counts.
HistogramResult makeHistogram(int32_t binCount) {
  HistogramResult h;
  h.r.assign(static_cast<size_t>(binCount), uint64_t{0});
  h.g.assign(static_cast<size_t>(binCount), uint64_t{0});
  h.b.assign(static_cast<size_t>(binCount), uint64_t{0});
  h.luma.assign(static_cast<size_t>(binCount), uint64_t{0});
  return h;
}

void recomputeSampleCount(HistogramResult& h) {
  uint64_t total = 0;
  for (uint64_t c : h.luma) total += c;
  h.sampleCount = total;
}

}  // namespace

// ops/AutoLevels (docs/operations.md §1.2: Auto-tone, Auto-contrast,
// Auto-white-balance, plus this module's own Equalize addition). Pure CPU
// math over hand-built core::HistogramResult fixtures -- no Document, no
// PaintSim, no GPU involvement anywhere in this section.
//
// Covers: an already-full [0,1] histogram solves near-neutral; a
// lower-half-confined histogram solves to a white point that stretches it,
// checked against a hand-computed value derived from the same
// bin-center/srgbDecode chain AutoLevels.cpp itself documents; the clip
// fraction actually moves the white point away from a lone top-bin outlier
// (and only away from it -- clip=0 keeps the outlier); auto-tone produces
// three DIFFERENT LevelsParams on a colour-cast fixture while auto-contrast
// produces three IDENTICAL ones on the same fixture; Equalize never exceeds
// color::kMaxCurvePointsPerChannel points and is monotonically
// non-decreasing; and three degenerate fixtures (all-zero, and a single
// occupied bin) produce no NaN and no reversed black/white pair from any of
// the four solvers.
bool runAutoLevelsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr int32_t kBins = 256;

  // --- 1. An already-full [0,1] histogram (every bin occupied, clip = 0
  // so the search lands on the true extremes with no ambiguity) solves to
  // near-neutral LevelsParams: bin 0's own value is already close to 0 and
  // bin 255's is already close to 1 -- an auto op run on an already-correct
  // image should barely move it. -------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    for (int32_t i = 0; i < kBins; ++i) {
      h.r[static_cast<size_t>(i)] = 100;
      h.g[static_cast<size_t>(i)] = 100;
      h.b[static_cast<size_t>(i)] = 100;
      h.luma[static_cast<size_t>(i)] = 100;
    }
    recomputeSampleCount(h);

    const AutoLevelsParams noClip{0.0f};
    const std::array<LevelsParams, 3> tone = solveAutoTone(h, noClip);
    const std::array<LevelsParams, 3> contrast = solveAutoContrast(h, noClip);

    for (const auto& p : {tone[0], tone[1], tone[2], contrast[0]}) {
      check(p.blackIn >= 0.0f && p.blackIn < 0.01f,
            "solveAutoTone/solveAutoContrast: an already-full histogram's black point stays "
            "near 0");
      check(p.whiteIn > 0.95f && p.whiteIn <= 1.0f + 1e-6f,
            "solveAutoTone/solveAutoContrast: an already-full histogram's white point stays "
            "near 1");
    }
  }

  // --- 2. A histogram confined to the LOWER HALF (bins 0..127 only) solves
  // to a white point that STRETCHES it: direction (whiteIn < 1, so a value
  // at the old top of the occupied range now maps to output white) AND a
  // hand-computed exact value, tied to the exact bin-center/srgbDecode
  // chain AutoLevels.hpp's own doc comment specifies -- an off-by-one here
  // (edge instead of center, or a shifted bin index) changes this number
  // measurably, which is the point of checking it exactly rather than only
  // checking the direction. ------------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    for (int32_t i = 0; i <= 127; ++i) h.r[static_cast<size_t>(i)] = 100;
    recomputeSampleCount(h);

    const AutoLevelsParams noClip{0.0f};
    const std::array<LevelsParams, 3> tone = solveAutoTone(h, noClip);

    const float expectedWhiteLinear = srgbDecode((127.0f + 0.5f) / 256.0f);
    const float expectedBlackLinear = srgbDecode((0.0f + 0.5f) / 256.0f);

    check(tone[0].whiteIn < 1.0f,
          "solveAutoTone: a lower-half-confined channel solves a white point BELOW 1 -- "
          "stretches the image toward white");
    check(std::fabs(tone[0].whiteIn - expectedWhiteLinear) < 1e-5f,
          "solveAutoTone: that white point matches the hand-computed "
          "srgbDecode((127+0.5)/256) exactly");
    check(std::fabs(tone[0].blackIn - expectedBlackLinear) < 1e-5f,
          "solveAutoTone: the black point matches the hand-computed srgbDecode((0+0.5)/256) "
          "exactly");
  }

  // --- 3. The clip fraction actually clips: a lone outlier bin at the very
  // top (bin 255, count 1) sits far above the real top of the distribution
  // (bin 200, count 1000). At clip = 0 the outlier alone defines the white
  // point; at clip = 0.001 it is discarded and the white point instead
  // lands on the real top of the distribution -- asserted by checking which
  // hand-computed bin value the solved white point matches, not merely that
  // it changed. -------------------------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    h.r[0] = 1000;    // anchors the black point, not under test here
    h.r[200] = 1000;  // the real top of the distribution
    h.r[255] = 1;      // a single stray outlier pixel
    recomputeSampleCount(h);

    const float outlierWhite = srgbDecode((255.0f + 0.5f) / 256.0f);
    const float realTopWhite = srgbDecode((200.0f + 0.5f) / 256.0f);
    check(std::fabs(outlierWhite - realTopWhite) > 1e-4f,
          "runAutoLevelsTest: sanity check -- the outlier and the real top bin decode to "
          "measurably different linear values");

    const std::array<LevelsParams, 3> unclipped = solveAutoTone(h, AutoLevelsParams{0.0f});
    check(std::fabs(unclipped[0].whiteIn - outlierWhite) < 1e-5f,
          "solveAutoTone: clip = 0 lets the lone top outlier alone define the white point");

    const std::array<LevelsParams, 3> clipped = solveAutoTone(h, AutoLevelsParams{0.001f});
    check(std::fabs(clipped[0].whiteIn - realTopWhite) < 1e-5f,
          "solveAutoTone: clip = 0.001 discards that outlier and the white point lands on the "
          "real top of the distribution instead");
    check(std::fabs(clipped[0].whiteIn - unclipped[0].whiteIn) > 1e-4f,
          "solveAutoTone: ...so the white point measurably MOVED between the two clip settings");
  }

  // --- 4. Auto-tone vs. auto-contrast, on ONE fixture with a deliberate
  // colour cast (R confined low, G confined mid, B confined high): the
  // clearest statement of the difference between the two ops.
  // Auto-tone's three per-channel LevelsParams must DIFFER (that is what
  // neutralises the cast); auto-contrast's three must be IDENTICAL (that is
  // what preserves hue). ----------------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    for (int32_t i = 0; i <= 127; ++i) h.r[static_cast<size_t>(i)] = 50;    // R confined low
    for (int32_t i = 64; i <= 191; ++i) h.g[static_cast<size_t>(i)] = 50;   // G confined mid
    for (int32_t i = 128; i <= 255; ++i) h.b[static_cast<size_t>(i)] = 50;  // B confined high
    for (int32_t i = 0; i <= 255; ++i) h.luma[static_cast<size_t>(i)] = 50;  // composite: full range
    recomputeSampleCount(h);

    const AutoLevelsParams noClip{0.0f};
    const std::array<LevelsParams, 3> tone = solveAutoTone(h, noClip);
    const std::array<LevelsParams, 3> contrast = solveAutoContrast(h, noClip);

    check(std::fabs(tone[0].blackIn - tone[1].blackIn) > 1e-4f ||
              std::fabs(tone[0].whiteIn - tone[1].whiteIn) > 1e-4f,
          "solveAutoTone: R and G solve to DIFFERENT LevelsParams on a colour-cast fixture");
    check(std::fabs(tone[1].blackIn - tone[2].blackIn) > 1e-4f ||
              std::fabs(tone[1].whiteIn - tone[2].whiteIn) > 1e-4f,
          "solveAutoTone: G and B solve to DIFFERENT LevelsParams on the same fixture");

    check(contrast[0].blackIn == contrast[1].blackIn && contrast[1].blackIn == contrast[2].blackIn &&
              contrast[0].whiteIn == contrast[1].whiteIn && contrast[1].whiteIn == contrast[2].whiteIn,
          "solveAutoContrast: R, G and B solve to IDENTICAL LevelsParams on the same fixture -- "
          "hue preserved");
  }

  // --- 5. Equalize: never more control points than
  // kMaxCurvePointsPerChannel (color/LutBake.hpp), and monotonically
  // non-decreasing in both x and y. ------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    for (int32_t i = 0; i < kBins; ++i) h.luma[static_cast<size_t>(i)] = 10;
    recomputeSampleCount(h);

    const Curve curve = solveEqualize(h, AutoLevelsParams{0.0f});
    check(curve.size() <= static_cast<size_t>(kMaxCurvePointsPerChannel),
          "solveEqualize: never emits more control points than kMaxCurvePointsPerChannel");
    check(!curve.empty(), "solveEqualize: a genuinely spread-out histogram produces a real curve");

    bool monotonic = true;
    for (size_t i = 1; i < curve.size(); ++i) {
      if (curve[i].x < curve[i - 1].x || curve[i].y < curve[i - 1].y) monotonic = false;
    }
    check(monotonic, "solveEqualize: the curve is monotonically non-decreasing in both x and y");
  }

  // --- 6. Degenerate: an all-zero histogram must not divide by zero,
  // produce NaN, or move anything -- every solver returns its neutral
  // identity. -----------------------------------------------------------
  {
    HistogramResult h = makeHistogram(kBins);
    recomputeSampleCount(h);

    const std::array<LevelsParams, 3> tone = solveAutoTone(h, AutoLevelsParams{});
    const std::array<LevelsParams, 3> contrast = solveAutoContrast(h, AutoLevelsParams{});
    const std::array<LevelsParams, 3> color = solveAutoColor(h, AutoLevelsParams{});
    const Curve curve = solveEqualize(h, AutoLevelsParams{});

    bool allIdentityNoNaN = true;
    for (const auto* channels : {&tone, &contrast, &color}) {
      for (const LevelsParams& p : *channels) {
        if (std::isnan(p.blackIn) || std::isnan(p.whiteIn)) allIdentityNoNaN = false;
        if (p.whiteIn < p.blackIn) allIdentityNoNaN = false;  // never reversed
        if (!(p.blackIn == 0.0f && p.whiteIn == 1.0f && p.gamma == 1.0f && p.blackOut == 0.0f &&
              p.whiteOut == 1.0f))
          allIdentityNoNaN = false;
      }
    }
    check(allIdentityNoNaN,
          "solveAutoTone/solveAutoContrast/solveAutoColor: an all-zero histogram solves to "
          "exactly the neutral identity on every channel, no NaN, never reversed");
    check(curve.empty(), "solveEqualize: an all-zero histogram produces an empty (identity) curve");
  }

  // --- 6b. Degenerate: a SINGLE occupied bin (all mass at bin 128) must
  // not divide by zero or produce a reversed black/white point -- it is
  // well-defined (blackIn == whiteIn, not reversed) but not identity, since
  // there genuinely IS one data point, just no spread to stretch. ----------
  {
    HistogramResult h = makeHistogram(kBins);
    h.r[128] = 500;
    h.g[128] = 500;
    h.b[128] = 500;
    h.luma[128] = 500;
    recomputeSampleCount(h);

    const AutoLevelsParams noClip{0.0f};
    const std::array<LevelsParams, 3> tone = solveAutoTone(h, noClip);
    const std::array<LevelsParams, 3> contrast = solveAutoContrast(h, noClip);
    const std::array<LevelsParams, 3> color = solveAutoColor(h, noClip);
    const Curve curve = solveEqualize(h, noClip);

    bool wellDefined = true;
    for (const auto* channels : {&tone, &contrast, &color}) {
      for (const LevelsParams& p : *channels) {
        if (std::isnan(p.blackIn) || std::isnan(p.whiteIn)) wellDefined = false;
        if (p.whiteIn < p.blackIn) wellDefined = false;
      }
    }
    check(wellDefined,
          "solveAutoTone/solveAutoContrast/solveAutoColor: a single-occupied-bin histogram "
          "produces no NaN and no reversed black/white pair");
    check(std::fabs(tone[0].blackIn - tone[0].whiteIn) < 1e-6f,
          "solveAutoTone: a single occupied bin solves black == white (a real point, zero "
          "spread) rather than a reversed pair");
    check(curve.empty(),
          "solveEqualize: a single-occupied-bin histogram (nothing to redistribute) produces an "
          "empty (identity) curve");
    // Grey-world on a perfectly neutral single-value image: all three
    // channel means are equal, so grayTarget == each mean and every
    // whiteIn solves to 1 (no correction needed).
    check(std::fabs(color[0].whiteIn - 1.0f) < 1e-5f && std::fabs(color[1].whiteIn - 1.0f) < 1e-5f &&
              std::fabs(color[2].whiteIn - 1.0f) < 1e-5f,
          "solveAutoColor: a perfectly neutral single-value image solves whiteIn == 1 on every "
          "channel -- no correction applied where none is needed");
  }

  // --- The named limitation, pinned so it stays a known cost rather than
  // becoming a surprise -----------------------------------------------------
  //
  // AutoLevels.hpp's "the cost of expressing a gain as a white point" section:
  // a channel the grey-world estimate BOOSTS gets `whiteIn < 1`, and
  // `applyLevelsChannel()`'s `t` clamp then saturates every input above that
  // white point. So the correction clips the boosted channel's highlights.
  //
  // Asserted rather than merely documented, and asserted as the CONSEQUENCE
  // (a bright value comes out at exactly 1.0) rather than as the mechanism, so
  // it reddens if some future change makes the clipping worse or moves it,
  // and so a reader who trips over it in use finds this line when they grep.
  {
    HistogramResult h;
    h.r.assign(256, 0);
    h.g.assign(256, 0);
    h.b.assign(256, 0);
    h.luma.assign(256, 0);
    // A warm cast: red sits high, blue low. Blue is therefore the boosted
    // channel and the one that will clip.
    h.r[200] = 1000;
    h.g[128] = 1000;
    h.b[80] = 1000;
    h.luma[150] = 1000;

    const std::array<LevelsParams, 3> color = solveAutoColor(h, AutoLevelsParams{});
    check(color[2].whiteIn < 1.0f && color[0].whiteIn > 1.0f,
          "solveAutoColor: under a warm cast, blue is boosted (whiteIn < 1) and red is "
          "suppressed (whiteIn > 1) -- the fixture really is the case being described");

    // A blue value comfortably below 1.0 but above the solved white point.
    const float clipping = color[2].whiteIn + (1.0f - color[2].whiteIn) * 0.5f;
    const float out = applyLevelsChannel(clipping, color[2]);
    check(std::fabs(out - 1.0f) < 1e-6f,
          "solveAutoColor: KNOWN COST -- a boosted channel's values above its solved white "
          "point saturate to 1.0, clipping highlights (AutoLevels.hpp names why this trade "
          "was taken over the two alternatives)");
  }

  std::printf("[selftest] auto-levels %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
