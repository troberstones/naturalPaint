#include "core/SelectionShapes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace np {
namespace {

// --- Ellipse ---------------------------------------------------------------
//
// The antiderivative of sqrt(1 - u^2), which is what makes the ellipse exact
// rather than sampled: `integral sqrt(1-u^2) du = (u*sqrt(1-u^2) + asin(u))/2`.
//
// `u` is clamped to [-1, 1] before use. That is not defensive tidying -- the
// caller derives it from a texel edge divided by a radius, and a texel that
// straddles the ellipse's extreme produces a value a few ULP outside the
// domain. Unclamped, `asin` returns NaN there and one poisoned texel spreads
// through the difference below into a whole row.
double halfDiskIntegral(double u) {
  const double c = std::clamp(u, -1.0, 1.0);
  return (c * std::sqrt(std::max(0.0, 1.0 - c * c)) + std::asin(c)) * 0.5;
}

// The area of the ellipse lying LEFT of the vertical line `x = X`, within the
// horizontal band [v0, v1].
//
// Every texel's coverage is a difference of two of these -- `A(x+1) - A(x)` --
// which is why the whole shape is expressed as one function of a single
// vertical cut rather than as a four-sided clip. A cut is a closed form; a
// clip would be a case analysis.
double ellipseAreaLeftOf(double X, double v0, double v1, double cx, double cy,
                         double rx, double ry) {
  const double lo = std::max(v0, cy - ry);
  const double hi = std::min(v1, cy + ry);
  if (hi <= lo) return 0.0;

  // `s` is the cut's offset from the centre, so the whole problem becomes
  // symmetric in it and the three regimes below are decided by comparing it to
  // the ellipse's half-width at each height.
  const double s = X - cx;
  if (s <= -rx) return 0.0;  // the cut is left of the ellipse entirely

  // The integral of the ellipse's half-width w(v) over [a, b].
  const auto intW = [&](double a, double b) {
    return rx * ry * (halfDiskIntegral((b - cy) / ry) - halfDiskIntegral((a - cy) / ry));
  };

  if (s >= rx) return 2.0 * intW(lo, hi);  // the cut is right of it entirely

  // Otherwise the cut crosses the ellipse, at exactly two heights. Above and
  // below those the ellipse is narrower than |s| and the band is saturated --
  // wholly left of the cut, or wholly right of it. Between them the covered
  // width is the linear `s + w(v)`.
  const double vc = ry * std::sqrt(std::max(0.0, 1.0 - (s * s) / (rx * rx)));
  const double bandLo = cy - vc;
  const double bandHi = cy + vc;

  double total = 0.0;
  // Below the crossing: saturated. Full width if the cut is right of centre,
  // nothing if it is left.
  {
    const double a = lo, b = std::min(hi, bandLo);
    if (b > a && s > 0.0) total += 2.0 * intW(a, b);
  }
  // Between the crossings: the linear regime.
  {
    const double a = std::max(lo, bandLo), b = std::min(hi, bandHi);
    if (b > a) total += s * (b - a) + intW(a, b);
  }
  // Above the crossing: saturated again.
  {
    const double a = std::max(lo, bandHi), b = hi;
    if (b > a && s > 0.0) total += 2.0 * intW(a, b);
  }
  return total;
}

// --- Polygon ---------------------------------------------------------------

struct GridKey {
  int32_t x, y;
  bool operator==(const GridKey& o) const noexcept { return x == o.x && y == o.y; }
};
struct GridKeyHash {
  size_t operator()(const GridKey& k) const noexcept {
    return (static_cast<size_t>(static_cast<uint32_t>(k.x)) << 32) ^
           static_cast<uint32_t>(k.y);
  }
};

// Mark every texel the segment passes through, by walking the grid lines it
// crosses (Amanatides-Woo).
//
// The obvious alternative -- test every texel in the segment's bounding box --
// is quadratic in the segment's length for a diagonal edge, and a lasso is
// mostly long diagonal edges. This walk is linear in the number of texels
// actually touched, which is the number that later get the expensive exact
// clip.
void markSegmentTexels(SelectionPoint a, SelectionPoint b,
                       std::unordered_set<GridKey, GridKeyHash>& out) {
  int32_t x = static_cast<int32_t>(std::floor(a.x));
  int32_t y = static_cast<int32_t>(std::floor(a.y));
  const int32_t xEnd = static_cast<int32_t>(std::floor(b.x));
  const int32_t yEnd = static_cast<int32_t>(std::floor(b.y));

  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const int32_t stepX = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
  const int32_t stepY = dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0);

