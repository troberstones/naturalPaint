#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>

#include "app/StrokeSession.hpp"

// app/StrokeSession's applyPerDabScatter(): docs/reachability-audit.md B5's
// second defect. Ours used to draw SCATTER's angle off the whole circle
// unconditionally, which smears a dab ALONG the stroke as much as across it
// -- a blurrier line, not a rougher one. Photoshop confines it to the
// stroke's own PERPENDICULAR unless "Both Axes" is ticked, and that is off
// by default.
//
// **Asserted geometrically**, per the audit item: this section projects each
// dab's actual displacement onto the stroke's tangent and its perpendicular
// and checks the two components directly, rather than trusting a flag that
// merely says which branch ran.
namespace np {

bool runScatterTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  BrushTip tip;
  tip.radius = 20.0f;
  tip.scatter = 0.5f;  // radii -- magnitude = 0.5 * 20 = 10 px
  const float kMagnitude = tip.scatter * tip.radius;

  // --- 1. The identity: no scatter linked, nothing moves --------------------
  {
    BrushTip zero = tip;
    zero.scatter = 0.0f;
    const Vec2 c{100.0f, 200.0f};
    const Vec2 perp = applyPerDabScatter(c, zero, 12345ull, 0, 3.0f, 4.0f);
    zero.scatterBothAxes = true;
    const Vec2 iso = applyPerDabScatter(c, zero, 12345ull, 0, 3.0f, 4.0f);
    check(perp.x == c.x && perp.y == c.y && iso.x == c.x && iso.y == c.y,
          "scatter: tip.scatter == 0 is the identity in BOTH axis modes -- no draw spent");
  }

  // --- 2. Perpendicular by default: tangent component is ~0, exactly -------
  //
  // Stroke step (3, 4) -- a non-axis-aligned tangent, so a defect that leaked
  // scatter onto the wrong axis could not hide behind a coincidental zero
  // component the way (1, 0) or (0, 1) would let it.
  {
    const float stepDx = 3.0f, stepDy = 4.0f;
    const float tLen = std::sqrt(stepDx * stepDx + stepDy * stepDy);
    const float tanX = stepDx / tLen, tanY = stepDy / tLen;
    // The perpendicular unit, 90 degrees from the tangent -- independent of
    // `applyPerDabScatter()`'s own internal `atan2`/`kHalfPi` construction,
    // so this is a check against the GEOMETRY, not against a mirror of the
    // implementation.
    const float perpX = -tanY, perpY = tanX;

    const Vec2 centre{500.0f, 500.0f};
    constexpr uint32_t kDraws = 400;
    float maxAbsTangent = 0.0f;
    float minPerp = 1e9f, maxPerp = -1e9f;
    bool sawNegativeSide = false, sawPositiveSide = false;
    for (uint32_t i = 0; i < kDraws; ++i) {
      const Vec2 got = applyPerDabScatter(centre, tip, 0x9e3779b97f4a7c15ull, i, stepDx, stepDy);
      const float dx = got.x - centre.x, dy = got.y - centre.y;
      const float alongTangent = dx * tanX + dy * tanY;
      const float alongPerp = dx * perpX + dy * perpY;
      maxAbsTangent = std::max(maxAbsTangent, std::fabs(alongTangent));
      minPerp = std::min(minPerp, alongPerp);
      maxPerp = std::max(maxPerp, alongPerp);
      if (alongPerp < 0.0f) sawNegativeSide = true;
      if (alongPerp > 0.0f) sawPositiveSide = true;
    }
    std::printf("  scatter: perpendicular mode, max |tangent component| over %u draws = %.8f px\n",
                kDraws, static_cast<double>(maxAbsTangent));
    std::printf("  scatter: perpendicular mode, perpendicular component range = [%.4f, %.4f] px\n",
                static_cast<double>(minPerp), static_cast<double>(maxPerp));

    // Tolerance derived empirically: the geometry is exact by construction
    // (every displacement is `magnitude` times a unit vector built from
    // `std::cos`/`std::sin` of `std::atan2(stepDy, stepDx) + kHalfPi`), so the
    // only residual along the tangent is float rounding in that trig chain.
    // Measured above at magnitude 10 px, the observed maximum is on the order
    // of 1e-6 px; 0.01 px (1e-3 of the 10 px magnitude) is three orders of
    // magnitude above that measurement and eleven hundred times smaller than
    // the magnitude itself, so it cannot be crossed by rounding but would be
    // crossed at once by a real leak onto the tangent axis.
    check(maxAbsTangent < 0.01f,
          "scatter: perpendicular mode -- displacement along the stroke's TANGENT is ~zero for "
          "every draw (float-rounding scale, not a fraction of the 10 px magnitude)");
    check(maxPerp > kMagnitude * 0.9f && minPerp < -kMagnitude * 0.9f,
          "scatter: perpendicular mode -- displacement across the stroke (the PERPENDICULAR "
          "component) reaches close to +/- the full magnitude on both sides");
    check(sawNegativeSide && sawPositiveSide,
          "scatter: perpendicular mode -- both sides of the stroke are reached, not always the "
          "same one");
  }

