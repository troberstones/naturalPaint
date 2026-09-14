#include "ui/BrushSettingsWindow.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "app/AppState.hpp"
#include "app/ControlsLayout.hpp"
#include "brush/BrushModel.hpp"
#include "brush/BrushModelFields.hpp"
#include "brush/Grain.hpp"
#include "brush/ToolOptionsBlend.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/BrushPanelLayout.hpp"
#include "ui/LabelledControl.hpp"
#include "ui/PatternPicker.hpp"
#include "ui/StabiliserPanel.hpp"
#include "ui/TaperPanel.hpp"

namespace np {
namespace {

using K = BrushRowKind;

constexpr float kListWidth = 184.0f;
constexpr float kGutter = 14.0f;  // the not-painted ring sits in it, left of its row
constexpr float kTipGridHeight = 206.0f;
constexpr float kPatternGridHeight = 150.0f;

// Every leaf of one BrushModel by the path the tables use, from the same walk
// that saves and loads the model.
struct LeafIndex {
  std::unordered_map<std::string, bool*> bools;
  std::unordered_map<std::string, int32_t*> ints;
  std::unordered_map<std::string, float*> floats;
  std::unordered_map<std::string, std::string*> strings;
  std::unordered_map<std::string, VarianceControl*> controls;
  std::unordered_map<std::string, CoverageBlend*> blends;
};

template <typename T>
T* leafAt(const std::unordered_map<std::string, T*>& leaves, const std::string& path) {
  const auto it = leaves.find(path);
  return it == leaves.end() ? nullptr : it->second;
}

LeafIndex indexLeaves(BrushModel& model) {
  LeafIndex ix;
  visitBrushModelFields(model, [&](const std::string& path, auto& leaf) {
    using T = std::decay_t<decltype(leaf)>;
    if constexpr (std::is_same_v<T, bool>) ix.bools.emplace(path, &leaf);
    else if constexpr (std::is_same_v<T, int32_t>) ix.ints.emplace(path, &leaf);
    else if constexpr (std::is_same_v<T, float>) ix.floats.emplace(path, &leaf);
    else if constexpr (std::is_same_v<T, std::string>) ix.strings.emplace(path, &leaf);
    else if constexpr (std::is_same_v<T, VarianceControl>) ix.controls.emplace(path, &leaf);
    else if constexpr (std::is_same_v<T, CoverageBlend>) ix.blends.emplace(path, &leaf);
  });
  return ix;
}

// --- Label-left controls with the page's own column ------------------------
//
// Not `ctlSlider()`'s column: that one is shared with the docked BRUSH column
// and only grows, so "Foreground/Background Jitter" drawn here would push every
// label over there. Reset per page, and pre-grown to the page's widest label so
// switching pages does not shift the first rows for a frame.
float g_pageColumn = 0.0f;

void pageLabel(const char* label, char* idOut, size_t cap) {
  std::snprintf(idOut, cap, "##%s", label);
  const float startX = ImGui::GetCursorPosX();
  const LabelledControlLayout lay = layoutLabelledControl(
      g_pageColumn, ImGui::CalcTextSize(label).x, ImGui::GetContentRegionAvail().x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (!lay.labelOnOwnLine) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(startX + lay.labelColumn);
  }
  ImGui::SetNextItemWidth(lay.widgetWidth);
}

bool pageSlider(const char* label, float* v, float lo, float hi, const char* fmt) {
  char id[96];
  pageLabel(label, id, sizeof(id));
  return ImGui::SliderFloat(id, v, lo, hi, fmt);
}

bool pageSliderInt(const char* label, int* v, int lo, int hi) {
  char id[96];
  pageLabel(label, id, sizeof(id));
  return ImGui::SliderInt(id, v, lo, hi);
}

bool pageBeginCombo(const char* label, const char* preview) {
  char id[96];
  pageLabel(label, id, sizeof(id));
  return ImGui::BeginCombo(id, preview);
}

void resetPageColumn(BrushPanel panel) {
  g_pageColumn = 0.0f;
  const float avail = ImGui::GetContentRegionAvail().x - kGutter;
  auto grow = [&](const char* label) {
    (void)layoutLabelledControl(g_pageColumn, ImGui::CalcTextSize(label).x, avail);
  };
  for (const BrushRowSpec& row : brushPanelRows()) {
    if (row.panel != panel) continue;
    switch (row.kind) {
      case K::Slider:
      case K::IntSlider:
      case K::Blend:
      case K::Pattern:
      case K::ToolBlend: grow(row.label); break;
      case K::Jitter:
        grow(row.label);
        grow("Control");
        grow("Fade Steps");
        if (row.minimumLabel != nullptr) grow(row.minimumLabel);
        break;
      case K::Separator:
      case K::Check:
      case K::TipPicker: break;
    }
  }
}

void itemTooltip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", text);
}

void warningText(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, atelierToken(kWarning));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// A hollow ring in the warning colour: saved with the brush, not painted yet.
// Hollow so it reads as "missing", not as a status light that is on.
void notPaintedRing(float cx, float cy, const char* why) {
  const float r = ImGui::GetFontSize() * 0.24f;
  ImGui::GetWindowDrawList()->AddCircle(ImVec2(cx, cy), r, atelierToken(kWarning), 16, 1.5f);
  const float reach = r + 3.0f;
  if (ImGui::IsWindowHovered() &&
      ImGui::IsMouseHoveringRect(ImVec2(cx - reach, cy - reach), ImVec2(cx + reach, cy + reach)))
    ImGui::SetTooltip("%s", why);
}

// --- Rows -------------------------------------------------------------------

struct Page {
  AppState& st;
  GpuContext& gpu;
  LeafIndex& ix;
};

const char* rowLabelFor(const char* path) {
  for (const BrushRowSpec& row : brushPanelRows())
    if (std::string(row.path) == path && row.label[0] != '\0') return row.label;
  return path;
}

bool rowEnabled(const Page& pg, const BrushRowSpec& row, std::string& why) {
  switch (row.enable) {
    case BrushRowEnable::Always: return true;
    case BrushRowEnable::ControlIsPenTilt: {
      why = std::string("Only used when ") + rowLabelFor(row.enablePath) +
            "'s Control is Pen Tilt.";
      const VarianceControl* control =
          leafAt(pg.ix.controls, std::string(row.enablePath) + ".control");
      return control != nullptr && *control == VarianceControl::PenTilt;
    }
    case BrushRowEnable::BoolOn: {
      why = std::string("Only used when ") + rowLabelFor(row.enablePath) + " is on.";
      const bool* on = leafAt(pg.ix.bools, row.enablePath);
      return on != nullptr && *on;
    }
    case BrushRowEnable::ProceduralTip:
      why = "A bitmap tip carries its own edge; Hardness shapes only the round tip.";
      return pg.st.brush.tipBitmap == nullptr;
  }
  return true;
}

bool drawJitterRow(Page& pg, const BrushRowSpec& row) {
  const std::string base = row.path;
  float* jitter = leafAt(pg.ix.floats, base + ".jitter");
  VarianceControl* control = leafAt(pg.ix.controls, base + ".control");
  int32_t* fadeSteps = leafAt(pg.ix.ints, base + ".fadeSteps");
  if (jitter == nullptr || control == nullptr || fadeSteps == nullptr) return false;

  bool changed = false;
  float shown = brushRowShown(*jitter, row.scale);
  if (pageSlider(row.label, &shown, row.lo, row.hi, row.fmt))
    changed |= brushRowStore(shown, row.scale, *jitter);

  // A control this row does not offer (an older preset, a hand-edited file)
  // still shows as the preview, so it is visible rather than silently replaced.
  if (pageBeginCombo("Control", varianceControlName(*control))) {
    for (VarianceControl choice : brushControlChoices(row.controls)) {
      const bool selected = *control == choice;
      if (ImGui::Selectable(varianceControlName(choice), selected) && !selected) {
        *control = choice;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (*control == VarianceControl::Fade) {
    char id[96];
    pageLabel("Fade Steps", id, sizeof(id));
    int steps = *fadeSteps;
    if (ImGui::InputInt(id, &steps)) {
      *fadeSteps = std::clamp(steps, 1, 9999);
      changed = true;
    }
  }
  if (row.minimumLabel != nullptr) {
    if (float* minimum = leafAt(pg.ix.floats, base + ".minimum")) {
      float m = brushRowShown(*minimum, 100.0f);
      if (pageSlider(row.minimumLabel, &m, 0.0f, 100.0f, "%.0f%%"))
        changed |= brushRowStore(m, 100.0f, *minimum);
    }
  }
  return changed;
}

bool drawBlendRow(Page& pg, const BrushRowSpec& row) {
  CoverageBlend* blend = leafAt(pg.ix.blends, row.path);
  if (blend == nullptr) return false;
  bool changed = false;
  auto itemLabel = [](CoverageBlend b) {
    std::string label = coverageBlendName(b);
    if (!coverageBlendIsRenderable(b)) label += "  (not painted)";
    return label;
  };
  if (pageBeginCombo(row.label, itemLabel(*blend).c_str())) {
    for (CoverageBlend choice : brushBlendChoices(row.dualModes)) {
      const bool selected = *blend == choice;
      if (ImGui::Selectable(itemLabel(choice).c_str(), selected) && !selected) {
        *blend = choice;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (!coverageBlendIsRenderable(*blend))
    textDisabledWrapped("%s has no published formula, so it is not painted: the %s is left out "
                        "while it is chosen. It is kept so the brush saves as it came.",
                        coverageBlendName(*blend), row.dualModes ? "second tip" : "pattern");
  return changed;
}

bool drawToolBlendRow(Page& pg, const BrushRowSpec& row) {
  std::string* id = leafAt(pg.ix.strings, row.path);
  if (id == nullptr) return false;
  const std::vector<BrushToolBlendChoice>& choices = brushToolBlendChoices();
  auto itemLabel = [&](const std::string& psId) {
    std::string label = psId.empty() ? "Normal" : psId;
    for (const BrushToolBlendChoice& choice : choices)
      if (psId == choice.id) label = choice.label;
    BlendMode mode = BlendMode::Normal;
    if (!blendModeFromPsToolOptions(psId, mode)) label += "  (not painted)";
    return label;
  };
  bool changed = false;
  const std::string current = itemLabel(*id);
  if (pageBeginCombo(row.label, current.c_str())) {
    for (const BrushToolBlendChoice& choice : choices) {
      const std::string label = itemLabel(choice.id);
      const bool selected = label == current;
      if (ImGui::Selectable(label.c_str(), selected) && !selected) {
        *id = choice.id;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  BlendMode mode = BlendMode::Normal;
  std::string reason;
  if (!blendModeFromPsToolOptions(*id, mode, &reason)) textDisabledWrapped("%s", reason.c_str());
  textDisabledWrapped("Applied on RGB layers and when stroking a path; not yet on "
                      "Strokes layers or Pigment layers.");
  return changed;
}

bool drawTipPickerRow(Page& pg, const BrushRowSpec& row) {
  bool changed = false;
  if (ImGui::BeginChild("tips", ImVec2(0.0f, kTipGridHeight), ImGuiChildFlags_None))
    changed = drawBrushTipPicker(pg.st, pg.gpu, row.panel == BrushPanel::DualBrush);
  ImGui::EndChild();
  return changed;
}

bool drawPatternRow(Page& pg, const BrushRowSpec& row) {
  const PatternRef& pattern = pg.st.brush.model.texture.pattern;
  char id[96];
  pageLabel(row.label, id, sizeof(id));
  if (pattern.empty()) {
    ImGui::TextDisabled("None: the brush uses its Paper Grain");
  } else {
    const std::string& name = pattern.name.empty() ? pattern.id : pattern.name;
    if (pattern.field != nullptr) {
      ImGui::Text("%s  %d x %d", name.c_str(), pattern.field->width, pattern.field->height);
    } else {
      ImGui::TextDisabled("%s  (not found)", name.c_str());
      itemTooltip("This paper is in neither pattern folder. Load the .abr it came from, or\n"
                  "pick another below.");
    }
  }
  bool changed = false;
  if (ImGui::BeginChild("patterns", ImVec2(0.0f, kPatternGridHeight), ImGuiChildFlags_None))
    changed = drawBrushPatternPicker(pg.st, pg.gpu);
  ImGui::EndChild();
  return changed;
}

// Returns true when the row wrote to the model.
bool drawRow(Page& pg, const BrushRowSpec& row, bool panelOn) {
  if (row.kind == K::Separator) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    return false;
  }
  std::string why;
  const bool enabled = (panelOn || brushRowLiveWhilePanelOff(row)) && rowEnabled(pg, row, why);
  const ImVec2 rowStart = ImGui::GetCursorScreenPos();
  bool changed = false;
  ImGui::PushID(row.path);
  ImGui::BeginDisabled(!enabled);
  switch (row.kind) {
    case K::Separator: break;
    case K::Check:
      if (bool* v = leafAt(pg.ix.bools, row.path)) changed = ImGui::Checkbox(row.label, v);
      break;
    case K::Slider:
      if (float* v = leafAt(pg.ix.floats, row.path)) {
        float shown = brushRowShown(*v, row.scale);
        if (pageSlider(row.label, &shown, row.lo, row.hi, row.fmt))
          changed = brushRowStore(shown, row.scale, *v);
      }
      break;
    case K::IntSlider:
      if (int32_t* v = leafAt(pg.ix.ints, row.path)) {
        int shown = *v;
        if (pageSliderInt(row.label, &shown, static_cast<int>(row.lo), static_cast<int>(row.hi))) {
          *v = shown;
          changed = true;
        }
      }
      break;
    case K::Jitter: changed = drawJitterRow(pg, row); break;
    case K::Blend: changed = drawBlendRow(pg, row); break;
    case K::TipPicker: changed = drawTipPickerRow(pg, row); break;
    case K::Pattern: changed = drawPatternRow(pg, row); break;
    case K::ToolBlend: changed = drawToolBlendRow(pg, row); break;
  }
  ImGui::EndDisabled();
  if (!enabled && !why.empty()) itemTooltip(why.c_str());
  ImGui::PopID();
  if (row.notPainted != nullptr)
    notPaintedRing(rowStart.x - kGutter * 0.5f, rowStart.y + ImGui::GetFrameHeight() * 0.5f,
                   row.notPainted);
  return changed;
}

// A Photoshop or Tool Options page: the panel's rows, greyed while its switch is off.
bool drawModelPage(Page& pg, BrushPanel panel) {
  const BrushPanelSpec& spec = brushPanelSpec(panel);
  const bool* on = spec.enablePath[0] != '\0' ? leafAt(pg.ix.bools, spec.enablePath) : nullptr;
  const bool panelOn = on == nullptr || *on;
  if (spec.notPainted != nullptr) warningText(spec.notPainted);
  if (!panelOn) {
    bool pickTurnsOn = false;
    for (const BrushRowSpec& row : brushPanelRows())
      if (row.panel == panel && brushRowLiveWhilePanelOff(row)) pickTurnsOn = true;
    textDisabledWrapped(pickTurnsOn ? "%s is off. Tick it in the list, or pick a pattern, to paint "
                                      "with these settings."
                                    : "%s is off. Tick it in the list to paint with these settings.",
                        spec.label);
  }
  resetPageColumn(panel);
  bool changed = false;
  // Greyed row by row rather than around the page: ImGui cannot re-enable a
  // control inside a disabled block, and the pattern grid must stay live.
  ImGui::Indent(kGutter);
  for (const BrushRowSpec& row : brushPanelRows())
    if (row.panel == panel) changed |= drawRow(pg, row, panelOn);
  ImGui::Unindent(kGutter);
  return changed;
}

// --- The list ---------------------------------------------------------------

// Returns true when the Dual Brush switch was flipped.
bool drawPanelList(AppState& st, LeafIndex& ix) {
  bool dualToggled = false;
  BrushPanelGroup group = BrushPanelGroup::Photoshop;
  for (size_t i = 0; i < kBrushPanelCount; ++i) {
    const auto panel = static_cast<BrushPanel>(i);
    const BrushPanelSpec& spec = brushPanelSpec(panel);
    if (panel == BrushPanel::LinkMatrix && !st.showAdvancedDynamics) continue;
    if (spec.group != group) {
      group = spec.group;
      ImGui::Spacing();
      pushAtelierMono();
      ImGui::TextDisabled("%s", group == BrushPanelGroup::ToolPreset ? "TOOL PRESET" : "NATURALPAINT");
      popAtelierMono();
    }
    ImGui::PushID(static_cast<int>(i));
    const float frame = ImGui::GetFrameHeight();
    if (bool* on = spec.enablePath[0] != '\0' ? leafAt(ix.bools, spec.enablePath) : nullptr) {
      if (ImGui::Checkbox("##on", on) && panel == BrushPanel::DualBrush) dualToggled = true;
    } else {
      ImGui::Dummy(ImVec2(frame, frame));
    }
    ImGui::SameLine();
    const float ring = spec.notPainted != nullptr ? ImGui::GetFontSize() : 0.0f;
    ImGui::AlignTextToFramePadding();
    if (spec.hasPage) {
      const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - ring);
      if (ImGui::Selectable(spec.label, st.brushSettingsPanel == static_cast<int>(i),
                            ImGuiSelectableFlags_None, ImVec2(width, 0.0f)))
        st.brushSettingsPanel = static_cast<int>(i);
    } else {
      ImGui::TextUnformatted(spec.label);
    }
    if (spec.notPainted != nullptr) {
      ImGui::SameLine();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(ring, frame));
      notPaintedRing(p.x + ring * 0.5f, p.y + frame * 0.5f, spec.notPainted);
    }
    ImGui::PopID();
  }
  return dualToggled;
}

// The stored selection names a page that can be shown, or falls back to the tip.
BrushPanel shownPanel(AppState& st) {
  const int i = st.brushSettingsPanel;
  const bool inRange = i >= 0 && i < static_cast<int>(kBrushPanelCount);
  if (inRange) {
    const auto panel = static_cast<BrushPanel>(i);
    const bool hidden = panel == BrushPanel::LinkMatrix && !st.showAdvancedDynamics;
    if (brushPanelSpec(panel).hasPage && !hidden) return panel;
  }
  st.brushSettingsPanel = 0;
  return BrushPanel::TipShape;
}

}  // namespace

void drawBrushSettingsWindow(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  if (!st.showBrushSettings) return;

  ImGui::SetNextWindowSize(ImVec2(620.0f, 720.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 380.0f), ImVec2(FLT_MAX, FLT_MAX));
  // The id after ### is new so imgui.ini's size for the old tabbed window, too
  // narrow for a list beside a page, is not applied to this one.
  if (!ImGui::Begin("Brush Settings###BrushSettingsPanels", &st.showBrushSettings)) {
    ImGui::End();
    return;
  }

  drawBrushPresetHeader(st);

  LeafIndex ix = indexLeaves(st.brush.model);
  const BrushPanel panel = shownPanel(st);

  // Measured on the previous frame; the first frame's guess only sets where the
  // body ends for one frame.
  static float previewHeight = 150.0f;
  const float bodyHeight = std::max(
      160.0f, ImGui::GetContentRegionAvail().y - previewHeight - ImGui::GetStyle().ItemSpacing.y);

  bool dualChanged = false;
  if (ImGui::BeginChild("body", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, atelierToken(kChromeDeep));
    if (ImGui::BeginChild("panels", ImVec2(kListWidth, 0.0f),
                          ImGuiChildFlags_AlwaysUseWindowPadding))
      dualChanged |= drawPanelList(st, ix);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::BeginChild("page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
      Page pg{st, gpu, ix};
      ImGui::SeparatorText(brushPanelSpec(panel).label);
      switch (panel) {
        case BrushPanel::TipShape:
        case BrushPanel::ShapeDynamics:
        case BrushPanel::Scattering:
        case BrushPanel::Texture:
        case BrushPanel::ColorDynamics:
        case BrushPanel::Transfer:
        case BrushPanel::BrushPose:
        case BrushPanel::Noise:
        case BrushPanel::WetEdges:
        case BrushPanel::BuildUp:
        case BrushPanel::Smoothing:
        case BrushPanel::ProtectTexture:
        case BrushPanel::ToolOptions: (void)drawModelPage(pg, panel); break;
        case BrushPanel::DualBrush: dualChanged |= drawModelPage(pg, panel); break;
        case BrushPanel::Paint: drawBrushPaintGroup(st); break;
        case BrushPanel::PaperGrain: drawBrushTextureGroup(st, /*ownPage=*/true); break;
        case BrushPanel::TaperStabiliser:
          drawBrushTaperControls(st);
          ImGui::Spacing();
          ImGui::SeparatorText("Stabiliser");
          drawPerBrushStabiliserControls(st);
          break;
        case BrushPanel::LinkMatrix: drawBrushDynamicsGroup(st); break;
        case BrushPanel::Count: break;
      }
    }
    ImGui::EndChild();
  }
  ImGui::EndChild();

  // `BrushState::dualTip` is a cache of the model's Dual Brush; the stroke
  // reads the cache, so an edit that skipped this would paint the old tip.
  if (dualChanged) {
    st.brush.dualTip = dualTipFromModel(st.brush.model.dual);
    st.brush.dualBlend = st.brush.model.dual.blend;
  }

  const float previewTop = ImGui::GetCursorPosY();
  ImGui::Separator();
  drawBrushPreviewStroke(st, gpu, lut);
  previewHeight = ImGui::GetCursorPosY() - previewTop;

  ImGui::End();
}

}  // namespace np
