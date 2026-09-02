#include "app/MoveTool.hpp"

namespace np {

bool toolMovesPixels(Tool tool) noexcept { return tool == Tool::Move; }

MoveTarget moveTargetFor(const OpenDocument& od) noexcept {
  // The header's section 2. `has_value()`, not "covers anything": an empty
  // selection is refused by name at begin time, which says more than
  // silently promoting the gesture to a whole-layer move would.
  return od.selection.has_value() ? MoveTarget::SelectionPixels : MoveTarget::WholeLayer;
}

TransformBeginResult beginMove(TransformSession& session, const OpenDocument& od) {
  TransformBeginResult r;
  const std::optional<size_t> li = activeLayerIndex(od);
  if (!li) {
    r.error = "move refused: this document has no layer to move.";
    return r;
  }
  return moveTargetFor(od) == MoveTarget::SelectionPixels
             ? session.beginSelectionPixels(od, *od.selection, *li)
             : session.beginLayer(od, *li);
}

void setMoveTranslation(TransformSession& session, float dx, float dy) noexcept {
  session.setPending(transformTranslate(dx, dy));
}

TransformCommitResult nudgeMove(OpenDocument& od, float dx, float dy) {
  TransformSession session;
  const TransformBeginResult began = beginMove(session, od);
  if (!began.ok) {
    TransformCommitResult out;
    out.error = began.error;
    return out;
  }
  setMoveTranslation(session, dx, dy);
  return session.commit(od);
}

}  // namespace np
