#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/MeasureLine.hpp"
#include "app/ToolSwitch.hpp"
#include "app/TransformSession.hpp"
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

    // **A recorded-edit selection dies with the tool that made it, from BOTH
    // writers.** A `flatEditKey()` means nothing except against the edit list
    // it was picked from, and the canvas overlay brightens whatever is in
    // this set -- so a selection left standing while a different tool is
    // armed is a highlight that says "Delete will remove these" when Delete
    // no longer will. Asserted over every FlatsTool, including re-picking
    // SELECT EDITS itself, because "clears on a change" is not the rule:
    // clears on a PICK is.
    bool selectionDies = true;
    for (int v = 0; v <= static_cast<int>(FlatsTool::SelectEdits); ++v) {
      AppState h;
      setFlatsTool(h, FlatsTool::SelectEdits);
      h.flatsEditSelection = {flatEditKey(FlatEditRef{4, 1}), flatEditKey(FlatEditRef{6, 2})};
      h.flatsEditBox = std::array<float, 4>{1, 2, 3, 4};
      h.flatsEditBoxAdditive = true;
      setFlatsTool(h, static_cast<FlatsTool>(v));
      if (!h.flatsEditSelection.empty() || h.flatsEditBox.has_value() || h.flatsEditBoxAdditive)
        selectionDies = false;

      AppState o;
      setFlatsTool(o, FlatsTool::SelectEdits);
      o.flatsEditSelection = {flatEditKey(FlatEditRef{4, 1})};
      o.flatsEditBox = std::array<float, 4>{1, 2, 3, 4};
      setActiveTool(o, Tool::Brush);
      if (!o.flatsEditSelection.empty() || o.flatsEditBox.has_value()) selectionDies = false;
    }
    check(selectionDies,
          "toolswitch: **picking any tool drops the selected recorded edits** -- through "
          "setFlatsTool for every FlatsTool including SELECT EDITS itself, and through "
          "setActiveTool; a key outlives the list it indexes and the overlay would go on "
          "promising a Delete that no longer reaches them");

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
    // The user's words, twice: "the tool state should be exclusive." Two
    // palettes each draw a selection, and `flatsToolIsActive()` is what both
    // read to decide who is lit, so it is asserted here -- nothing headless
    // can see an ImGui cell, but the predicate the cell's `selected` is
    // ANDed with is ordinary testable state.
    //
    // **This assertion was inverted once, and the first version was wrong in
    // a way worth keeping written down.** It used to require a Flats layer to
    // be selected before the flatting tool counted as active, on the
    // reasoning that a tool which cannot act should not claim to be. That
    // produced two live tools rather than none: with DELETE picked and an
    // ordinary layer selected, TOOLS re-lit its cell, the flats palette went
    // on showing DELETE accented, and the brush really did still paint. It is
    // also why DELETE looked broken -- the flats route stood down whenever
    // the layer was wrong, so a click on a fill went nowhere.
    //
    // So the PICK decides. Whether it can act on this layer is a separate
    // question, answered on the click with a refusal.
    {
      AppState e;
      OpenDocument od;
      od.document = Document::createBlank(8, 8, WorkingSpace{});
      addLayer(od.document, 1, makeFlatsLayer("Flats"));
      od.activeLayer = 1;
      e.documents.add(std::move(od));

      setActiveTool(e, Tool::Brush);
      check(!flatsToolIsActive(e),
            "toolswitch: with no flatting tool picked the REGULAR tool is the active one");

      setFlatsTool(e, FlatsTool::DeleteFill);
      check(flatsToolIsActive(e) && e.brush.tool == Tool::Brush,
            "toolswitch: **picking a flatting tool makes it the active tool and the regular "
            "palette draws nothing selected** -- while `brush.tool` is still remembered, not "
            "cleared, so leaving flatting mode gives it back");

      // The layer does NOT decide. Every one of these used to flip the answer
      // back to the regular tool, which is the defect above.
      e.documents.active()->activeLayer = 0;  // an ordinary layer
      check(flatsToolIsActive(e),
            "toolswitch: **selecting a non-Flats layer does NOT re-activate the regular tool** -- "
            "the flatting tool stays active and the click is refused with a sentence, rather than "
            "the canvas being handed back to a tool the user did not pick");

      e.documents.active()->activeLayer = 1;
      e.documents.active()->document.layers[1].locked = true;
      check(flatsToolIsActive(e),
            "toolswitch: ...and a LOCKED Flats layer is the same -- still active, still refused "
            "on the click");
      e.documents.active()->document.layers[1].locked = false;

      // One deliberate pick, one active tool: the only way out of flatting
      // mode is choosing a tool, which is what makes the state unambiguous.
      setActiveTool(e, Tool::Lasso);
      check(!flatsToolIsActive(e) && e.flatsTool == FlatsTool::None && e.brush.tool == Tool::Lasso,
            "toolswitch: picking a regular tool clears the flats tool outright, so the two can "
            "never both consider themselves active");

      // The whole point, stated as one assertion over the whole enum: for
      // every flatting tool, exactly one of the two is active -- never both,
      // never neither.
      bool exactlyOne = true;
      for (int v = 1; v <= static_cast<int>(FlatsTool::SelectEdits); ++v) {
        AppState x;
        OpenDocument xd;
        xd.document = Document::createBlank(8, 8, WorkingSpace{});
        x.documents.add(std::move(xd));
        setActiveTool(x, Tool::Brush);
        if (flatsToolIsActive(x)) exactlyOne = false;          // regular active
        setFlatsTool(x, static_cast<FlatsTool>(v));
        if (!flatsToolIsActive(x)) exactlyOne = false;          // flats active
        setActiveTool(x, Tool::Brush);
        if (flatsToolIsActive(x) || x.flatsTool != FlatsTool::None) exactlyOne = false;
      }
      check(exactlyOne,
            "toolswitch: for EVERY flatting tool, exactly one of the two palettes is active at a "
            "time -- and it holds with no Flats layer in the document at all, which is the case "
            "the layer-gated version got wrong");
    }

    // A half-finished two-click merge belongs to the gesture being abandoned.
    AppState m;
    m.flatsMergeFirst = std::array<float, 2>{4.0f, 5.0f};
    setFlatsTool(m, FlatsTool::DeleteFill);
    check(!m.flatsMergeFirst.has_value(),
          "toolswitch: picking a flatting tool cancels a half-armed merge, rather than "
          "carrying its first point into the next gesture");
  }

  // ==========================================================================
  // 7. The tool a live transform session installs.
  // ==========================================================================
  //
  // A transform session puts a gizmo on the canvas and does NOT disable the
  // active tool's own click handling, so whatever gesture the user was in the
  // middle of is still armed underneath it. With the Text tool that gesture
  // CREATES A LAYER: press Cmd+T while editing a caption, click anywhere off
  // the block, and a second text layer appeared behind the gizmo. Measured in
  // the running app before this existed -- four layers before the stray
  // click, five after.
  {
    std::printf("  -- 7. the tool a transform session installs --\n");

    AppState t;
    setActiveTool(t, Tool::Text);
    const bool changed = enterTransformTool(t);
    check(t.brush.tool == Tool::Move && changed,
          "toolswitch: REQUIRED -- beginning a transform leaves the pointer on the MOVE tool. "
          "Whatever tool was active keeps its canvas gesture armed under the gizmo, and the "
          "Text tool's gesture on empty canvas is 'make a new text layer'");
    check(hasPreviousTool(t) && previousTool(t) == Tool::Text,
          "toolswitch: and the tool it took over from is in the ledger like any deliberate "
          "pick, so `previousTool()` still names what the user was actually using");

    // Already on Move -- the Move tool's own drag begins a session this way,
    // and re-picking the tool you already have must not overwrite the ledger
    // with itself (the same rule `setActiveTool()` states for a palette
    // click on the selected cell).
    AppState m;
    setActiveTool(m, Tool::Pen);
    setActiveTool(m, Tool::Move);
    const bool movedAgain = enterTransformTool(m);
    check(!movedAgain && m.brush.tool == Tool::Move && previousTool(m) == Tool::Pen,
          "toolswitch: beginning one while ALREADY on Move reports no change and leaves the "
          "ledger alone -- it is not a switch");

    // Space held: the installed tool is the borrowed Hand and the tool the
    // user is in is `springReturn`. The borrow ends -- a gizmo is up and the
    // pan is over -- and the ledger records the tool they were really in,
    // never the Hand they never picked (header section 1).
    AppState h;
    setActiveTool(h, Tool::Text);
    beginSpringHand(h);
    check(h.brush.tool == Tool::Hand, "toolswitch: (setup) the Hand is borrowed");
    enterTransformTool(h);
    check(h.brush.tool == Tool::Move && !springHandHeld(h) && previousTool(h) == Tool::Text,
          "toolswitch: REQUIRED -- with Space held it ends the borrow and records TEXT as the "
          "previous tool. Recording the Hand would put a tool the user never chose into the "
          "ledger, which is exactly what `effectiveTool()` exists to prevent");

    // Flatting mode is a second answer to "what does a click mean", and it
    // has to go for the same reason the tool does -- a bridge stroke under a
    // live gizmo is the same defect wearing ADR-0009's hat. This comes free
    // from routing through `setActiveTool()`, and is asserted so that it
    // stays true if this ever stops doing so.
    AppState f;
    setFlatsTool(f, FlatsTool::BridgePen);
    check(f.flatsTool == FlatsTool::BridgePen, "toolswitch: (setup) flatting mode is on");
    enterTransformTool(f);
    check(f.flatsTool == FlatsTool::None && f.brush.tool == Tool::Move,
          "toolswitch: and it leaves flatting mode too -- a flatting gesture is a second "
          "meaning for a click, and one armed under a gizmo is the same hole");
  }

  // ==========================================================================
  // 8. A LIVE GIZMO IS MODAL: the tool cannot change until it is finished.
  // ==========================================================================
  //
  // Section 7 put the pointer on Move when a session begins, which fixed the
  // gesture the user was holding. It did not close the door: **the palette was
  // still live**, so picking the Text tool out of it while the gizmo was up
  // put the stray-layer hole straight back. Reported as "I can select a tool
  // while transforming... the transform needs to be committed before another
  // action can be performed."
  //
  // The refusal is asserted at `setActiveTool()` rather than at the palette
  // because that is where it is enforced -- ToolSwitch.hpp section 0's "one
  // writer" argument applies to the rules a writer keeps as much as to the
  // field it writes. The palette, the flyout, the Goodies menu and the flats
  // panel each draw themselves greyed from `transformModalRefusal()`, and
  // `app/selftest/ToolSurface.cpp` pins the menu's half of that.
  {
    std::printf("  -- 8. a live gizmo is modal --\n");

    // The fixture: one document, active, with a session live on its layer 0.
    // Built through the real `beginLayer()` rather than by poking fields --
    // there is no setter for `documentId_` and TransformSession.hpp says so
    // deliberately, which is what makes this the state the app can reach.
    // A blank document is not enough: `beginLayer()` measures
    // `layerContentBounds()` and refuses a layer with nothing in it, so the
    // fixture has to paint one texel before the session can exist at all.
    auto addInkedDocument = [](AppState& st) -> OpenDocument* {
      OpenDocument* od = st.documents.add(makeBlankOpenDocument(32, 24, WorkingSpace{}));
      if (od == nullptr || od->document.layers.empty()) return nullptr;
      TileStore& tiles = *od->document.layers[0].rgbTiles;
      for (int32_t y = 4; y < 12; ++y)
        for (int32_t x = 4; x < 12; ++x)
          tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
              .writePixel(tileLocalOffset(PixelCoord{x, y}), {1.0f, 0.5f, 0.25f, 1.0f});
      od->recordEdit("ink fixture", EditKind::Content);
      return od;
    };
    auto withLiveSession = [&](AppState& st) -> bool {
      OpenDocument* od = addInkedDocument(st);
      if (od == nullptr) return false;
      return st.transform.beginLayer(*od, 0).ok && st.transform.active();
    };

    AppState t;
    setActiveTool(t, Tool::Brush);
    check(withLiveSession(t), "toolswitch: (setup) a transform session is live on the "
                              "active document");
    check(transformModalRefusal(t) != nullptr,
          "toolswitch: REQUIRED -- a live gizmo on the document in front of the user refuses "
          "a tool change, and says so in a sentence a greyed cell can show");

    const Tool before = t.brush.tool;
    const bool accepted = setActiveTool(t, Tool::Text);
    check(!accepted && t.brush.tool == before,
          "toolswitch: REQUIRED -- picking the TEXT tool under a live gizmo is refused and "
          "changes nothing. This is the reported defect: the Text tool's gesture on empty "
          "canvas makes a layer, and the palette was handing it back while the box was up");
    check(previousTool(t) == Tool::Brush || !hasPreviousTool(t),
          "toolswitch: and a refused pick does not move the ledger -- `previousTool()` must "
          "not learn about a switch that never happened");

    // Every cell, not just the content-making ones. The reported symptom was
    // the Text tool, but a modal state that leaks for the Hand is not modal.
    check(!setActiveTool(t, Tool::Hand) && !setActiveTool(t, Tool::Move) &&
              !setActiveTool(t, Tool::Zoom),
          "toolswitch: REQUIRED -- and it refuses EVERY tool, including the Move cell that is "
          "already lit and the two that need no document. This axis is a property of the "
          "session, not of the tool");

    // The one switch that is not the user changing their mind. Every begin
    // calls it with the session ALREADY live, so a shared gate would refuse
    // exactly the switch that makes the modality true in the first place.
    AppState e;
    setActiveTool(e, Tool::Text);
    check(withLiveSession(e), "toolswitch: (setup) a second fixture with a live session");
    check(enterTransformTool(e) && e.brush.tool == Tool::Move,
          "toolswitch: REQUIRED -- `enterTransformTool()` is exempt. It is called AFTER the "
          "begin succeeds, so a gate it shared with `setActiveTool()` would refuse the very "
          "switch that installs the modal tool");

    // The flatting palette is a second answer to "what does a click mean", and
    // it writes `brush.tool` by its own route -- so it needs its own check,
    // not an inherited one.
    AppState f;
    check(withLiveSession(f), "toolswitch: (setup) a third fixture with a live session");
    check(!setFlatsTool(f, FlatsTool::BridgePen) && f.flatsTool == FlatsTool::None &&
              f.brush.tool != Tool::Pencil,
          "toolswitch: REQUIRED -- a FLATTING tool is refused too, and installs no host tool. "
          "`setFlatsTool()` writes `brush.tool` directly rather than through "
          "`setActiveTool()`, so it cannot inherit the check");

    // **The dead end this scoping exists to avoid.** A session outlives a
    // document switch, and the canvas block's Escape key is scoped to the
    // active document -- so a lock keyed on `active()` alone would freeze the
    // palette over a document that shows no gizmo and offers no key to lift
    // it. Driven the way the app reaches it: two documents, session on the
    // first, the second made active.
    AppState away;
    OpenDocument* a = addInkedDocument(away);
    check(a != nullptr && away.transform.beginLayer(*a, 0).ok,
          "toolswitch: (setup) a session on document A");
    OpenDocument* b = addInkedDocument(away);
    check(b != nullptr && away.documents.active() == b && away.transform.active(),
          "toolswitch: (setup) document B is active and the session on A is still live");
    check(transformModalRefusal(away) == nullptr && setActiveTool(away, Tool::Text) &&
              away.brush.tool == Tool::Text,
          "toolswitch: REQUIRED -- a session on a document the user has tabbed AWAY from does "
          "not lock the palette. It draws no gizmo and its Escape key is scoped to the "
          "document it is on, so locking from behind it would be a modal state with no "
          "dialog on screen and no way out");

    // The other half of that scoping lives in the canvas block, which
    // re-installs Move whenever the gizmo is on screen -- so coming back to A
    // with the Text tool in hand does not reopen the hole. That line reads the
    // ImGui frame this suite has no way to run; what is assertable here is the
    // call it makes, and that it is a no-op once Move is installed.
    check(enterTransformTool(away) && away.brush.tool == Tool::Move &&
              !enterTransformTool(away),
          "toolswitch: and returning to A re-installs Move, reporting a change once and "
          "nothing on every frame after -- the canvas block calls this every frame the gizmo "
          "is up, and a call that moved the ledger each time would be a lie in the ledger");

    // Nothing live, nothing refused -- the case that must not regress, since
    // every tool change in the application goes through the same door.
    AppState idle;
    check(transformModalRefusal(idle) == nullptr && setActiveTool(idle, Tool::Lasso) &&
              idle.brush.tool == Tool::Lasso,
          "toolswitch: with no session at all the setter is exactly what it was");

    // **Space still pans.** The borrow does not go through `setActiveTool()`
    // (header section 1) and is deliberately left open: seeing the far end of
    // what you are transforming is not choosing a tool, and letting go changes
    // nothing about what a click means.
    AppState pan;
    check(withLiveSession(pan), "toolswitch: (setup) a fourth fixture with a live session");
    enterTransformTool(pan);
    check(beginSpringHand(pan) && pan.brush.tool == Tool::Hand,
          "toolswitch: REQUIRED -- Space still borrows the Hand under a live gizmo. A modal "
          "transform you cannot pan is one you cannot aim");
    check(endSpringHand(pan) && pan.brush.tool == Tool::Move,
          "toolswitch: and letting go hands the Move tool back, not whatever was underneath");

    // The Eyedropper's borrow needs no gate: Move is the only tool a live
    // session can be in, and Move is not eligible. Asserted rather than
    // assumed, because it is a gate that exists by construction -- exactly
    // the kind that stops existing when someone adds a row to a table.
    check(!springEyedropperEligible(Tool::Move, BucketFill::Colour) &&
              !springEyedropperEligible(Tool::Move, BucketFill::Flats),
          "toolswitch: and Alt cannot borrow the Eyedropper under a gizmo without a line "
          "being written -- Move is the only tool a session can be in, and Move is not "
          "eligible for that borrow in either fill mode");
  }

  std::printf("[selftest] tool switch %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
