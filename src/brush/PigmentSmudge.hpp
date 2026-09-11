#pragma once

#include <cstdint>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/Pigment.hpp"
#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"

// brush/PigmentSmudge -- **dragging paint that is already on a Pigment layer,
// and mixing it under Kubelka-Munk as it goes.**
//
// ==========================================================================
// 0. Why this exists now, and why it is a sibling of brush/Smudge
// ==========================================================================
//
// PRD F7 (**P1**): "Smudge on RGB and Pigment layers". CONTEXT.md's glossary
// says the same thing in the words this module has to live up to: smudge
// "works on **RGB** and **Pigment** layers -- and on a Pigment layer it mixes
// under Kubelka-Munk." Until this module the Pigment half of that row was
// refused by name, and `brush/Smudge` §7 and `app/StrokeSession.hpp` §1 both
// stated the one condition that would open it:
//
//   > "The row opens when someone decides what the mass-weighted mean of a
//   > footprint of latents is and asserts it, not before."
//
// §1 is that decision and `app/selftest/PigmentSmudge.cpp` is the assertion.
// The objection was never plumbing -- the dab stream, the falloff, the grain,
// the selection gate and the tile loop are all `brush/Smudge`'s, borrowed with
// their reasons. It was that `brush/Smudge` §2's pick-up is an ARITHMETIC mean
// of premultiplied quadruples, and an arithmetic mean of latents is a second
// mixing rule sitting beside the one this application exists for.
//
// **Why a sibling module and not a `PigmentTileStore&` overload on
// `SmudgeStroke`**, for `brush/PigmentErase` §0's reason one tool over: the two
// share everything around the texel and differ in what the finger IS -- four
// premultiplied floats against a straight latent beside a mass -- and in what
// a texel emptied of paint is allowed to hold (`brush/PigmentErase` §3: a stale
// hue at mass 0 is well-formed here and malformed there). One class holding
// both fingers would put both conventions inside one loop whose arithmetic
// comment describes only one of them.
//
// ==========================================================================
// 1. The mass-weighted mean of a footprint of latents -- the decision
// ==========================================================================
//
// **The answer is the one the brush already gives.** `brush/Deposit` §1's
// "invariant's other half, which `--selftest` found rather than confirmed":
// below saturation, any sequence of deposits leaves
//
//     z = sum(z_i * dm_i) / sum(dm_i)
//
// -- the latent is the mean of the latents present, **weighted by how much of
// each is present**. That is what a Kubelka-Munk mix of those paints is in this
// build (a lerp of latents is a KM mix, `core/Blend`'s `mixLatents()`), and it
// is the only mixing rule the codebase has. So the pick-up is that same mean
// taken over the footprint, with each texel's paint counted by its coverage
// times its mass:
//
//     W     = sum_i(w_i)                          // w_i = grain * dabCoverage
//     M     = sum_i(w_i * m_i)
//     pick.mass   = M / W
//     pick.latent = sum_i(w_i * m_i * z_i) / M    // (M == 0 -> no paint, §3)
//
// It is `brush/Smudge` §2's premultiplied mean, read in the one storage where
// "premultiplied" means `(latent * mass, mass)`: `core/Composite` projects a
// Pigment texel as `(latentToRgb(latent) * mass, mass)`, mass IS the alpha, and
// weighting each latent by its mass is exactly what premultiplying does to a
// colour. The two modules therefore agree about how much paint a footprint
// holds (`pick.mass` is `brush/Smudge`'s `pick[3]` over the projected texels)
// and differ only in what "the colour of it" means -- a mean of reflectance
// latents rather than of display-referred light.
//
// **Three things are decisions and are asserted:**
//
//   (i) **A texel at mass 0 contributes coverage to `W` and nothing to the
//       latent.** `brush/Smudge` §2(iii)'s rule -- emptiness is picked up, so a
//       smear thins as it leaves the paint -- arrives here in the mass and only
//       in the mass. A zero-mass texel's latent is either `Latent{}` (never
//       painted) or a stale hue `brush/PigmentErase` §3 deliberately left
//       behind, and neither is paint. The rejected arithmetic mean averages
//       them in: `Latent{}` is `c = {0,0,0}`, whose implied fourth Mixbox
//       weight is 1, i.e. **white**, so every smear that touched blank paper
//       would drift toward white -- and a smear across an erased patch would
//       resurrect the hue the eraser took away. `--selftest` computes that
//       rejected mean on the identical footprint and asserts it is different.
//
//   (ii) **One pigment in, the same pigment out, at ZERO tolerance.** The
//       latent mean is accumulated as a running `mixLatents()` --
//       `z_acc = lerp(z_acc, z_i, w_i*m_i / M_so_far)` -- rather than as the
//       quotient. The two are the same number in exact arithmetic; the lerp
//       form is chosen for `brush/Deposit` §1(i)'s reason: `std::lerp(a, a, t)`
//       is specified to return `a`, so a footprint of one pigment at any mix of
//       masses picks up *that pigment*, bit for bit, and so does every write
//       below. That is `brush/Deposit` §1's idempotence-in-hue invariant held
//       by a tool that moves paint rather than adds it, and it is the property
//       that makes a smudge on a Pigment layer safe: **it can mix the paints
//       already in the picture and cannot invent one.**
//
//   (iii) **It agrees with the brush.** A footprint half pigment A at mass 1
//       and half pigment B at mass 0.25 picks up `mixLatents(A, B, 0.2)` --
//       the latent `depositTexel()` produces when 0.25 of B is laid on 1.0 of
//       A. `--selftest` asserts the two against each other, which is the
//       mechanical form of "the same mixing rule, not a second one".
//
// ==========================================================================
// 2. The finger and the write: `brush/Smudge` §3 with mass as the alpha
// ==========================================================================
//
// Per dab, with `pick` from §1, `strength` latched at pen-down and `a =
// clamp(flow * cov * sel * strength)` exactly as `brush/Smudge` §3 derives it:
//
//     finger'.mass   = lerp(pick.mass, finger.mass, strength)
//     finger'.latent = mixLatents(pick.latent, finger.latent,
//                                 s*f.m / ((1-s)*p.m + s*f.m))
//
//     dst'.mass      = lerp(dst.mass, finger'.mass, a)
//     dst'.latent    = mixLatents(dst.latent, finger'.latent,
//                                 a*f.m / ((1-a)*d.m + a*f.m))
//
// **Every mass is `brush/Smudge`'s alpha lerp, and every latent is the §1 rule
// applied to what survives the lerp** -- `(1-a)*dst.mass` of the destination's
// paint and `a*finger.mass` of the finger's, mixed in proportion. So the stored
// latent is always the mass-weighted mean of the paint actually present, which
// is `depositTexel()`'s invariant, and the stored mass is always a convex
// combination of two masses that were each at most `kMaxMass`, so the cap holds
// without a clamp that could bite (one is kept anyway at the point of storage,
// `brush/PigmentErase`'s discipline for the other end of the same range).
//
// **Both endpoints of strength are exact, for `brush/Smudge` §3's reasons with
// one more line of argument each.** At strength 0, `a == 0` and the texel is
// returned bit-identical -- a whole stroke is a byte-for-byte no-op with no undo
// step. At strength 1 the finger's mass is `lerp(_, f.m, 1) == f.m` and its
// latent weight is `f.m / (0*p.m + f.m) == 1` exactly, so the finger never
// changes after the first dab loads it. And `a == 1` writes the finger exactly:
// `(1-1)*d.m == 0`, so the latent weight is `a*f.m / a*f.m == 1`.
//
// **The two latent endpoints that matter most are exact by the same
// arithmetic, and they are §1(i) seen from the write side:**
//
//   * **A loaded finger over a mass-0 texel lays down the finger's latent
//     outright** (`(1-a)*0 == 0`, weight 1) -- whatever stale hue the texel
//     held is not mixed in. That is `depositTexel()`'s §1(ii) rule ("`w = 1`
//     is the answer that erases that bias"), reached by the same limit.
//   * **An empty finger over paint thins it and leaves its hue exactly where it
//     was** (weight 0) -- less paint of the same colour, which is PRD F10's
//     definition of taking paint away and the Pigment reading of
//     `brush/Smudge`'s "a texel half *present* rather than half *bright*".
//
// **The first dab LOADS the finger outright** -- `brush/Smudge` §3's latch,
// unchanged, for the unchanged reason: blending from an empty start would make
// a strength-1 smudge carry nothing forever.
//
// ==========================================================================
// 3. Costs, the selection, and what is borrowed unchanged
// ==========================================================================
//
// **"Nothing to move" is on MASS, not on all seven channels** -- the pigment
// eraser's §4 inversion of the RGB rule, for its reason. A texel is left alone
// when both it and the finger hold no paint (`!(mass > 0)` on both, whatever
// stale latents either carries), or when it already equals the finger exactly.
// Testing the latents of an empty pair too would make every texel the pigment
// eraser has finished with look occupied, and a smudge dragged across an erased
// patch with an empty finger would rewrite each one with an unchanged mass 0 and
// dirty a **224 KiB** tile for no change.
//
// **An absent tile is skipped whole while the finger holds no paint**, and
// otherwise read as empty and written into -- `brush/Smudge` §6 both halves: a
// smudge GROWS the painted region, so it must allocate tiles a loaded finger
// passes over, and must not allocate one for dragging nothing across nothing.
// On this storage the second half is worth more: every such tile would be 224
// KiB, reported dirty, re-composited and re-uploaded per frame of the drag.
//
// **The selection bounds the write and not the pick-up** -- `brush/Smudge` §4,
// every word of it, including its honest limit: there is no per-stroke
// accumulator (a finger's colour changes every dab, so a scalar "fraction
// replaced" would be a fraction of nothing in particular), so within one
// stroke a partly-selected texel scrubbed repeatedly converges on the finger.
// What that leaves unbounded is the same pair of quantities it leaves unbounded
// there: which paint a texel holds (`brush/Deposit` §4 leaves the latent
// uncapped by design) and, rate-gated only, how much.
//
// **Strength is `BrushTip::smudgeStrength`**, the same field, the same slider
// and the same default as the RGB route (`brush/Smudge` §3b) -- one tool, one
// STRENGTH control, whichever layer kind is under it.
//
// ==========================================================================
// 4. What is deliberately not here
// ==========================================================================
//
// **No wet mix.** DESIGN-imaging.md's table is explicit and CONTEXT.md repeats
// it: a smudge displaces paint; *wet mix* carries a persistent reservoir that
// loads and unloads and needs a film depth and a contact mask a flat Pigment
// layer does not have. The finger here is a per-stroke colour that is emptied
// at pen-up, exactly as `brush/Smudge`'s is -- it is not a reservoir, it does
// not run dry, and it is not the Media layer's `oil_transfer.wgsl`.
//
// **No `Document`, no `Layer`, no `History`** -- the boundary every module in
// this family draws.
//
// **No finger painting and no sample-all-layers**, for `brush/Smudge` §7's
// reasons: `BrushTip::pigment` is not read (a smudge that reached for the
// loaded paint would be a brush), and the pick-up reads the target layer's own
// store and nothing else.
namespace np {

// §2's write, as a pure function of one texel -- asserted directly for
// `smudgeTexel()`'s reason: the invariants are about this arithmetic.
//
// `dst` is the stored texel; `finger` is the carried paint ALREADY updated for
// this dab (`smudgePigmentFinger()`); `weight` is `flow * coverage` with the
// selection folded in; `strength` is the stroke's latched strength.
//
// Total: every refusal -- `weight <= 0`, `strength <= 0`, a NaN in either, both
// texels empty of paint (§3, whatever their latents), and `dst == finger` --
// returns `dst` **bit-identical** with `dabAlpha == 0`, the caller's signal to
// skip the texel entirely rather than unshare its tile.
struct PigmentSmudgeStep {
  PigmentTexel texel{};   // the texel to store
  float dabAlpha = 0.0f;  // a; 0 means "nothing to do here"
};
PigmentSmudgeStep smudgePigmentTexel(const PigmentTexel& dst, const PigmentTexel& finger,
                                     float weight, float strength) noexcept;

// §2's finger update, pure and asserted directly. `loaded` is false only on a
// stroke's first dab, and then the answer is `pick` outright.
PigmentTexel smudgePigmentFinger(const PigmentTexel& finger, bool loaded, const PigmentTexel& pick,
                                 float strength) noexcept;

// One Pigment smudge stroke in flight: the latched strength and the carried
// paint. `SmudgeStroke`'s shape member for member, for `brush/Smudge` §1's
// reasons -- a finger from one stroke spent at another's strength is the
// combination binding the two at `begin()` makes unspellable.
class PigmentSmudgeStroke {
 public:
  // Pen-down: latches the strength (clamped to [0,1]) and empties the finger.
  void begin(float strength) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up: empties the finger and leaves `strength()` alone.
  void end() noexcept;

