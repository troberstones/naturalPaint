#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "core/DirtyTiles.hpp"
#include "core/Document.hpp"
#include "gfx/Wgpu.hpp"

// ui/DocumentTexture -- the open document, on screen.
//
// ==========================================================================
// What this closes
// ==========================================================================
//
// Phase 5 built nine steps of document model -- layers, blend modes, Pigment
// layers, masks, adjustment layers, copy-on-write tiles, history, the history
// panel and clipping masks -- and **none of it could be seen**. The canvas
// drew `sim::PaintSim`'s dense texture and nothing else;
// `compositeDocumentPremultiplied()` had exactly two callers, io/Export and
// `--selftest`, so the only way to look at a document was to export it to a
// file and open that file in something else. ui/MacPaintUI.cpp's layers panel
// said so on screen, in its own text.
//
// This module is the missing edge: `core::Document` -> one WGPUTexture ->
// one `ImDrawList::AddImageQuad` on the canvas quad.
//
// **It is not the stroke bridge.** A stroke still writes PaintSim's texture
// and reaches no `Layer::rgbTiles`. What changed is that the document is now
// *visible* over the paper, so a layer, a mask, an opacity, a blend mode or a
// clip is something a person can look at rather than something only a test
// can assert. The two pictures are stacked, not merged, and merging them is a
// later step.
//
// ==========================================================================
// The three decisions
// ==========================================================================
//
// --- 1. RGBA16Float, never 8-bit ------------------------------------------
//
// PRD B6: "16-bit float per channel end to end." Phase 4 spent three steps
// (io/Export's 16-bit PNG path, io/ImageIO's storage policy, io/ImageDecode's
// linearising decode) keeping the file boundary off 8 bits, and routing the
// *screen* boundary through `RGBA8Unorm` would have thrown that away at the
// last possible moment -- for a saving of 4 bytes a texel on one texture.
//
// The cost of the alternative is measured rather than asserted: `--selftest`'s
// `document texture` section runs the 8-bit path beside this one on the same
// document and prints both maximum errors. Half's 11-bit significand also
// carries values 8 bits cannot separate at all -- two linear samples one
// 8-bit code apart round to the *same* byte and stay distinct in f16 -- and
// that is asserted, not argued.
//
// The format is also the one `core::Tile` already stores in
// (DESIGN-imaging.md §2), and the same one ui/NaturalPaintUI's
// `uploadTileMips()` uploads, so nothing here invents a third convention.
//
// --- 2. Straight alpha, not premultiplied ---------------------------------
//
// Storage in this codebase is premultiplied and `compositeDocumentPremultiplied()`
// returns premultiplied floats, so this module *undoes* that before upload
// (via core/Premultiply's shared guard -- this is its fifth caller and the
// reason it finally got promoted out of four copies).
//
// The reason is ImGui, and it is not negotiable from this side. The ImGui
// draw pipeline is created once, for every widget in the application, with
//
//     srcFactor = SrcAlpha,  dstFactor = OneMinusSrcAlpha
//
// which is the *straight*-alpha `over`. Handing it premultiplied texels makes
// it compute `c*a*a + dst*(1-a)`: correct only where alpha is 0 or 1, and
// visibly wrong -- darker -- at every partial coverage, which is exactly the
// soft edge of every brush stroke this application exists to draw. The
// arithmetic of both readings is computed and printed side by side in
// `--selftest`, on a half-alpha texel, so the difference is a number.
//
// The alternative -- switching ImGui's global blend state to
// `(One, OneMinusSrcAlpha)` for this one quad -- would change how **every
// other widget in the window** composites: every anti-aliased glyph, every
// panel background, every border. A one-quad requirement does not get to
// rewrite the application's blend state.
//
// --- 3. Cached on OpenDocument::revision ----------------------------------
//
// A CPU composite of a 2048x2048 document was measured at 0.0458 s (Phase 5
// step 6's own measurement). PRD F3 (P0) asks for pen-to-photon under 20 ms,
// so recompositing every frame overruns the entire end-to-end budget by ~2.3x
// on one document before the solver, the upload or the present have run.
//
// **20 ms is pen-to-photon, not a compute budget** -- input event to displayed
// frame -- so a composite that merely fits inside it has still spent all of it
// and left nothing for anything else. Every "% of budget" figure this codebase
// prints against F3 should be read that way. (An earlier draft of this
// decision, and of `--selftest`'s `document texture` section, attributed a
// 16.67 ms frame budget to PRD A1. A1 is the no-document-open memory
// requirement and the PRD sets no frame budget at all; the citation was
// invented, and both sites now cite F3.)
//
// So the composite is redone only when `OpenDocument::revision` changes --
// which is what `recordEdit()` bumps, and therefore what every layer
// operation, every property setter and every history move already move. A
// frame that changed nothing costs two integer comparisons. What that saves
// is measured and printed by `--selftest`, and the live counters are on
// screen in the layers panel, so the cache is observable in the running app
// rather than believed.
//
// **The key is (id, revision, width, height), not revision alone.** Revisions
// are per-document counters starting at 0, so two open documents both at
// revision 0 would collide and the second would be shown the first's pixels;
// `DocumentId` is process-unique (`allocateDocumentId()`) and separates them.
// Width and height are in the key because they decide the *texture*, not just
// its contents.
//
// **The one way to defeat it**: writing tiles directly, without going through
// `recordEdit()`. Nothing in the UI does that -- every path is a `core/LayerOps`
// call funnelled through `recordLayerEdit()` -- but a test that pokes
// `rgbTiles` by hand and then asks for a view will get the previous upload.
// That is a property of caching on a revision counter and it is stated here
// rather than discovered.
//
// --- 4. Dirty-region incremental, on both halves of the edge --------------
//
// Decision 3's cache has exactly two answers: the frame is free, or the whole
// canvas is recomposited and re-uploaded. `--selftest` measures the second at
// 22 ms for 1024x1024 and 89 ms for 2048x2048. That was tolerable while the
// only way to change a document was a menu item. It stopped being tolerable
// for two reasons:
//
//   1. the layers panel makes toggling visibility, opacity and blend a
//      constant activity, and every toggle costs a visible stall;
//   2. **the stroke bridge would trigger a full recomposite on every dab**,
//      which PRD F3 (P0) forbids in exactly those words: "Pen-to-photon
//      latency under 20 ms; the in-progress stroke does not wait on a full
//      document re-composite." 22 ms exceeds the whole pen-to-photon budget
//      before the solver, the upload or the present have run.

