#include "ui/DocumentTexture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

#include "core/Composite.hpp"
#include "core/Half.hpp"
#include "core/Premultiply.hpp"
#include "gfx/Context.hpp"

namespace np {

DocumentTextureKey documentTextureKey(const OpenDocument& doc) noexcept {
  DocumentTextureKey key;
  key.id = doc.id;
  key.revision = doc.revision;
  key.width = doc.document.width;
  key.height = doc.document.height;
  return key;
}

std::vector<uint16_t> compositeDocumentStraightHalf(const Document& doc,
                                                    std::vector<std::string>* warningsOut) {
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc, warningsOut);
  std::vector<uint16_t> out(premultiplied.size());
  for (size_t i = 0; i + 3 < premultiplied.size(); i += 4) {
    const std::array<float, 4> straight = unpremultiply(std::array<float, 4>{
        premultiplied[i + 0], premultiplied[i + 1], premultiplied[i + 2], premultiplied[i + 3]});
    for (size_t c = 0; c < 4; ++c) out[i + c] = floatToHalf(straight[c]);
  }
  return out;
}

// Straight-alpha f16, texel by texel, exactly as `compositeDocumentStraightHalf()`
// does it -- the same `unpremultiply()` and the same `floatToHalf()` in the
// same order, so a texel that went through the incremental path and one that
// went through the full path are the same bits and not merely the same value.
// The full path calls that function; this one is the region form of its body,
// and there is no third spelling. Declared in ui/DocumentTexture.hpp (rather
// than kept file-local) so a second CPU-only caller -- app/ProfileToggle.cpp,
// which mirrors this object's incremental update path without a GPU -- reuses
// it instead of a second copy that could quietly drift from this one.
void packStraightHalfRow(const float* premultiplied, size_t texels, uint16_t* out) {
  for (size_t t = 0; t < texels; ++t) {
    const std::array<float, 4> straight = unpremultiply(
        std::array<float, 4>{premultiplied[t * 4 + 0], premultiplied[t * 4 + 1],
                             premultiplied[t * 4 + 2], premultiplied[t * 4 + 3]});
    for (size_t c = 0; c < 4; ++c) out[t * 4 + c] = floatToHalf(straight[c]);
  }
}

namespace {

// Ascending (y, x) -- the order `documentDirtyTiles()`/`canvasTiles()` already
// promise, and the order decision 6's band/run walk (`compositeAndUploadTileList()`)
// requires of whatever it is handed.
bool tileRasterLess(TileCoord a, TileCoord b) noexcept {
  return a.y != b.y ? a.y < b.y : a.x < b.x;
}

// AABB overlap between a tile and `viewport`, padded by `kViewportMarginPixels`
// on every side -- see the header's decision 6 for why the pad exists (a
// scroll smaller than one tile must never newly reveal a tile that was
// deferred a moment before).
bool tileIntersectsViewport(TileCoord tile, const DocumentTextureViewport& viewport) noexcept {
  const PixelCoord origin = tileOrigin(tile);
  return origin.x < viewport.x1 + kViewportMarginPixels &&
         origin.x + kTileSize > viewport.x0 - kViewportMarginPixels &&
         origin.y < viewport.y1 + kViewportMarginPixels &&
         origin.y + kTileSize > viewport.y0 - kViewportMarginPixels;
}

}  // namespace

