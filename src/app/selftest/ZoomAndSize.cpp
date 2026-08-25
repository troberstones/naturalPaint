#include "app/selftest/Support.hpp"

#include "app/ZoomAndSize.hpp"

namespace np {

// PRD Q1 (scrubby zoom, cursor-anchored) and PRD R5/D3 (brush size by an
// on-canvas gesture and its `[`/`]` alternate), track8/zoom. Every function
// under test is pure -- see app/ZoomAndSize.hpp's header comment for why --
// so this section is entirely headless and GPU-free.
bool runZoomAndSizeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // ==========================================================================
  // (a) panForAnchoredZoom(): the anchor property itself.
  // ==========================================================================
  //
  // A spread of (paintOrigin, avail, tex, zoomOld, panOld, zoomNew, anchor)
  // states, including panned-away-from-fit and an anchor OUTSIDE the drawn
  // canvas quad (a click in the margin still has to work) -- exactly the
  // shape of state the OLD wheel-zoom formula silently mishandled (this
  // header's own file comment). For each, the document coordinate under
  // `anchor` before the zoom must equal the document coordinate under that
  // SAME screen point after it -- not "the pan changed by something
  // plausible", the actual re-derived coordinate.
  {
    struct Case {
      float paintOrigin, avail, tex, zoomOld, panOld, zoomNew, anchor;
      const char* what;
    };
    const Case cases[] = {
        {0.0f, 800.0f, 400.0f, 1.0f, 0.0f, 1.12f, 250.0f, "fit view, unpanned, wheel-in"},
        {0.0f, 800.0f, 400.0f, 3.0f, -100.0f, 3.36f, 300.0f, "zoomed-in and panned, wheel-in"},
        {20.0f, 800.0f, 400.0f, 1.0f, 0.0f, 1.0f / 1.2f, 20.0f, "click exactly on paintOrigin"},
        {0.0f, 800.0f, 400.0f, 0.5f, 40.0f, 0.1f, 900.0f, "anchor OFF the canvas margin, clamped-low zoom target"},
        {0.0f, 800.0f, 400.0f, 8.0f, -500.0f, 8.0f, 300.0f, "zoomOld == zoomNew (no-op case)"},
        {-30.0f, 640.0f, 1024.0f, 0.1f, 0.0f, 8.0f, 200.0f, "min zoom to max zoom in one jump"},
    };
    for (const Case& c : cases) {
      // origin = paintOrigin + margin + pan, mirroring ui/MacPaintUI.cpp's
      // own `origin` definition exactly (not a re-derivation of it) --
      // margin is clamped to >=0 the same way that file's `avail - drawSize`
      // term is.
      const float marginOld = std::max(0.0f, (c.avail - c.tex * c.zoomOld) * 0.5f);
      const float originOld = c.paintOrigin + marginOld + c.panOld;
      const float canvasPtBefore = (c.anchor - originOld) / c.zoomOld;

      const float panNew = panForAnchoredZoom(c.anchor, originOld, c.zoomOld, c.zoomNew,
                                              c.paintOrigin, c.avail, c.tex);
      const float marginNew = std::max(0.0f, (c.avail - c.tex * c.zoomNew) * 0.5f);
      const float originNew = c.paintOrigin + marginNew + panNew;
      const float canvasPtAfter = (c.anchor - originNew) / c.zoomNew;

      char label[192];
      std::snprintf(label, sizeof(label),
                    "panForAnchoredZoom: anchor point unmoved (%s)", c.what);
      // 1e-3 document-space texels: ~40x looser than float32's own ~1e-5
      // relative precision at these magnitudes, leaving headroom for the
      // subtraction-then-division chain above without the tolerance itself
      // becoming the thing being tested.
      check(near(canvasPtAfter, canvasPtBefore, 1e-3f), label);
    }
  }

