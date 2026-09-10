#include "app/ToolSwitch.hpp"

#include "app/CropTool.hpp"
#include "app/MeasureLine.hpp"

namespace np {

namespace {

// `setActiveTool()` with the gizmo's refusal (ToolSwitch.hpp section 5) already
// answered -- the whole of the old `setActiveTool()`, unchanged.
//
// It exists so that `enterTransformTool()` can reach the ledger, the crop
// cancel and the flats clear WITHOUT going through the check, which it must:
// by the time it is called the session it belongs to is already live, so the
// public setter would refuse the one switch that is not the user changing
// their mind. Putting the exemption in a private entry point rather than in a
// `bool force` argument keeps a caller from ever asking for it by accident --
// there is exactly one, in this file, four lines below.
void installTool(AppState& st, Tool next) noexcept {
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

  // **A deliberate ordinary pick leaves flatting mode.** The flatting tools
  // are a second sticky mode layered over the same canvas clicks, so without
  // this one line "what does a click mean" would have two answers at once --
  // the user picks the Brush from TOOLS, drags on a Flats layer, and gets a
  // bridge stroke because DELETE was still lit in a palette they may not
  // even have on screen. Cleared here, in the one place that already owns
  // this question, rather than in each of the palette's own callers.
  //
  // Not conditional on `next == outgoing`: re-picking the tool you already
  // have is exactly how a user says "stop doing the other thing".
  st.flatsTool = FlatsTool::None;

  // Picking the tool that is already selected is not a switch. The palette
  // cell, the flyout row and the menu item can all deliver one, and treating
  // it as a switch would overwrite the previous tool with itself -- a
  // Hand -> Hand pick losing the real previous is the concrete loss.
  if (next == outgoing) return;

  st.tools.previous = outgoing;
  st.tools.hasPrevious = true;

  // **A pending crop does not survive a tool change.** It is the polygon
  // lasso's rule (`ui/MacPaintUI.cpp`: "switching away mid-path abandons it.
  // Leaving it live would resume a stale path on return, with vertices the
  // user has long forgotten placing") and the stakes here are higher: a crop
  // left armed would reappear the next time Crop is chosen, and the next Enter
  // would destroy a document to a rectangle drawn some time ago.
  //
  // Placed after the early return above on purpose -- picking Crop while Crop
  // is already selected is not a switch, and must not throw away the rectangle
  // the user is in the middle of adjusting. `next` is not tested at all: a
  // crop is discarded when the user leaves the crop tool AND when they arrive
  // at it, and arriving with a stale shape from before is the same defect
  // wearing the other hat.
  cropCancel(st.crop);
}

}  // namespace

const char* toolChangeRefusal(const AppState& st) noexcept {
  if (!st.transform.active()) return nullptr;
  // The document scoping ToolSwitch.hpp section 5 argues for, spelled the same
  // way `ui/MacPaintUI.cpp`'s `transformOnThisDoc` spells it. A session on a
  // document the user has tabbed away from draws no gizmo and offers no key
  // that ends it, and locking the palette from behind it would be a modal
  // state with no visible dialog and no way out.
  const OpenDocument* od = st.documents.active();
  if (od == nullptr || st.transform.documentId() != od->id) return nullptr;
  // The wording is `ui/MacPaintUI.cpp`'s own, from the numeric Transform
  // dialog's refusal of a second session -- the same state, already given a
  // sentence, and a second phrasing for it would be two voices for one fact.
  return "A transform is in progress. Press Return to apply it or Escape to cancel it.";
}

bool setActiveTool(AppState& st, Tool next) noexcept {
  if (toolChangeRefusal(st) != nullptr) return false;
  installTool(st, next);
  return true;
}

bool enterTransformTool(AppState& st) noexcept {
  // `effectiveTool()`, not `brush.tool`: with Space held the installed tool is
  // the borrowed Hand and the tool the user is actually in is `springReturn`.
  // Reporting "changed" off the Hand would be answering about the borrow.
  // `installTool()` below ends that borrow either way, which is right -- a
  // gizmo is up, and the pan the user was in the middle of is over.
  const bool changed = effectiveTool(st) != Tool::Move;
  // `installTool()`, not `setActiveTool()`: the session is already live by the
  // time this is called (every call site checks the begin succeeded first), so
  // the public setter would refuse it. See `installTool()`'s own comment.
  installTool(st, Tool::Move);
  return changed;
}

bool hasPreviousTool(const AppState& st) noexcept { return st.tools.hasPrevious; }

Tool previousTool(const AppState& st) noexcept { return st.tools.previous; }

Tool effectiveTool(const AppState& st) noexcept {
  if (st.tools.springHeld) return st.tools.springReturn;
  if (st.tools.springEyedropperHeld) return st.tools.springEyedropperReturn;
  return st.brush.tool;
}

bool springHandHeld(const AppState& st) noexcept { return st.tools.springHeld; }

bool beginSpringHand(AppState& st) noexcept {
  // Auto-repeat, or a second press event with no intervening release (a
  // window that lost and regained focus with the key down will deliver
  // one). Re-borrowing would overwrite `springReturn` with the Hand already
  // installed, and the release would then strand the user in it.
  if (st.tools.springHeld) return false;
  // The other spring holds `brush.tool` right now -- refusing here rather
  // than stealing it out from under the Eyedropper borrow is what keeps the
  // two mutually exclusive by construction (ToolSwitch.hpp's own comment on
  // `beginSpringEyedropper()`), and it is a real sequence: Alt held with the
  // mouse still up borrows the Eyedropper, and Space pressed before Alt is
  // released would otherwise install the Hand over it with nothing left
  // remembering which tool either borrow started from.
  if (st.tools.springEyedropperHeld) return false;
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

bool springEyedropperHeld(const AppState& st) noexcept { return st.tools.springEyedropperHeld; }

bool springEyedropperEligible(Tool t, BucketFill fill) noexcept {
  switch (t) {
    // The paint-tool family: nothing here already has a use for a bare Alt.
    case Tool::Brush:
    case Tool::Water:
    case Tool::DryBrush:
    case Tool::Pencil:
    case Tool::Eraser:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Smudge:
      return true;
    // ADR-0009: the Flats-mode bucket spends Alt carving a new fill out of a
    // leaked area (AppState.hpp's `kBucketFills` row for it says so); only
    // the ordinary Colour-tolerance bucket has Alt to lend.
    case Tool::PaintBucket:
      return fill == BucketFill::Colour;
    // Every one of these already has a live, shipped meaning for Alt, or
    // reads the canvas by a different gesture entirely -- listed rather than
    // caught by a `default:` so `-Wswitch` still catches a `Tool` this
    // predicate has not been told about, the same house style
    // `ui/ToolCursor.cpp`'s `cursorForTool()` uses.
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::CloneStamp:
    case Tool::Gradient:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      return false;
  }
  // Unreachable for any real `Tool` value -- see `cursorForTool()`'s own
  // identical trailing return for why a switch that already lists every
  // enumerator still needs one.
  return false;
}

bool beginSpringEyedropper(AppState& st) noexcept {
  if (st.tools.springEyedropperHeld) return false;
  // Mirrors `beginSpringHand()`'s own guard just above, in the other
  // direction: the Hand is already installed in `brush.tool`, and this
  // borrow must not overwrite it out from under that one.
  if (st.tools.springHeld) return false;
  // The business rule, not a UI gesture-priority guard -- checked here, once,
  // rather than at the `ui/MacPaintUI.cpp` call site, so `--selftest` can
  // assert "a begin while ineligible is refused" through this function alone
  // and a second call site can never diverge from it. `effectiveTool()`
  // rather than `st.brush.tool`: identical whenever this line is reached
  // (the guard above already refused a live Hand-borrow), but it is the one
  // spelling that stays correct if a future caller ever reaches here through
  // a borrow this file does not yet know about.
  if (!springEyedropperEligible(effectiveTool(st), st.bucketFill)) return false;
  st.tools.springEyedropperHeld = true;
  st.tools.springEyedropperReturn = st.brush.tool;
  st.brush.tool = Tool::Eyedropper;
  // Deliberately does NOT touch `previous`/`hasPrevious` -- same reasoning
  // as the Hand's borrow, header §1.
  return true;
}

bool endSpringEyedropper(AppState& st) noexcept {
  if (!st.tools.springEyedropperHeld) return false;
  st.tools.springEyedropperHeld = false;
  st.brush.tool = st.tools.springEyedropperReturn;
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

bool setFlatsTool(AppState& st, FlatsTool next) noexcept {
  // Its own check rather than `setActiveTool()`'s, because the switch at the
  // bottom of this function writes `brush.tool` directly -- ToolSwitch.hpp
  // section 5's second paragraph on this function says why it has to.
  if (toolChangeRefusal(st) != nullptr) return false;
  st.flatsTool = next;
  // Picking a flatting tool cancels a half-finished two-click merge: the
  // armed point belongs to the gesture being abandoned, and carrying it into
  // the next one would merge two fills the user never paired.
  st.flatsMergeFirst.reset();
  if (next == FlatsTool::None) return true;

  // **The host tool, per ADR-0009's table.** The flatting gestures are the
  // existing tools scoped to a layer kind, not a parallel set: a bridge IS
  // the Pencil, a group IS the Lasso. Setting the host here is what keeps
  // the cursor, the options row and the canvas's own drag handling agreeing
  // with the palette -- picking BRIDGE and then finding the Marquee's
  // rubber-band on screen would be the palette lying about what it did.
  //
  // Written straight to `brush.tool` rather than through `setActiveTool()`,
  // and that is deliberate: `setActiveTool()` clears `flatsTool` (above), so
  // routing through it here would undo the line before it. The ledger is not
  // touched for the same reason -- the user picked a flatting tool, not the
  // Pencil, and "previous tool" should take them back to whatever they were
  // using before flatting.
  switch (next) {
    case FlatsTool::BridgePen:    st.brush.tool = Tool::Pencil; break;
    case FlatsTool::BridgeEraser: st.brush.tool = Tool::Eraser; break;
    case FlatsTool::Group:
    case FlatsTool::ShapeFill:    st.brush.tool = Tool::Lasso; break;
    case FlatsTool::Carve:        st.brush.tool = Tool::PaintBucket; break;
    // DeleteFill, MergePair, DrawMerge and SelectEdits have no host in
    // ADR-0009's table -- they are bare canvas clicks and drags, and the
    // flats route below takes the event before any tool sees it. Leaving
    // `brush.tool` alone means the tool the user had is still theirs when
    // they leave flatting mode.
    case FlatsTool::DeleteFill:
    case FlatsTool::MergePair:
    case FlatsTool::DrawMerge:
    case FlatsTool::SelectEdits:
    case FlatsTool::None:         break;
  }
  return true;
}

}  // namespace np
