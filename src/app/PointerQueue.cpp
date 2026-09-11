#include "app/PointerQueue.hpp"

#include <algorithm>

namespace np {

namespace {

// `PointerAxis` -> `axisSinceMotion_` slot; -1 for the axes nothing reads.
int axisSlot(PointerAxis a) noexcept {
  switch (a) {
    case PointerAxis::Pressure: return 0;
    case PointerAxis::TiltX: return 1;
    case PointerAxis::TiltY: return 2;
    case PointerAxis::Rotation: return 3;
    case PointerAxis::Other: return -1;
  }
  return -1;
}

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

}  // namespace

bool PointerQueue::processed(uint32_t seq) const noexcept {
  // Serial-number order (RFC 1982 style): `seq` precedes the bound when the
  // signed 32-bit distance is negative, which stays right across the
  // counter's wrap. Before the first `beginFrame()` nothing is processed.
  return haveBound_ && static_cast<int32_t>(seq - uiProcessedBound_) < 0;
}

PointerQueue::Gesture* PointerQueue::find(uint64_t id) noexcept {
  for (Gesture& g : gestures_)
    if (g.id == id) return &g;
  return nullptr;
}

const PointerQueue::Gesture* PointerQueue::find(uint64_t id) const noexcept {
  for (const Gesture& g : gestures_)
    if (g.id == id) return &g;
  return nullptr;
}

uint64_t PointerQueue::openGesture(bool pen, uint32_t downSeq) {
  // Section 4's record bound: the oldest record goes, with its samples.
  if (gestures_.size() >= kMaxGestures) {
    const uint64_t victim = gestures_.front().id;
    gestures_.erase(gestures_.begin());
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [victim](const PointerSample& s) { return s.gesture == victim; }),
                   samples_.end());
    if (openPen_ == victim) openPen_ = 0;
    if (openMouse_ == victim) openMouse_ = 0;
    if (penDownSeqPending_ == victim) penDownSeqPending_ = 0;
    if (penUpSeqPending_ == victim) penUpSeqPending_ = 0;
  }
  Gesture g;
  g.id = nextGesture_++;
  g.pen = pen;
  g.downSeq = downSeq;
  gestures_.push_back(g);
  return g.id;
}

void PointerQueue::endGesture(uint64_t id, uint32_t endSeq) noexcept {
  if (Gesture* g = find(id)) {
    g->ended = true;
    g->endSeq = endSeq;
  }
}

void PointerQueue::enqueue(const PointerEvent& e, bool pen, uint64_t gesture) {
  // Section 4's sample bound: a full queue evicts its OLDEST sample.
  if (samples_.size() >= kMaxSamples) samples_.erase(samples_.begin());
  PointerSample s;
  s.x = e.x;
  s.y = e.y;
  s.isPen = pen;
  s.timestamp = e.timestamp;
  s.gesture = gesture;
  if (pen) {
    s.pressure = latest_.pressure;
    s.tiltXDeg = latest_.tiltXDeg;
    s.tiltYDeg = latest_.tiltYDeg;
    s.rotationDeg = latest_.rotationDeg;
  }
  // A mouse sample keeps `PointerSample`'s own defaults: pressure 1.0 and
  // 0/0/0 degrees, a mouse's neutral reading (that struct's comment).
  samples_.push_back(s);
}

void PointerQueue::patchAxis(const PointerEvent& e) noexcept {
  const int slot = axisSlot(e.axis);
  if (slot < 0) return;
  // Rule (b)'s condition, read BEFORE this event marks its axis as reported.
  const bool firstOfItsAxis = !axisSinceMotion_[static_cast<size_t>(slot)];
  axisSinceMotion_[static_cast<size_t>(slot)] = true;
  // Only the open (in-contact) gesture is ever patched -- section 2.
  if (openPen_ == 0) return;

  bool newest = true;
  for (auto it = samples_.rbegin(); it != samples_.rend() && it->gesture == openPen_; ++it) {
    const bool sameReport = it->timestamp == e.timestamp;       // rule (a)
    const bool structural = newest && firstOfItsAxis;           // rule (b)
    if (!sameReport && !structural) break;
    switch (e.axis) {
      case PointerAxis::Pressure: it->pressure = clamp01(e.value); break;
      case PointerAxis::TiltX: it->tiltXDeg = e.value; break;
      case PointerAxis::TiltY: it->tiltYDeg = e.value; break;
      case PointerAxis::Rotation: it->rotationDeg = e.value; break;
      case PointerAxis::Other: break;
    }
    newest = false;
  }
}

