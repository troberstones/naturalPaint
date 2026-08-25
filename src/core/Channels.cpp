#include "core/Channels.hpp"

#include <string>
#include <utility>

#include "core/Document.hpp"

// core/Channels -- the bodies. Every decision is argued in the header; what is
// here is the arithmetic, plus the two places where an implementation choice
// was forced by something the header could not see.
namespace np {
namespace {

// True when every texel of every stored tile is zero. Shared by
// `channelIsEmpty()` and `selectionFromQuickMask()`'s empty test so the two
// cannot come to different conclusions about the same store -- which they
// would, sooner or later, as one of them grew a short-circuit the other did
// not.
bool coverageIsEmpty(const SelectionTileStore& tiles) noexcept {
  for (const auto& [coord, tile] : tiles) {
    (void)coord;
    if (!tile.selectsNothing()) return false;
  }
  return true;
}

// Rebuilds `tiles` keeping only the tiles that carry something, and returns
// how many were dropped.
//
// **A rebuild rather than an in-place erase, because `TileStoreOf` has no
// erase.** That is not an oversight in this file: the store's whole soundness
// argument is an enumeration of the ways to get a mutable tile out of it, and
// adding a removal path is a change to a template six other modules iterate.
// The rebuild is O(kept tiles) in refcount bumps and **zero** in texel copies,
// because `getOrCreate(c) = tile` on a fresh store copies a 16 KiB tile --
// which is the one thing worth knowing here, so it is spelled: this costs
// 16 KiB memcpy per *kept* tile, not per document tile, and it runs at
// conversion and save boundaries rather than per stroke.
size_t compactCoverage(SelectionTileStore& tiles) {
  SelectionTileStore kept;
  size_t dropped = 0;
  for (const auto& [coord, tile] : tiles) {
    if (tile.selectsNothing()) {
      ++dropped;
      continue;
    }
    kept.getOrCreate(coord) = tile;
  }
  if (dropped != 0) tiles = std::move(kept);
  return dropped;
}

}  // namespace

// --- The channel -------------------------------------------------------------

float channelCoverageAt(const AlphaChannel& channel, PixelCoord docTexel) noexcept {
  // Through `selectionTileCoverage()` and not through a local `tile ? ... : 0`,
  // because that leaf is where core/SelectionMask.hpp put the absent-tile rule
  // and a second copy of it here is exactly the drift that header warns about.
  return selectionTileCoverage(channel.tiles.find(tileCoordAt(docTexel)),
                               tileLocalOffset(docTexel));
}

bool channelIsEmpty(const AlphaChannel& channel) noexcept {
  return coverageIsEmpty(channel.tiles);
}

size_t compactChannel(AlphaChannel& channel) { return compactCoverage(channel.tiles); }

// --- The two conversions (PRD E13) ------------------------------------------

AlphaChannel channelFromSelection(const Selection& selection, std::string name) {
  AlphaChannel channel;
  channel.name = std::move(name);
  // The store copy the header promises. Copy-on-write: this is a refcount per
  // tile, not 16 KiB per tile, and the first write through either side
  // unshares. Nothing is quantised, complemented or dropped, which is what
  // makes the round trip exact on the tile set as well as on the coverage.
  channel.tiles = selection.tiles;
  return channel;
}

Selection selectionFromChannel(const AlphaChannel& channel) {
  Selection selection;
  selection.tiles = channel.tiles;
  return selection;
}

// --- Document-level: save and restore a selection (PRD E11) -----------------

const AlphaChannel* findChannel(const Document& doc, std::string_view name) {
  for (const AlphaChannel& channel : doc.channels) {
    if (channel.name == name) return &channel;
  }
  return nullptr;
}

AlphaChannel* findChannelForWrite(Document& doc, std::string_view name) {
  for (AlphaChannel& channel : doc.channels) {
    if (channel.name == name) return &channel;
  }
  return nullptr;
}

std::string uniqueChannelName(const Document& doc, std::string_view desired) {
  if (!desired.empty() && findChannel(doc, desired) == nullptr) return std::string(desired);

  // An empty request starts numbering at 1 and always carries a number
  // ("Alpha 1"); a taken request keeps its own spelling and starts at 2
  // ("Selection", then "Selection 2"). Two rules rather than one because the
  // stem is a placeholder the user never typed, and a bare "Alpha" alongside
  // "Alpha 2" would read as a different kind of thing rather than the first of
  // a series.
  const std::string stem(desired.empty() ? std::string_view(kDefaultChannelNameStem) : desired);
  for (int n = desired.empty() ? 1 : 2;; ++n) {
    std::string candidate = stem + " " + std::to_string(n);
    if (findChannel(doc, candidate) == nullptr) return candidate;
  }
}

size_t saveSelectionAsChannel(Document& doc, const Selection& selection, std::string name) {
  AlphaChannel channel = channelFromSelection(selection, uniqueChannelName(doc, name));
  // Compacted here and not in `channelFromSelection()`, which the header
  // requires to be verbatim so the round trip is exact. This is the boundary
  // where the coverage stops being transient -- it is about to enter the
  // document, a history snapshot and a file -- so it is the right place, and
  // it is also what the file reader would do on the next load anyway. Doing it
  // here makes save-then-reload an identity instead of nearly one.
  compactChannel(channel);
  doc.channels.push_back(std::move(channel));
  return doc.channels.size() - 1;
}

std::optional<Selection> loadChannelAsSelection(const Document& doc, std::string_view name) {
  const AlphaChannel* channel = findChannel(doc, name);
  // nullopt for "no such channel", and the header says why it must not be an
  // empty Selection: answering a lookup failure with an engaged-empty
  // selection would refuse every edit everywhere with nothing on screen to
  // explain it.
  if (channel == nullptr) return std::nullopt;
  return selectionFromChannel(*channel);
}

// --- Quick mask (PRD E12) ---------------------------------------------------

QuickMask quickMaskFromSelection(const Selection* active) {
  QuickMask mask;
  // A null active selection leaves `mask.coverage` default-constructed, which
  // is an empty store: the blank overlay the user paints into. See the header
  // on why "no restriction" is deliberately *not* read as a full mask here.
  if (active != nullptr) mask.coverage = *active;
  return mask;
}

std::optional<Selection> selectionFromQuickMask(const QuickMask& mask) {
  Selection out = mask.coverage;
  compactCoverage(out.tiles);
  // The empty case, argued at length in the header: both readings of "the mask
  // is blank" -- never painted, or deliberately erased -- want no selection at
  // all rather than a selection that selects nothing.
  if (out.tiles.occupiedTileCount() == 0) return std::nullopt;
  return out;
}

void paintQuickMask(QuickMask& mask, PixelCoord docTexel, float coverage) {
  const TileCoord coord = tileCoordAt(docTexel);
  const PixelCoord local = tileLocalOffset(docTexel);

  // Writing 0.0 into a tile that does not exist would allocate 16 KiB to agree
  // with the default. An eraser dragged across unpainted space is exactly that
  // gesture, so the check is not defensive -- it is the difference between an
  // eraser costing nothing and an eraser allocating the canvas.
  //
  // The comparison is against 0.0f rather than against
  // `SelectionTile::writeCoverage()`'s quantisation of it, because a negative
  // coverage clamps to zero too and must take the same path.
  if (coverage <= 0.0f && mask.coverage.tiles.find(coord) == nullptr) return;

  mask.coverage.tiles.getOrCreate(coord).writeCoverage(local, coverage);
}

float quickMaskCoverageAt(const QuickMask& mask, PixelCoord docTexel) noexcept {
  return selectionTileCoverage(mask.coverage.tiles.find(tileCoordAt(docTexel)),
                               tileLocalOffset(docTexel));
}

}  // namespace np
