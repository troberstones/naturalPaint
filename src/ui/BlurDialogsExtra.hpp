#pragma once

// ui/BlurDialogsExtra -- the Filter menu's Radial/Spin+Zoom Blur and Lens
// Blur additions (docs/operations.md §2.2, P2), split out of
// ui/MacPaintUI.cpp for ui/FilterDialogsExtra.hpp's own reason: that file is
// 23k lines, other tracks edit it concurrently, and a dialog belongs in a new
// file with only a small hook (a request call, a draw call) left behind in
// it.
//
// Same two-step split every deferred menu item in this build uses:
// `performMenuAction()`'s `MenuAction::RadialBlur`/`LensBlur` cases
// (ui/MacPaintUI.cpp) call `requestRadialBlurDialog()`/`requestLensBlurDialog()`,
// which only set a flag -- a native menu's AppKit callback has no ImGui frame
// in progress to call `ImGui::OpenPopup()` from. `drawRadialBlurDialog()`/
// `drawLensBlurDialog()` are called once per frame from inside the frame and
// are what actually open the popups.
//
// Live preview through the same two door functions ui/FilterDialogsExtra.hpp
// already uses (`setExternalFilterPreview()`/`clearExternalFilterPreview()`,
// ui/MacPaintUI.hpp).
#include "app/ViewTransform.hpp"

struct ImDrawList;

namespace np {

struct AppState;
struct RadialBlurParams;

void requestRadialBlurDialog();
void drawRadialBlurDialog(AppState& st);
// While Radial Blur is open, its centre and amount handles over the active
// canvas pane (screen rect paneMin..paneMax), dragged with the pointer.
void drawRadialBlurCanvasHandles(AppState& st, const ViewTransform& view, Vec2 paneMin,
                                 Vec2 paneMax, ImDrawList* dl);
// While Radial Blur is open, true for a click on the other split pane
// (paneMin..paneMax). The caller focuses that pane; the dialog re-centres on
// its document next frame.
bool radialBlurTakesPaneClick(Vec2 paneMin, Vec2 paneMax);
// The parameters the dialog would apply now, for the self-test.
const RadialBlurParams& radialBlurDialogParams();

void requestLensBlurDialog();
void drawLensBlurDialog(AppState& st);

}  // namespace np
