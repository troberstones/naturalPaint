#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/StrokeSession.hpp"
#include "brush/Dynamics.hpp"

namespace np {

// track10/feel: PaintCopilot §3.2 (arXiv:2605.20941)'s two pressure-feel
// contributions -- see this file's own declaration in app/SelfTest.hpp for
// the section summary, and brush/Dynamics.hpp's `EasingPreset` comment for
// the curve-vs-hard-code argument this section's first half exists to back
// up with numbers rather than only prose.
bool runPressureFeelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // ======================================================================
  // 1. The closed-form laws themselves, independent of the curve that
  //    samples them -- endpoints exact, and monotonic across the domain.
  // ======================================================================
  {
    check(pressureResponseRadiusNorm(0.0f) == 0.0f,
          "radius law: log(1+9p)/log(10) at p=0 is exactly 0.0 -- log(1)=0");
    check(pressureResponseRadiusNorm(1.0f) == 1.0f,
          "radius law: at p=1 is exactly 1.0 -- log(10)/log(10)=1");
    check(pressureResponseOpacityNorm(0.0f) == 0.0f,
          "opacity law: p^2.5 at p=0 is exactly 0.0");
    check(pressureResponseOpacityNorm(1.0f) == 1.0f,
          "opacity law: p^2.5 at p=1 is exactly 1.0");

    // The formula's own value at p=0.5, computed independently of this
    // codebase (a hand-derived literal, not read back from the function
    // under test) -- log10(5.5) and 0.5^2.5 respectively.
    constexpr float kLogAtHalf = 0.7403627f;    // log10(1 + 9*0.5) = log10(5.5)
    constexpr float kPowAtHalf = 0.1767767f;    // 0.5^2.5 = 0.25 * sqrt(0.5)
    check(nearf(pressureResponseRadiusNorm(0.5f), kLogAtHalf, 1e-5f),
          "radius law: matches the hand-derived log10(5.5) at p=0.5");
    check(nearf(pressureResponseOpacityNorm(0.5f), kPowAtHalf, 1e-5f),
          "opacity law: matches the hand-derived 0.5^2.5 at p=0.5");

    bool radiusMonotonic = true, opacityMonotonic = true;
    float prevR = -1.0f, prevO = -1.0f;
    for (int i = 0; i <= 1000; ++i) {
      const float p = static_cast<float>(i) / 1000.0f;
      const float r = pressureResponseRadiusNorm(p);
      const float o = pressureResponseOpacityNorm(p);
      if (r < prevR) radiusMonotonic = false;
      if (o < prevO) opacityMonotonic = false;
      prevR = r;
      prevO = o;
    }
    check(radiusMonotonic, "radius law: non-decreasing across 1001 samples of [0,1]");
    check(opacityMonotonic, "opacity law: non-decreasing across 1001 samples of [0,1]");
  }

