#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/Document.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/NpaintFile.hpp"

// io/TileResidency (PLAN.md "Phase 4 -- Write it out", step 5: "Wire OIIO's
// `ImageCache` as the residency layer for unmodified source tiles. This is
// the main reason the dependency earns its cost").
//
// **No OpenImageIO header is included here, and none may be.** Same hard rule
// io/OiioBackend.hpp and io/NpaintFile.hpp both state: io/OiioBackend.cpp is
// the only translation unit in this project permitted to `#include
// <OpenImageIO/...>`, and src/CMakeLists.txt adds it to the target only when
// NP_USE_OIIO is ON. TileResidency.cpp is compiled in *both* configurations
// and guards its own `#include "io/OiioBackend.hpp"` with `#if
// defined(NP_USE_OIIO)`.
//
// --- The critical difference from step 4 ----------------------------------
//
// `.npaint` is inherently an OIIO feature, so io/NpaintFile refuses by name
// in the build that has no backend. **This module is not like that.** Opening
// a PNG and painting on it works identically with NP_USE_OIIO=OFF -- that is
// PRD I1 and I3, and it is already true today. So what is being added here is
// a *residency strategy that is swapped in when OIIO is available*, not a
// feature that appears. `TileResidencyMode::Eager` is today's behaviour,
// restated as one implementation of the interface below, and it is what every
// document gets in the OFF build. Only `Cached` needs the backend, and only
// `Cached` is refused without it.
//
// --- What ADR-0001 actually asks for --------------------------------------
//
// ADR-0001's model is "near-zero memory when nothing is loaded ... a resource
// property, not a limit on features", with an idle-RSS assertion in
// `--selftest` so the invariant fails the build rather than drifting. Its
// amendment adds the bound that matters here: only *visible* documents hold
// GPU textures, at most two, and "view tiles are bounded by viewport rather
// than image size".
//
// This module is the CPU-side half of that same sentence. A tile that came
// from a file and has not been modified is reconstructible from the file, so
// holding it in our own memory makes our footprint track *document size*.
// Serving it from a cache with a fixed budget makes the footprint track *the
// budget*. That is the whole idea, and the measurements below say precisely
// where it pays and where it does not.
//
// --- Where the seam goes, and why not the other place ---------------------
//
// The seam is here, in `io/`, as a layer that sits between a `core::Layer`'s
// tile storage and the file. `core::TileStore` was **not** given a residency
// backend. Three reasons, in decreasing order of force:
//
//  1. `core/` is the domain model and knows nothing about files. io/NpaintFile
//     already made exactly this call for exactly this reason -- `NpaintCarry`
//     is "deliberately NOT a member of `core::Document` ... putting a bag of
//     OpenEXR header attributes on it would make every consumer of Document
//     depend on a file format's shape". A residency backend inside TileStore
//     would be worse: it would make every consumer of a *tile* depend on one.
//  2. `core/TileStore.hpp`'s contract is "every tile a TileStore holds is
//     resident for as long as the store holds it", and that contract is worth
//     keeping true. Putting eviction inside it would silently turn
//     `find()`'s `const Tile*` into a pointer that can go stale, which is a
//     lifetime change to a type used by ops/, ui/, core/Probe and
//     core/Histogram. Layering *above* TileStore leaves all of that alone:
//     the owned store still holds exactly what it says it holds.
//  3. Phase 5 step 6 ("COW tiles -- copy-on-write with reference-counted
//     history") is going to rework tile ownership anyway. Building a second,
//     competing ownership mechanism inside TileStore now would mean building
//     the same decision twice and then reconciling them.
//
// The cost of this choice, stated rather than hidden: a consumer that wants
// clean pixels must go through `LayerResidency` instead of reading
// `Layer::rgbTiles` directly. Today there is no such consumer -- see the
// deferral list.
//
// --- How a tile is identified, and how that survives an edit --------------
//
// A clean tile's identity is `(path, subimage, miplevel, tile origin)`, which
// is exactly the key OpenImageIO's `ImageCache` uses internally. `TileSourceRef`
// carries the first three; the fourth is derived from the `TileCoord` by the
// same `tileOrigin()` core/Tile.hpp already defines, so document space and
// cache space are the same grid rather than two grids that have to be kept in
// step.
//
// That identity survives the document being edited because **editing never
// moves a clean tile; it stops the tile being clean**. `tileForWrite()`
// promotes the tile into the owned `TileStore` and it is never looked up in
// the cache again -- see the copy-on-first-write section. Nothing renumbers,
// re-keys, or invalidates the remaining clean tiles, because nothing about
// them changed.
//
// The one thing that *would* break the mapping is the layer moving in
// document space. It cannot today: `io/ImageIO`'s `writeDecodedImageIntoLayer()`
// places at document origin, and io/NpaintFile only accepts a part as a layer
// when its data window is aligned to the `kTileSize` grid in document
// coordinates. So the data window's origin is carried explicitly below
// (`dataX`/`dataY`, non-zero for a real `.npaint` layer part whose bounding
// box does not start at 0,0) and no other offset exists to get wrong.
//
// --- Copy-on-first-write --------------------------------------------------
//
// The rule, and getting it wrong silently loses paint:
//
//   * `readTile()` never promotes. It answers from the owned store if the
//     tile is owned, and otherwise from the cache, into a single staging
//     tile whose contents are valid only until the next call.
//   * `tileForWrite()` promotes, once, at the first write: it copies the
//     clean tile out of the cache into the owned `TileStore` and returns the
//     owned copy. From that moment `isOwned()` is true and the cache is never
//     consulted for that coordinate again, so a later eviction cannot touch
//     it.
//   * **A promotion whose clean read fails returns `nullptr`, it does not
//     start from zeros.** Zero-filling would mean painting one stroke onto a
//     document whose backing file went missing quietly erased the pixels
//     underneath the stroke. `--selftest` covers this case specifically.
//   * The owned `TileStore`'s own key set *is* the dirty set. There is no
//     second `unordered_set<TileCoord>` to keep in step with it, and no dirty
//     flag on `core::Tile` (which is deliberately "nothing but its texel
//     buffer" and has a static_assert pinning that).
//
// The cache is **not** a write-back cache and must never be treated as one.
// Nothing in this module writes to it, and the only path from an owned tile
// back to a file is io/NpaintFile's `saveNpaint()`.
//
// --- When the file is missing, truncated, or changed ----------------------
//
// Measured against this build, and each one is why the corresponding check
// exists:
//
//  * **Missing / truncated.** `ImageCache::get_pixels()` returns false with a
//    specific error ("Could not open ...", "Some tiles were missing or
//    corrupted") and leaves the destination buffer untouched -- it does not
//    zero it. So the failure is detectable, and this module reports it as
//    `TileFetchStatus::Failed` with that message rather than serving whatever
//    was in the staging tile.
//  * **Changed on disk after open.** The cache does **not** notice. Verified:
//    a file rewritten with different pixels at the same dimensions, under an
//    open cache, serves the *old* pixels indefinitely. That is stale data
//    presented as truth, which is worse than an error, so this module stamps
//    the backing file's size and mtime at open and re-checks them on every
//    cached fetch. A mismatch is `Failed`, naming the file. Cost: one
//    `stat()`, measured at **1.256 us** on this machine (200 000 calls). See
//    the deferral list for the obvious way to stop paying it per tile.
//  * **Outside the data window.** Measured, and the nastiest of the four:
//    `get_pixels()` for a region outside a part's data window returns
//    **success**, having written zeros. Serving that as pixels would turn a
//    layer's transparent surround into "the file says transparent" when the
//    file says nothing at all. This module bounds-checks against the data
//    window itself and answers `Absent`, which is the same answer
//    `TileStore::find()` gives for a tile that does not exist.
//
// --- Deliberate deferrals -------------------------------------------------
//
// Each with a reason and an unblocking condition, in io/NpaintFile.hpp's
// style, because the alternative is a speculative abstraction for layers that
// do not exist:
//
//  * **No document-level residency manager, and no `openResidentDocument()`.**
//    A `Document` would need an owning record holding one `LayerResidency`
//    per layer, and that record is exactly what PLAN.md Phase 4 step 8
//    ("Document lifecycle -- revert, duplicate document, save a copy ... open
//    recent") has to invent, with Phase 5's tabs deciding its lifetime. There
//    is no open-document record in this build at all -- `main.cpp` holds no
//    `Document`. Building one here to hang residencies off would be inventing
//    step 8's central decision as a side effect of this one. `npaintLayerTileSource()`
//    below is the whole of the document-to-file link this step needs, and it
//    is derived from `NpaintCarry::partOrder`, which io/NpaintFile already
//    computed -- nothing here re-derives the part-to-layer rule.
//    *Unblocked by:* step 8's open-document record.
//
//  * **No consumer reads through this module yet.** `core/Probe`,
//    `core/Histogram`, `io/Export`'s flattener and `ui/NaturalPaintUI` all
//    read `Layer::rgbTiles` directly, and converting them is only correct
//    once a document can actually *be* opened in cached mode by the
//    application -- which needs the record above. Stated plainly so it is not
//    mistaken for working: a `Document` returned alongside a cached
//    `LayerResidency` has an **empty** `rgbTiles` until tiles are promoted,
//    and flattening or probing it today would see transparent black. Nothing
//    in this build does, because nothing in this build opens a document that
//    way except `--selftest`, which reads through the residency as it should.
//    *Unblocked by:* the same step 8 record, at which point each consumer
//    takes a `LayerResidency&` instead of a `TileStore&`.
//
//  * **Untiled sources are refused, not adapted.** See
//    `openCachedLayerResidency()`; this is a measurement, not an omission.
//
//  * **No mip levels.** `automip` is off and `miplevel` is always 0 in
//    everything this build writes, because io/NpaintFile writes no pyramid
//    (its own deferral list says so) and the only pyramid in this codebase is
//    `ui/NaturalPaintUI`'s display-side one. `TileSourceRef::miplevel` exists
//    because it is half of the cache's own key and leaving it out would make
//    the identity incomplete, but nothing sets it non-zero.
//    *Unblocked by:* io/NpaintFile writing a pyramid, which its deferral list
//    says becomes worthwhile precisely when this module exists.
//
//  * **One `stat()` per fetch, not one per frame.** Hoisting the staleness
//    check to once per frame (or onto a filesystem watch) needs a frame -- a
//    caller that draws a document -- and there is none. 1.256 us x ~240
//    viewport tiles is 0.30 ms against phase 1's measured 12.1 ms p50, which
//    is affordable enough that paying it per fetch and keeping the check
//    unconditional is the right trade today.
//    *Unblocked by:* a real draw loop over a Document.
namespace np {

// --- The residency strategies ---------------------------------------------

enum class TileResidencyMode {
  // Every tile the document has is in our own `TileStore`, allocated at open.
  // This is exactly what `io/ImageIO`'s `openImageAsDocument()` and
  // io/NpaintFile's `loadNpaint()` already do, and it is the only mode the
  // NP_USE_OIIO=OFF build has.
  Eager,
  // Unmodified tiles live in OpenImageIO's `ImageCache` under a fixed memory
  // budget and are fetched on demand; only modified tiles are ours. Requires
  // NP_USE_OIIO and a genuinely tiled source.
  Cached,
};

inline const char* tileResidencyModeName(TileResidencyMode mode) {
  return mode == TileResidencyMode::Eager ? "Eager" : "Cached";
}

// Where one layer's clean pixels live. These four fields are the cache's own
// key, spelled in this project's types: OpenImageIO addresses a cached tile by
// `(ustring filename, subimage, miplevel, x, y, z)`, and the last three come
// from the `TileCoord` at fetch time via core/Tile.hpp's `tileOrigin()`.
struct TileSourceRef {
  std::string path;
  // A multi-part EXR's parts are OpenImageIO subimages, in file order. For a
  // `.npaint` that is part 0 = the composite and part N = a layer; use
  // `npaintLayerTileSource()` rather than computing it, so the part-to-layer
  // rule stays in io/NpaintFile where it was decided.
  int32_t subimage = 0;
  // Always 0 in everything this build writes -- see the deferral list.
  int32_t miplevel = 0;

