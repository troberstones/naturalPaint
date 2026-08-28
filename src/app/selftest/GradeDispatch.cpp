#include "app/selftest/Support.hpp"

#include <chrono>

// docs/architecture-review.md P0-5 ("std::function called per pixel, when
// the fast path already exists in this repo"). See SelfTest.hpp for the
// full breakdown; in short, this file answers the three questions that
// finding raises:
//
//  1. Correctness: core/OpStack.hpp's new applyOpDirect()/applyOpsPremultiplied()
//     (a switch over core::Op, replacing core/Composite.cpp's per-pixel walk
//     through a std::vector<PointOp> of std::function closures) must compute
//     exactly what the closure path already did -- not "close", exactly,
//     since it is the same six functions called with the same params in the
//     same order.
//  2. Whether the review's *suggested* fix -- baking onto color/LutBake's
//     32^3 3-D LUT and sampling it per pixel instead -- is accurate enough
//     to ship on a save/export path. That LUT is entirely GPU-resident
//     (color/LutBake.hpp's Lut3D holds a WGPUTexture, nothing a CPU
//     compositor can read without a device readback), and its one existing
//     CPU-reachable caller, sim::PaintSim::updateGradePreview(), is a
//     narrow single-run live preview blit over the SDR paint canvas -- not
//     the multi-layer, multi-adjustment-layer document walk core/Composite
//     does on every save and export. Reusing it there is a different,
//     larger piece of work (threading a GpuContext through a walk that
//     today has none, and paying a synchronous GPU readback at every
//     rebake) than this finding's fix. So this file builds a from-scratch,
//     CPU-only, selftest-only bake + tetrahedral sample -- mirroring
//     color/LutBake.cpp's documented shaper-domain contract exactly, just
//     in C++ instead of WGSL -- purely to measure that question honestly,
//     not to ship it.
//  3. The speed this finding is actually about: wall-clock, before
//     (std::function closures) and after (the switch), at two realistic
//     canvas sizes.
namespace np {
namespace {

// ------------------------------------------------------------- op fixtures
//
// One "realistic" value per op kind (never the neutral/identity default --
// see this file's vacuity guard below), plus one deliberately harsh Curves
// shape for the "which op is a bad fit for a LUT" question ADR-0004 already
// predicts an answer to ("a 32^3 LUT cannot represent... a near-vertical
// curve segment").

Op opLevels() {
  Op op;
  op.pointKind = PointOpKind::Levels;
  LevelsParams p;
  p.blackIn = 0.05f;
  p.whiteIn = 0.90f;
  p.gamma = 1.4f;
  p.blackOut = 0.02f;
  p.whiteOut = 0.95f;
  op.levels = {p, p, p};
  return op;
}

Op opCurvesModerate() {
  Op op;
  op.pointKind = PointOpKind::Curves;
  const Curve s = {{0.0f, 0.0f}, {0.25f, 0.15f}, {0.5f, 0.5f}, {0.75f, 0.85f}, {1.0f, 1.0f}};
  op.curves = {s, s, s};
  return op;
}

// A contrast crunch: the middle segment climbs 0.8 of the curve's whole
// range across 0.1 of its domain -- an 8x steeper-than-average tangent
// dropped into one 1/32nd-wide LUT cell.
Op opCurvesHarsh() {
  Op op;
  op.pointKind = PointOpKind::Curves;
  const Curve steep = {{0.0f, 0.0f}, {0.45f, 0.1f}, {0.55f, 0.9f}, {1.0f, 1.0f}};
  op.curves = {steep, steep, steep};
  return op;
}

Op opExposure() {
  Op op;
  op.pointKind = PointOpKind::Exposure;
  op.exposure.stops = 1.5f;
  return op;
}

Op opSaturation() {
  Op op;
  op.pointKind = PointOpKind::Saturation;
  op.saturation.scale = 1.4f;
  return op;
}

// A mild desaturation rather than opSaturation()'s 1.4x boost -- used only
// to build conservativeStack() below, to separate "does the LUT round-trip
// badly in general" from "does it round-trip badly specifically when an op
// pushes a channel outside [0,1]" (see this file's LUT-accuracy section for
// why oversaturating a near-primary colour does exactly that).
Op opSaturationConservative() {
  Op op;
  op.pointKind = PointOpKind::Saturation;
  op.saturation.scale = 0.85f;
  return op;
}

Op opGrayscale() {
  Op op;
  op.pointKind = PointOpKind::Grayscale;  // default Rec.709 weights
  return op;
}

Op opChannelMixer() {
  Op op;
  op.pointKind = PointOpKind::ChannelMixer;
  op.channelMixer.matrix = {
      {{0.9f, 0.1f, 0.0f, 0.02f}, {0.05f, 0.85f, 0.1f, 0.0f}, {0.0f, 0.15f, 0.85f, -0.01f}}};
  return op;
}

// The realistic multi-op stack the task's error measurement asks for --
// five of the six kinds (Grayscale, tested alone above, is deliberately
// left out of the stack: chaining it collapses every later op's input to a
// single luma value, which would make the LUT's job trivially *easier*
// rather than exercising a genuine multi-op grade the way a Levels ->
// Curves -> Exposure -> Saturation -> Channel-mixer stack does).
std::vector<Op> realisticStack() {
  return {opLevels(), opCurvesModerate(), opExposure(), opSaturation(), opChannelMixer()};
}

// The identical stack with opSaturationConservative() in place of
// opSaturation() -- everything else unchanged. Exists only to show the LUT
// accuracy section's finding is about *which values an op pushes out of
// [0,1]*, not an inherent property of chaining five ops.
std::vector<Op> conservativeStack() {
  return {opLevels(), opCurvesModerate(), opExposure(), opSaturationConservative(),
          opChannelMixer()};
}

// Rebuilds `op` as an ops::PointOp closure -- the exact mapping
// core/OpStack.cpp's anonymous-namespace toPointOp() applies (that function
// has internal linkage and cannot be called from this translation unit, so
// this is a second, hand-written copy of the same six-way switch, the same
// duplication app/selftest/LutBake.cpp already accepts for its own
// GPU-vs-CPU cross-check). Used only to construct the "before" side of the
// regression and timing comparisons below -- ops::PointOp/
// applyPointOpsPremultiplied() themselves are untouched by this task.
PointOp toClosure(const Op& op) {
  switch (op.pointKind) {
    case PointOpKind::Levels: {
      const std::array<LevelsParams, 3> p = op.levels;
      return [p](const std::array<float, 3>& rgb) { return applyLevels(rgb, p); };
    }
    case PointOpKind::Curves: {
      const std::array<Curve, 3> p = op.curves;
      return [p](const std::array<float, 3>& rgb) { return applyCurves(rgb, p); };
    }
    case PointOpKind::Exposure: {
      const ExposureParams p = op.exposure;
      return [p](const std::array<float, 3>& rgb) { return applyExposure(rgb, p); };
    }
    case PointOpKind::Saturation: {
      const SaturationParams p = op.saturation;
      return [p](const std::array<float, 3>& rgb) { return applySaturation(rgb, p); };
    }
    case PointOpKind::Grayscale: {
      const GrayscaleParams p = op.grayscale;
      return [p](const std::array<float, 3>& rgb) { return applyGrayscale(rgb, p); };
    }
    case PointOpKind::ChannelMixer: {
      const ChannelMixerParams p = op.channelMixer;
      return [p](const std::array<float, 3>& rgb) { return applyChannelMixer(rgb, p); };
    }
  }
  return [](const std::array<float, 3>& rgb) { return rgb; };
}

std::vector<PointOp> toClosures(const std::vector<Op>& ops) {
  std::vector<PointOp> out;
  out.reserve(ops.size());
  for (const Op& op : ops) out.push_back(toClosure(op));
  return out;
}

// ------------------------------------------------------ CPU-only LUT bake
//
// Everything below this point exists only to measure question 2 above; it
// is deliberately not color/LutBake.hpp's Lut3D (a GPU texture) and not
// wired into any shipped call path -- see this file's header comment.

std::array<float, 3> shaperEncode3(const std::array<float, 3>& lin) noexcept {
  return {shaperEncode(lin[0]), shaperEncode(lin[1]), shaperEncode(lin[2])};
}

std::array<float, 3> shaperDecode3(const std::array<float, 3>& shaped) noexcept {
  return {shaperDecode(shaped[0]), shaperDecode(shaped[1]), shaperDecode(shaped[2])};
}

// The exact op chain, evaluated directly through applyOpDirect() -- what
// core/Composite.cpp's shipped per-pixel switch actually computes, and
// therefore this file's ground truth for "how far off is the LUT."
std::array<float, 3> referenceChain(const std::array<float, 3>& rgb, const std::vector<Op>& ops) {
  std::array<float, 3> v = rgb;
  for (const Op& op : ops) {
    if (!op.enabled || op.opClass != OpClass::PointA) continue;
    v = applyOpDirect(v, op);
  }
  return v;
}

// Bakes `ops` onto a `size`^3 grid of shaper-domain RGB triples, flattened
// x-major (index = ((x*size + y)*size + z)*3), reproducing color/
// LutBake.cpp's own documented per-op-pass contract (that file's header
// comment) in plain C++: the grid coordinate IS the seed's shaper-domain
// value; every op except Curves wraps its linear-domain math in
// shaperDecode -> op -> shaperEncode -> clamp01; Curves runs directly on
// the shaper-domain texel (ADR-0004: curve control points are authored in
// that domain already). No half-float quantization is simulated here --
// this reference isolates "what does tetrahedral interpolation itself
// cost", a different question from "what does rgba16float storage cost",
// which app/selftest/LutBake.cpp's simulateBakeCpu() already answers.
std::vector<float> bakeCpuLut(const std::vector<Op>& ops, int32_t size) {
  const size_t s = static_cast<size_t>(size);
  std::vector<float> out(s * s * s * 3);
  for (int32_t x = 0; x < size; ++x) {
    for (int32_t y = 0; y < size; ++y) {
      for (int32_t z = 0; z < size; ++z) {
        std::array<float, 3> shaped{(static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                                    (static_cast<float>(y) + 0.5f) / static_cast<float>(size),
                                    (static_cast<float>(z) + 0.5f) / static_cast<float>(size)};
        for (const Op& op : ops) {
          if (!op.enabled || op.opClass != OpClass::PointA) continue;
          if (op.pointKind == PointOpKind::Curves) {
            shaped = {evalCurve(op.curves[0], shaped[0]), evalCurve(op.curves[1], shaped[1]),
                      evalCurve(op.curves[2], shaped[2])};
          } else {
            shaped = shaperEncode3(applyOpDirect(shaperDecode3(shaped), op));
          }
          for (float& c : shaped) c = std::clamp(c, 0.0f, 1.0f);
        }
        const size_t idx = ((static_cast<size_t>(x) * s + static_cast<size_t>(y)) * s +
                            static_cast<size_t>(z)) * 3;
        out[idx + 0] = shaped[0];
        out[idx + 1] = shaped[1];
        out[idx + 2] = shaped[2];
      }
    }
  }
  return out;
}

std::array<float, 3> lutAt(const std::vector<float>& lut, int32_t size, int x, int y, int z) {
  const size_t s = static_cast<size_t>(size);
  const size_t idx =
      ((static_cast<size_t>(x) * s + static_cast<size_t>(y)) * s + static_cast<size_t>(z)) * 3;
  return {lut[idx + 0], lut[idx + 1], lut[idx + 2]};
}

// Six-tetrahedra decomposition of the unit cube (Kasson et al. 1993 --
// ADR-0004's own "tetrahedral is the fix Resolve and OCIO use" note). Which
// of the six tetrahedra (fx,fy,fz) falls in is decided purely by their
// relative order; every branch walks the same corner000 -> one-axis ->
// two-axis -> all-three-axes path, weighted by the three fractions sorted
// into that order.
std::array<float, 3> sampleTetrahedral(const std::vector<float>& lut, int32_t size,
                                       const std::array<float, 3>& shaper) {
  // color/LutBake.cpp's own seed-pass convention: texel (i) stores the
  // value at (i+0.5)/size, i.e. texel centres sit at half-texel offsets --
  // the same convention GPU hardware trilinear sampling uses. The inverse
  // of "texel i is at (i+0.5)/size" is "coordinate c's continuous index is
  // c*size - 0.5", clamped so the interpolated cell never reads outside
  // the baked grid.
  int i0 = 0, j0 = 0, k0 = 0;
  auto axis = [size](float c, int& i0out) {
    float idx = std::clamp(c, 0.0f, 1.0f) * static_cast<float>(size) - 0.5f;
    idx = std::clamp(idx, 0.0f, static_cast<float>(size - 1));
    i0out = std::min(static_cast<int>(idx), size - 2);
    if (i0out < 0) i0out = 0;
    return idx - static_cast<float>(i0out);
  };
  const float fx = axis(shaper[0], i0);
  const float fy = axis(shaper[1], j0);
  const float fz = axis(shaper[2], k0);

  const auto c000 = lutAt(lut, size, i0, j0, k0);
  const auto c100 = lutAt(lut, size, i0 + 1, j0, k0);
  const auto c010 = lutAt(lut, size, i0, j0 + 1, k0);
  const auto c001 = lutAt(lut, size, i0, j0, k0 + 1);
  const auto c110 = lutAt(lut, size, i0 + 1, j0 + 1, k0);
  const auto c101 = lutAt(lut, size, i0 + 1, j0, k0 + 1);
  const auto c011 = lutAt(lut, size, i0, j0 + 1, k0 + 1);
  const auto c111 = lutAt(lut, size, i0 + 1, j0 + 1, k0 + 1);

  std::array<float, 3> out{};
  for (int ch = 0; ch < 3; ++ch) {
    const size_t c = static_cast<size_t>(ch);
    const float v000 = c000[c], v100 = c100[c], v010 = c010[c], v001 = c001[c];
    const float v110 = c110[c], v101 = c101[c], v011 = c011[c], v111 = c111[c];
    float v;
    if (fx >= fy) {
      if (fy >= fz)
        v = v000 + fx * (v100 - v000) + fy * (v110 - v100) + fz * (v111 - v110);
      else if (fx >= fz)
        v = v000 + fx * (v100 - v000) + fz * (v101 - v100) + fy * (v111 - v101);
      else
        v = v000 + fz * (v001 - v000) + fx * (v101 - v001) + fy * (v111 - v101);
    } else {
      if (fz >= fy)
        v = v000 + fz * (v001 - v000) + fy * (v011 - v001) + fx * (v111 - v011);
      else if (fz >= fx)
        v = v000 + fy * (v010 - v000) + fz * (v011 - v010) + fx * (v111 - v011);
      else
        v = v000 + fy * (v010 - v000) + fx * (v110 - v010) + fz * (v111 - v110);
    }
    out[c] = v;
  }
  return out;
}

// Absolute error between the LUT's approximation and referenceChain(),
// maxed and meaned per channel over a `steps`^3 grid spanning [lo, hi] on
// every one of R, G and B independently -- the "dense sample of the RGB
// cube" the task asks for, not a handful of hand-picked points.
struct ErrorStats {
  std::array<float, 3> maxAbs{0.0f, 0.0f, 0.0f};
  std::array<double, 3> sumAbs{0.0, 0.0, 0.0};
  size_t n = 0;
};

ErrorStats measureLutError(const std::vector<Op>& ops, int32_t size, int steps, float lo,
                           float hi) {
  const std::vector<float> lut = bakeCpuLut(ops, size);
  ErrorStats stats;
  for (int xi = 0; xi < steps; ++xi) {
    const float r = lo + (hi - lo) * (steps > 1 ? static_cast<float>(xi) / (steps - 1) : 0.5f);
    for (int yi = 0; yi < steps; ++yi) {
      const float g = lo + (hi - lo) * (steps > 1 ? static_cast<float>(yi) / (steps - 1) : 0.5f);
      for (int zi = 0; zi < steps; ++zi) {
        const float b = lo + (hi - lo) * (steps > 1 ? static_cast<float>(zi) / (steps - 1) : 0.5f);
        const std::array<float, 3> lin{r, g, b};
        const std::array<float, 3> ref = referenceChain(lin, ops);

        std::array<float, 3> shapedIn = shaperEncode3(lin);
        for (float& c : shapedIn) c = std::clamp(c, 0.0f, 1.0f);
        const std::array<float, 3> approx = shaperDecode3(sampleTetrahedral(lut, size, shapedIn));

        for (int ch = 0; ch < 3; ++ch) {
          const size_t c = static_cast<size_t>(ch);
          const float err = std::fabs(approx[c] - ref[c]);
          stats.maxAbs[c] = std::max(stats.maxAbs[c], err);
          stats.sumAbs[c] += err;
        }
        ++stats.n;
      }
    }
  }
  return stats;
}

void printError(const char* label, const ErrorStats& s) {
  std::printf(
      "  [measured] LUT accuracy, %-38s max |err| R=%.3e G=%.3e B=%.3e   mean |err| R=%.3e "
      "G=%.3e B=%.3e  (n=%zu)\n",
      label, static_cast<double>(s.maxAbs[0]), static_cast<double>(s.maxAbs[1]),
      static_cast<double>(s.maxAbs[2]), s.sumAbs[0] / static_cast<double>(s.n),
      s.sumAbs[1] / static_cast<double>(s.n), s.sumAbs[2] / static_cast<double>(s.n), s.n);
}

float maxOf(const std::array<float, 3>& v) { return std::max({v[0], v[1], v[2]}); }

}  // namespace

bool runGradeDispatchTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // -----------------------------------------------------------------------
  // 1. Vacuity guard, first: every case below is meaningless if its stack
  //    happens to be a no-op. Confirmed against a representative pixel, not
  //    assumed from the params looking non-neutral.
  // -----------------------------------------------------------------------
  {
    const std::vector<Op> stack = realisticStack();
    const std::array<float, 3> probe{0.5f, 0.4f, 0.3f};
    const std::array<float, 3> graded = referenceChain(probe, stack);
    const float delta =
        std::fabs(graded[0] - probe[0]) + std::fabs(graded[1] - probe[1]) + std::fabs(graded[2] - probe[2]);
    std::printf("  [measured] realisticStack() on (0.50,0.40,0.30) -> (%.4f,%.4f,%.4f), |delta|=%.4f\n",
               static_cast<double>(graded[0]), static_cast<double>(graded[1]),
               static_cast<double>(graded[2]), static_cast<double>(delta));
    check(delta > 0.05f,
          "vacuity guard: realisticStack() changes a representative pixel by more than 0.05");
  }

