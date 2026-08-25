#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/Pigment.hpp"
#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"

// brush/PigmentErase -- **taking paint off a Pigment layer: mass down, latent
// left exactly where it was.**
//
// ==========================================================================
// 0. Why this exists now and did not exist last batch
// ==========================================================================
//
// `brush/RgbErase` §0 and `app/StrokeSession` §1 both recorded, at length, that
// the Pigment row of ADR-0007's table was **refused by name** rather than
// built, and both gave the same single reason -- not the arithmetic, which
// ADR-0007 specifies completely, but the gate:
//
//   > "`depositDab(PigmentTileStore&, ...)` takes no `Selection` at all -- the
//   > pigment deposit route does not implement PRD E1 (**P0**) today [...] An
//   > eraser is the worst tool to give a half-answer there: gated, it would be
//   > the only thing on a Pigment layer that stopped at the ants, and un-gated
//   > it would destroy paint outside a selection the user had drawn precisely to
//   > protect it, with one undo step covering the whole stroke."
//
// `brush/Deposit` §4 is that gate. With it in place the objection is spent, and
// the two conditions the refusal named are now both met: the pigment deposit
// stops at the ants, and so does this.
//
// **Almost nothing here is new.** `dabCoverage()`, `dabPixelBounds()`,
// `BrushTip::spacingPx()`, `StrokeAlphaStore`, the hoisted tile loop with its
// lazily-fetched write handle, the skip-an-absent-tile asymmetry and the
// per-stroke floor are all `brush/RgbErase`'s, borrowed with their reasons.
// Exactly three things are genuinely different and they are §§1-3.
//
// **Why `PigmentErase` and not `Erase`.** `brush/Deposit` is the *pigment*
// deposit under an unprefixed name only because it was written first, before
// there was a second storage to distinguish it from; `brush/RgbDeposit` and
// `brush/RgbErase` then had to say which they were. A file called `brush/Erase`
// sitting beside `brush/RgbErase` would read as "the general one that RgbErase
// specialises", which is the opposite of the truth -- they are siblings over
// two storages. Naming the storage costs one word and removes that reading.
//
// ==========================================================================
// 1. What the Pigment analogue of alpha IS, checked rather than assumed
// ==========================================================================
//
// Four sources say the same thing in the same words, and they were read rather
// than summarised:
//
//   * `core/Pigment.hpp`, on `PigmentTexel`: "`mass` is the Pigment analogue of
//     alpha (PRD F10: erase 'reduces ... Mass on Pigment layers leaving the
//     Latent untouched'), and core/Composite projects the pair to a
//     premultiplied RGBA texel as `(latentToRgb(latent) * mass, mass)`."
//   * `docs/document-format.md`, on the seventh stored channel: "`pig.m` is the
//     seventh, and it is not part of the latent at all -- it is the Pigment
//     analogue of alpha, the quantity PRD F10's eraser reduces."
//   * ADR-0007's table row, and its argument: "Reducing mass while holding the
//     latent means a half-erased pixel is *less paint of the same colour* [...]
//     mass is a linear quantity, so scaling it is a valid operation on latents,
//     whereas pushing the latent toward 'nothing' is not defined."
//   * PRD F10 itself (**P0**), and PRD §7's acceptance row: "erase on a Pigment
//     layer | Mass falls, Latent unchanged | F10".
//
// `core/Composite.cpp`'s `projectPigmentTexel()` is the mechanical form of the
// first: `{rgb[0]*m, rgb[1]*m, rgb[2]*m, m}`. So mass really is the alpha, and
// scaling it really is linear in the projection -- halving mass halves all four
// projected channels, which is *exactly* what `brush/RgbErase` §1's
// destination-out does to a premultiplied RGBA texel. The two modules therefore
// agree on the composited result and differ only in how many stored numbers
// they have to touch to get it.
//
// `core/SelectionMask.hpp`'s `clearThroughSelection(PigmentTileStore&)` already
// implements the same rule for PRD M1's clear, in the same words ("scales
// **mass alone and leaves the latent untouched** -- which is not an analogy but
// PRD F10's own rule for the eraser"), and warns what scaling the latent as
// well would do: "a half-erased red would stop being red rather than becoming
// less of it. That is the pigment equivalent of the fringe the premultiplied
// path avoids." This module is that rule with a per-stroke floor on it.
//
// ==========================================================================
// 2. The per-stroke FLOOR, borrowed as an ARGUMENT and not as code
// ==========================================================================
//
// `brush/RgbErase` §2 derives it in full and none of it is repeated here; what
// matters is **whether it carries to a different storage**, and it does,
// because the accumulator does not hold any of that storage's units.
//
// The accumulator holds `E`, the **fraction of whatever was here that this
// stroke has taken away**. It is dimensionless, it starts at 0 at pen-down
// whatever the texel holds, and the arithmetic is:
//
//     E' = min(strength*sel, E + w*(1 - E))     // w = flow * cov * sel
//     e  = (E' - E) / (1 - E)
//     m' = m * (1 - e)                          // MASS ONLY; latent untouched
//
// giving, at any number of dabs and any dab order,
//
//     mass_final = mass_0 * (1 - strength*sel)      // the FLOOR
//
// which is `brush/RgbErase`'s `alpha_final = alpha_0 * (1 - strength*sel)` with
// `alpha` replaced by `mass`. The identity it rests on -- `1 - E' =
// (1 - E)(1 - e)` -- is about **repeated multiplication by factors in [0,1]**,
// and is therefore a statement about the *composition* of the dabs rather than
// about what is being multiplied. Scaling one f16 mass channel composes exactly
// as scaling four f16 premultiplied channels does. So the equation carries
// unchanged, and `mass_0` never has to be stored, because the multiplication is
// the memory.
//
// **The rejected per-dab model is the same one**, and it is worth restating
// only for its number: `e = flow * cov * strength` with no memory of the dabs
// before it retains `(1 - flow*cov*strength)^N`, so a "50 % eraser" scrubbed 50
// times over one spot removes 99.99 % of the paint. `--selftest` computes it on
// the identical inputs and asserts it is wrong, so the good assertion cannot
// pass against the bad implementation.
//
// **Strength is `BrushTip::opacity`**, latched at pen-down by the same
// `StrokeSession::begin()` that latches it for the RGB erase, meaning the same
// thing in the same units. `BrushTip::pigment` and `BrushTip::linearRgb` are
// **not read at all** -- the mechanical form of ADR-0007's rejection of "erase
// paints the background colour", which on a Pigment layer is wrong twice
// because white under Kubelka-Munk is opaque paint rather than the absence of
// paint.
//
// **The selection enters exactly twice**, for `brush/RgbErase` §3's reason and
// `brush/Deposit` §4's: into `w`, so one pass through a half-selected texel
// removes half of what it would; and into the cap on `E`, so no number of
// passes goes past half. With the first alone `E' = E + w(1-E)` still converges
// to `strength` for any positive `w`, so a feathered selection edge would come
// out hard for a slow stroke and soft for a fast one -- ADR-0003's speed
// dependence in another costume, on the one tool whose damage is invisible
// until the layer under it is.
//
// A null `Selection*` is "no restriction", 1.0 everywhere; a null *tile* inside
// an engaged selection is "selects nothing", 0.0. `core/SelectionMask.hpp`
// requires each hoisted per-texel loop to own its own copy of that branch and
// to be named where it does; this is one of them, and `--selftest` drives both
// nulls through it.
//
// ==========================================================================
// 3. A zero-mass texel keeps its hue, and that is NOT the malformed case
// ==========================================================================
//
// This is the one place the pigment eraser is genuinely not the RGB eraser, and
// getting it backwards would put a fringe on exactly the soft edges an eraser
// is used for -- which is the same symptom as the RGB mistake, arrived at from
// the opposite direction.
//
// `brush/RgbErase` §1 insists a fully erased texel hold **RGB 0 as well as
// alpha 0**, because `core::Tile` is PREMULTIPLIED: `(colour, 0)` is malformed,
// and `core/Composite` reads it as an additive glow contributing colour with no
// coverage.
//
// **A Pigment texel is not premultiplied.** `core/Pigment.hpp` stores a
// STRAIGHT latent beside a mass, and `projectPigmentTexel()` multiplies the two
// at composite time: `(latentToRgb(latent) * 0, 0)` is `(0,0,0,0)` exactly, for
// **any** finite latent, because `latentToRgb()` clamps its output to [0,1] and
// `finite * 0.0f` is exactly `0.0f`. So a texel at mass 0 with a stale hue
// contributes nothing, is indistinguishable in the composite from a texel that
// was never painted, and is not malformed. The two conventions are simply
// different, and the invariant each one needs is different with them.
//
// **So this module does NOT clear the latent, and three separate things say
// so.** PRD F10 and ADR-0007 both spell the rule as "leaving the Latent
// untouched". `clearThroughSelection(PigmentTileStore&)` already implements it
// that way for the clear. And `brush/Deposit` §1(ii) was *written for this
// texel*: it defines `m + dm == 0 -> w = 1` precisely so that the next deposit
// onto an erased texel takes the brush's latent outright rather than being
// dragged toward the stale one -- "a stale hue at zero coverage, which PRD
// F10's eraser (mass down, 'leaving the Latent untouched') deliberately
// creates and which the *next* deposit must not be biased by. `w = 1` is the
// answer that erases that bias." The bias is already handled at the one place
// it could bite, so clearing the latent here would be a second, redundant
// answer to a solved problem -- and a lossy one, since there is no latent that
// means "no pigment": `Latent{}` is `c = {0,0,0}`, whose implied fourth weight
// `c3 = 1` is **white**, the exact colour ADR-0007 rejects erasing toward.
//
// `--selftest` asserts the composited consequence rather than the storage
// convention: a texel erased to nothing projects to four exact zeros, and a
// half-erased red is still red at half the mass rather than pink.
//
// ==========================================================================
// 4. Erasing nothing must COST nothing
// ==========================================================================
//
// `brush/RgbErase` §4's asymmetry, on a tile four times the size:
//
//   * **An absent layer tile is skipped whole**, before `getOrCreate()`. A
//     `PigmentTile` is **224 KiB** against `core::Tile`'s 128, so an eraser
//     dragged across blank canvas that allocated per tile crossed would grow
//     the document faster than the RGB one does, and every such tile would then
//     be reported dirty, re-composited and re-uploaded every frame of the drag.
//   * **A texel with `mass == 0` is skipped**, inside the tile. **Mass alone,
//     and not the latent as well** -- which is the deliberate inverse of
//     `brush/RgbErase`'s all-four-channels test, and §3 is the reason: a mass-0
//     texel with a stale latent is not malformed here, it is what this very
//     module leaves behind, so testing the latent too would make every erased
//     texel look occupied and be rewritten (with an unchanged mass 0) by every
//     later dab of the same scrub -- dirtying its tile forever for no change.
//
// The visible consequence, and the one `--selftest` asserts: **a stroke that
// erased nothing records nothing** -- no tiles, no revision bump, no history
// entry (`app/StrokeSession` §2), reached by arithmetic rather than by a
// special case.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the
// boundary `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw,
// for the same reason: this is the arithmetic of removal against a
// `core::PigmentTileStore`, and the stroke lifecycle is `app/StrokeSession`'s.
//
// **No unmixing.** ADR-0007's consequence, stated so the UI does not imply
// otherwise: "Erasing on a Pigment layer that has been mixed is **lossy and
// correct** -- you get less of the mixed colour, never the original components
// back." Mixing is a lerp of latents and lerps are not invertible; nothing here
// pretends otherwise.
//
// **No Media, Strokes or mask erase.** ADR-0007 defines all three and
// `strokeRouteFor()` still refuses all three by name: erasing a Strokes layer
// deletes dab records rather than pixels (PRD F11, a structural edit), erasing
// a Media layer removes the dry deposit and not the film or the saturation
// (which is Blot, PRD F12, P2), and erasing a parametric kind paints its mask.
//
// **No fluid behaviour**, exactly as `brush/Deposit` says of itself. Removing
// mass from a flat CPU deposit is not lifting wet paint with a tissue; that is
// Blot, it is a solver operation, and it is P2.
namespace np {

// §2's rule, as a pure function of one texel, for the one reason a pure
// function earns its keep here: the invariants are about *this arithmetic*, so
// `--selftest` asserts them on this and not on a tile of it.
//
// `dst` is the stored texel; `strokeErase` is `E`, the fraction this stroke has
// already removed here; `weight` is `flow * coverage` with the selection
// already folded in; `strength` is the stroke's ceiling on `E`, which is the
// FLOOR on the mass that survives.
//
// Total: defined for every finite input, including `E >= 1`, `E >= strength`,
// `weight <= 0`, `strength <= 0` and a `dst` at mass 0, each of which returns
// `dst` **bit-identical** with `dabAlpha == 0` -- the caller's signal to skip
// the texel entirely. Bit-identical rather than recomputed for
// `eraseRgbTexel()`'s stated reason: a stroke that has reached its floor must
// stop perturbing the tile it is scrubbing over, or the caller's "nothing to do
// here" test never fires and a 224 KiB tile is re-uploaded on every frame of a
// drag that is changing nothing.
struct PigmentEraseStep {
  PigmentTexel texel{};      // the texel to store
  float strokeErase = 0.0f;  // E', to put back in the accumulator
  float dabAlpha = 0.0f;     // e; 0 means "nothing to do here"
};
PigmentEraseStep erasePigmentTexel(const PigmentTexel& dst, float strokeErase, float weight,
                                   float strength) noexcept;

// One Pigment erase stroke in flight: the latched strength, and the accumulator
// that makes it a per-stroke floor rather than a per-dab multiplier.
//
// **The accumulator is `StrokeAlphaStore`, borrowed from brush/RgbDeposit and
// not re-declared**, exactly as `RgbEraseStroke` borrows it and for a reason
// that is stronger here rather than weaker: `E` is a dimensionless fraction in
// [0,1], so it is not the RGB layer's alpha in the first place and there is
// nothing about it that belongs to `core::Tile`. A `PigmentStrokeMassTile` that
// stored the same float under a pigment-sounding name would be 64 KiB of
// identical code, and `brush/RgbDeposit` §3's derivation of why it is float and
// not half (`N * 2^-11` of drift on the ceiling over a stroke of N dabs) would
// then exist in two places to fall out of step.
class PigmentEraseStroke {
 public:
  // Pen-down. Latches the strength and clears any accumulator a previous stroke
  // left -- erasure carried across strokes would let the floor of the last one
  // stop the first dab of the next, so a second pass would refuse to cut
  // deeper, which is the one thing §2 says a second pass must do.
  //
  // `strength` is clamped to [0,1]; a non-positive one leaves a stroke that
  // removes nothing, which is a legitimate setting and not an error.
  void begin(float strength) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator and leaves `strength()` alone, so the counts
  // below still read correctly after a stroke ends.
  void end() noexcept;

