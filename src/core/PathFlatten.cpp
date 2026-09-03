#include "core/PathFlatten.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// The smallest tolerance the segment-count formula is allowed to see. Not a
// quality floor -- `kMaxSegmentsPerCurve` is what actually bounds the work --
// but a guard against a caller passing 0 or a denormal and turning the
// division into an infinity that then survives the clamp as a huge count.
constexpr float kMinTolerancePx = 1.0e-4f;

float lengthOf(float x, float y) noexcept { return std::sqrt(x * x + y * y); }

}  // namespace

size_t cubicSegmentCount(const PathPoint p[4], float tolerancePx) noexcept {
  const float tol = std::max(tolerancePx, kMinTolerancePx);

  // The two second differences of the control polygon. See the header: this
  // is the quantity that bounds |B''| and, unlike a chord distance, is
  // invariant under translation -- so a path far from the origin does not pay
  // for being far from the origin.
  const float ax = p[0].x - 2.0f * p[1].x + p[2].x;
  const float ay = p[0].y - 2.0f * p[1].y + p[2].y;
  const float bx = p[1].x - 2.0f * p[2].x + p[3].x;
  const float by = p[1].y - 2.0f * p[2].y + p[3].y;
  const float L = std::max(lengthOf(ax, ay), lengthOf(bx, by));

  if (!(L > 0.0f)) return 1;  // exactly straight, or non-finite: one segment

  const float n = std::sqrt(3.0f * L / (4.0f * tol));
  if (!(n > 1.0f)) return 1;
  if (!(n < static_cast<float>(kMaxSegmentsPerCurve)))
    return kMaxSegmentsPerCurve;
  return static_cast<size_t>(std::ceil(n));
}

PathPoint cubicAt(const PathPoint p[4], float t) noexcept {
  const float u = 1.0f - t;
  const float w0 = u * u * u;
  const float w1 = 3.0f * u * u * t;
  const float w2 = 3.0f * u * t * t;
  const float w3 = t * t * t;
  return PathPoint{p[0].x * w0 + p[1].x * w1 + p[2].x * w2 + p[3].x * w3,
                   p[0].y * w0 + p[1].y * w1 + p[2].y * w2 + p[3].y * w3};
}

std::vector<FlatContour> flattenPath(const Path& path, float tolerancePx) {
  std::vector<FlatContour> out;
  // The untrusted-input boundary. See core/Path.hpp's `pathIsFinite()`: a NaN
  // here does not make a wrong pixel, it makes the rasteriser's scanline
  // bounds compare false in both directions.
  if (!pathIsFinite(path)) return out;

  for (const SubPath& sub : path.subpaths) {
    const size_t segs = subPathSegmentCount(sub);
    if (segs == 0) continue;

    FlatContour contour;
    contour.closed = sub.closed;
    contour.points.push_back(sub.anchors[0].pt);

    for (size_t i = 0; i < segs; ++i) {
      PathPoint seg[4];
      subPathSegment(sub, i, seg);
      const size_t n = cubicSegmentCount(seg, tolerancePx);
      // Start at 1: t=0 is the point the previous segment already emitted (or
      // the seed above), and emitting it twice would give the rasteriser a
      // zero-length edge and the stroker a zero-length normal to normalise.
      for (size_t k = 1; k <= n; ++k)
        contour.points.push_back(
            cubicAt(seg, static_cast<float>(k) / static_cast<float>(n)));
    }

    // A closed contour's final point is the first anchor again, emitted by the
    // closing segment's t=1. Drop it: `FlatContour` implies the closing edge,
    // and a duplicated endpoint is a zero-length edge in every consumer.
    if (contour.closed && contour.points.size() >= 2) contour.points.pop_back();

    if (contour.points.size() >= 2) out.push_back(std::move(contour));
  }
  return out;
}

