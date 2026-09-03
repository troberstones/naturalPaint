#include "core/Path.hpp"

#include <algorithm>
#include <cmath>

namespace np {

size_t subPathSegmentCount(const SubPath& sub) noexcept {
  const size_t n = sub.anchors.size();
  if (n < 2) return 0;
  return sub.closed ? n : n - 1;
}

void subPathSegment(const SubPath& sub, size_t i, PathPoint out[4]) noexcept {
  const size_t n = sub.anchors.size();
  // Wraps only when closed, and `subPathSegmentCount()` has already made the
  // last open segment unreachable -- so the modulo is the closing segment's
  // whole implementation.
  const size_t j = (i + 1) % n;
  out[0] = sub.anchors[i].pt;
  out[1] = sub.anchors[i].out;
  out[2] = sub.anchors[j].in;
  out[3] = sub.anchors[j].pt;
}

bool pathIsEmpty(const Path& path) noexcept {
  for (const SubPath& sub : path.subpaths)
    if (sub.anchors.size() >= 2) return false;
  return true;
}

bool pathIsFinite(const Path& path) noexcept {
  for (const SubPath& sub : path.subpaths) {
    for (const Anchor& a : sub.anchors) {
      // All six coordinates, not just `pt`: a finite anchor with an infinite
      // handle still sends the flattener's subdivision off to infinity, and
      // the handle is the coordinate an SVG `d` string is most likely to
      // carry garbage in (it is the one a human never types).
      if (!std::isfinite(a.pt.x) || !std::isfinite(a.pt.y)) return false;
      if (!std::isfinite(a.in.x) || !std::isfinite(a.in.y)) return false;
      if (!std::isfinite(a.out.x) || !std::isfinite(a.out.y)) return false;
    }
  }
  return true;
}

PathBounds pathControlBounds(const Path& path) noexcept {
  PathBounds b;
  auto include = [&b](PathPoint p) {
    if (!b.valid) {
      b.valid = true;
      b.minX = b.maxX = p.x;
      b.minY = b.maxY = p.y;
      return;
    }
    b.minX = std::min(b.minX, p.x);
    b.maxX = std::max(b.maxX, p.x);
    b.minY = std::min(b.minY, p.y);
    b.maxY = std::max(b.maxY, p.y);
  };

  for (const SubPath& sub : path.subpaths) {
    const size_t n = sub.anchors.size();
    if (n == 0) continue;
    // A lone anchor contributes its own position and nothing else: its handles
    // control no segment (section 3), so including them would report bounds
    // for geometry that is not drawn.
    if (n == 1) {
      include(sub.anchors[0].pt);
      continue;
    }
    // Every anchor's position counts; a handle counts only when it is a
    // control point of a segment that exists. Walking segments rather than
    // anchors is what keeps an open subpath's unused first `in` and last
    // `out` -- which are still stored, deliberately -- out of the answer.
    for (const Anchor& a : sub.anchors) include(a.pt);
    const size_t segs = subPathSegmentCount(sub);
    for (size_t i = 0; i < segs; ++i) {
      PathPoint seg[4];
      subPathSegment(sub, i, seg);
      include(seg[1]);
      include(seg[2]);
    }
  }
  return b;
}

void moveAnchorTo(Anchor& anchor, PathPoint to) noexcept {
  const float dx = to.x - anchor.pt.x;
  const float dy = to.y - anchor.pt.y;
  anchor.pt = to;
  anchor.in.x += dx;
  anchor.in.y += dy;
  anchor.out.x += dx;
  anchor.out.y += dy;
}

}  // namespace np
