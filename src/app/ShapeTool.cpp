#include "app/ShapeTool.hpp"

#include <algorithm>
#include <cmath>

#include "app/AppState.hpp"  // the real `enum class Tool`, opaque in the header
#include "io/SvgPath.hpp"    // svgRectPath/svgEllipsePath/svgLinePath -- §3

namespace np {

const ShapeKindRow kShapeKinds[kShapeKindCount] = {
    {ShapeKind::Rectangle, "Rectangle", "Drag a rectangle from corner to corner."},
    {ShapeKind::Ellipse, "Ellipse", "Drag an ellipse within a bounding box."},
    {ShapeKind::RoundedRect, "Rounded Rectangle",
     "Drag a rectangle with a corner radius set below."},
    {ShapeKind::Polygon, "Polygon", "Drag a regular polygon with the side count set below."},
    {ShapeKind::Line, "Line", "Drag a straight line from one point to another."},
};

bool toolCreatesShapes(Tool t) noexcept { return t == Tool::Shape; }

namespace {

// The corner-drag bounding box, shared by every kind except Line (§3's own
// account of why Line is not built from one).
//
// `shiftConstrain` forces a square box -- the one modifier meaning that reads
// the same across Rectangle (a square), Ellipse (a circle) and Polygon (a
// polygon inscribed in a circle rather than an ellipse): all three become
// "equal width and height" from the identical two lines here, which is what
// makes Shift's meaning one rule instead of three.
struct BBox {
  float minX, minY, maxX, maxY;
};

BBox dragBBox(PathPoint p0, PathPoint p1, bool shiftConstrain, bool fromCenter) noexcept {
  float dx = p1.x - p0.x;
  float dy = p1.y - p0.y;
  if (shiftConstrain) {
    const float side = std::max(std::fabs(dx), std::fabs(dy));
    // `dx == 0.0f` (a drag that has moved on only one axis so far) has no
    // sign to preserve; +1 picks a direction rather than leaving the square
    // undefined for that one frame.
    dx = std::copysign(side, dx != 0.0f ? dx : 1.0f);
    dy = std::copysign(side, dy != 0.0f ? dy : 1.0f);
  }
  if (fromCenter) {
    const float hx = std::fabs(dx);
    const float hy = std::fabs(dy);
    return BBox{p0.x - hx, p0.y - hy, p0.x + hx, p0.y + hy};
  }
  const float ex = p0.x + dx;
  const float ey = p0.y + dy;
  return BBox{std::min(p0.x, ex), std::min(p0.y, ey), std::max(p0.x, ex), std::max(p0.y, ey)};
}

// A regular-ish polygon inscribed in `box`: `sides` vertices spaced evenly by
// angle, at `(rx * cos, ry * sin)` from the box's centre. Equal rx/ry (Shift
// held, via `dragBBox()` above) makes every vertex land on one circle, which
// is what "regular polygon" means; unequal rx/ry inscribes the same vertex
// angles in an ellipse instead, which is a deliberate consequence of sharing
// `dragBBox()` with the other three kinds rather than a second geometry this
// file would otherwise need for the non-Shift case.
//
// The first vertex points straight up (`-pi/2`), matching every other
// polygon tool's convention (Illustrator, Figma) so a pentagon drawn here
// looks like the pentagon a user already expects.
//
// Degenerates to an empty `Path` for a collapsed box, the same rule
// `svgRectPath()`/`svgEllipsePath()` state for their own non-positive
// dimensions -- so a one-axis drag (rx or ry at zero) produces nothing here
// too, rather than a polygon flattened onto a line no rasteriser can fill.
Path polygonPath(BBox box, int sides) noexcept {
  const int n = std::max(3, sides);
  const float cx = (box.minX + box.maxX) * 0.5f;
  const float cy = (box.minY + box.maxY) * 0.5f;
  const float rx = (box.maxX - box.minX) * 0.5f;
  const float ry = (box.maxY - box.minY) * 0.5f;
  if (!(rx > 0.0f) || !(ry > 0.0f)) return Path{};

  constexpr float kPi = 3.14159265358979323846f;
  Path path;
  SubPath sub;
  sub.closed = true;
  sub.anchors.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float angle = -kPi * 0.5f + static_cast<float>(i) * (2.0f * kPi / static_cast<float>(n));
    Anchor a;
    // A straight anchor, `svgPolyPath()`'s own `straightAnchor()` shape: all
    // three points coincide, which is what "no handle" means for a segment
    // between two polygon vertices (core/Path.hpp §2).
    a.pt = a.in = a.out = PathPoint{cx + rx * std::cos(angle), cy + ry * std::sin(angle)};
    sub.anchors.push_back(a);
  }
  path.subpaths.push_back(std::move(sub));
  return path;
}

// Line's own two endpoints. Not built from `dragBBox()`: a line has no
// width/height to equalise, so Shift's meaning here is "snap the drag angle
// to the nearest 45 degrees" rather than "make the box square", and Option's
// is "extend from the midpoint" rather than "extend from a corner".
void lineEndpoints(PathPoint p0, PathPoint p1, bool shiftConstrain, bool fromCenter,
                   PathPoint* start, PathPoint* end) noexcept {
  float dx = p1.x - p0.x;
  float dy = p1.y - p0.y;
  if (shiftConstrain) {
    constexpr float kPi = 3.14159265358979323846f;
    const float len = std::hypot(dx, dy);
    const float quarter = kPi * 0.25f;
    const float angle = std::round(std::atan2(dy, dx) / quarter) * quarter;
    dx = len * std::cos(angle);
    dy = len * std::sin(angle);
  }
  *end = PathPoint{p0.x + dx, p0.y + dy};
  *start = fromCenter ? PathPoint{p0.x - dx, p0.y - dy} : p0;
}

}  // namespace

