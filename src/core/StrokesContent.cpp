#include "core/StrokesContent.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace np {
namespace {

void mix(uint64_t& h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

// Bit patterns, never values -- see `strokesContentHash()`'s header comment.
void mixF(uint64_t& h, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  mix(h, bits);
}

void mixDab(uint64_t& h, const DabRecord& d) {
  mix(h, d.id);
  mix(h, d.strokeId);
  mixF(h, d.x);
  mixF(h, d.y);
  mixF(h, d.radius);
  mixF(h, d.hardness);
  mixF(h, d.roundness);
  mixF(h, d.angle);
  // `edgePx` changes a dab's pixels (brush/Deposit.hpp §2), so a record
  // differing only there must miss both the evaluation cache and
  // core/DirtyTiles -- "every field", as the header promises.
  mixF(h, d.edgePx);
  mixF(h, d.flow);
  for (const float c : d.rgba) mixF(h, c);
  mix(h, static_cast<uint64_t>(d.source));
  mixF(h, d.sourceDx);
  mixF(h, d.sourceDy);
}

// The seed both hashes start from. Shared, so that
// `strokesPrefixHash(c, c.dabs.size())` is exactly the dab half of
// `strokesContentHash(c)` -- a property the checkpoint logic in
// brush/StrokesLayer relies on without ever having to state it twice.
constexpr uint64_t kSeed = 0x9e3779b97f4a7c15ull;

int64_t cellKey(int cx, int cy) noexcept {
  return (static_cast<int64_t>(cy) << 32) |
         static_cast<int64_t>(static_cast<uint32_t>(cx));
}

// Floor-division to a cell index. `x >> 6` would be right for the positive
// half and wrong by one for every negative coordinate, which is the half a
// dab hanging off the top-left of the canvas lives in.
int cellOf(int v) noexcept {
  return static_cast<int>(std::floor(static_cast<double>(v) / StrokesIndex::kCellSize));
}

}  // namespace

DabBounds dabRecordBounds(const DabRecord& dab) noexcept {
  // Every non-finite input answers "covers nothing" rather than propagating a
  // NaN into an int cast, which is undefined behaviour and, on this target,
  // silently produces INT_MIN -- a rectangle spanning the whole coordinate
  // space that a scan would then walk.
  if (!std::isfinite(dab.x) || !std::isfinite(dab.y) || !std::isfinite(dab.radius) ||
      dab.radius <= 0.0f)
    return DabBounds{};
  // The circumscribing square of the MAJOR axis; the header argues why the
  // ellipse's tight box is deliberately not derived here.
  const double r = static_cast<double>(dab.radius);
  const double lo = -1e9, hi = 1e9;
  const double x0 = std::clamp(std::floor(dab.x - r), lo, hi);
  const double y0 = std::clamp(std::floor(dab.y - r), lo, hi);
  const double x1 = std::clamp(std::ceil(dab.x + r) + 1.0, lo, hi);
  const double y1 = std::clamp(std::ceil(dab.y + r) + 1.0, lo, hi);
  DabBounds b;
  b.x0 = static_cast<int>(x0);
  b.y0 = static_cast<int>(y0);
  b.x1 = static_cast<int>(x1);
  b.y1 = static_cast<int>(y1);
  return b;
}

uint64_t strokesPrefixHash(const StrokesContent& content, size_t n) noexcept {
  uint64_t h = kSeed;
  const size_t count = std::min(n, content.dabs.size());
  mix(h, count);
  for (size_t i = 0; i < count; ++i) mixDab(h, content.dabs[i]);
  return h;
}

uint64_t strokesContentHash(const StrokesContent& content) noexcept {
  uint64_t h = strokesPrefixHash(content, content.dabs.size());
  // `nextDabId` last, so the prefix property above holds: the dab half of
  // this hash is exactly the full-length prefix hash.
  mix(h, content.nextDabId);
  return h;
}

StrokesIndex::StrokesIndex(const StrokesContent& content) {
  dabCount_ = content.dabs.size();
  bounds_.reserve(content.dabs.size());
  for (const DabRecord& d : content.dabs) bounds_.push_back(dabRecordBounds(d));
  // Built into a map and then flattened, rather than into the sorted vector
  // directly: insertion into a sorted vector is quadratic in the bucket
  // count, and a long stroke crosses hundreds of cells.
  std::vector<std::pair<int64_t, std::vector<size_t>>> tmp;
  std::vector<std::pair<int64_t, size_t>> pairs;
  pairs.reserve(content.dabs.size() * 2);
  for (size_t i = 0; i < content.dabs.size(); ++i) {
    const DabBounds& b = bounds_[i];
    if (b.empty()) continue;
    const int cx0 = cellOf(b.x0), cy0 = cellOf(b.y0);
    const int cx1 = cellOf(b.x1 - 1), cy1 = cellOf(b.y1 - 1);
    for (int cy = cy0; cy <= cy1; ++cy)
      for (int cx = cx0; cx <= cx1; ++cx) pairs.emplace_back(cellKey(cx, cy), i);
  }
  // One sort, then one pass: the dab indices come out ascending within each
  // bucket for free, which is the ordering `query()` promises its callers and
  // the reason nothing downstream sorts.
  std::sort(pairs.begin(), pairs.end());
  for (const auto& [key, dab] : pairs) {
    if (tmp.empty() || tmp.back().first != key) tmp.emplace_back(key, std::vector<size_t>{});
    tmp.back().second.push_back(dab);
  }
  buckets_ = std::move(tmp);
}

const std::vector<size_t>* StrokesIndex::bucket(int64_t key) const noexcept {
  const auto it = std::lower_bound(
      buckets_.begin(), buckets_.end(), key,
      [](const std::pair<int64_t, std::vector<size_t>>& b, int64_t k) { return b.first < k; });
  if (it == buckets_.end() || it->first != key) return nullptr;
  return &it->second;
}

size_t StrokesIndex::entryCount() const noexcept {
  size_t n = 0;
  for (const auto& b : buckets_) n += b.second.size();
  return n;
}

std::vector<size_t> StrokesIndex::query(int x0, int y0, int x1, int y1) const {
  std::vector<size_t> out;
  if (x1 <= x0 || y1 <= y0 || buckets_.empty()) return out;
  const int cx0 = cellOf(x0), cy0 = cellOf(y0);
  const int cx1 = cellOf(x1 - 1), cy1 = cellOf(y1 - 1);
  for (int cy = cy0; cy <= cy1; ++cy) {
    for (int cx = cx0; cx <= cx1; ++cx) {
      const std::vector<size_t>* b = bucket(cellKey(cx, cy));
      if (b == nullptr) continue;
      // The cell is the broad phase; the stored bounds are the exact test the
      // header promises. Without this line a query returns every dab that
      // merely shared a 64-square cell with a real hit, which is a different
      // set and one no caller asked for.
      for (const size_t i : *b) {
        const DabBounds& db = bounds_[i];
        if (db.x0 < x1 && db.x1 > x0 && db.y0 < y1 && db.y1 > y0) out.push_back(i);
      }
    }
  }
  // A dab spanning several queried cells appears once per cell; the promise
  // is ascending AND unique.
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

size_t eraseDabsUnderDisc(StrokesContent& content, float cx, float cy, float r,
                          std::vector<DabRecord>* removedOut) {
  if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(r) || r <= 0.0f) return 0;
  const float r2 = r * r;
  // The index narrows the CANDIDATES; containment of the centre is then
  // re-tested exactly, which is the broad/narrow split `StrokesIndex::query()`
  // documents. Without the index this is a scan of every dab on the layer per
  // eraser dab, and an eraser emits one every quarter radius.
  const StrokesIndex index(content);
  const DabBounds disc{static_cast<int>(std::floor(cx - r)), static_cast<int>(std::floor(cy - r)),
                       static_cast<int>(std::ceil(cx + r)) + 1,
                       static_cast<int>(std::ceil(cy + r)) + 1};
  const std::vector<size_t> candidates = index.query(disc.x0, disc.y0, disc.x1, disc.y1);
  // A candidate's BOUNDS overlap the disc; its CENTRE may not be inside it.
  // The header argues at length why the centre is the test.
  std::vector<char> doomed(content.dabs.size(), 0);
  size_t removed = 0;
  for (const size_t i : candidates) {
    const DabRecord& d = content.dabs[i];
    const float dx = d.x - cx, dy = d.y - cy;
    if (dx * dx + dy * dy > r2) continue;
    doomed[i] = 1;
    ++removed;
    if (removedOut != nullptr) removedOut->push_back(d);
  }
  if (removed == 0) return 0;
  // One stable compaction rather than `erase()` per hit: paint order is the
  // vector's order (core/StrokesContent.hpp), so the survivors must keep
  // their relative order, and N erases from the middle is quadratic.
  size_t out = 0;
  for (size_t i = 0; i < content.dabs.size(); ++i)
    if (!doomed[i]) content.dabs[out++] = content.dabs[i];
  content.dabs.resize(out);
  // `nextDabId` is deliberately NOT rewound. An id must never be reused --
  // an undo entry, and a future panel selection, name a dab by it, and
  // reusing one would silently reattach a stale reference to a new dab.
  return removed;
}

}  // namespace np
