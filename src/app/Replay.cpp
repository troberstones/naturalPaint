#include "app/Replay.hpp"

#include <string>
#include <utility>

#include "app/CommandsOpStack.hpp"
#include "app/Recorder.hpp"

namespace np {
namespace {

// The parameter key that carries a serialised op. One spelling, shared with
// app/CommandsOpStack.cpp, because a pre-flight that looked under a different
// key than the applier reads would pass a step the applier then refuses --
// which is the pre-flight being decorative.
constexpr const char* kOpParamKey = "op";

std::string stepPrefix(size_t index, const std::string& id) {
  return "step " + std::to_string(index + 1) + " (\"" + id + "\")";
}

// Everything a command is allowed to have changed, moved from the scratch
// document to the caller's. See app/Replay.hpp section 1 for what is
// deliberately absent and how `--selftest` catches an omission here.
void commitReplay(OpenDocument& doc, OpenDocument&& scratch) {
  doc.document = std::move(scratch.document);
  doc.selection = std::move(scratch.selection);
  doc.lastDeselected = std::move(scratch.lastDeselected);
  doc.refineUndoStack = std::move(scratch.refineUndoStack);
  doc.selectionRevision = scratch.selectionRevision;
  doc.activeLayer = scratch.activeLayer;
  doc.maskIsEditTarget = scratch.maskIsEditTarget;
}

}  // namespace

ReplayResult replayAction(OpenDocument& doc, const Action& action, std::string historyLabel) {
  ReplayResult result;

  // ---- Pre-flight -------------------------------------------------------
  //
  // Only what is knowable without running anything. Layer names are NOT
  // checked here and app/Replay.hpp section 1 says why: the plan's own example
  // selects a layer that step 2 creates, so a static name check either refuses
  // a legal action or stops checking, and "stops checking" is the option that
  // writes a wrong file quietly. Those are caught at apply time instead, by
  // the command's own precondition, and cost nothing because the run is on a
  // copy.
  for (size_t i = 0; i < action.steps.size(); ++i) {
    const Command& step = action.steps[i];
    if (findCommand(step.id) == nullptr) {
      result.status = "refused: " + stepPrefix(i, step.id) +
                      " names a command this build does not have. The whole action is "
                      "refused rather than run with the step skipped -- a skipped step "
                      "writes a file that looks correct and is not.";
      return result;
    }
    // An op-carrying step is decoded here, not merely at apply time, because
    // this is the refusal docs/automation-plan.md §7 asks for by name: "an
    // `Unknown` op record -- an action from a newer build -- refuses the run".
    // `opFromJson()` already refuses a non-`PointA` class and an unknown kind
    // with a sentence of its own, so this reports that sentence rather than
    // inventing a second, vaguer one.
    if (const JsonValue* op = step.params.find(kOpParamKey); op != nullptr) {
      Op decoded;
      std::string why;
      if (!opFromJson(*op, &decoded, &why)) {
        result.status = stepPrefix(i, step.id) + ": " + why;
        return result;
      }
    }
  }

  // ---- The run, on a copy ----------------------------------------------
  //
  // The scratch history is reset to a single baseline rather than inherited:
  // the appliers each record into it and none of those entries is ever shown,
  // so carrying the caller's entries in would be copying a stack of documents
  // to throw it away.
  OpenDocument scratch = doc;
  scratch.history = History();
  scratch.history.begin("replay base", scratch.document);
  scratch.unsavedEdits.clear();
  scratch.unsavedEditsDropped = 0;

  // Section 4: a live recording watches the user, not the replay.
  RecorderSuspension noRecording;

  for (size_t i = 0; i < action.steps.size(); ++i) {
    const Command& step = action.steps[i];
    const CommandResult r = applyCommand(scratch, step);

    ReplayStepReport report;
    report.index = i;
    report.commandId = step.id;
    report.ok = r.ok;
    report.status = r.status;
    report.warnings = r.warnings;
    report.texelsChanged = r.texelsChanged;

    for (const std::string& w : r.warnings) result.warnings.push_back(stepPrefix(i, step.id) + ": " + w);

    // Section 3. A step that meant to change pixels and changed none is the
    // exact shape of "thirty files written unmodified and reported as
    // successes". It does not fail the run -- a blur on an already-flat plate
    // is legitimately a no-op -- but it is never invisible.
    if (r.ok && r.changesPixels && r.texelsChanged == 0) {
      const std::string warning =
          stepPrefix(i, step.id) +
          " succeeded and changed no pixels. It was applied to a document where it had "
          "nothing to do; if that is a surprise, the step is running on a different layer "
          "or through a different selection than it was recorded under.";
      report.warnings.push_back(warning);
      result.warnings.push_back(warning);
    }

    result.steps.push_back(std::move(report));

    if (!r.ok) {
      // Nothing is committed. `doc` has not been written by this function at
      // any point on this path.
      result.status = stepPrefix(i, step.id) + " refused, so the action \"" + action.name +
                      "\" was not applied and the document is unchanged. " + r.status;
      return result;
    }
  }

  // ---- Commit ----------------------------------------------------------
  commitReplay(doc, std::move(scratch));

  // Section 2: one entry, named for the action, however many steps it took.
  doc.recordEdit(historyLabel.empty() ? action.name : std::move(historyLabel));

  result.ok = true;
  result.status = "applied \"" + action.name + "\": " + std::to_string(action.steps.size()) +
                  (action.steps.size() == 1 ? " step" : " steps") +
                  (result.warnings.empty()
                       ? "."
                       : ", with " + std::to_string(result.warnings.size()) +
                             (result.warnings.size() == 1 ? " warning." : " warnings."));
  return result;
}

}  // namespace np
