#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/LayerComp.hpp"
#include "core/LayerOps.hpp"

// core/LayerCompOps (PLAN.md "Phase 5 -- Stack it", step 12; PRD C14). The
// operations over `Document::comps`: capture, restore, rename, delete,
// reorder.
//
// Free functions in their own translation unit, returning `core::LayerOpResult`
// -- core/LayerOps.hpp's own shape, deliberately reused rather than a sixth
// result type invented. That is not tidiness: `app::recordLayerEdit()` takes a
// `LayerOpResult` and is the single funnel that bumps `OpenDocument::revision`,
// appends a `core::History` entry and hands app/Journal a structural change. A
// new result type would have needed a second funnel, and a second funnel is
// exactly how "restoring a comp is undoable" quietly stops being true.
//
// ==========================================================================
// Restoring a comp is an EDIT
// ==========================================================================
//
// `History::restoreSnapshot()` sets the precedent one level up and states the
// argument: restoring "is an ordinary edit -- `restoreSnapshot()` calls
// `record()`, so it truncates, appends and is itself undoable, which is both
// what Photoshop does and the only answer consistent with 'history is a linear
// list with a cursor'". The same holds here, and the same mechanism delivers
// it: `restoreLayerComp()` mutates the document and returns a `LayerOpResult`,
// and every caller in the running application passes it to
// `recordLayerEdit()`. `--selftest` asserts that undo after a restore gives
// back a document byte-identical to the one before it.
//
// ==========================================================================
// The layer-set mismatch, which is the real design work in this step
// ==========================================================================
//
// A comp captured when the document had five layers, restored after one was
// deleted, three were added and two were reordered. Every one of those moves
// every index above it, so an index-keyed comp does not merely lose an entry
// -- it applies each remaining entry to a *different layer*, silently, and the
// result is a wrong picture that does not look like an error. app/HistoryPanel
// keys its rows by `HistoryEntry::serial` and never by index for exactly this
// reason, and section (b) of that header is the argument; `Layer::id` is the
// same answer to the same hazard.
//
// So the rules, in the order `restoreLayerComp()` applies them:
//
//  1. **An entry is matched by `Layer::id`, never by position.** A comp's
//     entries are in capture-time stack order for legibility only; nothing
//     reads that order.
//  2. **Two layers with the same nonzero id -> the whole restore is refused**,
//     naming both indices and the id, and nothing is changed. It is the one
//     state in which "apply this entry" has more than one answer, and picking
//     either would be the silent misapplication this design exists to prevent.
//     `duplicateLayer()` resets a copy's id so this cannot arise from the UI;
//     the refusal exists because a `Document` is a plain aggregate that
//     anything can write to, and `--selftest` constructs the state directly.
//  3. **No entry matches any layer -> refused**, with both counts. A comp none
//     of whose layers are still here is a comp for a different document, and
//     applying zero of it and reporting success would be indistinguishable
//     from a comp that does nothing.
//  4. **Otherwise the restore applies what it can and reports the rest, with
//     numbers.** `LayerCompRestoreReport` below counts and names four
//     categories, and `layerCompRestoreSummary()` turns it into the one
//     sentence a panel shows. A partial restore is the ordinary case -- delete
//     one layer of five and every comp is partial forever -- so refusing it
//     would make comps useless after the first delete. What must never happen
//     is applying an entry to the wrong layer, and rule 2 is what stops that.
//
// **The lock is honoured, not bypassed.** Every property a restore writes goes
// through the core/LayerOps setter for it, so a locked layer keeps its
// opacity, blend and clip (core/LayerOps.hpp's lock section) and only its
// visibility moves -- which is exactly the split that header already argues
// for. The report names the locked layers rather than failing quietly, and
// this is why the restore calls five-line setters instead of assigning members:
// a second copy of the lock rule here would be a second chance to get it wrong.
namespace np {

// Assigns `Layer::id` to every layer that has none, and to any layer whose id
// duplicates an earlier one's, from `Document::nextLayerId`.
//
// Idempotent, O(n^2) in the layer count with n in the dozens, and **stable**:
// a layer that already holds a unique nonzero id always keeps it, so every comp
// that refers to it keeps working. Where two layers share an id the *lower*
// one keeps it, which is the one that has been in the document longer
// (`duplicateLayer()` inserts the copy above the source), so a comp captured
// before a duplicate still names the layer it meant.
//
// Also raises `nextLayerId` past every id it finds, so a document whose counter
// was lost -- loaded from a file another tool stripped `np:comps` out of, say
// -- still cannot re-issue an id a live layer holds.
//
// Called by `captureLayerComp()` and by nothing else in the running
// application: a document that never captures a comp never gets an id, which
// is what keeps this feature off every path that predates it.
void normalizeLayerIds(Document& doc);

// A default name for a new comp: "Comp N", one above the highest number
// already used by an existing "Comp N", so capturing, deleting and capturing
// again does not hand out a name already on screen.
//
// `core::defaultNewLayerName()`'s rule and reasoning exactly. Comp names are
// **not** required to be unique -- nothing identifies a comp by name -- for the
// same reason layer names are not (docs/document-format.md).
std::string defaultNewCompName(const Document& doc);

// --- Capture --------------------------------------------------------------

// Appends a comp holding every layer's current state, under `name`.
//
// Assigns layer ids first (`normalizeLayerIds()`), so the comp's entries always
// name real, unique ids. `name` may be anything, including a name another comp
// already has and including empty -- an empty one displays as `(unnamed comp)`
// through app/CompPanel, the same rule app/LayerPanel applies to a nameless
// layer.
//
// Refused, by name, on a document with **no layers**: a comp of nothing is a
// row a user can click that cannot change anything, and offering one would put
// a control in the panel whose only possible outcome is a refusal later.
LayerOpResult captureLayerComp(Document& doc, std::string name);

// --- Restore --------------------------------------------------------------

// What a restore could not do. Every count is exact and every name is a layer's
// own name; see this header's mismatch section for what each one means.
struct LayerCompRestoreReport {
  // Entries whose `layerId` matched a layer that is still here.
  size_t entriesApplied = 0;
  // Of those, how many actually changed something. A comp restored twice
  // running changes nothing the second time and says so.
  size_t layersChanged = 0;

