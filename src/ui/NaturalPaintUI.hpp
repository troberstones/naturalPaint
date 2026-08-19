#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "app/AppState.hpp"
#include "core/Document.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"

#include "imgui.h"

// ui/NaturalPaintUI (PLAN.md "Phase 2 -- See a file", step 8: "Tiled viewport
// draw"). A read-only proof of the tile pipeline -- core::Document -> one GPU
// texture per occupied core::Tile -> screen -- deliberately NOT wired into
// the interactive painting canvas: src/ui/MacPaintUI.cpp/.hpp keeps drawing
// PaintSim's single dense canvasView() exactly as it does today, completely
// unchanged and running independently of this module. This module doesn't
// know about PaintSim, brushes, tools, or anything else app/AppState.hpp
// carries beyond the one type it deliberately reuses (CanvasView, for pan/
// zoom) -- it only knows how to put a Document's tiles on screen.
//
// Whether/when a live PaintSim's state ever gets "baked to tiles" so this
// module could show *live* painting is a separate, undecided architecture
// question (DESIGN-imaging.md) -- this step leaves that alone entirely; the
// Document handed to setDocument() is treated as immutable.
//
// Naming note (docs/ui.md §6): this module is naturalPaint's own -- not a
// port of the source wireframe's internal "Atelier" codename, which was
// explicitly rejected as this project's name ("Decided: the project keeps
// the name naturalPaint").
namespace np {

// Mip pyramid (PLAN.md "Phase 2 -- See a file", step 9: "Mip pyramid for
// tiles, so a 25% zoom evaluates at a matching level"). Deliberately kept
// out of core/ entirely -- see this file's own header comment on why -- and
// generated fresh, CPU-side, every time setDocument() uploads a tile,
// exactly like the RGBA16Float upload step 8 already does. Nothing here is
// written back into core::Tile/core::TileStore/the Document; it is purely an
// upload-time detail of this read-only viewer.
//
// kTileSize (core/Tile.hpp) is 128 = 2^7, so the natural power-of-two chain
// is 128, 64, 32, 16, 8, 4, 2, 1 -- 8 levels, mip 0 (full res) through mip 7
// (a single texel covering the whole tile).
constexpr int mipLevelCountFor(int32_t tileSize) noexcept {
  int levels = 1;
  while (tileSize > 1) {
    tileSize /= 2;
    ++levels;
  }
  return levels;
}
inline constexpr int kMipLevelCount = mipLevelCountFor(kTileSize);
static_assert(kMipLevelCount == 8,
             "kTileSize=128 (2^7) should yield an 8-level mip chain (128..1)");

// One mip level's decoded pixel data: `size` x `size` texels, RGBA
// interleaved, row-major -- the same layout core::Tile::data() uses, just in
// `float` rather than packed half floats. Kept as plain float throughout
// buildMipChain() (see below) rather than round-tripping through half at
// every level; packing to half happens once, at upload time, in
// TiledDocumentView::setDocument().
struct MipLevel {
  int32_t size = 0;
  std::vector<float> texels;
};

// Builds the full mip chain for one already-populated core::Tile: level 0 is
// that tile's own full-resolution data (decoded via Tile::readPixel(), i.e.
// already unpacked from half to float); each subsequent level is a plain 2x2
// box-filter average of the level above, computed in linear float space (the
// tile's own storage space -- DESIGN-imaging.md's colour pipeline keeps
// tiles in linear light, so no transfer-function work is needed here).
//
// A plain box-filter average is the *correct* downsample here with no
// separate unpremultiply/premultiply step, because core::Tile stores
// premultiplied RGBA (DESIGN-imaging.md §2; io/ImageIO.cpp's `rgb *= a` on
// write): averaging premultiplied texels gives the same result as
// unpremultiplying, averaging, and re-premultiplying, without straight
// alpha's dark-fringing hazard at translucent edges -- exactly the standard
// benefit premultiplied alpha exists for. Pure CPU, no GPU handle involved,
// so this is directly unit-testable from --selftest (see SelfTest.cpp's
// mip-pyramid test).
std::vector<MipLevel> buildMipChain(const Tile& tile);

// "So a 25% zoom evaluates at a matching level" (PLAN.md step 9's own
// wording): at zoom=1.0 (100%) level 0 (128px) is correct; at zoom=0.25
// (25%) the matching resolution is 128*0.25=32px, i.e. mip level 2
// (128 -> 64 -> 32) -- PLAN.md's own literal example. General formula:
// level = clamp(floor(-log2(zoom)), 0, kMipLevelCount-1). Pure math, no GPU
// or ImGui context needed, so directly unit-testable; draw() below is the
// one call site that actually uses it.
int mipLevelForZoom(float zoom) noexcept;

// One uploaded tile's GPU resources. `texture` owns the full mip chain's
// storage (mipLevelCount == kMipLevelCount); `view` spans every level
// (mirrors step 8's original all-levels view, still used where a specific
// level doesn't matter, e.g. the --selftest blit that always reads level 0
// explicitly). `levelViews[i]` is a single-level view scoped to exactly mip
// level i (baseMipLevel=i, mipLevelCount=1) -- draw() below binds one of
// these, picked by mipLevelForZoom(), instead of relying on the GPU/ImGui's
// own automatic LOD selection (not obviously correct for an externally
// supplied WebGPU texture view, and not deterministic/testable the way an
// explicit single-level bind is). Built once in setDocument(), alongside the
// mip upload -- no new GPU objects are created per frame.
struct GpuTile {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  std::vector<WGPUTextureView> levelViews;
};

// A tile's on-screen quad, in the same screen-pixel space MacPaintUI's canvas
// block computes `origin`/`drawSize` in (MacPaintUI.cpp ~line 478-482).
struct TileScreenRect {
  ImVec2 min;
  ImVec2 max;
};

// Where tile `coord` lands on screen given the current pan/zoom `view` and
// `canvasOrigin` -- the screen position document pixel (0, 0) would sit at
// before pan/zoom is applied (MacPaintUI's `canvasPos`: the canvas window's
// top-left, MacPaintUI.cpp ~line 469). Pure geometry -- no GPU call, no live
// ImGui context required, so it's callable unit-test-style from --selftest
// exactly like any other math function:
//
//   screenPos       = canvasOrigin + tileOriginPx * zoom + (panX, panY)
//   tileScreenSize  = kTileSize * zoom
//
// -- the same transform shape MacPaintUI.cpp ~line 478-482 applies to its one
// dense canvas texture, just driven by each tile's own core::tileOrigin()
// instead of a single texture's corner.
TileScreenRect tileScreenRect(TileCoord coord, const CanvasView& view, ImVec2 canvasOrigin);

// Mirrors MacPaintUI.cpp's canvas wheel-zoom block (~line 514-521), ported to
// operate on a caller-supplied CanvasView instead of reaching into AppState.
// This module has no opinion on brushes/tools/hover state, so *whether* to
// call this (e.g. only while the canvas region is hovered) stays the
// caller's decision, same as MacPaintUI's own call site gates it on
// `hovered`. `originOffset` is `(origin - canvasPos)` in MacPaintUI's own
// variable names: the screen-space offset (centring + pan) the drawn content
// currently sits at, which is what the ported formula anchors the zoom to.
// A no-op when `wheelDelta == 0`.
void zoomOnWheel(CanvasView& view, float wheelDelta, ImVec2 originOffset);

// Mirrors MacPaintUI.cpp's canvas drag-to-pan block (~line 523-530), ported
// to operate on a caller-supplied CanvasView. Which gesture counts as "pan"
// (which mouse button, which tool) is policy this module deliberately
// doesn't own -- callers apply this only once they've decided a pan gesture
// is in progress, same as MacPaintUI's own `panning` bool gates its call.
void applyPan(CanvasView& view, ImVec2 dragDelta);

// Uploads and draws one Document's RGB layer as a set of independent
// per-tile GPU textures (PLAN.md step 8), each carrying a full mip chain
// (PLAN.md step 9). Not a live-painting surface: setDocument() uploads once
// and the tiles are then treated as immutable -- there is no per-frame
// re-upload and no write path back into the Document. Read-only on purpose;
// see this header's own top comment.
class TiledDocumentView {
 public:
  // Releases any previously uploaded tiles, then uploads one mip-enabled
  // RGBA16Float GPU texture per occupied tile of `doc`'s first RGB-kind
  // layer with populated `rgbTiles` (core/Layer.hpp: "populated only when
  // kind == RGB"). A Document with no such layer, or whose RGB layer has
  // zero occupied tiles, leaves this holding nothing -- draw() below then
  // emits nothing, not a crash.
  //
  // For each occupied tile, builds the full mip chain via buildMipChain()
  // above and uploads every level (one wgpuQueueWriteTexture call per level,
  // each at its own mipLevel in WGPUTexelCopyTextureInfo) into a single
  // texture whose mipLevelCount spans the whole chain, then creates
  // GpuTile::levelViews -- one single-level WGPUTextureView per level, for
  // draw() to pick from by zoom. All of this happens once, synchronously --
  // not deferred, not re-run or re-generated per frame. Correct because this
  // step's Document is opened once and never mutated (no live painting into
  // it here -- see this header's top comment for why that bridge is out of
  // scope); a future step that does support live edits into a Document would
  // need its own invalidation/re-upload path, not a change to this one.
  void setDocument(GpuContext& gpu, const Document& doc);

