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

// The op-index bounds check, the same shape as inRange() above. Separate
// because it names a different list -- "op 4 of layer 2" rather than "layer 4"
// -- and because it is checked *after* the layer index, so it can index
// `doc.layers[index]` safely.
bool opInRange(const Document& doc, size_t index, size_t opIndex, const char* what,
               LayerOpResult* out) {
  const size_t count = doc.layers[index].ops.size();
  if (opIndex < count) return true;
  *out = fail(std::string(what) + " refused: there is no op at index " +
              std::to_string(opIndex) + " on " + describe(doc, index) + " -- its stack has " +
              std::to_string(count) +
              " op(s), indexed from 0 at the bottom (evaluated first). core::OpStack throws on "
              "an out-of-range index; a refusal is what a panel row that went stale under a "
              "delete should get instead.");
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

Layer makeAdjustmentLayer(std::string name) {
  Layer layer;
  layer.kind = LayerKind::Adjustment;
  // No `rgbTiles`, no `pigmentTiles`, no `mask` and an empty `ops` -- see
  // core/LayerOps.hpp. Every one of those absences is the kind's definition
  // rather than an omission.
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
  // Dragging a clipped layer *to* the bottom is refused: `setLayerClipped()`
  // refuses to put a clip there, and a drag that did it anyway would make that
  // refusal decorative. See core/LayerOps.hpp on why this is a refusal rather
  // than a silent un-clip.
  //
  // **This is not the only reorder that can reach a baseless clip, and it is
  // deliberately not made so.** Moving a *base* out from under its run --
  // `moveLayer(0, 2)` on `[base, clipped, top]` -- leaves the clipped layer at
  // index 0 with nothing below it, and that move is allowed. Refusing it would
  // let one layer's flag veto a reorder of a different layer, which is a worse
  // rule than the state it would prevent: the state is already one a document
  // must tolerate, because PRD I10 says a file may carry any flag this build
  // did not write. So the compositor is the safety net rather than a second
  // gate -- `core::clipRuns()` marks it `clippedWithoutBase`, composites it
  // unclipped, and warns by name. `--selftest` asserts that orphaning path
  // directly, so "allowed, and degrades loudly" is a checked claim and not an
  // oversight.
  if (to == 0 && from != 0 && doc.layers[from].clipped) {
    return fail("move layer refused: " + describe(doc, from) +
                " is clipped, and index 0 is the bottom of a " +
                std::to_string(doc.layers.size()) +
                "-layer stack. PRD C9 clips a layer by \"the alpha of the layer below it\", and "
                "at index 0 there is no layer below. Un-clip it first, or move it to index 1 or "
                "above. Nothing was changed -- clearing the clip for you would make a drag "
                "change a layer's properties as a side effect.");
  }
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
  // The one member a copy must NOT inherit: `Layer::id` is an identity, and two
  // layers sharing one would make every layer comp that names it ambiguous
  // (core/Layer.hpp). 0 means unassigned, so the copy gets a fresh id from
  // `normalizeLayerIds()` the next time one is needed -- and the *source* keeps
  // the id every existing comp already refers to, which is what makes
  // duplicating a layer invisible to the comps that mention it.
  copy.id = 0;
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

LayerOpResult setLayerClipped(Document& doc, size_t index, bool clipped) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer clipping", &refusal)) return refusal;
  if (!notLocked(doc, index, "set layer clipping", &refusal)) return refusal;

  // Every refusal below is a reason a layer may not *become* clipped. None of
  // them is a reason it may not stop being clipped, so un-clipping skips them
  // all -- a state a document can hold (from a file, PRD I10) must always be
  // one a user can get out of.
  if (clipped) {
    if (index == 0) {
      return fail("set layer clipping refused: " + describe(doc, index) +
                  " is the bottom layer of a " + std::to_string(doc.layers.size()) +
                  "-layer stack. PRD C9 clips a layer by \"the alpha of the layer below it\", "
                  "and at index 0 there is no layer below -- so there is no alpha to clip by. "
                  "Layers are indexed from 0 at the bottom of the stack.");
    }
    // The base a clip would use: the nearest layer below that is not itself
    // clipped. Derived here exactly as core/Composite's `clipRuns()` derives
    // it, so the setter cannot accept a clip the compositor would then have to
    // warn about.
    size_t base = index;
    while (base > 0 && doc.layers[base - 1].clipped) --base;
    if (base == 0) {
      return fail("set layer clipping refused: every layer beneath " + describe(doc, index) +
                  " is already clipped, down to layer 0, so the whole run has no base. A "
                  "clipped layer is never another clipped layer's base -- a run of them clips "
                  "to one alpha rather than eroding each other (core/Composite.hpp §12) -- so "
                  "clipping this one would leave it with nothing to clip to.");
    }
    const Layer& below = doc.layers[base - 1];
    const bool holdsPixels = (below.kind == LayerKind::RGB && below.rgbTiles.has_value()) ||
                             (below.kind == LayerKind::Pigment && below.pigmentTiles.has_value());
    if (!holdsPixels) {
      return fail("set layer clipping refused: the nearest layer beneath " +
                  describe(doc, index) + " that is not itself clipped is " +
                  describe(doc, base - 1) + ", a " + std::string(layerKindName(below.kind)) +
                  " layer, which holds no pixels and therefore has no alpha to clip by. "
                  "Clipping is not resolved by searching further down the stack, because that "
                  "would clip to something other than the layer below (PRD C9).");
    }
    // PRD L5 against PRD C9. `mixPairing()` is asked rather than the blend
    // strings inspected here, so the setter and the compositor agree by
    // construction about which pairs actually exist.
    const MixPairing pairing = mixPairing(doc);
    const bool inPair = pairing.mixedWithBelow[index] || pairing.consumedByAbove[index];
    if (inPair) {
      return fail("set layer clipping refused: " + describe(doc, index) +
                  " is currently half of a `Mix` pair, and a mix and a clip are two different "
                  "relationships with the same neighbour -- a mix composites the two layers as "
                  "one unit (PRD L5), while a clip makes the lower one the alpha that decides "
                  "where the upper one shows (PRD C9). Change the blend mode away from \"" +
                  std::string(blendModeName(BlendMode::Mix)) + "\" first.");
    }
  }

  const std::string label =
      std::string(clipped ? "clip " : "unclip ") + describe(doc, index);
  doc.layers[index].clipped = clipped;
  return succeed(label, index);
}

LayerOpResult setLayerName(Document& doc, size_t index, std::string name) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "rename layer", &refusal)) return refusal;
  if (!notLocked(doc, index, "rename layer", &refusal)) return refusal;
  const std::string label = "rename " + describe(doc, index) + " to \"" + name + "\"";
  doc.layers[index].name = std::move(name);
  return succeed(label, index);
}

