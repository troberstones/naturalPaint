#include "brush/CloneStamp.hpp"

#include <algorithm>
#include <cmath>

namespace np {

CloneStampStep cloneStampTexel(const std::array<float, 4>& dst, const std::array<float, 4>& src,
                               float strokeAlpha, float weight, float opacity,
                               bool alphaLocked) noexcept {
  CloneStampStep out;
  // The no-op answer, returned by every one of the five refusals below. `dst`
  // rather than something recomputed from it -- see the header's note on
  // bit-identity: a stroke that has reached its ceiling must leave the tile it
  // is scrubbing over completely alone.
  out.premultiplied = dst;
  out.strokeAlpha = strokeAlpha;
  out.dabAlpha = 0.0f;

  const float cap = std::clamp(opacity, 0.0f, 1.0f);
  const float a0 = std::clamp(strokeAlpha, 0.0f, 1.0f);
  const float headroom = 1.0f - a0;

  // `!(x > 0)` rather than `x <= 0` throughout, so a NaN weight or a NaN
  // accumulator refuses instead of propagating into the layer -- the same guard
  // shape `depositRgbTexel()`, `eraseRgbTexel()` and `layerCoverage()` all use.
  if (!(weight > 0.0f)) return out;    // no coverage, or the dab has no flow
  if (!(cap > 0.0f)) return out;       // a stroke asked to transfer nothing
  if (!(headroom > 0.0f)) return out;  // A == 1: done here, and the divisor's zero
  if (!(a0 < cap)) return out;         // the ceiling is already reached

  // **Nothing at the source** -- header §4. All four channels, not the alpha
  // alone: a texel holding colour at alpha 0 is malformed rather than empty, and
  // this function must not be the thing that declares it absent. Exact equality
  // is the right test and not a tolerance, because the question is "has anything
  // ever been written there", and an unwritten `core::Tile` texel is exactly four
  // zero half words. Skipping is arithmetic and not an optimisation: at `src == 0`
  // the composite below is `dst * (1 - 0)`, which is `dst` bit for bit.
  if (src[0] == 0.0f && src[1] == 0.0f && src[2] == 0.0f && src[3] == 0.0f) return out;

  // `brush/RgbDeposit` §2, unchanged: `a1` is the stroke's total after this dab,
  // capped at the opacity; `a` is the coverage for which one composite lands the
  // total exactly there, from the identity `1 - a1 = (1 - a0)(1 - a)`.
  float a1 = a0 + weight * headroom;
  if (a1 > cap) a1 = cap;
  float a = (a1 - a0) / headroom;
  // Algebraically `a <= 1` always, since `a1 <= 1`. Clamped anyway because the
  // subtraction and the division are each rounded, and an `a` of 1+1ulp would
  // make the keep factor negative -- storing a texel with negative alpha, which
  // core/Composite reads straight into its accumulator. Same discipline as both
  // sibling routes clamping the same range.
  if (a > 1.0f) a = 1.0f;

  // Header §1. The keep factor is `1 - src[3] * a` and NOT `1 - a`: the source
  // carries its own coverage, so a transparent source must leave the destination
  // alone rather than cut a hole in it.
  const float keep = 1.0f - src[3] * a;
  if (alphaLocked) {
    // §1's colour-only form. The `src[3]` that would appear in an
    // un-premultiply cancels against the coverage, so there is no division here
    // and no singularity at `src[3] == 0`. `dst[3]` is copied through rather
    // than recomputed, which is what makes this a freeze rather than a bound a
    // later dab could still move -- `brush/RgbDeposit` §4.5's own argument.
    out.premultiplied = {dst[0] * keep + src[0] * a * dst[3],
                         dst[1] * keep + src[1] * a * dst[3],
                         dst[2] * keep + src[2] * a * dst[3], dst[3]};
  } else {
    out.premultiplied = {src[0] * a + dst[0] * keep, src[1] * a + dst[1] * keep,
                         src[2] * a + dst[2] * keep, src[3] * a + dst[3] * keep};
  }
  out.strokeAlpha = a1;
  out.dabAlpha = a;
  return out;
}

void CloneStampStroke::begin(const TileStore& source, Vec2 offset, float opacity,
                             bool alphaLocked) {
  // **The snapshot, taken before a single texel is written** -- header §2. A
  // copy IS the share (core/TileStore.hpp), so this costs one map node and one
  // atomic increment per existing tile and no tile data at all; the first write
  // to each destination tile unshares it and leaves this holding the pre-stroke
  // bytes.
  source_ = source;
  // Header §3: whole texels, nearest. `std::lround` rather than a cast, so an
  // offset of -0.5 goes to -1 rather than to 0 -- a truncating cast is
  // asymmetric about zero, which would make a leftward clone and a rightward
  // clone of the same magnitude land differently.
  offsetX_ = static_cast<int32_t>(std::lround(offset.x));
  offsetY_ = static_cast<int32_t>(std::lround(offset.y));
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  alphaLocked_ = alphaLocked;
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  alpha_ = StrokeAlphaStore{};
  active_ = true;
}

void CloneStampStroke::end() noexcept {
  alpha_ = StrokeAlphaStore{};
  // The snapshot shares tiles with the live layer, so an application sitting
  // idle after a long clone would otherwise hold every tile the stroke unshared
  // at twice its size.
  source_ = TileStore{};
  active_ = false;
}

float CloneStampStroke::strokeAlphaAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = alpha_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount CloneStampStroke::cloneDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                        int32_t canvasW, int32_t canvasH,
                                        const Selection* selection,
                                        std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(opacity_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from the other three
  // routes -- the shape of a dab is not a property of what the dab does, and a
  // second falloff here would be a second place for the clone and the brush to
  // disagree about where a tip ends.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab across
  // the three stores keyed by the *destination* coordinate (the layer, the
  // accumulator, the selection). Ascending (y, x) so `touchedOut` comes out in
  // `sortUniqueTiles()`'s order for the common single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null *tile*
      // inside an engaged selection is "selects nothing".
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
      // an absent tile says.
      const StrokeAlphaTile* alphaRead = alpha_.find(coord);

      // The layer's READ handle, likewise possibly absent. Rebound to the write
      // handle at the `getOrCreate` below for the "detached pointer" reason
      // core/TileStore.hpp states and both sibling routes repeat.
      const Tile* dstRead = store.find(coord);

      Tile* dst = nullptr;
      StrokeAlphaTile* alphaWrite = nullptr;

      // **The SOURCE tile cannot be hoisted out of the texel loop the way the
      // other three are.** The offset is constant but it is not tile-aligned, so
      // one destination tile reads from up to four source tiles. A one-entry
      // memo instead: along a scanline the source tile changes at most once, so
      // this is one hash lookup per tile transition rather than one per texel,
      // which is the same saving the hoist buys the other three stores.
      const Tile* srcTile = nullptr;
      TileCoord srcCoord{0, 0};
      bool srcCoordValid = false;

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
          // (§2e). Applied at the DESTINATION position, not the source: grain is
          // the tooth of the paper the mark is being made on, and a clone that
          // carried the source's grain with it would reproduce the texture of
          // where the paint came from rather than of where it is going.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          // The source read, out of the pre-stroke snapshot and never out of
          // `store` -- header §2, and the whole reason this module exists as
          // something other than a colour swap in `brush/RgbDeposit`.
          //
          // Out of canvas reads as four zeros rather than clamping to the edge
          // (§4): a clamp would smear the border row across everything sampled
          // past it, which looks like a working clone and is not one.
          const int32_t sx = x + offsetX_;
          const int32_t sy = y + offsetY_;
          std::array<float, 4> src{0.0f, 0.0f, 0.0f, 0.0f};
          if (sx >= 0 && sy >= 0 && sx < canvasW && sy < canvasH) {
            const PixelCoord sp{sx, sy};
            const TileCoord sc = tileCoordAt(sp);
            if (!srcCoordValid || !(sc == srcCoord)) {
              srcCoord = sc;
              srcCoordValid = true;
              srcTile = source_.find(sc);
            }
            if (srcTile != nullptr) src = srcTile->readPixel(tileLocalOffset(sp));
          }

          const std::array<float, 4> before =
              dstRead != nullptr ? dstRead->readPixel(local)
                                 : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          const float accumulated = alphaRead != nullptr ? alphaRead->at(local) : 0.0f;
          // **The selection enters TWICE, and both are load-bearing** -- the
          // argument `brush/RgbDeposit` §4 derives by measurement and
          // `brush/RgbErase` §3 restates. Into the weight, so one pass through a
          // half-selected texel transfers half of what it would; and into the
          // ceiling, so *no number of passes* takes that texel past half. The
          // first alone is a speed limit rather than a bound, and a scrubbed
          // stroke walks straight through it.
          const CloneStampStep step =
              cloneStampTexel(before, src, accumulated, tip.flow * cov * sel, opacity_ * sel,
                              alphaLocked_);
          // The ceiling, the transparent tail of the falloff, a texel the
          // selection excluded and a texel with nothing at its source all arrive
          // here as `dabAlpha == 0`, and all four mean the same thing: do not
          // touch this texel, do not unshare its tile, do not report it dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            dstRead = dst;  // never read the pre-unshare pointer again
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
            // **`getOrCreate` above is what unshares this tile from the
            // snapshot**, so `srcTile` may now name the tile the snapshot kept
            // rather than the one `store` holds -- which is exactly the point.
            // The memo is deliberately NOT invalidated here: it points into
            // `source_`, which nothing in this loop writes.
          }
          if (alphaWrite == nullptr) {
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

StrokeDeposit CloneStampStroke::cloneDabs(TileStore& store, const BrushTip& tip,
                                          const std::vector<Vec2>& dabs, int32_t canvasW,
                                          int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = cloneDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
