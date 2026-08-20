#include "io/TileResidency.hpp"

#include <sys/stat.h>

#include <cstring>
#include <utility>

// Same guarded include io/NpaintFile.cpp, io/Export.cpp and io/Capabilities.cpp
// all use: this translation unit is compiled in BOTH configurations, and
// io/OiioBackend.cpp -- the only one allowed an OpenImageIO header -- is added
// to the target only under NP_USE_OIIO.
#if defined(NP_USE_OIIO)
#include "io/OiioBackend.hpp"
#endif

namespace np {
namespace {

#if defined(NP_USE_OIIO)
constexpr bool kOiioBackend = true;
#else
constexpr bool kOiioBackend = false;
#endif

// The refusal the OFF build gives. Unlike io/NpaintFile's, this one has a
// real alternative to name, because eager residency is not a degraded mode:
// it is what every document in this build uses, and PRD I1/I3 require that
// opening and painting a file work identically here.
std::string noBackendRefusal(const std::string& path) {
  return "cached tile residency for '" + path +
         "' is unavailable: this build was compiled with NP_USE_OIIO=OFF, and OpenImageIO's "
         "ImageCache is what serves unmodified tiles on demand. Rebuild with `cmake -S . -B "
         "build -DNP_USE_OIIO=ON -DCMAKE_PREFIX_PATH=\"$HOME/.local/openimageio\"` to enable "
         "it. Nothing is lost by not doing so: TileResidencyMode::Eager reads the whole "
         "source at open and is the residency strategy this build uses for every document, "
         "so opening a file and painting on it behaves identically here (PRD I1, I3).";
}

// Whether `value` is a multiple of `kTileSize` -- the alignment io/NpaintFile
// already requires of any part it turns into a layer, restated here because
// this module accepts sources io/NpaintFile never saw (a tiled TIFF, a plain
// tiled EXR).
bool tileAligned(int32_t value) { return floorMod(value, kTileSize) == 0; }

}  // namespace

// --- LayerResidency -------------------------------------------------------

LayerResidency LayerResidency::adoptEager(TileStore tiles) {
  LayerResidency residency;
  residency.mode_ = TileResidencyMode::Eager;
  residency.owned_ = std::move(tiles);
  return residency;
}

bool LayerResidency::sourceCovers(TileCoord coord) const noexcept {
  if (mode_ != TileResidencyMode::Cached) return false;
  const PixelCoord origin = tileOrigin(coord);
  // The whole tile must lie inside the data window. A partially covered tile
  // is refused rather than served half-filled: OpenImageIO would return
  // success with zeros for the outside part (measured), and a tile that is
  // half file and half invention is the kind of thing that looks right until
  // it is composited against something.
  //
  // Nothing this module accepts can produce a partial tile anyway -- the data
  // window is checked to be tile-aligned at open -- so this is the belt to
  // that braces, and it is what keeps the invariant true if a future source
  // relaxes the alignment rule.
  return origin.x >= dataX_ && origin.y >= dataY_ &&
         origin.x + kTileSize <= dataX_ + dataWidth_ &&
         origin.y + kTileSize <= dataY_ + dataHeight_;
}

bool LayerResidency::backingFileUnchanged(std::string* errorOut) const {
  if (sourceSize_ < 0) return true;  // eager: nothing stamped, nothing to check
  struct stat st {};
  if (::stat(source_.path.c_str(), &st) != 0) {
    if (errorOut) {
      *errorOut = "tile fetch failed: the backing file '" + source_.path +
                  "' can no longer be read (it was removed or renamed since the document was "
                  "opened). Refusing rather than serving the cache's copy, which would be a "
                  "document silently backed by a file that is gone.";
    }
    return false;
  }
  const int64_t size = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
  const int64_t sec = static_cast<int64_t>(st.st_mtimespec.tv_sec);
  const int64_t nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#else
  const int64_t sec = static_cast<int64_t>(st.st_mtim.tv_sec);
  const int64_t nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
  if (size != sourceSize_ || sec != sourceMtimeSec_ || nsec != sourceMtimeNsec_) {
    if (errorOut) {
      *errorOut = "tile fetch failed: the backing file '" + source_.path +
                  "' changed on disk since the document was opened (size " +
                  std::to_string(sourceSize_) + " -> " + std::to_string(size) +
                  "). OpenImageIO's ImageCache does not notice this -- verified: it keeps "
                  "serving the pixels it read before -- so the document would silently mix "
                  "old cached tiles with new ones. Refusing instead.";
    }
    return false;
  }
  return true;
}

bool LayerResidency::fetchClean(TileCoord coord, Tile* out, std::string* errorOut) {
  if (mode_ != TileResidencyMode::Cached || !sourceCovers(coord)) return false;
  if (!backingFileUnchanged(errorOut)) return false;
#if defined(NP_USE_OIIO)
  const PixelCoord origin = tileOrigin(coord);
  return oiioTileCacheFetchHalfRgba(source_.path, source_.subimage, source_.miplevel, origin.x,
                                    origin.y, out->data(), errorOut);
#else
  // Unreachable: mode_ can only be Cached if openCachedLayerResidency()
  // succeeded, and it cannot succeed in this build. Kept as a refusal rather
  // than an assert so the OFF build has no path that returns pixels it did
  // not read.
  (void)out;
  if (errorOut) *errorOut = noBackendRefusal(source_.path);
  return false;
#endif
}

TileFetch LayerResidency::readTile(TileCoord coord) {
  TileFetch result;
  if (const Tile* owned = owned_.find(coord)) {
    result.status = TileFetchStatus::Owned;
    result.tile = owned;
    return result;
  }
  if (mode_ != TileResidencyMode::Cached || !sourceCovers(coord)) {
    result.status = TileFetchStatus::Absent;
    return result;
  }
  if (!fetchClean(coord, staging_.get(), &result.error)) {
    ++failedFetches_;
    result.status = TileFetchStatus::Failed;
    return result;
  }
  ++cleanFetches_;
  result.status = TileFetchStatus::Clean;
  result.tile = staging_.get();
  return result;
}

Tile* LayerResidency::tileForWrite(TileCoord coord, std::string* errorOut) {
  // `findForWrite()` rather than `find()` since PLAN.md Phase 5 step 6 made
  // `find()` const-only: an already-promoted tile whose store has been copied
  // (a document duplicate, a history entry) must be un-shared before this
  // returns a writable pointer to it. That is the copy-on-write barrier
  // core/TileStore.hpp owns; this module's own copy-on-first-write is the
  // *file* half of the same idea and the two compose -- see that header's
  // "How this relates to io/TileResidency".
  if (Tile* owned = owned_.findForWrite(coord)) return owned;

  // Not owned yet. In cached mode with the source covering this coordinate,
  // the promotion must start from the file's pixels -- and must fail rather
  // than start from zeros if it cannot get them, or the first stroke on a
  // document whose file went away erases what was underneath it.
  if (mode_ == TileResidencyMode::Cached && sourceCovers(coord)) {
    // Into the staging tile first, and only then into the owned store.
    // Fetching straight into `owned_.getOrCreate(coord)` would allocate the
    // owned tile before knowing whether the read succeeds, leaving a
    // zero-filled tile owned -- which is the exact data loss this path
    // exists to prevent, arrived at by a different route. (A 128 KiB local
    // would work too and is what this did first; the staging tile is already
    // there for precisely this, and keeps it off the stack.)
    if (!fetchClean(coord, staging_.get(), errorOut)) {
      ++failedFetches_;
      return nullptr;
    }
    ++cleanFetches_;
    ++promotions_;
    Tile& slot = owned_.getOrCreate(coord);
    slot = *staging_;
    return &slot;
  }

  // Either eager (where a tile that is not owned genuinely does not exist
  // yet) or outside the source's data window. Both mean transparent black,
  // which is what a freshly created TileStore tile already is -- so this is a
  // promotion from nothing, not a failure.
  ++promotions_;
  return &owned_.getOrCreate(coord);
}

size_t LayerResidency::residentBytes() const noexcept {
  return owned_.occupiedTileCount() * sizeof(Tile) + (staging_ ? sizeof(Tile) : 0);
}

// --- Opening a cached residency -------------------------------------------

bool openCachedLayerResidency(const TileSourceRef& source, size_t budgetBytes,
                              LayerResidency* out, std::string* errorOut) {
  auto fail = [&](const std::string& message) {
    if (errorOut) *errorOut = message;
    return false;
  };
  if (!out) return fail("cached residency open failed: null destination.");

  if (!kOiioBackend) return fail(noBackendRefusal(source.path));

#if defined(NP_USE_OIIO)
  const OiioTileCacheOpen opened =
      oiioTileCacheOpen(source.path, source.subimage, source.miplevel, budgetBytes);
  if (!opened.ok) {
    return fail("cached residency open failed for '" + source.path + "' subimage " +
                std::to_string(source.subimage) + ": " + opened.error);
  }

  if (opened.tileWidth != kTileSize || opened.tileHeight != kTileSize) {
    const std::string stored =
        (opened.tileWidth <= 0 || opened.tileHeight <= 0)
            ? std::string("scanline-stored (untiled)")
            : (std::to_string(opened.tileWidth) + "x" + std::to_string(opened.tileHeight) +
               " tiles");
    return fail(
        "cached residency refused for '" + source.path + "' subimage " +
        std::to_string(source.subimage) + ": it is " + stored + ", and this residency serves " +
        std::to_string(kTileSize) + "x" + std::to_string(kTileSize) +
        " tiles. Caching it would be measurably worse than reading it once: with an untiled "
        "source OpenImageIO's cache holds the whole image for a single tile request (16.00 "
        "MiB, 15.9 ms, for one 128x128 region of a 2048x2048 PNG), and forcing it to "
        "sub-tile costs ~1549 us per scattered cold tile against ~49 us for a genuinely "
        "tiled source. Use TileResidencyMode::Eager, which reads the source once and is what "
        "every document in this application already uses.");
  }

  if (opened.channels != Tile::kChannels || opened.sampleTypeName != "half") {
    return fail("cached residency refused for '" + source.path + "' subimage " +
                std::to_string(source.subimage) + ": it holds " +
                std::to_string(opened.channels) + " channel(s) of '" + opened.sampleTypeName +
                "', and a tile is exactly " + std::to_string(Tile::kChannels) +
                " channels of half (core/Tile.hpp). A cached fetch is a copy of half words "
                "with no conversion stage, which is what makes it bit-identical to the eager "
                "path; converting silently would make that claim false. Use "
                "TileResidencyMode::Eager for this source.");
  }

  if (!tileAligned(opened.dataX) || !tileAligned(opened.dataY) ||
      !tileAligned(opened.dataWidth) || !tileAligned(opened.dataHeight)) {
    return fail("cached residency refused for '" + source.path + "' subimage " +
                std::to_string(source.subimage) + ": its data window (" +
                std::to_string(opened.dataWidth) + "x" + std::to_string(opened.dataHeight) +
                " at " + std::to_string(opened.dataX) + "," + std::to_string(opened.dataY) +
                ") is not aligned to the " + std::to_string(kTileSize) +
                "-pixel tile grid, so a document tile would straddle the window edge and be "
                "served half from the file and half from OpenImageIO's zero fill. This is "
                "the same alignment io/NpaintFile already requires of a part it accepts as a "
                "layer.");
  }

  struct stat st {};
  if (::stat(source.path.c_str(), &st) != 0) {
    return fail("cached residency open failed: '" + source.path +
                "' could not be stat()ed, so the staleness check that keeps the cache from "
                "serving pixels of a file that has since changed cannot be armed.");
  }

  LayerResidency residency;
  residency.mode_ = TileResidencyMode::Cached;
  residency.source_ = source;
  residency.dataX_ = opened.dataX;
  residency.dataY_ = opened.dataY;
  residency.dataWidth_ = opened.dataWidth;
  residency.dataHeight_ = opened.dataHeight;
  residency.staging_ = std::make_unique<Tile>();
  residency.sourceSize_ = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
  residency.sourceMtimeSec_ = static_cast<int64_t>(st.st_mtimespec.tv_sec);
  residency.sourceMtimeNsec_ = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#else
  residency.sourceMtimeSec_ = static_cast<int64_t>(st.st_mtim.tv_sec);
  residency.sourceMtimeNsec_ = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
  *out = std::move(residency);
  return true;
#else
  (void)budgetBytes;
  return false;  // unreachable: the !kOiioBackend branch above already returned
#endif
}

// --- Document-to-file link ------------------------------------------------

std::optional<TileSourceRef> npaintLayerTileSource(const std::string& path,
                                                   const NpaintCarry& carry,
                                                   size_t layerIndex) {
  for (size_t slot = 0; slot < carry.partOrder.size(); ++slot) {
    const NpaintPartSlot& entry = carry.partOrder[slot];
    if (entry.kind != NpaintPartSlot::Kind::Layer) continue;
    if (entry.index != layerIndex) continue;
    TileSourceRef ref;
    ref.path = path;
    // partOrder describes the parts *after* part 0 (the composite), in file
    // order, and OpenImageIO numbers subimages in that same file order.
    ref.subimage = static_cast<int32_t>(slot) + 1;
    ref.miplevel = 0;
    return ref;
  }
  return std::nullopt;
}

// --- Process-wide cache statistics ----------------------------------------

bool tileCacheStatistics(TileCacheStats* out) {
#if defined(NP_USE_OIIO)
  if (!out) return false;
  OiioTileCacheStats stats;
  if (!oiioTileCacheStatistics(&stats)) return false;
  out->memoryUsedBytes = stats.memoryUsedBytes;
  out->imageSizeBytes = stats.imageSizeBytes;
  out->tilesCreated = stats.tilesCreated;
  out->tilesCurrent = stats.tilesCurrent;
  out->tilesPeak = stats.tilesPeak;
  out->budgetBytes = stats.budgetBytes;
  return true;
#else
  (void)out;
  return false;
#endif
}

void tileCacheInvalidate(const std::string& path) {
#if defined(NP_USE_OIIO)
  oiioTileCacheInvalidate(path);
#else
  (void)path;
#endif
}

bool tileCacheSetBudgetBytes(size_t budgetBytes) {
#if defined(NP_USE_OIIO)
  return oiioTileCacheSetBudget(budgetBytes);
#else
  (void)budgetBytes;
  return false;
#endif
}

}  // namespace np