  friend bool operator==(const TileSourceRef&, const TileSourceRef&) = default;
};

// The cache's memory budget, process-wide.
//
// **Derived, then measured, then landed above the derivation** -- and the
// measurement moved the answer, so both halves are recorded.
//
// The derivation: ADR-0001's amendment bounds the working set at "view tiles
// ... bounded by viewport rather than image size", giving ~30 MiB at
// 2560x1440, and at most two documents visible at once, so ~60 MiB is the
// most a correct implementation ever needs resident to draw everything on
// screen. A budget below that would evict tiles the very next frame asks for
// again.
//
// The measurement, against this project's OpenImageIO 3.1, sweeping 256
// distinct 128 KiB tiles (32.00 MiB of content) at eight nominal budgets:
//
//   nominal MiB |  1     2     4     8    16    24    32    64
//   used MiB    | 9.88  9.88  9.88  9.88 15.88 23.88 31.88 32.00
//
// So `max_memory_MB` is honoured to within 1 % at 16 MiB and above, and is
// **fiction below about 10 MiB**: this build's cache never dropped below 79
// resident tiles (9.88 MiB) for one open file whatever was asked for. A
// budget chosen for tightness rather than for the working set would therefore
// not have been obeyed, and the number reported at run time would have been
// the only way to find that out.
//
// 64 MiB: above the 60 MiB working set, comfortably inside the honoured
// range, and -- the point of the whole exercise -- **independent of document
// size**, which is what turns an O(document) cost into an O(budget) one.
//
// Relation to the idle-RSS ceiling `--selftest` enforces: none, by
// construction. The cache is created lazily, on the first cached open, and
// costs **0.11 MB** at creation (measured) before any tile is read. Idle RSS
// is sampled in `main.cpp` before any document exists, so a budget of any
// size cannot move it. What the budget does bound is the *loaded* footprint,
// which no assertion covers yet because no document survives past
// `--selftest`.
inline constexpr size_t kTileCacheBudgetBytes = 64ull * 1024 * 1024;

// --- One fetch ------------------------------------------------------------

enum class TileFetchStatus {
  // Served from the owned `TileStore`: this tile has been written and is ours.
  Owned,
  // Served from the residency's clean source. In `Eager` mode this never
  // occurs (every clean tile is already owned storage); in `Cached` mode the
  // pixels are in the staging tile, valid until the next call.
  Clean,
  // No such tile: outside the source's data window, or never allocated. The
  // same answer `TileStore::find()` gives, and it means transparent black by
  // core/TileStore.hpp's own convention -- not an error.
  Absent,
  // The clean source could not be read. `TileFetch::error` says why, naming
  // the file. Never accompanied by pixels.
  Failed,
};

struct TileFetch {
  TileFetchStatus status = TileFetchStatus::Absent;

