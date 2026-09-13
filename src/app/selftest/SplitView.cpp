#include "app/selftest/Support.hpp"

#include <cmath>
#include <string>

#include "app/AppState.hpp"
#include "app/SplitView.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"

// Track `split`: View > Split View / Match Zoom.
//
// Pane geometry itself (`atelierSplitPanes()` tiles the canvas with no
// overlap) and the pane/companion repair rules (a closed companion is
// replaced, a session down to one document collapses to one pane) are
// already asserted in app/selftest/DocumentResidency.cpp's own §1/§2 against
// the same production functions -- not repeated here. What is new to this
// track, and what this section owns, is: the match-zoom mapping, the pane
// hit-test, the two entry points a click and a menu item actually call
// (`focusSplitPane()`, `toggleSplitView()`), and the one piece of state
// neither older section could have covered because it did not exist yet --
// `AtelierSplitState::companionView` resetting when the companion changes.
namespace np {

bool runSplitViewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // -----------------------------------------------------------------------
  // 1. matchZoomView -- pure, GPU-free arithmetic
  // -----------------------------------------------------------------------
  std::printf("  -- 1. matchZoomView --\n");
  {
    CanvasView src;
    src.zoom = 2.0f;
    src.panX = 30.0f;
    src.panY = -20.0f;
    src.rotation = 0.7f;
    src.mirrorX = true;

    // Identity: same document size on both sides.
    const CanvasView same = matchZoomView(src, 800.0f, 600.0f, 800.0f, 600.0f);
    check(same.zoom == src.zoom && same.panX == src.panX && same.panY == src.panY,
          "same-size documents: pan/zoom pass through exactly");

    // Fixture check before trusting the mapped result on it.
    const float srcW = 800.0f, srcH = 600.0f, dstW = 400.0f, dstH = 900.0f;
    check(srcW != dstW && srcH != dstH, "fixture: the two documents differ in both dimensions");

    const CanvasView mapped = matchZoomView(src, srcW, srcH, dstW, dstH);
    check(mapped.zoom == src.zoom, "the same zoom factor, not rescaled");
    check(mapped.panX == src.panX * (dstW / srcW), "panX scales by the width ratio (exact)");
    check(mapped.panY == src.panY * (dstH / srcH), "panY scales by the height ratio (exact)");

    // The derivation's own check, not the formula restated: recompute the
    // normalised centre fraction independently on each side and compare.
    const float srcFracX = 0.5f - src.panX / (src.zoom * srcW);
    const float dstFracX = 0.5f - mapped.panX / (mapped.zoom * dstW);
    const float srcFracY = 0.5f - src.panY / (src.zoom * srcH);
    const float dstFracY = 0.5f - mapped.panY / (mapped.zoom * dstH);
    check(std::fabs(srcFracX - dstFracX) < 1e-6f, "the normalised centre X survives the mapping");
    check(std::fabs(srcFracY - dstFracY) < 1e-6f, "the normalised centre Y survives the mapping");

    // Rotation/mirror: the companion never draws rotated or mirrored (its own
    // quad is always axis-aligned), so these are simply carried over from
    // `source` rather than transformed -- asserted here so a future change
    // that started zeroing them, or rotating them, would go red.
    check(mapped.rotation == src.rotation && mapped.mirrorX == src.mirrorX &&
              mapped.mirrorY == src.mirrorY,
          "every other CanvasView field is copied from source, untouched");

    // Degenerate guard: a source with no extent has no "relative position"
    // to preserve, so the function must not divide by zero.
    const CanvasView guarded = matchZoomView(src, 0.0f, 100.0f, 50.0f, 50.0f);
    check(guarded.zoom == src.zoom && guarded.panX == src.panX && guarded.panY == src.panY,
          "a zero source dimension returns source unchanged");
  }

  // -----------------------------------------------------------------------
  // 2. atelierPaneAt -- the click-routing hit-test
  // -----------------------------------------------------------------------
  std::printf("  -- 2. atelierPaneAt --\n");
  {
    const AtelierRect canvas{0.0f, 0.0f, 800.0f, 600.0f};
    const AtelierPanes panes = atelierSplitPanes(canvas, AtelierSplit::Columns);
    check(panes.count == 2, "fixture: the canvas actually split into two panes");

    check(atelierPaneAt(panes, panes.pane[0].x + 5.0f, panes.pane[0].y + 5.0f) == 0,
          "a point inside pane 0 hits pane 0");
    check(atelierPaneAt(panes, panes.pane[1].x + 5.0f, panes.pane[1].y + 5.0f) == 1,
          "a point inside pane 1 hits pane 1");
    check(atelierPaneAt(panes, panes.divider.x + panes.divider.w * 0.5f,
                        panes.divider.y + 5.0f) == -1,
          "a point on the divider hits neither pane");
    check(atelierPaneAt(panes, -10.0f, -10.0f) == -1,
          "a point outside both rects hits neither pane");

    const AtelierPanes single = atelierSplitPanes(canvas, AtelierSplit::Single);
    check(atelierPaneAt(single, 10.0f, 10.0f) == 0 && atelierPaneAt(single, 790.0f, 590.0f) == 0,
          "a single, unsplit pane covers the whole canvas");
  }

