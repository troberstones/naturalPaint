#include "brush/CoverageBlend.hpp"

#include <algorithm>

namespace np {

const char* coverageBlendName(CoverageBlend blend) noexcept {
  switch (blend) {
    case CoverageBlend::Multiply: return "Multiply";
    case CoverageBlend::Overlay: return "Overlay";
    case CoverageBlend::ColorBurn: return "Color Burn";
    case CoverageBlend::HardMix: return "Hard Mix";
    case CoverageBlend::LinearBurn: return "Linear Burn";
    case CoverageBlend::ColorDodge: return "Color Dodge";
    case CoverageBlend::Darken: return "Darken";
    case CoverageBlend::Subtract: return "Subtract";
    case CoverageBlend::Height: return "Height";
    case CoverageBlend::LinearHeight: return "Linear Height";
  }
  return "unknown blend";
}

bool coverageBlendIsRenderable(CoverageBlend blend) noexcept {
  return blend != CoverageBlend::LinearHeight;
}

float applyCoverageBlend(CoverageBlend blend, float base, float second) noexcept {
  const float a = std::clamp(base, 0.0f, 1.0f);
  const float b = std::clamp(second, 0.0f, 1.0f);

  // **No blend may create coverage where the primary has none.** Both callers
  // depend on this and neither could recover from its absence: a dual brush's
  // second tip must not paint outside the first tip's footprint, and grain
  // "can only thin or empty a texel already inside the footprint, never add
  // one outside it" (brush/Deposit.cpp's own §2e comment) -- the deposit
  // loops only visit texels `dabCoverage()` already accepted, so a blend that
  // returned more than zero at zero would be writing outside the bounds the
  // caller computed.
  //
  // Only Hard Mix can violate it, since `0 + second >= 1` is satisfiable.
  // This guard came from `combineDualCoverage()`, which had it and which this
  // function replaces; folding it in here rather than leaving it at one call
  // site is what makes the property hold for the grain caller too.
  if (!(a > 0.0f)) return 0.0f;

  switch (blend) {
    case CoverageBlend::Multiply:
      return a * b;
    case CoverageBlend::Overlay:
      // The standard separable form. Written with the `a < 0.5f` test rather
      // than a branchless mix because the two halves are genuinely different
      // formulas and a reader should see that.
      return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
    case CoverageBlend::ColorBurn:
      // The three-way form, matching the reference `combineDualCoverage()`
      // was checked against (Ryan Juckett's published HLSL translation of
      // Photoshop's blend modes): a saturated `a` stays white without
      // dividing, and `b == 0` is the formula's pole, defined as 0 -- which
      // is also its analytic continuation, since `(1-a)/b -> +inf` as
      // `b -> 0+`, `min(1, .) -> 1`, and `1 - 1 == 0`.
      if (a >= 1.0f) return 1.0f;
      if (b <= 0.0f) return 0.0f;
      return 1.0f - std::min(1.0f, (1.0f - a) / b);
    case CoverageBlend::HardMix:
      // A hard threshold, which is what makes it the only mode here whose
      // output is never between 0 and 1. The `a > 0` half of the guard is at
      // the top of this function; see there.
      return (a + b) >= 1.0f ? 1.0f : 0.0f;
    case CoverageBlend::LinearBurn:
      return std::max(0.0f, a + b - 1.0f);
    case CoverageBlend::ColorDodge:
      // The mirror singularity: `b == 1` divides by zero and the answer is
      // white.
      return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b));
    case CoverageBlend::Darken:
      return std::min(a, b);
    case CoverageBlend::Subtract:
    case CoverageBlend::Height:
      // The same arithmetic for both, deliberately and visibly -- see the
      // enum's own comment on Height. Zimmer's `clamp(P*S*O1 - G, 0, 1)` with
      // the caller having already folded `S` and `O1` into `base`.
      return std::max(0.0f, a - b);
    case CoverageBlend::LinearHeight:
      // Not a formula. The primary coverage, unchanged, so a caller that did
      // not ask `coverageBlendIsRenderable()` gets the first tip alone rather
      // than a guess that paints.
      return a;
  }
  return a;
}

}  // namespace np
