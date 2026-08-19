#pragma once

#include <cstddef>
#include <optional>

#include "ops/PointOps.hpp"  // Curve, CurvePoint

namespace np {

// PLAN.md Phase 3 step 8 ("Op-stack UI... and a curve widget operating in
// the shaper domain").
//
// Pure math and list mutation, no ImGui/GPU dependency -- mirrors
// app/Snapping.hpp's own split exactly (see that header's doc comment): this
// is the piece of the curve widget --selftest can actually exercise
// headlessly (see SelfTest.cpp's runCurveEditTest()). The plot, the click/
// drag/right-click handling and the spline draw itself are UI
// (ui/MacPaintUI.cpp); everything here is what that UI calls into.
//
// A curve widget's plot area is a square of `plotSize` screen px representing
// the shaper-domain [0,1]x[0,1] square -- ops/PointOps.hpp's Curve control
// points are already shaper-domain coordinates by contract (ADR-0004), so
// nothing here does any colour-domain conversion; `cx`/`cy` below are always
// literally a Curve's own (x,y). Plot-local pixel coordinates have (0,0) at
// the plot's top-left corner and (plotSize,plotSize) at its bottom-right,
// y growing downward (ImGui's own screen-space convention) -- but a curve's
// y=0..1 axis grows *upward* (the conventional curves-tool orientation, low
// input at bottom-left), so curveToPlot()/plotToCurve() flip y, not just
// scale it.

// Curve-space (x,y) -> plot-local pixel coordinates, at a square plot of
// side `plotSize`. Not clamped -- neither input nor output -- so a caller
// drawing evalCurve()'s flat extrapolation past the authored x-range (its
// own documented boundary behaviour) can still plot points outside the
// square if it chooses to. `plotSize <= 0` maps everything to (0,0) rather
// than dividing by zero.
void curveToPlot(float cx, float cy, float plotSize, float& px, float& py) noexcept;

// Inverse of curveToPlot() above. Also unclamped -- clamping a result to
// [0,1] (the only range evalCurve() gives meaning to) is the caller's
// decision, not this function's; insertPoint()/movePoint() below both do it
// themselves before touching a Curve.
void plotToCurve(float px, float py, float plotSize, float& cx, float& cy) noexcept;

// Nearest control point in `curve` to plot-local pixel (px, py), within
// `radiusPx` of it (inclusive) -- the hit-test every drag/delete gesture
// starts from. Returns nullopt if no point in `curve` is within radiusPx,
// including when `curve` is empty. Each candidate point is mapped through
// curveToPlot() at the given `plotSize` before its screen-space distance to
// (px, py) is measured. On a tie, the earlier index wins (a strict `<`, not
// `<=`, when replacing the running best) -- deterministic, not
// order-of-iteration-dependent by accident.
std::optional<size_t> hitTestPoint(const Curve& curve, float px, float py, float plotSize,
                                    float radiusPx) noexcept;

// Inserts a new point at curve-space (cx, cy) -- both clamped to [0,1]
// first, the only range evalCurve() gives meaning to -- keeping `curve`
// sorted ascending by `.x` regardless of where the new point's x falls
// relative to existing points' own insertion order (evalCurve()/
// applyCurves()'s documented caller contract). Returns the new point's
// index. No de-duplication against an already-matching x: two points
// sharing an x is a degenerate curve evalCurve() doesn't specially guard
// against either, and inserting one is the caller's choice to make, not
// this function's to refuse -- it lands immediately after any existing
// points with that exact x.
size_t insertPoint(Curve& curve, float cx, float cy);

// Moves the point at `index` to curve-space (cx, cy) -- clamped to [0,1] --
// then re-sorts `curve` by `.x` if the move crossed a neighbour, preserving
// the ascending-x invariant every function here (and evalCurve()/
// applyCurves()) relies on. Returns the point's index *after* the move,
// which can differ from `index` if it crossed one or more neighbours.
// `index` must be < curve.size() (bounds-checked via std::vector::at, same
// discipline as core::OpStack's own mutators -- throws std::out_of_range on
// misuse).
size_t movePoint(Curve& curve, size_t index, float cx, float cy);

// Removes the point at `index`. Bounds-checked the same way as movePoint()
// above.
void removePoint(Curve& curve, size_t index);

}  // namespace np
