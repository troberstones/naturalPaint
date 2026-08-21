#include "core/LayerCompOps.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <utility>

#include "core/Blend.hpp"

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

// `comp 2 ("Cool variant")` -- core/LayerOps.cpp's `describe()` for the other
// list. The index is always present because comp names are not unique and may
// be empty.
std::string describeComp(const Document& doc, size_t index) {
  std::string s = "comp " + std::to_string(index);
  if (index < doc.comps.size() && !doc.comps[index].name.empty())
    s += " (\"" + doc.comps[index].name + "\")";
  return s;
}

// The one bounds check over `Document::comps`, so every operation refuses an
// out-of-range index with the same sentence.
bool compInRange(const Document& doc, size_t index, const char* what, LayerOpResult* out) {
  if (index < doc.comps.size()) return true;
  *out = fail(std::string(what) + " refused: there is no comp at index " +
              std::to_string(index) + " -- this document has " +
              std::to_string(doc.comps.size()) + " comp(s).");
  return false;
}

// How a layer is named in a report: its own name, or a positional stand-in so
// an entry can never read as blank. app/LayerPanel's `Layer N` rule, applied to
// a sentence rather than to a row.
std::string layerLabel(const std::string& name, size_t index) {
  if (!name.empty()) return "\"" + name + "\"";
  return "layer " + std::to_string(index);
}

// The same, for a layer that is no longer here: there is no index to fall back
// on, because the whole point is that it has no position any more.
std::string goneLayerLabel(const LayerCompEntry& entry) {
  if (!entry.nameAtCapture.empty()) return "\"" + entry.nameAtCapture + "\"";
  return "an unnamed layer (id " + std::to_string(entry.layerId) + ")";
}

std::string joinNames(const std::vector<std::string>& names, size_t limit = 4) {
  std::string s;
  for (size_t i = 0; i < names.size() && i < limit; ++i) {
    if (i != 0) s += ", ";
    s += names[i];
  }
  if (names.size() > limit) s += " and " + std::to_string(names.size() - limit) + " more";
  return s;
}

}  // namespace

void normalizeLayerIds(Document& doc) {
  // Raise the counter past anything already present first. Without this a
  // document whose `nextLayerId` was lost -- one loaded from a file another
  // tool stripped `np:comps` out of -- would re-issue an id a live layer still
  // holds, and the duplicate check below would then take that live layer's id
  // away from it.
  for (const Layer& layer : doc.layers)
    if (layer.id >= doc.nextLayerId) doc.nextLayerId = layer.id + 1;

  // Bottom to top, first occurrence keeps the id. `duplicateLayer()` inserts a
  // copy *above* its source, so where two layers share an id it is the source
  // -- the one every existing comp already refers to -- that keeps it.
  std::vector<uint64_t> seen;
  seen.reserve(doc.layers.size());
  for (Layer& layer : doc.layers) {
    const bool taken =
        layer.id != 0 && std::find(seen.begin(), seen.end(), layer.id) != seen.end();
    if (layer.id == 0 || taken) layer.id = doc.nextLayerId++;
    seen.push_back(layer.id);
  }
}

std::string defaultNewCompName(const Document& doc) {
  // `core::defaultNewLayerName()`'s scan, over the other list. One above the
  // highest existing "Comp N" rather than `comps.size() + 1`, which collides
  // the moment a comp is deleted from the middle.
  long highest = 0;
  for (const LayerComp& comp : doc.comps) {
    if (comp.name.rfind("Comp ", 0) != 0) continue;
    const std::string digits = comp.name.substr(5);
    if (digits.empty()) continue;
    bool allDigits = true;
    for (const char c : digits)
      if (c < '0' || c > '9') allDigits = false;
    if (!allDigits) continue;
    long value = 0;
    for (const char c : digits) {
      value = value * 10 + (c - '0');
      if (value > 1000000) break;  // saturate rather than overflow
    }
    if (value > highest) highest = value;
  }
  return "Comp " + std::to_string(highest + 1);
}

LayerOpResult captureLayerComp(Document& doc, std::string name) {
  if (doc.layers.empty()) {
    return fail("capture comp refused: this document has no layers, so there is no state to "
                "capture. A comp of nothing is a row that cannot change anything when it is "
                "clicked.");
  }
  // Ids first: an entry names a layer by id, so every layer must have one
  // before anything is read. This is the only place in the running application
  // that assigns them (core/Layer.hpp).
  normalizeLayerIds(doc);

  LayerComp comp;
  comp.name = std::move(name);
  comp.layers.reserve(doc.layers.size());
  for (const Layer& layer : doc.layers) {
    LayerCompEntry entry;
    entry.layerId = layer.id;
    entry.visible = layer.visible;
    entry.clipped = layer.clipped;
    entry.opacity = layer.opacity;
    entry.blend = layer.blend;
    entry.nameAtCapture = layer.name;
    comp.layers.push_back(std::move(entry));
  }

  const std::string label =
      "capture " + (comp.name.empty() ? std::string("comp") : "comp \"" + comp.name + "\"");
  doc.comps.push_back(std::move(comp));
  return succeed(label, doc.comps.size() - 1);
}

