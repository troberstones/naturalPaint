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

// brush/RgbDeposit -- **painting on a plain RGB layer.**
//
// ==========================================================================
// 0. What this is, and what it is not a second copy of
// ==========================================================================
//
// `brush/Deposit` deposits *pigment*: latent weights plus a mass, into a
// `PigmentTileStore`, mixed by Kubelka-Munk. That is the medium this
// application exists for, and none of it applies to a `LayerKind::RGB` layer,
// whose texel is four half floats of premultiplied linear light and has no
// mass, no latent and no notion of mixing at all. Until this module a brush
// aimed at an RGB layer had nowhere to go, and `strokeRouteFor()` sent it to
// the solver canvas instead -- paint appeared, on something that was not the
// layer the user had selected. That is the defect this closes, and the routing
// half of the fix is in `app/StrokeSession`.
//
// **Almost nothing here is new.** The dab stream is `brush/StrokePath`'s, the
// falloff is `dabCoverage()`, the footprint is `dabPixelBounds()`, the spacing
// is `BrushTip::spacingPx()`, and the tile-major loop with its lazily fetched
// tile is `depositDab()`'s, line for line, including the reason (its §3: a
// tile is reported at the moment its first changed texel is written, so
// reporting and writing are the same branch and cannot disagree). Those pieces
// were already kind-agnostic. Three things are genuinely different: what one
// dab does to one texel, the per-stroke accumulator that decides it, and --
// since the brush's own `Md ` blend mode landed (§2a) -- which pre-stroke
// texel that composite reads.
//
// ==========================================================================
// 1. Premultiplied, and linear. Both, and neither is optional
// ==========================================================================
//
// **`core::Tile` stores premultiplied ("associated") alpha.** core/TileStore.hpp
// says so where the type is defined ("rgba16float, premultiplied alpha"),
// `core/Composite` reads a texel with `readPixel()` and blends it with no
// un-premultiply anywhere, `ops/Filters`' whole spatial pass takes an explicit
// premultiplied-alpha argument, and `fillThroughSelection()` -- the paint
// bucket, the closest existing writer to this one -- premultiplies its caller's
// straight colour once, outside every loop, and then blends all four channels
// with the *same* `keep` factor. This module writes the identical shape of
// texel, and §2's composite is exactly that bucket's `out = s' + dst*(1-a)`.
//
// The consequence worth stating, because it is what "associated" buys: RGB and
// alpha scale together, so a texel at the soft rim of a dab is half *present*
// rather than half *bright*, and a feathered edge has no fringe. Storing
// straight alpha and dividing at the composite would put a division by a
// near-zero alpha at every rim texel of every dab.
//
// **The colour is linear; the palette is not.** `paint/Palette`'s `rgb` is
// display-referred sRGB -- it is drawn straight into an 8-bit swatch and handed
// raw to the Mixbox LUT, whose API is sRGB -- while a document part holds
// scene-referred linear data (DESIGN-imaging.md, PRD B6). `BrushTip::linearRgb`
// is the decoded value and `app/StrokeSession`'s `brushTipFor()` is the one
// place the decode happens, beside the `rgbToLatent()` call that consumes the
// same palette entry for the other route. Skipping it lands every stroke at
// roughly half the swatch's brightness, which reads as a colour-management bug
// somewhere else entirely rather than as a missing one-line conversion --
// exactly the failure `ui/MacPaintUI`'s `foregroundLinearRgba()` was pulled out
// to prevent for the bucket and the gradient. `--selftest` asserts the tip's
// colour *equals* that function's, so the two cannot drift.
//
// ==========================================================================
// 2. Flow and opacity are different quantities, and this is the whole module
// ==========================================================================
//
// This is the decision that has to be right, and the one that is easiest to get
// plausibly wrong -- because the wrong version paints, and looks like paint.
//
//   **Flow** is how much a *single dab* lays down. `BrushTip::flow`, the same
//   number the pigment route calls "mass laid down per dab where coverage is
//   1", which is `BrushState::native.load` (brush/NativeBrush.hpp), scaled
//   per stroke by the model's Transfer Flow Variance (`app/StrokeSession`'s
//   `transferFlowMul_`). This used to read "`BrushState::load` scaled by the
//   DYNAMICS matrix": the field moved into `NativeBrush`, and the matrix is
//   shelved (`ui/DynamicsMatrixPanel.hpp`) -- nothing that paints reads it.
//
//   **Opacity** is the ceiling a *single stroke* can reach, no matter how many
//   dabs it spends getting there. `BrushTip::opacity`, latched at pen-down.
//
// The tempting implementation applies opacity per dab -- `a = flow * cov *
// opacity`, or `a = min(opacity, flow * cov)` -- and it is wrong in a way a
// still picture cannot show. At the default spacing of 0.25 radii a dab
// overlaps its neighbours about four deep, and a stroke drawn slowly is sampled
// no differently from a fast one (ADR-0003 makes the *dab* set depend on
// distance alone) but a stroke drawn *back over itself* is. So under a per-dab
// opacity:
//
//   * no setting ever produces a flat 50 % pass -- every overlap compounds, so
//     a stroke at "50 %" reaches 75 %, then 87.5 %, then 1;
//   * the crossing of two strokes is darker than either, at every setting,
//     including the setting the user picked precisely to stop that happening;
//   * "opacity" and "flow" become the same slider with two names.
//
// That is the difference between a brush engine and a dab stamper, and every
// application that ships a brush makes it.
//
// **The fix is a per-stroke alpha accumulator.** `A` is how much alpha *this
// stroke* has already laid at this texel; it starts at 0 at pen-down, is
// remembered across dabs, and is thrown away at pen-up. Per dab, with `w =
// flow * coverage` (coverage already gated by the selection, §4):
//
//     A' = min(opacity, A + w * (1 - A))     // accumulate, capped at opacity
//     a  = (A' - A) / (1 - A)                // this dab's composite alpha
//
// and `a` is then an ordinary source-over of an opaque source into the layer:
//
//     dst' = (rgb * a, a) + dst * (1 - a)    // premultiplied, all four channels
//
// **`a` is exact rather than approximate, and that is worth spelling out.**
// Repeated source-over composes in the transparency, not in the alpha: after
// two passes at `a1` and `a2` the destination retains `(1-a1)(1-a2)` of what it
// held. So the `a` for which one more pass lands the stroke's total at exactly
// `A'` is the one satisfying `1 - A' = (1 - A)(1 - a)`, which rearranges to the
// second line above. It is an identity, not a fit. The one assumption it needs
// is that **the colour does not change during the stroke** -- if it did, the
// incremental composite would be a weighted history of several colours and the
// single scalar `A` could not describe it. That is why `begin()` latches the
// colour rather than reading it off the tip each dab, and why `opacity` is
// latched with it: both are per-stroke properties, and a stroke whose ceiling
// moved half way through has no well-defined ceiling.
//
// Three limits, each a real input rather than a defensive clause:
//
//   * `A -> 1`. The divisor is `1 - A`. At `A == 1` the texel is already
//     opaque, there is nothing left to add, and the dab is skipped -- which is
//     the correct answer *and* keeps the division out of the singular case. A
//     stroke at opacity 1 reaches it in ordinary use, so this is the common
//     path's own end state, not an edge case.
//   * `A >= opacity`. Also skipped, and this is the cap doing its job: every
//     dab after the ceiling is reached writes nothing at all, so a slow stroke
//     and a fast one land in the same place instead of the slow one being
//     darker.
//   * `flow > 1`. Deliberately not clamped, for `brush/Deposit`'s stated reason
//     ("a flow above 1 is a legitimate one dab saturates the paper tip"): the
//     `min` already caps `A'`, so a flow of 2.5 simply means one dab reaches
//     the ceiling. `opacity` *is* clamped to [0,1], because an alpha above 1 is
//     not a meaning this or any other compositor has.
//
// ==========================================================================
// 2a. The brush's OWN blend mode (`Md `) -- STROKE-level, never per dab
// ==========================================================================
//
// `BrushTip::blend` (brush/Deposit.hpp), set by `blendModeFromPsToolOptions()`
// (brush/ToolOptionsBlend.hpp) from a `.abr`'s tool-options `Md ` id, is read
// here and nowhere else -- see that header and `BrushTip::blend`'s own
// comment for why the pigment and erase routes refuse it by name.
//
// **Photoshop applies a brush's blend mode to the STROKE as a unit, against
// the layer as it stood before the stroke began -- not per dab against
// whatever the layer already holds.** Reusing the per-dab source-over loop
// §2 already has, unmodified, gets this wrong: dab 2 would multiply the
// ALREADY-multiplied result dab 1 left, so an overlapping stroke at 50 % flow
// would come out darker than a single stroke-level Multiply at flow 1 -- the
// exact "flow and opacity become the same slider" defect §2 spends five
// paragraphs refusing, wearing a different hat.
//
// So the composite is written directly against the texel latched at the
// stroke's first touch (`dst0`, §3) and the stroke's own running ceiling
// (`A'`, §2's `a1`), never against an intermediate write:
//
//     target = blend(straight(dst0), ink)          -- core/Blend.hpp's
//                                                       blendPixel(), §2a's
//                                                       own note below
//     out.rgb = target * A'  +  dst0.rgb * (1 - A')
//     out.a   =        A'  +  dst0.a   * (1 - A')
//
// `A'` here is `depositRgbTexel()`'s own `a1` -- the CUMULATIVE stroke alpha
// after this dab, not the per-dab increment `a` -- because the claim is
// about the whole stroke's composite against `dst0`, recomputed fresh every
// dab from the two quantities that do not change dab to dab (`dst0`) or that
// already carry the whole stroke's history (`A'`). Recomputing from `A'`
// rather than composing `a` onto a live value is also what makes the result
// **order-independent within the stroke**: two dabs at flow 0.5 that both
// reach `A' = 1` and one dab at flow 1 that reaches it directly write the
// IDENTICAL texel, because the formula above only ever looks at the pair
// `(dst0, A')`, never at how many dabs it took to get there.
//
// **`blendPixel()` already carries the "blend over a transparent destination
// is the source colour" rule, so this file does not re-derive it.** Calling
// it with an OPAQUE source (`ink` at alpha 1) collapses its three-term
// Porter-Duff split (core/Blend.hpp's own derivation) to exactly
// `lerp(ink, blend(straight(dst0), ink), dst0.a)` -- verified algebraically
// here for `Multiply` and `Min` (both commutative in the two arguments the
// split passes them) and asserted by `--selftest` at `dst0.a == 0`, where it
// must equal `ink` exactly.
//
// For `BlendMode::Normal`, `target == ink` and the formula above is
// algebraically `depositRgbTexel()`'s own §2 composite -- but **Normal never
// reaches this formula**: `RgbStroke::depositDab()` calls this composite
// only when the stroke's blend is not Normal, so the Normal path is the
// exact code and the exact floating-point sequence it always was, not a
// specialisation of this one that happens to agree. `--selftest` asserts
// that by construction (dispatch, not arithmetic) as well as by comparing a
// Normal stroke's stored bytes against the pre-existing path's.
//
// **Alpha lock re-derived** (§4.5 is the unblended case; this is the same
// argument with the blend's `target` standing in for `ink`). §4.5's per-dab
// rule is a straight-colour lerp toward a CONSTANT target at the per-dab
// rate; iterated, it is the identical "repeated composite of a constant"
// identity §2 already uses for alpha, just at the straight-colour level, so
// it closes the same way -- cumulative `A'` in place of the per-dab
// increment, `dst0` in place of the live value:
//
//     out.rgb = dst0.rgb * (1 - A')  +  target * A' * dst0.a
//     out.a   = dst0.a                                          (frozen)
//
// which is §4.5's own `dst.rgb*(1-a) + ink*a*dst.a` / `dst.a` with exactly
// those two substitutions, and reduces to it when `target == ink`, `dst0 ==
// dst` and `A' == a` -- the unblended, single-dab case.
//
// **Only `Normal`, `Multiply` and `Min` (Darken) are reachable here.**
// `blendModeFromPsToolOptions()` refuses `linearBurn` and `Dslv` by name
// (brush/ToolOptionsBlend.hpp), so `BrushTip::blend` never holds anything
// this composite has not been checked against; `RgbStroke` does not switch
// on the mode at all, it hands whatever `core::BlendMode` it was given
// straight to `blendPixel()`, which is defined for every mode in the enum.
//
// ==========================================================================
// 3. Where the accumulator lives, and what it costs
// ==========================================================================
//
// `A` is per texel and a stroke is sparse, so the accumulator is a tile store
// of its own -- the same `TileStoreOf<T>` template every other store in this
// codebase is an alias of, instantiated on a plain float tile. Allocate on
// write, query without allocating, iterate only what exists: exactly the three
// properties a stroke's scratch alpha needs, already written and already
// tested, and using it means the accumulator's tiles are keyed by the *same*
// `TileCoord` as the layer's, so the deposit loop hoists both lookups out of
// the texel loop together.
//
// **Float, not half.** 64 KiB per touched tile against 32 KiB, and the extra
// 32 KiB buys the thing the model is judged on: `A` is a running accumulation
// over up to hundreds of dabs, and rounding it to binary16 after every one
// would make the ceiling drift by roughly `N * 2^-11` -- visible as a stroke
// that stops slightly short of, or slightly past, the opacity that was asked
// for, and different for a slow stroke than a fast one, which is the exact
// symptom this module exists to remove. The *layer* still rounds to half at
// every write, because that is what the document stores; the accumulator is the
// one place the exact answer is kept, and `--selftest` asserts the cap against
// it at zero tolerance and against the stored texel at a derived f16 bound.
//
// **Allocated at pen-down, freed at pen-up.** `begin()` starts with no tiles
// and `end()` drops them all; a stroke across a 4K canvas touching 30 tiles
// holds 1.9 MiB for the duration of one drag and nothing afterwards. It is
// never copied out of the stroke that owns it, which is what makes the
// `getOrCreate()` in the deposit loop free of the copy-on-write barrier's copy.
//
// **§2a's second store, and why it costs half of this one rather than the
// same amount again.** A blended stroke needs `dst0` -- the texel as it
// stood before the stroke touched it -- and `dst` alone cannot recover that
// (`dst = dst0*(1-A') + target*A'` is not invertible once `target != dst0`),
// so it is latched into a second sparse tile store, `RgbStroke::dst0_`, the
// moment a texel is first written this stroke (the same instant `alpha_`
// gains its first non-zero entry there -- §2a's dispatch reads that as the
// latch signal rather than adding a second one). It reuses `core::Tile`
// itself as the element type rather than a bespoke struct: `dst0` is read
// out of the LAYER, which already rounds every channel to half on write
// (core/TileStore.hpp), so storing it at float would spend 64 KiB claiming a
// precision the source value never had, and `core::Tile` is already exactly
// the "128x128 texels, four half channels, premultiplied" shape this needs
// -- its own `static_assert(sizeof(Tile) == 128 * 1024)` is what keeps this
// half-plane choice honest, with no second assertion to duplicate it.
// Contrast `StrokeAlphaTile`'s choice of float over half (above): `A` is
// accumulated into, hundreds of times, so its rounding error compounds and
// float is what damps that; `dst0` is written ONCE per texel per stroke and
// only ever read after, so there is no accumulation to damp and half is the
// honest cost. **Allocated only when `blend_ != BlendMode::Normal`** -- a
// Normal stroke's `dst0_` stays completely empty (0 tiles, 0 bytes), which
// is what keeps the Normal path's memory shape, and `--selftest`'s
// tile-byte-count assertion for it, exactly what they were before §2a.
// Freed at `end()` alongside `alpha_`, for the identical reason.
//
// ==========================================================================
// 4. The selection bounds the deposit (PRD E1, P0)
// ==========================================================================
//
// "Every deposit and every op respects the active selection", and a brush that
// ignored it would be the one tool in the build that painted outside the
// marching ants -- the paint bucket already intersects its fill region with the
// selection for exactly this reason.
//
// **The selection enters the rule twice, and the second one is the one that
// makes it a bound.** Per texel, with `s` the selection's coverage there:
//
//     w   = flow * cov * s          // one dab lays `s` of what it would have
//     cap = opacity * s             // and no number of dabs goes past `s`
//
// The first multiply is the obvious one and is exactly what the paint bucket
// does (`weight = coverage * opacity`, one pass). The second was **found rather
// than designed**: with the first alone, a half-selected texel still climbs to
// the full `opacity`, because the accumulator has no memory of the selection
// and `A' = A + w(1-A)` converges to the ceiling for *any* positive `w`. It
// takes longer to get there, which is the tell -- a stroke drawn slowly through
// a feathered selection came out with a harder edge than the same stroke drawn
// quickly, which is precisely the speed dependence §2 exists to remove, wearing
// a different hat. A bound a scrubbing brush can walk through is a speed limit,
// not a bound, and PRD E1 asks for a bound.
//
// With both, a feathered selection edge survives any amount of scrubbing
// *within one stroke*, and the stroke's alpha there converges to `opacity * s`.
// It does **not** survive repeated separate strokes -- each new stroke starts
// its accumulator at 0 and composites over what the last one left, so passes 1
// and 2 through a half-selected texel reach 0.5 and then 0.75. That is the same
// thing a second stroke does anywhere else, it is what every editor does, and
// making it otherwise would need the selection to be a mask on the *layer*
// rather than a bound on the deposit.
//
// `nullptr` means "no restriction" and 1.0 everywhere, which is
// core/SelectionMask.hpp's convention and NOT the inverse (a caller who writes
// `sel ? cov : 0` has inverted the editor). That header also warns that a
// per-texel loop cannot afford a hash lookup per texel and must therefore hoist
// the tile and own the null branch itself, and name any loop that does so: this
// is one. The tile is hoisted per tile coordinate, and an engaged selection
// with no tile at that coordinate skips the whole tile before anything is
// allocated.
//
// ==========================================================================
// 4.5. Alpha lock freezes `dst.a`; it is a FREEZE, not another bound
// ==========================================================================
//
// Photoshop's "Lock transparent pixels", core/Layer.hpp's `alphaLocked`: the
// layer's alpha stops moving and its colour keeps changing. It is tempting to
// reach for the same tool §4 already built -- multiply the weight by one more
// factor, here `dst.a` -- because that is what "the selection enters twice"
// already looks like. **That is the wrong rule, and the reason is the same
// one §4 spends four paragraphs on: a multiplicative factor is a BOUND, and a
// bound is something a second stroke can climb past.** `sel * dst.a` on the
// weight would still let the stroke's own accumulator drive `dst.a` upward --
// one pass reaches `dst.a0 * a`, a second pass composites over THAT and
// reaches further, exactly the "0.5, then 0.75" creep §4 measures for the
// selection and rejects for it. A lock that crept was never a lock.
//
// **The fix is not a bound on the input, it is a different composite on the
// output**, and it needs no per-dab memory at all: whatever alpha `a` this
// dab would have contributed is spent on colour only, and the stored alpha is
// copied through unchanged.
//
//     out.rgb = dst.rgb * (1 - a) + ink * a * dst.a
//     out.a   = dst.a
//
// Read at the *straight* colour it is built from: `dst.rgb / dst.a` is the
// texel's current straight colour (where `dst.a > 0`), `out.rgb / dst.a` is
// `straight * (1 - a) + ink * a` -- an ordinary lerp toward the ink, at the
// SAME `a` the unlocked route would have composited with -- and multiplying
// back through by `dst.a` (which does not change) is what keeps the texel
// premultiplied. So an alpha-locked dab paints exactly like an unlocked one
// felt to the eye, except the rim of the dab does not grow the shape.
//
// **At `dst.a == 0` this yields nothing, with no branch needed**: `dst.rgb`
// is `0` there (premultiplied, §1), so `dst.rgb * (1 - a)` is `0`, and
// `ink * a * dst.a` is `ink * a * 0`, also `0`. `out.a` is `dst.a`, `0`
// again. The whole texel comes back bit-identical to `dst` -- "no paint on
// transparent texels" is not a case this rule handles, it is a value this
// rule's own arithmetic already produces.
//
// **`a` itself is untouched -- this changes only the last two lines of
// `depositRgbTexel()`.** The accumulator, the ceiling, the selection's two
// entries into `weight` and `cap`: none of it knows or needs to know the
// layer is locked. `a` still means "how much of this stroke's opacity budget
// this dab spends", and alpha lock only changes what that budget is spent
// ON -- alpha when unlocked, colour alone when locked. That is also why the
// eraser cannot simply run this rule with `ink` set to "nothing": erasing
// *is* moving alpha, brush/RgbErase.hpp's whole subject, and there is no dab
// in this rule that does that -- which is the point, and why
// `app/StrokeSession.cpp`'s `strokeRouteFor()` refuses `StrokeRoute::RgbErase`
// on an alpha-locked layer by name rather than routing it here.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`.** Same boundary
// `brush/Deposit` draws and for the same reason: this is the arithmetic of
// deposition against a `core::TileStore`, and the stroke lifecycle -- pen-down,
// live feedback, one undo step at pen-up, which tool routes here at all --
// belongs to `app/StrokeSession`, which owns the record and the history that
// `app/` owns.
//
// **No PER-DAB blend mode, no smudge, no texture.** §2a's blend mode is a
// STROKE-level composite against the texel latched before the stroke began,
// never a per-dab operation against the live layer -- a brush that picked its
// own blend mode per dab is the feature this section used to say did not
// exist, and it still does not: the per-dab loop composites `weight`/`A'`
// exactly as §2 always did, and only the final write is where §2a's formula
// stands in for §2's. `Layer::blend` still applies to the layer as a whole at
// composite time and is untouched, and is a different mode vocabulary
// entirely (`core::BlendMode` is shared machinery, not a shared setting).
//
// **No eraser -- it is `brush/RgbErase`, a sibling.** That module borrows this
// one's dab stream, falloff, footprint, tile loop and accumulator *type*, and
// differs in the two places that matter: a dab is a destination-out rather than
// a source-over, and the accumulator holds the fraction this stroke has REMOVED
// rather than the alpha it has added -- so §2's ceiling becomes a floor of
// `alpha_0 * (1 - strength)` there. A `bool erasing` parameter on
// `depositRgbTexel()` was the alternative and was rejected: it would put both
// arithmetics inside one function whose §2 argument describes only one of them.
//
// **No fluid behaviour at all**, exactly as `brush/Deposit` says of itself: no
// water, no diffusion, no edge darkening, no granulation, no paper tooth. An
// RGB layer has nowhere to keep any of it.
namespace np {

// One tile's worth of per-stroke accumulated alpha -- §3.
//
// Nothing but its buffer, the same discipline `core::Tile`, `core::PigmentTile`,
// `core::MaskTile` and `core::SelectionTile` each keep, so the static_assert
// below is a real check rather than a decoration.
struct StrokeAlphaTile {
  static constexpr size_t kTexelCount =
      static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize);

