#include "app/selftest/Support.hpp"

#include "app/TouchGestureSession.hpp"
#include "app/ViewTransform.hpp"

namespace np {

namespace {

// The layout the whole suite works in. `paintOrigin` at the origin and an
// `avail` larger than `tex` keep the margin terms non-zero, so a solve that
// forgot them would show up rather than cancel.
constexpr float kAvailX = 800.0f, kAvailY = 600.0f;
constexpr float kTexX = 400.0f, kTexY = 300.0f;

// The screen position of the canvas's own centre under `view` -- the same
// algebra `TouchGestureSession` and `panForAnchoredZoomRotateTo()` both invert,
// spelled once here so the assertions round-trip through the REAL
// `ViewTransform` rather than through a second copy of the anchoring maths.
Vec2 pivotFor(const CanvasView& view, Vec2 paintOrigin, Vec2 avail, Vec2 tex) {
  const float drawX = tex.x * view.zoom;
  const float drawY = tex.y * view.zoom;
  const float marginX = std::max(0.0f, (avail.x - drawX) * 0.5f);
  const float marginY = std::max(0.0f, (avail.y - drawY) * 0.5f);
  return Vec2{paintOrigin.x + marginX + view.panX + drawX * 0.5f,
              paintOrigin.y + marginY + view.panY + drawY * 0.5f};
}

}  // namespace

// app/TouchGestureSession's "hold a gesture across frames" half -- now a CHAIN
// OF ONE-FRAME ANCHOR SOLVES rather than a fixed gesture-start baseline (see
// that header for why the baseline model was replaced). The invariant this
// suite exists to pin is the one the feature is actually asking for: the canvas
// point under the fingers stays under the fingers, whatever combination of
// pinch, twist and slide they are doing.
//
// Every case verifies through the real `ViewTransform` both views render with,
// never by re-deriving the anchoring arithmetic a second time -- the same
// posture app/selftest/ZoomAndSize.cpp's own cases take.
bool runTouchGestureSessionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  std::printf("[selftest] touch gesture session: per-frame anchor solve\n");

  const Vec2 paintOrigin{0.0f, 0.0f};
  const Vec2 avail{kAvailX, kAvailY};
  const Vec2 tex{kTexX, kTexY};
  const Vec2 canvasCenter{tex.x * 0.5f, tex.y * 0.5f};
  // Off-centre on purpose: an anchor at the canvas centre would leave pan at 0
  // for any zoom, masking a solve that ignored the anchor entirely.
  const Vec2 cursorScreen{300.0f, 300.0f};

  // Touch points are in SCREEN units now (the caller converts from the
  // device's normalised units before calling -- see the header).
  auto pt = [](uint64_t id, float x, float y) { return TrackpadTouchPoint{id, x, y}; };

