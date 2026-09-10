#pragma once

#include <string>
#include <vector>

#include "app/Action.hpp"
#include "app/Batch.hpp"

// app/BatchDialog -- the BATCH dialog's *model*
// (docs/automation-plan.md step 7; PRD P4, P7).
//
// The model/chrome split is app/ActionsPanel's, followed for its reason and
// not by habit: what a control says and when it greys itself is a decision
// with a right answer, and a decision that can only be reached by drawing a
// frame is a decision `--selftest` cannot ask about. Everything below takes
// state and returns what to draw. `ui/MacPaintUI.cpp` draws it and decides
// nothing.
//
// ==========================================================================
// (1) What this dialog greys, and what it lets the run refuse
// ==========================================================================
//
// There are two kinds of "you cannot do that yet" here and they belong in
// different places.
//
// **Cheap and local** -- no action chosen, no source files, no output
// directory named. These are properties of what the user has typed, they cost
// nothing to check, and a button that stays live for them would open a file
// dialog to tell the user something the dialog already knew. They grey the
// button, with the reason travelling on the button.
//
// **Expensive or on-disk** -- the output directory does not exist, an output
// path resolves to an input, two sources render the same output name, the
// sources disagree in size while a step carries a pixel-unit parameter. These
// are `planBatch()`'s, and they stay there. Three reasons, in order of how
// much they matter:
//
//   * `planBatch()` is what `runBatch()` runs first. A dialog that
//     re-implemented any of these would be a second opinion, and the moment
//     the two disagreed the disagreement would be silent -- the dialog would
//     say "30 files will be written" and the run would refuse, or worse, the
//     other way around.
//   * They touch the filesystem. `pathsNameTheSameFile()` canonicalises, so
//     the collision check is O(sources x sources) `stat` calls. That is fine
//     once per press and wrong once per frame.
//   * The refusals are already written, as whole sentences naming the file and
//     the reason. Re-deriving a shorter version for a tooltip would be a
//     second, worse copy of the best text in the module.
//
// So: the buttons answer what they can answer for free, and everything else
// arrives as a report with a refusal in it. **`BatchDialogView::error` is
// where a whole-run refusal is shown**, and it is deliberately shown in the
// report area rather than on the button, because it is an answer to a question
// the user asked, not a reason they could not ask it.
//
// ==========================================================================
// (2) PREVIEW is `planBatch()`, not a flag on the run
// ==========================================================================
//
// The dry run and the real run share their whole pre-flight because the dry
// run *is* the pre-flight: `runBatch()` calls `planBatch()` and stops if it
// refused. A `bool dryRun` parameter threaded through one function would have
// been the arrangement where a check can be skipped in one mode and not the
// other, which for the check that stops a batch overwriting its own inputs
// (PRD P4) is not a risk worth taking for a parameter's convenience.
//
// The consequence a reader should hold onto: **a PREVIEW that reports 30 files
// and a RUN that refuses cannot both happen**, because the refusal would have
// been in the preview.
//
// `BatchDialogView::reportWasPreview` says which produced the report on
// screen, and the chrome must show it. A report is a list of file names and
// outcomes either way; the difference between "these files were written" and
// "these files would be written" is the entire meaning of the panel, and it
// cannot be left to the user's memory of which button they pressed.
//
// ==========================================================================
// (3) The unchanged files are the point of the report
// ==========================================================================
//
// docs/automation-plan.md §7: "A silent no-op is the failure mode this feature
// is built to have." In the UI a no-op is a greyed menu item; in a batch it is
// thirty files written **unmodified** and reported as successes.
//
// `app/Batch` already counts them (`BatchReport::unchanged()`) and names them
// in `batchSummary()`. This model's job is to stop the chrome burying that: a
// row whose `unchangedByAction` is set gets `BatchReportRow::conspicuous`, and
// the summary line is carried whole rather than recomputed. A thirty-row table
// where three rows are quietly different is a table nobody reads.
namespace np {

// One button, and why it is grey when it is. app/ActionsPanel's, deliberately
// the same shape -- two panels in one feature that disagreed about how a
// disabled control explains itself would be a worse inconsistency than the
// duplication of a two-field struct.
struct BatchDialogButton {
  bool enabled = false;
  // Empty when `enabled`. One sentence otherwise, naming the thing and the
  // reason.
  std::string disabledReason;
};

// One row of the report table.
struct BatchReportRow {
  size_t ordinal = 0;
  // What the user typed, not the canonicalised form -- a report should quote
  // the path they wrote.
  std::string sourcePath;
  // The resolved output *filename*, or empty when the name could not resolve.
  std::string filename;
  // `Written`, `Skipped`, `Failed` or `NotAttempted`, from
  // `exportItemOutcomeName()` so the dialog and the `--batch` report say the
  // same words.
  std::string outcome;
  // Why, for everything but `Written`. Empty for `Written`.
  std::string reason;
  size_t bytesWritten = 0;
  // §3. True when the action ran and changed nothing in this file. The chrome
  // must make these visually distinct from a plain success; a per-row warning
  // in a thirty-row table is not conspicuous.
  bool conspicuous = false;
  std::vector<std::string> warnings;
};

// What the dialog is holding. Owned by `AppState`.
struct BatchDialogState {
  // The action, once one has been loaded from the library. Empty `steps` means
  // none is chosen.
  Action action;
  // The library file the action came from, for the chooser's selected row.
  std::string actionPath;