  // Zero -- "this stroke has laid nothing here yet" -- which is the correct
  // implicit content of a tile the stroke has not reached, and is also what an
  // *absent* tile means, so the two agree and a miss needs no allocation.
  std::array<float, kTexelCount> alpha{};

  float at(PixelCoord local) const noexcept { return alpha[index(local)]; }
  void set(PixelCoord local, float v) noexcept { alpha[index(local)] = v; }

 private:
  static size_t index(PixelCoord local) noexcept {
    return static_cast<size_t>(local.y) * static_cast<size_t>(kTileSize) +
           static_cast<size_t>(local.x);
  }
};

static_assert(sizeof(StrokeAlphaTile) == 64 * 1024,
              "one 128x128 float stroke-alpha tile must be exactly 64 KiB -- twice a MaskTile "
              "and half a core::Tile (this header's section 3 says why float, not f16)");

using StrokeAlphaStore = TileStoreOf<StrokeAlphaTile>;

// §2a/§3's second store: one sparse `core::Tile` per touched tile, holding
// `dst0` -- the texel latched at this stroke's first touch, before any
// blended composite wrote to it. Reusing `core::Tile` rather than a bespoke
// struct is what keeps its own `static_assert(sizeof(Tile) == 128 * 1024)`
// (core/TileStore.hpp) the one honest size check this needs; see §3's "half,
// not float" note for why that size, not `StrokeAlphaTile`'s 64 KiB float
// one, is the right cost here. Only populated for a stroke whose `blend !=
// BlendMode::Normal` -- a Normal stroke's store stays at 0 tiles, 0 bytes.
using StrokeDst0Store = TileStoreOf<Tile>;

