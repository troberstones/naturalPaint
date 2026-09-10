#pragma once

#include <cstdint>

#include "app/AppState.hpp"

// app/ToolSwitch -- the one writer of `AppState::brush.tool`, and the two
// things that only a single writer can answer.
//
// ==========================================================================
// 0. Why this file exists at all
// ==========================================================================
//
// Two reports need the same missing fact, *which tool was active before this
// one*:
//
//   T20 "space bar should switch to the hand tool while held down and go back
//        to the previous tool when released."
//   T24 "the measure tool's angle should be remembered so that when the
//        transform panel is open, and the measure was the last tool, the angle
//        from the measure is put into the transform angle field; if it wasn't
//        the last tool the angle should be zero."
//
// Nothing in the build recorded it. `brush.tool` was assigned from four
// places -- `ui/MacPaintUI.cpp`'s palette cell, its flyout row and its
// `MenuAction::ToolItem` arm, plus `src/main.cpp`'s demo setup -- and the
// obvious fix is to write `st.tools.previous = st.brush.tool;` above each of
// the four. That is exactly the shape this codebase has just paid for: the
// gradient tool never once committed a ramp because `marqueeDragging` had
// three independent writers and a sibling tool's `else` arm cleared it every
// frame. Four writers of a *derived* fact is worse than three writers of a
// raw one, because the fifth assignment site (there is always a fifth) is
// added by someone who has never read this comment and the ledger silently
// starts lying instead of visibly breaking.
//
// So: one setter, and `brush.tool` is written nowhere outside this
// translation unit. `--selftest` cannot press a key or open a modal, which is
// the other half of the argument -- lifting these decisions out of the canvas
// block is what makes them assertable at all.
//
// ==========================================================================
// 1. The spring-loaded Hand is a BORROW, not a switch
// ==========================================================================
//
// It has its own pair of functions here rather than going through
// `setActiveTool()`, and the reason is that routing it through the setter
// makes the feature erase itself:
//
//   Space down -> setActiveTool(Hand): previous = Brush, tool = Hand
//   Space up   -> setActiveTool(previous = Brush): previous = HAND, tool = Brush
//
// After one pan the ledger says the user's previous tool is the Hand -- a
// tool they never chose -- and after a second pan it says it again. The whole
// point of T20 is that a spring-load leaves no trace, and a ledger entry is a
// trace. The rejected alternative was to special-case `Tool::Hand` inside
// `setActiveTool()` ("never record the Hand as previous"), which is worse in
// both directions: it also breaks a user who *deliberately* picks the Hand
// from the palette and then picks something else, and it hides a rule about
// one gesture inside a function that knows nothing about gestures.
//
// What it does NOT do is write `brush.tool` from the UI. Section 0's rule is
// "one writer", and the writer is this file, not this function -- so the
// borrow lives here, beside the setter, where the two can be read together
// and where `setActiveTool()` can end a borrow that a deliberate pick has
// overtaken (section 2).
//
// ==========================================================================
// 2. Picking a tool while Space is held
// ==========================================================================
//
// Reachable: hold Space to pan the canvas into view, then click a palette
// cell with the other hand before letting go. The pick must win -- restoring
// the borrowed-from tool half a second later would throw away a deliberate
// choice -- so `setActiveTool()` ends the borrow, and records the tool the
// user was actually in (`springReturn`) as the previous one rather than the
// Hand that happened to be installed at that instant. Everything the user did
// deliberately is in the ledger; nothing the spring did is.
//
// ==========================================================================
// 3. T24's predicate is NOT "previous == Measure", and that is a finding
// ==========================================================================
//
// The obvious reading of T24 is `previousTool(st) == Tool::Measure`. It is
// dead code. `ui/MacPaintUI.cpp`'s Measure handler ends with an `else if
// (st.measure.active) clearMeasureLine(st.measure);` -- **every frame the
// Measure tool is not the active one, the ruler is destroyed**. By the time
// Measure is the *previous* tool there is no line left, `measureLineAppliesTo()`
// is false, and a `previous == Measure` seed would return zero on one hundred
// per cent of the states the running application can actually produce. It
// would look like a shipped feature and never once fire, and the only way to
// write a passing assertion for it would be to hand-build an `AppState` the
// app cannot reach -- the "green assertion, dead probe" failure this project
// has already been bitten by.
//
// What the report is actually describing is the reachable sequence: drag a
// ruler with Measure, then open `Image > Transform...` from the menu bar --
// which does not change the tool. Measure is still selected. So the predicate
// is `effectiveTool(st) == Tool::Measure`, and "effective" is where the
// previous-tool machinery genuinely earns its place: while Space is held,
// `brush.tool` is the borrowed Hand and the tool the user is in is
// `springReturn`. Seeding off `brush.tool` would silently stop working for
// anyone who panned the canvas to see the far end of their own ruler.
//
// (The same borrow is why that `clearMeasureLine()` arm now asks
// `springHandHeld()` first: borrowing the Hand is not leaving the tool, and a
// ruler destroyed by the pan the user did to read it is the accident that arm
// was never written for.)
//
// ==========================================================================
// 4. Why a new pair rather than functions on app/AppState.hpp
// ==========================================================================
//
// `AppState.hpp` is a data header included by most of `src/`; it declares
// state, not policy, and has no `.cpp`. Section 0's guarantee is "one
// translation unit writes this field", which needs a translation unit. A
// header-inline setter would also put the ledger rules in a file every UI
// source already includes, where the next `st.brush.tool = ...` is one line
// away from the rule forbidding it rather than one file away.
//
// ==========================================================================
// 5. A LIVE GIZMO IS MODAL: while one is on screen the tool cannot change
// ==========================================================================
//
// `enterTransformTool()` below puts the pointer on Move when a session
// begins, so that the tool the user was holding stops making content under
// the gizmo. That fixed the gesture and left the door open: **the palette was
// still live.** Pick the Text tool out of it while the box is up and the hole
// is back, byte for byte -- a click off the block makes a second text layer
// behind a gizmo that is still waiting for Return. Reported as "I can select
// a tool while transforming".
//
// So a transform is modal, in the sense a dialog is: it has to be finished
// before anything else is chosen. Return applies it, Escape cancels it, and
// until one of those the tool is Move.
//
// **Enforced HERE, not at the four controls that ask for a switch.** The
// palette cell, the flyout row, the Goodies > Tools menu item and the flats
// palette are four places, and §0's whole argument is that a rule spread over
// four call sites is a rule the fifth one will not have. The controls still
// draw themselves disabled -- a refusal the user cannot see coming is its own
// defect -- but drawing is what they do, and the refusing is done once, in
// the writer, where a control added next year inherits it without being told.
//
// **Scoped to the document the gizmo is ON, deliberately.** A session outlives
// a document switch (`app/TransformSession.hpp`), and the canvas block's
// Return/Escape keys are themselves scoped to the active document -- so a
// lock keyed on `active()` alone would freeze the whole palette over a
// document that shows no gizmo and offers no key that ends it. That is a
// dead end with no exit, which is worse than the hole it closes. The
// predicate below is the same `active() && documentId() == the active
// document's` the gizmo, the Escape key and the three-way composite are all
// already drawn from: the tool is locked exactly when the box is visible.
//
// The other half of that scoping is in `ui/MacPaintUI.cpp`: returning to a
// document with a live session re-installs Move, because the palette is
// unlocked while you are away and the tool can have drifted before you come
// back. The rule is "while the gizmo is up the tool is Move", and it is worth
// nothing if it holds only at the moment the gizmo appears.
//
// **Space still pans.** `beginSpringHand()` does not go through
// `setActiveTool()` (§1) and is deliberately not gated: borrowing the Hand to
// see the far end of what you are transforming is not choosing a tool, it
// changes nothing about what a click means when you let go, and Photoshop's
// Free Transform allows it too. The Eyedropper's borrow is already impossible
// here without a line being written -- `springEyedropperEligible(Tool::Move)`
// is false, and Move is the only tool a live session can be in.

