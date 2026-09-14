#pragma once

// ui/DustScratchesDialog -- the Filter menu's Dust & Scratches dialog
// (PRD D11), split out of ui/MacPaintUI.cpp for ui/FilterDialogsExtra.hpp's
// own reason: that file is 23k lines, other tracks edit it concurrently, and
// a dialog belongs in a new file with only a small hook left behind in it.
//
// Same two-step split as every deferred menu item in this build:
// `performMenuAction()`'s `MenuAction::DustScratches` case (ui/MacPaintUI.cpp)
// calls `requestDustScratchesDialog()`, which only sets a flag -- a native
// menu's AppKit callback has no ImGui frame in progress to call
// `ImGui::OpenPopup()` from. `drawDustScratchesDialog()` is called once per
// frame from inside the frame and is what actually opens the popup.
namespace np {

struct AppState;

void requestDustScratchesDialog();
void drawDustScratchesDialog(AppState& st);

}  // namespace np
