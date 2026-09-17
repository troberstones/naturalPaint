#include "app/WarpMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/Parallel.hpp"

namespace np {
namespace {

// Bernstein cubic basis at `t`, already clamped to [0,1] by every caller.
std::array<float, 4> bernstein(float t) noexcept {
  const float u = 1.0f - t;
  return {u * u * u, 3.0f * u * u * t, 3.0f * u * t * t, t * t * t};
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
  const float denom = static_cast<float>(3 * m.n_);
  for (int row = 0; row < side; ++row) {
    for (int col = 0; col < side; ++col) {
      // Section 2: evenly spaced points along the rectangle's own edges
      // reproduce it exactly, because a plane through 16 coplanar control
      // points IS the plane.
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
  setAt(ref.row, ref.col, Point2{at(ref.row, ref.col).x + delta.x, at(ref.row, ref.col).y + delta.y});
  if (!ref.isAnchor()) return;
  // Section 3: the up-to-four cardinal neighbours ride along rigidly, which
  // preserves each one's tangent direction and length exactly.
  const int side = pointsPerSide();
  const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  for (const auto& o : offsets) {
    const int r = ref.row + o[0], c = ref.col + o[1];
    if (r < 0 || c < 0 || r >= side || c >= side) continue;
    // A neighbour that is ALSO an anchor (adjacent cell size 1, i.e. n==... )
    // cannot happen for n >= 2 since anchors are 3 apart, but guard anyway:
    // dragging one anchor must never silently drag a different one.
    const WarpControlRef neighbour{true, r, c};
    if (neighbour.isAnchor()) continue;
    setAt(r, c, Point2{at(r, c).x + delta.x, at(r, c).y + delta.y});
  }
}

Point2 WarpMesh::evaluate(float u, float v) const noexcept {
  u = std::clamp(u, 0.0f, static_cast<float>(n_));
  v = std::clamp(v, 0.0f, static_cast<float>(n_));
  int cellI = static_cast<int>(std::floor(u));
  int cellJ = static_cast<int>(std::floor(v));
  cellI = std::clamp(cellI, 0, n_ - 1);
  cellJ = std::clamp(cellJ, 0, n_ - 1);
  const float t = clamp01(u - static_cast<float>(cellI));
  const float s = clamp01(v - static_cast<float>(cellJ));
  const std::array<float, 4> bu = bernstein(t);
  const std::array<float, 4> bv = bernstein(s);
  float x = 0.0f, y = 0.0f;
  for (int b = 0; b < 4; ++b) {
    for (int a = 0; a < 4; ++a) {
      const Point2 p = at(3 * cellJ + b, 3 * cellI + a);
      const float w = bu[static_cast<size_t>(a)] * bv[static_cast<size_t>(b)];
      x += w * p.x;
      y += w * p.y;
    }
  }
  return Point2{x, y};
}

WarpMesh WarpMesh::refit(int newN) const {
  const int clampedN = std::clamp(newN, 3, 5);
  if (isAffine()) {
    // Exact path: the same affine fit, re-sampled at the new flat net's own
    // fractional positions.
    const int side = pointsPerSide();
    const int last = side - 1;
    const Point2 origin = at(0, 0);
    const Point2 xAxis = Point2{at(0, last).x - origin.x, at(0, last).y - origin.y};
    const Point2 yAxis = Point2{at(last, 0).x - origin.x, at(last, 0).y - origin.y};
    WarpMesh out;
    out.n_ = clampedN;
    out.bounds_ = bounds_;
    const int newSide = out.pointsPerSide();
    out.net_.assign(static_cast<size_t>(newSide) * newSide, Point2{});
    const float denom = static_cast<float>(newSide - 1);
    for (int row = 0; row < newSide; ++row) {
      for (int col = 0; col < newSide; ++col) {
        const float fu = static_cast<float>(col) / denom;
        const float fv = static_cast<float>(row) / denom;
        out.net_[static_cast<size_t>(row) * newSide + col] =
            Point2{origin.x + fu * xAxis.x + fv * yAxis.x, origin.y + fu * xAxis.y + fv * yAxis.y};
      }
    }
    return out;
  }

  // Approximate path: place the new net's ANCHORS at this surface's own
  // `evaluate()`, then rebuild handles locally the way `flat()` derives them
  // -- thirds of the straight segment between neighbouring (already-curved)
  // anchors. Named as an approximation in this header's own `refit()` doc.
  WarpMesh out;
  out.n_ = clampedN;
  out.bounds_ = bounds_;
  const int newSide = out.pointsPerSide();
  out.net_.assign(static_cast<size_t>(newSide) * newSide, Point2{});
  std::vector<Point2> anchors(static_cast<size_t>(clampedN + 1) * (clampedN + 1));
  for (int j = 0; j <= clampedN; ++j) {
    for (int i = 0; i <= clampedN; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(clampedN) * static_cast<float>(n_);
      const float v = static_cast<float>(j) / static_cast<float>(clampedN) * static_cast<float>(n_);
      anchors[static_cast<size_t>(j) * (clampedN + 1) + i] = evaluate(u, v);
    }
  }
  auto anchorAt = [&](int i, int j) { return anchors[static_cast<size_t>(j) * (clampedN + 1) + i]; };
  for (int j = 0; j <= clampedN; ++j) {
    for (int i = 0; i <= clampedN; ++i) {
      out.setAt(3 * j, 3 * i, anchorAt(i, j));
    }
  }
  // Handles: thirds along the straight segment to each neighbouring anchor,
  // exactly `flat()`'s own construction applied per edge instead of globally.
  for (int j = 0; j <= clampedN; ++j) {
    for (int i = 0; i < clampedN; ++i) {
      const Point2 a = anchorAt(i, j), b = anchorAt(i + 1, j);
      out.setAt(3 * j, 3 * i + 1, Point2{a.x + (b.x - a.x) / 3.0f, a.y + (b.y - a.y) / 3.0f});
      out.setAt(3 * j, 3 * i + 2, Point2{a.x + (b.x - a.x) * 2.0f / 3.0f, a.y + (b.y - a.y) * 2.0f / 3.0f});
    }
  }
  for (int i = 0; i <= clampedN; ++i) {
    for (int j = 0; j < clampedN; ++j) {
      const Point2 a = anchorAt(i, j), b = anchorAt(i, j + 1);
      out.setAt(3 * j + 1, 3 * i, Point2{a.x + (b.x - a.x) / 3.0f, a.y + (b.y - a.y) / 3.0f});
      out.setAt(3 * j + 2, 3 * i, Point2{a.x + (b.x - a.x) * 2.0f / 3.0f, a.y + (b.y - a.y) * 2.0f / 3.0f});
    }
  }
  // Interior "twist" points (section 3's own scope note): bilinear blend of
  // the four surrounding handles just placed, which is the plain,
  // no-additional-curvature filler `flat()`'s own construction implies for
  // an unbent cell and stays reasonable for a mildly bent one.
  for (int cj = 0; cj < clampedN; ++cj) {
    for (int ci = 0; ci < clampedN; ++ci) {
      for (int b = 1; b <= 2; ++b) {
        for (int a = 1; a <= 2; ++a) {
          const Point2 left = out.at(3 * cj + b, 3 * ci);
          const Point2 right = out.at(3 * cj + b, 3 * ci + 3);
          const Point2 top = out.at(3 * cj, 3 * ci + a);
          const Point2 bottom = out.at(3 * cj + 3, 3 * ci + a);
          const float fa = static_cast<float>(a) / 3.0f;
          const float fb = static_cast<float>(b) / 3.0f;
          const Point2 fromRow{left.x + (right.x - left.x) * fa, left.y + (right.y - left.y) * fa};
          const Point2 fromCol{top.x + (bottom.x - top.x) * fb, top.y + (bottom.y - top.y) * fb};
          out.setAt(3 * cj + b, 3 * ci + a,
                    Point2{0.5f * (fromRow.x + fromCol.x), 0.5f * (fromRow.y + fromCol.y)});
        }
      }
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
  const int n = mesh.n();
  for (int J = 0; J < n; ++J) {
    for (int I = 0; I < n; ++I) {
      // Section 4(d): bound every U-direction curve at any fixed V by the
      // max second difference of the cell's own control ROWS, and every
      // V-direction curve by its own control COLUMNS.
      for (int b = 0; b < 4; ++b) {
        const Point2 p0 = mesh.at(3 * J + b, 3 * I + 0);
        const Point2 p1 = mesh.at(3 * J + b, 3 * I + 1);
        const Point2 p2 = mesh.at(3 * J + b, 3 * I + 2);
        const Point2 p3 = mesh.at(3 * J + b, 3 * I + 3);
        const float d01 = std::hypot(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
        const float d12 = std::hypot(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y);
        dMax = std::max({dMax, d01, d12});
      }
      for (int a = 0; a < 4; ++a) {
        const Point2 p0 = mesh.at(3 * J + 0, 3 * I + a);
        const Point2 p1 = mesh.at(3 * J + 1, 3 * I + a);
        const Point2 p2 = mesh.at(3 * J + 2, 3 * I + a);
        const Point2 p3 = mesh.at(3 * J + 3, 3 * I + a);
        const float d01 = std::hypot(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
        const float d12 = std::hypot(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y);
        dMax = std::max({dMax, d01, d12});
      }
    }
  }
  if (dMax <= 0.0f) return 1;  // flat: one quad per cell already exact
  const float nSub = std::sqrt(3.0f * dMax / (4.0f * tol));
  return std::clamp(static_cast<int>(std::ceil(nSub)), 1, kMaxSubdivisionsPerCell);
}

std::vector<WarpQuad> tessellateWarpMesh(const WarpMesh& mesh, int subdivisionsPerCell) {
  const int sub = std::max(subdivisionsPerCell, 1);
  const int n = mesh.n();
  std::vector<WarpQuad> quads;
  quads.reserve(static_cast<size_t>(n) * n * sub * sub);
  for (int J = 0; J < n; ++J) {
    for (int I = 0; I < n; ++I) {
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

// Newton solve for the (s, t) in the quad's own parametrisation such that
// bilerp(q, s, t) == p. Eight iterations: the quad is small enough (chord
// bound) to be near-parallelogram, so this converges to float precision in
// two or three, and the fixed count costs nothing a data-dependent stopping
// rule would save here. Returns false (leaving `*s`/`*t` at their last
// estimate) when the quad is degenerate (a Jacobian that will not invert).
//
// `dsLenOut`/`dtLenOut` (optional): the local |dQ/ds| / |dQ/dt| pixels-per-
// parameter-unit at the converged (s, t) -- the last iteration's own
// Jacobian columns, already computed for the Newton step and reused rather
// than recomputed. `warpImage()`'s boundary antialiasing uses these to turn
// a parametric overshoot at the mesh's own outer edge into a physical pixel
// distance for a coverage ramp.
bool invertBilinearQuad(const WarpQuad& q, Point2 p, float* s, float* t,
                        float* dsLenOut = nullptr, float* dtLenOut = nullptr) noexcept {
  float ss = 0.5f, tt = 0.5f;
  float dsx = 0.0f, dsy = 0.0f, dtx = 0.0f, dty = 0.0f;
  for (int iter = 0; iter < 8; ++iter) {
    const float omS = 1.0f - ss, omT = 1.0f - tt;
    const Point2 Q{omS * omT * q.dst00.x + ss * omT * q.dst10.x + ss * tt * q.dst11.x +
                       omS * tt * q.dst01.x,
                   omS * omT * q.dst00.y + ss * omT * q.dst10.y + ss * tt * q.dst11.y +
                       omS * tt * q.dst01.y};
    const float rx = p.x - Q.x, ry = p.y - Q.y;
    // dQ/ds = (1-t)(dst10-dst00) + t(dst11-dst01)
    dsx = omT * (q.dst10.x - q.dst00.x) + tt * (q.dst11.x - q.dst01.x);
    dsy = omT * (q.dst10.y - q.dst00.y) + tt * (q.dst11.y - q.dst01.y);
    // dQ/dt = (1-s)(dst01-dst00) + s(dst11-dst10)
    dtx = omS * (q.dst01.x - q.dst00.x) + ss * (q.dst11.x - q.dst10.x);
    dty = omS * (q.dst01.y - q.dst00.y) + ss * (q.dst11.y - q.dst10.y);
    const float det = dsx * dty - dtx * dsy;
    if (std::fabs(det) < 1e-9f) {
      *s = ss;
      *t = tt;
      if (dsLenOut) *dsLenOut = std::hypot(dsx, dsy);
      if (dtLenOut) *dtLenOut = std::hypot(dtx, dty);
      return false;
    }
    const float invDet = 1.0f / det;
    const float dS = (rx * dty - dtx * ry) * invDet;
    const float dT = (dsx * ry - rx * dsy) * invDet;
    ss += dS;
    tt += dT;
    if (std::fabs(dS) < 1e-6f && std::fabs(dT) < 1e-6f) break;
  }
  *s = ss;
  *t = tt;
  if (dsLenOut) *dsLenOut = std::hypot(dsx, dsy);
  if (dtLenOut) *dtLenOut = std::hypot(dtx, dty);
  return true;
}

// A uniform bucket grid over destination-local pixel coordinates, mapping
// each bucket to the indices of every `WarpQuad` whose pixel bounding box
// touches it. `warpImage()`'s gather rasteriser (below) looks up ONE bucket
// per destination pixel instead of scanning every quad -- the same reason
// any other broad-phase spatial index exists.
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
  for (int iy = iy0; iy <= iy1; ++iy) {
    const float wy = resampleKernelWeight(kernel, sy - (static_cast<float>(iy) + 0.5f));
    if (wy == 0.0f) continue;
    const int cy = std::clamp(iy, 0, h - 1);
    for (int ix = ix0; ix <= ix1; ++ix) {
      const float wx = resampleKernelWeight(kernel, sx - (static_cast<float>(ix) + 0.5f));
      if (wx == 0.0f) continue;
      const int cx = std::clamp(ix, 0, w - 1);
      const float weight = wx * wy;
      const float* texel = src.px.data() + (static_cast<size_t>(cy) * w + cx) * 4u;
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
  const float n = static_cast<float>(mesh.n());
  const int outW = static_cast<int>(out->width), outH = static_cast<int>(out->height);
  if (quads.empty()) return true;

  // --- Broad phase: bucket every quad by its own pixel bounding box --------
  //
  // Was a scatter (iterate quads, write whichever destination pixels each
  // one's bbox+membership test claims): with `dstRegion` sized to the union
  // of every quad, neighbouring quads' boxes overlap wherever the surface
  // curves, and the OLD code let whichever quad happened to be LAST in raster
  // order win that overlap with no ownership rule -- a big, wrongly-shaped,
  // uniformly-sampled patch wherever a late quad's box swept over pixels an
  // earlier, correct quad had already written. Gather (below) instead asks,
  // per destination pixel, "which quad's PARAMETRIC interior actually
  // contains me", which is well-defined everywhere the surface does not fold
  // over itself -- the tessellation partitions `[0,n]x[0,n]` into
  // non-overlapping cells, and away from a fold their IMAGES do not overlap
  // either. That also makes the pixel loop embarrassingly parallel: each
  // destination pixel is resolved independently, unlike the old scatter loop
  // whose write order was the whole correctness argument.
  //
  // Bucket size: the mean quad footprint, so a typical pixel's bucket holds
  // a small, roughly constant number of candidates regardless of `n` or the
  // chord-error subdivision count.
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
  // so `parallelFor` (core/Parallel.hpp) can hand rows to every core instead
  // of this running single-threaded -- the whole fix for PRD D23's warp
  // preview costing ~5.3 s/recompute on an iPad (vs ~0.9 s on an M-series
  // Mac): the per-texel work here is the same kernel-weighted resample the
  // old scatter loop did, it is just no longer serialised through one
  // core. -----------------------------------------------------------------
  // TEMPORARY (device profiling A/B, remove before landing): grain >= outH
  // forces parallelFor's own serial fallback path (core/Parallel.hpp: "below
  // `grain` items, parallelFor just runs the loop serially"), isolating the
  // parallelism variable from the broad-phase/gather-rasteriser rewrite so
  // the iPad's real per-core speedup can be measured against the identical
  // algorithm.
  parallelFor(static_cast<size_t>(outH), static_cast<size_t>(outH) + 1, [&](size_t iyz) {
    const int iy = static_cast<int>(iyz);
    const float py = static_cast<float>(dstRegion.y + iy) + 0.5f;
    for (int ix = 0; ix < outW; ++ix) {
      const float px = static_cast<float>(dstRegion.x + ix) + 0.5f;
      const std::vector<uint32_t>& candidates = grid.bucketAt(ix, iy);
      if (candidates.empty()) continue;

      // Pass 1: the pixel's true owner -- the quad whose PARAMETRIC interior
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

      float coverage = 1.0f;
      if (bestIdx < 0) {
        // Pass 2: nobody's strict interior reached this pixel. Only relevant
        // within half a texel of the mesh's own OUTER silhouette (the warp's
        // boundary antialiasing this function now provides, scoped to warp
        // the way `ops/Transform.hpp`'s hard-edged affine path is not asked
        // to change): an interior seam between two cells is already covered
        // by pass 1 above (their shared control points make their strict
        // interiors meet with no gap beyond float noise), so a pixel that
        // reaches here from an INTERIOR edge is a rounding sliver smaller
        // than the antialiasing band and is left transparent same as before
        // rather than guessed at.
        float bestPixelDist = -1.0f;
        for (uint32_t qi : candidates) {
          const WarpQuad& q = quads[qi];
          const bool outerU0 = q.u0 <= 0.0f, outerU1 = q.u1 >= n;
          const bool outerV0 = q.v0 <= 0.0f, outerV1 = q.v1 >= n;
          if (!outerU0 && !outerU1 && !outerV0 && !outerV1) continue;
          float s = 0.0f, t = 0.0f, dsLen = 0.0f, dtLen = 0.0f;
          if (!invertBilinearQuad(q, Point2{px, py}, &s, &t, &dsLen, &dtLen)) continue;
          const float kRelaxedEps = 1.0f;  // parameter units; converted to pixels below
          if (s < -kRelaxedEps || s > 1.0f + kRelaxedEps || t < -kRelaxedEps ||
              t > 1.0f + kRelaxedEps)
            continue;
          // Overshoot on whichever side is actually an outer edge, in pixels.
          float pixelDist = 1e9f;
          if (outerU0) pixelDist = std::min(pixelDist, -s * dsLen);
          if (outerU1) pixelDist = std::min(pixelDist, (1.0f - s) * dsLen);
          if (outerV0) pixelDist = std::min(pixelDist, -t * dtLen);
          if (outerV1) pixelDist = std::min(pixelDist, (1.0f - t) * dtLen);
          if (pixelDist > bestPixelDist) {
            bestPixelDist = pixelDist;
            bestIdx = static_cast<int>(qi);
            bestS = s;
            bestT = t;
          }
        }
        if (bestIdx < 0) continue;
        // Half-pixel antialiasing ramp: full coverage a half pixel inside the
        // true edge, zero a half pixel outside -- the standard analytic-
        // coverage width for a hard edge sampled at the pixel centre.
        coverage = std::clamp(bestPixelDist / 0.5f + 0.5f, 0.0f, 1.0f);
        if (coverage <= 0.0f) continue;
      }

      const WarpQuad& q = quads[static_cast<size_t>(bestIdx)];
      const float u = q.u0 + (q.u1 - q.u0) * clamp01(bestS);
      const float v = q.v0 + (q.v1 - q.v0) * clamp01(bestT);
      const float sx = (u / n) * srcW;
      const float sy = (v / n) * srcH;
      float texel[4];
      sampleKernel(src, sx, sy, kernel, texel);
      float* d = out->px.data() + (static_cast<size_t>(iy) * outW + ix) * 4u;
      d[0] = texel[0] * coverage;
      d[1] = texel[1] * coverage;
      d[2] = texel[2] * coverage;
      d[3] = texel[3] * coverage;
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
