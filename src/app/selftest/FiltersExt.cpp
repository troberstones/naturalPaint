#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "ops/Filters.hpp"
#include "ops/Roi.hpp"

namespace np {

namespace {

// The same private splitmix64-finalizer fixture source every ops/ selftest
// section keeps its own copy of rather than sharing (app/selftest/Filters.cpp
// and app/selftest/FilterMenu.cpp both say why: one file's own change should
// not ripple into a fixture three other files also build on).
float extNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

std::array<float, 4> extReadAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

// Byte-for-byte tile disagreement count, app/selftest/Filters.cpp's own
// `filterTileMismatches()`, restated: the seam property is an exactness
// claim, not a within-tolerance one, so the comparison stays bitwise.
size_t extTileMismatches(const TileStore& a, const TileStore& b, const PixelRect& rect) {
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

// Runs `op` as one whole-rectangle request and again as two requests split at
// `splitX` (not tile-aligned), and reports how many tiles of the result
// disagree. Zero is the only acceptable answer for every filter below.
template <class Op>
size_t extSeamMismatch(Op&& op, const PixelRect& whole, int32_t splitX) {
  TileStore single;
  TileStore split;
  op(whole, &single);
  op(PixelRect{whole.x0, whole.y0, splitX, whole.y1}, &split);
  op(PixelRect{splitX, whole.y0, whole.x1, whole.y1}, &split);
  return extTileMismatches(single, split, whole);
}

// A 384x128 (3-tile) sparse field with the MIDDLE tile deliberately missing --
// app/selftest/Filters.cpp's own section-3 fixture, restated, so the seam
// check below runs on a store with a genuine hole in it rather than a
// suspiciously complete one.
TileStore extSparseField(uint64_t seed) {
  TileStore tiles;
  uint64_t counter = seed;
  for (const int32_t tx : {0, 2, 3}) {
    Tile& t = tiles.getOrCreate(TileCoord{tx, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = extNoise(counter++);
        t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
      }
    }
  }
  return tiles;
}

// The vertical twin of `extSparseField()` -- a 128x384 (3-tile-TALL) field,
// middle tile missing -- for motion blur's own seam check. Motion blur is
// this file's one op whose apron genuinely differs between axes, and a
// horizontal-only sparse fixture split along x (the fixture and split every
// other seam check in this file uses) can prove the LEFT/RIGHT margin
// correct while staying completely insensitive to a broken UP/DOWN margin --
// there is no vertical tile boundary in a single row of tiles for a y-apron
// defect to ever cross. This fixture and a split along y exist so that half
// of the anisotropic apron is not merely declared correct (section 1's
// formula check) but exercised.
TileStore extSparseFieldVertical(uint64_t seed) {
  TileStore tiles;
  uint64_t counter = seed;
  for (const int32_t ty : {0, 2, 3}) {
    Tile& t = tiles.getOrCreate(TileCoord{0, ty});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = extNoise(counter++);
        t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
      }
    }
  }
  return tiles;
}

// The y-split twin of `extSeamMismatch()`: one whole-rectangle request
// against two requests split at `splitY` (not tile-aligned).
template <class Op>
size_t extSeamMismatchVertical(Op&& op, const PixelRect& whole, int32_t splitY) {
  TileStore single;
  TileStore split;
  op(whole, &single);
  op(PixelRect{whole.x0, whole.y0, whole.x1, splitY}, &split);
  op(PixelRect{whole.x0, splitY, whole.x1, whole.y1}, &split);
  return extTileMismatches(single, split, whole);
}

// A DENSE 384x128 (3-tile) field -- every tile present, no gap -- for the
// degenerate-parameter checks below. `extSparseField()`'s missing middle
// tile is exactly right for section 3's seam check and exactly wrong for an
// identity check: `scatterAligned()`'s write loop (the fast path every
// zero-strength case in this file takes) allocates a destination tile for
// EVERY tile the request rectangle touches, including one whose source tile
// is absent -- so an "identity" run over a sparse field legitimately
// produces a tile `dst` now HAS (transparent black) where `src` has none at
// all, and a same-content-different-occupied-set store correctly reads as
// "not bit-identical" to `extTileMismatches()`. app/selftest/Filters.cpp's
// own identity checks take the identical precaution (`filtersTestField()`,
// no gap) for the same reason.
TileStore extDenseField(uint64_t seed) {
  TileStore tiles;
  uint64_t counter = seed;
  for (int32_t tx = 0; tx <= 2; ++tx) {
    Tile& t = tiles.getOrCreate(TileCoord{tx, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = extNoise(counter++);
        t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
      }
    }
  }
  return tiles;
}

// A whole-canvas fixture for the timing section: every tile of a `sizePx`
// square gets content, matching app/selftest/FilterMenu.cpp's own
// `fillWholeCanvasField()` reasoning -- these filters run over the whole
// canvas rectangle regardless of how much of it is painted, so a fixture
// that leaves most tiles empty would time a best case nobody's painting sits
// at.
TileStore extWholeField(int32_t sizePx, uint64_t seed) {
  TileStore tiles;
  uint64_t counter = seed;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty) {
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; y += 3) {
        for (int32_t x = 0; x < kTileSize; x += 3) {
          const float v = extNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
  return tiles;
}

template <class F>
double extTimeMs(F&& f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// An INDEPENDENT bilinear read of `store` at a fractional document position
// -- a fresh ~15-line implementation, not a call into ops/Filters.cpp's
// `planeBilinear()`, so section 7's motion-blur cross-check is not just the
// production code compared against itself under a different name.
std::array<float, 4> extBilinearRead(const TileStore& store, float fx, float fy) {
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const std::array<float, 4> p00 = extReadAt(store, x0, y0);
  const std::array<float, 4> p10 = extReadAt(store, x0 + 1, y0);
  const std::array<float, 4> p01 = extReadAt(store, x0, y0 + 1);
  const std::array<float, 4> p11 = extReadAt(store, x0 + 1, y0 + 1);
  std::array<float, 4> out{};
  for (int32_t c = 0; c < 4; ++c) {
    const size_t i = static_cast<size_t>(c);
    const float top = p00[i] + tx * (p10[i] - p00[i]);
    const float bottom = p01[i] + tx * (p11[i] - p01[i]);
    out[i] = top + ty * (bottom - top);
  }
  return out;
}

// The independent reference `motionBlurTiles()` is checked against: a box
// average of `extBilinearRead()` samples along the direction vector, read
// straight from `store` rather than from any gathered plane. Structurally
// unrelated to `ops/Filters.cpp`'s implementation apart from calling the
// same `std::cos`/`std::sin` -- which is not under test here, only the
// gather-apron-and-combine shape is.
std::array<float, 4> extMotionBlurReference(const TileStore& store, int32_t x, int32_t y,
                                            float angle, int32_t radius) {
  const float cosA = std::cos(angle);
  const float sinA = std::sin(angle);
  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  for (int32_t t = -radius; t <= radius; ++t) {
    const std::array<float, 4> s = extBilinearRead(
        store, static_cast<float>(x) + static_cast<float>(t) * cosA,
        static_cast<float>(y) + static_cast<float>(t) * sinA);
    for (int32_t c = 0; c < 4; ++c) sum[static_cast<size_t>(c)] += s[static_cast<size_t>(c)];
  }
  const float inv = 1.0f / static_cast<float>(2 * radius + 1);
  return {sum[0] * inv, sum[1] * inv, sum[2] * inv, sum[3] * inv};
}

// A "naive" median that clamps its window to `outRect` instead of reading
// into the neighbouring tile -- the exact rank-statistic form of
// `blurTiles()`'s original tile-local bug, built here (not in production
// code) purely to PROVE the real seam assertion below is sensitive rather
// than merely satisfied, the same thing app/selftest/Filters.cpp does with
// its own "naive per-tile blur" for section 3. Deliberately does not go
// through `ops/Filters.cpp` at all.
void extNaiveMedian(const TileStore& src, const PixelRect& outRect, int32_t radius,
                    TileStore* dst) {
  const TileRange tiles = roiTileRange(outRect);
  for (int32_t ty = tiles.y0; ty < tiles.y1; ++ty) {
    for (int32_t tx = tiles.x0; tx < tiles.x1; ++tx) {
      const TileCoord coord{tx, ty};
      const PixelRect span = roiIntersect(roiTileRect(coord), outRect);
      if (roiIsEmpty(span)) continue;
      Tile& out = dst->getOrCreate(coord);
      const PixelCoord origin = tileOrigin(coord);
      for (int32_t y = span.y0; y < span.y1; ++y) {
        for (int32_t x = span.x0; x < span.x1; ++x) {
          std::vector<float> alphas;
          std::vector<float> rs, gs, bs;
          for (int32_t wy = std::max(y - radius, outRect.y0);
               wy <= std::min(y + radius, outRect.y1 - 1); ++wy) {
            for (int32_t wx = std::max(x - radius, outRect.x0);
                 wx <= std::min(x + radius, outRect.x1 - 1); ++wx) {
              const std::array<float, 4> t = extReadAt(src, wx, wy);
              alphas.push_back(t[3]);
              if (t[3] > 0.0f) {
                const float inv = 1.0f / t[3];
                rs.push_back(t[0] * inv);
                gs.push_back(t[1] * inv);
                bs.push_back(t[2] * inv);
              }
            }
          }
          std::sort(alphas.begin(), alphas.end());
          const float aMed = alphas[alphas.size() / 2];
          std::array<float, 4> outPixel{0.0f, 0.0f, 0.0f, 0.0f};
          if (!rs.empty()) {
            std::sort(rs.begin(), rs.end());
            std::sort(gs.begin(), gs.end());
            std::sort(bs.begin(), bs.end());
            outPixel = {rs[rs.size() / 2] * aMed, gs[gs.size() / 2] * aMed,
                       bs[bs.size() / 2] * aMed, aMed};
          }
          out.writePixel(PixelCoord{x - origin.x, y - origin.y}, outPixel);
        }
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// ops/Filters sections 7-9: emboss, median/despeckle, motion blur -- three
// filters added to extend the filter set app/selftest/Filters.cpp's own
// header block covers (sections 1-6). No PRD/PLAN.md entry names these in
// advance; the obligation this file exists to discharge is ops/Filters.hpp's
// own stated one -- "every output texel's value must not depend on which
// rectangle the caller asked for it in" -- for three ops added after the
// original six, each of which earns that property by a different route (see
// each section's own header comment in ops/Filters.hpp).
//
// The throughline across sections 3, 5 and 6 below: emboss inherits the
// property for free (a two-tap linear combination, same shape as a blur's
// weighted average); median and motion blur do not, and each has its own
// "proved sensitive, not merely satisfied" check -- section 5 runs a
// naive, tile-clamped median against the real one and shows they disagree
// at a tile boundary; section 6 cross-checks motion blur's general-angle
// path against an independently written bilinear reference.
bool runFiltersExtTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // f16 store floors, restated from app/selftest/Filters.cpp section 0:
  // absolute half-ulp just below 1.0 is 2^-12, relative is 2^-11. Every
  // tolerance below is built from these two numbers scaled by the expected
  // magnitude, not chosen by feel.
  const double halfFloor = std::ldexp(1.0, -12);
  const double halfRelFloor = std::ldexp(1.0, -11);
  auto tolFor = [&](double magnitude) {
    return std::max(halfFloor, std::fabs(magnitude) * halfRelFloor * 4.0);
  };

  // --- 1. ROI declarations ------------------------------------------------
  {
    const EmbossParams e{3, -2, 1.0f, 1.0f};
    check(embossRoiOp(e) == RoiOp{3, 3, 2, 2, 0, 0},
          "emboss: the apron is |dx| left/right and |dy| up/down regardless of the taps' own "
          "signs -- both taps are read, so both sides of each axis are needed");
    check(embossRoiOp(EmbossParams{0, 0, 1.0f, 1.0f}) == RoiOp{},
          "emboss: a zero direction declares the identity apron -- both taps land on the "
          "output texel itself");

    check(medianRoiOp(MedianParams{5}) == roiDilateOp(5) &&
              medianRoiOp(MedianParams{0}) == RoiOp{},
          "median: the ROI is exactly a blur's -- roiDilateOp(radius) -- the shape ops/Roi.hpp's "
          "own header predicted for this filter before it existed");

    check(motionBlurRoiOp(MotionBlurParams{0.7f, 0}) == RoiOp{},
          "motion blur: radius 0 declares the identity apron at ANY angle");
    // The formula itself, recomputed independently rather than compared
    // against a hand-typed literal -- what a sabotage of the formula (e.g.
    // swapping which axis gets which margin) would actually break.
    for (const float angle : {0.0f, 0.37f, 1.9f, -2.4f}) {
      for (const int32_t radius : {1, 4, 11}) {
        const float c = std::fabs(std::cos(angle));
        const float s = std::fabs(std::sin(angle));
        const int32_t mx = static_cast<int32_t>(std::ceil(static_cast<float>(radius) * c)) + 1;
        const int32_t my = static_cast<int32_t>(std::ceil(static_cast<float>(radius) * s)) + 1;
        const RoiOp got = motionBlurRoiOp(MotionBlurParams{angle, radius});
        check(got == RoiOp{mx, mx, my, my, 0, 0},
              "motion blur: the declared apron matches ceil(radius*|cos|)+1 in x and "
              "ceil(radius*|sin|)+1 in y, for a spread of angles and radii");
      }
    }
    // The one exact case: at angle 0, sin is exactly 0.0f in float (not
    // merely close), so the y-margin is exactly 1 -- the bilinear safety
    // texel and nothing else -- and the x-margin is exactly radius+1.
    check(motionBlurRoiOp(MotionBlurParams{0.0f, 7}) == RoiOp{8, 8, 1, 1, 0, 0},
          "motion blur: angle 0 is the one case with no floating-point noise in the trig -- "
          "the y-margin is exactly 1 texel of bilinear safety, not a near-zero apron");
  }

  // --- 2. Refusals ----------------------------------------------------------
  {
    const TileStore src = extSparseField(1000);
    TileStore dst;
    const PixelRect r{0, 0, 64, 64};
    TileStore srcCopy = src;
    check(!embossTiles(src, r, EmbossParams{1, 0, 1.0f, 1.0f}, nullptr) &&
              !embossTiles(srcCopy, r, EmbossParams{1, 0, 1.0f, 1.0f}, &srcCopy) &&
              !embossTiles(src, roiEmptyRect(), EmbossParams{1, 0, 1.0f, 1.0f}, &dst) &&
              !embossParamsValid(EmbossParams{1, 0, 1.0f, -1.0f}) &&
              !embossParamsValid(
                  EmbossParams{1, 0, std::numeric_limits<float>::quiet_NaN(), 1.0f}) &&
              !embossParamsValid(EmbossParams{1, 0, 1.0f, std::numeric_limits<float>::quiet_NaN()}),
          "emboss: refused by name -- null dst, dst aliasing src, an empty rect, a negative "
          "amount, a non-finite depth or amount");

    check(!medianTiles(src, r, MedianParams{2}, nullptr) &&
              !medianTiles(srcCopy, r, MedianParams{2}, &srcCopy) &&
              !medianTiles(src, roiEmptyRect(), MedianParams{2}, &dst) &&
              !medianParamsValid(MedianParams{-1}),
          "median: refused by name -- null dst, dst aliasing src, an empty rect, a negative "
          "radius");

    check(!motionBlurTiles(src, r, MotionBlurParams{0.0f, 3}, nullptr) &&
              !motionBlurTiles(srcCopy, r, MotionBlurParams{0.0f, 3}, &srcCopy) &&
              !motionBlurTiles(src, roiEmptyRect(), MotionBlurParams{0.0f, 3}, &dst) &&
              !motionBlurParamsValid(MotionBlurParams{0.0f, -1}) &&
              !motionBlurParamsValid(
                  MotionBlurParams{std::numeric_limits<float>::quiet_NaN(), 3}),
          "motion blur: refused by name -- null dst, dst aliasing src, an empty rect, a "
          "negative radius, a non-finite angle");
  }

  // --- 3. The seam, for all three ------------------------------------------
  {
    const TileStore sparse = extSparseField(5000);
    const PixelRect whole{0, 0, 384, 128};

    const EmbossParams eParams{5, -3, 1.5f, 0.8f};
    const size_t em = extSeamMismatch(
        [&](const PixelRect& rr, TileStore* d) { embossTiles(sparse, rr, eParams, d); }, whole,
        77);

    const MedianParams mParams{2};
    const size_t md = extSeamMismatch(
        [&](const PixelRect& rr, TileStore* d) { medianTiles(sparse, rr, mParams, d); }, whole,
        77);

    const MotionBlurParams bParams{0.7f, 3};
    const size_t mb = extSeamMismatch(
        [&](const PixelRect& rr, TileStore* d) { motionBlurTiles(sparse, rr, bParams, d); }, whole,
        77);

    std::printf("  [selftest] filtersExt: seam mismatches (tiles) -- emboss %zu, median %zu, "
                "motion blur %zu\n",
                em, md, mb);
    check(em == 0 && md == 0 && mb == 0,
          "filtersExt: all three ops are bit-identical when split across a tile boundary, on a "
          "sparse store with a missing tile, at a split that is not tile-aligned -- exactly "
          "app/selftest/Filters.cpp's own section-3 standard, applied to the three ops it "
          "predates");

    // Median and motion blur additionally reproduced per-tile, which is what
    // an evaluator redrawing a viewport actually asks for (three separate
    // 128-wide requests rather than one 384-wide one).
    TileStore medianWhole, medianPerTile;
    medianTiles(sparse, whole, mParams, &medianWhole);
    for (int32_t tx = 0; tx < 3; ++tx) {
      medianTiles(sparse, PixelRect{tx * 128, 0, tx * 128 + 128, 128}, mParams, &medianPerTile);
    }
    check(extTileMismatches(medianWhole, medianPerTile, whole) == 0,
          "median: three separate per-tile requests reproduce the single whole-region one "
          "exactly");

    TileStore motionWhole, motionPerTile;
    motionBlurTiles(sparse, whole, bParams, &motionWhole);
    for (int32_t tx = 0; tx < 3; ++tx) {
      motionBlurTiles(sparse, PixelRect{tx * 128, 0, tx * 128 + 128, 128}, bParams,
                      &motionPerTile);
    }
    check(extTileMismatches(motionWhole, motionPerTile, whole) == 0,
          "motion blur: three separate per-tile requests reproduce the single whole-region one "
          "exactly");

    // The vertical twin, at a NEAR-VERTICAL angle so the kernel's reach is
    // almost entirely in y -- the case `em`/`md`/`mb` above cannot exercise
    // at all (a single tile ROW has no vertical tile boundary to cross). An
    // apron formula that mixed up which margin belongs to which axis, or
    // that simply forgot the angle depends on y at all, would pass every
    // check above and fail only here.
    const TileStore tallSparse = extSparseFieldVertical(6000);
    const PixelRect tallWhole{0, 0, 128, 384};
    const MotionBlurParams verticalParams{1.5f, 3};  // 1.5 rad ~ 85.9 degrees from +x
    const size_t mbVertical = extSeamMismatchVertical(
        [&](const PixelRect& rr, TileStore* d) { motionBlurTiles(tallSparse, rr, verticalParams, d); },
        tallWhole, 77);
    std::printf("  [selftest] filtersExt: motion blur VERTICAL seam mismatches (tiles), near-"
                "vertical angle: %zu\n",
                mbVertical);
    check(mbVertical == 0,
          "motion blur: the seam property holds across a VERTICAL tile boundary too, at an "
          "angle that stresses the up/down margin specifically -- the anisotropic apron's "
          "other axis, which a horizontal-only fixture can never exercise");
  }

  // --- 4. Degenerate parameters --------------------------------------------
  {
    const TileStore field = extDenseField(9000);
    const PixelRect whole{0, 0, 384, 128};

    TileStore embossIdentity;
    embossTiles(field, whole, EmbossParams{4, -1, 3.0f, 0.0f}, &embossIdentity);
    check(extTileMismatches(field, embossIdentity, whole) == 0,
          "emboss: amount 0 is the identity, bit for bit, over a noisy field -- the short "
          "circuit skips the gather entirely rather than computing and discarding it");

    TileStore medianIdentity;
    medianTiles(field, whole, MedianParams{0}, &medianIdentity);
    check(extTileMismatches(field, medianIdentity, whole) == 0,
          "median: radius 0 (a 1x1 window) is the identity, bit for bit -- its own single "
          "sample IS its median");

    TileStore motionIdentity;
    motionBlurTiles(field, whole, MotionBlurParams{1.234f, 0}, &motionIdentity);
    check(extTileMismatches(field, motionIdentity, whole) == 0,
          "motion blur: radius 0 is the identity, bit for bit, at an arbitrary angle -- a "
          "single tap at t=0 samples exactly the output texel");
  }

  // --- 5. Emboss: known input/output, and the flat-grey degenerate case ---
  {
    // A hard step in a GREY (r=g=b=v) fixture: Rec.709 luma of an equal-RGB,
    // opaque texel is exactly v regardless of the channel weights (they sum
    // to 1), which is what makes the arithmetic below hand-computable rather
    // than merely plausible.
    TileStore step;
    Tile& t = step.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = x < 64 ? 0.2f : 0.8f;
        t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    const PixelRect r0{0, 0, 128, 128};
    const EmbossParams full{1, 0, 2.0f, 1.0f};  // amount 1: out collapses to relief*srcA exactly
    TileStore outFull;
    embossTiles(step, r0, full, &outFull);

    // Deep in either flat plateau, ahead == behind, so the gradient (hence
    // relief - 0.5) is exactly zero: BOTH plateaus converge to the same
    // 0.5 grey card at amount 1, regardless of their own very different v.
    const float flatVal = extReadAt(outFull, 20, 64)[0];
    const float flatVal2 = extReadAt(outFull, 100, 64)[0];
    std::printf("  [selftest] filtersExt: emboss flat-plateau value (expect 0.5) -- x=20: "
                "%.6f, x=100: %.6f\n",
                static_cast<double>(flatVal), static_cast<double>(flatVal2));
    check(std::fabs(static_cast<double>(flatVal) - 0.5) <= tolFor(0.5) &&
              std::fabs(static_cast<double>(flatVal2) - 0.5) <= tolFor(0.5),
          "emboss: two plateaus 0.2 and 0.8 both come back at EXACTLY 0.5 in their own "
          "interior at amount 1 -- the gradient is zero away from the step regardless of the "
          "underlying level, so 'depth' cannot be read off a flat region's own brightness");

    // At the step itself: ahead=0.8, behind=0.2 on both sides of the
    // boundary (x=63's ahead is x=64, x=64's ahead is x=65 -- both land in
    // the 0.8 plateau; x=63's behind is x=62, x=64's behind is x=63 -- both
    // land in the 0.2 plateau), so relief = 0.5 + 2*(0.8-0.2) = 1.7 on BOTH
    // texels flanking the step, an HDR overshoot this filter does not clip
    // (only clampStorable's [0, kFilterMaxLinear] applies).
    const float atStepLeft = extReadAt(outFull, 63, 64)[0];
    const float atStepRight = extReadAt(outFull, 64, 64)[0];
    std::printf("  [selftest] filtersExt: emboss at the step (expect 1.7) -- x=63: %.6f, "
                "x=64: %.6f\n",
                static_cast<double>(atStepLeft), static_cast<double>(atStepRight));
    check(std::fabs(static_cast<double>(atStepLeft) - 1.7) <= tolFor(1.7) &&
              std::fabs(static_cast<double>(atStepRight) - 1.7) <= tolFor(1.7),
          "emboss: the ridge at a 0.6-tall step reaches 1.7 -- an overshoot past the source's "
          "own [0.2, 0.8] range -- on BOTH texels flanking it, because both share the same "
          "(ahead, behind) pair one texel apart");

    // amount 0.5: the general blend, not the amount=1 special case where the
    // source term cancels out of the formula entirely.
    const EmbossParams half{1, 0, 2.0f, 0.5f};
    TileStore outHalf;
    embossTiles(step, r0, half, &outHalf);
    const float halfVal = extReadAt(outHalf, 20, 64)[0];  // src 0.2, relief 0.5: 0.2+0.5*(0.5-0.2)
    check(std::fabs(static_cast<double>(halfVal) - 0.35) <= tolFor(0.35),
          "emboss: amount 0.5 in a flat region blends exactly halfway between the source (0.2) "
          "and the relief card (0.5) -- 0.35 -- proving the lerp itself, not only its amount=1 "
          "collapse");

    // dx = dy = 0: both taps read the output texel itself, so the gradient
    // is identically zero EVERYWHERE, including across a noisy (not flat)
    // field -- the "flat grey card, not identity" degenerate case
    // ops/Filters.hpp's section 7 argues is a real, well-defined answer
    // rather than a second identity.
    const TileStore noisy = extSparseField(42);
    const EmbossParams zeroDir{0, 0, 5.0f, 1.0f};
    TileStore outZeroDir;
    embossTiles(noisy, PixelRect{0, 0, 128, 128}, zeroDir, &outZeroDir);
    bool allHalf = true;
    double worstZeroDir = 0.0;
    for (int32_t y = 0; y < kTileSize; y += 7) {
      for (int32_t x = 0; x < kTileSize; x += 7) {
        const std::array<float, 4> px = extReadAt(outZeroDir, x, y);
        // source alpha is 1.0 everywhere in extSparseField, so relief*srcA
        // == relief == 0.5 exactly for every channel.
        for (int32_t c = 0; c < 3; ++c) {
          const double d = std::fabs(static_cast<double>(px[static_cast<size_t>(c)]) - 0.5);
          worstZeroDir = std::max(worstZeroDir, d);
          if (d > tolFor(0.5)) allHalf = false;
        }
      }
    }
    std::printf("  [selftest] filtersExt: emboss dx=dy=0 worst deviation from 0.5 over a noisy "
                "field: %.3e (tolerance %.3e)\n",
                worstZeroDir, tolFor(0.5));
    check(allHalf,
          "emboss: dx=dy=0 returns the flat 0.5 relief card EVERYWHERE on a noisy field, not "
          "just a flat one -- confirming the degenerate case is 'no direction was given' and "
          "not an accidental identity that only a flat fixture would hide a bug in");
  }

  // --- 6. Median: an isolated speckle removed completely, and transparent
  //        samples excluded from the colour vote even when their RGB is
  //        garbage ----------------------------------------------------------
  {
    const std::array<float, 4> flat{0.3f, 0.6f, 0.1f, 1.0f};
    const std::array<float, 4> salt{0.9f, 0.05f, 0.99f, 1.0f};
    TileStore speckled;
    Tile& t = speckled.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, flat);
    }
    t.writePixel(PixelCoord{64, 64}, salt);

    TileStore despeckled;
    medianTiles(speckled, PixelRect{0, 0, 128, 128}, MedianParams{1}, &despeckled);

    // The reference is `speckled`'s OWN stored representation of `flat` at a
    // non-salt texel, read back through the store's half round trip -- NOT
    // the raw `flat` float literal. `0.3f`/`0.6f`/`0.1f` are not exactly
    // representable in half, so the value this filter's arithmetic actually
    // reproduces (a half-rounded sample, selected and re-stored) is the
    // ONE the store already rounded to, which can differ from the raw
    // literal in its last bit -- comparing against the literal would be
    // testing the store's rounding, not the filter.
    const std::array<float, 4> flatStored = extReadAt(speckled, 0, 0);

    // Compared PER-TEXEL and INSET from the tile's own edge, not by
    // whole-tile memcmp: a texel within `radius` of the edge legitimately
    // reads a window that reaches past the document into a tile that does
    // not exist, and every op in this file treats that as transparent black
    // -- exactly like `blurTiles()` fading a painted region's edge to
    // transparent rather than to black -- so its median legitimately is NOT
    // `flat` there. That is a different, already-covered property (the
    // absent-tile convention), not the one this check is for.
    bool interiorMatches = true;
    for (int32_t y = 1; y < kTileSize - 1 && interiorMatches; ++y) {
      for (int32_t x = 1; x < kTileSize - 1; ++x) {
        if (extReadAt(despeckled, x, y) != flatStored) {
          interiorMatches = false;
          break;
        }
      }
    }
    check(interiorMatches,
          "median: a single-texel outlier is removed COMPLETELY at radius 1 -- every 3x3 "
          "window containing it still has an 8-1 majority, so the whole tile's INTERIOR comes "
          "back bit-identical to the speckle-free field, not merely 'less speckled'");

    // Half the tile is opaque colour A, the other half is fully transparent
    // but stores GARBAGE premultiplied RGB on purpose -- a real invalid
    // premultiplied texel, to prove the implementation truly excludes it by
    // `A > 0` rather than by the RGB happening to be zero.
    const std::array<float, 4> colourA{0.1f, 0.2f, 0.3f, 1.0f};
    const std::array<float, 4> garbage{0.77f, 0.88f, 0.99f, 0.0f};
    TileStore mixed;
    Tile& mt = mixed.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        mt.writePixel(PixelCoord{x, y}, x < 64 ? colourA : garbage);
      }
    }
    TileStore mixedOut;
    medianTiles(mixed, PixelRect{0, 0, 128, 128}, MedianParams{3}, &mixedOut);
    // x=63: a 7-wide window [60,66] straddling the boundary, 4 opaque
    // columns and 3 garbage-but-transparent columns per row -- every
    // surviving (opaque) sample is colourA exactly, so the colour median is
    // colourA regardless of the window's skew, and the alpha median (28
    // ones vs 21 zeros in a 49-sample window) is 1.
    // Reference is `mixed`'s own stored (half-rounded) colourA, read back
    // from an interior opaque texel -- not the raw `colourA` literal, for
    // the identical reason the despeckle check above reads `flatStored`
    // rather than `flat`.
    const std::array<float, 4> colourAStored = extReadAt(mixed, 10, 64);
    const std::array<float, 4> atBoundary = extReadAt(mixedOut, 63, 64);
    check(atBoundary == colourAStored,
          "median: at a boundary window with a MAJORITY-opaque mix, the result is exactly "
          "colourA -- the transparent samples' garbage RGB contributed nothing, not even "
          "diluted");
    // x=90: window [87,93] entirely inside the transparent/garbage region --
    // no opaque sample anywhere in the window, so the colour vote is empty
    // and the result is the documented (0,0,0,0), not a blend of garbage.
    const std::array<float, 4> deepGarbage = extReadAt(mixedOut, 90, 64);
    check(deepGarbage == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "median: a window with NO opaque sample at all returns exactly (0,0,0,0) -- "
          "'nothing there is nothing to grade', even though every sample in the window holds "
          "non-zero garbage RGB behind its zero alpha");

    // Sensitivity: the naive, tile-clamped median (built above, independent
    // of ops/Filters.cpp) disagrees with the real one right at a non-tile-
    // aligned split, on the SAME sparse fixture and split section 3 already
    // proved the real op is immune to -- proof this filter's discipline is
    // load-bearing, not merely unexercised.
    const TileStore sparse = extSparseField(5000);
    const PixelRect whole{0, 0, 384, 128};
    TileStore naiveWhole, naiveSplit;
    extNaiveMedian(sparse, whole, 2, &naiveWhole);
    extNaiveMedian(sparse, PixelRect{0, 0, 77, 128}, 2, &naiveSplit);
    extNaiveMedian(sparse, PixelRect{77, 0, 384, 128}, 2, &naiveSplit);
    const size_t naiveMismatch = extTileMismatches(naiveWhole, naiveSplit, whole);
    std::printf("  [selftest] filtersExt: median sensitivity -- the NAIVE tile-clamped "
                "reference disagrees on %zu tile(s) at the same split the real op passed\n",
                naiveMismatch);
    check(naiveMismatch > 0,
          "median: the naive, tile-clamped reference DOES fail this file's own seam check -- "
          "proving section 3's assertion is sensitive to the discipline medianTiles() actually "
          "follows, not vacuously true for any implementation of a windowed median");
  }

  // --- 7. Motion blur: exact grid cases, and an independent bilinear
  //        cross-check at a general angle -------------------------------
  {
    // Angle 0: a horizontal linear ramp. A box average of a linear ramp over
    // a window symmetric about x equals the ramp's own value AT x exactly
    // (in exact arithmetic) -- a hand-checkable identity, not a guessed
    // tolerance.
    TileStore ramp;
    Tile& rt = ramp.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = static_cast<float>(x) / 127.0f;
        rt.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    TileStore rampOut;
    motionBlurTiles(ramp, PixelRect{0, 0, 128, 128}, MotionBlurParams{0.0f, 3}, &rampOut);
    const double expectRamp = 64.0 / 127.0;
    const double gotRamp = static_cast<double>(extReadAt(rampOut, 64, 64)[0]);
    check(std::fabs(gotRamp - expectRamp) <= tolFor(expectRamp),
          "motion blur: angle 0, radius 3 on a horizontal ramp reproduces the ramp's own "
          "centre value exactly -- a symmetric box average of a linear function is the "
          "function's value at the centre, in exact arithmetic");

    // Angle pi/2: the identical ramp, vertical instead.
    TileStore vramp;
    Tile& vt = vramp.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const float v = static_cast<float>(y) / 127.0f;
        vt.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
      }
    }
    TileStore vrampOut;
    const float halfPi = std::acos(-1.0f) / 2.0f;
    motionBlurTiles(vramp, PixelRect{0, 0, 128, 128}, MotionBlurParams{halfPi, 3}, &vrampOut);
    const double gotVramp = static_cast<double>(extReadAt(vrampOut, 64, 64)[0]);
    check(std::fabs(gotVramp - expectRamp) <= tolFor(expectRamp),
          "motion blur: angle pi/2 on the same ramp rotated 90 degrees reproduces the identical "
          "centre value -- the kernel does not silently prefer one axis");

    // A general angle (3-4-5 triangle: cos = 0.8, sin = 0.6, both exact
    // rationals in real arithmetic, close to it in float), cross-checked
    // against `extMotionBlurReference()` -- structurally independent of
    // ops/Filters.cpp -- over a noisy interior region where the apron stays
    // fully inside the one populated tile.
    const TileStore noisy = extSparseField(777);
    const float angle = std::atan2(3.0f, 4.0f);
    const int32_t radius = 4;
    const MotionBlurParams general{angle, radius};
    TileStore generalOut;
    motionBlurTiles(noisy, PixelRect{0, 0, 128, 128}, general, &generalOut);
    double worstDiff = 0.0;
    for (int32_t y = 10; y < 118; y += 5) {
      for (int32_t x = 10; x < 118; x += 5) {
        const std::array<float, 4> got = extReadAt(generalOut, x, y);
        const std::array<float, 4> ref = extMotionBlurReference(noisy, x, y, angle, radius);
        for (int32_t c = 0; c < 4; ++c) {
          worstDiff = std::max(
              worstDiff, std::fabs(static_cast<double>(got[static_cast<size_t>(c)]) -
                                   static_cast<double>(ref[static_cast<size_t>(c)])));
        }
      }
    }
    // Both sides read the same underlying half-precision INPUT store and do
    // the identical arithmetic (a box average of bilinear samples), but only
    // `got` passes through a SECOND half round trip -- `motionBlurTiles()`
    // stores its float result to `generalOut`'s half texels, where `ref` is
    // a raw float never written anywhere -- so the dominant, expected
    // disagreement is exactly the store's own absolute rounding floor
    // (`halfFloor`, 2^-12), not float summation-order noise. Measured worst
    // case lands almost exactly AT that floor, which is the printed number
    // confirming the two paths are doing the same arithmetic (a
    // disagreement dominated by anything else -- a wrong tap, a wrong
    // weight, a swapped axis -- would be orders of magnitude larger, not a
    // near-exact match to one specific, independently-known rounding
    // constant); the tolerance is set at 2x the floor to leave headroom
    // without hiding a real disagreement.
    std::printf("  [selftest] filtersExt: motion blur general-angle (atan2(3,4)) worst diff "
                "against an independent bilinear reference: %.3e (f16 store floor %.3e)\n",
                worstDiff, halfFloor);
    check(worstDiff < 2.0 * halfFloor,
          "motion blur: the production gather-plane-and-combine path agrees with an "
          "independently written direct-bilinear reference to within the f16 store's own "
          "rounding floor, at an angle stressing the anisotropic apron formula on both axes "
          "at once");
  }

