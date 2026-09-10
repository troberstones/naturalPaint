#include "ops/Lens.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Parallel.hpp"

namespace np {
namespace {

// The radial scale `s(r)` of ops/Lens.hpp section 2, taking `r2` already
// normalised by the half-diagonal squared. One function, so the model exists
// once: `lensSourcePosition()` and `lensParamsValid()`'s monotonicity walk
// both call it, and a version that retyped the polynomial in the validator
// could accept coefficients the sampler then folds.
inline double radialScale(const LensParams& p, double r2) noexcept {
  return 1.0 + static_cast<double>(p.k1) * r2 + static_cast<double>(p.k2) * r2 * r2;
}

struct Frame {
  double cx = 0.0;
  double cy = 0.0;
  double halfDiag = 1.0;
};

Frame frameOf(const LensParams& p) noexcept {
  Frame f;
  const double w = static_cast<double>(p.frame.width());
  const double h = static_cast<double>(p.frame.height());
  f.cx = (static_cast<double>(p.frame.x0) + static_cast<double>(p.frame.x1)) * 0.5;
  f.cy = (static_cast<double>(p.frame.y0) + static_cast<double>(p.frame.y1)) * 0.5;
  // Half the diagonal, so `r == 1` at the frame's corners. Guarded against
  // zero only so a caller that got past `lensParamsValid()` some other way
  // cannot divide by it; an empty frame is already refused.
  f.halfDiag = 0.5 * std::sqrt(w * w + h * h);
  if (!(f.halfDiag > 0.0)) f.halfDiag = 1.0;
  return f;
}

inline double channelScale(const LensParams& p, int channel) noexcept {
  if (channel == 0) return 1.0 + static_cast<double>(p.caRed);
  if (channel == 2) return 1.0 + static_cast<double>(p.caBlue);
  return 1.0;  // green, and alpha with it -- ops/Lens.hpp section 2
}

// True when every coefficient is zero, i.e. the map is the identity and the
// gather has nothing to do. Compared against exact zero deliberately: a
// coefficient of 1e-30 is not the identity, it is a request whose answer
// happens to round to the input, and the two must not be conflated by a
// tolerance nobody chose.
inline bool isIdentity(const LensParams& p) noexcept {
  return p.k1 == 0.0f && p.k2 == 0.0f && p.caRed == 0.0f && p.caBlue == 0.0f;
}

// One gathered sample out of a flat premultiplied source image.
//
// This is `transformImage()`'s inner loop, and the comments there are the long
// form of every decision in it: the hard source boundary (outside is
// transparent black, so a correction that pulls content in from beyond the
// frame leaves an honest hole rather than smeared border colour), the clamped
// taps inside that boundary (so an opaque picture stays opaque at its edge --
// the measured defect was Lanczos3 leaving an 8->21 upscale's border at alpha
// 0.944), the normalisation over the whole footprint, and the `alpha <= 0`
// rule applied where the value is created.
//
// It is not shared code with that loop because sharing it would mean either
// exporting `transformImage()`'s body as a public per-texel entry point --
// making the per-texel setup (`maxTaps`, the scratch vectors) a per-call cost
// there, where it is the file's dominant measured cost -- or moving both into
// a third header for two callers. The duplication is four arithmetic lines
// around calls to the SAME kernel functions, which is the part that could
// actually drift and does not.
//
// Writes nothing and returns false when the position is outside the source.
bool gatherSample(const TransformImage& src, double px, double py, ResampleKernel kernel,
                  float* wx, float* wy, int maxTaps, float out[4]) noexcept {
  const auto sw = static_cast<int64_t>(src.width);
  const auto sh = static_cast<int64_t>(src.height);
  if (!std::isfinite(px) || !std::isfinite(py)) return false;
  if (px < 0.0 || py < 0.0 || px >= static_cast<double>(sw) || py >= static_cast<double>(sh))
    return false;

  const float radius = resampleKernelRadius(kernel);
  const auto i0 = static_cast<int64_t>(std::floor(px - static_cast<double>(radius) - 0.5));
  const auto i1 = static_cast<int64_t>(std::ceil(px + static_cast<double>(radius) - 0.5));
  const auto j0 = static_cast<int64_t>(std::floor(py - static_cast<double>(radius) - 0.5));
  const auto j1 = static_cast<int64_t>(std::ceil(py + static_cast<double>(radius) - 0.5));
  const int nx = static_cast<int>(i1 - i0 + 1);
  const int ny = static_cast<int>(j1 - j0 + 1);
  if (nx <= 0 || ny <= 0 || nx > maxTaps || ny > maxTaps) return false;

  float sumW = 0.0f;
  for (int k = 0; k < nx; ++k)
    wx[k] = resampleKernelWeight(kernel,
                                 static_cast<float>(px - (static_cast<double>(i0 + k) + 0.5)));
  for (int k = 0; k < ny; ++k)
    wy[k] = resampleKernelWeight(kernel,
                                 static_cast<float>(py - (static_cast<double>(j0 + k) + 0.5)));
  for (int ky = 0; ky < ny; ++ky)
    for (int kx = 0; kx < nx; ++kx) sumW += wy[ky] * wx[kx];
  if (!(std::fabs(sumW) > 1e-12f)) return false;

  double acc[4] = {0.0, 0.0, 0.0, 0.0};
  for (int ky = 0; ky < ny; ++ky) {
    const int64_t sy = std::clamp<int64_t>(j0 + ky, 0, sh - 1);
    const float wyv = wy[ky];
    if (wyv == 0.0f) continue;
    const float* srcRow = src.px.data() + static_cast<size_t>(sy) * src.width * 4u;
    for (int kx = 0; kx < nx; ++kx) {
      const int64_t sx = std::clamp<int64_t>(i0 + kx, 0, sw - 1);
      const double w = static_cast<double>(wyv) * static_cast<double>(wx[kx]);
      if (w == 0.0) continue;
      const float* q = srcRow + static_cast<size_t>(sx) * 4u;
      acc[0] += w * static_cast<double>(q[0]);
      acc[1] += w * static_cast<double>(q[1]);
      acc[2] += w * static_cast<double>(q[2]);
      acc[3] += w * static_cast<double>(q[3]);
    }
  }
  const double inv = 1.0 / static_cast<double>(sumW);
  out[0] = static_cast<float>(acc[0] * inv);
  out[1] = static_cast<float>(acc[1] * inv);
  out[2] = static_cast<float>(acc[2] * inv);
  out[3] = static_cast<float>(acc[3] * inv);
  return true;
}

}  // namespace

bool lensParamsValid(const LensParams& p) noexcept {
  if (!std::isfinite(p.k1) || !std::isfinite(p.k2)) return false;
  if (!std::isfinite(p.caRed) || !std::isfinite(p.caBlue)) return false;
  if (roiIsEmpty(p.frame)) return false;
  // A channel scaled by zero or less collapses or mirrors that channel through
  // the optical centre, which is not an aberration, it is a different picture.
  if (!(1.0f + p.caRed > 0.0f) || !(1.0f + p.caBlue > 0.0f)) return false;

  // The sampled monotonicity walk ops/Lens.hpp describes, including its stated
  // limit. `r * s(r)` strictly increasing on [0, 1] is what makes the map a
  // bijection of the picture onto itself.
  constexpr int kSteps = 256;
  double previous = 0.0;
  for (int i = 1; i <= kSteps; ++i) {
    const double r = static_cast<double>(i) / static_cast<double>(kSteps);
    const double mapped = r * radialScale(p, r * r);
    if (!std::isfinite(mapped) || !(mapped > previous)) return false;
    previous = mapped;
  }
  return true;
}

Point2 lensSourcePosition(const LensParams& p, Point2 dst, int channel) noexcept {
  const Frame f = frameOf(p);
  const double dx = static_cast<double>(dst.x) - f.cx;
  const double dy = static_cast<double>(dst.y) - f.cy;
  const double r2 = (dx * dx + dy * dy) / (f.halfDiag * f.halfDiag);
  const double scale = radialScale(p, r2) * channelScale(p, channel);
  return Point2{static_cast<float>(f.cx + dx * scale), static_cast<float>(f.cy + dy * scale)};
}

bool lensCorrectTiles(const TileStore& src, const PixelRect& outRect, const LensParams& p,
                      TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (roiIsEmpty(outRect)) return false;
  if (!lensParamsValid(p)) return false;

  // The identity short-circuit (LensParams::allowExactIdentity). A verbatim
  // texel copy, not a gather: `readPixel()`/`writePixel()` is a
  // half->float->half round trip, which is exact for every finite half, so the
  // result is bit-identical to the source and `compositeFilterResult()`
  // reports zero texels changed -- a zero-strength correction is honestly not
  // an edit rather than an edit that happens to look the same.
  if (p.allowExactIdentity && isIdentity(p)) {
    for (int32_t y = outRect.y0; y < outRect.y1; ++y) {
      for (int32_t x = outRect.x0; x < outRect.x1; ++x) {
        const PixelCoord doc{x, y};
        const TileCoord coord = tileCoordAt(doc);
        const Tile* srcTile = src.find(coord);
        if (srcTile == nullptr) continue;  // absent tile is already transparent
        const std::array<float, 4> v = srcTile->readPixel(tileLocalOffset(doc));
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) continue;
        dst->getOrCreate(coord).writePixel(tileLocalOffset(doc), v);
      }
    }
    return true;
  }

