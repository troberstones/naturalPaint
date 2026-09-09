#include "app/Recorder.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include "core/Channels.hpp"

namespace np {
namespace {

// True when two coverage stores describe the same selection.
//
// Both directions are walked, because "every tile of A matches B" alone is
// satisfied by an A with no tiles at all -- which would match an empty
// marquee to any saved channel, and put a channel's name on a shape the user
// never selected.
bool coverageIdentical(const SelectionTileStore& a, const SelectionTileStore& b) {
  const auto containedIn = [](const SelectionTileStore& lhs, const SelectionTileStore& rhs) {
    for (const auto& [coord, tile] : lhs) {
      // A tile that selects nothing is indistinguishable from not existing
      // (core/SelectionMask.hpp), and `saveSelectionAsChannel()` compacts them
      // away on the way in. Comparing tile *sets* rather than coverage would
      // therefore report a live selection as differing from the channel that
      // was made from it -- which is the one case that must match.
      if (tile.selectsNothing()) continue;
      const SelectionTile* other = rhs.find(coord);
      if (other == nullptr) return false;
      if (std::memcmp(tile.data(), other->data(), SelectionTile::kTexelCount) != 0) return false;
    }
    return true;
  };
  return containedIn(a, b) && containedIn(b, a);
}

// Whether this command advertises a `"layer"` key the recorder can write the
// target's name into. Read off the table rather than kept as a list here: a
// second list of which commands address a layer is a second thing to keep in
// step with app/Command's rows, and it would go stale silently the first time
// a family registers a new one.
bool takesLayerParam(const CommandSpec& spec) {
  return std::find(spec.paramNames.begin(), spec.paramNames.end(), "layer") !=
         spec.paramNames.end();
}

}  // namespace

std::string channelMatchingSelection(const Document& doc, const Selection& selection) {
  for (const AlphaChannel& channel : doc.channels)
    if (coverageIdentical(channel.tiles, selection.tiles)) return channel.name;
  return {};
}

void Recorder::arm(const OpenDocument& doc) {
  state_ = State::Recording;
  steps_.clear();
  refusals_.clear();
  warnings_.clear();

  // The baseline the `select_layer` rule measures against. Without it the
  // first step would always look like a change of active layer and every
  // recording would open with a `select_layer` that selects what is already
  // selected -- see app/Recorder.hpp §3, which also names what this costs.
  const std::optional<size_t> active = activeLayerIndex(doc);
  haveLastActive_ = active.has_value();
  lastActiveLayer_ = active ? doc.document.layers[*active].name : std::string();
}

void Recorder::stop() {
  // Only a live recording stops. Calling `stop()` on an idle recorder must not
  // move it to Stopped: "stopped" is a state with a recording behind it that
  // the panel offers to save, and an empty one offered for saving is a file
  // with no steps in it.
  if (state_ == State::Recording) state_ = State::Stopped;
}

void Recorder::note(const Command& command, const CommandResult& result,
                    const RecorderPreState& before) {
  if (state_ != State::Recording) return;

  // app/Recorder.hpp §2: what the user tried is not what the user did.
  if (!result.ok) return;

  // --- §4: the marquee refusal -------------------------------------------
  if (result.changesPixels && before.selectionLive && before.selectionChannel.empty()) {
    refusals_.push_back(
        "refused to record \"" + command.id +
        "\": a selection was live that no saved channel matches. A selection is session "
        "state and never reaches a file, so this step would replay with nothing selected -- "
        "and nothing selected means no restriction, so it would cover the whole canvas and "
        "report success. Save the selection as a named alpha channel first (a channel is "
        "document data and survives into the file), or deselect before recording.");
    return;
  }
  if (result.changesPixels && before.selectionLive) {
    warnings_.push_back(
        "\"" + command.id + "\" was recorded under the saved selection \"" +
        before.selectionChannel +
        "\". The step list cannot carry that on its own -- no command that loads a channel "
        "as a selection is registered in this build -- so the action must load \"" +
        before.selectionChannel + "\" before this step or it will apply to the whole canvas.");
  }

  // --- §3: pin the active layer when it moved ----------------------------
  //
  // Emitted BEFORE the step it protects, and measured against what the
  // recording last *said* was active rather than against where the last
  // command happened to leave the selection.
  bool relyingOnTheName = false;
  if (before.haveActiveLayer && (!haveLastActive_ || before.activeLayerName != lastActiveLayer_)) {
    JsonValue pin = JsonValue::object();
    pin.set("layer", JsonValue::string(before.activeLayerName));
    steps_.push_back(Command{"select_layer", pin});
    haveLastActive_ = true;
    lastActiveLayer_ = before.activeLayerName;
    relyingOnTheName = true;
  }

  // --- the step itself, with its target written down ----------------------
  //
  // app/CommandSupport.hpp's targeting rule states the half this implements:
  // "the recorder always writes the name". A hand-written action may leave
  // `"layer"` out and mean "whatever `select_layer` last chose"; a recorded
  // one never does, because the user's intent was the layer they were looking
  // at and not the one a replay happens to arrive with.
  Command step = command;
  const CommandSpec* spec = findCommand(command.id);
  if (spec != nullptr && takesLayerParam(*spec) && step.params.find("layer") == nullptr &&
      before.haveActiveLayer) {
    step.params.set("layer", JsonValue::string(before.activeLayerName));
    relyingOnTheName = true;
  }

  // Layer names are not unique (core/Channels.hpp contrasts them with channel
  // names, which are). By-name targeting resolves to the first match, so a
  // recording that just wrote down a duplicated name has recorded a step that
  // may replay onto a different layer of the same name -- including on the
  // very document it was recorded from.
  if (relyingOnTheName && before.activeNameAmbiguous) {
    warnings_.push_back("\"" + command.id + "\" was recorded against the layer name \"" +
                        before.activeLayerName +
                        "\", which more than one layer in this document carries. Replaying it "
                        "resolves to the first such layer, which may not be the one the step "
                        "ran on. Rename the layer to something unique and re-record.");
  }

  // A `select_layer` step IS the recording saying which layer is active, so it
  // -- and nothing else -- updates the baseline. Crediting a structural
  // command's implicit move here instead is the change that would delete the
  // post-flatten pin §3 exists for.
  if (command.id == "select_layer") {
    const JsonValue* named = step.params.find("layer");
    if (named != nullptr && named->isString()) {
      haveLastActive_ = true;
      lastActiveLayer_ = named->asString();
    }
  }

  steps_.push_back(std::move(step));
}

Recorder& sessionRecorder() {
  static Recorder recorder;
  return recorder;
}

RecorderTap::RecorderTap(const OpenDocument& doc, const Command& command) : command_(command) {
  Recorder& recorder = sessionRecorder();
  // Everything below costs a name copy and, under a live marquee, a memcmp per
  // saved channel. None of it is paid unless a recording is running.
  if (!recorder.isRecording()) return;
  recorder_ = &recorder;

  const std::optional<size_t> active = activeLayerIndex(doc);
  if (active) {
    before_.haveActiveLayer = true;
    before_.activeLayerName = doc.document.layers[*active].name;
    before_.activeNameAmbiguous =
        layerIndexNamed(doc.document, before_.activeLayerName) != *active;
  }
  if (doc.selection) {
    before_.selectionLive = true;
    before_.selectionChannel = channelMatchingSelection(doc.document, *doc.selection);
  }
}

CommandResult RecorderTap::record(CommandResult result) {
  if (recorder_ != nullptr) recorder_->note(command_, result, before_);
  return result;
}

}  // namespace np
