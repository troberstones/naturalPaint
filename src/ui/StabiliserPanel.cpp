#include "ui/StabiliserPanel.hpp"

#include <cfloat>
#include <cstdio>

#include "imgui.h"
#include "ui/AtelierChrome.hpp"  // pushAtelierMono()/popAtelierMono(), for the options-bar field

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
    // Wave 2 brief item 2: while the pen is down and not moving, the string
    // decays toward zero so the nib reaches the pen after about this many ms
    // (StabiliserParams::catchUpMs's own comment has the exact 95% formula).
    // 0 = off, the string just sits at its full window (wave 1's behaviour).
    std::snprintf(id, sizeof(id), "Catch up (ms)##%scatchup", idPrefix);
    changed |= ImGui::SliderFloat(id, &p.catchUpMs, 0.0f, 2000.0f, "%.0f");
    ImGui::SetItemTooltip(
        "While the pen is held still, the pulled string shortens so the brush point reaches "
        "the pen after about this long -- 0 turns it off. Moving the pen again brings the full "
        "string length straight back.");
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

// The options-bar field's own short form -- Wave 2 brief item 4's examples
// verbatim ("Pulled 16 px", "Avg 40", "Off"), not `describeEffective()`'s
// longer "Stabiliser: Weighted average, strength 40 (global x1.50)": the bar
// has room for a field, not a sentence, the same reason TIP truncates its
// dab names next to this one.
std::string compactEffectiveLabel(const StabiliserParams& eff) {
  char buf[32];
  switch (eff.mode) {
    case StabiliserMode::Off:
      return "Off";
    case StabiliserMode::PulledString:
      std::snprintf(buf, sizeof(buf), "Pulled %.0f px", eff.stringPx);
      return buf;
    case StabiliserMode::WeightedAverage:
      std::snprintf(buf, sizeof(buf), "Avg %.0f", eff.strength);
      return buf;
  }
  return "Off";
}

// The popup body both the options-bar field and the Brush Settings window's
// button open -- one definition so the two surfaces cannot disagree about
// what "the Stabiliser popover" contains (Wave 2 brief item 4: only the
// TRIGGER differs between them).
void drawStabiliserPopoverContents(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs);
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
    drawStabiliserPopoverContents(st);
    ImGui::EndPopup();
  }
}

void drawStabiliserOptionsBarField(AppState& st) {
  ensureStrokePreferencesLoaded(st.strokePreferences, st.strokePreferencesLoaded,
                                st.stabiliserPrefs);
  const StabiliserParams eff = resolveStabiliser(st.stabiliserPrefs, st.brush.native.stabiliser);
  // Same trigger shape as TIP's dab picker just below this in the band: a
  // `BeginCombo` styled with the band's own mono push/pop and width, so it
  // reads as one more field rather than a button that happens to sit among
  // them. `BeginCombo`/`EndCombo` bracket a popup with no restriction to
  // `Selectable` rows -- the same freeform content the button's popup drew
  // works unchanged inside it.
  pushAtelierMono();
  ImGui::SetNextItemWidth(130.0f);
  // Same eight-item popup cap as `ui/TaperPanel.cpp`'s own field, and the same
  // reason to lift it: these contents are far taller than eight rows, so the
  // bottom of them was reachable only by scrolling a popup that does not look
  // scrollable.
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginCombo("##stabiliserField", compactEffectiveLabel(eff).c_str())) {
    drawStabiliserPopoverContents(st);
    ImGui::EndCombo();
  }
  popAtelierMono();
  ImGui::SetItemTooltip("%s", describeEffective(eff, st.brush.native.stabiliser).c_str());
}

}  // namespace np
