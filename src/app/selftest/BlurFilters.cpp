#include "app/selftest/Support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "app/BlurCommandsExtra.hpp"
#include "app/Command.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/FilterOps.hpp"
#include "core/SelectionMask.hpp"
#include "io/Json.hpp"
#include "ops/LensBlur.hpp"
#include "ops/RadialBlur.hpp"

// app/selftest/BlurFilters -- docs/operations.md §2.2: Radial/Spin+
// Zoom blur (ops/RadialBlur.hpp) and Lens blur (ops/LensBlur.hpp), and their
// app/FilterOps.hpp/Command wiring. The same shape as app/selftest/
// FiltersExt.cpp's own sections: engine correctness against fixtures built
// so the property under test cannot pass by the fixture being degenerate,
// then the app-layer wiring (refusal by name, selection respected).
namespace np {
namespace {

// The same private splitmix64-finalizer fixture source every ops/ selftest
// section keeps its own copy of (app/selftest/FiltersExt.cpp's own comment
// on why: one file's own change should not ripple into a fixture other
// files also build on).
float blurNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

// A single-tile field with real, non-flat, non-monotonic content -- the
// identical "why not a ramp/flat fill" argument app/selftest/CommandCallsites
// and FiltersExt both make: a filter that is secretly a no-op on a degenerate
// fixture would still look green.
TileStore blurWholeField(int32_t sizePx, uint64_t seed) {
  TileStore tiles;
  uint64_t counter = seed;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty) {
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = blurNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.3f + 0.4f * v, 1.0f});
        }
      }
    }
  }
  return tiles;
}

TileStore blurFlatField(int32_t sizePx, const std::array<float, 4>& v) {
  TileStore tiles;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, v);
    }
  return tiles;
}

// A step function of RADIUS ONLY, alternating every `bandWidth` texels --
// genuinely radially symmetric (a function of distance from `(cx, cy)` and
// nothing else), and coarse enough that a probe texel several texels from a
// band edge reads the identical stored value at every grid point a bilinear
// sample near it could touch. That is what turns "Spin leaves this
// unchanged" into a claim provable without a curvature bound: away from an
// edge, every tap this filter takes lands inside one constant band, so the
// only source of error left is float summation, not interpolation.
TileStore blurRadialBandField(int32_t sizePx, float cx, float cy, float bandWidth) {
  TileStore tiles;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const float x = static_cast<float>(tx * kTileSize + lx);
          const float y = static_cast<float>(ty * kTileSize + ly);
          const float r = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
          const int32_t band = static_cast<int32_t>(std::floor(r / bandWidth));
          const float v = (band % 2 == 0) ? 0.2f : 0.8f;
          t.writePixel(PixelCoord{lx, ly}, {v, v, v, 1.0f});
        }
      }
    }
  return tiles;
}

// The angular twin: a step function of ANGLE ONLY about `(cx, cy)`, boundaries
// at every `360/numBands` degrees starting at 0. Zoom moves along a ray of
// constant angle, so the identical "away from an edge, every tap lands in one
// constant band" argument applies with radius and angle swapped.
TileStore blurAngularBandField(int32_t sizePx, float cx, float cy, int32_t numBands) {
  TileStore tiles;
  const float step = 2.0f * 3.14159265f / static_cast<float>(numBands);
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const float x = static_cast<float>(tx * kTileSize + lx);
          const float y = static_cast<float>(ty * kTileSize + ly);
          const float theta = std::atan2(y - cy, x - cx);
          const int32_t band =
              static_cast<int32_t>(std::floor((theta + 3.14159265f) / step));
          const float v = (band % 2 == 0) ? 0.2f : 0.8f;
          t.writePixel(PixelCoord{lx, ly}, {v, v, v, 1.0f});
        }
      }
    }
  return tiles;
}