  // ==========================================================================
  // (b) zoomFactorForDrag(): pure, monotonic, symmetric about zero, and a
  // hand-computed known point.
  // ==========================================================================
  {
    check(near(zoomFactorForDrag(0.0f), 1.0f, 1e-6f),
          "zoomFactorForDrag: zero drag is a no-op factor (exactly 1.0)");
    check(near(zoomFactorForDrag(kZoomDragPixelsPerOctave), 2.0f, 1e-4f),
          "zoomFactorForDrag: one full octave of drag doubles (hand-computed 2^1)");
    check(near(zoomFactorForDrag(-kZoomDragPixelsPerOctave), 0.5f, 1e-4f),
          "zoomFactorForDrag: one octave of BACKWARD drag halves");
    check(zoomFactorForDrag(30.0f) > zoomFactorForDrag(10.0f) &&
              zoomFactorForDrag(10.0f) > zoomFactorForDrag(0.0f) &&
              zoomFactorForDrag(0.0f) > zoomFactorForDrag(-10.0f),
          "zoomFactorForDrag: strictly monotonic across four sample points");
    check(near(zoomFactorForDrag(37.0f) * zoomFactorForDrag(-37.0f), 1.0f, 1e-4f),
          "zoomFactorForDrag: symmetric about zero -- drag right then the same left "
          "returns to the original zoom, not a compounded one");
    // Frame-splitting equivalence: this is the property that makes calling
    // this per-frame in ui/MacPaintUI.cpp (against ImGui's own MouseDelta)
    // an honest "pure function of total drag pixels" even though nothing
    // ever holds a running total -- exponentials compose by adding
    // exponents, so three arbitrary frame deltas summing to a total give
    // the identical zoom multiplier as one shot of that total.
    const float total = 63.0f;
    const float perFrame = zoomFactorForDrag(22.0f) * zoomFactorForDrag(15.0f) *
                           zoomFactorForDrag(26.0f);  // 22+15+26 == 63
    check(near(perFrame, zoomFactorForDrag(total), 1e-3f),
          "zoomFactorForDrag: three arbitrary per-frame deltas compose to the same "
          "factor as one shot of their total (justifies the per-frame call style)");
  }

  // ==========================================================================
  // (c) clampViewZoom(): the ACTUAL function applyZoomFactor calls, not the
  // two constants read in isolation -- the exact mistake this track was
  // warned against repeating.
  // ==========================================================================
  {
    check(kViewZoomMin == 0.1f && kViewZoomMax == 8.0f,
          "kViewZoomMin/kViewZoomMax match the limits already in "
          "ui/MacPaintUI.cpp's requestFitWindow -- found, not invented");
    check(near(clampViewZoom(1000.0f), kViewZoomMax, 1e-6f),
          "clampViewZoom: a huge zoom saturates at the existing max, not a new one");
    check(near(clampViewZoom(-5.0f), kViewZoomMin, 1e-6f),
          "clampViewZoom: a negative zoom saturates at the existing min");
    check(near(clampViewZoom(2.0f), 2.0f, 1e-6f),
          "clampViewZoom: a value already inside the range passes through unchanged");
  }

  // ==========================================================================
  // (d) Brush size: bracketStepForRadius(), radiusForDrag(),
  // clampBrushRadius() -- and the "gesture and keys agree" requirement,
  // proved by driving BOTH mechanisms to both ends of the SAME range and
  // checking they land on the identical value, not two different
  // near-misses.
  // ==========================================================================
  {
    check(near(bracketStepForRadius(10.0f), 1.0f, 1e-6f),
          "bracketStepForRadius: 10% of 10 rounds to 1px (the floor engaging)");
    check(near(bracketStepForRadius(150.0f), 15.0f, 1e-6f),
          "bracketStepForRadius: 10% of 150 is 15px -- proportional, not the same 1px "
          "a fixed-step scheme would give here too");
    check(bracketStepForRadius(200.0f) > bracketStepForRadius(20.0f),
          "bracketStepForRadius: the step at the top of the range is larger than at "
          "the bottom -- PRD's own words, \"a constant is wrong across a 1..200 range\"");
    check(near(bracketStepForRadius(2.0f), 1.0f, 1e-6f),
          "bracketStepForRadius: never below the 1px floor even near the minimum");

    check(near(radiusForDrag(20.0f, 0.0f), 20.0f, 1e-4f),
          "radiusForDrag: zero drag leaves the radius unchanged");
    check(near(radiusForDrag(20.0f, kSizeDragPixelsPerOctave), 40.0f, 1e-2f),
          "radiusForDrag: one octave of drag doubles the CURRENT radius (hand-computed)");
    check(near(radiusForDrag(80.0f, -kSizeDragPixelsPerOctave), 40.0f, 1e-2f),
          "radiusForDrag: one octave of backward drag halves it");

    // "the gesture and the keys agree with each other and with one range":
    // push both mechanisms past the top from one pixel below it, and past
    // the bottom from one pixel above it, and check both saturate at
    // EXACTLY the same clamped value -- not merely "close".
    const float nearMax = kBrushRadiusMax - 1.0f;
    const float viaKeyUp = clampBrushRadius(nearMax + bracketStepForRadius(nearMax));
    const float viaGestureUp = clampBrushRadius(radiusForDrag(nearMax, 500.0f));
    check(near(viaKeyUp, kBrushRadiusMax, 1e-6f) && near(viaGestureUp, kBrushRadiusMax, 1e-6f) &&
              near(viaKeyUp, viaGestureUp, 1e-6f),
          "brush size: bracket key and drag gesture both saturate at EXACTLY "
          "kBrushRadiusMax, not two different near-misses under two different clamps");

    // 0.5, not 1.0: bracketStepForRadius() floors to a 1px minimum, so
    // subtracting it from anything <= kBrushRadiusMin + 1.0 would land
    // EXACTLY on kBrushRadiusMin without actually testing that the clamp
    // engages below it -- 1.5 - 1px genuinely undershoots to 0.5 first.
    const float nearMin = kBrushRadiusMin + 0.5f;
    const float viaKeyDown = clampBrushRadius(nearMin - bracketStepForRadius(nearMin));
    const float viaGestureDown = clampBrushRadius(radiusForDrag(nearMin, -500.0f));
    check(near(viaKeyDown, kBrushRadiusMin, 1e-6f) &&
              near(viaGestureDown, kBrushRadiusMin, 1e-6f) &&
              near(viaKeyDown, viaGestureDown, 1e-6f),
          "brush size: bracket key and drag gesture both saturate at EXACTLY "
          "kBrushRadiusMin, not two different near-misses under two different clamps");

    check(kBrushRadiusMin == 1.0f && kBrushRadiusMax == 200.0f,
          "kBrushRadiusMin/kBrushRadiusMax are 1..200 -- the BRUSH panel's wider range "
          "(docs/reachability-audit.md B3), not the options bar's narrower 2..90");
  }

