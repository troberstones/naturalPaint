#include "brush/Heal.hpp"

#include <algorithm>
#include <cmath>

#include "ops/Poisson.hpp"

namespace np {

std::array<float, 4> healClampTexel(std::array<float, 4> texel) noexcept {
  // Header: alpha is a coverage, colour is light. `!(x > 0)` rather than
  // `x <= 0` so a NaN out of the solve lands on the zero branch instead of
  // being stored -- the same guard shape every texel step in `brush/` uses.
  float a = texel[3];
  if (!(a > 0.0f)) a = 0.0f;
  if (a > 1.0f) a = 1.0f;
  if (!(a > 0.0f)) return {0.0f, 0.0f, 0.0f, 0.0f};  // no coverage carries no colour
  for (int c = 0; c < 3; ++c)
    if (!(texel[static_cast<size_t>(c)] > 0.0f)) texel[static_cast<size_t>(c)] = 0.0f;
  texel[3] = a;
  return texel;
}

void HealStroke::begin(const TileStore& source, Vec2 offset, float opacity, bool alphaLocked) {
  // The snapshot, taken before a single texel is written -- header §2. A copy IS
  // the share, so this costs one map node and one atomic increment per existing
  // tile and no tile data at all.
  source_ = source;
  // Header §3: whole texels, nearest. `std::lround` rather than a cast, so an
  // offset of -0.5 goes to -1 rather than to 0 -- a truncating cast is
  // asymmetric about zero, which would make a leftward heal and a rightward one
  // of the same magnitude land differently.
  offsetX_ = static_cast<int32_t>(std::lround(offset.x));
  offsetY_ = static_cast<int32_t>(std::lround(offset.y));
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  alphaLocked_ = alphaLocked;
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held.
  alpha_ = StrokeAlphaStore{};
  active_ = true;
}

void HealStroke::end() noexcept {
  alpha_ = StrokeAlphaStore{};
  source_ = TileStore{};
  active_ = false;
}

float HealStroke::strokeAlphaAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = alpha_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount HealStroke::healDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                 int32_t canvasW, int32_t canvasH, const Selection* selection,
                                 std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(opacity_ > 0.0f)) return count;

  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  // Header §1: the dab's box grown by one texel of Dirichlet skin, clipped to
  // the canvas. Every texel the dab can write is then an INTERIOR texel of the
  // patch and gets a real correction -- without the growth the rim of the dab
  // would be pinned to what was already there, which is a one-texel ring of
  // un-healed copy around every dab and the seam the tool exists to remove.
  const int32_t px0 = std::max<int32_t>(0, b.x0 - 1);
  const int32_t py0 = std::max<int32_t>(0, b.y0 - 1);
  const int32_t px1 = std::min<int32_t>(canvasW - 1, b.x1 + 1);
  const int32_t py1 = std::min<int32_t>(canvasH - 1, b.y1 + 1);
  const int32_t pw = px1 - px0 + 1;
  const int32_t ph = py1 - py0 + 1;
  if (pw <= 0 || ph <= 0) return count;
  const size_t patchN = static_cast<size_t>(pw) * static_cast<size_t>(ph);

  // **Both patches are gathered before a single texel is written** -- header §2.
  // That, and not the store either one is read from, is what makes one dab's
  // answer independent of the order its texels are visited in.
  std::vector<std::array<float, 4>> src(patchN, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
  std::vector<std::array<float, 4>> dst(patchN, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
  for (int32_t y = py0; y <= py1; ++y) {
    for (int32_t x = px0; x <= px1; ++x) {
      const size_t i = static_cast<size_t>(y - py0) * static_cast<size_t>(pw) +
                       static_cast<size_t>(x - px0);
      const PixelCoord dp{x, y};
      const Tile* dstTile = store.find(tileCoordAt(dp));
      if (dstTile != nullptr) dst[i] = dstTile->readPixel(tileLocalOffset(dp));
      // The source read, out of the pre-stroke snapshot and never out of
      // `store`. Out of canvas reads as four zeros rather than clamping to the
      // edge -- a clamp would smear the border row across everything sampled
      // past it, which looks like a working heal and is not one
      // (brush/CloneStamp §4, same rule).
      const int32_t sx = x + offsetX_;
      const int32_t sy = y + offsetY_;
      if (sx < 0 || sy < 0 || sx >= canvasW || sy >= canvasH) continue;
      const PixelCoord sp{sx, sy};
      const Tile* srcTile = source_.find(tileCoordAt(sp));
      if (srcTile != nullptr) src[i] = srcTile->readPixel(tileLocalOffset(sp));
    }
  }

  // `ops/Poisson` §0. The rim comes back as `dst` bit for bit, so the skin this
  // function grew is exactly the part the composite below will find nothing to
  // do at.
  const std::vector<std::array<float, 4>> healed = healPatch(src, dst, pw, ph);

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile -- `brush/CloneStamp::cloneDab()`'s loop,
  // unchanged, because the bookkeeping it does (one hash lookup per tile per dab
  // across the layer, the accumulator and the selection; `touchedOut` appended
  // at the moment of the first write) is not a property of what the dab
  // computes. The one difference is that the source is now an index into
  // `healed` rather than a lookup, which is why this loop has no source-tile
  // memo: the patch is already materialised.
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

      const StrokeAlphaTile* alphaRead = alpha_.find(coord);
      // The layer's READ handle, possibly absent. Rebound to the write handle at
      // the `getOrCreate` below for the "detached pointer" reason
      // core/TileStore.hpp states.
      const Tile* dstRead = store.find(coord);

      Tile* dstTile = nullptr;
      StrokeAlphaTile* alphaWrite = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;

          // Paper tooth at this texel's ABSOLUTE canvas position, exactly as
          // every other layer-writing route computes it (`grainReachesRoute()`
          // in app/StrokeSession.hpp asserts that this route answers the
          // question rather than inheriting an answer). At the DESTINATION
          // position, not the source: grain is the tooth of the paper the mark
          // is being made on.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const size_t pi = static_cast<size_t>(y - py0) * static_cast<size_t>(pw) +
                            static_cast<size_t>(x - px0);
          const std::array<float, 4> ink = healClampTexel(healed[pi]);

          const std::array<float, 4> before =
              dstRead != nullptr ? dstRead->readPixel(local)
                                 : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          const float accumulated = alphaRead != nullptr ? alphaRead->at(local) : 0.0f;
          // **`cloneStampTexel()`, by call and not by copy** -- header §0. The
          // selection enters twice, and both are load-bearing
          // (`brush/RgbDeposit` §4): into the weight, so one pass through a
          // half-selected texel transfers half of what it would; and into the
          // ceiling, so no number of passes takes that texel past half.
          const CloneStampStep step =
              cloneStampTexel(before, ink, accumulated, tip.flow * cov * sel, opacity_ * sel,
                              alphaLocked_);
          // The ceiling, the transparent tail of the falloff, a texel the
          // selection excluded and a texel whose healed value is four zeros all
          // arrive here as `dabAlpha == 0`, and all four mean the same thing: do
          // not touch this texel, do not unshare its tile, do not report it
          // dirty.
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dstTile == nullptr) {
            dstTile = &store.getOrCreate(coord);
            dstRead = dstTile;  // never read the pre-unshare pointer again
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (alphaWrite == nullptr) {
            alphaWrite = &alpha_.getOrCreate(coord);
            alphaRead = alphaWrite;
          }
          alphaWrite->set(local, step.strokeAlpha);
          dstTile->writePixel(local, step.premultiplied);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit HealStroke::healDabs(TileStore& store, const BrushTip& tip,
                                   const std::vector<Vec2>& dabs, int32_t canvasW,
                                   int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = healDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
