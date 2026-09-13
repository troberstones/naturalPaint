#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/ViewTransform.hpp"
#include "app/ZoomAndSize.hpp"      // kViewZoomMin, kViewZoomMax
#include "app/ZoomToSelection.hpp"

// PRD Q1 (P0): View > Zoom to Selection.
// `fitZoomToSelection()` is pure -- see app/ZoomToSelection.hpp's own header
// for why -- so this section is entirely headless and GPU-free, the same
// posture app/selftest/ZoomAndSize.cpp already takes for the sibling zoom
// functions this one reuses (`clampViewZoom()`, `kViewZoomMin`/`Max`).
namespace np {

bool runZoomToSelectionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  const Vec2 paintOrigin{0.0f, 0.0f};
  const Vec2 avail{800.0f, 600.0f};
  const float texW = 1000.0f;
  const float texH = 800.0f;
  const Vec2 canvasCenter{texW * 0.5f, texH * 0.5f};

  // Reconstructs the real screen position `selCenter` lands at, given a
  // `fitZoomToSelection()` result -- `ui/MacPaintUI.cpp`'s own `pivotScreen =
  // paintOrigin + margin(zoom) + pan + drawSize(zoom)/2`, copied here (not
  // re-derived) for the identical reason app/selftest/ZoomAndSize.cpp's own
  // (a2) section copies it: verifying through the REAL `ViewTransform`, not
  // through this function's own arithmetic checking itself.
  auto screenPositionOf = [&](Vec2 selCenter, const CanvasView& view, const ZoomToSelectionFit& fit) {
    CanvasView fitted = view;
    fitted.zoom = fit.zoom;
    const Vec2 drawSize{texW * fit.zoom, texH * fit.zoom};
    const Vec2 margin{std::max(0.0f, (avail.x - drawSize.x) * 0.5f),
                      std::max(0.0f, (avail.y - drawSize.y) * 0.5f)};
    const Vec2 pivotScreen{paintOrigin.x + margin.x + fit.panX + drawSize.x * 0.5f,
                           paintOrigin.y + margin.y + fit.panY + drawSize.y * 0.5f};
    const ViewTransform xform(fitted, canvasCenter, pivotScreen);
    return xform.toScreen(selCenter);
  };
  const Vec2 viewportCenter{paintOrigin.x + avail.x * 0.5f, paintOrigin.y + avail.y * 0.5f};

  // ==========================================================================
  // (a) No rotation: the largest zoom that fits, and the selection centred.
  // ==========================================================================
  {
    // A 200x200 selection in an 800x600 viewport: width wants zoom 4
    // (800/200), height wants zoom 3 (600/200) -- the tighter one, 3, wins.
    const CanvasView view;  // identity: zoom/pan ignored by the function, rotation/mirror 0
    const ZoomToSelectionFit fit =
        fitZoomToSelection(100.0f, 100.0f, 300.0f, 300.0f, texW, texH, view, paintOrigin, avail,
                          /*marginPx=*/0.0f);
    check(near(fit.zoom, 3.0f, 1e-4f),
          "no rotation: the tighter axis (height, needing zoom 3) decides the fit, not the "
          "looser one (width, which alone would allow zoom 4)");

    const Vec2 selCenter{200.0f, 200.0f};
    const Vec2 landed = screenPositionOf(selCenter, view, fit);
    check(near(landed.x, viewportCenter.x, 1e-2f) && near(landed.y, viewportCenter.y, 1e-2f),
          "no rotation: the selection's own centre lands exactly on the viewport's centre, "
          "round-tripped through the real ViewTransform the canvas actually renders with");
  }