// §2's rule, as a pure function of one texel, for the one reason a pure
// function earns its keep here: the invariants are about *this arithmetic*, so
// `--selftest` asserts them on this and not on a tile of it.
//
// `dst` is the stored PREMULTIPLIED texel; `straightLinearRgb` is the ink's
// STRAIGHT linear colour (an opaque source -- the stroke's alpha is `a`, not
// the ink's); `strokeAlpha` is `A`; `weight` is `flow * coverage` with the
// selection already folded in; `opacity` is the stroke's ceiling.
//
// Total: defined for every finite input, including `A >= 1`, `A >= opacity`,
// `weight <= 0` and `opacity <= 0`, each of which returns `dst` unchanged with
// `dabAlpha == 0` -- the caller's signal to skip the texel entirely rather than
// write a value equal to the one already there.
struct RgbDepositStep {
  std::array<float, 4> premultiplied{};  // the texel to store
  float strokeAlpha = 0.0f;              // A', to put back in the accumulator
  float dabAlpha = 0.0f;                 // a; 0 means "nothing to do here"
};
// `alphaLocked` selects §4.5's composite over §2's: `a`, the accumulator
// arithmetic and every one of the four refusals above are IDENTICAL either
// way, because none of them is a statement about where `a` ends up spent.
// Only the last two lines -- what gets written for `premultiplied` -- differ.
RgbDepositStep depositRgbTexel(const std::array<float, 4>& dst,
                               const std::array<float, 3>& straightLinearRgb, float strokeAlpha,
                               float weight, float opacity, bool alphaLocked = false) noexcept;

