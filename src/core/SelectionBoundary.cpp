#include "core/SelectionBoundary.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace np {
namespace {

// The scan buffer is the tile plus a one-texel apron, so the four neighbour
// tests below are four contiguous byte reads with no bounds branch and no hash
// lookup in the inner loop.
//
// **This is the whole performance story of the file**, so it is worth saying
// why the obvious version is not good enough. Testing four neighbours by
// calling `selectionCoverageAt()` costs a hash lookup per neighbour per texel:
// on the realistic worst case this measures against -- a full-canvas selection
// on 2048x2048, which is 256 tiles and 4.19 M texels -- that is 16.8 M hash
// lookups, and it does not fit in PRD F3's 20 ms whatever the cache does with
// the answer. Filling an apron costs one pass over the tile plus four edge
// rows, and turns the inner loop into array indexing.
//
// The apron's corners are never read: the edge test is 4-connected, so only the
// four side neighbours matter. They are filled with zero like everything else
// and left alone.
constexpr int32_t kPaddedSize = kTileSize + 2;

// A grid vertex packed into one integer key. A bijection on the pair of int32s
// -- not a hash -- so distinct vertices cannot collide, which matters because
// the walk below identifies a contour's closure by vertex equality.
uint64_t packVertex(BoundaryVertex v) noexcept {
  return (static_cast<uint64_t>(static_cast<uint32_t>(v.x)) << 32) |
         static_cast<uint64_t>(static_cast<uint32_t>(v.y));
}

// The 2-D cross product of two directions. Used for exactly two things: telling
// a turning point from a collinear one during the collapse (zero means
// straight), and resolving the one ambiguous vertex during the walk. Both are
// integer arithmetic on unit axis vectors, so both are exact.
int32_t crossOf(BoundaryVertex a, BoundaryVertex b) noexcept {
  return a.x * b.y - a.y * b.x;
}

// The directed edges leaving one grid vertex.
//
// **Two is the maximum, and it is a parity result rather than an assumption.**
// Four texels meet at a grid vertex; going round them, an incident grid edge is
// a boundary edge exactly when the pair of texels it separates disagree, and
// the number of disagreements around a cycle of four is always even -- 0, 2 or
// 4. Every boundary edge is emitted once, with a consistent orientation, so
// out-degree equals in-degree equals half of that: 0, 1 or 2. The guard in
// `addEdge()` below is therefore unreachable; it is there so that a later
// change to the orientation rule fails a test rather than writing past the end
// of this array.
struct VertexOut {
  BoundaryVertex to[2]{};
  bool used[2]{false, false};
  int count = 0;
};

}  // namespace

