#include "app/CurveEdit.hpp"

#include <algorithm>
#include <cmath>

namespace np {

void curveToPlot(float cx, float cy, float plotSize, float& px, float& py) noexcept {
  if (!(plotSize > 0.0f)) {
    px = 0.0f;
    py = 0.0f;
    return;
  }
  px = cx * plotSize;
  py = (1.0f - cy) * plotSize;  // y-up curve space -> y-down screen space
}

void plotToCurve(float px, float py, float plotSize, float& cx, float& cy) noexcept {
  if (!(plotSize > 0.0f)) {
    cx = 0.0f;
    cy = 0.0f;
    return;
  }
  cx = px / plotSize;
  cy = 1.0f - py / plotSize;
}

std::optional<size_t> hitTestPoint(const Curve& curve, float px, float py, float plotSize,
                                    float radiusPx) noexcept {
  std::optional<size_t> best;
  float bestDist = 0.0f;
  for (size_t i = 0; i < curve.size(); ++i) {
    float cpx = 0.0f, cpy = 0.0f;
    curveToPlot(curve[i].x, curve[i].y, plotSize, cpx, cpy);
    const float dx = cpx - px;
    const float dy = cpy - py;
    const float dist = std::sqrt(dx * dx + dy * dy);
    // `<=` for the radius test (a point exactly on the boundary counts as a
    // hit), `<` for replacing an already-found best (see this function's
    // doc comment on the earlier-index-wins tie-break).
    if (dist <= radiusPx && (!best.has_value() || dist < bestDist)) {
      bestDist = dist;
      best = i;
    }
  }
  return best;
}

size_t insertPoint(Curve& curve, float cx, float cy) {
  cx = std::clamp(cx, 0.0f, 1.0f);
  cy = std::clamp(cy, 0.0f, 1.0f);
  const auto it = std::upper_bound(
      curve.begin(), curve.end(), cx,
      [](float x, const CurvePoint& p) { return x < p.x; });
  const size_t idx = static_cast<size_t>(it - curve.begin());
  curve.insert(it, CurvePoint{cx, cy});
  return idx;
}

size_t movePoint(Curve& curve, size_t index, float cx, float cy) {
  const CurvePoint p{std::clamp(cx, 0.0f, 1.0f), std::clamp(cy, 0.0f, 1.0f)};
  (void)curve.at(index);  // bounds-check -- throws std::out_of_range on misuse
  curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(index));
  // Re-insert through insertPoint() rather than hand-rolling a "shift past
  // crossed neighbours" loop -- it already does exactly that (find the
  // sorted position, splice in), so this both re-sorts when the move crossed
  // a neighbour and leaves the order untouched when it didn't, with one
  // implementation instead of two.
  return insertPoint(curve, p.x, p.y);
}

void removePoint(Curve& curve, size_t index) {
  (void)curve.at(index);  // bounds-check -- throws std::out_of_range on misuse
  curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(index));
}

}  // namespace np
