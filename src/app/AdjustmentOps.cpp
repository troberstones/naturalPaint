#include "app/AdjustmentOps.hpp"

#include "app/PixelOpBridge.hpp"
#include "core/Histogram.hpp"
#include "core/SelectionMask.hpp"
#include "ops/PointOpTiles.hpp"

namespace np {
namespace {

// Each command's params -> the one-element `PointOpRun` the engine takes.
//
// The lambda captures its params **by value**. That is not incidental: the
// run outlives the call that built it only in the sense that
// `applyPixelFilter()` holds it for the duration of the engine call, but a
// capture by reference would make every one of these a dangling read the
// moment a caller passed a temporary -- and `ui/MacPaintUI.cpp`'s dialogs
// pass exactly that, since they build the params struct inline from their
// slider state. By value costs one small copy per adjustment (not per texel;
// the `std::function` is constructed once and then invoked per texel), which
// is nothing against a canvas-sized loop.
PointOpRun runFor(const std::array<LevelsParams, 3>& channels) {
  return PointOpRun{[channels](const std::array<float, 3>& rgb) {
    return applyLevels(rgb, channels);
  }};
}

PointOpRun runFor(const std::array<Curve, 3>& channels) {
  return PointOpRun{[channels](const std::array<float, 3>& rgb) {
    return applyCurves(rgb, channels);
  }};
}

PointOpRun runFor(const ExposureParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyExposure(rgb, p); }};
}

PointOpRun runFor(const ChannelMixerParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyChannelMixer(rgb, p); }};
}

PointOpRun runFor(const GrayscaleParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyGrayscale(rgb, p); }};
}

// The nine that needed new arithmetic, plus Invert. Each is the same three
// lines as the five above; the overload set is what lets every `applyX()` and
// `previewX()` below share one construction rather than each spelling out its
// own lambda, which is where a preview and a commit would drift apart.
PointOpRun runFor(const GainOffsetGammaParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyGainOffsetGamma(rgb, p); }};
}

PointOpRun runFor(const HueSaturationParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyHueSaturation(rgb, p); }};
}

PointOpRun runFor(const VibranceParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyVibrance(rgb, p); }};
}

PointOpRun runFor(const ColorBalanceParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyColorBalance(rgb, p); }};
}

PointOpRun runFor(const BlackAndWhiteParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyBlackAndWhite(rgb, p); }};
}

PointOpRun runFor(const PhotoFilterParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyPhotoFilter(rgb, p); }};
}

PointOpRun runFor(const InvertParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyInvert(rgb, p); }};
}

PointOpRun runFor(const PosterizeParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyPosterize(rgb, p); }};
}

PointOpRun runFor(const ThresholdParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyThreshold(rgb, p); }};
}

PointOpRun runFor(const GradientMapParams& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyGradientMap(rgb, p); }};
}

}  // namespace

FilterOpResult applyLevelsAdjustment(OpenDocument& doc,
                                     const std::array<LevelsParams, 3>& channels) {
  return applyPixelFilter(doc, pointOpTiles, runFor(channels), "levels");
}

FilterOpResult applyCurvesAdjustment(OpenDocument& doc, const std::array<Curve, 3>& channels) {
  return applyPixelFilter(doc, pointOpTiles, runFor(channels), "curves");
}

FilterOpResult applyExposureAdjustment(OpenDocument& doc, const ExposureParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "exposure");
}

FilterOpResult applyChannelMixerAdjustment(OpenDocument& doc, const ChannelMixerParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "channel mixer");
}

FilterOpResult applyDesaturate(OpenDocument& doc) {
  // Default `GrayscaleParams` -- Rec.709 weights. Constructed here rather than
  // taken as an argument because the menu item takes no parameters; see this
  // file's header on why Desaturate has no dialog and no preview twin.
  return applyPixelFilter(doc, pointOpTiles, runFor(GrayscaleParams{}), "desaturate");
}

// Each below is its `apply` twin's params-building preamble feeding
// `computePixelFilter()` instead of `applyPixelFilter()` -- same `runFor()`
// overload, so a preview cannot silently pick different parameters than the
// button next to it would commit.
FilterOpResult previewLevelsAdjustment(const OpenDocument& doc,
                                       const std::array<LevelsParams, 3>& channels,
                                       TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(channels), previewOut);
}

FilterOpResult previewCurvesAdjustment(const OpenDocument& doc,
                                       const std::array<Curve, 3>& channels,
                                       TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(channels), previewOut);
}

FilterOpResult previewExposureAdjustment(const OpenDocument& doc, const ExposureParams& params,
                                         TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewChannelMixerAdjustment(const OpenDocument& doc,
                                             const ChannelMixerParams& params,
                                             TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

// --------------------------------------------------------------------------
// The nine that needed new arithmetic, and Invert
// --------------------------------------------------------------------------
//
// The history labels are what the History panel shows, so each is the menu
// item's own name in lower case -- "brightness/contrast", not "gain offset
// gamma". A user undoing a step reads the command they chose, not the
// primitive underneath it.

FilterOpResult applyBrightnessContrast(OpenDocument& doc, const GainOffsetGammaParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "brightness/contrast");
}

FilterOpResult applyHueSaturationAdjustment(OpenDocument& doc, const HueSaturationParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "hue/saturation");
}

