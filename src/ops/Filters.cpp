#include "ops/Filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "color/Shaper.hpp"
#include "core/Parallel.hpp"
#include "core/Tile.hpp"

namespace np {

namespace {

// --- the shared gather ------------------------------------------------------
//
// The blur half of every blur-based op in this file, and a deliberate near-copy
// of the gather loop in ops/Blur.cpp's `blurTiles()`.
//
// **Why not just call `blurTiles()` and subtract two stores?** Because
// `blurTiles()` writes its result through `Tile::writePixel`, i.e. it rounds
// the blurred value to half before anything else can see it -- and every op
// here immediately computes `src - blur`, a difference of two nearly equal
// numbers. Rounding both operands first is catastrophic cancellation: the
// blurred term's f16 error, 2.441e-04 at full scale, lands undivided in a
// difference whose own magnitude is often smaller than that, and the result
// stops being "the exact operator, rounded once by the store" -- which is the
// property ops/Blur.hpp establishes and this file has no business giving up.
// Keeping the blurred plane in `float` until the combine costs one gather loop
// of duplication and keeps the storage format the only source of error.
// --selftest measures both paths and prints the gap.
//
// The honest note on the duplication: when a third caller wants this, the
// gather belongs in ops/Blur beside `blurTiles()` as a shared entry point.
// Extracting it means editing ops/Blur, which is another track's file.
//
// Returns false only when the ROI is degenerate; `plane` is left holding
// `need.width() * need.height() * Tile::kChannels` floats, blurred in place.
bool gatherBlurredPlane(const TileStore& src, const PixelRect& outRect, const BlurParams& blur,
                        PixelRect* need, std::vector<float>* plane) {
  // The one line ops/Roi exists for: to write `outRect`, read it dilated by
  // the kernel's reach. The apron is what makes the result independent of how
  // the caller sliced its request.
  *need = roiBackward(blurRoiOp(blur), outRect);
  const int32_t w = need->width();
  const int32_t h = need->height();
  if (w <= 0 || h <= 0) return false;

  // Zero-initialised: an absent tile is transparent black under premultiplied
  // alpha, so "the tile does not exist" needs no special case.
  plane->assign(static_cast<size_t>(w) * static_cast<size_t>(h) *
                    static_cast<size_t>(Tile::kChannels),
                0.0f);

  // docs/architecture-review.md P0-3: safe to parallelize directly, exactly
  // as ops/Blur.cpp's own gather loop is (this is the near-copy the header
  // comment above already names). Read-only against `src` via `find()`
  // (`const`, so concurrent calls from multiple workers are ordinary
  // concurrent reads of an unmodified map), and every tile's `span` lands in
  // a disjoint slice of `*plane`. No reservation phase needed -- see
  // core/Parallel.hpp for why that phase exists at all and which loop in
  // this file actually needs it (scatterAligned, below).
  const TileRange gatherTiles = roiTileRange(*need);
  {
    const int32_t tilesWide = gatherTiles.tilesWide();
    const PixelRect needRect = *need;
    float* const planeData = plane->data();
    parallelFor(static_cast<size_t>(gatherTiles.tileCount()), kParallelForDefaultGrain,
               [&](size_t i) {
                 const int32_t tx = gatherTiles.x0 + static_cast<int32_t>(i) % tilesWide;
                 const int32_t ty = gatherTiles.y0 + static_cast<int32_t>(i) / tilesWide;
                 const TileCoord coord{tx, ty};
                 const Tile* tile = src.find(coord);
                 if (tile == nullptr) return;
                 const PixelRect span = roiIntersect(roiTileRect(coord), needRect);
                 const PixelCoord origin = tileOrigin(coord);
                 for (int32_t y = span.y0; y < span.y1; ++y) {
                   for (int32_t x = span.x0; x < span.x1; ++x) {
                     const std::array<float, 4> rgba =
                         tile->readPixel(PixelCoord{x - origin.x, y - origin.y});
                     const size_t base =
                         (static_cast<size_t>(y - needRect.y0) * static_cast<size_t>(w) +
                          static_cast<size_t>(x - needRect.x0)) *
                         static_cast<size_t>(Tile::kChannels);
                     planeData[base + 0] = rgba[0];
                     planeData[base + 1] = rgba[1];
                     planeData[base + 2] = rgba[2];
                     planeData[base + 3] = rgba[3];
                   }
                 }
               });
  }

  // Aliased in and out, exactly as `blurTiles()` does it: `blurPlane` holds
  // its own intermediate, so the peak stays at the two planes ops/Blur.hpp's
  // apron table is costed for.
  blurPlane(plane->data(), w, h, Tile::kChannels, blur, plane->data());
  return true;
}

std::array<float, 4> planeTexel(const std::vector<float>& plane, const PixelRect& need, int32_t x,
                                int32_t y) {
  const size_t base = (static_cast<size_t>(y - need.y0) * static_cast<size_t>(need.width()) +
                       static_cast<size_t>(x - need.x0)) *
                      static_cast<size_t>(Tile::kChannels);
  return {plane[base + 0], plane[base + 1], plane[base + 2], plane[base + 3]};
}

// --- the two scatter walks --------------------------------------------------
//
// Both mirror `blurTiles()`'s write loop: iterate the tiles `outRect` touches,
// clip to the tile, and write only texels inside the request so a caller can
// filter a region into an existing store without erasing its surroundings.
//
// `scatterAligned` additionally hoists the SOURCE tile, which it may do
// because every op that uses it combines the blurred value with the source at
// **the same document coordinate** -- so the source tile is the very tile the
// write loop already named. That is what lets this file hold two planes rather
// than three (ops/Filters.hpp's cost section).
template <class Combine>
void scatterAligned(const TileStore& src, const PixelRect& outRect, TileStore* dst,
                    Combine&& combine) {
  // docs/architecture-review.md P0-3, two phases per core/Parallel.hpp's
  // rule: `dst->getOrCreate()` mutates `dst`'s map and must run serially;
  // `src.find()` is a `const` read and is safe to call from any thread once
  // reservation has finished, so it is deferred into the parallel phase
  // below rather than resolved here -- doing it here would cost nothing for
  // correctness but would make this phase do work a worker thread could be
  // doing instead.
  const TileRange writeTiles = roiTileRange(outRect);
  struct ReservedTile {
    TileCoord coord;
    PixelRect span;
    Tile* outTile;
  };
  std::vector<ReservedTile> reserved;
  reserved.reserve(static_cast<size_t>(writeTiles.tileCount()));
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& outTile = dst->getOrCreate(coord);
      reserved.push_back(ReservedTile{coord, span, &outTile});
    }
  }

  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t i) {
    const ReservedTile& r = reserved[i];
    // Taken inside the parallel body, not the reservation loop above, so the
    // reservation phase (the one part of this function that has to be
    // serial) stays as short as possible. Safe here for the same reason it
    // was safe when this file only had one loop: `src` is a different store
    // (the callers refuse `dst == &src`), and a tile the two happen to share
    // stays alive through its own shared_ptr even after `dst` unshares its
    // slot.
    const Tile* srcTile = src.find(r.coord);
    const PixelCoord origin = tileOrigin(r.coord);
    for (int32_t y = r.span.y0; y < r.span.y1; ++y) {
      for (int32_t x = r.span.x0; x < r.span.x1; ++x) {
        const PixelCoord local{x - origin.x, y - origin.y};
        const std::array<float, 4> in =
            srcTile != nullptr ? srcTile->readPixel(local) : std::array<float, 4>{0, 0, 0, 0};
        r.outTile->writePixel(local, combine(x, y, in));
      }
    }
  });
}