  // The source is materialised over `frame`, NOT over `outRect`: the gather is
  // global, so a destination texel on the right edge of a requested strip can
  // legitimately read from the left of the picture. This is the allocation
  // ops/Transform.hpp names as the cost of the flat bridge -- a full 4K canvas
  // is 141 MB in flight -- and it is paid once per call rather than per tile.
  const uint32_t fw = static_cast<uint32_t>(p.frame.width());
  const uint32_t fh = static_cast<uint32_t>(p.frame.height());
  const TransformImage source = imageFromTileStore(src, p.frame.x0, p.frame.y0, fw, fh);
  if (!source.valid()) return false;

  TransformImage result;
  result.width = static_cast<uint32_t>(outRect.width());
  result.height = static_cast<uint32_t>(outRect.height());
  result.px.assign(result.sampleCount(), 0.0f);

  const int maxTaps = static_cast<int>(std::ceil(2.0f * resampleKernelRadius(p.kernel))) + 2;
  const bool splitChannels = (p.caRed != 0.0f || p.caBlue != 0.0f);

  // Threaded over destination rows, the same safe axis and the same argument
  // as `transformImage()`'s loop: every row reads only `source`, which nothing
  // writes, and writes only its own slice of `result.px`, so no row's value
  // depends on when any other row runs and the per-texel accumulation order is
  // untouched -- bit-identical to the serial version by construction. The
  // scratch weight buffers are per row, not shared, for exactly the reason
  // that loop gives: two rows on two threads would otherwise race for the same
  // slots.
  //
  // **Two coordinate spaces, kept explicit.** `lensSourcePosition()` takes and
  // returns DOCUMENT coordinates, because the optical centre is a property of
  // the document rectangle `frame` names. The flat `source` buffer starts at
  // that frame's origin, so a document position becomes a buffer position by
  // subtracting it -- once, in `toBuffer` below. Doing that subtraction in
  // some places and not others is the bug that centres the lens on the
  // document origin, and on a frame that starts at 0,0 -- which every caller
  // in this build passes -- it looks exactly like correct code.
  const double frameX0 = static_cast<double>(p.frame.x0);
  const double frameY0 = static_cast<double>(p.frame.y0);
  parallelFor(result.height, kParallelForDefaultGrain, [&](size_t rowIdx) {
    std::vector<float> wx(static_cast<size_t>(maxTaps));
    std::vector<float> wy(static_cast<size_t>(maxTaps));
    float* row = result.px.data() + rowIdx * result.width * 4u;
    const auto toBuffer = [&](Point2 docPos, float rgba[4]) {
      return gatherSample(source, static_cast<double>(docPos.x) - frameX0,
                          static_cast<double>(docPos.y) - frameY0, p.kernel, wx.data(), wy.data(),
                          maxTaps, rgba);
    };
    for (uint32_t i = 0; i < result.width; ++i) {
      const Point2 dstPoint{static_cast<float>(outRect.x0) + static_cast<float>(i) + 0.5f,
                            static_cast<float>(outRect.y0) + static_cast<float>(rowIdx) + 0.5f};

      float* d = row + static_cast<size_t>(i) * 4u;
      float green[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (!toBuffer(lensSourcePosition(p, dstPoint, 1), green))
        continue;  // outside the frame: stays transparent black

      d[1] = green[1];
      d[3] = green[3];
      if (!splitChannels) {
        d[0] = green[0];
        d[2] = green[2];
      } else {
        float side[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        d[0] = toBuffer(lensSourcePosition(p, dstPoint, 0), side) ? side[0] : 0.0f;
        d[2] = toBuffer(lensSourcePosition(p, dstPoint, 2), side) ? side[2] : 0.0f;
      }

      // core/Premultiply's rule, applied where the value is created rather
      // than left for a later reader: a negative alpha out of a negative
      // kernel lobe is not a coverage.
      if (d[3] <= 0.0f) d[0] = d[1] = d[2] = d[3] = 0.0f;
    }
  });

  tileStoreFromImage(result, outRect.x0, outRect.y0, dst);
  return true;
}

}  // namespace np
