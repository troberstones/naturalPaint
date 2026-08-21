#include "app/selftest/Support.hpp"

#include "core/PigmentBake.hpp"

// The solver-to-document mass mapping (PLAN.md roadmap section 11).
//
// `core/PigmentBake.hpp` argues at length why a baked tile stores the
// Beer-Lambert coverage rather than the solver's own mass. This section is
// that argument as arithmetic, and its centre is §4: **the bake produces no
// flash**, which is asserted by showing that the pixel `shaders/composite.wgsl`
// puts on screen for a wet texel and the pixel `core/Composite` puts on screen
// for the baked tile are the same pixel -- not close, the same.
//
// §1 checks the two languages still agree, by reading the shader source. That
// is possible because `NP_SHADER_DIR` points at the tree's own `shaders/` and
// the passes reload from disk, so this is the file the GPU actually runs, not
// a copy of it. It is the only cross-language assertion in the suite that
// reads the other language rather than transcribing it.

namespace np {
namespace {

// The shader text, or empty if it could not be read. A missing file is a
// failure rather than a skip: these constants exist in two places and the
// whole point of §1 is that nothing silently stops checking that.
std::string shaderText(const char* relative) {
  std::string path = std::string(NP_SHADER_DIR) + "/" + relative;
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// `composite.wgsl`'s own screen arithmetic, transcribed for one texel, with
// the fibre gain factored out so a caller can ask for it with or without the
// paper grain. Returns straight (non-premultiplied) linear RGB over `base`.
std::array<float, 3> screenRgb(const Latent& latent, float simMass, float absorption,
                               float fibreGain, const std::array<float, 3>& base) {
  const std::array<float, 3> pigment = latentToRgb(latent);
  const float opacity = 1.0f - std::exp(-absorption * simMass);
  std::array<float, 3> out{};
  for (size_t i = 0; i < 3; ++i)
    out[i] = base[i] * (1.0f - opacity) + pigment[i] * fibreGain * opacity;
  return out;
}

// `core/Composite`'s projection of a Pigment texel -- `(latentToRgb*m, m)`,
// premultiplied -- composited `over` the same base.
std::array<float, 3> bakedRgb(const PigmentTexel& texel, const std::array<float, 3>& base) {
  const std::array<float, 3> pigment = latentToRgb(texel.latent);
  std::array<float, 3> out{};
  for (size_t i = 0; i < 3; ++i)
    out[i] = pigment[i] * texel.mass + base[i] * (1.0f - texel.mass);
  return out;
}

}  // namespace

bool runPigmentBakeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ======================================================================
  // 1. The two languages still agree -- read, not transcribed.
  // ======================================================================
  std::printf("  -- 1. the constants, read out of the shader the GPU runs --\n");
  {
    const std::string composite = shaderText("composite.wgsl");
    const std::string mixbox = shaderText("include/mixbox.wgsl");
    check(!composite.empty(), "bake: shaders/composite.wgsl is readable at NP_SHADER_DIR");
    check(!mixbox.empty(), "bake: shaders/include/mixbox.wgsl is readable at NP_SHADER_DIR");

    // Written as the shader writes them. If someone retunes the medium in
    // WGSL, this fails and names the constant, instead of the bake quietly
    // disagreeing with the screen by a fixed factor forever.
    check(composite.find("exp(-2.6 * mass)") != std::string::npos &&
              kAbsorptionWatercolor == 2.6f,
          "bake: the watercolour absorption is 2.6 in BOTH languages");
    check(composite.find("exp(-4.2 * mass)") != std::string::npos && kAbsorptionInk == 4.2f,
          "bake: the ink absorption is 4.2 in BOTH languages");
    check(composite.find("pigC * 0.75") != std::string::npos &&
              kSuspendedWeightWatercolor == 0.75f,
          "bake: the watercolour suspended weight is 0.75 in BOTH languages");
    check(composite.find("pigC * 0.55") != std::string::npos && kSuspendedWeightInk == 0.55f,
          "bake: the ink suspended weight is 0.55 in BOTH languages");
    check(mixbox.find("max(cm.w, 1e-5)") != std::string::npos && kMassEpsilon == 1e-5f,
          "bake: the mass-divide guard is 1e-5 in BOTH languages");

    // The fibre gains, which are deliberately NOT baked. Asserted so the
    // decision stays visible: if someone changes the grain, this names the
    // thing a baked stroke is losing.
    check(composite.find("0.93 + 0.07 * fibre") != std::string::npos,
          "bake: watercolour fibre gain is 0.93+0.07*fibre -- grain, deliberately not baked");
    check(composite.find("0.90 + 0.10 * fibre") != std::string::npos,
          "bake: ink fibre gain is 0.90+0.10*fibre -- same decision");
  }

