#include "app/ViewTransform.hpp"

#include <cmath>

namespace np {

// M = zoom * rotate(theta) * mirror(mx, my).
//
//   mirror(mx, my) = [mx  0 ]      rotate(theta) = [cosT  -sinT]
//                    [0   my]                      [sinT   cosT]
//
//   rotate(theta) * mirror(mx, my) = [cosT*mx   -sinT*my]
//                                    [sinT*mx    cosT*my]
//
// then every entry scales by `zoom`. mirror is applied before rotate so
// mirroring reflects the canvas about its own axes first and the rotation
// then turns the already-mirrored result -- the same order a painter
// flipping, then rotating, a physical sheet would experience.
//
// At mirrorX = mirrorY = false, rotation = 0: mx = my = 1, cosT = 1, sinT =
// 0, so M reduces to `zoom * identity` and toScreen(p) below reduces to
// `pivotScreen + (p - canvasCenter) * zoom`, which is exactly
// `origin + p * zoom` once pivotScreen == origin + drawSize/2 and
// canvasCenter == (texW/2, texH/2) cancel out algebraically -- see
// MacPaintUI.cpp's canvas block for that cancellation spelled out, and
// SelfTest.cpp's runViewTransformTest() for the "identity" case that pins
// exactly this.
ViewTransform::ViewTransform(const CanvasView& view, Vec2 canvasCenter, Vec2 pivotScreen)
    : canvasCenter_(canvasCenter), pivotScreen_(pivotScreen) {
  const float mx = view.mirrorX ? -1.0f : 1.0f;
  const float my = view.mirrorY ? -1.0f : 1.0f;
  const float cosT = std::cos(view.rotation);
  const float sinT = std::sin(view.rotation);

  m_.a = view.zoom * cosT * mx;
  m_.b = view.zoom * -sinT * my;
  m_.c = view.zoom * sinT * mx;
  m_.d = view.zoom * cosT * my;
}

Vec2 ViewTransform::toScreen(Vec2 canvasPt) const {
  const Vec2 rel{canvasPt.x - canvasCenter_.x, canvasPt.y - canvasCenter_.y};
  const Vec2 t = m_.apply(rel);
  return Vec2{pivotScreen_.x + t.x, pivotScreen_.y + t.y};
}

Vec2 ViewTransform::toCanvas(Vec2 screenPt) const {
  const Vec2 rel{screenPt.x - pivotScreen_.x, screenPt.y - pivotScreen_.y};
  const Vec2 t = m_.inverse().apply(rel);
  return Vec2{canvasCenter_.x + t.x, canvasCenter_.y + t.y};
}

}  // namespace np
