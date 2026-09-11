#include "brush/Stabiliser.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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
      // `responsiveness` is deliberately NOT scaled by `amt` -- it is a
      // cutoff-curve shape control, not a magnitude the "amount" slider's
      // 0-300% is meant to stretch (see StabiliserParams::responsiveness).
      // `catchUpMs` is untouched too (stays at `eff`'s copy of `global`'s):
      // it is a duration, not a "string length / strength" magnitude, so it
      // is one of the options that always follows the global setting.
      break;
    }
    case StabiliserBrushMode::Own:
      eff.mode = brush.own.mode;
      eff.stringPx = brush.own.stringPx;
      eff.strength = brush.own.strength;
      eff.responsiveness = brush.own.responsiveness;
      eff.catchUpMs = brush.own.catchUpMs;
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
  pulledStringPauseGap0_ = 0.0f;
  haveFilter_ = false;
  filtPos_ = Vec2{};
  prevRawPos_ = Vec2{};
  filtSpeed_ = 0.0f;
  filtPressure_ = 1.0f;
  prevTsNs_ = 0;
  prevRealTsNs_ = 0;
  pathHistory_.clear();
}

float Stabiliser::effectiveStringPx() const noexcept {
  const float z = (params_.scaleWithZoom && zoom_ > 1e-6f) ? zoom_ : 1.0f;
  return std::max(params_.stringPx / z, 0.0f);
}

void Stabiliser::appendPathHistory(const StrokeSample& raw) noexcept {
  pathHistory_.push_back(raw);
  if (pathHistory_.size() > kMaxPathHistory) pathHistory_.erase(pathHistory_.begin());
}

float Stabiliser::totalArcLength() const noexcept {
  float acc = 0.0f;
  for (size_t i = 0; i + 1 < pathHistory_.size(); ++i) {
    const Vec2 p0 = pathHistory_[i].pos, p1 = pathHistory_[i + 1].pos;
    acc += std::hypot(p1.x - p0.x, p1.y - p0.y);
  }
  return acc;
}

float Stabiliser::projectArcLength(Vec2 from) const noexcept {
  if (pathHistory_.size() < 2) return 0.0f;
  float acc = 0.0f;
  float bestD2 = std::numeric_limits<float>::max();
  float bestS = 0.0f;
  for (size_t i = 0; i + 1 < pathHistory_.size(); ++i) {
    const Vec2 p0 = pathHistory_[i].pos, p1 = pathHistory_[i + 1].pos;
    const float ux = p1.x - p0.x, uy = p1.y - p0.y;
    const float len2 = ux * ux + uy * uy;
    float t = 0.0f;
    if (len2 > 1e-9f)
      t = std::clamp(((from.x - p0.x) * ux + (from.y - p0.y) * uy) / len2, 0.0f, 1.0f);
    const float px = p0.x + ux * t, py = p0.y + uy * t;
    const float dx = px - from.x, dy = py - from.y;
    const float d2 = dx * dx + dy * dy;
    const float segLen = std::sqrt(len2);
    if (d2 < bestD2) {
      bestD2 = d2;
      bestS = acc + segLen * t;
    }
    acc += segLen;
  }
  return bestS;
}

Vec2 Stabiliser::pointAtArcLength(float s) const noexcept {
  if (pathHistory_.empty()) return nib_;
  if (pathHistory_.size() == 1) return pathHistory_.front().pos;
  float acc = 0.0f;
  for (size_t i = 0; i + 1 < pathHistory_.size(); ++i) {
    const Vec2 p0 = pathHistory_[i].pos, p1 = pathHistory_[i + 1].pos;
    const float segLen = std::hypot(p1.x - p0.x, p1.y - p0.y);
    if (s <= acc + segLen || i + 2 == pathHistory_.size()) {
      const float t = segLen > 1e-6f ? std::clamp((s - acc) / segLen, 0.0f, 1.0f) : 0.0f;
      return Vec2{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t};
    }
    acc += segLen;
  }
  return pathHistory_.back().pos;
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
    pulledStringPauseGap0_ = 0.0f;
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
  // `catchUpMs`'s decay starts fresh from THIS gap every real sample -- "when
  // the pen moves again the full string length returns" (Wave 2 brief item
  // 2) needs no separate reset flag, because a real sample always rewrites
  // this before `tickPulledString()` can read it. Arc length, not straight-
  // line distance, so the decay's own walk (`pointAtArcLength()`) starts
  // from the same measure it ends at.
  pulledStringPauseGap0_ = std::max(0.0f, totalArcLength() - projectArcLength(nib_));
  return true;
}

