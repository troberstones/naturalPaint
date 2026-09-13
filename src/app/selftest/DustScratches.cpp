#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/Command.hpp"
#include "app/FilterCommandsFilters.hpp"
#include "app/FilterOps.hpp"
#include "core/SelectionMask.hpp"
#include "ops/Filters.hpp"

// PRD D11: Filter > Dust & Scratches -- ops/Filters.hpp
// section 11, `dustScratchesTiles()`. Median gated by a threshold; this file
// proves the gate itself (the engine already has section 8's own seam and
// rank-statistic coverage in app/selftest/FiltersExt.cpp, and this filter
// reuses that exact per-texel computation rather than a second copy of it).
namespace np {
namespace {

// A 256x256 (2x2-tile), fully opaque, smoothly varying grey ramp, with ONE
// texel driven far off it -- app/selftest/FilterMenu.cpp's own fixture shape,
// restated for a fixture that needs a "speck" rather than merely "content".
// Opaque throughout so straight and premultiplied agree everywhere and the
// gate's alpha term stays at exactly 0 -- the colour term is what section 11
// is actually testing.
constexpr int32_t kSize = 256;
constexpr int32_t kSpeckX = 128;
constexpr int32_t kSpeckY = 128;

TileStore dustFixture() {
  TileStore tiles;
  for (int32_t ty = 0; ty < kSize / kTileSize; ++ty) {
    for (int32_t tx = 0; tx < kSize / kTileSize; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const int32_t x = tx * kTileSize + lx;
          const int32_t y = ty * kTileSize + ly;
          const float v = 0.1f + 0.6f * (static_cast<float>(x + y) / static_cast<float>(2 * kSize));
          t.writePixel(PixelCoord{lx, ly}, {v, v, v, 1.0f});
        }
      }
    }
  }
  Tile& speckTile = tiles.getOrCreate(tileCoordAt(PixelCoord{kSpeckX, kSpeckY}));
  speckTile.writePixel(tileLocalOffset(PixelCoord{kSpeckX, kSpeckY}), {1.0f, 0.0f, 1.0f, 1.0f});
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

OpenDocument makeDustScratchesDocument(const char* title) {
  OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, title);
  *od.document.layers[0].rgbTiles = dustFixture();
  od.recordEdit("dust & scratches fixture", EditKind::Content);
  return od;
}

}  // namespace

