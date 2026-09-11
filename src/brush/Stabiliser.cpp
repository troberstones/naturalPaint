#include "brush/Stabiliser.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// One-euro filter primitives (Casiez, Godin, Pouderoux, CHI 2012).
constexpr float kSpeedFilterHz = 1.0f;  // fixed dcutoff on the speed signal

float alphaFor(float cutoffHz, float dtSec) noexcept {
  const float tau = 1.0f / (2.0f * 3.14159265358979323846f * std::max(cutoffHz, 1e-4f));
  return dtSec / (dtSec + tau);
}

// See Stabiliser.hpp's own comment on `StabiliserParams::strength` for the
// stated mapping; strength <= 0 is handled by the caller as a full bypass.
float fcMinForStrength(float strength) noexcept {
  const float t = std::clamp(strength, 0.0f, 100.0f) / 100.0f;
  return 10.0f + (0.3f - 10.0f) * t;
}

float betaForResponsiveness(float responsiveness) noexcept {
  const float t = std::clamp(responsiveness, 0.0f, 100.0f) / 100.0f;
  return 0.03f * t;
}

}  // namespace

StabiliserParams resolveStabiliser(const StabiliserParams& global,
                                   const BrushStabiliserSetting& brush) noexcept {
  StabiliserParams eff = global;
  switch (brush.mode) {
    case StabiliserBrushMode::Off:
      eff.mode = StabiliserMode::Off;
      break;
    case StabiliserBrushMode::FollowGlobal: {
      const float amt = std::clamp(brush.amountPct, 0.0f, 300.0f) / 100.0f;
      eff.stringPx = std::max(0.0f, global.stringPx * amt);
      eff.strength = std::clamp(global.strength * amt, 0.0f, 100.0f);
      eff.responsiveness = std::clamp(global.responsiveness * amt, 0.0f, 100.0f);
      break;
    }
    case StabiliserBrushMode::Own:
      eff.mode = brush.own.mode;
      eff.stringPx = brush.own.stringPx;
      eff.strength = brush.own.strength;
      eff.responsiveness = brush.own.responsiveness;
      break;
  }
  return eff;
}

StabiliserBrushMode stabiliserBrushModeFromAbrSmoothing(bool smoothing) noexcept {
  return smoothing ? StabiliserBrushMode::FollowGlobal : StabiliserBrushMode::Off;
}

void Stabiliser::begin(const StabiliserParams& params, float viewZoom) noexcept {
  params_ = params;
  zoom_ = viewZoom;
  haveRaw_ = false;
  lastRaw_ = StrokeSample{};
  nib_ = Vec2{};
  haveNib_ = false;
  snappedPressure_ = 1.0f;
  haveFilter_ = false;
  filtPos_ = Vec2{};
  prevRawPos_ = Vec2{};
  filtSpeed_ = 0.0f;
  filtPressure_ = 1.0f;
  prevTsNs_ = 0;
}

float Stabiliser::effectiveStringPx() const noexcept {
  const float z = (params_.scaleWithZoom && zoom_ > 1e-6f) ? zoom_ : 1.0f;
  return std::max(params_.stringPx / z, 0.0f);
}

bool Stabiliser::addSamplePulledString(const StrokeSample& raw, StrokeSample& out) noexcept {
  out.timestamp = raw.timestamp;
  out.tilt = raw.tilt;
  out.azimuth = raw.azimuth;
  out.barrel = raw.barrel;

  if (!haveNib_) {
    nib_ = raw.pos;
    haveNib_ = true;
    snappedPressure_ = raw.pressure;
    out.pos = nib_;
    out.pressure = raw.pressure;
    return true;
  }

  const float dx = raw.pos.x - nib_.x;
  const float dy = raw.pos.y - nib_.y;
  const float dist = std::hypot(dx, dy);
  const float window = effectiveStringPx();
  bool moved = false;
  if (dist > window) {
    const float t = (dist - window) / dist;
    nib_.x += dx * t;
    nib_.y += dy * t;
    moved = true;
  }
  out.pos = nib_;
  if (params_.stabilisePressure) {
    // The string is taut exactly when the nib moves this sample -- pressure
    // follows the same on/off rule rather than a continuous blend, which has
    // no natural unit to blend pressure against in a purely geometric filter.
    if (moved) snappedPressure_ = raw.pressure;
    out.pressure = snappedPressure_;
  } else {
    out.pressure = raw.pressure;
  }
  return true;
}

