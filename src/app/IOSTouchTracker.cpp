#include "app/IOSTouchTracker.hpp"

namespace np {

IOSTouchTracker& iosTouchTracker() {
  static IOSTouchTracker tracker;
  return tracker;
}

IOSTouchTracker::Touch* IOSTouchTracker::find(uint64_t id) noexcept {
  for (size_t i = 0; i < touchCount_; ++i)
    if (touches_[i].id == id) return &touches_[i];
  return nullptr;
}

void IOSTouchTracker::down(uint64_t id, float x, float y, uint64_t timestampNs) {
  if (find(id) != nullptr) return;  // a duplicate DOWN for an id already tracked: ignore it
  if (touchCount_ < kMaxTracked) touches_[touchCount_++] = Touch{id, x, y};
  updateEpisode(timestampNs);
}

void IOSTouchTracker::motion(uint64_t id, float x, float y) {
  if (Touch* t = find(id)) {
    t->x = x;
    t->y = y;
    updateEpisode(0);  // a motion cannot itself end or begin a 2-finger episode
  }
}

void IOSTouchTracker::up(uint64_t id, uint64_t timestampNs) {
  for (size_t i = 0; i < touchCount_; ++i) {
    if (touches_[i].id != id) continue;
    // Compact: move the last entry into this slot, matching `down()`'s own
    // "order does not matter, only membership" contract -- nothing here
    // reads `touches_` by position, only by `id` (`find()`) or by "the first
    // two" when `touchCount_ == 2` exactly (`oneFingerTouch()`/
    // `twoFingerTouch()`/`updateEpisode()`), which compaction preserves.
    touches_[i] = touches_[touchCount_ - 1];
    --touchCount_;
    break;
  }
  updateEpisode(timestampNs);
}

int IOSTouchTracker::count() const noexcept { return static_cast<int>(touchCount_); }

std::optional<TrackpadTouchPoint> IOSTouchTracker::oneFingerTouch() const noexcept {
  if (touchCount_ != 1) return std::nullopt;
  return TrackpadTouchPoint{touches_[0].id, touches_[0].x, touches_[0].y};
}

std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>>
IOSTouchTracker::twoFingerTouch() const noexcept {
  if (touchCount_ != 2) return std::nullopt;
  return std::make_pair(TrackpadTouchPoint{touches_[0].id, touches_[0].x, touches_[0].y},
                        TrackpadTouchPoint{touches_[1].id, touches_[1].x, touches_[1].y});
}

bool IOSTouchTracker::consumeTwoFingerTapUndo() noexcept {
  const bool pending = tapPending_;
  tapPending_ = false;
  return pending;
}

void IOSTouchTracker::updateEpisode(uint64_t timestampNs) noexcept {
  const bool nowTwo = touchCount_ == 2;
  if (nowTwo && !twoFingerEpisodeOpen_) {
    // Rising edge: a fresh episode, judged from here.
    twoFingerEpisodeOpen_ = true;
    episodeStartNs_ = timestampNs;
    episodeStartMidX_ = (touches_[0].x + touches_[1].x) * 0.5f;
    episodeStartMidY_ = (touches_[0].y + touches_[1].y) * 0.5f;
    episodeMaxMoveSq_ = 0.0f;
    return;
  }
  if (nowTwo && twoFingerEpisodeOpen_) {
    // Still two: track the farthest the pair's own midpoint has strayed from
    // where the episode began -- a pinch/rotate whose midpoint happens to
    // return near its start by the time both fingers lift must not read as a
    // tap, and the RUNNING max (not the final position) is what catches
    // that.
    const float midX = (touches_[0].x + touches_[1].x) * 0.5f;
    const float midY = (touches_[0].y + touches_[1].y) * 0.5f;
    const float dx = midX - episodeStartMidX_;
    const float dy = midY - episodeStartMidY_;
    const float moveSq = dx * dx + dy * dy;
    if (moveSq > episodeMaxMoveSq_) episodeMaxMoveSq_ = moveSq;
    return;
  }
  if (!nowTwo && twoFingerEpisodeOpen_) {
    // Falling edge -- either a lift (count dropped to 0 or 1) or a third
    // finger joined (count rose to 3+, `down()`'s own call into this). Both
    // end the episode the same way: judge what it did, once.
    twoFingerEpisodeOpen_ = false;
    const uint64_t elapsed = timestampNs >= episodeStartNs_ ? timestampNs - episodeStartNs_ : 0;
    constexpr float kThresholdSq = kTapMoveThreshold * kTapMoveThreshold;
    if (elapsed <= kTapMaxDurationNs && episodeMaxMoveSq_ <= kThresholdSq) tapPending_ = true;
  }
}

}  // namespace np
