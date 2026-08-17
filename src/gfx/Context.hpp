#pragma once
#include <SDL3/SDL.h>

#include <cstdint>

#include "gfx/Wgpu.hpp"

namespace np {

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

  uint32_t width = 0;
  uint32_t height = 0;

 private:
  SDL_MetalView metalView_ = nullptr;
};

}  // namespace np