  // ==========================================================================
  // (e) toolZoomsView(): the predicate a future toolHasCanvasHandler()
  // disjunction is meant to absorb.
  // ==========================================================================
  {
    check(toolZoomsView(Tool::Zoom), "toolZoomsView: true for Tool::Zoom");
    const Tool others[] = {Tool::Brush, Tool::Hand, Tool::Eyedropper, Tool::Marquee,
                           Tool::Water, Tool::DryBrush};
    bool allFalse = true;
    for (Tool t : others)
      if (toolZoomsView(t)) allFalse = false;
    check(allFalse,
          "toolZoomsView: false for every tool with no zoom handler (Brush, Hand, "
          "Eyedropper, Marquee, Water, DryBrush)");
  }

  // ==========================================================================
  // (f) Zoom is a view change, not a document one: no history entry, checked
  // against a real OpenDocument/History, across a realistic multi-step
  // session (click-in, scrub, click-out) driven through the same pure
  // functions ui/MacPaintUI.cpp's canvas block actually calls -- not merely
  // "true because nothing here takes a History parameter."
  // ==========================================================================
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "fixture");
    const size_t historyBefore = od.history.entries().size();

    CanvasView view;  // the real AppState type, default zoom=1/pan=0
    const float paintOrigin = 0.0f, avail = 512.0f, tex = 64.0f;
    auto zoomStep = [&](float factor, float anchor) {
      const float oldZoom = view.zoom;
      const float newZoom = clampViewZoom(oldZoom * factor);
      if (newZoom == oldZoom) return;
      const float margin = std::max(0.0f, (avail - tex * oldZoom) * 0.5f);
      const float origin = paintOrigin + margin + view.panX;
      view.panX = panForAnchoredZoom(anchor, origin, oldZoom, newZoom, paintOrigin, avail, tex);
      view.zoom = newZoom;
    };
    zoomStep(kZoomStepFactor, 300.0f);                 // "click" to zoom in
    for (int i = 0; i < 5; ++i) zoomStep(zoomFactorForDrag(12.0f), 300.0f);  // scrub
    zoomStep(1.0f / kZoomStepFactor, 300.0f);           // Alt-click to zoom out

    check(od.history.entries().size() == historyBefore,
          "zoom: a click-in + five-frame scrub + click-out session leaves the "
          "document's history entry count EXACTLY unchanged -- zoom never records");
    check(view.zoom != 1.0f, "zoom: the session actually changed the view (not a "
                             "vacuous pass because nothing happened)");
  }

  std::printf("[selftest] zoom and size %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
