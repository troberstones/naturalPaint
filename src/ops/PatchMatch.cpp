#include "ops/PatchMatch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

// The argument for every decision here is in ops/PatchMatch.hpp. What is
// left is Barnes et al.'s search itself (random init, propagation, random
// search, inside a coarse-to-fine pyramid) and the voting reconstruction --
// short, and the places a wrong scan order or a wrong bound would silently
// read or write the hole.
namespace np {
namespace {

// splitmix64's finalizer, ops/Filters.cpp's own copy restated here so this
// file needs no cross-TU dependency on it.
uint64_t pmSplitMix64(uint64_t z) noexcept {
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// A stateless hash of five components -- the seed and four caller-chosen
// coordinates (level, iteration/phase, a pixel's linear index, a draw
// counter) -- so every random decision in the search is a pure function of
// what produced it and nothing else, regardless of scan order.
uint64_t pmHash(uint64_t seed, int64_t a, int64_t b, int64_t c, int64_t d) noexcept {
  uint64_t h = seed ^ 0x243f6a8885a308d3ULL;
  h = pmSplitMix64(h + static_cast<uint64_t>(a));
  h = pmSplitMix64(h + static_cast<uint64_t>(b));
  h = pmSplitMix64(h + static_cast<uint64_t>(c));
  h = pmSplitMix64(h + static_cast<uint64_t>(d));
  return h;
}

// A uniform index in [0, n). `n` is always small (a candidate list, a search
// span), so the modulo bias this invites is negligible.
int32_t pmUniformIndex(uint64_t h, int32_t n) noexcept {
  if (n <= 1) return 0;
  return static_cast<int32_t>((h >> 32) % static_cast<uint64_t>(n));
}

// One pyramid level: a dense gather of the working rectangle, mutated in
// place as the hole is voted on.
struct Level {
  int32_t w = 0, h = 0;
  std::vector<float> img;        // w*h*4, premultiplied RGBA
  std::vector<uint8_t> isHole;   // coverage > 0.5, see ops/PatchMatch.hpp
  std::vector<uint8_t> isExcluded;
  std::vector<int32_t> holeSat;  // summed-area table of isHole, (w+1)*(h+1)

  size_t index(int32_t x, int32_t y) const noexcept {
    return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
  }
};

void buildHoleSat(Level& lv) {
  lv.holeSat.assign(static_cast<size_t>(lv.w + 1) * static_cast<size_t>(lv.h + 1), 0);
  const int32_t stride = lv.w + 1;
  for (int32_t y = 0; y < lv.h; ++y) {
    int32_t rowSum = 0;
    for (int32_t x = 0; x < lv.w; ++x) {
      rowSum += lv.isHole[lv.index(x, y)];
      lv.holeSat[static_cast<size_t>(y + 1) * static_cast<size_t>(stride) +
                static_cast<size_t>(x + 1)] =
          lv.holeSat[static_cast<size_t>(y) * static_cast<size_t>(stride) +
                    static_cast<size_t>(x + 1)] +
          rowSum;
    }
  }
}

// Hole texels within `[x0,x1) x [y0,y1)`, already clamped by the caller to
// the level's own bounds, via the table `buildHoleSat()` built.
int32_t holeCountIn(const Level& lv, int32_t x0, int32_t y0, int32_t x1, int32_t y1) noexcept {
  const size_t stride = static_cast<size_t>(lv.w + 1);
  return lv.holeSat[static_cast<size_t>(y1) * stride + static_cast<size_t>(x1)] -
        lv.holeSat[static_cast<size_t>(y0) * stride + static_cast<size_t>(x1)] -
        lv.holeSat[static_cast<size_t>(y1) * stride + static_cast<size_t>(x0)] +
        lv.holeSat[static_cast<size_t>(y0) * stride + static_cast<size_t>(x0)];
}

// A centre is valid when its whole (2r+1)x(2r+1) window fits inside the
// level, touches no hole texel, and the centre itself is not excluded. This
// is the ONE predicate every accepted candidate -- init, propagation, random
// search -- is checked against, which is what makes "no source patch
// overlaps the hole" true by construction rather than by review.
bool validSourceCentre(const Level& lv, int32_t cx, int32_t cy, int32_t r) noexcept {
  if (cx - r < 0 || cy - r < 0 || cx + r + 1 > lv.w || cy + r + 1 > lv.h) return false;
  if (lv.isExcluded[lv.index(cx, cy)] != 0) return false;
  return holeCountIn(lv, cx - r, cy - r, cx + r + 1, cy + r + 1) == 0;
}

// SSD between the patch at `(tx,ty)` (may read hole texels -- the live
// estimate) and the patch at `(sx,sy)` (always a validated, hole-free
// source). Both windows are fully in bounds by construction of their
// callers.
double patchDistance(const Level& lv, int32_t tx, int32_t ty, int32_t sx, int32_t sy,
                     int32_t r) noexcept {
  double sum = 0.0;
  for (int32_t dy = -r; dy <= r; ++dy) {
    const size_t rowT = lv.index(0, ty + dy);
    const size_t rowS = lv.index(0, sy + dy);
    for (int32_t dx = -r; dx <= r; ++dx) {
      const size_t it = (rowT + static_cast<size_t>(tx + dx)) * static_cast<size_t>(Tile::kChannels);
      const size_t is = (rowS + static_cast<size_t>(sx + dx)) * static_cast<size_t>(Tile::kChannels);
      for (int c = 0; c < Tile::kChannels; ++c) {
        const double diff =
            static_cast<double>(lv.img[it + static_cast<size_t>(c)]) -
            static_cast<double>(lv.img[is + static_cast<size_t>(c)]);
        sum += diff * diff;
      }
    }
  }
  return sum;
}

// A 2x2 box downsample. Colour averages only the NON-hole sub-texels (a
// block that is entirely hole gets a placeholder of 0, which voting
// overwrites before it is ever read as a distance operand); `isHole` and
// `isExcluded` are OR-reductions, so a coarse hole is always a superset of
// the fine hole's footprint -- the property the NNF upsample step depends on.
Level downsample(const Level& fine) {
  Level coarse;
  coarse.w = (fine.w + 1) / 2;
  coarse.h = (fine.h + 1) / 2;
  const size_t area = static_cast<size_t>(coarse.w) * static_cast<size_t>(coarse.h);
  coarse.img.assign(area * static_cast<size_t>(Tile::kChannels), 0.0f);
  coarse.isHole.assign(area, 0);
  coarse.isExcluded.assign(area, 0);
  for (int32_t y = 0; y < coarse.h; ++y) {
    for (int32_t x = 0; x < coarse.w; ++x) {
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      int32_t count = 0;
      bool anyHole = false;
      bool anyExcluded = false;
      for (int32_t dy = 0; dy < 2; ++dy) {
        const int32_t fy = y * 2 + dy;
        if (fy >= fine.h) continue;
        for (int32_t dx = 0; dx < 2; ++dx) {
          const int32_t fx = x * 2 + dx;
          if (fx >= fine.w) continue;
          const size_t fi = fine.index(fx, fy);
          if (fine.isExcluded[fi] != 0) anyExcluded = true;
          if (fine.isHole[fi] != 0) {
            anyHole = true;
            continue;
          }
          ++count;
          const size_t base = fi * static_cast<size_t>(Tile::kChannels);
          for (int c = 0; c < Tile::kChannels; ++c) sum[c] += fine.img[base + static_cast<size_t>(c)];
        }
      }
      const size_t ci = coarse.index(x, y);
      coarse.isHole[ci] = anyHole ? 1 : 0;
      coarse.isExcluded[ci] = anyExcluded ? 1 : 0;
      if (count > 0) {
        const size_t base = ci * static_cast<size_t>(Tile::kChannels);
        for (int c = 0; c < Tile::kChannels; ++c)
          coarse.img[base + static_cast<size_t>(c)] = static_cast<float>(sum[c] / count);
      }
    }
  }
  return coarse;
}

// Reconstructs every hole texel of `lv` as the average of every hole texel
// `c` whose validated source patch covers it -- `c` always qualifies for
// itself (offset (0,0)), so every hole texel gets at least one vote, and
// every vote reads a texel inside SOME `c`'s validated (hole-free) source
// window, which is why this can never read a value voting has not already
// settled. New values are written into a scratch copy so a texel's own vote
// reads its neighbours' values from BEFORE this pass, not mid-update.
void voteLevel(Level& lv, const std::vector<int32_t>& nnfX, const std::vector<int32_t>& nnfY,
              int32_t r) {
  std::vector<float> next = lv.img;
  for (int32_t y = 0; y < lv.h; ++y) {
    for (int32_t x = 0; x < lv.w; ++x) {
      const size_t p = lv.index(x, y);
      if (lv.isHole[p] == 0) continue;
      double acc[4] = {0.0, 0.0, 0.0, 0.0};
      int32_t votes = 0;
      for (int32_t dy = -r; dy <= r; ++dy) {
        const int32_t cy = y + dy;
        if (cy < 0 || cy >= lv.h) continue;
        for (int32_t dx = -r; dx <= r; ++dx) {
          const int32_t cx = x + dx;
          if (cx < 0 || cx >= lv.w) continue;
          const size_t ci = lv.index(cx, cy);
          if (lv.isHole[ci] == 0) continue;
          const int32_t sx = nnfX[ci] + (x - cx);
          const int32_t sy = nnfY[ci] + (y - cy);
          const size_t base =
              lv.index(sx, sy) * static_cast<size_t>(Tile::kChannels);
          for (int c = 0; c < Tile::kChannels; ++c) acc[c] += lv.img[base + static_cast<size_t>(c)];
          ++votes;
        }
      }
      // `votes >= 1` always: dx=dy=0 is `c == p`, and `p` is hole so it has
      // its own NNF entry -- see this function's own comment.
      const size_t base = p * static_cast<size_t>(Tile::kChannels);
      for (int c = 0; c < Tile::kChannels; ++c)
        next[base + static_cast<size_t>(c)] = static_cast<float>(acc[c] / votes);
    }
  }
  lv.img.swap(next);
}

}  // namespace

bool patchMatchParamsValid(const PatchMatchParams& p) noexcept {
  if (p.hole == nullptr) return false;
  if (selectionSelectsNothing(*p.hole)) return false;
  if (p.patchRadius < 1 || p.patchRadius > kPatchMatchMaxPatchRadius) return false;
  if (p.iterations < 1 || p.iterations > kPatchMatchMaxIterations) return false;
  if (p.pyramidLevels < 1 || p.pyramidLevels > kPatchMatchMaxPyramidLevels) return false;
  return p.sourceMargin >= 0 && p.sourceMargin <= kPatchMatchMaxSourceMargin;
}

std::optional<PixelRect> patchMatchHoleBounds(const PatchMatchParams& p) {
  if (p.hole == nullptr) return std::nullopt;
  const std::optional<SelectionBounds> b = selectionBounds(*p.hole);
  if (!b.has_value()) return std::nullopt;
  // `selectionBounds()` bounds every texel of nonzero coverage, which is a
  // superset of this file's own coverage > 0.5 hole -- a safe over-
  // approximation for the rectangle a caller must contain and the scratch
  // must cover, not a claim that it is the tightest possible box for THIS
  // op's own threshold.
  return PixelRect{b->x0, b->y0, b->x1, b->y1};
}

bool patchMatchTiles(const TileStore& src, const PixelRect& outRect, const PatchMatchParams& p,
                     TileStore* dst, std::vector<PatchMatchNnfEntry>* nnfOut) {
  if (nnfOut != nullptr) nnfOut->clear();
  if (dst == nullptr || dst == &src) return false;
  if (!patchMatchParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  const std::optional<PixelRect> holeBoundsOpt = patchMatchHoleBounds(p);
  if (!holeBoundsOpt.has_value()) return false;
  const PixelRect hole = *holeBoundsOpt;
  if (!roiContains(outRect, hole)) return false;
  if (roiTexelCount(hole) > kPatchMatchMaxHoleTexels) return false;

  const int32_t r = p.patchRadius;
  // Never clamped to `outRect` -- ops/Inpaint.hpp's own argument: an absent
  // tile reads as transparent black, which is valid (if usually uninteresting)
  // source data, so a hole near the edge of the painted region needs no
  // special case. The margin is widened to at least `2r + 2` -- not merely
  // `r + 1` -- so at least one valid source centre is guaranteed to exist
  // just outside the hole: a centre needs `r` texels of clearance from the
  // gathered rectangle's own edge AND `r + 1` from the hole's, and those two
  // requirements stack rather than share.
  const int32_t margin = std::max(p.sourceMargin, 2 * r + 2);
  const PixelRect work = roiExpandUniform(hole, margin);
  const int64_t texels = roiTexelCount(work);
  if (texels <= 0 || texels > (static_cast<int64_t>(1) << 30)) return false;

  Level fine;
  fine.w = work.width();
  fine.h = work.height();
  const size_t n = static_cast<size_t>(texels);
  fine.img.assign(n * static_cast<size_t>(Tile::kChannels), 0.0f);
  fine.isHole.assign(n, 0);
  fine.isExcluded.assign(n, 0);

  // Gather -- the identical shape ops/Inpaint.hpp's own gather loop uses.
  {
    const TileRange tiles = roiTileRange(work);
    for (int32_t ty = tiles.y0; ty < tiles.y1; ++ty) {
      for (int32_t tx = tiles.x0; tx < tiles.x1; ++tx) {
        const TileCoord coord{tx, ty};
        const PixelRect span = roiIntersect(roiTileRect(coord), work);
        if (roiIsEmpty(span)) continue;
        const Tile* imageTile = src.find(coord);
        const SelectionTile* holeTile = p.hole->tiles.find(coord);
        const SelectionTile* exTile =
            p.excludeSource != nullptr ? p.excludeSource->tiles.find(coord) : nullptr;
        if (imageTile == nullptr && holeTile == nullptr && exTile == nullptr) continue;
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t y = span.y0; y < span.y1; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            const PixelCoord local{x - origin.x, y - origin.y};
            const size_t i = fine.index(x - work.x0, y - work.y0);
            if (imageTile != nullptr) {
              const std::array<float, 4> rgba = imageTile->readPixel(local);
              const size_t base = i * static_cast<size_t>(Tile::kChannels);
              fine.img[base + 0] = rgba[0];
              fine.img[base + 1] = rgba[1];
              fine.img[base + 2] = rgba[2];
              fine.img[base + 3] = rgba[3];
            }
            // > 0.5, not > 0 -- see ops/PatchMatch.hpp's own section on why.
            if (selectionTileCoverage(holeTile, local) > 0.5f) fine.isHole[i] = 1;
            if (exTile != nullptr && selectionTileCoverage(exTile, local) > 0.5f)
              fine.isExcluded[i] = 1;
          }
        }
      }
    }
  }
  buildHoleSat(fine);

