#include "app/LayerPanel.hpp"

#include <cmath>
#include <cstdio>
#include <optional>

#include "core/Blend.hpp"

namespace np {

size_t layerIndexForPanelRow(size_t row, size_t layerCount) noexcept {
  if (layerCount == 0 || row >= layerCount) return 0;
  return layerCount - 1 - row;
}

size_t panelRowForLayerIndex(size_t layerIndex, size_t layerCount) noexcept {
  if (layerCount == 0 || layerIndex >= layerCount) return 0;
  return layerCount - 1 - layerIndex;
}

const char* layerKindGlyph(LayerKind kind) noexcept {
  switch (kind) {
    case LayerKind::Pigment: return "\xE2\x97\x89";     // U+25C9 fisheye
    case LayerKind::RGB: return "\xE2\x96\xA1";         // U+25A1 white square
    case LayerKind::Media: return "\xE2\x97\x88";       // U+25C8 white diamond, black small
    case LayerKind::Strokes: return "\xE2\x9C\x82";     // U+2702 scissors
    case LayerKind::Adjustment: return "\xE2\x96\xA4";  // U+25A4 square, horizontal fill
    case LayerKind::Text: return "T";
    case LayerKind::Flats: return "\xE2\x96\xA9";       // U+25A9 square, orthogonal fill
  }
  return "?";
}

std::string layerRowTitle(const Layer& layer, size_t layerIndex) {
  if (!layer.name.empty()) return layer.name;
  return "Layer " + std::to_string(layerIndex + 1);
}

std::string layerRowSubLine(const Layer& layer) {
  // U+00B7 MIDDLE DOT, docs/ui.md's own separator.
  static constexpr const char* kSep = " \xC2\xB7 ";

  std::string s = layerKindName(layer.kind);
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

  s += kSep;
  std::string blend = layer.blend;
  for (char& c : blend)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  s += blend.empty() ? "?" : blend;
  if (!blendIsImplemented(layer.blend)) s += " (!)";
  // PRD B7, on the row as well as in the dropdown, and from the same field
  // (`BlendModeInfo::space`) rather than from a second list here. A row is
  // where a user reads what a layer does, so a display-referred mode that was
  // labelled only while the dropdown happened to be open would not be
  // "labelled as such" in any useful sense.
  if (const std::optional<BlendMode> mode = blendModeFromName(layer.blend))
    if (blendModeInfo(*mode).space == BlendSpace::DisplayReferred) s += " (display-referred)";

  // Whole percent, rounded to nearest with ties going up. An opacity outside [0,1]
  // cannot be set through core/LayerOps and cannot be saved, but it can exist
  // in memory (Layer is a plain aggregate), so this prints what is there rather
  // than what ought to be -- a row that showed "100%" for a stored 1.5 would
  // hide the very thing a reader is looking at the panel to find.
  char pct[32];
  std::snprintf(pct, sizeof(pct), "%.0f%%",
                std::floor(static_cast<double>(layer.opacity) * 100.0 + 0.5));
  s += kSep;
  s += pct;

  if (!layer.visible) {
    s += kSep;
    s += "HIDDEN";
  }
  if (layer.locked) {
    s += kSep;
    s += "LOCKED";
  }
  return s;
}

std::vector<BlendMode> blendMenuForLayer(const Document& doc, size_t layerIndex) {
  std::vector<BlendMode> out;
  if (layerIndex >= doc.layers.size()) return out;
  for (const BlendModeInfo& info : allBlendModes())
    if (blendModeAvailableForLayer(doc, layerIndex, info.mode)) out.push_back(info.mode);
  return out;
}

std::string blendMenuEntryText(BlendMode mode) {
  const BlendModeInfo& info = blendModeInfo(mode);
  std::string s = info.label;
  // PRD B7, read straight off the data. Two spaces rather than one so the
  // marker reads as an annotation on the label and not as part of it.
  if (info.space == BlendSpace::DisplayReferred) s += "  (display-referred)";
  // The "(not composited yet)" marker step 2 put here for `Mix` is gone,
  // because as of PLAN.md Phase 5 step 3 `Mix` is composited -- and it is
  // composited in exactly the situation this menu offers it in, since
  // `blendMenuForLayer()` filters through the same PRD L5 predicate
  // (`blendModeAvailableForLayer()`) that decides whether a mix can be
  // formed. So a mode reachable from this menu is a mode that works; the
  // marker would now be a warning about nothing.
  //
  // The mechanism it belonged to is not gone with it: `layerRowSubLine()`
  // still marks a layer whose carried `np:blend` this build cannot honour
  // with `(!)`, which is the case that genuinely still exists -- a newer
  // build's name, arriving from a file, which the dropdown never offers.
  if (!info.compositesPixels && !info.compositesLatents) s += "  (not composited yet)";
  return s;
}

size_t blendMenuSelection(const Document& doc, size_t layerIndex,
                          const std::vector<BlendMode>& menu) {
  if (layerIndex >= doc.layers.size()) return menu.size();
  const std::string& carried = doc.layers[layerIndex].blend;
  for (size_t i = 0; i < menu.size(); ++i)
    if (carried == blendModeName(menu[i])) return i;
  return menu.size();
}

}  // namespace np
