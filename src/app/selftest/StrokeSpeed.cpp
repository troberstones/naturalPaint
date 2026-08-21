#include "app/selftest/Support.hpp"

namespace np {

// Pins ADR-0003: paints the identical straight-line path twice, at very
// different simulated speeds (few pointer samples vs. many, over the same
// distance -- see strokeViaDabs()'s comment), and checks the two runs
// deposit the same total pigment mass. This is the property the old
// per-frame, dt-scaled splat.wgsl broke: it deposited proportionally to how
// many times sim.frame() got called for a given stroke, so a coarsely
// sampled ("fast") stroke deposited far less than a finely sampled ("slow")
// one covering the identical distance. Arc-length dabs remove sampling rate
// from the equation: dab positions and count along a fixed path depend only
// on distance travelled and spacing.
bool runStrokeSpeedTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const uint32_t W = sim.width(), H = sim.height();
  sim.setMode(gpu, PaintMode::Watercolor);

  SimParams p{};
  p.brushRadius = 24.0f;
  p.brushWater = 1.3f;
  p.brushPigment = 1.1f;
  p.brushHardness = 0.4f;
  loadPigment(p, lut, pigmentIndex("Ultramarine Blue"), true);

  const float ax = W * 0.2f, ay = H * 0.5f, bx = W * 0.8f, by = H * 0.5f;
  constexpr float kSpacing = 0.25f;  // matches BrushState's default

  sim.clearCanvas(gpu);
  const double massFast = strokeViaDabs(gpu, sim, p, kSpacing, ax, ay, bx, by, /*numSamples=*/6);

  sim.clearCanvas(gpu);
  const double massSlow = strokeViaDabs(gpu, sim, p, kSpacing, ax, ay, bx, by, /*numSamples=*/240);

  const double rel = std::abs(massFast - massSlow) / std::max(massSlow, 1.0);
  std::printf("[selftest] stroke speed: fast(6 samples) mass=%.1f  "
              "slow(240 samples) mass=%.1f  rel diff=%.4f\n",
              massFast, massSlow, rel);

  check(massFast > 0.0 && massSlow > 0.0, "both runs deposited pigment");
  // 5%: generous enough to absorb the one-dab-ish boundary rounding a
  // straight path can differ by between two sampling densities (the emitter
  // lags one real sample at the head and extrapolates the tail on flush()),
  // tight enough that reintroducing dt-scaled deposition -- which would
  // make the 240-sample run deposit on the order of 40x the 6-sample run's
  // mass -- fails it by nearly two orders of magnitude, not by a rounding
  // error.
  check(rel < 0.05,
        "deposited mass matches within 5% regardless of stroke speed (ADR-0003)");

  std::printf("[selftest] stroke speed %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
