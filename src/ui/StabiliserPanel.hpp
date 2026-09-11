#pragma once

#include "app/AppState.hpp"

namespace np {

// ui/StabiliserPanel -- the Stabiliser popover (brush Tool Options bar) and
// the per-brush controls repeated in the Brush Settings window's own
// NATURALPAINT group. Saves `stroke-preferences.txt` on every edit; the load
// itself is `app/StrokePreferences.hpp`'s `ensureStrokePreferencesLoaded()`,
// not this file's -- it has to be reachable from the pen-down path too, not
// only from a popover that may never be drawn.

// The popover: a button plus a popup with "All brushes" (the global
// setting), "This brush" (follow x amount / off / own), the resolved
// effective setting as one line, and a plain-voice note on where it does not
// apply. The one call `drawBrushToolOptionsGroup()` makes into this file.
void drawStabiliserPopover(AppState& st);

// The "This brush" controls alone, for `drawBrushNativeGroup()`.
void drawPerBrushStabiliserControls(AppState& st);

}  // namespace np
