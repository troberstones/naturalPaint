#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "app/AppState.hpp"
#include "app/StrokePreferences.hpp"
#include "app/UserBrushLibrary.hpp"
#include "brush/Library.hpp"
#include "brush/Stabiliser.hpp"
#include "brush/StrokePath.hpp"

namespace np {
namespace {

float distance(Vec2 a, Vec2 b) noexcept { return std::hypot(a.x - b.x, a.y - b.y); }

// A tiny, deterministic pseudo-random generator -- splitmix64, the same one
// `brush/Dynamics.hpp`'s NOISE/RANDOM sources use -- so the "known seed"
// assertion 2 asks for is reproducible without a `<random>` dependency.
uint64_t splitmix(uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

float unitFloat(uint64_t& state) noexcept {
  return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1u << 24) - 0.5f;
}

}  // namespace

// Wave 2 stroke stabiliser (brush/Stabiliser.hpp): pulled string and
// weighted average (1-euro filter, Casiez et al. CHI 2012), the global/
// per-brush resolution rule, catch-up at end and while paused, and
// persistence for both `stroke-preferences.txt` (global) and the two new
// `user-presets.txt` keys (per brush). Pure module, headless -- `--selftest`
// drives `Stabiliser`/`StrokePath` directly with no document, no GPU.
bool runStabiliserTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] stabiliser\n");

  // ==========================================================================
  // Fix 1: the global setting loads before pen-down, without any UI call --
  // it used to load only when the Stabiliser popover had been drawn at
  // least once (`ui/StabiliserPanel.cpp`'s own `ensureLoaded()`, now
  // `app/StrokePreferences.hpp`'s `ensureStrokePreferencesLoaded()`).
  // ==========================================================================
  {
    const std::string path = "/private/tmp/np-scatter/wave2/fix1-stroke-preferences.txt";
    StrokePreferencesStore writer;
    StabiliserParams toWrite;
    toWrite.mode = StabiliserMode::WeightedAverage;
    std::string writeErr;
    PigmentBuildup writeBuildup;
    check(writer.saveToFile(path, toWrite, writeBuildup, &writeErr),
          "setup: the fixture prefs file writes");

    const char* prevEnv = std::getenv("NP_STROKE_PREFERENCES");
    const std::string prevEnvCopy = prevEnv != nullptr ? prevEnv : std::string();
    setenv("NP_STROKE_PREFERENCES", path.c_str(), 1);

    AppState st;  // fresh: strokePreferencesLoaded starts false, stabiliserPrefs at its default
    check(st.stabiliserPrefs.mode == StabiliserMode::Off,
          "setup: a fresh AppState's stabiliserPrefs starts at the compiled-in default (Off)");
    // No UI call anywhere on this path -- the same app/ loader the pen-down
    // canvas block now calls right before `resolveStabiliser()`.
    ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                  st.stabiliserPrefs, st.pigmentBuildup);
    const StabiliserParams eff = resolveStabiliser(st.stabiliserPrefs, st.brush.native.stabiliser);
    check(eff.mode == StabiliserMode::WeightedAverage,
          "fix 1: a fresh AppState whose prefs file says Weighted average resolves to Weighted "
          "average at pen-down, without any UI call");

    if (prevEnv != nullptr) setenv("NP_STROKE_PREFERENCES", prevEnvCopy.c_str(), 1);
    else unsetenv("NP_STROKE_PREFERENCES");
  }

  // ==========================================================================
  // 1. Pulled string.
  // ==========================================================================
  {
    StabiliserParams p;
    p.mode = StabiliserMode::PulledString;
    p.stringPx = 16.0f;

    // Jitter within the window never moves the nib.
    Stabiliser stab;
    stab.begin(p, 1.0f);
    uint64_t rng = 12345;
    const Vec2 centre{100.0f, 100.0f};
    StrokeSample out;
    StrokeSample first;
    first.pos = centre;
    stab.addSample(first, out);
    const Vec2 nib0 = stab.nibPos();
    bool nibMoved = false;
    for (int i = 0; i < 200; ++i) {
      StrokeSample s;
      s.pos = Vec2{centre.x + unitFloat(rng) * 6.0f, centre.y + unitFloat(rng) * 6.0f};  // +-3px
      stab.addSample(s, out);
      if (distance(stab.nibPos(), nib0) > 1e-4f) nibMoved = true;
    }
    check(!nibMoved, "pulled string: +-3px jitter inside a 16px string never moves the nib");

    // A straight drag leaves the nib exactly stringPx behind the pointer.
    Stabiliser drag;
    drag.begin(p, 1.0f);
    StrokeSample d0;
    d0.pos = Vec2{0.0f, 0.0f};
    drag.addSample(d0, out);
    for (int i = 1; i <= 500; ++i) {
      StrokeSample s;
      s.pos = Vec2{static_cast<float>(i) * 2.0f, 0.0f};
      drag.addSample(s, out);
    }
    const float trail = distance(drag.nibPos(), drag.rawPos());
    check(std::fabs(trail - 16.0f) < 1e-2f,
          "pulled string: a straight drag leaves the nib exactly stringPx behind the pointer");
  }

  // ==========================================================================
  // 2. Weighted average (1-euro filter).
  // ==========================================================================
  {
    // Jitter RMS drop at strength 50: a straight line with known-seed noise
    // added perpendicular to it, RMS of the OUTPUT against the true line vs
    // RMS of the RAW input against it.
    StabiliserParams p;
    p.mode = StabiliserMode::WeightedAverage;
    p.strength = 50.0f;
    p.responsiveness = 50.0f;
    Stabiliser stab;
    stab.begin(p, 1.0f);
    uint64_t rng = 987654321ull;
    double rawSq = 0.0, outSq = 0.0;
    int n = 0;
    uint64_t ts = 0;
    for (int i = 0; i < 400; ++i) {
      const float trueY = 100.0f;
      const float noise = unitFloat(rng) * 4.0f;  // +-2px off the true line
      StrokeSample raw;
      raw.pos = Vec2{static_cast<float>(i) * 3.0f, trueY + noise};
      raw.timestamp = ts;
      ts += 8'000'000ull;  // 125 Hz
      StrokeSample out;
      stab.addSample(raw, out);
      if (i > 10) {  // past the filter's own settling
        rawSq += static_cast<double>(noise) * noise;
        outSq += static_cast<double>(out.pos.y - trueY) * (out.pos.y - trueY);
        ++n;
      }
    }
    const double rawRms = std::sqrt(rawSq / n);
    const double outRms = std::sqrt(outSq / n);
    std::printf("  [measured] jitter RMS: raw %.3f px, filtered %.3f px (strength 50)\n", rawRms,
               outRms);
    check(outRms < rawRms * 0.5,
          "weighted average: strength 50 drops jitter RMS by at least half");

    // Lag at high speed: responsiveness 100 vs 0, same strength -- the
    // 1-euro filter's own defining property.
    const auto steadyLag = [&](float responsiveness) {
      StabiliserParams lp;
      lp.mode = StabiliserMode::WeightedAverage;
      lp.strength = 50.0f;
      lp.responsiveness = responsiveness;
      Stabiliser s;
      s.begin(lp, 1.0f);
      uint64_t t = 0;
      StrokeSample out;
      float lastLag = 0.0f;
      for (int i = 0; i < 300; ++i) {
        StrokeSample raw;
        raw.pos = Vec2{static_cast<float>(i) * 20.0f, 0.0f};  // a fast, steady stroke
        raw.timestamp = t;
        t += 8'000'000ull;
        s.addSample(raw, out);
        lastLag = raw.pos.x - out.pos.x;
      }
      return lastLag;
    };
    const float lagLow = steadyLag(0.0f);
    const float lagHigh = steadyLag(100.0f);
    std::printf("  [measured] steady-state lag at fast, constant speed: responsiveness 0 -> "
               "%.2f px, responsiveness 100 -> %.2f px\n",
               lagLow, lagHigh);
    check(lagHigh < lagLow,
          "weighted average: lag at high speed is smaller with responsiveness 100 than 0");
  }

  // ==========================================================================
  // 3. Catch up at end / off.
  // ==========================================================================
  {
    const auto lastDabDistanceToLift = [&](bool catchUpAtEnd) {
      StabiliserParams p;
      p.mode = StabiliserMode::WeightedAverage;
      p.strength = 90.0f;  // heavy smoothing, so the nib visibly lags
      p.responsiveness = 0.0f;
      p.catchUpAtEnd = catchUpAtEnd;
      Stabiliser stab;
      stab.begin(p, 1.0f);
      StrokePath path;
      path.reset();
      std::vector<StrokeDab> dabs;
      uint64_t t = 0;
      const float spacingPx = 0.5f;  // fine, so the tail is not spacing-limited
      Vec2 lift{0.0f, 0.0f};
      for (int i = 0; i < 200; ++i) {
        StrokeSample raw;
        raw.pos = Vec2{static_cast<float>(i) * 4.0f, 0.0f};
        raw.timestamp = t;
        t += 8'000'000ull;
        lift = raw.pos;
        StrokeSample out;
        stab.addSample(raw, out);
        path.addPoint(out, spacingPx, dabs);
      }
      if (p.mode != StabiliserMode::Off && p.catchUpAtEnd) {
        std::vector<StrokeSample> steps;
        if (stab.forceCatchUp(steps))
          for (const StrokeSample& s : steps) path.addPoint(s, spacingPx, dabs);
      }
      path.flush(spacingPx, dabs);
      return dabs.empty() ? -1.0f : distance(dabs.back().pos, lift);
    };
    const float withCatchUp = lastDabDistanceToLift(true);
    const float withoutCatchUp = lastDabDistanceToLift(false);
    std::printf("  [measured] last dab to lift point: catch-up on %.3f px, off %.3f px\n",
               withCatchUp, withoutCatchUp);
    check(withCatchUp >= 0.0f && withCatchUp < 0.5f,
          "catch up at end: the last dab lands within 0.5px of the lift point");
    check(withoutCatchUp > 1.0f,
          "catch up at end, off: the last dab does NOT land near the lift point");
  }

  // ==========================================================================
  // 4. Catch up while paused.
  // ==========================================================================
  {
    StabiliserParams p;
    p.mode = StabiliserMode::WeightedAverage;
    p.strength = 90.0f;
    p.responsiveness = 0.0f;
    p.catchUpWhilePaused = true;
    Stabiliser stab;
    stab.begin(p, 1.0f);
    uint64_t t = 0;
    StrokeSample out;
    for (int i = 0; i < 30; ++i) {  // a fast run-up, so the nib is well behind
      StrokeSample raw;
      raw.pos = Vec2{static_cast<float>(i) * 30.0f, 0.0f};
      raw.timestamp = t;
      t += 8'000'000ull;
      stab.addSample(raw, out);
    }
    const float lagAtPause = distance(stab.nibPos(), stab.rawPos());
    float prevLag = lagAtPause;
    bool monotonic = true;
    for (int i = 0; i < 60; ++i) {  // pen down, still: tick() only, no new samples
      t += 16'000'000ull;          // ~60 Hz frame cadence
      StrokeSample tickOut;
      stab.tick(t, tickOut);
      const float lag = distance(stab.nibPos(), stab.rawPos());
      if (lag > prevLag + 1e-4f) monotonic = false;
      prevLag = lag;
    }
    std::printf("  [measured] lag at pause %.2f px, after 60 ticks %.4f px\n", lagAtPause,
               prevLag);
    check(lagAtPause > 1.0f, "setup: the run-up leaves a real lag to converge from");
    check(monotonic && prevLag < lagAtPause * 0.05f,
          "catch up while paused: repeated tick()s with no new samples converge the nib to the "
          "pen");
  }

  // ==========================================================================
  // Fix 5: a leap after a pause, with "catch up while paused" OFF -- nothing
  // converges the nib during the pause, so the first real sample after a
  // long one would otherwise see a huge dt and jump most of the way to the
  // pen in a single step. Capped to a plausible single-frame dt instead.
  // ==========================================================================
  {
    StabiliserParams p;
    p.mode = StabiliserMode::WeightedAverage;
    p.strength = 100.0f;
    p.responsiveness = 0.0f;
    p.catchUpWhilePaused = false;
    Stabiliser stab;
    stab.begin(p, 1.0f);
    uint64_t t = 1'000'000'000ull;
    StrokeSample out;
    float x = 0.0f;
    for (int i = 0; i < 60; ++i, t += 4'166'667ull) {  // run-up at 240 Hz
      x = static_cast<float>(i) * 4.0f;
      StrokeSample raw;
      raw.pos = Vec2{x, 0.0f};
      raw.timestamp = t;
      stab.addSample(raw, out);
    }
    const float lagBeforePause = x - out.pos.x;
    const float nibBeforePause = out.pos.x;
    t += 2'000'000'000ull;  // 2 s pause; catchUpWhilePaused is off, so no tick() would run anyway
    StrokeSample resume;
    resume.pos = Vec2{x + 1.0f, 0.0f};
    resume.timestamp = t;
    stab.addSample(resume, out);
    const float jump = out.pos.x - nibBeforePause;
    std::printf("  [measured] lag before pause %.2f px, jump on the first sample after a 2s "
               "pause %.2f px\n",
               lagBeforePause, jump);
    check(lagBeforePause > 3.0f, "setup: the run-up leaves a real lag to converge from");
    check(jump < lagBeforePause * 0.2f,
          "fix 5: capped dt keeps the first sample after a long pause from jumping most of the "
          "way to the pen in one step");
  }

  // ==========================================================================
  // Fix 6: a tick's timestamp (a frame-poll clock) must never leak into a
  // real sample's own dt. A tick fired AFTER a real sample but with a LATER
  // hardware-adjacent timestamp than the NEXT real sample (plausible: the
  // tick uses `SDL_GetTicksNS()` at frame-poll time, the next real sample
  // carries the pen's own, earlier, hardware timestamp) used to make that
  // next real sample's dt negative -> the dt<=0 guard -> a near-zero dt ->
  // an absurd computed speed -> the cutoff opening all the way -> the nib
  // snapping onto the raw sample instead of smoothing it.
  // ==========================================================================
  {
    StabiliserParams p;
    p.mode = StabiliserMode::WeightedAverage;
    p.strength = 50.0f;
    p.responsiveness = 100.0f;
    p.catchUpWhilePaused = true;
    Stabiliser stab;
    stab.begin(p, 1.0f);
    const uint64_t t0 = 1'000'000'000ull;
    StrokeSample out;
    StrokeSample s1;
    s1.pos = Vec2{0.0f, 0.0f};
    s1.timestamp = t0;
    stab.addSample(s1, out);  // bootstrap

    StrokeSample s2;
    s2.pos = Vec2{1.0f, 0.0f};
    s2.timestamp = t0 + 4'000'000ull;  // 4 ms later, a normal pen cadence
    stab.addSample(s2, out);
    const float nibAfterS2 = out.pos.x;

    StrokeSample tickOut;
    // 10 ms after t0 -- LATER than s3's own hardware timestamp below, the
    // scenario a frame-poll clock racing ahead of the pen's own produces.
    stab.tick(t0 + 10'000'000ull, tickOut);

    StrokeSample s3;
    s3.pos = Vec2{2.0f, 0.0f};
    s3.timestamp = t0 + 8'000'000ull;  // 4 ms after s2 -- same normal cadence
    stab.addSample(s3, out);
    std::printf("  [measured] nib after s2 %.4f, after the out-of-order tick + s3 %.4f (raw was "
               "2.0)\n",
               nibAfterS2, out.pos.x);
    // Contrary to the original hypothesis, the bug's ~0.1 ms guard-dt does
    // NOT snap the nib onto the raw sample -- the same corrupted near-zero
    // dt also feeds the speed low-pass (dAlpha), so the responsiveness
    // cutoff computes an absurdly LOW speed reading and closes down instead
    // of opening: alpha ~= dt/(dt+tau), so a tiny dt makes for a tiny alpha
    // and s3 is *under*-applied rather than snapped. Measured: buggy ~0.27,
    // fixed ~0.48; 0.35 sits strictly between the two.
    check(out.pos.x > 0.35f,
          "fix 6: a real sample's dt is measured from the last real sample, never from an "
          "intervening tick -- s3 responds normally, not suppressed by a corrupted near-zero dt");
  }

  // ==========================================================================
  // Fix 11b: "scale with zoom" reaches weighted average's speed/beta term
  // too, not just pulled string's window -- a higher zoom (more screen px
  // per canvas px) should read the SAME canvas-space motion as FASTER, and
  // open the cutoff more.
  // ==========================================================================
  {
    const auto steadyLagAtZoom = [&](float zoom) {
      StabiliserParams lp;
      lp.mode = StabiliserMode::WeightedAverage;
      lp.strength = 50.0f;
      lp.responsiveness = 100.0f;
      lp.scaleWithZoom = true;
      Stabiliser s;
      s.begin(lp, zoom);
      uint64_t t = 0;
      StrokeSample out;
      float lastLag = 0.0f;
      for (int i = 0; i < 300; ++i) {
        StrokeSample raw;
        raw.pos = Vec2{static_cast<float>(i) * 20.0f, 0.0f};  // identical canvas-space motion
        raw.timestamp = t;
        t += 8'000'000ull;
        s.addSample(raw, out);
        lastLag = raw.pos.x - out.pos.x;
      }
      return lastLag;
    };
    const float lagZoom1 = steadyLagAtZoom(1.0f);
    const float lagZoom4 = steadyLagAtZoom(4.0f);
    std::printf("  [measured] scale with zoom, weighted average: steady lag at 1x zoom %.2f px, "
               "4x zoom %.2f px\n",
               lagZoom1, lagZoom4);
    check(lagZoom4 < lagZoom1 * 0.9f,
          "fix 11b: a higher zoom reads the identical canvas motion as faster (screen px/s) and "
          "shrinks the lag, so scale with zoom now reaches weighted average too");
  }

  // ==========================================================================
  // 5. Sample-rate independence (pulled string): the same physical path fed
  //    at two densities gives the same dab positions, within spacing
  //    tolerance.
  // ==========================================================================
  {
    StabiliserParams p;
    p.mode = StabiliserMode::PulledString;
    p.stringPx = 12.0f;
    const float spacingPx = 4.0f;
    const auto dabsFor = [&](int samples) {
      Stabiliser stab;
      stab.begin(p, 1.0f);
      StrokePath path;
      path.reset();
      std::vector<StrokeDab> dabs;
      for (int i = 0; i <= samples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(samples);
        StrokeSample raw;
        raw.pos = Vec2{u * 300.0f, 0.0f};  // the same 300px straight path
        StrokeSample out;
        stab.addSample(raw, out);
        path.addPoint(out, spacingPx, dabs);
      }
      path.flush(spacingPx, dabs);
      return dabs;
    };
    const std::vector<StrokeDab> sparse = dabsFor(20);   // "60 Hz" over the path
    const std::vector<StrokeDab> dense = dabsFor(80);    // "240 Hz"
    check(!sparse.empty() && !dense.empty(), "setup: both densities emit dabs");
    check(std::abs(static_cast<int>(sparse.size()) - static_cast<int>(dense.size())) <= 1,
          "pulled string: the two densities emit the same number of dabs (+-1)");
    bool positionsMatch = sparse.size() == dense.size();
    if (positionsMatch) {
      for (size_t i = 0; i < sparse.size(); ++i)
        if (distance(sparse[i].pos, dense[i].pos) > spacingPx * 0.5f) positionsMatch = false;
    }
    check(positionsMatch,
          "pulled string: dab positions match within spacing tolerance at 60Hz vs 240Hz");
  }

  // ==========================================================================
  // 6. resolveStabiliser: exact expected effective settings.
  // ==========================================================================
  {
    StabiliserParams global;
    global.mode = StabiliserMode::WeightedAverage;
    global.stringPx = 20.0f;
    global.strength = 40.0f;
    global.responsiveness = 30.0f;
    global.catchUpAtEnd = true;
    global.catchUpWhilePaused = false;
    global.stabilisePressure = true;
    global.scaleWithZoom = true;
    global.showString = false;

    BrushStabiliserSetting followHalf;
    followHalf.mode = StabiliserBrushMode::FollowGlobal;
    followHalf.amountPct = 50.0f;
    const StabiliserParams effFollow = resolveStabiliser(global, followHalf);
    // Fix 4: amount scales stringPx/strength only. Responsiveness stays at
    // the GLOBAL's own value (30, not 15) -- scaling it too made a higher
    // amount respond LESS to speed, backwards from what the slider promises.
    check(effFollow.mode == StabiliserMode::WeightedAverage &&
              effFollow.stringPx == 10.0f && effFollow.strength == 20.0f &&
              effFollow.responsiveness == 30.0f && effFollow.catchUpAtEnd == true &&
              effFollow.catchUpWhilePaused == false && effFollow.stabilisePressure == true &&
              effFollow.scaleWithZoom == true && effFollow.showString == false,
          "resolveStabiliser: follow global x50% halves stringPx/strength, leaves "
          "responsiveness at the global's own value, keeps mode and every option");

    BrushStabiliserSetting off;
    off.mode = StabiliserBrushMode::Off;
    const StabiliserParams effOff = resolveStabiliser(global, off);
    check(effOff.mode == StabiliserMode::Off, "resolveStabiliser: off -> mode Off, exactly");

    BrushStabiliserSetting own;
    own.mode = StabiliserBrushMode::Own;
    own.own.mode = StabiliserMode::PulledString;
    own.own.stringPx = 9.0f;
    own.own.strength = 77.0f;
    own.own.responsiveness = 11.0f;
    const StabiliserParams effOwn = resolveStabiliser(global, own);
    check(effOwn.mode == StabiliserMode::PulledString && effOwn.stringPx == 9.0f &&
              effOwn.strength == 77.0f && effOwn.responsiveness == 11.0f &&
              effOwn.catchUpAtEnd == global.catchUpAtEnd &&
              effOwn.catchUpWhilePaused == global.catchUpWhilePaused &&
              effOwn.stabilisePressure == global.stabilisePressure &&
              effOwn.scaleWithZoom == global.scaleWithZoom &&
              effOwn.showString == global.showString,
          "resolveStabiliser: own takes its own mode/stringPx/strength/responsiveness, the "
          "global's options");

    // The default -- Follow global at 100% -- is the global's setting exactly.
    check(resolveStabiliser(global, BrushStabiliserSetting{}).mode == global.mode &&
              resolveStabiliser(global, BrushStabiliserSetting{}).stringPx == global.stringPx,
          "resolveStabiliser: follow global x100% (the default) is the global setting exactly");
  }

  // ==========================================================================
  // 7. Persistence.
  // ==========================================================================
  {
    // stroke-preferences.txt: round trip, and an unknown line survives.
    StabiliserParams g;
    g.mode = StabiliserMode::PulledString;
    g.stringPx = 22.5f;
    g.strength = 61.0f;
    g.responsiveness = 8.0f;
    g.catchUpAtEnd = false;
    g.catchUpWhilePaused = true;
    g.stabilisePressure = true;
    g.scaleWithZoom = true;
    g.showString = false;

    StrokePreferencesStore store;
    // brush/Deposit.hpp §1a's mode shares this record, so it shares its
    // round-trip: non-default, so a serializer that dropped it would come back
    // as the default rather than as the value written.
    PigmentBuildup b;
    b.mode = PigmentBuildupMode::Wash;
    const std::string written = store.serialize(g, b);
    StabiliserParams reloaded;
    PigmentBuildup reloadedBuildup;
    StrokePreferencesStore reader;
    reader.parse(written, reloaded, reloadedBuildup);
    check(reloadedBuildup.mode == PigmentBuildupMode::Wash,
          "stroke-preferences.txt: the pigment buildup mode round-trips");

    // A file written by the build that had two buildup switches instead.
    StabiliserParams oldGlobal;
    PigmentBuildup oldBuildup;
    StrokePreferencesStore oldReader;
    oldReader.parse("naturalPaint-stroke-preferences 1\nbuildupSaturating 1\n"
                    "buildupStrokeCeiling 1\n",
                    oldGlobal, oldBuildup);
    check(oldReader.unknownLines().empty() && oldBuildup.mode == PigmentBuildupMode::BuildUp,
          "stroke-preferences.txt: the two retired buildup switches are dropped, not carried "
          "forward as unknown lines, and select nothing");
    check(reloaded.mode == g.mode && reloaded.stringPx == g.stringPx &&
              reloaded.strength == g.strength && reloaded.responsiveness == g.responsiveness &&
              reloaded.catchUpAtEnd == g.catchUpAtEnd &&
              reloaded.catchUpWhilePaused == g.catchUpWhilePaused &&
              reloaded.stabilisePressure == g.stabilisePressure &&
              reloaded.scaleWithZoom == g.scaleWithZoom && reloaded.showString == g.showString,
          "stroke-preferences.txt: every field round-trips exactly");

    const std::string withUnknown =
        "naturalPaint-stroke-preferences 1\nmode 1\nfutureKey 42\n";
    StabiliserParams parsedUnknown;
    PigmentBuildup unknownBuildup;
    StrokePreferencesStore unknownReader;
    unknownReader.parse(withUnknown, parsedUnknown, unknownBuildup);
    const std::string resaved = unknownReader.serialize(parsedUnknown, unknownBuildup);
    check(resaved.find("futureKey 42") != std::string::npos,
          "stroke-preferences.txt: an unknown key survives a parse/serialize round trip");

    // Fix 8: NaN is rejected (not cast into `mode`'s enum, not carried into
    // a slider range -- undefined behaviour and a garbage value
    // respectively), and a merely out-of-range value is clamped rather than
    // rejected outright.
    StabiliserParams badGlobal;
    PigmentBuildup badBuildup;
    StrokePreferencesStore badReader;
    badReader.parse(
        "naturalPaint-stroke-preferences 1\nmode 7\nstrength nan\nstringPx -40\n"
        "responsiveness 900\nfoo bar\n",
        badGlobal, badBuildup);
    std::printf("  [measured] fix 8 fixture: mode %d strength %f stringPx %f responsiveness %f, "
               "%zu unknown line(s)\n",
               static_cast<int>(badGlobal.mode), badGlobal.strength, badGlobal.stringPx,
               badGlobal.responsiveness, badReader.unknownLines().size());
    check(badGlobal.mode == StabiliserParams{}.mode,
          "fix 8: 'mode 7' (out of the enum's range) is rejected, not cast -- mode stays default");
    check(std::isfinite(badGlobal.strength) && badGlobal.strength == StabiliserParams{}.strength,
          "fix 8: 'strength nan' is rejected outright, not clamped into a value that happens to "
          "be finite -- strength stays default");
    check(badGlobal.stringPx == 0.0f,
          "fix 8: 'stringPx -40' (out of the 0-200 slider range) is clamped to 0, not rejected");
    check(badGlobal.responsiveness == 100.0f,
          "fix 8: 'responsiveness 900' (out of the 0-100 slider range) is clamped to 100");
    check(badReader.unknownLines().size() == 3,
          "fix 8: the three rejected lines (mode, strength, foo) all survive as unknown lines "
          "for the next save");

    // user-presets.txt: the `taperin`/`taperout`/`stabiliser` lines round trip.
    BrushPreset preset;
    preset.name = "Taper And Stabiliser";
    preset.native.taperIn = BrushTaper{true, 48.0f, 15.0f, true};
    // Switched OFF but set: the length has to survive the toggle, which is
    // the whole reason `on` is a field rather than "lengthPx > 0".
    preset.native.taperOut = BrushTaper{false, 120.0f, 5.0f, false};
    preset.native.stabiliser.mode = StabiliserBrushMode::Own;
    preset.native.stabiliser.amountPct = 250.0f;
    preset.native.stabiliser.own.mode = StabiliserMode::WeightedAverage;
    preset.native.stabiliser.own.stringPx = 5.0f;
    preset.native.stabiliser.own.strength = 33.0f;
    preset.native.stabiliser.own.responsiveness = 66.0f;

    UserBrushLibraryStore ubStore;
    BrushLibrary lib;
    lib.presets.push_back(preset);
    const std::string presetsText = ubStore.serialize(lib);

    UserBrushLibraryStore ubReader;
    BrushLibrary reloadedLib;
    ubReader.parse(presetsText, reloadedLib);
    const BrushPreset* back = nullptr;
    for (const BrushPreset& p : reloadedLib.presets)
      if (p.name == "Taper And Stabiliser") back = &p;
    check(back != nullptr && brushTaperEqual(back->native.taperIn, preset.native.taperIn) &&
              brushTaperEqual(back->native.taperOut, preset.native.taperOut),
          "user-presets.txt: both `taperin`/`taperout` lines round-trip exactly, switched-off "
          "settings included");
    check(back != nullptr && back->native.stabiliser.mode == StabiliserBrushMode::Own &&
              back->native.stabiliser.amountPct == 250.0f &&
              back->native.stabiliser.own.mode == StabiliserMode::WeightedAverage &&
              back->native.stabiliser.own.stringPx == 5.0f &&
              back->native.stabiliser.own.strength == 33.0f &&
              back->native.stabiliser.own.responsiveness == 66.0f,
          "user-presets.txt: the new `stabiliser` line round-trips exactly");

    // Fix 11a: `own` is serialised whenever it differs from ITS OWN default,
    // regardless of the active mode -- a painter can tune `own` under Own,
    // then switch back to Follow global (both `mode`/`amountPct` back at
    // their defaults) without resetting it, and that tuning must survive.
    BrushPreset ownTunedButFollowing;
    ownTunedButFollowing.name = "Own Tuned But Following";
    ownTunedButFollowing.native.stabiliser.mode = StabiliserBrushMode::FollowGlobal;
    ownTunedButFollowing.native.stabiliser.amountPct = 100.0f;  // both at their defaults
    ownTunedButFollowing.native.stabiliser.own.strength = 77.0f;  // but `own` was tuned
    UserBrushLibraryStore ownStore;
    BrushLibrary ownLib;
    ownLib.presets.push_back(ownTunedButFollowing);
    const std::string ownText = ownStore.serialize(ownLib);
    check(ownText.find("stabiliser ") != std::string::npos,
          "fix 11a: a `stabiliser` line is written even though mode/amountPct are both at their "
          "defaults, because `own` itself differs from its own default");
    UserBrushLibraryStore ownReader;
    BrushLibrary ownReloaded;
    ownReader.parse(ownText, ownReloaded);
    const BrushPreset* ownBack = nullptr;
    for (const BrushPreset& p : ownReloaded.presets)
      if (p.name == "Own Tuned But Following") ownBack = &p;
    check(ownBack != nullptr && ownBack->native.stabiliser.own.strength == 77.0f,
          "fix 11a: the tuned `own.strength` survives the round trip even though the active mode "
          "never left Follow global");

    // A file with a key this build does not know, inside a preset scope,
    // still loads and preserves that line -- the "an older build ignores it"
    // half of the same claim, exercised the other direction (a NEWER key
    // this parser does not recognise).
    const std::string futureFixture =
        "naturalPaint-user-presets 1\n"
        "preset Future Brush\n"
        "scalars 20 0.5 0.366 1 0 0.9 1.3\n"
        "futureFeature 1 2 3\n";
    UserBrushLibraryStore futureReader;
    BrushLibrary futureLib;
    futureReader.parse(futureFixture, futureLib);
    const std::string futureResaved = futureReader.serialize(futureLib);
    check(futureResaved.find("futureFeature 1 2 3") != std::string::npos,
          "user-presets.txt: a key this build does not know survives a parse/serialize round "
          "trip");

    // Fix 7: a REJECTED `stabiliser`/`taper` line -- an out-of-range ordinal
    // (a future `StabiliserBrushMode`/`StabiliserMode` member) or simply
    // malformed (wrong field count) -- survives a save too, the same
    // protection an unrecognised KEY already has.
    const std::string rejectedFixture =
        "naturalPaint-user-presets 1\n"
        "preset Rejected Lines\n"
        "scalars 20 0.5 0.366 1 0 0.9 1.3\n"
        "stabiliser 3 100 0 16 40 50\n"
        "taper 60 0\n";
    UserBrushLibraryStore rejectedReader;
    BrushLibrary rejectedLib;
    rejectedReader.parse(rejectedFixture, rejectedLib);
    const std::string rejectedResaved = rejectedReader.serialize(rejectedLib);
    check(rejectedResaved.find("stabiliser 3 100 0 16 40 50") != std::string::npos,
          "fix 7: a `stabiliser` line with an out-of-range ordinal survives a save rather than "
          "being silently dropped");
    check(rejectedResaved.find("taper 60 0") != std::string::npos,
          "fix 7: a malformed `taper` line (wrong field count) survives a save too");

    // Fix 8: NaN/out-of-range `taper` values from the file are validated the
    // same way stroke-preferences.txt's own fields are.
    const auto taperMinSizeFor = [&](const char* tail) {
      const std::string fx = std::string("naturalPaint-user-presets 1\npreset T\n"
                                         "scalars 20 0.5 0.5 1 0 0.9 1.3\n") +
                             tail + "\n";
      UserBrushLibraryStore r;
      BrushLibrary lib2;
      r.parse(fx, lib2);
      // A copy, not a pointer into `lib2` -- `lib2` is local to this lambda.
      for (const BrushPreset& q : lib2.presets)
        if (q.name == "T") return std::optional<float>(q.native.taperIn.minSizePct);
      return std::optional<float>();
    };
    const std::optional<float> clampedHi = taperMinSizeFor("taper 60 150 0");
    check(clampedHi.has_value() && *clampedHi == 100.0f,
          "fix 8: 'taper 60 150 0' (taperMinSize past the 0-100 range) is clamped to 100");
    const std::optional<float> clampedLo = taperMinSizeFor("taper 60 -100 0");
    check(clampedLo.has_value() && *clampedLo == 0.0f,
          "fix 8: 'taper 60 -100 0' (taperMinSize below the 0-100 range) is clamped to 0");
    {
      const std::string nanFx =
          "naturalPaint-user-presets 1\npreset NanTaper\n"
          "scalars 20 0.5 0.5 1 0 0.9 1.3\ntaper nan 0 0\n";
      UserBrushLibraryStore r;
      BrushLibrary lib2;
      r.parse(nanFx, lib2);
      const BrushPreset* p = nullptr;
      for (const BrushPreset& q : lib2.presets)
        if (q.name == "NanTaper") p = &q;
      check(p != nullptr && p->native.taperIn.lengthPx == 0.0f && !p->native.taperIn.on,
            "fix 8: 'taper nan 0 0' is rejected outright -- the entry taper stays default (off), "
            "not NaN");
      const std::string nanResaved = r.serialize(lib2);
      check(nanResaved.find("taper nan 0 0") != std::string::npos,
            "fix 8: the rejected NaN `taper` line is preserved verbatim for the next save");
    }

    // The `.abr` mapping.
    check(stabiliserBrushModeFromAbrSmoothing(true) == StabiliserBrushMode::FollowGlobal &&
              stabiliserBrushModeFromAbrSmoothing(false) == StabiliserBrushMode::Off,
          "ABR import: toolOptions/smoothing true -> Follow global, false -> Off");
  }

  // ==========================================================================
  // Wave 2 (tablet feedback) item 2: pulled string's `catchUpMs`. While the
  // pen is down and not moving, `tick()` closes the nib-to-pointer gap
  // exponentially -- 95% closed by `catchUpMs`, reaches the pen well beyond
  // it, 0 = off (the string just sits at its full window, wave 1's
  // behaviour -- the RED case below).
  // ==========================================================================
  {
    const auto runUpThenPause = [&](float catchUpMs, int ticks) {
      StabiliserParams p;
      p.mode = StabiliserMode::PulledString;
      p.stringPx = 20.0f;
      p.catchUpMs = catchUpMs;
      Stabiliser stab;
      stab.begin(p, 1.0f);
      uint64_t t = 0;
      StrokeSample out;
      // A straight run-up long enough that the string is fully taut (dist ==
      // stringPx behind) well before the pen stops -- assertion 1's own
      // property, reused here as the setup.
      for (int i = 0; i < 40; ++i) {
        StrokeSample raw;
        raw.pos = Vec2{static_cast<float>(i) * 5.0f, 0.0f};
        raw.timestamp = t;
        t += 8'000'000ull;  // 8 ms cadence
        stab.addSample(raw, out);
      }
      const float gapAtPause = distance(stab.nibPos(), stab.rawPos());
      for (int i = 0; i < ticks; ++i) {
        t += 16'000'000ull;  // ~60 Hz frame cadence
        StrokeSample tickOut;
        stab.tick(t, tickOut);
      }
      const float gapAfter = distance(stab.nibPos(), stab.rawPos());
      return std::pair<float, float>(gapAtPause, gapAfter);
    };

    // 400 ms default, ticked exactly to 400 ms (25 ticks * 16 ms): the gap
    // should be ~5% of what it was at pause (the formula's own definition).
    const auto [gap0, gapAt400] = runUpThenPause(400.0f, 25);
    std::printf("  [measured] catchUpMs=400: gap at pause %.2f px, after 400ms %.3f px (%.1f%% "
               "of gap0)\n",
               gap0, gapAt400, 100.0f * gapAt400 / gap0);
    check(gap0 > 15.0f, "setup: the run-up leaves the string fully taut (~stringPx) to converge "
                        "from");
    check(gapAt400 < gap0 * 0.10f,
          "catchUpMs: ~95% of the gap is closed by catchUpMs (measured within 10% of gap0, "
          "vs. gap0 itself the whole time)");

    // Reaches the pen well beyond catchUpMs.
    const auto [gap0b, gapFar] = runUpThenPause(400.0f, 200);  // 3.2 s of ticks
    std::printf("  [measured] catchUpMs=400: gap after 3.2s of ticks %.4f px\n", gapFar);
    check(gapFar < 0.05f, "catchUpMs: the nib reaches the pen (gap -> ~0) well beyond catchUpMs");

    // RED: 0 = off. Without this feature (wave 1), the string just sits at
    // its full window forever -- exactly this.
    const auto [gap0c, gapOff] = runUpThenPause(0.0f, 200);
    std::printf("  [measured] catchUpMs=0 (off): gap at pause %.2f px, after 3.2s of ticks %.2f "
               "px\n",
               gap0c, gapOff);
    check(std::abs(gapOff - gap0c) < 1e-3f,
          "catchUpMs=0: RED case -- the string never shortens while paused");

    // "When the pen moves again the full string length returns, measured
    // from where the brush point now is": partial decay, then a real sample
    // far away must leave the nib exactly `stringPx` behind IT, not still
    // catching up toward the old pause point.
    {
      StabiliserParams p;
      p.mode = StabiliserMode::PulledString;
      p.stringPx = 20.0f;
      p.catchUpMs = 400.0f;
      Stabiliser stab;
      stab.begin(p, 1.0f);
      uint64_t t = 0;
      StrokeSample out;
      for (int i = 0; i < 40; ++i) {
        StrokeSample raw;
        raw.pos = Vec2{static_cast<float>(i) * 5.0f, 0.0f};
        raw.timestamp = t;
        t += 8'000'000ull;
        stab.addSample(raw, out);
      }
      for (int i = 0; i < 12; ++i) {  // ~190 ms of partial catch-up, not yet done
        t += 16'000'000ull;
        StrokeSample tickOut;
        stab.tick(t, tickOut);
      }
      StrokeSample resume;
      resume.pos = Vec2{stab.rawPos().x + 200.0f, 0.0f};  // a big move, straight line
      resume.timestamp = t + 8'000'000ull;
      stab.addSample(resume, out);
      const float freshGap = distance(out.pos, resume.pos);
      std::printf("  [measured] gap right after the resuming sample: %.3f px (stringPx 20)\n",
                 freshGap);
      check(std::abs(freshGap - 20.0f) < 0.01f,
            "catchUpMs: moving the pen again restores the full string window immediately, "
            "measured from the pointer's new position");
    }
  }

  // ==========================================================================
  // Wave 2 (tablet feedback) item 3: both catch-ups follow the raw path the
  // pointer actually took, not a straight line. An L-shaped path -- a long
  // horizontal run, then a short vertical run (shorter than stringPx, so the
  // corner lies INSIDE the string when the pen lifts) -- is the shape the
  // brief's own assertion names. `forceCatchUp()`'s steps must hug the
  // polyline within 0.5 px; the straight line the OLD forceCatchUp (wave 1)
  // would have drawn -- from wherever the nib was lagging straight to the
  // lift point -- is shown for contrast (it cuts the corner by roughly
  // stringPx/sqrt(2), the brief's own estimate).
  // ==========================================================================
  {
    const auto distPointToSegment = [](Vec2 p, Vec2 a, Vec2 b) {
      const float ux = b.x - a.x, uy = b.y - a.y;
      const float len2 = ux * ux + uy * uy;
      float t = 0.0f;
      if (len2 > 1e-9f) t = std::clamp(((p.x - a.x) * ux + (p.y - a.y) * uy) / len2, 0.0f, 1.0f);
      const float qx = a.x + ux * t, qy = a.y + uy * t;
      return distance(p, Vec2{qx, qy});
    };

    StabiliserParams p;
    p.mode = StabiliserMode::PulledString;
    p.stringPx = 30.0f;
    Stabiliser stab;
    stab.begin(p, 1.0f);

    uint64_t t = 0;
    StrokeSample out;
    std::vector<Vec2> raw;  // the ground-truth polyline, recorded independently
    for (int i = 0; i <= 100; ++i) {  // horizontal leg: (0,0) -> (100,0)
      StrokeSample s;
      s.pos = Vec2{static_cast<float>(i), 0.0f};
      s.timestamp = t;
      t += 4'000'000ull;
      stab.addSample(s, out);
      raw.push_back(s.pos);
    }
    const Vec2 corner{100.0f, 0.0f};
    for (int i = 1; i <= 20; ++i) {  // vertical leg: (100,0) -> (100,20) -- 20 < stringPx (30)
      StrokeSample s;
      s.pos = Vec2{100.0f, static_cast<float>(i)};
      s.timestamp = t;
      t += 4'000'000ull;
      stab.addSample(s, out);
      raw.push_back(s.pos);
    }
    const Vec2 liftPoint = raw.back();
    const Vec2 nibBeforeCatchUp = stab.nibPos();

    // The RED line: what a straight-line catch-up (wave 1's forceCatchUp)
    // would have drawn -- the single segment from the lagging nib straight
    // to the lift point. Its worst deviation from the true polyline is at
    // the corner vertex itself (the segment cuts inside the L, the corner
    // sticks out of it).
    const float naiveCornerCut = distPointToSegment(corner, nibBeforeCatchUp, liftPoint);

    std::vector<StrokeSample> steps;
    check(stab.forceCatchUp(steps), "trajectory: forceCatchUp() reports a walk");
    check(steps.size() >= 2,
          "trajectory: the walk to the lift point emits more than one dab -- it does not jump "
          "in a single step across the corner");

    // Deviation of the walk from the raw polyline, measured from the nib's
    // own starting position onward -- that leg is part of the walk now: the
    // first step is the nib itself pulled a little way along, not a jump
    // sideways onto the polyline.
    float maxDeviation = 0.0f;
    for (size_t k = 0; k + 1 < steps.size(); ++k) {
      constexpr int kSamples = 20;
      for (int s = 0; s <= kSamples; ++s) {
        const float t = static_cast<float>(s) / kSamples;
        const Vec2 q{steps[k].pos.x + (steps[k + 1].pos.x - steps[k].pos.x) * t,
                    steps[k].pos.y + (steps[k + 1].pos.y - steps[k].pos.y) * t};
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < raw.size(); ++i)
          best = std::min(best, distPointToSegment(q, raw[i], raw[i + 1]));
        maxDeviation = std::max(maxDeviation, best);
      }
    }
    const float endError = distance(steps.back().pos, liftPoint);

    std::printf("  [measured] nib before catch-up (%.2f, %.2f); straight-line RED deviation at "
               "the corner %.2f px (~stringPx/sqrt(2) = %.2f); trajectory walk max deviation "
               "from the raw polyline %.4f px over %zu step(s); end error %.4f px\n",
               nibBeforeCatchUp.x, nibBeforeCatchUp.y, naiveCornerCut, 30.0f / std::sqrt(2.0f),
               maxDeviation, steps.size(), endError);

    check(naiveCornerCut > 10.0f,
          "trajectory: RED -- a straight-line catch-up cuts well past 0.5px across this L's "
          "corner");
    // THRESHOLD MOVED, deliberately: this read `< 0.5f` while the walk emitted
    // the raw samples themselves and so traced the polyline exactly. It now
    // replays the tail through the string, which ROUNDS this right angle
    // instead of tracing it -- the same trade the string makes everywhere
    // else, and the price of not painting raw pen jitter for the last string
    // length of every stroke (the release-fix block below measures that).
    // Half the straight line's cut is the claim that survives: the walk still
    // goes around the bend rather than across it.
    check(maxDeviation < 0.5f * naiveCornerCut,
          "trajectory: the walk rounds this L's corner at less than half the depth a straight-"
          "line catch-up cuts it");
    check(endError < 0.01f, "trajectory: the walk ends exactly at the lift point");
  }

  // ==========================================================================
  // Wave 2 item 2, persistence: `catchUpMs` round-trips through both files,
  // clamps like its neighbours, and `resolveStabiliser()` treats it as an
  // option (FollowGlobal passes it through unscaled) except under `Own`
  // (takes the brush's own value) -- `resolveStabiliser()`'s own comment on
  // why.
  // ==========================================================================
  {
    const std::string path = "/private/tmp/np-scatter/wave2/fix-catchupms-stroke-prefs.txt";
    StrokePreferencesStore writer;
    StabiliserParams toWrite;
    toWrite.mode = StabiliserMode::PulledString;
    toWrite.catchUpMs = 777.0f;
    std::string writeErr;
    PigmentBuildup writeBuildup;
    check(writer.saveToFile(path, toWrite, writeBuildup, &writeErr),
          "setup: catchUpMs stroke-preferences.txt fixture writes");
    StrokePreferencesStore reader;
    StabiliserParams reread;
    PigmentBuildup rereadBuildup;
    std::string readErr;
    check(reader.loadFromFile(path, reread, rereadBuildup, &readErr),
          "setup: catchUpMs stroke-preferences.txt fixture reads back");
    check(reread.catchUpMs == 777.0f,
          "persistence: catchUpMs round-trips through stroke-preferences.txt");

    StrokePreferencesStore clampReader;
    StabiliserParams clamped;
    PigmentBuildup clampBuildup;
    clampReader.parse("naturalPaint-stroke-preferences 1\ncatchUpMs 9999\n", clamped,
                      clampBuildup);
    check(clamped.catchUpMs == 2000.0f, "persistence: catchUpMs above 2000 clamps to 2000");
    StrokePreferencesStore clampReader2;
    StabiliserParams clamped2;
    PigmentBuildup clampBuildup2;
    clampReader2.parse("naturalPaint-stroke-preferences 1\ncatchUpMs -5\n", clamped2,
                       clampBuildup2);
    check(clamped2.catchUpMs == 0.0f, "persistence: catchUpMs below 0 clamps to 0");

    // user-presets.txt: the brush's own `catchUpMs`, a SEPARATE key
    // (`stabiliserCatchUpMs`) from the 6-field `stabiliser` line -- growing
    // that line's field count would break every file already on disk with
    // it (`taper`'s own precedent, `UserBrushLibrary.cpp`'s comment at the
    // read side). An OLD-format `stabiliser` line with no
    // `stabiliserCatchUpMs` beside it must still parse cleanly, at the
    // compiled-in own-default (400).
    const std::string oldFormatFixture =
        "naturalPaint-user-presets 1\n"
        "preset Old Format\n"
        "scalars 20 0.5 0.5 1 0 0.9 1.3\n"
        "stabiliser 2 100 1 16 40 50\n";  // mode=Own, own.mode=PulledString, no catchUpMs key
    UserBrushLibraryStore oldReader;
    BrushLibrary oldLib;
    oldReader.parse(oldFormatFixture, oldLib);
    const BrushPreset* oldPreset = nullptr;
    for (const BrushPreset& q : oldLib.presets)
      if (q.name == "Old Format") oldPreset = &q;
    check(oldPreset != nullptr &&
              oldPreset->native.stabiliser.mode == StabiliserBrushMode::Own &&
              oldPreset->native.stabiliser.own.stringPx == 16.0f &&
              oldPreset->native.stabiliser.own.catchUpMs == 400.0f,
          "persistence: an old-format `stabiliser` line (no stabiliserCatchUpMs) still parses, "
          "own.catchUpMs at its default");

    const std::string newFixture =
        "naturalPaint-user-presets 1\n"
        "preset New Format\n"
        "scalars 20 0.5 0.5 1 0 0.9 1.3\n"
        "stabiliser 2 100 1 16 40 50\n"
        "stabiliserCatchUpMs 888\n";
    UserBrushLibraryStore newReader;
    BrushLibrary newLib;
    newReader.parse(newFixture, newLib);
    const BrushPreset* newPreset = nullptr;
    for (const BrushPreset& q : newLib.presets)
      if (q.name == "New Format") newPreset = &q;
    check(newPreset != nullptr && newPreset->native.stabiliser.own.catchUpMs == 888.0f,
          "persistence: stabiliserCatchUpMs round-trips for a brush's Own setting");
    const std::string newResaved = newReader.serialize(newLib);
    check(newResaved.find("stabiliserCatchUpMs 888") != std::string::npos,
          "persistence: stabiliserCatchUpMs is written back out on save");

    // resolveStabiliser: FollowGlobal passes catchUpMs through UNSCALED (it
    // is a duration, not a "string length / strength" magnitude); Own takes
    // the brush's own.
    StabiliserParams global;
    global.catchUpMs = 500.0f;
    BrushStabiliserSetting followHalf;
    followHalf.mode = StabiliserBrushMode::FollowGlobal;
    followHalf.amountPct = 50.0f;
    const StabiliserParams effFollow = resolveStabiliser(global, followHalf);
    check(effFollow.catchUpMs == 500.0f,
          "resolveStabiliser: Follow global at 50% still carries the global's catchUpMs "
          "unscaled");
    BrushStabiliserSetting ownSetting;
    ownSetting.mode = StabiliserBrushMode::Own;
    ownSetting.own.catchUpMs = 250.0f;
    const StabiliserParams effOwn = resolveStabiliser(global, ownSetting);
    check(effOwn.catchUpMs == 250.0f,
          "resolveStabiliser: Own takes the brush's own catchUpMs, not the global's");
  }

  // ==========================================================================
  // String-fix brief (tablet feedback, second pulled-string session): "the
  // catchup is also causing issues where the brush path jitters between the
  // catchup position and the interpolated position ... oscillating around a
  // curve with a slow mouse". Root cause was the paused catch-up (old
  // `tickPulledString()`) snapping the nib onto the raw polyline via a
  // GLOBAL nearest-point search -- ill-conditioned on a jittery, near-
  // coincident path -- and engaging on ANY no-sample frame rather than a
  // genuine pause, so an ordinary missed-frame tick at a slow report rate
  // dissolved the string mid-stroke. The fix: one motion rule while the pen
  // is down -- the nib moves only by the pulled-string chord constraint
  // toward `lastRaw_.pos`, through a window that stays at the full string
  // length until the pen has been stationary (a fixed anchor) for
  // `kCatchUpHoldOffMs`, then shrinks exponentially -- so the two rules that
  // used to fight can no longer disagree.
  //
  // Shared fixture, built once per run: a SLOW pointer tracing a quarter-
  // circle arc (radius 120 px, ~0.6 px/sample, +-0.8 px deterministic
  // jitter), stringPx 4.1 (the user's own saved setting), catchUpMs 400,
  // ticked once between every pair of samples at a 16 ms frame cadence -- the
  // user's exact situation: a real pointer event every frame, spatially slow
  // enough that consecutive samples sit well under a pixel apart, plus one
  // extra frame poll (the tick) squeezed in between that finds no new sample.
  // ==========================================================================
  {
    constexpr float kHalfPi = 1.57079632679489661923f;
    constexpr float kRadius = 120.0f;
    constexpr float kArcStepPx = 0.6f;
    constexpr float kJitterPx = 0.8f;
    constexpr float kStringPxFixture = 4.1f;
    constexpr float kCatchUpMsFixture = 400.0f;
    const int numSteps = static_cast<int>(std::lround((kRadius * kHalfPi) / kArcStepPx));

    struct FixtureResult {
      // Assertion 1: worst (distAfter - distBefore) over any single tick --
      // positive means the nib moved AWAY from the pen on that tick.
      float worstAwayFromPen = 0.0f;
      float nibPathLen = 0.0f;  // assertion 2
      float rawPathLen = 0.0f;  // assertion 2
      float minGap = std::numeric_limits<float>::max();  // assertion 3
      bool everTaut = false;
      // A tick "engages" when it actually moves the nib -- assertion 3's own
      // direct signal. On a purely geometric pulled string, a raw sample can
      // legitimately leave slack under `stringPx` (the pointer jittering
      // sideways/backward relative to the nib is ordinary chord geometry,
      // nothing to do with catch-up) -- an absolute nib-to-pen-distance floor
      // can't tell that apart from real erosion, so this counts what the
      // paused catch-up itself actually did instead.
      int numEngagedTicks = 0;
    };

    // Runs the shared fixture once for a given `catchUpMs` so red (3d17b5f)
    // and green (fixed) numbers -- and the `catchUpMs = 0` control below --
    // come from the exact same raw path (same seed, same arc, same jitter).
    const auto runFixture = [&](float catchUpMs) {
      FixtureResult r;
      StabiliserParams p;
      p.mode = StabiliserMode::PulledString;
      p.stringPx = kStringPxFixture;
      p.catchUpMs = catchUpMs;
      Stabiliser stab;
      stab.begin(p, 1.0f);

      uint64_t rng = 20260911ull;  // fixed seed -- deterministic, no platform RNG dependence
      uint64_t t = 0;
      StrokeSample out;
      Vec2 prevRaw{};
      Vec2 prevNib{};
      bool haveRaw = false;
      bool haveNib = false;

      const auto noteGap = [&](float gap) {
        if (gap >= 0.99f * kStringPxFixture) r.everTaut = true;
        r.minGap = std::min(r.minGap, gap);
      };
      const auto noteNib = [&](Vec2 nibNow) {
        if (haveNib) r.nibPathLen += distance(nibNow, prevNib);
        prevNib = nibNow;
        haveNib = true;
      };

      for (int i = 0; i <= numSteps; ++i) {
        const float theta = (static_cast<float>(i) / static_cast<float>(numSteps)) * kHalfPi;
        const Vec2 base{kRadius * std::cos(theta), kRadius * std::sin(theta)};
        const Vec2 jitter{unitFloat(rng) * 2.0f * kJitterPx, unitFloat(rng) * 2.0f * kJitterPx};
        StrokeSample s;
        s.pos = Vec2{base.x + jitter.x, base.y + jitter.y};
        s.timestamp = t;

        stab.addSample(s, out);
        if (haveRaw) r.rawPathLen += distance(s.pos, prevRaw);
        prevRaw = s.pos;
        haveRaw = true;
        noteNib(stab.nibPos());
        noteGap(distance(stab.nibPos(), stab.rawPos()));

        if (i == numSteps) break;  // no tick follows the final sample

        t += 8'000'000ull;  // the missed-frame tick, halfway to the next sample
        const float distBefore = distance(stab.nibPos(), stab.rawPos());
        StrokeSample tickOut;
        if (stab.tick(t, tickOut)) ++r.numEngagedTicks;
        const float distAfter = distance(stab.nibPos(), stab.rawPos());
        r.worstAwayFromPen = std::max(r.worstAwayFromPen, distAfter - distBefore);
        noteNib(stab.nibPos());
        noteGap(distAfter);

        t += 8'000'000ull;  // the next real sample, one 16 ms frame period after the last
      }
      return r;
    };

    const FixtureResult res = runFixture(kCatchUpMsFixture);
    // Control: the identical raw path/jitter with catch-up fully disabled --
    // any gap it still shows is pulled string's own ordinary geometry (a
    // jittery raw sample landing closer to the nib than a straight pull
    // would leave it), never a "pause". Assertion 3 needs this to separate
    // that from what catch-up itself contributes.
    const FixtureResult control = runFixture(0.0f);
    std::printf("  [measured] string-fix: worst nib-away-from-pen step %.4f px; nib path %.2f px "
               "vs raw path %.2f px (ratio %.4f); min nib-to-pen gap %.4f px (no-catch-up control "
               "%.4f px); %d/%d ticks engaged (control %d)\n",
               res.worstAwayFromPen, res.nibPathLen, res.rawPathLen,
               res.nibPathLen / res.rawPathLen, res.minGap, control.minGap, res.numEngagedTicks,
               numSteps, control.numEngagedTicks);

    // 1. The nib never moves away from the pen across a tick.
    check(res.worstAwayFromPen <= 1e-4f,
          "string-fix 1: the nib never moves away from the pen across a tick");

    // 2. Smoothing actually smooths -- the nib's own path is well under the
    // raw (jittery) path's length.
    check(res.nibPathLen < 0.6f * res.rawPathLen,
          "string-fix 2: smoothing actually smooths (nib path length < 0.6x raw path length)");

    // 3. Catch-up does not engage while the pen is moving: this fixture
    // never truly pauses (the arc keeps advancing every sample), so the
    // paused catch-up should never once fire, and should therefore leave the
    // exact same minimum gap the no-catch-up control shows.
    check(res.everTaut, "setup: the string does reach its full length on this fixture");
    check(res.numEngagedTicks == 0,
          "string-fix 3: catch-up never actually engages while the pen keeps moving (0 of the "
          "ticks moved the nib)");
    check(std::fabs(res.minGap - control.minGap) < 1e-3f,
          "string-fix 3: with catch-up on, the minimum nib-to-pen gap matches the no-catch-up "
          "control exactly -- no erosion beyond pulled string's own ordinary geometry");
  }

  // ==========================================================================
  // Release-fix (tablet feedback, third pulled-string session): with the
  // paused catch-up stable, "the release and catchup mechanism ... is still
  // resulting in a hitch". The release walk emitted the RAW samples of the
  // tail it crossed, so the last string length of every stroke was painted
  // through unsmoothed pen positions -- the one stretch of the stroke the
  // stabiliser was switched on for and did not stabilise -- reached by a step
  // sideways off the nib onto that polyline. It now replays the same tail
  // through the string with the window ramped from the nib's lag to zero, so
  // the walk starts where the nib is and stays smoothed until the last steps,
  // where the window has to reach zero for the stroke to end at the pen.
  //
  // Fixture: a slow pointer on a STRAIGHT line (0.6 px/sample, +-0.8 px
  // deterministic jitter, stringPx 4.1 -- the user's own saved setting), so
  // the ideal walk is nearly straight and anything the walk paints beyond
  // that is jitter it should not have had. The old walk is recomputed here
  // from the same raw samples, the same way it did it (project the nib onto
  // the whole polyline, emit every raw sample past that arc length), so the
  // red numbers are measured in this run rather than quoted from a stashed
  // build.
  // ==========================================================================
  {
    constexpr float kStringPxFixture = 4.1f;
    constexpr float kStepPx = 0.6f;
    constexpr float kJitterPx = 0.8f;
    constexpr int kNumSamples = 300;

    StabiliserParams p;
    p.mode = StabiliserMode::PulledString;
    p.stringPx = kStringPxFixture;
    p.catchUpMs = 0.0f;  // the release is what is under test, not the pause
    Stabiliser stab;
    stab.begin(p, 1.0f);

    uint64_t rng = 20260911ull;  // fixed seed -- deterministic, no platform RNG
    uint64_t t = 0;
    StrokeSample out;
    std::vector<Vec2> raw;
    for (int i = 0; i < kNumSamples; ++i) {
      StrokeSample sm;
      sm.pos = Vec2{static_cast<float>(i) * kStepPx + unitFloat(rng) * 2.0f * kJitterPx,
                    unitFloat(rng) * 2.0f * kJitterPx};
      sm.timestamp = t;
      t += 4'000'000ull;
      stab.addSample(sm, out);
      raw.push_back(sm.pos);
    }
    const Vec2 liftPoint = raw.back();
    const Vec2 nibBefore = stab.nibPos();

    // The RED walk: the old algorithm, on this same history.
    std::vector<Vec2> redWalk;
    {
      float acc = 0.0f, bestD2 = std::numeric_limits<float>::max(), s0 = 0.0f;
      for (size_t i = 0; i + 1 < raw.size(); ++i) {
        const Vec2 a = raw[i], b = raw[i + 1];
        const float ux = b.x - a.x, uy = b.y - a.y;
        const float len2 = ux * ux + uy * uy;
        float u = 0.0f;
        if (len2 > 1e-9f)
          u = std::clamp(((nibBefore.x - a.x) * ux + (nibBefore.y - a.y) * uy) / len2, 0.0f, 1.0f);
        const float d2 = distance(nibBefore, Vec2{a.x + ux * u, a.y + uy * u});
        if (d2 * d2 < bestD2) {
          bestD2 = d2 * d2;
          s0 = acc + std::sqrt(len2) * u;
        }
        acc += std::sqrt(len2);
      }
      acc = 0.0f;
      for (size_t i = 0; i + 1 < raw.size(); ++i) {
        acc += distance(raw[i], raw[i + 1]);
        if (acc > s0) redWalk.push_back(raw[i + 1]);
      }
    }

    std::vector<StrokeSample> steps;
    check(stab.forceCatchUp(steps), "release-fix: forceCatchUp() reports a walk");
    std::vector<Vec2> greenWalk;
    for (const StrokeSample& st : steps) greenWalk.push_back(st.pos);

    // Length of each walk (from the nib, where the stroke actually is) and
    // its worst sideways excursion from the straight nib -> lift line. On a
    // straight fixture both are pure jitter: the straight line is the ideal.
    auto measure = [&](const std::vector<Vec2>& walk, float& len, float& wobble,
                       float& biggestStep) {
      len = 0.0f;
      wobble = 0.0f;
      biggestStep = 0.0f;
      Vec2 prev = nibBefore;
      const float ux = liftPoint.x - nibBefore.x, uy = liftPoint.y - nibBefore.y;
      const float chord = std::hypot(ux, uy);
      for (const Vec2& q : walk) {
        len += distance(prev, q);
        biggestStep = std::max(biggestStep, distance(prev, q));
        prev = q;
        if (chord > 1e-6f)
          wobble = std::max(wobble, std::fabs((q.x - nibBefore.x) * uy -
                                              (q.y - nibBefore.y) * ux) / chord);
      }
    };
    float redLen = 0.0f, redWobble = 0.0f, redStep = 0.0f;
    float greenLen = 0.0f, greenWobble = 0.0f, greenStep = 0.0f;
    measure(redWalk, redLen, redWobble, redStep);
    measure(greenWalk, greenLen, greenWobble, greenStep);
    const float chord = distance(nibBefore, liftPoint);

    std::printf("  [measured] release-fix: nib lag %.2f px; walk length RED %.2f px / GREEN "
                "%.2f px (straight line %.2f px); sideways wobble RED %.4f px / GREEN %.4f px; "
                "biggest single step RED %.4f px / GREEN %.4f px; steps RED %zu / GREEN %zu; "
                "end error %.4f px\n",
                chord, redLen, greenLen, chord, redWobble, greenWobble, redStep, greenStep,
                redWalk.size(), greenWalk.size(), distance(greenWalk.back(), liftPoint));

    check(redLen > 1.2f * chord,
          "release-fix: RED -- emitting the raw samples paints noticeably more path than the "
          "tail is long, because it paints the jitter");
    check(greenLen < 1.05f * chord,
          "release-fix: the replayed walk is within 5% of the straight-line length -- the "
          "jitter is gone, not merely reduced");
    check(greenWobble < 0.5f * redWobble,
          "release-fix: the walk's worst sideways excursion is less than half the raw tail's");
    check(distance(greenWalk.back(), liftPoint) < 1e-3f,
          "release-fix: the walk still ends exactly at the lift point");
    // The window has to actually REACH zero along the tail, not be cut to zero
    // at the last step by the lift point being written in exactly: a ramp that
    // stalls leaves the whole lag in one final jump.
    check(greenStep <= redStep,
          "release-fix: the walk advances in steps no larger than the pen's own samples -- the "
          "window ramps to zero across the tail rather than jumping at the end");
    // And the tail walked is the one the nib actually lags behind, found by
    // walking back from the lift point -- not an arc length picked by a
    // nearest-point search over the whole stroke, which can land anywhere the
    // path happens to pass close by.
    check(static_cast<float>(greenWalk.size()) < 2.0f * chord / kStepPx,
          "release-fix: the walk spans only the tail the nib lags behind, not an arbitrary "
          "earlier stretch of the stroke");
  }

  std::printf("[selftest] stabiliser %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