// The same walk for an op whose source texel is somewhere else -- offset,
// which reads a translated and possibly wrapped coordinate and therefore
// cannot hoist anything per output tile.
//
// **Deliberately NOT parallelized, unlike every other tile loop in this
// file (docs/architecture-review.md P0-3).** `scatterAligned` above and
// `gatherBlurredPlane`'s gather loop are safe to hand to `parallelFor`
// because their per-texel work is stateless between calls. This one's only
// caller, `offsetTiles`, is not: it closes over a one-entry lookup cache
// (`cachedCoord`/`cachedTile`/`cacheValid`) that `produce` mutates on every
// call and reads on the next, on the documented assumption that consecutive
// texels usually share a source tile. That is exactly the kind of shared
// mutable state `parallelFor`'s contract (core/Parallel.hpp) requires a body
// NOT to have -- two workers racing through different destination tiles
// would tear that cache's three variables against each other, and the
// result would be a plausible-looking wrong pixel (a stale `cachedTile`
// read as if it were current), not a crash. Fixing it properly means giving
// each parallel task its own cache, which is a real change to
// `offsetTiles`'s structure and out of scope for "the loop body should be
// unchanged" -- so `offsetTiles` stays serial rather than risk shipping a
// race. See the architecture-review P0-3 task's own report for this being
// called out explicitly rather than silently skipped.
template <class Produce>
void scatterFree(const PixelRect& outRect, TileStore* dst, Produce&& produce) {
  const TileRange writeTiles = roiTileRange(outRect);
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& outTile = dst->getOrCreate(coord);
      const PixelCoord origin = tileOrigin(coord);
      for (int32_t y = span.y0; y < span.y1; ++y) {
        for (int32_t x = span.x0; x < span.x1; ++x) {
          outTile.writePixel(PixelCoord{x - origin.x, y - origin.y}, produce(x, y));
        }
      }
    }
  }
}

