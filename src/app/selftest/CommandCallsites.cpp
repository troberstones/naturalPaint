#include "app/selftest/Support.hpp"

#include <functional>

#include "app/AdjustmentOps.hpp"
#include "app/Command.hpp"
#include "app/CommandsImage.hpp"
#include "app/CommandsLayers.hpp"
#include "app/CropTool.hpp"
#include "app/FilterOps.hpp"
#include "app/Recorder.hpp"
#include "core/LayerOps.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {
namespace {

// A 64x64 RGB document whose one tile holds a ramp with a hard edge in it.
//
// **Not a flat fill, and that is the difference between this section proving
// something and proving nothing.** Every filter here is the identity on a
// uniform surface: a blur of a constant is that constant, and
// `applyPixelFilter()` reports `texelsChanged == 0` and writes nothing. Two
// paths that both write nothing compare bit-identical, so a flat fixture would
// make section B green with the appliers unplugged entirely.
//
// The ramp also gives the four solvers a histogram to solve against, and the
// hard edge gives Median and Unsharp Mask something to find.
OpenDocument makeCallsiteDocument() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "callsites");
  od.document.layers[0].name = "Base";
  Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
  for (int32_t y = 0; y < kTileSize; ++y) {
    for (int32_t x = 0; x < kTileSize; ++x) {
      const float ramp = static_cast<float>(x % 64) / 63.0f;
      const float step = (x < 32) ? 0.15f : 0.85f;
      t.writePixel(PixelCoord{x, y}, {ramp, step, 1.0f - ramp * 0.5f, 1.0f});
    }
  }
  od.recordEdit("callsite fixture", EditKind::Content);
  return od;
}

// Bit-exact, including NaN-free-ness: `==` on the floats, not a tolerance.
// The claim under test is "this reroute changed nothing", and a tolerance
// would let a reroute that ran a slightly different op pass.
bool sameLayerPixels(const OpenDocument& a, const OpenDocument& b) {
  if (a.document.layers.size() != b.document.layers.size()) return false;
  for (size_t i = 0; i < a.document.layers.size(); ++i) {
    const Layer& la = a.document.layers[i];
    const Layer& lb = b.document.layers[i];
    if (la.rgbTiles.has_value() != lb.rgbTiles.has_value()) return false;
    if (!la.rgbTiles.has_value()) continue;
    const Tile* ta = la.rgbTiles->find(TileCoord{0, 0});
    const Tile* tb = lb.rgbTiles->find(TileCoord{0, 0});
    if ((ta == nullptr) != (tb == nullptr)) return false;
    if (ta == nullptr) continue;
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x)
        if (ta->readPixel(PixelCoord{x, y}) != tb->readPixel(PixelCoord{x, y})) return false;
  }
  return true;
}

// The params a UI control holds, at values that are not the identity -- so
// that a command which quietly lost a parameter would produce a different
// picture from the applier called with the same struct, which is exactly what
// section B asks.
UnsharpParams unsharpFixture() {
  UnsharpParams p;
  p.blur.kind = BlurKind::Gaussian;
  p.blur.sigma = 2.5f;
  p.amount = 1.75f;
  p.threshold = 0.03f;
  return p;
}

NoiseParams noiseFixture() {
  NoiseParams p;
  p.amount = 0.25f;
  p.distribution = NoiseDistribution::Uniform;
  p.monochrome = true;
  // A seed that is not 0, so a step that dropped the key would produce
  // different grain rather than the same grain by accident.
  p.seed = 0x5eedu;
  return p;
}

EmbossParams embossFixture() {
  EmbossParams p;
  p.dx = 2;
  p.dy = -3;
  p.depth = 1.75f;
  p.amount = 0.8f;
  return p;
}

MotionBlurParams motionFixture() {
  MotionBlurParams p;
  p.radius = 5;
  p.angleRadians = 0.7f;
  return p;
}

std::array<LevelsParams, 3> levelsFixture() {
  std::array<LevelsParams, 3> c{};
  for (size_t i = 0; i < 3; ++i) {
    c[i].blackIn = 0.05f + 0.01f * static_cast<float>(i);
    c[i].whiteIn = 0.90f - 0.02f * static_cast<float>(i);
    c[i].gamma = 1.2f + 0.1f * static_cast<float>(i);
    c[i].blackOut = 0.02f;
    c[i].whiteOut = 0.98f;
  }
  return c;
}

