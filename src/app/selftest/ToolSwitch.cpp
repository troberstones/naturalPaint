#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/MeasureLine.hpp"
#include "app/ToolSwitch.hpp"
#include "core/LayerOps.hpp"

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

  // ========================================================================
  // The flatting tools: a second sticky mode over the same canvas clicks
  // ========================================================================
  //
  // These exist because a palette button cannot use the key path (the docks
  // draw before the canvas hit-test, so the pointer is never over a texel when
  // a button is clicked). That makes `flatsTool` a second answer to "what does
  // a click mean", and this block pins the rule that keeps it from becoming an
  // ambiguous one.
  {
    AppState st;
    setFlatsTool(st, FlatsTool::DeleteFill);
    check(st.flatsTool == FlatsTool::DeleteFill, "toolswitch: a flatting tool can be picked");

    // **The rule.** Without this, a user picks the Brush in TOOLS, drags on a
    // Flats layer, and gets a bridge stroke because DELETE was still lit in a
    // palette they may not even have on screen.
    setActiveTool(st, Tool::Brush);
    check(st.flatsTool == FlatsTool::None,
          "toolswitch: **picking an ordinary tool leaves flatting mode** -- one deliberate "
          "pick, one answer to what a canvas click means");

    // Re-picking the tool you already hold is exactly how a user says "stop
    // doing the other thing", so the clear must not be behind the
    // `next == outgoing` early return.
    setFlatsTool(st, FlatsTool::BridgePen);
    setActiveTool(st, Tool::Brush);
    check(st.flatsTool == FlatsTool::None,
          "toolswitch: ...including re-picking the tool already held, which is ahead of the "
          "no-op early return rather than behind it");

    // **NO flatting tool touches the regular toolbox. Every one of the nine.**
    //
    // This assertion is the reverse of the one it replaces. `setFlatsTool()`
    // used to install a host tool from ADR-0009's table -- BRIDGE set the
    // Pencil, GROUP set the Lasso -- and the test pinned that table. In use
    // it was wrong and the user said so: one click lit a cell in each of two
    // palettes, and leaving flatting mode then handed back a tool nobody had
    // chosen.
    //
    // Asserted over the WHOLE enum rather than the five that used to have
    // hosts, and from several different starting tools, because "leaves it
    // alone" is only worth anything if it holds for the tool the user
    // actually had. A regression here is silent: the flats gesture still
    // works, the palette merely lies about what else it did.
    bool independent = true;
    for (const Tool start : {Tool::Move, Tool::Brush, Tool::Lasso, Tool::PaintBucket}) {
      for (int v = 0; v < static_cast<int>(FlatsTool::SelectEdits) + 1; ++v) {
        AppState h;
        setActiveTool(h, start);
        const Tool before = h.brush.tool;
        setFlatsTool(h, static_cast<FlatsTool>(v));
        if (h.brush.tool != before) independent = false;
        if (h.flatsTool != static_cast<FlatsTool>(v)) independent = false;
      }
    }
    check(independent,
          "toolswitch: **picking a flatting tool never overwrites `brush.tool`** -- the regular "
          "tool is REMEMBERED (it stops being active, see the exclusivity block below, but it is "
          "not destroyed), over every FlatsTool and from four different starting tools");

    // The other half of the same rule, and the reason the above is SAFE.
    // GROUP and SHAPE used to reach their gesture through `Tool::Lasso`:
    // `flatsLassoCommit()` was an interception inside `case Tool::Lasso:` and
    // ran only while that tool was active, so installing it was load-bearing.
    // The flats canvas route owns the lasso path now
    // (`ui/MacPaintUI.cpp`'s `flatsToolOwnsCanvasNow()`), which is why
    // dropping the host tool does not silently kill those two. Nothing
    // headless can reach an ImGui frame to prove that, so what is pinned here
    // is the invariant it rests on: the two lasso tools are ordinary members
    // of the enum with no tool requirement of their own.
    AppState g;
    setActiveTool(g, Tool::Brush);
    setFlatsTool(g, FlatsTool::Group);
    check(g.flatsTool == FlatsTool::Group && g.brush.tool == Tool::Brush,
          "toolswitch: GROUP is picked with the Brush still active -- its gesture no longer "
          "depends on the Lasso being installed behind the user's back");

    // ---- the tool state is EXCLUSIVE ------------------------------------
    //
    // Two palettes each draw a selection, and the user's own words for the
    // defect were "the tool state should be exclusive": activating a flats
    // tool has to DEACTIVATE the regular one. `flatsToolIsActive()` is what
    // both palettes read to decide who is lit, so it is asserted here rather
    // than left to a screenshot -- nothing headless can see an ImGui cell,
    // but the predicate the cell's `selected` is ANDed with is ordinary
    // testable state.
    //
    // Note what is deliberately NOT asserted: that `brush.tool` was cleared.
    // It is remembered on purpose, so leaving flatting mode gives back the
    // tool the user had. "Exclusive" is about which one is ACTIVE, not about
    // destroying the other.
    {
      AppState e;
      OpenDocument od;
      od.document = Document::createBlank(8, 8, WorkingSpace{});
      addLayer(od.document, 1, makeFlatsLayer("Flats"));
      od.activeLayer = 1;
      e.documents.add(std::move(od));

      setActiveTool(e, Tool::Brush);
      check(!flatsToolIsActive(e),
            "toolswitch: with no flatting tool picked the REGULAR tool is the active one, even "
            "on a Flats layer");

      setFlatsTool(e, FlatsTool::DeleteFill);
      check(flatsToolIsActive(e) && e.brush.tool == Tool::Brush,
            "toolswitch: **picking a flatting tool makes it the active tool and the regular "
            "palette draws nothing selected** -- while `brush.tool` is still remembered, not "
            "cleared, so leaving flatting mode gives it back");

      // The other direction, which is what keeps "exactly one lit" true
      // rather than producing a moment with neither: on a layer the flats
      // tool cannot act on, the palette greys itself out and the regular
      // tool really is the active one again.
      e.documents.active()->activeLayer = 0;
      check(!flatsToolIsActive(e) && e.flatsTool == FlatsTool::DeleteFill,
            "toolswitch: with a non-Flats layer selected the regular tool is active again -- the "
            "flats tool stays PICKED but stops counting, which is what the greyed palette "
            "already shows");

      e.documents.active()->activeLayer = 1;
      e.documents.active()->document.layers[1].locked = true;
      check(!flatsToolIsActive(e),
            "toolswitch: ...and a LOCKED Flats layer is the same case -- the flats tool cannot "
            "act, so it is not what a click means");
      e.documents.active()->document.layers[1].locked = false;

      // And a deliberate regular pick ends it outright, so the exclusivity
      // cannot get stuck with both sides believing they are active.
      setActiveTool(e, Tool::Lasso);
      check(!flatsToolIsActive(e) && e.flatsTool == FlatsTool::None && e.brush.tool == Tool::Lasso,
            "toolswitch: picking a regular tool clears the flats tool outright, so the two can "
            "never both consider themselves active");
    }

    // A half-finished two-click merge belongs to the gesture being abandoned.
    AppState m;
    m.flatsMergeFirst = std::array<float, 2>{4.0f, 5.0f};
    setFlatsTool(m, FlatsTool::DeleteFill);
    check(!m.flatsMergeFirst.has_value(),
          "toolswitch: picking a flatting tool cancels a half-armed merge, rather than "
          "carrying its first point into the next gesture");
  }

  std::printf("[selftest] tool switch %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
