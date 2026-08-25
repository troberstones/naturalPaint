#include "brush/Dynamics.hpp"

#include <cmath>
#include <cstdio>

namespace np {
namespace {

float clamp01(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;  // also catches NaN
  return v < 1.0f ? v : 1.0f;
}

// The preset curves, as control points in curve space.
//
// EaseOut and SCurve are three-point approximations rather than exact
// analytic easings: evalCurve() interpolates a monotone spline through the
// control points, so a midpoint is enough to bend it the right way, and
// three points is what the widget can show the user as draggable handles.
// An exact 1-(1-t)^2 would need points the user could not then move without
// the chip silently stopping to match.
constexpr float kEaseOutMid = 0.75f;   // t=0.5 lifted well above the diagonal
constexpr float kSCurveLow = 0.15f;    // t=0.25 pulled below
constexpr float kSCurveHigh = 0.85f;   // t=0.75 pushed above

}  // namespace

const char* sourceName(DynamicSource source) noexcept {
  switch (source) {
    case DynamicSource::Pressure: return "PRESSURE";
    case DynamicSource::Tilt: return "TILT";
    case DynamicSource::Azimuth: return "AZIMUTH";
    case DynamicSource::Barrel: return "BARREL";
    case DynamicSource::Velocity: return "VELOCITY";
    case DynamicSource::Fade: return "FADE";
    case DynamicSource::Noise: return "NOISE";
    case DynamicSource::Random: return "RANDOM";
  }
  return "?";
}

const char* sourceDisplay(DynamicSource source, float normalised, char* out,
                          size_t cap) noexcept {
  if (out == nullptr || cap == 0) return out;
  switch (source) {
    // Random has no value between dabs -- it is redrawn per dab, so any
    // number the gutter showed would be a number the next dab already
    // discarded. The design draws this cell in the muted grey, not the
    // foreground, for exactly that reason.
    case DynamicSource::Random:
      std::snprintf(out, cap, "%s", "\xE2\x80\x94");  // em dash
      return out;
    // Tilt is an altitude off the page normal: 0 is upright, 1 is flat.
    case DynamicSource::Tilt:
      std::snprintf(out, cap, "%.0f\xC2\xB0", clamp01(normalised) * 90.0f);
      return out;
    case DynamicSource::Azimuth:
      std::snprintf(out, cap, "%.0f\xC2\xB0", clamp01(normalised) * 360.0f);
      return out;
    // Barrel rotation is signed -- a pen can be twirled either way from its
    // rest orientation -- so its [0,1] maps onto [-180,180], which is why the
    // design's own gutter reads "-12" and not a value in [0,360].
    case DynamicSource::Barrel:
      std::snprintf(out, cap, "%.0f\xC2\xB0", clamp01(normalised) * 360.0f - 180.0f);
      return out;
    case DynamicSource::Pressure:
    case DynamicSource::Velocity:
    case DynamicSource::Fade:
    case DynamicSource::Noise:
      std::snprintf(out, cap, "%.2f", clamp01(normalised));
      return out;
  }
  std::snprintf(out, cap, "%s", "?");
  return out;
}

const char* targetAbbrev(DynamicTarget target) noexcept {
  switch (target) {
    case DynamicTarget::Size: return "SZ";
    case DynamicTarget::Angle: return "AN";
    case DynamicTarget::Roundness: return "RD";
    case DynamicTarget::Hardness: return "HD";
    case DynamicTarget::Flow: return "FL";
    case DynamicTarget::Scatter: return "SC";
    case DynamicTarget::Spacing: return "SP";
    case DynamicTarget::Concentration: return "CT";
    case DynamicTarget::Hue: return "HU";
    case DynamicTarget::Saturation: return "SA";
    case DynamicTarget::Value: return "VA";
    case DynamicTarget::Wetness: return "WT";
  }
  return "??";
}

const char* targetName(DynamicTarget target) noexcept {
  switch (target) {
    case DynamicTarget::Size: return "Size";
    case DynamicTarget::Angle: return "Angle";
    case DynamicTarget::Roundness: return "Roundness";
    case DynamicTarget::Hardness: return "Hardness";
    case DynamicTarget::Flow: return "Flow";
    case DynamicTarget::Scatter: return "Scatter";
    case DynamicTarget::Spacing: return "Spacing";
    case DynamicTarget::Concentration: return "Concentration";
    case DynamicTarget::Hue: return "Hue";
    case DynamicTarget::Saturation: return "Saturation";
    case DynamicTarget::Value: return "Value";
    case DynamicTarget::Wetness: return "Wetness";
  }
  return "?";
}

TargetCombine targetCombine(DynamicTarget target) noexcept {
  switch (target) {
    // Rotations and displacements accumulate; scales compose. Angle is the
    // case the design forces: tilt, azimuth and barrel all drive AN in its
    // own matrix, and three rotations that multiplied would cancel to nothing
    // the moment any one of them resolved to zero.
    case DynamicTarget::Angle:
    case DynamicTarget::Scatter:
    case DynamicTarget::Hue:
      return TargetCombine::Add;
    case DynamicTarget::Size:
    case DynamicTarget::Roundness:
    case DynamicTarget::Hardness:
    case DynamicTarget::Flow:
    case DynamicTarget::Spacing:
    case DynamicTarget::Concentration:
    case DynamicTarget::Saturation:
    case DynamicTarget::Value:
    case DynamicTarget::Wetness:
      return TargetCombine::Multiply;
  }
  return TargetCombine::Multiply;
}

float targetIdentity(DynamicTarget target) noexcept {
  return targetCombine(target) == TargetCombine::Add ? 0.0f : 1.0f;
}

void targetDefaultRange(DynamicTarget target, float& lo, float& hi) noexcept {
  switch (target) {
    // A full turn. A rotation link defaulting to a one-degree span would look
    // broken rather than subtle -- the user would drag the curve and see
    // nothing move.
    case DynamicTarget::Angle:
      lo = 0.0f;
      hi = 360.0f;
      return;
    // Hue is a signed rotation about the wheel, so its neutral is the middle
    // of the range rather than an end of it.
    case DynamicTarget::Hue:
      lo = -0.5f;
      hi = 0.5f;
      return;
    default:
      lo = 0.0f;
      hi = 1.0f;
      return;
  }
}

Curve easingCurve(EasingPreset preset) noexcept {
  switch (preset) {
    case EasingPreset::Linear:
      return Curve{{0.0f, 0.0f}, {1.0f, 1.0f}};
    case EasingPreset::EaseOut:
      return Curve{{0.0f, 0.0f}, {0.5f, kEaseOutMid}, {1.0f, 1.0f}};
    case EasingPreset::SCurve:
      return Curve{{0.0f, 0.0f}, {0.25f, kSCurveLow}, {0.75f, kSCurveHigh}, {1.0f, 1.0f}};
  }
  return Curve{{0.0f, 0.0f}, {1.0f, 1.0f}};
}

bool matchesPreset(const Curve& curve, EasingPreset preset) noexcept {
  // An empty curve is linear by evalCurve()'s degenerate case, so it matches
  // the Linear chip and nothing else -- which is what a freshly created link
  // should light up.
  const Curve want = easingCurve(preset);
  if (curve.empty()) return preset == EasingPreset::Linear;
  if (curve.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i) {
    if (std::fabs(curve[i].x - want[i].x) > 1e-4f) return false;
    if (std::fabs(curve[i].y - want[i].y) > 1e-4f) return false;
  }
  return true;
}

float linkContribution(const BrushLink& link, float source) noexcept {
  float t = clamp01(source);
  if (link.invert) t = 1.0f - t;
  // evalCurve() extrapolates flat past the authored x-range and does not
  // confine y, so a control point dragged above the plot can hand back
  // something outside [0,1]. Clamping here rather than trusting the widget
  // keeps a negative size factor out of the deposit path by construction.
  const float u = clamp01(link.curve.empty() ? t : evalCurve(link.curve, t));
  return link.rangeLo + (link.rangeHi - link.rangeLo) * u;
}

float sourceValue(const DynamicInputs& inputs, DynamicSource source) noexcept {
  switch (source) {
    case DynamicSource::Pressure: return inputs.pressure;
    case DynamicSource::Tilt: return inputs.tilt;
    case DynamicSource::Azimuth: return inputs.azimuth;
    case DynamicSource::Barrel: return inputs.barrel;
    case DynamicSource::Velocity: return inputs.velocity;
    case DynamicSource::Fade: return inputs.fade;
    case DynamicSource::Noise: return inputs.noise;
    case DynamicSource::Random: return inputs.random;
  }
  return 0.0f;
}

size_t findLink(const BrushLinkSet& set, DynamicSource source,
                DynamicTarget target) noexcept {
  for (size_t i = 0; i < set.links.size(); ++i) {
    if (set.links[i].source == source && set.links[i].target == target) return i;
  }
  return kNoLink;
}

size_t addLink(BrushLinkSet& set, const BrushLink& link) {
  const size_t existing = findLink(set, link.source, link.target);
  if (existing != kNoLink) {
    set.links[existing] = link;
    return existing;
  }
  set.links.push_back(link);
  return set.links.size() - 1;
}

bool removeLink(BrushLinkSet& set, DynamicSource source, DynamicTarget target) {
  const size_t at = findLink(set, source, target);
  if (at == kNoLink) return false;
  set.links.erase(set.links.begin() + static_cast<std::ptrdiff_t>(at));
  return true;
}

DynamicResult evaluateLinks(const BrushLinkSet& set,
                            const DynamicInputs& inputs) noexcept {
  DynamicResult out{};
  for (size_t i = 0; i < kDynamicTargetCount; ++i)
    out.value[i] = targetIdentity(static_cast<DynamicTarget>(i));

  for (const BrushLink& link : set.links) {
    if (!link.enabled) continue;
    const float contribution = linkContribution(link, sourceValue(inputs, link.source));
    const size_t slot = static_cast<size_t>(link.target);
    if (targetCombine(link.target) == TargetCombine::Add)
      out.value[slot] += contribution;
    else
      out.value[slot] *= contribution;
  }
  return out;
}

}  // namespace np