VectorShape shapeToolGeometry(const ShapeToolState& tool, PathPoint p0, PathPoint p1,
                              bool shiftConstrain, bool fromCenter) noexcept {
  VectorShape s;

  // A plain click: nothing to draw. Exact equality, `pathEditUpdate()`'s own
  // "held-still pointer" test (`app/PenTool.cpp`) -- the drag start and the
  // release are the identical document point.
  if (p0.x == p1.x && p0.y == p1.y) return s;

  if (tool.kind == ShapeKind::Line) {
    PathPoint start{}, end{};
    lineEndpoints(p0, p1, shiftConstrain, fromCenter, &start, &end);
    s.path = svgLinePath(start.x, start.y, end.x, end.y);
    return s;
  }

  const BBox box = dragBBox(p0, p1, shiftConstrain, fromCenter);
  const float w = box.maxX - box.minX;
  const float h = box.maxY - box.minY;

  switch (tool.kind) {
    case ShapeKind::Rectangle:
      s.path = svgRectPath(box.minX, box.minY, w, h, 0.0f, 0.0f);
      break;
    case ShapeKind::RoundedRect: {
      const float r = std::max(0.0f, tool.cornerRadius);
      s.path = svgRectPath(box.minX, box.minY, w, h, r, r);
      break;
    }
    case ShapeKind::Ellipse: {
      const float cx = (box.minX + box.maxX) * 0.5f;
      const float cy = (box.minY + box.maxY) * 0.5f;
      s.path = svgEllipsePath(cx, cy, w * 0.5f, h * 0.5f);
      break;
    }
    case ShapeKind::Polygon:
      s.path = polygonPath(box, tool.sides);
      break;
    case ShapeKind::Line:
      break;  // handled above; unreachable
  }
  return s;
}

ShapeCommitResult commitShapeTool(const ShapeToolState& tool, PathPoint p0, PathPoint p1,
                                  bool shiftConstrain, bool fromCenter, const VectorStyle& style,
                                  std::vector<VectorShape>* shapes, uint64_t* nextShapeId,
                                  PathEditState* pathEdit) {
  VectorShape s = shapeToolGeometry(tool, p0, p1, shiftConstrain, fromCenter);
  if (pathIsEmpty(s.path)) return ShapeCommitResult::Empty;

  setVectorStyle(&s, style);
  s.id = (*nextShapeId)++;
  const uint64_t id = s.id;
  shapes->push_back(std::move(s));

  if (pathEdit != nullptr) {
    std::vector<uint64_t> one{id};
    pathEditSelectShapes(pathEdit, one, SelectionCombine::Replace, *shapes);
  }
  return ShapeCommitResult::Committed;
}

}  // namespace np
