#include "ops/Blur.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/Parallel.hpp"
#include "core/Tile.hpp"

namespace np {

namespace {

// One 1-D convolution along a strided line, with **zero outside the line**.
//
// `stride` is in floats, so the same function serves both passes: a horizontal
// line has stride `channels` and a vertical one has stride `width * channels`,
// and neither pass needs its own copy of the loop. Writing it once is worth
// more than the vertical pass's worse cache behaviour costs -- the two passes
// have to agree exactly about clipping and normalisation or the separable
// result stops matching a direct 2-D convolution, and two copies of a clipped
// accumulation loop is precisely the kind of thing that drifts.
//
// The known cost of the shared form, named rather than hidden: the vertical
// pass strides by a whole row, so for a wide kernel every tap is a cache miss.
// The standard fix is to transpose between passes and run both horizontally.
// Not done here -- it needs a third full-size buffer or an in-place blocked
// transpose, and this file's first duty is to be checkable against a direct
// convolution. A profile is the thing that should change it.
//
// **The accumulator is `float`, deliberately, and ops/Blur.hpp's "the
// accumulator is f32" section carries the measurements.** Half loses the low
// bits of a wide kernel outright (2.09e-02 of error at sigma = 200, against
// 4.64e-07 for this float accumulator and 2.44e-04 for the store it is written
// to); double would buy precision the f16 store provably cannot hold.
void convolveLine(const float* in, int64_t inStride, int32_t n, const float* kernel,
                  int32_t apron, float* out, int64_t outStride) {
  for (int32_t i = 0; i < n; ++i) {
    const int32_t lo = std::max(0, i - apron);
    const int32_t hi = std::min(n - 1, i + apron);
    float acc = 0.0f;
    for (int32_t t = lo; t <= hi; ++t) {
      acc += kernel[t - i + apron] * in[static_cast<int64_t>(t) * inStride];
    }
    out[static_cast<int64_t>(i) * outStride] = acc;
  }
}

// The box kernel's O(1)-per-texel form: a sliding window sum, divided by the
// full width `2r+1` **whether or not the window is clipped**.
//
// That divisor is the whole subtlety. Dividing by the number of texels
// actually inside the line would be a "renormalise at the edge" box, which is
// a different operator -- it does not fade to zero at a boundary, so its
// output cannot match the Gaussian path's edge behaviour, cannot be gathered
// with an apron and cropped, and would reintroduce exactly the tile seam this
// file exists to prevent. Dividing by the constant width makes this a true
// convolution against a zero-extended signal, which is what `convolveLine`
// above does too.
//
// The running sum is kept in `double`, and ops/Blur.hpp's kernel section
// carries the measurement and the honest caveat: an f32 running sum drifts by
// 1.39e-06 over a 2048-texel row at r = 64, which is already 175x below the
// f16 store's floor and would have been acceptable *at that row length*. The
// drift scales with the row -- i.e. with the caller's gather rectangle -- and
// the store's floor does not, so a double accumulator removes a number that
// would otherwise need re-measuring every time a caller blurs a bigger region.
void boxLine(const float* in, int64_t inStride, int32_t n, int32_t radius, float* out,
             int64_t outStride) {
  const double invWidth = 1.0 / static_cast<double>(2 * radius + 1);
  double sum = 0.0;
  const int32_t primeHi = std::min(n - 1, radius);
  for (int32_t t = 0; t <= primeHi; ++t) sum += in[static_cast<int64_t>(t) * inStride];
  for (int32_t i = 0; i < n; ++i) {
    out[static_cast<int64_t>(i) * outStride] = static_cast<float>(sum * invWidth);
    const int32_t entering = i + 1 + radius;
    const int32_t leaving = i - radius;
    if (entering < n) sum += in[static_cast<int64_t>(entering) * inStride];
    if (leaving >= 0) sum -= in[static_cast<int64_t>(leaving) * inStride];
  }
}

}  // namespace

bool blurParamsValid(const BlurParams& p) noexcept {
  if (p.kind == BlurKind::Gaussian) return std::isfinite(p.sigma) && p.sigma >= 0.0f;
  return p.boxRadius >= 0;
}

int32_t blurApron(const BlurParams& p) noexcept {
  if (!blurParamsValid(p)) return 0;
  if (p.kind == BlurKind::Box) return p.boxRadius;
  if (p.sigma <= 0.0f) return 0;
  // ceil(4 * sigma). The truncation constant is measured, not chosen for
  // roundness -- ops/Blur.hpp's kernel section carries the numbers and the
  // reason 4 rather than the more usual 3 (at 3 sigma the truncation error is
  // larger than the f16 store's own quantisation step, so a wider kernel would
  // change stored values; at 4 sigma it is smaller, so it cannot).
  const double reach = std::ceil(4.0 * static_cast<double>(p.sigma));
  // Clamped into ops/Roi's coordinate range so an absurd sigma cannot produce
  // a margin that overflows a rectangle. The clamp is unreachable for any
  // sigma a document can justify; it exists so that "unreachable" does not
  // have to be argued from the caller's good behaviour.
  const double capped = std::min(reach, static_cast<double>(kRoiCoordLimit));
  return static_cast<int32_t>(capped);
}

RoiOp blurRoiOp(const BlurParams& p) noexcept { return roiDilateOp(blurApron(p)); }

std::vector<float> blurKernel(const BlurParams& p) {
  const int32_t apron = blurApron(p);
  const size_t taps = static_cast<size_t>(2 * apron + 1);
  std::vector<float> kernel(taps, 0.0f);
  if (apron == 0) {
    kernel[0] = 1.0f;
    return kernel;
  }

  std::vector<double> weights(taps, 0.0);
  if (p.kind == BlurKind::Box) {
    for (double& w : weights) w = 1.0;
  } else {
    // **Box-integrated, not point-sampled.** Tap i is the mass of the
    // continuous Gaussian falling inside texel i's own footprint
    // [i-0.5, i+0.5), which is what a texel means: an area, not a sample
    // point. Expressed through erf because that is what the normal CDF is:
    //   Phi(t) = 0.5 * (1 + erf(t / (sigma * sqrt(2))))
    // and the tap is Phi(i+0.5) - Phi(i-0.5). The 0.5 factors cancel in the
    // difference, so only the erf pair is evaluated.
    //
    // Rejected: exp(-i^2 / (2 sigma^2)) sampled at integer i and renormalised,
    // which is the shortcut in most implementations. It converges to the same
    // thing for wide kernels and is measurably wrong for narrow ones, where
    // most of the kernel's mass falls between the taps -- and narrow is
    // exactly where a feather radius of 1 or 2 texels lives.
    const double invScale = 1.0 / (static_cast<double>(p.sigma) * std::sqrt(2.0));
    for (int32_t i = -apron; i <= apron; ++i) {
      const double hi = std::erf((static_cast<double>(i) + 0.5) * invScale);
      const double lo = std::erf((static_cast<double>(i) - 0.5) * invScale);
      weights[static_cast<size_t>(i + apron)] = 0.5 * (hi - lo);
    }
  }

  // Normalised in double so the taps sum to 1 as exactly as the format allows.
  // This is what makes a blur preserve DC: a constant field must blur to
  // itself, or every flat region changes brightness with the radius and the
  // filter is unusable at any setting. The truncated tail is absorbed here,
  // which is why the truncation constant is chosen against the *shape* error
  // rather than the missing mass -- the missing mass is redistributed by this
  // division and never appears as a brightness shift.
  double sum = 0.0;
  for (const double w : weights) sum += w;
  const double inv = sum > 0.0 ? 1.0 / sum : 0.0;
  for (size_t i = 0; i < taps; ++i) kernel[i] = static_cast<float>(weights[i] * inv);
  return kernel;
}

void blurPlane(const float* src, int32_t width, int32_t height, int32_t channels,
               const BlurParams& p, float* dst) {
  if (src == nullptr || dst == nullptr || width <= 0 || height <= 0 || channels <= 0) return;

  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height) *
                        static_cast<size_t>(channels);
  const int32_t apron = blurApron(p);
  if (apron == 0 || !blurParamsValid(p)) {
    // Identity. A memmove rather than a convolution with a single unit tap, so
    // a zero-radius request cannot perturb a value by even one ulp -- the same
    // discipline ops/Resample takes for a 1:1 resize.
    if (dst != src) std::memmove(dst, src, texels * sizeof(float));
    return;
  }

