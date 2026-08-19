#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"

// app/LayerPanel (PLAN.md "Phase 5 -- Stack it", step 1; docs/ui.md §3.2's
// layer rows).
//
// Pure list mapping and row text, no ImGui and no GPU -- the same split
// app/CurveEdit.hpp already documents and the reason Phase 3 step 8 was
// testable at all. The panel chrome (the window, the buttons, the drag state)
// is ui/MacPaintUI.cpp; everything a `--selftest` can actually check about
// what a row *says* and which layer a row *is* lives here.
//
// --- The one thing this file exists for -----------------------------------
//
// **The panel is upside down relative to the model, and exactly one function
// is allowed to know that.**
//
// `Document::layers` is ordered bottom to top: index 0 is the bottom layer.
// That is not a local convention -- docs/document-format.md fixes it ("Part
// order is layer order, bottom to top, after part 0"), io/NpaintFile writes
// `layers[0]` as the first layer part, and core/Composite walks index 0 first
// so the bottom layer is what everything else lands on.
//
// A layers panel shows the top layer at the top. Every editor does; a panel
// listing the stack bottom-first would read as upside down to anyone who has
// used one. So the panel's row 0 is `layers.back()`, and the mapping is
// `layerIndex = layerCount - 1 - row`.
//
// Both directions live here, they are each other's inverse, and nothing else
// in the codebase reverses a layer index. A second reversal somewhere in the
// draw loop is exactly how "up" ends up moving a layer down, and how a
// round-tripped file comes back with its stack flipped -- which is why the
// round trip through io/NpaintFile is asserted for order, not just for
// contents.
namespace np {

// Panel row (0 = top of the panel = top of the stack) -> index into
// `Document::layers` (0 = bottom of the stack).
//
// `row >= layerCount` returns 0 rather than wrapping through unsigned
// subtraction, which is what an unchecked `count - 1 - row` would do and is the
// one arithmetic accident this whole file exists to prevent. A caller with a
// stale row from a panel whose document shrank gets a valid index, never a
// three-billion-element one; `layerCount == 0` returns 0 too, which is not a
// valid index into anything and must be checked by the caller like any other
// index into an empty list.
size_t layerIndexForPanelRow(size_t row, size_t layerCount) noexcept;

// The exact inverse: index into `Document::layers` -> panel row. Same
// out-of-range rule.
size_t panelRowForLayerIndex(size_t layerIndex, size_t layerCount) noexcept;

// The glyph docs/ui.md §3.2 assigns each kind ("A kind glyph left of the
// thumbnail"). Returned as a UTF-8 string rather than a char, because every one
// of them is multi-byte.
const char* layerKindGlyph(LayerKind kind) noexcept;

// The row's title: the layer's own name, or a synthesised "Layer N" for an
// unnamed one so a row can never be blank. `layerIndex` is the model index, and
// N is `layerIndex + 1` -- a *positional* label, deliberately not the same
// thing as `core::defaultNewLayerName()`'s allocated name, which is a real name
// stored on the layer. This one is only ever displayed and changes when the
// layer moves; that is correct for a placeholder and would be wrong for a name.
std::string layerRowTitle(const Layer& layer, size_t layerIndex);

// The monospace sub-line docs/ui.md §3.2 specifies: `RGB · NORMAL · 100%`.
//
// Kind first (§3.2's resolution: "the kind leads the existing monospace
// sub-line"), then the blend name upper-cased, then opacity as a whole
// percent. A hidden layer additionally reads `· HIDDEN`, and a locked one
// `· LOCKED`, because the row's own eye and lock controls are the only other
// place that state appears and a text sub-line is what `--selftest` can read.
//
// The blend name is upper-cased **as carried**, never mapped through a table:
// an unrecognised `np:blend` from a newer build shows as itself
// (`LINEAR-BURN`), which is the value-level PRD I10 preservation core/Layer.hpp
// keeps the member a string for. A blend this build cannot composite is marked
// with a trailing `(!)` -- the panel's half of core/Composite's "never
// silently" rule -- and a display-referred one (PRD B7) additionally reads
// `SCREEN (display-referred)`, from the same `BlendModeInfo::space` field
// `blendMenuEntryText()` uses. A row is where a user reads what a layer does,
// so a label that only appeared while the dropdown happened to be open would
// not be "labelled as such" in any useful sense.
std::string layerRowSubLine(const Layer& layer);

// --- The blend dropdown (PLAN.md Phase 5 step 2; PRD B7, C3, L5) ----------

// The modes the dropdown offers for `doc.layers[layerIndex]`, in
// `allBlendModes()`' order, filtered by `blendModeAvailableForLayer()`.
//
// **PRD L5 lives in that predicate, not here.** This function does not know
// what `Mix` is or that Pigment layers are special; it asks core/Blend about
// each mode in turn. `core::setLayerBlend()` asks the same predicate before it
// writes, so a mode the dropdown does not offer is also a mode the model
// refuses -- L5 is not merely a thing the UI declines to draw.
//
// An out-of-range `layerIndex` yields an empty list rather than the whole set:
// a panel row that no longer names a layer must offer nothing, not everything.
std::vector<BlendMode> blendMenuForLayer(const Document& doc, size_t layerIndex);

// What one dropdown entry reads. **The only function that turns a mode into
// menu text, which is what makes PRD B7 enforceable rather than aspirational**
// ("display-referred modes are labelled as such"): the marker is derived from
// `BlendModeInfo::space` every time this is called, so there is no path from a
// mode to a menu entry that skips the label. A mode added to core/Blend's
// table cannot even be constructed without a `space` value.
//
// A display-referred mode reads `Screen  (display-referred)`. A linear-light
// one reads its bare label -- the working space *is* linear (PRD B1), so
// labelling the majority case would be noise that makes the minority case
// harder to see, which is the opposite of what B7 asks for.
//
// A mode this build cannot composite additionally reads `(not composited
// yet)`, from `BlendModeInfo::compositesPixels`. That is `Mix` and only `Mix`
// today; offering it silently would put PRD C3's P0 feature in a menu where
// choosing it appears to do nothing.
std::string blendMenuEntryText(BlendMode mode);

// The index into `blendMenuForLayer(doc, layerIndex)`'s result that matches
// the layer's current `blend` string, or `menu.size()` when the layer carries
// a name that is not in the menu at all -- an unrecognised one from a newer
// build (PRD I10), or `Mix` on a layer L5 no longer permits it on after a
// reorder. A combo has to render *something* for that layer, and rendering
// the first entry instead would be a silent lie about what the layer carries;
// ui/MacPaintUI shows the carried string itself in that case.
size_t blendMenuSelection(const Document& doc, size_t layerIndex,
                          const std::vector<BlendMode>& menu);

}  // namespace np
