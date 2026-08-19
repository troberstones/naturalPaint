#include "ops/PointOps.hpp"

#include <algorithm>
#include <cmath>

#include "color/Shaper.hpp"

namespace np {
namespace {

// Guards Levels' `whiteIn - blackIn` divisor against a degenerate
// blackIn == whiteIn (or a caller-supplied inverted/near-equal pair),
// mirroring the epsilon guard pattern PointOps.hpp's own Levels doc
// comment specifies.
constexpr float kLevelsEpsilon = 1e-6f;

}  // namespace

float applyLevelsChannel(float input, const LevelsParams& p) noexcept {
  const float range = std::max(p.whiteIn - p.blackIn, kLevelsEpsilon);
  float t = (input - p.blackIn) / range;
  // Required by math, not policy: pow() below would see a negative base
  // for any input below blackIn (t < 0), which is NaN in general for a
  // fractional exponent (1/gamma) -- see PointOps.hpp's header comment
  // for why this is the one exception to "ops don't clamp their output"
  // (this clamps an internal normalized value, not the final output).
  t = std::clamp(t, 0.0f, 1.0f);
  t = std::pow(t, 1.0f / p.gamma);
  return t * (p.whiteOut - p.blackOut) + p.blackOut;
}

std::array<float, 3> applyLevels(const std::array<float, 3>& rgb,
                                  const std::array<LevelsParams, 3>& channels) noexcept {
  return {applyLevelsChannel(rgb[0], channels[0]), applyLevelsChannel(rgb[1], channels[1]),
          applyLevelsChannel(rgb[2], channels[2])};
}

namespace {

float secantSlope(const CurvePoint& a, const CurvePoint& b) noexcept {
  return (b.y - a.y) / (b.x - a.x);
}

// Catmull-Rom-style tangent at control point `i` of an >= 2 point curve,
// adapted for non-uniform x-spacing per PointOps.hpp's evalCurve() doc
// comment: an interior point averages its two adjacent secant slopes; an
// endpoint takes the one-sided secant slope of its single adjacent
// segment. At exactly 2 points, i == 0 and i == n-1 are the *same* two
// points, so both branches below independently evaluate to the one
// shared secant slope -- which is exactly what makes the Hermite formula
// reduce to a straight line in that case (see evalCurve()'s own comment).
float tangentAt(const Curve& points, size_t i) noexcept {
  const size_t n = points.size();
  if (i == 0) return secantSlope(points[0], points[1]);
  if (i == n - 1) return secantSlope(points[n - 2], points[n - 1]);
  const float mPrev = secantSlope(points[i - 1], points[i]);
  const float mNext = secantSlope(points[i], points[i + 1]);
  return 0.5f * (mPrev + mNext);
}

}  // namespace

float evalCurve(const Curve& points, float x) noexcept {
  const size_t n = points.size();
  // Degenerate: 0 or 1 control points is identity, in whatever domain x
  // happens to be -- see PointOps.hpp's doc comment for why applyCurves()
  // relies on exactly this (rather than the shaper round-trip) for an
  // unauthored channel.
  if (n < 2) return x;

  // Flat extrapolation outside [x_first, x_last] -- the standard
  // curves-tool convention, not a continuation of the end tangent.
  if (x <= points.front().x) return points.front().y;
  if (x >= points.back().x) return points.back().y;

  // Linear scan to the segment [i, i+1] containing x. Points are sorted
  // ascending by x (the caller's contract); curve control-point counts
  // are small (a handful, authored interactively through a curve UI), so
  // this need not be a binary search.
  size_t i = 0;
  while (i + 1 < n && points[i + 1].x < x) ++i;

  const CurvePoint& p0 = points[i];
  const CurvePoint& p1 = points[i + 1];
  const float dx = p1.x - p0.x;
  const float t = (x - p0.x) / dx;
  const float t2 = t * t;
  const float t3 = t2 * t;

  const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
  const float h10 = t3 - 2.0f * t2 + t;
  const float h01 = -2.0f * t3 + 3.0f * t2;
  const float h11 = t3 - t2;

  const float m0 = tangentAt(points, i);
  const float m1 = tangentAt(points, i + 1);

  return h00 * p0.y + h10 * dx * m0 + h01 * p1.y + h11 * dx * m1;
}

std::array<float, 3> applyCurves(const std::array<float, 3>& rgb,
                                  const std::array<Curve, 3>& channels) noexcept {
  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) {
    const Curve& curve = channels[static_cast<size_t>(c)];
    if (curve.size() < 2) {
      // Identity: skip the shaper round-trip entirely rather than lean
      // on shaperDecode(shaperEncode(x)) == x holding only to float
      // tolerance -- per evalCurve()'s own degenerate-case doc comment.
      out[static_cast<size_t>(c)] = rgb[static_cast<size_t>(c)];
      continue;
    }
    const float shapedIn = shaperEncode(rgb[static_cast<size_t>(c)]);
    const float shapedOut = evalCurve(curve, shapedIn);
    out[static_cast<size_t>(c)] = shaperDecode(shapedOut);
  }
  return out;
}

std::array<float, 3> applyExposure(const std::array<float, 3>& rgb,
                                    const ExposureParams& p) noexcept {
  const float mult = std::exp2(p.stops);
  return {rgb[0] * mult, rgb[1] * mult, rgb[2] * mult};
}

float computeLuma(const std::array<float, 3>& rgb, const std::array<float, 3>& weights) noexcept {
  return weights[0] * rgb[0] + weights[1] * rgb[1] + weights[2] * rgb[2];
}

std::array<float, 3> applySaturation(const std::array<float, 3>& rgb,
                                      const SaturationParams& p) noexcept {
  const float luma = computeLuma(rgb, p.lumaWeights);
  return {luma + (rgb[0] - luma) * p.scale, luma + (rgb[1] - luma) * p.scale,
          luma + (rgb[2] - luma) * p.scale};
}

std::array<float, 3> applyGrayscale(const std::array<float, 3>& rgb,
                                     const GrayscaleParams& p) noexcept {
  const float luma = computeLuma(rgb, p.lumaWeights);
  return {luma, luma, luma};
}

std::array<float, 3> applyChannelMixer(const std::array<float, 3>& rgb,
                                        const ChannelMixerParams& p) noexcept {
  std::array<float, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    const std::array<float, 4>& row = p.matrix[i];
    out[i] = row[0] * rgb[0] + row[1] * rgb[1] + row[2] * rgb[2] + row[3];
  }
  return out;
}

std::array<float, 4> applyPointOpsPremultiplied(const std::array<float, 4>& premultiplied,
                                                 const std::vector<PointOp>& ops) {
  // Mirrors core/Probe.cpp's unpremultiply() guard exactly: a <= 0 means
  // nothing to grade, and RGB is arbitrary under premultiplied alpha, so
  // the defined result is fully transparent black -- the same value an
  // untouched core::Tile texel already reads.
  const float a = premultiplied[3];
  if (a <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};

  std::array<float, 3> straight{premultiplied[0] / a, premultiplied[1] / a,
                                premultiplied[2] / a};
  for (const PointOp& op : ops) straight = op(straight);

  // Re-premultiply; alpha itself is never touched (see PointOps.hpp's doc
  // comment on this function for why that invariant holds for every op
  // in the committed P0 set).
  return {straight[0] * a, straight[1] * a, straight[2] * a, a};
}

}  // namespace np
