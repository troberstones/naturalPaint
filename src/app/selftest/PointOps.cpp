#include "app/selftest/Support.hpp"

namespace np {

// ops/PointOps (Phase 3 steps 2+3; docs/operations.md §1.1; PRD B4). See
// SelfTest.hpp for the full breakdown. Pure CPU math throughout, matching
// runShaperTest()/runColorSpaceTest()'s own headless-first-class status --
// no PaintSim or gpu involvement anywhere in this function.
bool runPointOpsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };

  // --- Levels ---
  {
    // Neutral params (blackIn=0, whiteIn=1) is a true no-op for any input
    // *within* that [0,1] range -- deliberately not testing an HDR value
    // above 1.0 here: with whiteIn=1 the internal t-clamp legitimately
    // saturates such an input to whiteOut, which is correct Levels
    // black/white-point behaviour (every real levels tool clips outside
    // its own range), not a bug in "neutral is identity."
    const LevelsParams neutral{};
    const std::array<float, 3> rgb{0.05f, 0.5f, 0.95f};
    const std::array<float, 3> out = applyLevels(rgb, {neutral, neutral, neutral});
    check(nearRgb(out, rgb, kTol), "levels: neutral params is a true no-op within [blackIn, whiteIn]");
  }
  {
    // The flip side of the case above, made explicit rather than left
    // implicit: with default whiteIn=1/blackIn=0, an input outside that
    // range legitimately saturates to whiteOut/blackOut -- this IS the
    // internal t-clamp's documented consequence (PointOps.hpp's Levels
    // doc comment), not the general "ops don't clamp output" policy being
    // violated. A caller who needs Levels to pass HDR headroom through
    // untouched sets whiteIn above their expected max linear value.
    const LevelsParams neutral{};
    check(near(applyLevelsChannel(1.7f, neutral), 1.0f, kTol),
          "levels: neutral params saturates an above-whiteIn (HDR) input to whiteOut, by design");
    check(near(applyLevelsChannel(-0.3f, neutral), 0.0f, kTol),
          "levels: neutral params saturates a below-blackIn input to blackOut, by design");
  }
  {
    // Hand-computed: blackIn=0.1, whiteIn=0.9, gamma=2.0, blackOut=0.05,
    // whiteOut=0.95, input=0.5.
    //   t = (0.5-0.1)/(0.9-0.1) = 0.5
    //   t = pow(0.5, 1/2.0) = sqrt(0.5) = 0.7071067811865476
    //   out = 0.7071067811865476*(0.95-0.05)+0.05 = 0.6863961030678928
    LevelsParams p;
    p.blackIn = 0.1f;
    p.whiteIn = 0.9f;
    p.gamma = 2.0f;
    p.blackOut = 0.05f;
    p.whiteOut = 0.95f;
    const float out = applyLevelsChannel(0.5f, p);
    check(near(out, 0.6863961f, kTol), "levels: hand-computed non-trivial case matches");
  }
  {
    // Input below blackIn must not produce NaN -- the internal
    // clamp-t-to-[0,1]-before-pow() guard. blackIn=0.2, whiteIn=0.8,
    // gamma=0.5 (a fractional exponent -- exactly the case that would hit
    // pow() on a negative base without the clamp), blackOut/whiteOut left
    // at their neutral 0/1 default.
    //   t = (-1.0-0.2)/(0.8-0.2) = -2.0, clamped to 0
    //   pow(0, 1/0.5) = pow(0, 2.0) = 0
    //   out = 0*(1-0)+0 = 0
    LevelsParams p;
    p.blackIn = 0.2f;
    p.whiteIn = 0.8f;
    p.gamma = 0.5f;
    const float out = applyLevelsChannel(-1.0f, p);
    check(!std::isnan(out), "levels: input below blackIn does not produce NaN");
    check(near(out, 0.0f, kTol), "levels: below-blackIn input clamps to blackOut as hand-computed");
  }

  // --- Curves ---
  {
    Curve empty;
    check(near(evalCurve(empty, 0.37f), 0.37f, kTol), "evalCurve: 0 control points is identity");
    // 1 point is identity too -- NOT the single point's own y.
    Curve one = {{0.5f, 0.9f}};
    check(near(evalCurve(one, 0.37f), 0.37f, kTol),
          "evalCurve: 1 control point is identity, not the point's own y");
  }
  {
    // 2 points must reduce the Hermite formula exactly to the straight
    // line between them -- checked at several interior x, not just the
    // endpoints. Algebraically: with both endpoint tangents equal to the
    // shared secant slope m=(y1-y0)/dx, y(t) collapses to
    // y0*(1-t) + y1*t (h00-h10-h11 == 1-t and h10+h01+h11 == t identically).
    const Curve two = {{0.0f, 0.2f}, {1.0f, 0.8f}};
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const float expected = 0.2f + (0.8f - 0.2f) * x;
      const float actual = evalCurve(two, x);
      char label[112];
      std::snprintf(label, sizeof label,
                    "evalCurve: 2-point curve reduces to a straight line at x=%.2f", x);
      check(near(actual, expected, 1e-4f), label);
    }
  }
  {
    // Hand-computed 3-point interior case: points (0,0), (0.5,1.0),
    // (1.0,0.0), evaluated at x=0.25 (segment [0, 0.5]).
    //   tangent at (0,0)   [endpoint]: secant((0,0),(0.5,1.0)) = 2.0
    //   tangent at (0.5,1) [interior]: avg(secant((0,0),(0.5,1))=2.0,
    //                                      secant((0.5,1),(1,0))=-2.0) = 0.0
    //   t = (0.25-0)/0.5 = 0.5
    //   h00=0.5, h10=0.125, h01=0.5, h11=-0.125 (standard values at t=0.5)
    //   y = 0.5*0 + 0.125*0.5*2.0 + 0.5*1.0 + (-0.125)*0.5*0.0
    //     = 0 + 0.125 + 0.5 + 0 = 0.625
    const Curve three = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    check(near(evalCurve(three, 0.25f), 0.625f, kTol),
          "evalCurve: hand-computed 3-point interior case matches");
  }
  {
    // Flat extrapolation outside the authored x-range.
    const Curve c = {{0.2f, 0.3f}, {0.8f, 0.6f}};
    check(near(evalCurve(c, -5.0f), 0.3f, kTol),
          "evalCurve: extrapolates flat below the first control point");
    check(near(evalCurve(c, 5.0f), 0.6f, kTol),
          "evalCurve: extrapolates flat above the last control point");
  }
  {
    // (0,0)-(1,1) is the shaper-domain identity line (a 2-point curve, so
    // it reduces to y=x exactly per the property above). Composed end to
    // end -- shaperEncode -> evalCurve -> shaperDecode -- a spread of
    // linear inputs whose shaperEncode() lands inside [0,1] (true of all
    // five chosen here) must come back unchanged.
    std::array<Curve, 3> identityLine;
    identityLine[0] = identityLine[1] = identityLine[2] = Curve{{0.0f, 0.0f}, {1.0f, 1.0f}};
    for (float v : {0.0f, 0.02f, 0.18f, 0.5f, 1.0f}) {
      const std::array<float, 3> rgb{v, v, v};
      const std::array<float, 3> out = applyCurves(rgb, identityLine);
      char label[112];
      std::snprintf(label, sizeof label,
                    "applyCurves: shaper-domain identity line round-trips linear=%.3f", v);
      check(nearRgb(out, rgb, kTol), label);
    }
  }
  {
    // 0-point-per-channel curves through applyCurves(): exact passthrough,
    // no shaper round-trip at all (unlike the identity-line case above,
    // which does round-trip through the shaper and only approximately
    // preserves the input to float tolerance).
    const std::array<Curve, 3> empty{};
    const std::array<float, 3> rgb{0.37f, -0.4f, 12.0f};
    const std::array<float, 3> out = applyCurves(rgb, empty);
    check(nearRgb(out, rgb, kTol), "applyCurves: 0-point channels are an exact passthrough");
  }

  // --- Exposure ---
  {
    const std::array<float, 3> rgb{0.3f, 0.6f, 0.9f};
    check(nearRgb(applyExposure(rgb, ExposureParams{1.0f}), {0.6f, 1.2f, 1.8f}, kTol),
          "exposure: +1 stop doubles");
    check(nearRgb(applyExposure(rgb, ExposureParams{-1.0f}), {0.15f, 0.3f, 0.45f}, kTol),
          "exposure: -1 stop halves");
    check(nearRgb(applyExposure(rgb, ExposureParams{0.0f}), rgb, kTol),
          "exposure: 0 stops is identity");
  }

  // --- Saturation ---
  {
    const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
    check(nearRgb(applySaturation(rgb, SaturationParams{1.0f}), rgb, kTol),
          "saturation: scale=1 is identity");

    // Rec.709 luma computed by hand, directly from the literal weights --
    // independent of computeLuma()'s own copy of them.
    const float luma = 0.2126f * 0.2f + 0.7152f * 0.6f + 0.0722f * 0.9f;
    check(nearRgb(applySaturation(rgb, SaturationParams{0.0f}), {luma, luma, luma}, kTol),
          "saturation: scale=0 collapses every channel to the same value (the luma)");

    SaturationParams p;
    p.scale = 2.0f;
    const std::array<float, 3> expected{luma + (rgb[0] - luma) * 2.0f,
                                        luma + (rgb[1] - luma) * 2.0f,
                                        luma + (rgb[2] - luma) * 2.0f};
    check(nearRgb(applySaturation(rgb, p), expected, kTol),
          "saturation: hand-computed scale=2.0 (Rec.709 weights) case matches");
  }

  // --- Grayscale ---
  {
    const std::array<float, 3> red{1.0f, 0.0f, 0.0f};
    check(nearRgb(applyGrayscale(red, GrayscaleParams{}), {0.2126f, 0.2126f, 0.2126f}, kTol),
          "grayscale: pure red -> (0.2126, 0.2126, 0.2126)");

    const std::array<float, 3> rgb{0.3f, 0.5f, 0.7f};
    const float expectedLuma = 0.2126f * 0.3f + 0.7152f * 0.5f + 0.0722f * 0.7f;
    check(nearRgb(applyGrayscale(rgb, GrayscaleParams{}),
                  {expectedLuma, expectedLuma, expectedLuma}, kTol),
          "grayscale: general RGB case matches hand-computed Rec.709 luma");
  }

  // --- Channel mixer ---
  {
    const std::array<float, 3> rgb{0.2f, 0.5f, 0.8f};
    check(nearRgb(applyChannelMixer(rgb, ChannelMixerParams{}), rgb, kTol),
          "channel mixer: identity matrix is a no-op");

    ChannelMixerParams swapRB;
    swapRB.matrix = {{{0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}};
    check(nearRgb(applyChannelMixer(rgb, swapRB), {0.8f, 0.5f, 0.2f}, kTol),
          "channel mixer: hand-computed R/B swap matches");

    ChannelMixerParams offset;
    offset.matrix = {
        {{1.0f, 0.0f, 0.0f, 0.1f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}}};
    check(nearRgb(applyChannelMixer(rgb, offset), {0.3f, 0.5f, 0.8f}, kTol),
          "channel mixer: hand-computed +0.1 R-offset case matches");
  }

  // --- Premultiply wrapper (PRD B4) ---
  {
    const std::array<float, 4> transparent{0.3f, 0.4f, 0.5f, 0.0f};
    const std::vector<PointOp> anyOp{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{5.0f}); }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(transparent, anyOp);
    check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 0.0f,
          "premultiply wrapper: alpha<=0 maps to {0,0,0,0} untouched, regardless of the op");
  }
  {
    // Hand-computed: premultiplied (0.5,0,0,0.5) -> unpremultiply ->
    // (1,0,0) -> +1 stop exposure -> (2,0,0) -> re-premultiply by the
    // unchanged alpha 0.5 -> (1,0,0,0.5).
    const std::array<float, 4> premultiplied{0.5f, 0.0f, 0.0f, 0.5f};
    const std::vector<PointOp> ops{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{1.0f}); }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    check(near(out[0], 1.0f, kTol) && near(out[1], 0.0f, kTol) && near(out[2], 0.0f, kTol) &&
              near(out[3], 0.5f, kTol),
          "premultiply wrapper: hand-computed partially-transparent +1-stop example matches");
  }
  {
    // Alpha is never altered, regardless of which op runs -- even one
    // that collapses RGB entirely (saturation scale=0).
    const std::array<float, 4> premultiplied{0.2f, 0.4f, 0.1f, 0.37f};
    const std::vector<PointOp> ops{[](const std::array<float, 3>& rgb) {
      return applySaturation(rgb, SaturationParams{0.0f});
    }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    check(near(out[3], 0.37f, kTol), "premultiply wrapper: alpha is never altered by any op");
  }
  {
    // Composing two ops in sequence: premultiplied (0.4,0.2,0.0,0.4) ->
    // unpremultiply -> (1.0,0.5,0.0) -> exposure -1 stop -> (0.5,0.25,0.0)
    // -> saturation scale=0 (collapse to the Rec.709 luma of that
    // intermediate result) -> luma = 0.2126*0.5+0.7152*0.25+0.0722*0.0
    // = 0.2851 -> (0.2851,0.2851,0.2851) -> re-premultiply by the
    // unchanged alpha 0.4.
    const std::array<float, 4> premultiplied{0.4f, 0.2f, 0.0f, 0.4f};
    const std::vector<PointOp> ops{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{-1.0f}); },
        [](const std::array<float, 3>& rgb) {
          return applySaturation(rgb, SaturationParams{0.0f});
        },
    };
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    const float luma = 0.2126f * 0.5f + 0.7152f * 0.25f + 0.0722f * 0.0f;
    const float expected = luma * 0.4f;
    check(near(out[0], expected, kTol) && near(out[1], expected, kTol) &&
              near(out[2], expected, kTol) && near(out[3], 0.4f, kTol),
          "premultiply wrapper: composing two ops in sequence matches manual hand computation");
  }

  std::printf("[selftest] point ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
