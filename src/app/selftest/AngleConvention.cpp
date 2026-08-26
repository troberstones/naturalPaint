#include "app/selftest/Support.hpp"

#include <cmath>

#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"

namespace np {

// ---------------------------------------------------------------------------
// track10/angle -- is the angle input interpreted correctly?
//
// Four questions, this file's own share of the answer:
//
//   1. Self-consistency: does DIRECTION's tangent actually turn the tip to
//      FACE the stroke, or is it off by 90 degrees, mirrored, or reversed?
//   3. Convention: is `BrushTip::angle`'s positive sense clockwise or
//      counter-clockwise on screen?
//
// (Questions 2 and 4 are answered by io/AbrBrushes.cpp's own comment on its
// `Angl` line and by `app/selftest/AbrBrushes.cpp`'s section 8, respectively
// -- this file has no importer in it at all, on purpose: everything below is
// `brush/Deposit.hpp` and `brush/Dynamics.hpp` alone, so it stays true
// whether or not any `.abr` is ever opened.)
//
// **The method, both times: query `dabCoverage()`, never read an angle
// number back out of the code under test.** A wrong sign or a swapped axis
// in `dynamicDirection()` or in `dabCoverage()`'s own rotation could still
// produce a plausible-looking DEGREE value -- the wrap tests in
// `DynamicsSources.cpp` already prove the numbers `dynamicDirection()`
// returns are the ones `atan2` gives, by hand, at the cardinal headings. What
// they do not prove is that the DAB actually painted BY those numbers points
// where the numbers claim. So every check here instead asks "does the tip's
// elongated footprint actually reach a point at this WORLD offset", building
// the query point from a hand-picked vector (never from `cos(tip.angle)`
// re-derived) and reading `dabCoverage()`'s real answer at it -- the two
// numbers this test could get backwards (the query angle and the tip's own
// rotation) are typed in independently, at two different places in the file.
// ---------------------------------------------------------------------------

namespace {

// Shared shape for every probe below: elongated enough (roundness 0.2, minor
// semi-axis = 0.2 * 25 = 5 px) that a query 15 px off-centre is unambiguous
// -- either within the major-axis falloff (radius 25, flat core 0.3 * 25 =
// 7.5 px, so 15 px sits inside the smoothstep, comfortably nonzero) or
// outside the minor-axis rim (5 px) by a factor of three, which the exact
// hand-worked numbers in each section below confirm lands far past
// `brush/Deposit.hpp`'s own "exactly 0.0f at or beyond the rim" contract --
// not a near-miss anywhere this file queries.
constexpr float kRadius = 25.0f;
constexpr float kRoundness = 0.2f;
constexpr float kHardness = 0.3f;
constexpr float kProbeDist = 15.0f;
// Comfortably above 0 -- the worked examples in each section below land
// coverage near 0.6, so this only guards against "near enough to zero that
// the smoothstep's own float error could explain it", not against a subtler
// miss.
constexpr float kNonzeroFloor = 0.05f;

BrushTip ellipticalTip(float angleDeg) noexcept {
  BrushTip t;
  t.radius = kRadius;
  t.roundness = kRoundness;
  t.hardness = kHardness;
  t.angle = angleDeg;
  return t;
}

}  // namespace

bool runAngleConventionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // 1. PIN: `BrushTip::angle`'s positive sense is CLOCKWISE on screen.
  //
  // `dabCoverage()`'s own rotation (`brush/Deposit.cpp`, restated in
  // `brush/Deposit.hpp` sect2b): `u = dx*cos(angle) + dy*sin(angle)`. Setting
  // the query offset TO the tip's own axis direction, `(dx, dy) =
  // d*(cos(angle), sin(angle))`, makes `u == d, v == 0` by construction --
  // full coverage. So the major axis, in WORLD space, sits at direction
  // `(cos(angle), sin(angle))`, and because +y is DOWN on screen (every
  // raster in this build), sweeping `angle` up from 0 sweeps that direction
  // from due east toward due south -- the same "clockwise on screen" fact
  // `ops/Gradient.hpp`'s `Angular` gradient and `ops/Transform.hpp`'s
  // `transformRotateDegrees()` each derive independently for their own
  // rotations, restated here for `BrushTip::angle` specifically because
  // nothing before this file asserted it for THIS rotation.
  //
  // Angle 40 degrees, hand-picked: not a multiple of 90 (where an ellipse's
  // own 180-degree symmetry hides a mirrored convention -- exactly what let
  // Blot Bot 5's `angle 90.0` ship for however long the importer's sign was
  // wrong and nothing noticed) and not one of `--selftest`'s own other
  // stock angles, so a copy-paste from elsewhere could not accidentally
  // pass this by reusing a query vector meant for a different check.
  // ==========================================================================
  {
    const float angleDeg = 40.0f;
    const float t = angleDeg * 0.017453292519943295f;  // pi / 180
    const BrushTip tip = ellipticalTip(angleDeg);

    // On-axis: (dx, dy) = d * (cos(angle), sin(angle)) -- built from the SAME
    // 40 degrees the tip itself carries, but as a hand-typed cos/sin pair,
    // not as anything read back out of `dabCoverage()`.
    const float onAxisCov =
        dabCoverage(tip, kProbeDist * std::cos(t), kProbeDist * std::sin(t));
    std::printf("  [measured] coverage on the +40deg axis, 15px out: %.4f\n",
                static_cast<double>(onAxisCov));
    check(onAxisCov > kNonzeroFloor,
          "convention: a query point built from (cos 40deg, sin 40deg) -- the SAME direction "
          "a tip.angle of 40 claims for its major axis -- lands inside the tip's footprint");

    // The CCW mirror: (dx, dy) = d * (cos(-angle), sin(-angle)). If the
    // engine's rotation sense were the opposite of what section's opening
    // comment derives (a negated `angle`, or a swapped `u`/`v` sign), THIS
    // point -- not the one above -- would be the one landing inside the
    // footprint. Worked by hand: at t=40deg, this query's own world-angle is
    // -40deg, so `phi - t = -80deg`; `u = 15*cos(-80deg) ~= 2.6`,
    // `v_raw = 15*sin(-80deg) ~= -14.8`, `v/roundness ~= -73.9` -- a squared
    // distance ratio of about 2.96, well past the rim at 1.0.
    const float mirrorCov =
        dabCoverage(tip, kProbeDist * std::cos(-t), kProbeDist * std::sin(-t));
    std::printf("  [measured] coverage on the -40deg axis (the CCW mirror): %.4f\n",
                static_cast<double>(mirrorCov));
    check(mirrorCov == 0.0f,
          "convention: the CCW-mirrored query point -- (cos -40deg, sin -40deg) -- lands "
          "EXACTLY outside the footprint, at zero tolerance (brush/Deposit.hpp's own "
          "'exactly 0.0f at or beyond the rim' contract) -- a mirrored rotation sense would "
          "swap this result with the one above");
  }

