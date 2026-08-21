#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/History.hpp"

// app/HistoryPanel (PLAN.md "Phase 5 -- Stack it", step 8: "**History panel**
// listing entries by originating tool or op; clicking one moves the cursor
// there in a single replay, not N"; PRD O2, O3, and O4/O1 made visible).
// docs/ui.md §5: "The History panel (PRD O2) joins the right-hand docked
// column. It lists entries by originating tool, and clicking one moves the
// history cursor -- which for Media layers is a single replay from the nearest
// keyframe, not N replays."
//
// Pure list mapping, row text and one click action; no ImGui and no GPU -- the
// same split app/LayerPanel.hpp and app/CurveEdit.hpp already document, and the
// reason a panel can be checked headlessly at all. The panel chrome (the
// section header, the selectables, the scroll region) is ui/MacPaintUI.cpp;
// everything a `--selftest` can check about what a row *says* and which state a
// row *is* lives here.
//
// core/History.hpp's "what is deliberately NOT here" predicts what this module
// would be owed -- `entries()`, `snapshots()`, `cursor()`, per-entry `label`
// and `serial`, `droppedEntryCount()` and `jumpTo()` -- and it was very nearly
// right. Four more of that class's existing members are read here and they are
// named rather than left for a reader to discover: `budgetBytes()` and
// `truncatedEntryCount()`, because a refusal in this codebase carries numbers
// and those are two of them; `empty()`; and `restoreSnapshot()`, for the
// snapshot group's own action. **Not one line was added to `core::History` for
// this step**, which is the useful form of that prediction being right: the
// panel is a reader of the model and adds no second notion of history state.
//
// ==========================================================================
// (a) Row order -- and it is the OPPOSITE of app/LayerPanel's
// ==========================================================================
//
// **This panel does not reverse anything. Row 0 is `entries()[0]`, the oldest
// state; the last row is the newest.** Stated up front because the sibling
// file next to this one exists almost entirely to own a reversal, and a reader
// who has just come from it will otherwise "fix" one of the two to match the
// other.
//
// The two panels differ because the two lists mean different things:
//
//   * `Document::layers` is a **compositing order** -- bottom first, because
//     core/Composite walks index 0 first and everything else lands on it. What
//     a user sees is that stack from the front, where the top layer occludes,
//     so the panel must show it top-first and app/LayerPanel owns the single
//     `count - 1 - row` that does it.
//   * `History::entries()` is a **temporal order** -- oldest first, because
//     `record()` appends. A list of things that happened reads downward, in
//     this application and in every editor that has such a panel; "opened" at
//     the top and the newest edit at the bottom is what the user watched
//     happen. Reversing it would make the cursor travel *up* as work
//     progresses, and would put the baseline at the bottom of a list that grew
//     downward away from it.
//
// So the model order already *is* the reading order, and introducing a
// reversal here would manufacture a second index mapping where zero are
// needed. `historyRowForSerial()` / `historySerialForRow()` below are
// therefore NOT a reversal pair the way app/LayerPanel's two functions are;
// they map between a stable id and a position, which is a different hazard and
// the one this file is actually shaped around.
//
// ==========================================================================
// (b) A row is keyed by `HistoryEntry::serial`, never by its index
// ==========================================================================
//
// core/History.hpp states the hazard outright and this file is the enforcement
// of it: "eviction shifts every index down by one, which would silently
// repoint a panel row or a pending 'jump here' action at a different state".
// Truncation does the same from the other end.
//
// **So `historyPanelClick()` takes a serial and never an index.** A click
// carries the id of the state the user pointed at, and if that state is no
// longer here the click is **refused with numbers, not redirected to a
// neighbour** -- the state beside a discarded one is a different picture, not
// a near miss, and silently installing it would be the worst possible answer:
// the user asked to go back to a known place and would arrive somewhere else
// with no indication that anything was wrong. `--selftest` runs exactly that
// scenario (record, evict, then click with a pre-eviction serial) and also
// shows what an index-keyed panel would have done with the same click.
//
// `HistoryPanelRow::index` is present for `--selftest`, which uses it to prove
// the mapping is exact; the chrome needs it for nothing and does not read it.
// It is the entry's position **at the moment the row list was built** and is
// invalidated by the next mutation of the History, so nothing may store it
// across a frame. The serial is what a pending action carries.
//
// **The lookup is a binary search, and a broken invariant can only ever cause
// a refusal.** `entries()` is strictly ascending in `serial`: `record()`
// appends `nextSerial_++`, eviction erases from the front and truncation from
// the back, none of which can disorder what is left. `snapshots()` likewise.
// So `std::lower_bound` is correct and the click costs O(log n) rather than
// O(n) -- but the found entry's serial is compared against the requested one
// before anything moves, so if that invariant were ever broken the lookup
// would fail to find a present serial (a refusal, which is loud) and could not
// return the wrong row (a silent misdirection, which is the failure this file
// exists to prevent). `--selftest` asserts the invariant directly across
// begin / record / truncate / evict / restore, so the slower failure mode is
// not a silent one either.
//
// Serial 0 is never issued (`History::nextSerial_` starts at 1), which is what
// makes it usable as "no serial" in `historySerialForRow()`'s return.
//
// ==========================================================================
// (c) The redo tail is a different thing from history, and says so
// ==========================================================================
//
// Entries after the cursor are a branch the user has left. They are reachable
// -- `redo()` walks into them -- but the **next edit destroys them**, because
// `History::record()` truncates everything after the cursor before it appends.
// A panel that drew them identically to the states behind the cursor would be
// lying about what the next click costs.
//
// So every row carries a `HistoryRowState` -- `Past`, `Current` or `Redoable`
// -- and `historyRowText()` puts the word in the row text, which is what makes
// it checkable by `--selftest` rather than a colour only a screenshot shows.
// `historyRedoTailNote()` is the sentence version, naming how many states the
// next edit would discard.
//
// **What a row does NOT show is a number.** Any number on a history row is
// either an index -- which eviction shifts, and which this whole file exists
// to keep out of a click -- or a serial, which is an internal id and means
// nothing to a user. Position is conveyed by the row's position; the row text
// is the label and the state, and nothing else.
//
// ==========================================================================
// (d) PRD O3, made countable rather than argued
// ==========================================================================
//
// "Jumping back N entries costs one replay from the nearest keyframe, not N
// replays." `historyPanelClick()` reports `cursorMoves`, which is **1 on any
// successful click regardless of distance, and 0 on a refusal**, incremented
// at the one line that calls `History::jumpTo()`. There is no loop in this
// file and no call to `undo()` or `redo()` anywhere in it.
//
// `--selftest` does not take that on trust: it writes the per-step walk beside
// it -- the panel an implementer would get by calling `undo()` until the
// cursor arrives -- counts its calls (N) against this one (1), and times both
// at N = 1 and N = 40. The numbers print on every run.
//
// The click's own cost is O(log n) in the *list length* from the serial
// lookup, and O(1) in the *distance travelled*, which is the quantity PRD O3
// is about. Nothing here materialises an intermediate state, because there are
// no intermediate states to materialise: core/History.hpp's every entry is a
// keyframe, so the nearest keyframe at or before the target is the target and
// the replay range is empty. When Media layers land (phase 11) that replay
// range stops being empty and grows a loop inside `History::jumpTo()`; this
// file does not change, because it already asks for one cursor move and counts
// one.
//
// ==========================================================================
// (e) Eviction and snapshots, made legible rather than silent
// ==========================================================================
//
// **Eviction.** `History::droppedEntryCount()` is nonzero once the byte budget
// has eaten the oldest states. A user whose undo silently stops going back has
// no way to tell that from a history that simply started there, so
// `historyDroppedNote()` says it in a line above the list, with the count and
// the budget it was spent against. It is a *line* and not a *row*: a row
// implies something to click, and there is nothing there to jump to.
//
// **Snapshots are a second list, and the panel shows them as one.**
// core/History.hpp's hardest decision is that a snapshot lives in
// `snapshots()` and not in `entries()`, because a flag on a list entry dies on
// truncation at exactly the moment the feature exists for. That decision is
// only honest if the panel presents them the same way -- a group of their own,
// below the linear list, in the order taken, with no cursor relationship at
// all. Interleaving them into the rows would put a state from an abandoned
// branch in the middle of the undo chain, which is the thing the second list
// was built to avoid.
//
// The two clicks are therefore different actions and are different functions:
//
//   * `historyPanelClick(serial)` moves the cursor. On a *snapshot's* serial
//     it refuses by name, because a snapshot has no cursor position to move
//     to.
//   * `historyPanelRestoreSnapshot(serial)` restores one, which
//     `History::restoreSnapshot()` performs as an ordinary `record()` -- it
//     truncates, appends and is itself undoable. The result says so through
//     `appendedEntry`, so a caller cannot mistake it for a traversal. On an
//     *entry's* serial it refuses by name, for the mirror reason.
//
// Every refusal in this file follows the codebase's convention and
// `History::budgetPressure()`'s tone exactly: **name the numbers, say what did
// not happen, and never silently do nothing.**
//
// ==========================================================================
// What is deliberately NOT here
// ==========================================================================
//
//  * **Making revert undoable.** core/History.hpp's `begin()` comment defers
//    that decision to this step, so it is answered here: it stays as it is.
//    app/DocumentLifecycle's revert refusal already tells the user in so many
//    words that "there is no undo for it", `revertDocument()` re-seeds the
//    history with a "revert to saved" baseline, and that is exactly what the
//    panel then shows -- one row reading `revert to saved · CURRENT` with
//    nothing above it. The promise is visible rather than merely kept, and no
//    code was needed to make it so. Changing it would mean holding the
//    pre-revert document alive across a re-seed, which is a lifecycle change
//    and not a panel one.
//  * **Coalescing, selection state, drag, or a "clear history" button.** The
//    first is core/History's (and needs strokes, which do not exist); the
//    others are chrome. This file holds no mutable state at all -- every
//    function takes the History and derives everything from it, so there is no
//    panel-side cache to go stale when a state is evicted underneath it, which
//    is the same class of bug (b) is about.
//  * **Thumbnails.** For the reason drawLayersSection() already gives: the
//    painting canvas is not one of these documents, so a thumbnail would imply
//    a connection that does not exist.
namespace np {

// "No such row." Returned by `historyRowForSerial()` for a serial this history
// does not hold -- an evicted one, a truncated one, a snapshot's, or one that
// was never issued.
inline constexpr size_t kNoHistoryRow = static_cast<size_t>(-1);

// Where a row sits relative to the cursor. Section (c) above.
enum class HistoryRowState {
  Past,      // before the cursor: reachable by undo, and safe from the next edit
  Current,   // the state on screen
  Redoable,  // after the cursor: reachable by redo, and DISCARDED by the next edit
};

// The single word `historyRowText()` puts in a row, exported so the chrome and
// `--selftest` share one vocabulary instead of two spellings of it.
const char* historyRowStateWord(HistoryRowState state) noexcept;

// One row of the linear list, oldest first.
struct HistoryPanelRow {
  // Position in `History::entries()` **at the moment this row list was
  // built**, and identical to the row's position in the panel (section (a):
  // there is no reversal). Invalidated by the next mutation of the History;
  // nothing may store it across a frame. See section (b).
  size_t index = 0;