  // --- 3. Isotropic when Both Axes is set: the tangent is NOT confined -----
  //
  // The contrasting case, same tip and same stroke, only `scatterBothAxes`
  // flipped. If section 2's near-zero tangent component were an accident of
  // this stroke's geometry rather than the perpendicular confinement, this
  // section would ALSO read near-zero -- it does not.
  {
    BrushTip iso = tip;
    iso.scatterBothAxes = true;
    const float stepDx = 3.0f, stepDy = 4.0f;
    const float tLen = std::sqrt(stepDx * stepDx + stepDy * stepDy);
    const float tanX = stepDx / tLen, tanY = stepDy / tLen;

    const Vec2 centre{500.0f, 500.0f};
    constexpr uint32_t kDraws = 400;
    float minTangent = 1e9f, maxTangent = -1e9f;
    for (uint32_t i = 0; i < kDraws; ++i) {
      const Vec2 got = applyPerDabScatter(centre, iso, 0x9e3779b97f4a7c15ull, i, stepDx, stepDy);
      const float dx = got.x - centre.x, dy = got.y - centre.y;
      const float alongTangent = dx * tanX + dy * tanY;
      minTangent = std::min(minTangent, alongTangent);
      maxTangent = std::max(maxTangent, alongTangent);
    }
    const float spread = maxTangent - minTangent;
    std::printf("  scatter: isotropic mode, tangent component range = [%.4f, %.4f] px (spread %.4f)\n",
                static_cast<double>(minTangent), static_cast<double>(maxTangent),
                static_cast<double>(spread));

    // A full-circle draw's tangent component is `magnitude * cos(angle)` for
    // a uniform angle, so over 400 draws it should sweep close to the whole
    // [-magnitude, +magnitude] range. The threshold is half the magnitude
    // (5 px of 10): far enough above the perpendicular case's measured
    // ~1e-6 px spread that the two modes cannot be confused, and comfortably
    // below what 400 draws of a real full-circle spread achieves (measured
    // above), so this is not a coin flip against the sampling.
    check(spread > kMagnitude * 0.5f,
          "scatter: isotropic mode -- the tangent component varies WIDELY across draws (spread > "
          "half the magnitude), unlike the perpendicular mode's near-zero spread -- proof by "
          "contrast, on the same stroke and the same tip");
  }

  // --- 4. The first dab: no tangent yet reads as due +x, not undefined -----
  //
  // `depositPending()`'s own contract for its first dab is `stepDx = stepDy =
  // 0.0` (brush/Dynamics.hpp's `dynamicDirection()` states the identical
  // convention for DIRECTION). `std::atan2(0, 0) == 0` -- "due +x" -- so the
  // first dab's perpendicular is due +y: displacement.x should be ~0 and
  // displacement.y should reach the full magnitude, deterministically.
  {
    const Vec2 centre{0.0f, 0.0f};
    float maxAbsX = 0.0f;
    bool sawFullMagnitudeY = false;
    for (uint32_t i = 0; i < 100; ++i) {
      const Vec2 got = applyPerDabScatter(centre, tip, 0xabc123ull, i, 0.0f, 0.0f);
      maxAbsX = std::max(maxAbsX, std::fabs(got.x));
      if (std::fabs(std::fabs(got.y) - kMagnitude) < 1e-3f) sawFullMagnitudeY = true;
    }
    std::printf("  scatter: first-dab (0,0) step, max |x displacement| over 100 draws = %.8f px\n",
                static_cast<double>(maxAbsX));
    // Same rounding-scale tolerance as section 2, restated for this
    // dedicated zero-vector case rather than assumed to fall out of it.
    check(maxAbsX < 0.01f,
          "scatter: a (0,0) step -- the stroke's first dab -- scatters along Y, not X, exactly "
          "as `dynamicDirection()`'s own \"due +x\" convention for no-tangent-yet implies");
    check(sawFullMagnitudeY,
          "scatter: and the Y displacement reaches the full magnitude, on at least one draw of "
          "100");
  }

  std::printf("[selftest] scatter %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
