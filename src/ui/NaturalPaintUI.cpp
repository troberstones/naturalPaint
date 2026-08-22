#include "ui/NaturalPaintUI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/Half.hpp"

namespace np {

std::vector<MipLevel> buildMipChain(const Tile& tile) {
  std::vector<MipLevel> levels;
  levels.reserve(static_cast<size_t>(kMipLevelCount));

  // Level 0: the tile's own full-resolution data, decoded from half to
  // float via readPixel() -- everything downstream stays in float, only
  // packed back to half at upload time (uploadTileMips()).
  MipLevel level0;
  level0.size = kTileSize;
  level0.texels.resize(static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize) * 4);
  for (int32_t y = 0; y < kTileSize; ++y) {
    for (int32_t x = 0; x < kTileSize; ++x) {
      const std::array<float, 4> px = tile.readPixel(PixelCoord{x, y});
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(kTileSize) +
                        static_cast<size_t>(x)) *
                       4;
      level0.texels[i + 0] = px[0];
      level0.texels[i + 1] = px[1];
      level0.texels[i + 2] = px[2];
      level0.texels[i + 3] = px[3];
    }
  }
  levels.push_back(std::move(level0));

  // Each subsequent level is a plain 2x2 box-filter average of the level
  // directly above it -- not of level 0 -- so the chain is genuinely
  // recursive (128->64->32->...), matching a real GPU mip pyramid rather
  // than repeatedly downsampling the original.
  for (int lvl = 1; lvl < kMipLevelCount; ++lvl) {
    const MipLevel& prev = levels.back();
    MipLevel next;
    next.size = prev.size / 2;
    next.texels.resize(static_cast<size_t>(next.size) * static_cast<size_t>(next.size) * 4);
    for (int32_t y = 0; y < next.size; ++y) {
      for (int32_t x = 0; x < next.size; ++x) {
        const int32_t sx = x * 2, sy = y * 2;
        const size_t i00 = (static_cast<size_t>(sy) * static_cast<size_t>(prev.size) +
                            static_cast<size_t>(sx)) *
                           4;
        const size_t i10 = (static_cast<size_t>(sy) * static_cast<size_t>(prev.size) +
                            static_cast<size_t>(sx + 1)) *
                           4;
        const size_t i01 = (static_cast<size_t>(sy + 1) * static_cast<size_t>(prev.size) +
                            static_cast<size_t>(sx)) *
                           4;
        const size_t i11 = (static_cast<size_t>(sy + 1) * static_cast<size_t>(prev.size) +
                            static_cast<size_t>(sx + 1)) *
                           4;
        const size_t o =
            (static_cast<size_t>(y) * static_cast<size_t>(next.size) + static_cast<size_t>(x)) * 4;
        for (int c = 0; c < 4; ++c) {
          next.texels[o + c] = 0.25f * (prev.texels[i00 + c] + prev.texels[i10 + c] +
                                        prev.texels[i01 + c] + prev.texels[i11 + c]);
        }
      }
    }
    levels.push_back(std::move(next));
  }
  return levels;
}

int mipLevelForZoom(float zoom) noexcept {
  constexpr int kMaxLevel = kMipLevelCount - 1;
  // zoom<=0 is degenerate input (not a real CanvasView state -- MacPaintUI's
  // own wheel-zoom handling clamps zoom to [0.1, 8.0]), and NaN>0.0f is
  // false, so this one condition also catches NaN -- both treated as "as
  // zoomed-out as possible" rather than evaluating log2 of a non-positive
  // number.
  if (!(zoom > 0.0f)) return kMaxLevel;
  const float raw = std::floor(-std::log2(zoom));
  if (!std::isfinite(raw)) return kMaxLevel;
  return std::clamp(static_cast<int>(raw), 0, kMaxLevel);
}

