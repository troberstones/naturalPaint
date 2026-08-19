#pragma once

#include <cstdint>
#include <functional>

// Tile geometry (PLAN.md "Phase 2 -- See a file", step 1; DESIGN-imaging.md
// "Sparse copy-on-write tiles"). The document is a hash map from tile
// coordinate to tile, not a flat buffer -- this header is just the geometry
// and coordinate math that map depends on. core/TileStore, the actual sparse
// map, is the next step and isn't here yet.
namespace np {

// Edge length of a square tile, in document pixels. Signed (not uint32_t,
// despite e.g. PaintSim's kBrushGrid) because it is a divisor/modulus in the
// floor-division math below, and document coordinates can be negative --
// mixing a negative int32_t with an unsigned divisor is a classic silent
// bug (the dividend gets converted to unsigned first).
//
// 128 is DESIGN-imaging.md's recommended default: large enough to keep
// per-tile bookkeeping and dispatch overhead down, small enough that a short
// stroke's write amplification (dirtying and copy-on-write-snapshotting a
// tile it barely touches) stays cheap. It is also the on-disk EXR tile size
// (docs/document-format.md, `np:tileSize 128`), so changing it is a
// file-format decision, not just a memory-layout one. Kept constexpr, per
// the design doc, so it stays a single tunable knob.
inline constexpr int32_t kTileSize = 128;

// A tile's address in the sparse tile grid. Deliberately not just "two
// ints" or reused from a pixel-coordinate type: TileCoord{3, 4} and a
// document pixel at (3, 4) are unrelated quantities (one tile-widths away
// from the origin vs. three pixels), and giving them the same type would
// let them be swapped at a call site with no compiler diagnostic.
struct TileCoord {
  int32_t x = 0;
  int32_t y = 0;

  friend bool operator==(const TileCoord&, const TileCoord&) = default;

  // Combined-integer key, for callers that want one rather than relying on
  // the std::hash specialization below (e.g. an ordered container, or a
  // debug log). int32_t -> uint32_t is a bit-preserving bijection in
  // two's-complement (guaranteed by the standard as of C++20), so packing
  // the two halves side by side is itself a bijection: distinct coordinates
  // never collide, only hash-table buckets can.
  constexpr uint64_t packed() const noexcept {
    return (uint64_t{static_cast<uint32_t>(x)} << 32) | uint64_t{static_cast<uint32_t>(y)};
  }
};

// A 2D document-pixel position. Used both for absolute document coordinates
// and, where noted, for a position relative to a tile's origin -- the two
// are distinguished by context/naming, not by type, since unlike the
// tile/pixel split above there is no compile-time confusion to guard
// against (both are "a pixel", just in different frames).
struct PixelCoord {
  int32_t x = 0;
  int32_t y = 0;

  friend bool operator==(const PixelCoord&, const PixelCoord&) = default;
};

// C++'s / and % truncate toward zero, so -1 / 128 == 0 and -1 % 128 == -1.
// That puts document pixel -1 in tile 0 (same as pixel +1) instead of the
// tile just below zero, and gives it a negative local offset. A document
// whose content can be dragged past the top-left origin needs floor
// semantics instead: strictly decreasing tile index as x/y cross each
// multiple of kTileSize, and an offset that always lands in [0, kTileSize).
constexpr int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a / b;
  const int32_t r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

constexpr int32_t floorMod(int32_t a, int32_t b) {
  const int32_t r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

// Which tile a document pixel falls in. TileStore's "allocate on write"
// needs this to find/create the owning tile.
constexpr TileCoord tileCoordAt(PixelCoord doc) noexcept {
  return TileCoord{floorDiv(doc.x, kTileSize), floorDiv(doc.y, kTileSize)};
}

// The pixel's offset within that same tile, each component in
// [0, kTileSize). Whatever reads or writes the pixel needs this alongside
// tileCoordAt -- the tile coordinate alone only gets you to the tile.
constexpr PixelCoord tileLocalOffset(PixelCoord doc) noexcept {
  return PixelCoord{floorMod(doc.x, kTileSize), floorMod(doc.y, kTileSize)};
}

// The document-pixel coordinate of a tile's top-left corner -- where the
// tile lands when compositing/drawing it.
constexpr PixelCoord tileOrigin(TileCoord tile) noexcept {
  return PixelCoord{tile.x * kTileSize, tile.y * kTileSize};
}

}  // namespace np

namespace std {

// Lets TileStore use TileCoord directly as an unordered_map key
// (std::unordered_map<np::TileCoord, Tile>) instead of every caller having
// to remember to hash .packed() themselves.
template <>
struct hash<np::TileCoord> {
  size_t operator()(const np::TileCoord& t) const noexcept {
    // splitmix64's finalizer over the packed key. A bare packed() would
    // leave one 32-bit half zero whenever tiles form a single row or
    // column (a common case: a tall, narrow document, or a stroke that
    // only ever moves along one axis), which some unordered_map
    // implementations turn into real bucket collisions; this mixes both
    // halves together first.
    uint64_t z = t.packed() + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return static_cast<size_t>(z);
  }
};

}  // namespace std
