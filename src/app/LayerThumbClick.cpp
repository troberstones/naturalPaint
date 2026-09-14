#include "app/LayerThumbClick.hpp"

#include "core/LayerOps.hpp"

namespace np {

LayerThumbClickResult applyLayerThumbClick(OpenDocument& od, size_t layerIndex, LayerThumb which,
                                           bool shift, bool alt) {
  LayerThumbClickResult r;
  if (layerIndex >= od.document.layers.size()) return r;
  const Layer& layer = od.document.layers[layerIndex];

  if (which == LayerThumb::Content || !layer.mask.has_value()) {
    setActiveLayer(od, layerIndex);
    od.maskIsEditTarget = false;
    od.maskViewLayerIndex.reset();
    r.action = LayerThumbAction::TargetContent;
    r.selectsRow = true;
    return r;
  }

  if (alt) {
    const bool viewingThis = maskViewLayer(od) == &layer;
    setActiveLayer(od, layerIndex);
    od.maskIsEditTarget = true;
    if (viewingThis) {
      od.maskViewLayerIndex.reset();
    } else {
      od.maskViewLayerIndex = layerIndex;
      od.viewedChannelName.reset();  // one replacement view at a time
    }
    r.action = LayerThumbAction::ToggleMaskView;
    r.selectsRow = true;
    return r;
  }

  if (shift) {
    const DocumentOpResult out =
        recordLayerEdit(od, setLayerMaskEnabled(od.document, layerIndex, !layer.maskEnabled));
    r.action = LayerThumbAction::ToggleMaskEnabled;
    if (!out.ok) r.error = out.error;
    return r;
  }

  setActiveLayer(od, layerIndex);
  od.maskIsEditTarget = true;
  r.action = LayerThumbAction::TargetMask;
  r.selectsRow = true;
  return r;
}

const Layer* maskViewLayer(const OpenDocument& od) {
  if (!od.maskViewLayerIndex.has_value()) return nullptr;
  const std::optional<size_t> active = activeLayerIndex(od);
  if (!active.has_value() || *active != *od.maskViewLayerIndex) return nullptr;
  if (*active >= od.document.layers.size()) return nullptr;
  const Layer& layer = od.document.layers[*active];
  return layer.mask.has_value() ? &layer : nullptr;
}

}  // namespace np
