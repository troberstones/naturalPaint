#pragma once

#include <cstdint>
#include <vector>

#include "app/AppState.hpp"  // Tool
#include "core/SelectionOps.hpp"  // SelectionCombine
#include "core/VectorShape.hpp"
#include "ops/Transform.hpp"  // Mat3, Point2, mat3MapPoint

// app/PenTool -- the headless core of Stage 4's vector editing, per
// docs/vector-editing.md, the design pass written before any of this code.
// That file is cited by section number throughout; read it first.
//
// ==========================================================================
// 1. WHAT IS AND IS NOT HERE
// ==========================================================================
//
// Everything below is pure geometry over `std::vector<VectorShape>` and the
// two selection types this file defines: no ImGui, no GPU, no `AppState`, no
// drawing. The integrator's UI layer (`ui/MacPaintUI.cpp`) is a thin caller
// of these functions, the same relationship `app/MoveTool.hpp` and
// `app/TransformSession.hpp` have to their own canvas blocks.
//
// **Not here, on purpose:**
//
//   * `PathEditState` -- the interactive session (which drag is live, the
//     component pivot and its "user placed" flag) lives on `AppState` beside
//     `gradientDrag`, per docs/vector-editing.md section 6. This header
//     declares `PathDragKind`, the enum that state is built from, because
//     the six-way distinction is part of the model this file defines and a
//     caller needs the type to hold a `PathDragKind` field at all -- but it
//     declares no `AppState` member and touches no file under `ui/`.
//   * `Tool::Pen`'s `implemented` flag in `ui/AtelierChrome.cpp`, and a term
//     for `toolEditsPath()` in that file's `toolHasCanvasHandler()`. Both
//     happen in the commit that wires an actual canvas gesture to this
//     model, which lands in `ui/MacPaintUI.cpp` and is the integrator's,
//     serialised -- see `toolEditsPath()`'s own comment below for why wiring
//     the gate now would turn `--selftest` red.
//
// ==========================================================================
// 2. THE GATE
// ==========================================================================
//
// `Tool::Pen` and `Tool::Curve` are docs/ui.md section 4a's own grouping --
// "Pen and Curve are blocked on PLAN Phase 13: there is no path model in the
// build" -- and both author/edit the anchor-based path this file now gives a
// model to. `Tool::Shape` is a different kind of gap ("blocked on nothing
// structural") and does not go through anchor-level editing at all, so it is
// not one of these two.
//
// **This is declared and defined, but deliberately NOT yet a term in
// `toolHasCanvasHandler()`.** `Tool::Pen`'s `kToolMeta` row still reads
// `implemented = false`, and `app/selftest/Eyedropper.cpp`'s tripwire
// (section 6 of that file) asserts `toolImplemented(t) == toolHasCanvasHandler(t)`
// for every `Tool`, with a *second* assertion that `toolNoHandlerException()`
// holds zero rows. Wiring `toolEditsPath()` into the gate now, with Pen still
// marked unbuilt, would make those two disagree -- and the tempting repair,
// an exception-table row, is exactly the row that second assertion exists to
// keep empty (see that file's own comment: "the one that existed was
// `Tool::Zoom`, and scrubby zoom landing paid it off... NO recorded
// exceptions remain"). So the predicate exists, headless and tested here,
// and stays unreferenced by `ui/` until the commit that flips the flag and
// wires the gesture in the same breath -- the discipline `app/MoveTool.hpp`
// section 5 and the eraser/pencil/dodge/burn/clone-stamp/smudge rows before
// it all followed.
namespace np {

bool toolEditsPath(Tool t) noexcept;

// ==========================================================================
// 3. SELECTION -- two modes, one modifier grammar
// ==========================================================================
//
// docs/vector-editing.md's opening paragraph: Shape mode (Maya's "object
// mode") selects whole shapes; Component mode selects a set of anchors that
// may span several shapes and stops existing the moment the selection
// changes. `PathSelection` holds exactly one of the two vectors live at a
// time, gated by `mode` -- a tagged union would say the same thing with more
// ceremony for two `std::vector`s that are cheap to leave empty.
enum class PathSelectMode { Shape, Component };

// A tangent handle is addressed by which anchor it belongs to and which of
// its two handles it is. `Point` addresses the anchor itself.
//
// **What `part` scopes, and what it does not.** It is how a single click is
// addressed for hit-testing and for a `TangentDrag` that moves one handle
// alone (docs/vector-editing.md section 6). It does NOT scope
// `applyAffineToSelection()`'s component-mode transform: section 7 of that
// same file is explicit that a component-mode affine moves an anchor's three
// points *together*, so that function looks at which ANCHORS are referenced
// by a selection (de-duplicating `Point`/`InHandle`/`OutHandle` entries for
// the same anchor down to one), not which `part` any one entry named.
enum class AnchorPart { Point, InHandle, OutHandle };

// One selected component: a specific anchor, or one of its handles, on a
// specific subpath of a specific shape.
struct ComponentRef {
  uint64_t shapeId = 0;
  uint32_t subPath = 0;
  uint32_t anchor = 0;
  AnchorPart part = AnchorPart::Point;
};

// Exact equality over all four fields -- what "the same component" means
// everywhere below (deduplicating a selection, or telling an incoming
// marquee result apart from what is already selected).
bool operator==(const ComponentRef& a, const ComponentRef& b) noexcept;

// A total order with no meaning beyond making `ComponentRef` sortable, so the
// set operations below can run as sorted-vector merges
// (`std::set_union`/`_difference`/`_intersection`) instead of an O(n*m)
// nested search or a `std::set` whose per-node overhead buys nothing for
// selections that run to a few hundred components at most, never millions.
bool operator<(const ComponentRef& a, const ComponentRef& b) noexcept;

struct PathSelection {
  PathSelectMode mode = PathSelectMode::Shape;
  std::vector<uint64_t> shapes;          // Shape mode
  std::vector<ComponentRef> components;  // Component mode
};

// ==========================================================================
// 4. THE MODIFIER GRAMMAR -- SET semantics, not coverage semantics
// ==========================================================================
//
// docs/vector-editing.md section 4: the mapping itself is not invented here,
// it is `core/SelectionOps.hpp:94`'s `selectionCombineFromModifiers()`,
// reused unchanged. What is new is what each of the four rules means over a
// SET of ids/components rather than over fractional coverage:
//
//   Replace     the incoming set, `current` discarded
//   Add         union
//   Subtract    `current` minus `incoming`
//   Intersect   `current` INTERSECT `incoming`
//
// Both take `current` as an in/out pointer (mirroring the coverage
// `combineSelections()`'s `base`/`addend` naming would otherwise obscure --
// there is no meaningful "return a new vector" shape here that is cheaper
// than mutating in place, and every call site already owns the vector it is
// updating). The result is always de-duplicated and sorted, so `Add` twice
// over the same marquee is idempotent the same way the coverage rules'
// idempotence is a stated property in core/SelectionOps.hpp.
void combineShapeSelection(std::vector<uint64_t>* current,
                            const std::vector<uint64_t>& incoming, SelectionCombine how);
void combineComponentSelection(std::vector<ComponentRef>* current,
                                const std::vector<ComponentRef>& incoming, SelectionCombine how);

// ==========================================================================
// 5. HIT TESTING
// ==========================================================================
//
// `pickRadiusPx` is in DOCUMENT units; the caller converts from screen (its
// own current zoom) so the pick radius is constant ON SCREEN at any zoom --
// the same convention `app/TransformSession.hpp`'s `handleRadius` parameter
// uses for its own `hitTestTransformHandle()`.
//
// Priority order is docs/vector-editing.md section 3's, and it is asserted
// by name in `app/selftest/PenTool.cpp`, not left implicit in the sequence
// of checks below:
//
//   1. gnomon handles (axis arrows, scale boxes, rotate ring, free-move
//      centre) -- unless `gnomonSuppressed` (Alt/Option held: section 3's
//      escape hatch, suppresses the WHOLE gnomon for the press, not handle
//      by handle)
//   2. the pivot marker, when pivot-move mode is active
//   3. anchor points
//   4. tangent handles -- ONLY for anchors already present in `selection`'s
//      component set (section 3: "tangent handles are drawn only for
//      selected anchors," which is what keeps the overlapping set small)
//   5. path segments
//   6. empty canvas -- `PathHitKind::None`, the caller's cue to start a
//      marquee
enum class PathHitKind { None, GnomonHandle, PivotMarker, Anchor, Tangent, Segment };

struct PathHit {
  PathHitKind kind = PathHitKind::None;
  ComponentRef component;  // meaningful for Anchor/Tangent
  uint64_t shapeId = 0;    // meaningful for Anchor/Tangent/Segment
};

// The gnomon's own document-space geometry, purely as a function of the
// CURRENT selection's pivot and bounds -- the same shape as
// `app/TransformSession.hpp`'s `transformHandlePositions()` /
// `hitTestTransformHandle()` pair, and for the identical reason: a hit test
// has to check against SOMETHING, and that something is derived data a
// caller should read to draw the gnomon rather than a second, independently
// positioned copy `ui/` maintains that could disagree with what this file
// hit-tests against.
//
// `valid` is false when the selection is empty (Shape mode with no shapes,
// or Component mode with no components) -- there is no pivot and no bounds,
// so there is no gnomon to hit and `hitTestPath()` skips straight to anchor
// testing.
//
// `reachPx` is in the same document-space-that-tracks-screen-size unit as
// `pickRadiusPx`, for the same reason `app/TransformSession.hpp`'s
// `rotateReach` parameter is: this file has no notion of zoom (docs/
// vector-editing.md section 2's gnomon is drawn through
// `ViewTransform::toScreen()` by the caller, not by anything here), so a
// caller that wants the gnomon to hold a constant ON-SCREEN size passes its
// own `reachPx = screenPx / zoom`, exactly as `kDefaultRotateHandleReach`'s
// own comment states the rule. The default is a reasonable 1:1-zoom size so
// a caller that has not yet wired zoom-awareness still gets a gnomon that
// hit-tests somewhere sane.
inline constexpr float kDefaultGnomonReachPx = 40.0f;

struct GnomonHandlePositions {
  bool valid = false;
  PathPoint center;       // free-move handle, at the pivot
  PathPoint axisXTip;     // +document-X axis arrow's tip
  PathPoint axisYTip;     // +document-Y axis arrow's tip
  PathPoint corners[4];   // the four scale-box corners of the selection bounds
  float rotateRingRadius = 0.0f;  // ring is centred at `center`
};

GnomonHandlePositions gnomonHandlePositions(const std::vector<VectorShape>& shapes,
                                             const PathSelection& selection,
                                             float reachPx = kDefaultGnomonReachPx) noexcept;

// `pivotMoveModeActive` answers priority level 2 above -- a mode this file
// has no other way to learn, since `PathEditState` (the thing that would
// carry it) lives on `AppState`, which this file does not include. Defaults
// false so a caller that has not wired pivot-move mode yet still gets a
// correct 1/3/4/5/6 ordering.
PathHit hitTestPath(const std::vector<VectorShape>& shapes, const PathSelection& selection,
                     PathPoint at, float pickRadiusPx, bool gnomonSuppressed,
                     bool pivotMoveModeActive = false,
                     float gnomonReachPx = kDefaultGnomonReachPx);

// Everything inside a document-space rectangle. `componentsInRect()` tests
// each anchor's ON-CURVE point only (`AnchorPart::Point`) -- a marquee drags
// out a box over the picture the user is looking at, not over handles that
// are only drawn for anchors already selected (section 3 again), so there is
// nothing for an unselected anchor's handle to be marquee-tested against.
// Both are inclusive of `rect`'s own edges, matching the coverage tools'
// `selectRectangle()` convention of clamped, closed bounds.
std::vector<ComponentRef> componentsInRect(const std::vector<VectorShape>& shapes,
                                            PathBounds rect);

// A shape is included when `rect` overlaps its `pathControlBounds()` --
// deliberately the loose hull bound `core/Path.hpp` documents as "can be too
// large," not the tight one. A marquee-select of whole shapes is a coarse
// gesture (the user is dragging a box, not aiming at a curve), and the loose
// bound is the one every other conservative-inclusion caller in this codebase
// already accepts (`vectorShapesBounds()`, tile allocation) rather than a
// second, more expensive geometry test this operation does not need.
std::vector<uint64_t> shapesIntersectingRect(const std::vector<VectorShape>& shapes,
                                              PathBounds rect);

// ==========================================================================
// 6. PIVOTS -- two concepts, section 1's whole argument
// ==========================================================================
//
// `VectorShape::pivot`: `nullopt` means "use the centroid," a stored `Point`
// means the user placed it there and it must survive the shape later being
// edited into a different centroid (`core/VectorShape.hpp`'s own comment).
// The centroid this file computes for the `nullopt` case is the CENTRE OF
// `pathTightBounds()`, not an area-weighted integral over the curve. That
// matches `app/TransformSession.hpp`'s own manipulator-box precedent
// (`TransformHandlePositions::center`) and `core/PathFlatten.hpp`'s own
// statement of where the tight bound is "worth the arithmetic": "the
// manipulator's box... a hull bound around a curve with long handles is
// visibly, wrongly loose in both." An exact area centroid would need
// Green's-theorem integration over every cubic segment for a property (the
// manipulator's default handle position) nobody can tell apart from the
// bounding-box centre by looking at it -- unlike the bound ITSELF, which
// `pathTightBounds()` already pays for precisely because looseness there is
// visible.
PathPoint shapePivot(const VectorShape& shape) noexcept;

// The transient component-mode pivot: docs/vector-editing.md's table says
// "centroid of the selected anchors," meaning the arithmetic mean of the
// selected anchors' `pt` positions -- not their handles, and not an
// area-weighted anything, because a component selection is a bag of points
// with no enclosed area to weight by. Empty selection returns `{0, 0}`; a
// caller with an empty component selection has no gnomon to place regardless
// (`gnomonHandlePositions()` reports `valid = false` for the same case).
PathPoint componentPivot(const std::vector<VectorShape>& shapes,
                          const std::vector<ComponentRef>& selection) noexcept;

// ==========================================================================
// 7. THE MANIPULATOR
// ==========================================================================
//
// Shape mode: every selected shape's geometry AND its stored `pivot` (when
// one is set) move by `affine`. A `nullopt` pivot is left alone -- there is
// nothing to transform, and the next `shapePivot()` call recomputes the
// centroid from the now-moved geometry for free, which is precisely how
// "the pivot travels with the shape" is supposed to look for the common case
// of a user who has never placed one.
//
// Component mode: the SET OF ANCHORS referenced by `selection.components`
// (de-duplicated across `Point`/`InHandle`/`OutHandle` entries for the same
// anchor -- see `AnchorPart`'s own comment) has its three points, `pt`/
// `in`/`out`, transformed TOGETHER. No shape's `pivot` moves. This is
// docs/vector-editing.md section 1's "make it stick" and section 7's free
// answer from `Anchor::in`/`out` being absolute points: applying `affine` to
// all three of a selected anchor's points is already the correct operation,
// with no special case for the anchor whose neighbour is unselected -- that
// neighbour's own handle, pointing at the moved anchor, is untouched because
// it belongs to a DIFFERENT `Anchor` that was never in the selected set.
void applyAffineToSelection(std::vector<VectorShape>* shapes, const PathSelection& selection,
                             const Mat3& affine);

// Moves one shape's pivot, editing no geometry. `to` is document space, same
// as every anchor (section 1: there is no separate local space to convert
// out of).
void setShapePivot(VectorShape* shape, PathPoint to) noexcept;

// ==========================================================================
// 8. GESTURE STATE -- the enum only; the struct lives on `AppState`
// ==========================================================================
//
// docs/vector-editing.md section 6: six distinct drag meanings, not the
// three the plan guessed, named explicitly so a manipulator drag can never
// be confused with a marquee drag the way `marqueeDragging`'s three writers
// let the Gradient tool's drag collide with the selection switch's `else`
// arm (`ui/MacPaintUI.cpp:13105`, and see that section's own account of why
// this shipped as a bug once already). `PathEditState` -- which of these is
// live, plus the transient component pivot and its "user placed" flag -- is
// declared on `AppState` beside `gradientDrag`, by the integrator, with
// `app/PenTool.cpp` as its only writer; this header only names the enum the
// state is built from.
enum class PathDragKind {
  None,
  Manipulator,  // the gnomon: translate / scale / rotate the selection
  PivotMove,    // move the pivot alone, editing no geometry
  Marquee,      // rubber-band component selection
  AnchorDrag,   // drag a single anchor point directly
  TangentDrag,  // drag one tangent handle
  PenExtend,    // the Pen tool laying down a new anchor
};

}  // namespace np
