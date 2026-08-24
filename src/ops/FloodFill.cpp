#include "ops/FloodFill.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "color/Space.hpp"
#include "core/Premultiply.hpp"

namespace np {
namespace {

// The scratch state of one texel during a contiguous fill. Three states, not a
// visited *bit*, because the third one is what makes the span walk terminate:
// a run may only start at a texel this call has never examined, so a range
// pushed by two different neighbours does not re-expand the same run and
// re-push its neighbours forever. With a plain visited bit the algorithm is
// still finite (each texel is filled once) but re-scans overlapping ranges, and
// the redundancy grows with the region's perimeter.
//
// `kBlocked` also memoises the *negative* answer, which is the more valuable
// half: a texel just outside the tolerance is examined from the row above, the
// row below and its own row, and `floodFillDistance()` costs three `pow()`
// calls. Caching it makes the predicate exactly one evaluation per texel.
constexpr uint8_t kUnexamined = 0;
constexpr uint8_t kBlocked = 1;
constexpr uint8_t kFilled = 2;

constexpr size_t kScratchTexels = static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize);

// A horizontal range of one row, queued for examination. The unit of work is a
// *range*, not a texel -- this is the whole reason the traversal is a scanline
// fill: a solid 4096-wide region queues one of these per row rather than 4096
// per row, and nothing recurses. See the header's §3.
struct ScanRange {
  int32_t y = 0;
  int32_t x0 = 0;
  int32_t x1 = 0;  // inclusive
};

// Everything that has to be looked up per texel, with the lookups hoisted to
// per *tile* instead.
//
// Three maps are keyed by the same `TileCoord` -- the source tiles, the scratch
// states, and the output coverage -- so one cached coordinate serves all three
// and a horizontal run inside one tile pays no hash lookups at all after the
// first. That is what "pages through a TileStore rather than materialising the
// document" means here in practice: the walker never holds more than the tiles
// it is standing on, and a region confined to four tiles never touches a fifth.
class FloodWalk {
 public:
  FloodWalk(const TileStore& source, Selection& out) noexcept : source_(source), out_(out) {}

  // True when this call is the one that filled `doc` -- false both when the
  // texel is outside the tolerance and when some earlier call already claimed
  // it. Callers rely on that distinction; see `kUnexamined` above.
  //
  // Bounds are the caller's business: this is only ever reached for a texel
  // already known to be inside the document.
  bool claim(PixelCoord doc, const FloodFillReference& reference,
             const FloodFillParams& params) {
    seek(tileCoordAt(doc));
    const PixelCoord local = tileLocalOffset(doc);
    const size_t index = static_cast<size_t>(local.y) * static_cast<size_t>(kTileSize) +
                         static_cast<size_t>(local.x);
    uint8_t& state = (*scratch_)[index];
    if (state != kUnexamined) return false;

    const float coverage = floodFillCoverage(floodFillDistance(reference, read(local)), params);
    if (coverage <= 0.0f) {
      state = kBlocked;
      return false;
    }
    state = kFilled;
    // Allocated only now, on the first texel of this tile that is actually
    // selected. core/SelectionMask's constructor invariant is that no stored
    // tile is entirely zero, and creating the tile at `seek()` time would break
    // it for every tile the region merely passes the edge of.
    if (outTile_ == nullptr) outTile_ = &out_.tiles.getOrCreate(cached_);
    outTile_->writeCoverage(local, coverage);
    return true;
  }

 private:
  void seek(TileCoord coord) {
    if (has_ && cached_ == coord) return;
    cached_ = coord;
    has_ = true;
    // Absent source tile is not an error and not an empty region: it reads as
    // {0,0,0,0}, transparent black, which is exactly what an unallocated texel
    // *is* under premultiplied alpha. A fill seeded on blank canvas therefore
    // floods across space no tile exists for, which is the ordinary case for a
    // bucket on an empty layer.
    sourceTile_ = source_.find(coord);
    auto it = scratchTiles_.find(coord);
    if (it == scratchTiles_.end()) {
      it = scratchTiles_.emplace(coord, std::vector<uint8_t>(kScratchTexels, kUnexamined)).first;
    }
    scratch_ = &it->second;
    // Deliberately `find`, not `getOrCreate`: see the invariant note in
    // `claim()`. Null here means "not allocated yet", not "no coverage".
    outTile_ = out_.tiles.findForWrite(coord);
  }