void PointerQueue::push(const PointerEvent& e) {
  switch (e.kind) {
    case PointerEventKind::PenAxis:
      switch (e.axis) {
        case PointerAxis::Pressure: latest_.pressure = clamp01(e.value); break;
        case PointerAxis::TiltX: latest_.tiltXDeg = e.value; break;
        case PointerAxis::TiltY: latest_.tiltYDeg = e.value; break;
        case PointerAxis::Rotation: latest_.rotationDeg = e.value; break;
        case PointerAxis::Other: break;
      }
      patchAxis(e);
      break;

    case PointerEventKind::PenDown:
      // A second down without an up cannot come from one pen; end the stale
      // gesture where the new one starts rather than leave it open forever.
      if (openPen_ != 0) endGesture(openPen_, e.uiSeq);
      openPen_ = openGesture(/*pen=*/true, e.uiSeq);
      // Provisional: refined by the synthesised mouse button-down SDL pushes
      // right behind this event, which is the press ImGui sees (section 3).
      // If pen mouse emulation is off there is none, ImGui never sees a pen
      // press either, and this provisional stamp is as good as any.
      penDownSeqPending_ = openPen_;
      enqueue(e, /*pen=*/true, openPen_);
      break;

    case PointerEventKind::PenUp:
      if (openPen_ != 0) {
        endGesture(openPen_, e.uiSeq);
        penUpSeqPending_ = openPen_;
        openPen_ = 0;
      }
      break;

    case PointerEventKind::PenMotion:
      // Rule (b)'s report boundary: a position event, queued or not, starts
      // a new report on every backend.
      axisSinceMotion_.fill(false);
      // Hover (no open gesture) is never queued -- section 1.
      if (openPen_ != 0) enqueue(e, /*pen=*/true, openPen_);
      break;

    case PointerEventKind::MouseButtonDown:
      if (!e.left) break;
      if (e.isPenSynthesizedMouse) {
        // Never a sample (section 1); only the date of the pen's press.
        if (Gesture* g = find(penDownSeqPending_)) g->downSeq = e.uiSeq;
        penDownSeqPending_ = 0;
        break;
      }
      if (openMouse_ != 0) endGesture(openMouse_, e.uiSeq);
      openMouse_ = openGesture(/*pen=*/false, e.uiSeq);
      enqueue(e, /*pen=*/false, openMouse_);
      break;

    case PointerEventKind::MouseButtonUp:
      if (!e.left) break;
      if (e.isPenSynthesizedMouse) {
        if (Gesture* g = find(penUpSeqPending_)) g->endSeq = e.uiSeq;
        penUpSeqPending_ = 0;
        break;
      }
      // Never a sample (section 1) -- it ends the gesture.
      if (openMouse_ != 0) {
        endGesture(openMouse_, e.uiSeq);
        openMouse_ = 0;
      }
      break;

    case PointerEventKind::MouseMotion:
      if (e.isPenSynthesizedMouse) break;  // the pen's own duplicate
      if (!e.left) break;                  // hover
      if (openMouse_ != 0) enqueue(e, /*pen=*/false, openMouse_);
      break;
  }
}

void PointerQueue::beginFrame(uint32_t uiProcessedBound) noexcept {
  uiProcessedBound_ = uiProcessedBound;
  haveBound_ = true;
}

uint64_t PointerQueue::claimGesture() noexcept {
  for (Gesture& g : gestures_) {
    if (g.claimed || !processed(g.downSeq)) continue;
    if (g.ended && processed(g.endSeq)) continue;
    g.claimed = true;
    return g.id;
  }
  return 0;
}

std::vector<PointerSample> PointerQueue::takeForStroke(uint64_t gesture) {
  std::vector<PointerSample> out;
  const Gesture* g = find(gesture);
  if (g == nullptr || !processed(g->downSeq)) return out;
  auto keep = std::stable_partition(samples_.begin(), samples_.end(),
                                    [gesture](const PointerSample& s) { return s.gesture != gesture; });
  out.assign(keep, samples_.end());
  samples_.erase(keep, samples_.end());
  return out;
}

void PointerQueue::endFrame() {
  // Offered and not taken: dropped (section 3). A sample whose gesture record
  // is gone (evicted) is dropped with it.
  samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                [this](const PointerSample& s) {
                                  const Gesture* g = find(s.gesture);
                                  return g == nullptr || processed(g->downSeq);
                                }),
                 samples_.end());
  // Over from ImGui's side: forgotten, claimed or not.
  gestures_.erase(std::remove_if(gestures_.begin(), gestures_.end(),
                                 [this](const Gesture& g) { return g.ended && processed(g.endSeq); }),
                  gestures_.end());
}

}  // namespace np
