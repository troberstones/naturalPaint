#include "brush/Stabiliser.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// One-euro filter primitives (Casiez, Godin, Pouderoux, CHI 2012).
constexpr float kSpeedFilterHz = 1.0f;  // fixed dcutoff on the speed signal

// Pulled string's paused catch-up (string-fix brief). A ball around a FIXED
// anchor, not a per-sample delta -- `stationaryAnchor_`'s own comment in
// Stabiliser.hpp says why.
constexpr float kStationaryPx = 1.0f;
// Longer than any plausible event-delivery hiccup (a slow pointer routinely
// leaves a 60 fps frame with no new sample; that must never read as a
// pause) AND longer than a few samples' worth of ordinary jitter briefly
// holding a genuinely-still-moving pointer inside `kStationaryPx` (measured
// against a slow, jittery arc -- `app/selftest/Stabiliser.cpp`'s own
// string-fix section), while staying well short of a deliberate pause.
constexpr float kCatchUpHoldOffMs = 80.0f;

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
  stationaryAnchor_ = Vec2{};
  stationaryStartNs_ = 0;
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

bool Stabiliser::addSamplePulledString(const StrokeSample& raw, StrokeSample& out) noexcept {
  out.timestamp = raw.timestamp;
  out.tilt = raw.tilt;
  out.azimuth = raw.azimuth;
  out.barrel = raw.barrel;

  if (!haveNib_) {
    nib_ = raw.pos;
    haveNib_ = true;
    snappedPressure_ = raw.pressure;
    stationaryAnchor_ = raw.pos;
    stationaryStartNs_ = raw.timestamp;
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
  // A FIXED anchor, not "moved since the previous sample" -- a pen held
  // still still reports sub-pixel jitter forever (a per-sample test would
  // never let catch-up's hold-off clock start), while genuinely slow motion
  // leaves a fixed `kStationaryPx` ball behind after that much travel, so
  // the clock correctly restarts once real motion resumes.
  const float anchorDist =
      std::hypot(raw.pos.x - stationaryAnchor_.x, raw.pos.y - stationaryAnchor_.y);
  if (anchorDist > kStationaryPx) {
    stationaryAnchor_ = raw.pos;
    stationaryStartNs_ = raw.timestamp;
  }
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
  if (nowNs <= stationaryStartNs_) return false;

  // Catch-up engages only once the pen has held still (a fixed anchor,
  // `stationaryAnchor_`'s own comment) for longer than `kCatchUpHoldOffMs`
  // -- string-fix brief defect B: without this hold-off, an ordinary frame
  // that simply missed the next pointer event (routine at 60 fps for a slow
  // pointer) looks identical to a genuine pause, and the string dissolves
  // mid-stroke.
  const float elapsedMs = static_cast<float>(nowNs - stationaryStartNs_) / 1e6f;
  if (elapsedMs <= kCatchUpHoldOffMs) return false;

  // "95% of the WINDOW closed by catchUpMs, measured from when catch-up
  // engages": window(t) = stringPx * exp(-(t - holdOff)/tau), tau =
  // catchUpMs / ln(20). Subtracting the hold-off keeps window(holdOff) ==
  // stringPx exactly, i.e. continuous with the window right before catch-up
  // engaged -- without it the window would jump from 1.0 to exp(-holdOff/tau)
  // in a single frame and the nib would visibly pop.
  const float tau = params_.catchUpMs / std::log(20.0f);
  const float scale = std::exp(-(elapsedMs - kCatchUpHoldOffMs) / tau);
  const float window = effectiveStringPx() * scale;

  // The IDENTICAL chord step `addSamplePulledString()` uses, toward
  // `lastRaw_.pos` -- never a snap onto the raw polyline. While the pen is
  // down the nib now moves by this ONE rule regardless of who calls it, so a
  // no-sample frame can no longer disagree with a real sample about where
  // the nib belongs (string-fix brief defects A/C) -- the oscillation
  // becomes unrepresentable rather than merely rare.
  const float dx = lastRaw_.pos.x - nib_.x;
  const float dy = lastRaw_.pos.y - nib_.y;
  const float dist = std::hypot(dx, dy);
  if (dist <= window) return false;  // no movement, no emitted sample, no work
  const float t = (dist - window) / dist;
  nib_.x += dx * t;
  nib_.y += dy * t;

  out = lastRaw_;
  out.pos = nib_;
  out.timestamp = nowNs;
  out.pressure = params_.stabilisePressure ? snappedPressure_ : lastRaw_.pressure;
  return true;
}

bool Stabiliser::forceCatchUp(std::vector<StrokeSample>& steps) noexcept {
  steps.clear();
  if (!haveRaw_) return false;

  // How far behind the pen the nib is, and the tail of the raw path it has
  // not caught up to: walk BACKWARDS from the lift point until the path
  // behind us is at least as long as that lag. A chord is never longer than
  // the arc it subtends, so this stops at or after the nib's true position
  // along the path -- never on an unrelated earlier stretch of it, which is
  // what a global nearest-point search returns once a slow, jittery stroke
  // wanders back within a string length of itself (the same ill-conditioning
  // that made the paused catch-up oscillate).
  const float lag = std::hypot(lastRaw_.pos.x - nib_.x, lastRaw_.pos.y - nib_.y);
  size_t first = pathHistory_.empty() ? 0 : pathHistory_.size() - 1;
  float tailArc = 0.0f;
  while (first > 0 && tailArc < lag) {
    const Vec2 a = pathHistory_[first - 1].pos, b = pathHistory_[first].pos;
    tailArc += std::hypot(b.x - a.x, b.y - a.y);
    --first;
  }

  if (pathHistory_.size() - first < 2 || tailArc <= 1e-6f) {
    // No tail to walk: `pathHistory_` empty (Off mode never appends to it, or
    // a caller fed no samples through `addSample()` at all -- selftest
    // fixtures that call this directly), or a nib already at the lift point.
    // The one exact point this always had.
    steps.push_back(lastRaw_);
  } else {
    // Replay that tail through the string itself rather than emitting the raw
    // samples: each step is the nib pulled toward the next raw sample, with
    // the window ramped from `lag` to zero by ARC LENGTH along the tail (not
    // per sample, so the walk's shape does not change with the pen's report
    // rate). Two things follow, and both are the point of doing it this way.
    // The walk STARTS where the nib already is, so the stroke continues
    // instead of jumping sideways onto the polyline -- that jump is painted
    // ink, however legitimately the string put the nib off the path. And the
    // walk is smoothed by the same rule as the rest of the stroke, so the
    // last string length of every stroke no longer ends in raw pen jitter.
    // The window still has to reach zero for the stroke to end where the pen
    // lifted, so the final steps do track the pen closely; what they no
    // longer do is start there.
    float acc = 0.0f;
    for (size_t i = first + 1; i < pathHistory_.size(); ++i) {
      const Vec2 p0 = pathHistory_[i - 1].pos, p1 = pathHistory_[i].pos;
      acc += std::hypot(p1.x - p0.x, p1.y - p0.y);
      const float window = lag * std::max(0.0f, 1.0f - acc / tailArc);
      const float dx = p1.x - nib_.x, dy = p1.y - nib_.y;
      const float dist = std::hypot(dx, dy);
      if (dist > window) {
        const float t = (dist - window) / dist;
        nib_.x += dx * t;
        nib_.y += dy * t;
      }
      StrokeSample step = pathHistory_[i];
      step.pos = nib_;
      steps.push_back(step);
    }
    // `acc` and `tailArc` sum the same segments in opposite orders, so the
    // last window is only nearly zero. The lift point is exact by fiat.
    steps.back() = lastRaw_;
  }

  nib_ = lastRaw_.pos;
  filtPos_ = lastRaw_.pos;
  prevRawPos_ = lastRaw_.pos;
  filtPressure_ = lastRaw_.pressure;
  snappedPressure_ = lastRaw_.pressure;
  // A jump-to-the-lift-point catch-up also counts as the pen settling there
  // -- the stationary clock restarts from this instant so a subsequent
  // paused tick's hold-off is measured from here, not from stale state.
  stationaryAnchor_ = lastRaw_.pos;
  stationaryStartNs_ = lastRaw_.timestamp;
  return true;
}

}  // namespace np