std::array<Curve, 3> curvesFixture() {
  std::array<Curve, 3> c{};
  c[0] = {{0.0f, 0.1f}, {0.5f, 0.4f}, {1.0f, 0.95f}};
  c[1] = {{0.0f, 0.0f}, {0.25f, 0.45f}, {1.0f, 1.0f}};
  // Deliberately EMPTY: the blue curve nobody touched. `doCurves()` reads a
  // channel of fewer than two points as that channel's identity, and the
  // encoder has to write the empty list rather than skip it -- skipping would
  // make the array two long and refuse the whole step.
  c[2] = {};
  return c;
}

ChannelMixerParams mixerFixture() {
  ChannelMixerParams p;
  p.matrix = {{{0.8f, 0.15f, 0.05f, 0.02f},
               {0.10f, 0.85f, 0.05f, -0.01f},
               {0.05f, 0.10f, 0.85f, 0.03f}}};
  return p;
}

HueSaturationParams hueSatFixture() {
  HueSaturationParams p;
  p.hueDegrees = 35.0f;
  p.saturation = 1.4f;
  p.lightness = 0.08f;
  p.colorize = false;
  p.colorizeHueDegrees = 12.0f;
  p.colorizeSaturation = 0.7f;
  return p;
}

ColorBalanceParams balanceFixture() {
  ColorBalanceParams p;
  p.shadowsLift = {0.05f, -0.02f, 0.01f};
  p.midtonesGamma = {-0.03f, 0.04f, 0.02f};
  p.highlightsGain = {0.02f, 0.01f, -0.04f};
  p.preserveLuminosity = false;
  return p;
}

BlackAndWhiteParams monoFixture() {
  BlackAndWhiteParams p;
  p.reds = 0.4f;
  p.yellows = 0.8f;
  p.greens = 0.5f;
  p.cyans = 0.3f;
  p.blues = 0.2f;
  p.magentas = 0.6f;
  return p;
}

PhotoFilterParams photoFixture() {
  PhotoFilterParams p;
  p.color = {0.95f, 0.6f, 0.25f};
  p.density = 0.4f;
  p.preserveLuminosity = false;
  return p;
}

GradientMapParams gradientFixture() {
  GradientMapParams p;
  p.stops.colorStops = {{0.0f, {0.05f, 0.0f, 0.2f}, 0.5f},
                        {0.45f, {0.8f, 0.3f, 0.1f}, 0.4f},
                        {1.0f, {1.0f, 0.95f, 0.7f}, 0.5f}};
  p.lumaWeights = {0.3f, 0.6f, 0.1f};
  return p;
}

// One row of section B: a command the UI can now issue, and the applier call
// that same control used to make. The two run on two copies of one fixture and
// must leave the identical pixels.
struct RerouteCase {
  const char* what;
  Command command;
  std::function<void(OpenDocument&)> applier;
};