  // ==========================================================================
  // (a) Rising edge: the first frame two touches appear must leave `view`
  // EXACTLY untouched -- there is no previous frame to solve against, so there
  // is nothing to apply. Bit-for-bit, not merely "close": a solve that ran an
  // identity round-trip anyway would perturb pan by floating-point noise.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    view.panX = 12.0f;
    view.panY = -7.0f;
    view.rotation = 0.2f;
    const CanvasView before = view;
    session.update(std::make_pair(pt(1, 320.0f, 300.0f), pt(2, 480.0f, 300.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    check(view.zoom == before.zoom && view.panX == before.panX && view.panY == before.panY &&
              view.rotation == before.rotation,
          "rising edge: the frame two touches first appear leaves view exactly untouched");
  }

  // ==========================================================================
  // (b) **The regression this model was written for.** Fingers held perfectly
  // still for 300 frames: the view must not move AT ALL. The previous model
  // added `TwoTouchDelta::panDx/panDy` -- a SINCE-GESTURE-START quantity --
  // with `+=` every frame, so a held gesture kept sliding; that is the
  // "swimmy, uncontrollable" feel. Asserted against the view as it was before
  // the held frames, so any per-frame creep accumulates into a visible failure
  // rather than hiding under a per-frame tolerance.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.3f;
    view.panX = 12.0f;
    view.panY = -7.0f;
    view.rotation = 0.35f;
    const auto a = pt(1, 300.0f, 280.0f);
    const auto b = pt(2, 460.0f, 340.0f);
    session.update(std::make_pair(a, b), view, canvasCenter, paintOrigin, avail, tex, cursorScreen,
                   TouchAnchor::TouchMidpoint);
    const CanvasView held = view;
    for (int i = 0; i < 300; ++i) {
      session.update(std::make_pair(a, b), view, canvasCenter, paintOrigin, avail, tex,
                     cursorScreen, TouchAnchor::TouchMidpoint);
    }
    check(near(view.panX, held.panX, 1e-2f) && near(view.panY, held.panY, 1e-2f) &&
              near(view.zoom, held.zoom, 1e-4f) && near(view.rotation, held.rotation, 1e-4f),
          "fingers held still for 300 frames: the view does not drift by even a pixel -- the "
          "compounding pan the old model re-applied every frame");
  }

  // ==========================================================================
  // (c) **The feature itself**, over 200 frames of arbitrary combined motion:
  // each frame scales, rotates AND translates the touch pair at once, and the
  // canvas point under the PREVIOUS midpoint must land exactly under the
  // CURRENT midpoint. Read back through the real `ViewTransform`. This is the
  // "no matter which of the three the user is actually doing" claim, asserted
  // rather than asserted-about.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    view.panX = 5.0f;
    view.panY = -3.0f;
    view.rotation = 0.1f;

    auto ptA = pt(1, 300.0f, 260.0f);
    auto ptB = pt(2, 420.0f, 330.0f);
    session.update(std::make_pair(ptA, ptB), view, canvasCenter, paintOrigin, avail, tex,
                   cursorScreen, TouchAnchor::TouchMidpoint);

    float worst = 0.0f;
    bool zoomInRange = true;
    for (int i = 0; i < 200; ++i) {
      // A deliberately awkward mix: the scale factor oscillates either side of
      // 1, the rotation per frame is not a round number, and the translation
      // is not aligned to either axis.
      const float t = static_cast<float>(i);
      const float s = 1.0f + 0.03f * std::sin(t * 0.37f);
      const float dTheta = 0.021f * std::cos(t * 0.23f);
      const Vec2 slide{0.9f * std::sin(t * 0.11f), -0.7f * std::cos(t * 0.17f)};

      const Vec2 mid{(ptA.x + ptB.x) * 0.5f, (ptA.y + ptB.y) * 0.5f};
      auto moved = [&](TrackpadTouchPoint p) {
        const float rx = p.x - mid.x, ry = p.y - mid.y;
        const float c = std::cos(dTheta), sn = std::sin(dTheta);
        p.x = mid.x + (rx * c - ry * sn) * s + slide.x;
        p.y = mid.y + (rx * sn + ry * c) * s + slide.y;
        return p;
      };
      const TrackpadTouchPoint prevA = ptA, prevB = ptB;
      ptA = moved(ptA);
      ptB = moved(ptB);

      // The canvas point under the previous midpoint, in the view as it stands
      // BEFORE this frame's solve.
      const Vec2 prevMid{(prevA.x + prevB.x) * 0.5f, (prevA.y + prevB.y) * 0.5f};
      const ViewTransform beforeXform(view, canvasCenter, pivotFor(view, paintOrigin, avail, tex));
      const Vec2 anchorCanvas = beforeXform.toCanvas(prevMid);

      session.update(std::make_pair(ptA, ptB), view, canvasCenter, paintOrigin, avail, tex,
                     cursorScreen, TouchAnchor::TouchMidpoint);

      const Vec2 curMid{(ptA.x + ptB.x) * 0.5f, (ptA.y + ptB.y) * 0.5f};
      const ViewTransform afterXform(view, canvasCenter, pivotFor(view, paintOrigin, avail, tex));
      const Vec2 landed = afterXform.toScreen(anchorCanvas);
      worst = std::max(worst, std::max(std::fabs(landed.x - curMid.x),
                                       std::fabs(landed.y - curMid.y)));
      if (view.zoom < 0.1f || view.zoom > 8.0f) zoomInRange = false;
    }
    check(worst <= 0.05f,
          "200 frames of combined pinch+twist+slide: the canvas point under the fingers stays "
          "under the fingers every single frame");
    check(zoomInRange, "...and zoom never leaves its clamp across that run");
  }

