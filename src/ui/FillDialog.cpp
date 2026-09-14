#include "ui/FillDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/CommandsFill.hpp"
#include "app/FilterOps.hpp"
#include "core/Blend.hpp"
#include "core/SelectionMask.hpp"
#include "ops/Fill.hpp"
#include "ops/Pattern.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/FillDialog.hpp for the shape this follows and why it is a separate
// translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_fillRequested = false;
bool g_strokeRequested = false;
bool g_definePatternRequested = false;

// The one door every Filter/Adjustments dialog commits through
// (docs/automation.md §2.3) is `ui/MacPaintUI.cpp`'s file-scope
// `pixelOpFooter()`, which is not declared in any header -- nothing outside
// that file has called it before this track. Rather than widen
// `ui/MacPaintUI.hpp`'s surface for one more `#include`, this is its body,
// copied rather than shared: `dialogStatusLine()` + `DialogFooter` +
// `runPixelCommand()`, the three pieces that ARE exported
// (`ui/Dialog.hpp`, `ui/MacPaintUI.hpp`). `define_pattern` changes no pixel
// at all, so this footer's name says what it actually is -- "the command
// footer", not "the pixel-op footer" -- while doing the identical thing.
//
// **One deliberate omission against the original**: `pixelOpFooter()` hands a
// closing success's sentence to `g_docStatus` so it is still readable after
// the popup closes. That variable has no extern declaration anywhere and this
// track cannot tell, from outside `ui/MacPaintUI.cpp`, which anonymous
// namespace it sits in -- guessing wrong is a link error a build catches
// immediately, but widening that file's surface to find out is exactly the
// "small hooks only" line this header's own comment draws. The sentence is
// shown in the dialog itself while it is open (via `status` below) and is
// lost on a successful close instead of surviving into the chrome's message
// band; a real but narrow gap, not a silent one.
void fillDialogFooter(OpenDocument* od, std::string& status, const Command& command,
                      const char* nothingChangedText, bool commitEnabled = true) {
  dialogStatusLine(DialogStatus::Error, status);
  DialogFooter footer;
  footer.commit = "Apply";
  footer.commitEnabled = commitEnabled && od != nullptr;
  switch (dialogFooter(footer)) {
    case DialogAction::Commit: {
      const PixelCommandOutcome out = runPixelCommand(*od, command, nothingChangedText);
      if (!out.closeDialog) {
        status = out.status;
        break;
      }
      status.clear();
      ImGui::CloseCurrentPopup();
      break;
    }
    case DialogAction::Cancel:
      status.clear();
      ImGui::CloseCurrentPopup();
      break;
    default:
      break;
  }
}

// The canvas preview, recomputed once an edit has settled: the inputs moved
// and no control is mid-drag or mid-typing. A full-layer fill or stroke costs
// hundreds of milliseconds at the default document size, so not per tick.
// Named on the External door so the unnamed dialogs' every-frame clear, drawn
// before these, leaves it alone.
void updateFillPreview(const char* owner, const OpenDocument* od, const Command& command,
                       bool enabled, std::string& key) {
  if (od == nullptr || !enabled) {
    clearExternalFilterPreview(owner);
    key.clear();
    return;
  }
  if (ImGui::IsAnyItemActive()) return;
  std::string next = std::to_string(od->id) + '/' + std::to_string(od->revision) + '/' +
                     std::to_string(od->selectionRevision) + '/' +
                     std::to_string(od->activeLayer) + '/' + command.params.write(0);
  if (next == key) return;
  key = std::move(next);
  TileStore tiles;
  size_t changed = 0;
  if (previewFillCommand(*od, command, &tiles, &changed).empty() && changed > 0) {
    if (const std::optional<size_t> idx = activeLayerIndex(*od)) {
      setExternalFilterPreview(od->id, *idx, std::move(tiles), owner);
      return;
    }
  }
  clearExternalFilterPreview(owner);
}

// ==========================================================================
// The shared "what am I painting with" block -- PRD D26's Foreground
// colour | Colour | Pattern | Gradient, a blend mode and an opacity. Fill and
// Stroke both draw and resolve this identically; only Stroke adds a width and
// a location beyond it.
// ==========================================================================

struct FillSourceUi {
  int source = 0;  // 0 Foreground, 1 Colour, 2 Pattern, 3 Gradient
  float color[3] = {0.0f, 0.0f, 0.0f};
  int patternIndex = 0;
  // Degrees, clockwise on screen (document space is y-down -- `core/Gradient
  // .hpp`'s own Angular-kind comment). 0 runs left to right.
  float gradientAngleDeg = 0.0f;
  int blendIndex = 0;  // index into blendModesForFillDialog(), below
  float opacity = 1.0f;
};

// `BlendMode::Mix` excluded: `ops/Fill.hpp`'s `fillParamsValid()` refuses it
// outright (it is a Kubelka-Munk lerp between two Pigment layers' latents,
// and a fill's source has none), so offering it in this combo would be a
// control that is always a refusal away from doing anything -- `docs/ui.md`'s
// rule that no dead control looks live, applied to a value rather than to a
// whole item.
std::vector<BlendMode> blendModesForFillDialog() {
  std::vector<BlendMode> modes;
  for (const BlendModeInfo& info : allBlendModes())
    if (info.mode != BlendMode::Mix) modes.push_back(info.mode);
  return modes;
}