  std::array<float, 4> read(PixelCoord local) const noexcept {
    if (sourceTile_ == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return sourceTile_->readPixel(local);
  }

  const TileStore& source_;
  Selection& out_;
  // Sparse like everything else here: 16 KiB of state per *touched* tile, freed
  // when the walk ends. A dense document-sized visited buffer would be 16 MiB
  // at 4096x4096 and would be allocated even for a fill that covers four texels.
  std::unordered_map<TileCoord, std::vector<uint8_t>> scratchTiles_;

  TileCoord cached_{};
  bool has_ = false;
  const Tile* sourceTile_ = nullptr;
  std::vector<uint8_t>* scratch_ = nullptr;
  SelectionTile* outTile_ = nullptr;
};

// The whole-document predicate pass -- PRD D25's "fill all similar", which is
// not a flood fill and shares nothing with one but the predicate.
//
// The tile set it walks is a real decision, not an optimisation. Normally only
// the source's *occupied* tiles can hold a matching texel, so a fill-all-similar
// on a mostly-empty layer costs what the layer costs. But if the seed matches
// the implicit empty texel -- a transparent seed, which is what clicking blank
// canvas gives -- then every unallocated texel matches too, and the honest
// answer really is every tile in the document. That case is dense by
// definition, exactly as `invertSelection()` is, and it is charged rather than
// silently truncated to "the tiles that happen to exist".
Selection globalSimilar(const TileStore& source, const FloodFillReference& reference,
                        int32_t width, int32_t height, const FloodFillParams& params) {
  Selection out;

  const bool emptyMatches =
      floodFillCoverage(floodFillDistance(reference, {0.0f, 0.0f, 0.0f, 0.0f}), params) > 0.0f;

  std::vector<TileCoord> coords;
  if (emptyMatches) {
    const TileCoord last = tileCoordAt(PixelCoord{width - 1, height - 1});
    for (int32_t ty = 0; ty <= last.y; ++ty) {
      for (int32_t tx = 0; tx <= last.x; ++tx) coords.push_back(TileCoord{tx, ty});
    }
  } else {
    for (const auto& [coord, tile] : source) {
      (void)tile;
      coords.push_back(coord);
    }
  }

  for (const TileCoord coord : coords) {
    const Tile* src = source.find(coord);
    const PixelCoord origin = tileOrigin(coord);
    SelectionTile built;
    bool any = false;
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const int32_t docY = origin.y + ly;
      // The document, not the tile. An edge tile of a 100-tall document holds
      // 128 rows and 28 of them are not part of the picture; selecting them
      // would hand every downstream consumer coverage over texels that do not
      // exist. Same clip `invertSelection()` applies, for the same reason.
      if (docY < 0 || docY >= height) continue;
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const int32_t docX = origin.x + lx;
        if (docX < 0 || docX >= width) continue;
        const PixelCoord local{lx, ly};
        const std::array<float, 4> texel =
            src != nullptr ? src->readPixel(local) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        const float coverage = floodFillCoverage(floodFillDistance(reference, texel), params);
        if (coverage <= 0.0f) continue;
        built.writeCoverage(local, coverage);
        any = true;
      }
    }
    if (any) out.tiles.getOrCreate(coord) = built;
  }
  return out;
}

}  // namespace

FloodFillReference floodFillReferenceFrom(const std::array<float, 4>& premultiplied) noexcept {
  // core/Premultiply's guard, not a hand-rolled divide: `a <= 0` gives
  // {0,0,0,0}, so a never-written texel and a written-transparent one are the
  // same colour at this boundary exactly as they are at the eyedropper's and
  // the exporter's. Without it a transparent texel's arbitrary RGB -- which
  // nothing stored and nothing can recover -- would steer the wand.
  const std::array<float, 4> straight = unpremultiply(premultiplied);
  return FloodFillReference{{srgbEncode(straight[0]), srgbEncode(straight[1]),
                             srgbEncode(straight[2])},
                            straight[3]};
}