std::vector<RerouteCase> rerouteCases() {
  std::vector<RerouteCase> cases;
  auto add = [&cases](const char* what, Command c, std::function<void(OpenDocument&)> fn) {
    cases.push_back({what, std::move(c), std::move(fn)});
  };

  add("filter_gaussian_blur", gaussianBlurCommand(3.0f),
      [](OpenDocument& d) { applyGaussianBlur(d, 3.0f); });
  add("filter_sharpen", sharpenCommand(1.4f), [](OpenDocument& d) { applySharpen(d, 1.4f); });
  add("filter_unsharp_mask", unsharpMaskCommand(unsharpFixture()),
      [](OpenDocument& d) { applyUnsharpMask(d, unsharpFixture()); });
  add("filter_add_noise", addNoiseCommand(noiseFixture()),
      [](OpenDocument& d) { applyAddNoise(d, noiseFixture()); });
  add("filter_emboss", embossCommand(embossFixture()),
      [](OpenDocument& d) { applyEmboss(d, embossFixture()); });
  add("filter_median", medianCommand(MedianParams{2}),
      [](OpenDocument& d) { applyMedian(d, MedianParams{2}); });
  add("filter_motion_blur", motionBlurCommand(motionFixture()),
      [](OpenDocument& d) { applyMotionBlur(d, motionFixture()); });

  add("adjust_levels", levelsCommand(levelsFixture()),
      [](OpenDocument& d) { applyLevelsAdjustment(d, levelsFixture()); });
  add("adjust_curves", curvesCommand(curvesFixture()),
      [](OpenDocument& d) { applyCurvesAdjustment(d, curvesFixture()); });
  add("adjust_exposure", exposureCommand(ExposureParams{-1.25f}),
      [](OpenDocument& d) { applyExposureAdjustment(d, ExposureParams{-1.25f}); });
  add("adjust_channel_mixer", channelMixerCommand(mixerFixture()),
      [](OpenDocument& d) { applyChannelMixerAdjustment(d, mixerFixture()); });
  add("adjust_desaturate", desaturateCommand(), [](OpenDocument& d) { applyDesaturate(d); });
  add("adjust_brightness_contrast",
      brightnessContrastCommand(GainOffsetGammaParams{1.3f, -0.05f, 1.15f}),
      [](OpenDocument& d) {
        applyBrightnessContrast(d, GainOffsetGammaParams{1.3f, -0.05f, 1.15f});
      });
  add("adjust_hue_saturation", hueSaturationCommand(hueSatFixture()),
      [](OpenDocument& d) { applyHueSaturationAdjustment(d, hueSatFixture()); });
  add("adjust_vibrance", vibranceCommand(VibranceParams{0.6f, {0.3f, 0.6f, 0.1f}}),
      [](OpenDocument& d) {
        applyVibranceAdjustment(d, VibranceParams{0.6f, {0.3f, 0.6f, 0.1f}});
      });
  add("adjust_color_balance", colorBalanceCommand(balanceFixture()),
      [](OpenDocument& d) { applyColorBalanceAdjustment(d, balanceFixture()); });
  add("adjust_black_and_white", blackAndWhiteCommand(monoFixture()),
      [](OpenDocument& d) { applyBlackAndWhiteAdjustment(d, monoFixture()); });
  add("adjust_photo_filter", photoFilterCommand(photoFixture()),
      [](OpenDocument& d) { applyPhotoFilterAdjustment(d, photoFixture()); });
  add("adjust_posterize", posterizeCommand(PosterizeParams{6}),
      [](OpenDocument& d) { applyPosterizeAdjustment(d, PosterizeParams{6}); });
  add("adjust_threshold", thresholdCommand(ThresholdParams{0.35f, 0.9f}),
      [](OpenDocument& d) { applyThresholdAdjustment(d, ThresholdParams{0.35f, 0.9f}); });
  add("adjust_gradient_map", gradientMapCommand(gradientFixture()),
      [](OpenDocument& d) { applyGradientMapAdjustment(d, gradientFixture()); });
  // The five the menu reaches with no parameters at all. Each encoder writes
  // an empty object, which app/CommandsImage.hpp argues is exactly -- not
  // approximately -- what `applyInvert(doc)` meant.
  add("adjust_invert", invertCommand(), [](OpenDocument& d) { applyInvert(d); });
  add("adjust_auto_tone", autoToneCommand(), [](OpenDocument& d) { applyAutoTone(d); });
  add("adjust_auto_contrast", autoContrastCommand(),
      [](OpenDocument& d) { applyAutoContrast(d); });
  add("adjust_auto_color", autoColorCommand(), [](OpenDocument& d) { applyAutoColor(d); });
  add("adjust_equalize", equalizeCommand(), [](OpenDocument& d) { applyEqualize(d); });
  return cases;
}

}  // namespace

