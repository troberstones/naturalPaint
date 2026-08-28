#include "app/AdjustmentOps.hpp"

#include "app/PixelOpBridge.hpp"
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

}  // namespace np
