#pragma once

#include <vector>

#include "brush/StrokePath.hpp"

// docs/testing-issues.md T10 ("The three selection-drag gestures are
// missing"): the geometry behind Shift-constrain, Option-from-centre and
// Space-move on the rectangle/ellipse marquee drag.
//
// Pure math, no ImGui/GPU dependency -- the same split app/CurveEdit.hpp and
// app/ControlsLayout.hpp already use (see either header's own doc comment):
// this is the piece of the marquee drag `--selftest` can exercise headlessly.
// The mouse-down latch that picks add/subtract/intersect
// (`SelectionCombine`, core/SelectionOps.hpp, read at
// ui/MacPaintUI.cpp:8584) is deliberately untouched by any of this -- these
// functions only ever produce a *shape*, never a *combine mode*, and the
// widget layer is the only thing that reads Shift/Option for both purposes.
//
// --- Why the anchor is never rewritten --------------------------------
//
// T10 requires Option to be togglable **mid-drag, in both directions**:
// press it, the shape jumps to draw from the centre; release it, the shape
// jumps back to drawing from the corner; press it again and it goes back to
// centre -- with the drag's *anchor* (where the mouse went down) never
// moving underfoot. That is only possible if the anchor stored by the
// caller (`ui/MacPaintUI.cpp`'s `marqueeX0/Y0`) is always the corner the
// drag started at, and `fromCentre` below is purely an interpretation of
// it, applied fresh every frame from the still-unmodified anchor. A design
// that rewrote the anchor into a centre the first time Option was seen would
// have nothing to jump back to on release.
//
// --- Why Space-move is separate state, not a rewritten anchor -----------
//
// **The offset moves the ANCHOR only, and the current point stays the live
// cursor.** That looks asymmetric, and the symmetric-looking alternative is
// wrong, so it is worth being exact about why.
//
// The obvious reading of "translate the whole region" is to add the offset to
// both corners. But the call site derives the offset *from* the same cursor
// it passes as the current point (see ui/MacPaintUI.cpp's Marquee case), so
// the two arguments are not independent -- during a Space-move they advance
// together. Adding the offset to both then counts the hand's movement twice
// on the moving corner, and the box grows by the distance moved on every
// frame.
//
// Write the two corners out and it falls apart cleanly. Let the anchor be A,
// the cursor at Space-down be R, and the hand then move by d, so the live
// cursor is R+d:
//
//   moving corner = cursor            = R + d
//   anchor corner = A + offset        = A + d
//   size          = (R + d) - (A + d) = R - A     <- frozen, as required
//
// Both corners still shift by exactly d, so it *is* a pure translation -- the
// symmetry is in the result, not in the formula. And at the moment Space is
// released the moving corner equals the cursor, which is what lets the drag
// carry on from the moved origin without the shape jumping.
//
// The genuinely symmetric formula fails that last property too: freezing the
// current point at R and offsetting both gives the right size during the
// move, then jumps by d the instant Space comes up.
//
// A test that varies the offset with the cursor held still will pass either
// formula. app/selftest/SelectionDrag.cpp therefore drives the coupled loop
// the call site actually runs; that section is the one that pins this down.

