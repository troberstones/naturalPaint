#pragma once

#include <optional>

#include "app/AppState.hpp"          // Tool
#include "app/DocumentLifecycle.hpp"  // OpenDocument, activeLayerIndex()
#include "app/TransformSession.hpp"

// app/MoveTool -- `Tool::Move` (docs/ui.md section 2's palette cell, shortcut
// `V`; PRD D14/D16 by way of app/TransformSession).
//
// ==========================================================================
// 1. THIS FILE CONTAINS NO TRANSLATION ENGINE, ON PURPOSE
// ==========================================================================
//
// Everything a move needs was already built and already proven: a session
// that accumulates a matrix and writes nothing until `commit()`
// (app/TransformSession), a resampler with an exact no-kernel path for an
// integer translate (ops/Transform, PRD D15), a live pixel preview
// (ui/TransformPreviewTexture) and a composite split that draws the layers
// above the moving one back in front of it (ui/TransformCompositeSplit).
// The Move tool is Free Transform with the scale handles, the rotate
// affordance and the Return/Escape modality taken away -- so writing a
// second translation path here would be building a second thing that can
// disagree with the first about what "moved by (dx, dy)" means, and a second
// thing to keep on the exact path.
//
// What this file *is*, therefore, is the four decisions a Move gesture has
// to make that a Free Transform gesture does not, each of them in one
// testable, headless place rather than inline in `ui/MacPaintUI.cpp` where
// `--selftest` cannot reach it:
//
//   * which target a pen-down acts on (section 2),
//   * that a refusal is surfaced rather than swallowed (section 3),
//   * what the keyboard form of the same gesture is (section 4),
//   * and which of `toolHasCanvasHandler()`'s gates Move belongs to
//     (section 5).
//
// ==========================================================================
// 2. DECISION: A SELECTION MOVES THE SELECTED PIXELS; NO SELECTION MOVES THE
//    WHOLE LAYER -- AND IT IS *THE SAME* RULE FREE TRANSFORM USES
// ==========================================================================
//
// `moveTargetFor()` is `od.selection ? SelectionPixels : WholeLayer`, which
// is Photoshop's own rule and the one a user who has just drawn a marquee
// expects: always moving the whole layer would silently ignore a selection
// they made on purpose.
//
// The part worth writing down is not the rule but the fact that it is
// **shared**. `ui/MacPaintUI.cpp`'s `st.requestFreeTransform` block spelled
// this branch inline; the Move tool now asks this function, and that block
// could too. Two tools that both move pixels and disagree about whether a
// selection scopes the move would be a genuinely nasty defect to diagnose --
// the user's mental model of "what am I about to move" would depend on which
// of two commands they reached for -- so the branch is one function with one
// answer. Note in particular what is NOT here: no "fall back to the whole
// layer if the selection turns out to cover nothing". A selection that
// selects nothing makes `beginSelectionPixels()` refuse **by name**
// ("the selection covers no pixels"), and that sentence is a truer answer
// than silently moving the entire layer instead of the nothing the user had
// selected.
//
// ==========================================================================
// 3. DECISION: A REFUSED MOVE IS A SENTENCE, NEVER A DEAD DRAG
// ==========================================================================
//
// `TransformSession::beginLayer()` refuses a locked layer, an out-of-range
// index, a layer kind that holds no pixels at all (Adjustment/Text/Group/...)
// and a layer whose storage is empty; `beginSelectionPixels()` additionally
// refuses a Pigment layer and an empty selection. Every one of those refusals
// arrives as prose in `TransformBeginResult::error`.
//
// `beginMove()` forwards that prose unchanged and returns `ok == false`, and
// its one production caller puts it in the status band and **does not start a
// drag**. That ordering matters: a drag that began on a refused target would
// upload a preview, hide the layer from the composite through
// ui/TransformCompositeSplit, move a picture around under the pointer for as
// long as the button was held, and then commit nothing -- which is a far
// worse lie than no motion at all. So the refusal is checked before any of
// that starts, not after.
//
// **Rejected: a slashed-circle cursor over a refusing layer.**
// `ui/ToolCursor.cpp` does exactly that for the bucket and the brush, via
// `pixelOpRefusalFor()` / `strokeRouteFor()`, and it would be the better
// affordance here too -- the refusal would arrive before the gesture is
// spent rather than after. It is not built because neither of those two
// predicates answers Move's question: Move accepts a Pigment layer for a
// whole-layer move (which `pixelOpRefusalFor()` refuses) and refuses an
// empty layer (which it accepts), so wiring the cursor to either would make
// it lie in both directions. The honest version needs the layer-level half
// of `beginLayer()`'s refusal ladder lifted out of app/TransformSession into
// a predicate both can call, which is a change to that file's public shape
// and is deliberately left undone here rather than approximated. Recorded so
// the next reader knows the cursor's silence is a known gap, not an
// oversight.
//
// ==========================================================================
// 4. DECISION: ARROW KEYS NUDGE, AND EACH PRESS IS ITS OWN UNDO STEP
// ==========================================================================
//
// The arrow keys are the conventional keyboard form of this tool, and
// `nudgeMove()` is it: begin, translate by whole pixels, commit -- one call,
// no session held between presses.
//
// A press is one history entry rather than a run coalesced into one. That is
// the more expensive choice and it is taken deliberately: `core::History`
// has no notion of "extend the entry I just made", every other edit in this
// build is one entry per gesture, and inventing a coalescing rule here
// (which presses merge? after what idle gap? across a tool change?) is a
// behaviour to design, not a detail to guess. Ten presses giving ten undo
// steps is predictable; ten presses giving somewhere between one and ten
// depending on typing speed is not.
//
// Nudging is **always lossless**: the translation is whole pixels, so
// `exactRemapKind()` classifies it as the identity permutation and
// `ops::transformImage()` takes PRD D15's no-kernel path. Ten nudges are ten
// exact copies, not ten Catmull-Rom convolutions -- which is the property
// that makes nudge-nudge-nudge-back-again a genuinely free operation and is
// asserted, not assumed, in `app/selftest/MoveTool.cpp`.
//
// The `10.0f` "big nudge" step Shift conventionally selects is the caller's
// business, not this file's -- `nudgeMove()` takes floats and does not name
// a step size, for the same reason app/TransformSession names no zoom.
//
// ==========================================================================
// 5. DECISION: MOVE GETS A SEVENTH GATE, NOT A WIDENED SIXTH
// ==========================================================================
//
// `toolHasCanvasHandler()` (ui/AtelierChrome.cpp) is six OR'd predicates,
// and `--selftest` asserts `toolImplemented(t) == toolHasCanvasHandler(t)`
// for every tool. Move fits none of the six:
//
//   `toolWritesRgbPixels()`   is the paint-bucket/gradient gate, and it is
//                             read in two places, not one:
//                             `ui/MacPaintUI.cpp`'s fill block (which routes
//                             through `pixelOpRefusalFor()` and
//                             ops/FloodFill) and `ui/ToolCursor.cpp:160`
//                             (which hands out that family's cursor and
//                             refusal). Move writes pixels, but through
//                             `TransformSession::commit()`, refuses on
//                             different grounds, and has its own
//                             `ToolCursor::MoveObject`. Widening this
//                             predicate would give Move the bucket's
//                             refusal logic and the bucket's cursor -- the
//                             exact "wired by widening an existing
//                             predicate rather than adding a new one"
//                             mistake app/selftest/Eyedropper.cpp's Zoom
//                             assertion was written to catch.
//   `toolBeginsStroke()`      is `strokeRouteFor()`'s own answer. Move has
//                             no route and lays down no dab; a stroke row
//                             for it would have to be a lie to make this
//                             true.
//   `toolDrawsSelection()`    builds a `Selection` by gesture. Move *carries*
//                             the active selection along with the pixels
//                             (app/TransformSession section 3) but creates
//                             none.
//   `toolSamplesCanvas()`     reads colour and writes none.
//   `toolPansView()`          moves the VIEW. Move moves the CONTENT, and
//                             conflating those two is the single most
//                             confusable pair in the whole palette --
//                             `ui/ToolCursor.cpp` already keeps `Pan` and
//                             `MoveObject` apart as distinct intents for
//                             this reason, and this predicate must too.
//   `toolZoomsView()`         likewise the view.
//
// So `toolMovesPixels()` below is the seventh gate, in the same shape as the
// other six: it is the literal expression `ui/MacPaintUI.cpp`'s Move block
// is gated on, so a Move block that is deleted or disabled makes the
// predicate false and reddens the completeness check, rather than leaving a
// palette cell that highlights and does nothing.
namespace np {

// Whether `tool` repositions existing pixels by dragging them: `Tool::Move`,
// and today nothing else. Section 5 is the argument for this being its own
// predicate rather than a term added to one of the other six.
bool toolMovesPixels(Tool tool) noexcept;

// Which of `TransformSession`'s two targets a Move gesture on `od` acts on.
// Section 2.
enum class MoveTarget { WholeLayer, SelectionPixels };

MoveTarget moveTargetFor(const OpenDocument& od) noexcept;

// Begins a Move on `od`'s active layer, through `session`, against the
// target `moveTargetFor()` chose. `session` is left untouched and inactive
// on refusal, and the returned `error` is the sentence a caller must show
// (section 3).
//
// Takes the `OpenDocument` rather than its `Document` for
// `TransformSession::beginLayer()`'s own stated reason: the session records
// which document it belongs to, so a commit cannot land in another one.
TransformBeginResult beginMove(TransformSession& session, const OpenDocument& od);

// One frame of a Move drag: `session`'s pending matrix becomes the PURE
// translation `(dx, dy)`, in document pixels.
//
// Written as an absolute `transformTranslate()` of the whole gesture's
// offset rather than as an incremental multiply of the previous frame's
// matrix -- app/TransformSession section 6's rule, for the reason given
// there (repeated floating-point composition drifts) and for one more that
// is specific to this tool: an absolute translate is *structurally* a
// translate, so no accumulation of drag frames can ever introduce a scale or
// a shear that would knock the commit off PRD D15's exact path. A drag that
// ends on whole-pixel coordinates is lossless no matter how many frames it
// took to get there.
//
// A no-op when no session is active, matching `setPending()`'s own contract.
void setMoveTranslation(TransformSession& session, float dx, float dy) noexcept;

// The keyboard form: move `od`'s active layer (or its selected pixels) by
// `(dx, dy)` and commit, in one call and one undo step. Section 4.
//
// Uses its own private `TransformSession` -- a nudge has no interactive
// state to keep between calls, and borrowing the UI's session would leave a
// half-live gizmo behind if the commit refused. Refusals come back in
// `error` exactly as `beginMove()`'s do, and leave `od` untouched.
TransformCommitResult nudgeMove(OpenDocument& od, float dx, float dy);

}  // namespace np