// So the composite is now **dirty-region incremental**: core/DirtyTiles says
// which tiles a change can have moved, core/Composite recomposites exactly
// those, and this module re-uploads exactly those. The three parts each have
// their own home and each is argued where it lives; what belongs here is the
// upload and the policy.
//
// **The upload is one `wgpuQueueWriteTexture` per dirty tile, sub-rectangle,
// with no staging copy.** `WGPUTexelCopyBufferLayout` carries an `offset`, so
// the source can be the tile's first texel *inside the canvas-sized half
// buffer this object already holds*, with `bytesPerRow` the canvas stride --
// no repacking, no scratch. The 256-byte row alignment that
// app/Screenshot.hpp fights is not in play in this direction at all
// (`wgpuQueueWriteTexture` re-stages rows itself and accepts any stride --
// asserted by decision 3's own 61-texel-wide fixture, and re-asserted here at
// a sub-rectangle). Worth recording anyway, because it is what would make the
// packed form free if this ever needed one: a 128-texel tile row at
// RGBA16Float is 128 x 8 = 1024 bytes = exactly 4 x 256.
//
// **The half buffer is kept**, which is the memory this decision costs: one
// canvas of RGBA f16, 32 MiB at 2048x2048, mirroring the texture. It has to
// be, because an incremental upload sends *part* of a picture and the rest of
// that picture has to still exist somewhere. `--selftest` prints the figure.
//
// **The float accumulator is not kept.** core/Composite's region walk writes
// into a caller-owned rectangle, and this module composites **one tile row at
// a time**: the tiles arrive sorted by (y, x), so a band is a contiguous run
// with the same tile y, and the scratch is bounded by
// `canvasWidth * 128 * 4` floats -- 4 MiB at 2048 wide, whatever the shape of
// the dirty set. A bounding box would have been one call instead of a few,
// and would have been the whole canvas for two dirty tiles in opposite
// corners.
//
// **When incremental stops winning.** `core::preferFullRecomposite()` owns the
// threshold and core/DirtyTiles.hpp derives it from a measurement rather than
// a guess: the crossover was expected somewhere around a third of the canvas
// and measures at 107-109% of it, so the full path is taken only when the
// dirty set already covers the canvas. It is always *safe* to take it -- a
// full recomposite is what this module did before this step -- so the constant
// is a performance choice and never a correctness one.
//
// --- 5. Two slots, re-pointed -- never three, and never released ---------
//
// PRD **A6** (P0): "Only *visible* documents hold GPU textures, at most two."
// ADR-0001's amendment is where the number comes from -- documents present as
// tabs with an optional two-tab split, so the predicate is *visible*, not
// *active*.
//
// Until PLAN.md Phase 5 step 14 there was exactly one `DocumentTexture` in the
// process, file-scope in ui/MacPaintUI.cpp, and decision 3's key is
// `(id, revision, width, height)`. **That is why the cap is not a nicety
// bolted onto the split -- it is what makes the split possible at all.** Two
// visible documents driven through one instance miss the key *alternately*:
// every frame, each of them finds the other's id in `key_`, and each pays a
// full recomposite and a full re-upload. `--selftest`'s residency section runs
// exactly that -- the rejected alternative, beside the built one -- and prints
// both upload counts, which is the difference between two uploads per frame
// and zero.
//
// So `DocumentTexturePool` holds **`kVisibleDocumentCap` slots and no way to
// make a third**: a fixed array, not a map that is trimmed afterwards. The cap
// is therefore structural. A hidden document holds zero bytes because no slot
// is keyed to it -- not because something remembered to free it.
//
// **Eviction is a re-point, not a release, and that is the load-bearing
// choice.** A slot handed a document it does not hold takes decision 3's key
// miss: the whole canvas is recomposited into `halves_` and re-uploaded into
// the texture the slot already owns. Nothing is created and nothing is freed
// unless the two documents differ in *size*, which is `freshTexture` and is
// the pre-existing retire path.
//
// The reason it has to be a re-point is ImGui, and it is worth being exact
// about, because `retired_`'s own comment below states the hazard slightly
// wrong and this is the correction:
//
//   `ImGui_ImplWGPU_RenderDrawData()` caches one bind group per image, keyed
//   by `ImHashData(&tex_id, ...)` -- the texture *view pointer* -- and clears
//   the map only in `ImGui_ImplWGPU_InvalidateDeviceObjects()`. The cached
//   entry is a `WGPUBindGroup` **created with that view as an entry**, and a
//   wgpu bind group holds a strong reference to its resources. So the view
//   cannot be freed while the bind group lives, the allocator cannot hand its
//   address back, and the use-after-free `retired_` defends against cannot
//   happen. What *does* happen is the other failure:
//   `wgpuTextureViewRelease()` on a view ImGui has drawn frees **nothing**,
//   because the bind group is still holding it. Releasing a slot would
//   therefore not give the bytes back; it would only make this object stop
//   counting them.
//
// A pool that released on eviction would thus pin one texture per (document,
// size) ever displayed -- twenty tabs cycled through the split would pin
// twenty textures while reporting two. Re-pointing pins two, because two views
// are all that are ever created. That is the whole argument, and it is why
// `release()` stays the thing nothing calls.
//
// **What the cap does not fix**, stated rather than discovered: two documents
// of *different sizes* alternating through one slot retire a texture each
// time, and `retired_` is never freed. The bound is unchanged in kind -- the
// number of distinct document sizes displayed -- but the split makes reaching
// it easy where a single document made it rare. `retiredTextureBytes()` is
// public so the figure is observable rather than argued about, and
// `--selftest` measures it.
namespace np {

struct GpuContext;

// What "the same picture" means for the cache. Compared by value; `id`
// separates two documents that are both at revision 0.
struct DocumentTextureKey {
  DocumentId id = 0;
  uint64_t revision = 0;
  int32_t width = 0;
  int32_t height = 0;

