#include "ui/TransformPreviewTexture.hpp"

#include <array>
#include <cstring>

#include "core/Clipboard.hpp"
#include "core/Half.hpp"
#include "core/Premultiply.hpp"
#include "gfx/Context.hpp"
#include "ops/Transform.hpp"

namespace np {

std::vector<uint16_t> transformPreviewStraightHalf(const Layer& layer, const Selection* selection,
                                                    const DocumentRegion& sourceBounds) {
  if (sourceBounds.empty()) return {};

  // Non-destructive by construction, not by convention: `copyThroughSelection()`
  // takes `layer` by const&, so this call cannot be the second writer
  // app/TransformSession.hpp's header warns against, whatever this function's
  // body does. It is the identical read `TransformSession::commit()`'s
  // `SelectionPixels` path is about to perform destructively via
  // `cutThroughSelection()` -- same coverage weighting, same "null selection
  // is the whole layer" rule -- minus the erase half.
  const Clipboard clip = copyThroughSelection(layer, selection);

  // Pigment: named scope reduction, this header's own section on it. A
  // Pigment layer's Clipboard carries `pigmentTiles`, not `rgbTiles` --
  // projecting that to a displayable colour needs `latentToRgb()` per texel
  // (core/Composite.hpp), which this file does not add.
  if (clip.kind != LayerKind::RGB || !clip.rgbTiles.has_value()) return {};

  const TransformImage img = imageFromTileStore(*clip.rgbTiles, sourceBounds.x, sourceBounds.y,
                                                sourceBounds.width, sourceBounds.height);
  if (!img.valid()) return {};

  // Straight alpha, matching ui/DocumentTexture's own convention exactly
  // (its header's decision 2): this texture is drawn through the identical
  // ui/CanvasQuad pipeline and Dear ImGui blend state, and a second alpha
  // convention on that one shared pipeline would reproduce the
  // present-transfer defect app/selftest/PresentTransfer.cpp exists to catch
  // -- just on a quad that defect's own coverage never looks at.
  std::vector<uint16_t> out(img.px.size());
  for (size_t i = 0; i + 3 < img.px.size(); i += 4) {
    const std::array<float, 4> straight = unpremultiply(
        std::array<float, 4>{img.px[i + 0], img.px[i + 1], img.px[i + 2], img.px[i + 3]});
    for (size_t c = 0; c < 4; ++c) out[i + c] = floatToHalf(straight[c]);
  }
  return out;
}

bool TransformPreviewTexture::upload(GpuContext& gpu, const Layer& layer,
                                     const Selection* selection,
                                     const DocumentRegion& sourceBounds) {
  reset();
  const std::vector<uint16_t> half = transformPreviewStraightHalf(layer, selection, sourceBounds);
  if (half.empty()) return false;

  WGPUTextureDescriptor td = {};
  td.label = sv("transform preview");
  td.dimension = WGPUTextureDimension_2D;
  td.size = {sourceBounds.width, sourceBounds.height, 1};
  td.format = WGPUTextureFormat_RGBA16Float;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
  view_ = wgpuTextureCreateView(texture_, nullptr);
  width_ = static_cast<int32_t>(sourceBounds.width);
  height_ = static_cast<int32_t>(sourceBounds.height);

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = texture_;
  dst.mipLevel = 0;
  dst.aspect = WGPUTextureAspect_All;

  WGPUTexelCopyBufferLayout layout = {};
  layout.bytesPerRow = sourceBounds.width * 4u * sizeof(uint16_t);
  layout.rowsPerImage = sourceBounds.height;

  const WGPUExtent3D extent = {sourceBounds.width, sourceBounds.height, 1};
  wgpuQueueWriteTexture(gpu.queue, &dst, half.data(), half.size() * sizeof(uint16_t), &layout,
                        &extent);
  return true;
}

void TransformPreviewTexture::reset() {
  // Safe to release right away, unlike ui/DocumentTexture::retired_'s
  // release-would-crash hazard: THAT hazard is Dear ImGui's WebGPU backend
  // caching one bind group per `AddImage()` texture-id, freed only at
  // `ImGui_ImplWGPU_InvalidateDeviceObjects()` -- so releasing a view it
  // still holds a bind group for turns the NEXT unrelated submit into a
  // validation abort (ui/DocumentTexture.hpp's decision 5 has the exact
  // message). This view is never handed to `AddImage()`; it only ever goes
  // through `addCanvasQuad()`, and ui/CanvasQuad.cpp's own `flushCanvasQuads()`
  // builds every bind group FRESH each frame from the raw view pointer and
  // releases it right after that frame's submit (its own comment: "Built
  // fresh each frame and released after the submit ... That cache is exactly
  // what makes a retired texture view unfreeable ... this module declines to
  // add a second one"). So by the time `reset()` can run -- always a later
  // call than the `addCanvasQuad()` that queued this view, in a codebase with
  // no threading between them -- nothing on the GPU side is still holding a
  // reference to release out from under, and `wgpuTextureRelease` (never
  // `wgpuTextureDestroy` -- the same distinction ui/DocumentTexture.cpp's
  // `release()` draws, for the same "there may be a queued write still
  // pending" reason) is exactly as safe here as it is there.
  if (view_ != nullptr) wgpuTextureViewRelease(view_);
  if (texture_ != nullptr) wgpuTextureRelease(texture_);
  view_ = nullptr;
  texture_ = nullptr;
  width_ = 0;
  height_ = 0;
}

}  // namespace np
