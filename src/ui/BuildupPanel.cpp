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
// the Pigment deposit it says so: an RGB layer is what File > New makes, and
// switches that read "Saturate" over a stroke they cannot reach look broken.
std::string compactLabel(const PigmentBuildup& b, StrokeRoute route) {
  if (route != StrokeRoute::CpuDeposit) return "Off here";
  if (b.saturating && b.strokeCeiling) return "Sat + cap";
  if (b.saturating) return "Saturate";
  if (b.strokeCeiling) return "Stroke cap";
  return "Linear";
}

}  // namespace

void drawBuildupControls(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs, st.pigmentBuildup);
  PigmentBuildup& b = st.pigmentBuildup;
  const StrokeRoute route = activeRoute(st);
  const bool reaches = route == StrokeRoute::CpuDeposit;

  ImGui::TextDisabled("Where a stroke crosses itself");
  ImGui::Spacing();
  if (route == StrokeRoute::RgbDeposit) {
    ImGui::TextUnformatted("This layer is RGB, where strokes already\n"
                           "diminish on overlap and stop at Opacity.\n"
                           "These switches are for Pigment layers.");
    ImGui::Spacing();
  } else if (!reaches) {
    ImGui::Text("No effect on this stroke (%s).\nThese switches are for Pigment layers.",
                strokeRouteName(route));
    ImGui::Spacing();
  }
  ImGui::BeginDisabled(!reaches);

  if (ImGui::Checkbox("Diminishing overlaps", &b.saturating)) save(st);
  ImGui::SetItemTooltip(
      "Each overlap adds a share of what is still empty, so paint approaches the paper's "
      "capacity instead of reaching it in a fixed number of passes. The first touch of a "
      "stroke is unchanged.");

  if (ImGui::Checkbox("Limit one stroke to its Opacity", &b.strokeCeiling)) save(st);
  ImGui::SetItemTooltip(
      "One stroke lays at most its Opacity of paint at any one place, however often it "
      "crosses itself. A second stroke still layers over the first.");
  if (b.strokeCeiling) {
    // Here as well as in Brush Settings, because the switch is useless without
    // it and the options bar has no room for a field of its own.
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Opacity", &st.brush.opacity, 0.0f, 1.0f, "%.2f");
    if (st.brush.opacity >= 1.0f)
      ImGui::TextDisabled("At 100%% this changes nothing.");
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