// --- shared arithmetic ------------------------------------------------------

// The clamp ops/Filters.hpp argues for on the two log-domain ops: the storage
// format's own non-negative range, and nothing narrower.
//
// Written as an explicit test rather than through `std::fmin`/`std::fmax`
// because those return the *non-NaN* operand, which would map a NaN to
// `kFilterMaxLinear` -- the largest value the format has, propagated through
// every later blur's apron. This form maps a NaN to 0 instead: still a lie,
// but a quiet one that cannot make a neighbourhood explode.
float clampStorable(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;
  return v > kFilterMaxLinear ? kFilterMaxLinear : v;
}

// Unsharp's per-texel gate. One scalar for all four channels -- see
// ops/Filters.hpp on why a per-channel gain would change hue at a threshold
// boundary. Returns 1.0 when no threshold was asked for, without touching the
// shaper at all: the two transcendentals per colour channel are paid only by
// callers who set a threshold.
float unsharpGain(const UnsharpParams& p, const std::array<float, 4>& s,
                  const std::array<float, 4>& b) noexcept {
  if (!(p.threshold > 0.0f)) return 1.0f;

  // Coverage joins the comparison undivided; colour joins it through the
  // shaper, so that "ignore differences below this" means the same visible
  // amount at every level rather than 234x more in the shadows.
  float worst = std::fabs(s[3] - b[3]);
  if (s[3] > 0.0f && b[3] > 0.0f) {
    const float invS = 1.0f / s[3];
    const float invB = 1.0f / b[3];
    for (int32_t c = 0; c < 3; ++c) {
      const float d = std::fabs(shaperEncode(s[static_cast<size_t>(c)] * invS) -
                                shaperEncode(b[static_cast<size_t>(c)] * invB));
      worst = std::max(worst, d);
    }
  }
  if (!(worst > p.threshold)) return 0.0f;
  // Soft, not hard. The hard form jumps by `amount * |d|` at exactly this
  // point -- 563 f16 storage steps at threshold 0.02 on a mid-tone -- drawing
  // a contour wherever the picture's local contrast equals the dialog's
  // number. ops/Filters.hpp carries the table.
  return (worst - p.threshold) / worst;
}

// splitmix64's finalizer, the same mixer `std::hash<TileCoord>` uses. Applied
// once per component in sequence rather than to a linear combination of them:
// a combination lets x and y trade off against one another, which shows up as
// diagonal structure in the grain.
uint64_t splitMix64(uint64_t z) noexcept {
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

}  // namespace

// ==========================================================================
// 1. Highpass
// ==========================================================================

RoiOp highpassRoiOp(const BlurParams& blur) noexcept { return blurRoiOp(blur); }

bool highpassTiles(const TileStore& src, const PixelRect& outRect, const BlurParams& blur,
                   TileStore* dst) {
  // Refused by name, matching `blurTiles()` one for one. `dst == &src` is the
  // one that matters: the write walk re-reads `src` at every output texel, so
  // filtering a store into itself would read texels the scatter had already
  // replaced -- a plausible-looking result rather than an obvious failure.
  if (dst == nullptr || dst == &src) return false;
  if (!blurParamsValid(blur)) return false;
  if (roiIsEmpty(outRect)) return false;

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherBlurredPlane(src, outRect, blur, &need, &plane)) return false;

  // All four channels, no clamp. The result is a signed difference field and
  // not an image; ops/Filters.hpp says so at length, including why no mid-grey
  // bias is added.
  scatterAligned(src, outRect, dst,
                 [&](int32_t x, int32_t y, const std::array<float, 4>& s) {
                   const std::array<float, 4> b = planeTexel(plane, need, x, y);
                   return std::array<float, 4>{s[0] - b[0], s[1] - b[1], s[2] - b[2],
                                               s[3] - b[3]};
                 });
  return true;
}

