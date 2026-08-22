#include "core/History.hpp"

#include <cstdio>
#include <unordered_map>
#include <utility>

#include "core/Layer.hpp"
#include "core/TileShare.hpp"

namespace np {
namespace {

// --- The byte scan --------------------------------------------------------
//
// Every slot of every store of every layer, with the two facts the accounting
// needs: the tile's identity (its address -- the only honest key for a shared
// object, since summing per-document numbers counts a shared tile once per
// holder) and how many holders it has in the whole process.
//
// `tileUseCount(coord)` is one extra hash lookup per slot on top of the
// iteration. That is the entire cost of this being possible without touching
// core/TileStore, which step 6 shaped for exactly this question.
template <class Store, class Fn>
void visitStoreSlots(const Store& store, Fn&& fn) {
  for (const auto& [coord, tile] : store) {
    fn(static_cast<const void*>(&tile), store.tileUseCount(coord),
       sizeof(typename Store::TileType));
  }
}

// All three stores, per core/TileShare.hpp: RGB *or* pigment (at most one is
// engaged) plus the mask, which is orthogonal to both and sized differently.
// Deliberately not "tiles x 128 KiB" -- that is wrong for two of the three.
template <class Fn>
void visitDocumentSlots(const Document& doc, Fn&& fn) {
  for (const Layer& layer : doc.layers) {
    if (layer.rgbTiles) visitStoreSlots(*layer.rgbTiles, fn);
    if (layer.pigmentTiles) visitStoreSlots(*layer.pigmentTiles, fn);
    if (layer.mask) visitStoreSlots(*layer.mask, fn);
  }
}

struct SlotTally {
  size_t useCount = 0;    // holders anywhere in the process
  size_t heldHere = 0;    // slots inside this History that point at it
  size_t heldBySnap = 0;  // ... of which, slots inside a snapshot
  size_t bytes = 0;
};

std::string mib(size_t bytes) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return std::string(buf);
}

}  // namespace

// --- Recording ------------------------------------------------------------

void History::begin(std::string label, const Document& doc) {
  entries_.clear();
  cursor_ = 0;
  droppedEntries_ = 0;
  truncatedEntries_ = 0;
  entries_.push_back(HistoryEntry{std::move(label), doc, nextSerial_++});
  // Snapshots are untouched on purpose -- "exempt until dismissed", and a new
  // baseline is not a dismissal (see the header on revert).
  enforceBudget();
}

void History::record(std::string label, const Document& doc) {
  if (entries_.empty()) {
    begin(std::move(label), doc);
    return;
  }

  // Truncate first: everything strictly after the cursor is a branch the user
  // has just left. Erasing releases those entries' shared_ptr slots, so every
  // tile version only the tail held is freed here, at this line, and not on
  // some later sweep.
  if (cursor_ + 1 < entries_.size()) {
    truncatedEntries_ += entries_.size() - (cursor_ + 1);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), entries_.end());
  }

  entries_.push_back(HistoryEntry{std::move(label), doc, nextSerial_++});
  cursor_ = entries_.size() - 1;
  enforceBudget();
}

bool History::amend(std::string label, const Document& doc) {
  // Empty, or the cursor is somewhere the user has undone back to. Either way
  // there is no "top entry that is mine" to extend -- see the header for why
  // this is a refusal rather than a fallback to record().
  if (entries_.empty() || cursor_ + 1 != entries_.size()) return false;

  HistoryEntry& top = entries_.back();
  const uint64_t serial = top.serial;  // kept: same act, more of it done
  top.label = std::move(label);
  top.document = doc;
  top.serial = serial;

  // The document grew, so the entry's byte count did too. record() enforces
  // after every append for the same reason: an amended entry that pushed the
  // history over budget would otherwise sit there until the *next* unrelated
  // edit happened to notice.
  enforceBudget();
  return true;
}

// --- Traversal ------------------------------------------------------------

const Document* History::jumpTo(size_t index) {
  if (index >= entries_.size()) return nullptr;
  cursor_ = index;
  return &entries_[index].document;
}

const Document* History::undo() { return canUndo() ? jumpTo(cursor_ - 1) : nullptr; }

const Document* History::redo() { return canRedo() ? jumpTo(cursor_ + 1) : nullptr; }

// --- Snapshots ------------------------------------------------------------

size_t History::takeSnapshot(std::string label, const Document& doc) {
  snapshots_.push_back(HistoryEntry{std::move(label), doc, nextSerial_++});
  // No enforceBudget() here, and that is the point of O4: a snapshot cannot be
  // reclaimed, so re-running eviction could only evict ordinary entries to pay
  // for a snapshot the user just asked for. It would silently trade away the
  // undo depth they can see for bytes they just chose to spend.
  return snapshots_.size() - 1;
}

