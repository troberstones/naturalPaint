#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <utility>

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
// **Copy-on-write landed at PLAN.md Phase 5 step 6** -- the deferral this
// header carried since step 2 ("No COW yet ... today a tile is a plain,
// uniquely-owned 128 KiB buffer") is closed, and the argument is in the
// `TileStoreOf` comment below rather than here so it sits next to the code
// that implements it.
//
// Still explicitly NOT here, and each is somebody else's step:
//   - Residency, eviction, or LRU paging. That's Phase 4's OIIO ImageCache
//     territory, and it landed *outside* this header as io/TileResidency --
//     see "How this relates to io/TileResidency" below, which is the one
//     question a reviewer should ask about two overlapping copy-on-write
//     mechanisms.
//   - A mip pyramid. That's Phase 2 step 9, later in this same phase.
//   - A dirty set, an undo entry, a history list, compression, mmap spill.
//     Phase 5 step 7 owns all of those; what this header owes it is a
//     *sharing primitive with a bytes number*, and nothing more.
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
//
// ==========================================================================
// PLAN.md Phase 5 step 6 -- copy-on-write
// ==========================================================================
//
// --- The sharing unit is one tile, and the refcount is inside the slot ----
//
// The map's value went from `T` to `std::shared_ptr<T>`. That single change
// is the whole mechanism, and each half of it is a decision:
//
//  * **The unit is a tile, not a store and not a layer.** The point of the
//    step is that "a history entry, a duplicated layer, or a journal snapshot
//    costs only the tiles that actually differ", and a coarser unit cannot
//    deliver that: sharing whole *stores* would make the first texel written
//    after a snapshot clone the entire layer (32.0 MiB for the 256-tile
//    document io/TileResidency uses as its realistic fixture), which is the
//    cost this step exists to remove. A *finer* unit than a tile has nothing
//    to recommend it -- 128 KiB is already the granularity every other part
//    of this codebase dirties, allocates, saves and uploads at
//    (core/Tile.hpp: 128 is "small enough that a short stroke's write
//    amplification ... stays cheap", and it is the on-disk EXR tile size).
//
//  * **The count lives in the shared_ptr's control block, not on the tile.**
//    core/Tile, core/PigmentTile and core/MaskTile each carry a
//    `static_assert` pinning them to "nothing but their texel buffer" --
//    128 KiB, 224 KiB, 32 KiB exactly -- and io/TileResidency depends on that
//    (a cached fetch is "a memcpy of half words with no conversion stage").
//    An intrusive refcount member would break all three assertions, change
//    the on-disk-to-in-memory size relationship, and put mutable bookkeeping
//    inside the object that gets memcpy'd. Out-of-line is the only option
//    that leaves the tile types alone, and `std::shared_ptr` is the standard
//    library's out-of-line refcount. `make_shared` puts the control block and
//    the tile in one allocation, so the overhead is one allocation's header
//    plus 16 bytes of control block against a 128 KiB payload -- 0.02 %,
//    against the ~1.6 KiB of `unordered_map` node + hash bucket already spent
//    per tile.
//
//    Rejected: a hand-rolled intrusive refcount, which would be smaller and
//    could be non-atomic. It is not worth writing lifetime code by hand for
//    an allocation this large; the counter is not the cost (measured -- see
//    `--selftest`'s `cow tiles` section, where a document copy is dominated
//    by `unordered_map` node insertion, not by the increments).
//
// --- Copying a store IS the share, and that is the whole API --------------
//
// There is no `share()` function, no `snapshot()` and no handle type. The
// copy constructor and copy assignment do it, because *every existing deep
// copy in this codebase is already spelled as a copy*: `core::duplicateLayer`
// says `Layer copy = doc.layers[index]`, `app::duplicateDocument` says
// `copy.document = source.document`, and app/Journal's deferred background
// writer would say the same thing. Those three call sites got copy-on-write
// with no edit at all, which is the strongest evidence that the seam is in
// the right place: a mechanism that needed every caller to opt in would have
// been a mechanism most callers forgot.
//
// The cost of that choice, stated rather than hidden: a copy that a reader
// *wants* to be deep is now not, and there is exactly one way to ask for the
// old behaviour -- `unshareAll()` below (and `unshareDocumentTiles()` in
// core/TileShare.hpp). Nothing in this build wants one; `--selftest` uses it
// to measure the baseline this step is judged against.
//
// --- What "write" means, and every path that is one ----------------------
//
// Sharing is only sound if *every* mutating path passes the barrier. There
// are exactly two ways to obtain a non-const `T` from a store, and both are
// barriers:
//
//   getOrCreate(coord)    -- allocate-or-unshare, then hand out `T&`.
//   findForWrite(coord)   -- unshare if present, then hand out `T*`; null
//                            when the tile does not exist.
//
// `find()` is const-only now (it used to have a non-const overload returning
// `T*`, which would have been an unbarriered write handle), and iteration is
// const-only for the same reason -- see `begin()` below. So the enumeration
// of writers in this build is closed and short:
//
//   io/ImageIO::writeDecodedImageIntoLayer   getOrCreate per texel
//   io/NpaintFile's four unpackers           getOrCreate per tile
//                                            (RGB x2, mask, pigment)
//   io/TileResidency::tileForWrite           findForWrite, then getOrCreate
//                                            for the promotion -- see below
//   core/LayerOps::addLayerMask              creates an empty store
//   app/SelfTest                             getOrCreate per texel/tile
//
// and that is all of them, because `sim::PaintSim` still owns one dense GPU
// texture and no stroke reaches a `Layer` (core/Layer.hpp says so at length).
// When the canvas bridge lands it will write through `getOrCreate` like every
// other writer, and get copy-on-write for free.
//
// **The one rule a caller can still break, and it is the iterator-invalidation
// rule in a new costume:** a `T&` obtained from a barrier stays a live write
// handle to the tile it named, so copying the store *after* taking the
// reference and writing *through* it afterwards writes into a tile the copy
// now shares. Take the reference, write, then copy -- never the other order.
// No path in this build does otherwise (the three production writers above
// each hold their reference for the duration of one texel or one tile fill,
// with no store copy in between).
//
// --- What `find()`'s pointer means now -----------------------------------
//
// io/TileResidency chose its own seam partly to avoid "silently turning
// `find()`'s `const Tile*` into a pointer that can go stale", so this owes a
// precise answer. The pointer's *validity* is *extended*, never shortened: it
// used to die with the one store that owned the tile, and now survives as
// long as any sharer does. What is new is that it can become **detached** --
// if the store it came from later unshares that coordinate, the store's
// current tile is a different object and the old pointer keeps showing the
// pre-write value. It never dangles, and it is never garbage.
//
// Every existing holder is safe, and not by luck:
//   core/Composite  hoists the tile lookup out of its texel loop and writes
//                   nothing during a walk.
//   core/Probe      one lookup, one read.
//   core/Histogram  one lookup per tile, read-only by construction.
//   io/NpaintFile   iterates for packing; the loader writes through
//                   getOrCreate and holds nothing across it.
//   io/TileResidency::readTile -- already documents exactly this rule for
//                   `TileFetch::tile`: "valid until that store is mutated".
//                   That sentence was written before this step and is still
//                   the correct one.
//
// --- Thread safety: the count is atomic, the map is not ------------------
//
// `std::shared_ptr`'s refcount is atomic, so this is what holds and what does
// not:
//
//   NOT SAFE -- two threads touching the *same* store when either mutates.
//     Unchanged from before this step: `unordered_map` is not thread-safe and
//     nothing here made it so.
//   NOT SAFE -- reading through a raw `const T*` while another thread writes
//     that same tile through the store it came from. Hold a *copy of the
//     store* instead (that is a share, and a shared tile can only be written
//     through a fresh copy), not a pointer.
//   SAFE, and this is the property Phase 5 step 7 needs -- **destroying a
//     snapshot store on one thread while another thread mutates a store that
//     shares its tiles.** The mutator's `use_count() > 1` test is
//     conservative in the only direction that matters: if it observes 2 it
//     copies (correct, and at worst one wasted copy), and if it observes 1
//     then the other thread's release is already visible and no other owner
//     exists (correct). Two threads each mutating a *different* store that
//     shares a tile is safe for the same reason -- both observe >= 2 and both
//     copy.
//
// So step 7 may evict a history tail, and app/Journal may write a snapshot,
// on a background thread, provided each holds its own store copy. What is
// still forbidden is the thing app/Journal already says is forbidden ("a
// second thread writing documents needs a rule about what may mutate a
// document while it is being written") -- and the rule this header supplies
// is exactly: *take a copy on the owning thread, hand the copy over*. The
// copy is now cheap enough to make that the obvious design rather than the
// expensive one.
//
// The atomics are not free and the cost is measured rather than waved at:
// `--selftest` prints the per-tile cost of a shared copy, which is one atomic
// increment plus one `unordered_map` node insertion, and the increment is the
// small half.
//
// --- How this relates to io/TileResidency, which also has an owned/clean
//     distinction and its own copy-on-first-write ------------------------
//
// Two copy-on-write mechanisms in one codebase is a smell, so: they are
// deliberately different things, and they compose rather than overlap.
//
//   This one answers **"who else in this process holds these bytes?"** Its
//   copy materialises from another in-memory tile, it is always possible, and
//   it cannot fail except for `bad_alloc`.
//
//   io/TileResidency's answers **"where do these bytes come from?"** Its
//   promotion materialises from a *file*, through OpenImageIO's `ImageCache`,
//   and it can fail -- and its most important line is that a promotion whose
//   clean read fails "returns nullptr, it does not start from zeros", because
//   the alternative silently erases the pixels under a stroke. There is no
//   in-memory refcount that can express that failure, and no file that a
//   refcount reaching zero should reach back into.
//
// They stack: `LayerResidency::owned_` is a `TileStore`, so the tiles it has
// promoted are themselves shareable by this mechanism, and `tileForWrite()`
// goes through `findForWrite()`/`getOrCreate()` like every other writer -- so
// promoting a tile in a residency whose owned store was copied unshares
// first, and the copy does not see the promotion. Unifying them would mean
// putting a file-backed residency backend *inside* this template, which is
// the design io/TileResidency.hpp rejected for three stated reasons, the
// first of which ("core/ is the domain model and knows nothing about files")
// this step does not get to overturn.
template <class T>
class TileStoreOf {
 public:
  using TileType = T;

