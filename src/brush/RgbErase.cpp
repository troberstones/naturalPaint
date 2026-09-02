#include "brush/RgbErase.hpp"

#include <algorithm>

namespace np {

RgbEraseStep eraseRgbTexel(const std::array<float, 4>& dst, float strokeErase, float weight,
                           float strength) noexcept {
  RgbEraseStep out;
  // The no-op answer, returned by every one of the five refusals below. `dst`
  // rather than something recomputed from it -- see the header's note on
  // bit-identity: a stroke that has reached its floor must leave the tile it is
  // scrubbing over completely alone.
  out.premultiplied = dst;
  out.strokeErase = strokeErase;
  out.dabAlpha = 0.0f;

  const float cap = std::clamp(strength, 0.0f, 1.0f);
  const float e0 = std::clamp(strokeErase, 0.0f, 1.0f);
  const float headroom = 1.0f - e0;

  // `!(x > 0)` rather than `x <= 0` throughout, so a NaN weight or a NaN
  // accumulator refuses instead of propagating into the layer -- the same guard
  // shape `depositRgbTexel()`, `depositDab()` and `layerCoverage()` all use.
  if (!(weight > 0.0f)) return out;    // no coverage, or the dab has no flow
  if (!(cap > 0.0f)) return out;       // a stroke asked to remove nothing
  if (!(headroom > 0.0f)) return out;  // E == 1: gone already, and the divisor's zero
  if (!(e0 < cap)) return out;         // the floor is already reached (§2)

  // **Nothing here to remove** -- header §4. All four channels, not the alpha
  // alone: a texel holding colour at alpha 0 is malformed rather than empty, and
  // this function must not be the thing that declares it absent. Exact equality
  // is the right test and not a tolerance, because the question is "has anything
  // ever been written here", and an unwritten `core::Tile` texel is exactly
  // four zero half words.
  if (dst[0] == 0.0f && dst[1] == 0.0f && dst[2] == 0.0f && dst[3] == 0.0f) return out;

  // Header §2. `e1` is the fraction this stroke has removed after this dab,
  // capped at the strength; `e` is the destination-out alpha for which one
  // composite lands the total exactly there, from the identity
  // `1 - e1 = (1 - e0)(1 - e)`.
  float e1 = e0 + weight * headroom;
  if (e1 > cap) e1 = cap;
  float e = (e1 - e0) / headroom;
  // Algebraically `e <= 1` always, since `e1 <= 1`. Clamped anyway because the
  // subtraction and the division are each rounded, and an `e` of 1+1ulp would
  // make `keep` negative -- storing a texel with negative alpha, which
  // core/Composite reads straight into its accumulator and which is a document
  // no reader in this codebase has a meaning for. Same discipline as
  // `depositRgbTexel()` clamping the other end of the same range.
  if (e > 1.0f) e = 1.0f;

  // Premultiplied destination-out: ONE factor, all four channels (§1).
  //
  //     dst' = dst * (1 - e)
  //
  // Scaling the alpha alone would leave `(colour * a, a(1-e))`, which
  // un-premultiplies to a *brighter* colour than the paint it came from and
  // diverges as `e -> 1` -- the fringe on exactly the soft edges an eraser is
  // used for. At `e == 1` every channel is exactly zero, which is what makes a
  // fully erased texel indistinguishable from one that was never painted rather
  // than a texel that glows with no coverage.
  const float keep = 1.0f - e;
  out.premultiplied = {dst[0] * keep, dst[1] * keep, dst[2] * keep, dst[3] * keep};
  out.strokeErase = e1;
  out.dabAlpha = e;
  return out;
}

void RgbEraseStroke::begin(float strength) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  erased_ = StrokeAlphaStore{};
  active_ = true;
}

void RgbEraseStroke::end() noexcept {
  erased_ = StrokeAlphaStore{};
  active_ = false;
}