bool History::dismissSnapshot(size_t index) {
  if (index >= snapshots_.size()) return false;
  snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

const Document* History::restoreSnapshot(size_t index) {
  if (index >= snapshots_.size()) return nullptr;
  // By value: record() may erase from entries_ but never from snapshots_, so a
  // reference would be safe today -- and would be a trap the first time
  // restoring is made to do anything else. The copy is a share (step 6).
  const Document state = snapshots_[index].document;
  record("restore snapshot: " + snapshots_[index].label, state);
  return &entries_.back().document;
}

// --- The byte bound -------------------------------------------------------

HistoryBytes History::bytes() const {
  HistoryBytes out;

  std::unordered_map<const void*, SlotTally> tally;
  tally.reserve(entries_.size() * 8 + 16);

  auto add = [&](const HistoryEntry& entry, bool isSnapshot) {
    out.shown += documentTileBytes(entry.document);
    visitDocumentSlots(entry.document, [&](const void* p, size_t useCount, size_t bytes) {
      SlotTally& t = tally[p];
      t.useCount = useCount;  // identical on every sighting; nothing mutates during a scan
      t.bytes = bytes;
      ++t.heldHere;
      if (isSnapshot) ++t.heldBySnap;
    });
  };

  for (const HistoryEntry& e : entries_) add(e, false);
  for (const HistoryEntry& e : snapshots_) add(e, true);

  for (const auto& [ptr, t] : tally) {
    (void)ptr;
    out.distinct += t.bytes;
    ++out.distinctTiles;
    // The whole rule, in one line: a tile whose every holder is inside this
    // History is a byte dropping this History would return. One whose
    // use_count exceeds the slots we counted is also held by the live
    // document (or by a duplicate, or by app/Journal's copy), and dropping
    // history would not free it -- so it is not history's byte to spend.
    if (t.heldHere == t.useCount) {
      out.attributable += t.bytes;
      // Exempt when a snapshot holds it AND no evictable entry is its sole
      // other holder -- i.e. evicting every entry still would not free it.
      if (t.heldBySnap > 0) out.exemptFromEviction += t.bytes;
    }
  }
  return out;
}

void History::setBudgetBytes(size_t bytes) {
  budgetBytes_ = bytes;
  enforceBudget();
}

bool History::evictOldest() {
  // `[0, cursor_)` is the evictable range -- see the header for the two
  // independent reasons that is the only correct end.
  if (cursor_ == 0 || entries_.empty()) return false;
  entries_.erase(entries_.begin());
  --cursor_;
  ++droppedEntries_;
  return true;
}

size_t History::shownBytesCheap() const {
  // O(entries x layers), not O(slots): `TileStoreOf::tileBytes()` is
  // `tiles_.size() * sizeof(T)`, a multiply, so this needs no iteration and no
  // hash lookups at all.
  size_t total = 0;
  for (const HistoryEntry& e : entries_) total += documentTileBytes(e.document);
  for (const HistoryEntry& e : snapshots_) total += documentTileBytes(e.document);
  return total;
}

void History::enforceBudget() {
  // The early-out, and it is sound rather than a heuristic:
  // `attributable <= distinct <= shown` by construction (deduplicating cannot
  // grow a sum, and keeping a subset cannot grow it either), so a history whose
  // *undeduplicated* total already fits is within budget and the O(slots) scan
  // can be skipped entirely. This is the common case by a wide margin -- it
  // fails only once the history's entries collectively point at more than
  // 128 MiB of tiles counted the pre-copy-on-write way -- and it is what keeps
  // an ordinary edit's history cost O(entries x layers).
  if (shownBytesCheap() <= budgetBytes_) return;

  // Drop ONE, then re-measure. core/TileShare.hpp's finding is that exclusive
  // bytes are a lower bound on a multi-entry eviction and never additive, so
  // there is no arithmetic that says how many entries an overrun is worth: a
  // tile two doomed entries share becomes free only when the second goes.
  // Predicting would over-evict; this cannot.
  while (bytes().attributable > budgetBytes_) {
    if (!evictOldest()) break;
  }
}

std::string History::budgetPressure() const {
  const HistoryBytes b = bytes();
  if (b.attributable <= budgetBytes_) return {};

  std::string out = "undo history is over its byte budget: holding " + mib(b.attributable) +
                    " against a " + mib(budgetBytes_) + " budget";
  if (b.exemptFromEviction > 0) {
    out += ", of which " + mib(b.exemptFromEviction) + " is held by " +
           std::to_string(snapshots_.size()) +
           (snapshots_.size() == 1 ? " snapshot" : " snapshots") +
           ", which are exempt from eviction until dismissed";
  }
  if (cursor_ == 0) {
    out += ". Nothing is evictable: the cursor is at the oldest state, and eviction never "
           "discards the current state or anything after it";
  } else {
    out += ". Everything evictable has already been dropped";
  }
  out += ". Nothing has been refused -- an edit that cannot be recorded cannot be undone.";
  return out;
}

}  // namespace np