  // The pyramid, finest first. A level is added only while the hole would
  // still have at least `2r + 2` texels of margin to the level's own edge at
  // that resolution -- the identical floor `margin` itself was widened to
  // above, and for the identical reason: `marginLeft` tracks it alongside the
  // dimensions because a level whose hole nearly fills it can run out of
  // margin well before it runs out of raw width or height.
  std::vector<Level> levels;
  levels.push_back(std::move(fine));
  int32_t marginLeft = margin;
  while (static_cast<int32_t>(levels.size()) < p.pyramidLevels) {
    if (marginLeft / 2 < 2 * r + 2) break;
    const Level& prev = levels.back();
    if (std::min(prev.w, prev.h) <= 2 * r + 2) break;
    Level next = downsample(prev);
    if (next.w < 2 * r + 2 || next.h < 2 * r + 2) break;
    buildHoleSat(next);
    levels.push_back(std::move(next));
    marginLeft /= 2;
  }

  std::vector<int32_t> nnfX, nnfY;
  for (int32_t li = static_cast<int32_t>(levels.size()) - 1; li >= 0; --li) {
    Level& lv = levels[static_cast<size_t>(li)];
    const size_t area = static_cast<size_t>(lv.w) * static_cast<size_t>(lv.h);

    // The valid-centre list this level's random draws sample from, built
    // once per level: validity depends on the hole's SHAPE, which does not
    // change while this level is being worked.
    std::vector<PixelCoord> validCentres;
    validCentres.reserve(area);
    for (int32_t y = 0; y < lv.h; ++y)
      for (int32_t x = 0; x < lv.w; ++x)
        if (validSourceCentre(lv, x, y, r)) validCentres.push_back(PixelCoord{x, y});
    // Nothing to synthesise from at all -- a named refusal, not a crash or a
    // fill of black. See ops/PatchMatch.hpp: "Select All" is this case.
    if (validCentres.empty()) return false;

    std::vector<int32_t> curX(area, -1), curY(area, -1);
    if (li == static_cast<int32_t>(levels.size()) - 1) {
      for (int32_t y = 0; y < lv.h; ++y) {
        for (int32_t x = 0; x < lv.w; ++x) {
          const size_t i = lv.index(x, y);
          if (lv.isHole[i] == 0) continue;
          const uint64_t h = pmHash(p.seed, li, -1, static_cast<int64_t>(i), 0);
          const PixelCoord c =
              validCentres[static_cast<size_t>(pmUniformIndex(h, static_cast<int32_t>(validCentres.size())))];
          curX[i] = c.x;
          curY[i] = c.y;
        }
      }
    } else {
      const Level& coarseLv = levels[static_cast<size_t>(li + 1)];
      for (int32_t y = 0; y < lv.h; ++y) {
        for (int32_t x = 0; x < lv.w; ++x) {
          const size_t i = lv.index(x, y);
          if (lv.isHole[i] == 0) continue;
          const int32_t cx = std::min(x / 2, coarseLv.w - 1);
          const int32_t cy = std::min(y / 2, coarseLv.h - 1);
          const size_t ci = coarseLv.index(cx, cy);
          const int32_t sx = nnfX[ci] * 2 + (x - cx * 2);
          const int32_t sy = nnfY[ci] * 2 + (y - cy * 2);
          if (validSourceCentre(lv, sx, sy, r)) {
            curX[i] = sx;
            curY[i] = sy;
          } else {
            const uint64_t h = pmHash(p.seed, li, -1, static_cast<int64_t>(i), 1);
            const PixelCoord c = validCentres[static_cast<size_t>(
                pmUniformIndex(h, static_cast<int32_t>(validCentres.size())))];
            curX[i] = c.x;
            curY[i] = c.y;
          }
        }
      }
    }
    nnfX.swap(curX);
    nnfY.swap(curY);

    voteLevel(lv, nnfX, nnfY, r);

    for (int32_t iter = 0; iter < p.iterations; ++iter) {
      const bool forward = (iter % 2) == 0;
      for (int32_t yy = 0; yy < lv.h; ++yy) {
        const int32_t y = forward ? yy : (lv.h - 1 - yy);
        for (int32_t xx = 0; xx < lv.w; ++xx) {
          const int32_t x = forward ? xx : (lv.w - 1 - xx);
          const size_t i = lv.index(x, y);
          if (lv.isHole[i] == 0) continue;

          int32_t bestX = nnfX[i];
          int32_t bestY = nnfY[i];
          double bestD = patchDistance(lv, x, y, bestX, bestY, r);

          auto tryCandidate = [&](int32_t cx, int32_t cy) {
            if (cx == bestX && cy == bestY) return;
            if (!validSourceCentre(lv, cx, cy, r)) return;
            const double d = patchDistance(lv, x, y, cx, cy, r);
            if (d < bestD) {
              bestD = d;
              bestX = cx;
              bestY = cy;
            }
          };

          // Propagation: a hole neighbour already visited this sweep offers
          // its own match, shifted by one texel -- the move that makes a
          // good match spread across the hole instead of being found
          // independently at every texel.
          if (forward) {
            if (x > 0 && lv.isHole[lv.index(x - 1, y)] != 0) {
              const size_t ni = lv.index(x - 1, y);
              tryCandidate(nnfX[ni] + 1, nnfY[ni]);
            }
            if (y > 0 && lv.isHole[lv.index(x, y - 1)] != 0) {
              const size_t ni = lv.index(x, y - 1);
              tryCandidate(nnfX[ni], nnfY[ni] + 1);
            }
          } else {
            if (x + 1 < lv.w && lv.isHole[lv.index(x + 1, y)] != 0) {
              const size_t ni = lv.index(x + 1, y);
              tryCandidate(nnfX[ni] - 1, nnfY[ni]);
            }
            if (y + 1 < lv.h && lv.isHole[lv.index(x, y + 1)] != 0) {
              const size_t ni = lv.index(x, y + 1);
              tryCandidate(nnfX[ni], nnfY[ni] - 1);
            }
          }

          // Random search: a shrinking window around the current best,
          // halving until it is under one texel.
          int32_t radius = std::max(lv.w, lv.h);
          int64_t step = 0;
          while (radius >= 1) {
            const uint64_t hx = pmHash(p.seed, li, iter, static_cast<int64_t>(i), step * 2);
            const uint64_t hy = pmHash(p.seed, li, iter, static_cast<int64_t>(i), step * 2 + 1);
            const int32_t span = 2 * radius + 1;
            tryCandidate(bestX + (pmUniformIndex(hx, span) - radius),
                        bestY + (pmUniformIndex(hy, span) - radius));
            radius /= 2;
            ++step;
          }

          nnfX[i] = bestX;
          nnfY[i] = bestY;
        }
      }
      voteLevel(lv, nnfX, nnfY, r);
    }
  }