float floodFillDistance(const FloodFillReference& reference,
                        const std::array<float, 4>& candidatePremultiplied) noexcept {
  const std::array<float, 4> straight = unpremultiply(candidatePremultiplied);
  float worst = 0.0f;
  for (int c = 0; c < 3; ++c) {
    worst = std::max(worst, std::fabs(srgbEncode(straight[c]) - reference.encodedRgb[c]));
  }
  // Alpha, linear and unencoded -- opacity is not light (see the header). It
  // enters the same `max`, so the tolerance keeps one meaning: every term is
  // within it.
  return std::max(worst, std::fabs(straight[3] - reference.alpha));
}

float floodFillDistanceBetween(const std::array<float, 4>& aPremultiplied,
                               const std::array<float, 4>& bPremultiplied) noexcept {
  return floodFillDistance(floodFillReferenceFrom(aPremultiplied), bPremultiplied);
}

float floodFillCoverage(float distance, const FloodFillParams& params) noexcept {
  // The seed is always fully selected, whatever the parameters say. A tolerance
  // of 0 means "exactly this colour" and must still select the texel that was
  // clicked; an `edgeBand` wider than the tolerance must not partially select
  // it either. Both fall out of doing this first.
  if (distance <= 0.0f) return 1.0f;

  const float tolerance = params.tolerance > 0.0f ? params.tolerance : 0.0f;
  // Written as `!(d < t)` rather than `d >= t` so a NaN distance -- reachable
  // from a NaN in a decoded file -- is refused rather than admitted.
  if (!(distance < tolerance)) return 0.0f;

  // Clamped to the tolerance: a band wider than the whole accepted range would
  // otherwise make even a near-exact match partially covered.
  const float band = std::min(std::max(params.edgeBand, 0.0f), tolerance);
  // A zero band is the hard edge -- Photoshop's Anti-alias unticked. Note that
  // the *reachable set* is `distance < tolerance` in both branches, so turning
  // antialiasing off changes the weights and never the region.
  if (band <= 0.0f) return 1.0f;

  const float ramp = (tolerance - distance) / band;
  return ramp < 1.0f ? ramp : 1.0f;
}

Selection floodFillSelection(const TileStore& source, PixelCoord seed, int32_t width,
                             int32_t height, const FloodFillParams& params) {
  Selection out;
  if (width <= 0 || height <= 0) return out;
  if (seed.x < 0 || seed.y < 0 || seed.x >= width || seed.y >= height) return out;

  // One lookup and three `pow()` calls, paid once for the whole operation.
  const Tile* seedTile = source.find(tileCoordAt(seed));
  const std::array<float, 4> seedTexel =
      seedTile != nullptr ? seedTile->readPixel(tileLocalOffset(seed))
                          : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  const FloodFillReference reference = floodFillReferenceFrom(seedTexel);

  if (params.reach == FloodFillReach::Global) {
    return globalSimilar(source, reference, width, height, params);
  }

  FloodWalk walk(source, out);
  std::vector<ScanRange> pending;
  pending.push_back(ScanRange{seed.y, seed.x, seed.x});

  // The span walk. Each iteration takes one queued range, finds the runs of
  // newly-claimable texels inside it, expands each run to its true extent along
  // the row, and queues the rows above and below over that extent. Nothing
  // recurses and nothing is queued per texel; the queue holds ranges, and the
  // total number ever pushed is bounded by twice the number of runs the region
  // decomposes into.
  while (!pending.empty()) {
    const ScanRange range = pending.back();
    pending.pop_back();

    // Both push sites below produce columns that are already inside the
    // document, so this clamp is dead code today -- and it is here anyway
    // because `claim()` has no bounds check of its own and `tileCoordAt()`
    // happily maps a negative column onto a real tile. A range that reached the
    // walker out of bounds would therefore not crash; it would quietly allocate
    // tiles outside the picture and fill them, which is the failure that gets
    // found months later. Enforcing the invariant where it is *relied on*
    // rather than where it happens to be established costs two comparisons per
    // dequeued range. (Discovered by perturbing the push sites to 8-connected
    // and watching the fill escape the document, not by inspection.)
    int32_t x = std::max(range.x0, 0);
    const int32_t stop = std::min(range.x1, width - 1);
    while (x <= stop) {
      if (!walk.claim(PixelCoord{x, range.y}, reference, params)) {
        ++x;
        continue;
      }
      // Expansion runs to the row's own limits, not to the parent range's --
      // that is what lets the fill reach around a U-shaped region rather than
      // stopping under the span that spawned it.
      int32_t left = x;
      while (left - 1 >= 0 && walk.claim(PixelCoord{left - 1, range.y}, reference, params)) {
        --left;
      }
      int32_t right = x;
      while (right + 1 < width && walk.claim(PixelCoord{right + 1, range.y}, reference, params)) {
        ++right;
      }

      // 4-connected: the rows above and below are queued over exactly this
      // run's columns. Widening them by one on each side is how an 8-connected
      // fill is spelled, and the header says why that is refused.
      if (range.y - 1 >= 0) pending.push_back(ScanRange{range.y - 1, left, right});
      if (range.y + 1 < height) pending.push_back(ScanRange{range.y + 1, left, right});

      x = right + 1;
    }
  }
  return out;
}

