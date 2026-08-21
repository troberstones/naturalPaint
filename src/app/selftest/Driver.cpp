#include "app/selftest/Support.hpp"

// The three --selftest drivers: runSelfTest() (the interactive/visual pigment
// walkthrough), runModeTest() and runDiagnostic(). They are not sections --
// nothing in main.cpp's `ok` chain calls them -- so they share one TU rather
// than getting one each.

namespace np {

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


}  // namespace np