  // Distance along the segment (in units of the segment's own length) to the
  // next grid line in each axis, and the distance between consecutive ones.
  // Infinity for an axis with no motion, which makes that axis never win the
  // comparison below rather than needing a branch inside the loop.
  const float inf = std::numeric_limits<float>::infinity();
  const float tDeltaX = stepX != 0 ? std::fabs(1.0f / dx) : inf;
  const float tDeltaY = stepY != 0 ? std::fabs(1.0f / dy) : inf;
  float tMaxX = stepX > 0   ? (static_cast<float>(x + 1) - a.x) / dx
                : stepX < 0 ? (static_cast<float>(x) - a.x) / dx
                            : inf;
  float tMaxY = stepY > 0   ? (static_cast<float>(y + 1) - a.y) / dy
                : stepY < 0 ? (static_cast<float>(y) - a.y) / dy
                            : inf;

  out.insert(GridKey{x, y});
  // The bound is a safety rail, not the termination condition: the walk ends
  // when it reaches the destination texel. A degenerate segment whose deltas
  // underflow could otherwise spin, and a rasteriser that hangs on bad input
  // is worse than one that stops early.
  const int64_t limit = static_cast<int64_t>(std::abs(xEnd - x)) +
                        static_cast<int64_t>(std::abs(yEnd - y)) + 2;
  for (int64_t i = 0; i < limit && (x != xEnd || y != yEnd); ++i) {
    if (tMaxX < tMaxY) {
      x += stepX;
      tMaxX += tDeltaX;
    } else {
      y += stepY;
      tMaxY += tDeltaY;
    }
    out.insert(GridKey{x, y});
  }
}

// Sutherland-Hodgman against one half-plane of the texel square.
// `keep(p)` is true for the side that survives; `cut` interpolates the
// crossing point.
template <typename KeepFn, typename CutFn>
void clipHalfPlane(std::vector<SelectionPoint>& poly, std::vector<SelectionPoint>& scratch,
                   KeepFn keep, CutFn cut) {
  scratch.clear();
  const size_t n = poly.size();
  for (size_t i = 0; i < n; ++i) {
    const SelectionPoint& cur = poly[i];
    const SelectionPoint& prev = poly[(i + n - 1) % n];
    const bool curIn = keep(cur);
    const bool prevIn = keep(prev);
    if (curIn) {
      if (!prevIn) scratch.push_back(cut(prev, cur));
      scratch.push_back(cur);
    } else if (prevIn) {
      scratch.push_back(cut(prev, cur));
    }
  }
  poly.swap(scratch);
}

// The exact covered area of one texel: the polygon clipped to the texel's unit
// square, by shoelace.
//
// **Taken as an absolute value and clamped to [0,1]**, which is where the
// nonzero-winding rule stated in the header is approximated rather than
// implemented. For a path that does not cross itself *inside this one texel*
// the two agree exactly. For one that does, the shoelace sum partially
// cancels and the clamp saturates instead. That case needs a lasso to loop
// within a 1x1 texel, and paying a full winding rasteriser for it would cost
// every other texel in the document.
float texelAreaClipped(const std::vector<SelectionPoint>& vertices, float x0, float y0,
                       std::vector<SelectionPoint>& poly, std::vector<SelectionPoint>& scratch) {
  poly.assign(vertices.begin(), vertices.end());
  const float x1 = x0 + 1.0f, y1 = y0 + 1.0f;

  clipHalfPlane(
      poly, scratch, [&](const SelectionPoint& p) { return p.x >= x0; },
      [&](const SelectionPoint& a, const SelectionPoint& b) {
        const float t = (x0 - a.x) / (b.x - a.x);
        return SelectionPoint{x0, a.y + t * (b.y - a.y)};
      });
  if (poly.size() < 3) return 0.0f;
  clipHalfPlane(
      poly, scratch, [&](const SelectionPoint& p) { return p.x <= x1; },
      [&](const SelectionPoint& a, const SelectionPoint& b) {
        const float t = (x1 - a.x) / (b.x - a.x);
        return SelectionPoint{x1, a.y + t * (b.y - a.y)};
      });
  if (poly.size() < 3) return 0.0f;
  clipHalfPlane(
      poly, scratch, [&](const SelectionPoint& p) { return p.y >= y0; },
      [&](const SelectionPoint& a, const SelectionPoint& b) {
        const float t = (y0 - a.y) / (b.y - a.y);
        return SelectionPoint{a.x + t * (b.x - a.x), y0};
      });
  if (poly.size() < 3) return 0.0f;
  clipHalfPlane(
      poly, scratch, [&](const SelectionPoint& p) { return p.y <= y1; },
      [&](const SelectionPoint& a, const SelectionPoint& b) {
        const float t = (y1 - a.y) / (b.y - a.y);
        return SelectionPoint{a.x + t * (b.x - a.x), y1};
      });
  if (poly.size() < 3) return 0.0f;

  double twiceArea = 0.0;
  for (size_t i = 0, n = poly.size(); i < n; ++i) {
    const SelectionPoint& p = poly[i];
    const SelectionPoint& q = poly[(i + 1) % n];
    twiceArea += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
  }
  return static_cast<float>(std::clamp(std::fabs(twiceArea) * 0.5, 0.0, 1.0));
}

}  // namespace

