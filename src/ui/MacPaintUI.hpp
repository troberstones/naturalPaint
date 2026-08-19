#pragma once
#include <cstdint>
#include <memory>

#include "app/AppState.hpp"
#include "gfx/Context.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Lays out the whole window: menu bar, tool palette on the left, pigment strip
// along the bottom, solver controls on the right, canvas in the middle.
// Also folds canvas input into `state` (stroke position, pressure, pan/zoom).
//
// `sim` is null until the first paint-tool stroke actually starts (1.4 /
// ADR-0001: idle costs zero GPU memory), at which point drawUI constructs it
// via ensurePaintSim() using `gpu`/`lut`/`canvasW`/`canvasH`. Every other
// access site here is conditional on `sim` already existing; `canvasW`/
// `canvasH` are handed in separately so cosmetic reads (status text, canvas
// layout) don't need a live sim at all -- they're the same dimensions
// PaintSim gets constructed with.
void drawUI(AppState& state, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH);

}  // namespace np