PathBounds pathTightBounds(const Path& path) noexcept {
  PathBounds b;
  if (!pathIsFinite(path)) return b;

  auto include = [&b](float x, float y) {
    if (!b.valid) {
      b.valid = true;
      b.minX = b.maxX = x;
      b.minY = b.maxY = y;
      return;
    }
    b.minX = std::min(b.minX, x);
    b.maxX = std::max(b.maxX, x);
    b.minY = std::min(b.minY, y);
    b.maxY = std::max(b.maxY, y);
  };

  // One axis of one segment. B'(t) = 0 is a quadratic in t; the endpoints are
  // always included by the caller, so only roots strictly inside (0,1) can
  // extend the bound.
  auto axisExtrema = [&](float v0, float v1, float v2, float v3,
                         float roots[2]) -> int {
    const float a = -v0 + 3.0f * v1 - 3.0f * v2 + v3;
    const float bq = 2.0f * (v0 - 2.0f * v1 + v2);
    const float c = v1 - v0;
    int count = 0;
    // Degenerate to linear when the cubic term vanishes -- not a rare case,
    // it is every straight segment this model stores as a cubic.
    if (std::fabs(a) < 1.0e-12f) {
      if (std::fabs(bq) > 1.0e-12f) {
        const float t = -c / bq;
        if (t > 0.0f && t < 1.0f) roots[count++] = t;
      }
      return count;
    }
    const float disc = bq * bq - 4.0f * a * c;
    if (disc < 0.0f) return 0;
    const float s = std::sqrt(disc);
    for (const float t : {(-bq + s) / (2.0f * a), (-bq - s) / (2.0f * a)})
      if (t > 0.0f && t < 1.0f) roots[count++] = t;
    return count;
  };

  for (const SubPath& sub : path.subpaths) {
    if (sub.anchors.empty()) continue;
    if (sub.anchors.size() == 1) {
      include(sub.anchors[0].pt.x, sub.anchors[0].pt.y);
      continue;
    }
    for (const Anchor& a : sub.anchors) include(a.pt.x, a.pt.y);

    const size_t segs = subPathSegmentCount(sub);
    for (size_t i = 0; i < segs; ++i) {
      PathPoint seg[4];
      subPathSegment(sub, i, seg);
      float roots[2];
      int n = axisExtrema(seg[0].x, seg[1].x, seg[2].x, seg[3].x, roots);
      for (int r = 0; r < n; ++r) {
        const PathPoint q = cubicAt(seg, roots[r]);
        include(q.x, q.y);
      }
      n = axisExtrema(seg[0].y, seg[1].y, seg[2].y, seg[3].y, roots);
      for (int r = 0; r < n; ++r) {
        const PathPoint q = cubicAt(seg, roots[r]);
        include(q.x, q.y);
      }
    }
  }
  return b;
}

