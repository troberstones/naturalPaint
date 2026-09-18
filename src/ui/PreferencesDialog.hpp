#pragma once

// ui/PreferencesDialog -- the Preferences window: interface scale, and what a
// single finger does on a touchscreen.
//
// A modal through `ui/Dialog.hpp` rather than a dockable panel, per ADR-0010
// ("one modal module", enforced by app/selftest/DialogModule.cpp's source
// scan): these are settings a user opens, changes and closes, not a tool
// surface they work alongside.
//
// Same two-step split as every other dialog here
// (ui/DustScratchesDialog.hpp's own comment has the reason in full):
// `performMenuAction()` calls `requestPreferencesDialog()`, which only sets a
// flag, because a native menu's AppKit callback has no ImGui frame in progress
// to open a popup from; `drawPreferencesDialog()` runs once per frame inside
// the frame and is what actually opens it.
namespace np {

struct AppState;

void requestPreferencesDialog();
void drawPreferencesDialog(AppState& st);

// Applies `st.uiPreferences.uiScale` to ImGui's font and style, but only when
// it differs from `st.uiScaleApplied` -- see that field's comment for why
// re-applying every frame would compound. Called once per frame from the UI's
// top level, so a scale loaded from disk takes effect on the first frame
// rather than only after the Preferences window has been opened.
void applyUiScaleIfChanged(AppState& st);

// Same shape as `applyUiScaleIfChanged()` above, for `st.uiPreferences.theme`
// against `st.themeApplied`: pushes the theme into ImGui's style only when it
// differs, and is called once per frame from the same top-level spot so a
// theme loaded from disk (or changed via the System option tracking the OS)
// takes effect without the Preferences window ever having been opened.
void applyThemeIfChanged(AppState& st);

}  // namespace np
