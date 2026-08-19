#pragma once
#include <cstdint>
#include <vector>

namespace np {

// Pen-to-photon latency (PRD F3): the gap between an input event arriving and
// the frame it produced actually reaching the screen. Samples accumulate for
// one stroke at a time; p50/p99 print when the stroke ends.
class Latency {
 public:
  // --latency: also print a line for every sample, not just the stroke summary.
  void setVerbose(bool v) { verbose_ = v; }

  // Call once per frame, right after wgpuSurfacePresent. `drewPoint` is
  // whether this frame's brushActive was set; `inputEventNs` is the
  // freshest relevant input timestamp seen during this frame's poll loop, or
  // 0 if none arrived this frame (a held stroke with no new samples, say).
  void recordFrame(bool drewPoint, uint64_t inputEventNs, uint64_t presentNs);

  // Call on the strokeActive: true -> false transition. Prints p50/p99 for
  // whatever samples were collected and clears them for the next stroke.
  void endStroke();

 private:
  std::vector<float> samplesMs_;
  bool verbose_ = false;
};

}  // namespace np
