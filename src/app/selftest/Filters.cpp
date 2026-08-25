#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "color/Shaper.hpp"
#include "ops/Blur.hpp"
#include "ops/Filters.hpp"
#include "ops/Roi.hpp"

namespace np {

namespace {

// The same deterministic value source ops/Blur's section uses -- splitmix64's
// finalizer -- so the two sections' fixtures are built from one mixer rather
// than two that could drift. Not <random>: a Mersenne twister's stream is
// standard-library dependent in ways a selftest that compares two paths bit
// for bit should not have to care about.
float filtersTestNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

TileStore filtersTestField(int32_t tx0, int32_t tx1) {
  TileStore tiles;
  uint64_t counter = 0;
  for (int32_t tx = tx0; tx <= tx1; ++tx) {
    Tile& t = tiles.getOrCreate(TileCoord{tx, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = filtersTestNoise(counter++);
        t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
      }
    }
  }
  return tiles;
}

float filtersTexelAt(const TileStore& tiles, int32_t x, int32_t y, int32_t channel) {
  const Tile* t = tiles.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return 0.0f;
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}))[static_cast<size_t>(channel)];
}

// How many of the tiles `rect` touches disagree BYTE FOR BYTE. Bitwise rather
// than within-a-tolerance on purpose: the seam property ops/Blur.hpp
// establishes is an exactness claim, and softening it to "close enough" is
// precisely how a filter that quietly depends on its request rectangle would
// slip through.
size_t filterTileMismatches(const TileStore& a, const TileStore& b, const PixelRect& rect) {
  size_t n = 0;
  const TileRange range = roiTileRange(rect);
  for (int32_t ty = range.y0; ty < range.y1; ++ty) {
    for (int32_t tx = range.x0; tx < range.x1; ++tx) {
      const Tile* p = a.find(TileCoord{tx, ty});
      const Tile* q = b.find(TileCoord{tx, ty});
      if (p == nullptr || q == nullptr) {
        if (p != q) ++n;
        continue;
      }
      if (std::memcmp(p->data(), q->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0) ++n;
    }
  }
  return n;
}

// Runs one op over `whole` as a single request and again as two requests split
// at `splitX`, and reports how many tiles differ. Zero is the only acceptable
// answer for every filter in ops/Filters.
template <class Op>
size_t filterSeamMismatch(Op&& op, const PixelRect& whole, int32_t splitX) {
  TileStore single;
  TileStore split;
  op(whole, &single);
  op(PixelRect{whole.x0, whole.y0, splitX, whole.y1}, &split);
  op(PixelRect{splitX, whole.y0, whole.x1, whole.y1}, &split);
  return filterTileMismatches(single, split, whole);
}

}  // namespace