// --- Organisation rather than appearance (PLAN.md Phase 5 step 11; PRD C15)
//
// Both deliberately skip the lock check; the header's lock section says why.

LayerOpResult setLayerColorLabel(Document& doc, size_t index, std::string colorLabel) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer colour label", &refusal)) return refusal;
  const std::string label =
      (colorLabel.empty() ? "clear colour label on " : "label " + colorLabel + " on ") +
      describe(doc, index);
  doc.layers[index].colorLabel = std::move(colorLabel);
  return succeed(label, index);
}

LayerOpResult setLayerLinkGroup(Document& doc, size_t index, uint64_t group) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "set layer link", &refusal)) return refusal;
  const std::string label = (group == 0 ? "unlink " : "link ") + describe(doc, index);
  doc.layers[index].linkGroup = group;
  return succeed(label, index);
}

// --- The layer's own op stack ---------------------------------------------
//
// See core/LayerOps.hpp for why these are five functions and not one setter.
// Each is: the layer bounds check, the lock, the *op* bounds check, then the
// matching core::OpStack mutator. The op bounds check exists because OpStack
// indexes through `std::vector::at` and throws; a UI must get a sentence.

LayerOpResult addLayerOp(Document& doc, size_t index, Op op) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "add op", &refusal)) return refusal;
  if (!notLocked(doc, index, "add op", &refusal)) return refusal;
  const std::string label = "add " + opDisplayName(op) + " to " + describe(doc, index);
  doc.layers[index].ops.add(std::move(op));
  return succeed(label, index);
}

LayerOpResult removeLayerOp(Document& doc, size_t index, size_t opIndex) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "delete op", &refusal)) return refusal;
  if (!notLocked(doc, index, "delete op", &refusal)) return refusal;
  if (!opInRange(doc, index, opIndex, "delete op", &refusal)) return refusal;
  const std::string label = "delete " + opDisplayName(doc.layers[index].ops.at(opIndex)) +
                            " from " + describe(doc, index);
  doc.layers[index].ops.remove(opIndex);
  return succeed(label, index);
}

LayerOpResult moveLayerOp(Document& doc, size_t index, size_t from, size_t to) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "move op", &refusal)) return refusal;
  if (!notLocked(doc, index, "move op", &refusal)) return refusal;
  if (!opInRange(doc, index, from, "move op", &refusal)) return refusal;
  if (!opInRange(doc, index, to, "move op", &refusal)) return refusal;
  const std::string label = "move " + opDisplayName(doc.layers[index].ops.at(from)) +
                            " to position " + std::to_string(to) + " on " + describe(doc, index);
  // `from == to` is a no-op reorder that still reports ok, exactly as
  // moveLayer() does, and for the same reason: the gesture happened.
  if (from != to) doc.layers[index].ops.reorder(from, to);
  return succeed(label, index);
}

LayerOpResult setLayerOp(Document& doc, size_t index, size_t opIndex, Op op) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "edit op", &refusal)) return refusal;
  if (!notLocked(doc, index, "edit op", &refusal)) return refusal;
  if (!opInRange(doc, index, opIndex, "edit op", &refusal)) return refusal;
  const std::string label = "edit " + opDisplayName(op) + " on " + describe(doc, index);
  doc.layers[index].ops.setOp(opIndex, std::move(op));
  return succeed(label, index);
}

LayerOpResult setLayerOpEnabled(Document& doc, size_t index, size_t opIndex, bool enabled) {
  LayerOpResult refusal;
  if (!inRange(doc, index, "enable op", &refusal)) return refusal;
  if (!notLocked(doc, index, "enable op", &refusal)) return refusal;
  if (!opInRange(doc, index, opIndex, "enable op", &refusal)) return refusal;
  const std::string label = std::string(enabled ? "enable " : "disable ") +
                            opDisplayName(doc.layers[index].ops.at(opIndex)) + " on " +
                            describe(doc, index);
  doc.layers[index].ops.setEnabled(opIndex, enabled);
  return succeed(label, index);
}

}  // namespace np
