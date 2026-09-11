#include "app/selftest/Support.hpp"

#include <cmath>

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

    // Open interval, not closed: `StrokePath::addPoint()`'s own contract is
    // that the first dab of a drag lands ONE FULL SPACING in, never at the
    // first sample itself (`app/selftest/StrokePath.cpp` section 5 asserts
    // the identical thing for position) -- and `flush()`'s trailing segment
    // never reaches its own far endpoint either (`leftover_` is spent before
    // `u` reaches 1). So neither the exact 0.0 the first sample carried nor
    // the exact 1.0 the last one did should ever appear on a dab.
    check(!dabs.empty() && dabs.front().pressure > 0.0f && dabs.front().pressure < 1.0f,
          "two-sample rising segment: first dab's pressure is strictly inside (0, 1)");
    check(!dabs.empty() && dabs.back().pressure > 0.0f && dabs.back().pressure < 1.0f,
          "two-sample rising segment: last dab's pressure is strictly inside (0, 1)");
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

    // Position tolerance: `app/selftest/StrokePath.cpp` section 6 already
    // established 0.05 px as the bound for this exact comparison (2 vs 41
    // samples over an 80 px collinear path) -- the chord-walk error of a
    // 24-subdivision piecewise-linear approximation of a curve that, for
    // evenly spaced collinear input, degenerates to a straight line either
    // way (that test's own comment). Reused verbatim rather than re-derived,
    // since it is the identical geometry at a different path length.
    constexpr float kPosTol = 0.05f;

    // Pressure tolerance, derived rather than guessed: for straight, evenly
    // spaced collinear input the centripetal Catmull-Rom parameter `u` is
    // linear in POSITION (uniform knot spacing -- the same fact that makes
    // the position comparison above exact), and this fixture's raw sample
    // pressures lie exactly on one global linear ramp regardless of `n` --
    // so with infinite-precision arithmetic the 2-sample and 20-sample runs
    // would report BIT-IDENTICAL pressure at every matching dab position,
    // for the identical reason their positions already are. The only real
    // divergence is the SAME chord-walk float noise the position bound above
    // already measured, carried into pressure through this fixture's own
    // pressure-per-pixel gradient: `d(pressure)/d(x) = (1-0) / (kX1-kX0)`.
    // `kPosTol` of positional slop maps to that much pressure slop at worst;
    // a 10x margin absorbs the fact that the pressure interpolation itself
    // walks its own 24-subdivision `u` parameter alongside the position one,
    // roughly doubling (not 10x-ing) the noise budget in the worst case.
    const float pressurePerPx = 1.0f / (kX1 - kX0);
    const float kPressureTol = kPosTol * pressurePerPx * 10.0f;
    std::printf("  [derived] rate independence tolerances: position %.4f px, pressure %.6f "
                "(gradient %.6f/px x %.2f px x 10)\n",
                static_cast<double>(kPosTol), static_cast<double>(kPressureTol),
                static_cast<double>(pressurePerPx), static_cast<double>(kPosTol));

    bool posOk = coarse.size() == fine.size();
    bool pressureOk = coarse.size() == fine.size();
    for (size_t i = 0; i < coarse.size() && i < fine.size(); ++i) {
      if (!closeTo(coarse[i].pos.x, fine[i].pos.x, kPosTol) ||
          !closeTo(coarse[i].pos.y, fine[i].pos.y, kPosTol))
        posOk = false;
      if (!closeTo(coarse[i].pressure, fine[i].pressure, kPressureTol)) pressureOk = false;
    }
    check(posOk, "rate independence: dab positions match within the position tolerance above");
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
    // through are unaffected by this whole track: `hardwareInputs_` is still
    // latched once per frame from `dynamicInputsFor(AppState)` at `begin()`/
    // `setTip()`, exactly as before Track A -- this section's sample-level
    // conversion has no `has*` fields of its own to disagree with it (see
    // `StrokeSample`'s own header comment). Asserted here as the "as before"
    // half of this section's claim, against the one path that sets them:
    // `dynamicInputsFor()` on a mouse-only (never-`penSeen`) `AppState`.
    AppState mouseOnly;
    const DynamicInputs mouseInputs = dynamicInputsFor(mouseOnly);
    check(mouseInputs.hasPressure && !mouseInputs.hasTilt && !mouseInputs.hasBarrel,
          "mouse sample: dynamicInputsFor()'s own has* flags are unchanged by this track "
          "(hasPressure, !hasTilt, !hasBarrel)");
    check(mouseInputs.pressure == 1.0f && mouseInputs.tilt == 0.0f && mouseInputs.azimuth == 0.0f &&
              mouseInputs.barrel == 0.5f,
          "mouse sample: dynamicInputsFor()'s own four values are unchanged by this track");
  }

  // ==========================================================================
  // 5. The old addPoint(x, y) wrapper and the new addSample() with neutral
  //    axes produce bit-identical dabs, end to end through a real
  //    StrokeSession -- not merely through StrokePath alone.
  // ==========================================================================
  {
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
        StrokeSample ss;  // default-constructed: neutral axes, `addPoint()`'s own
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
    const float rEarly = s.lastDabRadius();
    s.addSample(StrokeSample{Vec2{366.67f, 250.0f}, 0.66f, 0.0f, 0.0f, 0.5f});
    const float rMid = s.lastDabRadius();
    s.addSample(StrokeSample{Vec2{500.0f, 250.0f}, 1.0f, 0.0f, 0.0f, 0.5f});
    s.end();
    const float rLate = s.lastDabRadius();

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