// The rectangle a dialog-driven gradient spans: the selection's bounds, or
// the layer's canvas when there is none -- reach-fill.md's own words. Unlike
// the on-canvas Gradient tool, a modal has no drag to aim the ramp with, so
// this dialog adds an Angle control and always spans the whole rectangle
// corner to corner, at that angle -- a design decision this track made
// because the brief describes the CONTENT (stops, kind, angle) but not how a
// dialog with no drag gesture picks two endpoints from it.
PixelRect fillDialogGradientBounds(const OpenDocument& od) {
  if (od.selection.has_value()) {
    if (const std::optional<SelectionBounds> b = selectionBounds(*od.selection))
      return PixelRect{b->x0, b->y0, b->x1, b->y1};
  }
  return PixelRect{0, 0, static_cast<int32_t>(od.document.width),
                   static_cast<int32_t>(od.document.height)};
}

void drawFillSourceControls(FillSourceUi& ui) {
  static const char* kSourceItems[] = {"Foreground Colour", "Colour", "Pattern", "Gradient"};
  dialogRadioRow("Contents", &ui.source, kSourceItems, 4);

  if (ui.source == 1) {
    dialogColor("Colour", ui.color);
  } else if (ui.source == 2) {
    const size_t count = sessionPatterns().size();
    if (count == 0) {
      dialogHint("No patterns are defined yet -- Edit > Define Pattern... first.");
    } else {
      std::vector<std::string> names;
      names.reserve(count);
      for (size_t i = 0; i < count; ++i) names.push_back(sessionPatterns().at(i).name);
      std::vector<const char*> items;
      items.reserve(count);
      for (const std::string& n : names) items.push_back(n.c_str());
      ui.patternIndex = std::clamp(ui.patternIndex, 0, static_cast<int>(count) - 1);
      dialogCombo("Pattern", &ui.patternIndex, items.data(), static_cast<int>(count));
    }
  } else if (ui.source == 3) {
    dialogSlider("Angle", &ui.gradientAngleDeg, 0.0f, 360.0f, "%.0f", "deg");
    dialogHint(
        "Uses the gradient tool's current ramp, kind and spread from the options bar -- open "
        "the tool's stop editor there to change the colours.");
  }

  const std::vector<BlendMode> blendModes = blendModesForFillDialog();
  std::vector<const char*> blendLabels;
  blendLabels.reserve(blendModes.size());
  for (BlendMode m : blendModes) blendLabels.push_back(blendModeInfo(m).label);
  ui.blendIndex = std::clamp(ui.blendIndex, 0, static_cast<int>(blendModes.size()) - 1);
  dialogCombo("Blend", &ui.blendIndex, blendLabels.data(), static_cast<int>(blendLabels.size()));
  dialogSlider("Opacity", &ui.opacity, 0.0f, 1.0f, "%.2f");
}

// Turns the controls above into `ops/Fill::FillParams`. Empty return is
// success; otherwise the sentence to show instead of running anything --
// worded for the person looking at the dialog, which is why it is not always
// the same sentence `fill`/`stroke`'s own command-layer refusal would give
// (that one is worded for a `.npaction` file's reader).
std::string resolveFillParams(const FillSourceUi& ui, AppState& st, OpenDocument* od,
                              FillParams* out) {
  const std::vector<BlendMode> blendModes = blendModesForFillDialog();
  out->blend = blendModes[std::clamp(ui.blendIndex, 0, static_cast<int>(blendModes.size()) - 1)];
  out->opacity = ui.opacity;

  if (ui.source == 0 || ui.source == 1) {
    out->source = FillSource::Color;
    if (ui.source == 0) {
      const std::array<float, 4> fg = foregroundLinearRgba(st.brush);
      out->color = {fg[0], fg[1], fg[2]};
    } else {
      out->color = {ui.color[0], ui.color[1], ui.color[2]};
    }
    return {};
  }
  if (ui.source == 2) {
    out->source = FillSource::Pattern;
    if (sessionPatterns().size() == 0)
      return "No patterns are defined yet -- Edit > Define Pattern... first.";
    const size_t idx = static_cast<size_t>(
        std::clamp(ui.patternIndex, 0, static_cast<int>(sessionPatterns().size()) - 1));
    out->pattern = &sessionPatterns().at(idx);
    return {};
  }
  // Gradient.
  if (od == nullptr) return "No document is open.";
  out->source = FillSource::Gradient;
  out->gradientStops = currentGradientStops(st.brush, st.gradient);
  out->gradientGeometry.kind = st.gradient.kind;
  out->gradientGeometry.spread = st.gradient.spread;
  const PixelRect r = fillDialogGradientBounds(*od);
  const float cx = 0.5f * static_cast<float>(r.x0 + r.x1);
  const float cy = 0.5f * static_cast<float>(r.y0 + r.y1);
  const float w = static_cast<float>(r.width());
  const float h = static_cast<float>(r.height());
  const float halfDiag = 0.5f * std::sqrt(w * w + h * h);
  const float rad = ui.gradientAngleDeg * (3.14159265358979f / 180.0f);
  const float dx = std::cos(rad), dy = std::sin(rad);
  if (out->gradientGeometry.kind == GradientKind::Radial) {
    out->gradientGeometry.x0 = cx;
    out->gradientGeometry.y0 = cy;
    out->gradientGeometry.x1 = cx + dx * halfDiag;
    out->gradientGeometry.y1 = cy + dy * halfDiag;
  } else {
    out->gradientGeometry.x0 = cx - dx * halfDiag;
    out->gradientGeometry.y0 = cy - dy * halfDiag;
    out->gradientGeometry.x1 = cx + dx * halfDiag;
    out->gradientGeometry.y1 = cy + dy * halfDiag;
  }
  return {};
}

}  // namespace

