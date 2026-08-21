#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/DocumentLifecycle.hpp"
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
  uint64_t uploads() const noexcept { return uploads_; }
  uint64_t cacheHits() const noexcept { return hits_; }
  double lastUploadMs() const noexcept { return lastUploadMs_; }
  double totalUploadMs() const noexcept { return totalUploadMs_; }

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

  uint64_t uploads_ = 0;
  uint64_t hits_ = 0;
  double lastUploadMs_ = 0.0;
  double totalUploadMs_ = 0.0;
};

}  // namespace np