// ==========================================================================
// 2. Unsharp mask
// ==========================================================================

bool unsharpParamsValid(const UnsharpParams& p) noexcept {
  if (!blurParamsValid(p.blur)) return false;
  if (!std::isfinite(p.amount) || p.amount < 0.0f) return false;
  if (!std::isfinite(p.threshold) || p.threshold < 0.0f) return false;
  return true;
}

RoiOp unsharpRoiOp(const UnsharpParams& p) noexcept { return blurRoiOp(p.blur); }

bool unsharpMaskTiles(const TileStore& src, const PixelRect& outRect, const UnsharpParams& p,
                      TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!unsharpParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherBlurredPlane(src, outRect, p.blur, &need, &plane)) return false;

  scatterAligned(src, outRect, dst,
                 [&](int32_t x, int32_t y, const std::array<float, 4>& s) {
                   const std::array<float, 4> b = planeTexel(plane, need, x, y);
                   // ONE coefficient, FOUR channels. The whole
                   // premultiplied-alpha argument in ops/Filters.hpp reduces
                   // to this line: because k is shared, a soft edge of
                   // constant straight colour keeps that colour exactly while
                   // its alpha gets sharper.
                   const float k = p.amount * unsharpGain(p, s, b);
                   std::array<float, 4> out{s[0] + k * (s[0] - b[0]), s[1] + k * (s[1] - b[1]),
                                            s[2] + k * (s[2] - b[2]), s[3] + k * (s[3] - b[3])};
                   // Coverage is a fraction, so it is clamped -- and RGB is
                   // rescaled by the same factor, which is what makes the
                   // clamp a change of coverage rather than of colour. Clamping
                   // alpha alone would brighten the overshoot rim by exactly
                   // the factor it clipped. RGB itself is NOT clamped: the
                   // overshoot in light is what unsharp masking IS.
                   if (!(out[3] > 0.0f)) return std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
                   if (out[3] > 1.0f) {
                     const float inv = 1.0f / out[3];
                     out[0] *= inv;
                     out[1] *= inv;
                     out[2] *= inv;
                     out[3] = 1.0f;
                   }
                   return out;
                 });
  return true;
}

// ==========================================================================
// 3. Sharpen
// ==========================================================================

UnsharpParams sharpenParams(float strength) noexcept {
  UnsharpParams p;
  p.blur.kind = BlurKind::Gaussian;
  p.blur.sigma = kSharpenSigma;
  p.amount = strength;
  p.threshold = 0.0f;  // a one-click filter has no threshold to click
  return p;
}

bool sharpenTiles(const TileStore& src, const PixelRect& outRect, float strength,
                  TileStore* dst) {
  return unsharpMaskTiles(src, outRect, sharpenParams(strength), dst);
}

// ==========================================================================
// 4. Offset with wrap
// ==========================================================================

bool offsetParamsValid(const OffsetParams& p) noexcept {
  if (p.edge == OffsetEdge::Transparent) return true;
  // Wrapping is modular arithmetic and modular arithmetic needs a period. An
  // empty rectangle is refused rather than silently demoted to Transparent,
  // because a caller who forgot to fill in the canvas would otherwise get a
  // filter that works and is wrong at exactly one edge.
  return !roiIsEmpty(p.wrapRect);
}

RoiOp offsetRoiOp(const OffsetParams& p) noexcept { return roiOffsetOp(p.dx, p.dy); }

PixelRect offsetSourceRect(const OffsetParams& p, const PixelRect& outRect) noexcept {
  if (roiIsEmpty(outRect)) return roiEmptyRect();
  const PixelRect translated = roiBackward(offsetRoiOp(p), outRect);
  if (p.edge == OffsetEdge::Transparent || roiIsEmpty(p.wrapRect)) return translated;
  // Tight when the wrap does not bite -- every texel of the translated
  // rectangle is already inside the period, so no coordinate is reduced --
  // and the whole period when it does. Up to four disjoint rectangles is the
  // exact answer in that case and no `RoiOp` can express it; ops/Roi.hpp's
  // rule decides which way to round: too large is slow, too small is wrong.
  if (roiContains(p.wrapRect, translated)) return translated;
  return p.wrapRect;
}

