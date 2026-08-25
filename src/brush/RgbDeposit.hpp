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
// were already kind-agnostic. Exactly two things are genuinely different: what
// one dab does to one texel, and the per-stroke accumulator that decides it.
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
//   1", which is `BrushState::load` scaled by the DYNAMICS matrix.
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
// **No blend mode, no smudge, no texture.** A dab is source-over of one colour.
// `Layer::blend` still applies to the layer as a whole at composite time and is
// untouched; a brush that could pick its own blend mode per dab is a different
// feature with its own UI.
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
RgbDepositStep depositRgbTexel(const std::array<float, 4>& dst,
                               const std::array<float, 3>& straightLinearRgb, float strokeAlpha,
                               float weight, float opacity) noexcept;

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
  void begin(const std::array<float, 3>& straightLinearRgb, float opacity) noexcept;

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator (§3) and leaves the ink alone, so the counts
  // below still read correctly after a stroke ends.
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

  // What the accumulator currently holds. `--selftest` prints both, because §3
  // makes a memory claim ("freed at pen-up") that is worth checking rather than
  // trusting.
  size_t accumulatorTiles() const noexcept { return alpha_.occupiedTileCount(); }
  size_t accumulatorBytes() const noexcept { return alpha_.tileBytes(); }

 private:
  std::array<float, 3> ink_{0.0f, 0.0f, 0.0f};
  float opacity_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore alpha_;
};

}  // namespace np
