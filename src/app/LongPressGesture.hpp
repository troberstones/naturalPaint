#pragma once

#include "app/PointerPolicy.hpp"  // kLongPressMs, kLongPressSlopPx

namespace np {

// app/LongPressGesture -- "a finger held still on one spot", recognised once.
//
// The truth table gives touch no way to sample a colour by tapping, on purpose:
// a tap is what a finger does by accident, and silently swapping the user's
// paint colour is an expensive accident. The deliberate version is a HOLD, and
// this is the recogniser for it. The same gesture gates the layer panel's
// drag-to-reorder, so both live off one recogniser rather than two sets of
// thresholds that can disagree about what "held" means.
//
// Deliberately a pure state machine over (position, time): no ImGui, no SDL, no
// clock of its own. The caller feeds it what it already has, which is what lets
// `--selftest` drive a whole gesture in a few lines and pin the edge cases that
// a device test can only reach by accident -- a press one millisecond short, a
// drag one pixel outside the slop, a second finger arriving mid-hold.
class LongPressGesture {
 public:
  enum class Phase : uint8_t {
    Idle,     // nothing held
    Pending,  // held, but not yet long enough (or still inside the slop)
    Fired,    // recognised -- reported for exactly one update
    // Moved too far, or already fired: this hold can no longer become a long
    // press, however long it continues. Never RETURNED to the caller -- it
    // reports as Pending, because a caller has nothing to do differently for
    // "held too briefly" versus "moved too far". It exists so the state
    // machine can remember the disqualification until the finger lifts.
    Disqualified,
  };

  // Feed one frame. `down` is whether a single finger is on the glass,
  // `x`/`y` its position in screen pixels, `nowMs` a monotonic timestamp.
  //
  // Returns `Fired` on the ONE update that crosses the threshold, never again
  // for that hold. One-shot by construction: a recogniser that kept answering
  // "yes, still held" would sample the colour under the finger on every frame
  // of a half-second hold, which is a hundred silent colour changes rather
  // than one.
  Phase update(bool down, float x, float y, double nowMs) {
    if (!down) {
      phase_ = Phase::Idle;
      return phase_;
    }
    if (phase_ == Phase::Idle) {  // the press edge: anchor here
      startX_ = x;
      startY_ = y;
      startMs_ = nowMs;
      phase_ = Phase::Pending;
      return phase_;
    }
    if (phase_ == Phase::Fired) return Phase::Pending;  // already spent

    // Wandering outside the slop means this is a DRAG, not a hold, and a drag
    // must never become a long press however long it lasts -- otherwise a slow
    // two-finger pan would sample a colour partway through. Disqualify the
    // whole gesture rather than re-anchoring: re-anchoring would let a finger
    // that crawls across the canvas fire repeatedly, once per time it happened
    // to pause.
    const float dx = x - startX_;
    const float dy = y - startY_;
    if (dx * dx + dy * dy > kLongPressSlopPx * kLongPressSlopPx) {
      phase_ = Phase::Disqualified;
      return Phase::Pending;
    }
    if (phase_ == Phase::Disqualified) return Phase::Pending;

    if (nowMs - startMs_ >= kLongPressMs) {
      phase_ = Phase::Fired;
      return Phase::Fired;
    }
    return Phase::Pending;
  }

  // Where the finger was anchored -- the point to sample, which is the PRESS
  // position rather than wherever the finger has drifted to by the time the
  // hold completes.
  float anchorX() const { return startX_; }
  float anchorY() const { return startY_; }

  void reset() { phase_ = Phase::Idle; }

 private:
  Phase phase_ = Phase::Idle;
  float startX_ = 0.0f;
  float startY_ = 0.0f;
  double startMs_ = 0.0;
};

}  // namespace np