  // ==========================================================================
  // (d) Pure translation, with the view already rotated: sliding both fingers
  // by the same screen vector must move the view by EXACTLY that vector. A
  // solve that applied the translation in canvas space instead of screen space
  // would pass at rotation 0 and fail here, which is why the view is turned.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.7f;
    view.rotation = 0.6f;
    view.panX = 4.0f;
    view.panY = 9.0f;
    session.update(std::make_pair(pt(1, 300.0f, 280.0f), pt(2, 460.0f, 340.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    const CanvasView before = view;
    session.update(std::make_pair(pt(1, 317.0f, 291.0f), pt(2, 477.0f, 351.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    check(near(view.panX, before.panX + 17.0f, 1e-2f) &&
              near(view.panY, before.panY + 11.0f, 1e-2f) &&
              near(view.zoom, before.zoom, 1e-4f) && near(view.rotation, before.rotation, 1e-4f),
          "pure slide under a rotated view: pan moves by exactly the fingers' screen travel, "
          "and neither zoom nor rotation is disturbed");
  }

  // ==========================================================================
  // (e) The trackpad branch. An indirect device has no honest absolute
  // midpoint, so the anchor is the cursor and the midpoint's travel carries
  // it; `panSign` is the natural/traditional scrolling preference. Both signs
  // are exercised, because this is a plain runtime argument precisely so the
  // shipped shape is the tested one.
  // ==========================================================================
  {
    for (const float sign : {1.0f, -1.0f}) {
      TouchGestureSession session;
      CanvasView view;
      view.zoom = 1.0f;
      session.update(std::make_pair(pt(1, 300.0f, 280.0f), pt(2, 460.0f, 340.0f)), view,
                     canvasCenter, paintOrigin, avail, tex, cursorScreen, TouchAnchor::Cursor,
                     sign);
      const CanvasView before = view;
      session.update(std::make_pair(pt(1, 310.0f, 280.0f), pt(2, 470.0f, 340.0f)), view,
                     canvasCenter, paintOrigin, avail, tex, cursorScreen, TouchAnchor::Cursor,
                     sign);
      check(near(view.panX, before.panX + 10.0f * sign, 1e-2f) &&
                near(view.panY, before.panY, 1e-2f),
            sign > 0.0f ? "trackpad, natural scrolling: the view follows the fingers"
                        : "trackpad, traditional scrolling: the view opposes the fingers");
    }
  }

  // ==========================================================================
  // (f) A zoom that CLAMPS must still leave the anchor pinned -- the pan is
  // solved against the clamped view, not the one the fingers asked for, so a
  // solve that used the unclamped zoom would slide the drawing out from under
  // the fingers exactly at the limit.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 7.9f;
    session.update(std::make_pair(pt(1, 300.0f, 300.0f), pt(2, 400.0f, 300.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    const Vec2 prevMid{350.0f, 300.0f};
    const ViewTransform beforeXform(view, canvasCenter, pivotFor(view, paintOrigin, avail, tex));
    const Vec2 anchorCanvas = beforeXform.toCanvas(prevMid);
    // Fingers spread far enough to demand well past the 8.0 limit.
    session.update(std::make_pair(pt(1, 150.0f, 300.0f), pt(2, 550.0f, 300.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    const ViewTransform afterXform(view, canvasCenter, pivotFor(view, paintOrigin, avail, tex));
    const Vec2 landed = afterXform.toScreen(anchorCanvas);
    check(near(view.zoom, 8.0f, 1e-4f) && near(landed.x, 350.0f, 0.05f) &&
              near(landed.y, 300.0f, 0.05f),
          "a pinch past the zoom limit clamps AND keeps the anchor pinned -- the pan is solved "
          "against the clamped view, not the requested one");
  }

  // ==========================================================================
  // (g) Falling edge, then a fresh gesture: lifting must not itself change the
  // view, and the next gesture must solve against the view as it now is rather
  // than resuming from anything the previous one remembered.
  // ==========================================================================
  {
    TouchGestureSession session;
    CanvasView view;
    view.zoom = 1.0f;
    session.update(std::make_pair(pt(1, 320.0f, 300.0f), pt(2, 480.0f, 300.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    session.update(std::make_pair(pt(1, 240.0f, 300.0f), pt(2, 560.0f, 300.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    check(near(view.zoom, 2.0f, 1e-3f), "a 2x spread zooms 2x");
    const CanvasView afterFirst = view;

    session.update(std::nullopt, view, canvasCenter, paintOrigin, avail, tex, cursorScreen);
    check(view.zoom == afterFirst.zoom && view.panX == afterFirst.panX &&
              view.panY == afterFirst.panY,
          "falling edge: lifting the fingers is not itself a change to the view");
    check(!session.active(), "...and the session reports itself inactive afterwards");

    // A new pair, at a different separation. Its FIRST frame must apply
    // nothing (it is a rising edge), and its second must scale relative to its
    // own previous frame -- halving here, from the 2.0x the last gesture left.
    session.update(std::make_pair(pt(3, 160.0f, 160.0f), pt(4, 160.0f, 480.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    check(near(view.zoom, 2.0f, 1e-3f), "fresh rising edge applies nothing, same as case (a)");
    session.update(std::make_pair(pt(3, 160.0f, 240.0f), pt(4, 160.0f, 400.0f)), view, canvasCenter,
                   paintOrigin, avail, tex, cursorScreen, TouchAnchor::TouchMidpoint);
    check(near(view.zoom, 1.0f, 1e-3f),
          "the new gesture scales from the view it found (2.0x halved is 1.0x), with nothing "
          "carried over from the previous gesture");
  }

  return ok;
}

}  // namespace np
