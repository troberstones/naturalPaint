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

// brush/CloneStamp -- **painting with texels read from somewhere else on the
// same layer.**
//
// ==========================================================================
// 0. What this is, and why it is a third sibling of brush/RgbDeposit rather
//    than a source mode on it
// ==========================================================================
//
// A clone stamp is the brush with its ink replaced by *a second read of the
// layer*: every texel under the tip takes the colour standing at a fixed
// vector away from it. Everything else about a stroke -- the dab stream, the
// falloff, the footprint, the paper grain, the tile-major loop with its lazily
// fetched write handle, the per-stroke opacity ceiling and its `StrokeAlphaStore`
// -- is `brush/RgbDeposit`'s, unchanged, including the reasons. Two things are
// genuinely different, and they are §§1-2 below.
//
// **A `bool cloning` on `RgbStroke` was rejected**, for the reason
// `brush/RgbErase` §0 gives about its own pair one level down: the two modules
// differ in what one dab does to one texel, and `RgbStroke`'s ink is a *single
// latched colour* whose invariant (§2 of that header: the accumulator is only
// exact against a colour that holds still for the whole stroke) is stated about
// a constant. Here the "ink" is a different value at every texel and is read
// out of a store that is being written in the same pass -- a hazard that header
// has no sentence about, because it cannot happen there. Threading a flag
// through `depositRgbTexel()` would put both arithmetics inside one function
// whose argument describes only one of them.
//
// **This is the RGB row and only the RGB row.** A Pigment layer refuses in
// `strokeRouteFor()` rather than silently doing nothing, and `app/StrokeSession`
// §1's table carries the argument: partial coverage on a Pigment texel is a
// Kubelka-Munk mixture of two latents weighted by mass (`depositTexel()`), not a
// lerp of four premultiplied channels, so the soft edge of a cloned dab means
// something different there. That is a second module the way `brush/PigmentErase`
// is, not a flag on this one.
//
// ==========================================================================
// 1. The arithmetic: source-over of a texel that carries its OWN alpha
// ==========================================================================
//
// `core::Tile` stores premultiplied ("associated") alpha, so both the
// destination and the source are `(colour * a, a)`. `brush/RgbDeposit` §1
// composites an *opaque* source scaled by the dab's alpha `a`; the source here
// is whatever was standing at the sampled coordinate, and it may be transparent,
// half covered or opaque. Scaling a premultiplied texel by `a` scales its
// coverage with its colour, which is exactly what "half of this paint" has to
// mean, so the source-over is:
//
//     S    = src * a                       // premultiplied, alpha = src[3] * a
//     dst' = S + dst * (1 - src[3] * a)
//
// One factor `1 - src[3] * a`, on all four channels, for the reason
// `fillThroughSelection()` and `brush/RgbErase` §1 both state about their own
// composites: it makes a partially covered texel half *present* rather than half
// *bright*, and it is why a cloned soft edge has no fringe.
//
// **`src[3]` and not `1` in the keep factor is the whole of this line.** Using
// `brush/RgbDeposit`'s `keep = 1 - a` with a *transparent* source would erase the
// destination -- dragging a clone stamp whose source sits over blank canvas would
// cut a hole in the paint under the tip, which is a tool doing the opposite of
// its name, invisibly, and only over the parts of the source that happen to be
// empty. It is also why the clone is not "the deposit with a per-texel colour":
// the colour and the coverage both come from the source, and only the second
// changes the composite's shape.
//
// **Exactness at full strength is a requirement, not a happy accident.** At
// `a == 1` over an opaque source the keep factor is exactly `1 - 1 * 1 == 0`,
// so `dst' == src` in every channel, bit for bit -- `0.0f * finite` is exactly
// `0.0f` and `x * 1.0f` is exactly `x`. A clone at full flow and full opacity
// **reproduces the source texels exactly**, and `--selftest` asserts that at
// zero tolerance rather than within a rounding bound, because "clone" is the one
// word in this build that promises a copy.
//
// **Alpha lock (`core/Layer.hpp`) is honoured, not refused**, unlike
// `brush/RgbErase`'s row: painting on an alpha-locked layer is the feature that
// flag exists to allow, and a clone *paints*. `brush/RgbDeposit` §4.5's
// colour-only composite carries over with no un-premultiply anywhere, which is
// worth spelling out because the obvious derivation needs one and then cancels
// it:
//
//     straight = src / src[3];  coverage = a * src[3]
//     dst'.rgb = dst.rgb * (1 - a*src[3]) + straight * (a*src[3]) * dst[3]
//              = dst.rgb * (1 - a*src[3]) + src.rgb * a * dst[3]
//     dst'.a   = dst[3]                                    // frozen
//
// The `src[3]` cancels, so the division never has to be written and the
// `src[3] == 0` singularity never has to be special-cased. A version that DID
// un-premultiply would be undefined on exactly the texels a clone stamp is most
// often dragged over -- the partially covered rim of an existing mark.
//
// ==========================================================================
// 2. Reading and writing one store in one pass -- the hazard, and the snapshot
// ==========================================================================
//
// **This is the module's real subject.** The source region and the destination
// region are two windows onto the *same* `core::TileStore`, and nothing stops
// them overlapping -- a one-texel offset is a legitimate, common gesture (it is
// how a smear is made) and a large offset still overlaps whenever the stroke is
// dragged across the source. A loop that read the live store would then feed its
// own output back in, and the result would depend on the order the loop happened
// to visit texels and tiles in:
//
//   * With a source one texel to the LEFT and the inner loop running in
//     ascending x, every texel reads the output of the texel before it, so a
//     shifted copy becomes **one column smeared across the whole dab**.
//   * With the source one texel to the RIGHT, the same loop reads texels it has
//     not written yet and produces the correct shifted copy.
//
// Same offset magnitude, opposite sign, two different *kinds* of answer -- and
// the same split again at tile granularity (the tile loop is (y, x)-ascending)
// and again at dab granularity. Nothing in the picture says which one happened.
//
// **The source is snapshotted at pen-down**, and the snapshot is a plain copy of
// the `TileStore`. That is not expensive and it is not a special mechanism:
// `core/TileStore.hpp` says "copying a store IS the share", so the copy costs one
// `unordered_map` node and one atomic increment per *existing* tile and not one
// texel of tile data, and the first write to each destination tile unshares it
// (`getOrCreate`) leaving the snapshot holding the pre-stroke bytes. This is the
// identical mechanism a history entry and `ui/DocumentTexture`'s frame snapshot
// already use, so the clone stamp adds no new lifetime rule to the codebase.
//
// The rule it does obey is the one that header states: *take the copy before any
// write handle into the same store is live*. `begin()` copies, and no dab has run
// yet, so there is no live handle to violate it with.
//
// Two rejected alternatives, recorded so they are not re-proposed:
//
//   * **Read the live store and forbid overlap.** There is no such thing as a
//     non-overlapping clone offset in general (the stroke moves, the offset does
//     not), and a tool that quietly produced a different picture depending on
//     which way the user dragged is worse than one that refuses.
//   * **Snapshot only the tiles the dab will read, per dab.** Correct for one
//     dab and wrong for a stroke: dab N would read dab N-1's output wherever the
//     stroke has already passed over its own source, which is the same hazard
//     one level up. The unit of the snapshot has to be the unit of the undo step,
//     and that is the stroke (`app/StrokeSession` §2).
//
// `--selftest` proves the property rather than the mechanism: the same stroke at
// offsets `-1` and `+1` must both produce the same shifted copy, which is a claim
// no live-reading implementation can satisfy in both directions at once.
//
// ==========================================================================
// 3. The offset is INTEGER texels, and that is a stated narrowing
// ==========================================================================
//
// `begin()` rounds the offset to the nearest whole texel and samples with no
// filtering. Two reasons, one of them a promise:
//
//   * §1's exactness claim only exists at integer offsets. A resampled source is
//     a *reconstruction* of the source, and "clone" would then mean "a blurred
//     copy" at every offset that was not whole -- with no control anywhere in
//     the chrome saying so.
//   * A filter kernel is a decision this build makes in `ops/Resample`, not in a
//     deposit loop, and choosing one here would be choosing it for the whole
//     codebase from the least visible place.
//
// The cost, stated rather than hidden: an offset set by two pointer positions is
// snapped by at most half a texel, which at any zoom below 200 % is smaller than
// the pixel the user clicked in.
//
// ==========================================================================
// 4. Cloning nothing must COST nothing
// ==========================================================================
//
// `brush/RgbErase` §4's asymmetry, arrived at from the other side. A dab whose
// **source** texel is empty writes nothing, and that is arithmetic rather than an
// optimisation: at `src == 0` the composite above is `0 * a + dst * (1 - 0 * a)`,
// which is `dst` bit for bit. Returning "nothing to do" instead is what stops a
// clone dragged over blank source from allocating a 128 KiB destination tile per
// tile it crossed and reporting every one of them dirty for re-composite on every
// frame of the drag.
//
// **The test is all four channels, not the alpha**, exactly as that header
// argues: a texel holding colour at alpha 0 is malformed rather than absent, and
// this function must not be the thing that quietly declares it empty -- it holds
// something, so it is cloned like anything else and the malformation is
// reproduced honestly rather than laundered.
//
// A source coordinate **outside the canvas**, or in a tile that does not exist,
// reads as four zeros and therefore takes the same path. There is no clamp-to-
// edge: clamping would smear the border row across everything sampled past it,
// which looks like a working clone and is not one.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the boundary
// `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw.
//
// **No cross-layer sampling.** Photoshop's clone stamp can read a different
// layer, or the composite of all of them. This one samples the store it writes,
// which is the layer the stroke is aimed at. Adding a second store is a signature
// change here and a source picker in the chrome; neither is built, and pretending
// otherwise by taking a `const TileStore*` nobody can set would be a dead
// parameter.
//
// **No anchor and no gesture.** *Where* the source is, and how an Option+click
// sets it, is session state that outlives a stroke -- it belongs to
// `app/AppState`'s `CloneSourceState` and `app/StrokeSession`'s two free
// functions, and this module takes the resolved vector. A module that owned the
// anchor would have to survive between strokes, which is the one thing a stroke
// object must not do.
//
// **No aligned/non-aligned toggle.** This build clones *aligned* -- the offset
// survives pen-up, so a second stroke continues the same copy rather than
// restarting it at the anchor. `app/AppState.hpp`'s `CloneSourceState` carries
// the argument and names the control that would switch it.
namespace np {

// §1's rule, as a pure function of two texels, for the reason
// `eraseRgbTexel()`/`depositRgbTexel()` are pure: the invariants are about *this
// arithmetic*, so `--selftest` asserts them on this and not on a tile of it.
//
// `dst` and `src` are stored PREMULTIPLIED texels; `strokeAlpha` is `A`, the
// fraction of the source this stroke has already transferred here; `weight` is
// `flow * coverage` with the selection already folded in; `opacity` is the
// stroke's ceiling on `A`.
//
// Total: defined for every finite input, including `A >= 1`, `A >= opacity`,
// `weight <= 0`, `opacity <= 0` and an all-zero `src`, each of which returns
// `dst` **bit-identical** with `dabAlpha == 0` -- the caller's signal to skip the
// texel rather than write a value equal to the one already there, for
// `depositRgbTexel()`'s stated reason (a stroke at its ceiling must stop
// perturbing the tile it is scrubbing over, or the tile is re-uploaded every
// frame of a drag that is changing nothing).
struct CloneStampStep {
  std::array<float, 4> premultiplied{};  // the texel to store
  float strokeAlpha = 0.0f;              // A', to put back in the accumulator
  float dabAlpha = 0.0f;                 // a; 0 means "nothing to do here"
};
CloneStampStep cloneStampTexel(const std::array<float, 4>& dst, const std::array<float, 4>& src,
                               float strokeAlpha, float weight, float opacity,
                               bool alphaLocked) noexcept;

// One clone stroke in flight: the pre-stroke snapshot of the source (§2), the
// integer offset (§3), the latched ceiling, and the accumulator that makes that
// ceiling a per-stroke bound rather than a per-dab multiplier.
//
// Deliberately a small object with an explicit lifetime rather than a free
// function, for `RgbStroke`'s and `RgbEraseStroke`'s stated reason: the
// accumulator is only correct against the opacity it was started with *and* the
// snapshot it was started against, so binding all three at `begin()` makes the
// combinations that can go wrong -- last stroke's snapshot, this stroke's
// accumulator -- unspellable.
class CloneStampStroke {
 public:
  // Pen-down. Takes the source snapshot (§2), rounds the offset to whole texels
  // (§3), latches the opacity and clears any accumulator a previous stroke left
  // -- the last for `StrokePath::reset()`'s reason, restated by both sibling
  // strokes: alpha carried across strokes would let the ceiling of the last one
  // cap the first dab of the next.
  //
  // `source` is copied, not borrowed, so nothing here outlives a caller.
  // `opacity` is clamped to [0,1]; a non-positive one leaves a stroke that
  // transfers nothing, which is a legitimate setting and not an error.
  //
  // `alphaLocked` is latched with them, for `RgbStroke::begin()`'s reason: a
  // lock cleared or set mid-drag must not change which composite the dabs
  // already spent were read back through. Read from the `Layer` by
  // `app/StrokeSession`, because this module deliberately knows nothing about
  // one (§5).
  void begin(const TileStore& source, Vec2 offset, float opacity, bool alphaLocked);

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator **and the snapshot**, which is the larger of
  // the two: the snapshot shares tiles with the live layer, so holding one after
  // the stroke would keep every tile the stroke unshared alive at twice its size
  // for as long as the application sat idle.
  void end() noexcept;