  // -----------------------------------------------------------------------
  // 2. Correctness regression: applyOpDirect()/applyOpsPremultiplied()
  //    (core/OpStack.hpp's switch, what core/Composite.cpp's per-pixel walk
  //    now calls) against ops::applyPointOpsPremultiplied() (the untouched
  //    std::function-closure path every other caller in this suite still
  //    uses) -- same six functions, same params, so exact equality is the
  //    right bar, not a tolerance.
  // -----------------------------------------------------------------------
  {
    const std::vector<std::pair<const char*, std::vector<Op>>> cases = {
        {"Levels", {opLevels()}},
        {"Curves", {opCurvesModerate()}},
        {"Exposure", {opExposure()}},
        {"Saturation", {opSaturation()}},
        {"Grayscale", {opGrayscale()}},
        {"ChannelMixer", {opChannelMixer()}},
        {"realistic stack (5 ops)", realisticStack()},
    };
    const std::vector<std::array<float, 4>> pixels = {
        {0.9f, 0.8f, 0.7f, 1.0f},
        {0.2f, 0.5f, 0.05f, 0.6f},
        {0.01f, 0.01f, 0.01f, 0.3f},
        {0.0f, 0.0f, 0.0f, 0.0f},  // transparent: both paths must short-circuit identically
    };
    for (const auto& [label, ops] : cases) {
      const std::vector<PointOp> closures = toClosures(ops);
      bool allExact = true;
      for (const std::array<float, 4>& px : pixels) {
        const std::array<float, 4> viaClosure = applyPointOpsPremultiplied(px, closures);
        const std::array<float, 4> viaSwitch = applyOpsPremultiplied(px, ops);
        for (int c = 0; c < 4; ++c)
          allExact = allExact && near(viaClosure[static_cast<size_t>(c)],
                                      viaSwitch[static_cast<size_t>(c)], 0.0f);
      }
      char buf[128];
      std::snprintf(buf, sizeof buf, "regression: switch matches closures exactly -- %s", label);
      check(allExact, buf);
    }
  }

