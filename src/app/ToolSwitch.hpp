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

namespace np {

// **The only writer of `AppState::brush.tool` outside this file.** Records
// the outgoing tool and installs `next`.
//
// A switch to the tool that is already active does not move the ledger: it is
// not a switch, and treating it as one would overwrite the real previous tool
// with itself. Reachable in the palette (clicking the cell that is already
// selected), in the flyout (picking the member already shown) and from the
// menu (`MenuAction::ToolItem` for the current tool), and a Hand -> Hand
// switch losing the real previous is the concrete loss.
//
// Ends a spring-loaded borrow if one is in flight -- see the header's §2.
void setActiveTool(AppState& st, Tool next) noexcept;

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
// while the Hand is borrowed for a Space-pan, when `brush.tool` is the Hand
// and this is what the user will get back. Ask this, not `brush.tool`,
// wherever the question is "what is the user doing" rather than "what does
// this frame's canvas gesture route to".
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
