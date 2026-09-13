#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/Command.hpp"
#include "app/FilterCommandsFilters.hpp"
#include "app/FilterOps.hpp"
#include "core/SelectionMask.hpp"
#include "ops/Filters.hpp"
#include "ops/PointOps.hpp"

// PRD D12: Image > Adjustments > Shadows/Highlights --
// ops/Filters.hpp section 12, `shadowsHighlightsTiles()`. The property under
// test throughout is the SPATIAL one PRD D12 exists for: a texel's push
// depends on its NEIGHBOURHOOD (the blurred guide), never on its own value,
// which a per-pixel curve cannot reproduce.
namespace np {
namespace {

// A 256x256 (2x2-tile), fully opaque field split down the middle: a dark
// background on the left (straight grey 0.05, below the mid-grey pivot), a
// bright one on the right (0.9, above it). Two probe texels carry the
// IDENTICAL value (0.3) in each half, far enough from the x=128 seam (96
// texels, well past sigma 20's own reach) that each probe's blurred guide
// reads its own half's background, not a blend of both -- and a single
// coloured patch in the dark half, for the hue-preservation check.
constexpr int32_t kSize = 256;
constexpr int32_t kDarkProbeX = 32;
constexpr int32_t kBrightProbeX = 224;
constexpr int32_t kProbeY = 128;
constexpr float kProbeValue = 0.3f;
constexpr int32_t kColourX = 32;
constexpr int32_t kColourY = 64;
constexpr std::array<float, 3> kColourProbe{0.30f, 0.15f, 0.05f};

TileStore shFixture() {
  TileStore tiles;
  for (int32_t ty = 0; ty < kSize / kTileSize; ++ty) {
    for (int32_t tx = 0; tx < kSize / kTileSize; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const int32_t x = tx * kTileSize + lx;
          const float v = x < kSize / 2 ? 0.05f : 0.9f;
          t.writePixel(PixelCoord{lx, ly}, {v, v, v, 1.0f});
        }
      }
    }
  }
  auto put = [&](int32_t x, int32_t y, const std::array<float, 4>& v) {
    Tile& t = tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}));
    t.writePixel(tileLocalOffset(PixelCoord{x, y}), v);
  };
  put(kDarkProbeX, kProbeY, {kProbeValue, kProbeValue, kProbeValue, 1.0f});
  put(kBrightProbeX, kProbeY, {kProbeValue, kProbeValue, kProbeValue, 1.0f});
  put(kColourX, kColourY, {kColourProbe[0], kColourProbe[1], kColourProbe[2], 1.0f});
  return tiles;
}

std::array<float, 4> readAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

bool tilesExactlyEqual(const TileStore& a, const TileStore& b) {
  if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a) {
    const Tile* other = b.find(coord);
    if (other == nullptr) return false;
    if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0)
      return false;
  }
  return true;
}

// Mean straight value over a small block, avoiding both probes and the
// x=128 seam.
float blockMean(const TileStore& store, int32_t x0, int32_t y0) {
  float sum = 0.0f;
  int32_t n = 0;
  for (int32_t y = y0; y < y0 + 8; ++y)
    for (int32_t x = x0; x < x0 + 8; ++x) {
      sum += readAt(store, x, y)[0];
      ++n;
    }
  return sum / static_cast<float>(n);
}

OpenDocument makeShadowsHighlightsDocument(const char* title) {
  OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, title);
  *od.document.layers[0].rgbTiles = shFixture();
  od.recordEdit("shadows/highlights fixture", EditKind::Content);
  return od;
}

ShadowsHighlightsParams testParams() noexcept {
  ShadowsHighlightsParams p;
  p.blur.kind = BlurKind::Gaussian;
  p.blur.sigma = 20.0f;
  p.shadows = 1.0f;
  p.highlights = 1.0f;
  p.tonalWidth = 0.15f;
  return p;
}

}  // namespace