  // Smudges one dab across `store`, clipped to the canvas and gated by
  // `selection` (nullptr means no restriction). Two passes, `SmudgeStroke`'s:
  // §1's pick-up, which updates the finger exactly once, then the write. Every
  // tile written is appended to `touchedOut` at the moment it is first written.
  DepositCount smudgeDab(PigmentTileStore& store, const BrushTip& tip, Vec2 centre,
                         int32_t canvasW, int32_t canvasH, const Selection* selection,
                         std::vector<TileCoord>* touchedOut);

  // Every dab in `dabs`, in order -- order is the direction the paint travels.
  StrokeDeposit smudgeDabs(PigmentTileStore& store, const BrushTip& tip,
                           const std::vector<Vec2>& dabs, int32_t canvasW, int32_t canvasH,
                           const Selection* selection);

  // The carried paint and whether a dab has loaded it yet. Exposed because it
  // is what `--selftest` asserts §1 against directly: stored texels have been
  // through binary16 once per dab and this has not.
  const PigmentTexel& finger() const noexcept { return finger_; }
  bool loaded() const noexcept { return loaded_; }

  float strength() const noexcept { return strength_; }

 private:
  float strength_ = 1.0f;
  bool active_ = false;
  bool loaded_ = false;
  // 28 bytes: six latent floats and a mass. Still no per-texel accumulator
  // (§3), so a stroke across thirty 224 KiB tiles costs nothing beyond them.
  PigmentTexel finger_{};
};

}  // namespace np
