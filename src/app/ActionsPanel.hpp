#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/Action.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/Recorder.hpp"

// app/ActionsPanel -- the ACTIONS panel's model: what the panel shows, which
// of its buttons are live, and the four mutations its rows offer.
// docs/automation-plan.md step 7 ("ACTIONS panel: record / stop / play, step
// list, reorder, delete"); PRD P1, P5.
//
// Pure list mapping, row text and button state; no ImGui and no GPU -- the
// same split app/LayerPanel.hpp and app/HistoryPanel.hpp already document, and
// for the reason app/HistoryPanel.hpp states outright: everything a
// `--selftest` can check about what a row *says* and what a button *does*
// lives here, and `ui/MacPaintUI.cpp` owns only the chrome. A panel whose
// logic is reachable only by drawing it is a panel with no assertions.
//
// ==========================================================================
// (1) The recorder is a process-wide singleton, and this panel is what arms it
// ==========================================================================
//
// `sessionRecorder()` is one recorder per process (app/Recorder.hpp §1: the
// command door's signature cannot carry a sink, so the sink is a global). The
// consequence belongs here rather than there, because this is the only thing
// that arms it: **a panel that leaves the recorder armed makes every later
// `applyCommand()` in the session append to a recording nobody is looking
// at** -- and nothing about that is visible. The steps do not appear on the
// canvas, the document does not change, and the first symptom is an action
// saved months later that contains an afternoon of unrelated work.
//
// So the arm/stop lifecycle is not left to the draw function, and this is the
// decision worth stating: **`actionsPanelGuard()` runs once a frame from the
// frame loop, NOT from the panel's draw path.** A hidden panel does not draw,
// a collapsed one draws no body, and a panel torn onto the flyout rail draws
// only when the rail is open -- so a stop that lives inside the draw is a stop
// that does not happen in exactly the states where the recording is least
// visible. The guard is above the panel for the same reason the recorder is
// above the document.
//
// It stops a live recording in three cases, each of which is a recording that
// has stopped meaning what it said:
//
//   * **The document closed.** Every step was recorded against that document,
//     and `applyCommand()` from here on would append edits to a different one.
//   * **The active document changed.** Same argument, one step earlier: a take
//     that spans two documents is a sequence that never happened on either.
//   * **The panel was put away** (`PanelPlacement::Hidden`). This is the
//     literal case in the paragraph above -- a recording nobody is looking at.
//
// Stopping is cheap and non-destructive: `Recorder::stop()` ends the take and
// leaves every step readable, so the user who reopens the panel finds a
// finished recording rather than a growing one. **Collapsing the panel does
// not stop it**, and the line is drawn there deliberately: a collapsed panel
// still has its titled grip in the dock, so it is still on screen and still
// one click from its own step list. A panel that is `Hidden` has neither.
//
// ==========================================================================
// (2) The rule about saving, which is the one this panel must not get wrong
// ==========================================================================
//
// `Recorder::usable()` is false once anything has been refused, and its header
// says why in one sentence: *"a recording with a hole in it is not a shorter
// recording"*. It is a sequence that replays confidently and does the wrong
// thing at step 4 -- silently, on thirty files, reporting success.
//
// **So SAVE is disabled for as long as the take carries a refusal, and every
// refusal is on screen while it is.** Both halves matter. Disabling alone
// would be a greyed button with no explanation; showing alone would be an
// explanation the user can click straight past. Each refusal is already a full
// sentence naming the command, the reason and the fix (app/Recorder.hpp §4),
// so the panel prints them verbatim rather than summarising them into a count.
//
// `ActionsPanelView` carries that as a checkable invariant rather than as a
// convention: `save.enabled` is true only when `refusals` is empty, and
// `--selftest` asserts the implication over every state this panel can reach
// rather than over the one the test happened to build.
//
// **The refusals are copied into the panel state when the take is adopted**,
// not read live from the recorder. `Recorder::arm()` discards the last
// recording's refusals along with its steps, so a panel reading them live
// would show a clean take the moment the user armed a second one -- while
// still holding the first take's holed steps in its list. The hole is a
// property of *this action*, so it is stored beside it.
//
// ==========================================================================
// (3) Two lists, one at a time, and which one is which
// ==========================================================================
//
// A recording in progress lives in the `Recorder`; an action being edited,
// loaded or saved lives in `ActionsPanelState::action`. Drawing them as one
// list with two owners is the shape that ends with a reorder applied to the
// wrong vector, so precedence is explicit and total:
//
//   * **While recording**, the list is `Recorder::steps()`, and it is
//     READ-ONLY. Reordering a take while it is still being taken would mean
//     the next recorded step lands somewhere the user did not expect, and
//     `Recorder` has no API to reorder itself -- the panel would be editing a
//     copy, which is app/selftest's own recurring trap.
//   * **Otherwise**, the list is `action.steps`, and it is editable.
//
// `actionsPanelStop()` is the seam: it stops the recorder and copies the take
// into `action`, which is the single moment the two lists meet.
//
// ==========================================================================
// (4) A row shows the LABEL, never the id
// ==========================================================================
//
// `CommandSpec` carries both and app/Command.hpp is explicit about the
// difference: the id is the file key ("stable, lower_snake_case, written into
// `.npaction` files") and the label is "human text for the ACTIONS panel's
// step list. Free to be reworded; the id is not." A panel keyed on the id
// would put `filter_gaussian_blur` in front of a user and, worse, would make
// the id the thing everyone learns to read -- at which point rewording a label
// is free and renaming an id is not, which is backwards.
//
// A step whose id this build does not have still gets a row: it reads
// `unknown command "..."` and carries the id, because that is the one case
// where the id IS the information. `readAction()` refuses such a step at load
// (io/ActionFile.hpp), so it can only arrive from a hand-built action in
// memory -- but a row that silently vanished would make an action look shorter
// than it is, which is §2's failure in a different coat.
namespace np {

// "No row selected", and the return of every lookup that finds nothing.
// `size_t(-1)` rather than an index, so a stale selection can never be
// mistaken for row 0 -- app/HistoryPanel.hpp's `kNoHistoryRow` rule.
inline constexpr size_t kNoActionStep = static_cast<size_t>(-1);

// Everything the ACTIONS panel remembers between frames.
//
// Session state, and deliberately not persisted: an action that matters is in
// the library as a file, and restoring a half-finished take from three
// launches ago as the panel's opening picture would present it as the thing
// the user was doing.
struct ActionsPanelState {
  // The take being edited: adopted from the recorder on stop, replaced
  // wholesale by a load, and edited by the two row verbs.
  Action action;

