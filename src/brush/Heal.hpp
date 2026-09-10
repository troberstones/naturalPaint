#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "brush/CloneStamp.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/StrokePath.hpp"
#include "core/SelectionMask.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// brush/Heal -- **the clone stamp that keeps the destination's light.**
//
// ==========================================================================
// 0. What this is, and why it is a fourth sibling rather than a flag on
//    brush/CloneStamp
// ==========================================================================
//
// PRD D6, PLAN.md Phase 8. A heal is a clone whose copied texels are corrected
// so that they match the illumination they are being dropped into: the *texture*
// comes from the source, the *light* comes from the surround of the destination.
// `ops/Poisson` §0 derives the arithmetic (Perez et al.'s seamless clone, in the
// `f = src + h` form where `h` is one harmonic interpolation of one ring of
// numbers) and this module says nothing about it a second time.
//
// **A `bool healing` on `CloneStampStroke` was rejected**, and the reason is
// structural rather than stylistic. `brush/CloneStamp`'s inner loop is a
// per-texel pure function of two texels: `cloneStampTexel(dst, src, ...)` can be
// stated, asserted and reasoned about one texel at a time, and that is the
// property its own §0 uses to argue *it* is not a flag on `brush/RgbDeposit`.
// The healed source at a texel is not a function of that texel at all -- it
// depends on every texel around the whole rim of the dab, so the dab has to be
// solved as a *patch* before its first texel can be written. A flag would put
// two different shapes of computation inside one loop, and the flag would be the
// only thing saying which one was running.
//
// **What IS shared is the composite, deliberately and by call.** Once the patch
// is solved, laying a healed texel down is `cloneStampTexel()` unchanged: a
// premultiplied source that carries its own alpha, source-over onto the
// destination, with the per-stroke accumulator, the opacity ceiling and the
// alpha-locked colour-only form. That arithmetic and all four of its invariants
// (`brush/CloneStamp` §1) are about how a copied texel is *laid down*, not about
// where its value came from -- so this module calls that function rather than
// restating it, which is the same call `brush/PencilDeposit` makes on
// `depositRgbTexel()`. A second copy here is exactly the drift this codebase's
// headers keep naming.
//
// **This is the RGB row and only the RGB row**, for `brush/CloneStamp` §0's
// reason with one more step in it: a gradient-domain solve is stated over a
// linear vector space, and a Pigment texel is a Kubelka-Munk latent premultiplied
// by mass. The Laplacian of a latent is not a quantity this build has decided
// the meaning of, and inventing one in a deposit loop would be deciding it for
// the whole codebase from the least visible place. `strokeRouteFor()` refuses a
// Pigment layer by name (app/StrokeSession §1c).
//
// ==========================================================================
// 1. The patch: which texels are solved, and which are the boundary
// ==========================================================================
//
// One dab, one solve. The region is the dab's own bounding box
// (`dabPixelBounds()`, the same footprint every other route uses -- the shape of
// a dab is not a property of what the dab does), **grown by one texel on every
// side and clipped to the canvas**. That one-texel skin is the Dirichlet
// boundary: `ops/Poisson::healPatch()` holds its rim at the destination's own
// values and solves the inside, so growing the box by one is what makes every
// texel the dab can actually write an *interior* texel with a real correction,
// rather than a rim texel pinned to what was already there.
//
// **At the canvas edge the skin is clipped away**, and that is a stated
// narrowing rather than an oversight: a dab whose bounding box touches the
// border has its outermost in-canvas row as the boundary instead, so a one-texel
// rim right at the edge of the document is left unhealed. The alternative --
// treating outside-the-canvas as a boundary value of zero -- would pull every
// heal near the border towards black, which is a visible wrong answer where this
// is an invisible missing one.
//
// **Boundary from the LIVE layer, source from the PRE-STROKE SNAPSHOT**, and
// the asymmetry is the whole of §2.
//
// ==========================================================================
// 2. Two reads, two different stores, and why that is not a contradiction
// ==========================================================================
//
// `brush/CloneStamp` §2 is the hazard: source and destination are two windows
// onto one `TileStore`, so a loop that read the live store would feed its own
// output back in and the picture would depend on which way the texel and tile
// loops happened to run. That argument applies here unchanged **to the source**,
// and this module takes the identical remedy: the source is a copy of the store
// taken at pen-down, which costs one map node per existing tile and no texel
// data (`core/TileStore.hpp`: "copying a store IS the share").
//
// The **destination boundary is read live**, and that is a different question
// with a different answer:
//
//   * Within one dab it changes nothing about order-independence. The whole
//     patch -- source and destination both -- is gathered *before* the first
//     texel of that dab is written, so the solve sees one consistent snapshot of
//     the destination and no texel of this dab can influence another. That is
//     precisely the property §2 of the clone's header is about, preserved.
//   * Across dabs it is the *feature*. Dab N's boundary must include dab N-1's
//     output, or a stroke would be a row of independently-lit patches with a
//     seam between each pair -- every one of them individually seamless against
//     paint the stroke has already replaced. Dab order is the stroke path, which
//     is deterministic and is what the user drew; texel and tile order is an
//     implementation detail that nothing in the picture records. The first is
//     allowed to matter and the second is not.
//
// ==========================================================================
// 3. The offset is INTEGER texels, and this build heals ALIGNED
// ==========================================================================
//
// Both inherited from `brush/CloneStamp` §3 and `app/AppState`'s
// `CloneSourceState`, and both deliberately rather than by default.
//
// Integer texels: a resampled source is a *reconstruction* of the source, and a
// filter kernel is a decision `ops/Resample` owns rather than a deposit loop.
// The heal has one extra reason the clone does not -- the correction is a
// solve over the source's own Laplacian, and resampling changes that Laplacian,
// so a fractional offset would quietly change what the tool is copying rather
// than only where from.
//
// Aligned: the offset survives pen-up, so a second stroke continues the same
// copy rather than restarting at the anchor. `AppState::CloneSourceState` carries
// that argument and names the options-bar control that would switch it; matching
// it here rather than reopening it is what keeps the two tools' one shared source
// gesture meaning one thing.
//
// ==========================================================================
// 4. What a heal from nothing does, and why it is NOT the clone's answer
// ==========================================================================
//
// `brush/CloneStamp` §4: a dab whose source texel is empty writes nothing, and
// that is arithmetic -- at `src == 0` the clone's composite is `dst` bit for bit.
//
// **Here it is not.** With an empty source the patch's rim carries `dst - 0`,
// so the correction is the harmonic interpolation of the surrounding paint and
// the healed patch is a smooth fill of the hole -- which is the *correct*
// gradient-domain answer and a genuinely useful one (it is what a spot heal over
// featureless skin does). So a heal over blank source is not free and does not
// pretend to be.
//
// The property that survives is the one that matters for cost: **a heal that has
// nothing to say costs nothing.** Blank source over blank destination gives a rim
// of exact zeros, `ops/Poisson` §1 answers it with `h == 0` exactly, the healed
// texel is four zeros, and `cloneStampTexel()`'s own empty-source rule then
// skips it -- so a stroke dragged across empty canvas allocates no tile, reports
// none and moves no revision, exactly as the clone does.
//
// ==========================================================================
// 5. The cost, measured
// ==========================================================================
//
// One dab is one multigrid solve over its own bounding box, four channels, so
// the cost is quadratic in the tip radius. Measured on this machine
// (`ops/Poisson` at its default 8-cycle ceiling): a 21x21 patch is 0.14 ms, a
// 129x129 patch is 7.2 ms, and a 403x403 patch -- the widest tip the UI offers,
// `kBrushRadiusMax` -- is 264 ms.
//
// That last number is a real limit and it is recorded rather than hidden: a heal
// at maximum radius is not interactive. The tool is a retouching tool used in
// short strokes at moderate size and it is comfortable there; the fix when the
// large end is wanted is to solve the correction at a capped resolution and
// interpolate it up, which is sound because `h` is harmonic and therefore has no
// high frequencies to lose -- not to make the solve less converged, which would
// trade a visible seam for a hidden one.
//
// ==========================================================================
// 6. What is deliberately not here
// ==========================================================================
//
// **No `Document`, no `Layer`, no `History`, no `OpenDocument`** -- the boundary
// every other `brush/` module draws.
//
// **No anchor and no gesture.** Where the source is lives on
// `app/AppState`'s `CloneSourceState` -- the same one the clone stamp uses, and
// `app/StrokeSession` §1c argues why one anchor serves both tools rather than
// two. This module takes the resolved vector.
//
// **No cross-layer sampling**, and **no aligned/non-aligned toggle** -- both for
// `brush/CloneStamp` §5's reasons, unchanged.
//
// **No separate "spot heal".** Photoshop's Spot Healing Brush picks its own
// source. That is a source-selection policy, not a different arithmetic: it would
// sit above this module and hand it an offset, and nothing here would change.
namespace np {

// A solved texel made storable.
//
// The solve is a linear interpolation of differences and knows nothing about
// what `core::Tile` may hold, so it can hand back an alpha outside [0,1] or a
// negative radiance -- neither of which is a texel. Two rules, both narrow:
//
//   * **Alpha is clamped to [0,1]**, because it is a coverage and coverage
//     outside that range has no meaning; `core/Composite` reads it straight into
//     an accumulator.
//   * **Colour is clamped at zero from BELOW and not from above.** Negative light
//     is not a measurement; a value above 1 is (`color/Space.hpp`: "Working-space
//     values are linear light and can legitimately exceed 1.0"), and clamping the
//     top would be this deposit loop deciding the build's dynamic range.
//
// And one consequence of premultiplied storage: **coverage zero carries no
// colour.** A texel the solve pushed to `a == 0` while leaving colour behind is
// exactly the malformed texel `brush/CloneStamp` §4 refuses to launder, and here
// it would be *manufactured* rather than reproduced -- so it is zeroed outright.
std::array<float, 4> healClampTexel(std::array<float, 4> texel) noexcept;

// One heal stroke in flight. The same four members `CloneStampStroke` binds at
// `begin()` -- the pre-stroke source snapshot, the integer offset, the latched
// ceiling and the accumulator that makes that ceiling a per-stroke bound -- for
// that class's stated reason: the accumulator is only correct against the opacity
// it was started with *and* the snapshot it was started against, so binding all
// three at `begin()` makes the wrong combinations unspellable.
class HealStroke {
 public:
  // Pen-down. Takes the source snapshot (§2), rounds the offset to whole texels
  // (§3), latches the opacity and clears any accumulator a previous stroke left.
  //
  // `source` is copied, not borrowed. `opacity` is clamped to [0,1]; a
  // non-positive one leaves a stroke that transfers nothing, which is a
  // legitimate setting and not an error. `alphaLocked` is latched with them so a
  // lock toggled mid-drag cannot change which composite the dabs already spent
  // were read back through.
  void begin(const TileStore& source, Vec2 offset, float opacity, bool alphaLocked);

