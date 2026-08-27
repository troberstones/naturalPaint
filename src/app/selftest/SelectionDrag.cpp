#include "app/selftest/Support.hpp"

namespace np {

// docs/testing-issues.md T10 ("The three selection-drag gestures are
// missing"). See SelfTest.hpp for the full breakdown. Pure CPU --
// app/SelectionDrag.hpp has no ImGui/GPU/PaintSim involvement whatsoever.
bool runSelectionDragTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  // Every input below is an integer, and computeSelectionDragBox() is
  // nothing but +, -, std::max and std::copysign over them, so an exact
  // result is representable in float without rounding. 1e-4f is not "the
  // arithmetic is imprecise" slack -- there is none to give here -- it is
  // the same round-trip-safety margin app/CurveEdit's own selftest uses for
  // an identically shallow chain of float ops, kept only so a legitimate
  // refactor that reorders the arithmetic isn't flagged over the last bit
  // of a float.
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol = 1e-4f;
  auto boxEq = [&](const SelectionDragBox& b, float x0, float y0, float x1, float y1) {
    return nearf(b.x0, x0, kTol) && nearf(b.y0, y0, kTol) && nearf(b.x1, x1, kTol) &&
           nearf(b.y1, y1, kTol);
  };

  // --- plain corner drag (no modifiers): matches the old min/max exactly ---
  {
    const SelectionDragBox fwd = computeSelectionDragBox(100.0f, 100.0f, 150.0f, 130.0f, 0.0f,
                                                          0.0f, false, false);
    check(boxEq(fwd, 100.0f, 100.0f, 150.0f, 130.0f),
          "corner drag: anchor(100,100)->cur(150,130), no modifiers, is the plain box");

    // The reverse drag (mouse-down at the far corner, dragged back) must
    // land on the identical sorted box -- this is what lets the commit code
    // drop its own std::min/std::max now that SelectionDragBox promises
    // sorted output.
    const SelectionDragBox rev = computeSelectionDragBox(150.0f, 130.0f, 100.0f, 100.0f, 0.0f,
                                                          0.0f, false, false);
    check(boxEq(rev, 100.0f, 100.0f, 150.0f, 130.0f),
          "corner drag: the reverse-direction drag sorts to the identical box");
  }

  // --- Shift-constrain: square off the LARGER delta, keeping each axis's
  // own sign (T10's explicit "not jumping quadrant" requirement) ---
  {
    // |dx|=50 > |dy|=30: the square is 50x50, growing in +x/+y (dx's own
    // sign, not a magnitude comparison result).
    const SelectionDragBox wideDx = computeSelectionDragBox(100.0f, 100.0f, 150.0f, 130.0f, 0.0f,
                                                             0.0f, /*constrain=*/true, false);
    check(boxEq(wideDx, 100.0f, 100.0f, 150.0f, 150.0f),
          "constrain: |dx|>|dy| squares to |dx| (50x50), matching dx's own sign");

    // |dy|=50 > |dx|=30: the square is 50x50 in the other axis this time.
    const SelectionDragBox wideDy = computeSelectionDragBox(100.0f, 100.0f, 130.0f, 150.0f, 0.0f,
                                                             0.0f, true, false);
    check(boxEq(wideDy, 100.0f, 100.0f, 150.0f, 150.0f),
          "constrain: |dy|>|dx| squares to |dy| (50x50)");

    // The sabotage-proof case: a drag toward the upper-LEFT (both deltas
    // negative) must square into the upper-left quadrant, not flip to
    // upper-right because a naive `std::max(dx,dy)` (no fabs, no copysign)
    // would compare -30 against -40 and hand back -30 as "the max".
    const SelectionDragBox upLeft = computeSelectionDragBox(100.0f, 100.0f, 70.0f, 60.0f, 0.0f,
                                                             0.0f, true, false);
    check(boxEq(upLeft, 60.0f, 60.0f, 100.0f, 100.0f),
          "constrain: a drag toward the upper-left squares into the SAME quadrant "
          "(sign preserved per axis, not the sign of whichever |delta| lost the max)");
  }

