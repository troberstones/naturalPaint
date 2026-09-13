#include "app/ZoomToSelection.hpp"

#include <algorithm>

#include "app/ViewTransform.hpp"
#include "app/ZoomAndSize.hpp"  // kViewZoomMax, clampViewZoom

namespace np {
namespace {

// `view`'s rotation and mirror alone (zoom held at 1), applied to a vector
// FROM the canvas centre -- the same isolation trick app/ZoomAndSize.hpp's
// `panForAnchoredZoomRotate()` describes: a throwaway `ViewTransform` with
// both `canvasCenter` and `pivotScreen` at the origin makes `toScreen(v)`
// reduce to exactly `M.apply(v)`, with no translation to subtract back out.
Vec2 unitTransform(const CanvasView& view, Vec2 v) noexcept {
  CanvasView unit = view;
  unit.zoom = 1.0f;
  const ViewTransform xform(unit, Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f});
  return xform.toScreen(v);
}

}  // namespace

ZoomToSelectionFit fitZoomToSelection(float selX0, float selY0, float selX1, float selY1,
                                      float texW, float texH, const CanvasView& view,
                                      Vec2 paintOrigin, Vec2 avail, float marginPx) noexcept {
  const Vec2 selCenter{(selX0 + selX1) * 0.5f, (selY0 + selY1) * 0.5f};

  // The selection's four corners, relative to its own centre, rotated (and
  // mirrored) at unit zoom -- the per-zoom-unit size of its screen-space
  // bounding box. All four corners, not two: a rotation can swap which pair
  // of the rectangle's corners are the extremal ones.
  const Vec2 corners[4] = {
      Vec2{selX0 - selCenter.x, selY0 - selCenter.y},
      Vec2{selX1 - selCenter.x, selY0 - selCenter.y},
      Vec2{selX1 - selCenter.x, selY1 - selCenter.y},
      Vec2{selX0 - selCenter.x, selY1 - selCenter.y},
  };
  float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
  for (int i = 0; i < 4; ++i) {
    const Vec2 c = unitTransform(view, corners[i]);
    if (i == 0) {
      minX = maxX = c.x;
      minY = maxY = c.y;
    } else {
      minX = std::min(minX, c.x);
      maxX = std::max(maxX, c.x);
      minY = std::min(minY, c.y);
      maxY = std::max(maxY, c.y);
    }
  }
  const float widthPerZoom = maxX - minX;
  const float heightPerZoom = maxY - minY;

  const float effAvailX = std::max(1.0f, avail.x - 2.0f * marginPx);
  const float effAvailY = std::max(1.0f, avail.y - 2.0f * marginPx);

  // A near-zero extent (a 1-texel selection) asks for an effectively
  // unbounded zoom; `clampViewZoom()` below is what actually decides the
  // answer, the same as `requestFitWindow`'s own division would at a
  // vanishing document size.
  constexpr float kEpsilon = 1e-6f;
  const float widthZoom = widthPerZoom > kEpsilon ? effAvailX / widthPerZoom : kViewZoomMax;
  const float heightZoom = heightPerZoom > kEpsilon ? effAvailY / heightPerZoom : kViewZoomMax;
  const float zoom = clampViewZoom(std::min(widthZoom, heightZoom));

  // The pan that puts `selCenter` at the viewport's own centre, at `zoom` --
  // `ui/MacPaintUI.cpp`'s own `pivotScreen = paintOrigin + margin(zoom) + pan
  // + drawSize(zoom)/2`, inverted for `pan`. The same algebra
  // `panForAnchoredZoom()`'s header describes, solved for a chosen canvas
  // point (the selection's centre) rather than one read back off the OLD
  // view.
  CanvasView fitted = view;
  fitted.zoom = zoom;
  const ViewTransform xform(fitted, Vec2{texW * 0.5f, texH * 0.5f}, Vec2{0.0f, 0.0f});
  const Vec2 vecFromCenter = xform.toScreen(selCenter);

  const Vec2 drawSize{texW * zoom, texH * zoom};
  const Vec2 margin{std::max(0.0f, (avail.x - drawSize.x) * 0.5f),
                    std::max(0.0f, (avail.y - drawSize.y) * 0.5f)};
  const Vec2 viewportCenter{paintOrigin.x + avail.x * 0.5f, paintOrigin.y + avail.y * 0.5f};
  const Vec2 pivotTarget{viewportCenter.x - vecFromCenter.x, viewportCenter.y - vecFromCenter.y};

  ZoomToSelectionFit result;
  result.zoom = zoom;
  result.panX = pivotTarget.x - paintOrigin.x - margin.x - drawSize.x * 0.5f;
  result.panY = pivotTarget.y - paintOrigin.y - margin.y - drawSize.y * 0.5f;
  return result;
}

}  // namespace np