  // -----------------------------------------------------------------------
  // 3. LUT accuracy: a from-scratch CPU bake + tetrahedral sample (this
  //    file's own, see header comment for why it is not color/LutBake.hpp's
  //    GPU Lut3D) against referenceChain(), over a 24-step-per-axis grid of
  //    the [0,1] working range -- 13824 sample points per case, not a
  //    handful of hand-picked ones.
  // -----------------------------------------------------------------------
  constexpr int kSteps = 24;
  {
    // Sanity check on the measurement pipeline itself: an empty op list
    // must bake/sample back to (very close to) the identity, or the bake or
    // the tetrahedral sampler has a bug that would otherwise be invisible
    // (an error measurement over a broken *harness* could easily look like
    // "the LUT is accurate" for the wrong reason).
    const ErrorStats identity = measureLutError({}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("identity (sanity check on this harness)", identity);
    check(maxOf(identity.maxAbs) < 5e-3f,
          "LUT sanity: an empty op list round-trips through bake+sample near-exactly");

    const ErrorStats levels = measureLutError({opLevels()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Levels", levels);
    const ErrorStats curvesModerate =
        measureLutError({opCurvesModerate()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Curves (moderate S-curve)", curvesModerate);
    const ErrorStats curvesHarsh = measureLutError({opCurvesHarsh()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Curves (harsh contrast-crunch)", curvesHarsh);
    const ErrorStats exposure = measureLutError({opExposure()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Exposure", exposure);
    const ErrorStats saturation = measureLutError({opSaturation()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Saturation", saturation);
    const ErrorStats grayscale = measureLutError({opGrayscale()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("Grayscale", grayscale);
    const ErrorStats channelMixer =
        measureLutError({opChannelMixer()}, kLutSize, kSteps, 0.0f, 1.0f);
    printError("ChannelMixer", channelMixer);
    const ErrorStats stack = measureLutError(realisticStack(), kLutSize, kSteps, 0.0f, 1.0f);
    printError("realistic stack (5 ops, oversaturating)", stack);
    const ErrorStats conservative = measureLutError(conservativeStack(), kLutSize, kSteps, 0.0f, 1.0f);
    printError("conservative stack (5 ops, in-gamut)", conservative);

    // Finding #1, stated as a check rather than left to be read off the
    // printed numbers alone: ADR-0004 predicts "a 32^3 LUT cannot represent
    // ... a near-vertical curve segment", and a deliberately steep Curves op
    // should measure a materially larger error than the same LUT baking a
    // moderate one -- not merely a noisier number, several times larger, at
    // exactly the finding's stated cause (grid resolution vs. tangent
    // steepness).
    check(maxOf(curvesHarsh.maxAbs) > maxOf(curvesModerate.maxAbs) * 3.0f,
          "LUT accuracy: the harsh contrast-crunch curve's max error is materially larger than "
          "the moderate curve's -- confirms ADR-0004's steep-tangent prediction rather than "
          "asserting it from the doc alone");

    // Finding #2, not predicted by ADR-0004 in so many words but the same
    // mechanism at heart: color/LutBake's own documented contract clamps
    // every op pass's shaper-domain result to [0,1] before writing it into
    // the grid (color/LutBake.hpp's header comment), but ops/PointOps'
    // ops never clamp their own output (PointOps.hpp: "Do NOT clamp their
    // output... whether/how a shaped value gets clamped... is color/
    // LutBake's... job"). Saturation's scale=1.4 does exactly what an
    // "oversaturate" control is for: it pushes a near-primary colour's
    // low channel(s) negative (verified by hand: Rec.709 luma of pure red
    // is 0.2126, so scale 1.4 sends green/blue to 0.2126 + (0-0.2126)*1.4
    // = -0.085) -- a value the reference chain keeps exactly, and the LUT
    // bake clamps away at every grid node the moment it is baked, not only
    // near the RGB cube's edges the way Curves' failure is localised to one
    // curve segment. ChannelMixer is the fairest same-shape comparison
    // (also a linear combination needing the same shaper round-trip) --
    // its matrix keeps outputs close to [0,1] for [0,1] inputs by
    // construction, so its own clamp cost stays small, and Saturation's is
    // the outlier by roughly two orders of magnitude, not a close call.
    check(maxOf(saturation.maxAbs) > maxOf(channelMixer.maxAbs) * 10.0f,
          "LUT accuracy: Saturation's out-of-[0,1] clamp cost is far larger than "
          "ChannelMixer's -- oversaturating a near-primary colour is a second, distinct "
          "bad fit for this LUT design, not merely a steep-curve problem");

    // Finding #2's other half: the same five-op *shape*, with only
    // Saturation's scale changed from 1.4 (push out of gamut) to 0.85 (stay
    // inside it), should recover a materially smaller error -- proving the
    // problem is "does this particular op push values outside [0,1]", not
    // "chaining five ops through a LUT is inherently this inaccurate."
    check(maxOf(conservative.maxAbs) < maxOf(stack.maxAbs) * 0.25f,
          "LUT accuracy: swapping in an in-gamut Saturation recovers most of the accuracy the "
          "oversaturating stack lost -- the problem is which values leave [0,1], not the LUT "
          "or the five-op chain itself");

    // Every op/shape that never asks a channel to leave [0,1] stays within
    // a bound generous enough not to flake on a legitimate float/libm
    // difference across platforms, but tight enough that a real formula bug
    // (wrong constant, wrong channel, a broken tetrahedral corner) -- which
    // produces errors an order of magnitude or more larger, the same
    // "different order of magnitude" signature runLutBakeTest()'s own
    // kResidualTol comment describes -- would still trip it. Saturation and
    // the oversaturating stack are deliberately excluded here: their larger
    // error is Finding #2 above, already checked on its own terms, not a
    // sanity bound this pathological-by-design pair is expected to clear.
    for (const ErrorStats* s : {&levels, &exposure, &grayscale, &channelMixer, &curvesModerate,
                                &conservative}) {
      check(maxOf(s->maxAbs) < 0.15f,
            "LUT accuracy: in-gamut ops/stacks stay under a generous 0.15 max-error sanity bound");
    }
  }

  // -----------------------------------------------------------------------
  // 4. Speed: the realistic stack's op-chain evaluation, closures (before)
  //    vs. the switch (after), over a synthetic WxH canvas -- printed, not
  //    check()-gated, per this suite's documented wall-clock flake class.
  // -----------------------------------------------------------------------
  {
    const std::vector<Op> stack = realisticStack();
    const std::vector<PointOp> closures = toClosures(stack);

    auto pixelAt = [](int64_t i, int32_t side) {
      const float t = static_cast<float>(i % side) / static_cast<float>(side);
      return std::array<float, 4>{0.2f + 0.5f * t, 0.4f, 0.6f - 0.3f * t, 0.15f + 0.8f * t};
    };

    for (const int32_t side : {1024, 2048}) {
      const int64_t n = static_cast<int64_t>(side) * static_cast<int64_t>(side);

      // One untimed warm-up pass each, so the first call's cold-cache cost
      // doesn't land lopsidedly on whichever path runs first.
      double warm = 0.0;
      for (int64_t i = 0; i < 4096; ++i) {
        const auto out = applyPointOpsPremultiplied(pixelAt(i, side), closures);
        warm += out[0];
        const auto out2 = applyOpsPremultiplied(pixelAt(i, side), stack);
        warm += out2[0];
      }

      double sumOld = 0.0;
      const auto t0 = std::chrono::steady_clock::now();
      for (int64_t i = 0; i < n; ++i) {
        const std::array<float, 4> out = applyPointOpsPremultiplied(pixelAt(i, side), closures);
        sumOld += out[0] + out[1] + out[2] + out[3];
      }
      const auto t1 = std::chrono::steady_clock::now();

      double sumNew = 0.0;
      for (int64_t i = 0; i < n; ++i) {
        const std::array<float, 4> out = applyOpsPremultiplied(pixelAt(i, side), stack);
        sumNew += out[0] + out[1] + out[2] + out[3];
      }
      const auto t2 = std::chrono::steady_clock::now();

      const double msOld = std::chrono::duration<double, std::milli>(t1 - t0).count();
      const double msNew = std::chrono::duration<double, std::milli>(t2 - t1).count();
      std::printf(
          "  [measured] %dx%d adjustment-layer op-chain (5-op stack, %lld pixels): "
          "closures(before) %.2f ms, switch(after) %.2f ms, speedup %.2fx  (checksums "
          "%.6g/%.6g/%.6g, ignore)\n",
          side, side, static_cast<long long>(n), msOld, msNew,
          msNew > 0.0 ? msOld / msNew : 0.0, warm, sumOld, sumNew);
    }

    // The same question asked literally, not just isolated: a real
    // core::compositeDocumentPremultiplied() call, on a document holding
    // one opaque painted RGB layer under one Adjustment layer carrying the
    // realistic 5-op stack -- the actual save/export entry point this
    // finding is about, tile walk/mask lookups/accumulator writes and all.
    // "Before" is deliberately not reconstructed here: that would mean
    // re-deriving core/Composite.cpp's whole walk against the old
    // std::function path from scratch (a second compositor to keep in
    // sync), which is a much larger and more error-prone thing to build
    // than the isolated op-chain comparison above already honestly answers
    // -- this is context on the absolute cost, not a second before/after
    // pair.
    for (const int32_t side : {1024, 2048}) {
      Document doc = Document::createBlank(side, side, WorkingSpace{});
      for (int32_t y = 0; y < side; ++y) {
        for (int32_t x = 0; x < side; ++x) {
          const float t = static_cast<float>(x) / static_cast<float>(side);
          const PixelCoord at{x, y};
          doc.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {0.2f + 0.5f * t, 0.4f, 0.6f - 0.3f * t, 1.0f});
        }
      }
      addLayer(doc, 1, makeAdjustmentLayer("bench"));
      for (const Op& op : stack) doc.layers[1].ops.add(op);

      (void)compositeDocumentPremultiplied(doc);  // untimed warm-up
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<float> out = compositeDocumentPremultiplied(doc);
      const auto t1 = std::chrono::steady_clock::now();
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      std::printf(
          "  [measured] %dx%d full document composite (current code, one RGB layer + one "
          "5-op Adjustment layer): %.2f ms  (output size %zu floats, ignore)\n",
          side, side, ms, out.size());
    }
  }

  std::printf("[selftest] grade dispatch %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
