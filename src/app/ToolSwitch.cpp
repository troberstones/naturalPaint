#include "app/ToolSwitch.hpp"

#include "app/MeasureLine.hpp"

namespace np {

void setActiveTool(AppState& st, Tool next) noexcept {
  // The tool the user is *leaving*, which is not always `brush.tool`: while
  // Space is held `brush.tool` is the borrowed Hand and the tool the user
  // actually has selected is `springReturn` (header §2). Recording the Hand
  // here would put a tool the user never picked into the ledger.
  const Tool outgoing = st.tools.springHeld ? st.tools.springReturn : st.brush.tool;

  // A deliberate pick beats a borrow, and ends it. Cleared before the write
  // below rather than after, so that the `next == outgoing` early return
  // cannot leave the borrow flagged with `brush.tool` already handed over --
  // a state in which a later Space release would install `springReturn` on
  // top of the user's fresh choice.
  st.tools.springHeld = false;

  // Unconditional, and ahead of the early return: when a borrow was in flight
  // this is also what un-installs the Hand, and `next == outgoing` is exactly
  // the case where `brush.tool` (== Hand) still differs from `next`.
  st.brush.tool = next;

  // Picking the tool that is already selected is not a switch. The palette
  // cell, the flyout row and the menu item can all deliver one, and treating
  // it as a switch would overwrite the previous tool with itself -- a
  // Hand -> Hand pick losing the real previous is the concrete loss.
  if (next == outgoing) return;

  st.tools.previous = outgoing;
  st.tools.hasPrevious = true;
}

bool hasPreviousTool(const AppState& st) noexcept { return st.tools.hasPrevious; }

Tool previousTool(const AppState& st) noexcept { return st.tools.previous; }

Tool effectiveTool(const AppState& st) noexcept {
  return st.tools.springHeld ? st.tools.springReturn : st.brush.tool;
}

bool springHandHeld(const AppState& st) noexcept { return st.tools.springHeld; }

bool beginSpringHand(AppState& st) noexcept {
  // Auto-repeat, or a second press event with no intervening release (a
  // window that lost and regained focus with the key down will deliver
  // one). Re-borrowing would overwrite `springReturn` with the Hand already
  // installed, and the release would then strand the user in it.
  if (st.tools.springHeld) return false;
  st.tools.springHeld = true;
  st.tools.springReturn = st.brush.tool;
  st.brush.tool = Tool::Hand;
  // Deliberately does NOT touch `previous`/`hasPrevious` -- header §1.
  return true;
}

bool endSpringHand(AppState& st) noexcept {
  if (!st.tools.springHeld) return false;
  st.tools.springHeld = false;
  st.brush.tool = st.tools.springReturn;
  return true;
}

float transformSeedAngleDeg(const AppState& st, uint64_t activeDocumentId) noexcept {
  // Both halves of the conditional, in the order that makes the zero case
  // obvious: the wrong tool is zero, and so is a ruler that is not about this
  // document. Header §3 is why the tool test is `effectiveTool()` and not
  // `previousTool()`.
  if (effectiveTool(st) != Tool::Measure) return 0.0f;
  if (!measureLineAppliesTo(st.measure, activeDocumentId)) return 0.0f;
  return measureReadout(st.measure).angleDeg;
}

}  // namespace np
