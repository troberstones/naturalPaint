#pragma once

#include <cstdint>
#include <vector>

#include "app/PenTool.hpp"  // PathSelection, ComponentRef, PathSelectMode
#include "core/VectorShape.hpp"

// app/PathOps -- the PATHS panel's verbs, headless
// (docs/path-editing-plan.md section 1).
//
// ==========================================================================
// 1. WHAT IS AND IS NOT HERE
// ==========================================================================
//
// Pure list mutation and geometry over `std::vector<VectorShape>` plus a
// `PathSelection`: no ImGui, no GPU, no `AppState`, no drawing, no undo. The
// same relationship `app/PenTool` has to `ui/MacPaintUI.cpp`'s canvas block,
// and for the same reason -- every one of these is exercisable by
// `--selftest` with no window.
//
// **Not here, on purpose:**
//
//   * `recordEdit()`. Every verb below mutates `*shapes` directly and the
//     caller records one edit for the whole call. A verb that recorded its
//     own would need an `OpenDocument`, which is what makes the difference
//     between a module a test can drive and one it cannot.
//   * The buttons. `ui/MacPaintUI.cpp`'s PATHS panel is a thin caller: it
//     runs each verb's refusal check once per frame to decide what to grey,
//     and calls the verb on the click.
//   * Anything that needs a canvas point. Every verb acts on the SELECTION,
//     which is why the panel needs no `onCanvas` and no request-flag round
//     trip -- see this file's section 4.
//
// ==========================================================================
// 2. THE REFUSAL IS THE SPECIFICATION
// ==========================================================================
//
// Each verb returns a `PathOpResult`, not a `bool`. `changed` says whether
// `*shapes` was touched; `refusal` says why not when it was not.
//
// **A refusal enum rather than a bare false**, because this panel's buttons
// are greyed by asking the verb itself. A `bool` would force the panel to
// re-derive "can this run?" from the selection in its own words -- a second
// implementation of every precondition below, in the file least able to test
// it, drifting from this one the first time a rule changes. So the panel
// asks by CALLING: `pathOpCanRun()` runs the same preconditions against a
// const `shapes` and returns the same enum, and the two share their checks
// rather than agreeing by inspection.
//
// That also means a lit button cannot refuse. If the panel greys on
// `pathOpCanRun() != PathOpRefusal::None` and the click calls the verb, the
// only way to see a refusal sentence is to reach the verb another way -- a
// keyboard binding, a script. The sentence is still written, because
// "impossible today" is how a control lies tomorrow.
namespace np {

// Why a verb did nothing. `None` is the success case, paired with
// `changed == true`.
//
// Deliberately NOT one shared "invalid selection" value: each of these is a
// different sentence to the user and a different thing to do about it, and
// collapsing them is how a panel ends up saying "cannot do that" to six
// distinct situations.
enum class PathOpRefusal {
  None,
  // Nothing selected at all, in either mode.
  EmptySelection,
  // The verb needs anchors and the selection is in Shape mode (or vice
  // versa for the shape-level verbs). The panel's own MODE segment is the
  // fix, so the sentence names it.
  WrongSelectMode,
  // JOIN's arity: it needs exactly two anchors, and there is no sane
  // interpretation of three.
  NeedsTwoAnchors,
  // JOIN again: an anchor that is not the first or last of its subpath, or
  // is on a subpath that is already closed. Interior anchors have no free
  // end to attach anything to.
  NotAnEndpoint,
  // CLOSE on something already closed, OPEN on something already open.
  AlreadyClosed,
  AlreadyOpen,
  // A subpath of fewer than two anchors: there is no segment, so there is
  // nothing to close, reverse or cut.
  DegenerateSubPath,
  // INSERT needs two anchors that are neighbours on one subpath, because the
  // segment between them is what it splits.
  NotAdjacent,
  // COMPOUND needs two or more shapes to combine.
  NeedsTwoShapes,
  // RELEASE needs a shape that actually has more than one subpath.
  NotCompound,
  // The selection names a shape id, subpath or anchor that is not there --
  // a stale selection the caller should have pruned. Distinct from
  // `EmptySelection` because it means a BUG somewhere, not a user who has
  // not clicked yet.
  StaleSelection,
};

// A sentence for the user, in this codebase's refusal voice (the bucket's
// ladder, `g_strokeRefusal`): what happened and what to do, never a code.
// `None` returns an empty string rather than "no error" -- a caller
// displaying it unconditionally shows nothing, which is right.
const char* pathOpRefusalText(PathOpRefusal r) noexcept;

// What one verb did.
struct PathOpResult {
  bool changed = false;
  PathOpRefusal refusal = PathOpRefusal::None;

