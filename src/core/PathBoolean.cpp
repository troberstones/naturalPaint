#include "core/PathBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/PathFlatten.hpp"

// The method, in four passes:
//
//   1. Split every edge of both operands at every crossing -- including an
//      operand's crossings with itself -- so no fragment's interior touches
//      any other edge.
//   2. Keep a fragment exactly when the result differs on its two sides,
//      probed a hair either side of its midpoint against each operand under
//      that operand's own fill rule, and orient it so the result is on its
//      left. Coincident edges from both operands collapse to one fragment
//      by welding, and the side probe decides them like any other.
//   3. Chain the kept fragments into closed contours.
//   4. Drop collinear vertices and sliver contours.
//
// Pass 2 is why this needs no orientation normalisation of the inputs and no
// per-operation rule table for shared edges: "is the result different on the
// left than on the right" is the definition of a boundary.
namespace np {
namespace {

struct P {
  double x = 0.0;
  double y = 0.0;
};

P sub(P a, P b) { return {a.x - b.x, a.y - b.y}; }
double cross(P a, P b) { return a.x * b.y - a.y * b.x; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }
double length(P a) { return std::sqrt(dot(a, a)); }

struct Seg {
  P a, b;
};

std::vector<Seg> segmentsOf(const Path& path, float tolerancePx) {
  std::vector<Seg> out;
  for (const FlatContour& c : flattenPath(path, tolerancePx)) {
    const size_t n = c.points.size();
    if (n < 2) continue;
    // Every contour is closed for filling, open or not.
    for (size_t i = 0; i < n; ++i) {
      const PathPoint& p = c.points[i];
      const PathPoint& q = c.points[(i + 1) % n];
      if (p.x == q.x && p.y == q.y) continue;
      out.push_back({{p.x, p.y}, {q.x, q.y}});
    }
  }
  return out;
}

// Edges bucketed into horizontal rows, so a crossing search and a ray cast
// look at one row's edges rather than all of them.
struct RowIndex {
  double minY = 0.0;
  double rowH = 1.0;
  int rows = 1;
  std::vector<std::vector<uint32_t>> cells;
  std::vector<int> firstRow;

  int rowOf(double y) const {
    const double r = std::floor((y - minY) / rowH);
    if (!(r >= 0.0)) return 0;
    return static_cast<int>(std::min(r, static_cast<double>(rows - 1)));
  }

  void build(const std::vector<Seg>& segs, double slack) {
    double lo = 0.0, hi = 0.0;
    for (size_t i = 0; i < segs.size(); ++i) {
      const double a = std::min(segs[i].a.y, segs[i].b.y);
      const double b = std::max(segs[i].a.y, segs[i].b.y);
      lo = i == 0 ? a : std::min(lo, a);
      hi = i == 0 ? b : std::max(hi, b);
    }
    rows = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(segs.size()))) + 1, 1, 4096);
    minY = lo;
    rowH = (hi - lo) / rows;
    if (!(rowH > 0.0)) {
      rows = 1;
      rowH = 1.0;
    }
    cells.assign(static_cast<size_t>(rows), {});
    firstRow.resize(segs.size());
    for (size_t i = 0; i < segs.size(); ++i) {
      const int r0 = rowOf(std::min(segs[i].a.y, segs[i].b.y) - slack);
      const int r1 = rowOf(std::max(segs[i].a.y, segs[i].b.y) + slack);
      firstRow[i] = r0;
      for (int r = r0; r <= r1; ++r) cells[static_cast<size_t>(r)].push_back(static_cast<uint32_t>(i));
    }
  }
};

struct Operand {
  std::vector<Seg> segs;
  RowIndex index;
  FillRule rule = FillRule::NonZero;

  bool inside(P p) const {
    if (segs.empty()) return false;
    int winding = 0;
    for (uint32_t i : index.cells[static_cast<size_t>(index.rowOf(p.y))]) {
      const Seg& s = segs[i];
      // Half-open in y, so a ray through a shared vertex counts it once.
      const bool up = s.a.y <= p.y && s.b.y > p.y;
      const bool down = s.b.y <= p.y && s.a.y > p.y;
      if (!up && !down) continue;
      const double t = (p.y - s.a.y) / (s.b.y - s.a.y);
      if (s.a.x + t * (s.b.x - s.a.x) > p.x) winding += up ? 1 : -1;
    }
    return rule == FillRule::NonZero ? winding != 0 : (winding & 1) != 0;
  }
};

bool combine(PathBooleanOp op, bool inA, bool inB) {
  switch (op) {
    case PathBooleanOp::Union: return inA || inB;
    case PathBooleanOp::Intersect: return inA && inB;
    case PathBooleanOp::Difference: return inA && !inB;
    case PathBooleanOp::Xor: return inA != inB;
  }
  return false;
}

// Welds points closer than `tol` into one vertex id, so fragments whose ends
// were computed from different edges still meet exactly.
struct VertexPool {
  double tol;
  double cell;
  std::vector<P> points;
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;

  explicit VertexPool(double t) : tol(t), cell(t * 4.0) {}

