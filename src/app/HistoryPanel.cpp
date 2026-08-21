#include "app/HistoryPanel.hpp"

#include <algorithm>
#include <cstdio>

namespace np {
namespace {

// U+00B7 MIDDLE DOT -- docs/ui.md's own separator, and the one
// app/LayerPanel's row sub-line already uses. One vocabulary, two panels.
constexpr const char* kSep = " \xC2\xB7 ";

std::string mib(size_t bytes) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return std::string(buf);
}

// "6 states have" / "1 state has", from one place, because a refusal that
// reads "1 states" is a refusal a user trusts slightly less.
std::string count(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The serial lookup, for both lists. See HistoryPanel.hpp section (b): both
// are strictly ascending in `serial`, so `lower_bound` is correct and the
// click is O(log n) in the list length -- and the equality check after the
// search is what turns a broken invariant into a *refusal* (loud) rather than
// a row that points at the wrong state (silent, and the failure this file
// exists to prevent).
size_t indexOfSerial(const std::vector<HistoryEntry>& list, uint64_t serial) noexcept {
  const auto it = std::lower_bound(list.begin(), list.end(), serial,
                                   [](const HistoryEntry& e, uint64_t s) { return e.serial < s; });
  if (it == list.end() || it->serial != serial) return kNoHistoryRow;
  return static_cast<size_t>(it - list.begin());
}

// The tail every refusal here ends with. "Nothing moved" is only reassuring if
// it also says where nothing moved *from*.
std::string cursorTail(const History& history) {
  if (history.empty()) return "the history holds no states at all";
  const size_t at = history.cursor();
  return "the cursor has not moved and is still on state " + std::to_string(at + 1) + " of " +
         std::to_string(history.entries().size()) + " ('" + history.entries()[at].label + "')";
}

}  // namespace

const char* historyRowStateWord(HistoryRowState state) noexcept {
  switch (state) {
    case HistoryRowState::Past: return "PAST";
    case HistoryRowState::Current: return "CURRENT";
    case HistoryRowState::Redoable: return "REDOABLE";
  }
  return "?";
}

std::vector<HistoryPanelRow> historyPanelRows(const History& history) {
  const std::vector<HistoryEntry>& entries = history.entries();
  const size_t cursor = history.cursor();

  std::vector<HistoryPanelRow> rows;
  rows.reserve(entries.size());
  // Oldest first, and no reversal anywhere -- HistoryPanel.hpp section (a).
  // `row == index` is the whole mapping, which is why it is written as a plain
  // loop and not through a `layerIndexForPanelRow()`-shaped helper: there is
  // nothing for such a helper to get wrong, and having one would suggest there
  // is.
  for (size_t i = 0; i < entries.size(); ++i) {
    HistoryRowState state = HistoryRowState::Past;
    if (i == cursor)
      state = HistoryRowState::Current;
    else if (i > cursor)
      state = HistoryRowState::Redoable;
    rows.push_back(HistoryPanelRow{i, entries[i].serial, entries[i].label, state});
  }
  return rows;
}

std::string historyRowText(const HistoryPanelRow& row) {
  std::string s = row.label.empty() ? std::string("(unlabelled edit)") : row.label;
  s += kSep;
  s += historyRowStateWord(row.state);
  return s;
}

size_t historyRowForSerial(const History& history, uint64_t serial) noexcept {
  return indexOfSerial(history.entries(), serial);
}

uint64_t historySerialForRow(const History& history, size_t row) noexcept {
  const std::vector<HistoryEntry>& entries = history.entries();
  if (row >= entries.size()) return 0;  // never issued, so it cannot collide
  return entries[row].serial;
}

std::vector<HistorySnapshotRow> historySnapshotRows(const History& history) {
  const std::vector<HistoryEntry>& snaps = history.snapshots();
  std::vector<HistorySnapshotRow> rows;
  rows.reserve(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i)
    rows.push_back(HistorySnapshotRow{i, snaps[i].serial, snaps[i].label});
  return rows;
}

std::string historySnapshotRowText(const HistorySnapshotRow& row) {
  std::string s = row.label.empty() ? std::string("(unlabelled snapshot)") : row.label;
  s += kSep;
  s += "SNAPSHOT";
  return s;
}

HistoryPanelClick historyPanelClick(History& history, uint64_t serial) {
  HistoryPanelClick out;

  const size_t row = indexOfSerial(history.entries(), serial);
  if (row != kNoHistoryRow) {
    // **The one cursor move.** `row < entries().size()` by construction, so
    // this cannot fail. There is no loop in this file and no call to `undo()`
    // or `redo()` anywhere in it, which is PRD O3's "one replay from the
    // nearest keyframe, not N replays" made a property of this line rather
    // than of a comment; `cursorMoves` is what makes it countable from a test.
    out.document = history.jumpTo(row);
    out.cursorMoves = 1;
    out.ok = true;
    return out;
  }

  const size_t snap = indexOfSerial(history.snapshots(), serial);
  if (snap != kNoHistoryRow) {
    out.refusal = "history panel: that row is snapshot " + std::to_string(snap + 1) + " of " +
                  std::to_string(history.snapshots().size()) + " ('" +
                  history.snapshots()[snap].label +
                  "'), which is not on the linear list and has no cursor position to move to. "
                  "Restoring it is an edit and not a cursor move -- it truncates the redo tail, "
                  "appends one state at the bottom of the list and is itself undoable -- so it "
                  "goes through the snapshot group's own Restore. Nothing moved: " +
                  cursorTail(history) + ".";
    return out;
  }

  // Checked after the two lookups, not before them, so a snapshot's row on a
  // history with no linear entries yet still gets the accurate refusal above
  // rather than this one.
  if (history.empty()) {
    out.refusal =
        "history panel: there is nothing to jump to -- this document's history holds 0 states. "
        "Nothing moved.";
    return out;
  }

  // The case HistoryPanel.hpp section (b) exists for: a row from a panel whose
  // history was evicted or truncated underneath it. Refused with the numbers,
  // never redirected to whatever now sits at that position.
  const std::vector<HistoryEntry>& entries = history.entries();
  out.refusal = "history panel: no state in this history carries serial " +
                std::to_string(serial) +
                ", so the jump is refused rather than redirected to a neighbouring row -- the "
                "state beside a discarded one is a different picture, not a near miss. " +
                count(history.droppedEntryCount(), "state has", "states have") +
                " been dropped to stay inside a " + mib(history.budgetBytes()) +
                " byte budget and " +
                count(history.truncatedEntryCount(), "state was", "states were") +
                " truncated by an edit at a non-end cursor. " +
                count(entries.size(), "state remains", "states remain") + ", serials " +
                std::to_string(entries.front().serial) + " through " +
                std::to_string(entries.back().serial) + "; " + cursorTail(history) + ".";
  return out;
}

HistoryPanelClick historyPanelRestoreSnapshot(History& history, uint64_t serial) {
  HistoryPanelClick out;

  const size_t snap = indexOfSerial(history.snapshots(), serial);
  if (snap != kNoHistoryRow) {
    // An ordinary edit, by core/History's own design: it truncates, appends
    // and moves the cursor to the new last entry. `appendedEntry` is how a
    // caller knows the row list grew and must be rebuilt, and how a test
    // proves this is not a traversal wearing a traversal's name.
    out.document = history.restoreSnapshot(snap);
    out.ok = true;
    out.cursorMoves = 1;
    out.appendedEntry = true;
    return out;
  }

  const size_t row = indexOfSerial(history.entries(), serial);
  if (row != kNoHistoryRow) {
    out.refusal = "history panel: serial " + std::to_string(serial) + " is state " +
                  std::to_string(row + 1) + " of " + std::to_string(history.entries().size()) +
                  " on the linear list ('" + history.entries()[row].label +
                  "'), not a snapshot. Moving to it is a cursor move that records no edit, "
                  "which is that row's own click; restoring belongs to the snapshot group "
                  "alone. Nothing changed: " +
                  cursorTail(history) + ".";
    return out;
  }

  out.refusal = "history panel: no snapshot in this history carries serial " +
                std::to_string(serial) + " -- " +
                count(history.snapshots().size(), "snapshot is", "snapshots are") +
                " held. A dismissed snapshot is gone: 'exempt until dismissed' (PRD O4) means "
                "exactly that, and nothing brings one back. Nothing changed: " +
                cursorTail(history) + ".";
  return out;
}

std::string historyDroppedNote(const History& history) {
  const size_t dropped = history.droppedEntryCount();
  if (dropped == 0) return {};
  const std::string out = count(dropped, "earlier state has", "earlier states have") +
                          " been discarded to keep this history inside its " +
                          mib(history.budgetBytes()) + " byte budget";
  // Eviction stops at the cursor, so a nonzero dropped count always leaves at
  // least one entry -- the branch is a guard against a future `History` and not
  // a case that can be reached today.
  if (history.empty()) return out + ". Nothing is left to undo to.";
  return out + ". Undo stops at the oldest row here ('" + history.entries().front().label +
         "'); the states before it are gone, and a click carrying one of their rows is refused "
         "rather than redirected.";
}

std::string historyRedoTailNote(const History& history) {
  if (history.empty()) return {};
  const size_t after = history.entries().size() - 1 - history.cursor();
  if (after == 0) return {};
  return count(after, "state after this one can", "states after this one can") +
         " be redone. The next edit discards " + (after == 1 ? "it" : "them") +
         ": history is a linear list with a cursor, and recording at a non-end cursor truncates "
         "the tail.";
}

}  // namespace np
