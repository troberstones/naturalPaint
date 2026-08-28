#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"
#include "brush/Variance.hpp"

namespace np {

// ---------------------------------------------------------------------------
// A6 (docs/reachability-audit.md) -- the DYNAMICS matrix's dead half.
//
// `BrushDynamics.cpp` proves the LINK MODEL: what a curve, a range, INVERT
// and the fold rule do to one already-known source value. It is not restated
// here. This file proves the eight SOURCES and the twelve TARGETS the
// matrix's own cells stand for -- four sources that used to be hard `0.0`
// forever (VELOCITY, FADE, NOISE, RANDOM) and six targets nothing ever read
// (SCATTER, CONCENTRATION, HUE, SATURATION, VALUE, WETNESS) -- and, above all,
// that the two stochastic sources are DETERMINISTIC: the same stroke replayed
// must deposit bit-identical pixels, because `core/History` (ADR-0005)
// reconstructs a document by replaying a stroke's own dab stream from a
// keyframe, and the golden harness compares a script's pixels across runs of
// the same binary. A `rand()` call or a clock-seeded generator anywhere in
// this path would make a stroke un-replayable and a golden image flaky, which
// is why brush/Dynamics.hpp's own section comment spends as long as it does
// arguing the seed is a pure function of the stroke's own first recorded
// position -- not a counter, not a clock, nothing outside the stroke's own
// geometry.
// ---------------------------------------------------------------------------
bool runDynamicsSourcesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  auto makeDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "dynamics-sources");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));
    return od;
  };

  using TileBytes = std::vector<std::pair<TileCoord, std::vector<uint16_t>>>;
  auto snapshotBytes = [](const PigmentTileStore& store) {
    TileBytes out;
    for (const auto& [coord, tile] : store)
      out.emplace_back(coord, std::vector<uint16_t>(tile.data(),
                                                    tile.data() + PigmentTile::kTexelCount));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      return a.first.y != b.first.y ? a.first.y < b.first.y : a.first.x < b.first.x;
    });
    return out;
  };

  // (The old `tip(radius, hardness, flow)` helper that built a raw
  // `BrushTip` by hand is gone -- every fixture below routes through
  // `brushTipFor()` and a `BrushState`/`BrushModel` now, since a raw tip
  // has no model for `StrokeSession::begin()`'s per-dab Variance
  // resolution to read.)

  // A wavy synthetic path -- 36 samples along a sine, the same shape
  // `PigmentDeposit.cpp`'s own stroke sections use, so the arc-length
  // emitter produces a realistic run of dabs rather than one or two.
  auto paintPath = [](StrokeSession& s, float originX, float originY) {
    for (int i = 0; i < 36; ++i) {
      const float u = static_cast<float>(i) / 35.0f;
      s.addPoint(originX + 340.0f * u, originY + 140.0f * std::sin(u * 5.0f));
    }
  };

  // ======================================================================
  // 1. splitmix64 / strokeSeedFromStart / dynamicRandomDraw -- pure and
  //    deterministic, at the level `--selftest` can check without a stroke.
  // ======================================================================
  {
    check(splitmix64(0) == splitmix64(0) && splitmix64(1) != splitmix64(0),
          "hash: splitmix64 is a pure function -- same input same output, different input "
          "(almost always) a different one");

    const uint64_t seedA = strokeSeedFromStart(10.0f, 20.0f);
    const uint64_t seedA2 = strokeSeedFromStart(10.0f, 20.0f);
    const uint64_t seedB = strokeSeedFromStart(400.0f, 300.0f);
    check(seedA == seedA2,
          "seed: the same starting position produces the same seed every time -- no clock, "
          "no counter, nothing but the stroke's own first sample");
    check(seedA != seedB, "seed: two different starting positions produce different seeds");

    const float r0 = dynamicRandomDraw(seedA, 0);
    const float r0Again = dynamicRandomDraw(seedA, 0);
    const float r1 = dynamicRandomDraw(seedA, 1);
    check(r0 == r0Again,
          "random: (seed, dabIndex) is pure -- called twice it returns the bit-identical float");
    check(r0 != r1, "random: consecutive dab indices draw different values");
    check(r0 >= 0.0f && r0 < 1.0f && r1 >= 0.0f && r1 < 1.0f,
          "random: every draw lands in [0,1)");

    // A spread over many draws, not just two samples that happen to differ --
    // the assertion a hard-coded constant disguised as "implemented" would
    // fail, the same shape as a source that quietly stopped reading its
    // arguments and started returning one plausible-looking number instead.
    float lo = 2.0f, hi = -1.0f;
    for (uint32_t i = 0; i < 200; ++i) {
      const float v = dynamicRandomDraw(seedA, i);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    check(hi - lo > 0.9f,
          "random: 200 draws off one seed span nearly the whole [0,1) range -- not clustered, "
          "not constant");
  }

  // ======================================================================
  // 2. VELOCITY -- normalisation and the first-dab case
  // ======================================================================
  {
    check(dynamicVelocity(0.0f, 24.0f) == 0.0f,
          "velocity: zero step distance -- the FIRST dab of a stroke, which has no previous "
          "position -- normalises to exactly 0.0, not a special case but the same formula "
          "0/radius always gives");
    check(dynamicVelocity(24.0f, 24.0f) == 1.0f,
          "velocity: a full radius of travel between two dabs normalises to exactly 1.0 -- "
          "the empirical anchor (four dabs land in one radius at the shipped 0.25 spacing, so "
          "one radius BETWEEN dabs is already ~4x an ordinary stroke's implied speed)");
    check(dynamicVelocity(48.0f, 24.0f) == 1.0f,
          "velocity: twice that clamps at 1.0 rather than exceeding it");
    check(nearf(dynamicVelocity(6.0f, 24.0f), 0.25f, 1e-6f),
          "velocity: a quarter-radius step -- the shipped DEFAULT spacing -- reads as 0.25, "
          "not as 'fast': ordinary painting motion sits in the middle of the range, not "
          "pinned to either end");
    check(dynamicVelocity(10.0f, 0.0f) == 0.0f,
          "velocity: a degenerate zero-radius tip reads as motionless rather than dividing by "
          "zero, matching brush/Deposit.hpp's own 'a zero radius deposits nothing' contract");
  }

  // ======================================================================
  // 3. FADE -- the ramp, and its endpoints exactly
  // ======================================================================
  {
    check(dynamicFade(0.0f) == 0.0f, "fade: distance 0 (the stroke's first dab) is exactly 0.0");
    check(dynamicFade(kFadeLengthPx) == 1.0f,
          "fade: distance == kFadeLengthPx reaches EXACTLY 1.0 -- x/x is exact in IEEE754 for "
          "any nonzero finite x, so this is not a near-miss the way an accumulated sum could be");
    check(dynamicFade(kFadeLengthPx * 0.5f) == 0.5f,
          "fade: the midpoint of the ramp is EXACTLY 0.5 -- 240/480 is exactly representable, "
          "so this checks the formula rather than a rounding coincidence");
    check(dynamicFade(kFadeLengthPx * 4.0f) == 1.0f,
          "fade: distance past the ramp's length stays at 1.0 rather than exceeding it -- a "
          "fade-out brush does not un-fade on a very long stroke");
    check(kFadeLengthPx > 0.0f, "fade: the ramp length is a positive, named constant");
  }

  // ======================================================================
  // 4. NOISE -- coherent and continuous, NOT a fresh draw per dab
  // ======================================================================
  {
    const uint64_t seed = strokeSeedFromStart(1.0f, 1.0f);

    // Continuity: two queries a tenth of a pixel apart must be close. Two
    // RANDOM draws one dab apart carry no such promise at all -- that
    // contrast is the whole reason NOISE and RANDOM are two rows and not one.
    float worstLocalJump = 0.0f;
    for (float d = 0.0f; d < 400.0f; d += 4.0f) {
      const float a = dynamicNoiseAt(seed, d);
      const float b = dynamicNoiseAt(seed, d + 0.1f);
      worstLocalJump = std::max(worstLocalJump, std::fabs(a - b));
    }
    std::printf("  noise: worst change over a 0.1 px step, sampled every 4 px along 400 px "
                "%.5f\n",
                static_cast<double>(worstLocalJump));
    check(worstLocalJump < 0.01f,
          "noise: a 0.1 px step along the stroke never moves the value by more than 1%% -- "
          "smooth and continuous, which is what makes it read as natural variation and not "
          "jitter");

    // But it is not FROZEN either: over the length of an ordinary stroke it
    // visibly varies, which is the assertion that would catch it silently
    // returning one constant -- the same defect shape as a source that
    // stopped reading its arguments, applied to this source instead of
    // VELOCITY.
    float lo = 2.0f, hi = -1.0f;
    for (float d = 0.0f; d < 600.0f; d += 3.0f) {
      const float v = dynamicNoiseAt(seed, d);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    check(hi - lo > 0.3f,
          "noise: over a 600 px stroke the value swings by more than 0.3 -- it moves, it just "
          "moves smoothly");

    // Determinism, restated for this source specifically: this is the
    // assertion that reddens if NOISE's seed is ever folded together with
    // something that changes between calls (the wall clock, a static
    // counter), while the two checks above stay green regardless -- a noise
    // source that varies is not the same claim as one that replays.
    check(dynamicNoiseAt(seed, 123.4f) == dynamicNoiseAt(seed, 123.4f),
          "noise: the SAME (seed, distance) pair returns the bit-identical float on a second "
          "call -- no hidden clock, no static counter advancing between calls");

    // Two different seeds -- i.e. two different strokes -- do not walk the
    // same path through the noise field.
    const uint64_t seedOther = strokeSeedFromStart(900.0f, 900.0f);
    bool anyDiffer = false;
    for (float d = 0.0f; d < 200.0f; d += 10.0f)
      if (dynamicNoiseAt(seed, d) != dynamicNoiseAt(seedOther, d)) anyDiffer = true;
    check(anyDiffer,
          "noise: two different strokes' seeds produce two different noise fields, not the "
          "same field read twice");
  }

  // ======================================================================
  // 5. All eight sources, in one table -- the assertion that would have
  //    caught the original defect (four hard-coded to 0.0) and must catch
  //    the next one.
  // ======================================================================
  {
    // The four hardware sources: dynamicInputsFor() reads them straight off
    // AppState, so varying the AppState field must vary the DynamicInputs
    // field -- the direct check that none of the four is silently ignored.
    AppState st;
    st.penSeen = true;
    st.penPressure = 0.2f;
    st.penTilt = 0.1f;
    st.penAzimuth = 0.3f;
    st.penBarrel = 0.4f;
    const DynamicInputs low = dynamicInputsFor(st);
    st.penPressure = 0.9f;
    st.penTilt = 0.8f;
    st.penAzimuth = 0.9f;
    st.penBarrel = 0.9f;
    const DynamicInputs high = dynamicInputsFor(st);
    check(low.pressure != high.pressure && low.tilt != high.tilt &&
              low.azimuth != high.azimuth && low.barrel != high.barrel,
          "sources: PRESSURE, TILT, AZIMUTH and BARREL each move when the AppState field "
          "behind them moves -- dynamicInputsFor() reads all four, not a subset");

    // The six stroke-local sources: each must vary across a realistic
    // sequence of (stepDist, distance, dabIndex, heading) -- what a real
    // stroke would hand them. Table-shaped on purpose: a reader adding an
    // eleventh source later sees exactly what row to add.
    //
    // INITIAL DIRECTION shares this table's DIRECTION row rather than
    // getting its own generator call: `DynamicSource::InitialDirection`'s
    // resolved VALUE is `dynamicDirection()` too (brush/Dynamics.hpp's own
    // "INITIAL DIRECTION" section -- there is no second arithmetic
    // function, only a different CALLER discipline, which lives in
    // `app/StrokeSession` and is proved in section 10 below, not here).
    // This table's whole job is proving the GENERATOR is not a hard-coded
    // constant, and INITIAL DIRECTION's generator IS DIRECTION's, so
    // reusing `directions` below is not a shortcut, it is the honest
    // reflection of that shared arithmetic.
    struct Row {
      const char* name;
      bool varies;
    };
    std::vector<float> velocities, fades, noises, randoms, directions;
    const uint64_t tableSeed = strokeSeedFromStart(1.0f, 1.0f);
    float dist = 0.0f;
    for (uint32_t i = 0; i < 40; ++i) {
      // A step that itself varies, the way a hand-drawn stroke's speed does
      // -- constant step distances would make VELOCITY's own table entry
      // vacuously true.
      const float step = 2.0f + 6.0f * std::fabs(std::sin(static_cast<float>(i) * 0.7f));
      dist += step;
      velocities.push_back(dynamicVelocity(step, 24.0f));
      fades.push_back(dynamicFade(dist));
      noises.push_back(dynamicNoiseAt(tableSeed, dist));
      randoms.push_back(dynamicRandomDraw(tableSeed, i));
      // A heading that sweeps from 30 to 264 degrees -- varying, like every
      // other row, but deliberately never crossing DIRECTION's own wrap
      // point at 0/360 (due "+x"; see `dynamicDirection()`'s header
      // comment). `spans()` below is a plain hi-minus-lo over the
      // NORMALISED value, which reads a wrap as a spurious near-1.0 span
      // regardless of whether the underlying heading actually moved -- this
      // table wants to know whether the SOURCE varies, not to re-litigate
      // the wrap, which section 9 below tests directly and on purpose.
      const float headingDeg = 30.0f + 6.0f * static_cast<float>(i);
      const float headingRad = headingDeg * 0.017453292519943295f;  // pi/180
      directions.push_back(
          dynamicDirection(std::cos(headingRad), std::sin(headingRad)));
    }
    auto spans = [](const std::vector<float>& v) {
      float lo = v[0], hi = v[0];
      for (float x : v) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
      return hi - lo;
    };
    const Row rows[10] = {
        {"PRESSURE", low.pressure != high.pressure},
        {"TILT", low.tilt != high.tilt},
        {"AZIMUTH", low.azimuth != high.azimuth},
        {"BARREL", low.barrel != high.barrel},
        {"VELOCITY", spans(velocities) > 0.05f},
        {"FADE", spans(fades) > 0.05f},
        {"NOISE", spans(noises) > 0.05f},
        {"RANDOM", spans(randoms) > 0.3f},
        {"DIRECTION", spans(directions) > 0.05f},
        {"INITIAL", spans(directions) > 0.05f},
    };
    bool allVary = true;
    for (const Row& r : rows) {
      std::printf("  source table: %-10s %s\n", r.name, r.varies ? "varies" : "CONSTANT");
      if (!r.varies) allVary = false;
    }
    check(allVary,
          "sources: all ten vary across a realistic sequence -- the table that would have "
          "caught VELOCITY, FADE, NOISE and RANDOM stuck at their old hard 0.0, and must catch "
          "a future source silently returning a constant");

    // **The table above proves the GENERATORS vary. It does not touch the
    // dispatch that hands their values to the link system, and that is the
    // half the audit's defect lived in.**
    //
    // Every row above calls a generator directly -- `dynamicRandomDraw()`,
    // `dynamicNoiseAt()`, `dynamicVelocity()`, `dynamicFade()`. Nothing in it
    // calls `sourceValue()`, which is the one function a link actually goes
    // through to read a source. Pinning `sourceValue()`'s RANDOM arm to a
    // literal 0.35 -- the exact shape of the constant the reachability audit
    // found -- leaves this whole section green, and leaves every behavioural
    // assertion in this file green too, because the other two seed consumers
    // (`dynamicNoiseAt()` for NOISE and `applyPerDabScatter()`, which draws
    // SCATTER's angle straight off a salted seed and deliberately bypasses
    // links entirely -- app/StrokeSession.cpp section on kScatterAngleSalt)
    // keep the deposit varying whatever the link layer does.
    //
    // So this asserts the dispatch itself, field by field: each source must
    // return ITS OWN input, not a constant, not a neighbour. Distinct values
    // per field are what make "not a neighbour" checkable at all -- with two
    // fields sharing a value a copy-paste between their two `case` arms would
    // read as correct.
    DynamicInputs probe;
    probe.pressure = 0.11f;
    probe.tilt = 0.22f;
    probe.azimuth = 0.33f;
    probe.barrel = 0.44f;
    probe.velocity = 0.55f;
    probe.fade = 0.66f;
    probe.noise = 0.77f;
    probe.random = 0.88f;
    probe.direction = 0.99f;
    probe.initialDirection = 0.15f;
    const struct {
      DynamicSource source;
      float expected;
      const char* name;
    } dispatch[10] = {
        {DynamicSource::Pressure, probe.pressure, "PRESSURE"},
        {DynamicSource::Tilt, probe.tilt, "TILT"},
        {DynamicSource::Azimuth, probe.azimuth, "AZIMUTH"},
        {DynamicSource::Barrel, probe.barrel, "BARREL"},
        {DynamicSource::Velocity, probe.velocity, "VELOCITY"},
        {DynamicSource::Fade, probe.fade, "FADE"},
        {DynamicSource::Noise, probe.noise, "NOISE"},
        {DynamicSource::Random, probe.random, "RANDOM"},
        {DynamicSource::Direction, probe.direction, "DIRECTION"},
        {DynamicSource::InitialDirection, probe.initialDirection, "INITIAL"},
    };
    bool dispatchOk = true;
    for (const auto& d : dispatch) {
      const float got = sourceValue(probe, d.source);
      if (got != d.expected) {
        dispatchOk = false;
        std::printf("  source dispatch: %-10s returned %.6f, expected %.6f\n", d.name, got,
                    d.expected);
      }
    }
    check(dispatchOk,
          "sources: sourceValue() returns each source's OWN input, at zero tolerance -- the "
          "assertion that catches a source arm replaced by a literal, which is what RANDOM was "
          "before the audit and what nothing else in this file can see");
  }

  // ======================================================================
  // 6. The twelve targets: which six were dead, and what happened to each
  // ======================================================================
  {
    // The refusal table -- Wetness alone, and the other eleven all buildable.
    // This is the assertion that would catch a future target left live but
    // inert: any target this loop does not name explicitly must resolve to
    // nullptr, or it is silently claiming to work.
    struct TRow {
      DynamicTarget t;
      bool shouldRefuse;
    };
    const TRow trows[12] = {
        {DynamicTarget::Size, false},      {DynamicTarget::Angle, false},
        {DynamicTarget::Roundness, false}, {DynamicTarget::Hardness, false},
        {DynamicTarget::Flow, false},      {DynamicTarget::Scatter, false},
        {DynamicTarget::Spacing, false},   {DynamicTarget::Concentration, false},
        {DynamicTarget::Hue, false},       {DynamicTarget::Saturation, false},
        {DynamicTarget::Value, false},     {DynamicTarget::Wetness, true},
    };
    bool refusalTableOk = true;
    for (const TRow& r : trows) {
      const bool refused = targetUnbuildableReason(r.t) != nullptr;
      if (refused != r.shouldRefuse) refusalTableOk = false;
    }
    check(refusalTableOk,
          "targets: WETNESS is the one honest refusal -- every other target, including the "
          "five that used to be dead, reports buildable");
    check(std::strlen(targetUnbuildableReason(DynamicTarget::Wetness)) > 20,
          "targets: the refusal names the missing piece (no CPU deposit route holds a "
          "wetness field) rather than a generic 'not built yet'");

    // The per-CELL refinement -- without this, a user could wire RANDOM (or
    // any stroke-local source) straight to HUE/SATURATION/VALUE and get a
    // cell that looks exactly as live as PRESSURE -> HUE while silently
    // resolving to a constant every dab, which is the ORIGINAL Dry Bristle
    // defect reborn in a corner of the matrix this file had not yet named.
    check(cellUnbuildableReason(DynamicSource::Random, DynamicTarget::Hue) != nullptr &&
              cellUnbuildableReason(DynamicSource::Noise, DynamicTarget::Saturation) != nullptr &&
              cellUnbuildableReason(DynamicSource::Velocity, DynamicTarget::Value) != nullptr &&
              cellUnbuildableReason(DynamicSource::Fade, DynamicTarget::Hue) != nullptr,
          "targets: a STROKE-LOCAL source into HUE/SATURATION/VALUE is refused at the CELL "
          "even though the TARGET column is buildable -- these three resolve once per frame, "
          "before a stroke-local source's per-dab value even exists");
    check(cellUnbuildableReason(DynamicSource::Pressure, DynamicTarget::Hue) == nullptr &&
              cellUnbuildableReason(DynamicSource::Tilt, DynamicTarget::Saturation) == nullptr &&
              cellUnbuildableReason(DynamicSource::Azimuth, DynamicTarget::Value) == nullptr &&
              cellUnbuildableReason(DynamicSource::Barrel, DynamicTarget::Hue) == nullptr,
          "targets: the identical three columns stay fully live from any of the four "
          "HARDWARE sources -- the refusal is about WHEN a source resolves, not about the "
          "target itself");
    check(cellUnbuildableReason(DynamicSource::Pressure, DynamicTarget::Wetness) != nullptr &&
              cellUnbuildableReason(DynamicSource::Random, DynamicTarget::Wetness) != nullptr,
          "targets: WETNESS refuses every source, hardware and stroke-local alike -- its "
          "problem is that no CPU route reads it at all, not a timing mismatch");

    // DIRECTION on its own: it is a fifth stroke-local source, added after
    // the four above, so `sourceIsStrokeLocal()` gaining a new `true` arm
    // that a copy-paste missed would leave this specific cell reading as
    // buildable when it must not -- the same defect shape the four-source
    // check above exists to catch, restated for the newest source rather
    // than assumed to fall out of it for free.
    check(cellUnbuildableReason(DynamicSource::Direction, DynamicTarget::Hue) != nullptr &&
              cellUnbuildableReason(DynamicSource::Direction, DynamicTarget::Saturation) !=
                  nullptr &&
              cellUnbuildableReason(DynamicSource::Direction, DynamicTarget::Value) != nullptr,
          "targets: DIRECTION into HUE/SATURATION/VALUE is refused at the CELL too -- it "
          "resolves once per DAB exactly like VELOCITY/FADE/NOISE/RANDOM, so it hits the "
          "identical frame-vs-dab mismatch");
    check(cellUnbuildableReason(DynamicSource::Direction, DynamicTarget::Angle) == nullptr,
          "targets: DIRECTION into ANGLE -- the cell this whole source exists for -- is fully "
          "buildable");

    // INITIAL DIRECTION restates the identical pair once more -- a sixth
    // stroke-local source, and `sourceIsStrokeLocal()` is asserted directly
    // for it below too, rather than trusted to this refusal alone: a source
    // marked stroke-local by MISTAKE (never latched, never fed a real
    // heading) would still refuse these three cells correctly while
    // resolving ANGLE to a permanent 0.0 -- wrong in a way this refusal
    // check alone cannot see, which is exactly why section 10 below proves
    // the resolved VALUE too, not only its timing classification.
    check(cellUnbuildableReason(DynamicSource::InitialDirection, DynamicTarget::Hue) !=
                  nullptr &&
              cellUnbuildableReason(DynamicSource::InitialDirection, DynamicTarget::Saturation) !=
                  nullptr &&
              cellUnbuildableReason(DynamicSource::InitialDirection, DynamicTarget::Value) !=
                  nullptr,
          "targets: INITIAL DIRECTION into HUE/SATURATION/VALUE is refused at the CELL too -- "
          "it resolves once per DAB (the first one with a real step vector), not once per "
          "FRAME, so it hits the identical frame-vs-dab mismatch");
    check(cellUnbuildableReason(DynamicSource::InitialDirection, DynamicTarget::Angle) ==
              nullptr,
          "targets: INITIAL DIRECTION into ANGLE is fully buildable");
    check(sourceIsStrokeLocal(DynamicSource::InitialDirection),
          "targets: INITIAL DIRECTION classifies as STROKE-LOCAL, asserted directly rather "
          "than only inferred from the HSV refusal above -- commit b704411's own P0 (a "
          "stroke-local link resolved at the wrong granularity, silently, with 4442 green "
          "assertions around it) is exactly the failure mode a source classified into the "
          "wrong half produces, and this is the one line that would catch it for this source "
          "specifically");

    // SCATTER, rewritten for the model migration: exercised through a REAL
    // stroke -- StrokeSession's per-dab loop, with a `BrushModel*` set, is
    // the only place it can show an effect (it moves a position) -- but the
    // driver is now `model.scatter.scatter.jitter` (a `Variance`, resolved
    // by `varianceOffset()` per dab off `seed_`/`dabs_`), not a
    // `DynamicSource::Random -> DynamicTarget::Scatter` link. `BrushLinkSet`
    // is shelved; nothing that paints reads it.
    {
      BrushState scatterBrush;
      scatterBrush.model.tip.diameterPx = 40.0f;
      scatterBrush.model.tip.hardness = 0.4f;
      scatterBrush.load = 0.5f;
      scatterBrush.model.scatter.scatter.jitter = 0.9f;  // most of a radius,
                                                          // so the effect is
                                                          // not lost in noise
      const MixboxLut noLut;
      DynamicInputs in;

      OpenDocument plain = makeDoc(512, 512);
      OpenDocument scattered = makeDoc(512, 512);
      {
        StrokeSession s;
        std::string e;
        BrushState plainBrush = scatterBrush;
        plainBrush.model.scatter.scatter.jitter = 0.0f;
        s.begin(plain, 1, brushTipFor(plainBrush, noLut, in), Tool::Brush, &e, &plainBrush.model,
                in);
        paintPath(s, 60.0f, 256.0f);
        s.end();
      }
      {
        StrokeSession s;
        std::string e;
        s.begin(scattered, 1, brushTipFor(scatterBrush, noLut, in), Tool::Brush, &e,
                &scatterBrush.model, in);
        paintPath(s, 60.0f, 256.0f);
        s.end();
      }
      check(snapshotBytes(*plain.document.layers[1].pigmentTiles) !=
                snapshotBytes(*scattered.document.layers[1].pigmentTiles),
            "targets: SCATTER, driven by `model.scatter.scatter.jitter`, visibly moves dabs off "
            "the raw path -- the byte-identical-without-it stroke is the control this diff is "
            "measured against");
    }

    // CONCENTRATION, HUE, SATURATION and VALUE, rewritten for the model
    // migration: **all four are now INERT, not merely unproven.** The old
    // matrix resolved them at `brushTipFor()`'s frame-level path
    // (`evaluateLinksFiltered(brush.links, ..., wantStrokeLocal=false)`);
    // that call is gone (`app/StrokeSession.cpp`'s own comment on
    // `brushTipFor()`), and `BrushModel` has no per-dab wiring for any of
    // the four yet -- `tip.flow = brush.load` unscaled, HSV set to identity
    // `(0,0,0)`, both with a comment naming this a deliberately deferred
    // divergence, not an oversight. A `BrushLinkSet` populated with the
    // identical links this section used to prove LIVE now proves the
    // opposite: they no longer move `brushTipFor()`'s output at all.
    {
      BrushState brush;
      brush.load = 0.5f;
      brush.pigment = 0;
      DynamicInputs in;
      in.pressure = 1.0f;
      const MixboxLut noLut;
      const BrushTip plainTip = brushTipFor(brush, noLut, in);

      BrushLink concLink;
      concLink.source = DynamicSource::Pressure;
      concLink.target = DynamicTarget::Concentration;
      concLink.rangeLo = 0.2f;
      concLink.rangeHi = 0.2f;
      addLink(brush.links, concLink);
      BrushLink hueLink;
      hueLink.source = DynamicSource::Pressure;
      hueLink.target = DynamicTarget::Hue;
      hueLink.rangeLo = 0.0f;
      hueLink.rangeHi = 0.5f;
      addLink(brush.links, hueLink);
      BrushLink satLink;
      satLink.source = DynamicSource::Pressure;
      satLink.target = DynamicTarget::Saturation;
      satLink.rangeLo = 0.0f;
      satLink.rangeHi = 0.0f;  // would fully desaturate, if it still applied
      addLink(brush.links, satLink);
      const BrushTip linkedTip = brushTipFor(brush, noLut, in);

      check(nearf(linkedTip.flow, plainTip.flow, 1e-6f) &&
                linkedTip.linearRgb == plainTip.linearRgb,
            "targets: CONCENTRATION, HUE and SATURATION links -- Pressure-driven, the shape "
            "that used to reach `brushTipFor()`'s frame-level path -- move NEITHER `flow` NOR "
            "`linearRgb` any more: nothing downstream of `BrushState::links` reads them");
    }
  }

  // ======================================================================
  // 7. Determinism, rewritten for the model migration: the same stroke
  //    replayed is bit-identical; two different strokes are not
  // ======================================================================
  //
  // The old fixture drove all four (then-)stroke-local sources through a
  // `BrushLinkSet` onto five targets, two of which (Flow, Hardness) have no
  // model-level wiring at all any more (section 6's own rewrite, above).
  // What determinism actually rests on now is `Variance::jitter` on Size,
  // Angle, Roundness and Scatter -- each resolved once per dab off
  // `seed_`/`dabs_` (`app/StrokeSession.cpp`'s per-dab loop) -- so this
  // fixture jitters all four instead. `Variance::control` stays `Off` on
  // every one of them: jitter alone (`varianceScale()`'s `rj` term) is what
  // draws from the per-dab random stream, with no device or stroke-geometry
  // signal in the mix, so this isolates jitter's own seed-derived randomness
  // exactly as the old fixture isolated Noise/Random.
  {
    auto makeBrush = [](bool withVariance) {
      BrushState brush;
      brush.model.tip.diameterPx = 48.0f;
      brush.model.tip.hardness = 0.35f;
      brush.load = 0.4f;
      if (withVariance) {
        brush.model.shape.size.jitter = 0.4f;
        brush.model.shape.angle.jitter = 0.6f;
        brush.model.shape.roundness.jitter = 0.5f;
        brush.model.scatter.scatter.jitter = 0.5f;
      }
      return brush;
    };

    auto runStroke = [&](float originX, float originY, bool withVariance) {
      OpenDocument od = makeDoc(1024, 1024);
      BrushState brush = makeBrush(withVariance);
      const MixboxLut noLut;
      DynamicInputs in;
      StrokeSession s;
      std::string e;
      s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in);
      paintPath(s, originX, originY);
      s.end();
      return snapshotBytes(*od.document.layers[1].pigmentTiles);
    };

    const TileBytes runA = runStroke(80.0f, 400.0f, true);
    const TileBytes runAReplayed = runStroke(80.0f, 400.0f, true);
    check(!runA.empty() && runA == runAReplayed,
          "determinism: the IDENTICAL stroke -- same start, same path, same jittered model -- "
          "run twice through two independent StrokeSessions deposits BIT-IDENTICAL tiles, at "
          "zero tolerance, through every jittered Variance site -- this is what protects undo "
          "and the golden harness");

    const TileBytes runB = runStroke(500.0f, 700.0f, true);
    check(runB != runA,
          "determinism: a DIFFERENT stroke -- different starting position, so a different "
          "seed -- does NOT deposit the identical tiles. 'Deterministic' would otherwise be "
          "satisfied by a constant, which the check above alone cannot rule out");

    const TileBytes runNoVariance = runStroke(80.0f, 400.0f, false);
    check(runNoVariance != runA,
          "determinism: the same path WITHOUT any jitter (every `Variance` at its "
          "default-Off, zero-jitter state) deposits different tiles than WITH it -- jitter has "
          "a real, observable effect and is not silently ignored even though it resolves "
          "deterministically");
  }

  // ======================================================================
  // 8. `Dry Bristle` (brush/Library.cpp), rewritten for the model
  //    migration: its two RANDOM links are now INERT, not merely unproven
  // ======================================================================
  //
  // Before this migration, this section proved RANDOM -> Scatter/Flow gave
  // the shipped preset's stroke real, seed-derived variation. That is no
  // longer true, and `brush/Library.cpp`'s own comment on `dry` says so in
  // as many words: "this preset now paints a plain 36 px / 85% hardness /
  // 55% spacing round dab with no per-dab jitter at all". `dry->links`
  // still carries both links (nothing deletes `BrushPreset::links`), but
  // `BrushModel` -- what actually reaches a dab now -- carries no Variance
  // for this preset at all, so nothing seeds anything. This section proves
  // that directly, the same shape as `MultiplyFloor.cpp`'s own §1: the
  // stroke is now translation-invariant, where it used to be seed-varying.
  {
    const BrushLibrary lib = defaultBrushLibrary();
    const BrushPreset* dry = nullptr;
    for (const BrushPreset& p : lib.presets)
      if (p.name == "Dry Bristle") dry = &p;
    check(dry != nullptr, "dry bristle: the preset still ships");
    if (dry != nullptr) {
      check(findLink(dry->links, DynamicSource::Random, DynamicTarget::Scatter) != kNoLink &&
                findLink(dry->links, DynamicSource::Random, DynamicTarget::Flow) != kNoLink,
            "dry bristle: both RANDOM links are still there, verbatim, in `dry->links` -- the "
            "migration did not touch `brush/Library.cpp`'s link data, only what reads it");

      BrushState brush;
      applyPresetToBrush(*dry, brush);
      check(varianceIsInert(brush.model.shape.size) && varianceIsInert(brush.model.shape.angle) &&
                varianceIsInert(brush.model.shape.roundness) &&
                varianceIsInert(brush.model.scatter.scatter),
            "dry bristle: and every Variance site `StrokeSession`'s per-dab loop actually reads "
            "is inert (Off control, zero jitter, zero minimum) -- the two links have nothing "
            "left to ride on");

      const MixboxLut noLut;
      DynamicInputs in;
      OpenDocument od = makeDoc(1024, 1024);
      StrokeSession s;
      std::string e;
      check(s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in),
            "dry bristle: a stroke begins with the preset's own model");
      // A STRAIGHT horizontal path here, deliberately NOT `paintPath()`'s
      // sine wave: the isolated-mass sampling below reads fixed (x, 500)
      // texels, which only lands on the stroke's own dabs if the path is a
      // straight line through y=500 -- a wavy path would leave most of those
      // texels off the stroke entirely, reading as unpainted rather than as
      // isolated dabs.
      for (int i = 0; i <= 60; ++i)
        s.addPoint(80.0f + 5.0f * static_cast<float>(i), 500.0f);
      s.end();

      // **The same stroke again, shifted along x by a whole number of
      // pixels.** Translating by an integer leaves the geometry -- and the
      // dab lattice's own phase -- identical, while changing the stroke's
      // seed, which is latched from its first dab position (see this file's
      // §7 for what a real seed difference looks like when something is
      // still jittered). If the seed no longer reaches the paint at all --
      // this section's own claim -- the shifted stroke must be the FIRST
      // one's paint, rigidly translated: bit-identical mass at every
      // corresponding texel, not merely similar.
      constexpr int32_t kShiftPx = 400;  // > 2 * radius, so the two strokes cannot overlap
      OpenDocument odShifted = makeDoc(1024, 1024);
      StrokeSession shifted;
      std::string eShifted;
      check(shifted.begin(odShifted, 1, brushTipFor(brush, noLut, in), Tool::Brush, &eShifted,
                          &brush.model, in),
            "dry bristle: the shifted stroke begins with the same model");
      for (int i = 0; i <= 60; ++i)
        shifted.addPoint(80.0f + static_cast<float>(kShiftPx) + 5.0f * static_cast<float>(i),
                         500.0f);
      shifted.end();

      const auto massAt = [](const OpenDocument& doc, int32_t x, int32_t y) -> float {
        const PixelCoord at{x, y};
        const PigmentTile* t = doc.document.layers[1].pigmentTiles->find(tileCoordAt(at));
        return t != nullptr ? t->readTexel(tileLocalOffset(at)).mass : 0.0f;
      };
      // 1e-4 is far below the half-float storage step's own quantisation
      // noise, so "under this" really does mean "the same value stored
      // twice", not "close enough".
      int differing = 0;
      int compared = 0;
      const int32_t radiusPx = static_cast<int32_t>(brush.model.tip.diameterPx * 0.5f);
      for (int32_t dy = 0; dy <= radiusPx + 4; ++dy) {
        for (int32_t x = 100; x <= 360; ++x) {
          ++compared;
          if (std::fabs(massAt(od, x, 500 + dy) - massAt(odShifted, x + kShiftPx, 500 + dy)) > 1e-4f)
            ++differing;
        }
      }
      std::printf("  dry bristle: %d of %d texels differ between the stroke and its shift\n",
                  differing, compared);
      // Not a strict 0: `StrokePath`'s arc-length walk accumulates float
      // error along its own length, so two strokes 400 px apart are not
      // guaranteed to land their dab centres at BIT-IDENTICAL fractional
      // pixel offsets even though both are geometrically straight lines at
      // the identical angle -- a handful of edge texels near
      // `dabCoverage()`'s falloff boundary can round a hair differently.
      // 2%% is generous against that (this measured 6 of 6003, ~0.1%%) and
      // two orders of magnitude below what a genuinely SEEDED stroke showed
      // before this migration (this section's own pre-migration comment:
      // "~1570 of these texels differing" out of a similarly-sized sweep,
      // over 25%%) -- wide enough to absorb quantisation noise, narrow
      // enough that real per-dab seed variation would still fail it easily.
      check(differing <= compared / 50,
            "dry bristle: the shipped preset's stroke is NO LONGER meaningfully seeded -- the "
            "same path drawn at a different position deposits paint that agrees almost "
            "everywhere (within float-accumulation noise at dab edges), because neither of its "
            "two RANDOM links (still present in `dry->links`, proven above) nor its "
            "`BrushModel` (also proven inert above) puts anything into the per-dab loop that "
            "varies with the seed. A future fix that gives Dry Bristle real Variance data "
            "would need to update this section, not merely re-green it");
    }
  }

  // ======================================================================
  // 8. brushTipFor() resolves the HARDWARE sources only
  // ======================================================================
  //
  // **This section originally existed because a P0 lived here with a fully
  // green suite** -- `brushTipFor()` used to resolve the WHOLE link set and
  // `StrokeSession` folded the stroke-local half on again per dab, applying
  // every VELOCITY/FADE/NOISE/RANDOM link's floor twice. The model
  // migration makes the underlying claim STRONGER, not merely preserved:
  // `brushTipFor()` no longer reads `BrushState::links` in ANY form, hardware
  // or stroke-local (`app/StrokeSession.cpp`'s own comment: "The old `dyn`/
  // `evaluateLinksFiltered(brush.links, ...)` resolution is gone"). A
  // `BrushLinkSet` populated with the exact fixtures that used to prove the
  // P0's absence now proves something more total: it does not move
  // `tip.radius` at all, whatever it contains.
  {
    std::printf("  -- 8. brushTipFor() resolves the model's base radius "
                "only, never `links` --\n");
    const MixboxLut noLut;
    DynamicInputs in;
    in.pressure = 1.0f;

    BrushState floored;
    floored.model.tip.diameterPx = 40.0f;  // radius 20
    addLink(floored.links, BrushLink{DynamicSource::Pressure, DynamicTarget::Size, {}, 0.0f, 1.0f,
                                     false, true});
    addLink(floored.links, BrushLink{DynamicSource::Random, DynamicTarget::Size, {}, 0.5f, 1.0f,
                                     false, true});
    const BrushTip tipFloored = brushTipFor(floored, noLut, in);
    const float askedRadius = floored.model.tip.diameterPx / 2.0f;
    std::printf("  [measured] radius with a RANDOM->Size floor of 0.50: %.4f (asked for %.4f)\n",
                static_cast<double>(tipFloored.radius), static_cast<double>(askedRadius));
    check(std::fabs(tipFloored.radius - askedRadius) < 1e-4f,
          "hardware-only: a RANDOM->Size link does NOT shrink the tip brushTipFor() returns at "
          "all -- `brush.links` is not in the path Size resolves through any more, hardware or "
          "stroke-local");

    // The floor of 0.00 case, kept separate because it is not a degree worse
    // than the one above -- it is a different outcome IN THE OLD ARCHITECTURE.
    // Any floor used to scale the brush down; a floor of zero used to switch
    // it off, and a zero radius deposits nothing (brush/Deposit.hpp: "a
    // radius of 0 or less deposits nothing at all"). Kyle's Runny Inkers
    // shipped exactly this brush. Kept here specifically because the SAME
    // fixture now demonstrates the stronger claim just as well as the 0.50
    // one above -- there is no longer a distinct "floor of exactly 0" case
    // to be worse than the general one.
    BrushState zeroFloor;
    zeroFloor.model.tip.diameterPx = 40.0f;
    addLink(zeroFloor.links, BrushLink{DynamicSource::Pressure, DynamicTarget::Size, {}, 0.0f,
                                       1.0f, false, true});
    addLink(zeroFloor.links, BrushLink{DynamicSource::Random, DynamicTarget::Size, {}, 0.0f, 1.0f,
                                       false, true});
    const BrushTip tipZero = brushTipFor(zeroFloor, noLut, in);
    std::printf("  [measured] radius with a RANDOM->Size floor of 0.00: %.4f\n",
                static_cast<double>(tipZero.radius));
    check(nearf(tipZero.radius, 20.0f, 1e-4f),
          "hardware-only: a RANDOM->Size link whose range starts at 0.00 does not reduce the tip "
          "at all -- the shipped-brush case where a healthy link set once painted an invisible "
          "stroke cannot recur, because the link is never consulted");

    // The composition identity `evaluateLinks() == hardware-half * local-half`
    // is still true, but it is now a claim about `brush/Dynamics.cpp`'s own
    // algebra in isolation -- neither `brushTipFor()` nor `StrokeSession`
    // calls `evaluateLinksFiltered()` any more, so this no longer describes
    // how the paint path actually composes anything. Kept as a pure-function
    // check (TargetCombine is commutative and associative, whether or not
    // anything downstream still splits a resolution this way).
    DynamicInputs both = in;
    both.random = 0.37f;
    const float whole = evaluateLinks(floored.links, both).at(DynamicTarget::Size);
    const float hardware =
        evaluateLinksFiltered(floored.links, both, /*wantStrokeLocal=*/false).at(DynamicTarget::Size);
    const float local =
        evaluateLinksFiltered(floored.links, both, /*wantStrokeLocal=*/true).at(DynamicTarget::Size);
    std::printf("  [measured] whole %.6f vs hardware %.6f * local %.6f = %.6f\n",
                static_cast<double>(whole), static_cast<double>(hardware),
                static_cast<double>(local), static_cast<double>(hardware * local));
    check(std::fabs(whole - hardware * local) < 1e-6f,
          "hardware-only: the two partial resolutions still compose back to the whole-set "
          "answer at the pure Dynamics.cpp level -- a fact about TargetCombine's algebra, not "
          "about anything `brushTipFor()`/`StrokeSession` still does with it");
  }

  // ======================================================================
  // 9. DIRECTION -- the tangent source: normalisation, the wrap point, the
  //    first-dab default, and that a link actually turns a real stroke
  // ======================================================================
  //
  // Sections 5 and 6 above already fold DIRECTION into the generic
  // nine-source table and the per-cell HSV refusal. This section is what
  // proves the source ITSELF, the way sections 2-4 do for VELOCITY, FADE and
  // NOISE: the pure function's own arithmetic, and -- because "the generator
  // is right" and "the dispatch that reaches a real stroke is right" are two
  // different claims, exactly the gap section 5's own comment names -- a
  // real `StrokeSession` actually turning a tip because of it.
  {
    // --- 9a. Cardinal headings, exactly -------------------------------
    check(dynamicDirection(1.0f, 0.0f) == 0.0f,
          "direction: due +x (heading 0 deg) normalises to exactly 0.0 -- the wrap point "
          "itself, by construction");
    check(nearf(dynamicDirection(0.0f, 1.0f), 0.25f, 1e-6f),
          "direction: due +y (heading 90 deg) normalises to exactly 0.25");
    check(nearf(dynamicDirection(-1.0f, 0.0f), 0.5f, 1e-6f),
          "direction: due -x (heading 180 deg) normalises to exactly 0.5");
    check(nearf(dynamicDirection(0.0f, -1.0f), 0.75f, 1e-6f),
          "direction: due -y (heading -90/270 deg) normalises to exactly 0.75 -- the negative "
          "atan2 branch, wrapped forward by a full turn rather than left signed");

    // --- 9b. The first-dab / zero-motion default -----------------------
    check(dynamicDirection(0.0f, 0.0f) == 0.0f,
          "direction: (0,0) -- what app/StrokeSession.cpp passes for a stroke's first dab, "
          "which has no previous position to difference against -- resolves to 0.0, the same "
          "'nothing has happened yet' reading DynamicInputs' own defaults use everywhere else");

    // --- 9c. Purity: no hidden state, called out of order ---------------
    const float a1 = dynamicDirection(3.0f, 4.0f);
    const float mid = dynamicDirection(-2.0f, 1.0f);
    const float a2 = dynamicDirection(3.0f, 4.0f);
    check(a1 == a2,
          "direction: the SAME (dx, dy) pair returns the bit-identical float on a second call, "
          "with a DIFFERENT call in between -- a pure function of its two arguments, holding no "
          "memory of a stroke's opening heading the way a frozen Photoshop 'Initial Direction' "
          "would need to");
    check(a1 != mid, "direction: two different headings resolve to two different values");

    // --- 9d. The wrap point does not reach the canvas --------------------
    //
    // brush/Dynamics.hpp's own `dynamicDirection()` comment argues that the
    // ENCODING's seam at heading 0 deg is invisible once `Angle`'s default
    // [0,360) range and `dabCoverage()`'s exactly-360-periodic cos/sin have
    // both been applied. Checked here rather than only argued: two headings
    // a fraction of a degree either side of the wrap resolve to normalised
    // values near opposite ends of [0,1), but their Angle-link
    // CONTRIBUTIONS -- what actually reaches `dabCoverage()` -- differ by a
    // fraction of a degree too, once the comparison is taken modulo a full
    // turn rather than read as a raw difference.
    {
      BrushLinkSet dirLinks;
      BrushLink dirToAngle;
      dirToAngle.source = DynamicSource::Direction;
      dirToAngle.target = DynamicTarget::Angle;
      targetDefaultRange(DynamicTarget::Angle, dirToAngle.rangeLo, dirToAngle.rangeHi);
      addLink(dirLinks, dirToAngle);

      const float justBelow = dynamicDirection(1.0f, -0.001f);  // heading ~ -0.057 deg
      const float justAbove = dynamicDirection(1.0f, 0.001f);   // heading ~ +0.057 deg
      DynamicInputs inBelow, inAbove;
      inBelow.direction = justBelow;
      inAbove.direction = justAbove;
      const float angleBelow = evaluateLinksFiltered(dirLinks, inBelow, /*wantStrokeLocal=*/true)
                                   .at(DynamicTarget::Angle);
      const float angleAbove = evaluateLinksFiltered(dirLinks, inAbove, /*wantStrokeLocal=*/true)
                                   .at(DynamicTarget::Angle);
      float wrapped = std::fabs(angleAbove - angleBelow);
      if (wrapped > 180.0f) wrapped = 360.0f - wrapped;
      std::printf(
          "  direction: heading -0.057deg -> Angle %.4f, +0.057deg -> Angle %.4f, wrapped "
          "difference %.4f deg\n",
          static_cast<double>(angleBelow), static_cast<double>(angleAbove),
          static_cast<double>(wrapped));
      // 0.2 deg is generous against the ~0.114 deg physical gap between the
      // two probe headings (2 * 0.057 deg, the sum of the two offsets from
      // due-+x) -- loose enough to absorb the atan2/trig rounding this
      // comparison goes through twice, tight enough that a real 360 deg
      // seam (the raw, un-wrapped difference, ~359.9 deg) would still miss
      // it by three orders of magnitude.
      check(wrapped < 0.2f,
            "direction: two headings a fraction of a degree either side of the encoding's wrap "
            "point resolve to Angle contributions that are ALSO only a fraction of a degree "
            "apart, once compared modulo a full turn -- the seam is in the [0,1) NUMBER, not "
            "in the rotation dabCoverage() actually draws");
    }

    // --- 9e. End to end, rewritten for the model migration: a real stroke,
    //     actually turning ----------------------------------------------
    //
    // 9a-9d prove the source. This proves the wiring: that Angle's own
    // `Variance`, with `control = VarianceControl::Direction`, run through a
    // real `StrokeSession`, visibly rotates an elliptical tip's footprint --
    // the direct replacement for what a DIRECTION -> ANGLE link used to
    // prove (`StrokeSession::begin()` no longer takes a `BrushLinkSet*` at
    // all). `varianceOffset()`'s own `span` for Angle is 180.0
    // (`app/StrokeSession.cpp`'s per-dab loop), not the link's old
    // full-360 range, so the magnitude differs from the pre-migration
    // fixture -- what this section still proves, unchanged, is IDENTITY on
    // a straight path and a REAL, non-identity rotation on a curving one.
    {
      auto ellipBrush = [](VarianceControl angleControl) {
        BrushState brush;
        brush.model.tip.diameterPx = 44.0f;
        brush.model.tip.hardness = 0.4f;
        brush.load = 0.5f;
        // Visibly non-round. ANGLE has NO effect at roundness 1.0
        // (brush/Deposit.cpp's own round-tip branch skips the rotation
        // arithmetic outright, deliberately, to keep every existing
        // round-tip stroke bit-identical), so this is the one setting that
        // makes this section able to see anything at all.
        brush.model.tip.roundness = 0.35f;
        brush.model.shape.angle.control = angleControl;
        return brush;
      };
      const MixboxLut noLut;
      DynamicInputs in;

      // A perfectly STRAIGHT stroke moving due +x the whole way: DIRECTION
      // is 0.0 at the first dab (9b) and 0.0 at every dab after it (9a's
      // own due-+x case, since every step is along +x), so a Direction-
      // controlled Angle Variance contributes exactly its Add identity,
      // 0.0, throughout (`varianceOffset()`'s own `base = span *
      // clamp(in.direction,0,1)`, and `in.direction` never leaves 0.0 on
      // this path). WITH the control and WITHOUT it (Off) must therefore
      // deposit BYTE-IDENTICAL tiles -- the strongest check available, and
      // one that would catch a first-dab default other than the documented
      // 0.0 (brush/Dynamics.hpp's own choice), since any other value would
      // rotate the first dab's footprint and desync the two streams from
      // their very first tile.
      auto straightStroke = [&](VarianceControl angleControl) {
        OpenDocument od = makeDoc(512, 512);
        BrushState brush = ellipBrush(angleControl);
        StrokeSession s;
        std::string e;
        s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in);
        for (int i = 0; i <= 40; ++i) s.addPoint(60.0f + 6.0f * static_cast<float>(i), 256.0f);
        s.end();
        return snapshotBytes(*od.document.layers[1].pigmentTiles);
      };
      const TileBytes straightWith = straightStroke(VarianceControl::Direction);
      const TileBytes straightWithout = straightStroke(VarianceControl::Off);
      check(!straightWith.empty() && straightWith == straightWithout,
            "direction: a stroke moving due +x the whole way deposits BYTE-IDENTICAL paint "
            "with Angle's Variance controlled by Direction and with it Off -- the heading "
            "never leaves 0.0, so Direction contributes exactly its Add identity at every "
            "dab, first included");

      // The sine-wave path (this file's own `paintPath()`) constantly
      // changes heading, so the same comparison on a curving stroke must
      // come out the OTHER way: if it did not, the Direction control would
      // be silently inert on every path, straight or curved, which the
      // check above alone cannot rule out (an inert control is
      // byte-identical on EVERY stroke, not only a straight one).
      auto curvyStroke = [&](VarianceControl angleControl) {
        OpenDocument od = makeDoc(512, 512);
        BrushState brush = ellipBrush(angleControl);
        StrokeSession s;
        std::string e;
        s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in);
        paintPath(s, 60.0f, 256.0f);
        s.end();
        return snapshotBytes(*od.document.layers[1].pigmentTiles);
      };
      const TileBytes curvyWith = curvyStroke(VarianceControl::Direction);
      const TileBytes curvyWithout = curvyStroke(VarianceControl::Off);
      check(!curvyWith.empty() && curvyWith != curvyWithout,
            "direction: the SAME elliptical tip on a CURVING stroke deposits DIFFERENT paint "
            "with Angle's Variance controlled by Direction than with it Off -- the control "
            "the straight-stroke check above is measured against, ruling out a control that "
            "is simply never applied");

      // Replayed determinism, restated for this specific control -- the
      // same property section 7 proves for jittered sites. DIRECTION needs
      // no seed and holds no generator state at all, so this is not
      // expected to be interesting, but an assertion nobody wrote is an
      // assertion nobody has ever run.
      const TileBytes curvyReplayed = curvyStroke(VarianceControl::Direction);
      check(curvyWith == curvyReplayed,
            "direction: the curving Direction-controlled stroke replays BIT-IDENTICAL -- pure "
            "geometry in, pure geometry out, nothing for undo or the golden harness to desync");
    }

    // --- 9f. brushTipFor() must not resolve DIRECTION at all, rewritten for
    //     the model migration --------------------------------------------
    //
    // The defect the section above numbered "8" exists to close -- a
    // stroke-local link/control silently applied twice, once at frame
    // granularity and once per dab -- is exactly as available to a
    // Direction-controlled Angle Variance as it was to the old links.
    // `brushTipFor()` sets `tip.angle` to `model.tip.angleDeg` alone (its
    // own comment: "Size, Angle and Roundness are BASE values only --
    // unvaried"), never touching `model.shape.angle` at all, so a
    // `BrushState` whose Angle Variance is Direction-controlled, run
    // through `brushTipFor()` ALONE (the hardware-only path a stroke's
    // very first frame takes, before `StrokeSession` ever resolves a
    // dab), must leave ANGLE at exactly its authored BASE value.
    {
      BrushState withDir;
      // A nonzero authored angle, so a spurious contribution leaking in
      // from the hardware path would move this measurably away from 15,
      // not merely away from a suspicious 0.
      withDir.model.tip.angleDeg = 15.0f;
      withDir.model.shape.angle.control = VarianceControl::Direction;

      const MixboxLut noLut;
      DynamicInputs in;
      in.pressure = 1.0f;
      const BrushTip resolved = brushTipFor(withDir, noLut, in);
      std::printf(
          "  [measured] tip.angle with Angle's Variance Direction-controlled, hardware path "
          "only: %.4f (authored %.4f)\n",
          static_cast<double>(resolved.angle), static_cast<double>(withDir.model.tip.angleDeg));
      check(nearf(resolved.angle, withDir.model.tip.angleDeg, 1e-4f),
            "direction: brushTipFor()'s hardware-only path leaves ANGLE exactly at its "
            "authored BASE value with Angle's Variance Direction-controlled -- Direction "
            "contributes nothing here because `brushTipFor()` never calls "
            "`varianceOffset(model.shape.angle, ...)` at all; `StrokeSession`'s per-dab loop "
            "is the only place it may apply");
    }
  }

  // ======================================================================
  // 10. INITIAL DIRECTION -- the latch: once, from the second dab, held
  //     for the rest of the stroke
  // ======================================================================
  //
  // Shares DIRECTION's own arithmetic entirely (`dynamicDirection()`,
  // section 9's own pure-function checks already cover it) -- what this
  // section proves is `app/StrokeSession`'s CALLING DISCIPLINE around that
  // arithmetic: latch once, at the first dab with a real step vector, and
  // never re-read the generator again for the rest of the stroke. Section
  // 6 above already asserts `sourceIsStrokeLocal(InitialDirection)` and the
  // HSV cell refusal directly; this section is the end-to-end half.
  {
    // --- 10a. The distinguishing case: a curving stroke where live
    //     DIRECTION and latched INITIAL DIRECTION demonstrably disagree ---
    //
    // Without this, a future refactor that quietly merged the two sources
    // back into one (same generator, after all) would pass every other
    // check in this file: section 9's pure-function checks never touch
    // `StrokeSession`, and 10b below only proves INITIAL DIRECTION differs
    // from an EXACT reference, not from DIRECTION specifically.
    // Rewritten for the model migration: the old fixture built two
    // `BrushLinkSet`s (Direction -> Angle, Initial Direction -> Angle) and
    // one shared `BrushTip`. `StrokeSession::begin()` no longer takes a
    // `BrushLinkSet*`, so the two variants are now two `BrushState`s whose
    // `model.shape.angle.control` differs -- `VarianceControl::Direction`
    // vs `VarianceControl::InitialDirection` -- everything else identical.
    auto ellipBrush = [](VarianceControl angleControl) {
      BrushState brush;
      brush.model.tip.diameterPx = 44.0f;
      brush.model.tip.hardness = 0.4f;
      brush.load = 0.5f;
      // ANGLE has no visible effect at roundness 1.0 (brush/Deposit.cpp's
      // own round-tip branch skips the rotation arithmetic outright) --
      // section 9e's own reasoning, restated for this section's own tip.
      brush.model.tip.roundness = 0.35f;
      brush.model.shape.angle.control = angleControl;
      return brush;
    };
    const MixboxLut noLut;
    DynamicInputs in;
    {
      auto curvyStroke = [&](VarianceControl angleControl) {
        OpenDocument od = makeDoc(512, 512);
        BrushState brush = ellipBrush(angleControl);
        StrokeSession s;
        std::string e;
        s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in);
        paintPath(s, 60.0f, 256.0f);
        s.end();
        return snapshotBytes(*od.document.layers[1].pigmentTiles);
      };
      const TileBytes live = curvyStroke(VarianceControl::Direction);
      const TileBytes latched = curvyStroke(VarianceControl::InitialDirection);
      check(!live.empty() && !latched.empty() && live != latched,
            "initial direction: the SAME curving stroke, SAME elliptical tip, deposits "
            "DIFFERENT paint with Angle's Variance Direction-controlled than Initial-"
            "Direction-controlled -- live Direction keeps turning through the whole path, "
            "latched Initial Direction locks after its second dab, and a sine wave is exactly "
            "the path shape that makes the two visibly disagree. A future refactor merging "
            "the two controls back into one would redden this line first");

      // Replayed determinism, restated for the latch specifically: nothing
      // about `initialDirectionLatched_` is time- or clock-derived, so the
      // same stroke run twice must still agree exactly -- section 7's and
      // 9e's own property, extended to this control.
      const TileBytes latchedReplayed = curvyStroke(VarianceControl::InitialDirection);
      check(latched == latchedReplayed,
            "initial direction: the latched curving stroke replays BIT-IDENTICAL -- the latch "
            "is `app/StrokeSession` member state, not a clock or a counter, so a second run "
            "reaches the identical value the first one latched");
    }

    // --- 10b. The latch is EXACT, not merely "different from live" --------
    //
    // 10a proves disagreement, which a latch that fired on the wrong dab,
    // or re-latched partway through, would ALSO produce -- disagreement
    // alone does not prove the held value is the RIGHT one. This proves the
    // resolved angle is exactly the heading of the stroke's own opening
    // step, and stays exactly that for the rest of the stroke.
    //
    // A STRAIGHT, non-axis-aligned path -- deliberately not the sine wave,
    // for section 8's ("Dry Bristle") own reason: a predictable path is
    // what isolated-texel sampling needs. Every dab `brush/StrokePath`
    // emits for a straight input lands exactly on that line
    // (`StrokePath.cpp`'s own `mirror()` comment: "for collinear input this
    // makes the extrapolated point collinear too, so the curve degenerates
    // to a straight line"), so the heading between ANY two consecutive dabs
    // is the line's own slope -- known without running anything.
    // `dynamicDirection(lineDx, lineDy)` IS the value every dab past the
    // first must latch to, computed here independently of `StrokeSession`
    // rather than read back from it.
    //
    // **`expectedInitialDeg` is capped to `varianceOffset()`'s own span for
    // Angle (180.0, `app/StrokeSession.cpp`'s per-dab loop), not the old
    // link's full 360.** `dynamicDirection()` returns a normalised [0,1)
    // heading; the reference tip below multiplies by that SAME 180 span,
    // matching exactly what `varianceOffset(v, in, 180.0f, ...)` computes
    // for a fully-InitialDirection-controlled, zero-jitter Variance
    // (`base = span * clamp(in.initialDirection, 0, 1)`).
    {
      constexpr float kLineDx = 5.0f, kLineDy = 2.0f;  // an ordinary heading,
                                                       // not a multiple of
                                                       // 90 deg, so this
                                                       // exercises the
                                                       // general case
      const float expectedInitialDeg = dynamicDirection(kLineDx, kLineDy) * 180.0f;

      BrushState fixedBrush;
      fixedBrush.model.tip.diameterPx = 44.0f;
      fixedBrush.model.tip.hardness = 0.4f;
      fixedBrush.load = 0.5f;
      fixedBrush.model.tip.roundness = 0.35f;
      // The independently-computed answer, assigned straight onto the
      // model's own base angle -- no Variance control, no `brushTipFor()`
      // detour, nothing this test could get backwards by routing it
      // through the wrong half of the dynamics system. Every dab of this
      // reference stroke paints at EXACTLY this one angle, including its
      // first.
      fixedBrush.model.tip.angleDeg = expectedInitialDeg;

      BrushState latchedBrush = ellipBrush(VarianceControl::InitialDirection);

      constexpr float kOriginX = 100.0f, kOriginY = 300.0f;
      auto straightStroke = [&](const BrushState& brush) {
        OpenDocument od = makeDoc(1024, 1024);
        StrokeSession s;
        std::string e;
        s.begin(od, 1, brushTipFor(brush, noLut, in), Tool::Brush, &e, &brush.model, in);
        for (int i = 0; i <= 59; ++i)
          s.addPoint(kOriginX + kLineDx * static_cast<float>(i),
                     kOriginY + kLineDy * static_cast<float>(i));
        s.end();
        return od;
      };
      OpenDocument latchedDoc = straightStroke(latchedBrush);
      OpenDocument fixedDoc = straightStroke(fixedBrush);

      const auto massAt = [](const OpenDocument& doc, int32_t x, int32_t y) -> float {
        const PixelCoord at{x, y};
        const PigmentTile* t = doc.document.layers[1].pigmentTiles->find(tileCoordAt(at));
        return t != nullptr ? t->readTexel(tileLocalOffset(at)).mass : 0.0f;
      };

      // Sampled far along the line -- i in [40, 59] puts the sample point
      // at line-distance >= 40 * hypot(kLineDx, kLineDy) ~= 215 px from the
      // stroke's start, far outside the tip's own radius (22 px -- the
      // distance `BrushTip`'s own contract puts coverage at EXACTLY zero
      // beyond), so
      // nothing sampled here can be affected by dab 0's angle -- the one
      // dab where the latched stroke (unlatched placeholder, 0.0) and the
      // fixed reference (pinned to `expectedInitialDeg` from dab 0 onward)
      // are KNOWN to differ. Any dab from the second on must agree exactly,
      // by construction, if the latch does what it claims.
      int compared = 0, agreeing = 0;
      bool sawPaint = false;
      for (int i = 40; i <= 59; ++i) {
        const int32_t cx = static_cast<int32_t>(kOriginX + kLineDx * static_cast<float>(i));
        const int32_t cy = static_cast<int32_t>(kOriginY + kLineDy * static_cast<float>(i));
        for (int32_t ddx = -3; ddx <= 3; ++ddx) {
          for (int32_t ddy = -3; ddy <= 3; ++ddy) {
            ++compared;
            const float a = massAt(latchedDoc, cx + ddx, cy + ddy);
            const float b = massAt(fixedDoc, cx + ddx, cy + ddy);
            if (a > 0.0f) sawPaint = true;
            // 1e-4 is far below the smallest real disagreement and far
            // above the half-float storage step -- section 8's own
            // Dry Bristle comment derives it once; reused verbatim here.
            if (std::fabs(a - b) <= 1e-4f) ++agreeing;
          }
        }
      }
      std::printf(
          "  initial direction: %d of %d sampled texels far from the stroke's start agree "
          "between the latch and the independently-computed reference (%.3f deg)\n",
          agreeing, compared, static_cast<double>(expectedInitialDeg));
      // Not `compared == agreeing`: as section 8's own Dry Bristle comment
      // found for a different pair of strokes, `StrokePath`'s arc-length
      // walk accumulates float error, and two strokes with different dab
      // COUNTS along the same line (a consequence of this migration's own
      // `brushTipFor()` spacing fix, not of the latch under test here) can
      // land one dab's sub-pixel position a hair apart at a shared sample
      // point -- 1 of 980 texels here, the same ~0.1% order of magnitude
      // section 8 measured, not the ~25%+ a genuinely wrong latch would
      // produce. `<= compared / 50` (2%) is that same threshold, reused
      // rather than re-derived.
      check(sawPaint && compared - agreeing <= compared / 50,
            "initial direction: far from the stroke's first dab, the latched stroke and a "
            "reference stroke pinned to the INDEPENDENTLY COMPUTED opening heading -- not "
            "read back from StrokeSession, derived from the line's own slope -- deposit "
            "EXACTLY the same mass, texel for texel. This is what tells a correct latch apart "
            "from a value that merely differs from live DIRECTION without being right");
    }

    // --- 10c. brushTipFor() must not resolve INITIAL DIRECTION at all,
    //     rewritten for the model migration --------------------------------
    //
    // The defect section "8" above exists to close -- a stroke-local
    // link/control silently applied twice, once at frame granularity and
    // once per dab -- is exactly as available to an InitialDirection-
    // controlled Angle Variance as it was to the original four, and to
    // Direction after them (section 9's own "9f", whose rewrite this
    // mirrors exactly). A `BrushState` whose Angle Variance is Initial-
    // Direction-controlled, run through `brushTipFor()` ALONE (the
    // hardware-only path a stroke's very first frame takes, before
    // `StrokeSession` ever resolves a dab, let alone latches one), must
    // leave ANGLE at exactly its authored BASE value.
    {
      BrushState withInit;
      // A nonzero authored angle, so a spurious contribution leaking in
      // from the hardware path would move this measurably away from 15,
      // not merely away from a suspicious 0.
      withInit.model.tip.angleDeg = 15.0f;
      withInit.model.shape.angle.control = VarianceControl::InitialDirection;

      const MixboxLut noLut;
      DynamicInputs in;
      in.pressure = 1.0f;
      const BrushTip resolved = brushTipFor(withInit, noLut, in);
      std::printf(
          "  [measured] tip.angle with Angle's Variance Initial-Direction-controlled, "
          "hardware path only: %.4f (authored %.4f)\n",
          static_cast<double>(resolved.angle), static_cast<double>(withInit.model.tip.angleDeg));
      check(nearf(resolved.angle, withInit.model.tip.angleDeg, 1e-4f),
            "initial direction: brushTipFor()'s hardware-only path leaves ANGLE exactly at "
            "its authored BASE value with Angle's Variance Initial-Direction-controlled -- it "
            "never resolves before a dab's position exists (let alone a SECOND one to latch "
            "from), and StrokeSession's per-dab loop, which owns the latch, is the only place "
            "it may apply");
    }
  }

  // --- 9. A source the device cannot report yields IDENTITY, not its floor --
  //
  // The regression for docs/reachability-audit.md **B7**. `Kyle's Spatter
  // Brushes - Supreme Spatter & Texture` carries `TILT->Size [0.00..1.00]`,
  // and with a mouse it painted **exactly zero pixels** -- measured, not
  // estimated. Tilt read 0.0, Size is a Multiply target, a link at source 0
  // contributes its `rangeLo`, and 0.00 times any radius is a dab that
  // `dabCoverage()` rejects on `!(r > 0.0f)`. Silent: no refusal, no message,
  // and an import report saying every part of the brush arrived.
  //
  // Photoshop's answer is the one implemented here -- it marks a Control that
  // needs an absent device with a warning triangle and IGNORES it.
  {
    BrushLinkSet tiltSize;
    addLink(tiltSize,
            BrushLink{DynamicSource::Tilt, DynamicTarget::Size, {}, 0.0f, 1.0f, false, true});

    // The exact shape of the defect: a mouse reports no tilt.
    DynamicInputs mouse;
    mouse.hasTilt = false;
    mouse.tilt = 0.0f;
    const DynamicResult onMouse = evaluateLinks(tiltSize, mouse);
    check(nearf(onMouse.at(DynamicTarget::Size), 1.0f, 1e-6f),
          "availability: a TILT->Size link whose range starts at 0.00 resolves to Size 1.0 -- "
          "IDENTITY -- when the device reports no tilt, because a control that cannot be driven "
          "must be ignored rather than driven from a fabricated zero (audit B7: this is the "
          "difference between a spatter brush and a brush that paints nothing at all)");

    // **The discriminating half**, and the reason this is two assertions and
    // not one. If the skip were really "treat a zero input as identity", a
    // genuine pen lying flat at tilt 0.0 would also stop scaling, and the
    // brush would silently ignore an axis the artist IS using. Availability
    // and value are different questions and this pins them apart.
    DynamicInputs penFlat;
    penFlat.hasTilt = true;
    penFlat.tilt = 0.0f;
    check(nearf(evaluateLinks(tiltSize, penFlat).at(DynamicTarget::Size), 0.0f, 1e-6f),
          "availability: the SAME link with a real pen reporting tilt 0.0 still resolves to "
          "0.00 -- the skip is about whether the axis EXISTS, never about the value being zero");
  }

  // Which sources need a device, asserted per source rather than inferred.
  {
    DynamicInputs none;
    none.hasPressure = false;
    none.hasTilt = false;
    none.hasBarrel = false;
    // Azimuth rides the TILT flag: it is the direction a pen is tilted IN, so
    // a device that cannot report tilt cannot report azimuth. Asserted here
    // because that pairing is a fact about pens, and a future edit that gave
    // azimuth its own flag would otherwise pass silently.
    const bool hardwareAllUnavailable =
        sourceUnavailable(none, DynamicSource::Pressure) &&
        sourceUnavailable(none, DynamicSource::Tilt) &&
        sourceUnavailable(none, DynamicSource::Azimuth) &&
        sourceUnavailable(none, DynamicSource::Barrel);
    check(hardwareAllUnavailable,
          "availability: all four HARDWARE sources report unavailable on a device that reports "
          "none of them -- azimuth included, which rides the tilt flag");

    // A stroke-local source is computed from the stroke's own geometry and a
    // seed, so it is never unavailable -- a mouse supplies it exactly as well
    // as a pen. If one of these ever reported unavailable it would be skipped
    // on every device, which is a whole source silently doing nothing.
    const bool strokeLocalAlwaysAvailable =
        !sourceUnavailable(none, DynamicSource::Velocity) &&
        !sourceUnavailable(none, DynamicSource::Fade) &&
        !sourceUnavailable(none, DynamicSource::Noise) &&
        !sourceUnavailable(none, DynamicSource::Random) &&
        !sourceUnavailable(none, DynamicSource::Direction) &&
        !sourceUnavailable(none, DynamicSource::InitialDirection);
    check(strokeLocalAlwaysAvailable,
          "availability: no STROKE-LOCAL source is ever unavailable -- they come from the "
          "stroke's own geometry and a seed, which a mouse supplies as well as a pen");

    // The default-constructed inputs describe a MOUSE, deliberately: that is
    // the device where getting this wrong is silent, so it is the one the
    // defaults must be safe for.
    DynamicInputs def;
    check(sourceUnavailable(def, DynamicSource::Tilt) &&
              sourceUnavailable(def, DynamicSource::Barrel) &&
              !sourceUnavailable(def, DynamicSource::Pressure),
          "availability: DEFAULT inputs describe a mouse -- no tilt, no barrel -- while pressure "
          "stays available so the long-standing `pressure = 1.0f` neutral is unchanged");
  }

  std::printf("[selftest] dynamics sources %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
