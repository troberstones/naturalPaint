#include "app/CommandSupport.hpp"
#include "core/Blend.hpp"
#include "core/LayerOps.hpp"

// app/CommandsLayers -- the command rows for layer structure and layer
// properties: `allLayerCommands()`'s gestures, `allLayerSetCommands()`'s
// multi-selection gestures, and core/LayerOps' value setters.
//
// The two flat command enums are already walked tables, which is what makes
// registering them mechanical rather than a judgement call per row -- the
// discipline app/LayerEditor.hpp established so that "a command not in this
// enum is a command exactly one menu can reach".
namespace np {
namespace {

CommandResult doSelectLayer(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  setActiveLayer(doc, index);
  CommandResult r;
  r.ok = true;
  r.status = "select layer: \"" + doc.document.layers[index].name + "\"";
  return r;
}

CommandResult doNewRgbLayer(OpenDocument& doc, const JsonValue&) {
  const LayerEditResult e = applyLayerCommand(doc, LayerCommand::NewRgbLayer, doc.activeLayer);
  return fromLayerEdit(doc, e, "new RGB layer");
}

CommandResult doFlattenImage(OpenDocument& doc, const JsonValue&) {
  const LayerEditResult e = applyLayerCommand(doc, LayerCommand::FlattenImage, doc.activeLayer);
  return fromLayerEdit(doc, e, "flatten image");
}

CommandResult doSetLayerBlend(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  const std::string modeName = params.stringOr("mode", "");
  const std::optional<BlendMode> mode = blendModeFromName(modeName);
  if (!mode) return commandRefused("refused: no blend mode is named \"" + modeName + "\".");
  return fromDocumentOpResult(recordLayerEdit(doc, setLayerBlend(doc.document, index, *mode)),
                              "set blend mode");
}

}  // namespace

void registerLayerCommandRows(std::vector<CommandSpec>* out) {
  out->push_back({"select_layer", "Select Layer", {"layer"}, layerTargetUnavailable, doSelectLayer});
  out->push_back({"new_rgb_layer", "New RGB Layer", {}, documentAlwaysAvailable, doNewRgbLayer});
  out->push_back({"flatten_image", "Flatten Image", {}, documentUnavailable, doFlattenImage});
  out->push_back({"set_layer_blend", "Set Blend Mode", {"layer", "mode"}, layerTargetUnavailable,
                  doSetLayerBlend});
}

}  // namespace np
