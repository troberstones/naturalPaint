#pragma once
#include <cstdint>
#include <vector>

#include "app/AppState.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"

#include "imgui.h"

// ui/NaturalPaintUI -- the CPU-side mip pyramid for core::Tile (PLAN.md
// "Phase 2 -- See a file", step 9: "Mip pyramid for tiles, so a 25% zoom
// evaluates at a matching level") plus tileScreenRect(), the tile-to-screen
// placement geometry step 8 introduced alongside it.
//
// This header used to also declare TiledDocumentView, step 8's ("Tiled
// viewport draw") read-only proof of the tile pipeline -- core::Document ->
// one GPU texture per occupied core::Tile -> screen -- deliberately never
// wired into the interactive painting canvas (src/ui/MacPaintUI.cpp/.hpp
// kept drawing PaintSim's single dense canvasView() throughout). ui/
// DocumentTexture went on to become the production Document -> GPU-texture
// path; TiledDocumentView was confirmed unreachable from the live
// application and removed. What remains below has no dependency on that
// class and stays live: SelfTest.hpp's runMipPyramidTest() (app/selftest/
// MipPyramid.cpp) exercises it directly, uploading its own GPU tile via
// uploadTileMips() below rather than through a viewer class.
//
// Naming note (docs/ui.md §6): this module is naturalPaint's own -- not a
// port of the source wireframe's internal "Atelier" codename, which was
// explicitly rejected as this project's name ("Decided: the project keeps
// the name naturalPaint").
namespace np {

// Mip pyramid (PLAN.md "Phase 2 -- See a file", step 9: "Mip pyramid for
// tiles, so a 25% zoom evaluates at a matching level"). Deliberately kept
// out of core/ entirely -- see this file's own header comment on why -- and
// generated fresh, CPU-side, every time uploadTileMips() uploads a tile,
// exactly like the RGBA16Float upload step 8 already does. Nothing here is
// written back into core::Tile/core::TileStore/the Document; it is purely an
// upload-time detail.
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
// uploadTileMips().
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
// so this is directly unit-testable from --selftest (see SelfTest.hpp's
// mip-pyramid test).
std::vector<MipLevel> buildMipChain(const Tile& tile);

// "So a 25% zoom evaluates at a matching level" (PLAN.md step 9's own
// wording): at zoom=1.0 (100%) level 0 (128px) is correct; at zoom=0.25
// (25%) the matching resolution is 128*0.25=32px, i.e. mip level 2
// (128 -> 64 -> 32) -- PLAN.md's own literal example. General formula:
// level = clamp(floor(-log2(zoom)), 0, kMipLevelCount-1). Pure math, no GPU
// or ImGui context needed, so directly unit-testable; runMipPyramidTest()
// (app/selftest/MipPyramid.cpp) is the one call site that picks a GPU
// texture view by its result.
int mipLevelForZoom(float zoom) noexcept;

// One uploaded tile's GPU resources, returned by uploadTileMips() below.
// `texture` owns the full mip chain's storage (mipLevelCount ==
// kMipLevelCount). `levelViews[i]` is a single-level view scoped to exactly
// mip level i (baseMipLevel=i, mipLevelCount=1) -- one per level, so a
// caller can bind the level mipLevelForZoom() picks explicitly, instead of
// relying on the GPU's own automatic LOD selection (not obviously correct
// for an externally supplied WebGPU texture view, and not deterministic/
// testable the way an explicit single-level bind is). No all-levels view:
// nothing left in this codebase samples a GpuTile through automatic LOD, so
// there is nothing for one to serve.
struct GpuTile {
  WGPUTexture texture = nullptr;
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

// Builds the full mip chain for `tile` (buildMipChain() above) and uploads
// it to a fresh mipLevelCount == kMipLevelCount RGBA16Float GPU texture (one
// wgpuQueueWriteTexture call per level, each at its own mipLevel in
// WGPUTexelCopyTextureInfo), then builds GpuTile::levelViews -- one
// single-level WGPUTextureView per level, for a caller to pick from by zoom
// via mipLevelForZoom(). All of this happens once, synchronously, when
// called. The caller owns the result and must eventually pass it to
// releaseGpuTile() below.
//
// The one caller today is runMipPyramidTest() (app/selftest/MipPyramid.cpp),
// proving the mip-level pick actually changes which GPU texture data a
// render reads; this used to be TiledDocumentView::setDocument()'s per-tile
// upload step, factored out here once that read-only viewer class was
// confirmed unreachable from the live application and removed -- the upload
// logic itself owed nothing to the class that used to wrap it.
GpuTile uploadTileMips(GpuContext& gpu, const Tile& tile);

// Releases `tile`'s texture and every per-level view, then zeroes the
// struct. No GpuContext parameter -- matching PingPong::release() (sim/
// PaintSim.hpp) and color::releaseLut3D()'s (color/LutBake.hpp) own
// convention: wgpuTextureDestroy/Release and wgpuTextureViewRelease need
// only the handles being released, not the device that created them. Safe
// to call repeatedly (idempotent) and safe on a default-constructed
// GpuTile.
void releaseGpuTile(GpuTile& tile);

}  // namespace np
