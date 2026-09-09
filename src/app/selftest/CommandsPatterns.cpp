#include "app/selftest/Support.hpp"

#include <functional>

#include "app/Command.hpp"
#include "app/PixelOpBridge.hpp"
#include "core/SelectionMask.hpp"
#include "io/PsPatterns.hpp"
#include "ops/Lens.hpp"
#include "ops/Pattern.hpp"

namespace np {
namespace {

// A `w` x `h` RGB document with one opaque layer whose content is whatever
// `value` says. Built here rather than borrowed for the reason
// app/selftest/Command.cpp gives about its own fixture: a fixture that came
// out of one of the ops under test would share that op's assumptions.
OpenDocument makePatternsDocument(int32_t w, int32_t h,
                                  const std::function<std::array<float, 4>(int32_t, int32_t)>& value) {
  OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "patterns");
  od.document.layers[0].name = "Plate";
  TileStore& tiles = *od.document.layers[0].rgbTiles;
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x) {
      const PixelCoord doc{x, y};
      tiles.getOrCreate(tileCoordAt(doc)).writePixel(tileLocalOffset(doc), value(x, y));
    }
  od.recordEdit("patterns fixture", EditKind::Content);
  return od;
}

// The raw half bit patterns of one region, so "identical" can mean identical
// rather than identical-to-a-tolerance. `--selftest`'s exact-path sections use
// memcmp for the same reason PRD D15 asks for: a tolerance here would pass on
// exactly the implementation the claim forbids.
std::vector<uint16_t> rawBits(const TileStore& tiles, const PixelRect& r) {
  std::vector<uint16_t> out;
  out.reserve(static_cast<size_t>(r.width()) * static_cast<size_t>(r.height()) * 4u);
  for (int32_t y = r.y0; y < r.y1; ++y)
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const PixelCoord doc{x, y};
      const Tile* t = tiles.find(tileCoordAt(doc));
      if (t == nullptr) {
        out.insert(out.end(), {0, 0, 0, 0});
        continue;
      }
      const std::array<float, 4> v = t->readPixel(tileLocalOffset(doc));
      for (float c : v) out.push_back(floatToHalf(c));
    }
  return out;
}

std::array<float, 4> readTexel(const TileStore& tiles, int32_t x, int32_t y) {
  const PixelCoord doc{x, y};
  const Tile* t = tiles.find(tileCoordAt(doc));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(doc));
}

}  // namespace

