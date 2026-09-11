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
// apply. The one call `drawBrushToolOptionsGroup()` makes into this file --
// the Brush Settings window's own look, kept as it was (Wave 2 brief item 4
// only asks for the options bar's copy to match the bar).
void drawStabiliserPopover(AppState& st);

// The same popup contents, behind a field-styled trigger (a `BeginCombo`
// styled like every other field in the band, e.g. TIP's dab picker in
// `ui/AtelierChrome.cpp`) instead of a bare grey `ImGui::Button` -- the
// options bar's copy (`drawAtelierOptionsBarContent()`), which the button did
// not match (Wave 2 brief item 4). Draws the FIELD only, no label --
// `capsLabel("STABILISER")` lives in `AtelierChrome.cpp` (an anonymous-
// namespace helper this file cannot reach), so the caller draws it and
// `ImGui::SameLine()`s into this, exactly as it does for every other band
// field (WET, TIP, ...).
void drawStabiliserOptionsBarField(AppState& st);

// The "This brush" controls alone, for `drawBrushNativeGroup()`.
void drawPerBrushStabiliserControls(AppState& st);

}  // namespace np
