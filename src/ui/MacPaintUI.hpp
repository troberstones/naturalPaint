#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/GradientTool.hpp"
#include "core/LayerSetOps.hpp"
#include "gfx/Context.hpp"
#include "sim/PaintSim.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/MenuModel.hpp"
// For `SDL_SystemCursor`, the return type of `canvasCursorRequest()` below.
// ui/ToolCursor is the module that owns what the cursor means; this header only
// carries one of its values across.
#include "ui/MenuModel.hpp"
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

// T14 (docs/testing-issues.md): uploads the live pixel preview for whatever
// Free Transform session `state.transform` currently holds -- a no-op if none
// is active. `drawUI()`'s own canvas block calls this right after a
// `state.transform.beginLayer()`/`beginSelectionPixels()` it raises succeeds;
// it is public because main.cpp's drag-and-drop-a-picture path calls
// `state.transform.beginLayer()` a second time, outside `drawUI()` entirely,
// and needs the identical upload rather than a second copy of it drifting out
// of sync. See ui/TransformPreviewTexture.hpp for what "upload" means here --
// ONE crop, captured once, never per drag frame.
void beginTransformPreview(AppState& state, GpuContext& gpu);

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

// ui/ToolCursor.hpp §7's companion accessor: the SAME frame's request as a
// `ToolCursor` intent rather than a projected SDL shape, so
// `SystemCursorTable::apply()` can ask `shouldUseBitmapCursor()` whether a
// bitmap should win instead. `nullopt` under the identical circumstances
// `canvasCursorRequest()` is, AND on the guide-drag and pan/rotate frames
// where a shape is requested that is not a tool's intent at all -- see
// `g_canvasBitmapTool`'s own comment in ui/MacPaintUI.cpp.
std::optional<Tool> canvasCursorToolRequest();

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
// **`documentOpen` is the second axis (app/ToolSurface, docs/testing-issues.md
// T5), and it is here because A4 is the precedent for it.** With no document
// open, fifteen of the twenty-one built tools have nothing to act on --
// `sim::PaintSim`'s canvas is a real, paintable surface and is not a document
// -- and `MenuAction::ToolItem`'s handler calls `setActiveTool()`
// unconditionally, relying entirely on the `enabled` flag this function sets.
// So a Goodies menu that ignored the surface would be A4's own defect wearing
// the other axis: a third live route to a tool the palette and the flyout both
// correctly disable. It stays a parameter rather than an `AppState&` for the
// reason the next paragraph gives.
//
// **Exposed for --selftest, and that is the whole reason it is in this
// header.** `menuContextFromState()` itself is not: its first call loads
// `st.recentDocuments` from the user's real preferences file
// (app/DocumentLifecycle.hpp), which is exactly the file `--selftest` must
// never touch (`RecentDocuments`'s own header says why). This function reads
// nothing and touches no disk, so `app/selftest/MenuBasics.cpp` can call it
// directly to prove the Goodies menu enables exactly the implemented tools --
// counted against `toolImplemented()`, never a literal number, so the
// assertion stays true as tools ship -- and `app/selftest/ToolSurface.cpp` can
// call it with `documentOpen = false` to prove the same of the second axis.
std::vector<MenuFamilyEntry> toolMenuFamily(Tool current, bool documentOpen);

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

// The same, for **whichever** of the two colours `BrushState::colorMode`
// currently selects -- `app/StrokeSession`'s `foregroundSrgb()`, decoded.
//
// Both overloads exist on purpose. The index form above is the *palette*
// question ("what linear colour is row 6"), which `--selftest` walks over every
// row and which nothing about the eyedropper changes; this form is the
// *foreground* question ("what colour will the next fill actually use"), which
// is the one every call site in the running application wants. Collapsing them
// into one would have made the palette test un-writable without a BrushState.
std::array<float, 4> foregroundLinearRgba(const BrushState& brush);

// The exact `ImGuiColorEditFlags` the COLOR panel's RGB picker is drawn with.
//
// **A function rather than a literal at the call site, so `--selftest` can
// reach one bit of it.** `ImGuiColorEditFlags_HDR` is what stops the picker's
// numeric row clamping `BrushState::rgb` back into `[0,1]` on the first drag
// after an over-range pick (`ColorEdit4()`: `DragFloat(..., 0.0f, hdr ? 0.0f
// : 1.0f, ...)`, and a DragFloat whose min equals its max is unbounded). That
// makes it load-bearing for T25a's whole contract -- and invisible to both
// harnesses this project has: `--selftest` has no ImGui frame, and the clamp
// only fires while `g.ActiveId` is the drag (`DragBehavior()`'s own guard), so
// a golden screenshot with no input is byte-identical with the flag and
// without it. Measured, not assumed: removing the flag was sabotaged and the
// `color_overrange` view passed unchanged.
//
// So the flag set is named here and `--selftest` asserts the bit is in it.
// That is a weak assertion by construction -- it can only fail if someone
// edits this line -- and editing this line is exactly the regression, which
// nothing else in the project would notice.
//
// Returned as `int` rather than `ImGuiColorEditFlags` so this header does not
// have to include `imgui.h`; the two are the same type (imgui.h typedefs it),
// and the one caller and the one test both include imgui themselves.
int rgbColorPickerFlags() noexcept;

