#include "app/SplitView.hpp"

#include <algorithm>

namespace np {

Vec2 splitPaneOrigin(Vec2 paneOrigin, Vec2 paneSize, float docW, float docH,
                     const CanvasView& view) noexcept {
  const float drawW = docW * view.zoom;
  const float drawH = docH * view.zoom;
  const float marginX = std::max(0.0f, (paneSize.x - drawW) * 0.5f);
  const float marginY = std::max(0.0f, (paneSize.y - drawH) * 0.5f);
  return Vec2{paneOrigin.x + marginX + view.panX, paneOrigin.y + marginY + view.panY};
}

namespace {

// The inverse of `splitPaneOrigin()` composed with "screen centre of this
// pane": the document-space point currently there, as a fraction of
// `docW`x`docH`. `paneOrigin` is irrelevant to a fraction (it cancels), so
// this calls `splitPaneOrigin()` with it at the origin rather than asking a
// caller for a pane position that has no bearing on the answer.
Vec2 centreFraction(Vec2 paneSize, float docW, float docH, const CanvasView& view) noexcept {
  const Vec2 origin = splitPaneOrigin(Vec2{0.0f, 0.0f}, paneSize, docW, docH, view);
  const float dx = (paneSize.x * 0.5f - origin.x) / view.zoom;
  const float dy = (paneSize.y * 0.5f - origin.y) / view.zoom;
  return Vec2{dx / docW, dy / docH};
}

}  // namespace

CanvasView matchZoomView(const CanvasView& source, Vec2 srcPaneSize, float srcW, float srcH,
                         Vec2 dstPaneSize, float dstW, float dstH) noexcept {
  CanvasView out = source;
  if (srcW <= 0.0f || srcH <= 0.0f || dstW <= 0.0f || dstH <= 0.0f || source.zoom <= 0.0f)
    return out;

  const Vec2 frac = centreFraction(srcPaneSize, srcW, srcH, source);
  const float zoom = source.zoom;
  const float dstDrawW = dstW * zoom;
  const float dstDrawH = dstH * zoom;
  const float dstMarginX = std::max(0.0f, (dstPaneSize.x - dstDrawW) * 0.5f);
  const float dstMarginY = std::max(0.0f, (dstPaneSize.y - dstDrawH) * 0.5f);
  // Solving `splitPaneOrigin()`'s own placement for `panX`/`panY`, given
  // that this frame's `frac` document point must land back at the
  // destination pane's centre: `frac.x*dstDrawW == dstPaneSize.x/2 -
  // dstMarginX - dstPan.x`.
  out.panX = dstPaneSize.x * 0.5f - dstMarginX - frac.x * dstDrawW;
  out.panY = dstPaneSize.y * 0.5f - dstMarginY - frac.y * dstDrawH;
  return out;
}

}  // namespace np
