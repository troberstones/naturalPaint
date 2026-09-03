#include "app/PenTool.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>

#include "app/AppState.hpp"  // the real `enum class Tool`, opaque in the header
#include "core/PathFlatten.hpp"

namespace np {

bool toolEditsPath(Tool t) noexcept { return t == Tool::Pen || t == Tool::Curve; }

bool operator==(const ComponentRef& a, const ComponentRef& b) noexcept {
  return a.shapeId == b.shapeId && a.subPath == b.subPath && a.anchor == b.anchor &&
         a.part == b.part;
}

bool operator<(const ComponentRef& a, const ComponentRef& b) noexcept {
  const auto key = [](const ComponentRef& r) {
    return std::make_tuple(r.shapeId, r.subPath, r.anchor, static_cast<int>(r.part));
  };
  return key(a) < key(b);
}

namespace {

// --- small geometry helpers, private to this file -------------------------

float distance(PathPoint a, PathPoint b) noexcept {
  return std::hypot(a.x - b.x, a.y - b.y);
}

bool rectContains(PathBounds rect, PathPoint p) noexcept {
  return rect.valid && p.x >= rect.minX && p.x <= rect.maxX && p.y >= rect.minY &&
         p.y <= rect.maxY;
}

bool rectsOverlap(PathBounds a, PathBounds b) noexcept {
  if (!a.valid || !b.valid) return false;
  return a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY;
}

PathBounds boundsUnion(PathBounds a, PathBounds b) noexcept {
  if (!a.valid) return b;
  if (!b.valid) return a;
  PathBounds r;
  r.valid = true;
  r.minX = std::min(a.minX, b.minX);
  r.minY = std::min(a.minY, b.minY);
  r.maxX = std::max(a.maxX, b.maxX);
  r.maxY = std::max(a.maxY, b.maxY);
  return r;
}

const VectorShape* findShape(const std::vector<VectorShape>& shapes, uint64_t id) noexcept {
  for (const VectorShape& s : shapes)
    if (s.id == id) return &s;
  return nullptr;
}

VectorShape* findShapeMut(std::vector<VectorShape>* shapes, uint64_t id) noexcept {
  for (VectorShape& s : *shapes)
    if (s.id == id) return &s;
  return nullptr;
}

// Point-to-closed-or-open-segment distance, for the segment hit-test tier.
float distanceToSegment(PathPoint p, PathPoint a, PathPoint b) noexcept {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float lenSq = dx * dx + dy * dy;
  if (lenSq <= 1e-12f) return distance(p, a);
  float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq;
  t = std::clamp(t, 0.0f, 1.0f);
  const PathPoint proj{a.x + t * dx, a.y + t * dy};
  return distance(p, proj);
}

// The unique anchors (shapeId, subPath, anchor) a component selection
// references, ignoring `part` -- see AnchorPart's own comment on why
// `applyAffineToSelection()` must not scope by which handle was clicked.
struct AnchorKey {
  uint64_t shapeId;
  uint32_t subPath;
  uint32_t anchor;
  bool operator<(const AnchorKey& o) const noexcept {
    return std::make_tuple(shapeId, subPath, anchor) <
           std::make_tuple(o.shapeId, o.subPath, o.anchor);
  }
  bool operator==(const AnchorKey& o) const noexcept {
    return shapeId == o.shapeId && subPath == o.subPath && anchor == o.anchor;
  }
};

std::vector<AnchorKey> uniqueAnchors(const std::vector<ComponentRef>& components) {
  std::vector<AnchorKey> keys;
  keys.reserve(components.size());
  for (const ComponentRef& c : components) keys.push_back({c.shapeId, c.subPath, c.anchor});
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// The pivot a MULTI-shape Shape-mode selection manipulates about.
//
// `VectorShape::pivot` is inherently single-shape (core/VectorShape.hpp: "the
// pivot ... in the shape's own coordinates"), and docs/vector-editing.md
// says nothing about combining several. The rule adopted here, stated
// because the doc does not: a lone selected shape keeps its own stored (or
// centroid) pivot exactly as `shapePivot()` reports it -- "make it stick"
// only means something for a single object -- and a multi-shape selection
// manipulates about the centre of the UNION of the selected shapes' tight
// bounds, the same aggregate-box convention Illustrator's own multi-object
// transform uses, rather than an arbitrary pick among several stored pivots
// that would make which shape is "first" in a std::vector user-visible.
PathPoint pivotForSelection(const std::vector<VectorShape>& shapes,
                             const PathSelection& selection) noexcept {
  if (selection.mode == PathSelectMode::Component)
    return componentPivot(shapes, selection.components);
  if (selection.shapes.size() == 1) {
    if (const VectorShape* s = findShape(shapes, selection.shapes.front())) return shapePivot(*s);
    return PathPoint{0.0f, 0.0f};
  }
  PathBounds u;
  for (uint64_t id : selection.shapes)
    if (const VectorShape* s = findShape(shapes, id))
      u = boundsUnion(u, pathTightBounds(s->path));
  if (!u.valid) return PathPoint{0.0f, 0.0f};
  return PathPoint{(u.minX + u.maxX) * 0.5f, (u.minY + u.maxY) * 0.5f};
}

// The selection's bounds for the gnomon's scale-box corners: Shape mode is
// the union of the selected shapes' tight bounds (the same "manipulator's
// box" precedent `shapePivot()`'s comment cites); Component mode is the
// bounding box of the selected anchors' own `pt` positions, since a bag of
// points has no curve to take a tight bound of.
PathBounds boundsForSelection(const std::vector<VectorShape>& shapes,
                               const PathSelection& selection) noexcept {
  PathBounds b;
  if (selection.mode == PathSelectMode::Shape) {
    for (uint64_t id : selection.shapes)
      if (const VectorShape* s = findShape(shapes, id)) b = boundsUnion(b, pathTightBounds(s->path));
    return b;
  }
  for (const AnchorKey& k : uniqueAnchors(selection.components)) {
    const VectorShape* s = findShape(shapes, k.shapeId);
    if (!s || k.subPath >= s->path.subpaths.size()) continue;
    const SubPath& sub = s->path.subpaths[k.subPath];
    if (k.anchor >= sub.anchors.size()) continue;
    const PathPoint p = sub.anchors[k.anchor].pt;
    if (!b.valid) {
      b = PathBounds{true, p.x, p.y, p.x, p.y};
    } else {
      b.minX = std::min(b.minX, p.x);
      b.minY = std::min(b.minY, p.y);
      b.maxX = std::max(b.maxX, p.x);
      b.maxY = std::max(b.maxY, p.y);
    }
  }
  return b;
}

bool selectionIsEmpty(const PathSelection& selection) noexcept {
  return selection.mode == PathSelectMode::Shape ? selection.shapes.empty()
                                                  : selection.components.empty();
}

}  // namespace

// --- selection combine (section 4) -----------------------------------------

void combineShapeSelection(std::vector<uint64_t>* current, const std::vector<uint64_t>& incoming,
                            SelectionCombine how) {
  std::vector<uint64_t> a = *current;
  std::sort(a.begin(), a.end());
  a.erase(std::unique(a.begin(), a.end()), a.end());
  std::vector<uint64_t> b = incoming;
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());

