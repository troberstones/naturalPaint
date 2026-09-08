#pragma once

#include <array>
#include <vector>

#include "core/Layer.hpp"
#include "flats/Model.hpp"

// flats/Tool -- the flatting gestures, as edits to a Flats layer.
//
// Every function here takes the layer and its current evaluation, appends
// the recorded edit the gesture means to `layer.flats.edits`, and returns
// whether anything was recorded. Nothing here evaluates, composites, draws
// or records history: ui/MacPaintUI calls one of these on a click or a key,
// and on `true` records the document edit, which is what re-evaluates the
// layer through flats/FlatsLayer's cache. On `false` the UI shows the
// reason and records nothing -- a click that changed nothing is not an undo
// step, the paint bucket's own rule.
//
// Headless and GPU-free on purpose: app/selftest cannot run on a machine
// without a Metal adapter, and these are exactly the gestures worth pinning
// (ADR-0009's table), so flats/FlatsSelfTest exercises them through the
// `flatstest` binary.

namespace np {

// The 8-bit display colour of a foreground the palette holds as sRGB floats.
FlatRgb flatRgbFromSrgb(const std::array<float, 3>& srgb);

// The bucket on a Flats layer: recolour the fill under (x, y) -- and, with
// `allSameColour`, every fill wearing that fill's colour -- recording one
// `FlatRecolor` per fill at its anchor, replacing any earlier note for the
// same fill. `slot` is the palette swatch the colour came from, or -1.
// Returns the number of fills recoloured; 0 when the point is on no fill.
int flatsBucketRecolor(Layer& layer, const FlatEvaluation& e, float x, float y, FlatRgb color, int slot,
                       bool allSameColour);

// Option-click: carve a new fill out of the one under (x, y) with the
// layer's gap size as the ball radius. False when no ball fits there.
bool flatsBucketCarve(Layer& layer, const FlatEvaluation& e, float x, float y);

// `K`: delete the fill under (x, y). False on no fill or an already-deleted one.
bool flatsDeleteFill(Layer& layer, const FlatEvaluation& e, float x, float y);

// `M`, second click: merge the fill under b into the fill under a. False
// when either point misses, both are one fill, or either is the background.
bool flatsMergePair(Layer& layer, const FlatEvaluation& e, float ax, float ay, float bx, float by);

// Draw-merge: the stroke's fills merge into the one under its start.
bool flatsDrawMerge(Layer& layer, const FlatEvaluation& e, const FlatPolyline& pts);

// A lasso path as a group (name "Group N") or as a hand-drawn shape fill.
bool flatsGroupFromPath(Layer& layer, const FlatPolyline& path);
bool flatsShapeFromPath(Layer& layer, const FlatPolyline& path, FlatRgb color);

// A bridge pen stroke, or an eraser stroke over one.
bool flatsBridgeStroke(Layer& layer, const FlatPolyline& pts, bool erase);

// `Return`: accept suggestion `index` of `e.suggestions` as a bridge stroke.
bool flatsAcceptSuggestion(Layer& layer, const FlatEvaluation& e, int index);

// Layer > Cluster small fills: every small open-bordered fill merges into
// its neighbour, recorded as anchor-point merge pairs. Returns how many.
int flatsClusterSmall(Layer& layer, const FlatEvaluation& e, int maxArea);

// The select-edits tool: the nearest recorded edit within `reach`, removed.
bool flatsRemoveEditAt(Layer& layer, float x, float y, float reach);

// The maximum fill area `flatsClusterSmall` treats as small for a gap size,
// autoFlats' `max(500, minRegion * 10)`.
int flatsClusterMaxArea(const FlatParams& p) noexcept;

}  // namespace np
