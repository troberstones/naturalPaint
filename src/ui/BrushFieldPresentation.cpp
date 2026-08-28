#include "ui/BrushFieldPresentation.hpp"

#include <unordered_map>

namespace np {
namespace {

// Diameter range mirrors `kBrushRadiusMin`/`kBrushRadiusMax`
// (app/AppState.hpp), doubled: those two bound a RADIUS (the Tip Shape tab's
// own hand-written "Radius" slider), and `diameterPx` is twice that.
constexpr float kDiameterMin = 2.0f;
constexpr float kDiameterMax = 400.0f;

}  // namespace

const std::vector<BrushFieldSpec>& brushFieldPresentationTable() {
  static const std::vector<BrushFieldSpec> table = {
      // ---------------------------------------------------------------------
      // tip.* -- Tip Shape tab. diameterPx/angleDeg/roundness/spacingPercent/
      // hardness are ALSO here (for exhaustiveness), even though
      // drawBrushTipShapeGroup() draws them by hand with its own unit
      // conversions (radius-from-diameter, spacing-in-radii) and skips them
      // in its generic walk -- see that function's own comment.
      // ---------------------------------------------------------------------
      {"tip.dab.id", "Tip Bitmap Id", "%.3f", 0, 0, 0, 0, /*readOnly=*/true},
      {"tip.diameterPx", "Diameter", "%.0f px", kDiameterMin, kDiameterMax},
      {"tip.angleDeg", "Angle", "%.0f deg", -180.0f, 180.0f},
      {"tip.roundness", "Roundness", "%.2f", 0.05f, 1.0f},
      {"tip.spacingPercent", "Spacing", "%.0f%%", 1.0f, 1000.0f},
      {"tip.hardness", "Hardness", "%.2f", 0.0f, 1.0f},
      {"tip.spacingEnabled", "Spacing Enabled"},
      {"tip.flipX", "Flip X"},
      {"tip.flipY", "Flip Y"},

      // ---------------------------------------------------------------------
      // shape.* -- Shape Dynamics tab.
      // ---------------------------------------------------------------------
      {"shape.enabled", "Enabled"},
      {"shape.size.control", "Size Control"},
      {"shape.size.jitter", "Size Jitter", "%.2f", 0.0f, 1.0f},
      {"shape.size.minimum", "Size Minimum", "%.2f", 0.0f, 1.0f},
      {"shape.size.fadeSteps", "Size Fade Steps", "%d", 0, 0, 1, 9999},
      {"shape.angle.control", "Angle Control"},
      {"shape.angle.jitter", "Angle Jitter", "%.2f", 0.0f, 1.0f},
      {"shape.angle.minimum", "Angle Minimum", "%.2f", 0.0f, 1.0f},
      {"shape.angle.fadeSteps", "Angle Fade Steps", "%d", 0, 0, 1, 9999},
      {"shape.roundness.control", "Roundness Control"},
      {"shape.roundness.jitter", "Roundness Jitter", "%.2f", 0.0f, 1.0f},
      {"shape.roundness.minimum", "Roundness Minimum", "%.2f", 0.0f, 1.0f},
      {"shape.roundness.fadeSteps", "Roundness Fade Steps", "%d", 0, 0, 1, 9999},
      {"shape.flipXJitter", "Flip X Jitter"},
      {"shape.flipYJitter", "Flip Y Jitter"},
      {"shape.brushProjection", "Brush Projection"},
      {"shape.tiltScale", "Tilt Scale", "%.2f", 0.0f, 3.0f},

      // ---------------------------------------------------------------------
      // scatter.* -- Scattering tab.
      // ---------------------------------------------------------------------
      {"scatter.enabled", "Enabled"},
      {"scatter.scatter.control", "Scatter Control"},
      {"scatter.scatter.jitter", "Scatter Jitter", "%.2f", 0.0f, 1.0f},
      {"scatter.scatter.minimum", "Scatter Minimum", "%.2f", 0.0f, 1.0f},
      {"scatter.scatter.fadeSteps", "Scatter Fade Steps", "%d", 0, 0, 1, 9999},
      {"scatter.bothAxes", "Both Axes"},
      {"scatter.count", "Count", "%d", 0, 0, 1, 16},
      {"scatter.countJitter.control", "Count Control"},
      {"scatter.countJitter.jitter", "Count Jitter", "%.2f", 0.0f, 1.0f},
      {"scatter.countJitter.minimum", "Count Minimum", "%.2f", 0.0f, 1.0f},
      {"scatter.countJitter.fadeSteps", "Count Fade Steps", "%d", 0, 0, 1, 9999},

      // ---------------------------------------------------------------------
      // texture.* -- Texture tab, appended after the existing PAPER GRAIN
      // section (`st.brush.grain`, a different and already-wired mechanism --
      // see drawBrushTextureGroup()'s own comment on the two).
      // ---------------------------------------------------------------------
      {"texture.enabled", "Enabled"},
      {"texture.pattern.id", "Pattern Id", "%.3f", 0, 0, 0, 0, /*readOnly=*/true},
      {"texture.pattern.name", "Pattern Name", "%.3f", 0, 0, 0, 0, /*readOnly=*/true},
      {"texture.invert", "Invert"},
      {"texture.scalePercent", "Scale", "%.0f%%", 1.0f, 1000.0f},
      {"texture.depth", "Depth", "%.2f", 0.0f, 2.0f},
      {"texture.minimumDepth", "Minimum Depth", "%.2f", 0.0f, 1.0f},
      {"texture.depthJitter.control", "Depth Control"},
      {"texture.depthJitter.jitter", "Depth Jitter", "%.2f", 0.0f, 1.0f},
      {"texture.depthJitter.minimum", "Depth Jitter Minimum", "%.2f", 0.0f, 1.0f},
      {"texture.depthJitter.fadeSteps", "Depth Fade Steps", "%d", 0, 0, 1, 9999},
      {"texture.blend", "Blend Mode"},
      {"texture.brightness", "Brightness", "%.0f", -150.0f, 150.0f},
      {"texture.contrast", "Contrast", "%.0f", -50.0f, 100.0f},
      {"texture.eachTip", "Each Tip"},
      {"texture.protectTexture", "Protect Texture"},

      // ---------------------------------------------------------------------
      // dual.* -- Dual Brush tab. Entirely new: no hand-written body existed
      // for this panel before (naturalPaint-ui-design-gaps' own finding --
      // "8 of 12 Runny Inkers stamp a second tip we have no way to render").
      // ---------------------------------------------------------------------
      {"dual.enabled", "Enabled"},
      {"dual.tip.dab.id", "Second Tip Bitmap Id", "%.3f", 0, 0, 0, 0, /*readOnly=*/true},
      {"dual.tip.diameterPx", "Second Tip Diameter", "%.0f px", kDiameterMin, kDiameterMax},
      {"dual.tip.angleDeg", "Second Tip Angle", "%.0f deg", -180.0f, 180.0f},
      {"dual.tip.roundness", "Second Tip Roundness", "%.2f", 0.05f, 1.0f},
      {"dual.tip.spacingPercent", "Second Tip Spacing", "%.0f%%", 1.0f, 1000.0f},
      {"dual.tip.hardness", "Second Tip Hardness", "%.2f", 0.0f, 1.0f},
      {"dual.tip.spacingEnabled", "Second Tip Spacing Enabled"},
      {"dual.tip.flipX", "Second Tip Flip X"},
      {"dual.tip.flipY", "Second Tip Flip Y"},
      {"dual.blend", "Blend Mode"},
      {"dual.scatter.enabled", "Second Tip Scattering Enabled"},
      {"dual.scatter.scatter.control", "Second Tip Scatter Control"},
      {"dual.scatter.scatter.jitter", "Second Tip Scatter Jitter", "%.2f", 0.0f, 1.0f},
      {"dual.scatter.scatter.minimum", "Second Tip Scatter Minimum", "%.2f", 0.0f, 1.0f},
      {"dual.scatter.scatter.fadeSteps", "Second Tip Scatter Fade Steps", "%d", 0, 0, 1, 9999},
      {"dual.scatter.bothAxes", "Second Tip Both Axes"},
      {"dual.scatter.count", "Second Tip Count", "%d", 0, 0, 1, 16},
      {"dual.scatter.countJitter.control", "Second Tip Count Control"},
      {"dual.scatter.countJitter.jitter", "Second Tip Count Jitter", "%.2f", 0.0f, 1.0f},
      {"dual.scatter.countJitter.minimum", "Second Tip Count Minimum", "%.2f", 0.0f, 1.0f},
      {"dual.scatter.countJitter.fadeSteps", "Second Tip Count Fade Steps", "%d", 0, 0, 1,
       9999},
      {"dual.flip", "Flip"},

      // ---------------------------------------------------------------------
      // color.* -- Color Dynamics tab. Shown and editable throughout (the
      // panel persists to the model and to the saved preset), but the tab
      // carries a standing banner: measured 1 of 101 presets uses it at all,
      // and PsColorDynamics's own comment is that how it composes with the
      // foreground is not settled, so StrokeSession::brushTipFor() passes the
      // HSV identity regardless of what is set here (its own comment, "future
      // work, not this commit's"). That is a permanent fact about the ENGINE,
      // independent of this tab's own `enabled` gate, so it is a caption, not
      // a BeginDisabled() -- see drawBrushColorDynamicsGroup().
      // ---------------------------------------------------------------------
      {"color.enabled", "Enabled"},
      {"color.perTip", "Per Tip"},
      {"color.foregroundBackground.control", "Foreground/Background Control"},
      {"color.foregroundBackground.jitter", "Foreground/Background Jitter", "%.2f", 0.0f, 1.0f},
      {"color.foregroundBackground.minimum", "Foreground/Background Minimum", "%.2f", 0.0f,
       1.0f},
      {"color.foregroundBackground.fadeSteps", "Foreground/Background Fade Steps", "%d", 0, 0, 1,
       9999},
      {"color.hueJitter", "Hue Jitter", "%.0f%%", 0.0f, 100.0f},
      {"color.saturationJitter", "Saturation Jitter", "%.0f%%", 0.0f, 100.0f},
      {"color.brightnessJitter", "Brightness Jitter", "%.0f%%", 0.0f, 100.0f},
      {"color.purity", "Purity", "%.0f%%", -100.0f, 100.0f},

      // ---------------------------------------------------------------------
      // transfer.* -- Transfer tab. Only opacity/flow: wetness/mix are on the
      // omission list (brushFieldOmissionTable()).
      // ---------------------------------------------------------------------
      {"transfer.enabled", "Enabled"},
      {"transfer.opacity.control", "Opacity Control"},
      {"transfer.opacity.jitter", "Opacity Jitter", "%.2f", 0.0f, 1.0f},
      {"transfer.opacity.minimum", "Opacity Minimum", "%.2f", 0.0f, 1.0f},
      {"transfer.opacity.fadeSteps", "Opacity Fade Steps", "%d", 0, 0, 1, 9999},
      {"transfer.flow.control", "Flow Control"},
      {"transfer.flow.jitter", "Flow Jitter", "%.2f", 0.0f, 1.0f},
      {"transfer.flow.minimum", "Flow Minimum", "%.2f", 0.0f, 1.0f},
      {"transfer.flow.fadeSteps", "Flow Fade Steps", "%d", 0, 0, 1, 9999},

      // ---------------------------------------------------------------------
      // options.* -- Tool Options tab. The four override Variances are on
      // the omission list. `noise`/`wetEdges`/`airbrush`/`brushPose` (the
      // bare top-level checkbox tail, no `options.` prefix at all) are ALSO
      // drawn on this tab -- see drawBrushToolOptionsGroup()'s own comment on
      // why this tab is where they landed.
      // ---------------------------------------------------------------------
      {"options.blendMode", "Blend Mode Id"},
      {"options.opacity", "Opacity", "%.2f", 0.0f, 1.0f},
      {"options.flow", "Flow", "%.2f", 0.0f, 1.0f},
      {"options.smoothing", "Smoothing"},
      {"options.pressureOverridesSize", "Pressure Overrides Size"},
      {"options.pressureOverridesOpacity", "Pressure Overrides Opacity"},
      {"options.useLegacy", "Use Legacy"},
      {"noise", "Noise"},
      {"wetEdges", "Wet Edges"},
      {"airbrush", "Airbrush (Build-up)"},
      {"brushPose", "Brush Pose"},
  };
  return table;
}

const std::vector<BrushFieldOmission>& brushFieldOmissionTable() {
  static const std::vector<BrushFieldOmission> table = {
      // PsToolOptions's own comment: parsed and carried, never applied -- how
      // a tool preset's own override composes with the brush's own dynamics
      // is not determinable from the file.
      {"options.sizeOverride.control", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.sizeOverride.jitter", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.sizeOverride.minimum", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.sizeOverride.fadeSteps", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.sizeOverride.present", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.opacityOverride.control", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.opacityOverride.jitter", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.opacityOverride.minimum", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.opacityOverride.fadeSteps", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.opacityOverride.present", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.flowOverride.control", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.flowOverride.jitter", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.flowOverride.minimum", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.flowOverride.fadeSteps", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.flowOverride.present", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.colorOverride.control", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.colorOverride.jitter", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.colorOverride.minimum", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.colorOverride.fadeSteps", "tool preset override, never applied -- PsToolOptions's own comment"},
      {"options.colorOverride.present", "tool preset override, never applied -- PsToolOptions's own comment"},

      // PsTransfer's own comment: no engine target for either -- a Pigment
      // texel's seven channels do not include a water value.
      {"transfer.wetness.control", "no engine target -- PsTransfer's own comment"},
      {"transfer.wetness.jitter", "no engine target -- PsTransfer's own comment"},
      {"transfer.wetness.minimum", "no engine target -- PsTransfer's own comment"},
      {"transfer.wetness.fadeSteps", "no engine target -- PsTransfer's own comment"},
      {"transfer.wetness.present", "no engine target -- PsTransfer's own comment"},
      {"transfer.mix.control", "no engine target -- PsTransfer's own comment"},
      {"transfer.mix.jitter", "no engine target -- PsTransfer's own comment"},
      {"transfer.mix.minimum", "no engine target -- PsTransfer's own comment"},
      {"transfer.mix.fadeSteps", "no engine target -- PsTransfer's own comment"},
      {"transfer.mix.present", "no engine target -- PsTransfer's own comment"},

      // Variance.hpp's own comment: whether the file said "Off" or said
      // nothing at all -- internal bookkeeping, not a fact a painter sets
      // directly. Only for the 11 Variances this build otherwise shows; the
      // six Variances omitted whole (above) already cover their own
      // `.present` leaf under their own reason.
      {"shape.size.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"shape.angle.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"shape.roundness.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"scatter.scatter.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"scatter.countJitter.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"texture.depthJitter.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"dual.scatter.scatter.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"dual.scatter.countJitter.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"color.foregroundBackground.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"transfer.opacity.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},
      {"transfer.flow.present", "internal bookkeeping -- Variance.hpp's own comment on `present`"},

      // PsTipShape's own comment: which Photoshop classID the file used
      // (computedBrush vs sampledBrush). Hand-flipping it while dab.id/
      // dab.bitmap stay whatever they were would produce a model that
      // contradicts itself, and there is no control here that also
      // retargets the tip to match.
      {"tip.computed", "derived classification, not independently settable -- PsTipShape's own comment"},
      {"dual.tip.computed", "derived classification, not independently settable -- PsTipShape's own comment"},

      // AppState.hpp's own comment on BrushState::load/wetness: a deferred
      // divergence. The stroke reads BrushState::load/wetness (this
      // window's Paint tab), never BrushModel::load/wetness -- a second
      // live slider here would look identical and silently not be the same
      // number.
      {"load", "shadowed by the Paint tab's own Load slider -- AppState.hpp's own comment"},
      {"wetness", "shadowed by the Paint tab's own Water slider -- AppState.hpp's own comment"},
  };
  return table;
}

const BrushFieldSpec* findBrushFieldSpec(const std::string& path) noexcept {
  static const std::unordered_map<std::string, const BrushFieldSpec*> index = [] {
    std::unordered_map<std::string, const BrushFieldSpec*> m;
    for (const BrushFieldSpec& spec : brushFieldPresentationTable()) m[spec.path] = &spec;
    return m;
  }();
  const auto it = index.find(path);
  return it == index.end() ? nullptr : it->second;
}

const char* findBrushFieldOmissionReason(const std::string& path) noexcept {
  static const std::unordered_map<std::string, const char*> index = [] {
    std::unordered_map<std::string, const char*> m;
    for (const BrushFieldOmission& row : brushFieldOmissionTable()) m[row.path] = row.reason;
    return m;
  }();
  const auto it = index.find(path);
  return it == index.end() ? nullptr : it->second;
}

}  // namespace np