  // The stable id. This is what a click carries and what a pending action
  // stores. Never 0 for a real row.
  uint64_t serial = 0;

  // `HistoryEntry::label` -- core/LayerOps' own `editLabel`, which is PRD O2's
  // "the tool or op that produced them" and the same string PRD I11's refusals
  // name, so the panel cannot drift from the model's vocabulary.
  std::string label;

  HistoryRowState state = HistoryRowState::Past;
};

// The rows, oldest at the top. Empty for an empty history.
std::vector<HistoryPanelRow> historyPanelRows(const History& history);

// What one row reads: the label, then the state word, separated by docs/ui.md's
// own middle dot -- `add layer · CURRENT`. An entry with an empty label reads
// `(unlabelled edit)` rather than nothing, so a row can never be blank; that is
// app/LayerPanel's `Layer N` rule applied to the other panel.
std::string historyRowText(const HistoryPanelRow& row);

// --- The serial <-> row mapping (section (b)) -----------------------------

// The row showing `serial`, or `kNoHistoryRow` when this history holds no
// entry with that serial. A snapshot's serial is NOT found here: a snapshot is
// not on the linear list and has no row in it.
size_t historyRowForSerial(const History& history, uint64_t serial) noexcept;

// The exact inverse: the serial shown by `row`, or 0 for an out-of-range row.
// 0 is safe as "none" because `History` never issues it.
uint64_t historySerialForRow(const History& history, size_t row) noexcept;

// --- Snapshots (PRD O4), as their own group (section (e)) -----------------

struct HistorySnapshotRow {
  // Index into `History::snapshots()`, with the same
  // valid-until-the-next-mutation rule as `HistoryPanelRow::index`.
  size_t index = 0;
  uint64_t serial = 0;
  std::string label;
};

std::vector<HistorySnapshotRow> historySnapshotRows(const History& history);

// `before the risky bit · SNAPSHOT`. The word is not one of
// `HistoryRowState`'s on purpose: a snapshot is neither past, current nor
// redoable, because it is not on the linear list at all, and giving it one of
// those three words would be the interleaving section (e) rejects.
std::string historySnapshotRowText(const HistorySnapshotRow& row);

// --- The click ------------------------------------------------------------

struct HistoryPanelClick {
  bool ok = false;

