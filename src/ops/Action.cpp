#include "ops/Action.hpp"

#include <string>

namespace np {

const char* pointOpKindId(PointOpKind kind) noexcept {
  switch (kind) {
    case PointOpKind::Levels:       return "levels";
    case PointOpKind::Curves:       return "curves";
    case PointOpKind::Exposure:     return "exposure";
    case PointOpKind::Saturation:   return "saturation";
    case PointOpKind::Grayscale:    return "grayscale";
    case PointOpKind::ChannelMixer: return "channel_mixer";
    case PointOpKind::Invert:       return "invert";
    case PointOpKind::Posterize:    return "posterize";
    case PointOpKind::Threshold:    return "threshold";
  }
  // No `default:` above, deliberately: with the switch exhaustive over the
  // enum, appending a kind to `PointOpKind` makes the compiler point at this
  // function. Reaching here at all means a value was cast in from outside the
  // enum -- which is what a serialised kind code is before io/OpSerial has
  // validated it -- and the honest answer for that is "no id", not a guess.
  return nullptr;
}

LayerOpsToActionResult actionFromLayerOps(const Layer& layer, std::string actionName) {
  // ======================================================================
  // The command ids and parameter keys this converter emits, in ONE place.
  // ======================================================================
  //
  // **They are named here rather than spelled inline because they are not
  // this file's to decide.** `add_layer_op` is registered by
  // app/CommandsOpStack, which is empty on this branch
  // (docs/automation-plan.md step 1's remaining registrations are being built
  // separately). If the registration lands under a different id or with
  // different parameter keys, this block is the single edit that reconciles
  // them -- and `--selftest`'s tripwire below fails until somebody makes it,
  // rather than this converter quietly writing steps nothing can run.
  static constexpr const char* kSelectLayerId = "select_layer";
  static constexpr const char* kAddLayerOpId = "add_layer_op";
  static constexpr const char* kLayerKey = "layer";
  static constexpr const char* kKindKey = "kind";
  static constexpr const char* kEnabledKey = "enabled";

  LayerOpsToActionResult result;
  result.action.name = std::move(actionName);

  // Targeting is by name and only by name (docs/automation-plan.md §5), so a
  // layer with no name is not convertible: the resulting action would have no
  // way to say which layer it grades, and `resolveTarget()`'s fallback -- "the
  // active layer" -- means whatever the replaying document happens to have
  // selected, which is §7's "wrong and green".
  if (layer.name.empty()) {
    result.error =
        "refused: this layer has no name, and an action addresses layers by name so that "
        "replaying it on another document cannot silently act on a different one. Name the "
        "layer, then convert it.";
    return result;
  }

  for (size_t i = 0; i < layer.ops.size(); ++i) {
    const Op& op = layer.ops.at(i);
    if (op.opClass != OpClass::PointA) {
      // The refusal, not a skip. See ops/Action.hpp on why: an action that
      // drops an entry it cannot describe runs to completion and grades the
      // file differently from the document it came from, which is the exact
      // failure mode a batch turns into thirty wrong files reported as
      // successes.
      result.error = "refused: entry " + std::to_string(i + 1) + " of layer \"" + layer.name +
                     "\" is a " + opDisplayName(op) +
                     ", and this build can express only point ops in an action. Delete or bake "
                     "that entry, or convert the layer with the build that wrote it.";
      return result;
    }
    const char* kindId = pointOpKindId(op.pointKind);
    if (kindId == nullptr) {
      result.error = "refused: entry " + std::to_string(i + 1) + " of layer \"" + layer.name +
                     "\" has a point-op kind this build has no stable id for, so it cannot be "
                     "named in a file. This is a build inconsistency, not a document problem: "
                     "report it.";
      return result;
    }

    JsonValue params = JsonValue::object();
    params.set(kKindKey, JsonValue::string(kindId));
    params.set(kEnabledKey, JsonValue::boolean(op.enabled));
    result.action.steps.push_back(Command{kAddLayerOpId, std::move(params)});
  }

  // The `select_layer` step goes in FIRST and is emitted exactly once, ahead
  // of the op steps -- built here rather than before the loop so that a
  // refusal above returns an empty action rather than a one-step one.
  //
  // **Once, and none of the op steps repeats the layer name.** The steps that
  // follow carry no `"layer"` key at all, which `resolveTarget()` reads as
  // "whatever `select_layer` last chose". The alternative -- naming the layer
  // on every step -- is more robust against a step being reordered and much
  // worse against the thing that actually happens to these files, which is a
  // human retargeting one at another layer (PRD P5): with the name in six
  // places, five of them keep pointing at the old layer and the action still
  // runs.
  Command select;
  select.id = kSelectLayerId;
  select.params.set(kLayerKey, JsonValue::string(layer.name));
  result.action.steps.insert(result.action.steps.begin(), std::move(select));

  // The two honest limits of this converter, reported rather than assumed.
  //
  // A converted step carries the op's KIND and whether it is enabled, and not
  // the op's own parameters -- the gamma of a Levels, the points of a Curve.
  // Those live in `add_layer_op`'s parameter encoding, which belongs to the
  // registration named at the top of this function and does not exist on this
  // branch. Saying so in a warning is the difference between a user who knows
  // their curve did not come along and one who finds out from the output.
  if (!result.action.steps.empty() && layer.ops.size() > 0) {
    result.warnings.push_back(
        "layer \"" + layer.name + "\": each of the " + std::to_string(layer.ops.size()) +
        " converted grade steps carries its kind and its enabled flag, and not the op's own "
        "parameter values -- those are carried by the \"" + std::string(kAddLayerOpId) +
        "\" command's parameter encoding, which this build does not register. Re-convert with a "
        "build that does before relying on the result.");
  }
  if (findCommand(kAddLayerOpId) == nullptr && layer.ops.size() > 0) {
    result.warnings.push_back(
        "this build has no command called \"" + std::string(kAddLayerOpId) +
        "\", so the action below will be refused by name when it is loaded. That refusal is "
        "correct -- an action is executed, and a step this build cannot evaluate must not run.");
  }

  result.ok = true;
  return result;
}

}  // namespace np