  std::vector<uint64_t> result;
  switch (how) {
    case SelectionCombine::Replace:
      result = std::move(b);
      break;
    case SelectionCombine::Add:
      std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
    case SelectionCombine::Subtract:
      std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
    case SelectionCombine::Intersect:
      std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
  }
  *current = std::move(result);
}

void combineComponentSelection(std::vector<ComponentRef>* current,
                                const std::vector<ComponentRef>& incoming, SelectionCombine how) {
  std::vector<ComponentRef> a = *current;
  std::sort(a.begin(), a.end());
  a.erase(std::unique(a.begin(), a.end()), a.end());
  std::vector<ComponentRef> b = incoming;
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());

  std::vector<ComponentRef> result;
  switch (how) {
    case SelectionCombine::Replace:
      result = std::move(b);
      break;
    case SelectionCombine::Add:
      std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
    case SelectionCombine::Subtract:
      std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
    case SelectionCombine::Intersect:
      std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
      break;
  }
  *current = std::move(result);
}

// --- pivots (section 6) -----------------------------------------------------

PathPoint shapePivot(const VectorShape& shape) noexcept {
  if (shape.pivot) return *shape.pivot;
  const PathBounds b = pathTightBounds(shape.path);
  if (!b.valid) return PathPoint{0.0f, 0.0f};
  return PathPoint{(b.minX + b.maxX) * 0.5f, (b.minY + b.maxY) * 0.5f};
}

