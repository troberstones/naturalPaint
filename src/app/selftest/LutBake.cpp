#include "app/selftest/Support.hpp"

namespace np {

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
  // existing GPU-test style (runMipPyramidTest()'s own hand-picked corner/
  // known-pixel checks rather than exhaustive scans).
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

  // --- Invert, both domains -- the Display one is the only kernel in the
  // tree that includes shaders/include/srgb.wgsl, so this is what holds that
  // hand-port to color/Space.cpp's constants. ---
  {
    Op op;
    op.pointKind = PointOpKind::Invert;
    op.invert.domain = InvertParams::Domain::Linear;
    op.invert.amount = 0.75f;  // not 1.0: a kernel that ignored `amount` would
                               // otherwise be indistinguishable from a correct one
    const InvertParams params = op.invert;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyInvert(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake invert (linear domain, amount 0.75)");
  }
  {
    Op op;
    op.pointKind = PointOpKind::Invert;
    op.invert.domain = InvertParams::Domain::Display;
    op.invert.amount = 1.0f;
    const InvertParams params = op.invert;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyInvert(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake invert (display domain -- exercises srgb.wgsl)");
  }

  // --- Posterize. ---
  //
  // **A quantiser is a step function, so the fixture has to be checked and
  // not just chosen.** Everywhere else in this file a small CPU/GPU
  // disagreement stays small; here, a sample sitting within float noise of a
  // bin boundary would round one way on the CPU and the other on the GPU, and
  // the output would differ by a whole bin -- a real failure that says
  // nothing about the kernel. Rather than picking a level count and hoping,
  // the margin from every sample cell to the nearest boundary is measured
  // below and asserted to be far larger than the half-float step the bake
  // actually introduces.
  {
    constexpr int kLevels = 5;
    Op op;
    op.pointKind = PointOpKind::Posterize;
    op.posterize.levels = kLevels;

    const float step = 1.0f / static_cast<float>(kLevels - 1);
    float worstMargin = 1.0f;
    for (const auto& cell : sampleCells) {
      for (int c = 0; c < 3; ++c) {
        const float shaped = halfRT(gridCoord(cell[static_cast<size_t>(c)]));
        const float scaled = shaped / step;
        // Distance to the nearest .5, which is where std::round()/round() flips.
        const float margin = std::fabs(std::fabs(scaled - std::round(scaled)) - 0.5f);
        worstMargin = std::min(worstMargin, margin * step);
      }
    }
    std::printf("  [measured] posterize fixture: closest sample sits %.4f from a bin "
                "boundary, against a half-float step of about %.5f\n",
                static_cast<double>(worstMargin), 1.0 / 2048.0);
    check(worstMargin > 20.0f / 2048.0f,
          "LutBake posterize: no sample cell lands near a quantisation boundary, so a "
          "CPU/GPU disagreement here is the kernel and not a coin flip");

    const PosterizeParams params = op.posterize;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyPosterize(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake posterize (5 levels)");
  }

  // --- Threshold. Same step-function hazard as Posterize, and the same
  // treatment: the split point is compared against every sample's own shaped
  // luma rather than assumed to be clear of them.
  //
  // **And the split is 0.75 rather than the obvious 0.5 for a measured
  // reason.** Threshold's one real design decision is that the comparison
  // happens in the SHAPER domain, not in linear light (ops/ToneOps.hpp argues
  // why: 0.5 in linear light is nowhere near a perceptual midpoint). At a
  // split of 0.5 every sample cell here happens to fall on the same side of
  // both comparisons -- so a kernel that skipped the shaper encode entirely
  // and compared raw linear luma passed all six cells. That was measured, not
  // reasoned about: the sabotage was run and the test stayed green. At 0.75
  // three of the six cells disagree between the two comparisons, and the
  // assertion below states that as a property of the fixture so it cannot
  // quietly stop being true. ---
  {
    constexpr float kSplit = 0.75f;
    Op op;
    op.pointKind = PointOpKind::Threshold;
    op.threshold.threshold = kSplit;
    op.threshold.amount = 0.8f;  // not 1.0, for the reason Invert's is not

    float worstMargin = 1.0f;
    bool anyAbove = false;
    bool anyBelow = false;
    int domainDisagreements = 0;
    for (const auto& cell : sampleCells) {
      const std::array<float, 3> shaped{halfRT(gridCoord(cell[0])), halfRT(gridCoord(cell[1])),
                                        halfRT(gridCoord(cell[2]))};
      const std::array<float, 3> lin{shaperDecode(shaped[0]), shaperDecode(shaped[1]),
                                     shaperDecode(shaped[2])};
      const float linearLuma = computeLuma(lin);
      const float shapedLuma = shaperEncode(linearLuma);
      worstMargin = std::min(worstMargin, std::fabs(shapedLuma - kSplit));
      worstMargin = std::min(worstMargin, std::fabs(linearLuma - kSplit));
      if (shapedLuma >= kSplit) anyAbove = true; else anyBelow = true;
      if ((shapedLuma >= kSplit) != (linearLuma >= kSplit)) ++domainDisagreements;
    }
    std::printf("  [measured] threshold fixture: closest sample sits %.4f from the split in "
                "either domain; %d of %zu cells fall on OPPOSITE sides of it in the two "
                "domains\n",
                static_cast<double>(worstMargin), domainDisagreements, sampleCells.size());
    check(worstMargin > 20.0f / 2048.0f,
          "LutBake threshold: no sample cell's luma lands on the split, so a disagreement "
          "here is the kernel and not a coin flip");
    check(anyAbove && anyBelow,
          "LutBake threshold: the samples straddle the split -- some go white and some go "
          "black, so a kernel that returned one constant would fail rather than agree");
    check(domainDisagreements >= 2,
          "LutBake threshold: and the split separates the SHAPER-domain answer from the "
          "linear-light one on at least two cells, so a kernel that skipped the shaper "
          "encode cannot pass by coincidence");

    const ThresholdParams params = op.threshold;
    const std::vector<Op> gpuOps{op};
    const std::vector<PointOp> cpuOps{
        [params](const std::array<float, 3>& rgb) { return applyThreshold(rgb, params); }};
    runOneCase(gpuOps, cpuOps, "LutBake threshold (split 0.75, amount 0.8)");
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

  // --- Every kind bakes at all -------------------------------------------
  //
  // The cases above name their kinds one at a time, which means a
  // PointOpKind added later with no kernel behind it -- or with a kernel
  // whose WGSL does not compile, or whose bind group does not match the
  // layout -- reaches this file only if whoever added it also remembered to
  // add a case. `-Wswitch` cannot help here: color/LutBake.cpp looks its
  // kernel up in a table indexed by the enum's ordinal, and a table entry
  // naming a file that does not exist is a runtime failure, not a compile
  // error. This loop is over the enum itself, so it covers whatever the enum
  // holds.
  //
  // Params are left at their defaults deliberately: this asks "does this kind
  // have a working kernel", not "is the kernel right" -- which is what the
  // named cases above, with their non-default params, are for.
  {
    for (int k = 0; k <= static_cast<int>(PointOpKind::Threshold); ++k) {
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = static_cast<PointOpKind>(k);
      Lut3D lut = bakeLut(gpu, {op});
      char buf[160];
      std::snprintf(buf, sizeof buf,
                   "LutBake coverage: kind %d (%s) has a kernel that compiles and bakes", k,
                   pointOpKindName(op.pointKind));
      check(lut.texture != nullptr, buf);
      releaseLut3D(lut);
    }
  }

  std::printf("[selftest] lut bake %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
