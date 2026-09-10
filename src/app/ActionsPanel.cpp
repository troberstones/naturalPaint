#include "app/ActionsPanel.hpp"

#include <algorithm>
#include <filesystem>

#include "app/Command.hpp"
#include "io/ActionFile.hpp"

namespace np {
namespace {

// docs/ui.md's own separator, the one `historyRowText()` already uses. Kept as
// one constant so the two panels cannot drift into two punctuation marks.
constexpr const char* kDot = " \xC2\xB7 ";

// One parameter, as a row reads it.
//
// Numbers go through `JsonValue::write()`'s shortest-round-trip form rather
// than through `%g`: a sigma of 4 reads `4` and a sigma of 0.30000000000000004
// reads all of itself, which is the difference between a row that is short and
// a row that lies about what will be replayed.
std::string paramText(const JsonValue& params, const std::string& key) {
  const JsonValue* v = params.find(key);
  if (v == nullptr) return std::string();
  std::string text;
  if (v->isString()) {
    text = v->asString();
  } else {
    text = v->write(-1);
  }
  if (text.empty()) return std::string();
  return key + " " + text;
}

std::string stepText(const Command& step, const CommandSpec* spec) {
  if (spec == nullptr) {
    // §4: the one row where the id is the information.
    return "unknown command \"" + step.id + "\"";
  }
  std::string text = spec->label;
  for (const std::string& key : spec->paramNames) {
    const std::string one = paramText(step.params, key);
    if (one.empty()) continue;
    text += kDot;
    text += one;
  }
  return text;
}

std::vector<ActionStepRow> rowsFor(const std::vector<Command>& steps, size_t selected) {
  std::vector<ActionStepRow> rows;
  rows.reserve(steps.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    const CommandSpec* spec = findCommand(steps[i].id);
    ActionStepRow row;
    row.index = i;
    row.commandId = steps[i].id;
    row.known = spec != nullptr;
    row.label = spec != nullptr ? spec->label : std::string();
    row.text = stepText(steps[i], spec);
    row.selected = i == selected;
    rows.push_back(std::move(row));
  }
  return rows;
}

ActionsPanelButton live() {
  ActionsPanelButton b;
  b.enabled = true;
  return b;
}

ActionsPanelButton grey(std::string why) {
  ActionsPanelButton b;
  b.enabled = false;
  b.disabledReason = std::move(why);
  return b;
}

}  // namespace

ActionsPanelView actionsPanelView(const ActionsPanelState& st, const Recorder& rec,
                                  const OpenDocument* doc) {
  ActionsPanelView v;
  v.recording = rec.isRecording();
  v.stepsEditable = !v.recording;

  // §3: precedence between the two lists is total and stated once.
  const std::vector<Command>& steps = v.recording ? rec.steps() : st.action.steps;
  const size_t selected = v.recording ? kNoActionStep : st.selected;
  v.steps = rowsFor(steps, selected);

  // §2: while recording the refusals are the live take's, because that take is
  // what the list is showing; afterwards they are the adopted copy.
  v.refusals = v.recording ? rec.refusals() : st.refusals;
  v.warnings = v.recording ? rec.warnings() : st.warnings;

  const size_t n = steps.size();
  if (v.recording) {
    v.headline = "RECORDING" + std::string(kDot) + std::to_string(n) + " step(s)";
  } else if (n == 0) {
    v.headline = "No steps. Press RECORD and do the work, or load an action.";
  } else {
    const std::string name = st.action.name.empty() ? std::string("(unnamed)") : st.action.name;
    v.headline = name + kDot + std::to_string(n) + " step(s)";
  }

  // --- RECORD / STOP ------------------------------------------------------
  if (v.recording) {
    v.record = grey("Already recording. STOP ends the take; the steps stay here.");
  } else if (doc == nullptr) {
    v.record = grey("No document open. A recording is a sequence of edits to a document.");
  } else {
    v.record = live();
  }
  v.stop = v.recording ? live() : grey("Nothing is recording.");

  // --- PLAY ---------------------------------------------------------------
  //
  // Disabled while recording, and NOT because the engine could not cope --
  // `replayAction()` suspends the recorder for its duration (app/Replay.hpp
  // §4), so a replay taken mid-take is already safe. It is disabled because
  // the list below the button is the take being recorded, and a PLAY that ran
  // some other action while showing this one would be the panel's one
  // opportunity to lie about what it is about to do.
  if (v.recording) {
    v.play = grey("Stop the recording first. The list below is the take you are recording, "
                  "not a saved action.");
  } else if (doc == nullptr) {
    v.play = grey("No document open. There is nothing to replay this action against.");
  } else if (n == 0) {
    v.play = grey("This action has no steps.");
  } else {
    v.play = live();
  }

  // --- SAVE, and §2's invariant ------------------------------------------
  //
  // The order of these tests is the order a user hits them, so the sentence
  // they get names the first thing that is actually in their way.
  if (v.recording) {
    v.save = grey("Stop the recording first.");
  } else if (n == 0) {
    v.save = grey("This action has no steps.");
  } else if (!v.refusals.empty()) {
    // The rule this panel exists to get right. `Recorder::usable()` is the
    // model's own name for it; the refusals below the list are what makes the
    // greyed button mean something.
    v.save = grey(std::to_string(v.refusals.size()) +
                  " step(s) were refused while this was recorded, so it has a hole in it. "
                  "A recording with a hole is not a shorter recording -- it replays "
                  "confidently and does the wrong thing. Fix what the refusals below name "
                  "and record it again.");
  } else if (actionFileNameFor(st.action.name).empty()) {
    v.save = grey("Give the action a name first. A name needs at least one letter, digit, "
                  "space, dot, dash or underscore in it.");
  } else {
    v.save = live();
  }

  // --- the two row verbs --------------------------------------------------
  if (!v.stepsEditable) {
    const char* why = "The step list is read-only while recording.";
    v.removeStep = grey(why);
    v.moveUp = grey(why);
    v.moveDown = grey(why);
  } else if (selected == kNoActionStep || selected >= n) {
    const char* why = "Select a step first.";
    v.removeStep = grey(why);
    v.moveUp = grey(why);
    v.moveDown = grey(why);
  } else {
    v.removeStep = live();
    v.moveUp = selected == 0 ? grey("This is already the first step.") : live();
    v.moveDown = selected + 1 >= n ? grey("This is already the last step.") : live();
  }
  return v;
}

void actionsPanelRecord(ActionsPanelState& st, Recorder& rec, const OpenDocument& doc) {
  rec.arm(doc);
  st.action.steps.clear();
  st.refusals.clear();
  st.warnings.clear();
  st.selected = kNoActionStep;
  st.recordingDocument = doc.id;
  st.status = "Recording. Every command you apply to this document becomes a step.";
}

void actionsPanelStop(ActionsPanelState& st, Recorder& rec) {
  // Idempotent, and the guard below depends on it: the guard stops and adopts,
  // and a STOP click on the frame after would otherwise adopt an empty take
  // over the one that was just saved.
  if (!rec.isRecording()) return;
  rec.stop();
  st.action.steps = rec.steps();
  st.refusals = rec.refusals();
  st.warnings = rec.warnings();
  st.selected = kNoActionStep;
  st.recordingDocument = 0;
  st.status = "Recorded " + std::to_string(st.action.steps.size()) + " step(s).";
  if (!st.refusals.empty())
    st.status += " " + std::to_string(st.refusals.size()) +
                 " were refused, so this take cannot be saved as it stands.";
}

std::string actionsPanelGuard(ActionsPanelState& st, Recorder& rec,
                              const ActionsPanelContext& ctx) {
  if (!rec.isRecording()) return std::string();

  // Each of the three is a recording that has stopped meaning what it said --
  // see this file's header §1. Ordered most specific first so the sentence
  // names the thing that actually happened.
  std::string why;
  if (!ctx.documentOpen) {
    why = "the document it was recording was closed";
  } else if (st.recordingDocument != 0 && ctx.activeDocument != st.recordingDocument) {
    why = "you switched to another document, and a take that spans two documents is a "
          "sequence that never happened on either";
  } else if (!ctx.panelVisible) {
    why = "the ACTIONS panel was put away, and a recording nobody is looking at goes on "
          "collecting every command in the session";
  } else {
    return std::string();
  }

  actionsPanelStop(st, rec);
  const std::string sentence = "Recording stopped: " + why + ". " +
                               std::to_string(st.action.steps.size()) +
                               " step(s) were kept.";
  st.status = sentence;
  return sentence;
}

bool actionsPanelMoveStep(ActionsPanelState& st, size_t index, int delta) {
  const size_t n = st.action.steps.size();
  if (index >= n || delta == 0) return false;
  // Signed arithmetic on the way, unsigned on the way back: `index + delta`
  // with `delta == -1` and `index == 0` is the wrap that would put a step at
  // the far end of the list instead of refusing the move.
  const long long target = static_cast<long long>(index) + delta;
  if (target < 0 || target >= static_cast<long long>(n)) return false;
  const size_t to = static_cast<size_t>(target);

  Command moved = st.action.steps[index];
  st.action.steps.erase(st.action.steps.begin() + static_cast<long>(index));
  st.action.steps.insert(st.action.steps.begin() + static_cast<long>(to), std::move(moved));
  // The selection follows the STEP, not the row -- a MOVE UP that left the
  // highlight where it was would move a different step on the next click,
  // which is the classic way a reorder button walks a list apart.
  st.selected = to;
  return true;
}

bool actionsPanelDeleteStep(ActionsPanelState& st, size_t index) {
  if (index >= st.action.steps.size()) return false;
  st.action.steps.erase(st.action.steps.begin() + static_cast<long>(index));
  if (st.action.steps.empty()) {
    st.selected = kNoActionStep;
  } else {
    st.selected = std::min(index, st.action.steps.size() - 1);
  }
  return true;
}

std::vector<ActionLibraryRow> actionsPanelLibrary(const std::string& dir) {
  std::vector<ActionLibraryRow> rows;
  for (const std::string& path : listActionFiles(dir)) {
    ActionLibraryRow row;
    row.path = path;
    row.name = std::filesystem::path(path).stem().string();
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string actionsPanelSavePath(const ActionsPanelState& st, const std::string& dir,
                                 std::string* errorOut) {
  const std::string file = actionFileNameFor(st.action.name);
  if (file.empty()) {
    if (errorOut != nullptr)
      *errorOut = "This action has no usable name, so there is no file to write it to. "
                  "A name needs at least one letter, digit, space, dot, dash or "
                  "underscore in it.";
    return std::string();
  }
  if (errorOut != nullptr) errorOut->clear();
  return dir.empty() ? file : dir + "/" + file;
}

}  // namespace np
