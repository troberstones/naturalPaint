#pragma once

#include "app/AppState.hpp"

namespace np {

// ui/StabiliserPanel -- Wave 2's Stabiliser popover (brush Tool Options bar)
// and the per-brush controls repeated in the Brush Settings window's own
// NATURALPAINT group. Loads/saves `stroke-preferences.txt` itself, lazily,
// the same shape `ui/MacPaintUI.cpp`'s `ensureUserBrushLibraryLoaded()`/
// `saveUserBrushLibrary()` use for the brush library.

// The popover: a button plus a popup with "All brushes" (the global
// setting), "This brush" (follow x amount / off / own), the resolved
// effective setting as one line, and a plain-voice note on where it does not
// apply. The one call `drawBrushToolOptionsGroup()` makes into this file.
void drawStabiliserPopover(AppState& st);

// The "This brush" controls alone, for `drawBrushNativeGroup()`.
void drawPerBrushStabiliserControls(AppState& st);

}  // namespace np
