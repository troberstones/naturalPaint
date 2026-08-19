#pragma once

#include "app/AppState.hpp"      // CanvasView
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// PLAN.md Phase 2 step 11 ("View controls"): the one view matrix mirror,
// rotation, zoom and pan all compose through, so pen input can map back
// through this transform's actual analytic inverse instead of a second,
// independently hand-derived "inverse-looking" formula. Per docs/
// shortcuts.md section 3's own mandate: "the mirrors and the rotation
// compose into a single view matrix, and pen input maps back through its
// inverse -- otherwise painting under a mirror lands in the wrong place."
//
// Deliberately just the linear-algebra piece, not layout policy. Callers
// (ui/MacPaintUI.cpp's canvas block) still own `avail`/`canvasPos`/the
// existing centring-and-clamp arithmetic that turns zoom/pan into an
// on-screen quad -- that is presentation policy specific to one window
// layout, not part of "what does mirror+rotate+zoom+pan compose to." What
// this struct owns is: given where canvas-space's own centre currently sits
// on screen (`pivotScreen`, e.g. MacPaintUI's `origin + drawSize/2`) and
// where the canvas's own centre is in texel space (`canvasCenter`, e.g.
// `(texW/2, texH/2)`), map any canvas point to screen and back.

// A 2x2 linear map: (x, y) -> (a*x + b*y, c*x + d*y).
struct Mat2 {
  float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;

  Vec2 apply(Vec2 p) const { return Vec2{a * p.x + b * p.y, c * p.x + d * p.y}; }

  // The actual analytic inverse (adjugate over determinant), not a second,
  // separately typed-out formula -- this is what makes ViewTransform's
  // toCanvas() provably ViewTransform::toScreen()'s inverse rather than
  // merely "looks like it should be." Callers of ViewTransform never build
  // an "inverse Mat2" by hand; they only ever get one by calling this on the
  // forward matrix. Zoom is validated >0 by the UI's own clamp (0.1..8.0,
  // MacPaintUI.cpp), and mirror contributes only a determinant sign flip
  // (+-1), so the determinant is never zero in practice; the explicit guard
  // below just keeps this method total (returns the zero map) rather than
  // ever dividing by zero.
  Mat2 inverse() const {
    const float det = a * d - b * c;
    if (det == 0.0f) return Mat2{0.0f, 0.0f, 0.0f, 0.0f};
    const float invDet = 1.0f / det;
    return Mat2{d * invDet, -b * invDet, -c * invDet, a * invDet};
  }
};

class ViewTransform {
 public:
  ViewTransform() = default;

  // Builds M = zoom * rotate(view.rotation) * mirror(view.mirrorX,
  // view.mirrorY) once, up front -- both toScreen() and toCanvas() below
  // read this same M (toCanvas() via M.inverse()), so there is exactly one
  // place the mirror/rotate/zoom composition is spelled out.
  ViewTransform(const CanvasView& view, Vec2 canvasCenter, Vec2 pivotScreen);

  // Canvas/document-texel space -> screen space. Matches MacPaintUI's old
  // (pre-step-11) `origin + p*zoom` exactly when mirrorX/mirrorY/rotation
  // are all at identity -- see this .cpp's file comment for the algebra.
  Vec2 toScreen(Vec2 canvasPt) const;

  // Screen space -> canvas/document-texel space. Genuinely `M.inverse()`
  // applied, not a hand-rederived formula -- this is the concrete meaning of
  // "pen input maps back through its inverse."
  Vec2 toCanvas(Vec2 screenPt) const;

 private:
  Mat2 m_;
  Vec2 canvasCenter_;
  Vec2 pivotScreen_;
};

}  // namespace np
