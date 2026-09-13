#pragma once

// ui/FillDialog -- PRD D26's three modals: Edit > Fill..., Edit > Stroke...
// and Edit > Define Pattern..., split out of ui/MacPaintUI.cpp for the reason
// ui/NewDocumentDialog.hpp's own header gives: that file is 23k lines, other
// tracks edit it concurrently, and a dialog belongs in a new file with only a
// small hook (a request call, a draw call) left behind in it.
//
// Follows the identical two-step split every deferred menu item in this
// build uses: `performMenuAction()`'s `MenuAction::Fill`/`Stroke`/
// `DefinePattern` cases (ui/MacPaintUI.cpp) call `requestFillDialog()` etc.,
// which only set a flag -- a native menu's AppKit callback has no ImGui frame
// in progress to call `ImGui::OpenPopup()` from. `drawFillDialog()` etc. are
// called once per frame from inside the frame and are what actually open the
// popups.
//
// Fill and Stroke share one "what am I painting with" block (colour, pattern
// or gradient, a blend mode, an opacity) -- `drawFillSourceControls()` and
// `resolveFillParams()` in ui/FillDialog.cpp draw and resolve it once, so the
// two dialogs cannot describe that block two different ways.
namespace np {

struct AppState;

void requestFillDialog();
void drawFillDialog(AppState& st);

void requestStrokeDialog();
void drawStrokeDialog(AppState& st);

void requestDefinePatternDialog();
void drawDefinePatternDialog(AppState& st);

}  // namespace np