void requestFillDialog() { g_fillRequested = true; }

void drawFillDialog(AppState& st) {
  static FillSourceUi ui;
  static std::string status;
  static std::string previewKey;

  if (g_fillRequested) {
    g_fillRequested = false;
    status.clear();
    ImGui::OpenPopup("Fill");
  }
  if (!beginDialog("Fill")) {
    clearExternalFilterPreview("Fill");
    previewKey.clear();
    return;
  }

  OpenDocument* od = st.documents.active();
  drawFillSourceControls(ui);

  FillParams p;
  const std::string resolveError = resolveFillParams(ui, st, od, &p);
  const Command command = fillCommand(p);
  updateFillPreview("Fill", od, command, resolveError.empty(), previewKey);
  if (!resolveError.empty()) {
    dialogHint("%s", resolveError.c_str());
    fillDialogFooter(od, status, command, "", /*commitEnabled=*/false);
  } else {
    fillDialogFooter(od, status, command, "Nothing changed (opacity 0, or no selected texels).");
  }
  endDialog();
}

void requestStrokeDialog() { g_strokeRequested = true; }

void drawStrokeDialog(AppState& st) {
  static FillSourceUi ui;
  static float width = 4.0f;
  static int locationIdx = 1;  // Inside, Center, Outside
  static std::string status;
  static std::string previewKey;

  if (g_strokeRequested) {
    g_strokeRequested = false;
    status.clear();
    ImGui::OpenPopup("Stroke");
  }
  if (!beginDialog("Stroke")) {
    clearExternalFilterPreview("Stroke");
    previewKey.clear();
    return;
  }

  OpenDocument* od = st.documents.active();

  dialogSlider("Width", &width, 0.0f, 200.0f, "%.1f", "px");
  static const char* kLocationItems[] = {"Inside", "Center", "Outside"};
  dialogRadioRow("Location", &locationIdx, kLocationItems, 3);
  drawFillSourceControls(ui);

  FillParams p;
  std::string resolveError = resolveFillParams(ui, st, od, &p);
  if (resolveError.empty() && !(width > 0.0f))
    resolveError = "Width must be greater than zero; a zero-width stroke draws nothing.";
  if (od != nullptr && !od->selection.has_value())
    dialogHint("Nothing is selected, so this strokes the edge of the layer's non-transparent "
               "pixels.");

  StrokeParams sp;
  sp.fill = p;
  sp.width = width;
  sp.location = locationIdx == 0   ? StrokeLocation::Inside
               : locationIdx == 2 ? StrokeLocation::Outside
                                  : StrokeLocation::Center;

  const Command command = strokeCommand(sp);
  updateFillPreview("Stroke", od, command, resolveError.empty(), previewKey);
  if (!resolveError.empty()) {
    dialogHint("%s", resolveError.c_str());
    fillDialogFooter(od, status, command, "", /*commitEnabled=*/false);
  } else {
    fillDialogFooter(od, status, command,
                     "Nothing changed (opacity 0, or the band covered no texels).");
  }
  endDialog();
}

void requestDefinePatternDialog() { g_definePatternRequested = true; }

void drawDefinePatternDialog(AppState& st) {
  static char nameBuf[96] = "";
  static std::string status;

  if (g_definePatternRequested) {
    g_definePatternRequested = false;
    status.clear();
    ImGui::OpenPopup("Define Pattern");
  }
  if (!beginDialog("Define Pattern")) {
    return;
  }

  OpenDocument* od = st.documents.active();
  dialogInputText("Name", nameBuf, sizeof(nameBuf));
  dialogHint(
      "Reads the active selection's bounding rectangle, or the whole layer with nothing "
      "selected.");

  const bool haveName = nameBuf[0] != '\0';
  if (!haveName) dialogHint("Type a name -- a fill resolves a pattern by name.");
  fillDialogFooter(od, status, definePatternCommand(std::string(nameBuf)), "",
                   /*commitEnabled=*/haveName);
  endDialog();
}

}  // namespace np
