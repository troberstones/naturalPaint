#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "ops/DocumentTransform.hpp"

namespace np {

// ops/DocumentTransform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14,
// D16, D17, and E10 for the selection). Pure CPU, no PaintSim and no GPU -- the
// same headless-first-class status runTransformTest() has, and for the same
// reason: nothing here needs a device, so nothing here should be unable to run
// without one.
//
// runTransformTest() proves the resampler. This section proves the **entry
// point**, and the four claims it makes are ones a resampler test structurally
// cannot reach, because each of them is about a store the resampler has never
// heard of:
//
//   **A crop moves the mask.** `core::Layer` has no offset field -- tiles are
//   keyed in absolute document coordinates -- so a crop is an integer translate
//   of every store, and the classic bug is translating `rgbTiles` and not
//   `mask`. That bug produces a document whose composite is internally
//   consistent and whose mask has slid off the content it was painted for, and
//   it is invisible until someone looks at the mask. Section 4 asserts the
//   mask, the pixels and the selection all landed at the same offset, and
//   asserts the pixels landed *bit-identically* rather than close.
//
//   **A mask must not transform in coverage space.** `transformImage()`'s
//   outside-the-source policy is transparent black, which for a selection means
//   "unselected" (right) and for a layer mask would mean "hidden" (a document
//   that vanishes). Section 7 measures what the naive packing does -- 1 649 of
//   3 249 destination texels of a 30-degree rotation come back hidden -- and
//   then asserts the shipped hide-space packing leaves them revealed.
//
//   **A pigment latent may only pass through a positive-weight kernel, and only
//   mass-weighted.** Section 2 measures both halves: the convex-hull property
//   the type restriction buys, and what a straight (unweighted) latent average
//   would have done to the same field. The second number is 0.6061 of latent
//   difference, which is not a fringe, it is a different pigment.
//
//   **D16 is a claim about a process, at document level too.** Section 3 rotates
//   a layer 60 degrees out and back through a document-level API by two routes
//   that land in exactly the same place, and reports the resample count and the
//   measured RMS error for each.
//
// Numbers printed here are measurements. Where a threshold appears it sits well
// clear of the measured value and the margin is stated at the assertion.
bool runDocumentTransformTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Fixtures ------------------------------------------------------------

