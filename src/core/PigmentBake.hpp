#pragma once
#include <array>

#include "core/Pigment.hpp"

namespace np {

// The solver-to-document mapping: what a texel of sim/PaintSim's deposited
// pigment fields becomes as a `PigmentTexel` in a layer's tile.
//
// This is the arithmetic half of the stroke bridge -- the thing that has been
// missing since the solver was written, and the reason PLAN.md's roadmap
// section 11 could not start. It is deliberately in `core/` and deliberately
// GPU-free: it is a claim about numbers, and it is asserted as one.
//
// --- 1. The two are the same seven numbers -------------------------------
//
// `shaders/include/mixbox.wgsl`'s `latentFromMass()` divides a
// premultiplied-by-mass pair `(c*m, m)` / `(res*m, _)` back into six floats.
// `core/Pigment.hpp`'s `PigmentTexel` is `{Latent{c[3], res[3]}, mass}` -- the
// same six, in the same order, with the same premultiplied storage
// convention. CONTEXT.md says why that is not a coincidence: a Media layer is
// a Pigment layer with a solver attached, not a second storage format.
//
// So the latent needs no mapping at all. **The mass does**, and that is the
// one real decision here.
//
// --- 2. Why the mass cannot simply be copied ------------------------------
//
// `shaders/composite.wgsl` puts the solver on screen through Beer-Lambert:
//
//     opacity = 1 - exp(-absorption * mass)
//
// while `core/Composite` renders a Pigment tile as `(latentToRgb(l)*m, m)` --
// the mass IS the alpha, linearly. Those disagree, and the disagreement is
// visible: copying the raw mass makes a light wash jump by up to 0.24 in alpha
// the instant it is baked, and a heavy one darken slightly. A flash nobody
// could miss, at the exact moment a stroke is supposed to settle.
//
//     sim mass    on screen      if mass copied     delta
//        0.20       0.4055            0.2000       +0.2055
//        0.50       0.7275            0.5000       +0.2275
//        1.00       0.9257            1.0000       -0.0743
//
// **So the tile stores the Beer-Lambert coverage as its mass**, not the
// solver's quantity. Four reasons, in descending order of force:
//
// 1. It is *exactly* what is already on screen, so a bake produces no flash at
//    all -- not a small one, none.
// 2. It lands in [0,1], which is what `mixLatents()` and `mixedPairTexel()`
//    require. The raw mass does not: two overlapping dabs already exceed 1.
// 3. **`over` on Beer-Lambert coverages IS Beer-Lambert accumulation, to the
//    bit.** For a1 = 1-e^-k*m1 and a2 = 1-e^-k*m2,
//
//        a1 + a2*(1-a1) = 1 - (1-a1)(1-a2) = 1 - e^-k*(m1+m2)
//
//    so two glazes baked separately and composited `over` equal one bake of
//    the summed mass. A rolling bake behind a long stroke therefore cannot
//    drift from a single bake at the end of it. Nothing else has this
//    property, and `bakeCompositionHolds()` exists to assert it rather than
//    leave it as an argument in a comment.
// 4. ADR-0007 survives: "erase reduces Mass toward zero, leaving the Latent
//    untouched" is still true and still linear, because scaling a coverage
//    toward zero is as valid as scaling a quantity was.
//
// --- 3. What this costs, stated -------------------------------------------
//
// **Re-wetting is no longer exactly invertible at the top end.** Going back
// the other way needs `-ln(1 - mass)/absorption`, which is singular as mass
// approaches 1, so `simMassFromBaked()` clamps at `kMaxBakedMass`. That is
// not a fudge: f16 storage cannot represent a coverage closer to 1 than that
// anyway, so the clamp is at the precision boundary rather than inside it.
//
// **The fibre gain is deliberately NOT baked.** `composite.wgsl` multiplies
// the pigment colour by `(0.93 + 0.07*fibre)` (watercolour) or
// `(0.90 + 0.10*fibre)` (ink). That is paper grain in the colour channel;
// folding it into `latent.res` would make it mix as though it were pigment,
// which it is not. A baked stroke therefore loses up to 7% of spatial grain
// modulation. The right home for it is a paper layer under the document,
// applied by the view -- a product decision, flagged here, not made here.
//
// --- 4. Why only the DEPOSITED fields ------------------------------------
//
// `composite.wgsl` shows `depC + pigC*0.75` (watercolour) or `depC + pigC*0.55`
// (ink): settled pigment plus a weakened contribution from pigment still
// suspended in the water film. `projectSolverTexel()` takes **only the
// deposited pair**, and that is correct precisely because the bake is
// triggered by drying: a dry texel has no water film left, so its suspended
// mass has already transferred to the deposited field and `pigC` is zero.
//
// Baking a *wet* texel through this function would silently drop the
// suspended fraction. `suspendedWeightFor()` exists so a caller that must do
// that -- a forced bake on undo-while-wet -- can fold it in with the same
// weight the shader uses rather than inventing a second one.

// The Beer-Lambert absorption coefficients, which two languages have to agree
// on. These are the source; `shaders/composite.wgsl` hard-codes the same two
// numbers today, and `--selftest` asserts the values so a change on one side
// without the other shows up as a failure rather than as a slow drift.
inline constexpr float kAbsorptionWatercolor = 2.6f;
inline constexpr float kAbsorptionInk = 4.2f;

// How much a texel's still-suspended pigment contributes on screen, per
// medium. Not used by a dry bake -- see section 4 above.
inline constexpr float kSuspendedWeightWatercolor = 0.75f;
inline constexpr float kSuspendedWeightInk = 0.55f;

// The largest coverage a baked tile may hold. f16 carries an 11-bit
// significand, so nothing between this and 1.0 survives storage; clamping here
// keeps `simMassFromBaked()` finite without pretending to a precision the tile
// does not have. At the watercolour coefficient this maps back to a sim mass
// of about 2.93, already past visual saturation (1 - e^-2.6*2.93 = 0.99951).
inline constexpr float kMaxBakedMass = 1.0f - 1.0f / 2048.0f;

// The divisor guard, which must match `shaders/include/mixbox.wgsl`'s
// `latentFromMass()` character for character. A CPU bake using 1e-6 or 0
// would give a different latent for every trace-mass texel -- and those are
// exactly the soft edges of every stroke, so the difference would show up
// where it is least acceptable.
inline constexpr float kMassEpsilon = 1e-5f;

// `1 - exp(-absorption * simMass)`: the solver's unbounded quantity as the
// [0,1] coverage a Pigment tile stores. Monotonic, zero at zero.
float bakedMassFromSim(float simMass, float absorption) noexcept;

// The inverse, for re-wetting a baked tile back into the solver. Clamped at
// `kMaxBakedMass`; see section 3.
float simMassFromBaked(float bakedMass, float absorption) noexcept;

// One deposited solver texel as a document texel. `depC` is `(c0*m, c1*m,
// c2*m, m)` and `depR` is `(res.R*m, res.G*m, res.B*m, _)` -- `.w` of the
// second is written 0 by the solver and is ignored here rather than trusted.
PigmentTexel projectSolverTexel(const std::array<float, 4>& depC,
                                const std::array<float, 4>& depR,
                                float absorption) noexcept;

// Whether `over`-compositing two separately baked masses equals baking their
// sum, within `tolerance`. This is section 2's reason 3 as a predicate rather
// than a claim: it is what makes a rolling bake behind a long stroke provably
// identical to one bake at the end, and it is false for a linear mass copy,
// which is how `--selftest` shows the assertion is not vacuous.
bool bakeCompositionHolds(float simMassA, float simMassB, float absorption,
                          float tolerance) noexcept;

}  // namespace np
