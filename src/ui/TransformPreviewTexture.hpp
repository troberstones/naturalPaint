#pragma once

#include <cstdint>
#include <vector>

#include "core/Layer.hpp"
#include "core/SelectionMask.hpp"
#include "gfx/Wgpu.hpp"
#include "ops/DocumentTransform.hpp"

// ui/TransformPreviewTexture -- docs/testing-issues.md T14: a Free Transform
// drag shows the transformed PIXELS live, not only the wireframe box.
//
// ==========================================================================
// Why this is a new file and not a hijack of app/TransformSession
// ==========================================================================
//
// app/TransformSession.hpp's header is explicit: it has "no ui/ dependency
// of any kind", and its whole point (section 1) is that nothing but
// `commit()` ever reads a source texel or writes to the document -- that is
// what makes `cancel()` free and what its own bit-identical sabotage proof
// (`app/selftest/TransformSession.cpp`, section 5) guards. A live pixel
// preview is a SECOND reader of the source, added at DRAW time, that must
// never become a second writer. Keeping it in its own file, downstream of
// `app/TransformSession`'s public accessors (`pending()`, `sourceBounds()`,
// `layerIndex()`, `selectionSnapshot()`) rather than inside that class, is
// what keeps that boundary a compile-time fact rather than a promise: this
// file cannot reach `pending_` or `sourceBounds_` to mutate them because it
// is not a member of the class that owns them.
//
// ==========================================================================
// The cheap kernel IS the GPU sampler, and that is what makes this cheap
// ==========================================================================
//
// docs/testing-issues.md's T14 entry asks for "a cheap kernel (nearest or
// bilinear)" at draw time, as opposed to `commit()`'s one Catmull-Rom pass.
// This file does not implement a resampler at all: `ui/CanvasQuad` already
// draws an arbitrary quad by handing four corners and a texture to the GPU
// rasteriser, which samples with its own bilinear-minify/nearest-magnify
// filter (ui/CanvasQuad.cpp's own comment on why that split) for free, as
// part of drawing the quad. So "map the source through `pending()` at draw
// time with a cheap kernel" is: upload the UNTRANSFORMED source crop to a
// texture ONCE, and every frame draw it as a quad at the four corners
// `TransformHandlePositions` already computes (`topLeft/topRight/
// bottomRight/bottomLeft` -- the same corners the wireframe box's own line
// segments use, so the preview can never be misaligned with the handles a
// user is dragging). The per-frame cost is one draw call over however many
// SCREEN pixels the quad covers -- "view resolution", not document
// resolution, which is why this does not need the "resample every frame is
// too slow" argument `commit()`'s design exists to avoid. See this file's
// `--selftest` measurement (docs/testing-issues.md T14's own cost ask) for
// the number.
//
// ==========================================================================
// What is uploaded, and when -- exactly once per session, not once per frame
// ==========================================================================
//
// The source pixels cannot change while a session is active (that is
// app/TransformSession's whole invariant), so the crop is captured and
// uploaded exactly once, when a caller's `upload()` runs -- meant to be
// called right after `TransformSession::beginLayer()`/
// `beginSelectionPixels()` succeeds, never once per `updateDrag()` frame.
// `pending()` changing during the drag moves where the ALREADY-UPLOADED
// texture is drawn (the quad's four corners), not what it holds.
//
// **How "exactly once" is actually enforced, since it is not by a test.**
// `upload()` has exactly ONE caller -- ui/MacPaintUI.cpp's
// `beginTransformPreview()` -- and that function in turn has exactly TWO
// call sites, each one immediately after a `TransformSession::begin*()`:
// the `requestFreeTransform` handler in `drawUI()`'s canvas block, and
// main.cpp's drop-a-picture path. The draw block reads `view()` and never
// calls either. That is the whole enforcement, and it is call-site
// placement rather than an assertion: `--selftest` cannot see it, because
// proving it would mean driving `drawUI()` across frames, and this suite is
// headless. A reviewer checking this invariant checks those three greps, not
// a test line. (An earlier draft of this header claimed `--selftest`
// asserted an upload counter stayed at 1 across a drag; no such assertion
// existed, and the counter it named had no reader at all, so both are gone.)
//
// **Named rather than silent: that one upload does NOT itself fit PRD F3 at
// a large, fully-opaque layer.** `app/selftest/TransformPreviewTexture.cpp`'s
// own cost section measures `transformPreviewStraightHalf()` at a realistic
// 2048x2048 fully-covered layer at 49.7 ms -- 248% of F3's 20 ms budget,
// dominated by `imageFromTileStore()`'s float widen and the per-texel
// unpremultiply+`floatToHalf()` pass, both O(document pixels). This is a
// single hitch at the moment a session BEGINS (Cmd+T's mouse-down, or a
// picture landing via drag-and-drop), not a per-DRAG-FRAME cost -- section
// 5's sabotage proof (this same suite's `runTransformPreviewTextureTest()`)
// is what actually matters for T14's "does not blow the frame budget while
// dragging" concern, and it does not touch this cost at all, because
// `upload()` never runs again until the NEXT session begins. But it is a
// real, user-visible stall on a large document today, and this file does not
// claim otherwise. The fix this step's own brief names as "the obvious
// answer" -- packing at VIEW resolution (downsample the crop toward the
// quad's on-screen size before upload, bounding the cost by screen pixels
// rather than document pixels, the same way ui/DocumentTexture.hpp's own
// decision 4 bounds an incremental composite by the dirty set rather than
// the canvas) -- is not built here. It is real follow-on work, not a
// one-line fix: it needs a resample step (an area-average or similar
// prefilter, ops/Resample.hpp already has one) between the crop and the
// pack, sized to the current zoom, and re-triggered on a zoom change mid-
// drag -- which is scope this step did not take on, named rather than
// silently claimed solved.
//
// `transformPreviewStraightHalf()` is the CPU half, GPU-free and headlessly
// testable -- ui/DocumentTexture.hpp's own split, reused here for the same
// reason: a test should be able to check the bytes without a device. It
// reads `layer` through `core::copyThroughSelection()`, the SAME
// non-destructive read `TransformSession::commit()`'s `SelectionPixels`
// path is about to perform destructively via `cutThroughSelection()` -- this
// file uses the read half only, so a preview can never itself mutate the
// layer it is previewing. A null `selection` (the `Layer` target) copies the
// whole layer, which is `copyThroughSelection()`'s own documented behaviour
// and exactly matches `sourceBounds() == layerContentBounds()` for that
// target. A non-null `selection` (the `SelectionPixels` target) copies only
// what it covers, coverage-weighted at soft edges -- identical to what
// `commit()` will actually move, down to the same absent-tile-reads-as-
// transparent rule `imageFromTileStore()` documents, so the preview's edges
// do not lie about where the selection's boundary is.
//
// ==========================================================================
// Scope reduction, named rather than silent: RGB only, not Pigment
// ==========================================================================
//
// `TransformSession::beginLayer()` supports a whole-layer transform of a
// Pigment layer (its header's section 4) -- `ops::transformLayer()` already
// resamples Pigment mass-weighted, and `commit()` inherits that rather than
// re-deciding it. This file does not preview it: showing a Pigment layer's
// pixels means projecting each texel's `Latent` through `latentToRgb()`
// first (`core/Composite.hpp`'s own projection step, "mass is the layer's
// alpha"), and that projection is not built here. `transformPreviewStraightHalf()`
// returns empty for a Pigment layer's `Clipboard`, `upload()` reports that by
// returning `false`, and the caller (ui/MacPaintUI.cpp) falls back to the
// wireframe-only box exactly as it did before this file existed. A Pigment
// Free Transform therefore still shows the box, not the paint -- a real,
// bounded gap, stated here rather than discovered as a blank or wrong quad.
//
// ==========================================================================
// The known, accepted "ghost": this file does not hide the original
// ==========================================================================
//
// The preview quad is drawn OVER the ordinary document composite
// (ui/DocumentTexture), which still shows the layer's UNTRANSFORMED content
// at its original position -- nothing has been written to the document, so
// there is nothing else for it to show. Removing that original content from
// the visible picture for the duration of the drag would mean recompositing
// the document WITHOUT this one layer, every frame, and ui/DocumentTexture.hpp's
// own decision 3 already measured that class of cost for a comparable
// operation: 22 ms at 1024x1024 and 89 ms at 2048x2048 for a full
// recomposite, both well past PRD F3's 20 ms budget on their own, before
// this quad's own draw call. That is a strictly larger and more clearly
// forbidden cost than the resampling-kernel difference at the box's edges
// docs/testing-issues.md's T14 entry already accepts, so it is out of this
// file's scope and named here rather than silently attempted and slow, or
// silently skipped and undocumented. In practice this reads as the
// untransformed layer staying visible in place while the live preview shows
// where it is going -- strictly more information than the wireframe box
// alone gave, which is the bar T14 sets.
namespace np {

struct GpuContext;

// `sourceBounds`' crop of `layer`, through `selection` (or the whole layer
// for `selection == nullptr`), as straight-alpha RGBA16Float half words --
// ui/DocumentTexture's own upload convention (its header's decision 1 and 2),
// reused rather than reinvented so this quad and the document quad it draws
// beside need only one shared mental model. Empty for a Pigment layer (see
// this header's own scope note) or an empty `sourceBounds`.
std::vector<uint16_t> transformPreviewStraightHalf(const Layer& layer, const Selection* selection,
                                                    const DocumentRegion& sourceBounds);

// The GPU half: one small texture, written once per session.
class TransformPreviewTexture {
 public:
  // Captures and uploads `sourceBounds`' crop of `layer` (through
  // `selection`, per `transformPreviewStraightHalf()` above) ONCE. Call this
  // exactly when a transform session begins, not from a per-frame draw path
  // -- see this header's own section on why one upload is correct for the
  // whole drag. Returns false, and leaves `view()` null, for a Pigment layer
  // or an empty region -- the caller's cue to fall back to the wireframe-only
  // box.
  bool upload(GpuContext& gpu, const Layer& layer, const Selection* selection,
             const DocumentRegion& sourceBounds);

  // The view to hand `addCanvasQuad()`, or nullptr if nothing is uploaded.
  WGPUTextureView view() const noexcept { return view_; }

  // Releases the texture. Call when a session ends (`commit()` succeeding or
  // `cancel()`), so a later session's first draw frame cannot show a
  // previous session's pixels, and safe to call at any other time too --
  // see this file's .cpp for why this texture has none of ui/DocumentTexture's
  // retire-don't-release hazard.
  void reset();

 private:
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int32_t width_ = 0;
  int32_t height_ = 0;
};

}  // namespace np
