#pragma once

#include <array>

#include "ops/PointOps.hpp"

// ops/ColorOps (docs/operations.md §1.2 "Committed additions"). Four more
// class-A point ops -- Hue/Saturation/Lightness, Vibrance, Colour balance,
// Photo filter -- extending the family ops/PointOps.hpp starts. This file is
// PURE MATH ONLY: no menu, no dialog, no op-stack integration. Those are
// separate, later, unbuilt steps owned elsewhere.
//
// Every function here inherits PointOps.hpp's contract verbatim -- re-read
// that header's own opening comment before touching this one:
//  - `std::array<float,3> -> std::array<float,3>` on straight (NON-
//    premultiplied), scene-linear RGB. Never sees alpha, never touches it.
//  - Does NOT clamp its output. Scene-linear values legitimately exceed 1.0
//    (HDR headroom); the only permitted clamps below are ones a formula
//    *mathematically requires* (kept out of a negative `pow()` base, or a
//    division guarded away from zero), each one documented at its own site
//    exactly the way LevelsParams' `t` clamp is documented in PointOps.hpp.
//  - A default-constructed params struct is an *exact* identity (to the same
//    float tolerance PointOps.hpp's own tests hold themselves to -- see
//    app/selftest/PointOps.cpp's `nearRgb`/`kTol` precedent, which this
//    file's selftest reuses rather than inventing a second convention).
//  - No GPU/LUT/texture/tile/ImGui awareness whatsoever.
//
// Shaper domain vs linear, decided per op below (color/Shaper.hpp,
// docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md): a control
// authored against a perceptual sense of "brightness" belongs in the log-
// encoded shaper domain, the same reason Curves does; a control that is a
// physical light operation -- a multiply, or a geometric operation on the
// linear RGB vector itself -- belongs in linear, the same reason Exposure
// does. Each op states its own choice and why below; Hue/Saturation/
// Lightness actually splits across both within one op, which is exactly the
// kind of case this rule exists to make explicit rather than accidental.
namespace np {

// ---------------------------------------------------------------------
// 1. Hue / Saturation / Lightness (docs/operations.md §1.2 "Hue / saturation
// by range" minus the range qualifier -- this is the global ⌘U-shaped
// control the range-qualified version would sit on top of; PLAN.md's own
// ordering builds the global control first). Four independent knobs: hue
// rotation in degrees, a saturation scale, a lightness shift, and a
// `colorize` mode that maps every pixel to one hue while keeping its own
// luma.
//
// *** Working representation: rotation about the Rec.709 luma axis, not a
// naive RGB<->HSL round trip -- and why that matters ***
// Classic HSL is a real trap on scene-linear data for two independent
// reasons: (a) HSL's own lightness `L = (max+min)/2` is not this codebase's
// luma (kRec709LumaWeights/computeLuma() -- a plain average of the two
// extreme channels, weighted 0 toward the middle channel entirely), so a
// naive HSL round trip would quietly disagree with every other luma
// computation in this file and in PointOps.hpp; and (b) HSL's own hue/
// saturation formulas are built assuming RGB is confined to [0,1] (`max`,
// `min` and their ratios are the whole mechanism) and have no defined
// behaviour once scene-linear values exceed 1.0 or go negative -- exactly
// the HDR headroom this module is contractually required to pass through
// undamaged.
//
// The fix used here: rotate the RGB vector itself, in 3-D, about the axis
// `k = normalize(kRec709LumaWeights)` -- i.e. the Rec.709 weight vector
// turned into a unit direction in RGB space -- using Rodrigues' rotation
// formula (standard vector-algebra identity, no colour-science pedigree
// needed beyond "rotate a vector about an arbitrary axis"):
//
//   v_rot = v*cos(theta) + (k x v)*sin(theta) + k*(k . v)*(1 - cos(theta))
//
// This axis is not an arbitrary choice: the component of `v` along the
// rotation axis is invariant under a rotation about that axis by
// construction (basic property of any axis-angle rotation), and because
// `k` is parallel to kRec709LumaWeights, that invariant component is
// exactly proportional to `computeLuma(v)`. Rotating about this specific
// axis is therefore the one rotation that leaves this codebase's own luma
// untouched for every angle -- hue, saturation and colorize all agree on
// what "luma" means because all three ultimately go through computeLuma().
// A pure hue rotation applies this to the pixel's *deviation from its own
// luma* (`dev = rgb - luma*(1,1,1)`, the same decomposition
// applySaturation() already uses), not to the raw RGB triple -- `w . dev`
// is exactly 0 for any starting `rgb` (algebraic consequence of
// kRec709LumaWeights summing to 1.0), so the rotated deviation is still
// exactly luma-neutral for any angle, and adding it back to `luma*(1,1,1)`
// reconstructs a pixel whose luma is unchanged to the bit.
//
// A real, openly stated consequence: this rotation can and does produce
// negative channel values partway around the circle (the classic drawback
// of any matrix-based RGB hue rotation -- rotating a saturated primary's
// deviation vector swings some channels below zero before it swings them
// back positive). That is not a bug this module papers over: per the no-
// clamp contract above, those negative values pass straight through.
//
// Domain: hue rotation and the deviation decomposition both happen in
// *linear* RGB, not the shaper domain -- this is a literal geometric
// rotation of the physical RGB triple, and the luma-invariance property
// above is proved in the same linear space computeLuma() itself operates
// in; running it through shaperEncode()'s nonlinear per-channel curve first
// would break that invariance (a nonlinear per-channel remap does not
// commute with a 3-D rotation or with the weighted dot product luma is).
//
// Saturation: `p.saturation` is applied by literally calling
// applySaturation() (SaturationParams{p.saturation, kRec709LumaWeights})
// on the hue-rotated result -- not a hand-rolled equivalent -- so this
// op's saturation half is identical to, not merely "consistent with",
// PointOps.hpp's own Saturation op. Also linear, for the same reason
// applySaturation() itself is (PointOps.hpp's own §4).
//
// Lightness: `p.lightness` is the one perceptual control here, and runs in
// the *shaper* domain deliberately -- this is Photoshop-⌘U's "Lightness"
// slider, a perceptually-authored brighten/darken, not a physical stop
// count (that is what Exposure is for) -- shaperEncode() -> add
// `p.lightness` -> shaperDecode(), per channel, matching Curves' own
// domain choice and reasoning. `p.lightness == 0.0f` skips the round trip
// entirely rather than relying on shaperDecode(shaperEncode(x)) == x to
// only float tolerance, mirroring applyCurves()'s own documented reason
// for the identical skip (PointOps.hpp's evalCurve() doc comment).
//
// Colorize: replaces the hue-rotate-and-scale step above with "every pixel
// gets the *same* hue and saturation, but keeps its *own* luma" (the
// Photoshop-⌘U "Colorize" checkbox's behaviour). Built from one fixed
// reference chroma direction -- pure red's own deviation from its own luma,
// `refDev = (1,0,0) - luma(1,0,0)*(1,1,1)`, itself exactly luma-neutral by
// the same algebraic fact above -- rotated by `p.colorizeHueDegrees`,
// scaled by `p.colorizeSaturation`, and added to `luma(rgb)*(1,1,1)` (the
// *current* pixel's own grey point, valid because kRec709LumaWeights sums
// to 1.0 so a uniform triple `(m,m,m)` has luma exactly `m`). Luma
// preservation is therefore exact by the same rotation-invariant-component
// argument as ordinary hue rotation, not merely approximate.
struct HueSaturationParams {
  float hueDegrees = 0.0f;          // rotation about the luma axis; 0 = identity, 360 round-trips.
  float saturation = 1.0f;          // SaturationParams::scale convention: 1 = identity, 0 = grey.
  float lightness = 0.0f;           // shaper-domain additive shift; 0 = identity.
  bool colorize = false;            // false: ordinary hue/sat/lightness. true: map to one hue.
  float colorizeHueDegrees = 0.0f;  // colorize target hue, same rotation convention as hueDegrees.
  float colorizeSaturation = 0.5f;  // colorize target saturation; irrelevant when colorize == false.
};

std::array<float, 3> applyHueSaturation(const std::array<float, 3>& rgb,
                                         const HueSaturationParams& p) noexcept;

// ---------------------------------------------------------------------
// 2. Vibrance (docs/operations.md §1.2: "saturation weighted by existing
// saturation"). The saturation MEASURE is the substance of this op, stated
// explicitly rather than left implicit:
//
//   S = (max(rgb) - min(rgb)) / max(|max(rgb)|, epsilon)
//
// The classic HSV-style relative-chroma ratio -- 0 for a neutral grey
// (max == min), 1 for a fully saturated primary component (min == 0), and,
// deliberately left unclamped past that range for HDR/negative inputs (see
// below) rather than forced back into [0,1] -- an op-output clamp this
// module's contract forbids, and `S` feeds this op's own weighting, not the
// final pixel, so clamping it would be a policy choice hiding inside an
// internal quantity, exactly what the no-clamp policy exists to prevent.
//
// The per-pixel adaptive scale, fed straight into applySaturation() --
// reused, not reimplemented, exactly like the saturation half of
// Hue/Saturation/Lightness above:
//
//   scale = 1 + amount * (1 - kProtect * S),  kProtect = 0.5
//
// `kProtect = 0.5` is a deliberate, documented design constant, not a
// tunable parameter: it is chosen so the *absolute* RGB movement this op
// produces is monotonically increasing in `S` across the whole practical
// range `S in [0,1]` (movement is proportional to `(1 - kProtect*S) * S`
// once `S`'s own definition is substituted in, and that product's peak sits
// at `S = 1/(2*kProtect)` -- pinning `kProtect` at 0.5 pushes the peak to
// `S = 1`, i.e. off the end of the practical range, rather than into its
// middle where a "pastel moves more than a fully-saturated colour" reversal
// would appear). This is exactly the property runColorOpsTest() pins: a
// grey (S=0, zero movement by construction -- its deviation-from-luma is
// zero, so any scale multiplies it to zero) must move less than a pastel,
// which must move less than an already-saturated colour, even though the
// already-saturated colour gets the *smaller* scale-1 gain -- its larger
// starting deviation from luma more than makes up for the smaller gain.
struct VibranceParams {
  float amount = 0.0f;  // 0 = identity.
  std::array<float, 3> lumaWeights = kRec709LumaWeights;
};

std::array<float, 3> applyVibrance(const std::array<float, 3>& rgb,
                                    const VibranceParams& p) noexcept;

// ---------------------------------------------------------------------
// 3. Colour balance (docs/operations.md §1.2: "lift / gamma / gain by
// tonal range -- the grading control"). Three independent RGB triples --
// `shadowsLift`, `midtonesGamma`, `highlightsGain` -- each a signed push
// per channel, blended in by how far the *pixel's own* luma sits into that
// tonal range, plus an optional preserve-luminosity pass.
//
// *** Tonal-range weights -- the substance of this op ***
// Three overlapping "tent" functions of `L = computeLuma(rgb)`:
//
//   w_shadow(L)    = clamp(1 - 2L, 0, 1)
//   w_highlight(L) = clamp(2L - 1, 0, 1)
//   w_midtone(L)   = 1 - w_shadow(L) - w_highlight(L)
//
// `w_shadow` and `w_highlight` are never both nonzero at the same `L`
// (`w_shadow` is 0 for `L >= 0.5`, `w_highlight` is 0 for `L <= 0.5`), so
// `w_midtone` -- defined as the *remainder*, not a third independent tent
// -- is a genuine partition of unity: `w_shadow(L) + w_midtone(L) +
// w_highlight(L) == 1` for every real `L`, not just inside [0,1]. That
// last part matters for the no-clamp-output contract: an HDR pixel with
// `L > 1` gets `w_highlight = 1, w_shadow = w_midtone = 0` (full highlight
// weight, no contribution from the other two, no NaN, no blow-up) rather
// than the weights running away or leaving a gap that double-counts or
// under-counts the midtone band -- exactly what "make sure the three
// weights sum sensibly ... rather than double-counting the midtones" asks
// for.
//
// *** Per-channel formula -- ASC CDL-shaped (SMPTE ST 2095's published
// "slope / offset / power" grade primitive; the "lift/gamma/gain" naming
// this op uses throughout is that same primitive's colour-grading name),
// with each control weighted by the tonal-range function above ***
//
//   offset[c] = shadowsLift[c]    * w_shadow(L)
//   gain[c]   = 1 + highlightsGain[c] * w_highlight(L)
//   gammaDenom[c] = 1 + midtonesGamma[c] * w_midtone(L)   (epsilon-guarded, see below)
//   base[c] = rgb[c] * gain[c] + offset[c]
//   output[c] = base[c]                          if gammaDenom[c] == 1 (no gamma push at this pixel)
//             = pow(max(base[c], 0), 1/gammaDenom[c])   otherwise
//
// Two required-by-math clamps, neither a policy decision:
//  - `gammaDenom` is guarded away from 0 (mirrors LevelsParams' own
//    `whiteIn - blackIn` epsilon guard in PointOps.cpp) -- undefended, a
//    `midtonesGamma`/weight combination landing exactly on -1 divides by
//    zero and the resulting `1/0` power poisons the channel.
//  - `base` is floored at 0 only on the branch that actually calls `pow()`
//    with a non-1 exponent -- the same "negative base, fractional exponent
//    is NaN in general" necessity LevelsParams documents, scoped no wider
//    than the branch that needs it. The `gammaDenom == 1` branch (which
//    covers every default-constructed channel, since `midtonesGamma[c] ==
//    0` makes `gammaDenom[c] == 1` exactly) passes `base` through
//    unclamped, unlike Levels -- this is what keeps a default-constructed
//    ColorBalanceParams an *exact* identity for negative and HDR inputs
//    alike, not merely within [blackIn, whiteIn] the way Levels' own
//    identity is scoped.
//
// Preserve luminosity: when `preserveLuminosity` is set, the result is
// rescaled so `computeLuma(output) == computeLuma(rgb)` exactly (to float
// tolerance) -- `output *= computeLuma(rgb) / computeLuma(output)`,
// skipped (no rescale) if `computeLuma(output)` is at or below an epsilon,
// mirroring core/Premultiply's own "nothing sensible to rescale toward"
// guard for a degenerate denominator.
//
// Domain: linear, matching Levels' own precedent (Levels operates directly
// on linear values too -- only Curves shapes) -- the tonal-range weights
// are themselves luma-based, and luma is a linear-light quantity in this
// codebase; shaping first would make `w_shadow`/`w_midtone`/`w_highlight`
// measure something other than the luma this op's own formulas are stated
// against.
struct ColorBalanceParams {
  std::array<float, 3> shadowsLift = {0.0f, 0.0f, 0.0f};
  std::array<float, 3> midtonesGamma = {0.0f, 0.0f, 0.0f};
  std::array<float, 3> highlightsGain = {0.0f, 0.0f, 0.0f};
  bool preserveLuminosity = true;
};

std::array<float, 3> applyColorBalance(const std::array<float, 3>& rgb,
                                        const ColorBalanceParams& p) noexcept;

// ---------------------------------------------------------------------
// 4. Photo filter. Not in docs/operations.md's own table (that document
// covers the PRD's committed list; this one is built alongside it as part
// of the same op family, from the published-literature convention this
// class of tool always uses -- a Wratten-style gel filter's colour cast
// composited over the image at a strength, with an optional luminance-
// preserving rescale, not from any single vendor's implementation).
//
//   filtered[c] = rgb[c] * color[c]
//   blended[c]  = rgb[c] + density * (filtered[c] - rgb[c])
//              == rgb[c] * (1 + density * (color[c] - 1))
//
// `density == 0` is identity regardless of `color` (the `density *` factor
// zeroes the whole push); `color == (1,1,1)` (a colourless/neutral filter)
// is identity regardless of `density`, for the same reason. Both are true
// simultaneously in the default-constructed struct below.
//
// *** What "density" multiplies, and why a naive full multiply darkens ***
// `density` is a blend fraction between the untouched pixel and the fully
// filtered one -- it does NOT multiply brightness directly. A real gel
// filter's colour is necessarily a saturated hue, meaning at least one of
// its channels is `< 1.0` (an ideal neutral-density-only filter has no
// colour to speak of); multiplying scene light by any RGB triple whose
// channels are all `<= 1.0` can only hold each channel steady or reduce
// it, never raise it, so `computeLuma(filtered) <= computeLuma(rgb)`
// whenever `color != (1,1,1)` -- a `density == 1` naive multiply always
// darkens a real (non-neutral) filter colour, exactly the "gel filters
// steal light" behaviour photographers compensate for with a longer
// exposure and this op's `preserveLuminosity` flag exists to undo without
// one.
//
// Preserve luminosity: identical rescale to ColorBalance's own -- `output
// = blended * computeLuma(rgb) / computeLuma(blended)`, skipped when
// `computeLuma(blended)` is at or below an epsilon (nothing sensible to
// rescale toward, same core/Premultiply-style guard).
//
// Domain: linear -- this is a physical light multiply (a real gel filter
// attenuates specific wavelengths of the actual scene light hitting the
// sensor), the same reasoning Exposure states for its own stops multiply,
// not a perceptually-authored curve.
struct PhotoFilterParams {
  std::array<float, 3> color = {1.0f, 1.0f, 1.0f};  // filter colour; (1,1,1) = neutral/no cast.
  float density = 0.0f;                             // 0 = identity, regardless of color.
  bool preserveLuminosity = true;
};

std::array<float, 3> applyPhotoFilter(const std::array<float, 3>& rgb,
                                       const PhotoFilterParams& p) noexcept;

}  // namespace np
