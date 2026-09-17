#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "app/TouchGesture.hpp"  // TrackpadTouchPoint

namespace np {

// app/IOSTouchTracker -- the raw SDL_EVENT_FINGER_* capture layer for a real
// touchscreen, the iOS half of what `ui/MacTrackpadTouch.hpp` is for a Mac's
// indirect trackpad.
//
// **Why this needs no ObjC, unlike `ui/MacTrackpadTouch.mm`.** A trackpad's
// raw touches only reach a VIEW that opted in (`-setAcceptsTouchEvents:`),
// which is why that file has to splice into AppKit's responder chain at all.
// SDL already turns a touchscreen's raw contacts into its own
// `SDL_EVENT_FINGER_DOWN`/`MOTION`/`UP`, cross-platform, no view or window
// object required -- `main.cpp`'s existing `SDL_PollEvent()` loop already
// sees every one of them; this file just has to keep the ones this app
// wants. `SDL_TouchFingerEvent::x`/`y` are already normalized to the WINDOW
// (0,0 top-left, 1,1 bottom-right, y-DOWN) -- `TrackpadTouchPoint`'s exact
// contract, and no flip is needed the way `ui/MacTrackpadTouch.mm` needs one
// for `NSTouch.normalizedPosition`'s y-UP convention.
//
// **Why this exists at all, separately from `app/PointerQueue`.** A finger
// touch reaches this app TWICE: once as SDL's own synthesised
// `SDL_TOUCH_MOUSEID` mouse event (which keeps UI hit-testing and, on every
// platform but iOS, canvas painting working -- see `app/PointerQueue.hpp`
// section 1's own comment on why that synthesis is kept for a mouse-driven
// build), and once as the raw `SDL_EVENT_FINGER_*` this file reads. On iOS,
// where a finger must never draw (docs/ios-spike-plan.md's follow-up: "touch
// is for UI and pan/zoom, the pencil draws"), the raw stream is the only one
// that can tell a genuine one- or two-finger touch gesture apart from the
// pencil's own `SDL_PEN_MOUSEID` synthesis, which looks identical to a
// finger's `SDL_TOUCH_MOUSEID` synthesis by the time either reaches ImGui's
// mouse state.
//
// **Two-finger tap, not just the pair's own motion.** `app/TouchGesture.hpp`
// (`computeTwoTouchDelta()`) already answers "how has this pair moved since
// it began" for pinch/pan; what it does not answer is "did the pair ever
// move at all before both fingers lifted" -- a tap needs the NEGATIVE of a
// gesture, not one of its own outputs. This class watches for exactly that:
// a two-finger episode short enough and still enough to read as one input
// (mapped to Undo, `docs/ios-spike-plan.md`'s follow-up), consumed once by
// the caller the same one-shot way every other request flag in this codebase
// is (set here, read and cleared by `main.cpp`/`ui/MacPaintUI.cpp`).
class IOSTouchTracker {
 public:
  // One call per `SDL_EVENT_FINGER_DOWN`/`MOTION`/`UP` (and `CANCELED`,
  // folded into `up()` by the caller -- a cancelled touch is a lifted one as
  // far as this tracker's own state is concerned). `id` is `SDL_FingerID`,
  // opaque and stable for one finger's whole contact, matching
  // `TrackpadTouchPoint::identity`'s own contract.
  void down(uint64_t id, float x, float y, uint64_t timestampNs);
  void motion(uint64_t id, float x, float y);
  void up(uint64_t id, uint64_t timestampNs);

  // How many fingers are down right now (capped at reporting 3 for "three or
  // more", since nothing here distinguishes a fourth from a fifth).
  int count() const noexcept;

  // Exactly one finger down right now, or `std::nullopt` for any other
  // count -- the caller's own signal to pan-not-draw with it.
  std::optional<TrackpadTouchPoint> oneFingerTouch() const noexcept;

  // Exactly two fingers down right now, matched by identity -- `nullopt` for
  // any other count. Same contract `ui/MacTrackpadTouch.hpp::
  // pollTwoFingerTouch()` already has, so `app/TouchGestureSession` and
  // `app/TouchGesture.hpp::computeTwoTouchDelta()` need no changes at all to
  // drive pinch/rotate/pan from this instead of a trackpad.
  std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> twoFingerTouch() const noexcept;

  // One-shot: true the first time this is called after a two-finger episode
  // ended having moved less than `kTapMoveThreshold` (in the same
  // dimensionless [0,1] window-fraction units `x`/`y` are) and lasted less
  // than `kTapMaxDurationNs`. Clears itself on read, exactly like this
  // codebase's other one-shot request flags (`AppState::showDocumentGallery`
  // and its neighbours) -- a tap fires the action once, not once per frame
  // it happens to still be true.
  bool consumeTwoFingerTapUndo() noexcept;

  // The thresholds a two-finger touch is judged a TAP by, rather than a
  // pan/pinch that happened to end quickly. Both dimensionless: 0.02 of the
  // window's shorter dimension is a few mm on an iPad, comfortably more than
  // finger-tremor jitter and comfortably less than any deliberate pan.
  static constexpr float kTapMoveThreshold = 0.02f;
  static constexpr uint64_t kTapMaxDurationNs = 400'000'000ull;  // 400 ms

 private:
  struct Touch {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f;
  };
  // At most 4: this app only ever asks "is it exactly 1" or "is it exactly
  // 2", so nothing past a third finger changes any answer -- capped rather
  // than unbounded so a phone-sized touch digitiser reporting a phantom
  // contact storm cannot grow this without limit.
  static constexpr size_t kMaxTracked = 4;
  Touch touches_[kMaxTracked]{};
  size_t touchCount_ = 0;

  // The two-finger episode currently being watched for a tap -- valid only
  // while `twoFingerEpisodeOpen_` is true, which tracks "was `count()`
  // exactly 2 as of the last down/up/motion", the transition INTO which
  // starts a fresh episode and the transition OUT OF which (either direction:
  // a lift, or a third finger joining) judges it.
  bool twoFingerEpisodeOpen_ = false;
  uint64_t episodeStartNs_ = 0;
  float episodeStartMidX_ = 0.0f, episodeStartMidY_ = 0.0f;
  float episodeMaxMoveSq_ = 0.0f;
  bool tapPending_ = false;

  Touch* find(uint64_t id) noexcept;
  void updateEpisode(uint64_t timestampNs) noexcept;
};

// The single instance `main.cpp`'s SDL event loop feeds from
// `SDL_EVENT_FINGER_*` and `ui/MacTrackpadTouch.hpp`'s iOS branch reads from
// -- a Meyer's singleton for the identical reason `ui/MacTrackpadTouch.mm`'s
// own capture state is file-static: exactly one touchscreen, exactly one
// tracker, and every caller on every platform this app builds for already
// reaches trackpad/touch state through a bare function call with no object
// to thread through (`pollTwoFingerTouch()` and its neighbours take no
// arguments at all).
IOSTouchTracker& iosTouchTracker();

}  // namespace np
