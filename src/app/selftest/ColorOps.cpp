#include "app/selftest/Support.hpp"

#include "ops/ColorOps.hpp"

namespace np {

// ops/ColorOps (docs/operations.md §1.2 "Committed additions"): the four
// pure-math point ops extending ops/PointOps.hpp's family -- Hue/
// Saturation/Lightness, Vibrance, Colour balance, Photo filter. See
// SelfTest.hpp for the full breakdown. Pure CPU math throughout, matching
// runPointOpsTest()'s own headless-first-class status -- no PaintSim or GPU
// involvement anywhere in this function.
bool runColorOpsTest() {
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
  auto dist = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };

  // ===================== Hue / Saturation / Lightness =====================
  {
    // Default-constructed params (hueDegrees=0, saturation=1, lightness=0,
    // colorize=false) is an exact identity -- including for a negative and
    // an HDR (>1) component, since none of the internal steps clamp on this
    // path (rotateAroundAxis(dev, k, 0) returns dev unchanged; the
    // saturation=1 applySaturation() call is itself a near-identity;
    // lightness=0 skips the shaper round trip entirely).
    const HueSaturationParams neutral{};
    const std::array<float, 3> rgb{-0.3f, 0.6f, 1.8f};
    check(nearRgb(applyHueSaturation(rgb, neutral), rgb, kTol),
          "hueSat: default params is an exact identity, including negative and HDR components");
  }
  {
    // Hand-computed (Rodrigues' rotation about normalize(kRec709LumaWeights),
    // applied to pure red's deviation from its own luma): red rotated 90
    // degrees lands at this exact triple, not just "somewhere else".
    const std::array<float, 3> red{1.0f, 0.0f, 0.0f};
    HueSaturationParams p;
    p.hueDegrees = 90.0f;
    const std::array<float, 3> expected{0.03023732f, 0.34873527f, -0.59894625f};
    check(nearRgb(applyHueSaturation(red, p), expected, kTol),
          "hueSat: pure red rotated +90 degrees lands at the hand-computed triple");
    // Luma is exactly preserved by this rotation, for any angle -- proven
    // algebraically in ColorOps.hpp (w . dev == 0 identically, and a
    // rotation about an axis parallel to w preserves w . v).
    check(near(computeLuma(applyHueSaturation(red, p)), computeLuma(red), kTol),
          "hueSat: hue rotation preserves computeLuma() exactly, even at 90 degrees");
  }
  {
    // 360 degrees must round-trip to the identity (cos(2*pi)=1, sin(2*pi)=0
    // to float tolerance), checked on an arbitrary colour that is itself
    // both negative in one channel and HDR in another -- not just a
    // primary, and not just an in-range value.
    const std::array<float, 3> rgb{0.37f, -0.2f, 1.8f};
    HueSaturationParams p;
    p.hueDegrees = 360.0f;
    check(nearRgb(applyHueSaturation(rgb, p), rgb, kTol),
          "hueSat: a 360-degree hue rotation round-trips to the identity");
  }
  {
    // Saturation must be consistent with applySaturation(): with
    // hueDegrees=0 (so the hue-rotate step is a no-op) and colorize=false,
    // applyHueSaturation(rgb, {saturation=2}) must equal
    // applySaturation(rgb, {scale=2}) exactly, not merely resemble it --
    // it is the same function call under the hood.
    const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
    HueSaturationParams p;
    p.saturation = 2.0f;
    const std::array<float, 3> viaHueSat = applyHueSaturation(rgb, p);
    const std::array<float, 3> viaSaturation = applySaturation(rgb, SaturationParams{2.0f});
    check(nearRgb(viaHueSat, viaSaturation, kTol),
          "hueSat: saturation half matches applySaturation() exactly at hueDegrees=0");
  }
  {
    // Lightness direction: a positive shift brightens (shaper-domain
    // add), a negative shift darkens -- and the exact value is hand-
    // computed by composing the same published shaperEncode/shaperDecode
    // primitives runShaperTest() already pins elsewhere, not by re-trusting
    // applyHueSaturation()'s own internals.
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    HueSaturationParams brighten;
    brighten.lightness = 0.3f;
    const float expectedBright = shaperDecode(shaperEncode(0.5f) + 0.3f);
    check(nearRgb(applyHueSaturation(grey, brighten), {expectedBright, expectedBright, expectedBright},
                  kTol),
          "hueSat: +lightness matches shaperEncode/add/shaperDecode composed by hand");
    check(expectedBright > 0.5f, "hueSat: positive lightness brightens a mid-grey");

    HueSaturationParams darken;
    darken.lightness = -0.3f;
    const std::array<float, 3> out = applyHueSaturation(grey, darken);
    check(out[0] < 0.5f, "hueSat: negative lightness darkens a mid-grey");
  }
  {
    // Colorize: two pixels with DIFFERENT starting luma, colorized with the
    // SAME hue/saturation, must (a) each keep their OWN luma exactly, and
    // (b) share the exact same chroma offset from that luma -- "everything
    // maps to one hue" proven as an equality between two independent
    // pixels, not just eyeballed.
    HueSaturationParams p;
    p.colorize = true;
    p.colorizeHueDegrees = 200.0f;
    p.colorizeSaturation = 0.6f;
    const std::array<float, 3> px1{0.3f, 0.6f, 0.2f};
    const std::array<float, 3> px2{0.1f, 0.1f, 0.9f};
    const std::array<float, 3> out1 = applyHueSaturation(px1, p);
    const std::array<float, 3> out2 = applyHueSaturation(px2, p);
    check(near(computeLuma(out1), computeLuma(px1), kTol), "hueSat: colorize keeps pixel 1's own luma");
    check(near(computeLuma(out2), computeLuma(px2), kTol), "hueSat: colorize keeps pixel 2's own luma");
    const float luma1 = computeLuma(out1), luma2 = computeLuma(out2);
    const std::array<float, 3> offset1{out1[0] - luma1, out1[1] - luma1, out1[2] - luma1};
    const std::array<float, 3> offset2{out2[0] - luma2, out2[1] - luma2, out2[2] - luma2};
    check(nearRgb(offset1, offset2, kTol),
          "hueSat: colorize gives two different-luma pixels the identical chroma offset");
    // And the hand-computed value for pixel 1 specifically.
    const std::array<float, 3> expected1{0.10081464f, 0.59927059f, 0.79374629f};
    check(nearRgb(out1, expected1, kTol), "hueSat: colorize hand-computed case matches");
  }

