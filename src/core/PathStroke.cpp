#include "core/PathStroke.hpp"

#include <algorithm>
#include <cmath>

#include "core/PathFlatten.hpp"

namespace np {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct V2 {
  float x = 0.0f, y = 0.0f;
};

V2 sub(V2 a, V2 b) noexcept { return V2{a.x - b.x, a.y - b.y}; }
float len(V2 a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y); }
float cross(V2 a, V2 b) noexcept { return a.x * b.y - a.y * b.x; }
float dot(V2 a, V2 b) noexcept { return a.x * b.x + a.y * b.y; }

// Append a closed subpath through `pts`, with every handle coincident with
// its own anchor -- core/Path.hpp section 1's straight-line encoding, so
// these are genuine polygons rather than curves that resemble them.
//
// Winding is normalised to a consistent direction. This is the one invariant
// the whole union rests on (see the header): pieces wound against each other
// would CANCEL under nonzero instead of uniting, which shows up as holes
// wherever two pieces overlap.
void emitPolygon(Path& out, std::vector<V2> pts) {
  if (pts.size() < 3) return;
  double twiceArea = 0.0;
  for (size_t i = 0, n = pts.size(); i < n; ++i) {
    const V2& p = pts[i];
    const V2& q = pts[(i + 1) % n];
    twiceArea += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
  }
  if (twiceArea < 0.0) std::reverse(pts.begin(), pts.end());

  SubPath sub;
  sub.closed = true;
  sub.anchors.reserve(pts.size());
  for (const V2& p : pts) {
    Anchor a;
    a.pt = a.in = a.out = PathPoint{p.x, p.y};
    sub.anchors.push_back(a);
  }
  out.subpaths.push_back(std::move(sub));
}

// How many chords a circular arc of `sweep` radians and radius `r` needs to
// stay within `tolerancePx` of the true arc. Same sagitta relation the
// flattener uses, so a round cap is exactly as smooth as the curve it ends.
int arcChordCount(float r, float sweep, float tolerancePx) noexcept {
  const float tol = std::max(tolerancePx, 1.0e-4f);
  if (r <= tol) return 1;
  // sagitta = r (1 - cos(halfStep)) <= tol  =>  halfStep <= acos(1 - tol/r)
  const float halfStep = std::acos(std::max(-1.0f, 1.0f - tol / r));
  if (!(halfStep > 1.0e-6f)) return 1;
  const int n = static_cast<int>(std::ceil(std::fabs(sweep) / (2.0f * halfStep)));
  return std::clamp(n, 1, 512);
}

// A pie wedge centred at `c`, from angle `a0` sweeping `sweep` radians.
void emitArcWedge(Path& out, V2 c, float r, float a0, float sweep,
                  float tolerancePx) {
  if (r <= 0.0f || std::fabs(sweep) < 1.0e-7f) return;
  const int n = arcChordCount(r, sweep, tolerancePx);
  std::vector<V2> pts;
  pts.reserve(static_cast<size_t>(n) + 2);
  pts.push_back(c);
  for (int i = 0; i <= n; ++i) {
    const float t = a0 + sweep * (static_cast<float>(i) / static_cast<float>(n));
    pts.push_back(V2{c.x + r * std::cos(t), c.y + r * std::sin(t)});
  }
  emitPolygon(out, std::move(pts));
}

void emitDisc(Path& out, V2 c, float r, float tolerancePx) {
  if (r <= 0.0f) return;
  const int n = arcChordCount(r, 2.0f * kPi, tolerancePx);
  std::vector<V2> pts;
  pts.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float t = 2.0f * kPi * (static_cast<float>(i) / static_cast<float>(n));
    pts.push_back(V2{c.x + r * std::cos(t), c.y + r * std::sin(t)});
  }
  emitPolygon(out, std::move(pts));
}

// One segment's rectangle: the quad swept by a width-2h bar from a to b.
void emitSegmentQuad(Path& out, V2 a, V2 b, float h) {
  const V2 d = sub(b, a);
  const float L = len(d);
  if (L < 1.0e-9f) return;
  const V2 n{-d.y / L * h, d.x / L * h};
  emitPolygon(out, {V2{a.x + n.x, a.y + n.y}, V2{b.x + n.x, b.y + n.y},
                    V2{b.x - n.x, b.y - n.y}, V2{a.x - n.x, a.y - n.y}});
}

