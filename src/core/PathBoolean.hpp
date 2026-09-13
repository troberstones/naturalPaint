#pragma once

#include "core/Path.hpp"

// core/PathBoolean -- union, intersection, difference and XOR of two filled
// regions (docs/psd-vector-shapes.md S3).
//
// Each operand is a whole `Path` read under its OWN fill rule, so an EvenOdd
// ring and a NonZero square combine correctly. Open subpaths count as closed,
// matching core/PathRaster's fill.
//
// **The result is polygonal.** Operands are flattened with core/PathFlatten at
// `tolerancePx`, and every output anchor has `in == out == pt` (core/Path.hpp
// section 1's exact straight line). Keeping the curves would need cubic-cubic
// intersection and curve splitting through every stage below; within a tenth
// of a pixel nobody can see the difference, but an editor can, so callers that
// have a curve-preserving alternative (composePsdSubPaths' compound shortcut)
// should prefer it and use this where they do not.
//
// The output is always `FillRule::NonZero`, non-self-intersecting, with every
// contour oriented so the filled side is on its left: holes run opposite to
// their outlines, which is what makes one rule enough.
namespace np {

enum class PathBooleanOp {
  Union,
  Intersect,
  Difference,  // a minus b
  Xor,
};

inline constexpr float kPathBooleanDefaultTolerancePx = 0.1f;

// An empty Path when the result encloses nothing, or when either operand
// fails `pathIsFinite()`.
Path pathBoolean(PathBooleanOp op, const Path& a, const Path& b,
                 float tolerancePx = kPathBooleanDefaultTolerancePx);

// Net enclosed area of a path whose contours do not cross -- pathBoolean()'s
// own output -- as the absolute sum of signed polygon areas after flattening.
// Not a coverage integral: on a self-intersecting path it is meaningless.
double pathBooleanArea(const Path& path, float tolerancePx = kPathBooleanDefaultTolerancePx);

}  // namespace np