LayerOpResult restoreLayerComp(Document& doc, size_t compIndex, LayerCompRestoreReport* report) {
  LayerOpResult refusal;
  if (!compInRange(doc, compIndex, "restore comp", &refusal)) return refusal;

  const LayerComp& comp = doc.comps[compIndex];
  if (!comp.known) {
    return fail("restore comp refused: " + describeComp(doc, compIndex) + " is a " +
                std::to_string(comp.unrecognised.size()) +
                "-byte record written by a build whose comp format this one does not read. It "
                "is preserved verbatim and written back unchanged (PRD I10), which is what "
                "makes it safe to keep -- but its contents cannot be applied, and applying an "
                "empty comp in its place would be indistinguishable from a comp that does "
                "nothing. Nothing was changed.");
  }

  // Rule 2, checked before anything is written: an id two layers share has more
  // than one answer to "which layer is this entry for", and either answer is
  // the silent misapplication this whole design exists to prevent.
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (doc.layers[i].id == 0) continue;
    for (size_t j = i + 1; j < doc.layers.size(); ++j) {
      if (doc.layers[j].id != doc.layers[i].id) continue;
      return fail("restore comp refused: layers " + std::to_string(i) + " and " +
                  std::to_string(j) + " both carry layer id " +
                  std::to_string(doc.layers[i].id) +
                  ", so an entry naming that id has two possible destinations. A comp is "
                  "matched by id and never by position precisely so a restore cannot land on "
                  "the wrong layer, and picking one of the two would be that bug wearing a "
                  "coin toss. Nothing was changed. (`core::duplicateLayer()` gives a copy a "
                  "fresh id, so this state cannot be reached from the panel.)");
    }
  }

  // The id -> index map, built once. Ids are unique by the check above.
  std::unordered_map<uint64_t, size_t> byId;
  byId.reserve(doc.layers.size());
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (doc.layers[i].id != 0) byId.emplace(doc.layers[i].id, i);

  LayerCompRestoreReport local;
  size_t matched = 0;
  for (const LayerCompEntry& entry : comp.layers)
    if (byId.find(entry.layerId) != byId.end()) ++matched;

  // Rule 3. A comp none of whose layers survive is a comp for a different
  // document; reporting success for applying zero of it would be
  // indistinguishable from a comp that does nothing.
  if (matched == 0 && !comp.layers.empty()) {
    return fail("restore comp refused: not one of " + describeComp(doc, compIndex) + "'s " +
                std::to_string(comp.layers.size()) + " layer state(s) names a layer still in "
                "this document, which has " + std::to_string(doc.layers.size()) +
                " layer(s). Every layer the comp was captured over has been deleted (or the "
                "comp came from another document), so there is nothing it could restore. "
                "Nothing was changed.");
  }
  if (comp.layers.empty()) {
    return fail("restore comp refused: " + describeComp(doc, compIndex) +
                " holds no layer states at all. Nothing was changed.");
  }

  // --- Apply -------------------------------------------------------------
  //
  // Every property goes through its core/LayerOps setter, so the lock rule and
  // PRD L5's `Mix` restriction are enforced by the one implementation of each
  // rather than by a second copy here. The setters' results are inspected and
  // deliberately NOT recorded individually: a restore is one edit with one
  // label, and recording each property would put a dozen rows in the history
  // panel for one click.
  std::vector<bool> covered(doc.layers.size(), false);
  for (const LayerCompEntry& entry : comp.layers) {
    const auto it = byId.find(entry.layerId);
    if (it == byId.end()) {
      local.missingLayers.push_back(goneLayerLabel(entry));
      continue;
    }
    const size_t i = it->second;
    covered[i] = true;
    ++local.entriesApplied;

    const bool wasVisible = doc.layers[i].visible;
    const bool wasClipped = doc.layers[i].clipped;
    const float wasOpacity = doc.layers[i].opacity;
    const std::string wasBlend = doc.layers[i].blend;

    bool lockRefused = false;
    // Visibility first, and it is the one property a locked layer still
    // accepts -- core/LayerOps.hpp: "hiding a locked layer changes nothing
    // about the layer, and a lock that also freezes the eye icon is the one
    // behaviour every editor with a lock agrees is wrong". So its result is
    // not checked for a lock: there is no lock refusal for it to produce.
    if (doc.layers[i].visible != entry.visible) setLayerVisible(doc, i, entry.visible);

    if (doc.layers[i].opacity != entry.opacity)
      if (!setLayerOpacity(doc, i, entry.opacity).ok) lockRefused = true;

    if (doc.layers[i].blend != entry.blend) {
      // The captured value is a *string* (core/Layer.hpp keeps the member one
      // so a name from a newer build survives verbatim), and the setter takes a
      // `BlendMode`. A name this build has no mode for is named rather than
      // substituted: writing Normal instead would be a comp silently changing
      // what a layer does.
      const std::optional<BlendMode> mode = blendModeFromName(entry.blend);
      if (!mode.has_value()) {
        local.unsettableBlends.push_back(layerLabel(doc.layers[i].name, i) + " -> \"" +
                                         entry.blend + "\"");
      } else if (!setLayerBlend(doc, i, *mode).ok) {
        // Either the lock, or PRD L5 refusing `Mix` on a pair it no longer
        // applies to after a reorder. Both are real refusals with their own
        // sentences; the report names the layer and the value it could not take.
        if (doc.layers[i].locked) {
          lockRefused = true;
        } else {
          local.unsettableBlends.push_back(layerLabel(doc.layers[i].name, i) + " -> \"" +
                                           entry.blend + "\"");
        }
      }
    }

    if (doc.layers[i].clipped != entry.clipped)
      if (!setLayerClipped(doc, i, entry.clipped).ok) {
        // `setLayerClipped()` refuses the bottom layer, a base with no alpha
        // and a live `Mix` pair as well as a lock. Only the lock gets its own
        // report line; the rest fall out as "not fully applied" through the
        // changed-check below, because they are properties of where the layer
        // now sits rather than of the comp.
        if (doc.layers[i].locked) lockRefused = true;
      }

    if (lockRefused) local.lockedLayers.push_back(layerLabel(doc.layers[i].name, i));
    if (doc.layers[i].visible != wasVisible || doc.layers[i].clipped != wasClipped ||
        doc.layers[i].opacity != wasOpacity || doc.layers[i].blend != wasBlend)
      ++local.layersChanged;
  }

  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (!covered[i]) local.uncoveredLayers.push_back(layerLabel(doc.layers[i].name, i));

  if (report != nullptr) *report = local;

  const std::string label =
      "restore " + (comp.name.empty() ? std::string("comp") : "comp \"" + comp.name + "\"");
  return succeed(label, compIndex);
}

