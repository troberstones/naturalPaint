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

RgbDepositStep depositRgbTexelBlended(const std::array<float, 4>& dst0,
                                      const std::array<float, 3>& straightLinearRgb,
                                      BlendMode blend, float strokeAlpha, float weight,
                                      float opacity, bool alphaLocked) noexcept {
  RgbDepositStep out;
  // The no-op answer -- `dst0`, for the identical reason `depositRgbTexel()`
  // returns `dst` unchanged: a refused dab must be indistinguishable from one
  // that never ran, and the caller never writes this value when `dabAlpha ==
  // 0` regardless.
  out.premultiplied = dst0;
  out.strokeAlpha = strokeAlpha;
  out.dabAlpha = 0.0f;

  const float cap = std::clamp(opacity, 0.0f, 1.0f);
  const float a0 = std::clamp(strokeAlpha, 0.0f, 1.0f);
  const float headroom = 1.0f - a0;

  // Bit-for-bit the same four refusals, on the same guard shape, as
  // `depositRgbTexel()` -- header §2a's own note on why this is duplicated
  // rather than shared.
  if (!(weight > 0.0f)) return out;
  if (!(cap > 0.0f)) return out;
  if (!(headroom > 0.0f)) return out;
  if (!(a0 < cap)) return out;

  float a1 = a0 + weight * headroom;
  if (a1 > cap) a1 = cap;
  float a = (a1 - a0) / headroom;
  if (a > 1.0f) a = 1.0f;

  // Header §2a: the blend target, computed ONCE from the latched `dst0` --
  // never from a live/intermediate value -- with the ink as an OPAQUE source
  // (alpha 1). `blendPixel()`'s own three-term Porter-Duff split collapses
  // under that to exactly `lerp(ink, blend(straight(dst0), ink), dst0.a)`,
  // which is what makes this line also the "blend over a transparent
  // destination is the source colour" rule, with no separate branch for it.
  const std::array<float, 4> opaqueInk{straightLinearRgb[0], straightLinearRgb[1],
                                       straightLinearRgb[2], 1.0f};
  const std::array<float, 4> blended = blendPixel(blend, opaqueInk, dst0);
  const std::array<float, 3> target{blended[0], blended[1], blended[2]};

  // `keep` is against the CUMULATIVE `a1`, not the per-dab `a` -- header
  // §2a's whole point: the composite is written directly against `dst0` and
  // the stroke's running total, never against an intermediate write, so it
  // is exact and order-independent within the stroke.
  const float keep = 1.0f - a1;
  if (alphaLocked) {
    // §2a's re-derivation of §4.5: the identical structure with `dst0` and
    // `a1` standing in for `dst`/`a`, because the per-dab lerp toward a
    // CONSTANT target is the same repeated-composite identity, just at the
    // straight-colour level.
    out.premultiplied = {dst0[0] * keep + target[0] * a1 * dst0[3],
                         dst0[1] * keep + target[1] * a1 * dst0[3],
                         dst0[2] * keep + target[2] * a1 * dst0[3], dst0[3]};
  } else {
    // out = dst0*(1-A') + B'*A', B' the (opaque) blend target -- header
    // §2a's headline formula.
    out.premultiplied = {target[0] * a1 + dst0[0] * keep, target[1] * a1 + dst0[1] * keep,
                         target[2] * a1 + dst0[2] * keep, a1 + dst0[3] * keep};
  }
  out.strokeAlpha = a1;
  out.dabAlpha = a;
  return out;
}

void RgbStroke::begin(const std::array<float, 3>& straightLinearRgb, float opacity,
                      bool alphaLocked, BlendMode blend) noexcept {
  ink_ = straightLinearRgb;
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  alphaLocked_ = alphaLocked;
  blend_ = blend;
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  alpha_ = StrokeAlphaStore{};
  // Likewise §2a's latch store -- fresh, not cleared, and stays a fresh
  // (empty) store for the stroke's whole life when `blend_ == Normal`: no
  // code path below ever calls `dst0_.getOrCreate()` in that case.
  dst0_ = StrokeDst0Store{};
  active_ = true;
}

void RgbStroke::end() noexcept {
  alpha_ = StrokeAlphaStore{};
  dst0_ = StrokeDst0Store{};
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

      // §2a: the latched-`dst0` store's own read/write handles, hoisted the
      // identical way -- but ONLY looked up at all when this stroke is
      // blended, so an unblended stroke pays the extra `find()` nothing (not
      // even a lookup into an empty map) and its loop is the exact one it
      // always was.
      const bool blending = blend_ != BlendMode::Normal;
      const Tile* dst0Read = blending ? dst0_.find(coord) : nullptr;
      Tile* dst0Write = nullptr;

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
          //
          // §2a's dispatch: `blend_ == Normal` runs the EXACT branch and
          // arithmetic this loop always ran, against the live `before`.
          // Otherwise, `dst0` is latched from `before` the first time this
          // TEXEL is touched THIS STROKE -- `accumulated == 0` is that
          // signal, the same one `depositRgbTexel()`'s own `a0 < cap` refusal
          // already reads as "this stroke has not reached it yet" -- and read
          // back from `dst0Read` every dab after, never from the live tile.
          RgbDepositStep step;
          std::array<float, 4> dst0Val{};
          const bool firstTouch = blending && !(accumulated > 0.0f);
          if (blending) {
            dst0Val = firstTouch ? before
                                 : (dst0Read != nullptr ? dst0Read->readPixel(local)
                                                        : std::array<float, 4>{0.0f, 0.0f, 0.0f,
                                                                               0.0f});
            step = depositRgbTexelBlended(dst0Val, ink_, blend_, accumulated,
                                          tip.flow * cov * sel, opacity_ * sel, alphaLocked_);
          } else {
            step = depositRgbTexel(before, ink_, accumulated, tip.flow * cov * sel,
                                   opacity_ * sel, alphaLocked_);
          }
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
          if (firstTouch) {
            // Latch `dst0` for every later dab this stroke spends on this
            // texel -- written ONCE per texel per stroke, exactly when
            // `accumulated` was 0 above, never again (a second write here
            // would be latching an already-blended value, the compounding
            // bug §2a exists not to have).
            if (dst0Write == nullptr) {
              dst0Write = &dst0_.getOrCreate(coord);
              dst0Read = dst0Write;  // see the `srcTile` comment above: same
                                     // detached-pointer discipline
            }
            dst0Write->writePixel(local, dst0Val);
          }
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
