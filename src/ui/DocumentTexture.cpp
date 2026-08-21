#include "ui/DocumentTexture.hpp"

#include <array>
#include <chrono>

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

WGPUTextureView DocumentTexture::viewFor(GpuContext& gpu, const OpenDocument& doc,
                                         std::vector<std::string>* warningsOut) {
  const DocumentTextureKey key = documentTextureKey(doc);
  if (key.width <= 0 || key.height <= 0) return nullptr;

  if (view_ != nullptr && haveKey_ && key == key_) {
    ++hits_;
    return view_;
  }

  const auto started = std::chrono::steady_clock::now();

  if (texture_ == nullptr || key.width != texWidth_ || key.height != texHeight_) {
    // Retire, do not release -- see DocumentTexture::retired_ on ImGui's
    // bind-group cache being keyed by the view pointer's address.
    if (texture_ != nullptr) retired_.push_back(Retired{texture_, view_});

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
    // ui/NaturalPaintUI's TiledDocumentView, which builds one per 128-texel
    // tile to serve a zoomed-out tiled viewport, this is a single
    // canvas-sized quad drawn at the view's own zoom, and a mip chain would
    // be rebuilt on every edit for a level that is sampled only when the
    // whole document is minified past 1:2.
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
               WGPUTextureUsage_CopySrc;
    texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
    view_ = wgpuTextureCreateView(texture_, nullptr);
    texWidth_ = key.width;
    texHeight_ = key.height;
  }

  const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc.document, warningsOut);

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = texture_;
  dst.mipLevel = 0;
  dst.aspect = WGPUTextureAspect_All;

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
  wgpuQueueWriteTexture(gpu.queue, &dst, halves.data(), halves.size() * sizeof(uint16_t), &layout,
                        &extent);

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
}

}  // namespace np
