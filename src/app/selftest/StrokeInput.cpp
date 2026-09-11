#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/StrokeSession.hpp"
#include "brush/Dynamics.hpp"
#include "brush/StrokePath.hpp"
#include "brush/Variance.hpp"

namespace np {
namespace {

bool closeTo(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Feeds a straight, evenly spaced pressure ramp -- `n` samples from
// `(x0, y)` to `(x1, y)`, pressure linearly interpolated from `p0` to `p1`
// at each sample -- through a fresh `StrokePath`, exactly the way
// `app/selftest/StrokePath.cpp`'s own `walkLine()` drives positions alone.
// `n` stands in for sample RATE at a fixed path: few samples is what a slow
// tablet or a mouse hands this class, many is what a fast one does.
std::vector<StrokeDab> feedStraightRamp(int n, float x0, float x1, float y, float p0, float p1,
                                        float spacingPx) {
  StrokePath path;
  path.reset();
  std::vector<StrokeDab> dabs;
  for (int i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(n - 1);
    StrokeSample s;
    s.pos = Vec2{x0 + (x1 - x0) * t, y};
    s.pressure = p0 + (p1 - p0) * t;
    path.addPoint(s, spacingPx, dabs);
  }
  path.flush(spacingPx, dabs);
  return dabs;
}

}  // namespace

// Track A: full-rate pointer input and per-dab axis interpolation.
//
// `brush/StrokePath.hpp`'s own header used to say "feed it one new sample
// per render frame" -- true for POSITION (ADR-0003 never depended on how
// many frames a distance was divided into) and quietly false for every axis
// riding along with it, since `StrokeSession::depositPending()` used to seed
// every dab a frame emitted from ONE frame-latched `DynamicInputs`. A tablet
// reporting at 133-200 Hz into a 60 Hz frame could emit dozens of dabs a
// frame, all sharing one pressure/tilt/azimuth/barrel reading -- a brush
// with a PenPressure-controlled Size Variance stepped in blocks instead of
// tapering smoothly. This file asserts the fix: `brush/StrokePath`'s new
// `StrokeSample`/`StrokeDab` carry axes and interpolate them per dab,
// `StrokeSession::depositPending()` reads a dab's OWN axes rather than the
// frame's, and pressure smoothing is keyed to distance travelled rather than
// to how many render frames or raw samples that distance happened to be
// divided into.
bool runStrokeInputTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] stroke input: full-rate samples and per-dab axes\n");

  // ==========================================================================
  // 1. A straight segment fed as two samples, pressure 0 -> 1: dab pressure
  //    is strictly increasing and spans the OPEN interval (0, 1).
  // ==========================================================================
  {
    // spacingPx = 5 over a 200 px path: ~39 dabs, comfortably enough to
    // assert monotonicity rather than trust two endpoints alone.
    const std::vector<StrokeDab> dabs = feedStraightRamp(2, 0.0f, 200.0f, 50.0f, 0.0f, 1.0f, 5.0f);
    check(dabs.size() > 10, "setup: enough dabs to make monotonicity a real claim");

    bool increasing = dabs.size() > 1;
    for (size_t i = 1; i < dabs.size(); ++i) {
      if (dabs[i].pressure <= dabs[i - 1].pressure) increasing = false;
    }
    check(increasing, "two-sample rising segment: dab pressure is strictly increasing, dab to dab");

    // The span is the HALF-OPEN (0, 1], and the asymmetry is real rather
    // than sloppiness about the endpoints:
    //
    //  * strictly above 0: `StrokePath`'s walk emits its first dab ONE FULL
    //    SPACING in, never at the first sample itself (`app/selftest/
    //    StrokePath.cpp` section 5 asserts the identical thing for position),
    //    so no dab can carry the 0.0 the first sample held;
    //  * up to and INCLUDING 1: the far endpoint IS reachable, and this
    //    fixture reaches it exactly. The walk emits whenever the accumulated
    //    arc length crosses a spacing boundary, and `leftover_` starts at 0
    //    on a fresh path -- so a segment whose length is an exact integer
    //    multiple of the spacing puts its last dab exactly on its far
    //    endpoint. 200 px at 5 px spacing is 40 spacings exactly, which is
    //    why the measurement below reports the last dab at the last sample's
    //    own 1.0. A fixture with a non-multiple length would report just
    //    under it; neither is a defect, so the assertion admits both.
    std::printf("  [measured] two-sample 0->1 ramp over 200 px at 5 px spacing: %zu dabs, "
                "first pressure %.6f, last %.6f\n",
                dabs.size(), static_cast<double>(dabs.empty() ? 0.0f : dabs.front().pressure),
                static_cast<double>(dabs.empty() ? 0.0f : dabs.back().pressure));
    check(!dabs.empty() && dabs.front().pressure > 0.0f && dabs.front().pressure < 1.0f,
          "two-sample rising segment: first dab's pressure is strictly inside (0, 1)");
    check(!dabs.empty() && dabs.back().pressure <= 1.0f && dabs.back().pressure > 0.9f,
          "two-sample rising segment: last dab's pressure is in (0.9, 1] -- the ramp really is "
          "spanned, and the far endpoint is reachable but never exceeded");
  }