  // JOIN's honest guard (docs/path-editing-plan.md section 1.2). True when
  // the join consumed a second SHAPE, discarding its fill, stroke, stroke
  // style, pivot and name in favour of the surviving shape's.
  //
  // **Not a refusal**, because the user asked for the join and gets it. It
  // is the panel's cue to SAY what was dropped, which is the difference
  // between a documented rule and a silent loss the user discovers three
  // edits later with no undo left.
  bool discardedShapeStyle = false;

  // Shapes erased by this call: a shape whose last subpath was consumed by a
  // join or a compound, or whose last anchor was deleted. The caller MUST
  // pass these to `pathEditPruneSelection()` -- a `PathEditState` holding a
  // selection or an open placement session on a shape that no longer exists
  // is the dangle `pathEditBeginPen()` already carries a defensive arm for,
  // which is evidence the hazard is real rather than hypothetical.
  std::vector<uint64_t> erasedShapes;

  // Shapes minted by this call -- `ReleaseCompound`'s only, today. The
  // caller selects them, because this file does not touch the selection
  // (section 3).
  std::vector<uint64_t> createdShapes;
};

// ==========================================================================
// 3. THE VERBS
// ==========================================================================
//
// Every one takes the selection by const reference and never edits it: the
// selection is `PathEditState`'s, and `app/PenTool.cpp` is its only writer
// (that header's section 8). A verb that adjusted the selection to match
// what it just did would be a second writer of exactly the state that rule
// exists to keep single -- so the caller re-selects, through PenTool.

// Which verb, for the panel's one-per-frame `pathOpCanRun()` sweep and for
// its command dispatch. A value rather than one predicate per verb, so
// adding a verb is one enumerator and one switch arm rather than a new
// function the panel has to remember to call.
enum class PathOp {
  Close,
  Open,
  Join,
  Reverse,
  Smooth,
  Corner,
  Break,
  InsertAnchor,
  DeleteAnchor,
  MakeCompound,
  ReleaseCompound,
};

// Would `op` run against this selection? Returns `PathOpRefusal::None` when
// it would. Const: runs the verb's preconditions and no mutation, which is
// what makes it safe to call for every verb every frame.
//
// **Shares its checks with the verb rather than restating them** -- see
// section 2. `runPathOp()` below calls this first and returns its refusal.
PathOpRefusal pathOpCanRun(PathOp op, const std::vector<VectorShape>& shapes,
                           const PathSelection& selection) noexcept;

// Run `op`. A no-op returning the refusal when `pathOpCanRun()` says no, so
// a caller may call this without checking first and still cannot corrupt
// anything.
//
// `nextShapeId` is the layer's own `Layer::nextShapeId` counter, advanced by
// `ReleaseCompound` (the one verb that mints shapes) exactly as
// `pathEditBeginPen()` and `makeVectorLayer()` advance it, so an id is never
// reused within its layer. Every other verb leaves it alone. It is a
// required parameter rather than an optional one because a caller that
// forgot it would get duplicate ids from `ReleaseCompound` alone -- a defect
// that shows up as two shapes the selection cannot tell apart, far from the
// call that caused it.
PathOpResult runPathOp(PathOp op, std::vector<VectorShape>* shapes, uint64_t* nextShapeId,
                       const PathSelection& selection);

// A stable name for the button and for the undo entry ("close path",
// "join paths"). Lower case, present tense -- `recordEdit()`'s convention
// everywhere else in this build.
const char* pathOpEditName(PathOp op) noexcept;

// ==========================================================================
// 4. WHY THERE IS NO CANVAS POINT IN THIS FILE
// ==========================================================================
//
// The FLATS TOOLS panel learned, expensively, that a panel button can never
// see `onCanvas`: the docks draw BEFORE the canvas hit-test each frame, so
// when a button is clicked the pointer is over the panel and any command
// needing a canvas position has to be raised as a deferred action and
// consumed in the canvas route next frame.
//
// **None of the verbs above need one.** They all act on the persisted
// `PathSelection`, which outlives the frame, so the PATHS panel calls them
// directly with no request flag and no round trip. That is a real
// simplification over the flats panel and it is stated here so nobody adds
// the deferred-action machinery back by analogy.
//
// `InsertAnchor` is the one that could have needed a point, and does not: it
// splits the segment between two SELECTED adjacent anchors, at its midpoint
// in the curve's own parameter (t = 0.5), not wherever a pointer was. A
// click-to-insert gesture is the Pen's job on the canvas, not a button's.

// Split the segment between two adjacent selected anchors at t = 0.5,
// exposed on its own because `insertAnchorOnSegment` is the one verb whose
// geometry is worth asserting directly rather than through the selection
// plumbing. de Casteljau, so the curve is UNCHANGED by the insertion -- the
// new anchor lands on it and the four surrounding handles are re-derived to
// reproduce the same two half-curves exactly. An insertion that moved the
// curve would be a "add a point here" that redraws the user's shape.
void splitSegmentAt(SubPath& sub, size_t segmentIndex, float t);

}  // namespace np
