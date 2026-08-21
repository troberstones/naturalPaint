#include "app/selftest/Support.hpp"

namespace np {

// core/Half (factored out of sim/PaintSim.cpp's private half<->float decoder
// so core/Tile's pixel storage can reuse it for the encode direction too)
// and core/TileStore (PLAN.md Phase 2 step 2). Both land in one test since
// TileStore's pixel round-trip depends on Half being correct underneath it
// -- a Half bug would otherwise show up as a confusing TileStore failure.
//
// Half coverage: zero, negative zero, subnormals, ordinary values and the
// highest/lowest finite half magnitudes, round-tripped through both
// directions. This is the "silently corrupts everything downstream" class
// of bug -- every tile pixel, and every readback in sim/PaintSim.cpp, goes
// through this code -- so it gets a permanent gate here, in addition to the
// full-range exhaustive cross-check against the hardware `_Float16` path
// done once during development (see core/Half.hpp's header comment).
//
// TileStore coverage, per PLAN.md step 2's own wording:
//  - "allocate on write": getOrCreate() makes a tile that find() then sees.
//  - "query without allocating": find() on an untouched coordinate returns
//    nullptr AND leaves occupiedTileCount() unchanged -- the naive-operator[]
//    bug PLAN.md's phrasing is calling out.
//  - "iterate occupied tiles": iteration visits exactly the allocated
//    tiles, no more, no less.
//  - A negative document coordinate (x = -1) round-trips through
//    tileCoordAt/tileLocalOffset and a write/read, exercising Tile.hpp's
//    floor semantics rather than only the positive-coordinate happy path.
bool runTileStoreTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Half: direct round-trips, both directions ---
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  check(floatToHalf(0.0f) == 0x0000u, "floatToHalf(0.0) == +0 half bit pattern");
  check(floatToHalf(-0.0f) == 0x8000u, "floatToHalf(-0.0) == -0 half bit pattern");
  check(halfToFloat(0x0000u) == 0.0f && !std::signbit(halfToFloat(0x0000u)),
        "halfToFloat(+0 bits) == +0.0");
  check(halfToFloat(0x8000u) == 0.0f && std::signbit(halfToFloat(0x8000u)),
        "halfToFloat(-0 bits) == -0.0 (sign preserved)");

  // Smallest half subnormal (2^-24) and a mid-range subnormal (2^-20,
  // exactly representable in half -- no rounding to muddy the check).
  check(nearf(halfToFloat(floatToHalf(5.960464478e-8f)), 5.960464478e-8f, 1e-10f),
        "half subnormal round-trip: 2^-24");
  check(nearf(halfToFloat(floatToHalf(9.5367431640625e-7f)), 9.5367431640625e-7f, 1e-12f),
        "half subnormal round-trip: 2^-20 (exact)");

  // Ordinary values, including a negative one and a non-power-of-two.
  for (float v : {1.0f, -1.0f, 0.5f, 100.25f, -3.14159f, 0.1f}) {
    char label[64];
    std::snprintf(label, sizeof label, "half round-trip near %.5f", static_cast<double>(v));
    // 2^-11 relative is half's worst-case rounding error (10-bit mantissa).
    check(nearf(halfToFloat(floatToHalf(v)), v, std::fabs(v) * 0.0005f + 1e-6f), label);
  }

  // Highest and lowest finite half magnitudes: 65504 is exactly
  // representable in both float and half, so this must be an exact
  // round-trip, not just "close".
  check(floatToHalf(65504.0f) == 0x7BFFu, "floatToHalf(65504) == max finite half bits");
  check(halfToFloat(0x7BFFu) == 65504.0f, "halfToFloat(max finite bits) == 65504.0 exactly");
  check(floatToHalf(-65504.0f) == 0xFBFFu, "floatToHalf(-65504) == lowest finite half bits");
  check(halfToFloat(0xFBFFu) == -65504.0f, "halfToFloat(lowest finite bits) == -65504.0 exactly");
  // A magnitude beyond half's range must overflow to infinity, not wrap or
  // saturate silently.
  check(std::isinf(halfToFloat(floatToHalf(1.0e6f))),
        "a magnitude beyond half's range overflows to infinity, not silent saturation");