  // The state now installed, or nullptr on a refusal. **Valid until the next
  // mutation of this History**, which is `History::jumpTo()`'s own rule
  // restated: the usual call installs it immediately
  // (`od->document = *r.document;`).
  const Document* document = nullptr;

  // PRD O3, countable. **1 on success at any distance, 0 on a refusal.**
  // Incremented at the one line that calls `History::jumpTo()`; there is no
  // loop in this module. See section (d).
  size_t cursorMoves = 0;

  // True only for a snapshot restore, which `History::restoreSnapshot()`
  // performs as an ordinary `record()` -- so it truncated the redo tail,
  // appended a row at the bottom, and is itself undoable. A caller that
  // redraws the row list on a cursor move must also redraw on this.
  bool appendedEntry = false;

  // Empty on success. Otherwise one sentence naming the numbers and saying
  // what did not happen, in `History::budgetPressure()`'s tone.
  std::string refusal;
};

// Moves the cursor to the state carrying `serial`. **One cursor move, whatever
// the distance** (PRD O3).
//
// Refuses, and moves nothing, when: the history is empty; `serial` names a
// snapshot rather than a list entry; or `serial` names no state this history
// still holds. The last case is the one section (b) is about, and it is a
// refusal rather than a jump to the nearest surviving state.
HistoryPanelClick historyPanelClick(History& history, uint64_t serial);

// Restores the snapshot carrying `serial`, as an edit (section (e)). Refuses,
// and changes nothing, when `serial` names a list entry, or a snapshot that
// has been dismissed, or nothing at all.
HistoryPanelClick historyPanelRestoreSnapshot(History& history, uint64_t serial);

// --- The two notes around the list (sections (e) and (c)) -----------------

// "6 earlier states have been discarded to keep this history inside its
// 0.50 MiB byte budget. Undo stops at the oldest row here ..." -- or empty when
// nothing has been evicted. Every quantity it reads
// (`droppedEntryCount()`, `budgetBytes()`, the oldest entry's label) is O(1),
// which is the point: this runs every frame and `History::bytes()` is an
// O(slots) scan over every tile of every entry.
std::string historyDroppedNote(const History& history);

// "2 states after this one can be redone. The next edit discards them ..."
// Empty when the cursor is at the newest entry, which is the common case, so
// the panel stays quiet until there is a branch to warn about.
std::string historyRedoTailNote(const History& history);

}  // namespace np