  // Erases one dab from `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, §2).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to fold
  // with `sortUniqueTiles()`.
  //
  // A dab that lands on blank canvas, or all of whose texels have reached the
  // floor, writes nothing, allocates nothing and reports no tiles (§4).
  DepositCount eraseDab(PigmentTileStore& store, const BrushTip& tip, Vec2 centre,
                        int32_t canvasW, int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

  // Erases every dab in `dabs`, in order. Order matters for the same mechanism
  // it does in either deposit: `E` is a running accumulation, so a dab's
  // contribution depends on what the dabs before it left.
  StrokeDeposit eraseDabs(PigmentTileStore& store, const BrushTip& tip,
                          const std::vector<Vec2>& dabs, int32_t canvasW, int32_t canvasH,
                          const Selection* selection);

  // The fraction this stroke has removed at a document texel so far -- 0 for a
  // texel it has not reached. The accumulator's read side, exposed because it is
  // what `--selftest` asserts the floor against at zero tolerance: the stored
  // mass has been through binary16 once per dab and the accumulator has not.
  float strokeEraseAt(PixelCoord doc) const noexcept;

  float strength() const noexcept { return strength_; }

  // What the accumulator currently holds. `--selftest` prints both, because the
  // memory claim ("allocated at pen-down, freed at pen-up") is worth checking
  // rather than trusting -- and 64 KiB of accumulator against a 224 KiB tile is
  // a ratio worth seeing on this route in particular.
  size_t accumulatorTiles() const noexcept { return erased_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return erased_.tileBytes(); }

 private:
  float strength_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore erased_;
};

}  // namespace np
