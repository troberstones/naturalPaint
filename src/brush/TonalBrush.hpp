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

// brush/TonalBrush -- **Dodge and Burn: moving the tone under the tip without
// moving how much paint is there.**
//
// ==========================================================================
// 0. Two tools, ONE engine, and why the direction is a parameter
// ==========================================================================
//
// Dodge lightens and Burn darkens. They are not two operations that happen to
// look alike: they are one operation whose only free variable is a sign, and
// the darkroom the names come from made them with the same lamp and the same
// print -- one gesture held light back, the other let more through. Building
// them as two modules would give this codebase two copies of §3's accumulator
// argument, one of which would be edited when a bug was found in the other,
// and the pair would drift in exactly the place a user compares them: a
// painter dodges an edge, decides it went too far, and burns it back.
//
// So `TonalDirection` is latched at `begin()` beside the strength (§3), and
// every line below runs once. This is the OPPOSITE call to the one
// `brush/RgbErase` §0 makes about the deposit, and deliberately so: that
// header rejects a `bool erasing` on `brush/RgbDeposit` because the two
// differ in *what one dab does to one texel* and in what the accumulator
// counts. Here the accumulator counts the same thing for both tools, in the
// same units, with the same cap and the same closed form -- and the per-texel
// arithmetic differs only in the sign of one exponent. There is no second
// invariant that would have to be stated in a place where half the calls do
// not obey it, which is that header's actual test.
//
// **Almost nothing else here is new.** `dabCoverage()`, `dabPixelBounds()`,
// `grainCoverageAt()`, `StrokeAlphaStore`, and the tile-major loop with its
// lazily-fetched write handle are borrowed unchanged from `brush/RgbErase`,
// including the reasons -- `brush/Deposit` §3 on reporting a tile in the same
// branch that writes it, `brush/RgbDeposit` §3 on why the accumulator is float
// and not half, `core/SelectionMask.hpp` on a hoisted loop owning its own null
// branch.
//
// **`BrushTip::linearRgb` and `BrushTip::pigment` are not read here**, the same
// mechanical form of the same decision `brush/RgbErase` §0 makes: a dodge that
// had a colour would be a brush painting white, which is the model that makes
// a "lightened" area read as a wash of paint instead of as the same paint under
// more light.
//
// ==========================================================================
// 1. The arithmetic, and the alpha invariant it exists to protect
// ==========================================================================
//
// **`core::Tile` stores PREMULTIPLIED (associated) alpha.** `core/TileStore.hpp`
// says so where the type is defined, and `core/Composite` reads a texel with
// `readPixel()` and blends it with no un-premultiply anywhere. A tonal op is a
// per-channel COLOUR op, and DESIGN-imaging.md §2 states the rule for those in
// a boxed warning: "Un-premultiply before any per-channel colour op. Running
// levels or curves on premultiplied data darkens partially-transparent pixels
// wrongly." So, per texel, with `f` the tone curve of §2:
//
//     a  = dst.a                                  // the coverage, untouched
//     c  = dst.rgb / a                            // un-premultiply
//     c' = f(c)                                   // the tonal shift, per channel
//     dst' = (c' * a, a)                          // re-premultiply; a COPIED
//
// **The alpha channel is copied, not recomputed**, and that is the whole of
// the invariant this module owes. A tonal op adjusts colour; it does not create
// or destroy coverage. Dodging a fully transparent texel must leave it fully
// transparent -- and here it does so for a reason stronger than "the formula
// happens to preserve it": `dst'.a` is literally `dst.a`, the same `float` that
// came out of `readPixel()`, so the stored binary16 word is unchanged
// bit-for-bit rather than recovered by rounding. `--selftest` asserts alpha
// equality at ZERO tolerance on an opaque texel, a half-covered one and an
// empty one, because a tonal op that leaked into alpha would look like a
// slightly soft edge and not like a bug.
//
// Two mistakes this rules out, both of which read as a colour-management defect
// somewhere else entirely rather than as a dodge tool:
//
//   * **Applying `f` to the premultiplied channels directly.** That is
//     DESIGN-imaging's boxed warning taken head-on: for a texel at alpha 0.25,
//     the stored red is already a quarter of the colour, so `f` sees a value
//     four stops down and applies the shift it would have applied to a shadow.
//     A soft brush edge would then dodge harder than the centre of the same
//     stroke, and the effect would be a *rim*, which is the one artefact a
//     painter uses dodge to remove.
//   * **Letting the shift touch alpha.** `dst' = f(dst)` on all four channels
//     is the one-line version of this module and it is wrong twice: it makes
//     dodge an eraser (a lightening curve raises alpha toward 1, painting
//     coverage the user never asked for) and it makes burn a soft eraser
//     (a darkening curve pulls alpha toward 0). `brush/RgbErase` is where
//     alpha is allowed to move, and it is a different tool.
//
// **A texel at alpha 0 is left completely alone** -- and this is the one place
// this module deliberately disagrees with `brush/RgbErase` §4, which erases a
// malformed `(colour, 0)` texel rather than declaring it absent. It is the same
// question with a different right answer: the eraser's job is to remove what is
// there, and a malformed texel holds something, so it removes it. A tonal op's
// job is to shift a COLOUR, and at alpha 0 the un-premultiply above has no
// value to shift -- `c = rgb / 0`. Inventing one (treating the stored rgb as
// straight, say) would make Dodge the only tool in the build that reads a
// premultiplied texel as unassociated, and it would resurrect the malformed
// texel at a *different* brightness instead of leaving the erase route's
// well-formed zero alone. So: no coverage, no colour, nothing to do.
//
// ==========================================================================
// 2. Which SPACE the tone curve lives in -- the decision this module is about
// ==========================================================================
//
// **This build's texels are linear light.** CONTEXT.md's "Working space" entry
// ("Linear-light RGBA, sRGB/Rec.709 primaries by default, stored
// `rgba16float`") and DESIGN-imaging.md §2 ("decode to a linear working space
// on import, do all work there") are unambiguous, and every averaging operation
// in this codebase depends on it. **The classic dodge/burn is defined on
// display-referred tone** -- a darkroom print's density, and in every editor
// since, a curve on the 0-255 the user can see.
//
// The two are not the same operation and picking one silently is the failure
// this section exists to prevent. **The decision: the tone curve is applied to
// the DISPLAY-REFERRED value**, i.e. `f = srgbDecode . g . srgbEncode` with `g`
// a power law on [0,1], and the consequences are these:
//
//   * **The curve is `d -> d^gamma` on the sRGB-encoded value, gamma from
//     §3.** Both endpoints are fixed: `0^g == 0` and `1^g == 1` exactly, for
//     every gamma. That is precisely what "dodge lightens without blowing the
//     whites, burn darkens without crushing the blacks" means, and it is why a
//     power law and not a multiply.
//
//   * **The same stroke is a DIFFERENT exposure change at different tones**,
//     and that is the tool rather than a wart. At full strength (gamma 1/2, §3)
//     a display midtone 0.5 becomes 0.7071, which in linear light is 0.21404 ->
//     0.45805: a 2.14x multiply. The same stroke on display 0.25 (linear
//     0.05087) gives display 0.5, linear 0.21404: a **4.21x** multiply. A
//     darkroom dodge behaves this way because density is logarithmic, and a
//     painter reaching for this tool is reaching for "open up the shadows",
//     not for "add 1.1 stops everywhere".
//
//   * **The rejected alternative is a linear-light MULTIPLY**, and it is
//     rejected on the top end rather than the bottom. `c -> c * m` is a pure
//     exposure change, which is a defensible tool and is not this one: `m` is
//     unbounded above, so dodging a texel at linear 1.0 (diffuse white) by the
//     2.14x above puts it at 2.14 -- outside the display range, indistinguishable
//     from white on screen and on every export, and *not recoverable by burning
//     it back*, because the burn the user then reaches for is the same multiply
//     with `1/m` and would have to be applied to the identical texels to undo
//     it. A painter dodging a highlight would watch it stop responding and
//     have no way to tell that the information was still in the file. The power
//     law simply cannot leave [0,1].
//
//   * **A power law nearly commutes with a power-law transfer function**, so
//     "linear or display" is a smaller difference here than the multiply case
//     makes it sound -- `(c^p)^(1/2.4)` and `(c^(1/2.4))^p` are the same
//     number. It is NOT the same operator, for two reasons that are exactly
//     where the choice bites: sRGB has a linear toe below 0.0031308 with its
//     own slope (color/Space.hpp), so the two disagree in the deep shadows,
//     which is where dodge is used; and linear 1.0 is only *diffuse* white in
//     this pipeline, not the top of the range, so a domain clamp written in
//     linear would clamp at the wrong place.
//
//   * **Outside [0,1] the curve is the IDENTITY, per channel**, and this is
//     stated rather than discovered. `color/Space.hpp` is explicit that
//     working-space values legitimately exceed 1.0 ("HDR-ish highlights") and
//     can go slightly negative, and `srgbEncode()` is deliberately unclamped,
//     so `d` outside [0,1] is a real input and not a defensive clause. `d^g`
//     there has the WRONG SIGN in both directions: for `d = 1.75` (linear 4.0),
//     a dodge exponent of 0.5 gives 1.32, which is *less* light where the user
//     asked for more, and a burn exponent of 2 gives 3.06, which is more where
//     the user asked for less. Neither is visible -- both still clip to white --
//     so the tool would silently rewrite the highlight headroom of every stroke
//     that crossed a specular. Leaving it alone is continuous at 1 (`1^g == 1`),
//     monotone, and honest: a value already past display white has no
//     display-referred tone to move.
//
// ==========================================================================
// 3. A stroke that crosses itself applies its shift ONCE -- the accumulator
// ==========================================================================
//
// `brush/RgbDeposit` §2 gives a stroke an opacity ceiling and `brush/RgbErase`
// §2 gives it an erasure floor, both for the same reason, and the reason is the
// same a third time: at the default 0.25-radius spacing a dab overlaps its
// neighbours about four deep, so ANY per-dab tonal shift compounds. A "50 %
// dodge" scrubbed back and forth over one spot would drive that spot to white,
// two crossing strokes would be brighter than either, and a slowly-drawn stroke
// would dodge harder than a quickly-drawn one over the identical path -- which
// is ADR-0003's speed dependence arriving through the back door.
//
// So the stroke carries a per-texel **tone accumulator** `T`, dimensionless in
// [0,1], starting at 0 at pen-down and thrown away at pen-up:
//
//     T' = min(cap, T + w * (1 - T))       // accumulate, capped
//     dT = T' - T                          // this dab's share
//     g  = kFullGamma ^ (+dT)   for Burn
//     g  = kFullGamma ^ (-dT)   for Dodge
//     d' = d ^ g                           // per channel, §2's domain
//
// with `w = flow * coverage` (selection already folded in, §4) and `cap =
// strength * s`.
//
// **The composition is EXACT, and that is why the accumulator can be a scalar
// rather than a copy of the layer.** A power law composes multiplicatively in
// the exponent -- `(d^g1)^g2 == d^(g1*g2)` -- so after dabs `dT_1..dT_N` the
// texel holds
//
//     d_final = d_0 ^ prod(kFullGamma^(±dT_i)) = d_0 ^ kFullGamma^(±sum dT_i)
//             = d_0 ^ kFullGamma^(±T_final)
//
// at any number of dabs and in any dab order, and `d_0` never has to be stored
// because the exponentiation *is* the memory. This is the same structural
// argument `brush/RgbErase` §2 makes about transparency ("both a source-over
// and a destination-out compose multiplicatively in transparency"), moved one
// operation up: a source-over composes in `1 - alpha`, a destination-out
// composes in `1 - alpha`, and a gamma composes in the exponent.
//
// **The rejected model, for the record, is the tempting one.** `d' = d^(
// kFullGamma^(±flow*cov*strength))` per dab with no memory of the ones before
// is one line shorter and dodges, and looks like dodging. Its exponent product
// over N overlapping dabs is `kFullGamma^(±N*flow*cov*strength)`, which is
// unbounded in N: at flow 0.35, strength 0.5 and 50 dabs -- the numbers
// `app/selftest/TonalBrush.cpp` actually runs -- a display midtone reaches
// `0.5^(2^-8.75)`, i.e. 0.9984, which is white. That number is computed in the
// suite on the identical inputs and asserted to be WRONG, so the good assertion
// cannot pass against the bad implementation.
//
// **The mirror of §2's rejected multiply, restated in the accumulator's
// terms**: because the exponent is `kFullGamma^(±T)` and not `1 ± T`, a stroke
// at strength 0 is the exact identity (`kFullGamma^0 == 1`, and `d^1 == d`),
// and dodge and burn at equal strength are exact inverses of one another
// (`g_dodge * g_burn == 1`). Both are asserted, the second because "burn it
// back" is the gesture a painter uses to check that a dodge went too far.
//
// **`kFullGamma == 2` -- one stop of gamma at full strength.** Chosen, not
// found: at strength 1 a display midtone goes to 0.7071 under Dodge and to
// 0.2500 under Burn, which is a strong but recoverable single stroke, and the
// number is memorable in the unit the operation is actually in (`g = 2^-T` is
// an exp2, exactly 1 at `T == 0` with no rounding). The visible asymmetry
// between those two numbers is inherent rather than a bug: density is
// logarithmic, so equal steps in the exponent are equal steps in density and
// unequal steps in reflectance, which is true of the darkroom too.
//
// **Strength is `BrushTip::opacity`**, the same slider on the same latch, for
// `brush/RgbErase` §2's stated reason: it already means "the fraction of the
// maximum effect one stroke may reach" on three routes, and a fourth route with
// its own private "exposure" number would leave the OPACITY control in the
// brush panel inert and undimmed while Dodge was selected.
//
// Four limits, each a real input rather than a defensive clause:
//
//   * `T -> 1`. The divisor `1 - T` never appears -- unlike `brush/RgbErase`,
//     whose per-dab alpha is a ratio, this module's per-dab quantity is a
//     DIFFERENCE, so there is no singular case to guard and `T == 1` is
//     ordinary.
//   * `T >= cap`. Skipped, and this is the ceiling doing its job: every dab
//     after it writes nothing at all, so a scrubbed stroke stops dirtying tiles
//     and live feedback stops re-uploading them.
//   * `flow > 1`. Deliberately not clamped, for `brush/Deposit`'s stated reason
//     ("a flow above 1 is a legitimate one dab saturates the paper tip"): the
//     `min` already caps `T'`. `strength` *is* clamped to [0,1], because
//     `kFullGamma^1.4` is not a meaning the OPACITY slider has.
//   * **The texel is unmoved by the shift.** Pure black and pure white are
//     fixed points of every power law (§2), so a dab over them computes a
//     result bit-identical to what is there. It is treated as a refusal --
//     nothing written, no tile unshared, no dirty tile -- see §5.
//
// ==========================================================================
// 4. The selection bounds the shift, exactly twice (PRD E1, P0)
// ==========================================================================
//
// "Every deposit and every op respects the active selection." `brush/RgbDeposit`
// §4 found by measurement that gating only the RATE is not a bound, and
// `brush/RgbErase` §3 repeats the finding; the identical argument applies here
// and the identical fix does. Per texel, with `s` the selection's coverage:
//
//     w   = flow * cov * s       // one dab shifts `s` of what it would
//     cap = strength * s         // and no number of dabs goes past `s`
//
// With the first alone, `T' = T + w(1 - T)` still converges to `strength` for
// *any* positive `w` -- it just takes longer -- so a half-selected texel would
// dodge all the way, and a feathered selection edge would come out hard for a
// slow stroke and soft for a fast one. With both, a half-selected texel's
// exponent stops at `kFullGamma^(±strength/2)` against any amount of scrubbing
// within one stroke.
//
// `nullptr` means "no restriction" and 1.0 everywhere, which is
// core/SelectionMask.hpp's convention and NOT the inverse. That header requires
// every hoisted per-texel loop to own its own copy of the null branch, and
// warns that a perturbation inverting one copy leaves the others right; this is
// one such loop, `--selftest` drives both nulls through it, and an engaged
// selection with no tile at a coordinate skips that tile before anything is
// looked up.
//
// ==========================================================================
// 5. Toning nothing must COST nothing
// ==========================================================================
//
// `ops/FloodFill.cpp` states the asymmetry for its own pair and `brush/RgbErase`
// §4 for its own, and this module sits on the same side as both: a tonal shift
// can only modify a colour that is already there, so a tile that does not exist
// has nothing to shift.
//
//   * **An absent layer tile is skipped whole**, before `getOrCreate()`. A
//     224 KiB tile per tile of blank canvas the tool passes over would be a tool
//     that *grows* the document by being used on nothing, and every one of those
//     tiles would then be reported dirty and re-uploaded every frame of the drag.
//   * **A texel with no coverage is skipped**, inside the tile -- §1's rule,
//     which is a statement about what a tonal op means and not only a
//     divide-by-zero guard.
//   * **A texel the shift does not actually move is skipped**, by comparing the
//     computed four channels against the four that are there. That covers the
//     fixed points of §3's last bullet and the tail of the falloff where the
//     shift rounds away in binary16, and it is a comparison rather than a
//     special case so that a future curve inherits it.
//
// The visible consequence, and the one `--selftest` asserts: **a stroke that
// changed nothing records nothing.** No tiles reported, no revision bump, no
// history entry -- `app/StrokeSession` §2's rule reached by arithmetic rather
// than by a special case, because an undo step that undoes nothing is a worse
// defect than a missing one.
//
// ==========================================================================
// 6. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the same
// boundary `brush/Deposit`, `brush/RgbDeposit` and `brush/RgbErase` all draw,
// for the same reason: this is the arithmetic of a tonal shift against a
// `core::TileStore`, and the stroke lifecycle belongs to `app/StrokeSession`.
//
// **No tonal RANGE targeting.** Photoshop's dodge and burn take a
// Shadows/Midtones/Highlights selector that weights the shift by the texel's
// own luminance, and this build has one range: everything. Named rather than
// left to be discovered, because it is the first thing a user of the other tool
// will look for -- and because the shape it would take is already decided by
// §3: a range weight belongs in `w` and in `cap`, exactly where `s` goes, so it
// is a third factor on two lines and not a new accumulator.
//
// **No Protect Tones**, Photoshop's saturation-preserving variant, for the same
// reason and in the same place: it is a different `f`, not a different stroke.
//
// **No Pigment, Media, Strokes or mask row.** `app/StrokeSession` §1's table
// carries the argument for each refusal; the Pigment one is a decision rather
// than a gap and that header states it.
//
// **No blend mode.** `Layer::blend` still applies to the layer as a whole at
// composite time and is untouched. Dodging a layer whose blend is Multiply
// lightens its paint; it does not lighten the multiply.
namespace np {

// Which way the stroke moves the tone. Latched at `begin()` beside the
// strength (§3), never per dab: a stroke whose direction flipped half way
// through would have an accumulator counting two different things, and its
// closed form -- the whole reason the accumulator is a scalar -- would name a
// quantity that never existed.
enum class TonalDirection {
  Dodge,  // lightens; exponent kFullGamma^-T
  Burn,   // darkens;  exponent kFullGamma^+T
};

const char* tonalDirectionName(TonalDirection dir) noexcept;

// §3. One stop of gamma at full strength, in the unit the operation is in.
// A display midtone 0.5 lands at 0.7071 under a full-strength Dodge and at
// 0.2500 under a full-strength Burn.
inline constexpr float kTonalFullGamma = 2.0f;

// The tone curve of §2, on ONE display-referred channel value: `d^gamma`
// inside [0,1], the identity outside it. Exposed because §2's domain rule is
// the decision this module is about, and `--selftest` asserts it on the curve
// itself rather than only on a tile of texels that happened to sample it.
//
// Total: defined for every finite input, including `d <= 0`, `d >= 1` and a
// non-positive gamma, each of which returns `d` unchanged.
float tonalCurve(float displayValue, float gamma) noexcept;

// §3's rule, as a pure function of one texel, for the one reason a pure
// function earns its keep here: the invariants are about *this arithmetic*, so
// `--selftest` asserts them on this and not on a tile of it.
//
// `dst` is the stored PREMULTIPLIED texel; `strokeTone` is `T`, the fraction of
// this stroke's shift already applied here; `weight` is `flow * coverage` with
// the selection already folded in; `strength` is the stroke's cap on `T`.
//
// Total: defined for every finite input, including `T >= strength`, `weight <=
// 0`, `strength <= 0`, an alpha of 0 and a texel the curve does not move, each
// of which returns `dst` **bit-identical** with `changed == false` -- the
// caller's signal to skip the texel entirely rather than write a value equal to
// the one already there. Bit-identical rather than recomputed for
// `depositRgbTexel()`'s stated reason: a stroke that has reached its ceiling
// must stop perturbing the tile it is scrubbing over, or the caller's "nothing
// to do here" test never fires and the tile is re-uploaded on every frame of a
// drag that is changing nothing.
struct TonalStep {
  std::array<float, 4> premultiplied{};  // the texel to store
  float strokeTone = 0.0f;               // T', to put back in the accumulator
  float dabGamma = 1.0f;                 // g, this dab's exponent; 1 is the identity
  bool changed = false;                  // false means "nothing to do here"
};
TonalStep toneRgbTexel(const std::array<float, 4>& dst, float strokeTone, float weight,
                       float strength, TonalDirection direction) noexcept;

// One tonal stroke in flight: the latched direction and strength, and the
// accumulator that makes the stroke's shift apply once however far it crosses
// itself.
//
// Deliberately a small object with an explicit lifetime rather than a free
// function taking a `StrokeAlphaStore&`, for `RgbStroke`'s and
// `RgbEraseStroke`'s stated reason: the accumulator is only correct against the
// strength AND the direction it was started with (§3), so binding all three
// together at `begin()` makes the combinations that can go wrong -- an
// accumulator from one stroke with the strength or the sign of another --
// unspellable.
//
// **The accumulator is `StrokeAlphaStore`, borrowed from brush/RgbDeposit and
// not re-declared**, exactly as `brush/RgbErase` borrows it and for the same
// argument: it is the right shape (sparse, allocate-on-write,
// query-without-allocating, keyed by the same `TileCoord` as the layer's tiles)
// and the right precision (float, not half -- that header's §3 derives the
// `N * 2^-11` drift a half accumulator would put on the ceiling, and the drift
// lands on this ceiling for the same reason). What differs is only what the
// number *means*: `A` is alpha added, `E` is the fraction removed, `T` is the
// fraction of this stroke's tonal shift applied. All three start at 0, all
// three are capped, all three are dimensionless in [0,1] -- so a third 64 KiB
// tile type would be a copy with a different comment on it.
class TonalStroke {
 public:
  // Pen-down. Latches the direction and the strength and clears any
  // accumulator a previous stroke left, exactly as `StrokePath::reset()` clears
  // leftover arc length and for the same reason: tone carried across strokes
  // would let the ceiling of the last stroke stop the first dab of the next --
  // so a second pass would refuse to go further, which is the one thing §3 says
  // a second pass must be able to do.
  //
  // `strength` is clamped to [0,1]; a non-positive one leaves a stroke that
  // shifts nothing, which is a legitimate setting and not an error.
  void begin(float strength, TonalDirection direction) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator and leaves `strength()`/`direction()` alone,
  // so the counts below still read correctly after a stroke ends.
  void end() noexcept;