float RgbEraseStroke::strokeEraseAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = erased_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount RgbEraseStroke::eraseDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                      int32_t canvasW, int32_t canvasH, const Selection* selection,
                                      std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(strength_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from both deposit routes --
  // the shape of a dab is not a property of what the dab does, and a second
  // falloff here would be a second place for the eraser and the brush to
  // disagree about where a tip ends. That matters more here than anywhere else:
  // a painter alternates brush and eraser over one edge, and a rim that erased
  // one texel wider than it painted would eat the stroke it was tidying.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel, across the three stores keyed by the same
  // coordinate (the layer, the accumulator, the selection). Ascending (y, x) so
  // `touchedOut` comes out in `sortUniqueTiles()`'s order for the common
  // single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // **A tile that does not exist holds nothing to erase** -- header §4, and
      // `ops/FloodFill`'s own statement of the same asymmetry. Skipped before
      // anything is allocated, so an eraser dragged across blank canvas costs
      // nothing at all rather than 224 KiB per tile it crossed plus a dirty tile
      // per frame of the drag. This is also why the read handle below is
      // non-null on every path that follows.
      const Tile* srcTile = store.find(coord);
      if (srcTile == nullptr) continue;

      // §3. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null *tile*
      // inside an engaged selection is "selects nothing". Getting those two
      // nulls the same way round is how an eraser starts cutting outside the
      // ants, or stops cutting at all.
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
      // texel this stroke has not reached has `E == 0`, and that is exactly what
      // an absent tile says, so a dab that turns out to change nothing never
      // allocates the 64 KiB.
      const StrokeAlphaTile* eraseRead = erased_.find(coord);

      // Both write handles, fetched lazily at the first texel this dab actually
      // changes -- brush/Deposit §3, fact 2. A tile the bounding box clipped but
      // the disc missed, or one every texel of which has already reached the
      // floor, is never unshared and never reported.
      Tile* dst = nullptr;
      StrokeAlphaTile* eraseWrite = nullptr;

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
          // (§2e); see there for the full argument.
          //
          // **This route had no grain call at all until now**, so a brush with
          // PAPER GRAIN switched on painted textured on a Pigment layer and
          // perfectly smooth on an RGB one -- which is most layers -- with no
          // control disabled and nothing said. `grainCoverageAt()` returns its
          // input bit-identical when grain is off, so adding it changes
          // nothing for a brush that has not turned it on.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const std::array<float, 4> before = srcTile->readPixel(local);
          const float removed = eraseRead != nullptr ? eraseRead->at(local) : 0.0f;
          // **The selection enters TWICE, and both are load-bearing** -- §3.
          // Into the weight, so one pass through a half-selected texel removes
          // half of what it would have; and into the floor, so *no number of
          // passes* takes that texel past half. The first alone is a speed limit
          // rather than a bound, and a scrubbed stroke walks straight through it
          // -- which on an eraser means the paint outside a feathered selection
          // edge is gone and the undo step that would bring it back covers the
          // whole stroke.
          const RgbEraseStep step =
              eraseRgbTexel(before, removed, tip.flow * cov * sel, strength_ * sel);
          // The floor, the transparent tail of the falloff, a texel the
          // selection excluded and a texel with nothing on it all arrive here as
          // `dabAlpha == 0`, and all four mean the same thing: do not touch this
          // texel, do not unshare its tile, do not report it dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            // `getOrCreate` unshares a copy-on-write tile, so after it the
            // store's tile at this coordinate is a *different object* and
            // `srcTile` would keep showing the pre-write value --
            // core/TileStore.hpp calls that "detached", and a stroke reading
            // through a detached pointer would erase every dab after the first
            // from the pre-stroke texel, so a scrubbed erase would repeatedly
            // remove the same first bite and never get deeper.
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (eraseWrite == nullptr) {
            // `getOrCreate` on a store nobody else holds is an allocate-or-find
            // with no copy behind it -- the accumulator is never copied out of
            // the stroke, so the copy-on-write barrier never fires. If the tile
            // already existed, this is the same object `eraseRead` names.
            eraseWrite = &erased_.getOrCreate(coord);
            eraseRead = eraseWrite;
          }
          eraseWrite->set(local, step.strokeErase);
          dst->writePixel(local, step.premultiplied);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit RgbEraseStroke::eraseDabs(TileStore& store, const BrushTip& tip,
                                        const std::vector<Vec2>& dabs, int32_t canvasW,
                                        int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = eraseDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
