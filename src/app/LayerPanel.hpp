#pragma once

#include <cstddef>
#include <string>

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
// silently" rule.
std::string layerRowSubLine(const Layer& layer);

}  // namespace np
