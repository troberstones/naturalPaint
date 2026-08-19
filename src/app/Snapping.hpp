#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "app/AppState.hpp"      // Guide, GuideOrientation
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
//
// Pure math, no ImGui/GPU dependency -- this is the piece of the step that
// --selftest can actually exercise headlessly (see SelfTest.cpp's
// runGuidesGridSnapTest()). Rulers, drag-to-create, the guide popup and the
// grid overlay itself are UI (ui/MacPaintUI.cpp); everything here is what
// they call into.

// All grid-line positions (major and minor) in [rangeMin, rangeMax],
// anchored at document-space 0 -- not an arbitrary offset, so a grid drawn
// over any visible sub-rectangle of the canvas lines up with one drawn over
// any other. `subdivisions` is how many minor cells make up one major
// `spacing` interval (subdivisions == 1 means major lines only). Returns
// positions sorted ascending; empty if spacing/subdivisions are non-positive
// or the range is inverted. This is the function PLAN.md's own selftest
// scope note asks for: "given a spacing and subdivision count, the set of
// grid-line positions in a given range is exactly what you'd hand-compute."
std::vector<float> gridLinePositions(float spacing, int subdivisions, float rangeMin,
                                     float rangeMax);

// True if `pos` (as returned by gridLinePositions(), or any other document
// coordinate) falls on a *major* grid line -- i.e. a multiple of `spacing`
// itself, not merely one of the finer subdivision lines. Used by the grid
// overlay to draw major lines more strongly than minor ones.
bool isMajorGridLine(float pos, float spacing);

// Resolves a guide-position field's text to a document-space coordinate
// along an axis of length `axisExtent` (canvas width for a Vertical guide,
// canvas height for a Horizontal one). Accepts a plain number ("512") or a
// percentage ("50%", of `axisExtent`). Returns nullopt if `text` is empty
// or doesn't parse as either form -- the caller (the "Add Guide" popup)
// leaves the guide list untouched rather than adding garbage.
std::optional<float> parseGuidePosition(std::string_view text, float axisExtent);

// The result of resolveSnap() below: `point` is the (possibly-adjusted)
// document-space position; `snappedX`/`snappedY` say whether that axis
// actually moved -- independently, since a drag can snap in X without
// snapping in Y (dragging near a vertical guide with nothing nearby
// vertically), same as every painting app's guide snap.
struct SnapResult {
  Vec2 point;
  bool snappedX = false;
  bool snappedY = false;
};

// PRD Q6: snaps `point` to the nearest guide, grid line or canvas edge
// within `thresholdDoc` document-space px, independently per axis. Guides
// constrain only the axis perpendicular to their own orientation --
// horizontal guides (a fixed Y) pull Y, vertical guides (a fixed X) pull X --
// matching how a guide actually reads visually. Canvas edges are the
// rectangle (0,0)-(canvasW,canvasH); "layer bounds" from PRD Q6 has no
// separate code path here (see PLAN.md step 12's own scope note) -- the
// interactive canvas has exactly one implicit "layer" today, so layer-bounds
// snapping is this same canvas-edge check until real per-layer bounds exist.
// Grid lines are found analytically (nearest multiple of the minor spacing)
// rather than by enumerating gridLinePositions() over some range -- the
// canvas can be arbitrarily large and a snap only ever needs the single
// nearest line either side of `point`.
//
// Priority when two candidates are equidistant: grid, then canvas edges,
// then guides -- a guide is the most deliberate placement of the three, so
// it wins a tie. `thresholdDoc <= 0` (or an empty candidate set within
// range) leaves the point, and both snapped flags, untouched -- this is
// what lets a caller implement the global snapping toggle (PRD Q6) as
// "pass threshold 0 when st.snappingEnabled is false" rather than a second
// code path.
SnapResult resolveSnap(Vec2 point, const std::vector<Guide>& guides, float gridSpacing,
                       int gridSubdivisions, float canvasW, float canvasH,
                       float thresholdDoc);

}  // namespace np