// ------------------------------------------------------- the gradient tool
//
// The gradient tool's live ramp: `gradientToolStops()` fed the foreground
// this `BrushState` currently selects.
//
// **This one line is what makes `app/GradientTool.hpp` § 1's "one function"
// true across the app/ui boundary.** The stop list is built in `app/`, which
// cannot ask what the foreground colour is; the foreground lives in `ui/`,
// which is where all three of the tool's readers live. Without this adapter
// each of those readers would pair the two itself -- three call sites, three
// chances to pass the wrong colour, and a swatch that shows one ramp while
// the canvas takes another. With it there is one expression, called three
// times.
GradientStops currentGradientStops(const BrushState& brush);

// ------------------------------------------------------- the eyedropper
//
// PRD **Q10** (P0). What one eyedropper click *means*, separated from the
// mouse handling that triggers it, for exactly the reason `commitDrawnSelection()`
// above is separated: the rules here are about user intent, each of them
// inverts without producing a wrong pixel anywhere, and inside the canvas block
// no test could reach a single one of them.
struct EyedropperPick {
  // False when the click could not sample anything: no document, or a
  // coordinate whose sample box misses the canvas. The foreground is left
  // exactly as it was.
  bool applied = false;
  // What `probePixel()` returned, whether or not it was applied -- the
  // readout half of PLAN.md step 10 ("both linear and display values", PRD D2)
  // is this same struct, so the tool and the future probe readout share one
  // call rather than sampling twice.
  ProbeSample sample;
  // True when the pick moved the COLOR panel from PIGMENT mode to RGB mode.
  // See `report` for why it is allowed to.
  bool switchedToRgbMode = false;
  // True when the sampled texel was brighter than white (or, through the
  // mirrored transfer curves, darker than black) and the foreground therefore
  // now holds a value no swatch in this build can draw --
  // `color/Space.hpp`'s `exceedsDisplayRange()`, asked of what was stored.
  //
  // A field rather than something the caller re-derives, for the same reason
  // `switchedToRgbMode` is one: this is a *state this pick entered*, the
  // options bar has to report it, and a second site recomputing the predicate
  // off `st.brush.rgb` later would be describing whatever the foreground is
  // by then rather than what this pick did to it.
  bool overRange = false;
  // One sentence for the options bar, in `app/StrokeSession`'s refusal voice:
  // what was picked, and -- the cases that need saying -- that PIGMENT mode
  // was left behind because a sampled triple has no physical constants, and
  // that the value is above what the swatch beside it can draw.
  std::string report;
};

// Samples the **active document** at `at` with `st.eyedropper`'s settings and
// writes the result into `st.brush`'s foreground colour. Returns what happened.
//
// The document is read off `st.documents` rather than passed in, deliberately:
// the sample's `activeLayerIndex` has to be *that document's* `activeLayer`
// (`ProbeSource::CurrentLayer` reads it and `ActiveAndBelow` stops at it), and
// a caller free to pass one document with another's active index is a
// mismatch nothing would catch. With no document open, `applied` is false and
// `report` says so.
//
// **Picking while COLOR is in PIGMENT mode switches the panel to RGB mode**,
// and that is the decision the whole tool turned on. A pigment is a colour plus
// density, staining and granulation measured off a real paint; three floats off
// a canvas cannot supply those. The three alternatives and why they lost:
//
//   * *Snap to the nearest palette pigment.* Rejected outright. An eyedropper
//     exists to reproduce a colour exactly, and one that answered "Burnt
//     Sienna" to a sampled #7f3f00 is wrong in the single way this tool must
//     never be wrong -- and silently, since the swatch would look about right.
//   * *Refuse to pick in PIGMENT mode.* Rejected because PIGMENT is the
//     **default** mode, so the tool would do nothing at all out of the box:
//     the same silent no-op this whole track exists to remove, wearing a
//     different hat.
//   * *Keep the pigment selected and quietly paint the RGB colour.* Rejected
//     as the worst of the three -- the panel would go on showing a pigment
//     name and three constants for a colour that is no longer that pigment.
//
// So the mode moves, and the user is told twice: the COLOR panel's accent
// visibly jumps from PIGMENT to RGB, and `report` says it in words. The pigment
// selection itself is **not** cleared -- switching back to PIGMENT mode
// restores exactly the paint that was selected before the pick, and the three
// physical constants keep coming from it in the meantime
// (`foregroundPhysicalConstants()`).
//
// A sample with zero alpha -- nothing painted there in this sample source --
// is **not applied**. Writing transparent black into the foreground would
// destroy the user's colour in exchange for a value they cannot have meant to
// pick, and "I clicked on empty canvas" is not an instruction to paint in
// black.
EyedropperPick applyEyedropperPick(AppState& st, PixelCoord at);
// ---------------------------------------------------------------------------
// The Select menu (docs/reachability-audit.md C5; PRD E4/E8/E9) -- exposed
// for --selftest for the identical reason `commitDrawnSelection()` above is.
// ---------------------------------------------------------------------------
//
// The ImGui popups around these (ui/MacPaintUI.cpp's drawSelectMenuDialogs())
// cannot run headless -- there is no window, no frame, nothing for
// `ImGui::BeginPopupModal()` to draw into. What CAN run headless, and what
// app/selftest/SelectMenu.cpp actually needs proven, is the boundary between
// "what the dialog holds" and "what the engine sees": that
// `MenuAction::SelectGrow` reaches `growSelection()` and not
// `shrinkSelection()`, that the radius on screen is the radius the engine
// receives rather than a hardcoded default, and that colour/luminance range
// decode and forward their sliders rather than falling back to
// `SelectionRangeParams{}`'s defaults. These six functions ARE that boundary
// -- every popup's confirm button calls exactly one of them and nothing else,
// so a test that calls them the same way the button does is testing the real
// wiring and not a re-implementation of it.

