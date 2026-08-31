#include "app/ZoomAndSize.hpp"

#include <algorithm>
#include <cmath>

#include "app/ViewTransform.hpp"

namespace np {

float panForAnchoredZoom(float anchorScreen, float originOld, float zoomOld, float zoomNew,
                          float paintOriginAxis, float availAxis, float texAxis) noexcept {
  // The document coordinate currently under `anchorScreen`, read off the
  // same pre-rotation mapping `ui/MacPaintUI.cpp`'s `origin`/`drawSize`
  // already define: screenX = origin.x + canvasX * zoom. Deliberately the
  // same formula wheel-zoom always anchored against (ignoring rotation/
  // mirror) rather than a new one that would also have to invert the view's
  // rotation -- scrubby zoom inherits the same "anchoring under a rotated
  // view is a pre-existing limitation" the wheel already had; nothing here
  // makes that better or worse.
  const float canvasPt = (anchorScreen - originOld) / zoomOld;
  // Solve the NEW origin such that `anchorScreen` maps to the SAME
  // `canvasPt`: anchorScreen == originNew + canvasPt * zoomNew.
  const float originNew = anchorScreen - canvasPt * zoomNew;
  // `origin = paintOrigin + margin + pan` (`ui/MacPaintUI.cpp`'s own
  // `origin` definition) -- so panNew is originNew with the other two terms
  // subtracted back out, margin recomputed at the NEW zoom, since that is
  // what the real `origin` will be built from on the next frame.
  const float drawSizeNew = texAxis * zoomNew;
  const float marginNew = std::max(0.0f, (availAxis - drawSizeNew) * 0.5f);
  return originNew - paintOriginAxis - marginNew;
}

AnchoredPan panForAnchoredZoomRotate(const CanvasView& oldView, const CanvasView& newView,
                                     Vec2 canvasCenter, Vec2 pivotScreenOld, Vec2 anchorScreen,
                                     Vec2 paintOrigin, Vec2 avail, Vec2 tex) noexcept {
  // Step 1: the canvas point currently under the anchor, through the OLD
  // view's own (already-proven) inverse.
  const ViewTransform oldXform(oldView, canvasCenter, pivotScreenOld);
  const Vec2 anchorCanvas = oldXform.toCanvas(anchorScreen);

  // Step 2: what the NEW zoom/rotation/mirror alone (pivotScreen held at the
  // origin) does to the vector from canvas-centre to that point -- i.e.
  // `M_new * (anchorCanvas - canvasCenter)`, read off `toScreen()` rather
  // than a second copy of `ViewTransform`'s private matrix build.
  const ViewTransform newXformAtOrigin(newView, canvasCenter, Vec2{0.0f, 0.0f});
  const Vec2 rotatedOffset = newXformAtOrigin.toScreen(anchorCanvas);

  // Step 3: the pivotScreen that puts `anchorCanvas` back at `anchorScreen`
  // under the new transform, then `panForAnchoredZoom()`'s own algebra
  // (pivotScreen == paintOrigin + margin + pan + drawSize/2) inverted for
  // `pan`, one axis at a time, with margin recomputed at the NEW zoom.
  const float pivotNewX = anchorScreen.x - rotatedOffset.x;
  const float pivotNewY = anchorScreen.y - rotatedOffset.y;
  const float drawSizeNewX = tex.x * newView.zoom;
  const float drawSizeNewY = tex.y * newView.zoom;
  const float marginNewX = std::max(0.0f, (avail.x - drawSizeNewX) * 0.5f);
  const float marginNewY = std::max(0.0f, (avail.y - drawSizeNewY) * 0.5f);
  return AnchoredPan{pivotNewX - paintOrigin.x - marginNewX - drawSizeNewX * 0.5f,
                     pivotNewY - paintOrigin.y - marginNewY - drawSizeNewY * 0.5f};
}

float clampViewZoom(float zoom) noexcept {
  return std::clamp(zoom, kViewZoomMin, kViewZoomMax);
}

float zoomFactorForDrag(float dragPixelsX) noexcept {
  return std::pow(2.0f, dragPixelsX / kZoomDragPixelsPerOctave);
}

float clampBrushRadius(float radius) noexcept {
  return std::clamp(radius, kBrushRadiusMin, kBrushRadiusMax);
}

float bracketStepForRadius(float radius) noexcept {
  // std::round rather than floor/ceil: a radius of 14 steps by round(1.4)=1,
  // not floor(1.4)=1 either way at that value, but round is the one that
  // does not systematically bias every step downward across the range --
  // floor would make kBracketStepFraction quietly smaller than stated at
  // every non-exact multiple.
  return std::max(1.0f, std::round(radius * kBracketStepFraction));
}

float radiusForDrag(float startRadius, float dragPixelsX) noexcept {
  return startRadius * std::pow(2.0f, dragPixelsX / kSizeDragPixelsPerOctave);
}

bool toolZoomsView(Tool tool) noexcept { return tool == Tool::Zoom; }

CanvasDimensions canvasDimensionsFor(const OpenDocument* doc, uint32_t fallbackW,
                                     uint32_t fallbackH) noexcept {
  if (doc != nullptr && doc->document.width > 0 && doc->document.height > 0) {
    return CanvasDimensions{static_cast<float>(doc->document.width),
                            static_cast<float>(doc->document.height)};
  }
  return CanvasDimensions{static_cast<float>(fallbackW), static_cast<float>(fallbackH)};
}

CanvasDimensions paintSimDimensionsFor(const OpenDocument* doc, uint32_t fallbackW,
                                       uint32_t fallbackH) noexcept {
  const CanvasDimensions display = canvasDimensionsFor(doc, fallbackW, fallbackH);
  // uint64_t, and multiplied only after the widening cast: the two factors
  // are each bounded by kMaxDocumentPresetDimension (32768), whose square is
  // 2^30 -- inside uint32_t, but only just, and nothing here should depend on
  // that margin holding if the preset limit is ever raised.
  const uint64_t texels =
      static_cast<uint64_t>(display.w) * static_cast<uint64_t>(display.h);
  if (texels <= kPaintSimMaxTexels) return display;
  return CanvasDimensions{static_cast<float>(fallbackW), static_cast<float>(fallbackH)};
}

CanvasView resetCanvasView(const CanvasView& current) noexcept {
  CanvasView v = current;  // start from current: grayscale/grade ride along untouched
  v.zoom = 1.0f;
  v.panX = 0.0f;
  v.panY = 0.0f;
  v.mirrorX = false;
  v.mirrorY = false;
  v.rotation = 0.0f;
  return v;
}

}  // namespace np
