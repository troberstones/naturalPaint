#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "core/LayerSetOps.hpp"
#include "gfx/Context.hpp"
#include "sim/PaintSim.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/MenuModel.hpp"
// For `SDL_SystemCursor`, the return type of `canvasCursorRequest()` below.
// ui/ToolCursor is the module that owns what the cursor means; this header only
// carries one of its values across.
#include "ui/ToolCursor.hpp"

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

// What the canvas wants the mouse pointer to be **this frame**, or `nullopt`
// when the pointer is not over it.
//
// The same file-scope-plus-accessor shape `canvasDocumentTexture()` above uses,
// and for the same reason: the value is produced deep inside `drawUI()`'s
// canvas block, where `hovered`, the active layer and the in-flight gesture are
// all in scope, and it is consumed somewhere else entirely.
//
// **It is a request, not an application.** ui/ToolCursor §6 makes this build
// the only writer of the cursor -- the ImGui SDL3 backend is suppressed with
// `ImGuiConfigFlags_NoMouseCursorChange` -- and being the only writer is worth
// nothing if the writing happens in two places. `SystemCursorTable::apply()`
// in main.cpp is the one caller, once a frame; everything here does is say
// what the canvas would like.
//
// Cleared at the top of every `drawUI()`, so a frame in which the pointer left
// the canvas answers `nullopt` rather than the last frame's tool -- which is
// exactly the stale-cursor failure suppressing the backend was meant to end.
std::optional<SDL_SystemCursor> canvasCursorRequest();

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

// The status line beside the menus -- the one every document operation (open,
// save, revert, import, close) already leaves its result in.
//
// Exposed for main.cpp's drag-and-drop handler. A drop is an SDL *event*, so it
// is handled in the event loop with the other events, and it has to be able to
// report what it did; the alternative is a second status line somewhere else on
// screen saying the same kind of thing, which is how an application ends up
// with two places to look for one answer.
//
// Multi-line is expected: the status line draws the first line and shows the
// whole string as its tooltip, which is how a twelve-file drop's per-file
// refusals stay reachable without a dialog. Empty clears it.
void setDocumentStatusLine(std::string status);

// The tab strip's split arrangement, as if one of its two icons (docs/ui.md
// section 5's `columns-2` and `layout-grid`) had been clicked.
//
// `AtelierSplitState` is file-scope inside ui/MacPaintUI.cpp for the same
// reason the texture pool above is -- two places in that file need the same
// instance -- and this is the same one-caller wrapper the four setters above
// are. The caller is main.cpp's `--split-demo`, which needs the split already
// on before the first frame, because `--screenshot` photographs a frame
// nobody clicked in and there is no other route into `splitActive`: PLAN.md
// Phase 5 step 14 shipped the two-pane drawing compile-verified only, for
// exactly this reason.
//
// **It sets the arrangement and nothing else, because that is all the click
// handler sets.** The companion document and which pane holds the focus are
// re-derived from the session by `atelierPaneDocuments()` on every frame (see
// ui/AtelierChrome.hpp for the rule), so a setter that also wrote them would
// be photographing a state no click can produce -- which is precisely what
// `setLayersPanelSelection()`'s comment exists to prevent.
void setSplitArrangement(AtelierSplit mode);

// Where all five of PRD E3's selection tools end: the shape just drawn,
// combined with what was installed through the PRD E7 modifier the gesture
// latched (`AppState::marqueeCombine`), installed once.
//
// **Exposed for --selftest, and that is the whole reason it is in this
// header.** The three rules it encodes are about user *intent* rather than
// arithmetic -- what an empty gesture means, and what a refinement of nothing
// means -- and each is the kind of rule that inverts without producing a wrong
// pixel anywhere. Inside the mouse handler no test could reach them; the
// alternative was five copies none of which could be checked.
//
// `drawn` absent means the gesture produced NO SHAPE (a click with no drag, a
// two-point lasso, a wand on a layer it cannot read). That is distinct from a
// shape covering nothing, and the two get different answers.
void commitDrawnSelection(AppState& st, OpenDocument& od,
                          const std::optional<Selection>& drawn);

