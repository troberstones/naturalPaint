#include "ui/BrushPanelLayout.hpp"

#include <array>

namespace np {
namespace {

using P = BrushPanel;
using G = BrushPanelGroup;
using K = BrushRowKind;
using E = BrushRowEnable;

constexpr const char* kFlipNotPainted =
    "Flipping is saved with the brush but not applied to a stroke yet.";
constexpr const char* kDepthJitterNotPainted =
    "Depth Jitter and Minimum Depth are saved with the brush but not applied yet.";
constexpr const char* kOverrideNotPainted =
    "Saved with the tool preset but not applied: the file does not say how it combines with "
    "the brush's own Transfer.";

// Indexed by the enumerator; --selftest asserts every row carries its own id.
constexpr std::array<BrushPanelSpec, kBrushPanelCount> kPanels = {{
    {P::TipShape, "Brush Tip Shape", G::Photoshop, "", true, nullptr},
    {P::ShapeDynamics, "Shape Dynamics", G::Photoshop, "shape.enabled", true, nullptr},
    {P::Scattering, "Scattering", G::Photoshop, "scatter.enabled", true, nullptr},
    {P::Texture, "Texture", G::Photoshop, "texture.enabled", true, nullptr},
    {P::DualBrush, "Dual Brush", G::Photoshop, "dual.enabled", true, nullptr},
    {P::ColorDynamics, "Color Dynamics", G::Photoshop, "color.enabled", true,
     "Color Dynamics is saved with the brush but not applied to a stroke yet."},
    {P::Transfer, "Transfer", G::Photoshop, "transfer.enabled", true, nullptr},
    {P::BrushPose, "Brush Pose", G::Photoshop, "brushPose", false,
     "Brush Pose is saved with the brush but not applied: no preset measured uses it."},
    {P::Noise, "Noise", G::Photoshop, "noise", false,
     "Noise is saved with the brush but not applied: Photoshop publishes no formula for it."},
    {P::WetEdges, "Wet Edges", G::Photoshop, "wetEdges", false,
     "Wet Edges is saved with the brush but not applied yet."},
    {P::BuildUp, "Build-up", G::Photoshop, "airbrush", false, nullptr},
    {P::Smoothing, "Smoothing", G::Photoshop, "options.smoothing", false,
     "naturalPaint smooths with its own Stabiliser. This only chooses the Stabiliser's mode "
     "when the brush is imported."},
    {P::ProtectTexture, "Protect Texture", G::Photoshop, "texture.protectTexture", false,
     "naturalPaint always anchors a texture to the canvas, which is what Protect Texture does, "
     "so turning it off changes nothing yet."},
    {P::ToolOptions, "Tool Options", G::ToolPreset, "", true, nullptr},
    {P::Paint, "Load & Wetness", G::NaturalPaint, "", true, nullptr},
    {P::PaperGrain, "Paper Grain", G::NaturalPaint, "", true, nullptr},
    {P::TaperStabiliser, "Taper & Stabiliser", G::NaturalPaint, "", true, nullptr},
    {P::LinkMatrix, "Link Matrix", G::NaturalPaint, "", true, nullptr},
}};

}  // namespace

const BrushPanelSpec& brushPanelSpec(BrushPanel panel) noexcept {
  const size_t i = static_cast<size_t>(panel);
  return kPanels[i < kPanels.size() ? i : 0];
}

const char* brushPanelName(BrushPanel panel) noexcept {
  switch (panel) {
    case P::TipShape: return "TipShape";
    case P::ShapeDynamics: return "ShapeDynamics";
    case P::Scattering: return "Scattering";
    case P::Texture: return "Texture";
    case P::DualBrush: return "DualBrush";
    case P::ColorDynamics: return "ColorDynamics";
    case P::Transfer: return "Transfer";
    case P::BrushPose: return "BrushPose";
    case P::Noise: return "Noise";
    case P::WetEdges: return "WetEdges";
    case P::BuildUp: return "BuildUp";
    case P::Smoothing: return "Smoothing";
    case P::ProtectTexture: return "ProtectTexture";
    case P::ToolOptions: return "ToolOptions";
    case P::Paint: return "Paint";
    case P::PaperGrain: return "PaperGrain";
    case P::TaperStabiliser: return "TaperStabiliser";
    case P::LinkMatrix: return "LinkMatrix";
    case P::Count: break;
  }
  return "UNNAMED";
}

const std::vector<BrushRowSpec>& brushPanelRows() {
  static const std::vector<BrushRowSpec> rows = {
      // Brush Tip Shape: the tip grid, Size, Flip, Angle, Roundness, Hardness, Spacing.
      {.panel = P::TipShape, .kind = K::TipPicker, .path = "tip.dab.id", .label = "Tip"},
      {.panel = P::TipShape, .kind = K::Slider, .path = "tip.diameterPx", .label = "Size",
       .lo = 2.0f, .hi = 400.0f, .fmt = "%.0f px"},
      {.panel = P::TipShape, .kind = K::Check, .path = "tip.flipX", .label = "Flip X",
       .notPainted = kFlipNotPainted},
      {.panel = P::TipShape, .kind = K::Check, .path = "tip.flipY", .label = "Flip Y",
       .notPainted = kFlipNotPainted},
      {.panel = P::TipShape, .kind = K::Slider, .path = "tip.angleDeg", .label = "Angle",
       .lo = -180.0f, .hi = 180.0f, .fmt = "%.0f\xC2\xB0"},
      {.panel = P::TipShape, .kind = K::Slider, .path = "tip.roundness", .label = "Roundness",
       .scale = 100.0f, .lo = 1.0f},
      {.panel = P::TipShape, .kind = K::Slider, .path = "tip.hardness", .label = "Hardness",
       .scale = 100.0f, .enable = E::ProceduralTip},
      {.panel = P::TipShape, .kind = K::Slider, .path = "tip.spacingPercent", .label = "Spacing",
       .lo = 1.0f, .hi = 1000.0f},

      // Shape Dynamics.
      {.panel = P::ShapeDynamics, .kind = K::Jitter, .path = "shape.size", .label = "Size Jitter",
       .scale = 100.0f, .minimumLabel = "Minimum Diameter"},
      {.panel = P::ShapeDynamics, .kind = K::Slider, .path = "shape.tiltScale",
       .label = "Tilt Scale", .scale = 100.0f, .hi = 200.0f, .enable = E::ControlIsPenTilt,
       .enablePath = "shape.size",
       .notPainted = "Tilt Scale is saved with the brush but not applied yet."},
      {.panel = P::ShapeDynamics, .kind = K::Separator},
      {.panel = P::ShapeDynamics, .kind = K::Jitter, .path = "shape.angle",
       .label = "Angle Jitter", .scale = 100.0f, .controls = BrushControlSet::WithDirection},
      {.panel = P::ShapeDynamics, .kind = K::Separator},
      {.panel = P::ShapeDynamics, .kind = K::Jitter, .path = "shape.roundness",
       .label = "Roundness Jitter", .scale = 100.0f, .minimumLabel = "Minimum Roundness"},
      {.panel = P::ShapeDynamics, .kind = K::Separator},
      {.panel = P::ShapeDynamics, .kind = K::Check, .path = "shape.flipXJitter",
       .label = "Flip X Jitter", .notPainted = kFlipNotPainted},
      {.panel = P::ShapeDynamics, .kind = K::Check, .path = "shape.flipYJitter",
       .label = "Flip Y Jitter", .notPainted = kFlipNotPainted},
      {.panel = P::ShapeDynamics, .kind = K::Check, .path = "shape.brushProjection",
       .label = "Brush Projection",
       .notPainted = "Brush Projection is saved with the brush but not applied yet."},

      // Scattering.
      {.panel = P::Scattering, .kind = K::Jitter, .path = "scatter.scatter", .label = "Scatter",
       .scale = 100.0f, .hi = 1000.0f},
      {.panel = P::Scattering, .kind = K::Check, .path = "scatter.bothAxes", .label = "Both Axes"},
      {.panel = P::Scattering, .kind = K::Separator},
      {.panel = P::Scattering, .kind = K::IntSlider, .path = "scatter.count", .label = "Count",
       .lo = 1.0f, .hi = 16.0f},
      {.panel = P::Scattering, .kind = K::Jitter, .path = "scatter.countJitter",
       .label = "Count Jitter", .scale = 100.0f},

      // Texture.
      {.panel = P::Texture, .kind = K::Pattern, .path = "texture.pattern", .label = "Pattern"},
      {.panel = P::Texture, .kind = K::Check, .path = "texture.invert", .label = "Invert"},
      {.panel = P::Texture, .kind = K::Slider, .path = "texture.scalePercent", .label = "Scale",
       .lo = 1.0f, .hi = 1000.0f},
      {.panel = P::Texture, .kind = K::Slider, .path = "texture.brightness",
       .label = "Brightness", .lo = -150.0f, .hi = 150.0f, .fmt = "%.0f"},
      {.panel = P::Texture, .kind = K::Slider, .path = "texture.contrast", .label = "Contrast",
       .lo = -50.0f, .hi = 100.0f, .fmt = "%.0f"},
      {.panel = P::Texture, .kind = K::Separator},
      {.panel = P::Texture, .kind = K::Check, .path = "texture.eachTip",
       .label = "Texture Each Tip"},
      {.panel = P::Texture, .kind = K::Blend, .path = "texture.blend", .label = "Mode"},
      {.panel = P::Texture, .kind = K::Slider, .path = "texture.depth", .label = "Depth",
       .scale = 100.0f},
      {.panel = P::Texture, .kind = K::Slider, .path = "texture.minimumDepth",
       .label = "Minimum Depth", .scale = 100.0f, .enable = E::BoolOn,
       .enablePath = "texture.eachTip", .notPainted = kDepthJitterNotPainted},
      {.panel = P::Texture, .kind = K::Jitter, .path = "texture.depthJitter",
       .label = "Depth Jitter", .scale = 100.0f, .controls = BrushControlSet::NoRotation,
       .enable = E::BoolOn, .enablePath = "texture.eachTip",
       .notPainted = kDepthJitterNotPainted},

      // Dual Brush: Mode, Flip, the second tip, Size, Spacing, Scatter, Count.
      {.panel = P::DualBrush, .kind = K::Blend, .path = "dual.blend", .label = "Mode",
       .dualModes = true},
      {.panel = P::DualBrush, .kind = K::Check, .path = "dual.flip", .label = "Flip",
       .notPainted = kFlipNotPainted},
      {.panel = P::DualBrush, .kind = K::TipPicker, .path = "dual.tip.dab.id", .label = "Tip"},
      {.panel = P::DualBrush, .kind = K::Slider, .path = "dual.tip.diameterPx", .label = "Size",
       .lo = 2.0f, .hi = 400.0f, .fmt = "%.0f px"},
      {.panel = P::DualBrush, .kind = K::Slider, .path = "dual.tip.spacingPercent",
       .label = "Spacing", .lo = 1.0f, .hi = 1000.0f},
      {.panel = P::DualBrush, .kind = K::Separator},
      {.panel = P::DualBrush, .kind = K::Check, .path = "dual.scatter.enabled",
       .label = "Scatter Tips"},
      {.panel = P::DualBrush, .kind = K::Jitter, .path = "dual.scatter.scatter",
       .label = "Scatter", .scale = 100.0f, .hi = 1000.0f, .enable = E::BoolOn,
       .enablePath = "dual.scatter.enabled"},
      {.panel = P::DualBrush, .kind = K::Check, .path = "dual.scatter.bothAxes",
       .label = "Both Axes"},
      {.panel = P::DualBrush, .kind = K::IntSlider, .path = "dual.scatter.count",
       .label = "Count", .lo = 1.0f, .hi = 16.0f},

      // Color Dynamics.
      {.panel = P::ColorDynamics, .kind = K::Check, .path = "color.perTip",
       .label = "Apply Per Tip"},
      {.panel = P::ColorDynamics, .kind = K::Jitter, .path = "color.foregroundBackground",
       .label = "Foreground/Background Jitter", .scale = 100.0f},
      {.panel = P::ColorDynamics, .kind = K::Separator},
      {.panel = P::ColorDynamics, .kind = K::Slider, .path = "color.hueJitter",
       .label = "Hue Jitter"},
      {.panel = P::ColorDynamics, .kind = K::Slider, .path = "color.saturationJitter",
       .label = "Saturation Jitter"},
      {.panel = P::ColorDynamics, .kind = K::Slider, .path = "color.brightnessJitter",
       .label = "Brightness Jitter"},
      {.panel = P::ColorDynamics, .kind = K::Slider, .path = "color.purity", .label = "Purity",
       .lo = -100.0f},

      // Transfer.
      {.panel = P::Transfer, .kind = K::Jitter, .path = "transfer.opacity",
       .label = "Opacity Jitter", .scale = 100.0f, .minimumLabel = "Minimum"},
      {.panel = P::Transfer, .kind = K::Separator},
      {.panel = P::Transfer, .kind = K::Jitter, .path = "transfer.flow", .label = "Flow Jitter",
       .scale = 100.0f, .minimumLabel = "Minimum"},

      // Tool Options.
      {.panel = P::ToolOptions, .kind = K::ToolBlend, .path = "options.blendMode",
       .label = "Mode"},
      {.panel = P::ToolOptions, .kind = K::Slider, .path = "options.opacity", .label = "Opacity",
       .scale = 100.0f,
       .notPainted = "Saved with the tool preset but not applied: a stroke reads the options "
                     "bar's Opacity."},
      {.panel = P::ToolOptions, .kind = K::Slider, .path = "options.flow", .label = "Flow",
       .scale = 100.0f},
      {.panel = P::ToolOptions, .kind = K::Check, .path = "options.pressureOverridesSize",
       .label = "Pressure Controls Size", .notPainted = kOverrideNotPainted},
      {.panel = P::ToolOptions, .kind = K::Check, .path = "options.pressureOverridesOpacity",
       .label = "Pressure Controls Opacity", .notPainted = kOverrideNotPainted},
      {.panel = P::ToolOptions, .kind = K::Check, .path = "options.useLegacy",
       .label = "Use Legacy",
       .notPainted = "Use Legacy is saved with the tool preset but not applied."},
  };
  return rows;
}

const std::vector<BrushFieldOmission>& brushFieldOmissionTable() {
  constexpr const char* kOverride =
      "tool preset override, never applied -- PsToolOptions's own comment";
  constexpr const char* kNoEngineTarget = "no engine target -- PsTransfer's own comment";
  constexpr const char* kPresent =
      "internal bookkeeping -- Variance.hpp's own comment on `present`";
  constexpr const char* kComputed =
      "derived classification, not independently settable -- PsTipShape's own comment";
  constexpr const char* kNoMinimum =
      "Photoshop's panel has no Minimum for this setting, so there is nothing to set it with";
  constexpr const char* kDualTipShape =
      "Photoshop's Dual Brush panel has no control for this; the second tip keeps the shape its "
      "preset was saved with";
  constexpr const char* kDualCountJitter =
      "Photoshop's Dual Brush panel has no Count Jitter, and the second tip's cadence does not "
      "read one";
  static const std::vector<BrushFieldOmission> table = {
      {"options.sizeOverride.control", kOverride},
      {"options.sizeOverride.jitter", kOverride},
      {"options.sizeOverride.minimum", kOverride},
      {"options.sizeOverride.fadeSteps", kOverride},
      {"options.sizeOverride.present", kOverride},
      {"options.opacityOverride.control", kOverride},
      {"options.opacityOverride.jitter", kOverride},
      {"options.opacityOverride.minimum", kOverride},
      {"options.opacityOverride.fadeSteps", kOverride},
      {"options.opacityOverride.present", kOverride},
      {"options.flowOverride.control", kOverride},
      {"options.flowOverride.jitter", kOverride},
      {"options.flowOverride.minimum", kOverride},
      {"options.flowOverride.fadeSteps", kOverride},
      {"options.flowOverride.present", kOverride},
      {"options.colorOverride.control", kOverride},
      {"options.colorOverride.jitter", kOverride},
      {"options.colorOverride.minimum", kOverride},
      {"options.colorOverride.fadeSteps", kOverride},
      {"options.colorOverride.present", kOverride},

      {"transfer.wetness.control", kNoEngineTarget},
      {"transfer.wetness.jitter", kNoEngineTarget},
      {"transfer.wetness.minimum", kNoEngineTarget},
      {"transfer.wetness.fadeSteps", kNoEngineTarget},
      {"transfer.wetness.present", kNoEngineTarget},
      {"transfer.mix.control", kNoEngineTarget},
      {"transfer.mix.jitter", kNoEngineTarget},
      {"transfer.mix.minimum", kNoEngineTarget},
      {"transfer.mix.fadeSteps", kNoEngineTarget},
      {"transfer.mix.present", kNoEngineTarget},

      {"shape.size.present", kPresent},
      {"shape.angle.present", kPresent},
      {"shape.roundness.present", kPresent},
      {"scatter.scatter.present", kPresent},
      {"scatter.countJitter.present", kPresent},
      {"texture.depthJitter.present", kPresent},
      {"dual.scatter.scatter.present", kPresent},
      {"dual.scatter.countJitter.present", kPresent},
      {"color.foregroundBackground.present", kPresent},
      {"transfer.opacity.present", kPresent},
      {"transfer.flow.present", kPresent},

      {"tip.computed", kComputed},
      {"dual.tip.computed", kComputed},

      // True on all 101 presets measured, and naturalPaint always spaces dabs.
      {"tip.spacingEnabled",
       "on in every preset measured, and naturalPaint always spaces dabs by Spacing"},

      {"shape.angle.minimum", kNoMinimum},
      {"scatter.scatter.minimum", kNoMinimum},
      {"scatter.countJitter.minimum", kNoMinimum},
      {"dual.scatter.scatter.minimum", kNoMinimum},
      {"color.foregroundBackground.minimum", kNoMinimum},
      {"texture.depthJitter.minimum",
       "Photoshop's Minimum Depth is its own key, `texture.minimumDepth`, which has the control"},

      {"dual.tip.angleDeg", kDualTipShape},
      {"dual.tip.roundness", kDualTipShape},
      {"dual.tip.hardness", kDualTipShape},
      {"dual.tip.spacingEnabled", kDualTipShape},
      {"dual.tip.flipX", kDualTipShape},
      {"dual.tip.flipY", kDualTipShape},
      {"dual.scatter.countJitter.control", kDualCountJitter},
      {"dual.scatter.countJitter.jitter", kDualCountJitter},
      {"dual.scatter.countJitter.minimum", kDualCountJitter},
      {"dual.scatter.countJitter.fadeSteps", kDualCountJitter},
  };
  return table;
}

std::vector<std::string> brushRowLeafPaths(const BrushRowSpec& row) {
  const std::string path = row.path;
  switch (row.kind) {
    case K::Separator: return {};
    case K::Jitter: {
      std::vector<std::string> leaves = {path + ".control", path + ".jitter", path + ".fadeSteps"};
      if (row.minimumLabel != nullptr) leaves.push_back(path + ".minimum");
      return leaves;
    }
    case K::Pattern: return {path + ".id", path + ".name"};
    case K::Check:
    case K::Slider:
    case K::IntSlider:
    case K::Blend:
    case K::TipPicker:
    case K::ToolBlend: return {path};
  }
  return {};
}

const std::vector<VarianceControl>& brushControlChoices(BrushControlSet set) {
  using C = VarianceControl;
  static const std::vector<C> standard = {C::Off, C::Fade, C::PenPressure, C::PenTilt,
                                          C::StylusWheel, C::Rotation};
  static const std::vector<C> withDirection = {C::Off,         C::Fade,     C::PenPressure,
                                               C::PenTilt,     C::StylusWheel, C::Rotation,
                                               C::InitialDirection, C::Direction};
  static const std::vector<C> noRotation = {C::Off, C::Fade, C::PenPressure, C::PenTilt,
                                            C::StylusWheel};
  switch (set) {
    case BrushControlSet::Standard: return standard;
    case BrushControlSet::WithDirection: return withDirection;
    case BrushControlSet::NoRotation: return noRotation;
  }
  return standard;
}

const std::vector<CoverageBlend>& brushBlendChoices(bool dualModes) {
  using B = CoverageBlend;
  static const std::vector<B> texture = {B::Multiply,  B::Subtract,   B::Darken,
                                         B::Overlay,   B::ColorDodge, B::ColorBurn,
                                         B::LinearBurn, B::HardMix,  B::LinearHeight,
                                         B::Height};
  static const std::vector<B> dual = {B::Multiply,   B::Darken,  B::Overlay,
                                      B::ColorDodge, B::ColorBurn, B::LinearBurn,
                                      B::HardMix,    B::LinearHeight};
  return dualModes ? dual : texture;
}

const std::vector<BrushToolBlendChoice>& brushToolBlendChoices() {
  // The five ids the four packs measured use, in Photoshop's menu order.
  static const std::vector<BrushToolBlendChoice> choices = {
      {"Nrml", "Normal"},   {"Dslv", "Dissolve"},        {"Drkn", "Darken"},
      {"Mltp", "Multiply"}, {"linearBurn", "Linear Burn"},
  };
  return choices;
}

bool brushRowLiveWhilePanelOff(const BrushRowSpec& row) noexcept {
  return row.kind == K::Pattern;
}

float brushRowShown(float stored, float scale) noexcept { return stored * scale; }

bool brushRowStore(float shown, float scale, float& stored) noexcept {
  if (shown == brushRowShown(stored, scale) || scale == 0.0f) return false;
  stored = shown / scale;
  return true;
}

}  // namespace np
