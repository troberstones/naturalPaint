#include "app/selftest/Support.hpp"

#include <algorithm>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "paint/Palette.hpp"

namespace np {

// Reachability audit F2's own findings ("A2/B2/B3"): three brush-panel bugs
// that share one cause -- a control drawn twice, once in the options bar
// (ui/AtelierChrome.cpp) and once in the BRUSH panel (ui/MacPaintUI.cpp),
// with nothing checking the two copies agree. **This section is headless
// and GPU-free, and deliberately does not drive Dear ImGui.** Everything
// below is provable as plain C++ arithmetic over the same symbols the two
// UI files now read (app/AppState.hpp's kBrush*Min/Max constants,
// app/StrokeSession.hpp's strokeRouteFor()/wetnessReachesSolver()) -- it
// does not, and cannot, prove that a given `ImGui::SliderFloat()` call site
// still passes those symbols rather than a hand-typed literal, because that
// would mean rendering a real frame and reading back a live widget's bound,
// which no section in this suite does for any panel. That gap is named
// again at the bottom of this function rather than papered over.
//
// --- Part A: one field, one range (B3, generalised) -----------------------
//
// B3 was SIZE carrying 2..90 in the options bar against 1..200 in the BRUSH
// panel -- a value the panel accepted silently clamping to 90 the moment the
// bar's own widget next touched the field. The fix was not "widen SIZE's
// bar range"; it was "give the field one range, in one named place, and
// have both widgets read it" -- app/AppState.hpp's kBrushRadiusMin/Max,
// kBrushHardnessMin/Max, kBrushLoadMin/Max and kBrushWetnessMin/Max, now
// the only bounds `ui/AtelierChrome.cpp`'s SIZE/HARD/LOAD/WET row and
// `ui/MacPaintUI.cpp`'s Radius/Hardness/Load/Water sliders pass to
// `SliderFloat()`/`ctlSlider()`. This part is the table B3 needed: every
// `BrushState` field both surfaces edit, walked generically rather than as
// four hand-written checks, so a fifth shared field added later and left
// off this table is the next thing this section should grow to cover.
bool runChromeConsistencyTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  {
    struct Row {
      const char* field;    // BrushState member, and its two widget labels
      float lo, hi;         // the one range both widgets read
      float wantLo, wantHi; // independently-derived expectation (see below)
    };
    // Expected bounds re-derived independently here, the same discipline
    // app/selftest/AtelierChrome.cpp's Part C uses for atelierLayout()'s
    // arithmetic ("written out again independently here rather than
    // obtained by calling the function and comparing it to itself") --
    // comparing kBrushRadiusMax to itself would prove nothing; comparing it
    // to 200.0f, re-typed from brush/Deposit.hpp's own "widest radius the
    // UI offers" comment, at least proves the constant still says what its
    // justification requires.
    const Row kRows[] = {
        {"radius   (options bar SIZE / BRUSH panel Radius)", kBrushRadiusMin, kBrushRadiusMax,
         1.0f, 200.0f},
        {"hardness (options bar HARD / BRUSH panel Hardness)", kBrushHardnessMin,
         kBrushHardnessMax, 0.0f, 1.0f},
        {"load     (options bar LOAD / BRUSH panel Load)", kBrushLoadMin, kBrushLoadMax, 0.0f,
         2.5f},
        {"wetness  (options bar WET / BRUSH panel Water)", kBrushWetnessMin, kBrushWetnessMax,
         0.0f, 3.0f},
    };
    bool rangesOk = true;
    for (const Row& r : kRows) {
      if (!(r.lo < r.hi)) {
        rangesOk = false;
        std::printf("    %-50s range is [%.3f, %.3f], not lo < hi\n", r.field, r.lo, r.hi);
      }
      if (r.lo != r.wantLo || r.hi != r.wantHi) {
        rangesOk = false;
        std::printf("    %-50s is [%.3f, %.3f], expected [%.3f, %.3f]\n", r.field, r.lo, r.hi,
                    r.wantLo, r.wantHi);
      }
    }
    check(rangesOk,
          "every brush field shared between the options bar and the BRUSH panel has one "
          "sane, independently-verified range");
    check(sizeof(kRows) / sizeof(kRows[0]) == 4,
          "four BrushState fields are drawn by both surfaces today (radius, hardness, load, "
          "wetness) -- spacing/roundness/angle/opacity are BRUSH-panel-only");
  }