bool Stabiliser::addSampleWeightedAverage(const StrokeSample& raw, StrokeSample& out,
                                          bool isTick) noexcept {
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
    prevRealTsNs_ = raw.timestamp;  // bootstrap is always a real sample
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
    if (!isTick) prevRealTsNs_ = raw.timestamp;
    out.pos = raw.pos;
    out.pressure = raw.pressure;
    nib_ = out.pos;
    return true;
  }

  // A real sample's dt is measured from the last REAL sample, never from an
  // intervening tick: `tick()`'s `nowNs` is a frame-poll timestamp, a
  // different clock from the pen's own hardware timestamp, and comparing a
  // real sample against it can make dt land before zero (a later-polled
  // tick outrunning an earlier-timestamped pen event still in flight) --
  // exactly the "leap"/reordering the review's probe caught. A tick's own
  // dt still runs off `prevTsNs_`, whichever of the two last advanced it.
  const uint64_t basisTs = isTick ? prevTsNs_ : prevRealTsNs_;
  float dtSec = raw.timestamp > basisTs ? static_cast<float>(raw.timestamp - basisTs) / 1e9f
                                        : 1e-4f;  // guard dt <= 0
  dtSec = std::max(dtSec, 1e-4f);
  // Without `catchUpWhilePaused`, a long real pause must not register as one
  // giant dt on the next real sample -- that would let the filter's cutoff
  // open all the way and leap straight to the raw point instead of resuming
  // its normal smoothing. Capped at 50 ms: long enough to span a real pen
  // cadence gap, short enough that a multi-second pause no longer leaks in.
  if (!params_.catchUpWhilePaused) dtSec = std::min(dtSec, 0.05f);

  const float vx = (raw.pos.x - prevRawPos_.x) / dtSec;
  const float vy = (raw.pos.y - prevRawPos_.y) / dtSec;
  // `scaleWithZoom` reaches the speed term here too, not only pulled
  // string's window -- the same canvas-space motion at a higher zoom is more
  // screen px/sec, so it should read as faster and cut in sooner.
  const float zoomForSpeed = (params_.scaleWithZoom && zoom_ > 1e-6f) ? zoom_ : 1.0f;
  const float speed = std::hypot(vx, vy) * zoomForSpeed;
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
  if (!isTick) prevRealTsNs_ = raw.timestamp;
  out.pos = filtPos_;
  out.pressure = filtPressure_;
  nib_ = out.pos;
  return true;
}

bool Stabiliser::addSample(const StrokeSample& raw, StrokeSample& out) noexcept {
  lastRaw_ = raw;
  haveRaw_ = true;
  // Off's stroke is bit-identical to raw input and never catches up, so it
  // never pays to keep the history the other two modes' catch-ups walk
  // (`pathHistory_`'s own comment, `Stabiliser.hpp`).
  if (params_.mode != StabiliserMode::Off) appendPathHistory(raw);
  switch (params_.mode) {
    case StabiliserMode::Off:
      out = raw;
      nib_ = raw.pos;
      return true;
    case StabiliserMode::PulledString:
      return addSamplePulledString(raw, out);
    case StabiliserMode::WeightedAverage:
      return addSampleWeightedAverage(raw, out, /*isTick=*/false);
  }
  out = raw;
  return true;
}