  // The one intermediate ops/Blur.hpp names the cost of: the horizontal pass
  // cannot write into its own input, because a texel's neighbours must still
  // hold unblurred values when their turn comes. `dst` is allowed to alias
  // `src` precisely because this buffer exists -- after the horizontal pass
  // the source is dead, so the vertical pass may write over it.
  std::vector<float> mid(texels, 0.0f);

  const int64_t rowStride = static_cast<int64_t>(width) * channels;

  if (p.kind == BlurKind::Box) {
    const int32_t r = p.boxRadius;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t c = 0; c < channels; ++c) {
        const float* in = src + static_cast<int64_t>(y) * rowStride + c;
        float* out = mid.data() + static_cast<int64_t>(y) * rowStride + c;
        boxLine(in, channels, width, r, out, channels);
      }
    }
    for (int32_t x = 0; x < width; ++x) {
      for (int32_t c = 0; c < channels; ++c) {
        const float* in = mid.data() + static_cast<int64_t>(x) * channels + c;
        float* out = dst + static_cast<int64_t>(x) * channels + c;
        boxLine(in, rowStride, height, r, out, rowStride);
      }
    }
    return;
  }

  const std::vector<float> kernel = blurKernel(p);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t c = 0; c < channels; ++c) {
      const float* in = src + static_cast<int64_t>(y) * rowStride + c;
      float* out = mid.data() + static_cast<int64_t>(y) * rowStride + c;
      convolveLine(in, channels, width, kernel.data(), apron, out, channels);
    }
  }
  for (int32_t x = 0; x < width; ++x) {
    for (int32_t c = 0; c < channels; ++c) {
      const float* in = mid.data() + static_cast<int64_t>(x) * channels + c;
      float* out = dst + static_cast<int64_t>(x) * channels + c;
      convolveLine(in, rowStride, height, kernel.data(), apron, out, rowStride);
    }
  }
}

