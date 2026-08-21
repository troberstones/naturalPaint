#include "core/PigmentBake.hpp"

#include <algorithm>
#include <cmath>

namespace np {

float bakedMassFromSim(float simMass, float absorption) noexcept {
  if (!(simMass > 0.0f) || !(absorption > 0.0f)) return 0.0f;
  return 1.0f - std::exp(-absorption * simMass);
}

float simMassFromBaked(float bakedMass, float absorption) noexcept {
  if (!(bakedMass > 0.0f) || !(absorption > 0.0f)) return 0.0f;
  const float m = std::min(bakedMass, kMaxBakedMass);
  return -std::log1p(-m) / absorption;
}

PigmentTexel projectSolverTexel(const std::array<float, 4>& depC,
                                const std::array<float, 4>& depR,
                                float absorption) noexcept {
  PigmentTexel out;
  // The same guard the shader uses, for the same reason -- see kMassEpsilon.
  // Note it divides by the guarded value even when the real mass is smaller,
  // which is what keeps a trace-mass texel's latent finite instead of
  // exploding; the coverage below is computed from the *unguarded* mass, so a
  // texel with almost nothing in it stays almost transparent rather than
  // inheriting the guard's magnitude.
  const float mass = std::max(depC[3], kMassEpsilon);
  out.latent.c = {depC[0] / mass, depC[1] / mass, depC[2] / mass};
  out.latent.res = {depR[0] / mass, depR[1] / mass, depR[2] / mass};
  out.mass = std::min(bakedMassFromSim(depC[3], absorption), kMaxBakedMass);
  return out;
}

bool bakeCompositionHolds(float simMassA, float simMassB, float absorption,
                          float tolerance) noexcept {
  const float a = bakedMassFromSim(simMassA, absorption);
  const float b = bakedMassFromSim(simMassB, absorption);
  const float composited = a + b * (1.0f - a);  // `over`, on coverages
  const float atOnce = bakedMassFromSim(simMassA + simMassB, absorption);
  return std::fabs(composited - atOnce) <= tolerance;
}

}  // namespace np