  // The selected row, or `kNoActionStep`. An index rather than a stable id --
  // and unlike app/HistoryPanel's rows, that is correct here: nothing evicts a
  // step behind the panel's back, the only things that renumber the list are
  // this panel's own two mutations, and both of them move the selection
  // themselves.
  size_t selected = kNoActionStep;

  // The take's refusals and warnings, copied at adoption. See this header's
  // §2 for why they are not read live from the recorder.
  std::vector<std::string> refusals;
  std::vector<std::string> warnings;

  // Which document `arm()` was called on, so the guard can notice the user
  // switching tabs mid-take. Zero when nothing is recording.
  DocumentId recordingDocument = 0;

  // The last sentence the panel has to say -- a save's destination, a replay's
  // status, a guard's stop. Empty until something happens; never a silent
  // outcome.
  std::string status;
};

// One row of the step list.
struct ActionStepRow {
  // Position in the list being shown, which is also its index into whichever
  // vector §3 says is current. Valid until the next mutation.
  size_t index = 0;
  // The step's stable id. Present for `--selftest` and for the unknown-command
  // row; the chrome draws `text`.
  std::string commandId;
  // `CommandSpec::label`, or empty when this build has no such command.
  std::string label;
  // Whether `commandId` is registered in this build.
  bool known = false;
  // What the row reads: the label, then the parameters the command advertises,
  // separated by docs/ui.md's own middle dot -- `Gaussian Blur · sigma 2`.
  // Never empty, for the same reason `historyRowText()` is never empty.
  std::string text;
  bool selected = false;
};

// One button, and why it is grey when it is.
//
// The reason travels with the flag rather than being recomputed by the chrome,
// because a greyed button whose tooltip says nothing is the complaint
// docs/ui.md records about half the panels this one is modelled on.
struct ActionsPanelButton {
  bool enabled = false;
  // Empty when `enabled`. One sentence otherwise, in this codebase's refusal
  // register: name the thing, name the reason.
  std::string disabledReason;
};

// Everything the panel draws, derived from state that is not the panel's.
struct ActionsPanelView {
  bool recording = false;
  // Whether the row verbs act at all. False while recording -- §3.
  bool stepsEditable = false;
  // The line above the list. Never empty.
  std::string headline;
  std::vector<ActionStepRow> steps;

