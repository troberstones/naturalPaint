#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "ops/Transform.hpp"

namespace np {

// ops/Transform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14, D15,
// D16, D17). Pure CPU, no PaintSim and no GPU -- the same headless-first-class
// status runPointOpsTest(), runSelectionTest() and runShaperTest() have.
//
// Three of this section's claims cannot be made by a golden image, and they
// are the three the section exists for:
//
//   **D15 is a claim about bit equality.** A flip is a relabelling of the
//   pixel grid, so "close enough" is the exact implementation the requirement
//   forbids. Section 2 compares with memcmp at zero tolerance.
//
//   **D16 is a claim about a process, not a picture.** Three stacked
//   transforms composed into one matrix land in exactly the same place as
//   three applied in turn -- no geometric test can tell them apart. What
//   differs is how many times the reconstruction filter ran over the data, and
//   that only shows up as measured quality loss. Section 4 measures it.
//
//   **D17's downscale clause is a claim about what is NOT in the output.**
//   Aliased energy is indistinguishable from real content once folded, so the
//   test has to feed a pattern whose correct answer is known independently and
//   compare against it. Section 3 does, and runs the same input through the
//   deliberately-wrong path to report the ratio rather than asserting the
//   difference exists.
//
// Numbers printed by this section are measurements, not thresholds. Where a
// threshold appears it sits well clear of the measured value and the margin is
// stated at the assertion, so a run on different hardware fails for a real
// regression rather than for the last digit.
bool runTransformTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