// The join at vertex `p` between incoming direction `d0` and outgoing `d1`
// (both unit). Only the OUTER side needs geometry: on the inner side the two
// segment rectangles already overlap, and nonzero unions that for free.
void emitJoin(Path& out, V2 p, V2 d0, V2 d1, float h, const StrokeStyle& style,
              float tolerancePx) {
  const float turn = cross(d0, d1);
  if (std::fabs(turn) < 1.0e-9f && dot(d0, d1) > 0.0f) return;  // collinear

  // Outward normal on the outer side of the turn.
  const float s = (turn > 0.0f) ? -1.0f : 1.0f;
  const V2 n0{-d0.y * h * s, d0.x * h * s};
  const V2 n1{-d1.y * h * s, d1.x * h * s};
  const V2 c0{p.x + n0.x, p.y + n0.y};
  const V2 c1{p.x + n1.x, p.y + n1.y};

  switch (style.join) {
    case LineJoin::Bevel:
      emitPolygon(out, {p, c0, c1});
      return;
    case LineJoin::Round: {
      const float a0 = std::atan2(n0.y, n0.x);
      const float a1 = std::atan2(n1.y, n1.x);
      float sweep = a1 - a0;
      while (sweep > kPi) sweep -= 2.0f * kPi;
      while (sweep < -kPi) sweep += 2.0f * kPi;
      emitArcWedge(out, p, h, a0, sweep, tolerancePx);
      return;
    }
    case LineJoin::Miter: {
      // The miter tip is where the two offset lines meet. `1/cos(theta/2)`
      // blows up as the turn approaches 180 degrees, which is exactly what
      // `stroke-miterlimit` exists to bound; past it SVG requires a bevel.
      const float cosHalf =
          std::sqrt(std::max(0.0f, (1.0f + dot(d0, d1)) * 0.5f));
      if (cosHalf < 1.0e-6f || (1.0f / cosHalf) > std::max(1.0f, style.miterLimit)) {
        emitPolygon(out, {p, c0, c1});  // SVG's fallback, not an approximation
        return;
      }
      // Bisector of the two outward normals, extended to the miter length.
      V2 bis{n0.x + n1.x, n0.y + n1.y};
      const float bl = len(bis);
      if (bl < 1.0e-9f) {
        emitPolygon(out, {p, c0, c1});
        return;
      }
      const float miterLen = h / cosHalf;
      const V2 tip{p.x + bis.x / bl * miterLen, p.y + bis.y / bl * miterLen};
      emitPolygon(out, {p, c0, tip, c1});
      return;
    }
  }
}

void emitCap(Path& out, V2 end, V2 dir, float h, LineCap cap,
             float tolerancePx) {
  switch (cap) {
    case LineCap::Butt:
      return;
    case LineCap::Round: {
      const float a = std::atan2(-dir.x, dir.y);  // normal direction
      emitArcWedge(out, end, h, a, kPi, tolerancePx);
      return;
    }
    case LineCap::Square: {
      const V2 n{-dir.y * h, dir.x * h};
      const V2 e{dir.x * h, dir.y * h};
      emitPolygon(out, {V2{end.x + n.x, end.y + n.y},
                        V2{end.x + n.x + e.x, end.y + n.y + e.y},
                        V2{end.x - n.x + e.x, end.y - n.y + e.y},
                        V2{end.x - n.x, end.y - n.y}});
      return;
    }
  }
}

bool dashesAreUsable(const std::vector<float>& dashes) noexcept {
  if (dashes.empty()) return false;
  float total = 0.0f;
  for (float d : dashes) {
    if (!(d >= 0.0f) || !std::isfinite(d)) return false;
    total += d;
  }
  // An all-zero array would make the dash walk advance by nothing forever.
  // SVG says to treat it as solid; doing anything else here is a hang.
  return total > 1.0e-6f;
}

}  // namespace