  // Clones one dab into `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, `core/SelectionMask.hpp`'s convention and NOT
  // the inverse).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to fold
  // with `sortUniqueTiles()`.
  //
  // A dab whose source is blank, or all of whose texels have reached the
  // ceiling, writes nothing, allocates nothing and reports no tiles (§4).
  DepositCount cloneDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                        int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

  // Clones every dab in `dabs`, in order. Order matters for the accumulator the
  // same way it does in the deposit -- and, unlike the deposit, NOT for the
  // source, which is the whole of §2.
  StrokeDeposit cloneDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                          int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The fraction of the source this stroke has transferred at a document texel
  // so far -- 0 for a texel it has not reached. The accumulator's read side,
  // exposed because it is what `--selftest` asserts the ceiling against at zero
  // tolerance: the stored texel has been through binary16 once per dab and the
  // accumulator has not.
  float strokeAlphaAt(PixelCoord doc) const noexcept;

  float opacity() const noexcept { return opacity_; }
  // The rounded offset actually in use (§3) -- what `--selftest` reads to prove
  // the snapping happened rather than assuming it.
  int32_t offsetX() const noexcept { return offsetX_; }
  int32_t offsetY() const noexcept { return offsetY_; }

  // What the two stores currently hold. `--selftest` prints both, because the
  // memory claims ("the snapshot costs map nodes and not tile bytes"; "both are
  // freed at pen-up") are worth checking rather than trusting.
  size_t accumulatorTiles() const noexcept { return alpha_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return alpha_.tileBytes(); }
  size_t snapshotTiles() const noexcept { return source_.occupiedTileCount(); }

 private:
  // The pre-stroke source (§2). A whole store rather than the handful of tiles a
  // stroke turns out to read, because which tiles those are is not known until
  // the stroke is over, and because a copy is a share -- see the header.
  TileStore source_;
  int32_t offsetX_ = 0;
  int32_t offsetY_ = 0;
  float opacity_ = 1.0f;
  bool alphaLocked_ = false;
  bool active_ = false;
  StrokeAlphaStore alpha_;
};

}  // namespace np