  // ======================================================================
  // 2. The mapping itself.
  // ======================================================================
  std::printf("  -- 2. Beer-Lambert coverage, and why a raw mass copy will not do --\n");
  {
    // The table from core/PigmentBake.hpp's section 2, recomputed here. The
    // point is the *delta* column: that is the flash a naive bake would show.
    const float probes[] = {0.05f, 0.10f, 0.20f, 0.30f, 0.50f, 0.75f, 1.00f, 2.00f};
    float worstDelta = 0.0f;
    float worstAt = 0.0f;
    std::printf("  %-10s %-12s %-14s %s\n", "sim mass", "baked", "if copied", "delta");
    for (float m : probes) {
      const float baked = bakedMassFromSim(m, kAbsorptionWatercolor);
      const float copied = std::min(m, 1.0f);
      const float delta = baked - copied;
      if (std::fabs(delta) > std::fabs(worstDelta)) {
        worstDelta = delta;
        worstAt = m;
      }
      std::printf("  %-10.2f %-12.4f %-14.4f %+.4f\n", static_cast<double>(m),
                  static_cast<double>(baked), static_cast<double>(copied),
                  static_cast<double>(delta));
    }
    std::printf("  [selftest] bake: a raw mass copy would jump alpha by %+.4f at sim mass %.2f\n",
                static_cast<double>(worstDelta), static_cast<double>(worstAt));
    check(std::fabs(worstDelta) > 0.2f,
          "bake: copying the mass would move alpha by more than 0.2 -- a visible flash");

    check(bakedMassFromSim(0.0f, kAbsorptionWatercolor) == 0.0f,
          "bake: zero mass bakes to zero coverage exactly");
    // Monotonic and bounded over a range far past anything the solver makes.
    bool monotonic = true, bounded = true;
    float previous = -1.0f;
    for (int i = 0; i <= 2000; ++i) {
      const float m = static_cast<float>(i) * 0.01f;
      const float baked = bakedMassFromSim(m, kAbsorptionWatercolor);
      monotonic = monotonic && baked >= previous;
      bounded = bounded && baked >= 0.0f && baked <= 1.0f;
      previous = baked;
    }
    check(monotonic, "bake: the mapping is monotonic over sim mass 0..20");
    check(bounded, "bake: and never leaves [0,1] -- which a raw copy does at two dabs");
    check(bakedMassFromSim(2.0f, kAbsorptionWatercolor) > 1.0f - 1e-2f,
          "bake: a heavy load saturates rather than clipping at some other value");

    // Ink builds faster than watercolour at the same mass, which is the whole
    // reason there are two coefficients rather than one.
    check(bakedMassFromSim(0.2f, kAbsorptionInk) > bakedMassFromSim(0.2f, kAbsorptionWatercolor),
          "bake: ink builds faster than watercolour at equal mass");
  }

  // ======================================================================
  // 3. `over` on coverages IS Beer-Lambert accumulation.
  // ======================================================================
  //
  // The property that lets a rolling bake behind a long stroke be identical
  // to one bake at the end. Asserted across a grid, and then shown to be a
  // real discriminator by running the same grid through the mapping it
  // replaced.
  std::printf("  -- 3. two glazes baked separately == one bake of the sum --\n");
  {
    const float masses[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 0.9f, 1.5f, 3.0f};
    // f32 arithmetic, three exp/log at ~1e-7 relative each, over values that
    // reach 1.0 -- 1e-6 is roughly ten ulp and is derived, not dialled until
    // the test passed. The naive mapping below misses by 0.2, so this bound
    // cannot be hiding the thing the check exists to catch.
    constexpr float kTol = 1e-6f;
    bool holds = true;
    float worstBake = 0.0f;
    for (float a : masses) {
      for (float b : masses) {
        holds = holds && bakeCompositionHolds(a, b, kAbsorptionWatercolor, kTol);
        const float baked = bakedMassFromSim(a, kAbsorptionWatercolor);
        const float other = bakedMassFromSim(b, kAbsorptionWatercolor);
        worstBake = std::max(worstBake,
                             std::fabs(baked + other * (1.0f - baked) -
                                       bakedMassFromSim(a + b, kAbsorptionWatercolor)));
      }
    }
    std::printf("  [selftest] bake: over(bake a, bake b) vs bake(a+b), worst of 64 pairs %.3e\n",
                static_cast<double>(worstBake));
    check(holds, "bake: over(bake a, bake b) == bake(a+b) for every pair, at 1e-6");
    check(worstBake < kTol, "bake: and the worst measured error is inside that bound");

    // Non-vacuity. The same grid under the mapping this one replaced -- a
    // clamped linear copy -- must FAIL, or the check above proves nothing.
    float worstNaive = 0.0f;
    for (float a : masses) {
      for (float b : masses) {
        const float ca = std::min(a, 1.0f), cb = std::min(b, 1.0f);
        worstNaive = std::max(worstNaive, std::fabs(ca + cb * (1.0f - ca) - std::min(a + b, 1.0f)));
      }
    }
    std::printf("  [selftest] bake: the same grid under a linear mass copy is off by %.4f\n",
                static_cast<double>(worstNaive));
    check(worstNaive > 0.1f,
          "bake: a linear copy fails that identity badly -- so the check discriminates");

    // Also holds for ink, so the property is the mapping's and not the
    // constant's.
    bool inkHolds = true;
    for (float a : masses)
      for (float b : masses) inkHolds = inkHolds && bakeCompositionHolds(a, b, kAbsorptionInk, kTol);
    check(inkHolds, "bake: the identity is the mapping's, not the coefficient's -- ink too");
  }

