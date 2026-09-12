#include "ui/TaperPanel.hpp"

#include <cfloat>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "ui/AtelierChrome.hpp"  // pushAtelierMono()/popAtelierMono(), for the options-bar field

namespace np {
namespace {

// One taper, in the shape both ends share -- so the two read as the same
// control twice rather than as two features. The length stays editable while
// the taper is off: it is what you set BEFORE switching it on, and what the
// `on` field exists to preserve (`BrushTaper::on`'s own comment).
// What a taper switched on for the first time gets, so ticking the box does
// something visible. 0 px is a legal setting -- it is just not one anybody
// reaches for on purpose, and it is what "I ticked it and nothing happened"
// was.
constexpr float kDefaultLengthPx = 40.0f;

void drawOne(const char* title, const char* idScope, BrushTaper& taper) {
  ImGui::PushID(idScope);
  if (ImGui::Checkbox(title, &taper.on) && taper.on && taper.lengthPx <= 0.0f)
    taper.lengthPx = kDefaultLengthPx;
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("Length (px)", &taper.lengthPx, 0.0f, 500.0f, "%.0f");
  ImGui::BeginDisabled(!taper.on || taper.lengthPx <= 0.0f);
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("Min size %", &taper.minSizePct, 0.0f, 100.0f, "%.0f");
  ImGui::Checkbox("Taper flow too", &taper.flow);
  ImGui::EndDisabled();
  if (taper.on && taper.lengthPx <= 0.0f) ImGui::TextDisabled("0 px is no ramp -- set a length.");
  ImGui::PopID();
}

bool live(const BrushTaper& t) noexcept { return t.on && t.lengthPx > 0.0f; }

// What the options-bar field reads at a glance, in the band's own mono width.
std::string compactLabel(const NativeBrush& n) {
  char buf[64];
  if (live(n.taperIn) && live(n.taperOut))
    std::snprintf(buf, sizeof(buf), "In %.0f / out %.0f", n.taperIn.lengthPx,
                  n.taperOut.lengthPx);
  else if (live(n.taperIn))
    std::snprintf(buf, sizeof(buf), "In %.0f px", n.taperIn.lengthPx);
  else if (live(n.taperOut))
    std::snprintf(buf, sizeof(buf), "Out %.0f px", n.taperOut.lengthPx);
  else
    std::snprintf(buf, sizeof(buf), "Off");
  return buf;
}

}  // namespace

void drawBrushTaperControls(AppState& st) {
  drawOne("Entry taper", "taperin", st.brush.native.taperIn);
  ImGui::Spacing();
  drawOne("Exit taper", "taperout", st.brush.native.taperOut);
  ImGui::TextDisabled(
      "Applies to the CPU brush routes only -- not the GPU (oil/watercolour) solver route.");
  // Said where it is set: the stroke visibly changes at pen-up, which reads
  // as a glitch until you know it is the taper arriving.
  // `StrokeSession::allDabs_` explains why it can only arrive then.
  if (live(st.brush.native.taperOut))
    ImGui::TextDisabled("An exit taper can only be drawn once the stroke ends, so the\n"
                        "stroke is redrawn with it the moment the pen lifts.");
}

void drawTaperOptionsBarField(AppState& st) {
  pushAtelierMono();
  ImGui::SetNextItemWidth(130.0f);
  // Without a constraint of our own, a combo popup is capped at EIGHT items
  // tall (`ImGuiComboFlags_HeightRegular`, imgui_widgets.cpp's own
  // `CalcMaxPopupHeightFromItemCount()`) and silently scrolls the rest. These
  // contents are about a dozen rows, so the whole exit taper sat below the
  // fold -- which is exactly how it came to look like a control that did
  // nothing.
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  const bool open = ImGui::BeginCombo("##taperField", compactLabel(st.brush.native).c_str());
  popAtelierMono();
  if (open) {
    drawBrushTaperControls(st);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip(
      "Entry and exit taper: how far the stroke ramps up from its start and down to its end.");
}

}  // namespace np