std::array<float, 4> blurReadAt(const TileStore& s, int32_t x, int32_t y) {
  const Tile* t = s.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

bool blurTilesBitExact(const TileStore& a, const TileStore& b, const PixelRect& rect) {
  for (int32_t y = rect.y0; y < rect.y1; ++y)
    for (int32_t x = rect.x0; x < rect.x1; ++x)
      if (blurReadAt(a, x, y) != blurReadAt(b, x, y)) return false;
  return true;
}

// Float summation/division round-off from adding `2*samples+1` copies of a
// value and dividing back by that count (both RadialBlur methods' box
// average) -- a few ULPs per addition, scaled by the value's own magnitude
// plus a small floor for a near-zero value. Not a curvature bound: the
// fixtures below are built so every tap this filter takes reads the IDENTICAL
// stored value, so interpolation contributes nothing and this is the whole
// error budget.
bool blurApproxEqual(const std::array<float, 4>& a, const std::array<float, 4>& b,
                     int32_t samples) {
  const float tol = static_cast<float>(2 * samples + 1) * 4e-6f + 1e-6f;
  for (int32_t c = 0; c < 4; ++c)
    if (std::fabs(a[static_cast<size_t>(c)] - b[static_cast<size_t>(c)]) > tol) return false;
  return true;
}

double blurTimeMs(const std::function<void()>& f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// --- LensBlur: an independent, brute-force O(radius^2) reference ----------
//
// Structurally unrelated to ops/LensBlur.cpp's row-interval/prefix-sum
// derivation: every texel in the `[-radius, radius]^2` box is tested
// individually against the aperture's own half-plane/circle definition, and
// summed directly -- no row-span table, no prefix sums. Built to prove that
// derivation is the scanline decomposition of THIS geometry, not merely of
// itself. `highlightBoost` is not replicated here (kept at 0 in every fixture
// that uses this reference) -- ops/LensBlur.cpp's own boost is a simple
// pointwise pre-scale asserted separately, and duplicating it here would only
// be proving the multiply against itself.
constexpr double kBlurPiD = 3.14159265358979323846;

bool blurLensApertureInsideRef(int32_t bladeCount, float rotation, int32_t radius, int32_t tx,
                               int32_t ty) noexcept {
  if (bladeCount == 0) {
    return static_cast<int64_t>(tx) * tx + static_cast<int64_t>(ty) * ty <=
          static_cast<int64_t>(radius) * radius;
  }
  const double apothem = radius * std::cos(kBlurPiD / bladeCount);
  for (int32_t k = 0; k < bladeCount; ++k) {
    const double phi = static_cast<double>(rotation) + (k + 0.5) * (2.0 * kBlurPiD / bladeCount);
    const double proj = tx * std::cos(phi) + ty * std::sin(phi);
    if (proj > apothem + 1e-6) return false;
  }
  return true;
}

TileStore blurLensNaiveReference(const TileStore& src, const PixelRect& outRect,
                                 int32_t bladeCount, float rotation, int32_t radius) {
  TileStore dst;
  int64_t area = 0;
  for (int32_t dy = -radius; dy <= radius; ++dy)
    for (int32_t dx = -radius; dx <= radius; ++dx)
      if (blurLensApertureInsideRef(bladeCount, rotation, radius, dx, dy)) ++area;
  const float inv = area > 0 ? 1.0f / static_cast<float>(area) : 0.0f;

  for (int32_t y = outRect.y0; y < outRect.y1; ++y) {
    for (int32_t x = outRect.x0; x < outRect.x1; ++x) {
      std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
      for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
          if (!blurLensApertureInsideRef(bladeCount, rotation, radius, dx, dy)) continue;
          const std::array<float, 4> s = blurReadAt(src, x + dx, y + dy);
          sum[0] += s[0];
          sum[1] += s[1];
          sum[2] += s[2];
          sum[3] += s[3];
        }
      }
      Tile& t = dst.getOrCreate(tileCoordAt(PixelCoord{x, y}));
      t.writePixel(tileLocalOffset(PixelCoord{x, y}),
                  {sum[0] * inv, sum[1] * inv, sum[2] * inv, sum[3] * inv});
    }
  }
  return dst;
}

// --- the command-level selection check, shared by both filters ------------

struct SelectionCheckResult {
  bool ok = false;
  bool outsideUnchanged = true;
  bool insideChanged = false;
  size_t texelsChanged = 0;
};

// A 64x64 fixture, the left half selected. Structurally independent of
// app/selftest/CommandsImage.cpp's own image-command fixture -- this file
// does not share it, for the identical "one section's change should not
// ripple into another" reason every fixture in this suite is its own copy.
SelectionCheckResult blurSelectionCheck(const char* commandId, const JsonValue& params) {
  OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "blur selection fixture");
  Tile& t = doc.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
  for (int32_t y = 0; y < 64; ++y) {
    for (int32_t x = 0; x < 64; ++x) {
      const float v = blurNoise(static_cast<uint64_t>(x) * 131u + static_cast<uint64_t>(y));
      t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.4f, 1.0f});
    }
  }
  doc.recordEdit("blur selection fixture", EditKind::Content);
  const TileStore before = *doc.document.layers[0].rgbTiles;
  doc.selection = selectRectangle(0.0f, 0.0f, 32.0f, 64.0f);

  const CommandResult r = applyCommand(doc, Command{commandId, params});
  SelectionCheckResult out;
  out.ok = r.ok;
  out.texelsChanged = r.texelsChanged;
  if (!r.ok) return out;

  const Tile* beforeTile = before.find(TileCoord{0, 0});
  const Tile* afterTile = doc.document.layers[0].rgbTiles->find(TileCoord{0, 0});
  for (int32_t y = 0; y < 64; ++y) {
    for (int32_t x = 0; x < 64; ++x) {
      const bool differ =
          beforeTile->readPixel(PixelCoord{x, y}) != afterTile->readPixel(PixelCoord{x, y});
      if (x >= 32) {
        if (differ) out.outsideUnchanged = false;
      } else if (differ) {
        out.insideChanged = true;
      }
    }
  }
  return out;
}

}  // namespace