PixelCoord offsetSourceTexel(const OffsetParams& p, PixelCoord out) noexcept {
  const int32_t sx = static_cast<int32_t>(static_cast<int64_t>(out.x) - p.dx);
  const int32_t sy = static_cast<int32_t>(static_cast<int64_t>(out.y) - p.dy);
  if (p.edge == OffsetEdge::Transparent || roiIsEmpty(p.wrapRect)) return PixelCoord{sx, sy};
  // core/Tile.hpp's `floorMod`, not `%`. Its own comment makes the argument
  // this op needs verbatim: C truncates toward zero, so `-1 % 128` is `-1`,
  // and content dragged past the origin is the ordinary case for the left and
  // top edges rather than a corner case.
  return PixelCoord{p.wrapRect.x0 + floorMod(sx - p.wrapRect.x0, p.wrapRect.width()),
                    p.wrapRect.y0 + floorMod(sy - p.wrapRect.y0, p.wrapRect.height())};
}

bool offsetTiles(const TileStore& src, const PixelRect& outRect, const OffsetParams& p,
                 TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!offsetParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  // A one-entry lookup cache. The source coordinate walks in x with the
  // output, so consecutive texels share a source tile except where a wrap
  // boundary falls; that makes one remembered pointer worth as much as a real
  // cache and costs two words. Without it this is a hash lookup per texel,
  // which is the only reason this op would ever have been slower than a
  // memcpy.
  TileCoord cachedCoord{};
  const Tile* cachedTile = nullptr;
  bool cacheValid = false;

  scatterFree(outRect, dst, [&](int32_t x, int32_t y) {
    const PixelCoord s = offsetSourceTexel(p, PixelCoord{x, y});
    const TileCoord coord = tileCoordAt(s);
    if (!cacheValid || !(coord == cachedCoord)) {
      cachedTile = src.find(coord);
      cachedCoord = coord;
      cacheValid = true;
    }
    if (cachedTile == nullptr) return std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
    return cachedTile->readPixel(tileLocalOffset(s));
  });
  return true;
}

// ==========================================================================
// 5. Add noise
// ==========================================================================

bool noiseParamsValid(const NoiseParams& p) noexcept {
  return std::isfinite(p.amount) && p.amount >= 0.0f;
}

float filterRandomUniform(uint64_t seed, int32_t x, int32_t y, int32_t stream) noexcept {
  // Stateless by construction. See ops/Filters.hpp: a stream generator would
  // make a texel's value depend on how many texels were drawn before it, so
  // the same region requested as one call and as two would get different
  // grain across the split -- the tile-seam bug in a form no apron can fix.
  uint64_t h = seed ^ 0x243f6a8885a308d3ULL;
  h = splitMix64(h + static_cast<uint64_t>(static_cast<uint32_t>(x)));
  h = splitMix64(h + static_cast<uint64_t>(static_cast<uint32_t>(y)));
  h = splitMix64(h + static_cast<uint64_t>(static_cast<uint32_t>(stream)));
  // Bin centres over 2^24 bins, computed in double so the `+ 0.5` is not
  // rounded away at the top of the float mantissa. The low end is what
  // matters: the smallest draw is 2^-25, never 0, so Box-Muller's `log(u)`
  // below is always finite. (The top bin can round to exactly 1.0f, which is
  // harmless -- it makes the uniform offset exactly `+amount` and the Gaussian
  // radius exactly 0.)
  return static_cast<float>((static_cast<double>(h >> 40) + 0.5) * (1.0 / 16777216.0));
}

float filterNoiseOffset(const NoiseParams& p, int32_t x, int32_t y, int32_t channel) noexcept {
  if (!noiseParamsValid(p) || !(p.amount > 0.0f)) return 0.0f;
  if (channel < 0 || channel > 3) return 0.0f;
  // Monochrome collapses the three colour channels onto one stream --
  // luminance grain rather than colour speckle -- rather than averaging three
  // draws, which would have 1/sqrt(3) of the amplitude the user asked for.
  const int32_t stream = p.monochrome ? 0 : channel;
  const float u1 = filterRandomUniform(p.seed, x, y, stream);
  if (p.distribution == NoiseDistribution::Uniform) {
    return p.amount * (2.0f * u1 - 1.0f);
  }
  const float u2 = filterRandomUniform(p.seed, x, y, stream + 4);
  const float radius = std::sqrt(-2.0f * std::log(u1));
  return p.amount * radius * std::cos(6.283185307179586f * u2);
}

