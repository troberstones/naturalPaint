#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "core/Half.hpp"
#include "core/Tile.hpp"

// core/TileStore (PLAN.md "Phase 2 -- See a file", step 2; DESIGN-imaging.md
// "Sparse copy-on-write tiles"). Two things live here:
//
//   Tile      -- one 128x128 tile's pixel storage: rgba16float, premultiplied
//                alpha (DESIGN-imaging.md §2's colour pipeline decisions),
//                128 KiB once allocated (128*128*4 half floats).
//   TileStore -- the sparse hash map from TileCoord to Tile that a future
//                Layer will hold: allocate on write, query without
//                allocating, iterate only the tiles that exist.
//
// core/Tile.hpp (the previous step) is coordinate geometry only and has no
// pixel storage type -- this header is where that type and the map that
// holds it both live, per Tile.hpp's own header comment ("core/TileStore,
// the actual sparse map, is the next step and isn't here yet").
//
// Explicitly NOT here yet -- per PLAN.md step 2's own "No COW yet", and
// confirmed by where the plan places the adjacent, later features:
//   - Copy-on-write / reference counting. That's Phase 5 step 6, "COW
//     tiles". Today a tile is a plain, uniquely-owned 128 KiB buffer.
//   - Residency, eviction, or LRU paging. That's Phase 4's OIIO ImageCache
//     territory -- every tile a TileStore holds is resident for as long as
//     the store holds it.
//   - A mip pyramid. That's Phase 2 step 9, later in this same phase.
// Building any of the above now would be speculative work for phases that
// haven't been scoped; this header is exactly "a hash map plus
// allocate/query/iterate", the line PLAN.md's step 2 actually asks for.
namespace np {

// One tile's pixel storage. A flat, row-major, channel-interleaved buffer
// of half floats -- texel (x, y)'s four channels sit at
// `[(y*kTileSize + x)*4 .. +4)`, the same layout PaintSim's GPU readback
// already assumes for an RGBA16Float texture (see sim/PaintSim.cpp's
// readbackField). Callers work in ordinary `float`; the half<->float
// conversion happens at the read/write boundary via core/Half, so the
// resident cost matches DESIGN-imaging.md's "128 KiB per 128^2 tile"
// without every caller having to hand-roll the encoding.
//
// Pixel storage only. No coordinate math (Tile.hpp already owns
// document<->tile<->local conversion) and no allocation/occupancy policy
// (TileStore, below, owns that).
class Tile {
 public:
  // Value-initializes every texel to half-precision zero: transparent
  // black under premultiplied alpha, the correct implicit content for a
  // tile no stroke has touched yet.
  Tile() = default;

  static constexpr int32_t kChannels = 4;
  static constexpr size_t kTexelCount =
      static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize) * kChannels;

  // `local` must satisfy `0 <= local.x, local.y < kTileSize` -- exactly
  // what Tile.hpp's tileLocalOffset() produces from a document coordinate.
  // Decodes the four stored halfs to float.
  std::array<float, 4> readPixel(PixelCoord local) const noexcept {
    const size_t base = pixelIndex(local);
    return {halfToFloat(texels_[base + 0]), halfToFloat(texels_[base + 1]),
            halfToFloat(texels_[base + 2]), halfToFloat(texels_[base + 3])};
  }

  // Encodes `rgba` to half and stores it at `local` (same precondition on
  // `local` as readPixel).
  void writePixel(PixelCoord local, const std::array<float, 4>& rgba) noexcept {
    const size_t base = pixelIndex(local);
    texels_[base + 0] = floatToHalf(rgba[0]);
    texels_[base + 1] = floatToHalf(rgba[1]);
    texels_[base + 2] = floatToHalf(rgba[2]);
    texels_[base + 3] = floatToHalf(rgba[3]);
  }

  // Raw half-float storage, for bulk paths (decode-on-import, GPU
  // upload/download) that want to move a whole tile's texels at once
  // rather than pay a function call per channel.
  const uint16_t* data() const noexcept { return texels_.data(); }
  uint16_t* data() noexcept { return texels_.data(); }

 private:
  static size_t pixelIndex(PixelCoord local) noexcept {
    return (static_cast<size_t>(local.y) * static_cast<size_t>(kTileSize) +
            static_cast<size_t>(local.x)) *
           static_cast<size_t>(kChannels);
  }