  // Non-null exactly when `status` is `Owned` or `Clean`.
  //
  // **Lifetime.** For `Owned` it points into the residency's own `TileStore`
  // and is valid until that store is mutated. For `Clean` it points at the
  // residency's single staging tile and is valid **only until the next call
  // to any method on this object** -- the next `readTile()` overwrites it.
  // That is the whole point: a clean tile is not held, so a caller that wants
  // to keep one must copy it, and a caller that only reads it (compositing,
  // uploading a viewport tile, sampling a pixel) pays 128 KiB of transient
  // staging for the entire document rather than 128 KiB per tile.
  const Tile* tile = nullptr;

  std::string error;
};

// --- The residency itself -------------------------------------------------

// One layer's tiles, under one of the two strategies.
//
// Move-only: it owns a `TileStore` and, in cached mode, a staging tile and a
// claim on a file. Copying it would duplicate megabytes silently, and two
// copies disagreeing about which tiles are owned is precisely the
// paint-losing bug this class exists to prevent.
class LayerResidency {
 public:
  // An empty eager residency: no clean source, nothing owned. Every tile is
  // `Absent` until written. This is what a `Document::createBlank()` layer
  // has.
  LayerResidency() = default;

  LayerResidency(LayerResidency&&) noexcept = default;
  LayerResidency& operator=(LayerResidency&&) noexcept = default;
  LayerResidency(const LayerResidency&) = delete;
  LayerResidency& operator=(const LayerResidency&) = delete;