size_t fillThroughSelection(TileStore& tiles, const Selection& selection,
                            const std::array<float, 4>& straightLinearRgba, float opacity) {
  if (!(opacity > 0.0f)) return 0;

  const float srcAlpha = straightLinearRgba[3];
  // Premultiplied once, outside every loop. The caller holds a colour picker's
  // straight value; storage is associated (DESIGN-imaging.md §2), and the
  // conversion belongs at this boundary rather than at every call site.
  const std::array<float, 3> premultipliedRgb{straightLinearRgba[0] * srcAlpha,
                                              straightLinearRgba[1] * srcAlpha,
                                              straightLinearRgba[2] * srcAlpha};

  size_t changed = 0;

  // **Iterates the SELECTION's tiles, not the store's** -- the opposite of
  // `clearThroughSelection()`, and the asymmetry is the point. A clear can only
  // remove paint, so a tile that does not exist has nothing to lose and is
  // skipped. A fill *adds* paint, so the tiles it must touch are the ones the
  // selection names, most of which may not exist yet -- and a fill that walked
  // the store instead would silently do nothing on a blank layer, which is the
  // most common bucket click there is.
  std::vector<TileCoord> coords;
  coords.reserve(selection.tiles.occupiedTileCount());
  for (const auto& [coord, tile] : selection.tiles) {
    (void)tile;
    coords.push_back(coord);
  }

  for (const TileCoord coord : coords) {
    const SelectionTile* cover = selection.tiles.find(coord);
    if (cover == nullptr) continue;

    // Lazily obtained on the first texel that actually changes, so a selection
    // tile that is all zeros -- or a fill whose colour is already there -- does
    // not allocate 128 KiB, and does not unshare a copy-on-write tile it never
    // writes to.
    Tile* dst = nullptr;

    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const PixelCoord local{lx, ly};
        const float weight = cover->coverageAt(local) * opacity;
        if (weight <= 0.0f) continue;

        if (dst == nullptr) dst = &tiles.getOrCreate(coord);
        const std::array<float, 4> before = dst->readPixel(local);
        // Premultiplied source-over of a source scaled by `weight`:
        //     s' = (rgb*a*w, a*w),  out = s' + dst * (1 - a*w)
        // All four channels take the same `keep` factor, which is what makes a
        // partially-covered texel half *present* rather than half *bright* --
        // the same associated-alpha argument `clearThroughSelection()` makes in
        // the other direction, and the reason a feathered fill has no fringe.
        const float keep = 1.0f - srcAlpha * weight;
        const std::array<float, 4> after{premultipliedRgb[0] * weight + before[0] * keep,
                                         premultipliedRgb[1] * weight + before[1] * keep,
                                         premultipliedRgb[2] * weight + before[2] * keep,
                                         srcAlpha * weight + before[3] * keep};
        // Compared against the stored value, so "filled with the colour that
        // was already there" reports zero rather than counting texels it did
        // not move. Half-float rounding is on both sides of this comparison,
        // which is why it reads back through `readPixel` instead of trusting
        // the float arithmetic.
        dst->writePixel(local, after);
        if (dst->readPixel(local) != before) ++changed;
      }
    }
  }
  return changed;
}

}  // namespace np
