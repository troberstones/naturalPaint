#pragma once

#include "app/Command.hpp"
#include "ops/LensBlur.hpp"
#include "ops/RadialBlur.hpp"

// app/BlurCommandsExtra -- encoders for the Filter menu's Radial/Zoom Blur
// and Lens Blur additions (docs/operations.md §2.2, P2), following
// app/FilterCommandsExtra.hpp's own precedent: one small header declaring
// the encoders, implemented beside the readers that validate their keys
// (app/CommandsImage.cpp, next to `doMotionBlur()`) rather than carrying any
// logic here.
namespace np {

Command radialBlurCommand(const RadialBlurParams& p);
Command lensBlurCommand(const LensBlurParams& p);

}  // namespace np