  bool operator==(const DocumentTextureKey& other) const noexcept {
    return id == other.id && revision == other.revision && width == other.width &&
           height == other.height;
  }
  bool operator!=(const DocumentTextureKey& other) const noexcept { return !(*this == other); }
};

DocumentTextureKey documentTextureKey(const OpenDocument& doc) noexcept;

// The CPU half, GPU-free and headlessly testable: composite the document,
// un-premultiply it, and pack it as RGBA f16 in the exact layout
// `wgpuQueueWriteTexture` wants -- row-major, top to bottom, four channels,
// no padding. `warningsOut` is forwarded to `compositeDocumentPremultiplied()`
// unchanged (appended to, never cleared).
//
// Returns an empty vector for a non-positive canvas, matching the compositor.
std::vector<uint16_t> compositeDocumentStraightHalf(
    const Document& doc, std::vector<std::string>* warningsOut = nullptr);

// The per-texel body of that same pack, exposed for a caller that composites
// a REGION (rather than the whole canvas) and needs to pack it identically --
// `DocumentTexture::viewFor()`'s own incremental path, and
// app/ProfileToggle.cpp, which mirrors that path's CPU cost without a GPU.
// `premultiplied` is `texels` RGBA floats; `out` receives `texels` RGBA
// halves, straight-alpha, in the same row-major layout.
void packStraightHalfRow(const float* premultiplied, size_t texels, uint16_t* out);

// The GPU half: one texture, re-uploaded only when the key changes.
class DocumentTexture {
 public:
  // The view to hand `ImDrawList::AddImageQuad`, or nullptr for a document
  // with no canvas. Recomposites and re-uploads only on a key change.
  WGPUTextureView viewFor(GpuContext& gpu, const OpenDocument& doc,
                          std::vector<std::string>* warningsOut = nullptr);

