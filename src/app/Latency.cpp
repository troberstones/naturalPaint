#include "app/Latency.hpp"

#include <algorithm>
#include <cstdio>

namespace np {
namespace {

float percentile(const std::vector<float>& sortedMs, float p) {
  if (sortedMs.empty()) return 0.0f;
  const size_t idx = static_cast<size_t>(p * static_cast<float>(sortedMs.size() - 1));
  return sortedMs[idx];
}

}  // namespace

void Latency::recordFrame(bool drewPoint, uint64_t inputEventNs, uint64_t presentNs) {
  if (!drewPoint || inputEventNs == 0 || presentNs <= inputEventNs) return;
  const float ms = static_cast<float>(presentNs - inputEventNs) / 1e6f;
  samplesMs_.push_back(ms);
  if (verbose_) std::printf("[latency] frame: %.2fms\n", ms);
}

void Latency::endStroke() {
  if (samplesMs_.empty()) return;
  std::sort(samplesMs_.begin(), samplesMs_.end());
  const float p50 = percentile(samplesMs_, 0.50f);
  const float p99 = percentile(samplesMs_, 0.99f);
  std::printf("[latency] stroke: n=%zu p50=%.1fms p99=%.1fms\n", samplesMs_.size(), p50, p99);
  samplesMs_.clear();
}

}  // namespace np