// ops/Filters -- PLAN.md "Phase 6 -- Filter and transform it" (highpass as
// `src - blur(src)`, unsharp, offset with wrap, sharpen, add noise, local
// contrast; DESIGN-imaging.md "Class B"). Pure CPU, no PaintSim and no GPU,
// the same headless-first-class status runBlurTest() has.
//
// Four things carry the section, and every one of them is a wrong answer that
// looks plausible rather than a crash.
//
// **The seam, six times.** Section 3 asserts, for every filter in the file,
// that a request split across a tile boundary is bit-identical to the same
// request made once. It is inherited for the five that gather through
// ops/Blur's apron and it is *earned* for add noise, whose generator had to be
// made stateless to have it -- section 8 computes the stream-PRNG version on
// purpose and measures how far apart the two halves land.
//
// **Premultiplied alpha at a soft edge.** Sharpening RGB while copying alpha
// through is the obvious reading of "sharpen the picture" and it puts a bright
// rim inside every antialiased cut-out. Section 5 asserts that the shipped
// path leaves an un-premultiplied constant colour alone to within the storage
// format, and then computes the rejected form and measures its 83% drift, so
// the assertion is proved sensitive rather than merely satisfied.
//
// **The two domains, in opposite directions.** Add noise and unsharp's
// threshold are shaper-domain quantities and the blur underneath them is
// linear-light. Sections 8 and 9 measure the constancy that buys (0.83 of the
// level at every level over six stops) and section 9 measures what the linear
// alternative does instead: drive light NEGATIVE at an ordinary setting.
//
// **The exactness budget.** Every tolerance below is one of the two storage
// formats' own rounding and nothing else -- including the claim that the
// shaper round trip is free, which is checked over all 31 744 finite positive
// halves rather than sampled.
bool runFiltersTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

  // --- 0. The error floors every tolerance here is derived from ----------
  //
  // f16 store, absolute: a value in [0,1] rounds to half with at most half an
  // ulp of error and the ulp just below 1.0 is 2^-11, so the bound is 2^-12.
  // f16 store, relative: half carries 10 explicit mantissa bits, so the
  // relative spacing is 2^-10 and half of it is 2^-11. The relative bound is
  // the one the premultiplied-alpha checks need, because they divide.
  const double halfFloor = std::ldexp(1.0, -12);
  const double halfRelFloor = std::ldexp(1.0, -11);
  // An un-premultiplied colour read back out of the store passes through FOUR
  // roundings -- the stored source colour and alpha on the way in, the stored
  // result colour and alpha on the way out -- each bounded by the relative
  // floor above. Nothing here is a chosen number.
  const double premulHueTol = 4.0 * halfRelFloor;
  {
    double worstAbs = 0.0;
    double worstRel = 0.0;
    for (int32_t i = 0; i < 100000; ++i) {
      const float v = filtersTestNoise(static_cast<uint64_t>(i) + 0x51ed2701u);
      worstAbs = std::max(worstAbs,
                          static_cast<double>(std::fabs(halfToFloat(floatToHalf(v)) - v)));
      if (v > 1e-3f) {
        worstRel = std::max(worstRel, static_cast<double>(
                                          std::fabs(halfToFloat(floatToHalf(v)) - v) / v));
      }
    }
    std::printf("  [selftest] filters: error floors -- f16 abs %.4e (sampled %.4e), f16 rel "
                "%.4e (sampled %.4e), premultiplied-hue budget %.4e\n",
                halfFloor, worstAbs, halfRelFloor, worstRel, premulHueTol);
    check(worstAbs <= halfFloor && worstRel <= halfRelFloor,
          "filters: the f16 store's own rounding really is half an ulp in both the absolute "
          "and the relative sense -- every tolerance below is one of those two, not a taste");

    // The claim that lets two ops in this file leave linear light and come
    // back: the shaper round trip cannot change a stored value. Checked
    // EXHAUSTIVELY over the format rather than sampled, because "we tried
    // 100 000 random values" is not an argument about a 16-bit type.
    size_t changed = 0;
    size_t tested = 0;
    double worstShaperRel = 0.0;
    for (uint32_t bits = 0; bits < 0x7C00u; ++bits) {  // every finite positive half
      const float v = halfToFloat(static_cast<uint16_t>(bits));
      ++tested;
      const float rt = shaperDecode(shaperEncode(v));
      if (floatToHalf(rt) != static_cast<uint16_t>(bits)) ++changed;
      if (v > 1e-3f) {
        worstShaperRel = std::max(worstShaperRel,
                                  static_cast<double>(std::fabs(rt - v) / v));
      }
    }
    std::printf("  [selftest] filters: shaperDecode(shaperEncode(h)) over all %zu finite "
                "positive halves -- %zu change; worst relative error %.4e\n",
                tested, changed, worstShaperRel);
    check(changed == 0 && worstShaperRel < halfRelFloor,
          "filters: the shaper round trip does not change a single one of the 31 744 finite "
          "positive halves -- so add noise and local contrast leaving linear light and "
          "coming back costs the store nothing, and their neutral settings are the identity");
  }

  // --- 1. What each op declares to ops/Roi -------------------------------
  {
    BlurParams g;
    g.sigma = 8.0f;
    UnsharpParams u;
    u.blur = g;
    u.amount = 1.0f;
    u.threshold = 0.05f;
    LocalContrastParams lc;
    lc.blur = g;
    lc.amount = 0.5f;
    check(highpassRoiOp(g) == roiDilateOp(32) && unsharpRoiOp(u) == roiDilateOp(32) &&
              localContrastRoiOp(lc) == roiDilateOp(32),
          "filters: highpass, unsharp and local contrast declare EXACTLY the blur's apron -- "
          "the source term and the threshold gate read the output texel itself, so none of "
          "them may widen the fetch and none of them may narrow it");
    check(unsharpRoiOp(sharpenParams(1.0f)) == roiDilateOp(4) &&
              blurApron(sharpenParams(1.0f).blur) == 4,
          "sharpen: the fixed radius costs an apron of ceil(4*sigma) = 4 texels -- a one-click "
          "filter still pays a real gather, and a caller budgeting memory can read the number");
    check(noiseRoiOp() == RoiOp{},
          "noise: add noise is a POINT op and declares the identity -- a noise pass that "
          "claimed an apron would make every stack containing it fetch a wider source for "
          "nothing");

    // Offset is the first asymmetric op in the build, and therefore the first
    // one for which ops/Roi's two directions return different rectangles at
    // all. ops/Roi.hpp predicted this and could only test it on a synthetic op.
    OffsetParams off;
    off.dx = 5;
    off.dy = 3;
    off.wrapRect = PixelRect{0, 0, 256, 128};
    const PixelRect want{10, 10, 20, 20};
    check(roiBackward(offsetRoiOp(off), want) == PixelRect{5, 7, 15, 17} &&
              roiForward(offsetRoiOp(off), want) == PixelRect{15, 13, 25, 23},
          "offset: a production op finally distinguishes roiBackward from roiForward -- it "
          "reads BEHIND its output by the shift and its edits land AHEAD of it, and getting "
          "the sign backwards moves the picture the wrong way with nothing else complaining");

    // And the honest limit: wrap is not a RoiOp at all.
    const PixelRect tight = offsetSourceRect(off, PixelRect{64, 64, 128, 100});
    const PixelRect wide = offsetSourceRect(off, PixelRect{0, 0, 256, 128});
    check(tight == PixelRect{59, 61, 123, 97} && wide == off.wrapRect,
          "offset: the WRAP source rectangle is the translated one when the wrap does not "
          "bite and the whole period when it does -- a wrapped read is up to four disjoint "
          "rectangles, which no dilate-and-translate can express, and ops/Roi's rule says a "
          "too-large ROI is slow while a too-small one is silently wrong");
    OffsetParams clear = off;
    clear.edge = OffsetEdge::Transparent;
    check(offsetSourceRect(clear, PixelRect{0, 0, 256, 128}) ==
              roiBackward(offsetRoiOp(clear), PixelRect{0, 0, 256, 128}),
          "offset: in Transparent mode the source rectangle IS the RoiOp's answer, so the "
          "narrow algebra is still exact for the mode it can describe");
  }

  // --- 2. Refusals -------------------------------------------------------
  {
    const TileStore src = filtersTestField(0, 0);
    TileStore dst;
    BlurParams g;
    g.sigma = 4.0f;
    const PixelRect r{0, 0, 64, 64};

    check(!highpassTiles(src, r, g, nullptr) && !highpassTiles(src, roiEmptyRect(), g, &dst) &&
              !highpassTiles(src, r, BlurParams{BlurKind::Box, 0.0f, -2}, &dst),
          "filters: a null destination, an empty rectangle and a negative blur radius are "
          "refused by name rather than half-performed, exactly as blurTiles refuses them");

    UnsharpParams bad;
    bad.blur = g;
    bad.amount = -1.0f;
    check(!unsharpParamsValid(bad) && !unsharpMaskTiles(src, r, bad, &dst),
          "unsharp: a NEGATIVE amount is refused rather than run -- it is a blur spelled the "
          "hard way, so silently accepting it would turn a caller's sign bug into a filter "
          "that appears to work");
    bad.amount = 1.0f;
    bad.threshold = std::nanf("");
    check(!unsharpParamsValid(bad),
          "unsharp: a non-finite threshold is refused -- it would gate every texel to zero "
          "gain and look exactly like an amount of zero");

    LocalContrastParams neg;
    neg.blur = g;
    neg.amount = -0.5f;
    check(localContrastParamsValid(neg),
          "local contrast: a NEGATIVE amount is LEGAL and means flatten -- the opposite rule "
          "from unsharp's, because reducing local contrast is a thing a retoucher asks for "
          "and reversing a sharpen is not");

    check(!offsetParamsValid(OffsetParams{1, 1, OffsetEdge::Wrap, PixelRect{}}) &&
              offsetParamsValid(OffsetParams{1, 1, OffsetEdge::Transparent, PixelRect{}}),
          "offset: Wrap without a rectangle is refused rather than demoted to Transparent -- "
          "modular arithmetic has no meaning without a period, and the silent demotion would "
          "be wrong at exactly one edge of the canvas");

    // Filtering a store into itself. The write walk re-reads the source at
    // every output texel, so this would read texels the scatter had already
    // replaced -- a plausible smear rather than an obvious failure.
    TileStore aliased = filtersTestField(0, 0);
    check(!highpassTiles(aliased, r, g, &aliased) &&
              !unsharpMaskTiles(aliased, r, sharpenParams(1.0f), &aliased) &&
              !addNoiseTiles(aliased, r, NoiseParams{0.1f, NoiseDistribution::Gaussian, false, 1},
                             &aliased) &&
              !localContrastTiles(aliased, r, LocalContrastParams{g, 0.5f}, &aliased) &&
              !offsetTiles(aliased, r,
                           OffsetParams{3, 3, OffsetEdge::Wrap, PixelRect{0, 0, 128, 128}},
                           &aliased),
          "filters: every op refuses to filter a store INTO ITSELF -- the scatter would "
          "overwrite texels the gather has not read yet, and the result is a plausible "
          "progressive smear rather than anything a reviewer would notice");
  }

  // --- 3. The seam, for every filter in the file -------------------------
  //
  // The assertion this whole file exists for, repeated once per op. A split at
  // texel 77 is deliberately NOT tile-aligned, and the fixture has a hole in
  // it, so nothing here can pass by getting lucky with tile boundaries.
  {
    TileStore sparse;
    uint64_t counter = 5000;
    for (const int32_t tx : {0, 2, 3}) {  // tile 1 deliberately missing
      Tile& t = sparse.getOrCreate(TileCoord{tx, 0});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = filtersTestNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
    const PixelRect whole{0, 0, 384, 128};
    BlurParams g;
    g.sigma = 6.0f;

    size_t worstOp = 0;
    const size_t hp = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { highpassTiles(sparse, r, g, d); }, whole, 77);
    UnsharpParams u;
    u.blur = g;
    u.amount = 1.5f;
    u.threshold = 0.01f;
    const size_t us = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { unsharpMaskTiles(sparse, r, u, d); }, whole, 77);
    const size_t sh = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { sharpenTiles(sparse, r, 1.0f, d); }, whole, 128);
    OffsetParams off;
    off.dx = 37;
    off.dy = -19;
    off.wrapRect = whole;
    const size_t of = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { offsetTiles(sparse, r, off, d); }, whole, 77);
    const NoiseParams np{0.05f, NoiseDistribution::Gaussian, false, 0xC0FFEEull};
    const size_t ns = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { addNoiseTiles(sparse, r, np, d); }, whole, 77);
    LocalContrastParams lc;
    lc.blur = g;
    lc.amount = 0.6f;
    const size_t lcm = filterSeamMismatch(
        [&](const PixelRect& r, TileStore* d) { localContrastTiles(sparse, r, lc, d); }, whole,
        77);
    for (const size_t n : {hp, us, sh, of, ns, lcm}) worstOp = std::max(worstOp, n);
    std::printf("  [selftest] filters: seam mismatches (tiles) -- highpass %zu, unsharp %zu, "
                "sharpen %zu, offset %zu, noise %zu, local contrast %zu\n",
                hp, us, sh, of, ns, lcm);
    check(worstOp == 0,
          "filters: EVERY op is bit-identical when split across a tile boundary, on a sparse "
          "store, at a split that is not tile-aligned -- an output texel that depended on "
          "which rectangle it was asked for in would paint a grid of seams every 128 texels");

    // And the same op requested tile by tile, which is what an evaluator
    // redrawing a viewport actually does.
    TileStore whole1;
    TileStore perTile;
    unsharpMaskTiles(sparse, whole, u, &whole1);
    for (int32_t tx = 0; tx < 3; ++tx) {
      unsharpMaskTiles(sparse, PixelRect{tx * 128, 0, tx * 128 + 128, 128}, u, &perTile);
    }
    check(filterTileMismatches(whole1, perTile, whole) == 0,
          "unsharp: and three separate per-tile requests reproduce the single whole-region "
          "one exactly -- the case a tile cache and a scrolling viewport will actually "
          "generate, rather than the two-piece split above");
  }

  // --- 4. Highpass is `src - blur(src)`, and stays in float --------------
  {
    const TileStore src = filtersTestField(0, 1);
    BlurParams g;
    g.sigma = 6.0f;
    const PixelRect want{0, 0, 256, 128};

    TileStore hp;
    TileStore blurred;
    check(highpassTiles(src, want, g, &hp) && blurTiles(src, want, g, &blurred),
          "highpass: (fixture) highpassTiles and blurTiles both accept the request");

    // The definition, against the blur it is defined in terms of. Compared as
    // an exactness claim where it can be: with the blurred term routed through
    // the f16 store the two paths CANNOT agree, and the size of the gap is the
    // whole reason this file keeps its blurred plane in float.
    double worst = 0.0;
    size_t differ = 0;
    size_t total = 0;
    for (int32_t y = 0; y < 128; ++y) {
      for (int32_t x = 0; x < 256; ++x) {
        for (int32_t c = 0; c < 4; ++c) {
          const double viaStore = static_cast<double>(filtersTexelAt(src, x, y, c)) -
                                  filtersTexelAt(blurred, x, y, c);
          const double exact = filtersTexelAt(hp, x, y, c);
          worst = std::max(worst, std::fabs(viaStore - exact));
          ++total;
          if (floatToHalf(static_cast<float>(viaStore)) !=
              floatToHalf(static_cast<float>(exact))) {
            ++differ;
          }
        }
      }
    }
    std::printf("  [selftest] filters: highpass with the blur routed through the f16 store "
                "first moves %zu of %zu channels; worst gap %.4e (f16 floor %.4e)\n",
                differ, total, worst, halfFloor);
    check(worst < 4.0 * halfFloor && differ > total / 8,
          "highpass: subtracting an f16-ROUNDED blur disagrees with subtracting the float one "
          "on a large fraction of texels -- a difference of two nearly equal numbers cannot "
          "absorb a rounding of either, which is why the blurred plane is not routed through "
          "blurTiles and why the gap is bounded by a few store steps rather than by nothing");

    // A highpass of a flat region is zero, wherever the kernel's whole reach
    // is inside that region. DC preservation seen from the other side, and
    // what makes `src + amount*highpass` leave flat areas alone at any amount.
    //
    // **Not bit-zero, and the bound is derived rather than tried.** ops/Blur
    // normalises its taps to sum to 1 in *double* and then rounds each one to
    // float, so the float taps sum to 1 only to within `(2a+1) * 2^-24`
    // relative, and the two separable passes each pay it. The residue is
    // therefore bounded by `2 * (2a+1) * 2^-24 * value` -- for the 49 taps of
    // sigma 6 on this fixture's largest channel (alpha, 0.8) that is 4.67e-06,
    // which is 52x below the f16 store's own 2.441e-04 and so cannot change
    // any image value this difference is later added to. ops/Blur's own suite
    // gets exactly 0.0 for the same property because its fixture is 0.5, a
    // power of two, where the rounding cancels; this is the general case.
    TileStore flat;
    for (int32_t tx = 0; tx <= 2; ++tx) {
      Tile& t = flat.getOrCreate(TileCoord{tx, 0});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          t.writePixel(PixelCoord{x, y}, {0.4f, 0.2f, 0.1f, 1.0f});
        }
      }
    }
    TileStore flatHp;
    highpassTiles(flat, PixelRect{128, 0, 256, 128}, g, &flatHp);
    // Sampled only where the kernel's whole reach is inside the painted
    // region. The fixture is three tiles wide and ONE tall, so the top and
    // bottom `apron` rows are a blur of the constant field against the
    // transparency outside it -- correctly non-zero, and section 4 of
    // ops/Blur's own suite asserts the same edge behaviour from the other
    // side.
    const int32_t apron = blurApron(g);
    double flatWorst = 0.0;
    for (int32_t y = apron; y < 128 - apron; ++y) {
      for (int32_t x = 128; x < 256; ++x) {
        for (int32_t c = 0; c < 4; ++c) {
          flatWorst = std::max(flatWorst,
                               static_cast<double>(std::fabs(filtersTexelAt(flatHp, x, y, c))));
        }
      }
    }
    // And the edge rows really are non-zero, so the bounds above are a
    // statement about the fixture rather than a quiet way to make the check
    // pass.
    double edgeWorst = 0.0;
    for (int32_t x = 128; x < 256; ++x) {
      edgeWorst = std::max(edgeWorst, static_cast<double>(std::fabs(filtersTexelAt(flatHp, x, 0, 3))));
    }
    const double kernelNormBound =
        2.0 * static_cast<double>(2 * apron + 1) * std::ldexp(1.0, -24) * 0.8;
    std::printf("  [selftest] filters: highpass of a flat region -- interior residue %.4e "
                "(kernel normalisation bound %.4e, f16 store floor %.4e); one apron from the "
                "painted region's edge it is %.4f\n",
                flatWorst, kernelNormBound, halfFloor, edgeWorst);
    check(flatWorst <= kernelNormBound && kernelNormBound < halfFloor * 0.05 && edgeWorst > 0.1,
          "highpass: a flat region highpasses to zero to within the blur kernel's own float "
          "normalisation residue -- two orders of magnitude below what the store can hold, so "
          "`src - blur(src)` cannot invent a brightness shift that unsharp would then multiply "
          "by its amount -- while the rows within an apron of the painted region's edge are "
          "large, which is what makes the sampled bounds a fact about the fixture and not a "
          "fudge");

    // The identity blur. `src - src` is the zero field, which is what a
    // DIFFERENCE operator's neutral setting means -- not "unchanged".
    TileStore identity;
    highpassTiles(src, PixelRect{0, 0, 64, 64}, BlurParams{}, &identity);
    check(filtersTexelAt(identity, 32, 32, 0) == 0.0f &&
              filtersTexelAt(identity, 32, 32, 3) == 0.0f,
          "highpass: a zero-radius request gives the ZERO field -- the neutral setting of a "
          "difference is nothing at all, and expecting the source back is the confusion that "
          "would put a mid-grey bias in this operator");
  }

  // --- 5. Unsharp: premultiplied alpha at a soft edge --------------------
  {
    // An antialiased disc of ONE straight colour. The only thing varying is
    // coverage, so any change to the un-premultiplied colour is the filter's
    // fault and nothing else's.
    const float C[3] = {0.8f, 0.25f, 0.05f};
    TileStore soft;
    Tile& t = soft.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float d = std::sqrt(static_cast<float>((x - 64) * (x - 64) + (y - 64) * (y - 64)));
        float a = 1.0f - (d - 30.0f) / 12.0f;
        a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        t.writePixel(PixelCoord{x, y}, {C[0] * a, C[1] * a, C[2] * a, a});
      }
    }
    UnsharpParams p;
    p.blur.sigma = 3.0f;
    p.amount = 1.2f;
    TileStore out;
    TileStore blurred;
    unsharpMaskTiles(soft, PixelRect{0, 0, 128, 128}, p, &out);
    blurTiles(soft, PixelRect{0, 0, 128, 128}, p.blur, &blurred);

    // Sampled where alpha is large enough for the quotient to mean something.
    // Below about 1/100 of full coverage the stored premultiplied colour is
    // itself only a few half-steps, so the ratio's own error swamps the
    // filter's -- a limit of the storage format, not of the operator.
    double worst = 0.0;
    double worstRejected = 0.0;
    for (int32_t x = 0; x < 128; ++x) {
      const float a = filtersTexelAt(out, x, 64, 3);
      if (a > 1e-2f) {
        for (int32_t c = 0; c < 3; ++c) {
          worst = std::max(worst,
                           std::fabs(static_cast<double>(filtersTexelAt(out, x, 64, c)) / a -
                                     C[static_cast<size_t>(c)]) /
                               C[static_cast<size_t>(c)]);
        }
      }
      // The rejected form, computed here on purpose: sharpen RGB, copy alpha
      // through. Same blur, same amount, one line different.
      const float sa = filtersTexelAt(soft, x, 64, 3);
      if (sa > 1e-2f) {
        for (int32_t c = 0; c < 3; ++c) {
          const double s = filtersTexelAt(soft, x, 64, c);
          const double b = filtersTexelAt(blurred, x, 64, c);
          const double got = (s + p.amount * (s - b)) / sa;
          worstRejected = std::max(worstRejected,
                                   std::fabs(got - C[static_cast<size_t>(c)]) /
                                       C[static_cast<size_t>(c)]);
        }
      }
    }
    std::printf("  [selftest] filters: unsharp across a constant-colour soft edge -- relative "
                "hue drift %.4e (budget %.4e); the rejected RGB-only form drifts %.4f\n",
                worst, premulHueTol, worstRejected);
    check(worst <= premulHueTol,
          "unsharp: un-premultiplying a sharpened soft edge still gives the ORIGINAL colour, "
          "to four store roundings and no more -- one coefficient shared by all four channels "
          "makes RGB and alpha move in lockstep, so the alpha edge gets crisper and the "
          "colour does not move at all");
    check(worstRejected > 0.1,
          "unsharp: while sharpening RGB and copying alpha through drifts by tens of percent "
          "on the SAME edge -- the bright-and-dark rim around every antialiased cut-out, "
          "which is what proves the assertion above is sensitive rather than vacuous");

    // Coverage stays a coverage. An alpha above 1 makes core/Blend's
    // `src + (1-a)*dst` SUBTRACT the backdrop.
    float maxA = 0.0f;
    float minA = 1.0f;
    for (int32_t y = 0; y < 128; ++y) {
      for (int32_t x = 0; x < 128; ++x) {
        maxA = std::max(maxA, filtersTexelAt(out, x, y, 3));
        minA = std::min(minA, filtersTexelAt(out, x, y, 3));
      }
    }
    check(maxA <= 1.0f && minA >= 0.0f,
          "unsharp: the overshoot never drives ALPHA outside [0,1] -- coverage is a fraction, "
          "and an alpha above 1 turns core/Blend's src + (1-a)*dst into a subtraction of the "
          "backdrop, i.e. a dark halo nobody would trace back to a sharpen");

    // And the clamp is a change of COVERAGE, not of colour -- which is what
    // the `worst` figure above already proves, since the sampled band includes
    // clamped columns. Stated separately because the naive clamp is the
    // tempting one.
    double naiveClampDrift = 0.0;
    int32_t clampedColumns = 0;
    for (int32_t x = 0; x < 128; ++x) {
      const float sa = filtersTexelAt(soft, x, 64, 3);
      const float ba = filtersTexelAt(blurred, x, 64, 3);
      const float oa = sa + p.amount * (sa - ba);
      if (oa <= 1.0f || oa <= 1e-2f) continue;
      ++clampedColumns;
      for (int32_t c = 0; c < 3; ++c) {
        const double s = filtersTexelAt(soft, x, 64, c);
        const double b = filtersTexelAt(blurred, x, 64, c);
        naiveClampDrift = std::max(naiveClampDrift,
                                   std::fabs((s + p.amount * (s - b)) / 1.0 -
                                             C[static_cast<size_t>(c)]) /
                                       C[static_cast<size_t>(c)]);
      }
    }
    std::printf("  [selftest] filters: alpha overshot 1.0 on %d of 128 sampled columns; "
                "clamping alpha ALONE would drift the colour by %.4f there\n",
                clampedColumns, naiveClampDrift);
    check(clampedColumns > 0 && naiveClampDrift > 0.05,
          "unsharp: the alpha clamp is REACHED on this edge and clamping it on its own would "
          "brighten the rim by exactly the factor it clipped -- so rescaling RGB with it is "
          "load-bearing, not a precaution");
  }

  // --- 6. Unsharp's threshold, and sharpen's preset ----------------------
  {
    // Grain small against the edge, which is the only situation in which a
    // threshold is a useful control rather than a blunt one.
    const NoiseParams grain{0.004f, NoiseDistribution::Gaussian, true, 4242};
    TileStore s;
    Tile& t = s.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float base = x >= 96 ? 0.60f : 0.15f;
        const float v = shaperDecode(shaperEncode(base) + filterNoiseOffset(grain, x, y, 0));
        t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    double grainMove[3] = {0, 0, 0};
    double stepMove[3] = {0, 0, 0};
    const float thresholds[3] = {0.0f, 0.01f, 0.02f};
    for (int32_t i = 0; i < 3; ++i) {
      UnsharpParams p;
      p.blur.sigma = 2.0f;
      p.amount = 2.0f;
      p.threshold = thresholds[i];
      TileStore out;
      unsharpMaskTiles(s, PixelRect{0, 0, 128, 128}, p, &out);
      for (int32_t y = 20; y < 108; ++y) {
        for (int32_t x = 20; x < 70; ++x) {
          grainMove[i] = std::max(grainMove[i],
                                  static_cast<double>(std::fabs(
                                      filtersTexelAt(out, x, y, 0) - filtersTexelAt(s, x, y, 0))));
        }
        for (int32_t x = 92; x < 101; ++x) {
          stepMove[i] = std::max(stepMove[i],
                                 static_cast<double>(std::fabs(
                                     filtersTexelAt(out, x, y, 0) - filtersTexelAt(s, x, y, 0))));
        }
      }
    }
    std::printf("  [selftest] filters: unsharp threshold (grain sd 0.004 shaper, step 0.1142 "
                "shaper) -- t=0.000 grain %.5f step %.5f | t=0.010 grain %.5f step %.5f | "
                "t=0.020 grain %.5f step %.5f\n",
                grainMove[0], stepMove[0], grainMove[1], stepMove[1], grainMove[2], stepMove[2]);
    check(grainMove[2] == 0.0 && stepMove[2] > 0.2 && grainMove[1] < grainMove[0],
          "unsharp: at five standard deviations of the grain the threshold removes the grain "
          "EXACTLY -- the soft ramp's numerator clamps at zero rather than merely getting "
          "small -- while the step keeps most of its overshoot, which is the whole trade the "
          "control exists to offer");
    check(stepMove[0] > stepMove[1] && stepMove[1] > stepMove[2],
          "unsharp: and the step's overshoot falls SMOOTHLY as the threshold rises -- a hard "
          "gate would hold it constant and then drop it to nothing, drawing a contour through "
          "the picture wherever the local contrast equals the number in the dialog");

    // Sharpen is a preset, and the assertion is that it is exactly one.
    const TileStore src = filtersTestField(0, 0);
    TileStore viaSharpen;
    TileStore viaUnsharp;
    sharpenTiles(src, PixelRect{0, 0, 128, 128}, 0.8f, &viaSharpen);
    unsharpMaskTiles(src, PixelRect{0, 0, 128, 128}, sharpenParams(0.8f), &viaUnsharp);
    check(filterTileMismatches(viaSharpen, viaUnsharp, PixelRect{0, 0, 128, 128}) == 0 &&
              sharpenParams(0.8f).blur.sigma == kSharpenSigma &&
              sharpenParams(0.8f).threshold == 0.0f,
          "sharpen: it is unsharpMaskTiles at a fixed radius and nothing else -- a separate "
          "menu item, not separate arithmetic, so it cannot acquire a second convolution path "
          "and a second chance to reintroduce the tile seam");
    check(!sharpenTiles(src, PixelRect{0, 0, 128, 128}, -1.0f, &viaSharpen),
          "sharpen: a negative strength is refused through the same validation unsharp uses, "
          "rather than quietly blurring");

    // The number that chose the radius: how much of an isolated single-texel
    // spike the highpass keeps. At sigma 0.5 the kernel is mostly its own
    // centre tap and the filter amplifies sensor noise nearly as hard as a
    // real edge.
    double keep[2] = {0, 0};
    const float sigmas[2] = {0.5f, kSharpenSigma};
    for (int32_t i = 0; i < 2; ++i) {
      BlurParams b;
      b.sigma = sigmas[i];
      const std::vector<float> k = blurKernel(b);
      const double centre = k[static_cast<size_t>(blurApron(b))];
      keep[i] = 1.0 - centre * centre;  // separable: the 2-D centre tap is the square
    }
    std::printf("  [selftest] filters: sharpen radius -- a highpass keeps %.4f of an isolated "
                "spike at sigma 0.5 and %.4f at sigma %.1f\n",
                keep[0], keep[1], static_cast<double>(kSharpenSigma));
    check(near(keep[0], 0.5339, 1e-3) && near(keep[1], 0.8534, 1e-3),
          "sharpen: sigma 1.0 was chosen because at sigma 0.5 nearly half the 2-D kernel is "
          "the texel's own value, so `src - blur(src)` measures the texel rather than its "
          "neighbourhood -- these are the two numbers the choice rests on, pinned so a later "
          "'just make it cheaper' cannot move it without saying so");
  }

  // --- 7. Offset with wrap ------------------------------------------------
  {
    TileStore s;
    Tile& t = s.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        t.writePixel(PixelCoord{x, y},
                     {0.004f * static_cast<float>(x), 0.003f * static_cast<float>(y), 0.3f, 1.0f});
      }
    }
    OffsetParams p;
    p.wrapRect = PixelRect{0, 0, 128, 128};
    p.dx = 5;
    p.dy = 7;

    // The Euclidean modulus, which is the entire semantic content of "wrap".
    check(offsetSourceTexel(p, PixelCoord{2, 1}) == PixelCoord{125, 122},
          "offset: an output near the origin wraps to the FAR side -- C's % truncates toward "
          "zero, so -3 % 128 is -3, and the truncating remainder would read a tile that does "
          "not exist and leave a transparent band along the left and top edges");
    OffsetParams clear = p;
    clear.edge = OffsetEdge::Transparent;
    check(offsetSourceTexel(clear, PixelCoord{2, 1}) == PixelCoord{-3, -6},
          "offset: Transparent mode does NOT wrap -- it reads the coordinate as it falls, and "
          "an absent tile is already transparent black");

    TileStore out;
    offsetTiles(s, PixelRect{0, 0, 128, 128}, p, &out);
    double worst = 0.0;
    for (int32_t y = 0; y < 128; ++y) {
      for (int32_t x = 0; x < 128; ++x) {
        const PixelCoord sc = offsetSourceTexel(p, PixelCoord{x, y});
        for (int32_t c = 0; c < 4; ++c) {
          worst = std::max(worst,
                           static_cast<double>(std::fabs(filtersTexelAt(out, x, y, c) -
                                                         filtersTexelAt(s, sc.x, sc.y, c))));
        }
      }
    }
    check(worst == 0.0,
          "offset: every output texel is EXACTLY the source texel offsetSourceTexel names -- "
          "a pure gather moves bits and must not resample, so a single texel of disagreement "
          "means the op is interpolating something it was never asked to");

    // Whole periods are the identity, which is the property that makes this a
    // torus rather than an approximation of one.
    OffsetParams periods = p;
    periods.dx = 128 * 3;
    periods.dy = -128 * 2;
    TileStore rolled;
    offsetTiles(s, PixelRect{0, 0, 128, 128}, periods, &rolled);
    check(filterTileMismatches(s, rolled, PixelRect{0, 0, 128, 128}) == 0,
          "offset: shifting by whole periods -- in both directions, one of them negative -- "
          "returns the picture bit-identically, which is what 'the picture is a torus' means "
          "and what a truncating remainder would break for the negative one");

    // Transparent mode vacates rather than wraps.
    TileStore cleared;
    OffsetParams big = clear;
    big.dx = 200;
    offsetTiles(s, PixelRect{0, 0, 128, 128}, big, &cleared);
    check(filtersTexelAt(cleared, 64, 64, 3) == 0.0f,
          "offset: Transparent mode leaves the vacated band EMPTY rather than repeating the "
          "edge row -- a repeat-edge mode would smear one row of texels across the gap, which "
          "reads as a rendering fault rather than as a choice");
  }

  // --- 8. Add noise -------------------------------------------------------
  {
    const NoiseParams gauss{0.05f, NoiseDistribution::Gaussian, false, 99};

    // Reproducibility, which is the whole reason the seed is a parameter and
    // not a clock.
    double sameSeed = 0.0;
    double differentSeed = 0.0;
    long samples = 0;
    NoiseParams other = gauss;
    other.seed = 100;
    for (int32_t y = 0; y < 64; ++y) {
      for (int32_t x = 0; x < 64; ++x) {
        for (int32_t c = 0; c < 3; ++c) {
          sameSeed = std::max(sameSeed,
                              static_cast<double>(std::fabs(filterNoiseOffset(gauss, x, y, c) -
                                                            filterNoiseOffset(gauss, x, y, c))));
          differentSeed += std::fabs(filterNoiseOffset(gauss, x, y, c) -
                                     filterNoiseOffset(other, x, y, c));
          ++samples;
        }
      }
    }
    check(sameSeed == 0.0 && differentSeed / static_cast<double>(samples) > 0.01,
          "noise: the same seed at the same coordinate gives the same value bit for bit and a "
          "different seed gives a different one -- a filter whose output cannot be reproduced "
          "cannot be tested, which is why there is no clock and no default-random seed");

    // The distribution, which is what `amount` is denominated in.
    double sum = 0.0;
    double sumSq = 0.0;
    double lo = 1e30;
    double hi = -1e30;
    long n = 0;
    for (int32_t y = 0; y < 200; ++y) {
      for (int32_t x = 0; x < 200; ++x) {
        for (int32_t c = 0; c < 3; ++c) {
          const double v = filterNoiseOffset(gauss, x, y, c);
          sum += v;
          sumSq += v * v;
          lo = std::min(lo, v);
          hi = std::max(hi, v);
          ++n;
        }
      }
    }
    const double mean = sum / static_cast<double>(n);
    const double sd = std::sqrt(sumSq / static_cast<double>(n) - mean * mean);
    std::printf("  [selftest] filters: gaussian noise amount 0.05 over %ld samples -- mean "
                "%.4e, sd %.5f, range [%.4f, %.4f] = %.2f sd (bound 5.887 sd)\n",
                n, mean, sd, lo, hi, hi / 0.05);
    check(near(sd, 0.05, 0.002) && std::fabs(mean) < 0.002 && hi / 0.05 < 5.887 &&
              -lo / 0.05 < 5.887,
          "noise: `amount` IS the standard deviation, the mean is zero, and the tail is "
          "bounded at 5.887 sd by the generator's 24-bit mantissa -- an unbounded Box-Muller "
          "could put an arbitrary value in a single texel and no reader would find it");

    NoiseParams uni = gauss;
    uni.distribution = NoiseDistribution::Uniform;
    double uSum = 0.0;
    double uSumSq = 0.0;
    double uLo = 1e30;
    double uHi = -1e30;
    for (int32_t y = 0; y < 200; ++y) {
      for (int32_t x = 0; x < 200; ++x) {
        for (int32_t c = 0; c < 3; ++c) {
          const double v = filterNoiseOffset(uni, x, y, c);
          uSum += v;
          uSumSq += v * v;
          uLo = std::min(uLo, v);
          uHi = std::max(uHi, v);
        }
      }
    }
    const double uMean = uSum / static_cast<double>(n);
    const double uSd = std::sqrt(uSumSq / static_cast<double>(n) - uMean * uMean);
    check(near(uSd, 0.05 / std::sqrt(3.0), 0.001) && uHi <= 0.05 && uLo >= -0.05,
          "noise: the uniform distribution has the standard deviation a uniform HAS "
          "(amount/sqrt3) and never leaves +/-amount -- so the two distributions are not "
          "interchangeable at the same number and the dialog must not pretend they are");

    NoiseParams mono = gauss;
    mono.monochrome = true;
    bool monoAgrees = true;
    for (int32_t x = 0; x < 100; ++x) {
      if (filterNoiseOffset(mono, x, 3, 0) != filterNoiseOffset(mono, x, 3, 2)) monoAgrees = false;
    }
    check(monoAgrees && filterNoiseOffset(gauss, 5, 3, 0) != filterNoiseOffset(gauss, 5, 3, 2),
          "noise: monochrome shares ONE draw across R, G and B -- luminance grain rather than "
          "colour speckle -- and the per-channel mode genuinely draws three, which is the "
          "difference the checkbox is claiming to make");

    // **The shaper domain, as the number PLAN.md's trap is about.** The same
    // amount must perturb every level by the same RATIO.
    TileStore ramp;
    Tile& rt = ramp.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = std::pow(2.0f, -10.0f + 10.0f * static_cast<float>(x) / 127.0f);
        rt.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    NoiseParams rampNoise{0.05f, NoiseDistribution::Uniform, true, 7};
    TileStore noisy;
    addNoiseTiles(ramp, PixelRect{0, 0, 128, 128}, rampNoise, &noisy);
    double relLo = 1e30;
    double relHi = 0.0;
    double toeRel = 0.0;
    for (const int32_t x : {40, 70, 100, 120}) {  // all above the ACEScct knee (2^-7)
      double worstRel = 0.0;
      for (int32_t y = 0; y < 128; ++y) {
        const double base = filtersTexelAt(ramp, x, y, 0);
        worstRel = std::max(worstRel, std::fabs(filtersTexelAt(noisy, x, y, 0) - base) / base);
      }
      relLo = std::min(relLo, worstRel);
      relHi = std::max(relHi, worstRel);
    }
    for (int32_t y = 0; y < 128; ++y) {
      const double base = filtersTexelAt(ramp, 10, y, 0);  // 1.7e-3, below the knee
      toeRel = std::max(toeRel, std::fabs(filtersTexelAt(noisy, 10, y, 0) - base) / base);
    }
    // 2^(0.05 * 17.52) - 1 = 0.8353 is what the ACEScct log segment says an
    // amplitude of 0.05 must be, in linear light, at EVERY level.
    const double predicted = std::pow(2.0, 0.05 * 17.52) - 1.0;
    std::printf("  [selftest] filters: noise amount 0.05 on a ten-stop ramp -- relative "
                "perturbation %.4f..%.4f above the knee (ACEScct predicts %.4f), %.4f below "
                "it\n",
                relLo, relHi, predicted, toeRel);
    check(near(relHi, predicted, 0.02) && relHi - relLo < 0.03,
          "noise: the SAME amount perturbs every level by the same fraction over six stops, "
          "and by the fraction the ACEScct log segment predicts -- this is PLAN.md's 'add "
          "noise runs in the shaper domain' as a measurement, and a linear-light "
          "implementation would vary by 234x across this ramp instead");
    check(toeRel > 2.0 * relHi,
          "noise: and below the ACEScct breakpoint the curve is a straight line, so the grain "
          "reverts to fixed amplitude and is several times louder relative to the level -- a "
          "property of the published transfer function, stated rather than discovered");

    // Alpha, and the support.
    TileStore cutout;
    Tile& ct = cutout.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 32; y < 96; ++y) {
      for (int32_t x = 32; x < 96; ++x) ct.writePixel(PixelCoord{x, y}, {0.5f, 0.2f, 0.1f, 1.0f});
    }
    TileStore grainy;
    addNoiseTiles(cutout, PixelRect{0, 0, 128, 128}, NoiseParams{0.2f, NoiseDistribution::Gaussian,
                                                                false, 3},
                  &grainy);
    double outside = 0.0;
    bool alphaMoved = false;
    for (int32_t y = 0; y < 128; ++y) {
      for (int32_t x = 0; x < 128; ++x) {
        const bool inside = x >= 32 && x < 96 && y >= 32 && y < 96;
        if (inside) {
          if (filtersTexelAt(grainy, x, y, 3) != 1.0f) alphaMoved = true;
        } else {
          for (int32_t c = 0; c < 4; ++c) {
            outside = std::max(outside, static_cast<double>(filtersTexelAt(grainy, x, y, c)));
          }
        }
      }
    }
    check(outside == 0.0 && !alphaMoved,
          "noise: grain is added to STRAIGHT colour and re-premultiplied, so it scales with "
          "coverage, leaves alpha untouched, and cannot appear outside the shape -- a filter "
          "that grew a layer's support or ate holes in it would be doing something nobody "
          "asked a grain control for");

    // The neutral setting.
    const TileStore src = filtersTestField(0, 0);
    TileStore untouched;
    addNoiseTiles(src, PixelRect{0, 0, 128, 128}, NoiseParams{0.0f, NoiseDistribution::Gaussian,
                                                             false, 5},
                  &untouched);
    check(filterTileMismatches(src, untouched, PixelRect{0, 0, 128, 128}) == 0,
          "noise: amount 0 returns the source bit for bit -- a filter at its neutral setting "
          "must not perturb a value by an ulp, which for an op that leaves linear light and "
          "comes back is a claim about the shaper and not only about a branch");

    // The stream-PRNG version of this generator, computed on purpose, to show
    // what the seam assertion in section 3 is actually protecting.
    {
      // A counter walked in scan order over the FULL rectangle, versus the
      // same counter restarted for the right-hand half -- which is what any
      // stateful generator does when the caller splits the request.
      const int32_t w = 384;
      const int32_t split = 77;
      const int32_t probeX = 200;
      const int32_t probeY = 3;
      const uint64_t whole = static_cast<uint64_t>(probeY) * static_cast<uint64_t>(w) + probeX;
      const uint64_t piece =
          static_cast<uint64_t>(probeY) * static_cast<uint64_t>(w - split) + (probeX - split);
      std::printf("  [selftest] filters: a STREAM generator would draw sample #%llu here in "
                  "the whole-rectangle pass and #%llu in the split pass\n",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(piece));
      check(whole != piece &&
                filtersTestNoise(whole) != filtersTestNoise(piece) &&
                filterNoiseOffset(gauss, probeX, probeY, 0) ==
                    filterNoiseOffset(gauss, probeX, probeY, 0),
            "noise: a stateful generator would reach a DIFFERENT draw at the same texel once "
            "the request is split, which is the seam bug with no apron that can fix it -- the "
            "counter-based hash reaches the same one by construction, and section 3 asserts "
            "the consequence end to end");
    }
  }

  // --- 9. Local contrast --------------------------------------------------
  {
    // Detail of the same ABSOLUTE size on two plateaus 100x apart. A
    // ratio-preserving operator raises both local ratios to the same power;
    // an additive one does not.
    TileStore s;
    Tile& t = s.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float base = x < 64 ? 0.02f : 2.0f;
        const float v = base + ((x % 8) < 4 ? 0.004f : -0.004f);
        t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    auto localRatio = [&](const TileStore& st, int32_t x0) {
      double hi = 0.0;
      double lo = 1e30;
      for (int32_t x = x0; x < x0 + 16; ++x) {
        hi = std::max(hi, static_cast<double>(filtersTexelAt(st, x, 64, 0)));
        lo = std::min(lo, static_cast<double>(filtersTexelAt(st, x, 64, 0)));
      }
      return hi / lo;
    };
    LocalContrastParams lc;
    lc.blur.sigma = 6.0f;
    lc.amount = 1.0f;
    TileStore out;
    localContrastTiles(s, PixelRect{0, 0, 128, 128}, lc, &out);
    const double shadowExp = std::log(localRatio(out, 24)) / std::log(localRatio(s, 24));
    const double highExp = std::log(localRatio(out, 88)) / std::log(localRatio(s, 88));
    std::printf("  [selftest] filters: local contrast amount 1.0 raises the local ratio to the "
                "power %.4f in the shadows (level 0.02) and %.4f in the highlights (level "
                "2.0); the identity says 1 + amount = 2\n",
                shadowExp, highExp);
    check(near(shadowExp, 1.0 + lc.amount, 0.01) && near(highExp, 1.0 + lc.amount, 0.01),
          "local contrast: the local ratio is raised to exactly (1 + amount) at BOTH ends of "
          "a 100x range -- adding in the log domain is multiplying in linear, so one number "
          "does the same work in shadow and highlight, which is the whole reason this op is "
          "not just a large-radius unsharp");

    // The property that separates a multiplicative operator from an additive
    // one, and it bites at an ordinary setting rather than an extreme one.
    TileStore step;
    Tile& st = step.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = x < 64 ? 0.05f : 0.60f;
        st.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    BlurParams b;
    b.sigma = 4.0f;
    UnsharpParams u;
    u.blur = b;
    u.amount = 1.0f;
    LocalContrastParams lcStep;
    lcStep.blur = b;
    lcStep.amount = 1.0f;
    TileStore uo;
    TileStore lo2;
    unsharpMaskTiles(step, PixelRect{0, 0, 128, 128}, u, &uo);
    localContrastTiles(step, PixelRect{0, 0, 128, 128}, lcStep, &lo2);
    float unsharpMin = 1e9f;
    float localMin = 1e9f;
    for (int32_t x = 20; x < 108; ++x) {
      unsharpMin = std::min(unsharpMin, filtersTexelAt(uo, x, 64, 0));
      localMin = std::min(localMin, filtersTexelAt(lo2, x, 64, 0));
    }
    std::printf("  [selftest] filters: on a 0.05/0.60 step at amount 1.0 the linear unsharp "
                "undershoots to %+.5f and local contrast to %+.5f\n",
                static_cast<double>(unsharpMin), static_cast<double>(localMin));
    check(unsharpMin < -0.05f && localMin >= 0.0f,
          "local contrast: it CANNOT produce negative light, at an ordinary amount where the "
          "linear-light unsharp of the same radius already does -- negative light is a "
          "premultiplied texel that cannot be un-premultiplied and an inverted band at the "
          "dark side of every edge; a multiplicative operator has zero as an asymptote and an "
          "additive one walks straight past it");

    // Alpha is a tonal op's business exactly not at all.
    TileStore soft;
    Tile& ft = soft.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float a = std::min(1.0f, std::max(0.0f, (static_cast<float>(x) - 40.0f) / 20.0f));
        ft.writePixel(PixelCoord{x, y}, {0.5f * a, 0.3f * a, 0.1f * a, a});
      }
    }
    TileStore graded;
    localContrastTiles(soft, PixelRect{0, 0, 128, 128}, LocalContrastParams{b, 1.5f}, &graded);
    bool alphaHeld = true;
    for (int32_t x = 0; x < 128; ++x) {
      if (filtersTexelAt(graded, x, 64, 3) != filtersTexelAt(soft, x, 64, 3)) alphaHeld = false;
    }
    check(alphaHeld,
          "local contrast: alpha comes through UNCHANGED, bit for bit -- unsharp shares its "
          "coefficient with alpha on purpose so a soft edge gets crisper, and a tonal op that "
          "did the same would move the boundary of every shape it graded");

    // A flat interior is untouched, despite the shaper round trip and the
    // un-premultiply/re-premultiply by a non-unit alpha.
    TileStore flat;
    for (int32_t tx = 0; tx <= 2; ++tx) {
      Tile& tile = flat.getOrCreate(TileCoord{tx, 0});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          tile.writePixel(PixelCoord{x, y}, {0.4f, 0.2f, 0.1f, 0.8f});
        }
      }
    }
    TileStore flatOut;
    localContrastTiles(flat, PixelRect{128, 0, 256, 128}, LocalContrastParams{b, 0.8f}, &flatOut);
    check(filterTileMismatches(flat, flatOut, PixelRect{128, 0, 256, 128}) == 0,
          "local contrast: a flat region comes back BIT-IDENTICAL at a strong amount, over "
          "the WHOLE rectangle including the rows whose apron reaches outside the painted "
          "area -- because a blur is linear, blur(C*A) is C*blur(A), so the straight blurred "
          "colour is still exactly C where a highpass of the same rows is not zero; the "
          "divide-by-alpha, shaper round trip and multiply-by-alpha move no stored half");

    TileStore neutral;
    const TileStore src = filtersTestField(0, 0);
    localContrastTiles(src, PixelRect{0, 0, 128, 128}, LocalContrastParams{b, 0.0f}, &neutral);
    check(filterTileMismatches(src, neutral, PixelRect{0, 0, 128, 128}) == 0,
          "local contrast: and amount 0 is the identity over a NOISY field too, not only a "
          "flat one -- the short circuit and the arithmetic agree");
  }

  std::printf("[selftest] filters %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