  // Frees every texture this object ever created, including retired ones.
  //
  // **Nothing calls this today**, and that is deliberate rather than an
  // omission: gfx/Wgpu.hpp's convention is that every GPU object in this
  // codebase lives for the process, and this one is a function-local static
  // in ui/MacPaintUI.cpp whose destructor would otherwise run *after* the
  // device is gone. It exists for the caller that eventually needs it (a
  // device loss, or a real teardown order) and so that the retired list has a
  // documented end.
  void release();

  // Live counters, shown in the layers panel and asserted by `--selftest`.
  //
  // `uploads()` counts **key misses** -- frames where the cache could not
  // answer and this object had to bring the texture up to date. It counted
  // that before the incremental path existed and still does, so the split
  // below adds detail rather than moving the meaning:
  // `fullRecomposites() + incrementalUpdates() + emptyUpdates() == uploads()`.
  uint64_t uploads() const noexcept { return uploads_; }
  uint64_t cacheHits() const noexcept { return hits_; }
  double lastUploadMs() const noexcept { return lastUploadMs_; }
  double totalUploadMs() const noexcept { return totalUploadMs_; }

  // Key misses that recomposited the whole canvas, because the change was not
  // tile-local, because there was nothing to compare against, or because so
  // much was dirty that full was the cheaper answer.
  uint64_t fullRecomposites() const noexcept { return fullRecomposites_; }
  // Key misses served by recompositing and re-uploading only the dirty tiles.
  uint64_t incrementalUpdates() const noexcept { return incrementalUpdates_; }
  // Key misses where the revision moved but **nothing the compositor reads**
  // did -- a rename, a lock, a re-parent, or a `recordEdit()` with no
  // mutation behind it. Zero tiles composited and zero texels uploaded.
  uint64_t emptyUpdates() const noexcept { return emptyUpdates_; }

  // The last key miss, in numbers: how many tiles were recomposited, how many
  // texels were written to the texture, and -- when the answer was a full
  // recomposite -- why.
  size_t lastDirtyTiles() const noexcept { return lastDirtyTiles_; }
  uint64_t lastUploadedTexels() const noexcept { return lastUploadedTexels_; }
  uint64_t totalUploadedTexels() const noexcept { return totalUploadedTexels_; }
  FullRecompositeReason lastFullRecompositeReason() const noexcept { return lastFullReason_; }

  // The straight-alpha f16 canvas this object last uploaded from, exactly as
  // the texture holds it: `width * height * 4` halves, row-major, no padding.
  //
  // Public so that `--selftest` can assert the whole point of the step -- that
  // a sequence of incremental updates leaves this **bit-identical** to
  // `compositeDocumentStraightHalf()` of the same document -- without a GPU
  // readback in the loop. The GPU readback is asserted too, once, against this
  // buffer.
  const std::vector<uint16_t>& uploadedHalves() const noexcept { return halves_; }

  // Bytes this object holds that scale with the canvas: the half buffer that
  // mirrors the texture, the float scratch the incremental region walk
  // composites into, and the float scratch a full recomposite composites
  // into. What decision 4 (plus the full-recomposite buffer reuse above it)
  // costs, so a caller can print it rather than guess.
  size_t residentBytes() const noexcept {
    return halves_.capacity() * sizeof(uint16_t) + scratch_.capacity() * sizeof(float) +
           premultScratch_.capacity() * sizeof(float);
  }

