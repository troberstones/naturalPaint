#include "app/WarpMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "core/Parallel.hpp"

namespace np {
namespace {

// Uniform Catmull-Rom basis at `t` in [0,1], for the segment between P1 and
// P2 of a 4-point stencil (P0, P1, P2, P3) -- weights on (P0, P1, P2, P3) in
// that order. Standard derivation: P(t) = 0.5 * [2P1 + (-P0+P2)t +
// (2P0-5P1+4P2-P3)t^2 + (-P0+3P1-3P2+P3)t^3], expanded per control point.
// At t=0 this is (0,1,0,0) and at t=1 it is (0,0,1,0) -- the interpolation
// property that makes every grid point (not just the corners, unlike a
// Bezier's interior handles) land exactly on the surface.
std::array<float, 4> catmullRomWeights(float t) noexcept {
  const float t2 = t * t, t3 = t2 * t;
  return {-0.5f * t + t2 - 0.5f * t3, 1.0f - 2.5f * t2 + 1.5f * t3,
         0.5f * t + 2.0f * t2 - 1.5f * t3, -0.5f * t2 + 0.5f * t3};
}

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

float dist2(Point2 a, Point2 b) noexcept {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return dx * dx + dy * dy;
}

}  // namespace

WarpMesh WarpMesh::flat(const DocumentRegion& bounds, int n) {
  WarpMesh m;
  m.n_ = std::clamp(n, 3, 5);
  m.bounds_ = bounds;
  const int side = m.pointsPerSide();
  m.net_.assign(static_cast<size_t>(side) * side, Point2{});
  const float w = static_cast<float>(bounds.width);
  const float h = static_cast<float>(bounds.height);
  const float denom = static_cast<float>(side - 1);
  for (int row = 0; row < side; ++row) {
    for (int col = 0; col < side; ++col) {
      // Evenly spaced points along the rectangle's own edges reproduce it
      // exactly: a Catmull-Rom net through collinear, evenly spaced points
      // interpolates the straight line between them (same reasoning a
      // Bezier plane gave for free, just via a different basis).
      const float x = static_cast<float>(bounds.x) + (static_cast<float>(col) / denom) * w;
      const float y = static_cast<float>(bounds.y) + (static_cast<float>(row) / denom) * h;
      m.net_[static_cast<size_t>(row) * side + col] = Point2{x, y};
    }
  }
  return m;
}

Point2 WarpMesh::at(int row, int col) const noexcept {
  const int side = pointsPerSide();
  if (row < 0 || col < 0 || row >= side || col >= side) return Point2{};
  return net_[static_cast<size_t>(row) * side + col];
}

void WarpMesh::setAt(int row, int col, Point2 p) noexcept {
  const int side = pointsPerSide();
  if (row < 0 || col < 0 || row >= side || col >= side) return;
  net_[static_cast<size_t>(row) * side + col] = p;
}

Point2 WarpMesh::extendedAt(int row, int col) const noexcept {
  const int side = pointsPerSide();
  // Reflect col first (assuming row already valid), then reflect row using
  // already-col-reflected values -- composing the two one-axis reflections
  // is exactly how a corner phantom (both row and col out of range at once)
  // falls out correctly, with no separate corner case to get wrong.
  auto colReflected = [&](int r, int c) -> Point2 {
    if (c < 0) {
      const Point2 p0 = at(r, 0), p1 = at(r, 1);
      return Point2{2.0f * p0.x - p1.x, 2.0f * p0.y - p1.y};
    }
    if (c >= side) {
      const Point2 p0 = at(r, side - 1), p1 = at(r, side - 2);
      return Point2{2.0f * p0.x - p1.x, 2.0f * p0.y - p1.y};
    }
    return at(r, c);
  };
  if (row < 0) {
    const Point2 p0 = colReflected(0, col), p1 = colReflected(1, col);
    return Point2{2.0f * p0.x - p1.x, 2.0f * p0.y - p1.y};
  }
  if (row >= side) {
    const Point2 p0 = colReflected(side - 1, col), p1 = colReflected(side - 2, col);
    return Point2{2.0f * p0.x - p1.x, 2.0f * p0.y - p1.y};
  }
  return colReflected(row, col);
}

bool WarpMesh::isAffine(float toleranceDoc) const noexcept {
  const int side = pointsPerSide();
  const int last = side - 1;
  const Point2 origin = at(0, 0);
  const Point2 xAxis = Point2{at(0, last).x - origin.x, at(0, last).y - origin.y};
  const Point2 yAxis = Point2{at(last, 0).x - origin.x, at(last, 0).y - origin.y};
  const float tol2 = toleranceDoc * toleranceDoc;
  for (int row = 0; row < side; ++row) {
    for (int col = 0; col < side; ++col) {
      const float fu = static_cast<float>(col) / static_cast<float>(last);
      const float fv = static_cast<float>(row) / static_cast<float>(last);
      const Point2 predicted{origin.x + fu * xAxis.x + fv * yAxis.x,
                             origin.y + fu * xAxis.y + fv * yAxis.y};
      if (dist2(predicted, at(row, col)) > tol2) return false;
    }
  }
  return true;
}

bool WarpMesh::isIdentity() const noexcept {
  const WarpMesh flatOne = WarpMesh::flat(bounds_, n_);
  const int side = pointsPerSide();
  for (int row = 0; row < side; ++row) {
    for (int col = 0; col < side; ++col) {
      const Point2 a = at(row, col), b = flatOne.at(row, col);
      if (a.x != b.x || a.y != b.y) return false;
    }
  }
  return true;
}

void WarpMesh::dragControl(WarpControlRef ref, Point2 delta) noexcept {
  if (!ref.valid) return;
  // No neighbour ever needs to move: under Catmull-Rom, every curve's
  // tangent through a neighbouring point is DERIVED from that neighbour's
  // OWN neighbours at evaluation time, not stored as a separate handle here
  // -- so a neighbour's stored position stays exactly where the user left
  // it, and every curve touching it simply re-derives a new tangent next
  // `evaluate()` call. This one line replaces the old Bezier
  // `dragControl()`'s entire cardinal-handle ride-along block.
  setAt(ref.row, ref.col, Point2{at(ref.row, ref.col).x + delta.x, at(ref.row, ref.col).y + delta.y});
}

Point2 WarpMesh::evaluate(float u, float v) const noexcept {
  const int cellCount = std::max(cells(), 1);
  u = std::clamp(u, 0.0f, static_cast<float>(cellCount));
  v = std::clamp(v, 0.0f, static_cast<float>(cellCount));
  int cellI = static_cast<int>(std::floor(u));
  int cellJ = static_cast<int>(std::floor(v));
  cellI = std::clamp(cellI, 0, cellCount - 1);
  cellJ = std::clamp(cellJ, 0, cellCount - 1);
  const float t = clamp01(u - static_cast<float>(cellI));
  const float s = clamp01(v - static_cast<float>(cellJ));
  const std::array<float, 4> wa = catmullRomWeights(t);  // across columns (U)
  const std::array<float, 4> wb = catmullRomWeights(s);  // across rows (V)
  float x = 0.0f, y = 0.0f;
  for (int b = 0; b < 4; ++b) {
    for (int a = 0; a < 4; ++a) {
      const Point2 p = extendedAt(cellJ - 1 + b, cellI - 1 + a);
      const float w = wa[static_cast<size_t>(a)] * wb[static_cast<size_t>(b)];
      x += w * p.x;
      y += w * p.y;
    }
  }
  return Point2{x, y};
}

WarpMesh WarpMesh::refit(int newN) const {
  const int clampedN = std::clamp(newN, 3, 5);
  WarpMesh out;
  out.n_ = clampedN;
  out.bounds_ = bounds_;
  out.net_.assign(static_cast<size_t>(clampedN) * clampedN, Point2{});
  // Every new point is placed at THIS surface's own `evaluate()` -- unlike
  // the old Bezier `refit()`, there are no separate handles to re-derive
  // afterwards, so this one loop is the whole function. Exact for an affine
  // net (evaluate() reduces to the same bilinear map everywhere, the same
  // property `flat()`'s own comment relies on) and, for a bent one, places
  // every new grid point exactly on the old surface -- the same guarantee
  // the old approximating `refit()` gave for anchors only, now given for
  // every point since there is no other kind.
  const float denom = static_cast<float>(clampedN - 1);
  const float oldCells = static_cast<float>(cells());
  for (int row = 0; row < clampedN; ++row) {
    for (int col = 0; col < clampedN; ++col) {
      const float u = static_cast<float>(col) / denom * oldCells;
      const float v = static_cast<float>(row) / denom * oldCells;
      out.setAt(row, col, evaluate(u, v));
    }
  }
  return out;
}

WarpControlRef hitTestWarpControl(const WarpMesh& mesh, Point2 cursor, float radius) noexcept {
  const int side = mesh.pointsPerSide();
  const float r2 = radius * radius;
  WarpControlRef best;
  float bestDist2 = r2;
  for (int row = 0; row < side; ++row) {
    for (int col = 0; col < side; ++col) {
      const float d2 = dist2(mesh.at(row, col), cursor);
      if (d2 <= bestDist2) {
        bestDist2 = d2;
        best = WarpControlRef{true, row, col};
      }
    }
  }
  return best;
}

int warpChordSubdivisions(const WarpMesh& mesh, float toleranceDoc) noexcept {
  const float tol = std::max(toleranceDoc, 1e-4f);
  float dMax = 0.0f;
  const int cells = mesh.cells();
  // The EXACT max of a uniform Catmull-Rom segment's second derivative
  // magnitude over t in [0,1]: d2P/dt2(t) = A + 3t*B (vector-valued, affine
  // in t) where A = 2P0-5P1+4P2-P3 and B = -P0+3P1-3P2+P3 (both already
  // absorbing the formula's own leading 0.5, so this IS max|P''|, not a
  // scaled stand-in the way the old Bezier header's own `D` was). The norm
  // of a vector-valued affine function of a scalar is convex, so its max
  // over a closed interval is attained at an endpoint: max(|A|, |A+3B|) --
  // exact, not a triangle-inequality over-estimate (an earlier version of
  // this bound used |A|+3|B|, which measured 2-3x too many subdivisions --
  // caught by `--profile-vector-warp` running slower than the Bezier
  // rasteriser it replaced, not by inspection).
  auto secondDerivBound = [](Point2 p0, Point2 p1, Point2 p2, Point2 p3) noexcept {
    const float ax = 2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x;
    const float ay = 2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y;
    const float bx = -p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x;
    const float by = -p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y;
    return std::max(std::hypot(ax, ay), std::hypot(ax + 3.0f * bx, ay + 3.0f * by));
  };
  for (int J = 0; J < cells; ++J) {
    for (int I = 0; I < cells; ++I) {
      // U-direction curves (fixed row offset b, varying the 4 columns
      // I-1..I+2) and V-direction curves (fixed column offset a, varying
      // the 4 rows J-1..J+2) -- `extendedAt()` supplies the one-past-edge
      // phantom the same way `evaluate()` does, so a boundary cell's bound
      // is not silently computed from the wrong (clamped-into-range) point.
      for (int b = 0; b < 4; ++b) {
        const int row = J - 1 + b;
        const Point2 p0 = mesh.extendedAt(row, I - 1);
        const Point2 p1 = mesh.extendedAt(row, I + 0);
        const Point2 p2 = mesh.extendedAt(row, I + 1);
        const Point2 p3 = mesh.extendedAt(row, I + 2);
        dMax = std::max(dMax, secondDerivBound(p0, p1, p2, p3));
      }
      for (int a = 0; a < 4; ++a) {
        const int col = I - 1 + a;
        const Point2 p0 = mesh.extendedAt(J - 1, col);
        const Point2 p1 = mesh.extendedAt(J + 0, col);
        const Point2 p2 = mesh.extendedAt(J + 1, col);
        const Point2 p3 = mesh.extendedAt(J + 2, col);
        dMax = std::max(dMax, secondDerivBound(p0, p1, p2, p3));
      }
    }
  }
  if (dMax <= 0.0f) return 1;  // flat: one quad per cell already exact
  // The generic, basis-independent chord-remainder bound: a cubic segment
  // subdivided into `n` pieces has per-piece chord error `<= h^2/8 *
  // max|P''|` (`h = 1/n`, the standard Taylor-remainder bound for a
  // parabola's worth of curvature over a step of size `h`). `dMax` above IS
  // `max|P''|` already -- unlike the old Bezier header's own `D`, which
  // stood for `max|B''| / 6` (a Bernstein-specific convenience that let its
  // formula fold a `6` into the `3/4` constant below). Reusing that `3/4`
  // constant with THIS header's `dMax` would silently overshoot every
  // subdivision count by a factor of `sqrt(6) ~= 2.45` -- caught by
  // `--profile-vector-warp` measuring 5-6x more quads than the Bezier
  // rasteriser for a comparable bend, not by inspection.
  const float nSub = std::sqrt(dMax / (8.0f * tol));
  // Tried and measured OFF: a second floor forcing more subdivisions to
  // shrink each quad's pixel footprint, on the theory that the gather
  // rasteriser's bucket grid would then test fewer candidates per pixel.
  // `--profile-vector-warp` measured the opposite -- MORE subdivisions
  // (81 -> 576 -> 1089 quads) made `warpRgbTiles()` monotonically SLOWER
  // (2094 -> 2157 -> 2566 ms median), never faster, so the natural chord-
  // accurate count below is also the fastest one found; a residual gap
  // against the old Bezier rasteriser is not a subdivision-count problem
  // (see this file's own header on the remaining, not-yet-closed gap).
  return std::clamp(static_cast<int>(std::ceil(nSub)), 1, kMaxSubdivisionsPerCell);
}

std::vector<WarpQuad> tessellateWarpMesh(const WarpMesh& mesh, int subdivisionsPerCell) {
  const int sub = std::max(subdivisionsPerCell, 1);
  const int cells = mesh.cells();
  std::vector<WarpQuad> quads;
  quads.reserve(static_cast<size_t>(cells) * cells * sub * sub);
  for (int J = 0; J < cells; ++J) {
    for (int I = 0; I < cells; ++I) {
      for (int b = 0; b < sub; ++b) {
        const float v0 = static_cast<float>(J) + static_cast<float>(b) / sub;
        const float v1 = static_cast<float>(J) + static_cast<float>(b + 1) / sub;
        for (int a = 0; a < sub; ++a) {
          const float u0 = static_cast<float>(I) + static_cast<float>(a) / sub;
          const float u1 = static_cast<float>(I) + static_cast<float>(a + 1) / sub;
          WarpQuad q;
          q.u0 = u0;
          q.v0 = v0;
          q.u1 = u1;
          q.v1 = v1;
          q.dst00 = mesh.evaluate(u0, v0);
          q.dst10 = mesh.evaluate(u1, v0);
          q.dst11 = mesh.evaluate(u1, v1);
          q.dst01 = mesh.evaluate(u0, v1);
          quads.push_back(q);
        }
      }
    }
  }
  return quads;
}

DocumentRegion warpedRegion(const WarpMesh& mesh, int subdivisionsPerCell) noexcept {
  const std::vector<WarpQuad> quads = tessellateWarpMesh(mesh, subdivisionsPerCell);
  if (quads.empty()) return DocumentRegion{};
  float minX = quads[0].dst00.x, maxX = minX, minY = quads[0].dst00.y, maxY = minY;
  auto absorb = [&](Point2 p) {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  };
  for (const WarpQuad& q : quads) {
    absorb(q.dst00);
    absorb(q.dst10);
    absorb(q.dst11);
    absorb(q.dst01);
  }
  const int32_t x0 = static_cast<int32_t>(std::floor(minX)) - 1;
  const int32_t y0 = static_cast<int32_t>(std::floor(minY)) - 1;
  const int32_t x1 = static_cast<int32_t>(std::ceil(maxX)) + 1;
  const int32_t y1 = static_cast<int32_t>(std::ceil(maxY)) + 1;
  if (x1 <= x0 || y1 <= y0) return DocumentRegion{};
  DocumentRegion r;
  r.x = x0;
  r.y = y0;
  r.width = static_cast<uint32_t>(x1 - x0);
  r.height = static_cast<uint32_t>(y1 - y0);
  return r;
}

namespace {

// Closed-form inverse of the quad's own bilinear parametrisation: the (s, t)
// with `bilerp(q, s, t) == p`. Writing the bilinear map as
// `Q = A + sB + tC + stD` (A = dst00, B = dst10-A, C = dst01-A,
// D = dst11 - dst10 - dst01 + dst00) and crossing `E = p - A` with the
// `s`-dependent direction `C + sD` cancels `t` outright, leaving ONE
// quadratic in `s`:
//
//   cross(B,D) s^2 + (cross(B,C) - cross(E,D)) s - cross(E,C) = 0
//
// and then `t` falls out of `E - sB = t (C + sD)` by division along whichever
// axis of `C + sD` is larger. That replaces the fixed eight Newton
// iterations this used to run -- each with its own 2x2 inverse and divide --
// with one `sqrt`, and it is EXACT rather than converged-to-1e-6.
//
// `a` vanishes exactly when the quad is a parallelogram (D = 0, or D parallel
// to B), which a lightly bent warp's quads very nearly are, so the linear
// branch is the common one and is taken on its own merits, not as a
// degenerate fallback.
//
// Returns false -- and the caller skips this quad -- when no real root exists
// (the pixel genuinely has no preimage in this quad) or when both leading
// coefficients vanish (a degenerate quad). A folded warp can give a pixel two
// preimages inside one quad; both roots are scored and the more interior one
// wins, which is the same tie-break the gather loop applies ACROSS quads.
bool invertBilinearQuad(const WarpQuad& q, Point2 p, float* s, float* t) noexcept {
  const float bx = q.dst10.x - q.dst00.x, by = q.dst10.y - q.dst00.y;
  const float cx = q.dst01.x - q.dst00.x, cy = q.dst01.y - q.dst00.y;
  const float dx = q.dst11.x - q.dst10.x - q.dst01.x + q.dst00.x;
  const float dy = q.dst11.y - q.dst10.y - q.dst01.y + q.dst00.y;
  const float ex = p.x - q.dst00.x, ey = p.y - q.dst00.y;

  const float crossBD = bx * dy - by * dx;
  const float crossBC = bx * cy - by * cx;
  const float crossED = ex * dy - ey * dx;
  const float crossEC = ex * cy - ey * cx;

  const float a = crossBD;
  const float b = crossBC - crossED;
  const float c = -crossEC;

  // `t` for a candidate `s`, plus how interior the pair is. Divides along the
  // larger component of `C + sD` so a direction that is near-axis-aligned
  // does not divide by its own near-zero component.
  auto solveT = [&](float ss, float* tt) noexcept {
    const float rx = cx + ss * dx, ry = cy + ss * dy;
    const float nx = ex - ss * bx, ny = ey - ss * by;
    if (std::fabs(rx) >= std::fabs(ry)) {
      if (std::fabs(rx) < 1e-12f) return false;
      *tt = nx / rx;
    } else {
      *tt = ny / ry;
    }
    return true;
  };
  auto margin = [](float ss, float tt) noexcept {
    return std::min({ss, 1.0f - ss, tt, 1.0f - tt});
  };

  float roots[2];
  int rootCount = 0;
  // Scaled against the other coefficients rather than an absolute epsilon:
  // these are cross products of DOCUMENT-pixel vectors, so what counts as
  // "zero" for `a` depends on how large the quad is, not on a fixed number.
  const float scale = std::fabs(b) + std::fabs(a) + std::fabs(c) + 1e-20f;
  if (std::fabs(a) < 1e-7f * scale) {
    if (std::fabs(b) < 1e-12f * scale) return false;  // degenerate quad
    roots[rootCount++] = -c / b;
  } else {
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return false;  // no preimage in this quad at all
    const float sq = std::sqrt(disc);
    // The sign-stable form: computing both roots as (-b +/- sq) / 2a loses
    // the small one to cancellation when |b| >> |sq|.
    const float qq = -0.5f * (b + (b >= 0.0f ? sq : -sq));
    roots[rootCount++] = qq / a;
    if (std::fabs(qq) > 1e-20f) roots[rootCount++] = c / qq;
  }

  bool any = false;
  float bestMargin = -std::numeric_limits<float>::infinity();
  for (int i = 0; i < rootCount; ++i) {
    float tt = 0.0f;
    if (!solveT(roots[i], &tt)) continue;
    const float m = margin(roots[i], tt);
    if (!any || m > bestMargin) {
      any = true;
      bestMargin = m;
      *s = roots[i];
      *t = tt;
    }
  }
  return any;
}

// A uniform bucket grid over destination-local pixel coordinates, mapping
// each bucket to the indices of every `WarpQuad` whose pixel bounding box
// touches it. `warpImage()`'s gather rasteriser (below) looks up ONE bucket
// per destination pixel instead of scanning every quad -- the same reason
// any other broad-phase spatial index exists. Carried over unchanged from
// the Bezier design: gather-by-owning-quad is just as correct and just as
// parallel over a Catmull-Rom-tessellated set of quads as over a Bezier one
// -- what changed is only the FUNCTION `tessellateWarpMesh()` calls to place
// each quad's corners, not this broad-phase/gather architecture itself.
class QuadBucketGrid {
 public:
  QuadBucketGrid(int width, int height, int cellSize)
      : width_(width), height_(height), cellSize_(std::max(cellSize, 1)) {
    cols_ = (width_ + cellSize_ - 1) / cellSize_;
    rows_ = (height_ + cellSize_ - 1) / cellSize_;
    cols_ = std::max(cols_, 1);
    rows_ = std::max(rows_, 1);
    buckets_.resize(static_cast<size_t>(cols_) * rows_);
  }

