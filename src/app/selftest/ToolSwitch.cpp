#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/MeasureLine.hpp"
#include "app/ToolSwitch.hpp"

namespace np {

// ---------------------------------------------------------------------------
// app/ToolSwitch -- the previous tool (T20's spring-loaded Hand, T24's angle).
//
// **What this file can and cannot reach.** `--selftest` has no window, no
// ImGui frame and no modal, so it cannot press Space and it cannot open
// `Image > Transform...`. That limitation is the reason the module exists in
// the shape it does: the two decisions worth getting right -- "what is the
// previous tool" and "does the angle hand over" -- were lifted out of
// `ui/MacPaintUI.cpp`'s canvas block into free functions precisely so that
// they could be driven here in the same order the canvas block calls them in.
// What is left in the UI is the key read and the guards on it, and that
// residue is named in the report rather than pretended about.
//
// **The angle is never restated.** Section 5 asserts that the seed equals
// `measureReadout()`'s own `angleDeg`, not that it equals some degree value
// typed in here. `app/selftest/Measure.cpp` already pins that reading
// geometrically -- it feeds the angle to a `BrushTip` and asks `dabCoverage()`
// whether the footprint reaches a point on the measured line -- and repeating
// a number here would be a second, weaker copy of a fact that already has a
// strong owner. What this file owns is the *conditional*: whether the handoff
// happens at all, which is the half of T24 that is easy to drop and impossible
// to notice missing.
//
// **The enum is walked, not counted.** `ui/AtelierChrome.cpp`'s `kToolMeta` is
// indexed by `static_cast<size_t>(t)` behind a `static_assert` that checks the
// COUNT and not the order, and this build has been bitten by exactly that. So
// sections 1 and 5 iterate every value of `Tool` rather than picking three
// representative ones: "zero for every tool that is not Measure" is a claim
// about twenty-odd values, and asserting it for `Tool::Brush` alone would pass
// on an implementation that special-cased the Brush.
//
// Headless and GPU-free.
// ---------------------------------------------------------------------------

namespace {

// A ruler at a heading no axis and no diagonal produces, so that a seed which
// silently returned 0, 45, 90 or 180 could not be mistaken for a pass. The
// endpoints are deliberately not from the origin: `measureReadout()` works on
// the DIFFERENCE, and a line starting at (0,0) would pass on an implementation
// that read `x1`/`y1` alone.
MeasureLine rulerOn(uint64_t documentId) noexcept {
  MeasureLine line;
  beginMeasureLine(line, documentId, 40.0f, 25.0f);
  updateMeasureLine(line, 240.0f, 105.0f);
  endMeasureLine(line);
  return line;
}

}  // namespace

bool runToolSwitchTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  std::printf("[selftest] tool switch: the previous tool, the spring-loaded Hand, the seeded angle\n");