  // The texture behind the current view. Created `CopySrc` so that a caller
  // can read it back -- `--selftest` does, to prove the upload landed where it
  // claims. Null before the first upload.
  WGPUTexture texture() const noexcept { return texture_; }

  // How many textures have been parked by a size change (see `retired_`).
  // Public so that `--selftest` can assert the bound this class claims: a
  // re-upload retires nothing, and only a change of document *size* does.
  size_t retiredTextures() const noexcept { return retired_.size(); }

  // --- What this instance costs on the GPU, in bytes ---------------------
  //
  // PRD **A6** (P0) is a statement about bytes ("only *visible* documents hold
  // GPU textures, at most two"), so it needs a number and not an inference
  // from a pointer being non-null. `bytesPerTexel` is 8 and is derived here
  // rather than written as a literal at the call site, because it is a
  // property of the format decision at the top of this file: RGBA16Float is
  // four channels of two bytes, and changing that decision has to change this
  // figure with it.
  static constexpr size_t kBytesPerTexel = 4u * sizeof(uint16_t);

  // The live texture, or 0 before the first upload and after `release()`.
  size_t gpuTextureBytes() const noexcept {
    if (texture_ == nullptr) return 0;
    return static_cast<size_t>(texWidth_) * static_cast<size_t>(texHeight_) * kBytesPerTexel;
  }

  // The parked ones. **Not zero after a size change**, and reported separately
  // rather than folded into the line above, because they are two different
  // claims: the first is what a visible document costs and the second is what
  // this module has not been able to give back. See `retired_`.
  size_t retiredTextureBytes() const noexcept {
    size_t total = 0;
    for (const Retired& r : retired_)
      total += static_cast<size_t>(r.width) * static_cast<size_t>(r.height) * kBytesPerTexel;
    return total;
  }

  // The document whose pixels this instance currently holds, or 0 for one that
  // has never uploaded. What makes a pool slot's occupancy checkable.
  DocumentId documentId() const noexcept { return haveKey_ ? key_.id : 0; }

 private:
  struct Retired {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    // Carried so `retiredTextureBytes()` can report what parking costs. A
    // retired texture is unreachable from this object's view, so its size is
    // otherwise unrecoverable.
    int32_t width = 0;
    int32_t height = 0;
  };

  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int32_t texWidth_ = 0;
  int32_t texHeight_ = 0;

  DocumentTextureKey key_{};
  bool haveKey_ = false;

  // Textures replaced by a size change are parked here rather than released.
  //
  // ImGui's WebGPU backend caches one bind group per image, keyed by
  // `ImHashData(&tex_id, sizeof(tex_id))` -- a hash of the **texture view
  // pointer** -- and never invalidates an entry until
  // `ImGui_ImplWGPU_InvalidateDeviceObjects()`. Releasing a view whose address
  // the allocator then hands back for a new one would make the backend bind a
  // stale bind group holding a freed resource: a use-after-free that would
  // reproduce only when the allocator happened to reuse the address, i.e.
  // rarely, and on someone else's machine.
  //
  // Bounded by the number of distinct document *sizes* a session displays, not
  // by edits or by frames -- a re-upload into an existing texture retires
  // nothing.
  std::vector<Retired> retired_;

  // The straight-alpha f16 canvas the texture holds. Kept because an
  // incremental upload replaces part of a picture and the rest has to still
  // exist -- see decision 4.
  std::vector<uint16_t> halves_;

  // The document as it was when `halves_` was last brought up to date, held
  // as a copy-on-write `Document` copy.
  //
  // **Holding it is what makes the dirty set complete**, and that is a
  // structural property rather than a convention: a snapshot is an owner of
  // every tile, so `TileStoreOf`'s barrier must copy on the next write, so
  // the slot address must move. core/DirtyTiles.hpp §2 is the proof and names
  // the one caller rule that can defeat it -- which is also why the snapshot
  // is taken *here*, by the code that composites, at the moment it
  // composites, and never handed in by a caller who might be holding a
  // write handle across it.
  Document snapshot_;
  bool haveSnapshot_ = false;

  // Reused across calls; bounded by one tile row of the canvas, because the
  // region walk is driven a tile band at a time (decision 4).
  std::vector<float> scratch_;

  // The premultiplied float accumulator a **full** recomposite composites
  // into -- canvas-sized, like `halves_`, rather than one-tile-row-bounded
  // like `scratch_` above, because a full recomposite has no band to bound
  // it by. Held across calls and handed to
  // `compositeDocumentPremultipliedInto()`, which resizes it only when the
  // canvas size no longer matches, so a live document's repeated full
  // recomposites (the common case: this object's whole reason to exist)
  // reuse one allocation instead of paying a fresh multi-hundred-MB
  // allocate-and-zero every time.
  std::vector<float> premultScratch_;

