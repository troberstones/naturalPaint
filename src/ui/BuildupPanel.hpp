#pragma once

#include "app/AppState.hpp"

namespace np {

// ui/BuildupPanel -- how a pigment deposit builds up where a stroke overlaps
// itself (`brush/Deposit.hpp` §1a): Build-up or Wash, on the brush Tool
// Options bar.
//
// Global state, so it writes `stroke-preferences.txt` on every edit -- the
// same contract `ui/StabiliserPanel` has, and the same reason: a setting being
// judged by feel across sessions must survive a relaunch.

// The options bar's field, no label: `capsLabel("BUILDUP")` lives in
// `AtelierChrome.cpp`, exactly as it does for STABILISER and TAPER.
void drawBuildupOptionsBarField(AppState& st);

// The choice itself, for the field's popup.
void drawBuildupControls(AppState& st);

}  // namespace np
