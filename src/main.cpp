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
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "app/AppState.hpp"
#include "app/FixedStep.hpp"
#include "app/Keymap.hpp"
#include "app/Latency.hpp"
#include "app/Memory.hpp"
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

// Events that carry a new pointer sample worth judging for responsiveness.
// Pen events cover tablets; mouse events make --latency measurable on the
// mouse/trackpad hardware most dev machines actually have.
bool isPointerSampleEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_AXIS:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return true;
    default:
      return false;
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
  bool latencyVerbose = false;
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
    } else if (a == "--latency") {
      // Prints a line per recorded sample, not just the per-stroke summary.
      latencyVerbose = true;
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

  // 1.4 / ADR-0001: the true "idle" measurement -- SDL, the window and the
  // WebGPU device/surface all exist, but nothing sim-shaped does yet.
  // Captured here, once, before any of the branches below (including
  // --selftest itself) construct a PaintSim, so --selftest's idle-RSS
  // assertion checks a real "before any heavy subsystem exists" number
  // rather than one taken after a sim it constructs eagerly for its own
  // purposes.
  const size_t idleRssBytes = np::currentResidentBytes();

  // Null until something actually needs the solver. --selftest/--diag/
  // --modes exist specifically to exercise it, so they construct it via
  // ensurePaintSim() immediately below, same as before this change; the
  // interactive path leaves it null and defers construction to MacPaintUI's
  // canvas (see drawUI's doc comment) so idle RSS with nothing painted stays
  // near zero rather than paying for the sim on every launch.
  std::unique_ptr<np::PaintSim> sim;

  if (modeTest) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    np::runModeTest(gpu, *s, lut, "mode");
    s->shutdown(); gpu.shutdown();
    SDL_DestroyWindow(window); SDL_Quit();
    return 0;
  }

  if (diagSeconds > 0.0f) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    np::runDiagnostic(gpu, *s, lut, diagSeconds, "np");
    s->shutdown();
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  }

  if (selfTest) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    // 1.4 / ADR-0001 bullets 2 and 3: right after init(), still in the
    // default Watercolour mode, confirms the ink lattice / oil brush grid
    // are genuinely absent -- then cycles setMode() through all three media
    // and confirms each outgoing medium's fields actually get freed, not
    // just the incoming one's allocated.
    const bool fieldAllocOk = np::runFieldAllocationTest(gpu, *s);
    const bool pigmentOk = np::runSelfTest(gpu, *s, lut,
                                    selfTestOut ? selfTestOut : "selftest.png");
    // Headless, GPU-free — doesn't need sim/gpu at all, but runs from the
    // same --selftest entry point since it's the same "does the solver
    // still behave" gate a CI run would check.
    const bool accumulatorOk = np::runAccumulatorTest();
    // 2.3: color/Space's sRGB/Rec.709 transfer function round trip. Also
    // headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool colorSpaceOk = np::runColorSpaceTest();
    // Phase 3 step 1 (ADR-0004): color/Shaper's ACEScct log encode/decode --
    // breakpoint continuity, round trip, a hand-computed known-value check,
    // and monotonicity. Also headless and GPU-free -- pure CPU math, no
    // PaintSim involvement.
    const bool shaperOk = np::runShaperTest();
    // Phase 2 step 15: app/Keymap load, conflict detection and resolve().
    // Headless, GPU-free -- pure CPU/file-IO, no PaintSim involvement.
    const bool keymapOk = np::runKeymapTest();
    // Phase 2 step 2: core/Half's shared half<->float codec and
    // core/TileStore's allocate-on-write / query-without-allocating /
    // iterate-occupied sparse map. Also headless and GPU-free -- pure CPU,
    // no PaintSim involvement.
    const bool tileStoreOk = np::runTileStoreTest();
    // Phase 2 step 6 (decode half): io/ImageDecode's PNG/JPEG/TGA/BMP -> linear
    // float RGBA path. Also headless and GPU-free -- pure CPU decode, no
    // PaintSim involvement.
    const bool imageDecodeOk = np::runImageDecodeTest();
    // Phase 2 step 4: core/Document + core/Layer -- one-entry layer list,
    // LayerKind's seven CONTEXT.md values, and the RGB layer's tile storage
    // round-trip. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool documentOk = np::runDocumentTest();
    // Phase 2 step 14 (PRD C16): the base layer is an ordinary layer with
    // alpha, no locked Background -- core/Layer.hpp has no such concept at
    // all, so this proves the property rather than leaving it assumed. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool baseLayerAlphaOk = np::runBaseLayerAlphaTest();
    // Phase 2 step 5: Document::createBlank() -- given size/working-space,
    // exactly one RGB-kind layer, and zero tiles allocated even for a large
    // canvas. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool createBlankOk = np::runCreateBlankTest();
    // Phase 2 step 6 (the remaining half): io/ImageIO -- premultiply + pack
    // a decoded image into a Document's tiles, on top of createBlank() and
    // ImageDecode. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool imageIOOk = np::runImageIOTest();
    // Phase 2 step 13 (narrow, Document-level slice; PRD I14): io/ImageIO's
    // placeImageAsLayer() -- append an image as a new top layer onto an
    // already-open Document, distinct from openImageAsDocument() creating a
    // brand-new one. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool placeImageAsLayerOk = np::runPlaceImageAsLayerTest();
    // Phase 2 step 8: ui/NaturalPaintUI -- Document -> per-tile GPU texture
    // -> screen, a read-only proof of the tile pipeline independent of the
    // interactive painting canvas. Needs `gpu` for a real device/queue, but
    // no PaintSim -- this module never touches the solver.
    const bool tiledViewportOk = np::runTiledViewportTest(gpu);
    // Phase 2 step 9: ui/NaturalPaintUI's mip pyramid -- CPU-side box-filter
    // downsample correctness, the zoom->level formula, and an end-to-end
    // GPU proof that draw()'s level pick actually changes which texels
    // render. Needs `gpu` for the end-to-end part only -- see SelfTest.hpp.
    const bool mipPyramidOk = np::runMipPyramidTest(gpu);
    // Phase 2 step 10 (narrow, Document-level slice; PRD Q10): core/Probe's
    // probePixel() -- linear + display readout, NxN sample-size averaging,
    // sample-all-layers as a parameter of the sample rather than a separate
    // tool, and premultiply-aware un-premultiplication on read. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool probeOk = np::runProbeTest();
    // Phase 2 step 11 ("View controls", PRD Q1-Q4): the unified view
    // transform's round-trip identity, one hand-worked known-point check,
    // and the view-only proof that mirror/rotation/grayscale never mutate
    // PaintSim's own canvas texture. Needs `gpu`/`*s` only for that last
    // part -- see SelfTest.hpp for the full breakdown.
    const bool viewTransformOk = np::runViewTransformTest(gpu, *s);
    // Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7):
    // app/Snapping.hpp's pure math -- grid spacing/subdivision line
    // positions, the numeric/percentage guide-position parser, and the
    // snapping resolution function against hand-computable cases. Also
    // headless and GPU-free -- rulers/drag-to-create/the popup/the grid
    // overlay itself are UI with no headless driver; see SelfTest.hpp.
    const bool guidesGridSnapOk = np::runGuidesGridSnapTest();
    // Phase 3 step 7 ("Histogram over the visible region"): core/Histogram's
    // computeHistogram() -- display-domain R/G/B/Luma bin placement,
    // alpha<=0 texels excluded, region clipping against the tile store's
    // own allocated-tile iteration, and un-premultiply-before-bin on a
    // translucent pixel. Also headless and GPU-free -- pure CPU, no
    // PaintSim involvement.
    const bool histogramOk = np::runHistogramTest();
    // Phase 3 steps 2+3 (ops/PointOps; docs/operations.md §1.1; PRD B4):
    // Levels, Curves (through color/Shaper), Exposure, Saturation,
    // RGB->grayscale, channel mixer -- each a plain rgb->rgb function --
    // plus the un-premultiply/re-premultiply wrapper bracketing them.
    // Also headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool pointOpsOk = np::runPointOpsTest();
    // Phase 3 step 5 ("core/OpStack -- ordered ops, dirty tracking, run
    // detection for the collapse"): OpStack's add/remove/reorder/setEnabled/
    // setOp mutators and version() bumping, plus detectRuns()'s maximal-run
    // grouping over docs/operations.md's four op classes -- including the
    // "a disabled PointA entry does not split a run" rule. Also headless and
    // GPU-free -- pure CPU bookkeeping plus calls into the already-tested
    // ops/PointOps functions, no PaintSim involvement.
    const bool opStackOk = np::runOpStackTest();
    // Phase 3 step 4 (color/LutBake, ADR-0004): bakes a maximal run of
    // adjacent point ops onto a 32^3 rgba16float 3-D LUT via a seed compute
    // dispatch plus one dispatch per op, and checks the baked result
    // against the CPU ops/PointOps reference at hand-picked grid cells.
    // Needs `gpu` for a real device/queue -- genuine compute-shader work,
    // no PaintSim involvement.
    const bool lutBakeOk = np::runLutBakeTest(gpu);
    // Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch -> un-
    // shape"): sim::PaintSim::updateGradePreview()'s bake-gate/blit
    // pipeline against the live simulation canvas, checked against an
    // independent CPU trilinear-interpolation reference at hand-picked
    // canvas pixels, plus the version-gating rebake proof. Needs `*s` for
    // a real PaintSim -- the same shared instance every other PaintSim-
    // backed --selftest case in this chain already uses.
    const bool applyPassOk = np::runApplyPassTest(gpu, *s);
    // Phase 3 step 8 ("Op-stack UI... and a curve widget operating in the
    // shaper domain"): app/CurveEdit.hpp's pure screen<->curve-space
    // geometry and list-mutation math -- everything the interactive curve
    // widget (ui/MacPaintUI.cpp) calls into. Also headless and GPU-free --
    // pure CPU, no PaintSim involvement.
    const bool curveEditOk = np::runCurveEditTest();
    // 1.3 / ADR-0003: deposited mass must match regardless of stroke speed.
    const bool strokeSpeedOk = np::runStrokeSpeedTest(gpu, *s, lut);
    // 1.4 / ADR-0001 bullet 5: idle RSS, measured before this branch (or
    // any other) ever constructed a PaintSim.
    const bool idleMemOk = np::runIdleMemoryTest(idleRssBytes);
    const bool ok = pigmentOk && accumulatorOk && colorSpaceOk && shaperOk && keymapOk &&
                    tileStoreOk && imageDecodeOk && documentOk && baseLayerAlphaOk &&
                    createBlankOk && imageIOOk && placeImageAsLayerOk && probeOk &&
                    tiledViewportOk && mipPyramidOk && viewTransformOk && guidesGridSnapOk &&
                    histogramOk && pointOpsOk && opStackOk && lutBakeOk && applyPassOk &&
                    curveEditOk && strokeSpeedOk && idleMemOk && fieldAllocOk;
    s->shutdown();
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

  // app/Keymap (Phase 2 step 15, PRD R7/R8): bindings loaded from a data
  // file rather than the `if (e.key.key == SDLK_...)` checks this used to
  // be. Conflicts are detected at load time, not silently resolved by load
  // order -- report them now, once, rather than only when a bad key is
  // actually pressed.
  np::Keymap keymap;
  if (keymap.loadFromFile("default.json")) {
    if (keymap.hasConflicts()) {
      std::fprintf(stderr, "[keymap] %zu conflict(s) in the default keymap:\n",
                   keymap.conflicts().size());
      keymap.reportConflicts();
    }
  } else {
    std::fprintf(stderr,
                 "[keymap] failed to load %s/default.json -- keyboard shortcuts "
                 "will not resolve to any action this session\n",
                 NP_KEYMAP_DIR);
  }

  np::AppState st;
  // st.opStack starts empty -- PLAN.md Phase 3 step 8's real op-authoring
  // UI (ui/MacPaintUI.cpp's GRADE section: add/reorder/toggle/delete, plus
  // the curve widget) is how a user populates it now. Earlier, before this
  // step existed, this constructor seeded two fixed debug ops here so the
  // Apply pass (step 6) had something to show in the running app; that
  // scaffolding is gone now that real UI exists.
  st.sim.brushRadius = st.brush.radius;
  // Fixed timestep (PRD H7): the look of a wash should not depend on the
  // frame rate. `st.sim.dt` is set once, here, to the constant physics tick
  // — never recomputed per frame — so PaintSim::frame()'s existing
  // `params.dt = paramsIn.dt / activeSubsteps` division is unchanged in
  // form. What varies per frame is how many times frame() gets called; see
  // the accumulator loop below.
  st.sim.dt = np::kFixedDt;

  auto prev = std::chrono::steady_clock::now();
  uint32_t frame = 0;
  // Leftover simulated time, in ms, banked between render frames.
  float fixedStepAcc = 0.0f;

  np::Latency latency;
  latency.setVerbose(latencyVerbose);
  bool strokeWasActive = false;

  while (!st.quit) {
    st.lastInputEventNs = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      handlePenEvent(st, e);
      // e.common.timestamp is when SDL generated the event, not when we
      // happened to drain the queue for it — using our own SDL_GetTicksNS()
      // here would understate latency by however long the event sat queued.
      if (isPointerSampleEvent(e)) st.lastInputEventNs = e.common.timestamp;

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
        // Resolve the raw key event through the keymap rather than testing
        // SDL keycodes here. `activeScope` is std::nullopt because no
        // document/layer model exists yet (core/Document + core/Layer are a
        // later Phase 2 step) -- every binding that exists today is global,
        // so this is "no active layer kind," not a stand-in for a real
        // value being dropped.
        const np::KeyChord chord{e.key.key, np::keyModsFromSDL(e.key.mod)};
        const std::optional<std::string> action = keymap.resolve(chord, std::nullopt);
        if (action == "toggle_pause") st.paused = !st.paused;
        else if (action == "clear_canvas") st.requestClear = true;
        else if (action == "reload_shaders") st.requestReload = true;
        else if (action == "quit") st.quit = true;
        // PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4). Fit/100%/
        // zoom-in/zoom-out are request flags because they need the canvas
        // window's actual on-screen size, which only exists inside
        // MacPaintUI's canvas Begin()/End() block -- consumed there, same
        // as requestClear/requestReload are consumed in drawUI itself.
        // Mirror/rotation-reset/grayscale are plain view-state flips with
        // no layout dependency, so they're applied directly, right here,
        // the same way toggle_pause is above. `R` (unmodified) itself is
        // deliberately *not* a binding in keymaps/default.json -- it is a
        // held-plus-drag gesture (rotate view), which app/Keymap's
        // resolve() has no way to express (it only ever fires once per
        // discrete key-down); MacPaintUI.cpp's canvas block reads that key's
        // live held-state directly instead. See that file's comment at its
        // `rotateHeld` local for the full reasoning. `⌘⌥0` "zoom to
        // selection" (PRD Q1) is also deliberately absent -- there is no
        // selection/mask concept anywhere in this codebase yet (that's
        // phase 7, PRD E1); binding it now would mean inventing fake
        // selection state just to give the key something to do.
        else if (action == "fit_window") st.requestFitWindow = true;
        else if (action == "zoom_100") st.requestZoom100 = true;
        else if (action == "zoom_in") st.requestZoomIn = true;
        else if (action == "zoom_out") st.requestZoomOut = true;
        else if (action == "mirror_x") st.view.mirrorX = !st.view.mirrorX;
        else if (action == "mirror_y") st.view.mirrorY = !st.view.mirrorY;
        else if (action == "reset_rotation") st.view.rotation = 0.0f;
        else if (action == "toggle_grayscale") st.view.grayscale = !st.view.grayscale;
        // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping",
        // PRD Q5-Q7): guides/snapping/grid toggles, matching
        // docs/shortcuts.md section 3's Cmd+; / Cmd+Shift+; / Cmd+'.
        // Rulers deliberately has NO entry here and no keymaps/default.json
        // binding at all -- docs/shortcuts.md assigns rulers to Cmd+R, but
        // that chord is already bound to reload_shaders above (Phase 1,
        // predating both this step and step 11). Adding a second Cmd+R
        // binding for rulers would either trip Keymap's own load-time
        // conflict detector or silently make one of the two actions
        // unreachable depending on resolution order -- neither acceptable.
        // This is a known, pre-existing inconsistency between
        // docs/shortcuts.md and the already-shipped keymap, not something
        // to silently paper over by picking a different key; rulers are
        // menu-only (View > Rulers in MacPaintUI.cpp) until a product
        // decision resolves the conflict.
        else if (action == "toggle_guides") st.showGuides = !st.showGuides;
        else if (action == "toggle_snapping") st.snappingEnabled = !st.snappingEnabled;
        else if (action == "toggle_grid") st.showGrid = !st.showGrid;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    st.frameMs = std::chrono::duration<float, std::milli>(now - prev).count();
    prev = now;

    gpu.tick();

    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    np::drawUI(st, sim, gpu, lut, kCanvasW, kCanvasH);

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

    // Arc-length dab emission (1.3, ADR-0003): deposit whatever dabs
    // MacPaintUI's emitter produced this render frame *before* running
    // physics below, so the freshly-laid pigment gets to participate in
    // this frame's advection/diffusion immediately, same as everything
    // already on the canvas. Each dab is its own small, self-contained
    // dispatch (PaintSim::depositDab()) — a fast stroke emitting ten dabs
    // this frame means ten cheap splat dispatches, not ten trips through
    // frame()'s Jacobi solve. Oil doesn't deposit this way (see
    // depositDab()'s doc comment and the loop below), and nothing here
    // should run while paused. Guarded on `sim` existing at all (1.4 /
    // ADR-0001): st.pendingDabs can only be non-empty once MacPaintUI has
    // already constructed the sim (see drawUI's stroke-start block), so
    // this check is defensive rather than load-bearing -- but frame()
    // below genuinely has nothing to step before that first construction.
    if (sim) {
      if (!st.paused && sim->mode() != np::PaintMode::Oil) {
        for (const auto& d : st.pendingDabs) sim->depositDab(gpu, st.sim, d.x, d.y);
      }

      if (st.paused) st.sim.brushActive = 0;
      if (!st.paused || st.sim.brushActive) {
        // Fixed timestep (PRD H7): run however many kFixedDt ticks the
        // accumulator has banked, not exactly one. `steps` can be 0 (a very
        // fast frame hasn't banked a full tick yet) up to kMaxStepsPerFrame
        // (catching up after a stall) — see app/FixedStep.hpp for the two
        // independent caps involved.
        const int steps = np::consumeFixedSteps(fixedStepAcc, st.frameMs, np::kFixedDtMs,
                                                 np::kMaxCatchUpMs, np::kMaxStepsPerFrame);

        // Post-1.3, this guard exists purely for OIL. Watercolour and ink no
        // longer read brushActive/brushA/B inside frame() at all — their
        // deposition happens above, once per emitted dab, entirely outside
        // this loop — so for those two modes the zeroing below is inert
        // bookkeeping, not a gate. Oil's contact -> velocity -> transfer
        // pipeline (kOilSplat/kOilTransfer/kOilBrush) is still driven
        // through frame() itself, reading whatever (lastDab -> newestDab)
        // segment MacPaintUI set in st.sim this render frame. Replaying
        // that same segment on every one of `steps` substeps would
        // multiply oil's contact-driven transfer by however many physics
        // ticks this frame happened to run, so — same convention this loop
        // has used since 1.2 — only the first substep carries the real
        // flags; the rest run with both cleared, advancing oil's
        // levelling/advection only.
        const uint32_t brushActiveThisFrame = st.sim.brushActive;
        const uint32_t brushReloadThisFrame = st.sim.brushReload;
        for (int i = 0; i < steps; ++i) {
          st.sim.brushActive = (i == 0) ? brushActiveThisFrame : 0;
          st.sim.brushReload = (i == 0) ? brushReloadThisFrame : 0;
          sim->frame(gpu, st.sim);
        }
        st.sim.brushActive = brushActiveThisFrame;
        st.sim.brushReload = brushReloadThisFrame;
      }

      // Grayscale preview (PRD Q3): must run *after* the frame()/depositDab
      // calls above, not from inside drawUI (which ran earlier this same
      // iteration, before any of this frame's physics/composite work was
      // even submitted). ImGui's AddImageQuad call in MacPaintUI.cpp only
      // records a texture *view* handle into the draw list -- what it
      // actually samples at present time is whatever the GPU last wrote
      // into that view, which depends on GPU *submission* order, not CPU
      // recording order. Submitting this blit here, after frame()'s own
      // submission, guarantees it reads this frame's fresh composite rather
      // than the previous frame's. Skipped whenever the toggle is off, so
      // it costs nothing then.
      if (st.view.grayscale) sim->updateGrayscalePreview(gpu);
      // Apply pass (PLAN.md Phase 3 step 6): same ordering requirement as
      // the grayscale preview immediately above -- must run after this
      // frame's frame()/depositDab() submissions, not from inside drawUI
      // (which ran earlier this same iteration). Skipped whenever the
      // toggle is off, so it costs nothing then.
      if (st.view.grade) sim->updateGradePreview(gpu, st.opStack);
    }

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
    // st.paintingThisFrame (not st.sim.brushActive): post-1.3 brushActive
    // means "oil has a fresh dab-sourced segment," true on only a fraction
    // of painting frames — the wrong thing to correlate pen-to-photon
    // latency against. paintingThisFrame is the direct "was the user
    // painting, hovered, in bounds this frame" signal the old brushActive
    // used to carry.
    latency.recordFrame(st.paintingThisFrame, st.lastInputEventNs, SDL_GetTicksNS());
    if (strokeWasActive && !st.strokeActive) latency.endStroke();
    strokeWasActive = st.strokeActive;

    wgpuTextureViewRelease(backbuffer);
    wgpuTextureRelease(surfaceTex.texture);
  }

  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (sim) sim->shutdown();
  gpu.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
