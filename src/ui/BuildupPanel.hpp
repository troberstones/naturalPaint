#pragma once

#include "app/AppState.hpp"

namespace np {

// ui/BuildupPanel -- how a pigment deposit builds up where a stroke overlaps
// ITSELF (`brush/Deposit.hpp` §1a), as two independent switches on the brush
// Tool Options bar.
//
// Two switches rather than one three-way choice because the two rules are
// separable and are here to be compared: `saturating` changes the rate at
// every setting, `strokeCeiling` puts a limit on one stroke and does nothing
// at all until Opacity comes down. A painter judging these against another
// application needs all four combinations reachable, which a single enum
// would not give.
//
// Global state, so both write `stroke-preferences.txt` on every edit -- the
// same contract `ui/StabiliserPanel` has, and the same reason: a setting being
// judged by feel across sessions must survive a relaunch.

// The options bar's field, no label: `capsLabel("BUILDUP")` lives in
// `AtelierChrome.cpp`, exactly as it does for STABILISER and TAPER.
void drawBuildupOptionsBarField(AppState& st);

// The two switches themselves, for the field's popup.
void drawBuildupControls(AppState& st);

}  // namespace np