  // A slot is never null: `getOrCreate()` constructs the tile before
  // inserting, so a throwing allocation leaves the map untouched rather than
  // leaving an empty slot that `find()` would have to special-case.
  using Slot = std::shared_ptr<T>;
  using Map = std::unordered_map<TileCoord, Slot>;

  TileStoreOf() = default;

  // **These four are the sharing API.** Copying shares every tile with the
  // source in O(tiles) refcount increments; moving transfers them. Defaulted
  // rather than written out, because the correct behaviour is exactly what
  // the members do.
  TileStoreOf(const TileStoreOf&) = default;
  TileStoreOf& operator=(const TileStoreOf&) = default;
  TileStoreOf(TileStoreOf&&) noexcept = default;
  TileStoreOf& operator=(TileStoreOf&&) noexcept = default;

  // Finds the tile at `coord`, allocating a zero-initialized one if it
  // doesn't exist yet (128 KiB for a `Tile`, 224 KiB for a `PigmentTile`,
  // 32 KiB of 1.0 for a `MaskTile` -- whose default is *reveal*, not zero,
  // and which therefore stays correct here for free: a copy-on-write clone is
  // a copy of the tile, never a fresh default, so a shared all-reveal mask
  // tile written through one store leaves the other reading 1.0 exactly as
  // core/Mask.hpp requires).
  //
  // This is the "allocate on write" requirement AND the copy-on-write
  // barrier: every write path goes through here or `findForWrite()`, and
  // never through `find()`.
  T& getOrCreate(TileCoord coord) {
    const auto it = tiles_.find(coord);
    if (it == tiles_.end()) {
      Slot fresh = std::make_shared<T>();
      T& ref = *fresh;
      tiles_.emplace(coord, std::move(fresh));
      return ref;
    }
    unshare(it->second);
    return *it->second;
  }