  // Entries whose layer is no longer in this document -- deleted since the
  // capture. Named by `LayerCompEntry::nameAtCapture`, because an id means
  // nothing to a user.
  std::vector<std::string> missingLayers;

  // Layers in the document that this comp says nothing about -- added since the
  // capture. Their state is left exactly as it was, which is the only
  // defensible answer: the comp has no opinion about them.
  std::vector<std::string> uncoveredLayers;

  // Layers whose lock refused one or more properties. Their visibility was
  // still restored (core/LayerOps.hpp allows that on a locked layer); their
  // opacity, blend and clip were not.
  std::vector<std::string> lockedLayers;

  // Layers whose captured blend name this build cannot set -- a mode from a
  // newer build (PRD I10), or `Mix` on a pair PRD L5 no longer permits it on.
  // The captured string is named, never substituted.
  std::vector<std::string> unsettableBlends;

  bool fullyApplied() const noexcept {
    return missingLayers.empty() && lockedLayers.empty() && unsettableBlends.empty();
  }
};

// Restores comp `compIndex` onto `doc`. See this header's mismatch section for
// the four rules; the short version is that entries are matched by
// `Layer::id`, an ambiguous id refuses the whole restore, and anything else
// applies what it can and fills in `*report`.
//
// `report` may be null. On a refusal it is left untouched and **nothing in the
// document is changed** -- the refusals are all decided before the first write.
//
// The `editLabel` on success is `restore comp "<name>"`, so a history row reads
// as the gesture rather than as its consequences; `layerCompRestoreSummary()`
// is where the consequences are said.
LayerOpResult restoreLayerComp(Document& doc, size_t compIndex,
                               LayerCompRestoreReport* report = nullptr);

// The one sentence a panel shows after a restore, in io/Export's refusal style
// -- name the numbers, say what did not happen. Empty when the restore applied
// in full and changed something; a restore that applied in full and changed
// nothing says so, because a click that does nothing needs to say why.
std::string layerCompRestoreSummary(const LayerCompRestoreReport& report);

// --- The list operations --------------------------------------------------

// Renames comp `compIndex`. Any string is accepted, including one another comp
// already has -- comp names are not unique and nothing identifies a comp by
// name.
LayerOpResult renameLayerComp(Document& doc, size_t compIndex, std::string name);

// Deletes comp `compIndex`. Never touches a layer: a comp is a record of state,
// and deleting the record is not a change to the picture.
LayerOpResult deleteLayerComp(Document& doc, size_t compIndex);

// Moves comp `from` so it ends up at `to`, `core::moveLayer()`'s semantics
// exactly (both indices into the list as it stands *before* the move).
// `from == to` succeeds and changes nothing, matching `moveLayer()`.
LayerOpResult moveLayerComp(Document& doc, size_t from, size_t to);

}  // namespace np
