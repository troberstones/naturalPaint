#pragma once

// The selection shapes beyond the rectangle: ellipse, lasso and polygon lasso
// (PRD E3).
//
// Separate from core/SelectionMask, which owns the type and the one
// constructor whose coverage is *separable* -- an axis-aligned rectangle's
// covered area is the product of its two axis overlaps, and
// `selectRectangle()` says in its own comment that this stops being true here.
// So this file is where the non-separable answer gets worked out, and the
// question it has to answer for every shape is the same one: **what fraction
// of this texel's unit square does the shape actually cover?**
//
// --- Exact area, not supersampling ----------------------------------------
//
// Both constructors below compute the true covered area rather than counting
// samples inside the shape. The alternative -- an NxN sample grid per texel --
// was rejected for a reason worth writing down: the store quantises coverage
// to 1/255, so a 4x4 grid would throw away almost all of the precision the
// store is built to carry, and it would do it in exactly the place the eye is
// most sensitive to it. A near-horizontal lasso edge under 16-sample coverage
// walks up in visible 6% steps; the same edge with exact area walks up in
// steps of 1/255. That is the difference between an edge that looks
// antialiased and one that looks dithered.
//
// The cost is that "exact" has to be earned separately for each shape, since
// there is no general formula. See each function for how.

#include <vector>

#include "core/SelectionMask.hpp"

namespace np {

// A vertex in document texel space. Real numbers, like the rectangle's corners
// -- a lasso follows the pointer, and the pointer does not land on integers.
struct SelectionPoint {
  float x = 0.0f;
  float y = 0.0f;
};

// An antialiased axis-aligned ellipse, centred at (cx, cy) with radii rx/ry.
//
// **Exact area, by closed-form integration.** For one texel row the ellipse's
// horizontal extent is `cx +/- rx*sqrt(1 - ((t - cy)/ry)^2)`, so the area it
// covers inside a texel is the integral of that extent over the texel's
// y-span, clipped to the texel's x-span. `integral sqrt(1 - u^2) du` has a
// closed form, and the two places the clip boundary is crossed can be solved
// for directly, so the whole thing comes out in arcsines rather than samples.
//
// A zero or negative radius yields no tiles -- "selects nothing", the same
// answer `selectRectangle()` gives a degenerate rectangle, and for the same
// reason: it is not "selects everything".
Selection selectEllipse(float cx, float cy, float rx, float ry);

// An antialiased polygon: PRD E3's lasso AND its polygon lasso, which are the
// same thing at two sampling densities. A freehand lasso is a polygon with a
// vertex every pointer sample; a polygon lasso is one with a vertex per click.
// There is no second rasteriser for the freehand case.
//
// The path is **implicitly closed** -- the last vertex joins the first -- so
// callers pass the points they collected and do not repeat the first one.
// Fewer than three vertices enclose no area and yield no tiles.
//
// **Self-intersection uses the NONZERO winding rule**, not even-odd. A lasso
// that crosses itself is overwhelmingly a hand that wobbled rather than a
// request for a hole, and even-odd would punch that wobble out of the middle
// of the selection. Even-odd is the right rule for a glyph outline and the
// wrong one for a gesture.
//
// Exactness comes from splitting the work by texel rather than by formula:
// a texel the boundary crosses is clipped against the polygon and its true
// area taken, while a texel the boundary misses is wholly in or wholly out and
// costs a winding test on a scanline. So the expensive path is paid on the
// perimeter and the cheap one over the interior.
Selection selectPolygon(const std::vector<SelectionPoint>& vertices);

}  // namespace np