  // --- Option-from-centre: anchor is read as the centre, and this must be
  // pure reinterpretation of the SAME anchor -- not a rewrite of it, since
  // T10 requires toggling Option mid-drag to work in both directions ---
  {
    const SelectionDragBox corner = computeSelectionDragBox(100.0f, 100.0f, 130.0f, 120.0f, 0.0f,
                                                             0.0f, false, /*fromCentre=*/false);
    check(boxEq(corner, 100.0f, 100.0f, 130.0f, 120.0f),
          "from-centre off: anchor(100,100) is the corner, as before");

    // The exact same call, differing only in fromCentre, with the anchor
    // argument UNCHANGED -- this is what "toggle mid-drag without rewriting
    // the anchor" looks like as a function call rather than as UI state.
    const SelectionDragBox centre = computeSelectionDragBox(100.0f, 100.0f, 130.0f, 120.0f, 0.0f,
                                                             0.0f, false, /*fromCentre=*/true);
    check(boxEq(centre, 70.0f, 80.0f, 130.0f, 120.0f),
          "from-centre on: the SAME anchor(100,100) is now the box's centre, not its corner");
    check(nearf((centre.x0 + centre.x1) * 0.5f, 100.0f, kTol) &&
              nearf((centre.y0 + centre.y1) * 0.5f, 100.0f, kTol),
          "from-centre on: the box's own midpoint is exactly the anchor");

    // Toggling back OFF, again with the identical anchor, must recover
    // `corner` exactly -- the "both directions" half of T10's requirement.
    const SelectionDragBox cornerAgain = computeSelectionDragBox(
        100.0f, 100.0f, 130.0f, 120.0f, 0.0f, 0.0f, false, false);
    check(boxEq(cornerAgain, corner.x0, corner.y0, corner.x1, corner.y1),
          "from-centre toggled back off recovers the corner box exactly (both directions work)");
  }

  // --- Shift+Option together: constrain, then centre the constrained square
  // (docs/testing-issues.md's modifier table, "Shift+Option -- both") ---
  {
    const SelectionDragBox both = computeSelectionDragBox(100.0f, 100.0f, 130.0f, 160.0f, 0.0f,
                                                           0.0f, true, true);
    // dx=30, dy=60 -> constrain to 60 keeping sign -> centre-anchored square
    // of side 120, i.e. anchor +/- 60 on both axes.
    check(boxEq(both, 40.0f, 40.0f, 160.0f, 160.0f),
          "Shift+Option: constrains to the larger delta (60), then centres that "
          "square on the anchor");
  }

  // --- Space-move, the offset in isolation ---
  //
  // Read the assertion below carefully, because the obvious one belongs in
  // the COUPLED section at the bottom of this file and NOT here. With the
  // cursor held still, raising the offset is not a Space-move at all -- it is
  // a state the call site cannot produce, since it derives the offset from
  // that very cursor. What the function guarantees in isolation is narrower:
  // the offset moves the anchor corner and leaves the cursor corner alone.
  // The "size is unchanged" property only exists once the two advance
  // together, which is what the coupled section drives.
  {
    const SelectionDragBox unmoved = computeSelectionDragBox(100.0f, 100.0f, 150.0f, 130.0f, 0.0f,
                                                              0.0f, false, false);
    const SelectionDragBox moved = computeSelectionDragBox(100.0f, 100.0f, 150.0f, 130.0f, 20.0f,
                                                            -10.0f, false, false);
    check(boxEq(unmoved, 100.0f, 100.0f, 150.0f, 130.0f),
          "space-move: a zero offset is exactly the plain two-corner box");
    // Anchor (100,100) + (20,-10) = (120,90); the cursor corner (150,130) is
    // untouched. Sorted, that is x:[120,150], y:[90,130].
    check(boxEq(moved, 120.0f, 90.0f, 150.0f, 130.0f),
          "space-move in isolation: the offset moves the ANCHOR corner only -- the "
          "cursor corner is the cursor, and offsetting it too would double-count the "
          "hand's movement (see SelectionDrag.hpp)");

    // The offset composes with constrain/from-centre too (Space can be held
    // at the same time as either), because both read the anchor after it has
    // been moved.
    const SelectionDragBox movedCentre = computeSelectionDragBox(
        100.0f, 100.0f, 130.0f, 120.0f, 20.0f, -10.0f, false, true);
    check(nearf((movedCentre.x0 + movedCentre.x1) * 0.5f, 120.0f, kTol) &&
              nearf((movedCentre.y0 + movedCentre.y1) * 0.5f, 90.0f, kTol),
          "space-move composes with from-centre: the box's midpoint is the "
          "MOVED anchor (100,100)+(20,-10), not the original anchor");
  }

