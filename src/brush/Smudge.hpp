#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/SelectionMask.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// brush/Smudge -- **dragging colour that is already on the layer.**
//
// ==========================================================================
// 0. What this is, and why it is a third sibling rather than a flag
// ==========================================================================
//
// `brush/RgbDeposit` puts the tip's own ink down. `brush/RgbErase` takes what
// is there away. This one puts down **what it just picked up**, so the tip
// carries no ink at all and the layer is both the source and the destination.
// `brush/RgbDeposit` §5's "what is deliberately not here" list names it by
// name -- "No blend mode, no smudge, no texture" -- and this is that item,
// built beside it for `brush/RgbErase` §0's reason: the three modules share
// their dab stream, their falloff (`dabCoverage()`), their footprint
// (`dabPixelBounds()`), their grain modulation, their tile-major loop and
// their selection convention, and differ in the one place that matters --
// what one dab does to one texel, and what the stroke remembers between dabs.
//
// **This is the first route in this codebase whose dabs are not
// independent.** Deposit, erase and pigment deposit all compute a dab from
// `(tip, centre, dst)` alone; the accumulators `brush/RgbDeposit` §3 and
// `brush/RgbErase` §2 keep are per TEXEL, and a texel's history is read back
// out of the store it is keyed in. Smudge's memory is per STROKE and is a
// colour: what the tip is currently holding, which every texel of the next dab
// reads. §1 is where that lives and why.
//
// ==========================================================================
// 1. Where the carried colour lives: on the stroke object, not in a static
// ==========================================================================
//
// The finger is four floats and one "has it been loaded yet" bit. That is
// small enough that a file-static in `brush/Smudge.cpp`, with a free
// `smudgeDab()` beside the other routes' free functions, would compile and
// would appear to work -- which is exactly why the decision is worth writing
// down rather than discovering.
//
// **It goes on `SmudgeStroke`, which `app/StrokeSession` owns as a member
// beside `rgb_`, `erase_` and `pigErase_`.** `app/StrokeSession.hpp` already
// states the shape and the reason: each route's per-stroke state is "a small
// object with an explicit lifetime rather than a free function taking a
// `StrokeAlphaStore&`", begun at `begin()` and ended at `end()`, and the
// `else` branch of each of those three `if`s is the load-bearing half --
// whichever route a stroke took, the other engines must be left holding
// nothing, and an interrupted drag (a window blur, a lost pointer capture) is
// the case that reaches `begin()` with one of them still live. A fourth member
// inherits all of that for free. Three things a file-static would break, in
// increasing order of how long they would take to find:
//
//   * **Two documents.** `app/AppState` holds a document *list*; nothing stops
//     a second `StrokeSession` existing. One static finger means the colour
//     picked up in one document is laid down in another.
//   * **Cross-stroke leak.** A static's value survives pen-up, so the first
//     dab of the next stroke lays down the *last* stroke's colour before it
//     has picked anything up -- the same defect `StrokePath::reset()`,
//     `RgbStroke::begin()`'s ink latch and `StrokeSession::smoothPressure()`'s
//     `pressureSmoothLatched_` each exist to prevent, arriving through the one
//     door none of them guards. It is invisible until the two strokes are in
//     different colours.
//   * **It is not this codebase's shape.** `--selftest` drives these engines
//     directly, several sections in one process, in whatever order `main.cpp`
//     lists them; a static would make one section's fixture depend on whether
//     another ran first.
//
// The rejected middle option, for the record: a plain `SmudgeState` struct the
// caller declares and threads through a free `smudgeDab(state, ...)`. It fixes
// all three problems above and is genuinely close. It was rejected because the
// carried colour is only meaningful against the **strength it was carried
// with** (§3: strength is what decides how fast the finger decays, so a finger
// filled under one strength and spent under another describes nothing), and
// binding the two together at `begin()` is what makes that one wrong
// combination unspellable -- `RgbStroke`'s own stated reason for being a class
// rather than a free function over a store, and `RgbEraseStroke`'s for
// latching the strength beside the accumulator.
//
// **What is deliberately NOT here: a per-texel accumulator.** Both the deposit
// and the erase keep a `StrokeAlphaStore` -- 64 KiB per touched tile -- so that
// `opacity`/`strength` is a per-stroke ceiling or floor rather than a per-dab
// multiplier. Smudge has none, and this is a decision rather than an omission.
// `brush/RgbDeposit` §2 states the one assumption its accumulator needs: "the
// colour does not change during the stroke -- if it did, the incremental
// composite would be a weighted history of several colours and the single
// scalar `A` could not describe it." **A smudge's colour changes every dab, by
// definition.** A scalar "fraction of this texel replaced by the finger" would
// therefore be a fraction of *nothing in particular*, and capping it would cap
// a quantity with no meaning. What that costs is stated plainly in §4, not
// hidden: within one stroke, scrubbing a partially-selected texel converges on
// the finger rather than stopping at `sel`. The per-stroke state is four floats
// and a bit; a stroke across thirty tiles costs 20 bytes, not 1.9 MiB.
//
// ==========================================================================
// 2. What "pick up" means under a soft tip
// ==========================================================================
//
// A tip is not a pixel: `dabCoverage()` is 1 in the core and smoothsteps to 0
// at the rim (`brush/Deposit` §2), so "the colour under the tip" has to be a
// summary of a few hundred texels. The rule is the **coverage-weighted mean of
// the stored PREMULTIPLIED texel**, over the same footprint the write will
// use:
//
//     pick = sum_i(w_i * dst_i) / sum_i(w_i)        // all four channels
//     w_i  = grainCoverageAt(grain, dabCoverage(tip, dx, dy), x, y)
//
// Three things in those two lines are decisions.
//
// **(i) Weighted by coverage, and by exactly the coverage the write uses.** The
// alternative -- a flat mean over the bounding box, or a single sample at the
// dab centre -- makes the tool's pickup and its deposit disagree about where
// the tip is. A single centre sample is the version that looks right in a
// still: it smears, and it is a nearest-neighbour resample, so a smudge across
// a soft gradient comes out banded and a smudge along a one-texel line either
// grabs the line or misses it entirely depending on sub-texel phase. The
// weighted mean is also what makes the tool's own falloff (§5) smooth: the
// finger changes by a little when the tip has moved by a little, because the
// weights it averages under have moved by a little.
//
// **(ii) The mean is of the PREMULTIPLIED texel, and that is what makes alpha
// come out right for free.** `core::Tile` stores `(colour * a, a)`
// (core/TileStore.hpp), so averaging the stored quadruple weights each texel's
// *colour* by its own alpha automatically: a half-transparent red and an empty
// texel average to a quarter-alpha red, not to a half-bright pink at half
// alpha. Averaging the straight colour and the alpha separately would need an
// un-premultiply per texel -- a division by an alpha that is exactly 0 on every
// unpainted texel, which is most of them at the edge this tool is aimed at --
// and would produce a different, wrong answer at every rim texel. A convex
// combination of well-formed premultiplied texels is a well-formed
// premultiplied texel (if every input with alpha 0 has colour 0, then either
// some input has alpha > 0 and the mean's alpha is > 0, or every input is the
// zero texel and so is the mean), so the finger can never become the malformed
// "colour at alpha 0" that `brush/RgbErase` §1 describes and `core/Composite`
// would read as an additive glow.
//
// **(iii) An ABSENT tile contributes transparent black; it is not skipped.**
// This is the opposite of `brush/RgbErase` §4's rule and the difference is the
// whole tool. An eraser has nothing to take from an unpainted texel, so it
// skips it. A smudge dragged off the edge of a stroke is *supposed* to be
// picking up emptiness -- that is what makes the carried colour thin out as it
// leaves the paint (§5's falloff) instead of the tip behaving like a clone
// stamp that carries the same opaque red to the far side of the canvas. So the
// mean is taken over the entire covered footprint, with a missing tile and a
// never-written texel both reading as `(0,0,0,0)`, which is exactly what they
// hold.
//
// **The pick-up is computed BEFORE any of this dab's writes**, in a separate
// pass over the footprint, and the finger is updated exactly once per dab. If
// the two were interleaved, a texel's contribution to the mean would depend on
// whether the loop had reached it yet -- and the loop is tile-major, so the
// answer would depend on which tile boundary the dab happened to straddle.
// That is a rounding error you cannot see and cannot reproduce; two passes
// costs a second `dabCoverage()` evaluation per texel and buys an answer that
// does not depend on the tile grid at all.
//
// **The selection does NOT gate the pick-up, only the write** -- §4.
//
// ==========================================================================
// 3. Strength: one number, both directions, and both endpoints provable
// ==========================================================================
//
// Per dab, with `pick` from §2 and `strength` latched at pen-down:
//
//     finger' = lerp(pick, finger, strength)          // how much survives
//     a       = clamp(flow * cov * sel * strength)    // how much goes down
//     dst'    = lerp(dst, finger', a)                 // all four channels
//
// **Strength appears twice and it is one quantity: how far the finger
// dominates the canvas.** High strength means the canvas barely changes the
// finger (`finger' ~ finger`) and the finger strongly changes the canvas
// (`a ~ flow * cov`); low strength means the reverse. The two uses have
// opposite signs because they are the two ends of the same coupling, not
// because one number is doing two jobs.
//
// **Strength 0 is a bit-exact no-op, and that is a requirement rather than a
// nicety.** At 0, `a` is exactly 0 whatever `flow`, `cov` and `sel` are, so
// `smudgeTexel()` returns `dst` **bit-identical** with `dabAlpha == 0` -- the
// caller's signal to not fetch a write handle, not unshare the copy-on-write
// tile, not report the tile dirty and therefore not move the revision or
// record a history entry. A "smudge" at strength 0 leaves the document
// byte-for-byte as it found it and produces no undo step, which is
// `app/StrokeSession` §2's rule reached by arithmetic. The near-miss version
// is worth naming because it is what falls out if `strength` is left out of
// `a`: `finger' = pick` at strength 0, so `dst' = lerp(dst, localMean, flow *
// cov)` -- a **blur**. It writes every texel, allocates every tile, dirties
// the whole path and records an undo step, and a user who turned the slider to
// zero to make the tool stop watches it soften their painting instead.
//
// **Strength 1 carries indefinitely, and that is Photoshop's answer too.** At
// 1, `lerp(pick, finger, 1)` returns `finger` exactly (`std::lerp(a, b, 1)` is
// specified to return `b`), so after the first dab loads it the finger never
// changes again: the stroke lays the colour it picked up at pen-down along its
// entire length, however long, with no decay at all. That is not a degenerate
// case, it is the setting a user picks to drag one colour across a canvas.
//
// **The first dab LOADS the finger outright; it does not blend into it.**
// There is nothing to retain yet. Starting the finger at `(0,0,0,0)` and
// running the blend from dab one would make the strength-1 stroke -- the one
// setting above -- retain transparent black forever and do nothing at all,
// which is the single most confusing possible failure: the slider at maximum
// is the slider that stops working. `loaded()` is that latch, the same shape
// `StrokeSession`'s `seedLatched_` and `initialDirectionLatched_` use for the
// same reason.
//
// **`strength` is `BrushTip::opacity`**, and the argument is `brush/RgbErase`
// §2's word for word: it is the same slider, latched at pen-down by the same
// `StrokeSession::begin()`, meaning the same thing in the same units -- the
// fraction of the maximum effect one stroke may reach. A second "strength"
// number on `BrushState` would leave the OPACITY control inert while the
// smudge was selected, which is the failure the options bar's
// disabled-rather-than-hidden treatment exists to prevent.
//
// **`flow` is how fast one dab bites and is deliberately not clamped**, for
// `brush/Deposit`'s stated reason; `a` is clamped to [0,1] instead, because
// unlike the deposit and the erase there is no accumulator whose `min` would
// have capped it, and a mix fraction above 1 would extrapolate past the finger
// -- overshooting into negative alpha on the far side of a soft rim.
//
// ==========================================================================
// 4. The selection bounds the write, and the honest limit of that
// ==========================================================================
//
// PRD E1 (**P0**): "Every deposit and every op respects the active selection."
// `sel` multiplies `a`, so at `sel == 0` the texel is returned bit-identical
// and no tile is fetched: **nothing outside the marching ants is touched, at
// all, ever.** `--selftest` asserts the untouched region byte-for-byte rather
// than approximately, because what a smudge destroys outside a selection drawn
// to protect it is invisible until the layer under it is, and one undo step
// covers the whole stroke.
//
// **What `sel` between 0 and 1 does, stated rather than overclaimed.**
// `brush/RgbDeposit` §4 and `brush/RgbErase` §3 make the selection enter
// *twice* -- once in the rate, once as a cap on the per-stroke accumulator --
// because with the rate alone a scrubbing stroke walks through a feathered
// edge and the bound is only a speed limit. **Smudge has no accumulator to
// cap** (§1), so within one stroke a half-selected texel scrubbed repeatedly
// does converge on the finger.
//
// That is not the same defect wearing a different hat, and the difference is
// `brush/Deposit` §4's own precedent: it caps the *mass* by `sel` and
// deliberately leaves the *mixing weight* uncapped, so "hue keeps moving on a
// texel that has reached its selection-scaled cap ... the selection says how
// much of this edit lands here, not which colour it is allowed to be." A
// smudge moves colour, and colour is the quantity that precedent already
// leaves unbounded. What a smudge also moves is alpha, which is the quantity
// that precedent does bound -- and that half genuinely is only rate-gated
// here. Closing it would need the pre-stroke texel remembered per texel, which
// is `brush/RgbErase` §2's rejected `A0` store: a second copy of the layer,
// correct only for as long as nothing else writes the layer during the stroke.
// Rejected there, rejected here, and recorded so it is not re-proposed as new.
//
// `nullptr` means "no restriction" and 1.0 everywhere -- core/SelectionMask.hpp's
// convention and NOT the inverse (a caller who writes `sel ? cov : 0` has
// inverted the editor). That header requires every hoisted per-texel loop to
// own its own copy of the null branch and warns that a perturbation inverting
// one copy leaves the others right; this file has **two** such loops (§2's
// pick-up pass and the write pass), which is one more place to get it wrong,
// so they are deliberately not the same branch: the pick-up pass does not
// consult the selection at all, and the write pass owns the whole of it.
//
// **The pick-up reads through the ants, and that is the considered answer.**
// The selection is a bound on the *edit*; reading is not an edit, and every
// other reader in this build (the eyedropper, the clone source a future step
// will want, `core/Composite` itself) reads the layer without asking. Gating
// the pick-up too would also make the tool's own behaviour depend on where the
// tip's *rim* fell rather than on where it wrote -- the finger would thin out
// as the tip approached a selection edge from inside, so a smear would fade
// before it reached the boundary it was aimed at, for a reason nothing in the
// UI could show.
//
// ==========================================================================
// 5. Why a smudge fades, and what falls out of §§2-3 without being coded
// ==========================================================================
//
// Nothing in this module implements "the smear gets weaker the further you
// drag it". It is a consequence, and stating the mechanism is what keeps
// someone from later adding a distance ramp that would double it:
//
// Dragging out of a filled region, each dab's `pick` is the mean over a
// footprint that is part paint and part what the previous dabs left, and each
// `finger' = lerp(pick, finger, strength)` pulls the carried colour toward it.
// Past the boundary the footprint holds nothing but the tool's own thinning
// output, so the finger decays geometrically with dab index -- roughly
// `strength^n` -- and the alpha laid down decays with it. At `strength == 1`
// the decay factor is exactly 1 and the smear does not fade at all (§3), and
// at `strength == 0` there is no smear to fade. `--selftest` asserts the
// monotone fall along a straight drag, which is the assertion that fails
// against a tool that picked up once and stamped.
//
// **Alpha participates, and it is the point.** A rule that copied `dst.a`
// through unchanged -- `brush/RgbDeposit` §4.5's alpha-locked composite, which
// is a perfectly good rule for a *brush* -- would give a smudge that can
// recolour paint that is already there and can do **literally nothing** on the
// transparent side of an edge, which is where every user points it: smearing
// the edge of a stroke out into blank canvas is the tool's main use, and it is
// a movement of coverage before it is a movement of colour. So the write is one
// `lerp` factor across all four premultiplied channels, the same
// "one factor, all four channels" discipline `brush/RgbErase` §1 derives for
// the destination-out and `fillThroughSelection()` states for the bucket: it
// makes a texel half *present* rather than half *bright*, and a smeared rim
// has no fringe.
//
// The consequence, which is a feature and is asserted: a smudge **grows** the
// painted region, so unlike the eraser it must allocate tiles the stroke
// passes over. §6 is the one case where it must not.
//
// ==========================================================================
// 6. Smudging nothing with nothing must cost nothing
// ==========================================================================
//
// `brush/RgbErase` §4 skips an absent tile unconditionally -- an eraser has
// nothing to take there. **Smudge cannot borrow that**, because a loaded finger
// laying colour into empty space is the tool working (§5). The condition is
// therefore on the finger, not on the tile:
//
//   * **A texel whose four channels already equal the finger's is skipped**,
//     inside the tile, and this is the one that carries the guarantee. It is
//     an exact equality and not a tolerance, because it is the test for "the
//     lerp would return `dst` unchanged" and `lerp(x, x, a)` is exactly `x`.
//     Empty canvas under an empty finger is that case, so `getOrCreate()` is
//     never reached and **not one 224 KiB tile is allocated** by a smudge
//     dragged across an empty document -- no tiles reported, no revision
//     moved, no history entry. `sel == 0`, strength 0 and the transparent tail
//     of the falloff arrive at the same `dabAlpha == 0` and mean the same
//     thing.
//   * **An absent tile is skipped whole while the finger is the zero texel**,
//     before the texel loop runs at all. This one is speed, not correctness --
//     the texel test above has already made the allocation impossible -- and
//     it is what keeps a drag across blank canvas from evaluating
//     `dabCoverage()` a few hundred times per dab to prove it has nothing to
//     do. The moment the tip touches real paint the finger stops being zero
//     and the skip stops firing, which is the tool working.
//
// The visible consequence `--selftest` asserts: **a stroke that smudged
// nothing records nothing** -- `app/StrokeSession` §2's rule again, an undo
// step that undoes nothing being a worse defect than a missing one.
//
// ==========================================================================
// 7. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the same
// boundary `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw:
// this is the arithmetic of moving colour around a `core::TileStore`, and the
// stroke lifecycle belongs to `app/StrokeSession`.
//
// **No Pigment smudge.** `strokeRouteFor()` refuses it by name and
// `app/StrokeSession.hpp` §1 carries the argument: a Pigment texel is a
// Kubelka-Munk latent plus a mass, not a premultiplied quadruple, and §2's
// coverage-weighted arithmetic mean is not what mixing two latents means --
// `depositTexel()` mixes them with a *mass*-weighted lerp whose idempotence in
// hue is `brush/Deposit` §1's whole load-bearing invariant. Averaging latents
// linearly would be a second, unproven mixing rule sitting beside the one this
// application exists for. The refusal is conditional and names its condition,
// exactly as the Pigment *erase* row did before `brush/PigmentErase` paid it
// off.
//
// **No Finger Painting.** Photoshop's smudge has a checkbox that loads the
// finger with the *foreground colour* at pen-down instead of with the canvas.
// `BrushTip::linearRgb` is deliberately not read here, for `brush/RgbErase`'s
// reason one tool over: a smudge that reached for the ink would be a brush,
// and the one thing this tool must not do is introduce a colour that was not
// already in the picture. Adding it later is a `begin()` overload, not a
// change to any arithmetic below.
//
// **No sample-all-layers.** The pick-up reads the target layer's own store and
// nothing else. Sampling the composite would need `core/Composite` in `brush/`,
// which is the include edge §7's first paragraph refuses.
namespace np {

// §3's rule, as a pure function of one texel, for the one reason a pure
// function earns its keep in this family: the invariants are about *this
// arithmetic*, so `--selftest` asserts them on this and not on a tile of it.
//
// `dst` is the stored PREMULTIPLIED texel; `finger` is the carried colour,
// premultiplied in the same convention and ALREADY updated for this dab
// (§3's first line, which is `smudgeFinger()` below); `weight` is
// `flow * coverage` with the selection already folded in; `strength` is the
// stroke's latched dominance.
//
// Total: defined for every finite input, including `weight <= 0`,
// `strength <= 0`, a `weight * strength` above 1 and a `finger` equal to
// `dst`, each of which returns `dst` **bit-identical** with `dabAlpha == 0` --
// the caller's signal to skip the texel entirely rather than write a value
// equal to the one already there. Bit-identical rather than recomputed for
// `depositRgbTexel()`'s and `eraseRgbTexel()`'s stated reason: a dab that is
// changing nothing must stop perturbing the tile it is scrubbing over, or the
// caller's "nothing to do here" test never fires and the tile is re-uploaded
// on every frame of a drag that is changing nothing.
struct SmudgeStep {
  std::array<float, 4> premultiplied{};  // the texel to store
  float dabAlpha = 0.0f;                 // a; 0 means "nothing to do here"
};
SmudgeStep smudgeTexel(const std::array<float, 4>& dst, const std::array<float, 4>& finger,
                       float weight, float strength) noexcept;

// §3's first line, likewise pure and likewise asserted directly: the carried
// colour after one dab has picked `pick` up.
//
// `loaded` is false only on a stroke's very first dab, and then the answer is
// `pick` outright whatever `strength` says -- §3's "there is nothing to retain
// yet", and the reason a strength-1 smudge is not a no-op.
std::array<float, 4> smudgeFinger(const std::array<float, 4>& finger, bool loaded,
                                  const std::array<float, 4>& pick, float strength) noexcept;

// One smudge stroke in flight: the latched strength, and the four floats that
// are this route's entire per-stroke memory (§1).
//
// A small object with an explicit lifetime rather than a free function over a
// caller-owned struct, for `RgbStroke`'s and `RgbEraseStroke`'s stated reason
// applied to this route's own pairing: the carried colour is only meaningful
// against the strength it was carried with (§3), so binding the two together
// at `begin()` makes the one combination that can go wrong -- a finger from
// one stroke spent at another's strength -- unspellable.
class SmudgeStroke {
 public:
  // Pen-down. Latches the strength and **empties the finger**, exactly as
  // `StrokePath::reset()` clears leftover arc length and for the same reason:
  // a colour carried across strokes would make the first dab of a new stroke
  // lay down the previous stroke's paint before it had picked anything up,
  // which is invisible until the two strokes are in different colours.
  //
  // `strength` is clamped to [0,1]; a non-positive one leaves a stroke that
  // changes nothing, which is a legitimate setting and not an error (§3).
  void begin(float strength) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Empties the finger and leaves `strength()` alone, so the readers
  // below still answer correctly after a stroke ends.
  void end() noexcept;

