#include "app/selftest/Support.hpp"

#include <cmath>
#include <set>

#include "color/Space.hpp"
#include "core/Half.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "ops/FloodFill.hpp"

namespace np {
namespace {

// Writes a STRAIGHT linear colour into the store the way a real writer does --
// premultiplied on the way in (io/ImageIO.cpp's `rgb *= a`), so the fixtures
// hold what an opened document holds rather than what is convenient to assert
// against. Getting this wrong would make every alpha claim below vacuous.
void putStraight(TileStore& tiles, int32_t x, int32_t y, const std::array<float, 4>& straight) {
  const float a = straight[3];
  tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
      .writePixel(tileLocalOffset(PixelCoord{x, y}),
                  {straight[0] * a, straight[1] * a, straight[2] * a, a});
}

size_t countSelected(const Selection& sel, int32_t width, int32_t height) {
  size_t n = 0;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      if (selectionCoverageAt(&sel, PixelCoord{x, y}) > 0.0f) ++n;
    }
  }
  return n;
}

// The fixture §3 and §4 both lean on: one row of greys whose *display-encoded*
// distance from the seed at x = 0 rises in equal steps of `tolerance / 16.5`.
//
// The odd divisor is deliberate and is the second attempt. With `/16` the
// sixteenth texel lands exactly on the tolerance boundary, where half-float
// rounding of the stored value decides inclusion by a few parts in a million --
// so the hard-edge and soft-edge answers disagreed on that one texel and the
// "the ramp weights the boundary, it never moves it" assertion below failed for
// a reason that had nothing to do with the claim. A non-integer divisor puts
// every sample half a step clear of the boundary.
TileStore encodedRampRow(int32_t width, int32_t height) {
  TileStore src;
  const float step = kFloodDefaultTolerance / 16.5f;
  for (int32_t x = 0; x < width; ++x) {
    const float v = srgbDecode(static_cast<float>(x) * step);
    for (int32_t y = 0; y < height; ++y) putStraight(src, x, y, {v, v, v, 1.0f});
  }
  return src;
}

}  // namespace

