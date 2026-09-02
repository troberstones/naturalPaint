#include "brush/PencilDeposit.hpp"

#include <algorithm>

namespace np {

float pencilCoverage(float coverage) noexcept {
  // `!(c >= t)` and not `c < t`: the two differ on NaN, where this form
  // refuses (0) and the other admits (1). Same guard shape as
  // `depositRgbTexel()`'s four refusals, and here it is the difference between
  // a NaN coverage drawing a texel at full opacity and it drawing nothing.
  return !(coverage >= kPencilCoverageThreshold) ? 0.0f : 1.0f;
}

void PencilStroke::begin(const std::array<float, 3>& straightLinearRgb, float opacity,
                         bool alphaLocked) noexcept {
  ink_ = straightLinearRgb;
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  alphaLocked_ = alphaLocked;
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  alpha_ = StrokeAlphaStore{};
  active_ = true;
}

void PencilStroke::end() noexcept {
  alpha_ = StrokeAlphaStore{};
  active_ = false;
}

float PencilStroke::strokeAlphaAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = alpha_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount PencilStroke::drawDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                   int32_t canvasW, int32_t canvasH, const Selection* selection,
                                   std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  // **No `tip.flow` guard, unlike both sibling routes** -- header §2's last
  // paragraph. A pencil has no flow, so a preset that happens to carry a zero
  // one must not silently switch the tool off.
  if (!(opacity_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from all three sibling
  // routes -- the shape of a dab is not a property of what the dab does with
  // it, and this module modifies the *result* of the falloff rather than
  // computing a second one (§1). That matters more here than anywhere: a
  // painter alternates pencil and brush over one edge, and a pencil whose disc
  // was one texel wider than the brush's would show as a rim of the wrong
  // colour rather than as a hard edge.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel, across the three stores keyed by the same
  // coordinate (the layer, the accumulator, the selection). Ascending (y, x)
  // so `touchedOut` comes out in `sortUniqueTiles()`'s order for the common
  // single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // §3. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of
      // every hoisted loop -- a null *Selection* is "no restriction" and a
      // null *tile* inside an engaged selection is "selects nothing". Getting
      // those two nulls the same way round is how a pencil starts drawing
      // outside the ants, or stops drawing at all.
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
      // texel this stroke has not reached has `A == 0`, and that is exactly
      // what an absent tile says, so a dab that turns out to change nothing
      // never allocates the 64 KiB.
      const StrokeAlphaTile* alphaRead = alpha_.find(coord);

      // The layer's READ handle, likewise possibly absent -- an unwritten tile
      // is transparent black, which is what `core::Tile`'s value-initialized
      // texels already say. **Rebound to the write handle the moment there is
      // one**, at the `getOrCreate` below: that call unshares a copy-on-write
      // tile, so afterwards the store's tile at this coordinate is a different
      // object and this pointer would keep showing the pre-write value
      // (core/TileStore.hpp calls it "detached").
      const Tile* srcTile = store.find(coord);

      // Both write handles, fetched lazily at the first texel this dab
      // actually changes -- brush/Deposit §3, fact 2. A tile the bounding box
      // clipped but the disc missed, or one every texel of which the previous
      // dab already took to the ceiling (which per §2 is the common case), is
      // never created and never reported.
      Tile* dst = nullptr;
      StrokeAlphaTile* alphaWrite = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;

          // Paper tooth, at this texel's ABSOLUTE canvas position -- `x`/`y`,
          // not `dx`/`dy`, which is why it cannot live inside `dabCoverage()`.
          // Identical line and identical reasoning to the three sibling
          // routes' (brush/Deposit.cpp §2e).
          const float grained = grainCoverageAt(tip.grain, rawCov, x, y);

          // §1. The threshold is the LAST thing that happens to a coverage,
          // after the tip's own profile and after the paper -- which is what
          // makes the binary guarantee hold for a sampled bitmap tip, a Dual
          // Brush and grain, none of which `hardness` reaches. Grain becomes a
          // keep/drop speckle rather than a grey, which is what graphite on a
          // toothed paper actually is.
          const float cov = pencilCoverage(grained);
          if (!(cov > 0.0f)) continue;

          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const std::array<float, 4> before =
              srcTile != nullptr ? srcTile->readPixel(local)
                                 : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          const float accumulated = alphaRead != nullptr ? alphaRead->at(local) : 0.0f;
          // §§2-4. The weight is the selection's coverage and nothing else --
          // no flow, and `cov` is exactly 1 here by construction -- so the
          // first dab over this texel lands `a1` on `opacity_ * sel` and every
          // later one takes `depositRgbTexel()`'s `!(a0 < cap)` refusal. The
          // composite itself is `brush/RgbDeposit`'s, unmodified, for §4's
          // reason: nothing about a pencil changes what a covered texel
          // *becomes*.
          const RgbDepositStep step =
              depositRgbTexel(before, ink_, accumulated, sel, opacity_ * sel, alphaLocked_);
          // The ceiling (which is every dab after the first), the region
          // outside the disc, a texel the threshold dropped and a texel the
          // selection excluded all arrive here as `dabAlpha == 0`, and all four
          // mean the same thing: do not touch this texel, do not allocate its
          // tile, do not report it dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            srcTile = dst;  // never read the pre-unshare pointer again
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (alphaWrite == nullptr) {
            // `getOrCreate` on a store nobody else holds is an allocate-or-find
            // with no copy behind it -- the accumulator is never copied out of
            // the stroke, so the copy-on-write barrier never fires. If the tile
            // already existed, this is the same object `alphaRead` names.
            alphaWrite = &alpha_.getOrCreate(coord);
            alphaRead = alphaWrite;
          }
          alphaWrite->set(local, step.strokeAlpha);
          dst->writePixel(local, step.premultiplied);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit PencilStroke::drawDabs(TileStore& store, const BrushTip& tip,
                                     const std::vector<Vec2>& dabs, int32_t canvasW,
                                     int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = drawDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