// The UI -> command-layer reroute (docs/automation-plan.md step 2).
//
// **What this section is for, and what it deliberately does not do.** It does
// not re-test a blur, a rename or a merge: each of those has its own section,
// and app/selftest/CommandsImage.cpp already asserts that every adapter reads
// what it advertises. What nothing else can see is the seam step 2 built:
//
//  * that the UI's own entry points reach `app::applyCommand()` at all. A
//    dialog that called its applier directly would still produce the right
//    picture and would still pass every existing assertion -- and would be a
//    user action the recorder never sees, which is the entire failure step 2
//    exists to end. Section A arms a `Recorder`, calls the boundary the way
//    the button does, and asserts a step appeared. **That is the assertion
//    that catches a missed site.**
//  * that the reroute changed no pixels. Section B runs each of the
//    twenty-six pixel commands against the applier the control used to call,
//    on two copies of one fixture, and compares bit-exactly. An encoder that
//    misspelt an optional key would produce a step that ran with a defaulted
//    parameter and reported success -- the silent wrong answer
//    docs/automation-plan.md §7 is built around -- and this is what sees it.
//  * that every key an encoder writes is a key the row advertises (section
//    C). The pixel comparison catches a misspelt key only where the default
//    differs from the fixture; this catches it unconditionally, and against
//    the same `paramNames` list the ACTIONS panel will build its editor from.
//  * that the three-way outcome the dialogs share still maps the way it did
//    (section D), including the one case step 2 knowingly changed.
//
// Headless, GPU-free and filesystem-free. It calls into ui/MacPaintUI.cpp, but
// only its three non-ImGui boundary functions -- the same arrangement
// `applySelectRefineAction()` and its five siblings are already tested under.
bool runCommandCallsitesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  Recorder& session = sessionRecorder();

  std::printf("  -- A. each family's UI entry point reaches applyCommand() --\n");
  {
    // The pixel family: what a Filter or Adjustments confirm button calls.
    OpenDocument od = makeCallsiteDocument();
    session.arm(od);
    const PixelCommandOutcome blurred =
        runPixelCommand(od, gaussianBlurCommand(4.0f), "Nothing changed.");
    const std::vector<Command> pixelSteps = session.steps();
    session.stop();
    check(blurred.closeDialog && blurred.status.empty(),
          "pixel: the dialog closes with nothing to say on a real change");
    check(pixelSteps.size() == 1 && pixelSteps[0].id == "filter_gaussian_blur",
          "pixel: runPixelCommand() recorded exactly one filter_gaussian_blur step");
    check(pixelSteps.size() == 1 && pixelSteps[0].params.numberOr("sigma", -1.0) == 4.0,
          "pixel: and the dialog's sigma is the sigma in the step");
  }
  {
    // The layer-gesture family: what the Layer menu and the panel's buttons
    // call, through `runLayerCommand()`'s document half.
    OpenDocument od = makeCallsiteDocument();
    const size_t before = od.document.layers.size();
    session.arm(od);
    const LayerCommandOutcome made = runLayerGesture(od, LayerCommand::NewRgbLayer);
    const std::vector<Command> gestureSteps = session.steps();
    session.stop();
    check(made.ok && made.error.empty() && od.document.layers.size() == before + 1,
          "gesture: runLayerGesture() made the layer and reported no error");
    check(gestureSteps.size() == 1 && gestureSteps[0].id == "new_rgb_layer",
          "gesture: and recorded it as new_rgb_layer");
    // The adoption `runLayerCommand()` stopped doing for itself. A create's
    // landing index is not a clamp of anything, which is why this is the case
    // the suite pins: a stale index would still read as the old top layer.
    check(od.activeLayer + 1 == od.document.layers.size(),
          "gesture: fromLayerEdit() adopted the new layer, so the panel needs no assignment");
  }
  {
    // The setter family: what the BLEND combo, the OPACITY meter and Layer
    // Properties' controls call.
    OpenDocument od = makeCallsiteDocument();
    session.arm(od);
    const LayerCommandOutcome set = runActiveLayerSetter(od, setLayerOpacityCommand(0.25f));
    const std::vector<Command> setterSteps = session.steps();
    session.stop();
    check(set.ok && od.document.layers[od.activeLayer].opacity == 0.25f,
          "setter: runActiveLayerSetter() wrote the opacity");
    check(setterSteps.size() == 1 && setterSteps[0].id == "set_layer_opacity" &&
              setterSteps[0].params.numberOr("opacity", -1.0) == 0.25,
          "setter: and recorded set_layer_opacity carrying it");
    // **The encoder writes no `"layer"` key and the recorder adds one.** Both
    // halves matter and they are different claims. The encoder must not name a
    // layer, because the only name it could take is a panel row's and layer
    // names are not unique -- that is why the three row controls are not
    // migrated at all. The recorder must name one, because "the recorder
    // always writes the name" (app/CommandSupport.hpp's targeting rule) is
    // what makes the step mean the same layer on a replay.
    check(setLayerOpacityCommand(0.25f).params.find("layer") == nullptr,
          "setter: the encoder names no layer, so the target is the active one");
    check(setterSteps.size() == 1 &&
              setterSteps[0].params.stringOr("layer", "") ==
                  od.document.layers[od.activeLayer].name,
          "setter: and the recorder wrote that layer's name into the step");
  }
  {
    // A refusal is not a step, on the UI path as much as on the direct one --
    // the property that stops a recording from containing a click that did
    // nothing.
    OpenDocument od = makeCallsiteDocument();
    recordLayerEdit(od, setLayerLocked(od.document, od.activeLayer, true));
    session.arm(od);
    const PixelCommandOutcome refused =
        runPixelCommand(od, gaussianBlurCommand(4.0f), "Nothing changed.");
    const LayerCommandOutcome refusedSet =
        runActiveLayerSetter(od, setLayerOpacityCommand(0.5f));
    const size_t steps = session.steps().size();
    session.stop();
    check(!refused.closeDialog && !refused.status.empty(),
          "refusal: a refused filter leaves the dialog open with its reason on screen");
    check(!refusedSet.ok && !refusedSet.error.empty(),
          "refusal: and a refused setter reports the sentence core/LayerOps produced");
    check(steps == 0, "refusal: neither one is a recorded step");
  }

  std::printf("  -- B. the reroute changed no pixels --\n");
  {
    const std::vector<RerouteCase> cases = rerouteCases();
    check(cases.size() == 26, "cases: all twenty-six pixel commands the UI issues are covered");
    size_t matched = 0;
    size_t moved = 0;
    std::string firstBad;
    for (const RerouteCase& c : cases) {
      OpenDocument viaCommand = makeCallsiteDocument();
      OpenDocument viaApplier = makeCallsiteDocument();
      const CommandResult r = applyCommand(viaCommand, c.command);
      c.applier(viaApplier);
      if (!r.ok) {
        if (firstBad.empty()) firstBad = std::string(c.what) + ": " + r.status;
        continue;
      }
      // A case that changed nothing would compare equal for the wrong reason
      // -- see `makeCallsiteDocument()`'s own note -- so the count of cases
      // that actually moved a texel is asserted beside the match count.
      if (r.texelsChanged > 0) ++moved;
      if (sameLayerPixels(viaCommand, viaApplier)) {
        ++matched;
      } else if (firstBad.empty()) {
        firstBad = std::string(c.what) + ": pixels differ";
      }
    }
    if (!firstBad.empty()) std::printf("     first mismatch: %s\n", firstBad.c_str());
    check(matched == cases.size(),
          "pixels: every command leaves exactly what its applier left");
    check(moved == cases.size(),
          "pixels: and every one of them actually changed a texel, so the match means something");
  }
  {
    // The two geometry commands and the two crops compare extents rather than
    // texels: their appliers report a `DocumentOpOutcome` / a
    // `DocumentTransformResult`, and what a reroute could get wrong is the
    // width, the height or the kernel.
    OpenDocument viaCommand = makeCallsiteDocument();
    OpenDocument viaApplier = makeCallsiteDocument();
    applyCommand(viaCommand, imageSizeCommand(32, 48, ResampleKernel::Nearest));
    applyImageSize(viaApplier, 32, 48, ResampleKernel::Nearest);
    check(viaCommand.document.width == 32 && viaCommand.document.height == 48 &&
              sameLayerPixels(viaCommand, viaApplier),
          "geometry: image_size resamples to the same pixels with the same kernel");

    OpenDocument grownCommand = makeCallsiteDocument();
    OpenDocument grownApplier = makeCallsiteDocument();
    applyCommand(grownCommand, canvasSizeCommand(96, 96, CanvasAnchor::BottomRight));
    applyCanvasSize(grownApplier, 96, 96, CanvasAnchor::BottomRight);
    check(grownCommand.document.width == 96 && grownCommand.document.height == 96 &&
              sameLayerPixels(grownCommand, grownApplier),
          "geometry: canvas_size anchors the same way, with the anchor crossing by name");

    // The anchor really does travel as a name and not as `anchorIdx`. A cast
    // that had silently become an ordinal would still produce the right
    // picture above, because the grid is declared in the enum's order -- so
    // the encoding is asserted directly.
    const Command anchored = canvasSizeCommand(96, 96, CanvasAnchor::BottomRight);
    check(anchored.params.stringOr("anchor", "") == canvasAnchorName(CanvasAnchor::BottomRight) &&
              !anchored.params.find("anchor")->isNumber(),
          "geometry: the anchor is written as a name, never as the enum's position");

    OpenDocument trimmed = makeCallsiteDocument();
    const CommandResult trim = applyCommand(trimmed, trimToContentCommand());
    check(trim.ok, "geometry: trim_to_content reaches applyTrimToContent() and reports its result");
  }

  std::printf("  -- C. every key an encoder writes is a key the row advertises --\n");
  {
    // The reverse direction of app/selftest/CommandsImage.cpp's own check.
    // That one asserts every key an ADAPTER reads is advertised; this asserts
    // every key an ENCODER writes is, which is what catches the one bug the
    // pixel comparison can miss -- a misspelt optional key whose default
    // happens to equal the fixture's value.
    std::vector<Command> encoded = {gaussianBlurCommand(1.0f),
                                    sharpenCommand(1.0f),
                                    unsharpMaskCommand(unsharpFixture()),
                                    addNoiseCommand(noiseFixture()),
                                    embossCommand(embossFixture()),
                                    medianCommand(MedianParams{1}),
                                    motionBlurCommand(motionFixture()),
                                    levelsCommand(levelsFixture()),
                                    curvesCommand(curvesFixture()),
                                    exposureCommand(ExposureParams{1.0f}),
                                    channelMixerCommand(mixerFixture()),
                                    desaturateCommand(),
                                    brightnessContrastCommand(GainOffsetGammaParams{}),
                                    hueSaturationCommand(hueSatFixture()),
                                    vibranceCommand(VibranceParams{}),
                                    colorBalanceCommand(balanceFixture()),
                                    blackAndWhiteCommand(monoFixture()),
                                    photoFilterCommand(photoFixture()),
                                    posterizeCommand(PosterizeParams{4}),
                                    thresholdCommand(ThresholdParams{}),
                                    gradientMapCommand(gradientFixture()),
                                    invertCommand(),
                                    autoToneCommand(),
                                    autoContrastCommand(),
                                    autoColorCommand(),
                                    equalizeCommand(),
                                    imageSizeCommand(8, 8, ResampleKernel::Nearest),
                                    canvasSizeCommand(8, 8, CanvasAnchor::Center),
                                    cropToSelectionCommand(),
                                    trimToContentCommand(),
                                    setLayerBlendCommand(BlendMode::Multiply),
                                    setLayerOpacityCommand(0.5f),
                                    setLayerVisibleCommand(false),
                                    setLayerLockedCommand(true),
                                    setLayerClippedCommand(true),
                                    setLayerNameCommand("x"),
                                    setLayerColorLabelCommand("red")};
    bool everyIdKnown = true;
    bool everyKeyAdvertised = true;
    std::string firstBad;
    for (const Command& c : encoded) {
      const CommandSpec* spec = findCommand(c.id);
      if (spec == nullptr) {
        everyIdKnown = false;
        if (firstBad.empty()) firstBad = c.id + ": no such row";
        continue;
      }
      for (const std::pair<std::string, JsonValue>& kv : c.params.members()) {
        bool found = false;
        for (const std::string& name : spec->paramNames) found = found || name == kv.first;
        if (!found) {
          everyKeyAdvertised = false;
          if (firstBad.empty()) firstBad = c.id + ": writes unadvertised key \"" + kv.first + "\"";
        }
      }
    }
    if (!firstBad.empty()) std::printf("     first problem: %s\n", firstBad.c_str());
    check(encoded.size() == 37, "encoders: every encoder this build publishes is exercised");
    check(everyIdKnown, "encoders: every id an encoder writes resolves to a registered row");
    check(everyKeyAdvertised, "encoders: and every key it writes is one that row advertises");
  }

  std::printf("  -- D. the dialogs' three-way outcome --\n");
  {
    // The mapping `drawAdjustmentButtons()` and the seven filter dialogs share.
    // Asserted here rather than trusted, because it is the part that decides
    // whether a user sees an explanation or a popup that vanished.
    OpenDocument od = makeCallsiteDocument();
    const PixelCommandOutcome done =
        runPixelCommand(od, thresholdCommand(ThresholdParams{0.4f, 1.0f}), "SENTINEL");
    check(done.closeDialog && done.status.empty(), "outcome: a real change closes and says nothing");

    // A success that moved no texels: an engaged selection covering nothing.
    // This is the case that must still CLOSE and still show the dialog's own
    // sentence -- a neutral request is a legitimate thing to click OK on.
    OpenDocument empty = makeCallsiteDocument();
    empty.selection = Selection{};
    const PixelCommandOutcome nothing =
        runPixelCommand(empty, thresholdCommand(ThresholdParams{0.4f, 1.0f}), "SENTINEL");
    check(nothing.closeDialog && nothing.status == "SENTINEL",
          "outcome: a success that moved no texels closes and shows the dialog's own sentence");

    // The one behaviour step 2 knowingly changed: a parameter at its
    // documented identity is REFUSED by the command layer rather than run as
    // a no-op, so the dialog stays open with the reason. Pinned so that
    // changing it back is a decision somebody makes on purpose.
    OpenDocument identity = makeCallsiteDocument();
    const PixelCommandOutcome zero = runPixelCommand(identity, gaussianBlurCommand(0.0f), "S");
    check(!zero.closeDialog && zero.status.find("sigma") != std::string::npos,
          "outcome: an identity parameter is refused by name, not run and reported as a success");
  }

  return ok;
}

}  // namespace np