  // ======================================================================
  // 2. The Curve built from each law (EasingPreset::LogTaper / PowerIn) --
  //    what linkContribution() would actually evaluate, via evalCurve().
  //    x=0.5 sits exactly on a knot (control points at k/8), so this checks
  //    the SPLINE against the closed form with no interpolation-error
  //    tolerance to justify -- the spline passes through its own knots
  //    exactly, by evalCurve()'s own contract (ops/PointOps.hpp).
  // ======================================================================
  {
    const Curve logTaper = easingCurve(EasingPreset::LogTaper);
    const Curve powerIn = easingCurve(EasingPreset::PowerIn);
    check(logTaper.size() == 9, "LogTaper: built from exactly 9 knots (k/8, k=0..8)");
    check(powerIn.size() == 9, "PowerIn: built from exactly 9 knots (k/8, k=0..8)");

    check(evalCurve(logTaper, 0.0f) == 0.0f && evalCurve(logTaper, 1.0f) == 1.0f,
          "LogTaper curve: endpoints are exactly 0 and 1, same as the Linear preset's");
    check(evalCurve(powerIn, 0.0f) == 0.0f && evalCurve(powerIn, 1.0f) == 1.0f,
          "PowerIn curve: endpoints are exactly 0 and 1, same as the Linear preset's");

    const float logMid = evalCurve(logTaper, 0.5f);
    const float powMid = evalCurve(powerIn, 0.5f);
    check(nearf(logMid, pressureResponseRadiusNorm(0.5f), 1e-5f),
          "LogTaper curve: value at the x=0.5 KNOT matches the closed form exactly (no spline "
          "interpolation error possible at a knot)");
    check(nearf(powMid, pressureResponseOpacityNorm(0.5f), 1e-5f),
          "PowerIn curve: value at the x=0.5 KNOT matches the closed form exactly");

    // "Actually non-linear" -- DERIVED, not guessed: the gap between the
    // curve's own midpoint and the LINEAR interpolant's midpoint (0.5,
    // since Linear's two control points are (0,0)-(1,1)) is exactly
    // |f(0.5) - 0.5|, computed from the closed form above. 0.24 and 0.32
    // are the derived gaps for the log and power laws respectively --
    // this checks the built curve reproduces a gap of that size, not an
    // arbitrarily chosen threshold.
    const float logGap = std::fabs(logMid - 0.5f);
    const float powGap = std::fabs(powMid - 0.5f);
    check(logGap > 0.2f,
          "LogTaper curve: midpoint differs from the LINEAR interpolant's 0.5 by >0.2 -- "
          "log10(5.5)-0.5 = 0.240 derived above, so this is a wide margin, not a coin flip");
    check(powGap > 0.3f,
          "PowerIn curve: midpoint differs from the LINEAR interpolant's 0.5 by >0.3 -- "
          "0.5-0.5^2.5 = 0.323 derived above");

    bool logMonotonic = true, powMonotonic = true;
    float prevLog = -1.0f, prevPow = -1.0f;
    for (int i = 0; i <= 2000; ++i) {
      const float x = static_cast<float>(i) / 2000.0f;
      const float lv = evalCurve(logTaper, x);
      const float pv = evalCurve(powerIn, x);
      // A tiny negative epsilon, not zero: this is a property of the
      // Hermite spline `evalCurve()` actually runs, not of the source
      // formula, and asserting exact non-decrease would be asserting
      // float rounding never happens rather than asserting monotonicity.
      if (lv < prevLog - 1e-6f) logMonotonic = false;
      if (pv < prevPow - 1e-6f) powMonotonic = false;
      prevLog = lv;
      prevPow = pv;
    }
    check(logMonotonic, "LogTaper curve: non-decreasing across 2001 samples -- the SPLINE, not "
                        "just its 9 knots, has no overshoot dip");
    check(powMonotonic, "PowerIn curve: non-decreasing across 2001 samples -- likewise");

    check(matchesPreset(logTaper, EasingPreset::LogTaper),
          "matchesPreset: LogTaper's own curve matches itself");
    check(matchesPreset(powerIn, EasingPreset::PowerIn),
          "matchesPreset: PowerIn's own curve matches itself");
    check(!matchesPreset(logTaper, EasingPreset::Linear) &&
              !matchesPreset(logTaper, EasingPreset::PowerIn),
          "matchesPreset: LogTaper's curve matches neither Linear nor PowerIn");
    check(!matchesPreset(easingCurve(EasingPreset::Linear), EasingPreset::LogTaper),
          "matchesPreset: Linear's own curve does not match LogTaper");
  }

  // ======================================================================
  // 3. Opt-in, not global: defaultBrushLinks() -- what every new brush
  //    starts with -- still resolves Linear. The new presets change no
  //    existing brush's behaviour until something actually selects one.
  // ======================================================================
  {
    const BrushLinkSet defaults = defaultBrushLinks();
    const size_t sizeLink = findLink(defaults, DynamicSource::Pressure, DynamicTarget::Size);
    const size_t flowLink = findLink(defaults, DynamicSource::Pressure, DynamicTarget::Flow);
    check(sizeLink != kNoLink && flowLink != kNoLink,
          "defaultBrushLinks: still carries the PRESSURE->Size and PRESSURE->Flow links");
    check(sizeLink != kNoLink && matchesPreset(defaults.links[sizeLink].curve, EasingPreset::Linear),
          "defaultBrushLinks: PRESSURE->Size is still Linear -- LogTaper is opt-in, not a "
          "new default, so this migration's own selftest promise (identical to the old "
          "0.25+0.75p formula) still holds byte for byte");
    check(flowLink != kNoLink && matchesPreset(defaults.links[flowLink].curve, EasingPreset::Linear),
          "defaultBrushLinks: PRESSURE->Flow is still Linear -- PowerIn is opt-in too");
  }