  // --- TileStore: allocate on write / query without allocating / iterate ---
  {
    TileStore store;
    check(store.occupiedTileCount() == 0, "a fresh TileStore has no occupied tiles");

    const TileCoord origin{0, 0};
    check(store.find(origin) == nullptr,
          "find() on an untouched coordinate returns nullptr");
    check(store.occupiedTileCount() == 0,
          "find() on a miss allocates nothing -- occupied count still 0");

    const std::array<float, 4> pixel{0.25f, 0.5f, 0.75f, 1.0f};
    Tile& t = store.getOrCreate(origin);
    t.writePixel(PixelCoord{5, 7}, pixel);
    check(store.occupiedTileCount() == 1,
          "getOrCreate() on a miss allocates exactly one tile");

    const Tile* found = store.find(origin);
    check(found != nullptr, "find() sees the tile getOrCreate() just made");
    if (found) {
      const auto rt = found->readPixel(PixelCoord{5, 7});
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "write-then-read round-trips a pixel within half-float precision");
    }

    // getOrCreate() on an already-occupied coordinate must not allocate a
    // second tile, and must return the same one (still see the pixel).
    Tile& again = store.getOrCreate(origin);
    check(&again == found, "getOrCreate() on an occupied coordinate returns the same tile");
    check(store.occupiedTileCount() == 1,
          "getOrCreate() on a hit does not grow the occupied count");

    // A second, distinct tile.
    store.getOrCreate(TileCoord{3, -2}).writePixel(PixelCoord{0, 0}, {1.0f, 0.0f, 0.0f, 1.0f});
    check(store.occupiedTileCount() == 2, "a second write allocates a second tile");

    // Iteration visits exactly the allocated tiles, no more, no less.
    size_t seen = 0;
    bool sawOrigin = false, sawOther = false;
    for (const auto& [coord, tile] : store) {
      ++seen;
      (void)tile;
      if (coord == origin) sawOrigin = true;
      if (coord == TileCoord{3, -2}) sawOther = true;
    }
    check(seen == 2, "iteration visits exactly the 2 allocated tiles");
    check(sawOrigin && sawOther, "iteration visits both specific coordinates written");

    // A coordinate that was never written still isn't there.
    check(store.find(TileCoord{99, 99}) == nullptr,
          "an unrelated, never-written coordinate is still absent after other writes");
  }

  // --- Negative document coordinate: exercises Tile.hpp's floor semantics ---
  {
    TileStore store;
    const PixelCoord doc{-1, -1};
    const TileCoord coord = tileCoordAt(doc);
    const PixelCoord local = tileLocalOffset(doc);
    check(coord == (TileCoord{-1, -1}),
          "document pixel (-1,-1) falls in tile (-1,-1), not tile (0,0)");
    check(local == (PixelCoord{kTileSize - 1, kTileSize - 1}),
          "document pixel (-1,-1)'s local offset is the tile's bottom-right corner");

    const std::array<float, 4> pixel{0.1f, 0.2f, 0.3f, 0.4f};
    store.getOrCreate(coord).writePixel(local, pixel);
    const Tile* found = store.find(coord);
    check(found != nullptr, "negative-coordinate tile is findable after being written");
    if (found) {
      const auto rt = found->readPixel(local);
      check(nearf(rt[0], pixel[0], 0.001f) && nearf(rt[1], pixel[1], 0.001f) &&
                nearf(rt[2], pixel[2], 0.001f) && nearf(rt[3], pixel[3], 0.001f),
            "negative document coordinate round-trips a pixel correctly");
    }
    // Document pixel (0,0) is a different tile-local position entirely
    // (tile (0,0), offset (0,0)) -- confirms the floor split, not just a
    // single coordinate in isolation.
    check(tileCoordAt(PixelCoord{0, 0}) == (TileCoord{0, 0}),
          "document pixel (0,0) falls in tile (0,0) -- the floor boundary lands correctly");
  }

  std::printf("[selftest] tile store %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
