#include "app/SelectionDrag.hpp"

#include <algorithm>
#include <cmath>

namespace np {

void updateSelectionMove(SelectionMoveState& state, bool spaceHeld, float curX,
                          float curY) noexcept {
  if (spaceHeld && !state.active) {
    // Space just went down: remember where from, so the delta below is
    // measured against this instant rather than against wherever the
    // pointer happened to be when the drag started.
    state.active = true;
    state.refX = curX;
    state.refY = curY;
  } else if (!spaceHeld && state.active) {
    // Space just came up: bank the offset this segment produced into
    // `baseX/Y` so the *next* Space-press (if any) adds on top of it
    // instead of measuring from the drag's original anchor again.
    state.active = false;
    state.baseX = state.offsetX;
    state.baseY = state.offsetY;
  }
  if (state.active) {
    state.offsetX = state.baseX + (curX - state.refX);
    state.offsetY = state.baseY + (curY - state.refY);
  }
  // While inactive, offsetX/Y already equals baseX/Y from the segment that
  // just ended (or the {0,0} it was constructed with, if Space has never
  // been pressed this drag) -- nothing to do.
}

SelectionDragBox computeSelectionDragBox(float anchorX, float anchorY, float curX, float curY,
                                          float offsetX, float offsetY, bool constrainSquare,
                                          bool fromCentre) noexcept {
  // Space-move: the offset moves the ANCHOR, and the current point stays the
  // live cursor. Not a typo for symmetry -- SelectionDrag.hpp's opening note
  // works it through. In one line: the caller derives `offsetX/Y` from the
  // same cursor it passes as `curX/Y`, so offsetting both would count the
  // hand's movement twice on the moving corner and grow the box every frame.
  // Offsetting the anchor alone still shifts both corners by exactly the
  // distance moved, because the moving corner is the cursor and the cursor
  // has already moved.
  const float ax = anchorX + offsetX;
  const float ay = anchorY + offsetY;
  const float cx = curX;
  const float cy = curY;

  float dx = cx - ax;
  float dy = cy - ay;
  if (constrainSquare) {
    // The larger of the two deltas, keeping each axis's OWN sign via
    // std::copysign -- not the sign of whichever axis happened to be
    // larger -- so a drag up-and-slightly-right stays up-and-right after
    // constraining rather than jumping to up-and-left because |dy| won the
    // max(). std::copysign reads the sign bit of its second argument even
    // when that argument is exactly 0.0f (positive by default, matching a
    // drag that has not moved on that axis yet), so no dx==0/dy==0 special
    // case is needed.
    const float d = std::max(std::fabs(dx), std::fabs(dy));
    dx = std::copysign(d, dx);
    dy = std::copysign(d, dy);
  }

  float x0, y0, x1, y1;
  if (fromCentre) {
    // The anchor is the centre: the box extends the (possibly constrained)
    // delta on both sides of it.
    x0 = ax - dx;
    y0 = ay - dy;
    x1 = ax + dx;
    y1 = ay + dy;
  } else {
    // The anchor is a corner: the box is anchor and anchor+delta.
    x0 = ax;
    y0 = ay;
    x1 = ax + dx;
    y1 = ay + dy;
  }

  SelectionDragBox box;
  box.x0 = std::min(x0, x1);
  box.y0 = std::min(y0, y1);
  box.x1 = std::max(x0, x1);
  box.y1 = std::max(y0, y1);
  return box;
}

}  // namespace np