bool Stabiliser::tick(uint64_t nowNs, StrokeSample& out) noexcept {
  switch (params_.mode) {
    case StabiliserMode::WeightedAverage: {
      if (!params_.catchUpWhilePaused) return false;
      if (!haveFilter_) return false;
      if (nowNs <= prevTsNs_) return false;
      StrokeSample synthetic = lastRaw_;
      synthetic.timestamp = nowNs;
      return addSampleWeightedAverage(synthetic, out, /*isTick=*/true);
    }
    case StabiliserMode::PulledString:
      return tickPulledString(nowNs, out);
    case StabiliserMode::Off:
      return false;
  }
  return false;
}

bool Stabiliser::tickPulledString(uint64_t nowNs, StrokeSample& out) noexcept {
  if (params_.catchUpMs <= 0.0f) return false;  // 0 = off
  if (!haveNib_) return false;
  if (nowNs <= lastRaw_.timestamp) return false;

  // "95% of the gap closed by catchUpMs": gap(t) = gap0 * exp(-t/tau), so
  // gap(catchUpMs)/gap0 = 0.05 requires tau = catchUpMs / ln(20).
  const float elapsedMs = static_cast<float>(nowNs - lastRaw_.timestamp) / 1e6f;
  const float tau = params_.catchUpMs / std::log(20.0f);
  const float targetGap = pulledStringPauseGap0_ * std::exp(-elapsedMs / tau);

  // Walk the ACTUAL raw path, not a straight line to `lastRaw_.pos`: find
  // where the nib currently sits on that path (`projectArcLength`), then move
  // forward (never back -- `std::clamp`'s lower bound) to the arc length that
  // leaves exactly `targetGap` before the end. `pathHistory_` is a stroke's
  // worth of history (`kMaxPathHistory`), so this still finds the pause's own
  // recent bend even if it happened many samples before this tick.
  const float total = totalArcLength();
  const float s0 = projectArcLength(nib_);
  const float targetS = std::clamp(total - std::max(targetGap, 0.0f), s0, total);
  nib_ = pointAtArcLength(targetS);

  out = lastRaw_;
  out.pos = nib_;
  out.timestamp = nowNs;
  out.pressure = params_.stabilisePressure ? snappedPressure_ : lastRaw_.pressure;
  return true;
}

bool Stabiliser::forceCatchUp(std::vector<StrokeSample>& steps) noexcept {
  steps.clear();
  if (!haveRaw_) return false;

  // Walk the raw path from wherever the nib currently sits to the lift
  // point, one step per raw sample the walk passes -- an L-shaped path whose
  // corner falls inside that span is walked AROUND the corner, not cut
  // straight across it (Wave 2 brief item 3). `pathHistory_` empty (Off mode
  // never appends to it, or a caller fed no samples through `addSample()` at
  // all -- selftest fixtures that call `forceCatchUp()` directly) falls back
  // to the one exact point this always had.
  if (pathHistory_.size() < 2) {
    steps.push_back(lastRaw_);
  } else {
    const float s0 = projectArcLength(nib_);
    float acc = 0.0f;
    for (size_t i = 0; i + 1 < pathHistory_.size(); ++i) {
      const Vec2 p0 = pathHistory_[i].pos, p1 = pathHistory_[i + 1].pos;
      acc += std::hypot(p1.x - p0.x, p1.y - p0.y);
      if (acc > s0) steps.push_back(pathHistory_[i + 1]);
    }
    if (steps.empty()) steps.push_back(lastRaw_);  // nib already at/past the lift point
  }

  nib_ = lastRaw_.pos;
  filtPos_ = lastRaw_.pos;
  prevRawPos_ = lastRaw_.pos;
  filtPressure_ = lastRaw_.pressure;
  snappedPressure_ = lastRaw_.pressure;
  pulledStringPauseGap0_ = 0.0f;
  return true;
}

}  // namespace np
