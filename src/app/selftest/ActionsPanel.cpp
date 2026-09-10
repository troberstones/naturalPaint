#include "app/selftest/Support.hpp"

#include "app/ActionsPanel.hpp"
#include "app/Command.hpp"
#include "app/Recorder.hpp"
#include "core/Channels.hpp"
#include "core/LayerOps.hpp"
#include "core/SelectionMask.hpp"

namespace np {
namespace {

// A two-layer RGB document, the shape docs/automation-plan.md's worked case
// starts from.
//
// Built here rather than borrowed from app/selftest/Recorder.cpp's fixture:
// internal linkage does not cross a translation unit, and a shared fixture
// would make this section's answers depend on edits made for that one.
OpenDocument makeActionsPanelDocument(DocumentId id) {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "actions");
  od.id = id;
  od.document.layers[0].name = "Base";
  auto fill = [](TileStore& tiles, float v) {
    Tile& t = tiles.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x)
        t.writePixel(PixelCoord{x, y}, {v, v * 0.5f, 1.0f - v, 1.0f});
  };
  fill(*od.document.layers[0].rgbTiles, 0.25f);
  const LayerEditResult added = applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
  if (added.ok && od.document.layers.size() == 2) {
    od.document.layers[added.selected].name = "Detail";
    if (od.document.layers[added.selected].rgbTiles)
      fill(*od.document.layers[added.selected].rgbTiles, 0.75f);
    od.activeLayer = added.selected;
  }
  od.recordEdit("actions panel fixture", EditKind::Structural);
  return od;
}

Command blurStep(double sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return Command{"filter_gaussian_blur", p};
}

Command selectStep(const char* layer) {
  JsonValue p = JsonValue::object();
  p.set("layer", JsonValue::string(layer));
  return Command{"select_layer", p};
}

Command thresholdStep() {
  JsonValue p = JsonValue::object();
  p.set("threshold", JsonValue::number(0.5));
  p.set("amount", JsonValue::number(1.0));
  return Command{"adjust_threshold", p};
}

std::string rowIdAt(const ActionsPanelView& v, size_t i) {
  return i < v.steps.size() ? v.steps[i].commandId : std::string();
}

bool contains(const std::string& s, std::string_view needle) {
  return s.find(needle) != std::string::npos;
}

// A live marquee that no saved channel matches, which is what
// `Recorder::note()` refuses a pixel step under (app/Recorder.hpp §4). This is
// how a HOLED take is produced through the production path rather than by
// pushing a string into `refusals_` by hand -- the distinction the assertion
// discipline exists for.
void selectARectangle(OpenDocument& od) {
  od.selection = selectRectangle(8.0f, 8.0f, 32.0f, 32.0f);
  ++od.selectionRevision;
}

}  // namespace

