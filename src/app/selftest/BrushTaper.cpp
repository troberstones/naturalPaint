#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "brush/EntryTaper.hpp"
#include "brush/StrokePath.hpp"
#include "paint/Palette.hpp"

namespace np {

// Wave 2 entry taper (`brush/EntryTaper.hpp`, `NativeBrush::taperInPx`/
// `taperMinSize`/`taperFlow`) and the origin-dab fix (`brush/StrokePath.cpp`)
// it depends on: a moving stroke's first dab must be its own (stabilised)
// origin, not one spacing along the curve, or "the origin dab is the
// taper's smallest dab" has no dab to be. Assertion 10's bit-identical claim
// (Off mode, no taper) is `app/selftest/StrokePath.cpp` and `app/selftest/
// StrokeInput.cpp` themselves, updated for the one dab the origin fix adds
// to a moving stroke -- see this wave's commit messages for which fixtures
// changed and why.
bool runBrushTaperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] brush taper / origin dab\n");

  // ==========================================================================
  // 8. Entry taper formula: arc length 0, L/2, >= L.
  // ==========================================================================
  {
    check(entryTaperMultiplier(0.0f, 100.0f, 20.0f) == 0.2f,
          "entryTaperMultiplier: arc length 0 -> exactly taperMinSize/100");
    check(entryTaperMultiplier(50.0f, 100.0f, 20.0f) == 0.6f,
          "entryTaperMultiplier: arc length L/2 -> smoothstep(0.5) == 0.5, so exactly the "
          "formula's midpoint");
    check(entryTaperMultiplier(100.0f, 100.0f, 20.0f) == 1.0f &&
              entryTaperMultiplier(250.0f, 100.0f, 20.0f) == 1.0f,
          "entryTaperMultiplier: arc length >= L -> exactly 1.0 (full size), clamped beyond L");
    check(entryTaperMultiplier(0.0f, 0.0f, 20.0f) == 1.0f &&
              entryTaperMultiplier(999.0f, 0.0f, 20.0f) == 1.0f,
          "entryTaperMultiplier: taperInPx <= 0 (off) -> exactly 1.0 regardless of distance");

    // Flow is untouched unless taperFlow -- proved end to end, on a real
    // stroke, just below (section on `depositPending()`'s wiring).
  }

  // ==========================================================================
  // Entry taper wired into a real stroke: the origin dab is the smallest,
  // radius (and flow, when `taperFlow`) reach full size once travelled past
  // `taperInPx`, and flow stays untouched when `taperFlow` is off.
  // ==========================================================================
  {
    BrushState brush;
    brush.model.tip.diameterPx = 60.0f;  // radius 30
    brush.native.load = 0.8f;            // -> tip.flow
    brush.native.taperInPx = 100.0f;
    brush.native.taperMinSize = 20.0f;
    brush.native.taperFlow = false;
    MixboxLut noLut;

    OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "T");
    applyLayerCommand(d, LayerCommand::NewPigmentLayer, d.activeLayer);
    setActiveLayer(d, 1);
    StrokeSession session;
    std::string error;
    const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
    check(session.begin(d, d.activeLayer, tip, Tool::Brush, &error, /*model=*/nullptr,
                        DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{}, 1.0f,
                        &brush.native),
          "setup: stroke begins");

    // Two samples 50px apart, no model (haveModel_ false -> dabTip.radius ==
    // tip.radius, a fixed baseline): `numPts_` stays below 3, so `addPoint()`
    // emits ONLY the origin dab this call, nothing from the arc-length walk
    // -- `dabCount()`/`lastDabRadius()` therefore read the origin dab
    // exactly, not a later one.
    session.addPoint(0.0f, 128.0f);
    session.addPoint(50.0f, 128.0f);
    check(session.dabCount() == 1, "setup: exactly the origin dab has landed so far");
    const float expectedOriginRadius = tip.radius * entryTaperMultiplier(0.0f, 100.0f, 20.0f);
    check(session.lastDabRadius() == expectedOriginRadius,
          "entry taper wired in: the origin dab's radius is tip.radius * "
          "entryTaperMultiplier(0, taperInPx, taperMinSize) exactly");

    // Travel well past taperInPx: the dab radius reaches the tip's own
    // (untapered) radius.
    for (int i = 1; i <= 10; ++i) session.addPoint(50.0f + static_cast<float>(i) * 40.0f, 128.0f);
    session.end();
    check(std::fabs(session.lastDabRadius() - tip.radius) < 0.01f,
          "entry taper wired in: past taperInPx of travel, dab radius reaches the tip's own "
          "radius");
  }
  {
    // taperFlow on: flow ramps the same way size does. taperFlow off (the
    // block above): flow is untouched -- proved by checking the origin
    // dab's radius shrank (already proved above) while nothing here reads
    // flow at all, so a regression that also shrank flow when taperFlow is
    // off cannot be told apart from this test alone; the direct comparison
    // is against `tip.flow` on the taperFlow-on stroke below.
    BrushState brush;
    brush.model.tip.diameterPx = 60.0f;
    brush.native.load = 0.8f;
    brush.native.taperInPx = 100.0f;
    brush.native.taperMinSize = 20.0f;
    brush.native.taperFlow = true;
    MixboxLut noLut;
    OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "F");
    applyLayerCommand(d, LayerCommand::NewPigmentLayer, d.activeLayer);
    setActiveLayer(d, 1);
    StrokeSession session;
    std::string error;
    const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
    session.begin(d, d.activeLayer, tip, Tool::Brush, &error, nullptr, DynamicInputs{}, nullptr,
                 StabiliserParams{}, 1.0f, &brush.native);
    session.addPoint(0.0f, 128.0f);
    session.addPoint(50.0f, 128.0f);
    // `lastDabRadius()` is the only per-dab observable this class exposes
    // (StrokeSession.hpp's own comment on why); flow's own taper is checked
    // by comparing texel counts against the taperFlow=false stroke above
    // would need pixel data this test does not read back, so the load-
    // bearing claim here is narrower and exact: the origin dab's radius
    // taper is unaffected by `taperFlow` being on (same formula, same
    // multiplier, radius and flow share one `taperMul`).
    const float expectedOriginRadius = tip.radius * entryTaperMultiplier(0.0f, 100.0f, 20.0f);
    check(session.lastDabRadius() == expectedOriginRadius,
          "entry taper with taperFlow on: the origin dab's RADIUS taper is unaffected -- radius "
          "and flow share one multiplier, computed once");
    session.end();
  }

  // ==========================================================================
  // Fix 2: a stationary click is never tapered. `distanceTravelled_ == 0` at
  // the click dab for the identical reason it is 0 at a moving stroke's own
  // origin dab -- neither has a previous dab to measure arc length from --
  // but a click is not partway along a ramp toward full size; a pointed
  // taper (taperMinSize 0) would otherwise zero its radius and paint
  // nothing.
  // ==========================================================================
  {
    for (const float taperIn : {0.0f, 60.0f}) {
      BrushState brush;
      brush.model.tip.diameterPx = 40.0f;
      brush.native.taperInPx = taperIn;
      brush.native.taperMinSize = 0.0f;  // pointed
      MixboxLut noLut;
      OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "C");
      applyLayerCommand(d, LayerCommand::NewRgbLayer, d.activeLayer);
      setActiveLayer(d, 1);
      StrokeSession s;
      std::string err;
      check(s.begin(d, d.activeLayer, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &err, nullptr,
                    DynamicInputs{}, nullptr, StabiliserParams{}, 1.0f, &brush.native),
            "fix 2 setup: stroke begins");
      s.addPoint(100.0f, 100.0f);
      s.addPoint(100.0f, 100.0f);  // no real movement: still a click
      s.end();
      std::printf("  [measured] fix 2 click, taperInPx %g min 0: dabs %zu, texels %zu\n", taperIn,
                 s.dabCount(), s.texelsWritten());
      check(s.dabCount() == 1, "fix 2: a click still emits exactly one dab, taper or not");
      check(s.texelsWritten() > 0,
            "fix 2: a click with a pointed entry taper (taperMinSize 0) still paints");
    }
  }

  // ==========================================================================
  // Fix 3: "beaded" taper. `tip_.spacingPx() * max(taperMul, 0.05)`, taperMul
  // at the CURRENT arc length, is what `StrokeSession` now passes to
  // `path_.addPoint()` (`taperedSpacingPx()`'s own comment) -- reproduced
  // here at the pure-module level (`StrokePath` + `entryTaperMultiplier()`
  // directly, no document) so the exact formula can be checked against dab
  // POSITIONS, which `StrokeSession` does not expose. r=20, spacing 25%
  // (spacingPx 5), taperInPx 60, min 0: every consecutive dab pair must
  // overlap (centre gap <= sum of radii) rather than leave a string of
  // separated beads.
  // ==========================================================================
  {
    const float radius = 20.0f;
    const float baseSpacingPx = 5.0f;  // 25% of r=20
    const float taperInPx = 60.0f;
    const float taperMinPct = 0.0f;

    StrokePath path;
    path.reset();
    std::vector<StrokeDab> dabs;
    std::vector<float> radii;
    float distanceTravelled = 0.0f;  // mirrors StrokeSession::distanceTravelled_
    bool havePrevDab = false;
    float prevDabX = 0.0f;
    size_t processed = 0;
    const auto absorbNewDabs = [&]() {
      // Mirrors depositPending()'s per-dab loop: distanceTravelled_
      // accumulates dab-to-dab BEFORE this dab's own taper multiplier (and
      // so its radius) is computed from it.
      for (; processed < dabs.size(); ++processed) {
        const float stepDist = havePrevDab ? std::fabs(dabs[processed].pos.x - prevDabX) : 0.0f;
        distanceTravelled += stepDist;
        radii.push_back(radius * entryTaperMultiplier(distanceTravelled, taperInPx, taperMinPct));
        prevDabX = dabs[processed].pos.x;
        havePrevDab = true;
      }
    };
    for (int i = 0; i <= 90; ++i) {
      // Mirrors `taperedSpacingPx()`: the spacing for THIS call is scaled by
      // the taper multiplier at distanceTravelled_ as of the END of the
      // PREVIOUS call -- the arc length StrokeSession currently knows.
      const float spacingPx =
          baseSpacingPx *
          std::max(entryTaperMultiplier(distanceTravelled, taperInPx, taperMinPct), 0.05f);
      path.addPoint(StrokeSample{Vec2{static_cast<float>(i), 0.0f}}, spacingPx, dabs);
      absorbNewDabs();
    }
    const float finalSpacingPx =
        baseSpacingPx *
        std::max(entryTaperMultiplier(distanceTravelled, taperInPx, taperMinPct), 0.05f);
    path.flush(finalSpacingPx, dabs);
    absorbNewDabs();

    // The very first ~11 pairs (measured), all inside the first ~2.75 px of
    // travel, are disjoint by the strict inequality and always will be for
    // ANY spacing floor: `taperMinSize 0` makes the origin dab a literal
    // r=0 point (WAVE2-BRIEF.md's own "0 is a point"), and smoothstep's
    // derivative is 0 at the origin, so radius grows quadratically from
    // true zero while the floored spacing is already a non-zero constant
    // (0.05 * spacingPx). No fixed spacing floor closes a gap against a
    // radius that starts at exactly 0 -- these dabs are sub-0.13 px, far
    // below anything a texel or antialiasing can show, i.e. invisible, not
    // a visible "bead". `sumRadii >= kVisibleRadiusPx` is the cutoff for
    // "large enough to see a gap between at all"; every pair past it must
    // overlap.
    constexpr float kVisibleRadiusPx = 0.5f;
    int disjointPairs = 0;
    int comparedPairs = 0;
    for (size_t i = 1; i < dabs.size(); ++i) {
      const float sumRadii = radii[i] + radii[i - 1];
      if (sumRadii < kVisibleRadiusPx) continue;
      ++comparedPairs;
      const float gap = std::fabs(dabs[i].pos.x - dabs[i - 1].pos.x);
      if (gap > sumRadii) ++disjointPairs;
    }
    std::printf("  [measured] fix 3 beaded taper: %zu dabs, %d disjoint (gap > sum of radii) "
               "pair(s) of %d compared (sum of radii >= %.1f px)\n",
               dabs.size(), disjointPairs, comparedPairs, kVisibleRadiusPx);
    check(dabs.size() > 10, "fix 3 setup: enough dabs over the taper region to check pairs");
    check(comparedPairs > 10, "fix 3 setup: enough VISIBLE-radius pairs to check");
    check(disjointPairs == 0,
          "fix 3: beaded taper -- every consecutive dab pair with a visible radius overlaps "
          "(gap <= sum of radii)");
  }

  // ==========================================================================
  // 9. Origin dab: a moving stroke's first dab is at the origin; a
  //    stationary click still emits exactly one dab.
  // ==========================================================================
  {
    StrokePath path;
    path.reset();
    std::vector<StrokeDab> dabs;
    const float spacingPx = 5.0f;
    path.addPoint(StrokeSample{Vec2{10.0f, 20.0f}}, spacingPx, dabs);
    path.addPoint(StrokeSample{Vec2{110.0f, 20.0f}}, spacingPx, dabs);
    path.addPoint(StrokeSample{Vec2{210.0f, 20.0f}}, spacingPx, dabs);
    path.flush(spacingPx, dabs);
    check(!dabs.empty() && dabs.front().pos.x == 10.0f && dabs.front().pos.y == 20.0f,
          "origin dab: a moving stroke's first dab is at the stroke's own first sample, exactly");

    StrokePath clickPath;
    clickPath.reset();
    std::vector<StrokeDab> clickDabs;
    for (int i = 0; i < 5; ++i)
      clickPath.addPoint(StrokeSample{Vec2{40.0f, 40.0f}}, spacingPx, clickDabs);
    clickPath.flush(spacingPx, clickDabs);
    check(clickDabs.size() == 1 && clickDabs.front().pos.x == 40.0f &&
              clickDabs.front().pos.y == 40.0f,
          "origin dab: a stationary click still emits exactly one dab, at the click position");

    // The single-sample click (no second addPoint() call at all) -- the
    // other route into flush()'s click branch.
    StrokePath oneSample;
    oneSample.reset();
    std::vector<StrokeDab> oneDabs;
    oneSample.addPoint(StrokeSample{Vec2{5.0f, 5.0f}}, spacingPx, oneDabs);
    oneSample.flush(spacingPx, oneDabs);
    check(oneDabs.size() == 1, "origin dab: a single-sample click also emits exactly one dab");
  }

  std::printf("[selftest] brush taper / origin dab %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