// §2a's composite: the brush's own STROKE-level blend mode. `dst0` is the
// texel LATCHED at this stroke's first touch -- never a live/intermediate
// value, which is what makes this exact rather than compounding -- `blend`
// selects `Normal`/`Multiply`/`Min` (the three `blendModeFromPsToolOptions()`
// can ever hand `RgbStroke`, though this function does not itself check
// that: it hands `blend` straight to `blendPixel()`, which is total over the
// whole enum). `strokeAlpha`, `weight`, `opacity` and `alphaLocked` are
// `depositRgbTexel()`'s own, with the identical meaning -- the accumulator
// arithmetic (`a`, `a1`, all four refusals) is bit-for-bit duplicated from
// that function rather than shared through a helper, deliberately: it is the
// one piece of arithmetic `--selftest` asserts is byte-identical to the
// unblended path when `blend == Normal` (which never calls this function,
// so that identity is a dispatch guarantee, not a coincidence of shared
// code), and a shared helper is a place a future change to one could
// silently perturb the other.
//
// Unlike `depositRgbTexel()`, `dst0`'s ALPHA is the frozen value alpha lock
// reads (`dst0[3]`, not a live `dst.a`) and its RGB is what §2a's
// `target = blend(straight(dst0), ink)` is computed from -- this function
// never reads a "live" texel at all, which is the property that makes the
// result order-independent within the stroke (§2a).
RgbDepositStep depositRgbTexelBlended(const std::array<float, 4>& dst0,
                                      const std::array<float, 3>& straightLinearRgb,
                                      BlendMode blend, float strokeAlpha, float weight,
                                      float opacity, bool alphaLocked = false) noexcept;

