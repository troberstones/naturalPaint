#include "ui/TransformCompositeSplit.hpp"

#include "core/Layer.hpp"

namespace np {

namespace {

// A layer that composites as a plain `over` of its own pixels, reading
// nothing underneath it. This is the whole exactness question, per layer.
//
// `parent` is checked as well as `kind`: a layer inside a group composites
// into that group's own accumulator and is then blended as the group, so
// isolating it from its group changes where its pixels land even when the
// layer itself is an ordinary `normal` RGB layer.
bool compositesAsPlainOver(const Layer& l) noexcept {
  if (l.kind == LayerKind::Adjustment) return false;  // reads the backdrop, by definition
  if (l.kind == LayerKind::Group) return false;       // its own accumulator, its own blend
  if (l.blend != kDefaultBlendName) return false;     // every other mode reads the backdrop
  if (l.clipped) return false;                        // meaning depends on the layer beneath
  if (!l.parent.empty()) return false;                // inside a group; see above
  return true;
}

}  // namespace

bool transformSplitIsExact(const Document& doc, size_t layerIndex) noexcept {
  if (layerIndex >= doc.layers.size()) return false;

  // The transformed layer being inside a group is disqualifying on its own:
  // the below-half would composite that group without one of its members and
  // then blend the group, which is not "the picture minus one layer".
  if (!doc.layers[layerIndex].parent.empty()) return false;

  for (size_t i = layerIndex + 1; i < doc.layers.size(); ++i) {
    const Layer& l = doc.layers[i];
    if (!l.visible) continue;  // draws nothing either way
    if (!compositesAsPlainOver(l)) return false;
  }
  return true;
}

bool anyVisibleLayerAbove(const Document& doc, size_t layerIndex) noexcept {
  if (layerIndex >= doc.layers.size()) return false;
  for (size_t i = layerIndex + 1; i < doc.layers.size(); ++i) {
    if (doc.layers[i].visible) return true;
  }
  return false;
}

Document documentWithLayerHidden(const Document& doc, size_t layerIndex) {
  Document out = doc;
  if (layerIndex < out.layers.size()) out.layers[layerIndex].visible = false;
  return out;
}

Document documentWithLayersAtOrAboveHidden(const Document& doc, size_t layerIndex) {
  Document out = doc;
  for (size_t i = layerIndex; i < out.layers.size(); ++i) out.layers[i].visible = false;
  return out;
}

Document documentWithLayersAtOrBelowHidden(const Document& doc, size_t layerIndex) {
  Document out = doc;
  const size_t last = layerIndex < out.layers.size() ? layerIndex : out.layers.size();
  for (size_t i = 0; i <= last && i < out.layers.size(); ++i) out.layers[i].visible = false;
  return out;
}

}  // namespace np
