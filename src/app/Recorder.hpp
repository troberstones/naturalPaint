#pragma once

#include <string>
#include <vector>

#include "app/Command.hpp"
#include "app/DocumentLifecycle.hpp"
#include "core/SelectionMask.hpp"

// app/Recorder -- the session sink `applyCommand()` appends to while armed,
// and the two things it refuses to let a recording be quietly wrong about.
//
// docs/automation-plan.md step 3. Arm it, do the work by hand, stop; what
// comes out is an ordered `std::vector<Command>` that io/ActionFile turns into
// a `.npaction` and `replayAction()` runs on another document.
//
// ==========================================================================
// (1) Why the sink is session state, and why it is reachable as a global
// ==========================================================================
//
// A recording is not document data. It does not belong in `Document` (it would
// then be inside every `HistoryEntry` and every saved file, so an undo would
// revert part of a recording and a save would persist a half-finished one),
// and it does not belong on `OpenDocument` either: the user arms the recorder
// once, not once per tab.
//
// That leaves it above the document, which is where `AppState` lives -- but
// `applyCommand()` deliberately **cannot see `AppState`**
// (docs/automation-plan.md §7: "the moment a recordable command reaches for
// session state, it stops being replayable headlessly and the `--batch` path
// starts differing from the interactive one"). The command door's signature is
// `(OpenDocument&, const Command&)` and must stay that way, so the sink cannot
// arrive as a parameter. `sessionRecorder()` is therefore a function-local
// static: one recorder per process, exactly as there is one user per process.
//
// **Single-threaded, and not defended against threads.** Every writer is the
// UI thread draining the command queue at the top of a frame, or `--selftest`
// on its own thread of one. If a future worker ever calls `applyCommand()` off
// the main thread this needs a mutex, and the honest place to notice that is
// here rather than in a corrupted step list.
//
// **A replay is not a recording.** `replayAction()` (step 5) will drive
// `applyCommand()` too, and a recorder left armed across a replay would append
// the replayed steps to the recording it is playing back. Step 5 owns that
// interlock -- it has the loop and can suppress the tap around it. It is named
// here so the interlock is a thing someone deletes on purpose rather than a
// thing nobody wrote.
//
// ==========================================================================
// (2) The rule that decides what becomes a step
// ==========================================================================
//
// **Only a command that SUCCEEDED.** A refused command is not something the
// user did; it is something the user tried. Recording one gives a replay two
// ways to be wrong and no way to be right: it refuses again (noise in the
// per-file report of a thirty-file batch) or, on a document where the
// precondition happens to hold, it *succeeds* -- doing something the user
// never did, in a file that looks correct.
//
// So the tap sits after the applier, reads `CommandResult::ok`, and appends
// nothing when it is false. `applyCommand()`'s two earlier refusals -- an
// unknown id and a failed precondition -- return before the tap exists at all.
//
// ==========================================================================
// (3) The `select_layer` emission, which is the whole point of the track
// ==========================================================================
//
// docs/automation-plan.md §7: "the active layer is the state most likely to
// make a replay wrong and green". Every pixel op targets `activeLayerOf(doc)`,
// and `filter_gaussian_blur` has no `"layer"` parameter to pin it with -- the
// only thing that says which layer a blur landed on is a `select_layer` step
// in front of it.
//
// The rule is **"did the active layer change since the previous recorded
// step"**, and it is deliberately not "did the user click a layer row". A
// structural command moves the active layer *by itself*: `fromLayerEdit()`
// adopts `LayerEditResult::selected`, so a flatten leaves the survivor
// selected and a create leaves the new layer selected without the user
// touching the LAYERS panel. Watching for clicks would miss both, which are
// the two cases that matter.
//
// **The recorder does not credit that implicit move.** After a flatten it
// emits `select_layer "Flattened"` in front of the next step rather than
// trusting the replay to land the selection in the same place -- which is the
// plan's own worked example ("after `flatten_image` the active layer is a
// *different* layer, so 'blur the active layer' is only deterministic if the
// recording says which layer is active"). Trusting it would be relying on two
// documents' appliers agreeing about a selection nobody wrote down, which is
// the precise shape of a replay that is wrong and reports success.
//
// A `select_layer` step the user issued themselves *is* credited, because that
// step is the recording saying which layer is active -- so a hand-picked layer
// followed by three edits on it produces one `select_layer` and not four.
//
// **What this does NOT do, stated rather than left to be found:** it does not
// pin the layer that was active when the recorder was armed. "Changed since
// the previous recorded step" has no answer before the first step, so a
// recording whose first step is a blur says nothing about which layer it
// blurred, and a replay blurs whatever the target document has selected. The
// fix is one step long -- `arm()` could push a `select_layer` for the
// arm-time active layer -- and it is not taken here because it changes the
// step count the plan's step-3 gate states ("exactly five steps"), and the
// decision belongs to whoever writes the ACTIONS panel and looks at what the
// step list should open with.
//
// ==========================================================================
// (4) The marquee refusal
// ==========================================================================
//
// docs/automation-plan.md §7 again: "a selection is session state and is never
// in a file". Every destructive op is selection-bounded, so a blur recorded
// while a marquee was live replays with **no selection at all** -- and no
// selection means no restriction (core/SelectionMask.hpp), so the blur covers
// the whole canvas and reports success. Nothing in the file is wrong; the file
// is simply missing the half of the state that made the step mean what it
// meant.
//
// Recording the marquee's coordinates is the wrong answer -- they are
// meaningless at another resolution, which is the whole reason a batch exists.
// The right answer is a *named alpha channel*: `Document::channels` is
// document data, it is in the history snapshots and in the file, and
// `loadChannelAsSelection()` restores it by name on any document that carries
// it.
//
// So: a pixel step taken under a live selection that no saved channel matches
// is **refused at record time**, by name, with the fix in the sentence. A
// refusal is not a step and does not stop the recording; it lands in
// `refusals()`, and `usable()` is false for as long as one is there --
// because a recording missing a step in its middle is worse than no recording
// at all.
//
// **How "is this step selection-bounded?" is decided.**
// `CommandSpec::selectionBounded` -- the table's own answer, stated per row
// (app/Command.hpp defines it: true when an *absent* selection silently means
// "the whole canvas").
//
// It used to be `CommandResult::changesPixels`, which was described here as a
// deliberate over-approximation whose only error was to over-refuse. That was
// half right and half wrong, and the wrong half was the dangerous one:
//
//  * **Over-refusing, as advertised.** `image_size`, `canvas_size` and
//    `trim_to_content` all report changing pixels and none is bounded by the
//    selection -- each acts on the whole document by construction. Recording
//    any of them under a live marquee was refused for a reason that does not
//    apply to it, and the refusal named a fix that would not have changed
//    anything. (`flatten_image` belongs in that list by meaning and did not in
//    fact reach the guard, because `fromLayerEdit()` never sets
//    `changesPixels` -- app/Command.hpp on the field. It was right by
//    accident, which is its own reason not to keep the proxy.)
//  * **Under-refusing, which was not advertised and is the real defect.**
//    `define_pattern` changes no texel, so it reported `changesPixels ==
//    false` and was never policed -- while its source rectangle *is* the
//    selection's bounds, with absent meaning the whole canvas. A
//    `define_pattern` recorded under a marquee replayed as a pattern the size
//    of the document, and reported success. That is precisely the outcome
//    this section exists to prevent, reached through the check itself.
//
// The flag also cannot rot toward silence, which is the property
// `changesPixels` was chosen for: app/selftest/Command.cpp section H walks the
// whole table and requires every row that reaches the selection-aware
// `applyPixelFilter()` bridge to carry it, so a filter registered next month
// gets the rule for free or fails the suite by name.
namespace np {

// The name of the first channel in `doc` whose coverage is exactly
// `selection`'s, or "" when none is.
//
// **Exact, tile by tile, and not a bounding-box or coverage-sum comparison.**
// A channel that merely has the same area as the marquee is a different
// selection, and treating it as a match would put a name on a shape the user
// did not select -- which is the one failure this whole check exists to
// prevent, arrived at by a shortcut.
//
// Tiles that select nothing are skipped on both sides rather than compared:
// `saveSelectionAsChannel()` compacts on the way in, so an all-zero tile a
// boolean left behind exists in the live selection and not in the saved
// channel, and comparing tile *sets* would report a mismatch between a
// selection and the channel that was made from it.
std::string channelMatchingSelection(const Document& doc, const Selection& selection);

// Everything the recorder has to know about the document as it was BEFORE a
// command ran, captured by `RecorderTap`'s constructor.
//
// It is a before-picture because every question here has a different answer
// afterwards: a flatten changes which layer is active and how many layers
// exist, and asking after the fact would pin the layer the command *landed*
// on rather than the one it *ran* on.
struct RecorderPreState {
  // The active layer's name, and whether there was one at all. An empty name
  // is a legal layer name and is not the same as no layer, which is why the
  // flag is separate from the string.
  bool haveActiveLayer = false;
  std::string activeLayerName;

