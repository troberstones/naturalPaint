#pragma once

#include "app/Command.hpp"
#include "ops/PolarRemap.hpp"

// app/PolarRemapCommand -- the encoder for the Filter menu's Polar
// Coordinates addition (docs/operations.md §3), following
// app/BlurCommandsExtra.hpp's own precedent: one small header declaring the
// encoder, implemented beside the reader that validates its keys
// (app/CommandsImage.cpp, next to `doPolarRemap()`) rather than carrying any
// logic here. Its own header rather than a third entry in
// app/BlurCommandsExtra.hpp because a polar remap is not a blur -- that
// header's name says what it is a header of.
namespace np {

Command polarRemapCommand(const PolarRemapParams& p);

}  // namespace np