  // ==========================================================================
  // 2. Sample-RATE independence for pressure, the axis-carrying analogue of
  //    `app/selftest/StrokePath.cpp` section 6's own position claim. The SAME
  //    straight, evenly spaced ramp fed as 2 samples and as 20 (pressure
  //    linearly interpolated at each of the 20) must emit dabs at the same
  //    POSITIONS and at the same PRESSURES, both within a derived tolerance.
  // ==========================================================================
  {
    constexpr float kX0 = 0.0f, kX1 = 400.0f, kY = 120.0f;
    constexpr float kSpacingPx = 8.0f;
    const std::vector<StrokeDab> coarse = feedStraightRamp(2, kX0, kX1, kY, 0.0f, 1.0f, kSpacingPx);
    const std::vector<StrokeDab> fine = feedStraightRamp(20, kX0, kX1, kY, 0.0f, 1.0f, kSpacingPx);
    check(coarse.size() == fine.size(),
          "rate independence: 2 samples and 20 over the same ramp emit the same dab COUNT");

    // Positions: EXACT. Every segment of both runs -- including the first
    // and last, whose missing neighbour is mirrored -- sees four collinear
    // control points at one uniform spacing, so the centripetal knots are
    // uniform, the curve degenerates to the straight chord with `u` linear
    // along it, and the arc-length walk lands each dab on the identical
    // float. (`app/selftest/StrokePath.cpp` section 6 asserts the same
    // claim for 2 vs 41 samples under a 0.05 px bound; here it is held to
    // zero, which is what the brief for this test states and what the
    // measurement printed below shows.)
    //
    // Pressures: bounded by float ROUNDING alone, and the bound follows from
    // the same fact. Uniform knots make `u` equal the arc-length fraction of
    // its segment in both runs, and the fixture's sample pressures lie on
    // one global linear ramp -- so in exact arithmetic the two runs report
    // identical pressure at every (identical) dab position. The parameter
    // `u` and an arc-length fraction only part company on NON-uniform
    // sample spacing (brush/StrokePath.hpp's header on why `u` anyway), which
    // this fixture, by the brief's own specification, does not have.
    //
    // What is left is rounding in the few float operations between a
    // fixture's inputs and a dab's pressure: each fine-run endpoint pressure
    // `p0 + (p1-p0)*t` with `t = i/(n-1)` (3 ops), the chord-local parameter
    // `uAt = uPrev + t*(u-uPrev)` with `u = i/24` and `t = walked/edgeLen`
    // (5 ops), and the lerp `a + (b-a)*uAt` (3 ops). Every value is in
    // [0, 1], so each op's rounding is at most half an ULP of 1.0 (2^-24);
    // ~16 such contributions across the two runs is at most 8 ULPs of 1.0.
    // That -- 8 * 2^-23 -- is the bound, not a guess with a margin.
    constexpr float kPressureTol = 8.0f * 1.1920929e-7f;  // 8 ULP of 1.0f
    std::printf("  [derived] rate independence tolerances: position exact (0 px), pressure "
                "%.3g (8 ULP of 1.0)\n",
                static_cast<double>(kPressureTol));

    bool posOk = coarse.size() == fine.size();
    bool pressureOk = coarse.size() == fine.size();
    float maxPosErr = 0.0f, maxPressureErr = 0.0f;
    for (size_t i = 0; i < coarse.size() && i < fine.size(); ++i) {
      if (coarse[i].pos.x != fine[i].pos.x || coarse[i].pos.y != fine[i].pos.y) posOk = false;
      if (!closeTo(coarse[i].pressure, fine[i].pressure, kPressureTol)) pressureOk = false;
      maxPosErr = std::max({maxPosErr, std::fabs(coarse[i].pos.x - fine[i].pos.x),
                            std::fabs(coarse[i].pos.y - fine[i].pos.y)});
      maxPressureErr = std::max(maxPressureErr, std::fabs(coarse[i].pressure - fine[i].pressure));
    }
    // Printed so the margins above are evidence, not faith: the real
    // divergence against the bound it is asserted under.
    std::printf("  [measured] rate independence, %zu dabs: max |dpos| %.9g px, max |dpressure| "
                "%.9g\n",
                coarse.size(), static_cast<double>(maxPosErr), static_cast<double>(maxPressureErr));
    check(posOk, "rate independence: dab positions are bit-identical between 2 and 20 samples");
    check(pressureOk, "rate independence: dab pressures match within the derived tolerance above");
  }

