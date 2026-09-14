#include "brush/QuickMaskPaint.hpp"

#include "core/SelectionOps.hpp"

namespace np {

size_t paintQuickMaskDab(QuickMask& mask, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                         int32_t canvasH, bool erase) {
  if (!(tip.flow > 0.0f)) return 0;
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return 0;

  const SelectionCombine op = erase ? SelectionCombine::Subtract : SelectionCombine::Add;
  size_t texels = 0;
  for (int32_t y = b.y0; y <= b.y1; ++y) {
    const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
    for (int32_t x = b.x0; x <= b.x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
      const float cov = dabCoverage(tip, dx, dy);
      if (!(cov > 0.0f)) continue;
      const PixelCoord texel{x, y};
      const float before = quickMaskCoverageAt(mask, texel);
      // Opacity folded into the per-dab weight, not latched as a separate
      // ceiling the way brush/RgbDeposit's is -- see this file's header:
      // `max`/`min` are already idempotent, so a weight capped at
      // `flow * opacity` never climbs past it no matter how many dabs of a
      // lingering stroke propose the identical bound, which is a per-stroke
      // ceiling MaskPaint needs an accumulator to get and this gets for free.
      const float after = combineCoverage(before, tip.flow * tip.opacity * cov, op);
      // Same value in, same value quantised out -- `paintQuickMask()` would
      // skip this write anyway, but testing here also skips the redundant
      // find()/getOrCreate() pair on a texel this dab cannot change.
      if (after == before) continue;
      paintQuickMask(mask, texel, after);
      ++texels;
    }
  }
  return texels;
}

}  // namespace np
