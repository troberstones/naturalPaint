#include "gfx/Context.hpp"

#include <cstdio>

#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

namespace np {
namespace {

struct AdapterResult { WGPUAdapter adapter = nullptr; bool done = false; };
struct DeviceResult { WGPUDevice device = nullptr; bool done = false; };

}  // namespace

bool GpuContext::init(SDL_Window* window) {
  WGPUInstanceDescriptor instDesc = {};
  instance = wgpuCreateInstance(&instDesc);
  if (!instance) {
    std::fprintf(stderr, "[gpu] wgpuCreateInstance failed\n");
    return false;
  }

  // ---- surface (platform-specific; only macOS is wired up so far) ----
#if defined(__APPLE__)
  metalView_ = SDL_Metal_CreateView(window);
  if (!metalView_) {
    std::fprintf(stderr, "[gpu] SDL_Metal_CreateView: %s\n", SDL_GetError());
    return false;
  }
  WGPUSurfaceSourceMetalLayer src = {};
  src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
  src.layer = SDL_Metal_GetLayer(metalView_);

  WGPUSurfaceDescriptor surfDesc = {};
  surfDesc.nextInChain = &src.chain;
  surface = wgpuInstanceCreateSurface(instance, &surfDesc);
#else
  // Windows: WGPUSurfaceSourceWindowsHWND from SDL_PROP_WINDOW_WIN32_HWND_POINTER.
  // Linux:   WGPUSurfaceSourceXlibWindow / WGPUSurfaceSourceWaylandSurface.
#error "naturalPaint: surface creation not yet implemented for this platform"
#endif
  if (!surface) {
    std::fprintf(stderr, "[gpu] CreateSurface failed\n");
    return false;
  }

  // ---- adapter ----
  AdapterResult ares;
  WGPURequestAdapterOptions adapterOpts = {};
  adapterOpts.compatibleSurface = surface;
  adapterOpts.powerPreference = WGPUPowerPreference_HighPerformance;

  WGPURequestAdapterCallbackInfo aci = {};
  aci.mode = WGPUCallbackMode_AllowProcessEvents;
  aci.userdata1 = &ares;
  aci.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a,
                    WGPUStringView message, void* ud1, void*) {
    auto* r = static_cast<AdapterResult*>(ud1);
    if (status != WGPURequestAdapterStatus_Success) {
      std::fprintf(stderr, "[gpu] RequestAdapter: %.*s\n", svLen(message),
                   message.data ? message.data : "");
    }
    r->adapter = a;
    r->done = true;
  };
  wgpuInstanceRequestAdapter(instance, &adapterOpts, aci);
  while (!ares.done) wgpuInstanceProcessEvents(instance);
  adapter = ares.adapter;
  if (!adapter) return false;

  WGPUAdapterInfo info = {};
  wgpuAdapterGetInfo(adapter, &info);
  std::printf("[gpu] adapter: %.*s (%.*s)\n", svLen(info.device),
              info.device.data ? info.device.data : "", svLen(info.description),
              info.description.data ? info.description.data : "");

  // ---- device ----
  DeviceResult dres;
  WGPUDeviceDescriptor devDesc = {};
  // Requested only if the adapter has it. Needed to run the pigment fields at
  // rgba32float, which is the fix for the f16 rounding leak in transfer_pigment
  // (see README "Pigment is not conserved"). Never make it mandatory: it would
  // turn an unsupported GPU into a hard startup failure.
  static const WGPUFeatureName kFloat32Filterable = WGPUFeatureName_Float32Filterable;
  if (wgpuAdapterHasFeature(adapter, kFloat32Filterable)) {
    devDesc.requiredFeatureCount = 1;
    devDesc.requiredFeatures = &kFloat32Filterable;
    hasFloat32Filterable = true;
  }
  devDesc.uncapturedErrorCallbackInfo.callback =
      [](WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*) {
        std::fprintf(stderr, "[gpu] error (%d): %.*s\n", static_cast<int>(type),
                     svLen(message), message.data ? message.data : "");
      };
  devDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
  devDesc.deviceLostCallbackInfo.callback =
      [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message,
         void*, void*) {
        std::fprintf(stderr, "[gpu] device lost (%d): %.*s\n",
                     static_cast<int>(reason), svLen(message),
                     message.data ? message.data : "");
      };

  // ink_pigment.wgsl binds five storage textures; WebGPU only guarantees four.
  // Request the adapter's own limits verbatim rather than a hand-built struct:
  // for alignment limits *smaller* is better, so a zero-initialised struct asks
  // for something stricter than the adapter allows and device creation fails.
  WGPULimits adapterLimits = {};
  if (wgpuAdapterGetLimits(adapter, &adapterLimits) == WGPUStatus_Success) {
    devDesc.requiredLimits = &adapterLimits;
    maxStorageTextures = adapterLimits.maxStorageTexturesPerShaderStage;
    std::printf("[gpu] maxStorageTexturesPerShaderStage: %u\n", maxStorageTextures);
  }

  WGPURequestDeviceCallbackInfo dci = {};
  dci.mode = WGPUCallbackMode_AllowProcessEvents;
  dci.userdata1 = &dres;
  dci.callback = [](WGPURequestDeviceStatus status, WGPUDevice d,
                    WGPUStringView message, void* ud1, void*) {
    auto* r = static_cast<DeviceResult*>(ud1);
    if (status != WGPURequestDeviceStatus_Success) {
      std::fprintf(stderr, "[gpu] RequestDevice: %.*s\n", svLen(message),
                   message.data ? message.data : "");
    }
    r->device = d;
    r->done = true;
  };
  wgpuAdapterRequestDevice(adapter, &devDesc, dci);
  while (!dres.done) wgpuInstanceProcessEvents(instance);
  device = dres.device;
  if (!device) return false;

  queue = wgpuDeviceGetQueue(device);

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(surface, adapter, &caps);
  if (caps.formatCount > 0) surfaceFormat = caps.formats[0];
  wgpuSurfaceCapabilitiesFreeMembers(caps);

  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window, &w, &h);
  configureSurface(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  return true;
}

void GpuContext::configureSurface(uint32_t w, uint32_t h) {
  if (w == 0 || h == 0) return;
  width = w;
  height = h;

  WGPUSurfaceConfiguration config = {};
  config.device = device;
  config.format = surfaceFormat;
  // CopySrc is app/Screenshot: it lets the app photograph its own window, which
  // needs no Screen Recording permission and cannot be defeated by window focus
  // or Spaces. Free on a surface that was already a render target.
  config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  config.width = w;
  config.height = h;
  config.presentMode = WGPUPresentMode_Fifo;
  config.alphaMode = WGPUCompositeAlphaMode_Opaque;
  wgpuSurfaceConfigure(surface, &config);
}

void GpuContext::tick() {
  if (instance) wgpuInstanceProcessEvents(instance);
}

void GpuContext::shutdown() {
  if (queue) { wgpuQueueRelease(queue); queue = nullptr; }
  if (surface) { wgpuSurfaceRelease(surface); surface = nullptr; }
  if (device) { wgpuDeviceRelease(device); device = nullptr; }
  if (adapter) { wgpuAdapterRelease(adapter); adapter = nullptr; }
  if (instance) { wgpuInstanceRelease(instance); instance = nullptr; }
#if defined(__APPLE__)
  if (metalView_) {
    SDL_Metal_DestroyView(metalView_);
    metalView_ = nullptr;
  }
#endif
}

}  // namespace np
