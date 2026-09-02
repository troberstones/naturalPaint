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

// brush/PencilDeposit -- **the aliased mark: a pencil on a plain RGB layer.**
//
// ==========================================================================
// 0. The question this module exists to answer, and why the obvious answer is
//    only half of one
// ==========================================================================
//
// **What is a pencil that `brush/RgbDeposit` with `hardness = 1` is not?**
// That is the whole design question, it has to be answered before a line of
// this is worth writing, and the answer everyone reaches for first -- "a
// pencil is *aliased*: its coverage is 0 or 1, never a fraction, so its edge
// is hard at every radius" -- is **already true of this codebase's hard
// brush**, and was true before this file existed. `singleTipCoverage()`
// (brush/Deposit.cpp) clamps `hardness` to [0,1], compares `d = sqrt(d2)/r`
// against it, and returns `1.0f` for every `d <= h`; the surrounding
// `if (!(d2 < r2)) return 0.0f;` returns exactly zero outside the disc. At
// `h == 1` the smoothstep between them has no interval left to live in, so a
// hard round tip's coverage is **already** a two-valued function. Photoshop's
// own answer, transplanted here unexamined, would have produced a module whose
// distinguishing feature was a `hardness` the brush panel can already set.
//
// The real difference is one step further out, and it is the difference
// between an aliased **dab** and an aliased **mark**:
//
//   **A hard dab does not give you a hard stroke.** `brush/RgbDeposit` §2's
//   accumulator builds a texel toward the ceiling at a rate of
//   `flow * coverage` per dab. Dabs are stamped every `spacing * radius`
//   pixels, so a texel near the *spine* of a stroke is covered by several
//   consecutive dabs and a texel near its *rim* by one or two -- and at any
//   `flow < 1` those two texels therefore end the stroke at different alphas.
//   The dab is binary; the stroke is a ramp. Drag a hardness-1 brush at flow
//   0.25 and the mark has soft shoulders, which is exactly the artefact a
//   pencil is chosen to avoid.
//
// So this module is defined by **two** rules, not one, and the second is the
// one that could not be had from a slider:
//
//   §1  The tip's coverage is **thresholded** to 0 or 1 before it becomes a
//       weight -- which generalises `hardness = 1` from "the procedural round
//       tip" to *every* source of coverage this build has (a sampled `.abr`
//       bitmap tip, a Dual Brush, paper grain), none of which `hardness` even
//       reaches.
//   §2  **Flow is not read at all.** One dab takes a covered texel straight to
//       the stroke's ceiling, so the number of dabs that happen to overlap a
//       texel cannot change its final value, and the *stroke* is as binary as
//       the dab.
//
// Together they make one checkable claim, and it is the claim
// `app/selftest/PencilDeposit.cpp` is built around: **every texel a pencil
// stroke writes ends at exactly the same alpha, and every other texel is
// untouched.** Neither rule alone gives that, which is why neither alone is a
// pencil.
//
// **Rejected: a `bool aliased` on `RgbStroke`.** It is the cheap version and
// it fails the same way `brush/RgbErase` §0 describes for the eraser -- but a
// step worse. `RgbStroke`'s §2 is an argument about a *rate*, `flow`, reaching
// a ceiling over many dabs; this route has no rate, so half the calls into a
// merged `depositDab()` would be governed by a header section that explicitly
// does not describe them. The flag would also have to be read at two places
// inside the hot per-texel loop (before the grain multiply and again at the
// weight), which is precisely the shape of edit that later gets "simplified"
// back to one.
//
// **NOT rejected, and deliberately reused: the composite itself.** See §4 --
// this module has no colour arithmetic of its own at all.
//
// ==========================================================================
// 1. The threshold: where it is applied, and at what value
// ==========================================================================
//
// `pencilCoverage()` below is the whole of it: `coverage >= 0.5 -> 1`, else
// `0`. Two decisions are inside that sentence.
//
// **Why 0.5 and not "> 0".** Both produce a binary mark; they disagree about
// where its edge is. `> 0` keeps every texel the tip touches at all, so a
// pencil would draw a disc of the FULL `radius` while a brush at the same
// settings draws a visibly smaller mark whose falloff has faded out well
// before the rim -- switching tools would change the size of the mark, which
// no user would read as "hardness". `>= 0.5` is the classical
// area-sampling/majority rule: a texel is in the mark exactly when at least
// half of it is covered, which puts the edge on the profile's own half-height
// contour -- the visual centre of the falloff, and therefore the place a
// painter already perceives the brush's edge to be. So a pencil and a brush at
// one radius make marks of the same *size*, differing only in whether the rim
// is antialiased. And on a hard tip the two rules agree exactly (coverage is
// already 1 or 0, and `1 >= 0.5`), so this choice is invisible to precisely
// the tip where §0 says there was nothing to choose.
//
// **Why it is applied LAST, to whatever coverage the tip and the paper
// produced together, rather than inside `dabCoverage()`.** The threshold is a
// property of this route, not of the tip: `dabCoverage()` is shared with three
// other routes and a `bool` parameter on it would put a pencil-shaped branch
// in the middle of the shape code (brush/Deposit.hpp §2e makes the identical
// argument for keeping grain out of that function). Applying it last is also
// what makes §0's generalisation true rather than aspirational -- a sampled
// bitmap tip, a Dual Brush blend and paper grain all produce fractional
// coverage by paths `hardness` never touches, and one threshold at the end
// covers all three and covers whatever the fourth turns out to be.
//
// **Grain therefore becomes a binary keep/drop mask, and that is the right
// answer rather than a side effect.** `grainCoverageAt()` scales coverage down
// by the paper's tooth; thresholding after it means a texel is drawn where the
// tooth stands proud of the half-height line and skipped where it does not.
// That is what graphite does -- it sits on the peaks of the paper and misses
// the pits -- and it is a *speckle*, never a partial texel, so §0's claim
// survives with grain switched on. A pencil that multiplied its binary
// coverage by a fractional grain would have produced grey texels and quietly
// broken the one property the tool is for.
//
// ==========================================================================
// 2. One dab is the whole mark -- flow is not read
// ==========================================================================
//
// `BrushTip::flow` does not appear anywhere in this module. The weight handed
// to the composite is the selection's coverage alone (§3), so for a texel this
// stroke has not yet reached, `depositRgbTexel()`'s
// `a1 = min(cap, a0 + w*(1 - a0))` is `min(opacity*sel, sel)` -- and since
// `opacity <= 1`, that is `opacity * sel` **on the first dab that covers the
// texel**. Every later dab finds `a0 == cap`, takes the `!(a0 < cap)` refusal,
// and writes nothing at all.
//
// Three consequences, all of them observable and all of them asserted:
//
//   * **The stroke is uniform.** A rim texel covered once and a spine texel
//     covered six times finish at the identical value, which is §0's whole
//     point. There is no ramp for the dab count to modulate because there is
//     no rate.
//   * **The accumulator is still necessary**, and this is worth stating
//     because "it saturates in one dab" invites deleting it. Without it, dab
//     two would source-over `opacity` onto a texel already holding `opacity`
//     and take it to `2p - p^2`; a scrubbed pencil would darken toward opaque
//     and the OPACITY slider would become a flow slider under another name.
//     `brush/RgbDeposit` §2's argument for the ceiling is this module's
//     unchanged, and the fact that it is reached instantly does not make it
//     less load-bearing -- it makes it the only thing holding the value.
//   * **A scrubbed pencil stops writing.** Dab two onward return
//     `dabAlpha == 0` for every texel, so no tile is unshared, none is
//     reported dirty, and live feedback stops re-uploading a mark that is not
//     changing.
//
// **Flow being ignored is Photoshop's shape too** -- its Pencil has no Flow
// control and no airbrush build-up, for the reason above rather than by
// analogy. **The UI does not yet say so, and that is a real gap named rather
// than hidden**: OPACITY and WET are drawn disabled on the routes that ignore
// them (`ui/MacPaintUI.cpp`'s `drawBrushPaintGroup()`, via
// `wetnessReachesSolver()`), but FLOW has no bespoke control to disable -- it
// is drawn by `ui/BrushFieldPresentation.cpp`'s generic field table, which has
// no route-aware disable mechanism at all. A `flowReachesRoute()` predicate
// with no reader would be worse than none (`app/StrokeSession.hpp`'s own note
// on why `grainReachesRoute()` earned its name: two call sites already asking
// the question). The honest sequence is that table growing a disable hook
// first, and this route being its first customer.
//
// **A `flow <= 0` tip still draws, deliberately.** Both sibling routes open
// with `if (!(tip.flow > 0.0f)) return count;`, and copying that guard here
// would mean a preset whose flow happens to be 0 silently disables a tool that
// does not have a flow -- a tool that stops working for a reason its own
// header says is not one of its inputs.
//
// ==========================================================================
// 3. The selection is NOT thresholded, and that is the one exception
// ==========================================================================
//
// PRD E1 (**P0**): "every deposit and every op respects the active selection."
// `sel` enters exactly where it does in both sibling routes -- into the weight
// and into the ceiling:
//
//     w   = sel                       // §2: no flow, no coverage term
//     cap = opacity * sel
//
// **Only the second of those two is load-bearing here, and saying so is the
// point.** `brush/RgbDeposit` §4 needed both because its weight is a rate: a
// selection folded only into the rate is a speed limit that a scrubbed stroke
// walks straight through, which that header found by measurement. This route
// has no rate (§2), so `a1 = min(opacity*sel, sel)` is `opacity*sel` on the
// first dab whatever the weight term is, and the cap alone is the whole bound.
// The weight term is kept because it is structural parity with the three
// sibling loops rather than because it does work -- and it is recorded as
// redundant here so that a later reader does not derive a false confidence
// from its presence, and so that `--selftest` does not claim to have
// sabotage-proven a gate that cannot be observed. It becomes load-bearing the
// moment anything gives this route a rate, which is exactly when it would be
// forgotten if it had been dropped.
//
// **It is the one fractional number this module lets through**, and the
// alternative was considered and rejected. Thresholding the selection as well
// would keep the "every written texel holds the same alpha" claim true with no
// exceptions -- and would make the pencil the one tool in the build that turns
// a feathered selection edge into a jagged one, silently discarding a
// `selectRectangle(64.25f, ...)` fractional column the user chose on purpose.
// The threshold is about the *mark's own shape*; a selection is not part of
// that shape, it is a mask on where the mark is allowed to land, and PRD E2's
// whole point is that the mask carries real coverage rather than a bit.
//
// So the claim §0 states carries one stated qualifier: **within the
// selection's own interior** (or with no selection at all), every texel a
// pencil stroke writes holds exactly `opacity`. Under a feathered edge it
// holds `opacity * sel`, which is a soft edge the *selection* drew and not one
// the pencil did.
//
// `nullptr` means "no restriction" and 1.0 everywhere, which is
// core/SelectionMask.hpp's convention and NOT the inverse. That header
// requires every hoisted per-texel loop to own its own copy of the null
// branch, and warns that a perturbation inverting one copy leaves the others
// right; this is one such loop, `--selftest` drives both nulls through it, and
// an engaged selection with no tile at a coordinate skips that tile whole.
//
// ==========================================================================
// 4. The composite is `depositRgbTexel()`'s, reused rather than copied
// ==========================================================================
//
// This is where this module deliberately does the OPPOSITE of what
// `brush/RgbErase` §0 did, and the two decisions rest on the same test.
// `RgbErase` is a sibling module because what one dab does to one texel is
// genuinely different arithmetic -- a destination-out against a source-over,
// counting a different quantity. Here it is the *same* arithmetic: a
// premultiplied source-over of an opaque ink scaled by `a`, against a
// per-stroke ceiling `A`, with `alphaLocked`'s colour-only variant
// (brush/RgbDeposit §4.5) applying for exactly the same reason it applies to a
// brush. Nothing about the pencil changes what a covered texel *becomes*; it
// changes only which texels are covered and how fast they get there.
//
// **So a second copy of that composite would be a second place for the pencil
// and the brush to disagree about colour** -- about premultiplication, about
// the `1 + 1ulp` clamp, about the alpha-locked branch -- and this codebase has
// already paid for that class of duplication more than once (brush/Deposit
// §2e's grain call that reached three routes and not the fourth;
// `strokeRouteWritesLayer()`, extracted after the options bar kept its own
// copy of the test). `depositRgbTexel()` is a pure function with the ceiling
// arithmetic already in it and four documented refusals; it is called from
// here unchanged, with `weight = sel`.
//
// What is genuinely this module's own is therefore small and exactly matches
// what §§1-3 argue: `pencilCoverage()`, the decision not to read `flow`, and
// the loop that puts those together. That the module is short is the evidence
// that the decomposition is right, not a reason to fold it into its sibling.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No Pigment pencil, and `strokeRouteFor()` refuses that row by name rather
// than falling back to `CpuDeposit`.** app/StrokeSession.hpp §1 carries the
// argument; the short form is that a Pigment texel has *mass*, not alpha, and
// mass is mixed by Kubelka-Munk with no per-stroke ceiling at all
// (`brush/Deposit` §1 and the OPACITY slider's own disabled state on that
// route say so) -- so there is nothing there for §2's "one dab is the whole
// mark" to be a rule about, and the honest binary-mass analogue is a second
// engine with a design question nobody has answered ("what is graphite in a
// Kubelka-Munk medium" is not a question the palette has an entry for).
// Routing the pencil to `CpuDeposit` instead would be the worse failure: the
// tool would work, produce a soft-edged mark, and be a pencil in name only on
// the layer kind whose whole point is that it is not RGB.
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the same
// boundary `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw:
// this is arithmetic against a `core::TileStore`, and the stroke lifecycle
// belongs to `app/StrokeSession`.
//
// **No Auto Erase.** Photoshop's Pencil has a mode that draws the background
// colour wherever the stroke *starts* on a texel already holding the
// foreground. It is a real feature and it is not here, because it needs a
// second latched colour (`BrushState` has no background colour that reaches a
// stroke -- `foregroundSrgb()` is the whole of §7) and a pen-down-time read of
// the layer under the cursor, which is a decision about the stroke lifecycle
// and therefore `app/StrokeSession`'s rather than this file's.
//
// **No blend mode.** `Layer::blend` still applies to the layer as a whole at
// composite time and is untouched.
namespace np {

// §1's number. Half, because a texel is in the mark exactly when at least half
// of it is covered -- the area-sampling rule, which puts the mark's edge on the
// falloff's own half-height contour so a pencil and a brush at one radius draw
// marks of the same size. Named rather than spelled `0.5f` in the `.cpp`
// because `--selftest` asserts the boundary case *at* it, and a test carrying
// its own copy of a threshold is a test that keeps passing after the threshold
// moves.
inline constexpr float kPencilCoverageThreshold = 0.5f;

// §1, as a pure function of one coverage value, exposed for the reason
// `combineDualCoverage()` is (brush/Deposit.cpp's own note): the threshold IS
// the module's design, and an assertion that can only observe it through a
// tile of deposited texels is an assertion about the loop as much as about the
// rule.
//
// Returns exactly `1.0f` or exactly `0.0f`, never anything between, for every
// finite input including negatives and values above 1. A NaN coverage returns
// `0.0f` -- `!(c >= t)` rather than `c < t`, the same guard shape
// `depositRgbTexel()` and `layerCoverage()` use, so a NaN refuses instead of
// propagating into the layer.
float pencilCoverage(float coverage) noexcept;

// One pencil stroke in flight: the latched ink and ceiling, and the
// accumulator that -- per §2 -- holds the mark at exactly that ceiling instead
// of letting a scrubbed stroke climb toward opaque.
//
// Deliberately a small object with an explicit lifetime, for `RgbStroke`'s
// stated reason: the accumulator is only correct against the colour and
// ceiling it was started with, so binding all three at `begin()` makes the one
// combination that can go wrong -- accumulator from one stroke, ink from
// another -- unspellable.
//
// **The accumulator is `StrokeAlphaStore`, borrowed from brush/RgbDeposit and
// not re-declared**, exactly as `RgbEraseStroke` borrows it: it holds the same
// quantity this route needs (alpha this stroke has laid at this texel), keyed
// by the same `TileCoord`, at the same float precision and for the same
// reason. A second 64 KiB tile type would be a copy with a different comment.
class PencilStroke {
 public:
  // Pen-down. Latches the ink, the ceiling and the layer's alpha lock, and
  // drops any accumulator a previous stroke left -- `StrokePath::reset()`'s
  // reason: alpha carried across strokes would let the end of one stroke cap
  // the start of the next, so a second pencil pass in a second colour would
  // refuse to write.
  //
  // `opacity` is clamped to [0,1]; a non-positive one leaves a stroke that
  // draws nothing, which is a legitimate setting and not an error.
  void begin(const std::array<float, 3>& straightLinearRgb, float opacity,
             bool alphaLocked = false) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator and leaves the ink and ceiling alone, so the
  // counts below still read correctly after a stroke ends.
  void end() noexcept;