  std::array<uint16_t, kTexelCount> texels_{};
};

// A Tile is nothing but its texel buffer -- no vtable, no bookkeeping
// members -- so this should come out exactly as DESIGN-imaging.md's table
// says. If this ever fails, something grew the type by accident.
static_assert(sizeof(Tile) == 128 * 1024,
             "one 128x128 rgba16float tile must be exactly 128 KiB (DESIGN-imaging.md §2)");

// Sparse hash map from TileCoord to a tile of type `T`. Keyed with Tile.hpp's
// std::hash<TileCoord> specialization -- that's exactly what it exists for
// (a bare packed() key would leave one 32-bit half zero for single-row/
// column tile sets, which is common: a tall document, or a stroke that
// only moves along one axis).
//
// **Why this became a template at PLAN.md Phase 5 step 3, and why the name
// `TileStore` survived it.** That step needs a second, differently-shaped
// tile: core/Pigment's 7-channel f16 `PigmentTile`, which core/Layer.hpp
// predicted in advance ("Pigment/Media need a *different* shape -- 7 channels
// ... i.e. not a `core::TileStore<core::Tile>` at all"). The map's own
// behaviour -- allocate on write, query without allocating, iterate only what
// exists -- is identical for both, and a copy-pasted sibling class would be
// two implementations of the thing PLAN.md step 1 refused to have two of. So
// the map is a template and `TileStore` is an alias of its `Tile`
// instantiation: **every existing use site compiles unchanged**, which matters
// because `TileStore` appears by name in io/TileResidency, io/NpaintFile,
// io/ImageIO, core/Probe, core/Histogram, core/Layer and app/DocumentLifecycle
// and none of those has any business knowing a template exists.
//
// Naming the template `TileStoreOf<T>` rather than `TileStore<T>` is what
// makes that possible -- a class template *called* `TileStore` would force
// every one of those sites to say `TileStore<Tile>`, which is churn with no
// reader benefit. Step 4's single-channel mask store is the next instantiation
// and needs nothing new here.
template <class T>
class TileStoreOf {
 public:
  using TileType = T;

  // Finds the tile at `coord`, allocating a zero-initialized one if it
  // doesn't exist yet (128 KiB for a `Tile`, 224 KiB for a `PigmentTile`).
  // This is the "allocate on write" requirement: every write path (deposit,
  // import-decode, ...) goes through here, and never through find() below.
  T& getOrCreate(TileCoord coord) { return tiles_[coord]; }

  // Finds the tile at `coord` WITHOUT allocating one if it's absent --
  // returns nullptr rather than default-constructing a tile. This is the
  // "query without allocating" requirement: a naive `tiles_[coord]` read
  // would silently allocate a fresh 128 KiB tile on every miss (that's
  // exactly what operator[] does), which is precisely the bug PLAN.md's
  // wording calls out.
  const T* find(TileCoord coord) const noexcept {
    const auto it = tiles_.find(coord);
    return it == tiles_.end() ? nullptr : &it->second;
  }
  T* find(TileCoord coord) noexcept {
    const auto it = tiles_.find(coord);
    return it == tiles_.end() ? nullptr : &it->second;
  }

  // How many tiles are actually allocated right now.
  size_t occupiedTileCount() const noexcept { return tiles_.size(); }

  // Iterates exactly the allocated tiles -- begin()/end() so a plain
  // range-for works: `for (const auto& [coord, tile] : store) ...`.
  // Nothing here iterates a bounding rectangle or a canvas size; there is
  // no such thing at this layer, only the sparse set of tiles that exist.
  using Map = std::unordered_map<TileCoord, T>;
  typename Map::iterator begin() noexcept { return tiles_.begin(); }
  typename Map::iterator end() noexcept { return tiles_.end(); }
  typename Map::const_iterator begin() const noexcept { return tiles_.begin(); }
  typename Map::const_iterator end() const noexcept { return tiles_.end(); }

 private:
  Map tiles_;
};

// The 4-channel rgba16float store every existing caller means by `TileStore`.
// The `PigmentTile` instantiation is `PigmentTileStore`, declared in
// core/Pigment.hpp beside the tile it stores rather than here, so this header
// keeps knowing nothing about pigment.
using TileStore = TileStoreOf<Tile>;

}  // namespace np
