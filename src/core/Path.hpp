#pragma once

#include <cstddef>
#include <vector>

// core/Path -- the editable Bezier model (PLAN.md "13 -- Paths"; PRD J1,
// "Draw and edit Bezier paths -- add, delete, move anchors; convert corner <->
// smooth").
//
// This is the *authoring* representation, and it is deliberately not the
// representation a rasteriser wants. Every other file in the path subsystem
// converts away from it:
//
//   core/PathFlatten  Path -> polylines (adaptive subdivision)
//   core/PathRaster   polylines -> antialiased coverage spans
//   core/PathStroke   Path + width/caps/joins -> a new Path to fill
//
// ==========================================================================
// 1. Anchors, not verbs
// ==========================================================================
//
// The obvious model for a path is a verb stream -- `MoveTo`, `LineTo`,
// `CubicTo`, `Close` -- because that is what SVG's `d` attribute is, what
// PostScript is, and what every rasteriser's input looks like. It is the wrong
// model *here*, because PRD J1's verbs are "move anchors" and "convert corner
// <-> smooth", and neither of those is an operation on a verb stream. Finding
// "the anchor under the cursor" in a verb stream means walking it and tracking
// which control points belong to which on-curve point; finding it here is an
// index.
//
// So a subpath is a list of `Anchor`, each carrying its own two tangent
// handles, and the segment between anchors `i` and `i+1` is the cubic
//
//     anchors[i].pt -> anchors[i].out -> anchors[i+1].in -> anchors[i+1].pt
//
// **There is exactly one segment type.** A straight line is an anchor pair
// whose handles coincide with their own anchors, which makes it a cubic whose
// control points are collinear and evenly spaced -- geometrically exact, not
// an approximation. Quadratics are elevated to cubics and SVG's elliptical
// arcs are converted to cubics at parse time (io/SvgImport), so nothing
// downstream ever sees a second curve type and no consumer needs a `switch`.
// That is what makes the flattener and the stroker short.
//
// ==========================================================================
// 2. Handles are absolute, not relative to their anchor
// ==========================================================================
//
// `in` and `out` are positions in the same space as `pt`, not offsets from it.
// The alternative -- storing offsets -- makes "drag the anchor, handles
// follow" free, and that is genuinely the more common edit.
//
// Absolute wins anyway, for two reasons that outrank it:
//
//   * **Transforming a path is one map over every point, with no special
//     cases.** Under offsets, a transform has to apply its full affine to
//     `pt` and only its *linear part* to `in`/`out`, because an offset must
//     not be translated. That is a rule someone will forget exactly once, and
//     the symptom -- handles drifting away from their anchors under a
//     translate -- looks like a rounding bug rather than a missing branch.
//     PLAN Stage 4's manipulator transforms selected anchors *and* their
//     handles together; this is the representation that makes that one loop.
//   * **It is what SVG hands us**, so io/SvgImport stores what it parsed
//     rather than subtracting an anchor out of every control point and the
//     rasteriser adding it back.
//
// The cost is paid in one place: `moveAnchorTo()` below carries the handles,
// so callers do not open-code the three-point update and get it right twice
// and wrong the third time.
//
// ==========================================================================
// 3. What `in` means on the first anchor, and `out` on the last
// ==========================================================================
//
// For a **closed** subpath every anchor has both handles in use: the closing
// segment runs from the last anchor back to the first, so `anchors.back().out`
// and `anchors.front().in` are its two control points. There is no repeated
// vertex -- the closing segment is implied, exactly as core/SelectionShapes'
// `selectPolygon()` implies its own closing edge, and for the same reason
// (a repeated vertex is a second representation of the same path, and two
// representations means two of them can disagree).
//
// For an **open** subpath, `anchors.front().in` and `anchors.back().out` are
// not read by anything. They are still stored, and still transformed, because
// dropping them would mean closing a path silently discards the handles the
// user had already placed -- and closing is a one-keystroke, undoable act.
namespace np {

// A position in document texel space. Real numbers: an anchor lands where the
// pointer was, and the pointer does not land on integers.
//
// A fourth point type in this codebase (after `Vec2` in brush/StrokePath.hpp,
// `SelectionPoint` in core/SelectionShapes.hpp and `Point2` in
// ops/Transform.hpp), and that is a deliberate, narrow choice rather than an
// oversight. `Point2` is the one worth reusing -- it is the type
// `mat3MapPoint()` already speaks -- but it lives in ops/Transform.hpp, a
// 679-line image-resampling header that pulls in core/Tile.hpp and
// core/TileStore.hpp. A `Path` is a member of `core::Layer`, so reusing
// `Point2` would put that header into every translation unit that includes
// core/Layer.hpp, which is nearly all of them. The conversion is two floats
// and it happens in one place (core/PathTransform), so the trade is a handful
// of assignments against a tree-wide compile-time cost.
struct PathPoint {
  float x = 0.0f;
  float y = 0.0f;
};

// One on-curve point and its two off-curve tangent handles. See section 2 for
// why the handles are absolute and section 3 for when each is read.
struct Anchor {
  PathPoint pt;
  // Controls the segment ARRIVING at `pt` (from the previous anchor).
  PathPoint in;
  // Controls the segment LEAVING `pt` (towards the next anchor).
  PathPoint out;