  // Draws one dab into `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, §3).
  //
  // Named `drawDab` rather than `depositDab` on purpose: `depositDab` is the
  // name of the two functions that lay paint at a RATE, and this one does not
  // (§2). A reader who sees `pencil_.depositDab(...)` beside
  // `rgb_.depositDab(...)` in `app/StrokeSession`'s route dispatch would
  // reasonably assume the two take the same arguments to the same effect.
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to
  // fold with `sortUniqueTiles()`.
  //
  // A dab every one of whose texels has already reached the ceiling -- which,
  // per §2, is *every* dab after the first over a given texel -- writes
  // nothing, allocates nothing and reports no tiles.
  DepositCount drawDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                       int32_t canvasH, const Selection* selection,
                       std::vector<TileCoord>* touchedOut);

  // Draws every dab in `dabs`, in order.
  //
  // **A stroke is exactly the union of its dabs' thresholded discs, every
  // texel of it at the ceiling**, which is §2 being visible from outside: each
  // covered texel is decided by the first dab that reaches it and is refused
  // by every later one. `--selftest` computes that union from `dabCoverage()`
  // and the threshold alone and compares it against a painted stroke.
  //
  // Order therefore cannot matter, and the claim is stronger than the algebra
  // makes it look. `1 - prod(1 - w_i)` is symmetric in the dabs, so on paper
  // `brush/RgbDeposit` is order-independent too -- but the layer rounds to
  // binary16 once per *writing* dab, so a route with a per-dab rate takes a
  // different rounding path forwards and backwards and its tiles are not
  // bit-identical. This route writes each texel once, so they are.
  // `--selftest` asserts it at bit-identity for that reason.
  StrokeDeposit drawDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                         int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The alpha this stroke has laid at a document texel so far -- 0 for a texel
  // it has not reached. The accumulator's read side, exposed because it is
  // what `--selftest` asserts the ceiling against at zero tolerance: the
  // stored texel has been through binary16 once and the accumulator has not.
  float strokeAlphaAt(PixelCoord doc) const noexcept;

  float opacity() const noexcept { return opacity_; }
  bool alphaLocked() const noexcept { return alphaLocked_; }

  // What the accumulator currently holds. `--selftest` prints both, because
  // the memory claim ("allocated at pen-down, freed at pen-up") is worth
  // checking rather than trusting.
  size_t accumulatorTiles() const noexcept { return alpha_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return alpha_.tileBytes(); }

 private:
  std::array<float, 3> ink_{};
  float opacity_ = 1.0f;
  bool alphaLocked_ = false;
  bool active_ = false;
  StrokeAlphaStore alpha_;
};

}  // namespace np
