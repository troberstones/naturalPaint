#include "core/PathRaster.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

int32_t floorInt(float v) noexcept {
  return static_cast<int32_t>(std::floor(v));
}

// Coverage at or below this is treated as none. Not a quality setting: the
// accumulation is a sum of signed floats that cancels to zero outside the
// shape, and cancellation leaves noise of order 1e-7. Without a floor, a
// closed path emits a one-texel span of coverage 3e-8 along every scanline
// outside itself -- harmless to look at, but it makes a sparse consumer
// allocate a tile per row for a shape that does not touch them.
constexpr float kCoverageEpsilon = 1.0e-6f;

// One edge, already clipped to a single scanline row and to the clip's x
// range in the *cover* sense (see the header, section 3).
//
// `cover` is the signed vertical extent the edge spans inside a cell and
// `area` the signed area it cuts from that cell to the right of itself. The
// sweep in `sweepRow()` turns the pair into coverage; see this file's
// `accumulateSegment()` for the derivation of the `1 - (x1 + x2) / 2` term.
struct RowAccumulator {
  float* cover = nullptr;
  float* area = nullptr;
  int32_t clipX0 = 0;
  int32_t clipX1 = 0;
  // The columns actually touched this row, so the sweep and the clear both
  // cost the path's own width rather than the clip's.
  int32_t minX = 0;
  int32_t maxX = 0;
  bool any = false;
  // Set when an edge fell entirely right of the clip. Such an edge deposits
  // nothing (nothing inside the clip is right of it), but the winding it
  // *would* have cancelled never arrives -- so the fill it opens has to run to
  // the clip's right edge rather than stopping at the last touched cell.
  // Without this a shape wider than the clip is drawn as a single column.
  bool rightOfClip = false;

  void touch(int32_t ex) noexcept {
    if (!any) {
      any = true;
      minX = maxX = ex;
      return;
    }
    minX = std::min(minX, ex);
    maxX = std::max(maxX, ex);
  }
};

// Accumulate the part of one edge that lies inside one cell.
//
// `lx0`/`lx1` are cell-local x in [0, 1]; `dy` is the signed vertical extent
// covered inside this cell, already carrying the edge's winding direction.
//
// The area term: filling to the right of the edge, the region this piece
// contributes inside the cell is a trapezoid whose parallel sides are
// `1 - lx0` and `1 - lx1` and whose height is `dy`, so its area is
// `dy * (1 - (lx0 + lx1) / 2)`. `cover` is `dy` alone, which is what every
// cell strictly to the right of this one receives in full.
void accumulateCell(RowAccumulator& acc, int32_t ex, float lx0, float lx1,
                    float dy) noexcept {
  // Section 3: an edge left of the clip is clamped into the first column, not
  // dropped, so that shapes larger than the clip still fill it.
  if (ex < acc.clipX0) {
    ex = acc.clipX0;
    lx0 = lx1 = 0.0f;
  } else if (ex >= acc.clipX1) {
    // Right of the clip: no texel inside the clip lies to the right of this
    // edge, so it deposits nothing -- but the sweep must know it existed.
    acc.rightOfClip = true;
    return;
  }
  const size_t i = static_cast<size_t>(ex - acc.clipX0);
  acc.cover[i] += dy;
  acc.area[i] += dy * (1.0f - (lx0 + lx1) * 0.5f);
  acc.touch(ex);
}

