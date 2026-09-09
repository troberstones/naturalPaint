#include "app/ToolSwitch.hpp"

#include "app/CropTool.hpp"
#include "app/DocumentLifecycle.hpp"  // activeLayerOf()
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
  clearFlatsEditSelection(st);

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

void clearFlatsEditSelection(AppState& st) noexcept {
  st.flatsEditSelection.clear();
  st.flatsEditBox.reset();
  st.flatsEditBoxAdditive = false;
}

void setFlatsTool(AppState& st, FlatsTool next) noexcept {
  st.flatsTool = next;
  // **Any tool change drops the selection**, including picking SELECT EDITS
  // again. A selection is a set of `flatEditKey()` values, which mean nothing
  // except against the edit list they were picked from -- and a highlight
  // still burning on the canvas while a different tool is armed reads as
  // "Delete will remove these", which by then is no longer true.
  clearFlatsEditSelection(st);
  // Picking a flatting tool cancels a half-finished two-click merge: the
  // armed point belongs to the gesture being abandoned, and carrying it into
  // the next one would merge two fills the user never paired.
  st.flatsMergeFirst.reset();
  if (next == FlatsTool::None) return;

  // **The regular toolbox is deliberately left alone.**
  //
  // This used to install a host tool per ADR-0009's table -- BRIDGE set the
  // Pencil, GROUP set the Lasso -- on the reasoning that the flatting
  // gestures ARE those tools scoped to a layer kind, so the cursor and the
  // options row should agree with the palette. In use that reasoning is
  // wrong, and the user named it: picking a flats tool visibly moved the
  // selection in the tool palette, so one click lit a cell in each of two
  // palettes and put the app in a state neither of them described on its
  // own. Worse, leaving flatting mode then dropped the user on a tool they
  // never chose.
  //
  // The two palettes are now independent, which is only safe because the
  // flats canvas route owns every one of these gestures itself --
  // ui/MacPaintUI's route takes the pointer before the ordinary tools get
  // it, including the lasso path GROUP and SHAPE need. Before that route
  // owned the lasso, removing the host tool here would have silently killed
  // those two: their commit lived inside `case Tool::Lasso:` and ran only
  // while the Lasso was the active tool.
  //
  // So there is nothing left to do here but set the mode.
}

bool flatsToolIsActive(const AppState& st) {
  // **The PICK decides, not the selected layer.**
  //
  // This was gated on a Flats layer being selected and unlocked, on the
  // reasoning that a tool which cannot act should not claim to be active.
  // That reasoning produced the exact defect it was meant to avoid, and the
  // user found it twice: with a flatting tool picked and an ordinary layer
  // selected, `flatsToolIsActive()` was false, so TOOLS re-lit its cell while
  // the FLATS TOOLS palette went on showing its own picked cell accented --
  // two tools active at once -- and the ordinary tool really did still paint.
  // The same ambiguity is why DELETE appeared to do nothing: the flats route
  // stood down whenever the layer was wrong, so a click on a fill went
  // nowhere while the palette insisted DELETE was armed.
  //
  // So a flatting tool is active from the moment it is picked until something
  // else is picked, exactly as a brush is. Whether it can ACT on the current
  // layer is a separate question, answered on the click with a refusal that
  // names what to do -- not by silently handing the canvas back to a tool the
  // user did not choose.
  return st.flatsTool != FlatsTool::None;
}

}  // namespace np
