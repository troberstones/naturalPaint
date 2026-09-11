#pragma once

#include <cstdint>
#include <vector>

#include "brush/StrokePath.hpp"

namespace np {

// brush/Stabiliser -- sits between raw pointer samples and `StrokePath`:
// `StrokeSession::addSample()` feeds it a raw `StrokeSample`, it emits the
// smoothed sample the path actually walks. Pure geometry/filter state, no
// SDL/ImGui/Document -- `app/PointerQueue.{hpp,cpp}` is the pattern this
// copies.

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

  // Pulled string only. While the pen is down and not moving (tick()s with
  // no new sample), the gap between the nib and the pointer decays
  // exponentially toward zero so the nib reaches the pen after about this
  // many milliseconds -- precisely, 95% of the gap present when the pause
  // began is closed by `catchUpMs` (tau = catchUpMs / ln(20), gap(t) =
  // gap0 * exp(-t/tau)). 0 disables it (the string just sits at its full
  // window forever, wave 1's behaviour). Moving the pen again restores the
  // full window immediately, measured from wherever the nib now is -- the
  // ordinary pulled-string dead zone, untouched by this.
  float catchUpMs = 400.0f;

  bool catchUpAtEnd = true;
  // Weighted average only. Pulled string has its own pause catch-up now
  // (`catchUpMs` above, Wave 2 brief item 2) rather than sharing this flag --
  // a duration a painter tunes per string length reads more honestly as its
  // own field than as a shared on/off shared with a filter mode's own
  // "resting" cutoff, which is a different kind of convergence.
  bool catchUpWhilePaused = true;
  bool stabilisePressure = false;
  // Lengths are screen px rather than canvas px: `effectiveStringPx()` for
  // pulled string, and the speed that drives weighted average's `beta` term
  // (`brush/Stabiliser.cpp`'s own comment on why both need it) for the other.
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

// The one place the global/per-brush rule lives, so it stays easy to change.
// `Off` ignores the global entirely; `FollowGlobal` scales stringPx/strength
// by `amountPct/100` (clamped back into range), keeps the global's
// responsiveness UNSCALED (scaling it made higher amounts respond LESS to
// speed, the opposite of what the slider promises), and keeps the global's
// mode and options; `Own` takes the brush's own mode/stringPx/strength/
// responsiveness and the global's options.
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
  // raw position -- weighted average with `catchUpWhilePaused`, or pulled
  // string with `catchUpMs > 0` (both walk `pathHistory_`, not a straight
  // line, `tickPulledString()`'s own comment); false and `out` untouched
  // otherwise, including Off always.
  bool tick(uint64_t nowNs, StrokeSample& out) noexcept;

  // Stroke end, `catchUpAtEnd`: walks the nib to the last raw sample ALONG
  // THE RAW PATH the pointer actually took (not a straight line from
  // wherever the nib was lagging) -- one step per raw sample the walk
  // passes, ending exactly at the lift point (the last step's position is
  // exact; the dab `StrokePath` emits from each step is still spacing-
  // quantised like any other). `steps` is cleared and filled in walk order;
  // the caller feeds each one to `StrokePath::addPoint()` in turn, same as
  // any other sample. False (nothing written) if no sample has ever been
  // fed.
  bool forceCatchUp(std::vector<StrokeSample>& steps) noexcept;

  bool active() const noexcept { return haveRaw_; }
  Vec2 nibPos() const noexcept { return nib_; }
  Vec2 rawPos() const noexcept { return lastRaw_.pos; }
  // `stringPx`, screen-to-canvas converted when `scaleWithZoom` is set --
  // what the "show string" overlay should draw its circle/line at.
  float effectiveStringPx() const noexcept;
  const StabiliserParams& params() const noexcept { return params_; }

 private:
  bool addSamplePulledString(const StrokeSample& raw, StrokeSample& out) noexcept;
  // `isTick`: called from `tick()` with a synthetic sample rather than from
  // `addSample()` with a real one. The filter's own elapsed-time step still
  // runs off whichever of `prevTsNs_`/`prevRealTsNs_` last touched it, but a
  // REAL sample's dt is always measured from the last REAL sample (never
  // from an intervening tick) -- `brush/Stabiliser.cpp`'s own comment on why
  // a tick's timestamp is a different clock (frame time, not the pen's own)
  // and must never leak into a real sample's dt.
  bool addSampleWeightedAverage(const StrokeSample& raw, StrokeSample& out, bool isTick) noexcept;
  // Pulled string's own `tick()` handler -- `catchUpMs`'s exponential decay,
  // walked along `pathHistory_` (`pointAtArcLength()`) rather than straight
  // toward `lastRaw_.pos`, so a paused catch-up that spans a bend in the
  // recent path still follows it.
  bool tickPulledString(uint64_t nowNs, StrokeSample& out) noexcept;

  // The raw samples (this stroke, since `begin()`) the nib has not
  // necessarily caught up to yet -- "the path the pen actually took" that
  // both catch-ups (release and paused) walk instead of cutting a straight
  // line across it. Bounded at `kMaxPathHistory`: a hard cap, not an
  // arc-length one, because it costs one `erase(begin())` per sample past
  // the cap rather than a second length-tracking pass, and at typical
  // report rates (60-240 Hz) it comfortably outlasts any lag this build's
  // sliders (stringPx <= 200, strength/responsiveness's own bounded lag)
  // can build up. Only appended when `params_.mode != Off` -- Off's stroke
  // is bit-identical to raw input and has no catch-up to walk, so the
  // common case (Off is the compiled-in default) pays nothing for this.
  static constexpr size_t kMaxPathHistory = 512;
  std::vector<StrokeSample> pathHistory_;
  void appendPathHistory(const StrokeSample& raw) noexcept;
  // Total arc length of `pathHistory_`, and the arc length of the point on
  // it nearest `from` (nib's current position, which for weighted average is
  // a filtered point near but not exactly on the polyline -- this is its
  // projection) / at a given arc length from the start. The three primitives
  // both catch-ups are built from.
  float totalArcLength() const noexcept;
  float projectArcLength(Vec2 from) const noexcept;
  Vec2 pointAtArcLength(float s) const noexcept;

  StabiliserParams params_;
  float zoom_ = 1.0f;

  bool haveRaw_ = false;
  StrokeSample lastRaw_;
  Vec2 nib_{};

  bool haveNib_ = false;  // pulled string's own bootstrap latch
  float snappedPressure_ = 1.0f;  // pulled string's `stabilisePressure` state
  // The nib-to-pointer gap (arc length along `pathHistory_`), captured fresh
  // after every REAL pulled-string sample -- `catchUpMs`'s decay starts from
  // whatever this was, so "when the pen moves again the full string length
  // returns" (Wave 2 brief item 2) falls out for free: a real sample always
  // rewrites it before any tick reads it.
  float pulledStringPauseGap0_ = 0.0f;

  bool haveFilter_ = false;  // weighted average's own bootstrap latch
  Vec2 filtPos_{};
  Vec2 prevRawPos_{};
  float filtSpeed_ = 0.0f;
  float filtPressure_ = 1.0f;
  // Last time the filter itself was advanced, real sample or tick -- what
  // its own dt/alpha step is measured from.
  uint64_t prevTsNs_ = 0;
  // Last REAL sample's timestamp only, untouched by `tick()`. A real
  // sample's own dt is measured from this, not from `prevTsNs_`, so a tick
  // that ran in between -- on the frame clock, not the pen's -- can never
  // make the next real sample's dt negative or artificially tiny.
  uint64_t prevRealTsNs_ = 0;
};

}  // namespace np
