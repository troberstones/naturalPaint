#include "core/LayerOps.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace np {
namespace {

LayerOpResult fail(std::string message) {
  LayerOpResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

LayerOpResult succeed(std::string label, size_t index) {
  LayerOpResult r;
  r.ok = true;
  r.editLabel = std::move(label);
  r.index = index;
  return r;
}

// "layer 2 (\"Line pass\")" -- how every refusal and every edit label below
// names a layer. The index is always present because it is the only thing
// guaranteed to identify the layer (names are not unique and may be empty).
std::string describe(const Document& doc, size_t index) {
  std::string s = "layer " + std::to_string(index);
  if (index < doc.layers.size() && !doc.layers[index].name.empty())
    s += " (\"" + doc.layers[index].name + "\")";
  return s;
}

// The one bounds check, so every operation refuses an out-of-range index with
// the same sentence rather than five slightly different ones.
bool inRange(const Document& doc, size_t index, const char* what, LayerOpResult* out) {
  if (index < doc.layers.size()) return true;
  *out = fail(std::string(what) + " refused: there is no layer at index " +
              std::to_string(index) + " -- this document has " +
              std::to_string(doc.layers.size()) +
              " layer(s), indexed from 0 at the bottom of the stack.");
  return false;
}

// The lock refusal, in one place. `what` is the operation's own noun, so the
// message reads "remove layer refused: ...".
bool notLocked(const Document& doc, size_t index, const char* what, LayerOpResult* out) {
  if (!doc.layers[index].locked) return true;
  *out = fail(std::string(what) + " refused: " + describe(doc, index) +
              " is locked. A locked layer's content and its own place in the stack are frozen "
              "-- only its visibility and the lock itself can still be changed (core/LayerOps.hpp). "
              "Unlock it first.");
  return false;
}

}  // namespace

std::string defaultNewLayerName(const Document& doc) {
  // Scans for the highest existing "Layer N" and goes one above it, so the
  // name handed out is not one already visible in the panel. Deliberately not
  // "layers.size() + 1", which collides the moment a layer is deleted from the
  // middle of the stack.
  long highest = 0;
  for (const Layer& layer : doc.layers) {
    if (layer.name.rfind("Layer ", 0) != 0) continue;
    const std::string digits = layer.name.substr(6);
    if (digits.empty()) continue;
    bool allDigits = true;
    for (const char c : digits)
      if (c < '0' || c > '9') allDigits = false;
    if (!allDigits) continue;
    // No exception path needed: `digits` is known to be a non-empty run of
    // ASCII digits, and an absurdly long one saturates rather than throwing.
    long value = 0;
    for (const char c : digits) {
      value = value * 10 + (c - '0');
      if (value > 1000000) break;
    }
    if (value > highest) highest = value;
  }
  return "Layer " + std::to_string(highest + 1);
}

Layer makeRgbLayer(std::string name) {
  Layer layer;
  layer.kind = LayerKind::RGB;
  layer.rgbTiles.emplace();
  layer.name = std::move(name);
  return layer;
}

Layer makePigmentLayer(std::string name) {
  Layer layer;
  layer.kind = LayerKind::Pigment;
  layer.pigmentTiles.emplace();
  layer.name = std::move(name);
  return layer;
}

LayerOpResult addLayer(Document& doc, size_t index, Layer layer) {
  if (index > doc.layers.size()) {
    return fail("add layer refused: index " + std::to_string(index) +
                " is past the end of a " + std::to_string(doc.layers.size()) +
                "-layer document. An insert at exactly " +
                std::to_string(doc.layers.size()) +
                " appends to the top of the stack; anything beyond that is a caller error, "
                "not a request to append.");
  }
  const std::string name = layer.name;
  doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
  return succeed(name.empty() ? "add layer" : "add layer \"" + name + "\"", index);
}

LayerOpResult removeLayer(Document& doc, size_t index) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "remove layer", &refusal)) return refusal;
  if (!notLocked(doc, index, "remove layer", &refusal)) return refusal;
  const std::string label = "remove " + describe(doc, index);
  doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(index));
  return succeed(label, index);
}

LayerOpResult moveLayer(Document& doc, size_t from, size_t to) {
  LayerOpResult refusal;
  if (!inRange(doc, from, "move layer", &refusal)) return refusal;
  if (!inRange(doc, to, "move layer", &refusal)) return refusal;
  if (!notLocked(doc, from, "move layer", &refusal)) return refusal;
  const std::string label = "move " + describe(doc, from) + " to index " + std::to_string(to);
  if (from == to) return succeed(label, to);

  // Rotate rather than erase-then-insert: a rotate never destroys and rebuilds
  // the moved element, and it is the operation this actually is -- everything
  // between `from` and `to` shifts by one and the moved layer lands on the far
  // side. `core/OpStack::reorder()` is erase-and-insert and documents that
  // references into it are invalidated by a reorder; the same is true here, and
  // ui/MacPaintUI's panel takes the same "stop iterating this frame" precaution
  // drawGradeSection() already takes for that reason.
  const auto begin = doc.layers.begin();
  if (from < to) {
    std::rotate(begin + static_cast<std::ptrdiff_t>(from),
                begin + static_cast<std::ptrdiff_t>(from) + 1,
                begin + static_cast<std::ptrdiff_t>(to) + 1);
  } else {
    std::rotate(begin + static_cast<std::ptrdiff_t>(to),
                begin + static_cast<std::ptrdiff_t>(from),
                begin + static_cast<std::ptrdiff_t>(from) + 1);
  }
  return succeed(label, to);
}