// Accumulate one edge piece that lies within a single scanline row, splitting
// it at every integer x boundary it crosses.
//
// `dir` is the edge's winding direction (+1 for an edge that originally ran
// downwards, -1 for one that ran up). Every `dy` handed to `accumulateCell()`
// is already multiplied by it, so the sweep sees signed winding and never has
// to know which way an edge was drawn.
void accumulateSegment(RowAccumulator& acc, float x0, float y0, float x1,
                       float y1, float dir) noexcept {
  // Two distinct quantities, and conflating them is a sign bug that only
  // shows on upward edges: `dyGeom` is how far the segment actually travels
  // in y (used to interpolate positions), `dir` is the winding it carries
  // (used to scale every deposit).
  const float dyGeom = y1 - y0;
  if (dyGeom == 0.0f) return;  // horizontal: contributes no winding

  const float dx = x1 - x0;
  if (dx == 0.0f) {
    const int32_t ex = floorInt(x0);
    const float lx = x0 - static_cast<float>(ex);
    accumulateCell(acc, ex, lx, lx, dyGeom * dir);
    return;
  }

  const int32_t exFirst = floorInt(x0);
  const int32_t exLast = floorInt(x1);
  if (exFirst == exLast) {
    const float base = static_cast<float>(exFirst);
    accumulateCell(acc, exFirst, x0 - base, x1 - base, dyGeom * dir);
    return;
  }

  // Walk cell by cell in the direction of travel, splitting at each integer
  // x boundary and interpolating y there.
  const int32_t step = (dx > 0.0f) ? 1 : -1;
  const float invDx = 1.0f / dx;
  int32_t ex = exFirst;
  float xCur = x0;
  float yCur = y0;

  while (ex != exLast) {
    // The x boundary this cell exits through.
    const float xNext = static_cast<float>((step > 0) ? (ex + 1) : ex);
    const float t = (xNext - x0) * invDx;
    const float yNext = y0 + t * dyGeom;
    const float base = static_cast<float>(ex);
    accumulateCell(acc, ex, xCur - base, xNext - base, (yNext - yCur) * dir);
    xCur = xNext;
    yCur = yNext;
    ex += step;
  }
  const float base = static_cast<float>(ex);
  accumulateCell(acc, ex, xCur - base, x1 - base, (y1 - yCur) * dir);
}

// Apply the fill rule to an accumulated signed winding-coverage value.
//
// NonZero saturates the magnitude: any nonzero winding is inside.
// EvenOdd folds it into a triangle wave, so winding 0 and 2 are both outside
// and 1 and 3 both inside, with the ramp between them carrying the
// antialiasing. Section 2 of the header states where this is exact.
float applyFillRule(float acc, FillRule rule) noexcept {
  if (rule == FillRule::NonZero) return std::min(std::fabs(acc), 1.0f);
  float v = std::fmod(acc, 2.0f);
  if (v < 0.0f) v += 2.0f;
  return (v > 1.0f) ? (2.0f - v) : v;
}

}  // namespace

