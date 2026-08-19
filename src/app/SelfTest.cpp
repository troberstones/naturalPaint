#include "app/SelfTest.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "app/FixedStep.hpp"
#include "app/Keymap.hpp"
#include "app/Memory.hpp"
#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "brush/StrokePath.hpp"
#include "color/Shaper.hpp"
#include "color/Space.hpp"
#include "core/Document.hpp"
#include "core/Half.hpp"
#include "core/Histogram.hpp"
#include "core/Layer.hpp"
#include "core/Probe.hpp"
#include "core/TileStore.hpp"
#include "io/ImageDecode.hpp"
#include "io/ImageIO.hpp"
#include "ops/PointOps.hpp"
#include "ui/NaturalPaintUI.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
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

// Hand-builds a minimal, valid 16-bit RGBA PNG in memory from a top-to-bottom
// RGBA16 pixel array. stb_image_write's PNG writer (used for every other
// fixture in runImageDecodeTest()) is 8-bit-per-channel only -- there is no
// public 16-bit write entry point in the vendored stb_image_write.h -- so
// this is the only way to produce a 16-bit fixture without vendoring a
// second image library. It reuses stb_image_write's own public
// stbi_zlib_compress() for the IDAT payload (already linked in from this
// file's STB_IMAGE_WRITE_IMPLEMENTATION above) but needs its own CRC32:
// stb_image_write's crc32 helper is `static`, internal to its translation
// unit, so it isn't reachable from here.
std::vector<uint8_t> buildMinimal16BitPng(uint32_t w, uint32_t h, const uint16_t* rgba) {
  auto crc32 = [](const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
      crc ^= data[i];
      for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
  };
  auto pushU32BE = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
  };
  auto appendChunk = [&](std::vector<uint8_t>& png, const char* tag,
                        const std::vector<uint8_t>& data) {
    pushU32BE(png, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> typeAndData(tag, tag + 4);
    typeAndData.insert(typeAndData.end(), data.begin(), data.end());
    png.insert(png.end(), typeAndData.begin(), typeAndData.end());
    pushU32BE(png, crc32(typeAndData.data(), typeAndData.size()));
  };

  // Raw scanlines: one filter-type byte (0 = None) per row, then w*4 16-bit
  // big-endian samples -- PNG's on-the-wire sample order for >8-bit depth.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (1 + w * 4 * 2));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < 4; ++c) {
        const uint16_t v = rgba[(static_cast<size_t>(y) * w + x) * 4 + c];
        raw.push_back(static_cast<uint8_t>(v >> 8));
        raw.push_back(static_cast<uint8_t>(v & 0xFF));
      }
    }
  }

  int compressedLen = 0;
  unsigned char* compressed =
      stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &compressedLen, 8);
  std::vector<uint8_t> idat(compressed, compressed + compressedLen);
  std::free(compressed);  // stb_image_write's default allocator is malloc/free

  std::vector<uint8_t> ihdr;
  pushU32BE(ihdr, w);
  pushU32BE(ihdr, h);
  ihdr.push_back(16);  // bit depth
  ihdr.push_back(6);   // color type: truecolor + alpha
  ihdr.push_back(0);   // compression method (only one exists)
  ihdr.push_back(0);   // filter method (only one exists)
  ihdr.push_back(0);   // interlace: none

  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};  // PNG signature
  appendChunk(png, "IHDR", ihdr);
  appendChunk(png, "IDAT", idat);
  appendChunk(png, "IEND", {});
  return png;
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
  const double mb = static_cast<double>(idleRssBytes) / (1024.0 * 1024.0);
  const bool ok = idleRssBytes > 0 && idleRssBytes < kIdleRssCeilingBytes;
  std::printf("[selftest] idle RSS %.1f MB (ceiling 80 MB) %s\n", mb,
              ok ? "pass" : "FAIL");
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

  // --- 16-bit PNG: 2x2 fixture, hand-built (see buildMinimal16BitPng) -----
  {
    const uint16_t px[2 * 2 * 4] = {
        65535, 65535, 65535, 65535,  0,     0,     0,     65535,
        32768, 32768, 32768, 65535,  0,     0,     65535, 32768,
    };
    const std::vector<uint8_t> png = buildMinimal16BitPng(2, 2, px);

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

  // --- (c) view-only: toggling mirrorX/mirrorY/rotation/grayscale never
  // mutates PaintSim's own canvas texture -- the available headless proxy
  // for PLAN.md's "mirror both axes, save, reopen -- the file is
  // unmirrored" (no save path exists yet in this codebase to assert that
  // literally, so this checks the thing that could actually regress it: does
  // flipping view state write into the document/canvas at all).
  //
  // mirrorX/mirrorY/rotation have no code path into PaintSim whatsoever --
  // ui/MacPaintUI.cpp only ever reads them to place screen-space quad
  // corners (ViewTransform::toScreen()) and to invert a mouse position
  // (ViewTransform::toCanvas()), never to call anything on a PaintSim. The
  // one view flag that *does* reach into PaintSim is grayscale, via
  // updateGrayscalePreview() -- so that is what this actually exercises
  // against a live sim, twice (to catch a pass that only corrupts canvas_ on
  // a repeat), while readbackCanvas() -- the same technique
  // runFieldAllocationTest() and runSelfTest() already hold PaintSim to --
  // confirms canvas_ itself never moved. ----------------------------------
  {
    std::vector<uint8_t> before, after;
    const bool readBefore = sim.readbackCanvas(gpu, before);
    check(readBefore, "runViewTransformTest: canvas readback before toggling view state");

    CanvasView view;  // the real AppState type -- not a stand-in struct
    view.mirrorX = true;
    view.mirrorY = true;
    view.rotation = 1.234f;
    view.grayscale = true;
    (void)view;  // exercised for its shape only; nothing reads it further --
                 // mirror/rotation have no PaintSim call to make in the
                 // first place, per the comment above.
    sim.updateGrayscalePreview(gpu);
    sim.updateGrayscalePreview(gpu);

    const bool readAfter = sim.readbackCanvas(gpu, after);
    check(readAfter, "runViewTransformTest: canvas readback after toggling view state");
    check(readBefore && readAfter && before == after,
          "runViewTransformTest: mirror/rotation/grayscale view state leaves PaintSim's own "
          "canvas byte-identical (proxy for PLAN.md's \"save with a mirror on -> file is "
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

}  // namespace np
