#include "brush/Smudge.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// "Holding nothing", exactly -- all four stored channels at zero, which is
// what an unwritten `core::Tile` texel holds and therefore what the
// coverage-weighted mean over an entirely unpainted footprint comes back as.
// Exact equality rather than a tolerance for the header §6 reason: the
// question is "has this finger ever picked anything up", not "is it faint".
bool isEmptyTexel(const std::array<float, 4>& t) noexcept {
  return t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 0.0f;
}

}  // namespace

SmudgeStep smudgeTexel(const std::array<float, 4>& dst, const std::array<float, 4>& finger,
                       float weight, float strength) noexcept {
  SmudgeStep out;
  // The no-op answer, returned by every one of the four refusals below. `dst`
  // rather than something recomputed from it -- see the header's note on
  // bit-identity: a dab that is changing nothing must leave the tile it is
  // scrubbing over completely alone, or the caller's "nothing to do here" test
  // never fires and the tile is re-uploaded every frame of the drag.
  out.premultiplied = dst;
  out.dabAlpha = 0.0f;

  // `!(x > 0)` rather than `x <= 0` throughout, so a NaN weight or a NaN
  // strength refuses instead of propagating into the layer -- the same guard
  // shape `depositRgbTexel()`, `eraseRgbTexel()` and `layerCoverage()` all use.
  const float s = std::clamp(strength, 0.0f, 1.0f);
  if (!(weight > 0.0f)) return out;  // no coverage, no flow, or `sel == 0`
  if (!(s > 0.0f)) return out;       // header §3: strength 0 is a bit-exact no-op

  // Header §3. Clamped, unlike the deposit's and the erase's `weight`, because
  // neither an accumulator's `min` nor a headroom division caps it here: a mix
  // fraction above 1 would extrapolate PAST the finger, overshooting into
  // negative alpha on the far side of a soft rim, and `flow` is deliberately
  // not clamped upstream (`brush/Deposit`: "a flow above 1 is a legitimate one
  // dab saturates the paper tip").
  float a = weight * s;
  if (a > 1.0f) a = 1.0f;
  if (!(a > 0.0f)) return out;

  // **Nothing to move** -- header §6. The finger already holds exactly what
  // this texel holds, so `lerp(dst, finger, a)` is `dst` for every `a`. Empty
  // canvas under an empty finger is this case, which is what makes a smudge
  // across a blank document allocate nothing at all: `getOrCreate()` is behind
  // the caller's `dabAlpha > 0` test and is never reached.
  if (finger[0] == dst[0] && finger[1] == dst[1] && finger[2] == dst[2] &&
      finger[3] == dst[3])
    return out;

  // Premultiplied, ONE factor, all four channels (header §5). A convex
  // combination of two well-formed premultiplied texels is a well-formed
  // premultiplied texel, so this cannot manufacture `brush/RgbErase` §1's
  // malformed "colour at alpha 0"; and it moves coverage as well as colour,
  // which is what lets the tool smear the edge of a stroke into blank canvas
  // rather than only recolour paint that is already there.
  out.premultiplied = {std::lerp(dst[0], finger[0], a), std::lerp(dst[1], finger[1], a),
                       std::lerp(dst[2], finger[2], a), std::lerp(dst[3], finger[3], a)};
  out.dabAlpha = a;
  return out;
}

std::array<float, 4> smudgeFinger(const std::array<float, 4>& finger, bool loaded,
                                  const std::array<float, 4>& pick, float strength) noexcept {
  // Header §3: the stroke's first dab LOADS the finger rather than blending
  // into it. Blending from a zero start would make a strength-1 stroke retain
  // transparent black for ever -- the slider at maximum being the slider that
  // stops working.
  if (!loaded) return pick;
  const float s = std::clamp(strength, 0.0f, 1.0f);
  // `lerp(pick, finger, s)`: `s` is how much of the CARRIED colour survives.
  // Exact at both ends by specification -- `s == 0` returns `pick` and
  // `s == 1` returns `finger` -- which is what makes both of §3's endpoint
  // claims arithmetic rather than approximate.
  return {std::lerp(pick[0], finger[0], s), std::lerp(pick[1], finger[1], s),
          std::lerp(pick[2], finger[2], s), std::lerp(pick[3], finger[3], s)};
}