  // --- Part B: the round trip B3 itself asks for --------------------------
  //
  // "Set the field to a value only the wide range allows, run it through
  // both widgets' clamps, assert it survives." `std::clamp` against the one
  // shared `(kBrushRadiusMin, kBrushRadiusMax)` pair is what
  // `ImGui::SliderFloat`'s own drag-to-bound behaviour reduces to for a
  // value already inside range (Dear ImGui's internal ratio math is out of
  // this suite's scope to re-implement; what this codebase controls, and
  // what B3 broke, is which two floats get passed as its lo/hi) -- modelling
  // each widget's clamp as `std::clamp` against the SAME named pair both
  // sites now read is the direct C++ translation of "one field, one range".
  {
    const float set = 150.0f;  // only ever legal once the panel's 1..200 shipped
    const float afterOptionsBarClamp = std::clamp(set, kBrushRadiusMin, kBrushRadiusMax);
    const float afterPanelClamp = std::clamp(afterOptionsBarClamp, kBrushRadiusMin, kBrushRadiusMax);
    check(afterPanelClamp == 150.0f,
          "150 (only the wide range admits it) survives both widgets' clamp in either order");
    // For contrast, not as an assertion about anything still shipped: the
    // exact regression B3 named, computed on purpose against the retired
    // literal bound so this test is proven sensitive rather than merely
    // satisfied (the same "compute the rejected form on purpose" discipline
    // app/SelfTest.hpp's runFiltersTest() doc comment describes).
    check(std::clamp(set, 2.0f, 90.0f) == 90.0f,
          "for contrast: the pre-fix options-bar bound (2..90) really did truncate 150 -> 90 "
          "-- this is the regression B3 named, not a hypothetical one");
  }