// app/ActionsPanel (docs/automation-plan.md step 7) -- the ACTIONS panel's
// model, asserted as state-in / drawing-out.
//
// **What this section is for.** Not that a panel exists: that the four ways
// this particular panel can be silently wrong are closed.
//
//   * **It offers to save a recording with a hole in it.** A refused step is
//     not a shorter recording; it is a sequence that replays confidently and
//     does the wrong thing at step 4, on thirty files, reporting success. The
//     invariant below is `save.enabled => refusals.empty()`, asserted over a
//     sweep of states rather than over one.
//   * **It leaves the recorder armed.** `sessionRecorder()` is process-wide,
//     so a take nobody stopped goes on appending every `applyCommand()` in the
//     session -- invisibly, because nothing about it is on the canvas.
//   * **It edits the wrong list.** A recording lives in the `Recorder` and an
//     action lives in the panel; a reorder applied to the invisible one is a
//     change the user cannot see and cannot undo.
//   * **It reads a file key to a human.** `CommandSpec` carries an id and a
//     label and app/Command.hpp says which is which.
//
// Headless, GPU-free and filesystem-free -- `actionsPanelLibrary()` is
// exercised against a directory that does not exist, which is the one
// filesystem answer this module promises and the only one that needs no file.
bool runActionsPanelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // PLAN.md §1.5: an unexercised build option is not a seam. Nothing in
  // app/ActionsPanel reaches a file, an encoder, the GPU or ImGui, and there
  // is no `#ifdef` in it for one to guard.
  std::printf("[selftest] actions panel: state in, drawing out; no ImGui and no GPU\n");

  Recorder& session = sessionRecorder();
  // Whatever a previous section left armed. This section asserts about the
  // process-wide recorder, so it starts from a known state rather than from
  // the last one.
  session.stop();

  std::printf("  -- A. the buttons, with no document and with one --\n");
  {
    ActionsPanelState st;
    Recorder rec;
    const ActionsPanelView none = actionsPanelView(st, rec, nullptr);
    check(!none.record.enabled && contains(none.record.disabledReason, "No document open"),
          "empty: RECORD is grey with no document, and says so");
    check(!none.stop.enabled && !none.play.enabled && !none.save.enabled,
          "empty: STOP, PLAY and SAVE are all grey");
    check(!none.play.disabledReason.empty() && !none.save.disabledReason.empty(),
          "empty: every grey button carries a reason, never a bare grey");
    check(!none.headline.empty() && none.steps.empty(),
          "empty: the headline is never blank, even with nothing to show");

    OpenDocument od = makeActionsPanelDocument(1);
    const ActionsPanelView withDoc = actionsPanelView(st, rec, &od);
    check(withDoc.record.enabled && withDoc.record.disabledReason.empty(),
          "document: RECORD goes live, and its reason clears");
    check(!withDoc.play.enabled && contains(withDoc.play.disabledReason, "no steps"),
          "document: PLAY is still grey -- an empty action has nothing to run");
    check(!withDoc.save.enabled && contains(withDoc.save.disabledReason, "no steps"),
          "document: SAVE is still grey for the same reason");
  }

  std::printf("  -- B. recording: the list is the take, and it is read-only --\n");
  {
    ActionsPanelState st;
    OpenDocument od = makeActionsPanelDocument(1);
    actionsPanelRecord(st, session, od);
    check(session.isRecording() && st.recordingDocument == od.id,
          "record: the session recorder is armed, on this document");

    applyCommand(od, Command{"flatten_image", JsonValue::object()});
    applyCommand(od, blurStep(2.0));

    const ActionsPanelView v = actionsPanelView(st, session, &od);
    check(v.recording && v.steps.size() == 3,
          "record: the list shows the live take (flatten, the pin, the blur)");
    check(rowIdAt(v, 0) == "flatten_image" && rowIdAt(v, 1) == "select_layer" &&
              rowIdAt(v, 2) == "filter_gaussian_blur",
          "record: in the order the work was done, the pin included");
    check(!v.stepsEditable && !v.removeStep.enabled && !v.moveUp.enabled &&
              !v.moveDown.enabled,
          "record: the row verbs are all grey while a take is being recorded");
    check(contains(v.removeStep.disabledReason, "read-only"),
          "record: and they say why, rather than being grey for no stated reason");
    check(!v.record.enabled && contains(v.record.disabledReason, "Already recording"),
          "record: RECORD greys itself while recording");
    check(v.stop.enabled, "record: STOP is the live one");
    check(!v.play.enabled && contains(v.play.disabledReason, "Stop the recording first"),
          "record: PLAY is grey, because the list is the take and not a saved action");
    check(!v.save.enabled, "record: SAVE is grey until the take is finished");

    // The panel's own list is untouched while the recorder holds the take --
    // §3's precedence, which is what stops a reorder editing the invisible
    // vector.
    check(st.action.steps.empty(),
          "record: the panel's own action stays empty while the recorder owns the take");

    actionsPanelStop(st, session);
    check(!session.isRecording() && st.action.steps.size() == 3,
          "stop: the take is adopted into the panel's action");
    check(st.recordingDocument == 0, "stop: the recording document is released");

    const ActionsPanelView after = actionsPanelView(st, session, &od);
    check(!after.recording && after.stepsEditable && after.steps.size() == 3,
          "stop: the same three rows, now editable");
    check(after.play.enabled, "stop: PLAY goes live once there is an action to run");

    // A second STOP must not adopt an empty take over the one just saved.
    actionsPanelStop(st, session);
    check(st.action.steps.size() == 3, "stop: a second STOP changes nothing");
  }

  std::printf("  -- C. a row reads its LABEL, never its file key --\n");
  {
    ActionsPanelState st;
    Recorder rec;
    st.action.name = "Height prep 512";
    st.action.steps = {selectStep("Base"), blurStep(4.0), thresholdStep(),
                       Command{"not_a_command_in_this_build", JsonValue::object()}};
    const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
    check(v.steps.size() == 4, "rows: one row per step, unknown ids included");

    const CommandSpec* blur = findCommand("filter_gaussian_blur");
    check(blur != nullptr && v.steps.size() > 1 && v.steps[1].label == blur->label,
          "rows: the row's label is the registry's label");
    check(v.steps.size() > 1 && !contains(v.steps[1].text, "filter_gaussian_blur"),
          "rows: the file-key id is NOT what the row reads");
    check(v.steps.size() > 1 && contains(v.steps[1].text, "sigma 4"),
          "rows: the advertised parameters are in the row text");
    check(v.steps.size() > 2 && contains(v.steps[2].text, "threshold 0.5") &&
              contains(v.steps[2].text, "amount 1"),
          "rows: every advertised parameter, not only the first");

    check(v.steps.size() > 3 && !v.steps[3].known &&
              contains(v.steps[3].text, "not_a_command_in_this_build"),
          "rows: an unregistered id is a named unknown row, not a missing one");
    check(v.steps.size() > 3 && contains(v.steps[3].text, "unknown"),
          "rows: and it says the word, so a shorter list cannot pass for a shorter action");
    check(contains(v.headline, "Height prep 512") && contains(v.headline, "4 step"),
          "rows: the headline names the action and its length");
  }

  std::printf("  -- D. reorder and delete produce the action the user sees --\n");
  {
    ActionsPanelState st;
    Recorder rec;
    st.action.name = "Reorder me";
    st.action.steps = {selectStep("Base"), blurStep(1.0), thresholdStep()};

    check(!actionsPanelMoveStep(st, 3, +1) && !actionsPanelMoveStep(st, 0, -1),
          "verbs: an out-of-range index and a move off the top both refuse");
    check(st.action.steps.size() == 3 && st.action.steps[0].id == "select_layer",
          "verbs: and neither of those refusals moved anything");

    st.selected = 2;
    check(actionsPanelMoveStep(st, 2, -1), "verbs: moving the last step up succeeds");
    {
      const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
      check(rowIdAt(v, 0) == "select_layer" && rowIdAt(v, 1) == "adjust_threshold" &&
                rowIdAt(v, 2) == "filter_gaussian_blur",
            "verbs: the DRAWN list is the reordered one");
      check(st.selected == 1 && v.steps.size() > 1 && v.steps[1].selected,
            "verbs: the selection followed the step, not the row");
      check(v.moveUp.enabled && v.moveDown.enabled,
            "verbs: a middle row can go either way");
    }

    st.selected = 0;
    {
      const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
      check(!v.moveUp.enabled && contains(v.moveUp.disabledReason, "first step") &&
                v.moveDown.enabled,
            "verbs: the first row cannot move up, and says so");
    }
    st.selected = 2;
    {
      const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
      check(!v.moveDown.enabled && contains(v.moveDown.disabledReason, "last step"),
            "verbs: the last row cannot move down, and says so");
    }
    st.selected = kNoActionStep;
    {
      const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
      check(!v.removeStep.enabled && contains(v.removeStep.disabledReason, "Select a step"),
            "verbs: with nothing selected the row verbs are grey");
    }

    st.selected = 1;
    check(actionsPanelDeleteStep(st, 1), "verbs: deleting the middle step succeeds");
    {
      const ActionsPanelView v = actionsPanelView(st, rec, nullptr);
      check(v.steps.size() == 2 && rowIdAt(v, 0) == "select_layer" &&
                rowIdAt(v, 1) == "filter_gaussian_blur",
            "verbs: the DRAWN list lost exactly the row that was deleted");
      check(st.selected == 1, "verbs: the selection stayed on the row that moved up into it");
    }
    check(!actionsPanelDeleteStep(st, 9), "verbs: deleting past the end refuses");
    check(actionsPanelDeleteStep(st, 1) && actionsPanelDeleteStep(st, 0),
          "verbs: the last two deletes succeed");
    check(st.action.steps.empty() && st.selected == kNoActionStep,
          "verbs: emptying the list clears the selection rather than leaving row 0 selected");
  }

  std::printf("  -- E. a recording with a hole is never offered for saving --\n");
  {
    // The hole is produced through the production path: a pixel step taken
    // under a live marquee that no saved channel names is refused at record
    // time (app/Recorder.hpp §4), and `usable()` is false for as long as one
    // refusal is there.
    ActionsPanelState st;
    OpenDocument od = makeActionsPanelDocument(1);
    actionsPanelRecord(st, session, od);
    applyCommand(od, selectStep("Base"));
    selectARectangle(od);
    applyCommand(od, blurStep(2.0));
    actionsPanelStop(st, session);

    check(!st.refusals.empty(),
          "hole: the take carries a refusal (a pixel step under an unnamed marquee)");
    check(!st.action.steps.empty(), "hole: and it still carries the steps that were taken");

    const ActionsPanelView v = actionsPanelView(st, session, &od);
    check(!v.save.enabled, "hole: SAVE is refused");
    check(v.refusals.size() == st.refusals.size() && !v.refusals.empty(),
          "hole: and every refusal is on screen while it is");
    check(!v.refusals.empty() && v.refusals[0].size() > 40,
          "hole: each refusal is the recorder's own sentence, not a count");
    check(contains(v.save.disabledReason, "hole"),
          "hole: the greyed button says what is wrong with the take");
    check(v.play.enabled, "hole: PLAY is still live -- a holed take is runnable, just not savable");

    // **The invariant, over a sweep rather than over one state.** Every
    // combination of (holed / clean) x (recording / stopped) x (document /
    // none) x (named / unnamed), and in none of them may SAVE be live while a
    // refusal is outstanding.
    bool implicationHolds = true;
    size_t sweptStates = 0;
    size_t sweptSavable = 0;
    for (int holed = 0; holed < 2; ++holed) {
      for (int recording = 0; recording < 2; ++recording) {
        for (int haveDoc = 0; haveDoc < 2; ++haveDoc) {
          for (int named = 0; named < 2; ++named) {
            OpenDocument sweepDoc = makeActionsPanelDocument(2);
            ActionsPanelState s;
            Recorder r;
            s.action.name = named != 0 ? "Named" : "";
            s.action.steps = {selectStep("Base"), blurStep(1.0)};
            if (holed != 0) s.refusals.push_back("A step was refused, and here is the reason.");
            if (recording != 0) {
              r.arm(sweepDoc);
              if (holed != 0) {
                // `note()` is the production sink itself -- the same function
                // `RecorderTap::record()` calls -- driven with the pre-state a
                // live unnamed marquee produces. `applyCommand()` is not
                // usable here: it taps `sessionRecorder()`, and this sweep
                // needs a recorder of its own.
                CommandResult pixelStep;
                pixelStep.ok = true;
                pixelStep.status = "blurred";
                pixelStep.changesPixels = true;
                RecorderPreState under;
                under.haveActiveLayer = true;
                under.activeLayerName = "Detail";
                under.selectionLive = true;
                r.note(blurStep(1.0), pixelStep, under);
              }
            }
            const ActionsPanelView sv =
                actionsPanelView(s, r, haveDoc != 0 ? &sweepDoc : nullptr);
            ++sweptStates;
            if (sv.save.enabled) ++sweptSavable;
            if (sv.save.enabled && !sv.refusals.empty()) implicationHolds = false;
          }
        }
      }
    }
    std::printf("  swept %zu panel states, %zu of them offered SAVE\n", sweptStates,
                sweptSavable);
    check(implicationHolds && sweptStates == 16,
          "hole: over all 16 states, SAVE is live only when nothing was refused");
    // A sweep in which SAVE is never live would satisfy the implication
    // vacuously, which is the shape docs/testing-issues.md keeps warning
    // about. It has to be satisfiable too.
    check(sweptSavable > 0, "hole: and the sweep is not vacuous -- some states DO offer SAVE");
  }

  std::printf("  -- F. the arm/stop lifecycle, which nothing else guards --\n");
  {
    // The recorder is process-wide. Each case below leaves it armed by every
    // route the panel has, and asserts the guard closes it.
    ActionsPanelContext live;
    live.documentOpen = true;
    live.activeDocument = 1;
    live.panelVisible = true;

    {
      ActionsPanelState st;
      OpenDocument od = makeActionsPanelDocument(1);
      actionsPanelRecord(st, session, od);
      applyCommand(od, blurStep(1.0));
      check(actionsPanelGuard(st, session, live).empty() && session.isRecording(),
            "guard: an ordinary frame stops nothing");

      ActionsPanelContext closed = live;
      closed.documentOpen = false;
      closed.activeDocument = 0;
      const std::string said = actionsPanelGuard(st, session, closed);
      check(!session.isRecording(), "guard: closing the document stops the recording");
      check(contains(said, "closed") && contains(said, "step(s) were kept"),
            "guard: and says which and how many, rather than stopping in silence");
      check(!st.action.steps.empty(),
            "guard: the take is adopted, not thrown away -- stop is not a discard");
      check(st.status == said, "guard: the sentence survives in the panel's status line");
      check(actionsPanelGuard(st, session, closed).empty(),
            "guard: a second frame with the document still closed does nothing");
    }
    {
      ActionsPanelState st;
      OpenDocument od = makeActionsPanelDocument(1);
      actionsPanelRecord(st, session, od);
      ActionsPanelContext other = live;
      other.activeDocument = 7;
      const std::string said = actionsPanelGuard(st, session, other);
      check(!session.isRecording() && contains(said, "another document"),
            "guard: switching documents stops the take, because it would span two");
    }
    {
      ActionsPanelState st;
      OpenDocument od = makeActionsPanelDocument(1);
      actionsPanelRecord(st, session, od);
      ActionsPanelContext away = live;
      away.panelVisible = false;
      const std::string said = actionsPanelGuard(st, session, away);
      check(!session.isRecording() && contains(said, "put away"),
            "guard: putting the panel away stops the take -- nobody is looking at it");
    }
    {
      // Not a case, an anti-case: an idle recorder is not something the guard
      // may go poking at. A guard that "stopped" a recorder nobody armed would
      // wipe the panel's action on every frame the panel was hidden.
      ActionsPanelState st;
      st.action.steps = {blurStep(1.0)};
      Recorder idle;
      ActionsPanelContext away = live;
      away.panelVisible = false;
      away.documentOpen = false;
      check(actionsPanelGuard(st, idle, away).empty() && st.action.steps.size() == 1,
            "guard: with nothing recording it does nothing, whatever the context says");
    }
    check(!session.isRecording(),
          "guard: this section leaves the process-wide recorder stopped");
  }

  std::printf("  -- G. the library and the save path --\n");
  {
    ActionsPanelState st;
    std::string err = "not cleared";
    st.action.name = "Height prep 512";
    const std::string path = actionsPanelSavePath(st, "/tmp/np-actions", &err);
    check(path == "/tmp/np-actions/Height prep 512.npaction" && err.empty(),
          "save path: the directory, the sanitised name and the extension");

    // A name that is a path. `actionFileNameFor()` is the seam, and what it
    // guarantees is that the result is ONE path component under the directory
    // it was given -- not that the characters are unrecognisable. The `..`
    // that survives inside `_.._etc_passwd` is a filename, not a traversal,
    // and asserting it away would be asserting a property the seam does not
    // claim.
    st.action.name = "../../etc/passwd";
    const std::string escaped = actionsPanelSavePath(st, "/tmp/np-actions", &err);
    const std::string stem = escaped.size() > 16 ? escaped.substr(16) : escaped;
    check(escaped.rfind("/tmp/np-actions/", 0) == 0 && !stem.empty() &&
              stem.find('/') == std::string::npos && stem[0] != '.',
          "save path: a name that is a path lands as ONE component, not above the library");

    // A name with nothing left after sanitising -- every byte is a leading
    // dot, which `actionFileNameFor()` strips so a name cannot produce a
    // hidden file, or `.` or `..`.
    st.action.name = "...";
    check(actionsPanelSavePath(st, "/tmp/np-actions", &err).empty() && !err.empty(),
          "save path: a name with nothing usable in it refuses, rather than inventing one");

    // The one filesystem answer this module promises, and the only one that
    // needs no file: a library nobody has created yet is empty, not an error.
    check(actionsPanelLibrary("/tmp/np-actions-that-does-not-exist-0f3a").empty(),
          "library: a directory that does not exist lists as empty");

    // With steps, so the unnameable case is the FIRST thing in the way rather
    // than sitting behind "this action has no steps" -- the order the view
    // tests its reasons in is the order a user hits them, and an assertion
    // that reads the wrong one of them is measuring the wrong thing.
    st.action.steps = {blurStep(1.0)};
    const ActionsPanelView v = actionsPanelView(st, sessionRecorder(), nullptr);
    check(!v.save.enabled && contains(v.save.disabledReason, "name"),
          "save: an unnameable action cannot be saved, and the button says why");
  }

  return ok;
}

}  // namespace np
