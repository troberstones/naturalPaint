#include "core/LayerSetOps.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace np {
namespace {

std::string plural(size_t n, const char* singular, const char* pluralForm) {
  return std::to_string(n) + " " + (n == 1 ? singular : pluralForm);
}

std::string describeLayer(const Document& doc, size_t index) {
  const Layer& l = doc.layers[index];
  return "layer " + std::to_string(index) +
         (l.name.empty() ? std::string(" (unnamed)") : " ('" + l.name + "')");
}

LayerSetOpResult refuse(std::string error) {
  LayerSetOpResult out;
  out.ok = false;
  out.error = std::move(error);
  return out;
}

// --- Alignment geometry -----------------------------------------------------

enum class AlignAxis { X, Y };
enum class AlignEdge { Min, Center, Max };

// One thing an align moves: a link group, or a single unlinked layer.
// Section 4 of the header is why a group is one unit rather than N.
struct AlignUnit {
  std::vector<size_t> members;  // ascending
  LayerBounds bounds;
};

// The units of `effective`, in ascending order of their lowest member, with
// each link group folded into one entry.
std::vector<AlignUnit> unitsOf(const Document& doc, const LayerSelection& effective) {
  std::vector<AlignUnit> units;
  std::unordered_map<uint64_t, size_t> groupToUnit;
  for (const size_t index : effective.indices) {
    const uint64_t group = doc.layers[index].linkGroup;
    const bool linked = group != 0 && layerIsLinked(doc, index);
    if (linked) {
      const auto it = groupToUnit.find(group);
      if (it != groupToUnit.end()) {
        units[it->second].members.push_back(index);
        units[it->second].bounds =
            unionLayerBounds(units[it->second].bounds, layerContentBounds(doc.layers[index]));
        continue;
      }
      groupToUnit.emplace(group, units.size());
    }
    AlignUnit u;
    u.members.push_back(index);
    u.bounds = layerContentBounds(doc.layers[index]);
    units.push_back(std::move(u));
  }
  return units;
}

double unitCoordinate(const LayerBounds& b, AlignAxis axis, AlignEdge edge) {
  const double lo = axis == AlignAxis::X ? b.minX : b.minY;
  const double hi = axis == AlignAxis::X ? b.maxX : b.maxY;
  switch (edge) {
    case AlignEdge::Min:    return lo;
    case AlignEdge::Center: return (lo + hi) / 2.0;
    case AlignEdge::Max:    return hi;
  }
  return lo;
}

}  // namespace

// --- LayerSelection ---------------------------------------------------------

bool LayerSelection::contains(size_t index) const noexcept {
  return std::binary_search(indices.begin(), indices.end(), index);
}

LayerSelection makeLayerSelection(std::vector<size_t> indices) {
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  LayerSelection out;
  out.indices = std::move(indices);
  return out;
}

LayerSelection singleLayerSelection(size_t index) { return makeLayerSelection({index}); }

// --- Links ------------------------------------------------------------------

std::vector<size_t> linkedLayers(const Document& doc, size_t index) {
  if (index >= doc.layers.size()) return {};
  const uint64_t group = doc.layers[index].linkGroup;
  if (group == 0) return {index};
  std::vector<size_t> members;
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (doc.layers[i].linkGroup == group) members.push_back(i);
  }
  // "A group with fewer than two live members is not a link" -- the header's
  // section 4, implemented here and nowhere else, which is what keeps a
  // survivor's leftover number from ever reaching a caller as a link.
  if (members.size() < 2) return {index};
  return members;
}

bool layerIsLinked(const Document& doc, size_t index) {
  return linkedLayers(doc, index).size() > 1;
}

uint64_t nextLinkGroupId(const Document& doc) {
  uint64_t highest = 0;
  for (const Layer& l : doc.layers) highest = std::max(highest, l.linkGroup);
  return highest + 1;
}

LayerSelection expandSelectionByLinks(const Document& doc, const LayerSelection& sel) {
  std::vector<size_t> all;
  for (const size_t index : sel.indices) {
    if (index >= doc.layers.size()) {
      all.push_back(index);  // preserved so validation can refuse it by name
      continue;
    }
    for (const size_t member : linkedLayers(doc, index)) all.push_back(member);
  }
  return makeLayerSelection(std::move(all));
}

// --- The command table ------------------------------------------------------

