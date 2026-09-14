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
  ImGui::SetNextItemWidth(kBandFieldWidthPx);
  // `ImGuiComboFlags_HeightLargest` rather than a `SetNextWindowSizeConstraints`
  // of our own. Both uncap the popup, which a combo otherwise clamps to EIGHT
  // items and silently scrolls -- how the whole exit taper group once came to
  // sit below the fold. Only the flag is safe: a CLOSED combo returns from
  // `BeginCombo` before it looks at `NextWindowData` (imgui_widgets.cpp's
  // "Set popup size" block sits after that early return), so a constraint set
  // by hand is left pending and is consumed by whatever window is begun next
  // -- a dialog, a panel, anything -- which quietly resized windows that have
  // nothing to do with this field.
  const bool open = ImGui::BeginCombo("##taperField", compactLabel(st.brush.native).c_str(),
                                      ImGuiComboFlags_HeightLargest);
  popAtelierMono();
  if (open) {
    drawBrushTaperControls(st);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip(
      "Entry and exit taper: how far the stroke ramps up from its start and down to its end.");
}

}  // namespace np
