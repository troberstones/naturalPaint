#include "app/SplitView.hpp"

namespace np {

CanvasView matchZoomView(const CanvasView& source, float srcW, float srcH, float dstW,
                         float dstH) noexcept {
  if (srcW <= 0.0f || srcH <= 0.0f) return source;
  CanvasView out = source;
  out.panX = source.panX * (dstW / srcW);
  out.panY = source.panY * (dstH / srcH);
  return out;
}

}  // namespace np
