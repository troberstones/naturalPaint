#include "brush/Variance.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// The per-dab, per-site draw. `dynamicRandomDraw()` is already the stroke's
// deterministic random stream (brush/Dynamics.hpp) -- pure in (seed, index)
// and replayable, which is the property a re-rendered stroke needs. Mixing the
// site into the seed rather than into the index keeps each site's sequence
// independent while leaving `dabIndex` meaning what it means everywhere else.
float draw(uint64_t seed, uint32_t dabIndex, VarianceSite site) noexcept {
  // 0x9E3779B97F4A7C15 is the 64-bit golden-ratio odd constant this codebase
  // already uses to decorrelate seeds; multiplying by it spreads adjacent site
  // ordinals across the whole word rather than leaving them one apart, which a
  // hash keyed on the low bits would barely separate.
  const uint64_t salted = seed ^ (static_cast<uint64_t>(site) * 0x9E3779B97F4A7C15ull);
  return dynamicRandomDraw(salted, dabIndex);
}

// The control's own axis in [0,1], or 1.0 when it contributes nothing --
// which is BOTH the Off case and the no-such-device case, deliberately the
// same answer for the reason varianceScale()'s header gives (audit B7).
float controlAxis(const Variance& v, const DynamicInputs& in, uint32_t dabIndex) noexcept {
  switch (v.control) {
    case VarianceControl::Off:
      return 1.0f;
    case VarianceControl::Fade: {
      // **Photoshop's Fade is counted in DABS, and that is why this does not
      // read `DynamicInputs::fade`.** That field is `dynamicFade()`, a ramp
      // over a fixed 480 px of arc length -- the right unit for a matrix row
      // whose length must not depend on how hard the stroke pressed, and the
      // wrong one for a `fStp` that says "25 steps". Reading the file's own
      // number through a distance ramp would silently make every Fade the same
      // length regardless of what the brush asked for.
      if (v.fadeSteps <= 0) return 0.0f;
      const float t = std::min(1.0f, static_cast<float>(dabIndex) / static_cast<float>(v.fadeSteps));
      return 1.0f - t;  // Photoshop fades OUT, from full toward the minimum
    }
    case VarianceControl::PenPressure:
      return in.hasPressure ? std::clamp(in.pressure, 0.0f, 1.0f) : 1.0f;
    case VarianceControl::PenTilt:
      return in.hasTilt ? std::clamp(in.tilt, 0.0f, 1.0f) : 1.0f;
    case VarianceControl::StylusWheel:
      return 1.0f;  // no SDL axis reports an airbrush wheel; see the header
    case VarianceControl::Rotation:
      return in.hasBarrel ? std::clamp(in.barrel, 0.0f, 1.0f) : 1.0f;
    case VarianceControl::Direction:
      return std::clamp(in.direction, 0.0f, 1.0f);
    case VarianceControl::InitialDirection:
      return std::clamp(in.initialDirection, 0.0f, 1.0f);
  }
  return 1.0f;
}

}  // namespace

const char* varianceControlName(VarianceControl control) noexcept {
  switch (control) {
    case VarianceControl::Off: return "Off";
    case VarianceControl::Fade: return "Fade";
    case VarianceControl::PenPressure: return "Pen Pressure";
    case VarianceControl::PenTilt: return "Pen Tilt";
    case VarianceControl::StylusWheel: return "Stylus Wheel";
    case VarianceControl::Rotation: return "Rotation";
    case VarianceControl::Direction: return "Direction";
    case VarianceControl::InitialDirection: return "Initial Direction";
  }
  return "unknown control";
}

bool varianceControlSource(VarianceControl control, DynamicSource& out) noexcept {
  switch (control) {
    case VarianceControl::Off: return false;
    case VarianceControl::Fade: out = DynamicSource::Fade; return true;
    case VarianceControl::PenPressure: out = DynamicSource::Pressure; return true;
    case VarianceControl::PenTilt: out = DynamicSource::Tilt; return true;
    case VarianceControl::StylusWheel: return false;
    case VarianceControl::Rotation: out = DynamicSource::Barrel; return true;
    case VarianceControl::Direction: out = DynamicSource::Direction; return true;
    case VarianceControl::InitialDirection: out = DynamicSource::InitialDirection; return true;
  }
  return false;
}

float varianceScale(const Variance& v, const DynamicInputs& in, uint64_t seed, uint32_t dabIndex,
                    VarianceSite site) noexcept {
  const float m = std::clamp(v.minimum, 0.0f, 1.0f);
  const float jitter = std::clamp(v.jitter, 0.0f, 1.0f);
  const float rj = (jitter <= 0.0f) ? 1.0f : 1.0f - jitter * (1.0f - draw(seed, dabIndex, site));
  const float c = controlAxis(v, in, dabIndex);
  // The minimum is applied HERE, outside the product -- see the header. Two
  // varying things can attenuate each other; neither can push the result
  // below `m`.
  return m + (1.0f - m) * std::clamp(rj * c, 0.0f, 1.0f);
}

float varianceOffset(const Variance& v, const DynamicInputs& in, float span, uint64_t seed,
                     uint32_t dabIndex, VarianceSite site) noexcept {
  const float jitter = std::clamp(v.jitter, 0.0f, 1.0f);

  // The control's contribution is a BASE OFFSET, not a scale: an Angle
  // controlled by Direction wants the stroke's heading itself. Off and the
  // unavailable-device case both contribute nothing, which for an additive
  // target is 0 rather than the 1 a multiplicative one takes -- the identity
  // differs per target, which is exactly why `DynamicInputs`' availability
  // flags cannot be collapsed into the input values themselves.
  float base = 0.0f;
  switch (v.control) {
    case VarianceControl::Off:
    case VarianceControl::StylusWheel:
      break;
    case VarianceControl::Fade:
      base = span * (1.0f - controlAxis(v, in, dabIndex));
      break;
    case VarianceControl::PenPressure:
      if (in.hasPressure) base = span * std::clamp(in.pressure, 0.0f, 1.0f);
      break;
    case VarianceControl::PenTilt:
      if (in.hasTilt) base = span * std::clamp(in.tilt, 0.0f, 1.0f);
      break;
    case VarianceControl::Rotation:
      if (in.hasBarrel) base = span * std::clamp(in.barrel, 0.0f, 1.0f);
      break;
    case VarianceControl::Direction:
      base = span * std::clamp(in.direction, 0.0f, 1.0f);
      break;
    case VarianceControl::InitialDirection:
      base = span * std::clamp(in.initialDirection, 0.0f, 1.0f);
      break;
  }

  // Jitter is symmetric about the base: `draw` is in [0,1], so `2d - 1` is in
  // [-1,1]. An asymmetric spread would bias every jittered angle one way round
  // the circle, which reads as a brush that leans.
  const float spread = (jitter <= 0.0f)
                           ? 0.0f
                           : span * jitter * (2.0f * draw(seed, dabIndex, site) - 1.0f);
  return base + spread;
}

bool varianceIsInert(const Variance& v) noexcept {
  return v.control == VarianceControl::Off && v.jitter <= 0.0f && v.minimum <= 0.0f;
}

}  // namespace np