PathPoint componentPivot(const std::vector<VectorShape>& shapes,
                          const std::vector<ComponentRef>& selection) noexcept {
  const std::vector<AnchorKey> keys = uniqueAnchors(selection);
  if (keys.empty()) return PathPoint{0.0f, 0.0f};
  double sx = 0.0, sy = 0.0;
  size_t n = 0;
  for (const AnchorKey& k : keys) {
    const VectorShape* s = findShape(shapes, k.shapeId);
    if (!s || k.subPath >= s->path.subpaths.size()) continue;
    const SubPath& sub = s->path.subpaths[k.subPath];
    if (k.anchor >= sub.anchors.size()) continue;
    sx += sub.anchors[k.anchor].pt.x;
    sy += sub.anchors[k.anchor].pt.y;
    ++n;
  }
  if (n == 0) return PathPoint{0.0f, 0.0f};
  return PathPoint{static_cast<float>(sx / static_cast<double>(n)),
                    static_cast<float>(sy / static_cast<double>(n))};
}

void setShapePivot(VectorShape* shape, PathPoint to) noexcept { shape->pivot = to; }

// --- the manipulator (section 7) --------------------------------------------

void applyAffineToSelection(std::vector<VectorShape>* shapes, const PathSelection& selection,
                             const Mat3& affine) {
  const auto mapPt = [&](PathPoint p) {
    const Point2 mapped = mat3MapPoint(affine, Point2{p.x, p.y});
    return PathPoint{mapped.x, mapped.y};
  };

  if (selection.mode == PathSelectMode::Shape) {
    for (uint64_t id : selection.shapes) {
      VectorShape* s = findShapeMut(shapes, id);
      if (!s) continue;
      for (SubPath& sub : s->path.subpaths) {
        for (Anchor& a : sub.anchors) {
          a.pt = mapPt(a.pt);
          a.in = mapPt(a.in);
          a.out = mapPt(a.out);
        }
      }
      // A `nullopt` pivot is left alone -- see this function's header
      // comment: the centroid it stands for is recomputed from the
      // now-moved geometry the next time anyone asks.
      if (s->pivot) s->pivot = mapPt(*s->pivot);
    }
    return;
  }

  for (const AnchorKey& k : uniqueAnchors(selection.components)) {
    VectorShape* s = findShapeMut(shapes, k.shapeId);
    if (!s || k.subPath >= s->path.subpaths.size()) continue;
    SubPath& sub = s->path.subpaths[k.subPath];
    if (k.anchor >= sub.anchors.size()) continue;
    Anchor& a = sub.anchors[k.anchor];
    a.pt = mapPt(a.pt);
    a.in = mapPt(a.in);
    a.out = mapPt(a.out);
    // Deliberately no `s->pivot` write here: component mode never touches a
    // shape's stored pivot (section 7).
  }
}

// --- the gnomon and hit testing (section 5) ---------------------------------