bool runDustScratchesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const PixelRect kCanvasRect{0, 0, kSize, kSize};
  const DustScratchesParams kMedianOnly{2, 0.0f};

  std::printf("  -- A. the fixture's speck really exceeds the threshold used against it --\n");
  float speckDiff = 0.0f;
  {
    TileStore median;
    check(medianTiles(dustFixture(), kCanvasRect, MedianParams{2}, &median),
          "fixture: the reference median() call succeeds");
    const std::array<float, 4> src = readAt(dustFixture(), kSpeckX, kSpeckY);
    const std::array<float, 4> med = readAt(median, kSpeckX, kSpeckY);
    speckDiff = dustScratchesDiff(src, med);
    check(speckDiff > 0.2f,
          "fixture: the speck's shaper-domain difference from its own median is large, not an "
          "accidental near-zero this gate would swallow at any reasonable threshold");
  }

  std::printf("  -- B. radius 0 is the exact identity --\n");
  {
    TileStore out;
    check(dustScratchesTiles(dustFixture(), kCanvasRect, DustScratchesParams{0, 0.0f}, &out) &&
              tilesExactlyEqual(out, dustFixture()),
          "radius 0: bit-exact copy of the source, speck included");
  }

  std::printf("  -- C. a threshold below the speck's diff removes it; distant texels are untouched --\n");
  {
    TileStore out;
    check(dustScratchesTiles(dustFixture(), kCanvasRect, kMedianOnly, &out),
          "threshold 0.0 <  speck diff: the call succeeds");
    TileStore median;
    medianTiles(dustFixture(), kCanvasRect, MedianParams{2}, &median);
    check(readAt(out, kSpeckX, kSpeckY) == readAt(median, kSpeckX, kSpeckY),
          "the speck's texel became exactly its median value");
    check(readAt(out, kSpeckX, kSpeckY) != readAt(dustFixture(), kSpeckX, kSpeckY),
          "the speck's texel is no longer the source value -- the gate fired, not a no-op");
    // radius 2's window is 5x5; a texel more than 2+2 = 4 away on either axis
    // cannot be inside the speck's window at all (ops/Filters.hpp section
    // 8's own apron), so it is bit-identical to the source regardless of the
    // gate.
    bool distantUntouched = true;
    for (const auto [dx, dy] : {std::pair{10, 0}, std::pair{-10, 0}, std::pair{0, 10},
                                std::pair{0, -10}, std::pair{20, 20}}) {
      if (readAt(out, kSpeckX + dx, kSpeckY + dy) !=
          readAt(dustFixture(), kSpeckX + dx, kSpeckY + dy))
        distantUntouched = false;
    }
    check(distantUntouched,
          "texels outside the speck's median window are bit-identical to the source");
  }

  std::printf("  -- D. a threshold above every possible difference is the exact identity --\n");
  {
    TileStore out;
    // The shaper domain is bounded for any finite input this fixture can
    // produce (ops/Filters.hpp's own clamp to kFilterMaxLinear upstream of
    // this filter), so a threshold two orders of magnitude past the fixture's
    // own extremes never opens the gate anywhere.
    const DustScratchesParams huge{2, 100.0f};
    check(dustScratchesTiles(dustFixture(), kCanvasRect, huge, &out) &&
              tilesExactlyEqual(out, dustFixture()),
          "threshold 100.0: bit-exact copy of the source, speck included");
  }

  std::printf("  -- E. threshold 0 equals medianTiles() on every texel that differs at all --\n");
  {
    TileStore out;
    TileStore median;
    dustScratchesTiles(dustFixture(), kCanvasRect, kMedianOnly, &out);
    medianTiles(dustFixture(), kCanvasRect, MedianParams{2}, &median);
    bool agrees = true;
    int32_t differingTexelsChecked = 0;
    for (int32_t y = 0; y < kSize; ++y) {
      for (int32_t x = 0; x < kSize; ++x) {
        const std::array<float, 4> src = readAt(dustFixture(), x, y);
        const std::array<float, 4> med = readAt(median, x, y);
        // The gate opens (`worst > 0`) exactly where dust & scratches and a
        // plain median must agree; where it does not, dust & scratches keeps
        // the SOURCE bit-exactly, which a plain median is not obliged to
        // reproduce bit-for-bit (its own un-premultiply/re-premultiply round
        // trip on an already-uniform window need not be a no-op in the
        // low bits) -- so the comparison is scoped to where the brief's own
        // claim actually applies.
        if (dustScratchesDiff(src, med) > 0.0f) {
          ++differingTexelsChecked;
          if (readAt(out, x, y) != med) agrees = false;
        }
      }
    }
    check(differingTexelsChecked > 0,
          "fixture: at least one texel actually differs from its own median (else E proves "
          "nothing)");
    check(agrees, "every texel whose median differs from it at all took exactly that median");
  }

  std::printf("  -- F. the encoder replays bit-identical to the applier --\n");
  {
    OpenDocument applier = makeDustScratchesDocument("dust encoder a");
    OpenDocument viaCommand = makeDustScratchesDocument("dust encoder b");
    const DustScratchesParams p{2, 0.05f};
    applyDustScratches(applier, p);
    const CommandResult r = applyCommand(viaCommand, dustScratchesCommand(p));
    check(r.ok && tilesExactlyEqual(*viaCommand.document.layers[0].rgbTiles,
                                    *applier.document.layers[0].rgbTiles),
          "dustScratchesCommand() replays bit-identical to applyDustScratches()");
  }

  std::printf("  -- G. invalid parameters are refused by name --\n");
  {
    check(!dustScratchesParamsValid(DustScratchesParams{-1, 0.0f}),
          "engine: a negative radius is refused");
    check(!dustScratchesParamsValid(DustScratchesParams{2, -0.1f}),
          "engine: a negative threshold is refused");

    JsonValue zeroRadius = JsonValue::object();
    zeroRadius.set("radius", JsonValue::number(0));
    OpenDocument refusalDoc = makeDustScratchesDocument("dust refusal");
    const CommandResult r =
        applyCommand(refusalDoc, Command{"filter_dust_scratches", std::move(zeroRadius)});
    check(!r.ok && r.status.find("radius") != std::string::npos,
          "command: radius 0 is refused by name, not silently treated as an identity request");
  }

  std::printf("  -- H. a selection bounds the filter; texels outside it are untouched --\n");
  {
    OpenDocument od = makeDustScratchesDocument("dust selection");
    od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);
    const TileStore before = *od.document.layers[0].rgbTiles;
    const FilterOpResult r = applyDustScratches(od, DustScratchesParams{2, 0.0f});
    check(r.refusal == PixelOpRefusal::None,
          "selection: a small selection on a filled layer is not refused");
    bool outsideUntouched = true;
    for (int32_t y = 100; y < 104 && outsideUntouched; ++y)
      for (int32_t x = 100; x < 104; ++x)
        if (readAt(*od.document.layers[0].rgbTiles, x, y) != readAt(before, x, y))
          outsideUntouched = false;
    check(outsideUntouched, "texels outside the selection are bit-identical to the source");
  }

  std::printf("[selftest] dustScratches %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