  // -----------------------------------------------------------------------
  // 3. toggleSplitView -- the View > Split View menu item's real function
  // -----------------------------------------------------------------------
  std::printf("  -- 3. toggleSplitView refuses under two documents, else toggles --\n");
  {
    DocumentSession session;
    AtelierSplitState split;

    check(!toggleSplitView(session, split).empty() && split.mode == AtelierSplit::Single,
          "no open documents: refused, mode unchanged");

    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "A"));
    check(!toggleSplitView(session, split).empty() && split.mode == AtelierSplit::Single,
          "one open document: refused, mode unchanged");

    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "B"));
    check(toggleSplitView(session, split).empty() && split.mode == AtelierSplit::Columns,
          "two open documents: turns on, side by side");
    check(toggleSplitView(session, split).empty() && split.mode == AtelierSplit::Single,
          "pressed again: the way out is the way in, same as the tab strip's icons");
  }

  // -----------------------------------------------------------------------
  // 4. companionView resets when atelierPaneDocuments() hands the companion
  //    slot a genuinely different document
  // -----------------------------------------------------------------------
  std::printf("  -- 4. companionView resets when the companion document changes --\n");
  {
    DocumentSession session;
    AtelierSplitState split;
    split.mode = AtelierSplit::Columns;
    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "A"));
    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "B"));
    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "C"));

    AtelierPaneDocuments panes = atelierPaneDocuments(session, split);
    check(panes.count == 2, "fixture: split is active with a real companion");
    check(split.companionView.zoom <= 0.0f,
          "a companion new to the slot starts at the 'needs a fit' sentinel");

    // Simulate ui/MacPaintUI.cpp's render block having fit-and-centred it.
    split.companionView.zoom = 0.42f;
    split.companionView.panX = 7.0f;

    // Force a genuinely different companion by closing it. The repair rule
    // itself (a closed companion is replaced from tab order) is
    // DocumentResidency.cpp's own §2; what is asserted here is only the view.
    size_t companionIndex = session.count();
    for (size_t i = 0; i < session.count(); ++i)
      if (session.at(i) != nullptr && session.at(i)->id == split.companion) companionIndex = i;
    check(companionIndex < session.count(), "fixture: the companion is one of the open documents");
    std::string err;
    check(session.close(companionIndex, /*discardUnsavedChanges=*/true, &err),
          "closing the companion succeeds");

    panes = atelierPaneDocuments(session, split);
    check(panes.count == 2, "a replacement companion is found");
    check(split.companionView.zoom <= 0.0f,
          "the new companion's view is reset to the sentinel, not the old fit");
  }

  // -----------------------------------------------------------------------
  // 5. focusSplitPane -- the companion pane's click handler, driven for real
  // -----------------------------------------------------------------------
  //
  // This is also where "the first click on the unfocused pane only focuses"
  // lives, by construction rather than by a guard this test can trip: the
  // companion pane's `##focusPane2` button calls only this function
  // (ui/MacPaintUI.cpp), and nothing else in that pane's Begin()/End() block
  // reads a brush, a tool or a stroke -- there is no code path from that
  // click to a painted pixel for a test to catch reaching. What IS this
  // function's own contract, and what is asserted below, is that it moves
  // focus and swaps the two views.
  std::printf("  -- 5. focusSplitPane swaps the active document and the two views --\n");
  {
    DocumentSession session;
    AtelierSplitState split;
    split.mode = AtelierSplit::Columns;
    OpenDocument* a = session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "A"));
    OpenDocument* b = session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "B"));
    const AtelierPaneDocuments panes = atelierPaneDocuments(session, split);
    check(session.active() == b, "fixture: B is active, the one just added");

    CanvasView focusedView;
    focusedView.zoom = 3.0f;
    focusedView.panX = 11.0f;
    split.companionView.zoom = 0.5f;
    split.companionView.panX = -4.0f;

    focusSplitPane(session, split, focusedView, panes.focusedPane, b->id);
    check(session.active() == b && focusedView.zoom == 3.0f,
          "clicking the ALREADY-focused pane's own document is a no-op");

    const int otherPane = 1 - panes.focusedPane;
    focusSplitPane(session, split, focusedView, otherPane, a->id);
    check(session.active() == a, "clicking the companion pane makes its document active");
    check(split.focusedPane == otherPane, "the focused-pane index moves to where the click was");
    check(focusedView.zoom == 0.5f && focusedView.panX == -4.0f,
          "the focused view becomes what the companion pane's view was");
    check(split.companionView.zoom == 3.0f && split.companionView.panX == 11.0f,
          "the view that was focused becomes the new companion's");
    check(split.companion == b->id,
          "the document that lost focus is recorded as the companion");
  }

  std::printf("[selftest] SplitView %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