  // --- 8. Timing, at 1024^2 -------------------------------------------------
  {
    const int32_t sizePx = 1024;
    const TileStore field = extWholeField(sizePx, 31337);
    const PixelRect whole{0, 0, sizePx, sizePx};

    const EmbossParams eParams{2, -1, 1.0f, 1.0f};
    TileStore embossOut;
    const double embossMs =
        extTimeMs([&] { embossTiles(field, whole, eParams, &embossOut); });

    const MedianParams mParams{2};
    TileStore medianOutWhole;
    const double medianWholeMs =
        extTimeMs([&] { medianTiles(field, whole, mParams, &medianOutWhole); });
    TileStore medianOutPerTile;
    const double medianPerTileMs = extTimeMs([&] {
      for (int32_t ty = 0; ty < sizePx / kTileSize; ++ty) {
        for (int32_t tx = 0; tx < sizePx / kTileSize; ++tx) {
          medianTiles(field,
                      PixelRect{tx * kTileSize, ty * kTileSize, tx * kTileSize + kTileSize,
                                ty * kTileSize + kTileSize},
                      mParams, &medianOutPerTile);
        }
      }
    });

    const MotionBlurParams bParams{0.5f, 6};
    TileStore motionOutWhole;
    const double motionWholeMs =
        extTimeMs([&] { motionBlurTiles(field, whole, bParams, &motionOutWhole); });
    TileStore motionOutPerTile;
    const double motionPerTileMs = extTimeMs([&] {
      for (int32_t ty = 0; ty < sizePx / kTileSize; ++ty) {
        for (int32_t tx = 0; tx < sizePx / kTileSize; ++tx) {
          motionBlurTiles(field,
                          PixelRect{tx * kTileSize, ty * kTileSize, tx * kTileSize + kTileSize,
                                    ty * kTileSize + kTileSize},
                          bParams, &motionOutPerTile);
        }
      }
    });

    std::printf("  [measured] emboss %dx%d (dx=2,dy=-1): whole-region call %.3f ms\n", sizePx,
                sizePx, embossMs);
    std::printf(
        "  [measured] median %dx%d (radius=2): per-tile(64 calls) %.3f ms, whole-region %.3f "
        "ms, %.2fx\n",
        sizePx, sizePx, medianPerTileMs, medianWholeMs, medianPerTileMs / medianWholeMs);
    std::printf(
        "  [measured] motion blur %dx%d (radius=6): per-tile(64 calls) %.3f ms, whole-region "
        "%.3f ms, %.2fx\n",
        sizePx, sizePx, motionPerTileMs, motionWholeMs, motionPerTileMs / motionWholeMs);
    // Printed rather than check()-gated -- wall-clock is this suite's own
    // documented flake class (app/selftest/Parallel.cpp's identical
    // decision for blur and highpass's timing lines).
  }

  std::printf("[selftest] filtersExt %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
