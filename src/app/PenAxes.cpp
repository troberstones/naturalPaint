#include "app/PenAxes.hpp"

#include <cmath>

namespace np {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// tan() blows up at 90 and SDL's tilt range is inclusive of it.
float clampTiltDeg(float deg) noexcept {
  if (!(deg > -89.9f)) return -89.9f;  // also catches NaN
  return deg < 89.9f ? deg : 89.9f;
}

float clamp01(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;
  return v < 1.0f ? v : 1.0f;
}

}  // namespace

float penTiltNormalised(float xTiltDeg, float yTiltDeg) noexcept {
  const float tx = std::tan(clampTiltDeg(xTiltDeg) * kDegToRad);
  const float ty = std::tan(clampTiltDeg(yTiltDeg) * kDegToRad);
  const float lean = std::atan(std::hypot(tx, ty)) / kDegToRad;  // 0..90
  return clamp01(lean / 90.0f);
}

float penAzimuthNormalised(float xTiltDeg, float yTiltDeg) noexcept {
  const float tx = std::tan(clampTiltDeg(xTiltDeg) * kDegToRad);
  const float ty = std::tan(clampTiltDeg(yTiltDeg) * kDegToRad);
  // An upright pen has no azimuth. atan2(0,0) is 0 by the standard, but
  // saying so here makes it a decision rather than an inherited accident.
  if (tx == 0.0f && ty == 0.0f) return 0.0f;
  float deg = std::atan2(ty, tx) / kDegToRad;  // (-180, 180]
  if (deg < 0.0f) deg += 360.0f;
  return clamp01(deg / 360.0f);
}

float penBarrelNormalised(float rotationDeg) noexcept {
  return clamp01((rotationDeg + 180.0f) / 360.0f);
}

}  // namespace np
