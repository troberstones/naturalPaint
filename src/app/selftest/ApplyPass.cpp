#include "app/selftest/Support.hpp"

namespace np {

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


}  // namespace np