  // ============================= Vibrance =================================
  {
    check(nearRgb(applyVibrance({0.2f, 0.6f, 0.9f}, VibranceParams{}), {0.2f, 0.6f, 0.9f}, kTol),
          "vibrance: amount=0 is identity");
  }
  {
    // The ordering the op exists to produce: a grey moves less than a
    // pastel, which moves less than an already-saturated colour -- at a
    // fixed, shared amount. Distances are computed at runtime, not
    // hand-derived, since the point is the ordering, not the magnitude.
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> pastel{0.9f, 0.8f, 0.8f};
    const std::array<float, 3> saturated{0.9f, 0.1f, 0.1f};
    VibranceParams p;
    p.amount = 0.8f;
    const float moveGrey = dist(applyVibrance(grey, p), grey);
    const float movePastel = dist(applyVibrance(pastel, p), pastel);
    const float moveSaturated = dist(applyVibrance(saturated, p), saturated);
    check(moveGrey == 0.0f, "vibrance: a true grey never moves at all (zero deviation from its luma)");
    check(moveGrey < movePastel, "vibrance: grey moves less than a pastel");
    check(movePastel < moveSaturated, "vibrance: a pastel moves less than an already-saturated colour");

    // The defining property, not merely the ordering above (which a flat,
    // unweighted saturation scale would also satisfy, since deviation
    // itself grows with existing saturation): at the SAME nominal amount,
    // an already-saturated pixel must move LESS under vibrance's own
    // existing-saturation weighting than a flat, unweighted saturation
    // boost of that same nominal amount would.
    const float naiveMove = dist(applySaturation(saturated, SaturationParams{1.0f + p.amount}), saturated);
    check(moveSaturated < naiveMove,
          "vibrance: an already-saturated colour moves less than a flat unweighted boost of the same amount");
  }
  {
    // HDR headroom: a grey above 1.0 has zero saturation measure regardless
    // of magnitude, so vibrance must leave it exactly alone -- not clamp it
    // down to [0,1].
    const std::array<float, 3> hdrGrey{1.5f, 1.5f, 1.5f};
    VibranceParams p;
    p.amount = 1.0f;
    const std::array<float, 3> out = applyVibrance(hdrGrey, p);
    check(nearRgb(out, hdrGrey, kTol), "vibrance: an HDR grey (1.5) passes through untouched, unclamped");
  }
  {
    // Direction: positive amount increases the max-min spread (more
    // saturated); negative amount decreases it.
    const std::array<float, 3> rgb{0.8f, 0.4f, 0.2f};
    const auto spread = [](const std::array<float, 3>& c) {
      return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
    };
    const float baseSpread = spread(rgb);
    VibranceParams up;
    up.amount = 0.6f;
    VibranceParams down;
    down.amount = -0.6f;
    check(spread(applyVibrance(rgb, up)) > baseSpread, "vibrance: positive amount increases the spread");
    check(spread(applyVibrance(rgb, down)) < baseSpread, "vibrance: negative amount decreases the spread");
  }

