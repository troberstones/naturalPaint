#pragma once

#include <array>

#include "ops/Gradient.hpp"
#include "ops/PointOps.hpp"

// ops/MonoOps (docs/operations.md §1.2, "Committed additions": Black & white
// and Gradient map, both class A / P1). Two more members of ops/PointOps.hpp's
// `rgb -> rgb` family -- read that header's top-of-file comment in full before
// this one, since every rule it states ("straight, scene-linear RGB", "never
// touches alpha", "does not clamp except where the formula requires it",
// "no GPU/LUT/texture/tile/ImGui awareness") applies here unchanged and is not
// re-derived below. This file only states what is NEW about these two ops:
// the six-channel decomposition Black & White is built from, and the identity
// question a gradient map raises that none of PointOps.hpp's six ops do.
//
// Both ops are pure math, no menu, no dialog, no op-stack entry -- those are
// a separate, later piece of work (see ui/MenuModel.*, core/OpStack.*), not
// this file's job.
namespace np {

// ---------------------------------------------------------------------
// 1. Black & white (docs/operations.md §1.2: "six-channel weights, as the
// mono-conversion control" -- reds, yellows, greens, cyans, blues, magentas).
//
// --- Domain: scene-linear, straight RGB, same as Saturation/Grayscale ---
// This op computes directly on linear light, the same choice ops/PointOps.hpp
// already made for Saturation and Grayscale (both dot straight linear RGB
// against a weight vector) and deliberately NOT the shaper domain ADR-0004
// reserves for Curves. The shaper domain exists so a curve's AUTHORED control
// points land where a Photoshop-trained eye expects them on a log graph; a
// six-slider hue weight has no authored control-point coordinates to protect,
// it is a physical-ish weighting of a linear-light quantity exactly like
// Saturation's `lumaWeights`, so there is nothing here for the shaper to buy.
//
// --- The decomposition, stated as a formula ---
// The six weights sit at 60-degree intervals around the standard RGB hue
// wheel, in the exact cyclic order docs/operations.md names them: Red (0deg),
// Yellow (60deg), Green (120deg), Cyan (180deg), Blue (240deg), Magenta
// (300deg) -- the same wheel HSV's own hue angle is defined against, which is
// why the construction below reuses HSV's published max/min/mod hue formula
// (Wikipedia "HSL and HSV" derivation of hue from RGB; public colour-science
// mathematics, not any GPL or patented source) rather than inventing a new
// one:
//
//   M = max(r, g, b), m = min(r, g, b), C = M - m              (chroma)
//
//   C == 0 (achromatic): output = m. No hue exists, so which weight would
//   apply is undefined -- and moot, since the term it would scale is C = 0
//   regardless.
//
//   C  > 0: compute h in [0, 6) --
//     M == r:  h = mod6((g - b) / C)
//     M == g:  h = (b - r) / C + 2
//     M == b:  h = (r - g) / C + 4
//   (mod6 wraps a negative pre-wrap value from the M==r branch back into
//   [0, 6); the other two branches' own +2/+4 offsets never need it, but the
//   wrap is applied uniformly rather than only on the branch that needs it,
//   so the three branches share one normalisation step instead of two of
//   them silently relying on never hitting it.)
//
//   sector = floor(h) in {0..5}; frac = h - sector  (frac in [0, 1))
//   weight = lerp(w[sector], w[(sector + 1) % 6], frac)
//     where w = {reds, yellows, greens, cyans, blues, magentas} -- exactly
//     docs/operations.md's own order, so w[0] is the Red anchor, w[1] is
//     Yellow, and so on around the wheel to w[5] Magenta, wrapping back to
//     w[0] Red at h = 6.
//
//   output = m + weight * C
//
// **Why this isolates a pure primary to exactly one weight** (the property
// runMonoOpsTest() pins): pure red is (1, 0, 0). M = r = 1, m = 0, C = 1,
// and the M==r branch gives h = mod6((0 - 0) / 1) = 0, so sector = 0 and
// frac = 0 EXACTLY -- not approximately zero, exactly, because g and b are
// both exactly the channel's own minimum. `weight = lerp(w[0], w[1], 0)`
// reduces to `w[0]` = `reds` with a zero coefficient on `w[1]` = `yellows`
// and no appearance at all of w[2..5] (greens, cyans, blues, magentas) in
// the formula -- those five weights are not merely multiplied by something
// that happens to evaluate to zero for this input, they are absent from the
// expression `sector`/`frac` selects. The same argument holds, mutatis
// mutandis, for pure green, pure blue and the three secondaries: each pure
// anchor colour drives `frac` to exactly 0 or 1 at its own sector boundary,
// which is what "governed by exactly one weight" means here.
//
// --- The default weights, and why they are an EXACT Rec.709 grayscale ---
// The default weight for each of the six names is that colour's OWN
// Rec.709 luma at full value (`computeLuma()` on the pure/secondary colour
// itself), reusing `kRec709LumaWeights` rather than re-deriving new
// constants:
//
//   reds     = kRec709LumaWeights[0]                            = 0.2126
//   yellows  = kRec709LumaWeights[0] + kRec709LumaWeights[1]     = 0.9278
//   greens   = kRec709LumaWeights[1]                             = 0.7152
//   cyans    = kRec709LumaWeights[1] + kRec709LumaWeights[2]     = 0.7874
//   blues    = kRec709LumaWeights[2]                             = 0.0722
//   magentas = kRec709LumaWeights[0] + kRec709LumaWeights[2]     = 0.2848
//
// This is not merely "a sensible neutral" -- with these defaults,
// applyBlackAndWhite() is applyGrayscale()'s Rec.709 result for EVERY input,
// not only the six anchors, and that is provable rather than measured:
// `computeLuma()` is a linear functional of (r, g, b).
//
// **"Exactly" here means as ALGEBRA, not as a bit pattern**, and the
// distinction is worth stating because an earlier draft of this comment said
// EXACTLY without the qualifier and a bit-for-bit assertion built on it
// failed. The two sides are different expression trees -- `m + weight * C`
// against `0.2126r + 0.7152g + 0.0722b` -- so in floating point they land
// within an ulp of each other rather than on it.
// `app/selftest/AdjustmentMenu.cpp` measures the worst case over a real
// layer and prints it: 4.883e-04, which is precisely one f16 storage step at
// that magnitude, i.e. the whole of the disagreement is the tile format's
// rounding and none of it is the arithmetic. Fix a
// sector (fix which channel is max, which is min): inside that sector, as
// `frac` sweeps 0 to 1 at constant `m` and `C`, the pixel's own (r, g, b)
// moves affinely in `frac` (one channel pinned at `M`, one at `m`, the third
// interpolating linearly between them) -- so `computeLuma(rgb)` is affine in
// `frac` too. `m + weight(frac) * C` is *also* affine in `frac` (weight is a
// plain lerp). Two affine functions of `frac` that agree at `frac = 0` and
// `frac = 1` agree everywhere between -- and they DO agree at both
// endpoints, because that is exactly how the six defaults above were
// chosen: `w[sector]` and `w[sector + 1]` are, by construction, `computeLuma`
// of the two colours that sit at `frac = 0` and `frac = 1` of that sector.
// So the two formulas coincide throughout every sector, for any `m`, `C`
// and `frac` -- the six-weight construction is Rec.709 grayscale by default,
// generalised so each of the six hue directions can be pushed independently
// away from that shared baseline. runMonoOpsTest() checks this identity at
// an anchor, at a sector midpoint, and above 1.0 (HDR) -- not just "close",
// exactly, since the proof above gives an exact equality to check against.
//
// --- No clamp ---
// `m`, `C` and every weight above are used exactly as given; nothing here
// clamps `output`. A pixel with any channel above 1.0 (or below 0.0) simply
// carries that headroom through `m`/`C` into the result, matching
// PointOps.hpp's no-clamp policy -- there is no formula-mandated clamp
// anywhere in this construction (unlike Levels' `t`), so none is written.
struct BlackAndWhiteParams {
  float reds = kRec709LumaWeights[0];
  float yellows = kRec709LumaWeights[0] + kRec709LumaWeights[1];
  float greens = kRec709LumaWeights[1];
  float cyans = kRec709LumaWeights[1] + kRec709LumaWeights[2];
  float blues = kRec709LumaWeights[2];
  float magentas = kRec709LumaWeights[0] + kRec709LumaWeights[2];
};

// Output is always achromatic (r == g == b), by construction -- see the
// formula above; there is exactly one scalar computed and it is replicated
// to all three channels, the same "no single-channel representation yet"
// reason applyGrayscale() gives.
std::array<float, 3> applyBlackAndWhite(const std::array<float, 3>& rgb,
                                         const BlackAndWhiteParams& p) noexcept;

// ---------------------------------------------------------------------
// 2. Gradient map (docs/operations.md §1.2: "luma -> gradient; class A
// because the input is one scalar"). Reuses ops/Gradient.hpp's
// `GradientStops` and `gradientColorAt()` wholesale -- read that header's
// own §1/§2 before this one -- rather than inventing a second stop
// representation; the only new code this op needs is "compute a luma, feed
// it in as `t`".
//
// --- Domain: luma measured in scene-linear light, same as the stop ramp ---
// `ColorStop::color` is itself "straight, scene-linear RGB" (Gradient.hpp
// §2) and `position` is "conventionally in [0, 1]" against that same linear
// parameter -- a caller authoring a gradient map's stops is already placing
// them in this op's own working space. Computing `luma` via `computeLuma()`
// directly on the incoming linear `rgb` (the same call Saturation and
// Grayscale already make, reusing `kRec709LumaWeights` as the default
// weight) keeps `t` and the stop positions the caller wrote directly
// comparable, with no second implicit scale factor between "the luma this
// op measures" and "the domain the caller authored stops in". Shaper-
// encoding the luma first was considered and rejected: ADR-0004 reserves
// the shaper domain specifically for Curves' AUTHORED control points (a
// format-level commitment because saved curve numbers are coordinates in
// that domain); a gradient map's stops are not shaper-domain coordinates by
// any existing contract, and inventing a second, silent domain conversion
// here would make this op disagree with every other reader of the same
// `GradientStops` list (renderGradient() feeds it document-position `t`,
// not a re-encoded one).
struct GradientMapParams {
  // Straight linear RGB colour stops and (unused here -- see below)
  // opacity stops, positioned independently per Gradient.hpp §1.
  GradientStops stops;
  // Reused, not reinvented -- the same default and the same override point
  // Saturation/Grayscale already expose.
  std::array<float, 3> lumaWeights = kRec709LumaWeights;
};

// luma = computeLuma(rgb, p.lumaWeights); output = gradientColorAt(p.stops,
// luma). Opacity stops are never consulted: this whole file's contract is
// "never sees alpha, never touches it" (PointOps.hpp's header comment),
// and `gradientColorAt()` -- unlike `gradientSampleStraight()` -- is
// already the RGB-only half of ops/Gradient.hpp's evaluation API, so no
// alpha value is ever produced here to discard.
//
// --- The identity decision, made deliberately, not left implicit ---
// A gradient map has no colour ramp that is naturally a no-op -- unlike
// this file's other five siblings (Levels, Curves, Exposure, Saturation,
// Channel mixer), there is no stop list whose gradientColorAt() output
// equals its input for every rgb, because gradientColorAt() only ever
// returns a value ON the ramp, and "the ramp equals the identity function"
// is not expressible as a finite stop list. Two answers were considered:
//
//   (a) Default to a conventional black -> white ramp. Familiar to anyone
//       who has opened a gradient-map dialog, but NOT an identity by any
//       definition -- a fully saturated red input would come back grey.
//   (b) No colour stops means passthrough: `applyGradientMap()` returns
//       `rgb` unchanged when `stops.colorStops` is empty, rather than
//       calling `gradientColorAt()` at all.
//
// **(b) is what this file implements.** The reason is the same invariant
// every other op here already satisfies and that this file's own top-of-
// header contract states unconditionally: a default-constructed params
// struct is an exact identity. `GradientStops{}` is already empty by its
// own default (`std::vector` default-constructs empty) with no extra work
// needed, so choosing (b) makes `GradientMapParams{}` satisfy that
// invariant for free and keeps it true uniformly across all seven ops in
// this file family -- a caller (or a future LUT-bake pass walking a run of
// ops looking for a no-op to elide) never needs a special case for "except
// gradient map, whose identity is a whole authored ramp instead of a
// default-constructed struct." (a)'s black-to-white ramp is the right
// default for an interactive gradient-map DIALOG, where a user is expected
// to already be looking at stops -- but this file has no dialog and is
// judged purely as a pure-math function family, and bending its identity
// rule to match a UI convention this file does not own is the worse trade.
// This is deliberate, not an oversight: runMonoOpsTest() asserts (b)
// exactly, on more than one input, including an HDR one.
//
// Note this returns NOTHING like `gradientColorAt()`'s own "zero stops"
// behaviour (Gradient.hpp: "Zero stops returns black") -- that convention
// belongs to `renderGradient()`'s fill semantics ("no colour stops -> the
// gradient has no colour, so it renders nothing"), a decision about
// whether to paint a texel at all, which has no bearing on what THIS op's
// identity should be for an rgb value that is already there.
//
// --- Above the gradient's own domain: not this op's clamp ---
// `gradientColorAt()` already documents its own out-of-range behaviour:
// "Outside the stop range this extrapolates FLAT (the nearest end stop's
// colour)". A luma above the highest stop position (legitimate scene-linear
// HDR headroom, per this file's no-clamp policy) therefore reads back the
// top stop's colour, unchanged no matter how far above 1.0 the luma climbs
// -- and a luma below the lowest stop position reads the bottom stop's
// colour the same way. This is NOT a clamp `applyGradientMap()` imposes: it
// is `gradientColorAt()`'s own pre-existing, already-documented contract
// for a reused function, inherited unchanged rather than re-decided here.
// runMonoOpsTest() asserts the flat plateau on both sides, including that
// two different luma values far above the top stop produce the identical
// colour (proving the plateau is truly flat, not merely a close clamp).
std::array<float, 3> applyGradientMap(const std::array<float, 3>& rgb,
                                       const GradientMapParams& p) noexcept;

}  // namespace np
