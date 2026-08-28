#include "brush/RgbDeposit.hpp"

#include <algorithm>

namespace np {

RgbDepositStep depositRgbTexel(const std::array<float, 4>& dst,
                               const std::array<float, 3>& straightLinearRgb, float strokeAlpha,
                               float weight, float opacity, bool alphaLocked) noexcept {
  RgbDepositStep out;
  // The no-op answer, returned by every one of the four refusals below. `dst`
  // rather than something recomputed from it: a texel this dab does not change
  // must come back **bit-identical**, or a stroke that has reached its ceiling
  // would keep perturbing the tile it is scrubbing over and the caller's
  // "nothing to do here" test would never fire.
  out.premultiplied = dst;
  out.strokeAlpha = strokeAlpha;
  out.dabAlpha = 0.0f;

  const float cap = std::clamp(opacity, 0.0f, 1.0f);
  const float a0 = std::clamp(strokeAlpha, 0.0f, 1.0f);
  const float headroom = 1.0f - a0;

  // `!(x > 0)` rather than `x <= 0` throughout, so a NaN weight or a NaN
  // accumulator refuses instead of propagating into the layer -- the same
  // guard shape `depositDab()` and `layerCoverage()` both use.
  if (!(weight > 0.0f)) return out;      // no coverage, or the dab has no flow
  if (!(cap > 0.0f)) return out;         // a stroke asked to reach nothing
  if (!(headroom > 0.0f)) return out;    // A == 1: opaque, and the divisor's zero
  if (!(a0 < cap)) return out;           // the ceiling is already reached (§2)

  // Header §2. `a1` is the stroke's total after this dab, capped; `a` is the
  // composite alpha for which one source-over lands the total exactly there,
  // from the identity `1 - a1 = (1 - a0)(1 - a)`.
  float a1 = a0 + weight * headroom;
  if (a1 > cap) a1 = cap;
  float a = (a1 - a0) / headroom;
  // Algebraically `a <= 1` always, since `a1 <= 1`. Clamped anyway because the
  // subtraction and the division are each rounded and the *only* thing standing
  // between an `a` of 1+1ulp and a stored alpha above 1 is this line -- and an
  // alpha above 1 is a document no compositor in this codebase has a meaning
  // for (core/Composite reads it straight into the accumulator). Same
  // discipline as `kMaxMass` clamping at the point of storage rather than at
  // the reader.
  if (a > 1.0f) a = 1.0f;

  const float keep = 1.0f - a;
  if (alphaLocked) {
    // Header §4.5: the SAME `a`, spent on colour only. `dst[3]` is copied
    // through rather than recomputed, which is what makes this a freeze
    // rather than a bound that a second dab could still move -- there is no
    // expression here `dst[3]` is an input to, so there is nothing left for a
    // later pass to climb.
    out.premultiplied = {dst[0] * keep + straightLinearRgb[0] * a * dst[3],
                         dst[1] * keep + straightLinearRgb[1] * a * dst[3],
                         dst[2] * keep + straightLinearRgb[2] * a * dst[3], dst[3]};
  } else {
    // Premultiplied source-over of an OPAQUE source scaled by `a`:
    //     s' = (rgb * a, a),   out = s' + dst * (1 - a)
    // All four channels take the same `keep`, which is what makes a rim texel
    // half *present* rather than half *bright* -- the identical argument
    // `fillThroughSelection()` makes for the bucket's feathered edge, and the
    // reason there is no fringe (§1).
    out.premultiplied = {straightLinearRgb[0] * a + dst[0] * keep,
                         straightLinearRgb[1] * a + dst[1] * keep,
                         straightLinearRgb[2] * a + dst[2] * keep, a + dst[3] * keep};
  }
  out.strokeAlpha = a1;
  out.dabAlpha = a;
  return out;
}

void RgbStroke::begin(const std::array<float, 3>& straightLinearRgb, float opacity,
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

void RgbStroke::end() noexcept {
  alpha_ = StrokeAlphaStore{};
  active_ = false;
}

float RgbStroke::strokeAlphaAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = alpha_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount RgbStroke::depositDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                   int32_t canvasW, int32_t canvasH, const Selection* selection,
                                   std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(opacity_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from the pigment route --
  // the shape of a dab is not a property of what it is made of, and a second
  // falloff here would be a second place for the two routes to disagree about
  // where a brush ends.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel -- and here there are *three* stores keyed by the
  // same coordinate (the layer, the accumulator, the selection), so hoisting
  // saves three lookups per texel rather than one. Ascending (y, x) so
  // `touchedOut` comes out in `sortUniqueTiles()`'s order for the common
  // single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // §4. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null
      // *tile* inside an engaged selection is "selects nothing". Getting those
      // two nulls the same way round is how a brush starts painting outside the
      // ants, or stops painting at all.
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        // An engaged selection that names no tile here selects nothing here, so
        // the whole tile is skipped before anything is looked up or allocated.
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
      // never allocates the 64 KiB (§3).
      const StrokeAlphaTile* alphaRead = alpha_.find(coord);

      // The layer's READ handle, likewise possibly absent -- an unwritten tile
      // is transparent black, which is what `core::Tile`'s value-initialized
      // texels already say, so a miss reads as zeros rather than allocating.
      //
      // **Rebound to the write handle the moment there is one**, at the
      // `getOrCreate` below. That is not tidiness: `getOrCreate` unshares a
      // copy-on-write tile, so after it the store's tile at this coordinate is
      // a *different object* and this pointer would keep showing the pre-write
      // value -- core/TileStore.hpp calls that "detached", and a stroke reading
      // through a detached pointer would composite every dab after the first
      // against the pre-stroke texel.
      const Tile* srcTile = store.find(coord);

      // Both write handles, fetched lazily at the first texel this dab actually
      // changes -- brush/Deposit §3, fact 2. A tile the bounding box clipped
      // but the disc missed, or one every texel of which has already reached
      // the ceiling, is never created and never reported.
      Tile* dst = nullptr;
      StrokeAlphaTile* alphaWrite = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float cov = dabCoverage(tip, dx, dy);
          if (!(cov > 0.0f)) continue;
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const std::array<float, 4> before =
              srcTile != nullptr ? srcTile->readPixel(local)
                                 : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          const float accumulated = alphaRead != nullptr ? alphaRead->at(local) : 0.0f;
          // **The selection enters TWICE, and both are load-bearing** -- §4.
          // Into the weight, so one pass through a half-selected texel lays
          // half a dab (which is the paint bucket's `coverage * opacity`
          // exactly); and into the ceiling, so *no number of passes* takes that
          // texel past half. The first alone is a speed limit rather than a
          // bound, and a scrubbed stroke walks straight through it.
          const RgbDepositStep step = depositRgbTexel(before, ink_, accumulated,
                                                      tip.flow * cov * sel, opacity_ * sel,
                                                      alphaLocked_);
          // The ceiling, the transparent tail of the falloff, and a texel the
          // selection excluded all arrive here as `dabAlpha == 0`, and all three
          // mean the same thing: do not touch this texel, do not allocate its
          // tile, do not report it dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            srcTile = dst;  // see the `srcTile` comment above: never read the
                            // pre-unshare pointer again
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (alphaWrite == nullptr) {
            // `getOrCreate` on a store nobody else holds is an allocate-or-find
            // with no copy behind it -- the accumulator is never copied out of
            // the stroke (§3), so the copy-on-write barrier never fires. If the
            // tile already existed, this is the same object `alphaRead` names.
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

StrokeDeposit RgbStroke::depositDabs(TileStore& store, const BrushTip& tip,
                                     const std::vector<Vec2>& dabs, int32_t canvasW,
                                     int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = depositDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