bool blurTiles(const TileStore& src, const PixelRect& outRect, const BlurParams& p,
               TileStore* dst) {
  // Refusals by name. `dst == &src` is the one worth spelling out: gathering
  // from a store while scattering into it would read texels that had already
  // been replaced by blurred ones, which is a feedback filter rather than a
  // blur and which would produce a *plausible* image -- smoother in one
  // corner, progressively more so along the scan -- rather than an obvious
  // failure.
  if (dst == nullptr || dst == &src) return false;
  if (!blurParamsValid(p)) return false;
  if (roiIsEmpty(outRect)) return false;

  // **The whole point of ops/Roi, in one line.** The rectangle of source this
  // output needs is the output dilated by the kernel's reach -- walked
  // backwards, from what is wanted to what must be read. Everything below is
  // bookkeeping around this call.
  const PixelRect need = roiBackward(blurRoiOp(p), outRect);
  const int32_t w = need.width();
  const int32_t h = need.height();
  if (w <= 0 || h <= 0) return false;

  // Zero-initialised, which is exactly right: a tile core/TileStore does not
  // hold is transparent black under premultiplied alpha, so an absent tile
  // needs no special case at all -- it is simply not copied in. See
  // ops/Blur.hpp on why the result therefore fades to transparent rather than
  // to black at the edge of the painted region.
  const size_t planeFloats = static_cast<size_t>(w) * static_cast<size_t>(h) *
                             static_cast<size_t>(Tile::kChannels);
  std::vector<float> plane(planeFloats, 0.0f);

  // docs/architecture-review.md P0-3: safe to parallelize directly, no
  // reservation phase needed. This loop only reads `src` (a `const
  // TileStore&`, via `find()`) and writes into `plane`, and every tile's
  // `span` is disjoint from every other tile's -- that's what "tile" means.
  // `TileStoreOf::find()` never mutates the map, so concurrent calls from
  // multiple workers are ordinary concurrent reads of an unmodified
  // container. See core/Parallel.hpp for the one loop in this file (the
  // scatter below) that is NOT this simple.
  const TileRange gatherTiles = roiTileRange(need);
  {
    const int32_t tilesWide = gatherTiles.tilesWide();
    float* const planeData = plane.data();
    parallelFor(static_cast<size_t>(gatherTiles.tileCount()), kParallelForDefaultGrain,
               [&](size_t i) {
                 const int32_t tx = gatherTiles.x0 + static_cast<int32_t>(i) % tilesWide;
                 const int32_t ty = gatherTiles.y0 + static_cast<int32_t>(i) / tilesWide;
                 const TileCoord coord{tx, ty};
                 const Tile* tile = src.find(coord);
                 if (tile == nullptr) return;  // absent: already zero in `plane`
                 const PixelRect span = roiIntersect(roiTileRect(coord), need);
                 const PixelCoord origin = tileOrigin(coord);
                 for (int32_t y = span.y0; y < span.y1; ++y) {
                   for (int32_t x = span.x0; x < span.x1; ++x) {
                     const std::array<float, 4> rgba =
                         tile->readPixel(PixelCoord{x - origin.x, y - origin.y});
                     const size_t base =
                         (static_cast<size_t>(y - need.y0) * static_cast<size_t>(w) +
                          static_cast<size_t>(x - need.x0)) *
                         static_cast<size_t>(Tile::kChannels);
                     planeData[base + 0] = rgba[0];
                     planeData[base + 1] = rgba[1];
                     planeData[base + 2] = rgba[2];
                     planeData[base + 3] = rgba[3];
                   }
                 }
               });
  }

  // Aliased in and out: `blurPlane` holds its own intermediate, so this keeps
  // the peak at two planes rather than three. The numbers in ops/Blur.hpp's
  // apron section are for two.
  blurPlane(plane.data(), w, h, Tile::kChannels, p, plane.data());

  // docs/architecture-review.md P0-3, and the hazard core/Parallel.hpp names:
  // `TileStoreOf::getOrCreate` mutates `dst`'s unordered_map and is not safe
  // to call from two iterations at once. So this is two phases -- serially
  // reserve every destination tile the ROI touches (every `getOrCreate()`
  // call happens here, on one thread), then parallelize the pixel writes
  // over the now-stable slots. The pointers this reservation loop hands out
  // stay valid through the parallel phase below because `TileStoreOf::Slot`
  // is a `std::shared_ptr<T>` -- `getOrCreate()`'s `T&` addresses the tile
  // object the shared_ptr owns, not a map node, so nothing an insertion does
  // to the map's bucket layout can move it (core/TileStore.hpp, and
  // core/Parallel.hpp's file comment).
  const TileRange writeTiles = roiTileRange(outRect);
  struct ReservedTile {
    PixelRect span;
    PixelCoord origin;
    Tile* tile;
  };
  std::vector<ReservedTile> reserved;
  reserved.reserve(static_cast<size_t>(writeTiles.tileCount()));
  for (int32_t ty = writeTiles.y0; ty < writeTiles.y1; ++ty) {
    for (int32_t tx = writeTiles.x0; tx < writeTiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& tile = dst->getOrCreate(coord);
      reserved.push_back(ReservedTile{span, tileOrigin(coord), &tile});
    }
  }

  // Phase 2: the pixel work, unchanged from what this loop always did --
  // only who runs which iteration is new. Every `ReservedTile` names a
  // distinct tile (the map cannot hold two slots at one `TileCoord`), so no
  // two parallel iterations can write the same memory.
  parallelFor(reserved.size(), kParallelForDefaultGrain, [&](size_t i) {
    const ReservedTile& r = reserved[i];
    for (int32_t y = r.span.y0; y < r.span.y1; ++y) {
      for (int32_t x = r.span.x0; x < r.span.x1; ++x) {
        const size_t base = (static_cast<size_t>(y - need.y0) * static_cast<size_t>(w) +
                             static_cast<size_t>(x - need.x0)) *
                            static_cast<size_t>(Tile::kChannels);
        r.tile->writePixel(
            PixelCoord{x - r.origin.x, y - r.origin.y},
            {plane[base + 0], plane[base + 1], plane[base + 2], plane[base + 3]});
      }
    }
  });
  return true;
}

}  // namespace np
