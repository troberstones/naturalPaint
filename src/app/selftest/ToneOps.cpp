#include "app/selftest/Support.hpp"

#include "ops/ToneOps.hpp"

namespace np {

// ops/ToneOps (docs/operations.md §1.2 "Committed additions"): four more
// pure `rgb -> rgb` point ops extending the ops/PointOps family -- Gain /
// offset / gamma, Invert, Posterize, Threshold. Pure CPU math throughout,
// matching runPointOpsTest()'s own headless-first-class status -- no
// PaintSim or GPU involvement anywhere in this function.
//
// Each op's identity default is asserted exactly (bit-for-bit, not merely
// within tolerance), each parameter's direction is asserted (not just that
// something changed), every op has at least one hand-computed value with
// the arithmetic shown, HDR headroom above 1.0 is asserted un-clamped, and
// both Invert domains are covered independently.
bool runToneOpsTest() {
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

  // ===================================================================
  // 1. Gain / offset / gamma
  // ===================================================================
  {
    // Identity default (gain=1, offset=0, gamma=1): base == input exactly,
    // and the gamma==1 branch returns `base` without ever calling pow(),
    // so this must hold bit-for-bit -- including for a negative and an
    // above-1.0 (HDR) input, per ToneOps.hpp's own doc comment.
    const GainOffsetGammaParams neutral{};
    const std::array<float, 3> rgb{-0.4f, 0.3f, 2.7f};
    const std::array<float, 3> out = applyGainOffsetGamma(rgb, neutral);
    check(out[0] == rgb[0] && out[1] == rgb[1] && out[2] == rgb[2],
          "gainOffsetGamma: neutral params is an exact identity, incl. negative and HDR input");
  }
  {
    // Direction: gain scales the signal directly -- for a positive input,
    // a larger gain must produce a larger output.
    GainOffsetGammaParams p;
    p.gain = 2.0f;
    const float lo = applyGainOffsetGammaChannel(0.4f, GainOffsetGammaParams{});
    const float hi = applyGainOffsetGammaChannel(0.4f, p);
    check(hi > lo, "gainOffsetGamma: raising gain brightens a positive input");
  }
  {
    // Direction: offset is a pedestal shift applied after gain -- a larger
    // offset must produce a larger output, by exactly the offset delta
    // when gain=1/gamma=1 (a pure additive shift).
    GainOffsetGammaParams p;
    p.offset = 0.2f;
    const float out = applyGainOffsetGammaChannel(0.4f, p);
    check(near(out, 0.6f, kTol), "gainOffsetGamma: offset alone shifts output by exactly the offset");
  }
  {
    // Direction: for a base value inside (0,1), raising gamma above 1
    // lifts the output (the standard "gamma raises midtones" behaviour) --
    // pow(0.5, 1/gamma) grows toward 1 as gamma grows past 1, since the
    // exponent 1/gamma shrinks below 1.
    GainOffsetGammaParams flat;   // gain=1, offset=0 -> base == input == 0.5
    GainOffsetGammaParams lifted;
    lifted.gamma = 2.0f;
    const float baseOut = applyGainOffsetGammaChannel(0.5f, flat);
    const float liftedOut = applyGainOffsetGammaChannel(0.5f, lifted);
    check(near(baseOut, 0.5f, kTol), "gainOffsetGamma: gamma=1 on a 0.5 input is the input unchanged");
    check(liftedOut > baseOut, "gainOffsetGamma: raising gamma above 1 lifts a midtone (0,1) input");
  }
  {
    // Hand-computed: gain=2.0, offset=0.1, gamma=2.2, input=0.3.
    //   base = 0.3*2.0 + 0.1 = 0.7
    //   out  = pow(0.7, 1/2.2) = 0.8503349277020302
    GainOffsetGammaParams p;
    p.gain = 2.0f;
    p.offset = 0.1f;
    p.gamma = 2.2f;
    const float out = applyGainOffsetGammaChannel(0.3f, p);
    check(near(out, 0.8503349f, kTol), "gainOffsetGamma: hand-computed non-trivial case matches");
  }
  {
    // HDR headroom is not clamped: gain=1.5 on an above-white input stays
    // above 1.0, exactly -- no policy clamp anywhere in the gamma==1 path.
    GainOffsetGammaParams p;
    p.gain = 1.5f;
    const float out = applyGainOffsetGammaChannel(2.0f, p);
    check(near(out, 3.0f, kTol), "gainOffsetGamma: gain on an HDR input is not clamped to [0,1]");
  }
  {
    // gamma applied to an HDR base: gain=1, offset=0, gamma=2.0, input=4.0
    // -> base=4.0, out=pow(4.0, 0.5)=2.0 -- legitimately above 1.0, proving
    // the gamma stage itself does not clamp HDR headroom either.
    GainOffsetGammaParams p;
    p.gamma = 2.0f;
    const float out = applyGainOffsetGammaChannel(4.0f, p);
    check(near(out, 2.0f, kTol), "gainOffsetGamma: gamma stage leaves HDR headroom above 1.0 uncapped");
  }
  {
    // The one required-by-math clamp: offset=-1.0, gain=1, gamma=2.0,
    // input=0.0 drives base=-1.0, which pow()'s fractional exponent (0.5)
    // would turn into NaN without the internal base clamp.
    //   base = 0.0*1.0 + (-1.0) = -1.0, clamped to 0.0
    //   out  = pow(0.0, 0.5) = 0.0
    GainOffsetGammaParams p;
    p.offset = -1.0f;
    p.gamma = 2.0f;
    const float out = applyGainOffsetGammaChannel(0.0f, p);
    check(!std::isnan(out), "gainOffsetGamma: a negative base under fractional gamma does not produce NaN");
    check(near(out, 0.0f, kTol), "gainOffsetGamma: negative-base clamp matches the hand-computed result");
  }

  // ===================================================================
  // 2. Invert
  // ===================================================================
  {
    // `amount = 0` is an exact identity -- blend()'s `t == 0` early return
    // makes it bit-exact, including for a negative and an above-1.0 (HDR)
    // input. Still asserted, but note it is deliberately NOT the default:
    // see the assertion below it.
    InvertParams off;
    off.amount = 0.0f;
    const std::array<float, 3> rgb{-0.4f, 0.3f, 2.7f};
    const std::array<float, 3> out = applyInvert(rgb, off);
    check(out[0] == rgb[0] && out[1] == rgb[1] && out[2] == rgb[2],
          "invert: amount=0 is an exact identity, incl. negative and HDR input, in both domains");

    // **A default-constructed Invert must actually invert**, which is the
    // whole reason `amount` defaults to 1 rather than to 0 (ToneOps.hpp's
    // header comment on what a default params struct is taken to MEAN). The
    // Invert menu item has no dialog -- Photoshop's does not either -- so the
    // wiring layer's natural call is `applyInvert(rgb, InvertParams{})`, and
    // the first version of this op returned the input unchanged for it. That
    // was silent: no refusal, no message, an adjustment that ran and did
    // nothing. This assertion is the tripwire for it coming back.
    const std::array<float, 3> mid{0.3f, 0.3f, 0.3f};
    const std::array<float, 3> byDefault = applyInvert(mid, InvertParams{});
    check(near(byDefault[0], 0.7f, kTol),
          "invert: a DEFAULT-constructed Invert inverts (1 - 0.3 = 0.7) -- the no-dialog "
          "menu item's own call, which a 0 default would have made a silent no-op");
  }
  {
    // Direction: amount interpolates monotonically from input toward the
    // fully-inverted value. Linear domain, input=0.3 -> fully inverted 0.7.
    InvertParams half;
    half.amount = 0.5f;
    InvertParams full;
    full.amount = 1.0f;
    const std::array<float, 3> rgb{0.3f, 0.3f, 0.3f};
    const std::array<float, 3> halfOut = applyInvert(rgb, half);
    const std::array<float, 3> fullOut = applyInvert(rgb, full);
    check(near(halfOut[0], 0.5f, kTol), "invert: amount=0.5 sits halfway between input and full invert");
    check(near(fullOut[0], 0.7f, kTol), "invert: amount=1.0 (linear) hand-computed: 1 - 0.3 = 0.7");
    check(halfOut[0] > rgb[0] && halfOut[0] < fullOut[0],
          "invert: increasing amount moves monotonically from input toward the inverted value");
  }
  {
    // HDR headroom, linear domain: input above the 1.0 pivot inverts to a
    // NEGATIVE value -- the mathematically honest, un-clamped result (see
    // ToneOps.hpp's doc comment on why clamping here would be a bug).
    //   1.0 - 1.5 = -0.5
    InvertParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyInvert({1.5f, 1.5f, 1.5f}, p);
    check(near(out[0], -0.5f, kTol),
          "invert: linear domain, an above-white (HDR) input inverts to a negative value, unclamped");
  }
  {
    // Involution: applying linear-domain invert twice recovers the
    // original value exactly, even for HDR headroom above 1.0 -- the
    // property that would break if the op ever clamped its output.
    InvertParams p;
    p.amount = 1.0f;
    const std::array<float, 3> rgb{2.5f, 2.5f, 2.5f};
    const std::array<float, 3> once = applyInvert(rgb, p);
    const std::array<float, 3> twice = applyInvert(once, p);
    check(nearRgb(twice, rgb, kTol), "invert: linear domain is an involution, including for HDR input");
  }
  {
    // Display domain, hand-computed: input=0.5.
    //   srgbEncode(0.5) = 0.7353569830524495
    //   1.0 - encoded    = 0.26464301694755055
    //   srgbDecode(...)  = 0.05693648175459704
    InvertParams p;
    p.domain = InvertParams::Domain::Display;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyInvert({0.5f, 0.5f, 0.5f}, p);
    check(near(out[0], 0.05693648f, kTol), "invert: display domain hand-computed case matches");
  }
  {
    // Display domain HDR: srgbEncode is unclamped and monotonic, so an
    // above-1.0 linear input encodes to an above-1.0 encoded value, and
    // 1 - encoded goes negative -- srgbDecode mirrors that through its own
    // sign(x)*f(|x|) policy rather than refusing it. Hand-computed:
    // input=2.0 -> srgbEncode=1.3532560461493863 -> 1-that=-0.3532560...
    // -> srgbDecode(-0.3532560...) = -0.10243123227440179.
    InvertParams p;
    p.domain = InvertParams::Domain::Display;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyInvert({2.0f, 2.0f, 2.0f}, p);
    check(near(out[0], -0.10243123f, kTol),
          "invert: display domain, an HDR input inverts to a negative value too, unclamped");
  }
  {
    // Involution, display domain: same property, other domain.
    InvertParams p;
    p.domain = InvertParams::Domain::Display;
    p.amount = 1.0f;
    const std::array<float, 3> rgb{2.0f, 2.0f, 2.0f};
    const std::array<float, 3> once = applyInvert(rgb, p);
    const std::array<float, 3> twice = applyInvert(once, p);
    check(nearRgb(twice, rgb, kTol), "invert: display domain is an involution too, including for HDR input");
  }
  {
    // The domain field is real and visible: the same input, same amount,
    // different domain, gives genuinely different results. Hand-computed:
    // input=0.3, linear -> 0.7; display -> srgbDecode(1 - srgbEncode(0.3))
    // = srgbDecode(1 - 0.5838314900602575) = srgbDecode(0.4161685...) =
    // 0.1444831208854809.
    InvertParams lin;
    lin.amount = 1.0f;
    InvertParams disp;
    disp.domain = InvertParams::Domain::Display;
    disp.amount = 1.0f;
    const std::array<float, 3> rgb{0.3f, 0.3f, 0.3f};
    const std::array<float, 3> linOut = applyInvert(rgb, lin);
    const std::array<float, 3> dispOut = applyInvert(rgb, disp);
    check(near(linOut[0], 0.7f, kTol), "invert: domain=Linear hand-computed at input=0.3");
    check(near(dispOut[0], 0.1444831f, kTol), "invert: domain=Display hand-computed at the same input=0.3");
    check(!near(linOut[0], dispOut[0], kTol),
          "invert: linear and display domains genuinely disagree on the same input, as the spec requires");
  }

  // ===================================================================
  // 3. Posterize
  // ===================================================================
  {
    // Identity default (levels=0): exact, per ToneOps.hpp's doc comment --
    // "zero representable levels" is defined as pass-through, not merely
    // approximately close to it.
    const PosterizeParams neutral{};
    const std::array<float, 3> rgb{-0.4f, 0.3f, 2.7f};
    const std::array<float, 3> out = applyPosterize(rgb, neutral);
    check(out[0] == rgb[0] && out[1] == rgb[1] && out[2] == rgb[2],
          "posterize: levels=0 is an exact identity, incl. negative and HDR input");
  }
  {
    // levels=1: every input collapses to the SAME single output value,
    // hand-computable as shaperDecode(0.0f) -- ToneOps.hpp's doc comment
    // works out shaperDecode(0) = (0 - kOffsetB)/kSlopeA ≈ -0.00691688.
    PosterizeParams p;
    p.levels = 1;
    const std::array<float, 3> a = applyPosterize({0.1f, 0.1f, 0.1f}, p);
    const std::array<float, 3> b = applyPosterize({0.9f, 0.9f, 0.9f}, p);
    check(nearRgb(a, b, kTol), "posterize: levels=1 collapses two different inputs to the same output");
    check(near(a[0], -0.00691688f, kTol), "posterize: levels=1 hand-computed constant matches shaperDecode(0)");
  }
  {
    // Hand-computed, levels=2, input=0.3 (all three channels, since
    // posterize applies per-channel independently with a shared level
    // count):
    //   shaperEncode(0.3) = 0.4556526487348056
    //   step = 1/(2-1) = 1.0
    //   q = round(0.4556526487348056/1.0)*1.0 = 0.0
    //   shaperDecode(0.0) = -0.006916877586898862
    PosterizeParams p;
    p.levels = 2;
    const std::array<float, 3> out = applyPosterize({0.3f, 0.3f, 0.3f}, p);
    check(nearRgb(out, {-0.00691688f, -0.00691688f, -0.00691688f}, kTol),
          "posterize: levels=2 hand-computed case matches");
  }
  {
    // Direction: more levels means finer quantisation -- the distance
    // between output and input shrinks monotonically as levels grows.
    // Hand-computed at input=0.3: levels=4 error ≈0.2321, levels=16
    // error ≈0.0429, levels=64 error ≈0.0175.
    auto errAt = [&](int levels) {
      PosterizeParams p;
      p.levels = levels;
      const std::array<float, 3> out = applyPosterize({0.3f, 0.3f, 0.3f}, p);
      return std::fabs(out[0] - 0.3f);
    };
    const float err4 = errAt(4);
    const float err16 = errAt(16);
    const float err64 = errAt(64);
    check(err4 > err16 && err16 > err64,
          "posterize: increasing levels monotonically shrinks the quantisation error (finer bands)");
  }
  {
    // Quantising in the shaper domain, not linear, is the whole point of
    // this op (docs/operations.md: "or it bands unevenly") -- proven by a
    // hand-computed case where the shaper-domain answer and a naive
    // linear-domain answer visibly disagree. levels=3, input=1.0:
    //   shaperEncode(1.0) = 0.5547945205479452
    //   step = 1/(3-1) = 0.5
    //   q = round(0.5547945205479452/0.5)*0.5 = round(1.1095890...)*0.5
    //     = 1*0.5 = 0.5
    //   shaperDecode(0.5) = 0.5140569133280329
    // A bug that skipped the shaper round-trip and quantised 1.0 directly
    // in linear with the same step=0.5 would instead land exactly on the
    // input (1.0 is already a step=0.5 multiple), which is why this case
    // is sensitive to exactly the mistake docs/operations.md warns about.
    PosterizeParams p;
    p.levels = 3;
    const std::array<float, 3> out = applyPosterize({1.0f, 1.0f, 1.0f}, p);
    check(nearRgb(out, {0.5140569f, 0.5140569f, 0.5140569f}, kTol),
          "posterize: levels=3 hand-computed case proves the shaper round-trip runs, not a bare linear quantise");
  }

  // ===================================================================
  // 4. Threshold
  // ===================================================================
  {
    // Threshold quantises in the shaper domain too (this file's own doc
    // comment: Threshold is Posterize's levels==2 case), and this specific
    // luma value is where that choice is visible: raw linear luma=0.5 is
    // >= the default threshold=0.5 (would classify white), but
    // shaperEncode(0.5) = 0.497716894977169 is strictly BELOW 0.5 -- the
    // shaper's log curve dips slightly under the linear value here. A bug
    // that compared raw linear luma against `threshold` instead of running
    // it through shaperEncode() first would flip this case to white.
    ThresholdParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyThreshold({0.5f, 0.5f, 0.5f}, p);
    check(nearRgb(out, {0.0f, 0.0f, 0.0f}, kTol),
          "threshold: shaper-domain comparison at luma=0.5 is black, where a raw-linear comparison would be white");
  }
  {
    // `amount = 0` is an exact identity, per ToneOps.hpp's doc comment -- and
    // again deliberately not the default, for the reason the Invert section
    // above states at length.
    ThresholdParams off;
    off.amount = 0.0f;
    const std::array<float, 3> rgb{-0.4f, 0.3f, 2.7f};
    const std::array<float, 3> out = applyThreshold(rgb, off);
    check(out[0] == rgb[0] && out[1] == rgb[1] && out[2] == rgb[2],
          "threshold: amount=0 is an exact identity, incl. negative and HDR input");

    // A default-constructed Threshold must actually split. Grey 0.8 sits well
    // above the 0.5 shaper-domain default threshold, so it must come back
    // white -- not 0.8.
    const std::array<float, 3> byDefault = applyThreshold({0.8f, 0.8f, 0.8f}, ThresholdParams{});
    check(nearRgb(byDefault, {1.0f, 1.0f, 1.0f}, kTol),
          "threshold: a DEFAULT-constructed Threshold splits rather than passing through");
  }
  {
    // Hand-computed: rgb=(0.8,0.8,0.8), luma=0.8 (Rec.709 weights sum to
    // 1, so a grey pixel's luma equals its channel value), shaperEncode
    // (0.8) = 0.5364196292872511, which is >= the default threshold=0.5 --
    // the inclusive "at/above -> white" side.
    ThresholdParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyThreshold({0.8f, 0.8f, 0.8f}, p);
    check(nearRgb(out, {1.0f, 1.0f, 1.0f}, kTol),
          "threshold: an above-threshold luma (hand-computed) maps to white");
  }
  {
    // Hand-computed: rgb=(0.05,0.05,0.05), luma=0.05, shaperEncode(0.05) =
    // 0.308109127004146, strictly below the default threshold=0.5.
    ThresholdParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyThreshold({0.05f, 0.05f, 0.05f}, p);
    check(nearRgb(out, {0.0f, 0.0f, 0.0f}, kTol),
          "threshold: a below-threshold luma (hand-computed) maps to black");
  }
  {
    // Inclusive boundary: shaped luma exactly equal to `threshold` maps to
    // white ("at/above" per ToneOps.hpp's doc comment), not black.
    ThresholdParams p;
    p.amount = 1.0f;
    p.threshold = 0.5364196292872511f;  // == shaperEncode(0.8), hand-computed above
    const std::array<float, 3> out = applyThreshold({0.8f, 0.8f, 0.8f}, p);
    check(nearRgb(out, {1.0f, 1.0f, 1.0f}, kTol),
          "threshold: shaped luma exactly at the threshold value is on the white (inclusive) side");
  }
  {
    // Direction/amount: 0.5 sits halfway between the untouched input and
    // the fully-thresholded result, same blend() the identity case relies
    // on -- proves `amount` is a real continuous control, not a bool in
    // disguise.
    ThresholdParams p;
    p.amount = 0.5f;
    const std::array<float, 3> rgb{0.8f, 0.8f, 0.8f};  // luma 0.8 -> white side
    const std::array<float, 3> out = applyThreshold(rgb, p);
    check(near(out[0], 0.9f, kTol), "threshold: amount=0.5 sits halfway between input (0.8) and white (1.0)");
  }
  {
    // Threshold is luma-based, not per-channel: an unbalanced colour whose
    // R channel alone would be "white" but whose luma is well below
    // threshold must still map to black overall (matching Grayscale's own
    // luma-replicated-to-RGB shape, per ToneOps.hpp's doc comment).
    // luma(0.9, 0.05, 0.05) = 0.2126*0.9 + 0.7152*0.05 + 0.0722*0.05
    //                       = 0.19134 + 0.03576 + 0.00361 = 0.23071,
    // shaperEncode of which is well below the default threshold 0.5.
    ThresholdParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyThreshold({0.9f, 0.05f, 0.05f}, p);
    check(nearRgb(out, {0.0f, 0.0f, 0.0f}, kTol),
          "threshold: a red-heavy but luma-dark pixel maps to black, not white -- it is luma-based");
  }

  std::printf("[selftest] tone-ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
