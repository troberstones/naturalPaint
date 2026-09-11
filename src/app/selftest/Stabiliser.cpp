#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

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
        StrokeSample snapped;
        if (stab.forceCatchUp(snapped)) path.addPoint(snapped, spacingPx, dabs);
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
    check(effFollow.mode == StabiliserMode::WeightedAverage &&
              effFollow.stringPx == 10.0f && effFollow.strength == 20.0f &&
              effFollow.responsiveness == 15.0f && effFollow.catchUpAtEnd == true &&
              effFollow.catchUpWhilePaused == false && effFollow.stabilisePressure == true &&
              effFollow.scaleWithZoom == true && effFollow.showString == false,
          "resolveStabiliser: follow global x50% halves stringPx/strength/responsiveness, "
          "keeps mode and every option");

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
    const std::string written = store.serialize(g);
    StabiliserParams reloaded;
    StrokePreferencesStore reader;
    reader.parse(written, reloaded);
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
    StrokePreferencesStore unknownReader;
    unknownReader.parse(withUnknown, parsedUnknown);
    const std::string resaved = unknownReader.serialize(parsedUnknown);
    check(resaved.find("futureKey 42") != std::string::npos,
          "stroke-preferences.txt: an unknown key survives a parse/serialize round trip");

    // user-presets.txt: the new `taper`/`stabiliser` preset lines round trip.
    BrushPreset preset;
    preset.name = "Taper And Stabiliser";
    preset.native.taperInPx = 48.0f;
    preset.native.taperMinSize = 15.0f;
    preset.native.taperFlow = true;
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
    check(back != nullptr && back->native.taperInPx == 48.0f &&
              back->native.taperMinSize == 15.0f && back->native.taperFlow == true,
          "user-presets.txt: the new `taper` line round-trips exactly");
    check(back != nullptr && back->native.stabiliser.mode == StabiliserBrushMode::Own &&
              back->native.stabiliser.amountPct == 250.0f &&
              back->native.stabiliser.own.mode == StabiliserMode::WeightedAverage &&
              back->native.stabiliser.own.stringPx == 5.0f &&
              back->native.stabiliser.own.strength == 33.0f &&
              back->native.stabiliser.own.responsiveness == 66.0f,
          "user-presets.txt: the new `stabiliser` line round-trips exactly");

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

    // The `.abr` mapping.
    check(stabiliserBrushModeFromAbrSmoothing(true) == StabiliserBrushMode::FollowGlobal &&
              stabiliserBrushModeFromAbrSmoothing(false) == StabiliserBrushMode::Off,
          "ABR import: toolOptions/smoothing true -> Follow global, false -> Off");
  }

  std::printf("[selftest] stabiliser %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
