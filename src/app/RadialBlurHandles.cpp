#include "app/RadialBlurHandles.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Canvas texels per screen pixel is uniform: the view is zoom, rotation and
// mirror only.
float guideRadiusCanvas(const ViewTransform& view, Vec2 center) {
  const Vec2 a = view.toScreen(center);
  const Vec2 b = view.toScreen(Vec2{center.x + 1.0f, center.y});
  const float pxPerTexel = length(Vec2{b.x - a.x, b.y - a.y});
  return pxPerTexel > 0.0f ? kRadialBlurGuidePx / pxPerTexel : kRadialBlurGuidePx;
}

// Angle 0 points to canvas up (-y); positive turns toward +x.
Vec2 onCircle(Vec2 c, float r, float radians) {
  return Vec2{c.x + r * std::sin(radians), c.y - r * std::cos(radians)};
}

}  // namespace

RadialBlurHandleShape radialBlurHandleShape(const RadialBlurParams& params,
                                            const ViewTransform& view) {
  const Vec2 c{params.centerX, params.centerY};
  const float r = guideRadiusCanvas(view, c);
  RadialBlurHandleShape shape;
  shape.center = view.toScreen(c);
  shape.rest = view.toScreen(Vec2{c.x, c.y - r});

  if (params.method == RadialBlurMethod::Spin) {
    const float half =
        std::min(std::fabs(params.amount), kRadialBlurMaxSpinDegrees) * 0.5f * kDegToRad;
    const int steps = std::max(2, static_cast<int>(std::ceil(half / (3.0f * kDegToRad))) * 2);
    for (int i = 0; i <= steps; ++i) {
      const float t = -half + 2.0f * half * static_cast<float>(i) / static_cast<float>(steps);
      shape.guide.push_back(view.toScreen(onCircle(c, r, t)));
    }
    shape.amountHandle = view.toScreen(onCircle(c, r, half));
  } else {
    const float a = std::min(std::fabs(params.amount), kRadialBlurMaxZoom);
    shape.guide.push_back(view.toScreen(Vec2{c.x, c.y - r * (1.0f - a)}));
    shape.guide.push_back(view.toScreen(Vec2{c.x, c.y - r * (1.0f + a)}));
    shape.amountHandle = shape.guide.back();
  }
  return shape;
}

RadialBlurHandle radialBlurHandleAt(const RadialBlurHandleShape& shape, Vec2 pointer) {
  const auto near = [&](Vec2 p) {
    return length(Vec2{pointer.x - p.x, pointer.y - p.y}) <= kRadialBlurPickPx;
  };
  if (near(shape.center)) return RadialBlurHandle::Center;
  if (near(shape.amountHandle)) return RadialBlurHandle::Amount;
  return RadialBlurHandle::None;
}

RadialBlurHandleDrag beginRadialBlurHandleDrag(RadialBlurHandle handle,
                                               const RadialBlurParams& params,
                                               const ViewTransform& view, Vec2 pointer) {
  RadialBlurHandleDrag drag;
  drag.handle = handle;
  const Vec2 p = view.toCanvas(pointer);
  drag.grabOffset = Vec2{params.centerX - p.x, params.centerY - p.y};
  drag.amountSign = params.amount < 0.0f ? -1.0f : 1.0f;
  return drag;
}

void updateRadialBlurHandleDrag(const RadialBlurHandleDrag& drag, const ViewTransform& view,
                                Vec2 pointer, float docW, float docH, RadialBlurParams* params) {
  const Vec2 p = view.toCanvas(pointer);
  if (drag.handle == RadialBlurHandle::Center) {
    params->centerX = std::clamp(std::round(p.x + drag.grabOffset.x), 0.0f, docW);
    params->centerY = std::clamp(std::round(p.y + drag.grabOffset.y), 0.0f, docH);
    return;
  }
  if (drag.handle != RadialBlurHandle::Amount) return;

  const Vec2 c{params->centerX, params->centerY};
  const Vec2 v{p.x - c.x, p.y - c.y};
  float magnitude = 0.0f;
  if (params->method == RadialBlurMethod::Spin) {
    if (length(v) < 1e-4f) return;
    const float halfDegrees = std::fabs(std::atan2(v.x, -v.y)) / kDegToRad;
    magnitude = std::round(std::min(2.0f * halfDegrees, kRadialBlurMaxSpinDegrees));
  } else {
    const float along = -v.y / guideRadiusCanvas(view, c);
    magnitude = std::round(std::clamp(along - 1.0f, 0.0f, kRadialBlurMaxZoom) * 100.0f) / 100.0f;
  }
  params->amount = drag.amountSign * magnitude;
}

}  // namespace np
