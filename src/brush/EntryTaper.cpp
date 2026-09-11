#include "brush/EntryTaper.hpp"

#include <algorithm>

namespace np {

float entryTaperMultiplier(float distanceTravelledPx, float taperInPx,
                           float taperMinSizePct) noexcept {
  if (taperInPx <= 0.0f) return 1.0f;
  const float t = std::clamp(distanceTravelledPx / taperInPx, 0.0f, 1.0f);
  const float s = t * t * (3.0f - 2.0f * t);  // smoothstep
  const float minFrac = taperMinSizePct / 100.0f;
  return minFrac + (1.0f - minFrac) * s;
}

}  // namespace np