WGPUTextureView DocumentTexture::viewFor(GpuContext& gpu, const OpenDocument& doc,
                                         std::vector<std::string>* warningsOut,
                                         const DocumentTextureViewport* viewport,
                                         uint64_t variant) {
  DocumentTextureKey key = documentTextureKey(doc);
  key.variant = variant;
  if (key.width <= 0 || key.height <= 0) return nullptr;

  // Decision 6's `&& pendingTiles_.empty()` is load-bearing, not a hardening
  // afterthought: `key` does not move just because a call deferred some
  // tiles (nothing about the DOCUMENT changed), so a plain `key == key_`
  // hit here would return the cached view forever once a backlog exists --
  // the very frames that are supposed to trickle it down never would, since
  // MacPaintUI.cpp calls this every frame whether or not anything was
  // edited. Falling through instead lands in the `documentDirtyTiles()`
  // branch below with an EMPTY diff (the snapshot already matches, nothing
  // changed) and lets the backlog-fold step reconstruct the outstanding work
  // from `pendingTiles_` alone -- see that step's own comment.
  if (view_ != nullptr && haveKey_ && key == key_ && pendingTiles_.empty()) {
    ++hits_;
    return view_;
  }

  const auto started = std::chrono::steady_clock::now();

  // A new texture -- because there was none, or because the document changed
  // size -- holds nothing, so nothing can be incremental against it. Recorded
  // before the branch below, because `texWidth_`/`texHeight_` are about to be
  // overwritten.
  const bool freshTexture =
      texture_ == nullptr || key.width != texWidth_ || key.height != texHeight_;

  if (freshTexture) {
    // Retire, do not release -- see DocumentTexture::retired_ on ImGui's
    // bind-group cache being keyed by the view pointer's address.
    if (texture_ != nullptr)
      retired_.push_back(Retired{texture_, view_, texWidth_, texHeight_});

    WGPUTextureDescriptor td = {};
    td.label = sv("document composite");
    td.dimension = WGPUTextureDimension_2D;
    td.size = {static_cast<uint32_t>(key.width), static_cast<uint32_t>(key.height), 1};
    td.format = WGPUTextureFormat_RGBA16Float;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    // CopyDst for the upload; TextureBinding so ImGui can sample it; CopySrc
    // for the same "so --selftest can read this back" reason
    // sim/PaintSim.cpp's canvas target carries it. No mip chain: unlike
    // ui/NaturalPaintUI's per-tile mip pyramid (buildMipChain()/
    // uploadTileMips(), used by app/selftest/MipPyramid.cpp), which builds
    // one per 128-texel tile to serve a zoomed-out tiled viewer, this is a
    // single canvas-sized quad drawn at the view's own zoom, and a mip
    // chain would be rebuilt on every edit for a level that is sampled only
    // when the whole document is minified past 1:2.
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
               WGPUTextureUsage_CopySrc;
    texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
    view_ = wgpuTextureCreateView(texture_, nullptr);
    texWidth_ = key.width;
    texHeight_ = key.height;

    // Decision 6: a resize (or the very first texture) invalidates any
    // backlog tracked for the PREVIOUS geometry -- those coordinates may not
    // even name valid tiles of the new canvas, and `dirty.everything` below
    // is about to supersede them regardless.
    pendingTiles_.clear();
  }

  // --- Which answer this frame gets (see the header, decision 4) -----------
  //
  // The dirty set is asked for only when there is something to compare
  // against: the same document, at the same size, whose previous composite
  // this object still holds. Anything else is a full recomposite, and the
  // reason is recorded rather than left as "it just was".
  DocumentDirtyTiles dirty;
  if (!haveSnapshot_ || freshTexture || !haveKey_ || key_.id != key.id) {
    dirty.everything = true;
    dirty.reason = FullRecompositeReason::NoPreviousComposite;
  } else {
    dirty = documentDirtyTiles(snapshot_, doc.document);
    // The change *was* localised, but to so much of the canvas that a full
    // recomposite is the cheaper answer -- and it is always the safe one,
    // because it is what this module did before this step.
    if (!dirty.everything && preferFullRecomposite(dirty.tiles.size(), canvasTileCount(
                                                                          doc.document))) {
      dirty.everything = true;
      dirty.reason = FullRecompositeReason::None;  // nothing was wrong; it was just cheaper
      dirty.tiles.clear();
    }
  }

  // --- Decision 6: fold the standing backlog into THIS call's dirty set ----
  //
  // Before either branch below runs, and unconditionally -- this is the line
  // the header's decision 6 points at. `snapshot_` is about to be overwritten
  // with `doc.document` regardless of what gets processed, so a tile this
  // call does not actually composite must still be dirty on the NEXT call
  // even though the snapshot diff alone could no longer tell. Folding here,
  // rather than trusting the diff, is what makes that true. `dirty.everything`
  // already names every coordinate the backlog could, so there is nothing to
  // fold in that case. Either way the backlog is about to be superseded by
  // whatever this call decides to process, so it is cleared now and rebuilt
  // below only if a viewport split defers something new.
  if (!dirty.everything && !pendingTiles_.empty()) {
    std::vector<TileCoord> merged = dirty.tiles;
    merged.insert(merged.end(), pendingTiles_.begin(), pendingTiles_.end());
    std::sort(merged.begin(), merged.end(), tileRasterLess);
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    dirty.tiles = std::move(merged);
  }
  pendingTiles_.clear();

  const size_t canvasTexels =
      static_cast<size_t>(key.width) * static_cast<size_t>(key.height);
  // Sized here, unconditionally, rather than only inside the full-canvas fast
  // path below: decision 6's split branch can reach `compositeAndUploadTileList()`
  // on the very first call this object ever answers (a viewport that does not
  // cover the whole canvas on a brand-new document), and that function reads
  // and writes `halves_` at canvas-wide offsets. The condition is unchanged
  // from before decision 6 -- a resize only actually happens when the size
  // differs, i.e. only on `freshTexture` -- so a repeat call at the same size
  // still pays nothing here.
  if (halves_.size() != canvasTexels * 4) halves_.resize(canvasTexels * 4);

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = texture_;
  dst.mipLevel = 0;
  dst.aspect = WGPUTextureAspect_All;

  lastUploadedTexels_ = 0;
  lastCompositeMs_ = 0.0;
  lastPackMs_ = 0.0;

  // --- Decision 6: does `viewport` already cover this call's whole backlog?
  //
  // If so there is nothing to defer, and the two branches below run exactly
  // as they did before this decision existed -- including a null `viewport`
  // (every `--selftest` caller and app/ProfileToggle.cpp), which always lands
  // here. This is what keeps the common "document fits on screen" case from
  // paying the band/run machinery's per-run overhead for no reason: it still
  // gets the one-shot full-canvas blit, or the ordinary incremental loop,
  // exactly as before.
  bool needsSplit = false;
  if (viewport != nullptr) {
    if (dirty.everything) {
      needsSplit = !(viewport->x0 - kViewportMarginPixels <= 0 &&
                     viewport->y0 - kViewportMarginPixels <= 0 &&
                     viewport->x1 + kViewportMarginPixels >= key.width &&
                     viewport->y1 + kViewportMarginPixels >= key.height);
    } else {
      needsSplit = std::any_of(dirty.tiles.begin(), dirty.tiles.end(), [&](const TileCoord& t) {
        return !tileIntersectsViewport(t, *viewport);
      });
    }
  }

  if (!needsSplit) {
    lastDirtyTiles_ = dirty.everything ? canvasTileCount(doc.document) : dirty.tiles.size();
    lastFullReason_ = dirty.everything ? dirty.reason : FullRecompositeReason::None;

    if (dirty.everything) {
      // Composited into `premultScratch_`, an accumulator this object owns
      // across calls, instead of through `compositeDocumentStraightHalf()`'s
      // own fresh-vector-per-call path -- profiling a real 5000x2559, 50-layer
      // document found a plain `std::vector<float> out(w*h*4, 0.0f)` allocated
      // and zero-filled from scratch on *every* full recomposite was ~8.5% of
      // composite time on its own, even though this object recomposites the
      // same canvas size call after call. `compositeDocumentPremultipliedInto()`
      // reuses `premultScratch_`'s allocation and only re-zeros it (still a
      // full pass -- an untouched texel must still read transparent black,
      // see that function's own comment) once the size already matches.
      //
      // `compositeDocumentStraightHalf()` itself is untouched and still pays
      // its own one-shot allocation for its many other callers (io/Export,
      // `--selftest`'s oracle calls), none of which repeat on the same
      // canvas -- this is the same `unpremultiply()` + `floatToHalf()` packing
      // that function does, via the identical `packStraightHalfRow()` the
      // incremental band path below already uses, just reading from
      // `premultScratch_` instead of that function's temporary.
      {
        const auto t0 = std::chrono::steady_clock::now();
        compositeDocumentPremultipliedInto(doc.document, premultScratch_, warningsOut);
        lastCompositeMs_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }
      // `halves_` is already sized (see above, ahead of the split decision).
      {
        const auto t0 = std::chrono::steady_clock::now();
        packStraightHalfRow(premultScratch_.data(), canvasTexels, halves_.data());
        lastPackMs_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }

      WGPUTexelCopyBufferLayout layout = {};
      // 4 channels x 2 bytes. **Not padded to 256**, and that asymmetry is the
      // trap worth naming: `wgpuQueueWriteTexture` re-stages rows itself, so any
      // stride is legal here, while `wgpuCommandEncoderCopyTextureToBuffer` -- the
      // readback direction, which app/Screenshot.hpp documents at length -- does
      // require the 256-byte multiple. A document 61 texels wide (976 bytes/row,
      // not a multiple of 256) is uploaded and read back in `--selftest` so this
      // is a tested claim rather than a reading of the spec.
      layout.bytesPerRow = static_cast<uint32_t>(key.width) * 4u * sizeof(uint16_t);
      layout.rowsPerImage = static_cast<uint32_t>(key.height);

      const WGPUExtent3D extent = {static_cast<uint32_t>(key.width),
                                   static_cast<uint32_t>(key.height), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, halves_.data(), halves_.size() * sizeof(uint16_t),
                            &layout, &extent);
      lastUploadedTexels_ = canvasTexels;
      ++fullRecomposites_;
    } else if (dirty.tiles.empty()) {
      // The revision moved and nothing the compositor reads did -- a rename, a
      // lock, an unchanged `recordEdit()`. Nothing is composited and nothing is
      // uploaded, and the texture is already correct. core/DirtyTiles.hpp §3
      // lists the `Layer` members that reach here and why each is safe.
      //
      // The warnings still have to be produced: whether a document composites
      // approximately is a property of the document, not of whether this frame
      // happened to be cheap. Recomputing them costs one pass over the layer
      // list and no tile access at all.
      if (warningsOut != nullptr)
        compositeDocumentTilesPremultiplied(doc.document, {}, CompositeRegion{}, warningsOut);
      ++emptyUpdates_;
    } else {
      // Decision 4's band/run walk, extracted below (decision 6) so a
      // viewport-priority split can reuse it on a chosen subset instead of
      // always the whole dirty set. `dirty.tiles` here is exactly what this
      // branch always processed -- the entire backlog, folded with any pending
      // tiles above -- so this is behaviourally identical to before decision 6.
      compositeAndUploadTileList(gpu, doc, key, dst, dirty.tiles, warningsOut);
      ++incrementalUpdates_;
    }
  } else {
    // --- Decision 6: viewport-priority split --------------------------------
    //
    // Reached only when `viewport != nullptr` and it does NOT already cover
    // this call's whole backlog. `all` is that backlog, materialised: either
    // `dirty.tiles` (already an explicit list) or, when the backlog is
    // "everything", `canvasTiles()` -- the same tile set the one-shot fast
    // path above would otherwise have covered implicitly in one blit.
    const std::vector<TileCoord> all = dirty.everything ? canvasTiles(doc.document) : dirty.tiles;

    std::vector<TileCoord> inViewport;
    std::vector<TileCoord> outViewport;
    inViewport.reserve(all.size());
    outViewport.reserve(all.size());
    for (const TileCoord& t : all)
      (tileIntersectsViewport(t, *viewport) ? inViewport : outViewport).push_back(t);

    // Viewport-intersecting tiles are unconditional -- never subject to the
    // trickle budget -- which is what makes a genuine scroll- or zoom-into-
    // view prompt: the caller recomputes `viewport` from the CURRENT pan/zoom
    // every call, so a tile that newly intersects it is processed THIS call,
    // whether or not it was in `pendingTiles_` a moment ago.
    // Decision 6 (amended): the take comes from the time budget, not a fixed
    // count. `inViewport.size()` is passed because those tiles are
    // unconditional -- the frame pays them whether or not anything is
    // deferred -- so the budget governs only what this call *adds*. See
    // `kViewportTrickleBudgetMs` for why the rate is an estimate rather than
    // a clock checked mid-call.
    lastInViewportTiles_ = inViewport.size();
    lastTrickleTake_ = trickleTake(inViewport.size());
    const size_t take = std::min(outViewport.size(), lastTrickleTake_);
    std::vector<TileCoord> processSet = inViewport;
    processSet.insert(processSet.end(), outViewport.begin(),
                      outViewport.begin() + static_cast<ptrdiff_t>(take));
    // `inViewport` and the taken prefix of `outViewport` are each ascending
    // (y, x) on their own (both are filtered subsequences of `all`, which is
    // ascending), but concatenated they are not -- `compositeAndUploadTileList()`
    // requires ascending input, so this is a real sort, not a formality.
    std::sort(processSet.begin(), processSet.end(), tileRasterLess);

    // What is left over becomes the standing backlog -- folded into the
    // dirty set at the top of whichever future call (with or without a
    // viewport) finally catches it up.
    pendingTiles_.insert(outViewport.begin() + static_cast<ptrdiff_t>(take), outViewport.end());

    lastDirtyTiles_ = processSet.size();
    // A split defers "not yet," never "why" -- `FullRecompositeReason` names
    // reasons a full recomposite happens at all, and this call may not be
    // compositing the whole canvas even when `dirty.everything` was true.
    lastFullReason_ = FullRecompositeReason::None;

    if (processSet.empty()) {
      // Every dirty tile is off-screen and the trickle budget's own `take`
      // came up empty -- unreachable while `kMinViewportTrickleTiles >= 1`,
      // since `take >= 1` whenever `outViewport` is non-empty and
      // `inViewport` is what leaves `processSet` empty. Guarded anyway,
      // because warnings are a property of the document, not of whether a
      // tile got composited, and must never silently stop being produced.
      if (warningsOut != nullptr)
        compositeDocumentTilesPremultiplied(doc.document, {}, CompositeRegion{}, warningsOut);
      ++emptyUpdates_;
    } else {
      compositeAndUploadTileList(gpu, doc, key, dst, processSet, warningsOut);
      ++incrementalUpdates_;
    }
    if (!pendingTiles_.empty()) ++deferredUpdates_;
  }

  totalUploadedTexels_ += lastUploadedTexels_;

  // Taken **after** the composite and by this function, never by a caller:
  // core/DirtyTiles.hpp §2's completeness argument depends on no write handle
  // outliving it, and this is the narrowest possible window.
  snapshot_ = doc.document;
  haveSnapshot_ = true;

  key_ = key;
  haveKey_ = true;
  ++uploads_;
  // The CPU composite plus the pack plus the queue write, which is what a
  // cache hit avoids. It excludes whatever the GPU does with the staged copy
  // later -- that is not on this thread and not what the budget in the header
  // is about.
  lastUploadMs_ = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  totalUploadMs_ += lastUploadMs_;
  // After `lastUploadMs_` and `lastDirtyTiles_` are both final for this call,
  // and after `totalUploadMs_` so an observation can never be counted twice.
  observeTrickleRate();
  return view_;
}

