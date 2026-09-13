#pragma once

#include "app/Command.hpp"
#include "ops/Filters.hpp"  // LocalContrastParams
#include "ops/Lens.hpp"     // LensParams

// app/FilterCommandsExtra -- encoders for the three
// Filter-menu additions (Highpass, Local Contrast, Lens Correction) this
// track wires a dialog to.
//
// One header for all three rather than splitting them across
// app/CommandsImage.hpp and a Patterns header that did not exist, because
// `lens_correct` (app/CommandsPatterns.cpp, PRD D22) had NO encoder before
// this track -- nothing outside that file had ever needed to build one of its
// `Command`s -- and it is one row short of a family of its own. Each function
// is still implemented beside the reader that validates its keys
// (app/CommandsImage.hpp's own rule, restated: an encoder writes what it is
// given and lives next to the decoder it must not drift from), so this header
// carries no logic of its own.
namespace np {

Command highpassCommand(float sigma);
Command localContrastCommand(const LocalContrastParams& p);
Command lensCorrectCommand(const LensParams& p);

}  // namespace np
