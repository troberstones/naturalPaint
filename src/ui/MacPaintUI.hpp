#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "core/LayerSetOps.hpp"
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

// The canvas's document-composite textures (UI detour step 2; a pool of at
// most `kVisibleDocumentCap` of them since PLAN.md Phase 5 step 14), for
// reading their counters. It is file-scope inside ui/MacPaintUI.cpp because two places in
// that file need the same instance; this accessor exists so that main.cpp can
// report what the revision cache actually saved over a real session's frames,
// which is a different measurement from `--selftest`'s benchmark of the same
// code -- a benchmark shows the composite is expensive, a session shows how
// rarely it was paid.
//
// The layers panel shows the same numbers on screen, but the controls column
// scrolls and that section can sit below the fold at small window sizes, so
// the shutdown line is the one that is always observable.
const DocumentTexturePool& canvasDocumentTexture();

// Which layer the LAYERS panel, the `Layer` menu and **the brush** act on.
//
// It is `OpenDocument::activeLayer` now, not a file-scope index in
// ui/MacPaintUI.cpp -- see that member's own comment for why it lives on the
// document, and app/StrokeSession.hpp section 4 for what it unblocked. This
// wrapper exists for one caller, main.cpp's `--ui-layer-demo`, which drives
// the editor's own commands and would otherwise leave the panel expanded on
// whichever layer the selection happened to start on; it sets the active layer
// *and* collapses the multi-selection onto it, which `setActiveLayer()` alone
// does not do. Clamped, so an index past the end is not an error here.
void setLayersPanelSelection(OpenDocument& doc, size_t layerIndex);

// The whole multi-selection (PLAN.md Phase 5 step 11), for the one caller
// `setLayersPanelSelection()` exists for: main.cpp's `--ui-multiselect-demo`,
// which presses the set commands directly and would otherwise leave the panel
// photographing a single-row selection while the terminal reported a gesture
// over three. The primary row follows the lowest member, exactly as a click
// does. An empty selection is ignored -- the panel never has one.
void setLayersPanelSelectionSet(OpenDocument& doc, const LayerSelection& selection);

// What the LAYERS panel shows under the stack after an operation: the refusal
// sentence when one was refused, and core/Merge's warnings when one went ahead
// and cost something (PLAN.md Phase 5 step 10).
//
// Same shape and same one reason as the setter above: main.cpp's
// `--ui-merge-demo` calls `app::applyLayerCommand()` directly rather than
// through this file's `runLayerCommand()`, so without this the panel would be
// photographed showing nothing while the terminal showed the sentence. It sets
// exactly what a button press would have set, so the screenshot is of the
// state a click produces and not of a second one invented for it.
void setLayersPanelMessages(std::string error, std::vector<std::string> warnings);
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