  // =========================== Colour balance ==============================
  {
    // Default params is an exact identity, including a negative component
    // and an HDR component simultaneously -- every push is zero, so gain=1,
    // offset=0 on every channel, and midtonesGamma=0 keeps every channel on
    // the unclamped skip-branch (see ColorOps.hpp's doc comment for why
    // that branch exists). preserveLuminosity's own rescale is a no-op here
    // since output luma already equals input luma exactly.
    const std::array<float, 3> rgb{-0.3f, 1.7f, 0.5f};
    check(nearRgb(applyColorBalance(rgb, ColorBalanceParams{}), rgb, kTol),
          "colorBalance: default params is an exact identity, including negative and HDR components");
  }
  {
    // Shadows lift: hand-computed. rgb=(0.1,0.1,0.1), luma=0.1,
    // w_shadow=clamp(1-0.2,0,1)=0.8, offset_R=0.2*0.8=0.16, out_R=0.1+0.16=0.26.
    ColorBalanceParams p;
    p.shadowsLift = {0.2f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({0.1f, 0.1f, 0.1f}, p);
    check(near(out[0], 0.26f, kTol), "colorBalance: shadows lift hand-computed case matches");
  }
  {
    // Highlights gain: hand-computed. rgb=(0.9,0.9,0.9), luma=0.9,
    // w_highlight=clamp(1.8-1,0,1)=0.8, gain_R=1+0.5*0.8=1.4, out_R=0.9*1.4=1.26.
    ColorBalanceParams p;
    p.highlightsGain = {0.5f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({0.9f, 0.9f, 0.9f}, p);
    check(near(out[0], 1.26f, kTol), "colorBalance: highlights gain hand-computed case matches");
  }
  {
    // Midtones gamma: hand-computed. rgb=(0.5,0.5,0.5), luma=0.5,
    // w_midtone=1.0, gammaDenom_R=1+1.0*1.0=2.0, invPower=0.5,
    // out_R=pow(0.5,0.5)=sqrt(0.5).
    ColorBalanceParams p;
    p.midtonesGamma = {1.0f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({0.5f, 0.5f, 0.5f}, p);
    check(near(out[0], 0.70710678f, kTol), "colorBalance: midtones gamma hand-computed case matches");
  }
  {
    // The same control at an OFF-CENTER luma, so this pins w_midtone's
    // actual tent SHAPE, not just its peak value of 1.0 at luma=0.5 (which
    // a broken "w_midtone is always 1" implementation would satisfy just as
    // well as the real remainder formula -- exactly the gap the case above
    // does not close). Hand-computed: rgb=(0.05,0.05,0.05), luma=0.05,
    // w_shadow=clamp(1-0.1,0,1)=0.9, w_highlight=0, w_midtone=1-0.9-0=0.1,
    // gammaDenom_R=1+2.0*0.1=1.2, invPower=1/1.2, out_R=pow(0.05,1/1.2).
    ColorBalanceParams p;
    p.midtonesGamma = {2.0f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({0.05f, 0.05f, 0.05f}, p);
    check(near(out[0], 0.08237745f, kTol),
          "colorBalance: midtones gamma at an off-center luma pins w_midtone's actual shape");
  }
  {
    // The negative-base guard before pow(), mirroring Levels' own NaN-guard
    // test in app/selftest/PointOps.cpp: a strong negative shadows lift at a
    // luma where BOTH shadows and midtones weight is nonzero (luma=0.25)
    // pushes `base` negative while a nonzero midtonesGamma still takes the
    // pow() branch -- exactly the combination required to hit a negative
    // base with a fractional exponent. Hand-computed: rgb=(0.25,0.25,0.25),
    // w_shadow=0.5, w_midtone=0.5, offset_R=-1.0*0.5=-0.5,
    // base_R=0.25-0.5=-0.25 (negative), gammaDenom_R=1+0.5*0.5=1.25 -- the
    // clamp floors that -0.25 to 0 before pow(), giving exactly 0.
    ColorBalanceParams p;
    p.shadowsLift = {-1.0f, 0.0f, 0.0f};
    p.midtonesGamma = {0.5f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({0.25f, 0.25f, 0.25f}, p);
    check(!std::isnan(out[0]), "colorBalance: a negative base before pow() does not produce NaN");
    check(near(out[0], 0.0f, kTol), "colorBalance: negative-base case clamps to the hand-computed 0");
  }
  {
    // Non-double-counting at the extremes: at pure black (luma=0),
    // w_highlight is exactly 0, so a highlights-only push must leave the
    // pixel completely untouched; symmetrically at pure white (luma=1),
    // w_shadow is exactly 0, so a shadows-only push must do nothing.
    ColorBalanceParams highlightOnly;
    highlightOnly.highlightsGain = {0.9f, 0.9f, 0.9f};
    highlightOnly.preserveLuminosity = false;
    check(nearRgb(applyColorBalance({0.0f, 0.0f, 0.0f}, highlightOnly), {0.0f, 0.0f, 0.0f}, kTol),
          "colorBalance: highlights gain has zero effect at pure black (w_highlight=0)");

    ColorBalanceParams shadowOnly;
    shadowOnly.shadowsLift = {0.9f, 0.9f, 0.9f};
    shadowOnly.preserveLuminosity = false;
    check(nearRgb(applyColorBalance({1.0f, 1.0f, 1.0f}, shadowOnly), {1.0f, 1.0f, 1.0f}, kTol),
          "colorBalance: shadows lift has zero effect at pure white (w_shadow=0)");
  }
  {
    // Preserve luminosity: an asymmetric per-channel lift changes each
    // channel differently, but with preserveLuminosity on, the RESULT's
    // luma must match the INPUT's luma exactly (to tolerance) -- and the
    // un-preserved twin must NOT match, proving the flag actually does
    // something rather than the rescale being a no-op.
    ColorBalanceParams p;
    p.shadowsLift = {0.3f, -0.1f, 0.05f};
    p.preserveLuminosity = true;
    const std::array<float, 3> rgb{0.2f, 0.2f, 0.2f};
    const std::array<float, 3> preserved = applyColorBalance(rgb, p);
    check(near(computeLuma(preserved), computeLuma(rgb), kTol),
          "colorBalance: preserveLuminosity keeps output luma equal to input luma");

    ColorBalanceParams pUnpreserved = p;
    pUnpreserved.preserveLuminosity = false;
    const std::array<float, 3> unpreserved = applyColorBalance(rgb, pUnpreserved);
    check(!near(computeLuma(unpreserved), computeLuma(rgb), kTol),
          "colorBalance: without the flag, luma actually does drift (the flag is not a no-op)");
  }
  {
    // Above 1.0: an HDR grey (1.5) still gets a full-weight highlights push
    // (w_highlight saturates to 1 at luma>=1, per the tent functions'
    // definition for all real L), landing at exactly 3.0, not clamped down.
    ColorBalanceParams p;
    p.highlightsGain = {1.0f, 0.0f, 0.0f};
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyColorBalance({1.5f, 1.5f, 1.5f}, p);
    check(near(out[0], 3.0f, kTol), "colorBalance: HDR highlight push is unclamped (1.5 -> 3.0)");
  }

  // ============================= Photo filter ===============================
  {
    // Default params (color=(1,1,1), density=0) is an exact identity,
    // including a negative component and an HDR component together.
    const std::array<float, 3> rgb{-0.2f, 2.0f, 0.5f};
    check(nearRgb(applyPhotoFilter(rgb, PhotoFilterParams{}), rgb, kTol),
          "photoFilter: default params is an exact identity, including negative and HDR components");
  }
  {
    // A real (non-neutral) filter at full density darkens overall luma,
    // when NOT preserving luminosity -- hand-computed: grey (0.5,0.5,0.5)
    // through an orange filter (1.0,0.6,0.2) at density=1 is exactly
    // (0.5, 0.3, 0.1), luma 0.328 < the original 0.5.
    PhotoFilterParams p;
    p.color = {1.0f, 0.6f, 0.2f};
    p.density = 1.0f;
    p.preserveLuminosity = false;
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> out = applyPhotoFilter(grey, p);
    check(nearRgb(out, {0.5f, 0.3f, 0.1f}, kTol), "photoFilter: full-density hand-computed case matches");
    check(computeLuma(out) < computeLuma(grey),
          "photoFilter: a real filter colour at full density darkens overall luma without preserveLuminosity");
  }
  {
    // The same filter with preserveLuminosity on restores the original
    // luma exactly, while keeping the colour cast (still hand-computed).
    PhotoFilterParams p;
    p.color = {1.0f, 0.6f, 0.2f};
    p.density = 1.0f;
    p.preserveLuminosity = true;
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> out = applyPhotoFilter(grey, p);
    check(near(computeLuma(out), computeLuma(grey), kTol),
          "photoFilter: preserveLuminosity restores the pre-filter luma exactly");
    check(nearRgb(out, {0.76200927f, 0.45720556f, 0.15240185f}, kTol),
          "photoFilter: preserveLuminosity hand-computed case matches");
    check(out[0] > out[1] && out[1] > out[2], "photoFilter: the warm colour cast survives the rescale");
  }
  {
    // Above 1.0: the same orange filter applied to an HDR grey (2.0) is
    // unclamped -- exactly (2.0, 1.2, 0.4), not floored to [0,1].
    PhotoFilterParams p;
    p.color = {1.0f, 0.6f, 0.2f};
    p.density = 1.0f;
    p.preserveLuminosity = false;
    const std::array<float, 3> out = applyPhotoFilter({2.0f, 2.0f, 2.0f}, p);
    check(nearRgb(out, {2.0f, 1.2f, 0.4f}, kTol), "photoFilter: HDR input is unclamped (2.0 -> 1.2, 0.4)");
  }
  {
    // Density is a blend fraction, not a brightness multiplier: the filter
    // colour's own R channel is 1.0 (no push there), so R is untouched at
    // ANY density; G (filter component 0.6) strictly decreases as density
    // rises from 0 toward 1.
    PhotoFilterParams half;
    half.color = {1.0f, 0.6f, 0.2f};
    half.density = 0.5f;
    half.preserveLuminosity = false;
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> out = applyPhotoFilter(grey, half);
    check(near(out[0], 0.5f, kTol), "photoFilter: a filter channel of 1.0 leaves that channel untouched");
    check(out[1] > 0.3f && out[1] < 0.5f,
          "photoFilter: density=0.5 lands strictly between the untouched and fully-filtered G channel");
  }

  std::printf("[selftest] color ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
