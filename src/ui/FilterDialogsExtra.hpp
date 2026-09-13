#pragma once

// ui/FilterDialogsExtra -- reach wave, track `zoom`: the Filter menu's three
// additions (Highpass, Local Contrast, Lens Correction; PRD D22), split out
// of ui/MacPaintUI.cpp for the reason ui/FillDialog.hpp's own header gives:
// that file is 23k lines, other tracks edit it concurrently, and a dialog
// belongs in a new file with only a small hook (a request call, a draw call)
// left behind in it.
//
// Same two-step split every deferred menu item in this build uses:
// `performMenuAction()`'s `MenuAction::Highpass`/`LocalContrast`/`LensCorrect`
// cases (ui/MacPaintUI.cpp) call `requestHighpassDialog()` etc., which only
// set a flag -- a native menu's AppKit callback has no ImGui frame in
// progress to call `ImGui::OpenPopup()` from. `drawHighpassDialog()` etc. are
// called once per frame from inside the frame and are what actually open the
// popups.
//
// **Live preview, unlike ui/FillDialog's three.** These three DO get the
// canvas GPU preview `drawGaussianBlurDialog()` and its siblings show
// (ui/MacPaintUI.cpp's `FilterPreviewOwner`/`setFilterPreview()` machinery),
// through the two door functions that file exports for exactly this
// (`setExternalFilterPreview()`/`clearExternalFilterPreview()`,
// ui/MacPaintUI.hpp) -- see those two functions' own comment for why a door
// exists at all rather than each dialog reimplementing the composite loop.
namespace np {

struct AppState;

void requestHighpassDialog();
void drawHighpassDialog(AppState& st);

void requestLocalContrastDialog();
void drawLocalContrastDialog(AppState& st);

void requestLensCorrectDialog();
void drawLensCorrectDialog(AppState& st);

}  // namespace np
