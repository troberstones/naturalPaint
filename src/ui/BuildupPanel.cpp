#include "ui/BuildupPanel.hpp"

#include <cfloat>
#include <string>

#include "app/StrokePreferences.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"  // pushAtelierMono()/popAtelierMono(), for the options-bar field

namespace np {
namespace {

void save(AppState& st) {
  std::string err;
  st.strokePreferences.saveToFile(defaultStrokePreferencesFilePath(), st.stabiliserPrefs,
                                  st.pigmentBuildup, &err);
}

// What the closed field reads, so the band says which rules are live without
// being opened -- `ui/TaperPanel`'s `compactLabel()` for the same reason.
std::string compactLabel(const PigmentBuildup& b) {
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

  ImGui::TextDisabled("Where a stroke crosses itself");
  ImGui::Spacing();

  if (ImGui::Checkbox("Diminishing overlaps", &b.saturating)) save(st);
  ImGui::SetItemTooltip(
      "Each overlap adds a share of what is still empty, so paint approaches the paper's "
      "capacity instead of reaching it in a fixed number of passes. The first touch of a "
      "stroke is unchanged.");

  if (ImGui::Checkbox("Limit one stroke to its Opacity", &b.strokeCeiling)) save(st);
  ImGui::SetItemTooltip(
      "One stroke lays at most its Opacity of paint at any one place, however often it "
      "crosses itself. A second stroke still layers over the first.");
  if (b.strokeCeiling)
    ImGui::TextDisabled("Opacity is 100%% by default, where this\nchanges nothing -- bring it "
                        "down to see it.");

  ImGui::Spacing();
  ImGui::TextDisabled("Pigment layers only.");
}

void drawBuildupOptionsBarField(AppState& st) {
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
                                      compactLabel(st.pigmentBuildup).c_str(),
                                      ImGuiComboFlags_HeightLargest);
  popAtelierMono();
  if (open) {
    drawBuildupControls(st);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("How paint builds up where a stroke overlaps itself.");
}

}  // namespace np
