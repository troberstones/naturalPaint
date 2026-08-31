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

WGPUTextureView DocumentTexture::viewFor(GpuContext& gpu, const OpenDocument& doc,
                                         std::vector<std::string>* warningsOut) {
  const DocumentTextureKey key = documentTextureKey(doc);
  if (key.width <= 0 || key.height <= 0) return nullptr;

  if (view_ != nullptr && haveKey_ && key == key_) {
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

  const size_t canvasTexels =
      static_cast<size_t>(key.width) * static_cast<size_t>(key.height);

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = texture_;
  dst.mipLevel = 0;
  dst.aspect = WGPUTextureAspect_All;

  lastUploadedTexels_ = 0;
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
    compositeDocumentPremultipliedInto(doc.document, premultScratch_, warningsOut);
    if (halves_.size() != canvasTexels * 4) halves_.resize(canvasTexels * 4);
    packStraightHalfRow(premultScratch_.data(), canvasTexels, halves_.data());

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
    // One **tile band** at a time: `documentDirtyTiles()` returns its tiles
    // ascending by (y, x), so a band is a maximal run sharing a tile row. That
    // bounds the float scratch at one canvas row of tiles whatever the shape
    // of the dirty set -- see the header, decision 4, on why a bounding box
    // was rejected.
    size_t i = 0;
    // Warnings come from the first band that actually composites something,
    // and from that one only: every warning is per layer rather than per tile,
    // so letting each band append would repeat every sentence once per band --
    // and hanging them off `i == 0` would drop them entirely on a frame whose
    // first band turned out to be off-canvas.
    bool warned = false;
    while (i < dirty.tiles.size()) {
      size_t j = i;
      while (j < dirty.tiles.size() && dirty.tiles[j].y == dirty.tiles[i].y) ++j;

      // The band's rectangle, clipped to the canvas.
      const PixelCoord first = tileOrigin(dirty.tiles[i]);
      const PixelCoord last = tileOrigin(dirty.tiles[j - 1]);
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
      const std::vector<TileCoord> band(dirty.tiles.begin() + static_cast<ptrdiff_t>(i),
                                        dirty.tiles.begin() + static_cast<ptrdiff_t>(j));
      compositeDocumentTilesPremultiplied(doc.document, band, region,
                                          warned ? nullptr : warningsOut);
      warned = true;

      // Packed and uploaded **one dirty tile at a time, never one band at a
      // time**, and that is a correctness requirement rather than a shape
      // preference: a band is a run of tiles sharing a tile row and they need
      // not be adjacent, so the columns between two dirty tiles were never
      // composited and `scratch_` still holds whatever the previous call left
      // there. Packing a whole band would write that into the CPU mirror --
      // invisible on the GPU, because those columns are not uploaded either,
      // and therefore exactly the kind of divergence that would surface later
      // as a stale rectangle. `--selftest` dirties two tiles with a gap
      // between them for this reason.
      for (size_t k = i; k < j; ++k) {
        const PixelCoord origin = tileOrigin(dirty.tiles[k]);
        const int32_t tx0 = std::max(origin.x, 0);
        const int32_t tx1 = std::min(origin.x + kTileSize, key.width);
        if (tx1 <= tx0) continue;

        for (int32_t y = y0; y < y1; ++y) {
          packStraightHalfRow(scratch_.data() + (static_cast<size_t>(y - y0) *
                                                  static_cast<size_t>(region.width) +
                                              static_cast<size_t>(tx0 - x0)) *
                                                 4u,
                           static_cast<size_t>(tx1 - tx0),
                           halves_.data() + (static_cast<size_t>(y) *
                                                 static_cast<size_t>(key.width) +
                                             static_cast<size_t>(tx0)) *
                                                4u);
        }

        dst.origin = {static_cast<uint32_t>(tx0), static_cast<uint32_t>(y0), 0};

        WGPUTexelCopyBufferLayout tileLayout = {};
        // **No staging copy**: the source is the tile's first texel inside the
        // canvas-sized half buffer, and `bytesPerRow` is the canvas stride, so
        // wgpu walks the rows out of the middle of the picture. `offset` is
        // what makes that possible and is the whole reason this needs no
        // repacking.
        tileLayout.offset = (static_cast<uint64_t>(y0) * static_cast<uint64_t>(key.width) +
                             static_cast<uint64_t>(tx0)) *
                            4u * sizeof(uint16_t);
        tileLayout.bytesPerRow = static_cast<uint32_t>(key.width) * 4u * sizeof(uint16_t);
        tileLayout.rowsPerImage = static_cast<uint32_t>(y1 - y0);

        const WGPUExtent3D tileExtent = {static_cast<uint32_t>(tx1 - tx0),
                                         static_cast<uint32_t>(y1 - y0), 1};
        wgpuQueueWriteTexture(gpu.queue, &dst, halves_.data(),
                              halves_.size() * sizeof(uint16_t), &tileLayout, &tileExtent);
        lastUploadedTexels_ += static_cast<uint64_t>(tx1 - tx0) *
                               static_cast<uint64_t>(y1 - y0);
      }
      i = j;
    }
    ++incrementalUpdates_;
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
  return view_;
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
}

// --------------------------------------------------------------- the pool
//
// Deliberately arithmetic-free: every decision here is "which slot", and the
// slot does the work. See the header's decision 5 for why there is a fixed
// array rather than a map, and why a slot is re-pointed rather than released.
WGPUTextureView DocumentTexturePool::viewFor(GpuContext& gpu, const OpenDocument& doc,
                                             std::vector<std::string>* warningsOut) {
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
  WGPUTextureView view = slots_[chosen].texture.viewFor(gpu, doc, warningsOut);
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
