#pragma once

#include <cstddef>
#include <vector>

#include "core/Path.hpp"

// core/PathFlatten -- curves to polylines, and the two conversions that feed
// core/Path from outside (elliptical arcs, and a tight bound).
//
// Everything downstream of this file works on straight segments. That is the
// whole reason it exists: core/PathRaster rasterises polygons exactly (see its
// own header), core/PathStroke offsets polylines, and brush/StrokePath walks a
// polyline's arc length. One flattener with one tolerance rule means those
// three never disagree about where a curve is.
//
// ==========================================================================
// The segment count is computed, not discovered by recursion
// ==========================================================================
//
// The familiar way to flatten a cubic is recursive de Casteljau subdivision
// with a flatness test at each level. This file does not do that, and the
// reason is the untrusted input: io/SvgImport parses coordinates out of a file
// this build did not write, and a recursive subdivider's termination depends
// on the flatness test eventually passing. Under coordinates that are merely
// *large* rather than invalid -- a path in a document scaled by 1e6, which is
// a real thing exporters emit -- the test keeps failing and the recursion
// keeps going, so the guard has to be a depth limit, and a depth limit means
// the tolerance is silently not met.
//
// Instead the count comes from a closed-form bound on the error. For a cubic,
// approximating with `n` equal-parameter segments has error at most
//
//     err <= max|B''(t)| / (8 n^2),   and   max|B''(t)| <= 6 L
//     where L = max(|p0 - 2 p1 + p2|, |p1 - 2 p2 + p3|)
//
// so `err <= 3L / (4 n^2)`, and `n = ceil(sqrt(3L / (4 tol)))` meets any
// tolerance in one O(1) step with no recursion, no stack, and no depth limit.
// `L` is a second difference of the control points, which is exactly the
// quantity that stays small for a nearly-straight curve however large its
// coordinates are -- so a translated path costs what an untranslated one
// costs, which a chord-distance flatness test does not guarantee.
//
// The count is still clamped (`kMaxSegmentsPerCurve`), because a caller may
// pass a tolerance of zero or a curve whose control points are 1e30 apart. The
// clamp is a refusal to allocate unboundedly, not a quality setting, and it is
// far above what any real curve at a sane tolerance asks for.
//
// ==========================================================================
// Tolerance is in device pixels, so quality tracks zoom
// ==========================================================================
//
// Every entry point takes `tolerancePx`: the maximum distance, **in the space
// the points are in**, between the true curve and the polyline. Callers
// rasterising at 1:1 pass a fraction of a texel. Callers flattening for an
// on-screen overlay at 8x zoom pass `1/8` of that, because the error the user
// sees is the error times the zoom. Getting this backwards is why vector
// overlays go visibly faceted when you zoom in, and it is a caller's decision
// rather than this file's, so there is no default.
namespace np {

// A flattened contour: consecutive points joined by straight segments. A
// closed contour does NOT repeat its first point at the end -- the closing
// edge is implied, matching `SubPath::closed` and core/SelectionShapes'
// `selectPolygon()`, so that the two never disagree about how many edges a
// triangle has.
struct FlatContour {
  std::vector<PathPoint> points;
  bool closed = false;
};

// The clamp described above. A cubic at a 0.1px tolerance spanning the whole
// of a 5000px canvas asks for roughly 275 segments, so this is about three
// orders of magnitude of headroom before it binds.
inline constexpr size_t kMaxSegmentsPerCurve = 4096;

// How many line segments one cubic needs to meet `tolerancePx`. Always at
// least 1, never more than `kMaxSegmentsPerCurve`. Exposed because
// core/PathStroke needs the same count to keep its two offset sides in step,
// and recomputing it there from a copied formula is how the two sides drift.
size_t cubicSegmentCount(const PathPoint p[4], float tolerancePx) noexcept;

// Evaluate a cubic at `t` in [0, 1].
PathPoint cubicAt(const PathPoint p[4], float t) noexcept;

// Flatten one path. Subpaths with fewer than two anchors are dropped -- they
// enclose nothing and a zero-length contour is a special case every consumer
// would otherwise have to carry.
//
// Returns an empty vector for a path that `pathIsFinite()` rejects, rather
// than producing NaN vertices that the rasteriser would then have to defend
// against. The refusal is here because this is the boundary the untrusted
// coordinates cross.
std::vector<FlatContour> flattenPath(const Path& path, float tolerancePx);

// The tight bounds of the curve itself, as opposed to `pathControlBounds()`'s
// hull bound: solves the quadratic `B'(t) = 0` per axis per segment and
// includes the extrema that fall inside (0, 1).
//
// Worth the arithmetic exactly where a user can see the difference -- the
// manipulator's box and the layer's own extent on disk. A hull bound around a
// curve with long handles is visibly, wrongly loose in both.
PathBounds pathTightBounds(const Path& path) noexcept;

// --- SVG elliptical arcs ---------------------------------------------------

// Convert one SVG endpoint-parameterised arc (`A rx ry rot largeArc sweep x
// y`) into cubic segments appended to `out` as anchors continuing from
// `from`. The first appended anchor's `in` handle is set; the caller owns
// `from`'s `out` handle, which this writes through `fromOut`.
//
// Returns false and appends nothing when the arc degenerates to a line (zero
// radius, or coincident endpoints), which is not an error -- SVG specifies
// exactly that fallback, and the caller emits a line instead.
//
// Lives here rather than in io/SvgImport because it is pure curve geometry
// with a published derivation (SVG 1.1 appendix F.6), and because putting it
// beside `cubicSegmentCount()` keeps every "how many pieces does this curve
// need" decision in one file. Each arc is split at 90-degree boundaries and
// each piece approximated with the standard `k = 4/3 tan(theta/4)` handle
// length, whose worst-case radial error over a quarter turn is about 2.7e-4
// of the radius -- below a texel for any radius under ~3700px, and asserted
// as such in --selftest rather than taken on faith.
bool arcToCubics(PathPoint from, float rx, float ry, float xAxisRotationDeg,
                 bool largeArc, bool sweep, PathPoint to, PathPoint* fromOut,
                 std::vector<Anchor>* out);

}  // namespace np
