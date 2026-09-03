#pragma once

#include <vector>

#include "core/Path.hpp"
#include "core/PathFlatten.hpp"

// core/PathStroke -- turning a stroke into a fill.
//
// SVG's `stroke`, and PRD J4's fill's counterpart. **Not PRD J3** ("stroke a
// path with the current brush"), which is a different operation entirely:
// that one flattens a path and feeds brush/StrokePath's arc-length dab
// emitter, depositing pigment. This file produces *geometry* -- a new `Path`
// whose filled area is the stroked region -- so that a stroked shape and a
// filled one go through one rasteriser and get identical antialiasing.
//
// ==========================================================================
// The outline is a UNION of simple pieces, not an offset curve
// ==========================================================================
//
// The textbook stroker offsets the path to each side, joins the two offset
// curves at every vertex, and closes the ends with caps -- producing one
// outline contour. It is also where hand-written strokers go wrong, and the
// reason is that offsetting is not a closed operation on curves: the true
// offset of a cubic is not a cubic, tight curvature makes the offset
// self-intersect, and removing those self-intersections needs a boolean
// operation on curves that is far more machinery than the stroke itself.
//
// This file takes the other route. After flattening, a stroke is exactly:
//
//     the union of  (a rectangle per segment)
//                 + (a join wedge per interior vertex)
//                 + (a cap per open end)
//
// Each piece is emitted as its own closed subpath, all wound the same
// direction, and the result is filled **nonzero**. Overlap is then not a
// problem to solve but the mechanism: two overlapping rectangles wind to 2,
// and nonzero says 2 is inside exactly as 1 is. Self-intersection at tight
// curvature -- the case that breaks an offset stroker -- needs no special
// handling at all, because the pieces were never required to form a single
// non-self-intersecting outline.
//
// The costs are honest and small: more contours (and so more edges) than a
// single offset outline, and a result that is a fill region rather than a
// traced outline, so it cannot be handed to something that wants the stroke's
// own boundary as a curve. Nothing in this application wants that.
//
// Consequently **the returned path is always `FillRule::NonZero`**, whatever
// the input's rule was. An even-odd stroke would punch holes wherever the
// stroke crossed itself, which is not what a stroke means.
namespace np {

// SVG's `stroke-linecap`.
enum class LineCap { Butt, Round, Square };

// SVG's `stroke-linejoin`.
enum class LineJoin { Miter, Round, Bevel };

struct StrokeStyle {
  float width = 1.0f;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  // SVG's `stroke-miterlimit`: the ratio of miter length to stroke width past
  // which a miter join falls back to a bevel. 4 is SVG's initial value.
  float miterLimit = 4.0f;
  // SVG's `stroke-dasharray`, in user units, alternating on/off. Empty means
  // a solid stroke. An all-zero or negative array is treated as solid rather
  // than as an infinite loop of zero-length dashes.
  std::vector<float> dashes;
  float dashOffset = 0.0f;
};

// Build the fill region for stroking `path` with `style`.
//
// `tolerancePx` is core/PathFlatten's curve tolerance and is also the chord
// tolerance used for round joins and caps, so a round cap is as smooth as the
// curve it terminates.
//
// Returns an empty path (not a refusal) when the width is non-positive or the
// input encloses nothing -- a zero-width stroke draws nothing, which is what
// SVG specifies, and is not an error a caller should have to branch on.
Path strokePath(const Path& path, const StrokeStyle& style, float tolerancePx);

// The dash walk on its own, exposed because it is the part with the fiddly
// state (phase carried across segments, and across the closing edge of a
// closed contour) and therefore the part worth testing directly.
//
// Returns the "on" runs of `contour` as open polylines. A solid style returns
// the contour unchanged as a single run.
std::vector<FlatContour> dashContour(const FlatContour& contour,
                                     const std::vector<float>& dashes,
                                     float dashOffset);

}  // namespace np