bool runShadowsHighlightsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const PixelRect kCanvasRect{0, 0, kSize, kSize};
  const ShadowsHighlightsParams p = testParams();

  std::printf("  -- A. the fixture actually puts each probe on the side the test needs --\n");
  {
    check(readAt(shFixture(), kDarkProbeX, kProbeY) == readAt(shFixture(), kBrightProbeX, kProbeY),
          "fixture: the two probes start bit-identical (0.3 in both halves)");

    TileStore blurred;
    check(blurTiles(shFixture(), kCanvasRect, p.blur, &blurred),
          "fixture: the reference guide blur succeeds");
    auto guideLumaAt = [&](int32_t x, int32_t y) {
      const std::array<float, 4> g = readAt(blurred, x, y);
      if (!(g[3] > 0.0f)) return 0.0f;
      return computeLuma({g[0] / g[3], g[1] / g[3], g[2] / g[3]}, kRec709LumaWeights);
    };
    const float darkGuide = guideLumaAt(kDarkProbeX, kProbeY);
    const float brightGuide = guideLumaAt(kBrightProbeX, kProbeY);
    check(darkGuide < kShadowsHighlightsMidGray,
          "fixture: the dark probe's own guide reads below mid grey");
    check(brightGuide > kShadowsHighlightsMidGray,
          "fixture: the bright probe's own guide reads above mid grey");
    check(shadowsHighlightsGain(p, darkGuide) > 1.0f,
          "fixture: the dark guide's gain, at these params, genuinely lifts (> 1)");
    check(shadowsHighlightsGain(p, brightGuide) < 1.0f,
          "fixture: the bright guide's gain, at these params, genuinely lowers (< 1)");
  }

  std::printf("  -- B. the SPATIAL property: same value, different neighbourhood, different result --\n");
  {
    TileStore out;
    check(shadowsHighlightsTiles(shFixture(), kCanvasRect, p, &out),
          "engine: the call succeeds");
    const float darkAfter = readAt(out, kDarkProbeX, kProbeY)[0];
    const float brightAfter = readAt(out, kBrightProbeX, kProbeY)[0];
    check(darkAfter > kProbeValue,
          "the dark-neighbourhood probe was lifted above its own source value");
    check(brightAfter < kProbeValue,
          "the bright-neighbourhood probe was lowered below its own source value");
    check(std::fabs(darkAfter - brightAfter) > 0.05f,
          "two texels that started with the IDENTICAL value end up clearly different -- the "
          "property a per-pixel curve cannot have");
  }

  std::printf("  -- C. shadows raises a dark region's mean; highlights lowers a bright one's --\n");
  {
    TileStore out;
    shadowsHighlightsTiles(shFixture(), kCanvasRect, p, &out);
    const float darkBefore = blockMean(shFixture(), 8, 200);
    const float darkAfter = blockMean(out, 8, 200);
    const float brightBefore = blockMean(shFixture(), 216, 200);
    const float brightAfter = blockMean(out, 216, 200);
    check(darkAfter > darkBefore + 0.01f, "the dark region's mean rose by a real amount");
    check(brightAfter < brightBefore - 0.01f, "the bright region's mean fell by a real amount");
  }

  std::printf("  -- D. hue is preserved: a single scalar gain moves all three channels together --\n");
  {
    TileStore out;
    shadowsHighlightsTiles(shFixture(), kCanvasRect, p, &out);
    const std::array<float, 4> before = readAt(shFixture(), kColourX, kColourY);
    const std::array<float, 4> after = readAt(out, kColourX, kColourY);
    check(after[0] != before[0], "the colour probe actually moved (else D proves nothing)");
    // f16's mantissa is 10 bits, so a stored ratio carries at best a 2^-10
    // relative step; three stored channels compound that to a handful of
    // steps, so 8*2^-10 is a tolerance derived from the store, not guessed.
    const float tol = 8.0f * std::exp2(-10.0f);
    auto relClose = [&](float a, float b) {
      return std::fabs(a - b) <= tol * std::max(std::fabs(a), std::fabs(b));
    };
    check(relClose(before[0] / before[1], after[0] / after[1]) &&
              relClose(before[1] / before[2], after[1] / after[2]),
          "R:G and G:B ratios are preserved to within the f16 store's own rounding budget");
  }

  std::printf("  -- E. both amounts 0 is the exact identity --\n");
  {
    ShadowsHighlightsParams zero = p;
    zero.shadows = 0.0f;
    zero.highlights = 0.0f;
    TileStore out;
    check(shadowsHighlightsTiles(shFixture(), kCanvasRect, zero, &out) &&
              tilesExactlyEqual(out, shFixture()),
          "shadows=0, highlights=0: bit-exact copy of the source");
  }

  std::printf("  -- F. the encoder replays bit-identical to the applier --\n");
  {
    OpenDocument applier = makeShadowsHighlightsDocument("sh encoder a");
    OpenDocument viaCommand = makeShadowsHighlightsDocument("sh encoder b");
    applyShadowsHighlights(applier, p);
    const CommandResult r = applyCommand(viaCommand, shadowsHighlightsCommand(p));
    check(r.ok && tilesExactlyEqual(*viaCommand.document.layers[0].rgbTiles,
                                    *applier.document.layers[0].rgbTiles),
          "shadowsHighlightsCommand() replays bit-identical to applyShadowsHighlights()");
  }

  std::printf("  -- G. invalid parameters are refused by name --\n");
  {
    check(!shadowsHighlightsParamsValid(ShadowsHighlightsParams{BlurParams{}, 0.5f, 0.0f, -0.1f}),
          "engine: a non-positive tonal width is refused");

    OpenDocument refusalDoc = makeShadowsHighlightsDocument("sh refusal radius");
    JsonValue zeroRadius = JsonValue::object();
    zeroRadius.set("radius", JsonValue::number(0));
    zeroRadius.set("shadows", JsonValue::number(0.5));
    const CommandResult r1 = applyCommand(
        refusalDoc, Command{"filter_shadows_highlights", std::move(zeroRadius)});
    check(!r1.ok && r1.status.find("radius") != std::string::npos,
          "command: radius 0 is refused by name -- a zero-radius guide equals the source");

    JsonValue neither = JsonValue::object();
    neither.set("radius", JsonValue::number(20));
    const CommandResult r2 =
        applyCommand(refusalDoc, Command{"filter_shadows_highlights", std::move(neither)});
    check(!r2.ok, "command: naming neither shadows nor highlights is refused, not a silent no-op");
  }

  std::printf("  -- H. a selection bounds the filter; texels outside it are untouched --\n");
  {
    OpenDocument od = makeShadowsHighlightsDocument("sh selection");
    od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);
    const TileStore before = *od.document.layers[0].rgbTiles;
    const FilterOpResult r = applyShadowsHighlights(od, p);
    check(r.refusal == PixelOpRefusal::None,
          "selection: a small selection on a filled layer is not refused");
    bool outsideUntouched = true;
    for (int32_t y = 200; y < 204 && outsideUntouched; ++y)
      for (int32_t x = 200; x < 204; ++x)
        if (readAt(*od.document.layers[0].rgbTiles, x, y) != readAt(before, x, y))
          outsideUntouched = false;
    check(outsideUntouched, "texels outside the selection are bit-identical to the source");
  }

  std::printf("[selftest] shadowsHighlights %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