  // Adopts an already-populated `TileStore` as an eager residency -- the
  // shape today's `openImageAsDocument()` / `loadNpaint()` produce. Every
  // tile in it is `Owned` from the start, because in eager mode there is no
  // distinction between "came from the file" and "ours": the file was read in
  // full and nothing can be re-fetched.
  //
  // That is not a wart, it is the honest statement of what eager residency
  // *is*, and it is why `residentBytes()` below tells the two modes apart so
  // sharply.
  static LayerResidency adoptEager(TileStore tiles);

  TileResidencyMode mode() const noexcept { return mode_; }

  // The clean source, or an empty path in eager mode.
  const TileSourceRef& source() const noexcept { return source_; }

  // The clean source's data window, in document pixels. All zeros in eager
  // mode. Tile-aligned for every source this module accepts.
  int32_t dataX() const noexcept { return dataX_; }
  int32_t dataY() const noexcept { return dataY_; }
  int32_t dataWidth() const noexcept { return dataWidth_; }
  int32_t dataHeight() const noexcept { return dataHeight_; }

  // True when this coordinate has been promoted (or was eager from the
  // start). Equivalently: whether the owned store holds it. There is no
  // second bookkeeping structure.
  bool isOwned(TileCoord coord) const noexcept { return owned_.find(coord) != nullptr; }