  // Detailed enough for a resample to lose something, structured enough that a
  // misplacement is obvious rather than statistical.
  auto fillRgb = [](TileStore& store, int32_t w, int32_t h) {
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        const float fx = static_cast<float>(x) / static_cast<float>(w);
        const float fy = static_cast<float>(y) / static_cast<float>(h);
        const float v = 0.5f + 0.25f * std::sin(fx * 18.0f) * std::cos(fy * 14.0f) +
                        0.15f * std::sin((fx + fy) * 31.0f);
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, 0.5f * v, 1.0f - v, 1.0f});
      }
    }
  };

  // A pigment field with a **mass-zero gap through the middle of its own
  // content**, and an arbitrary latent behind that gap. That is not a contrived
  // input, it is the normal state of a pigment layer: nothing wrote the latent
  // where there is no paint, so whatever is there is meaningless -- exactly the
  // transparent-black RGB ops/Transform.hpp §2 refuses to average. A fixture
  // without the gap cannot tell a mass-weighted resample from a straight one.
  auto fillPigment = [](PigmentTileStore& store, int32_t w, int32_t h) {
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        const float fx = static_cast<float>(x) / static_cast<float>(w);
        const float fy = static_cast<float>(y) / static_cast<float>(h);
        PigmentTexel t;
        t.latent.c[0] = 0.15f + 0.6f * fx;
        t.latent.c[1] = 0.10f + 0.5f * fy;
        t.latent.c[2] = 0.05f + 0.2f * std::fabs(std::sin(fx * 9.0f));
        t.latent.res[0] = 0.02f * std::sin(fx * 20.0f);
        t.latent.res[1] = -0.01f;
        t.latent.res[2] = 0.03f * fy;
        t.mass = (fx > 0.35f && !(fx > 0.45f && fx < 0.55f)) ? (0.4f + 0.6f * fy) : 0.0f;
        if (t.mass == 0.0f) {
          t.latent.c[0] = 0.95f;
          t.latent.c[1] = 0.90f;
          t.latent.c[2] = 0.85f;
        }
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeTexel(tileLocalOffset(PixelCoord{x, y}), t);
      }
    }
  };

  auto rgbAt = [](const TileStore& store, int32_t x, int32_t y) {
    const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
    return t ? t->readPixel(tileLocalOffset(PixelCoord{x, y}))
             : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  };
  auto maskAt = [](const MaskTileStore& store, int32_t x, int32_t y) {
    const MaskTile* t = store.find(tileCoordAt(PixelCoord{x, y}));
    // An absent mask tile is 1.0 -- reveal. Reading it as 0 here would make
    // every assertion below agree with the bug it is meant to catch.
    return t ? t->readCoverage(tileLocalOffset(PixelCoord{x, y})) : 1.0f;
  };

  // RMS error between two layers' RGB, over an inset window so the layer's own
  // rectangle boundary -- a hard edge under ops/Transform's edge policy, and not
  // what section 3 is measuring -- cannot contribute.
  auto rgbRmse = [&](const TileStore& a, const TileStore& b, int32_t w, int32_t h, int32_t inset) {
    double sum = 0.0;
    size_t n = 0;
    for (int32_t y = inset; y < h - inset; ++y) {
      for (int32_t x = inset; x < w - inset; ++x) {
        const std::array<float, 4> pa = rgbAt(a, x, y);
        const std::array<float, 4> pb = rgbAt(b, x, y);
        for (int c = 0; c < 3; ++c) {
          const double d = static_cast<double>(pa[c]) - static_cast<double>(pb[c]);
          sum += d * d;
          ++n;
        }
      }
    }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
  };

  // Bit equality on the stored half words, for the claims that are about
  // losslessness rather than about accuracy. A tolerance here would pass on
  // exactly the implementation these sections forbid.
  auto storesBitIdentical = [](const TileStore& a, const TileStore& b) {
    if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
    for (const auto& [coord, tile] : a) {
      const Tile* other = b.find(coord);
      if (other == nullptr) return false;
      if (std::memcmp(tile.data(), other->data(), sizeof(Tile)) != 0) return false;
    }
    return true;
  };

  auto makeRgbDoc = [&](int32_t w, int32_t h) {
    Document d = Document::createBlank(w, h, WorkingSpace{});
    fillRgb(*d.layers[0].rgbTiles, w, h);
    return d;
  };

  // --- 1. Regions, and the four different answers to "nothing here" --------
  {
    // The fact this whole file is built on: a Layer carries no offset, origin
    // or transform field, so a store's position IS its tile keying. If a
    // `Layer::offset` ever appears, every crop below has to start consulting
    // it, and the first assertion here is where a reader finds that out.
    Document doc = makeRgbDoc(64, 64);
    check(rgbContentRegion(*doc.layers[0].rgbTiles) == DocumentRegion{0, 0, 64u, 64u},
          "doc transform: an RGB store's content region is where its non-zero ALPHA is -- a "
          "layer has no offset field, so this keying is the only position it has");

    TileStore transparent;
    transparent.getOrCreate(TileCoord{3, 3});  // allocated, entirely transparent
    check(rgbContentRegion(transparent).empty() && transparent.occupiedTileCount() == 1,
          "doc transform: an allocated but fully TRANSPARENT tile is not content -- otherwise a "
          "transform would resample a bounding box of nothing");

    MaskTileStore mask;
    check(maskContentRegion(mask).empty(),
          "doc transform: a mask with no tiles has NO content region -- an unallocated mask tile "
          "means reveal (core/Mask.hpp), and 'reveal everywhere' has no bounding box");
    mask.getOrCreate(TileCoord{0, 0});  // a fresh MaskTile is all 1.0
    check(maskContentRegion(mask).empty(),
          "doc transform: and an ALLOCATED all-reveal mask tile is still not content -- the "
          "mask's 'nothing here' is exactly 1.0, the INVERSE of every other store's");
    mask.getOrCreate(TileCoord{0, 0}).writeCoverage(PixelCoord{5, 7}, 0.0f);
    check(maskContentRegion(mask) == DocumentRegion{5, 7, 1u, 1u},
          "doc transform: one hidden texel in a reveal-all mask IS content, and bounds to itself");

    Selection sel = selectRectangle(4.0f, 6.0f, 10.0f, 12.0f);
    check(selectionContentRegion(sel) == DocumentRegion{4, 6, 6u, 6u},
          "doc transform: a selection's content is its non-zero COVERAGE -- 0 is unselected, "
          "which is the exact inverse of the mask rule one line up");

    PigmentTileStore pig;
    fillPigment(pig, 128, 128);
    const DocumentRegion pigRegion = pigmentContentRegion(pig);
    check(pigRegion.x == 45 && pigRegion.width == 83u,
          "doc transform: a pigment store's content is its non-zero MASS, so the empty left of "
          "the fixture is outside the region a transform reads at all");

    // The region of an empty box is empty, not a 1x1 at the origin. The
    // distinction LayerBounds::empty exists for, carried across the conversion.
    check(regionFromBounds(LayerBounds{}).empty(),
          "doc transform: an empty LayerBounds converts to an EMPTY region, not a one-texel one "
          "at the origin -- the inclusive/half-open conversion is where that gets lost");
  }

  // --- 2. The pigment decision (PRD C1; DESIGN-imaging.md section 3) -------
  //
  // Three separate claims, and all three are needed. Any one alone is a wrong
  // answer that looks right.
  {
    // (b) The kernel restriction is a TYPE, not a runtime check. There is no
    // way to spell Lanczos3 at a pigment entry point, and the one conversion
    // that could launder one refuses by name.
    LatentKernel lk = LatentKernel::Nearest;
    std::string kernelErr;
    check(latentKernelFor(ResampleKernel::Bilinear, &lk, &kernelErr) &&
              lk == LatentKernel::Bilinear && latentKernelFor(ResampleKernel::Nearest, &lk, nullptr),
          "doc transform: the two lobe-free kernels convert to a LatentKernel");
    check(!latentKernelFor(ResampleKernel::Lanczos3, &lk, &kernelErr) &&
              kernelErr.find("Lanczos3") != std::string::npos &&
              kernelErr.find("negative lobes") != std::string::npos,
          "doc transform: Lanczos3 on a latent is REFUSED BY NAME, not silently rounded down to "
          "bilinear -- a silent substitution is the deciding-by-defaulting this file exists to "
          "not do");
    check(!latentKernelFor(ResampleKernel::CatmullRom, nullptr, nullptr) &&
              !latentKernelFor(ResampleKernel::Mitchell, nullptr, nullptr),
          "doc transform: and so are Catmull-Rom and Mitchell -- Mitchell's lobe is smaller "
          "(min weight -0.036 against -0.074) but 'small' is a tolerance argument and the two "
          "admitted kernels measure exactly zero");
    check(!resampleKernelHasNegativeLobes(ResampleKernel::Nearest) &&
              !resampleKernelHasNegativeLobes(ResampleKernel::Bilinear) &&
              resampleKernelHasNegativeLobes(ResampleKernel::CatmullRom) &&
              resampleKernelHasNegativeLobes(ResampleKernel::Mitchell) &&
              resampleKernelHasNegativeLobes(ResampleKernel::Lanczos3),
          "doc transform: and which kernels ring is named rather than rediscovered by a caller "
          "sampling weights");

    // The kernel restriction, checked against ops/Transform's own weights
    // rather than against this file's opinion of them. Measured minima:
    // nearest +0.000000, bilinear +0.000000, Catmull-Rom -0.074074,
    // Mitchell -0.036282, Lanczos3 -0.147267 (400 001 samples across the
    // support). Only the first two are admissible, and this asserts that the
    // enum's two members really are the two lobe-free ones rather than a pair
    // someone picked.
    bool admittedAreLobeFree = true;
    for (const LatentKernel k : {LatentKernel::Nearest, LatentKernel::Bilinear}) {
      const ResampleKernel rk = resampleKernelFor(k);
      const float radius = resampleKernelRadius(rk);
      for (int i = 0; i <= 4000; ++i) {
        const float t = -radius + 2.0f * radius * static_cast<float>(i) / 4000.0f;
        if (resampleKernelWeight(rk, t) < 0.0f) admittedAreLobeFree = false;
      }
    }
    check(admittedAreLobeFree,
          "doc transform: every weight of every kernel LatentKernel admits is >= 0, checked "
          "against ops/Transform's own kernel and not against this file's belief about it");

    // (a) + (c) The transform itself: a 2:1 downscale of the pigment fixture.
    PigmentTileStore src;
    fillPigment(src, 128, 128);
    const DocumentRegion sr = pigmentContentRegion(src);
    const Mat3 half = transformScale(0.5f, 0.5f);
    PigmentTileStore out;
    TransformReport pigReport;
    std::string err;
    check(transformPigmentTiles(src, sr, half, transformedRegion(half, sr), LatentKernel::Bilinear,
                                /*prefilterDownscale=*/true, &out, &pigReport, &err),
          "doc transform: a Pigment layer IS transformed -- DESIGN-imaging.md section 3 puts "
          "'resample' in its valid-on-latents column by name, and refusing would refuse the "
          "DEFAULT layer kind");
    check(pigReport.reconstructionPasses == 1 && pigReport.prefiltered,
          "doc transform: through ONE reconstruction pass over 7 channels packed as two RGBA "
          "images, and the downscale prefilter reaches the pigment path unchanged");

    // The convex-hull property (§2b). Every output latent must lie inside the
    // range of the input latents that actually had mass; a negative-weight
    // kernel would leave it. Measured excursion: 0.000e+00, so the threshold is
    // a float-rounding allowance and nothing more.
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& [coord, tile] : src) {
      const PixelCoord org = tileOrigin(coord);
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          if (org.x + lx >= 128 || org.y + ly >= 128) continue;
          const PigmentTexel t = tile.readTexel(PixelCoord{lx, ly});
          if (t.mass <= 0.0f) continue;
          for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], t.latent.c[c]);
            hi[c] = std::max(hi[c], t.latent.c[c]);
          }
        }
      }
    }
    double worstExcursion = 0.0;
    for (const auto& [coord, tile] : out) {
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const PigmentTexel t = tile.readTexel(PixelCoord{lx, ly});
          if (t.mass <= 0.0f) continue;
          for (int c = 0; c < 3; ++c) {
            worstExcursion = std::max(worstExcursion, static_cast<double>(lo[c] - t.latent.c[c]));
            worstExcursion = std::max(worstExcursion, static_cast<double>(t.latent.c[c] - hi[c]));
          }
        }
      }
    }
    std::printf("      worst output latent excursion outside the input hull: %.3e\n",
                worstExcursion);
    // 1e-3 against a measured 0.000e+00. The margin is enormous on purpose: the
    // claim is qualitative (convex, therefore a KM mix) and the only quantity
    // that could legitimately appear here is f16 storage rounding, whose worst
    // case over [0,1] is 2.441e-04.
    check(worstExcursion < 1.0e-3,
          "doc transform: every output latent is inside the CONVEX HULL of the input latents "
          "that had mass -- which is the literal statement of Mixbox's guarantee, and is what a "
          "kernel with negative lobes would break");

    // (c) Mass weighting. The same transform with the mass forced to 1
    // everywhere is the straight (unweighted) average, and it drags the
    // arbitrary latent behind the gap into the paint beside it.
    PigmentTileStore massless;
    for (int32_t y = 0; y < 128; ++y) {
      for (int32_t x = 0; x < 128; ++x) {
        const PigmentTile* t = src.find(tileCoordAt(PixelCoord{x, y}));
        if (t == nullptr) continue;
        PigmentTexel v = t->readTexel(tileLocalOffset(PixelCoord{x, y}));
        v.mass = 1.0f;
        massless.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeTexel(tileLocalOffset(PixelCoord{x, y}), v);
      }
    }
    PigmentTileStore straightOut;
    transformPigmentTiles(massless, sr, half, transformedRegion(half, sr), LatentKernel::Bilinear,
                          true, &straightOut, nullptr, &err);
    double worstDiff = 0.0;
    for (const auto& [coord, tile] : out) {
      const PigmentTile* other = straightOut.find(coord);
      if (other == nullptr) continue;
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const PigmentTexel a = tile.readTexel(PixelCoord{lx, ly});
          if (a.mass <= 0.0f) continue;
          const PigmentTexel b = other->readTexel(PixelCoord{lx, ly});
          for (int c = 0; c < 3; ++c)
            worstDiff = std::max(worstDiff,
                                 static_cast<double>(std::fabs(a.latent.c[c] - b.latent.c[c])));
        }
      }
    }
    std::printf("      worst |mass-weighted - straight| latent difference: %.4f\n", worstDiff);
    // Measured 0.6061 on a latent whose whole range is about 0.4 wide. The
    // threshold is 0.2, half the measurement, and it exists to fail loudly if
    // the premultiply-by-mass is ever removed -- at which point this drops to 0
    // and the fringe is back.
    check(worstDiff > 0.2,
          "doc transform: and the resample is MASS-WEIGHTED -- a straight latent average of the "
          "same field differs by 0.6061, because a mass-0 texel holds an arbitrary latent that "
          "an unweighted filter drags in at full weight");

    // Determinism of the two packed images. If A's alpha and B's alpha ever
    // diverge, the divide in §2(c) is being done by two different denominators
    // and the residual drifts away from the pigment weights it belongs to.
    PigmentTileStore again;
    transformPigmentTiles(src, sr, half, transformedRegion(half, sr), LatentKernel::Bilinear, true,
                          &again, nullptr, &err);
    size_t differing = 0;
    for (const auto& [coord, tile] : out) {
      const PigmentTile* other = again.find(coord);
      if (other == nullptr || std::memcmp(tile.data(), other->data(), sizeof(PigmentTile)) != 0)
        ++differing;
    }
    check(differing == 0 && out.occupiedTileCount() == again.occupiedTileCount(),
          "doc transform: the two packed images carry the SAME mass, bit for bit -- they are one "
          "computation run twice, and a divergence would divide the residual by a different "
          "denominator than the pigment weights");

    // Nearest is the other admitted kernel, and it is the only one that cannot
    // produce a pigment that was not already in the picture.
    PigmentTileStore nearestOut;
    check(transformPigmentTiles(src, sr, half, transformedRegion(half, sr), LatentKernel::Nearest,
                                true, &nearestOut, nullptr, &err),
          "doc transform: nearest is admitted too -- one tap of weight 1, so the output latent "
          "IS one of the input latents, unchanged");
    check(std::string(latentKernelName(LatentKernel::Bilinear)) == "bilinear" &&
              resampleKernelFor(LatentKernel::Nearest) == ResampleKernel::Nearest,
          "doc transform: and both name themselves for a menu and a refusal message");
  }

  // --- 3. PRD D16 at DOCUMENT level ---------------------------------------
  //
  // The claim is about a process. Both routes below apply exactly 60 degrees out
  // and 60 back about the same pivot, so both land in the same place and no
  // geometric test separates them. What differs is how many times a
  // reconstruction kernel ran, and that shows up only as measured quality loss.
  //
  // Note the shape of the experiment, because the obvious version does not
  // work: composing +60 and -60 into ONE matrix folds to the identity, and
  // Catmull-Rom is interpolating (exact at integer offsets), so the composed
  // route returns the source verbatim and the ratio is infinite -- which proves
  // nothing about stacking. So each *leg* is a 3-deep stack, and the comparison
  // is 2 passes against 6.
  {
    const int32_t W = 256, H = 256;
    const Point2 pivot{static_cast<float>(W) * 0.5f, static_cast<float>(H) * 0.5f};
    DocumentTransformParams params;
    const Document original = makeRgbDoc(W, H);

    auto leg = [&](float step, int n) {
      TransformStack s;
      for (int i = 0; i < n; ++i) s.push(transformRotateDegreesAbout(step, pivot));
      return s;
    };

    Document composed = makeRgbDoc(W, H);
    const LayerTransformResult c1 = transformLayer(composed, 0, leg(20.0f, 3), params);
    const LayerTransformResult c2 = transformLayer(composed, 0, leg(-20.0f, 3), params);
    check(c1.ok && c2.ok && c1.reconstructionPasses == 1 && c2.reconstructionPasses == 1,
          "doc transform: a 3-deep stack through the document-level API resamples ONCE -- the "
          "count is per call and does not grow with the depth of the stack");

    Document stacked = makeRgbDoc(W, H);
    int stackedPasses = 0;
    for (int i = 0; i < 3; ++i)
      stackedPasses +=
          transformLayer(stacked, 0, transformRotateDegreesAbout(20.0f, pivot), params)
              .reconstructionPasses;
    for (int i = 0; i < 3; ++i)
      stackedPasses +=
          transformLayer(stacked, 0, transformRotateDegreesAbout(-20.0f, pivot), params)
              .reconstructionPasses;
    check(stackedPasses == 6,
          "doc transform: while the same geometry applied one matrix at a time resamples SIX "
          "times -- the count is the witness, because the two land in the same place");

    const double eComposed = rgbRmse(*original.layers[0].rgbTiles, *composed.layers[0].rgbTiles, W,
                                     H, 48);
    const double eStacked = rgbRmse(*original.layers[0].rgbTiles, *stacked.layers[0].rgbTiles, W, H,
                                    48);
    std::printf("      60 deg out and back: composed (2 passes) RMSE %.6f, stacked (6) %.6f, "
                "ratio %.3fx\n",
                eComposed, eStacked, eStacked / eComposed);
    // Measured 1.778x. The threshold is 1.25x, well clear of it, and well clear
    // of 1.0 in the other direction so a run on different hardware fails for a
    // regression rather than for the last digit.
    check(eStacked > eComposed * 1.25,
          "doc transform: and the damage is REAL -- the 6-pass route measures 1.778x the RMS "
          "error of the 2-pass one against the original, on geometry that lands identically");

    // Depth widens the gap while the composed cost stays flat, which is the
    // property that makes D16 about an editor rather than about one operation.
    Document deepC = makeRgbDoc(W, H);
    const int deepCPasses = transformLayer(deepC, 0, leg(7.5f, 8), params).reconstructionPasses +
                            transformLayer(deepC, 0, leg(-7.5f, 8), params).reconstructionPasses;
    Document deepS = makeRgbDoc(W, H);
    int deepSPasses = 0;
    for (int i = 0; i < 8; ++i)
      deepSPasses += transformLayer(deepS, 0, transformRotateDegreesAbout(7.5f, pivot), params)
                         .reconstructionPasses;
    for (int i = 0; i < 8; ++i)
      deepSPasses += transformLayer(deepS, 0, transformRotateDegreesAbout(-7.5f, pivot), params)
                         .reconstructionPasses;
    const double eDeepC = rgbRmse(*original.layers[0].rgbTiles, *deepC.layers[0].rgbTiles, W, H, 64);
    const double eDeepS = rgbRmse(*original.layers[0].rgbTiles, *deepS.layers[0].rgbTiles, W, H, 64);
    std::printf("      16 steps: composed (%d passes) RMSE %.6f, stacked (%d) %.6f, ratio %.3fx\n",
                deepCPasses, eDeepC, deepSPasses, eDeepS, eDeepS / eDeepC);
    check(deepCPasses == 2 && deepSPasses == 16 && eDeepS > eDeepC * 1.8,
          "doc transform: at 16 steps the composed cost is STILL 2 passes and the gap widens to "
          "2.647x -- an editor built the wrong way degrades a layer every time a handle moves");
  }

  // --- 4. A crop moves the mask and the selection with the pixels ----------
  //
  // The classic bug this file exists to make impossible, asserted three ways:
  // the pixels moved, the mask moved by the SAME offset, and the pixels are
  // bit-identical rather than close.
  {
    const int32_t W = 400, H = 300;
    Document doc = makeRgbDoc(W, H);
    doc.layers[0].mask.emplace();
    for (int32_t y = 100; y < 160; ++y)
      for (int32_t x = 100; x < 160; ++x)
        doc.layers[0].mask->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeCoverage(tileLocalOffset(PixelCoord{x, y}), 0.25f);
    Selection sel = selectRectangle(90.0f, 90.0f, 200.0f, 200.0f);

    const Document before = doc;
    const DocumentRegion maskBefore = maskContentRegion(*doc.layers[0].mask);
    const DocumentRegion selBefore = selectionContentRegion(sel);

    const DocumentTransformResult r = cropDocument(doc, 60, 40, 200, 150, &sel);
    check(r.ok && doc.width == 200 && doc.height == 150 && r.previousWidth == W &&
              r.previousHeight == H && r.layersTouched == 1 && r.selectionMoved,
          "doc transform: a crop sets the new extent and reports the old one");
    check(r.reconstructionPasses == 0,
          "doc transform: and resamples NOTHING -- a crop is an index copy at an integer offset, "
          "and routing it through the matrix path would make it lossy for no reason at all");

    const DocumentRegion rgbAfter = rgbContentRegion(*doc.layers[0].rgbTiles);
    const DocumentRegion maskAfter = maskContentRegion(*doc.layers[0].mask);
    const DocumentRegion selAfter = selectionContentRegion(sel);
    check(rgbAfter.x == -60 && rgbAfter.y == -40,
          "doc transform: the PIXELS moved by -origin -- a layer has no offset field, so the tile "
          "keying is the offset and moving it is the whole of the crop");
    check(maskAfter.x == maskBefore.x - 60 && maskAfter.y == maskBefore.y - 40,
          "doc transform: **the MASK moved by exactly the same offset** -- a crop that moves "
          "pixels and not coverage slides a mask off the content it was painted for, and is "
          "discovered far from the crop that caused it");
    check(selAfter.x == selBefore.x - 60 && selAfter.y == selBefore.y - 40,
          "doc transform: and so did the SELECTION, which is passed by parameter rather than "
          "left on app::OpenDocument for a caller to forget");

    // Bit-exactness, on the values rather than on a tolerance.
    size_t differingTexels = 0;
    for (int32_t y = 0; y < 150; ++y)
      for (int32_t x = 0; x < 200; ++x)
        if (rgbAt(*before.layers[0].rgbTiles, x + 60, y + 40) !=
            rgbAt(*doc.layers[0].rgbTiles, x, y))
          ++differingTexels;
    check(differingTexels == 0,
          "doc transform: every texel of the cropped region is BIT-IDENTICAL to where it came "
          "from -- 0 of 30000, because translatedTileStore moves raw half words and never "
          "decodes one");
    check(maskAt(*doc.layers[0].mask, 45, 65) == 0.25f && maskAt(*doc.layers[0].mask, 5, 5) == 1.0f,
          "doc transform: the mask's VALUES survive too, and the region it never covered still "
          "reveals -- an absent mask tile is 1.0, not 0");

    // Content outside the new canvas is kept, not clipped: undo has to be able
    // to give it back, and the tiles live in absolute document coordinates.
    check(rgbAt(*doc.layers[0].rgbTiles, -10, -10)[3] == 1.0f,
          "doc transform: content cropped OUT is kept rather than discarded -- tiles live in "
          "absolute document coordinates, and a crop that destroyed them would make undo a lie");

    // A crop with a negative origin is how "extend the canvas" is spelled.
    Document grown = makeRgbDoc(64, 64);
    const DocumentTransformResult gr = cropDocument(grown, -10, -20, 100, 100, nullptr);
    check(gr.ok && grown.width == 100 && rgbContentRegion(*grown.layers[0].rgbTiles).x == 10,
          "doc transform: a negative crop origin is how 'extend the canvas' is spelled -- one "
          "signed-origin operation rather than a second code path");
  }

  // --- 5. Canvas size and image size (PRD D17) -----------------------------
  {
    // The nine-cell anchor grid, on an ODD growth so the floor-versus-round
    // decision is observable. 141 - 100 = 41; floored, the extra pixel goes
    // right/bottom for every parity, so a user dragging a size field does not
    // see the content jitter between 20 and 21.
    struct AnchorCase {
      CanvasAnchor anchor;
      int32_t expectX;
      int32_t expectY;
    };
    const AnchorCase cases[] = {
        {CanvasAnchor::TopLeft, 0, 0},      {CanvasAnchor::TopCenter, 20, 0},
        {CanvasAnchor::TopRight, 41, 0},    {CanvasAnchor::CenterLeft, 0, 20},
        {CanvasAnchor::Center, 20, 20},     {CanvasAnchor::CenterRight, 41, 20},
        {CanvasAnchor::BottomLeft, 0, 41},  {CanvasAnchor::BottomCenter, 20, 41},
        {CanvasAnchor::BottomRight, 41, 41}};
    bool anchorsOk = true;
    for (const AnchorCase& c : cases) {
      Document d = Document::createBlank(100, 100, WorkingSpace{});
      fillRgb(*d.layers[0].rgbTiles, 4, 4);
      const DocumentTransformResult r = resizeDocumentCanvas(d, 141, 141, c.anchor, nullptr);
      const DocumentRegion after = rgbContentRegion(*d.layers[0].rgbTiles);
      if (!r.ok || r.reconstructionPasses != 0 || d.width != 141 || after.x != c.expectX ||
          after.y != c.expectY)
        anchorsOk = false;
    }
    check(anchorsOk,
          "doc transform: all nine canvas anchors place the content where the grid says, at 0 "
          "resamples, with the odd pixel FLOORED to the right/bottom for every parity");

    // Image size: the matrix path, one pass, and a 1:1 request that does not
    // touch a value.
    Document img = makeRgbDoc(128, 128);
    const Document beforeResize = img;
    DocumentTransformParams params;
    const DocumentTransformResult same = resizeDocumentImage(img, 128, 128, params, nullptr);
    check(same.ok && same.reconstructionPasses == 0 &&
              storesBitIdentical(*beforeResize.layers[0].rgbTiles, *img.layers[0].rgbTiles),
          "doc transform: a 1:1 image size is BIT-IDENTICAL, not an identity resample -- a "
          "reconstruction kernel at integer offsets is an identity only to within weight "
          "rounding");

    const DocumentTransformResult down = resizeDocumentImage(img, 40, 40, params, nullptr);
    check(down.ok && img.width == 40 && img.height == 40 && down.reconstructionPasses == 1,
          "doc transform: a real image size resamples exactly once and sets the new extent");
    check(rgbContentRegion(*img.layers[0].rgbTiles).width <= 42u,
          "doc transform: and the content lands inside the new canvas rather than at the old "
          "scale -- the destination region is derived from the matrix, not from the old extent");

    // The downscale prefilter has to reach the document-level entry point, or
    // PRD D17's own clause is unmet at exactly the level a user meets it.
    // 1-pixel stripes reduced 256 -> 35, measured through the pigment path
    // because that is the one this file added: prefiltered the mass field is
    // flat to sd 0.0324, unfiltered it swings at sd 0.2939 -- 9.07x.
    PigmentTileStore stripes;
    for (int32_t y = 0; y < 8; ++y)
      for (int32_t x = 0; x < 256; ++x) {
        PigmentTexel t;
        t.latent.c[0] = 0.3f;
        t.latent.c[1] = 0.2f;
        t.latent.c[2] = 0.1f;
        t.mass = (x % 2 == 0) ? 1.0f : 0.0f;
        stripes.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeTexel(tileLocalOffset(PixelCoord{x, y}), t);
      }
    const DocumentRegion stripeRegion = pigmentContentRegion(stripes);
    const Mat3 shrink = transformScale(35.0f / 256.0f, 1.0f);
    double sd[2] = {0.0, 0.0};
    for (int pass = 0; pass < 2; ++pass) {
      PigmentTileStore out;
      std::string err;
      transformPigmentTiles(stripes, stripeRegion, shrink, transformedRegion(shrink, stripeRegion),
                            LatentKernel::Bilinear, /*prefilterDownscale=*/pass == 0, &out, nullptr,
                            &err);
      double sum = 0.0, sum2 = 0.0;
      size_t n = 0;
      for (int32_t x = 2; x < 33; ++x) {
        const PigmentTile* t = out.find(tileCoordAt(PixelCoord{x, 3}));
        const float m = t ? t->readTexel(tileLocalOffset(PixelCoord{x, 3})).mass : 0.0f;
        sum += m;
        sum2 += static_cast<double>(m) * m;
        ++n;
      }
      const double mean = sum / static_cast<double>(n);
      sd[pass] = std::sqrt(std::max(0.0, sum2 / static_cast<double>(n) - mean * mean));
    }
    std::printf("      1px stripes 256 -> 35 in MASS: prefiltered sd %.4f, unfiltered sd %.4f, "
                "%.2fx\n",
                sd[0], sd[1], sd[1] / sd[0]);
    // Threshold 4x against a measured 9.07x. The reduction is deliberately not
    // a power of two: at an exact 8x the broken path accidentally lands on
    // alternate stripes and passes.
    check(sd[1] > sd[0] * 4.0,
          "doc transform: the area-average prefilter reaches the PIGMENT path -- 1px mass "
          "stripes reduced 256 -> 35 come out flat to sd 0.0324, and unfiltered swing at 0.2939");
  }

  // --- 6. Exact paths at document level (PRD D15) --------------------------
  {
    const int32_t W = 200, H = 120;
    const Document original = makeRgbDoc(W, H);
    Document doc = original;
    DocumentTransformParams params;

    const DocumentTransformResult f1 =
        transformDocument(doc, transformFlipHorizontal(static_cast<uint32_t>(W)), W, H, params,
                          nullptr);
    check(f1.ok && f1.reconstructionPasses == 0,
          "doc transform: a document-level horizontal flip resamples NOTHING -- the matrix "
          "reaches exactRemapKind() through the region fold unchanged, because both folded "
          "translations are integer");
    const DocumentTransformResult f2 =
        transformDocument(doc, transformFlipHorizontal(static_cast<uint32_t>(W)), W, H, params,
                          nullptr);
    check(f2.ok && storesBitIdentical(*original.layers[0].rgbTiles, *doc.layers[0].rgbTiles),
          "doc transform: and flip-then-flip-back is BIT-IDENTICAL by memcmp, not close -- a "
          "tolerance here would pass on exactly the implementation D15 forbids");

    Document quarter = original;
    const DocumentTransformResult q =
        transformDocument(quarter, transformRotate90(1, static_cast<uint32_t>(W),
                                                     static_cast<uint32_t>(H)),
                          static_cast<uint32_t>(H), static_cast<uint32_t>(W), params, nullptr);
    check(q.ok && q.reconstructionPasses == 0 && quarter.width == H && quarter.height == W,
          "doc transform: a quarter turn of the canvas is exact too, and TRANSPOSES the extent");

    // Four quarter turns is the identity, still with no arithmetic on a texel.
    Document spun = original;
    for (int i = 0; i < 4; ++i)
      transformDocument(spun,
                        transformRotate90(1, static_cast<uint32_t>(spun.width),
                                          static_cast<uint32_t>(spun.height)),
                        static_cast<uint32_t>(spun.height), static_cast<uint32_t>(spun.width),
                        params, nullptr);
    check(spun.width == W && spun.height == H &&
              storesBitIdentical(*original.layers[0].rgbTiles, *spun.layers[0].rgbTiles),
          "doc transform: four quarter turns return the document bit-for-bit -- four lossless "
          "edits must not stack into a lossy one");
  }

  // --- 7. Masks (the change of variable that keeps a document visible) -----
  {
    const int32_t W = 128, H = 128;
    Document doc = makeRgbDoc(W, H);
    doc.layers[0].mask.emplace();
    for (int32_t y = 40; y < 80; ++y)
      for (int32_t x = 40; x < 80; ++x)
        doc.layers[0].mask->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeCoverage(tileLocalOffset(PixelCoord{x, y}), 0.0f);

    DocumentTransformParams params;
    const LayerTransformResult r =
        transformLayer(doc, 0, transformRotateDegreesAbout(30.0f, Point2{64.0f, 64.0f}), params);
    check(r.ok && r.movedMask && r.movedRgb && r.reconstructionPasses == 1,
          "doc transform: a rotate moves the pixels and the mask by the same matrix, and the "
          "pass count is a MAX over the layer's stores -- they hold disjoint data, so resampling "
          "both is still one pass per stored value");
    check(maskAt(*doc.layers[0].mask, 5, 5) == 1.0f && maskAt(*doc.layers[0].mask, 120, 120) == 1.0f,
          "doc transform: **and the corners the rotation opened up stay REVEALED** -- the mask "
          "transforms in hide space, 1-coverage, so the resampler's transparent-black outside "
          "means reveal rather than hiding the document");
    check(doc.layers[0].mask->occupiedTileCount() <= 4u,
          "doc transform: the rotated mask allocates only the tiles the hidden square lands in, "
          "not a bounding box of reveal tiles");

    // What the naive packing would have done to the same geometry, measured
    // rather than described: coverage packed verbatim, so outside means hidden.
    {
      const DocumentRegion sr{40, 40, 40u, 40u};
      const Mat3 rot = transformRotateDegreesAbout(30.0f, Point2{64.0f, 64.0f});
      const DocumentRegion dr = transformedRegion(rot, sr);
      TransformImage src;
      src.width = sr.width;
      src.height = sr.height;
      src.px.assign(static_cast<size_t>(sr.width) * sr.height * 4u, 1.0f);  // revealed everywhere
      TransformImage dst;
      std::string err;
      TransformParams p;
      transformImage(src,
                     mat3Multiply(mat3Multiply(transformTranslate(-static_cast<float>(dr.x),
                                                                  -static_cast<float>(dr.y)),
                                               rot),
                                  transformTranslate(static_cast<float>(sr.x),
                                                     static_cast<float>(sr.y))),
                     dr.width, dr.height, p, &dst, nullptr, &err);
      size_t hidden = 0;
      for (size_t i = 3; i < dst.px.size(); i += 4)
        if (dst.px[i] < 0.5f) ++hidden;
      std::printf("      naive coverage-space pack would hide %zu of %zu destination texels\n",
                  hidden, dst.px.size() / 4u);
      check(hidden > dst.px.size() / 8u,
            "doc transform: and the naive packing really would hide them -- 1649 of 3249 for one "
            "30-degree rotation, which is the failure the change of variable designs out");
    }

    // An engaged mask with no tiles is the canonical "reveal all", and it is
    // free. Transforming it must not turn it into a bounding box of allocated
    // reveal tiles that composite identically and cost 32 KiB each.
    Document revealAll = makeRgbDoc(64, 64);
    revealAll.layers[0].mask.emplace();
    const LayerTransformResult rr =
        transformLayer(revealAll, 0, transformRotateDegreesAbout(11.0f, Point2{32.0f, 32.0f}),
                       params);
    check(rr.ok && !rr.movedMask && revealAll.layers[0].mask->occupiedTileCount() == 0,
          "doc transform: a reveal-all mask is left ALONE by a transform -- reveal everywhere is "
          "invariant under every transform, and materialising it would cost 32 KiB a tile to say "
          "what zero tiles already say");

    // The hide-space substitution is bit-exact, over every representable mask
    // word rather than over a sample of them.
    size_t nonExact = 0;
    for (int w = 0; w < 65536; ++w) {
      MaskTile a;
      a.data()[0] = static_cast<uint16_t>(w);
      const float coverage = a.readCoverage(PixelCoord{0, 0});
      MaskTile b;
      b.writeCoverage(PixelCoord{0, 0}, 1.0f - (1.0f - coverage));
      if (b.data()[0] != floatToHalf(coverage)) ++nonExact;
    }
    check(nonExact == 0,
          "doc transform: 1-(1-c) is bit-exact for all 65536 half words -- computed in float32, "
          "where a binary16 coverage and its complement are both exactly representable, so the "
          "change of variable costs an exact path nothing");
  }

  // --- 8. Selections (PRD E10) --------------------------------------------
  {
    Selection sel = selectRectangle(10.25f, 20.5f, 60.75f, 70.0f);
    const DocumentRegion sr = selectionContentRegion(sel);
    const Mat3 move = transformTranslate(37.0f, -13.0f);
    Selection moved;
    TransformReport rep;
    std::string err;
    check(transformSelectionCoverage(sel, sr, move, transformedRegion(move, sr), TransformParams{},
                                     &moved, &rep, &err),
          "doc transform: a selection transforms -- PRD E10's 'moving coverage without touching "
          "pixels', and it needs no change of variable because 0 already means outside");
    const DocumentRegion mr = selectionContentRegion(moved);
    check(rep.reconstructionPasses == 0 && mr.x == sr.x + 37 && mr.y == sr.y - 13,
          "doc transform: an integer translate of a selection takes the EXACT path and lands "
          "where it was asked to");

    size_t differingBytes = 0;
    for (int32_t y = sr.y; y < sr.y + static_cast<int32_t>(sr.height); ++y) {
      for (int32_t x = sr.x; x < sr.x + static_cast<int32_t>(sr.width); ++x) {
        const SelectionTile* a = sel.tiles.find(tileCoordAt(PixelCoord{x, y}));
        const SelectionTile* b = moved.tiles.find(tileCoordAt(PixelCoord{x + 37, y - 13}));
        const float va = selectionTileCoverage(a, tileLocalOffset(PixelCoord{x, y}));
        const float vb = selectionTileCoverage(b, tileLocalOffset(PixelCoord{x + 37, y - 13}));
        if (va != vb) ++differingBytes;
      }
    }
    check(differingBytes == 0,
          "doc transform: and every coverage byte survives -- uint8 -> float -> uint8 round-trips "
          "losslessly for all 256 values, measured, because writeCoverage rounds to nearest");

    // A rotated selection still selects roughly the same area -- coverage is
    // conserved by a resample, which is what makes E10 'moving' rather than
    // 'redrawing'.
    Selection spun;
    const Mat3 rot = transformRotateDegreesAbout(25.0f, Point2{35.0f, 45.0f});
    transformSelectionCoverage(sel, sr, rot, transformedRegion(rot, sr), TransformParams{}, &spun,
                               nullptr, &err);
    auto coverageSum = [](const Selection& s) {
      double total = 0.0;
      for (const auto& [coord, tile] : s.tiles)
        for (int32_t ly = 0; ly < kTileSize; ++ly)
          for (int32_t lx = 0; lx < kTileSize; ++lx) total += tile.coverageAt(PixelCoord{lx, ly});
      return total;
    };
    const double before = coverageSum(sel);
    const double after = coverageSum(spun);
    std::printf("      selection coverage: %.1f before a 25 deg rotation, %.1f after (%.2f%%)\n",
                before, after, 100.0 * (after - before) / before);
    // 3% against a rectangle of about 2500 selected texels: a rotation
    // resamples the marquee's own hard edge, and the boundary is one texel of
    // a ~200-texel perimeter. Not a tight bound and not meant to be -- it is
    // here to catch a transform that dropped or doubled the selection, not to
    // pin a filter.
    check(std::fabs(after - before) < before * 0.03,
          "doc transform: a rotated selection conserves total coverage to within 3% -- E10 is "
          "moving the marquee, not redrawing it");
  }

  // --- 9. Locked layers, and why the two levels differ ---------------------
  {
    Document doc = makeRgbDoc(64, 64);
    doc.layers[0].locked = true;
    doc.layers[0].name = "background";
    DocumentTransformParams params;

    const LayerTransformResult lr = transformLayer(doc, 0, transformScale(2.0f, 2.0f), params);
    check(!lr.ok && lr.error.find("locked") != std::string::npos &&
              lr.error.find("background") != std::string::npos,
          "doc transform: a PER-LAYER transform refuses a locked layer by name, exactly as "
          "core::translateLayer does -- moving a layer's pixels is the most content-y edit there "
          "is");

    const DocumentRegion beforeCrop = rgbContentRegion(*doc.layers[0].rgbTiles);
    const DocumentTransformResult cr = cropDocument(doc, 10, 10, 40, 40, nullptr);
    const DocumentRegion afterCrop = rgbContentRegion(*doc.layers[0].rgbTiles);
    check(cr.ok && cr.lockedLayersMoved == 1 && afterCrop.x == beforeCrop.x - 10 &&
              doc.layers[0].locked,
          "doc transform: while a DOCUMENT-level crop moves it anyway and says so -- a lock that "
          "left one layer behind would misregister it against every other by exactly the crop, "
          "which is the lock destroying the document to protect a layer");

    Document mixed = makeRgbDoc(64, 64);
    mixed.layers[0].locked = true;
    const DocumentTransformResult ir =
        resizeDocumentImage(mixed, 32, 32, DocumentTransformParams{}, nullptr);
    check(ir.ok && ir.lockedLayersMoved == 1 && mixed.layers[0].locked && mixed.width == 32,
          "doc transform: image size likewise, and the lock is RESTORED afterwards rather than "
          "quietly cleared");
  }

  // --- 10. Kinds with no pixels, and refusals ------------------------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    Layer adjustment;
    adjustment.kind = LayerKind::Adjustment;  // no rgbTiles, no pigmentTiles
    doc.layers.push_back(adjustment);
    Layer text;
    text.kind = LayerKind::Text;
    doc.layers.push_back(text);
    fillRgb(*doc.layers[0].rgbTiles, 64, 64);

    DocumentTransformParams params;
    const LayerTransformResult ar = transformLayer(doc, 1, transformScale(2.0f, 2.0f), params);
    check(ar.ok && !ar.movedRgb && !ar.movedPigment && ar.reconstructionPasses == 0,
          "doc transform: a layer with no pixel storage SUCCEEDS and moves nothing -- this is "
          "where it parts company with translateLayer, which refuses; refusing a whole-document "
          "crop because the stack holds a Text placeholder would be refusing on a technicality");

    const DocumentTransformResult dr =
        transformDocument(doc, transformScale(0.5f, 0.5f), 32, 32, params, nullptr);
    check(dr.ok && dr.layersTouched == 3 && doc.width == 32,
          "doc transform: so a document containing one is still resizable, and reports every "
          "layer it walked");

    // Refusals, each naming what it refused.
    Document ref = makeRgbDoc(32, 32);
    const DocumentTransformResult zeroCrop = cropDocument(ref, 0, 0, 0, 8, nullptr);
    check(!zeroCrop.ok && zeroCrop.error.find("0x8") != std::string::npos && ref.width == 32,
          "doc transform: a zero crop extent is refused with the extent in the message, and the "
          "document keeps its own");
    check(!resizeDocumentImage(ref, 0, 8, DocumentTransformParams{}, nullptr).ok,
          "doc transform: and so is a zero image size");

    Document zero;  // 0x0, which a default-constructed Document really is
    check(!resizeDocumentImage(zero, 16, 16, DocumentTransformParams{}, nullptr).ok,
          "doc transform: an image size FROM a zero-extent document is refused -- there is no "
          "scale factor from nothing, and inventing one would silently place the content");

    const LayerTransformResult sing =
        transformLayer(ref, 0, transformSkewDegrees(45.0f, 45.0f), params);
    check(!sing.ok && !sing.error.empty() && ref.layers[0].rgbTiles->occupiedTileCount() > 0,
          "doc transform: a singular matrix is refused BEFORE any store is replaced, so a layer "
          "cannot be left with transformed pixels and an untransformed mask");
    check(!transformDocument(ref, transformSkewDegrees(45.0f, 45.0f), 32, 32, params, nullptr).ok,
          "doc transform: and the document-level entry point refuses it up front too, rather "
          "than discovering it on layer 7 of 9");

    check(!transformRgbTiles(*ref.layers[0].rgbTiles, DocumentRegion{0, 0, 32u, 32u},
                             mat3Identity(), DocumentRegion{0, 0, 32u, 32u}, TransformParams{},
                             nullptr, nullptr, nullptr),
          "doc transform: a null destination store is refused rather than dereferenced");
  }

  std::printf("[selftest] document transform %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
