#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <set>

#include "brush/Variance.hpp"

namespace np {
namespace {

DynamicInputs mouseInputs() {
  DynamicInputs in;  // defaults describe a MOUSE: no tilt, no barrel
  return in;
}

DynamicInputs penInputs(float pressure, float tilt, float barrel) {
  DynamicInputs in;
  in.pressure = pressure;
  in.tilt = tilt;
  in.barrel = barrel;
  in.hasPressure = true;
  in.hasTilt = true;
  in.hasBarrel = true;
  return in;
}

}  // namespace

bool runVarianceTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  constexpr uint64_t kSeed = 0x5EEDF00Dull;

  // ==========================================================================
  std::printf("  -- A. the floor is applied ONCE (audit B6) --\n");
  // ==========================================================================
  {
    // **The whole reason this type exists.** The old importer expanded one
    // Photoshop dynamics object into up to TWO links on a Multiply target, each
    // carrying its own copy of `Mnm `. Two links multiply, so a 30% floor
    // expressed twice floored at 0.09 instead of 0.30. Here the minimum is
    // outside the product, so no combination of control and jitter can reach
    // below it.
    Variance v;
    v.present = true;
    v.control = VarianceControl::PenPressure;
    v.jitter = 1.0f;    // maximum jitter
    v.minimum = 0.30f;  // a 30% floor
    v.fadeSteps = 25;

    // Pressure 0 AND the worst possible jitter draw: the two things that could
    // each drive this to zero, applied together.
    float lowest = 2.0f;
    for (uint32_t dab = 0; dab < 4096; ++dab)
      lowest = std::min(lowest, varianceScale(v, penInputs(0.0f, 0.0f, 0.0f), kSeed, dab,
                                              VarianceSite::Size));
    check(nearf(lowest, 0.30f, 1e-6f),
          "variance: a 30% minimum floors at 0.30 exactly, never at 0.09");

    // And the ceiling is 1 -- a floor must not also be a scale.
    float highest = -1.0f;
    Variance open = v;
    open.jitter = 0.0f;
    for (uint32_t dab = 0; dab < 64; ++dab)
      highest = std::max(highest, varianceScale(open, penInputs(1.0f, 1.0f, 1.0f), kSeed, dab,
                                                VarianceSite::Size));
    check(nearf(highest, 1.0f, 1e-6f), "variance: full pressure with no jitter resolves to 1.0");

    // The identical variance resolved through the ROUNDNESS site must floor
    // identically -- `multiplyFloor[Roundness]` was imported and then never
    // applied at all (audit B7's sibling), which this makes unrepresentable
    // because roundness takes the same call as size.
    float roundLowest = 2.0f;
    for (uint32_t dab = 0; dab < 1024; ++dab)
      roundLowest = std::min(roundLowest, varianceScale(v, penInputs(0.0f, 0.0f, 0.0f), kSeed, dab,
                                                        VarianceSite::Roundness));
    check(nearf(roundLowest, 0.30f, 1e-6f),
          "variance: roundness floors by the same call as size, not by a second path");
  }

  // ==========================================================================
  std::printf("  -- B. an absent device is IDENTITY, not the floor (audit B7) --\n");
  // ==========================================================================
  {
    // `Kyle's Spatter Brushes - Supreme Spatter & Texture` carries a
    // Tilt-driven size with a floor of 0. Painting it with a mouse used to
    // resolve tilt to 0.0, multiply the radius by the floor, and deposit
    // EXACTLY ZERO PIXELS -- with no refusal and an import report saying
    // everything arrived.
    Variance tiltSize;
    tiltSize.present = true;
    tiltSize.control = VarianceControl::PenTilt;
    tiltSize.jitter = 0.0f;
    tiltSize.minimum = 0.0f;

    const float mouse = varianceScale(tiltSize, mouseInputs(), kSeed, 0, VarianceSite::Size);
    check(nearf(mouse, 1.0f, 1e-6f),
          "variance: a tilt control on a device with no tilt resolves to 1.0, not 0.0");

    // A pen that DOES report tilt must still be driven by it -- the fix must
    // not be "ignore tilt".
    const float pen = varianceScale(tiltSize, penInputs(1.0f, 0.25f, 0.0f), kSeed, 0,
                                    VarianceSite::Size);
    check(nearf(pen, 0.25f, 1e-6f), "variance: a device that DOES report tilt is still driven by it");

    Variance barrel = tiltSize;
    barrel.control = VarianceControl::Rotation;
    check(nearf(varianceScale(barrel, mouseInputs(), kSeed, 0, VarianceSite::Angle), 1.0f, 1e-6f),
          "variance: a barrel-rotation control with no barrel axis resolves to 1.0");

    // Stylus Wheel has no SDL axis at all and never will from this input path.
    Variance wheel = tiltSize;
    wheel.control = VarianceControl::StylusWheel;
    DynamicSource unused{};
    check(!varianceControlSource(VarianceControl::StylusWheel, unused),
          "variance: Stylus Wheel maps to NO source -- it is refused, not aliased onto barrel");
    check(nearf(varianceScale(wheel, penInputs(1.0f, 1.0f, 1.0f), kSeed, 0, VarianceSite::Size),
                1.0f, 1e-6f),
          "variance: a Stylus Wheel control resolves to identity on every device");
  }