GnomonHandlePositions gnomonHandlePositions(const std::vector<VectorShape>& shapes,
                                             const PathSelection& selection,
                                             float reachPx) noexcept {
  GnomonHandlePositions g;
  if (selectionIsEmpty(selection)) return g;

  const PathPoint pivot = pivotForSelection(shapes, selection);
  const PathBounds bounds = boundsForSelection(shapes, selection);

  g.valid = true;
  g.center = pivot;
  g.axisXTip = PathPoint{pivot.x + reachPx, pivot.y};
  g.axisYTip = PathPoint{pivot.x, pivot.y + reachPx};

  // The scale-box corners sit at the selection bounds when they exist, and
  // fall back to a small box around the pivot for a bounds-less selection
  // (e.g. a single anchor, whose bounding box is a point) so the gnomon
  // still has four distinguishable corners to test against.
  float minX = pivot.x - reachPx * 0.5f, minY = pivot.y - reachPx * 0.5f;
  float maxX = pivot.x + reachPx * 0.5f, maxY = pivot.y + reachPx * 0.5f;
  if (bounds.valid) {
    minX = bounds.minX;
    minY = bounds.minY;
    maxX = bounds.maxX;
    maxY = bounds.maxY;
  }
  g.corners[0] = PathPoint{minX, minY};
  g.corners[1] = PathPoint{maxX, minY};
  g.corners[2] = PathPoint{maxX, maxY};
  g.corners[3] = PathPoint{minX, maxY};

  const float halfDiagonal =
      0.5f * std::hypot(static_cast<double>(maxX - minX), static_cast<double>(maxY - minY));
  g.rotateRingRadius = std::max(reachPx, static_cast<float>(halfDiagonal)) + reachPx * 0.5f;
  return g;
}