const std::vector<LayerSetCommand>& allLayerSetCommands() {
  static const std::vector<LayerSetCommand> kAll = {
      LayerSetCommand::DeleteLayers,          LayerSetCommand::DuplicateLayers,
      LayerSetCommand::MoveLayersUp,          LayerSetCommand::MoveLayersDown,
      LayerSetCommand::GroupLayers,           LayerSetCommand::UngroupLayers,
      LayerSetCommand::ShowLayers,            LayerSetCommand::HideLayers,
      LayerSetCommand::LockLayers,            LayerSetCommand::UnlockLayers,
      LayerSetCommand::ClipLayers,            LayerSetCommand::UnclipLayers,
      LayerSetCommand::LinkLayers,            LayerSetCommand::UnlinkLayers,
      LayerSetCommand::LabelNone,             LayerSetCommand::LabelRed,
      LayerSetCommand::LabelOrange,           LayerSetCommand::LabelYellow,
      LayerSetCommand::LabelGreen,            LayerSetCommand::LabelBlue,
      LayerSetCommand::LabelViolet,           LayerSetCommand::LabelGrey,
      LayerSetCommand::AlignSelectionLeft,    LayerSetCommand::AlignSelectionCenterX,
      LayerSetCommand::AlignSelectionRight,   LayerSetCommand::AlignSelectionTop,
      LayerSetCommand::AlignSelectionCenterY, LayerSetCommand::AlignSelectionBottom,
      LayerSetCommand::AlignCanvasLeft,       LayerSetCommand::AlignCanvasCenterX,
      LayerSetCommand::AlignCanvasRight,      LayerSetCommand::AlignCanvasTop,
      LayerSetCommand::AlignCanvasCenterY,    LayerSetCommand::AlignCanvasBottom,
      LayerSetCommand::DistributeHorizontally, LayerSetCommand::DistributeVertically,
  };
  return kAll;
}

const char* layerSetCommandLabel(LayerSetCommand command) noexcept {
  switch (command) {
    case LayerSetCommand::DeleteLayers:           return "Delete Layers";
    case LayerSetCommand::DuplicateLayers:        return "Duplicate Layers";
    case LayerSetCommand::MoveLayersUp:           return "Move Layers Up";
    case LayerSetCommand::MoveLayersDown:         return "Move Layers Down";
    case LayerSetCommand::GroupLayers:            return "Group Layers";
    case LayerSetCommand::UngroupLayers:          return "Ungroup Layers";
    case LayerSetCommand::ShowLayers:             return "Show Layers";
    case LayerSetCommand::HideLayers:             return "Hide Layers";
    case LayerSetCommand::LockLayers:             return "Lock Layers";
    case LayerSetCommand::UnlockLayers:           return "Unlock Layers";
    case LayerSetCommand::ClipLayers:             return "Clip Layers to Layer Below";
    case LayerSetCommand::UnclipLayers:           return "Unclip Layers";
    case LayerSetCommand::LinkLayers:             return "Link Layers";
    case LayerSetCommand::UnlinkLayers:           return "Unlink Layers";
    case LayerSetCommand::LabelNone:              return "Colour Label: None";
    case LayerSetCommand::LabelRed:               return "Colour Label: Red";
    case LayerSetCommand::LabelOrange:            return "Colour Label: Orange";
    case LayerSetCommand::LabelYellow:            return "Colour Label: Yellow";
    case LayerSetCommand::LabelGreen:             return "Colour Label: Green";
    case LayerSetCommand::LabelBlue:              return "Colour Label: Blue";
    case LayerSetCommand::LabelViolet:            return "Colour Label: Violet";
    case LayerSetCommand::LabelGrey:              return "Colour Label: Grey";
    case LayerSetCommand::AlignSelectionLeft:     return "Align Left Edges";
    case LayerSetCommand::AlignSelectionCenterX:  return "Align Horizontal Centres";
    case LayerSetCommand::AlignSelectionRight:    return "Align Right Edges";
    case LayerSetCommand::AlignSelectionTop:      return "Align Top Edges";
    case LayerSetCommand::AlignSelectionCenterY:  return "Align Vertical Centres";
    case LayerSetCommand::AlignSelectionBottom:   return "Align Bottom Edges";
    case LayerSetCommand::AlignCanvasLeft:        return "Align Left to Canvas";
    case LayerSetCommand::AlignCanvasCenterX:     return "Align Horizontal Centre to Canvas";
    case LayerSetCommand::AlignCanvasRight:       return "Align Right to Canvas";
    case LayerSetCommand::AlignCanvasTop:         return "Align Top to Canvas";
    case LayerSetCommand::AlignCanvasCenterY:     return "Align Vertical Centre to Canvas";
    case LayerSetCommand::AlignCanvasBottom:      return "Align Bottom to Canvas";
    case LayerSetCommand::DistributeHorizontally: return "Distribute Horizontally";
    case LayerSetCommand::DistributeVertically:   return "Distribute Vertically";
  }
  return "?";
}