  bool active() const noexcept { return active_; }

  // Pen-up. Frees the accumulator **and the snapshot** -- the larger of the two,
  // since it shares a tile with the live layer for every tile the layer had at
  // pen-down.
  void end() noexcept;

  // Solves and lays down one dab, clipped to the canvas and gated by `selection`
  // (nullptr means no restriction, `core/SelectionMask.hpp`'s convention and NOT
  // the inverse).
  //
  // Every tile it writes is appended to `touchedOut` when that is non-null, at
  // the moment the tile is first written. Duplicates across dabs are the
  // caller's to fold with `sortUniqueTiles()`.
  //
  // **The selection gates the deposit and not the solve.** The patch is solved
  // over its whole rectangle whichever texels the ants admit, because the
  // correction at an admitted texel depends on its neighbours whether or not
  // those are selected -- solving a masked-out region as if it were a boundary
  // would make the answer inside a selection depend on the selection's shape,
  // which is a different picture from the same gesture with the ants moved.
  // PRD E1's guarantee is about what is *written*, and nothing outside the
  // selection is.
  DepositCount healDab(TileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                       int32_t canvasH, const Selection* selection,
                       std::vector<TileCoord>* touchedOut);

  // Heals every dab in `dabs`, in order. Order matters for the accumulator, and
  // -- unlike the clone -- it matters for the *result* too, deliberately: §2's
  // second bullet is why each dab's boundary must see the one before it.
  StrokeDeposit healDabs(TileStore& store, const BrushTip& tip, const std::vector<Vec2>& dabs,
                         int32_t canvasW, int32_t canvasH, const Selection* selection);

  // The fraction of the source this stroke has transferred at a document texel
  // so far -- 0 for a texel it has not reached. `--selftest` asserts the ceiling
  // against this at zero tolerance: the stored texel has been through binary16
  // once per dab and the accumulator has not.
  float strokeAlphaAt(PixelCoord doc) const noexcept;

  float opacity() const noexcept { return opacity_; }
  // The rounded offset actually in use (§3) -- what `--selftest` reads to prove
  // the snapping happened rather than assuming it.
  int32_t offsetX() const noexcept { return offsetX_; }
  int32_t offsetY() const noexcept { return offsetY_; }

  size_t accumulatorTiles() const noexcept { return alpha_.occupiedTileCount(); }
  size_t snapshotTiles() const noexcept { return source_.occupiedTileCount(); }

 private:
  TileStore source_;
  int32_t offsetX_ = 0;
  int32_t offsetY_ = 0;
  float opacity_ = 1.0f;
  bool alphaLocked_ = false;
  bool active_ = false;
  StrokeAlphaStore alpha_;
};

}  // namespace np