PathHit hitTestPath(const std::vector<VectorShape>& shapes, const PathSelection& selection,
                     PathPoint at, float pickRadiusPx, bool gnomonSuppressed,
                     bool pivotMoveModeActive, float gnomonReachPx) {
  PathHit hit;

  // Tier 1: the gnomon -- unless Alt/Option is suppressing the whole thing.
  if (!gnomonSuppressed) {
    const GnomonHandlePositions g = gnomonHandlePositions(shapes, selection, gnomonReachPx);
    if (g.valid) {
      float best = pickRadiusPx;
      bool found = false;
      const auto consider = [&](PathPoint p) {
        const float d = distance(at, p);
        if (d <= best) {
          best = d;
          found = true;
        }
      };
      consider(g.center);
      consider(g.axisXTip);
      consider(g.axisYTip);
      for (const PathPoint& c : g.corners) consider(c);
      const float ringDist = std::abs(distance(at, g.center) - g.rotateRingRadius);
      if (ringDist <= best) found = true;
      if (found) {
        hit.kind = PathHitKind::GnomonHandle;
        return hit;
      }
    }
  }

  // Tier 2: the pivot marker, only while pivot-move mode is active.
  if (pivotMoveModeActive && !selectionIsEmpty(selection)) {
    const PathPoint pivot = pivotForSelection(shapes, selection);
    if (distance(at, pivot) <= pickRadiusPx) {
      hit.kind = PathHitKind::PivotMarker;
      return hit;
    }
  }

  // Tier 3: anchor points, over every shape (not just the selected ones --
  // clicking an unselected anchor is how a selection is made in the first
  // place).
  {
    float best = pickRadiusPx;
    bool found = false;
    PathHit candidate;
    for (const VectorShape& s : shapes) {
      for (uint32_t si = 0; si < s.path.subpaths.size(); ++si) {
        const SubPath& sub = s.path.subpaths[si];
        for (uint32_t ai = 0; ai < sub.anchors.size(); ++ai) {
          const float d = distance(at, sub.anchors[ai].pt);
          if (d <= best) {
            best = d;
            found = true;
            candidate.kind = PathHitKind::Anchor;
            candidate.shapeId = s.id;
            candidate.component = ComponentRef{s.id, si, ai, AnchorPart::Point};
          }
        }
      }
    }
    if (found) return candidate;
  }

  // Tier 4: tangent handles, only for anchors already in the component
  // selection (section 3: they are only DRAWN for selected anchors, so only
  // those are reachable).
  if (selection.mode == PathSelectMode::Component) {
    float best = pickRadiusPx;
    bool found = false;
    PathHit candidate;
    for (const AnchorKey& k : uniqueAnchors(selection.components)) {
      const VectorShape* s = findShape(shapes, k.shapeId);
      if (!s || k.subPath >= s->path.subpaths.size()) continue;
      const SubPath& sub = s->path.subpaths[k.subPath];
      if (k.anchor >= sub.anchors.size()) continue;
      const Anchor& a = sub.anchors[k.anchor];
      const float dIn = distance(at, a.in);
      if (dIn <= best) {
        best = dIn;
        found = true;
        candidate.kind = PathHitKind::Tangent;
        candidate.shapeId = k.shapeId;
        candidate.component = ComponentRef{k.shapeId, k.subPath, k.anchor, AnchorPart::InHandle};
      }
      const float dOut = distance(at, a.out);
      if (dOut <= best) {
        best = dOut;
        found = true;
        candidate.kind = PathHitKind::Tangent;
        candidate.shapeId = k.shapeId;
        candidate.component = ComponentRef{k.shapeId, k.subPath, k.anchor, AnchorPart::OutHandle};
      }
    }
    if (found) return candidate;
  }

  // Tier 5: path segments. Flattened at a fraction of the pick radius, so the
  // polyline's own approximation error stays well under the tolerance being
  // tested against.
  {
    const float tolerance = std::max(pickRadiusPx * 0.25f, 0.01f);
    float best = pickRadiusPx;
    bool found = false;
    uint64_t bestShapeId = 0;
    for (const VectorShape& s : shapes) {
      const std::vector<FlatContour> contours = flattenPath(s.path, tolerance);
      for (const FlatContour& c : contours) {
        const size_t n = c.points.size();
        if (n < 2) continue;
        const size_t edges = c.closed ? n : n - 1;
        for (size_t i = 0; i < edges; ++i) {
          const PathPoint& p0 = c.points[i];
          const PathPoint& p1 = c.points[(i + 1) % n];
          const float d = distanceToSegment(at, p0, p1);
          if (d <= best) {
            best = d;
            found = true;
            bestShapeId = s.id;
          }
        }
      }
    }
    if (found) {
      hit.kind = PathHitKind::Segment;
      hit.shapeId = bestShapeId;
      return hit;
    }
  }

  // Tier 6: nothing -- empty canvas, start a marquee.
  return hit;
}

std::vector<ComponentRef> componentsInRect(const std::vector<VectorShape>& shapes,
                                            PathBounds rect) {
  std::vector<ComponentRef> result;
  for (const VectorShape& s : shapes) {
    for (uint32_t si = 0; si < s.path.subpaths.size(); ++si) {
      const SubPath& sub = s.path.subpaths[si];
      for (uint32_t ai = 0; ai < sub.anchors.size(); ++ai) {
        if (rectContains(rect, sub.anchors[ai].pt))
          result.push_back(ComponentRef{s.id, si, ai, AnchorPart::Point});
      }
    }
  }
  return result;
}

std::vector<uint64_t> shapesIntersectingRect(const std::vector<VectorShape>& shapes,
                                              PathBounds rect) {
  std::vector<uint64_t> result;
  for (const VectorShape& s : shapes) {
    if (rectsOverlap(rect, pathControlBounds(s.path))) result.push_back(s.id);
  }
  return result;
}


// ==========================================================================
// The interactive session (app/PenTool.hpp's four transitions)
// ==========================================================================
//
// `ui/` calls only these; it never assigns to a `PathEditState` field. Keeping
// every transition in one place is what makes the state machine reviewable --
// and it is the rule a sibling tool's shared drag flag broke, silently, for
// its whole history.