  // Smudges one dab across `store`, clipped to the canvas and gated by
  // `selection` (nullptr means no restriction, §4).
  //
  // Two passes over the footprint: §2's pick-up, which reads and updates the
  // finger exactly once, and then the write. Every tile it writes is appended
  // to `touchedOut` when that is non-null, at the moment the tile is first
  // written -- the same branch as the write, for `brush/Deposit` §3's reason.
  // Duplicates across dabs are the caller's to fold with `sortUniqueTiles()`.
  //
  // A dab on blank canvas with an empty finger writes nothing, allocates
  // nothing and reports no tiles (§6). A dab with a loaded finger over blank
  // canvas allocates and writes, which is the tool working.
  DepositCount smudgeDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                         int32_t canvasH, const Selection* selection,
                         std::vector<TileCoord>* touchedOut);

  // Smudges every dab in `dabs`, in order. Order matters more here than on any
  // other route: the finger is a running state, so dab N's output depends on
  // every dab before it, and reversing the list reverses the direction the
  // colour travels.
  StrokeDeposit smudgeDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                           int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The carried colour, premultiplied, and whether any dab has loaded it yet.
  // Exposed because it is what `--selftest` asserts the pick-up rule against
  // directly: the stored texels have been through binary16 once per dab and
  // this has not.
  const std::array<float, 4>& finger() const noexcept { return finger_; }
  bool loaded() const noexcept { return loaded_; }

  float strength() const noexcept { return strength_; }

 private:
  float strength_ = 1.0f;
  bool active_ = false;
  // §3's latch. False means "no dab has picked anything up yet", and the next
  // dab loads `finger_` outright instead of blending into it.
  bool loaded_ = false;
  // The whole of this route's per-stroke memory: 16 bytes, against the 64 KiB
  // per touched tile the deposit and the erase each spend on an accumulator
  // they need and this one cannot use (§1).
  std::array<float, 4> finger_{};
};

}  // namespace np