  // True when `layerIndexNamed()` would resolve `activeLayerName` to a
  // *different* layer -- i.e. an earlier layer shares the name. Layer names
  // are deliberately not unique (core/Channels.hpp says so, contrasting them
  // with channel names), so the recorder's by-name targeting can be ambiguous,
  // and a recording that is ambiguous must say so out loud rather than
  // resolving to the first match and looking fine.
  bool activeNameAmbiguous = false;

  // Whether a marquee was live, and the saved channel that matches it if one
  // does. Both computed before the command, because the command is what would
  // change them.
  bool selectionLive = false;
  std::string selectionChannel;
};

// The session recorder.
class Recorder {
 public:
  // Idle is the state before the first `arm()`; there is no route back to it,
  // and that is not an oversight. "Throw this recording away" is `arm()`
  // followed by `stop()`, and a `reset()` that returned to Idle would be a
  // second spelling of the same thing with a third state to test against.
  enum class State { Idle, Recording, Stopped };

  // Starts a new recording, discarding whatever the last one left behind.
  //
  // Takes the document because §3's rule needs a baseline: the layer that is
  // active at arm time is the one the recording starts on, and pinning it
  // again in front of the first step would be a `select_layer` that changes
  // nothing.
  void arm(const OpenDocument& doc);

  // Stops. The steps stay readable -- stopping is what the ACTIONS panel does
  // before offering to save them -- and nothing further is appended.
  void stop();

