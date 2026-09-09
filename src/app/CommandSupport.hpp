#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/Command.hpp"
#include "app/CropTool.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/FilterOps.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"

// app/CommandSupport -- the translation layer every command adapter shares,
// and the targeting rule they all obey.
//
// **Header-only, and internal to the command family files**, exactly as
// app/PixelOpBridge.hpp is header-only and internal to the two op files that
// share it. The precedent is deliberate: that header exists because nineteen
// adjustments and seven filters were asking the same five questions, and "the
// moment two files answer them separately, the two menus start disagreeing
// about what 'the selected layer' means". The command families are that same
// argument one level up again -- there are four of them now, and a second
// hand-written `resolveTarget()` is how two commands would come to disagree
// about what `"layer": "Base"` means.
//
// Each existing family reports its outcome differently, for its own good
// reasons, and none of them is changed here. The five `from*()` helpers below
// are the whole bridge -- one per outcome type the appliers already return,
// which is why there are five rather than one: `FilterOpResult`,
// `DocumentOpOutcome`, `DocumentOpResult`, `LayerEditResult` and
// `DocumentTransformResult` differ in what they can honestly say about how
// much changed, and flattening them into one would mean losing the
// distinction that makes a zero texel count meaningful.
namespace np {

inline CommandResult commandRefused(std::string why) {
  CommandResult r;
  r.ok = false;
  r.status = std::move(why);
  return r;
}

inline CommandResult fromFilterResult(const FilterOpResult& f, const OpenDocument& doc,
                                      const char* opName) {
  CommandResult r;
  r.changesPixels = true;
  if (f.refusal != PixelOpRefusal::None) {
    r.ok = false;
    r.status = pixelOpRefusalMessage(f.refusal, activeLayerOf(doc), opName);
    return r;
  }
  r.ok = true;
  r.texelsChanged = f.texelsChanged;
  r.status = std::string(opName) + ": " + std::to_string(f.texelsChanged) + " texels changed";
  return r;
}

inline CommandResult fromDocumentOutcome(const DocumentOpOutcome& o, const char* opName) {
  CommandResult r;
  r.ok = o.ok;
  r.changesPixels = true;
  r.status = o.ok ? std::string(opName) + ": done" : o.error;
  // A document op reports no texel count. `applyImageSize()` cannot tell us
  // whether it resized or declined a no-op, so a success is reported as a
  // change -- an honest 1 rather than a 0 that would raise the replayer's
  // "this step did nothing" warning about a resize that did happen.
  if (o.ok) r.texelsChanged = 1;
  return r;
}

inline CommandResult fromDocumentOpResult(const DocumentOpResult& d, const char* opName) {
  CommandResult r;
  r.ok = d.ok;
  r.warnings = d.warnings;
  r.status = d.ok ? std::string(opName) + ": done" : d.error;
  if (d.ok) r.texelsChanged = 1;
  return r;
}

// **This one takes the document, because a layer command moves the selection
// and the next step in an action depends on where it landed.**
// `LayerEditResult::selected` is where the gesture left the selection -- the
// new layer after a create, the survivor after a flatten -- and
// ui/MacPaintUI.cpp:364 assigns it back through `setActiveLayer()` on every
// menu and panel route. A replayer that did not would leave a create's every
// following step painting on the layer the user had selected BEFORE:
// docs/automation-plan.md §7's "the active layer is the state most likely to
// make a replay wrong and green".
inline CommandResult fromLayerEdit(OpenDocument& doc, const LayerEditResult& e,
                                   const char* opName) {
  CommandResult r;
  r.ok = e.ok;
  r.warnings = e.warnings;
  r.status = e.ok ? std::string(opName) + ": done" : e.error;
  if (e.ok) {
    setActiveLayer(doc, e.selected);
    r.texelsChanged = 1;
  }
  return r;
}

// The fifth, and the only one that can tell a real edit from a no-op it was
// asked for. `ops/DocumentTransform`'s crops (`applyCropToSelection()`,
// `applyTrimToContent()`, app/CropTool.hpp) report a
// `DocumentTransformResult`, which -- unlike `DocumentOpOutcome` above --
// carries `previousWidth`/`previousHeight`. `fromDocumentOutcome()` has to
// report an honest 1 for every success precisely because it cannot see those,
// and says so; here they are, so this helper does better rather than copying
// that concession.
//
// **`texelsChanged == 0` on a successful crop is the point of this helper,
// not an oversight.** `applyCropRegion()` records no history entry for a
// region that is already the whole canvas -- "a no-op the user asked for is
// not an edit", `applyImageSize()`'s own rule. In the UI that is a
// nothing-happens the user can see. In a batch it is thirty files written
// unmodified and reported as successes, so the zero is passed through for the
// replayer to turn into a warning (docs/automation-plan.md §7). Reporting 1
// here would make `trim_to_content` on an already-tight document
// indistinguishable from one that trimmed.
//
// The locked-layer count becomes a WARNING rather than a refusal, which is
// `ops/DocumentTransform.hpp` §5's own decision restated at this layer: a
// document-level op moves every layer including locked ones, and that header
// argues at length why refusing on a lock would be wrong here even though a
// layer-level op must. §5 also says it is a number "a UI needs to say so out
// loud rather than have the user discover it" -- a batch report is such a UI,
// and this is the only place the count could reach it.
inline CommandResult fromDocumentTransform(const DocumentTransformResult& d,
                                           const OpenDocument& doc, const char* opName) {
  CommandResult r;
  r.ok = d.ok;
  r.changesPixels = true;
  if (!d.ok) {
    r.status = d.error;
    return r;
  }
  const bool extentChanged = d.previousWidth != static_cast<int32_t>(doc.document.width) ||
                             d.previousHeight != static_cast<int32_t>(doc.document.height);
  r.texelsChanged = extentChanged ? 1 : 0;
  r.status = extentChanged ? std::string(opName) + ": " + std::to_string(d.previousWidth) + "x" +
                                 std::to_string(d.previousHeight) + " -> " +
                                 std::to_string(doc.document.width) + "x" +
                                 std::to_string(doc.document.height)
                           : std::string(opName) + ": the document was already that size, so "
                                                   "nothing was changed and nothing was recorded";
  if (d.lockedLayersMoved > 0)
    r.warnings.push_back(std::to_string(d.lockedLayersMoved) +
                         " locked layer(s) were moved: a document-level op changes the whole "
                         "canvas, so a lock cannot exempt a layer from it.");
  return r;
}

// --- targeting -----------------------------------------------------------
//
// Every layer-addressing command reads an optional `"layer"` key naming its
// target and falls back to the active layer when it is absent. The recorder
// always writes the name (docs/automation-plan.md §5); a hand-written action
// may leave it out and mean "whatever `select_layer` last chose".
//
// Empty string when the target resolves; otherwise the refusal sentence.
inline std::string resolveTarget(const OpenDocument& doc, const JsonValue& params, size_t* out) {
  const JsonValue* named = params.find("layer");
  if (named != nullptr && named->isString()) {
    const size_t i = layerIndexNamed(doc.document, named->asString());
    if (i >= doc.document.layers.size()) {
      return "refused: this document has no layer named \"" + named->asString() +
             "\". An action addresses layers by name, so that replaying it on another "
             "document cannot silently act on a different one.";
    }
    *out = i;
    return {};
  }
  const std::optional<size_t> active = activeLayerIndex(doc);
  if (!active) return "refused: this document has no layer to act on.";
  *out = *active;
  return {};
}

// --- the shared preconditions -------------------------------------------

// What every pixel op needs: an active layer that can take pixels.
inline std::string pixelOpUnavailable(const OpenDocument& doc, const JsonValue&) {
  const Layer* target = activeLayerOf(doc);
  const PixelOpRefusal why = pixelOpRefusalFor(target);
  if (why == PixelOpRefusal::None) return {};
  return pixelOpRefusalMessage(why, target, "this command");
}

inline std::string layerTargetUnavailable(const OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  return resolveTarget(doc, params, &index);
}

inline std::string documentUnavailable(const OpenDocument& doc, const JsonValue&) {
  if (doc.document.layers.empty()) return "refused: this document has no layers.";
  return {};
}

// A document with no layers can still take a layer-creating command; that is
// the one thing it can take.
inline std::string documentAlwaysAvailable(const OpenDocument&, const JsonValue&) { return {}; }

}  // namespace np