size_t DocumentTexture::trickleTake(size_t inViewportTiles) const noexcept {
  // Nothing observed yet: the floor. A first call has no basis for taking
  // more, and taking the old fixed budget makes the very first deferred call
  // behave exactly as it did before this change.
  if (msPerTileEma_ <= 0.0) return kMinViewportTrickleTiles;

  // The viewport's own tiles are not optional, so they are spent from the
  // budget before anything is bought with it. A viewport that alone exceeds
  // the deadline buys the floor and nothing more -- it cannot buy negative
  // work, and refusing to trickle at all would strand the backlog forever.
  const double required = msPerTileEma_ * static_cast<double>(inViewportTiles);
  const double remaining = trickleBudgetMs_ - required;
  if (remaining <= 0.0) return kMinViewportTrickleTiles;

  const double n = remaining / msPerTileEma_;
  if (n <= static_cast<double>(kMinViewportTrickleTiles)) return kMinViewportTrickleTiles;
  if (n >= static_cast<double>(kMaxViewportTrickleTiles)) return kMaxViewportTrickleTiles;
  return static_cast<size_t>(n);
}

void DocumentTexture::observeTrickleRate() noexcept {
  // A call that composited nothing says nothing about the rate. So does one
  // the clock reported as taking no measurable time -- dividing by it would
  // drive the estimate to zero and the take straight to the cap, which is
  // precisely the stale-optimistic case the cap exists to bound rather than
  // a reading worth learning from.
  if (lastDirtyTiles_ == 0 || lastUploadMs_ <= 0.0) return;

  const double observed = lastUploadMs_ / static_cast<double>(lastDirtyTiles_);
  constexpr double kAlpha = 0.25;
  msPerTileEma_ = msPerTileEma_ <= 0.0 ? observed
                                       : (1.0 - kAlpha) * msPerTileEma_ + kAlpha * observed;
}