bool Stabiliser::addSampleWeightedAverage(const StrokeSample& raw, StrokeSample& out) noexcept {
  out.timestamp = raw.timestamp;
  out.tilt = raw.tilt;
  out.azimuth = raw.azimuth;
  out.barrel = raw.barrel;

  if (!haveFilter_) {
    filtPos_ = raw.pos;
    prevRawPos_ = raw.pos;
    filtSpeed_ = 0.0f;
    filtPressure_ = raw.pressure;
    prevTsNs_ = raw.timestamp;
    haveFilter_ = true;
    out.pos = raw.pos;
    out.pressure = raw.pressure;
    nib_ = out.pos;
    return true;
  }

  if (params_.strength <= 0.0f) {
    // "0 = no smoothing": bypass rather than a filter with a huge cutoff, so
    // the output is bit-identical to the raw sample, not merely close to it.
    filtPos_ = raw.pos;
    prevRawPos_ = raw.pos;
    filtPressure_ = raw.pressure;
    prevTsNs_ = raw.timestamp;
    out.pos = raw.pos;
    out.pressure = raw.pressure;
    nib_ = out.pos;
    return true;
  }

  float dtSec = raw.timestamp > prevTsNs_
                    ? static_cast<float>(raw.timestamp - prevTsNs_) / 1e9f
                    : 1e-4f;  // guard dt <= 0 (out-of-order/duplicate timestamp)
  dtSec = std::max(dtSec, 1e-4f);

  const float vx = (raw.pos.x - prevRawPos_.x) / dtSec;
  const float vy = (raw.pos.y - prevRawPos_.y) / dtSec;
  const float speed = std::hypot(vx, vy);
  const float dAlpha = alphaFor(kSpeedFilterHz, dtSec);
  filtSpeed_ = dAlpha * speed + (1.0f - dAlpha) * filtSpeed_;

  const float fcMin = fcMinForStrength(params_.strength);
  const float beta = betaForResponsiveness(params_.responsiveness);
  const float cutoff = std::max(fcMin + beta * filtSpeed_, 0.01f);
  const float alpha = alphaFor(cutoff, dtSec);

  filtPos_.x = alpha * raw.pos.x + (1.0f - alpha) * filtPos_.x;
  filtPos_.y = alpha * raw.pos.y + (1.0f - alpha) * filtPos_.y;
  filtPressure_ =
      params_.stabilisePressure ? alpha * raw.pressure + (1.0f - alpha) * filtPressure_
                                : raw.pressure;

  prevRawPos_ = raw.pos;
  prevTsNs_ = raw.timestamp;
  out.pos = filtPos_;
  out.pressure = filtPressure_;
  nib_ = out.pos;
  return true;
}

bool Stabiliser::addSample(const StrokeSample& raw, StrokeSample& out) noexcept {
  lastRaw_ = raw;
  haveRaw_ = true;
  switch (params_.mode) {
    case StabiliserMode::Off:
      out = raw;
      nib_ = raw.pos;
      return true;
    case StabiliserMode::PulledString:
      return addSamplePulledString(raw, out);
    case StabiliserMode::WeightedAverage:
      return addSampleWeightedAverage(raw, out);
  }
  out = raw;
  return true;
}

bool Stabiliser::tick(uint64_t nowNs, StrokeSample& out) noexcept {
  if (params_.mode != StabiliserMode::WeightedAverage) return false;
  if (!params_.catchUpWhilePaused) return false;
  if (!haveFilter_) return false;
  if (nowNs <= prevTsNs_) return false;
  StrokeSample synthetic = lastRaw_;
  synthetic.timestamp = nowNs;
  return addSampleWeightedAverage(synthetic, out);
}

bool Stabiliser::forceCatchUp(StrokeSample& out) noexcept {
  if (!haveRaw_) return false;
  out = lastRaw_;
  nib_ = lastRaw_.pos;
  filtPos_ = lastRaw_.pos;
  prevRawPos_ = lastRaw_.pos;
  filtPressure_ = lastRaw_.pressure;
  snappedPressure_ = lastRaw_.pressure;
  return true;
}

}  // namespace np