  // Whether the clean source covers this coordinate at all.
  bool sourceCovers(TileCoord coord) const noexcept;

  // Reads without promoting. See `TileFetch::tile` for the lifetime rule.
  //
  // Non-const because a cached fetch mutates the staging tile -- which is the
  // truthful signature. A `const` read that secretly refills a buffer would
  // be a lie about thread-safety as well as about mutation.
  TileFetch readTile(TileCoord coord);

  // Copy-on-first-write. Returns the owned, writable tile for `coord`,
  // promoting the clean tile into the owned store on the first call.
  //
  // Returns **nullptr** (setting `*errorOut`) only when a promotion needed a
  // clean tile that could not be read -- a missing, truncated or
  // changed-on-disk backing file. It never falls back to zeros; see the
  // header comment on why that would be data loss rather than a graceful
  // degradation. For a coordinate the source does not cover, promotion is
  // from transparent black, which is not a failure: that is what an
  // unallocated tile means everywhere else in this codebase.
  Tile* tileForWrite(TileCoord coord, std::string* errorOut = nullptr);

  // The owned tiles, for the paths that already speak `TileStore` --
  // io/NpaintFile's `saveNpaint()` above all, which persists what we own.
  const TileStore& ownedTiles() const noexcept { return owned_; }
  TileStore& ownedTiles() noexcept { return owned_; }

  // --- Measurement ---------------------------------------------------------

  // Bytes held in *this process's own* memory on this layer's behalf: the
  // owned tiles, plus the single staging tile in cached mode. Excludes
  // whatever OpenImageIO's cache holds, which is process-wide and reported
  // separately by `tileCacheStatistics()`.
  //
  // This is the number PLAN.md step 5 is really about, and the one
  // `--selftest` prints for both modes on the same document.
  size_t residentBytes() const noexcept;

  size_t ownedTileCount() const noexcept { return owned_.occupiedTileCount(); }

  // Counters, so a test can prove the cache was actually consulted rather
  // than inferring it.
  uint64_t cleanFetchCount() const noexcept { return cleanFetches_; }
  uint64_t promotionCount() const noexcept { return promotions_; }
  uint64_t failedFetchCount() const noexcept { return failedFetches_; }

 private:
  bool fetchClean(TileCoord coord, Tile* out, std::string* errorOut);
  bool backingFileUnchanged(std::string* errorOut) const;

  TileResidencyMode mode_ = TileResidencyMode::Eager;
  TileStore owned_;
  TileSourceRef source_;
  int32_t dataX_ = 0, dataY_ = 0, dataWidth_ = 0, dataHeight_ = 0;

  // Allocated only in cached mode, so `residentBytes()` is honest about eager
  // residency costing no staging at all.
  std::unique_ptr<Tile> staging_;

  // The backing file's identity at open, for the staleness check. `size` of
  // -1 means "not stamped" (eager mode).
  int64_t sourceSize_ = -1;
  int64_t sourceMtimeSec_ = 0;
  int64_t sourceMtimeNsec_ = 0;

  uint64_t cleanFetches_ = 0;
  uint64_t promotions_ = 0;
  uint64_t failedFetches_ = 0;