  // ======================================================================
  // 4. dynamicPressureEma() in isolation -- the pure step, fixed point,
  //    hand-workable literals, and geometric convergence toward a held
  //    input.
  // ======================================================================
  {
    check(dynamicPressureEma(0.5f, 0.5f) == 0.5f,
          "ema: a held-constant input is an exact fixed point (0.7x + 0.3x == x)");
    check(nearf(dynamicPressureEma(0.0f, 1.0f), 0.3f, 1e-6f),
          "ema: first step from 0 toward a held 1.0 is exactly 0.3 (the paper's own literal)");
    check(nearf(dynamicPressureEma(0.3f, 1.0f), 0.51f, 1e-6f),
          "ema: second step is 0.7*0.3 + 0.3*1.0 = 0.51, hand-workable");
    check(nearf(dynamicPressureEma(-1.0f, 2.0f), 0.3f, 1e-6f),
          "ema: out-of-range inputs are clamped to [0,1] first, same as linkContribution()'s "
          "own source clamp -- so this reads as dynamicPressureEma(0,1), not as extrapolation");

    // Repeated application toward a HELD target converges geometrically:
    // after n steps from 0 toward 1, the gap to the target is exactly
    // 0.7^n (by induction: gap(n) = 1 - value(n), value(n) = 0.7*value(n-1)
    // + 0.3, so gap(n) = 0.7*gap(n-1)). Checked at two points along the
    // way rather than only at the end, so a filter that overshot or
    // stalled partway would be caught, not just one that never moved.
    float smoothed = 0.0f;
    for (int step = 1; step <= 20; ++step) {
      smoothed = dynamicPressureEma(smoothed, 1.0f);
      if (step == 10) {
        // 1 - 0.7^10 = 0.97175...
        check(nearf(smoothed, 1.0f - std::pow(0.7f, 10.0f), 1e-4f),
              "ema: after 10 steps toward a held 1.0, matches 1-0.7^10 [derived, not measured]");
      }
    }
    check(nearf(smoothed, 1.0f, 1e-3f),
          "ema: after 20 steps toward a held 1.0, converged within 0.001 (0.7^20 ~ 0.0008)");
  }

