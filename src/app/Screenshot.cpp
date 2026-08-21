#include "app/Screenshot.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "gfx/Context.hpp"

// No STB_IMAGE_WRITE_IMPLEMENTATION here: io/Export.cpp is the one translation
// unit that defines it (see the note at the top of that file), and this one
// links against the bodies compiled there.
#include "stb_image_write.h"

namespace np {
namespace {

// Round `n` up to the next multiple of `to`. WebGPU requires
// `bytesPerRow % 256 == 0` on a texture-to-buffer copy.
uint32_t alignUp(uint32_t n, uint32_t to) { return (n + to - 1) / to * to; }

constexpr uint32_t kCopyAlign = 256;

// Where red sits in one texel of `format`, or -1 for a format this cannot
// write. Refusing by name beats guessing: BGRA and RGBA differ only in that
// reds and blues are exchanged, which is exactly the kind of error a
// screenshot of a mostly-grey UI would carry through review unnoticed.
int redOffsetForFormat(WGPUTextureFormat format) {
  switch (format) {
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb: return 0;
    case WGPUTextureFormat_BGRA8Unorm:
    case WGPUTextureFormat_BGRA8UnormSrgb: return 2;
    default: return -1;
  }
}

}  // namespace

bool captureSurfaceToPng(GpuContext& gpu, WGPUTexture surfaceTexture, uint32_t width,
                         uint32_t height, const std::string& path, std::string* errorOut) {
  auto fail = [&](std::string why) {
    if (errorOut != nullptr) *errorOut = std::move(why);
    return false;
  };
  if (surfaceTexture == nullptr || width == 0 || height == 0)
    return fail("screenshot: no surface texture to capture");

  const int redOffset = redOffsetForFormat(gpu.surfaceFormat);
  if (redOffset < 0)
    return fail("screenshot refused: the surface format this adapter preferred (" +
                std::to_string(static_cast<int>(gpu.surfaceFormat)) +
                ") is not one of the four 8-bit RGBA/BGRA formats this writer knows how to "
                "reorder. Guessing the channel order would produce a PNG with red and blue "
                "exchanged, which is subtle enough to survive being looked at.");

  // Trap 1: the copy stride is padded to 256, and the padding is dropped on
  // the way out. A 1280-wide window is already a legal 5120 bytes, so an
  // unpadded version of this would work here and skew the first odd size.
  const uint32_t tightBytesPerRow = width * 4;
  const uint32_t bytesPerRow = alignUp(tightBytesPerRow, kCopyAlign);
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);
  if (staging == nullptr) return fail("screenshot: could not allocate a staging buffer");

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = surfaceTexture;
  srcTex.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = height;
  WGPUExtent3D extent = {width, height, 1};

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcTex, &dstBuf, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  // The same map-and-spin app/SelfTest's readbackRGBA16F() uses, and for the
  // same reason: `AllowProcessEvents` plus an explicit pump is the only way to
  // resolve a map without a running frame loop underneath it.
  struct MapState {
    bool done = false;
    bool ok = false;
  } state;
  WGPUBufferMapCallbackInfo mci = {};
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.userdata1 = &state;
  mci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
    auto* s = static_cast<MapState*>(ud1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, total, mci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);

  bool ok = false;
  std::vector<uint8_t> rgba;
  if (state.ok) {
    const void* raw = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (raw != nullptr) {
      const auto* bytes = static_cast<const uint8_t*>(raw);
      rgba.resize(static_cast<size_t>(width) * height * 4);
      // Trap 2: reorder to RGBA, per texel, dropping the row padding.
      const int b0 = 2 - redOffset;  // 2 when red is at 0, 0 when red is at 2
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = bytes + static_cast<size_t>(y) * bytesPerRow;
        uint8_t* dst = rgba.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
          dst[x * 4 + 0] = src[x * 4 + static_cast<uint32_t>(redOffset)];
          dst[x * 4 + 1] = src[x * 4 + 1];
          dst[x * 4 + 2] = src[x * 4 + static_cast<uint32_t>(b0)];
          // The surface is configured CompositeAlphaMode_Opaque, so whatever
          // the backbuffer holds in alpha is not meaningful. Forcing 255 keeps
          // a viewer from rendering the whole capture transparent.
          dst[x * 4 + 3] = 255;
        }
      }
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);

  if (!ok) return fail("screenshot: the staging buffer could not be mapped for reading");
  if (stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                     rgba.data(), static_cast<int>(width * 4)) == 0)
    return fail("screenshot: could not write " + path);
  return true;
}

}  // namespace np