  State state() const noexcept { return state_; }
  bool isRecording() const noexcept { return state_ == State::Recording; }

  const std::vector<Command>& steps() const noexcept { return steps_; }

  // Steps this recorder declined to record, each a full sentence naming the
  // command, the reason and the fix.
  const std::vector<std::string>& refusals() const noexcept { return refusals_; }

  // Things recorded, but recorded with something the recording cannot express
  // on its own -- a saved selection that the action file still has to load, an
  // ambiguous layer name. Not refusals: the step is in `steps()`.
  const std::vector<std::string>& warnings() const noexcept { return warnings_; }

  // False once anything has been refused. **A recording with a hole in it is
  // not a shorter recording**; it is a sequence that replays confidently and
  // does the wrong thing at step 4. Whatever offers to save an action asks
  // this first.
  bool usable() const noexcept { return refusals_.empty(); }

  // The tap itself. Called once per `applyCommand()`, after the applier.
  void note(const Command& command, const CommandResult& result, const RecorderPreState& before);

 private:
  State state_ = State::Idle;
  std::vector<Command> steps_;
  std::vector<std::string> refusals_;
  std::vector<std::string> warnings_;

  // The layer the recording has *established* as active: the arm-time active
  // layer, then whatever the last `select_layer` step named. Deliberately not
  // updated by a structural command's implicit move -- see §3.
  bool haveLastActive_ = false;
  std::string lastActiveLayer_;
};

// The one recorder. See §1 for why it is a global and not a member of
// `AppState`.
Recorder& sessionRecorder();

// `applyCommand()`'s hook, and the only thing in app/Command.cpp that knows a
// recorder exists.
//
// It is an object rather than a function because the recorder needs both
// sides of the applier: what the document looked like going in
// (`RecorderPreState`) and whether the command succeeded coming out. A single
// after-the-fact call could not answer "which layer did this run on?" for any
// command that moves the selection, which is every structural one.
//
// Costs nothing measurable when the recorder is idle: the constructor reads
// one enum and returns.
class RecorderTap {
 public:
  RecorderTap(const OpenDocument& doc, const Command& command);

  // Passes `result` straight through, appending a step first when it should.
  // The return is the call's own result unchanged -- the recorder never edits
  // what a command reports.
  CommandResult record(CommandResult result);

 private:
  Recorder* recorder_ = nullptr;  // null unless armed when the command started
  const Command& command_;
  RecorderPreState before_;
};

}  // namespace np