  // ======================================================================
  // 5. StrokeSession::smoothPressure() -- the STATEFUL wrapper: latches on
  //    the first call after begin(), follows dynamicPressureEma() after
  //    that, and -- the assertion this whole section exists to make -- does
  //    NOT carry a smoothed value across an end()/begin() pair.
  // ======================================================================
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "pressure-feel");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));

    auto tip = [](float radius) {
      BrushTip t;
      t.radius = radius;
      t.hardness = 0.5f;
      t.flow = 0.5f;
      return t;
    };

    StrokeSession s;
    std::string err;
    check(s.begin(od, 1, tip(20.0f), Tool::Brush, &err),
          "smoothPressure: stroke A begins on the Pigment layer");

    const float a1 = s.smoothPressure(1.0f);
    check(a1 == 1.0f,
          "smoothPressure: stroke A's FIRST call returns the raw sample unchanged -- no "
          "manufactured ramp-up from a filter that has no history yet");
    const float a2 = s.smoothPressure(0.0f);
    check(nearf(a2, 0.7f, 1e-6f),
          "smoothPressure: stroke A's second call (raw drops to 0) is the plain EMA step from "
          "the first call's 1.0, exactly 0.7 -- proves smoothing is actually applied, not a "
          "pass-through");
    const float a3 = s.smoothPressure(0.0f);
    check(nearf(a3, 0.49f, 1e-6f),
          "smoothPressure: stroke A's third call continues the SAME recursion -- 0.7*0.7 = 0.49");
    s.end();

    // Stroke B: a fresh begin() on the SAME StrokeSession object. If the
    // per-stroke EMA state were not reset here, this first call would blend
    // 0.49 (stroke A's last smoothed value) with the new raw sample instead
    // of latching onto it -- 0.7*0.49 + 0.3*0.2 = 0.403, a value easily
    // distinguished from the correct 0.2.
    std::string err2;
    check(s.begin(od, 1, tip(20.0f), Tool::Brush, &err2),
          "smoothPressure: stroke B begins (reusing the same StrokeSession object)");
    const float b1 = s.smoothPressure(0.2f);
    check(b1 == 0.2f,
          "smoothPressure: stroke B's FIRST call returns ITS OWN raw sample exactly, 0.2 -- "
          "NOT a value blended with stroke A's leftover 0.49. This is the reset.");

    // The recursion still runs correctly from the new latch point, so the
    // reset did not also break ordinary smoothing within stroke B.
    const float b2 = s.smoothPressure(1.0f);
    check(nearf(b2, 0.7f * 0.2f + 0.3f * 1.0f, 1e-6f),
          "smoothPressure: stroke B's second call resumes the SAME recursion from ITS OWN "
          "latch, 0.7*0.2 + 0.3*1.0");
    s.end();

    // Convergence at the StrokeSession level too, not only for the pure
    // function above -- stroke C holds pressure at 1.0 for 20 calls after
    // an initial low latch and should end up close to it.
    std::string err3;
    check(s.begin(od, 1, tip(20.0f), Tool::Brush, &err3),
          "smoothPressure: stroke C begins");
    float c = s.smoothPressure(0.0f);
    check(c == 0.0f, "smoothPressure: stroke C's first call latches at its own raw 0.0");
    for (int i = 0; i < 20; ++i) c = s.smoothPressure(1.0f);
    check(nearf(c, 1.0f, 1e-3f),
          "smoothPressure: stroke C converges to a held 1.0 within 0.001 after 20 calls, same "
          "bound as the pure-function check above");
    s.end();
  }

  // ======================================================================
  // 6. REACHABILITY. A preset `easingCurve()` can build but no chip can
  //    select is a feature that exists and cannot be turned on -- the
  //    silent-no-op class (docs/reachability-audit.md). The LINK editor's
  //    chip row walks `easingPresetCount()`/`easingPresetAt()` rather than a
  //    literal count of its own (`ui/MacPaintUI.cpp`, drawLinkEditor's chip
  //    loop), so asserting the ENUMERATION is complete is asserting the row
  //    is complete. Dropping a preset from `kEasingPresetOrder` -- which is
  //    exactly what "add a preset, forget the UI" looks like -- reddens this.
  // ======================================================================
  {
    // Every preset value, written out here INDEPENDENTLY of the order array
    // under test. This list is the ground truth; if the enum grows, this
    // fails until someone adds the new value here AND to the order array.
    const EasingPreset all[] = {EasingPreset::Linear, EasingPreset::EaseOut, EasingPreset::SCurve,
                                EasingPreset::LogTaper, EasingPreset::PowerIn};
    const size_t allCount = sizeof(all) / sizeof(all[0]);

    check(easingPresetCount() == allCount,
          "reachability: the chip row enumerates exactly as many presets as EasingPreset has "
          "values -- 5");

    bool everyPresetReachable = true;
    for (size_t i = 0; i < allCount; ++i) {
      bool found = false;
      for (size_t j = 0; j < easingPresetCount(); ++j)
        if (easingPresetAt(j) == all[i]) found = true;
      if (!found) everyPresetReachable = false;
    }
    check(everyPresetReachable,
          "reachability: EVERY preset value appears in the enumeration the chip row walks -- "
          "none is buildable from code and selectable from nowhere");

    // ...and no duplicates, so the count above cannot be satisfied by one
    // preset listed twice while another is missing.
    bool allDistinct = true;
    for (size_t i = 0; i < easingPresetCount(); ++i)
      for (size_t j = i + 1; j < easingPresetCount(); ++j)
        if (easingPresetAt(i) == easingPresetAt(j)) allDistinct = false;
    check(allDistinct, "reachability: no preset is enumerated twice");

    // Each chip's label is non-empty and unique -- two chips reading the same
    // word would be two buttons the user cannot tell apart, which is a
    // different way for one of them to be unreachable in practice.
    bool namesUsable = true;
    for (size_t i = 0; i < easingPresetCount(); ++i) {
      const char* ni = easingPresetName(easingPresetAt(i));
      if (ni == nullptr || ni[0] == '\0') namesUsable = false;
      for (size_t j = i + 1; j < easingPresetCount(); ++j)
        if (std::strcmp(ni, easingPresetName(easingPresetAt(j))) == 0) namesUsable = false;
    }
    check(namesUsable, "reachability: every chip has a non-empty label, and no two share one");

    // And the chip actually DOES something distinguishable: clicking chip i
    // assigns easingCurve(preset i), which matchesPreset must then light for
    // i and for no other chip. That is the round trip the row depends on to
    // show which preset is active.
    bool roundTrips = true;
    for (size_t i = 0; i < easingPresetCount(); ++i) {
      const Curve c = easingCurve(easingPresetAt(i));
      for (size_t j = 0; j < easingPresetCount(); ++j) {
        const bool lit = matchesPreset(c, easingPresetAt(j));
        if (lit != (i == j)) roundTrips = false;
      }
    }
    check(roundTrips,
          "reachability: clicking chip i sets a curve that lights chip i and NO other -- the "
          "five presets are mutually distinguishable, not near-duplicates");

    check(easingPresetAt(easingPresetCount()) == EasingPreset::Linear &&
              easingPresetAt(9999) == EasingPreset::Linear,
          "reachability: an out-of-range index resolves to Linear, the identity shape -- "
          "unreachable through easingPresetCount(), but not undefined");
  }

  std::printf("[selftest] pressure feel %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
