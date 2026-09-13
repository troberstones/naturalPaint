#pragma once

// ui/ShadowsHighlightsDialog -- Image > Adjustments > Shadows/Highlights
// (PRD D12), split out of ui/MacPaintUI.cpp for ui/FilterDialogsExtra.hpp's
// own reason (that file is 23k lines and other tracks edit it concurrently).
//
// Unlike ui/DustScratchesDialog.hpp's Filter-menu pair, this dialog is
// opened through `AppState::requestAdjustment`/`AdjustmentRequest`
// (app/AppState.hpp), the same door every other Image > Adjustments dialog
// uses -- `ui/MacPaintUI.cpp`'s `serviceAdjustmentRequest()` already calls
// `ImGui::OpenPopup("Shadows/Highlights")` for
// `AdjustmentRequest::ShadowsHighlights`, so there is no `request*Dialog()`
// function of its own here to set a flag: `drawShadowsHighlightsDialog()`
// only needs to be called once per frame from `drawAdjustmentDialogs()`.
namespace np {

struct AppState;

void drawShadowsHighlightsDialog(AppState& st);

}  // namespace np