  // ======================================================================
  // 4. No flash: the wet pixel and the baked pixel are the same pixel.
  // ======================================================================
  //
  // This is the claim the whole design turns on, and the reason it is a test
  // rather than a paragraph. At fibre == 1 the gain is exactly 1, so the two
  // renderers must agree bit for bit; the grain difference is measured
  // separately below rather than folded in and tolerated.
  std::printf("  -- 4. the bake is invisible: solver pixel == baked pixel --\n");
  {
    const std::array<float, 3> bases[] = {
        {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.85f, 0.80f, 0.72f}, {0.2f, 0.4f, 0.6f}};
    const std::array<float, 3> colours[] = {
        {0.85f, 0.10f, 0.10f}, {0.10f, 0.20f, 0.75f}, {0.95f, 0.85f, 0.15f}, {0.3f, 0.3f, 0.3f}};
    const float masses[] = {0.02f, 0.1f, 0.4f, 1.0f, 2.5f};

    MixboxLut lut;
    const bool lutOk = lut.load(NP_MIXBOX_LUT);
    check(lutOk, "bake: the Mixbox LUT loaded, so these are real pigment latents");

    float worst = 0.0f;
    float worstGrain = 0.0f;
    for (const auto& base : bases) {
      for (const auto& colour : colours) {
        const Latent latent = lut.rgbToLatent(colour[0], colour[1], colour[2]);
        for (float m : masses) {
          // The solver's own fields for this texel: latent premultiplied by
          // mass, exactly as splat/transfer write them.
          const std::array<float, 4> depC = {latent.c[0] * m, latent.c[1] * m, latent.c[2] * m, m};
          const std::array<float, 4> depR = {latent.res[0] * m, latent.res[1] * m,
                                             latent.res[2] * m, 0.0f};
          const PigmentTexel baked = projectSolverTexel(depC, depR, kAbsorptionWatercolor);

          const std::array<float, 3> wet =
              screenRgb(latent, m, kAbsorptionWatercolor, 1.0f, base);
          const std::array<float, 3> dry = bakedRgb(baked, base);
          for (size_t i = 0; i < 3; ++i) worst = std::max(worst, std::fabs(wet[i] - dry[i]));

          // The stated cost: what the grain would have contributed, at the
          // extreme of the fibre range. Measured, not assumed.
          const std::array<float, 3> grained =
              screenRgb(latent, m, kAbsorptionWatercolor, 0.93f, base);
          for (size_t i = 0; i < 3; ++i)
            worstGrain = std::max(worstGrain, std::fabs(grained[i] - wet[i]));
        }
      }
    }
    std::printf("  [selftest] bake: 80 (base, colour, mass) cases -- worst channel difference "
                "between the solver's pixel and the baked tile's is %.3e\n",
                static_cast<double>(worst));
    check(worst < 1e-6f,
          "bake: the baked tile renders the SAME pixel as the solver did -- no flash");

    std::printf("  [selftest] bake: the paper grain the bake drops is worth up to %.4f per "
                "channel (fibre gain 0.93..1.00), which is a stated cost, not an error\n",
                static_cast<double>(worstGrain));
    check(worstGrain > 0.0f && worstGrain < 0.08f,
          "bake: and the dropped grain is bounded at 7% -- the cost the header names");
  }