  // Finds the tile at `coord` WITHOUT allocating one if it's absent --
  // returns nullptr rather than default-constructing a tile. This is the
  // "query without allocating" requirement: a naive `tiles_[coord]` read
  // would silently allocate a fresh 128 KiB tile on every miss (that's
  // exactly what operator[] does), which is precisely the bug PLAN.md's
  // wording calls out.
  //
  // Const-only as of Phase 5 step 6. The non-const overload that used to sit
  // here returned a `T*` nobody could tell from a read, which after
  // copy-on-write is an unbarriered write handle; `findForWrite()` is that
  // overload renamed so the barrier is visible at the call site.
  const T* find(TileCoord coord) const noexcept {
    const auto it = tiles_.find(coord);
    return it == tiles_.end() ? nullptr : it->second.get();
  }

  // The write half of `find()`: null when the tile does not exist, and
  // otherwise a writable tile this store alone holds, copying first if it was
  // shared.
  T* findForWrite(TileCoord coord) {
    const auto it = tiles_.find(coord);
    if (it == tiles_.end()) return nullptr;
    unshare(it->second);
    return it->second.get();
  }

  // Shares `src`'s tile at `coord` into this store WITHOUT copying it: both
  // stores end up pointing at the same tile, and the first write through
  // either one unshares it. Returns false when `src` holds no such tile.
  //
  // This is the selective form of the copy constructor, which shares
  // everything. It exists for PRD M5, which requires the clipboard to hold "a
  // copy-on-write tile reference, **not** a flattened buffer" and calls that a
  // Lightweight requirement rather than a convenience -- a 4K full-document
  // copy is 68 MB at rgba16float, and PRD A5 forbids holding that invisibly.
  // A clipboard cannot use the copy constructor, because it takes only the
  // tiles a selection covers.
  //
  // Safe by construction rather than by care at the call site: `unshare()` is
  // the single barrier every write path already goes through, and it triggers
  // on `use_count() > 1` -- which is exactly what this creates.
  bool shareTileFrom(const TileStoreOf& src, TileCoord coord) {
    const auto it = src.tiles_.find(coord);
    if (it == src.tiles_.end()) return false;
    tiles_[coord] = it->second;  // shared_ptr copy: one refcount increment
    return true;
  }

