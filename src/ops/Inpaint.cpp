#include "ops/Inpaint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

// The argument for every decision below is in ops/Inpaint.hpp. What is left
// here is the fast marching method itself, which is short, and the two places
// where the roles of "known" and "unknown" are swapped between the two passes,
// which is where it would be easy to get wrong.
namespace np {
namespace {

// Sethian's three sets, in the order the march moves through them. The
// numeric values are not load-bearing; `kUnknown` being the only value the
// inner loops test for is.
constexpr uint8_t kKnown = 0;    // finalised: T is correct and the texel may be read
constexpr uint8_t kBand = 1;     // on the front: T is provisional
constexpr uint8_t kUnknown = 2;  // not reached yet

// The initial T of an unreached texel. Large enough that a real distance
// never approaches it (the largest hole this file will size a scratch for is
// under 2^16 texels across) and small enough to stay exact in float.
constexpr float kFarT = 1.0e6f;

// The floor under `dir`. A source texel lying exactly perpendicular to the
// front's normal has a zero cosine, and dropping it from the average
// altogether would throw away a legitimate neighbour -- and, at a front with
// only perpendicular sources, would leave the weight sum at zero and break
// ops/Inpaint.hpp section 2's convexity argument. Small enough that such a
// texel loses to any non-perpendicular one by orders of magnitude, positive
// so that it never disappears.
constexpr float kDirFloor = 1.0e-6f;

// Telea eq. (4)-(5): solve |grad T| = 1 at one texel from one horizontal and
// one vertical neighbour, taking the larger root of the upwind quadratic when
// both are finalised and the one-sided answer when only one is.
//
// `a` and `b` are linear indices already known to be in range. A neighbour
// that is not `kKnown` contributes nothing -- its own T is still provisional,
// and the whole point of the heap is that a provisional value never feeds a
// finalised one.
float solveEikonal(int64_t a, int64_t b, const uint8_t* flags, const float* t) noexcept {
  const bool haveA = flags[a] == kKnown;
  const bool haveB = flags[b] == kKnown;
  if (haveA && haveB) {
    const float t1 = t[a];
    const float t2 = t[b];
    const float diff = t1 - t2;
    // `2 - diff^2` goes negative when the two neighbours are further apart
    // than the grid spacing allows, which happens on a ragged front. The
    // quadratic has no real root there and the correct answer is the
    // one-sided one, which is what `s += r` with `r == 0` degrades to.
    const float r = std::sqrt(std::max(0.0f, 2.0f - diff * diff));
    float s = (t1 + t2 - r) * 0.5f;
    if (s >= t1 && s >= t2) return s;
    s += r;
    if (s >= t1 && s >= t2) return s;
    return kFarT;
  }
  if (haveA) return 1.0f + t[a];
  if (haveB) return 1.0f + t[b];
  return kFarT;
}

// One fast march over a `w` x `h` grid.
//
// `flags` arrives holding only `kKnown` and `kUnknown`; this function finds
// the front between them, seeds it, and marches until every reachable
// `kUnknown` texel has a finalised `t`. **Both passes use this**, with the
// two labels meaning opposite things: inward, `kKnown` is the region outside
// the hole; outward, it is the hole. That symmetry is the reason the outward
// distances cost nothing to add.
//
// `onBand(x, y, index)` is called exactly once per texel, at the moment it
// leaves `kUnknown` and its `t` is set -- which is where the inward pass does
// its filling, because that is the moment at which every texel that may be
// read has already been finalised.
//
// The outermost one-texel frame of the grid is never marched, so the four-way
// neighbour reads below need no bounds test. The caller guarantees this costs
// nothing: the grid is the hole dilated by `radius + 1 >= 2`, so no hole texel
// and no texel any fill reads is in the frame.
template <typename OnBand>
void fastMarch(uint8_t* flags, float* t, int32_t w, int32_t h, OnBand&& onBand) {
  using Node = std::pair<float, int64_t>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> heap;
  const int64_t stride = static_cast<int64_t>(w);

  // The seed is collected before it is applied: flipping a texel to `kBand`
  // while still scanning would hide it from its own neighbours' tests and
  // leave part of the front unseeded.
  std::vector<int64_t> seeds;
  for (int32_t y = 1; y + 1 < h; ++y) {
    for (int32_t x = 1; x + 1 < w; ++x) {
      const int64_t i = static_cast<int64_t>(y) * stride + x;
      if (flags[i] != kKnown) continue;
      if (flags[i - 1] == kUnknown || flags[i + 1] == kUnknown ||
          flags[i - stride] == kUnknown || flags[i + stride] == kUnknown) {
        seeds.push_back(i);
      }
    }
  }
  for (const int64_t i : seeds) {
    flags[i] = kBand;
    t[i] = 0.0f;
    heap.emplace(0.0f, i);
  }

  while (!heap.empty()) {
    const int64_t i = heap.top().second;
    heap.pop();
    flags[i] = kKnown;
    const int32_t x = static_cast<int32_t>(i % stride);
    const int32_t y = static_cast<int32_t>(i / stride);
    static constexpr int32_t kDx[4] = {-1, 1, 0, 0};
    static constexpr int32_t kDy[4] = {0, 0, -1, 1};
    for (int q = 0; q < 4; ++q) {
      const int32_t nx = x + kDx[q];
      const int32_t ny = y + kDy[q];
      if (nx < 1 || ny < 1 || nx + 1 >= w || ny + 1 >= h) continue;
      const int64_t n = static_cast<int64_t>(ny) * stride + nx;
      if (flags[n] != kUnknown) continue;
      const float dist = std::min(
          std::min(solveEikonal(n - 1, n - stride, flags, t),
                   solveEikonal(n + 1, n - stride, flags, t)),
          std::min(solveEikonal(n - 1, n + stride, flags, t),
                   solveEikonal(n + 1, n + stride, flags, t)));
      t[n] = dist;
      flags[n] = kBand;
      onBand(nx, ny, n);
      heap.emplace(dist, n);
    }
  }
}

}  // namespace

bool inpaintParamsValid(const InpaintParams& p) noexcept {
  // The inverted default, in one line: no selection is no hole. See
  // ops/Inpaint.hpp section 1 -- reading `nullptr` the way every other op
  // here correctly reads it would mean "the whole document is a hole".
  if (p.hole == nullptr) return false;
  if (selectionSelectsNothing(*p.hole)) return false;
  return p.radius >= 1 && p.radius <= kInpaintMaxRadius;
}

std::optional<PixelRect> inpaintHoleBounds(const InpaintParams& p) {
  if (p.hole == nullptr) return std::nullopt;
  const std::optional<SelectionBounds> b = selectionBounds(*p.hole);
  if (!b.has_value()) return std::nullopt;
  return PixelRect{b->x0, b->y0, b->x1, b->y1};
}

bool inpaintTiles(const TileStore& src, const PixelRect& outRect, const InpaintParams& p,
                  TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!inpaintParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  const std::optional<PixelRect> holeBounds = inpaintHoleBounds(p);
  if (!holeBounds.has_value()) return false;
  const PixelRect hole = *holeBounds;

  // ops/Inpaint.hpp section 4: this op cannot be evaluated in pieces, so a
  // request that does not contain the whole hole is refused rather than
  // answered with a fill that depends on where the caller cut.
  if (!roiContains(outRect, hole)) return false;

  // The scratch rectangle. `radius + 1` and not `radius`: the fill reads
  // sources up to `radius` outside the hole, and `fastMarch()` leaves the
  // grid's outermost frame unmarched, so one more texel of margin is what
  // makes that frame provably unreachable rather than merely unlikely.
  const PixelRect work = roiExpandUniform(hole, p.radius + 1);
  const int32_t w = work.width();
  const int32_t h = work.height();
  if (w < 3 || h < 3) return false;
  // An arithmetic guard, not a policy: `1 << 30` is exactly the largest
  // document core/CanvasLimits.hpp's 32768-texel preset ceiling can produce,
  // so a `work` rectangle past it did not come from a selection over a real
  // document. See ops/Inpaint.hpp section 5 for why the *memory* a large hole
  // costs is left as a cost rather than turned into a second refusal here.
  const int64_t texels = roiTexelCount(work);
  if (texels <= 0 || texels > (static_cast<int64_t>(1) << 30)) return false;
  const size_t n = static_cast<size_t>(texels);

  // Zero-initialised for the same reason ops/Blur.cpp's gather plane is: a
  // tile core/TileStore does not hold is transparent black, so an absent tile
  // needs no special case -- it is simply not copied in.
  std::vector<float> img(n * static_cast<size_t>(Tile::kChannels), 0.0f);
  std::vector<uint8_t> flagsIn(n, kKnown);
  std::vector<uint8_t> flagsOut(n, kUnknown);
  std::vector<uint8_t> isHole(n, 0);
  std::vector<float> tIn(n, kFarT);
  std::vector<float> tOut(n, kFarT);

  // Gather and classify in one pass over the tiles, hoisting both the image
  // tile and the SELECTION tile out of the per-texel loop.
  // core/SelectionMask.hpp names this as a real drift risk: a hoisted loop
  // owns the null branch itself. Here the null-`Selection` branch cannot
  // arise (`inpaintParamsValid()` refused it above, which is this file's
  // whole first section), and what is hoisted is the null-*tile* branch,
  // which `selectionTileCoverage()` answers with 0.0 -- outside the
  // selection, so outside the hole, so source data.
  {
    const TileRange tiles = roiTileRange(work);
    for (int32_t ty = tiles.y0; ty < tiles.y1; ++ty) {
      for (int32_t tx = tiles.x0; tx < tiles.x1; ++tx) {
        const TileCoord coord{tx, ty};
        const PixelRect span = roiIntersect(roiTileRect(coord), work);
        if (roiIsEmpty(span)) continue;
        const Tile* imageTile = src.find(coord);
        const SelectionTile* holeTile = p.hole->tiles.find(coord);
        if (imageTile == nullptr && holeTile == nullptr) continue;
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t y = span.y0; y < span.y1; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            const PixelCoord local{x - origin.x, y - origin.y};
            const size_t i = static_cast<size_t>(y - work.y0) * static_cast<size_t>(w) +
                             static_cast<size_t>(x - work.x0);
            if (imageTile != nullptr) {
              const std::array<float, 4> rgba = imageTile->readPixel(local);
              const size_t base = i * static_cast<size_t>(Tile::kChannels);
              img[base + 0] = rgba[0];
              img[base + 1] = rgba[1];
              img[base + 2] = rgba[2];
              img[base + 3] = rgba[3];
            }
            // Strictly greater than zero, not a threshold -- ops/Inpaint.hpp
            // section 1 on why a half-covered rim texel is a hole texel.
            if (selectionTileCoverage(holeTile, local) > 0.0f) isHole[i] = 1;
          }
        }
      }
    }
  }
  for (size_t i = 0; i < n; ++i) {
    // The two passes' opposite labellings, written next to each other so the
    // inversion is visible rather than inferred.
    flagsIn[i] = isHole[i] != 0 ? kUnknown : kKnown;
    flagsOut[i] = isHole[i] != 0 ? kKnown : kUnknown;
    if (isHole[i] == 0) tIn[i] = 0.0f;
    if (isHole[i] != 0) tOut[i] = 0.0f;
  }

  // Pass 1, outward: how far each source texel lies OUTSIDE the rim. Run
  // first because the inward pass's `lev` term reads it. See ops/Inpaint.hpp
  // section 3 on why a flat T outside would silently delete a third of the
  // weighting.
  fastMarch(flagsOut.data(), tOut.data(), w, h, [](int32_t, int32_t, int64_t) {});

  // The signed level-set field the `lev` term compares against: negative
  // outside the rim, positive inside, filled in for a hole texel at the
  // moment that texel is filled. A texel the outward march never reached is
  // the grid's own frame, which no fill can read (see `fastMarch()`); it is
  // zeroed rather than left at `kFarT` so that a future change to the margin
  // cannot turn an unreachable value into a wrong one.
  std::vector<float> tLevel(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    if (isHole[i] != 0) continue;
    tLevel[i] = tOut[i] >= kFarT ? 0.0f : -tOut[i];
  }

  const int64_t stride = static_cast<int64_t>(w);
  const int32_t radius = p.radius;
  const float radiusSq = static_cast<float>(radius) * static_cast<float>(radius);
  const uint8_t* flags = flagsIn.data();
  const float* tField = tIn.data();

  // Pass 2, inward: the fill. Telea's estimate, one texel at a time, in
  // strictly increasing distance from the rim.
  fastMarch(flagsIn.data(), tIn.data(), w, h,
            [&](int32_t x, int32_t y, int64_t i) {
              tLevel[static_cast<size_t>(i)] = tField[i];

              // grad T at p, one-sided wherever the far side is still
              // unfilled. A central difference across an unfilled neighbour
              // would be a difference against `kFarT`.
              float gx = 0.0f;
              float gy = 0.0f;
              const bool right = flags[i + 1] != kUnknown;
              const bool left = flags[i - 1] != kUnknown;
              if (right && left) {
                gx = (tField[i + 1] - tField[i - 1]) * 0.5f;
              } else if (right) {
                gx = tField[i + 1] - tField[i];
              } else if (left) {
                gx = tField[i] - tField[i - 1];
              }
              const bool down = flags[i + stride] != kUnknown;
              const bool up = flags[i - stride] != kUnknown;
              if (down && up) {
                gy = (tField[i + stride] - tField[i - stride]) * 0.5f;
              } else if (down) {
                gy = tField[i + stride] - tField[i];
              } else if (up) {
                gy = tField[i] - tField[i - stride];
              }
              const float gradLength = std::sqrt(gx * gx + gy * gy);
              // No normal to weight by -- every finalised neighbour is on one
              // level set. Isotropic rather than zero: see ops/Inpaint.hpp
              // section 3's degenerate cases.
              const bool haveNormal = gradLength > 0.0f;
              if (haveNormal) {
                gx /= gradLength;
                gy /= gradLength;
              }

              // Accumulated in double for ops/Blur.hpp's reason one step
              // further on: the weights here span the full range of
              // `1/d^2 * lev`, so the sum is much worse conditioned than a
              // normalised convolution's, and the destination is an f16 store
              // whose 2.44e-4 floor the accumulator must stay well under.
              double acc[4] = {0.0, 0.0, 0.0, 0.0};
              double weightSum = 0.0;
              const float tp = tField[i];
              for (int32_t qy = y - radius; qy <= y + radius; ++qy) {
                if (qy < 1 || qy + 1 >= h) continue;
                for (int32_t qx = x - radius; qx <= x + radius; ++qx) {
                  if (qx < 1 || qx + 1 >= w) continue;
                  const int64_t j = static_cast<int64_t>(qy) * stride + qx;
                  if (j == i) continue;
                  // `kUnknown` is the only thing skipped: a texel outside the
                  // hole and a hole texel already filled are equally good
                  // sources, which is what makes the fill propagate inward
                  // rather than stopping one texel in.
                  if (flags[j] == kUnknown) continue;
                  const float rx = static_cast<float>(x - qx);
                  const float ry = static_cast<float>(y - qy);
                  const float d2 = rx * rx + ry * ry;
                  // Telea's B_eps(p) is a disc, not the square the loop walks.
                  if (d2 > radiusSq) continue;
                  const float d = std::sqrt(d2);
                  float dir = haveNormal ? std::fabs(rx * gx + ry * gy) / d : 1.0f;
                  if (dir < kDirFloor) dir = kDirFloor;
                  const float distance = 1.0f / d2;
                  const float lev =
                      1.0f / (1.0f + std::fabs(tLevel[static_cast<size_t>(j)] - tp));
                  const double weight = static_cast<double>(dir) * static_cast<double>(distance) *
                                        static_cast<double>(lev);
                  weightSum += weight;
                  const size_t base = static_cast<size_t>(j) * static_cast<size_t>(Tile::kChannels);
                  for (int c = 0; c < Tile::kChannels; ++c)
                    acc[c] += weight * static_cast<double>(img[base + c]);
                }
              }
              // Every weight is strictly positive and the texel that put this
              // one on the band is one of them, so this branch is taken for
              // every filled texel. It is a branch and not an assert because
              // "the fill left this texel alone" is a defined outcome (the
              // texel keeps its original value) and a crash is not.
              if (weightSum <= 0.0) return;
              const size_t base = static_cast<size_t>(i) * static_cast<size_t>(Tile::kChannels);
              for (int c = 0; c < Tile::kChannels; ++c)
                img[base + c] = static_cast<float>(acc[c] / weightSum);
            });

  // Scatter: the hole and nothing else (ops/Inpaint.hpp section 5). Two
  // passes per tile for `compositeFilterResult()`'s own reason -- asking
  // `getOrCreate()` for a tile the hole does not actually reach would
  // allocate one this op never writes, and every allocated tile is a tile the
  // composite then walks.
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
            const size_t i = static_cast<size_t>(y - work.y0) * static_cast<size_t>(w) +
                             static_cast<size_t>(x - work.x0);
            if (isHole[i] != 0) {
              any = true;
              break;
            }
          }
        }
        if (!any) continue;
        Tile& tile = dst->getOrCreate(coord);
        for (int32_t y = span.y0; y < span.y1; ++y) {
          for (int32_t x = span.x0; x < span.x1; ++x) {
            const size_t i = static_cast<size_t>(y - work.y0) * static_cast<size_t>(w) +
                             static_cast<size_t>(x - work.x0);
            if (isHole[i] == 0) continue;
            const size_t base = i * static_cast<size_t>(Tile::kChannels);
            tile.writePixel(PixelCoord{x - origin.x, y - origin.y},
                            {img[base + 0], img[base + 1], img[base + 2], img[base + 3]});
          }
        }
      }
    }
  }
  return true;
}

}  // namespace np