  // ==========================================================================
  // 3. Distance-keyed pressure smoothing (brush/Dynamics.hpp's
  //    `dynamicPressureSmoothedByDistance()`): rate independence, and a
  //    constant-pressure stroke is a fixed point.
  // ==========================================================================
  {
    // 3a. Fixed point: a held-constant raw pressure equal to the previous
    // smoothed value must not move, at ANY distance -- `(1-a)*p + a*p == p`
    // for any `a`, exactly what the header's own formula reduces to. Float
    // rounding in computing `a = 1 - exp(-ds/k)` and its complement can move
    // the result by up to about one ULP of 0.5, hence the tight but nonzero
    // tolerance rather than `==`.
    bool fixedPoint = true;
    for (const float ds : {0.0f, 1.0f, 5.0f, 14.0f, 50.0f, 1000.0f}) {
      const float r = dynamicPressureSmoothedByDistance(0.5f, 0.5f, ds);
      if (!closeTo(r, 0.5f, 1e-6f)) fixedPoint = false;
    }
    check(fixedPoint, "distance smoothing: a constant-pressure stroke is a fixed point, any ds");

    // 3b. Rate independence: the SAME physical stroke -- a linear pressure
    // ramp along arc length, slope `m` -- sampled at 2x the rate (half the
    // step distance) must reach the same smoothed pressure at the same arc
    // length, within a tolerance derived from the filter's own closed form.
    //
    // For a linear target `p(s) = m*s` sampled at uniform step `ds`, the
    // discrete recursion `p~_n = (1-a)*p~_{n-1} + a*p(s_n)`, `a = 1 -
    // exp(-ds/k)`, has a STEADY-STATE lag (solved by assuming `p~_n =
    // p(s_n) - L` and requiring the recursion hold):
    //
    //   L(ds) = m * ds * (1 - a) / a
    //
    // which -> `m*k` as `ds -> 0` (the continuous-time exponential lag, the
    // standard result for a first-order low-pass fed a ramp) and differs
    // from that limit at finite `ds` by a term of order `ds`. Two sampling
    // rates therefore converge to steady states that differ by
    // `|L(dsCoarse) - L(dsFine)|` -- computed below from the SAME formula
    // the filter itself follows, not a hand-typed constant, so the bound is
    // exactly as trustworthy as the formula it is derived from. A checkpoint
    // well past the transient (>7 time constants from the ramp's start, where
    // the residual initial-condition term `exp(-s/k)` is below 1e-3) is where
    // the steady-state formula actually applies.
    auto steadyStateLag = [](float m, float k, float ds) {
      const float a = 1.0f - std::exp(-ds / k);
      return m * ds * (1.0f - a) / a;
    };
    auto simulateRamp = [](float ds, int steps, float m) {
      float smoothed = 0.0f;
      bool latched = false;
      for (int i = 1; i <= steps; ++i) {
        const float raw = m * (static_cast<float>(i) * ds);
        if (!latched) {
          smoothed = raw;
          latched = true;
        } else {
          smoothed = dynamicPressureSmoothedByDistance(smoothed, raw, ds);
        }
      }
      return smoothed;
    };

    constexpr float kM = 0.005f;  // pressure per px -- 1.0 reached at 200 px
    const float kK = kPressureSmoothingPx;
    constexpr float kDsCoarse = 10.0f, kDsFine = 5.0f;
    constexpr int kStepsCoarse = 15, kStepsFine = 30;  // both reach 150 px --
                                                       // 150/14 =~ 10.7 time
                                                       // constants, so the
                                                       // transient term above
                                                       // is below 1e-3
    const float smoothedCoarse = simulateRamp(kDsCoarse, kStepsCoarse, kM);
    const float smoothedFine = simulateRamp(kDsFine, kStepsFine, kM);
    const float lagCoarse = steadyStateLag(kM, kK, kDsCoarse);
    const float lagFine = steadyStateLag(kM, kK, kDsFine);
    // 50% margin over the steady-state-formula difference for the residual
    // transient (bounded above by ~1e-3 in pressure units, see the comment
    // above) and for the discrete simulation not being infinitely long.
    const float tol = std::fabs(lagCoarse - lagFine) * 1.5f + 1e-3f;
    std::printf("  [measured] rate independence -- coarse (ds=%.0f) smoothed = %.6f, "
                "fine (ds=%.0f) smoothed = %.6f, derived tolerance = %.6f\n",
                static_cast<double>(kDsCoarse), static_cast<double>(smoothedCoarse),
                static_cast<double>(kDsFine), static_cast<double>(smoothedFine),
                static_cast<double>(tol));
    check(smoothedCoarse < kM * 150.0f && smoothedFine < kM * 150.0f,
          "setup: both runs lag behind the raw ramp, as a low-pass must");
    check(closeTo(smoothedCoarse, smoothedFine, tol),
          "distance smoothing: 2x sample rate reaches the same smoothed pressure at the same arc "
          "length, within the derived tolerance");
  }