  ActionsPanelButton record;
  ActionsPanelButton stop;
  ActionsPanelButton play;
  ActionsPanelButton save;
  ActionsPanelButton removeStep;
  ActionsPanelButton moveUp;
  ActionsPanelButton moveDown;

  // The take's refusals, verbatim and in order. **Non-empty exactly when
  // `save.enabled` is false for that reason**, which is §2's invariant.
  std::vector<std::string> refusals;
  std::vector<std::string> warnings;
};

// What the panel draws right now. `doc` is the active document, or nullptr
// when none is open.
//
// Reads `rec` rather than taking a copy of its steps, so the list is live
// while a recording is in progress; costs one pass over the steps.
ActionsPanelView actionsPanelView(const ActionsPanelState& st, const Recorder& rec,
                                  const OpenDocument* doc);

// --- the three lifecycle verbs -------------------------------------------

// Arms the session recorder on `doc` and clears the panel's take.
//
// Clearing is not a courtesy: `Recorder::arm()` discards the previous
// recording, so a panel that kept its old list would be showing steps no
// recorder holds and offering to save them beside a take that is growing.
void actionsPanelRecord(ActionsPanelState& st, Recorder& rec, const OpenDocument& doc);

// Stops the recorder and adopts the take -- steps, refusals and warnings --
// into `st.action`. The one moment §3's two lists meet.
//
// Idempotent on a recorder that is not recording, so a double-click on STOP,
// or a STOP after the guard already stopped it, cannot adopt an empty take
// over the one the guard just saved.
void actionsPanelStop(ActionsPanelState& st, Recorder& rec);

// What the frame loop calls, every frame, whether or not the panel drew.
struct ActionsPanelContext {
  bool documentOpen = false;
  // The active document's id, or 0 when none.
  DocumentId activeDocument = 0;
  // False when the panel is `PanelPlacement::Hidden`. Collapsed and flown-out
  // both count as visible -- see this header's §1.
  bool panelVisible = true;
};

// Stops a live recording that has stopped meaning what it said (§1), adopting
// the take exactly as `actionsPanelStop()` would.
//
// Returns the sentence naming what it did, or "" when it did nothing. The
// sentence is also left in `st.status`, so a panel reopened three minutes
// later still says why its take is finished.
std::string actionsPanelGuard(ActionsPanelState& st, Recorder& rec,
                              const ActionsPanelContext& ctx);

// --- the two row verbs ---------------------------------------------------

// Moves the step at `index` by `delta` rows, carrying the selection with it.
// Returns false and changes nothing for an out-of-range index or a move off
// either end -- which is what makes the panel's greyed MOVE UP on row 0 a
// statement about the model rather than only about the chrome.
bool actionsPanelMoveStep(ActionsPanelState& st, size_t index, int delta);

// Deletes the step at `index`. The selection stays at `index` -- now the row
// that moved up into it -- or clamps to the last row, or becomes
// `kNoActionStep` when the list is empty. Returns false for an out-of-range
// index, having changed nothing.
bool actionsPanelDeleteStep(ActionsPanelState& st, size_t index);

// --- the library ---------------------------------------------------------

// One row of the load list.
struct ActionLibraryRow {
  std::string path;
  // The file's stem, which is `actionFileNameFor()`'s sanitised form of the
  // action's name and not the name itself -- the real name is inside the file
  // and only a load can read it. Shown because it is what the user will
  // recognise, and because reading fifty files to title fifty rows is a
  // directory listing that opens fifty files.
  std::string name;
};

// Every `.npaction` in `dir`, sorted, ready to draw. A missing directory is an
// empty list, following `listActionFiles()`.
std::vector<ActionLibraryRow> actionsPanelLibrary(const std::string& dir);

// Where SAVE would write, or "" with a reason in `*errorOut`.
//
// The reason is always the same one -- a name with nothing usable in it -- and
// it is a refusal rather than a generated filename because `actionFileNameFor`
// is the seam between user text and a path (io/ActionFile.hpp) and inventing
// `untitled.npaction` on the far side of it would put two different actions in
// one file.
std::string actionsPanelSavePath(const ActionsPanelState& st, const std::string& dir,
                                 std::string* errorOut = nullptr);

}  // namespace np