  // ==========================================================================
  // (b) A margin shrinks the fit -- the parameter is load-bearing, not
  // decorative.
  // ==========================================================================
  {
    const CanvasView view;
    const ZoomToSelectionFit noMargin =
        fitZoomToSelection(100.0f, 100.0f, 300.0f, 300.0f, texW, texH, view, paintOrigin, avail,
                          0.0f);
    const ZoomToSelectionFit withMargin =
        fitZoomToSelection(100.0f, 100.0f, 300.0f, 300.0f, texW, texH, view, paintOrigin, avail,
                          40.0f);
    check(withMargin.zoom < noMargin.zoom,
          "margin: a positive margin fits the selection at a SMALLER zoom than no margin at "
          "all -- the effective viewport shrank by exactly the margin the caller asked for");

    // Exact values on each limiting axis, so dropping either axis's margin shows:
    // 400x100 wants (800-80)/400 = 1.8 wide; 200x200 wants (600-80)/200 = 2.6 tall.
    const ZoomToSelectionFit wide =
        fitZoomToSelection(100.0f, 250.0f, 500.0f, 350.0f, texW, texH, view, paintOrigin, avail,
                          40.0f);
    check(near(wide.zoom, 1.8f, 1e-4f),
          "margin: a width-limited fit leaves exactly the margin on both sides (zoom 1.8)");
    check(near(withMargin.zoom, 2.6f, 1e-4f),
          "margin: a height-limited fit leaves exactly the margin top and bottom (zoom 2.6)");
  }

  // ==========================================================================
  // (c) A rotated view: a wide, flat selection needs a smaller zoom once its
  // OWN bounding box is rotated, and it still lands centred.
  // ==========================================================================
  {
    // 200 wide x 50 tall -- picked wide-and-flat so a 45-degree rotation
    // visibly grows its screen-space bounding box (the diagonal of a 200x50
    // rectangle is far longer than either side), rather than a square, whose
    // axis-aligned bbox is rotation-invariant and would silently pass a
    // formula that ignored rotation entirely.
    CanvasView view;
    const ZoomToSelectionFit unrotated =
        fitZoomToSelection(100.0f, 100.0f, 300.0f, 150.0f, texW, texH, view, paintOrigin, avail,
                          0.0f);
    view.rotation = 0.78539816f;  // pi/4
    const ZoomToSelectionFit rotated =
        fitZoomToSelection(100.0f, 100.0f, 300.0f, 150.0f, texW, texH, view, paintOrigin, avail,
                          0.0f);
    check(rotated.zoom < unrotated.zoom,
          "rotated: a 45-degree view rotation grows the selection's screen-space bounding box, "
          "so the fit that clears it needs a SMALLER zoom than the unrotated fit does");

    const Vec2 selCenter{200.0f, 125.0f};
    const Vec2 landed = screenPositionOf(selCenter, view, rotated);
    check(near(landed.x, viewportCenter.x, 1e-2f) && near(landed.y, viewportCenter.y, 1e-2f),
          "rotated: the selection's own centre still lands on the viewport's centre under "
          "rotation -- the fit honours the current view rather than assuming it away");
  }

  // ==========================================================================
  // (d) The existing zoom limits, at both ends, on selections chosen to force
  // each -- not clamped by a coincidence.
  // ==========================================================================
  {
    // A single texel: both axes ask for an enormous zoom (800x and 600x),
    // which the existing 8x ceiling has to cut off.
    const CanvasView view;
    const ZoomToSelectionFit onePixel = fitZoomToSelection(500.0f, 400.0f, 501.0f, 401.0f, texW,
                                                           texH, view, paintOrigin, avail, 0.0f);
    check(near(onePixel.zoom, kViewZoomMax, 1e-4f),
          "clamp: a 1-texel selection is clamped to kViewZoomMax, not the raw (huge) ratio");

    // The whole canvas, in a viewport far smaller than it: both axes ask for
    // well under the existing 0.1x floor.
    const Vec2 tinyAvail{50.0f, 50.0f};
    const ZoomToSelectionFit wholeCanvas = fitZoomToSelection(
        0.0f, 0.0f, texW, texH, texW, texH, view, paintOrigin, tinyAvail, 0.0f);
    check(near(wholeCanvas.zoom, kViewZoomMin, 1e-4f),
          "clamp: the whole canvas, fit into a much smaller viewport, is clamped to "
          "kViewZoomMin, not the raw (tiny) ratio");
  }

  std::printf("[selftest] zoom to selection %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