// Decision 4's band/run walk, unmodified in shape from when it lived inline
// in `viewFor()`'s incremental branch -- only `dirty.tiles` became `tiles`,
// the parameter. Decision 6 is the reason it moved: a viewport-priority split
// needs this exact machinery on a caller-chosen subset, and duplicating it
// would risk the two copies drifting apart, which this file's own decision 4
// already refused once for the run-batching inside this same loop ("this is
// a strict generalisation of it, not a second path with its own risk of
// drifting from the slow one").
//
// `tiles` must be ascending by (y, x) and unique -- `documentDirtyTiles()`,
// `canvasTiles()` and decision 6's own sort before calling this all promise
// that. `dst.texture`/`mipLevel`/`aspect` must already be set by the caller;
// only `dst.origin` is written here, once per run.
void DocumentTexture::compositeAndUploadTileList(GpuContext& gpu, const OpenDocument& doc,
                                                 const DocumentTextureKey& key,
                                                 WGPUTexelCopyTextureInfo& dst,
                                                 const std::vector<TileCoord>& tiles,
                                                 std::vector<std::string>* warningsOut) {
  // One **tile band** at a time: `tiles` is ascending by (y, x), so a band is
  // a maximal run sharing a tile row. That bounds the float scratch at one
  // canvas row of tiles whatever the shape of the dirty set -- see the
  // header, decision 4, on why a bounding box was rejected.
  size_t i = 0;
  // Warnings come from the first band that actually composites something,
  // and from that one only: every warning is per layer rather than per tile,
  // so letting each band append would repeat every sentence once per band --
  // and hanging them off `i == 0` would drop them entirely on a frame whose
  // first band turned out to be off-canvas.
  bool warned = false;
  while (i < tiles.size()) {
    size_t j = i;
    while (j < tiles.size() && tiles[j].y == tiles[i].y) ++j;

    // The band's rectangle, clipped to the canvas.
    const PixelCoord first = tileOrigin(tiles[i]);
    const PixelCoord last = tileOrigin(tiles[j - 1]);
    const int32_t x0 = std::max(first.x, 0);
    const int32_t x1 = std::min(last.x + kTileSize, key.width);
    const int32_t y0 = std::max(first.y, 0);
    const int32_t y1 = std::min(first.y + kTileSize, key.height);
    if (x1 <= x0 || y1 <= y0) {
      i = j;
      continue;  // a store may hold a tile entirely outside the canvas
    }

    const size_t bandTexels =
        static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
    if (scratch_.size() < bandTexels * 4) scratch_.resize(bandTexels * 4);

    CompositeRegion region;
    region.pixels = scratch_.data();
    region.origin = PixelCoord{x0, y0};
    region.width = x1 - x0;
    region.height = y1 - y0;
    const std::vector<TileCoord> band(tiles.begin() + static_cast<ptrdiff_t>(i),
                                      tiles.begin() + static_cast<ptrdiff_t>(j));
    {
      const auto t0 = std::chrono::steady_clock::now();
      compositeDocumentTilesPremultiplied(doc.document, band, region,
                                          warned ? nullptr : warningsOut);
      lastCompositeMs_ +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    warned = true;

    // Packed and uploaded **one contiguous run of adjacent tiles at a
    // time, never a whole band in one shot and never one tile at a time
    // either**. A band is a run of tiles sharing a tile row and they need
    // not be adjacent, so packing (and, worse, uploading) a whole band in
    // one call would carry the columns between two non-adjacent dirty
    // tiles -- never composited this call, so `scratch_` still holds
    // whatever a previous call left there -- onto the GPU as if they had
    // changed. `--selftest` dirties two tiles with a gap between them for
    // exactly this reason, and a run stops at that gap.
    //
    // Within a run, though, there is no gap: every texel from the first
    // tile's left edge to the last tile's right edge was just composited
    // into `scratch_` above, so packing and uploading the run's whole
    // x-range in one call each is bit-identical to doing it tile by tile --
    // `unpremultiply()`/`floatToHalf()` are pure per-texel functions with
    // no cross-texel state -- and costs one `wgpuQueueWriteTexture` call
    // instead of however many tiles the run spans. A run of one tile (the
    // previous behaviour, and the common case for scattered single-tile
    // edits) costs exactly what the old per-tile loop cost; this is a
    // strict generalisation of it, not a separate fast path with its own
    // risk of drifting from the slow one.
    //
    // **Measured to not be where a real 5000x2559, 50-layer document's
    // ~450ms toggling a layer covering 780 of 800 tiles goes** -- fewer
    // upload calls is worth having regardless, but `lastCompositeMs()`
    // against `lastUploadMs()` (main.cpp's `--frame-trace`) says the
    // composite math dominates, not the call count this collapses. See
    // that split before assuming this paragraph's optimism about upload
    // calls generalises to why any large-footprint toggle is slow.
    size_t k = i;
    while (k < j) {
      size_t runEnd = k + 1;
      while (runEnd < j && tileOrigin(tiles[runEnd]).x ==
                               tileOrigin(tiles[runEnd - 1]).x + kTileSize)
        ++runEnd;

      const PixelCoord runFirst = tileOrigin(tiles[k]);
      const PixelCoord runLast = tileOrigin(tiles[runEnd - 1]);
      const int32_t rtx0 = std::max(runFirst.x, 0);
      const int32_t rtx1 = std::min(runLast.x + kTileSize, key.width);
      if (rtx1 <= rtx0) {
        k = runEnd;
        continue;  // a run entirely outside the canvas
      }

      {
        const auto t0 = std::chrono::steady_clock::now();
        for (int32_t y = y0; y < y1; ++y) {
          packStraightHalfRow(scratch_.data() + (static_cast<size_t>(y - y0) *
                                                  static_cast<size_t>(region.width) +
                                              static_cast<size_t>(rtx0 - x0)) *
                                                 4u,
                           static_cast<size_t>(rtx1 - rtx0),
                           halves_.data() + (static_cast<size_t>(y) *
                                                 static_cast<size_t>(key.width) +
                                             static_cast<size_t>(rtx0)) *
                                                4u);
        }
        lastPackMs_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
      }

      dst.origin = {static_cast<uint32_t>(rtx0), static_cast<uint32_t>(y0), 0};

      WGPUTexelCopyBufferLayout runLayout = {};
      // **No staging copy**: the source is the run's first texel inside the
      // canvas-sized half buffer, and `bytesPerRow` is the canvas stride, so
      // wgpu walks the rows out of the middle of the picture. `offset` is
      // what makes that possible and is the whole reason this needs no
      // repacking.
      runLayout.offset = (static_cast<uint64_t>(y0) * static_cast<uint64_t>(key.width) +
                          static_cast<uint64_t>(rtx0)) *
                         4u * sizeof(uint16_t);
      runLayout.bytesPerRow = static_cast<uint32_t>(key.width) * 4u * sizeof(uint16_t);
      runLayout.rowsPerImage = static_cast<uint32_t>(y1 - y0);

      const WGPUExtent3D runExtent = {static_cast<uint32_t>(rtx1 - rtx0),
                                      static_cast<uint32_t>(y1 - y0), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, halves_.data(),
                            halves_.size() * sizeof(uint16_t), &runLayout, &runExtent);
      lastUploadedTexels_ += static_cast<uint64_t>(rtx1 - rtx0) *
                             static_cast<uint64_t>(y1 - y0);
      k = runEnd;
    }
    i = j;
  }
}

// **`wgpuTextureRelease` only, never `wgpuTextureDestroy`**, and that is not a
// stylistic preference -- it is a crash this module's `--selftest` section
// found on its first run.
//
// `wgpuQueueWriteTexture` does not write anything when it is called: it stages
// the copy on the queue's own pending encoder, which is flushed at the **next**
// `wgpuQueueSubmit`. `wgpuTextureDestroy` invalidates the resource
// *immediately*, so destroying a texture that has an unflushed write staged
// against it turns the next unrelated submit anywhere in the process into
//
//     Validation Error: Texture with 'document composite' label has been
//     destroyed
//
// which aborts, with a message pointing at whatever happened to submit next
// rather than at the release. Release decrements the reference count instead
// and lets wgpu free the memory once the pending work that still refers to it
// has drained -- the same guarantee, without the window.
void DocumentTexture::release() {
  for (const Retired& r : retired_) {
    if (r.view) wgpuTextureViewRelease(r.view);
    if (r.texture) wgpuTextureRelease(r.texture);
  }
  retired_.clear();
  if (view_) wgpuTextureViewRelease(view_);
  if (texture_) wgpuTextureRelease(texture_);
  view_ = nullptr;
  texture_ = nullptr;
  texWidth_ = 0;
  texHeight_ = 0;
  haveKey_ = false;
  // The CPU mirror and the snapshot go with the texture: keeping either would
  // leave this object claiming to know what a texture that no longer exists
  // holds, which is exactly the stale-pixel failure the dirty set exists to
  // prevent. `shrink_to_fit` because these are the canvas-proportional
  // allocations and `release()` is the one call that says the canvas is gone.
  halves_.clear();
  halves_.shrink_to_fit();
  scratch_.clear();
  scratch_.shrink_to_fit();
  premultScratch_.clear();
  premultScratch_.shrink_to_fit();
  snapshot_ = Document{};
  haveSnapshot_ = false;
  // Decision 6: a backlog names coordinates of a texture that no longer
  // exists -- the same reason `halves_`/`snapshot_` do not survive this call.
  pendingTiles_.clear();
}

// --------------------------------------------------------------- the pool
//
// Deliberately arithmetic-free: every decision here is "which slot", and the
// slot does the work. See the header's decision 5 for why there is a fixed
// array rather than a map, and why a slot is re-pointed rather than released.
WGPUTextureView DocumentTexturePool::viewFor(GpuContext& gpu, const OpenDocument& doc,
                                             std::vector<std::string>* warningsOut,
                                             const DocumentTextureViewport* viewport) {
  // A document with no canvas gets no slot at all -- `DocumentTexture::viewFor()`
  // would return null without touching anything, and claiming a slot for it
  // would evict a document that does have pixels in favour of one that never
  // will.
  if (doc.document.width <= 0 || doc.document.height <= 0) return nullptr;

  size_t chosen = 0;
  bool found = false;
  for (size_t i = 0; i < kVisibleDocumentCap; ++i) {
    if (slots_[i].id == doc.id) {
      chosen = i;
      found = true;
      break;
    }
  }

  if (!found) {
    // An empty slot first, then the least recently asked for. `lastUsed == 0`
    // is "never used", which sorts below every real serial, so the two cases
    // are one comparison rather than two passes.
    for (size_t i = 1; i < kVisibleDocumentCap; ++i)
      if (slots_[i].lastUsed < slots_[chosen].lastUsed) chosen = i;
    // Only a slot that was holding *something else* is an eviction. Filling an
    // empty slot is not, and counting it as one would make a session that
    // never opened the split report evictions.
    if (slots_[chosen].id != 0) ++evictions_;
    slots_[chosen].id = doc.id;
  }

  slots_[chosen].lastUsed = ++serial_;
  WGPUTextureView view = slots_[chosen].texture.viewFor(gpu, doc, warningsOut, viewport);
  lastUploadMs_ = slots_[chosen].texture.lastUploadMs();
  return view;
}

bool DocumentTexturePool::holds(DocumentId id) const noexcept {
  if (id == 0) return false;
  for (const Slot& s : slots_)
    if (s.id == id && s.texture.texture() != nullptr) return true;
  return false;
}

size_t DocumentTexturePool::residentDocuments() const noexcept {
  size_t n = 0;
  for (const Slot& s : slots_)
    if (s.texture.texture() != nullptr) ++n;
  return n;
}

size_t DocumentTexturePool::gpuTextureBytes() const noexcept {
  size_t total = 0;
  for (const Slot& s : slots_) total += s.texture.gpuTextureBytes();
  return total;
}

size_t DocumentTexturePool::residentBytes() const noexcept {
  size_t total = 0;
  for (const Slot& s : slots_) total += s.texture.residentBytes();
  return total;
}

size_t DocumentTexturePool::retiredTextureBytes() const noexcept {
  size_t total = 0;
  for (const Slot& s : slots_) total += s.texture.retiredTextureBytes();
  return total;
}

size_t DocumentTexturePool::retiredTextures() const noexcept {
  size_t total = 0;
  for (const Slot& s : slots_) total += s.texture.retiredTextures();
  return total;
}

uint64_t DocumentTexturePool::uploads() const noexcept {
  uint64_t total = 0;
  for (const Slot& s : slots_) total += s.texture.uploads();
  return total;
}

uint64_t DocumentTexturePool::cacheHits() const noexcept {
  uint64_t total = 0;
  for (const Slot& s : slots_) total += s.texture.cacheHits();
  return total;
}

uint64_t DocumentTexturePool::totalUploadedTexels() const noexcept {
  uint64_t total = 0;
  for (const Slot& s : slots_) total += s.texture.totalUploadedTexels();
  return total;
}

double DocumentTexturePool::totalUploadMs() const noexcept {
  double total = 0.0;
  for (const Slot& s : slots_) total += s.texture.totalUploadMs();
  return total;
}

DocumentId DocumentTexturePool::slotDocument(size_t index) const noexcept {
  return index < kVisibleDocumentCap ? slots_[index].id : 0;
}

void DocumentTexturePool::release() {
  for (Slot& s : slots_) {
    s.texture.release();
    s.id = 0;
    s.lastUsed = 0;
  }
  serial_ = 0;
}

}  // namespace np
