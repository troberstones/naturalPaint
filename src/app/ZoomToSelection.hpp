#pragma once

#include "app/CanvasView.hpp"     // CanvasView
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// app/ZoomToSelection -- PRD Q1 (P0), reach wave track `zoom`: View > Zoom to
// Selection.
//
// Pure and headless, like app/ZoomAndSize.hpp's own functions and for the
// identical reason stated there -- so `--selftest` can assert on the actual
// arithmetic rather than on constants nothing calls. This file takes no
// `OpenDocument`: `ui/MacPaintUI.cpp`'s canvas block is the only caller, and
// it already holds the selection's bounds (`core::selectionBounds()`), the
// document's on-screen size and the canvas window's own geometry at the one
// point `requestFitWindow` is serviced -- this is that same request, framing
// a rectangle instead of the whole document.
//
// Reuses app/ZoomAndSize.hpp's `clampViewZoom()` (0.1x..8x) rather than a
// second limit: PRD Q1 asks to fit a selection within the existing zoom
// range, not to invent a wider one.

// A comfortable margin: enough that a selection's own marching ants (drawn
// ON the selection boundary, not outside it) do not sit flush against the
// window edge after the fit. Named once so `ui/MacPaintUI.cpp` and
// `--selftest` cannot use two different numbers for "small".
constexpr float kZoomToSelectionMarginPx = 24.0f;

// The three `CanvasView` fields a fit decides. Mirror is untouched (framing a
// rectangle does not care which way the picture faces) and rotation is an
// INPUT, not reset -- this frames the selection AS the view is currently
// rotated, exactly as `requestFitWindow` frames the whole document as
// currently rotated.
struct ZoomToSelectionFit {
  float zoom;
  float panX;
  float panY;
};

// `sel*` is the selection's bounds in document-texel space -- an exclusive
// rectangle (`x1 > x0`, `y1 > y0`), core/SelectionMask.hpp's own
// `SelectionBounds` shape, passed as four floats rather than that struct so
// this file stays free of core/SelectionMask's own dependencies (core/Pigment,
// core/TileStore), the same "pure and headless" argument app/ZoomAndSize.hpp
// makes for itself.
//
// `texW`/`texH` is the document's on-screen size in texels
// (`canvasDimensionsFor()`'s answer). `view` supplies rotation and mirror
// ONLY -- its own zoom/pan are ignored; this function computes new ones.
// `paintOrigin`/`avail` are the canvas window's on-screen rectangle, exactly
// `ui/MacPaintUI.cpp`'s own locals of those names at the point
// `requestFitWindow` is serviced.
//
// `marginPx` is a fixed screen-space margin, in pixels at the FITTED zoom
// (not a fraction of the selection), left around the selection's rotated
// bounding box so its own marching ants are not drawn flush against the
// window edge.
//
// A degenerate selection (a single texel, or a rotated bounding box smaller
// than `marginPx` could ever leave room for) still returns a finite result:
// `clampViewZoom()` has the last word, exactly as it does for
// `requestFitWindow` and every wheel/pinch zoom.
ZoomToSelectionFit fitZoomToSelection(float selX0, float selY0, float selX1, float selY1,
                                      float texW, float texH, const CanvasView& view,
                                      Vec2 paintOrigin, Vec2 avail, float marginPx) noexcept;

}  // namespace np
