#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/Command.hpp"
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
// reasons, and none of them is changed here. The four `from*()` helpers below
// are the whole bridge.
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
