#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/Action.hpp"
#include "app/DocumentLifecycle.hpp"

// app/Replay -- running an `Action` against an `OpenDocument`
// (docs/automation-plan.md step 5; PLAN.md "Phase 19 -- Automate it").
//
// app/Action holds the model, io/ActionFile holds the file, and this holds the
// execution. The split is core/OpStack + io/OpSerial's, for the reason
// app/Action.hpp already argues at length: the audience that edits an action
// in a text file and the audience that executes it pull in different
// directions, and a module serving both compromises both.
//
// ==========================================================================
// (1) Replay runs on a COPY and commits only on success
// ==========================================================================
//
// The gate this step has to pass is "an action referencing a layer the
// document lacks refuses **without touching a pixel**". There are two ways to
// get there and they are not equally good.
//
// The obvious one is to check everything up front. It cannot work, and the
// plan's own example says why: the action's third step selects `"Flattened"`,
// a layer that does not exist until the second step creates it. Any static
// pre-flight either refuses that legal action or stops checking layer names,
// and the second option is the one that writes a wrong file quietly.
//
// The other is to apply the steps and undo them if one fails. That is a
// restore procedure, and a restore procedure is code that can have a bug in
// it -- a member it forgets to put back, a counter it double-counts. The
// failure it protects against is exactly the one nobody tests, because you
// only reach it when a replay has already gone wrong.
//
// So a replay runs against a scratch `OpenDocument` and the caller's document
// is assigned only when every step has succeeded. "Refuses without touching a
// pixel" then stops being a promise this file makes and becomes something it
// **cannot break**: on the refusing path there is no write to the caller's
// document anywhere in this translation unit. The cost is one `core::Document`
// copy per replay, which core/History's Phase 5 step 6 measurement puts at
// 0.000011 s and +0.0 MiB for a 32.0 MiB / 256-tile document -- the tiles are
// shared until written.
//
// **What the commit copies, and why the list is short.** A command may change
// `document`, `selection`, `lastDeselected`, `refineUndoStack`,
// `selectionRevision`, `activeLayer` and `maskIsEditTarget`. It may not change
// this document's identity (`id`, `path`, `title`, `residencyMode`) and it
// does not get to write the session's bookkeeping (`revision`,
// `unsavedEdits`, `history`) -- section 2 owns that. A member added to
// `OpenDocument` and forgotten here would be a silent partial commit, so
// `--selftest` does not check the list by reading it: it runs the same steps
// twice, once through `replayAction()` and once through `applyCommand()`
// directly, and asserts the two documents agree. A member left out fails that
// comparison whatever it is called.
//
// ==========================================================================
// (2) One history entry, so undo takes the action back in one stroke
// ==========================================================================
//
// Every applier calls `OpenDocument::recordEdit()`, so a seven-step action
// applied to the live document would leave seven entries and a user undoing it
// would press Cmd+Z seven times without ever being told why. Those seven
// entries land in the **scratch** document's history, which is discarded; the
// caller's history gains exactly one entry, labelled with the action's name.
//
// This is not `amendEdit()`. That extends an entry the caller already owns and
// its header is explicit that it cannot tell whether the top entry is theirs.
// Here there is no entry to extend -- the replay's own edits were never in
// this history in the first place.
//
// ==========================================================================
// (3) A step that changed nothing is a warning, never a pass
// ==========================================================================
//
// docs/automation-plan.md §7: "A silent no-op is the failure mode this feature
// is built to have." `applyImageSize()` records no history for a resize to the
// size the document already is, and a filter on a non-RGB layer is refused by
// `pixelOpRefusalFor()`. Interactively those are a greyed menu item and a
// nothing-happens. In a batch they are thirty files written **unmodified** and
// reported as successes.
//
// So a step whose spec says it changes pixels, that succeeded, and that
// reports zero texels changed produces a warning naming the step and its
// index. The run still completes -- a no-op is not necessarily wrong, and
// refusing one would make "blur, then threshold" fail on an already-flat
// plate -- but it is never invisible.
//
// ==========================================================================
// (4) A live recording does not record a replay
// ==========================================================================
//
// `applyCommand()` taps the recorder, so an armed `Recorder` would otherwise
// watch a replay go past and append every one of its steps to whatever the
// user was recording. That is not a thing anyone asks for: a user recording
// "my export prep" who plays an existing action wants **one** thing recorded,
// and this build has no command that means "run action X" -- so recording the
// expansion would silently inline it, and the inlined copy would then stop
// tracking the action it came from.
//
// Replay therefore suspends recording for its duration (`RecorderSuspension`,
// app/Recorder.hpp) and restores it afterwards, whether it succeeded, refused
// or threw. The interactive path is free to record its own single step for the
// replay later, when an id for one exists.
namespace np {

// What one step did. One of these per step **attempted** -- a run that refuses
// at step 4 of 7 reports four, not seven, and `ReplayResult::steps.size()` is
// therefore how far the run got.
struct ReplayStepReport {
  // Zero-based index into `Action::steps`, so a refusal can be pointed at a
  // line of the file the user can go and look at.
  size_t index = 0;
  std::string commandId;
  bool ok = false;
  // The command's own sentence, in either direction. Never empty.
  std::string status;
  std::vector<std::string> warnings;
  size_t texelsChanged = 0;
};

struct ReplayResult {
  // True only when every step succeeded. A partial run is a failure, and the
  // caller's document is untouched -- see this header's section 1.
  bool ok = false;
  // One sentence naming what happened: the action and its step count on
  // success, or the step that refused and its index and reason. Never empty,
  // in either direction.
  std::string status;
  // Per-step, in order, for the steps that were attempted.
  std::vector<ReplayStepReport> steps;
  // Every step's warnings, flattened and prefixed with the step's position, in
  // order -- plus this file's own zero-texel warnings (section 3). A caller
  // showing one line per problem shows these; a caller showing a table walks
  // `steps` instead.
  std::vector<std::string> warnings;
};

// Runs `action` against `doc`, committing only if every step succeeds.
//
// On success `doc` holds the result and its history has gained exactly one
// entry named for the action. On failure `doc` is bit-for-bit what it was, and
// `ReplayResult::status` names the step that refused, its index and its
// reason.
//
// `historyLabel`, when empty, defaults to the action's name; `--batch` passes
// nothing because a headless document's history is never shown, and the
// ACTIONS panel passes nothing because the action's name is the right label.
// It exists for a caller replaying one action as part of a larger act it wants
// named as a whole.
ReplayResult replayAction(OpenDocument& doc, const Action& action,
                          std::string historyLabel = std::string());

}  // namespace np
