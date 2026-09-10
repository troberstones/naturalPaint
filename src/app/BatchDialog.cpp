#include "app/BatchDialog.hpp"

#include <sstream>
#include <utility>

#include "app/Command.hpp"
#include "io/ActionFile.hpp"
#include "io/ExportStates.hpp"

namespace np {
namespace {

std::string trimmed(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// The step list the dialog shows, in the ACTIONS panel's terms: the command's
// label, not its id. The id is the file key and is never UI copy -- that rule
// is docs/automation-plan.md §5's, and it is why an unknown id shows as itself
// rather than as a blank row.
std::string stepText(const Command& step) {
  const CommandSpec* spec = findCommand(step.id);
  if (spec == nullptr) return step.id + "  (not a command this build has)";
  return spec->label;
}

}  // namespace

std::vector<std::string> batchDialogSources(const std::string& sourcesText) {
  std::vector<std::string> out;
  std::istringstream in(sourcesText);
  std::string line;
  while (std::getline(in, line)) {
    const std::string path = trimmed(line);
    if (!path.empty()) out.push_back(path);
  }
  return out;
}

BatchRequest batchDialogRequest(const BatchDialogState& st) {
  BatchRequest r;
  r.action = st.action;
  r.sources = batchDialogSources(st.sourcesText);
  r.outputDirectory = trimmed(st.outputDirectory);
  r.format = st.format;
  r.nameTemplate = st.nameTemplate;
  return r;
}

BatchDialogView batchDialogView(const BatchDialogState& st) {
  BatchDialogView v;
  const std::vector<std::string> sources = batchDialogSources(st.sourcesText);
  v.sourceCount = sources.size();
  v.actionName = st.action.name;
  for (const Command& step : st.action.steps) v.actionSteps.push_back(stepText(step));

  v.headline = "One action, applied to many files. The inputs are never written to.";

  // §1: only what is free to answer. Order matters -- the sentence a user gets
  // should name the first thing actually in their way.
  BatchDialogButton gate;
  if (st.action.steps.empty()) {
    gate.disabledReason =
        "Choose an action first. A batch with no steps would open every file and write it "
        "back unchanged, which is thirty files touched for nothing.";
  } else if (sources.empty()) {
    gate.disabledReason =
        "Add at least one input file. A run over nothing reports success and does nothing.";
  } else if (trimmed(st.outputDirectory).empty()) {
    gate.disabledReason =
        "Name an output directory. It must already exist -- nothing is created on your "
        "behalf here, because a batch that silently makes folders puts files somewhere you "
        "did not look.";
  } else {
    gate.enabled = true;
  }
  // Both buttons have the same precondition, and that is the point: PREVIEW
  // must be reachable exactly whenever RUN is, or the safe button would be the
  // one a user could not press.
  v.preview = gate;
  v.run = gate;

  v.haveReport = st.haveReport;
  v.reportWasPreview = st.reportWasPreview;
  v.status = st.status;
  if (!st.haveReport) return v;

  v.error = st.report.error;
  v.summary = batchSummary(st.report);
  for (const BatchItem& item : st.report.items) {
    BatchReportRow row;
    row.ordinal = item.ordinal;
    row.sourcePath = item.sourcePath;
    row.filename = item.filename;
    row.outcome = exportItemOutcomeName(item.outcome);
    row.reason = item.reason;
    row.bytesWritten = item.bytesWritten;
    row.conspicuous = item.unchangedByAction;
    row.warnings = item.warnings;
    v.rows.push_back(std::move(row));
  }
  return v;
}

void batchDialogPreview(BatchDialogState& st) {
  st.report = planBatch(batchDialogRequest(st));
  st.haveReport = true;
  st.reportWasPreview = true;
  st.status = st.report.error.empty()
                  ? "Previewed. Nothing has been written."
                  : "Previewed, and the run would be refused. Nothing has been written.";
}

void batchDialogRun(BatchDialogState& st) {
  st.report = runBatch(batchDialogRequest(st));
  st.haveReport = true;
  st.reportWasPreview = false;
  st.status = st.report.error.empty() ? "Run finished." : "Refused. Nothing was written.";
}

bool batchDialogLoadAction(BatchDialogState& st, const std::string& path) {
  Action loaded;
  std::string error;
  if (!loadActionFromFile(path, &loaded, &error)) {
    // The reader's own sentence, verbatim. It already names the file and what
    // was wrong with it.
    st.status = error;
    return false;
  }
  st.action = std::move(loaded);
  st.actionPath = path;
  // A new action invalidates the report on screen. Keeping it would leave a
  // table of outcomes above a different action's name, which is the same class
  // of lie as §2's preview-versus-run.
  st.haveReport = false;
  st.reportWasPreview = false;
  st.report = BatchReport();
  st.status = "Loaded \"" + st.action.name + "\": " + std::to_string(st.action.steps.size()) +
              (st.action.steps.size() == 1 ? " step." : " steps.");
  return true;
}

}  // namespace np