  // ==========================================================================
  // 4. A mouse sample produces the same DynamicInputs a mouse produces
  //    today -- bit-exact.
  // ==========================================================================
  {
    PointerSample mouseSample;
    mouseSample.x = 123.0f;
    mouseSample.y = 456.0f;
    mouseSample.isPen = false;
    // Deliberately garbage in the axis fields a real mouse event would never
    // populate (`queueMousePointerSample()` in main.cpp always leaves them
    // at their PointerSample defaults) -- proving `isPen == false` alone,
    // not "happens to already be zero", is what makes this an early return.
    mouseSample.pressure = 0.2f;
    mouseSample.tiltXDeg = 30.0f;
    mouseSample.tiltYDeg = -30.0f;
    mouseSample.rotationDeg = 90.0f;

    const StrokeSample ss = strokeSampleFromPointer(mouseSample, Vec2{123.0f, 456.0f});
    check(ss.pos.x == 123.0f && ss.pos.y == 456.0f,
          "mouse sample: position passes through unchanged");
    check(ss.pressure == 1.0f, "mouse sample: pressure 1.0, bit-exact -- ignores the garbage input");
    check(ss.tilt == 0.0f && ss.azimuth == 0.0f,
          "mouse sample: neutral tilt/azimuth, bit-exact -- ignores the garbage input");
    // 0.5, not `DynamicInputs{}`'s own bare 0.0 default -- `penBarrelNormalised(0)`'s rest
    // reading, which is what `dynamicInputsFor()` has always given a mouse
    // (`AppState::penBarrel`'s own default is 0.5, not 0.0 -- see that
    // field's comment). `StrokeSample`'s default matches THIS, not
    // `DynamicInputs`'s bare struct default, for the identical reason.
    check(ss.barrel == 0.5f, "mouse sample: barrel at the pen's rest reading (0.5), bit-exact -- "
                            "matches dynamicInputsFor()'s own mouse default, ignores garbage input");

    // The `has*` flags a real interactive mouse stroke reaches painting
    // through are unaffected by this whole track. `hardwareInputs_` is
    // latched once per frame at `begin()`/`setTip()`, now from
    // `strokeHardwareInputsFor(AppState)` rather than bare
    // `dynamicInputsFor()` -- and for a mouse the two must be the SAME
    // DynamicInputs, bit for bit; the sibling only ever adds flags for a pen
    // in contact. Asserted in two halves: `dynamicInputsFor()` itself is
    // untouched (mouse-only, never-`penSeen` AppState), and the sibling
    // agrees with it field by field on that AppState.
    AppState mouseOnly;
    const DynamicInputs mouseInputs = dynamicInputsFor(mouseOnly);
    check(mouseInputs.hasPressure && !mouseInputs.hasTilt && !mouseInputs.hasBarrel,
          "mouse sample: dynamicInputsFor()'s own has* flags are unchanged by this track "
          "(hasPressure, !hasTilt, !hasBarrel)");
    check(mouseInputs.pressure == 1.0f && mouseInputs.tilt == 0.0f && mouseInputs.azimuth == 0.0f &&
              mouseInputs.barrel == 0.5f,
          "mouse sample: dynamicInputsFor()'s own four values are unchanged by this track");
    const DynamicInputs mouseStroke = strokeHardwareInputsFor(mouseOnly);
    check(mouseStroke.pressure == mouseInputs.pressure && mouseStroke.tilt == mouseInputs.tilt &&
              mouseStroke.azimuth == mouseInputs.azimuth &&
              mouseStroke.barrel == mouseInputs.barrel &&
              mouseStroke.hasPressure == mouseInputs.hasPressure &&
              mouseStroke.hasTilt == mouseInputs.hasTilt &&
              mouseStroke.hasBarrel == mouseInputs.hasBarrel,
          "mouse stroke: strokeHardwareInputsFor() is bit-identical to dynamicInputsFor() for a "
          "mouse -- values and has* flags");

    // The pen half of "set the has* flags correctly for pen vs mouse"
    // (DynamicInputs' own comment: which axes the current device REPORTS).
    // Three cases, each the one a wrong rule would get wrong:
    //  * a pen in contact that has reported both axes -> both flags;
    //  * a pen in contact that never sent a rotation axis -> tilt only
    //    (a barrel Control must read identity, not the 0.5 rest value);
    //  * the same fully-reporting pen, but NOT in contact -- a mouse stroke
    //    made after the pen was put down -> mouse flags, or every Tilt
    //    Control would read a stale pen tilt on a mouse stroke.
    const auto penState = [](bool down, bool reportsTilt, bool reportsBarrel) {
      AppState st;
      st.penSeen = true;
      st.penDown = down;
      st.penReportsTilt = reportsTilt;
      st.penReportsBarrel = reportsBarrel;
      return strokeHardwareInputsFor(st);
    };
    const DynamicInputs penFullIn = penState(true, true, true);
    check(penFullIn.hasPressure && penFullIn.hasTilt && penFullIn.hasBarrel,
          "pen stroke: a pen in contact that reports tilt and barrel sets hasTilt and hasBarrel");
    const DynamicInputs penNoBarrelIn = penState(true, true, false);
    check(penNoBarrelIn.hasTilt && !penNoBarrelIn.hasBarrel,
          "pen stroke: a pen that never reported barrel rotation leaves hasBarrel false");
    const DynamicInputs penLiftedIn = penState(false, true, true);
    check(!penLiftedIn.hasTilt && !penLiftedIn.hasBarrel,
          "mouse stroke after pen use: pen not in contact reads a mouse's has* flags");
  }

