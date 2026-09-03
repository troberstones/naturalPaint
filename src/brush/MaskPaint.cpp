#include "brush/MaskPaint.hpp"

#include <algorithm>

#include "brush/Grain.hpp"
#include "color/Space.hpp"
#include "ops/PointOps.hpp"

namespace np {

float maskTargetForInk(const std::array<float, 3>& linearRgb) noexcept {
  // Header §2. `computeLuma()` with its default `kRec709LumaWeights` -- the one
  // luma this codebase has, shared with the grayscale operator, the saturation
  // operator and the luminance-range selection, so a mask painted with a
  // coloured brush and the same colour turned grey by Image > Adjustments >
  // Desaturate agree about which grey it is.
  const float luma = computeLuma(linearRgb);
  // Weights in linear light, encode the scalar afterwards
  // (core/SelectionRefine.hpp's own order, and its argument for it). The
  // encode is what makes a 50 % grey swatch paint coverage 0.5 rather than
  // 0.214.
  const float encoded = srgbEncode(luma);
  // The same clamp shape every coverage in this codebase gets -- `!(v > 0)`
  // rather than `std::max`, so a NaN ink lands on 0 instead of propagating into
  // a mask and from there into every composited texel under it
  // (`core::maskCoverageClamp()`, `core::layerCoverage()`).
  return maskCoverageClamp(encoded);
}

MaskPaintStep paintMaskTexel(float dst, float strokeApplied, float weight, float ceiling,
                             float target) noexcept {
  MaskPaintStep out;
  // The no-op answer, returned by every refusal below. `dst` bit-identical
  // rather than something recomputed from it -- see the header's note: a stroke
  // that has reached its ceiling must leave the tile it is scrubbing over
  // completely alone, or the caller's "nothing to do here" test never fires.
  out.coverage = dst;
  out.strokeApplied = strokeApplied;
  out.changed = false;

  const float cap = std::clamp(ceiling, 0.0f, 1.0f);
  const float a0 = std::clamp(strokeApplied, 0.0f, 1.0f);
  const float headroom = 1.0f - a0;

  // `!(x > 0)` rather than `x <= 0` throughout, so a NaN weight or a NaN
  // accumulator refuses instead of propagating into the mask -- the same guard
  // shape `depositRgbTexel()`, `eraseRgbTexel()` and `layerCoverage()` all use.
  if (!(weight > 0.0f)) return out;    // no coverage, or the dab has no flow
  if (!(cap > 0.0f)) return out;       // a stroke asked to change nothing
  if (!(headroom > 0.0f)) return out;  // A == 1: arrived, and the divisor's zero
  if (!(a0 < cap)) return out;         // the ceiling is already reached (§3)

  // **Already there** -- header §5. This is the skip that replaces
  // `brush/RgbErase`'s "an absent tile holds nothing", and unlike that one it is
  // a test on the VALUE rather than on the storage, because an absent mask tile
  // holds 1.0 and painting black onto it is the common case. Exact equality is
  // the right test and not a tolerance: the question is "would this write change
  // the stored word", and a `dst` that is bit-equal to `target` cannot, since
  // the lerp below fixes both endpoints.
  if (dst == target) return out;

  // §3. `a1` is the fraction of the way to the target this stroke has travelled
  // after this dab, capped at the ceiling; `f` is the lerp factor for which one
  // step from the current value lands the total exactly there, from
  // `1 - a1 = (1 - a0)(1 - f)`.
  float a1 = a0 + weight * headroom;
  if (a1 > cap) a1 = cap;
  float f = (a1 - a0) / headroom;
  // Algebraically `f <= 1` always, since `a1 <= 1`. Clamped anyway because the
  // subtraction and the division are each rounded, and an `f` of 1+1ulp would
  // put the result marginally past the target -- which `maskCoverageClamp()`
  // below would catch at the ends but not in the middle. Same discipline as
  // `eraseRgbTexel()` clamping the other end of the same range.
  if (f > 1.0f) f = 1.0f;

  const float v = dst + f * (target - dst);
  out.coverage = maskCoverageClamp(v);
  out.strokeApplied = a1;
  out.changed = true;
  return out;
}

void MaskPaintStroke::begin(float target, float ceiling) noexcept {
  target_ = maskCoverageClamp(target);
  ceiling_ = std::clamp(ceiling, 0.0f, 1.0f);
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  applied_ = StrokeAlphaStore{};
  active_ = true;
}

void MaskPaintStroke::end() noexcept {
  applied_ = StrokeAlphaStore{};
  active_ = false;
}

float MaskPaintStroke::strokeAppliedAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = applied_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount MaskPaintStroke::paintDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre,
                                       int32_t canvasW, int32_t canvasH,
                                       const Selection* selection,
                                       std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(ceiling_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from every other route --
  // the shape of a dab is not a property of what the dab does, and a second
  // falloff here would be a second place for the mask brush and the layer brush
  // to disagree about where a tip ends. That matters here in a way it does not
  // elsewhere: a painter paints a shape on the layer and then paints the same
  // shape into the mask to trim it, and a rim one texel wider in the mask would
  // eat the edge it was tidying.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel, across the three stores keyed by the same
  // coordinate (the mask, the accumulator, the selection). Ascending (y, x) so
  // `touchedOut` comes out in `sortUniqueTiles()`'s order for the common
  // single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // **No skip for an absent tile here** -- header §5, and this is the line
      // where copying `brush/RgbErase.cpp` would have produced a mask brush
      // that did nothing on every mask this application can create. An absent
      // mask tile means 1.0, so it has plenty to change.
      const MaskTile* srcTile = store.find(coord);

      // §4. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null *tile*
      // inside an engaged selection is "selects nothing". Getting those two
      // nulls the same way round is how a mask brush starts painting outside the
      // ants, or stops painting at all.
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

      // The accumulator's READ handle, which may legitimately be absent -- a
      // texel this stroke has not reached has `A == 0`, and that is exactly what
      // an absent tile says, so a dab that turns out to change nothing never
      // allocates the 64 KiB.
      const StrokeAlphaTile* appliedRead = applied_.find(coord);

      // Both write handles, fetched lazily at the first texel this dab actually
      // changes -- brush/Deposit §3, fact 2. A tile the bounding box clipped but
      // the disc missed, or one every texel of which is already at the target,
      // is never allocated, never unshared and never reported.
      MaskTile* dst = nullptr;
      StrokeAlphaTile* appliedWrite = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;

          // Paper tooth, at this texel's ABSOLUTE canvas position -- `x`/`y`,
          // not `dx`/`dy`, which is why it cannot live inside `dabCoverage()`.
          // Identical line and identical reasoning to brush/Deposit.cpp's own
          // (§2e) and to every other layer-writing route's.
          //
          // **This call is what keeps `grainReachesRoute()` honest.** That
          // predicate delegates to `strokeRouteWritesLayer()`, and its own
          // comment warns in as many words that a route added to that predicate
          // without a `grainCoverageAt()` call would leave the predicate correct
          // and the PAPER GRAIN group's enabled state wrong, with nothing to
          // notice. This route is in that predicate, so it makes the call, and
          // `app/selftest/StrokeSession.cpp` asserts the agreement route by
          // route.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          // §5: absent means reveal, so 1.0 -- the opposite reading from an
          // absent content tile, and the one core/Mask.hpp designs the whole
          // default-filled `MaskTile` around.
          const float before = srcTile != nullptr ? srcTile->readCoverage(local) : 1.0f;
          const float applied = appliedRead != nullptr ? appliedRead->at(local) : 0.0f;
          // **The selection enters TWICE, and both are load-bearing** -- §4.
          // Into the weight, so one pass through a half-selected texel travels
          // half as far; and into the ceiling, so no number of passes takes that
          // texel past half way. The first alone is a speed limit rather than a
          // bound, and a scrubbed stroke walks straight through it.
          const MaskPaintStep step =
              paintMaskTexel(before, applied, tip.flow * cov * sel, ceiling_ * sel, target_);
          // The ceiling, the transparent tail of the falloff, a texel the
          // selection excluded and a texel already at the target all arrive here
          // as `changed == false`, and all four mean the same thing: do not
          // touch this texel, do not allocate its tile, do not report it dirty.
          if (!step.changed) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            // `getOrCreate` unshares a copy-on-write tile, so after it the
            // store's tile at this coordinate is a *different object* and
            // `srcTile` would keep showing the pre-write value --
            // core/TileStore.hpp calls that "detached", and a stroke reading
            // through a detached pointer would move every dab after the first
            // from the pre-stroke coverage, so a scrubbed stroke would
            // repeatedly take the same first step and never arrive.
            //
            // It also turns the absent-tile case into a present one, correctly:
            // `MaskTile`'s constructor fills with 1.0, which is the same value
            // `before` was just read as.
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (appliedWrite == nullptr) {
            // `getOrCreate` on a store nobody else holds is an allocate-or-find
            // with no copy behind it -- the accumulator is never copied out of
            // the stroke, so the copy-on-write barrier never fires.
            appliedWrite = &applied_.getOrCreate(coord);
            appliedRead = appliedWrite;
          }
          appliedWrite->set(local, step.strokeApplied);
          dst->writeCoverage(local, step.coverage);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit MaskPaintStroke::paintDabs(MaskTileStore& store, const BrushTip& tip,
                                         const std::vector<Vec2>& dabs, int32_t canvasW,
                                         int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = paintDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
