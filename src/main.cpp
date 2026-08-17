// naturalPaint — real-time watercolour on WebGPU.
//
//   Curtis et al. 1997   shallow-water + pigment transport + capillary layer
//   Stam 1999            semi-Lagrangian advection, Jacobi projection
//   Sochorova & Jamriska 2021  Mixbox latent-space pigment mixing
//
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "app/AppState.hpp"
#include "app/SelfTest.hpp"
#include "gfx/Context.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"
#include "ui/MacPaintUI.hpp"
#include "ui/Theme.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_wgpu.h"

namespace {

constexpr uint32_t kCanvasW = 1024;
constexpr uint32_t kCanvasH = 1024;

void handlePenEvent(np::AppState& st, const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_PEN_DOWN:
      st.penSeen = true;
      st.penDown = true;
      break;
    case SDL_EVENT_PEN_UP:
      st.penDown = false;
      st.penPressure = 0.0f;
      break;
    case SDL_EVENT_PEN_AXIS:
      if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        st.penSeen = true;
        st.penPressure = std::clamp(e.paxis.value, 0.0f, 1.0f);
      }
      break;
    default:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // --selftest [out.png] runs the solver headless and checks that latent-space
  // pigment mixing actually produces green where blue crosses yellow.
  const char* selfTestOut = nullptr;
  bool selfTest = false;
  float diagSeconds = 0.0f;
  bool modeTest = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (a == "--selftest") {
      selfTest = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') selfTestOut = argv[++i];
    } else if (a == "--modes") {
      modeTest = true;
    } else if (a == "--diag") {
      // --diag [seconds] : run the solver headless and report where the
      // pigment goes over time.
      diagSeconds = 20.0f;
      if (i + 1 < argc && argv[i + 1][0] != '-') diagSeconds = std::atof(argv[++i]);
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
      "naturalPaint", 1480, 940,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }

  np::GpuContext gpu;
  if (!gpu.init(window)) return 1;

  np::MixboxLut lut;
  if (!lut.load(NP_MIXBOX_LUT)) {
    std::fprintf(stderr, "Could not load the Mixbox LUT. Expected it at:\n  %s\n",
                 NP_MIXBOX_LUT);
    return 1;
  }

  np::PaintSim sim;
  if (!sim.init(gpu, kCanvasW, kCanvasH, lut)) {
    std::fprintf(stderr, "Simulation failed to initialise.\n");
    return 1;
  }

  if (modeTest) {
    np::runModeTest(gpu, sim, lut, "mode");
    sim.shutdown(); gpu.shutdown();
    SDL_DestroyWindow(window); SDL_Quit();
    return 0;
  }

  if (diagSeconds > 0.0f) {
    np::runDiagnostic(gpu, sim, lut, diagSeconds, "np");
    sim.shutdown();
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  }

  if (selfTest) {
    const bool ok = np::runSelfTest(gpu, sim, lut,
                                    selfTestOut ? selfTestOut : "selftest.png");
    sim.shutdown();
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
  }

  // ---- ImGui ----
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the layout is fixed; don't persist window state
  np::applyMacPaintDarkTheme();

  ImGui_ImplSDL3_InitForOther(window);
  ImGui_ImplWGPU_InitInfo wgpuInit;
  wgpuInit.Device = gpu.device;
  wgpuInit.NumFramesInFlight = 3;
  wgpuInit.RenderTargetFormat = gpu.surfaceFormat;
  wgpuInit.DepthStencilFormat = WGPUTextureFormat_Undefined;
  ImGui_ImplWGPU_Init(&wgpuInit);

  np::AppState st;
  st.sim.brushRadius = st.brush.radius;
  // Fixed timestep: the look of a wash should not depend on the frame rate.
  st.sim.dt = 1.0f;

  auto prev = std::chrono::steady_clock::now();
  uint32_t frame = 0;

  while (!st.quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      handlePenEvent(st, e);

      if (e.type == SDL_EVENT_QUIT) st.quit = true;
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          e.window.windowID == SDL_GetWindowID(window))
        st.quit = true;
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        gpu.configureSurface(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        ImGui_ImplWGPU_InvalidateDeviceObjects();
        ImGui_ImplWGPU_CreateDeviceObjects();
      }
      if (e.type == SDL_EVENT_KEY_DOWN) {
        const bool cmd = (e.key.mod & SDL_KMOD_GUI) != 0;
        if (e.key.key == SDLK_SPACE) st.paused = !st.paused;
        if (cmd && e.key.key == SDLK_K) st.requestClear = true;
        if (cmd && e.key.key == SDLK_N) st.requestClear = true;
        if (cmd && e.key.key == SDLK_R) st.requestReload = true;
        if (cmd && e.key.key == SDLK_Q) st.quit = true;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    st.frameMs = std::chrono::duration<float, std::milli>(now - prev).count();
    prev = now;

    gpu.tick();

    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    np::drawUI(st, sim, gpu);

    ImGui::Render();

    // ---- simulation ----
    const auto& pig = np::defaultPalette()[st.brush.pigment];
    const np::Latent z = lut.rgbToLatent(pig.rgb[0], pig.rgb[1], pig.rgb[2]);
    for (int i = 0; i < 3; ++i) {
      st.sim.brushLatentC[i] = z.c[i];
      st.sim.brushLatentR[i] = z.res[i];
    }
    // Physical constants follow the selected paint, not a global slider, so
    // switching from Phthalo Blue to Ultramarine actually changes behaviour.
    st.sim.density = pig.density;
    st.sim.staining = pig.staining;
    st.sim.granulation = pig.granulation;
    st.sim.frame = frame++;

    if (st.paused) st.sim.brushActive = 0;
    if (!st.paused || st.sim.brushActive) sim.frame(gpu, st.sim);

    // ---- present ----
    WGPUSurfaceTexture surfaceTex = {};
    wgpuSurfaceGetCurrentTexture(gpu.surface, &surfaceTex);
    if (!surfaceTex.texture) continue;

    WGPUTextureView backbuffer = wgpuTextureCreateView(surfaceTex.texture, nullptr);

    WGPURenderPassColorAttachment att = {};
    att.view = backbuffer;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue = {0.07, 0.07, 0.075, 1.0};

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(gpu.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    wgpuSurfacePresent(gpu.surface);
    wgpuTextureViewRelease(backbuffer);
    wgpuTextureRelease(surfaceTex.texture);
  }

  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  sim.shutdown();
  gpu.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
