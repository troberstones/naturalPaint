#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "app/AppState.hpp"
#include "gfx/Context.hpp"
#include "sim/PaintSim.hpp"
#include "ui/DocumentTexture.hpp"

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

// The canvas's document-composite texture (UI detour step 2), for reading its
// counters. It is file-scope inside ui/MacPaintUI.cpp because two places in
// that file need the same instance; this accessor exists so that main.cpp can
// report what the revision cache actually saved over a real session's frames,
// which is a different measurement from `--selftest`'s benchmark of the same
// code -- a benchmark shows the composite is expensive, a session shows how
// rarely it was paid.
//
// The layers panel shows the same numbers on screen, but the controls column
// scrolls and that section can sit below the fold at small window sizes, so
// the shutdown line is the one that is always observable.
const DocumentTexture& canvasDocumentTexture();

// Which layer the LAYERS panel and the `Layer` menu act on, as an index into
// `Document::layers` (bottom-first), never a panel row.
//
// The selection is UI state and lives in ui/MacPaintUI.cpp with the rest of it;
// this setter exists for one caller, main.cpp's `--ui-layer-demo`, which drives
// the editor's own commands and would otherwise leave the panel expanded on
// whichever layer the selection happened to start on. Clamped by the panel
// itself on the next frame, so an index past the end is not an error here.
void setLayersPanelSelection(size_t layerIndex);

// The sentence the COMPS panel shows under its list after a restore --
// `core::layerCompRestoreSummary()`'s, naming what the restore could not do.
//
// Set by the panel itself whenever its own Restore button is pressed. This
// setter exists for the same one caller `setLayersPanelSelection()` does,
// main.cpp's `--comps-demo`, which restores through the same recorded funnel
// before the first frame is drawn and would otherwise leave the panel with
// nothing to say about a restore that has already happened. Empty clears it.
void setCompsPanelRestoreSummary(std::string summary);

}  // namespace np
