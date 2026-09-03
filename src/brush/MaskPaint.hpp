#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "core/Mask.hpp"
#include "core/SelectionMask.hpp"

// brush/MaskPaint -- **painting a layer mask.**
//
// ==========================================================================
// 0. What this closes
// ==========================================================================
//
// `core/Mask.hpp` and `core/Layer.hpp` both said, in the same words, that
// **nothing in this build could paint a mask**: "the content of a mask can
// only come from a `.npaint` or from a test writing texels". A mask store, a
// compositor that reads it and a panel that says `MASK` all existed; the one
// missing piece was a stroke route. This is that route, and `docs/
// testing-issues.md` T16 is where the gap was reported.
//
// ADR-0007's own table already names this row -- it lists what an eraser
// removes per layer kind and ends with "the mask on the parametric kinds" --
// and `brush/RgbErase.hpp` §5 records "no mask erase" as one of the three rows
// still refused in `strokeRouteFor()`. This is that row arriving from the
// *paint* side rather than the erase side, and §1 is why it is one route and
// not two.
//
// ==========================================================================
// 1. One route, not a deposit and an erase, because a mask has no zero
// ==========================================================================
//
// `brush/RgbDeposit` and `brush/RgbErase` are two modules because the two
// arithmetics genuinely differ: paint *adds* alpha toward 1 and erase *scales*
// it toward 0, and a texel's "empty" is four zero words in both.
//
// **A mask has no such asymmetry.** Every sample is a coverage in [0,1] with
// no privileged end -- `core/Mask.hpp` is explicit that 1.0 is the identity of
// the multiply and 0.0 is real content costing 32 KiB per tile, so "hiding"
// and "revealing" are two directions of one operation and neither is removal.
// One route that moves a texel **toward a target coverage** is therefore not a
// merged pair; it is the only shape the quantity has. Painting black hides,
// painting white reveals, painting grey lands in between, and all three are
// the same line of arithmetic -- which is exactly how a mask is painted in
// every editor that has one.
//
// The consequence for `strokeRouteFor()`: the **Brush** takes this route and
// the eraser does not. An eraser aimed at a mask has no meaning that is not
// already spelled "paint white", and inventing one would be a second control
// over the same number. `app/StrokeSession.hpp` §1 records that row and the
// tools still refused.
//
// ==========================================================================
// 2. Where the target coverage comes from, and why it is encoded
// ==========================================================================
//
// The brush carries a colour, and a mask carries a coverage, so something has
// to convert one into the other. `maskTargetForInk()` below does it, and its
// two halves are both decisions already made elsewhere in this codebase rather
// than new ones:
//
//   * **Rec.709 luma in LINEAR light, then sRGB-encode the scalar.**
//     `core/SelectionRefine.hpp`'s luminance range states this order and
//     argues it: weights applied to display-encoded values are not an
//     approximation of luminance, they are the different quantity Y'; and
//     `ops/PointOps`' `computeLuma()` with `kRec709LumaWeights` is the one
//     luma this codebase has, so a fourth copy of those three literals would
//     be a fourth thing to keep in step.
//   * **The encode is what makes the picker honest.** A mask sample *is* an
//     opacity, so it is not gamma-encoded (ui/DabPicker.hpp §2) -- but the
//     grey the user chose in the colour picker is a *display* grey, and 50 %
//     grey means "let half of this through". Encoding the linear luma turns
//     that display grey back into the number it looked like: 50 % grey gives
//     coverage 0.500, not the 0.214 the raw linear value would give. Skipping
//     the encode would make every mid-grey stroke about twice as opaque as the
//     swatch it was painted with, which is a colour-management bug wearing an
//     opacity's clothes.
//
// Black gives exactly 0 and white exactly 1 -- both ends fix, which is why no
// black-and-white test could ever have caught the missing encode either. That
// is `app/selftest/PresentTransfer.cpp`'s lesson applied one module over, and
// it is why `--selftest` asserts the **mid**-grey number.
//
// ==========================================================================
// 3. The per-stroke ceiling, borrowed whole from brush/RgbDeposit §2
// ==========================================================================
//
// An accumulator `A` remembers what FRACTION of the way from this stroke's
// starting coverage to its target this texel has already been moved, so a
// stroke at 50 % opacity moves half way and stops however long it is scrubbed,
// instead of compounding 0.5 -> 0.75 -> 0.875 -> 1 over its own overlaps.
// Every word of `brush/RgbDeposit.hpp` §2's argument transfers, including the
// reason the accumulator is `float` and not half (§3 there derives the
// `N * 2^-11` drift a half accumulator would put on the ceiling).
//
// The arithmetic, per dab, with `w` the dab's weight and `cap` the ceiling:
//
//     A' = min(cap, A + w(1 - A))
//     f  = (A' - A) / (1 - A)          // this dab's lerp factor
//     v' = v + f(target - v)
//
// which telescopes -- by induction on the dabs -- to exactly
//
//     v_end = v_start + A_end (target - v_start)
//
// so a stroke's whole effect at a texel is one lerp by the accumulator, and
// that closed form is what `--selftest` asserts at zero tolerance. It is the
// assertion that catches a per-dab multiplier masquerading as a ceiling: with
// `f = w` alone, `v` converges to `target` for any positive `w`, so a slow
// stroke would end up more opaque than a fast one over the identical path,
// which is ADR-0003's speed dependence arriving through the back door.
//
// ==========================================================================
// 4. The selection enters TWICE, exactly as it does on every other route
// ==========================================================================
//
// PRD E1 (P0), and `brush/RgbErase.hpp` §3 derives why one is not enough:
// gating only the rate is a speed limit rather than a bound, because
// `A' = A + w(1 - A)` still converges to the ceiling for any positive `w`. So
// the selection coverage `s` enters the weight (one pass moves `s` of what it
// would) **and** the ceiling (no number of passes goes past `s * opacity`).
//
// `nullptr` means "no restriction" and 1.0 everywhere -- `core/
// SelectionMask.hpp`'s convention, and NOT the inverse. That header requires
// every hoisted per-texel loop to own its own copy of the null branch and
// warns that a perturbation inverting one copy leaves the others right; this
// is one such loop and `--selftest` drives both nulls through it.
//
// ==========================================================================
// 5. The asymmetry with the eraser: an ABSENT tile is not empty here
// ==========================================================================
//
// **This is the one place where copying `brush/RgbErase`'s structure would
// have been wrong, and it is worth stating loudly because it fails silently.**
// That module skips an absent tile whole, before `getOrCreate()`, on the
// argument `ops/FloodFill.cpp` makes: "a clear can only remove paint, so a
// tile that does not exist has nothing to lose".
//
// A mask tile that does not exist **means 1.0** (core/Mask.hpp: "an
// unallocated mask tile therefore means 1.0, not 0.0"), so it holds the most
// content a mask texel can hold. Painting black onto blank mask *is* the
// common case -- `core::addLayerMask()` creates a store with zero tiles, so
// every mask starts that way -- and a route that skipped absent tiles would be
// a mask brush that did nothing at all on every mask the application can
// create. It would look exactly like a missing route, which is the defect it
// was built to close.
//
// So the skip here is a different test, and it is the honest one: **a texel
// already at the value this dab would leave it at is skipped**, whether that
// value came from a tile or from the absent-means-1.0 rule. Painting white on
// an untouched mask therefore still costs nothing and allocates nothing --
// the cheap case survives -- but it survives for the true reason rather than
// by accident of storage.
//
// ==========================================================================
// 6. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the same
// boundary `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw.
// This is the arithmetic of coverage against a `core::MaskTileStore`; the
// stroke lifecycle, and the decision that a mask is the edit target at all,
// belong to `app/StrokeSession`.
//
// **No feather, no density, no invert, no "apply mask", no selection <-> mask
// conversion (PRD E5), no Hide All.** `core/Mask.hpp` lists all six as mask
// *editing* rather than mask *painting*, and none of them is a stroke.
//
// **No smudge, clone, dodge or burn on a mask.** Each is a genuine question
// about what that operation means on a scalar coverage field, and each would
// need its own answer; `strokeRouteFor()` refuses them by name rather than
// routing them somewhere plausible.
namespace np {

// §2. The coverage a stroke carrying `linearRgb` paints toward: Rec.709 luma
// in linear light, sRGB-encoded, clamped to [0,1].
//
// Exact at both ends -- black is 0, white is 1 -- which is precisely why the
// assertion that matters is the mid-grey one.
float maskTargetForInk(const std::array<float, 3>& linearRgb) noexcept;

// §3's rule, as a pure function of one texel, for the reason `eraseRgbTexel()`
// is one: the invariants are about *this arithmetic*, so `--selftest` asserts
// them on this rather than on a tile of it.
//
// `dst` is the stored coverage (or 1.0 for an absent tile, §5); `strokeApplied`
// is `A`; `weight` is `flow * coverage` with the selection already folded in;
// `ceiling` is the stroke's cap on `A`, which is `opacity * s`.
//
// Total: defined for every finite input, including `A >= 1`, `A >= ceiling`,
// `weight <= 0`, `ceiling <= 0` and a `dst` already at the target -- each of
// which returns `dst` **bit-identical** with `changed == false`, the caller's
// signal to skip the texel rather than write a value equal to the one already
// there. Bit-identity rather than a recomputed equal value, for
// `depositRgbTexel()`'s stated reason: a stroke that has reached its ceiling
// must stop perturbing the tile it is scrubbing over, or the caller's "nothing
// to do here" test never fires and the tile is re-uploaded on every frame of a
// drag that is changing nothing.
struct MaskPaintStep {
  float coverage = 1.0f;      // the value to store
  float strokeApplied = 0.0f;  // A', to put back in the accumulator
  bool changed = false;        // false means "nothing to do here"
};
MaskPaintStep paintMaskTexel(float dst, float strokeApplied, float weight, float ceiling,
                             float target) noexcept;

// One mask stroke in flight: the latched target and ceiling, and the
// accumulator that makes the ceiling a per-stroke bound rather than a per-dab
// multiplier.
//
// A small object with an explicit lifetime rather than a free function taking a
// `StrokeAlphaStore&`, for `RgbStroke`'s and `RgbEraseStroke`'s stated reason:
// the accumulator is only correct against the target and ceiling it was started
// with (§3), so binding the three together at `begin()` makes the one
// combination that can go wrong -- accumulator from one stroke, target from
// another -- unspellable.
//
// **The accumulator is `StrokeAlphaStore`, borrowed from brush/RgbDeposit and
// not re-declared**, exactly as `brush/RgbErase` borrows it and for the same
// reason: it is the right shape (sparse, allocate-on-write,
// query-without-allocating, keyed by the same `TileCoord`) and the right
// precision. What differs is only what the number means -- `A` here is the
// fraction of the way to the target this stroke has travelled.
class MaskPaintStroke {
 public:
  // Pen-down. Latches the target coverage and the ceiling and clears any
  // accumulator a previous stroke left, exactly as `StrokePath::reset()` clears
  // leftover arc length and for the same reason: a fraction carried across
  // strokes would let the ceiling of the last stroke stop the first dab of the
  // next, so a second pass would refuse to go deeper -- which is the one thing
  // §3 says a second pass must do.
  //
  // Both arguments are clamped to [0,1]; a non-positive `ceiling` leaves a
  // stroke that changes nothing, which is a legitimate setting and not an
  // error.
  void begin(float target, float ceiling) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator and leaves `target()`/`ceiling()` alone, so
  // the counts below still read correctly after a stroke ends.
  void end() noexcept;