// ops/FloodFill (PLAN.md "Phase 6" paint bucket + "Phase 7" magic wand; PRD
// D25, E2, E3, D26). Pure CPU, no PaintSim and no GPU, like every other
// selection section.
//
// The section is organised around the four decisions that are expensive to get
// wrong and cheap to get wrong silently:
//
//   1. The tolerance is measured on DISPLAY-ENCODED values, not linear ones. A
//      linear tolerance produces a wand that works on the subject and floods
//      the shadows, with no wrong pixel to point at -- only a tool that feels
//      unpredictable. So the 18x asymmetry is measured here rather than
//      described in a comment.
//   2. The coverage is ANTIALIASED (PRD E2, P0) and the ramp's width is derived
//      from two real quantisations. The derivation is re-run in the binary.
//   3. The ramp weights the boundary and never MOVES it -- the same texels are
//      reached with antialiasing on and off.
//   4. The wand and the bucket share one predicate. The assertion for that is
//      that filling through the wand's own selection changes exactly the texels
//      the wand selected: if a second similarity test ever grows inside the
//      bucket, those two sets stop agreeing.
bool runFloodFillTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto grey = [](float v) { return std::array<float, 4>{v, v, v, 1.0f}; };

  const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
  const std::array<float, 4> white{1.0f, 1.0f, 1.0f, 1.0f};
  const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};

  // --- 1. The domain decision: a tolerance is NOT a linear distance --------
  {
    // Bisect for the linear value at which the default tolerance is exactly
    // reached, from a black seed going up and from a white seed going down.
    // Bisection rather than two hard-coded numbers so the measurement is of the
    // shipped function, not of a comment that used to describe it.
    auto edgeFrom = [&](float seed, float far) {
      float in = seed, out = far;
      for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (in + out);
        if (floodFillDistanceBetween(grey(seed), grey(mid)) < kFloodDefaultTolerance) {
          in = mid;
        } else {
          out = mid;
        }
      }
      return in;
    };
    const float blackUp = edgeFrom(0.0f, 1.0f);          // ~0.014444
    const float whiteDown = 1.0f - edgeFrom(1.0f, 0.0f);  // ~0.262090
    std::printf("  [selftest] flood: tolerance %.6f reaches %.6f of linear light above BLACK "
                "but %.6f below WHITE -- %.2fx\n",
                static_cast<double>(kFloodDefaultTolerance), static_cast<double>(blackUp),
                static_cast<double>(whiteDown),
                static_cast<double>(whiteDown / blackUp));

    check(whiteDown / blackUp > 15.0f,
          "flood: the same tolerance spans 18x more LINEAR light at white than at black -- "
          "which is why a tolerance measured in linear light cannot mean one thing");
    // The positive half of the same claim: uniform in the domain it is defined
    // on. Both edges sit at the tolerance to within the bisection's resolution.
    check(near(floodFillDistanceBetween(grey(0.0f), grey(blackUp)), kFloodDefaultTolerance,
               1e-4f) &&
              near(floodFillDistanceBetween(grey(1.0f), grey(1.0f - whiteDown)),
                   kFloodDefaultTolerance, 1e-4f),
          "flood: while in the DISPLAY-ENCODED domain both edges sit at exactly the tolerance "
          "-- the number means one thing, and it means it where the user is looking");
    // Stated the other way round, in the units a user types.
    std::printf("  [selftest] flood: a LINEAR tolerance of 0.05 would be %.1f sRGB code values "
                "wide at black and %.1f at white\n",
                static_cast<double>(255.0f * srgbEncode(0.05f)),
                static_cast<double>(255.0f * (1.0f - srgbEncode(0.95f))));
    check(kFloodDefaultTolerance == 32.0f / 255.0f,
          "flood: and the default IS Photoshop's 32/255, quoted rather than invented, because "
          "the whole argument is that the number must mean what users already think");
  }

  // --- 2. The predicate itself --------------------------------------------
  {
    check(floodFillDistanceBetween(black, black) == 0.0f &&
              floodFillDistanceBetween(white, white) == 0.0f &&
              floodFillDistanceBetween(clear, clear) == 0.0f,
          "flood: a colour is at distance zero from itself, so the clicked texel is always "
          "fully selected whatever the tolerance says");
    check(floodFillDistanceBetween(black, white) == floodFillDistanceBetween(white, black),
          "flood: the metric is symmetric -- seeding either end of an edge measures the same "
          "difference");

    // Alpha is in the distance, and it has to be: un-premultiplying a
    // transparent texel gives {0,0,0,0}, which is RGB-identical to opaque
    // black. A wand that dropped alpha would treat blank canvas and a black
    // shape as one colour.
    check(floodFillDistanceBetween(clear, black) >= 1.0f,
          "flood: transparent and opaque BLACK are maximally different -- they are identical "
          "on RGB alone once un-premultiplied, so alpha must be part of the distance");
    check(floodFillDistanceBetween(grey(0.5f), {0.5f * 0.5f, 0.5f * 0.5f, 0.5f * 0.5f, 0.5f}) ==
              0.5f,
          "flood: and it is compared LINEARLY, not through a transfer function -- alpha is "
          "opacity, not light, the same policy ops/PointOps states");

    // The un-premultiply guard, exercised on a texel a file could contain but
    // no writer here produces: colour under zero alpha is unrecoverable, so it
    // must not steer the wand.
    check(floodFillDistanceBetween(clear, {0.3f, 0.4f, 0.5f, 0.0f}) == 0.0f,
          "flood: RGB stored under ZERO alpha does not affect the distance -- core/Premultiply's "
          "guard defines it away, so a corrupt file cannot make the wand see a colour that is "
          "not there");

    // Chebyshev, not Euclidean: three channels moving together are not further
    // apart than one channel moving alone by the same amount.
    const float v = srgbDecode(0.05f);
    check(near(floodFillDistanceBetween(black, {v, v, v, 1.0f}), 0.05f, 1e-4f) &&
              near(floodFillDistanceBetween(black, {v, 0.0f, 0.0f, 1.0f}), 0.05f, 1e-4f),
          "flood: distance is the MAX over channels, not the root-sum-square -- so the "
          "tolerance means 'every channel is within it' regardless of which way the colour "
          "moved");
  }

  // --- 3. PRD E2: antialiased coverage, and where the ramp width comes from -
  {
    // The derivation, re-run: the largest gap, in the encoded metric, between
    // two ADJACENT representable halfs in [0,1]. ops/FloodFill.hpp's
    // kFloodEdgeBandFloor is 255x this, so that the uint8 coverage store's 256
    // levels are all reachable.
    float worstStep = 0.0f;
    float previous = -1.0f;
    for (uint32_t bits = 0; bits < 65536u; ++bits) {
      const float v = halfToFloat(static_cast<uint16_t>(bits));
      if (!std::isfinite(v) || v < 0.0f || v > 1.0f) continue;
      if (previous >= 0.0f && v > previous) {
        worstStep = std::max(worstStep, srgbEncode(v) - srgbEncode(previous));
      }
      // The half bit patterns for [0,1) ascend monotonically, and 1.0 follows;
      // negative zero is the only value that could arrive out of order and it
      // compares equal to the +0.0 already seen.
      previous = std::max(previous, v);
    }
    std::printf("  [selftest] flood: worst encoded step between adjacent halfs in [0,1] = "
                "%.9f; 255x = %.7f (kFloodEdgeBandFloor = %.7f)\n",
                static_cast<double>(worstStep), static_cast<double>(255.0f * worstStep),
                static_cast<double>(kFloodEdgeBandFloor));
    // 1e-4, not the 1e-5 this first asserted: the step is a difference of two
    // srgbEncode() results near 1.0, where one float ulp is 6e-8, and Apple's
    // libm and glibc round powf() differently in the last ulp -- measured,
    // 255 x 6e-8 = 1.5e-5 between the two, which is exactly the gap that
    // failed on Linux. The constant was derived on macOS; the tolerance now
    // admits a few ulps of libm on either side, which changes nothing about
    // what it asserts (a band no narrower than the coarsest step, to within
    // the arithmetic that produced it).
    check(near(kFloodEdgeBandFloor, 255.0f * worstStep, 1e-4f),
          "flood: the default edge band IS 255 x the coarsest source step, re-measured here "
          "rather than trusted -- a band any narrower cannot fill the uint8 store's 256 levels");

    // And that the criterion buys what it claims, and is not vacuous: the
    // derived band reaches (essentially) all 256 levels while a tenth of it
    // reaches a fraction.
    auto levelsAt = [&](float seed, float band) {
      FloodFillParams p;
      p.edgeBand = band;
      const FloodFillReference ref = floodFillReferenceFrom(grey(seed));
      std::set<uint8_t> seen;
      SelectionTile probe;
      for (uint32_t bits = 0; bits < 65536u; ++bits) {
        const float v = halfToFloat(static_cast<uint16_t>(bits));
        if (!std::isfinite(v) || v < 0.0f || v > 1.0f) continue;
        const float cov = floodFillCoverage(floodFillDistance(ref, grey(v)), p);
        if (cov <= 0.0f) continue;
        probe.writeCoverage(PixelCoord{0, 0}, cov);
        seen.insert(static_cast<uint8_t>(probe.coverageAt(PixelCoord{0, 0}) * 255.0f + 0.5f));
      }
      return seen.size();
    };
    const size_t wide = levelsAt(0.5f, kFloodDefaultEdgeBand);
    const size_t narrow = levelsAt(0.5f, kFloodDefaultEdgeBand * 0.1f);
    std::printf("  [selftest] flood: distinct uint8 coverage levels at a 0.5 seed -- derived "
                "band %zu, one tenth of it %zu\n",
                wide, narrow);
    check(wide >= 250 && narrow <= 120,
          "flood: the derived band yields ~256 distinct coverage levels and a tenth of it "
          "yields a fraction -- so the floor is a real constraint, not a number that would "
          "have passed whatever it was");

    // The ramp on a real fixture. Six texels fully selected, eleven partial,
    // the rest excluded; monotone; the seed exactly 1.0.
    const int32_t W = 24, H = 4;
    const TileStore ramp = encodedRampRow(W, H);
    const Selection soft = floodFillSelection(ramp, PixelCoord{0, 0}, W, H, FloodFillParams{});
    int full = 0, partial = 0, excluded = 0;
    bool monotone = true;
    float last = 2.0f;
    for (int32_t x = 0; x < W; ++x) {
      const float c = selectionCoverageAt(&soft, PixelCoord{x, 0});
      if (c >= 1.0f) {
        ++full;
      } else if (c > 0.0f) {
        ++partial;
      } else {
        ++excluded;
      }
      if (c > last) monotone = false;
      last = c;
    }
    std::printf("  [selftest] flood: encoded ramp -> %d fully selected, %d partial, %d "
                "excluded (edge coverages %.3f .. %.3f)\n",
                full, partial, excluded,
                static_cast<double>(selectionCoverageAt(&soft, PixelCoord{6, 0})),
                static_cast<double>(selectionCoverageAt(&soft, PixelCoord{16, 0})));
    check(partial >= 8 && full >= 4 && excluded >= 4,
          "flood: a ramp of colours crossing the tolerance gives PARTIAL coverage over a band "
          "of texels -- this is the line between an antialiased wand and a bitmask (PRD E2)");
    check(monotone,
          "flood: and coverage falls monotonically as the colour gets further from the seed, "
          "so the edge is a ramp and not a pattern");
    check(selectionCoverageAt(&soft, PixelCoord{0, 0}) == 1.0f,
          "flood: the clicked texel is at exactly 1.0 -- a wand that partially selected what "
          "was clicked would be indefensible whatever the arithmetic said");

    // "Whatever the arithmetic said" is the load-bearing half of that, and the
    // default parameters do not test it: at tolerance 32/255 the ramp already
    // saturates well before distance zero. These are the two settings where the
    // formula would hand back something less than 1 for the seed, and both are
    // reachable from a UI slider.
    {
      FloodFillParams exact;
      exact.tolerance = 0.0f;
      FloodFillParams overwide;
      overwide.edgeBand = 1.0f;  // wider than the whole accepted band
      check(floodFillCoverage(0.0f, exact) == 1.0f &&
                floodFillCoverage(0.0f, overwide) == 1.0f,
            "flood: at tolerance 0, and at an edgeBand wider than the tolerance, the seed is "
            "STILL exactly 1.0 -- the two parameter settings where the ramp formula alone "
            "would partially select the texel the user clicked");
      const Selection only = floodFillSelection(ramp, PixelCoord{0, 0}, W, H, exact);
      check(countSelected(only, W, H) == static_cast<size_t>(H) &&
                selectionCoverageAt(&only, PixelCoord{0, 0}) == 1.0f,
            "flood: and tolerance 0 means 'exactly this colour' -- it selects the seed's own "
            "column and stops, rather than selecting nothing at all");
    }

    // Turning antialiasing off must change the WEIGHTS and not the REGION.
    FloodFillParams hard;
    hard.edgeBand = 0.0f;
    const Selection crisp = floodFillSelection(ramp, PixelCoord{0, 0}, W, H, hard);
    bool sameReach = true;
    bool anyPartial = false;
    for (int32_t y = 0; y < H && sameReach; ++y) {
      for (int32_t x = 0; x < W; ++x) {
        const float a = selectionCoverageAt(&soft, PixelCoord{x, y});
        const float b = selectionCoverageAt(&crisp, PixelCoord{x, y});
        if ((a > 0.0f) != (b > 0.0f)) {
          sameReach = false;
          break;
        }
        if (b > 0.0f && b < 1.0f) anyPartial = true;
      }
    }
    check(sameReach && !anyPartial,
          "flood: with edgeBand 0 the SAME texels are selected, every one of them fully -- the "
          "ramp weights the boundary and never moves it, so an antialias toggle cannot "
          "silently change which region gets filled");
  }

  // --- 4. Traversal: contiguous is a flood fill, global is not -------------
  {
    // Two identical black squares on white, far apart.
    const int32_t W = 40, H = 20;
    TileStore src;
    for (int32_t y = 0; y < H; ++y) {
      for (int32_t x = 0; x < W; ++x) putStraight(src, x, y, white);
    }
    for (int32_t y = 2; y < 8; ++y) {
      for (int32_t x = 2; x < 8; ++x) putStraight(src, x, y, black);
      for (int32_t x = 30; x < 36; ++x) putStraight(src, x, y, black);
    }

    const Selection near1 = floodFillSelection(src, PixelCoord{4, 4}, W, H, FloodFillParams{});
    FloodFillParams global;
    global.reach = FloodFillReach::Global;
    const Selection all = floodFillSelection(src, PixelCoord{4, 4}, W, H, global);

    check(countSelected(near1, W, H) == 36 && countSelected(all, W, H) == 72,
          "flood: Contiguous finds one 6x6 square and Global finds both -- PRD D25's two "
          "modes, sharing a predicate and nothing else");
    check(selectionCoverageAt(&near1, PixelCoord{32, 4}) == 0.0f &&
              selectionCoverageAt(&all, PixelCoord{32, 4}) == 1.0f,
          "flood: the far square is unreachable by traversal and matched by predicate -- which "
          "is exactly the difference between the two, stated on one texel");

    // 4-connected, not 8. A diagonal touch must not be crossed.
    {
      TileStore diag;
      for (int32_t y = 0; y < 6; ++y) {
        for (int32_t x = 0; x < 6; ++x) putStraight(diag, x, y, white);
      }
      putStraight(diag, 1, 1, black);
      putStraight(diag, 2, 2, black);
      const Selection s = floodFillSelection(diag, PixelCoord{1, 1}, 6, 6, FloodFillParams{});
      check(countSelected(s, 6, 6) == 1,
            "flood: a diagonal touch is NOT crossed -- an 8-connected fill leaks through every "
            "hairline and hand-drawn outline, and the leak is invisible until the background "
            "floods");
    }

    // The U: the fill has to go down, along, and back up. This is the case a
    // span walker gets wrong if it clips child rows to the parent's span.
    {
      const int32_t U = 11;
      TileStore u;
      for (int32_t y = 0; y < U; ++y) {
        for (int32_t x = 0; x < U; ++x) putStraight(u, x, y, white);
      }
      for (int32_t y = 0; y < U; ++y) {
        putStraight(u, 0, y, black);
        putStraight(u, U - 1, y, black);
      }
      for (int32_t x = 0; x < U; ++x) putStraight(u, x, U - 1, black);
      const Selection s = floodFillSelection(u, PixelCoord{0, 0}, U, U, FloodFillParams{});
      check(countSelected(s, U, U) == 31 &&
                selectionCoverageAt(&s, PixelCoord{U - 1, 0}) == 1.0f,
            "flood: a U-shaped region is filled to its far tip -- the span walk expands each "
            "row to its own extent, not to the extent of the span that spawned it");
    }

    // Scanline, not recursion. 160 000 texels in one region: a per-texel
    // recursive fill dies on the stack here, and a per-texel queue holds one
    // entry per texel where this holds one per run.
    {
      const int32_t B = 400;
      TileStore big;
      for (int32_t y = 0; y < B; ++y) {
        for (int32_t x = 0; x < B; ++x) putStraight(big, x, y, black);
      }
      const Selection s = floodFillSelection(big, PixelCoord{200, 200}, B, B, FloodFillParams{});
      check(countSelected(s, B, B) == static_cast<size_t>(B) * B,
            "flood: a 400x400 solid region fills completely without recursing 160 000 frames "
            "deep -- the traversal is span-based, and this is the assertion that fails if it "
            "is ever rewritten per texel");
    }

    // Paging: the fill crosses tile seams continuously and allocates exactly
    // the tiles it needs.
    {
      const int32_t Wl = 300, Hl = 8;
      TileStore wide;
      for (int32_t y = 0; y < Hl; ++y) {
        for (int32_t x = 0; x < Wl; ++x) putStraight(wide, x, y, black);
      }
      const Selection s = floodFillSelection(wide, PixelCoord{0, 0}, Wl, Hl, FloodFillParams{});
      check(countSelected(s, Wl, Hl) == static_cast<size_t>(Wl) * Hl &&
                selectionCoverageAt(&s, PixelCoord{127, 0}) == 1.0f &&
                selectionCoverageAt(&s, PixelCoord{128, 0}) == 1.0f,
            "flood: the fill reads and writes continuously across a tile seam -- it pages "
            "through the store rather than materialising the document");
      check(s.tiles.occupiedTileCount() == 3,
            "flood: and allocates exactly the three tiles a 300-wide row spans, not one per "
            "document tile");
    }

    // The document bound, inside an edge tile. A 100-wide document's tile 0
    // physically holds 128 columns; the last 28 are not part of the picture.
    {
      TileStore empty;
      const Selection s = floodFillSelection(empty, PixelCoord{0, 0}, 100, 100, FloodFillParams{});
      check(countSelected(s, 128, 128) == 10000,
            "flood: a fill on a blank layer floods the whole document through tiles that do "
            "not exist -- an absent tile reads as transparent, which is what it IS");
      check(selectionCoverageAt(&s, PixelCoord{99, 99}) == 1.0f &&
                selectionCoverageAt(&s, PixelCoord{100, 99}) == 0.0f &&
                selectionCoverageAt(&s, PixelCoord{99, 100}) == 0.0f,
            "flood: and stops at the DOCUMENT edge, not the tile edge -- the 28 columns past "
            "column 99 exist in tile 0 and are not part of the picture");

      // The same clip, on the other reach. Global builds its own tile grid
      // rather than walking spans, so it needs its own copy of the rule and
      // therefore its own assertion -- a perturbation that removed only the
      // predicate pass's row clip left every span-based check above passing.
      FloodFillParams g;
      g.reach = FloodFillReach::Global;
      const Selection gs = floodFillSelection(empty, PixelCoord{0, 0}, 100, 100, g);
      check(countSelected(gs, 128, 128) == 10000 &&
                selectionCoverageAt(&gs, PixelCoord{100, 99}) == 0.0f &&
                selectionCoverageAt(&gs, PixelCoord{99, 100}) == 0.0f,
            "flood: fill-all-similar clips to the DOCUMENT as well -- on a size that is not a "
            "whole number of tiles the margin inside the edge tiles stays unselected, and the "
            "two reaches must not disagree about where the picture stops");
    }

    // Global's tile set is a decision, not an optimisation: a transparent seed
    // legitimately matches every unallocated texel, so the answer is dense.
    {
      TileStore sparse;
      putStraight(sparse, 400, 400, black);
      FloodFillParams g;
      g.reach = FloodFillReach::Global;
      const Selection onBlack = floodFillSelection(sparse, PixelCoord{400, 400}, 512, 512, g);
      const Selection onEmpty = floodFillSelection(sparse, PixelCoord{0, 0}, 512, 512, g);
      check(onBlack.tiles.occupiedTileCount() == 1,
            "flood: fill-all-similar on an opaque seed touches only the tiles that exist -- a "
            "mostly-empty layer costs what the layer costs");
      check(onEmpty.tiles.occupiedTileCount() == 16,
            "flood: while a TRANSPARENT seed matches every unallocated texel too, so the answer "
            "really is every tile in the document -- dense by definition, like invert, and "
            "charged rather than truncated to 'the tiles that happen to exist'");
    }

    // Degenerate seeds give an ENGAGED, EMPTY selection -- never the
    // default-constructed one a caller could mistake for "no restriction".
    {
      const Selection outside =
          floodFillSelection(src, PixelCoord{-1, 0}, W, H, FloodFillParams{});
      const Selection nodoc = floodFillSelection(src, PixelCoord{0, 0}, 0, 0, FloodFillParams{});
      check(outside.tiles.occupiedTileCount() == 0 && selectionSelectsNothing(outside) &&
                nodoc.tiles.occupiedTileCount() == 0,
            "flood: a seed outside the document selects NOTHING rather than everything -- the "
            "hazard core/SelectionMask names, arriving here through a click on the canvas "
            "margin");
    }
  }

  // --- 5. The bucket: one predicate, one write path (PRD D25, D26) ---------
  {
    const int32_t W = 40, H = 20;
    TileStore src;
    for (int32_t y = 0; y < H; ++y) {
      for (int32_t x = 0; x < W; ++x) putStraight(src, x, y, white);
    }
    for (int32_t y = 2; y < 8; ++y) {
      for (int32_t x = 2; x < 8; ++x) putStraight(src, x, y, black);
    }
    const Selection wand = floodFillSelection(src, PixelCoord{4, 4}, W, H, FloodFillParams{});

    // THE unification assertion. If a second similarity test ever grows inside
    // the bucket, the set of texels it writes stops matching the set the wand
    // found, and this is what says so.
    {
      TileStore filled = src;
      const size_t changed = fillThroughSelection(filled, wand, {1.0f, 0.0f, 0.0f, 1.0f});
      bool exact = true;
      for (int32_t y = 0; y < H && exact; ++y) {
        for (int32_t x = 0; x < W; ++x) {
          const TileCoord tc = tileCoordAt(PixelCoord{x, y});
          const PixelCoord lc = tileLocalOffset(PixelCoord{x, y});
          const bool moved = filled.find(tc)->readPixel(lc) != src.find(tc)->readPixel(lc);
          if (moved != (selectionCoverageAt(&wand, PixelCoord{x, y}) > 0.0f)) {
            exact = false;
            break;
          }
        }
      }
      check(changed == 36 && exact,
            "bucket: filling through the wand's OWN selection moves exactly the texels the "
            "wand selected -- the bucket has no second opinion about what a similar colour is");

      const std::array<float, 4> inside =
          filled.find(TileCoord{0, 0})->readPixel(PixelCoord{4, 4});
      const std::array<float, 4> outside =
          filled.find(TileCoord{0, 0})->readPixel(PixelCoord{20, 10});
      check(inside[0] == 1.0f && inside[1] == 0.0f && inside[3] == 1.0f,
            "bucket: an opaque colour at full coverage lands EXACTLY, not approximately -- a "
            "source-over that did not reduce to a replace at coverage 1 would tint every fill");
      check(outside[0] == 1.0f && outside[1] == 1.0f && outside[3] == 1.0f,
            "bucket: and one texel outside the selection is untouched");

      const size_t again = fillThroughSelection(filled, wand, {1.0f, 0.0f, 0.0f, 1.0f});
      check(again == 0,
            "bucket: filling the same colour again reports ZERO changed -- 'nothing to do' and "
            "'nothing selected' are different answers a UI has to tell apart");
    }

    // Coverage-weighted, premultiply-correct. The claim that fails silently:
    // if RGB did not fall with alpha, every feathered fill would fringe.
    {
      TileStore blank;
      const Selection half = selectRectangle(0.0f, 0.0f, 1.5f, 1.0f);
      const size_t changed = fillThroughSelection(blank, half, {1.0f, 0.0f, 0.0f, 1.0f});
      const Tile* t = blank.find(TileCoord{0, 0});
      const std::array<float, 4> edge =
          t != nullptr ? t->readPixel(PixelCoord{1, 0}) : std::array<float, 4>{-1, -1, -1, -1};
      std::printf("  [selftest] bucket: 0.5-covered texel filled to rgba(%.4f, %.4f, %.4f, "
                  "%.4f)\n",
                  static_cast<double>(edge[0]), static_cast<double>(edge[1]),
                  static_cast<double>(edge[2]), static_cast<double>(edge[3]));
      check(changed == 2 && near(edge[3], 0.5f, 2.0f / 255.0f),
            "bucket: a half-covered texel receives half the alpha -- the fill is WEIGHTED by "
            "coverage, not thresholded by it (PRD E2 reaching the pixels)");
      check(near(edge[0], edge[3], 1e-3f) && edge[1] == 0.0f,
            "bucket: and its RED tracks its ALPHA, because the store is premultiplied -- "
            "un-premultiplying still gives pure red, so a feathered fill has no fringe");
      check(blank.find(TileCoord{0, 0}) != nullptr,
            "bucket: the fill CREATED the tile it needed -- it walks the selection's tiles, "
            "not the store's, or a bucket on a blank layer would silently do nothing");
    }

    // A TRANSLUCENT colour over existing paint, at partial coverage. This is
    // the only fixture in which the source-over is not degenerate: with an
    // opaque colour `1 - a*w` collapses to `1 - w`, and with full coverage it
    // collapses to `1 - a`, so both of the checks above would pass an
    // implementation that had dropped either factor from the `keep` term.
    {
      TileStore over;
      for (int32_t x = 0; x < 2; ++x) putStraight(over, x, 0, white);
      const Selection half = selectRectangle(0.0f, 0.0f, 1.5f, 1.0f);
      fillThroughSelection(over, half, {1.0f, 0.0f, 0.0f, 0.5f});
      const Tile* t = over.find(TileCoord{0, 0});
      const std::array<float, 4> full = t->readPixel(PixelCoord{0, 0});
      const std::array<float, 4> part = t->readPixel(PixelCoord{1, 0});
      std::printf("  [selftest] bucket: 50%% red over white -- fully covered texel "
                  "rgba(%.4f, %.4f, %.4f, %.4f), half-covered rgba(%.4f, %.4f, %.4f, %.4f)\n",
                  static_cast<double>(full[0]), static_cast<double>(full[1]),
                  static_cast<double>(full[2]), static_cast<double>(full[3]),
                  static_cast<double>(part[0]), static_cast<double>(part[1]),
                  static_cast<double>(part[2]), static_cast<double>(part[3]));
      check(near(full[0], 1.0f, 2e-3f) && near(full[1], 0.5f, 2e-3f) &&
                near(full[3], 1.0f, 2e-3f),
            "bucket: a 50%-opaque red over opaque white gives pink and stays OPAQUE -- the "
            "fill is a source-over, not a replace, so filling a translucent colour does not "
            "punch the alpha down to the colour's own");
      // Half the coverage of half an alpha: the source contributes a quarter,
      // so green retains 0.749 rather than the 0.5 a `keep` missing the
      // coverage factor would leave.
      check(near(part[1], 0.749f, 3e-3f) && near(part[3], 1.0f, 2e-3f),
            "bucket: and at half coverage it contributes a QUARTER -- coverage and the "
            "colour's own alpha both scale the source, which is the one arithmetic a fixture "
            "with either factor at 1 cannot tell apart");
    }

    // Sparsity and copy-on-write, the same property clearThroughSelection is
    // held to: a fill through a small selection must not deep-copy a document.
    {
      TileStore store = src;
      store.getOrCreate(TileCoord{9, 9}).writePixel(PixelCoord{0, 0}, {1, 1, 1, 1});
      TileStore shared = store;
      check(shared.isTileShared(TileCoord{9, 9}), "bucket: (fixture) a copied store shares");
      fillThroughSelection(shared, wand, {0.0f, 1.0f, 0.0f, 1.0f});
      check(shared.isTileShared(TileCoord{9, 9}),
            "bucket: a fill through a small selection leaves the FAR tile shared -- the "
            "selection's tile set bounds the work, so filling does not cost the document");
      check(!shared.isTileShared(TileCoord{0, 0}) &&
                store.find(TileCoord{0, 0})->readPixel(PixelCoord{4, 4})[1] == 0.0f,
            "bucket: while the tile it wrote was unshared and the ORIGINAL still holds its "
            "own paint");
    }

    // Zero opacity writes nothing AND allocates nothing.
    {
      TileStore blank;
      const size_t changed =
          fillThroughSelection(blank, selectAll(64, 64), {1.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
      check(changed == 0 && blank.occupiedTileCount() == 0,
            "bucket: at zero opacity nothing is written and no tile is allocated -- a no-op "
            "must not cost 128 KiB");
    }

    // selectAll() is the spelling of "fill the layer". fillThroughSelection
    // takes a reference precisely so that the unbounded null-Selection request
    // cannot be made at all.
    {
      TileStore blank;
      const size_t changed =
          fillThroughSelection(blank, selectAll(64, 64), {0.0f, 0.0f, 1.0f, 1.0f});
      check(changed == 64 * 64,
            "bucket: 'fill the layer' is spelled selectAll(w, h) -- this function takes a "
            "Selection by REFERENCE so the unbounded 'no restriction' request, which would be "
            "an infinite plane of tiles on a write path, cannot be expressed");
    }

    // And the combining is core/SelectionOps' job, not this file's -- the wand
    // installs a selection like any other tool.
    {
      const Selection marquee = selectRectangle(0.0f, 0.0f, 5.0f, 5.0f);
      const Selection both = combineSelections(marquee, wand, SelectionCombine::Intersect);
      check(selectionCoverageAt(&both, PixelCoord{4, 4}) == 1.0f &&
                selectionCoverageAt(&both, PixelCoord{6, 6}) == 0.0f,
            "flood: the wand's output is an ordinary Selection and intersects with a marquee "
            "through the existing algebra -- no tool-specific combination path");
    }
  }

  std::printf("[selftest] floodfill %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