// app/CommandsPatterns, ops/Lens and ops/Pattern -- PLAN.md Phase 19 step 5's
// two parked P2 image ops (PRD D22 lens correction, PRD D27 pattern
// define/fill), and the three command rows that reach them.
//
// **This section asserts the arithmetic, not that the calls return.** The two
// claims it is built around, one per op:
//
//  * **A zero-strength lens correction is the identity TO THE BIT**, and it is
//    asserted twice: once through the short-circuit that guarantees it, and
//    once again with that short-circuit deliberately switched off, so that the
//    claim is about the *gather* and not about the branch in front of it. A
//    zero-coefficient pass that quietly rewrote every texel through a
//    reconstruction kernel would be invisible in one pass and cumulative over
//    a batch, which is exactly the damage ops/Transform.hpp section 1 is
//    written about.
//
//  * **The geometry is the model.** Section B4 gathers a lens correction of an
//    exact linear ramp and checks each output texel against `a + b * srcX`,
//    with `srcX` taken from `lensSourcePosition()`. Catmull-Rom reproduces a
//    linear function exactly, so this measures where the op *read from*
//    independently of how it interpolated -- a sign error, a missing
//    half-diagonal normalisation or a half-texel offset all fail it, and none
//    of them would fail a test that only compared two blurry pictures.
//
//  * **A pattern tiled at its own size reproduces itself exactly at the tile
//    boundaries.** The seam is where the off-by-one lives: an output that is
//    one texel wrong at x = W and nowhere else looks like paper and averages
//    like paper. Section D asserts the four blocks of a 2W x 2H fill are
//    bit-identical to each other and to the pattern, then names the two seam
//    columns and the two seam rows individually so a failure says which edge.
//
//  * **The euclidean modulus.** C's `%` truncates, so a negative document
//    texel or a non-zero tiling origin reads off the front of the pattern
//    buffer. Section E asserts `patternSourceTexel()` at negative operands
//    directly, because that is the one line the whole tiling rests on.
//
// Headless, GPU-free and filesystem-free.
bool runCommandsPatternsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // ------------------------------------------------------------------
  std::printf("  -- A. the three rows are registered --\n");
  {
    const CommandSpec* lens = findCommand("lens_correct");
    const CommandSpec* define = findCommand("define_pattern");
    const CommandSpec* fill = findCommand("fill_with_pattern");
    check(lens != nullptr, "rows: lens_correct is in the table");
    check(define != nullptr, "rows: define_pattern is in the table");
    check(fill != nullptr, "rows: fill_with_pattern is in the table");
    // Every key the adapter reads must be one the table advertises, which is
    // what makes the ACTIONS panel's step editor and the file format agree
    // with the code rather than with a comment.
    if (lens != nullptr) {
      const std::vector<std::string>& n = lens->paramNames;
      check(std::find(n.begin(), n.end(), "k1") != n.end() &&
                std::find(n.begin(), n.end(), "k2") != n.end() &&
                std::find(n.begin(), n.end(), "ca_red") != n.end() &&
                std::find(n.begin(), n.end(), "ca_blue") != n.end() &&
                std::find(n.begin(), n.end(), "kernel") != n.end(),
            "rows: lens_correct advertises its five parameters");
    }
    if (fill != nullptr) {
      const std::vector<std::string>& n = fill->paramNames;
      check(std::find(n.begin(), n.end(), "pattern") != n.end() &&
                std::find(n.begin(), n.end(), "origin_x") != n.end() &&
                std::find(n.begin(), n.end(), "origin_y") != n.end(),
            "rows: fill_with_pattern advertises pattern and its origin");
    }
  }

  // ------------------------------------------------------------------
  std::printf("  -- B. lens correction: the maths --\n");

  // B1. The model, read straight off `lensSourcePosition()`.
  {
    LensParams p;
    p.frame = PixelRect{0, 0, 64, 48};
    p.k1 = 0.25f;
    const float cx = 32.0f, cy = 24.0f;

    const Point2 atCentre = lensSourcePosition(p, Point2{cx, cy}, 1);
    check(std::fabs(atCentre.x - cx) < 1e-4f && std::fabs(atCentre.y - cy) < 1e-4f,
          "lens: the optical centre maps to itself for any k");

    // r == 1 exactly at a corner, by the half-diagonal normalisation, so the
    // scale there is 1 + k1 + k2 and the displacement is that times the
    // half-diagonal. Asserting the corner is asserting the normalisation:
    // divide by the half-WIDTH instead and this number changes.
    p.k2 = -0.10f;
    const double halfDiag = 0.5 * std::sqrt(64.0 * 64.0 + 48.0 * 48.0);
    const Point2 corner = lensSourcePosition(p, Point2{0.0f, 0.0f}, 1);
    const double expectScale = 1.0 + 0.25 - 0.10;
    const double gotDist = std::sqrt(static_cast<double>(corner.x - cx) * (corner.x - cx) +
                                     static_cast<double>(corner.y - cy) * (corner.y - cy));
    check(std::fabs(gotDist - expectScale * halfDiag) < 1e-2,
          "lens: r is normalised by the half-diagonal (r == 1 at a corner)");

    // The sign convention, which is the thing a reader most wants asserted:
    // positive k1 reads from FURTHER OUT, which drags the outer picture in --
    // the correction for barrel.
    LensParams pos;
    pos.frame = p.frame;
    pos.k1 = 0.30f;
    LensParams neg = pos;
    neg.k1 = -0.30f;
    const Point2 probe{48.0f, 36.0f};
    const double baseR = std::hypot(probe.x - cx, probe.y - cy);
    const Point2 sPos = lensSourcePosition(pos, probe, 1);
    const Point2 sNeg = lensSourcePosition(neg, probe, 1);
    check(std::hypot(sPos.x - cx, sPos.y - cy) > baseR,
          "lens: k1 > 0 samples further out (corrects barrel)");
    check(std::hypot(sNeg.x - cx, sNeg.y - cy) < baseR,
          "lens: k1 < 0 samples further in (corrects pincushion)");

    // Chromatic aberration is a per-channel radial scale with GREEN as the
    // reference, which is what stops a colour control from cancelling a
    // geometry one.
    LensParams ca;
    ca.frame = p.frame;
    ca.caRed = 0.01f;
    const Point2 r0 = lensSourcePosition(ca, probe, 0);
    const Point2 g0 = lensSourcePosition(ca, probe, 1);
    const Point2 b0 = lensSourcePosition(ca, probe, 2);
    check(std::hypot(r0.x - cx, r0.y - cy) > std::hypot(g0.x - cx, g0.y - cy),
          "lens: ca_red scales R's radius away from G's");
    check(std::fabs(b0.x - g0.x) < 1e-5f && std::fabs(b0.y - g0.y) < 1e-5f,
          "lens: ca_blue == 0 leaves B exactly on G");
    check(std::fabs(g0.x - probe.x) < 1e-5f && std::fabs(g0.y - probe.y) < 1e-5f,
          "lens: G is the reference channel and takes no scale of its own");
  }

  // B2. The refusals, by name.
  {
    LensParams p;
    p.frame = PixelRect{0, 0, 64, 48};
    check(lensParamsValid(p), "lens: a zero-coefficient request is valid");
    LensParams empty = p;
    empty.frame = PixelRect{0, 0, 0, 48};
    check(!lensParamsValid(empty), "lens: an empty frame is refused");
    LensParams nan = p;
    nan.k1 = std::numeric_limits<float>::quiet_NaN();
    check(!lensParamsValid(nan), "lens: a non-finite coefficient is refused");
    LensParams mirrored = p;
    mirrored.caRed = -1.5f;
    check(!lensParamsValid(mirrored), "lens: ca_red <= -1 (a mirrored channel) is refused");
    // s(1) = 1 - 2 = -1: past the turn, two destination rings read one source
    // ring and the result is a mirrored halo that looks like a lens effect.
    LensParams folding = p;
    folding.k1 = -2.0f;
    check(!lensParamsValid(folding), "lens: a radial map that folds the picture is refused");
  }

  // B3. THE identity claim, asserted twice.
  {
    OpenDocument od = makePatternsDocument(40, 28, [](int32_t x, int32_t y) {
      const float v = 0.25f + 0.4f * static_cast<float>((x * 7 + y * 3) % 11) / 11.0f;
      return std::array<float, 4>{v, 1.0f - v, 0.5f * v + 0.2f, 1.0f};
    });
    const PixelRect canvas{0, 0, 40, 28};
    const std::vector<uint16_t> before = rawBits(*od.document.layers[0].rgbTiles, canvas);

    LensParams p;
    p.frame = canvas;  // every coefficient stays at its zero default
    TileStore out;
    check(lensCorrectTiles(*od.document.layers[0].rgbTiles, canvas, p, &out),
          "lens: a zero-strength correction runs");
    check(rawBits(out, canvas) == before,
          "lens: zero distortion is the identity TO THE BIT (short-circuit)");

    // And again with the short-circuit off, so the claim is about the gather.
    // Catmull-Rom is interpolating and its weights at non-zero integer offsets
    // are exactly 0 in IEEE arithmetic, so the gather at a texel centre reads
    // one tap at weight 1 and is bit-exact -- which is a property of the
    // kernel, not a coincidence, and is worth pinning here because it is what
    // makes the short-circuit an optimisation rather than a cover-up.
    LensParams gathered = p;
    gathered.allowExactIdentity = false;
    TileStore out2;
    check(lensCorrectTiles(*od.document.layers[0].rgbTiles, canvas, gathered, &out2),
          "lens: the identity case runs through the real gather too");
    check(rawBits(out2, canvas) == before,
          "lens: zero distortion is the identity TO THE BIT (gather, no short-circuit)");
  }

  // B4. **The geometry is the model.** A linear ramp, gathered, checked
  // against `a + b * srcX` with srcX from `lensSourcePosition()`.
  {
    constexpr int32_t kW = 96, kH = 72;
    constexpr float kA = 0.20f, kB = 0.004f;
    OpenDocument od = makePatternsDocument(kW, kH, [](int32_t x, int32_t) {
      const float v = kA + kB * (static_cast<float>(x) + 0.5f);
      return std::array<float, 4>{v, v, v, 1.0f};
    });
    LensParams p;
    p.frame = PixelRect{0, 0, kW, kH};
    p.k1 = 0.08f;
    p.kernel = ResampleKernel::CatmullRom;
    TileStore out;
    check(lensCorrectTiles(*od.document.layers[0].rgbTiles, p.frame, p, &out),
          "lens: the ramp gather runs");

    // Only texels whose whole footprint stays inside the picture: the edge
    // policy clamps taps at the border on purpose (ops/Transform.hpp), so a
    // border texel is deliberately NOT `a + b*srcX` and asserting it there
    // would be asserting the wrong claim.
    double worst = 0.0;
    int32_t worstX = -1, worstY = -1;
    size_t sampled = 0;
    for (int32_t y = 16; y < kH - 16; ++y) {
      for (int32_t x = 16; x < kW - 16; ++x) {
        const Point2 s = lensSourcePosition(
            p, Point2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, 1);
        if (s.x < 4.0f || s.y < 4.0f || s.x > static_cast<float>(kW) - 4.0f ||
            s.y > static_cast<float>(kH) - 4.0f)
          continue;
        const double expect = static_cast<double>(kA) + static_cast<double>(kB) * s.x;
        const double got = readTexel(out, x, y)[0];
        const double err = std::fabs(got - expect);
        if (err > worst) {
          worst = err;
          worstX = x;
          worstY = y;
        }
        ++sampled;
      }
    }
    // The tolerance is set by the half-float store, not by the resample:
    // Catmull-Rom reproduces a linear function exactly, so the residual is the
    // source quantisation (~4.9e-4 near 0.5, times the kernel's ~1.25 sum of
    // absolute weights) plus one more quantisation writing the answer back.
    std::printf("  [measured] lens ramp: %zu texels, worst |got - (a + b*srcX)| = %.6f at (%d,%d)\n",
                sampled, worst, worstX, worstY);
    check(sampled > 2000, "lens: the ramp check sampled the interior");
    check(worst < 3e-3, "lens: a corrected ramp reads a + b*srcX -- the geometry IS the model");
  }

  // B5. The seam invariant `frame` exists for: a strip's answer equals the
  // whole canvas's answer restricted to that strip, bit for bit. Derive the
  // optical centre from `outRect` instead and the two halves centre two
  // different lenses.
  {
    constexpr int32_t kW = 80, kH = 64;
    OpenDocument od = makePatternsDocument(kW, kH, [](int32_t x, int32_t y) {
      const float v = 0.15f + 0.6f * static_cast<float>((x * 5 + y * 11) % 13) / 13.0f;
      return std::array<float, 4>{v, v * 0.5f, 1.0f - v, 1.0f};
    });
    LensParams p;
    p.frame = PixelRect{0, 0, kW, kH};
    p.k1 = -0.12f;
    p.k2 = 0.04f;
    TileStore whole;
    TileStore strip;
    const PixelRect stripRect{kW / 2, 8, kW, kH - 8};
    check(lensCorrectTiles(*od.document.layers[0].rgbTiles, p.frame, p, &whole) &&
              lensCorrectTiles(*od.document.layers[0].rgbTiles, stripRect, p, &strip),
          "lens: both the whole-canvas and the strip request run");
    check(rawBits(whole, stripRect) == rawBits(strip, stripRect),
          "lens: a strip's texels are bit-identical to the whole canvas's (the seam rule)");
  }

  // B6. Chromatic aberration moves R and leaves G alone, in the pixels and not
  // only in the model.
  {
    constexpr int32_t kW = 64, kH = 64;
    OpenDocument od = makePatternsDocument(kW, kH, [](int32_t x, int32_t y) {
      const float v = ((x / 4 + y / 4) % 2 == 0) ? 0.85f : 0.15f;
      return std::array<float, 4>{v, v, v, 1.0f};
    });
    LensParams none;
    none.frame = PixelRect{0, 0, kW, kH};
    LensParams ca = none;
    ca.caRed = 0.02f;
    ca.allowExactIdentity = false;  // `none` must run the same path to compare
    none.allowExactIdentity = false;
    TileStore plain, aberrated;
    check(lensCorrectTiles(*od.document.layers[0].rgbTiles, none.frame, none, &plain) &&
              lensCorrectTiles(*od.document.layers[0].rgbTiles, ca.frame, ca, &aberrated),
          "lens: both CA comparison passes run");
    size_t redDiffers = 0, greenDiffers = 0, blueDiffers = 0;
    for (int32_t y = 8; y < kH - 8; ++y)
      for (int32_t x = 8; x < kW - 8; ++x) {
        const std::array<float, 4> a = readTexel(plain, x, y);
        const std::array<float, 4> b = readTexel(aberrated, x, y);
        if (std::fabs(a[0] - b[0]) > 1e-3f) ++redDiffers;
        if (a[1] != b[1]) ++greenDiffers;
        if (a[2] != b[2]) ++blueDiffers;
      }
    std::printf("  [measured] lens CA: R differs at %zu texels, G at %zu, B at %zu\n", redDiffers,
                greenDiffers, blueDiffers);
    check(redDiffers > 100, "lens: ca_red actually moves the red channel");
    check(greenDiffers == 0, "lens: ca_red leaves green bit-identical");
    check(blueDiffers == 0, "lens: ca_red leaves blue bit-identical");
  }

  // ------------------------------------------------------------------
  std::printf("  -- C. define pattern --\n");
  {
    constexpr int32_t kW = 24, kH = 16;
    OpenDocument od = makePatternsDocument(kW, kH, [](int32_t x, int32_t y) {
      return std::array<float, 4>{static_cast<float>(x) / 32.0f, static_cast<float>(y) / 32.0f,
                                  0.5f, 1.0f};
    });
    Pattern pat;
    std::string err;
    check(definePattern(*od.document.layers[0].rgbTiles, PixelRect{4, 2, 12, 10}, "swatch", &pat,
                        &err),
          "define: an 8x8 region defines");
    check(pat.valid() && pat.width == 8 && pat.height == 8, "define: the pattern is 8x8");
    check(pat.name == "swatch" && !pat.id.empty(), "define: the pattern carries a name and an id");
    // The copy is a copy: no premultiply conversion, no scaling, no transfer
    // function. This is what makes define/fill on an untouched region exact.
    bool copyExact = true;
    for (int32_t y = 0; y < 8; ++y)
      for (int32_t x = 0; x < 8; ++x) {
        const std::array<float, 4> src =
            readTexel(*od.document.layers[0].rgbTiles, 4 + x, 2 + y);
        const float* d = pat.px.data() + (static_cast<size_t>(y) * 8 + x) * 4u;
        for (int c = 0; c < 4; ++c)
          if (d[c] != src[c]) copyExact = false;
      }
    check(copyExact, "define: the stored texels are the layer's texels, unconverted");

    Pattern bad;
    check(!definePattern(*od.document.layers[0].rgbTiles, PixelRect{4, 4, 4, 9}, "x", &bad, &err),
          "define: an empty rectangle is refused");
    check(contains(err, "empty"), "define: the empty refusal names the reason");
    check(!definePattern(*od.document.layers[0].rgbTiles, PixelRect{0, 0, 9000, 4}, "x", &bad,
                         &err),
          "define: an over-large rectangle is refused");
    check(contains(err, "4096"), "define: the size refusal names the cap");
  }

  // The .abr bridge -- the one conversion that keeps io/PsPatterns the only
  // decoder of Photoshop pattern bytes in this build.
  {
    PsPattern ps;
    ps.id = "63d61f21-0000-0000-0000-bc81e4dfd608";
    ps.name = "Extra Heavy Canvas";
    ps.width = 3;
    ps.height = 2;
    ps.height8 = {0, 51, 255, 128, 200, 17};
    const Pattern p = patternFromPsPattern(ps);
    check(p.valid() && p.width == 3 && p.height == 2, "abr bridge: dimensions carry across");
    check(p.id == ps.id && p.name == ps.name, "abr bridge: the joining UUID and name carry across");
    bool greyOpaque = true;
    for (size_t i = 0; i < 6; ++i) {
      const float v = static_cast<float>(ps.height8[i]) / 255.0f;
      const float* d = p.px.data() + i * 4u;
      if (d[0] != v || d[1] != v || d[2] != v || d[3] != 1.0f) greyOpaque = false;
    }
    check(greyOpaque, "abr bridge: a height field becomes opaque grey, scaled by 1/255 and no more");
  }

  // ------------------------------------------------------------------
  std::printf("  -- D. pattern fill: the seam --\n");
  {
    constexpr int32_t kPW = 12, kPH = 10;
    // A pattern every one of whose texels is distinct, so a one-texel slip at
    // a seam cannot be masked by a neighbour that happened to match.
    OpenDocument seed = makePatternsDocument(kPW, kPH, [](int32_t x, int32_t y) {
      const float v = static_cast<float>(y * kPW + x) / 256.0f;
      return std::array<float, 4>{v, 0.5f - v * 0.25f, 0.25f + v * 0.5f, 1.0f};
    });
    Pattern pat;
    std::string err;
    check(definePattern(*seed.document.layers[0].rgbTiles, PixelRect{0, 0, kPW, kPH}, "tile", &pat,
                        &err),
          "fill: the source pattern defines");

    OpenDocument od = makePatternsDocument(kPW * 2, kPH * 2,
                                           [](int32_t, int32_t) {
                                             return std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
                                           });
    PatternFillParams fp;
    fp.pattern = &pat;
    const PixelRect canvas{0, 0, kPW * 2, kPH * 2};
    TileStore filled;
    check(patternFillTiles(*od.document.layers[0].rgbTiles, canvas, fp, &filled),
          "fill: the 2W x 2H fill runs");

    // The four blocks are bit-identical to each other AND to the pattern.
    const PixelRect b00{0, 0, kPW, kPH};
    const PixelRect b10{kPW, 0, kPW * 2, kPH};
    const PixelRect b01{0, kPH, kPW, kPH * 2};
    const PixelRect b11{kPW, kPH, kPW * 2, kPH * 2};
    const std::vector<uint16_t> block = rawBits(filled, b00);
    check(block == rawBits(filled, b10) && block == rawBits(filled, b01) &&
              block == rawBits(filled, b11),
          "fill: all four tiles are bit-identical to each other");
    check(block == rawBits(*seed.document.layers[0].rgbTiles, PixelRect{0, 0, kPW, kPH}),
          "fill: a tile is bit-identical to the pattern it came from");

    // And the seams by name, so a failure says which edge slipped. The last
    // column of one tile and the first column of the next are the two texels
    // an off-by-one swaps, repeats or drops.
    bool vSeam = true, hSeam = true;
    for (int32_t y = 0; y < kPH; ++y) {
      if (readTexel(filled, kPW - 1, y) != readTexel(filled, kPW * 2 - 1, y)) vSeam = false;
      if (readTexel(filled, kPW, y) != readTexel(filled, 0, y)) vSeam = false;
    }
    for (int32_t x = 0; x < kPW; ++x) {
      if (readTexel(filled, x, kPH - 1) != readTexel(filled, x, kPH * 2 - 1)) hSeam = false;
      if (readTexel(filled, x, kPH) != readTexel(filled, x, 0)) hSeam = false;
    }
    check(vSeam, "fill: the vertical seam at x = W repeats the pattern's column 0 exactly");
    check(hSeam, "fill: the horizontal seam at y = H repeats the pattern's row 0 exactly");
  }

  // ------------------------------------------------------------------
  std::printf("  -- E. the euclidean modulus --\n");
  {
    Pattern pat;
    pat.width = 8;
    pat.height = 5;
    pat.px.assign(pat.sampleCount(), 0.0f);
    // Truncating `%` gives -3 here, which indexes off the FRONT of the buffer.
    check(patternSourceTexel(pat, 3, 0, PixelCoord{0, 0}).x == 5,
          "modulus: a positive origin wraps forwards (0 - 3 -> 5, not -3)");
    check(patternSourceTexel(pat, 0, 2, PixelCoord{0, 0}).y == 3,
          "modulus: the same, on y");
    check(patternSourceTexel(pat, -3, -1, PixelCoord{0, 0}).x == 3 &&
              patternSourceTexel(pat, -3, -1, PixelCoord{0, 0}).y == 1,
          "modulus: a negative origin shifts the phase the other way");
    check(patternSourceTexel(pat, 0, 0, PixelCoord{-1, -1}).x == 7 &&
              patternSourceTexel(pat, 0, 0, PixelCoord{-1, -1}).y == 4,
          "modulus: a negative document texel wraps to the far edge");
    check(patternSourceTexel(pat, 0, 0, PixelCoord{8, 5}).x == 0 &&
              patternSourceTexel(pat, 0, 0, PixelCoord{8, 5}).y == 0,
          "modulus: exactly one period returns to the origin");
    check(patternSourceTexel(pat, 0, 0, PixelCoord{-16, -15}).x == 0,
          "modulus: several periods negative still lands in range");
  }

  // ------------------------------------------------------------------
  std::printf("  -- F. the commands, and the selection bound --\n");
  {
    constexpr int32_t kW = 40, kH = 32;
    OpenDocument od = makePatternsDocument(kW, kH, [](int32_t x, int32_t y) {
      const float v = static_cast<float>((x * 3 + y * 5) % 17) / 17.0f;
      return std::array<float, 4>{v, 1.0f - v, 0.5f, 1.0f};
    });

    // define_pattern refuses by name before it reads a texel.
    Command def{"define_pattern", JsonValue::object()};
    CommandResult r = applyCommand(od, def);
    check(!r.ok && contains(r.status, "name"), "command: define_pattern with no name refuses by name");

    // A pattern from an explicit rectangular selection.
    od.selection = selectRectangle(4.0f, 6.0f, 12.0f, 14.0f);
    def.params.set("name", JsonValue::string("plate"));
    r = applyCommand(od, def);
    check(r.ok && !r.changesPixels && r.texelsChanged == 0,
          "command: define_pattern succeeds and reports no texel change");
    check(contains(r.status, "8x8"), "command: define_pattern reports the pattern's size");
    const Pattern* stored = sessionPatterns().findByName("plate");
    check(stored != nullptr && stored->width == 8 && stored->height == 8,
          "command: the pattern reached the session store, resolvable by name");

    // fill_with_pattern refuses an unknown name -- by name, and saying why the
    // pattern might be missing.
    Command fill{"fill_with_pattern", JsonValue::object()};
    fill.params.set("pattern", JsonValue::string("no-such-pattern"));
    r = applyCommand(od, fill);
    check(!r.ok && contains(r.status, "no-such-pattern") && contains(r.status, "session"),
          "command: an unknown pattern name refuses, naming it and the session rule");

    // A fractional origin is refused rather than rounded.
    fill.params.set("pattern", JsonValue::string("plate"));
    fill.params.set("origin_x", JsonValue::number(2.5));
    r = applyCommand(od, fill);
    check(!r.ok && contains(r.status, "whole texels"),
          "command: a fractional tiling origin refuses rather than rounding");

    // And the real fill, bounded by the same selection every other menu pixel
    // op is bounded by. Outside the marquee must be untouched, bit for bit --
    // which is app/PixelOpBridge's answer, and asserting it here is what says
    // this op asks it rather than answering the question itself.
    const std::vector<uint16_t> outsideBefore =
        rawBits(*od.document.layers[0].rgbTiles, PixelRect{20, 20, kW, kH});
    fill.params.set("origin_x", JsonValue::number(0.0));
    fill.params.set("origin_y", JsonValue::number(0.0));
    r = applyCommand(od, fill);
    check(r.ok && r.changesPixels && r.texelsChanged > 0,
          "command: fill_with_pattern fills and reports texels changed");
    check(rawBits(*od.document.layers[0].rgbTiles, PixelRect{20, 20, kW, kH}) == outsideBefore,
          "command: nothing outside the selection moved (the bridge does the bounding)");
    // Inside, the texels are the pattern's, tiled from the document origin.
    bool insideTiled = true;
    for (int32_t y = 6; y < 14; ++y)
      for (int32_t x = 4; x < 12; ++x) {
        const PixelCoord s = patternSourceTexel(*stored, 0, 0, PixelCoord{x, y});
        const float* q =
            stored->px.data() +
            (static_cast<size_t>(s.y) * stored->width + static_cast<size_t>(s.x)) * 4u;
        const std::array<float, 4> got = readTexel(*od.document.layers[0].rgbTiles, x, y);
        for (int c = 0; c < 4; ++c)
          if (std::fabs(got[c] - q[c]) > 1e-3f) insideTiled = false;
      }
    check(insideTiled, "command: inside the selection, the texels are the pattern's own");

    // lens_correct through the same door: a zero-strength request is honestly
    // not an edit, and a folding one refuses with a sentence.
    Command lens{"lens_correct", JsonValue::object()};
    r = applyCommand(od, lens);
    check(r.ok && r.texelsChanged == 0,
          "command: a zero-strength lens_correct succeeds and changes nothing");
    lens.params.set("k1", JsonValue::number(-2.0));
    r = applyCommand(od, lens);
    check(!r.ok && contains(r.status, "fold"),
          "command: a folding lens_correct refuses and says what folded");
    lens.params.set("k1", JsonValue::number(0.05));
    lens.params.set("kernel", JsonValue::string("no-such-kernel"));
    r = applyCommand(od, lens);
    check(!r.ok && contains(r.status, "no-such-kernel"),
          "command: an unknown kernel name refuses, naming it");
    lens.params.set("kernel", JsonValue::string("lanczos3"));
    r = applyCommand(od, lens);
    check(r.ok && r.texelsChanged > 0, "command: a real lens_correct runs and changes texels");

    sessionPatterns().clear();
  }

  return ok;
}

}  // namespace np
