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
// **What would make this section worthless, stated first.** If
// `toolActsWithoutDocument()` turned out to equal `toolImplemented()` or
// `toolHasCanvasHandler()` for every `Tool`, this would be a synonym dressed
// as an axis and every assertion below would be unfalsifiable. So the FIRST
// thing asserted is the strict-subset relation in both directions, counted off
// the predicates rather than off a literal, and the six-and-fifteen split is
// then traced tool by tool to the gate that produced it.
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

    // The three paint tools, through the route table itself. This is section
    // C's claim from the predicate's side; both are kept, because a route
    // table that changed and a predicate that did not is exactly the drift the
    // whole design is arranged to make impossible.
    check(toolActsWithoutDocument(Tool::Brush) && toolActsWithoutDocument(Tool::Water) &&
              toolActsWithoutDocument(Tool::DryBrush),
          "gate: Brush, Water and Dry Brush survive through strokeRouteFor(t, nullptr)");
  }

  // -----------------------------------------------------------------------
  // C. THE LINE. The bare canvas is a supported workflow and still paints.
  // -----------------------------------------------------------------------
  //
  // File > "New" is called "New Canvas" precisely so it cannot be read as
  // "New Document" (docs/testing-issues.md T5). Painting with no document open
  // is designed behaviour, and a change that made the document-scoped tools
  // legible by also switching the paint tools off would have destroyed the one
  // thing it was supposed to protect. Asserted as its own claim, in the route
  // table's own terms, rather than left to be inferred from section B.
  {
    check(strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::Water, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::DryBrush, nullptr) == StrokeRoute::PaintSim,
          "the line: with NO document, the three paint tools still route to the solver "
          "canvas -- painting the bare canvas is supported and is not disabled here");
    check(toolSurfaceRefusal(Tool::Brush, false) == nullptr &&
              toolSurfaceRefusal(Tool::Water, false) == nullptr &&
              toolSurfaceRefusal(Tool::DryBrush, false) == nullptr &&
              toolSurfaceRefusal(Tool::Hand, false) == nullptr &&
              toolSurfaceRefusal(Tool::Zoom, false) == nullptr &&
              toolSurfaceRefusal(Tool::Measure, false) == nullptr,
          "the line: and none of the six is given a refusal sentence, so no live cell can "
          "be dimmed by the palette's tooltip path either");
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
    const std::vector<MenuFamilyEntry> closed = toolMenuFamily(Tool::Brush, false);
    const std::vector<MenuFamilyEntry> open = toolMenuFamily(Tool::Brush, true);
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
  }

  std::printf("[selftest] tool surface %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