namespace {

bool componentIsSelected(const std::vector<ComponentRef>& selection,
                         const ComponentRef& ref) {
  for (const ComponentRef& c : selection)
    if (c.shapeId == ref.shapeId && c.subPath == ref.subPath && c.anchor == ref.anchor)
      return true;
  return false;
}

bool shapeIsSelected(const std::vector<uint64_t>& selection, uint64_t id) {
  return std::find(selection.begin(), selection.end(), id) != selection.end();
}

// Every anchor of one shape, as components. Used when a click lands on a
// segment in Component mode: Photoshop's Direct Selection shows a path's
// anchors when you click its outline, without selecting any of them for
// dragging, and this is the closest honest equivalent.
std::vector<ComponentRef> allAnchorsOf(const std::vector<VectorShape>& shapes,
                                       uint64_t shapeId) {
  std::vector<ComponentRef> out;
  for (const VectorShape& s : shapes) {
    if (s.id != shapeId) continue;
    for (size_t sp = 0; sp < s.path.subpaths.size(); ++sp)
      for (size_t a = 0; a < s.path.subpaths[sp].anchors.size(); ++a)
        out.push_back(ComponentRef{shapeId, static_cast<uint32_t>(sp),
                                   static_cast<uint32_t>(a), AnchorPart::Point});
  }
  return out;
}

}  // namespace

void pathEditCancel(PathEditState* state) noexcept {
  if (state == nullptr) return;
  state->drag = PathDragKind::None;
  state->dragComponent = ComponentRef{};
  state->geometryEditOpened = false;
  // Released rather than merely cleared: this holds a full copy of the layer's
  // geometry, and a cancelled drag has no reason to keep a document's worth of
  // anchors alive until the next one.
  state->shapesAtDragStart.clear();
  state->shapesAtDragStart.shrink_to_fit();
}

void pathEditRefreshPivot(PathEditState* state, const std::vector<VectorShape>& shapes) {
  if (state == nullptr) return;
  // The user-placed flag is the whole point (docs/vector-editing.md section 1):
  // without it, placing a pivot and then extending the selection by one anchor
  // silently throws the placement away.
  if (state->componentPivotIsUserPlaced) return;
  state->componentPivot = componentPivot(shapes, state->selection.components);
}

void pathEditSetSelectMode(PathEditState* state, PathSelectMode mode,
                           const std::vector<VectorShape>& shapes) {
  if (state == nullptr) return;
  if (state->selection.mode == mode) return;

  // Abandon the live gesture BEFORE the selection changes underneath it: a
  // Manipulator drag holds `shapesAtDragStart` against the old selection, and
  // letting the next `pathEditUpdate()` apply that affine to a selection the
  // user did not have at pen-down is exactly the class of cross-gesture
  // contamination `PathEditState`'s single-writer rule exists to prevent.
  pathEditCancel(state);

  if (mode == PathSelectMode::Component) {
    std::vector<ComponentRef> carried;
    for (uint64_t id : state->selection.shapes) {
      std::vector<ComponentRef> anchors = allAnchorsOf(shapes, id);
      carried.insert(carried.end(), anchors.begin(), anchors.end());
    }
    state->selection.shapes.clear();
    state->selection.components.clear();
    combineComponentSelection(&state->selection.components, carried,
                              SelectionCombine::Replace);
  } else {
    std::vector<uint64_t> carried;
    carried.reserve(state->selection.components.size());
    for (const ComponentRef& c : state->selection.components) carried.push_back(c.shapeId);
    state->selection.components.clear();
    state->selection.shapes.clear();
    // `Replace` de-duplicates and sorts, which is what turns "one id per
    // selected anchor" into "one id per shape" without a second pass here.
    combineShapeSelection(&state->selection.shapes, carried, SelectionCombine::Replace);
  }

  state->selection.mode = mode;
  state->componentPivotIsUserPlaced = false;
  pathEditRefreshPivot(state, shapes);
}

