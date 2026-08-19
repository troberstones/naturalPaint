#include "app/SelfTest.hpp"

#include <SDL3/SDL_keyboard.h>

// PLAN.md Phase 4 step 9 measures fsync against F_FULLFSYNC, which is the
// measurement that justifies app/Journal choosing the first. Nothing else in
// this file touches a raw file descriptor.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/CurveEdit.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/FixedStep.hpp"
#include "app/Journal.hpp"
#include "app/Keymap.hpp"
#include "app/LayerPanel.hpp"
#include "app/Memory.hpp"
#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "brush/StrokePath.hpp"
#include "color/LutBake.hpp"
#include "color/Shaper.hpp"
#include "color/Space.hpp"
#include "core/Blend.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Half.hpp"
#include "core/Histogram.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"
#include "core/OpStack.hpp"
#include "core/Probe.hpp"
#include "core/TileStore.hpp"
#include "io/Export.hpp"
#include "io/ExportAs.hpp"
#include "io/ImageDecode.hpp"
#include "io/ImageIO.hpp"
#include "io/NpaintFile.hpp"
#include "io/TileResidency.hpp"
#include "ops/PointOps.hpp"
#include "ops/Resample.hpp"
#include "ui/NaturalPaintUI.hpp"

// NOTE on STB_IMAGE_WRITE_IMPLEMENTATION: io/Export.cpp is the one
// translation unit that defines it -- that macro may only be defined once
// across the whole binary. It used to be defined here, back when --selftest
// was the only thing in the codebase that wrote an image; PRD B6's export
// path is production code now, so the implementation moved to the
// production module and this file includes the header for declarations only
// and links against the bodies io/Export.cpp compiled in. Same arrangement
// io/ImageDecode.cpp already has with paint/Palette.cpp for stb_image.h.
//
// One consequence worth knowing: stbi_zlib_compress() is declared only
// inside stb_image_write.h's implementation section, so it is no longer
// visible here. Nothing in this file needs it -- the 16-bit PNG writer that
// did is now io/Export.hpp's encodePng16(), which this file calls.
#include "stb_image_write.h"

namespace np {
namespace {

int pigmentIndex(const char* name) {
  const auto& pal = defaultPalette();
  for (size_t i = 0; i < pal.size(); ++i)
    if (std::string(pal[i].name) == name) return static_cast<int>(i);
  return 0;
}

// `physical` is false when the caller is overriding density/staining/granulation
// itself, e.g. the NP_WET calibration path.
void loadPigment(SimParams& p, const MixboxLut& lut, int index, bool physical) {
  const auto& pg = defaultPalette()[index];
  const Latent z = lut.rgbToLatent(pg.rgb[0], pg.rgb[1], pg.rgb[2]);
  for (int i = 0; i < 3; ++i) {
    p.brushLatentC[i] = z.c[i];
    p.brushLatentR[i] = z.res[i];
  }
  if (physical) {
    p.density = pg.density;
    p.staining = pg.staining;
    p.granulation = pg.granulation;
  }
}

// Drag the brush from a to b over `steps` frames, stepping the solver each
// time. Pre-1.3 this deposited paint implicitly, once per frame() call, via
// the kSplat/kInkSplat gated inside frame() itself; that path is gone for
// watercolour and ink (ADR-0003 -- deposition is a standalone per-dab
// dispatch now, see PaintSim::depositDab()), so this helper deposits one dab
// per step explicitly for those two media. Oil is unaffected: it still
// deposits through frame()'s own kOilSplat/kOilTransfer/kOilBrush, driven by
// the same brushA/B/brushActive this helper has always set. This is
// deliberately *not* routed through the arc-length emitter (StrokePath) --
// these callers (runSelfTest, runModeTest, runDiagnostic) are exercising
// pigment mixing and transport, not spacing, so "one dab per step" is the
// direct, simplest equivalent of what used to happen automatically.
void stroke(GpuContext& gpu, PaintSim& sim, SimParams& p, float ax, float ay,
            float bx, float by, int steps) {
  const bool depositsViaDab =
      sim.mode() == PaintMode::Watercolor || sim.mode() == PaintMode::Ink;
  float px = ax, py = ay;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float x = ax + (bx - ax) * t;
    const float y = ay + (by - ay) * t;
    p.brushAx = px; p.brushAy = py;
    p.brushBx = x;  p.brushBy = y;
    p.brushActive = 1;
    if (depositsViaDab) sim.depositDab(gpu, p, x, y);
    sim.frame(gpu, p);
    px = x; py = y;
  }
  p.brushActive = 0;
}

void settle(GpuContext& gpu, PaintSim& sim, SimParams& p, int frames) {
  p.brushActive = 0;
  for (int i = 0; i < frames; ++i) sim.frame(gpu, p);
}

// Drags the brush from a to b, but deposits the way 1.3 actually deposits:
// through the arc-length emitter and PaintSim::depositDab(), not stroke()'s
// per-frame swept-segment convention (which splat.wgsl no longer implements
// -- deposition is per-dab now, ADR-0003). `numSamples` stands in for "how
// many times the solver would have sampled the pointer during this stroke",
// i.e. a proxy for stroke speed at a fixed sampling rate: few samples over
// this distance is a fast stroke, many samples is a slow one. Returns the
// total pigment mass (suspended + deposited) after the stroke. No physics
// substeps run in between dabs -- computeStats() is read immediately after
// the last one, so this isolates deposition itself from any transport that
// would otherwise happen at a different total tick count between the two
// speeds (a confound, not the thing ADR-0003 makes a claim about).
double strokeViaDabs(GpuContext& gpu, PaintSim& sim, const SimParams& p,
                     float spacing, float ax, float ay, float bx, float by,
                     int numSamples) {
  StrokePath path;
  path.reset();
  std::vector<Vec2> dabs;
  const float spacingPx = std::max(spacing * p.brushRadius, 0.1f);

  for (int i = 0; i <= numSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(numSamples);
    const float x = ax + (bx - ax) * t;
    const float y = ay + (by - ay) * t;
    dabs.clear();
    path.addPoint(x, y, spacingPx, dabs);
    for (const auto& d : dabs) sim.depositDab(gpu, p, d.x, d.y);
  }
  dabs.clear();
  path.flush(spacingPx, dabs);  // pen-up: emit the tail segment too
  for (const auto& d : dabs) sim.depositDab(gpu, p, d.x, d.y);

  const auto s = sim.computeStats(gpu);
  return s.suspended + s.deposited;
}

struct RGB { float r, g, b; };

RGB sampleMean(const std::vector<uint8_t>& px, uint32_t w, uint32_t cx,
               uint32_t cy, int half) {
  double r = 0, g = 0, b = 0;
  int n = 0;
  for (int y = -half; y <= half; ++y) {
    for (int x = -half; x <= half; ++x) {
      const size_t i = ((static_cast<size_t>(cy + y)) * w + (cx + x)) * 4;
      r += px[i]; g += px[i + 1]; b += px[i + 2];
      ++n;
    }
  }
  return {static_cast<float>(r / n), static_cast<float>(g / n),
          static_cast<float>(b / n)};
}

// stb_image_write's write_to_func callback: append `size` bytes to the
// std::vector<uint8_t> passed as `context`. Used by runImageDecodeTest() to
// generate fixtures entirely in memory (no scratch files).
void appendToVector(void* context, void* data, int size) {
  auto* v = static_cast<std::vector<uint8_t>*>(context);
  const auto* b = static_cast<const uint8_t*>(data);
  v->insert(v->end(), b, b + size);
}

}  // namespace

bool runSelfTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPng) {
  const uint32_t W = sim.width(), H = sim.height();
  const float midY = static_cast<float>(H) * 0.5f;
  const float midX = static_cast<float>(W) * 0.5f;

  SimParams p{};
  p.dt = 1.0f;
  p.brushRadius = 42.0f;
  p.brushWater = 1.3f;
  p.brushPigment = 1.1f;
  p.brushHardness = 0.4f;

  sim.clearCanvas(gpu);

  // --- horizontal yellow stroke, then let it dry down a little ---
  loadPigment(p, lut, pigmentIndex("Hansa Yellow"), true);
  stroke(gpu, sim, p, W * 0.18f, midY, W * 0.82f, midY, 40);
  settle(gpu, sim, p, 30);

  // --- vertical blue stroke straight through it ---
  loadPigment(p, lut, pigmentIndex("Phthalo Blue"), true);
  stroke(gpu, sim, p, midX, H * 0.18f, midX, H * 0.82f, 40);
  settle(gpu, sim, p, 60);

  std::vector<uint8_t> px;
  if (!sim.readbackCanvas(gpu, px)) {
    std::fprintf(stderr, "[selftest] canvas readback failed\n");
    return false;
  }
  if (outPng) {
    stbi_write_png(outPng, static_cast<int>(W), static_cast<int>(H), 4, px.data(),
                   static_cast<int>(W * 4));
    std::printf("[selftest] wrote %s\n", outPng);
  }

  const RGB blank = sampleMean(px, W, W / 10, H / 10, 6);
  const RGB yellow = sampleMean(px, W, static_cast<uint32_t>(W * 0.26f),
                                static_cast<uint32_t>(midY), 6);
  const RGB blue = sampleMean(px, W, static_cast<uint32_t>(midX),
                              static_cast<uint32_t>(H * 0.26f), 6);
  const RGB mix = sampleMean(px, W, static_cast<uint32_t>(midX),
                             static_cast<uint32_t>(midY), 6);

  std::printf("[selftest] paper   rgb(%3.0f,%3.0f,%3.0f)\n", blank.r, blank.g, blank.b);
  std::printf("[selftest] yellow  rgb(%3.0f,%3.0f,%3.0f)\n", yellow.r, yellow.g, yellow.b);
  std::printf("[selftest] blue    rgb(%3.0f,%3.0f,%3.0f)\n", blue.r, blue.g, blue.b);
  std::printf("[selftest] overlap rgb(%3.0f,%3.0f,%3.0f)\n", mix.r, mix.g, mix.b);

  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-46s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  check(yellow.r > blank.r * 0.5f && yellow.b < yellow.r * 0.6f,
        "yellow stroke laid down pigment");
  check(blue.b > blue.r * 1.2f, "blue stroke laid down pigment");

  // The actual claim: green means the mix happened in latent space. Grey would
  // mean it happened in RGB.
  check(mix.g > mix.r * 1.15f, "overlap is greener than it is red");
  check(mix.g > mix.b * 1.05f, "overlap is greener than it is blue");

  std::printf("[selftest] %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

void runModeTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPrefix) {
  const uint32_t W = sim.width(), H = sim.height();

  for (int mi = 0; mi < static_cast<int>(PaintMode::Count); ++mi) {
    const PaintMode m = static_cast<PaintMode>(mi);
    sim.setMode(gpu, m);
    sim.clearCanvas(gpu);

    SimParams p{};
    p.dt = 1.0f;
    p.brushRadius = 34.0f;
    p.brushHardness = 0.45f;
    p.brushWater = (m == PaintMode::Ink) ? 0.9f : 1.3f;
    p.brushPigment = (m == PaintMode::Ink) ? 1.6f : 1.1f;

    // Oil wants a loaded brush and a firmer press.
    p.brushLoad = 1.6f;
    p.penetration = 0.9f;
    if (m == PaintMode::Oil) p.viscosity = 0.18f;  // levelling
    if (const char* ss = std::getenv("NP_SETTLE")) p.settleScale = std::atof(ss);
    if (const char* bl = std::getenv("NP_BLOCK")) p.blocking = std::atof(bl);
    if (const char* om = std::getenv("NP_OMEGA")) p.omega = std::atof(om);
    if (const char* sb = std::getenv("NP_SUB")) sim.substeps = std::atoi(sb);

    const char* first = (m == PaintMode::Ink) ? "Lamp Black" : "Hansa Yellow";
    const char* second = (m == PaintMode::Ink) ? "Lamp Black" : "Phthalo Blue";

    loadPigment(p, lut, pigmentIndex(first), true);
    p.brushReload = 1;
    stroke(gpu, sim, p, W * 0.16f, H * 0.42f, W * 0.84f, H * 0.42f, 60);
    p.brushReload = 0;
    settle(gpu, sim, p, 40);

    loadPigment(p, lut, pigmentIndex(second), true);
    p.brushReload = 1;
    stroke(gpu, sim, p, W * 0.30f, H * 0.20f, W * 0.62f, H * 0.80f, 60);
    p.brushReload = 0;
    settle(gpu, sim, p, 90);

    std::vector<uint8_t> px;
    if (!sim.readbackCanvas(gpu, px)) {
      std::printf("[modes] %s: readback failed\n", paintModeName(m));
      continue;
    }
    std::string out = std::string(outPrefix) + "_" + paintModeName(m) + ".png";
    stbi_write_png(out.c_str(), static_cast<int>(W), static_cast<int>(H), 4,
                   px.data(), static_cast<int>(W * 4));

    const auto s = sim.computeStats(gpu);
    std::printf("[modes] %-12s  pigment %9.1f (susp %7.1f / dep %7.1f)  "
                "activeCells %6.0f  meanSpeed %7.4f  -> %s\n",
                paintModeName(m), s.suspended + s.deposited, s.suspended,
                s.deposited, s.wetCells, s.meanSpeed, out.c_str());
  }
}

void runDiagnostic(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                   float seconds, const char* outPngPrefix) {
  const uint32_t W = sim.width(), H = sim.height();

  SimParams p{};
  p.dt = 1.0f;
  p.brushRadius = 70.0f;
  p.brushWater = 2.0f;
  p.brushPigment = 1.4f;
  p.brushHardness = 0.5f;
  const char* pigName = std::getenv("NP_PIGMENT");
  loadPigment(p, lut, pigmentIndex(pigName ? pigName : "Ultramarine Blue"), true);
  if (const char* g = std::getenv("NP_GRAN")) p.granulation = std::atof(g);
  if (const char* d = std::getenv("NP_DIFF")) p.pigmentDiffuse = std::atof(d);
  if (const char* sl = std::getenv("NP_SLOPE")) p.paperSlope = std::atof(sl);
  if (const char* ad = std::getenv("NP_ADH")) p.adhesion = std::atof(ad);
  if (const char* e = std::getenv("NP_EDGE")) p.edgeDarkening = std::atof(e);
  if (const char* w = std::getenv("NP_WORK")) setWorkingTime(p, std::atof(w));
  if (const char* tx = std::getenv("NP_TILTX")) p.tiltX = std::atof(tx);
  if (const char* ty = std::getenv("NP_TILTY")) p.tiltY = std::atof(ty);
  std::printf("[diag] pigment=%s granulation=%.2f staining=%.2f diffuse=%.2f\n",
              pigName ? pigName : "Ultramarine Blue", p.granulation, p.staining,
              p.pigmentDiffuse);

  if (const char* j = std::getenv("NP_JACOBI")) sim.jacobiIterations = std::atoi(j);
  std::printf("[diag] jacobiIterations=%d substeps=%d\n", sim.jacobiIterations, sim.substeps);

  sim.clearCanvas(gpu);

  // A single fat wet blob in the centre — no stroke motion, so anything that
  // happens afterwards is the solver's doing, not the brush's. This is a
  // direct depositDab() priming loop, not a StrokePath-driven one: 20
  // explicit deposits at the identical point is exactly what "prime a fat
  // stationary blob" means here, and is unrelated to (does not contradict)
  // the arc-length emitter's rule that a stationary *stroke* accumulates no
  // arc length and therefore emits no dabs -- there is no emitter in this
  // loop at all, just direct calls to the same primitive it dispatches to.
  for (int i = 0; i < 20; ++i) {
    p.brushAx = W * 0.5f; p.brushAy = H * 0.5f;
    p.brushBx = W * 0.5f; p.brushBy = H * 0.5f;
    p.brushActive = 1;
    sim.depositDab(gpu, p, W * 0.5f, H * 0.5f);
    sim.frame(gpu, p);
  }
  p.brushActive = 0;

  const int totalFrames = static_cast<int>(seconds * 60.0f);
  const int sampleEvery = totalFrames / 10;

  std::printf("\n  t(s)   suspended   deposited   susp%%   wetCells   pigCells  "
              "pig/wet   meanSpeed    maxSpeed\n");
  std::printf("  ----------------------------------------------------------------"
              "----------------------------------\n");

  auto report = [&](int frame) {
    const auto s = sim.computeStats(gpu);
    const double totalPig = s.suspended + s.deposited;
    const double suspPct = totalPig > 0 ? 100.0 * s.suspended / totalPig : 0.0;
    const double ratio = s.wetCells > 0 ? s.pigmentCells / s.wetCells : 0.0;
    std::printf("  %5.1f  %10.1f  %10.1f  %5.1f  %9.0f  %9.0f  %7.3f  %10.5f  %10.5f\n",
                frame / 60.0, s.suspended, s.deposited, suspPct, s.wetCells,
                s.pigmentCells, ratio, s.meanSpeed, s.maxSpeed);
  };

  report(0);
  int driedFrame = -1;
  for (int f = 1; f <= totalFrames; ++f) {
    sim.frame(gpu, p);
    if (f % sampleEvery == 0) report(f);
    // Poll for the moment the canvas stops moving, at finer resolution than the
    // table: this is what the Working time control is meant to set.
    if (driedFrame < 0 && f % 15 == 0 && sim.computeStats(gpu).wetCells == 0)
      driedFrame = f;
  }
  {
    std::vector<float> dep;
    if (sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float, dep)) {
      double sum = 0, cy = 0, cx = 0;
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
          const float m = dep[((size_t)y * W + x) * 4 + 3];
          sum += m; cy += m * y; cx += m * x;
        }
      if (sum > 0)
        std::printf("\n[diag] deposited centre of mass: (%.1f, %.1f)  "
                    "offset from brush (%.1f, %.1f)\n",
                    cx / sum, cy / sum, cx / sum - W * 0.5, cy / sum - H * 0.5);
    }
  }
  std::printf("\n[diag] working time set to %.1f s (evaporation %.5f); "
              "canvas went dry at %s\n",
              workingTimeOf(p), p.evaporation,
              driedFrame < 0 ? "never (still wet)"
                             : (std::to_string(driedFrame / 60.0).substr(0, 4) + " s").c_str());

  if (outPngPrefix) {
    std::vector<uint8_t> px;
    if (sim.readbackCanvas(gpu, px)) {
      std::string out = std::string(outPngPrefix) + "_diag.png";
      stbi_write_png(out.c_str(), static_cast<int>(W), static_cast<int>(H), 4,
                     px.data(), static_cast<int>(W * 4));
      std::printf("\n[diag] wrote %s\n", out.c_str());
    }
  }
}

bool runAccumulatorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Synthetic "frame time" sequence in ms: a mix of typical paces
  // (~8ms/120Hz, ~16ms/60Hz, ~33ms/30Hz) plus one deliberate 500ms stall
  // (a breakpoint, a window drag) to exercise the catch-up cap.
  const float frameTimesMs[] = {8.3f,  16.7f, 8.3f,  33.3f, 16.7f,
                                500.0f, 16.7f, 8.3f,  33.3f, 16.7f};
  constexpr int n = static_cast<int>(sizeof(frameTimesMs) / sizeof(frameTimesMs[0]));
  constexpr int kStallIdx = 5;

  // --- determinism: two independent accumulators fed the identical input
  // sequence must produce the identical step-count sequence and end in the
  // identical state. This is the property that matters for the interactive
  // path: --diag/--selftest never touch main.cpp's accumulator (they call
  // PaintSim::frame() directly, no wall clock involved), so this is the
  // only place that property is actually checked. ---
  float accA = 0.0f, accB = 0.0f;
  int stepsA[n], stepsB[n];
  for (int i = 0; i < n; ++i) {
    stepsA[i] = consumeFixedSteps(accA, frameTimesMs[i], kFixedDtMs, kMaxCatchUpMs,
                                  kMaxStepsPerFrame);
    stepsB[i] = consumeFixedSteps(accB, frameTimesMs[i], kFixedDtMs, kMaxCatchUpMs,
                                  kMaxStepsPerFrame);
  }
  bool sameSteps = true;
  for (int i = 0; i < n; ++i)
    if (stepsA[i] != stepsB[i]) sameSteps = false;
  check(sameSteps && accA == accB,
        "two accumulators fed identical input produce identical output");

  // --- the 500ms stall must clamp to kMaxStepsPerFrame (8), not
  // floor(500 / (1000/240)) = 120. ---
  check(stepsA[kStallIdx] == kMaxStepsPerFrame,
        "500ms stall clamps to the per-frame step cap, not floor(500/dt)=120");

  // --- the two caps are independent. kMaxCatchUpMs bounds how much real
  // time a single call may ADD to the accumulator; kMaxStepsPerFrame
  // separately bounds how many ticks a single call may REPORT. Isolate the
  // add-side cap: with a tick so coarse that even a maxCatchUpMs-sized
  // addition is still under one tick, and an effectively unlimited step
  // budget, a single huge stall should leave acc at exactly kMaxCatchUpMs —
  // not at the raw, far larger, stall duration. ---
  {
    float acc = 0.0f;
    const float coarseDtMs = kMaxCatchUpMs * 10.0f;
    const int steps =
        consumeFixedSteps(acc, /*realElapsedMs=*/5000.0f, coarseDtMs, kMaxCatchUpMs,
                          /*maxSteps=*/1000);
    check(steps == 0, "coarse tick + huge stall: no whole tick has banked yet");
    check(acc == kMaxCatchUpMs,
          "add-side cap alone bounds acc growth, independent of the step cap");
  }

  // --- and isolate the step-side cap: even with a generous add-side cap
  // that would bank far more than kMaxStepsPerFrame ticks in one shot, the
  // reported step count still holds at the cap. ---
  {
    float acc = 0.0f;
    const int steps = consumeFixedSteps(acc, /*realElapsedMs=*/5000.0f, kFixedDtMs,
                                        /*maxCatchUpMs=*/5000.0f, kMaxStepsPerFrame);
    check(steps == kMaxStepsPerFrame,
          "step-side cap holds even when the add-side cap alone would allow more");
  }

  std::printf("[selftest] accumulator %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// color/Space (Phase 2.3): round-trips both transfer functions and checks
// they agree with each other only where they should (they must not, since
// sRGB and Rec.709 are genuinely different curves despite sharing
// primaries -- see color/Space.hpp).
bool runColorSpaceTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // Spread of linear-domain values: negative (op headroom), zero, either
  // side of each curve's toe breakpoint, mid-range, 1.0 exactly, and above
  // 1.0 (HDR-ish highlight) to exercise the deliberately-unclamped path.
  const float linearValues[] = {-0.5f, 0.0f, 0.001f, 0.01f, 0.02f,
                                0.18f, 0.5f, 1.0f,   2.0f,  4.0f};

  for (float x : linearValues) {
    char label[96];

    const float srgbRT = srgbDecode(srgbEncode(x));
    std::snprintf(label, sizeof label, "sRGB decode(encode(%.4f)) round-trips", x);
    check(near(srgbRT, x, kTol), label);

    const float rec709RT = rec709Decode(rec709Encode(x));
    std::snprintf(label, sizeof label, "Rec.709 decode(encode(%.4f)) round-trips", x);
    check(near(rec709RT, x, kTol), label);
  }

  // The inverse direction too, from the encoded side, including values
  // above 1.0 -- an encoded value need not be display-clamped either.
  // (0.081 is Rec.709's exact encoded-domain breakpoint -- both curves'
  // toe/power segments meet there only approximately, since the published
  // standard constants are rounded decimals, so an input landing exactly
  // on it is a testing artifact rather than a meaningful precision bug.
  // 0.08 and 0.085 straddle it instead.)
  const float encodedValues[] = {-0.3f, 0.0f, 0.02f, 0.08f, 0.085f, 0.3f, 0.9f, 1.0f, 1.5f};
  for (float x : encodedValues) {
    char label[96];

    const float srgbRT = srgbEncode(srgbDecode(x));
    std::snprintf(label, sizeof label, "sRGB encode(decode(%.4f)) round-trips", x);
    check(near(srgbRT, x, kTol), label);

    const float rec709RT = rec709Encode(rec709Decode(x));
    std::snprintf(label, sizeof label, "Rec.709 encode(decode(%.4f)) round-trips", x);
    check(near(rec709RT, x, kTol), label);
  }

  // Sanity checks against known reference points (IEC 61966-2-1 / BT.709).
  check(near(srgbEncode(1.0f), 1.0f, kTol), "sRGB encode(1.0) == 1.0");
  check(near(srgbDecode(1.0f), 1.0f, kTol), "sRGB decode(1.0) == 1.0");
  check(near(srgbEncode(0.0f), 0.0f, kTol), "sRGB encode(0.0) == 0.0");
  check(near(rec709Encode(1.0f), 1.0f, kTol), "Rec.709 encode(1.0) == 1.0");
  check(near(rec709Decode(1.0f), 1.0f, kTol), "Rec.709 decode(1.0) == 1.0");
  // 18% grey, a standard mid-tone reference: sRGB encodes it to roughly
  // 0.46, well off linear 0.18 -- the whole reason the shaper domain in
  // ADR-0004 exists rather than authoring curves against raw linear.
  check(near(srgbEncode(0.18f), 0.4613f, 0.001f),
        "sRGB encode(0.18) lands near the textbook ~0.46 (mid-grey emphasis)");

  // sRGB and Rec.709 must actually differ mid-curve -- if they agreed
  // everywhere, conflating "same primaries" with "same transfer function"
  // (the mistake this task's spec explicitly calls out) would have crept
  // back in unnoticed.
  check(!near(srgbEncode(0.5f), rec709Encode(0.5f), 1e-3f),
        "sRGB and Rec.709 encode 0.5 to visibly different values");

  // Values above 1.0 are not clamped -- they pass through the same curve
  // and keep growing monotonically, confirming the "don't clamp inside the
  // transfer function" decision actually holds in the implementation.
  check(srgbEncode(4.0f) > srgbEncode(1.0f), "sRGB encode does not clamp above 1.0");
  check(rec709Encode(4.0f) > rec709Encode(1.0f),
        "Rec.709 encode does not clamp above 1.0");

  std::printf("[selftest] color space %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// color/Shaper (Phase 3 step 1, ADR-0004). See SelfTest.hpp for the full
// breakdown; ADR-0004 flags this as the single hardest-to-reverse decision
// in the whole colour pipeline ("saved curve control points are coordinates
// in that domain, so changing the shaper later silently shifts every grade
// in every saved document"), so this gets the same rigor as
// runColorSpaceTest() above, plus an explicit breakpoint-continuity check
// that re-derives both formula branches independently of Shaper.cpp's own
// copy of the constants.
bool runShaperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // Re-typed directly from the published ACEScct spec (S-2016-005) -- the
  // same values color/Shaper.cpp uses, kept independent here (not included
  // from that file's anonymous namespace) so this test cannot pass by
  // construction just because it shares Shaper.cpp's own copy of them.
  constexpr float kBreakLin = 0.0078125f;  // 2^-7
  constexpr float kSlopeA = 10.5402377416545f;
  constexpr float kOffsetB = 0.0729055341958355f;
  constexpr float kLogA = 9.72f;
  constexpr float kLogB = 17.52f;

  // --- Continuity at the breakpoint: evaluate BOTH branch formulas
  // directly at lin = kBreakLin, not just calling shaperEncode() once. This
  // is the property that proves the published constants are genuinely
  // self-consistent (a smooth curve, not two segments that merely happen
  // to touch) -- see Shaper.cpp's header comment for the hand-worked
  // derivation this pins. ---
  const float linBranch = kSlopeA * kBreakLin + kOffsetB;
  const float logBranch = (std::log2(kBreakLin) + kLogA) / kLogB;
  std::printf("[selftest] shaper breakpoint: linear branch=%.10f  log branch=%.10f  "
              "shaperEncode(breakLin)=%.10f\n",
              linBranch, logBranch, shaperEncode(kBreakLin));
  check(near(linBranch, logBranch, 1e-6f),
        "shaper breakpoint: hand-evaluated linear and log branches agree in value");
  check(near(shaperEncode(kBreakLin), linBranch, 1e-6f),
        "shaperEncode(breakLin) matches the hand-evaluated linear branch");
  check(near(shaperEncode(kBreakLin), logBranch, 1e-6f),
        "shaperEncode(breakLin) matches the hand-evaluated log branch");

  // --- Round trip, both directions. Negative, zero, either side of the
  // breakpoint, 0.18 (18% grey), 1.0 exactly, and above 1.0 (2.0, 4.0,
  // 16.0) to prove the HDR-headroom property ADR-0004 asks for. ---
  const float linearValues[] = {-0.5f,  0.0f,        0.001f,           kBreakLin * 0.5f,
                                kBreakLin, kBreakLin * 2.0f, 0.18f,           1.0f,
                                2.0f,    4.0f,        16.0f};
  for (float x : linearValues) {
    char label[96];
    const float rt = shaperDecode(shaperEncode(x));
    std::snprintf(label, sizeof label, "shaper decode(encode(%.6f)) round-trips", x);
    check(near(rt, x, kTol), label);
  }
  // Encoded-domain spread for the inverse direction, including the
  // breakpoint itself (0.1552511415525113) and shaperEncode(1.0)
  // (0.5547945205479452) as landmark points.
  const float shapedValues[] = {-0.2f,  0.0f,       0.05f, 0.1552511415525113f,
                                0.3f,   0.5547945205479452f, 0.7f, 0.9f, 1.5f};
  for (float y : shapedValues) {
    char label[96];
    const float rt = shaperEncode(shaperDecode(y));
    std::snprintf(label, sizeof label, "shaper encode(decode(%.6f)) round-trips", y);
    check(near(rt, y, kTol), label);
  }

  // --- Known-value sanity check against a hand-computable reference point,
  // independent of the code's own internal consistency: at lin = 1.0 (above
  // the breakpoint), shaperEncode(1.0) = (log2(1)+9.72)/17.52 = 9.72/17.52. ---
  const float enc1 = shaperEncode(1.0f);
  std::printf("[selftest] shaperEncode(1.0) = %.10f (expected 9.72/17.52 = %.10f)\n", enc1,
              9.72f / 17.52f);
  check(near(enc1, 9.72f / 17.52f, kTol),
        "shaperEncode(1.0) lands near the hand-computed 9.72/17.52 (~0.5547945)");

  // --- Monotonicity: a sorted spread of linear inputs must produce a
  // sorted spread of shaped outputs, across and away from the breakpoint. A
  // non-monotonic log-domain shaper would silently break curve editing. ---
  const float sortedSpread[] = {-1.0f, -0.1f, 0.0f,      0.001f, kBreakLin, 0.05f,
                                0.18f, 0.5f,  1.0f,      2.0f,   8.0f,      16.0f,
                                64.0f};
  bool monotonic = true;
  for (size_t i = 1; i < sizeof(sortedSpread) / sizeof(sortedSpread[0]); ++i) {
    if (!(shaperEncode(sortedSpread[i]) > shaperEncode(sortedSpread[i - 1]))) monotonic = false;
  }
  check(monotonic, "shaperEncode is strictly increasing over a sorted sampled spread");

  std::printf("[selftest] shaper %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// Pins ADR-0003: paints the identical straight-line path twice, at very
// different simulated speeds (few pointer samples vs. many, over the same
// distance -- see strokeViaDabs()'s comment), and checks the two runs
// deposit the same total pigment mass. This is the property the old
// per-frame, dt-scaled splat.wgsl broke: it deposited proportionally to how
// many times sim.frame() got called for a given stroke, so a coarsely
// sampled ("fast") stroke deposited far less than a finely sampled ("slow")
// one covering the identical distance. Arc-length dabs remove sampling rate
// from the equation: dab positions and count along a fixed path depend only
// on distance travelled and spacing.
bool runStrokeSpeedTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const uint32_t W = sim.width(), H = sim.height();
  sim.setMode(gpu, PaintMode::Watercolor);

  SimParams p{};
  p.brushRadius = 24.0f;
  p.brushWater = 1.3f;
  p.brushPigment = 1.1f;
  p.brushHardness = 0.4f;
  loadPigment(p, lut, pigmentIndex("Ultramarine Blue"), true);

  const float ax = W * 0.2f, ay = H * 0.5f, bx = W * 0.8f, by = H * 0.5f;
  constexpr float kSpacing = 0.25f;  // matches BrushState's default

  sim.clearCanvas(gpu);
  const double massFast = strokeViaDabs(gpu, sim, p, kSpacing, ax, ay, bx, by, /*numSamples=*/6);

  sim.clearCanvas(gpu);
  const double massSlow = strokeViaDabs(gpu, sim, p, kSpacing, ax, ay, bx, by, /*numSamples=*/240);

  const double rel = std::abs(massFast - massSlow) / std::max(massSlow, 1.0);
  std::printf("[selftest] stroke speed: fast(6 samples) mass=%.1f  "
              "slow(240 samples) mass=%.1f  rel diff=%.4f\n",
              massFast, massSlow, rel);

  check(massFast > 0.0 && massSlow > 0.0, "both runs deposited pigment");
  // 5%: generous enough to absorb the one-dab-ish boundary rounding a
  // straight path can differ by between two sampling densities (the emitter
  // lags one real sample at the head and extrapolates the tail on flush()),
  // tight enough that reintroducing dt-scaled deposition -- which would
  // make the 240-sample run deposit on the order of 40x the 6-sample run's
  // mass -- fails it by nearly two orders of magnitude, not by a rounding
  // error.
  check(rel < 0.05,
        "deposited mass matches within 5% regardless of stroke speed (ADR-0003)");

  std::printf("[selftest] stroke speed %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// 1.4 / ADR-0001 bullet 5. The 40 MB ceiling is PLAN.md's phase-1 exit
// criterion; it has headroom above SDL/window/WebGPU-device baseline
// footprint (per ADR-0001's own amendment, this ceiling needs slack for
// allocator and driver-version noise, not a tight bound). idleRssBytes == 0
// means task_info() itself failed, which is a measurement failure, not a
// pass.
bool runIdleMemoryTest(size_t idleRssBytes) {
  // 80 MB, not the original 40: measured on 2026-08-18 that SDL3's own video
  // subsystem init (window server connection, display enumeration,
  // keyboard/mouse/pen setup — see SDL_VideoInit()) costs ~57 MB before any
  // of this project's code runs, and WebGPU/Metal device creation adds only
  // ~5 MB more on top. 40 MB was never checked against that baseline. See
  // PLAN.md's Findings for the full breakdown; PRD.md A1 revised to match.
  constexpr size_t kIdleRssCeilingBytes = 80ull * 1024 * 1024;

  // --- The NP_USE_OIIO allowance, stated out loud rather than folded in ---
  //
  // **The 80 MB core ceiling above is unchanged and still binds the
  // dependency-free build.** What follows is a separate, additive,
  // separately-printed allowance that applies only to the NP_USE_OIIO=ON
  // configuration, and it exists because that configuration genuinely does
  // not fit under 80 MB. Measured, on the same machine, in the same session:
  //
  //   NP_USE_OIIO=OFF  idle RSS  63.3 MB   (under the 80 MB ceiling)
  //   NP_USE_OIIO=ON   idle RSS  91.8 MB   (over it, by 11.8 MB)
  //
  // The 28.5 MB difference was isolated to dyld, not to anything this code
  // does, with a standalone two-line program that reads RSS as its first
  // statement: linking nothing costs 1.03 MB resident, linking
  // libOpenImageIO + libOpenImageIO_Util (and their transitive OpenEXR,
  // Imath, OpenColorIO, libtiff, libpng, libjpeg-turbo and giflib
  // dependencies) costs 30.56 MB -- 29.5 MB paid before main() runs a line.
  // The same program then makes its first OpenImageIO call and RSS moves by
  // 0.02 MB, which independently confirms two things: io/Capabilities' lazy,
  // cached probe costs essentially nothing, and there is no eager
  // initialisation left to defer.
  //
  // That last point matters for what this allowance is NOT. PLAN.md step 6
  // ("Lazy OIIO init -- on first file open, not at startup, so PRD A2
  // holds") will not recover this: OpenImageIO's own initialisation is
  // already lazy and already free. The 29.5 MB is the dynamic loader mapping
  // and relocating the libraries, which only dlopen()-ing OpenImageIO on
  // first use could defer -- a different linkage architecture, not a tuning
  // change, and not in Phase 4 step 2/3's scope.
  //
  // So: 32 MB, slightly above the measured 29.5 MB so a future OpenImageIO
  // point release does not turn this into a flake, and deliberately not
  // "whatever makes 91.8 pass". If the OIIO build's idle RSS ever needs more
  // than this, that is a real regression and should fail here rather than
  // being accommodated again.
  constexpr size_t kOiioDylibAllowanceBytes = 32ull * 1024 * 1024;
  const bool oiio = oiioBackendCompiledIn();
  const size_t ceiling = kIdleRssCeilingBytes + (oiio ? kOiioDylibAllowanceBytes : 0);

  const double mb = static_cast<double>(idleRssBytes) / (1024.0 * 1024.0);
  const bool ok = idleRssBytes > 0 && idleRssBytes < ceiling;
  if (oiio) {
    // Printed as a sum, never as one number, so the allowance can never
    // read as if the core budget had quietly grown.
    std::printf("[selftest] idle RSS %.1f MB (ceiling 80 MB core + 32 MB OpenImageIO dylib "
                "allowance = %.0f MB; the OpenImageIO dylib chain costs 29.5 MB at load, "
                "measured) %s\n",
                mb, static_cast<double>(ceiling) / (1024.0 * 1024.0), ok ? "pass" : "FAIL");
  } else {
    std::printf("[selftest] idle RSS %.1f MB (ceiling 80 MB) %s\n", mb, ok ? "pass" : "FAIL");
  }
  return ok;
}

// 1.4 / ADR-0001 bullets 2 and 3. Not a tautology: allocFields()
// unconditionally builds the shared water/pigment/deposit set (needed by all
// three media), but allocInkFields()/allocOilFields() are only ever reached
// from setMode(Ink)/setMode(Oil) — a sim fresh out of init() has never taken
// either path, so the first half checks that fact holds rather than
// assuming it. The second half exercises setMode() across all three media
// and checks the *outgoing* medium's fields actually get freed, not just
// the incoming one's allocated — this is what makes ADR-0001's per-mode
// residency table (watercolour/ink/oil each quoted independently) true of a
// session that visits more than one medium, rather than only of a session
// that visits exactly one.
bool runFieldAllocationTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  check(!sim.inkFieldsAllocated(),
        "ink lattice absent -- no ink content has ever been painted");
  check(!sim.oilFieldsAllocated(),
        "oil brush grid absent -- no oil content has ever been painted");

  sim.setMode(gpu, PaintMode::Ink);
  check(sim.inkFieldsAllocated(), "ink lattice allocated after switching to Ink");
  sim.setMode(gpu, PaintMode::Oil);
  check(!sim.inkFieldsAllocated(), "ink lattice freed after switching away to Oil");
  check(sim.oilFieldsAllocated(), "oil brush grid allocated after switching to Oil");
  sim.setMode(gpu, PaintMode::Watercolor);
  check(!sim.oilFieldsAllocated(),
        "oil brush grid freed after switching away to Watercolour");
  check(!sim.inkFieldsAllocated(), "ink lattice still absent (never revisited)");

  std::printf("[selftest] field allocation %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// Phase 2 step 15 / PRD R7, R8. Two halves:
//
// 1. The real, shipped keymaps/default.json loads clean (no false-positive
//    conflicts -- it only binds four distinct chords to four distinct
//    global actions today, so it shouldn't have any) and resolves its real
//    chords to the action names main.cpp actually dispatches on.
//
// 2. A small in-memory fixture keymap -- not keymaps/default.json --
//    purpose-built to exercise the layer-kind-scope-aware conflict
//    detector. The shipped default keeps to real, currently-existing
//    actions only (see Keymap.hpp's header comment for why binding a key to
//    a dead action would be worse than not building the scope machinery
//    generally); the scope-conflict behaviour genuinely has nothing to
//    exercise it in the real file today, since no Pigment/Media/Strokes/
//    Adjustment/Text/Flats layer exists anywhere in the codebase yet. This
//    fixture is that exercise: two different, currently-nonexistent
//    layer-kind scopes sharing a key (must NOT conflict), a global binding
//    added on top of one of them (must conflict, twice -- global overlaps
//    every scope), and a scope-only binding checked through resolve().
bool runKeymapTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. the real default keymap ---
  Keymap km;
  const bool loaded = km.loadFromFile("default.json");
  check(loaded, "keymaps/default.json loads");
  if (loaded) {
    check(!km.hasConflicts(), "keymaps/default.json has no false-positive conflicts");

    const auto space = km.resolve(KeyChord{SDLK_SPACE, 0}, std::nullopt);
    check(space == std::optional<std::string>("toggle_pause"),
          "resolve(Space) -> toggle_pause");

    const auto cmdK = km.resolve(KeyChord{SDLK_K, kModCmd}, std::nullopt);
    const auto cmdN = km.resolve(KeyChord{SDLK_N, kModCmd}, std::nullopt);
    check(cmdK == std::optional<std::string>("clear_canvas") &&
              cmdN == std::optional<std::string>("clear_canvas"),
          "resolve(Cmd+K) and resolve(Cmd+N) both -> clear_canvas");

    const auto cmdR = km.resolve(KeyChord{SDLK_R, kModCmd}, std::nullopt);
    check(cmdR == std::optional<std::string>("reload_shaders"),
          "resolve(Cmd+R) -> reload_shaders");

    const auto cmdQ = km.resolve(KeyChord{SDLK_Q, kModCmd}, std::nullopt);
    check(cmdQ == std::optional<std::string>("quit"), "resolve(Cmd+Q) -> quit");

    const auto unbound = km.resolve(KeyChord{SDLK_Z, 0}, std::nullopt);
    check(unbound == std::nullopt, "resolve() on an unbound chord returns nothing");
  }

  // --- 2. the scope-conflict fixture (test-only; never shipped) ---
  const char* fixture = R"json(
{
  "name": "keymap test fixture",
  "bindings": [
    { "key": "G", "mods": ["Cmd"], "scope": "Flats",   "action": "fixture.flats_action" },
    { "key": "G", "mods": ["Cmd"], "scope": "Media",    "action": "fixture.media_action" },
    { "key": "G", "mods": ["Cmd"],                      "action": "fixture.global_action" },
    { "key": "H",                  "scope": "Strokes",  "action": "fixture.strokes_h_action" }
  ]
}
)json";

  Keymap fx;
  const bool fxLoaded = fx.loadFromString(fixture, "keymap test fixture");
  check(fxLoaded, "fixture keymap parses");
  if (fxLoaded) {
    auto hasConflict = [&](const std::string& a, const std::string& b) {
      for (const auto& c : fx.conflicts())
        if ((c.actionA == a && c.actionB == b) || (c.actionA == b && c.actionB == a))
          return true;
      return false;
    };

    check(!hasConflict("fixture.flats_action", "fixture.media_action"),
          "same key, disjoint layer-kind scopes (Flats vs Media) is NOT a conflict");
    check(hasConflict("fixture.global_action", "fixture.flats_action"),
          "same key, global vs Flats-scoped IS a conflict (global overlaps every scope)");
    check(hasConflict("fixture.global_action", "fixture.media_action"),
          "same key, global vs Media-scoped IS a conflict (global overlaps every scope)");
    check(fx.conflicts().size() == 2,
          "exactly the two global-vs-scoped pairs are reported, not the disjoint pair");

    const KeyChord h{SDL_GetKeyFromName("H"), 0};
    check(fx.resolve(h, LayerKind::Strokes) ==
              std::optional<std::string>("fixture.strokes_h_action"),
          "resolve(H, scope=Strokes) returns the Strokes-scoped action");
    check(fx.resolve(h, LayerKind::Media) == std::nullopt,
          "resolve(H, scope=Media) returns nothing -- H is only bound under Strokes");
    check(fx.resolve(h, std::nullopt) == std::nullopt,
          "resolve(H, no active layer) returns nothing -- no global H binding exists");
  }

  std::printf("[selftest] keymap %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// core/Half (factored out of sim/PaintSim.cpp's private half<->float decoder
// so core/Tile's pixel storage can reuse it for the encode direction too)
// and core/TileStore (PLAN.md Phase 2 step 2). Both land in one test since
// TileStore's pixel round-trip depends on Half being correct underneath it
// -- a Half bug would otherwise show up as a confusing TileStore failure.
//
// Half coverage: zero, negative zero, subnormals, ordinary values and the
// highest/lowest finite half magnitudes, round-tripped through both
// directions. This is the "silently corrupts everything downstream" class
// of bug -- every tile pixel, and every readback in sim/PaintSim.cpp, goes
// through this code -- so it gets a permanent gate here, in addition to the
// full-range exhaustive cross-check against the hardware `_Float16` path
// done once during development (see core/Half.hpp's header comment).
//
// TileStore coverage, per PLAN.md step 2's own wording:
//  - "allocate on write": getOrCreate() makes a tile that find() then sees.
//  - "query without allocating": find() on an untouched coordinate returns
//    nullptr AND leaves occupiedTileCount() unchanged -- the naive-operator[]
//    bug PLAN.md's phrasing is calling out.
//  - "iterate occupied tiles": iteration visits exactly the allocated
//    tiles, no more, no less.
//  - A negative document coordinate (x = -1) round-trips through
//    tileCoordAt/tileLocalOffset and a write/read, exercising Tile.hpp's
//    floor semantics rather than only the positive-coordinate happy path.
bool runTileStoreTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Half: direct round-trips, both directions ---
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  check(floatToHalf(0.0f) == 0x0000u, "floatToHalf(0.0) == +0 half bit pattern");
  check(floatToHalf(-0.0f) == 0x8000u, "floatToHalf(-0.0) == -0 half bit pattern");
  check(halfToFloat(0x0000u) == 0.0f && !std::signbit(halfToFloat(0x0000u)),
        "halfToFloat(+0 bits) == +0.0");
  check(halfToFloat(0x8000u) == 0.0f && std::signbit(halfToFloat(0x8000u)),
        "halfToFloat(-0 bits) == -0.0 (sign preserved)");

  // Smallest half subnormal (2^-24) and a mid-range subnormal (2^-20,
  // exactly representable in half -- no rounding to muddy the check).
  check(nearf(halfToFloat(floatToHalf(5.960464478e-8f)), 5.960464478e-8f, 1e-10f),
        "half subnormal round-trip: 2^-24");
  check(nearf(halfToFloat(floatToHalf(9.5367431640625e-7f)), 9.5367431640625e-7f, 1e-12f),
        "half subnormal round-trip: 2^-20 (exact)");

  // Ordinary values, including a negative one and a non-power-of-two.
  for (float v : {1.0f, -1.0f, 0.5f, 100.25f, -3.14159f, 0.1f}) {
    char label[64];
    std::snprintf(label, sizeof label, "half round-trip near %.5f", static_cast<double>(v));
    // 2^-11 relative is half's worst-case rounding error (10-bit mantissa).
    check(nearf(halfToFloat(floatToHalf(v)), v, std::fabs(v) * 0.0005f + 1e-6f), label);
  }

  // Highest and lowest finite half magnitudes: 65504 is exactly
  // representable in both float and half, so this must be an exact
  // round-trip, not just "close".
  check(floatToHalf(65504.0f) == 0x7BFFu, "floatToHalf(65504) == max finite half bits");
  check(halfToFloat(0x7BFFu) == 65504.0f, "halfToFloat(max finite bits) == 65504.0 exactly");
  check(floatToHalf(-65504.0f) == 0xFBFFu, "floatToHalf(-65504) == lowest finite half bits");
  check(halfToFloat(0xFBFFu) == -65504.0f, "halfToFloat(lowest finite bits) == -65504.0 exactly");
  // A magnitude beyond half's range must overflow to infinity, not wrap or
  // saturate silently.
  check(std::isinf(halfToFloat(floatToHalf(1.0e6f))),
        "a magnitude beyond half's range overflows to infinity, not silent saturation");

  // --- TileStore: allocate on write / query without allocating / iterate ---
  {
    TileStore store;
    check(store.occupiedTileCount() == 0, "a fresh TileStore has no occupied tiles");

    const TileCoord origin{0, 0};
    check(store.find(origin) == nullptr,
          "find() on an untouched coordinate returns nullptr");
    check(store.occupiedTileCount() == 0,
          "find() on a miss allocates nothing -- occupied count still 0");

    const std::array<float, 4> pixel{0.25f, 0.5f, 0.75f, 1.0f};
    Tile& t = store.getOrCreate(origin);
    t.writePixel(PixelCoord{5, 7}, pixel);
    check(store.occupiedTileCount() == 1,
          "getOrCreate() on a miss allocates exactly one tile");

    const Tile* found = store.find(origin);
    check(found != nullptr, "find() sees the tile getOrCreate() just made");
    if (found) {
      const auto rt = found->readPixel(PixelCoord{5, 7});
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "write-then-read round-trips a pixel within half-float precision");
    }

    // getOrCreate() on an already-occupied coordinate must not allocate a
    // second tile, and must return the same one (still see the pixel).
    Tile& again = store.getOrCreate(origin);
    check(&again == found, "getOrCreate() on an occupied coordinate returns the same tile");
    check(store.occupiedTileCount() == 1,
          "getOrCreate() on a hit does not grow the occupied count");

    // A second, distinct tile.
    store.getOrCreate(TileCoord{3, -2}).writePixel(PixelCoord{0, 0}, {1.0f, 0.0f, 0.0f, 1.0f});
    check(store.occupiedTileCount() == 2, "a second write allocates a second tile");

    // Iteration visits exactly the allocated tiles, no more, no less.
    size_t seen = 0;
    bool sawOrigin = false, sawOther = false;
    for (const auto& [coord, tile] : store) {
      ++seen;
      (void)tile;
      if (coord == origin) sawOrigin = true;
      if (coord == TileCoord{3, -2}) sawOther = true;
    }
    check(seen == 2, "iteration visits exactly the 2 allocated tiles");
    check(sawOrigin && sawOther, "iteration visits both specific coordinates written");

    // A coordinate that was never written still isn't there.
    check(store.find(TileCoord{99, 99}) == nullptr,
          "an unrelated, never-written coordinate is still absent after other writes");
  }

  // --- Negative document coordinate: exercises Tile.hpp's floor semantics ---
  {
    TileStore store;
    const PixelCoord doc{-1, -1};
    const TileCoord coord = tileCoordAt(doc);
    const PixelCoord local = tileLocalOffset(doc);
    check(coord == (TileCoord{-1, -1}),
          "document pixel (-1,-1) falls in tile (-1,-1), not tile (0,0)");
    check(local == (PixelCoord{kTileSize - 1, kTileSize - 1}),
          "document pixel (-1,-1)'s local offset is the tile's bottom-right corner");

    const std::array<float, 4> pixel{0.1f, 0.2f, 0.3f, 0.4f};
    store.getOrCreate(coord).writePixel(local, pixel);
    const Tile* found = store.find(coord);
    check(found != nullptr, "negative-coordinate tile is findable after being written");
    if (found) {
      const auto rt = found->readPixel(local);
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "negative document coordinate round-trips a pixel correctly");
    }
    // Document pixel (0,0) is a different tile-local position entirely
    // (tile (0,0), offset (0,0)) -- confirms the floor split, not just a
    // single coordinate in isolation.
    check(tileCoordAt(PixelCoord{0, 0}) == (TileCoord{0, 0}),
          "document pixel (0,0) falls in tile (0,0) -- the floor boundary lands correctly");
  }

  std::printf("[selftest] tile store %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// io/ImageDecode (PLAN.md Phase 2 step 6, decode half). PLAN.md's own verify
// criterion for this phase is explicit: open both an 8-bit and a 16-bit PNG
// and check known pixel values, so those two cases are not optional. BMP,
// TGA and JPEG fixtures round out coverage of the other three formats
// STBI_ONLY_x now admits (Palette.cpp) -- these are the format the actual
// STBI_ONLY_x wiring could silently break (a wrong macro leaves stb_image's
// decoder for that format compiled out and decodeImageLinear() failing at
// runtime instead of at compile time).
bool runImageDecodeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // 8-bit source: byte quantization plus the transfer-function curve itself
  // can shift the decoded value by a bit more than 1/255 in the steep part
  // of the curve, hence a tolerance a little looser than "1 ULP at 8 bits".
  constexpr float kTol8 = 0.01f;
  // 16-bit source is precise enough for a tight check.
  constexpr float kTol16 = 0.001f;
  // JPEG is lossy (8x8 block DCT + chroma subsampling) -- even a flat block
  // isn't guaranteed byte-exact after quantization, so this stays generous
  // and is not meant to catch small regressions, only "did JPEG decode at
  // all and land in roughly the right place."
  constexpr float kTolJpeg = 0.06f;

  // --- 8-bit PNG: 2x2 fixture with known corners --------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        128, 128, 128, 255,  200, 40,  40,  128,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    std::string err;
    const DecodedImage img = decodeImageLinear(png.data(), png.size(), &err);
    check(img.valid(), "8-bit PNG fixture decodes");
    if (img.valid()) {
      check(img.width == 2 && img.height == 2, "8-bit PNG: dimensions match");
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      const float* tl = at(0, 0);
      const float* tr = at(1, 0);
      const float* bl = at(0, 1);
      const float* br = at(1, 1);
      check(near(tl[0], 1.0f, kTol8) && near(tl[1], 1.0f, kTol8) && near(tl[2], 1.0f, kTol8) &&
                near(tl[3], 1.0f, kTol8),
            "8-bit PNG: white corner (255) decodes to linear (1,1,1,1)");
      check(near(tr[0], 0.0f, kTol8) && near(tr[1], 0.0f, kTol8) && near(tr[2], 0.0f, kTol8) &&
                near(tr[3], 1.0f, kTol8),
            "8-bit PNG: black corner (0) decodes to linear (0,0,0,1)");
      check(near(bl[0], srgbDecode(128 / 255.0f), kTol8) &&
                near(bl[1], srgbDecode(128 / 255.0f), kTol8),
            "8-bit PNG: mid-grey (128) matches srgbDecode(128/255)");
      check(near(br[0], srgbDecode(200 / 255.0f), kTol8) &&
                !near(br[0], 200 / 255.0f, 0.02f),
            "8-bit PNG: colour channel is sRGB-decoded, not left encoded");
      check(near(br[3], 128 / 255.0f, 1e-4f),
            "8-bit PNG: alpha (128) passes through linearly, not sRGB-decoded");
    }
  }

  // --- 8-bit PNG, no alpha channel: decodes fully opaque -------------------
  {
    const uint8_t px[3] = {100, 100, 100};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 3, px, 3);

    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid() && img.width == 1 && img.height == 1, "3-channel PNG fixture decodes");
    if (img.valid()) {
      check(near(img.pixels[3], 1.0f, 1e-4f),
            "PNG with no alpha channel decodes fully opaque (alpha = 1.0)");
      check(near(img.pixels[0], srgbDecode(100 / 255.0f), kTol8),
            "3-channel PNG: colour channel still sRGB-decoded");
    }
  }

  // --- 16-bit PNG: 2x2 fixture, built by io/Export's encodePng16() ---------
  // stb_image_write's PNG writer (used for every other fixture here) is
  // 8-bit-per-channel only, so this fixture goes through the hand-rolled
  // 16-bit writer instead. That writer used to live in this file as a
  // test-only helper; PRD B6 made 16-bit export a P0 production
  // requirement, so it is now io/Export.hpp's encodePng16() and this
  // fixture calls the same one production export does -- there is exactly
  // one 16-bit PNG writer in the binary, not a test copy that can drift.
  {
    const uint16_t px[2 * 2 * 4] = {
        65535, 65535, 65535, 65535,  0,     0,     0,     65535,
        32768, 32768, 32768, 65535,  0,     0,     65535, 32768,
    };
    const std::vector<uint8_t> png = encodePng16(2, 2, px);

    std::string err;
    const DecodedImage img = decodeImageLinear(png.data(), png.size(), &err);
    check(img.valid(), "16-bit PNG fixture decodes");
    if (!img.valid() && !err.empty()) std::printf("    (%s)\n", err.c_str());
    if (img.valid()) {
      check(img.width == 2 && img.height == 2, "16-bit PNG: dimensions match");
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      const float* tl = at(0, 0);
      const float* tr = at(1, 0);
      const float* bl = at(0, 1);
      const float* br = at(1, 1);
      check(near(tl[0], 1.0f, kTol16) && near(tl[3], 1.0f, kTol16),
            "16-bit PNG: white corner (65535) ~ linear (1,1,1,1)");
      check(near(tr[0], 0.0f, kTol16) && near(tr[3], 1.0f, kTol16),
            "16-bit PNG: black corner (0) ~ linear (0,0,0,1)");
      check(near(bl[0], srgbDecode(32768 / 65535.0f), kTol16),
            "16-bit PNG: mid-grey (32768) matches srgbDecode(32768/65535)");
      check(near(br[2], srgbDecode(65535 / 65535.0f), kTol16) && near(br[0], 0.0f, kTol16) &&
                near(br[1], 0.0f, kTol16),
            "16-bit PNG: pure-blue pixel isolated to the right channel");
      check(near(br[3], 32768 / 65535.0f, 1e-4f),
            "16-bit PNG: alpha (32768) passes through linearly at 16-bit precision");
    }
  }

  // --- BMP: lossless container, exact-value check --------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        255, 0,   0,   255,  0,   255, 0,   255,
    };
    std::vector<uint8_t> bmp;
    stbi_write_bmp_to_func(&appendToVector, &bmp, 2, 2, 4, px);

    const DecodedImage img = decodeImageLinear(bmp.data(), bmp.size());
    check(img.valid() && img.width == 2 && img.height == 2, "BMP fixture decodes");
    if (img.valid()) {
      // BMP is lossless but stb_image normalizes row order to top-to-bottom
      // regardless of source format (BMP's own on-disk convention is
      // bottom-up), so corner checks here are the same shape as PNG's.
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      check(near(at(0, 0)[0], 1.0f, kTol8), "BMP: white corner round-trips");
      check(near(at(1, 0)[0], 0.0f, kTol8), "BMP: black corner round-trips");
      check(near(at(0, 1)[0], srgbDecode(1.0f), kTol8) && near(at(0, 1)[1], 0.0f, kTol8),
            "BMP: pure-red pixel isolated to the right channel");
    }
  }

  // --- TGA: lossless container, exact-value check ---------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        255, 0,   0,   255,  0,   255, 0,   255,
    };
    std::vector<uint8_t> tga;
    stbi_write_tga_to_func(&appendToVector, &tga, 2, 2, 4, px);

    const DecodedImage img = decodeImageLinear(tga.data(), tga.size());
    check(img.valid() && img.width == 2 && img.height == 2, "TGA fixture decodes");
    if (img.valid()) {
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      check(near(at(0, 0)[0], 1.0f, kTol8), "TGA: white corner round-trips");
      check(near(at(1, 0)[0], 0.0f, kTol8), "TGA: black corner round-trips");
      check(near(at(0, 1)[0], srgbDecode(1.0f), kTol8) && near(at(0, 1)[1], 0.0f, kTol8),
            "TGA: pure-red pixel isolated to the right channel");
    }
  }

  // --- JPEG: lossy, generous tolerance, no exact-value assertions ----------
  {
    // 16x16, aligned to JPEG's 8x8 DCT block size: left half solid white,
    // right half solid black, sampled well away from the block boundary in
    // the middle to dodge quantization ringing at the edge.
    constexpr int kJpegSize = 16;
    std::vector<uint8_t> px(static_cast<size_t>(kJpegSize) * kJpegSize * 3);
    for (int y = 0; y < kJpegSize; ++y)
      for (int x = 0; x < kJpegSize; ++x) {
        const uint8_t v = (x < kJpegSize / 2) ? 255 : 0;
        uint8_t* p = &px[(static_cast<size_t>(y) * kJpegSize + x) * 3];
        p[0] = p[1] = p[2] = v;
      }
    std::vector<uint8_t> jpg;
    stbi_write_jpg_to_func(&appendToVector, &jpg, kJpegSize, kJpegSize, 3, px.data(), 90);

    const DecodedImage img = decodeImageLinear(jpg.data(), jpg.size());
    check(img.valid() && img.width == kJpegSize && img.height == kJpegSize,
          "JPEG fixture decodes");
    if (img.valid()) {
      auto at = [&](int x, int y) {
        return &img.pixels[(static_cast<size_t>(y) * kJpegSize + x) * 4];
      };
      const float* white = at(3, 8);   // interior of the white block
      const float* black = at(12, 8);  // interior of the black block
      check(near(white[0], 1.0f, kTolJpeg), "JPEG: white block decodes near linear 1.0");
      check(near(black[0], 0.0f, kTolJpeg), "JPEG: black block decodes near linear 0.0");
      check(near(white[3], 1.0f, 1e-4f),
            "JPEG (no alpha channel) decodes fully opaque (alpha = 1.0)");
    }
  }

  std::printf("[selftest] image decode %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// core/Document + core/Layer (PLAN.md Phase 2 step 4). See SelfTest.hpp for
// the full breakdown; in short: LayerKind's seven CONTEXT.md values
// round-trip through the name helpers that moved here from app/Keymap in
// this same step; a default Layer is Pigment-kinded with no RGB storage
// (only RGB is wired up -- "design for N, ship 1"); a Document holds what
// it's built with and starts with no layers (nothing here manufactures the
// one-layer document -- that's PLAN.md's next, separate step,
// Document::createBlank()); and a hand-built one-entry, RGB-kind layer list
// round-trips a pixel through core::TileStore exactly as runTileStoreTest()
// already proved TileStore itself does, just reached through Layer::rgbTiles
// this time.
bool runDocumentTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- LayerKind: all seven CONTEXT.md kinds round-trip name <-> kind ---
  const LayerKind allKinds[] = {LayerKind::Pigment,    LayerKind::RGB,   LayerKind::Media,
                                 LayerKind::Strokes,    LayerKind::Adjustment,
                                 LayerKind::Text,       LayerKind::Flats};
  bool allRoundTrip = true;
  for (LayerKind k : allKinds) {
    const auto back = layerKindFromName(layerKindName(k));
    if (!back || *back != k) allRoundTrip = false;
  }
  check(allRoundTrip, "all 7 CONTEXT.md LayerKind values round-trip through name<->kind");
  check(layerKindFromName("NotAKind") == std::nullopt,
        "layerKindFromName() rejects an unrecognized name");

  // --- Layer: default kind is Pigment (CONTEXT.md's domain default), and
  // only RGB actually gets tile storage wired up ---
  Layer defaultLayer;
  check(defaultLayer.kind == LayerKind::Pigment,
        "a default-constructed Layer's kind is Pigment (CONTEXT.md's domain default)");
  check(!defaultLayer.rgbTiles.has_value(),
        "a default (Pigment-kind) Layer has no RGB tile storage populated");

  // --- Document: holds what it's constructed with; layer list starts empty
  // (this step ships the types, not a document-creation policy) ---
  Document doc;
  doc.width = 64;
  doc.height = 48;
  doc.workingSpace.primaries = kRec709Primaries;

  check(doc.width == 64 && doc.height == 48, "Document holds its assigned width/height");
  check(doc.workingSpace.primaries.redX == kRec709Primaries.redX &&
            doc.workingSpace.primaries.whiteY == kRec709Primaries.whiteY,
        "Document holds its assigned working space");
  check(doc.layers.empty(), "a freshly constructed Document's layer list starts empty");

  // --- The "ship 1" case: one RGB-kind layer, added the way a future
  // createBlank()/place-image step would, exercising the round trip
  // PLAN.md's step asks for ---
  Layer rgbLayer;
  rgbLayer.kind = LayerKind::RGB;
  rgbLayer.rgbTiles.emplace();
  doc.layers.push_back(rgbLayer);

  check(doc.layers.size() == 1, "Document's layer list holds exactly one entry (ship 1)");
  check(doc.layers[0].kind == LayerKind::RGB, "the one layer's kind is RGB");
  check(doc.layers[0].rgbTiles.has_value(), "the RGB layer's tile storage is populated");

  if (doc.layers[0].rgbTiles) {
    TileStore& tiles = *doc.layers[0].rgbTiles;
    const std::array<float, 4> pixel{0.2f, 0.4f, 0.6f, 1.0f};
    const TileCoord coord{0, 0};
    tiles.getOrCreate(coord).writePixel(PixelCoord{10, 20}, pixel);

    const Tile* found = tiles.find(coord);
    check(found != nullptr, "a pixel written through the layer's TileStore is findable");
    if (found) {
      const auto rt = found->readPixel(PixelCoord{10, 20});
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "the layer's RGB tile storage round-trips a pixel via core::TileStore's API");
    }
  }

  std::printf("[selftest] document %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 2 step 14 / PRD C16: "the base layer is an ordinary layer
// with alpha, no locked Background." core/Layer.hpp has no
// background/locked concept at all -- every Layer, first or otherwise, is
// the same struct -- so the property holds by construction; this proves it
// rather than leaving it as an assumption. The two things a locked/special
// Background *would* do differently from an ordinary layer: refuse a
// non-opaque alpha, and be distinguishable from other layers by some flag.
// Neither exists here.
bool runBaseLayerAlphaTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  const Document doc = Document::createBlank(64, 64, WorkingSpace{});
  check(doc.layers.size() == 1, "createBlank()'s base layer is the document's only layer");

  Layer& base = const_cast<Document&>(doc).layers[0];
  check(base.rgbTiles.has_value(), "the base layer has ordinary RGB tile storage");
  if (base.rgbTiles) {
    // Fully transparent -- if the base layer were a locked/opaque
    // Background, this write would either be rejected or silently forced
    // back to alpha=1. Neither happens: TileStore::writePixel/readPixel
    // treat this layer exactly like any other.
    const std::array<float, 4> transparent{0.0f, 0.0f, 0.0f, 0.0f};
    base.rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{5, 5}, transparent);
    const auto rt = base.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{5, 5});
    check(nearf(rt[3], 0.0f, 0.001f),
          "the base layer's alpha channel writes and reads back 0 -- not clamped to opaque");

    // And an ordinary partial alpha, at the opposite end from the existing
    // io/ImageIO coverage's 128/255 and 64/255 fixtures, to show the whole
    // [0,1] range is honoured, not just "not fully transparent."
    const std::array<float, 4> partial{0.5f, 0.25f, 0.1f, 0.75f};
    base.rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{6, 6}, partial);
    const auto rt2 = base.rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{6, 6});
    check(nearf(rt2[3], 0.75f, 0.01f),
          "the base layer's alpha channel round-trips an arbitrary partial value");
  }

  std::printf("[selftest] base layer alpha %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// Document::createBlank() (PLAN.md Phase 2 step 5, PRD C7). Separate
// function from runDocumentTest() above rather than folded into it: step 4
// shipped the Document/Layer *types*; step 5 is a distinct policy decision
// on top of them (what kind the blank layer is, whether it starts with any
// tiles allocated), same "one function per PLAN.md step" shape as
// runTileStoreTest()/runImageDecodeTest() elsewhere in this file.
//
// The size/working-space/layer-count/layer-kind checks are the ordinary
// half. The interesting half is occupiedTileCount() == 0 for a *large*
// canvas: a naive createBlank() that loops over width/height allocating a
// tile per (or per-region) would still pass every check at a small size,
// and only a large one (4096x4096 here) forces enough tiles that an
// accidental pre-allocation loop becomes visibly, immediately wrong rather
// than passing by coincidence.
bool runCreateBlankTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- small canvas: the ordinary happy path ---
  {
    WorkingSpace space;
    space.primaries = kRec709Primaries;
    const Document doc = Document::createBlank(64, 48, space);

    check(doc.width == 64 && doc.height == 48,
          "createBlank(64,48): Document holds the given width/height");
    check(doc.workingSpace.primaries.redX == kRec709Primaries.redX &&
              doc.workingSpace.primaries.whiteY == kRec709Primaries.whiteY,
          "createBlank(64,48): Document holds the given working space");
    check(doc.layers.size() == 1, "createBlank(64,48): layer list has exactly one entry");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "createBlank(64,48): the one layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value(),
            "createBlank(64,48): the RGB layer's tile storage is populated");
      if (doc.layers[0].rgbTiles)
        check(doc.layers[0].rgbTiles->occupiedTileCount() == 0,
              "createBlank(64,48): zero tiles allocated immediately after creation");
    }
  }

  // --- large canvas (4096x4096): the assertion that actually catches a
  // wrong implementation. Same checks, but at a size where "pre-allocate a
  // grid across the canvas" would be an obvious, large, non-zero tile
  // count rather than something a small-canvas test could miss by luck. ---
  {
    WorkingSpace space;
    space.primaries = kRec709Primaries;
    const Document doc = Document::createBlank(4096, 4096, space);

    check(doc.width == 4096 && doc.height == 4096,
          "createBlank(4096,4096): Document holds the given width/height");
    check(doc.layers.size() == 1,
          "createBlank(4096,4096): layer list has exactly one entry");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "createBlank(4096,4096): the one layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value(),
            "createBlank(4096,4096): the RGB layer's tile storage is populated");
      if (doc.layers[0].rgbTiles)
        check(doc.layers[0].rgbTiles->occupiedTileCount() == 0,
              "createBlank(4096,4096): zero tiles allocated despite the large canvas "
              "(PRD C2 -- memory tracks content, not canvas dimensions)");
    }
  }

  std::printf("[selftest] create blank %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// io/ImageIO (PLAN.md Phase 2 step 6's remaining half). See SelfTest.hpp for
// the full breakdown; in short: openImageAsDocument() on a small PNG with
// alpha < 1 corners produces a right-sized, one-RGB-layer Document whose
// tiles hold rgb*a (not straight rgb) at those corners; a multi-tile-
// spanning image occupies exactly the tiles its footprint covers, not
// something tied to a larger nominal canvas (PRD C2); the lower-level
// writeDecodedImageIntoLayer() round-trips against a hand-built Layer on
// its own, independent of openImageAsDocument(); and corrupt/truncated
// bytes fail cleanly.
bool runImageIOTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // Byte-quantization tolerance, same magnitude as runImageDecodeTest()'s
  // kTol8 -- this fixture is 8-bit-per-channel, and every value checked
  // here passed through decodeImageLinear() first.
  constexpr float kTol8 = 0.01f;

  // --- openImageAsDocument: 2x2 PNG, two corners with alpha < 1 so
  // premultiplied and straight alpha are actually distinguishable ---------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  200, 40,  40,  128,
        0,   0,   0,   255,  100, 150, 200, 64,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    std::string err;
    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size(), &err);
    check(docOpt.has_value(), "openImageAsDocument: valid 2x2 PNG fixture decodes");

    if (docOpt) {
      const Document& doc = *docOpt;
      check(doc.width == 2 && doc.height == 2,
            "openImageAsDocument: Document is sized to the decoded image");
      check(doc.layers.size() == 1, "openImageAsDocument: Document has exactly one layer");
      if (doc.layers.size() == 1) {
        check(doc.layers[0].kind == LayerKind::RGB,
              "openImageAsDocument: the one layer is RGB-kind");
        check(doc.layers[0].rgbTiles.has_value(),
              "openImageAsDocument: the RGB layer's tile storage is populated");

        if (doc.layers[0].rgbTiles) {
          const TileStore& tiles = *doc.layers[0].rgbTiles;
          check(tiles.occupiedTileCount() == 1,
                "openImageAsDocument: a 2x2 image occupies exactly one 128x128 tile "
                "(PRD C2 -- memory tracks occupied tiles, not canvas dimensions)");

          const Tile* tile = tiles.find(TileCoord{0, 0});
          check(tile != nullptr, "openImageAsDocument: tile (0,0) exists");
          if (tile) {
            const auto tl = tile->readPixel(PixelCoord{0, 0});
            check(near(tl[0], 1.0f, kTol8) && near(tl[1], 1.0f, kTol8) &&
                      near(tl[2], 1.0f, kTol8) && near(tl[3], 1.0f, kTol8),
                  "openImageAsDocument: opaque white corner (alpha=1) premultiplies to itself");

            const auto bl = tile->readPixel(PixelCoord{0, 1});
            check(near(bl[0], 0.0f, kTol8) && near(bl[1], 0.0f, kTol8) &&
                      near(bl[2], 0.0f, kTol8) && near(bl[3], 1.0f, kTol8),
                  "openImageAsDocument: opaque black corner (alpha=1) premultiplies to itself");

            // (1,0): (200, 40, 40, 128) -- alpha = 128/255 ~ 0.502.
            const auto tr = tile->readPixel(PixelCoord{1, 0});
            const float trA = 128 / 255.0f;
            const float trRLin = srgbDecode(200 / 255.0f);
            const float trGLin = srgbDecode(40 / 255.0f);
            const float trBLin = srgbDecode(40 / 255.0f);
            check(near(tr[0], trRLin * trA, kTol8) && near(tr[1], trGLin * trA, kTol8) &&
                      near(tr[2], trBLin * trA, kTol8) && near(tr[3], trA, kTol8),
                  "openImageAsDocument: alpha=128/255 pixel stores rgb*a, alpha unchanged");
            check(!near(tr[0], trRLin, 0.05f),
                  "openImageAsDocument: that pixel's red genuinely differs from the "
                  "un-multiplied value -- proves premultiply ran, not just alpha passthrough");

            // (1,1): (100, 150, 200, 64) -- alpha = 64/255 ~ 0.251.
            const auto br = tile->readPixel(PixelCoord{1, 1});
            const float brA = 64 / 255.0f;
            const float brRLin = srgbDecode(100 / 255.0f);
            const float brGLin = srgbDecode(150 / 255.0f);
            const float brBLin = srgbDecode(200 / 255.0f);
            check(near(br[0], brRLin * brA, kTol8) && near(br[1], brGLin * brA, kTol8) &&
                      near(br[2], brBLin * brA, kTol8) && near(br[3], brA, kTol8),
                  "openImageAsDocument: alpha=64/255 pixel stores rgb*a, alpha unchanged");
            check(!near(br[2], brBLin, 0.05f),
                  "openImageAsDocument: that pixel's blue genuinely differs from the "
                  "un-multiplied value -- proves premultiply ran, not just alpha passthrough");
          }
        }
      }
    }
  }

  // --- occupied-tile count tracks the image's own footprint, not a larger
  // nominal canvas: a 140x140 opaque-grey image spans a 2x2 grid of
  // 128x128 tiles (tile (1,*) and (*,1) only exist because the image
  // crosses x=128/y=128), so this also exercises the multi-tile write path
  // runImageDecodeTest()'s 2x2 fixtures above never reach ---------------
  {
    constexpr int kSize = 140;
    std::vector<uint8_t> px(static_cast<size_t>(kSize) * kSize * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = 128;
      px[i + 1] = 128;
      px[i + 2] = 128;
      px[i + 3] = 255;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, kSize, kSize, 4, px.data(), kSize * 4);

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "openImageAsDocument: 140x140 PNG fixture decodes");
    if (docOpt && docOpt->layers.size() == 1 && docOpt->layers[0].rgbTiles) {
      check(docOpt->layers[0].rgbTiles->occupiedTileCount() == 4,
            "openImageAsDocument: a 140x140 image occupies exactly the 2x2=4 tiles its "
            "footprint spans, not a count tied to some other canvas size");
    }
  }

  // --- writeDecodedImageIntoLayer: separately callable against a
  // hand-built Layer, independent of openImageAsDocument -- this is the
  // exact reuse PLAN.md step 13 ("place an image as a layer") needs later
  // against a layer inside an already-open Document -----------------------
  {
    const uint8_t px[1 * 1 * 4] = {60, 120, 180, 90};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 4, px, 4);

    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid(), "writeDecodedImageIntoLayer: 1x1 fixture decodes");
    if (img.valid()) {
      Layer layer;
      layer.kind = LayerKind::RGB;
      layer.rgbTiles.emplace();
      writeDecodedImageIntoLayer(img, layer);

      check(layer.rgbTiles->occupiedTileCount() == 1,
            "writeDecodedImageIntoLayer: writing a 1x1 image allocates exactly one tile");
      const Tile* tile = layer.rgbTiles->find(TileCoord{0, 0});
      check(tile != nullptr, "writeDecodedImageIntoLayer: tile (0,0) exists after writing");
      if (tile) {
        const auto rt = tile->readPixel(PixelCoord{0, 0});
        const float a = 90 / 255.0f;
        check(near(rt[0], srgbDecode(60 / 255.0f) * a, kTol8) &&
                  near(rt[1], srgbDecode(120 / 255.0f) * a, kTol8) &&
                  near(rt[2], srgbDecode(180 / 255.0f) * a, kTol8) && near(rt[3], a, kTol8),
              "writeDecodedImageIntoLayer: pixel lands premultiplied in a hand-built Layer");
      }
    }

    // A Layer whose RGB tile storage isn't populated (the common case --
    // default kind is Pigment, per core/Layer.hpp) must be a safe no-op,
    // never a crash: this is the misuse case, not the intended call shape.
    Layer noRgbLayer;
    writeDecodedImageIntoLayer(img, noRgbLayer);
    check(!noRgbLayer.rgbTiles.has_value(),
          "writeDecodedImageIntoLayer: no-op against a Layer with no RGB tile storage");
  }

  // --- Failure handling: corrupt/truncated bytes fail cleanly, never a
  // bogus Document -----------------------------------------------------
  {
    const uint8_t garbage[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::string err;
    const std::optional<Document> docOpt =
        openImageAsDocument(garbage, sizeof(garbage), &err);
    check(!docOpt.has_value(), "openImageAsDocument: garbage bytes return std::nullopt");
    check(!err.empty(),
          "openImageAsDocument: failure forwards decodeImageLinear()'s error string");
  }
  {
    // A real PNG stream, cut off partway through -- exercises stb_image's
    // "ran out of bytes mid-decode" path, not just "never a PNG at all".
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        0,   0,   0,   255,  255, 255, 255, 255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);
    const size_t truncatedSize = png.size() / 2;

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), truncatedSize);
    check(!docOpt.has_value(),
          "openImageAsDocument: truncated PNG bytes return std::nullopt, not a bogus Document");
  }
  {
    const std::optional<Document> docOpt = openImageAsDocument(nullptr, 0);
    check(!docOpt.has_value(), "openImageAsDocument: null/empty input returns std::nullopt");
  }

  std::printf("[selftest] image io %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runPlaceImageAsLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol8 = 0.01f;  // same magnitude as runImageIOTest()'s kTol8.

  // Same 2x2 fixture as runImageIOTest()'s first block -- two corners with
  // alpha < 1 so premultiplied and straight alpha are distinguishable.
  const uint8_t px[2 * 2 * 4] = {
      255, 255, 255, 255,  200, 40,  40,  128,
      0,   0,   0,   255,  100, 150, 200, 64,
  };
  std::vector<uint8_t> png;
  stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

  // --- placing into a Document::createBlank() document: the base layer
  // stays at index 0, untouched, and the placed image lands as a second
  // layer appended to the end (top of the stack) ------------------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    check(doc.layers.size() == 1, "placeImageAsLayer: fixture starts as a 1-layer document");

    std::string err;
    const bool placed = placeImageAsLayer(doc, png.data(), png.size(), &err);
    check(placed, "placeImageAsLayer: valid 2x2 PNG places successfully");
    check(err.empty(), "placeImageAsLayer: no error string on success");

    check(doc.layers.size() == 2,
          "placeImageAsLayer: document now has exactly two layers");
    if (doc.layers.size() == 2) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "placeImageAsLayer: original base layer at index 0 is still RGB-kind");
      check(doc.layers[0].rgbTiles.has_value() && doc.layers[0].rgbTiles->occupiedTileCount() == 0,
            "placeImageAsLayer: original base layer at index 0 is unchanged (still no tiles)");

      const Layer& placedLayer = doc.layers[1];
      check(placedLayer.kind == LayerKind::RGB,
            "placeImageAsLayer: the new layer (index 1, top of the stack) is RGB-kind");
      check(placedLayer.rgbTiles.has_value(),
            "placeImageAsLayer: the new layer's tile storage is populated");

      if (placedLayer.rgbTiles) {
        const TileStore& tiles = *placedLayer.rgbTiles;
        check(tiles.occupiedTileCount() == 1,
              "placeImageAsLayer: a 2x2 image occupies exactly one 128x128 tile");

        const Tile* tile = tiles.find(TileCoord{0, 0});
        check(tile != nullptr, "placeImageAsLayer: tile (0,0) exists in the new layer");
        if (tile) {
          // (1,0): (200, 40, 40, 128) -- alpha = 128/255 ~ 0.502.
          const auto tr = tile->readPixel(PixelCoord{1, 0});
          const float trA = 128 / 255.0f;
          const float trRLin = srgbDecode(200 / 255.0f);
          check(near(tr[0], trRLin * trA, kTol8) && near(tr[3], trA, kTol8),
                "placeImageAsLayer: placed pixel's rgb*a matches the source image "
                "(premultiplied), alpha unchanged");
          check(!near(tr[0], trRLin, 0.05f),
                "placeImageAsLayer: that pixel's red genuinely differs from the "
                "un-multiplied value -- proves premultiply ran, not just a copy");

          // Opaque corner: alpha = 1, so premultiplied == straight.
          const auto tl = tile->readPixel(PixelCoord{0, 0});
          check(near(tl[0], 1.0f, kTol8) && near(tl[3], 1.0f, kTol8),
                "placeImageAsLayer: opaque corner (alpha=1) premultiplies to itself");
        }
      }
    }
  }

  // --- placing into a Document that starts with zero layers: the function
  // doesn't assume index 0 already exists -- it only ever appends -------
  {
    Document doc;  // width/height/workingSpace default-constructed; layers empty.
    check(doc.layers.empty(), "placeImageAsLayer: fixture starts as a 0-layer document");

    const bool placed = placeImageAsLayer(doc, png.data(), png.size());
    check(placed, "placeImageAsLayer: places successfully into a 0-layer document");
    check(doc.layers.size() == 1,
          "placeImageAsLayer: a 0-layer document ends up with exactly one layer, at index 0");
    if (doc.layers.size() == 1) {
      check(doc.layers[0].kind == LayerKind::RGB,
            "placeImageAsLayer: that layer is RGB-kind");
      check(doc.layers[0].rgbTiles.has_value() &&
                doc.layers[0].rgbTiles->occupiedTileCount() == 1,
            "placeImageAsLayer: that layer's tiles hold the placed image");
    }
  }

  // --- the DecodedImage-taking overload, exercised directly (not only
  // reached through the file-bytes overload) ----------------------------
  {
    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid(), "placeImageAsLayer: DecodedImage fixture decodes");

    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    const bool placed = placeImageAsLayer(doc, img);
    check(placed, "placeImageAsLayer(DecodedImage): places successfully");
    check(doc.layers.size() == 2,
          "placeImageAsLayer(DecodedImage): document now has exactly two layers");
  }

  // --- clean failure: garbage bytes leave doc.layers completely
  // untouched, never a partially-inserted broken layer -------------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    const size_t layersBefore = doc.layers.size();

    const uint8_t garbage[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::string err;
    const bool placed = placeImageAsLayer(doc, garbage, sizeof(garbage), &err);
    check(!placed, "placeImageAsLayer: garbage bytes fail cleanly (returns false)");
    check(!err.empty(),
          "placeImageAsLayer: failure forwards decodeImageLinear()'s error string");
    check(doc.layers.size() == layersBefore,
          "placeImageAsLayer: doc.layers is completely untouched after a failed place");
  }

  // --- clean failure via the DecodedImage overload: an invalid image is
  // also a no-op, matching writeDecodedImageIntoLayer()'s own contract ---
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    const size_t layersBefore = doc.layers.size();

    const DecodedImage invalid;  // width == 0, valid() == false.
    const bool placed = placeImageAsLayer(doc, invalid);
    check(!placed, "placeImageAsLayer(DecodedImage): invalid image fails cleanly");
    check(doc.layers.size() == layersBefore,
          "placeImageAsLayer(DecodedImage): doc.layers is untouched after a failed place");
  }

  std::printf("[selftest] place image as layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

namespace {

// Embedded WGSL for a tiny textured-quad blit, used only by
// runTiledViewportTest() below to place one uploaded tile at a known screen
// rect and read the result back -- NOT part of ui/NaturalPaintUI's production
// API, which draws exclusively via ImDrawList::AddImage
// (TiledDocumentView::draw()). AddImage's own rendering goes through ImGui's
// WebGPU backend, which needs a live ImGui context/frame to drive; this
// module's actual position math and GPU upload don't need any of that, so
// this shader lets --selftest exercise them directly against a real
// offscreen WebGPU render target instead -- the same rigor every other piece
// of this project's pipeline is held to (PaintSim's own readbackCanvas/
// readbackField).
//
// `vs` maps the unit square [0,1]^2 (via vertex_index, two triangles) onto
// [P.rectMin, P.rectMax] in pixel space, then to NDC against P.targetSize --
// exactly the rect tileScreenRect() computes, handed in via the uniform
// below. `fs` does a plain nearest-texel fetch (textureLoad, no sampler) at
// the tile-local texel the interpolated uv lands on, so a fixture pixel maps
// to a screen block with an exact, hand-computable boundary -- no bilinear
// blending to reason about when picking sample points below.
//
// `fs` derives the texel grid from textureDimensions(tileTex) rather than a
// hardcoded 128, so this same shader works whether `tileTex` is bound to a
// tile's all-levels view (level 0's own dimensions, 128x128, same as
// before -- runTiledViewportTest() below reads that view's level 0
// explicitly) or one of GpuTile::levelViews' single-level views (that
// level's own, smaller dimensions -- runMipPyramidTest()'s mip-selection
// proof below). A texture_2d view scoped to exactly one mip level reports
// that level's size as textureDimensions' result, which is exactly what
// this needs and why NaturalPaintUI's draw() binds a single-level view per
// PLAN.md step 9 rather than relying on automatic LOD selection.
constexpr const char* kBlitShaderSrc = R"(
struct BlitParams {
  rectMin : vec2<f32>,
  rectMax : vec2<f32>,
  targetSize : vec2<f32>,
  _pad : vec2<f32>,
};

@group(0) @binding(0) var<uniform> P : BlitParams;
@group(0) @binding(1) var tileTex : texture_2d<f32>;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var uvs = array<vec2<f32>, 6>(
      vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
      vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
  let uv = uvs[vi];
  let px = mix(P.rectMin, P.rectMax, uv);
  var out : VSOut;
  out.pos = vec4<f32>(px.x / P.targetSize.x * 2.0 - 1.0,
                      1.0 - px.y / P.targetSize.y * 2.0, 0.0, 1.0);
  out.uv = uv;
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let clampedUv = clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.999999));
  let dims = vec2<f32>(textureDimensions(tileTex));
  let texel = vec2<i32>(clampedUv * dims);
  return textureLoad(tileTex, texel, 0);
}
)";

struct BlitParams {
  float rectMinX = 0, rectMinY = 0;
  float rectMaxX = 0, rectMaxY = 0;
  float targetW = 0, targetH = 0;
  float pad0 = 0, pad1 = 0;
};
static_assert(sizeof(BlitParams) == 32, "must match kBlitShaderSrc's BlitParams layout");

// Same technique as gfx/ShaderLoader.cpp's compileShader(), just from an
// in-memory string instead of a file on disk -- this shader is test-only
// scaffolding, not one of the solver's reloadable-from-disk passes, so it has
// no reason to live under shaders/ or go through NP_SHADER_DIR.
WGPUShaderModule compileBlitShader(GpuContext& gpu) {
  wgpuDevicePushErrorScope(gpu.device, WGPUErrorFilter_Validation);

  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(kBlitShaderSrc);

  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  desc.label = sv("tiled-viewport-selftest-blit");
  WGPUShaderModule mod = wgpuDeviceCreateShaderModule(gpu.device, &desc);

  struct ScopeResult { bool done = false; bool failed = false; } res;
  WGPUPopErrorScopeCallbackInfo pci = {};
  pci.mode = WGPUCallbackMode_AllowProcessEvents;
  pci.userdata1 = &res;
  pci.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message,
                    void* ud1, void*) {
    auto* r = static_cast<ScopeResult*>(ud1);
    if (type != WGPUErrorType_NoError) {
      r->failed = true;
      std::fprintf(stderr, "[selftest] tiled-viewport blit shader:\n%.*s\n", svLen(message),
                   message.data ? message.data : "");
    }
    r->done = true;
  };
  wgpuDevicePopErrorScope(gpu.device, pci);
  while (!res.done) wgpuInstanceProcessEvents(gpu.instance);

  if (res.failed || !mod) {
    if (mod) wgpuShaderModuleRelease(mod);
    return nullptr;
  }
  return mod;
}

// Same copy-to-buffer / map / decode technique as PaintSim::readbackField()
// (sim/PaintSim.cpp), generalized over an explicit width/height/texture
// instead of a PaintSim instance's own fields -- reusing readbackField()
// itself isn't practical here: it reads through `this->width_`/`height_`,
// a whole PaintSim's canvas dimensions (1024x1024 in this codebase's
// --selftest setup), not this test's small offscreen target, and
// constructing an unrelated full PaintSim (~193 MB, ADR-0001) just to borrow
// one method would make this test far heavier than the thing it's testing.
bool readbackRGBA16F(GpuContext& gpu, WGPUTexture tex, uint32_t width, uint32_t height,
                     std::vector<float>& out) {
  const uint32_t bytesPerRow = width * 8;  // RGBA16Float = 8 bytes/texel
  if (bytesPerRow % 256 != 0) return false;
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = tex;
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

  struct MapState { bool done = false; bool ok = false; } state;
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
  if (state.ok) {
    const void* raw = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (raw) {
      const size_t n = static_cast<size_t>(width) * height * 4;
      out.resize(n);
      const auto* h = static_cast<const uint16_t*>(raw);
      for (size_t i = 0; i < n; ++i) out[i] = halfToFloat(h[i]);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

// 3-D analog of readbackRGBA16F() above, for color/LutBake's kLutSize^3
// rgba16float LUT textures (PLAN.md Phase 3 step 4). Same copy-to-buffer /
// map / decode technique, generalized to a WGPUExtent3D copy region instead
// of a 2-D one.
//
// Copies the whole volume in a single wgpuCommandEncoderCopyTextureToBuffer
// call rather than size separate 2-D per-layer copies: a 3-D texture copy's
// `copySize.depthOrArrayLayers` is the z-extent and `layout.rowsPerImage` is
// how many rows (here, texels in Y) separate consecutive z-slices in the
// destination buffer, so setting rowsPerImage to the texture's own height
// (not padded) is sufficient for one call to cover every slice contiguously
// -- there is no per-layer wgpu-native API to reach for here, only the one
// already used for 2-D. bytesPerRow's 256-byte alignment requirement is
// satisfied with zero padding for this LUT's own size: size (32) texels x 8
// bytes/texel (rgba16float) = 256 bytes/row exactly, the same convenient
// coincidence readbackRGBA16F()'s own 2-D readback callers already lean on
// at typical field widths.
bool readbackRGBA16F3D(GpuContext& gpu, WGPUTexture tex, uint32_t size, std::vector<float>& out) {
  const uint32_t bytesPerRow = size * 8;  // RGBA16Float = 8 bytes/texel
  if (bytesPerRow % 256 != 0) return false;
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * size * size;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = tex;
  srcTex.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = size;
  WGPUExtent3D extent = {size, size, size};

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcTex, &dstBuf, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  struct MapState { bool done = false; bool ok = false; } state;
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
  if (state.ok) {
    const void* raw = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (raw) {
      const size_t n = static_cast<size_t>(size) * size * size * 4;
      out.resize(n);
      const auto* h = static_cast<const uint16_t*>(raw);
      for (size_t i = 0; i < n; ++i) out[i] = halfToFloat(h[i]);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

// Shared by runTiledViewportTest() and runMipPyramidTest() (PLAN.md step 9):
// given an already-built blit `pipeline` (caller owns/releases it --
// building it is the one step each call site still does itself, so each
// keeps its own "shader compiles"/"pipeline builds" check() lines), draws
// `tileView` at `rect` into a fresh `targetSize` x `targetSize` RGBA16Float
// offscreen target -- the exact placement TiledDocumentView::draw() would
// use for that tile -- and reads the result back to `outPixels` via
// readbackRGBA16F(). Everything this function itself creates (the uniform
// buffer, bind group, and render target) is released before returning;
// `pipeline` and `tileView` are the caller's.
bool blitPipelineRenderAndReadback(GpuContext& gpu, WGPURenderPipeline pipeline,
                                   WGPUTextureView tileView, TileScreenRect rect,
                                   uint32_t targetSize, std::vector<float>& outPixels) {
  WGPUTextureDescriptor rtd = {};
  rtd.label = sv("blit-selftest-target");
  rtd.dimension = WGPUTextureDimension_2D;
  rtd.size = {targetSize, targetSize, 1};
  rtd.format = WGPUTextureFormat_RGBA16Float;
  rtd.mipLevelCount = 1;
  rtd.sampleCount = 1;
  rtd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  WGPUTexture targetTex = wgpuDeviceCreateTexture(gpu.device, &rtd);
  WGPUTextureView targetView = wgpuTextureCreateView(targetTex, nullptr);

  BlitParams bp;
  bp.rectMinX = rect.min.x;
  bp.rectMinY = rect.min.y;
  bp.rectMaxX = rect.max.x;
  bp.rectMaxY = rect.max.y;
  bp.targetW = static_cast<float>(targetSize);
  bp.targetH = static_cast<float>(targetSize);

  WGPUBufferDescriptor ubd = {};
  ubd.size = sizeof(bp);
  ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  WGPUBuffer ubuf = wgpuDeviceCreateBuffer(gpu.device, &ubd);
  wgpuQueueWriteBuffer(gpu.queue, ubuf, 0, &bp, sizeof(bp));

  WGPUBindGroupEntry entries[2] = {};
  entries[0].binding = 0;
  entries[0].buffer = ubuf;
  entries[0].offset = 0;
  entries[0].size = sizeof(bp);
  entries[1].binding = 1;
  entries[1].textureView = tileView;

  WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = bgl;
  bgd.entryCount = 2;
  bgd.entries = entries;
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPURenderPassColorAttachment att = {};
  att.view = targetView;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 0.0};

  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  const bool readOk = readbackRGBA16F(gpu, targetTex, targetSize, targetSize, outPixels);

  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(bgl);
  wgpuBufferDestroy(ubuf);
  wgpuBufferRelease(ubuf);
  wgpuTextureViewRelease(targetView);
  wgpuTextureDestroy(targetTex);
  wgpuTextureRelease(targetTex);

  return readOk;
}

}  // namespace

bool runTiledViewportTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol = 0.02f;

  // --- a Document with no RGB layer at all, and a createBlank()'d one (RGB
  // layer present, zero occupied tiles) both upload nothing, and draw()
  // no-ops safely -- even given a null ImDrawList, the one call in this
  // module that needs a live ImGui context, which this headless test
  // deliberately never constructs (see SelfTest.hpp's comment on this
  // function) -------------------------------------------------------------
  {
    Document noLayers;
    noLayers.width = 64;
    noLayers.height = 64;
    TiledDocumentView tv;
    tv.setDocument(gpu, noLayers);
    check(tv.tileCount() == 0,
          "TiledDocumentView: a Document with no RGB layer uploads zero tiles");
    tv.draw(nullptr, CanvasView{}, ImVec2(0, 0));
    check(true, "TiledDocumentView: draw() doesn't crash with zero tiles / a null ImDrawList");
    tv.release();
  }
  {
    const Document blank = Document::createBlank(256, 256, WorkingSpace{});
    TiledDocumentView tv;
    tv.setDocument(gpu, blank);
    check(tv.tileCount() == 0,
          "TiledDocumentView: a freshly createBlank()'d Document uploads zero tiles "
          "(RGB layer present, but nothing painted into it yet)");
    tv.release();
  }

  // --- tileScreenRect(): pure geometry, checked against a hand-computed
  // expectation independent of anything GPU-side --------------------------
  CanvasView view;
  view.zoom = 2.0f;
  view.panX = 5.0f;
  view.panY = -3.0f;
  const ImVec2 canvasOrigin(10.0f, 20.0f);
  const TileScreenRect rect = tileScreenRect(TileCoord{0, 0}, view, canvasOrigin);
  // screenPos = canvasOrigin + tileOrigin({0,0})*zoom + (panX,panY)
  //           = (10,20) + (0,0)*2 + (5,-3) = (15,17); size = 128*2 = 256.
  check(near(rect.min.x, 15.0f, 1e-3f) && near(rect.min.y, 17.0f, 1e-3f) &&
            near(rect.max.x, 271.0f, 1e-3f) && near(rect.max.y, 273.0f, 1e-3f),
        "tileScreenRect: matches canvasOrigin + tileOrigin*zoom + pan, tile size kTileSize*zoom");

  // --- end to end: a known-pixel Document -> uploaded tile -> a dedicated
  // offscreen WebGPU render pass places it at tileScreenRect()'s own rect ->
  // read back and check known corners land at the expected screen pixel and
  // colour (PLAN.md step 8's actual verify criterion) ---------------------
  {
    // 2x2 fixture, opaque corners (alpha=255) so premultiplied == straight --
    // io/ImageIO's premultiply behaviour is already covered by
    // runImageIOTest(); this fixture is about position and upload, not
    // premultiply, so keeping alpha out of the arithmetic keeps the expected
    // colours exact (0.0/1.0, not a fraction).
    const uint8_t px[2 * 2 * 4] = {
        255, 0,   0,   255,  0,   255, 0,   255,
        0,   0,   255, 255,  255, 255, 255, 255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "runTiledViewportTest: 2x2 fixture PNG decodes");
    if (!docOpt) {
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    TiledDocumentView tv;
    tv.setDocument(gpu, *docOpt);
    check(tv.tileCount() == 1, "TiledDocumentView: the 2x2 fixture uploads exactly one tile");
    const auto it = tv.tiles().find(TileCoord{0, 0});
    check(it != tv.tiles().end(), "TiledDocumentView: the uploaded tile is at TileCoord{0,0}");
    if (it == tv.tiles().end()) {
      tv.release();
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    WGPUShaderModule shaderMod = compileBlitShader(gpu);
    check(shaderMod != nullptr, "runTiledViewportTest: blit shader compiles");
    if (!shaderMod) {
      tv.release();
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    WGPUColorTargetState target = {};
    target.format = WGPUTextureFormat_RGBA16Float;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fs = {};
    fs.module = shaderMod;
    fs.entryPoint = sv("fs");
    fs.targetCount = 1;
    fs.targets = &target;

    WGPURenderPipelineDescriptor rd = {};
    rd.label = sv("tiled-viewport-selftest-blit");
    rd.vertex.module = shaderMod;
    rd.vertex.entryPoint = sv("vs");
    rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rd.primitive.frontFace = WGPUFrontFace_CCW;
    rd.primitive.cullMode = WGPUCullMode_None;
    rd.multisample.count = 1;
    rd.multisample.mask = 0xFFFFFFFF;
    rd.fragment = &fs;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
    wgpuShaderModuleRelease(shaderMod);
    check(pipeline != nullptr, "runTiledViewportTest: blit render pipeline builds");

    if (pipeline) {
      constexpr uint32_t kTargetSize = 64;  // comfortably covers the corner this test samples
      std::vector<float> pixels;
      // it->second.view is the tile's all-levels view; the blit shader
      // reads its level 0 explicitly (kBlitShaderSrc's `textureLoad(...,
      // 0)`), so this exercises the exact same level-0 pixels step 8's
      // test always has, just via the shared helper both this test and
      // runMipPyramidTest() below now use.
      const bool readOk =
          blitPipelineRenderAndReadback(gpu, pipeline, it->second.view, rect, kTargetSize, pixels);
      check(readOk, "runTiledViewportTest: offscreen render target reads back");

      if (readOk) {
        auto sample = [&](uint32_t x, uint32_t y) {
          const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
          return std::array<float, 4>{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        };
        auto pixNear = [&](std::array<float, 4> a, std::array<float, 4> b, const char* what) {
          check(near(a[0], b[0], kTol) && near(a[1], b[1], kTol) && near(a[2], b[2], kTol) &&
                    near(a[3], b[3], kTol),
                what);
        };

        // rect.min = (15,17), zoom = 2 -> tile texel (tx,ty) covers screen
        // [15+tx*2, 15+tx*2+2) x [17+ty*2, 17+ty*2+2); sampling the first
        // pixel of each block is unambiguous since 15/17 are exact integers.
        pixNear(sample(15, 17), {1.0f, 0.0f, 0.0f, 1.0f},
               "runTiledViewportTest: fixture's top-left red pixel lands at the tile's screen "
               "origin (rect.min)");
        pixNear(sample(17, 17), {0.0f, 1.0f, 0.0f, 1.0f},
               "runTiledViewportTest: fixture's top-right green pixel lands one zoomed texel "
               "to the right");
        pixNear(sample(15, 19), {0.0f, 0.0f, 1.0f, 1.0f},
               "runTiledViewportTest: fixture's bottom-left blue pixel lands one zoomed texel "
               "down");
        pixNear(sample(17, 19), {1.0f, 1.0f, 1.0f, 1.0f},
               "runTiledViewportTest: fixture's bottom-right white pixel lands diagonally "
               "opposite the origin");
        // Untouched tile interior (only the 2x2 fixture corner was ever
        // written; core::Tile value-initializes the rest to zero) stays
        // transparent black, not garbage.
        pixNear(sample(25, 27), {0.0f, 0.0f, 0.0f, 0.0f},
               "runTiledViewportTest: unpainted tile interior reads back transparent black");
        // Outside the tile's own screen quad entirely (rect.min is (15,17);
        // this is well above/left of it) -- proves the draw is actually
        // bounded to the computed rect, not filling the whole target.
        pixNear(sample(2, 2), {0.0f, 0.0f, 0.0f, 0.0f},
               "runTiledViewportTest: area outside the tile's screen quad stays untouched "
               "(clear colour)");
      }

      wgpuRenderPipelineRelease(pipeline);
    }

    tv.release();
  }

  std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 2 step 9 ("Mip pyramid for tiles, so a 25% zoom evaluates at
// a matching level"). See SelfTest.hpp for the full breakdown; in short:
// CPU-only downsample correctness for buildMipChain() (no GPU), CPU-only
// level-selection formula checks for mipLevelForZoom() (no GPU), and an
// end-to-end GPU proof that draw()'s own level pick actually changes which
// texels land on screen -- extending runTiledViewportTest()'s own offscreen
// blit-and-readback technique (immediately above) rather than duplicating
// it from scratch.
bool runMipPyramidTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol = 0.02f;

  // --- CPU-side downsample correctness: no GPU needed. A hand-built Tile
  // with a known, distinct 4x4 ramp in its R channel (v(x,y) = x + 4*y; G,
  // B, A left at 0/0/1) lets mip level 1's four corner texels, and mip
  // level 2's single corner texel, be hand-computed independently of
  // buildMipChain() itself -- checking both the 128->64 step and the
  // 64->32 step, i.e. the recursive "downsample the downsample" step, not
  // just level 0->1 -----------------------------------------------------
  {
    Tile tile;  // value-initialized to transparent black everywhere else
    for (int32_t y = 0; y < 4; ++y) {
      for (int32_t x = 0; x < 4; ++x) {
        const float v = static_cast<float>(x + 4 * y);
        tile.writePixel(PixelCoord{x, y}, {v, 0.0f, 0.0f, 1.0f});
      }
    }

    const std::vector<MipLevel> mips = buildMipChain(tile);
    check(mips.size() == static_cast<size_t>(kMipLevelCount),
          "buildMipChain: produces the full 128..1 chain (8 levels)");

    if (mips.size() >= 3) {
      check(mips[0].size == 128 && mips[1].size == 64 && mips[2].size == 32,
            "buildMipChain: level sizes halve each step (128, 64, 32, ...)");

      auto texelR = [&](const MipLevel& lvl, int32_t x, int32_t y) {
        return lvl.texels[(static_cast<size_t>(y) * static_cast<size_t>(lvl.size) +
                           static_cast<size_t>(x)) *
                          4];
      };
      // Hand-computed level-1 corner values -- each is the average of the
      // corresponding 2x2 block of level 0's ramp; all four blocks sit
      // entirely inside the written 4x4 region, so texels outside it (left
      // at 0 by Tile's default construction) don't influence these.
      //   (0,0): {0,1,4,5}/4 = 2.5   (1,0): {2,3,6,7}/4 = 4.5
      //   (0,1): {8,9,12,13}/4 = 10.5  (1,1): {10,11,14,15}/4 = 12.5
      check(near(texelR(mips[1], 0, 0), 2.5f, 1e-4f) && near(texelR(mips[1], 1, 0), 4.5f, 1e-4f) &&
                near(texelR(mips[1], 0, 1), 10.5f, 1e-4f) &&
                near(texelR(mips[1], 1, 1), 12.5f, 1e-4f),
            "buildMipChain: level 1's four corner texels equal the hand-computed 2x2 box-filter "
            "average of level 0's ramp");
      // Level 2's corner texel recurses on level 1's own four values above
      // (2.5, 4.5, 10.5, 12.5), not on level 0 directly -- {2.5+4.5+10.5+
      // 12.5}/4 = 7.5. This is the check that actually exercises the
      // recursive step: a chain that only handles level 0->1 correctly and
      // silently no-ops (or copies) every level after would fail this.
      check(near(texelR(mips[2], 0, 0), 7.5f, 1e-4f),
            "buildMipChain: level 2's corner texel equals the hand-computed average of level "
            "1's own values -- proves the recursive downsample-the-downsample step, not just "
            "level 0->1");
    }
  }

  // --- level selection: mipLevelForZoom() is pure math, no GPU needed.
  // PLAN.md's own literal example (zoom=0.25 -> the 32px level, mip 2) plus
  // zoom=1.0 -> level 0, and clamping at both extremes ------------------
  check(mipLevelForZoom(1.0f) == 0, "mipLevelForZoom: 100% zoom selects level 0 (full 128px res)");
  check(mipLevelForZoom(0.25f) == 2,
        "mipLevelForZoom: 25% zoom selects level 2 (128 -> 64 -> 32px) -- PLAN.md's own example");
  check(mipLevelForZoom(0.5f) == 1, "mipLevelForZoom: 50% zoom selects level 1 (64px)");
  check(mipLevelForZoom(2.0f) == 0, "mipLevelForZoom: zooming in past 100% still clamps at level 0");
  check(mipLevelForZoom(1000.0f) == 0,
        "mipLevelForZoom: extreme zoom-in clamps at level 0, not a negative level");
  check(mipLevelForZoom(0.001f) == kMipLevelCount - 1,
        "mipLevelForZoom: extreme zoom-out clamps at the smallest level rather than going out "
        "of range");

  // --- end-to-end: a known, non-uniform (checkerboard) tile -> uploaded
  // mip chain -> the level draw() would pick for a given zoom -> an
  // offscreen render placed at tileScreenRect()'s own rect -> read back and
  // confirm the pixels match the *downsampled* level's known value, not
  // level 0's -- the check that actually proves level selection is wired
  // into the real GPU draw path, not just computed and ignored -----------
  {
    Document doc = Document::createBlank(kTileSize, kTileSize, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    // Finest-period checkerboard, opaque red/green. Any 2x2 box-filter
    // average of it is *exactly* uniform (0.5, 0.5, 0, 1) -- true of every
    // block, so mip level 1 and every level after it reads back that one
    // uniform colour everywhere, cleanly distinguishable from level 0's
    // alternating pure red/green at the same texel.
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const bool evenParity = ((x + y) % 2) == 0;
        tile.writePixel(PixelCoord{x, y}, evenParity
                                              ? std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f}
                                              : std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f});
      }
    }

    TiledDocumentView tv;
    tv.setDocument(gpu, doc);
    check(tv.tileCount() == 1, "runMipPyramidTest: checkerboard fixture uploads exactly one tile");
    const auto it = tv.tiles().find(TileCoord{0, 0});
    check(it != tv.tiles().end(), "runMipPyramidTest: the uploaded tile is at TileCoord{0,0}");

    if (it != tv.tiles().end()) {
      check(it->second.levelViews.size() == static_cast<size_t>(kMipLevelCount),
            "runMipPyramidTest: the uploaded tile carries one per-level view for each of the 8 "
            "mip levels");

      // PLAN.md's own literal example: 25% zoom -> the 32px level.
      constexpr float kZoom = 0.25f;
      const int level = mipLevelForZoom(kZoom);
      check(level == 2,
            "runMipPyramidTest: zoom=0.25 selects level 2 -- exactly the level draw() itself "
            "would compute for this zoom");

      if (level >= 0 && static_cast<size_t>(level) < it->second.levelViews.size()) {
        CanvasView view;
        view.zoom = kZoom;
        const ImVec2 canvasOrigin(10.0f, 20.0f);
        const TileScreenRect rect = tileScreenRect(TileCoord{0, 0}, view, canvasOrigin);

        WGPUShaderModule shaderMod = compileBlitShader(gpu);
        check(shaderMod != nullptr, "runMipPyramidTest: blit shader compiles");

        if (shaderMod) {
          WGPUColorTargetState target = {};
          target.format = WGPUTextureFormat_RGBA16Float;
          target.writeMask = WGPUColorWriteMask_All;

          WGPUFragmentState fs = {};
          fs.module = shaderMod;
          fs.entryPoint = sv("fs");
          fs.targetCount = 1;
          fs.targets = &target;

          WGPURenderPipelineDescriptor rd = {};
          rd.label = sv("mip-pyramid-selftest-blit");
          rd.vertex.module = shaderMod;
          rd.vertex.entryPoint = sv("vs");
          rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
          rd.primitive.frontFace = WGPUFrontFace_CCW;
          rd.primitive.cullMode = WGPUCullMode_None;
          rd.multisample.count = 1;
          rd.multisample.mask = 0xFFFFFFFF;
          rd.fragment = &fs;
          WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
          wgpuShaderModuleRelease(shaderMod);
          check(pipeline != nullptr, "runMipPyramidTest: blit render pipeline builds");

          if (pipeline) {
            constexpr uint32_t kTargetSize = 64;  // comfortably covers rect (32x32 at zoom=0.25)
            auto sampleAt = [&](const std::vector<float>& px, uint32_t x, uint32_t y) {
              const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
              return std::array<float, 4>{px[i], px[i + 1], px[i + 2], px[i + 3]};
            };
            auto pixNear = [&](std::array<float, 4> a, std::array<float, 4> b, const char* what) {
              check(near(a[0], b[0], kTol) && near(a[1], b[1], kTol) && near(a[2], b[2], kTol) &&
                        near(a[3], b[3], kTol),
                    what);
            };
            const uint32_t sx = static_cast<uint32_t>(rect.min.x) + 2;
            const uint32_t sy = static_cast<uint32_t>(rect.min.y) + 2;

            std::vector<float> levelPixels;
            const bool levelReadOk = blitPipelineRenderAndReadback(
                gpu, pipeline, it->second.levelViews[static_cast<size_t>(level)], rect,
                kTargetSize, levelPixels);
            check(levelReadOk, "runMipPyramidTest: offscreen render of the level-2 view reads "
                               "back");
            if (levelReadOk) {
              pixNear(sampleAt(levelPixels, sx, sy), {0.5f, 0.5f, 0.0f, 1.0f},
                     "runMipPyramidTest: rendered pixels match level 2's known downsampled "
                     "colour (uniform 0.5/0.5/0/1), proving level selection reached the real "
                     "GPU draw path");
            }

            // Contrast check: the exact same screen rect, rendered from
            // level 0's own single-level view instead, reads back a pure
            // checkerboard colour at this texel -- never the level-2 grey
            // -- so the level-2 result above is not a value level 0 could
            // have produced by coincidence. textureLoad is a point sample
            // (no bilinear filtering), so whichever texel the pixel-centre
            // arithmetic actually lands on, the value it reads is always
            // *exactly* pure red or pure green, never a blend -- checked as
            // "one of the two", not a specific one, so this doesn't depend
            // on hand-tracking sub-pixel/texel-index arithmetic through the
            // vertex shader's interpolation.
            std::vector<float> level0Pixels;
            const bool level0ReadOk = blitPipelineRenderAndReadback(
                gpu, pipeline, it->second.levelViews[0], rect, kTargetSize, level0Pixels);
            check(level0ReadOk,
                  "runMipPyramidTest: offscreen render of level 0's own view reads back "
                  "(contrast check)");
            if (level0ReadOk) {
              const std::array<float, 4> px = sampleAt(level0Pixels, sx, sy);
              const bool isPureCheckerColor =
                  (near(px[0], 1.0f, kTol) && near(px[1], 0.0f, kTol)) ||
                  (near(px[0], 0.0f, kTol) && near(px[1], 1.0f, kTol));
              check(isPureCheckerColor && near(px[2], 0.0f, kTol) && near(px[3], 1.0f, kTol),
                    "runMipPyramidTest: the same screen rect rendered from level 0 instead "
                    "reads a pure checkerboard colour (red or green), not level 2's grey -- "
                    "the level-2 result above genuinely differs from level 0, not "
                    "coincidentally equal");
            }

            wgpuRenderPipelineRelease(pipeline);
          }
        }
      }
    }

    tv.release();
  }

  std::printf("[selftest] mip pyramid %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runProbeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // Byte-quantization tolerance, same magnitude as runImageIOTest()'s kTol8 --
  // every value checked here passed through an 8-bit-per-channel PNG fixture.
  constexpr float kTol8 = 0.01f;

  // --- fixture: a 3x3, fully-opaque PNG with distinct, known bytes per
  // pixel, laid out row-major (row0 = y=0, etc.) so both a point sample and
  // an NxN box average have a hand-computable expectation. Opaque (alpha =
  // 255) deliberately, so premultiplied == straight here -- this fixture is
  // about sample-size averaging, not premultiply; that gets its own,
  // separately alpha < 1 fixture below (runImageIOTest()'s own precedent:
  // keep premultiply out of the arithmetic when a test isn't about it) -----
  const std::optional<Document> gridOpt = [] {
    const uint8_t px[3 * 3 * 4] = {
        10,  20,  30,  255,  40,  50,  60,  255,  70,  80,  90,  255,
        100, 110, 120, 255,  130, 140, 150, 255,  160, 170, 180, 255,
        190, 200, 210, 255,  220, 230, 240, 255,  250, 5,   15,  255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 3, 3, 4, px, 3 * 4);
    return openImageAsDocument(png.data(), png.size());
  }();
  check(gridOpt.has_value(), "runProbeTest: 3x3 grid fixture PNG decodes");
  if (!gridOpt) {
    std::printf("[selftest] pixel probe %s\n", ok ? "PASS" : "FAIL");
    return ok;
  }
  const Document& grid = *gridOpt;

  // srgbDecode() of a known byte, exactly the way this fixture's expected
  // values are derived everywhere below -- same technique runImageIOTest()
  // already uses for its own expected values, rather than pre-baking
  // decoded floats by hand.
  auto dec = [](uint8_t byte) { return srgbDecode(byte / 255.0f); };

  // --- point sample (sampleSize=1): returns the exact stored pixel, both
  // linear and display-encoded -------------------------------------------
  {
    ProbeParams p;
    p.sampleSize = 1;
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, p);
    const float rLin = dec(130), gLin = dec(140), bLin = dec(150);
    check(near(s.linear[0], rLin, kTol8) && near(s.linear[1], gLin, kTol8) &&
              near(s.linear[2], bLin, kTol8) && near(s.linear[3], 1.0f, kTol8),
          "probePixel: point sample returns the exact known linear pixel value");
    check(near(s.display[0], srgbEncode(rLin), kTol8) &&
              near(s.display[1], srgbEncode(gLin), kTol8) &&
              near(s.display[2], srgbEncode(bLin), kTol8) && near(s.display[3], 1.0f, kTol8),
          "probePixel: point sample's display value is srgbEncode() of its linear value");
    // The fixture's own byte was itself sRGB-encoded, so encode(decode(x))
    // should land back near the original normalized byte -- a second,
    // independent check that display isn't accidentally returning the
    // linear value unencoded (encode and decode are different curves away
    // from 0, so a bug here wouldn't pass the check above by accident).
    check(near(s.display[0], 130 / 255.0f, kTol8) && near(s.display[1], 140 / 255.0f, kTol8) &&
              near(s.display[2], 150 / 255.0f, kTol8),
          "probePixel: point sample's display value round-trips back to the source byte");
  }

  // --- NxN box average, fully-painted interior: sampleSize=3 centred on
  // the fixture's own centre pixel covers exactly its 9 texels, all
  // painted -- isolates averaging correctness from any edge/bounds
  // behaviour (that's the next block) ---------------------------------
  {
    ProbeParams p;
    p.sampleSize = 3;
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, p);
    const uint8_t rBytes[9] = {10, 40, 70, 100, 130, 160, 190, 220, 250};
    const uint8_t gBytes[9] = {20, 50, 80, 110, 140, 170, 200, 230, 5};
    const uint8_t bBytes[9] = {30, 60, 90, 120, 150, 180, 210, 240, 15};
    float rSum = 0, gSum = 0, bSum = 0;
    for (int i = 0; i < 9; ++i) {
      rSum += dec(rBytes[i]);
      gSum += dec(gBytes[i]);
      bSum += dec(bBytes[i]);
    }
    const float rAvg = rSum / 9.0f, gAvg = gSum / 9.0f, bAvg = bSum / 9.0f;
    check(near(s.linear[0], rAvg, kTol8) && near(s.linear[1], gAvg, kTol8) &&
              near(s.linear[2], bAvg, kTol8) && near(s.linear[3], 1.0f, kTol8),
          "probePixel: 3x3 box average over a fully-painted fixture matches the hand-computed "
          "per-channel mean, not a single sample or an edge-clamped value");
    // Genuinely distinct from the centre pixel's own point-sample value
    // (checked above) -- proves this is actually averaging the box, not
    // just re-reading the centre texel under a different sampleSize.
    check(!near(s.linear[0], dec(130), 0.02f),
          "probePixel: the 3x3 average genuinely differs from the centre pixel's own value");
  }

  // --- NxN box average straddling painted and never-painted texels:
  // sampleSize=3 centred on the fixture's top-left corner (0,0) covers x/y
  // in [-1,1] -- only 4 of the 9 texels ((0,0),(1,0),(0,1),(1,1)) were ever
  // painted, the rest fall in a tile that was never allocated. Averaging in
  // premultiplied space and un-premultiplying once at the end (Probe.cpp's
  // own documented reasoning) means the 5 missing texels dilute alpha
  // (4/9) without dragging the reported *colour* toward black -- so the
  // expected linear colour is exactly the straight average of the 4
  // painted texels, not a darker value and not an edge-repeated one -------
  {
    ProbeParams p;
    p.sampleSize = 3;
    const ProbeSample s = probePixel(grid, PixelCoord{0, 0}, p);
    const float rAvg = (dec(10) + dec(40) + dec(100) + dec(130)) / 4.0f;
    const float gAvg = (dec(20) + dec(50) + dec(110) + dec(140)) / 4.0f;
    const float bAvg = (dec(30) + dec(60) + dec(120) + dec(150)) / 4.0f;
    check(near(s.linear[0], rAvg, kTol8) && near(s.linear[1], gAvg, kTol8) &&
              near(s.linear[2], bAvg, kTol8),
          "probePixel: a box straddling unpainted texels keeps the un-premultiplied colour at "
          "the painted texels' own average, not darkened toward black");
    check(near(s.linear[3], 4.0f / 9.0f, kTol8),
          "probePixel: that same box's alpha reflects exactly how much of it was actually "
          "painted (4 of 9 texels), proving missing texels dilute coverage rather than being "
          "skipped or edge-clamped to a painted neighbour");
  }

  // --- translucent pixel: proves un-premultiplication actually ran, the
  // same "check against the raw stored value, not just a plausible number"
  // discipline runImageIOTest()'s own premultiply checks use -------------
  {
    const uint8_t px[1 * 1 * 4] = {60, 120, 180, 90};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 4, px, 4);
    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "runProbeTest: translucent 1x1 fixture decodes");
    if (docOpt && docOpt->layers.size() == 1 && docOpt->layers[0].rgbTiles) {
      const ProbeSample s = probePixel(*docOpt, PixelCoord{0, 0});
      const float a = 90 / 255.0f;
      const float rLin = dec(60), gLin = dec(120), bLin = dec(180);
      check(near(s.linear[0], rLin, kTol8) && near(s.linear[1], gLin, kTol8) &&
                near(s.linear[2], bLin, kTol8) && near(s.linear[3], a, kTol8),
            "probePixel: translucent pixel's reported linear colour is the straight (source) "
            "value, not the premultiplied one");

      const Tile* tile = docOpt->layers[0].rgbTiles->find(TileCoord{0, 0});
      check(tile != nullptr, "runProbeTest: translucent fixture's tile exists");
      if (tile) {
        const auto raw = tile->readPixel(PixelCoord{0, 0});
        check(near(raw[0], rLin * a, kTol8) && near(raw[2], bLin * a, kTol8),
              "runProbeTest: sanity check -- the tile itself really does store rgb*a "
              "premultiplied (same fact runImageIOTest() already covers)");
        // The actual "prove un-premultiply ran" assertion: the reported
        // straight colour must genuinely differ from the raw premultiplied
        // storage, not merely be plausible. This fixture's alpha (90/255 ~
        // 0.353) means straight = premultiplied / 0.353 ~ premultiplied *
        // 2.83, so every channel's true gap is well above float/quantization
        // noise -- red, the smallest, is still ~0.029 (srgbDecode(60/255) ~
        // 0.045 vs. its raw premultiplied ~0.016) -- but comfortably below
        // kTol8 (0.01)'s own quantization allowance would be too loose here,
        // so this uses a tighter 0.015 margin instead of runImageIOTest()'s
        // 0.05 (that test's fixture has brighter channels and a bigger gap).
        check(!near(s.linear[0], raw[0], 0.015f) && !near(s.linear[2], raw[2], 0.015f),
              "probePixel: reported colour genuinely differs from the raw premultiplied tile "
              "value -- proves un-premultiplication ran, not just alpha passthrough");
      }
    }
  }

  // --- sampleAllLayers vs. single/active-layer sampling: today's
  // core::Document only ever has at most one populated RGB layer (see
  // Probe.cpp / ProbeParams::sampleAllLayers's own doc comment for why), so
  // this cannot yet assert the two modes differ -- what IS testable today
  // is that the parameter is genuinely wired through and both modes agree,
  // rather than one of them being dead code -------------------------------
  {
    ProbeParams single;
    single.sampleSize = 3;
    single.sampleAllLayers = false;
    ProbeParams all;
    all.sampleSize = 3;
    all.sampleAllLayers = true;
    const ProbeSample sSingle = probePixel(grid, PixelCoord{1, 1}, single);
    const ProbeSample sAll = probePixel(grid, PixelCoord{1, 1}, all);
    check(near(sSingle.linear[0], sAll.linear[0], 1e-6f) &&
              near(sSingle.linear[1], sAll.linear[1], 1e-6f) &&
              near(sSingle.linear[2], sAll.linear[2], 1e-6f) &&
              near(sSingle.linear[3], sAll.linear[3], 1e-6f),
          "probePixel: sampleAllLayers is wired through and agrees with single-layer sampling "
          "on today's at-most-one-RGB-layer Document (the two modes have no way to differ yet "
          "-- see ProbeParams::sampleAllLayers)");
  }

  // --- out-of-bounds / never-painted / misuse: all sane, documented,
  // never a crash or garbage read -----------------------------------------
  {
    const ProbeSample farAway = probePixel(grid, PixelCoord{10000, -10000});
    check(near(farAway.linear[0], 0.0f, 1e-6f) && near(farAway.linear[1], 0.0f, 1e-6f) &&
              near(farAway.linear[2], 0.0f, 1e-6f) && near(farAway.linear[3], 0.0f, 1e-6f) &&
              near(farAway.display[0], 0.0f, 1e-6f) && near(farAway.display[3], 0.0f, 1e-6f),
          "probePixel: a far-away, never-painted coordinate reads back fully transparent "
          "black, not a crash or garbage value");

    Document empty;
    const ProbeSample noLayers = probePixel(empty, PixelCoord{0, 0});
    check(near(noLayers.linear[3], 0.0f, 1e-6f),
          "probePixel: a Document with no layers at all is a safe no-op, not a crash");

    const Document blank = Document::createBlank(64, 64, WorkingSpace{});
    const ProbeSample noTiles = probePixel(blank, PixelCoord{5, 5});
    check(near(noTiles.linear[3], 0.0f, 1e-6f),
          "probePixel: a createBlank()'d Document (RGB layer present, zero tiles painted) "
          "reads back fully transparent black");

    ProbeParams badIndex;
    badIndex.activeLayerIndex = 5;  // grid only has one layer, index 0
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, badIndex);
    check(near(s.linear[3], 0.0f, 1e-6f),
          "probePixel: an out-of-range activeLayerIndex is a safe no-op, not a crash");

    ProbeParams zeroSize;
    zeroSize.sampleSize = 0;
    const ProbeSample s2 = probePixel(grid, PixelCoord{1, 1}, zeroSize);
    check(near(s2.linear[0], dec(130), kTol8) && near(s2.linear[3], 1.0f, kTol8),
          "probePixel: sampleSize <= 0 is clamped up to 1 (a point sample), not a crash or "
          "divide-by-zero");
  }

  std::printf("[selftest] pixel probe %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runViewTransformTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearVec = [&](Vec2 a, Vec2 b, float tol) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };

  // --- (a) round-trip identity: toCanvas(toScreen(p)) == p, across a spread
  // of zoom/pan/mirrorX/mirrorY/rotation combinations, including mirrors
  // and a non-zero rotation together -- the concrete proof that "pen input
  // maps back through its inverse" (docs/shortcuts.md section 3) actually
  // holds, for the general case, not just identity. ---------------------
  {
    struct Case {
      float zoom, panX, panY, rotation;
      bool mirrorX, mirrorY;
      const char* what;
    };
    const Case cases[] = {
        {1.0f, 0.0f, 0.0f, 0.0f, false, false, "identity"},
        {2.5f, 30.0f, -12.0f, 0.0f, false, false, "zoom+pan only"},
        {1.0f, 0.0f, 0.0f, 0.0f, true, false, "mirror X only"},
        {1.0f, 0.0f, 0.0f, 0.0f, false, true, "mirror Y only"},
        {1.0f, 0.0f, 0.0f, 0.0f, true, true, "both mirrors (180 degrees)"},
        {1.0f, 0.0f, 0.0f, 0.7f, false, false, "rotation only"},
        {3.0f, -40.0f, 18.0f, 1.9f, true, true,
         "zoom+pan+both mirrors+rotation together"},
        {0.35f, 100.0f, -75.0f, -2.4f, true, false,
         "small zoom, big pan, mirror X, negative rotation"},
    };
    const Vec2 canvasCenter{512.0f, 384.0f};
    const Vec2 pivotScreen{640.0f, 400.0f};
    const Vec2 samplePts[] = {{0.0f, 0.0f}, {1024.0f, 768.0f}, {512.0f, 384.0f},
                              {200.0f, 600.0f}, {900.0f, 50.0f}};

    for (const auto& c : cases) {
      CanvasView v;
      v.zoom = c.zoom;
      v.panX = c.panX;
      v.panY = c.panY;
      v.mirrorX = c.mirrorX;
      v.mirrorY = c.mirrorY;
      v.rotation = c.rotation;
      const ViewTransform xform(v, canvasCenter, pivotScreen);
      for (Vec2 p : samplePts) {
        const Vec2 s = xform.toScreen(p);
        const Vec2 back = xform.toCanvas(s);
        char label[192];
        std::snprintf(label, sizeof(label),
                      "runViewTransformTest: round-trip identity (%s) at canvas (%.0f,%.0f)",
                      c.what, p.x, p.y);
        check(nearVec(back, p, 1e-2f), label);
      }
    }
  }

  // --- (b) hand-computed known point: zoom=2, mirrorX=true, rotation=90
  // degrees, canvasCenter=(100,50), pivotScreen=(300,200), canvas point
  // p=(150,50).
  //
  //   rel = p - canvasCenter = (50, 0)
  //   mirror X (mx=-1, my=1): (-50, 0)
  //   rotate 90 degrees (cosT=0, sinT=1):
  //     x' = x*cosT - y*sinT = -50*0 - 0*1 = 0
  //     y' = x*sinT + y*cosT = -50*1 + 0*0 = -50
  //   scale by zoom=2: (0, -100)
  //   + pivotScreen (300, 200) => (300, 100)
  //
  // -- checked against the transform's actual output, not just re-asserted
  // as "whatever toScreen returns", and then round-tripped back through
  // toCanvas() to confirm it lands on the same (300, 100) -> (150, 50). ---
  {
    CanvasView v;
    v.zoom = 2.0f;
    v.mirrorX = true;
    v.mirrorY = false;
    v.rotation = 1.5707963267948966f;  // 90 degrees, avoiding an M_PI dependency
    const Vec2 canvasCenter{100.0f, 50.0f};
    const Vec2 pivotScreen{300.0f, 200.0f};
    const ViewTransform xform(v, canvasCenter, pivotScreen);

    const Vec2 p{150.0f, 50.0f};
    const Vec2 s = xform.toScreen(p);
    check(nearVec(s, Vec2{300.0f, 100.0f}, 1e-2f),
          "runViewTransformTest: hand-computed toScreen (zoom=2, mirrorX, 90-degree rotation) "
          "lands exactly where hand-worked algebra predicts");
    const Vec2 back = xform.toCanvas(s);
    check(nearVec(back, p, 1e-2f),
          "runViewTransformTest: that same hand-computed screen point round-trips back to the "
          "original canvas point through toCanvas()");
  }

  // --- (c) view-only: toggling mirrorX/mirrorY/rotation/grayscale/grade
  // never mutates PaintSim's own canvas texture -- the available headless
  // proxy for PLAN.md's "mirror both axes, save, reopen -- the file is
  // unmirrored" (no save path exists yet in this codebase to assert that
  // literally, so this checks the thing that could actually regress it: does
  // flipping view state write into the document/canvas at all).
  //
  // mirrorX/mirrorY/rotation have no code path into PaintSim whatsoever --
  // ui/MacPaintUI.cpp only ever reads them to place screen-space quad
  // corners (ViewTransform::toScreen()) and to invert a mouse position
  // (ViewTransform::toCanvas()), never to call anything on a PaintSim. The
  // two view flags that *do* reach into PaintSim are grayscale (via
  // updateGrayscalePreview()) and grade (PLAN.md Phase 3 step 6, via
  // updateGradePreview()) -- so those are what this actually exercises
  // against a live sim, each twice (to catch a pass that only corrupts
  // canvas_ on a repeat), while readbackCanvas() -- the same technique
  // runFieldAllocationTest() and runSelfTest() already hold PaintSim to --
  // confirms canvas_ itself never moved. updateGradePreview() is exercised
  // with an empty (identity-bake) OpStack here -- this block's job is only
  // "does running the grade preview ever touch canvas_", not grading
  // correctness, which runApplyPassTest() (Phase 3 step 6's own dedicated
  // case, immediately below in the --selftest chain) already covers in
  // depth. ----------------------------------
  {
    std::vector<uint8_t> before, after;
    const bool readBefore = sim.readbackCanvas(gpu, before);
    check(readBefore, "runViewTransformTest: canvas readback before toggling view state");

    CanvasView view;  // the real AppState type -- not a stand-in struct
    view.mirrorX = true;
    view.mirrorY = true;
    view.rotation = 1.234f;
    view.grayscale = true;
    view.grade = true;
    (void)view;  // exercised for its shape only; nothing reads it further --
                 // mirror/rotation have no PaintSim call to make in the
                 // first place, per the comment above.
    sim.updateGrayscalePreview(gpu);
    sim.updateGrayscalePreview(gpu);
    const OpStack identityOps;  // empty -- detectRuns() has nothing to bake, a valid seed-only LUT
    sim.updateGradePreview(gpu, identityOps);
    sim.updateGradePreview(gpu, identityOps);

    const bool readAfter = sim.readbackCanvas(gpu, after);
    check(readAfter, "runViewTransformTest: canvas readback after toggling view state");
    check(readBefore && readAfter && before == after,
          "runViewTransformTest: mirror/rotation/grayscale/grade view state leaves PaintSim's "
          "own canvas byte-identical (proxy for PLAN.md's \"save with a mirror on -> file is "
          "unmirrored\" -- no save path exists yet to test that literally)");
  }

  std::printf("[selftest] view transform %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// See SelfTest.hpp for the full scope note on why this covers app/Snapping.hpp's
// pure math and nothing UI-shaped.
bool runGuidesGridSnapTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto vecNear = [&](const std::vector<float>& a, const std::vector<float>& b, float tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!nearf(a[i], b[i], tol)) return false;
    return true;
  };

  // --- gridLinePositions() / isMajorGridLine(): hand-computable cases ---
  {
    const auto lines = gridLinePositions(100.0f, 4, 0.0f, 100.0f);
    check(vecNear(lines, {0.0f, 25.0f, 50.0f, 75.0f, 100.0f}, 1e-3f),
          "gridLinePositions: spacing=100/subdivisions=4 over [0,100] is exactly "
          "{0,25,50,75,100}");

    const auto majorsOnly = gridLinePositions(100.0f, 1, 0.0f, 250.0f);
    check(vecNear(majorsOnly, {0.0f, 100.0f, 200.0f}, 1e-3f),
          "gridLinePositions: subdivisions=1 returns major lines only, {0,100,200}");

    // The grid always anchors to document-space 0, not the queried range's
    // own start -- a range that doesn't begin at 0 still lands on the same
    // 0-based lattice.
    const auto offsetRange = gridLinePositions(50.0f, 2, 60.0f, 140.0f);
    check(vecNear(offsetRange, {75.0f, 100.0f, 125.0f}, 1e-3f),
          "gridLinePositions: [60,140] at spacing=50/subdivisions=2 (minor=25) is "
          "{75,100,125}, anchored at 0 rather than the range start");

    check(gridLinePositions(0.0f, 4, 0.0f, 100.0f).empty(),
          "gridLinePositions: non-positive spacing returns nothing");
    check(gridLinePositions(100.0f, 0, 0.0f, 100.0f).empty(),
          "gridLinePositions: non-positive subdivisions returns nothing");

    check(isMajorGridLine(200.0f, 100.0f) && isMajorGridLine(0.0f, 100.0f) &&
              !isMajorGridLine(225.0f, 100.0f),
          "isMajorGridLine: multiples of spacing are major, others are not");
  }

  // --- parseGuidePosition(): plain numbers, percentages, whitespace, junk ---
  {
    const auto px = parseGuidePosition("512", 1024.0f);
    check(px.has_value() && nearf(*px, 512.0f, 1e-3f), "parseGuidePosition: \"512\" -> 512 px");

    const auto pct = parseGuidePosition("50%", 1024.0f);
    check(pct.has_value() && nearf(*pct, 512.0f, 1e-3f),
          "parseGuidePosition: \"50%%\" of a 1024 px axis -> 512");

    const auto pctSmall = parseGuidePosition("25%", 800.0f);
    check(pctSmall.has_value() && nearf(*pctSmall, 200.0f, 1e-3f),
          "parseGuidePosition: \"25%%\" of an 800 px axis -> 200");

    const auto withSpace = parseGuidePosition("  128  ", 1024.0f);
    check(withSpace.has_value() && nearf(*withSpace, 128.0f, 1e-3f),
          "parseGuidePosition: surrounding whitespace is trimmed");

    check(!parseGuidePosition("nope", 1024.0f).has_value(),
          "parseGuidePosition: unparseable text returns nothing");
    check(!parseGuidePosition("", 1024.0f).has_value(),
          "parseGuidePosition: empty text returns nothing");
    check(!parseGuidePosition("%", 1024.0f).has_value(),
          "parseGuidePosition: a bare \"%%\" with no digits returns nothing");
  }

  // --- resolveSnap(): the function that actually matters (PRD Q6). Canvas
  // is 1000x800; one horizontal guide at y=310, one vertical guide at
  // x=690 (both deliberately off the grid lattice below, so a snap to
  // (690,310) can only be explained by the guides, not a coincidental grid
  // line); grid spacing=50, subdivisions=1 (minor lines at multiples of
  // 50); snap threshold 5 document px. ---
  {
    const std::vector<Guide> guides = {
        Guide{GuideOrientation::Horizontal, 310.0f},
        Guide{GuideOrientation::Vertical, 690.0f},
    };
    constexpr float canvasW = 1000.0f, canvasH = 800.0f;
    constexpr float spacing = 50.0f;
    constexpr int subdivisions = 1;
    constexpr float threshold = 5.0f;

    {
      // 3px from the vertical guide, 2px from the horizontal one -- both
      // well outside range of any grid line (nearest grid x to 693 is 700,
      // 7px away; nearest grid y to 308 is 300, 8px away -- neither
      // qualifies within the 5px threshold), so this isolates guide
      // snapping specifically.
      const SnapResult r = resolveSnap(Vec2{693.0f, 308.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(r.snappedX && r.snappedY && nearf(r.point.x, 690.0f, 1e-3f) &&
                nearf(r.point.y, 310.0f, 1e-3f),
            "resolveSnap: a point near both guides snaps exactly onto them");
    }

    {
      const SnapResult r = resolveSnap(Vec2{432.0f, 217.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(!r.snappedX && !r.snappedY && nearf(r.point.x, 432.0f, 1e-3f) &&
                nearf(r.point.y, 217.0f, 1e-3f),
            "resolveSnap: a point far from every guide, grid line and canvas edge is left "
            "unchanged");
    }

    {
      // Nearest grid x to 452 is 450 (2px); nearest grid y to 218 is 200
      // (18px, outside threshold) -- X should snap to the grid, Y should
      // not snap to anything (axes snap independently).
      const SnapResult r = resolveSnap(Vec2{452.0f, 218.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, threshold);
      check(r.snappedX && nearf(r.point.x, 450.0f, 1e-3f),
            "resolveSnap: a point near a grid line snaps to that grid line");
      check(!r.snappedY && nearf(r.point.y, 218.0f, 1e-3f),
            "resolveSnap: ...while that same point's Y, near nothing, is left untouched "
            "(axes snap independently)");
    }

    {
      const SnapResult topLeft = resolveSnap(Vec2{2.0f, 2.0f}, guides, spacing, subdivisions,
                                             canvasW, canvasH, threshold);
      check(topLeft.snappedX && topLeft.snappedY && nearf(topLeft.point.x, 0.0f, 1e-3f) &&
                nearf(topLeft.point.y, 0.0f, 1e-3f),
            "resolveSnap: near the top-left corner snaps to the canvas origin edges");

      const SnapResult bottomRight =
          resolveSnap(Vec2{canvasW - 3.0f, canvasH - 1.0f}, guides, spacing, subdivisions,
                     canvasW, canvasH, threshold);
      check(bottomRight.snappedX && bottomRight.snappedY &&
                nearf(bottomRight.point.x, canvasW, 1e-3f) &&
                nearf(bottomRight.point.y, canvasH, 1e-3f),
            "resolveSnap: near the bottom-right corner snaps to the canvas far edges");
    }

    {
      // The global snapping toggle (PRD Q6) is implemented by the caller
      // passing threshold 0 rather than a second code path -- confirm that
      // actually disables snapping outright, even exactly on a guide.
      const SnapResult r = resolveSnap(Vec2{690.0f, 310.0f}, guides, spacing, subdivisions,
                                       canvasW, canvasH, 0.0f);
      check(!r.snappedX && !r.snappedY, "resolveSnap: a non-positive threshold snaps nothing");
    }
  }

  std::printf("[selftest] guides/grid/snap %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 3 step 7 ("Histogram over the visible region"). See
// SelfTest.hpp for the full breakdown; in short: empty/all-transparent,
// hand-computed per-channel bin placement (with a mixed-in alpha=0 texel
// that must contribute nothing), region clipping within one tile,
// HistogramParams::wholeDocument()'s exact span, and an un-premultiply
// proof on a translucent pixel -- mirroring runProbeTest()'s translucent-
// pixel discipline of checking against a specific hand-computed value, not
// just a plausible-looking one. Pure CPU -- computeHistogram() only ever
// reads a Document's tiles, no PaintSim or gpu involvement.
bool runHistogramTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- empty/all-transparent Document: createBlank() allocates zero tiles,
  // so every bin across all four channels stays zero and sampleCount is 0 --
  {
    const Document blank = Document::createBlank(64, 64, WorkingSpace{});
    const HistogramResult h = computeHistogram(blank, HistogramParams::wholeDocument(blank));
    check(h.sampleCount == 0,
          "computeHistogram: an all-transparent/unpainted Document reports sampleCount 0");
    bool allZero = true;
    for (uint64_t c : h.r) allZero = allZero && (c == 0);
    for (uint64_t c : h.g) allZero = allZero && (c == 0);
    for (uint64_t c : h.b) allZero = allZero && (c == 0);
    for (uint64_t c : h.luma) allZero = allZero && (c == 0);
    check(allZero, "computeHistogram: ...and every bin in all four channels is zero");
  }

  // --- hand-computed per-channel bin placement, plus a mixed-in alpha=0
  // texel that must contribute to nothing: pure red/green/blue opaque
  // texels at three distinct coordinates in one tile, plus a fourth,
  // alpha=0 texel carrying an otherwise-distinct colour that would be
  // trivially detectable if it leaked into any bin ------------------------
  {
    Document doc = Document::createBlank(32, 32, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{0, 0}, {1.0f, 0.0f, 0.0f, 1.0f});  // opaque red
    tile.writePixel(PixelCoord{1, 0}, {0.0f, 1.0f, 0.0f, 1.0f});  // opaque green
    tile.writePixel(PixelCoord{2, 0}, {0.0f, 0.0f, 1.0f, 1.0f});  // opaque blue
    tile.writePixel(PixelCoord{3, 0}, {0.5f, 0.5f, 0.5f, 0.0f});  // alpha 0 -- must not count

    const HistogramResult h = computeHistogram(doc, HistogramParams::wholeDocument(doc));

    check(h.sampleCount == 3,
          "computeHistogram: three opaque texels count, the alpha=0 texel does not");

    // srgbEncode(1.0) == 1.0 and srgbEncode(0.0) == 0.0 exactly (pow(x, *)
    // with x in {0,1} is exact in IEEE float), so every one of these lands
    // in bin 0 or bin (binCount-1) with no rounding ambiguity -- these are
    // exact integer equality checks, not tolerance-based.
    const size_t last = h.r.size() - 1;  // 255 at the default binCount == 256
    check(h.r[last] == 1 && h.r[0] == 2,
          "computeHistogram: R bin 255 holds the red texel; R bin 0 holds green+blue's R=0");
    check(h.g[last] == 1 && h.g[0] == 2,
          "computeHistogram: G bin 255 holds the green texel; G bin 0 holds red+blue's G=0");
    check(h.b[last] == 1 && h.b[0] == 2,
          "computeHistogram: B bin 255 holds the blue texel; B bin 0 holds red+green's B=0");

    // Luma: 0.2126 (red), 0.7152 (green), 0.0722 (blue) at binCount=256 ->
    // floor(x*256) = 54, 183, 18 -- each with a comfortable (>0.4-bin)
    // margin from the nearest integer boundary, so this is exact too.
    check(h.luma[54] == 1 && h.luma[183] == 1 && h.luma[18] == 1,
          "computeHistogram: Luma bins hold exactly the hand-computed Rec.709 luma bin for each "
          "of red/green/blue");
    uint64_t lumaTotal = 0;
    for (uint64_t c : h.luma) lumaTotal += c;
    check(lumaTotal == 3,
          "computeHistogram: no other Luma bin picked up a stray count -- exactly three "
          "qualifying texels total, matching sampleCount");
  }

  // --- region clipping: a region narrower than one allocated tile excludes
  // pixels inside that same tile but outside the region -------------------
  {
    Document doc = Document::createBlank(128, 128, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{5, 5}, {1.0f, 0.0f, 0.0f, 1.0f});    // inside the region below
    tile.writePixel(PixelCoord{50, 50}, {0.0f, 1.0f, 0.0f, 1.0f});  // same tile, outside it

    HistogramParams params;
    params.regionMin = PixelCoord{0, 0};
    params.regionMax = PixelCoord{10, 10};
    const HistogramResult h = computeHistogram(doc, params);

    check(h.sampleCount == 1,
          "computeHistogram: a region narrower than one tile counts only the texel inside it");
    const size_t last = h.r.size() - 1;
    check(h.r[last] == 1 && h.g[last] == 0,
          "computeHistogram: the in-region red texel is counted; the out-of-region green texel "
          "(same tile) is not");
  }

  // --- HistogramParams::wholeDocument(): spans exactly {0,0} to
  // {width,height}, and using it actually reaches a pixel at the document's
  // far corner (regionMax is exclusive, so this also proves the span is
  // {width,height}, not {width-1,height-1}) -------------------------------
  {
    Document doc = Document::createBlank(20, 15, WorkingSpace{});
    const HistogramParams params = HistogramParams::wholeDocument(doc);
    check(params.regionMin.x == 0 && params.regionMin.y == 0 && params.regionMax.x == 20 &&
              params.regionMax.y == 15,
          "HistogramParams::wholeDocument: spans exactly {0,0} to {width,height}");

    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{19, 14}, {1.0f, 1.0f, 1.0f, 1.0f});  // the document's far corner
    const HistogramResult h = computeHistogram(doc, params);
    check(h.sampleCount == 1,
          "computeHistogram: wholeDocument()'s region reaches the document's far corner pixel "
          "(19,14) -- proves regionMax is genuinely {width,height}, not {width-1,height-1}");
  }

  // --- translucent (partial-alpha) pixel bins at its un-premultiplied
  // straight colour, not its stored premultiplied value: premultiplied
  // (0.25, 0, 0, 0.5) un-premultiplies to linear (0.5, 0, 0) ---------------
  {
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{0, 0}, {0.25f, 0.0f, 0.0f, 0.5f});

    const HistogramResult h = computeHistogram(doc, HistogramParams::wholeDocument(doc));
    check(h.sampleCount == 1, "computeHistogram: the translucent texel counts exactly once");

    const float straightDisplay = srgbEncode(0.5f);         // expected: un-premultiplied
    const float premultipliedDisplay = srgbEncode(0.25f);   // what a premultiply bug would bin at
    const int32_t straightBin =
        std::clamp(static_cast<int32_t>(std::floor(straightDisplay * 256.0f)), 0, 255);
    const int32_t premultipliedBin =
        std::clamp(static_cast<int32_t>(std::floor(premultipliedDisplay * 256.0f)), 0, 255);
    check(straightBin != premultipliedBin,
          "runHistogramTest: sanity check -- srgbEncode(0.5) and srgbEncode(0.25) land in "
          "different bins, so this fixture can actually distinguish un-premultiplied from "
          "premultiplied binning");

    check(h.r[static_cast<size_t>(straightBin)] == 1,
          "computeHistogram: a translucent texel bins at srgbEncode() of its un-premultiplied "
          "straight colour -- the hand-computed premultiplied (0.25,0,0,0.5) -> straight "
          "(0.5,0,0) case");
    check(h.r[static_cast<size_t>(premultipliedBin)] == 0,
          "computeHistogram: ...and NOT at srgbEncode() of its raw stored premultiplied value "
          "(0.25) -- proves un-premultiplication actually ran, not just alpha passthrough");
    check(h.g[0] == 1 && h.b[0] == 1,
          "computeHistogram: the translucent texel's G/B channels (0 both pre- and "
          "post-un-premultiply) land in bin 0 either way");
  }

  std::printf("[selftest] histogram %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// ops/PointOps (Phase 3 steps 2+3; docs/operations.md §1.1; PRD B4). See
// SelfTest.hpp for the full breakdown. Pure CPU math throughout, matching
// runShaperTest()/runColorSpaceTest()'s own headless-first-class status --
// no PaintSim or gpu involvement anywhere in this function.
bool runPointOpsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };

  // --- Levels ---
  {
    // Neutral params (blackIn=0, whiteIn=1) is a true no-op for any input
    // *within* that [0,1] range -- deliberately not testing an HDR value
    // above 1.0 here: with whiteIn=1 the internal t-clamp legitimately
    // saturates such an input to whiteOut, which is correct Levels
    // black/white-point behaviour (every real levels tool clips outside
    // its own range), not a bug in "neutral is identity."
    const LevelsParams neutral{};
    const std::array<float, 3> rgb{0.05f, 0.5f, 0.95f};
    const std::array<float, 3> out = applyLevels(rgb, {neutral, neutral, neutral});
    check(nearRgb(out, rgb, kTol), "levels: neutral params is a true no-op within [blackIn, whiteIn]");
  }
  {
    // The flip side of the case above, made explicit rather than left
    // implicit: with default whiteIn=1/blackIn=0, an input outside that
    // range legitimately saturates to whiteOut/blackOut -- this IS the
    // internal t-clamp's documented consequence (PointOps.hpp's Levels
    // doc comment), not the general "ops don't clamp output" policy being
    // violated. A caller who needs Levels to pass HDR headroom through
    // untouched sets whiteIn above their expected max linear value.
    const LevelsParams neutral{};
    check(near(applyLevelsChannel(1.7f, neutral), 1.0f, kTol),
          "levels: neutral params saturates an above-whiteIn (HDR) input to whiteOut, by design");
    check(near(applyLevelsChannel(-0.3f, neutral), 0.0f, kTol),
          "levels: neutral params saturates a below-blackIn input to blackOut, by design");
  }
  {
    // Hand-computed: blackIn=0.1, whiteIn=0.9, gamma=2.0, blackOut=0.05,
    // whiteOut=0.95, input=0.5.
    //   t = (0.5-0.1)/(0.9-0.1) = 0.5
    //   t = pow(0.5, 1/2.0) = sqrt(0.5) = 0.7071067811865476
    //   out = 0.7071067811865476*(0.95-0.05)+0.05 = 0.6863961030678928
    LevelsParams p;
    p.blackIn = 0.1f;
    p.whiteIn = 0.9f;
    p.gamma = 2.0f;
    p.blackOut = 0.05f;
    p.whiteOut = 0.95f;
    const float out = applyLevelsChannel(0.5f, p);
    check(near(out, 0.6863961f, kTol), "levels: hand-computed non-trivial case matches");
  }
  {
    // Input below blackIn must not produce NaN -- the internal
    // clamp-t-to-[0,1]-before-pow() guard. blackIn=0.2, whiteIn=0.8,
    // gamma=0.5 (a fractional exponent -- exactly the case that would hit
    // pow() on a negative base without the clamp), blackOut/whiteOut left
    // at their neutral 0/1 default.
    //   t = (-1.0-0.2)/(0.8-0.2) = -2.0, clamped to 0
    //   pow(0, 1/0.5) = pow(0, 2.0) = 0
    //   out = 0*(1-0)+0 = 0
    LevelsParams p;
    p.blackIn = 0.2f;
    p.whiteIn = 0.8f;
    p.gamma = 0.5f;
    const float out = applyLevelsChannel(-1.0f, p);
    check(!std::isnan(out), "levels: input below blackIn does not produce NaN");
    check(near(out, 0.0f, kTol), "levels: below-blackIn input clamps to blackOut as hand-computed");
  }

  // --- Curves ---
  {
    Curve empty;
    check(near(evalCurve(empty, 0.37f), 0.37f, kTol), "evalCurve: 0 control points is identity");
    // 1 point is identity too -- NOT the single point's own y.
    Curve one = {{0.5f, 0.9f}};
    check(near(evalCurve(one, 0.37f), 0.37f, kTol),
          "evalCurve: 1 control point is identity, not the point's own y");
  }
  {
    // 2 points must reduce the Hermite formula exactly to the straight
    // line between them -- checked at several interior x, not just the
    // endpoints. Algebraically: with both endpoint tangents equal to the
    // shared secant slope m=(y1-y0)/dx, y(t) collapses to
    // y0*(1-t) + y1*t (h00-h10-h11 == 1-t and h10+h01+h11 == t identically).
    const Curve two = {{0.0f, 0.2f}, {1.0f, 0.8f}};
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const float expected = 0.2f + (0.8f - 0.2f) * x;
      const float actual = evalCurve(two, x);
      char label[112];
      std::snprintf(label, sizeof label,
                    "evalCurve: 2-point curve reduces to a straight line at x=%.2f", x);
      check(near(actual, expected, 1e-4f), label);
    }
  }
  {
    // Hand-computed 3-point interior case: points (0,0), (0.5,1.0),
    // (1.0,0.0), evaluated at x=0.25 (segment [0, 0.5]).
    //   tangent at (0,0)   [endpoint]: secant((0,0),(0.5,1.0)) = 2.0
    //   tangent at (0.5,1) [interior]: avg(secant((0,0),(0.5,1))=2.0,
    //                                      secant((0.5,1),(1,0))=-2.0) = 0.0
    //   t = (0.25-0)/0.5 = 0.5
    //   h00=0.5, h10=0.125, h01=0.5, h11=-0.125 (standard values at t=0.5)
    //   y = 0.5*0 + 0.125*0.5*2.0 + 0.5*1.0 + (-0.125)*0.5*0.0
    //     = 0 + 0.125 + 0.5 + 0 = 0.625
    const Curve three = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    check(near(evalCurve(three, 0.25f), 0.625f, kTol),
          "evalCurve: hand-computed 3-point interior case matches");
  }
  {
    // Flat extrapolation outside the authored x-range.
    const Curve c = {{0.2f, 0.3f}, {0.8f, 0.6f}};
    check(near(evalCurve(c, -5.0f), 0.3f, kTol),
          "evalCurve: extrapolates flat below the first control point");
    check(near(evalCurve(c, 5.0f), 0.6f, kTol),
          "evalCurve: extrapolates flat above the last control point");
  }
  {
    // (0,0)-(1,1) is the shaper-domain identity line (a 2-point curve, so
    // it reduces to y=x exactly per the property above). Composed end to
    // end -- shaperEncode -> evalCurve -> shaperDecode -- a spread of
    // linear inputs whose shaperEncode() lands inside [0,1] (true of all
    // five chosen here) must come back unchanged.
    std::array<Curve, 3> identityLine;
    identityLine[0] = identityLine[1] = identityLine[2] = Curve{{0.0f, 0.0f}, {1.0f, 1.0f}};
    for (float v : {0.0f, 0.02f, 0.18f, 0.5f, 1.0f}) {
      const std::array<float, 3> rgb{v, v, v};
      const std::array<float, 3> out = applyCurves(rgb, identityLine);
      char label[112];
      std::snprintf(label, sizeof label,
                    "applyCurves: shaper-domain identity line round-trips linear=%.3f", v);
      check(nearRgb(out, rgb, kTol), label);
    }
  }
  {
    // 0-point-per-channel curves through applyCurves(): exact passthrough,
    // no shaper round-trip at all (unlike the identity-line case above,
    // which does round-trip through the shaper and only approximately
    // preserves the input to float tolerance).
    const std::array<Curve, 3> empty{};
    const std::array<float, 3> rgb{0.37f, -0.4f, 12.0f};
    const std::array<float, 3> out = applyCurves(rgb, empty);
    check(nearRgb(out, rgb, kTol), "applyCurves: 0-point channels are an exact passthrough");
  }

  // --- Exposure ---
  {
    const std::array<float, 3> rgb{0.3f, 0.6f, 0.9f};
    check(nearRgb(applyExposure(rgb, ExposureParams{1.0f}), {0.6f, 1.2f, 1.8f}, kTol),
          "exposure: +1 stop doubles");
    check(nearRgb(applyExposure(rgb, ExposureParams{-1.0f}), {0.15f, 0.3f, 0.45f}, kTol),
          "exposure: -1 stop halves");
    check(nearRgb(applyExposure(rgb, ExposureParams{0.0f}), rgb, kTol),
          "exposure: 0 stops is identity");
  }

  // --- Saturation ---
  {
    const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
    check(nearRgb(applySaturation(rgb, SaturationParams{1.0f}), rgb, kTol),
          "saturation: scale=1 is identity");

    // Rec.709 luma computed by hand, directly from the literal weights --
    // independent of computeLuma()'s own copy of them.
    const float luma = 0.2126f * 0.2f + 0.7152f * 0.6f + 0.0722f * 0.9f;
    check(nearRgb(applySaturation(rgb, SaturationParams{0.0f}), {luma, luma, luma}, kTol),
          "saturation: scale=0 collapses every channel to the same value (the luma)");

    SaturationParams p;
    p.scale = 2.0f;
    const std::array<float, 3> expected{luma + (rgb[0] - luma) * 2.0f,
                                        luma + (rgb[1] - luma) * 2.0f,
                                        luma + (rgb[2] - luma) * 2.0f};
    check(nearRgb(applySaturation(rgb, p), expected, kTol),
          "saturation: hand-computed scale=2.0 (Rec.709 weights) case matches");
  }

  // --- Grayscale ---
  {
    const std::array<float, 3> red{1.0f, 0.0f, 0.0f};
    check(nearRgb(applyGrayscale(red, GrayscaleParams{}), {0.2126f, 0.2126f, 0.2126f}, kTol),
          "grayscale: pure red -> (0.2126, 0.2126, 0.2126)");

    const std::array<float, 3> rgb{0.3f, 0.5f, 0.7f};
    const float expectedLuma = 0.2126f * 0.3f + 0.7152f * 0.5f + 0.0722f * 0.7f;
    check(nearRgb(applyGrayscale(rgb, GrayscaleParams{}),
                  {expectedLuma, expectedLuma, expectedLuma}, kTol),
          "grayscale: general RGB case matches hand-computed Rec.709 luma");
  }

  // --- Channel mixer ---
  {
    const std::array<float, 3> rgb{0.2f, 0.5f, 0.8f};
    check(nearRgb(applyChannelMixer(rgb, ChannelMixerParams{}), rgb, kTol),
          "channel mixer: identity matrix is a no-op");

    ChannelMixerParams swapRB;
    swapRB.matrix = {{{0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}};
    check(nearRgb(applyChannelMixer(rgb, swapRB), {0.8f, 0.5f, 0.2f}, kTol),
          "channel mixer: hand-computed R/B swap matches");

    ChannelMixerParams offset;
    offset.matrix = {
        {{1.0f, 0.0f, 0.0f, 0.1f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}}};
    check(nearRgb(applyChannelMixer(rgb, offset), {0.3f, 0.5f, 0.8f}, kTol),
          "channel mixer: hand-computed +0.1 R-offset case matches");
  }

  // --- Premultiply wrapper (PRD B4) ---
  {
    const std::array<float, 4> transparent{0.3f, 0.4f, 0.5f, 0.0f};
    const std::vector<PointOp> anyOp{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{5.0f}); }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(transparent, anyOp);
    check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 0.0f,
          "premultiply wrapper: alpha<=0 maps to {0,0,0,0} untouched, regardless of the op");
  }
  {
    // Hand-computed: premultiplied (0.5,0,0,0.5) -> unpremultiply ->
    // (1,0,0) -> +1 stop exposure -> (2,0,0) -> re-premultiply by the
    // unchanged alpha 0.5 -> (1,0,0,0.5).
    const std::array<float, 4> premultiplied{0.5f, 0.0f, 0.0f, 0.5f};
    const std::vector<PointOp> ops{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{1.0f}); }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    check(near(out[0], 1.0f, kTol) && near(out[1], 0.0f, kTol) && near(out[2], 0.0f, kTol) &&
              near(out[3], 0.5f, kTol),
          "premultiply wrapper: hand-computed partially-transparent +1-stop example matches");
  }
  {
    // Alpha is never altered, regardless of which op runs -- even one
    // that collapses RGB entirely (saturation scale=0).
    const std::array<float, 4> premultiplied{0.2f, 0.4f, 0.1f, 0.37f};
    const std::vector<PointOp> ops{[](const std::array<float, 3>& rgb) {
      return applySaturation(rgb, SaturationParams{0.0f});
    }};
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    check(near(out[3], 0.37f, kTol), "premultiply wrapper: alpha is never altered by any op");
  }
  {
    // Composing two ops in sequence: premultiplied (0.4,0.2,0.0,0.4) ->
    // unpremultiply -> (1.0,0.5,0.0) -> exposure -1 stop -> (0.5,0.25,0.0)
    // -> saturation scale=0 (collapse to the Rec.709 luma of that
    // intermediate result) -> luma = 0.2126*0.5+0.7152*0.25+0.0722*0.0
    // = 0.2851 -> (0.2851,0.2851,0.2851) -> re-premultiply by the
    // unchanged alpha 0.4.
    const std::array<float, 4> premultiplied{0.4f, 0.2f, 0.0f, 0.4f};
    const std::vector<PointOp> ops{
        [](const std::array<float, 3>& rgb) { return applyExposure(rgb, ExposureParams{-1.0f}); },
        [](const std::array<float, 3>& rgb) {
          return applySaturation(rgb, SaturationParams{0.0f});
        },
    };
    const std::array<float, 4> out = applyPointOpsPremultiplied(premultiplied, ops);
    const float luma = 0.2126f * 0.5f + 0.7152f * 0.25f + 0.0722f * 0.0f;
    const float expected = luma * 0.4f;
    check(near(out[0], expected, kTol) && near(out[1], expected, kTol) &&
              near(out[2], expected, kTol) && near(out[3], 0.4f, kTol),
          "premultiply wrapper: composing two ops in sequence matches manual hand computation");
  }

  std::printf("[selftest] point ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// core/OpStack (Phase 3 step 5). See SelfTest.hpp for the full breakdown.
// Pure CPU bookkeeping plus calls into the already-tested ops/PointOps
// functions -- no PaintSim or gpu involvement anywhere in this function.
bool runOpStackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };
  // Runs `v` through `ops` in order, exactly the way a future color/LutBake
  // (or ops::applyPointOpsPremultiplied()) would consume an OpRun's
  // composed list.
  auto runOps = [](const std::vector<PointOp>& ops, std::array<float, 3> v) {
    for (const PointOp& op : ops) v = op(v);
    return v;
  };

  // --- empty stack: no runs, version starts at 0 ---
  {
    OpStack stack;
    check(stack.size() == 0, "OpStack: a default-constructed stack is empty");
    check(stack.version() == 0, "OpStack: version() starts at 0");
    check(stack.detectRuns().empty(), "OpStack: detectRuns() on an empty stack returns no runs");
  }

  // --- every mutator increments version(); at()/size() do not ---
  {
    OpStack stack;
    Op probe;
    probe.pointKind = PointOpKind::Exposure;
    probe.exposure.stops = 0.5f;

    const size_t idx = stack.add(probe);
    check(stack.version() == 1, "OpStack: add() increments version() by exactly one, from 0");
    check(idx == 0, "OpStack: add() returns the new entry's index");

    const uint64_t vAfterAdd = stack.version();
    (void)stack.at(idx);
    (void)stack.size();
    check(stack.version() == vAfterAdd, "OpStack: at()/size() do not change version()");

    stack.setEnabled(idx, false);
    check(stack.version() == vAfterAdd + 1, "OpStack: setEnabled() increments version()");
    check(!stack.at(idx).enabled, "OpStack: setEnabled() actually applies");

    // Same value it already has -- still bumps, by design (see
    // OpStack.hpp's version() doc comment: erring toward over-bumping
    // rather than checking whether anything actually changed).
    const uint64_t vBeforeNoopSet = stack.version();
    stack.setEnabled(idx, false);
    check(stack.version() == vBeforeNoopSet + 1,
          "OpStack: setEnabled() bumps version() even when the value doesn't change");

    const uint64_t vBeforeSetOp = stack.version();
    Op replacement;
    replacement.pointKind = PointOpKind::Saturation;
    stack.setOp(idx, replacement);
    check(stack.version() == vBeforeSetOp + 1, "OpStack: setOp() increments version()");
    check(stack.at(idx).pointKind == PointOpKind::Saturation,
          "OpStack: setOp() actually replaces the entry's data");

    const size_t idx2 = stack.add(Op{});
    const uint64_t vBeforeReorder = stack.version();
    stack.reorder(0, static_cast<size_t>(idx2));
    check(stack.version() == vBeforeReorder + 1, "OpStack: reorder() increments version()");

    const size_t sizeBeforeRemove = stack.size();
    const uint64_t vBeforeRemove = stack.version();
    stack.remove(0);
    check(stack.version() == vBeforeRemove + 1, "OpStack: remove() increments version()");
    check(stack.size() == sizeBeforeRemove - 1, "OpStack: remove() actually shrinks the stack");
  }

  // --- an all-PointA stack collapses into one run, composed in order ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 0.0f;
    stack.add(exposureOp);
    stack.add(saturationOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1, "detectRuns: an all-PointA stack collapses into exactly one run");
    if (runs.size() == 1) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 2,
            "detectRuns: the one run spans the whole stack, {0,2}");
      check(runs[0].ops.size() == 2,
            "detectRuns: the run's composed op list has both entries");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> viaDirect =
          applySaturation(applyExposure(rgb, exposureOp.exposure), saturationOp.saturation);
      check(nearRgb(viaRun, viaDirect, kTol),
            "detectRuns: running a value through the composed run matches calling "
            "applyExposure() then applySaturation() directly, in that order");
    }
  }

  // --- a disabled PointA entry in the middle does not split the run ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op disabledSaturation;
    disabledSaturation.pointKind = PointOpKind::Saturation;
    disabledSaturation.saturation.scale = 0.0f;  // would collapse to luma if it ran
    disabledSaturation.enabled = false;
    Op channelMixerOp;
    channelMixerOp.pointKind = PointOpKind::ChannelMixer;
    channelMixerOp.channelMixer.matrix = {
        {{0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}};  // R<->B swap

    stack.add(exposureOp);
    stack.add(disabledSaturation);
    stack.add(channelMixerOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1, "detectRuns: a disabled PointA entry does not split the run");
    if (runs.size() == 1) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 3,
            "detectRuns: the run still spans all three indices, {0,3}, including the disabled "
            "entry's slot");
      check(runs[0].ops.size() == 2,
            "detectRuns: the run's composed op list has only the two enabled entries");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> viaSkippingDisabled =
          applyChannelMixer(applyExposure(rgb, exposureOp.exposure), channelMixerOp.channelMixer);
      check(nearRgb(viaRun, viaSkippingDisabled, kTol),
            "detectRuns: the composed run matches skipping the disabled saturation op entirely");
    }
  }

  // --- a real class boundary splits the stack into two runs ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    // Pure test fixture, per this module's own doc comment -- no real
    // class-B/C/D op exists anywhere in this codebase yet. Constructed only
    // to exercise detectRuns()'s run-splitting on a genuine class boundary.
    Op spatialPlaceholder;
    spatialPlaceholder.opClass = OpClass::SpatialB;
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 2.0f;

    stack.add(exposureOp);
    stack.add(spatialPlaceholder);
    stack.add(saturationOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 2, "detectRuns: a non-PointA entry splits the stack into two runs");
    if (runs.size() == 2) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 1,
            "detectRuns: the first run is exactly the PointA entry before the boundary, {0,1}");
      check(runs[1].startIndex == 2 && runs[1].endIndex == 3,
            "detectRuns: the second run is exactly the PointA entry after the boundary, {2,3}");
      check(runs[0].ops.size() == 1 && runs[1].ops.size() == 1,
            "detectRuns: each run's composed op list has only its own side's op");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      check(nearRgb(runOps(runs[0].ops, rgb), applyExposure(rgb, exposureOp.exposure), kTol),
            "detectRuns: the first run's op is the exposure op");
      check(nearRgb(runOps(runs[1].ops, rgb), applySaturation(rgb, saturationOp.saturation), kTol),
            "detectRuns: the second run's op is the saturation op");
    }
  }

  // --- reorder(): order genuinely matters, and detectRuns() reflects it ---
  {
    // Exposure (a uniform scalar multiply) and Levels with gamma != 1 (a
    // nonlinear pow) do NOT commute -- unlike two purely linear ops (e.g.
    // Exposure+Saturation, which always agree regardless of order since
    // scalar multiplication commutes with any linear map -- see how the
    // all-PointA-stack case above deliberately avoided this trap), so this
    // pair genuinely exercises order.
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;  // x2

    Op levelsOp;
    levelsOp.pointKind = PointOpKind::Levels;
    LevelsParams lp;
    lp.gamma = 2.0f;
    levelsOp.levels = {lp, lp, lp};

    const std::array<float, 3> rgb{0.1f, 0.2f, 0.3f};
    const std::array<float, 3> exposureThenLevels =
        applyLevels(applyExposure(rgb, exposureOp.exposure), levelsOp.levels);
    const std::array<float, 3> levelsThenExposure =
        applyExposure(applyLevels(rgb, levelsOp.levels), exposureOp.exposure);
    check(!nearRgb(exposureThenLevels, levelsThenExposure, kTol),
          "reorder fixture sanity check: exposure-then-levels and levels-then-exposure "
          "genuinely disagree, so the reorder assertion below can't pass vacuously");

    OpStack stack;
    stack.add(exposureOp);  // index 0
    stack.add(levelsOp);    // index 1

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 1 && nearRgb(runOps(runs[0].ops, rgb), exposureThenLevels, kTol),
            "reorder: before reordering, the composed run matches exposure-then-levels");
    }

    const uint64_t versionBefore = stack.version();
    stack.reorder(1, 0);  // move Levels (index 1) to the front
    check(stack.version() == versionBefore + 1, "reorder(): increments version()");

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 1 && nearRgb(runOps(runs[0].ops, rgb), levelsThenExposure, kTol),
            "reorder: after moving Levels to index 0, the composed run matches "
            "levels-then-exposure");
    }
  }

  // --- remove(): run boundaries and the composed op list update correctly ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op spatialPlaceholder;
    spatialPlaceholder.opClass = OpClass::SpatialB;  // test fixture only, see above
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 0.0f;

    stack.add(exposureOp);         // index 0, run A
    stack.add(spatialPlaceholder); // index 1, boundary
    stack.add(saturationOp);       // index 2, run B

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 2 && runs[0].startIndex == 0 && runs[0].endIndex == 1 &&
                runs[1].startIndex == 2 && runs[1].endIndex == 3,
            "remove fixture sanity check: two runs at the expected indices before removal");
    }

    // Remove the exposure entry at index 0. The placeholder shifts down to
    // index 0, saturation shifts down to index 1 -- and the removed
    // exposure op's effect is gone from the one remaining run.
    const uint64_t versionBefore = stack.version();
    stack.remove(0);
    check(stack.version() == versionBefore + 1, "remove(): increments version()");
    check(stack.size() == 2, "remove(): the stack shrinks by exactly one entry");

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1 && runs[0].startIndex == 1 && runs[0].endIndex == 2,
          "remove: the run after the removed entry shifts its indices down by one, {1,2}");

    if (runs.size() == 1) {
      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> saturationOnly = applySaturation(rgb, saturationOp.saturation);
      check(nearRgb(viaRun, saturationOnly, kTol),
            "remove: the composed op list no longer includes the removed exposure op's effect");
    }
  }

  std::printf("[selftest] op stack %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// color/LutBake (PLAN.md Phase 3 step 4, ADR-0004). See SelfTest.hpp for the
// full breakdown. Needs `gpu` -- genuine compute-shader work, no PaintSim
// involvement.
bool runLutBakeTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto halfRT = [](float v) { return halfToFloat(floatToHalf(v)); };
  auto gridCoord = [](int idx) {
    return (static_cast<float>(idx) + 0.5f) / static_cast<float>(kLutSize);
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };

  // Tolerance derivation (PLAN.md Findings, 2026-08-18 Phase 3 step 4 row).
  // The real bake round-trips every shaper-domain value through rgba16float
  // storage once per ping-pong write (once for the seed, once per op pass)
  // -- half's own ~2^-11 relative quantization step, NOT float noise, is
  // the dominant and *expected* source of GPU-vs-CPU disagreement here, so
  // comparing against raw float math (this codebase's usual 1e-4) would be
  // comparing against a value the real bake could never actually produce.
  // simulateBakeCpu() below simulates that exact quantization, stage by
  // stage, via core::floatToHalf/halfToFloat, so what should be left for
  // this tolerance to cover is only the residual difference between
  // std::pow/std::log2/std::exp2 (CPU) and their WGSL/Metal equivalents
  // (GPU) at float32 precision.
  //
  // Measured directly before landing (temporary instrumentation, not left
  // in this file): across every case below, the simulated-vs-GPU max delta
  // at any sampled channel topped out at 1.46e-3, for the composed 3-op run
  // (seed + 3 op-pass writes -- the deepest ping-pong chain tested here);
  // single-op cases topped out at 4.88e-4, exactly one half ULP at unity
  // magnitude (2^-11), confirming the dominant term really is quantization,
  // not transcendental drift. A second check -- comparing the GPU result
  // against a reference that does *not* simulate the half round trips (the
  // naive "just call shaperEncode(clamp01(op(shaperDecode(x))))" version) --
  // produced deltas of the same order (up to 1.08e-3) rather than something
  // dramatically larger, because these sample points don't hit a
  // steep-tangent region where quantization would compound; the simulated
  // reference is still the theoretically correct comparison (it is what
  // the GPU actually computes, bit for bit, at each write) and is what this
  // test uses. kResidualTol below sits at 2e-3, comfortable but not loose
  // headroom above the observed 1.46e-3 worst case -- a real formula bug
  // (wrong constant, wrong tangent formula, wrong channel) produces
  // differences of a fundamentally different character and magnitude
  // (typically >= 1e-2, often much larger), not one this tolerance would
  // absorb.
  constexpr float kResidualTol = 2e-3f;

  // Hand-picked interior grid cells spanning low/mid/high shaper domain --
  // six samples, not the full 32768-cell sweep, matching this codebase's
  // existing GPU-test style (runTiledViewportTest()/runMipPyramidTest()'s
  // own hand-picked corner/known-pixel checks rather than exhaustive scans).
  const std::vector<std::array<int, 3>> sampleCells = {
      {4, 4, 4}, {8, 16, 24}, {16, 16, 16}, {24, 8, 16}, {28, 28, 28}, {2, 20, 10},
  };

  // Simulates the exact half-precision write/read the LUT undergoes at
  // every ping-pong stage -- seed write, then one write per op in `cpuOps`
  // -- so this returns the CPU-side equivalent of what the real GPU bake
  // actually stores, not what pure float math alone would produce. `cpuOps`
  // are plain ops::PointOp closures over the *same* params fed to the GPU
  // kernel; the wrapper shape here (shaperDecode -> op -> shaperEncode ->
  // clamp01) is applied uniformly across all six op kinds, including
  // Curves -- ops::applyCurves() already does its own internal
  // shaperEncode/evalCurve/shaperDecode, so wrapping it in this outer
  // decode/encode is a redundant round trip relative to the GPU's own
  // curves kernel (which skips the outer wrapper entirely, see
  // shaders/lut_op_curves.wgsl) -- but shaperDecode(shaperEncode(x)) == x
  // to float32 precision (color/Shaper.cpp is an exact inverse pair), so
  // the redundant round trip changes the simulated value by float-ULP noise
  // only, many orders of magnitude below kResidualTol. Using one uniform
  // wrapper for all six kinds here, rather than special-casing Curves,
  // keeps this simulation a direct, obviously-correct mirror of
  // color/LutBake.cpp's own per-op-pass loop.
  auto simulateBakeCpu = [&](int gx, int gy, int gz,
                             const std::vector<PointOp>& cpuOps) -> std::array<float, 3> {
    std::array<float, 3> shaped{gridCoord(gx), gridCoord(gy), gridCoord(gz)};
    for (float& c : shaped) c = halfRT(c);  // seed write
    for (const PointOp& op : cpuOps) {
      const std::array<float, 3> linearIn{shaperDecode(shaped[0]), shaperDecode(shaped[1]),
                                          shaperDecode(shaped[2])};
      const std::array<float, 3> linearOut = op(linearIn);
      std::array<float, 3> enc{shaperEncode(linearOut[0]), shaperEncode(linearOut[1]),
                               shaperEncode(linearOut[2])};
      for (float& c : enc) c = std::clamp(c, 0.0f, 1.0f);
      for (int c = 0; c < 3; ++c) shaped[static_cast<size_t>(c)] = halfRT(enc[static_cast<size_t>(c)]);
    }
    return shaped;
  };

  // Reads texel (gx,gy,gz) out of a flat readbackRGBA16F3D() buffer -- z
  // slowest, matching that helper's own copy layout (rowsPerImage == size,
  // depthOrArrayLayers == size).
  auto sampleLut = [&](const std::vector<float>& pixels, int gx, int gy, int gz) -> std::array<float, 3> {
    const size_t s = static_cast<size_t>(kLutSize);
    const size_t idx =
        ((static_cast<size_t>(gz) * s + static_cast<size_t>(gy)) * s + static_cast<size_t>(gx)) * 4;
    return {pixels[idx + 0], pixels[idx + 1], pixels[idx + 2]};
  };

  // Bakes `gpuOps` on the GPU, reads the LUT back, and checks every sample
  // cell against simulateBakeCpu(..., cpuOps) -- `cpuOps` must be the exact
  // same ops, in the same order, as plain ops::PointOp closures. Releases
  // the baked LUT before returning either way.
  auto runOneCase = [&](const std::vector<Op>& gpuOps, const std::vector<PointOp>& cpuOps,
                        const char* label) {
    char buf[160];
    Lut3D lut = bakeLut(gpu, gpuOps);
    std::snprintf(buf, sizeof buf, "%s: bake produces a non-null LUT texture", label);
    check(lut.texture != nullptr, buf);
    if (!lut.texture) return;

    std::vector<float> pixels;
    const bool readOk = readbackRGBA16F3D(gpu, lut.texture, static_cast<uint32_t>(lut.size), pixels);
    std::snprintf(buf, sizeof buf, "%s: GPU LUT readback succeeds", label);
    check(readOk, buf);
    if (readOk) {
      for (const auto& cell : sampleCells) {
        const std::array<float, 3> expected = simulateBakeCpu(cell[0], cell[1], cell[2], cpuOps);
        const std::array<float, 3> actual = sampleLut(pixels, cell[0], cell[1], cell[2]);
        std::snprintf(buf, sizeof buf, "%s: grid cell (%d,%d,%d) matches CPU reference", label,
                     cell[0], cell[1], cell[2]);
        check(nearRgb(actual, expected, kResidualTol), buf);
      }
    }
    releaseLut3D(lut);
  };

  // --- Empty run: the seed pass alone must already be a valid identity LUT ---
  {
    const std::vector<Op> gpuOps{};
    const std::vector<PointOp> cpuOps{};
    runOneCase(gpuOps, cpuOps, "LutBake empty run (seed-only identity)");
  }

  // --- Levels ---
  {
    LevelsParams p;
    p.blackIn = 0.1f;
    p.whiteIn = 0.9f;
    p.gamma = 1.8f;
    p.blackOut = 0.02f;
    p.whiteOut = 0.95f;
    Op op;
    op.pointKind = PointOpKind::Levels;
    op.levels = {p, p, p};
    const std::array<LevelsParams, 3> params = op.levels;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyLevels(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake levels");
  }

  // --- Exposure ---
  {
    Op op;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = 1.5f;
    const ExposureParams params = op.exposure;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyExposure(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake exposure");
  }

  // --- Saturation ---
  {
    Op op;
    op.pointKind = PointOpKind::Saturation;
    op.saturation.scale = 1.6f;  // default lumaWeights (Rec.709)
    const SaturationParams params = op.saturation;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applySaturation(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake saturation");
  }

  // --- Grayscale ---
  {
    Op op;
    op.pointKind = PointOpKind::Grayscale;  // default lumaWeights (Rec.709)
    const GrayscaleParams params = op.grayscale;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyGrayscale(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake grayscale");
  }

  // --- Channel mixer ---
  {
    Op op;
    op.pointKind = PointOpKind::ChannelMixer;
    op.channelMixer.matrix = {
        {{0.0f, 0.0f, 1.0f, 0.05f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -0.02f}}};
    const ChannelMixerParams params = op.channelMixer;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyChannelMixer(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake channel mixer");
  }

  // --- Curves: the highest-risk kernel -- three distinct curve shapes ---
  {
    // 2-point straight line: the case evalCurve()'s own Hermite formula
    // reduces to exactly, per ops/PointOps.hpp's doc comment.
    const Curve line = {{0.0f, 0.1f}, {1.0f, 0.9f}};
    Op op;
    op.pointKind = PointOpKind::Curves;
    op.curves = {line, line, line};
    const std::array<Curve, 3> params = op.curves;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyCurves(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake curves (2-point straight line)");
  }
  {
    // 3-point interior case -- exercises tangentAt()'s averaged-secant
    // branch for the middle point.
    const Curve three = {{0.0f, 0.05f}, {0.5f, 0.75f}, {1.0f, 0.95f}};
    Op op;
    op.pointKind = PointOpKind::Curves;
    op.curves = {three, three, three};
    const std::array<Curve, 3> params = op.curves;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyCurves(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake curves (3-point interior)");
  }
  {
    // A point count at kMaxCurvePointsPerChannel exactly -- the GPU
    // kernel's storage buffer bound, exercised right at its edge rather
    // than only comfortably below it.
    Curve many;
    for (int i = 0; i < kMaxCurvePointsPerChannel; ++i) {
      const float x = static_cast<float>(i) / static_cast<float>(kMaxCurvePointsPerChannel - 1);
      const float y = std::pow(x, 1.3f) * 0.9f + 0.05f * std::sin(x * 6.0f);
      many.push_back({x, y});
    }
    Op op;
    op.pointKind = PointOpKind::Curves;
    op.curves = {many, many, many};
    const std::array<Curve, 3> params = op.curves;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyCurves(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake curves (kMaxCurvePointsPerChannel points)");
  }

  // --- Composed multi-op run: proves the ping-pong sequencing itself, not
  // just any single kernel in isolation -- the GPU analogue of
  // runOpStackTest()'s own CPU-side composition proof. Exposure ->
  // Saturation -> Channel mixer, baked in one bakeLut() call. ---
  {
    Op e;
    e.pointKind = PointOpKind::Exposure;
    e.exposure.stops = -0.5f;
    Op s;
    s.pointKind = PointOpKind::Saturation;
    s.saturation.scale = 1.4f;
    Op m;
    m.pointKind = PointOpKind::ChannelMixer;
    m.channelMixer.matrix = {{{0.9f, 0.05f, 0.05f, 0.0f},
                              {0.05f, 0.9f, 0.05f, 0.01f},
                              {0.0f, 0.1f, 0.9f, 0.0f}}};
    const ExposureParams ep = e.exposure;
    const SaturationParams sp = s.saturation;
    const ChannelMixerParams cp = m.channelMixer;
    const std::vector<Op> gpuOps{e, s, m};
    const std::vector<PointOp> cpuOps{
        [ep](const std::array<float, 3>& rgb) { return applyExposure(rgb, ep); },
        [sp](const std::array<float, 3>& rgb) { return applySaturation(rgb, sp); },
        [cp](const std::array<float, 3>& rgb) { return applyChannelMixer(rgb, cp); },
    };
    runOneCase(gpuOps, cpuOps,
              "LutBake composed run (exposure -> saturation -> channel mixer)");
  }

  std::printf("[selftest] lut bake %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch ->
// un-shape"). See SelfTest.hpp for the full breakdown; this is the
// implementation.
bool runApplyPassTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  auto halfRT = [](float v) { return halfToFloat(floatToHalf(v)); };
  auto gridCoord = [](int idx) {
    return (static_cast<float>(idx) + 0.5f) / static_cast<float>(kLutSize);
  };

  // The one op this test bakes and grades with -- Saturation, scale 0.3,
  // the same value ui/MacPaintUI.cpp's "Test Grade (debug)" toggle uses
  // (not required for correctness, just tidy consistency).
  SaturationParams satParams;
  satParams.scale = 0.3f;

  // Exact per-corner LUT texel value: the same half-precision ping-pong
  // round trip color/LutBake.cpp's real GPU bake performs (seed write,
  // one Saturation-pass write) -- runLutBakeTest()'s own simulateBakeCpu()
  // above, specialized to this test's one fixed op and taking explicit
  // grid indices so it can be called for each of a trilinear sample's 8
  // surrounding corners below.
  auto texelAt = [&](int gx, int gy, int gz) -> std::array<float, 3> {
    std::array<float, 3> shaped{gridCoord(gx), gridCoord(gy), gridCoord(gz)};
    for (float& c : shaped) c = halfRT(c);  // seed write
    const std::array<float, 3> linearIn{shaperDecode(shaped[0]), shaperDecode(shaped[1]),
                                        shaperDecode(shaped[2])};
    const std::array<float, 3> linearOut = applySaturation(linearIn, satParams);
    std::array<float, 3> enc{shaperEncode(linearOut[0]), shaperEncode(linearOut[1]),
                             shaperEncode(linearOut[2])};
    for (float& c : enc) c = std::clamp(c, 0.0f, 1.0f);
    std::array<float, 3> out{};
    for (int c = 0; c < 3; ++c) out[static_cast<size_t>(c)] = halfRT(enc[static_cast<size_t>(c)]);
    return out;
  };

  // Exact trilinear interpolation of texelAt()'s 8 surrounding corners --
  // the same math textureSampleLevel() itself evaluates (ClampToEdge
  // address mode on every axis, single mip level so there is no mip
  // blend to model), run here on the CPU.
  //
  // Building this, rather than only picking canvas values close to a grid
  // line the way runLutBakeTest()'s own grid-cell picks do, is a
  // deliberate choice specific to this test: sim::PaintSim's canvas_ is
  // only reachable through the real solver/composite pipeline
  // (clearCanvas() gives exactly one fixed value; every other pixel is
  // emergent physics or the procedural paper substrate), so unlike
  // runLutBakeTest() -- which can freely pick any LUT grid cell directly
  // -- there is no way to force an arbitrary, precisely grid-aligned byte
  // value onto the live canvas. An exact interpolation reference is
  // correct for *any* input coordinate, not merely an approximation that
  // only holds near a grid line -- it still "eliminates the variable
  // you're not testing" (interpolation-weight uncertainty is computed
  // exactly here, not approximated away or left to accidentally leak into
  // the residual), the same goal runLutBakeTest()'s grid-cell picks
  // serve, just by a different, equally rigorous route.
  auto trilinearSample = [&](float u, float v, float w) -> std::array<float, 3> {
    auto axisIdx = [](float t, int& i0, int& i1, float& frac) {
      const float tc = t * static_cast<float>(kLutSize) - 0.5f;
      const float f0 = std::floor(tc);
      i0 = std::clamp(static_cast<int>(f0), 0, kLutSize - 1);
      i1 = std::clamp(static_cast<int>(f0) + 1, 0, kLutSize - 1);
      frac = tc - f0;
    };
    int xi0, xi1, yi0, yi1, zi0, zi1;
    float fx, fy, fz;
    axisIdx(u, xi0, xi1, fx);
    axisIdx(v, yi0, yi1, fy);
    axisIdx(w, zi0, zi1, fz);

    const auto c000 = texelAt(xi0, yi0, zi0), c100 = texelAt(xi1, yi0, zi0);
    const auto c010 = texelAt(xi0, yi1, zi0), c110 = texelAt(xi1, yi1, zi0);
    const auto c001 = texelAt(xi0, yi0, zi1), c101 = texelAt(xi1, yi0, zi1);
    const auto c011 = texelAt(xi0, yi1, zi1), c111 = texelAt(xi1, yi1, zi1);

    std::array<float, 3> out{};
    for (size_t c = 0; c < 3; ++c) {
      const float e00 = c000[c] * (1.0f - fx) + c100[c] * fx;
      const float e10 = c010[c] * (1.0f - fx) + c110[c] * fx;
      const float e01 = c001[c] * (1.0f - fx) + c101[c] * fx;
      const float e11 = c011[c] * (1.0f - fx) + c111[c] * fx;
      const float e0 = e00 * (1.0f - fy) + e10 * fy;
      const float e1 = e01 * (1.0f - fy) + e11 * fy;
      out[c] = e0 * (1.0f - fz) + e1 * fz;
    }
    return out;
  };

  // --- Set up a known, deterministic, unpainted canvas: blank paper
  // substrate only, no pigment. composite.wgsl's substrate() formula
  // applied to sim::PaintSim's own procedurally generated paper_ field,
  // which is fixed for the lifetime of this sim (generatePaper() only
  // ever runs once, from allocFields()) and therefore identical every
  // time --selftest runs -- a real, reproducible value this test can
  // check against, not one it gets to dictate outright. ---
  sim.setMode(gpu, PaintMode::Watercolor);
  sim.clearCanvas(gpu);
  SimParams blank{};
  sim.frame(gpu, blank);  // one step: zero fields stay zero; composite paints the substrate

  std::vector<uint8_t> canvasBefore;
  check(sim.readbackCanvas(gpu, canvasBefore),
        "runApplyPassTest: canvas readback (blank paper substrate)");

  OpStack opStack;
  Op satOp;
  satOp.opClass = OpClass::PointA;
  satOp.pointKind = PointOpKind::Saturation;
  satOp.saturation = satParams;
  opStack.add(satOp);

  sim.updateGradePreview(gpu, opStack);
  std::vector<uint8_t> gradedFirst;
  check(sim.readbackGraded(gpu, gradedFirst), "runApplyPassTest: graded readback (first bake)");

  // --- Tolerance derivation (PLAN.md Findings, 2026-08-18 Phase 3 step 6
  // row) -- measured empirically, not guessed, the same discipline
  // runLutBakeTest()'s own kResidualTol derivation used. On top of that
  // test's own sources of GPU-vs-CPU disagreement (half-precision
  // quantization at each LUT ping-pong write, float32 transcendental
  // drift between CPU std::pow/log2/exp2 and their WGSL/Metal
  // equivalents), this pass adds the canvas_ source and graded_
  // destination textures' own RGBA8Unorm quantization (~1/255 per
  // channel, roughly 4x coarser than the LUT's own half-float step) and
  // genuine hardware trilinear interpolation (vs. this test's own
  // from-scratch CPU formula above) rather than an exact texel fetch.
  // Measured directly before landing (temporary instrumentation, not left
  // in this file): across the six hand-picked canvas points below (18
  // channel comparisons total), the max residual between the GPU graded
  // readback and this CPU trilinear reference was 4.37e-3 (pixel
  // (204,921), R channel); every other channel/point landed well under
  // that, mostly in the 1-2e-3 band matching LutBake's own dominant
  // half-quantization term, with the outlier explained by the added
  // RGBA8Unorm quantization this pass has and LutBake's own comparisons
  // don't. kApplyResidualTol lands at 6e-3 -- the same ~1.37x headroom
  // ratio runLutBakeTest's own kResidualTol=2e-3 used above its measured
  // 1.46e-3 worst case, comfortable but not loose. A real formula bug
  // (wrong constant, wrong domain, a transposed axis in the trilinear
  // blend) produces differences of a fundamentally different character
  // (typically an order of magnitude larger or more), not one this
  // tolerance would quietly absorb.
  constexpr float kApplyResidualTol = 6e-3f;

  const uint32_t W = sim.width();
  struct Pt { float fx, fy; };
  const Pt points[] = {
      {0.10f, 0.10f}, {0.30f, 0.70f}, {0.50f, 0.50f},
      {0.80f, 0.20f}, {0.20f, 0.90f}, {0.95f, 0.08f},
  };
  const bool haveReadbacks = !canvasBefore.empty() && !gradedFirst.empty();
  check(haveReadbacks, "runApplyPassTest: both readbacks succeeded (prerequisite for the point checks below)");
  if (haveReadbacks) {
    for (const Pt& pt : points) {
      const uint32_t x = static_cast<uint32_t>(pt.fx * static_cast<float>(sim.width()));
      const uint32_t y = static_cast<uint32_t>(pt.fy * static_cast<float>(sim.height()));
      const size_t idx = (static_cast<size_t>(y) * W + x) * 4;

      const float r = canvasBefore[idx + 0] / 255.0f;
      const float g = canvasBefore[idx + 1] / 255.0f;
      const float b = canvasBefore[idx + 2] / 255.0f;
      const float u = shaperEncode(r), v = shaperEncode(g), w = shaperEncode(b);

      const std::array<float, 3> shapedExpected = trilinearSample(u, v, w);
      std::array<float, 3> expected{shaperDecode(shapedExpected[0]), shaperDecode(shapedExpected[1]),
                                    shaperDecode(shapedExpected[2])};
      for (float& c : expected) c = std::clamp(c, 0.0f, 1.0f);

      const float ar = gradedFirst[idx + 0] / 255.0f;
      const float ag = gradedFirst[idx + 1] / 255.0f;
      const float ab = gradedFirst[idx + 2] / 255.0f;

      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "runApplyPassTest: pixel (%u,%u) graded output matches CPU trilinear reference",
                    x, y);
      check(near(ar, expected[0], kApplyResidualTol) && near(ag, expected[1], kApplyResidualTol) &&
                near(ab, expected[2], kApplyResidualTol),
            buf);
    }
  }

  // --- Version gating: two calls with an unchanged OpStack stay
  // byte-identical; mutating the OpStack (setOp, bumping version())
  // changes the graded output -- proving the rebake path is exercised
  // for real, not merely present in the code. Not a proof the skip
  // branch was literally taken on the unchanged call (out of scope --
  // see SelfTest.hpp), only that the visible behaviour is correct either
  // way. ---
  sim.updateGradePreview(gpu, opStack);
  std::vector<uint8_t> gradedSecond;
  check(sim.readbackGraded(gpu, gradedSecond),
        "runApplyPassTest: graded readback (repeat call, unchanged OpStack)");
  check(haveReadbacks && !gradedSecond.empty() && gradedFirst == gradedSecond,
        "runApplyPassTest: repeating updateGradePreview() with an unchanged OpStack is byte-stable");

  Op mutatedSatOp = satOp;
  mutatedSatOp.saturation.scale = 1.6f;  // a visibly different scale, not just a re-set of 0.3
  opStack.setOp(0, mutatedSatOp);
  sim.updateGradePreview(gpu, opStack);
  std::vector<uint8_t> gradedThird;
  check(sim.readbackGraded(gpu, gradedThird),
        "runApplyPassTest: graded readback (after mutating the OpStack)");
  check(!gradedSecond.empty() && !gradedThird.empty() && gradedThird != gradedSecond,
        "runApplyPassTest: mutating the OpStack (setOp) changes the graded output -- the "
        "version-bump-triggers-rebake path is exercised for real");

  std::printf("[selftest] apply pass %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// PLAN.md Phase 3 step 8 ("Op-stack UI... and a curve widget operating in
// the shaper domain"). See SelfTest.hpp for the full breakdown. Pure CPU --
// app/CurveEdit.hpp has no ImGui/GPU/PaintSim involvement whatsoever.
bool runCurveEditTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto isAscendingX = [](const Curve& c) {
    for (size_t i = 1; i < c.size(); ++i)
      if (!(c[i - 1].x <= c[i].x)) return false;
    return true;
  };

  // --- curveToPlot() / plotToCurve(): the y-flip and the round trip ---
  {
    float px = 0.0f, py = 0.0f;
    curveToPlot(0.0f, 0.0f, 100.0f, px, py);
    check(nearf(px, 0.0f, 1e-4f) && nearf(py, 100.0f, 1e-4f),
          "curveToPlot: curve-space (0,0) (y-up) is plot-local (0,plotSize) "
          "(y-down) -- the bottom-left corner");

    curveToPlot(1.0f, 1.0f, 100.0f, px, py);
    check(nearf(px, 100.0f, 1e-4f) && nearf(py, 0.0f, 1e-4f),
          "curveToPlot: curve-space (1,1) is plot-local (plotSize,0) -- the top-right corner");

    curveToPlot(0.5f, 0.25f, 100.0f, px, py);
    check(nearf(px, 50.0f, 1e-4f) && nearf(py, 75.0f, 1e-4f),
          "curveToPlot: a hand-computed interior point (0.5,0.25) -> (50,75)");

    float cx = 0.0f, cy = 0.0f;
    plotToCurve(px, py, 100.0f, cx, cy);
    check(nearf(cx, 0.5f, 1e-4f) && nearf(cy, 0.25f, 1e-4f),
          "plotToCurve: inverts curveToPlot() exactly for the same point");

    curveToPlot(0.5f, 0.25f, 0.0f, px, py);
    check(nearf(px, 0.0f, 1e-4f) && nearf(py, 0.0f, 1e-4f),
          "curveToPlot: plotSize <= 0 returns (0,0) rather than dividing by zero");
    plotToCurve(10.0f, 10.0f, -5.0f, cx, cy);
    check(nearf(cx, 0.0f, 1e-4f) && nearf(cy, 0.0f, 1e-4f),
          "plotToCurve: plotSize <= 0 returns (0,0) rather than dividing by zero");
  }

  // --- hitTestPoint(): exact/inside/outside radius, empty curve, tie-break ---
  {
    const Curve curve = {CurvePoint{0.2f, 0.3f}, CurvePoint{0.6f, 0.7f}};
    constexpr float plotSize = 200.0f;
    float p0x = 0.0f, p0y = 0.0f;
    curveToPlot(0.2f, 0.3f, plotSize, p0x, p0y);

    const auto exact = hitTestPoint(curve, p0x, p0y, plotSize, 5.0f);
    check(exact.has_value() && *exact == 0, "hitTestPoint: a query exactly on a point hits it");

    const auto inside = hitTestPoint(curve, p0x + 4.0f, p0y, plotSize, 5.0f);
    check(inside.has_value() && *inside == 0,
          "hitTestPoint: a query 4px from a point hits it within a 5px radius");

    const auto outside = hitTestPoint(curve, p0x + 10.0f, p0y, plotSize, 5.0f);
    check(!outside.has_value(),
          "hitTestPoint: a query 10px from every point misses within a 5px radius");

    check(!hitTestPoint(Curve{}, 0.0f, 0.0f, plotSize, 1000.0f).has_value(),
          "hitTestPoint: an empty curve always misses, regardless of radius");

    // Two points equidistant (40px) from the query -- (0.1,0.1) and
    // (0.9,0.1) on a 100px plot both sit 40px from plot-local x=50 at the
    // same y -- must resolve to the earlier index (index 0), the
    // documented tie-break.
    const Curve tie = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.9f, 0.1f}};
    const auto tieHit = hitTestPoint(tie, 50.0f, 90.0f, 100.0f, 40.0f);
    check(tieHit.has_value() && *tieHit == 0,
          "hitTestPoint: two equidistant points resolve to the earlier index");
  }

  // --- insertPoint(): scrambled insertion order stays sorted at every step ---
  {
    Curve curve;
    const size_t i0 = insertPoint(curve, 0.5f, 0.0f);
    check(i0 == 0 && isAscendingX(curve),
          "insertPoint: first point into an empty curve lands at index 0");
    const size_t i1 = insertPoint(curve, 0.1f, 0.0f);
    check(i1 == 0 && isAscendingX(curve),
          "insertPoint: a smaller x inserts before the existing point (index 0)");
    const size_t i2 = insertPoint(curve, 0.9f, 0.0f);
    check(i2 == 2 && isAscendingX(curve),
          "insertPoint: a larger x appends at the end (index 2)");
    const size_t i3 = insertPoint(curve, 0.3f, 0.0f);
    check(i3 == 1 && isAscendingX(curve),
          "insertPoint: an interior x inserts between its neighbours (index 1)");
    const size_t i4 = insertPoint(curve, 0.7f, 0.0f);
    check(i4 == 3 && isAscendingX(curve),
          "insertPoint: a second interior x still finds the correct sorted slot (index 3)");
    check(curve.size() == 5 && nearf(curve[0].x, 0.1f, 1e-4f) &&
              nearf(curve[1].x, 0.3f, 1e-4f) && nearf(curve[2].x, 0.5f, 1e-4f) &&
              nearf(curve[3].x, 0.7f, 1e-4f) && nearf(curve[4].x, 0.9f, 1e-4f),
          "insertPoint: five points inserted in scrambled order end up exactly "
          "{0.1,0.3,0.5,0.7,0.9}");

    Curve clampCurve;
    insertPoint(clampCurve, -1.0f, 5.0f);
    check(clampCurve.size() == 1 && nearf(clampCurve[0].x, 0.0f, 1e-4f) &&
              nearf(clampCurve[0].y, 1.0f, 1e-4f),
          "insertPoint: out-of-[0,1] input is clamped before insertion");
  }

  // --- movePoint(): crossing a neighbour re-sorts, a small move doesn't ---
  {
    Curve curve = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t newIdx = movePoint(curve, 0, 0.7f, 0.15f);
    check(newIdx == 1 && isAscendingX(curve),
          "movePoint: moving index 0 past its neighbour returns its new index (1)");
    check(nearf(curve[0].x, 0.5f, 1e-4f) && nearf(curve[0].y, 0.5f, 1e-4f),
          "movePoint: the point originally at index 1 is now found at index 0 "
          "(the opposite relative position)");
    check(nearf(curve[1].x, 0.7f, 1e-4f) && nearf(curve[1].y, 0.15f, 1e-4f),
          "movePoint: the moved point itself lands at its new (x,y)");
    check(nearf(curve[2].x, 0.9f, 1e-4f) && nearf(curve[2].y, 0.9f, 1e-4f),
          "movePoint: the point not involved in the crossing is untouched");

    Curve noCross = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t sameIdx = movePoint(noCross, 1, 0.55f, 0.6f);
    check(sameIdx == 1 && isAscendingX(noCross),
          "movePoint: a small move that crosses no neighbour keeps the same index");
    check(nearf(noCross[0].x, 0.1f, 1e-4f) && nearf(noCross[2].x, 0.9f, 1e-4f),
          "movePoint: ...and leaves the other two points' order untouched");

    Curve clampCurve = {CurvePoint{0.1f, 0.1f}, CurvePoint{0.5f, 0.5f}, CurvePoint{0.9f, 0.9f}};
    const size_t clampedIdx = movePoint(clampCurve, 2, -3.0f, 10.0f);
    check(clampedIdx == 0 && isAscendingX(clampCurve),
          "movePoint: an out-of-[0,1] target clamps before moving/re-sorting");
    check(nearf(clampCurve[0].x, 0.0f, 1e-4f) && nearf(clampCurve[0].y, 1.0f, 1e-4f),
          "movePoint: the clamped point lands at (0,1) as the new first entry");

    bool threw = false;
    try {
      movePoint(clampCurve, 99, 0.5f, 0.5f);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    check(threw, "movePoint: an out-of-range index throws std::out_of_range, matching "
                 "core::OpStack's own bounds-checked mutator convention");
  }

  // --- removePoint(): removes exactly the intended point, shifts the rest ---
  {
    Curve curve = {CurvePoint{0.1f, 0.0f}, CurvePoint{0.3f, 0.0f}, CurvePoint{0.5f, 0.0f},
                   CurvePoint{0.7f, 0.0f}};
    removePoint(curve, 1);
    check(curve.size() == 3 && nearf(curve[0].x, 0.1f, 1e-4f) && nearf(curve[1].x, 0.5f, 1e-4f) &&
              nearf(curve[2].x, 0.7f, 1e-4f),
          "removePoint: removes exactly the intended point, shifting later indices down by one");

    bool threw = false;
    try {
      removePoint(curve, 50);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    check(threw, "removePoint: an out-of-range index throws std::out_of_range");
  }

  // --- 0/1-point degenerate cases evalCurve() itself documents as identity
  // (ops/PointOps.hpp), checked both by curve size (this module's own
  // bookkeeping) and by feeding the CurveEdit-built curve straight into the
  // real evalCurve() as an oracle -- proving the two modules agree, not
  // just that CurveEdit's sizes look plausible. ---
  {
    Curve curve;
    check(curve.size() == 0 && nearf(evalCurve(curve, 0.37f), 0.37f, 1e-6f),
          "degenerate: an empty curve is evalCurve() identity");
    insertPoint(curve, 0.5f, 0.9f);
    check(curve.size() == 1 && nearf(evalCurve(curve, 0.37f), 0.37f, 1e-6f),
          "degenerate: a single-point curve (from insertPoint()) is still evalCurve() identity");
    removePoint(curve, 0);
    check(curve.size() == 0 && nearf(evalCurve(curve, 0.62f), 0.62f, 1e-6f),
          "degenerate: removePoint() back down to empty restores evalCurve() identity");
  }

  std::printf("[selftest] curve edit %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runExportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // The round trip under test is:
  //
  //   tile (premultiplied half) -> un-premultiply -> transfer function ->
  //   quantize to N bits -> PNG -> decodeImageLinear() (sample/max, then
  //   srgbDecode) -> compare against the tile's own stored linear value
  //
  // Comparing against the *tile's* stored value (read back through
  // Tile::readPixel, i.e. after half rounding) rather than against the float
  // literal that was written in is deliberate: half-precision storage is
  // io/ImageIO's boundary, already covered by runImageIOTest(), and leaving
  // it in would swamp the term this test is actually about. Every fixture
  // pixel used for a precision claim is fully opaque (alpha = 1.0, exact in
  // half), so the un-premultiply step is a division by exactly 1.0 and
  // contributes nothing either. What is left is exactly one lossy stage:
  //
  //   quantization of the *encoded* value to N bits, re-expanded back
  //   through the sRGB decode curve.
  //
  // Worst case is half a quantization step, amplified by the decode curve's
  // steepest slope. d(linear)/d(encoded) for sRGB peaks at encoded = 1:
  // 2.4/1.055 * ((1+0.055)/1.055)^1.4 = 2.2749. So:
  //
  //   16-bit: 0.5/65535 * 2.2749 = 1.74e-5
  //    8-bit: 0.5/255   * 2.2749 = 4.46e-3
  //
  // Both are measured for real below and printed at run time, so the numbers
  // in this comment are checkable rather than asserted: this fixture measures
  // 1.371e-5 and 3.542e-3 respectively, each comfortably under -- and of the
  // same order as -- its bound. The measurement lands below the bound because
  // 16 sampled pixels cannot be expected to hit the worst-case rounding phase
  // at the worst-case slope, which is exactly why the tolerance is set from
  // the bound rather than from the measurement: a tolerance fitted to the
  // measured number alone would be fragile against any other fixture. The
  // landed tolerances sit ~1.4x above the *derived bound* (2.5e-5 / 1.74e-5 =
  // 1.44; 6.5e-3 / 4.46e-3 = 1.46), the same headroom ratio
  // runLutBakeTest()'s kResidualTol = 2e-3 used over its own 1.46e-3.
  constexpr float kRoundTripTol16 = 2.5e-5f;
  constexpr float kRoundTripTol8 = 6.5e-3f;
  // Tolerance for values checked in the *encoded* domain (recovered as
  // srgbEncode(decoded), i.e. the literal 0..1 sample the file carries).
  // One 16-bit quantization step is 1/65535 = 1.53e-5; 1e-4 leaves ~6x
  // headroom for the srgbDecode/srgbEncode float round trip on top.
  constexpr float kEncodedDomainTol = 1e-4f;

  // Writes a *straight* (non-premultiplied) linear RGBA value into a
  // document's layer, premultiplying on the way in exactly the way
  // io/ImageIO.cpp's writeDecodedImageIntoLayer() does (rgb *= a, alpha
  // unchanged) -- so these fixtures hold what a real opened/painted document
  // holds, not a hand-tuned storage layout the export path never sees.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  // The raw premultiplied texel as the tile actually stores it -- the
  // reference every premultiply claim below is checked against, the same
  // discipline runProbeTest() used ("not just a plausible-looking number").
  auto storedPixel = [](const Document& doc, size_t layerIndex, int32_t x,
                        int32_t y) -> std::array<float, 4> {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    return t ? t->readPixel(tileLocalOffset(p)) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };
  // decodeImageLinear() always applies srgbDecode to RGB, so re-encoding a
  // decoded channel recovers the literal 0..1 sample the exported file
  // carries. That is what makes "which transfer function did the exporter
  // actually apply" directly observable, rather than inferred.
  auto sampleOf = [](float decodedLinear) { return srgbEncode(decodedLinear); };

  // --- fixture: a 4x4, fully opaque linear ramp --------------------------
  // 16 distinct values per channel spanning [0,1] including both endpoints,
  // and each channel offset from the others so a bug that swapped or copied
  // channels could not pass.
  Document ramp = Document::createBlank(4, 4, WorkingSpace{});
  for (int i = 0; i < 16; ++i) {
    const float r = static_cast<float>(i) / 15.0f;
    const float g = static_cast<float>((i * 7) % 16) / 15.0f;
    const float b = static_cast<float>((i * 11) % 16) / 15.0f;
    writeStraight(ramp, 0, i % 4, i / 4, r, g, b, 1.0f);
  }

  // --- 16-bit PNG round trip (PLAN.md Phase 4 step 1's core claim) -------
  {
    const ExportResult enc = exportDocument(ramp, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(enc.ok && enc.error.empty(),
          "exportDocument: 16-bit sRGB PNG of a 4x4 ramp encodes without error");
    if (!enc.ok) {
      std::printf("    (error was: %s)\n", enc.error.c_str());
    } else {
      check(enc.bytes.size() > 8 && enc.bytes[0] == 137 && enc.bytes[1] == 'P' &&
                enc.bytes[2] == 'N' && enc.bytes[3] == 'G',
            "exportDocument: the bytes really are a PNG (signature)");
      // IHDR's bit-depth field sits at a fixed offset in every PNG: 8-byte
      // signature + 4-byte length + 4-byte "IHDR" + 4-byte width + 4-byte
      // height = byte 24. Checked directly, so "16-bit" is a property of the
      // file rather than of this module's own bookkeeping.
      check(enc.bytes.size() > 25 && enc.bytes[24] == 16,
            "exportDocument: the file's own IHDR declares bit depth 16, not 8");

      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      check(back.valid() && back.width == 4 && back.height == 4,
            "16-bit PNG export decodes back through decodeImageLinear() at the right size");
      if (back.valid()) {
        float maxResidual = 0.0f;
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 4; ++c)
            maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
        }
        std::printf("    [measured] 16-bit round-trip max residual = %.3e (tol %.3e)\n",
                    static_cast<double>(maxResidual), static_cast<double>(kRoundTripTol16));
        check(maxResidual <= kRoundTripTol16,
              "16-bit PNG round trip: every channel of every pixel returns within the "
              "derived 16-bit quantization tolerance");
      }
    }
  }

  // --- 8-bit, same document: same code path, measurably coarser ----------
  float maxResidual8 = 0.0f;
  {
    const ExportResult enc = exportDocument(ramp, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(enc.ok, "exportDocument: 8-bit sRGB PNG of the same ramp encodes without error");
    if (enc.ok) {
      check(enc.bytes.size() > 25 && enc.bytes[24] == 8,
            "exportDocument: the 8-bit file's own IHDR declares bit depth 8");
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      if (back.valid()) {
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 4; ++c)
            maxResidual8 = std::max(maxResidual8, std::fabs(got[c] - stored[c]));
        }
        std::printf("    [measured]  8-bit round-trip max residual = %.3e (tol %.3e)\n",
                    static_cast<double>(maxResidual8), static_cast<double>(kRoundTripTol8));
        check(maxResidual8 <= kRoundTripTol8,
              "8-bit PNG round trip: within the derived 8-bit quantization tolerance");
      }
    }
  }

  // --- PRD B6, proven rather than assumed: a value that survives 16 bits
  // and is genuinely lost at 8 --------------------------------------------
  //
  // Two pixels whose sRGB-encoded values are 0.5010 and 0.5030. Both land
  // inside 8-bit code 128's bucket ([127.5/255, 128.5/255) = [0.50000,
  // 0.50392)) with margin on both sides, so an 8-bit export cannot tell them
  // apart at all; at 16 bits they are codes 32833 and 32964, 131 apart.
  {
    Document pair = Document::createBlank(2, 1, WorkingSpace{});
    const float lin0 = srgbDecode(0.5010f);
    const float lin1 = srgbDecode(0.5030f);
    writeStraight(pair, 0, 0, 0, lin0, lin0, lin0, 1.0f);
    writeStraight(pair, 0, 1, 0, lin1, lin1, lin1, 1.0f);
    check(!near(lin0, lin1, 1e-6f),
          "B6 fixture: the two source pixels genuinely differ in linear value");

    const ExportResult e8 = exportDocument(pair, ImageFormat::Png,
                                           ExportTargetSpace::Rec709Srgb,
                                           ExportBitDepth::UInt8);
    const ExportResult e16 = exportDocument(pair, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(e8.ok && e16.ok, "B6: both the 8-bit and 16-bit exports of the pair succeed");
    if (e8.ok && e16.ok) {
      const DecodedImage b8 = decodeImageLinear(e8.bytes.data(), e8.bytes.size());
      const DecodedImage b16 = decodeImageLinear(e16.bytes.data(), e16.bytes.size());
      if (b8.valid() && b16.valid()) {
        const auto p8a = pixelOf(b8, 0, 0), p8b = pixelOf(b8, 1, 0);
        const auto p16a = pixelOf(b16, 0, 0), p16b = pixelOf(b16, 1, 0);
        check(p8a[0] == p8b[0],
              "B6: at 8 bits the two distinct values collapse to the identical sample "
              "-- the precision really is gone, not merely nudged");
        check(p16a[0] != p16b[0],
              "B6: at 16 bits the same two values stay distinct");
        check(near(p16a[0], srgbDecode(0.5010f), kRoundTripTol16 * 2.0f) &&
                  near(p16b[0], srgbDecode(0.5030f), kRoundTripTol16 * 2.0f),
              "B6: and each 16-bit value comes back at its own correct level, not just "
              "'different from the other one'");
        check(std::fabs(p16a[0] - p16b[0]) > 1e-3f,
              "B6: the surviving 16-bit difference is far larger than the 16-bit "
              "quantization step, so it is signal and not noise");
      }
    }
  }

  // --- PRD B6's loud-failure half: an unsatisfiable depth request --------
  {
    const ExportResult jpeg16 = exportDocument(ramp, ImageFormat::Jpeg,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt16);
    check(!jpeg16.ok && jpeg16.bytes.empty(),
          "B6: 16-bit into JPEG fails and writes no bytes, rather than degrading to 8");
    check(contains(jpeg16.error, "JPEG") && contains(jpeg16.error, "16-bit") &&
              contains(jpeg16.error, "8 bits per channel"),
          "B6: and the error names the format, the refused depth and the real limit "
          "-- not a bare 'export failed'");
    check(contains(jpeg16.error, "PNG"),
          "B6: the error also names the format that *can* carry the request");

    const ExportResult tga16 = exportDocument(ramp, ImageFormat::Tga,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(!tga16.ok && contains(tga16.error, "TGA"), "B6: 16-bit into TGA fails by name");
    const ExportResult bmp16 = exportDocument(ramp, ImageFormat::Bmp,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(!bmp16.ok && contains(bmp16.error, "BMP"), "B6: 16-bit into BMP fails by name");
    // Control: the refusals above are about the format's own limit, not a
    // blanket "16-bit never works".
    const ExportResult png16 = exportDocument(ramp, ImageFormat::Png,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(png16.ok, "B6 control: the identical 16-bit request into PNG still succeeds");
  }

  // --- PRD I5, proven: the target space is really applied and really
  // selectable -------------------------------------------------------------
  {
    // One known linear value, 0.5 -- exactly representable in half, so the
    // expected encoded sample is exact arithmetic with nothing to round.
    Document mid = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(mid, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);

    const ExportResult lin = exportDocument(mid, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::UInt16);
    const ExportResult srgb = exportDocument(mid, ImageFormat::Png,
                                             ExportTargetSpace::Rec709Srgb,
                                             ExportBitDepth::UInt16);
    const ExportResult r709 = exportDocument(mid, ImageFormat::Png,
                                             ExportTargetSpace::Rec709Bt709,
                                             ExportBitDepth::UInt16);
    check(lin.ok && srgb.ok && r709.ok,
          "I5: the same Document exports to all three target spaces");
    check(lin.bytes != srgb.bytes && srgb.bytes != r709.bytes && lin.bytes != r709.bytes,
          "I5: the three exports are pairwise different files -- the transfer function is "
          "genuinely selectable, not a parameter that gets ignored");

    const DecodedImage bl = decodeImageLinear(lin.bytes.data(), lin.bytes.size());
    const DecodedImage bs = decodeImageLinear(srgb.bytes.data(), srgb.bytes.size());
    const DecodedImage br = decodeImageLinear(r709.bytes.data(), r709.bytes.size());
    if (bl.valid() && bs.valid() && br.valid()) {
      const float linSample = sampleOf(pixelOf(bl, 0, 0)[0]);
      const float srgbSample = sampleOf(pixelOf(bs, 0, 0)[0]);
      const float r709Sample = sampleOf(pixelOf(br, 0, 0)[0]);
      check(near(linSample, 0.5f, kEncodedDomainTol),
            "I5: Rec709Linear writes the linear value 0.5 to the file verbatim -- no "
            "transfer function applied");
      check(!near(linSample, srgbEncode(0.5f), 0.01f),
            "I5: and that linear sample is emphatically NOT srgbEncode(0.5) ~ 0.735 -- the "
            "no-encode option is verifiably not silently sRGB-encoding");
      check(near(srgbSample, srgbEncode(0.5f), kEncodedDomainTol),
            "I5: Rec709Srgb writes srgbEncode(0.5) -- color/Space's own curve, not an "
            "approximation of it");
      check(near(r709Sample, rec709Encode(0.5f), kEncodedDomainTol),
            "I5: Rec709Bt709 writes rec709Encode(0.5)");
      check(!near(srgbSample, r709Sample, 1e-3f),
            "I5: sRGB and BT.709 land on genuinely different samples -- the two curves are "
            "not being conflated because their primaries match");
    }
  }

  // --- Premultiply: undone correctly, checked against the tile's own raw
  // premultiplied storage --------------------------------------------------
  {
    Document alpha = Document::createBlank(3, 1, WorkingSpace{});
    // (0,0): translucent. (1,0): deliberately never written -- fully
    // transparent, exercising the a <= 0 guard. (2,0): opaque control.
    writeStraight(alpha, 0, 0, 0, 0.8f, 0.4f, 0.2f, 0.5f);
    writeStraight(alpha, 0, 2, 0, 0.25f, 0.5f, 0.75f, 1.0f);

    const std::array<float, 4> raw = storedPixel(alpha, 0, 0, 0);
    check(raw[3] > 0.0f && raw[0] < 0.5f,
          "premultiply fixture: the tile really stores rgb*a (red 0.8*0.5 ~ 0.4), so "
          "un-premultiplying is something the export path has to actually do");

    // Rec709Linear so the check is pure premultiply arithmetic with no
    // transfer function standing between the tile and the file sample.
    const ExportResult enc = exportDocument(alpha, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::UInt16);
    check(enc.ok, "premultiply: a document with alpha < 1 exports to 16-bit linear PNG");
    if (enc.ok) {
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      if (back.valid()) {
        const auto got = pixelOf(back, 0, 0);
        // The expectation is derived from the tile's own stored values, not
        // from the 0.8/0.4/0.2 literals -- so this checks the export path,
        // not the fixture.
        const float expR = raw[0] / raw[3], expG = raw[1] / raw[3], expB = raw[2] / raw[3];
        check(near(sampleOf(got[0]), expR, kEncodedDomainTol) &&
                  near(sampleOf(got[1]), expG, kEncodedDomainTol) &&
                  near(sampleOf(got[2]), expB, kEncodedDomainTol),
              "premultiply: the exported RGB is exactly the tile's stored rgb divided by "
              "its stored alpha");
        check(near(got[3], raw[3], kRoundTripTol16),
              "premultiply: alpha itself is written straight through, un-curved and "
              "un-divided");
        check(!near(sampleOf(got[0]), raw[0], 0.05f),
              "premultiply: the exported red genuinely differs from the raw premultiplied "
              "value -- proves un-premultiply ran, not just alpha passthrough");

        const auto empty = pixelOf(back, 1, 0);
        check(near(empty[0], 0.0f, 1e-6f) && near(empty[1], 0.0f, 1e-6f) &&
                  near(empty[2], 0.0f, 1e-6f) && near(empty[3], 0.0f, 1e-6f),
              "premultiply: a never-painted texel exports as transparent black (the "
              "a <= 0 guard core/Probe.cpp uses), not as a divide-by-zero");

        const std::array<float, 4> rawOpaque = storedPixel(alpha, 0, 2, 0);
        const auto opaque = pixelOf(back, 2, 0);
        check(near(sampleOf(opaque[0]), rawOpaque[0], kEncodedDomainTol) &&
                  near(opaque[3], 1.0f, kRoundTripTol16),
              "premultiply: an alpha=1 texel is unchanged by the division (control)");
      }
    }
  }

  // --- The primaries scope decision, enforced rather than documented-only -
  {
    Document wide = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(wide, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    // ACEScg's red primary (x = 0.713), i.e. a genuinely different gamut --
    // not a rounding-level perturbation.
    wide.workingSpace.primaries.redX = 0.713f;
    wide.workingSpace.primaries.redY = 0.293f;

    const ExportResult enc = exportDocument(wide, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(!enc.ok && enc.bytes.empty(),
          "primaries: a working space whose primaries differ from the target's is refused, "
          "not silently exported as if it matched");
    check(contains(enc.error, "primaries") && contains(enc.error, "0.7130"),
          "primaries: the error names the mismatch and quotes the offending coordinate");
    check(contains(enc.error, "transfer functions only"),
          "primaries: and says why -- this build converts transfer functions, not gamuts");

    // The check is a real comparison, not a blanket rejection: the same
    // document with matching primaries exports fine.
    Document matched = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(matched, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    check(exportDocument(matched, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                         ExportBitDepth::UInt16)
              .ok,
          "primaries control: the default Rec.709 working space exports without complaint");
  }

  // --- JPEG has no alpha channel: refused by name, not silently dropped ---
  {
    Document translucent = Document::createBlank(2, 1, WorkingSpace{});
    writeStraight(translucent, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    writeStraight(translucent, 0, 1, 0, 0.5f, 0.5f, 0.5f, 0.25f);

    const ExportResult enc = exportDocument(translucent, ImageFormat::Jpeg,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(!enc.ok, "alpha: a translucent document is refused for JPEG, which has no alpha");
    check(contains(enc.error, "no alpha channel") && contains(enc.error, "x=1, y=0"),
          "alpha: the error names the format's limitation and the first offending pixel");

    // The same document exports fine to the three formats that do carry
    // alpha -- so the refusal is about JPEG, not about alpha in general.
    check(exportDocument(translucent, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                         ExportBitDepth::UInt8)
                  .ok &&
              exportDocument(translucent, ImageFormat::Tga, ExportTargetSpace::Rec709Srgb,
                             ExportBitDepth::UInt8)
                  .ok &&
              exportDocument(translucent, ImageFormat::Bmp, ExportTargetSpace::Rec709Srgb,
                             ExportBitDepth::UInt8)
                  .ok,
          "alpha control: PNG/TGA/BMP accept the same translucent document");
  }

  // --- PRD I1's write half: all four formats actually produce a file that
  // decodes back ------------------------------------------------------------
  {
    // Uniform, fully opaque mid-grey over 8x8 -- uniform specifically so
    // JPEG's block transform has nothing to ring on and its own lossiness
    // stays a quantization question rather than a spatial one.
    Document flat = Document::createBlank(8, 8, WorkingSpace{});
    for (int32_t y = 0; y < 8; ++y)
      for (int32_t x = 0; x < 8; ++x) writeStraight(flat, 0, x, y, 0.5f, 0.25f, 0.75f, 1.0f);
    const std::array<float, 4> stored = storedPixel(flat, 0, 4, 4);

    struct Case { ImageFormat format; const char* name; float tol; };
    // PNG/TGA/BMP are lossless containers at 8 bits, so they get the derived
    // 8-bit quantization tolerance. JPEG is lossy by construction and gets a
    // deliberately looser one -- calling that out rather than quietly using
    // one tolerance for all four.
    const Case cases[] = {
        {ImageFormat::Png, "PNG", kRoundTripTol8},
        {ImageFormat::Tga, "TGA", kRoundTripTol8},
        {ImageFormat::Bmp, "BMP", kRoundTripTol8},
        {ImageFormat::Jpeg, "JPEG", 0.02f},
    };
    for (const Case& c : cases) {
      const ExportResult enc =
          exportDocument(flat, c.format, ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
      char label[96];
      std::snprintf(label, sizeof(label), "I1: 8-bit %s export round-trips through the decoder",
                    c.name);
      bool caseOk = enc.ok;
      if (enc.ok) {
        const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
        caseOk = back.valid() && back.width == 8 && back.height == 8;
        if (caseOk) {
          const auto got = pixelOf(back, 4, 4);
          caseOk = near(got[0], stored[0], c.tol) && near(got[1], stored[1], c.tol) &&
                   near(got[2], stored[2], c.tol);
        }
      }
      check(caseOk, label);
    }
  }

  // --- flattenDocumentToLinear on its own ---------------------------------
  {
    // Content outside the canvas rectangle is clipped away: export writes
    // the document's canvas, not its content's bounding box.
    Document offCanvas = Document::createBlank(4, 4, WorkingSpace{});
    writeStraight(offCanvas, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    writeStraight(offCanvas, 0, 200, 200, 1.0f, 0.0f, 0.0f, 1.0f);  // a whole tile away
    const DecodedImage flat = flattenDocumentToLinear(offCanvas);
    check(flat.valid() && flat.width == 4 && flat.height == 4,
          "flattenDocumentToLinear: the result is exactly the canvas size");
    if (flat.valid()) {
      check(near(pixelOf(flat, 0, 0)[3], 1.0f, 1e-6f) &&
                near(pixelOf(flat, 3, 3)[3], 0.0f, 1e-6f),
            "flattenDocumentToLinear: in-canvas content survives, out-of-canvas content is "
            "clipped rather than wrapped or resized into view");
    }

    // Two layers, disjoint content -- the plain-sum path core/Probe.cpp's
    // sampleAllLayers already documents. Both layers' pixels must appear.
    Document twoLayers = Document::createBlank(2, 1, WorkingSpace{});
    writeStraight(twoLayers, 0, 0, 0, 1.0f, 0.0f, 0.0f, 1.0f);
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    twoLayers.layers.push_back(std::move(second));
    writeStraight(twoLayers, 1, 1, 0, 0.0f, 0.0f, 1.0f, 1.0f);
    const DecodedImage both = flattenDocumentToLinear(twoLayers);
    check(both.valid() && near(pixelOf(both, 0, 0)[0], 1.0f, 1e-3f) &&
              near(pixelOf(both, 1, 0)[2], 1.0f, 1e-3f),
          "flattenDocumentToLinear: every RGB layer contributes (the plain sum core/Probe "
          "already uses -- no compositing model exists to do better yet)");

    // A blank document is a legitimate thing to export: fully transparent,
    // not an error.
    const Document blank = Document::createBlank(2, 2, WorkingSpace{});
    const ExportResult blankEnc = exportDocument(blank, ImageFormat::Png,
                                                 ExportTargetSpace::Rec709Srgb,
                                                 ExportBitDepth::UInt16);
    check(blankEnc.ok, "export: a createBlank()'d document with zero painted tiles exports "
                       "successfully rather than erroring");
    if (blankEnc.ok) {
      const DecodedImage back = decodeImageLinear(blankEnc.bytes.data(), blankEnc.bytes.size());
      check(back.valid() && near(pixelOf(back, 0, 0)[3], 0.0f, 1e-6f),
            "export: and it comes back fully transparent");
    }

    // A zero-sized document has nothing to write, and says so.
    Document empty;
    const ExportResult emptyEnc = exportDocument(empty, ImageFormat::Png,
                                                 ExportTargetSpace::Rec709Srgb,
                                                 ExportBitDepth::UInt8);
    check(!emptyEnc.ok && contains(emptyEnc.error, "no pixels to export"),
          "export: a zero-sized document fails with a specific error, not a crash or an "
          "empty file");
  }

  // --- exportDocumentToFile: same bytes, and failures never touch disk ----
  {
    const char* path = "selftest_export.png";
    std::remove(path);
    std::string err;
    const bool wrote = exportDocumentToFile(ramp, path, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16, &err);
    check(wrote && err.empty(), "exportDocumentToFile: writes a 16-bit PNG to disk");
    if (wrote) {
      std::FILE* f = std::fopen(path, "rb");
      std::vector<uint8_t> fileBytes;
      if (f) {
        uint8_t buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) fileBytes.insert(fileBytes.end(), buf, buf + n);
        std::fclose(f);
      }
      const ExportResult inMemory = exportDocument(ramp, ImageFormat::Png,
                                                   ExportTargetSpace::Rec709Srgb,
                                                   ExportBitDepth::UInt16);
      check(inMemory.ok && fileBytes == inMemory.bytes,
            "exportDocumentToFile: the file on disk is byte-identical to exportDocument()'s "
            "in-memory result -- one encode path, not two");
    }
    std::remove(path);

    const char* refusedPath = "selftest_export_refused.jpg";
    std::remove(refusedPath);
    std::string refusedErr;
    const bool refused = exportDocumentToFile(ramp, refusedPath, ImageFormat::Jpeg,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16, &refusedErr);
    check(!refused && contains(refusedErr, "JPEG"),
          "exportDocumentToFile: a refused request forwards the encode's own specific error");
    std::FILE* shouldNotExist = std::fopen(refusedPath, "rb");
    check(shouldNotExist == nullptr,
          "exportDocumentToFile: and leaves no file behind -- nothing is opened until the "
          "encode has fully succeeded");
    if (shouldNotExist) {
      std::fclose(shouldNotExist);
      std::remove(refusedPath);
    }
  }

  std::printf("[selftest] export %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runFormatSupportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Which backend set was compiled in. A plain constant rather than an
  // #ifdef around each case, so both configurations execute the same
  // assertions and each one states the correct answer for its build -- see
  // SelfTest.hpp on why compiling this section out of the OFF build would
  // defeat its purpose.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif
  check(oiioBackendCompiledIn() == kOiioBuild,
        "the capability module and this test agree on which build this is");
  std::printf("    %s\n", imageBackendSummary().c_str());

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // Three genuinely different storage models are round-tripped below, and
  // they get three separately derived numbers rather than one number that
  // covers the worst of them.
  //
  // (1) **Half-float, encoded domain.** IEEE binary16 has 10 stored mantissa
  //     bits, so for a value v in [2^e, 2^(e+1)) one ulp is 2^(e-10) and the
  //     worst rounding error is half of that: at most v * 2^-11. For the
  //     [0,1] samples written here that bounds the absolute error at
  //     2^-11 = 4.883e-4. Landed 7.0e-4, ~1.43x the bound -- the same
  //     headroom ratio runLutBakeTest()'s kResidualTol = 2e-3 used over its
  //     own measured 1.46e-3, and step 1's kRoundTripTol16 over its 1.74e-5.
  constexpr float kHalfEncodedTol = 7.0e-4f;
  // (2) **16-bit integer, linear domain.** Identical in every term to
  //     runExportTest()'s own kRoundTripTol16 and re-derived here rather
  //     than shared, because it is being applied to different formats
  //     (TIFF, DPX) through a different encoder: half a quantization step,
  //     0.5/65535, amplified by the sRGB decode curve's steepest slope,
  //     2.4/1.055 * ((1+0.055)/1.055)^1.4 = 2.2749, giving 1.736e-5. Landed
  //     2.5e-5, 1.44x.
  constexpr float kInteger16Tol = 2.5e-5f;
  // (3) **Radiance RGBE (HDR), linear domain.** A fundamentally coarser
  //     storage than either: three 8-bit mantissas sharing one 8-bit
  //     exponent, so a channel's precision is set by the *largest* channel
  //     in its pixel. One mantissa step is 1/256 of the shared scale, and
  //     the shared scale is the smallest power of two above the largest
  //     channel -- so for a pixel whose largest channel is m, the absolute
  //     error on any channel is bounded by 2m/256 = m/128 (a full step, not
  //     half of one: RGBE encoders truncate rather than round to nearest).
  //     The fixture below peaks at m = 1.0, giving a bound of 7.8125e-3;
  //     measured 7.324e-3 -- close enough to the bound to confirm the model
  //     rather than merely not contradict it -- and landed 1.1e-2, 1.41x the
  //     bound and the same headroom ratio as the other two. Deliberately
  //     a separate, separately-labelled constant rather than folding HDR
  //     into the others: using one tolerance for RGBE and half would hide
  //     the fact that RGBE is ~16x coarser.
  constexpr float kRgbeTol = 1.1e-2f;

  // Same fixture helpers runExportTest() uses, and for the same reason:
  // writing *straight* linear RGBA through io/ImageIO.cpp's own `rgb *= a`
  // premultiply means the documents under test hold what a real
  // opened/painted document holds, and every precision claim is checked
  // against the tile's own post-half-rounding stored value rather than the
  // float literal that went in.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto storedPixel = [](const Document& doc, size_t layerIndex, int32_t x,
                        int32_t y) -> std::array<float, 4> {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    return t ? t->readPixel(tileLocalOffset(p)) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };

  // The shared 4x4 opaque ramp: 16 distinct values per channel spanning
  // [0,1] including both endpoints, each channel offset from the others so
  // a bug that swapped or copied channels could not pass.
  Document ramp = Document::createBlank(4, 4, WorkingSpace{});
  for (int i = 0; i < 16; ++i) {
    writeStraight(ramp, 0, i % 4, i / 4, static_cast<float>(i) / 15.0f,
                  static_cast<float>((i * 7) % 16) / 15.0f,
                  static_cast<float>((i * 11) % 16) / 15.0f, 1.0f);
  }

  // --- PRD I1's four: identical answers in both configurations ------------
  {
    struct Expect { ImageFormat format; bool alpha; bool uint8; bool uint16; };
    const Expect expects[] = {
        {ImageFormat::Png, true, true, true},
        {ImageFormat::Jpeg, false, true, false},
        {ImageFormat::Tga, true, true, false},
        {ImageFormat::Bmp, true, true, false},
    };
    bool available = true, stbBacked = true, depths = true, alpha = true, noFloat = true;
    for (const Expect& e : expects) {
      const FormatCapability& c = formatCapability(e.format);
      available = available && c.canRead && c.canWrite && c.unavailableReason.empty();
      stbBacked = stbBacked && c.backend == FormatBackend::Stb;
      depths = depths && c.canWriteDepth(ExportBitDepth::UInt8) == e.uint8 &&
               c.canWriteDepth(ExportBitDepth::UInt16) == e.uint16;
      alpha = alpha && c.hasAlpha == e.alpha;
      noFloat = noFloat && !c.canWriteDepth(ExportBitDepth::Half) &&
                !c.canWriteDepth(ExportBitDepth::Float32);
    }
    check(available, "I1: PNG/JPEG/TGA/BMP are readable and writable, with no caveat");
    check(stbBacked,
          "I1: and all four are stb-backed in BOTH configurations -- OpenImageIO never "
          "intercepts them, so it cannot regress them");
    check(depths, "I1: PNG carries 8 and 16-bit integer; JPEG/TGA/BMP carry 8 only");
    check(alpha, "I1: PNG/TGA/BMP carry alpha, JPEG does not");
    check(noFloat,
          "B6: none of the four integer formats claims a half or 32-bit-float depth");
  }

  // --- The OIIO-backed four: available exactly when the backend is --------
  {
    struct Expect {
      ImageFormat format;
      bool alpha;
      bool uint8, uint16, half, float32;
    };
    // Every `false` here for an otherwise-plausible depth is a case where
    // OpenImageIO accepts the request and writes something else: EXR+UINT8
    // and EXR+UINT16 -> half, TIFF+HALF and DPX+HALF -> float,
    // HDR+anything-but-FLOAT -> float. io/Capabilities probes for the
    // substitution and reports the depth unwritable, so these are `false`.
    //
    // Two of these rows were wrong when this test was first written -- I had
    // TIFF accepting half and DPX refusing 32-bit float, both from memory of
    // what those formats "usually" do. The runtime probe disagreed and the
    // probe was right (OpenImageIO's TIFF writer substitutes float for half;
    // its DPX writer genuinely writes 32-bit float, which the DPX spec's R32
    // element does allow). Left recorded here because it is the concrete
    // argument for PRD I3's wording: a hand-maintained table would have
    // shipped both mistakes, and the only reason this one is right is that
    // nobody is maintaining it -- the numbers below are transcribed from
    // what the linked library actually did.
    const Expect expects[] = {
        {ImageFormat::Exr, true, false, false, true, true},
        {ImageFormat::Tiff, true, true, true, false, true},
        {ImageFormat::Hdr, false, false, false, false, true},
        {ImageFormat::Dpx, true, true, true, false, true},
    };
    for (const Expect& e : expects) {
      const FormatCapability& c = formatCapability(e.format);
      char label[128];
      std::snprintf(label, sizeof(label),
                    "%s is readable+writable exactly when the OIIO backend is present",
                    imageFormatName(e.format));
      check(c.canRead == kOiioBuild && c.canWrite == kOiioBuild, label);
      std::snprintf(label, sizeof(label), "%s reports the right backend for this build",
                    imageFormatName(e.format));
      check(c.backend == (kOiioBuild ? FormatBackend::Oiio : FormatBackend::None), label);

      std::snprintf(label, sizeof(label), "%s: the writable depth set matches this build",
                    imageFormatName(e.format));
      check(c.canWriteDepth(ExportBitDepth::UInt8) == (kOiioBuild && e.uint8) &&
                c.canWriteDepth(ExportBitDepth::UInt16) == (kOiioBuild && e.uint16) &&
                c.canWriteDepth(ExportBitDepth::Half) == (kOiioBuild && e.half) &&
                c.canWriteDepth(ExportBitDepth::Float32) == (kOiioBuild && e.float32),
            label);
      std::snprintf(label, sizeof(label), "%s: alpha-channel support matches this build",
                    imageFormatName(e.format));
      check(c.hasAlpha == (kOiioBuild && e.alpha), label);

      if (!kOiioBuild) {
        std::snprintf(label, sizeof(label),
                      "%s: the OFF build's reason names NP_USE_OIIO, not a bare failure",
                      imageFormatName(e.format));
        check(contains(c.unavailableReason, "NP_USE_OIIO") &&
                  contains(c.unavailableReason, imageFormatName(e.format)),
              label);
      } else {
        std::snprintf(label, sizeof(label), "%s: available, so it carries no failure reason",
                      imageFormatName(e.format));
        check(c.unavailableReason.empty(), label);
      }
    }
    // HDR having no alpha is not written down anywhere -- it is discovered
    // by OpenImageIO's writer refusing to open a 4-channel image. Called
    // out separately because it is the clearest single case of a capability
    // that a hand-maintained table would have got wrong.
    check(formatCapability(ImageFormat::Hdr).hasAlpha == false,
          "HDR reports no alpha channel (Radiance RGBE is 3-channel by definition)");
  }

  // --- PSD: read-only where it exists at all ------------------------------
  {
    const FormatCapability& psd = formatCapability(ImageFormat::Psd);
    check(psd.canRead == kOiioBuild,
          "PSD is readable exactly when the OIIO backend is present (flattened read is "
          "PLAN.md step 2's actual wording)");
    check(!psd.canWrite,
          "PSD is NOT writable in either build -- PSD export is phase 15, and this "
          "OpenImageIO has no PSD writer at all");
    const ExportResult psdExport = exportDocument(ramp, ImageFormat::Psd,
                                                  ExportTargetSpace::Rec709Srgb,
                                                  ExportBitDepth::UInt8);
    check(!psdExport.ok && psdExport.bytes.empty(),
          "PSD export is refused and writes no bytes");
    check(contains(psdExport.error, "PSD") &&
              (contains(psdExport.error, "phase 15") ||
               contains(psdExport.error, "NP_USE_OIIO")),
          "PSD export's refusal names the format and why -- read-only here, or no backend");
  }

  // --- Camera raw: unsupported in BOTH builds. The I3 assertion. ----------
  {
    const FormatCapability& raw = formatCapability(ImageFormat::CameraRaw);
    check(!raw.canRead && !raw.canWrite,
          "camera raw is unsupported in this build -- INCLUDING the OIIO build, which is "
          "the assertion a hardcoded 'NP_USE_OIIO implies step 2's list' table would fail");
    check(raw.backend == FormatBackend::None, "camera raw reports no backend at all");
    check(!raw.unavailableReason.empty(), "camera raw's refusal carries a reason");
    if (kOiioBuild) {
      check(contains(raw.unavailableReason, "LibRaw"),
            "camera raw's reason names LibRaw's deliberate exclusion from this "
            "OpenImageIO build");
      check(contains(raw.unavailableReason, "run time") &&
                contains(raw.unavailableReason, "'raw'"),
            "camera raw's reason says the answer came from asking OpenImageIO at run "
            "time, and names the plugin it looked for");
    } else {
      check(contains(raw.unavailableReason, "NP_USE_OIIO"),
            "camera raw's reason names NP_USE_OIIO in the no-backend build");
    }
    check(formatCapability(ImageFormat::Exr).canRead == kOiioBuild &&
              !formatCapability(ImageFormat::CameraRaw).canRead,
          "I3: the query distinguishes two OIIO-listed formats from each other -- EXR "
          "present, camera raw absent -- rather than answering per build option");
  }

  // --- formatsThatCanWriteDepth(): the answer used to build refusals ------
  {
    const std::string u8 = formatsThatCanWriteDepth(ExportBitDepth::UInt8);
    const std::string u16 = formatsThatCanWriteDepth(ExportBitDepth::UInt16);
    const std::string h = formatsThatCanWriteDepth(ExportBitDepth::Half);
    const std::string f32 = formatsThatCanWriteDepth(ExportBitDepth::Float32);
    std::printf("    [query] 8-bit integer:  %s\n", u8.c_str());
    std::printf("    [query] 16-bit integer: %s\n", u16.c_str());
    std::printf("    [query] 16-bit half:    %s\n", h.empty() ? "(none)" : h.c_str());
    std::printf("    [query] 32-bit float:   %s\n", f32.empty() ? "(none)" : f32.c_str());
    check(contains(u8, "PNG") && contains(u8, "JPEG") && contains(u8, "TGA") &&
              contains(u8, "BMP"),
          "query: all four I1 formats are listed as 8-bit-writable");
    check(contains(u16, "PNG") && !contains(u16, "JPEG"),
          "query: PNG is listed as 16-bit-integer-writable and JPEG is not");
    check(f32.empty() != kOiioBuild,
          "query: a 32-bit-float-capable format exists exactly when the OIIO backend does");
    if (kOiioBuild) {
      check(h == "EXR",
            "query: EXR is the ONLY format here that writes half -- TIFF and DPX accept "
            "the request and write 32-bit float instead, so they are not listed");
      check(contains(f32, "EXR") && contains(f32, "TIFF") && contains(f32, "HDR") &&
                contains(f32, "DPX") && !contains(f32, "PNG"),
            "query: all four OIIO formats write 32-bit float, and none of the I1 four do");
    } else {
      check(h.empty(), "query: no format writes half without the OIIO backend");
    }
  }

  // --- PRD B6 for the float depths: refused loudly by an integer format ---
  // Runs identically in both builds -- these four formats are stb-backed
  // either way, so this is not an OIIO-conditional claim.
  {
    const ImageFormat integerOnly[] = {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Tga,
                                       ImageFormat::Bmp};
    bool allRefused = true, allNamed = true;
    for (ImageFormat f : integerOnly) {
      const ExportResult r = exportDocument(ramp, f, ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::Float32);
      allRefused = allRefused && !r.ok && r.bytes.empty();
      allNamed = allNamed && contains(r.error, imageFormatName(f)) &&
                 contains(r.error, "32-bit float") && contains(r.error, "PRD B6");
    }
    check(allRefused, "B6: a 32-bit-float request into PNG/JPEG/TGA/BMP writes no bytes");
    check(allNamed,
          "B6: and each error names the format, the refused depth and PRD B6 -- not a "
          "bare 'export failed'");

    const ExportResult halfPng = exportDocument(ramp, ImageFormat::Png,
                                                ExportTargetSpace::Rec709Linear,
                                                ExportBitDepth::Half);
    check(!halfPng.ok && contains(halfPng.error, "16-bit half float"),
          "B6: half into PNG is refused by its full name -- 'sixteen bits' is not "
          "ambiguous here between integer and half");
    // Control: the same PNG accepts the 16-bit *integer* request, so the
    // refusal above is about half specifically and not about 16 bits.
    check(exportDocument(ramp, ImageFormat::Png, ExportTargetSpace::Rec709Linear,
                         ExportBitDepth::UInt16)
              .ok,
          "B6 control: PNG still accepts 16-bit integer, so the half refusal is about "
          "the sample type and not the bit count");
  }

  // --- The no-backend build's loud failure --------------------------------
  {
    const ExportResult exr = exportDocument(ramp, ImageFormat::Exr,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::Half);
    check(exr.ok == kOiioBuild,
          "EXR export succeeds exactly when the OIIO backend is compiled in");
    if (!kOiioBuild) {
      check(contains(exr.error, "EXR") && contains(exr.error, "NP_USE_OIIO") &&
                contains(exr.error, "-DNP_USE_OIIO=ON"),
            "no-backend build: the EXR refusal names the format, the build option and the "
            "exact flag to turn it on");
      check(contains(exr.error, "PRD I3"),
            "no-backend build: and says this is PRD I3 working, not a defect");
    }
  }

  // Everything past this point needs a real backend. Guarded by a plain
  // runtime `if`, not an #ifdef: every call below compiles in both
  // configurations (the OIIO types never cross into this file), so the OFF
  // build still type-checks all of it.
  if (kOiioBuild) {
    // --- EXR round trip: exactly lossless, and why that is the right claim
    //
    // The chain is: tile (half) -> readPixel (exact) -> flatten's sum of a
    // single contribution (exact) -> un-premultiply by an alpha of exactly
    // 1.0 (exact) -> re-associate by the same 1.0 (exact) -> Rec709Linear,
    // i.e. no transfer function (exact) -> no clamp, because the depth is
    // float -> float-to-half, of a value that *is already a half* (exact).
    // Not one stage of that rounds, so the correct assertion is equality,
    // not a tolerance -- and it is the same claim docs/document-format.md
    // makes for the native container: "HALF channels -- byte-identical, no
    // conversion". A tolerance here would let a real regression through.
    {
      const ExportResult half = exportDocument(ramp, ImageFormat::Exr,
                                               ExportTargetSpace::Rec709Linear,
                                               ExportBitDepth::Half);
      check(half.ok && half.error.empty(), "EXR: a 4x4 ramp encodes to half without error");
      if (half.ok) {
        check(half.bytes.size() > 4 && half.bytes[0] == 0x76 && half.bytes[1] == 0x2f &&
                  half.bytes[2] == 0x31 && half.bytes[3] == 0x01,
              "EXR: the bytes really are an OpenEXR file (magic 0x76 0x2f 0x31 0x01)");
        // Decoded through the *production* entry point, which is what proves
        // io/ImageDecode's OpenImageIO fallback is wired up rather than just
        // written.
        const DecodedImage back = decodeImageLinear(half.bytes.data(), half.bytes.size());
        check(back.valid() && back.width == 4 && back.height == 4,
              "EXR: decodeImageLinear() reads it back at the right size -- the stb-first, "
              "OpenImageIO-fallback path works through the existing entry point");
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int c = 0; c < 4; ++c)
              maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
          }
          std::printf("    [measured] EXR half   linear round-trip max residual = %.3e "
                      "(expected exactly 0)\n",
                      static_cast<double>(maxResidual));
          check(maxResidual == 0.0f,
                "EXR half: every channel of every pixel returns bit-exact -- the working "
                "space is already half, so a linear EXR loses nothing at all");
        }
      }

      const ExportResult f32 = exportDocument(ramp, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      check(f32.ok, "EXR: the same ramp encodes to 32-bit float");
      if (f32.ok) {
        check(f32.bytes.size() > half.bytes.size(),
              "EXR: the 32-bit-float file is larger than the half one -- the depth "
              "parameter reaches the actual writer");
        const DecodedImage back = decodeImageLinear(f32.bytes.data(), f32.bytes.size());
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int c = 0; c < 4; ++c)
              maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
          }
          std::printf("    [measured] EXR float  linear round-trip max residual = %.3e "
                      "(expected exactly 0)\n",
                      static_cast<double>(maxResidual));
          check(maxResidual == 0.0f, "EXR 32-bit float: bit-exact round trip as well");
        }
      }
    }

    // --- The target space still reaches an EXR (PRD I5) -------------------
    //
    // Checked in the *encoded* domain: an sRGB-encoded EXR is a float file,
    // so the decoder correctly does not apply a curve to it, and what comes
    // back is the literal file sample. That makes "which transfer function
    // did the exporter apply" directly observable here, exactly as
    // runExportTest()'s own I5 case makes it observable for PNG.
    {
      const ExportResult lin = exportDocument(ramp, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Half);
      const ExportResult srgb = exportDocument(ramp, ImageFormat::Exr,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::Half);
      check(lin.ok && srgb.ok && lin.bytes != srgb.bytes,
            "I5: EXR honours the target space -- linear and sRGB produce different files");
      const DecodedImage back = decodeImageLinear(srgb.bytes.data(), srgb.bytes.size());
      if (back.valid()) {
        float maxResidual = 0.0f;
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 3; ++c)
            maxResidual = std::max(maxResidual, std::fabs(got[c] - srgbEncode(stored[c])));
        }
        std::printf("    [measured] EXR half   sRGB sample residual         = %.3e "
                    "(tol %.3e)\n",
                    static_cast<double>(maxResidual), static_cast<double>(kHalfEncodedTol));
        check(maxResidual <= kHalfEncodedTol,
              "I5: each sRGB EXR sample is srgbEncode() of the stored value, within the "
              "derived half-float tolerance");
      }
    }

    // --- 32-bit float is genuinely deeper than half (PRD B6) --------------
    //
    // Driven through encodeLinearImage() with a hand-built DecodedImage
    // rather than a Document, deliberately: the working space's tiles are
    // themselves half, so a value that half cannot represent could not
    // survive long enough to reach the exporter from a Document. This is
    // the one place the 32-bit path's extra precision is actually
    // observable, so it is tested where it is observable.
    {
      DecodedImage img;
      img.width = 1;
      img.height = 1;
      img.pixels = {0.1f, 0.1f, 0.1f, 1.0f};  // 0.1 is not representable in half
      const ExportResult asHalf =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Half);
      const ExportResult asFloat =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Float32);
      check(asHalf.ok && asFloat.ok, "B6: 0.1 encodes to both half and 32-bit-float EXR");
      if (asHalf.ok && asFloat.ok) {
        const DecodedImage h = decodeImageLinear(asHalf.bytes.data(), asHalf.bytes.size());
        const DecodedImage f = decodeImageLinear(asFloat.bytes.data(), asFloat.bytes.size());
        if (h.valid() && f.valid()) {
          const float hv = pixelOf(h, 0, 0)[0], fv = pixelOf(f, 0, 0)[0];
          std::printf("    [measured] 0.1f -> half %.9f (err %.3e), float %.9f (err %.3e)\n",
                      static_cast<double>(hv), static_cast<double>(std::fabs(hv - 0.1f)),
                      static_cast<double>(fv), static_cast<double>(std::fabs(fv - 0.1f)));
          check(fv == 0.1f,
                "B6: the 32-bit-float file returns 0.1 bit-exact -- 32 bits really are "
                "32 bits, not 16 rounded up");
          check(hv != 0.1f && near(hv, 0.1f, 1e-4f),
                "B6: the half file returns a close but genuinely different value, so the "
                "two depths are not the same thing wearing different names");
        }
      }
    }

    // --- Values above 1.0: kept by a float depth, clipped by an integer ---
    {
      DecodedImage img;
      img.width = 1;
      img.height = 1;
      img.pixels = {4.0f, 12.5f, 1000.0f, 1.0f};  // all exact in half
      const ExportResult exr =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Half);
      const ExportResult png =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Png,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::UInt16);
      check(exr.ok && png.ok, "headroom: the same >1.0 pixel encodes to both EXR and PNG");
      if (exr.ok && png.ok) {
        const DecodedImage be = decodeImageLinear(exr.bytes.data(), exr.bytes.size());
        const DecodedImage bp = decodeImageLinear(png.bytes.data(), png.bytes.size());
        if (be.valid() && bp.valid()) {
          const auto e = pixelOf(be, 0, 0);
          const auto p = pixelOf(bp, 0, 0);
          check(e[0] == 4.0f && e[1] == 12.5f && e[2] == 1000.0f,
                "headroom: EXR keeps 4.0 / 12.5 / 1000.0 exactly -- the [0,1] clamp is "
                "keyed to the depth, not applied blindly");
          check(near(p[0], 1.0f, 1e-4f) && near(p[1], 1.0f, 1e-4f) &&
                    near(p[2], 1.0f, 1e-4f),
                "headroom: 16-bit-integer PNG clips all three to full scale, which is a "
                "property of asking for an integer file, not a depth truncation");
        }
      }
    }

    // --- Associated alpha: the two conversions agree ----------------------
    //
    // EXR is written with alpha associated (premultiplied) and read back
    // un-associated; TIFF is written and read straight. Exporting the same
    // translucent document to both and getting the same answer is what a
    // *paired* conversion looks like -- if either half were missing, the
    // EXR result would be off by a factor of alpha (2x here), which this
    // comparison would miss by three orders of magnitude. It does not prove
    // the file's samples are premultiplied (nothing reachable from here can
    // read the raw samples), and is not claimed to.
    {
      Document alpha = Document::createBlank(3, 1, WorkingSpace{});
      writeStraight(alpha, 0, 0, 0, 0.8f, 0.4f, 0.2f, 0.5f);
      // (1,0) deliberately never written -- fully transparent, exercising
      // the a <= 0 guard on both the associate and un-associate sides.
      writeStraight(alpha, 0, 2, 0, 0.25f, 0.5f, 0.75f, 1.0f);

      const ExportResult exr = exportDocument(alpha, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      const ExportResult tiff = exportDocument(alpha, ImageFormat::Tiff,
                                               ExportTargetSpace::Rec709Linear,
                                               ExportBitDepth::Float32);
      check(exr.ok && tiff.ok, "alpha: a translucent document exports to EXR and TIFF");
      if (exr.ok && tiff.ok) {
        const DecodedImage be = decodeImageLinear(exr.bytes.data(), exr.bytes.size());
        const DecodedImage bt = decodeImageLinear(tiff.bytes.data(), tiff.bytes.size());
        if (be.valid() && bt.valid()) {
          const auto e = pixelOf(be, 0, 0);
          const auto t = pixelOf(bt, 0, 0);
          const std::array<float, 4> raw = storedPixel(alpha, 0, 0, 0);
          check(near(e[0], t[0], 1e-6f) && near(e[1], t[1], 1e-6f) &&
                    near(e[2], t[2], 1e-6f) && near(e[3], t[3], 1e-6f),
                "alpha: EXR (associate on write, un-associate on read) and TIFF (neither) "
                "return the same straight colour -- the paired conversion cancels");
          check(near(e[0], raw[0] / raw[3], 1e-6f),
                "alpha: and that colour is the tile's own stored rgb divided by its own "
                "stored alpha, not a plausible-looking number");
          const auto empty = pixelOf(be, 1, 0);
          check(empty[0] == 0.0f && empty[1] == 0.0f && empty[2] == 0.0f && empty[3] == 0.0f,
                "alpha: a never-painted texel round-trips through EXR as transparent "
                "black, not a divide-by-zero NaN");
          check(near(pixelOf(be, 2, 0)[3], 1.0f, 1e-6f),
                "alpha: the opaque control keeps alpha 1 (association by 1.0 is identity)");
        }
      }
    }

    // --- TIFF and DPX at 16-bit integer -----------------------------------
    {
      const struct { ImageFormat format; const char* name; } cases[] = {
          {ImageFormat::Tiff, "TIFF"}, {ImageFormat::Dpx, "DPX"}};
      for (const auto& c : cases) {
        const ExportResult enc = exportDocument(ramp, c.format, ExportTargetSpace::Rec709Srgb,
                                                ExportBitDepth::UInt16);
        char label[96];
        std::snprintf(label, sizeof(label), "%s: 16-bit-integer sRGB export round-trips",
                      c.name);
        bool caseOk = enc.ok;
        float maxResidual = 0.0f;
        if (enc.ok) {
          const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
          caseOk = back.valid() && back.width == 4 && back.height == 4;
          if (caseOk) {
            for (int i = 0; i < 16; ++i) {
              const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
              const std::array<float, 4> got =
                  pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
              for (int ch = 0; ch < 3; ++ch)
                maxResidual = std::max(maxResidual, std::fabs(got[ch] - stored[ch]));
            }
            std::printf("    [measured] %-4s 16-bit round-trip max residual   = %.3e "
                        "(tol %.3e)\n",
                        c.name, static_cast<double>(maxResidual),
                        static_cast<double>(kInteger16Tol));
            caseOk = maxResidual <= kInteger16Tol;
          }
        } else {
          std::printf("    (%s error was: %s)\n", c.name, enc.error.c_str());
        }
        check(caseOk, label);
      }
    }

    // --- HDR: no alpha, 32-bit float only, and RGBE's coarser precision ---
    {
      Document translucent = Document::createBlank(2, 1, WorkingSpace{});
      writeStraight(translucent, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
      writeStraight(translucent, 0, 1, 0, 0.5f, 0.5f, 0.5f, 0.25f);
      const ExportResult refused = exportDocument(translucent, ImageFormat::Hdr,
                                                  ExportTargetSpace::Rec709Linear,
                                                  ExportBitDepth::Float32);
      check(!refused.ok && contains(refused.error, "no alpha channel") &&
                contains(refused.error, "x=1, y=0"),
            "HDR: a translucent document is refused by name, the same check JPEG gets -- "
            "and nothing had to be told that HDR has no alpha");

      const ExportResult enc = exportDocument(ramp, ImageFormat::Hdr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      check(enc.ok, "HDR: the fully opaque ramp exports");
      if (enc.ok) {
        const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
        check(back.valid() && back.width == 4 && back.height == 4,
              "HDR: decodeImageLinear() reads the Radiance file back at the right size");
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int ch = 0; ch < 3; ++ch)
              maxResidual = std::max(maxResidual, std::fabs(got[ch] - stored[ch]));
          }
          std::printf("    [measured] HDR (RGBE) round-trip max residual    = %.3e "
                      "(tol %.3e)\n",
                      static_cast<double>(maxResidual), static_cast<double>(kRgbeTol));
          check(maxResidual <= kRgbeTol,
                "HDR: within the separately derived RGBE tolerance -- shared-exponent "
                "storage, ~16x coarser than half, and labelled as such");
          check(near(pixelOf(back, 0, 0)[3], 1.0f, 1e-6f),
                "HDR: the synthesized alpha comes back fully opaque");
        }
      }
      const ExportResult wrongDepth = exportDocument(ramp, ImageFormat::Hdr,
                                                    ExportTargetSpace::Rec709Linear,
                                                    ExportBitDepth::UInt8);
      check(!wrongDepth.ok && contains(wrongDepth.error, "HDR") &&
                contains(wrongDepth.error, "8-bit integer"),
            "HDR: an 8-bit request is refused, even though OpenImageIO would have "
            "accepted it and written 32-bit float instead");
    }

    // --- Flattened PSD read, from a hand-built fixture ---------------------
    //
    // Built byte by byte here rather than checked in as a binary, for the
    // same reason io/Export's encodePng16() started life as a --selftest
    // fixture builder: a test whose input this file constructs from the
    // published file layout cannot pass by construction against a decoder
    // that shares its assumptions. Layout (Adobe Photoshop File Formats
    // spec): 26-byte header, then three length-prefixed sections left
    // empty, then a compression word and raw *planar* channel data.
    {
      std::vector<uint8_t> psd;
      auto u16 = [&](uint32_t v) {
        psd.push_back(static_cast<uint8_t>(v >> 8));
        psd.push_back(static_cast<uint8_t>(v));
      };
      auto u32 = [&](uint32_t v) {
        psd.push_back(static_cast<uint8_t>(v >> 24));
        psd.push_back(static_cast<uint8_t>(v >> 16));
        psd.push_back(static_cast<uint8_t>(v >> 8));
        psd.push_back(static_cast<uint8_t>(v));
      };
      const char signature[] = "8BPS";
      psd.insert(psd.end(), signature, signature + 4);
      u16(1);                                    // version 1 (PSD, not PSB)
      for (int i = 0; i < 6; ++i) psd.push_back(0);  // reserved, must be zero
      u16(3);                                    // channel count
      u32(2);                                    // height
      u32(2);                                    // width
      u16(8);                                    // bits per channel
      u16(3);                                    // colour mode: RGB
      u32(0);                                    // colour mode data: none
      u32(0);                                    // image resources: none
      u32(0);                                    // layer and mask info: none
      u16(0);                                    // compression: raw
      // Planar: the whole R plane, then G, then B. Four pixels: red, green,
      // blue, and one mixed value whose exact sRGB-decoded result is
      // hand-checkable below.
      const uint8_t planes[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 64}};
      for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 4; ++i) psd.push_back(planes[c][i]);

      std::string err;
      const DecodedImage back = decodeImageLinear(psd.data(), psd.size(), &err);
      check(back.valid() && back.width == 2 && back.height == 2,
            "PSD: a hand-built 52-byte flattened PSD decodes through decodeImageLinear()");
      if (back.valid()) {
        const auto red = pixelOf(back, 0, 0);
        const auto mixed = pixelOf(back, 1, 1);
        check(near(red[0], 1.0f, 1e-6f) && near(red[1], 0.0f, 1e-6f) &&
                  near(red[2], 0.0f, 1e-6f) && near(red[3], 1.0f, 1e-6f),
              "PSD: the planar channel layout is read in the right order (pixel 0 is red, "
              "not 'the first byte of each plane concatenated')");
        // 8-bit integer source, so io/OiioBackend applies the same sRGB
        // decode assumption io/ImageDecode.cpp already documents for
        // untagged integer files -- checked against color/Space's own curve
        // rather than a literal.
        check(near(mixed[0], srgbDecode(255.0f / 255.0f), 1e-6f) &&
                  near(mixed[1], srgbDecode(128.0f / 255.0f), 1e-6f) &&
                  near(mixed[2], srgbDecode(64.0f / 255.0f), 1e-6f),
              "PSD: an integer-typed source is sRGB-decoded to linear, matching "
              "io/ImageDecode's own documented assumption for untagged files");
        check(near(mixed[3], 1.0f, 1e-6f),
              "PSD: a 3-channel source decodes as fully opaque");
      }
    }
  }

  std::printf("[selftest] format support %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// ==========================================================================
// Phase 4 step 4 -- native `.npaint` save and load (multi-part tiled EXR).
// ==========================================================================
bool runNpaintFormatTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Which backend set was compiled in. A plain constant rather than an
  // #ifdef around each case, following runFormatSupportTest()'s own
  // precedent and PLAN.md §1.5's "an unexercised build option is not a
  // seam": every assertion below runs in both configurations and states the
  // correct answer for the one it is in.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // There is exactly one number here, and most of this section deliberately
  // does not use it: the layer round trip is asserted at **zero tolerance**,
  // because the chain it travels has no rounding stage in it. A tile holds
  // `uint16_t` half words; buildLayerPart() memcpys them into the part
  // buffer; OpenImageIO writes them as TypeDesc::HALF; ZIP is lossless;
  // reading asks for TypeDesc::HALF back; unpackLayerPart() memcpys them
  // into a tile. No float appears anywhere along it. That is precisely
  // docs/document-format.md's own justification for EXR ("Working space is
  // `rgba16float` -> HALF channels -- byte-identical, no conversion"), and a
  // tolerance there would let a real regression through. It is also the same
  // claim, and the same reasoning, runFormatSupportTest() applies to the EXR
  // *export* round trip.
  //
  // The composite (part 0) is a different matter and is the one place a
  // tolerance is honest, because it is a regenerated product that genuinely
  // passes through float:
  //
  //   tile half -> readPixel (exact: every half is exactly a float)
  //     -> flatten's composite (PLAN.md Phase 5 step 1: `over`, honouring
  //        opacity). This used to be a *sum of a single contribution*, i.e.
  //        exact; it is now up to two roundings per channel -- the opacity
  //        multiply (`* 0.72f` on layer 0) and `over`'s multiply-add -- each
  //        <= 2^-24 relative
  //     -> un-premultiply, /a (rounds, <= 2^-24 relative)
  //     -> re-associate, *a (rounds, <= 2^-24 relative)
  //     -> float-to-half (rounds, <= 2^-11 relative -- binary16 has 10
  //        stored mantissa bits, so half an ulp is v * 2^-11)
  //     -> half-to-float on read (exact)
  //     -> un-premultiply for the DecodedImage contract, /a (<= 2^-24)
  //
  // The float roundings are ~8000x smaller than the half one and are kept in
  // the bound rather than waved away: relative error <= 2^-11 + 5*2^-24 =
  // 4.8830e-4 + 2.98e-7 = 4.8860e-4. The fixture's composite values are all
  // <= 1.0, so that is also the absolute bound. Landed 7.0e-4 -- 1.43x the
  // *derived bound*, not 1.43x the measurement, matching runLutBakeTest()'s
  // kResidualTol / runApplyPassTest()'s kApplyResidualTol / step 1's
  // kRoundTripTol16 discipline and runFormatSupportTest()'s own
  // kHalfEncodedTol, which is the same 2^-11 argument for the same reason.
  // The measured worst case is printed at run time so the derivation is
  // checkable rather than merely asserted.
  //
  // And for the *opaque* pixels the composite is additionally asserted
  // **exactly**: at alpha == 1.0 (exact in half) the two un-premultiplies
  // and the re-association are all by exactly 1.0, so the only surviving
  // stage is a float-to-half of a value that already is a half. Zero.
  //
  // Phase 5 step 1 nearly made that half of the claim vacuous and it is worth
  // saying why it did not. Compositing honours opacity, and layer 0 sits at
  // 0.72, so **none of layer 0's opaque pixels composites to alpha 1.0 any
  // more** -- 1.0 * 0.72 lands at 0.72. The pixel that keeps this assertion
  // meaningful is layer 1's opaque texel at (129,3), which nothing overlaps
  // and which sits at opacity 1.0, so it composites to alpha exactly 1.0 and
  // travels the whole multiply-by-one chain above. That is also why layer 1
  // had to be the visible one (see the fixture).
  constexpr float kCompositeTol = 7.0e-4f;

  // Fixture helpers, matching runExportTest()/runFormatSupportTest()'s: a
  // document holds *premultiplied* halfs, so a fixture writes straight
  // values through the same `rgb *= a` io/ImageIO.cpp performs on import,
  // and every precision claim is checked against the tile's own stored
  // value rather than the float literal that went in.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto storedWords = [](const Document& doc, size_t layerIndex,
                        TileCoord coord) -> const uint16_t* {
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(coord);
    return t ? t->data() : nullptr;
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };

  // A three-layer document with content in several different tiles (so the
  // per-part data window is genuinely a bounding box and not "the canvas"),
  // distinct metadata on every layer, and a translucent pixel as well as
  // opaque ones.
  auto buildFixture = []() {
    Document doc = Document::createBlank(400, 300, WorkingSpace{});
    doc.layers[0].name = "Line pass";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.72f;
    doc.layers[0].visible = true;
    doc.layers[0].locked = true;
    doc.layers[0].parent = "G0001";

    Layer mid;
    mid.kind = LayerKind::RGB;
    mid.rgbTiles.emplace();
    mid.name = "Flats";
    mid.blend = "normal";
    mid.opacity = 1.0f;
    // **Visible, and this moved here from `top` in Phase 5 step 1.** The
    // fixture needs exactly one layer with `np:visible == 0` to prove the
    // attribute round-trips as false rather than as absent, and it needs the
    // two-layers-at-one-pixel construction below to stay a *real* overlap so
    // the composite tolerance keeps bounding something. Those two demands
    // collided the moment a hidden layer started contributing nothing: this
    // layer owns half the overlap, so hiding it would have voided the
    // measurement silently. `top` -- which has no tiles at all and already
    // contributes nothing at opacity 0.0 -- carries the `false` instead, at no
    // cost to either property.
    mid.visible = true;
    mid.locked = false;
    mid.parent = "";
    doc.layers.push_back(std::move(mid));

    Layer top;
    top.kind = LayerKind::RGB;
    top.rgbTiles.emplace();
    top.name = "Layer 1";  // deliberately duplicated below -- names are not unique
    // **Changed from "screen" by PLAN.md Phase 5 step 2, and the change is
    // load-bearing rather than cosmetic.** This fixture's blend names carry
    // two claims at once: that `np:blend` round-trips *verbatim* (PRD I10),
    // and that a name this build cannot composite is reported at the save
    // boundary rather than silently approximated. Step 2 implements both
    // "multiply" and "screen", which would have quietly voided the second
    // claim -- the save would produce no warnings and the assertion below
    // would have to be deleted -- and weakened the first, since a name the
    // build recognises proves nothing about carrying one it does not.
    // "linear-burn" is outside core/Blend's set, so both claims are back.
    // Layer 0 keeps "multiply" so the round trip also covers a name that IS
    // in the set. No pixel changes: this layer has no tiles, is hidden and
    // sits at opacity 0, so it has never contributed to the composite.
    top.blend = "linear-burn";
    top.opacity = 0.0f;
    top.visible = false;  // see `mid.visible` above for why the false lives here
    top.locked = true;
    top.parent = "G0001";
    doc.layers.push_back(std::move(top));
    return doc;
  };

  Document fixture = buildFixture();
  // Layer 0: two well-separated tiles, so its data window spans a 3x2 tile
  // block with a hole in it -- the case a rectangular EXR data window cannot
  // encode sparsely and the reader's drop-all-zero-tiles rule handles.
  writeStraight(fixture, 0, 5, 7, 0.25f, 0.5f, 0.75f, 1.0f);
  writeStraight(fixture, 0, 300, 200, 1.0f, 0.0f, 0.5f, 1.0f);
  // A translucent texel, so the un-premultiply path is exercised on both
  // sides rather than only alpha == 1. Alpha is 0.3, NOT 0.5: a
  // power-of-two alpha makes the divide and the multiply back exact, which
  // would make the composite tolerance below vacuous -- it measured exactly
  // zero with 0.5 and never touched the term it exists to bound.
  writeStraight(fixture, 0, 6, 7, 0.8f, 0.4f, 0.2f, 0.3f);
  // Layer 1: one tile, far from layer 0's.
  writeStraight(fixture, 1, 129, 3, 0.125f, 0.875f, 0.0f, 1.0f);
  // Two layers contributing to the SAME pixel, with alphas that do not sum
  // to 1 and premultiplied values whose float sum is not itself a half.
  // This is the only construction in this fixture that makes the composite's
  // float->half stage actually round: everywhere else the un-premultiply and
  // re-association are by the same alpha and cancel exactly, which measured
  // 0.000e+00 and left the derived tolerance below bounding nothing.
  writeStraight(fixture, 0, 10, 10, 0.1f, 0.2f, 0.3f, 0.5f);
  writeStraight(fixture, 1, 10, 10, 0.7f, 0.6f, 0.5f, 0.25f);
  // Layer 2: no painted tiles at all -- the empty-layer edge case (EXR has
  // no zero-area part, so this is written as one all-zero tile and must come
  // back with zero tiles allocated, not one).

  // --- Request validation: identical in both build configurations --------
  //
  // Deliberately checked before anything that needs a backend. saveNpaint()
  // validates the request before it consults NP_USE_OIIO, so a malformed
  // request is refused the same way everywhere and --selftest can prove all
  // of PRD I11's refusals in the build with no OpenImageIO in it.
  {
    // PRD I7. The predicate first, since it is the public, backend-free half.
    std::string why;
    check(npaintCompressionIsLossy("dwaa", &why) && contains(why, "dwaa") &&
              contains(why, "lossy") && contains(why, "zip"),
          "I7: dwaa is refused by name, with the reason and an alternative");
    check(npaintCompressionIsLossy("DWAB", nullptr) && npaintCompressionIsLossy("b44", nullptr) &&
              npaintCompressionIsLossy("b44a", nullptr),
          "I7: dwab, b44 and b44a are lossy too, and the match is case-insensitive");
    check(npaintCompressionIsLossy("dwaa:45", &why) && contains(why, "dwaa"),
          "I7: the `name:level` form is caught -- `dwaa:45` cannot slip past");
    check(npaintCompressionIsLossy("pxr24", &why) && contains(why, "24 bits"),
          "I7: pxr24 is refused too (lossless only for half/integer, and a .npaint file can "
          "carry float parts forward)");
    // `&why` on the last one deliberately: the predicate must *clear* the
    // reason string when it answers "not lossy", or a caller reusing one
    // buffer across calls would report the previous refusal's wording.
    check(!npaintCompressionIsLossy("zip", nullptr) && !npaintCompressionIsLossy("piz", nullptr) &&
              !npaintCompressionIsLossy("zips", nullptr) &&
              !npaintCompressionIsLossy("rle", nullptr) &&
              !npaintCompressionIsLossy("none", &why) && why.empty(),
          "I7: the five lossless compressors are not refused, and the reason string is "
          "cleared for them");

    NpaintSaveOptions lossy;
    lossy.compression = "dwab:60";
    const NpaintSaveResult r = saveNpaint(fixture, "selftest_npaint_never.npaint", lossy);
    check(!r.ok && contains(r.error, "dwab") && contains(r.error, "lossy") &&
              contains(r.error, "PRD I7"),
          "I7: saveNpaint() itself refuses a lossy compressor by name (both builds)");
    std::FILE* f = std::fopen("selftest_npaint_never.npaint", "rb");
    check(f == nullptr, "I7: and nothing was written -- the refusal precedes the file");
    if (f) {
      std::fclose(f);
      std::remove("selftest_npaint_never.npaint");
    }

    NpaintSaveOptions unknown;
    unknown.compression = "squish9000";
    const NpaintSaveResult u = saveNpaint(fixture, "selftest_npaint_never.npaint", unknown);
    check(!u.ok && contains(u.error, "squish9000") && contains(u.error, "zip"),
          "I7: an unrecognised compressor is refused too -- an unknown name cannot be "
          "assumed lossless");

    Document zeroCanvas;
    const NpaintSaveResult z = saveNpaint(zeroCanvas, "selftest_npaint_never.npaint");
    check(!z.ok && contains(z.error, "no canvas"),
          "I11: a zero-area canvas is refused with a specific message");

    Document withPigment = Document::createBlank(64, 64, WorkingSpace{});
    Layer pigment;
    pigment.kind = LayerKind::Pigment;
    pigment.name = "Wet wash";
    withPigment.layers.push_back(std::move(pigment));
    const NpaintSaveResult p = saveNpaint(withPigment, "selftest_npaint_never.npaint");
    check(!p.ok && contains(p.error, "layer 1") && contains(p.error, "Wet wash") &&
              contains(p.error, "Pigment") && contains(p.error, "pig."),
          "I11: a Pigment layer is refused by index, name and kind -- naming exactly what "
          "would be lost, not degrading silently");

    Document badOpacity = Document::createBlank(64, 64, WorkingSpace{});
    badOpacity.layers[0].opacity = 1.5f;
    const NpaintSaveResult o = saveNpaint(badOpacity, "selftest_npaint_never.npaint");
    check(!o.ok && contains(o.error, "opacity") && contains(o.error, "[0, 1]"),
          "I11: an out-of-range opacity is refused rather than written for readers to "
          "misinterpret");

    Document noTiles = Document::createBlank(64, 64, WorkingSpace{});
    noTiles.layers[0].rgbTiles.reset();
    const NpaintSaveResult n = saveNpaint(noTiles, "selftest_npaint_never.npaint");
    check(!n.ok && contains(n.error, "no tile storage"),
          "I11: an RGB layer with no tile storage is refused as malformed");

    // The two measured OpenImageIO limitations, refused by name. Both are
    // request-shape checks, so both fire in either build.
    NpaintCarry blobCarry;
    NpaintAttribute blob;
    blob.name = "np:futureOps";
    blob.type = NpaintAttribute::Type::Blob;
    blob.blobValue = {1, 2, 250, 4, 5};
    blobCarry.documentAttributes.push_back(blob);
    const NpaintSaveResult b =
        saveNpaint(fixture, "selftest_npaint_never.npaint", {}, &blobCarry);
    check(!b.ok && contains(b.error, "np:futureOps") && contains(b.error, "UINT8[n]") &&
              contains(b.error, "base64"),
          "measured: a UINT8[n] blob attribute is refused by name (OpenImageIO drops array "
          "attributes on write) with the string-encoding workaround named");

    NpaintCarry scanlineCarry;
    NpaintRawPart scanline;
    scanline.name = "X0001";
    scanline.width = scanline.height = 2;
    scanline.channelNames = {"a", "b"};
    scanline.sampleTypeName = "float";
    scanline.rawPixels.assign(2 * 2 * 2 * sizeof(float), 0);
    scanlineCarry.rawParts.push_back(scanline);
    const NpaintSaveResult sc =
        saveNpaint(fixture, "selftest_npaint_never.npaint", {}, &scanlineCarry);
    check(!sc.ok && contains(sc.error, "X0001") && contains(sc.error, "scanline"),
          "measured: a carried scanline part is refused by name (OpenEXR multi-part cannot "
          "mix scanline and tiled parts)");
  }

  // --- The NP_USE_OIIO=OFF refusal, and its converse ---------------------
  {
    const NpaintSaveResult r = saveNpaint(fixture, "selftest_npaint_gate.npaint");
    check(r.ok == kOiioBuild,
          "the save entry point exists in both builds and succeeds in exactly the one with "
          "an OpenEXR writer");
    if (!kOiioBuild) {
      check(contains(r.error, ".npaint") && contains(r.error, "NP_USE_OIIO=OFF") &&
                contains(r.error, "-DNP_USE_OIIO=ON") && contains(r.error, "OpenEXR"),
            "OFF build: the save refusal names .npaint, the build option, and the cmake line "
            "that enables it");
      check(contains(r.error, "exportDocumentToFile"),
            "OFF build: and names the alternative that does work in this build (PRD I1's "
            "stb-backed formats), per io/Export.cpp's refusal style");
      const NpaintLoadResult l = loadNpaint("selftest_npaint_gate.npaint");
      check(!l.ok && contains(l.error, "NP_USE_OIIO=OFF") && contains(l.error, ".npaint"),
            "OFF build: the load entry point refuses the same way, from the same wording");
    }
    std::remove("selftest_npaint_gate.npaint");
  }

  // Everything past this point needs a real backend. A plain runtime `if`,
  // not an #ifdef: every call below compiles in both configurations, so the
  // OFF build still type-checks all of it.
  if (kOiioBuild) {
    const char* kPath = "selftest_npaint_roundtrip.npaint";
    std::remove(kPath);

    // --- The round trip ---------------------------------------------------
    const NpaintSaveResult saved = saveNpaint(fixture, kPath);
    check(saved.ok && saved.error.empty(), "save: a three-layer document writes without error");
    check(saved.partsWritten == 4,
          "save: four parts -- part 0's composite plus one per layer (PRD I4)");
    // **Changed by PLAN.md Phase 5 step 1, and narrowed by step 2.** This used
    // to assert `saved.warnings.empty()`. It cannot, and the reason is the
    // point rather than an inconvenience: this fixture's top layer carries a
    // blend name core/Blend does not implement, so part 0 -- which is
    // regenerated on every save (PRD I12) and is a real composite rather than
    // a sum -- is an approximation of what that layer means. The save still
    // goes ahead, because refusing would make a PRD I10-preserved blend name
    // the thing that stops the document being saved (core/Composite.hpp argues
    // that at length); it just says so. Asserting the *content* of the warning
    // rather than merely its count is what makes "never silently" checkable.
    //
    // Step 2 took this from two warnings to one: "multiply" is implemented
    // now, so only the deliberately-unknown "linear-burn" on the top layer
    // remains. See the fixture for why that layer's name was changed rather
    // than the assertion relaxed.
    {
      bool warnedUnknown = false;
      for (const std::string& w : saved.warnings)
        if (contains(w, "\"linear-burn\"") && contains(w, "Layer 1")) warnedUnknown = true;
      check(saved.warnings.size() == 1 && warnedUnknown,
            "save: the one layer whose blend this build cannot composite is named, with its "
            "blend, rather than silently composited as `over`");
    }

    // The file really is an OpenEXR file, checked in its own bytes rather
    // than by trusting the writer: magic 0x76 0x2f 0x31 0x01, then the
    // version int whose bit 12 (0x1000) is OpenEXR's multi-part flag.
    std::vector<uint8_t> head;
    if (std::FILE* f = std::fopen(kPath, "rb")) {
      uint8_t buf[8] = {};
      const size_t n = std::fread(buf, 1, sizeof(buf), f);
      head.assign(buf, buf + n);
      std::fclose(f);
    }
    check(head.size() == 8 && head[0] == 0x76 && head[1] == 0x2f && head[2] == 0x31 &&
              head[3] == 0x01,
          "save: the bytes on disk are an OpenEXR file (magic 0x76 0x2f 0x31 0x01)");
    check(head.size() == 8 && (head[5] & 0x10) != 0,
          "save: with OpenEXR's multi-part bit set in the version field -- multi-part is a "
          "property of the file, not of our reader");

    const NpaintLoadResult loaded = loadNpaint(kPath);
    check(loaded.ok && loaded.error.empty(), "load: reads back without error");
    check(loaded.warnings.empty(),
          "load: with no warnings at all -- a file this build wrote is a file it fully "
          "understands");
    check(loaded.document.width == 400 && loaded.document.height == 300,
          "load: the canvas comes back from the display window");
    check(loaded.document.layers.size() == 3, "load: all three layers come back");

    if (loaded.document.layers.size() == 3) {
      // --- Bit-exactness, at zero tolerance ------------------------------
      size_t comparedTiles = 0;
      size_t differingWords = 0;
      for (size_t li = 0; li < 3; ++li) {
        const TileStore& src = *fixture.layers[li].rgbTiles;
        const TileStore& dst = *loaded.document.layers[li].rgbTiles;
        for (const auto& [coord, tile] : src) {
          (void)tile;
          const uint16_t* a = storedWords(fixture, li, coord);
          const Tile* bt = dst.find(coord);
          if (!a || !bt) {
            differingWords += Tile::kTexelCount;
            continue;
          }
          ++comparedTiles;
          for (size_t w = 0; w < Tile::kTexelCount; ++w) {
            if (a[w] != bt->data()[w]) ++differingWords;
          }
        }
      }
      std::printf("    [measured] %zu tiles compared, %zu of %zu half words differ "
                  "(expected exactly 0)\n",
                  comparedTiles, differingWords, comparedTiles * Tile::kTexelCount);
      check(comparedTiles == 4 && differingWords == 0,
            "fidelity: every layer's tiles come back BIT-identical -- zero tolerance, the "
            "claim docs/document-format.md makes for HALF channels");

      check(loaded.document.layers[0].rgbTiles->occupiedTileCount() == 2 &&
                loaded.document.layers[1].rgbTiles->occupiedTileCount() == 2 &&
                loaded.document.layers[2].rgbTiles->occupiedTileCount() == 0,
            "fidelity: the sparse tile set survives -- including the hole inside layer 0's "
            "data window and the layer that has no tiles at all");

      // --- The seven per-layer np:* attributes ---------------------------
      const Layer& l0 = loaded.document.layers[0];
      const Layer& l1 = loaded.document.layers[1];
      const Layer& l2 = loaded.document.layers[2];
      check(l0.kind == LayerKind::RGB && l1.kind == LayerKind::RGB && l2.kind == LayerKind::RGB,
            "np:kind: every layer's kind round-trips");
      check(l0.name == "Line pass" && l1.name == "Flats" && l2.name == "Layer 1",
            "np:name: the user-facing name round-trips, and is allowed to duplicate -- the "
            "part id (L0001) is what has to be unique");
      check(l0.blend == "multiply" && l1.blend == "normal" && l2.blend == "linear-burn",
            "np:blend: the blend identity round-trips verbatim");
      check(l0.opacity == 0.72f && l1.opacity == 1.0f && l2.opacity == 0.0f,
            "np:opacity: exact float equality, including 0.0 (an absent attribute would "
            "read back as the 1.0 default and fail here)");
      check(l0.visible == true && l1.visible == true && l2.visible == false,
            "np:visible: round-trips, with false actually distinguishable from absent");
      check(l0.locked == true && l1.locked == false && l2.locked == true,
            "np:locked: round-trips");
      check(l0.parent == "G0001" && l1.parent == "" && l2.parent == "G0001",
            "np:parent: round-trips (and the empty case lands on the reader's own default, "
            "since OpenImageIO drops empty string attributes -- measured)");

      check(loaded.carry.layerPartNames.size() == 3 &&
                loaded.carry.layerPartNames[0] == "L0001" &&
                loaded.carry.layerPartNames[1] == "L0002" &&
                loaded.carry.layerPartNames[2] == "L0003",
            "part naming: stable synthetic ids in layer order, one-based, as "
            "docs/document-format.md requires");

      // --- Document-level attributes -------------------------------------
      check(loaded.carry.sourceVersion == kNpaintFormatVersion,
            "np:version: written and read back");
      check(loaded.carry.basis == kNpaintPigmentBasis, "np:basis: written and read back");
      const Primaries& p = loaded.document.workingSpace.primaries;
      check(p.redX == kRec709Primaries.redX && p.greenY == kRec709Primaries.greenY &&
                p.whiteX == kRec709Primaries.whiteX && p.whiteY == kRec709Primaries.whiteY,
            "I6: the working space's primaries survive through the standard EXR "
            "`chromaticities` attribute");
    }

    // --- PRD I12: part 0 is regenerated, never stale ---------------------
    {
      const DecodedImage expect = flattenDocumentToLinear(fixture);
      float worst = 0.0f;
      float worstOpaque = 0.0f;
      if (loaded.composite.valid() && expect.valid()) {
        for (uint32_t y = 0; y < expect.height; ++y) {
          for (uint32_t x = 0; x < expect.width; ++x) {
            const auto e = pixelOf(expect, x, y);
            const auto g = pixelOf(loaded.composite, x, y);
            for (int c = 0; c < 4; ++c) {
              const float d = std::fabs(e[c] - g[c]);
              worst = std::max(worst, d);
              if (e[3] == 1.0f || e[3] == 0.0f) worstOpaque = std::max(worstOpaque, d);
            }
          }
        }
      }
      std::printf("    [measured] composite vs. flattenDocumentToLinear: max residual = "
                  "%.3e (bound %.3e), and %.3e at alpha in {0,1} (expected exactly 0)\n",
                  worst, static_cast<double>(kCompositeTol) / 1.43, worstOpaque);
      check(loaded.composite.valid() && loaded.composite.width == 400 &&
                loaded.composite.height == 300,
            "I5b: part 0 is a full-canvas composite any EXR reader can show");
      check(worst <= kCompositeTol,
            "I5b: and it matches io/Export's flattener within the derived half tolerance");
      check(worstOpaque == 0.0f,
            "I5b: exactly, at every fully opaque or fully transparent pixel -- where the "
            "un-premultiply and re-association are both by exactly 1.0 and nothing rounds");

      // Now mutate a layer and save again. A composite that were merely
      // copied forward, or written once and left, would still show the old
      // pixel -- which is the failure docs/document-format.md §3.4 says
      // "nobody notices for months".
      const auto before = pixelOf(loaded.composite, 5, 7);
      writeStraight(fixture, 0, 5, 7, 0.0f, 1.0f, 0.25f, 1.0f);
      const NpaintSaveResult again = saveNpaint(fixture, kPath, {}, &loaded.carry);
      check(again.ok, "I12: the mutated document saves again over the same file");
      const NpaintLoadResult reloaded = loadNpaint(kPath);
      check(reloaded.ok, "I12: and reads back");
      if (reloaded.ok && reloaded.composite.valid()) {
        const auto after = pixelOf(reloaded.composite, 5, 7);
        const DecodedImage expect2 = flattenDocumentToLinear(fixture);
        const auto want = pixelOf(expect2, 5, 7);
        check(before[0] != after[0] || before[1] != after[1],
              "I12: the composite at the mutated pixel actually CHANGED -- a test that only "
              "checked part 0 exists would not catch a stale one");
        check(std::fabs(after[0] - want[0]) <= kCompositeTol &&
                  std::fabs(after[1] - want[1]) <= kCompositeTol &&
                  std::fabs(after[2] - want[2]) <= kCompositeTol,
              "I12: and it changed to match the NEW flattened document, not to something "
              "merely different");
        // And the layer tiles are still bit-exact after the second save, so
        // regenerating the composite did not disturb them.
        const uint16_t* src = storedWords(fixture, 0, TileCoord{0, 0});
        const Tile* dst = reloaded.document.layers[0].rgbTiles->find(TileCoord{0, 0});
        bool same = src && dst;
        if (same) {
          for (size_t w = 0; w < Tile::kTexelCount && same; ++w)
            same = src[w] == dst->data()[w];
        }
        check(same, "I12: and the layer tiles are still bit-exact after the second save");
      }
    }

    std::remove(kPath);
  }

  // --- PRD I10: unrecognised attributes and parts survive verbatim -------
  if (kOiioBuild) {
    const char* kPath = "selftest_npaint_carry.npaint";
    std::remove(kPath);

    // Everything here is deliberately something this build's code has no
    // knowledge of whatsoever: attribute names it never writes, on a part
    // whose np:kind it cannot hold, with channels it has never heard of.
    NpaintCarry carry;
    NpaintAttribute s;
    s.name = "np:futureComps";
    s.type = NpaintAttribute::Type::String;
    s.stringValue = "comp1:on,off,on|comp2:off,on,on";
    NpaintAttribute i;
    i.name = "np:futureRevision";
    i.type = NpaintAttribute::Type::Int;
    i.intValue = 1234567;
    NpaintAttribute fl;
    fl.name = "np:futureGamma";
    fl.type = NpaintAttribute::Type::Float;
    fl.floatValue = 2.4f;
    carry.documentAttributes = {s, i, fl};

    NpaintAttribute lm;
    lm.name = "np:futureMaskLink";
    lm.type = NpaintAttribute::Type::String;
    lm.stringValue = "M0007";
    carry.layerAttributes = {{lm}, {}, {}};

    // A future writer's Pigment layer: the exact case io/NpaintFile.hpp's
    // deferral list says must survive. `pig.c0` sorts before `pig.m`, which
    // matters -- OpenEXR stores channels in a sorted ChannelList, so a
    // fixture whose channel order is not already sorted would come back
    // reordered and the comparison would fail for a reason that has nothing
    // to do with preservation.
    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.x = 0;
    pigment.y = 0;
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    pigment.channelNames = {"pig.c0", "pig.m"};
    pigment.sampleTypeName = "float";
    {
      const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                              0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
      pigment.rawPixels.resize(sizeof(vals));
      std::memcpy(pigment.rawPixels.data(), vals, sizeof(vals));
    }
    NpaintAttribute pk;
    pk.name = "np:kind";
    pk.type = NpaintAttribute::Type::String;
    pk.stringValue = "Pigment";
    NpaintAttribute pm;
    pm.name = "np:medium";
    pm.type = NpaintAttribute::Type::String;
    pm.stringValue = "watercolour";
    pigment.attributes = {pk, pm};
    carry.rawParts = {pigment};

    // Interleaved on purpose: layer, then the foreign part, then layer.
    // Appending carried parts at the end would silently reorder the stack.
    carry.partOrder = {{NpaintPartSlot::Kind::Layer, 0},
                       {NpaintPartSlot::Kind::RawPart, 0},
                       {NpaintPartSlot::Kind::Layer, 1}};
    carry.layerPartNames = {"L0001", "L0003"};
    carry.basis = "future-basis-v9";

    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 1.0f);

    const NpaintSaveResult saved = saveNpaint(doc, kPath, {}, &carry);
    check(saved.ok && saved.partsWritten == 4,
          "I10: a document with one foreign part saves -- 4 parts (composite, 2 layers, the "
          "carried Pigment part)");

    const NpaintLoadResult back = loadNpaint(kPath);
    check(back.ok, "I10: and reads back");
    if (back.ok) {
      check(back.document.layers.size() == 2,
            "I10: the foreign part did NOT become a layer -- the reader is strict in its own "
            "disfavour rather than half-understanding it");
      bool warnedByName = false;
      for (const std::string& w : back.warnings) {
        if (w.find("L0002") != std::string::npos && w.find("Pigment") != std::string::npos)
          warnedByName = true;
      }
      check(warnedByName,
            "I10: and the load says so by name, naming the np:kind it could not hold");

      check(back.carry.documentAttributes.size() == 3,
            "I10: all three unrecognised document attributes came back");
      bool docAttrsExact = back.carry.documentAttributes.size() == 3;
      if (docAttrsExact) {
        // Order is EXR's own (sorted), so compare as a set by name.
        auto findByName = [&](const char* n) -> const NpaintAttribute* {
          for (const auto& a : back.carry.documentAttributes)
            if (a.name == n) return &a;
          return nullptr;
        };
        const NpaintAttribute* gs = findByName("np:futureComps");
        const NpaintAttribute* gi = findByName("np:futureRevision");
        const NpaintAttribute* gf = findByName("np:futureGamma");
        docAttrsExact = gs && gi && gf && *gs == s && *gi == i && *gf == fl;
      }
      check(docAttrsExact,
            "I10: each one byte-for-byte identical -- string, int and float, none of which "
            "this build's code knows anything about");

      check(back.carry.layerAttributes.size() == 2 &&
                back.carry.layerAttributes[0].size() == 1 &&
                back.carry.layerAttributes[0][0] == lm &&
                back.carry.layerAttributes[1].empty(),
            "I10: an unrecognised attribute on a *layer* part survives, attached to the "
            "right layer");

      check(back.carry.basis == "future-basis-v9",
            "I10: np:basis is preserved verbatim rather than overwritten with this build's");

      check(back.carry.rawParts.size() == 1 && back.carry.rawParts[0].name == "L0002" &&
                back.carry.rawParts[0].channelNames ==
                    std::vector<std::string>{"pig.c0", "pig.m"} &&
                back.carry.rawParts[0].sampleTypeName == "float" &&
                back.carry.rawParts[0].width == 4 && back.carry.rawParts[0].height == 2 &&
                back.carry.rawParts[0].rawPixels == pigment.rawPixels,
            "I10: the whole foreign part survives -- name, channel names, sample type, data "
            "window and every pixel byte");
      check(back.carry.rawParts.size() == 1 &&
                back.carry.rawParts[0].attributes.size() == 2,
            "I10: including both of its own np:* attributes");

      check(back.carry.partOrder.size() == 3 &&
                back.carry.partOrder[0].kind == NpaintPartSlot::Kind::Layer &&
                back.carry.partOrder[1].kind == NpaintPartSlot::Kind::RawPart &&
                back.carry.partOrder[2].kind == NpaintPartSlot::Kind::Layer,
            "I10: and it is still BETWEEN the two layers -- part order is layer order, so "
            "appending carried parts at the end would be data loss in the ordering");
      check(back.carry.layerPartNames.size() == 2 &&
                back.carry.layerPartNames[0] == "L0001" &&
                back.carry.layerPartNames[1] == "L0003",
            "I10: the layers kept their original part ids, so an np:parent link inside the "
            "foreign part still points where it did");

      // The second generation. One round trip proving preservation is a
      // weaker claim than it looks: it only shows the reader kept what the
      // writer had in hand. Saving the *loaded* carry and reading it again
      // proves the loop is closed.
      const char* kPath2 = "selftest_npaint_carry2.npaint";
      std::remove(kPath2);
      const NpaintSaveResult again = saveNpaint(back.document, kPath2, {}, &back.carry);
      check(again.ok && again.partsWritten == 4,
            "I10: the loaded carry saves straight back out");
      const NpaintLoadResult third = loadNpaint(kPath2);
      check(third.ok && third.carry.documentAttributes == back.carry.documentAttributes &&
                third.carry.layerAttributes == back.carry.layerAttributes &&
                third.carry.basis == back.carry.basis &&
                third.carry.rawParts.size() == 1 &&
                third.carry.rawParts[0].rawPixels == pigment.rawPixels &&
                third.carry.rawParts[0].attributes == back.carry.rawParts[0].attributes,
            "I10: and a SECOND generation is still identical -- an older build can open a "
            "newer document, edit it, save it, and destroy nothing");
      std::remove(kPath2);
    }
    std::remove(kPath);
  }

  // --- PRD I8: `.npaint` and `.exr` are the same container ----------------
  if (kOiioBuild) {
    const char* kExr = "selftest_npaint_as.exr";
    std::remove(kExr);
    const NpaintSaveResult saved = saveNpaint(fixture, kExr);
    check(saved.ok, "I8: the same document saves under a .exr name");
    const NpaintLoadResult back = loadNpaint(kExr);
    check(back.ok && back.document.layers.size() == 3,
          "I8: and reads back identically -- the writer is chosen by format name, never by "
          "the path's extension, so handoff really is a rename");
    std::remove(kExr);
  }

  // Scratch files: every path this section touches, removed unconditionally,
  // whether or not the assertion that created it passed.
  for (const char* p : {"selftest_npaint_never.npaint", "selftest_npaint_gate.npaint",
                        "selftest_npaint_roundtrip.npaint", "selftest_npaint_carry.npaint",
                        "selftest_npaint_carry2.npaint", "selftest_npaint_as.exr"}) {
    std::remove(p);
  }

  std::printf("[selftest] npaint format %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runTileResidencyTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Which residency strategies this build actually has. A plain constant
  // rather than an #ifdef around each case, following
  // runFormatSupportTest()/runNpaintFormatTest()'s precedent and PLAN.md
  // §1.5's "an unexercised build option is not a seam".
  //
  // This section's relationship to that rule is different from step 4's,
  // though, and the difference is the point: `.npaint` is inherently an OIIO
  // feature, so its OFF build refuses by name and there is nothing else to
  // assert. **Residency is not a feature, it is a strategy.** The OFF build
  // has a complete, correct one -- Eager -- so most of what follows runs
  // identically in both configurations and asserts the *same* answers, not
  // merely the correct answer for each build. Only the cached strategy, and
  // the measurements that exist to judge it, are ON-only.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  // --- Thresholds, derived from phase 1's measured latency ---------------
  //
  // There is no tolerance in this section: every pixel comparison below is at
  // **zero tolerance**, for exactly io/NpaintFile.hpp's reason. A cached
  // fetch asks OpenImageIO for TypeDesc::HALF and receives the file's own
  // half words; the eager path memcpy'd those same words out of the same
  // file. No float appears on either side, so a difference of one ulp is a
  // bug, not rounding. The claim holds and is asserted as equality of 128 KiB
  // memcmp, not as a norm.
  //
  // What does need deriving is how slow a fetch may be before the residency
  // costs more than it saves, and that comes from a measurement this project
  // already made rather than from a target invented here. PLAN.md's Findings
  // record phase 1.1's pen-to-photon baseline: **p50 12.1-12.4 ms, p99
  // 15.7-16.4 ms**, against PRD F3's < 20 ms. So one frame's worth of work is
  // ~12.1 ms at p50.
  //
  // The worst honest demand on the residency is redrawing a full viewport:
  // ADR-0001's amendment sizes that at 2560x1440, which is
  // ceil(2560/128) x ceil(1440/128) = 20 x 12 = **240 tiles**. For a viewport
  // refresh to fit inside one p50 frame and leave nothing for anything else,
  // a fetch must cost at most 12.1 ms / 240 = **50.4 us**.
  //
  // That is the bound. It is asserted against the *warm* fetch, which is the
  // steady state a second and subsequent frame see, and where the measured
  // cost is 3.1-4.5 us -- ~11x of headroom.
  constexpr double kWarmFetchBudgetUs = 50.4;
  //
  // The **cold** fetch is deliberately given a much looser ceiling, and the
  // reason is a finding rather than a convenience: the measured cold cost of
  // 49-62 us/tile is *at or just past* the 50.4 us bound, so a fully cold
  // 2560x1440 viewport does not fit in one frame. That is reported in the
  // output rather than asserted away, because it is true and it is what a
  // reader of this section needs to know. What is asserted is a regression
  // guard well clear of machine noise and of a cold filesystem cache: 500 us
  // is ~8x the measured value, and still 3x better than the 1549 us/tile that
  // the untiled-source path measured, which is the thing this number exists
  // to stay on the right side of.
  constexpr double kColdFetchCeilingUs = 500.0;

  // --- Fixture -----------------------------------------------------------
  //
  // 2048x2048: 16x16 = 256 tiles, 32.00 MiB of half words. Chosen so the
  // document is genuinely larger than the eviction budget the test sets
  // later, which is the only way eviction can be *proven* rather than
  // assumed, and large enough that "resident bytes" is a number worth
  // comparing.
  constexpr int32_t kCanvas = 2048;
  constexpr int32_t kTilesPerSide = kCanvas / kTileSize;  // 16
  constexpr size_t kTileBytes = sizeof(Tile);             // 128 KiB

  // Content is deterministic and every tile differs from every other, so a
  // comparison cannot pass by two tiles happening to be identical. Built by
  // filling one tile's worth of half words once and then stamping the tile's
  // own coordinate into a few texels, rather than calling floatToHalf 16.7
  // million times -- the fixture is not what is under test.
  auto buildFixture = [&]() {
    Document doc = Document::createBlank(kCanvas, kCanvas, WorkingSpace{});
    doc.layers[0].name = "Source";
    TileStore& tiles = *doc.layers[0].rgbTiles;

    std::array<uint16_t, Tile::kTexelCount> base{};
    for (size_t i = 0; i < Tile::kTexelCount; i += 4) {
      const float t = static_cast<float>(i % 8192) / 8192.0f;
      base[i + 0] = floatToHalf(t);
      base[i + 1] = floatToHalf(1.0f - t);
      base[i + 2] = floatToHalf(t * t);
      base[i + 3] = floatToHalf(1.0f);  // opaque: keeps the composite simple
    }
    for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
      for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
        Tile& tile = tiles.getOrCreate(TileCoord{tx, ty});
        std::memcpy(tile.data(), base.data(), Tile::kTexelCount * sizeof(uint16_t));
        // Stamp the coordinate, so no two tiles are byte-equal.
        for (int32_t k = 0; k < 8; ++k) {
          const float a = static_cast<float>(tx * 37 + ty * 11 + k) / 512.0f;
          tile.writePixel(PixelCoord{k, k}, {a, 1.0f - a, a * 0.5f, 1.0f});
        }
      }
    }
    return doc;
  };

  // --- Part A: the interface itself, identical in both configurations -----

  {
    // Eager residency is today's behaviour restated as one implementation of
    // the interface, so it is asserted the same way in both builds.
    TileStore store;
    store.getOrCreate(TileCoord{1, 1}).writePixel(PixelCoord{0, 0}, {0.5f, 0.25f, 0.125f, 1.0f});
    store.getOrCreate(TileCoord{4, 2});
    LayerResidency eager = LayerResidency::adoptEager(std::move(store));

    check(eager.mode() == TileResidencyMode::Eager && eager.source().path.empty(),
          "eager residency has no backing file -- the source was read in full at open");
    check(eager.ownedTileCount() == 2 && eager.residentBytes() == 2 * kTileBytes,
          "eager: every tile is ours, and resident bytes are exactly the tiles");

    const TileFetch present = eager.readTile(TileCoord{1, 1});
    check(present.status == TileFetchStatus::Owned && present.tile != nullptr,
          "eager: an existing tile reads back as Owned, not Clean -- in eager mode there is "
          "no such thing as a tile that came from the file and is not ours");
    check(std::fabs(present.tile->readPixel(PixelCoord{0, 0})[0] - 0.5f) < 1e-3f,
          "eager: and it is the tile that was put in");

    const TileFetch missing = eager.readTile(TileCoord{9, 9});
    check(missing.status == TileFetchStatus::Absent && missing.tile == nullptr,
          "eager: an unallocated tile is Absent -- the same answer TileStore::find() gives, "
          "and it means transparent black rather than an error");

    check(!eager.isOwned(TileCoord{9, 9}), "eager: and Absent is not Owned");
    std::string writeError;
    Tile* fresh = eager.tileForWrite(TileCoord{9, 9}, &writeError);
    check(fresh != nullptr && writeError.empty(),
          "eager: writing to an unallocated tile promotes it from transparent black, which "
          "is not a failure -- that is what an absent tile already means");
    check(eager.isOwned(TileCoord{9, 9}) && eager.ownedTileCount() == 3 &&
              eager.residentBytes() == 3 * kTileBytes,
          "eager: and it is now owned, and counted");
    check(eager.readTile(TileCoord{9, 9}).status == TileFetchStatus::Owned,
          "eager: a promoted tile reads back as Owned");
    check(eager.tileForWrite(TileCoord{9, 9}) == fresh && eager.ownedTileCount() == 3,
          "copy-on-FIRST-write: a second write returns the same tile and promotes nothing "
          "again");
  }

  {
    // npaintLayerTileSource() is pure index arithmetic over data io/NpaintFile
    // already produced, so it runs and asserts the same answers in both
    // builds. The carry is hand-built rather than loaded, precisely so the
    // OFF build can check the mapping too.
    NpaintCarry carry;
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::Layer, 0});
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::RawPart, 0});
    carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::Layer, 1});

    const std::optional<TileSourceRef> first = npaintLayerTileSource("doc.npaint", carry, 0);
    const std::optional<TileSourceRef> second = npaintLayerTileSource("doc.npaint", carry, 1);
    const std::optional<TileSourceRef> absent = npaintLayerTileSource("doc.npaint", carry, 7);

    check(first.has_value() && first->subimage == 1 && first->miplevel == 0 &&
              first->path == "doc.npaint",
          "npaintLayerTileSource: layer 0 is subimage 1 -- part 0 is the composite");
    check(second.has_value() && second->subimage == 3,
          "npaintLayerTileSource: a carried foreign part between the layers shifts the "
          "second layer to subimage 3, because subimages are file order and so is partOrder");
    check(!absent.has_value(),
          "npaintLayerTileSource: a layer with no part behind it returns nullopt rather than "
          "naming a subimage that does not hold it");
  }

  // --- Part B: the cached strategy ---------------------------------------

  const char* kDocPath = "selftest_residency_doc.npaint";
  const char* kCopyPath = "selftest_residency_copy.npaint";
  const char* kTruncPath = "selftest_residency_trunc.npaint";
  const char* kMutPath = "selftest_residency_mut.npaint";
  const char* kPngPath = "selftest_residency_untiled.png";
  for (const char* p : {kDocPath, kCopyPath, kTruncPath, kMutPath, kPngPath}) std::remove(p);

  if (!kOiioBuild) {
    // The refusal, and the fact that it names a real alternative rather than
    // just a missing build option -- which is the whole difference between
    // this step and step 4.
    TileSourceRef ref;
    ref.path = kDocPath;
    ref.subimage = 1;
    LayerResidency cached;
    std::string error;
    const bool opened = openCachedLayerResidency(ref, kTileCacheBudgetBytes, &cached, &error);
    check(!opened, "OFF build: a cached residency cannot be opened");
    check(contains(error, "NP_USE_OIIO") && contains(error, "-DNP_USE_OIIO=ON") &&
              contains(error, kDocPath),
          "OFF build: the refusal names the build option, the cmake line that enables it, "
          "and the file");
    check(contains(error, "Eager") && contains(error, "identically"),
          "OFF build: and names Eager as a COMPLETE alternative -- unlike .npaint, nothing "
          "is lost here, because PRD I1/I3 require opening and painting a file to behave the "
          "same in this build");
    check(cached.mode() == TileResidencyMode::Eager && cached.residentBytes() == 0,
          "OFF build: a refused open leaves the destination untouched, not half-built");

    TileCacheStats stats;
    check(!tileCacheStatistics(&stats),
          "OFF build: there is no cache to report statistics for, and saying so beats "
          "reporting zeros that look like an empty cache");
    check(!tileCacheSetBudgetBytes(kTileCacheBudgetBytes),
          "OFF build: and no budget to set");
  }

  if (kOiioBuild) {
    // Save the fixture, then load it back the eager way. That gives both a
    // real tiled file and the reference the cached path is compared against.
    Document fixture = buildFixture();
    const NpaintSaveResult saved = saveNpaint(fixture, kDocPath);
    check(saved.ok && saved.partsWritten == 2,
          "a 2048x2048 document saves as a tiled .npaint (composite + one layer part)");

    NpaintLoadResult eagerLoad = loadNpaint(kDocPath);
    check(eagerLoad.ok && eagerLoad.document.layers.size() == 1,
          "and loads back the eager way -- the reference every comparison below uses");

    if (saved.ok && eagerLoad.ok) {
      LayerResidency eager =
          LayerResidency::adoptEager(std::move(*eagerLoad.document.layers[0].rgbTiles));
      eagerLoad.document.layers[0].rgbTiles.emplace();

      const std::optional<TileSourceRef> ref =
          npaintLayerTileSource(kDocPath, eagerLoad.carry, 0);
      check(ref.has_value() && ref->subimage == 1,
            "the layer's pixels are subimage 1 of the .npaint, via the part order "
            "io/NpaintFile already recorded");

      LayerResidency cached;
      std::string openError;
      const bool opened =
          ref.has_value() &&
          openCachedLayerResidency(*ref, kTileCacheBudgetBytes, &cached, &openError);
      check(opened,
            "**docs/document-format.md §1's claim, tested**: our own .npaint opens as a "
            "cached residency -- the ImageCache serves OUR documents, not only imports");
      if (!opened) std::printf("    open error: %s\n", openError.c_str());

      if (opened) {
        check(cached.mode() == TileResidencyMode::Cached && cached.dataX() == 0 &&
                  cached.dataY() == 0 && cached.dataWidth() == kCanvas &&
                  cached.dataHeight() == kCanvas,
              "the cached residency's data window is the layer part's own, tile-aligned");

        // ---- The headline measurement --------------------------------------
        const size_t eagerBytes = eager.residentBytes();
        const size_t cachedBytes = cached.residentBytes();
        std::printf(
            "    resident in OUR memory, %dx%d document: eager %.2f MiB (%zu tiles) vs "
            "cached %.2f MiB (%zu tiles + 1 staging)\n",
            kCanvas, kCanvas, static_cast<double>(eagerBytes) / 1048576.0,
            eager.ownedTileCount(), static_cast<double>(cachedBytes) / 1048576.0,
            cached.ownedTileCount());
        check(eagerBytes == static_cast<size_t>(kTilesPerSide) * kTilesPerSide * kTileBytes,
              "eager residency holds the whole document: 256 tiles, 32.00 MiB");
        check(cachedBytes == kTileBytes && cached.ownedTileCount() == 0,
              "cached residency holds ONE staging tile and owns nothing -- an unmodified "
              "tile is not in our memory at all");

        // ---- Bit-identity, at zero tolerance --------------------------------
        size_t compared = 0, identical = 0, absentBoth = 0, mismatched = 0;
        for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
          for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
            const TileCoord coord{tx, ty};
            const Tile* eagerTile = eager.ownedTiles().find(coord);
            const TileFetch got = cached.readTile(coord);
            ++compared;
            if (eagerTile == nullptr) {
              // The eager reader drops all-zero tiles; the cache has no such
              // rule and returns the zeros the file holds. Equal content,
              // different representation -- asserted rather than glossed.
              const bool cachedIsZero =
                  got.status == TileFetchStatus::Clean &&
                  std::all_of(got.tile->data(), got.tile->data() + Tile::kTexelCount,
                              [](uint16_t w) { return w == 0; });
              if (got.status == TileFetchStatus::Absent || cachedIsZero) {
                ++absentBoth;
              } else {
                ++mismatched;
              }
              continue;
            }
            if (got.status == TileFetchStatus::Clean &&
                std::memcmp(eagerTile->data(), got.tile->data(),
                            Tile::kTexelCount * sizeof(uint16_t)) == 0) {
              ++identical;
            } else {
              ++mismatched;
            }
          }
        }
        std::printf("    tiles compared %zu: bit-identical %zu, both-empty %zu, mismatched %zu\n",
                    compared, identical, absentBoth, mismatched);
        check(compared == 256 && identical == 256 && mismatched == 0,
              "**every tile served from the cache is BIT-IDENTICAL to the eager path** -- "
              "zero tolerance, memcmp of all 128 KiB, because half words go in and half "
              "words come out with no conversion stage anywhere");
        check(cached.cleanFetchCount() == 256 && cached.promotionCount() == 0,
              "and reading 256 tiles promoted none of them -- readTile() never makes a tile "
              "ours");
        check(cached.residentBytes() == kTileBytes,
              "and our resident bytes did not move: 256 tiles were read, one staging tile "
              "was held");

        // ---- Cost of a cold fetch vs a resident one -------------------------
        //
        // Timed in-process over many iterations. No subprocess anywhere near
        // this loop: spawning one dominates the measurement, which is a trap
        // this project has already fallen into once.
        tileCacheInvalidate(kDocPath);
        auto t0 = std::chrono::steady_clock::now();
        for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
          for (int32_t tx = 0; tx < kTilesPerSide; ++tx) (void)cached.readTile(TileCoord{tx, ty});
        auto t1 = std::chrono::steady_clock::now();
        const double coldUs =
            std::chrono::duration<double, std::micro>(t1 - t0).count() / 256.0;

        constexpr int kWarmIterations = 20000;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kWarmIterations; ++i) {
          (void)cached.readTile(TileCoord{i % kTilesPerSide, (i / kTilesPerSide) % kTilesPerSide});
        }
        t1 = std::chrono::steady_clock::now();
        const double warmUs =
            std::chrono::duration<double, std::micro>(t1 - t0).count() / kWarmIterations;

        std::printf(
            "    fetch cost: cold %.2f us/tile (256 tiles), warm %.3f us/tile (%d fetches); "
            "one 2560x1440 viewport = 240 tiles -> cold %.1f ms, warm %.2f ms against phase "
            "1's 12.1 ms p50 frame\n",
            coldUs, warmUs, kWarmIterations, coldUs * 240.0 / 1000.0, warmUs * 240.0 / 1000.0);
        check(warmUs < kWarmFetchBudgetUs,
              "a warm fetch is inside the derived per-tile interactive budget (12.1 ms p50 / "
              "240 viewport tiles = 50.4 us)");
        check(coldUs < kColdFetchCeilingUs,
              "a cold fetch stays far inside the regression ceiling -- and see the printed "
              "cold viewport figure, which does NOT fit in one frame");

        // ---- The cache's own accounting -------------------------------------
        TileCacheStats stats;
        const bool haveStats = tileCacheStatistics(&stats);
        check(haveStats, "OpenImageIO's own ImageCache statistics are readable");
        if (haveStats) {
          std::printf(
              "    ImageCache: %.2f MiB used of %.2f MiB budget; tiles created %d, current "
              "%d, peak %d; images total %.2f MiB uncompressed\n",
              static_cast<double>(stats.memoryUsedBytes) / 1048576.0,
              static_cast<double>(stats.budgetBytes) / 1048576.0, stats.tilesCreated,
              stats.tilesCurrent, stats.tilesPeak,
              static_cast<double>(stats.imageSizeBytes) / 1048576.0);
          check(stats.memoryUsedBytes > 0 && stats.tilesCurrent > 0,
                "the cache is genuinely holding the tiles our memory is not");
          check(stats.budgetBytes >= static_cast<int64_t>(kTileCacheBudgetBytes) - 1048576,
                "and it is holding them under the budget this module set");
        }

        // ---- Eviction, proven rather than assumed ---------------------------
        //
        // Shrink the budget below the document, sweep every tile, then ask
        // for the FIRST tile again. If `tiles_created` grows, that specific
        // tile was dropped and had to be re-read -- which is evidence, where
        // "current < created" alone would only be a hint.
        constexpr size_t kSmallBudget = 4ull * 1024 * 1024;
        check(tileCacheSetBudgetBytes(kSmallBudget), "the budget can be shrunk at run time");
        // Invalidating first is load-bearing, and it was found by the test
        // failing rather than by reading the documentation: **shrinking the
        // budget evicts nothing on its own.** OpenImageIO frees tiles when it
        // *adds* one that would exceed the budget, so with every tile of this
        // document already resident, a re-read is a hit and the first version
        // of this check measured 256 of 256 tiles still resident under a
        // 4 MiB budget. Making the sweep cold is what puts the cache in the
        // state where the budget is actually enforced -- and it is also the
        // honest state to measure, since a budget only ever bites while
        // reading something new.
        tileCacheInvalidate(kDocPath);
        for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
          for (int32_t tx = 0; tx < kTilesPerSide; ++tx) (void)cached.readTile(TileCoord{tx, ty});
        TileCacheStats sweep;
        tileCacheStatistics(&sweep);
        (void)cached.readTile(TileCoord{0, 0});
        TileCacheStats after;
        tileCacheStatistics(&after);
        std::printf(
            "    under a %.0f MiB budget: %.2f MiB held, %d tiles resident of %d created; "
            "re-reading tile (0,0) took created %d -> %d\n",
            static_cast<double>(kSmallBudget) / 1048576.0,
            static_cast<double>(sweep.memoryUsedBytes) / 1048576.0, sweep.tilesCurrent,
            sweep.tilesCreated, sweep.tilesCreated, after.tilesCreated);
        check(sweep.tilesCurrent < 256,
              "under a budget smaller than the document, fewer tiles are resident than the "
              "document has");
        check(after.tilesCreated > sweep.tilesCreated,
              "**eviction really happened**: re-reading an already-swept tile created a new "
              "cache tile, so that exact tile had been dropped");
        check(cached.residentBytes() == kTileBytes,
              "and none of that touched our own memory -- eviction is the cache's business");

        // ---- Copy-on-first-write, and paint surviving an eviction -----------
        const TileCoord painted{2, 3};
        std::string promoteError;
        Tile* owned = cached.tileForWrite(painted, &promoteError);
        check(owned != nullptr && promoteError.empty(),
              "copy-on-first-write: a clean tile promotes to an owned one");
        if (owned != nullptr) {
          const Tile* reference = eager.ownedTiles().find(painted);
          check(reference != nullptr &&
                    std::memcmp(owned->data(), reference->data(),
                                Tile::kTexelCount * sizeof(uint16_t)) == 0,
                "and the promoted tile starts as the FILE's pixels, bit for bit -- not as "
                "zeros, which would erase what the stroke was painted on top of");
          check(cached.isOwned(painted) && cached.ownedTileCount() == 1 &&
                    cached.residentBytes() == 2 * kTileBytes,
                "and it is now ours: one owned tile plus the staging tile");

          // Paint one distinctive texel.
          const PixelCoord brush{11, 13};
          owned->writePixel(brush, {0.875f, 0.0f, 0.375f, 1.0f});

          // Force the cache to churn hard enough that this tile's *clean*
          // copy is certainly gone, then read the tile back.
          for (int pass = 0; pass < 2; ++pass)
            for (int32_t ty = 0; ty < kTilesPerSide; ++ty)
              for (int32_t tx = 0; tx < kTilesPerSide; ++tx)
                if (!(tx == painted.x && ty == painted.y))
                  (void)cached.readTile(TileCoord{tx, ty});
          tileCacheInvalidate(kDocPath);

          const TileFetch back = cached.readTile(painted);
          check(back.status == TileFetchStatus::Owned,
                "after eviction AND a full cache invalidation, the painted tile still reads "
                "as Owned -- it stopped being the cache's business at the moment it was "
                "written");
          bool paintSurvived = false, restIntact = false;
          if (back.tile != nullptr) {
            const std::array<float, 4> px = back.tile->readPixel(brush);
            paintSurvived = std::fabs(px[0] - 0.875f) < 1e-3f && px[1] == 0.0f &&
                            std::fabs(px[2] - 0.375f) < 1e-3f;
            if (reference != nullptr) {
              // Everything except the painted texel must still be the file's.
              const size_t brushIndex =
                  (static_cast<size_t>(brush.y) * kTileSize + brush.x) * Tile::kChannels;
              restIntact = std::memcmp(back.tile->data(), reference->data(),
                                       brushIndex * sizeof(uint16_t)) == 0 &&
                           std::memcmp(back.tile->data() + brushIndex + 4,
                                       reference->data() + brushIndex + 4,
                                       (Tile::kTexelCount - brushIndex - 4) * sizeof(uint16_t)) ==
                               0;
            }
          }
          check(paintSurvived,
                "**the paint survived the eviction** -- the cache is not a write-back cache "
                "and an owned tile is never re-fetched");
          check(restIntact,
                "and the rest of that tile is still the file's pixels, bit for bit, so the "
                "promotion copied rather than reconstructed");

          // An untouched neighbour is still clean and still correct after all
          // that churn, which is what proves the promotion was local.
          const TileCoord neighbour{3, 3};
          const TileFetch fresh = cached.readTile(neighbour);
          const Tile* neighbourRef = eager.ownedTiles().find(neighbour);
          check(fresh.status == TileFetchStatus::Clean && neighbourRef != nullptr &&
                    std::memcmp(fresh.tile->data(), neighbourRef->data(),
                                Tile::kTexelCount * sizeof(uint16_t)) == 0,
                "an untouched neighbour is still Clean and still bit-identical -- promoting "
                "one tile did not make the rest ours");
        }
        check(tileCacheSetBudgetBytes(kTileCacheBudgetBytes), "the budget is restored");

        // ---- Outside the data window ----------------------------------------
        const TileFetch beyond = cached.readTile(TileCoord{kTilesPerSide + 4, 0});
        check(beyond.status == TileFetchStatus::Absent && beyond.tile == nullptr,
              "a tile outside the data window is Absent, NOT a successful read of zeros -- "
              "measured, OpenImageIO returns success with a zero fill there, and serving "
              "that would turn 'the file says nothing' into 'the file says transparent'");
      }

      // ---- The composite part is cacheable too ------------------------------
      TileSourceRef compositeRef;
      compositeRef.path = kDocPath;
      compositeRef.subimage = 0;
      LayerResidency composite;
      std::string compositeError;
      check(openCachedLayerResidency(compositeRef, kTileCacheBudgetBytes, &composite,
                                     &compositeError),
            "part 0, the composite, opens as a cached residency as well -- every part this "
            "build writes is tiled HALF RGBA, so the whole container is cache-addressable");
    }

    // ---- An untiled source is refused, with the measurement in the message
    {
      std::vector<uint16_t> px(16 * 16 * 4, 0x8000);
      const std::vector<uint8_t> png = encodePng16(16, 16, px.data());
      FILE* f = std::fopen(kPngPath, "wb");
      if (f != nullptr) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
      }
      TileSourceRef ref;
      ref.path = kPngPath;
      LayerResidency residency;
      std::string error;
      const bool opened =
          openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
      check(!opened, "an untiled (scanline) source is refused for cached residency");
      check(contains(error, "scanline-stored") && contains(error, "Eager"),
            "and the refusal names the storage and points at Eager, rather than offering a "
            "mode that measured 1549 us per scattered cold tile against 49 us for a tiled "
            "one");
    }

    // ---- Missing file: fail loudly, and NEVER zero-fill a promotion --------
    {
      NpaintLoadResult src = loadNpaint(kDocPath);
      if (src.ok) {
        const NpaintSaveResult copy = saveNpaint(src.document, kCopyPath, {}, &src.carry);
        TileSourceRef ref;
        ref.path = kCopyPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string error;
        const bool opened =
            copy.ok && openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
        check(opened, "a copy of the document opens as a cached residency");
        if (opened) {
          check(residency.readTile(TileCoord{0, 0}).status == TileFetchStatus::Clean,
                "and serves a tile while the file is there");
          std::remove(kCopyPath);
          const TileFetch gone = residency.readTile(TileCoord{5, 5});
          check(gone.status == TileFetchStatus::Failed && gone.tile == nullptr &&
                    contains(gone.error, kCopyPath),
                "with the file removed, a fetch Fails by name and serves no pixels at all");

          std::string promoteError;
          Tile* promoted = residency.tileForWrite(TileCoord{6, 6}, &promoteError);
          check(promoted == nullptr && !promoteError.empty(),
                "**and a promotion refuses rather than starting from zeros** -- zero-filling "
                "here would mean one stroke silently erased the image underneath it");
          check(!residency.isOwned(TileCoord{6, 6}) && residency.ownedTileCount() == 0,
                "a refused promotion leaves nothing owned behind");
        }
        tileCacheInvalidate(kCopyPath);
      }
    }

    // ---- Truncated file ----------------------------------------------------
    {
      FILE* in = std::fopen(kDocPath, "rb");
      if (in != nullptr) {
        std::fseek(in, 0, SEEK_END);
        const long size = std::ftell(in);
        std::fseek(in, 0, SEEK_SET);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        const size_t read = std::fread(bytes.data(), 1, bytes.size(), in);
        std::fclose(in);
        // Keep the header (EXR headers and the tile offset table live at the
        // front) and drop most of the pixel data, so the open can succeed and
        // the fetch is what fails -- the more interesting of the two paths.
        FILE* out = std::fopen(kTruncPath, "wb");
        if (out != nullptr) {
          std::fwrite(bytes.data(), 1, read / 4, out);
          std::fclose(out);
        }
        TileSourceRef ref;
        ref.path = kTruncPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string openError;
        const bool opened =
            openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &openError);
        bool refusedSomewhere = !opened;
        std::string message = openError;
        if (opened) {
          // The last tile is certainly past the truncation point.
          const TileFetch got =
              residency.readTile(TileCoord{kTilesPerSide - 1, kTilesPerSide - 1});
          refusedSomewhere = got.status == TileFetchStatus::Failed && got.tile == nullptr;
          message = got.error;
        }
        check(refusedSomewhere && !message.empty(),
              "a truncated file is refused -- at open or at the first fetch past the "
              "truncation -- and never serves partial or zeroed pixels as if they were the "
              "file's");
        check(contains(message, kTruncPath),
              "and the refusal names the file rather than wearing OpenEXR's wording alone");
        tileCacheInvalidate(kTruncPath);
      }
    }

    // ---- Changed on disk after open ---------------------------------------
    {
      NpaintLoadResult src = loadNpaint(kDocPath);
      if (src.ok) {
        const NpaintSaveResult first = saveNpaint(src.document, kMutPath, {}, &src.carry);
        TileSourceRef ref;
        ref.path = kMutPath;
        ref.subimage = 1;
        LayerResidency residency;
        std::string error;
        const bool opened =
            first.ok && openCachedLayerResidency(ref, kTileCacheBudgetBytes, &residency, &error);
        check(opened, "a document opens as a cached residency, and its identity is stamped");
        if (opened) {
          check(residency.readTile(TileCoord{1, 1}).status == TileFetchStatus::Clean,
                "and serves tiles from the file it stamped");
          // Rewrite the same path with a materially different document, so
          // both size and mtime move.
          Document replacement = Document::createBlank(kCanvas, kCanvas, WorkingSpace{});
          replacement.layers[0]
              .rgbTiles->getOrCreate(TileCoord{1, 1})
              .writePixel(PixelCoord{0, 0}, {1.0f, 1.0f, 1.0f, 1.0f});
          const NpaintSaveResult second = saveNpaint(replacement, kMutPath);
          check(second.ok, "the file is then rewritten underneath the open residency");
          const TileFetch stale = residency.readTile(TileCoord{1, 1});
          check(stale.status == TileFetchStatus::Failed && stale.tile == nullptr,
                "**the next fetch Fails rather than serving what the cache still holds** -- "
                "measured, OpenImageIO's cache does not notice a file changing and would "
                "have served the old pixels indefinitely");
          check(contains(stale.error, "changed on disk") && contains(stale.error, kMutPath),
                "and says so by name, so a stale document is a loud error rather than a "
                "quiet wrong picture");
        }
        tileCacheInvalidate(kMutPath);
      }
    }

    tileCacheInvalidate(kDocPath);
  }

  // Scratch files: every path this section touches, removed unconditionally,
  // whether or not the assertion that created it passed.
  for (const char* p : {kDocPath, kCopyPath, kTruncPath, kMutPath, kPngPath}) std::remove(p);

  std::printf("[selftest] tile residency %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runExportAsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // Which backend set was compiled in -- a plain constant rather than an
  // #ifdef around each case, exactly as runFormatSupportTest()/
  // runNpaintFormatTest()/runTileResidencyTest() carry it, so both
  // configurations execute the same assertions and each states the correct
  // answer for its build. That matters more here than anywhere: the whole
  // point of the preset behaviour under test is what an NP_USE_OIIO=ON
  // preset does in an OFF build.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif
  check(oiioBackendCompiledIn() == kOiioBuild,
        "the capability query and this test agree on which build this is");

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // (1) **The resampler.** ops/Resample accumulates in double (relative
  //     error ~1e-16 over the at most 64 terms any fixture below sums) and
  //     rounds exactly twice on the way out: once storing the horizontal
  //     pass's result as float, once storing the final un-premultiplied
  //     value. Each rounding is at most half a float ulp, i.e. 6e-8 relative,
  //     so for the [0,1] values used here the bound is 2 * 6e-8 = 1.2e-7.
  //     Landed 1.0e-6, 8.3x the bound -- deliberately looser than the ~1.4x
  //     the round-trip tolerances elsewhere in this file use, because these
  //     comparisons are against hand-computed decimal references (0.3666667
  //     for (0.2 + 0.35)/1.5) whose own decimal-to-float conversion is worth
  //     more headroom than the arithmetic is.
  constexpr float kResampleTol = 1.0e-6f;
  // (2) **The linear-light proof's 8-bit round trip.** A 2x2 checker of
  //     linear 0 and 1 halved must land on linear 0.5, written through the
  //     sRGB curve at 8 bits: srgbEncode(0.5) = 0.73535, times 255 = 187.51,
  //     quantized to code 188. That rounding moves the encoded value by
  //     (188 - 187.51)/255 = 1.92e-3, and the sRGB *decode* slope at that
  //     point -- 2.4/1.055 * ((0.737255 + 0.055)/1.055)^1.4 = 1.5194 --
  //     turns it into a linear error of 2.92e-3. Landed 4.0e-3, 1.37x the
  //     bound, the same headroom ratio runExportTest()'s own tolerances use.
  //     Measured below and printed, so the derivation is checkable.
  constexpr float kLinearLightTol = 4.0e-3f;

  auto writeStraight = [](Document& doc, int32_t x, int32_t y, float r, float g, float b,
                          float a) {
    TileStore& tiles = *doc.layers[0].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto makeImage = [](uint32_t w, uint32_t h) {
    DecodedImage img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0.0f);
    return img;
  };
  auto setPixel = [](DecodedImage& img, uint32_t x, uint32_t y, float r, float g, float b,
                     float a) {
    float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  };
  auto sampleAt = [](const std::vector<float>& v, uint32_t w, uint32_t x, uint32_t y, int c) {
    return v[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(c)];
  };

  // --- What the dialog is allowed to offer (PRD I3, I5) ------------------
  //
  // The dialog builds its combo boxes from exactly these two functions, so
  // "the dialog can never offer a combination io/Export will refuse" is
  // checkable here rather than by clicking.
  {
    const std::vector<ImageFormat> formats = offerableExportFormats();
    auto offered = [&](ImageFormat f) {
      return std::find(formats.begin(), formats.end(), f) != formats.end();
    };
    check(offered(ImageFormat::Png) && offered(ImageFormat::Jpeg) &&
              offered(ImageFormat::Tga) && offered(ImageFormat::Bmp),
          "PRD I1's four formats are offerable in BOTH build configurations");
    check(offered(ImageFormat::Exr) == kOiioBuild && offered(ImageFormat::Tiff) == kOiioBuild &&
              offered(ImageFormat::Hdr) == kOiioBuild && offered(ImageFormat::Dpx) == kOiioBuild,
          "EXR/TIFF/HDR/DPX are offerable exactly when the OIIO backend is compiled in");
    check(!offered(ImageFormat::Psd) && !offered(ImageFormat::CameraRaw),
          "the read-only formats are never offered as export targets, in either build");

    const std::vector<ExportBitDepth> png = offerableExportDepths(ImageFormat::Png);
    check(png.size() == 2 && png[0] == ExportBitDepth::UInt8 && png[1] == ExportBitDepth::UInt16,
          "PNG offers exactly 8- and 16-bit integer -- no float depth it cannot write");
    const std::vector<ExportBitDepth> jpeg = offerableExportDepths(ImageFormat::Jpeg);
    check(jpeg.size() == 1 && jpeg[0] == ExportBitDepth::UInt8,
          "JPEG offers 8-bit only, so 16-bit-into-JPEG is unreachable from the dialog");
    const std::vector<ExportBitDepth> exr = offerableExportDepths(ImageFormat::Exr);
    check(exr.size() == (kOiioBuild ? 2u : 0u) &&
              (!kOiioBuild ||
               (exr[0] == ExportBitDepth::Half && exr[1] == ExportBitDepth::Float32)),
          "EXR offers half and 32-bit float and NOT 8-bit -- the depth probe's answer, not a "
          "guess about what EXR 'should' do");
    check(offerableExportDepths(ImageFormat::Psd).empty(),
          "a format this build cannot write offers no depths at all");
  }

  // --- resolveExportSize(): every mode, hand-computed ---------------------
  {
    uint32_t w = 0, h = 0;
    std::string err;

    ExportResize none;
    check(resolveExportSize(none, 1024, 768, &w, &h, &err) && w == 1024 && h == 768,
          "resize None resolves to the document's own size");

    ExportResize pct;
    pct.mode = ExportResizeMode::Percent;
    pct.percent = 50.0f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 512 && h == 384,
          "resize 50% of 1024x768 is 512x384");
    pct.percent = 33.0f;
    // 1024 * 0.33 = 337.92 -> 338; 768 * 0.33 = 253.44 -> 253. Each axis
    // rounds independently, which is what a percentage means.
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 338 && h == 253,
          "resize 33% rounds each axis half-away-from-zero: hand-computed 338x253");
    pct.percent = 100.0f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 1024 && h == 768,
          "resize 100% is exactly the source size, not source-minus-rounding");
    pct.percent = 0.01f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 1 && h == 1,
          "a percentage that would round an axis to zero clamps to 1 -- 1024x0 is not an image");

    pct.percent = 150.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "enlarge") &&
              contains(err, "Fit within") && w == 0 && h == 0,
          "resize above 100% is refused by name, and points at the mode that does clamp");
    pct.percent = 0.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "not a size"),
          "resize 0% is refused rather than producing a zero-sized file");
    pct.percent = -25.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "not a size"),
          "a negative percentage is refused with the same named reason");

    ExportResize fit;
    fit.mode = ExportResizeMode::FitWithin;
    fit.maxWidth = 2048;
    fit.maxHeight = 2048;
    // 4000x1000 into a 2048 box: the width binds (2048/4000 = 0.512), so
    // 4000*0.512 = 2048 and 1000*0.512 = 512.
    check(resolveExportSize(fit, 4000, 1000, &w, &h, &err) && w == 2048 && h == 512,
          "fit-within picks the binding axis and preserves aspect: hand-computed 2048x512");
    check(resolveExportSize(fit, 100, 50, &w, &h, &err) && w == 100 && h == 50,
          "fit-within NEVER enlarges: a document smaller than the box exports at 1:1");
    fit.maxWidth = 0;
    check(!resolveExportSize(fit, 100, 50, &w, &h, &err) && contains(err, "at least 1 pixel"),
          "a fit-within box with a zero side is refused by name");
    check(!resolveExportSize(none, 0, 100, &w, &h, &err) && contains(err, "0x100"),
          "a zero-sized source is refused, and the message quotes the size");
  }

  // --- resampleAreaAverage(): against hand-computed references ------------
  {
    std::string err;
    std::vector<float> out;

    // (a) 4x2 -> 2x1. Every destination texel is the mean of a 2x2 block,
    // hand-computed: (0.0+0.2+0.8+1.0)/4 = 0.5 and (0.4+0.6+0.1+0.3)/4 = 0.35.
    const float rows[2][4] = {{0.0f, 0.2f, 0.4f, 0.6f}, {0.8f, 1.0f, 0.1f, 0.3f}};
    DecodedImage block = makeImage(4, 2);
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 4; ++x)
        setPixel(block, x, y, rows[y][x], rows[y][x], rows[y][x], 1.0f);
    check(resampleAreaAverage(block.pixels.data(), 4, 2, 2, 1, &out, &err) && out.size() == 8,
          "resample 4x2 -> 2x1 succeeds and produces exactly 2x1x4 samples");
    check(nearf(sampleAt(out, 2, 0, 0, 0), 0.5f, kResampleTol) &&
              nearf(sampleAt(out, 2, 1, 0, 0), 0.35f, kResampleTol),
          "resample 4x2 -> 2x1 matches the hand-computed 2x2 block means (0.5, 0.35)");
    check(sampleAt(out, 2, 0, 0, 3) == 1.0f && sampleAt(out, 2, 1, 0, 3) == 1.0f,
          "a fully opaque source stays EXACTLY opaque -- weights sum to 1 in double, so a "
          "downscale cannot make a document un-exportable to JPEG by rounding");

    // (b) 3x1 -> 2x1: a non-integer scale factor, where the fractional edge
    // weights are the whole point. Footprints are [0,1.5) and [1.5,3), so
    // dst0 = (s0 + 0.5*s1)/1.5 and dst1 = (0.5*s1 + s2)/1.5.
    const float s[3] = {0.2f, 0.7f, 0.1f};
    DecodedImage odd = makeImage(3, 1);
    for (uint32_t x = 0; x < 3; ++x) setPixel(odd, x, 0, s[x], s[x], s[x], 1.0f);
    const float expect0 = (0.2f + 0.5f * 0.7f) / 1.5f;  // 0.3666667
    const float expect1 = (0.5f * 0.7f + 0.1f) / 1.5f;  // 0.3
    check(resampleAreaAverage(odd.pixels.data(), 3, 1, 2, 1, &out, &err) &&
              nearf(sampleAt(out, 2, 0, 0, 0), expect0, kResampleTol) &&
              nearf(sampleAt(out, 2, 1, 0, 0), expect1, kResampleTol),
          "a non-integer 3->2 reduction matches the hand-computed fractional-weight "
          "reference (0.366667, 0.300000)");

    // (c) A constant image survives a lopsided reduction bit-exactly. This
    // is the strongest statement available about the weight normalisation:
    // no ulp of drift anywhere, at zero tolerance.
    DecodedImage flat = makeImage(500, 30);
    for (uint32_t y = 0; y < 30; ++y)
      for (uint32_t x = 0; x < 500; ++x) setPixel(flat, x, y, 0.3f, 0.6f, 0.9f, 1.0f);
    bool bitExact = true;
    if (resampleAreaAverage(flat.pixels.data(), 500, 30, 61, 7, &out, &err)) {
      for (size_t i = 0; i < out.size(); i += 4) {
        if (out[i] != 0.3f || out[i + 1] != 0.6f || out[i + 2] != 0.9f || out[i + 3] != 1.0f)
          bitExact = false;
      }
    } else {
      bitExact = false;
    }
    check(bitExact,
          "a constant 500x30 image reduced to 61x7 is bit-identical to its own constant at "
          "ZERO tolerance -- every one of 1708 samples");

    // (d) 1:1 is a verbatim copy, not a premultiply/divide round trip.
    check(resampleAreaAverage(block.pixels.data(), 4, 2, 4, 2, &out, &err) &&
              out == block.pixels,
          "a 1:1 'resize' returns the source bit-for-bit, short-circuiting the alpha round "
          "trip that could otherwise cost an ulp");

    // (e) Every refusal, by its error string.
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 8, 2, &out, &err) &&
              contains(err, "enlarge") && contains(err, "the width") && out.empty(),
          "an upscale is refused by name, says which axis grew, and leaves no partial buffer");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 2, 4, &out, &err) &&
              contains(err, "the height"),
          "an upscale in the other axis names the height instead");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 8, 4, &out, &err) &&
              contains(err, "both axes"),
          "an upscale in both axes names both");
    check(!resampleAreaAverage(nullptr, 4, 2, 2, 1, &out, &err) && contains(err, "source pixels"),
          "a null source is refused rather than dereferenced");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 0, 1, &out, &err) &&
              contains(err, "at least 1 pixel"),
          "a zero destination dimension is refused by name");
  }

  // --- The phase 6 warning, measured rather than asserted -----------------
  //
  // PLAN.md phase 6: "**downscale must prefilter** (area average or descend
  // the mip pyramid); no reconstruction filter fixes aliasing after the
  // fact." Both patterns below have a known exact mean, so "how much of the
  // answer is alias" is a number, not an opinion. The naive path -- point
  // sampling the source at the destination grid -- is implemented here in
  // the test rather than shipped in ops/, because a wrong resampler has no
  // business being in the binary.
  {
    constexpr uint32_t kSrc = 512, kDst = 64;  // an exact factor of 8
    auto rmsAgainst = [](const std::vector<float>& v, uint32_t w, uint32_t h, float expected) {
      double acc = 0.0;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const double d = static_cast<double>(v[(static_cast<size_t>(y) * w + x) * 4]) -
                           static_cast<double>(expected);
          acc += d * d;
        }
      return static_cast<float>(std::sqrt(acc / (static_cast<double>(w) * h)));
    };
    auto pointSample = [](const DecodedImage& img, uint32_t dstW, uint32_t dstH) {
      std::vector<float> out(static_cast<size_t>(dstW) * dstH * 4u, 0.0f);
      for (uint32_t y = 0; y < dstH; ++y)
        for (uint32_t x = 0; x < dstW; ++x) {
          const uint32_t sx = x * (img.width / dstW);
          const uint32_t sy = y * (img.height / dstH);
          const float* p = &img.pixels[(static_cast<size_t>(sy) * img.width + sx) * 4];
          float* d = &out[(static_cast<size_t>(y) * dstW + x) * 4];
          for (int c = 0; c < 4; ++c) d[c] = p[c];
        }
      return out;
    };

    // (a) A 1-pixel checkerboard: exactly at the source Nyquist limit, mean
    // 0.5 everywhere, and the classic case a naive resize destroys utterly.
    DecodedImage checker = makeImage(kSrc, kSrc);
    for (uint32_t y = 0; y < kSrc; ++y)
      for (uint32_t x = 0; x < kSrc; ++x) {
        const float v = ((x + y) & 1u) ? 1.0f : 0.0f;
        setPixel(checker, x, y, v, v, v, 1.0f);
      }
    std::vector<float> filtered;
    std::string err;
    check(resampleAreaAverage(checker.pixels.data(), kSrc, kSrc, kDst, kDst, &filtered, &err),
          "aliasing fixture: a 512x512 1-px checkerboard reduces to 64x64");
    const std::vector<float> naive = pointSample(checker, kDst, kDst);
    const float filteredRms = rmsAgainst(filtered, kDst, kDst, 0.5f);
    const float naiveRms = rmsAgainst(naive, kDst, kDst, 0.5f);
    std::printf("    [measured] 1-px checker 512->64: prefiltered RMS error vs the true mean "
                "0.5 = %.3e, naive point-sample = %.3e -- the naive result is a uniformly "
                "%s 64x64 image with no trace of the pattern left\n",
                static_cast<double>(filteredRms), static_cast<double>(naiveRms),
                naive[0] < 0.5f ? "black" : "white");
    check(filteredRms <= 1e-6f,
          "prefiltered: every destination texel of the checker lands on the true mean 0.5, "
          "within a float ulp");
    check(naiveRms >= 0.49f,
          "naive: the same 50%-grey pattern collapses to a FLAT image at full amplitude -- "
          "the aliased answer is not noisy, it is confidently wrong");

    // (b) A period-3 stripe pattern, whose period does not divide the scale
    // factor. This is where the box kernel's own limit shows: its sinc
    // frequency response leaves a residual ripple, and the honest thing is
    // to measure it rather than claim the filter is perfect.
    DecodedImage stripes = makeImage(kSrc, kSrc);
    for (uint32_t y = 0; y < kSrc; ++y)
      for (uint32_t x = 0; x < kSrc; ++x) {
        const float v = (x % 3u == 0u) ? 1.0f : 0.0f;
        setPixel(stripes, x, y, v, v, v, 1.0f);
      }
    std::vector<float> stripesFiltered;
    check(resampleAreaAverage(stripes.pixels.data(), kSrc, kSrc, kDst, kDst, &stripesFiltered,
                              &err),
          "aliasing fixture: a period-3 stripe pattern reduces to 64x64");
    const std::vector<float> stripesNaive = pointSample(stripes, kDst, kDst);
    const float mean3 = 1.0f / 3.0f;
    const float stripesFilteredRms = rmsAgainst(stripesFiltered, kDst, kDst, mean3);
    const float stripesNaiveRms = rmsAgainst(stripesNaive, kDst, kDst, mean3);
    std::printf("    [measured] period-3 stripes 512->64: prefiltered RMS = %.4f, naive = "
                "%.4f (%.1fx worse)\n",
                static_cast<double>(stripesFilteredRms), static_cast<double>(stripesNaiveRms),
                stripesFilteredRms > 0.0f
                    ? static_cast<double>(stripesNaiveRms / stripesFilteredRms)
                    : 0.0);
    check(stripesFilteredRms < stripesNaiveRms / 5.0f,
          "prefiltered beats naive by more than 5x on a pattern whose period does not divide "
          "the scale factor -- the case a box kernel is NOT perfect on");
    check(stripesFilteredRms > 0.0f && stripesFilteredRms < 0.10f,
          "and the box kernel's own residual ripple is real but bounded -- reported, not "
          "claimed away");
  }

  // --- Linear light, proven by the number the file carries ----------------
  {
    DecodedImage checker = makeImage(2, 2);
    setPixel(checker, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    setPixel(checker, 1, 0, 0.0f, 0.0f, 0.0f, 1.0f);
    setPixel(checker, 0, 1, 0.0f, 0.0f, 0.0f, 1.0f);
    setPixel(checker, 1, 1, 1.0f, 1.0f, 1.0f, 1.0f);

    std::vector<float> half;
    std::string err;
    check(resampleAreaAverage(checker.pixels.data(), 2, 2, 1, 1, &half, &err) &&
              nearf(half[0], 0.5f, kResampleTol),
          "a 2x2 black/white checker halves to linear 0.5 -- the average of the LIGHT, not of "
          "the codes");

    DecodedImage one = makeImage(1, 1);
    one.pixels = half;
    const ExportResult enc = encodeLinearImage(one, WorkingSpace{}, ImageFormat::Png,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt8);
    check(enc.ok, "the halved checker encodes to an 8-bit sRGB PNG");
    if (enc.ok) {
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      check(back.valid() && back.width == 1 && back.height == 1,
            "and decodes back as a 1x1 image");
      if (back.valid()) {
        const float gotLinear = back.pixels[0];
        const float gotCode = srgbEncode(gotLinear) * 255.0f;
        // The wrong pipeline: average the *encoded* values instead of the
        // linear ones. srgbEncode(0) = 0 and srgbEncode(1) = 1, so it lands
        // on encoded 0.5 -- 8-bit code 128 -- which decodes to linear 0.214.
        const float wrongEncoded = 0.5f * (srgbEncode(0.0f) + srgbEncode(1.0f));
        const float wrongLinear = srgbDecode(wrongEncoded);
        std::printf("    [measured] halved checker: correct path -> 8-bit code %.1f (linear "
                    "%.4f); averaging the encoded values instead -> encoded %.4f, code %.1f "
                    "before rounding (linear %.4f), %.3f too dark\n",
                    static_cast<double>(gotCode), static_cast<double>(gotLinear),
                    static_cast<double>(wrongEncoded),
                    static_cast<double>(wrongEncoded * 255.0f),
                    static_cast<double>(wrongLinear),
                    static_cast<double>(0.5f - wrongLinear));
        check(nearf(gotLinear, 0.5f, kLinearLightTol),
              "the exported file decodes to linear 0.5 within the derived 8-bit tolerance");
        check(nearf(gotCode, 188.0f, 0.51f),
              "the file's own 8-bit code is 188 = round(255 * srgbEncode(0.5)), the hand-"
              "computed answer");
        check(!nearf(wrongLinear, 0.5f, 0.2f) && wrongLinear < 0.25f,
              "and the encoded-domain average -- the bug this ordering prevents -- would have "
              "landed at linear 0.214, more than half a stop dark");
      }
    }
  }

  // --- Alpha: filtered premultiplied, not straight ------------------------
  {
    // A fully transparent texel whose straight RGB is arbitrary green sits
    // next to an opaque red one. Under premultiplied filtering the green
    // contributes nothing at all; under a straight average it contaminates
    // half the result.
    DecodedImage pair = makeImage(2, 1);
    setPixel(pair, 0, 0, 1.0f, 0.0f, 0.0f, 1.0f);
    setPixel(pair, 1, 0, 0.0f, 1.0f, 0.0f, 0.0f);

    std::vector<float> out;
    std::string err;
    check(resampleAreaAverage(pair.pixels.data(), 2, 1, 1, 1, &out, &err), "alpha fixture resamples");
    if (out.size() == 4) {
      const float straightAverageGreen = 0.5f * (0.0f + 1.0f);
      std::printf("    [measured] transparent-green + opaque-red -> green channel %.4f "
                  "(premultiplied filtering); a straight-alpha average gives %.4f\n",
                  static_cast<double>(out[1]), static_cast<double>(straightAverageGreen));
      check(out[1] == 0.0f,
            "the fully transparent texel's colour contributes EXACTLY nothing -- zero, not "
            "nearly zero");
      check(nearf(out[0], 1.0f, kResampleTol),
            "and the surviving colour is the opaque texel's own, un-diluted by the "
            "transparent one");
      check(nearf(out[3], 0.5f, kResampleTol),
            "while alpha itself averages normally, to 0.5");
      check(straightAverageGreen > 0.4f,
            "control: the straight-alpha average this avoids really would have been 0.5 green");
    }
  }

  // --- One set of refusal strings, not two --------------------------------
  //
  // The claim io/ExportAs.hpp makes about the dialog: every message it shows
  // is io/Export's own. Asserted by string equality against both the shared
  // helper and a real encode, rather than by reading the code.
  {
    ExportRequest req;
    req.format = ImageFormat::Jpeg;
    req.bitDepth = ExportBitDepth::UInt16;
    const ExportValidation v = validateExportRequest(req, 8, 8, nullptr, nullptr);
    const std::string direct = exportRefusalReason(ImageFormat::Jpeg,
                                                   ExportTargetSpace::Rec709Srgb,
                                                   ExportBitDepth::UInt16, nullptr, nullptr);
    check(!v.ok && !direct.empty() && v.error == direct,
          "validateExportRequest quotes exportRefusalReason() byte for byte");

    Document doc = Document::createBlank(2, 2, WorkingSpace{});
    for (int32_t y = 0; y < 2; ++y)
      for (int32_t x = 0; x < 2; ++x) writeStraight(doc, x, y, 0.5f, 0.25f, 0.75f, 1.0f);
    const ExportResult enc = exportDocument(doc, ImageFormat::Jpeg,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(!enc.ok && enc.error == direct,
          "and a real encodeLinearImage() failure is that identical string -- there is one "
          "set of refusal strings in this binary, not a UI copy that can drift");
    check(contains(direct, "JPEG") && contains(direct, "16-bit integer"),
          "the shared string still names the format and the refused depth (PRD B6)");
  }

  // --- Every validation refusal, checked by its message -------------------
  {
    ExportRequest req;
    req.format = ImageFormat::Psd;
    std::string why = exportRequestAvailability(req);
    check(!why.empty() && contains(why, "PSD"),
          "a read-only format (PSD) is refused by name in both builds");
    req.format = ImageFormat::CameraRaw;
    why = exportRequestAvailability(req);
    check(!why.empty() && contains(why, "camera raw"),
          "camera raw is refused by name in both builds");

    req.format = ImageFormat::Exr;
    req.bitDepth = ExportBitDepth::Half;
    why = exportRequestAvailability(req);
    check(why.empty() == kOiioBuild,
          "an EXR half request is available exactly in the OIIO build");
    if (!kOiioBuild)
      check(contains(why, "NP_USE_OIIO"),
            "and in the OFF build the reason names the build option that would provide it");
    if (kOiioBuild) {
      req.bitDepth = ExportBitDepth::UInt8;
      why = exportRequestAvailability(req);
      check(!why.empty() && contains(why, "EXR") && contains(why, "8-bit integer"),
            "in the ON build an 8-bit EXR is still refused -- the depth probe's answer, which "
            "is the case OpenImageIO would have silently substituted half for");
    }

    // The resize refusal reaches validation intact.
    ExportRequest big;
    big.resize.mode = ExportResizeMode::Percent;
    big.resize.percent = 200.0f;
    const ExportValidation up = validateExportRequest(big, 100, 100, nullptr, nullptr);
    check(!up.ok && contains(up.error, "enlarge"),
          "an upscaling request fails validation with ops/Resample's own wording");

    // Primaries, which only a real working space can trip.
    WorkingSpace odd;
    odd.primaries.redX = 0.7347f;  // ACES AP0-ish red, far outside the 1e-6 epsilon
    ExportRequest plain;
    const ExportValidation prim = validateExportRequest(plain, 4, 4, &odd, nullptr);
    check(!prim.ok && contains(prim.error, "primaries"),
          "a primaries mismatch is refused when a working space is supplied");
    check(validateExportRequest(plain, 4, 4, nullptr, nullptr).ok,
          "...and skipped, not silently passed, when no working space is supplied -- the same "
          "request with no document is a legal preset");

    // Translucency, which only real pixels can trip.
    DecodedImage soft = makeImage(2, 1);
    setPixel(soft, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    setPixel(soft, 1, 0, 1.0f, 1.0f, 1.0f, 0.5f);
    ExportRequest jpg;
    jpg.format = ImageFormat::Jpeg;
    const ExportValidation trans = validateExportRequest(jpg, 2, 1, nullptr, &soft);
    check(!trans.ok && contains(trans.error, "no alpha channel") && contains(trans.error, "x=1"),
          "a translucent document into JPEG is refused, naming the first offending pixel");
    check(validateExportRequest(jpg, 2, 1, nullptr, nullptr).ok,
          "...and the same request with no pixels to look at is a legal preset");
  }

  // --- PRD I11's warnings: legal, lossy, and named with a number ----------
  {
    ExportRequest req;
    req.resize.mode = ExportResizeMode::Percent;
    req.resize.percent = 50.0f;
    ExportValidation v = validateExportRequest(req, 1000, 1000, nullptr, nullptr);
    bool sawResize = false, sawDepth = false;
    for (const std::string& w : v.warnings) {
      if (contains(w, "1000x1000 -> 500x500") && contains(w, "75.0%")) sawResize = true;
      if (contains(w, "256 levels")) sawDepth = true;
    }
    check(v.ok && v.outWidth == 500 && v.outHeight == 500,
          "a 50% downscale of 1000x1000 validates, at 500x500");
    check(sawResize,
          "I11: the downscale warning names the exact sizes and that it discards 75.0% of the "
          "pixels -- a number, not 'some quality loss'");
    check(sawDepth, "I11: the 8-bit warning names 256 levels against the half-float working space");

    req.resize.mode = ExportResizeMode::None;
    req.targetSpace = ExportTargetSpace::Rec709Linear;
    v = validateExportRequest(req, 8, 8, nullptr, nullptr);
    bool sawBanding = false;
    for (const std::string& w : v.warnings)
      if (contains(w, "coarser near black") && contains(w, "12.9x")) sawBanding = true;
    check(sawBanding,
          "I11: 8-bit *linear* is warned about with its measured 12.9x shadow-step penalty, "
          "derived from color/Space's own curve at run time");

    DecodedImage hot = makeImage(2, 1);
    setPixel(hot, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    setPixel(hot, 1, 0, 2.5f, 1.0f, 1.0f, 1.0f);
    ExportRequest srgb8;
    v = validateExportRequest(srgb8, 2, 1, nullptr, &hot);
    bool sawClip = false;
    for (const std::string& w : v.warnings)
      if (contains(w, "2.5000") && contains(w, "clips to white") && contains(w, "x=1"))
        sawClip = true;
    check(v.ok && sawClip,
          "I11: an integer depth warns that the document's 2.5 highlight clips, and says where");

    ExportRequest jpg;
    jpg.format = ImageFormat::Jpeg;
    v = validateExportRequest(jpg, 8, 8, nullptr, nullptr);
    bool sawLossy = false, sawNoAlpha = false;
    for (const std::string& w : v.warnings) {
      if (contains(w, "JPEG is lossy")) sawLossy = true;
      if (contains(w, "no alpha channel")) sawNoAlpha = true;
    }
    check(v.ok && sawLossy && sawNoAlpha,
          "I11: JPEG warns that it is lossy at quality 95 and that it carries no alpha");

    ExportRequest clean;
    clean.bitDepth = ExportBitDepth::UInt16;
    v = validateExportRequest(clean, 8, 8, nullptr, nullptr);
    check(v.ok && v.warnings.empty(),
          "control: a 16-bit sRGB PNG at document size warns about nothing at all -- warnings "
          "are earned, not decorative");
  }

  // --- Presets: save, serialize, load, apply ------------------------------
  {
    ExportPresetStore store;
    std::string err;

    ExportPreset web;
    web.name = "  Web preview  ";  // deliberately padded; storage trims
    web.request.format = ImageFormat::Png;
    web.request.targetSpace = ExportTargetSpace::Rec709Srgb;
    web.request.bitDepth = ExportBitDepth::UInt8;
    web.request.resize.mode = ExportResizeMode::FitWithin;
    web.request.resize.maxWidth = 2048;
    web.request.resize.maxHeight = 1024;
    web.request.resize.percent = 42.5f;  // the *other* mode's number, non-default

    ExportPreset comp;
    comp.name = "Comp EXR";
    comp.request.format = ImageFormat::Exr;
    comp.request.targetSpace = ExportTargetSpace::Rec709Linear;
    comp.request.bitDepth = ExportBitDepth::Half;
    comp.request.resize.mode = ExportResizeMode::Percent;
    comp.request.resize.percent = 50.0f;

    ExportPreset print;
    print.name = "Print TIFF";
    print.request.format = ImageFormat::Tiff;
    print.request.targetSpace = ExportTargetSpace::Rec709Bt709;
    print.request.bitDepth = ExportBitDepth::UInt16;

    check(store.savePreset(web, &err) && store.savePreset(comp, &err) &&
              store.savePreset(print, &err),
          "three presets save, including two this build may have no writer for");
    check(store.presets().size() == 3 && store.presets()[0].name == "Web preview",
          "the stored name is trimmed of surrounding whitespace");

    const std::string text = store.serialize();
    ExportPresetStore reloaded;
    check(reloaded.loadFromString(text, "round trip") && reloaded.error().empty(),
          "the serialized preset file parses back");
    check(reloaded.problems().empty(),
          "and no preset is skipped in EITHER build -- an unwritable format is still a "
          "perfectly readable preset");
    bool allFieldsEqual = reloaded.presets().size() == store.presets().size();
    for (size_t i = 0; allFieldsEqual && i < store.presets().size(); ++i) {
      const ExportPreset& a = store.presets()[i];
      const ExportPreset& b = reloaded.presets()[i];
      allFieldsEqual = a.name == b.name && a.request.format == b.request.format &&
                       a.request.targetSpace == b.request.targetSpace &&
                       a.request.bitDepth == b.request.bitDepth &&
                       a.request.resize.mode == b.request.resize.mode &&
                       a.request.resize.percent == b.request.resize.percent &&
                       a.request.resize.maxWidth == b.request.resize.maxWidth &&
                       a.request.resize.maxHeight == b.request.resize.maxHeight;
    }
    check(allFieldsEqual,
          "every field of every preset survives save -> serialize -> load, including the "
          "numbers the active resize mode does not read");

    // The cross-build case, which is the reason any of this is interesting.
    const ExportPreset* loadedComp = reloaded.find("comp exr");
    check(loadedComp != nullptr, "preset lookup is case-insensitive");
    if (loadedComp) {
      check(loadedComp->request.format == ImageFormat::Exr &&
                loadedComp->request.bitDepth == ExportBitDepth::Half,
            "the EXR half preset still names EXR half after the round trip -- NEVER silently "
            "replaced by a format this build happens to be able to write");
      const std::string why = exportRequestAvailability(loadedComp->request);
      check(why.empty() == kOiioBuild,
            "and it is reported available exactly in the OIIO build");
      const ExportValidation v =
          validateExportRequest(loadedComp->request, 512, 512, nullptr, nullptr);
      check(v.ok == kOiioBuild && (v.ok || v.error == why),
            "applying it validates in the ON build and refuses in the OFF build with that same "
            "named reason");
      check(!v.ok || (v.outWidth == 256 && v.outHeight == 256),
            "and where it does apply, its 50% resize resolves to 256x256");
    }

    // Replace by name, case-insensitively; delete; and the name rules.
    ExportPreset again;
    again.name = "WEB PREVIEW";
    again.request.bitDepth = ExportBitDepth::UInt16;
    check(store.savePreset(again, &err) && store.presets().size() == 3,
          "saving under an existing name (different case) replaces rather than duplicating");
    check(store.find("Web preview") != nullptr &&
              store.find("Web preview")->request.bitDepth == ExportBitDepth::UInt16 &&
              store.find("Web preview")->name == "WEB PREVIEW",
          "...the replacement's settings and capitalisation both win");
    check(store.removePreset("print tiff") && store.presets().size() == 2 &&
              !store.removePreset("print tiff"),
          "delete removes by name case-insensitively, and reports a second attempt as a miss");

    ExportPreset bad;
    bad.name = "   ";
    check(!store.savePreset(bad, &err) && contains(err, "empty or whitespace"),
          "a whitespace-only preset name is refused by name");
    bad.name = std::string(ExportPresetStore::kMaxPresetNameLength + 1, 'x');
    check(!store.savePreset(bad, &err) && contains(err, "the limit is 64"),
          "an over-long preset name is refused, naming the limit");
    bad.name = std::string("bell\x07here");
    check(!store.savePreset(bad, &err) && contains(err, "control character"),
          "a preset name with a control character is refused, naming the byte");
    check(store.presets().size() == 2, "and none of the three refusals modified the store");
  }

  // --- Preset file: the awkward inputs ------------------------------------
  {
    ExportPresetStore store;
    check(!store.loadFromString("this is not json", "fixture") && !store.error().empty(),
          "a structurally broken preset file fails to load, with a message that says where");
    check(!store.loadFromString("{ \"version\": 1 }", "fixture") &&
              contains(store.error(), "presets"),
          "a JSON file with no presets array is refused as 'not an export preset file'");
    check(store.loadFromString("{ \"version\": 1, \"presets\": [] }", "fixture") &&
              store.presets().empty(),
          "an empty preset list is a valid file, not an error");

    // A token from some future build: that one preset is skipped and
    // described; the rest load. See io/ExportAs.hpp on the cost.
    const char* mixed =
        "{ \"version\": 1, \"presets\": ["
        " {\"name\":\"future\",\"format\":\"jpegxl\",\"space\":\"rec709-srgb\","
        "  \"depth\":\"uint8\",\"resize\":\"none\",\"percent\":100,\"maxWidth\":1,"
        "  \"maxHeight\":1},"
        " {\"name\":\"present\",\"format\":\"png\",\"space\":\"rec709-srgb\","
        "  \"depth\":\"uint16\",\"resize\":\"none\",\"percent\":100,\"maxWidth\":1,"
        "  \"maxHeight\":1} ] }";
    check(store.loadFromString(mixed, "fixture") && store.presets().size() == 1 &&
              store.presets()[0].name == "present",
          "a preset naming an unrecognised format is skipped while the rest still load");
    check(store.problems().size() == 1 && contains(store.problems()[0], "jpegxl") &&
              contains(store.problems()[0], "future"),
          "...and the skip is reported, naming the preset and the token");

    // An unknown *field* is forward-compatible and must not break anything.
    const char* extraField =
        "{ \"version\": 2, \"presets\": ["
        " {\"name\":\"ok\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1,"
        "  \"futureField\":{\"nested\":[1,2,true,null]}} ], \"futureTop\": \"ignored\" }";
    check(store.loadFromString(extraField, "fixture") && store.presets().size() == 1 &&
              store.problems().empty(),
          "unrecognised fields (and a future version number) are skipped, not fatal");

    const char* dupes =
        "{ \"presets\": ["
        " {\"name\":\"A\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1},"
        " {\"name\":\"a\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1} ] }";
    check(!store.loadFromString(dupes, "fixture") &&
              contains(store.error(), "two presets are both named") && store.presets().empty(),
          "two presets whose names differ only in case are a load failure, not a menu with two "
          "identical rows");
  }

  // --- Preset file: a real file, and where it lives -----------------------
  {
    const char* path = "selftest_exportas_presets.json";
    std::remove(path);

    ExportPresetStore empty;
    check(empty.loadFromFile("selftest_exportas_does_not_exist.json") &&
              empty.presets().empty() && empty.error().empty(),
          "a preset file that does not exist is an empty store, not a failure -- every first "
          "run would otherwise look broken");

    ExportPresetStore store;
    ExportPreset p;
    p.name = "Round trip";
    p.request.format = ImageFormat::Bmp;
    p.request.targetSpace = ExportTargetSpace::Rec709Bt709;
    p.request.resize.mode = ExportResizeMode::FitWithin;
    p.request.resize.maxWidth = 777;
    p.request.resize.maxHeight = 555;
    std::string err;
    check(store.savePreset(p, &err) && store.saveToFile(path, &err),
          "a preset store writes to a real file");
    ExportPresetStore fromDisk;
    check(fromDisk.loadFromFile(path) && fromDisk.presets().size() == 1 &&
              fromDisk.presets()[0].request.resize.maxWidth == 777 &&
              fromDisk.presets()[0].request.resize.maxHeight == 555 &&
              fromDisk.presets()[0].request.format == ImageFormat::Bmp,
          "...and reads back identically from disk");
    std::remove(path);

    // The location override, which is what lets this run without touching
    // the developer's own settings.
    const char* previous = std::getenv("NP_EXPORT_PRESETS");
    const std::string saved = previous ? previous : "";
    setenv("NP_EXPORT_PRESETS", "/tmp/np-selftest-presets.json", 1);
    check(defaultExportPresetsPath() == "/tmp/np-selftest-presets.json",
          "$NP_EXPORT_PRESETS overrides the preset file location");
    unsetenv("NP_EXPORT_PRESETS");
    const std::string fallback = defaultExportPresetsPath();
    check(contains(fallback, "naturalPaint") && contains(fallback, "export-presets.json"),
          "and the default location is a per-user application-data path, never the source tree");
    if (previous) setenv("NP_EXPORT_PRESETS", saved.c_str(), 1);
  }

  // --- The whole operation, end to end ------------------------------------
  {
    // A 64x64 document, half black and half white in vertical stripes of 1
    // px, exported at 25% -- so the export path really does flatten, resize
    // and encode, and the answer is the known mean.
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    for (int32_t y = 0; y < 64; ++y)
      for (int32_t x = 0; x < 64; ++x) {
        const float v = (x & 1) ? 1.0f : 0.0f;
        writeStraight(doc, x, y, v, v, v, 1.0f);
      }

    ExportRequest req;
    req.format = ImageFormat::Png;
    req.targetSpace = ExportTargetSpace::Rec709Srgb;
    req.bitDepth = ExportBitDepth::UInt16;
    req.resize.mode = ExportResizeMode::Percent;
    req.resize.percent = 25.0f;

    const ExportResult out = exportDocumentWithRequest(doc, req);
    check(out.ok && out.error.empty(), "exportDocumentWithRequest: 64x64 at 25% encodes");
    if (out.ok) {
      const DecodedImage back = decodeImageLinear(out.bytes.data(), out.bytes.size());
      check(back.valid() && back.width == 16 && back.height == 16,
            "...and the file really is 16x16 -- the resize reached the encoder");
      if (back.valid()) {
        float maxDev = 0.0f;
        for (size_t i = 0; i < back.pixels.size(); i += 4)
          maxDev = std::max(maxDev, std::fabs(back.pixels[i] - 0.5f));
        std::printf("    [measured] 1-px stripes 64->16 through the full export path: max "
                    "deviation from the true linear mean 0.5 = %.3e\n",
                    static_cast<double>(maxDev));
        check(maxDev <= 1.0e-4f,
              "every texel of the exported file is the true linear mean 0.5, within the 16-bit "
              "quantization step");
        check(back.pixels[3] == 1.0f,
              "and the fully opaque document is still fully opaque after the resize");
      }
    }

    // A request the document itself makes illegal still refuses through the
    // composed path, with the same string.
    ExportRequest tooBig = req;
    tooBig.resize.percent = 400.0f;
    const ExportResult refused = exportDocumentWithRequest(doc, tooBig);
    check(!refused.ok && refused.bytes.empty() && contains(refused.error, "enlarge"),
          "an upscaling request refuses through the composed path and writes no bytes");

    const char* filePath = "selftest_exportas_out.png";
    std::string fileErr;
    check(exportDocumentWithRequestToFile(doc, filePath, req, &fileErr),
          "exportDocumentWithRequestToFile writes the resized file");
    std::FILE* f = std::fopen(filePath, "rb");
    check(f != nullptr, "...and the file is really there");
    if (f) std::fclose(f);
    std::remove(filePath);

    check(!exportDocumentWithRequestToFile(doc, "selftest_exportas_refused.png", tooBig,
                                           &fileErr) &&
              contains(fileErr, "enlarge"),
          "a refused request forwards the same error and never opens the file");
    std::FILE* shouldNotExist = std::fopen("selftest_exportas_refused.png", "rb");
    check(shouldNotExist == nullptr, "...leaving nothing behind on disk");
    if (shouldNotExist) {
      std::fclose(shouldNotExist);
      std::remove("selftest_exportas_refused.png");
    }
  }

  std::printf("[selftest] export as %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// ==========================================================================
// Phase 4 step 8 -- document lifecycle (revert, duplicate document, save a
// copy, save incremental, open recent).
// ==========================================================================
bool runDocumentLifecycleTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // PLAN.md §1.5 again: one constant, every assertion stating the correct
  // answer for the configuration it is in, nothing compiled out. Half of this
  // section is pure path and list arithmetic that behaves identically in both
  // builds; the other half is the file operations, which are `.npaint`
  // operations and therefore refuse by name without OpenImageIO.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  namespace fs = std::filesystem;
  std::error_code ec;

  // All scratch state lives in one directory rather than in `selftest_*`
  // files beside it, because saveDocumentIncremental() *lists its containing
  // directory* to find the highest existing version -- run in the working
  // directory it would scan whatever else happens to be there, and the
  // answers would depend on the developer's file names. Removed
  // unconditionally at the end of this function, including on the paths whose
  // assertions failed.
  const std::string dir = "selftest_lifecycle";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const auto inDir = [&](const char* name) { return dir + "/" + name; };
  const auto touch = [&](const char* name) {
    std::ofstream f(inDir(name), std::ios::binary);
    f << "not a real document; only its name matters to the version scan\n";
  };

  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto readStraightRed = [](const Document& doc, size_t layerIndex, int32_t x, int32_t y) {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    if (!t) return -1.0f;
    const std::array<float, 4> px = t->readPixel(tileLocalOffset(p));
    return px[3] > 0.0f ? px[0] / px[3] : 0.0f;
  };
  // Bit-exact comparison of two documents' tile storage, at zero tolerance --
  // the same claim io/NpaintFile's own round trip makes and for the same
  // reason: a duplicate is a memory copy and a `.npaint` round trip has no
  // rounding stage, so anything but bit equality is a bug.
  auto tilesIdentical = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const auto& ta = a.layers[i].rgbTiles;
      const auto& tb = b.layers[i].rgbTiles;
      if (ta.has_value() != tb.has_value()) return false;
      if (!ta) continue;
      if (ta->occupiedTileCount() != tb->occupiedTileCount()) return false;
      for (const auto& [coord, tile] : *ta) {
        const Tile* other = tb->find(coord);
        if (!other) return false;
        if (std::memcmp(tile.data(), other->data(), sizeof(Tile)) != 0) return false;
      }
    }
    return true;
  };

  // The fixture: a two-layer document plus a carry holding data this build has
  // no knowledge of -- three unrecognised document attributes, one
  // unrecognised layer attribute, and a whole foreign `np:kind="Pigment"`
  // part sitting *between* the two layers. Every lifecycle operation that
  // writes is checked against this, because a revert or a duplicate is
  // exactly where PRD I10's carry-through would get quietly dropped.
  auto makeFixtureCarry = []() {
    NpaintCarry carry;
    NpaintAttribute s;
    s.name = "np:futureNote";
    s.type = NpaintAttribute::Type::String;
    s.stringValue = "written by a newer build";
    NpaintAttribute i;
    i.name = "np:futureRevision";
    i.type = NpaintAttribute::Type::Int;
    i.intValue = 424242;
    NpaintAttribute fl;
    fl.name = "np:futureGamma";
    fl.type = NpaintAttribute::Type::Float;
    fl.floatValue = 2.4f;
    carry.documentAttributes = {s, i, fl};

    NpaintAttribute lm;
    lm.name = "np:futureMaskLink";
    lm.type = NpaintAttribute::Type::String;
    lm.stringValue = "M0007";
    carry.layerAttributes = {{lm}, {}};

    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    // Already sorted: OpenEXR keeps channels in a sorted ChannelList, so an
    // unsorted fixture would come back reordered for a reason unrelated to
    // preservation (runNpaintFormatTest's own note).
    pigment.channelNames = {"pig.c0", "pig.m"};
    pigment.sampleTypeName = "float";
    {
      const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                              0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
      pigment.rawPixels.resize(sizeof(vals));
      std::memcpy(pigment.rawPixels.data(), vals, sizeof(vals));
    }
    NpaintAttribute pk;
    pk.name = "np:kind";
    pk.type = NpaintAttribute::Type::String;
    pk.stringValue = "Pigment";
    pigment.attributes = {pk};
    carry.rawParts = {pigment};
    carry.partOrder = {{NpaintPartSlot::Kind::Layer, 0},
                       {NpaintPartSlot::Kind::RawPart, 0},
                       {NpaintPartSlot::Kind::Layer, 1}};
    carry.layerPartNames = {"L0001", "L0003"};
    carry.basis = "future-basis-v9";
    return carry;
  };
  auto makeFixtureDocument = [&]() {
    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 0.5f);
    return doc;
  };
  // The carry a file came back with still holds everything the fixture put in
  // -- the three document attributes at their exact values, the layer
  // attribute, and the foreign part's bytes.
  auto carryIntact = [](const NpaintCarry& c) {
    if (c.documentAttributes.size() != 3) return false;
    bool sawNote = false, sawRev = false, sawGamma = false;
    for (const NpaintAttribute& a : c.documentAttributes) {
      if (a.name == "np:futureNote" && a.stringValue == "written by a newer build") sawNote = true;
      if (a.name == "np:futureRevision" && a.intValue == 424242) sawRev = true;
      if (a.name == "np:futureGamma" && a.floatValue == 2.4f) sawGamma = true;
    }
    if (!(sawNote && sawRev && sawGamma)) return false;
    if (c.rawParts.size() != 1) return false;
    const NpaintRawPart& p = c.rawParts[0];
    if (p.name != "L0002" || p.sampleTypeName != "float") return false;
    if (p.channelNames != std::vector<std::string>{"pig.c0", "pig.m"}) return false;
    const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                            0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
    if (p.rawPixels.size() != sizeof(vals)) return false;
    if (std::memcmp(p.rawPixels.data(), vals, sizeof(vals)) != 0) return false;
    if (c.basis != "future-basis-v9") return false;
    if (c.partOrder.size() != 3 || c.partOrder[1].kind != NpaintPartSlot::Kind::RawPart)
      return false;
    return true;
  };

  // --- The record and its identity ---------------------------------------
  {
    const DocumentId a = allocateDocumentId();
    const DocumentId b = allocateDocumentId();
    check(a != 0 && b != 0 && b > a, "document ids are non-zero and monotonic");

    OpenDocument blank = makeBlankOpenDocument(64, 32, WorkingSpace{}, "Sketch");
    check(blank.id != 0 && blank.document.width == 64 && blank.document.height == 32 &&
              blank.document.layers.size() == 1,
          "makeBlankOpenDocument wraps Document::createBlank (PRD C7)");
    check(!blank.isDirty() && !blank.hasPath() && blank.unsavedWorkSummary().empty(),
          "a blank document starts clean and unbound -- nothing to lose, nothing to save over");
    check(documentDisplayName(blank) == "Sketch",
          "an unbound document shows its title");

    blank.recordEdit("place image as layer");
    check(blank.isDirty() && blank.revision == 1,
          "recordEdit marks the document dirty");
    check(contains(blank.unsavedWorkSummary(), "1 unsaved change") &&
              contains(blank.unsavedWorkSummary(), "place image as layer"),
          "...and the summary names the edit, not just its existence (PRD I11)");

    // The cap, and that the *count* stays exact past it -- a refusal that
    // said "32 unsaved changes" when there were 40 would be worse than one
    // that said nothing.
    OpenDocument many = makeBlankOpenDocument(8, 8, WorkingSpace{});
    for (int i = 0; i < 40; ++i) many.recordEdit("stroke " + std::to_string(i));
    check(many.unsavedEdits.size() == kMaxTrackedUnsavedEdits &&
              many.unsavedEditsDropped == 40 - kMaxTrackedUnsavedEdits,
          "the unsaved-edit label list is capped but the count is not");
    check(contains(many.unsavedWorkSummary(), "40 unsaved changes") &&
              contains(many.unsavedWorkSummary(), "and 8 more"),
          "...and the summary says exactly how many labels it is not showing");
  }

  // --- The session owns them ----------------------------------------------
  {
    DocumentSession session;
    check(session.empty() && session.active() == nullptr,
          "a fresh session holds no documents and active() is null, not a dangling reference");

    OpenDocument* a = session.add(makeBlankOpenDocument(16, 16, WorkingSpace{}, "A"));
    OpenDocument* b = session.add(makeBlankOpenDocument(16, 16, WorkingSpace{}, "B"));
    check(session.count() == 2 && session.active() == b,
          "adding a document makes it active");
    const DocumentId aId = a->id;
    session.setActive(0);
    check(session.active() == a && session.find(aId) == a,
          "documents are addressable by index and by id");
    // Pointer stability is the reason for the unique_ptr indirection: a
    // dialog holding `a` must not be looking at freed memory because another
    // document was opened.
    for (int i = 0; i < 32; ++i) session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}));
    check(session.find(aId) == a,
          "a pointer into the session survives 32 more documents being opened");

    session.setActive(0);
    std::string closeErr;
    a->recordEdit("stroke");
    check(!session.close(0, false, &closeErr) && contains(closeErr, "unsaved change") &&
              contains(closeErr, "stroke") && session.count() == 34,
          "closing a dirty document refuses and names what would be lost");
    check(session.close(0, true, &closeErr) && session.count() == 33,
          "...and closes when the discard is explicit");
    check(!session.close(999, true, &closeErr) && contains(closeErr, "no open document"),
          "closing an index that is not there refuses rather than crashing");
  }

  // --- Duplicate document --------------------------------------------------
  //
  // Runs in both builds: duplication touches no file.
  {
    OpenDocument source;
    source.id = allocateDocumentId();
    source.document = makeFixtureDocument();
    source.carry = makeFixtureCarry();
    source.path = inDir("original.npaint");
    source.savedRevision = source.revision;

    OpenDocument copy = duplicateDocument(source);

    check(copy.path.empty(),
          "a duplicate does NOT inherit the original's path -- the next Save cannot overwrite "
          "it");
    check(copy.id != source.id && copy.id != 0,
          "a duplicate is a different document, so it gets a different id");
    check(copy.isDirty() && contains(copy.unsavedWorkSummary(), "duplicate of original.npaint"),
          "a duplicate is unsaved from birth and says what it is a duplicate of");
    check(tilesIdentical(copy.document, source.document),
          "every tile is copied bit-identically at zero tolerance");
    check(carryIntact(copy.carry),
          "the PRD I10 carry -- unknown attributes AND the foreign Pigment part -- comes with "
          "the duplicate");
    check(copy.carry.layerPartNames == source.carry.layerPartNames,
          "...including the layer part ids, so np:parent links inside the foreign part still "
          "point where they did");

    // Deep, not shared.
    writeStraight(copy.document, 0, 3, 3, 0.9f, 0.9f, 0.9f, 1.0f);
    check(std::fabs(readStraightRed(source.document, 0, 3, 3) - 0.5f) < 1e-3f &&
              std::fabs(readStraightRed(copy.document, 0, 3, 3) - 0.9f) < 1e-3f,
          "painting on the duplicate does not reach the original (deep tile copy)");

    // The refusal that makes the unbound path safe rather than merely
    // unbound. This one is identical in both builds: it never reaches
    // io/NpaintFile at all.
    const DocumentOpResult noPath = saveDocument(copy);
    check(!noPath.ok && contains(noPath.error, "never been saved") &&
              contains(noPath.error, "Save As"),
          "Save on the duplicate refuses by name instead of writing to the original's path");
  }

  // --- Save incremental: the naming rule ----------------------------------
  //
  // Pure path arithmetic plus a directory listing, so every case below runs
  // and asserts the same answer in both builds.
  {
    std::string next, err;

    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v001.npaint"),
          "an unversioned name gets _v001 (three digits, zero-padded)");

    touch("paint_v001.npaint");
    touch("paint_v003.npaint");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "the gap at _v002 is stepped over, not filled -- a lower number written later "
          "sorts wrong forever");
    check(nextIncrementalPath(inDir("paint_v001.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "incrementing an OLD version still lands above the highest existing one");

    touch("paint_v009.png");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "a sibling with a different extension is a different series and is ignored");

    touch("other_v050.npaint");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "a sibling with a different base name is a different series and is ignored");

    touch("shot_v7.npaint");
    check(nextIncrementalPath(inDir("shot_v7.npaint"), &next, &err) &&
              next == inDir("shot_v8.npaint"),
          "an already-versioned name is incremented, keeping its own zero padding");

    touch("wide_v999.npaint");
    check(nextIncrementalPath(inDir("wide_v999.npaint"), &next, &err) &&
              next == inDir("wide_v1000.npaint"),
          "padding grows rather than truncating: _v999 -> _v1000");

    touch("render001.npaint");
    check(nextIncrementalPath(inDir("render001.npaint"), &next, &err) &&
              next == inDir("render001_v001.npaint"),
          "a trailing number with no _v marker is part of the name, not a version");

    check(!nextIncrementalPath("", &next, &err) && contains(err, "no path"),
          "an empty path refuses by name");
    check(!nextIncrementalPath(dir + "/no-such-directory/a.npaint", &next, &err) &&
              contains(err, "could not list"),
          "a directory that cannot be listed refuses by name rather than guessing _v001");

    // A relative path stays relative: nothing silently absolutises a name the
    // user typed.
    check(nextIncrementalPath("bare.npaint", &next, &err) && next == "bare_v001.npaint",
          "a bare file name with no directory produces a bare file name");
  }

  // --- Open recent: the MRU -----------------------------------------------
  //
  // Also entirely build-independent.
  {
    RecentDocuments recent;
    std::string err;
    check(recent.add("/tmp/np/a.npaint", &err) && recent.add("/tmp/np/b.npaint", &err) &&
              recent.add("/tmp/np/c.npaint", &err) && recent.entries().size() == 3,
          "the recent list records what it is given");
    check(recent.entries()[0].path == "/tmp/np/c.npaint" &&
              recent.entries()[2].path == "/tmp/np/a.npaint",
          "...most recent first");
    check(recent.entries()[0].displayName == "c.npaint",
          "...with the file name alone for a menu");

    recent.add("/tmp/np/a.npaint", &err);
    check(recent.entries().size() == 3 && recent.entries()[0].path == "/tmp/np/a.npaint",
          "re-adding an entry moves it to the front rather than duplicating it");

    recent.add("/tmp/np/./x/../b.npaint", &err);
    check(recent.entries().size() == 3 && recent.entries()[0].path == "/tmp/np/b.npaint",
          "dedup is on the normalised path, so ./x/../b.npaint is the same entry as b.npaint");

    for (int i = 0; i < 15; ++i) recent.add("/tmp/np/f" + std::to_string(i) + ".npaint", &err);
    check(recent.entries().size() == RecentDocuments::kCapacity &&
              recent.entries()[0].path == "/tmp/np/f14.npaint" &&
              recent.entries()[9].path == "/tmp/np/f5.npaint",
          "capacity is 10, oldest first out");

    check(!recent.add("", &err) && contains(err, "empty path"),
          "an empty path is refused by name");
    check(!recent.add("/tmp/np/bad\nname.npaint", &err) && contains(err, "control character") &&
              contains(err, "one path per line"),
          "a path with a newline is refused by name -- the file format is one path per line");

    // Round trip through the file format.
    const std::string text = recent.serialize();
    RecentDocuments reread;
    reread.loadFromString(text, "test");
    check(reread.entries().size() == recent.entries().size() &&
              reread.entries()[0].path == recent.entries()[0].path &&
              reread.entries().back().path == recent.entries().back().path,
          "serialize -> load preserves the list and its order exactly");

    RecentDocuments dup;
    dup.loadFromString("# header\n\n/tmp/np/a.npaint\n/tmp/np/b.npaint\n/tmp/np/a.npaint\n",
                       "test");
    check(dup.entries().size() == 2 && dup.entries()[0].path == "/tmp/np/a.npaint",
          "a file listing the same path twice loads as one entry, at its most recent position");

    RecentDocuments broken;
    broken.loadFromString("/tmp/np/ok.npaint\n\x01" "bad\n", "recent.txt");
    check(broken.entries().size() == 1 && broken.problems().size() == 1 &&
              contains(broken.problems()[0], "recent.txt:2"),
          "an unusable line is skipped, reported with its line number, and does not fail the "
          "load -- a corrupt recent list can never stop the application starting");

    const std::string recentPath = inDir("recent-documents.txt");
    check(recent.saveToFile(recentPath, &err), "the recent list writes to a real file");
    RecentDocuments fromDisk;
    check(fromDisk.loadFromFile(recentPath) &&
              fromDisk.entries().size() == RecentDocuments::kCapacity &&
              fromDisk.entries()[0].path == recent.entries()[0].path,
          "...and reads back identically");
    RecentDocuments absent;
    check(absent.loadFromFile(inDir("no-such-recent.txt")) && absent.entries().empty() &&
              absent.error().empty(),
          "a recent file that does not exist is an empty list, not an error");

    // The location override, so this test never touches the developer's own
    // list -- io/ExportAs' $NP_EXPORT_PRESETS precedent, same directory.
    const char* previous = std::getenv("NP_RECENT_DOCUMENTS");
    const std::string saved = previous ? previous : "";
    setenv("NP_RECENT_DOCUMENTS", "/tmp/np-selftest-recent.txt", 1);
    check(defaultRecentDocumentsPath() == "/tmp/np-selftest-recent.txt",
          "$NP_RECENT_DOCUMENTS overrides the recent-documents file location");
    unsetenv("NP_RECENT_DOCUMENTS");
    const std::string fallback = defaultRecentDocumentsPath();
    check(contains(fallback, "naturalPaint") && contains(fallback, "recent-documents.txt"),
          "...and the default is the same per-user application-data directory as the presets");
    if (previous) setenv("NP_RECENT_DOCUMENTS", saved.c_str(), 1);

    // The missing-entry rule, which is the one PRD-shaped requirement in the
    // MRU: never silently drop, always say why.
    RecentDocuments missing;
    missing.add(inDir("gone.npaint"), &err);
    std::string why;
    check(recentDocumentMissing(inDir("gone.npaint"), &why) && contains(why, "no longer there"),
          "a recent entry whose file is gone is reported missing, with a reason");
    OpenDocument opened;
    const DocumentOpResult openMissing = openRecentDocument(missing, 0, &opened);
    check(!openMissing.ok && contains(openMissing.error, "no longer there") &&
              contains(openMissing.error, "kept in the list"),
          "opening it refuses by name and says the entry was kept, not dropped");
    check(missing.entries().size() == 1,
          "...and the entry really is still there afterwards");
    check(missing.remove(inDir("gone.npaint")) && missing.entries().empty(),
          "removing it is something the user does deliberately");
    check(!recentDocumentMissing(recentPath, &why) && why.empty(),
          "a recent entry whose file exists is not reported missing");
    check(!openRecentDocument(missing, 3, &opened).ok,
          "an out-of-range recent index refuses rather than reading memory it does not own");
  }

  // --- The file-backed operations -----------------------------------------
  //
  // From here on the answers differ by build, because `.npaint` is an OIIO
  // feature. Every refusal below is io/NpaintFile's own -- this module adds
  // no second vocabulary -- so the OFF build's assertions check that the
  // refusal arrives intact through each lifecycle entry point rather than
  // that a new message was written.
  {
    const std::string basePath = inDir("doc.npaint");
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.carry = makeFixtureCarry();

    RecentDocuments recent;
    const DocumentOpResult saved = saveDocumentAs(doc, basePath, {}, &recent);
    check(saved.ok == kOiioBuild, "Save As writes a .npaint exactly in the build that has one");
    if (!kOiioBuild) {
      check(contains(saved.error, ".npaint") && contains(saved.error, "NP_USE_OIIO"),
            "...and without OpenImageIO it refuses with io/NpaintFile's own named refusal");
      check(doc.path.empty() && recent.entries().empty(),
            "a refused Save As rebinds nothing and records nothing in Open Recent");
      const DocumentOpResult revertNoFile = revertDocument(doc, {true});
      check(!revertNoFile.ok && contains(revertNoFile.error, "never been saved"),
            "revert on a document that was never saved refuses by name in this build too");
      const DocumentOpResult incNoPath = saveDocumentIncremental(doc);
      check(!incNoPath.ok && contains(incNoPath.error, "no version number to increment"),
            "save incremental refuses a document with no path, naming Save As");
      const DocumentOpResult copyRefused = saveDocumentCopy(doc, inDir("copy.npaint"));
      check(!copyRefused.ok && contains(copyRefused.error, "NP_USE_OIIO"),
            "save a copy refuses through the same named refusal");
    }

    if (kOiioBuild) {
      check(doc.path == basePath && !doc.isDirty(),
            "Save As rebinds the document to the new path and clears the dirty state");
      check(recent.entries().size() == 1 &&
                recent.entries()[0].path == normalizeDocumentPath(basePath),
            "...and records it in Open Recent");

      // ---- Save a copy: the whole point is the path NOT changing ----------
      const std::string copyPath = inDir("doc_copy.npaint");
      const uint64_t revisionBefore = doc.revision;
      doc.recordEdit("stroke before the copy");
      const DocumentOpResult copied = saveDocumentCopy(doc, copyPath);
      check(copied.ok, "save a copy writes the file");
      check(doc.path == basePath,
            "SAVE A COPY LEAVES THE DOCUMENT'S PATH UNCHANGED -- the whole distinction from "
            "Save As");
      check(doc.isDirty() && doc.revision == revisionBefore + 1,
            "...and leaves the document dirty, because the copy is not this document's file");
      const NpaintLoadResult copyBack = loadNpaint(copyPath);
      check(copyBack.ok && tilesIdentical(copyBack.document, doc.document),
            "the copy holds the same pixels, bit for bit");
      check(copyBack.ok && carryIntact(copyBack.carry),
            "...and the same PRD I10 carry, so a copy is not where a foreign part is dropped");
      check(recent.entries().size() == 1,
            "save a copy adds nothing to Open Recent -- a copy was never an open document");
      const DocumentOpResult copyOntoSelf = saveDocumentCopy(doc, basePath);
      check(!copyOntoSelf.ok && contains(copyOntoSelf.error, "already bound"),
            "saving a copy onto the document's own file refuses -- that is a Save, not a copy");

      // ---- Revert ---------------------------------------------------------
      // The saved file still holds the pre-edit pixels; change them in memory
      // and prove revert brings the file's version back.
      writeStraight(doc.document, 0, 3, 3, 0.95f, 0.95f, 0.95f, 1.0f);
      doc.recordEdit("paint stroke on layer 1");
      const DocumentOpResult revertRefused = revertDocument(doc);
      check(!revertRefused.ok && contains(revertRefused.error, "paint stroke on layer 1") &&
                contains(revertRefused.error, "2 unsaved changes"),
            "revert refuses a dirty document and NAMES the edits it would discard (PRD I11)");
      check(std::fabs(readStraightRed(doc.document, 0, 3, 3) - 0.95f) < 1e-2f,
            "...and a refused revert changes nothing");

      const DocumentOpResult reverted = revertDocument(doc, {true});
      check(reverted.ok, "revert with the discard confirmed reloads the file");
      check(std::fabs(readStraightRed(doc.document, 0, 3, 3) - 0.5f) < 1e-3f,
            "...and the pixels are the last saved ones again");
      check(!doc.isDirty() && doc.unsavedEdits.empty() && doc.revision > 0,
            "...leaving the document clean, but with a bumped revision so derived caches "
            "re-read");
      check(carryIntact(doc.carry),
            "the carry after a revert is the FILE's carry, foreign part included -- a revert "
            "is not where PRD I10 data goes missing");

      // A revert whose file has gone must not also destroy the copy in
      // memory. This is the case that turns a mistake into data loss if the
      // load is not done into a temporary first.
      const std::string doomedPath = inDir("doomed.npaint");
      OpenDocument doomed;
      doomed.id = allocateDocumentId();
      doomed.document = makeFixtureDocument();
      check(saveDocumentAs(doomed, doomedPath).ok, "a second document saves for the next case");
      writeStraight(doomed.document, 0, 3, 3, 0.42f, 0.42f, 0.42f, 1.0f);
      doomed.recordEdit("stroke");
      fs::remove(doomedPath, ec);
      const DocumentOpResult lost = revertDocument(doomed, {true});
      check(!lost.ok && !lost.error.empty(),
            "revert forwards the loader's own error when the file has gone");
      check(std::fabs(readStraightRed(doomed.document, 0, 3, 3) - 0.42f) < 1e-2f &&
                doomed.isDirty(),
            "...and the in-memory document is left EXACTLY as it was, not half-replaced");

      // ---- Duplicate, then save: the original must survive ----------------
      OpenDocument dup = duplicateDocument(doc);
      writeStraight(dup.document, 0, 3, 3, 0.125f, 0.125f, 0.125f, 1.0f);
      const std::string dupPath = inDir("doc_dup.npaint");
      check(saveDocumentAs(dup, dupPath).ok, "the duplicate saves to its own chosen path");
      const NpaintLoadResult originalAfter = loadNpaint(basePath);
      check(originalAfter.ok &&
                std::fabs(readStraightRed(originalAfter.document, 0, 3, 3) - 0.5f) < 1e-3f,
            "...and the ORIGINAL file on disk is untouched by it");
      check(originalAfter.ok && carryIntact(originalAfter.carry),
            "...still carrying the foreign part it was saved with");

      // ---- Save incremental, for real -------------------------------------
      const DocumentOpResult inc1 = saveDocumentIncremental(doc, {}, &recent);
      check(inc1.ok && inc1.path == inDir("doc_v001.npaint") && doc.path == inc1.path,
            "save incremental writes _v001 and rebinds the document to it");
      check(recent.entries()[0].path == normalizeDocumentPath(inc1.path),
            "...and it becomes the most recent document");
      const DocumentOpResult inc2 = saveDocumentIncremental(doc, {}, &recent);
      check(inc2.ok && inc2.path == inDir("doc_v002.npaint"),
            "a second incremental save writes _v002");
      const NpaintLoadResult v1 = loadNpaint(inDir("doc_v001.npaint"));
      check(v1.ok && carryIntact(v1.carry),
            "_v001 still exists, unoverwritten, carry intact -- an incremental save never "
            "replaces an earlier version");
      check(!doc.isDirty(), "an incremental save leaves the document clean");

      // ---- Open, and the round trip through openNpaintDocument ------------
      OpenDocument reopened;
      RecentDocuments recent2;
      const DocumentOpResult openedDoc = openNpaintDocument(basePath, &reopened, &recent2);
      check(openedDoc.ok && reopened.path == basePath && !reopened.isDirty(),
            "openNpaintDocument binds the path and starts clean");
      check(carryIntact(reopened.carry),
            "...with the file's carry in the record, which is what makes every save above "
            "preserve it");
      check(recent2.entries().size() == 1 &&
                recent2.entries()[0].path == normalizeDocumentPath(basePath),
            "...and opening a document is what Open Recent is a list of");

      // ---- Open Recent end to end -----------------------------------------
      OpenDocument viaRecent;
      const DocumentOpResult recentOpen = openRecentDocument(recent2, 0, &viaRecent);
      check(recentOpen.ok && viaRecent.path == recent2.entries()[0].path &&
                tilesIdentical(viaRecent.document, reopened.document),
            "opening the first Open Recent entry gives back the same document");

      // ---- The tile cache must not serve the previous contents ------------
      //
      // io/TileResidency stamps size+mtime at *open*, so a residency opened
      // after an overwrite passes its own staleness check while OpenImageIO's
      // cache still holds the old tiles. Measured on this build before the
      // invalidation was added: the read below returned the PREVIOUS pixel
      // value and reported success. Every write path here calls
      // tileCacheInvalidate() for exactly that reason.
      const std::string cachePath = inDir("cached.npaint");
      OpenDocument seed;
      seed.id = allocateDocumentId();
      seed.document = makeFixtureDocument();
      check(saveDocumentAs(seed, cachePath).ok, "a document for the residency case saves");
      // Re-opened rather than reused: `npaintLayerTileSource()` is derived
      // from `NpaintCarry::partOrder`, which only a *load* fills in, so a
      // record that has only ever been saved cannot name its own subimages.
      // That is a real limitation of this step and is recorded as such in
      // DocumentLifecycle.hpp rather than papered over here.
      OpenDocument cachedDoc;
      check(openNpaintDocument(cachePath, &cachedDoc).ok,
            "...and re-opening it gives the carry a residency source can be derived from");

      const auto readThroughCache = [&](float* out) {
        *out = -1.0f;
        const std::optional<TileSourceRef> src =
            npaintLayerTileSource(cachePath, cachedDoc.carry, 0);
        if (!src) return false;
        LayerResidency res;
        std::string resErr;
        if (!openCachedLayerResidency(*src, kTileCacheBudgetBytes, &res, &resErr)) return false;
        const TileFetch f = res.readTile(tileCoordAt(PixelCoord{3, 3}));
        if (!f.tile) return false;
        const std::array<float, 4> px = f.tile->readPixel(tileLocalOffset(PixelCoord{3, 3}));
        *out = px[3] > 0.0f ? px[0] / px[3] : 0.0f;
        return true;
      };

      float firstRead = -1.0f;
      check(readThroughCache(&firstRead) && std::fabs(firstRead - 0.5f) < 1e-3f,
            "the cached residency serves the saved pixel");

      writeStraight(cachedDoc.document, 0, 3, 3, 0.25f, 0.25f, 0.25f, 1.0f);
      cachedDoc.recordEdit("stroke");
      check(saveDocument(cachedDoc).ok && !cachedDoc.isDirty(),
            "Save writes over the document's own bound path and clears the dirty state");
      float secondRead = -1.0f;
      readThroughCache(&secondRead);
      std::printf("    [measured] cached read of an overwritten .npaint: %.4f before the "
                  "rewrite, %.4f after (the file now holds 0.2500)\n",
                  static_cast<double>(firstRead), static_cast<double>(secondRead));
      check(std::fabs(secondRead - 0.25f) < 1e-3f,
            "a cached read after a lifecycle write returns the NEW pixels, not the cache's "
            "previous ones");
    }
  }

  // --- Clean up ------------------------------------------------------------
  fs::remove_all(dir, ec);
  check(!fs::exists(dir, ec), "every scratch file this section wrote is removed");

  std::printf("[selftest] document lifecycle %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

// ==========================================================================
// Phase 4 step 9 -- the recovery journal (ADR-0008; PRD O5-O10).
// ==========================================================================
bool runRecoveryJournalTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // PLAN.md §1.5. The journal itself cannot run without a writer, but the
  // scratch directory's naming and dating, the liveness rule, the whole
  // timer, the sidecar format and the truncation refusal are all
  // file-format-free -- so they are asserted in both configurations, and the
  // parts that differ state the correct answer for each rather than being
  // compiled out.
#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  namespace fs = std::filesystem;
  std::error_code ec;

  // Everything this section writes lives here, and $NP_JOURNAL_DIR points the
  // module at it for the whole run -- so nothing below can touch
  // ~/Library/Application Support/naturalPaint, which the OIIO-build run is
  // separately verified not to create. Same override mechanism, and the same
  // reason, as $NP_RECENT_DOCUMENTS in the lifecycle section above.
  const std::string root = "selftest_journal";
  fs::remove_all(root, ec);
  const char* previousRoot = std::getenv("NP_JOURNAL_DIR");
  const std::string savedRoot = previousRoot ? previousRoot : "";
  setenv("NP_JOURNAL_DIR", root.c_str(), 1);

  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto readStraightRed = [](const Document& doc, size_t layerIndex, int32_t x, int32_t y) {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    if (!t) return -1.0f;
    const std::array<float, 4> px = t->readPixel(tileLocalOffset(p));
    return px[3] > 0.0f ? px[0] / px[3] : 0.0f;
  };
  // Zero tolerance, as everywhere else a `.npaint` round trip is checked:
  // there is no rounding stage anywhere in the chain (io/NpaintFile.hpp's
  // HALF section), so anything short of bit equality is a bug rather than
  // drift.
  auto tilesIdentical = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const auto& ta = a.layers[i].rgbTiles;
      const auto& tb = b.layers[i].rgbTiles;
      if (ta.has_value() != tb.has_value()) return false;
      if (!ta) continue;
      if (ta->occupiedTileCount() != tb->occupiedTileCount()) return false;
      for (const auto& [coord, tile] : *ta) {
        const Tile* other = tb->find(coord);
        if (!other) return false;
        if (std::memcmp(tile.data(), other->data(), sizeof(Tile)) != 0) return false;
      }
    }
    return true;
  };
  // Every `np:*` layer attribute io/NpaintFile writes, compared field by
  // field -- the journal claims to recover the model, and the model is these
  // as much as it is the pixels.
  auto layerMetadataIdentical = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const Layer& x = a.layers[i];
      const Layer& y = b.layers[i];
      if (x.kind != y.kind || x.name != y.name || x.blend != y.blend) return false;
      if (x.opacity != y.opacity || x.visible != y.visible || x.locked != y.locked) return false;
      if (x.parent != y.parent) return false;
    }
    return true;
  };

  // The same fixture shape the lifecycle section uses: two layers, every
  // metadata field set to a non-default value, plus a carry holding data this
  // build has no knowledge of. PRD I10 has to survive a journal round trip
  // exactly as it survives a save -- and it does so for the same reason, that
  // the journal calls saveNpaint() with the document's own carry.
  auto makeFixtureCarry = []() {
    NpaintCarry carry;
    NpaintAttribute s;
    s.name = "np:futureNote";
    s.type = NpaintAttribute::Type::String;
    s.stringValue = "written by a newer build";
    NpaintAttribute i;
    i.name = "np:futureRevision";
    i.type = NpaintAttribute::Type::Int;
    i.intValue = 424242;
    carry.documentAttributes = {s, i};

    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    pigment.channelNames = {"pig.c0", "pig.m"};
    pigment.sampleTypeName = "float";
    {
      const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                              0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
      pigment.rawPixels.resize(sizeof(vals));
      std::memcpy(pigment.rawPixels.data(), vals, sizeof(vals));
    }
    NpaintAttribute pk;
    pk.name = "np:kind";
    pk.type = NpaintAttribute::Type::String;
    pk.stringValue = "Pigment";
    pigment.attributes = {pk};
    carry.rawParts = {pigment};
    carry.partOrder = {{NpaintPartSlot::Kind::Layer, 0},
                       {NpaintPartSlot::Kind::RawPart, 0},
                       {NpaintPartSlot::Kind::Layer, 1}};
    carry.layerPartNames = {"L0001", "L0003"};
    carry.basis = "future-basis-v9";
    return carry;
  };
  auto makeFixtureDocument = [&]() {
    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.375f;
    doc.layers[0].visible = false;
    doc.layers[0].locked = true;
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    second.parent = "L0009";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 0.5f);
    return doc;
  };
  auto carryIntact = [](const NpaintCarry& c) {
    if (c.documentAttributes.size() != 2) return false;
    bool sawNote = false, sawRev = false;
    for (const NpaintAttribute& a : c.documentAttributes) {
      if (a.name == "np:futureNote" && a.stringValue == "written by a newer build") sawNote = true;
      if (a.name == "np:futureRevision" && a.intValue == 424242) sawRev = true;
    }
    if (!(sawNote && sawRev)) return false;
    if (c.rawParts.size() != 1) return false;
    const NpaintRawPart& p = c.rawParts[0];
    if (p.name != "L0002" || p.sampleTypeName != "float") return false;
    if (p.channelNames != std::vector<std::string>{"pig.c0", "pig.m"}) return false;
    const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                            0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
    if (p.rawPixels.size() != sizeof(vals)) return false;
    if (std::memcmp(p.rawPixels.data(), vals, sizeof(vals)) != 0) return false;
    if (c.basis != "future-basis-v9") return false;
    return c.partOrder.size() == 3 && c.partOrder[1].kind == NpaintPartSlot::Kind::RawPart;
  };

  auto writeTextFile = [](const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
  };
  // A second, independent FNV-1a 64, written here rather than exposed from
  // app/Journal on purpose: a test that reuses the implementation it is
  // checking proves only that the implementation agrees with itself. When
  // this one and the module's disagree, the "a hand-built entry is intact"
  // assertion below fails.
  auto fnv1a = [](const std::string& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) {
      h ^= c;
      h *= 1099511628211ull;
    }
    return h;
  };

  // --- Availability, and the honest answer for each build -----------------
  {
    check(journalAvailable() == kOiioBuild,
          "the journal is available exactly in the build that has a writer");
    const std::string why = journalUnavailableReason();
    check(why.empty() == kOiioBuild,
          "...and it explains itself exactly when it is not");
    if (!kOiioBuild) {
      check(contains(why, ".npaint") && contains(why, "NP_USE_OIIO") && contains(why, "cmake"),
            "the reason is io/NpaintFile's own refusal, naming the cmake line -- not a "
            "second vocabulary invented here");
      check(!fs::exists("journal-availability-probe.npaint", ec),
            "...and asking for the reason opened no file: the probe stops at the backend "
            "gate");
    }
  }

  // --- Where the scratch lives --------------------------------------------
  {
    check(defaultJournalRootPath() == root, "$NP_JOURNAL_DIR overrides the scratch location");
    unsetenv("NP_JOURNAL_DIR");
    const std::string fallback = defaultJournalRootPath();
    setenv("NP_JOURNAL_DIR", root.c_str(), 1);
    check(contains(fallback, "naturalPaint") && contains(fallback, "recovery"),
          "the default is a per-user application-data path under naturalPaint/recovery");
    check(!contains(fallback, "Caches"),
          "...and deliberately NOT under Caches, which the system may purge -- after a "
          "crash the journal is the only copy of the work");
  }

  // --- The timer rule, as a pure function ---------------------------------
  //
  // Asserted in both builds because journalWriteDue() is where the rule
  // lives; JournalSession::tick() calls it and holds no second copy, so a
  // build with no session still checks the timer itself.
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "Timing");
    JournalEntryState never;
    check(journalWriteDue(doc, never, 0.0, 60.0) == JournalDue::No,
          "a clean document is never due -- nothing to lose");

    doc.recordEdit("add layer");  // structural by default
    check(journalWriteDue(doc, never, 0.0, 60.0) == JournalDue::Structural,
          "a dirty document that has never been journalled is due immediately");

    JournalEntryState written;
    written.everWritten = true;
    written.revision = doc.revision;
    written.structuralRevision = doc.structuralRevision;
    written.lastWriteSeconds = 100.0;
    check(journalWriteDue(doc, written, 100.0, 60.0) == JournalDue::No,
          "...and is not due again until something changes");

    OpenDocument painted = doc;
    painted.recordEdit("paint stroke", EditKind::Content);
    check(journalWriteDue(painted, written, 100.0, 60.0) == JournalDue::No,
          "a content edit does NOT trigger a write of its own (ADR-0008 rejects a disk "
          "write per keystroke)");
    check(journalWriteDue(painted, written, 159.9, 60.0) == JournalDue::No,
          "...it waits for the interval");
    check(journalWriteDue(painted, written, 160.0, 60.0) == JournalDue::Interval,
          "...and fires when the interval has elapsed, to the second");

    OpenDocument structural = doc;
    structural.recordEdit("delete layer", EditKind::Structural);
    check(journalWriteDue(structural, written, 100.0, 60.0) == JournalDue::Structural,
          "a STRUCTURAL edit is due at once, not at the next interval (PRD O5)");
    check(structural.structuralRevision == doc.structuralRevision + 1 &&
              painted.structuralRevision == doc.structuralRevision,
          "...because only a structural edit moves structuralRevision");
    check(painted.isDirty() && painted.revision == doc.revision + 1,
          "...while both kinds are equally dirty and equally unsaved");

    JournalEntryState held = written;
    held.overdue = true;
    check(journalWriteDue(doc, held, 100.0, 60.0) == JournalDue::Overdue,
          "a write deferred by a stroke stays due, rather than waiting a whole interval");

    OpenDocument saved = painted;
    saved.savedRevision = saved.revision;
    check(journalWriteDue(saved, written, 1.0e9, 60.0) == JournalDue::No,
          "a saved document is not due at any time -- its content is in the user's file");
  }

  // --- Beginning a session ------------------------------------------------
  //
  // The one place the two builds diverge structurally: without a writer there
  // is no session, and no directory is created either, because an empty
  // scratch directory would be offered for recovery next launch and hold
  // nothing.
  {
    JournalSession session;
    std::string beginError;
    const bool begun = session.begin({}, &beginError);
    check(begun == kOiioBuild, "a journal session begins exactly in the build that can write");
    if (!kOiioBuild) {
      check(contains(beginError, "NP_USE_OIIO"),
            "...and refuses with io/NpaintFile's own named refusal");
      check(!fs::exists(root, ec),
            "...creating no scratch directory at all: an empty one would be offered next "
            "launch and hold nothing");
    } else {
      check(session.active() && contains(session.directory(), root.c_str()),
            "the session directory is under the configured root");
      const std::string name = fs::path(session.directory()).filename().string();
      // PRD O8's "named and dated", in the directory name itself:
      // session-YYYYMMDD-HHMMSS-<pid>.
      bool shaped = name.rfind("session-", 0) == 0 && name.size() >= 24;
      if (shaped)
        for (size_t i = 8; i < 16; ++i)
          if (i != 16 && !std::isdigit(static_cast<unsigned char>(name[i]))) shaped = false;
      check(shaped, "...and is named and dated: session-YYYYMMDD-HHMMSS-<pid>");
      check(fs::exists(session.directory() + "/session.txt", ec) &&
                fs::exists(session.directory() + "/session.lock", ec),
            "...with a dated descriptor and a lock file beside it");

      // The flock probe: our own live session must never be offered back to
      // us. This is the assertion that a pid check could not make, and it is
      // the same mechanism that makes a lock left by a machine that lost
      // power impossible -- the kernel releases it when the holder dies.
      check(discoverRecoverySessions().empty(),
            "a LIVE session is not offered for recovery -- the flock probe, not the pid");

      std::string finishError;
      check(session.finishClean(&finishError) && !fs::exists(session.directory(), ec),
            "a clean shutdown removes the whole scratch directory");
      check(discoverRecoverySessions().empty(),
            "...so a session that ended normally leaves nothing to offer (PRD O8)");
    }
  }

  // --- A hand-built journal entry: the integrity check, in both builds -----
  //
  // The model file here is 64 bytes of nonsense rather than a real `.npaint`,
  // deliberately: what is being checked is that the size-and-hash record is
  // consulted BEFORE the file format reader is, which is what makes a
  // truncated journal a named refusal instead of a half-loaded document. In
  // the build with no reader at all, that ordering is provable -- the refusal
  // must be about truncation and must NOT be io/NpaintFile's missing-backend
  // message.
  {
    const std::string sessionDir = root + "/session-20260101-120000-4242";
    fs::create_directories(sessionDir, ec);
    writeTextFile(sessionDir + "/session.txt",
                  "# naturalPaint recovery session v1\nstartedAtEpoch 1767268800\n"
                  "startedAtLocal 2026-01-01 12:00:00\npid 4242\nend\n");
    const std::string modelPath = sessionDir + "/doc-0001.npaint";
    const std::string modelBody(64, 'x');
    writeTextFile(modelPath, modelBody);

    auto sidecar = [&](uint64_t bytes, uint64_t hash, bool terminate) {
      std::string s = "# naturalPaint journal entry v1";
      s += "\nid 7";
      s += "\nslot 1";
      s += "\nmodel doc-0001.npaint";
      s += "\nmodelBytes " + std::to_string(bytes);
      s += "\nmodelHash " + std::to_string(hash);
      s += "\npath /tmp/some/where/painting.npaint";
      s += "\ntitle ";
      s += "\ndisplayName painting.npaint";
      s += "\nrevision 4";
      s += "\nsavedRevision 1";
      s += "\nstructuralRevision 2";
      s += "\nresidency Eager";
      s += "\neditsDropped 0";
      s += "\nedit place image as layer";
      s += "\nedit duplicate";
      s += "\nunsavedSummary 3 unsaved changes";
      s += "\nwrittenAtEpoch 1767268860";
      s += "\nwrittenAtLocal 2026-01-01 12:01:00\n";
      if (terminate) s += "end\n";
      return s;
    };
    const std::string sidecarPath = sessionDir + "/doc-0001.journal";

    // (1) Sound entry: size and hash agree.
    writeTextFile(sidecarPath, sidecar(64, fnv1a(modelBody), true));
    std::vector<RecoverySession> found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1,
          "an unclean scratch directory is discovered, with its journalled document");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      check(found[0].startedAtLocal == "2026-01-01 12:00:00" && found[0].pid == 4242,
            "...named and dated from what the session recorded, not from a file mtime "
            "(which would date it to the crash)");
      check(found[0].documents[0].intact && found[0].documents[0].problem.empty(),
            "...and its integrity record checks out (two independent FNV-1a agreeing)");
      check(found[0].documents[0].displayName == "painting.npaint" &&
                found[0].documents[0].boundPath == "/tmp/some/where/painting.npaint" &&
                found[0].documents[0].unsavedSummary == "3 unsaved changes",
            "...offered by name, with the file it came from and what is unsaved");

      OpenDocument out;
      const DocumentOpResult r = recoverDocument(found[0].documents[0], &out);
      check(!r.ok && !contains(r.error, "truncated") && !contains(r.error, "hash"),
            "an intact entry gets as far as the format reader (this one is not a real "
            ".npaint, so the reader is what refuses it)");
    }

    // (2) Truncated model: the size disagrees.
    writeTextFile(modelPath, std::string(40, 'x'));
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 && !found[0].documents[0].intact,
          "a TRUNCATED journalled document is detected");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      const RecoveryDocument& entry = found[0].documents[0];
      check(contains(entry.problem, "truncated") && contains(entry.problem, "40") &&
                contains(entry.problem, "64"),
            "...and the refusal names both byte counts rather than saying 'corrupt'");
      OpenDocument out;
      const DocumentOpResult r = recoverDocument(entry, &out);
      check(!r.ok && contains(r.error, "truncated") && !contains(r.error, "NP_USE_OIIO"),
            "...and recovery refuses it on the integrity record, BEFORE the format reader "
            "-- provably so in the build that has no reader");
      check(fs::exists(modelPath, ec) && fs::exists(sidecarPath, ec),
            "...leaving both journal files exactly where they were: a recovery that can "
            "destroy what it failed to read is worse than none");
    }

    // (3) Right length, wrong contents.
    writeTextFile(modelPath, std::string(63, 'x') + "y");
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 &&
              !found[0].documents[0].intact &&
              contains(found[0].documents[0].problem, "hash"),
          "a model file of the right length but the wrong contents is refused too");

    // (4) The sidecar itself truncated -- the crash that happens *during* the
    // journal write. Rename-into-place makes this unreachable in practice,
    // which is exactly why the reader must still refuse it rather than trust
    // the mechanism.
    writeTextFile(modelPath, modelBody);
    writeTextFile(sidecarPath, sidecar(64, fnv1a(modelBody), false));
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 &&
              !found[0].documents[0].intact &&
              contains(found[0].documents[0].problem, "truncated"),
          "a journal entry with no terminating 'end' line is refused, not half-read");

    // (5) Explicit discard is the only thing that deletes.
    found = discoverRecoverySessions();
    std::string discardError;
    check(discardRecoverySession(found[0], &discardError) && !fs::exists(sessionDir, ec),
          "discarding a session is a separate, explicit call -- nothing else deletes");
    check(discoverRecoverySessions().empty(), "...and it is gone from the offer");
  }

  // --- The real thing: write, crash, recover ------------------------------
  if (kOiioBuild) {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.carry = makeFixtureCarry();
    doc.path = "/tmp/np-not-written-to/original.npaint";
    doc.recordEdit("place image as layer");
    const Document before = doc.document;

    DocumentSession documents;
    OpenDocument* live = documents.add(std::move(doc));
    std::string crashedDirectory;

    {
      JournalSession session;
      std::string beginError;
      check(session.begin({}, &beginError), "a session begins for the round trip");
      crashedDirectory = session.directory();

      // A structural edit is due at once, so one tick at t=0 writes it.
      JournalTickResult tickResult = session.tick(documents, {0.0, false});
      check(tickResult.documentsWritten == 1 && tickResult.activeDocumentWritten &&
                tickResult.errors.empty(),
            "the timer writes the ACTIVE document on a tick -- no deactivation anywhere "
            "(PRD O6)");
      check(fs::exists(session.directory() + "/doc-0001.npaint", ec) &&
                fs::exists(session.directory() + "/doc-0001.journal", ec),
            "...as a `.npaint` written by saveNpaint plus its sidecar (PRD O7)");
      check(!fs::exists(session.directory() + "/doc-0001.tmp.npaint", ec),
            "...with the write-to-temp-then-rename temporary gone");
      check(!fs::exists(live->path, ec),
            "...and the user's own file untouched: autosave never writes over it (PRD O9)");

      tickResult = session.tick(documents, {0.1, false});
      check(tickResult.documentsWritten == 0,
            "an unchanged document is not rewritten on every tick");

      // PRD O6, the part the ADR says the old scheme got wrong: a tile
      // changed in the ACTIVE document reaches disk on the timer.
      writeStraight(live->document, 0, 3, 3, 0.875f, 0.5f, 0.25f, 1.0f);
      live->recordEdit("paint stroke on layer 1", EditKind::Content);
      tickResult = session.tick(documents, {0.2, false});
      check(tickResult.documentsWritten == 0,
            "a content edit alone does not write before the interval");
      tickResult = session.tick(documents, {0.2 + kJournalIntervalSeconds, true});
      check(tickResult.documentsWritten == 0 && tickResult.deferredByStroke == 1,
            "a due write that collides with an active stroke is deferred (PRD O10)");
      tickResult = session.tick(documents, {0.3 + kJournalIntervalSeconds, false});
      check(tickResult.documentsWritten == 1 && tickResult.activeDocumentWritten,
            "...and happens on the first tick after the stroke ends, not an interval later");

      // The in-process crash: the session's destructor releases the lock and
      // leaves the directory. No signal, no kill -9, no second process --
      // and it is the same code path an abnormal exit takes, because
      // finishClean() is the only thing that removes anything.
    }
    check(fs::exists(crashedDirectory, ec),
          "a session that ends without finishClean leaves its scratch directory behind");

    std::vector<RecoverySession> found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].directory == crashedDirectory &&
              found[0].documents.size() == 1,
          "...and the now-unlocked directory is offered on the next launch");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      const RecoveryDocument& entry = found[0].documents[0];
      check(entry.intact && entry.displayName == "original.npaint" &&
                entry.boundPath == live->path,
            "the offer names the document and the file it came from");
      check(!found[0].startedAtLocal.empty() && found[0].startedAtLocal.size() == 19,
            "...and is dated (PRD O8)");

      OpenDocument recovered;
      const DocumentOpResult r = recoverDocument(entry, &recovered);
      check(r.ok, "recovery reads the journal back through loadNpaint");
      if (r.ok) {
        check(tilesIdentical(recovered.document, live->document),
              "every tile comes back bit-identical at zero tolerance, the paint stroke "
              "included");
        check(!tilesIdentical(recovered.document, before),
              "...and is genuinely the journalled state, not the state it was opened in");
        check(std::fabs(readStraightRed(recovered.document, 0, 3, 3) - 0.875f) < 1.0e-3f,
              "...so the ACTIVE document's dirty tiles really did reach disk on the timer");
        check(layerMetadataIdentical(recovered.document, live->document),
              "every np:* layer attribute survives -- kind, name, blend, opacity, visible, "
              "locked, parent");
        check(carryIntact(recovered.carry),
              "the PRD I10 carry survives journal -> recover: unknown attributes and a "
              "whole foreign part");
        check(recovered.path == live->path,
              "the recovered document is bound to the file it came from...");
        check(recovered.isDirty() && recovered.revision == live->revision &&
                  recovered.savedRevision == live->savedRevision,
              "...and comes back dirty, with the same revision it was journalled at");
        check(recovered.unsavedEdits.size() == 2 &&
                  recovered.unsavedEdits[0] == "place image as layer" &&
                  recovered.unsavedEdits[1] == "paint stroke on layer 1",
              "...naming what is unsaved, in order, so the user knows what was rescued");
        check(recovered.id != live->id,
              "...with a fresh document id: a recovered document is not the same document");
      }
      check(fs::exists(entry.modelPath, ec),
            "a successful recovery deletes nothing either -- a crash before the user saves "
            "must not cost them the journal twice");
    }

    // A saved document's journal is dropped: its content is in the user's own
    // file, and offering it next launch would train the user to dismiss the
    // one dialog that must not be dismissed reflexively.
    {
      JournalSession session;
      std::string beginError;
      check(session.begin({}, &beginError), "a second session begins");
      session.tick(documents, {0.0, false});
      check(session.entryCount() == 1, "the dirty document is journalled");
      live->savedRevision = live->revision;  // as a successful save leaves it
      const JournalTickResult afterSave = session.tick(documents, {1.0, false});
      check(afterSave.entriesDropped == 1 && session.entryCount() == 0 &&
                !fs::exists(session.directory() + "/doc-0001.npaint", ec),
            "...and its journal is dropped once it is clean and bound (ADR-0008)");
      std::string finishError;
      session.finishClean(&finishError);
    }
    fs::remove_all(crashedDirectory, ec);
  }

  // --- What the interval costs, measured ----------------------------------
  if (kOiioBuild) {
    // io/TileResidency's own "realistic document": 2048x2048, every one of
    // the 256 tiles occupied, i.e. 32.0 MiB of half data. The interval in
    // app/Journal.hpp is derived from this number, so it is measured every
    // run rather than quoted from a comment.
    Document big = Document::createBlank(2048, 2048, WorkingSpace{});
    for (int32_t ty = 0; ty < 16; ++ty)
      for (int32_t tx = 0; tx < 16; ++tx)
        writeStraight(big, 0, tx * 128 + 1, ty * 128 + 1, 0.5f, 0.25f, 0.125f, 1.0f);
    check(big.layers[0].rgbTiles->occupiedTileCount() == 256,
          "the cost fixture really is 256 tiles (32.0 MiB of half data)");

    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = std::move(big);
    doc.recordEdit("fill");

    JournalSession session;
    std::string beginError;
    double writeSeconds = 0.0;
    if (session.begin({}, &beginError)) {
      std::string writeError;
      check(session.writeEntry(doc, &writeError, &writeSeconds), "one journal write of it");

      const auto copyStart = std::chrono::steady_clock::now();
      const Document snapshot = doc.document;
      const auto copyEnd = std::chrono::steady_clock::now();
      const double copySeconds = std::chrono::duration<double>(copyEnd - copyStart).count();
      check(snapshot.layers[0].rgbTiles->occupiedTileCount() == 256, "and one deep copy of it");

      const double duty = writeSeconds / kJournalIntervalSeconds;
      std::printf("    [measured] journal write of a 2048x2048 document (256 tiles, 32.0 MiB "
                  "half): %.3f s\n",
                  writeSeconds);
      std::printf("    [measured] interval %.0f s -> %.2f%% duty cycle; a crash loses at most "
                  "%.0f s of edits\n",
                  kJournalIntervalSeconds, duty * 100.0, kJournalIntervalSeconds);
      std::printf("    [measured] deep copy of the same document: %.3f s -- what a background "
                  "writer would still cost this thread without COW tiles (Phase 5 step 6)\n",
                  copySeconds);
      // A ceiling, not a target: the point is to catch an order-of-magnitude
      // regression in the writer, not to police normal variance.
      check(duty < 0.03,
            "one journal write stays under 3% of the interval -- the arithmetic the "
            "interval was chosen from still holds");

      // fsync vs F_FULLFSYNC, which is the measurement app/Journal's
      // durability choice rests on.
      const std::string syncPath = session.directory() + "/sync-probe.bin";
      const std::string payload(1u << 20, 'z');
      auto timeSync = [&](bool full) {
        const int fd = ::open(syncPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) return -1.0;
        ssize_t wrote = ::write(fd, payload.data(), payload.size());
        (void)wrote;
        const auto t0 = std::chrono::steady_clock::now();
        if (full)
          ::fcntl(fd, F_FULLFSYNC);
        else
          ::fsync(fd);
        const auto t1 = std::chrono::steady_clock::now();
        ::close(fd);
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
      };
      const double plain = timeSync(false);
      const double full = timeSync(true);
      std::printf("    [measured] durability of a 1 MiB write: fsync %.2f ms, F_FULLFSYNC "
                  "%.2f ms (%.1fx) -- why the journal uses the first\n",
                  plain, full, plain > 0.0 ? full / plain : 0.0);
      check(plain >= 0.0 && full >= 0.0, "both durability calls are available on this system");

      std::string finishError;
      session.finishClean(&finishError);
    }
  }

  // --- What discovery costs at launch (PRD A2's budget) -------------------
  {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<RecoverySession> found = discoverRecoverySessions();
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("    [measured] launch-time discovery over %zu session(s): %.3f ms of PRD "
                "A2's 100 ms cold-start budget\n",
                found.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  // --- Clean up ------------------------------------------------------------
  fs::remove_all(root, ec);
  check(!fs::exists(root, ec), "every scratch file this section wrote is removed");
  if (previousRoot)
    setenv("NP_JOURNAL_DIR", savedRoot.c_str(), 1);
  else
    unsetenv("NP_JOURNAL_DIR");

  std::printf("[selftest] recovery journal %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


// ---------------------------------------------------------------------------
// PLAN.md Phase 5 step 1 -- multiple layers, with reorder, visibility, lock
// and opacity, and the `over` compositing that makes the last two mean
// anything. See app/SelfTest.hpp for the section's own contents list.
// ---------------------------------------------------------------------------
bool runBlendTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // **Almost everything below asserts at exactly zero tolerance, and that is
  // the design of the fixtures rather than a shortcut.** Every value fed to
  // `blendPixel()` here is a dyadic rational with a short binary expansion
  // (halves, quarters, eighths), and every formula in core/Blend is built from
  // `+`, `-`, `*`, `min` and `max` only -- there is not a single division in
  // any of them, which is precisely why they were derived into premultiplied
  // form instead of un-premultiplying and re-premultiplying around a straight-
  // colour blend. So each product and each sum lands back on the float grid
  // exactly, and a hand-computed reference can be compared with `==`.
  //
  // Two places need a tolerance and each derives its own:
  //
  //  * `kUnpremultiplyTol`, for anything read back through
  //    `flattenDocumentToLinear()`, whose final un-premultiply is one float
  //    division by the composited alpha. IEEE-754 requires it correctly
  //    rounded, so at most half an ulp: for a result in [0.25, 1) that is
  //    2^-25 = 2.98e-8 absolute. Landed 1.0e-7 -- 3.4x the derived bound,
  //    the identical figure and identical derivation runLayerStackTest() uses.
  //  * `kMixboxTol`, for the Mixbox comparison, derived at its own fixture
  //    below where the quantities it bounds are in view.
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  auto eq4 = [](const std::array<float, 4>& a, float r, float g, float b, float alpha) {
    return a[0] == r && a[1] == g && a[2] == b && a[3] == alpha;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // The six modes this build composites, in table order, for the loops that
  // must hold for *every* mode rather than for one.
  const BlendMode kImplemented[] = {BlendMode::Normal, BlendMode::Plus, BlendMode::Multiply,
                                    BlendMode::Screen, BlendMode::Min,  BlendMode::Max};

  // --- The vocabulary: one table, one parse, one spelling ----------------
  {
    check(allBlendModes().size() == 7,
          "table: seven modes -- the linear-safe six plus `Mix` (PLAN.md Phase 5 step 2)");

    bool indexedByEnumerator = true, namesUnique = true, labelsUnique = true,
         roundTrips = true;
    for (const BlendModeInfo& a : allBlendModes()) {
      if (&blendModeInfo(a.mode) != &a) indexedByEnumerator = false;
      const std::optional<BlendMode> back = blendModeFromName(a.name);
      if (!back.has_value() || *back != a.mode) roundTrips = false;
      for (const BlendModeInfo& b : allBlendModes()) {
        if (&a == &b) continue;
        if (std::string(a.name) == b.name) namesUnique = false;
        if (std::string(a.label) == b.label) labelsUnique = false;
      }
    }
    check(indexedByEnumerator,
          "table: blendModeInfo(m) returns m's own row -- the table is indexed by enumerator, "
          "so a row in the wrong place would hand out another mode's B7 classification");
    check(namesUnique && labelsUnique,
          "table: every np:blend name and every UI label is distinct");
    check(roundTrips,
          "table: name -> mode -> name round-trips for all seven, so nothing on disk means "
          "two things");

    check(blendModeFromName(kDefaultBlendName) == BlendMode::Normal,
          "table: core/Layer.hpp's kDefaultBlendName (\"normal\") IS `over` -- the default a "
          "Layer is constructed with must resolve, or every untouched document warns");
    check(!blendModeFromName("linear-burn").has_value() && !blendModeFromName("").has_value(),
          "table: an unrecognised name and an empty one both fail to parse rather than "
          "resolving to a default");
    check(!blendModeFromName("Multiply").has_value() && !blendModeFromName("MULTIPLY").has_value(),
          "table: the match is case-SENSITIVE -- two spellings on disk would both be `the` "
          "name and neither would round-trip the other");

    bool implementedAgrees = true;
    for (const BlendModeInfo& info : allBlendModes())
      if (blendIsImplemented(info.name) != info.compositesPixels) implementedAgrees = false;
    check(implementedAgrees,
          "table: blendIsImplemented() answers exactly BlendModeInfo::compositesPixels, so "
          "there is one source of truth for what this build can composite");
    check(blendIsImplemented("multiply") && blendIsImplemented("screen") &&
              blendIsImplemented("min") && blendIsImplemented("max") &&
              blendIsImplemented("plus"),
          "table: the five modes step 1 could not composite are implemented now");
    check(!blendIsImplemented("mix") && !blendIsImplemented("linear-burn") &&
              !blendIsImplemented(""),
          "table: `mix` is NOT -- it lerps latents and no layer stores one (Phase 5 step 3) "
          "-- and neither is an unrecognised name or an empty one");
  }

  // --- PRD B7: display-referred modes are labelled as such ---------------
  //
  // The label is a field on the mode's metadata, so this section reads the
  // classification out of the data and then proves the classification is
  // *true* rather than merely present.
  {
    size_t displayReferred = 0;
    bool markerMatchesData = true;
    for (const BlendModeInfo& info : allBlendModes()) {
      const std::string entry = blendMenuEntryText(info.mode);
      const bool marked = entry.find("(display-referred)") != std::string::npos;
      if (marked != (info.space == BlendSpace::DisplayReferred)) markerMatchesData = false;
      if (info.space == BlendSpace::DisplayReferred) ++displayReferred;
    }
    check(markerMatchesData,
          "B7: every mode's menu entry carries the display-referred marker exactly when its "
          "BlendModeInfo::space says so -- the label is derived from the data on every call, "
          "so there is no path from a mode to menu text that skips it");
    check(displayReferred == 1 &&
              blendModeInfo(BlendMode::Screen).space == BlendSpace::DisplayReferred,
          "B7: exactly one mode in the set is display-referred, and it is `screen`");

    // ...and here is why, numerically, rather than by assertion. The criterion
    // core/Blend.hpp states is monotonicity over the whole non-negative range
    // a linear working space can hold. At full coverage on both sides the
    // formulas reduce to their straight-colour forms, so this is the plain
    // test: raise the backdrop and see whether the result rises.
    const std::array<float, 4> opaqueSrc{2.0f, 2.0f, 2.0f, 1.0f};
    const std::array<float, 4> dimBackdrop{2.0f, 2.0f, 2.0f, 1.0f};
    const std::array<float, 4> brightBackdrop{3.0f, 3.0f, 3.0f, 1.0f};
    const float screenDim = blendPixel(BlendMode::Screen, opaqueSrc, dimBackdrop)[0];
    const float screenBright = blendPixel(BlendMode::Screen, opaqueSrc, brightBackdrop)[0];
    std::printf("  [measured] screen(2,2) = %.3f, screen(2,3) = %.3f (both should be >= 2 for a "
                "mode with no reference white)\n",
                static_cast<double>(screenDim), static_cast<double>(screenBright));
    check(screenDim == 0.0f && screenBright == -1.0f,
          "B7: screen(2,2) is exactly 0 and screen(2,3) exactly -1 -- above 1.0 it is not even "
          "monotone, so adding light makes it darker. That is what `1.0 is white` costs, and "
          "it is why screen carries the label");

    bool othersMonotone = true;
    for (BlendMode m : kImplemented) {
      if (m == BlendMode::Screen) continue;
      const float dim = blendPixel(m, opaqueSrc, dimBackdrop)[0];
      const float bright = blendPixel(m, opaqueSrc, brightBackdrop)[0];
      if (!(bright >= dim)) othersMonotone = false;
    }
    check(othersMonotone,
          "B7: and every other implemented mode IS monotone at the same HDR values -- so the "
          "classification separates the set rather than labelling everything");

    // The row shows the label too, not only the dropdown.
    Layer row;
    row.kind = LayerKind::RGB;
    row.blend = "screen";
    check(contains(layerRowSubLine(row), "SCREEN (display-referred)"),
          "B7: the layer row's sub-line carries the label as well -- a label that only "
          "appeared while the dropdown was open would not be `labelled as such`");
    row.blend = "multiply";
    check(!contains(layerRowSubLine(row), "display-referred") &&
              !contains(layerRowSubLine(row), "(!)"),
          "B7: a linear-light mode's row carries neither the label nor the unimplemented "
          "marker -- the working space IS linear, so labelling the majority case would bury "
          "the minority one");
    check(contains(blendMenuEntryText(BlendMode::Mix), "(not composited yet)"),
          "B7: `Mix` additionally says it does not composite yet, so a P0 feature cannot sit "
          "in a menu appearing to work");
  }

  // --- Every mode against a hand-computed reference: OPAQUE --------------
  //
  // Both fully opaque, so each formula collapses to its straight-colour form
  // and the references are the textbook ones:
  //
  //   src straight = premultiplied = (0.5,  0.25, 0.75), a = 1
  //   dst straight = premultiplied = (0.5,  1.0,  0.25), a = 1
  //
  //   plus      cs + cb            = (1.0,   1.25,  1.0)
  //   multiply  cs * cb            = (0.25,  0.25,  0.1875)
  //   screen    cs + cb - cs*cb    = (0.75,  1.0,   0.8125)
  //   min                          = (0.5,   0.25,  0.25)
  //   max                          = (0.5,   1.0,   0.75)
  //   over      cs                 = (0.5,   0.25,  0.75)
  //
  // Note plus and screen deliberately exceed 1.0 in a channel: nothing here
  // clamps, because clamping is a display/export policy (color/Space.hpp) and
  // io/Export makes that decision at its own quantization step.
  {
    const std::array<float, 4> s{0.5f, 0.25f, 0.75f, 1.0f};
    const std::array<float, 4> d{0.5f, 1.0f, 0.25f, 1.0f};
    check(eq4(blendPixel(BlendMode::Normal, s, d), 0.5f, 0.25f, 0.75f, 1.0f),
          "opaque: over gives the source exactly -- an opaque layer hides what is under it");
    check(eq4(blendPixel(BlendMode::Plus, s, d), 1.0f, 1.25f, 1.0f, 1.0f),
          "opaque: plus gives (1.0, 1.25, 1.0) exactly, unclamped past 1.0");
    check(eq4(blendPixel(BlendMode::Multiply, s, d), 0.25f, 0.25f, 0.1875f, 1.0f),
          "opaque: multiply gives (0.25, 0.25, 0.1875) exactly");
    check(eq4(blendPixel(BlendMode::Screen, s, d), 0.75f, 1.0f, 0.8125f, 1.0f),
          "opaque: screen gives (0.75, 1.0, 0.8125) exactly");
    check(eq4(blendPixel(BlendMode::Min, s, d), 0.5f, 0.25f, 0.25f, 1.0f),
          "opaque: min takes the darker channel of each pair");
    check(eq4(blendPixel(BlendMode::Max, s, d), 0.5f, 1.0f, 0.75f, 1.0f),
          "opaque: max takes the lighter channel of each pair");
  }

  // --- Every mode against a hand-computed reference: PARTIAL ALPHA -------
  //
  // **This is the fixture that catches the classic bug in this area.**
  // Multiply and screen are almost always written for straight, opaque colour;
  // applied to premultiplied values with partial alpha, the naive transcription
  // (`cs * cb`, `cs + cb - cs*cb` with nothing else) is wrong for multiply and
  // right for screen, and there is no way to tell which without doing the
  // arithmetic. So it is done here, twice, by two different routes.
  //
  // Fixture:
  //   src straight (1, 0, 0.5) at as = 0.5  ->  cs = (0.5, 0,   0.25)
  //   dst straight (0, 1, 0.25) at ab = 0.5 ->  cb = (0,   0.5, 0.125)
  //
  // Route 1 -- core/Blend's premultiplied forms:
  //   sOnly = 1-ab = 0.5, bOnly = 1-as = 0.5, ao = 0.5 + 0.5*0.5 = 0.75
  //   over      cs + cb*0.5                      = (0.5,    0.25,  0.3125)
  //   plus      cs + cb                          = (0.5,    0.5,   0.375)
  //   multiply  cs*cb + cs*0.5 + cb*0.5          = (0.25,   0.25,  0.21875)
  //   screen    cs + cb - cs*cb                  = (0.5,    0.5,   0.34375)
  //   min       min(ab*cs, as*cb) + cs*.5 + cb*.5= (0.25,   0.25,  0.25)
  //   max       max(ab*cs, as*cb) + cs*.5 + cb*.5= (0.5,    0.5,   0.3125)
  //
  // Route 2 -- Porter-Duff regions, computed independently. At as = ab = 0.5
  // the pixel splits into three regions of area 0.25 each (source only,
  // backdrop only, both) plus 0.25 of nothing. Taking the blue channel of
  // multiply: source-only 0.25*0.5 = 0.125, backdrop-only 0.25*0.25 = 0.0625,
  // both 0.25*(0.5*0.25) = 0.03125. Sum 0.21875. It agrees, and it agrees for
  // a reason that has nothing to do with the algebra above -- which is the
  // point of doing it twice.
  {
    const std::array<float, 4> s{0.5f, 0.0f, 0.25f, 0.5f};
    const std::array<float, 4> d{0.0f, 0.5f, 0.125f, 0.5f};
    check(eq4(blendPixel(BlendMode::Normal, s, d), 0.5f, 0.25f, 0.3125f, 0.75f),
          "partial: over at 50%/50% gives (0.5, 0.25, 0.3125) at alpha 0.75");
    check(eq4(blendPixel(BlendMode::Plus, s, d), 0.5f, 0.5f, 0.375f, 0.75f),
          "partial: plus is the premultiplied sum, and its alpha is still the UNION alpha "
          "0.75 rather than Porter-Duff PLUS's 1.0 -- coverage does not add because light "
          "does");
    check(eq4(blendPixel(BlendMode::Multiply, s, d), 0.25f, 0.25f, 0.21875f, 0.75f),
          "partial: multiply gives (0.25, 0.25, 0.21875) -- the three-term premultiplied "
          "form, NOT the naive cs*cb, which would give (0, 0, 0.03125) and lose both "
          "layers wherever the other does not cover");
    check(eq4(blendPixel(BlendMode::Screen, s, d), 0.5f, 0.5f, 0.34375f, 0.75f),
          "partial: screen gives (0.5, 0.5, 0.34375) -- here the premultiplied form really "
          "IS the straight one, cs + cb - cs*cb, which is exactly the coincidence that gets "
          "assumed for multiply and is false there");
    check(eq4(blendPixel(BlendMode::Min, s, d), 0.25f, 0.25f, 0.25f, 0.75f),
          "partial: min gives (0.25, 0.25, 0.25), via min(ab*cs, as*cb) -- no division by "
          "alpha anywhere");
    check(eq4(blendPixel(BlendMode::Max, s, d), 0.5f, 0.5f, 0.3125f, 0.75f),
          "partial: max gives (0.5, 0.5, 0.3125)");

    // The naive premultiplied transcriptions, written here so the assertions
    // above are demonstrably not tautologies against a copy of the code.
    check(blendPixel(BlendMode::Multiply, s, d)[2] != s[2] * d[2],
          "partial: and the naive `cs*cb` genuinely differs from what multiply returns, so "
          "the assertion above could not pass against the wrong implementation");
  }

  // --- Alpha is `over` for every mode ------------------------------------
  //
  // The separable-blend formula's `ao` does not mention the blend function at
  // all: a blend mode changes colour, not coverage. Checked across the whole
  // alpha grid rather than at one pair, because this is the property that
  // makes "switch a layer's blend and the composite's alpha channel does not
  // move" true.
  {
    bool alphaIsOver = true;
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float as : alphas) {
      for (float ab : alphas) {
        const std::array<float, 4> s{0.25f * as, 0.5f * as, 0.75f * as, as};
        const std::array<float, 4> d{0.75f * ab, 0.25f * ab, 0.5f * ab, ab};
        const float expect = compositeOver(s, d)[3];
        for (BlendMode m : kImplemented)
          if (blendPixel(m, s, d)[3] != expect) alphaIsOver = false;
      }
    }
    check(alphaIsOver,
          "alpha: every mode produces exactly `over`'s alpha across a 5x5 alpha grid -- a "
          "blend mode changes colour, never coverage");
  }

  // --- A fully transparent source is an exact identity, for every mode ---
  //
  // If this fails for any mode, that mode is wrong: an empty layer must
  // contribute nothing. It holds exactly (not nearly) because as == 0 makes
  // cs == 0 for well-formed premultiplied data, which kills every term
  // carrying cs and leaves `cb * 1.0f` -- a multiplication by literal 1.0f.
  {
    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> backdrops[] = {
        {0.75f, 0.25f, 0.5f, 1.0f},      // opaque
        {0.375f, 0.125f, 0.25f, 0.5f},   // half-covered
        {0.0f, 0.0f, 0.0f, 0.0f},        // also empty
        {1.5f, 0.0f, 2.25f, 1.0f},       // above 1.0, where screen misbehaves
    };
    bool identity = true, passthrough = true;
    for (const std::array<float, 4>& b : backdrops) {
      for (BlendMode m : kImplemented) {
        if (blendPixel(m, clear, b) != b) identity = false;
        // ...and the mirror: an empty backdrop passes the source through
        // untouched, which is what makes a single-layer document composite
        // identically under any mode.
        if (blendPixel(m, b, clear) != b) passthrough = false;
      }
      // `Mix` too: it falls back to `over`, and `over` has the identity.
      if (blendPixel(BlendMode::Mix, clear, b) != b) identity = false;
    }
    check(identity,
          "transparent: a fully transparent source is a BIT-EXACT identity on the backdrop "
          "for every mode, over four backdrops including an HDR one");
    check(passthrough,
          "transparent: and a fully transparent backdrop passes the source through bit-"
          "exactly for every mode -- which is why a single-layer document composites the "
          "same whatever its blend");
  }

  // --- `over` did not move by one ulp ------------------------------------
  //
  // Step 1's regression boundary rests on `compositeOver()` being exactly what
  // it was, so this checks it against a second, independent transcription of
  // step 1's formula written here rather than called -- the same discipline
  // the journal section's second FNV-1a and step 1's second plain sum follow.
  // The values are deliberately NOT dyadic: 0.1f and 0.3f are not exact in
  // binary, so any reassociation of `src + dst*(1-src.a)` would show.
  {
    auto step1Over = [](const std::array<float, 4>& src, const std::array<float, 4>& dst) {
      const float inv = 1.0f - src[3];
      return std::array<float, 4>{src[0] + dst[0] * inv, src[1] + dst[1] * inv,
                                  src[2] + dst[2] * inv, src[3] + dst[3] * inv};
    };
    bool identicalToStep1 = true, dispatchIsOver = true;
    for (int i = 0; i <= 20; ++i) {
      for (int j = 0; j <= 20; ++j) {
        const float as = static_cast<float>(i) * 0.05f;
        const float ab = static_cast<float>(j) * 0.05f;
        const std::array<float, 4> s{0.1f * as, 0.3f * as, 0.7f * as, as};
        const std::array<float, 4> d{0.9f * ab, 0.3f * ab, 0.1f * ab, ab};
        if (compositeOver(s, d) != step1Over(s, d)) identicalToStep1 = false;
        if (blendPixel(BlendMode::Normal, s, d) != step1Over(s, d)) dispatchIsOver = false;
      }
    }
    check(identicalToStep1,
          "over: compositeOver() is bit-identical to step 1's formula across 441 non-dyadic "
          "alpha pairs -- moving it into core/Blend perturbed nothing");
    check(dispatchIsOver,
          "over: and blendPixel(Normal, ...) is the same function, not the general three-term "
          "form -- which is algebraically equal and NOT bit-equal, so the dispatch has to "
          "special-case it");
  }

  // --- The regression boundary, now for every mode -----------------------
  //
  // Step 1 asserts that a single-layer document and a non-overlapping
  // multi-layer one composite byte-identically to the plain sum they replaced.
  // This step could break that in a new way -- by making the answer depend on
  // the blend mode -- so the same claim is re-made here with a DIFFERENT blend
  // on every layer, against a second implementation of the plain sum written
  // in this test.
  {
    auto plainSum = [](const Document& doc) {
      const size_t w = static_cast<size_t>(doc.width);
      std::vector<float> out(w * static_cast<size_t>(doc.height) * 4, 0.0f);
      for (const Layer& layer : doc.layers) {
        if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;
        for (const auto& [coord, tile] : *layer.rgbTiles) {
          const PixelCoord origin = tileOrigin(coord);
          for (int32_t ty = 0; ty < kTileSize; ++ty) {
            const int32_t dy = origin.y + ty;
            if (dy < 0 || dy >= doc.height) continue;
            for (int32_t tx = 0; tx < kTileSize; ++tx) {
              const int32_t dx = origin.x + tx;
              if (dx < 0 || dx >= doc.width) continue;
              const std::array<float, 4> p = tile.readPixel(PixelCoord{tx, ty});
              float* o = &out[(static_cast<size_t>(dy) * w + static_cast<size_t>(dx)) * 4];
              for (int c = 0; c < 4; ++c) o[c] += p[c];
            }
          }
        }
      }
      return out;
    };
    auto addRgb = [](Document& doc, const char* blend) {
      Layer l;
      l.kind = LayerKind::RGB;
      l.rgbTiles.emplace();
      l.blend = blend;
      doc.layers.push_back(std::move(l));
    };
    auto writeStraight = [](Document& doc, size_t li, int32_t x, int32_t y, float r, float g,
                            float b, float a) {
      const PixelCoord p{x, y};
      doc.layers[li].rgbTiles->getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p),
                                                                     {r * a, g * a, b * a, a});
    };

    Document doc = Document::createBlank(300, 300, WorkingSpace{});
    doc.layers[0].blend = "multiply";
    addRgb(doc, "screen");
    addRgb(doc, "plus");
    addRgb(doc, "min");
    addRgb(doc, "max");
    // One texel per layer, all in different tiles, none overlapping, and with
    // values that are NOT exactly representable in half so a one-ulp
    // difference anywhere would show. One sits off-canvas to exercise the
    // clip, and one is translucent.
    writeStraight(doc, 0, 3, 3, 0.1f, 0.2f, 0.3f, 1.0f);
    writeStraight(doc, 1, 140, 3, 0.7f, 0.6f, 0.55f, 0.35f);
    writeStraight(doc, 2, 3, 140, 0.9f, 0.05f, 0.45f, 1.0f);
    writeStraight(doc, 3, 260, 260, 0.33f, 0.66f, 0.99f, 0.8f);
    writeStraight(doc, 4, 290, 290, 0.15f, 0.25f, 0.35f, 1.0f);

    const std::vector<float> composited = compositeDocumentPremultiplied(doc);
    const std::vector<float> summed = plainSum(doc);
    check(composited.size() == summed.size() &&
              std::memcmp(composited.data(), summed.data(),
                          composited.size() * sizeof(float)) == 0,
          "regression: five non-overlapping layers, each with a DIFFERENT blend mode, "
          "composite BYTE-identically to the plain sum step 1 replaced -- a blend mode "
          "cannot perturb content nothing overlaps");

    // And the negative control: one overlapping texel is enough to break it,
    // so the identity above is a property of non-overlap and not of the code.
    writeStraight(doc, 1, 3, 3, 0.4f, 0.4f, 0.4f, 0.5f);
    const std::vector<float> overlapped = compositeDocumentPremultiplied(doc);
    const std::vector<float> overlapSum = plainSum(doc);
    check(std::memcmp(overlapped.data(), overlapSum.data(),
                      overlapped.size() * sizeof(float)) != 0,
          "regression: and ONE overlapping texel breaks it, so the identity is about "
          "non-overlap rather than about the implementation");
  }

  // --- A blend mode really reaches the document walk ---------------------
  //
  // Everything above tests the primitive. This tests that core/Composite
  // resolves the layer's `blend` string and dispatches on it, at the document
  // level, hand-computed end to end.
  //
  //   bottom  straight (0.5, 1.0, 0.25) opaque, blend "normal"
  //   top     straight (0.5, 0.25, 0.75) opaque, blend "multiply"
  //   multiply of two opaque layers = the straight product
  //           = (0.25, 0.25, 0.1875), alpha 1
  //   alpha is exactly 1 so the flattener's un-premultiply is a division by
  //   1.0 and the whole chain is exact -- this one is a zero-tolerance check.
  {
    Document doc = Document::createBlank(1, 1, WorkingSpace{});
    Layer top;
    top.kind = LayerKind::RGB;
    top.rgbTiles.emplace();
    top.blend = "multiply";
    doc.layers.push_back(std::move(top));
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                   {0.5f, 1.0f, 0.25f, 1.0f});
    doc.layers[1].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                   {0.5f, 0.25f, 0.75f, 1.0f});

    std::vector<std::string> warnings;
    const DecodedImage flat = flattenDocumentToLinear(doc, &warnings);
    check(flat.valid() && flat.pixels[0] == 0.25f && flat.pixels[1] == 0.25f &&
              flat.pixels[2] == 0.1875f && flat.pixels[3] == 1.0f,
          "document: a `multiply` layer really multiplies through the flattener -- exactly "
          "(0.25, 0.25, 0.1875) at alpha 1, no tolerance");
    check(warnings.empty(),
          "document: and produces NO warning, because this build composites it faithfully "
          "now -- the step-1 approximation notice is gone for the five modes it landed");

    // The probe must agree, through the same dispatch. An eyedropper that read
    // `over` while the export wrote `multiply` would be a bug nobody could
    // explain.
    ProbeParams all;
    all.sampleAllLayers = true;
    const ProbeSample sample = probePixel(doc, PixelCoord{0, 0}, all);
    check(near(sample.linear[0], 0.25f, kUnpremultiplyTol) &&
              near(sample.linear[2], 0.1875f, kUnpremultiplyTol),
          "document: core/Probe dispatches on the same blend mode as the flattener, so the "
          "eyedropper and the export cannot disagree");

    // Switching only the blend mode must not move alpha -- the document-level
    // form of the per-pixel claim above.
    const float alphaMultiply = flat.pixels[3];
    doc.layers[1].blend = "screen";
    const DecodedImage screened = flattenDocumentToLinear(doc);
    check(screened.pixels[3] == alphaMultiply && screened.pixels[0] == 0.75f,
          "document: switching the blend mode changes colour (0.25 -> 0.75 in red) and "
          "leaves alpha exactly where it was");
  }

  // --- The unimplemented-blend contract, still holding in substance ------
  //
  // Step 1's ruling was: composited as `over`, warned by name, never silently,
  // never refused, and the value itself preserved verbatim (PRD I10). This
  // step changes *which* names fall into it, and must not weaken any of it.
  // The two remaining cases are checked separately because their sentences
  // differ -- an unknown name means "this build is behind the document",
  // `mix` means "this build is behind PLAN.md".
  {
    auto oneLayerDoc = [](const char* blend) {
      Document doc = Document::createBlank(1, 1, WorkingSpace{});
      Layer top;
      top.kind = LayerKind::RGB;
      top.rgbTiles.emplace();
      top.name = "Line pass";
      top.blend = blend;
      doc.layers.push_back(std::move(top));
      doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                     {0.5f, 0.0f, 0.0f, 0.5f});
      doc.layers[1].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                     {0.0f, 0.0f, 0.5f, 0.5f});
      return doc;
    };

    Document unknown = oneLayerDoc("linear-burn");
    std::vector<std::string> unknownWarnings;
    const DecodedImage unknownFlat = flattenDocumentToLinear(unknown, &unknownWarnings);
    check(unknownWarnings.size() == 1 && contains(unknownWarnings[0], "linear-burn") &&
              contains(unknownWarnings[0], "Line pass") &&
              contains(unknownWarnings[0], "newer build"),
          "unknown: an unrecognised blend still produces exactly one warning, naming the "
          "layer, the value, and where it most likely came from");
    check(contains(unknownWarnings[0], "multiply") && contains(unknownWarnings[0], "screen"),
          "unknown: and the sentence lists the modes that ARE implemented, generated from "
          "core/Blend's table rather than typed into the message");
    check(unknown.layers[1].blend == "linear-burn",
          "unknown: the value itself is untouched, so PRD I10 still carries it to disk "
          "verbatim -- which is the whole reason Layer::blend stayed a std::string");

    Document asNormal = oneLayerDoc(kDefaultBlendName);
    const DecodedImage normalFlat = flattenDocumentToLinear(asNormal);
    check(unknownFlat.pixels.size() == normalFlat.pixels.size() &&
              std::memcmp(unknownFlat.pixels.data(), normalFlat.pixels.data(),
                          unknownFlat.pixels.size() * sizeof(float)) == 0,
          "unknown: and the composite is byte-identical to `over`, so the warning describes "
          "what actually happened");

    Document mixed = oneLayerDoc("mix");
    std::vector<std::string> mixWarnings;
    const DecodedImage mixFlat = flattenDocumentToLinear(mixed, &mixWarnings);
    check(mixWarnings.size() == 1 && contains(mixWarnings[0], "\"mix\"") &&
              contains(mixWarnings[0], "latent") && contains(mixWarnings[0], "step 3"),
          "mix: a `mix` blend on an RGB layer warns with its OWN sentence -- it is a name "
          "this build knows and cannot apply, with a named unblocking condition, which is a "
          "different fact from an unknown name");
    check(std::memcmp(mixFlat.pixels.data(), normalFlat.pixels.data(),
                      mixFlat.pixels.size() * sizeof(float)) == 0,
          "mix: and it too is composited as `over`, never refused -- refusing would make a "
          "preserved np:blend the thing that stops the document being saved");

    // The export boundary carries both out, on success and on refusal alike.
    const ExportResult png = exportDocument(mixed, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
    check(png.ok && png.warnings.size() == 1,
          "mix: exportDocument() carries the warning out with the bytes");
  }

  // --- PRD L5: `Mix` only between two Pigment layers ---------------------
  {
    auto layerOfKind = [](LayerKind kind) {
      Layer l;
      l.kind = kind;
      if (kind == LayerKind::RGB) l.rgbTiles.emplace();
      return l;
    };
    auto menuHas = [](const std::vector<BlendMode>& menu, BlendMode m) {
      return std::find(menu.begin(), menu.end(), m) != menu.end();
    };

    Document rgbOnly = Document::createBlank(4, 4, WorkingSpace{});
    rgbOnly.layers.push_back(layerOfKind(LayerKind::RGB));
    check(blendMenuForLayer(rgbOnly, 1).size() == 6 &&
              !menuHas(blendMenuForLayer(rgbOnly, 1), BlendMode::Mix),
          "L5: an RGB layer over an RGB layer is offered the six linear modes and NOT `Mix`");

    Document pigments = Document::createBlank(4, 4, WorkingSpace{});
    pigments.layers.clear();
    pigments.layers.push_back(layerOfKind(LayerKind::Pigment));
    pigments.layers.push_back(layerOfKind(LayerKind::Pigment));
    check(menuHas(blendMenuForLayer(pigments, 1), BlendMode::Mix) &&
              blendMenuForLayer(pigments, 1).size() == 7,
          "L5: a Pigment layer sitting on another Pigment layer IS offered `Mix`");
    check(!menuHas(blendMenuForLayer(pigments, 0), BlendMode::Mix),
          "L5: but the BOTTOM Pigment layer is not -- there is nothing beneath it to mix "
          "with, and `layers` is bottom-to-top so `beneath` is index - 1");

    Document mixedKinds = Document::createBlank(4, 4, WorkingSpace{});  // layers[0] is RGB
    mixedKinds.layers.push_back(layerOfKind(LayerKind::Pigment));
    check(!menuHas(blendMenuForLayer(mixedKinds, 1), BlendMode::Mix),
          "L5: a Pigment layer over an RGB layer is not offered `Mix` -- BOTH layers must be "
          "Pigment (docs/ui.md 3.4), because there is no latent under it to mix into");
    check(blendMenuForLayer(mixedKinds, 9).empty(),
          "L5: an out-of-range row offers nothing at all, rather than the whole set");

    // The model refuses what the menu does not offer, through the same
    // predicate -- so L5 is not merely a thing the UI declines to draw.
    const LayerOpResult refusedMix = setLayerBlend(mixedKinds, 1, BlendMode::Mix);
    check(!refusedMix.ok && contains(refusedMix.error, "L5") &&
              contains(refusedMix.error, "Pigment") &&
              mixedKinds.layers[1].blend == kDefaultBlendName,
          "L5: setLayerBlend() refuses `Mix` there by name, citing the requirement, and the "
          "layer really is unchanged -- the dropdown is not the only thing enforcing it");
    const LayerOpResult allowedMix = setLayerBlend(pigments, 1, BlendMode::Mix);
    check(allowedMix.ok && pigments.layers[1].blend == "mix" &&
              contains(allowedMix.editLabel, "mix"),
          "L5: and allows it where the pair holds, writing the canonical name and reporting "
          "an edit label naming the mode");

    // A reorder can take a layer out of L5's reach while it still carries the
    // value. The menu stops offering it and the selection reports "not in this
    // menu" rather than silently reading as the first entry.
    check(moveLayer(pigments, 1, 0).ok, "L5: the `mix` layer is moved to the bottom");
    const std::vector<BlendMode> afterMove = blendMenuForLayer(pigments, 0);
    check(!menuHas(afterMove, BlendMode::Mix) && pigments.layers[0].blend == "mix",
          "L5: after the move the menu no longer offers `Mix` while the layer still CARRIES "
          "it -- the value is preserved, not coerced");
    check(blendMenuSelection(pigments, 0, afterMove) == afterMove.size(),
          "L5: and the dropdown reports `not in this menu` rather than defaulting to entry "
          "0, which would be a silent lie about what the layer says");
  }

  // --- core/LayerOps' setter ---------------------------------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    const LayerOpResult set = setLayerBlend(doc, 0, BlendMode::Screen);
    check(set.ok && doc.layers[0].blend == "screen" && set.index == 0 &&
              contains(set.editLabel, "screen"),
          "setter: setLayerBlend() writes the canonical np:blend name and reports the edit "
          "label a caller should record");
    check(blendMenuSelection(doc, 0, blendMenuForLayer(doc, 0)) == 3,
          "setter: and the dropdown then selects `screen`, which is entry 3 of the menu");

    doc.layers[0].locked = true;
    const LayerOpResult refused = setLayerBlend(doc, 0, BlendMode::Multiply);
    check(!refused.ok && contains(refused.error, "locked") && doc.layers[0].blend == "screen",
          "setter: a LOCKED layer refuses it by name and really is unchanged -- which blend "
          "a layer uses is part of how it looks, so a lock that froze content but not "
          "blending would be a lock in name only");
    const LayerOpResult oob = setLayerBlend(doc, 7, BlendMode::Multiply);
    check(!oob.ok && contains(oob.error, "index"),
          "setter: an out-of-range index is refused with the same sentence every other "
          "core/LayerOps operation uses");
  }

  // --- `Mix` itself: the KM latent lerp, against real Mixbox data --------
  //
  // PRD C3 is P0 and this is the half of it that can land honestly today.
  // `core::Layer` has no latent storage -- `rgbTiles` is 4-channel rgba16float
  // and Pigment/Media need a 7-channel tile that PLAN.md Phase 5 step 3 owns
  // -- so there are no layer latents to lerp. Synthesising them from RGB was
  // rejected: `rgbToLatent()`'s decomposition is "plausible rather than true"
  // (docs/ui.md 3.3) and a `Mix` built on it would be confident, wrong colour
  // that looked like the feature working.
  //
  // What is real today is the latent representation itself, so `mixLatents()`
  // is tested against it: the actual Mixbox LUT, the actual palette pigments,
  // and PLAN.md's own Phase 5 verify sentence -- "blue ... over yellow gives
  // green under `Mix`".
  {
    MixboxLut lut;
    const bool lutLoaded = lut.load(NP_MIXBOX_LUT);
    check(lutLoaded,
          "mix: the real Mixbox LUT loads -- this section asserts against measured pigment "
          "data, not against a stand-in");

    // Endpoints and the implied fourth weight, which need no LUT and no
    // tolerance at all.
    const Latent a{{0.25f, 0.5f, 0.125f}, {0.0625f, -0.125f, 0.375f}};
    const Latent b{{0.75f, 0.125f, 0.0f}, {-0.5f, 0.25f, 0.125f}};
    const Latent at0 = mixLatents(a, b, 0.0f);
    const Latent at1 = mixLatents(a, b, 1.0f);
    check(at0.c == a.c && at0.res == a.res && at1.c == b.c && at1.res == b.res,
          "mix: t = 0 returns the first latent and t = 1 the second, EXACTLY -- std::lerp's "
          "endpoint guarantee, so a Pigment layer at 0% or 100% cannot drift");

    const Latent half = mixLatents(a, b, 0.5f);
    const float impliedA = 1.0f - (a.c[0] + a.c[1] + a.c[2]);
    const float impliedB = 1.0f - (b.c[0] + b.c[1] + b.c[2]);
    const float impliedHalf = 1.0f - (half.c[0] + half.c[1] + half.c[2]);
    check(impliedHalf == 0.5f * (impliedA + impliedB),
          "mix: the IMPLIED fourth pigment weight lerps by the same t -- so lerping this "
          "codebase's six floats is exactly Mixbox's seven, not an approximation of them");

    if (lutLoaded) {
      // Cadmium Yellow and Cobalt Blue, straight from the shipped palette:
      // the two pigments runSelfTest()'s own "blue crossing yellow gives
      // green" case uses, so this section and that one cannot disagree about
      // what the model says.
      const Pigment& yellow = defaultPalette()[0];
      const Pigment& blue = defaultPalette()[7];
      const Latent zy = lut.rgbToLatent(yellow.rgb[0], yellow.rgb[1], yellow.rgb[2]);
      const Latent zb = lut.rgbToLatent(blue.rgb[0], blue.rgb[1], blue.rgb[2]);
      const std::array<float, 3> km = lut.latentToRgb(mixLatents(zy, zb, 0.5f));
      const std::array<float, 3> naive = {0.5f * (yellow.rgb[0] + blue.rgb[0]),
                                          0.5f * (yellow.rgb[1] + blue.rgb[1]),
                                          0.5f * (yellow.rgb[2] + blue.rgb[2])};
      std::printf("  [measured] mix(yellow, blue, 0.5): KM (%.3f, %.3f, %.3f) vs. naive RGB "
                  "lerp (%.3f, %.3f, %.3f)\n",
                  static_cast<double>(km[0]), static_cast<double>(km[1]),
                  static_cast<double>(km[2]), static_cast<double>(naive[0]),
                  static_cast<double>(naive[1]), static_cast<double>(naive[2]));
      check(km[1] > km[0] && km[1] > km[2],
            "mix: yellow mixed with blue gives GREEN -- green is the largest channel, which "
            "is PLAN.md's own Phase 5 verify sentence and the reason the Mixbox licence is "
            "being accepted");
      // The separation threshold, from the measurement printed above rather
      // than picked: the naive lerp's red is 0.498 (it cannot be otherwise --
      // it is the arithmetic mean of 0.996 and 0.0), and the KM mix's is
      // 0.189. Half the naive value, 0.249, sits between them with 32%
      // headroom on the measured figure, and is the natural statement of the
      // qualitative claim ("KM keeps the mix saturated where the RGB lerp
      // washes it out") rather than a number tuned to the result. This is a
      // separation, not a tolerance: it must not tighten onto 0.189.
      check(km[0] < naive[0] * 0.5f,
            "mix: and it is not the RGB lerp: the KM mix's red is under half the naive "
            "lerp's, which is the desaturated-grey failure the whole model exists to avoid");

      // The endpoints of that mix, projected back. This is what makes the
      // *middle* of it meaningful: a mix between two colours the model cannot
      // reproduce would be a mix of something else.
      //
      // It is exact for a structural reason worth stating, because the obvious
      // derivation (8-bit LUT quantisation, amplified by evalPolynomial()'s
      // largest coefficient) would bound the wrong thing entirely and land
      // three orders of magnitude too loose. `rgbToLatent()` *defines* the
      // residual as `rgb - evalPolynomial(c)`, and `latentToRgb()` returns
      // `evalPolynomial(c) + res` from the same `c` -- so every bit of LUT
      // error, quantisation included, is absorbed into the residual by
      // construction and the round trip is `p + (r - p)`.
      //
      // Tolerance, derived from that expression rather than from the LUT: two
      // correctly-rounded float operations on values of magnitude <= 1, so at
      // most 2 ulps of 1.0 = 2^-23 = 1.19e-7. Bounded at 5.0e-7, 4.2x the
      // derived bound. The measurement is printed, and it is 0.
      const std::array<float, 3> backY = lut.latentToRgb(zy);
      const std::array<float, 3> backB = lut.latentToRgb(zb);
      float worst = 0.0f;
      for (int i = 0; i < 3; ++i) {
        worst = std::fmax(worst, std::fabs(backY[i] - yellow.rgb[i]));
        worst = std::fmax(worst, std::fabs(backB[i] - blue.rgb[i]));
      }
      std::printf("  [measured] rgb -> latent -> rgb on the two palette pigments: max residual "
                  "= %.3e (bound 5.000e-07)\n",
                  static_cast<double>(worst));
      check(worst <= 5.0e-7f,
            "mix: latent -> rgb reproduces the pigments it came from -- the residual channel "
            "absorbs the LUT's error by construction, so `Mix` at t = 0 and t = 1 gives back "
            "exactly the pigments being mixed");
    }
  }

  std::printf("[selftest] blend modes %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool runLayerStackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // Most of this section asserts at **exactly zero tolerance**, and that is
  // the interesting part rather than a shortcut. Every fixture value below is
  // chosen to be exactly representable in binary16 (halves and quarters), so
  // the whole premultiplied chain -- store as half, read back as float, scale
  // by an exact opacity, `src + dst * (1 - src.a)` -- is exact: each operand
  // is a small dyadic rational and every product and sum lands back on the
  // float grid with no rounding at all. The hidden-layer, non-overlap and
  // opacity-composition claims are therefore bit-exact comparisons, which is
  // the only strength at which "contributes exactly nothing" means anything.
  //
  // The one lossy stage is `flattenDocumentToLinear()`'s final un-premultiply,
  // a single float division by the composited alpha. IEEE-754 requires it to
  // be correctly rounded, so its error is at most half an ulp: for a result in
  // [0.25, 1) that is 2^-25 = 2.98e-8 absolute. Landed 1.0e-7 -- 3.4x the
  // derived bound, the same "a small multiple of the bound, never of the
  // measurement" discipline runExportTest()'s kRoundTripTol16 and
  // runNpaintFormatTest()'s kCompositeTol both follow.
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  // Same fixture helper every other section uses: a document holds
  // *premultiplied* halves, so a fixture writes straight values through the
  // same `rgb *= a` io/ImageIO.cpp performs on import.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto addRgbLayer = [](Document& doc, std::string name) {
    Layer l;
    l.kind = LayerKind::RGB;
    l.rgbTiles.emplace();
    l.name = std::move(name);
    doc.layers.push_back(std::move(l));
  };

  // --- `over` against a hand-computed reference --------------------------
  //
  // Two translucent layers over the same pixel, both at alpha 0.5, both fully
  // saturated in one channel. The arithmetic, in full, in premultiplied
  // linear light:
  //
  //   bottom straight (1, 0, 0, 0.5) -> premultiplied (0.5, 0, 0,   0.5)
  //   top    straight (0, 0, 1, 0.5) -> premultiplied (0,   0, 0.5, 0.5)
  //
  //   acc  = bottom over transparent black
  //        = (0.5, 0, 0, 0.5) + (0,0,0,0) * (1 - 0.5)
  //        = (0.5, 0, 0, 0.5)
  //   out  = top over acc
  //        = (0, 0, 0.5, 0.5) + (0.5, 0, 0, 0.5) * (1 - 0.5)
  //        = (0 + 0.25, 0, 0.5 + 0, 0.5 + 0.25)
  //        = (0.25, 0, 0.5, 0.75)
  //
  //   straight = out.rgb / out.a = (0.25/0.75, 0, 0.5/0.75, 0.75)
  //            = (1/3, 0, 2/3, 0.75)
  //
  // Every intermediate is a dyadic rational and exact in both half and float;
  // only the two divisions at the end round. Note what the answer is *not*: a
  // plain sum would give premultiplied (0.5, 0, 0.5, 1.0), i.e. straight
  // (0.5, 0, 0.5, 1.0) -- a fully opaque even purple. The two answers differ
  // in every channel, so this fixture cannot pass against the code it
  // replaced.
  {
    Document doc = Document::createBlank(1, 1, WorkingSpace{});
    doc.layers[0].name = "bottom";
    addRgbLayer(doc, "top");
    writeStraight(doc, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(doc, 1, 0, 0, 0.0f, 0.0f, 1.0f, 0.5f);

    const DecodedImage flat = flattenDocumentToLinear(doc);
    const auto got = pixelOf(flat, 0, 0);
    check(near(got[0], 1.0f / 3.0f, kUnpremultiplyTol) && near(got[1], 0.0f, 0.0f) &&
              near(got[2], 2.0f / 3.0f, kUnpremultiplyTol) &&
              near(got[3], 0.75f, 0.0f),
          "over: two 50%-alpha layers composite to the hand-computed (1/3, 0, 2/3, 0.75)");
    check(!near(got[3], 1.0f, 1e-3f),
          "over: and NOT to the plain sum's fully opaque alpha 1.0 -- the fixture would pass "
          "against the summing flattener if it did");

    // The primitive on its own, at the same numbers, so a failure says whether
    // the arithmetic or the document walk is wrong.
    const std::array<float, 4> prim =
        compositeOver({0.0f, 0.0f, 0.5f, 0.5f}, {0.5f, 0.0f, 0.0f, 0.5f});
    check(prim[0] == 0.25f && prim[1] == 0.0f && prim[2] == 0.5f && prim[3] == 0.75f,
          "over: compositeOver() alone gives premultiplied (0.25, 0, 0.5, 0.75), exactly");

    // Reorder, and the result must flip in the direction the ordering
    // convention predicts: `layers` is bottom-to-top, so moving index 0 to
    // index 1 puts red on top, and red must now dominate.
    const LayerOpResult moved = moveLayer(doc, 0, 1);
    check(moved.ok, "reorder: moveLayer(0 -> 1) succeeds");
    const DecodedImage swapped = flattenDocumentToLinear(doc);
    const auto after = pixelOf(swapped, 0, 0);
    check(near(after[0], 2.0f / 3.0f, kUnpremultiplyTol) &&
              near(after[2], 1.0f / 3.0f, kUnpremultiplyTol),
          "reorder: swapping the two layers swaps which colour dominates -- red 2/3 blue 1/3, "
          "the mirror of before, because layers[] is bottom-to-top");
    check(near(after[3], 0.75f, 0.0f),
          "reorder: and alpha is unchanged at 0.75 -- `over` is order-dependent in colour, "
          "not in coverage");
  }

  // --- Opacity is a multiplier, and provably not the same thing as alpha --
  {
    // Both fixtures use only exactly-representable values, so these are
    // zero-tolerance comparisons.
    Document byOpacity = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(byOpacity, 0, 0, 0, 0.5f, 0.25f, 0.75f, 1.0f);
    byOpacity.layers[0].opacity = 0.5f;

    Document byAlpha = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(byAlpha, 0, 0, 0, 0.5f, 0.25f, 0.75f, 0.5f);

    const auto o = pixelOf(flattenDocumentToLinear(byOpacity), 0, 0);
    const auto a = pixelOf(flattenDocumentToLinear(byAlpha), 0, 0);
    check(o[0] == a[0] && o[1] == a[1] && o[2] == a[2] && o[3] == a[3],
          "opacity: a fully opaque layer at 50% opacity composites bit-identically to the "
          "same colour stored at alpha 0.5");
    check(o[3] == 0.5f, "opacity: and the composited alpha is exactly 0.5");

    // ...and yet it is not alpha, because the two compose rather than one
    // overriding the other.
    Document both = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(both, 0, 0, 0, 0.5f, 0.25f, 0.75f, 0.5f);
    both.layers[0].opacity = 0.5f;
    const auto b = pixelOf(flattenDocumentToLinear(both), 0, 0);
    check(b[3] == 0.25f,
          "opacity: alpha 0.5 at 50% opacity composites to alpha exactly 0.25 -- the two "
          "multiply, so opacity is a coverage scale and not an alpha replacement");
    check(b[0] == 0.5f && b[1] == 0.25f && b[2] == 0.75f,
          "opacity: and the straight colour is untouched by it -- scaling a premultiplied "
          "texel scales coverage, not hue");

    // Nothing was mutated to achieve any of that.
    const Tile* t = both.layers[0].rgbTiles->find(TileCoord{0, 0});
    const std::array<float, 4> stored =
        t ? t->readPixel(PixelCoord{0, 0}) : std::array<float, 4>{0, 0, 0, 0};
    check(stored[3] == 0.5f && both.layers[0].opacity == 0.5f,
          "opacity: the stored texel alpha and Layer::opacity are both unchanged after "
          "compositing -- the multiplier is applied to a copy, never baked into the tile");

    // The scalar itself, including the clamp and the NaN guard, since
    // Layer::opacity is a public member of a plain aggregate.
    Layer probe;
    probe.opacity = 0.25f;
    check(layerCoverage(probe) == 0.25f, "opacity: layerCoverage() passes an in-range value "
                                          "through unchanged");
    probe.opacity = 1.5f;
    check(layerCoverage(probe) == 1.0f, "opacity: layerCoverage() clamps above 1 rather than "
                                         "letting `1 - a` go negative");
    probe.opacity = -0.5f;
    check(layerCoverage(probe) == 0.0f, "opacity: layerCoverage() clamps below 0");
    probe.opacity = std::nanf("");
    check(layerCoverage(probe) == 0.0f,
          "opacity: a NaN opacity yields 0 rather than propagating across the canvas");
  }

  // --- A hidden layer contributes EXACTLY nothing (zero tolerance) -------
  {
    // The comparison is against the same document with the layer *deleted*,
    // not against a hand-written expectation: "hidden" has to mean "as if it
    // were not there", and only removing it actually proves that.
    auto build = [&]() {
      Document doc = Document::createBlank(200, 200, WorkingSpace{});
      doc.layers[0].name = "keep";
      writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.75f, 1.0f);
      writeStraight(doc, 0, 150, 150, 0.25f, 0.5f, 0.5f, 0.5f);
      addRgbLayer(doc, "hide me");
      writeStraight(doc, 1, 3, 3, 0.75f, 0.5f, 0.25f, 1.0f);   // exactly overlapping
      writeStraight(doc, 1, 4, 3, 1.0f, 1.0f, 1.0f, 1.0f);     // and one of its own
      writeStraight(doc, 1, 150, 150, 0.5f, 0.5f, 0.5f, 0.5f);
      return doc;
    };

    Document hidden = build();
    hidden.layers[1].visible = false;
    Document deleted = build();
    deleted.layers.pop_back();

    const DecodedImage h = flattenDocumentToLinear(hidden);
    const DecodedImage d = flattenDocumentToLinear(deleted);
    check(h.pixels.size() == d.pixels.size() && !h.pixels.empty() &&
              std::memcmp(h.pixels.data(), d.pixels.data(),
                          h.pixels.size() * sizeof(float)) == 0,
          "hidden: a hidden layer's composite is BYTE-identical to the same document with "
          "that layer deleted -- zero tolerance, over 200x200x4 floats");

    // Zero opacity is the same claim by the other route.
    Document zeroOpacity = build();
    zeroOpacity.layers[1].opacity = 0.0f;
    const DecodedImage z = flattenDocumentToLinear(zeroOpacity);
    check(z.pixels.size() == d.pixels.size() &&
              std::memcmp(z.pixels.data(), d.pixels.data(),
                          z.pixels.size() * sizeof(float)) == 0,
          "hidden: a layer at 0.0 opacity is byte-identical to the same deletion too");

    // The negative control: the same fixture *visible* must differ, or the two
    // assertions above would pass against a flattener that ignored layer 1
    // entirely.
    const DecodedImage shown = flattenDocumentToLinear(build());
    check(shown.pixels.size() == d.pixels.size() &&
              std::memcmp(shown.pixels.data(), d.pixels.data(),
                          shown.pixels.size() * sizeof(float)) != 0,
          "hidden: and the same layer *visible* genuinely changes the composite -- the two "
          "checks above cannot pass vacuously");
  }

  // --- The regression property: non-overlapping documents are unchanged --
  //
  // PLAN.md's standard verification is "the selftest output changes by
  // additions only", and this step legitimately breaks that for documents
  // whose layers overlap. What must NOT change is everything else, and this
  // block asserts the boundary rather than leaving it to a diff: against a
  // second, independent implementation of the plain sum this step replaced --
  // written here rather than called, exactly like the recovery journal
  // section's own second FNV-1a -- a single-layer document and a multi-layer
  // document with no two layers covering the same pixel must produce
  // bit-identical output.
  {
    auto plainSumFlatten = [](const Document& doc) {
      const size_t w = static_cast<size_t>(doc.width);
      const size_t h = static_cast<size_t>(doc.height);
      std::vector<float> premul(w * h * 4, 0.0f);
      for (const Layer& layer : doc.layers) {
        if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;
        for (const auto& [coord, tile] : *layer.rgbTiles) {
          const PixelCoord origin = tileOrigin(coord);
          for (int32_t ty = 0; ty < kTileSize; ++ty) {
            const int32_t dy = origin.y + ty;
            if (dy < 0 || dy >= doc.height) continue;
            for (int32_t tx = 0; tx < kTileSize; ++tx) {
              const int32_t dx = origin.x + tx;
              if (dx < 0 || dx >= doc.width) continue;
              const std::array<float, 4> px = tile.readPixel(PixelCoord{tx, ty});
              float* dst = &premul[(static_cast<size_t>(dy) * w + static_cast<size_t>(dx)) * 4];
              dst[0] += px[0];
              dst[1] += px[1];
              dst[2] += px[2];
              dst[3] += px[3];
            }
          }
        }
      }
      std::vector<float> straight(premul.size(), 0.0f);
      for (size_t i = 0; i < premul.size(); i += 4) {
        const float a = premul[i + 3];
        if (a <= 0.0f) continue;
        straight[i + 0] = premul[i + 0] / a;
        straight[i + 1] = premul[i + 1] / a;
        straight[i + 2] = premul[i + 2] / a;
        straight[i + 3] = a;
      }
      return straight;
    };
    auto identical = [](const std::vector<float>& a, const DecodedImage& b) {
      return a.size() == b.pixels.size() && !a.empty() &&
             std::memcmp(a.data(), b.pixels.data(), a.size() * sizeof(float)) == 0;
    };

    // Deliberately awkward: translucent texels (so the un-premultiply really
    // runs), several tiles, content off the canvas edge, and values that are
    // NOT exactly representable in half, so a difference of one ulp anywhere
    // would show up.
    Document single = Document::createBlank(300, 200, WorkingSpace{});
    writeStraight(single, 0, 0, 0, 0.1f, 0.2f, 0.3f, 1.0f);
    writeStraight(single, 0, 7, 9, 0.8f, 0.4f, 0.2f, 0.3f);
    writeStraight(single, 0, 199, 150, 0.37f, 0.61f, 0.94f, 0.77f);
    writeStraight(single, 0, 290, 195, 1.0f, 1.0f, 1.0f, 1.0f);
    writeStraight(single, 0, 400, 400, 0.5f, 0.5f, 0.5f, 1.0f);  // off canvas
    check(identical(plainSumFlatten(single), flattenDocumentToLinear(single)),
          "regression: a SINGLE-layer document composites bit-identically to the plain sum "
          "this step replaced -- every float, including the translucent ones");

    Document disjoint = single;
    addRgbLayer(disjoint, "second");
    addRgbLayer(disjoint, "third");
    writeStraight(disjoint, 1, 1, 0, 0.55f, 0.05f, 0.95f, 0.42f);
    writeStraight(disjoint, 1, 8, 9, 0.13f, 0.79f, 0.31f, 1.0f);
    writeStraight(disjoint, 1, 250, 180, 0.9f, 0.1f, 0.6f, 0.66f);
    writeStraight(disjoint, 2, 2, 0, 0.22f, 0.44f, 0.88f, 0.9f);
    writeStraight(disjoint, 2, 128, 128, 0.71f, 0.29f, 0.07f, 1.0f);
    check(identical(plainSumFlatten(disjoint), flattenDocumentToLinear(disjoint)),
          "regression: and so does a THREE-layer document whose layers never cover the same "
          "pixel -- `over` reduces to the sum exactly when nothing overlaps");

    // The negative control again: one overlapping texel and the two must part
    // company, or the two checks above would be asserting that the compositor
    // is the summer.
    Document overlapping = disjoint;
    writeStraight(overlapping, 1, 0, 0, 0.6f, 0.6f, 0.6f, 0.5f);  // onto layer 0's (0,0)
    check(!identical(plainSumFlatten(overlapping), flattenDocumentToLinear(overlapping)),
          "regression: one overlapping texel is enough to make the two disagree -- the "
          "identity above is a property of non-overlap, not of the implementation");
  }

  // --- Layer operations, and what each does to the dirty state -----------
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "ops");
    check(!od.isDirty() && od.revision == 0 && od.structuralRevision == 0,
          "ops: a blank document starts clean, at revision 0");

    const DocumentOpResult added =
        recordLayerEdit(od, addLayer(od.document, 1, makeRgbLayer("Layer 2")));
    check(added.ok && od.document.layers.size() == 2 && od.document.layers[1].name == "Layer 2",
          "ops: addLayer inserts at the requested index");
    check(od.isDirty() && od.revision == 1 && od.structuralRevision == 1,
          "ops: and bumps BOTH revision and structuralRevision by exactly one -- a layer "
          "change is structural, so PRD O5's journal writes at once rather than on the timer");
    check(od.unsavedEdits.size() == 1 && contains(od.unsavedEdits[0], "Layer 2"),
          "ops: the recorded edit label names the layer, for PRD I11's refusal and the "
          "future History panel");

    const DocumentOpResult dup = recordLayerEdit(od, duplicateLayer(od.document, 0));
    check(dup.ok && od.document.layers.size() == 3,
          "ops: duplicateLayer inserts the copy directly above its source");
    check(od.revision == 2 && od.structuralRevision == 2, "ops: and is structural too");

    const size_t before = od.document.layers.size();
    const DocumentOpResult moved = recordLayerEdit(od, moveLayer(od.document, 0, 2));
    check(moved.ok && od.document.layers.size() == before,
          "ops: moveLayer keeps the layer count");
    check(od.revision == 3 && od.structuralRevision == 3, "ops: and is structural too");

    const DocumentOpResult removed = recordLayerEdit(od, removeLayer(od.document, 2));
    check(removed.ok && od.document.layers.size() == before - 1, "ops: removeLayer removes one");
    check(od.revision == 4 && od.structuralRevision == 4, "ops: and is structural too");

    const DocumentOpResult vis = recordLayerEdit(od, setLayerVisible(od.document, 0, false));
    check(vis.ok && !od.document.layers[0].visible && od.revision == 5 &&
              od.structuralRevision == 5,
          "ops: a property change (visibility) is structural as well -- it changes what the "
          "composite is, and the journal must not learn about it a minute later");

    // A refused operation must leave the record completely alone.
    const uint64_t rev = od.revision;
    const size_t edits = od.unsavedEdits.size();
    const DocumentOpResult bad = recordLayerEdit(od, removeLayer(od.document, 99));
    check(!bad.ok && contains(bad.error, "index 99"),
          "ops: an out-of-range index is refused by name, naming the index");
    check(od.revision == rev && od.structuralRevision == rev &&
              od.unsavedEdits.size() == edits,
          "ops: and a refused operation records NOTHING -- no revision bump, no label. A "
          "document is not dirty because someone tried something that did not happen");

    const DocumentOpResult badOpacity =
        recordLayerEdit(od, setLayerOpacity(od.document, 0, 1.5f));
    check(!badOpacity.ok && contains(badOpacity.error, "1.5") &&
              contains(badOpacity.error, "[0, 1]"),
          "ops: an out-of-range opacity is refused by name rather than clamped -- clamping "
          "would only move io/NpaintFile's own refusal to the next save");

    // Removing the last layer is allowed (PRD C16: no privileged background).
    Document lastOne = Document::createBlank(8, 8, WorkingSpace{});
    check(removeLayer(lastOne, 0).ok && lastOne.layers.empty(),
          "ops: removing the only layer is allowed -- PRD C16 rules out a special locked "
          "Background, and a zero-layer document is representable");

    // Duplication is a real deep copy, the same claim duplicateDocument makes.
    Document deep = Document::createBlank(64, 64, WorkingSpace{});
    writeStraight(deep, 0, 1, 1, 0.5f, 0.5f, 0.5f, 1.0f);
    check(duplicateLayer(deep, 0).ok && deep.layers.size() == 2,
          "ops: duplicateLayer succeeds on a layer with tiles");
    writeStraight(deep, 1, 1, 1, 0.25f, 0.25f, 0.25f, 1.0f);
    const Tile* src = deep.layers[0].rgbTiles->find(TileCoord{0, 0});
    check(src != nullptr && src->readPixel(PixelCoord{1, 1})[0] == 0.5f,
          "ops: painting into the copy does not reach the source -- the TileStore really was "
          "deep-copied, at 128 KiB per tile (COW is Phase 5 step 6)");

    check(defaultNewLayerName(Document::createBlank(8, 8, WorkingSpace{})) == "Layer 1",
          "ops: defaultNewLayerName() starts at \"Layer 1\"");
    Document named = Document::createBlank(8, 8, WorkingSpace{});
    named.layers[0].name = "Layer 7";
    addRgbLayer(named, "Layer 3");
    check(defaultNewLayerName(named) == "Layer 8",
          "ops: and goes one above the highest existing \"Layer N\", not one above the count "
          "-- so deleting from the middle cannot hand out a name already on screen");
  }

  // --- `locked`, refusing exactly what it should, by name ----------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    doc.layers[0].name = "Line pass";
    addRgbLayer(doc, "Free");
    doc.layers[0].locked = true;

    const LayerOpResult rm = removeLayer(doc, 0);
    check(!rm.ok && contains(rm.error, "locked") && contains(rm.error, "Line pass") &&
              doc.layers.size() == 2,
          "locked: removeLayer is refused, naming the layer and the lock");
    const LayerOpResult mv = moveLayer(doc, 0, 1);
    check(!mv.ok && contains(mv.error, "locked") && doc.layers[0].name == "Line pass",
          "locked: moving the locked layer itself is refused");
    const LayerOpResult op = setLayerOpacity(doc, 0, 0.5f);
    check(!op.ok && contains(op.error, "locked") && doc.layers[0].opacity == 1.0f,
          "locked: setLayerOpacity is refused, and the value really is unchanged");
    const LayerOpResult rn = setLayerName(doc, 0, "nope");
    check(!rn.ok && contains(rn.error, "locked") && doc.layers[0].name == "Line pass",
          "locked: setLayerName is refused");

    const LayerOpResult hide = setLayerVisible(doc, 0, false);
    check(hide.ok && !doc.layers[0].visible,
          "locked: hiding a locked layer is ALLOWED -- it changes nothing about the layer, "
          "and every editor with a lock agrees the eye icon stays live");
    check(setLayerVisible(doc, 0, true).ok, "locked: and showing it again is allowed");

    const LayerOpResult dupLocked = duplicateLayer(doc, 0);
    check(dupLocked.ok && doc.layers.size() == 3 && doc.layers[1].locked,
          "locked: duplicating a locked layer is allowed (it reads the source) and the copy "
          "inherits the lock -- duplication is not a one-step way to launder it off");

    // Moving a *different* layer past a locked one is allowed: locking one
    // layer must not freeze the whole stack.
    const size_t topIndex = doc.layers.size() - 1;
    check(!doc.layers[topIndex].locked && moveLayer(doc, topIndex, 0).ok &&
              doc.layers[0].name == "Free",
          "locked: an unlocked layer may still be moved past a locked one -- a lock freezes "
          "one layer, not the document");

    // And the lock itself always comes off.
    const size_t lockedNow = 1;
    check(doc.layers[lockedNow].locked && setLayerLocked(doc, lockedNow, false).ok &&
              !doc.layers[lockedNow].locked,
          "locked: setLayerLocked(false) is allowed on a locked layer -- a lock that cannot "
          "be removed is a bug, not a lock");
    check(removeLayer(doc, lockedNow).ok,
          "locked: and once unlocked, the operation that was refused succeeds");
  }

  // --- An unimplemented blend: composited as `over`, never silently ------
  {
    // **Changed by PLAN.md Phase 5 step 2**, which is the step that made the
    // old wording false: these two used to read "\"normal\" is the one blend
    // this build implements" and "everything else ... is reported
    // unimplemented", and core/Blend has since landed five more. What this
    // section is actually for survives unchanged -- a blend this build cannot
    // composite is composited as `over` and reported, never silently -- so the
    // fixture below moved to a name that is still outside the set. The full
    // enumeration is asserted in runBlendTest(); these two only pin the
    // boundary this section's fixtures sit on.
    check(blendIsImplemented(kDefaultBlendName) && blendIsImplemented("multiply"),
          "blend: \"normal\" and the rest of core/Blend's linear set are implemented");
    check(!blendIsImplemented("linear-burn") && !blendIsImplemented("mix") &&
              !blendIsImplemented(""),
          "blend: a newer build's name, `mix` (no latents to lerp yet) and an empty string "
          "are reported unimplemented rather than assumed to be `over`");

    Document odd = Document::createBlank(1, 1, WorkingSpace{});
    odd.layers[0].name = "Line pass";
    addRgbLayer(odd, "");
    odd.layers[1].blend = "linear-burn";
    writeStraight(odd, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(odd, 1, 0, 0, 0.0f, 0.0f, 1.0f, 0.5f);

    std::vector<std::string> warnings;
    const DecodedImage flat = flattenDocumentToLinear(odd, &warnings);
    check(warnings.size() == 1 && contains(warnings[0], "linear-burn") &&
              contains(warnings[0], "layer 1"),
          "blend: an unimplemented blend produces exactly one warning, naming the layer and "
          "the blend it asked for");
    check(contains(warnings[0], "core/Blend"),
          "blend: and points at where the real implementation is coming from");

    // The pixels really are `over`, not something else: identical to the same
    // document with the blend set to normal.
    Document asNormal = odd;
    asNormal.layers[1].blend = kDefaultBlendName;
    const DecodedImage normalFlat = flattenDocumentToLinear(asNormal);
    check(flat.pixels.size() == normalFlat.pixels.size() &&
              std::memcmp(flat.pixels.data(), normalFlat.pixels.data(),
                          flat.pixels.size() * sizeof(float)) == 0,
          "blend: and the composite is byte-identical to `over` -- the warning describes what "
          "actually happened rather than hinting at something else");
    check(odd.layers[1].blend == "linear-burn",
          "blend: while the value itself is untouched, so PRD I10 still carries it to disk");

    // A hidden layer with an unimplemented blend is still warned about: the
    // document is approximate whether or not this particular layer mattered
    // today.
    Document hiddenOdd = odd;
    hiddenOdd.layers[1].visible = false;
    std::vector<std::string> hiddenWarnings;
    flattenDocumentToLinear(hiddenOdd, &hiddenWarnings);
    check(hiddenWarnings.size() == 1,
          "blend: a HIDDEN layer's unimplemented blend is still reported -- unhiding it must "
          "not be where the user first learns the composite is approximate");

    // The export boundary carries it too, on success and on refusal alike.
    const ExportResult png = exportDocument(odd, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(png.ok && png.warnings.size() == 1 && contains(png.warnings[0], "linear-burn"),
          "blend: exportDocument() carries the warning out with the bytes");
    const ExportResult refused = exportDocument(odd, ImageFormat::Jpeg,
                                                ExportTargetSpace::Rec709Srgb,
                                                ExportBitDepth::UInt8);
    check(!refused.ok && refused.warnings.size() == 1,
          "blend: and carries it out with a REFUSAL too -- fixing the refusal must not be "
          "how the approximation gets discovered");
  }

  // --- The probe agrees with the flattener -------------------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    addRgbLayer(doc, "top");
    writeStraight(doc, 0, 1, 1, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(doc, 1, 1, 1, 0.0f, 0.0f, 1.0f, 0.5f);

    ProbeParams all;
    all.sampleAllLayers = true;
    const ProbeSample sample = probePixel(doc, PixelCoord{1, 1}, all);
    const auto flat = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(near(sample.linear[0], flat[0], kUnpremultiplyTol) &&
              near(sample.linear[2], flat[2], kUnpremultiplyTol) &&
              near(sample.linear[3], flat[3], kUnpremultiplyTol),
          "probe: sampleAllLayers composites through the SAME `over` the flattener uses -- an "
          "eyedropper and an export that disagreed would be a bug nobody could explain");

    doc.layers[1].visible = false;
    const ProbeSample hiddenSample = probePixel(doc, PixelCoord{1, 1}, all);
    check(near(hiddenSample.linear[0], 1.0f, kUnpremultiplyTol) &&
              near(hiddenSample.linear[3], 0.5f, 0.0f),
          "probe: and honours `visible` -- with the blue layer hidden it reads the red one "
          "underneath");

    ProbeParams justTop;
    justTop.sampleAllLayers = false;
    justTop.activeLayerIndex = 1;
    const ProbeSample own = probePixel(doc, PixelCoord{1, 1}, justTop);
    check(near(own.linear[2], 1.0f, kUnpremultiplyTol) && near(own.linear[3], 0.5f, 0.0f),
          "probe: single-layer mode reads a HIDDEN layer's own colour, deliberately ignoring "
          "visible/opacity -- it asks what is on the layer, not what the document shows");
  }

  // --- The panel's one reversal, and what a row says ---------------------
  {
    check(layerIndexForPanelRow(0, 3) == 2 && layerIndexForPanelRow(1, 3) == 1 &&
              layerIndexForPanelRow(2, 3) == 0,
          "panel: row 0 is the TOP of the stack (the last element of layers[]), which is what "
          "every layers panel shows first");
    check(panelRowForLayerIndex(2, 3) == 0 && panelRowForLayerIndex(0, 3) == 2,
          "panel: panelRowForLayerIndex() is its exact inverse");
    check(layerIndexForPanelRow(5, 3) == 0 && layerIndexForPanelRow(0, 0) == 0 &&
              panelRowForLayerIndex(9, 3) == 0,
          "panel: an out-of-range row or index returns 0 rather than wrapping through "
          "unsigned subtraction -- the one arithmetic accident this mapping exists to avoid");

    Layer row;
    row.kind = LayerKind::RGB;
    check(std::string(layerRowSubLine(row)) == "RGB \xC2\xB7 NORMAL \xC2\xB7 100%",
          "panel: the sub-line reads `RGB - NORMAL - 100%`, docs/ui.md's own row format");
    row.opacity = 0.72f;
    row.visible = false;
    row.locked = true;
    check(std::string(layerRowSubLine(row)) ==
              "RGB \xC2\xB7 NORMAL \xC2\xB7 72% \xC2\xB7 HIDDEN \xC2\xB7 LOCKED",
          "panel: hidden and locked are spelled out on the sub-line, so --selftest can read "
          "state the eye and lock glyphs otherwise only show");
    row.blend = "linear-burn";
    check(contains(layerRowSubLine(row), "LINEAR-BURN (!)"),
          "panel: an unrecognised blend shows as itself, marked (!) -- the panel's half of "
          "\"never silently composited as over\"");

    Layer unnamed;
    check(layerRowTitle(unnamed, 0) == "Layer 1" && layerRowTitle(unnamed, 4) == "Layer 5",
          "panel: an unnamed layer gets a positional placeholder title, never a blank row");
    unnamed.name = "Line pass";
    check(layerRowTitle(unnamed, 4) == "Line pass",
          "panel: a named layer shows its own name");
    check(std::string(layerKindGlyph(LayerKind::RGB)) != std::string(layerKindGlyph(
              LayerKind::Pigment)),
          "panel: every kind's glyph is distinct, per docs/ui.md 3.2 (the wireframe's rows "
          "could not tell a Pigment layer from an RGB one, which hid the differentiator)");
  }

  // --- The round trip: order and all six metadata fields -----------------
  {
    const char* kPath = "selftest_layerstack.npaint";
    std::remove(kPath);

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0].name = "bottom";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.25f;
    doc.layers[0].visible = false;
    doc.layers[0].locked = true;
    doc.layers[0].parent = "G0001";
    addRgbLayer(doc, "middle");
    doc.layers[1].opacity = 0.5f;
    // The second locked layer is the middle one, not the top one, and that is
    // forced rather than arbitrary: the top layer is about to be moved, and
    // core/LayerOps refuses to move a locked layer. Two layers still carry
    // `np:locked = 1` and one carries 0, which is all the round trip needs --
    // and the reorder additionally exercises the rule that an unlocked layer
    // may still travel *past* a locked one.
    doc.layers[1].locked = true;
    addRgbLayer(doc, "top");
    doc.layers[2].blend = "screen";
    // One identifiable texel per layer, in a different tile each, so the order
    // is checkable from the pixels and not only from the names.
    writeStraight(doc, 0, 1, 1, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(doc, 1, 130, 1, 0.0f, 1.0f, 0.0f, 1.0f);
    writeStraight(doc, 2, 1, 130, 0.0f, 0.0f, 1.0f, 1.0f);

    // Reorder before saving, so the file records an order that is NOT the one
    // the layers were created in -- a round trip that only ever saw creation
    // order could not catch a reversal.
    check(moveLayer(doc, 2, 0).ok, "round trip: the top layer is moved to the bottom first");
    check(doc.layers[0].name == "top" && doc.layers[1].name == "bottom" &&
              doc.layers[2].name == "middle",
          "round trip: and the in-memory order really is top/bottom/middle before saving");

    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok == kOiioBuild,
          kOiioBuild ? "round trip: the reordered three-layer document saves"
                     : "round trip: saving is refused in the NP_USE_OIIO=OFF build, which has "
                       "no `.npaint` writer at all");
    if (!kOiioBuild) {
      check(contains(saved.error, "NP_USE_OIIO"),
            "round trip: and the refusal is io/NpaintFile's own, naming the build option");
    }
    if (kOiioBuild && saved.ok) {
      const NpaintLoadResult back = loadNpaint(kPath);
      check(back.ok && back.document.layers.size() == 3,
            "round trip: and reads back with three layers");
      if (back.ok && back.document.layers.size() == 3) {
        const Layer& b0 = back.document.layers[0];
        const Layer& b1 = back.document.layers[1];
        const Layer& b2 = back.document.layers[2];
        check(b0.name == "top" && b1.name == "bottom" && b2.name == "middle",
              "round trip: in exactly the saved order, bottom-first -- the part order IS the "
              "layer order (docs/document-format.md)");
        check(b0.blend == "screen" && b1.blend == "multiply" && b2.blend == kDefaultBlendName,
              "round trip: np:blend travels with its own layer, not with its old index");
        check(b0.opacity == 1.0f && b1.opacity == 0.25f && b2.opacity == 0.5f,
              "round trip: np:opacity likewise, at exact float equality");
        check(b0.visible == true && b1.visible == false && b2.visible == true,
              "round trip: np:visible likewise");
        check(b0.locked == false && b1.locked == true && b2.locked == true,
              "round trip: np:locked likewise");
        check(b1.parent == "G0001" && b0.parent.empty() && b2.parent.empty(),
              "round trip: np:parent likewise -- still carried, still never acted on");
        check(b0.kind == LayerKind::RGB && b1.kind == LayerKind::RGB &&
                  b2.kind == LayerKind::RGB,
              "round trip: np:kind likewise");

        // And the pixels moved with the metadata: blue was the top layer and
        // is now the bottom one, so its texel must be in layers[0].
        const Tile* blue = b0.rgbTiles->find(TileCoord{0, 1});
        check(blue != nullptr && blue->readPixel(PixelCoord{1, 2})[2] == 1.0f,
              "round trip: and each layer's TILES came back with it -- the reordered bottom "
              "layer holds the blue texel it had before the save");

        // The composite the file carries must agree with compositing the
        // loaded document, which is only true if the order survived.
        const DecodedImage expect = flattenDocumentToLinear(back.document);
        const DecodedImage direct = flattenDocumentToLinear(doc);
        check(expect.valid() && direct.valid() &&
                  std::memcmp(expect.pixels.data(), direct.pixels.data(),
                              expect.pixels.size() * sizeof(float)) == 0,
              "round trip: the loaded document composites BYTE-identically to the one that "
              "was saved -- order, visibility and opacity all survived together");
      }
    }
    std::remove(kPath);
    std::FILE* left = std::fopen(kPath, "rb");
    check(left == nullptr, "round trip: the scratch file is removed");
    if (left) std::fclose(left);
  }

  std::printf("[selftest] layer stack %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
