#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "app/AppState.hpp"
#include "app/SplitView.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"

// View > Split View / Match Zoom.
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
  // 1. splitPaneOrigin / matchZoomView -- pure, GPU-free arithmetic
  // -----------------------------------------------------------------------
  //
  // The pane-centre fraction below is derived by INVERTING the real
  // placement function, `splitPaneOrigin()`, not by restating a closed-form
  // formula in the test -- a prior version of this section hardcoded
  // `0.5 - pan/(zoom*docSize)`, which is only the pane-centre point while
  // the document fits inside its pane (`splitPaneOrigin()`'s margin > 0).
  // Once the document is zoomed past its pane -- margin == 0, the ordinary
  // zoomed-in case -- that formula is simply wrong, and `matchZoomView()`
  // inherited the same mistake because it never saw either pane's size.
  // Calling the shared function here means this check is blind to which
  // case applies, exactly like the production code it verifies.
  std::printf("  -- 1. splitPaneOrigin / matchZoomView --\n");
  {
    auto centreFraction = [](Vec2 paneSize, float docW, float docH, const CanvasView& view) {
      const Vec2 origin = splitPaneOrigin(Vec2{0.0f, 0.0f}, paneSize, docW, docH, view);
      const float dx = (paneSize.x * 0.5f - origin.x) / view.zoom;
      const float dy = (paneSize.y * 0.5f - origin.y) / view.zoom;
      return Vec2{dx / docW, dy / docH};
    };

    // 1a. splitPaneOrigin() itself: centred when the document fits, offset
    // by pan from `paneOrigin` alone (margin == 0) once it does not.
    {
      CanvasView fits;
      fits.zoom = 1.0f;
      const Vec2 o = splitPaneOrigin(Vec2{10.0f, 20.0f}, Vec2{800.0f, 600.0f}, 200.0f, 100.0f, fits);
      check(o.x == 10.0f + (800.0f - 200.0f) * 0.5f && o.y == 20.0f + (600.0f - 100.0f) * 0.5f,
            "a document smaller than its pane is centred, unpanned");

      CanvasView zoomedIn;
      zoomedIn.zoom = 3.0f;
      zoomedIn.panX = -15.0f;
      zoomedIn.panY = 5.0f;
      check(zoomedIn.zoom * 200.0f > 150.0f && zoomedIn.zoom * 100.0f > 100.0f,
            "fixture: this document at this zoom exceeds its (smaller) pane");
      const Vec2 o2 =
          splitPaneOrigin(Vec2{10.0f, 20.0f}, Vec2{150.0f, 100.0f}, 200.0f, 100.0f, zoomedIn);
      check(o2.x == 10.0f + zoomedIn.panX && o2.y == 20.0f + zoomedIn.panY,
            "a document larger than its pane has no margin -- pan is the whole offset");
    }

    CanvasView src;
    src.zoom = 2.0f;
    src.panX = 30.0f;
    src.panY = -20.0f;
    src.rotation = 0.7f;
    src.mirrorX = true;

    // 1b. Identity: same pane size AND same document size on both sides.
    // Tolerance rather than `==`: the general mapping now divides by
    // `zoom*docW` and multiplies back, which the old ratio-of-1.0 shortcut
    // never had to do, so exactness is not guaranteed even here.
    const Vec2 samePane{800.0f, 600.0f};
    const CanvasView same = matchZoomView(src, samePane, 800.0f, 600.0f, samePane, 800.0f, 600.0f);
    check(same.zoom == src.zoom && std::fabs(same.panX - src.panX) < 1e-3f &&
              std::fabs(same.panY - src.panY) < 1e-3f,
          "identical pane and document on both sides: pan/zoom pass through");

    // 1c. Both documents fit their panes (the case the old, pane-blind
    // formula happened to get right).
    {
      const Vec2 srcPane{800.0f, 600.0f}, dstPane{500.0f, 500.0f};
      const float srcW = 200.0f, srcH = 150.0f, dstW = 100.0f, dstH = 200.0f;
      check(src.zoom * srcW <= srcPane.x && src.zoom * srcH <= srcPane.y &&
                src.zoom * dstW <= dstPane.x && src.zoom * dstH <= dstPane.y,
            "fixture 1c: both documents fit their panes at this zoom");
      const CanvasView mapped = matchZoomView(src, srcPane, srcW, srcH, dstPane, dstW, dstH);
      check(mapped.zoom == src.zoom, "the same zoom factor, not rescaled");
      const Vec2 srcFrac = centreFraction(srcPane, srcW, srcH, src);
      const Vec2 dstFrac = centreFraction(dstPane, dstW, dstH, mapped);
      check(std::fabs(srcFrac.x - dstFrac.x) < 1e-4f && std::fabs(srcFrac.y - dstFrac.y) < 1e-4f,
            "1c: the pane-centre document fraction is equal on both sides (both fit)");
    }

    // 1d. Both documents are zoomed PAST their panes, different sizes --
    // the case the old formula silently mishandled.
    {
      CanvasView zoomedSrc = src;
      zoomedSrc.zoom = 2.0f;
      const Vec2 srcPane{300.0f, 200.0f}, dstPane{250.0f, 150.0f};
      const float srcW = 1000.0f, srcH = 800.0f, dstW = 600.0f, dstH = 900.0f;
      check(srcW != dstW && srcH != dstH, "fixture 1d: the two documents differ in both dimensions");
      check(zoomedSrc.zoom * srcW > srcPane.x && zoomedSrc.zoom * srcH > srcPane.y &&
                zoomedSrc.zoom * dstW > dstPane.x && zoomedSrc.zoom * dstH > dstPane.y,
            "fixture 1d: both documents exceed their panes at this zoom");
      const CanvasView mapped = matchZoomView(zoomedSrc, srcPane, srcW, srcH, dstPane, dstW, dstH);
      check(mapped.zoom == zoomedSrc.zoom, "1d: the same zoom factor, not rescaled");
      const Vec2 srcFrac = centreFraction(srcPane, srcW, srcH, zoomedSrc);
      const Vec2 dstFrac = centreFraction(dstPane, dstW, dstH, mapped);
      check(std::fabs(srcFrac.x - dstFrac.x) < 1e-4f && std::fabs(srcFrac.y - dstFrac.y) < 1e-4f,
            "1d: the pane-centre document fraction is equal on both sides (both zoomed in)");
    }

    // 1e. One document fits its pane, the other does not -- the mixed case.
    {
      CanvasView mixedSrc = src;
      mixedSrc.zoom = 1.0f;
      const Vec2 srcPane{800.0f, 600.0f}, dstPane{300.0f, 200.0f};
      const float srcW = 200.0f, srcH = 150.0f, dstW = 1000.0f, dstH = 800.0f;
      check(mixedSrc.zoom * srcW <= srcPane.x && mixedSrc.zoom * srcH <= srcPane.y,
            "fixture 1e: the source document fits its pane");
      check(mixedSrc.zoom * dstW > dstPane.x && mixedSrc.zoom * dstH > dstPane.y,
            "fixture 1e: the destination document does not fit its pane");
      const CanvasView mapped = matchZoomView(mixedSrc, srcPane, srcW, srcH, dstPane, dstW, dstH);
      const Vec2 srcFrac = centreFraction(srcPane, srcW, srcH, mixedSrc);
      const Vec2 dstFrac = centreFraction(dstPane, dstW, dstH, mapped);
      check(std::fabs(srcFrac.x - dstFrac.x) < 1e-4f && std::fabs(srcFrac.y - dstFrac.y) < 1e-4f,
            "1e: the pane-centre document fraction is equal on both sides (fits vs. does not)");
    }

    // 1f. Rotation/mirror: the companion never draws rotated or mirrored
    // (its own quad is always axis-aligned), so these are simply carried
    // over from `source` rather than transformed.
    {
      const Vec2 pane{800.0f, 600.0f};
      const CanvasView mapped = matchZoomView(src, pane, 200.0f, 150.0f, pane, 300.0f, 250.0f);
      check(mapped.rotation == src.rotation && mapped.mirrorX == src.mirrorX &&
                mapped.mirrorY == src.mirrorY,
            "every other CanvasView field is copied from source, untouched");
    }

    // 1g. Degenerate guards: no document extent, and no zoom.
    {
      const Vec2 pane{800.0f, 600.0f};
      const CanvasView guarded = matchZoomView(src, pane, 0.0f, 100.0f, pane, 50.0f, 50.0f);
      check(guarded.zoom == src.zoom && guarded.panX == src.panX && guarded.panY == src.panY,
            "a zero source dimension returns source unchanged");
      CanvasView noZoom = src;
      noZoom.zoom = 0.0f;
      const CanvasView guarded2 = matchZoomView(noZoom, pane, 100.0f, 100.0f, pane, 50.0f, 50.0f);
      check(guarded2.panX == noZoom.panX && guarded2.panY == noZoom.panY,
            "a zero zoom returns source unchanged rather than dividing by it");
    }
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

  std::printf("  -- 6. companionViewForFrame picks the companion pane's view each frame --\n");
  {
    const Vec2 focusedPane{400.0f, 300.0f};
    const Vec2 companionPane{400.0f, 300.0f};
    CanvasView focused;
    focused.zoom = 2.0f;
    focused.panX = -150.0f;
    focused.panY = -40.0f;
    CanvasView kept;
    kept.zoom = 0.75f;
    kept.panX = 12.0f;
    kept.panY = -7.0f;
    CanvasView sentinel;
    sentinel.zoom = 0.0f;

    const CanvasView matched =
        matchZoomView(focused, focusedPane, 800.0f, 600.0f, companionPane, 500.0f, 900.0f);
    check(matched.zoom != kept.zoom || matched.panX != kept.panX || matched.panY != kept.panY,
          "fixture: the match-zoom result differs from the companion's kept view");
    const CanvasView on = companionViewForFrame(kept, true, focused, focusedPane, 800.0f, 600.0f,
                                                companionPane, 500.0f, 900.0f);
    check(on.zoom == matched.zoom && on.panX == matched.panX && on.panY == matched.panY,
          "Match Zoom on: the companion follows the focused pane over its own view");

    const CanvasView off = companionViewForFrame(kept, false, focused, focusedPane, 800.0f,
                                                 600.0f, companionPane, 500.0f, 900.0f);
    check(off.zoom == kept.zoom && off.panX == kept.panX && off.panY == kept.panY,
          "Match Zoom off: a companion view that is already set is kept");

    const CanvasView fitted = companionViewForFrame(sentinel, false, focused, focusedPane, 800.0f,
                                                    600.0f, companionPane, 500.0f, 900.0f);
    const float expectFit = std::min((400.0f - 48.0f) / 500.0f, (300.0f - 48.0f) / 900.0f);
    check(expectFit != (300.0f / 900.0f),
          "fixture: the inset changes the fit, so a fit without it would be caught");
    check(fitted.zoom == expectFit && fitted.panX == 0.0f && fitted.panY == 0.0f,
          "Match Zoom off, needs a fit: fitted inside a 24 px inset and centred");
  }

  std::printf("[selftest] SplitView %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