Selection selectEllipse(float cx, float cy, float rx, float ry) {
  Selection out;
  // Degenerate is "selects nothing", matching selectRectangle() -- and
  // deliberately not "selects everything", which is what a default-constructed
  // Selection would be read as by a caller treating it as absent.
  if (!(rx > 0.0f) || !(ry > 0.0f)) return out;

  const int32_t tx0 = static_cast<int32_t>(std::floor(cx - rx));
  const int32_t ty0 = static_cast<int32_t>(std::floor(cy - ry));
  const int32_t tx1 = static_cast<int32_t>(std::ceil(cx + rx));
  const int32_t ty1 = static_cast<int32_t>(std::ceil(cy + ry));

  for (int32_t y = ty0; y < ty1; ++y) {
    const double v0 = y, v1 = y + 1;
    // One running value per row: the area left of this texel's left edge. The
    // next texel's coverage is the next cut minus this one, so each row costs
    // one evaluation per texel rather than two.
    double leftArea = ellipseAreaLeftOf(tx0, v0, v1, cx, cy, rx, ry);
    for (int32_t x = tx0; x < tx1; ++x) {
      const double rightArea = ellipseAreaLeftOf(x + 1, v0, v1, cx, cy, rx, ry);
      const double cov = rightArea - leftArea;
      leftArea = rightArea;
      if (cov <= 0.0) continue;  // absent tile already says "not selected"
      const PixelCoord doc{x, y};
      out.tiles.getOrCreate(tileCoordAt(doc))
          .writeCoverage(tileLocalOffset(doc), static_cast<float>(cov));
    }
  }
  return out;
}

Selection selectPolygon(const std::vector<SelectionPoint>& vertices) {
  Selection out;
  if (vertices.size() < 3) return out;

  float minX = vertices[0].x, maxX = vertices[0].x;
  float minY = vertices[0].y, maxY = vertices[0].y;
  for (const SelectionPoint& p : vertices) {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  const int32_t tx0 = static_cast<int32_t>(std::floor(minX));
  const int32_t ty0 = static_cast<int32_t>(std::floor(minY));
  const int32_t tx1 = static_cast<int32_t>(std::ceil(maxX));
  const int32_t ty1 = static_cast<int32_t>(std::ceil(maxY));

  // Which texels the boundary crosses. These are the only ones that need the
  // exact clip; everything else is wholly in or wholly out and is settled by a
  // winding test that costs one scanline walk per row.
  std::unordered_set<GridKey, GridKeyHash> boundary;
  for (size_t i = 0, n = vertices.size(); i < n; ++i) {
    markSegmentTexels(vertices[i], vertices[(i + 1) % n], boundary);
  }

  std::vector<SelectionPoint> poly, scratch;
  std::vector<std::pair<float, int32_t>> crossings;

  for (int32_t y = ty0; y < ty1; ++y) {
    // Winding along this row, sampled at the texel centres. Interior texels
    // take their answer from here; boundary texels overwrite it below.
    const float yc = static_cast<float>(y) + 0.5f;
    crossings.clear();
    for (size_t i = 0, n = vertices.size(); i < n; ++i) {
      const SelectionPoint& p = vertices[i];
      const SelectionPoint& q = vertices[(i + 1) % n];
      // Half-open in y, so a vertex lying exactly on the scanline is counted
      // once rather than twice or never -- the standard rule, and the one that
      // stops a horizontal run of vertices from punching a hole.
      const bool pBelow = p.y <= yc;
      const bool qBelow = q.y <= yc;
      if (pBelow == qBelow) continue;
      const float t = (yc - p.y) / (q.y - p.y);
      crossings.emplace_back(p.x + t * (q.x - p.x), qBelow ? -1 : 1);
    }
    std::sort(crossings.begin(), crossings.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (int32_t x = tx0; x < tx1; ++x) {
      float cov;
      if (boundary.count(GridKey{x, y}) != 0) {
        cov = texelAreaClipped(vertices, static_cast<float>(x), static_cast<float>(y), poly,
                               scratch);
      } else {
        const float xc = static_cast<float>(x) + 0.5f;
        int32_t winding = 0;
        for (const auto& [cxr, dir] : crossings) {
          if (cxr > xc) break;
          winding += dir;
        }
        cov = winding != 0 ? 1.0f : 0.0f;
      }
      if (cov <= 0.0f) continue;
      const PixelCoord doc{x, y};
      out.tiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), cov);
    }
  }
  return out;
}

}  // namespace np
