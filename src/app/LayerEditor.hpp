#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/LayerSetOps.hpp"
#include "core/OpStack.hpp"

// app/LayerEditor (UI detour step 3, problem 2: "five built features have no
// entry point").
//
// core/LayerOps has had `makePigmentLayer()`, `makeAdjustmentLayer()`,
// `addLayerMask()`/`removeLayerMask()` and, as of this step, five op-stack
// operations. Every one of them was tested and none of them could be reached
// from the running application: the panel offered "+ Add", which made an RGB
// layer, and nothing else. This file is the list of gestures the application
// offers over a layer stack, and the one place that turns a gesture into an
// edit.
//
// --- Why a command list and not two sets of button handlers ---------------
//
// The same gestures now appear twice on screen -- in the `Layer` menu on the
// main menu bar and as buttons in the LAYERS panel -- and there is exactly one
// implementation of each. A menu item and a panel button that each called
// core/LayerOps themselves would be two chances to get the insertion index,
// the selection bookkeeping or the recording wrong, and one of the two would
// be the one nobody screenshots.
//
// `--selftest` cannot check how a control looks; it can check what a control
// *does*, and this is the surface it checks. Every assertion about "the New
// Pigment Layer item makes a Pigment layer above the selection and selects it"
// is an assertion about `applyLayerCommand()`.
//
// --- The rule that makes the panel honest ---------------------------------
//
// **Every mutation goes through `recordLayerEdit()`**, which is what makes it
// an `EditKind::Structural` edit on the open document: it bumps
// `OpenDocument::revision`, appends a `core::History` entry (so undo takes it
// back) and hands app/Journal a structural change to write.
//
// That is not only bookkeeping. ui/DocumentTexture caches the composite by
// `revision`, so **a mutation that bypasses `recordLayerEdit()` does not reach
// the screen**: the tile is written, the cache sees the same revision it saw
// last frame, and the canvas keeps showing the previous composite. The one
// place in the *running application* that writes tiles outside this funnel is
// main.cpp's `--demo-document` fixture, which calls `recordEdit()` by hand for
// exactly this reason (`--selftest` writes them directly too, and asserts this
// property with them). There is no path here that can trip it, because there is
// no path here that touches `doc.layers` directly.
//
// --- What is here and what stays in the panel -----------------------------
//
// A `LayerCommand` is a gesture with no value attached: a menu item or a
// button. The controls that carry a value -- the opacity slider, the blend
// dropdown, the rename field, the op params editors -- stay in
// ui/MacPaintUI.cpp and call core/LayerOps' setters directly through the same
// `recordLayerEdit()` funnel, because there is nothing for this file to add to
// them but a second signature.
namespace np {

// One gesture the layer editor offers. Ordered as the `Layer` menu presents
// them: creation, then the whole-layer operations, then the mask, then the
// per-layer flags.
enum class LayerCommand {
  // PRD C16: a new layer is an ordinary layer, inserted directly above the
  // selection, which is where every editor puts one.
  NewRgbLayer,
  // PLAN.md Phase 5 step 3. Latent-times-mass tiles, empty.
  NewPigmentLayer,
  // PLAN.md Phase 5 step 5. No tile storage at all: its op stack is its
  // content, so a fresh one is an exact no-op until an op is added to it.
  NewAdjustmentLayer,
  DuplicateLayer,
  DeleteLayer,
  MoveLayerUp,
  MoveLayerDown,
  // PLAN.md Phase 5 step 4. "Reveal all", which costs no allocation.
  AddMask,
  RemoveMask,
  ToggleVisible,
  ToggleLocked,
  // PLAN.md Phase 5 step 9 / PRD C9.
  ToggleClipped,
  // PLAN.md Phase 5 step 10 / PRD C10 (P0) and C11 (P1) -- core/Merge. Listed
  // here rather than wired into the menu separately for this file's own
  // reason: `allLayerCommands()` is what both the `Layer` menu and the LAYERS
  // panel walk, so a merge added anywhere else would be a merge only one of
  // them offered. They sit after the flags because that is the order the menu
  // presents them in -- creation, whole-layer operations, mask, flags, then
  // the operations that consume layers.
  MergeDown,
  MergeVisible,
  StampVisible,
  FlattenImage,
  RasteriseLayer,
  // PLAN.md Phase 5 step 12 / PRD C14: capture the whole stack's current
  // visibility and properties as a named comp.
  //
  // **The only comp gesture that belongs in this list, and the boundary is this
  // file's own rule.** A `LayerCommand` is "a gesture with no value attached";
  // capture is exactly that -- it takes the state that is already on screen and
  // names it `core::defaultNewCompName()`, the way `NewRgbLayer` takes
  // `core::defaultNewLayerName()`. Restore, rename, delete and reorder all
  // carry a *comp index*, which this signature has no room for and should not
  // grow one for: `applyLayerCommand()`'s `selected` is an index into
  // `Document::layers`, and overloading it to sometimes mean an index into
  // `Document::comps` is precisely the kind of double meaning that produces an
  // operation on the wrong list. They live in the COMPS panel and call
  // core/LayerCompOps through the same `recordLayerEdit()` funnel, which is
  // where the opacity slider and the blend dropdown already are.
  CaptureComp,
};

// Every command, in menu order. Used by the menu, by the panel and by the
// test, so a command added to the enum without a menu entry fails the test
// rather than being quietly unreachable -- which is the exact failure this
// whole file exists to fix.
const std::vector<LayerCommand>& allLayerCommands();

// The menu text: "New Pigment Layer", "Add Layer Mask". Title case, because
// these are menu items; the panel's buttons abbreviate them itself.
const char* layerCommandLabel(LayerCommand command) noexcept;

// The compact icon a toolbar button draws for this command, UTF-8, or `""`
// for a command with no icon distinct enough to earn one (`CaptureComp` --
// COMPS has its own panel and this list is LAYERS') -- never a fallback
// string, which is ui/Fonts' job exactly as it is for `layerKindGlyph()`.
//
// The three creation commands return **the exact glyph the layer they create
// will carry** -- `app/LayerPanel`'s `layerKindGlyph()` -- so the icon a user
// clicks and the kind glyph on the row it produces are the same mark, not two
// marks that happen to agree today. The rest have no row of their own to
// borrow from, so this is their only source, and every one of them is a
// codepoint ui/Fonts.cpp's Menlo merge was measured to draw before it was
// picked (`--selftest fonts` asserts the coverage, not this file).
const char* layerCommandGlyph(LayerCommand command) noexcept;

// Whether the command can be offered at all for `selected` -- what a menu item
// greys itself out on.
//
// **This is availability, not permission.** A command is unavailable when the
// gesture makes no sense on this row at all: Delete with no layers, Move Up on
// the top layer, Remove Layer Mask on a layer with no mask, Clip on the bottom
// layer. Everything else -- the lock, a clip with no alpha below it, a `Mix`
// pair -- is a *refusal*, and a refusal is offered, attempted and answered
// with core/LayerOps' own sentence naming the layer and what to do about it.
// Greying those out instead would replace a sentence that explains with a
// control that silently does nothing, which is the same trade docs/ui.md
// rejects for the Clip checkbox's own tooltip.
bool layerCommandAvailable(const Document& doc, LayerCommand command, size_t selected);

struct LayerEditResult {
  bool ok = false;
  // core/LayerOps' own refusal sentence, verbatim. Empty when `ok`.
  std::string error;
  // What went ahead but is worth saying (PLAN.md Phase 5 step 10). Only the
  // five core/Merge commands ever fill this: a merge is the one gesture here
  // that succeeds *and* destroys something -- a mask, an op stack, a layer, a
  // tile that fell outside the canvas -- and core/Merge.hpp §3 argues at
  // length why each of those is reported rather than refused. Empty for every
  // other command, and empty on a refusal.
  std::vector<std::string> warnings;
  // Where the selection ends up: the new layer after a create, the copy after
  // a duplicate, the moved layer after a reorder, the row that took the
  // deleted one's place after a delete, and the unchanged selection after any
  // refusal.
  size_t selected = 0;
};

// Applies one command to the active document's layer stack, records it, and
// reports where the selection went.
//
// `selected` is an index into `Document::layers` (bottom-first), never a panel
// row -- app/LayerPanel owns that reversal and nothing here reverses anything.
// An out-of-range `selected` is not clamped: it is passed to core/LayerOps,
// which refuses it by name with the numbers, because a clamp would silently
// act on a different layer than the one the caller named.
LayerEditResult applyLayerCommand(OpenDocument& doc, LayerCommand command, size_t selected);

// --- The multi-selection entry point (PLAN.md Phase 5 step 11; PRD C12, C13,
//     C15) -------------------------------------------------------------------
//
// **A second entry point rather than a widened `applyLayerCommand()`**, and
// this file's own doc comment above `LayerCommand::CaptureComp` is why: that
// signature's `selected` is an index into `Document::layers` and overloading it
// to mean anything else "is precisely the kind of double meaning that produces
// an operation on the wrong list". A set is not an index, and every existing
// assertion in `--selftest` is about the single-index path, which is unchanged
// here -- not one line of `applyLayerCommand()` moved.
//
// The division of labour is the same one this file already has: core owns the
// rules (core/LayerSetOps: what a set means, what order each verb walks it,
// the all-or-nothing trial), and this function owns the funnel -- one
// `recordLayerEdit()` per gesture, which is what makes a five-layer delete one
// undo instead of five.

struct LayerSetEditResult {
  bool ok = false;
  // core/LayerSetOps' or core/LayerOps' own refusal sentence, verbatim.
  std::string error;
  // What went ahead but is worth saying -- today only an align or a distribute
  // that had to round to whole pixels. Same channel the merge family's warnings
  // already use, so the panel needs no second place to show one.
  std::vector<std::string> warnings;
  // Where the selection ends up; unchanged after a refusal.
  LayerSelection selection;
};

// Applies one set command to the whole selection and records it as **one**
// structural edit. `sel` holds indices into `Document::layers`, never panel
// rows.
LayerSetEditResult applyLayerSetCommand(OpenDocument& doc, LayerSetCommand command,
                                        const LayerSelection& sel);

// The two value-carrying set controls, through the same funnel -- the opacity
// slider and the blend dropdown when more than one row is selected.
LayerSetEditResult applyLayerSetOpacity(OpenDocument& doc, const LayerSelection& sel,
                                        float opacity);
LayerSetEditResult applyLayerSetBlend(OpenDocument& doc, const LayerSelection& sel,
                                      BlendMode mode);

// A new op, as both op-stack editors add one: class PointA, the given kind,
// and **disabled**.
//
// Disabled is the rule PLAN.md Phase 3 step 8 set for the GRADE stack
// ("adding an op should never itself change what's on screen, only enabling it
// should") and this is the one place it now lives, so a per-layer op stack
// cannot drift from the session one. It matters more here than it did there: a
// per-layer op stack composites immediately, so an op that arrived enabled
// would change the canvas the instant a menu was clicked, with default params
// the user has not seen yet.
Op makeNewOp(PointOpKind kind);

}  // namespace np