  // Applies one dab to `store`, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, §4).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to fold
  // with `sortUniqueTiles()`.
  //
  // A dab that lands on blank canvas, or all of whose texels have reached the
  // ceiling, writes nothing, allocates nothing and reports no tiles (§5).
  DepositCount toneDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                       int32_t canvasH, const Selection* selection,
                       std::vector<TileCoord>* touchedOut);

  // Applies every dab in `dabs`, in order. Order does not change the CLOSED
  // FORM (§3's exponent product is commutative), but it does change the
  // intermediate binary16 roundings, so the two are not bit-identical and this
  // is not a claim that they are.
  StrokeDeposit toneDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                         int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The fraction of this stroke's shift applied at a document texel so far -- 0
  // for a texel it has not reached. The accumulator's read side, exposed because
  // it is what `--selftest` asserts the ceiling against at zero tolerance: the
  // stored texel has been through binary16 once per dab and the accumulator has
  // not.
  float strokeToneAt(PixelCoord doc) const noexcept;

  float strength() const noexcept { return strength_; }
  TonalDirection direction() const noexcept { return direction_; }

  // This stroke's exponent at full accumulation, `kTonalFullGamma^(±strength)`
  // -- the closed form of §3 with `T == strength`. `--selftest` asserts the
  // stored texel against this rather than against a number typed twice.
  float ceilingGamma() const noexcept;

  // What the accumulator currently holds. `--selftest` prints both, because the
  // memory claim ("allocated at pen-down, freed at pen-up") is worth checking
  // rather than trusting.
  size_t accumulatorTiles() const noexcept { return toned_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return toned_.tileBytes(); }

 private:
  TonalDirection direction_ = TonalDirection::Dodge;
  float strength_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore toned_;
};

}  // namespace np