  // =========================================================================
  // 1. The setter is a ledger, and a non-switch does not move it
  // =========================================================================
  {
    AppState st;
    check(!hasPreviousTool(st),
          "toolswitch: a session that has never switched tools says so, rather "
          "than claiming the launch default as a previous tool");

    setActiveTool(st, Tool::Measure);
    check(st.brush.tool == Tool::Measure && hasPreviousTool(st) &&
              previousTool(st) == Tool::Brush,
          "toolswitch: the setter installs the new tool and records the outgoing "
          "one -- the fact four assignment sites each overwrote without reading");

    setActiveTool(st, Tool::Hand);
    check(st.brush.tool == Tool::Hand && previousTool(st) == Tool::Measure,
          "toolswitch: and the ledger follows, one switch behind");

    // The concrete loss the brief names: a Hand -> Hand pick must not lose the
    // real previous. Reachable three ways -- clicking the palette cell that is
    // already selected, choosing the flyout member already shown, and the
    // native menu's `MenuAction::ToolItem` for the current tool.
    setActiveTool(st, Tool::Hand);
    check(st.brush.tool == Tool::Hand && previousTool(st) == Tool::Measure,
          "toolswitch: picking the tool that is ALREADY active is not a switch -- "
          "a Hand -> Hand pick must not overwrite the previous tool with itself");

    // Walked, not sampled: the guard above is one comparison, and a version of
    // it that special-cased a single value would pass a three-tool spot check.
    bool sameToolHeldEverywhere = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      AppState w;
      setActiveTool(w, Tool::Eyedropper);  // a known previous to protect
      setActiveTool(w, t);
      const Tool expectedPrev = (t == Tool::Eyedropper) ? Tool::Brush : Tool::Eyedropper;
      setActiveTool(w, t);  // the non-switch
      if (w.brush.tool != t || previousTool(w) != expectedPrev) sameToolHeldEverywhere = false;
    }
    check(sameToolHeldEverywhere,
          "toolswitch: for EVERY value of Tool, re-picking it leaves the ledger "
          "where it was (the enum walked -- kToolMeta's static_assert checks the "
          "count, not the order, and this build has been bitten by that)");
  }

  // =========================================================================
  // 2. The spring-loaded Hand borrows, and leaves no trace
  // =========================================================================
  //
  // T20. The whole feature is that it leaves nothing behind: a Space press
  // that recorded "previous = Hand" and then restored the Hand on release
  // would erase the very fact the ledger exists to keep, and it would do it
  // silently, one pan at a time.
  {
    AppState st;
    setActiveTool(st, Tool::Eyedropper);
    setActiveTool(st, Tool::Brush);
    const Tool ledgerBefore = previousTool(st);

    check(!springHandHeld(st) && effectiveTool(st) == Tool::Brush,
          "toolswitch: with Space up there is no borrow, and the effective tool "
          "is simply the selected one");

    check(beginSpringHand(st) && st.brush.tool == Tool::Hand,
          "toolswitch: Space down borrows the Hand -- `brush.tool` really becomes "
          "it, so `toolPansView()` and the cursor and the palette highlight all "
          "follow without a second opinion about which tool is live");
    check(effectiveTool(st) == Tool::Brush,
          "toolswitch: ...while the tool the USER is in stays what they picked");
    check(previousTool(st) == ledgerBefore && hasPreviousTool(st),
          "toolswitch: and the borrow writes NO ledger entry -- recording "
          "'previous = Hand' here is what would make the feature erase itself");

    // Key auto-repeat, and the focus-regained-with-the-key-down case.
    check(!beginSpringHand(st) && st.brush.tool == Tool::Hand &&
              effectiveTool(st) == Tool::Brush,
          "toolswitch: a repeat press is refused rather than re-borrowing -- a "
          "second borrow would owe back the Hand and strand the user in it");

    check(endSpringHand(st) && st.brush.tool == Tool::Brush,
          "toolswitch: Space up gives back exactly the tool that was held");
    check(previousTool(st) == ledgerBefore && effectiveTool(st) == Tool::Brush,
          "toolswitch: and the whole borrow is invisible to the ledger afterwards");

    // "with no previous tool ever set": a release with no press must be a
    // no-op, not an install of whatever `springReturn` happens to hold.
    AppState fresh;
    setActiveTool(fresh, Tool::Pencil);
    check(!endSpringHand(fresh) && fresh.brush.tool == Tool::Pencil,
          "toolswitch: a Space release with no press behind it changes nothing -- "
          "it must not install a stale return tool, and must not leave the Hand");
  }

  // =========================================================================
  // 3. A deliberate pick during a borrow wins, and is recorded as the user's
  // =========================================================================
  //
  // Reachable: hold Space to drag the canvas into view, then click a palette
  // cell with the other hand before letting go.
  {
    AppState st;
    setActiveTool(st, Tool::Brush);
    beginSpringHand(st);
    setActiveTool(st, Tool::Pencil);
    check(!springHandHeld(st) && st.brush.tool == Tool::Pencil,
          "toolswitch: a deliberate pick made while Space is held ends the borrow "
          "and stands -- restoring the borrowed-from tool on release would throw "
          "away a choice the user made on purpose");
    check(previousTool(st) == Tool::Brush,
          "toolswitch: ...and the tool recorded as previous is the one the USER "
          "was in, not the Hand that happened to be installed at that instant");
    check(!endSpringHand(st) && st.brush.tool == Tool::Pencil,
          "toolswitch: the later Space release is then a no-op over that pick");
  }

  // =========================================================================
  // 4. A borrow is application state, and survives a document switch
  // =========================================================================
  //
  // The tool is not a property of a document -- tabbing between two open
  // pictures does not change which tool is selected, and must not change what
  // a held Space owes back either.
  {
    AppState st;
    setActiveTool(st, Tool::Smudge);
    st.documents.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "A"));
    st.documents.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "B"));
    st.documents.setActive(0);
    beginSpringHand(st);
    st.documents.setActive(1);
    check(springHandHeld(st) && st.brush.tool == Tool::Hand &&
              effectiveTool(st) == Tool::Smudge,
          "toolswitch: a held Space survives tabbing to another document -- the "
          "tool is application state, not a property of the picture");
    check(endSpringHand(st) && st.brush.tool == Tool::Smudge,
          "toolswitch: and the release still gives back the tool the borrow took, "
          "on whichever document is now in front");
  }

  // =========================================================================
  // 5. T24 -- the angle handoff, BOTH directions
  // =========================================================================
  //
  // "the angle from the measure is put into the transform angle field, if it
  // wasn't the last tool the angle should be zero." The second clause is the
  // whole feature as much as the first, and is the half that shows up as a
  // transform silently pre-rotated by a ruler the user forgot they dragged.
  {
    constexpr uint64_t kDocA = 11u;
    constexpr uint64_t kDocB = 12u;

    AppState st;
    setActiveTool(st, Tool::Measure);
    st.measure = rulerOn(kDocA);
    const float measured = measureReadout(st.measure).angleDeg;

    check(measured != 0.0f,
          "toolswitch: the fixture ruler is at a heading no axis produces, so a "
          "seed that always answered zero could not pass section 5 by accident");
    check(transformSeedAngleDeg(st, kDocA) == measured,
          "toolswitch: with Measure selected and the ruler on THIS document, the "
          "transform dialog opens at measureReadout()'s own angle -- forwarded, "
          "not recomputed (there is one vector-to-heading function in this build)");

    check(transformSeedAngleDeg(st, kDocB) == 0.0f,
          "toolswitch: a ruler measured on another document seeds exactly zero -- "
          "its texels are that document's texels, and a plausible wrong angle is "
          "worse than none (app/MeasureLine.hpp section 1)");

    // The borrow again, and the one place the previous-tool machinery genuinely
    // earns its keep in T24: pan the canvas to see the far end of your own
    // ruler, then open the dialog.
    beginSpringHand(st);
    check(transformSeedAngleDeg(st, kDocA) == measured,
          "toolswitch: the handoff survives a Space-pan -- it asks which tool the "
          "user is IN, so panning to look at the ruler does not lose it");
    endSpringHand(st);

    // Every other tool, walked. This is the "if it wasn't the last tool" half.
    bool zeroEverywhereElse = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (t == Tool::Measure) continue;
      AppState w;
      setActiveTool(w, t);
      w.measure = rulerOn(kDocA);  // the ruler is still there; the tool is not
      if (transformSeedAngleDeg(w, kDocA) != 0.0f) zeroEverywhereElse = false;
    }
    check(zeroEverywhereElse,
          "toolswitch: EVERY tool that is not Measure seeds exactly zero, even "
          "with a live ruler in AppState (the enum walked, not sampled)");

    // A recorded decision, asserted so that it is a decision and not a drift:
    // Measure as the PREVIOUS tool seeds nothing. `ui/MacPaintUI.cpp`'s Measure
    // handler clears the ruler on every frame Measure is not active, so a
    // `previous == Measure` predicate would fire on no reachable state at all --
    // a feature that looks shipped and never runs.
    AppState after;
    setActiveTool(after, Tool::Measure);
    setActiveTool(after, Tool::Brush);
    after.measure = rulerOn(kDocA);
    check(previousTool(after) == Tool::Measure && transformSeedAngleDeg(after, kDocA) == 0.0f,
          "toolswitch: Measure as the PREVIOUS tool seeds zero, on purpose -- the "
          "ruler is destroyed the frame the tool changes, so keying the handoff "
          "off it would be a branch no running state can reach");

    // No ruler, with Measure selected: the tool is right and there is still
    // nothing to hand over.
    //
    // **A default-constructed `MeasureLine` is the wrong fixture for this and
    // was the first thing written here.** Its four endpoints are zero, so
    // `measureReadout()` reports 0 degrees for it by IEEE contract
    // (`app/MeasureLine.hpp` section 4) -- the assertion passed with the
    // `measureLineAppliesTo()` gate deleted, which means it was not watching
    // the gate at all, only restating `atan2(0, 0) == 0`. Sabotage caught it.
    //
    // The state the running application actually produces is a ruler that was
    // dragged and then thrown away: `clearMeasureLine()` sets `active` false
    // and deliberately leaves the coordinates alone, so a seed missing its
    // gate would hand over the stale heading of a line that is no longer on
    // screen -- the plausible wrong number this whole conditional exists to
    // prevent.
    AppState cleared;
    setActiveTool(cleared, Tool::Measure);
    cleared.measure = rulerOn(kDocA);
    clearMeasureLine(cleared.measure);
    check(measureReadout(cleared.measure).angleDeg == measured,
          "toolswitch: a thrown-away ruler keeps its coordinates, so this fixture "
          "really can hand a stale angle to a seed that forgot to check");
    check(transformSeedAngleDeg(cleared, kDocA) == 0.0f,
          "toolswitch: ...and a ruler the user has already dismissed seeds exactly "
          "zero, rather than the heading it had when it was last on screen");
  }

  std::printf("[selftest] tool switch %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