LayerOpResult duplicateLayer(Document& doc, size_t index) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "duplicate layer", &refusal)) return refusal;
  // Deliberately no lock check -- duplicating reads the source and changes
  // nothing about it. See core/LayerOps.hpp's lock section.
  const std::string label = "duplicate " + describe(doc, index);
  Layer copy = doc.layers[index];  // deep copy: TileStore is a plain map of owned tiles
  if (!copy.name.empty()) copy.name += " copy";
  doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                    std::move(copy));
  return succeed(label, index + 1);
}

LayerOpResult setLayerVisible(Document& doc, size_t index, bool visible) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer visibility", &refusal)) return refusal;
  // No lock check: hiding a locked layer is allowed. See the header.
  doc.layers[index].visible = visible;
  return succeed(std::string(visible ? "show " : "hide ") + describe(doc, index), index);
}

LayerOpResult setLayerOpacity(Document& doc, size_t index, float opacity) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer opacity", &refusal)) return refusal;
  if (!notLocked(doc, index, "set layer opacity", &refusal)) return refusal;
  if (!(opacity >= 0.0f && opacity <= 1.0f)) {
    // `!(a && b)` rather than `a < 0 || a > 1` so a NaN -- which compares false
    // against everything -- is refused rather than passing both tests.
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "set layer opacity refused: %.6g is outside [0, 1]. io/NpaintFile refuses to "
                  "save an opacity outside that range by name (PRD I11), so accepting it here "
                  "would only move the refusal to the next save.",
                  static_cast<double>(opacity));
    return fail(buf);
  }
  doc.layers[index].opacity = opacity;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "set opacity to %.0f%%", static_cast<double>(opacity) * 100.0);
  return succeed(std::string(buf) + " on " + describe(doc, index), index);
}

LayerOpResult setLayerLocked(Document& doc, size_t index, bool locked) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer lock", &refusal)) return refusal;
  // No lock check: a lock that cannot be removed is not a lock.
  const std::string label = std::string(locked ? "lock " : "unlock ") + describe(doc, index);
  doc.layers[index].locked = locked;
  return succeed(label, index);
}

LayerOpResult setLayerBlend(Document& doc, size_t index, BlendMode mode) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer blend mode", &refusal)) return refusal;
  if (!notLocked(doc, index, "set layer blend mode", &refusal)) return refusal;
  if (!blendModeAvailableForLayer(doc, index, mode)) {
    // Today this can only be `Mix` (it is the only mode with
    // `pigmentPairOnly`), but the sentence is written from the metadata rather
    // than hard-coding that, so a second restricted mode would not need a
    // second message.
    return fail("set layer blend mode refused: \"" + std::string(blendModeName(mode)) +
                "\" is not available on " + describe(doc, index) +
                ". PRD L5: `Mix` appears only between two Pigment layers -- the layer and the "
                "one directly beneath it must both be Pigment layers, so the bottom layer can "
                "never take it.");
  }
  const std::string label = "set " + describe(doc, index) + " to blend mode \"" +
                            blendModeName(mode) + "\"";
  doc.layers[index].blend = blendModeName(mode);
  return succeed(label, index);
}

LayerOpResult addLayerMask(Document& doc, size_t index) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "add layer mask", &refusal)) return refusal;
  if (!notLocked(doc, index, "add layer mask", &refusal)) return refusal;
  if (doc.layers[index].mask.has_value()) {
    return fail("add layer mask refused: " + describe(doc, index) +
                " already has a mask. There is exactly one mask slot per layer (PRD C4's "
                "\"per-layer mask\"), and silently replacing the existing one would discard "
                "every texel in it. Remove the mask first if that is what was meant.");
  }
  // Engaged with zero tiles: an unallocated mask tile means 1.0 (core/Mask.hpp),
  // so "reveal all" is genuinely free -- no allocation at all, which is PRD C2
  // applied to a mask. It is `mask.emplace()` and not a canvas-sized fill for
  // exactly that reason.
  doc.layers[index].mask.emplace();
  return succeed("add mask to " + describe(doc, index), index);
}

LayerOpResult removeLayerMask(Document& doc, size_t index) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "remove layer mask", &refusal)) return refusal;
  if (!notLocked(doc, index, "remove layer mask", &refusal)) return refusal;
  if (!doc.layers[index].mask.has_value()) {
    return fail("remove layer mask refused: " + describe(doc, index) +
                " has no mask. Nothing was changed; a no-op that reported success would put a "
                "\"remove mask\" entry in the journal for an edit that did not happen.");
  }
  // The mask's tiles go with it. This is **discard**, never "apply": applying a
  // mask means baking its coverage into the layer's own alpha or mass, which is
  // a destructive edit with a different name, a different undo entry and (on a
  // Pigment layer) a different meaning -- PRD F10's mass reduction. Neither is
  // built here; see core/LayerOps.hpp.
  doc.layers[index].mask.reset();
  return succeed("remove mask from " + describe(doc, index), index);
}

LayerOpResult setLayerName(Document& doc, size_t index, std::string name) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "rename layer", &refusal)) return refusal;
  if (!notLocked(doc, index, "rename layer", &refusal)) return refusal;
  const std::string label = "rename " + describe(doc, index) + " to \"" + name + "\"";
  doc.layers[index].name = std::move(name);
  return succeed(label, index);
}

}  // namespace np
