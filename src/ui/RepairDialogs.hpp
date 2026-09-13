#pragma once

// ui/RepairDialogs -- two dialogs (Content-Aware Fill, PRD
// D7's second half; Seam Heal, PRD D8), split out of ui/MacPaintUI.cpp for
// the reason ui/FilterDialogsExtra.hpp's own header gives: that file is 23k
// lines, other tracks edit it concurrently, and a dialog belongs in a new
// file with only a small hook (a request call, a draw call) left behind in
// it.
//
// Same two-step split every deferred menu item uses: `performMenuAction()`'s
// `MenuAction::ContentAwareFill`/`SeamHeal` cases (ui/MacPaintUI.cpp) call
// `requestContentAwareFillDialog()`/`requestSeamHealDialog()`, which only set
// a flag; `drawContentAwareFillDialog()`/`drawSeamHealDialog()` are called
// once per frame from inside the frame and are what actually open the
// popups.
//
// Both get the live canvas preview through `setExternalFilterPreview()`/
// `clearExternalFilterPreview()` (ui/MacPaintUI.hpp), exactly as
// ui/FilterDialogsExtra.hpp's three do, and both carry a "Recompute" button
// (ops/PatchMatch.hpp's search is a function of its seed as well as its
// pixels, so the same request can legitimately want a second roll).
namespace np {

struct AppState;

void requestContentAwareFillDialog();
void drawContentAwareFillDialog(AppState& st);

void requestSeamHealDialog();
void drawSeamHealDialog(AppState& st);

}  // namespace np