// One RGB stroke in flight: the latched ink, and the accumulator that makes
// `opacity` a per-stroke ceiling rather than a per-dab multiplier.
//
// Deliberately a small object with an explicit lifetime rather than a free
// function taking a `StrokeAlphaStore&`: the accumulator is only correct
// against the colour and ceiling it was started with (§2), so binding all three
// together at `begin()` makes the one combination that can go wrong --
// accumulator from one stroke, colour from another -- unspellable.
class RgbStroke {
 public:
  // Pen-down. Latches the ink and clears any accumulator a previous stroke
  // left, exactly as `StrokePath::reset()` clears leftover arc length and for
  // the same reason: alpha carried across strokes would let the end of one
  // stroke cap the start of the next.
  //
  // `opacity` is clamped to [0,1]; a non-positive one leaves a stroke that
  // deposits nothing, which is a legitimate setting and not an error.
  //
  // `alphaLocked` is latched here for the identical reason `opacity` is
  // (header §2, §4.5): it is a per-stroke property read out of the target
  // `Layer` once, at pen-down, by `app/StrokeSession.cpp` -- a lock cleared or
  // set mid-drag must not change which composite the dabs already spent are
  // read back through. Defaulted to `false` so every existing caller that
  // painted an unlocked layer keeps compiling and keeps its behaviour.
  //
  // `blend` is latched with them, for the same reason again (§2a): the
  // stroke-level composite is only correct against the mode it started with,
  // and a mode that changed mid-drag has no well-defined `dst0`/`target`
  // pairing. Defaulted to `BlendMode::Normal` so every existing caller keeps
  // compiling and keeps painting the unblended path; `app/StrokeSession.cpp`
  // is the only caller that ever passes anything else, and only on the RGB
  // deposit route (`BrushTip::blend`'s own comment names it as the one
  // reader).
  void begin(const std::array<float, 3>& straightLinearRgb, float opacity,
            bool alphaLocked = false, BlendMode blend = BlendMode::Normal) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator AND §2a's latched-`dst0` store (§3) and
  // leaves the ink alone, so the counts below still read correctly after a
  // stroke ends.
  void end() noexcept;