std::vector<FlatContour> dashContour(const FlatContour& contour,
                                     const std::vector<float>& dashes,
                                     float dashOffset) {
  std::vector<FlatContour> out;
  if (contour.points.size() < 2) return out;
  if (!dashesAreUsable(dashes)) {
    out.push_back(contour);
    return out;
  }

  // SVG repeats an odd-length array twice so that on/off alternation is
  // well defined ("5" means 5 on, 5 off).
  std::vector<float> pattern = dashes;
  if (pattern.size() % 2 == 1)
    pattern.insert(pattern.end(), dashes.begin(), dashes.end());

  float cycle = 0.0f;
  for (float d : pattern) cycle += d;

  // Normalise the offset into one cycle, then find the starting index and how
  // far into it we already are. A negative offset is legal in SVG.
  float phase = std::fmod(dashOffset, cycle);
  if (phase < 0.0f) phase += cycle;
  size_t idx = 0;
  while (phase >= pattern[idx]) {
    phase -= pattern[idx];
    idx = (idx + 1) % pattern.size();
  }
  bool on = (idx % 2) == 0;
  float remaining = pattern[idx] - phase;

  FlatContour run;
  run.closed = false;
  if (on) run.points.push_back(contour.points[0]);

  const size_t n = contour.points.size();
  // A closed contour dashes around its closing edge too, so the walk covers
  // n edges rather than n-1. Forgetting this leaves a gap at the seam of
  // every dashed circle.
  const size_t edges = contour.closed ? n : n - 1;

  for (size_t i = 0; i < edges; ++i) {
    const PathPoint a = contour.points[i];
    const PathPoint b = contour.points[(i + 1) % n];
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float segLen = std::sqrt(dx * dx + dy * dy);
    if (segLen < 1.0e-12f) continue;
    float travelled = 0.0f;

    while (segLen - travelled > remaining) {
      travelled += remaining;
      const float t = travelled / segLen;
      const PathPoint p{a.x + dx * t, a.y + dy * t};
      if (on) {
        run.points.push_back(p);
        if (run.points.size() >= 2) out.push_back(run);
        run.points.clear();
      } else {
        run.points.clear();
        run.points.push_back(p);
      }
      on = !on;
      idx = (idx + 1) % pattern.size();
      remaining = pattern[idx];
      // A zero-length entry in the pattern is legal and must not spin.
      if (remaining <= 0.0f) remaining = 1.0e-6f;
    }
    remaining -= (segLen - travelled);
    if (on) run.points.push_back(b);
  }
  if (on && run.points.size() >= 2) out.push_back(run);
  return out;
}

Path strokePath(const Path& path, const StrokeStyle& style, float tolerancePx) {
  Path out;
  // The union only unites because every piece is wound alike; an even-odd
  // result would punch a hole wherever the stroke crossed itself.
  out.rule = FillRule::NonZero;
  if (!(style.width > 0.0f) || !std::isfinite(style.width)) return out;

  const float h = style.width * 0.5f;
  const std::vector<FlatContour> contours = flattenPath(path, tolerancePx);
  const bool dashed = dashesAreUsable(style.dashes);

  for (const FlatContour& src : contours) {
    std::vector<FlatContour> runs;
    if (dashed)
      runs = dashContour(src, style.dashes, style.dashOffset);
    else
      runs.push_back(src);

    for (const FlatContour& c : runs) {
      const size_t n = c.points.size();
      if (n < 2) {
        // A dash of zero length under a round cap is a dot, which is what SVG
        // draws; under butt it is nothing.
        if (n == 1 && style.cap == LineCap::Round)
          emitDisc(out, V2{c.points[0].x, c.points[0].y}, h, tolerancePx);
        continue;
      }

      const size_t edges = c.closed ? n : n - 1;
      auto pointAt = [&](size_t i) {
        return V2{c.points[i % n].x, c.points[i % n].y};
      };
      auto dirAt = [&](size_t i) -> V2 {
        const V2 a = pointAt(i), b = pointAt(i + 1);
        const V2 d = sub(b, a);
        const float L = len(d);
        return (L < 1.0e-12f) ? V2{0.0f, 0.0f} : V2{d.x / L, d.y / L};
      };

      for (size_t i = 0; i < edges; ++i)
        emitSegmentQuad(out, pointAt(i), pointAt(i + 1), h);

      // Interior joins. A closed contour also joins across its seam, which is
      // the vertex an open one caps instead.
      const size_t joins = c.closed ? edges : (edges >= 1 ? edges - 1 : 0);
      for (size_t j = 0; j < joins; ++j) {
        const size_t v = c.closed ? ((j + 1) % n) : (j + 1);
        const V2 d0 = dirAt(c.closed ? j : j);
        const V2 d1 = dirAt(c.closed ? ((j + 1) % n) : (j + 1));
        if (len(d0) < 0.5f || len(d1) < 0.5f) continue;  // degenerate edge
        emitJoin(out, pointAt(v), d0, d1, h, style, tolerancePx);
      }

      if (!c.closed) {
        const V2 startDir = dirAt(0);
        const V2 endDir = dirAt(edges - 1);
        if (len(startDir) > 0.5f)
          emitCap(out, pointAt(0), V2{-startDir.x, -startDir.y}, h, style.cap,
                  tolerancePx);
        if (len(endDir) > 0.5f)
          emitCap(out, pointAt(edges), endDir, h, style.cap, tolerancePx);
      }
    }
  }
  return out;
}

}  // namespace np