namespace np {

// State for the Space-move gesture (T10), living for the lifetime of one
// marquee drag. Reset to a fresh `SelectionMoveState{}` whenever a new drag
// starts -- an offset from a previous drag has no meaning for this one.
//
// `offsetX`/`offsetY` are what `computeSelectionDragBox()` below adds to the
// anchor (see this header's opening note); everything else here is bookkeeping
// `updateSelectionMove()` needs to make repeated press/release of Space
// during one drag additive rather than replacing.
struct SelectionMoveState {
  // True for exactly the frames between a Space-down and the next Space-up
  // during this drag.
  bool active = false;
  // The cursor position (document texel space) at the most recent Space-down
  // -- the reference point `offsetX/Y` is measured from while `active`.
  float refX = 0.0f, refY = 0.0f;
  // The offset accumulated by every *earlier* Space-move segment of this
  // drag. `offsetX/Y` below is this plus the live delta while `active`, and
  // becomes exactly this again the instant Space is released -- which is
  // what lets a second Space-press keep moving the shape from where the
  // first one left it, rather than measuring from the drag's original
  // anchor every time.
  float baseX = 0.0f, baseY = 0.0f;
  // The current total offset: `baseX/Y` while inactive, `baseX/Y` plus
  // `(cur - ref)` while active. This is the value `computeSelectionDragBox()`
  // actually wants.
  float offsetX = 0.0f, offsetY = 0.0f;
};

// Advances `state` by one frame of the Space-move gesture. `spaceHeld` is
// this frame's live read of the Space key; `curX/curY` the live cursor, in
// the same document texel space as the marquee drag's own anchor/current
// point.
//
// Must be called once per frame for the whole duration of a marquee drag
// (including the frames Space transitions either way) for `state.active` to
// correctly detect the down/up edges -- a caller that only calls this while
// some other condition holds will miss a transition that happened outside
// it.
void updateSelectionMove(SelectionMoveState& state, bool spaceHeld, float curX,
                          float curY) noexcept;

// The marquee drag's bounding box, in the same document texel space as its
// inputs, with T10's three gestures folded in. Always returned sorted --
// `x0 <= x1` and `y0 <= y1` -- exactly like the plain min/max the caller used
// to take of two raw corners, so a caller clamping the result to the canvas
// (as ui/MacPaintUI.cpp already does) needs no further sorting.
struct SelectionDragBox {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// `anchorX/Y` is the drag's stored start point (`marqueeX0/Y0`, unmodified
// by Option -- see this header's doc comment on why); `curX/Y` is the live
// cursor. `offsetX/Y` is `SelectionMoveState::offsetX/Y` above, added to the
// **anchor only** -- this header's opening note derives why that, and not the
// symmetric-looking alternative, is what makes a Space-move a translation.
// `constrainSquare` and `fromCentre` are this frame's live Shift/Option reads.
//
// Order of operations: the Space offset moves the anchor first, then Shift's
// constrain (recomputing the delta from the moved anchor, so a constrained
// shape that is then moved stays constrained), then Option's from-centre
// reinterpretation of that same moved anchor.
// Shift+Option during the drag composes both, matching the modifier table
// in docs/testing-issues.md's T10 entry.
SelectionDragBox computeSelectionDragBox(float anchorX, float anchorY, float curX, float curY,
                                          float offsetX, float offsetY, bool constrainSquare,
                                          bool fromCentre) noexcept;

// docs/testing-issues.md T13 ("The ellipse marquee draws a rectangle while
// you drag it"): the point run behind the ellipse marquee's LIVE preview.
//
// `x0,y0,x1,y1` is meant to be a `SelectionDragBox`'s own fields -- this
// frame's `computeSelectionDragBox()` result, already carrying T10's
// Shift-constrain, Option-from-centre and Space-move -- and NOT the drag's
// raw anchor/cursor. `ui/MacPaintUI.cpp`'s `case Tool::EllipseMarquee:`
// commit arm builds the real selection from that same box the identical
// way (`selectEllipse(cx, cy, rx, ry)` with `cx,cy` the box's centre and
// `rx,ry` its half-extents), so a caller that inscribes the SAME box here
// draws exactly the shape mouse-up commits. Feeding this the drag's raw
// corners instead would repeat T13's original bug one level down --
// app/selftest/EllipseMarqueePreview.cpp's "wrong box" case measures how
// far that disagreement runs under Option-from-centre, where the anchor
// stops being a corner at all.
//
// `segments` is the caller's resolution choice, not a hidden constant --
// ui/MacPaintUI.cpp draws with a fixed count sized for the screen (see its
// call site's own comment), and the selftest can ask for a finer run to
// measure agreement against core/SelectionBoundary's crack-edge trace
// without the sampling itself dominating the measured gap. `segments < 3`
// returns empty: fewer than three points cannot close into a shape, and an
// empty run draws nothing rather than a degenerate sliver.
//
// Pure and ImGui-free, the same split every function in this header keeps.
std::vector<Vec2> ellipseMarqueePreviewPoints(float x0, float y0, float x1, float y1,
                                              int segments);

}  // namespace np
