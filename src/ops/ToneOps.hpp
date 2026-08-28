#pragma once

#include <array>

// ops/ToneOps -- docs/operations.md §1.2 "Committed additions": four more
// class-A point ops extending the ops/PointOps family (see PointOps.hpp's
// header comment for the family's shared contract, which this file inherits
// verbatim rather than re-deriving). Gain/offset/gamma, Invert, Posterize,
// Threshold.
//
// PURE MATH ONLY. No menu, no dialog, no op-stack (core/OpStack) wiring --
// those are separate, later steps owned elsewhere. Every function below is,
// like every function in PointOps.hpp, a plain
// `std::array<float,3> -> std::array<float,3>` on straight (non-
// premultiplied), scene-linear RGB:
//  - Never sees alpha, never touches it.
//  - Does NOT clamp its output, except where the formula itself
//    mathematically requires a narrow internal clamp (documented inline,
//    same discipline as LevelsParams' `t` clamp in PointOps.hpp).
//  - No GPU/LUT/texture/ImGui awareness whatsoever.
//
// *** The identity-default contract, and why two of these four structs carry
// an `amount` field the other two don't ***
// A default-constructed params struct must be an EXACT identity --
// f(x) == x, bit for bit, not merely "close". For Gain/offset/gamma this
// falls out for free (gain=1, offset=0, gamma=1 is the mathematical
// identity already). Posterize gets it for free too, by definition: 0 is
// declared the "off" level count (see its own comment below for why that is
// also the mathematically honest reading of "zero representable levels",
// not just a convenient bolt-on). Invert and Threshold have no such natural
// off state -- every domain choice and every threshold value still produces
// a real inversion or a real black/white split. Rather than special-case
// "0 means disabled" onto a field that has a real meaning at 0 (which is
// exactly the kind of overloaded-sentinel bug this codebase's `checkedAdd`
// and `LevelsParams` epsilon guards exist to avoid), both carry a genuine
// `amount` blend field -- 0 = untouched input, 1 = the full effect -- the
// same "mix" knob every Nuke colour node exposes, and this whole family is
// already framed in Nuke's terms (docs/operations.md: "the Nuke primitive").
// `amount` is not clamped to [0,1] either, consistent with this module's
// no-clamp policy: values outside it linearly extrapolate past or below the
// two endpoints, which is harmless (no pow/log/div involved) and is exactly
// the kind of caller-visible headroom the rest of this family already
// preserves.
//
// *** ...and why those two DEFAULT that field to 1, breaking the rule above
// on purpose ***
// `amount` was first written defaulting to 0, which made
// `applyInvert(rgb, InvertParams{})` return `rgb` -- a default-constructed
// Invert that does not invert. That is a worse bug than the inconsistency it
// bought. Photoshop's Image > Adjustments > Invert has NO dialog: it is a
// menu item that acts immediately, so the wiring layer's natural call is
// exactly `applyInvert(rgb, InvertParams{})`, and a caller who wrote it would
// get silence, with nothing anywhere to read that says why. Threshold is the
// same case one step removed.
//
// So the rule above is restated more carefully, because its loose phrasing is
// what produced that: **a default-constructed params struct means what the op
// is NAMED after.** For a parameterised op -- Levels, Curves,
// gain/offset/gamma -- the thing it is named after IS the identity, because
// "levels" with nothing set is a passthrough. For an op that names one
// specific effect, the neutral value is not the identity; it is that effect at
// full strength. `amount = 0` is still an exact identity and is still asserted
// as one. It is simply no longer where the default sits.
namespace np {

// ---------------------------------------------------------------------
// 1. Gain / offset / gamma (docs/operations.md §1.2: "the Nuke primitive;
// the honest form of brightness/contrast"). Deliberately linear-light, not
// the shaper domain -- like PointOps.hpp's Exposure, this is a physical
// light transform (a scale, a pedestal shift, a power curve), not something
// authored against perceptual/log coordinates.
//
// Operand order -- gain, then offset, then gamma:
//
//   base   = input * gain + offset
//   output = pow(base, 1 / gamma)
//
// This is the ASC Color Decision List's published "Slope / Offset / Power"
// (SOP) primitive (ASC-CDL, standardised as SMPTE ST 2095-1) -- the same
// three-stage decomposition, in the same order, and the reason
// docs/operations.md calls this "the honest form" of brightness/contrast:
// gain (slope) scales the signal directly, so it reads as a contrast
// control; offset shifts the pedestal *after* that scale, so an offset of,
// say, +0.1 always means "+0.1 of light" regardless of what gain is set to,
// rather than also being amplified by it (the order `(input + offset) *
// gain` would couple the two controls together, which is the "dishonest",
// hard-to-reason-about form this op exists to avoid); gamma reshapes the
// tone curve last, after both linear stages have settled. Gain and offset
// alone are already the whole of Exposure and Levels' black/white-point
// scaling in one line; gamma is what makes this the general primitive both
// of those are special cases of.
//
// `gain = 1, offset = 0, gamma = 1` is the identity default: `base ==
// input` exactly, and the gamma stage is skipped outright (see
// applyGainOffsetGammaChannel()'s implementation comment) rather than
// computed as `pow(input, 1)`, so the identity holds bit-for-bit including
// for a negative `input` -- scene-linear values are not guaranteed
// non-negative (compositing headroom can drive one below zero), and this op
// must not silently reshape that case when nothing asked it to.
//
// Gain/offset alone cannot drive `base` negative in a way that matters
// (multiply and add are total functions), but a caller-supplied negative
// `offset` combined with a non-1 `gamma` can. `pow()` with a fractional
// exponent (`1/gamma`) on a negative base is NaN in general, so `base` is
// clamped to >= 0 immediately before that `pow()` call and only then -- the
// same "clamp an internal value out of mathematical necessity, not the
// op's final output, out of policy" pattern LevelsParams' `t` clamp already
// establishes in PointOps.hpp. This never fires when `gamma == 1`, since
// that branch returns `base` directly without calling `pow()` at all.
struct GainOffsetGammaParams {
  float gain = 1.0f;
  float offset = 0.0f;
  float gamma = 1.0f;
};

// One channel's worth of the formula above. Exposed standalone for the same
// reason PointOps.hpp's applyLevelsChannel() is: independently
// hand-checkable, and applyGainOffsetGamma() is nothing but this run three
// times.
float applyGainOffsetGammaChannel(float input, const GainOffsetGammaParams& p) noexcept;

std::array<float, 3> applyGainOffsetGamma(const std::array<float, 3>& rgb,
                                           const GainOffsetGammaParams& p) noexcept;

// ---------------------------------------------------------------------
// 2. Invert (docs/operations.md §1.2: "in linear or display domain -- the
// choice is visible, so expose it"). `domain` is a real field, not an
// implicit convention -- both branches below are independently exercised.
//
//   Domain::Linear:
//     inverted = pivot - input,   pivot = 1.0 (the working space's nominal
//                                  "diffuse white", the same reference
//                                  Levels' whiteIn=1.0 default and
//                                  Exposure's un-stopped 1.0 already use)
//
//   Domain::Display:
//     encoded  = srgbEncode(input)      -- color/Space.hpp's *display*
//                                           transfer function, chosen over
//                                           color/Shaper.hpp's *grading*
//                                           one because "display domain" is
//                                           literally what Space.hpp's own
//                                           header comment calls this
//                                           function, and because Invert's
//                                           historical meaning (a
//                                           photographic negative) is
//                                           defined against the encoded
//                                           values a viewer actually sees,
//                                           not a grading-authoring
//                                           coordinate system
//     inverted_encoded = 1.0 - encoded
//     inverted = srgbDecode(inverted_encoded)
//
// output = lerp(input, inverted, amount) = input + (inverted - input) * amount
//
// *** What "invert" means above 1.0, and why this does NOT clamp ***
// Both formulas above are involutions: applying either one twice returns
// the original value EXACTLY, for every real input, including scene-linear
// HDR headroom above 1.0 -- not just values inside [0,1]. Linear domain:
// pivot - (pivot - x) == x algebraically, for any x. Display domain relies
// on srgbEncode/srgbDecode already being exact inverses of each other for
// every real input (color/Space.hpp's own contract: unclamped, and negative
// input mirrored via sign(x)*f(|x|) rather than refused), so
// srgbDecode(1 - (1 - srgbEncode(x))) == srgbDecode(srgbEncode(x)) == x
// the same way. A linear input above 1.0 inverts to a NEGATIVE value in
// both domains (pivot - x < 0 when x > pivot; srgbEncode(x) > 1 for x > 1
// since the encode curve is monotonic and unclamped, so 1 - encoded < 0,
// and srgbDecode of a negative encoded value mirrors to a negative linear
// result). A negative scene-linear RGB value has no physical meaning as
// light, but it is the mathematically honest result of inverting an
// unbounded pivot-relative quantity -- clamping it to 0 would silently
// destroy the property that makes Invert reversible at all: apply it twice
// to an HDR highlight and clamping would NOT return the original value,
// silently eating the very headroom this whole op family exists to
// preserve. That is why Invert, alone among this file's four ops, has no
// clamp anywhere in its implementation, not even an internal one.
struct InvertParams {
  enum class Domain { Linear, Display };
  Domain domain = Domain::Linear;
  // 0 = untouched input, 1 = the full invert. **1 is the default**, because a
  // default-constructed Invert has to invert -- see this file's header comment
  // on why that deliberately overrides the identity-default rule the other two
  // structs follow.
  float amount = 1.0f;
};

std::array<float, 3> applyInvert(const std::array<float, 3>& rgb, const InvertParams& p) noexcept;

// ---------------------------------------------------------------------
// 3. Posterize (docs/operations.md §1.2: "quantise in the shaper domain or
// it bands unevenly"). `levels` is a level *count* shared across all three
// channels (there is no per-channel posterize in the committed spec).
//
// Why the shaper domain and not linear: scene-linear light is NOT
// perceptually uniform -- doubling a dark value is a huge perceptual jump,
// doubling a bright one is barely visible (the same non-uniformity
// color/Shaper.hpp's own header comment cites as the reason curve control
// points are authored in shaper space, not linear). Quantising linear
// values into N equal-width bins therefore puts far too MANY of those bins
// in the highlights (where the eye can't tell them apart) and far too FEW
// in the shadows (where a single wide linear bin spans a perceptually huge,
// visibly banded range). The shaper's log encoding is close to
// perceptually uniform by construction (that is what "authored where a
// user expects them" already relies on for Curves), so equal-width bins in
// THAT domain read as evenly-spaced bands on screen -- which is the
// concrete meaning of "or it bands unevenly" above.
//
//   shaped = shaperEncode(input)
//   step   = 1 / (levels - 1)                         [levels >= 2]
//   q      = round(shaped / step) * step
//   output = shaperDecode(q)
//
// Two degenerate level counts, decided and handled explicitly rather than
// falling out of the formula above by accident:
//
//   levels == 0: "zero representable levels" cannot encode any value at
//   all -- there is no honest quantised answer, so this is defined as a
//   pass-through (output == input exactly). This is not a bolted-on "off"
//   flag: it is the same reading LevelsParams' epsilon guard and
//   checkedAdd() already apply elsewhere in this codebase to a formula
//   input that would otherwise be meaningless (here, dividing the domain
//   into zero bins), and it conveniently is also this struct's required
//   identity default (PosterizeParams{} has levels == 0).
//
//   levels == 1: exactly one representable output overall -- the general
//   formula's `step = 1/(levels-1)` divides by zero here, so it is handled
//   separately rather than left to produce inf/NaN. The formula for
//   levels >= 2 always places bin index 0 at shaped == 0 (round(0/step)*
//   step == 0), so levels == 1 is defined as extending that SAME bin-0
//   convention to being the only bin: every input maps to
//   `shaperDecode(0.0f)`, one fixed, hand-computable constant
//   (≈ -0.00692 in linear light -- shaperEncode's own affine segment,
//   `(0 - kOffsetB) / kSlopeA`, evaluated at its zero crossing; see
//   color/Shaper.cpp).
//
// Direction: increasing `levels` shrinks `step`, so output converges
// monotonically toward `input` as `levels` grows -- "more levels" is
// visibly "finer quantisation", the property a posterize control is
// supposed to have.
struct PosterizeParams {
  int levels = 0;
};

std::array<float, 3> applyPosterize(const std::array<float, 3>& rgb,
                                     const PosterizeParams& p) noexcept;

// ---------------------------------------------------------------------
// 4. Threshold (docs/operations.md §1.2, listed immediately after
// Posterize with no further note -- treated here as Posterize's own
// extreme case, levels == 2, which is what motivates reusing its domain
// choice below rather than picking a fresh one).
//
// The scalar being compared against `threshold` is per-pixel Rec.709 luma
// (PointOps.hpp's own `computeLuma()` / `kRec709LumaWeights`, reused
// verbatim rather than re-derived -- the same shared helper Saturation and
// Grayscale already both call), not each channel independently: Threshold
// is conventionally a mono black/white split on a pixel's overall
// brightness (matching Grayscale's own luma-replicated-to-RGB output
// shape), not three independently-clipping channels.
//
// Domain: the shaper domain, for the identical reason Posterize is in the
// shaper domain above -- a threshold is a 2-level posterize, and linear
// light's perceptual non-uniformity means a threshold value of, say, 0.5 in
// LINEAR light does not sit anywhere near a perceptual midpoint (roughly
// 73% of the way to sRGB white, in fact -- see this file's Invert doc
// comment's srgbEncode(0.5) hand computation for the same curve's shape),
// while 0.5 in the SHAPER domain is close to one. This also means a
// `threshold` value here is a coordinate in the same domain Curves'
// control points already are, per ADR-0004 -- consistent with the rest of
// this codebase's "the shaper domain is where perceptual choices are
// authored" policy, not a fresh convention invented for this op.
//
//   luma   = computeLuma(input)              -- Rec.709 weights
//   shaped = shaperEncode(luma)
//   bw     = (shaped >= threshold) ? 1.0 : 0.0        -- (bw, bw, bw)
//   output = lerp(input, (bw,bw,bw), amount)
//
// "At/above -> white" is the inclusive side: `shaped == threshold` maps to
// white (1,1,1), matching the `>=` above; strictly below maps to black
// (0,0,0). `amount` exists for the identical reason Invert's does (see
// this file's header comment) -- Threshold has no natural off state via
// `threshold` alone, since every real threshold value still produces a
// real black/white split.
struct ThresholdParams {
  float threshold = 0.5f;
  // 0 = untouched input, 1 = the full black/white split. **1 is the
  // default**, for the same reason Invert's is -- see this file's header
  // comment on what a default-constructed params struct is taken to mean.
  float amount = 1.0f;
};

std::array<float, 3> applyThreshold(const std::array<float, 3>& rgb,
                                     const ThresholdParams& p) noexcept;

}  // namespace np
