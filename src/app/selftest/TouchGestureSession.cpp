#include "app/selftest/Support.hpp"

#include "app/TouchGestureSession.hpp"
#include "app/ViewTransform.hpp"

namespace np {

// item 4 ("trackpad interactions feel unnatural"): app/TouchGestureSession's
// "hold a gesture's own fixed baseline across frames" half -- the piece
// app/TouchGesture.hpp's own stateless `computeTwoTouchDelta()` selftest
// cannot exercise (see that file's own selftest, `runTouchGestureTest()`,
// for the pure per-frame math this builds on). Every case here verifies the
// RESULT by round-tripping through the real `ViewTransform` both the old
// and new views actually render with -- not by re-deriving the anchoring
// arithmetic a second time, the same posture app/selftest/ZoomAndSize.cpp's
// own `panForAnchoredZoomRotate()` cases (a2)(ii)/(iii) already take.
bool runTouchGestureSessionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  std::printf("[selftest] touch gesture session: gesture-start baseline held across frames\n");

  const Vec2 paintOrigin{0.0f, 0.0f};
  const Vec2 avail{800.0f, 600.0f};
  const Vec2 tex{400.0f, 300.0f};
  const Vec2 canvasCenter{tex.x * 0.5f, tex.y * 0.5f};
  const Vec2 cursorScreen{300.0f, 300.0f};  // off-centre: a trivial anchor-at-centre case
                                            // would leave pan at 0 for any zoom, masking bugs