bool pathEditBegin(PathEditState* state, const std::vector<VectorShape>& shapes,
                   PathPoint at, float pickRadiusPx, bool gnomonSuppressed,
                   SelectionCombine how, uint64_t documentId) {
  if (state == nullptr) return false;
  state->documentId = documentId;
  state->dragStart = at;
  state->dragNow = at;
  state->dragComponent = ComponentRef{};
  state->geometryEditOpened = false;

  const PathHit hit =
      hitTestPath(shapes, state->selection, at, pickRadiusPx, gnomonSuppressed);

  const bool component = state->selection.mode == PathSelectMode::Component;

  switch (hit.kind) {
    case PathHitKind::Anchor:
    case PathHitKind::Tangent: {
      if (component) {
        // **Dragging an already-selected anchor moves the whole selection;
        // dragging an unselected one replaces the selection with it.** That is
        // every direct-manipulation editor's rule, and getting it backwards
        // makes a multi-anchor drag impossible: the first press would collapse
        // the selection the user just built.
        if (!componentIsSelected(state->selection.components, hit.component)) {
          std::vector<ComponentRef> one{hit.component};
          combineComponentSelection(&state->selection.components, one, how);
          pathEditRefreshPivot(state, shapes);
        }
      } else {
        if (!shapeIsSelected(state->selection.shapes, hit.shapeId)) {
          std::vector<uint64_t> one{hit.shapeId};
          combineShapeSelection(&state->selection.shapes, one, how);
        }
      }
      state->dragComponent = hit.component;
      state->drag = (hit.kind == PathHitKind::Anchor) ? PathDragKind::AnchorDrag
                                                      : PathDragKind::TangentDrag;
      state->shapesAtDragStart = shapes;
      return true;
    }

    case PathHitKind::Segment: {
      if (component) {
        // Show the path's anchors without selecting any for dragging, then
        // start no drag -- a segment is not a handle.
        std::vector<ComponentRef> anchors = allAnchorsOf(shapes, hit.shapeId);
        combineComponentSelection(&state->selection.components, anchors, how);
        pathEditRefreshPivot(state, shapes);
        state->drag = PathDragKind::None;
        return false;
      }
      if (!shapeIsSelected(state->selection.shapes, hit.shapeId)) {
        std::vector<uint64_t> one{hit.shapeId};
        combineShapeSelection(&state->selection.shapes, one, how);
      }
      // A whole-shape move is the manipulator's translate, reached without
      // touching the gnomon -- the same way Move commits on pen-up rather than
      // on Return.
      state->drag = PathDragKind::Manipulator;
      state->shapesAtDragStart = shapes;
      return true;
    }

    case PathHitKind::PivotMarker: {
      state->drag = PathDragKind::PivotMove;
      state->shapesAtDragStart = shapes;
      // Editing no geometry, so this opens no undo entry until it actually
      // moves a pivot -- which IS document data (core/VectorShape.hpp).
      return true;
    }

    case PathHitKind::GnomonHandle: {
      state->drag = PathDragKind::Manipulator;
      state->shapesAtDragStart = shapes;
      return true;
    }

    case PathHitKind::None: {
      // Empty canvas: a marquee, and the modifier decides whether it replaces
      // or extends. A bare click on nothing clears the selection, which is
      // what every editor does and what makes "click away to deselect" work.
      if (how == SelectionCombine::Replace) {
        state->selection.shapes.clear();
        state->selection.components.clear();
        pathEditRefreshPivot(state, shapes);
      }
      state->drag = PathDragKind::Marquee;
      return false;
    }
  }
  return false;
}

