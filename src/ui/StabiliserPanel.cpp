#include "ui/StabiliserPanel.hpp"

#include <cstdio>

#include "imgui.h"

namespace np {
namespace {

void save(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs);
  std::string err;
  st.strokePreferences.saveToFile(defaultStrokePreferencesFilePath(), st.stabiliserPrefs, &err);
}

const char* modeName(StabiliserMode m) noexcept {
  switch (m) {
    case StabiliserMode::Off: return "Off";
    case StabiliserMode::PulledString: return "Pulled string";
    case StabiliserMode::WeightedAverage: return "Weighted average";
  }
  return "Off";
}

// Draws the mode/parameter/option controls for one `StabiliserParams` in
// place, returning true if anything changed. `showOptions` is false for a
// brush's `Own` block -- those five options are the global's, so `Own`'s own
// copies are never drawn and never read.
bool drawParams(const char* idPrefix, StabiliserParams& p, bool showOptions) {
  bool changed = false;
  char id[64];
  int mode = static_cast<int>(p.mode);
  std::snprintf(id, sizeof(id), "##%smode", idPrefix);
  if (ImGui::Combo(id, &mode, "Off\0Pulled string\0Weighted average\0")) {
    p.mode = static_cast<StabiliserMode>(mode);
    changed = true;
  }
  if (p.mode == StabiliserMode::PulledString) {
    std::snprintf(id, sizeof(id), "String (px)##%sstring", idPrefix);
    changed |= ImGui::SliderFloat(id, &p.stringPx, 0.0f, 200.0f, "%.1f");
  } else if (p.mode == StabiliserMode::WeightedAverage) {
    std::snprintf(id, sizeof(id), "Strength##%sstrength", idPrefix);
    changed |= ImGui::SliderFloat(id, &p.strength, 0.0f, 100.0f, "%.0f");
    std::snprintf(id, sizeof(id), "Responsiveness##%sresp", idPrefix);
    changed |= ImGui::SliderFloat(id, &p.responsiveness, 0.0f, 100.0f, "%.0f");
  }
  if (showOptions) {
    changed |= ImGui::Checkbox("Catch up at stroke end", &p.catchUpAtEnd);
    ImGui::BeginDisabled(p.mode != StabiliserMode::WeightedAverage);
    changed |= ImGui::Checkbox("Catch up while paused", &p.catchUpWhilePaused);
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Stabilise pressure", &p.stabilisePressure);
    changed |= ImGui::Checkbox("Scale with zoom", &p.scaleWithZoom);
    changed |= ImGui::Checkbox("Show string", &p.showString);
  }
  return changed;
}

std::string describeEffective(const StabiliserParams& eff, const BrushStabiliserSetting& brush) {
  if (eff.mode == StabiliserMode::Off) return "Off";
  std::string s = modeName(eff.mode);
  if (eff.mode == StabiliserMode::PulledString) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", string %.0f px", eff.stringPx);
    s += buf;
  } else {
    char buf[80];
    std::snprintf(buf, sizeof(buf), ", strength %.0f", eff.strength);
    s += buf;
  }
  if (brush.mode == StabiliserBrushMode::FollowGlobal && brush.amountPct != 100.0f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " (global x%.2f)", brush.amountPct / 100.0f);
    s += buf;
  } else if (brush.mode == StabiliserBrushMode::Own) {
    s += " (this brush's own)";
  }
  return s;
}

}  // namespace

void drawPerBrushStabiliserControls(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs);
  BrushStabiliserSetting& s = st.brush.native.stabiliser;
  int mode = static_cast<int>(s.mode);
  if (ImGui::Combo("Stabiliser##brushMode", &mode, "Follow global\0Off\0Own\0"))
    s.mode = static_cast<StabiliserBrushMode>(mode);
  if (s.mode == StabiliserBrushMode::FollowGlobal) {
    ImGui::SliderFloat("Amount##brushAmount", &s.amountPct, 0.0f, 300.0f, "%.0f%%");
  } else if (s.mode == StabiliserBrushMode::Own) {
    drawParams("brushOwn", s.own, /*showOptions=*/false);
  }
  const StabiliserParams eff = resolveStabiliser(st.stabiliserPrefs, s);
  ImGui::TextDisabled("%s", describeEffective(eff, s).c_str());
}

void drawStabiliserPopover(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs);
  const StabiliserParams eff = resolveStabiliser(st.stabiliserPrefs, st.brush.native.stabiliser);
  char buttonLabel[80];
  std::snprintf(buttonLabel, sizeof(buttonLabel), "Stabiliser: %s",
               describeEffective(eff, st.brush.native.stabiliser).c_str());
  if (ImGui::Button(buttonLabel)) ImGui::OpenPopup("##stabiliserPopover");
  if (ImGui::BeginPopup("##stabiliserPopover")) {
    ImGui::TextUnformatted("ALL BRUSHES");
    ImGui::Separator();
    if (drawParams("global", st.stabiliserPrefs, /*showOptions=*/true)) save(st);

    ImGui::Spacing();
    ImGui::TextUnformatted("THIS BRUSH");
    ImGui::Separator();
    drawPerBrushStabiliserControls(st);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled(
        "Applies to the CPU brush routes only, next stroke onward -- not the GPU (oil/"
        "watercolour) solver route.");
    ImGui::EndPopup();
  }
}

}  // namespace np
