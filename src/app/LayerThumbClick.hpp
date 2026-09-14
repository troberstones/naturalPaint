#pragma once

#include <cstddef>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "core/Layer.hpp"

// app/LayerThumbClick: what a click on a layer row's two thumbnails means
// (T16), with the modifiers already read. ui/MacPaintUI issues the buttons and
// reads Shift/Alt; everything the click changes is decided here.
namespace np {

enum class LayerThumb { Content, Mask };

enum class LayerThumbAction {
  None,
  TargetContent,      // aim the pen at the layer's pixels; leaves mask view
  TargetMask,         // aim the pen at the mask
  ToggleMaskEnabled,  // Shift-click on the mask
  ToggleMaskView,     // Option-click on the mask
};

struct LayerThumbClickResult {
  LayerThumbAction action = LayerThumbAction::None;
  bool selectsRow = false;  // the panel should make this row its selection
  std::string error;        // a refused toggle, verbatim
};

// Photoshop's rules. Option wins over Shift; Shift-click does not select the
// row; a click on a maskless row's mask slot reads as the layer thumbnail.
LayerThumbClickResult applyLayerThumbClick(OpenDocument& od, size_t layerIndex, LayerThumb which,
                                           bool shift, bool alt);

// The layer whose mask the canvas shows alone, or null. Only while the layer
// Option-clicked is still the active one and still has a mask, so selecting
// another row leaves the view without a second writer of the index.
const Layer* maskViewLayer(const OpenDocument& od);

}  // namespace np
