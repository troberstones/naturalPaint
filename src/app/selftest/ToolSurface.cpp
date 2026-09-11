#include "app/selftest/Support.hpp"

#include <cstring>
#include <string>

#include "app/MoveTool.hpp"       // toolMovesPixels(), the gate Move answers
#include "app/StrokeSession.hpp"  // the route table and five of the gates
#include "app/ToolSurface.hpp"
#include "app/ZoomAndSize.hpp"  // toolZoomsView()
#include "imgui.h"
#include "ui/AtelierChrome.hpp"  // toolImplemented(), toolHasCanvasHandler(), toolName()
#include "ui/MacPaintUI.hpp"     // toolMenuFamily(), A4's own function

namespace np {

// docs/testing-issues.md T5, short-term half -- app/ToolSurface.
//
// **Reversed 2026-09-08.** This section used to pin a six-and-fifteen split
// with Brush, Water and Dry Brush among the six that survive with no
// document open, because painting the bare `sim::PaintSim` canvas was a
// supported workflow. It no longer is: T5's long-term half tore the canvas
// down with the last document instead, so those three tools joined the
// eighteen that need one. Section C below used to be titled "the bare canvas
// is a supported workflow and still paints"; it now asserts the opposite,
// under the same name, for the same reason a fixture that used to prove a
// route worked now has to prove it refuses.
//
// **What would make this section worthless, stated first.** If
// `toolActsWithoutDocument()` turned out to equal `toolImplemented()` or
// `toolHasCanvasHandler()` for every `Tool`, this would be a synonym dressed
// as an axis and every assertion below would be unfalsifiable. So the FIRST
// thing asserted is the strict-subset relation in both directions, counted off
// the predicates rather than off a literal, and the three-and-eighteen split
// is then traced tool by tool to the gate that produced it.
//
// Headless and GPU-free: no device, no window, no ImGui frame. The palette
// cells these predicates dim are photographed by `tools/golden/run_golden.sh`'s
// `no_document` view instead, which is the only place that drawing can be
// checked at all.
bool runToolSurfaceTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] tool surface: T5's second axis -- can this tool act on the canvas in "
      "front of the user\n");

  // The shared clause every refusal in this build ends with
  // (ui/MacPaintUI.cpp:350 and :5352). Asserted rather than assumed, because
  // "matches the existing voice" is otherwise a claim nothing checks.
  const char* kClause = "no document is open. File > New Document makes one.";

  // -----------------------------------------------------------------------
  // A. The axis is an axis: a strict, proper subset of toolImplemented()
  // -----------------------------------------------------------------------
  {
    int implemented = 0;
    int handled = 0;
    int withoutDoc = 0;
    bool everyWithoutDocIsImplemented = true;
    bool everyWithoutDocIsHandled = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (toolImplemented(t)) ++implemented;
      if (toolHasCanvasHandler(t)) ++handled;
      if (toolActsWithoutDocument(t)) {
        ++withoutDoc;
        if (!toolImplemented(t)) everyWithoutDocIsImplemented = false;
        if (!toolHasCanvasHandler(t)) everyWithoutDocIsHandled = false;
      }
    }

    // The relationship the existing suite already pins, restated here as the
    // baseline this axis is measured against -- if it ever stopped holding,
    // "strictly fewer than implemented" would be measuring something else.
    check(implemented == handled,
          "baseline: toolImplemented() still equals toolHasCanvasHandler() by count -- "
          "runEyedropperTest() owns the per-tool form of this claim");

    check(everyWithoutDocIsImplemented && everyWithoutDocIsHandled,
          "axis: every tool that acts with NO document is built AND has a canvas handler "
          "-- the new predicate is a subset, never a widening");

    // **The half that makes the assertion falsifiable.** A synonym would put
    // these two counts equal; an axis must leave built tools behind.
    check(withoutDoc > 0 && withoutDoc < implemented,
          "axis: it is a PROPER subset -- some built tools act with no document and some "
          "do not, so this is a second question and not a synonym for the first");

    // Counted, not asserted as a literal, and then the split is named so the
    // number in the report is a fact rather than an estimate.
    std::printf("  [measured] %d of %d built tools act with no document open\n", withoutDoc,
                implemented);
  }

  // -----------------------------------------------------------------------
  // B. Each survivor traced to its OWN gate -- never to a list
  // -----------------------------------------------------------------------
  //
  // The shape app/selftest/Eyedropper.cpp uses for Zoom and Move: assert the
  // gate that answers, AND that the neighbouring gates do not, so a future
  // "fix" that widens an existing predicate instead of reading the right one
  // fails here.
  {
    check(toolActsWithoutDocument(Tool::Hand) && toolPansView(Tool::Hand) &&
              !toolZoomsView(Tool::Hand) &&
              strokeRouteFor(Tool::Hand, nullptr) == StrokeRoute::None,
          "gate: the Hand survives through toolPansView() -- the view is process state and "
          "there is no document in the expression at all");
    check(toolActsWithoutDocument(Tool::Zoom) && toolZoomsView(Tool::Zoom) &&
              !toolPansView(Tool::Zoom),
          "gate: Zoom survives through its OWN toolZoomsView(), not by being folded into "
          "the pan gate");
    check(toolActsWithoutDocument(Tool::Measure) && toolMeasuresCanvas(Tool::Measure) &&
              !toolSamplesCanvas(Tool::Measure),
          "gate: Measure survives through toolMeasuresCanvas() -- MeasureLine's documentId "
          "0 IS the no-document value, and it is not the eyedropper's gate");

    // **Brush, Water and Dry Brush are the reversal's whole point.**
    // `strokeRouteFor(t, nullptr)` still answers `PaintSim` -- that table did
    // not change, only what `PaintSim` on its own is worth changed -- so the
    // subset relation up in section A would go vacuous here if this predicate
    // still said yes. It must not.
    check(!toolActsWithoutDocument(Tool::Brush) && !toolActsWithoutDocument(Tool::Water) &&
              !toolActsWithoutDocument(Tool::DryBrush) &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::Water, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::DryBrush, nullptr) == StrokeRoute::PaintSim,
          "gate, reversed: Brush, Water and Dry Brush no longer survive with no document, "
          "even though strokeRouteFor(t, nullptr) still answers PaintSim for all three -- "
          "that route means \"which layer\", not \"does a canvas exist\"");
  }

  // -----------------------------------------------------------------------
  // C. THE LINE, reversed. There is no bare canvas left to paint.
  // -----------------------------------------------------------------------
  //
  // Until 2026-09-08 this section asserted the opposite of what follows:
  // File > "New" was renamed "New Canvas" precisely so painting with no
  // document open could not be mistaken for "New Document", and a change that
  // made the document-scoped tools legible by also switching the paint tools
  // off would have destroyed the one thing that renaming protected. T5's
  // long-term half removed the thing being protected -- `sim::PaintSim` is
  // torn down with the last document and never rebuilt without one -- so the
  // workflow this section used to pin no longer exists, and pinning it as
  // still working would be asserting a state the build can no longer reach.
  {
    check(toolSurfaceRefusal(Tool::Brush, false) != nullptr &&
              toolSurfaceRefusal(Tool::Water, false) != nullptr &&
              toolSurfaceRefusal(Tool::DryBrush, false) != nullptr,
          "the line, reversed: with NO document, the three paint tools ARE given a refusal "
          "sentence now -- there is no canvas left for them to reach");
    check(toolSurfaceRefusal(Tool::Hand, false) == nullptr &&
              toolSurfaceRefusal(Tool::Zoom, false) == nullptr &&
              toolSurfaceRefusal(Tool::Measure, false) == nullptr,
          "the line, reversed: Hand, Zoom and Measure are the only three left with no "
          "refusal sentence -- they move process state or measure geometry, neither of "
          "which needed a canvas to begin with");
  }

  // -----------------------------------------------------------------------
  // D. The other side of the line, each through the gate that refuses it
  // -----------------------------------------------------------------------
  {
    // The five stroke tools the route table sends nowhere with a null target,
    // by name, with the argument written in strokeRouteFor()'s own comment:
    // the solver has no alpha, no aliased mark, no smudge, nothing to sample
    // and no tonal step.
    const Tool kStrokeRefusers[] = {Tool::Eraser,     Tool::Pencil, Tool::Smudge,
                                    Tool::CloneStamp, Tool::Dodge,  Tool::Burn};
    bool everyStrokeRefuserRefuses = true;
    for (Tool t : kStrokeRefusers) {
      if (strokeRouteFor(t, nullptr) != StrokeRoute::None) everyStrokeRefuserRefuses = false;
      if (toolActsWithoutDocument(t)) everyStrokeRefuserRefuses = false;
      if (!toolImplemented(t)) everyStrokeRefuserRefuses = false;  // built, and still refused
    }
    check(everyStrokeRefuserRefuses,
          "refused: Eraser, Pencil, Smudge, Clone Stamp, Dodge and Burn are BUILT and still "
          "cannot act with no document -- through the route table's own nullptr row");

    // **The three paint tools, joining the refused side for the first time.**
    // Their route is NOT `None` like the six above -- `strokeRouteFor()`
    // still answers `PaintSim`, unchanged -- so they refuse through a
    // DIFFERENT gate: `toolBeginsStroke()`'s fallback in
    // `toolSurfaceRefusal()` ("Nothing to paint on"), reached precisely
    // because `toolActsWithoutDocument()` no longer accepts the `PaintSim`
    // route on its own (section B's reversed check, from the predicate's
    // side).
    const Tool kPaintTools[] = {Tool::Brush, Tool::Water, Tool::DryBrush};
    bool everyPaintToolRefusesNow = true;
    for (Tool t : kPaintTools) {
      if (toolActsWithoutDocument(t)) everyPaintToolRefusesNow = false;
      if (strokeRouteFor(t, nullptr) != StrokeRoute::PaintSim) everyPaintToolRefusesNow = false;
      if (!toolBeginsStroke(t)) everyPaintToolRefusesNow = false;
      if (!toolImplemented(t)) everyPaintToolRefusesNow = false;  // built, and still refused
    }
    check(everyPaintToolRefusesNow,
          "refused: Brush, Water and Dry Brush are BUILT, still route to PaintSim, and still "
          "cannot act with no document -- because there is no PaintSim to route to");

    check(pixelOpRefusalFor(nullptr) == PixelOpRefusal::NoLayer &&
              !toolActsWithoutDocument(Tool::PaintBucket) &&
              !toolActsWithoutDocument(Tool::Gradient),
          "refused: the bucket and the gradient, through pixelOpRefusalFor(nullptr) == "
          "NoLayer -- that enum's own first non-None row");

    bool everySelectionToolRefuses = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (toolDrawsSelection(t) && toolActsWithoutDocument(t)) everySelectionToolRefuses = false;
    }
    check(everySelectionToolRefuses,
          "refused: every toolDrawsSelection() tool -- a Selection is bounded by a "
          "document's width and height and lives on OpenDocument");

    check(!toolActsWithoutDocument(Tool::Move) && toolMovesPixels(Tool::Move),
          "refused: Move -- beginMove() takes the document by reference, so this is "
          "structurally impossible rather than merely declined");
    check(!toolActsWithoutDocument(Tool::Eyedropper) && toolSamplesCanvas(Tool::Eyedropper),
          "refused: the eyedropper -- applyEyedropperPick()'s own first branch says the "
          "solver canvas is not a document and probePixel() takes one");
  }

  // -----------------------------------------------------------------------
  // E. With a document open, the axis admits everything the build has
  // -----------------------------------------------------------------------
  //
  // The other half of "walk every Tool against the predicate with no document
  // open and with one open". `documentOpen = true` must never refuse: whatever
  // else is wrong with the gesture at that point -- a locked layer, the wrong
  // layer kind, no clone anchor -- belongs to the refusal ladder at the moment
  // of the gesture, and re-answering any of it here would be a second copy to
  // drift (app/ToolSurface.hpp §2).
  {
    bool nothingRefusedWithADocument = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      if (toolSurfaceRefusal(static_cast<Tool>(i), true) != nullptr)
        nothingRefusedWithADocument = false;
    }
    check(nothingRefusedWithADocument,
          "with a document: no Tool is refused by the SURFACE axis -- the refusal ladder "
          "owns locked layers and layer kinds, and this axis must not restate them");
  }

  // -----------------------------------------------------------------------
  // F. The sentences: one reason, never two, and the build's own clause
  // -----------------------------------------------------------------------
  {
    bool everyRefusalCarriesTheClause = true;
    bool everyNotBuiltCellIsSilent = true;
    bool everyBuiltRefuserSpeaks = true;
    int spoken = 0;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const char* why = toolSurfaceRefusal(t, false);
      if (!toolHasCanvasHandler(t)) {
        // The case that matters: "Not built yet." is already the whole answer
        // for these seven cells, and a second sentence underneath would tell a
        // user to open a document so a tool that does not exist can fail on it.
        if (why != nullptr) everyNotBuiltCellIsSilent = false;
        continue;
      }
      if (toolActsWithoutDocument(t)) continue;  // section C already pinned these
      if (why == nullptr) {
        everyBuiltRefuserSpeaks = false;
        continue;
      }
      ++spoken;
      if (std::strstr(why, kClause) == nullptr) everyRefusalCarriesTheClause = false;
    }
    check(everyBuiltRefuserSpeaks && spoken > 0,
          "sentences: every built tool the surface refuses says so -- a dimmed cell that "
          "explained nothing would be the silent no-op this whole discipline is against");
    check(everyRefusalCarriesTheClause,
          "sentences: each one ends in the build's EXISTING clause verbatim, not a third "
          "phrasing of the one fact (ui/MacPaintUI.cpp:350 and :5352)");
    check(everyNotBuiltCellIsSilent,
          "sentences: a cell with no canvas handler gets NO surface sentence -- two axes, "
          "two reasons, never stacked on one cell");

    // The lead-in is chosen by GATE, not by Tool, so a family shares one
    // sentence and a sixth member of any family inherits it. Asserted by
    // showing two members of one family agree and two families differ.
    check(std::strcmp(toolSurfaceRefusal(Tool::Marquee, false),
                      toolSurfaceRefusal(Tool::Lasso, false)) == 0 &&
              std::strcmp(toolSurfaceRefusal(Tool::PaintBucket, false),
                          toolSurfaceRefusal(Tool::Gradient, false)) == 0 &&
              std::strcmp(toolSurfaceRefusal(Tool::Marquee, false),
                          toolSurfaceRefusal(Tool::PaintBucket, false)) != 0,
          "sentences: the lead-in follows the GATE -- one family, one sentence, and two "
          "families do not share one");
  }

  // -----------------------------------------------------------------------
  // G. The Goodies menu, A4's own function, on the second axis
  // -----------------------------------------------------------------------
  //
  // A4 (docs/reachability-audit.md) was a menu offering all 27 tools while the
  // palette correctly gated the same list one panel over. `MenuAction::ToolItem`
  // still calls `setActiveTool()` unconditionally, so the `enabled` flag this
  // function sets is the whole gate -- which means the identical defect is
  // available on the second axis, and this is the assertion that closes it.
  // `app/selftest/MenuBasics.cpp` owns the `documentOpen = true` half.
  {
    const std::vector<MenuFamilyEntry> closed = toolMenuFamily(Tool::Brush, false, nullptr);
    const std::vector<MenuFamilyEntry> open = toolMenuFamily(Tool::Brush, true, nullptr);
    check(closed.size() == static_cast<size_t>(Tool::Count) && closed.size() == open.size(),
          "menu: the Goodies tool family still offers every tool in both states -- "
          "disabled, not hidden");

    size_t enabledClosed = 0;
    size_t enabledOpen = 0;
    size_t expectedClosed = 0;
    bool everyDisabledCarriesOneReason = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (toolImplemented(t) && toolActsWithoutDocument(t)) ++expectedClosed;
      if (closed[static_cast<size_t>(i)].enabled) ++enabledClosed;
      if (open[static_cast<size_t>(i)].enabled) ++enabledOpen;
      const MenuFamilyEntry& e = closed[static_cast<size_t>(i)];
      if (!e.enabled && e.tooltip.empty()) everyDisabledCarriesOneReason = false;
      // Exactly one reason, never both: the two predicates are disjoint by
      // construction and this is what proves the construction held.
      if (!e.enabled && e.tooltip.find("Not built yet.") != std::string::npos &&
          e.tooltip.find(kClause) != std::string::npos)
        everyDisabledCarriesOneReason = false;
      if (e.enabled && !e.tooltip.empty()) everyDisabledCarriesOneReason = false;
    }
    check(enabledClosed == expectedClosed && enabledClosed < enabledOpen,
          "menu: with no document the Goodies menu enables exactly the tools that clear "
          "BOTH axes, and strictly fewer than it does with one -- A4's defect, measured "
          "on the second axis");
    check(everyDisabledCarriesOneReason,
          "menu: every disabled entry carries exactly one reason and every enabled one "
          "carries none");

    // The THIRD axis (app/ToolSwitch.hpp section 5). Same defect shape, one
    // axis further on: the palette and the flyout both refuse every cell
    // while a transform gizmo is up, and a Goodies menu that did not would be
    // A4 a third time -- a live route to the tool change the other two
    // correctly disable. The sentence is passed in rather than read from an
    // `AppState` so this stays the pure, disk-free function the header
    // promises.
    const char* kModal = "A transform is in progress. Press Return to apply it or Escape to "
                         "cancel it.";
    const std::vector<MenuFamilyEntry> modal = toolMenuFamily(Tool::Brush, true, kModal);
    size_t enabledModal = 0;
    size_t carryingModalReason = 0;
    for (const MenuFamilyEntry& e : modal) {
      if (e.enabled) ++enabledModal;
      if (e.tooltip.find("transform is in progress") != std::string::npos)
        ++carryingModalReason;
    }
    check(modal.size() == open.size() && enabledModal == 0,
          "menu: REQUIRED -- with a transform gizmo live the Goodies menu enables NOT ONE "
          "tool, where the same call with no gizmo enables the built ones. This axis is a "
          "property of the session, so it takes every entry rather than a subset");
    // Every BUILT tool: the unbuilt cells keep "Not built yet.", which stays
    // the more useful sentence about a cell that is still dead once the gizmo
    // has gone.
    size_t builtCount = 0;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i)
      if (toolImplemented(static_cast<Tool>(i))) ++builtCount;
    check(carryingModalReason == builtCount,
          "menu: and every BUILT entry carries the gizmo's own sentence, so a user who "
          "reaches past the greyed palette to the menu is told the same thing there -- the "
          "unbuilt ones keep \"Not built yet.\", which outlives the session");
  }

  // -----------------------------------------------------------------------
  // H. The LAYER menu is GREYED by a live gizmo -- the one menu that is
  // -----------------------------------------------------------------------
  //
  // `ui/MenuModel.hpp`'s `menuActionEndsTransform()` says the menu bar
  // CANCELS a transform rather than being blocked by it, because greying it
  // would take Undo, Save and Quit with it. The Layer menu is the exception,
  // and the exception is about what these commands are: the
  // delete/reorder/merge/group family is `docs/testing-issues.md` T29's own
  // measured corruption, and it is the LAYERS panel's buttons wearing a
  // different hat. That panel is refused outright, so offering the same acts
  // one menu over -- at the price of the transform -- would be two surfaces
  // disagreeing about a single thing.
  {
    // A three-layer fixture, so the reorder and merge commands have somewhere
    // to go and `layerCommandAvailable()` answers true for a real majority of
    // the list rather than for two entries.
    Document doc;
    doc.width = 32;
    doc.height = 24;
    for (int k = 0; k < 3; ++k) {
      Layer L;
      L.kind = LayerKind::RGB;
      L.name = "L" + std::to_string(k);
      L.rgbTiles = TileStore{};
      doc.layers.push_back(std::move(L));
    }
    // **A NON-EMPTY selection**, and that is not incidental: with an empty one
    // every `layerSetCommandAvailable()` answers false, so both lists come
    // back with zero enabled and "the gizmo disabled them" would be true of a
    // list that was already dead. The first version of this fixture had that
    // bug and the `enabledSetHeld < enabledSetFree` term is what caught it.
    const LayerSelection two = makeLayerSelection({0, 1});
    const char* kModal = "A transform is in progress. Press Return to apply it or Escape to "
                         "cancel it.";

    const std::vector<MenuFamilyEntry> freeCmds = layerMenuFamily(doc, 1, nullptr);
    const std::vector<MenuFamilyEntry> heldCmds = layerMenuFamily(doc, 1, kModal);
    const std::vector<MenuFamilyEntry> freeSet = layerSetMenuFamily(doc, two, nullptr);
    const std::vector<MenuFamilyEntry> heldSet = layerSetMenuFamily(doc, two, kModal);

    check(!freeCmds.empty() && freeCmds.size() == heldCmds.size() &&
              !freeSet.empty() && freeSet.size() == heldSet.size(),
          "layer menu: both families still offer every command in both states -- disabled, "
          "not hidden, the same rule the tool family follows");

    size_t enabledFree = 0;
    size_t enabledHeld = 0;
    for (const MenuFamilyEntry& e : freeCmds) if (e.enabled) ++enabledFree;
    for (const MenuFamilyEntry& e : heldCmds) if (e.enabled) ++enabledHeld;
    check(enabledFree > 0,
          "layer menu: (setup) with no gizmo the fixture really does enable commands -- an "
          "assertion against a list that was empty either way would prove nothing");
    check(enabledHeld == 0,
          "layer menu: REQUIRED -- with a gizmo live NOT ONE Layer command is enabled. This "
          "is the delete/reorder/merge family T29 measured, and the LAYERS panel is already "
          "refused; the menu must not be the way round it");

    size_t enabledSetFree = 0;
    size_t enabledSetHeld = 0;
    for (const MenuFamilyEntry& e : freeSet) if (e.enabled) ++enabledSetFree;
    for (const MenuFamilyEntry& e : heldSet) if (e.enabled) ++enabledSetHeld;
    check(enabledSetHeld == 0 && enabledSetHeld < enabledSetFree,
          "layer menu: REQUIRED -- and neither is one entry of the Selection submenu, which "
          "is the multi-layer form of the same commands. Stopping one list and not the other "
          "would only move the hole one submenu over");

    // The sentence reaches the greyed entry, and reaches ONLY the entries the
    // gizmo is the sole reason for -- a command already unavailable on its own
    // terms keeps the empty tooltip it has always had, because naming the
    // gizmo on "Remove Mask" over a layer with no mask names the wrong
    // obstacle.
    size_t carrying = 0;
    for (const MenuFamilyEntry& e : heldCmds)
      if (e.tooltip.find("transform is in progress") != std::string::npos) ++carrying;
    check(carrying == enabledFree,
          "layer menu: REQUIRED -- the gizmo's sentence lands on exactly the commands it is "
          "the SOLE reason for. Two axes, never two sentences: one that was already "
          "unavailable keeps its own empty tooltip rather than being told to press Escape");
  }

  // -----------------------------------------------------------------------
  // I. And the CONTEXT really asks. The wiring, not the classification.
  // -----------------------------------------------------------------------
  //
  // Sections G and H drive `toolMenuFamily()` and the two layer families with
  // a `modalWhy` handed to them, which proves what each does with one. It does
  // NOT prove anybody fetches it. Replacing `transformModalRefusal(st)` in
  // `menuContextFromState()` with `nullptr` produced **zero failures** across
  // the whole suite -- three perfect classifications and no wire between them
  // and the application, which is the "green assertion, dead probe" shape
  // this project has been bitten by before. This section is that wire.
  {
    AppState st;
    // The precondition `ui/MacPaintUI.hpp` states, and the assertion that it
    // held: the flag is the eager recent-documents load's own guard, so
    // setting it skips the read of the user's real preferences file.
    st.recentDocumentsLoaded = true;

    OpenDocument* od = st.documents.add(makeBlankOpenDocument(32, 24, WorkingSpace{}));
    check(od != nullptr && !od->document.layers.empty(),
          "menu wiring: (setup) one open document with a layer");
    TileStore& tiles = *od->document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 6; ++y)
      for (int32_t x = 0; x < 6; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {1.0f, 0.5f, 0.25f, 1.0f});
    od->recordEdit("ink", EditKind::Content);

    const MenuContext before = menuContextFromState(st);
    check(st.recentDocuments.entries().empty(),
          "menu wiring: and the precondition held -- nothing was read from the user's real "
          "recent-documents file, which is the one file this suite must never touch");

    size_t toolsBefore = 0;
    size_t layersBefore = 0;
    for (const MenuFamilyEntry& e : before.tools) if (e.enabled) ++toolsBefore;
    for (const MenuFamilyEntry& e : before.layerCommands) if (e.enabled) ++layersBefore;
    check(toolsBefore > 0 && layersBefore > 0,
          "menu wiring: (setup) with no gizmo the assembled context enables tools and layer "
          "commands -- an assertion against two empty lists would prove nothing");

    check(st.transform.beginLayer(*od, 0).ok && st.transform.active(),
          "menu wiring: (setup) a transform session begins on that document");

    const MenuContext held = menuContextFromState(st);
    size_t toolsHeld = 0;
    size_t layersHeld = 0;
    size_t layerSetHeld = 0;
    for (const MenuFamilyEntry& e : held.tools) if (e.enabled) ++toolsHeld;
    for (const MenuFamilyEntry& e : held.layerCommands) if (e.enabled) ++layersHeld;
    for (const MenuFamilyEntry& e : held.layerSetCommands) if (e.enabled) ++layerSetHeld;
    check(toolsHeld == 0 && layersHeld == 0 && layerSetHeld == 0,
          "menu wiring: REQUIRED -- with a session live on the active document, the assembled "
          "context disables every tool AND every Layer command. This is the line that fetches "
          "the refusal, and a sabotage of it reddens nothing else in the suite");

    check(held.layerCommands.size() == before.layerCommands.size() &&
              held.tools.size() == before.tools.size(),
          "menu wiring: ...by disabling them, not by dropping them -- a menu that shortened "
          "under a gizmo would move every item the user was aiming at");
  }

  std::printf("[selftest] tool surface %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