  // PRD J1's "convert corner <-> smooth". **A hint about intent, not a
  // geometric invariant**: nothing in this file enforces that a smooth
  // anchor's handles are actually collinear, and nothing should. The flag
  // says what the *editor* should do when one handle is dragged -- mirror the
  // other, or leave it alone -- and an imported path whose handles happen to
  // be collinear is not thereby a path whose author wanted them locked
  // together. io/SvgImport therefore leaves this false on everything it
  // imports rather than guessing from the geometry.
  bool smooth = false;
};

// One connected run of anchors. A `Path` may have several: the letter "o" is
// two, and so is any shape with a hole.
struct SubPath {
  std::vector<Anchor> anchors;

  // Whether the closing segment (last anchor back to the first) exists. See
  // section 3 -- it is implied, never a repeated anchor.
  bool closed = false;
};

// How overlapping subpaths combine into filled area.
//
// **Both rules are needed, and this is not a preference.** core/SelectionShapes
// argues at length that NONZERO is the right rule for a lasso -- "a hand that
// wobbled rather than a request for a hole" -- and that same comment names the
// case this enum exists for: "even-odd is the right rule for a glyph outline".
// SVG's `fill-rule` carries both, TrueType outlines rely on nonzero and
// PostScript/CFF ones on even-odd, so a rasteriser that implements only one
// renders some fonts and some imported files wrong. See core/PathRaster for
// why that choice constrains the rasterisation algorithm itself.
enum class FillRule {
  NonZero,
  EvenOdd,
};

// A whole path: any number of subpaths sharing one fill rule.
struct Path {
  std::vector<SubPath> subpaths;
  FillRule rule = FillRule::NonZero;
};

// An axis-aligned bounds. `valid` is false for an empty path -- distinct from
// a zero-area bounds around a single point, which is a real answer.
struct PathBounds {
  bool valid = false;
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;
};

// How many cubic segments a subpath has: `n - 1` open, `n` closed, and 0 for
// fewer than two anchors either way. Every walk over a subpath's geometry
// needs this, and open-coding `closed ? n : n - 1` is how a closed single
// anchor becomes a segment from a point to itself.
size_t subPathSegmentCount(const SubPath& sub) noexcept;

// The four control points of segment `i`, in order
// (`p0 = anchors[i].pt`, `p1 = anchors[i].out`, `p2`/`p3` from the next
// anchor, wrapping to anchor 0 when closed). `i` must be less than
// `subPathSegmentCount(sub)`.
void subPathSegment(const SubPath& sub, size_t i, PathPoint out[4]) noexcept;

// True when the path encloses no area *representable* here: no subpaths, or
// every subpath has fewer than two anchors. It says nothing about whether the
// enclosed area is zero -- a closed triangle with three identical anchors is
// not empty by this test, and the rasteriser will correctly produce no
// coverage for it.
bool pathIsEmpty(const Path& path) noexcept;

// True when every coordinate of every anchor and handle is finite.
//
// **The rasteriser's precondition, checked here rather than there.** A NaN
// coordinate in a scanline rasteriser does not produce a wrong pixel; it
// produces a comparison that is false in both directions, which walks a loop
// bound off the end. io/SvgImport parses numbers out of a file this build did
// not write, so this is an untrusted-input guard, not an assertion about our
// own arithmetic.
bool pathIsFinite(const Path& path) noexcept;

// Bounds of the **control points**, not of the curve.
//
// A cubic is contained in the convex hull of its four control points, so this
// is a correct conservative bound and never too small -- which is the property
// every caller actually needs (allocating tiles, deciding what to redraw,
// hit-testing a click). It can be too *large*, for a curve whose handles swing
// well outside the shape.
//
// The tight bound requires solving the derivative per axis per segment, and
// core/PathFlatten offers it as `pathTightBounds()` for the callers that need
// it -- the layer's own extent on disk, and the manipulator's box, both of
// which a user can see and would notice being loose.
PathBounds pathControlBounds(const Path& path) noexcept;

// Move an anchor to a new position, carrying its two handles by the same
// delta so their shape relative to the anchor is preserved. Section 2's
// stated cost, paid in one place.
void moveAnchorTo(Anchor& anchor, PathPoint to) noexcept;

}  // namespace np