  // --- updateSelectionMove(): the state machine behind Space-move, proving
  // repeated press/release during one drag is ADDITIVE, not a reset each
  // time (mirrors T10's "carry on from the moved origin" requirement, and is
  // Space's own version of Option's mid-drag-both-directions test above) ---
  {
    SelectionMoveState s;
    check(!s.active && nearf(s.offsetX, 0.0f, kTol) && nearf(s.offsetY, 0.0f, kTol),
          "updateSelectionMove: a freshly constructed state is inert");

    updateSelectionMove(s, /*spaceHeld=*/false, 10.0f, 10.0f);
    check(!s.active && nearf(s.offsetX, 0.0f, kTol),
          "updateSelectionMove: Space not held yet -- no change");

    updateSelectionMove(s, /*spaceHeld=*/true, 10.0f, 10.0f);
    check(s.active && nearf(s.offsetX, 0.0f, kTol) && nearf(s.offsetY, 0.0f, kTol),
          "updateSelectionMove: Space-down at the same point the cursor already sat -- "
          "zero delta so far, but now active");

    updateSelectionMove(s, true, 15.0f, 12.0f);
    check(s.active && nearf(s.offsetX, 5.0f, kTol) && nearf(s.offsetY, 2.0f, kTol),
          "updateSelectionMove: while held, the offset is the live delta from Space-down");

    updateSelectionMove(s, /*spaceHeld=*/false, 15.0f, 12.0f);
    check(!s.active && nearf(s.offsetX, 5.0f, kTol) && nearf(s.offsetY, 2.0f, kTol),
          "updateSelectionMove: Space released -- goes inactive, offset is KEPT, not reset");

    // Press again, from a DIFFERENT point, and move again: the second
    // segment must add onto the 5,2 already banked, not replace it and not
    // measure from the drag's very first Space-down.
    updateSelectionMove(s, true, 15.0f, 12.0f);
    updateSelectionMove(s, true, 20.0f, 10.0f);
    check(s.active && nearf(s.offsetX, 10.0f, kTol) && nearf(s.offsetY, 0.0f, kTol),
          "updateSelectionMove: a second Space-press adds its own delta onto the "
          "already-banked offset from the first (5+5=10, 2-2=0), proving press/release "
          "toggles both directions without losing the earlier move");

    updateSelectionMove(s, false, 20.0f, 10.0f);
    check(!s.active && nearf(s.offsetX, 10.0f, kTol) && nearf(s.offsetY, 0.0f, kTol),
          "updateSelectionMove: released again -- still inactive, final offset banked");
  }

  // --- The two above, COUPLED the way ui/MacPaintUI.cpp actually couples
  // them. This is the assertion that matters, and it is not implied by
  // either of the two sections above.
  //
  // The pure-function section varies `offset` with `cur` held still. That
  // pairing never occurs during a real Space-move: the call site feeds the
  // SAME live cursor to updateSelectionMove() (which derives the offset from
  // it) and to computeSelectionDragBox() (as the current point), so the two
  // arguments advance together. A formula can satisfy the decoupled test and
  // still resize the box on every frame of a real Space-move.
  //
  // So this drives the real loop: anchor at (100,100), drag out to
  // (200,160), press Space, and move. The box must translate and its size
  // must not change by a single texel.
  {
    const float ax = 100.0f, ay = 100.0f;
    SelectionMoveState s;
    float curX = 200.0f, curY = 160.0f;

    auto frame = [&](bool spaceHeld) {
      updateSelectionMove(s, spaceHeld, curX, curY);
      return computeSelectionDragBox(ax, ay, curX, curY, s.offsetX, s.offsetY, false, false);
    };

    const SelectionDragBox before = frame(false);
    const float w = before.x1 - before.x0;
    const float h = before.y1 - before.y0;

    frame(true);            // Space goes down where the cursor already is
    curX = 250.0f;          // ...and the hand moves, as it must to move the shape
    curY = 140.0f;
    const SelectionDragBox during = frame(true);

    check(nearf(during.x1 - during.x0, w, kTol) && nearf(during.y1 - during.y0, h, kTol),
          "space-move, COUPLED as the call site couples it: moving the cursor with "
          "Space held translates the box and leaves its SIZE unchanged");
    check(nearf(during.x0 - before.x0, 50.0f, kTol) &&
              nearf(during.y0 - before.y0, -20.0f, kTol),
          "space-move, coupled: the box translated by exactly the cursor's delta");

    // Release and keep dragging: the moving corner must still be under the
    // cursor, or the shape jumps the instant Space comes up.
    // The drag runs down-and-right from the moved anchor (150,80) to the
    // cursor (250,140), so the cursor is the (x1,y1) corner. Asserted as
    // "some corner is the cursor" rather than naming one, because which
    // corner the cursor occupies depends on the drag's direction and this
    // property has to hold for all four quadrants.
    const SelectionDragBox atRelease = frame(false);
    const bool cursorIsACorner =
        (nearf(atRelease.x0, curX, kTol) || nearf(atRelease.x1, curX, kTol)) &&
        (nearf(atRelease.y0, curY, kTol) || nearf(atRelease.y1, curY, kTol));
    check(cursorIsACorner,
          "space-move, coupled: on release the moving corner is still exactly under "
          "the cursor, so carrying on drawing does not jump");
  }

  std::printf("[selftest] selection drag %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