namespace {

// The label a `Label*` command writes, or nullptr when the command is not one.
const char* labelForCommand(LayerSetCommand command) noexcept {
  switch (command) {
    case LayerSetCommand::LabelNone:   return kNoLayerColorLabel;
    case LayerSetCommand::LabelRed:    return "red";
    case LayerSetCommand::LabelOrange: return "orange";
    case LayerSetCommand::LabelYellow: return "yellow";
    case LayerSetCommand::LabelGreen:  return "green";
    case LayerSetCommand::LabelBlue:   return "blue";
    case LayerSetCommand::LabelViolet: return "violet";
    case LayerSetCommand::LabelGrey:   return "grey";
    default:                           return nullptr;
  }
}

// --- Grouping (section 5) ---------------------------------------------------

// The contiguous run of layers directly below `groupIndex` whose `parent`
// names that group -- section 5's invariant, read back rather than assumed.
// A downward scan, so a same-tag layer that is NOT contiguous with the group
// (a state this file's own operations never produce, but a hand-built
// `Document` might) is simply not included, which is the same "absent means
// neutral" answer core/Mask.hpp gives a missing tile: it does not crash and
// it does not guess.
//
// Returns `[groupIndex + 1, groupIndex]` (an empty, well-formed range with
// `first > second`) for a group with no members -- callers check `first <=
// second` before iterating rather than special-casing size 0.
std::pair<size_t, size_t> groupMemberSpan(const Document& doc, size_t groupIndex) {
  const std::string& tag = doc.layers[groupIndex].groupTag;
  size_t first = groupIndex;
  while (first > 0 && doc.layers[first - 1].parent == tag) --first;
  if (first == groupIndex) return {groupIndex + 1, groupIndex};  // no members
  return {first, groupIndex - 1};
}

// One contiguous block of layers, moved as a unit by `GroupLayers`. An
// ordinary selected layer is a span of one; a selected Group layer is itself
// plus its own (already contiguous, however deeply nested) member run --
// section 5's "nesting falls out of moving spans".
struct LayerSpan {
  size_t first = 0, last = 0;  // inclusive, in the ORIGINAL document
  // Index, within the extracted block, of the element that was actually
  // selected by the user -- the one whose `parent` gets rewritten to the new
  // group's tag. For an ordinary layer this is the span's only element; for a
  // Group it is the LAST element (the group's own entry sits at the top of
  // its own contiguous run, by this file's own placement rule).
  size_t selectedOffset = 0;
};

// (axis, edge, to-canvas) for the twelve align commands; `false` when the
// command is not an align.
bool alignSpecFor(LayerSetCommand command, AlignAxis* axis, AlignEdge* edge, bool* toCanvas) {
  switch (command) {
    case LayerSetCommand::AlignSelectionLeft:
      *axis = AlignAxis::X; *edge = AlignEdge::Min;    *toCanvas = false; return true;
    case LayerSetCommand::AlignSelectionCenterX:
      *axis = AlignAxis::X; *edge = AlignEdge::Center; *toCanvas = false; return true;
    case LayerSetCommand::AlignSelectionRight:
      *axis = AlignAxis::X; *edge = AlignEdge::Max;    *toCanvas = false; return true;
    case LayerSetCommand::AlignSelectionTop:
      *axis = AlignAxis::Y; *edge = AlignEdge::Min;    *toCanvas = false; return true;
    case LayerSetCommand::AlignSelectionCenterY:
      *axis = AlignAxis::Y; *edge = AlignEdge::Center; *toCanvas = false; return true;
    case LayerSetCommand::AlignSelectionBottom:
      *axis = AlignAxis::Y; *edge = AlignEdge::Max;    *toCanvas = false; return true;
    case LayerSetCommand::AlignCanvasLeft:
      *axis = AlignAxis::X; *edge = AlignEdge::Min;    *toCanvas = true;  return true;
    case LayerSetCommand::AlignCanvasCenterX:
      *axis = AlignAxis::X; *edge = AlignEdge::Center; *toCanvas = true;  return true;
    case LayerSetCommand::AlignCanvasRight:
      *axis = AlignAxis::X; *edge = AlignEdge::Max;    *toCanvas = true;  return true;
    case LayerSetCommand::AlignCanvasTop:
      *axis = AlignAxis::Y; *edge = AlignEdge::Min;    *toCanvas = true;  return true;
    case LayerSetCommand::AlignCanvasCenterY:
      *axis = AlignAxis::Y; *edge = AlignEdge::Center; *toCanvas = true;  return true;
    case LayerSetCommand::AlignCanvasBottom:
      *axis = AlignAxis::Y; *edge = AlignEdge::Max;    *toCanvas = true;  return true;
    default:
      return false;
  }
}

bool isDistribute(LayerSetCommand c) noexcept {
  return c == LayerSetCommand::DistributeHorizontally ||
         c == LayerSetCommand::DistributeVertically;
}

bool isGeometry(LayerSetCommand c) noexcept {
  AlignAxis a = AlignAxis::X;
  AlignEdge e = AlignEdge::Min;
  bool canvas = false;
  return alignSpecFor(c, &a, &e, &canvas) || isDistribute(c);
}

// Turns a per-unit integer delta into translate calls over the unit's members.
// Every geometric command funnels through here, so "a link group moves as one"
// has exactly one implementation.
bool translateUnits(Document& scratch, const std::vector<AlignUnit>& units,
                    const std::vector<std::pair<int32_t, int32_t>>& deltas,
                    std::string* errorOut) {
  for (size_t u = 0; u < units.size(); ++u) {
    for (const size_t member : units[u].members) {
      const LayerOpResult r = translateLayer(scratch, member, deltas[u].first, deltas[u].second);
      if (!r.ok) {
        *errorOut = r.error;
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool layerSetCommandAvailable(const Document& doc, LayerSetCommand command,
                              const LayerSelection& sel) {
  const size_t count = doc.layers.size();
  if (sel.empty()) return false;
  for (const size_t index : sel.indices) {
    if (index >= count) return false;
  }
  switch (command) {
    case LayerSetCommand::MoveLayersUp:
      return sel.indices.back() + 1 < count;
    case LayerSetCommand::MoveLayersDown:
      return sel.indices.front() > 0;
    // Ungroup is meaningless on anything but a Group -- the same "this
    // gesture makes no sense on this row at all" test the header applies to
    // Move Up on the top layer. Whether it can actually SUCCEED (a locked
    // layer in the way, a clip that would land at index 0) is a refusal,
    // decided in `applyLayerSetOp()`, not here.
    case LayerSetCommand::UngroupLayers:
      for (const size_t index : sel.indices) {
        if (doc.layers[index].kind != LayerKind::Group) return false;
      }
      return true;
    // The bottom layer has nothing below it to be clipped by -- the same
    // property-of-the-row rule `app::layerCommandAvailable()` applies to the
    // single-selection Clip. Unclipping is never unavailable.
    case LayerSetCommand::ClipLayers:
      return sel.indices.front() > 0;
    // A link needs two layers by definition. Unlink is offered on any
    // selection, because a selection that contains no link is a no-op the user
    // asked for, not an error (`setLayerVisible()`'s rule).
    case LayerSetCommand::LinkLayers:
      return sel.size() >= 2;
    case LayerSetCommand::AlignSelectionLeft:
    case LayerSetCommand::AlignSelectionCenterX:
    case LayerSetCommand::AlignSelectionRight:
    case LayerSetCommand::AlignSelectionTop:
    case LayerSetCommand::AlignSelectionCenterY:
    case LayerSetCommand::AlignSelectionBottom:
      return sel.size() >= 2;
    case LayerSetCommand::AlignCanvasLeft:
    case LayerSetCommand::AlignCanvasCenterX:
    case LayerSetCommand::AlignCanvasRight:
    case LayerSetCommand::AlignCanvasTop:
    case LayerSetCommand::AlignCanvasCenterY:
    case LayerSetCommand::AlignCanvasBottom:
      return !documentCanvasBounds(doc).empty;
    // Three is the smallest set with something in the middle to space out.
    case LayerSetCommand::DistributeHorizontally:
    case LayerSetCommand::DistributeVertically:
      return sel.size() >= 3;
    default:
      return true;
  }
}

LayerSetOpResult applyLayerSetOp(Document& doc, LayerSetCommand command,
                                 const LayerSelection& sel) {
  if (sel.empty()) {
    return refuse(std::string(layerSetCommandLabel(command)) +
                  " refused: no layers are selected. Nothing was changed.");
  }
  for (const size_t index : sel.indices) {
    if (index >= doc.layers.size()) {
      return refuse(std::string(layerSetCommandLabel(command)) + " refused: index " +
                    std::to_string(index) + " is out of range; this document has " +
                    std::to_string(doc.layers.size()) + " layer(s). Nothing was changed.");
    }
  }

  // The atomic trial -- header section 3. Every mutation below runs on this
  // copy, and the copy replaces the document only if all of them succeed.
  // Cheap because `TileStoreOf`'s slots are shared (Phase 5 step 6).
  Document scratch = doc;
  LayerSetOpResult out;
  const size_t n = sel.size();

  const std::vector<size_t> ascending = sel.indices;
  std::vector<size_t> descending = sel.indices;
  std::reverse(descending.begin(), descending.end());

  auto runOver = [&](const std::vector<size_t>& order, auto&& op) -> bool {
    for (const size_t index : order) {
      const LayerOpResult r = op(index);
      if (!r.ok) {
        out.error = r.error;
        return false;
      }
    }
    return true;
  };

  if (const char* label = labelForCommand(command); label != nullptr) {
    if (!runOver(ascending,
                 [&](size_t i) { return setLayerColorLabel(scratch, i, std::string(label)); }))
      return refuse(std::move(out.error));
    out.editLabel = (label[0] == '\0' ? "clear the colour label on "
                                      : std::string("label ") + label + " on ") +
                    plural(n, "layer", "layers");
    out.selection = sel;
  } else if (isGeometry(command)) {
    // Geometry, and only geometry, follows links -- header section 4.
    const LayerSelection effective = expandSelectionByLinks(scratch, sel);
    const std::vector<AlignUnit> units = unitsOf(scratch, effective);
    for (const AlignUnit& u : units) {
      if (u.bounds.empty) {
        return refuse(std::string(layerSetCommandLabel(command)) + " refused: " +
                      describeLayer(scratch, u.members.front()) +
                      " has no content -- every texel is empty -- so it has no edges to "
                      "align. Nothing was changed.");
      }
    }

    AlignAxis axis = AlignAxis::X;
    AlignEdge edge = AlignEdge::Min;
    bool toCanvas = false;
    std::vector<std::pair<int32_t, int32_t>> deltas(units.size(), {0, 0});
    double worstRounding = 0.0;

    if (alignSpecFor(command, &axis, &edge, &toCanvas)) {
      LayerBounds reference;
      if (toCanvas) {
        reference = documentCanvasBounds(scratch);
        if (reference.empty) {
          return refuse(std::string(layerSetCommandLabel(command)) +
                        " refused: this document's canvas is " +
                        std::to_string(scratch.width) + " x " + std::to_string(scratch.height) +
                        ", which has no edges to align to. Nothing was changed.");
        }
      } else {
        if (units.size() < 2) {
          return refuse(std::string(layerSetCommandLabel(command)) + " refused: the " +
                        plural(n, "selected layer", "selected layers") +
                        " form a single unit (a link group moves as one), so there is "
                        "nothing to align them to. Align to the canvas instead, or unlink "
                        "them. Nothing was changed.");
        }
        for (const AlignUnit& u : units) reference = unionLayerBounds(reference, u.bounds);
      }
      const double target = unitCoordinate(reference, axis, edge);
      for (size_t u = 0; u < units.size(); ++u) {
        const double delta = target - unitCoordinate(units[u].bounds, axis, edge);
        const double rounded = std::round(delta);
        worstRounding = std::max(worstRounding, std::abs(delta - rounded));
        const int32_t step = static_cast<int32_t>(rounded);
        deltas[u] = axis == AlignAxis::X ? std::pair<int32_t, int32_t>{step, 0}
                                         : std::pair<int32_t, int32_t>{0, step};
      }
    } else {
      // Distribute: the two extreme units stay put and the ones between them
      // get equally spaced **centres**. Centres rather than gaps because a gap
      // distribution is undefined the moment two units overlap, which is the
      // normal state of a layer stack.
      axis = command == LayerSetCommand::DistributeHorizontally ? AlignAxis::X : AlignAxis::Y;
      if (units.size() < 3) {
        return refuse(std::string(layerSetCommandLabel(command)) + " refused: " +
                      plural(units.size(), "unit", "units") +
                      " to distribute (a link group counts as one); at least 3 are needed "
                      "for there to be anything between the ends. Nothing was changed.");
      }
      std::vector<size_t> order(units.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return unitCoordinate(units[a].bounds, axis, AlignEdge::Center) <
               unitCoordinate(units[b].bounds, axis, AlignEdge::Center);
      });
      const double first = unitCoordinate(units[order.front()].bounds, axis, AlignEdge::Center);
      const double last = unitCoordinate(units[order.back()].bounds, axis, AlignEdge::Center);
      const double spacing = (last - first) / static_cast<double>(order.size() - 1);
      for (size_t k = 0; k < order.size(); ++k) {
        const size_t u = order[k];
        const double want = first + spacing * static_cast<double>(k);
        const double delta = want - unitCoordinate(units[u].bounds, axis, AlignEdge::Center);
        const double rounded = std::round(delta);
        worstRounding = std::max(worstRounding, std::abs(delta - rounded));
        const int32_t step = static_cast<int32_t>(rounded);
        deltas[u] = axis == AlignAxis::X ? std::pair<int32_t, int32_t>{step, 0}
                                         : std::pair<int32_t, int32_t>{0, step};
      }
    }

    std::string error;
    if (!translateUnits(scratch, units, deltas, &error)) return refuse(std::move(error));
    if (worstRounding > 0.0) {
      // core/LayerGeometry translates by whole pixels only, so a centre that
      // lands on a half pixel is rounded rather than resampled. Reported rather
      // than swallowed: the alternative is a user measuring a one-pixel
      // discrepancy and finding no record that anything rounded.
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "%s: rounded to whole pixels; the largest adjustment was %.1f px. A "
                    "sub-pixel move would resample, and this build has no transform.",
                    layerSetCommandLabel(command), worstRounding);
      out.warnings.emplace_back(buf);
    }
    out.editLabel = std::string(layerSetCommandLabel(command)) + " (" +
                    plural(units.size(), "unit", "units") + ")";
    out.selection = sel;
  } else {
    switch (command) {
      case LayerSetCommand::DeleteLayers: {
        if (!runOver(descending, [&](size_t i) { return removeLayer(scratch, i); }))
          return refuse(std::move(out.error));
        out.editLabel = "delete " + plural(n, "layer", "layers");
        // The row that takes the place of the lowest layer removed, clamped to
        // what is left -- `app::applyLayerCommand()`'s single-selection rule,
        // which lands the selection where the user's eye already is.
        const size_t left = scratch.layers.size();
        out.selection = left == 0 ? LayerSelection{}
                                  : singleLayerSelection(std::min(
                                        ascending.front() > 0 ? ascending.front() - 1 : 0,
                                        left - 1));
        break;
      }
      case LayerSetCommand::DuplicateLayers: {
        if (!runOver(descending, [&](size_t i) { return duplicateLayer(scratch, i); }))
          return refuse(std::move(out.error));
        out.editLabel = "duplicate " + plural(n, "layer", "layers");
        // The copies, which is where the single-selection Duplicate leaves the
        // selection too. A copy of source `s` lands at `s + 1 + (how many
        // selected layers sit below s)`, because each of those inserted one row
        // under it.
        std::vector<size_t> copies;
        for (size_t k = 0; k < ascending.size(); ++k) copies.push_back(ascending[k] + 1 + k);
        out.selection = makeLayerSelection(std::move(copies));
        break;
      }
      case LayerSetCommand::MoveLayersUp: {
        // Refused here rather than by `moveLayer()`, in this list's own
        // vocabulary: handing the top layer's index + 1 to core/LayerOps gets a
        // true sentence about an index the user never named ("index 5 is out of
        // range"), which is the exact trade `app::applyLayerCommand()` already
        // refuses to make for the single-selection Move Down.
        if (ascending.back() + 1 >= scratch.layers.size()) {
          return refuse("move layers up refused: " +
                        describeLayer(scratch, scratch.layers.size() - 1) +
                        " is already at the top of the stack. Nothing was changed.");
        }
        if (!runOver(descending, [&](size_t i) { return moveLayer(scratch, i, i + 1); }))
          return refuse(std::move(out.error));
        out.editLabel = "move " + plural(n, "layer", "layers") + " up";
        std::vector<size_t> moved;
        for (const size_t i : ascending) moved.push_back(i + 1);
        out.selection = makeLayerSelection(std::move(moved));
        break;
      }
      case LayerSetCommand::MoveLayersDown: {
        if (ascending.front() == 0) {
          return refuse("move layers down refused: " + describeLayer(scratch, 0) +
                        " is already at the bottom of the stack. Nothing was changed.");
        }
        if (!runOver(ascending, [&](size_t i) { return moveLayer(scratch, i, i - 1); }))
          return refuse(std::move(out.error));
        out.editLabel = "move " + plural(n, "layer", "layers") + " down";
        std::vector<size_t> moved;
        for (const size_t i : ascending) moved.push_back(i - 1);
        out.selection = makeLayerSelection(std::move(moved));
        break;
      }
      case LayerSetCommand::GroupLayers: {
        // Section 5's two pre-checks: a locked member freezes its place in
        // the stack, and grouping an already-grouped layer is refused rather
        // than silently re-parented.
        for (const size_t index : ascending) {
          if (scratch.layers[index].locked) {
            return refuse("group layers refused: " + describeLayer(scratch, index) +
                          " is locked, and grouping moves a layer's place in the stack "
                          "exactly as a reorder does. Unlock it first. Nothing was changed.");
          }
          if (!scratch.layers[index].parent.empty()) {
            return refuse("group layers refused: " + describeLayer(scratch, index) +
                          " is already inside a group. Ungroup it first, or select its "
                          "containing group instead of its members directly. Nothing was "
                          "changed.");
          }
        }

        // One span per selected index -- an ordinary layer is a span of
        // itself, a Group is itself plus its own contiguous member run.
        // `sp.last == index` always (section 5), so spans share `ascending`'s
        // strictly increasing order and, being non-overlapping, their
        // `.first` values are strictly increasing too.
        std::vector<LayerSpan> spans;
        spans.reserve(n);
        for (const size_t index : ascending) {
          LayerSpan sp;
          sp.last = index;
          if (scratch.layers[index].kind == LayerKind::Group) {
            const auto [memberFirst, memberLast] = groupMemberSpan(scratch, index);
            sp.first = (memberFirst <= memberLast) ? memberFirst : index;
          } else {
            sp.first = index;
          }
          sp.selectedOffset = sp.last - sp.first;
          spans.push_back(sp);
        }
        for (size_t k = 1; k < spans.size(); ++k) {
          if (spans[k].first <= spans[k - 1].last) {
            return refuse("group layers refused: the selection overlaps itself around " +
                          describeLayer(scratch, spans[k].last) +
                          " -- selecting both a group and one of its own members is not "
                          "a set this command can move as one block. Nothing was changed.");
          }
        }

        // The group's identity is assigned now, before anything moves --
        // `makeGroupLayer()` only consumes `doc.nextGroupId`, which is
        // position-independent.
        Layer group = makeGroupLayer(scratch, defaultNewGroupName(scratch));
        const std::string groupName = group.name;

        // Extract every span, DESCENDING by `.first`, so an as-yet-unextracted
        // span's indices stay valid -- section 3's own rule for Delete,
        // applied to ranges instead of single indices.
        std::vector<std::vector<Layer>> blocks(spans.size());
        for (size_t k = spans.size(); k-- > 0;) {
          std::vector<Layer> block;
          block.reserve(spans[k].last - spans[k].first + 1);
          for (size_t idx = spans[k].first; idx <= spans[k].last; ++idx)
            block.push_back(std::move(scratch.layers[idx]));
          scratch.layers.erase(scratch.layers.begin() + static_cast<ptrdiff_t>(spans[k].first),
                               scratch.layers.begin() + static_cast<ptrdiff_t>(spans[k].last) + 1);
          blocks[k] = std::move(block);
        }

        // Reassemble in ORIGINAL ascending order -- k = 0, 1, 2, ... -- which
        // is the one line a reversal bug would change to `k = spans.size();
        // k-- > 0` and silently swap the members' relative order. Marks each
        // span's originally-selected element (`selectedOffset` within its own
        // block) as it goes, so the group tag lands on exactly the layer the
        // user selected -- the group's own entry for a re-nested span, the
        // sole element for an ordinary one.
        std::vector<Layer> members;
        std::vector<size_t> relabel;
        for (size_t k = 0; k < blocks.size(); ++k) {
          relabel.push_back(members.size() + spans[k].selectedOffset);
          for (Layer& l : blocks[k]) members.push_back(std::move(l));
        }
        for (const size_t offset : relabel) members[offset].parent = group.groupTag;

        // The topmost span's own `.first`, adjusted for every block already
        // removed below it -- `blocks.back()` is the topmost span's own
        // extracted block, so everything before it in `members` is exactly
        // "how many layers sat below the topmost span's start and are now
        // gone".
        const size_t insertPos = spans.back().first - (members.size() - blocks.back().size());

        scratch.layers.insert(scratch.layers.begin() + static_cast<ptrdiff_t>(insertPos),
                              std::make_move_iterator(members.begin()),
                              std::make_move_iterator(members.end()));
        const size_t groupIndex = insertPos + members.size();
        scratch.layers.insert(scratch.layers.begin() + static_cast<ptrdiff_t>(groupIndex),
                              std::move(group));

        // The one post-condition `moveLayer()`/`setLayerClipped()` cannot
        // check for us, because neither is what moved these layers --
        // core/Layer.hpp: a clipped layer has nothing below index 0 to be
        // clipped by.
        if (!scratch.layers.empty() && scratch.layers[0].clipped) {
          return refuse(
              "group layers refused: this would move a clipped layer to the bottom of the "
              "stack, where core::setLayerClipped() and moveLayer() both refuse to put one "
              "directly -- there is nothing below index 0 for it to be clipped by. Nothing "
              "was changed.");
        }

        out.editLabel = "group " + plural(n, "layer", "layers") + " into \"" + groupName + "\"";
        out.selection = singleLayerSelection(groupIndex);
        break;
      }
      case LayerSetCommand::UngroupLayers: {
        for (const size_t index : ascending) {
          if (scratch.layers[index].kind != LayerKind::Group) {
            return refuse("ungroup layers refused: " + describeLayer(scratch, index) +
                          " is not a group. Nothing was changed.");
          }
          if (scratch.layers[index].locked) {
            return refuse("ungroup layers refused: " + describeLayer(scratch, index) +
                          " is locked, and ungrouping removes it exactly as deleting it "
                          "would. Unlock it first. Nothing was changed.");
          }
        }

        // Descending, Delete's own idiom: removing one group's single entry
        // (net -1 length) must not invalidate a not-yet-processed group's
        // index, and every not-yet-processed group in `descending` sits
        // BELOW every already-processed one, so it never does.
        struct UngroupedSpan { size_t memberFirst = 0, count = 0; };
        std::vector<UngroupedSpan> recorded;
        recorded.reserve(n);
        for (const size_t groupIndex : descending) {
          const auto [memberFirst, memberLast] = groupMemberSpan(scratch, groupIndex);
          const std::string outerParent = scratch.layers[groupIndex].parent;
          std::vector<Layer> members;
          if (memberFirst <= memberLast) {
            members.reserve(memberLast - memberFirst + 1);
            for (size_t idx = memberFirst; idx <= memberLast; ++idx) {
              Layer m = std::move(scratch.layers[idx]);
              // Nesting-aware promotion: a member of the ungrouped group
              // becomes a member of ITS OWN parent (possibly none), never a
              // top-level layer regardless of nesting depth -- section 5.
              m.parent = outerParent;
              members.push_back(std::move(m));
            }
          }
          recorded.push_back({memberFirst, members.size()});
          // Extract the whole [memberFirst, groupIndex] span (members plus
          // the group's own marker entry) and reinsert just the members, in
          // the SAME relative order, at `memberFirst` -- the symmetric
          // extract-then-splice `GroupLayers` above uses. The one line
          // (`members.begin(), members.end()` vs `members.rbegin(),
          // members.rend()`) a reversal bug would change.
          scratch.layers.erase(
              scratch.layers.begin() + static_cast<ptrdiff_t>(memberFirst),
              scratch.layers.begin() + static_cast<ptrdiff_t>(groupIndex) + 1);
          scratch.layers.insert(scratch.layers.begin() + static_cast<ptrdiff_t>(memberFirst),
                                std::make_move_iterator(members.begin()),
                                std::make_move_iterator(members.end()));
        }

        // Every promoted member's FINAL index, adjusting each recorded
        // position for the net -1 shift every later (lower) group's own
        // splice contributes -- `recorded[k]` was measured before the
        // `(recorded.size() - 1 - k)` splices that came after it in this
        // descending walk, each of which sits strictly below it and shifts
        // it down by exactly one.
        std::vector<size_t> selIdx;
        for (size_t k = 0; k < recorded.size(); ++k) {
          const size_t shift = recorded.size() - 1 - k;
          const size_t start = recorded[k].memberFirst - shift;
          for (size_t j = 0; j < recorded[k].count; ++j) selIdx.push_back(start + j);
        }
        out.editLabel = "ungroup " + plural(n, "group", "groups");
        out.selection = makeLayerSelection(std::move(selIdx));
        break;
      }
      case LayerSetCommand::ShowLayers:
      case LayerSetCommand::HideLayers: {
        const bool visible = command == LayerSetCommand::ShowLayers;
        if (!runOver(ascending, [&](size_t i) { return setLayerVisible(scratch, i, visible); }))
          return refuse(std::move(out.error));
        out.editLabel = (visible ? "show " : "hide ") + plural(n, "layer", "layers");
        out.selection = sel;
        break;
      }
      case LayerSetCommand::LockLayers:
      case LayerSetCommand::UnlockLayers: {
        const bool locked = command == LayerSetCommand::LockLayers;
        if (!runOver(ascending, [&](size_t i) { return setLayerLocked(scratch, i, locked); }))
          return refuse(std::move(out.error));
        out.editLabel = (locked ? "lock " : "unlock ") + plural(n, "layer", "layers");
        out.selection = sel;
        break;
      }
      case LayerSetCommand::ClipLayers:
      case LayerSetCommand::UnclipLayers: {
        const bool clipped = command == LayerSetCommand::ClipLayers;
        if (!runOver(ascending, [&](size_t i) { return setLayerClipped(scratch, i, clipped); }))
          return refuse(std::move(out.error));
        out.editLabel = (clipped ? "clip " : "unclip ") + plural(n, "layer", "layers");
        out.selection = sel;
        break;
      }
      case LayerSetCommand::LinkLayers: {
        if (n < 2) {
          return refuse("link layers refused: a link needs at least two layers and " +
                        plural(n, "layer is", "layers are") +
                        " selected. Nothing was changed.");
        }
        const uint64_t group = nextLinkGroupId(scratch);
        if (!runOver(ascending, [&](size_t i) { return setLayerLinkGroup(scratch, i, group); }))
          return refuse(std::move(out.error));
        out.editLabel = "link " + plural(n, "layer", "layers");
        out.selection = sel;
        break;
      }
      case LayerSetCommand::UnlinkLayers: {
        if (!runOver(ascending, [&](size_t i) { return setLayerLinkGroup(scratch, i, 0); }))
          return refuse(std::move(out.error));
        out.editLabel = "unlink " + plural(n, "layer", "layers");
        out.selection = sel;
        break;
      }
      default:
        return refuse("unknown layer set command.");
    }
  }

  doc = std::move(scratch);
  out.ok = true;
  out.error.clear();
  return out;
}

LayerSetOpResult setLayerSetOpacity(Document& doc, const LayerSelection& sel, float opacity) {
  if (sel.empty()) return refuse("set opacity refused: no layers are selected.");
  Document scratch = doc;
  for (const size_t index : sel.indices) {
    const LayerOpResult r = setLayerOpacity(scratch, index, opacity);
    if (!r.ok) return refuse(r.error);
  }
  LayerSetOpResult out;
  out.ok = true;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(opacity) * 100.0);
  out.editLabel = "set " + plural(sel.size(), "layer", "layers") + " to " + buf + " opacity";
  out.selection = sel;
  doc = std::move(scratch);
  return out;
}

LayerSetOpResult setLayerSetBlend(Document& doc, const LayerSelection& sel, BlendMode mode) {
  if (sel.empty()) return refuse("set blend mode refused: no layers are selected.");
  Document scratch = doc;
  for (const size_t index : sel.indices) {
    const LayerOpResult r = setLayerBlend(scratch, index, mode);
    if (!r.ok) return refuse(r.error);
  }
  LayerSetOpResult out;
  out.ok = true;
  out.editLabel = "set " + plural(sel.size(), "layer", "layers") + " to " +
                  blendModeName(mode) + " blend";
  out.selection = sel;
  doc = std::move(scratch);
  return out;
}

}  // namespace np
