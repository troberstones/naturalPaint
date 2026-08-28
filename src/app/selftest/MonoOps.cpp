#include "app/selftest/Support.hpp"

#include "ops/MonoOps.hpp"

namespace np {

// ops/MonoOps (docs/operations.md §1.2: Black & white and Gradient map, both
// class A / P1). See SelfTest.hpp for the full breakdown. Pure CPU math
// throughout, matching runPointOpsTest()'s/runGradientTest()'s own
// headless-first-class status -- no PaintSim or GPU involvement anywhere in
// this function.
bool runMonoOpsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };

  // =========================================================================
  // Black & white
  // =========================================================================

  // --- Achromatic output: r == g == b for every input, chromatic or not ---
  {
    const BlackAndWhiteParams p{};
    const std::array<float, 3> cases[] = {
        {1.0f, 0.0f, 0.0f}, {0.2f, 0.7f, 0.9f}, {2.0f, 1.0f, 0.0f}, {-0.5f, -0.5f, -0.5f},
        {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    bool allAchromatic = true;
    for (const auto& c : cases) {
      const auto out = applyBlackAndWhite(c, p);
      if (out[0] != out[1] || out[1] != out[2]) allAchromatic = false;
    }
    check(allAchromatic, "black&white: output is achromatic (r==g==b) for every input");
  }

  // --- Isolation: pure red is governed by `reds` and NOTHING else ---
  {
    BlackAndWhiteParams a{};
    BlackAndWhiteParams b{};
    b.greens = a.greens + 5.0f;   // perturb every OTHER weight
    b.yellows = a.yellows - 3.0f;
    b.cyans = a.cyans + 2.0f;
    b.blues = a.blues - 1.0f;
    b.magentas = a.magentas + 4.0f;
    // reds left identical between a and b.
    const std::array<float, 3> pureRed{1.0f, 0.0f, 0.0f};
    const auto outA = applyBlackAndWhite(pureRed, a);
    const auto outB = applyBlackAndWhite(pureRed, b);
    check(nearRgb(outA, outB, 1e-6f),
          "black&white: pure red is unchanged by perturbing every non-reds weight");
    check(near(outA[0], a.reds, 1e-6f), "black&white: pure red equals the `reds` weight exactly");
  }
  // --- Symmetric check: pure green is governed by `greens` and nothing else ---
  {
    BlackAndWhiteParams a{};
    BlackAndWhiteParams b{};
    b.reds = a.reds + 5.0f;
    b.yellows = a.yellows - 3.0f;
    b.cyans = a.cyans + 2.0f;
    b.blues = a.blues - 1.0f;
    b.magentas = a.magentas + 4.0f;
    const std::array<float, 3> pureGreen{0.0f, 1.0f, 0.0f};
    const auto outA = applyBlackAndWhite(pureGreen, a);
    const auto outB = applyBlackAndWhite(pureGreen, b);
    check(nearRgb(outA, outB, 1e-6f),
          "black&white: pure green is unchanged by perturbing every non-greens weight");
    check(near(outA[0], a.greens, 1e-6f), "black&white: pure green equals the `greens` weight exactly");
  }

  // --- Default weights: an EXACT Rec.709 grayscale, not merely a neutral
  // one -- see MonoOps.hpp's affine-agreement proof. Checked at an anchor,
  // at a sector midpoint (hand-computed below), and above 1.0 (HDR). ---
  {
    const BlackAndWhiteParams p{};
    const GrayscaleParams g{};
    const std::array<float, 3> cases[] = {
        {1.0f, 0.0f, 0.0f},   // anchor (Red)
        {0.8f, 0.5f, 0.2f},   // general, mid-sector
        {2.0f, 1.0f, 0.0f},   // above 1.0 -- HDR, same sector as the case above
    };
    bool allMatch = true;
    for (const auto& c : cases) {
      const auto bw = applyBlackAndWhite(c, p);
      const auto gs = applyGrayscale(c, g);
      if (!near(bw[0], gs[0], 1e-5f)) allMatch = false;
    }
    check(allMatch, "black&white: default weights equal applyGrayscale()'s Rec.709 luma exactly");

    // Hand-computed: rgb = (1, 0.5, 0). M=r=1, m=0, C=1. M==r branch:
    // h = (g-b)/C = 0.5, sector 0, frac 0.5.
    // weight = lerp(reds=0.2126, yellows=0.9278, 0.5) = 0.5702.
    // output = m + weight*C = 0 + 0.5702 = 0.5702.
    // Cross-checked against Rec.709 luma directly:
    // 0.2126*1 + 0.7152*0.5 + 0.0722*0 = 0.2126 + 0.3576 = 0.5702.
    const std::array<float, 3> orange{1.0f, 0.5f, 0.0f};
    const auto out = applyBlackAndWhite(orange, p);
    check(near(out[0], 0.5702f, 1e-4f), "black&white: hand-computed mid-sector case (0.5702)");
  }

  // --- Custom weights: reds=1, everything else=0 -- isolates exactly one
  // sector's endpoint weight, hand-checkable without touching the default
  // Rec.709 constants at all. ---
  {
    BlackAndWhiteParams p{};
    p.reds = 1.0f;
    p.yellows = p.greens = p.cyans = p.blues = p.magentas = 0.0f;
    const auto red = applyBlackAndWhite({1.0f, 0.0f, 0.0f}, p);
    check(near(red[0], 1.0f, 1e-6f), "black&white: custom weights, pure red -> reds=1 exactly");
    const auto green = applyBlackAndWhite({0.0f, 1.0f, 0.0f}, p);
    check(near(green[0], 0.0f, 1e-6f), "black&white: custom weights, pure green -> greens=0 exactly");
  }

  // --- No clamp: achromatic HDR and achromatic below-zero both pass
  // straight through as `m`, unclamped. ---
  {
    const BlackAndWhiteParams p{};
    const auto hi = applyBlackAndWhite({3.0f, 3.0f, 3.0f}, p);
    check(near(hi[0], 3.0f, 1e-6f), "black&white: achromatic HDR (3,3,3) is not clamped to 1.0");
    const auto lo = applyBlackAndWhite({-1.0f, -1.0f, -1.0f}, p);
    check(near(lo[0], -1.0f, 1e-6f), "black&white: achromatic below-zero (-1,-1,-1) is not clamped to 0.0");
  }

  // =========================================================================
  // Gradient map
  // =========================================================================

  // --- Identity: default-constructed GradientMapParams (no colour stops)
  // is an exact passthrough -- the (b) choice documented in MonoOps.hpp,
  // checked on more than one input including HDR and negative. ---
  {
    const GradientMapParams p{};
    const std::array<float, 3> cases[] = {
        {0.3f, 0.9f, -0.2f}, {2.0f, 3.0f, 4.0f}, {0.0f, 0.0f, 0.0f}};
    bool allIdentity = true;
    for (const auto& c : cases) {
      if (applyGradientMap(c, p) != c) allIdentity = false;
    }
    check(allIdentity, "gradient map: default (no colour stops) is an exact passthrough");
  }

  // --- Known black->white ramp, luma computed in linear light ---
  {
    GradientMapParams p{};
    p.stops.colorStops = {
        ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f},
        ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f},
    };
    // Pure red's luma under default (Rec.709) weights is exactly
    // kRec709LumaWeights[0] = 0.2126 -- the stop midpoints are both 0.5
    // (a straight lerp per ColorStop::midpoint's own doc comment), so
    // gradientColorAt(stops, 0.2126) = black + 0.2126*(white-black)
    //                                = (0.2126, 0.2126, 0.2126).
    const auto out = applyGradientMap({1.0f, 0.0f, 0.0f}, p);
    check(nearRgb(out, {0.2126f, 0.2126f, 0.2126f}, 1e-4f),
          "gradient map: pure red's luma (0.2126) through a black->white ramp");
  }

  // --- Known two-colour ramp, luma at the midpoint ---
  {
    GradientMapParams p{};
    p.stops.colorStops = {
        ColorStop{0.0f, {1.0f, 0.0f, 0.0f}, 0.5f},  // red
        ColorStop{1.0f, {0.0f, 0.0f, 1.0f}, 0.5f},  // blue
    };
    // rgb = (0.5, 0.5, 0.5): luma = 0.5*(sum of Rec.709 weights) = 0.5*1.0
    // = 0.5 exactly (kRec709LumaWeights sums to 1.0). At t=0.5, a straight
    // lerp of red->blue is (0.5, 0, 0.5).
    const auto out = applyGradientMap({0.5f, 0.5f, 0.5f}, p);
    check(nearRgb(out, {0.5f, 0.0f, 0.5f}, 1e-4f),
          "gradient map: hand-computed red->blue ramp at luma 0.5");
  }

  // --- Above the gradient's domain: flat extrapolation from
  // gradientColorAt(), inherited unchanged -- NOT a clamp this op imposes.
  // Two different far-above-range lumas must land on the identical colour,
  // proving the plateau is truly flat rather than merely close. ---
  {
    GradientMapParams p{};
    p.stops.colorStops = {
        ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f},
        ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f},
    };
    // rgb=(2,2,2): luma = 2.0 (sum of weights is 1.0), well above the top
    // stop at t=1.
    const auto hi1 = applyGradientMap({2.0f, 2.0f, 2.0f}, p);
    // rgb=(1000,1000,1000): luma = 1000.0, far further above.
    const auto hi2 = applyGradientMap({1000.0f, 1000.0f, 1000.0f}, p);
    check(nearRgb(hi1, {1.0f, 1.0f, 1.0f}, 1e-4f),
          "gradient map: luma 2.0 (above domain) extrapolates flat to the top stop");
    check(hi1 == hi2,
          "gradient map: luma 2.0 and luma 1000.0 give the IDENTICAL colour (flat, not clamped-close)");
    // Below the domain: symmetric check on the low side.
    const auto lo = applyGradientMap({-3.0f, -3.0f, -3.0f}, p);
    check(nearRgb(lo, {0.0f, 0.0f, 0.0f}, 1e-4f),
          "gradient map: luma -3.0 (below domain) extrapolates flat to the bottom stop");
  }

  // --- Custom lumaWeights are honoured, not silently hardcoded to
  // Rec.709 -- isolate the red channel as the only source of luma. ---
  {
    GradientMapParams p{};
    p.lumaWeights = {1.0f, 0.0f, 0.0f};
    p.stops.colorStops = {
        ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f},
        ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f},
    };
    // rgb=(1,0,0): with red-only weights, luma=1.0 -> top stop (white).
    const auto redDrives = applyGradientMap({1.0f, 0.0f, 0.0f}, p);
    check(nearRgb(redDrives, {1.0f, 1.0f, 1.0f}, 1e-4f),
          "gradient map: custom lumaWeights isolating red -> pure red maps to the top stop");
    // rgb=(0,1,1): with red-only weights, luma=0.0 -> bottom stop (black),
    // even though this pixel is visually bright.
    const auto greenBlueIgnored = applyGradientMap({0.0f, 1.0f, 1.0f}, p);
    check(nearRgb(greenBlueIgnored, {0.0f, 0.0f, 0.0f}, 1e-4f),
          "gradient map: custom lumaWeights isolating red -> green/blue-only pixel maps to the bottom stop");
  }

  std::printf("[selftest] mono ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