  // Scatter: the hole and nothing else, plus the NNF instrumentation when
  // asked for. `levels[0]` is the finest level -- the one just finished.
  const Level& finest = levels[0];
  {
    const TileRange tiles = roiTileRange(hole);
    for (int32_t ty = tiles.y0; ty < tiles.y1; ++ty) {
      for (int32_t tx = tiles.x0; tx < tiles.x1; ++tx) {
        const TileCoord coord{tx, ty};
        const PixelRect span = roiIntersect(roiTileRect(coord), hole);
        if (roiIsEmpty(span)) continue;
        const PixelCoord origin = tileOrigin(coord);
        bool any = false;
        for (int32_t y = span.y0; y < span.y1 && !any; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            if (finest.isHole[finest.index(x - work.x0, y - work.y0)] != 0) {
              any = true;
              break;
            }
          }
        }
        if (!any) continue;
        Tile& tile = dst->getOrCreate(coord);
        for (int32_t y = span.y0; y < span.y1; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            const size_t i = finest.index(x - work.x0, y - work.y0);
            if (finest.isHole[i] == 0) continue;
            const size_t base = i * static_cast<size_t>(Tile::kChannels);
            tile.writePixel(
                PixelCoord{x - origin.x, y - origin.y},
                {finest.img[base + 0], finest.img[base + 1], finest.img[base + 2],
                 finest.img[base + 3]});
            if (nnfOut != nullptr) {
              nnfOut->push_back({PixelCoord{x, y},
                                 PixelCoord{work.x0 + nnfX[i], work.y0 + nnfY[i]}});
            }
          }
        }
      }
    }
  }

  return true;
}

}  // namespace np