  uint64_t uploads_ = 0;
  uint64_t hits_ = 0;
  uint64_t fullRecomposites_ = 0;
  uint64_t incrementalUpdates_ = 0;
  uint64_t emptyUpdates_ = 0;
  size_t lastDirtyTiles_ = 0;
  uint64_t lastUploadedTexels_ = 0;
  uint64_t totalUploadedTexels_ = 0;
  FullRecompositeReason lastFullReason_ = FullRecompositeReason::None;
  double lastUploadMs_ = 0.0;
  double totalUploadMs_ = 0.0;
};

// PRD A6's "at most two", as a number one place can change it.
inline constexpr size_t kVisibleDocumentCap = 2;

// The visible documents' textures -- at most `kVisibleDocumentCap` of them.
//
// See decision 5 above for why this is a fixed array of slots that are
// re-pointed rather than a cache that is trimmed. Every counter below is a sum
// over the slots and therefore **monotonic across an eviction**: a re-pointed
// slot keeps counting from where it was, so the layers panel's live
// upload/cache numbers never fall when the user changes which documents are on
// screen.
class DocumentTexturePool {
 public:
  // The view for `doc`, bringing its slot up to date. Identical in meaning to
  // `DocumentTexture::viewFor()` -- this only decides *which* instance answers.
  //
  // The slot chosen for a document that has none is the least recently asked
  // for, which is least-recently-visible given that a visible document is
  // asked for every frame and a hidden one never is.
  WGPUTextureView viewFor(GpuContext& gpu, const OpenDocument& doc,
                          std::vector<std::string>* warningsOut = nullptr);

  // Whether `id`'s pixels are resident. False for every hidden document, which
  // is PRD A6 as a predicate.
  bool holds(DocumentId id) const noexcept;

  // Slots currently holding a texture: 0, 1 or `kVisibleDocumentCap`.
  size_t residentDocuments() const noexcept;

  // PRD A6 in bytes: the live textures, summed. Two visible 2048x2048
  // documents are 2 x 32 MiB; a third open tab adds nothing to this figure.
  size_t gpuTextureBytes() const noexcept;

  // The CPU side of the same question -- each slot's f16 mirror and region
  // scratch (decision 4). Also bounded by the cap, and also zero for a hidden
  // document.
  size_t residentBytes() const noexcept;

  // Textures parked by a size change across every slot. Decision 5's stated
  // cost; not given back.
  size_t retiredTextureBytes() const noexcept;
  size_t retiredTextures() const noexcept;

  // Slots re-pointed at a different document. The cap doing its job, counted:
  // a session that never opens the split leaves this at zero.
  uint64_t evictions() const noexcept { return evictions_; }

  // Decision 3's counters, summed over the slots. `lastUploadMs()` is the most
  // recent slot's, not a sum -- it is "what the last composite cost", which is
  // what the layers panel says it is.
  uint64_t uploads() const noexcept;
  uint64_t cacheHits() const noexcept;
  // Texels written to a texture across every slot. What a thrashing pair
  // costs, in the only unit that does not vary with the machine.
  uint64_t totalUploadedTexels() const noexcept;
  double lastUploadMs() const noexcept { return lastUploadMs_; }
  double totalUploadMs() const noexcept;

  // Slot inspection, for `--selftest`: which document a slot holds and what it
  // uploaded. `index` must be under `kVisibleDocumentCap`.
  DocumentId slotDocument(size_t index) const noexcept;
  const DocumentTexture& slot(size_t index) const noexcept { return slots_[index].texture; }

  // Frees every slot. Nothing calls this, for `DocumentTexture::release()`'s
  // reason and for decision 5's: on a view ImGui has drawn it would not give
  // the bytes back anyway.
  void release();

 private:
  struct Slot {
    DocumentTexture texture;
    DocumentId id = 0;
    // Monotonic, so "least recently visible" is a comparison and not a scan of
    // frame numbers this object would otherwise have to be told.
    uint64_t lastUsed = 0;
  };

  std::array<Slot, kVisibleDocumentCap> slots_{};
  uint64_t serial_ = 0;
  uint64_t evictions_ = 0;
  double lastUploadMs_ = 0.0;
};

}  // namespace np