  // ==========================================================================
  // (a) Rising edge: the very first frame two touches appear, `view` must be
  // left EXACTLY untouched -- there is no motion yet to apply, only a
  // baseline to remember. A bug that applied an "identity" recompute anyway
  // could still coincidentally leave zoom/rotation alone but perturb pan
  // through floating-point round-trip; this checks bit-for-bit unchanged
  // fields, not merely "close".
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    view.panX = 12.0f;
    view.panY = -7.0f;
    view.rotation = 0.2f;
    const CanvasView before = view;
    const TrackpadTouchPoint a{1, 0.4f, 0.5f};
    const TrackpadTouchPoint b{2, 0.6f, 0.5f};
    session.update(std::make_pair(a, b), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);
    check(view.zoom == before.zoom && view.panX == before.panX && view.panY == before.panY &&
              view.rotation == before.rotation,
          "rising edge: the frame two touches first appear, view is left exactly untouched -- "
          "only a baseline is snapshotted, nothing is applied yet");
  }

  // ==========================================================================
  // (b) A second frame with the SAME two touch positions (no motion since
  // the baseline): zoom and rotation must stay exactly what they started
  // at, and pan must stay exactly what it started at too -- an anchored
  // recompute that changes NOTHING about the touches must be a true no-op,
  // not merely "close to" the original pan.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    view.panX = 12.0f;
    view.panY = -7.0f;
    const TrackpadTouchPoint a{1, 0.4f, 0.5f};
    const TrackpadTouchPoint b{2, 0.6f, 0.5f};
    session.update(std::make_pair(a, b), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);  // rising edge
    session.update(std::make_pair(a, b), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);  // same positions again
    check(near(view.zoom, 1.0f, 1e-6f) && near(view.rotation, 0.0f, 1e-6f) &&
              near(view.panX, 12.0f, 1e-3f) && near(view.panY, -7.0f, 1e-3f),
          "no motion since the baseline: zoom, rotation and pan all stay exactly where the "
          "gesture started -- an anchored recompute of nothing is a true no-op");
  }

  // ==========================================================================
  // (c) Pure zoom (touches move directly apart, 2x, shared midpoint fixed),
  // anchored at a cursor OFF the canvas centre -- verified by ROUND-TRIP:
  // the canvas point under the cursor before the zoom must be the exact
  // same canvas point under the cursor after, read back through the real
  // `ViewTransform` both views render with.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    const CanvasView oldView = view;
    const float marginOldX = std::max(0.0f, (avail.x - tex.x * oldView.zoom) * 0.5f);
    const float marginOldY = std::max(0.0f, (avail.y - tex.y * oldView.zoom) * 0.5f);
    const Vec2 pivotOld{paintOrigin.x + marginOldX + oldView.panX + tex.x * oldView.zoom * 0.5f,
                        paintOrigin.y + marginOldY + oldView.panY + tex.y * oldView.zoom * 0.5f};
    const ViewTransform oldXform(oldView, canvasCenter, pivotOld);
    const Vec2 anchorCanvas = oldXform.toCanvas(cursorScreen);

    const TrackpadTouchPoint startA{1, 0.4f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.6f, 0.5f};  // 0.2 apart
    session.update(std::make_pair(startA, startB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);  // rising edge
    const TrackpadTouchPoint curA{1, 0.3f, 0.5f};
    const TrackpadTouchPoint curB{2, 0.7f, 0.5f};  // 0.4 apart: 2x
    session.update(std::make_pair(curA, curB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);

    check(near(view.zoom, 2.0f, 1e-3f), "pure touch zoom: view.zoom is exactly the touches' own "
                                        "2x distance ratio, from the gesture's fixed baseline");

    const float marginNewX = std::max(0.0f, (avail.x - tex.x * view.zoom) * 0.5f);
    const float marginNewY = std::max(0.0f, (avail.y - tex.y * view.zoom) * 0.5f);
    const Vec2 pivotNew{paintOrigin.x + marginNewX + view.panX + tex.x * view.zoom * 0.5f,
                        paintOrigin.y + marginNewY + view.panY + tex.y * view.zoom * 0.5f};
    const ViewTransform newXform(view, canvasCenter, pivotNew);
    const Vec2 anchorAfter = newXform.toScreen(anchorCanvas);
    check(near(anchorAfter.x, cursorScreen.x, 1e-2f) && near(anchorAfter.y, cursorScreen.y, 1e-2f),
          "pure touch zoom: the SAME canvas point stays exactly under the cursor after, "
          "round-tripped through the real ViewTransform -- the cursor-pin this replaces the "
          "old always-pivot-at-canvas-centre rotate behaviour with");
  }

  // ==========================================================================
  // (d) Falling edge then a fresh rising edge with a DIFFERENT touch pair
  // and a DIFFERENT view: the new gesture must re-baseline from what IS
  // there now, not drift from the old gesture's own remembered start -- a
  // stale `active_`/`startA_` left over from (c) would show up here as a
  // wrong zoom.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    const TrackpadTouchPoint startA{1, 0.4f, 0.5f};
    const TrackpadTouchPoint startB{2, 0.6f, 0.5f};
    session.update(std::make_pair(startA, startB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);
    const TrackpadTouchPoint curA{1, 0.3f, 0.5f};
    const TrackpadTouchPoint curB{2, 0.7f, 0.5f};
    session.update(std::make_pair(curA, curB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);  // view.zoom is now 2.0

    session.update(std::nullopt, view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);  // falling edge: fingers lifted
    check(near(view.zoom, 2.0f, 1e-3f),
          "falling edge: lifting the fingers leaves the view exactly where the last frame put "
          "it -- releasing is not itself a change");

    // A brand-new gesture, different touch pair, starting from the view the
    // PREVIOUS gesture left behind (2.0x) -- this is the realistic
    // "zoom in, let go, zoom in again" sequence.
    const TrackpadTouchPoint newStartA{3, 0.2f, 0.2f};
    const TrackpadTouchPoint newStartB{4, 0.2f, 0.6f};  // 0.4 apart
    session.update(std::make_pair(newStartA, newStartB), view, canvasCenter, paintOrigin, avail,
                    tex, cursorScreen);
    check(near(view.zoom, 2.0f, 1e-3f),
          "fresh rising edge: view is untouched on the baseline-snapshot frame regardless of "
          "which gesture came before -- same invariant as case (a)");
    const TrackpadTouchPoint newCurA{3, 0.2f, 0.3f};
    const TrackpadTouchPoint newCurB{4, 0.2f, 0.5f};  // 0.2 apart: HALF the new baseline
    session.update(std::make_pair(newCurA, newCurB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);
    check(near(view.zoom, 1.0f, 1e-3f),
          "fresh rising edge re-baselines from the NEW touch pair's own start, not the old "
          "gesture's -- 0.5x of the view the previous gesture left at 2.0x is 1.0x, not a "
          "value computed against the stale 0.4-apart baseline case (c) used");
  }

  // ==========================================================================
  // (e) `lastDelta()` exposes the touch pair's own raw pan (`panDx`/
  // `panDy`), independently of whatever `update()` wrote into `view` --
  // the caller (ui/MacPaintUI.cpp) reads this to add the touch-drag
  // translation on top, in screen points, after applying `deviceSize` and
  // the natural/traditional scrolling preference (this class's own doc
  // comment on why that conversion does not belong here).
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    const TrackpadTouchPoint startA{1, 0.3f, 0.3f};
    const TrackpadTouchPoint startB{2, 0.5f, 0.4f};
    session.update(std::make_pair(startA, startB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);
    check(near(session.lastDelta().panDx, 0.0f, 1e-6f) &&
              near(session.lastDelta().panDy, 0.0f, 1e-6f),
          "lastDelta() on the baseline-snapshot frame itself is the identity delta -- there is "
          "no motion yet to report");
    const TrackpadTouchPoint curA{1, 0.1f, 0.5f};   // both shifted by (-0.2, +0.2)
    const TrackpadTouchPoint curB{2, 0.3f, 0.6f};
    session.update(std::make_pair(curA, curB), view, canvasCenter, paintOrigin, avail, tex,
                    cursorScreen);
    check(near(session.lastDelta().panDx, -0.2f, 1e-4f) &&
              near(session.lastDelta().panDy, 0.2f, 1e-4f),
          "lastDelta() reports the touch pair's own exact shared-midpoint shift for the "
          "caller to convert and add -- app/TouchGesture.hpp's own pure math, unmodified");
  }

  return ok;
}

}  // namespace np