// **The one place a history cursor actually moves**, direction `-1` for undo
// and `+1` for redo (D1, docs/reachability-audit.md). Settles wet paint
// first when `sim` is non-null (app/StrokeBake.hpp section 4 -- an undo must
// not leave paint on screen that no state in the history actually holds),
// then moves `od.history`'s cursor and installs the document at the new
// position. The HISTORY panel's buttons, the title bar's, the Edit menu's
// Undo/Redo and ⌘Z/⇧⌘Z (via `AppState::requestUndo`/`requestRedo`) all call
// this and nothing else, so none of the four can drift from what the others
// do.
//
// **Exposed for --selftest, and that is the whole reason it is in this
// header** -- `commitDrawnSelection()`'s comment above says why the pattern
// exists. With `sim` left null (the default-constructed, idle state
// ADR-0001 already assumes) this settles nothing and only moves the cursor,
// which is exactly what a headless test needs to assert that undo and redo
// reach the same implementation: the observable state a `History` ends up
// in, not which function's name appears in a call stack.
//
// Callers are expected to have already checked `History::canUndo()` /
// `canRedo()`; this does not re-check.
void moveHistoryCursor(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
                       OpenDocument& od, int direction);

// The Goodies menu's tool family, exactly as `menuContextFromState()` builds
// `MenuContext::tools` (A4, docs/reachability-audit.md). Factored out of that
// function into its own name for two reasons: it is the one piece of
// `menuContextFromState()` that has no `AppState&` dependency beyond the
// current tool, and it is the piece that had the bug -- `enabled` was
// unconditionally `true`, so all 27 tools were selectable from this menu
// while `toolButton()` (ui/AtelierChrome.cpp) correctly gated the same list
// one panel over. `toolImplemented()` is the single predicate both now share.
//
// **Exposed for --selftest, and that is the whole reason it is in this
// header.** `menuContextFromState()` itself is not: its first call loads
// `st.recentDocuments` from the user's real preferences file
// (app/DocumentLifecycle.hpp), which is exactly the file `--selftest` must
// never touch (`RecentDocuments`'s own header says why). This function reads
// nothing and touches no disk, so `app/selftest/MenuBasics.cpp` can call it
// directly to prove the Goodies menu enables exactly the implemented tools --
// counted against `toolImplemented()`, never a literal number, so the
// assertion stays true as tools ship.
std::vector<MenuFamilyEntry> toolMenuFamily(Tool current);

// The foreground colour as STRAIGHT LINEAR RGBA -- what the paint bucket (PRD
// D25/D26) and the gradient (D24) both need, and what neither can be handed
// directly.
//
// **`paint/Palette`'s `rgb` is display-referred sRGB, not linear.** It is
// drawn straight into an 8-bit swatch and handed raw to the Mixbox LUT, whose
// API is sRGB, so both of its existing consumers want it encoded. The two ops
// here want the opposite, because this build's working space is linear
// (DESIGN-imaging.md), and ops/Gradient.hpp puts the conversion explicitly on
// the caller: "a colour picked from an sRGB swatch must be decoded by whoever
// builds the stop list, not here".
//
// Exposed rather than left inline because the failure is silent and plausible:
// omit the decode and every fill lands far darker than the swatch that was
// clicked, which reads as a colour-management bug somewhere else entirely
// rather than as a missing one-line conversion. Alpha is always 1.0 -- the
// foreground well has no opacity of its own; the gradient's fade lives in its
// opacity stops and the bucket's in its `opacity` argument.
//
// An out-of-range index yields opaque black rather than reading past the
// palette.
std::array<float, 4> foregroundLinearRgba(int pigmentIndex);

}  // namespace np