  // The source files, one per line, exactly as typed. Parsed by
  // `batchDialogSources()` rather than kept parsed, so what the user sees and
  // what the run gets cannot drift.
  std::string sourcesText;

  std::string outputDirectory;
  ExportRequest format;
  std::string nameTemplate = "{name}";

  // The last report, and which button produced it.
  bool haveReport = false;
  bool reportWasPreview = false;
  BatchReport report;

  // The last sentence the dialog has to say. Never a silent outcome.
  std::string status;
};

// Everything the dialog draws.
struct BatchDialogView {
  // The line above the controls. Never empty.
  std::string headline;
  // The chosen action's name, or empty.
  std::string actionName;
  // One line per step, in `CommandSpec::label` terms -- the same text the
  // ACTIONS panel's list shows, so a user recognises the action they picked.
  std::vector<std::string> actionSteps;
  // How many source lines parsed to a path. Shown so "30 files" is visible
  // before anything is pressed.
  size_t sourceCount = 0;

  BatchDialogButton preview;
  BatchDialogButton run;

  bool haveReport = false;
  // §2. Which button produced the report below.
  bool reportWasPreview = false;
  // A whole-run refusal. Non-empty means nothing was opened and nothing
  // written -- see §1 for why this is not on a button.
  std::string error;
  std::vector<BatchReportRow> rows;
  // `batchSummary()` verbatim. Empty when there is no report.
  std::string summary;
  std::string status;
};

// The source lines that parse to a path: non-empty after trimming, in order,
// duplicates kept.
//
// **Duplicates are kept deliberately.** Two identical sources resolve to one
// output name, which `planBatch()` refuses by name as an output collision --
// a better answer than this function quietly de-duplicating and the user never
// learning their list had a repeat in it.
std::vector<std::string> batchDialogSources(const std::string& sourcesText);

// The request the buttons would submit.
BatchRequest batchDialogRequest(const BatchDialogState& st);

// What the dialog draws right now.
BatchDialogView batchDialogView(const BatchDialogState& st);

// PREVIEW: fills `st.report` from `planBatch()` and marks it a preview.
void batchDialogPreview(BatchDialogState& st);

// RUN: fills `st.report` from `runBatch()` and marks it not a preview.
void batchDialogRun(BatchDialogState& st);

// Loads an action from the library into `st`, replacing whatever was there.
// Returns false and leaves `st.action` untouched when the file will not read,
// putting the reader's own sentence in `st.status`.
bool batchDialogLoadAction(BatchDialogState& st, const std::string& path);

}  // namespace np
