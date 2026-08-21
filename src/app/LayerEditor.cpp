#include "app/LayerEditor.hpp"

#include <utility>

#include "core/LayerOps.hpp"

namespace np {
namespace {

// The insertion point for a new layer: directly above the selection, or 0 in a
// document with no layers at all (which is representable -- core/LayerOps.hpp
// on why removing the last layer is allowed).
size_t insertionIndexFor(const Document& doc, size_t selected) {
  if (doc.layers.empty()) return 0;
  if (selected >= doc.layers.size()) return doc.layers.size();
  return selected + 1;
}

LayerEditResult refused(std::string error, size_t selected) {
  LayerEditResult out;
  out.ok = false;
  out.error = std::move(error);
  out.selected = selected;
  return out;
}

// One create/mutate operation, recorded, with the selection it leaves behind.
// `newSelection` is used only when the operation succeeded.
LayerEditResult record(OpenDocument& doc, LayerOpResult result, size_t selected,
                       size_t newSelection) {
  const DocumentOpResult out = recordLayerEdit(doc, std::move(result));
  if (!out.ok) return refused(out.error, selected);
  LayerEditResult r;
  r.ok = true;
  r.selected = newSelection;
  return r;
}

}  // namespace

const std::vector<LayerCommand>& allLayerCommands() {
  static const std::vector<LayerCommand> kAll = {
      LayerCommand::NewRgbLayer,    LayerCommand::NewPigmentLayer,
      LayerCommand::NewAdjustmentLayer, LayerCommand::DuplicateLayer,
      LayerCommand::DeleteLayer,    LayerCommand::MoveLayerUp,
      LayerCommand::MoveLayerDown,  LayerCommand::AddMask,
      LayerCommand::RemoveMask,     LayerCommand::ToggleVisible,
      LayerCommand::ToggleLocked,   LayerCommand::ToggleClipped,
  };
  return kAll;
}

const char* layerCommandLabel(LayerCommand command) noexcept {
  switch (command) {
    case LayerCommand::NewRgbLayer:        return "New RGB Layer";
    case LayerCommand::NewPigmentLayer:    return "New Pigment Layer";
    case LayerCommand::NewAdjustmentLayer: return "New Adjustment Layer";
    case LayerCommand::DuplicateLayer:     return "Duplicate Layer";
    case LayerCommand::DeleteLayer:        return "Delete Layer";
    case LayerCommand::MoveLayerUp:        return "Move Layer Up";
    case LayerCommand::MoveLayerDown:      return "Move Layer Down";
    case LayerCommand::AddMask:            return "Add Layer Mask";
    case LayerCommand::RemoveMask:         return "Remove Layer Mask";
    case LayerCommand::ToggleVisible:      return "Toggle Visibility";
    case LayerCommand::ToggleLocked:       return "Toggle Lock";
    case LayerCommand::ToggleClipped:      return "Clip to Layer Below";
  }
  return "?";
}

bool layerCommandAvailable(const Document& doc, LayerCommand command, size_t selected) {
  const size_t count = doc.layers.size();
  // A create is the one thing that works on an empty document -- everything
  // else needs a layer to act on.
  const bool haveSelection = selected < count;
  switch (command) {
    case LayerCommand::NewRgbLayer:
    case LayerCommand::NewPigmentLayer:
    case LayerCommand::NewAdjustmentLayer:
      return true;
    case LayerCommand::DuplicateLayer:
    case LayerCommand::DeleteLayer:
    case LayerCommand::ToggleVisible:
    case LayerCommand::ToggleLocked:
      return haveSelection;
    // "Up the panel is up the stack" -- app/LayerPanel owns the reversal, and
    // up the stack is a higher index.
    case LayerCommand::MoveLayerUp:
      return haveSelection && selected + 1 < count;
    case LayerCommand::MoveLayerDown:
      return haveSelection && selected > 0;
    case LayerCommand::AddMask:
      return haveSelection && !doc.layers[selected].mask.has_value();
    case LayerCommand::RemoveMask:
      return haveSelection && doc.layers[selected].mask.has_value();
    // The bottom layer has nothing below it to be clipped by, which is a
    // property of the row rather than of the stack -- the same idiom the panel
    // already uses to disable Up on the top row. Every other reason a clip
    // cannot be taken is a refusal with a sentence; see the header.
    // Un-clipping is always available on a clipped layer, including one that
    // arrived at index 0 from a file.
    case LayerCommand::ToggleClipped:
      return haveSelection && (selected > 0 || doc.layers[selected].clipped);
  }
  return false;
}

LayerEditResult applyLayerCommand(OpenDocument& od, LayerCommand command, size_t selected) {
  Document& doc = od.document;
  const size_t count = doc.layers.size();
  const size_t at = insertionIndexFor(doc, selected);

  switch (command) {
    case LayerCommand::NewRgbLayer:
      return record(od, addLayer(doc, at, makeRgbLayer(defaultNewLayerName(doc))), selected, at);
    case LayerCommand::NewPigmentLayer:
      return record(od, addLayer(doc, at, makePigmentLayer(defaultNewLayerName(doc))), selected,
                    at);
    case LayerCommand::NewAdjustmentLayer:
      return record(od, addLayer(doc, at, makeAdjustmentLayer(defaultNewLayerName(doc))), selected,
                    at);
    case LayerCommand::DuplicateLayer:
      // The copy lands directly above the source (core/LayerOps.hpp), and the
      // selection follows it there -- duplicating and then immediately editing
      // the original is not what the gesture means.
      return record(od, duplicateLayer(doc, selected), selected, selected + 1);
    case LayerCommand::DeleteLayer: {
      // The row that takes the deleted one's place: the layer below it, or the
      // new top of a stack whose top was just removed. Computed from the
      // *pre-delete* count, and never wrapped through unsigned subtraction on
      // an empty document.
      const size_t after = (selected > 0) ? selected - 1 : 0;
      return record(od, removeLayer(doc, selected), selected, count > 1 ? after : 0);
    }
    case LayerCommand::MoveLayerUp:
      return record(od, moveLayer(doc, selected, selected + 1), selected, selected + 1);
    case LayerCommand::MoveLayerDown:
      // `selected == 0` would wrap; core/LayerOps would then refuse an
      // enormous index by name, which is a true sentence about a number the
      // user never typed. Refused here instead, in the panel's own vocabulary.
      if (selected == 0 || selected >= count)
        return refused("move layer refused: " +
                           std::string(count == 0 ? "this document has no layers"
                                                  : "the bottom layer is already at the bottom of "
                                                    "the stack") +
                           ". Nothing was changed.",
                       selected);
      return record(od, moveLayer(doc, selected, selected - 1), selected, selected - 1);
    case LayerCommand::AddMask:
      return record(od, addLayerMask(doc, selected), selected, selected);
    case LayerCommand::RemoveMask:
      return record(od, removeLayerMask(doc, selected), selected, selected);
    // The three toggles read the layer's current value, which means they must
    // not read `doc.layers[selected]` before it is known to exist. Out of
    // range, each hands core/LayerOps the index with a placeholder value: the
    // bounds check refuses before the value is looked at, so the refusal is
    // core's own sentence with the numbers in it rather than a second one
    // invented here.
    case LayerCommand::ToggleVisible:
      return record(od,
                    setLayerVisible(doc, selected,
                                    selected < count ? !doc.layers[selected].visible : true),
                    selected, selected);
    case LayerCommand::ToggleLocked:
      return record(od,
                    setLayerLocked(doc, selected,
                                   selected < count ? !doc.layers[selected].locked : true),
                    selected, selected);
    case LayerCommand::ToggleClipped:
      return record(od,
                    setLayerClipped(doc, selected,
                                    selected < count ? !doc.layers[selected].clipped : true),
                    selected, selected);
  }
  return refused("unknown layer command.", selected);
}

Op makeNewOp(PointOpKind kind) {
  Op op;
  op.opClass = OpClass::PointA;
  op.pointKind = kind;
  op.enabled = false;  // see the header: build, then reveal
  return op;
}

}  // namespace np