namespace np {

// **The only writer of `AppState::brush.tool` outside this file.** Records
// the outgoing tool and installs `next`. Returns false, changing nothing at
// all, when §5's live gizmo refuses the switch.
//
// A switch to the tool that is already active does not move the ledger: it is
// not a switch, and treating it as one would overwrite the real previous tool
// with itself. Reachable in the palette (clicking the cell that is already
// selected), in the flyout (picking the member already shown) and from the
// menu (`MenuAction::ToolItem` for the current tool), and a Hand -> Hand
// switch losing the real previous is the concrete loss. That case returns
// TRUE: the request was honoured, and the ledger not moving is what honouring
// it means. False is reserved for a refusal.
//
// Ends a spring-loaded borrow if one is in flight -- see the header's §2.
bool setActiveTool(AppState& st, Tool next) noexcept;

// **Install the tool a LIVE TRANSFORM SESSION is driven with: `Tool::Move`.**
// Returns true if that actually changed the tool.
//
// Called when a session BEGINS from a command -- Cmd+T, Edit > Free
// Transform, Image > Transform... -- and never from the Move tool's own drag,
// which is already in it and whose `beginMove()` is guarded on there being no
// session anyway.
//
// **Why the tool has to change at all.** A transform session puts a gizmo on
// the canvas; it does NOT disable the active tool's own click handling. So
// whatever the user was doing when they pressed Cmd+T, its canvas gesture is
// still armed underneath the gizmo -- and for the tools that make content,
// that gesture makes content. The Text tool's rule for a click on empty
// canvas is CREATE A NEW POINT TEXT LAYER (ui/MacPaintUI.cpp's Text block),
// so: type a caption, press Cmd+T to turn it, click anywhere off the block to
// re-aim, and a second text layer appeared behind the gizmo. Measured before
// this existed -- four layers before the stray click, five after, with the
// session still live. The Pen, the Shape tool and the flatting gestures all
// have handlers of the same shape.
//
// The alternative was a `!st.transform.active()` term on the Text block's own
// gate. Rejected: it fixes the one tool that was reported and leaves the same
// hole under every other content-making tool, and it is a rule about what a
// transform means written into a place that knows only about text. Changing
// the tool says the same thing once, in the file that owns "what does a click
// mean", and it is visible -- the palette highlight moves, so the user can
// see why their clicks stopped drawing.
//
// The outgoing tool goes into the ledger like any deliberate pick, so
// `previousTool()` still names what they were using. Nothing restores it when
// the session ends: that is Photoshop's behaviour after Cmd+T, and a tool
// that silently changed back would undo a pick the user may have made
// deliberately while the gizmo was up.
bool enterTransformTool(AppState& st) noexcept;

// **The sentence a modal surface shows while §5's live gizmo is up**, or
// `nullptr` when no gizmo is in the way.
//
// **Named for the gizmo rather than for the tool**, because it outgrew the
// tool palette. Six surfaces read it now -- the palette cell, the flyout row,
// the Goodies tool family, the flats palette, the options band and the LAYERS
// panel -- and only four of those are about tools. What they share is the
// question, not the widget: *is a transform gizmo live on the document in
// front of the user*. It stays in this file because this is where the half
// that REFUSES is enforced (`setActiveTool()`, `setFlatsTool()`), and a
// predicate kept away from its own enforcement is a predicate the two can
// disagree about.
//
// A `const char*` refusal rather than a `bool`, matching
// `app/ToolSurface.hpp`'s `toolSurfaceRefusal()` exactly: every one of those
// surfaces needs a reason to put in front of the user, and a bool would have
// each of them inventing its own wording for the same state. One string, six
// surfaces, and the setters below test the same function for nullptr -- so
// what a panel says and what a setter does cannot drift apart.
//
// Takes no `Tool`. This axis is a property of the session, not of the tool:
// every cell is refused, including the Move cell that is already lit.
//
// **Not the menu bar's rule.** The menu CANCELS a live transform rather than
// being greyed by it (`ui/MenuModel.hpp`'s `menuActionEndsTransform()`), and
// the difference is that the menu carries the escape hatches -- Undo, Save,
// Quit -- while a palette of tools and a panel of layer buttons carry none.
// Greying the menu would trap a user behind a box; greying these does not.
const char* transformModalRefusal(const AppState& st) noexcept;

// Pick a flatting tool (or `FlatsTool::None` to leave flatting mode), and
// install the host tool ADR-0009's table gives it. The single writer of
// `AppState::flatsTool`, for the reason this header gives about
// `brush.tool`: two writers of "what does a click mean" is how a gesture
// ends up meaning two things at once.
//
// Refused, changing nothing, under §5's live gizmo -- and it needs its own
// check rather than inheriting `setActiveTool()`'s, because the switch
// statement at the bottom of it writes `brush.tool` DIRECTLY (that function
// would clear the very `flatsTool` this one is setting). Two writers of
// `brush.tool` in this file means two places the gizmo has to be asked about.
bool setFlatsTool(AppState& st, FlatsTool next) noexcept;

// Whether any tool switch has happened yet this session. False at launch, and
// `previousTool()` means nothing until it is true.
bool hasPreviousTool(const AppState& st) noexcept;

// The tool the user was in before the current one. Undefined-but-safe (the
// launch default) when `hasPreviousTool()` is false -- a real `Tool` value
// rather than a sentinel, deliberately, because `ui/AtelierChrome.cpp`'s
// `kToolMeta` is indexed by `static_cast<size_t>(t)` and a sentinel handed to
// it is an out-of-bounds read.
Tool previousTool(const AppState& st) noexcept;

// The tool the USER believes is selected. Identical to `st.brush.tool` except
// while the Hand is borrowed for a Space-pan or the Eyedropper is borrowed
// for an Alt-sample, when `brush.tool` is the borrowed tool and this is what
// the user will get back. Ask this, not `brush.tool`, wherever the question
// is "what is the user doing" rather than "what does this frame's canvas
// gesture route to" -- the canvas gesture and the cursor are the latter
// question, and both already read `st.brush.tool` directly for the Hand;
// the Eyedropper spring keeps that same shape rather than routing its own
// sample and cursor through this function, which would have to reveal the
// borrowed tool here and contradict the sentence above.
Tool effectiveTool(const AppState& st) noexcept;

// --- the spring-loaded Hand (T20) -----------------------------------------

// Whether the Hand is currently borrowed.
bool springHandHeld(const AppState& st) noexcept;

// Space went down: borrow the Hand, remembering what to give back. Returns
// false and changes nothing if a borrow is already in flight, which is what
// key auto-repeat delivers -- without that guard the second press would
// record `springReturn = Hand` and the release would strand the user in the
// Hand tool, the exact self-erasure §1 rejects.
bool beginSpringHand(AppState& st) noexcept;

// Space came up (or the borrow is being cancelled for any other reason):
// hand the tool back. Returns false and changes nothing when no borrow is in
// flight -- a release with no press must be a no-op and must NOT install
// `springReturn`, or a document switch, a focus loss or a stray key event
// would silently swap the user's tool for whatever the field happened to
// hold. This is the "no previous tool ever set" case, and it is a no-op by
// construction rather than by a guard at each call site.
bool endSpringHand(AppState& st) noexcept;

// --- the spring-loaded Eyedropper (Alt/Option) -----------------------------
//
// Alt held over a paintable tool borrows the Eyedropper: clicks and drags
// sample the canvas exactly as `Tool::Eyedropper` does, and letting go hands
// the borrowed-from tool back. The same shape as the Hand's pair just above,
// with the same two reasons for existing as its own functions rather than
// `setActiveTool()` calls -- header §1's self-erasure argument, and §0's
// "one writer" -- and it deliberately does NOT move `previous`/`hasPrevious`
// for the identical reason.
//
// **Not every tool spring-loads it.** Clone Stamp keeps Alt for picking its
// source, the four selection tools keep Alt-subtract, Zoom keeps Alt-out,
// Pen keeps Alt to suppress its gnomon, and the Flats-mode paint bucket
// keeps Alt to carve a fill -- every one of those is a live, shipped meaning
// for the same chord, and this borrow must not steal it. `AppState.hpp`'s
// `BucketFill` is why `PaintBucket` alone needs a second argument: the SAME
// tool means "carve a fill" in Flats mode and "the ordinary bucket" in
// Colour mode, and only the latter has Alt to spare.
bool springEyedropperHeld(const AppState& st) noexcept;

// PURE: whether Alt spring-loads the Eyedropper for tool `t` with the paint
// bucket's fill mode `fill` (read only when `t == Tool::PaintBucket`, and
// otherwise ignored). No `AppState` reference so `--selftest` can walk every
// `(Tool, BucketFill)` pair without standing up a document or a canvas.
bool springEyedropperEligible(Tool t, BucketFill fill) noexcept;

// Alt went down over an eligible tool: borrow the Eyedropper, remembering
// what to give back. Returns false and changes nothing if a borrow is
// already in flight (auto-repeat, the Hand's own guard's reasoning) OR if
// the Hand is currently borrowed -- the two springs are mutually exclusive
// by construction, never by a caller remembering to check the other one
// first, because a caller that forgets is exactly how two borrows would end
// up live at once with only one `brush.tool` to restore to.
bool beginSpringEyedropper(AppState& st) noexcept;

// Alt came up (or the borrow is being cancelled for any other reason): hand
// the tool back. Returns false and changes nothing when no borrow is in
// flight, the Hand's own no-op-by-construction reasoning above.
bool endSpringEyedropper(AppState& st) noexcept;

// --- T24: the angle `Image > Transform...` opens with ---------------------

// The value `drawNumericTransformDialog()`'s `Rotate (deg)` field is seeded
// with when the dialog opens: the ruler's heading when Measure is the tool
// the user is in and the ruler belongs to the document in front of them, and
// **exactly zero** in every other case -- a different tool, no ruler, or a
// ruler measured on a document the user has since tabbed away from (whose
// texels are not these texels; `app/MeasureLine.hpp` §1).
//
// The angle is `measureReadout()`'s, never recomputed -- there is one
// vector-to-heading function in this build and `app/selftest/AngleConvention.cpp`
// pins it geometrically. A second `atan2` here would be a second place for
// the sign to be wrong.
//
// `activeDocumentId` rather than reaching into `st.documents`: the caller
// already has the `OpenDocument` it just began a transform session on, and
// taking the id keeps this callable from `--selftest` without a document
// session to stand up.
float transformSeedAngleDeg(const AppState& st, uint64_t activeDocumentId) noexcept;

}  // namespace np