  // ==========================================================================
  // 2. DIRECTION really turns the tip to FACE the stroke -- not 90 degrees
  //    off, not mirrored, not reversed.
  //
  // A travel vector (3, 4) -- a 3-4-5 triangle, so its own unit vector
  // (0.6, 0.8) and perpendicular (-0.8, 0.6) have no rounding to hide a
  // small error behind. `dynamicDirection()` is the ONLY function-under-test
  // call in this section; every query point below is built straight from
  // (0.6, 0.8) and its rotations, never from `cos(resolved angle)`.
  // ==========================================================================
  {
    constexpr float kDx = 3.0f, kDy = 4.0f;  // travel vector
    const float angleDeg = dynamicDirection(kDx, kDy) * 360.0f;
    const BrushTip tip = ellipticalTip(angleDeg);
    std::printf("  [measured] DIRECTION(3,4) resolves to tip.angle = %.4f deg\n",
                static_cast<double>(angleDeg));

    // Along the travel direction itself: (0.6, 0.8) * 15.
    const float alongCov = dabCoverage(tip, 0.6f * kProbeDist, 0.8f * kProbeDist);
    std::printf("  [measured] coverage ALONG the travel vector: %.4f\n",
                static_cast<double>(alongCov));
    check(alongCov > kNonzeroFloor,
          "direction: a DIRECTION -> ANGLE tip's footprint reaches a point 15px out ALONG the "
          "stroke's own travel vector (3,4) -- the tip is actually facing the way it moved");

    // Perpendicular to travel: (-0.8, 0.6) * 15 -- the 90-degree-off case.
    const float perpCov = dabCoverage(tip, -0.8f * kProbeDist, 0.6f * kProbeDist);
    std::printf("  [measured] coverage PERPENDICULAR to the travel vector: %.4f\n",
                static_cast<double>(perpCov));
    check(perpCov == 0.0f,
          "direction: the SAME tip's footprint does NOT reach a point 15px out PERPENDICULAR "
          "to travel -- rules out a link that turns the tip 90 degrees off the heading");

    // Reflected about the travel line: (0.6, -0.8) * 15 -- the mirror case.
    const float mirrorCov = dabCoverage(tip, 0.6f * kProbeDist, -0.8f * kProbeDist);
    std::printf("  [measured] coverage on the MIRRORED travel vector (0.6,-0.8): %.4f\n",
                static_cast<double>(mirrorCov));
    check(mirrorCov == 0.0f,
          "direction: nor does it reach a point reflected about the travel line -- rules out "
          "a DIRECTION whose sign disagrees with dabCoverage()'s own rotation sense, the exact "
          "failure mode section 1 above pins for a hand-set angle");

    // A stroke moving the OPPOSITE way along the identical line, (-3, -4):
    // DIRECTION resolves to a heading 180 degrees away, and an ellipse is
    // symmetric under a 180-degree turn about its own centre -- so this is
    // not a defect for a plain elliptical tip to reveal, and is asserted as
    // the identity it actually is rather than left unexplained. (An
    // ASYMMETRIC bitmap tip, unlike a bare ellipse, WOULD tell forward from
    // reverse -- brush/Deposit.hpp sect2c's own bitmap mapping applies the
    // identical `angle`/`roundness` rotation before sampling, so this
    // section's convention pin covers that path too, even though the
    // check below cannot exercise the asymmetry itself without a real
    // sampled tip, which --selftest cannot ship.)
    const float reverseAngleDeg = dynamicDirection(-kDx, -kDy) * 360.0f;
    const BrushTip reverseTip = ellipticalTip(reverseAngleDeg);
    const float reverseAlongCov = dabCoverage(reverseTip, 0.6f * kProbeDist, 0.8f * kProbeDist);
    std::printf("  [measured] coverage ALONG (3,4) with a tip resolved from (-3,-4): %.4f\n",
                static_cast<double>(reverseAlongCov));
    check(std::fabs(reverseAlongCov - alongCov) < 1e-5f,
          "direction: a stroke travelling the OPPOSITE way along the same line resolves a "
          "180-degree-different angle, which an elliptical tip cannot tell from the original "
          "-- the identical coverage this line measures, not a difference, is the correct "
          "answer for a shape with no 'front'");
  }

  std::printf("[selftest] angle convention %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
