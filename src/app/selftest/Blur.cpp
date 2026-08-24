#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "ops/Blur.hpp"
#include "ops/Feather.hpp"
#include "ops/Roi.hpp"

namespace np {

namespace {

// A deterministic value source, so every run of this section sees the same
// field. Not <random>: a Mersenne twister's stream is standard-library
// dependent in ways a selftest that compares two paths bit for bit should not
// have to care about, and this is three lines. splitmix64's finalizer, the
// same mixer std::hash<TileCoord> uses.
float blurTestNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// Fills `tiles` over the given tile range with a premultiplied, opaque,
// deterministic field. Opaque so that "did RGB and A stay in lockstep" has a
// meaningful answer, and varying so that a blur has something to average.
TileStore blurTestField(int32_t tx0, int32_t tx1, int32_t ty0, int32_t ty1) {
  TileStore tiles;
  uint64_t counter = 0;
  for (int32_t ty = ty0; ty <= ty1; ++ty) {
    for (int32_t tx = tx0; tx <= tx1; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = blurTestNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
  return tiles;
}

float texelAt(const TileStore& tiles, int32_t x, int32_t y, int32_t channel) {
  const Tile* t = tiles.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return 0.0f;
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}))[static_cast<size_t>(channel)];
}

// Worst absolute disagreement between two stores over a rectangle.
double storeMaxDiff(const TileStore& a, const TileStore& b, const PixelRect& r) {
  double worst = 0.0;
  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      for (int32_t c = 0; c < 4; ++c) {
        worst = std::max(worst, static_cast<double>(
                                    std::fabs(texelAt(a, x, y, c) - texelAt(b, x, y, c))));
      }
    }
  }
  return worst;
}

}  // namespace