  // ==========================================================================
  // 5. The old addPoint(x, y) wrapper and the new addSample() produce
  //    bit-identical dabs, end to end through a real StrokeSession -- not
  //    merely through StrokePath alone.
  //
  //    The stroke below begins with a default `DynamicInputs{}` latch, which
  //    is where the two routes are required to coincide: `addPoint()` builds
  //    its sample's axes from THAT latch (its own header comment argues why
  //    it must, and `app/selftest/ActiveLayer.cpp` guards the case where the
  //    latch is not neutral), and `addSample()` here is handed a default
  //    `StrokeSample`. Those two readings agree on pressure/tilt/azimuth and
  //    differ only in `barrel` -- 0.0 against `StrokeSample`'s own rest
  //    reading of 0.5 -- which `hasBarrel == false` makes unreadable
  //    downstream. So "bit-identical pixels" is a real claim about the
  //    geometry and deposition being one shared path, not two.
  // ==========================================================================
  {
    // 5a. StrokePath alone, bit-exact, on a CURVED path (a straight one would
    // make the Catmull-Rom fit trivially linear and prove less). The old
    // `Vec2` overloads against the `StrokeSample` ones fed neutral axes AND
    // fed deliberately wild axes: positions must match with `==` in both,
    // since axes ride along the walk and must never feed back into it.
    {
      constexpr int kN = 12;
      constexpr float kSpacing = 3.0f;
      const auto samplePos = [](int i) {
        const float a = static_cast<float>(i) * 0.4f;
        return Vec2{200.0f + 120.0f * std::cos(a), 200.0f + 80.0f * std::sin(a)};
      };
      StrokePath oldPath, neutralPath, wildPath;
      oldPath.reset();
      neutralPath.reset();
      wildPath.reset();
      std::vector<Vec2> oldDabs;
      std::vector<StrokeDab> neutralDabs, wildDabs;
      for (int i = 0; i < kN; ++i) {
        const Vec2 p = samplePos(i);
        oldPath.addPoint(p.x, p.y, kSpacing, oldDabs);
        neutralPath.addPoint(StrokeSample{p}, kSpacing, neutralDabs);
        const float w = static_cast<float>(i % 3) * 0.5f;
        wildPath.addPoint(StrokeSample{p, w, 1.0f - w, w * 0.3f, 0.9f - w}, kSpacing, wildDabs);
      }
      oldPath.flush(kSpacing, oldDabs);
      neutralPath.flush(kSpacing, neutralDabs);
      wildPath.flush(kSpacing, wildDabs);

      bool neutralExact = !oldDabs.empty() && oldDabs.size() == neutralDabs.size();
      bool wildExact = !oldDabs.empty() && oldDabs.size() == wildDabs.size();
      for (size_t i = 0; i < oldDabs.size(); ++i) {
        if (i >= neutralDabs.size() || neutralDabs[i].pos.x != oldDabs[i].x ||
            neutralDabs[i].pos.y != oldDabs[i].y)
          neutralExact = false;
        if (i >= wildDabs.size() || wildDabs[i].pos.x != oldDabs[i].x ||
            wildDabs[i].pos.y != oldDabs[i].y)
          wildExact = false;
      }
      std::printf("  [measured] curved path, %d samples at %.0f px spacing: %zu dabs\n", kN,
                  static_cast<double>(kSpacing), oldDabs.size());
      check(neutralExact, "StrokePath: Vec2 addPoint()/flush() and StrokeSample ones with neutral "
                          "axes emit bit-identical dab positions on a curved path");
      check(wildExact, "StrokePath: non-neutral axes do not move a single dab position -- axes "
                       "ride along the walk and never feed back into it");
    }

    auto makeDoc = [](int32_t w, int32_t h) {
      OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "stroke-input-neutral");
      recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("r")));
      return od;
    };
    BrushTip tip;
    tip.radius = 20.0f;
    tip.hardness = 1.0f;
    tip.flow = 1.0f;
    tip.opacity = 1.0f;
    tip.linearRgb = {0.4f, 0.2f, 0.6f};
    tip.spacing = 0.3f;

    auto paintViaAddPoint = [&]() {
      OpenDocument od = makeDoc(256, 256);
      StrokeSession s;
      std::string err;
      s.begin(od, 1, tip, Tool::Brush, &err, nullptr, DynamicInputs{});
      for (int i = 0; i < 6; ++i) s.addPoint(40.0f + 20.0f * static_cast<float>(i), 128.0f);
      s.end();
      return std::pair<OpenDocument, StrokeSession>{std::move(od), std::move(s)};
    };
    auto paintViaAddSample = [&]() {
      OpenDocument od = makeDoc(256, 256);
      StrokeSession s;
      std::string err;
      s.begin(od, 1, tip, Tool::Brush, &err, nullptr, DynamicInputs{});
      for (int i = 0; i < 6; ++i) {
        StrokeSample ss;  // default-constructed: the neutral reading this
                          // stroke's own `DynamicInputs{}` latch also holds
        ss.pos = Vec2{40.0f + 20.0f * static_cast<float>(i), 128.0f};
        s.addSample(ss);
      }
      s.end();
      return std::pair<OpenDocument, StrokeSession>{std::move(od), std::move(s)};
    };

    auto [odA, sA] = paintViaAddPoint();
    auto [odB, sB] = paintViaAddSample();

    check(sA.dabCount() > 0, "setup: the addPoint() stroke deposited at least one real dab");
    check(sA.dabCount() == sB.dabCount() && sA.texelsWritten() == sB.texelsWritten(),
          "neutral axes: addPoint() and addSample() agree on dabCount()/texelsWritten() exactly");
    check(sA.strokeTiles() == sB.strokeTiles(),
          "neutral axes: addPoint() and addSample() dirty the identical tile set");

    bool pixelsEqual = true;
    for (int32_t y = 100; y <= 156 && pixelsEqual; ++y) {
      for (int32_t x = 20; x <= 160 && pixelsEqual; ++x) {
        const Tile* ta = odA.document.layers[1].rgbTiles->find(tileCoordAt(PixelCoord{x, y}));
        const Tile* tb = odB.document.layers[1].rgbTiles->find(tileCoordAt(PixelCoord{x, y}));
        const std::array<float, 4> pa =
            ta != nullptr ? ta->readPixel(tileLocalOffset(PixelCoord{x, y}))
                         : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        const std::array<float, 4> pb =
            tb != nullptr ? tb->readPixel(tileLocalOffset(PixelCoord{x, y}))
                         : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        if (pa != pb) pixelsEqual = false;
      }
    }
    check(pixelsEqual, "neutral axes: every stored pixel across the painted region is bit-identical "
                       "between the addPoint() stroke and the addSample() one");
  }

  // ==========================================================================
  // 6. Per-dab axes actually reach a real accumulator: a PenPressure-
  //    controlled Size Variance, painted through a real StrokeSession with a
  //    rising-pressure stroke, must produce RISING dab radii -- not a value
  //    latched once for the whole stroke. This is the sabotage that reading
  //    `hardwareInputs_` instead of the dab's own axes in `depositPending()`
  //    must turn red (A6's third sabotage).
  // ==========================================================================
  {
    OpenDocument od = makeBlankOpenDocument(512, 512, WorkingSpace{}, "stroke-input-radii");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));

    constexpr float kBaseRadius = 40.0f;
    BrushModel model;
    model.tip.diameterPx = kBaseRadius * 2.0f;
    model.tip.angleDeg = 0.0f;
    model.tip.roundness = 1.0f;
    model.scatter.count = 1;
    // jitter/minimum stay at their default (0) -- inert besides the control,
    // so the resolved Size multiplier is EXACTLY `dab.pressure`
    // (brush/Variance.cpp's own formula: m=0, rj=1 since jitter<=0, c =
    // clamp(pressure,0,1) when hasPressure, result = 0 + 1*c = c).
    model.shape.size.control = VarianceControl::PenPressure;

    BrushTip tip;
    tip.radius = kBaseRadius;
    tip.hardness = 1.0f;
    tip.flow = 1.0f;
    tip.opacity = 1.0f;
    tip.linearRgb = {0.5f, 0.3f, 0.2f};
    tip.spacing = 0.05f;  // -> spacingPx = 2 px: many dabs per segment, so
                          // each of the checkpoints below has real dabs to
                          // report rather than landing between them

    // A deliberately WRONG, CONSTANT `hardwareInputs` pressure -- 0.5,
    // distinct from every dab's own pressure below (0.0, 0.33, 0.66, 1.0).
    // The correct implementation never reads this value's `.pressure` for
    // Size (only its `.hasPressure` flag, per `depositPending()`'s own
    // comment); a sabotaged one that read `hardwareInputs_` instead of the
    // dab's own axes would report `kBaseRadius * 0.5` for EVERY checkpoint
    // below, flat rather than rising -- the exact thing this section proves
    // does not happen.
    DynamicInputs wrongConstant;
    wrongConstant.hasPressure = true;
    wrongConstant.pressure = 0.5f;

    StrokeSession s;
    std::string err;
    const bool began = s.begin(od, 1, tip, Tool::Brush, &err, &model, wrongConstant);
    check(began, "setup: the rising-pressure stroke began");

    // Four raw samples -- a genuinely rising-pressure stroke, `StrokeDab`s
    // interpolated between consecutive pairs exactly as section 1 above --
    // fed one at a time so `lastDabRadius()` can be read after each call
    // that has enough context to emit (`addPoint()`'s own "three real
    // samples before the first segment" rule, brush/StrokePath.hpp): the
    // 3rd sample's own call first emits, walking samples 1->2 (pressure
    // 0.0->0.33); the 4th's call walks samples 2->3 (0.33->0.66); `end()`'s
    // flush walks the final segment, samples 3->4 (0.66->1.0).
    s.addSample(StrokeSample{Vec2{100.0f, 250.0f}, 0.0f, 0.0f, 0.0f, 0.5f});
    s.addSample(StrokeSample{Vec2{233.33f, 250.0f}, 0.33f, 0.0f, 0.0f, 0.5f});
    s.addSample(StrokeSample{Vec2{366.67f, 250.0f}, 0.66f, 0.0f, 0.0f, 0.5f});
    const float rEarly = s.lastDabRadius();  // samples 1->2 walked: 0.0 -> 0.33
    s.addSample(StrokeSample{Vec2{500.0f, 250.0f}, 1.0f, 0.0f, 0.0f, 0.5f});
    const float rMid = s.lastDabRadius();  // samples 2->3 walked: 0.33 -> 0.66
    s.end();
    const float rLate = s.lastDabRadius();  // flush walked 3->4: 0.66 -> 1.0

    std::printf("  [measured] PenPressure Size, rising stroke -- early %.3f px, mid %.3f px, "
                "late %.3f px (base %.1f px, sabotaged-constant reading would be %.1f px flat)\n",
                static_cast<double>(rEarly), static_cast<double>(rMid), static_cast<double>(rLate),
                static_cast<double>(kBaseRadius), static_cast<double>(kBaseRadius * 0.5f));

    check(rEarly > 0.0f && rMid > 0.0f && rLate > 0.0f,
          "setup: all three checkpoints landed on a real dab");
    check(rEarly < rMid && rMid < rLate,
          "PenPressure Size: dab radii RISE across the rising-pressure stroke (per-dab axes, not "
          "one frame-latched reading)");
    // Thresholds bracket the sabotaged flat reading (kBaseRadius * 0.5 = 20)
    // with margin: the correct early checkpoint is near pressure 0.3 (radius
    // ~12), the late one near pressure 0.98 (radius ~39) -- see the comment
    // above `wrongConstant` for why 0.5/20 is what a sabotaged run would give
    // at EVERY checkpoint instead.
    check(rEarly < kBaseRadius * 0.5f,
          "PenPressure Size: the EARLY checkpoint is well below the sabotaged constant reading");
    check(rLate > kBaseRadius * 0.75f,
          "PenPressure Size: the LATE checkpoint is well above the sabotaged constant reading");
  }

  std::printf("[selftest] stroke input %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
