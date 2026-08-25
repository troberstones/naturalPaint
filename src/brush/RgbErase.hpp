#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/SelectionMask.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// brush/RgbErase -- **taking paint off a plain RGB layer.**
//
// ==========================================================================
// 0. What this is, and why it is a sibling of brush/RgbDeposit rather than a
//    flag on it
// ==========================================================================
//
// `brush/RgbDeposit` §5 lists what that module deliberately does not do, and
// the first item on the list is "no eraser". This is that item, built beside it
// rather than inside it, and the reason is not tidiness: the two modules share
// their dab stream, their falloff, their footprint, their tile-major loop and
// their accumulator *type*, and differ in the one place that matters -- what
// one dab does to one texel, and what the per-stroke accumulator counts. A
// `bool erasing` parameter threaded through `depositRgbTexel()` would put both
// arithmetics inside one function whose §2 argument is written about only one
// of them, and the invariant that argument protects (a stroke's ceiling is
// exact rather than approximate) would then be stated in a place where half the
// calls do not obey it.
//
// **Almost nothing here is new.** `dabCoverage()`, `dabPixelBounds()`,
// `BrushTip::spacingPx()`, `StrokeAlphaStore` and the hoisted tile loop with
// its lazily-fetched write handle are all borrowed unchanged, including the
// reasons -- `brush/Deposit` §3 on reporting a tile in the same branch that
// writes it, `brush/RgbDeposit` §3 on why the accumulator is float and not
// half, `core/SelectionMask.hpp` on a hoisted loop owning its own null branch.
// Exactly two things are genuinely different, and they are §§1-2 below.
//
// **The design this implements, and the part of it this does not.**
// ADR-0007 ("Erase is mass reduction, not a colour") and PRD F9/F10 (both
// **P0**) say the eraser is the brush with a negative deposit step, inheriting
// the whole dynamics matrix, and that what it removes depends on the layer
// kind: **alpha on RGB**, Mass on Pigment with the Latent left alone, deposit on
// Media, dab records on Strokes, the mask on the parametric kinds. This module
// is the RGB row and only the RGB row. **The Pigment row is
// `brush/PigmentErase`**, a sibling of this file rather than a flag on it -- it
// landed once `brush/Deposit` §4 gave the pigment route PRD E1's selection
// gate, which is the single blocker this header used to record. The remaining
// rows are refusals in `strokeRouteFor()` rather than silent no-ops, and that
// header's §1 table is where they are recorded.
//
// **Never implement erase as painting the background colour.** MacPaint's
// model, still Photoshop's on a locked Background layer, and ADR-0007 rejects
// it in those words: on a layer with real alpha it is wrong, and on a Pigment
// layer it is wrong twice, because white under Kubelka-Munk is opaque paint
// rather than the absence of paint. Nothing in this file has a colour in it at
// all -- the ink on the tip is not read, which is the mechanical form of that
// decision.
//
// ==========================================================================
// 1. The arithmetic: destination-out, on ALL FOUR channels
// ==========================================================================
//
// `core::Tile` stores premultiplied ("associated") alpha -- core/TileStore.hpp
// says so where the type is defined, and `core/Composite` reads a texel with
// `readPixel()` and blends it with no un-premultiply anywhere. Erasing is
// therefore a destination-out composite with an opaque source:
//
//     dst' = dst * (1 - e)                  // rgb AND alpha, one factor
//
// **The claim that this is the whole of it was checked against
// `ops/FloodFill.cpp` rather than assumed, and it holds.**
// `fillThroughSelection()` blends its four channels with the *same* `keep`
// factor and says why: it "makes a partially-covered texel half *present*
// rather than half *bright* -- the same associated-alpha argument
// `clearThroughSelection()` makes in the other direction, and the reason a
// feathered fill has no fringe". That is the identical argument in the identical
// storage convention, and this module agrees with it: a premultiplied texel is
// `(colour * a, a)`, so scaling all four by `1 - e` gives
// `(colour * a(1-e), a(1-e))` -- still exactly `colour` at the new alpha. The
// colour never moves, only how much of it is there.
//
// Two mistakes this rules out, both of which look like a colour-management bug
// somewhere else entirely rather than like an eraser:
//
//   * **Scaling alpha alone.** The texel becomes `(colour * a, a(1-e))`, which
//     un-premultiplies to `colour / (1-e)` -- a half-erased red rim reads back
//     as a *brighter* red than the paint it came from, and at `e -> 1` it
//     diverges. This is the fringe, and it appears on exactly the soft edges an
//     eraser is used for.
//   * **Erasing toward the background colour** (scaling RGB toward white or
//     toward the canvas). That is ADR-0007's rejected model, and on a layer it
//     leaves opaque white where the user asked for nothing.
//
// **A texel at alpha 0 must hold RGB 0 as well.** It falls out of the shared
// factor -- `e == 1` multiplies all four channels by exactly zero, and `0.0f *
// finite` is exactly `0.0f` -- so this module cannot produce the malformed
// texel `(colour, 0)` that `core/Composite` would read as an additive glow
// contributing colour with no coverage. `--selftest` asserts all four channels
// at exactly zero rather than only the alpha, because that is the one form of
// the mistake a picture does not show until the layer is composited over
// something.
//
// ==========================================================================
// 2. The per-stroke FLOOR, which is this module's whole argument
// ==========================================================================
//
// `brush/RgbDeposit` §2 gives a stroke an opacity **ceiling**: an accumulator
// `A` remembers how much alpha this stroke has laid at this texel, so a stroke
// at 50 % reaches 50 % and stops however long it is scrubbed, instead of
// compounding 0.5 -> 0.75 -> 0.875 -> 1 over its own overlaps. Every word of
// that argument applies here with the sign flipped: an eraser at 50 % strength
// must take a texel to **half the alpha it had** and stop, however long the user
// scrubs. Without it "strength" is a second flow slider with a different name,
// two crossing erase strokes cut deeper than either, and a slowly-drawn erase
// removes more than a quickly-drawn one over the identical path -- which is
// ADR-0003's speed dependence arriving through the back door.
//
// **The mirror is not the obvious one, and this is the part to get right.**
// The brush accumulates toward 1 from below; the eraser drives toward 0 from
// above. Mirroring the *headroom* term directly -- keeping the accumulator in
// the texel's own alpha and replacing `1 - A` with `A` --
//
//     A' = max(floor, A - w * A)                      // REJECTED
//
// fails on the question it makes you ask next: what is `floor`?
//
//   * `floor = 1 - strength`, an absolute alpha, means a 50 %-strength eraser
//     **does nothing at all** to a texel that is already thinner than 0.5.
//     Dragging it over a faint wash leaves the wash untouched; the tool reads as
//     broken, and the user's only diagnosis is "the eraser stopped working".
//   * `floor = A0 * (1 - strength)`, the answer that is actually meant, needs
//     `A0` -- the alpha at pen-down -- remembered per texel. That is a second
//     per-texel store holding a *copy of the layer*, correct only for as long as
//     nothing else writes the layer during the stroke, and undefined on a texel
//     whose four channels disagree about what "its alpha" is.
//
// **Accumulate the fraction removed, not the alpha.** Repeated destination-out
// composes in the retained fraction, not in the alpha: after dabs `e_1..e_N` the
// texel holds `dst0 * prod(1 - e_i)`. So define the stroke's **erasure** `E` by
// `1 - E = prod(1 - e_i)` -- "the fraction of whatever was here that this stroke
// has taken away". `E` starts at 0 at pen-down for every texel whatever its
// alpha, is remembered across dabs, and is thrown away at pen-up. Per dab, with
// `w = flow * coverage` (coverage already gated by the selection, §3) and
// `strength` the stroke's ceiling on `E`:
//
//     E' = min(strength, E + w * (1 - E))    // accumulate, capped at strength
//     e  = (E' - E) / (1 - E)                // this dab's destination-out alpha
//     dst' = dst * (1 - e)                   // premultiplied, all four channels
//
// which is `brush/RgbDeposit` §2 line for line on a different quantity -- and it
// is the *same identity*, not an analogy: `1 - E' = (1 - E)(1 - e)` rearranges
// to the second line exactly, because both a source-over and a
// destination-out compose multiplicatively in transparency. So the closed form
// of a whole stroke, at any number of dabs and any dab order, is
//
//     alpha_final = alpha_0 * (1 - strength)          // the FLOOR
//
// and `alpha_0` never has to be stored, because the multiplication *is* the
// memory. That is the sense in which the headroom is not mirrored: the brush's
// headroom `1 - A` is the room left to *fill*, the eraser's `1 - E` is the room
// left to *remove*, and only the second is a pure ratio -- which is exactly why
// it is the one that works on a texel that starts partially transparent.
//
// **The rejected per-dab model, for the record.** `e = flow * cov * strength`
// per dab with no memory of the ones before is the version that paints, and
// looks like erasing. At the default 0.25-radius spacing a dab overlaps its
// neighbours about four deep, so the retained fraction is
// `(1 - flow*cov*strength)^N` and it goes to zero: a "50 % eraser" scrubbed 50
// times over one spot removes 99.99 % of the paint. `--selftest` computes that
// number on the identical inputs and asserts it is wrong, so the good assertion
// cannot pass against the bad implementation.
//
// Four limits, each a real input rather than a defensive clause:
//
//   * `E -> 1`. The divisor is `1 - E`. At `E == 1` the texel has been erased
//     completely, there is nothing left to remove, and the dab is skipped --
//     which is the correct answer *and* keeps the division out of the singular
//     case. A stroke at strength 1 reaches it in ordinary use.
//   * `E >= strength`. Also skipped, and this is the floor doing its job: every
//     dab after it is reached writes nothing at all, so a scrubbed erase stops
//     dirtying tiles and live feedback stops re-uploading them.
//   * `flow > 1`. Deliberately not clamped, for `brush/Deposit`'s stated reason
//     ("a flow above 1 is a legitimate one dab saturates the paper tip"): the
//     `min` already caps `E'`, so a flow of 2.5 means one dab reaches the floor.
//     `strength` *is* clamped to [0,1], because removing 140 % of a texel is not
//     a meaning any compositor has.
//   * **The destination is already empty.** Nothing to remove, so nothing is
//     written -- see §4, which is where that turns from arithmetic into the
//     difference between an eraser that costs nothing and one that allocates the
//     canvas.
//
// **Strength is `BrushTip::opacity`, and that is deliberate rather than
// convenient.** It is the same slider, latched at pen-down by the same
// `StrokeSession::begin()`, meaning the same thing in the same units: the
// fraction of the maximum effect one stroke may reach. Giving the eraser a
// second "strength" number on `BrushState` would leave the OPACITY control in
// the brush panel doing nothing while the eraser was selected, which is the
// failure the panel's own caption ("Drawn disabled rather than hidden on the
// routes that ignore it") exists to prevent.
//
// ==========================================================================
// 3. The selection bounds the erase, exactly twice (PRD E1, P0)
// ==========================================================================
//
// "Every deposit and every op respects the active selection." An eraser that
// ignored it would be the one tool in the build that cut through the marching
// ants, and it would be the *worst* one to have that defect, because what it
// destroys outside the selection is not visible until the layer under it is.
//
// `brush/RgbDeposit` §4 found by measurement that gating only the rate is not a
// bound, and the identical argument applies here. Per texel, with `s` the
// selection's coverage:
//
//     w   = flow * cov * s            // one dab removes `s` of what it would
//     cap = strength * s              // and no number of dabs goes past `s`
//
// With the first alone, `E' = E + w(1 - E)` still converges to `strength` for
// *any* positive `w` -- it just takes longer. A half-selected texel would erase
// fully; it would merely need more scrubbing, so a feathered selection edge
// would come out hard for a slow stroke and soft for a fast one. That is the
// speed dependence §2 exists to remove, wearing a different hat. With both, a
// half-selected texel keeps at least half of its paint against any amount of
// scrubbing *within one stroke*, and the floor there is
// `alpha_0 * (1 - strength * s)`.
//
// It does **not** survive repeated separate strokes -- each new stroke starts
// its accumulator at 0 and multiplies into what the last one left, so two passes
// of a strength-1 eraser through a half-selected texel leave 0.5 then 0.25 of
// the original. That is what every editor does, it is what the deposit route
// does in the other direction, and making it otherwise would need the selection
// to be a mask on the *layer* rather than a bound on the stroke.
//
// `nullptr` means "no restriction" and 1.0 everywhere, which is
// core/SelectionMask.hpp's convention and NOT the inverse (a caller who writes
// `sel ? cov : 0` has inverted the editor). That header requires every hoisted
// per-texel loop to own its own copy of the null branch, and warns that a
// perturbation inverting one copy leaves the others right; this is one such
// loop, `--selftest` drives both nulls through it, and an engaged selection with
// no tile at a coordinate skips that tile before anything is looked up.
//
// ==========================================================================
// 4. Erasing nothing must COST nothing -- the asymmetry with the deposit
// ==========================================================================
//
// `ops/FloodFill.cpp` states this asymmetry for its own pair and it is exactly
// the one here: "A clear can only remove paint, so a tile that does not exist
// has nothing to lose and is skipped. A fill *adds* paint, so the tiles it must
// touch are the ones the selection names, most of which may not exist yet."
// `core/Channels.cpp` makes the same point in the same words about the same
// gesture -- "an eraser dragged across unpainted space is exactly that" -- and
// draws the line between "an eraser costing nothing and an eraser allocating the
// canvas".
//
// So this module skips at two granularities, and both are load-bearing:
//
//   * **An absent layer tile is skipped whole**, before `getOrCreate()`. A
//     224 KiB tile allocated per tile of blank canvas the eraser passes over
//     would be a tool that *grows* the document by being used on nothing, and
//     every one of those tiles would then be reported dirty, re-composited and
//     re-uploaded every frame of the drag.
//   * **A texel whose four stored channels are all exactly zero is skipped**,
//     inside the tile. Testing all four rather than the alpha alone is the
//     deliberate half of this: a texel with alpha 0 and non-zero RGB is
//     malformed (§1), and this function must not be the thing that quietly
//     declares it absent -- it holds something, so the eraser scales it like
//     anything else and it is gone for the right reason.
//
// The visible consequence, and the one `--selftest` asserts: **a stroke that
// erased nothing records nothing.** No tiles reported, no revision bump, no
// history entry -- `app/StrokeSession` §2's rule reached by arithmetic rather
// than by a special case, because an undo step that undoes nothing is a worse
// defect than a missing one.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the same
// boundary `brush/Deposit` and `brush/RgbDeposit` both draw, for the same
// reason: this is the arithmetic of removal against a `core::TileStore`, and the
// stroke lifecycle belongs to `app/StrokeSession`.
//
// **No Pigment, Media, Strokes or mask erase.** ADR-0007 defines all four.
// **Pigment is built, in `brush/PigmentErase`**, and it is not simply this
// arithmetic applied to `pig.m`: the accumulator and the floor carry across
// unchanged (its §2), but a Pigment texel is straight rather than premultiplied,
// so §4's all-four-channels emptiness test becomes a mass-only one and §1's
// "alpha 0 with colour is malformed" rule does not hold there at all (its §3).
// The other three are still refusals in `strokeRouteFor()`, whose §1 carries the
// argument for each row.
//
// **No colour at all.** `BrushTip::linearRgb` and `BrushTip::pigment` are not
// read here. An eraser that reached for either would be a brush painting the
// background, which is the model ADR-0007 exists to reject.
//
// **No blend mode.** `Layer::blend` still applies to the layer as a whole at
// composite time and is untouched. Erasing a layer whose blend is Multiply
// removes its paint; it does not remove the multiply.
namespace np {

// §2's rule, as a pure function of one texel, for the one reason a pure
// function earns its keep here: the invariants are about *this arithmetic*, so
// `--selftest` asserts them on this and not on a tile of it.
//
// `dst` is the stored PREMULTIPLIED texel; `strokeErase` is `E`, the fraction
// this stroke has already removed here; `weight` is `flow * coverage` with the
// selection already folded in; `strength` is the stroke's ceiling on `E`, which
// is the FLOOR on what survives.
//
// Total: defined for every finite input, including `E >= 1`, `E >= strength`,
// `weight <= 0`, `strength <= 0` and an all-zero `dst`, each of which returns
// `dst` **bit-identical** with `dabAlpha == 0` -- the caller's signal to skip
// the texel entirely rather than write a value equal to the one already there.
// Bit-identical rather than recomputed for `depositRgbTexel()`'s stated reason:
// a stroke that has reached its floor must stop perturbing the tile it is
// scrubbing over, or the caller's "nothing to do here" test never fires and the
// tile is re-uploaded on every frame of a drag that is changing nothing.
struct RgbEraseStep {
  std::array<float, 4> premultiplied{};  // the texel to store
  float strokeErase = 0.0f;              // E', to put back in the accumulator
  float dabAlpha = 0.0f;                 // e; 0 means "nothing to do here"
};
RgbEraseStep eraseRgbTexel(const std::array<float, 4>& dst, float strokeErase, float weight,
                           float strength) noexcept;

// One erase stroke in flight: the latched strength, and the accumulator that
// makes it a per-stroke floor rather than a per-dab multiplier.
//
// Deliberately a small object with an explicit lifetime rather than a free
// function taking a `StrokeAlphaStore&`, for `RgbStroke`'s stated reason: the
// accumulator is only correct against the strength it was started with (§2), so
// binding the two together at `begin()` makes the one combination that can go
// wrong -- accumulator from one stroke, strength from another -- unspellable.
//
// **The accumulator is `StrokeAlphaStore`, borrowed from brush/RgbDeposit and
// not re-declared.** It is the right shape (sparse, allocate-on-write,
// query-without-allocating, keyed by the same `TileCoord` as the layer's tiles)
// and the right precision (float, not half -- that header's §3 derives the
// `N * 2^-11` drift a half accumulator would put on the ceiling, and the drift
// lands on the floor here for the same reason). What differs is only what the
// number *means*: `A` is alpha this stroke has added, `E` is the fraction this
// stroke has removed. Both start at 0, both are capped, both are dimensionless
// in [0,1] -- so the storage is genuinely the same thing and a second 64 KiB
// tile type would be a copy with a different comment on it.
class RgbEraseStroke {
 public:
  // Pen-down. Latches the strength and clears any accumulator a previous stroke
  // left, exactly as `StrokePath::reset()` clears leftover arc length and for
  // the same reason: erasure carried across strokes would let the floor of the
  // last stroke stop the first dab of the next -- so a second pass would refuse
  // to cut deeper, which is the one thing §2 says a second pass must do.
  //
  // `strength` is clamped to [0,1]; a non-positive one leaves a stroke that
  // removes nothing, which is a legitimate setting and not an error.
  void begin(float strength) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator and leaves `strength()` alone, so the counts
  // below still read correctly after a stroke ends.
  void end() noexcept;

  // Erases one dab from `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, §3).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to fold
  // with `sortUniqueTiles()`.
  //
  // A dab that lands on blank canvas, or all of whose texels have reached the
  // floor, writes nothing, allocates nothing and reports no tiles (§4).
  DepositCount eraseDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                        int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

  // Erases every dab in `dabs`, in order. Order matters for the same mechanism
  // it does in the deposit: `E` is a running accumulation, so a dab's
  // contribution depends on what the dabs before it left.
  StrokeDeposit eraseDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                          int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The fraction this stroke has removed at a document texel so far -- 0 for a
  // texel it has not reached. The accumulator's read side, exposed because it is
  // what `--selftest` asserts the floor against at zero tolerance: the stored
  // texel has been through binary16 once per dab and the accumulator has not.
  float strokeEraseAt(PixelCoord doc) const noexcept;

  float strength() const noexcept { return strength_; }

  // What the accumulator currently holds. `--selftest` prints both, because the
  // memory claim ("allocated at pen-down, freed at pen-up") is worth checking
  // rather than trusting.
  size_t accumulatorTiles() const noexcept { return erased_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return erased_.tileBytes(); }

 private:
  float strength_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore erased_;
};

}  // namespace np