bool arcToCubics(PathPoint from, float rx, float ry, float xAxisRotationDeg,
                 bool largeArc, bool sweep, PathPoint to, PathPoint* fromOut,
                 std::vector<Anchor>* out) {
  if (!out || !fromOut) return false;
  if (!std::isfinite(rx) || !std::isfinite(ry)) return false;
  if (!std::isfinite(to.x) || !std::isfinite(to.y)) return false;

  rx = std::fabs(rx);
  ry = std::fabs(ry);
  // SVG 1.1 F.6.2: a zero radius, or coincident endpoints, means "draw a
  // line". Not an error -- the spec names both -- so the caller emits a line.
  if (rx < 1.0e-9f || ry < 1.0e-9f) return false;
  if (std::fabs(to.x - from.x) < 1.0e-12f && std::fabs(to.y - from.y) < 1.0e-12f)
    return false;

  const float phi = xAxisRotationDeg * kPi / 180.0f;
  const float cosPhi = std::cos(phi);
  const float sinPhi = std::sin(phi);

  // F.6.5 step 1: the endpoints in the ellipse's own (unrotated) frame.
  const float dx2 = (from.x - to.x) * 0.5f;
  const float dy2 = (from.y - to.y) * 0.5f;
  const float x1p = cosPhi * dx2 + sinPhi * dy2;
  const float y1p = -sinPhi * dx2 + cosPhi * dy2;

  // F.6.6: radii too small to span the chord are scaled up until they fit,
  // which the spec requires rather than treating as an error.
  float rxs = rx * rx, rys = ry * ry;
  const float lambda = (x1p * x1p) / rxs + (y1p * y1p) / rys;
  if (lambda > 1.0f) {
    const float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
    rxs = rx * rx;
    rys = ry * ry;
  }

  // F.6.5 step 2: the centre, in the ellipse frame.
  float num = rxs * rys - rxs * y1p * y1p - rys * x1p * x1p;
  const float den = rxs * y1p * y1p + rys * x1p * x1p;
  if (den < 1.0e-20f) return false;
  num = std::max(num, 0.0f);  // clamp the rounding that makes a tangent arc negative
  float coef = std::sqrt(num / den);
  if (largeArc == sweep) coef = -coef;
  const float cxp = coef * (rx * y1p / ry);
  const float cyp = coef * -(ry * x1p / rx);

  // F.6.5 steps 3-4: the centre back in user space, and the two angles.
  const float cx = cosPhi * cxp - sinPhi * cyp + (from.x + to.x) * 0.5f;
  const float cy = sinPhi * cxp + cosPhi * cyp + (from.y + to.y) * 0.5f;

  const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
  const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

  const float theta1 = std::atan2(uy, ux);
  float dTheta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  if (!sweep && dTheta > 0.0f) dTheta -= 2.0f * kPi;
  if (sweep && dTheta < 0.0f) dTheta += 2.0f * kPi;

  // Split at 90-degree boundaries. The `k = 4/3 tan(theta/4)` handle length is
  // exact at the endpoints and in the tangent direction for any sweep, but its
  // radial error grows with the sweep; a quarter turn holds it to ~2.7e-4 of
  // the radius, which --selftest asserts rather than assumes.
  const int pieces =
      std::max(1, static_cast<int>(std::ceil(std::fabs(dTheta) / (kPi * 0.5f) - 1.0e-6f)));
  const float delta = dTheta / static_cast<float>(pieces);
  const float k = 4.0f / 3.0f * std::tan(delta * 0.25f);

  // Point and tangent on the rotated ellipse at angle `t`.
  auto pointAt = [&](float t, PathPoint* p, PathPoint* tangent) {
    const float ct = std::cos(t), st = std::sin(t);
    const float ex = rx * ct, ey = ry * st;
    p->x = cosPhi * ex - sinPhi * ey + cx;
    p->y = sinPhi * ex + cosPhi * ey + cy;
    const float dex = -rx * st, dey = ry * ct;
    tangent->x = cosPhi * dex - sinPhi * dey;
    tangent->y = sinPhi * dex + cosPhi * dey;
  };

  float t0 = theta1;
  PathPoint p0, d0;
  pointAt(t0, &p0, &d0);
  // The arc's own start replaces the caller's endpoint only in tangent, never
  // in position: `from` is authoritative so the subpath stays connected even
  // when F.6.6 scaled the radii.
  fromOut->x = from.x + k * d0.x;
  fromOut->y = from.y + k * d0.y;

  for (int i = 0; i < pieces; ++i) {
    const float t1 = t0 + delta;
    PathPoint p1, d1;
    pointAt(t1, &p1, &d1);

    Anchor a;
    a.pt = (i == pieces - 1) ? to : p1;  // land exactly on the stated endpoint
    a.in.x = p1.x - k * d1.x;
    a.in.y = p1.y - k * d1.y;
    // Filled in by the next iteration, or left coincident on the last piece;
    // the caller overwrites it if another command follows.
    a.out = a.pt;
    out->push_back(a);

    if (i + 1 < pieces) {
      // The piece boundary is a genuinely smooth join, so the outgoing handle
      // mirrors the incoming one through the shared point.
      out->back().out.x = p1.x + k * d1.x;
      out->back().out.y = p1.y + k * d1.y;
    }
    t0 = t1;
  }
  return true;
}

}  // namespace np
