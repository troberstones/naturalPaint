#pragma once

#include "app/AppState.hpp"

namespace np {

// ui/TaperPanel -- the two stroke tapers (`brush/Taper.hpp`,
// `NativeBrush::taperIn`/`taperOut`) as one control each, in the two places a
// painter looks for them: the brush Tool Options bar and the Brush Settings
// window's NATURALPAINT group. Per-brush state, so nothing here saves a
// preferences file -- a preset carries these (`app/UserBrushLibrary.cpp`'s
// `taperin`/`taperout` keys).

// The options bar's field: a `BeginCombo` styled like every other field in
// the band, whose popup is the same contents the Brush Settings group draws.
// The FIELD only, no label -- `capsLabel("TAPER")` lives in
// `AtelierChrome.cpp`, so the caller draws it and `SameLine()`s into this,
// exactly as it does for STABILISER and every other band field.
void drawTaperOptionsBarField(AppState& st);

// Both tapers, one switchable group each, for `drawBrushNativeGroup()`.
void drawBrushTaperControls(AppState& st);

}  // namespace np