PathEditChange pathEditUpdate(PathEditState* state, std::vector<VectorShape>* shapes,
                              PathPoint at) {
  if (state == nullptr || shapes == nullptr) return PathEditChange::None;
  if (state->drag == PathDragKind::None) return PathEditChange::None;
  state->dragNow = at;

  const float dx = at.x - state->dragStart.x;
  const float dy = at.y - state->dragStart.y;
  // A held-still pointer is the common case during a drag, not the rare one.
  // Doing nothing here keeps it out of the undo history entirely.
  if (dx == 0.0f && dy == 0.0f) return PathEditChange::None;

  switch (state->drag) {
    case PathDragKind::Marquee:
      // Selection is resolved at pen-up, against the finished rectangle.
      return PathEditChange::None;

    case PathDragKind::PivotMove: {
      // Editing no geometry: the pivot moves and every anchor stays put. In
      // Component mode the transient pivot moves and nothing in the document
      // changes at all, so there is no edit to record.
      if (state->selection.mode == PathSelectMode::Component) {
        state->componentPivot = at;
        state->componentPivotIsUserPlaced = true;
        return PathEditChange::None;
      }
      bool moved = false;
      for (VectorShape& s : *shapes) {
        if (!shapeIsSelected(state->selection.shapes, s.id)) continue;
        setShapePivot(&s, at);
        moved = true;
      }
      if (!moved) return PathEditChange::None;
      const bool first = !state->geometryEditOpened;
      state->geometryEditOpened = true;
      return first ? PathEditChange::EditBegan : PathEditChange::EditContinued;
    }

    case PathDragKind::AnchorDrag:
    case PathDragKind::TangentDrag:
    case PathDragKind::Manipulator:
    case PathDragKind::PenExtend: {
      // **Rebuilt from the pen-down snapshot, never accumulated.** One affine
      // against the original geometry -- so the result depends on where the
      // pointer IS, not on how many frames it took to get there.
      *shapes = state->shapesAtDragStart;

      if (state->drag == PathDragKind::TangentDrag) {
        // One handle, alone: this is the gesture that BREAKS a smooth anchor,
        // so it deliberately does not go through applyAffineToSelection().
        for (VectorShape& s : *shapes) {
          if (s.id != state->dragComponent.shapeId) continue;
          if (state->dragComponent.subPath >= s.path.subpaths.size()) continue;
          SubPath& sub = s.path.subpaths[state->dragComponent.subPath];
          if (state->dragComponent.anchor >= sub.anchors.size()) continue;
          Anchor& a = sub.anchors[state->dragComponent.anchor];
          if (state->dragComponent.part == AnchorPart::InHandle) {
            a.in = at;
          } else if (state->dragComponent.part == AnchorPart::OutHandle) {
            a.out = at;
          }
        }
      } else {
        applyAffineToSelection(shapes, state->selection, transformTranslate(dx, dy));
      }

      const bool first = !state->geometryEditOpened;
      state->geometryEditOpened = true;
      return first ? PathEditChange::EditBegan : PathEditChange::EditContinued;
    }

    case PathDragKind::None:
      return PathEditChange::None;
  }
  return PathEditChange::None;
}

void pathEditEnd(PathEditState* state, const std::vector<VectorShape>& shapes) {
  if (state == nullptr) return;

  if (state->drag == PathDragKind::Marquee) {
    PathBounds box;
    box.valid = true;
    box.minX = std::min(state->dragStart.x, state->dragNow.x);
    box.maxX = std::max(state->dragStart.x, state->dragNow.x);
    box.minY = std::min(state->dragStart.y, state->dragNow.y);
    box.maxY = std::max(state->dragStart.y, state->dragNow.y);
    if (state->selection.mode == PathSelectMode::Component) {
      std::vector<ComponentRef> hits = componentsInRect(shapes, box);
      combineComponentSelection(&state->selection.components, hits,
                                SelectionCombine::Add);
      pathEditRefreshPivot(state, shapes);
    } else {
      std::vector<uint64_t> hits = shapesIntersectingRect(shapes, box);
      combineShapeSelection(&state->selection.shapes, hits, SelectionCombine::Add);
    }
  } else {
    pathEditRefreshPivot(state, shapes);
  }

  pathEditCancel(state);
}

}  // namespace np
