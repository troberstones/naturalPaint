#pragma once

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
// `TiledDocumentView` uploads, so nothing here invents a third convention.
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
// step 6's own measurement). PRD A1 asks for 60 fps, a 0.0167 s budget, so
// recompositing every frame misses it by ~2.7x on one document before the
// solver has run at all.
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
//
// (Decision 3 above attributes a 16.67 ms frame budget to PRD A1. A1 is the
// no-document-open memory requirement and the PRD has no frame budget; F3's
// 20 ms pen-to-photon is the real one. That line predates this step and is
// deliberately left alone rather than widening this diff -- see
// core/DirtyTiles.hpp.)
//
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
  // mirrors the texture, plus the float scratch the region walk composites
  // into. What decision 4 costs, so a caller can print it rather than guess.
  size_t residentBytes() const noexcept {
    return halves_.capacity() * sizeof(uint16_t) + scratch_.capacity() * sizeof(float);
  }

  // The texture behind the current view. Created `CopySrc` so that a caller
  // can read it back -- `--selftest` does, to prove the upload landed where it
  // claims. Null before the first upload.
  WGPUTexture texture() const noexcept { return texture_; }

  // How many textures have been parked by a size change (see `retired_`).
  // Public so that `--selftest` can assert the bound this class claims: a
  // re-upload retires nothing, and only a change of document *size* does.
  size_t retiredTextures() const noexcept { return retired_.size(); }

 private:
  struct Retired {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
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

}  // namespace np