TileScreenRect tileScreenRect(TileCoord coord, const CanvasView& view, ImVec2 canvasOrigin) {
  const PixelCoord originPx = tileOrigin(coord);
  const ImVec2 screenPos(canvasOrigin.x + static_cast<float>(originPx.x) * view.zoom + view.panX,
                          canvasOrigin.y + static_cast<float>(originPx.y) * view.zoom + view.panY);
  const float sizePx = static_cast<float>(kTileSize) * view.zoom;
  return TileScreenRect{screenPos, ImVec2(screenPos.x + sizePx, screenPos.y + sizePx)};
}

GpuTile uploadTileMips(GpuContext& gpu, const Tile& tile) {
  // PLAN.md step 9: build the full mip chain CPU-side (see buildMipChain()
  // above) before touching the GPU at all -- this is the same ephemeral,
  // upload-time-only detail step 8's single-level upload already was, just
  // now with kMipLevelCount levels instead of one.
  const std::vector<MipLevel> mips = buildMipChain(tile);

  WGPUTextureDescriptor td = {};
  td.label = sv("tile mip chain");
  td.dimension = WGPUTextureDimension_2D;
  td.size = {static_cast<uint32_t>(kTileSize), static_cast<uint32_t>(kTileSize), 1};
  // Same format core::Tile already stores in (DESIGN-imaging.md §2), so
  // level 0's upload below is a direct byte-for-byte copy -- no conversion,
  // matching sim/PaintSim.cpp's makeField() pattern for a texture that needs
  // no render-target/storage usage, just an upload target the GPU can
  // sample.
  td.format = WGPUTextureFormat_RGBA16Float;
  td.mipLevelCount = static_cast<uint32_t>(mips.size());
  td.sampleCount = 1;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  WGPUTexture tex = wgpuDeviceCreateTexture(gpu.device, &td);

  // One wgpuQueueWriteTexture call per level, each at its own mipLevel and
  // its own (smaller) byte size -- still uploaded once, synchronously, here,
  // not regenerated or re-uploaded per frame.
  std::vector<uint16_t> packed;  // scratch, reused/resized across levels
  for (size_t level = 0; level < mips.size(); ++level) {
    const MipLevel& ml = mips[level];
    packed.resize(ml.texels.size());
    for (size_t i = 0; i < ml.texels.size(); ++i) packed[i] = floatToHalf(ml.texels[i]);

    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = tex;
    dst.mipLevel = static_cast<uint32_t>(level);
    dst.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = static_cast<uint32_t>(ml.size) * Tile::kChannels * sizeof(uint16_t);
    layout.rowsPerImage = static_cast<uint32_t>(ml.size);

    const WGPUExtent3D extent = {static_cast<uint32_t>(ml.size), static_cast<uint32_t>(ml.size),
                                 1};
    wgpuQueueWriteTexture(gpu.queue, &dst, packed.data(), packed.size() * sizeof(uint16_t),
                          &layout, &extent);
  }

  GpuTile gt;
  gt.texture = tex;
  gt.levelViews.resize(mips.size());
  for (size_t level = 0; level < mips.size(); ++level) {
    WGPUTextureViewDescriptor vd = {};
    vd.label = sv("tile mip level");
    vd.format = WGPUTextureFormat_RGBA16Float;
    vd.dimension = WGPUTextureViewDimension_2D;
    vd.baseMipLevel = static_cast<uint32_t>(level);
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = 1;
    vd.aspect = WGPUTextureAspect_All;
    gt.levelViews[level] = wgpuTextureCreateView(tex, &vd);
  }
  return gt;
}

void releaseGpuTile(GpuTile& tile) {
  for (WGPUTextureView v : tile.levelViews) {
    if (v) wgpuTextureViewRelease(v);
  }
  tile.levelViews.clear();
  if (tile.texture) {
    wgpuTextureDestroy(tile.texture);
    wgpuTextureRelease(tile.texture);
  }
  tile.texture = nullptr;
}

}  // namespace np