SelectionBoundary extractSelectionBoundary(const Selection& selection, float threshold) {
  SelectionBoundary out;

  // --- the threshold, converted once into the store's own units ------------
  //
  // Coverage is stored as a uint8 and read back as `v * (1/255)`, so
  // `v/255 >= threshold` is exactly `v >= 255*threshold`, and the smallest
  // whole `v` that satisfies it is `ceil(255*threshold)`. Comparing bytes
  // rather than floats is not a micro-optimisation dressed up as a decision:
  // it is what turns the inner loop below into a byte compare that the compiler
  // vectorises, and it measured as most of the difference between an extraction
  // that fits inside PRD F3's 20 ms frame and one that does not.
  //
  // It is also exact, which the float form is not obviously: the comparison now
  // happens on integers, so no rounding of `v * (1/255)` can put a texel on the
  // wrong side of the line, and the same selection always yields the same
  // boundary on every machine.
  //
  // **Clamped to at least 1.** A threshold of zero would make the unbounded
  // region outside the selection count as selected -- an absent tile reads 0.0
  // (core/SelectionMask.hpp's inverted default), the apron below is zero-filled
  // to match, and `>= 0` is true of both -- so the boundary would collapse to
  // nothing for every input. That is the most confusing possible failure: the
  // tool works, the selection works, and the ants are simply gone. At 1, "above
  // zero" means "any coverage at all" and keeps its natural reading.
  const float scaled = threshold * 255.0f;
  const int thresholdInt =
      scaled <= 1.0f ? 1 : (scaled >= 255.0f ? 255 : static_cast<int>(std::ceil(scaled)));
  const uint8_t thresholdByte = static_cast<uint8_t>(thresholdInt);

  // Sorted rather than taken in hash order. See the header on determinism: the
  // edge set does not depend on visit order, but the contour order and each
  // contour's starting vertex do, and a golden image that depends on hash
  // seeding is a golden image that flakes.
  std::vector<TileCoord> coords;
  coords.reserve(selection.tiles.occupiedTileCount());
  for (const auto& [coord, tile] : selection.tiles) {
    (void)tile;
    coords.push_back(coord);
  }
  std::sort(coords.begin(), coords.end(), [](TileCoord a, TileCoord b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });

  std::unordered_map<uint64_t, VertexOut> outgoing;
  // One entry per directed edge, in generation order. The walk needs a
  // deterministic list of places to start contours from, and the hash map
  // cannot supply one.
  std::vector<BoundaryVertex> edgeStarts;

  auto addEdge = [&](BoundaryVertex from, BoundaryVertex to) {
    VertexOut& v = outgoing[packVertex(from)];
    if (v.count >= 2) return;  // unreachable -- see VertexOut
    v.to[v.count] = to;
    ++v.count;
    edgeStarts.push_back(from);
    ++out.unitEdgeCount;
  };

  std::vector<uint8_t> inside(static_cast<size_t>(kPaddedSize) * kPaddedSize, 0);

  for (const TileCoord coord : coords) {
    const SelectionTile* tile = selection.tiles.find(coord);
    if (tile == nullptr) continue;
    const SelectionTile* above = selection.tiles.find(TileCoord{coord.x, coord.y - 1});
    const SelectionTile* below = selection.tiles.find(TileCoord{coord.x, coord.y + 1});
    const SelectionTile* leftT = selection.tiles.find(TileCoord{coord.x - 1, coord.y});
    const SelectionTile* rightT = selection.tiles.find(TileCoord{coord.x + 1, coord.y});

    // Zero everywhere first, which is also the correct apron value for a
    // neighbour tile that does not exist: absent means outside.
    std::fill(inside.begin(), inside.end(), static_cast<uint8_t>(0));
    const uint8_t* src = tile->data();
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const size_t row = static_cast<size_t>(ly + 1) * kPaddedSize;
      const uint8_t* srcRow = src + static_cast<size_t>(ly) * kTileSize;
      for (int32_t lx = 0; lx < kTileSize; ++lx)
        inside[row + static_cast<size_t>(lx) + 1] = srcRow[lx] >= thresholdByte ? 1 : 0;
    }
    if (above != nullptr) {
      const uint8_t* r = above->data() + static_cast<size_t>(kTileSize - 1) * kTileSize;
      for (int32_t lx = 0; lx < kTileSize; ++lx)
        inside[static_cast<size_t>(lx) + 1] = r[lx] >= thresholdByte ? 1 : 0;
    }
    if (below != nullptr) {
      const size_t row = static_cast<size_t>(kTileSize + 1) * kPaddedSize;
      const uint8_t* r = below->data();
      for (int32_t lx = 0; lx < kTileSize; ++lx)
        inside[row + static_cast<size_t>(lx) + 1] = r[lx] >= thresholdByte ? 1 : 0;
    }
    if (leftT != nullptr) {
      const uint8_t* d = leftT->data();
      for (int32_t ly = 0; ly < kTileSize; ++ly)
        inside[static_cast<size_t>(ly + 1) * kPaddedSize] =
            d[static_cast<size_t>(ly) * kTileSize + kTileSize - 1] >= thresholdByte ? 1 : 0;
    }
    if (rightT != nullptr) {
      const uint8_t* d = rightT->data();
      for (int32_t ly = 0; ly < kTileSize; ++ly)
        inside[static_cast<size_t>(ly + 1) * kPaddedSize + kTileSize + 1] =
            d[static_cast<size_t>(ly) * kTileSize] >= thresholdByte ? 1 : 0;
    }

    // The orientation is fixed and consistent: each texel's four sides are
    // emitted head-to-tail round the texel (top rightwards, right downwards,
    // bottom leftwards, left upwards, in a y-down document). That consistency
    // is what makes the linking below a matter of following endpoints rather
    // than of searching, and it is what makes in-degree equal out-degree at
    // every vertex.
    const PixelCoord origin = tileOrigin(coord);
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const size_t row = static_cast<size_t>(ly + 1) * kPaddedSize;
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const size_t c = row + static_cast<size_t>(lx) + 1;
        if (inside[c] == 0) continue;
        const int32_t x = origin.x + lx;
        const int32_t y = origin.y + ly;
        if (inside[c - kPaddedSize] == 0) addEdge({x, y}, {x + 1, y});
        if (inside[c + 1] == 0) addEdge({x + 1, y}, {x + 1, y + 1});
        if (inside[c + kPaddedSize] == 0) addEdge({x + 1, y + 1}, {x, y + 1});
        if (inside[c - 1] == 0) addEdge({x, y + 1}, {x, y});
      }
    }
  }

  // --- link the edges into closed loops, collapsing collinear runs ---------
  for (const BoundaryVertex start : edgeStarts) {
    {
      const auto it = outgoing.find(packVertex(start));
      if (it == outgoing.end()) continue;
      bool anyFree = false;
      for (int i = 0; i < it->second.count; ++i)
        if (!it->second.used[i]) anyFree = true;
      if (!anyFree) continue;
    }

    std::vector<BoundaryVertex> walk;
    BoundaryVertex cur = start;
    BoundaryVertex dir{0, 0};
    bool haveDir = false;

    while (true) {
      const auto it = outgoing.find(packVertex(cur));
      if (it == outgoing.end()) break;
      VertexOut& vo = it->second;

      int slot[2] = {-1, -1};
      int free = 0;
      for (int i = 0; i < vo.count; ++i)
        if (!vo.used[i]) slot[free++] = i;
      // Only reachable at the vertex the contour started from, once its last
      // edge has been consumed -- every other vertex has as many exits as
      // entrances, so an entered vertex always has one left.
      if (free == 0) break;

      int chosen = slot[0];
      if (free == 2 && haveDir) {
        // The checkerboard vertex: two selected texels meeting at a corner. The
        // header states the choice -- keep them as separate contours, matching
        // the 4-connectivity the edge test itself uses. Concretely that is the
        // turn that closes the texel we arrived along, which under this file's
        // orientation is the candidate with a positive cross against the
        // incoming direction. Both candidates get walked eventually; this only
        // decides which loop each ends up in.
        const BoundaryVertex d0{vo.to[slot[0]].x - cur.x, vo.to[slot[0]].y - cur.y};
        chosen = crossOf(dir, d0) > 0 ? slot[0] : slot[1];
      }

      const BoundaryVertex next = vo.to[chosen];
      vo.used[chosen] = true;
      walk.push_back(cur);
      dir = BoundaryVertex{next.x - cur.x, next.y - cur.y};
      haveDir = true;
      cur = next;
      if (cur == start) break;
    }

    const size_t n = walk.size();
    if (n == 0) continue;

    // Keep only the turning points. A run of collinear unit edges is one
    // segment to draw and one segment to measure arc length along; keeping all
    // of them would make a full-canvas outline 8192 line calls a frame instead
    // of four. The seam is handled by the wrap-around indices rather than
    // afterwards, so a corner that happens to fall on the walk's first vertex
    // is not silently kept.
    BoundaryContour contour;
    contour.vertices.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const BoundaryVertex prev = walk[(i + n - 1) % n];
      const BoundaryVertex here = walk[i];
      const BoundaryVertex after = walk[(i + 1) % n];
      const BoundaryVertex d0{here.x - prev.x, here.y - prev.y};
      const BoundaryVertex d1{after.x - here.x, after.y - here.y};
      if (crossOf(d0, d1) == 0) continue;
      contour.vertices.push_back(here);
    }
    if (!contour.vertices.empty()) out.contours.push_back(std::move(contour));
  }

  return out;
}

const SelectionBoundary& SelectionBoundaryCache::boundaryFor(const Selection* selection,
                                                             uint64_t documentId,
                                                             uint64_t selectionRevision) {
  if (primed_ && documentId == documentId_ && selectionRevision == revision_) return boundary_;
  boundary_ = selection != nullptr ? extractSelectionBoundary(*selection) : SelectionBoundary{};
  documentId_ = documentId;
  revision_ = selectionRevision;
  primed_ = true;
  ++extractions_;
  return boundary_;
}

}  // namespace np
