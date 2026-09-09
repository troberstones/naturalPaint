#include <cmath>
#include <optional>

#include "app/AdjustmentOps.hpp"
#include "app/CommandSupport.hpp"
#include "app/FilterOps.hpp"
#include "ops/Transform.hpp"

// app/CommandsImage -- the command rows for everything that changes pixels or
// the document's own geometry: the Filter menu's seven, Image > Adjustments'
// nineteen plus its four auto solvers, and the four document commands.
//
// Every one of them is an adapter and nothing more: read the parameters,
// refuse what is missing or out of range by name, call the applier that
// already exists, and translate its result through app/CommandSupport.hpp.
// **No adapter here may reimplement an op**; if a value needs clamping or a
// default, the applier's own params struct is where that already lives.
namespace np {
namespace {

CommandResult doImageSize(OpenDocument& doc, const JsonValue& params) {
  if (!params.hasNumber("width") || !params.hasNumber("height"))
    return commandRefused("refused: image_size needs a width and a height.");
  const double w = params.numberOr("width", 0.0);
  const double h = params.numberOr("height", 0.0);
  if (w < 1.0 || h < 1.0 || w != std::floor(w) || h != std::floor(h))
    return commandRefused("refused: image_size needs whole pixel counts of at least 1.");
  ResampleKernel kernel = ResampleKernel::CatmullRom;
  const std::string kernelName = params.stringOr("kernel", "");
  if (!kernelName.empty()) {
    const std::optional<ResampleKernel> k = resampleKernelFromName(kernelName);
    if (!k) return commandRefused("refused: no resample kernel is named \"" + kernelName + "\".");
    kernel = *k;
  }
  return fromDocumentOutcome(
      applyImageSize(doc, static_cast<uint32_t>(w), static_cast<uint32_t>(h), kernel),
      "image size");
}

CommandResult doGaussianBlur(OpenDocument& doc, const JsonValue& params) {
  if (!params.hasNumber("sigma"))
    return commandRefused("refused: filter_gaussian_blur needs a sigma.");
  const double sigma = params.numberOr("sigma", 0.0);
  if (!(sigma > 0.0)) return commandRefused("refused: a Gaussian blur needs a sigma above zero.");
  return fromFilterResult(applyGaussianBlur(doc, static_cast<float>(sigma)), doc, "gaussian blur");
}

CommandResult doThreshold(OpenDocument& doc, const JsonValue& params) {
  ThresholdParams p;
  p.threshold = static_cast<float>(params.numberOr("threshold", p.threshold));
  p.amount = static_cast<float>(params.numberOr("amount", p.amount));
  return fromFilterResult(applyThresholdAdjustment(doc, p), doc, "threshold");
}

}  // namespace

void registerImageCommands(std::vector<CommandSpec>* out) {
  out->push_back({"image_size", "Image Size", {"width", "height", "kernel"}, documentUnavailable,
                  doImageSize});
  out->push_back({"filter_gaussian_blur", "Gaussian Blur", {"sigma"}, pixelOpUnavailable,
                  doGaussianBlur});
  out->push_back({"adjust_threshold", "Threshold", {"threshold", "amount"}, pixelOpUnavailable,
                  doThreshold});
}

}  // namespace np