  friend bool openCachedLayerResidency(const TileSourceRef&, size_t, LayerResidency*,
                                       std::string*);
};

// Opens `source` as a cached residency.
//
// `budgetBytes` sets the process-wide cache budget (see
// `kTileCacheBudgetBytes`); it is applied on every call because
// OpenImageIO's budget is a property of the cache, not of one file, and
// letting each caller silently inherit whichever value happened to be set
// first would make the footprint depend on open order.
//
// Refuses, by name and with the measurement behind the refusal, when:
//
//  * the build has no OpenImageIO. The message names `.npaint`-style tiled
//    sources, `NP_USE_OIIO`, the cmake line, and -- unlike io/NpaintFile's
//    refusal, which has no alternative to offer -- the fact that
//    `TileResidencyMode::Eager` is a *complete* alternative here, because
//    PRD I1/I3 require opening and painting a file to work identically in
//    this build.
//  * the file cannot be opened, or the subimage does not exist.
//  * the subimage is not 4-channel `half`. `core::Tile` is exactly
//    rgba16float and a cached fetch is a `memcpy` of half words with no
//    conversion stage -- that is what makes the bit-identity claim below
//    true, and quietly converting would make it false.
//  * **the subimage is not stored in `kTileSize`-sized tiles.** This is the
//    refusal a reviewer is most likely to argue with, so the measurement is
//    here rather than in a commit message. An untiled (scanline) source gives
//    the cache nothing to be partial about:
//      - with `autotile` off, reading a single 128x128 region of a 2048x2048
//        PNG pulls the **whole image** into the cache -- 16.00 MiB for one
//        tile, and 15.9 ms to do it. There is no residency win at all.
//      - with `autotile=128` the memory is bounded (1.00 MiB for the first
//        tile), but the underlying decoder must restart to reach a scanline
//        it has passed, and 16 scattered cold tiles measured **1549 us
//        each** -- 12.8 % of phase 1's measured 12.1 ms p50 pen-to-photon
//        budget, per tile, against ~240 tiles in a 2560x1440 viewport.
//    Both are worse than reading the file once, which is what `Eager` does.
//    So the refusal names the format's storage and points at `Eager`, rather
//    than offering a mode that is measurably a loss. For comparison, the same
//    fetch against a 128x128-tiled EXR measures **49-62 us** cold and
//    **3.1-4.5 us** warm.
//
// On refusal `*out` is left untouched, so a rejected open cannot leave a
// half-built residency.
bool openCachedLayerResidency(const TileSourceRef& source, size_t budgetBytes,
                              LayerResidency* out, std::string* errorOut);

// The subimage a `.npaint` layer's pixels live in, for a document that
// `loadNpaint()` produced.
//
// Derived entirely from `NpaintCarry::partOrder`, which io/NpaintFile filled
// in file order while reading -- so the part-to-layer rule (named `L####`,
// `np:kind="RGB"`, channels exactly R/G/B/A in half, data window tile-aligned)
// stays the one io/NpaintFile decided, and is not restated here where it could
// drift. Part 0 is the composite, so a layer part's subimage is its position
// in `partOrder` plus one.
//
// Returns `std::nullopt` when `layerIndex` is not in `partOrder` -- a layer
// created since the load, or a document that was never loaded from a file. A
// layer with no file behind it has no cached residency to open, and saying so
// with an empty optional is better than returning a `TileSourceRef` that
// names a subimage which does not hold it.
std::optional<TileSourceRef> npaintLayerTileSource(const std::string& path,
                                                   const NpaintCarry& carry,
                                                   size_t layerIndex);

// --- Process-wide cache statistics ----------------------------------------

// OpenImageIO's own accounting, not an estimate of ours. Every field is read
// straight out of the `ImageCache`'s `stat:*` attributes.
struct TileCacheStats {
  // `stat:cache_memory_used` -- bytes held by the tile cache right now. This
  // is the number to compare against `LayerResidency::residentBytes()`.
  int64_t memoryUsedBytes = 0;
  // `stat:image_size` -- uncompressed size of every image the cache has been
  // asked about. What an eager load of all of them would have cost.
  int64_t imageSizeBytes = 0;
  // `stat:tiles_created` / `_current` / `_peak`. `created > current` is the
  // only direct evidence that eviction happened; a re-fetch of an already
  // swept tile incrementing `created` proves *that specific tile* was
  // dropped, which is how `--selftest` proves it rather than assuming it.
  int32_t tilesCreated = 0;
  int32_t tilesCurrent = 0;
  int32_t tilesPeak = 0;
  // The budget currently in force, in bytes.
  int64_t budgetBytes = 0;
};

// False when no cache has ever been created (nothing has been opened in
// cached mode), and always false in the NP_USE_OIIO=OFF build -- there is no
// cache to report on, and reporting zeros would be indistinguishable from a
// cache that exists and is empty.
bool tileCacheStatistics(TileCacheStats* out);

// Drops everything the cache holds for `path`, forcing the next fetch to be
// cold. Exists for two real reasons and not as a general-purpose knob: a
// document being closed should not keep its file's tiles resident, and
// `--selftest` needs to make a fetch cold on demand to time one. No-op when
// there is no cache.
void tileCacheInvalidate(const std::string& path);

// Sets the process-wide budget. Returns false in the OFF build, or when no
// cache exists yet. Separate from `openCachedLayerResidency()`'s parameter so
// a test can shrink the budget on an already-open cache and watch eviction
// happen.
bool tileCacheSetBudgetBytes(size_t budgetBytes);

}  // namespace np
