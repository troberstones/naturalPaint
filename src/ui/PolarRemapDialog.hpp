#pragma once

// ui/PolarRemapDialog -- the Filter menu's Polar Coordinates addition
// (docs/operations.md §3), split out of ui/MacPaintUI.cpp for
// ui/BlurDialogsExtra.hpp's own reason: that file is 23k lines, other tracks
// edit it concurrently, and a dialog belongs in a new file with only a small
// hook (a request call, a draw call) left behind in it.
//
// Same two-step split every deferred menu item in this build uses:
// `performMenuAction()`'s `MenuAction::PolarRemap` case (ui/MacPaintUI.cpp)
// calls `requestPolarRemapDialog()`, which only sets a flag -- a native
// menu's AppKit callback has no ImGui frame in progress to call
// `ImGui::OpenPopup()` from. `drawPolarRemapDialog()` is called once per
// frame from inside the frame and is what actually opens the popup.
//
// Live preview through the same two door functions ui/FilterDialogsExtra.hpp
// already uses (`setExternalFilterPreview()`/`clearExternalFilterPreview()`,
// ui/MacPaintUI.hpp). No canvas handles: unlike Radial Blur, there is no
// centre or amount to drag -- the frame IS the parameter
// (ops/PolarRemap.hpp's own header comment).
namespace np {

struct AppState;

void requestPolarRemapDialog();
void drawPolarRemapDialog(AppState& st);

}  // namespace np