// The enable predicate for Grow, Shrink and Feather: an ENGAGED selection.
// All three take a `const Selection&` (core/SelectionRefine.hpp,
// ops/Feather.hpp), not a `const Selection*`, so there is no way to hand them
// "no restriction" -- `od.selection` must `has_value()`.
bool selectRefineEnabled(const OpenDocument& od) noexcept;

// The enable predicate for Colour Range and Luminance Range: an RGB layer to
// sample. Both take a `const TileStore&`, not a `Selection` at all (PRD E9),
// so unlike the three above they need NO selection already drawn.
bool selectRangeEnabled(const OpenDocument& od) noexcept;

// The enable predicate for `MenuAction::SelectUndoRefine`: the stack this
// file's own `installRefinedSelection()` pushes to is non-empty.
bool selectUndoRefineEnabled(const OpenDocument& od) noexcept;

// The dialog -> engine boundary for Grow, Shrink and Feather. `action` picks
// the engine function -- `SelectGrow` to `growSelection()`, `SelectShrink` to
// `shrinkSelection()`, `SelectFeather` to `featherSelection()` -- and
// `radius` is passed through exactly as the dialog's slider holds it. Any
// other `action` returns `current` unchanged; the three popups this backs
// never pass one.
Selection applySelectRefineAction(MenuAction action, const Selection& current, float radius);

// The dialog -> engine boundary for Colour Range. `swatchSrgb` is the
// dialog's `ImGui::ColorEdit3` value -- display-encoded sRGB, matching
// `foregroundLinearRgba()`'s own input above -- and is decoded to STRAIGHT
// LINEAR here, the one boundary core/SelectionRefine.hpp asks for, rather
// than at every call site. `tolerance`/`edgeBand` are the dialog's own
// sliders, forwarded into a `SelectionRangeParams` rather than left at that
// struct's defaults.
Selection applySelectColourRangeAction(const std::array<float, 3>& swatchSrgb, float tolerance,
                                       float edgeBand, const TileStore& source, int32_t width,
                                       int32_t height);

// The dialog -> engine boundary for Luminance Range. `low`/`high`/`edgeBand`
// are the dialog's own sliders -- display-encoded Rec.709 luminance
// (core/SelectionRefine.hpp), forwarded into a `SelectionLuminanceRange`
// rather than left at that struct's defaults (which select nearly
// everything: 0..1).
Selection applySelectLuminanceRangeAction(float low, float high, float edgeBand,
                                          const TileStore& source, int32_t width, int32_t height);

// Where every one of the six functions above ends up: installs `result` as
// `od.selection` (through `installSelection()`, so the revision bump and the
// existing `lastDeselected` bookkeeping happen exactly once) and pushes what
// it REPLACED onto `od.refineUndoStack` first. See that member's own comment
// (app/DocumentLifecycle.hpp) for why this is a dedicated stack and not
// `core::History`.
//
// Not `installSelection()` itself: that function is also what every
// interactive marquee drag calls, once a frame, for as long as the drag
// lasts -- and a marquee drag is not five hundred refine-undo entries.
void installRefinedSelection(OpenDocument& od, std::optional<Selection> result);

// `MenuAction::SelectUndoRefine`'s body. Pops the most recent entry off
// `od.refineUndoStack` and restores exactly the selection it replaced
// (`std::nullopt` included -- see that member's comment on why "no
// selection" is a real, restorable state and not the same as an
// empty-but-engaged one). Returns false and changes nothing on an empty
// stack, which is the enable predicate above turned into a runtime guard as
// well as a greyed menu item -- a native menu backend re-validates on open,
// but a queued action from before the last validation should still refuse
// quietly rather than pop a stack that emptied out from under it.
bool undoLastRefine(OpenDocument& od);

}  // namespace np