  void insert(uint32_t quadIndex, int lx, int ly, int hx, int hy) {
    lx = std::clamp(lx, 0, width_ - 1);
    hx = std::clamp(hx, 0, width_ - 1);
    ly = std::clamp(ly, 0, height_ - 1);
    hy = std::clamp(hy, 0, height_ - 1);
    if (lx > hx || ly > hy) return;
    const int bx0 = lx / cellSize_, bx1 = hx / cellSize_;
    const int by0 = ly / cellSize_, by1 = hy / cellSize_;
    for (int by = by0; by <= by1; ++by) {
      for (int bx = bx0; bx <= bx1; ++bx) {
        buckets_[static_cast<size_t>(by) * cols_ + bx].push_back(quadIndex);
      }
    }
  }

  const std::vector<uint32_t>& bucketAt(int px, int py) const noexcept {
    const int bx = std::clamp(px / cellSize_, 0, cols_ - 1);
    const int by = std::clamp(py / cellSize_, 0, rows_ - 1);
    return buckets_[static_cast<size_t>(by) * cols_ + bx];
  }

 private:
  int width_, height_, cellSize_, cols_, rows_;
  std::vector<std::vector<uint32_t>> buckets_;
};

// One kernel-weighted sample of `src` at (sx, sy), source-local pixel
// coordinates with the usual pixel-centre-at-half-integer convention
// (`ops/DocumentTransform.hpp`'s own Point2 comment). Taps that overhang the
// source edge clamp to the edge texel -- `ops/Transform.hpp`'s own edge
// policy for a kernel footprint, reused verbatim rather than re-decided.
void sampleKernel(const TransformImage& src, float sx, float sy, ResampleKernel kernel,
                  float* out4) noexcept {
  const float radius = resampleKernelRadius(kernel);
  const int ix0 = static_cast<int>(std::floor(sx - radius));
  const int ix1 = static_cast<int>(std::floor(sx + radius)) + 1;
  const int iy0 = static_cast<int>(std::floor(sy - radius));
  const int iy1 = static_cast<int>(std::floor(sy + radius)) + 1;
  float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float wsum = 0.0f;
  const int w = static_cast<int>(src.width), h = static_cast<int>(src.height);
  // Both weight vectors are evaluated ONCE, rather than the column weights
  // being re-evaluated inside the row loop for every row of the footprint --
  // `ops/Transform.cpp`'s affine sampler already hoists its own this way and
  // this one had simply not. For the CatmullRom default that is 12
  // `resampleKernelWeight()` calls per destination pixel instead of 42, and
  // that function lives in another translation unit, so without LTO each one
  // is a real call through a switch.
  constexpr int kMaxTaps = 8;  // Lanczos3 (radius 3) is the widest: 8 columns
  const int nx = ix1 - ix0 + 1;
  const int ny = iy1 - iy0 + 1;
  float wxs[kMaxTaps], wys[kMaxTaps];
  if (nx <= 0 || ny <= 0 || nx > kMaxTaps || ny > kMaxTaps) {  // unreachable for the kernels that exist
    out4[0] = out4[1] = out4[2] = out4[3] = 0.0f;
    return;
  }
  for (int k = 0; k < nx; ++k)
    wxs[k] = resampleKernelWeight(kernel, sx - (static_cast<float>(ix0 + k) + 0.5f));
  for (int k = 0; k < ny; ++k)
    wys[k] = resampleKernelWeight(kernel, sy - (static_cast<float>(iy0 + k) + 0.5f));

  // The tap window above is deliberately one wider than any kernel's support
  // on each side (`ops/Transform.cpp` says why: a tight bound computed with
  // ceil/floor on a float is where an off-by-one at exact ties lives), so its
  // outermost taps weigh exactly zero -- two of the six columns, for
  // CatmullRom. Trimming them off the ENDS, by looking at the weights that
  // were actually computed rather than by re-deriving a tighter index range,
  // keeps the wide window's tie-safety and still shrinks the inner loop from
  // 6x6 to 4x4. Bit-identical: a tap dropped here contributed `0.0f * texel`
  // to the accumulator and `0.0f` to `wsum`.
  int kx0 = 0, kx1 = nx - 1, ky0 = 0, ky1 = ny - 1;
  while (kx0 <= kx1 && wxs[kx0] == 0.0f) ++kx0;
  while (kx1 >= kx0 && wxs[kx1] == 0.0f) --kx1;
  while (ky0 <= ky1 && wys[ky0] == 0.0f) ++ky0;
  while (ky1 >= ky0 && wys[ky1] == 0.0f) --ky1;

  for (int ky = ky0; ky <= ky1; ++ky) {
    const float wy = wys[ky];
    if (wy == 0.0f) continue;
    const int cy = std::clamp(iy0 + ky, 0, h - 1);
    const float* row = src.px.data() + static_cast<size_t>(cy) * w * 4u;
    for (int kx = kx0; kx <= kx1; ++kx) {
      const float wx = wxs[kx];
      if (wx == 0.0f) continue;  // an interior zero, which trimming the ends cannot remove
      const int cx = std::clamp(ix0 + kx, 0, w - 1);
      const float weight = wx * wy;
      const float* texel = row + static_cast<size_t>(cx) * 4u;
      acc[0] += weight * texel[0];
      acc[1] += weight * texel[1];
      acc[2] += weight * texel[2];
      acc[3] += weight * texel[3];
      wsum += weight;
    }
  }
  if (wsum > 1e-8f) {
    const float inv = 1.0f / wsum;
    out4[0] = acc[0] * inv;
    out4[1] = acc[1] * inv;
    out4[2] = acc[2] * inv;
    out4[3] = acc[3] * inv;
  } else {
    out4[0] = out4[1] = out4[2] = out4[3] = 0.0f;
  }
}

}  // namespace

bool warpImage(const TransformImage& src, const WarpMesh& mesh, const DocumentRegion& dstRegion,
              ResampleKernel kernel, TransformImage* out, std::string* errorOut) {
  if (out == nullptr || out == &src) {
    if (errorOut) *errorOut = "warp refused: no destination image, or destination aliases source.";
    return false;
  }
  *out = TransformImage{};
  if (dstRegion.empty()) return true;
  out->width = dstRegion.width;
  out->height = dstRegion.height;
  out->px.assign(out->sampleCount(), 0.0f);

  if (src.width != mesh.bounds().width || src.height != mesh.bounds().height) {
    if (errorOut)
      *errorOut = "warp refused: the source image (" + std::to_string(src.width) + "x" +
                  std::to_string(src.height) + ") does not cover the mesh's own bounds (" +
                  std::to_string(mesh.bounds().width) + "x" + std::to_string(mesh.bounds().height) +
                  ").";
    return false;
  }
  if (mesh.bounds().empty()) return true;

  // Fast, bit-exact path for an untouched net -- see `WarpMesh::isIdentity()`.
  if (mesh.isIdentity() && dstRegion == mesh.bounds()) {
    *out = src;
    return true;
  }

  const int subdivisions = warpChordSubdivisions(mesh);
  const std::vector<WarpQuad> quads = tessellateWarpMesh(mesh, subdivisions);
  const float srcW = static_cast<float>(src.width);
  const float srcH = static_cast<float>(src.height);
  const float cellCount = static_cast<float>(std::max(mesh.cells(), 1));
  const int outW = static_cast<int>(out->width), outH = static_cast<int>(out->height);
  if (quads.empty()) return true;

  // --- Broad phase: bucket every quad by its own pixel bounding box --------
  // Unchanged from the Bezier design (see class comment above): gather asks,
  // per destination pixel, "which quad's PARAMETRIC interior actually
  // contains me", well-defined everywhere the surface does not fold over
  // itself, and embarrassingly parallel across destination pixels.
  double bboxPerimeterSum = 0.0;
  std::vector<std::array<int, 4>> quadBoxes(quads.size());  // lx, ly, hx, hy (destination-local)
  for (size_t i = 0; i < quads.size(); ++i) {
    const WarpQuad& q = quads[i];
    const float minX = std::min({q.dst00.x, q.dst10.x, q.dst11.x, q.dst01.x});
    const float maxX = std::max({q.dst00.x, q.dst10.x, q.dst11.x, q.dst01.x});
    const float minY = std::min({q.dst00.y, q.dst10.y, q.dst11.y, q.dst01.y});
    const float maxY = std::max({q.dst00.y, q.dst10.y, q.dst11.y, q.dst01.y});
    const int lx = static_cast<int>(std::floor(minX)) - dstRegion.x;
    const int hx = static_cast<int>(std::ceil(maxX)) - dstRegion.x;
    const int ly = static_cast<int>(std::floor(minY)) - dstRegion.y;
    const int hy = static_cast<int>(std::ceil(maxY)) - dstRegion.y;
    quadBoxes[i] = {lx, ly, hx, hy};
    bboxPerimeterSum += std::max(0, hx - lx) + std::max(0, hy - ly);
  }
  const int meanSpan = quads.empty()
                           ? 4
                           : std::clamp(static_cast<int>(bboxPerimeterSum / (2.0 * quads.size())) + 1,
                                       2, 64);
  QuadBucketGrid grid(outW, outH, meanSpan);
  for (size_t i = 0; i < quads.size(); ++i) {
    const auto& b = quadBoxes[i];
    grid.insert(static_cast<uint32_t>(i), b[0], b[1], b[2], b[3]);
  }

  // --- Gather: one destination pixel at a time, independent of every other,
  // so `parallelFor` (core/Parallel.hpp) hands rows to every core -- one row
  // (`outW` gathers, each a Newton solve plus a kernel-weighted resample) is
  // comfortably above core/Parallel.hpp's own measured per-tile floor, so
  // its default grain is used rather than a bespoke number. No boundary
  // antialiasing here (deliberately, per this file's own header): a pixel
  // that finds no quad's strict interior is simply left transparent, same
  // hard edge `ops/Transform.hpp`'s affine path already has.
  parallelFor(static_cast<size_t>(outH), kParallelForDefaultGrain, [&](size_t iyz) {
    const int iy = static_cast<int>(iyz);
    const float py = static_cast<float>(dstRegion.y + iy) + 0.5f;
    for (int ix = 0; ix < outW; ++ix) {
      const float px = static_cast<float>(dstRegion.x + ix) + 0.5f;
      const std::vector<uint32_t>& candidates = grid.bucketAt(ix, iy);
      if (candidates.empty()) continue;

      // The pixel's true owner -- the quad whose PARAMETRIC interior
      // contains it, picking the most interior candidate on the rare overlap
      // a self-intersecting (folded) warp produces, since "most interior" is
      // the candidate least likely to be the sliver on the wrong side of a
      // fold.
      int bestIdx = -1;
      float bestS = 0.0f, bestT = 0.0f, bestMargin = -1.0f;
      constexpr float kStrictEps = 1e-4f;
      for (uint32_t qi : candidates) {
        float s = 0.0f, t = 0.0f;
        if (!invertBilinearQuad(quads[qi], Point2{px, py}, &s, &t)) continue;
        if (s < -kStrictEps || s > 1.0f + kStrictEps || t < -kStrictEps || t > 1.0f + kStrictEps)
          continue;
        const float margin = std::min({s, 1.0f - s, t, 1.0f - t});
        if (margin > bestMargin) {
          bestMargin = margin;
          bestIdx = static_cast<int>(qi);
          bestS = s;
          bestT = t;
        }
      }
      if (bestIdx < 0) continue;  // no quad claims this pixel -- hard edge, left transparent

      const WarpQuad& q = quads[static_cast<size_t>(bestIdx)];
      const float u = q.u0 + (q.u1 - q.u0) * clamp01(bestS);
      const float v = q.v0 + (q.v1 - q.v0) * clamp01(bestT);
      const float sx = (u / cellCount) * srcW;
      const float sy = (v / cellCount) * srcH;
      float texel[4];
      sampleKernel(src, sx, sy, kernel, texel);
      float* d = out->px.data() + (static_cast<size_t>(iy) * outW + ix) * 4u;
      d[0] = texel[0];
      d[1] = texel[1];
      d[2] = texel[2];
      d[3] = texel[3];
    }
  });
  return true;
}

bool warpRgbTiles(const TileStore& in, const WarpMesh& mesh, const DocumentRegion& dstRegion,
                  ResampleKernel kernel, TileStore* out, std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "warp refused: no destination tile store was given.";
    return false;
  }
  *out = TileStore{};
  if (mesh.bounds().empty() || dstRegion.empty()) return true;
  const DocumentRegion srcRegion = mesh.bounds();
  const TransformImage src =
      imageFromTileStore(in, srcRegion.x, srcRegion.y, srcRegion.width, srcRegion.height);
  TransformImage dst;
  if (!warpImage(src, mesh, dstRegion, kernel, &dst, errorOut)) return false;
  tileStoreFromImage(dst, dstRegion.x, dstRegion.y, out);
  return true;
}