  // How many tiles are actually allocated right now.
  size_t occupiedTileCount() const noexcept { return tiles_.size(); }

  // --- Copy-on-write accounting, which is what Phase 5 step 7 needs -------
  //
  // Step 7's undo history is bounded in *bytes* (PRD A9), so it has to be
  // able to ask a stored document what it costs. The honest answer is two
  // numbers, not one, and confusing them is how a byte-bounded history
  // over-evicts:
  //
  //   tileBytes()           what this store *shows* -- every tile it refers
  //                         to, shared or not. The size of the picture.
  //   exclusiveTileBytes()  what freeing this store would actually give back
  //                         -- only the tiles no one else refers to. The size
  //                         of the entry.
  //
  // For a fresh, unshared store the two are equal. For a history entry that
  // differs from its neighbour by one stroke, the second is the one that is
  // small, and it is the one an eviction budget must spend.
  size_t tileBytes() const noexcept { return tiles_.size() * sizeof(T); }

  size_t exclusiveTileBytes() const noexcept {
    size_t bytes = 0;
    for (const auto& entry : tiles_) {
      if (entry.second.use_count() == 1) bytes += sizeof(T);
    }
    return bytes;
  }

  size_t sharedTileCount() const noexcept {
    size_t n = 0;
    for (const auto& entry : tiles_) {
      if (entry.second.use_count() > 1) ++n;
    }
    return n;
  }

  // How many stores (including this one) refer to this coordinate's tile;
  // 0 when it does not exist here. 1 means "mine alone, a write is in place".
  size_t tileUseCount(TileCoord coord) const noexcept {
    const auto it = tiles_.find(coord);
    return it == tiles_.end() ? 0 : static_cast<size_t>(it->second.use_count());
  }

  bool isTileShared(TileCoord coord) const noexcept { return tileUseCount(coord) > 1; }

  // Makes every tile in this store unique, copying each one that is shared.
  // The explicit way to spell the deep copy that `TileStoreOf(const
  // TileStoreOf&)` used to be implicitly, for a caller that genuinely wants
  // the bytes -- and the baseline `--selftest` measures copy-on-write
  // against, since a `copy + unshareAll()` is exactly the old behaviour and
  // can be timed in the same binary as the new one.
  void unshareAll() {
    for (auto& entry : tiles_) unshare(entry.second);
  }

