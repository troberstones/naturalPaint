#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"

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

  auto tip = [](float radius, float hardness, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = hardness;
    t.flow = flow;
    return t;
  };

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

    // The four stroke-local sources: each must vary across a realistic
    // sequence of (stepDist, distance, dabIndex) -- what a real stroke would
    // hand them. Table-shaped on purpose: a reader adding a ninth source
    // later sees exactly what row to add.
    struct Row {
      const char* name;
      bool varies;
    };
    std::vector<float> velocities, fades, noises, randoms;
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
    }
    auto spans = [](const std::vector<float>& v) {
      float lo = v[0], hi = v[0];
      for (float x : v) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
      return hi - lo;
    };
    const Row rows[8] = {
        {"PRESSURE", low.pressure != high.pressure},
        {"TILT", low.tilt != high.tilt},
        {"AZIMUTH", low.azimuth != high.azimuth},
        {"BARREL", low.barrel != high.barrel},
        {"VELOCITY", spans(velocities) > 0.05f},
        {"FADE", spans(fades) > 0.05f},
        {"NOISE", spans(noises) > 0.05f},
        {"RANDOM", spans(randoms) > 0.3f},
    };
    bool allVary = true;
    for (const Row& r : rows) {
      std::printf("  source table: %-10s %s\n", r.name, r.varies ? "varies" : "CONSTANT");
      if (!r.varies) allVary = false;
    }
    check(allVary,
          "sources: all eight vary across a realistic sequence -- the table that would have "
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
    const struct {
      DynamicSource source;
      float expected;
      const char* name;
    } dispatch[8] = {
        {DynamicSource::Pressure, probe.pressure, "PRESSURE"},
        {DynamicSource::Tilt, probe.tilt, "TILT"},
        {DynamicSource::Azimuth, probe.azimuth, "AZIMUTH"},
        {DynamicSource::Barrel, probe.barrel, "BARREL"},
        {DynamicSource::Velocity, probe.velocity, "VELOCITY"},
        {DynamicSource::Fade, probe.fade, "FADE"},
        {DynamicSource::Noise, probe.noise, "NOISE"},
        {DynamicSource::Random, probe.random, "RANDOM"},
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

    // Scatter and Concentration: cheap, local to the dab, exercised through a
    // REAL stroke -- StrokeSession's per-dab loop, with `strokeLocalLinks`
    // set, is the only place either one can show an effect (Scatter moves a
    // position; Concentration scales a mass this loop deposits).
    {
      BrushLinkSet links;
      BrushLink scatterLink;
      scatterLink.source = DynamicSource::Random;
      scatterLink.target = DynamicTarget::Scatter;
      scatterLink.rangeLo = 0.0f;
      scatterLink.rangeHi = 0.9f;  // most of a radius, so the effect is not lost in noise
      addLink(links, scatterLink);

      OpenDocument plain = makeDoc(512, 512);
      OpenDocument scattered = makeDoc(512, 512);
      {
        StrokeSession s;
        std::string e;
        s.begin(plain, 1, tip(20.0f, 0.4f, 0.5f), Tool::Brush, &e);
        paintPath(s, 60.0f, 256.0f);
        s.end();
      }
      {
        StrokeSession s;
        std::string e;
        s.begin(scattered, 1, tip(20.0f, 0.4f, 0.5f), Tool::Brush, &e, &links);
        paintPath(s, 60.0f, 256.0f);
        s.end();
      }
      check(snapshotBytes(*plain.document.layers[1].pigmentTiles) !=
                snapshotBytes(*scattered.document.layers[1].pigmentTiles),
            "targets: SCATTER, driven by RANDOM, visibly moves dabs off the raw path -- the "
            "byte-identical-without-it stroke is the control this diff is measured against");
    }
    {
      // CONCENTRATION is driven by PRESSURE here -- a hardware source -- so
      // it resolves through `brushTipFor()`'s frame-level path, exactly like
      // Hue/Saturation/Value below and NOT through `strokeLocalLinks_` (that
      // path only re-resolves VELOCITY/FADE/NOISE/RANDOM-sourced links --
      // `evaluateLinksFiltered(..., wantStrokeLocal=true)` would silently
      // skip a PRESSURE link, which is exactly the kind of mistake this
      // section exists to make impossible to make unnoticed: comparing
      // `tip.flow` straight out of `brushTipFor()` is the one comparison
      // that cannot be fooled by resolving the link in the wrong place).
      BrushState brush;
      brush.load = 0.5f;
      DynamicInputs in;
      in.pressure = 1.0f;
      const MixboxLut noLut;
      const BrushTip unthrottled = brushTipFor(brush, noLut, in);

      BrushLink concLink;
      concLink.source = DynamicSource::Pressure;
      concLink.target = DynamicTarget::Concentration;
      concLink.rangeLo = 0.2f;
      concLink.rangeHi = 0.2f;  // flat, so the result is independent of the
                                // pressure value and isolates CONCENTRATION
                                // from FLOW, which the tip already carries
      addLink(brush.links, concLink);
      const BrushTip throttled = brushTipFor(brush, noLut, in);

      std::printf("  concentration: unthrottled flow %.4f, CONCENTRATION 0.2 flow %.4f\n",
                  static_cast<double>(unthrottled.flow), static_cast<double>(throttled.flow));
      check(nearf(throttled.flow, unthrottled.flow * 0.2f, 1e-5f),
            "targets: CONCENTRATION scales the SAME product FLOW does -- a link pinning it to "
            "0.2 deposits exactly a fifth the flow of the unthrottled tip, not a coincidental "
            "amount");
    }

    // Hue/Saturation/Value: the frame-level path (brushTipFor(), driven by a
    // hardware source), which is fully live today with no gap -- see this
    // file's own header comment on why the stroke-local four do not reach
    // these three yet.
    {
      BrushState brush;
      brush.pigment = 0;
      const MixboxLut noLut;  // unused by this comparison; both sides take
                              // the identical no-LUT fallback, so only
                              // linearRgb's decode is being compared
      DynamicInputs plainIn;
      plainIn.pressure = 1.0f;
      const BrushTip plainTip = brushTipFor(brush, noLut, plainIn);

      BrushLink hueLink;
      hueLink.source = DynamicSource::Pressure;
      hueLink.target = DynamicTarget::Hue;
      hueLink.rangeLo = 0.0f;
      hueLink.rangeHi = 0.5f;  // a full half-turn at full pressure
      addLink(brush.links, hueLink);
      const BrushTip huedTip = brushTipFor(brush, noLut, plainIn);
      check(huedTip.linearRgb != plainTip.linearRgb,
            "targets: HUE, driven by PRESSURE, visibly rotates the deposited colour -- "
            "brushTipFor() applies applyHsvDynamics() before either colour decode");

      BrushState desat;
      desat.pigment = 0;
      BrushLink satLink;
      satLink.source = DynamicSource::Pressure;
      satLink.target = DynamicTarget::Saturation;
      satLink.rangeLo = 0.0f;
      satLink.rangeHi = 0.0f;  // fully desaturate regardless of pressure
      addLink(desat.links, satLink);
      const BrushTip desatTip = brushTipFor(desat, noLut, plainIn);
      const Hsv desatHsv = rgbToHsv({srgbEncode(desatTip.linearRgb[0]),
                                     srgbEncode(desatTip.linearRgb[1]),
                                     srgbEncode(desatTip.linearRgb[2])});
      check(desatHsv.s < 0.02f,
            "targets: SATURATION pinned to 0 deposits a grey, whatever the swatch's own hue");
    }
  }

  // ======================================================================
  // 7. Determinism: the same stroke replayed is bit-identical; two
  //    different strokes are not
  // ======================================================================
  {
    // A link set that exercises all four stroke-local sources at once, over
    // five different targets, so this is not a single-source proof.
    BrushLinkSet links;
    addLink(links, BrushLink{DynamicSource::Random, DynamicTarget::Scatter, {}, 0.0f, 0.5f, false,
                             true});
    addLink(links, BrushLink{DynamicSource::Random, DynamicTarget::Flow, {}, 0.3f, 1.0f, false,
                             true});
    addLink(links, BrushLink{DynamicSource::Noise, DynamicTarget::Hardness, {}, 0.3f, 1.0f, false,
                             true});
    addLink(links, BrushLink{DynamicSource::Velocity, DynamicTarget::Size, {}, 0.6f, 1.0f, false,
                             true});
    addLink(links, BrushLink{DynamicSource::Fade, DynamicTarget::Roundness, {}, 0.5f, 1.0f, false,
                             true});

    auto runStroke = [&](float originX, float originY, const BrushLinkSet* useLinks) {
      OpenDocument od = makeDoc(1024, 1024);
      StrokeSession s;
      std::string e;
      s.begin(od, 1, tip(24.0f, 0.35f, 0.4f), Tool::Brush, &e, useLinks);
      paintPath(s, originX, originY);
      s.end();
      return snapshotBytes(*od.document.layers[1].pigmentTiles);
    };

    const TileBytes runA = runStroke(80.0f, 400.0f, &links);
    const TileBytes runAReplayed = runStroke(80.0f, 400.0f, &links);
    check(!runA.empty() && runA == runAReplayed,
          "determinism: the IDENTICAL stroke -- same start, same path, same link set -- run "
          "twice through two independent StrokeSessions deposits BIT-IDENTICAL tiles, at zero "
          "tolerance, through Noise and Random both -- this is what protects undo and the "
          "golden harness");

    const TileBytes runB = runStroke(500.0f, 700.0f, &links);
    check(runB != runA,
          "determinism: a DIFFERENT stroke -- different starting position, so a different "
          "seed -- does NOT deposit the identical tiles. 'Deterministic' would otherwise be "
          "satisfied by a constant, which the check above alone cannot rule out");

    const TileBytes runNoLinks = runStroke(80.0f, 400.0f, nullptr);
    check(runNoLinks != runA,
          "determinism: the same path WITHOUT the stroke-local link set deposits different "
          "tiles than WITH it -- the four sources have a real, observable effect and are not "
          "silently ignored even when they resolve deterministically");
  }

  // ======================================================================
  // 8. `Dry Bristle` (brush/Library.cpp) -- what its two RANDOM links do now
  //    that RANDOM is no longer stuck at 0.0
  // ======================================================================
  {
    const BrushLibrary lib = defaultBrushLibrary();
    const BrushPreset* dry = nullptr;
    for (const BrushPreset& p : lib.presets)
      if (p.name == "Dry Bristle") dry = &p;
    check(dry != nullptr, "dry bristle: the preset still ships");
    if (dry != nullptr) {
      check(findLink(dry->links, DynamicSource::Random, DynamicTarget::Scatter) != kNoLink &&
                findLink(dry->links, DynamicSource::Random, DynamicTarget::Flow) != kNoLink,
            "dry bristle: both RANDOM links are still exactly what the preset shipped with -- "
            "fixing RANDOM did not require touching this file, because RANDOM -> SCATTER was "
            "a dead source feeding a dead target and RANDOM -> FLOW was a dead source feeding "
            "an already-live one");

      BrushState brush;
      applyPresetToBrush(*dry, brush);
      OpenDocument od = makeDoc(1024, 1024);
      StrokeSession s;
      std::string e;
      check(s.begin(od, 1, tip(brush.radius, brush.hardness, brush.load), Tool::Brush, &e,
                    &brush.links),
            "dry bristle: a stroke begins with the preset's own link set as its stroke-local "
            "set");
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
      // pixels.** This is the measurement, and the two rejected ones are
      // worth recording because each looked correct and neither was.
      //
      // Reading accumulated mass along the CENTRELINE proves nothing: pigment
      // accumulates as `m' = min(m + dm, kMaxMass)`, several dabs overlap
      // every centreline texel at this preset's ~10 px spacing, and a sweep
      // down from y=500 reads exactly 1.000000 for fifteen straight rows. The
      // cap, not the dynamics, decides the value.
      //
      // Comparing several columns of the SAME stroke out at the rim, where
      // the sum stays under the cap, survives that objection and still fails
      // -- it just fails silently. It rests on the columns being an exact
      // multiple of the dab spacing apart, so that a constant RANDOM would
      // repeat the identical local pattern at each one. They are not: the
      // spacing is 0.55 * 18 px ~= 9.9 px and the columns are 60 px apart,
      // which is 6.06 dabs, so every column sits at a different phase in the
      // dab lattice and reads a different mass **whatever RANDOM does**.
      // Pinning RANDOM to the audit's constant 0.35 leaves that version of
      // this assertion green. It measures lattice phase and calls it entropy.
      //
      // Shifting the stroke by a whole number of pixels is what this settles
      // on, and it is worth being exact about what that does and does not
      // prove. Translating by an integer leaves the geometry -- and so the
      // lattice phase, and so everything the previous attempt was
      // accidentally measuring -- identical, while changing the stroke's
      // seed, which is latched from its first dab position. So a difference
      // here can only come from the seed.
      //
      // **It does NOT isolate RANDOM -> FLOW, and must not claim to.** Three
      // separate things read that seed: `local.random` (the RANDOM source,
      // which reaches FLOW through a link), `local.noise` (the NOISE source),
      // and `applyPerDabScatter()`, which draws SCATTER's angle straight off
      // a salted copy of the seed and deliberately bypasses the link system
      // altogether. Pinning `sourceValue()`'s RANDOM arm to a constant still
      // leaves ~1570 of these texels differing, because the scatter angle
      // alone keeps moving dabs about. The claim that RANDOM specifically is
      // not a constant belongs to the `sourceValue()` dispatch assertion in
      // section 5, which tests it directly; what this one proves is the
      // end-to-end property that the seed reaches the paint at all.
      constexpr int32_t kShiftPx = 400;  // > 2 * radius, so the two strokes cannot overlap
      OpenDocument odShifted = makeDoc(1024, 1024);
      StrokeSession shifted;
      std::string eShifted;
      check(shifted.begin(odShifted, 1, tip(brush.radius, brush.hardness, brush.load), Tool::Brush,
                          &eShifted, &brush.links),
            "dry bristle: the shifted stroke begins with the same link set");
      for (int i = 0; i <= 60; ++i)
        shifted.addPoint(80.0f + static_cast<float>(kShiftPx) + 5.0f * static_cast<float>(i),
                         500.0f);
      shifted.end();

      const auto massAt = [](const OpenDocument& doc, int32_t x, int32_t y) -> float {
        const PixelCoord at{x, y};
        const PigmentTile* t = doc.document.layers[1].pigmentTiles->find(tileCoordAt(at));
        return t != nullptr ? t->readTexel(tileLocalOffset(at)).mass : 0.0f;
      };
      // 1e-4 is far below the smallest real disagreement and far above the
      // half-float storage step, so it separates "the draws differed" from
      // "the same value quantised twice" without sitting near either.
      int differing = 0;
      int compared = 0;
      for (int32_t dy = 0; dy <= static_cast<int32_t>(brush.radius) + 4; ++dy) {
        for (int32_t x = 100; x <= 360; ++x) {
          ++compared;
          if (std::fabs(massAt(od, x, 500 + dy) - massAt(odShifted, x + kShiftPx, 500 + dy)) > 1e-4f)
            ++differing;
        }
      }
      std::printf("  dry bristle: %d of %d texels differ between the stroke and its shift\n",
                  differing, compared);
      check(differing > 0,
            "dry bristle: the shipped preset's stroke is SEEDED -- the same path drawn at a "
            "different position deposits different paint, so the stroke seed reaches the paint "
            "end to end. Zero differing texels would mean every stroke-local source resolved to "
            "the same thing regardless of where the stroke started");
    }
  }

  // ======================================================================
  // 8. brushTipFor() resolves the HARDWARE sources only
  // ======================================================================
  //
  // **This section exists because a P0 lived here with a fully green suite.**
  // `brushTipFor()` used to resolve the WHOLE link set, and `StrokeSession`
  // then folded the stroke-local half on again per dab -- so every
  // VELOCITY/FADE/NOISE/RANDOM link was applied twice. `DynamicInputs`
  // defaults those four to 0, and a link at source 0 contributes exactly its
  // `rangeLo`, so the spurious second factor was the link's own floor.
  //
  // Nothing in 4442 assertions noticed. The reason is worth stating: every
  // existing assertion about `brushTipFor()` used a link set whose stroke-local
  // floors happened to be irrelevant, and every assertion about deposition
  // asked whether paint landed, not how MUCH relative to what was asked for.
  // A brush at 30% of its size still paints.
  //
  // The two cases below are the ones that bite, and the second is the one that
  // shipped: an imported Photoshop brush whose minimum size is 0.00 painted
  // NOTHING AT ALL -- no refusal, no message, a perfectly healthy link set.
  {
    std::printf("  -- 8. brushTipFor() resolves hardware sources only --\n");
    const MixboxLut noLut;
    DynamicInputs in;
    in.pressure = 1.0f;

    BrushState floored;
    floored.radius = 20.0f;
    floored.links = BrushLinkSet{};
    addLink(floored.links, BrushLink{DynamicSource::Pressure, DynamicTarget::Size, {}, 0.0f, 1.0f,
                                     false, true});
    addLink(floored.links, BrushLink{DynamicSource::Random, DynamicTarget::Size, {}, 0.5f, 1.0f,
                                     false, true});
    const BrushTip tipFloored = brushTipFor(floored, noLut, in);
    std::printf("  [measured] radius with a RANDOM->Size floor of 0.50: %.4f (asked for %.4f)\n",
                static_cast<double>(tipFloored.radius), static_cast<double>(floored.radius));
    check(std::fabs(tipFloored.radius - floored.radius) < 1e-4f,
          "hardware-only: a RANDOM->Size link does NOT shrink the tip brushTipFor() returns -- "
          "the stroke-local half is StrokeSession's to apply, per dab, and applying it here too "
          "multiplies the link's floor in a second time");

    // The floor of 0.00 case, kept separate because it is not a degree worse
    // than the one above -- it is a different outcome. Any floor scales the
    // brush down; a floor of zero switches it off, and a zero radius deposits
    // nothing (brush/Deposit.hpp: "a radius of 0 or less deposits nothing at
    // all"). Kyle's Runny Inkers ships exactly this brush.
    BrushState zeroFloor;
    zeroFloor.radius = 20.0f;
    zeroFloor.links = BrushLinkSet{};
    addLink(zeroFloor.links, BrushLink{DynamicSource::Pressure, DynamicTarget::Size, {}, 0.0f,
                                       1.0f, false, true});
    addLink(zeroFloor.links, BrushLink{DynamicSource::Random, DynamicTarget::Size, {}, 0.0f, 1.0f,
                                       false, true});
    const BrushTip tipZero = brushTipFor(zeroFloor, noLut, in);
    std::printf("  [measured] radius with a RANDOM->Size floor of 0.00: %.4f\n",
                static_cast<double>(tipZero.radius));
    check(tipZero.radius > 1e-4f,
          "hardware-only: a RANDOM->Size link whose range starts at 0.00 does not reduce the tip "
          "to a ZERO radius, which deposits nothing -- the shipped-brush case where a healthy "
          "link set painted an invisible stroke and no assertion anywhere saw it");

    // And the equality the split rests on, checked rather than assumed:
    // resolving the two halves separately and composing must equal resolving
    // the whole set in one pass. app/StrokeSession.cpp's own comment claims
    // this from TargetCombine being commutative and associative; this is that
    // claim as an assertion, on a link set that has both halves live.
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
          "hardware-only: the two partial resolutions compose back to the whole-set answer, so "
          "splitting them costs nothing -- the property that makes applying each half EXACTLY "
          "once both necessary and sufficient");
  }

  std::printf("[selftest] dynamics sources %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
