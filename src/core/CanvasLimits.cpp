#include "core/CanvasLimits.hpp"

namespace np {
namespace {

// Not an atomic and not guarded. One writer (`main()`, before any document
// exists or any worker thread is started), then read-only for the life of the
// process -- the same discipline `core/ResourcePaths` uses for its own
// process-wide answer. A second writer would be the bug, not a missing lock,
// which is why `setMaxCanvasDimension()` documents "called once".
int32_t g_maxCanvasDimension = kFallbackMaxCanvasDimension;

}  // namespace

int32_t maxCanvasDimension() noexcept { return g_maxCanvasDimension; }

void setMaxCanvasDimension(int32_t px) noexcept {
  if (px > 0) g_maxCanvasDimension = px;
}

std::string canvasDimensionRefusal(int32_t width, int32_t height, int32_t maxDim) {
  // A non-positive extent belongs to the caller's own vocabulary (see the
  // header), and a non-positive ceiling means `setMaxCanvasDimension()` was
  // handed something it already refuses to store -- neither is this
  // function's refusal to make.
  if (maxDim <= 0) return {};
  if (width <= maxDim && height <= maxDim) return {};

  // Name the axis that actually broke it, both axes' values, and the limit.
  // "too big" without the number is not actionable: the user's next question
  // is always "too big for what", and the answer is a figure they can compare
  // their file against.
  const char* axis = width > maxDim ? (height > maxDim ? "Both sides are" : "The width is")
                                    : "The height is";
  return std::to_string(width) + " x " + std::to_string(height) +
         " is larger than this display adapter can draw. " + axis + " over the " +
         std::to_string(maxDim) + " pixel limit on a single canvas.";
}

std::string canvasDimensionRefusal(int32_t width, int32_t height) {
  return canvasDimensionRefusal(width, height, maxCanvasDimension());
}

}  // namespace np
