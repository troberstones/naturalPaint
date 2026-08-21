#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerSetOps.hpp"

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
// percent. A masked layer additionally reads `· MASK` (§3.2's own example row
// is `STROKES · NORMAL · MASK`), a hidden layer `· HIDDEN`, and a locked one
// `· LOCKED`, because the row's own eye and lock controls are the only other
// place that state appears and a text sub-line is what `--selftest` can read.
//
// A layer with a non-empty op stack reads `· 2 OPS` (`· 1 OP` for one), before
// the mask marker. It is present for every kind, because `Layer::ops` is, but
// it matters most on an Adjustment layer: that kind holds no pixels, so its op
// stack is the only content it has and a row that said nothing about it would
// be describing an empty layer. An empty stack -- every layer any `.npaint`
// written before PLAN.md Phase 5 step 5 carries -- prints nothing at all.
//
// `MASK` is present exactly when `Layer::mask` is engaged and says nothing
// about the mask's contents -- a mask that reveals everything composites
// byte-identically to no mask at all (core/Mask.hpp), so this marker is the
// only thing a user can see that tells the two apart.
//
// A labelled layer reads `· RED` last of all (PLAN.md Phase 5 step 11, PRD
// C15), upper-cased **as carried** for the same PRD I10 reason the blend name
// is: a label a newer build invented shows as itself rather than being
// normalised away or dropped. An unlabelled layer -- every layer this build has
// ever created -- prints nothing at all, which is what keeps every existing
// `--selftest` sub-line assertion character-identical.
//
// The **link** is deliberately not here, and that is the same boundary this
// function already draws for the clip: whether a layer is linked depends on
// whether any *other* layer carries the same group number, which is a question
// only a stack can answer. `layerLinkPartnerCount()` below takes the Document.
//
// A clipped layer reads `· CLIPPED` after the mask marker (PLAN.md Phase 5
// step 9, PRD C9) -- and docs/ui.md §3.2's own example row was `ADJUSTMENT ·
// CLIPPED` several steps before the feature existed, so the word is the
// document's rather than this file's. It reports what the layer **asks for**,
// not whether the ask can be honoured where it sits: this function takes a
// `Layer` and no `Document`, exactly as the `(!)` blend marker does, and
// "is there anything below to clip to" is a question only a stack can answer.
// core/Composite warns about a clipped layer with no base at every boundary
// that writes a file.
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

// --- Colour labels, links and panel filtering (PLAN.md Phase 5 step 11; PRD
//     C15) --------------------------------------------------------------------

// The swatch this build draws for a label name, or `std::nullopt` for a name it
// has none for -- including `kNoLayerColorLabel`. **A name with no swatch is
// not an error**: `core::Layer::colorLabel` is a string precisely so a label a
// newer build invented survives a round trip (PRD I10), and the panel's answer
// is to draw no chip and let `layerRowSubLine()` show the name as text. Mapping
// an unknown name onto some default colour would be the one behaviour that
// makes two different labels look like the same one.
//
// Values are the sRGB the chip is filled with, in [0,1] -- display values, not
// Working-space linear ones, because this is chrome and never touches a pixel
// of the document (ui/Theme.hpp's colours are the same).
struct LayerLabelSwatch {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  friend bool operator==(const LayerLabelSwatch&, const LayerLabelSwatch&) = default;
};
std::optional<LayerLabelSwatch> layerColorLabelSwatch(std::string_view name);

// How many **other** layers share this layer's link group; 0 for an unlinked
// layer and for the lone survivor of a deleted pair (core/LayerSetOps.hpp §4).
// This is the whole of what a row needs to decide whether to draw a link badge.
size_t layerLinkPartnerCount(const Document& doc, size_t layerIndex);

// --- Filtering the panel (PRD C15's third clause) --------------------------
//
// **The rule, because a filter that hides rows is a filter that can make a
// command act invisibly:**
//
//   A filter changes **which rows are drawn** and nothing else. It does not
//   change the selection, and it does not change the document.
//
//   **A hidden row stays selected** -- clearing the filter brings it back
//   exactly as it was. Dropping it instead would make typing three characters
//   into a search box a destructive edit to the user's selection, recoverable
//   by nothing (a selection is not in the undo stack).
//
//   **A command acts only on the rows the user can see.** `ui/MacPaintUI` calls
//   `restrictSelectionToFilter()` on the way into every set command, so a layer
//   that is selected but filtered out is not deleted, hidden, moved or aligned
//   by a gesture aimed at the rows on screen. The alternative -- act on the
//   whole selection -- is the invisible-action failure: five rows visible,
//   Delete pressed, eight layers gone.
//
//   **If restriction empties the set, the command refuses** rather than
//   quietly doing nothing, and the sentence says how many selected layers the
//   filter is hiding. Silence here would be indistinguishable from a broken
//   button.
struct LayerFilter {
  // Case-insensitive substring of the row's **title as displayed**
  // (`layerRowTitle()`), not of `Layer::name`. That is deliberate: an unnamed
  // layer's row reads "Layer 3", so a user typing `3` is matching what is in
  // front of them. Matching the empty stored name instead would make the
  // synthesised rows unfindable by the only text they show.
  std::string text;

  // `std::nullopt` matches every kind.
  std::optional<LayerKind> kind;

  // Whether anything is being filtered at all. An inactive filter matches every
  // layer, so no caller needs a special case for "no filter".
  bool active() const noexcept { return !text.empty() || kind.has_value(); }
};

bool layerMatchesFilter(const Document& doc, size_t layerIndex, const LayerFilter& filter);

// The layers the panel draws, as **model indices ascending** (bottom-first).
// The panel walks the result in reverse, which is the same single reversal
// `layerIndexForPanelRow()` owns -- this function deliberately does not return
// panel rows, so there is still exactly one place in the codebase that knows
// the panel is upside down.
std::vector<size_t> layersMatchingFilter(const Document& doc, const LayerFilter& filter);

// The selection with every filtered-out member removed. See the rule above:
// this is what a command receives, while the panel keeps the unrestricted
// selection so clearing the filter restores it.
LayerSelection restrictSelectionToFilter(const Document& doc, const LayerSelection& sel,
                                         const LayerFilter& filter);

// How many members `restrictSelectionToFilter()` would drop -- the number the
// refusal sentence needs when it drops all of them.
size_t layersHiddenFromSelection(const Document& doc, const LayerSelection& sel,
                                 const LayerFilter& filter);

}  // namespace np
