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

namespace {

// Non-sRGB if the adapter offers one, because ImGui's gamma branch keys off it
// (see the call site). The adapter's own preference wins among the candidates
// that qualify, so this is "the adapter's choice, minus the sRGB trap" rather
// than a format imposed on it.
//
// If an adapter ever offers *only* sRGB formats this returns one, and the
// application still presents a correct document -- ui/CanvasQuad compiles the
// encode out when its attachment does it in hardware -- but the chrome carries
// the sag described above. Said out loud rather than silently tolerated.
WGPUTextureFormat pickPresentFormat(const WGPUTextureFormat* formats, size_t count) {
  if (count == 0) return WGPUTextureFormat_BGRA8Unorm;
  for (size_t i = 0; i < count; ++i) {
    if (!presentFormatIsSrgb(formats[i])) return formats[i];
  }
  std::printf("[gpu] WARNING: this adapter offers no non-sRGB surface format, so Dear ImGui "
              "will decode its already-encoded chrome colours and the hardware will re-encode "
              "them. Dark UI tokens will read up to 9 code values low. The document is "
              "unaffected.\n");
  return formats[0];
}

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
    // The canvas is one texture this wide (ui/DocumentTexture), so this is
    // also the largest document that can be drawn. Reported rather than only
    // stored: a refusal that names a number the user never saw reads as
    // arbitrary. See core/CanvasLimits.hpp for what happened before anything
    // consulted it.
    maxTextureDimension = adapterLimits.maxTextureDimension2D;
    std::printf("[gpu] maxTextureDimension2D: %u\n", maxTextureDimension);
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
  // A **non-sRGB** surface, chosen deliberately rather than taken from
  // `caps.formats[0]`, which on this adapter is `BGRA8UnormSrgb`.
  //
  // Dear ImGui's WGPU backend keys its fragment-shader gamma off exactly this
  // value: `pow(rgb, 2.2)` for any `...UnormSrgb` format, `pow(rgb, 1.0)`
  // otherwise. ImGui's vertex colours are already sRGB-encoded bytes, so on an
  // sRGB surface they were decoded (approximately, by that `pow`) and then
  // re-encoded (exactly, by the hardware) -- two curves that are not inverses,
  // which pulled every dark token down by up to 9 code values and was the
  // whole of the unexplained "tokens land 4/255 dark" measurement.
  //
  // On a non-sRGB surface that branch selects 1.0, ImGui's bytes reach the
  // swapchain untouched, and the chrome -- and the pigment swatches, which are
  // *content* and were sagging too -- is exactly right with no colour in this
  // application pre-compensated for a backend quirk.
  //
  // The document is linear light and does need encoding; ui/CanvasQuad does it
  // in its own shader, at the one place a linear value becomes a screen value.
  // See src/app/selftest/PresentTransfer.cpp, which asserts both halves.
  surfaceFormat = pickPresentFormat(caps.formats, caps.formatCount);
  std::printf("[gpu] surface format: %d (%s), %zu offered\n",
              static_cast<int>(surfaceFormat),
              surfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb   ? "BGRA8UnormSrgb"
              : surfaceFormat == WGPUTextureFormat_RGBA8UnormSrgb ? "RGBA8UnormSrgb"
              : surfaceFormat == WGPUTextureFormat_BGRA8Unorm     ? "BGRA8Unorm"
              : surfaceFormat == WGPUTextureFormat_RGBA8Unorm     ? "RGBA8Unorm"
                                                                 : "other",
              caps.formatCount);
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