  // A band-limited-ish test field with an opaque disc in it: enough detail for
  // a resample to lose, not so much that one pass destroys it and the
  // generational comparison in section 4 saturates.
  auto makeContent = [](uint32_t w, uint32_t h) {
    TransformImage img;
    img.width = w;
    img.height = h;
    img.px.assign(static_cast<size_t>(w) * h * 4u, 0.0f);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const float fx = static_cast<float>(x) / static_cast<float>(w);
        const float fy = static_cast<float>(y) / static_cast<float>(h);
        const float v = 0.5f + 0.25f * std::sin(fx * 18.0f) * std::cos(fy * 14.0f) +
                        0.15f * std::sin((fx + fy) * 31.0f);
        const float dx = fx - 0.5f, dy = fy - 0.5f;
        const float disc = std::sqrt(dx * dx + dy * dy) < 0.25f ? 1.0f : 0.0f;
        float* p = img.px.data() + (static_cast<size_t>(y) * w + x) * 4u;
        p[0] = v;
        p[1] = 0.5f * v + 0.4f * disc;
        p[2] = 1.0f - v;
        p[3] = 1.0f;
      }
    }
    return img;
  };

  // RMS error over an inner disc, so the source rectangle's own boundary --
  // which is a hard edge under this file's edge policy and is not what section
  // 4 is measuring -- cannot contribute.
  auto rmseInnerDisc = [](const TransformImage& a, const TransformImage& b, float radius) {
    const float cx = static_cast<float>(a.width) * 0.5f;
    const float cy = static_cast<float>(a.height) * 0.5f;
    double sum = 0.0;
    size_t n = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
      for (uint32_t x = 0; x < a.width; ++x) {
        const float dx = static_cast<float>(x) + 0.5f - cx;
        const float dy = static_cast<float>(y) + 0.5f - cy;
        if (dx * dx + dy * dy > radius * radius) continue;
        const float* pa = a.px.data() + (static_cast<size_t>(y) * a.width + x) * 4u;
        const float* pb = b.px.data() + (static_cast<size_t>(y) * b.width + x) * 4u;
        for (int c = 0; c < 3; ++c) {
          const double d = static_cast<double>(pa[c]) - static_cast<double>(pb[c]);
          sum += d * d;
          ++n;
        }
      }
    }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
  };

  auto bitIdentical = [](const TransformImage& a, const TransformImage& b) {
    return a.width == b.width && a.height == b.height && a.px.size() == b.px.size() &&
           std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(float)) == 0;
  };

  std::string err;

  // --- 1. The matrix, and the composition order everyone gets backwards ---
  {
    const Mat3 t = transformTranslate(10.0f, -4.0f);
    const Mat3 s = transformScale(2.0f, 3.0f);

    // `mat3Multiply(a, b)` applies b FIRST. The whole D16 guarantee is a claim
    // about a product of matrices, so an order convention that is only
    // documented and never checked is a guarantee about the wrong picture.
    const Point2 viaMatrix = mat3MapPoint(mat3Multiply(t, s), Point2{1.0f, 1.0f});
    const Point2 byHand = mat3MapPoint(t, mat3MapPoint(s, Point2{1.0f, 1.0f}));
    check(viaMatrix.x == byHand.x && viaMatrix.y == byHand.y && viaMatrix.x == 12.0f &&
              viaMatrix.y == -1.0f,
          "transform: mat3Multiply(a, b) applies b FIRST -- scale then translate, not the "
          "other way, or every composed stack is wrong by a translation");

    TransformStack stack;
    stack.push(s);
    stack.push(t);
    const Point2 viaStack = mat3MapPoint(stack.composed(), Point2{1.0f, 1.0f});
    check(viaStack.x == byHand.x && viaStack.y == byHand.y,
          "transform: a stack folds so the FIRST pushed transform is applied first -- the "
          "order the user built it in, not the order the matrices multiply in");
    check(stack.size() == 2, "transform: and pushing a transform costs a matrix, not a resample");

    TransformStack emptyStack;
    const Point2 viaEmpty = mat3MapPoint(emptyStack.composed(), Point2{7.0f, 9.0f});
    check(viaEmpty.x == 7.0f && viaEmpty.y == 9.0f,
          "transform: an empty stack composes to the IDENTITY -- a transform tool nobody has "
          "dragged yet is a no-op, not an error");

    Mat3 inv;
    const Mat3 tricky = mat3Multiply(transformRotateDegreesAbout(37.0f, Point2{20.0f, 11.0f}),
                                     transformSkewDegrees(12.0f, -5.0f));
    check(mat3Invert(tricky, &inv), "transform: a rotate-and-skew is invertible");
    const Point2 rt = mat3MapPoint(inv, mat3MapPoint(tricky, Point2{33.0f, -8.0f}));
    check(near(rt.x, 33.0, 1e-3) && near(rt.y, -8.0, 1e-3),
          "transform: and its inverse round-trips a point -- the resampler reads through this "
          "inverse, so an error here is a whole image in the wrong place");

    Mat3 dummy;
    check(!mat3Invert(transformScale(1.0f, 0.0f), &dummy),
          "transform: a zero scale on one axis is REFUSED as non-invertible, not divided by");
  }

  // --- 2. PRD D15: flips and quarter turns are EXACT ----------------------
  //
  // Deliberately run with Lanczos3 selected. If the exact path ever stops
  // firing, the section does not fall back to something that nearly works -- it
  // falls back to the widest, ringiest kernel in the build, and the memcmp
  // below fails loudly instead of by a few ulps.
  {
    const TransformImage img = makeContent(97, 61);
    TransformParams p;
    p.kernel = ResampleKernel::Lanczos3;

    TransformImage flipped, back;
    TransformReport r1{}, r2{};
    check(transformImage(img, transformFlipHorizontal(97), 97, 61, p, &flipped, &r1, &err),
          "transform: a horizontal flip runs");
    check(r1.exact == ExactRemap::FlipHorizontal && r1.reconstructionPasses == 0,
          "transform: a flip is classified EXACT and runs ZERO reconstruction passes -- PRD "
          "D15 is a claim about the code path, not only about the pixels");
    check(transformImage(flipped, transformFlipHorizontal(97), 97, 61, p, &back, &r2, &err) &&
              bitIdentical(img, back),
          "transform: flip, flip back, BIT-IDENTICAL to the original -- a tolerance here would "
          "pass on exactly the filtered implementation D15 forbids");

    // The corner texel really did move, so the memcmp above is not passing on
    // an accidental no-op.
    check(flipped.px[0] == img.px[(96) * 4u] && flipped.px[0] != img.px[0],
          "transform: and the flip actually moved the pixels -- destination texel 0 is source "
          "texel 96, not source texel 0");

    TransformImage vflip, vback;
    TransformReport r3{};
    check(transformImage(img, transformFlipVertical(61), 97, 61, p, &vflip, &r3, &err) &&
              r3.exact == ExactRemap::FlipVertical &&
              transformImage(vflip, transformFlipVertical(61), 97, 61, p, &vback, nullptr, &err) &&
              bitIdentical(img, vback),
          "transform: a vertical flip is exact and bit-reversible on a NON-square image, where "
          "an axis mix-up would otherwise cancel out");

    // Four quarter turns, through the 61x97 intermediate extent.
    TransformImage cur = img;
    uint32_t w = 97, h = 61;
    bool everyTurnExact = true;
    for (int i = 0; i < 4; ++i) {
      TransformImage next;
      TransformReport r{};
      if (!transformImage(cur, transformRotate90(1, w, h), h, w, p, &next, &r, &err) ||
          r.reconstructionPasses != 0 || r.exact != ExactRemap::Rotate90) {
        everyTurnExact = false;
      }
      cur = next;
      std::swap(w, h);
    }
    check(everyTurnExact && w == 97 && h == 61 && bitIdentical(img, cur),
          "transform: four 90-degree turns of a 97x61 image return it BIT-IDENTICAL, through "
          "two transposed extents -- the odd-turn extent is where an off-by-one hides");

    // The snap that makes the exact path reachable from a user-facing angle.
    check(exactRemapKind(transformRotateDegrees(90.0f)) == ExactRemap::Rotate90,
          "transform: an angle of exactly 90 degrees SNAPS to the exact matrix -- cosf(pi/2) is "
          "-4.4e-8, so a trig-built quarter turn would silently resample");
    check(exactRemapKind(transformRotateDegrees(90.0001f)) == ExactRemap::None,
          "transform: and 90.0001 degrees does NOT snap -- the exact path is for transforms "
          "that are exact, not for ones that are nearly exact");

    // Composition must stay on the exact path, or PRD D16 and PRD D15 fight
    // each other: stacking two exact operations would produce an inexact one.
    check(exactRemapKind(mat3Multiply(transformFlipHorizontal(61),
                                      transformRotate90(1, 97, 61))) == ExactRemap::Transpose,
          "transform: flip composed with a quarter turn is STILL exact (a transpose) -- two "
          "lossless edits must not stack into a lossy one");

    // And the escape hatch used by section 3 and 4 really does bypass it, so
    // those sections are measuring the general path and not this one.
    TransformParams noExact = p;
    noExact.allowExactPaths = false;
    TransformImage forced;
    TransformReport r4{};
    check(transformImage(img, transformFlipHorizontal(97), 97, 61, noExact, &forced, &r4, &err) &&
              r4.reconstructionPasses == 1 && !bitIdentical(img, forced),
          "transform: with exact paths disabled the same flip DOES resample -- which is how the "
          "sections below can measure the wrong path instead of asserting it is worse");
  }

  // --- 3. PRD D17 / PLAN.md's trap: a downscale must prefilter -------------
  //
  // The input is one-pixel-wide vertical stripes: the highest spatial
  // frequency a raster can carry, and the pattern that aliases catastrophically
  // when point-sampled at a coarser grid.
  //
  // The reduction is 256 -> 35, deliberately NOT a power of two. An integer 8x
  // reduction of a period-2 pattern is measured below too and it is the reason
  // this section does not use one: every destination sample lands on the same
  // phase, so the naive path returns a constant 0.5 and *accidentally passes*.
  // A test that only checked 256 -> 32 would certify an unfiltered resampler.
  {
    auto stripes = [](uint32_t n, uint32_t period) {
      TransformImage img;
      img.width = n;
      img.height = n;
      img.px.assign(static_cast<size_t>(n) * n * 4u, 0.0f);
      for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
          const float v = (x % period) == 0 ? 1.0f : 0.0f;
          float* p = img.px.data() + (static_cast<size_t>(y) * n + x) * 4u;
          p[0] = p[1] = p[2] = v;
          p[3] = 1.0f;
        }
      }
      return img;
    };
    auto stats = [](const TransformImage& img, double* meanOut, double* sdOut, double* minOut) {
      double mean = 0.0, mn = 1e30;
      const size_t n = static_cast<size_t>(img.width) * img.height;
      for (size_t i = 0; i < n; ++i) {
        const double v = img.px[i * 4u];
        mean += v;
        mn = std::min(mn, v);
      }
      mean /= static_cast<double>(n);
      double var = 0.0;
      for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(img.px[i * 4u]) - mean;
        var += d * d;
      }
      *meanOut = mean;
      *sdOut = std::sqrt(var / static_cast<double>(n));
      *minOut = mn;
    };

    const TransformImage src = stripes(256, 2);
    TransformParams good;
    good.kernel = ResampleKernel::CatmullRom;
    TransformParams bad = good;
    bad.prefilterDownscale = false;

    TransformImage filtered, naive;
    TransformReport rf{}, rn{};
    check(resizeImage(src, 35, 35, good, &filtered, &rf, &err) &&
              resizeImage(src, 35, 35, bad, &naive, &rn, &err),
          "transform: a 256 -> 35 reduction of 1-pixel stripes runs both ways");

    double fMean = 0, fSd = 0, fMin = 0, nMean = 0, nSd = 0, nMin = 0;
    stats(filtered, &fMean, &fSd, &fMin);
    stats(naive, &nMean, &nSd, &nMin);
    std::printf("  [selftest] transform: 1px stripes 256->35, true mean 0.5 -- prefiltered "
                "mean %.6f sd %.6f min %.4f ; unfiltered mean %.6f sd %.6f min %.4f (sd ratio "
                "%.2fx)\n",
                fMean, fSd, fMin, nMean, nSd, nMin, nSd / fSd);

    // Both paths preserve the mean, because a box and a cubic both have unit
    // DC gain. The mean is not the claim. The claim is that the STRUCTURE the
    // output carries is the source's and not an artefact of the sample grid,
    // and that is what the deviation measures.
    check(near(fMean, 0.5, 0.01),
          "transform: a prefiltered reduction of 50%-duty stripes averages to 0.5 -- the DC "
          "level is right, which is the part that is right either way");
    check(fSd < 0.08,
          "transform: and it lands within 0.08 of flat everywhere -- 1-pixel stripes reduced "
          "7.3x carry no structure a 35-pixel image can represent, so a flat field IS the "
          "correct answer");
    check(nSd > 0.25,
          "transform: while the SAME reduction with the prefilter off swings over 0.25 -- that "
          "is moire, invented by the sample grid, and no reconstruction filter removes it "
          "afterwards because it is now indistinguishable from real content");
    check(nSd / fSd > 5.0,
          "transform: the unfiltered path is more than 5x noisier by deviation (measured 10.1x "
          "at time of writing) -- PLAN.md names this as one of the two most commonly botched "
          "operations in the phase");

    // The degenerate ratio, measured so the choice of 35 is visible rather
    // than arbitrary.
    TransformImage p2f, p2n;
    resizeImage(src, 32, 32, good, &p2f, nullptr, &err);
    resizeImage(src, 32, 32, bad, &p2n, nullptr, &err);
    double aMean = 0, aSd = 0, aMin = 0, bMean = 0, bSd = 0, bMin = 0;
    stats(p2f, &aMean, &aSd, &aMin);
    stats(p2n, &bMean, &bSd, &bMin);
    std::printf("  [selftest] transform: the SAME stripes at an exact 8x (256->32) give sd "
                "%.6f filtered and %.6f unfiltered -- every sample lands on one phase, so the "
                "broken path passes; this is why the assertions above use 35, not 32\n",
                aSd, bSd);

    // A pattern whose period does not divide the reduction: here the naive
    // path gets the DC level wrong as well, and produces negative light.
    const TransformImage p3 = stripes(256, 3);
    TransformImage p3f, p3n;
    resizeImage(p3, 32, 32, good, &p3f, nullptr, &err);
    resizeImage(p3, 32, 32, bad, &p3n, nullptr, &err);
    stats(p3f, &aMean, &aSd, &aMin);
    stats(p3n, &bMean, &bSd, &bMin);
    std::printf("  [selftest] transform: period-3 stripes 256->32, true mean %.6f -- "
                "prefiltered %.6f (min %.4f), unfiltered %.6f (min %.4f)\n",
                86.0 / 256.0, aMean, aMin, bMean, bMin);
    check(near(aMean, 86.0 / 256.0, 1e-3) && !near(bMean, 86.0 / 256.0, 1e-3),
          "transform: on a period-3 pattern the prefiltered reduction keeps the true average "
          "and the unfiltered one does NOT -- aliasing moves energy, it does not only "
          "rearrange it");
    check(aMin >= 0.0f && bMin < 0.0f,
          "transform: and the unfiltered path produces NEGATIVE linear light from Catmull-Rom's "
          "lobes ringing on aliased edges, which the prefiltered one never reaches");

    // The other half of D17's clause: a pure downscale should not pay a
    // reconstruction pass at all, because the prefilter already landed on the
    // destination extent.
    check(rf.prefiltered && rf.prefilterWidth == 35 && rf.prefilterHeight == 35 &&
              rf.reconstructionPasses == 0,
          "transform: a pure downscale prefilters straight onto the destination extent and "
          "then runs ZERO reconstruction passes -- the area average IS the answer, and "
          "filtering it again would only soften it");
    check(!rn.prefiltered && rn.reconstructionPasses == 1,
          "transform: while the deliberately-wrong path skipped the prefilter, which is what "
          "the comparison above depends on");

    // A downscale must equal a hand-computed PREMULTIPLIED box average. This
    // is where straight-alpha filtering would show up as a wrong number rather
    // than as a fringe somebody has to notice.
    TransformImage varied;
    varied.width = 64;
    varied.height = 64;
    varied.px.assign(64u * 64u * 4u, 0.0f);
    for (uint32_t y = 0; y < 64; ++y) {
      for (uint32_t x = 0; x < 64; ++x) {
        const float a = static_cast<float>((x * 7 + y * 13) % 100) / 99.0f;
        float* p = varied.px.data() + (static_cast<size_t>(y) * 64 + x) * 4u;
        p[0] = (static_cast<float>((x * 3) % 17) / 16.0f) * a;
        p[1] = (static_cast<float>((y * 5) % 11) / 10.0f) * a;
        p[2] = (static_cast<float>((x + y) % 23) / 22.0f) * a;
        p[3] = a;
      }
    }
    TransformImage reduced;
    resizeImage(varied, 16, 16, good, &reduced, nullptr, &err);
    double worstBox = 0.0;
    for (uint32_t j = 0; j < 16; ++j) {
      for (uint32_t i = 0; i < 16; ++i) {
        double acc[4] = {0.0, 0.0, 0.0, 0.0};
        for (uint32_t dy = 0; dy < 4; ++dy) {
          for (uint32_t dx = 0; dx < 4; ++dx) {
            const float* s =
                varied.px.data() + (static_cast<size_t>(j * 4 + dy) * 64 + (i * 4 + dx)) * 4u;
            for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>(s[c]);
          }
        }
        const float* d = reduced.px.data() + (static_cast<size_t>(j) * 16 + i) * 4u;
        for (int c = 0; c < 4; ++c)
          worstBox = std::max(worstBox, std::fabs(acc[c] / 16.0 - static_cast<double>(d[c])));
      }
    }
    std::printf("  [selftest] transform: exact 4x reduction vs a hand-computed PREMULTIPLIED "
                "4x4 box average -- max abs error %.4g\n",
                worstBox);
    check(worstBox < 1e-6,
          "transform: an exact 4x reduction equals the hand-computed PREMULTIPLIED box average "
          "to under 1e-6 (measured 6.3e-8, the cost of the un-premultiply round trip through "
          "ops/Resample's straight-alpha interface) -- filtering straight alpha instead would "
          "miss by far more than the tolerance");
  }

  // --- 4. PRD D16: compose once, resample once ----------------------------
  //
  // Both paths end at the same geometry, so this cannot be a comparison of
  // pictures against each other -- it is a comparison of each against the
  // ORIGINAL. Rotate by 60 degrees and rotate back: the composed path spends
  // two resamples doing it, the stacked path spends four for the same
  // geometry. Whatever survives is the measure.
  {
    const TransformImage img = makeContent(256, 256);
    const Point2 centre{128.0f, 128.0f};
    const Mat3 step = transformRotateDegreesAbout(20.0f, centre);

    TransformStack stack;
    stack.push(step);
    stack.push(step);
    stack.push(step);
    const Mat3 composed = stack.composed();
    Mat3 undo;
    check(mat3Invert(composed, &undo), "transform: the composed 60-degree rotation inverts");

    // First: the two routes agree on WHERE the pixels go. Without this the
    // quality comparison below would be measuring a bug, not a design.
    double worstGeom = 0.0;
    for (float x : {0.0f, 60.0f, 128.0f, 200.0f, 256.0f}) {
      for (float y : {0.0f, 60.0f, 128.0f, 200.0f, 256.0f}) {
        const Point2 a = mat3MapPoint(composed, Point2{x, y});
        const Point2 b =
            mat3MapPoint(step, mat3MapPoint(step, mat3MapPoint(step, Point2{x, y})));
        worstGeom = std::max(worstGeom, static_cast<double>(std::max(std::fabs(a.x - b.x),
                                                                     std::fabs(a.y - b.y))));
      }
    }
    std::printf("  [selftest] transform: composed vs successive geometry -- max corner "
                "disagreement %.4g px over a 256px canvas\n",
                worstGeom);
    check(worstGeom < 1e-3,
          "transform: composing three rotations lands the corners within 1e-3 px of applying "
          "them in turn (measured 3.1e-5) -- this is why no geometric test can catch the wrong "
          "design, and why the assertion below has to be a quality measurement");

    TransformParams p;
    p.kernel = ResampleKernel::CatmullRom;
    TransformImage a1, aOut;
    transformImage(img, composed, 256, 256, p, &a1, nullptr, &err);
    transformImage(a1, undo, 256, 256, p, &aOut, nullptr, &err);

    TransformImage b1, b2, b3, bOut;
    transformImage(img, step, 256, 256, p, &b1, nullptr, &err);
    transformImage(b1, step, 256, 256, p, &b2, nullptr, &err);
    transformImage(b2, step, 256, 256, p, &b3, nullptr, &err);
    transformImage(b3, undo, 256, 256, p, &bOut, nullptr, &err);

    const double composedErr = rmseInnerDisc(img, aOut, 100.0f);
    const double stackedErr = rmseInnerDisc(img, bOut, 100.0f);
    std::printf("  [selftest] transform: 3x20-degree round trip, Catmull-Rom -- composed "
                "(2 resamples) rmse %.6f, successive (4 resamples) rmse %.6f, ratio %.4f\n",
                composedErr, stackedErr, stackedErr / composedErr);
    check(composedErr > 0.0 && stackedErr > composedErr,
          "transform: composing the stack loses LESS than applying it step by step -- same "
          "geometry, fewer generations");
    check(stackedErr / composedErr > 1.15,
          "transform: and by more than 15% RMS (measured 1.46x) -- a design that resampled per "
          "transform would be wrong even though it looks right, because the loss is monotonic, "
          "invisible per step and permanent");

    // Deeper stacks lose more, which is the part that makes it a defect rather
    // than a constant: the damage grows with how much the user edits.
    const Mat3 fine = transformRotateDegreesAbout(7.5f, centre);
    TransformStack deep;
    for (int i = 0; i < 8; ++i) deep.push(fine);
    const Mat3 deepComposed = deep.composed();
    Mat3 deepUndo;
    mat3Invert(deepComposed, &deepUndo);
    TransformImage d1, dOut;
    transformImage(img, deepComposed, 256, 256, p, &d1, nullptr, &err);
    transformImage(d1, deepUndo, 256, 256, p, &dOut, nullptr, &err);
    TransformImage cur = img;
    for (int i = 0; i < 8; ++i) {
      TransformImage next;
      transformImage(cur, fine, 256, 256, p, &next, nullptr, &err);
      cur = next;
    }
    TransformImage eOut;
    transformImage(cur, deepUndo, 256, 256, p, &eOut, nullptr, &err);
    const double deepComposedErr = rmseInnerDisc(img, dOut, 100.0f);
    const double deepStackedErr = rmseInnerDisc(img, eOut, 100.0f);
    std::printf("  [selftest] transform: 8x7.5-degree round trip -- composed (2 resamples) "
                "rmse %.6f, successive (9 resamples) rmse %.6f, ratio %.4f\n",
                deepComposedErr, deepStackedErr, deepStackedErr / deepComposedErr);
    check(deepStackedErr / deepComposedErr > stackedErr / composedErr,
          "transform: an 8-deep stack loses proportionally MORE than a 3-deep one (measured "
          "1.87x against 1.46x) while the composed cost stays at two resamples -- the damage "
          "grows with how much the user edits, which is what makes it a defect");
    check(near(deepComposedErr, composedErr, 1e-4),
          "transform: while the composed cost is the SAME for eight transforms as for three -- "
          "PRD D16's 'resample once, from the original pixels' is a flat cost, not a smaller "
          "growing one");

    // Every general-path call must resample exactly once. If this ever reads
    // more, something inside grew a second pass and the whole guarantee is
    // gone regardless of what the matrices did.
    TransformReport onePass{};
    transformImage(img, composed, 256, 256, p, &a1, &onePass, &err);
    check(onePass.reconstructionPasses == 1,
          "transform: and ONE transformImage() call runs exactly ONE reconstruction pass -- the "
          "prefilter and the reconstruction are two halves of one generation, not two");
  }

  // --- 5. The five kernels -------------------------------------------------
  {
    // A constant opaque field must survive every kernel, over the WHOLE image
    // including the border. This is the assertion that caught the first
    // version of the edge policy: letting off-image taps contribute
    // transparent black at full weight left a fully opaque field at alpha
    // 0.944 around the edge under Lanczos3 -- an Image Size that quietly makes
    // an opaque picture translucent.
    TransformImage flat;
    flat.width = 8;
    flat.height = 8;
    flat.px.assign(8u * 8u * 4u, 0.0f);
    for (uint32_t i = 0; i < 64; ++i) {
      float* p = flat.px.data() + static_cast<size_t>(i) * 4u;
      p[0] = 0.6f;
      p[1] = 0.3f;
      p[2] = 0.1f;
      p[3] = 1.0f;
    }
    bool allFlat = true;
    double worstAlpha = 0.0;
    for (ResampleKernel k : {ResampleKernel::Nearest, ResampleKernel::Bilinear,
                             ResampleKernel::CatmullRom, ResampleKernel::Mitchell,
                             ResampleKernel::Lanczos3}) {
      TransformParams p;
      p.kernel = k;
      TransformImage up;
      if (!resizeImage(flat, 21, 21, p, &up, nullptr, &err)) {
        allFlat = false;
        continue;
      }
      for (uint32_t i = 0; i < 21u * 21u; ++i) {
        const float* q = up.px.data() + static_cast<size_t>(i) * 4u;
        worstAlpha = std::max(worstAlpha, std::fabs(static_cast<double>(q[3]) - 1.0));
        if (std::fabs(static_cast<double>(q[0]) - 0.6) > 1e-5) allFlat = false;
      }
    }
    std::printf("  [selftest] transform: constant opaque field upscaled 8->21 through all five "
                "kernels -- worst |alpha - 1| anywhere including the border: %.3g\n",
                worstAlpha);
    check(allFlat && worstAlpha < 1e-5,
          "transform: a constant OPAQUE field stays constant and opaque under every kernel, "
          "border texels included (measured 3.6e-7) -- an edge policy that fades instead is a "
          "resize that makes an opaque image translucent");

    // Nearest must be a pure pick: every output value is a source value, byte
    // for byte, or it is not nearest neighbour.
    TransformImage ramp;
    ramp.width = 4;
    ramp.height = 1;
    ramp.px = {0.1f, 0, 0, 1, 0.2f, 0, 0, 1, 0.3f, 0, 0, 1, 0.4f, 0, 0, 1};
    TransformParams np_;
    np_.kernel = ResampleKernel::Nearest;
    TransformImage doubled;
    resizeImage(ramp, 8, 1, np_, &doubled, nullptr, &err);
    bool pureDouble = true;
    for (uint32_t i = 0; i < 8; ++i) {
      if (doubled.px[static_cast<size_t>(i) * 4u] != ramp.px[static_cast<size_t>(i / 2) * 4u])
        pureDouble = false;
    }
    check(pureDouble,
          "transform: nearest doubles a 4-texel ramp into exact pairs of the SAME float values "
          "-- nearest that interpolates anything is not nearest, and it is the only kernel that "
          "can be trusted with indexed-like content");

    // The kernels are the ones PLAN.md names, at the radii that make them
    // those kernels. A Catmull-Rom evaluated at radius 1 is a bilinear filter
    // wearing its name.
    check(resampleKernelRadius(ResampleKernel::Nearest) == 0.5f &&
              resampleKernelRadius(ResampleKernel::Bilinear) == 1.0f &&
              resampleKernelRadius(ResampleKernel::CatmullRom) == 2.0f &&
              resampleKernelRadius(ResampleKernel::Mitchell) == 2.0f &&
              resampleKernelRadius(ResampleKernel::Lanczos3) == 3.0f,
          "transform: the five kernels carry their defining radii (0.5, 1, 2, 2, 3) -- a cubic "
          "truncated to radius 1 is a tent filter under a better name");
    check(resampleKernelWeight(ResampleKernel::CatmullRom, 0.0f) == 1.0f &&
              near(resampleKernelWeight(ResampleKernel::CatmullRom, 1.0f), 0.0, 1e-6) &&
              near(resampleKernelWeight(ResampleKernel::CatmullRom, 2.0f), 0.0, 1e-6),
          "transform: Catmull-Rom is INTERPOLATING -- 1 at the sample and 0 at every other "
          "integer offset, so it passes source values through unchanged");
    check(resampleKernelWeight(ResampleKernel::Mitchell, 0.0f) < 1.0f &&
              resampleKernelWeight(ResampleKernel::Mitchell, 0.0f) > 0.7f,
          "transform: Mitchell is APPROXIMATING -- its centre weight is below 1, which is the "
          "trade it makes for less ringing and the reason the two cubics are both offered");
    check(resampleKernelWeight(ResampleKernel::CatmullRom, 1.5f) < 0.0f &&
              resampleKernelWeight(ResampleKernel::Lanczos3, 1.5f) < 0.0f &&
              resampleKernelWeight(ResampleKernel::Bilinear, 0.5f) > 0.0f,
          "transform: the cubics and Lanczos have NEGATIVE lobes and bilinear does not -- that "
          "is what makes them sharper, and what makes them able to invent negative linear "
          "light next to a highlight");
  }

  // --- 6. Premultiplied, not straight -- the classic fringe bug ------------
  {
    // Under premultiplied alpha a fully transparent texel carries no colour at
    // all, so it contributes nothing to an average. Under straight alpha it
    // carries whatever arbitrary RGB happened to be stored behind alpha 0, and
    // that bleeds out as a halo. The two answers differ by an entire channel.
    TransformImage pair;
    pair.width = 2;
    pair.height = 1;
    pair.px = {0.0f, 0.0f, 0.0f, 0.0f,   // transparent -- premultiplied, so no colour
               1.0f, 0.0f, 0.0f, 1.0f};  // opaque red
    TransformParams p;
    p.kernel = ResampleKernel::Bilinear;
    TransformImage halved;
    check(resizeImage(pair, 1, 1, p, &halved, nullptr, &err),
          "transform: a transparent texel beside an opaque one halves");
    std::printf("  [selftest] transform: transparent + opaque red, halved -> (%.4f, %.4f, "
                "%.4f) at alpha %.4f\n",
                halved.px[0], halved.px[1], halved.px[2], halved.px[3]);
    check(near(halved.px[0], 0.5, 1e-5) && near(halved.px[3], 0.5, 1e-5),
          "transform: gives premultiplied (0.5, 0, 0) at alpha 0.5 -- which un-premultiplies "
          "to FULL red at half coverage, not to half-strength red, because the transparent "
          "texel contributed neither colour nor weight");
    check(halved.px[1] == 0.0f && halved.px[2] == 0.0f,
          "transform: and contributes exactly ZERO of any other channel -- a straight-alpha "
          "average would have mixed in whatever RGB sat behind alpha 0, which is the halo "
          "every scaled logo with a transparent background has");

    // Negative alpha out of a negative lobe is not a coverage. The rule
    // matches core/Premultiply's `a <= 0 -> {0,0,0,0}` so the resampler and
    // every read boundary in the build agree on what a transparent texel is.
    TransformImage spike;
    spike.width = 9;
    spike.height = 1;
    spike.px.assign(9u * 4u, 0.0f);
    for (uint32_t i = 3; i < 6; ++i) {
      float* q = spike.px.data() + static_cast<size_t>(i) * 4u;
      q[0] = q[1] = q[2] = q[3] = 1.0f;
    }
    TransformParams lz;
    lz.kernel = ResampleKernel::Lanczos3;
    TransformImage rung;
    transformImage(spike, transformTranslate(0.37f, 0.0f), 9, 1, lz, &rung, nullptr, &err);
    bool noNegativeAlpha = true;
    bool rgbZeroedWithAlpha = true;
    for (uint32_t i = 0; i < 9; ++i) {
      const float* q = rung.px.data() + static_cast<size_t>(i) * 4u;
      if (q[3] < 0.0f) noNegativeAlpha = false;
      if (q[3] == 0.0f && (q[0] != 0.0f || q[1] != 0.0f || q[2] != 0.0f))
        rgbZeroedWithAlpha = false;
    }
    check(noNegativeAlpha && rgbZeroedWithAlpha,
          "transform: Lanczos3 ringing on a hard alpha step never leaves a NEGATIVE alpha in "
          "the output, and a texel driven to zero alpha carries zero colour -- the same rule "
          "core/Premultiply applies at every read boundary, applied where the value is made");
  }

  // --- 7. PRD D17: crop and canvas size do not resample --------------------
  {
    const TransformImage img = makeContent(37, 23);
    TransformImage cropped, restored;
    check(cropImage(img, 5, 3, 20, 15, &cropped, &err) && cropped.width == 20 &&
              cropped.height == 15,
          "transform: a crop produces the requested extent");
    check(cropImage(cropped, -5, -3, 37, 23, &restored, &err),
          "transform: and can be placed back into the original extent");
    bool interiorIdentical = true;
    for (uint32_t y = 3; y < 18 && interiorIdentical; ++y) {
      for (uint32_t x = 5; x < 25; ++x) {
        const float* a = img.px.data() + (static_cast<size_t>(y) * 37 + x) * 4u;
        const float* b = restored.px.data() + (static_cast<size_t>(y) * 37 + x) * 4u;
        if (std::memcmp(a, b, sizeof(float) * 4u) != 0) {
          interiorIdentical = false;
          break;
        }
      }
    }
    check(interiorIdentical,
          "transform: crop out and back is BIT-IDENTICAL over the kept rectangle -- a crop is "
          "an index copy, and routing it through the matrix path would have made it lossy for "
          "no reason at all");

    // A crop that hangs off the edge is not an error, it is a canvas
    // extension: the same operation with a signed origin.
    TransformImage overhang;
    check(cropImage(img, -4, -4, 10, 10, &overhang, &err) && overhang.px[0] == 0.0f &&
              overhang.px[3] == 0.0f,
          "transform: a crop rectangle hanging off the edge fills with TRANSPARENT BLACK "
          "rather than refusing -- a crop handle dragged outside the image needs no second "
          "code path");
    TransformImage outside;
    check(cropImage(img, 500, 500, 4, 4, &outside, &err) &&
              outside.px == std::vector<float>(4u * 4u * 4u, 0.0f),
          "transform: and one entirely outside gives an entirely transparent result, not a "
          "failure");

    // Canvas size: the centred offset is FLOORED, so an odd growth puts the
    // extra pixel on the same side every time. Rounding would flip sides with
    // parity, which reads as a one-pixel jitter while a user drags the field.
    TransformImage grown;
    check(resizeCanvas(img, 40, 26, CanvasAnchor::Center, &grown, &err) && grown.width == 40,
          "transform: a centred canvas grow runs");
    check(std::memcmp(img.px.data(), grown.px.data() + (1u * 40u + 1u) * 4u,
                      sizeof(float) * 4u) == 0,
          "transform: 37 -> 40 centred puts the source at offset (1, 1) -- floored, so the odd "
          "pixel lands on the same side at every size instead of flipping with parity");
    TransformImage shrunk;
    check(resizeCanvas(img, 20, 20, CanvasAnchor::BottomRight, &shrunk, &err) &&
              shrunk.width == 20 && shrunk.height == 20,
          "transform: and a canvas SHRINK is the same operation with a negative offset, which "
          "is where a truncating divide would round the wrong way");

    // Image size: 1:1 is not a resize.
    TransformParams p;
    TransformImage same;
    TransformReport r{};
    check(resizeImage(img, 37, 23, p, &same, &r, &err) && bitIdentical(img, same) &&
              r.reconstructionPasses == 0,
          "transform: an image resize to the size it already is is BIT-IDENTICAL and runs no "
          "pass -- a resize that changes nothing must not cost an ulp");
    // And a non-uniform resize, one axis up and one down, must prefilter only
    // the axis that shrank.
    TransformImage mixed;
    TransformReport rm{};
    check(resizeImage(img, 74, 11, p, &mixed, &rm, &err) && mixed.width == 74 &&
              mixed.height == 11 && rm.prefiltered && rm.prefilterWidth == 37 &&
              rm.prefilterHeight == 11,
          "transform: a resize that ENLARGES one axis and shrinks the other prefilters only "
          "the axis that shrank (37x23 -> 37x11) -- prefiltering the growing axis would throw "
          "away detail there is room for");
  }

  // --- 8. PRD D14: the four-corner perspective solve -----------------------
  {
    const std::array<Point2, 4> src{Point2{0.0f, 0.0f}, Point2{100.0f, 0.0f},
                                    Point2{100.0f, 80.0f}, Point2{0.0f, 80.0f}};
    const std::array<Point2, 4> dst{Point2{10.0f, 5.0f}, Point2{120.0f, 20.0f},
                                    Point2{95.0f, 90.0f}, Point2{20.0f, 70.0f}};
    Mat3 h;
    check(transformFromQuad(src, dst, &h, &err),
          "transform: four non-degenerate corners solve to a homography");
    double worst = 0.0;
    for (int i = 0; i < 4; ++i) {
      const Point2 p = mat3MapPoint(h, src[i]);
      worst = std::max(worst, static_cast<double>(std::max(std::fabs(p.x - dst[i].x),
                                                           std::fabs(p.y - dst[i].y))));
    }
    std::printf("  [selftest] transform: homography maps its own four corners with max error "
                "%.4g px\n",
                worst);
    check(worst < 1e-4,
          "transform: and the solved matrix maps those four corners onto their targets to under "
          "1e-4 px (measured 7.6e-6) -- a handle the user dragged has to land where they "
          "dropped it, and a float solve on a slanted quad loses the far corner visibly");
    check(h.m[6] != 0.0f || h.m[7] != 0.0f,
          "transform: a genuinely perspective quad produces a NON-ZERO projective row -- an "
          "affine-only solve would fit three corners and miss the fourth");
    check(exactRemapKind(h) == ExactRemap::None,
          "transform: and a perspective matrix is never classified exact, whatever its linear "
          "part looks like");

    Mat3 dead;
    const std::array<Point2, 4> collinear{Point2{0, 0}, Point2{1, 1}, Point2{2, 2},
                                          Point2{3, 3}};
    check(!transformFromQuad(collinear, dst, &dead, &err) && !err.empty(),
          "transform: four COLLINEAR source points are refused by name -- there is no matrix "
          "that maps a line onto a quad, and a silently-singular one fills the destination "
          "with infinities");

    // A perspective transform still runs end to end and still resamples once.
    TransformImage photo = makeContent(64, 64);
    TransformParams p;
    TransformImage warped;
    TransformReport r{};
    const std::array<Point2, 4> unit{Point2{0, 0}, Point2{64, 0}, Point2{64, 64},
                                     Point2{0, 64}};
    const std::array<Point2, 4> slanted{Point2{8, 4}, Point2{60, 10}, Point2{56, 58},
                                        Point2{4, 50}};
    Mat3 warp;
    check(transformFromQuad(unit, slanted, &warp, &err) &&
              transformImage(photo, warp, 64, 64, p, &warped, &r, &err) &&
              r.reconstructionPasses == 1,
          "transform: a four-corner warp resamples exactly ONCE, like every other general "
          "transform -- perspective is a different matrix, not a different number of passes");
  }

  // --- 9. The tile-store bridge --------------------------------------------
  {
    // The bridge is what makes "a 3x3 matrix per layer" real rather than a
    // claim about a flat buffer. core/LayerGeometry.hpp refused every non-
    // integer transform on the grounds that this did not exist.
    TileStore store;
    for (int32_t y = 0; y < 200; ++y) {
      for (int32_t x = 0; x < 200; ++x) {
        Tile& t = store.getOrCreate(tileCoordAt(PixelCoord{x, y}));
        const float v = static_cast<float>((x * 5 + y * 3) % 64) / 63.0f;
        t.writePixel(tileLocalOffset(PixelCoord{x, y}), std::array<float, 4>{v, 1.0f - v, 0.5f, 1.0f});
      }
    }
    const size_t before = store.occupiedTileCount();
    check(before == 4,
          "transform: a 200x200 region occupies four 128px tiles, which is the geometry the "
          "bridge has to cross");

    const TransformImage img = imageFromTileStore(store, 0, 0, 200, 200);
    check(img.valid() && img.width == 200 && img.height == 200,
          "transform: the store materialises into a flat premultiplied image");
    // Half-float round trip: the store is rgba16float, so this is the only
    // quantisation the bridge adds and it is the same one every write pays.
    double worstHalf = 0.0;
    for (int32_t y = 0; y < 200; ++y) {
      for (int32_t x = 0; x < 200; ++x) {
        const float v = static_cast<float>((x * 5 + y * 3) % 64) / 63.0f;
        worstHalf = std::max(
            worstHalf,
            std::fabs(static_cast<double>(img.px[(static_cast<size_t>(y) * 200 + x) * 4u]) -
                      static_cast<double>(halfToFloat(floatToHalf(v)))));
      }
    }
    check(worstHalf == 0.0,
          "transform: and every texel matches the half-float the store actually holds EXACTLY "
          "-- the bridge widens, it does not re-quantise");

    // Read outside the written region: absent tiles must read as transparent
    // black, the store's own implicit content, so 'never touched' and 'written
    // transparent' are the same colour here as they are everywhere else.
    const TransformImage far_ = imageFromTileStore(store, 1000, 1000, 8, 8);
    check(far_.px == std::vector<float>(8u * 8u * 4u, 0.0f),
          "transform: a region over ABSENT tiles reads transparent black rather than "
          "allocating them -- materialising a bounding box must not fill the store");

    // Round trip through an exact flip, back into a fresh store.
    TransformParams p;
    TransformImage flipped;
    transformImage(img, transformFlipHorizontal(200), 200, 200, p, &flipped, nullptr, &err);
    TileStore out;
    tileStoreFromImage(flipped, 0, 0, &out);
    const TransformImage reread = imageFromTileStore(out, 0, 0, 200, 200);
    check(bitIdentical(flipped, reread),
          "transform: an image written back into a store and read out again is BIT-IDENTICAL "
          "-- the flip is exact, so the bridge must not be the thing that loses a bit");

    // An entirely transparent image must not allocate tiles that were not
    // there. A transform's destination is a bounding box and a rotate empties
    // most of its corners; allocating 128 KiB for each would cost more than
    // the content.
    TransformImage empty;
    empty.width = 300;
    empty.height = 300;
    empty.px.assign(300u * 300u * 4u, 0.0f);
    TileStore fresh;
    tileStoreFromImage(empty, 0, 0, &fresh);
    check(fresh.occupiedTileCount() == 0,
          "transform: writing a fully transparent image allocates NO tiles -- a 45-degree "
          "rotate's empty corners must not cost 128 KiB each");

    // But an existing tile IS cleared, because that is the half of a transform
    // that moves content away from where it was.
    tileStoreFromImage(empty, 0, 0, &out);
    const TransformImage cleared = imageFromTileStore(out, 0, 0, 200, 200);
    check(cleared.px == std::vector<float>(200u * 200u * 4u, 0.0f),
          "transform: while an EXISTING tile written with transparency is cleared -- skipping "
          "that would leave the layer's old pixels behind as a ghost of where it used to be");
  }

  // --- 10. Refusals name what they refused ---------------------------------
  {
    const TransformImage img = makeContent(16, 16);
    TransformParams p;
    TransformImage out;

    check(!transformImage(img, mat3Identity(), 0, 8, p, &out, nullptr, &err) &&
              err.find("0x8") != std::string::npos && out.px.empty(),
          "transform: a zero destination extent is refused with the extent in the message, and "
          "leaves NO partially written image behind");

    // A 45-degree skew on both axes is genuinely singular -- [[1,1],[1,1]] has
    // determinant zero -- so it is refused rather than approximated.
    check(!transformImage(img, transformSkewDegrees(45.0f, 45.0f), 16, 16, p, &out, nullptr,
                          &err) &&
              !err.empty(),
          "transform: a 45-degree skew on BOTH axes is exactly singular and is refused by name "
          "-- there is no source position for a destination pixel to read from, so there is "
          "nothing to approximate");

    // In-place is refused rather than handled. Every path clears the
    // destination before reading the source, so an aliased call that "worked"
    // would return true and a blank image -- the worst possible outcome, since
    // nothing reports a failure and the pixels are simply gone.
    TransformImage inPlace = img;
    check(!transformImage(inPlace, transformScale(2.0f, 2.0f), 32, 32, p, &inPlace, nullptr,
                          &err) &&
              !cropImage(inPlace, 0, 0, 8, 8, &inPlace, &err) && inPlace.valid(),
          "transform: passing the same image as source and destination is REFUSED by name, and "
          "the image is left intact -- an in-place call that silently returned a blank buffer "
          "would report success and lose the layer");

    TransformImage bogus;
    bogus.width = 4;
    bogus.height = 4;
    bogus.px.resize(7);  // not 4*4*4
    check(!transformImage(bogus, mat3Identity(), 4, 4, p, &out, nullptr, &err) &&
              !resizeImage(bogus, 2, 2, p, &out, nullptr, &err) &&
              !cropImage(bogus, 0, 0, 2, 2, &out, &err),
          "transform: an image whose buffer does not match its extent is refused by every "
          "entry point, rather than read past");
  }

  std::printf("[selftest] transform %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
