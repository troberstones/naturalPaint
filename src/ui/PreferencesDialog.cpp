#include "ui/PreferencesDialog.hpp"

#include <string>

#include "app/AppState.hpp"
#include "app/UiPreferences.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Dialog.hpp"

namespace np {
namespace {

bool g_preferencesRequested = false;

// Saved immediately on every edit, the way ui/StabiliserPanel.cpp and
// ui/BuildupPanel.cpp already save theirs -- there is no OK/Cancel model for
// preferences in this build, and inventing one here would make Preferences the
// only settings surface that can lose a change by being dismissed.
void save(AppState& st) {
  ensureUiPreferencesLoaded(st.uiPreferencesStore, st.uiPreferencesLoaded, st.uiPreferences);
  st.uiPreferencesStore.saveToFile(defaultUiPreferencesFilePath(), st.uiPreferences, nullptr);
}

}  // namespace

void applyUiScaleIfChanged(AppState& st) {
  ensureUiPreferencesLoaded(st.uiPreferencesStore, st.uiPreferencesLoaded, st.uiPreferences);
  if (st.uiScaleApplied == st.uiPreferences.uiScale) return;

  // The style is rebuilt from a pristine copy taken once, rather than scaled
  // from wherever the last scale left it. `ImGuiStyle::ScaleAllSizes()`
  // multiplies the live style IN PLACE, so applying 1.25 twice gives 1.5625,
  // and walking a slider from 1.0 to 2.0 and back would leave the padding
  // permanently wrong. Capturing the unscaled baseline is what makes this
  // idempotent and reversible.
  static ImGuiStyle baseStyle;
  static bool baseCaptured = false;
  if (!baseCaptured) {
    baseStyle = ImGui::GetStyle();
    baseCaptured = true;
  }
  ImGuiStyle& style = ImGui::GetStyle();
  style = baseStyle;
  style.ScaleAllSizes(st.uiPreferences.uiScale);
  // Fonts scale separately: `ScaleAllSizes()` touches metrics only, so without
  // this the interface would get roomier without the text getting bigger.
  ImGui::GetIO().FontGlobalScale = st.uiPreferences.uiScale;
  st.uiScaleApplied = st.uiPreferences.uiScale;
}

void applyThemeIfChanged(AppState& st) {
  ensureUiPreferencesLoaded(st.uiPreferencesStore, st.uiPreferencesLoaded, st.uiPreferences);
  if (st.themeApplied == st.uiPreferences.theme) return;
  setAtelierThemeMode(st.uiPreferences.theme);
  applyAtelierTheme();
  st.themeApplied = st.uiPreferences.theme;
}

void requestPreferencesDialog() { g_preferencesRequested = true; }

void drawPreferencesDialog(AppState& st) {
  if (g_preferencesRequested) {
    g_preferencesRequested = false;
    ensureUiPreferencesLoaded(st.uiPreferencesStore, st.uiPreferencesLoaded, st.uiPreferences);
    ImGui::OpenPopup("Preferences");
  }
  if (!beginDialog("Preferences")) return;

  dialogSection("Interface");
  float scale = st.uiPreferences.uiScale;
  if (dialogSlider("UI scale", &scale, kUiScaleMin, kUiScaleMax, "%.2fx")) {
    st.uiPreferences.uiScale = scale;
    // Applied on the NEXT frame's `applyUiScaleIfChanged()` rather than here:
    // rebuilding the style midway through a frame would leave this dialog's
    // own already-drawn widgets measured against the old metrics and the rest
    // against the new ones.
    save(st);
  }
  dialogHint(
      "Scales the whole interface -- text, padding and every control -- without changing the "
      "document or the brush. Takes effect immediately.");

  const char* const themeItems[] = {"Dark", "Light", "Match system"};
  int theme = static_cast<int>(st.uiPreferences.theme);
  if (dialogCombo("Theme", &theme, themeItems, 3)) {
    st.uiPreferences.theme = static_cast<AtelierThemeMode>(theme);
    // Same next-frame handoff as the scale slider above, via
    // `applyThemeIfChanged()` -- not applied here, for the same reason.
    save(st);
  }
  dialogHint("Changes the interface's colours. \"Match system\" follows the OS's own light/dark setting.");

  dialogSection("Touch");
  const char* const gestureItems[] = {"Pan the canvas", "Pick a colour", "Do nothing"};
  int gesture = static_cast<int>(st.uiPreferences.oneFinger);
  if (dialogCombo("One finger", &gesture, gestureItems, 3)) {
    st.uiPreferences.oneFinger = static_cast<OneFingerGesture>(gesture);
    save(st);
  }
  float debounce = st.uiPreferences.oneFingerDebounceMs;
  if (dialogSlider("Hold before it counts", &debounce, kOneFingerDebounceMinMs,
                   kOneFingerDebounceMaxMs, "%.0f ms")) {
    st.uiPreferences.oneFingerDebounceMs = debounce;
    save(st);
  }
  dialogHint(
      "Two fingers always pan, zoom and rotate, and the Apple Pencil always draws -- these "
      "settings are only about a single finger. The hold delay is how long that finger must "
      "stay down before its gesture starts, so brushing the glass in passing moves nothing.");

  DialogFooter footer;
  footer.commit = nullptr;  // nothing to commit: every edit above already saved
  footer.cancel = "Close";
  footer.note = "Changes apply as you make them.";
  if (dialogFooter(footer) != DialogAction::None) ImGui::CloseCurrentPopup();
  endDialog();
}

}  // namespace np