void rasterizeContours(const std::vector<FlatContour>& contours, FillRule rule,
                       const RasterClip& clip, PathRasterScratch& scratch,
                       const SpanFn& emit) {
  if (!emit) return;
  const int32_t width = clip.x1 - clip.x0;
  const int32_t height = clip.y1 - clip.y0;
  if (width <= 0 || height <= 0) return;

  const size_t w = static_cast<size_t>(width);
  if (scratch.cover.size() < w) scratch.cover.assign(w, 0.0f);
  if (scratch.area.size() < w) scratch.area.assign(w, 0.0f);
  if (scratch.coverage.size() < w) scratch.coverage.assign(w, 0.0f);
  // A reused scratch may be wider than this clip and dirty beyond `w`; only
  // the first `w` entries are ever read, and they are cleared per row below.
  std::fill(scratch.cover.begin(), scratch.cover.begin() + width, 0.0f);
  std::fill(scratch.area.begin(), scratch.area.begin() + width, 0.0f);

  // Gather edges once. Each is stored top-to-bottom with its winding
  // direction, so the row walk never has to reason about orientation again.
  struct Edge {
    float x0, y0, x1, y1;  // y0 < y1
    float dir;             // +1 if the original ran downwards, else -1
  };
  std::vector<Edge> edges;
  for (const FlatContour& c : contours) {
    const size_t n = c.points.size();
    if (n < 2) continue;
    // An open contour is filled as if closed -- that is what a fill means, and
    // it matches SVG, which closes every subpath implicitly when filling.
    for (size_t i = 0; i < n; ++i) {
      const PathPoint& a = c.points[i];
      const PathPoint& b = c.points[(i + 1) % n];
      if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
          !std::isfinite(b.y))
        continue;
      if (a.y == b.y) continue;
      if (a.y < b.y)
        edges.push_back(Edge{a.x, a.y, b.x, b.y, 1.0f});
      else
        edges.push_back(Edge{b.x, b.y, a.x, a.y, -1.0f});
    }
  }
  if (edges.empty()) return;

  // Bucket by first row so each row visits only the edges that start in it,
  // and carry an active list forward. Without this the cost is
  // O(rows * edges) and a full-page path with a few thousand edges becomes
  // quadratic.
  std::sort(edges.begin(), edges.end(),
            [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });

  RowAccumulator acc;
  acc.cover = scratch.cover.data();
  acc.area = scratch.area.data();
  acc.clipX0 = clip.x0;
  acc.clipX1 = clip.x1;

  std::vector<const Edge*> active;
  size_t next = 0;

  for (int32_t y = clip.y0; y < clip.y1; ++y) {
    const float rowTop = static_cast<float>(y);
    const float rowBot = rowTop + 1.0f;

    while (next < edges.size() && edges[next].y0 < rowBot)
      active.push_back(&edges[next++]);
    active.erase(std::remove_if(active.begin(), active.end(),
                                [rowTop](const Edge* e) { return e->y1 <= rowTop; }),
                 active.end());
    if (active.empty()) {
      // Nothing crosses this row. Any edge that started above and ends below
      // would still be active, so an empty active list genuinely means empty.
      continue;
    }

    acc.any = false;
    acc.rightOfClip = false;
    for (const Edge* e : active) {
      const float yTop = std::max(e->y0, rowTop);
      const float yBot = std::min(e->y1, rowBot);
      if (yBot <= yTop) continue;
      const float invDy = 1.0f / (e->y1 - e->y0);
      const float xTop = e->x0 + (yTop - e->y0) * (e->x1 - e->x0) * invDy;
      const float xBot = e->x0 + (yBot - e->y0) * (e->x1 - e->x0) * invDy;
      // Endpoints are stored top-to-bottom so the list can stay sorted by y0;
      // the winding travels separately in `dir` and is applied per deposit.
      accumulateSegment(acc, xTop, yTop, xBot, yBot, e->dir);
    }

    if (!acc.any) continue;

    const int32_t sweepFrom = std::max(acc.minX, clip.x0);
    // Normally the sweep stops at the last cell an edge touched, because a
    // closed path's windings cancel there and everything further right is
    // empty. When an edge fell right of the clip that cancellation never
    // happens inside the clip, so the fill must run to the clip's edge.
    const int32_t sweepTo = acc.rightOfClip
                                ? (clip.x1 - 1)
                                : std::min(acc.maxX, clip.x1 - 1);

    float running = 0.0f;
    int32_t runStart = -1;
    for (int32_t x = sweepFrom; x <= sweepTo; ++x) {
      const size_t i = static_cast<size_t>(x - clip.x0);
      const float cov = applyFillRule(running + acc.area[i], rule);
      running += acc.cover[i];
      scratch.coverage[i] = cov;
      if (cov > kCoverageEpsilon) {
        if (runStart < 0) runStart = x;
      } else if (runStart >= 0) {
        emit(y, runStart, x, &scratch.coverage[static_cast<size_t>(runStart - clip.x0)]);
        runStart = -1;
      }
    }
    if (runStart >= 0)
      emit(y, runStart, sweepTo + 1,
           &scratch.coverage[static_cast<size_t>(runStart - clip.x0)]);

    // Clear only what this row touched. Cells beyond `maxX` that the
    // `rightOfClip` sweep read were never written and are already zero.
    const int32_t clearTo = std::min(acc.maxX, clip.x1 - 1);
    for (int32_t x = sweepFrom; x <= clearTo; ++x) {
      const size_t i = static_cast<size_t>(x - clip.x0);
      acc.cover[i] = 0.0f;
      acc.area[i] = 0.0f;
    }
  }
}

void rasterizePath(const Path& path, float tolerancePx, const RasterClip& clip,
                   PathRasterScratch& scratch, const SpanFn& emit) {
  rasterizeContours(flattenPath(path, tolerancePx), path.rule, clip, scratch, emit);
}

RasterClip clipForPath(const Path& path, int32_t width, int32_t height) noexcept {
  RasterClip c;
  const PathBounds b = pathTightBounds(path);
  if (!b.valid) return c;
  c.x0 = std::max<int32_t>(0, floorInt(b.minX));
  c.y0 = std::max<int32_t>(0, floorInt(b.minY));
  c.x1 = std::min<int32_t>(width, static_cast<int32_t>(std::ceil(b.maxX)));
  c.y1 = std::min<int32_t>(height, static_cast<int32_t>(std::ceil(b.maxY)));
  if (c.x1 <= c.x0 || c.y1 <= c.y0) return RasterClip{};
  return c;
}

}  // namespace np