void SmudgeStroke::begin(float strength) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  finger_ = {0.0f, 0.0f, 0.0f, 0.0f};
  loaded_ = false;
  active_ = true;
}

void SmudgeStroke::end() noexcept {
  finger_ = {0.0f, 0.0f, 0.0f, 0.0f};
  loaded_ = false;
  active_ = false;
}

DepositCount SmudgeStroke::smudgeDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                     int32_t canvasW, int32_t canvasH,
                                     const Selection* selection,
                                     std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  // **Strength alone is the early exit, and `flow` deliberately is not.**
  // Strength 0 makes the whole tool a no-op (header §3), finger included --
  // nothing can ever be written, so the carried colour is unobservable and
  // spending a pick-up pass on it would be work for a value nobody can read.
  // `flow` is a different quantity: it is how much a dab LAYS DOWN, and the
  // pick-up rule (§2) does not mention it. Skipping the pick-up on a flow-0 dab
  // would be an undocumented coupling with a real consequence, because flow is
  // resolved per DAB now (Photoshop's Transfer Flow, `app/StrokeSession`'s
  // `transferFlowMul_`): a dab whose resolved flow momentarily hit 0 would
  // FREEZE the finger, and the next dab would lay down a colour picked up from
  // somewhere the tip has since left. So a flow-0 dab picks up and writes
  // nothing, which is also what makes the pick-up rule assertable on its own.
  if (!(strength_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from all three sibling
  // routes -- the shape of a dab is not a property of what the dab does, and a
  // second falloff here would be a second place for the smudge and the brush to
  // disagree about where a tip ends. That matters here for the same reason it
  // does for the eraser: a painter alternates brush and smudge over one edge.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // ---------------------------------------------------------------------
  // Pass 1 -- the pick-up (header §2).
  //
  // A separate pass, before any write, because the mean must not depend on
  // which texels this same dab has already changed: the loop below is
  // tile-major, so an interleaved version's answer would depend on which tile
  // boundary the dab happened to straddle. It costs a second `dabCoverage()`
  // per texel and buys an answer independent of the tile grid.
  //
  // **The selection is not consulted here** -- §4: the selection bounds the
  // edit, and reading is not an edit. The write pass below owns the whole of
  // the null-`Selection` branch core/SelectionMask.hpp requires each hoisted
  // loop to own; this pass deliberately has none to get wrong.
  //
  // **An absent tile contributes transparent black rather than being skipped**
  // -- §2(iii), and it is the opposite of `brush/RgbErase` §4's rule on
  // purpose. Picking up emptiness is how the carried colour thins out as the
  // tip leaves the paint, which is the whole of §5's falloff.
  // ---------------------------------------------------------------------
  double sumW = 0.0;
  double sum[4] = {0.0, 0.0, 0.0, 0.0};
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const Tile* srcTile = store.find(coord);
      const PixelCoord org = tileOrigin(coord);
      const int32_t x0 = std::max(b.x0, org.x);
      const int32_t x1 = std::min(b.x1, org.x + kTileSize - 1);
      const int32_t y0 = std::max(b.y0, org.y);
      const int32_t y1 = std::min(b.y1, org.y + kTileSize - 1);
      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;
          // Paper tooth at this texel's ABSOLUTE canvas position, the same
          // call and the same reasoning as `brush/Deposit` §2e -- and the
          // same coverage the write pass uses, so the tip picks up from
          // exactly where it will put down.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;
          sumW += static_cast<double>(cov);
          if (srcTile == nullptr) continue;  // transparent black: adds weight,
                                             // adds no colour
          const std::array<float, 4> px =
              srcTile->readPixel(tileLocalOffset(PixelCoord{x, y}));
          for (int c = 0; c < 4; ++c) sum[c] += static_cast<double>(cov) * px[c];
        }
      }
    }
  }
  // A footprint the falloff (or the grain) covered nowhere. No pick-up, so the
  // finger is left alone rather than being loaded with a divide by zero, and
  // there is nothing to write either.
  if (!(sumW > 0.0)) return count;

  const std::array<float, 4> pick{
      static_cast<float>(sum[0] / sumW), static_cast<float>(sum[1] / sumW),
      static_cast<float>(sum[2] / sumW), static_cast<float>(sum[3] / sumW)};

  // Exactly once per dab, from a pick-up computed before any of this dab's
  // writes -- header §2's last paragraph and §3's first line.
  finger_ = smudgeFinger(finger_, loaded_, pick, strength_);
  loaded_ = true;

  // ---------------------------------------------------------------------
  // Pass 2 -- the write.
  //
  // Skipped entirely for a dab with no flow: every texel's weight would be 0
  // and `smudgeTexel()` would refuse every one of them, so this is the same
  // answer arrived at without walking the footprint a second time. The pick-up
  // above has already happened, which is the point (see the early exit's own
  // comment).
  // ---------------------------------------------------------------------
  if (!(tip.flow > 0.0f)) return count;
  const bool fingerEmpty = isEmptyTexel(finger_);

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel, across the two stores keyed by the same
  // coordinate (the layer and the selection). Ascending (y, x) so `touchedOut`
  // comes out in `sortUniqueTiles()`'s order for the common single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      const Tile* srcTile = store.find(coord);
      // **Not `brush/RgbErase` §4's unconditional skip.** A loaded finger
      // laying colour into empty space is this tool working -- a smudge GROWS
      // the painted region -- so the skip is conditional on the finger, not on
      // the tile. Speed rather than correctness (§6: the per-texel equality
      // test below is what actually makes the allocation impossible), and it
      // is what keeps a drag across blank canvas from evaluating the falloff a
      // few hundred times per dab to prove it has nothing to do.
      if (srcTile == nullptr && fingerEmpty) continue;

      // §4. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null
      // *tile* inside an engaged selection is "selects nothing". Getting those
      // two nulls the same way round is how a smudge starts smearing outside
      // the ants, or stops smearing at all.
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        if (cover == nullptr) continue;
      }

      const PixelCoord org = tileOrigin(coord);
      const int32_t x0 = std::max(b.x0, org.x);
      const int32_t x1 = std::min(b.x1, org.x + kTileSize - 1);
      const int32_t y0 = std::max(b.y0, org.y);
      const int32_t y1 = std::min(b.y1, org.y + kTileSize - 1);

      // Fetched lazily at the first texel this dab actually changes --
      // brush/Deposit §3, fact 2. A tile the bounding box clipped but the disc
      // missed, or one every texel of which already matches the finger, is
      // never unshared and never reported.
      Tile* dst = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          // An absent tile reads as transparent black here for the same reason
          // it does in pass 1 -- that is what it holds.
          const std::array<float, 4> before =
              srcTile != nullptr ? srcTile->readPixel(local)
                                 : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          const SmudgeStep step = smudgeTexel(before, finger_, tip.flow * cov * sel, strength_);
          // The transparent tail of the falloff, a texel the selection
          // excluded, a strength of 0 and a texel the finger already matches
          // all arrive here as `dabAlpha == 0`, and all four mean the same
          // thing: do not touch this texel, do not unshare its tile, do not
          // report it dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            // `getOrCreate` unshares a copy-on-write tile, so after it the
            // store's tile at this coordinate is a *different object* and
            // `srcTile` would keep showing the pre-write value --
            // core/TileStore.hpp calls that "detached". A smudge reading
            // through a detached pointer would re-read the pre-dab texel for
            // every later texel of the same tile, which for this route is not
            // merely stale: the whole tool is a read of what it has just
            // written.
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          dst->writePixel(local, step.premultiplied);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit SmudgeStroke::smudgeDabs(TileStore& store, const BrushTip& tip,
                                       const std::vector<Vec2>& dabs, int32_t canvasW,
                                       int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = smudgeDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