  static uint64_t key(int64_t cx, int64_t cy) {
    return (static_cast<uint64_t>(cx) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(cy);
  }

  uint32_t add(P p) {
    const int64_t cx = static_cast<int64_t>(std::floor(p.x / cell));
    const int64_t cy = static_cast<int64_t>(std::floor(p.y / cell));
    for (int64_t dy = -1; dy <= 1; ++dy)
      for (int64_t dx = -1; dx <= 1; ++dx) {
        auto it = grid.find(key(cx + dx, cy + dy));
        if (it == grid.end()) continue;
        for (uint32_t id : it->second)
          if (length(sub(points[id], p)) <= tol) return id;
      }
    const uint32_t id = static_cast<uint32_t>(points.size());
    points.push_back(p);
    grid[key(cx, cy)].push_back(id);
    return id;
  }
};

struct Split {
  double t;
  P p;
};

// Records where `s` and `o` meet, as split points on each. Endpoints within
// `tol` of the other segment split it at the endpoint's exact coordinates, so
// T-junctions and collinear overlaps become shared vertices.
void intersect(const Seg& s, const Seg& o, double tol, std::vector<Split>& onS,
               std::vector<Split>& onO) {
  const P d1 = sub(s.b, s.a);
  const P d2 = sub(o.b, o.a);
  const double l1 = length(d1);
  const double l2 = length(d2);
  if (l1 == 0.0 || l2 == 0.0) return;

  // An endpoint of one lying on the interior of the other.
  auto endpointOn = [tol](P q, const Seg& seg, P d, double len, std::vector<Split>& splits) {
    const double t = dot(sub(q, seg.a), d) / (len * len);
    if (t * len <= tol || (1.0 - t) * len <= tol) return;
    if (std::fabs(cross(d, sub(q, seg.a))) / len > tol) return;
    splits.push_back({t, q});
  };
  endpointOn(o.a, s, d1, l1, onS);
  endpointOn(o.b, s, d1, l1, onS);
  endpointOn(s.a, o, d2, l2, onO);
  endpointOn(s.b, o, d2, l2, onO);

  const double den = cross(d1, d2);
  // Parallel (or collinear, fully handled by the endpoint tests above).
  if (std::fabs(den) <= 1e-12 * l1 * l2) return;
  const P r = sub(o.a, s.a);
  const double t = cross(r, d2) / den;
  const double u = cross(r, d1) / den;
  // Proper interior crossing only; anything within `tol` of an end was an
  // endpoint case above.
  if (t * l1 <= tol || (1.0 - t) * l1 <= tol) return;
  if (u * l2 <= tol || (1.0 - u) * l2 <= tol) return;
  const P p{s.a.x + t * d1.x, s.a.y + t * d1.y};
  onS.push_back({t, p});
  onO.push_back({u, p});
}

}  // namespace

