#include "app/ToolLayerCompat.hpp"

namespace np {

bool toolNeedsPixelLayer(Tool tool) noexcept {
  switch (tool) {
    // `pixelOpRefusalFor()`'s own two direct callers.
    case Tool::PaintBucket:
    case Tool::Gradient:
    // The selection family: a `Selection` is meaningless over a layer with no
    // pixels for it to bound, and the Magic Wand probes `rgbTiles` directly.
    // None of the five has a Pigment-specific arm the way the brush family
    // does, so `rgbTiles.has_value()` alone is the right, and only, question.
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
      return true;

    // Deliberately everything else, INCLUDING the brush family -- this
    // header's own closing paragraphs carry the argument: the brush family's
    // per-layer-kind rules already live in `strokeRouteFor()` (Pigment admits
    // Brush/Dry Brush and refuses Pencil/Dodge/Burn/Clone/Heal, all by name,
    // none of it collapsible into one `rgbTiles` question), Pen/Curve/
    // PathSelect/Shape/Text exist to CREATE the layer kind this axis would
    // otherwise refuse them for, and the Eyedropper's "Current & Below"/"All
    // Layers" sources read the composite rather than the active layer's own
    // store.
    default:
      return false;
  }
}

bool toolValidForLayer(Tool tool, const Layer* layer) noexcept {
  if (layer == nullptr) return true;  // "no active layer" is not this axis's question
  if (!toolNeedsPixelLayer(tool)) return true;
  return layer->rgbTiles.has_value();
}

const char* toolLayerRefusal(Tool tool, const Layer* layer) {
  if (toolValidForLayer(tool, layer)) return nullptr;
  // One sentence for the whole family: a painter who reaches for the wand on
  // a text layer does not need to know that the underlying test is
  // `rgbTiles.has_value()`, only that this tool wants a pixel layer and the
  // active one is not one.
  return "this tool needs a pixel layer -- the active layer has none.";
}

}  // namespace np
