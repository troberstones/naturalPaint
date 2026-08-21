#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/LayerCompOps.hpp"

// app/CompPanel (PLAN.md "Phase 5 -- Stack it", step 12; PRD C14). The COMPS
// section of the right-hand docked column, minus its chrome.
//
// Pure list mapping, row text and the sentence a restore leaves behind; no
// ImGui and no GPU -- the same split app/LayerPanel.hpp and app/HistoryPanel.hpp
// already document, and the reason a panel can be checked headlessly at all.
// The chrome (the collapsing header, the selectables, the rename field) is
// ui/MacPaintUI.cpp.
//
// ==========================================================================
// (a) Row order: the model order, like HISTORY and unlike LAYERS
// ==========================================================================
//
// **Row 0 is `comps[0]`. Nothing here reverses anything.** Stated up front
// because two sibling files sit next to this one and they disagree with each
// other: app/LayerPanel exists almost entirely to own a reversal, and
// app/HistoryPanel exists partly to say that it must not.
//
// This list is with HISTORY. `Document::layers` is a *compositing* order, and
// the panel must show it top-first because that is the end the viewer looks at
// it from. `Document::comps` has no such intrinsic direction -- a comp is not
// above or below another comp -- so the order is simply the order the user put
// them in, read downward, and `moveLayerComp()` is how they change it. A
// reversal here would manufacture a second index mapping where zero are
// needed, and would make "move up" mean `to = from + 1` in one panel and
// `to = from - 1` in another.
//
// ==========================================================================
// (b) A comp row is keyed by index, and that is NOT the mistake
//     app/HistoryPanel exists to prevent
// ==========================================================================
//
// The obvious objection, answered here because a reader coming from
// app/HistoryPanel.hpp section (b) will raise it: history rows carry a stable
// `serial` because **the history mutates underneath the panel without the user
// touching it** -- the byte budget evicts the oldest entries whenever an edit
// is recorded, so a row index the panel handed out last frame can name a
// different state this frame, silently.
//
// `Document::comps` has no evictor, no budget and no background mutation. The
// only things that change it are the five operations in core/LayerCompOps, all
// of which are user gestures, and every one of them goes through
// `recordLayerEdit()`, which redraws the panel. So a comp index is stale only
// in the one window where the layers panel's own indices are stale -- between
// a mutation and the end of the frame -- and `compPanelRows()` is rebuilt from
// the document every frame like every other list in this column.
//
// The rule that follows, and the one thing the chrome must obey: **an action is
// applied after the row loop, never inside it**, exactly as
// `drawLayersSection()`'s `structureChanged` and `drawHistorySection()`'s
// deferred click already do. No comp index is ever stored across a frame.
//
// **Where a stable identity is genuinely load-bearing, this step has one.** It
// is `core::Layer::id`, inside a comp's entries, because *that* list -- the
// layer stack -- really does move underneath a comp: a delete, an add or a
// reorder shifts every index above it, and a comp holds its entries across
// arbitrarily many of those. core/Layer.hpp and core/LayerCompOps.hpp carry
// that argument. Adding a second id for the comp rows would be machinery for a
// hazard that is not there.
//
// ==========================================================================
// (c) What a row says
// ==========================================================================
//
// The comp's name, then how many layer states it holds, then -- and this is the
// part that only exists because comps outlive the stack they were captured from
// -- **how many of those layers are still in this document**. A comp captured
// over five layers, three of which have since been deleted, reads
//
//     Cool variant · 5 layers · 2 still here
//
// so the thing that makes a restore partial is legible *before* the click
// rather than only in the sentence afterwards. A comp that matches the document
// exactly says nothing extra, which keeps the common case quiet.
//
// The count is computed against the live document every frame, which is O(comps
// x layers) with both in the dozens -- the same order as the row loop itself,
// and nothing here is cached, so there is no panel-side state to go stale when
// a layer is deleted underneath it.
namespace np {

// One row of the comps list, in model order (section (a)).
struct CompPanelRow {
  // Index into `Document::comps` **at the moment this row list was built**, and
  // identical to the row's position in the panel. See section (b): valid within
  // the frame, never stored across one.
  size_t index = 0;

  // `LayerComp::name`, verbatim and possibly empty. `compRowText()` is what
  // turns an empty one into something a user can read.
  std::string name;

  // How many layer states the comp holds, and how many of them name a layer
  // that is still in this document. `stillHere <= captured` always.
  size_t captured = 0;
  size_t stillHere = 0;

  // False for a comp record carried verbatim from a build whose format this one
  // does not read (`LayerComp::known`). The row still exists -- dropping it
  // would hide something the file contains -- and it says so.
  bool known = true;
};

std::vector<CompPanelRow> compPanelRows(const Document& doc);

// What one row reads: the name, then the counts, separated by docs/ui.md's own
// middle dot. An unnamed comp reads `(unnamed comp)` rather than nothing, which
// is app/LayerPanel's `Layer N` rule and app/HistoryPanel's `(unlabelled edit)`
// rule applied to the third list. An unreadable one reads
// `... · UNREADABLE (kept)`.
std::string compRowText(const CompPanelRow& row);

// True when restoring this row would apply less than the whole comp -- i.e.
// when the row's counts differ. What the chrome greys nothing out on: a partial
// restore is offered, attempted and answered with
// `core::layerCompRestoreSummary()`'s sentence, because a sentence explains and
// a disabled button does not. That is app/LayerEditor.hpp's own
// availability-versus-refusal rule, and this is the same trade.
bool compRowIsPartial(const CompPanelRow& row);

// --- Which way is up (section (a), made into two functions) ---------------
//
// **In this panel, up the panel is DOWN the index**, because row 0 is
// `comps[0]` and nothing is reversed. In the LAYERS panel it is the other way
// round, because that one *is* reversed. app/LayerPanel.hpp says why a second
// place that knows a direction is how "up" ends up moving a thing down, and
// these exist because this panel got it wrong first: the chrome originally
// wired Up to `index + 1`, copied from the layers panel, which moved a comp
// down the list.
//
// So the arithmetic is here, where `--selftest` can check it, and the draw loop
// calls it. `kNoCompRow` means the row is already at that end and the button is
// disabled.
inline constexpr size_t kNoCompRow = static_cast<size_t>(-1);

// The `core::moveLayerComp()` destination for moving `row` one place up the
// panel (toward `comps[0]`, which is drawn first).
size_t compRowMoveUpTarget(size_t row, size_t rowCount) noexcept;

// ...and one place down it. Each other's inverse where both are defined, which
// is what the test asserts rather than the two formulae separately.
size_t compRowMoveDownTarget(size_t row, size_t rowCount) noexcept;

}  // namespace np