  // ==========================================================================
  std::printf("  -- C. Fade counts DABS, not pixels --\n");
  // ==========================================================================
  {
    // Photoshop's `fStp` says "steps". This build's own `dynamicFade()` is a
    // ramp over a fixed 480 px of arc length -- the right unit for a matrix
    // row, the wrong one for a file that names a count. Reading the file's
    // number through the distance ramp would make every Fade the same length
    // whatever the brush asked for. Measured `fStp` across the four packs is
    // 1..25, so the difference between brushes is real.
    Variance fade;
    fade.present = true;
    fade.control = VarianceControl::Fade;
    fade.jitter = 0.0f;
    fade.minimum = 0.0f;
    fade.fadeSteps = 10;

    const DynamicInputs in = penInputs(1.0f, 0.5f, 0.5f);
    check(nearf(varianceScale(fade, in, kSeed, 0, VarianceSite::Size), 1.0f, 1e-6f),
          "variance: Fade is full at dab 0");
    check(nearf(varianceScale(fade, in, kSeed, 5, VarianceSite::Size), 0.5f, 1e-6f),
          "variance: Fade is half way at dab 5 of 10");
    check(nearf(varianceScale(fade, in, kSeed, 10, VarianceSite::Size), 0.0f, 1e-6f),
          "variance: Fade reaches the minimum at exactly fadeSteps");
    check(nearf(varianceScale(fade, in, kSeed, 999, VarianceSite::Size), 0.0f, 1e-6f),
          "variance: Fade stays at the minimum past fadeSteps, it does not wrap");

    Variance shorter = fade;
    shorter.fadeSteps = 2;
    check(varianceScale(shorter, in, kSeed, 5, VarianceSite::Size) <
              varianceScale(fade, in, kSeed, 5, VarianceSite::Size),
          "variance: a shorter fStp fades sooner -- the file's own number is used");
  }

  // ==========================================================================
  std::printf("  -- D. sites draw independently --\n");
  // ==========================================================================
  {
    // Without a per-site salt every jittered property of one dab draws the SAME
    // number and moves in lockstep, which reads as one coherent wobble rather
    // than three independent variations. This is the difference between a
    // brush that looks hand-made and one that looks like it is vibrating, and
    // it is invisible in any single-property test.
    Variance v;
    v.present = true;
    v.jitter = 1.0f;

    const DynamicInputs in = penInputs(1.0f, 0.5f, 0.5f);
    size_t lockstep = 0;
    for (uint32_t dab = 0; dab < 256; ++dab) {
      const float size = varianceScale(v, in, kSeed, dab, VarianceSite::Size);
      const float angle = varianceScale(v, in, kSeed, dab, VarianceSite::Angle);
      const float scatter = varianceScale(v, in, kSeed, dab, VarianceSite::Scatter);
      if (nearf(size, angle, 1e-6f) || nearf(size, scatter, 1e-6f)) ++lockstep;
    }
    check(lockstep == 0,
          "variance: Size, Angle and Scatter draw different values on every one of 256 dabs");

    // Determinism is the other half: the same (seed, dab, site) must be bit
    // identical, or a replayed stroke is a different stroke.
    bool deterministic = true;
    for (uint32_t dab = 0; dab < 64; ++dab)
      if (varianceScale(v, in, kSeed, dab, VarianceSite::Size) !=
          varianceScale(v, in, kSeed, dab, VarianceSite::Size))
        deterministic = false;
    check(deterministic, "variance: the same (seed, dab, site) resolves bit-identically");
  }

  // ==========================================================================
  std::printf("  -- E. the additive form --\n");
  // ==========================================================================
  {
    // Angle and Scatter are offsets, so their identity is 0 rather than 1 --
    // the identity differs per TARGET while an input is per SOURCE, which is
    // exactly why availability cannot be folded into the input value.
    Variance inert;
    check(nearf(varianceOffset(inert, mouseInputs(), 180.0f, kSeed, 0, VarianceSite::Angle), 0.0f,
                1e-6f),
          "variance: an inert additive variance offsets by 0, not by 1");

    // Jitter is symmetric about the base, or every jittered angle leans one
    // way round the circle.
    Variance spread;
    spread.present = true;
    spread.jitter = 1.0f;
    double sum = 0.0;
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t dab = 0; dab < 20000; ++dab) {
      const float o = varianceOffset(spread, mouseInputs(), 180.0f, kSeed, dab,
                                     VarianceSite::Angle);
      sum += o;
      lo = std::min(lo, o);
      hi = std::max(hi, o);
    }
    check(std::fabs(sum / 20000.0) < 3.0,
          "variance: additive jitter is symmetric about its base -- the mean is ~0");
    check(lo > -180.5f && hi < 180.5f && lo < -150.0f && hi > 150.0f,
          "variance: additive jitter spans the full +/- span and does not exceed it");

    check(varianceIsInert(inert) && !varianceIsInert(spread),
          "variance: inertness distinguishes 'the file said nothing' from 'the file said Off'");
  }

  std::printf("[selftest] variance %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