// ops/Roi, ops/Blur and ops/Feather -- PLAN.md "Phase 6 -- Filter and transform
// it" (ROI propagation, Gaussian/box blur) and PRD E4 (feather). Pure CPU, no
// PaintSim and no GPU, the same headless-first-class status runPointOpsTest()
// and runSelectionTest() have.
//
// Two assertions carry the section, and both are about a wrong answer that
// looks plausible rather than about a crash.
//
// **The tile seam.** A blur that reads only its own tile is exactly right in
// the middle of every tile and wrong by up to half of full scale on a grid of
// lines every 128 texels. Section 3 asserts the property positively (a blur
// split across a tile boundary is bit-identical to the same blur computed in
// one call) and then computes the broken version on purpose to prove the
// assertion is sensitive rather than merely satisfied.
//
// **The ROI direction.** `roiBackward` and `roiForward` agree for every
// symmetric kernel, so a stack of blurs cannot tell them apart -- section 1
// checks them on an asymmetric op no production code has yet, which is the
// only way the property can be pinned before the op that needs it arrives.
bool runBlurTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

  // The two error floors every tolerance in this section is derived from.
  // These are the *formats'* own rounding, not chosen numbers: no tolerance
  // below is a round figure picked to make a check pass, and each is stated as
  // the exact bound with a measurement backing it up in the same run.
  //
  // f16 store: a value in [0,1] rounds to half with at most half an ulp of
  // error, and the ulp just below 1.0 is 2^-11, so the bound is 2^-12.
  // uint8 coverage: core/SelectionMask quantises round-to-nearest on a 1/255
  // grid, so the bound is half of one step.
  const double halfFloor = std::ldexp(1.0, -12);
  const double uint8Floor = 0.5 / 255.0;
  double sampledHalfError = 0.0;
  for (int32_t i = 0; i < 100000; ++i) {
    const float v = blurTestNoise(static_cast<uint64_t>(i) + 0x51ed2701u);
    sampledHalfError = std::max(
        sampledHalfError, static_cast<double>(std::fabs(halfToFloat(floatToHalf(v)) - v)));
  }
  std::printf("  [selftest] blur: error floors -- f16 store %.4e (worst of 100k samples "
              "%.4e), uint8 coverage half-step %.4e\n",
              halfFloor, sampledHalfError, uint8Floor);
  check(sampledHalfError <= halfFloor && sampledHalfError > halfFloor * 0.5,
        "blur: the f16 store's worst rounding over [0,1] really is half an ulp below 1.0 "
        "and the sample reaches it -- every tolerance below is this bound, not a taste");

  // --- 1. ops/Roi: which way the walk goes -------------------------------
  {
    // Identity: a point op reads exactly the texel it writes. `RoiOp{}` is the
    // correct declaration for every class-A op, so this must be free.
    const PixelRect want{10, 20, 30, 40};
    check(roiBackward(RoiOp{}, want) == want && roiForward(RoiOp{}, want) == want,
          "roi: a point op's ROI is the identity in both directions -- levels and curves "
          "must not widen a single fetch");

    // A symmetric blur-shaped op. This is the case that CANNOT distinguish the
    // two directions, checked first so the asymmetric case below is visibly
    // the one doing the work.
    const RoiOp blur8 = roiDilateOp(8);
    check(roiBackward(blur8, want) == PixelRect{2, 12, 38, 48} &&
              roiForward(blur8, want) == PixelRect{2, 12, 38, 48},
          "roi: a SYMMETRIC kernel's backward and forward ROI are the same rectangle -- "
          "which is exactly why a stack of blurs cannot catch a walk that goes the wrong "
          "way");

    // The asymmetric op. Reaches 3 texels toward -x and 1 toward +x.
    // Backward: read [x0-3, x1+1). Forward: a source change is seen by outputs
    // [x-1, x+3], so the margins SWAP sides.
    const RoiOp lopsided{3, 1, 0, 0, 0, 0};
    check(roiBackward(lopsided, PixelRect{10, 0, 20, 1}) == PixelRect{7, 0, 21, 1},
          "roi: BACKWARD grows toward the side the kernel reaches -- 'to write here, read "
          "further left'");
    check(roiForward(lopsided, PixelRect{10, 0, 20, 1}) == PixelRect{9, 0, 23, 1},
          "roi: FORWARD reflects the kernel -- a left-reaching op makes a change visible to "
          "the RIGHT, so forward is not backward with a sign flipped");
    check(roiBackward(lopsided, PixelRect{10, 0, 20, 1}) !=
              roiForward(lopsided, PixelRect{10, 0, 20, 1}),
          "roi: and the two therefore disagree on an asymmetric op, which is the whole "
          "reason both functions exist");

    // Translation. An Offset of +5 means the picture moves right, so producing
    // output at x reads source at x-5.
    const RoiOp shift = roiOffsetOp(5, -2);
    check(roiBackward(shift, PixelRect{10, 10, 20, 20}) == PixelRect{5, 12, 15, 22},
          "roi: an offset op reads BEHIND its output by the shift -- getting this sign "
          "backwards moves the picture the wrong way and nothing else complains");
    check(roiForward(shift, PixelRect{10, 10, 20, 20}) == PixelRect{15, 8, 25, 18},
          "roi: and a source change lands AHEAD of it by the same shift");

    // The safety invariant: the round trip may overshoot, never undershoot.
    // Checked over a table including the asymmetric and translating cases,
    // because a new op type's first duty is to satisfy this.
    bool roundTripsHold = true;
    const RoiOp table[] = {RoiOp{}, blur8, lopsided, shift, RoiOp{2, 7, 5, 1, -4, 9}};
    const PixelRect probes[] = {PixelRect{0, 0, 1, 1}, PixelRect{-50, -50, -10, -10},
                                PixelRect{1000, 2000, 1128, 2128}};
    for (const RoiOp& op : table) {
      for (const PixelRect& r : probes) {
        if (!roiRoundTripContainsRequest(op, r)) roundTripsHold = false;
      }
    }
    check(roundTripsHold,
          "roi: forward(backward(want)) CONTAINS want for every op shape -- an ROI that is "
          "too big is slow, one that is too small is silently wrong at every tile edge");

    // The empty-rectangle trap. Expanding the canonical empty rectangle must
    // not conjure a rectangle around the origin.
    check(roiIsEmpty(roiExpandUniform(roiEmptyRect(), 64)) &&
              roiIsEmpty(roiBackward(blur8, roiEmptyRect())) &&
              roiIsEmpty(roiBackwardChain({blur8, blur8}, roiEmptyRect())),
          "roi: dilating an EMPTY rectangle yields nothing -- the naive x0 -= margin turns "
          "'nothing to redraw' into 'fetch the tiles around the origin'");
    check(roiUnion(roiEmptyRect(), want) == want && roiUnion(want, roiEmptyRect()) == want,
          "roi: empty is the identity for union, so folding dirty rectangles needs no "
          "first-iteration special case");

    // Composition, and the walk/fold equivalence.
    const std::vector<RoiOp> stack{roiDilateOp(4), shift, lopsided};
    check(roiComposeChain(stack) == RoiOp{7, 5, 4, 4, 5, -2},
          "roi: composing a stack adds margins and adds offsets -- the closed form that "
          "lets an evaluator ask 'how far does this stack reach' once instead of per tile");
    check(roiBackwardChain(stack, want) == roiBackward(roiComposeChain(stack), want) &&
              roiForwardChain(stack, want) == roiForward(roiComposeChain(stack), want),
          "roi: the step-by-step walk and the folded op agree in both directions -- so a "
          "cache may key on the fold without changing what gets fetched");
    // Honesty, printed rather than asserted: within this algebra composition
    // commutes, so a chain walked in the wrong ORDER returns the same
    // rectangle. The direction that IS observable is backward-vs-forward,
    // above. See ops/Roi.hpp.
    std::printf("  [selftest] blur: (note) RoiOp composition commutes, so chain ORDER is not "
                "yet observable; backward-vs-forward is, and is asserted above\n");

    // Tile ranges: the point where ROI stops being arithmetic and becomes a
    // fetch list. Rounding outward, and half-open on both ends.
    check(roiTileRange(PixelRect{0, 0, 128, 128}) == TileRange{0, 0, 1, 1},
          "roi: a rectangle exactly one tile wide is ONE tile, not two -- the half-open "
          "high edge must not claim the next tile");
    check(roiTileRange(PixelRect{0, 0, 129, 1}) == TileRange{0, 0, 2, 1} &&
              roiTileRange(PixelRect{127, 0, 129, 1}) == TileRange{0, 0, 2, 1},
          "roi: one texel past the boundary pulls in the whole next tile -- a fetch list "
          "rounds outward or the blur reads a tile it never asked for");
    check(roiTileRange(PixelRect{-1, -1, 1, 1}) == TileRange{-1, -1, 1, 1},
          "roi: negative document coordinates floor rather than truncate, so content "
          "dragged past the origin lands in the tile below zero and not in tile 0");
    check(roiTileRect(TileCoord{2, -1}) == PixelRect{256, -128, 384, 0},
          "roi: and a tile's own rectangle is the inverse of that mapping");
  }

  // --- 2. The kernel: DC, normalisation, apron, separability -------------
  {
    BlurParams g;
    g.sigma = 8.0f;
    check(blurApron(g) == 32 && blurRoiOp(g) == roiDilateOp(32),
          "blur: the Gaussian apron is ceil(4*sigma) and the ROI is exactly that dilation "
          "-- the apron and the ROI are one number, or a gather and a kernel disagree");
    BlurParams b;
    b.kind = BlurKind::Box;
    b.boxRadius = 5;
    check(blurApron(b) == 5 && blurKernel(b).size() == 11,
          "blur: a box of radius r is a 2r+1 kernel, so its apron is r");
    BlurParams zero;
    check(blurApron(zero) == 0 && blurKernel(zero).size() == 1 && blurKernel(zero)[0] == 1.0f,
          "blur: a zero-radius request is the identity, one unit tap -- a filter at its "
          "neutral setting must not perturb a value by an ulp");
    check(!blurParamsValid(BlurParams{BlurKind::Gaussian, -1.0f, 0}) &&
              !blurParamsValid(BlurParams{BlurKind::Box, 0.0f, -1}),
          "blur: a negative radius is REFUSED rather than clamped -- it is a caller bug and "
          "silently treating it as zero hides it");

    // Normalisation, in the only form that matters: a constant field must come
    // back constant. If the taps did not sum to 1 every flat region would
    // change brightness with the radius and the filter would be unusable at
    // any setting.
    for (const BlurParams p : {BlurParams{BlurKind::Gaussian, 12.0f, 0},
                               BlurParams{BlurKind::Box, 0.0f, 9}}) {
      const int32_t a = blurApron(p);
      const int32_t n = 4 * a + 64;
      std::vector<float> flat(static_cast<size_t>(n) * n, 0.5f);
      std::vector<float> out(flat.size(), 0.0f);
      blurPlane(flat.data(), n, n, 1, p, out.data());
      double worst = 0.0;
      for (int32_t y = a; y < n - a; ++y) {
        for (int32_t x = a; x < n - a; ++x) {
          worst = std::max(worst,
                           static_cast<double>(std::fabs(out[static_cast<size_t>(y) * n + x] -
                                                         0.5f)));
        }
      }
      check(worst == 0.0,
            "blur: a constant field blurs to itself EXACTLY -- the weights are normalised "
            "in double, so no radius changes a flat region's brightness");
    }

    // Separability is an identity, not an approximation: two 1-D passes must
    // equal the 2-D convolution they factor. Tolerance derived, not chosen --
    // the f32 accumulator's own error against a double reference, measured
    // here in the same run.
    {
      BlurParams p;
      p.sigma = 6.0f;
      const int32_t a = blurApron(p);
      const int32_t n = 128;
      std::vector<float> src(static_cast<size_t>(n) * n);
      for (size_t i = 0; i < src.size(); ++i) src[i] = blurTestNoise(i + 900000u);
      std::vector<float> got(src.size(), 0.0f);
      blurPlane(src.data(), n, n, 1, p, got.data());
      const std::vector<float> k = blurKernel(p);
      double worst = 0.0;
      for (int32_t y = a; y < n - a; ++y) {
        for (int32_t x = a; x < n - a; ++x) {
          double sum = 0.0;
          for (int32_t dy = -a; dy <= a; ++dy) {
            for (int32_t dx = -a; dx <= a; ++dx) {
              sum += static_cast<double>(k[static_cast<size_t>(dy + a)]) *
                     k[static_cast<size_t>(dx + a)] *
                     src[static_cast<size_t>(y + dy) * n + (x + dx)];
            }
          }
          worst = std::max(worst,
                           std::fabs(sum - got[static_cast<size_t>(y) * n + x]));
        }
      }
      std::printf("  [selftest] blur: separable two-pass vs direct 2-D convolution: %.4e\n",
                  worst);
      check(worst < halfFloor,
            "blur: the two 1-D passes equal a direct 2-D convolution to better than the f16 "
            "store can represent -- separable is exact arithmetic, not a fast approximation");
    }

    // The accumulator decision, re-measured rather than quoted. A half-
    // precision accumulator is what PLAN.md warns about for wide kernels, and
    // the point is that it is not a marginal loss.
    {
      const double sigma = 200.0;
      BlurParams p;
      p.sigma = static_cast<float>(sigma);
      const int32_t a = blurApron(p);
      const int32_t n = 3 * a;
      const std::vector<float> k = blurKernel(p);
      std::vector<float> step(static_cast<size_t>(n), 0.0f);
      for (int32_t i = n / 2; i < n; ++i) step[static_cast<size_t>(i)] = 1.0f;
      double worstF32 = 0.0, worstF16 = 0.0;
      for (int32_t i = a; i < n - a; i += 7) {
        double ref = 0.0;
        float accF32 = 0.0f;
        float accF16 = 0.0f;
        for (int32_t t = i - a; t <= i + a; ++t) {
          const float w = k[static_cast<size_t>(t - i + a)];
          const float v = step[static_cast<size_t>(t)];
          ref += static_cast<double>(w) * v;
          accF32 += w * v;
          accF16 = halfToFloat(floatToHalf(accF16 + halfToFloat(floatToHalf(w * v))));
        }
        worstF32 = std::max(worstF32, std::fabs(ref - accF32));
        worstF16 = std::max(worstF16, std::fabs(ref - accF16));
      }
      std::printf("  [selftest] blur: sigma=200 (%d taps) accumulator error -- f32 %.4e, "
                  "f16 %.4e, f16 store floor %.4e\n",
                  2 * a + 1, worstF32, worstF16, halfFloor);
      check(worstF32 < halfFloor * 0.01,
            "blur: the f32 accumulator's error on a 1601-tap kernel is two orders of "
            "magnitude below what the f16 store can hold -- so the storage format is the "
            "error floor and the accumulator is not");
      check(worstF16 > halfFloor,
            "blur: while an f16 accumulator would exceed that floor outright -- PLAN.md's "
            "'a 200px Gaussian accumulating in f16 loses low bits', as a number");
    }
  }

  // --- 3. The tile seam. The assertion this whole file exists for. -------
  {
    BlurParams p;
    p.sigma = 6.0f;
    const TileStore src = blurTestField(-1, 3, 0, 0);

    // Same blur, requested as one region and as two -- the split falling
    // exactly on the tile boundary.
    TileStore single, split;
    check(blurTiles(src, PixelRect{0, 0, 256, 128}, p, &single) &&
              blurTiles(src, PixelRect{0, 0, 128, 128}, p, &split) &&
              blurTiles(src, PixelRect{128, 0, 256, 128}, p, &split),
          "blur: (fixture) blurTiles accepts a whole-region request and two tile-sized ones");

    int32_t bitwiseDifferences = 0;
    for (int32_t tx = 0; tx <= 1; ++tx) {
      const Tile* a = single.find(TileCoord{tx, 0});
      const Tile* b = split.find(TileCoord{tx, 0});
      if (a == nullptr || b == nullptr ||
          std::memcmp(a->data(), b->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0) {
        ++bitwiseDifferences;
      }
    }
    check(bitwiseDifferences == 0,
          "blur: a blur split across a tile boundary is BIT-IDENTICAL to the same blur "
          "computed in one call -- every output texel's taps come from the apron, never "
          "from the tile it happens to live in");

    // The ragged, sparse case: a split at texel 77, nowhere near a tile edge,
    // over a store with a hole in the middle. Absent tiles read as transparent
    // black, so this also checks that "the tile does not exist" and "the tile
    // is empty" produce the same blur.
    {
      TileStore sparse;
      uint64_t counter = 5000;
      for (const int32_t tx : {0, 2}) {  // tile 1 deliberately missing
        Tile& t = sparse.getOrCreate(TileCoord{tx, 0});
        for (int32_t y = 0; y < kTileSize; ++y) {
          for (int32_t x = 0; x < kTileSize; ++x) {
            const float v = blurTestNoise(counter++);
            t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
          }
        }
      }
      TileStore whole, ragged;
      const PixelRect want{0, 0, 300, 100};
      blurTiles(sparse, want, p, &whole);
      blurTiles(sparse, PixelRect{0, 0, 77, 100}, p, &ragged);
      blurTiles(sparse, PixelRect{77, 0, 300, 100}, p, &ragged);
      check(storeMaxDiff(whole, ragged, want) == 0.0,
            "blur: and the same holds for a split that is NOT tile-aligned, over a sparse "
            "store with a missing tile -- the seam property is about the apron, not about "
            "getting lucky with tile boundaries");
    }

    // Proof that the assertion above is sensitive. Blur each tile as if it
    // were the whole world -- the bug -- and measure how wrong it is.
    {
      TileStore naive;
      for (int32_t tx = 0; tx <= 1; ++tx) {
        std::vector<float> plane(static_cast<size_t>(kTileSize) * kTileSize * 4, 0.0f);
        const Tile* t = src.find(TileCoord{tx, 0});
        for (int32_t y = 0; y < kTileSize; ++y) {
          for (int32_t x = 0; x < kTileSize; ++x) {
            const std::array<float, 4> px = t->readPixel(PixelCoord{x, y});
            const size_t base = (static_cast<size_t>(y) * kTileSize + x) * 4;
            for (int32_t c = 0; c < 4; ++c) plane[base + c] = px[static_cast<size_t>(c)];
          }
        }
        blurPlane(plane.data(), kTileSize, kTileSize, 4, p, plane.data());
        Tile& out = naive.getOrCreate(TileCoord{tx, 0});
        for (int32_t y = 0; y < kTileSize; ++y) {
          for (int32_t x = 0; x < kTileSize; ++x) {
            const size_t base = (static_cast<size_t>(y) * kTileSize + x) * 4;
            out.writePixel(PixelCoord{x, y},
                           {plane[base], plane[base + 1], plane[base + 2], plane[base + 3]});
          }
        }
      }
      const double atSeam = storeMaxDiff(single, naive, PixelRect{126, 0, 130, 128});
      const double inInterior = storeMaxDiff(single, naive, PixelRect{64, 0, 65, 128});
      std::printf("  [selftest] blur: a tile-local blur (the bug) is wrong by %.4f at the tile "
                  "boundary and %.4e in the tile interior\n",
                  atSeam, inInterior);
      check(atSeam > 0.1 && inInterior == 0.0,
            "blur: the tile-local blur this file rejects is wrong by a large fraction of "
            "full scale AT the seam and exactly right in the interior -- which is why the "
            "bug survives review, and why the check above is worth its cost");
    }

    // And against an independent reference: one flat float plane, gathered by
    // hand, blurred once. The only difference permitted is the f16 store.
    {
      const PixelRect want{0, 0, 256, 128};
      const PixelRect need = roiBackward(blurRoiOp(p), want);
      const int32_t w = need.width(), h = need.height();
      std::vector<float> plane(static_cast<size_t>(w) * h * 4, 0.0f);
      for (int32_t y = need.y0; y < need.y1; ++y) {
        for (int32_t x = need.x0; x < need.x1; ++x) {
          const size_t base =
              (static_cast<size_t>(y - need.y0) * w + (x - need.x0)) * 4;
          for (int32_t c = 0; c < 4; ++c) {
            plane[base + static_cast<size_t>(c)] = texelAt(src, x, y, c);
          }
        }
      }
      blurPlane(plane.data(), w, h, 4, p, plane.data());
      // Compared as an EXACTNESS claim rather than a tolerance. The reference
      // is a float and the store holds halves, so the honest question is not
      // "how far apart are they" -- which would need a per-value ulp bound and
      // would be a tolerance with a thumb on it -- but "is the stored half
      // exactly what this float rounds to". If the tiling contributed any
      // error at all, however small, some texel would round to a different
      // half and this would fail.
      size_t notTheRounding = 0;
      double worst = 0.0;
      for (int32_t y = want.y0; y < want.y1; ++y) {
        for (int32_t x = want.x0; x < want.x1; ++x) {
          const size_t base = (static_cast<size_t>(y - need.y0) * w + (x - need.x0)) * 4;
          const Tile* t = single.find(tileCoordAt(PixelCoord{x, y}));
          const std::array<float, 4> got = t->readPixel(tileLocalOffset(PixelCoord{x, y}));
          for (int32_t c = 0; c < 4; ++c) {
            const float reference = plane[base + static_cast<size_t>(c)];
            if (got[static_cast<size_t>(c)] != halfToFloat(floatToHalf(reference))) {
              ++notTheRounding;
            }
            worst = std::max(worst, static_cast<double>(
                                        std::fabs(got[static_cast<size_t>(c)] - reference)));
          }
        }
      }
      std::printf("  [selftest] blur: blurTiles vs one hand-gathered float plane -- %zu of "
                  "%lld texels differ from that float ROUNDED TO HALF; worst raw gap %.4e "
                  "(f16 store bound %.4e)\n",
                  notTheRounding, static_cast<long long>(roiTexelCount(want)) * 4, worst,
                  halfFloor);
      check(notTheRounding == 0,
            "blur: every texel the tiled path stored is EXACTLY the single-buffer blur's "
            "float rounded to half -- the tiling contributes zero error and the storage "
            "format contributes all of it");
    }

    // Refusals.
    TileStore dst;
    check(!blurTiles(src, PixelRect{0, 0, 16, 16}, p, nullptr) &&
              !blurTiles(src, roiEmptyRect(), p, &dst) &&
              !blurTiles(src, PixelRect{0, 0, 16, 16}, BlurParams{BlurKind::Box, 0.0f, -2},
                         &dst),
          "blur: a null destination, an empty rectangle and a negative radius are refused "
          "by name rather than half-performed");
    TileStore aliased = blurTestField(0, 0, 0, 0);
    check(!blurTiles(aliased, PixelRect{0, 0, 128, 128}, p, &aliased),
          "blur: blurring a store into ITSELF is refused -- the gather would read texels "
          "the scatter had already replaced, which produces a plausible smeared image "
          "rather than an obvious failure");
  }

  // --- 4. The domain: linear light, premultiplied alpha ------------------
  {
    // Linear light, as the one number that separates the two answers. Blur a
    // hard black/white step and read the midpoint: linear averaging gives 0.5.
    // Averaging sRGB CODES and decoding gives 0.214 -- DESIGN-imaging.md's
    // "muddy blurs", a factor of 2.3 on a mid-tone.
    {
      BlurParams p;
      p.sigma = 4.0f;
      TileStore src;
      Tile& t = src.getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = x >= 64 ? 1.0f : 0.0f;
          t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
        }
      }
      TileStore dst;
      blurTiles(src, PixelRect{0, 0, 128, 128}, p, &dst);
      const double lo = texelAt(dst, 63, 64, 0), hi = texelAt(dst, 64, 64, 0);
      std::printf("  [selftest] blur: black/white step blurred, the two centre texels read "
                  "%.5f and %.5f (sum %.5f)\n",
                  lo, hi, lo + hi);
      check(near(lo + hi, 1.0, 2.0 * halfFloor),
            "blur: the two texels straddling a black/white edge sum to 1.0 -- the kernel is "
            "symmetric and averages LINEAR light, not display codes (an sRGB-domain blur "
            "would put the midpoint at 0.214)");
      // Sampled where the whole apron is inside the painted region, on both
      // sides: at x = 100 every tap lands in white, at x = 30 every tap lands
      // in black. Deliberately NOT at x = 127, which is inside the apron of
      // the *store's* edge and therefore correctly reads below 1.0 -- the
      // painted region fades into transparency there, which is section 4's
      // next assertion and not a failure of this one.
      check(near(texelAt(dst, 100, 64, 0), 1.0, halfFloor) &&
                near(texelAt(dst, 30, 64, 0), 0.0, halfFloor),
            "blur: and a kernel's width away from the edge nothing moved at all -- a blur "
            "is local, so it must not perturb a flat region outside its own reach");
    }

    // Premultiplied, and the property that would fail if anyone "fixed" the
    // edge handling. An opaque square blurred into empty space fades to
    // TRANSPARENT, not to black: RGB and A fall together, so un-premultiplying
    // the soft edge still gives the original colour.
    {
      BlurParams p;
      p.sigma = 4.0f;
      TileStore src;
      Tile& t = src.getOrCreate(TileCoord{0, 0});
      for (int32_t y = 32; y < 96; ++y) {
        for (int32_t x = 32; x < 96; ++x) t.writePixel(PixelCoord{x, y}, {1, 0, 0, 1});
      }
      TileStore dst;
      blurTiles(src, PixelRect{0, 0, 128, 128}, p, &dst);
      double worstRatio = 0.0;
      float faintestA = 1.0f, faintestR = 1.0f;
      for (int32_t x = 20; x < 60; ++x) {
        const float a = texelAt(dst, x, 64, 3);
        const float r = texelAt(dst, x, 64, 0);
        if (a <= 1e-3f) continue;
        worstRatio = std::max(worstRatio, std::fabs(static_cast<double>(r) / a - 1.0));
        if (a < faintestA) {
          faintestA = a;
          faintestR = r;
        }
      }
      std::printf("  [selftest] blur: over the soft edge max |R/A - 1| = %.4e (faintest "
                  "sample R=%.5f A=%.5f)\n",
                  worstRatio, static_cast<double>(faintestR), static_cast<double>(faintestA));
      check(worstRatio <= 1e-5,
            "blur: un-premultiplying the soft edge still gives pure red -- RGB and ALPHA "
            "fall in lockstep, so a blurred cut-out fades to transparent rather than "
            "acquiring the black or white halo a straight-alpha blur produces");
      check(texelAt(dst, 5, 64, 3) == 0.0f,
            "blur: and far outside the shape there is nothing at all -- an absent tile is "
            "transparent black, which needs no special case because zero already is one");
    }
  }

  // --- 5. Box blur -------------------------------------------------------
  {
    BlurParams p;
    p.kind = BlurKind::Box;
    p.boxRadius = 2;
    // A single lit texel spreads into exactly 2r+1 texels of 1/(2r+1) each,
    // in each axis. Hand-computable, which is the point.
    const int32_t n = 32;
    std::vector<float> src(static_cast<size_t>(n) * n, 0.0f);
    src[static_cast<size_t>(16) * n + 16] = 1.0f;
    std::vector<float> out(src.size(), 0.0f);
    blurPlane(src.data(), n, n, 1, p, out.data());
    const double expected = 1.0 / 25.0;  // (2r+1)^2 = 25 texels share it
    bool flat = true;
    double total = 0.0;
    for (int32_t y = 14; y <= 18; ++y) {
      for (int32_t x = 14; x <= 18; ++x) {
        const double v = out[static_cast<size_t>(y) * n + x];
        total += v;
        if (!near(v, expected, 1e-6)) flat = false;
      }
    }
    check(flat && near(total, 1.0, 1e-6),
          "box: one lit texel spreads to exactly (2r+1)^2 texels of equal weight summing to "
          "1 -- a box kernel is flat and loses nothing, which is what makes it the right "
          "prefilter for a decimation");
    check(out[static_cast<size_t>(16) * n + 19] == 0.0f,
          "box: and stops dead at r+1 -- the support is exactly the window, so its apron is "
          "exactly r");
    // The divisor is the FULL window even at the buffer edge, not the number
    // of texels actually present. The alternative -- renormalising at the edge
    // -- is a different operator that cannot be gathered with an apron and
    // cropped, which is to say it reintroduces the seam.
    std::vector<float> ones(static_cast<size_t>(n) * n, 1.0f);
    std::vector<float> edge(ones.size(), 0.0f);
    blurPlane(ones.data(), n, n, 1, p, edge.data());
    check(near(edge[0], (3.0 / 5.0) * (3.0 / 5.0), 1e-6),
          "box: at the corner of the buffer the window is divided by its FULL width, so the "
          "result falls off -- a box that renormalised at its edges could not be gathered "
          "with an apron and cropped, which is the seam bug wearing a different hat");
  }

  // --- 6. PRD E4's feather (ops/Feather) ---------------------------------
  {
    const Selection hard = selectRectangle(32.0f, 32.0f, 96.0f, 96.0f);
    auto cov = [](const Selection& s, int32_t x, int32_t y) {
      return selectionCoverageAt(&s, PixelCoord{x, y});
    };

    check(featherSigmaForRadius(8.0f) == 4.0f && blurApron(featherBlurParams(8.0f)) == 16,
          "feather: radius maps to sigma = radius/2, so the soft band is `radius` texels "
          "either side of the edge rather than about 2.5 times the number the user typed");

    const Selection soft = featherSelection(hard, 8.0f);
    std::printf("  [selftest] feather: r=8 profile across the edge at x=32: %.5f %.5f | "
                "%.5f %.5f, at -r %.5f, at +r %.5f\n",
                cov(soft, 30, 64), cov(soft, 31, 64), cov(soft, 32, 64), cov(soft, 33, 64),
                cov(soft, 24, 64), cov(soft, 40, 64));

    // The conservation property, and the sharpest thing this section asserts:
    // a symmetric normalised kernel moves coverage across the edge without
    // creating or destroying any.
    check(near(static_cast<double>(cov(soft, 31, 64)) + cov(soft, 32, 64), 1.0, 2.0 * uint8Floor),
          "feather: the coverage pair straddling the original edge sums to 1.0 -- what the "
          "feather takes from inside appears outside, so the 50% contour does not move and "
          "a mask cannot creep every time its radius is adjusted");
    check(cov(soft, 32, 64) > 0.4f && cov(soft, 32, 64) < 0.6f,
          "feather: and the edge texel itself sits near half coverage rather than near 0 or "
          "1 -- the softening is centred on the edge, not offset from it");
    check(cov(soft, 64, 64) == 1.0f,
          "feather: the interior stays FULLY selected -- a feather softens a boundary, it "
          "does not dim the selection");
    check(cov(soft, 24, 64) > 0.0f && cov(soft, 24, 64) < 0.1f &&
              cov(soft, 40, 64) > 0.9f && cov(soft, 40, 64) < 1.0f,
          "feather: `radius` texels outside the edge a few percent remains and `radius` "
          "inside a few percent is missing -- the transition is essentially contained in "
          "the band the dialog's number describes");

    bool monotone = true;
    float previous = -1.0f;
    for (int32_t x = 0; x <= 64; ++x) {
      const float v = cov(soft, x, 64);
      if (v < previous) monotone = false;
      previous = v;
    }
    check(monotone,
          "feather: the falloff is monotone across the edge -- a ripple here would be the "
          "signature of a box approximation standing in for the Gaussian");

    // The requirement that put this file in the blur track: an absent
    // selection tile means 0.0, so feathering an edge has to write coverage
    // into tiles the input did not have.
    {
      const Selection nearEdge = selectRectangle(64.0f, 0.0f, 128.0f, 64.0f);
      const Selection spread = featherSelection(nearEdge, 8.0f);
      check(nearEdge.tiles.occupiedTileCount() == 1 &&
                spread.tiles.occupiedTileCount() > 1 && cov(spread, 128, 32) > 0.4f,
            "feather: a marquee flush against a tile edge feathers INTO the neighbouring "
            "tile the input never had -- an absent selection tile is 0.0, so the blur must "
            "read and write beyond the stored set or the feather is cut off on the tile "
            "grid");
      check(cov(spread, 128 + 16, 32) == 0.0f,
            "feather: and stops within the apron rather than smearing on -- 2*radius out, "
            "coverage is below half a uint8 step and quantises to nothing");
      std::printf("  [selftest] feather: one input tile became %zu output tiles (9 were "
                  "within the apron; the rest quantised to zero and were dropped)\n",
                  spread.tiles.occupiedTileCount());
      check(spread.tiles.occupiedTileCount() < 9,
            "feather: tiles whose feathered coverage rounds entirely to zero are DROPPED, "
            "not stored as 16 KiB of 'not selected' -- core/SelectionMask's constructor "
            "invariant, applied to the apron");
    }

    // The tile-seam property again, in coverage: a shape centred on a tile
    // boundary must feather symmetrically across it.
    {
      const Selection straddling = selectRectangle(96.0f, 0.0f, 160.0f, 64.0f);
      const Selection f = featherSelection(straddling, 8.0f);
      double worst = 0.0;
      for (int32_t d = 0; d < 48; ++d) {
        worst = std::max(worst, static_cast<double>(
                                    std::fabs(cov(f, 127 - d, 32) - cov(f, 128 + d, 32))));
      }
      check(worst == 0.0,
            "feather: a shape centred on the x=128 tile boundary feathers EXACTLY "
            "symmetrically across it -- the seam property, checked on the store whose "
            "absent tiles mean the opposite of a layer mask's");
    }

    // Against an independent reference. The tolerance is the uint8 grid, and
    // nothing else: the operator is exact and the store is the error.
    {
      const float radius = 6.0f;
      const Selection got = featherSelection(hard, radius);
      const BlurParams p = featherBlurParams(radius);
      const int32_t a = blurApron(p);
      const std::vector<float> k = blurKernel(p);
      double worst = 0.0;
      for (int32_t y = 32 - a; y < 96 + a; y += 3) {
        for (int32_t x = 32 - a; x < 96 + a; x += 3) {
          double sum = 0.0;
          for (int32_t dy = -a; dy <= a; ++dy) {
            for (int32_t dx = -a; dx <= a; ++dx) {
              sum += static_cast<double>(k[static_cast<size_t>(dy + a)]) *
                     k[static_cast<size_t>(dx + a)] * cov(hard, x + dx, y + dy);
            }
          }
          worst = std::max(worst, std::fabs(sum - cov(got, x, y)));
        }
      }
      std::printf("  [selftest] feather: vs a direct 2-D convolution of the coverage field: "
                  "%.4e (uint8 half-step %.4e)\n",
                  worst, uint8Floor);
      check(worst <= uint8Floor,
            "feather: agrees with a direct 2-D convolution to within HALF A UINT8 STEP and "
            "no more -- the feather itself is exact and core/SelectionMask's 8-bit grid is "
            "the entire error");
    }

    // The degenerate radii and the two absences.
    {
      const Selection unchanged = featherSelection(hard, 0.0f);
      check(unchanged.tiles.occupiedTileCount() == hard.tiles.occupiedTileCount() &&
                unchanged.tiles.sharedTileCount() == unchanged.tiles.occupiedTileCount(),
            "feather: radius 0 returns the selection SHARED rather than copied -- a filter "
            "at its neutral setting costs a refcount, not 16 KiB a tile");
      check(cov(featherSelection(hard, -3.0f), 64, 64) == 1.0f,
            "feather: a negative radius is the identity too, rather than an inverted or "
            "empty selection");

      Selection nothing;
      const Selection stillNothing = featherSelection(nothing, 8.0f);
      check(stillNothing.tiles.occupiedTileCount() == 0 &&
                selectionSelectsNothing(stillNothing),
            "feather: an ENGAGED selection that selects nothing feathers to the same thing "
            "-- blurring zero is zero, and it must not become 'no selection', which means "
            "the opposite");
    }
  }

  std::printf("[selftest] blur %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