  // Deposits one dab into `store`, clipped to the canvas and gated by
  // `selection` (nullptr means no restriction, §4).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written -- the same branch as the write, for
  // `brush/Deposit` §3's reason. Duplicates across dabs are the caller's to
  // fold with `sortUniqueTiles()`.
  //
  // A dab every one of whose texels has already reached the ceiling writes
  // nothing, allocates nothing and reports no tiles. That is the cap being
  // observable rather than merely arithmetic: a stroke scrubbed back and forth
  // stops dirtying tiles once it is done, so live feedback stops re-uploading
  // them too.
  //
  // §2a: when `blend_ != BlendMode::Normal`, each texel this dab actually
  // changes is composited through `depositRgbTexelBlended()` against `dst0_`
  // (latching it first if this is the texel's first touch this stroke)
  // instead of through `depositRgbTexel()` against the live tile -- the only
  // difference the blend mode makes to this loop. `blend_ == Normal` takes
  // the exact branch and exact code this function always ran.
  DepositCount depositDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                          int32_t canvasH, const Selection* selection,
                          std::vector<TileCoord>* touchedOut);

  // Deposits every dab in `dabs`, in order. Order matters here for the same
  // reason it does for pigment, though for a different mechanism: `A` is a
  // running accumulation, so a dab's contribution depends on what the dabs
  // before it left.
  StrokeDeposit depositDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                            int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The alpha this stroke has laid at a document texel so far -- 0 for a texel
  // it has not reached. The accumulator's read side, exposed because it is what
  // `--selftest` asserts the ceiling against at zero tolerance (§3): the stored
  // texel has been through binary16 once per dab and the accumulator has not.
  float strokeAlphaAt(PixelCoord doc) const noexcept;

  const std::array<float, 3>& ink() const noexcept { return ink_; }
  float opacity() const noexcept { return opacity_; }
  BlendMode blend() const noexcept { return blend_; }

  // What the accumulator currently holds. `--selftest` prints both, because §3
  // makes a memory claim ("freed at pen-up") that is worth checking rather than
  // trusting.
  size_t accumulatorTiles() const noexcept { return alpha_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return alpha_.tileBytes(); }

  // §2a/§3's second store: what `dst0_` currently holds. Zero for the whole
  // life of a Normal stroke -- never allocated, not merely emptied -- which
  // is exactly what `--selftest` checks to tell "the Normal path was left
  // alone" from "the Normal path happens to read as empty".
  size_t dst0Tiles() const noexcept { return dst0_.occupiedTileCount(); }
  size_t dst0Bytes() const noexcept { return dst0_.tileBytes(); }

 private:
  std::array<float, 3> ink_{0.0f, 0.0f, 0.0f};
  float opacity_ = 1.0f;
  bool active_ = false;
  bool alphaLocked_ = false;
  BlendMode blend_ = BlendMode::Normal;
  StrokeAlphaStore alpha_;
  // §2a/§3: `dst0`, latched at each touched texel's first dab this stroke.
  // Stays empty for the whole stroke when `blend_ == BlendMode::Normal`.
  StrokeDst0Store dst0_;
};

}  // namespace np