std::string layerCompRestoreSummary(const LayerCompRestoreReport& report) {
  std::string s;
  if (!report.missingLayers.empty()) {
    s += std::to_string(report.missingLayers.size()) +
         " layer state(s) could not be restored because the layer is no longer in this "
         "document (" + joinNames(report.missingLayers) + "). ";
  }
  if (!report.lockedLayers.empty()) {
    s += std::to_string(report.lockedLayers.size()) +
         " locked layer(s) kept their opacity, blend and clip -- a lock freezes those, and "
         "only visibility is still restored (" + joinNames(report.lockedLayers) +
         "). Unlock them and restore again. ";
  }
  if (!report.unsettableBlends.empty()) {
    s += std::to_string(report.unsettableBlends.size()) +
         " blend value(s) this build cannot set were left as they are rather than substituted "
         "(" + joinNames(report.unsettableBlends) + "). ";
  }
  if (!report.uncoveredLayers.empty()) {
    s += std::to_string(report.uncoveredLayers.size()) +
         " layer(s) were added after this comp was captured and were left untouched (" +
         joinNames(report.uncoveredLayers) + "). ";
  }
  if (s.empty() && report.layersChanged == 0) {
    s = "The document was already in this comp's state, so nothing changed. All " +
        std::to_string(report.entriesApplied) + " layer state(s) matched.";
  }
  if (!s.empty() && !report.missingLayers.empty()) {
    s += "The " + std::to_string(report.entriesApplied) +
         " state(s) that did match were applied; a comp is matched by layer id and never by "
         "position, so nothing landed on a layer it was not captured from.";
  }
  return s;
}

LayerOpResult renameLayerComp(Document& doc, size_t compIndex, std::string name) {
  LayerOpResult refusal;
  if (!compInRange(doc, compIndex, "rename comp", &refusal)) return refusal;
  const std::string label = "rename " + describeComp(doc, compIndex) + " to \"" + name + "\"";
  doc.comps[compIndex].name = std::move(name);
  return succeed(label, compIndex);
}

LayerOpResult deleteLayerComp(Document& doc, size_t compIndex) {
  LayerOpResult refusal;
  if (!compInRange(doc, compIndex, "delete comp", &refusal)) return refusal;
  const std::string label = "delete " + describeComp(doc, compIndex);
  doc.comps.erase(doc.comps.begin() + static_cast<std::ptrdiff_t>(compIndex));
  return succeed(label, compIndex);
}

LayerOpResult moveLayerComp(Document& doc, size_t from, size_t to) {
  LayerOpResult refusal;
  if (!compInRange(doc, from, "move comp", &refusal)) return refusal;
  if (!compInRange(doc, to, "move comp", &refusal)) return refusal;
  const std::string label = "move " + describeComp(doc, from) + " to position " +
                            std::to_string(to);
  if (from == to) return succeed(label, to);
  LayerComp moved = std::move(doc.comps[from]);
  doc.comps.erase(doc.comps.begin() + static_cast<std::ptrdiff_t>(from));
  doc.comps.insert(doc.comps.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
  return succeed(label, to);
}

}  // namespace np