  // ======================================================================
  // 5. The latent survives, and the round trip back to the solver.
  // ======================================================================
  std::printf("  -- 5. the latent, the epsilon, and re-wetting --\n");
  {
    MixboxLut lut;
    lut.load(NP_MIXBOX_LUT);
    const Latent latent = lut.rgbToLatent(0.2f, 0.55f, 0.35f);

    // The latent is mass-invariant: the same pigment at any load must project
    // to the same six numbers, because that is what "premultiplied by mass"
    // means and what the solver's advection relies on.
    float worstLatent = 0.0f;
    for (float m : {0.01f, 0.1f, 1.0f, 5.0f}) {
      const std::array<float, 4> depC = {latent.c[0] * m, latent.c[1] * m, latent.c[2] * m, m};
      const std::array<float, 4> depR = {latent.res[0] * m, latent.res[1] * m, latent.res[2] * m,
                                         0.0f};
      const PigmentTexel t = projectSolverTexel(depC, depR, kAbsorptionWatercolor);
      for (size_t i = 0; i < 3; ++i) {
        worstLatent = std::max(worstLatent, std::fabs(t.latent.c[i] - latent.c[i]));
        worstLatent = std::max(worstLatent, std::fabs(t.latent.res[i] - latent.res[i]));
      }
    }
    check(worstLatent < 1e-6f,
          "bake: the latent is mass-invariant -- the same pigment at any load");

    // A trace-mass texel: the guard keeps the latent finite, and the coverage
    // stays near zero rather than inheriting the guard's magnitude. Getting
    // this wrong would put opaque speckle along every soft stroke edge.
    const std::array<float, 4> traceC = {0.0f, 0.0f, 0.0f, 1e-9f};
    const PigmentTexel trace = projectSolverTexel(traceC, {0.0f, 0.0f, 0.0f, 0.0f},
                                                  kAbsorptionWatercolor);
    check(std::isfinite(trace.latent.c[0]) && trace.mass < 1e-6f,
          "bake: a trace-mass texel stays finite AND stays transparent");

    // Re-wetting, and the clamp that makes it total.
    float worstRound = 0.0f;
    for (float m : {0.01f, 0.05f, 0.2f, 0.5f, 1.0f, 2.0f}) {
      const float baked = bakedMassFromSim(m, kAbsorptionWatercolor);
      const float back = simMassFromBaked(baked, kAbsorptionWatercolor);
      worstRound = std::max(worstRound, std::fabs(back - m));
    }
    std::printf("  [selftest] bake: sim -> baked -> sim round trip, worst error %.3e over "
                "mass 0.01..2.0\n",
                static_cast<double>(worstRound));
    check(worstRound < 1e-4f, "bake: re-wetting recovers the solver mass it came from");
    check(std::isfinite(simMassFromBaked(1.0f, kAbsorptionWatercolor)),
          "bake: a fully opaque tile re-wets to a FINITE mass -- the clamp, doing its job");
    std::printf("  [selftest] bake: a saturated tile re-wets to sim mass %.3f (clamped at "
                "coverage %.6f, f16's own limit)\n",
                static_cast<double>(simMassFromBaked(1.0f, kAbsorptionWatercolor)),
                static_cast<double>(kMaxBakedMass));

    // f16 storage, which is what the tile actually holds.
    const std::array<float, 4> depC = {latent.c[0] * 0.4f, latent.c[1] * 0.4f, latent.c[2] * 0.4f,
                                       0.4f};
    const std::array<float, 4> depR = {latent.res[0] * 0.4f, latent.res[1] * 0.4f,
                                       latent.res[2] * 0.4f, 0.0f};
    const PigmentTexel baked = projectSolverTexel(depC, depR, kAbsorptionWatercolor);
    PigmentTile tile;
    tile.writeTexel(PixelCoord{0, 0}, baked);
    const PigmentTexel stored = tile.readTexel(PixelCoord{0, 0});
    float worstStore = std::fabs(stored.mass - baked.mass);
    for (size_t i = 0; i < 3; ++i) {
      worstStore = std::max(worstStore, std::fabs(stored.latent.c[i] - baked.latent.c[i]));
      worstStore = std::max(worstStore, std::fabs(stored.latent.res[i] - baked.latent.res[i]));
    }
    std::printf("  [selftest] bake: through PigmentTile's f16 storage, worst channel error "
                "%.3e (f16 gives ~4.9e-4 relative)\n",
                static_cast<double>(worstStore));
    check(worstStore < 1e-3f,
          "bake: the baked texel survives f16 -- the same rounding every tile write pays");
  }

  std::printf("[selftest] pigment bake %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
