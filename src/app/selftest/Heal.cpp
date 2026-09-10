#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "brush/CloneStamp.hpp"
#include "brush/Deposit.hpp"
#include "brush/Heal.hpp"
#include "core/SelectionShapes.hpp"
#include "ops/Poisson.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The Heal tool (PRD D6, PLAN.md Phase 8): the gradient-domain solve
// (`ops/Poisson`), the stroke that drives it (`brush/Heal`), and the routing
// and registration that make it reachable (`app/StrokeSession` §1c).
//
// See app/SelfTest.hpp for the section's contents list. Three things belong
// here because they are what the section is *for*:
//
//   * **A heal that is only a clone passes most tests a clone passes.** The two
//     tools share a source, a gesture, a composite and a footprint; the single
//     thing that separates them is what happens to the copied values on the
//     way. So the load-bearing assertions here are all *comparative* -- the
//     same fixture, the same offset, the same tip, run through both tools, with
//     the clone's answer printed beside the heal's. A section that only asserted
//     "the heal wrote something plausible" would stay green over an
//     implementation that had quietly become a second clone stamp.
//   * **Two of the claims are EXACT, and that is deliberate** (`ops/Poisson`
//     §1). Healing from a region identical to the destination changes nothing
//     bit for bit; healing from a region that differs by a pure constant
//     restores the destination bit for bit. Those are the two cases a converged
//     iterative solver would only *approach*, and asserting them at zero
//     tolerance is what stops the tool's central promise from becoming a claim
//     about how many cycles happened to run.
//   * **The solver is checked against analytic answers, not against a
//     fixture.** A linear ramp is harmonic, so the correction over a ramped rim
//     IS that ramp; and whatever the rim, the interior Laplacian of the answer
//     must be zero. Both are properties of the equation rather than of a
//     recorded output, which is the same standard `app/selftest/Flats.cpp`
//     holds `flats/Membrane` to with its strip and disc.
// ---------------------------------------------------------------------------
bool runHealTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  auto writeAt = [](TileStore& store, int32_t x, int32_t y, const std::array<float, 4>& t) {
    const PixelCoord p{x, y};
    store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), t);
  };

  // A hard disc, so `dabCoverage()` is exactly 1.0f across the whole disc and
  // the keep factor in the composite is exactly 0 -- which is what lets the
  // exactness claims below be about the SOLVE rather than about a falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.opacity = 1.0f;
    return t;
  };

  // **A horizontal ramp in exact sixteenths.** Every value is `1/4 + x/128`,
  // which is a dyadic rational small enough to be exact in binary16 for the
  // whole canvas -- so the fixture survives storage unchanged, the difference
  // between two columns is exact, and "bit for bit" below is a claim about the
  // heal rather than about rounding.
  //
  // A ramp and not a flat fill, because a flat fixture cannot tell a heal from
  // a smear, from a shift, or from doing nothing at all. A ramp and not the
  // clone section's per-column sawtooth, because a ramp has an *analytic*
  // property this tool is about: it is linear, so a shifted ramp differs from
  // the original by a constant, which is exactly the illumination difference a
  // heal must remove and a clone must not.
  auto rampValue = [](int32_t x) { return 0.25f + static_cast<float>(x) / 128.0f; };
  auto rampTexel = [&](int32_t x) {
    const float v = rampValue(x);
    return std::array<float, 4>{v, v, v, 1.0f};
  };
  auto fillRamp = [&](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) writeAt(store, x, y, rampTexel(x));
  };

  auto fillRect = [&](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      const std::array<float, 4>& t) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) writeAt(store, x, y, t);
  };

  auto makeRgbDoc = [](int32_t w, int32_t h) {
    return makeBlankOpenDocument(w, h, WorkingSpace{}, "heal");
  };

  std::printf("[selftest] heal (ops/Poisson, brush/Heal, app/StrokeSession §1c)\n");

  // ======================================================================
  // 0. The solver's own claims, on plain float grids (ops/Poisson)
  // ======================================================================
  //
  // No tiles, no dabs, no tool -- just the equation. Everything the tool
  // promises rests on these three, and each is checked against an answer that
  // can be written down rather than against a recorded output.
  {
    const int w = 41, h = 33;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    auto ringWrite = [&](std::vector<float>& u, const std::function<float(int, int)>& f) {
      for (int x = 0; x < w; ++x) {
        u[static_cast<size_t>(x)] = f(x, 0);
        u[static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
            f(x, h - 1);
      }
      for (int y = 1; y < h - 1; ++y) {
        u[static_cast<size_t>(y) * static_cast<size_t>(w)] = f(0, y);
        u[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(w - 1)] =
            f(w - 1, y);
      }
    };

    // (a) A constant rim, EXACTLY. `ops/Poisson` §1: a constant is harmonic, so
    // the constant IS the solution -- not something an iteration approaches.
    // 0.3f is deliberately NOT a dyadic rational; the claim is that the value
    // is copied, so a value that would survive any amount of arithmetic would
    // make the assertion weaker rather than stronger.
    {
      std::vector<float> u(n, 0.0f);
      ringWrite(u, [](int, int) { return 0.3f; });
      harmonicFill(u, w, h);
      bool exact = true;
      for (float v : u)
        if (v != 0.3f) exact = false;
      check(exact,
            "solve: a CONSTANT rim gives that constant on every interior cell, bit for bit -- "
            "a constant is harmonic, so this is the exact solution rather than a fast path, "
            "and it is what makes the tool's two exactness claims possible at all");
    }

    // (b) A linear ramp. A linear function has a five-point Laplacian of
    // exactly zero, so it is its own harmonic extension -- an analytic answer
    // over a rim that is not constant, which is the case the multigrid actually
    // has to run for.
    {
      std::vector<float> u(n, 0.0f);
      auto f = [](int x, int y) {
        return 0.5f + 0.01f * static_cast<float>(x) + 0.02f * static_cast<float>(y);
      };
      ringWrite(u, f);
      harmonicFill(u, w, h);
      double worst = 0.0;
      for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x)
          worst = std::max(worst, std::fabs(static_cast<double>(
                                      u[static_cast<size_t>(y) * static_cast<size_t>(w) +
                                        static_cast<size_t>(x)] -
                                      f(x, y))));
      std::printf("  [measured] ramped rim, %dx%d: worst interior error %.3e\n", w, h, worst);
      check(worst < 1.0e-3,
            "solve: a rim carrying a LINEAR RAMP is reproduced across the interior -- a linear "
            "function is harmonic, so this is an analytic answer and not a recorded one, and "
            "it is the case the multigrid has to converge on rather than recognise");
    }

    // (c) The defining property, over a rim that is neither constant nor
    // linear: the answer is harmonic. Checked one cell in from the rim, where
    // every neighbour of the tested cell is itself interior.
    {
      std::vector<float> u(n, 0.0f);
      ringWrite(u, [&](int x, int y) {
        return (x < w / 2 ? 1.0f : 0.0f) + 0.5f * static_cast<float>(y) / static_cast<float>(h);
      });
      harmonicFill(u, w, h);
      double worst = 0.0;
      for (int y = 2; y < h - 2; ++y)
        for (int x = 2; x < w - 2; ++x) {
          const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) +
                           static_cast<size_t>(x);
          const double lap = 4.0 * u[i] - (static_cast<double>(u[i - 1]) + u[i + 1] +
                                           u[i - static_cast<size_t>(w)] +
                                           u[i + static_cast<size_t>(w)]);
          worst = std::max(worst, std::fabs(lap));
        }
      std::printf("  [measured] stepped rim: worst interior |laplacian| %.3e\n", worst);
      check(worst < 1.0e-3,
            "solve: over a rim that is neither constant nor linear the interior comes back "
            "HARMONIC -- the five-point Laplacian is zero everywhere inside, which is the "
            "equation itself and not a property of any particular fixture");
    }
  }

  // ======================================================================
  // 1. healPatch(): the two exact cases, and the boundary (ops/Poisson §§0-1)
  // ======================================================================
  {
    const int w = 25, h = 21;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    // A texture with real structure in it, so "unchanged" and "correct" cannot
    // both be true of an implementation that returned its input.
    std::vector<std::array<float, 4>> dst(n);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const float v = 0.25f + static_cast<float>(((x * 5 + y * 3) % 9)) / 64.0f;
        dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = {
            v, v * 0.5f, v * 0.25f, 1.0f};
      }

    {
      const std::vector<std::array<float, 4>> out = healPatch(dst, dst, w, h);
      bool exact = true;
      for (size_t i = 0; i < n; ++i)
        if (out[i] != dst[i]) exact = false;
      check(exact,
            "patch: healing from a region IDENTICAL to the destination changes not one bit -- "
            "the rim of dst-src is all zeros, so the correction is exactly zero; a tool that "
            "perturbed the picture by a rounding error per dab would dirty tiles and re-upload "
            "textures through a drag that changed nothing");
    }

    {
      // The textbook case: the same material under different light. Every
      // component shifted by the SAME dyadic constant, so the subtraction that
      // builds the rim is exact and the claim is about the solve.
      std::vector<std::array<float, 4>> src(n);
      for (size_t i = 0; i < n; ++i)
        src[i] = {dst[i][0] + 0.125f, dst[i][1] + 0.125f, dst[i][2] + 0.125f, dst[i][3]};
      const std::vector<std::array<float, 4>> out = healPatch(src, dst, w, h);
      bool exact = true;
      for (size_t i = 0; i < n; ++i)
        if (out[i] != dst[i]) exact = false;
      check(exact,
            "patch: a source that differs by a pure CONSTANT heals back to the destination, "
            "bit for bit -- the one property that separates this tool from the clone stamp, "
            "asserted at zero tolerance rather than within a convergence bound");
    }

    {
      // A genuinely different source: a different texture AND a different mean.
      // Now the answer is not any of its inputs, and the two halves of the
      // gradient-domain promise can be separated.
      std::vector<std::array<float, 4>> src(n);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const float v = 0.75f + static_cast<float>(((x * 2 + y * 7) % 5)) / 64.0f;
          src[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = {
              v, v * 0.5f, v * 0.25f, 1.0f};
        }
      const std::vector<std::array<float, 4>> out = healPatch(src, dst, w, h);

      bool rimIsDst = true;
      for (int x = 0; x < w; ++x) {
        if (out[static_cast<size_t>(x)] != dst[static_cast<size_t>(x)]) rimIsDst = false;
        const size_t b = static_cast<size_t>(h - 1) * static_cast<size_t>(w) +
                         static_cast<size_t>(x);
        if (out[b] != dst[b]) rimIsDst = false;
      }
      check(rimIsDst,
            "patch: the border ring comes back as the DESTINATION, bit for bit -- it is the "
            "Dirichlet condition rather than a computed value, and a caller that compared it "
            "against dst to decide whether a texel needs writing would otherwise dirty a tile "
            "per dab over a patch the solve had nothing to say about");

      // The gradient-domain guarantee itself: the answer carries the SOURCE's
      // texture. `laplace(healed) == laplace(src)` inside, because
      // `healed = src + h` and `h` is harmonic.
      double worstLap = 0.0;
      for (int y = 2; y < h - 2; ++y)
        for (int x = 2; x < w - 2; ++x) {
          const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) +
                           static_cast<size_t>(x);
          const auto lap = [&](const std::vector<std::array<float, 4>>& a) {
            return 4.0 * a[i][0] - (static_cast<double>(a[i - 1][0]) + a[i + 1][0] +
                                    a[i - static_cast<size_t>(w)][0] +
                                    a[i + static_cast<size_t>(w)][0]);
          };
          worstLap = std::max(worstLap, std::fabs(lap(out) - lap(src)));
        }

      // And the other half: the answer sits at the DESTINATION's level, not the
      // source's. The source is 0.5 brighter; the seam a plain copy would leave
      // is what the heal has to close.
      const size_t centre = static_cast<size_t>(h / 2) * static_cast<size_t>(w) +
                            static_cast<size_t>(w / 2);
      const double healSeam = std::fabs(static_cast<double>(out[centre][0]) - dst[centre][0]);
      const double cloneSeam = std::fabs(static_cast<double>(src[centre][0]) - dst[centre][0]);
      std::printf("  [measured] different source: worst |lap(healed)-lap(src)| %.3e; centre "
                  "differs from dst by %.4f healed vs %.4f copied\n",
                  worstLap, healSeam, cloneSeam);
      check(worstLap < 1.0e-3,
            "patch: the healed patch has the SOURCE's Laplacian everywhere inside -- the "
            "texture is carried across unchanged, which is the half of the promise a clone "
            "also keeps");
      check(healSeam < cloneSeam / 4.0,
            "patch: and it sits at the DESTINATION's level -- the half a clone does NOT keep, "
            "measured with the copy's own error printed beside it so an implementation that "
            "had quietly become a second clone stamp could not pass");
    }

    {
      const std::vector<std::array<float, 4>> tiny(4, std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
      const std::vector<std::array<float, 4>> tinyDst(4, std::array<float, 4>{0.5f, 0.5f, 0.5f, 1.0f});
      const std::vector<std::array<float, 4>> out = healPatch(tiny, tinyDst, 2, 2);
      check(out == tinyDst,
            "patch: a patch with no interior at all (2x2 -- every cell is rim) comes back as "
            "the destination rather than as an error; a dab clipped to the very corner of a "
            "canvas is a legitimate input, not a bug");
    }
  }

  // ======================================================================
  // 2. healClampTexel(): what the solve may hand back, and what may be stored
  // ======================================================================
  //
  // The solve is a linear interpolation of differences and knows nothing about
  // `core::Tile`. It can return an alpha above 1 or a negative radiance, and
  // both would reach `core/Composite`'s accumulator unexamined.
  {
    check(healClampTexel({0.5f, 0.5f, 0.5f, 1.5f})[3] == 1.0f,
          "clamp: an alpha the solve pushed above 1 comes back at 1 -- coverage above full is "
          "not a measurement, and core/Composite reads it straight into its accumulator");
    const std::array<float, 4> negative = healClampTexel({-0.25f, 0.5f, -1.0f, 1.0f});
    check(negative[0] == 0.0f && negative[1] == 0.5f && negative[2] == 0.0f,
          "clamp: negative light is clamped to zero and positive light is left alone -- "
          "including above 1, because a working-space value over 1 IS a measurement and "
          "clamping it here would be a deposit loop deciding the build's dynamic range");
    check(healClampTexel({0.9f, 0.9f, 0.9f, -0.2f}) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "clamp: a texel driven to zero coverage carries no colour -- premultiplied storage "
          "means colour is already scaled by coverage, so leaving it behind would MANUFACTURE "
          "the malformed texel the clone stamp's own rule refuses to launder");
  }

  // ======================================================================
  // 3. One heal dab: the constant-offset case, with the clone measured beside
  // ======================================================================
  //
  // The whole tool in one gesture. The layer holds a linear ramp; the source is
  // that ramp read 24 texels to the left, which -- because a ramp is linear --
  // is the destination minus a constant. A HEAL must therefore leave the layer
  // bit-identical, and a CLONE must shift it by 24 texels' worth of ramp. Same
  // fixture, same offset, same tip: the only variable is the tool.
  {
    auto run = [&](bool healing) {
      OpenDocument od = makeRgbDoc(256, 256);
      TileStore& store = *od.document.layers[0].rgbTiles;
      fillRamp(store, 0, 0, 255, 255);
      DepositCount c;
      if (healing) {
        HealStroke s;
        s.begin(store, Vec2{-24.0f, 0.0f}, 1.0f, false);
        c = s.healDab(store, discTip(12.0f, 1.0f), Vec2{128.5f, 128.5f}, 256, 256, nullptr,
                      nullptr);
        s.end();
      } else {
        CloneStampStroke s;
        s.begin(store, Vec2{-24.0f, 0.0f}, 1.0f, false);
        c = s.cloneDab(store, discTip(12.0f, 1.0f), Vec2{128.5f, 128.5f}, 256, 256, nullptr,
                       nullptr);
        s.end();
      }
      int32_t unchanged = 0, moved = 0;
      for (int32_t y = 120; y <= 136; ++y)
        for (int32_t x = 120; x <= 136; ++x) {
          if (readAt(store, x, y) == rampTexel(x)) ++unchanged;
          else ++moved;
        }
      return std::tuple<size_t, int32_t, int32_t, float>{c.texels, unchanged, moved,
                                                         readAt(store, 128, 128)[0]};
    };
    const auto healed = run(true);
    const auto cloned = run(false);
    std::printf("  [measured] ramp, offset (-24,0): heal wrote %zu texels leaving %d/%d "
                "unchanged, centre %.6f; clone wrote %zu leaving %d/%d, centre %.6f (want "
                "%.6f healed, %.6f copied)\n",
                std::get<0>(healed), std::get<1>(healed),
                std::get<1>(healed) + std::get<2>(healed),
                static_cast<double>(std::get<3>(healed)), std::get<0>(cloned),
                std::get<1>(cloned), std::get<1>(cloned) + std::get<2>(cloned),
                static_cast<double>(std::get<3>(cloned)),
                static_cast<double>(rampValue(128)), static_cast<double>(rampValue(104)));
    check(std::get<0>(healed) > 0 && std::get<2>(healed) == 0,
          "dab: a heal whose source differs from the destination only by the ILLUMINATION "
          "leaves every texel it wrote bit-identical -- it found nothing to repair and said "
          "so by changing nothing, over a fixture where doing nothing is not the same as "
          "doing anything else");
    check(std::get<1>(cloned) == 0 && std::get<3>(cloned) == rampValue(104),
          "dab: while the CLONE stamp, on the identical fixture at the identical offset, moves "
          "every one of those texels -- the negative that makes the line above a claim about "
          "the solve rather than about a stroke that failed to run");
  }

  // ======================================================================
  // 4. The source comes from the PRE-STROKE SNAPSHOT (brush/Heal §2)
  // ======================================================================
  //
  // `brush/CloneStamp` §2's hazard, inherited: source and destination are two
  // windows onto one store. The proof here is direct rather than by symmetry --
  // the live store's SOURCE region is overwritten after `begin()` has taken the
  // snapshot, and the dab must be unaffected. An implementation that read
  // `store` instead of `source_` produces a different picture, and nothing in
  // that picture would say so.
  //
  // The source region sits 100 texels away from the dab, so it is outside the
  // patch entirely: the destination boundary IS read live and deliberately so
  // (§2), and overlapping the two would make this assertion measure that
  // instead.
  {
    auto run = [&](bool mutateAfterBegin) {
      OpenDocument od = makeRgbDoc(256, 256);
      TileStore& store = *od.document.layers[0].rgbTiles;
      fillRamp(store, 0, 0, 255, 255);
      fillRect(store, 0, 100, 60, 160, {0.75f, 0.25f, 0.5f, 1.0f});  // the source patch
      HealStroke s;
      s.begin(store, Vec2{-100.0f, 0.0f}, 1.0f, false);
      if (mutateAfterBegin) fillRect(store, 0, 100, 60, 160, {0.125f, 0.875f, 0.0625f, 1.0f});
      s.healDab(store, discTip(12.0f, 1.0f), Vec2{130.5f, 130.5f}, 256, 256, nullptr, nullptr);
      s.end();
      std::vector<std::array<float, 4>> out;
      for (int32_t y = 120; y <= 141; ++y)
        for (int32_t x = 120; x <= 141; ++x) out.push_back(readAt(store, x, y));
      return out;
    };
    const std::vector<std::array<float, 4>> quiet = run(false);
    const std::vector<std::array<float, 4>> disturbed = run(true);
    bool same = quiet.size() == disturbed.size();
    if (same)
      for (size_t i = 0; i < quiet.size(); ++i)
        if (quiet[i] != disturbed[i]) same = false;
    // The negative half: the dab has to have DONE something, or "identical" is
    // a statement about two untouched fixtures.
    bool changed = false;
    for (int32_t i = 0; i < static_cast<int32_t>(quiet.size()); ++i)
      if (quiet[static_cast<size_t>(i)] != rampTexel(120 + i % 22)) changed = true;
    check(same && changed,
          "snapshot: overwriting the SOURCE region in the live store after begin() cannot "
          "change what the dab lays down -- it samples the pre-stroke copy, which is what "
          "makes an overlapping source and destination produce one answer instead of one per "
          "iteration order");
  }

  // ======================================================================
  // 5. The selection bounds the heal, and does NOT bound the solve (PRD E1)
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRamp(store, 0, 0, 255, 255);
    fillRect(store, 0, 100, 60, 160, {0.75f, 0.25f, 0.5f, 1.0f});

    Selection sel;
    setSelectionToRect(sel, PixelRect{100, 100, 30, 60}, 256, 256);
    const std::array<float, 4> outsideBefore = readAt(store, 140, 130);

    HealStroke s;
    s.begin(store, Vec2{-100.0f, 0.0f}, 1.0f, false);
    const DepositCount c =
        s.healDab(store, discTip(20.0f, 1.0f), Vec2{125.5f, 130.5f}, 256, 256, &sel, nullptr);
    s.end();
    check(c.texels > 0 && readAt(store, 140, 130) == outsideBefore,
          "selection: a texel outside the marching ants is bit-identical after a dab that "
          "crossed it -- PRD E1's guarantee is about what is WRITTEN, and the ants gate the "
          "deposit exactly as they gate every other route");
    check(readAt(store, 115, 130) != rampTexel(115),
          "selection: while a texel inside them was healed -- the gate is a gate and not an "
          "off switch");
  }

  // ======================================================================
  // 6. Healing NOTHING costs nothing -- and healing FROM nothing does not
  // ======================================================================
  //
  // `brush/Heal` §4, the one place this tool deliberately parts company with
  // `brush/CloneStamp` §4. A clone whose source is empty writes nothing,
  // because at `src == 0` its composite is the destination bit for bit. A heal
  // whose source is empty has a rim of `dst - 0` and therefore a real
  // correction: it fills the hole smoothly, which is the correct
  // gradient-domain answer and is what a spot heal over featureless paint does.
  //
  // What survives is the cost claim, and it is the one that matters: blank
  // source over blank destination is a rim of exact zeros, so the correction is
  // exactly zero, the healed texel is four zeros and the composite skips it.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const size_t tilesBefore = store.occupiedTileCount();
    HealStroke s;
    s.begin(store, Vec2{-90.0f, -90.0f}, 1.0f, false);
    std::vector<Vec2> dabs;
    for (int i = 0; i < 40; ++i)
      dabs.push_back(Vec2{100.5f + static_cast<float>(i) * 2.0f, 128.5f});
    const StrokeDeposit d = s.healDabs(store, discTip(14.0f, 1.0f), dabs, 256, 256, nullptr);
    s.end();
    check(d.dabs == 40 && d.texels == 0 && d.tiles.empty() &&
              store.occupiedTileCount() == tilesBefore,
          "empty: forty dabs across blank canvas from a blank source allocate not one tile and "
          "report none -- the correction over an all-zero rim is exactly zero, so this is "
          "arithmetic rather than an optimisation");
  }
  {
    // And the divergence itself, asserted rather than left as prose: an empty
    // source INTO paint is an inpaint, not a no-op.
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 100, 100, 160, 160, {0.5f, 0.25f, 0.125f, 1.0f});
    HealStroke s;
    // A source 200 texels up: entirely off the painted rectangle and, for most
    // of the patch, off the canvas -- both read as four zeros.
    s.begin(store, Vec2{0.0f, -200.0f}, 1.0f, false);
    const DepositCount c =
        s.healDab(store, discTip(8.0f, 1.0f), Vec2{130.5f, 130.5f}, 256, 256, nullptr, nullptr);
    s.end();
    const std::array<float, 4> centre = readAt(store, 130, 130);
    std::printf("  [measured] heal from blank source into paint: %zu texels, centre "
                "(%.4f %.4f %.4f %.4f)\n",
                c.texels, static_cast<double>(centre[0]), static_cast<double>(centre[1]),
                static_cast<double>(centre[2]), static_cast<double>(centre[3]));
    check(c.texels > 0 && std::fabs(centre[0] - 0.5f) < 0.02f && centre[3] > 0.9f,
          "empty: but a heal from a BLANK source into surrounding paint fills the hole with "
          "that paint rather than doing nothing -- the deliberate divergence from the clone's "
          "rule, and the answer the equation actually gives");
  }

  // ======================================================================
  // 7. The per-stroke ceiling, and paper grain (brush/Heal, RgbDeposit §2)
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRamp(store, 0, 0, 255, 255);
    fillRect(store, 0, 100, 60, 160, {0.75f, 0.25f, 0.5f, 1.0f});
    HealStroke s;
    s.begin(store, Vec2{-100.0f, 0.0f}, 0.5f, false);
    std::vector<Vec2> dabs(30, Vec2{130.5f, 130.5f});  // scrubbed, same spot
    s.healDabs(store, discTip(10.0f, 0.5f), dabs, 256, 256, nullptr);
    const float reached = s.strokeAlphaAt(PixelCoord{130, 130});
    s.end();
    std::printf("  [measured] 30 dabs at opacity 0.5: strokeAlpha reached %.7f\n",
                static_cast<double>(reached));
    check(reached <= 0.5f && reached > 0.49f,
          "ceiling: opacity is a per-STROKE bound and not a per-dab multiplier -- thirty dabs "
          "scrubbed over one texel stop exactly at it, at zero tolerance on the accumulator "
          "rather than on the binary16 texel it produced");
  }
  {
    auto dabWithGrain = [&](bool grain) {
      OpenDocument od = makeRgbDoc(256, 256);
      TileStore& store = *od.document.layers[0].rgbTiles;
      fillRamp(store, 0, 0, 255, 255);
      fillRect(store, 0, 100, 60, 160, {0.75f, 0.25f, 0.5f, 1.0f});
      BrushTip t = discTip(20.0f, 1.0f);
      if (grain) {
        t.grain.enabled = true;
        t.grain.periodX = 24;
        t.grain.periodY = 24;
        t.grain.depth = 0.6f;
        t.grain.strength = 1.0f;
      }
      HealStroke s;
      s.begin(store, Vec2{-100.0f, 0.0f}, 1.0f, false);
      const DepositCount c =
          s.healDab(store, t, Vec2{130.5f, 130.5f}, 256, 256, nullptr, nullptr);
      s.end();
      // How many texels the dab left standing at the fixture's own value.
      // Grain lowers coverage rather than stopping a write, so the observable
      // is that the repair stops being complete wherever the tooth stands up.
      int32_t untouched = 0;
      for (int32_t y = 112; y <= 148; ++y)
        for (int32_t x = 112; x <= 148; ++x)
          if (readAt(store, x, y) == rampTexel(x)) ++untouched;
      return std::pair<size_t, int32_t>{c.texels, untouched};
    };
    const auto plain = dabWithGrain(false);
    const auto grained = dabWithGrain(true);
    std::printf("  [measured] one dab of %zu texels: %d left at the fixture value smooth, %d "
                "through paper tooth\n",
                plain.first, plain.second, grained.second);
    check(grained.second > plain.second,
          "grain: the paper tooth is applied on THIS route too -- `grainReachesRoute()` "
          "answers true for it, and this asserts the CALL is there rather than that a table "
          "says so, which is the failure that once left a working control greyed out");
  }

  // ======================================================================
  // 8. The routing table's Heal rows (app/StrokeSession §1c)
  // ======================================================================
  {
    Layer rgbLayer = makeRgbLayer("r");
    Layer pigment = makePigmentLayer("p");
    Layer lockedRgb = makeRgbLayer("lr");
    lockedRgb.locked = true;
    Layer alphaLockedRgb = makeRgbLayer("alr");
    alphaLockedRgb.alphaLocked = true;
    Layer storelessRgb = makeRgbLayer("sr");
    storelessRgb.rgbTiles.reset();
    Layer adjustment = makeAdjustmentLayer("adj");

    check(strokeRouteFor(Tool::Heal, &rgbLayer) == StrokeRoute::Heal &&
              strokeRouteFor(Tool::CloneStamp, &rgbLayer) == StrokeRoute::CloneStamp,
          "routing: the heal on a writable RGB layer takes its OWN route -- not the clone's, "
          "which is what a flag on that engine would have given it and what would have made "
          "the two indistinguishable to the per-frame re-validation");
    check(strokeRouteFor(Tool::Heal, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer REFUSES the heal by name while still taking the brush -- "
          "the correction is a Laplacian and a difference of latents, and this build has not "
          "decided what either means");
    check(strokeRouteFor(Tool::Heal, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: no layer at all is None for the heal and PaintSim for the brush -- the "
          "solver canvas is neither a source to sample nor a surround to solve against");
    check(strokeRouteFor(Tool::Heal, &lockedRgb) == StrokeRoute::None,
          "routing: a locked RGB layer refuses, checked before the kind so the message names "
          "the one thing a user can fix");
    check(strokeRouteFor(Tool::Heal, &alphaLockedRgb) == StrokeRoute::Heal &&
              strokeRouteFor(Tool::Eraser, &alphaLockedRgb) == StrokeRoute::None,
          "routing: an ALPHA-LOCKED layer still takes the heal while refusing the eraser -- "
          "the solve produces an alpha correction, but the composite is the colour-only form "
          "that copies dst[3] through, so the lock is honoured rather than made decorative");
    check(strokeRouteFor(Tool::Heal, &storelessRgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Heal, &adjustment) == StrokeRoute::None,
          "routing: an RGB layer with no store and an Adjustment layer each refuse -- neither "
          "holds texels to write");
    check(strokeRouteFor(Tool::Heal, &rgbLayer, LayerEditTarget::Mask) == StrokeRoute::None,
          "routing: and a MASK target refuses -- the solve is four channels of premultiplied "
          "colour and a mask sample is one scalar coverage with no privileged end");
    check(std::string(strokeRouteName(StrokeRoute::Heal)) == "heal" &&
              strokeRouteWritesLayer(StrokeRoute::Heal) &&
              grainReachesRoute(StrokeRoute::Heal) && !wetnessReachesSolver(StrokeRoute::Heal),
          "routing: the route has a name of its own, answers the predicate four call sites "
          "ask, and is a grain route rather than a solver one");
    check(std::string(strokeEditLabel(Tool::Heal)) == "heal" &&
              std::string(strokeEditLabel(Tool::CloneStamp)) == "clone stamp",
          "routing: and its history entry is its own noun -- two tools that share a source, a "
          "gesture and a composite must not share a row label, or the panel cannot tell a "
          "copy from a repair");
  }

  // ======================================================================
  // 9. The registration a new Tool value has to reach (docs/spec §2)
  // ======================================================================
  {
    check(toolImplemented(Tool::Heal) && toolHasCanvasHandler(Tool::Heal) &&
              toolBeginsStroke(Tool::Heal) && toolNoHandlerException(Tool::Heal) == nullptr,
          "chrome: the palette cell is live, and it is live through the same probe of "
          "strokeRouteFor() the canvas block is gated on -- no hand-written second table and "
          "no recorded exception");
    check(std::string(toolName(Tool::Heal)) == "Heal" &&
              toolShortcutLabel(Tool::Heal) == "J" && toolIconCodepoint(Tool::Heal) != 0u,
          "chrome: named, and carrying docs/shortcuts.md section 1's own reserved letter -- "
          "the row that reserved J had a name and no tool behind it");
    check(toolGroupIndex(Tool::Heal) == toolGroupIndex(Tool::CloneStamp) &&
              toolGroupIndex(Tool::Heal) >= 0 &&
              toolGroupDefaultMember(toolGroupIndex(Tool::Heal)) == Tool::CloneStamp,
          "chrome: it shares palette slot 7 with the Clone Stamp as a flyout sibling, and the "
          "cell still SHOWS the Clone Stamp -- a palette a user already knows must not change "
          "which icon a slot draws because a sibling arrived");
    check(cursorForTool(Tool::Heal) == cursorForTool(Tool::CloneStamp),
          "chrome: and the pointer is the paint cursor, derived beside the clone's rather "
          "than from a second opinion about which tools paint");
    check(toolUsesCloneSource(Tool::Heal) && toolUsesCloneSource(Tool::CloneStamp) &&
              !toolUsesCloneSource(Tool::Brush) && !toolUsesCloneSource(Tool::Smudge),
          "chrome: the Option+click source gesture is a PREDICATE both tools answer, not a "
          "list at the three call sites in ui/MacPaintUI that read it -- the anchoring gate, "
          "the offset latch and the source marker");
  }

  // ======================================================================
  // 10. The gesture end to end, through StrokeSession (app/StrokeSession §1c)
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRamp(store, 0, 0, 255, 255);
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;
    const std::array<float, 4> before = readAt(store, 130, 130);

    // The refusal first, and the sentence it prints.
    AppState::CloneSourceState clone;
    StrokeSession s0;
    std::string why;
    check(!s0.begin(od, 0, discTip(10.0f, 1.0f), Tool::Heal, &why, nullptr, DynamicInputs{},
                    &clone) &&
              contains(why, "heal") && contains(why, "no source set") &&
              contains(why, "Option-click") && !contains(why, "clone"),
          "refusal: a heal with no source refuses out loud and names ITSELF -- the anchor is "
          "shared with the clone stamp and the sentence must not be, or a user who picked "
          "Heal is told about a tool they did not choose");
    s0.addPoint(130.0f, 130.0f);
    s0.addPoint(140.0f, 130.0f);
    s0.end();
    check(readAt(store, 130, 130) == before && od.history.entries().size() == entries &&
              od.revision == rev && s0.texelsWritten() == 0,
          "refusal: and the layer, the revision and the history are all untouched -- a refusal "
          "that still recorded an undo step would be worse than the silent no-op it replaces");

    // Then the same anchor, set once, driving the heal.
    setCloneAnchor(clone, Vec2{30.0f, 130.0f});
    check(latchCloneOffset(clone, Vec2{130.0f, 130.0f}) && clone.offset.x == -100.0f,
          "gesture: one Option+click and one pen-down latch offset = anchor - penDown, the "
          "same two free functions the clone stamp drives -- one anchor, two tools");

    StrokeSession s;
    std::string e;
    check(s.begin(od, 0, discTip(10.0f, 1.0f), Tool::Heal, &e, nullptr, DynamicInputs{},
                  &clone) &&
              s.route() == StrokeRoute::Heal && e.empty(),
          "gesture: with a source set the same call BEGINS, and reports the heal route");
    check(s.healOffsetX() == -100 && s.healOffsetY() == 0 &&
              s.healSnapshotTiles() == store.occupiedTileCount() &&
              s.cloneSnapshotTiles() == 0,
          "gesture: the session latched the rounded offset into the HEAL engine and took its "
          "snapshot there -- with the clone engine left holding nothing, which is what each "
          "begin()'s else branch is for");
    s.addPoint(130.0f, 130.0f);
    s.addPoint(134.0f, 130.0f);
    s.addPoint(138.0f, 130.0f);
    s.end();
    std::printf("  [measured] heal stroke: %zu texels, %zu new entries, last \"%s\"\n",
                s.texelsWritten(), od.history.entries().size() - entries,
                od.history.entries().empty() ? "" : od.history.entries().back().label.c_str());
    check(s.texelsWritten() > 0 && od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "heal",
          "gesture: the whole stroke is exactly one history entry, labelled \"heal\"");
    check(s.healSnapshotTiles() == 0,
          "gesture: and the snapshot is dropped at pen-up -- it shares a tile with the layer "
          "for every tile the layer had, so holding one across an idle session would double "
          "the document");
  }

  // ======================================================================
  // 11. The offset is snapped to whole texels (brush/Heal §3)
  // ======================================================================
  {
    TileStore store;
    HealStroke s;
    s.begin(store, Vec2{-1.5f, 2.4f}, 1.0f, false);
    check(s.offsetX() == -2 && s.offsetY() == 2,
          "offset: rounded to the nearest whole texel, away from zero at the half -- a "
          "truncating cast is asymmetric about zero, and a fractional offset would change the "
          "source's own Laplacian, which is the thing being copied");
    s.end();
    check(s.snapshotTiles() == 0 && !s.active(),
          "offset: and end() drops the snapshot and the accumulator together");
  }

  std::printf("[selftest] heal %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