bool addNoiseTiles(const TileStore& src, const PixelRect& outRect, const NoiseParams& p,
                   TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!noiseParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  scatterAligned(src, outRect, dst,
                 [&](int32_t x, int32_t y, const std::array<float, 4>& s) {
                   // Amount 0 is the identity, and exactly so: the copy skips
                   // the shaper round trip entirely. It would not have needed
                   // to -- the round trip is measured not to change a single
                   // one of the 31 744 finite positive halves -- but paying
                   // six transcendentals a texel to reproduce the input is not
                   // a cost a neutral setting should have.
                   if (!(p.amount > 0.0f)) return s;
                   const float a = s[3];
                   // Nothing there is nothing to grade. This is also what
                   // keeps the op from growing a layer's support: noise is
                   // added to the STRAIGHT colour and re-premultiplied, so the
                   // perturbation scales with coverage on its own.
                   if (!(a > 0.0f)) return s;
                   const float inv = 1.0f / a;
                   std::array<float, 4> out = s;
                   for (int32_t c = 0; c < 3; ++c) {
                     const size_t i = static_cast<size_t>(c);
                     const float shaped =
                         shaperEncode(s[i] * inv) + filterNoiseOffset(p, x, y, c);
                     out[i] = clampStorable(shaperDecode(shaped)) * a;
                   }
                   // Alpha is opacity, not light. color/Shaper.hpp's standing
                   // rule: no transfer function is ever applied to it, and
                   // nothing here perturbs it either -- grain must not eat
                   // holes in a layer.
                   out[3] = a;
                   return out;
                 });
  return true;
}

// ==========================================================================
// 6. Local contrast
// ==========================================================================

bool localContrastParamsValid(const LocalContrastParams& p) noexcept {
  // No sign test on `amount`: negative is legal here and means "flatten",
  // which is a thing a retoucher asks for. Unsharp's amount is different --
  // a negative unsharp is a blur spelled the hard way and is a caller bug.
  return blurParamsValid(p.blur) && std::isfinite(p.amount);
}

RoiOp localContrastRoiOp(const LocalContrastParams& p) noexcept { return blurRoiOp(p.blur); }

bool localContrastTiles(const TileStore& src, const PixelRect& outRect,
                        const LocalContrastParams& p, TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (!localContrastParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  PixelRect need{};
  std::vector<float> plane;
  if (!gatherBlurredPlane(src, outRect, p.blur, &need, &plane)) return false;

  scatterAligned(src, outRect, dst,
                 [&](int32_t x, int32_t y, const std::array<float, 4>& s) {
                   if (p.amount == 0.0f) return s;
                   const float a = s[3];
                   if (!(a > 0.0f)) return s;
                   const std::array<float, 4> b = planeTexel(plane, need, x, y);
                   if (!(b[3] > 0.0f)) return s;
                   const float invA = 1.0f / a;
                   const float invB = 1.0f / b[3];
                   std::array<float, 4> out = s;
                   for (int32_t c = 0; c < 3; ++c) {
                     const size_t i = static_cast<size_t>(c);
                     // The blur happened in linear light (an average of light
                     // is an average of light); the difference is taken and
                     // added back in the log domain, where adding IS
                     // multiplying -- so the same amount lifts a shadow's
                     // local contrast and a highlight's by the same RATIO.
                     // That is the whole difference between this and a
                     // large-radius unsharp, and it is why this one does not
                     // blow out highlights.
                     const float sc = shaperEncode(s[i] * invA);
                     const float bc = shaperEncode(b[i] * invB);
                     out[i] = clampStorable(shaperDecode(sc + p.amount * (sc - bc))) * a;
                   }
                   // Alpha untouched, and this is the line that separates a
                   // TONAL op from a sharpen: local contrast redistributes
                   // light inside the shape and must not move the shape's
                   // boundary by a texel. Because alpha does not change, the
                   // un-premultiply/re-premultiply is by the same number and
                   // the result needs no coverage clamp at all.
                   out[3] = a;
                   return out;
                 });
  return true;
}

}  // namespace np
