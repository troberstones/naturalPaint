#include "app/Command.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "app/AdjustmentOps.hpp"
#include "app/FilterOps.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "core/Blend.hpp"
#include "core/LayerOps.hpp"
#include "ops/Transform.hpp"

namespace np {
namespace {

// --- the four shapes an adapter has to bridge ----------------------------
//
// Each existing family reports differently, for its own good reasons, and
// none of them is changed here. These four helpers are the whole translation
// layer, written once so that fifty adapters do not each invent one.

CommandResult refused(std::string why) {
  CommandResult r;
  r.ok = false;
  r.status = std::move(why);
  return r;
}

CommandResult fromFilterResult(const FilterOpResult& f, const OpenDocument& doc,
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

CommandResult fromDocumentOutcome(const DocumentOpOutcome& o, const char* opName) {
  CommandResult r;
  r.ok = o.ok;
  r.changesPixels = true;
  r.status = o.ok ? std::string(opName) + ": done" : o.error;
  // A document op reports no texel count; `resizeImage()` refusing to record a
  // no-op is the case this stands in for, and the replayer's zero-change
  // warning is driven by `changesPixels` plus this staying 0 only when the op
  // genuinely did nothing. `applyImageSize()` cannot tell us which, so it is
  // reported as a change -- an honest 1 rather than a 0 that would raise a
  // warning about a resize that did happen.
  if (o.ok) r.texelsChanged = 1;
  return r;
}

CommandResult fromDocumentOpResult(const DocumentOpResult& d, const char* opName) {
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
// menu and panel route. A replayer that did not would leave `activeLayer`
// pointing past the end of a stack a flatten had just collapsed, and the very
// next step would act on the wrong layer or on none: docs/automation-plan.md
// §7's "the active layer is the state most likely to make a replay wrong and
// green", found here by this module's own selftest walking off the end.
CommandResult fromLayerEdit(OpenDocument& doc, const LayerEditResult& e, const char* opName) {
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

// Empty string when the target resolves; otherwise the refusal sentence.
std::string resolveTarget(const OpenDocument& doc, const JsonValue& params, size_t* out) {
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

// The precondition every pixel op shares.
std::string pixelOpUnavailable(const OpenDocument& doc, const JsonValue&) {
  const Layer* target = activeLayerOf(doc);
  const PixelOpRefusal why = pixelOpRefusalFor(target);
  if (why == PixelOpRefusal::None) return {};
  return pixelOpRefusalMessage(why, target, "this command");
}

std::string layerTargetUnavailable(const OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  return resolveTarget(doc, params, &index);
}

std::string documentUnavailable(const OpenDocument& doc, const JsonValue&) {
  if (doc.document.layers.empty()) return "refused: this document has no layers.";
  return {};
}

// --- the commands --------------------------------------------------------

CommandResult doSelectLayer(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return refused(why);
  doc.activeLayer = index;
  CommandResult r;
  r.ok = true;
  r.status = "select layer: \"" + doc.document.layers[index].name + "\"";
  return r;
}

CommandResult doImageSize(OpenDocument& doc, const JsonValue& params) {
  if (!params.hasNumber("width") || !params.hasNumber("height"))
    return refused("refused: image_size needs a width and a height.");
  const double w = params.numberOr("width", 0.0);
  const double h = params.numberOr("height", 0.0);
  if (w < 1.0 || h < 1.0 || w != std::floor(w) || h != std::floor(h))
    return refused("refused: image_size needs whole pixel counts of at least 1.");
  ResampleKernel kernel = ResampleKernel::CatmullRom;
  const std::string kernelName = params.stringOr("kernel", "");
  if (!kernelName.empty()) {
    const std::optional<ResampleKernel> k = resampleKernelFromName(kernelName);
    if (!k) return refused("refused: no resample kernel is named \"" + kernelName + "\".");
    kernel = *k;
  }
  return fromDocumentOutcome(applyImageSize(doc, static_cast<uint32_t>(w),
                                            static_cast<uint32_t>(h), kernel),
                             "image size");
}

CommandResult doFlattenImage(OpenDocument& doc, const JsonValue&) {
  const LayerEditResult e = applyLayerCommand(doc, LayerCommand::FlattenImage, doc.activeLayer);
  return fromLayerEdit(doc, e, "flatten image");
}

CommandResult doGaussianBlur(OpenDocument& doc, const JsonValue& params) {
  if (!params.hasNumber("sigma")) return refused("refused: filter_gaussian_blur needs a sigma.");
  const double sigma = params.numberOr("sigma", 0.0);
  if (!(sigma > 0.0)) return refused("refused: a Gaussian blur needs a sigma above zero.");
  return fromFilterResult(applyGaussianBlur(doc, static_cast<float>(sigma)), doc, "gaussian blur");
}

CommandResult doSetLayerBlend(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return refused(why);
  const std::string modeName = params.stringOr("mode", "");
  const std::optional<BlendMode> mode = blendModeFromName(modeName);
  if (!mode) return refused("refused: no blend mode is named \"" + modeName + "\".");
  return fromDocumentOpResult(recordLayerEdit(doc, setLayerBlend(doc.document, index, *mode)),
                              "set blend mode");
}

CommandResult doThreshold(OpenDocument& doc, const JsonValue& params) {
  ThresholdParams p;
  p.threshold = static_cast<float>(params.numberOr("threshold", p.threshold));
  p.amount = static_cast<float>(params.numberOr("amount", p.amount));
  return fromFilterResult(applyThresholdAdjustment(doc, p), doc, "threshold");
}

// --- the table -----------------------------------------------------------

const std::vector<CommandSpec>& table() {
  static const std::vector<CommandSpec> kTable = {
      {"select_layer", "Select Layer", {"layer"}, layerTargetUnavailable, doSelectLayer},
      {"image_size", "Image Size", {"width", "height", "kernel"}, documentUnavailable, doImageSize},
      {"flatten_image", "Flatten Image", {}, documentUnavailable, doFlattenImage},
      {"filter_gaussian_blur", "Gaussian Blur", {"sigma"}, pixelOpUnavailable, doGaussianBlur},
      {"set_layer_blend", "Set Blend Mode", {"layer", "mode"}, layerTargetUnavailable, doSetLayerBlend},
      {"adjust_threshold", "Threshold", {"threshold", "amount"}, pixelOpUnavailable, doThreshold},
  };
  return kTable;
}

}  // namespace

size_t layerIndexNamed(const Document& doc, std::string_view name) {
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (doc.layers[i].name == name) return i;
  return doc.layers.size();
}

const std::vector<CommandSpec>& allCommands() { return table(); }

const CommandSpec* findCommand(std::string_view id) {
  for (const CommandSpec& spec : table())
    if (id == spec.id) return &spec;
  return nullptr;
}

CommandResult applyCommand(OpenDocument& doc, const Command& command) {
  const CommandSpec* spec = findCommand(command.id);
  if (spec == nullptr) {
    return refused("refused: this build has no command called \"" + command.id +
                   "\". An action written by a newer build is refused rather than run "
                   "with the step skipped, because a skipped step writes a file that "
                   "looks correct and is not.");
  }
  const std::string unavailable = spec->unavailableReason(doc, command.params);
  if (!unavailable.empty()) return refused(unavailable);
  return spec->apply(doc, command.params);
}

}  // namespace np
