#include "app/ZoomAndSize.hpp"

#include <algorithm>
#include <cmath>

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

}  // namespace np
