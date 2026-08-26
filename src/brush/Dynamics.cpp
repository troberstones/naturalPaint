#include "brush/Dynamics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace np {
namespace {

float clamp01(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;  // also catches NaN
  return v < 1.0f ? v : 1.0f;
}

// `float` -> the `uint32_t` bit pattern the hash mixes, without the strict-
// aliasing violation a reinterpret_cast through pointers would be. `std::
// memcpy` of a `float` into a `uint32_t` is the standard-sanctioned way to do
// this and every compiler that matters optimises it to a single move -- there
// is no runtime copy here, only a type-pun with defined behaviour.
uint32_t floatBits(float v) noexcept {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
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
    case DynamicSource::Direction: return "DIRECTION";
    // Not "INITIAL DIRECTION": every row label here is one unspaced word
    // (matching PRESSURE, DIRECTION and the rest), and this row is drawn
    // immediately below DIRECTION's in the matrix's own enum order, so
    // "INITIAL" alone reads as "the initial [heading]" in context without
    // needing the second word repeated.
    case DynamicSource::InitialDirection: return "INITIAL";
  }
  return "?";
}

const char* sourceDisplay(DynamicSource source, float normalised, char* out,
                          size_t cap) noexcept {
  if (out == nullptr || cap == 0) return out;
  switch (source) {
    // Random has no value between dabs -- `dynamicRandomDraw()` genuinely
    // redraws it fresh per dab, so any number the gutter showed would be one
    // the next dab already discarded. The design draws this cell in the
    // muted grey, not the foreground, for exactly that reason.
    case DynamicSource::Random:
      std::snprintf(out, cap, "%s", "\xE2\x80\x94");  // em dash
      return out;
    // Tilt is an altitude off the page normal: 0 is upright, 1 is flat.
    case DynamicSource::Tilt:
      std::snprintf(out, cap, "%.0f\xC2\xB0", clamp01(normalised) * 90.0f);
      return out;
    // Direction and InitialDirection share AZIMUTH's plain 0-360, unsigned
    // convention -- see `dynamicDirection()`'s own comment on why (there is
    // no "rest orientation" for a heading the way there is for barrel
    // rotation, so there is nothing to centre a signed range on). Unlike
    // RANDOM above, InitialDirection's idle reading (0.0, "no stroke has
    // established a heading yet") IS meaningful between strokes -- it is
    // wrong only in the sense every stroke-local source's frame-sampled
    // idle value is "wrong" (VELOCITY/FADE/DIRECTION's own comments), not
    // in RANDOM's sense of "any number here is stale before you finish
    // reading it" -- so it gets the plain degree treatment, not the em dash.
    case DynamicSource::Azimuth:
    case DynamicSource::Direction:
    case DynamicSource::InitialDirection:
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

const char* targetUnbuildableReason(DynamicTarget target) noexcept {
  switch (target) {
    case DynamicTarget::Wetness:
      return "Not built yet. No CPU deposit route has a wetness field to scale -- a Pigment "
             "texel's seven channels do not include one, and giving it one is the solver "
             "readback bridge's job.";
    case DynamicTarget::Size:
    case DynamicTarget::Angle:
    case DynamicTarget::Roundness:
    case DynamicTarget::Hardness:
    case DynamicTarget::Flow:
    case DynamicTarget::Scatter:
    case DynamicTarget::Spacing:
    case DynamicTarget::Concentration:
    case DynamicTarget::Hue:
    case DynamicTarget::Saturation:
    case DynamicTarget::Value:
      return nullptr;
  }
  return nullptr;
}

const char* cellUnbuildableReason(DynamicSource source, DynamicTarget target) noexcept {
  if (const char* columnReason = targetUnbuildableReason(target)) return columnReason;
  // The one cell-level refinement: HUE/SATURATION/VALUE resolve at frame
  // granularity, inside `brushTipFor()`, against `dynamicInputsFor()`'s
  // hardware sample -- a stroke-local source fed into one of these three
  // would always read its frame-level default and paint a constant, which is
  // the shape of the ORIGINAL Dry Bristle defect this file exists to close,
  // not a new feature. See this function's own header comment.
  if ((target == DynamicTarget::Hue || target == DynamicTarget::Saturation ||
      target == DynamicTarget::Value) &&
      sourceIsStrokeLocal(source)) {
    return "Not built yet. VELOCITY/FADE/NOISE/RANDOM are resolved once per DAB; HUE, "
          "SATURATION and VALUE shift the colour once per FRAME, before a dab's own "
          "position exists. A link here would paint every dab the identical constant shift, "
          "not a moving one.";
  }
  return nullptr;
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
    case DynamicSource::Direction: return inputs.direction;
    case DynamicSource::InitialDirection: return inputs.initialDirection;
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

BrushLinkSet defaultBrushLinks() {
  BrushLinkSet set;
  BrushLink size;
  size.source = DynamicSource::Pressure;
  size.target = DynamicTarget::Size;
  // The literals from the two routes that used to read `pressureSize`, kept
  // as the range they always were. A linear curve, so the link IS the old
  // expression rather than an approximation of it.
  size.rangeLo = 0.25f;
  size.rangeHi = 1.0f;
  addLink(set, size);

  BrushLink flow;
  flow.source = DynamicSource::Pressure;
  flow.target = DynamicTarget::Flow;
  flow.rangeLo = 0.15f;
  flow.rangeHi = 1.0f;
  addLink(set, flow);
  return set;
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

// ---------------------------------------------------------------------------
// VELOCITY, FADE, NOISE, RANDOM -- see the header's own section comment for
// the determinism argument. Only the arithmetic is here.
// ---------------------------------------------------------------------------

uint64_t splitmix64(uint64_t x) noexcept {
  // Vigna 2015, public domain. Three xor-shift/multiply rounds; the constants
  // are the published ones (the golden-ratio increment and two odd 64-bit
  // multipliers chosen for full avalanche), not tuned here.
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

uint64_t strokeSeedFromStart(float startX, float startY) noexcept {
  // The two floats packed into one 64-bit word and mixed once: cheap, and a
  // single splitmix64 round already has full avalanche (Vigna's own
  // construction), so a second round here would cost cycles for no
  // additional decorrelation.
  const uint64_t packed = (static_cast<uint64_t>(floatBits(startX)) << 32) |
                          static_cast<uint64_t>(floatBits(startY));
  return splitmix64(packed);
}

float dynamicRandomDraw(uint64_t seed, uint32_t dabIndex) noexcept {
  // Re-mix the seed with the dab index before hashing, rather than just
  // hashing `seed + dabIndex`: splitmix64's own increment already IS a
  // counter-based construction (its whole contract is that consecutive
  // inputs produce well-separated outputs), so folding the index in through
  // the same mixer both sources reuse is the documented, tested way to draw
  // "the Nth value of stream S" rather than a bespoke combine this file would
  // have to justify on its own.
  const uint64_t mixed = splitmix64(seed ^ splitmix64(static_cast<uint64_t>(dabIndex)));
  // Top 24 bits: a float's mantissa (23 explicit bits plus the implicit
  // leading one) holds exactly 24 bits without rounding, so this can never
  // round up to the excluded 1.0f the way dividing a wider integer could.
  return static_cast<float>(mixed >> 40) / static_cast<float>(1u << 24);
}

float dynamicNoiseAt(uint64_t seed, float distanceAlongStroke) noexcept {
  // One lattice sample every kNoisePeriodPx of arc length -- coarse enough
  // that consecutive dabs (6 px apart at the shipped default spacing) fall
  // between the same two lattice points far more often than not, which is
  // what makes the result read as a slow drift rather than jitter; fine
  // enough that a stroke of ordinary length (a few hundred px) crosses
  // several periods and the noise visibly varies rather than holding one
  // smoothstep segment for the whole stroke.
  constexpr float kNoisePeriodPx = 48.0f;  // 2 * kFadeLengthPx's own 24 px
                                           // default-tip-radius unit, chosen
                                           // so the two stroke-length
                                           // constants in this file share one
                                           // empirical anchor rather than
                                           // inventing a second
  const float t = distanceAlongStroke / kNoisePeriodPx;
  const float lo = std::floor(t);
  const float frac = t - lo;
  const uint32_t i0 = static_cast<uint32_t>(static_cast<int64_t>(lo));
  const uint32_t i1 = i0 + 1u;
  // Two independent lattice draws off the SAME per-stroke seed, distinguished
  // by lattice index rather than by a second seed -- `dynamicRandomDraw()`'s
  // own counter-based contract already guarantees i0 and i1 decorrelate.
  const float v0 = dynamicRandomDraw(seed, i0);
  const float v1 = dynamicRandomDraw(seed, i1);
  // Smoothstep rather than a linear lerp between the two lattice values, for
  // `dabCoverage()`'s own reason (brush/Deposit.hpp §2): a linear ramp is C0,
  // and its derivative jumps at every lattice crossing -- visible as a kink
  // in whatever this noise drives, exactly the banding argument that already
  // ruled out a linear falloff for a dab's rim.
  const float smooth = frac * frac * (3.0f - 2.0f * frac);
  return v0 + (v1 - v0) * smooth;
}

float dynamicVelocity(float stepDistancePx, float radiusPx) noexcept {
  // radiusPx guarded off zero rather than asserted non-zero: brush/Deposit.hpp
  // already treats a zero-or-negative radius as "deposits nothing" rather
  // than as an error, and this function has the same contract -- a degenerate
  // tip reads as motionless rather than dividing by zero.
  //
  // **Return early rather than clamping the denominator.** Substituting a tiny
  // r and dividing (`stepDistancePx / 1e-6f`) is the obvious-looking guard and
  // it produces the exact opposite of the contract above: any real step over a
  // near-zero radius comes out as 1e7 and clamps to **1.0**, so a degenerate
  // tip reads as maximum speed. It also never divides by zero, so it looks
  // correct and passes any test that only checks for NaN. The value is what is
  // wrong, not the arithmetic.
  if (!(radiusPx > 1e-6f)) return 0.0f;
  return clamp01(stepDistancePx / radiusPx);
}

float dynamicFade(float distanceAlongStroke) noexcept {
  return clamp01(distanceAlongStroke / kFadeLengthPx);
}

float dynamicDirection(float dx, float dy) noexcept {
  // `std::atan2(0, 0)` is `0` by contract (IEEE754's own special case, which
  // this project's toolchain follows), so a stroke's first dab -- which
  // `app/StrokeSession` calls this with `dx = dy = 0.0` for, having no
  // previous dab to difference against -- and a dab that landed exactly on
  // the previous one both resolve to the SAME 0.0 heading through this one
  // formula, with no branch here to keep in sync with the caller's own
  // "no previous position" convention (`dynamicVelocity()`'s identical
  // shape, one function up).
  const float radians = std::atan2(dy, dx);
  // radians -> degrees, then wrapped into [0, 360) rather than left signed:
  // this header's own comment on why (AZIMUTH's convention, not BARREL's --
  // a heading has no rest orientation to be signed about).
  constexpr float kRadToDeg = 57.295779513082322865f;  // 180 / pi
  float degrees = radians * kRadToDeg;
  if (degrees < 0.0f) degrees += 360.0f;
  return degrees / 360.0f;
}

bool sourceIsStrokeLocal(DynamicSource source) noexcept {
  switch (source) {
    case DynamicSource::Velocity:
    case DynamicSource::Fade:
    case DynamicSource::Noise:
    case DynamicSource::Random:
    case DynamicSource::Direction:
    case DynamicSource::InitialDirection:
      return true;
    case DynamicSource::Pressure:
    case DynamicSource::Tilt:
    case DynamicSource::Azimuth:
    case DynamicSource::Barrel:
      return false;
  }
  return false;
}

DynamicResult evaluateLinksFiltered(const BrushLinkSet& set, const DynamicInputs& inputs,
                                    bool wantStrokeLocal) noexcept {
  DynamicResult out{};
  for (size_t i = 0; i < kDynamicTargetCount; ++i)
    out.value[i] = targetIdentity(static_cast<DynamicTarget>(i));

  for (const BrushLink& link : set.links) {
    if (!link.enabled) continue;
    if (sourceIsStrokeLocal(link.source) != wantStrokeLocal) continue;
    const float contribution = linkContribution(link, sourceValue(inputs, link.source));
    const size_t slot = static_cast<size_t>(link.target);
    if (targetCombine(link.target) == TargetCombine::Add)
      out.value[slot] += contribution;
    else
      out.value[slot] *= contribution;
  }
  return out;
}

// ---------------------------------------------------------------------------
// HUE, SATURATION, VALUE -- the sRGB shift. See the header's own section
// comment for why sRGB and not linear, and for what this deliberately does
// not touch (density, staining, granulation).
// ---------------------------------------------------------------------------

Hsv rgbToHsv(std::array<float, 3> srgb) noexcept {
  const float r = srgb[0], g = srgb[1], b = srgb[2];
  const float maxc = std::max(r, std::max(g, b));
  const float minc = std::min(r, std::min(g, b));
  const float delta = maxc - minc;
  Hsv out;
  out.v = maxc;
  out.s = maxc > 1e-8f ? delta / maxc : 0.0f;
  if (delta > 1e-8f) {
    float h;
    if (maxc == r) h = std::fmod((g - b) / delta, 6.0f);
    else if (maxc == g) h = (b - r) / delta + 2.0f;
    else h = (r - g) / delta + 4.0f;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
    out.h = h;
  }
  // else: delta ~ 0, a grey -- h stays its default-constructed 0.0f, the
  // documented convention for an undefined hue.
  return out;
}

std::array<float, 3> hsvToRgb(Hsv hsv) noexcept {
  // Wrap h into [0,1) rather than trust the caller -- `applyHsvDynamics()`
  // hands this an offset hue that can legitimately sit outside a single turn
  // once a Hue link's range or an inverted curve pushes it there.
  float h = hsv.h - std::floor(hsv.h);
  const float s = clamp01(hsv.s);
  const float v = std::max(hsv.v, 0.0f);
  const float c = v * s;
  const float hp = h * 6.0f;
  const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if (hp < 1.0f) { r = c; g = x; }
  else if (hp < 2.0f) { r = x; g = c; }
  else if (hp < 3.0f) { g = c; b = x; }
  else if (hp < 4.0f) { g = x; b = c; }
  else if (hp < 5.0f) { r = x; b = c; }
  else { r = c; b = x; }
  const float m = v - c;
  return {r + m, g + m, b + m};
}

std::array<float, 3> applyHsvDynamics(std::array<float, 3> srgb, float hueTurnsOffset,
                                      float satMul, float valMul) noexcept {
  // The identity short-circuit: see the header's comment on why this is not
  // an optimisation. Exact equality is deliberate -- `DynamicResult::at()`
  // returns the target's own `targetIdentity()` literal (0.0f / 1.0f) when
  // nothing drives it, not an approximation of one, so an undriven target
  // compares equal here every time.
  if (hueTurnsOffset == 0.0f && satMul == 1.0f && valMul == 1.0f) return srgb;
  Hsv hsv = rgbToHsv(srgb);
  hsv.h += hueTurnsOffset;
  hsv.s = clamp01(hsv.s * satMul);
  hsv.v = std::max(hsv.v * valMul, 0.0f);
  std::array<float, 3> out = hsvToRgb(hsv);
  out[0] = clamp01(out[0]);
  out[1] = clamp01(out[1]);
  out[2] = clamp01(out[2]);
  return out;
}

}  // namespace np
