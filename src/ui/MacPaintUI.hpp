#pragma once
#include "app/AppState.hpp"
#include "gfx/Context.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Lays out the whole window: menu bar, tool palette on the left, pigment strip
// along the bottom, solver controls on the right, canvas in the middle.
// Also folds canvas input into `state` (stroke position, pressure, pan/zoom).
void drawUI(AppState& state, PaintSim& sim, GpuContext& gpu);

}  // namespace np
