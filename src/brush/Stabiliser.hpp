#pragma once

#include <cstdint>

#include "brush/StrokePath.hpp"

namespace np {

// brush/Stabiliser -- Wave 2. Sits between raw pointer samples and
// `StrokePath`: `StrokeSession::addSample()` feeds it a raw `StrokeSample`,
// it emits the smoothed sample the path actually walks. Pure geometry/filter
// state, no SDL/ImGui/Document -- `app/PointerQueue.{hpp,cpp}` is the pattern
// this copies, and `app/selftest/Stabiliser.cpp` is what exercises it.

enum class StabiliserMode : uint8_t { Off, PulledString, WeightedAverage };

// A brush's own stabiliser choice (`NativeBrush`, naturalPaint's own
// section): follow the global setting (optionally scaled), sit out, or use
// its own mode/parameters while still taking the global's options.
enum class StabiliserBrushMode : uint8_t { FollowGlobal, Off, Own };

// The resolved (or global) setting: a mode, its geometric/filter parameters,
// and the options that apply regardless of mode.
struct StabiliserParams {
  StabiliserMode mode = StabiliserMode::Off;
  // Pulled string window, canvas px (or screen px, see `scaleWithZoom`).
  float stringPx = 16.0f;
  // Weighted average (1-euro filter, Casiez et al. CHI 2012): strength 0-100
  // maps to `fcMin`, the resting cutoff frequency -- 0 disables smoothing
  // entirely (the filter is bypassed, not merely weak), 100 gives a ~0.53 s
  // time constant at rest (`fcMin` = lerp(10 Hz, 0.3 Hz, strength/100) for
  // strength > 0). Responsiveness 0-100 maps to `beta`, how fast the cutoff
  // opens up with pen speed (`beta` = lerp(0, 0.03, responsiveness/100) Hz
  // per px/s) -- 0 keeps the lag constant at any speed, 100 nearly erases it
  // at a fast stroke. See `brush/Stabiliser.cpp` for the exact formulas.
  float strength = 40.0f;
  float responsiveness = 50.0f;

  bool catchUpAtEnd = true;
  // Weighted average only (brief's own "(default on, weighted average)") --
  // pulled string has nothing to converge while the pointer itself is still.
  bool catchUpWhilePaused = true;
  bool stabilisePressure = false;
  bool scaleWithZoom = false;
  bool showString = true;
};

// A brush's own setting: `mode` selects how `own`/`amountPct` combine with
// the global setting inside `resolveStabiliser()` below. `own` carries only
// mode/stringPx/strength/responsiveness when `mode == Own` -- its own option
// fields are never read; the global's options always apply.
struct BrushStabiliserSetting {
  StabiliserBrushMode mode = StabiliserBrushMode::FollowGlobal;
  float amountPct = 100.0f;  // 0-300, FollowGlobal only.
  StabiliserParams own;
};

// The one place the global/per-brush rule lives, so it stays easy to change
// (brief §"Resolve both into one effective setting"). `Off` ignores the
// global entirely; `FollowGlobal` scales stringPx/strength/responsiveness by
// `amountPct/100` (clamped back into range) and keeps the global's mode and
// options; `Own` takes the brush's own mode/stringPx/strength/responsiveness
// and the global's options.
StabiliserParams resolveStabiliser(const StabiliserParams& global,
                                   const BrushStabiliserSetting& brush) noexcept;

// `.abr` import (`io/AbrBrushes.cpp`): `toolOptions/smoothing` is the one bit
// a pack carries about smoothing -- `false` -> `Off`, `true` -> `Follow
// global` (at the default 100%, `BrushStabiliserSetting`'s own default).
StabiliserBrushMode stabiliserBrushModeFromAbrSmoothing(bool smoothing) noexcept;

// The filter itself. One instance per `StrokeSession`, `begin()` at pen-down.
class Stabiliser {
 public:
  void begin(const StabiliserParams& params, float viewZoom) noexcept;

  // Feeds one raw sample, returns the smoothed one `StrokePath` should walk.
  // Always succeeds (Off mode is an exact passthrough). Tilt/azimuth/barrel
  // are never smoothed, regardless of mode or `stabilisePressure`.
  bool addSample(const StrokeSample& raw, StrokeSample& out) noexcept;

  // No new sample this frame: let the nib keep converging toward the last
  // raw position (weighted average + `catchUpWhilePaused` only; false and
  // `out` untouched otherwise, including Off/PulledString).
  bool tick(uint64_t nowNs, StrokeSample& out) noexcept;

  // Stroke end, `catchUpAtEnd`: snaps straight to the last raw sample so the
  // final dab reaches the lift point. False (nothing written) if no sample
  // has ever been fed.
  bool forceCatchUp(StrokeSample& out) noexcept;

  bool active() const noexcept { return haveRaw_; }
  Vec2 nibPos() const noexcept { return nib_; }
  Vec2 rawPos() const noexcept { return lastRaw_.pos; }
  // `stringPx`, screen-to-canvas converted when `scaleWithZoom` is set --
  // what the "show string" overlay should draw its circle/line at.
  float effectiveStringPx() const noexcept;
  const StabiliserParams& params() const noexcept { return params_; }

 private:
  bool addSamplePulledString(const StrokeSample& raw, StrokeSample& out) noexcept;
  bool addSampleWeightedAverage(const StrokeSample& raw, StrokeSample& out) noexcept;

  StabiliserParams params_;
  float zoom_ = 1.0f;

  bool haveRaw_ = false;
  StrokeSample lastRaw_;
  Vec2 nib_{};

  bool haveNib_ = false;  // pulled string's own bootstrap latch
  float snappedPressure_ = 1.0f;  // pulled string's `stabilisePressure` state

  bool haveFilter_ = false;  // weighted average's own bootstrap latch
  Vec2 filtPos_{};
  Vec2 prevRawPos_{};
  float filtSpeed_ = 0.0f;
  float filtPressure_ = 1.0f;
  uint64_t prevTsNs_ = 0;
};

}  // namespace np