  // Releases every uploaded tile's texture + view and forgets them. Safe to
  // call repeatedly (idempotent) and safe on an instance that never held any
  // tiles. setDocument() already calls this itself before uploading a new
  // document's tiles, so callers only need this directly when retiring the
  // viewport for good.
  //
  // No GpuContext parameter -- matching PingPong::release() (sim/
  // PaintSim.hpp) and PaintSim::releaseFields()'s convention over the
  // instructions' example `release(GpuContext&)`: wgpuTextureDestroy/Release
  // and wgpuTextureViewRelease need only the handles being released, not the
  // device that created them, and that's the pattern already established
  // for canvas_/canvasView_ in sim/PaintSim.cpp.
  void release();

  // One ImDrawList::AddImage per uploaded tile, positioned via
  // tileScreenRect() above -- the only piece of this module that actually
  // needs a live ImGui context (an ImDrawList to draw into). `canvasOrigin`
  // plays the same role as MacPaintUI's `canvasPos`. A null `drawList`, or
  // an instance holding no tiles, is a safe no-op.
  //
  // Binds each tile's GpuTile::levelViews[mipLevelForZoom(view.zoom)] --
  // one explicit, single-level view chosen deterministically from the
  // current zoom -- rather than the all-levels `view` and the GPU/ImGui
  // backend's own automatic LOD selection (PLAN.md step 9).
  void draw(ImDrawList* drawList, const CanvasView& view, ImVec2 canvasOrigin) const;

  size_t tileCount() const { return tiles_.size(); }

  // Direct access to the uploaded tiles, keyed by TileCoord -- for
  // --selftest, which drives its own offscreen render/readback straight off
  // each tile's WGPUTextureView (see SelfTest.cpp's runTiledViewportTest)
  // rather than through ImGui's renderer, per this header's own note on
  // draw() being the one piece that needs a live ImGui context.
  const std::unordered_map<TileCoord, GpuTile>& tiles() const { return tiles_; }

 private:
  std::unordered_map<TileCoord, GpuTile> tiles_;
};

}  // namespace np