Path pathBoolean(PathBooleanOp op, const Path& a, const Path& b, float tolerancePx) {
  Path result;
  result.rule = FillRule::NonZero;
  if (!pathIsFinite(a) || !pathIsFinite(b)) return result;

  Operand A, B;
  A.segs = segmentsOf(a, tolerancePx);
  B.segs = segmentsOf(b, tolerancePx);
  A.rule = a.rule;
  B.rule = b.rule;

  std::vector<Seg> all = A.segs;
  all.insert(all.end(), B.segs.begin(), B.segs.end());
  if (all.empty()) return result;

  double minX = all[0].a.x, maxX = minX, minY = all[0].a.y, maxY = minY;
  for (const Seg& s : all)
    for (const P& p : {s.a, s.b}) {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minY = std::min(minY, p.y);
      maxY = std::max(maxY, p.y);
    }
  const double extent = std::max({maxX - minX, maxY - minY, 1.0});
  const double weldTol = extent * 1e-7;
  const double probeTol = extent * 1e-6;
  // Flattening a straight cubic leaves interior points that are collinear only
  // to float32 precision, which scales with the coordinates, not the extent.
  const double maxAbs = std::max({std::fabs(minX), std::fabs(maxX), std::fabs(minY), std::fabs(maxY), 1.0});
  const double collinearTol = std::max(weldTol, maxAbs * 1e-6);

  A.index.build(A.segs, 0.0);
  B.index.build(B.segs, 0.0);

  // 1. Split.
  RowIndex allIndex;
  allIndex.build(all, weldTol);
  std::vector<std::vector<Split>> splits(all.size());
  for (int r = 0; r < allIndex.rows; ++r) {
    const std::vector<uint32_t>& cell = allIndex.cells[static_cast<size_t>(r)];
    for (size_t i = 0; i < cell.size(); ++i)
      for (size_t j = i + 1; j < cell.size(); ++j) {
        const uint32_t si = cell[i], sj = cell[j];
        // Each pair is tested in the first row both occupy, and only there.
        if (std::max(allIndex.firstRow[si], allIndex.firstRow[sj]) != r) continue;
        intersect(all[si], all[sj], weldTol, splits[si], splits[sj]);
      }
  }

  VertexPool pool(weldTol);
  struct Frag {
    uint32_t from, to;
  };
  std::vector<Frag> kept;
  std::unordered_set<uint64_t> seen;
  for (size_t i = 0; i < all.size(); ++i) {
    std::vector<Split>& sp = splits[i];
    std::sort(sp.begin(), sp.end(), [](const Split& x, const Split& y) { return x.t < y.t; });
    std::vector<uint32_t> chain;
    chain.push_back(pool.add(all[i].a));
    for (const Split& s : sp) chain.push_back(pool.add(s.p));
    chain.push_back(pool.add(all[i].b));

    for (size_t k = 0; k + 1 < chain.size(); ++k) {
      const uint32_t u = chain[k], v = chain[k + 1];
      if (u == v) continue;
      // 2. Classify by the two sides.
      const P pu = pool.points[u], pv = pool.points[v];
      const P d = sub(pv, pu);
      const double len = length(d);
      const double eps = std::min(probeTol, len * 0.25);
      const P m{(pu.x + pv.x) * 0.5, (pu.y + pv.y) * 0.5};
      const P n{-d.y / len * eps, d.x / len * eps};
      const P left{m.x + n.x, m.y + n.y};
      const P right{m.x - n.x, m.y - n.y};
      const bool inLeft = combine(op, A.inside(left), B.inside(left));
      const bool inRight = combine(op, A.inside(right), B.inside(right));
      if (inLeft == inRight) continue;
      const Frag f = inLeft ? Frag{u, v} : Frag{v, u};
      // A coincident edge from the other operand welds to the same pair.
      if (!seen.insert((static_cast<uint64_t>(f.from) << 32) | f.to).second) continue;
      kept.push_back(f);
    }
  }

  // 3. Chain.
  std::unordered_map<uint32_t, std::vector<uint32_t>> outgoing;
  for (uint32_t e = 0; e < kept.size(); ++e) outgoing[kept[e].from].push_back(e);
  std::vector<bool> used(kept.size(), false);

  for (uint32_t start = 0; start < kept.size(); ++start) {
    if (used[start]) continue;
    used[start] = true;
    std::vector<uint32_t> loop = {kept[start].from};
    uint32_t cur = kept[start].to;
    P dir = sub(pool.points[kept[start].to], pool.points[kept[start].from]);
    bool closed = false;
    for (size_t steps = 0; steps <= kept.size(); ++steps) {
      if (cur == kept[start].from) {
        closed = true;
        break;
      }
      loop.push_back(cur);
      // At a vertex with several ways on, turn hardest towards the filled side
      // (the left), so a pinch point closes the contour it belongs to instead
      // of crossing into its neighbour as a figure eight.
      int64_t best = -1;
      double bestAngle = 0.0;
      for (uint32_t e : outgoing[cur]) {
        if (used[e]) continue;
        const P d = sub(pool.points[kept[e].to], pool.points[cur]);
        const double angle = std::atan2(cross(dir, d), dot(dir, d));
        if (best < 0 || angle > bestAngle) {
          best = e;
          bestAngle = angle;
        }
      }
      if (best < 0) break;  // a dead end: numerical debris, dropped below
      used[static_cast<size_t>(best)] = true;
      dir = sub(pool.points[kept[static_cast<size_t>(best)].to], pool.points[cur]);
      cur = kept[static_cast<size_t>(best)].to;
    }
    if (!closed) continue;

    // 4. Simplify.
    std::vector<P> pts;
    pts.reserve(loop.size());
    for (uint32_t id : loop) pts.push_back(pool.points[id]);
    bool removed = true;
    while (removed && pts.size() >= 3) {
      removed = false;
      for (size_t i = 0; i < pts.size() && pts.size() >= 3; ++i) {
        const P& prev = pts[(i + pts.size() - 1) % pts.size()];
        const P& next = pts[(i + 1) % pts.size()];
        const P chord = sub(next, prev);
        const double cl = length(chord);
        const double dist = cl > 0.0 ? std::fabs(cross(chord, sub(pts[i], prev))) / cl
                                     : length(sub(pts[i], prev));
        if (dist <= collinearTol) {
          pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
          removed = true;
        }
      }
    }
    if (pts.size() < 3) continue;
    double area2 = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) area2 += cross(pts[i], pts[(i + 1) % pts.size()]);
    if (std::fabs(area2) * 0.5 <= weldTol * extent) continue;

    SubPath sp;
    sp.closed = true;
    for (const P& p : pts) {
      Anchor an;
      an.pt = an.in = an.out = PathPoint{static_cast<float>(p.x), static_cast<float>(p.y)};
      sp.anchors.push_back(an);
    }
    result.subpaths.push_back(std::move(sp));
  }
  return result;
}

double pathBooleanArea(const Path& path, float tolerancePx) {
  double area2 = 0.0;
  for (const FlatContour& c : flattenPath(path, tolerancePx)) {
    const size_t n = c.points.size();
    for (size_t i = 0; i < n; ++i) {
      const PathPoint& p = c.points[i];
      const PathPoint& q = c.points[(i + 1) % n];
      area2 += static_cast<double>(p.x) * q.y - static_cast<double>(p.y) * q.x;
    }
  }
  return std::fabs(area2) * 0.5;
}

}  // namespace np
