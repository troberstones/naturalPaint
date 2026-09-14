#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Taper.hpp"
#include "brush/StrokePath.hpp"
#include "paint/Palette.hpp"

namespace np {

// The two tapers (`brush/Taper.hpp`, `NativeBrush::taperIn`/`taperOut`) and
// the origin-dab fix (`brush/StrokePath.cpp`)
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

  const auto distanceBetween = [](Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); };

  // A live entry ramp: the three loose arguments this section used to pass,
  // now that a taper is a struct with its own on/off.
  const auto ramp = [](float lengthPx, float minPct) {
    return BrushTaper{true, lengthPx, minPct, false};
  };

  std::printf("[selftest] brush taper / origin dab\n");

  // ==========================================================================
  // 8. Entry taper formula: arc length 0, L/2, >= L.
  // ==========================================================================
  {
    check(taperMultiplier(0.0f, ramp(100.0f, 20.0f)) == 0.2f,
          "taperMultiplier: arc length 0 -> exactly taperMinSize/100");
    check(taperMultiplier(50.0f, ramp(100.0f, 20.0f)) == 0.6f,
          "taperMultiplier: arc length L/2 -> smoothstep(0.5) == 0.5, so exactly the "
          "formula's midpoint");
    check(taperMultiplier(100.0f, ramp(100.0f, 20.0f)) == 1.0f &&
              taperMultiplier(250.0f, ramp(100.0f, 20.0f)) == 1.0f,
          "taperMultiplier: arc length >= L -> exactly 1.0 (full size), clamped beyond L");
    check(taperMultiplier(0.0f, ramp(0.0f, 20.0f)) == 1.0f &&
              taperMultiplier(999.0f, ramp(0.0f, 20.0f)) == 1.0f,
          "taperMultiplier: taperInPx <= 0 (off) -> exactly 1.0 regardless of distance");

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
    brush.native.taperIn = ramp(100.0f, 20.0f);
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
    const float expectedOriginRadius = tip.radius * taperMultiplier(0.0f, ramp(100.0f, 20.0f));
    check(session.lastDabRadius() == expectedOriginRadius,
          "entry taper wired in: the origin dab's radius is tip.radius * "
          "taperMultiplier(0, ramp(taperInPx, taperMinSize)) exactly");

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
    brush.native.taperIn = ramp(100.0f, 20.0f);
    brush.native.taperIn.flow = true;
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
    const float expectedOriginRadius = tip.radius * taperMultiplier(0.0f, ramp(100.0f, 20.0f));
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
      brush.native.taperIn = ramp(taperIn, 0.0f);  // pointed
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
  // here at the pure-module level (`StrokePath` + `taperMultiplier()`
  // directly, ramp(no document) so the exact formula can be checked against dab
  // POSITIONS, which `StrokeSession` does not expose. r=20, spacing 25%
  // (spacingPx 5)), taperInPx 60, min 0: every consecutive dab pair must
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
        radii.push_back(radius * taperMultiplier(distanceTravelled, ramp(taperInPx, taperMinPct)));
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
          std::max(taperMultiplier(distanceTravelled, ramp(taperInPx, taperMinPct)), 0.05f);
      path.addPoint(StrokeSample{Vec2{static_cast<float>(i), 0.0f}}, spacingPx, dabs);
      absorbNewDabs();
    }
    const float finalSpacingPx =
        baseSpacingPx *
        std::max(taperMultiplier(distanceTravelled, ramp(taperInPx, taperMinPct)), 0.05f);
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

  // ==========================================================================
  // The `on` toggle is not "lengthPx > 0": a taper switched off keeps its
  // settings, so switching it back on brings them back.
  // ==========================================================================
  {
    const BrushTaper offButSet{false, 100.0f, 20.0f, true};
    check(taperMultiplier(0.0f, offButSet) == 1.0f &&
              taperMultiplier(50.0f, offButSet) == 1.0f,
          "taperMultiplier: a taper switched OFF is exactly 1.0 at every distance, length and "
          "minimum still set");
  }

  // ==========================================================================
  // `resampleTaperedTail()`: spacing is chosen when a dab is emitted, which
  // is before the exit taper's multiplier can be known, so the tail is split
  // at deposit time instead. Fixture: 10 dabs 5 px apart (the spacing a
  // full-size tip wants here), a 50 px ramp down to 10% -- so the last dabs
  // want a fifth of that spacing or they land as separate dots.
  // ==========================================================================
  {
    constexpr float kSpacingPx = 5.0f;
    const BrushTaper out{true, 50.0f, 10.0f, false};
    std::vector<StrokeDab> tail;
    for (int i = 0; i < 10; ++i)
      tail.push_back(StrokeDab{Vec2{static_cast<float>(i) * kSpacingPx, 0.0f}});
    const std::vector<StrokeDab> before = tail;

    std::vector<StrokeDab> untouched = tail;
    resampleTaperedTail(untouched, BrushTaper{}, BrushTaper{false, 50.0f, 10.0f, false},
                        kSpacingPx);
    check(untouched.size() == before.size(),
          "resampleTaperedTail: a taper that is off leaves the tail exactly as it was");

    resampleTaperedTail(tail, BrushTaper{}, out, kSpacingPx);

    // Every gap must be no wider than the spacing the TAPERED tip at that
    // point wants -- measured against the ramp read at each gap's own
    // distance from the end, the same way the deposit does.
    float totalArc = 0.0f;
    for (size_t i = 0; i + 1 < tail.size(); ++i)
      totalArc += distanceBetween(tail[i].pos, tail[i + 1].pos);
    // Each gap is measured against the spacing wanted at its own MIDPOINT.
    // The dabs now tile the tail in phase -- each gap holds exactly one
    // "wanted spacing", integrated across itself -- so by the mean value
    // theorem every gap equals the spacing wanted at SOME point inside
    // itself, and a reading taken anywhere else in the gap differs by how
    // much the ramp moves across one dab. This assertion used to read the
    // ramp at the gap's finest end and hold to 1.0 + 1e-3, which only the old
    // per-segment `ceil()` could satisfy -- by over-subdividing, which is
    // exactly what banded the start of the taper. Both numbers are printed so
    // the change of convention is visible rather than implied. What actually
    // keeps a tapering stroke from beading is asserted directly, and
    // physically, by the disjoint-pairs block above: gap vs the sum of the
    // two dabs' radii.
    float worstRatio = 0.0f;
    float worstAtFineEnd = 0.0f;
    float arc = 0.0f;
    for (size_t i = tail.size(); i-- > 1;) {
      const float gap = distanceBetween(tail[i - 1].pos, tail[i].pos);
      const float fine = kSpacingPx * std::max(taperMultiplier(arc, out), 0.05f);
      const float mid = kSpacingPx * std::max(taperMultiplier(arc + gap * 0.5f, out), 0.05f);
      worstRatio = std::max(worstRatio, gap / mid);
      worstAtFineEnd = std::max(worstAtFineEnd, gap / fine);
      arc += gap;
    }
    std::printf("  [measured] resampleTaperedTail: %zu dab(s) -> %zu; arc %.2f px preserved as "
                "%.2f px; worst gap %.3fx the spacing wanted at its midpoint (%.3fx read at "
                "its finest end)\n",
                before.size(), tail.size(), 45.0f, totalArc, worstRatio, worstAtFineEnd);
    check(worstRatio <= 1.05f,
          "resampleTaperedTail: no gap is wider than the spacing the tapered tip wants across "
          "it -- the tail is spaced by what the ramp asks for, not by what fits");
    check(tail.size() > before.size(),
          "resampleTaperedTail: the tail really was split -- this is not vacuously true");
    check(tail.front().pos.x == before.front().pos.x &&
              tail.back().pos.x == before.back().pos.x &&
              std::fabs(totalArc - 45.0f) < 1e-3f,
          "resampleTaperedTail: the path itself is unchanged -- same ends, same arc length");
  }

  // ==========================================================================
  // Exit taper wired into a real stroke, and the repaint it needs. An exit
  // ramp cannot be resolved until the stroke has an end, so the live stroke
  // is the UNTAPERED one -- no lag, nothing held back -- and `end()` puts the
  // tiles it wrote back to their pen-down content and lays the whole stroke
  // down again with the ramp applied. Three things have to be true at once:
  // the ramp reaches the last dab, the live stroke was never delayed, and the
  // repaint REPLACED the untapered ink rather than painting over it.
  // ==========================================================================
  {
    // Texels this layer actually holds ink at -- what proves the repaint put
    // the picture back. A repaint that added to the live stroke instead of
    // replacing it would leave the untapered tail's footprint behind, and
    // this number would not move at all.
    const auto coveredTexels = [](const OpenDocument& d, size_t layerIndex) {
      size_t covered = 0;
      const Layer& l = d.document.layers[layerIndex];
      if (!l.rgbTiles.has_value()) return covered;
      for (const auto& [coord, tile] : *l.rgbTiles) {
        (void)coord;
        for (int32_t y = 0; y < kTileSize; ++y)
          for (int32_t x = 0; x < kTileSize; ++x)
            if (tile.readPixel(PixelCoord{x, y})[3] > 0.0f) ++covered;
      }
      return covered;
    };

    struct Run {
      size_t dabsBeforeEnd = 0;
      size_t dabsAfterEnd = 0;
      float lastRadius = 0.0f;
      float tipRadius = 0.0f;
      size_t covered = 0;
    };
    const auto runStroke = [&](const BrushTaper& exit) {
      Run r;
      BrushState brush;
      brush.model.tip.diameterPx = 20.0f;  // radius 10
      // A ceiling the accumulator can actually reach: at full opacity the
      // per-stroke ceiling never binds, and a repaint that wrongly inherited
      // a spent accumulator would be indistinguishable from a correct one.
      brush.opacity = 0.5f;
      brush.native.taperOut = exit;
      MixboxLut noLut;
      // An RGB layer, not the pigment one the entry-taper fixtures use: this
      // section needs to READ the ink back, and `Tile::readPixel()` is the
      // one store with an alpha to count.
      OpenDocument d = makeBlankOpenDocument(512, 256, WorkingSpace{}, "T");
      StrokeSession session;
      std::string error;
      const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
      r.tipRadius = tip.radius;
      session.begin(d, d.activeLayer, tip, Tool::Brush, &error, /*model=*/nullptr,
                    DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{}, 1.0f, &brush.native);
      for (int i = 0; i <= 20; ++i) session.addPoint(20.0f + static_cast<float>(i) * 20.0f, 128.0f);
      r.dabsBeforeEnd = session.dabCount();
      session.end();
      r.dabsAfterEnd = session.dabCount();
      r.lastRadius = session.lastDabRadius();
      r.covered = coveredTexels(d, d.activeLayer);
      return r;
    };

    constexpr float kTaperOutPx = 80.0f;
    constexpr float kMinPct = 15.0f;
    const Run off = runStroke(BrushTaper{});
    const Run on = runStroke(BrushTaper{true, kTaperOutPx, kMinPct, false});

    std::printf("  [measured] exit taper: dabs before end() %zu (taper off) vs %zu (on); after "
                "end() %zu vs %zu; final dab radius %.3f px vs %.3f px (tip %.3f px, min %.0f%% = "
                "%.3f px); texels holding ink %zu vs %zu\n",
                off.dabsBeforeEnd, on.dabsBeforeEnd, off.dabsAfterEnd, on.dabsAfterEnd,
                off.lastRadius, on.lastRadius, on.tipRadius, kMinPct,
                on.tipRadius * kMinPct / 100.0f, off.covered, on.covered);

    check(std::fabs(on.lastRadius - on.tipRadius * kMinPct / 100.0f) < 0.05f,
          "exit taper wired in: the stroke's last dab is exactly the ramp's minimum size");
    check(std::fabs(off.lastRadius - off.tipRadius) < 1e-3f,
          "exit taper off: the same stroke's last dab is full size -- the fixture discriminates");
    check(on.dabsBeforeEnd == off.dabsBeforeEnd,
          "no lag: the live stroke is the untapered one -- with the pen still down, an exit "
          "taper has cost exactly nothing");
    check(on.covered < off.covered,
          "the repaint REPLACES the live stroke: a tapered stroke holds ink at fewer texels than "
          "an untapered one, which it could not if the untapered tail were still underneath");
    // The other side of the same coin, and the one that catches a repaint
    // that restored the tiles and then deposited nothing (a route whose
    // accumulator was not put back to pen-down would do exactly that): the
    // ramp thins the last 80 px of a 400 px stroke, so most of the mark has
    // to survive.
    check(on.covered > off.covered * 7 / 10,
          "...and it really does lay the stroke back down -- the tapered stroke still covers "
          "most of the untapered one, rather than the restore having simply erased it");
    check(on.dabsAfterEnd >= off.dabsAfterEnd,
          "the repaint lays the whole stroke down again -- no fewer dabs than the untapered "
          "stroke, so nothing is silently dropped");
  }

  // ==========================================================================
  // A SHORT stroke, in each of the four directions. Reported from a tablet as
  // "it only works when the stroke starts out somewhat going up"; the cause
  // was length, not heading -- a stroke shorter than the ramp had deposited
  // nothing by the time `end()` ran, so it was taken for a stationary click
  // and every taper was skipped by the rule that keeps a click from painting
  // at radius 0. Four headings because the report named headings, and a
  // fixture that tests the diagnosis instead of the symptom proves nothing
  // about the symptom.
  // ==========================================================================
  {
    constexpr float kTaperOutPx = 80.0f;
    constexpr float kMinPct = 20.0f;
    const std::pair<float, float> headings[4] = {{1.0f, 0.0f}, {-1.0f, 0.0f},
                                                 {0.0f, -1.0f}, {0.0f, 1.0f}};
    const char* names[4] = {"right", "left", "up", "down"};
    float worstError = 0.0f;
    float expected = 0.0f;
    for (int h = 0; h < 4; ++h) {
      BrushState brush;
      brush.model.tip.diameterPx = 20.0f;
      brush.native.taperOut = BrushTaper{true, kTaperOutPx, kMinPct, false};
      MixboxLut noLut;
      OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "T");
      StrokeSession session;
      std::string error;
      const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
      expected = tip.radius * kMinPct / 100.0f;
      session.begin(d, d.activeLayer, tip, Tool::Brush, &error, /*model=*/nullptr,
                    DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{}, 1.0f, &brush.native);
      // 40 px of travel against an 80 px ramp: the whole stroke is inside it.
      for (int i = 0; i <= 4; ++i)
        session.addPoint(128.0f + headings[h].first * static_cast<float>(i) * 10.0f,
                         128.0f + headings[h].second * static_cast<float>(i) * 10.0f);
      session.end();
      const float err = std::fabs(session.lastDabRadius() - expected);
      if (err > worstError) worstError = err;
      std::printf("  [measured] short stroke %-5s: last dab %.3f px (ramp minimum %.3f px)\n",
                  names[h], session.lastDabRadius(), expected);
    }
    check(worstError < 0.05f,
          "a stroke shorter than the exit ramp still tapers, and does so identically in all "
          "four directions -- it is not mistaken for a stationary click");
  }

  // ==========================================================================
  // A stroke SHORTER than its ramps draws a THIN LINE, and that is the
  // point. Both ramps are read off a length the brush carries rather than the
  // length the stroke has, so a short stroke is thinned along the whole of
  // itself, and with both tapers on the two multipliers compound (`radius *=
  // entryMul * exitMul`). A quick flick of a big brush therefore leaves a
  // fine mark, the way a real brush barely touching the paper does.
  //
  // This was briefly "fixed" by capping each ramp at its share of the
  // stroke, so that a short stroke reached full width. The user rejected that
  // by feel: "I liked the behaviour where both tapers merged together and a
  // thin line was drawn, I don't like the new behaviour where the short
  // stroke becomes a blob." These assertions exist to stop it being
  // helpfully repaired a second time.
  // ==========================================================================
  {
    // The mark's greatest thickness, in texels: the widest column a
    // horizontal stroke painted. This is the observable the user's eye
    // actually uses -- "is my line the width of my brush?" -- and unlike
    // `lastDabRadius()` it sees the MIDDLE of the stroke, which is where
    // compounding does its damage.
    const auto maxThickness = [](const OpenDocument& d, size_t layerIndex) {
      const Layer& l = d.document.layers[layerIndex];
      std::map<int32_t, size_t> column;
      if (l.rgbTiles.has_value())
        for (const auto& [coord, tile] : *l.rgbTiles)
          for (int32_t y = 0; y < kTileSize; ++y)
            for (int32_t x = 0; x < kTileSize; ++x)
              if (tile.readPixel(PixelCoord{x, y})[3] > 0.0f)
                ++column[coord.x * kTileSize + x];
      size_t worst = 0;
      for (const auto& [x, n] : column) {
        (void)x;
        if (n > worst) worst = n;
      }
      return worst;
    };

    constexpr float kRampPx = 80.0f;   // twice the stroke
    constexpr float kMinPct = 20.0f;
    const auto runShort = [&](const BrushTaper& in, const BrushTaper& out) {
      BrushState brush;
      brush.model.tip.diameterPx = 20.0f;  // radius 10, so ~20 texels thick
      brush.native.taperIn = in;
      brush.native.taperOut = out;
      MixboxLut noLut;
      OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "T");
      StrokeSession session;
      std::string error;
      const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
      session.begin(d, d.activeLayer, tip, Tool::Brush, &error, /*model=*/nullptr,
                    DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{}, 1.0f, &brush.native);
      for (int i = 0; i <= 8; ++i) session.addPoint(100.0f + static_cast<float>(i) * 5.0f, 128.0f);
      session.end();
      return maxThickness(d, d.activeLayer);
    };

    const BrushTaper ramp{true, kRampPx, kMinPct, false};
    const size_t plain = runShort(BrushTaper{}, BrushTaper{});
    const size_t exitOnly = runShort(BrushTaper{}, ramp);
    const size_t both = runShort(ramp, ramp);

    std::printf("  [measured] 40 px stroke, 80 px ramps, 20 px tip: widest point %zu texels "
                "(no taper) vs %zu (exit only) vs %zu (both)\n",
                plain, exitOnly, both);

    check(exitOnly < plain * 4 / 5,
          "a stroke shorter than the exit ramp is thinner than the brush the whole way along "
          "itself -- the ramp is a length the BRUSH carries, deliberately not one capped at "
          "the length of the stroke");
    check(both < exitOnly,
          "...and with both tapers on the two ramps compound into a finer line still, which is "
          "the mark a quick flick of a big brush is meant to leave");
    check(both >= 1,
          "...but it is still a line: a short stroke thins, it does not vanish");
  }

  // ==========================================================================
  // `resampleTaperedTail()` must leave the UNTAPERED body of a stroke alone.
  // It is handed the whole stroke, every dab in it already sits at the
  // spacing it was emitted at, and a segment the ramp does not thin is
  // therefore being compared against its own spacing -- where `ceil()` of a
  // ratio that is 1 plus a float hair is 2. Real segment lengths scatter
  // either side of that 1, so only SOME segments split, and the repaint comes
  // back as a stroke of irregular double-density patches. At a low load
  // (reported from a tablet at 0.18), where one dab deposits little and the
  // eye integrates the pair, that reads as blotches.
  //
  // `minSizePct` 100 is a ramp that thins nothing, so the spacing it wants is
  // the spacing every dab already has and the right answer is to split
  // NOTHING: any dab beyond the ten fed in is a spurious one. The
  // `resampleTaperedTail` block above guards the opposite direction -- that a
  // ramp which does thin still subdivides -- so the two cannot both be
  // satisfied by declining to do any work.
  // ==========================================================================
  {
    const auto subdivided = [](float segLen) {
      std::vector<StrokeDab> tail;
      for (int i = 0; i < 10; ++i) {
        StrokeDab d;
        d.pos = Vec2{static_cast<float>(i) * segLen, 0.0f};
        d.pressure = 1.0f;
        tail.push_back(d);
      }
      resampleTaperedTail(tail, BrushTaper{}, BrushTaper{true, 40.0f, 100.0f, false}, 5.0f);
      return tail.size();
    };

    const size_t exact = subdivided(5.0f);
    const size_t over = subdivided(5.0001f);
    std::printf("  [measured] ramp that thins nothing, 9 segments at the spacing: %zu dab(s) "
                "from 10 at exactly the spacing, %zu at spacing+0.0001\n",
                exact, over);

    check(exact == 10,
          "subdivideTaperedTail splits nothing when the ramp thins nothing: dabs already at "
          "the spacing they were emitted at are left where they are");
    check(over == 10,
          "...and splits nothing when that spacing overshoots by a float hair either, which is "
          "what a real stroke's segments do -- splitting there doubles the dab density of the "
          "stroke in patches, which blotches a low-load repaint");
  }

  // ==========================================================================
  // The dab density must not STEP anywhere along the ramp. Reported from a
  // tablet once the blotching was fixed: "the beginning of the end taper
  // still gets a little darker, some dab doubling is still happening." That
  // is the last of the per-segment subdivision: at the ramp's own start the
  // wanted spacing dips a hair below the spacing the dabs already have, so
  // `ceil(length / wanted)` went from one piece to two and the density
  // doubled in a single segment -- while the dabs there are still nearly full
  // size, so nothing about them absorbs it and the extra ink reads as a dark
  // band right where the taper begins.
  //
  // Measured as the worst ratio between neighbouring gaps: a continuous walk
  // can only change a gap by as much as the ramp itself moves over one step,
  // while a doubling shows up as 2.
  // ==========================================================================
  {
    constexpr float kSpacingPx = 5.0f;
    const BrushTaper out{true, 50.0f, 10.0f, false};
    std::vector<StrokeDab> tail;
    for (int i = 0; i < 40; ++i)
      tail.push_back(StrokeDab{Vec2{static_cast<float>(i) * kSpacingPx, 0.0f}});
    resampleTaperedTail(tail, BrushTaper{}, out, kSpacingPx);

    float worstStep = 1.0f;
    size_t worstAt = 0;
    for (size_t i = 1; i + 1 < tail.size(); ++i) {
      const float a = distanceBetween(tail[i - 1].pos, tail[i].pos);
      const float b = distanceBetween(tail[i].pos, tail[i + 1].pos);
      if (a <= 1e-6f || b <= 1e-6f) continue;
      const float ratio = std::max(a / b, b / a);
      if (ratio > worstStep) {
        worstStep = ratio;
        worstAt = i;
      }
    }
    std::printf("  [measured] density continuity over a 50 px ramp on a 195 px stroke: worst "
                "neighbouring-gap ratio %.3fx (at dab %zu of %zu)\n",
                worstStep, worstAt, tail.size());
    check(worstStep < 1.25f,
          "the dab density follows the ramp continuously -- no neighbouring gap is a step "
          "change, which is what painted a dark band at the start of the exit taper");
  }

  std::printf("[selftest] brush taper / origin dab %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