  // Paints one dab into `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, §4).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to
  // fold with `sortUniqueTiles()`.
  //
  // A dab every texel of which is already at the value it would be left at
  // writes nothing, allocates nothing and reports no tiles (§5).
  DepositCount paintDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                        int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

  // Paints every dab in `dabs`, in order. Order matters for the same mechanism
  // it does in the deposit: `A` is a running accumulation, so a dab's
  // contribution depends on what the dabs before it left.
  StrokeDeposit paintDabs(MaskTileStore& store, const BrushTip& tip,
                          const std::vector<Vec2>& dabs, int32_t canvasW, int32_t canvasH,
                          const Selection* selection);

  // The fraction of the way to the target this stroke has travelled at a
  // document texel -- 0 for a texel it has not reached. The accumulator's read
  // side, exposed because it is what `--selftest` asserts §3's closed form
  // against at zero tolerance: the stored coverage has been through binary16
  // once per dab and the accumulator has not.
  float strokeAppliedAt(PixelCoord doc) const noexcept;

  float target() const noexcept { return target_; }
  float ceiling() const noexcept { return ceiling_; }

  // What the accumulator currently holds. `--selftest` prints both, because the
  // memory claim ("allocated at pen-down, freed at pen-up") is worth checking
  // rather than trusting.
  size_t accumulatorTiles() const noexcept { return applied_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return applied_.tileBytes(); }

 private:
  float target_ = 0.0f;
  float ceiling_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore applied_;
};

}  // namespace np