bool warpSelectionCoverage(const Selection& in, const DocumentRegion& srcRegion,
                          const WarpMesh& mesh, const DocumentRegion& dstRegion,
                          ResampleKernel kernel, Selection* out, std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "warp refused: no destination selection was given.";
    return false;
  }
  *out = Selection{};
  if (srcRegion.empty() || dstRegion.empty()) return true;

  TransformImage src;
  src.width = srcRegion.width;
  src.height = srcRegion.height;
  src.px.assign(static_cast<size_t>(srcRegion.width) * srcRegion.height * 4u, 0.0f);
  for (uint32_t y = 0; y < srcRegion.height; ++y) {
    for (uint32_t x = 0; x < srcRegion.width; ++x) {
      const PixelCoord doc{srcRegion.x + static_cast<int32_t>(x),
                           srcRegion.y + static_cast<int32_t>(y)};
      const SelectionTile* tile = in.tiles.find(tileCoordAt(doc));
      const float c = tile ? tile->coverageAt(tileLocalOffset(doc)) : 0.0f;
      float* d = src.px.data() + (static_cast<size_t>(y) * srcRegion.width + x) * 4u;
      d[0] = d[1] = d[2] = d[3] = c;
    }
  }

  TransformImage dst;
  if (!warpImage(src, mesh, dstRegion, kernel, &dst, errorOut)) return false;

  for (uint32_t y = 0; y < dstRegion.height; ++y) {
    for (uint32_t x = 0; x < dstRegion.width; ++x) {
      const float c = dst.px[(static_cast<size_t>(y) * dstRegion.width + x) * 4u + 3u];
      if (!(c > 0.0f)) continue;  // keeps the "no all-zero tile" invariant for free
      const PixelCoord doc{dstRegion.x + static_cast<int32_t>(x),
                           dstRegion.y + static_cast<int32_t>(y)};
      out->tiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), c);
    }
  }
  return true;
}

}  // namespace np