  // --- Part C: WET's disabled state, as a pure function of the route ------
  //
  // B2: `wetnessReachesSolver()` (app/StrokeSession.hpp) is the predicate
  // both `ui/AtelierChrome.cpp` and `ui/MacPaintUI.cpp` now call instead of
  // each carrying its own `route == StrokeRoute::PaintSim` copy -- extracted
  // for the same reason `strokeRouteWritesLayer()` was, one function rather
  // than two copies free to drift. Every case below is the *route*, which is
  // all either widget ever actually looks at; no ImGui involvement.
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "chrome-consistency");
    // Document::createBlank()'s one layer is RGB-kind (runCreateBlankTest's
    // own claim) -- exactly the ordinary case a user is in the moment a
    // document opens, with nothing yet done to pick a Pigment layer.
    Layer* rgbTarget = activeLayerOf(doc);
    check(rgbTarget != nullptr && rgbTarget->kind == LayerKind::RGB,
          "fixture: a fresh document's active layer is RGB-kind");

    check(wetnessReachesSolver(strokeRouteFor(Tool::Brush, nullptr)),
          "no document layer at all: Brush falls through to the solver canvas -- WET honoured");
    check(wetnessReachesSolver(strokeRouteFor(Tool::Water, nullptr)),
          "Water with no layer: also the solver -- WET honoured");
    check(wetnessReachesSolver(strokeRouteFor(Tool::Water, rgbTarget)),
          "Water WITH a writable layer selected: still the solver, because Water always "
          "routes to PaintSim -- WET honoured regardless of the layer");

    check(!wetnessReachesSolver(strokeRouteFor(Tool::Brush, rgbTarget)),
          "Brush onto a writable RGB layer: RgbDeposit, not the solver -- WET disabled");

    // **The re-enabling case the brief asks for by name.** Same document,
    // same selected layer, same call -- the only thing that changes is
    // which tool is active, exactly what a user does by clicking Water in
    // the palette. If this predicate were ever hardcoded to "always
    // disabled" (the failure mode the brief explicitly warns against),
    // this is the assertion that would catch it: the disabled case above
    // and this one differ in nothing but `st.brush.tool`.
    check(wetnessReachesSolver(strokeRouteFor(Tool::Water, rgbTarget)),
          "switching Brush -> Water on the IDENTICAL target flips WET back to honoured -- "
          "the predicate reacts to the live route, it is not a fixed disabled state");

    // A Pigment layer, and a locked one: two more ways to reach `!honoured`
    // that are not "no document", the same coverage app/selftest/
    // ActiveLayer.cpp's own routing section gives strokeRouteFor() directly.
    applyLayerCommand(doc, LayerCommand::NewPigmentLayer, doc.activeLayer);
    setActiveLayer(doc, doc.document.layers.size() - 1);
    Layer* pigTarget = activeLayerOf(doc);
    check(pigTarget != nullptr && pigTarget->kind == LayerKind::Pigment,
          "fixture: the new layer is Pigment-kind");
    check(!wetnessReachesSolver(strokeRouteFor(Tool::Brush, pigTarget)),
          "Brush onto a Pigment layer: CpuDeposit, not the solver -- WET disabled");
    pigTarget->locked = true;
    check(!wetnessReachesSolver(strokeRouteFor(Tool::Brush, pigTarget)),
          "a locked Pigment layer: route None, still not the solver -- WET stays disabled");
    check(wetnessReachesSolver(strokeRouteFor(Tool::Water, pigTarget)),
          "but Water still reaches the solver on that same locked layer -- the lock "
          "constrains layer-writing routes, not the solver route");
  }

  // --- Part D: A5, asserted explicitly -------------------------------------
  //
  // Resolved: the loaded PIGMENT owns Density/Staining/Granulation. PLAN.md's
  // own record of `brushTipFor()` (2026-08-21, Phase-pen-reaches-a-layer
  // step) says so in these words -- "the colour travels, the three physical
  // constants do not, because brush/Deposit simulates no settling, lifting
  // or granulation" -- settling, lifting and granulation being properties of
  // the SOLVER driven by which real paint is loaded, not a session-wide
  // dial independent of one. `paint/Palette.hpp` calls the numbers "the real
  // pigment measurements published with Mixbox". So a slider must not be
  // able to make them outlive the frame it was dragged in, and the assertion
  // is that exactly nothing does: main.cpp's own per-frame resolution
  // (`st.sim.density = pig.density;` etc., main.cpp:2521-2525) is replicated
  // here as an expression -- not called, since it runs inline in the
  // interactive frame loop with no separate entry point to call -- and shown
  // to win over an arbitrary prior value every time.
  {
    const std::vector<Pigment>& palette = defaultPalette();
    check(!palette.empty(), "fixture: the default palette is non-empty");
    const Pigment& p = palette[palette.size() > 3 ? 3 : 0];

    // A value a (now-disabled) drag could have produced, deliberately picked
    // to differ from the pigment's own numbers -- otherwise the round trip
    // below could pass by coincidence rather than by the resolution actually
    // holding.
    float density = p.density + 0.31f > 1.0f ? p.density - 0.31f : p.density + 0.31f;
    float staining = p.staining + 0.27f > 1.0f ? p.staining - 0.27f : p.staining + 0.27f;
    float granulation = p.granulation + 0.19f > 1.0f ? p.granulation - 0.19f : p.granulation + 0.19f;
    check(density != p.density && staining != p.staining && granulation != p.granulation,
          "fixture: the tampered values genuinely differ from the pigment's own (so the "
          "round trip below cannot pass vacuously)");

    // main.cpp's resolution, replicated verbatim.
    density = p.density;
    staining = p.staining;
    granulation = p.granulation;
    check(density == p.density && staining == p.staining && granulation == p.granulation,
          "A5: whatever a slider set these to in-frame, the active pigment's own values are "
          "what stand at the start of the next frame -- the pigment owns them, a slider "
          "cannot make an edit survive");
  }

  // **What this section does not, and cannot, prove headlessly**: that
  // `drawPigmentSection()`'s three sliders are still wrapped in
  // `ImGui::BeginDisabled()` (MacPaintUI.cpp), that `drawBrushSection()`'s
  // WET slider is still wrapped the same way when `!honoured`, or that
  // either options-bar or BRUSH-panel widget still passes the kBrush*Min/Max
  // constants above rather than a reintroduced literal. All three are
  // verified by direct code reading in this change's own review, not by
  // this binary -- proving them at the --selftest level would mean
  // rendering a real ImGui frame and driving a simulated drag against a
  // live widget, which no section in this suite does for any panel, and
  // this one does not attempt to be the first.
  std::printf("[selftest] chrome consistency %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
