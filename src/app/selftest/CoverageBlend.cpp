#include "app/selftest/Support.hpp"

#include "brush/CoverageBlend.hpp"
#include "brush/Deposit.hpp"

namespace np {

// ---------------------------------------------------------------------------
// brush/CoverageBlend: the ten ways two coverage values combine into one.
//
// This table is shared by the Dual Brush (`BlnM`) and the Texture panel
// (`textureBlendMode`) -- see its header for why one enum rather than two.
// Four of the arms MOVED here from `combineDualCoverage()` in
// brush/Deposit.cpp, so the first thing this file checks is that they did not
// change on the way: section B recomputes them from expressions written out
// by hand here rather than calling the function under test twice.
//
// Section D is the load-bearing one. **No blend may create coverage where the
// primary has none**, and both callers depend on it in ways neither could
// recover from: a dual brush's second tip must not paint outside the first
// tip's footprint, and `depositDab()` only ever visits texels `dabCoverage()`
// already accepted, so a blend returning more than zero at zero would be
// asking for a write the caller never computed bounds for. Exactly one of the
// ten can violate it -- Hard Mix, whose threshold `0 + second >= 1` is
// satisfiable -- which is why the guard is at the top of the function and not
// at one call site.
// ---------------------------------------------------------------------------
bool runCoverageBlendTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Every value asserted below is exact in binary floating point (halves,
  // quarters and eighths, and sums of them under 1), so these are `==`
  // comparisons and not near-equality ones. That is deliberate: a blend table
  // is arithmetic with no accumulation and no iteration, and a tolerance here
  // would hide exactly the kind of reordering this file exists to catch.
  const CoverageBlend kAll[] = {
      CoverageBlend::Multiply,   CoverageBlend::Overlay,    CoverageBlend::ColorBurn,
      CoverageBlend::HardMix,    CoverageBlend::LinearBurn, CoverageBlend::ColorDodge,
      CoverageBlend::Darken,     CoverageBlend::Subtract,   CoverageBlend::Height,
      CoverageBlend::LinearHeight,
  };
  constexpr int kCount = 10;

  // ======================================================================
  std::printf("  -- A. the enum: every mode named, exactly one unrenderable --\n");
  // ======================================================================
  {
    bool allNamed = true;
    bool allDistinct = true;
    for (int i = 0; i < kCount; ++i) {
      const char* ni = coverageBlendName(kAll[i]);
      if (ni == nullptr || ni[0] == '\0') allNamed = false;
      for (int j = i + 1; j < kCount; ++j) {
        if (std::strcmp(ni, coverageBlendName(kAll[j])) == 0) allDistinct = false;
      }
    }
    check(allNamed, "blend/name: every mode has a non-empty name");
    // Distinctness matters because the import report prints these names to
    // say what it could not composite; two modes sharing one string would
    // make a refusal impossible to attribute.
    check(allDistinct, "blend/name: no two modes share a name");

    int unrenderable = 0;
    for (const CoverageBlend b : kAll)
      if (!coverageBlendIsRenderable(b)) ++unrenderable;
    check(unrenderable == 1, "blend/renderable: exactly one mode has no formula");
    check(!coverageBlendIsRenderable(CoverageBlend::LinearHeight),
          "blend/renderable: and it is Linear Height, the one with no published formula");

    // The fallback, not a formula: a caller that skips the renderability
    // check gets the primary tip alone. Asserted so that "falls back to the
    // primary" cannot quietly become "falls back to Multiply" later.
    check(applyCoverageBlend(CoverageBlend::LinearHeight, 0.625f, 0.25f) == 0.625f,
          "blend/LinearHeight: returns the base untouched, a fallback not a guess");
    check(applyCoverageBlend(CoverageBlend::LinearHeight, 0.625f, 1.0f) == 0.625f,
          "blend/LinearHeight: and ignores `second` entirely, whatever it is");
  }

  // ======================================================================
  std::printf("  -- B. the four arms that MOVED from combineDualCoverage() --\n");
  // ======================================================================
  {
    // Recomputed from expressions written out here by hand. Calling
    // `combineDualCoverage()` and `applyCoverageBlend()` and comparing them
    // would prove nothing at all now that the first is implemented in terms
    // of the second -- it would be a tautology that passes however wrong both
    // are. These four expressions are the formulas as they read in
    // brush/Deposit.cpp before the move.
    auto legacyMultiply = [](float b, float s) { return b * s; };
    auto legacyOverlay = [](float b, float s) {
      return b < 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
    };
    auto legacyColorBurn = [](float b, float s) {
      if (b >= 1.0f) return 1.0f;
      if (s <= 0.0f) return 0.0f;
      return 1.0f - std::min(1.0f, (1.0f - b) / s);
    };
    auto legacyHardMix = [](float b, float s) {
      return (b > 0.0f && b + s >= 1.0f) ? 1.0f : 0.0f;
    };

    bool mulSame = true, ovlSame = true, cbrSame = true, hmxSame = true;
    for (int bi = 0; bi <= 16; ++bi) {
      for (int si = 0; si <= 16; ++si) {
        const float b = static_cast<float>(bi) / 16.0f;
        const float s = static_cast<float>(si) / 16.0f;
        if (applyCoverageBlend(CoverageBlend::Multiply, b, s) != legacyMultiply(b, s) &&
            b > 0.0f)
          mulSame = false;
        if (applyCoverageBlend(CoverageBlend::Overlay, b, s) != legacyOverlay(b, s) &&
            b > 0.0f)
          ovlSame = false;
        if (applyCoverageBlend(CoverageBlend::ColorBurn, b, s) != legacyColorBurn(b, s) &&
            b > 0.0f)
          cbrSame = false;
        if (applyCoverageBlend(CoverageBlend::HardMix, b, s) != legacyHardMix(b, s))
          hmxSame = false;
      }
    }
    // 17x17 exactly-representable pairs, every one bit-identical to the
    // pre-move expression. `b > 0` is excluded from the first three because
    // that case is section D's subject and the legacy expressions did not
    // guard it; Hard Mix is NOT excluded, because its legacy expression
    // carried the guard and this is where that is checked.
    check(mulSame, "blend/Multiply: bit-identical to the pre-move formula over a 17x17 grid");
    check(ovlSame, "blend/Overlay: bit-identical to the pre-move formula over a 17x17 grid");
    check(cbrSame, "blend/ColorBurn: bit-identical to the pre-move formula over a 17x17 grid");
    check(hmxSame,
          "blend/HardMix: bit-identical INCLUDING the base>0 guard it brought with it");

    // The two singular cases Color Burn's three-way form exists for, pinned
    // by name so a later "simplification" back to the two-way form is caught.
    check(applyCoverageBlend(CoverageBlend::ColorBurn, 1.0f, 0.0f) == 1.0f,
          "blend/ColorBurn: a saturated base stays saturated without dividing by zero");
    check(applyCoverageBlend(CoverageBlend::ColorBurn, 0.5f, 0.0f) == 0.0f,
          "blend/ColorBurn: second == 0 is the pole, defined as 0 (its own limit)");
    check(applyCoverageBlend(CoverageBlend::ColorBurn, 0.75f, 0.5f) == 0.5f,
          "blend/ColorBurn: 1 - min(1, 0.25/0.5) == 0.5");

    check(applyCoverageBlend(CoverageBlend::Multiply, 0.5f, 0.25f) == 0.125f,
          "blend/Multiply: 0.5 * 0.25 == 0.125");
    check(applyCoverageBlend(CoverageBlend::Overlay, 0.25f, 0.5f) == 0.25f,
          "blend/Overlay: below the hinge, 2 * 0.25 * 0.5 == 0.25");
    check(applyCoverageBlend(CoverageBlend::Overlay, 0.75f, 0.5f) == 0.75f,
          "blend/Overlay: above the hinge, 1 - 2 * 0.25 * 0.5 == 0.75");
    check(applyCoverageBlend(CoverageBlend::HardMix, 0.25f, 0.5f) == 0.0f &&
              applyCoverageBlend(CoverageBlend::HardMix, 0.75f, 0.5f) == 1.0f,
          "blend/HardMix: a hard threshold at base + second == 1, and nothing between");
  }

  // ======================================================================
  std::printf("  -- C. the six that arrive with the Texture panel --\n");
  // ======================================================================
  {
    check(applyCoverageBlend(CoverageBlend::LinearBurn, 0.75f, 0.5f) == 0.25f,
          "blend/LinearBurn: 0.75 + 0.5 - 1 == 0.25");
    check(applyCoverageBlend(CoverageBlend::LinearBurn, 0.25f, 0.5f) == 0.0f,
          "blend/LinearBurn: clamps at zero rather than going negative");

    check(applyCoverageBlend(CoverageBlend::ColorDodge, 0.25f, 0.5f) == 0.5f,
          "blend/ColorDodge: 0.25 / (1 - 0.5) == 0.5");
    check(applyCoverageBlend(CoverageBlend::ColorDodge, 0.5f, 1.0f) == 1.0f,
          "blend/ColorDodge: second == 1 is its pole, defined as full rather than inf");
    check(applyCoverageBlend(CoverageBlend::ColorDodge, 0.75f, 0.75f) == 1.0f,
          "blend/ColorDodge: and the quotient is clamped, never above 1");

    check(applyCoverageBlend(CoverageBlend::Darken, 0.75f, 0.25f) == 0.25f &&
              applyCoverageBlend(CoverageBlend::Darken, 0.25f, 0.75f) == 0.25f,
          "blend/Darken: the minimum, and symmetric in its two arguments");

    check(applyCoverageBlend(CoverageBlend::Subtract, 0.75f, 0.25f) == 0.5f,
          "blend/Subtract: 0.75 - 0.25 == 0.5");
    check(applyCoverageBlend(CoverageBlend::Subtract, 0.25f, 0.75f) == 0.0f,
          "blend/Subtract: clamps at zero, so it can empty a texel but not invert it");

    // **Height and Subtract are the same arithmetic here, and that is an
    // approximation this build states rather than a decoding.** Photoshop
    // distinguishes them; no source consulted gives Height a per-pixel
    // formula, so it resolves to the expired patent's own subtractive
    // mechanism (US 5,347,620's `P*S*O1 - G`), which IS a published
    // height-map combination this project is licensed to implement. The
    // assertion is here so the approximation is visible in the test output
    // rather than buried in a header, and so that implementing Height
    // properly later is a change that shows up red.
    bool heightIsSubtract = true;
    for (int bi = 0; bi <= 16; ++bi) {
      for (int si = 0; si <= 16; ++si) {
        const float b = static_cast<float>(bi) / 16.0f;
        const float s = static_cast<float>(si) / 16.0f;
        if (applyCoverageBlend(CoverageBlend::Height, b, s) !=
            applyCoverageBlend(CoverageBlend::Subtract, b, s))
          heightIsSubtract = false;
      }
    }
    check(heightIsSubtract,
          "blend/Height: identical to Subtract -- a STATED approximation, not a decoding");
  }

  // ======================================================================
  std::printf("  -- D. the invariant both callers depend on: no blend CREATES coverage --\n");
  // ======================================================================
  {
    // A dual brush's second tip must not paint outside the first tip's
    // footprint, and `depositDab()` only visits texels `dabCoverage()` already
    // accepted -- so a non-zero result at zero base is coverage nothing
    // computed bounds for. This is the guard's whole reason to exist, and it
    // is the sabotage target: delete the `if (!(a > 0.0f)) return 0.0f;` line
    // in brush/CoverageBlend.cpp and Hard Mix alone turns this red.
    bool noneCreate = true;
    const char* firstOffender = "none";
    for (const CoverageBlend blend : kAll) {
      for (int si = 0; si <= 32; ++si) {
        const float s = static_cast<float>(si) / 32.0f;
        if (applyCoverageBlend(blend, 0.0f, s) != 0.0f) {
          noneCreate = false;
          if (std::strcmp(firstOffender, "none") == 0) firstOffender = coverageBlendName(blend);
        }
        // A negative base is the same case reached from outside the domain --
        // `dabCoverage()` cannot return one, but the clamp is what makes that
        // an argument about this function rather than about its callers.
        if (applyCoverageBlend(blend, -0.5f, s) != 0.0f) noneCreate = false;
      }
    }
    if (!noneCreate)
      std::printf("    (first mode to create coverage from nothing: %s)\n", firstOffender);
    check(noneCreate,
          "blend/invariant: all ten modes return 0 at base 0, for every second in [0,1]");

    // The same claim through the dual brush's own entry point, because that
    // is the caller whose bounds depend on it and a thin wrapper is exactly
    // the kind of thing that grows a bug later.
    bool dualNoneCreate = true;
    for (const CoverageBlend blend : kAll)
      for (int si = 0; si <= 32; ++si)
        if (combineDualCoverage(blend, 0.0f, static_cast<float>(si) / 32.0f) != 0.0f)
          dualNoneCreate = false;
    check(dualNoneCreate,
          "blend/invariant: and through combineDualCoverage(), the caller that needs it");
  }

  // ======================================================================
  std::printf("  -- E. range and totality: no NaN, no escape from [0,1], no gap --\n");
  // ======================================================================
  {
    // Includes out-of-range inputs on both sides. `GrainParams::strength` is
    // explicitly allowed above 1, so a caller CAN hand this function a base
    // above 1; the clamp is what makes that a defined answer rather than an
    // arm-dependent one.
    const float kProbes[] = {-1.0f, -0.25f, 0.0f, 0.125f, 0.5f, 0.875f, 1.0f, 1.5f, 4.0f};
    bool inRange = true;
    bool finite = true;
    for (const CoverageBlend blend : kAll) {
      for (const float b : kProbes) {
        for (const float s : kProbes) {
          const float r = applyCoverageBlend(blend, b, s);
          if (!std::isfinite(r)) finite = false;
          if (r < 0.0f || r > 1.0f) inRange = false;
        }
      }
    }
    check(finite, "blend/range: finite for every mode, including out-of-range arguments");
    check(inRange, "blend/range: and always within [0,1], which is what a coverage is");

    // Totality: a mode added to the enum without an arm would fall to the
    // function's trailing `return a`, which is Linear Height's answer and
    // would be silently wrong for anything else. Counting the enumerators
    // here means adding one without updating this list is itself the failure.
    check(static_cast<int>(CoverageBlend::LinearHeight) == kCount - 1,
          "blend/totality: LinearHeight is last, so this file covers every enumerator");
  }

  return ok;
}

}  // namespace np