bool runBlurFiltersTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. RadialBlur: params validity and ROI derivation --\n");
  {
    check(!radialBlurParamsValid(
              RadialBlurParams{RadialBlurMethod::Spin, std::numeric_limits<float>::quiet_NaN(),
                               0.0f, 10.0f, 8}),
          "radial: a non-finite centre is refused");
    check(!radialBlurParamsValid(RadialBlurParams{RadialBlurMethod::Spin, 0.0f, 0.0f, 10.0f, 0}),
          "radial: samples < 1 is refused");
    check(radialBlurParamsValid(RadialBlurParams{}),
          "radial: the default-constructed params (amount 0, the identity) are valid");

    const PixelRect r1{0, 0, 64, 64};
    check(radialBlurRoiOp(RadialBlurParams{RadialBlurMethod::Spin, 32.0f, 32.0f, 0.0f, 8}, r1) ==
              RoiOp{},
          "radial: amount 0 declares the identity apron");

    // The margin formula, recomputed independently -- what a sabotage of the
    // derivation (e.g. dropping Spin's /2, or using the WRONG corner) would
    // actually break.
    bool marginMatches = true;
    const PixelRect rects[] = {{0, 0, 64, 64}, {10, 10, 74, 40}, {-20, -20, 20, 20}};
    for (const RadialBlurMethod method : {RadialBlurMethod::Spin, RadialBlurMethod::Zoom}) {
      for (const float amount : {5.0f, -30.0f, 90.0f}) {
        for (const PixelRect& rect : rects) {
          const RadialBlurParams p{method, 15.0f, 25.0f, amount, 8};
          const RoiOp got = radialBlurRoiOp(p, rect);
          float rMax = 0.0f;
          for (float x : {static_cast<float>(rect.x0), static_cast<float>(rect.x1)})
            for (float y : {static_cast<float>(rect.y0), static_cast<float>(rect.y1)})
              rMax = std::max(rMax, std::sqrt((x - p.centerX) * (x - p.centerX) +
                                              (y - p.centerY) * (y - p.centerY)));
          const float reach = method == RadialBlurMethod::Spin
                                  ? rMax * std::fabs(amount) * (3.14159265f / 180.0f) * 0.5f
                                  : rMax * std::fabs(amount);
          const int32_t expected = static_cast<int32_t>(std::ceil(reach)) + 1;
          if (!(got == RoiOp{expected, expected, expected, expected, 0, 0})) marginMatches = false;
        }
      }
    }
    check(marginMatches,
          "radial: the isotropic margin matches an independent recomputation of the same formula");
  }

  std::printf("  -- B. RadialBlur: a constant image stays constant (normalisation) --\n");
  {
    const TileStore flat = blurFlatField(96, {0.3f, 0.6f, 0.9f, 1.0f});
    const PixelRect whole{0, 0, 96, 96};
    for (const RadialBlurMethod method : {RadialBlurMethod::Spin, RadialBlurMethod::Zoom}) {
      TileStore out;
      // Spin's amount is degrees, Zoom's is a scale fraction -- the same two
      // units section D's "which axis" tests use, not one magnitude reused
      // for both (a fraction of 40 would zoom every tap into the padding
      // beyond the flat field's own tile, reading it back as zero and
      // measuring the apron's fallback rather than normalisation).
      const RadialBlurParams p{method, 48.0f, 48.0f, method == RadialBlurMethod::Spin ? 40.0f : 0.4f,
                               8};
      radialBlurTiles(flat, whole, p, &out);
      check(blurApproxEqual(blurReadAt(flat, 60, 55), blurReadAt(out, 60, 55), p.samples),
            method == RadialBlurMethod::Spin
                ? "radial: Spin of a constant image stays constant (the box average normalises)"
                : "radial: Zoom of a constant image stays constant (the box average normalises)");
    }
  }

  std::printf("  -- C. RadialBlur: amount 0 identity, and the centre texel fixed point --\n");
  {
    const TileStore field = blurWholeField(96, 777);
    const PixelRect whole{0, 0, 96, 96};

    for (const RadialBlurMethod method : {RadialBlurMethod::Spin, RadialBlurMethod::Zoom}) {
      TileStore out;
      const RadialBlurParams zero{method, 40.0f, 50.0f, 0.0f, 8};
      radialBlurTiles(field, whole, zero, &out);
      check(blurTilesBitExact(field, out, whole),
            method == RadialBlurMethod::Spin ? "radial: Spin amount 0 is a bit-exact identity"
                                             : "radial: Zoom amount 0 is a bit-exact identity");
    }

    const float cx = 48.0f, cy = 48.0f;
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Spin, cx, cy, 45.0f, 8};
      radialBlurTiles(field, whole, p, &out);
      check(blurApproxEqual(blurReadAt(field, 48, 48), blurReadAt(out, 48, 48), p.samples),
            "radial: Spin's centre texel is a fixed point");
    }
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Zoom, cx, cy, 0.6f, 8};
      radialBlurTiles(field, whole, p, &out);
      check(blurApproxEqual(blurReadAt(field, 48, 48), blurReadAt(out, 48, 48), p.samples),
            "radial: Zoom's centre texel is a fixed point");
    }
  }

  std::printf("  -- D. RadialBlur: which axis each method actually blurs --\n");
  {
    const int32_t size = 96;
    const float cx = 48.0f, cy = 48.0f;
    const TileStore radialBands = blurRadialBandField(size, cx, cy, 20.0f);
    const TileStore angularBands = blurAngularBandField(size, cx, cy, 8);  // 45-degree sectors
    const PixelRect whole{0, 0, size, size};

    // Spin of a radially-symmetric image is unchanged: probe at r=30, deep
    // inside the [20,40) band, with a sweep small enough (+/-5 degrees) that
    // even its farthest tap (30 * sin(5 deg) ~= 2.6 texels) stays inside it.
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Spin, cx, cy, 10.0f, 8};
      radialBlurTiles(radialBands, whole, p, &out);
      const int32_t px = static_cast<int32_t>(cx) + 30, py = static_cast<int32_t>(cy);
      check(blurApproxEqual(blurReadAt(radialBands, px, py), blurReadAt(out, px, py), p.samples),
            "radial: Spin of a radially-symmetric image (constant on its own circles) is "
            "unchanged");
    }

    // Zoom of an angularly-constant image is unchanged: probe at theta ~=
    // 22.5 degrees (mid-sector, as far as a 45-degree sector gets from either
    // edge), amount +/-0.3 so even the innermost tap (r*0.7) stays a healthy
    // arc-distance from a boundary.
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Zoom, cx, cy, 0.3f, 8};
      radialBlurTiles(angularBands, whole, p, &out);
      const int32_t px = static_cast<int32_t>(cx) + 28, py = static_cast<int32_t>(cy) + 12;
      check(blurApproxEqual(blurReadAt(angularBands, px, py), blurReadAt(out, px, py), p.samples),
            "radial: Zoom of an angularly-constant image (constant along its own rays) is "
            "unchanged");
    }

    // Spin of an angularly-varying image DOES change: probe one texel past
    // the 0-degree sector boundary, with a 20-degree sweep that straddles it.
    // Non-degeneracy first -- the two bands either side of that boundary must
    // actually differ, or a "the output changed" result would prove nothing.
    {
      const int32_t px = static_cast<int32_t>(cx) + 30, py = static_cast<int32_t>(cy) + 1;
      const int32_t qx = static_cast<int32_t>(cx) + 30, qy = static_cast<int32_t>(cy) - 1;
      const std::array<float, 4> justPast = blurReadAt(angularBands, px, py);
      const std::array<float, 4> justBefore = blurReadAt(angularBands, qx, qy);
      check(justPast != justBefore,
            "radial: the angular fixture is non-degenerate at the boundary this probes");

      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Spin, cx, cy, 20.0f, 8};
      radialBlurTiles(angularBands, whole, p, &out);
      const std::array<float, 4> before = blurReadAt(angularBands, px, py);
      const std::array<float, 4> after = blurReadAt(out, px, py);
      const float diff = std::fabs(after[0] - before[0]);
      check(diff > 0.05f,
            "radial: Spin of an angularly-varying image DOES change a texel near a sector edge");
    }

    // Zoom of a radial ramp DOES change: probe one texel past the r=20 band
    // boundary, with a zoom range that reaches back across it.
    {
      const int32_t px = static_cast<int32_t>(cx) + 21, py = static_cast<int32_t>(cy);
      const int32_t qx = static_cast<int32_t>(cx) + 19, qy = static_cast<int32_t>(cy);
      check(blurReadAt(radialBands, px, py) != blurReadAt(radialBands, qx, qy),
            "radial: the radial fixture is non-degenerate at the boundary this probes");

      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Zoom, cx, cy, 0.3f, 8};
      radialBlurTiles(radialBands, whole, p, &out);
      const float diff = std::fabs(blurReadAt(out, px, py)[0] - blurReadAt(radialBands, px, py)[0]);
      check(diff > 0.05f, "radial: Zoom of a radial ramp DOES change a texel near a radial edge");
    }
  }

  std::printf("  -- E. RadialBlur: ROI is sufficient (a split request agrees with the whole) --\n");
  {
    const TileStore field = blurWholeField(128, 4242);
    const PixelRect whole{0, 0, 128, 128};
    const RadialBlurParams p{RadialBlurMethod::Spin, 64.0f, 64.0f, 30.0f, 6};

    TileStore fromWhole;
    radialBlurTiles(field, whole, p, &fromWhole);

    TileStore fromSub;
    radialBlurTiles(field, PixelRect{40, 40, 90, 90}, p, &fromSub);

    bool subMatchesWhole = true;
    for (int32_t y = 40; y < 90; ++y)
      for (int32_t x = 40; x < 90; ++x)
        if (blurReadAt(fromSub, x, y) != blurReadAt(fromWhole, x, y)) subMatchesWhole = false;
    check(subMatchesWhole,
          "radial: the output inside a sub-rectangle equals the whole-canvas run at those texels");
  }

  std::printf("  -- F. LensBlur: params validity and ROI --\n");
  {
    check(!lensBlurParamsValid(LensBlurParams{0, 0.0f, -1, 1.0f, 0.0f}), "lens: negative radius refused");
    check(!lensBlurParamsValid(LensBlurParams{0, 0.0f, kLensBlurMaxRadius + 1, 1.0f, 0.0f}),
          "lens: a radius above the cap is refused");
    check(!lensBlurParamsValid(LensBlurParams{1, 0.0f, 10, 1.0f, 0.0f}),
          "lens: bladeCount 1 (neither 0 nor in [3,8]) refused");
    check(!lensBlurParamsValid(LensBlurParams{2, 0.0f, 10, 1.0f, 0.0f}), "lens: bladeCount 2 refused");
    check(!lensBlurParamsValid(LensBlurParams{9, 0.0f, 10, 1.0f, 0.0f}), "lens: bladeCount 9 refused");
    check(lensBlurParamsValid(LensBlurParams{0, 0.0f, 10, 1.0f, 0.0f}), "lens: bladeCount 0 (circle) accepted");
    check(lensBlurParamsValid(LensBlurParams{6, 0.0f, 10, 1.0f, 0.0f}), "lens: bladeCount 6 accepted");
    check(!lensBlurParamsValid(LensBlurParams{0, 0.0f, 10, 1.0f, -0.1f}),
          "lens: a negative highlightBoost refused");

    check(lensBlurRoiOp(LensBlurParams{6, 0.0f, 0, 1.0f, 0.0f}) == RoiOp{},
          "lens: radius 0 declares the identity apron");
    check(lensBlurRoiOp(LensBlurParams{6, 0.0f, 12, 1.0f, 0.0f}) == roiDilateOp(12),
          "lens: radius R declares an isotropic dilation by R, exactly a blur's");
  }

  std::printf("  -- G. LensBlur: radius 0 is a bit-exact identity --\n");
  {
    const TileStore field = blurWholeField(64, 555);
    const PixelRect whole{0, 0, 64, 64};
    TileStore out;
    lensBlurTiles(field, whole, LensBlurParams{6, 0.2f, 0, 1.0f, 0.0f}, &out);
    check(blurTilesBitExact(field, out, whole), "lens: radius 0 leaves every texel unchanged");
  }

  std::printf("  -- H. LensBlur: the discrete aperture area matches the polygon-area formula --\n");
  {
    bool areaMatches = true;
    for (const int32_t bladeCount : {3, 4, 5, 6, 7, 8}) {
      for (const int32_t radius : {8, 16, 30}) {
        const int64_t count = lensApertureTexelCount(bladeCount, 0.0f, radius);
        const double continuous =
            0.5 * bladeCount * radius * radius * std::sin(2.0 * kBlurPiD / bladeCount);
        // Discretisation error for a convex shape rasterised on a unit grid
        // is O(perimeter) (the boundary cells, not the interior, are where a
        // continuous area and a texel count can disagree) -- the regular
        // polygon's own perimeter, times a safety factor, is the tolerance.
        const double perimeter = bladeCount * 2.0 * radius * std::sin(kBlurPiD / bladeCount);
        const double tol = perimeter * 1.5 + 8.0;
        if (std::fabs(static_cast<double>(count) - continuous) > tol) areaMatches = false;
      }
    }
    for (const int32_t radius : {8, 16, 30}) {
      const int64_t count = lensApertureTexelCount(0, 0.0f, radius);
      const double continuous = kBlurPiD * radius * radius;
      const double tol = 2.0 * kBlurPiD * radius * 1.5 + 8.0;
      if (std::fabs(static_cast<double>(count) - continuous) > tol) areaMatches = false;
    }
    check(areaMatches,
          "lens: the discrete aperture's texel count is within a perimeter-scaled tolerance of "
          "the continuous polygon/circle area");
  }

  std::printf("  -- I. LensBlur: a single bright texel becomes the aperture's own shape --\n");
  {
    const int32_t canvasSize = 96;
    const int32_t radius = 15;
    const int32_t sx = 48, sy = 48;  // >= radius+2 from every edge on a 96px canvas
    TileStore src;
    src.getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{sx, sy}, {1.0f, 1.0f, 1.0f, 1.0f});
    const PixelRect whole{0, 0, canvasSize, canvasSize};

    auto litCount = [&](const TileStore& s) {
      int32_t n = 0;
      for (int32_t y = 0; y < canvasSize; ++y)
        for (int32_t x = 0; x < canvasSize; ++x)
          if (blurReadAt(s, x, y)[3] > 1e-6f) ++n;
      return n;
    };

    TileStore hexOut;
    lensBlurTiles(src, whole, LensBlurParams{6, 0.0f, radius, 1.0f, 0.0f}, &hexOut);
    const int32_t hexLit = litCount(hexOut);
    check(hexLit == lensApertureTexelCount(6, 0.0f, radius),
          "lens: the hexagon's lit-texel count matches the engine's own declared aperture size "
          "exactly (the source sits far from every edge)");

    TileStore circOut;
    lensBlurTiles(src, whole, LensBlurParams{0, 0.0f, radius, 1.0f, 0.0f}, &circOut);
    const int32_t circLit = litCount(circOut);
    check(circLit == lensApertureTexelCount(0, 0.0f, radius),
          "lens: the circle's lit-texel count matches the engine's own declared aperture size "
          "exactly");

    int32_t disagree = 0;
    for (int32_t y = 0; y < canvasSize; ++y)
      for (int32_t x = 0; x < canvasSize; ++x) {
        const bool hexLitHere = blurReadAt(hexOut, x, y)[3] > 1e-6f;
        const bool circLitHere = blurReadAt(circOut, x, y)[3] > 1e-6f;
        if (hexLitHere != circLitHere) ++disagree;
      }
    check(disagree > 0,
          "lens: the hexagon's lit region is NOT the circle's -- 6 blades really renders a "
          "polygon, not a disc");
  }

  std::printf("  -- J. LensBlur: energy is conserved without the specular boost --\n");
  {
    const int32_t canvasSize = kTileSize;  // one tile: no off-canvas reads to worry about
    const int32_t radius = 10;
    TileStore src;
    Tile& t = src.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 40; y < 60; ++y) {
      for (int32_t x = 40; x < 60; ++x) {
        const float v = blurNoise(static_cast<uint64_t>(x) * 97u + static_cast<uint64_t>(y));
        t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f, 1.0f});
      }
    }
    const PixelRect whole{0, 0, canvasSize, canvasSize};
    TileStore out;
    lensBlurTiles(src, whole, LensBlurParams{6, 0.3f, radius, 1.0f, 0.0f}, &out);

    double sumBefore[4] = {0, 0, 0, 0};
    double sumAfter[4] = {0, 0, 0, 0};
    for (int32_t y = 0; y < canvasSize; ++y) {
      for (int32_t x = 0; x < canvasSize; ++x) {
        const std::array<float, 4> b = blurReadAt(src, x, y);
        const std::array<float, 4> a = blurReadAt(out, x, y);
        for (int32_t c = 0; c < 4; ++c) {
          sumBefore[c] += b[static_cast<size_t>(c)];
          sumAfter[c] += a[static_cast<size_t>(c)];
        }
      }
    }
    bool conserved = true;
    for (int32_t c = 0; c < 4; ++c) {
      const double tol = 1e-3 * std::max(1.0, std::fabs(sumBefore[c]));
      if (std::fabs(sumAfter[c] - sumBefore[c]) > tol) conserved = false;
    }
    check(conserved,
          "lens: total energy in linear light is preserved (boost 0, content far from the edge)");
  }

  std::printf(
      "  -- K. LensBlur: the scanline decomposition equals an independent O(radius^2) "
      "reference --\n");
  {
    const int32_t size = 48;
    const int32_t radius = 6;
    const TileStore field = blurWholeField(size, 91);
    const PixelRect probe{radius + 2, radius + 2, size - radius - 2, size - radius - 2};

    for (const int32_t bladeCount : {0, 5, 6}) {
      const float rotation = bladeCount == 0 ? 0.0f : 0.4f;
      TileStore fast;
      lensBlurTiles(field, probe, LensBlurParams{bladeCount, rotation, radius, 1.0f, 0.0f}, &fast);
      const TileStore reference = blurLensNaiveReference(field, probe, bladeCount, rotation, radius);

      bool agree = true;
      for (int32_t y = probe.y0; y < probe.y1; ++y) {
        for (int32_t x = probe.x0; x < probe.x1; ++x) {
          const std::array<float, 4> f = blurReadAt(fast, x, y);
          const std::array<float, 4> r = blurReadAt(reference, x, y);
          for (int32_t c = 0; c < 4; ++c) {
            // The prefix-sum path and the direct-sum reference add the SAME
            // set of texels (both derive membership from the identical
            // half-plane/circle test) in a DIFFERENT order, so this is float
            // summation round-off over ~(2*radius+1)^2 terms, not a
            // correctness gap -- see this file's own comment on why the two
            // are structurally independent.
            if (std::fabs(f[static_cast<size_t>(c)] - r[static_cast<size_t>(c)]) > 2e-4f)
              agree = false;
          }
        }
      }
      check(agree, bladeCount == 0
                       ? "lens: the fast circle matches the brute-force reference"
                       : "lens: the fast polygon matches the brute-force reference");
    }
  }

  std::printf("  -- L. LensBlur: measured cost on a 2000-ish square layer --\n");
  {
    const int32_t size = 2048;
    const TileStore field = blurWholeField(size, 202);
    const PixelRect whole{0, 0, size, size};
    // docs/operations.md §2.3's own worked example: radius 64, the brute
    // force's "~12,000 taps per pixel". This file's fast path is O(radius)
    // per texel, not O(radius^2), which is the number below is measuring.
    const LensBlurParams p{6, 0.25f, 64, 1.0f, 0.0f};
    TileStore out;
    const double ms = blurTimeMs([&] { lensBlurTiles(field, whole, p, &out); });
    std::printf("     [measured] lens blur %dx%d radius=%d bladeCount=6: %.1f ms\n", size, size,
                p.radius, ms);
    // Printed rather than check()-gated -- wall-clock is this suite's own
    // documented flake class (app/selftest/FiltersExt.cpp's identical
    // decision for its own blur/median/motion-blur timing lines).
  }

  std::printf("  -- M. Both: refused by name, and the selection is respected --\n");
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "blur refusal fixture");
    auto num = [](double d) { return JsonValue::number(d); };

    {
      JsonValue p = JsonValue::object();
      p.set("amount", num(0.0));
      const CommandResult r = applyCommand(doc, Command{"filter_radial_blur", p});
      check(!r.ok && contains(r.status, "amount"), "radial: amount 0 is refused, naming amount");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("amount", num(10.0));
      p.set("samples", num(0));
      const CommandResult r = applyCommand(doc, Command{"filter_radial_blur", p});
      check(!r.ok && contains(r.status, "samples"), "radial: samples 0 is refused, naming samples");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("method", JsonValue::string("spiral"));
      p.set("amount", num(10.0));
      const CommandResult r = applyCommand(doc, Command{"filter_radial_blur", p});
      check(!r.ok && contains(r.status, "method"),
            "radial: an unknown method name is refused, naming method");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("radius", num(kLensBlurMaxRadius + 1));
      const CommandResult r = applyCommand(doc, Command{"filter_lens_blur", p});
      check(!r.ok, "lens: a radius above the cap is refused");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("radius", num(5));
      p.set("blade_count", num(2));
      const CommandResult r = applyCommand(doc, Command{"filter_lens_blur", p});
      check(!r.ok, "lens: a blade_count of 2 is refused");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("radius", num(5));
      p.set("highlight_boost", num(-0.5));
      const CommandResult r = applyCommand(doc, Command{"filter_lens_blur", p});
      check(!r.ok, "lens: a negative highlight_boost is refused");
    }

    {
      JsonValue p = JsonValue::object();
      p.set("method", JsonValue::string("zoom"));
      p.set("center_x", num(16.0));
      p.set("center_y", num(32.0));
      p.set("amount", num(0.6));
      p.set("samples", num(6));
      const SelectionCheckResult res = blurSelectionCheck("filter_radial_blur", p);
      check(res.ok && res.texelsChanged > 0, "radial: a real request succeeds and changes pixels");
      check(res.outsideUnchanged, "radial: texels outside the selection are untouched");
      check(res.insideChanged, "radial: texels inside the selection changed");
    }
    {
      JsonValue p = JsonValue::object();
      p.set("radius", num(8));
      p.set("blade_count", num(6));
      const SelectionCheckResult res = blurSelectionCheck("filter_lens_blur", p);
      check(res.ok && res.texelsChanged > 0, "lens: a real request succeeds and changes pixels");
      check(res.outsideUnchanged, "lens: texels outside the selection are untouched");
      check(res.insideChanged, "lens: texels inside the selection changed");
    }
  }

  std::printf("  -- N. Radial encoder round-trip, one per method --\n");
  {
    // The dialog records through radialBlurCommand(); the callsite table's
    // fixture is Spin only, so an encoder that dropped the method passed.
    auto noiseDoc = [] {
      OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "radial round-trip fixture");
      Tile& t = doc.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < 64; ++y)
        for (int32_t x = 0; x < 64; ++x) {
          const float v = blurNoise(static_cast<uint64_t>(x) * 131u + static_cast<uint64_t>(y));
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.4f, 1.0f});
        }
      doc.recordEdit("radial round-trip fixture", EditKind::Content);
      return doc;
    };
    const PixelRect whole{0, 0, 64, 64};
    const RadialBlurParams spin{RadialBlurMethod::Spin, 21.0f, 37.0f, 12.0f, 5};
    const RadialBlurParams zoom{RadialBlurMethod::Zoom, 21.0f, 37.0f, 0.35f, 5};

    std::vector<TileStore> viaCommand;
    for (const RadialBlurParams& params : {spin, zoom}) {
      OpenDocument viaEncoder = noiseDoc();
      OpenDocument direct = noiseDoc();
      const CommandResult r = applyCommand(viaEncoder, radialBlurCommand(params));
      applyRadialBlur(direct, params);
      const std::string name = radialBlurMethodName(params.method);
      const std::string applies = "radial round-trip: the encoded " + name + " command applies";
      const std::string identical = "radial round-trip: applyCommand(radialBlurCommand(" + name +
                                    ")) is bit-identical to applyRadialBlur";
      check(r.ok, applies.c_str());
      check(blurTilesBitExact(*viaEncoder.document.layers[0].rgbTiles,
                              *direct.document.layers[0].rgbTiles, whole),
            identical.c_str());
      viaCommand.push_back(*viaEncoder.document.layers[0].rgbTiles);
    }
    check(!blurTilesBitExact(viaCommand[0], viaCommand[1], whole),
          "radial round-trip fixture: Spin and Zoom give different results, so a lost method shows");

    // A hand-written action may omit the centre; it means the canvas centre,
    // (32, 32) on this 64 x 64 document -- not (0, 0).
    OpenDocument noCentre = noiseDoc();
    OpenDocument explicitCentre = noiseDoc();
    JsonValue params = JsonValue::object();
    params.set("method", JsonValue::string("spin"));
    params.set("amount", JsonValue::number(12.0));
    params.set("samples", JsonValue::number(5));
    const CommandResult r = applyCommand(noCentre, Command{"filter_radial_blur", params});
    applyRadialBlur(explicitCentre, RadialBlurParams{RadialBlurMethod::Spin, 32.0f, 32.0f, 12.0f, 5});
    check(r.ok && blurTilesBitExact(*noCentre.document.layers[0].rgbTiles,
                                    *explicitCentre.document.layers[0].rgbTiles, whole),
          "radial: a command without center_x/center_y blurs about the canvas centre");
  }

  std::printf("  -- O. RadialBlur: how far one lit texel is swept --\n");
  {
    // Section D proves which axis moves, not how far. One opaque texel at
    // (68, 48), 20 texels right of the centre, on a transparent field.
    const int32_t size = 96;
    const float cx = 48.0f, cy = 48.0f;
    TileStore impulse = blurFlatField(size, {0.0f, 0.0f, 0.0f, 0.0f});
    impulse.getOrCreate(tileCoordAt(PixelCoord{68, 48}))
        .writePixel(tileLocalOffset(PixelCoord{68, 48}), {1.0f, 1.0f, 1.0f, 1.0f});
    const PixelRect whole{0, 0, size, size};
    const float alphaLit = 0.005f;

    // Spin 60 degrees reaches +/-30 degrees along the r=20 circle: lit at
    // 0.75-0.85 of the half-angle, and more than 3 texels of arc past its end
    // nothing can reach.
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Spin, cx, cy, 60.0f, 16};
      radialBlurTiles(impulse, whole, p, &out);
      const float half = 30.0f * (3.14159265f / 180.0f);
      float litBand = 0.0f, pastEnd = 0.0f;
      int32_t litTexels = 0, pastTexels = 0;
      for (int32_t y = 0; y < size; ++y)
        for (int32_t x = 0; x < size; ++x) {
          const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
          if (std::fabs(std::sqrt(dx * dx + dy * dy) - 20.0f) >= 0.5f) continue;
          const float angle = std::atan2(dy, dx);
          const float a = blurReadAt(out, x, y)[3];
          if (angle >= 0.75f * half && angle <= 0.85f * half) {
            litBand = std::max(litBand, a);
            ++litTexels;
          } else if (angle >= 1.3f * half && angle <= 1.6f * half) {
            pastEnd = std::max(pastEnd, a);
            ++pastTexels;
          }
        }
      check(litTexels > 0 && pastTexels > 0, "radial sweep fixture: both Spin probe bands hold texels");
      check(litBand > alphaLit, "radial: Spin 60 reaches 0.8 of its 30-degree half-angle");
      check(pastEnd < 1e-6f, "radial: Spin 60 leaves the arc past 30 degrees untouched");
    }

    // Zoom 0.4 samples 0.6-1.4 times each radius, so the r=20 texel lights
    // radii 20/1.4 = 14.3 through 20/0.6 = 33.3 along the ray.
    {
      TileStore out;
      const RadialBlurParams p{RadialBlurMethod::Zoom, cx, cy, 0.4f, 16};
      radialBlurTiles(impulse, whole, p, &out);
      const auto alphaAt = [&](int32_t d) { return blurReadAt(out, 48 + d, 48)[3]; };
      check(alphaAt(15) > alphaLit, "radial: Zoom 0.4 reaches radius 15 (source read at x1.4)");
      check(alphaAt(32) > alphaLit, "radial: Zoom 0.4 reaches radius 32 (source read at x0.625)");
      check(alphaAt(11) < 1e-6f, "radial: Zoom 0.4 leaves radius 11 untouched");
      check(alphaAt(37) < 1e-6f, "radial: Zoom 0.4 leaves radius 37 untouched");
    }
  }

  std::printf("  -- P. LensBlur: the highlight boost brightens colour, never coverage --\n");
  {
    const int32_t size = 32;
    TileStore field;
    Tile& t = field.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < size; ++y)
      for (int32_t x = 0; x < size; ++x) {
        const bool bright = blurNoise(static_cast<uint64_t>(y) * 977u + static_cast<uint64_t>(x)) > 0.5f;
        const float a = 0.6f;
        const float c = a * (bright ? 0.9f : 0.1f);
        t.writePixel(PixelCoord{x, y}, {c, c, c, a});
      }
    const PixelRect probe{4, 4, size - 4, size - 4};
    TileStore boosted, plain;
    lensBlurTiles(field, probe, LensBlurParams{6, 0.2f, 3, 0.3f, 1.5f}, &boosted);
    lensBlurTiles(field, probe, LensBlurParams{6, 0.2f, 3, 0.3f, 0.0f}, &plain);
    bool alphaSame = true, colourChanged = false;
    for (int32_t y = probe.y0; y < probe.y1; ++y)
      for (int32_t x = probe.x0; x < probe.x1; ++x) {
        const std::array<float, 4> b = blurReadAt(boosted, x, y), q = blurReadAt(plain, x, y);
        if (b[3] != q[3]) alphaSame = false;
        if (b[0] != q[0]) colourChanged = true;
      }
    check(colourChanged, "lens boost fixture: the boost brightens some texel's colour");
    check(alphaSame, "lens: the highlight boost leaves alpha exactly as boost 0 does");
  }

  std::printf("[selftest] blurFilters %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
