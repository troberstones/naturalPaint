#include "ui/BuildupPanel.hpp"

#include <cfloat>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "app/StrokePreferences.hpp"
#include "app/StrokeSession.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"  // pushAtelierMono()/popAtelierMono(), for the options-bar field

namespace np {
namespace {

void save(AppState& st) {
  std::string err;
  st.strokePreferences.saveToFile(defaultStrokePreferencesFilePath(), st.stabiliserPrefs,
                                  st.pigmentBuildup, &err);
}

StrokeRoute activeRoute(AppState& st) {
  const OpenDocument* od = st.documents.active();
  return strokeRouteFor(st.brush.tool, od != nullptr ? activeLayerOf(*od) : nullptr);
}

// What the closed field reads, so the band says which rules are live without
// being opened -- `ui/TaperPanel`'s `compactLabel()` for the same reason. Off
// the two deposit routes it says so: "Wash" over a stroke it cannot reach
// looks broken.
bool reachesRoute(StrokeRoute route) {
  return route == StrokeRoute::CpuDeposit || route == StrokeRoute::RgbDeposit;
}

std::string compactLabel(const PigmentBuildup& b, StrokeRoute route) {
  if (!reachesRoute(route)) return "Off here";
  return b.mode == PigmentBuildupMode::Wash ? "Wash" : "Build-up";
}

}  // namespace

void drawBuildupControls(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs, st.pigmentBuildup);
  PigmentBuildup& b = st.pigmentBuildup;
  const StrokeRoute route = activeRoute(st);
  const bool reaches = reachesRoute(route);

  ImGui::TextDisabled("How a stroke builds where it overlaps");
  ImGui::Spacing();
  if (!reaches) {
    ImGui::Text("No effect on this stroke (%s).\nBuild-up and Wash are for brush strokes\n"
                "on Pigment and RGB layers.",
                strokeRouteName(route));
    ImGui::Spacing();
  }
  ImGui::BeginDisabled(!reaches);

  const bool wash = b.mode == PigmentBuildupMode::Wash;
  if (ImGui::RadioButton("Build-up", !wash)) {
    b.mode = PigmentBuildupMode::BuildUp;
    save(st);
  }
  ImGui::SetItemTooltip("Every dab adds paint, so a stroke darkens wherever it overlaps, "
                        "itself included.");
  if (ImGui::RadioButton("Wash", wash)) {
    b.mode = PigmentBuildupMode::Wash;
    save(st);
  }
  ImGui::SetItemTooltip("Each spot keeps the strongest dab the stroke laid there, so a stroke is "
                        "no darker where it crosses itself. Load and Opacity set how dark one "
                        "stroke is; a new stroke still layers on top.");
  if (wash) {
    // Here as well as in Brush Settings: in Wash, Opacity is how dark a stroke
    // is, and the options bar has no room for a field of its own.
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Opacity", &st.brush.opacity, 0.0f, 1.0f, "%.2f");
  }
  ImGui::EndDisabled();
}

void drawBuildupOptionsBarField(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs, st.pigmentBuildup);
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
  const bool open = ImGui::BeginCombo("##buildupField",
                                      compactLabel(st.pigmentBuildup, activeRoute(st)).c_str(),
                                      ImGuiComboFlags_HeightLargest);
  popAtelierMono();
  if (open) {
    drawBuildupControls(st);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("How paint builds up where a stroke overlaps itself.");
}

}  // namespace np
