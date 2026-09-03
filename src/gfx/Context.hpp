#pragma once
#include <SDL3/SDL.h>

#include <cstdint>

#include "gfx/Wgpu.hpp"

namespace np {

// Whether a swapchain format applies the sRGB encode in hardware on write.
//
// It decides two things that must agree, which is why it is one function
// rather than two switches: Dear ImGui's WGPU backend raises its fragment
// output to the 2.2 for exactly this set of formats, and ui/CanvasQuad must
// encode the linear document itself for exactly the complement of it.
// gfx/Context avoids these formats when the adapter offers an alternative --
// see `pickPresentFormat()` for why.
constexpr bool presentFormatIsSrgb(WGPUTextureFormat f) {
  switch (f) {
    case WGPUTextureFormat_BGRA8UnormSrgb:
    case WGPUTextureFormat_RGBA8UnormSrgb:
    case WGPUTextureFormat_BC1RGBAUnormSrgb:
    case WGPUTextureFormat_BC2RGBAUnormSrgb:
    case WGPUTextureFormat_BC3RGBAUnormSrgb:
    case WGPUTextureFormat_BC7RGBAUnormSrgb:
    case WGPUTextureFormat_ETC2RGB8UnormSrgb:
    case WGPUTextureFormat_ETC2RGB8A1UnormSrgb:
    case WGPUTextureFormat_ETC2RGBA8UnormSrgb: return true;
    default: return false;
  }
}

// Owns the WebGPU device and the window surface. Kept deliberately thin: the
// simulation and UI both just borrow `device` and `queue`.
class GpuContext {
 public:
  bool init(SDL_Window* window);
  void configureSurface(uint32_t width, uint32_t height);
  void shutdown();

  // Callbacks fire from here; call once per frame.
  void tick();

  WGPUInstance instance = nullptr;
  WGPUAdapter adapter = nullptr;
  WGPUDevice device = nullptr;
  WGPUQueue queue = nullptr;
  WGPUSurface surface = nullptr;
  WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;

  bool hasFloat32Filterable = false;
  uint32_t maxStorageTextures = 4;

  // The adapter's `maxTextureDimension2D`, and therefore the largest canvas
  // ui/DocumentTexture can create a composite texture for. Defaults to
  // WebGPU's guaranteed minimum so a failed `wgpuAdapterGetLimits()` leaves a
  // conservative figure rather than a zero that would refuse every document --
  // core/CanvasLimits.hpp is where that figure is consulted and why.
  uint32_t maxTextureDimension = 8192;

  uint32_t width = 0;
  uint32_t height = 0;

 private:
  SDL_MetalView metalView_ = nullptr;
};

}  // namespace np
