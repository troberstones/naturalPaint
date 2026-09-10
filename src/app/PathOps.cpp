#include "app/PathOps.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace np {
namespace {

// ==========================================================================
// Resolving a selection to geometry
// ==========================================================================
//
// Everything below works from two derived views of the selection, never from
// `PathSelection`'s vectors directly:
//
//   `touchedSubPaths()`   which subpaths the selection reaches
//   `touchedAnchors()`    which anchors it reaches
//
// Both are deduplicated and sorted, and both answer for EITHER mode -- Shape
// mode means "every subpath / every anchor of the selected shapes". That is
// what makes CLOSE, REVERSE and SMOOTH work with a shape selected rather than
// refusing and sending the user to the MODE segment for no reason: "smooth
// this path" is a sentence, and it means every knot on it.
//
// The verbs that genuinely cannot be said in Shape mode -- JOIN, INSERT --
// ask for `selection.components` themselves and refuse with
// `WrongSelectMode`. They need to know WHICH anchors, and "all of them" is
// not an answer to that.

// A subpath, addressed the way the verbs need it: by position in the shapes
// vector rather than by shape id, because every mutation below is an
// insert/erase on that vector and an id lookup would have to be redone after
// each one.
struct SubPathRef {
  size_t shape = 0;
  size_t sub = 0;
  bool operator<(const SubPathRef& o) const {
    return shape != o.shape ? shape < o.shape : sub < o.sub;
  }
  bool operator==(const SubPathRef& o) const { return shape == o.shape && sub == o.sub; }
};

struct AnchorAt {
  size_t shape = 0;
  size_t sub = 0;
  size_t anchor = 0;
  bool operator<(const AnchorAt& o) const {
    if (shape != o.shape) return shape < o.shape;
    if (sub != o.sub) return sub < o.sub;
    return anchor < o.anchor;
  }
  bool operator==(const AnchorAt& o) const {
    return shape == o.shape && sub == o.sub && anchor == o.anchor;
  }
};

size_t shapeIndex(const std::vector<VectorShape>& shapes, uint64_t id) {
  for (size_t i = 0; i < shapes.size(); ++i)
    if (shapes[i].id == id) return i;
  return shapes.size();  // "not found", checked by every caller
}

// Selected shapes, as indices, sorted and deduplicated. A selection naming a
// shape that is gone contributes nothing -- the caller learns that from the
// count coming back short, which is what `StaleSelection` is raised on.
std::vector<size_t> selectedShapeIndices(const std::vector<VectorShape>& shapes,
                                         const PathSelection& selection) {
  std::vector<size_t> out;
  if (selection.mode == PathSelectMode::Shape) {
    for (uint64_t id : selection.shapes) {
      const size_t i = shapeIndex(shapes, id);
      if (i < shapes.size()) out.push_back(i);
    }
  } else {
    for (const ComponentRef& c : selection.components) {
      const size_t i = shapeIndex(shapes, c.shapeId);
      if (i < shapes.size()) out.push_back(i);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<SubPathRef> touchedSubPaths(const std::vector<VectorShape>& shapes,
                                        const PathSelection& selection) {
  std::vector<SubPathRef> out;
  if (selection.mode == PathSelectMode::Shape) {
    for (uint64_t id : selection.shapes) {
      const size_t i = shapeIndex(shapes, id);
      if (i >= shapes.size()) continue;
      for (size_t sp = 0; sp < shapes[i].path.subpaths.size(); ++sp)
        out.push_back(SubPathRef{i, sp});
    }
  } else {
    for (const ComponentRef& c : selection.components) {
      const size_t i = shapeIndex(shapes, c.shapeId);
      if (i >= shapes.size()) continue;
      if (c.subPath >= shapes[i].path.subpaths.size()) continue;
      out.push_back(SubPathRef{i, c.subPath});
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// **`part` is deliberately ignored here.** `AnchorPart::Point`, `InHandle`
// and `OutHandle` for one anchor are three ways of naming the same knot, and
// every verb in this file acts on the knot: smoothing "the in-handle" is not
// a different operation from smoothing "the anchor". That is the same
// de-duplication `applyAffineToSelection()` performs for the same reason
// (`app/PenTool.hpp`'s `AnchorPart` comment).
std::vector<AnchorAt> touchedAnchors(const std::vector<VectorShape>& shapes,
                                     const PathSelection& selection) {
  std::vector<AnchorAt> out;
  if (selection.mode == PathSelectMode::Shape) {
    for (uint64_t id : selection.shapes) {
      const size_t i = shapeIndex(shapes, id);
      if (i >= shapes.size()) continue;
      const Path& p = shapes[i].path;
      for (size_t sp = 0; sp < p.subpaths.size(); ++sp)
        for (size_t a = 0; a < p.subpaths[sp].anchors.size(); ++a)
          out.push_back(AnchorAt{i, sp, a});
    }
  } else {
    for (const ComponentRef& c : selection.components) {
      const size_t i = shapeIndex(shapes, c.shapeId);
      if (i >= shapes.size()) continue;
      if (c.subPath >= shapes[i].path.subpaths.size()) continue;
      if (c.anchor >= shapes[i].path.subpaths[c.subPath].anchors.size()) continue;
      out.push_back(AnchorAt{i, c.subPath, c.anchor});
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool selectionIsEmpty(const PathSelection& s) {
  return s.mode == PathSelectMode::Shape ? s.shapes.empty() : s.components.empty();
}

// Two paints are "the same" for the purpose of deciding whether a consumed
// shape's style was actually LOST. Exact float comparison, deliberately: a
// near-miss is a difference the user chose, and rounding two distinct
// colours together to suppress a warning is the wrong direction to err.
bool paintSame(const Paint& a, const Paint& b) {
  if (a.on != b.on) return false;
  if (!a.on) return true;  // two "no paint"s are the same regardless of rgba
  return a.rgba == b.rgba;
}

bool styleSame(const VectorShape& a, const VectorShape& b) {
  return paintSame(a.fill, b.fill) && paintSame(a.stroke, b.stroke) &&
         a.strokeStyle.width == b.strokeStyle.width && a.strokeStyle.cap == b.strokeStyle.cap &&
         a.strokeStyle.join == b.strokeStyle.join &&
         a.strokeStyle.miterLimit == b.strokeStyle.miterLimit &&
         a.strokeStyle.dashes == b.strokeStyle.dashes &&
         a.strokeStyle.dashOffset == b.strokeStyle.dashOffset;
}

// Refit the tangents of anchors that are ALREADY marked smooth, after a
// topology change gave them a neighbour they did not have (a close, a join).
//
// **Only the smooth ones**, which is the difference between this and Curve
// mode's own refit: Curve mode is authoring every anchor as smooth by
// definition, so it refits unconditionally. Here the user's corners are the
// user's, and closing a rectangle must not round it off.
void refitSmoothSeam(SubPath& sub, size_t i) {
  if (i >= sub.anchors.size()) return;
  if (!sub.anchors[i].smooth) return;
  fitAnchorTangent(&sub, i, sub.closed);
}

// ==========================================================================
// The two-anchor verbs' shared precondition
// ==========================================================================
//
// JOIN and INSERT both need exactly two anchors named individually, which
// Shape mode cannot express. Factored out because the ORDER of the three
// checks is the specification: mode before arity before geometry, so a user
// in Shape mode is told to switch modes rather than told they have selected
// the wrong number of anchors -- which would be true and useless.
PathOpRefusal twoAnchorPrecondition(const std::vector<VectorShape>& shapes,
                                    const PathSelection& selection,
                                    AnchorAt* a, AnchorAt* b) {
  if (selectionIsEmpty(selection)) return PathOpRefusal::EmptySelection;
  if (selection.mode != PathSelectMode::Component) return PathOpRefusal::WrongSelectMode;
  const std::vector<AnchorAt> anchors = touchedAnchors(shapes, selection);
  if (anchors.empty()) return PathOpRefusal::StaleSelection;
  if (anchors.size() != 2) return PathOpRefusal::NeedsTwoAnchors;
  *a = anchors[0];
  *b = anchors[1];
  return PathOpRefusal::None;
}

bool isEndpointOfOpen(const SubPath& sub, size_t anchor) {
  if (sub.closed) return false;
  const size_t n = sub.anchors.size();
  if (n == 0) return false;
  return anchor == 0 || anchor == n - 1;
}

}  // namespace

const char* pathOpRefusalText(PathOpRefusal r) noexcept {
  switch (r) {
    case PathOpRefusal::None: return "";
    case PathOpRefusal::EmptySelection:
      return "Nothing is selected. Click a path on the canvas first.";
    case PathOpRefusal::WrongSelectMode:
      return "This needs individual anchors. Switch MODE to COMPONENT and click the "
             "anchors you mean.";
    case PathOpRefusal::NeedsTwoAnchors:
      return "Select exactly two anchors -- the two ends that should meet.";
    case PathOpRefusal::NotAnEndpoint:
      return "Both anchors must be a loose end: the first or last anchor of a path that "
             "is not already closed.";
    case PathOpRefusal::AlreadyClosed: return "That path is already closed.";
    case PathOpRefusal::AlreadyOpen: return "That path is already open.";
    case PathOpRefusal::DegenerateSubPath:
      return "That path has only one anchor, so it has no curve to work on yet.";
    case PathOpRefusal::NotAdjacent:
      return "Select two anchors that are next to each other -- the segment between them "
             "is what gets a new point.";
    case PathOpRefusal::NeedsTwoShapes: return "Select two or more paths to combine.";
    case PathOpRefusal::NotCompound:
      return "That path has only one contour, so there is nothing to release.";
    case PathOpRefusal::StaleSelection:
      return "The selection points at geometry that is no longer there.";
  }
  return "";
}

const char* pathOpEditName(PathOp op) noexcept {
  switch (op) {
    case PathOp::Close: return "close path";
    case PathOp::Open: return "open path";
    case PathOp::Join: return "join paths";
    case PathOp::Reverse: return "reverse path";
    case PathOp::Smooth: return "smooth anchors";
    case PathOp::Corner: return "corner anchors";
    case PathOp::Break: return "break tangents";
    case PathOp::InsertAnchor: return "insert anchor";
    case PathOp::DeleteAnchor: return "delete anchors";
    case PathOp::MakeCompound: return "make compound path";
    case PathOp::ReleaseCompound: return "release compound path";
  }
  return "path edit";
}

PathOpRefusal pathOpCanRun(PathOp op, const std::vector<VectorShape>& shapes,
                           const PathSelection& selection) noexcept {
  if (selectionIsEmpty(selection)) return PathOpRefusal::EmptySelection;

  switch (op) {
    case PathOp::Close:
    case PathOp::Open:
    case PathOp::Reverse: {
      const std::vector<SubPathRef> subs = touchedSubPaths(shapes, selection);
      if (subs.empty()) return PathOpRefusal::StaleSelection;
      // **Refuses only when NO touched subpath can take the verb.** A mixed
      // selection -- two open paths and one already closed -- closes the two
      // and leaves the third, which is what a user dragging a marquee over
      // three paths and pressing CLOSE means. Refusing the whole gesture
      // because one member was already done is the pedantic reading and the
      // annoying one.
      bool anyEligible = false;
      bool anyNonDegenerate = false;
      for (const SubPathRef& r : subs) {
        const SubPath& sub = shapes[r.shape].path.subpaths[r.sub];
        if (sub.anchors.size() < 2) continue;
        anyNonDegenerate = true;
        if (op == PathOp::Close && !sub.closed) anyEligible = true;
        if (op == PathOp::Open && sub.closed) anyEligible = true;
        if (op == PathOp::Reverse) anyEligible = true;
      }
      if (!anyNonDegenerate) return PathOpRefusal::DegenerateSubPath;
      if (!anyEligible)
        return op == PathOp::Close ? PathOpRefusal::AlreadyClosed : PathOpRefusal::AlreadyOpen;
      return PathOpRefusal::None;
    }

    case PathOp::Smooth:
    case PathOp::Corner:
    case PathOp::Break:
    case PathOp::DeleteAnchor: {
      // Anchor-level, but valid in BOTH modes: Shape mode means every anchor
      // of the selected shapes (see `touchedAnchors()`).
      const std::vector<AnchorAt> anchors = touchedAnchors(shapes, selection);
      if (anchors.empty()) return PathOpRefusal::StaleSelection;
      return PathOpRefusal::None;
    }

    case PathOp::Join: {
      AnchorAt a{}, b{};
      const PathOpRefusal pre = twoAnchorPrecondition(shapes, selection, &a, &b);
      if (pre != PathOpRefusal::None) return pre;
      const SubPath& subA = shapes[a.shape].path.subpaths[a.sub];
      const SubPath& subB = shapes[b.shape].path.subpaths[b.sub];
      if (!isEndpointOfOpen(subA, a.anchor) || !isEndpointOfOpen(subB, b.anchor))
        return PathOpRefusal::NotAnEndpoint;
      // Same subpath: this is a close, and it needs two DISTINCT ends. A
      // one-anchor subpath's only anchor is both its first and its last, so
      // `isEndpointOfOpen()` passes twice for it -- but `touchedAnchors()`
      // has already deduplicated those into one entry, so reaching here with
      // `a.anchor == b.anchor` is impossible and a two-anchor subpath is the
      // smallest thing that can close.
      if (a.shape == b.shape && a.sub == b.sub && subA.anchors.size() < 2)
        return PathOpRefusal::DegenerateSubPath;
      return PathOpRefusal::None;
    }

    case PathOp::InsertAnchor: {
      AnchorAt a{}, b{};
      const PathOpRefusal pre = twoAnchorPrecondition(shapes, selection, &a, &b);
      if (pre != PathOpRefusal::None) return pre;
      if (a.shape != b.shape || a.sub != b.sub) return PathOpRefusal::NotAdjacent;
      const SubPath& sub = shapes[a.shape].path.subpaths[a.sub];
      const size_t n = sub.anchors.size();
      if (n < 2) return PathOpRefusal::DegenerateSubPath;
      // `touchedAnchors()` sorts, so `a.anchor < b.anchor` always.
      const bool neighbours = b.anchor == a.anchor + 1;
      // The closing segment joins the last anchor back to the first, so those
      // two are adjacent on a CLOSED subpath and on no other.
      const bool acrossSeam = sub.closed && a.anchor == 0 && b.anchor == n - 1;
      if (!neighbours && !acrossSeam) return PathOpRefusal::NotAdjacent;
      return PathOpRefusal::None;
    }

    case PathOp::MakeCompound: {
      if (selection.mode != PathSelectMode::Shape) return PathOpRefusal::WrongSelectMode;
      const std::vector<size_t> sel = selectedShapeIndices(shapes, selection);
      if (sel.empty()) return PathOpRefusal::StaleSelection;
      if (sel.size() < 2) return PathOpRefusal::NeedsTwoShapes;
      return PathOpRefusal::None;
    }

    case PathOp::ReleaseCompound: {
      if (selection.mode != PathSelectMode::Shape) return PathOpRefusal::WrongSelectMode;
      const std::vector<size_t> sel = selectedShapeIndices(shapes, selection);
      if (sel.empty()) return PathOpRefusal::StaleSelection;
      for (size_t i : sel)
        if (shapes[i].path.subpaths.size() > 1) return PathOpRefusal::None;
      return PathOpRefusal::NotCompound;
    }
  }
  return PathOpRefusal::None;
}

namespace {

// ==========================================================================
// JOIN
// ==========================================================================
//
// The four endpoint combinations reduce to one append by reversing whichever
// side is facing the wrong way. Normalising like this rather than writing
// four arms is what keeps the anchor ORDER correct in all four -- and the
// order is exactly what `app/selftest/PathOps.cpp` asserts, because a join
// that produces the right anchor COUNT with the wrong order is a path that
// crosses itself and looks like a rendering bug.
PathOpResult doJoin(std::vector<VectorShape>* shapes, AnchorAt a, AnchorAt b) {
  PathOpResult out;

  // Same subpath, two distinct ends -> this is a close, not a concatenation.
  if (a.shape == b.shape && a.sub == b.sub) {
    SubPath& sub = (*shapes)[a.shape].path.subpaths[a.sub];
    sub.closed = true;
    refitSmoothSeam(sub, 0);
    refitSmoothSeam(sub, sub.anchors.size() - 1);
    out.changed = true;
    return out;
  }

  // `touchedAnchors()` sorted, so `a` is at or before `b` in the shapes
  // vector and, within one shape, at a lower subpath index. `a` therefore
  // survives -- the plan's "first shape's paint wins", made structural
  // rather than restated as a comparison here.
  SubPath tail = (*shapes)[b.shape].path.subpaths[b.sub];
  SubPath& head = (*shapes)[a.shape].path.subpaths[a.sub];

  // The selected end of `head` must become its LAST anchor, and the selected
  // end of `tail` its FIRST, so that appending puts the two selected anchors
  // next to each other. Each side needs at most one reversal.
  if (a.anchor == 0) reverseSubPath(head);
  if (b.anchor + 1 == tail.anchors.size()) reverseSubPath(tail);

  const size_t seam = head.anchors.size();  // index the tail's first anchor lands on
  head.anchors.insert(head.anchors.end(), tail.anchors.begin(), tail.anchors.end());
  // The two anchors either side of the new segment each gained a neighbour.
  // Only refit the ones the user had already marked smooth -- see
  // `refitSmoothSeam()`.
  if (seam > 0) refitSmoothSeam(head, seam - 1);
  refitSmoothSeam(head, seam);

  // Remove the donor subpath, and the donor shape if that was its last.
  VectorShape& donor = (*shapes)[b.shape];
  donor.path.subpaths.erase(donor.path.subpaths.begin() + static_cast<std::ptrdiff_t>(b.sub));
  if (donor.path.subpaths.empty()) {
    // Only a CROSS-SHAPE join can reach here (the same-shape case returned
    // above), so this is where a second shape's style is discarded. Reported
    // only when it was actually different from the survivor's: two identical
    // styles losing one of them is not a loss, and a warning that fires every
    // time is a warning nobody reads.
    out.discardedShapeStyle = !styleSame((*shapes)[a.shape], donor);
    out.erasedShapes.push_back(donor.id);
    shapes->erase(shapes->begin() + static_cast<std::ptrdiff_t>(b.shape));
  } else {
    out.discardedShapeStyle = false;
  }
  out.changed = true;
  return out;
}

}  // namespace

void splitSegmentAt(SubPath& sub, size_t segmentIndex, float t) {
  if (segmentIndex >= subPathSegmentCount(sub)) return;
  const size_t n = sub.anchors.size();
  const size_t i = segmentIndex;
  const size_t j = (i + 1) % n;

  auto lerp = [t](PathPoint a, PathPoint b) {
    return PathPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  };

  // de Casteljau. The two half-curves reproduce the original exactly, so the
  // shape does not move -- see the header: an insertion that redraws the
  // user's curve is not "add a point here".
  const PathPoint p0 = sub.anchors[i].pt;
  const PathPoint p1 = sub.anchors[i].out;
  const PathPoint p2 = sub.anchors[j].in;
  const PathPoint p3 = sub.anchors[j].pt;

  const PathPoint q0 = lerp(p0, p1);
  const PathPoint q1 = lerp(p1, p2);
  const PathPoint q2 = lerp(p2, p3);
  const PathPoint r0 = lerp(q0, q1);
  const PathPoint r1 = lerp(q1, q2);
  const PathPoint s = lerp(r0, r1);

  sub.anchors[i].out = q0;
  sub.anchors[j].in = q2;

  Anchor mid;
  mid.pt = s;
  mid.in = r0;
  mid.out = r1;
  // Smooth by construction: `r0`, `s` and `r1` are collinear -- that is a
  // property of de Casteljau, not something set here -- so the flag says what
  // is already true of the geometry rather than imposing it.
  mid.smooth = true;

  // `j == 0` is the closing segment, whose new anchor belongs at the END of
  // the list rather than before anchor 0: inserting at the front would
  // renumber every anchor and put the new point at the path's start instead
  // of its finish.
  const size_t insertAt = (j == 0) ? n : i + 1;
  sub.anchors.insert(sub.anchors.begin() + static_cast<std::ptrdiff_t>(insertAt), mid);
}

PathOpResult runPathOp(PathOp op, std::vector<VectorShape>* shapes, uint64_t* nextShapeId,
                       const PathSelection& selection) {
  PathOpResult out;
  if (shapes == nullptr || nextShapeId == nullptr) {
    out.refusal = PathOpRefusal::StaleSelection;
    return out;
  }
  out.refusal = pathOpCanRun(op, *shapes, selection);
  if (out.refusal != PathOpRefusal::None) return out;

  switch (op) {
    case PathOp::Close: {
      for (const SubPathRef& r : touchedSubPaths(*shapes, selection)) {
        SubPath& sub = (*shapes)[r.shape].path.subpaths[r.sub];
        if (sub.closed || sub.anchors.size() < 2) continue;
        sub.closed = true;
        // The first and last anchor each gained a neighbour across the new
        // closing segment; nothing else in the subpath changed.
        refitSmoothSeam(sub, 0);
        refitSmoothSeam(sub, sub.anchors.size() - 1);
        out.changed = true;
      }
      return out;
    }

    case PathOp::Open: {
      // Where the cut falls: BEFORE the lowest selected anchor of each
      // subpath, achieved by rotating that anchor to index 0 and dropping
      // the closing segment.
      //
      // **No anchor is duplicated.** Illustrator's scissors split the cut
      // anchor into two coincident endpoints; this leaves one. Duplicating
      // would put two anchors at the same point with no visible difference
      // and a selection that can pick either -- and `core/Path.hpp` section 3
      // is explicit that a repeated vertex is a second representation of the
      // same path.
      const std::vector<AnchorAt> anchors = touchedAnchors(*shapes, selection);
      std::vector<SubPathRef> done;
      for (const AnchorAt& at : anchors) {
        const SubPathRef ref{at.shape, at.sub};
        if (std::find(done.begin(), done.end(), ref) != done.end()) continue;
        SubPath& sub = (*shapes)[at.shape].path.subpaths[at.sub];
        if (!sub.closed || sub.anchors.size() < 2) continue;
        done.push_back(ref);
        std::rotate(sub.anchors.begin(),
                    sub.anchors.begin() + static_cast<std::ptrdiff_t>(at.anchor),
                    sub.anchors.end());
        sub.closed = false;
        out.changed = true;
      }
      return out;
    }

    case PathOp::Reverse: {
      for (const SubPathRef& r : touchedSubPaths(*shapes, selection)) {
        SubPath& sub = (*shapes)[r.shape].path.subpaths[r.sub];
        if (sub.anchors.size() < 2) continue;
        reverseSubPath(sub);
        out.changed = true;
      }
      return out;
    }

    case PathOp::Smooth: {
      for (const AnchorAt& at : touchedAnchors(*shapes, selection)) {
        SubPath& sub = (*shapes)[at.shape].path.subpaths[at.sub];
        fitAnchorTangent(&sub, at.anchor, sub.closed);
        out.changed = true;
      }
      return out;
    }

    case PathOp::Corner: {
      for (const AnchorAt& at : touchedAnchors(*shapes, selection)) {
        Anchor& a = (*shapes)[at.shape].path.subpaths[at.sub].anchors[at.anchor];
        // Handles collapse onto the point, so both adjoining segments become
        // cubics whose control points are collinear and evenly spaced --
        // geometrically exact straight lines (`core/Path.hpp` section 1), not
        // an approximation of them.
        a.in = a.pt;
        a.out = a.pt;
        a.smooth = false;
        out.changed = true;
      }
      return out;
    }

    case PathOp::Break: {
      for (const AnchorAt& at : touchedAnchors(*shapes, selection)) {
        Anchor& a = (*shapes)[at.shape].path.subpaths[at.sub].anchors[at.anchor];
        // The handles stay exactly where they are; only the intent flag
        // changes, so the next drag on one handle stops mirroring the other.
        // This is the verb a user wants when kinking a curve, and it is NOT
        // `Corner` -- see docs/path-editing-plan.md section 1.3.
        a.smooth = false;
        out.changed = true;
      }
      return out;
    }

    case PathOp::Join: {
      const std::vector<AnchorAt> anchors = touchedAnchors(*shapes, selection);
      return doJoin(shapes, anchors[0], anchors[1]);
    }

    case PathOp::InsertAnchor: {
      const std::vector<AnchorAt> anchors = touchedAnchors(*shapes, selection);
      const AnchorAt a = anchors[0];
      const AnchorAt b = anchors[1];
      SubPath& sub = (*shapes)[a.shape].path.subpaths[a.sub];
      // `pathOpCanRun()` has already established these are adjacent, either
      // as neighbours or across a closed subpath's seam. The seam's segment
      // index is the last one, not `a.anchor`.
      const size_t seg =
          (b.anchor == a.anchor + 1) ? a.anchor : subPathSegmentCount(sub) - 1;
      splitSegmentAt(sub, seg, 0.5f);
      out.changed = true;
      return out;
    }

    case PathOp::DeleteAnchor: {
      // Descending, so each erase leaves the indices of the not-yet-erased
      // anchors untouched. `touchedAnchors()` is sorted ascending, so this
      // is a reverse walk rather than a second sort.
      const std::vector<AnchorAt> anchors = touchedAnchors(*shapes, selection);
      for (size_t k = anchors.size(); k-- > 0;) {
        const AnchorAt& at = anchors[k];
        SubPath& sub = (*shapes)[at.shape].path.subpaths[at.sub];
        sub.anchors.erase(sub.anchors.begin() + static_cast<std::ptrdiff_t>(at.anchor));
        out.changed = true;
      }
      // Cascade, outermost last: an emptied subpath goes, and a shape whose
      // last subpath went goes with it. A subpath left with ONE anchor stays
      // -- that is the state the Pen's own first press creates, so erasing it
      // here would make "place a point, delete its neighbour" destroy the
      // shape.
      for (size_t i = shapes->size(); i-- > 0;) {
        Path& p = (*shapes)[i].path;
        for (size_t sp = p.subpaths.size(); sp-- > 0;)
          if (p.subpaths[sp].anchors.empty())
            p.subpaths.erase(p.subpaths.begin() + static_cast<std::ptrdiff_t>(sp));
        if (p.subpaths.empty()) {
          out.erasedShapes.push_back((*shapes)[i].id);
          shapes->erase(shapes->begin() + static_cast<std::ptrdiff_t>(i));
        }
      }
      return out;
    }

    case PathOp::MakeCompound: {
      const std::vector<size_t> sel = selectedShapeIndices(*shapes, selection);
      const size_t survivor = sel.front();  // sorted: the earliest in the vector

      // **Gathered ascending, erased descending, and the two passes cannot be
      // folded into one.** Appending inside a descending erase loop -- the
      // obvious single pass, since erasing from the back leaves the earlier
      // indices valid -- appends the donors in REVERSE, so three shapes
      // compound as A, C, B. That reorders the user's contours silently,
      // which for a compound path means the fill rule sees a different
      // winding order than the picture did. `app/selftest/PathOps.cpp`
      // asserts the order for exactly this reason, and caught it.
      std::vector<SubPath> gathered;
      for (size_t k = 1; k < sel.size(); ++k) {
        VectorShape& donor = (*shapes)[sel[k]];
        if (!styleSame((*shapes)[survivor], donor)) out.discardedShapeStyle = true;
        gathered.insert(gathered.end(), std::make_move_iterator(donor.path.subpaths.begin()),
                        std::make_move_iterator(donor.path.subpaths.end()));
        out.erasedShapes.push_back(donor.id);
      }
      Path& into = (*shapes)[survivor].path;
      into.subpaths.insert(into.subpaths.end(), std::make_move_iterator(gathered.begin()),
                           std::make_move_iterator(gathered.end()));
      for (size_t k = sel.size(); k-- > 1;)
        shapes->erase(shapes->begin() + static_cast<std::ptrdiff_t>(sel[k]));
      out.changed = true;
      return out;
    }

    case PathOp::ReleaseCompound: {
      // Descending over the selected shapes so an insertion for one does not
      // shift the index of another still to be processed.
      const std::vector<size_t> sel = selectedShapeIndices(*shapes, selection);
      for (size_t k = sel.size(); k-- > 0;) {
        const size_t i = sel[k];
        if ((*shapes)[i].path.subpaths.size() < 2) continue;
        // Every contour after the first becomes its own shape, carrying a
        // COPY of the original's paint -- releasing a compound must not
        // change how any of it looks, only how many objects it is.
        std::vector<VectorShape> made;
        VectorShape& src = (*shapes)[i];
        for (size_t sp = 1; sp < src.path.subpaths.size(); ++sp) {
          VectorShape piece = src;      // paint, stroke style, name
          piece.id = (*nextShapeId)++;  // never the source's id
          piece.pivot.reset();          // the stored pivot belonged to the whole
          piece.path.subpaths.assign(1, src.path.subpaths[sp]);
          out.createdShapes.push_back(piece.id);
          made.push_back(std::move(piece));
        }
        src.path.subpaths.resize(1);
        shapes->insert(shapes->begin() + static_cast<std::ptrdiff_t>(i) + 1,
                       std::make_move_iterator(made.begin()),
                       std::make_move_iterator(made.end()));
        out.changed = true;
      }
      return out;
    }
  }
  return out;
}

}  // namespace np