FilterOpResult applyVibranceAdjustment(OpenDocument& doc, const VibranceParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "vibrance");
}

FilterOpResult applyColorBalanceAdjustment(OpenDocument& doc, const ColorBalanceParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "colour balance");
}

FilterOpResult applyBlackAndWhiteAdjustment(OpenDocument& doc, const BlackAndWhiteParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "black & white");
}

FilterOpResult applyPhotoFilterAdjustment(OpenDocument& doc, const PhotoFilterParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "photo filter");
}

FilterOpResult applyPosterizeAdjustment(OpenDocument& doc, const PosterizeParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "posterize");
}

FilterOpResult applyThresholdAdjustment(OpenDocument& doc, const ThresholdParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "threshold");
}

FilterOpResult applyGradientMapAdjustment(OpenDocument& doc, const GradientMapParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "gradient map");
}

FilterOpResult applyInvert(OpenDocument& doc, const InvertParams& params) {
  return applyPixelFilter(doc, pointOpTiles, runFor(params), "invert");
}

FilterOpResult previewBrightnessContrast(const OpenDocument& doc,
                                         const GainOffsetGammaParams& params,
                                         TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewHueSaturationAdjustment(const OpenDocument& doc,
                                              const HueSaturationParams& params,
                                              TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewVibranceAdjustment(const OpenDocument& doc, const VibranceParams& params,
                                         TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewColorBalanceAdjustment(const OpenDocument& doc,
                                             const ColorBalanceParams& params,
                                             TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewBlackAndWhiteAdjustment(const OpenDocument& doc,
                                              const BlackAndWhiteParams& params,
                                              TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewPhotoFilterAdjustment(const OpenDocument& doc,
                                            const PhotoFilterParams& params,
                                            TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewPosterizeAdjustment(const OpenDocument& doc, const PosterizeParams& params,
                                          TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewThresholdAdjustment(const OpenDocument& doc, const ThresholdParams& params,
                                          TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

FilterOpResult previewGradientMapAdjustment(const OpenDocument& doc,
                                            const GradientMapParams& params,
                                            TileStore* previewOut) {
  return computePixelFilter(doc, pointOpTiles, runFor(params), previewOut);
}

// --------------------------------------------------------------------------
// The four solvers
// --------------------------------------------------------------------------

HistogramResult adjustmentHistogramFor(const OpenDocument& doc) {
  HistogramParams params = HistogramParams::wholeDocument(doc.document);
  // The ACTIVE LAYER only, and its bounding box when a selection is engaged --
  // this header's own "what gets histogrammed" section states both, and states
  // the approximation the bounding box is. `activeLayerIndex()` returning
  // nothing leaves the default 0, which is harmless: with no layer there is
  // nothing to solve for and every solver's degenerate path returns neutral
  // parameters anyway.
  params.sampleAllLayers = false;
  if (const std::optional<size_t> idx = activeLayerIndex(doc))
    params.activeLayerIndex = static_cast<int32_t>(*idx);
  if (doc.selection.has_value()) {
    if (const std::optional<SelectionBounds> b = selectionBounds(*doc.selection)) {
      params.regionMin = PixelCoord{b->x0, b->y0};
      params.regionMax = PixelCoord{b->x1, b->y1};
    }
  }
  return computeHistogram(doc.document, params);
}

FilterOpResult applyAutoTone(OpenDocument& doc, const AutoLevelsParams& tuning) {
  // Solve, then hand the result to the ORDINARY Levels path -- there is no
  // auto-tone engine, which is docs/operations.md §1.2's rule made literal.
  // The history label says "auto tone" rather than "levels", because the
  // command the user chose is what an undo step has to be readable as.
  const std::array<LevelsParams, 3> solved = solveAutoTone(adjustmentHistogramFor(doc), tuning);
  return applyPixelFilter(doc, pointOpTiles, runFor(solved), "auto tone");
}

FilterOpResult applyAutoContrast(OpenDocument& doc, const AutoLevelsParams& tuning) {
  const std::array<LevelsParams, 3> solved = solveAutoContrast(adjustmentHistogramFor(doc), tuning);
  return applyPixelFilter(doc, pointOpTiles, runFor(solved), "auto contrast");
}

FilterOpResult applyAutoColor(OpenDocument& doc, const AutoLevelsParams& tuning) {
  const std::array<LevelsParams, 3> solved = solveAutoColor(adjustmentHistogramFor(doc), tuning);
  return applyPixelFilter(doc, pointOpTiles, runFor(solved), "auto colour");
}

FilterOpResult applyEqualize(OpenDocument& doc, const AutoLevelsParams& tuning) {
  // Equalize is the one solver whose answer is not expressible as Levels, so
  // it returns a Curve and rides the ordinary Curves path instead. One curve
  // for all three channels, which is what keeps it hue-preserving --
  // ops/AutoLevels.hpp makes the same argument for auto-contrast.
  const Curve curve = solveEqualize(adjustmentHistogramFor(doc), tuning);
  const std::array<Curve, 3> channels{curve, curve, curve};
  return applyPixelFilter(doc, pointOpTiles, runFor(channels), "equalize");
}

}  // namespace np