  // Moves every tile by a whole number of **tiles**, re-keying the map and
  // **copying nothing** (PLAN.md Phase 5 step 11; core/LayerGeometry owns the
  // general operation this is the fast half of).
  //
  // The one translation of a layer's pixels that costs no bytes at all: the
  // slots move between keys, so a shared tile stays shared and an exclusive one
  // stays exclusive. `--selftest` measures it beside the sub-tile path, which
  // has to gather across tile boundaries and therefore has to copy.
  //
  // Deliberately **not** a general translate: a sub-tile shift is not
  // expressible as a re-key, and pretending it were is how a one-pixel nudge
  // silently becomes a 128-pixel one. core/LayerGeometry calls this only when
  // both deltas are exact multiples of `kTileSize`.
  void rekeyTiles(int32_t deltaTileX, int32_t deltaTileY) {
    if (deltaTileX == 0 && deltaTileY == 0) return;
    Map moved;
    moved.reserve(tiles_.size());
    for (auto& entry : tiles_) {
      moved.emplace(TileCoord{entry.first.x + deltaTileX, entry.first.y + deltaTileY},
                    std::move(entry.second));
    }
    tiles_ = std::move(moved);
  }

  // --- Iteration ----------------------------------------------------------
  //
  // Iterates exactly the allocated tiles -- begin()/end() so a plain
  // range-for works: `for (const auto& [coord, tile] : store) ...`, with
  // `tile` a `const T&` exactly as before this step. Nothing here iterates a
  // bounding rectangle or a canvas size; there is no such thing at this
  // layer, only the sparse set of tiles that exist.
  //
  // **Const-only, in both overloads**, and the proxy below exists to make
  // that free of churn. A non-const `begin()` handing out `T&` would be a
  // third way to write a tile with no barrier in front of it, and the whole
  // soundness argument above is an enumeration of the ways to get a mutable
  // tile. Yielding `std::pair<const TileCoord&, const T&>` rather than the
  // map's own `pair<const TileCoord, shared_ptr<T>>` is what lets every
  // existing `for (const auto& [coord, tile] : ...)` site in core/Composite,
  // core/Histogram, io/NpaintFile, ui/NaturalPaintUI and app/SelfTest --
  // twenty-two of them -- keep saying `tile.readPixel(...)` instead of
  // `tile->readPixel(...)`: the shared_ptr is an implementation detail of the
  // slot, not of the iteration.
  class ConstIterator {
   public:
    using MapIterator = typename Map::const_iterator;
    using value_type = std::pair<const TileCoord&, const T&>;
    using reference = value_type;
    using pointer = void;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    ConstIterator() = default;
    explicit ConstIterator(MapIterator it) noexcept : it_(it) {}

    reference operator*() const noexcept { return reference(it_->first, *it_->second); }
    ConstIterator& operator++() noexcept {
      ++it_;
      return *this;
    }
    ConstIterator operator++(int) noexcept {
      ConstIterator copy = *this;
      ++it_;
      return copy;
    }
    friend bool operator==(const ConstIterator& a, const ConstIterator& b) noexcept {
      return a.it_ == b.it_;
    }
    friend bool operator!=(const ConstIterator& a, const ConstIterator& b) noexcept {
      return a.it_ != b.it_;
    }

   private:
    MapIterator it_{};
  };

  ConstIterator begin() const noexcept { return ConstIterator(tiles_.begin()); }
  ConstIterator end() const noexcept { return ConstIterator(tiles_.end()); }

 private:
  // The barrier itself, in one place so `getOrCreate`, `findForWrite` and
  // `unshareAll` cannot drift. `use_count() > 1` is the whole test; see the
  // thread-safety section above for why observing a stale count is safe in
  // the one concurrent case this supports.
  static void unshare(Slot& slot) {
    if (slot.use_count() > 1) slot = std::make_shared<T>(*slot);
  }

  Map tiles_;
};

// The 4-channel rgba16float store every existing caller means by `TileStore`.
// The `PigmentTile` instantiation is `PigmentTileStore`, declared in
// core/Pigment.hpp beside the tile it stores rather than here, so this header
// keeps knowing nothing about pigment.
using TileStore = TileStoreOf<Tile>;

}  // namespace np
