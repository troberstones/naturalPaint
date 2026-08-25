#include "app/selftest/Support.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "core/SelectionShapes.hpp"
#include "ops/FloodFill.hpp"

namespace np {
namespace {

// The store's own quantisation step. Every tolerance in this section is stated
// as a multiple of THIS rather than as a decimal chosen to make an assertion
// pass: a SelectionTile holds one uint8 per texel, so nothing here can be more
// accurate than 1/255 and no bound tighter than it is meaningful.
constexpr float kQuantum = 1.0f / 255.0f;

float coverageAt(const Selection& s, int32_t x, int32_t y) {
  return selectionCoverageAt(&s, PixelCoord{x, y});
}

// `v` as the store would hold it.
//
// Every "should be 0.5" assertion below compares against THIS rather than
// against 0.5 with a tolerance, and the difference is not pedantry: the store
// reads back `n * (1/255)`, which for 126 of the 256 levels is not the same
// float as `n / 255`, so a tolerance loose enough to cover that gap is looser
// than the thing being measured. Quantising the expectation instead lets the
// comparison be exact equality, and an exact comparison cannot be quietly
// widened later to make a regression pass.
float quantised(float v) {
  SelectionTile t;
  t.writeCoverage(PixelCoord{0, 0}, v);
  return t.coverageAt(PixelCoord{0, 0});
}

double totalCoverage(const Selection& s) {
  double sum = 0.0;
  for (const auto& [coord, tile] : s.tiles) {
    (void)coord;
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) sum += tile.coverageAt(PixelCoord{x, y});
    }
  }
  return sum;
}

// What a SET-based transform is forced to do before it can run: decide, per
// texel, in or out. Used to demonstrate the loss PRD E8 exists to prevent --
// not as an alternative implementation, but as the thing being rejected.
Selection thresholded(const Selection& s, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  Selection out;
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      if (coverageAt(s, x, y) < 0.5f) continue;
      const PixelCoord p{x, y};
      out.tiles.getOrCreate(tileCoordAt(p)).writeCoverage(tileLocalOffset(p), 1.0f);
    }
  }
  return out;
}

}  // namespace

// core/SelectionRefine -- PRD E8 (grow/shrink through a distance transform) and
// PRD E9 (colour range, luminance range). Headless and GPU-free, pure CPU.
//
// Two claims carry this section, and neither is arithmetic that a golden image
// could catch.
//
// PRD E8 is a requirement about a *mechanism*: grow and shrink must go through
// a distance transform "so the radius is a real number and antialiasing
// survives". Iterated dilation would satisfy every plausible smoke test -- the
// selection would get bigger by roughly the right amount -- while failing both
// clauses, so the assertions below are written to fail specifically for a
// dilation: a fractional radius must move the edge by a fractional amount, and
// an antialiased boundary texel must come back with its coverage intact rather
// than rounded to a bit. Section 1 puts the two side by side, running the same
// edge through the operator and through a thresholded copy of itself.
//
// PRD E9's hazard is different and is one `ops/FloodFill.hpp` names outright:
// two implementations of "similar colour" that disagree. Section 5 asserts that
// colour range fed the colour under a texel returns the identical selection to
// a Global flood fill seeded on that texel -- texel for texel, not
// approximately -- so a second tolerance metric growing in this file is a test
// failure rather than a user complaint.
bool runSelectionRefineTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- 1. Antialiasing survives, which is half of PRD E8 ------------------
  {
    // The two conversions the whole operator is built on must be inverses, or
    // "grow by zero" is not an identity and every radius is off by a constant.
    //
    // Checked over the store's OWN 256 levels rather than over `i / 255`, and
    // at the store's own resolution: the intermediate `c - 0.5` rounds, so 47
    // of the levels come back one float ULP away. That is a fact about float
    // subtraction, not about this operator, and the claim that matters is that
    // nothing survives to the stored value.
    int perturbed = 0;
    double worstUlp = 0.0;
    bool stable = true;
    for (int i = 0; i <= 255; ++i) {
      const float c = quantised(static_cast<float>(i) / 255.0f);
      const float rt =
          selectionCoverageFromSignedDistance(selectionSignedDistanceFromCoverage(c));
      if (rt != c) {
        ++perturbed;
        worstUlp = std::max(worstUlp, std::fabs(static_cast<double>(rt) - c));
      }
      if (quantised(rt) != c) stable = false;
    }
    std::printf("  [selftest] refine: coverage<->distance round trip moves %d of 256 stored "
                "levels by at most %.3e (one float ULP) and 0 after requantisation\n",
                perturbed, worstUlp);
    check(stable && worstUlp < 1e-7,
          "refine: coverage<->signed distance round-trips through the STORE exactly at all "
          "256 levels -- if these two drift, every radius is off by a constant nobody traces");

    // A rectangle whose left edge falls at 10.3 leaves texel 10 at 0.7 covered.
    const Selection aa = selectRectangle(10.3f, 10.0f, 30.0f, 30.0f);
    const Selection grown0 = growSelection(aa, 0.0f);
    float worst = 0.0f;
    for (int32_t y = 5; y < 35; ++y) {
      for (int32_t x = 5; x < 35; ++x) {
        worst = std::max(worst, std::fabs(coverageAt(aa, x, y) - coverageAt(grown0, x, y)));
      }
    }
    std::printf("  [selftest] refine: grow by 0 differs from the input by %.8f over 30x30 "
                "(uint8 step %.6f)\n",
                static_cast<double>(worst), static_cast<double>(kQuantum));
    check(worst == 0.0f,
          "refine: grow by 0 is a BIT-EXACT identity -- not special-cased, so this is the "
          "distance field reproducing its own input and the sharpest test here");

    // The claim stated as a contrast. A set-based transform must decide in or
    // out first, and that decision is where the coverage goes.
    const Selection soft = selectRectangle(10.7f, 10.0f, 30.0f, 30.0f);  // texel 10 at 0.3
    const Selection softGrown = growSelection(soft, 0.0f);
    const Selection binGrown = growSelection(thresholded(soft, 0, 0, 40, 40), 0.0f);
    std::printf("  [selftest] refine: an edge texel at %.6f survives as %.6f through the "
                "distance field and as %.6f through a thresholded copy\n",
                static_cast<double>(coverageAt(soft, 10, 20)),
                static_cast<double>(coverageAt(softGrown, 10, 20)),
                static_cast<double>(coverageAt(binGrown, 10, 20)));
    check(coverageAt(softGrown, 10, 20) == coverageAt(soft, 10, 20) &&
              coverageAt(binGrown, 10, 20) == 0.0f,
          "refine: a 0.3-covered edge texel survives the real operator and is DESTROYED by "
          "thresholding first -- PRD E8's 'antialiasing survives', as the two numbers");

    check(!near(coverageAt(soft, 10, 20), 0.0f, 0.0f) &&
              coverageAt(soft, 10, 20) < 0.5f,
          "refine: and the demonstration is not rigged -- that texel really is fractional "
          "and really is below the 0.5 a threshold would use");
  }

  // --- 2. The radius is a real number, which is the other half ------------
  {
    // Integer-aligned, so every edge is hard and the answer is predictable
    // from geometry alone: the contour sits on the texel boundary at x = 20,
    // and growing by r moves it to 20 + r. Texel 22 spans [22, 23), so at
    // r = 2.5 exactly half of it is covered.
    const Selection rect = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);

    const Selection g225 = growSelection(rect, 2.25f);
    const Selection g250 = growSelection(rect, 2.5f);
    const Selection g275 = growSelection(rect, 2.75f);
    std::printf("  [selftest] refine: hard edge grown by 2.25/2.50/2.75 puts the new "
                "boundary texel at %.5f / %.5f / %.5f\n",
                static_cast<double>(coverageAt(g225, 22, 15)),
                static_cast<double>(coverageAt(g250, 22, 15)),
                static_cast<double>(coverageAt(g275, 22, 15)));
    check(coverageAt(g225, 22, 15) == quantised(0.25f) &&
              coverageAt(g250, 22, 15) == quantised(0.5f) &&
              coverageAt(g275, 22, 15) == quantised(0.75f),
          "refine: a FRACTIONAL radius moves the edge by a fractional amount -- 2.25/2.5/2.75 "
          "give the covered AREA of the crossed texel, which iterated dilation cannot spell");

    // The control. If the three numbers above were a blur rather than a moved
    // edge, an integer radius would be soft too.
    const Selection g200 = growSelection(rect, 2.0f);
    const Selection g300 = growSelection(rect, 3.0f);
    check(coverageAt(g200, 21, 15) == 1.0f && coverageAt(g200, 22, 15) == 0.0f &&
              coverageAt(g300, 22, 15) == 1.0f && coverageAt(g300, 23, 15) == 0.0f,
          "refine: an INTEGER radius leaves the edge hard -- so the fractional answers above "
          "are a moved boundary and not softening, which is the difference from a feather");

    // Shrink is the same sign read the other way, and must be symmetric.
    const Selection s250 = shrinkSelection(rect, 2.5f);
    check(coverageAt(s250, 17, 15) == quantised(0.5f) && coverageAt(s250, 18, 15) == 0.0f &&
              coverageAt(s250, 16, 15) == 1.0f,
          "refine: shrink by 2.5 is the mirror of grow by 2.5 -- one sign in one place, so "
          "the two cannot disagree about what half a texel means");
    check(totalCoverage(shrinkSelection(rect, 2.5f)) < totalCoverage(rect) &&
              totalCoverage(growSelection(rect, 2.5f)) > totalCoverage(rect),
          "refine: and they move the area in opposite directions, which is the one thing a "
          "flipped sign would break everywhere at once");

    // Grow then shrink by the same amount returns the original, to within the
    // store. Not an algebraic identity in general -- a Minkowski open/close
    // rounds off features narrower than the radius -- but exact for a
    // rectangle whose sides are far longer than 2.5.
    const Selection roundTrip = shrinkSelection(growSelection(rect, 2.5f), 2.5f);
    float rtWorst = 0.0f;
    for (int32_t y = 5; y < 26; ++y) {
      for (int32_t x = 5; x < 26; ++x) {
        rtWorst = std::max(rtWorst, std::fabs(coverageAt(rect, x, y) - coverageAt(roundTrip, x, y)));
      }
    }
    std::printf("  [selftest] refine: grow 2.5 then shrink 2.5 differs from the original by "
                "%.6f (%.2f uint8 steps)\n",
                static_cast<double>(rtWorst), static_cast<double>(rtWorst) * 255.0);
    check(rtWorst <= kQuantum,
          "refine: grow then shrink by the same radius returns a large rectangle unchanged "
          "to one uint8 step -- the field is not accumulating a bias per operation");
  }

  // --- 3. The transform is EUCLIDEAN, which is why it is not a chamfer ----
  {
    // The measurement the chamfer rejection rests on. A disc grown by r is a
    // disc of radius R + r, and core/SelectionShapes computes that analytically
    // by a completely different route -- closed-form area integration, no
    // distance field anywhere. So this compares two independent methods.
    const float cx = 64.5f, cy = 64.5f;
    const Selection disc = selectEllipse(cx, cy, 20.0f, 20.0f);
    const Selection grown = growSelection(disc, 6.5f);
    const Selection exact = selectEllipse(cx, cy, 26.5f, 26.5f);

    double worst = 0.0, sum = 0.0;
    long affected = 0;
    for (int32_t y = 20; y < 110; ++y) {
      for (int32_t x = 20; x < 110; ++x) {
        const float a = coverageAt(grown, x, y);
        const float b = coverageAt(exact, x, y);
        const double d = std::fabs(static_cast<double>(a) - static_cast<double>(b));
        worst = std::max(worst, d);
        if (a > 0.0f || b > 0.0f) {
          sum += d;
          ++affected;
        }
      }
    }
    const double pi = 3.14159265358979323846;
    const double grownRadius = std::sqrt(totalCoverage(grown) / pi);
    const double exactRadius = std::sqrt(totalCoverage(exact) / pi);
    std::printf("  [selftest] refine: disc r=20 grown by 6.5 vs the analytic r=26.5 disc -- "
                "max %.5f, mean %.5f over %ld texels\n",
                worst, sum / static_cast<double>(affected), affected);
    std::printf("  [selftest] refine: area-implied radius %.4f vs %.4f, error %.4f texels "
                "(a 3-4 chamfer's own anisotropy is 2.949 texels at this radius)\n",
                grownRadius, exactRadius, std::fabs(grownRadius - exactRadius));

    check(std::fabs(grownRadius - exactRadius) < 0.05,
          "refine: the grown disc's AREA lands on the analytic disc's to under 0.05 texels of "
          "radius -- the aggregate measure an octagonal chamfer metric could not pass");
    check(sum / static_cast<double>(affected) < 0.01,
          "refine: and mean per-texel coverage error is under 1 % -- the transform is "
          "Euclidean, not a weighted-neighbour approximation of one");
    check(worst < 0.15,
          "refine: worst single texel stays under 0.15, which is the straight-edge coverage "
          "model meeting a curved boundary and not a direction-dependent stretch");

    // Isotropy as a direct comparison rather than only as an area. Split by
    // DIRECTION, because that is the axis a chamfer metric fails along: it is
    // exact where it steps along a row and worst on the 45-degree diagonal, so
    // a per-sector error that is flat is the evidence the metric is Euclidean.
    //
    // Per sector rather than per texel: the contour at radius 26.5 falls
    // exactly between two texel centres on the axis, so any single texel there
    // reads a saturated 0 or 1 and would assert nothing at all.
    double axisWorst = 0.0, diagWorst = 0.0;
    for (int32_t y = 20; y < 110; ++y) {
      for (int32_t x = 20; x < 110; ++x) {
        const double d = std::fabs(static_cast<double>(coverageAt(grown, x, y)) -
                                   coverageAt(exact, x, y));
        double a = std::atan2(y + 0.5 - cy, x + 0.5 - cx) * 180.0 / pi;
        a = std::fmod(std::fabs(a), 90.0);
        const double offAxis = std::min(a, 90.0 - a);
        if (offAxis < 10.0) axisWorst = std::max(axisWorst, d);
        if (offAxis > 35.0) diagWorst = std::max(diagWorst, d);
      }
    }
    std::printf("  [selftest] refine: worst error within 10 deg of an AXIS %.5f, within 10 deg "
                "of a DIAGONAL %.5f\n",
                axisWorst, diagWorst);
    check(axisWorst < 0.15 && diagWorst < 0.15 && std::fabs(axisWorst - diagWorst) < 0.10,
          "refine: the grown contour tracks the analytic one equally well along an AXIS and "
          "along a DIAGONAL -- the two directions a chamfer metric cannot keep together");
  }

  // --- 4. Sparsity, bounds, and the states that are not each other --------
  {
    Selection empty;
    check(growSelection(empty, 5.0f).tiles.occupiedTileCount() == 0 &&
              shrinkSelection(empty, 5.0f).tiles.occupiedTileCount() == 0,
          "refine: growing a selection that selects NOTHING gives nothing -- there is no "
          "edge to move, and the answer is not 'everything'");

    const Selection small = selectRectangle(10.0f, 10.0f, 14.0f, 14.0f);
    check(shrinkSelection(small, 2.0f).tiles.occupiedTileCount() == 0,
          "refine: shrinking a 4x4 marquee by 2 removes it entirely rather than leaving a "
          "tile of zeros -- the constructor invariant survives an operator that can empty one");
    check(shrinkSelection(small, 100.0f).tiles.occupiedTileCount() == 0,
          "refine: and an absurd shrink does the same instead of underflowing into a full "
          "selection");

    const Selection nan = growSelection(small, std::numeric_limits<float>::quiet_NaN());
    check(coverageAt(nan, 10, 10) == 1.0f && coverageAt(nan, 20, 20) == 0.0f,
          "refine: a non-finite radius returns the input rather than a plane of NaNs -- which "
          "would clamp to garbage coverage instead of failing visibly");

    // The result composes, which is the reason both operators return a
    // Selection rather than something bespoke.
    const Selection grown = growSelection(small, 3.0f);
    const Selection clipped = combineSelections(grown, selectAll(12, 12), SelectionCombine::Intersect);
    check(coverageAt(grown, 8, 8) > 0.0f && coverageAt(clipped, 8, 8) > 0.0f &&
              coverageAt(clipped, 13, 13) == 0.0f && coverageAt(grown, 13, 13) > 0.0f,
          "refine: a grown selection is unclipped and composes through combineSelections() -- "
          "the canvas clip is one visible line, not a bound baked into the operator");

    // A grow reaches into tiles the input never had, and pays for exactly
    // those. A 4x4 marquee at (10,10) grown by 200 must not allocate the
    // apron's every tile as zeros.
    const Selection wide = growSelection(small, 20.0f);
    check(wide.tiles.occupiedTileCount() >= 1 && wide.tiles.occupiedTileCount() <= 4,
          "refine: a grow allocates only tiles that actually hold coverage, not the whole "
          "apron -- 16 KiB of 'not selected' per tile is what the invariant is protecting");
  }

  // --- 5. Colour range has NO second opinion about 'similar' --------------
  {
    TileStore src;
    for (int32_t y = 0; y < 60; ++y) {
      for (int32_t x = 0; x < 60; ++x) {
        const float v = x < 20 ? 0.05f : (x < 40 ? 0.25f : 0.8f);
        const PixelCoord p{x, y};
        src.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p),
                                                   {v, v * 0.5f, v * 0.25f, 1.0f});
      }
    }
    const PixelCoord seed{25, 25};
    const std::array<float, 4> seedTexel =
        src.find(tileCoordAt(seed))->readPixel(tileLocalOffset(seed));

    FloodFillParams flood;
    flood.reach = FloodFillReach::Global;
    const Selection viaFlood = floodFillSelection(src, seed, 60, 60, flood);
    const Selection viaRange = selectColourRange(src, seedTexel, 60, 60, {});

    long differing = 0;
    for (int32_t y = 0; y < 60; ++y) {
      for (int32_t x = 0; x < 60; ++x) {
        if (coverageAt(viaFlood, x, y) != coverageAt(viaRange, x, y)) ++differing;
      }
    }
    std::printf("  [selftest] refine: colour range vs a Global flood fill on the same texel -- "
                "%ld of 3600 texels differ\n",
                differing);
    check(differing == 0,
          "refine: colour range fed the colour under a texel returns EXACTLY what the wand's "
          "global reach returns -- one tolerance metric in the build, asserted rather than "
          "hoped for");

    check(coverageAt(viaRange, 25, 25) == 1.0f && coverageAt(viaRange, 5, 5) == 0.0f &&
              coverageAt(viaRange, 50, 50) == 0.0f,
          "refine: and it is doing real work -- the matching band is selected, the two other "
          "bands are not");

    // No connectivity, which is the difference from the wand. Two disjoint
    // patches of the same colour must both come back.
    TileStore split;
    for (int32_t y = 0; y < 40; ++y) {
      for (int32_t x = 0; x < 40; ++x) {
        const bool patch = (x < 10 && y < 10) || (x >= 30 && y >= 30);
        const PixelCoord p{x, y};
        split.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p),
                                                     patch ? std::array<float, 4>{1, 1, 1, 1}
                                                           : std::array<float, 4>{0, 0, 0, 1});
      }
    }
    const Selection both = selectColourRange(split, {1.0f, 1.0f, 1.0f, 1.0f}, 40, 40, {});
    check(coverageAt(both, 5, 5) == 1.0f && coverageAt(both, 35, 35) == 1.0f &&
              coverageAt(both, 20, 20) == 0.0f,
          "refine: colour range has NO connectivity -- two disjoint patches of one colour "
          "both come back, which is the whole difference from the magic wand");

    // Bounds, and the empty-and-engaged state.
    check(selectColourRange(src, seedTexel, 0, 0, {}).tiles.occupiedTileCount() == 0 &&
              selectionSelectsNothing(selectColourRange(src, {9.0f, 9.0f, 9.0f, 1.0f}, 60, 60, {})),
          "refine: a zero-size document and a colour nothing matches both give an ENGAGED, "
          "empty selection -- never the default-constructed 'no restriction' state");

    // The edge tile clip: a 100-wide document's tile 0 holds 128 columns and
    // the last 28 are not part of the picture.
    TileStore full;
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        full.getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{x, y}, {1, 1, 1, 1});
      }
    }
    const Selection clipped = selectColourRange(full, {1.0f, 1.0f, 1.0f, 1.0f}, 100, 100, {});
    check(coverageAt(clipped, 99, 99) == 1.0f && coverageAt(clipped, 100, 50) == 0.0f &&
              coverageAt(clipped, 50, 100) == 0.0f,
          "refine: coverage stops at the DOCUMENT edge, not the tile edge -- the 28 columns "
          "an edge tile holds beyond a 100-wide picture are not part of it");
  }

  // --- 6. Luminance range: one luma, one domain, and alpha ----------------
  {
    // The weights are ops/PointOps' and the encode is color/Space's, so these
    // are not free parameters -- they are what the grayscale operator and the
    // eyedropper already say about the same pixel.
    const float black = selectionLuminanceOf({0.0f, 0.0f, 0.0f, 1.0f});
    const float white = selectionLuminanceOf({1.0f, 1.0f, 1.0f, 1.0f});
    const float mid = selectionLuminanceOf({0.18f, 0.18f, 0.18f, 1.0f});
    const float green = selectionLuminanceOf({0.0f, 1.0f, 0.0f, 1.0f});
    const float blue = selectionLuminanceOf({0.0f, 0.0f, 1.0f, 1.0f});
    std::printf("  [selftest] refine: encoded luminance -- black %.5f, 18%% grey %.5f, "
                "white %.5f, green %.5f, blue %.5f\n",
                static_cast<double>(black), static_cast<double>(mid), static_cast<double>(white),
                static_cast<double>(green), static_cast<double>(blue));
    check(black == 0.0f && near(white, 1.0f, 1e-6f),
          "refine: luminance pins black at 0 and white at 1 -- the two ends the range's "
          "endpoints are quoted against");
    check(green > blue && green > mid,
          "refine: and green outweighs blue by a wide margin, which is Rec.709 showing "
          "through rather than a naive channel average");
    check(near(mid, 0.46136f, 1e-4f),
          "refine: 18 % linear grey sits at 0.46 ENCODED, not at 0.18 -- the range is quoted "
          "in the domain the user is looking at, which is the point of the encode");

    TileStore src;
    for (int32_t y = 0; y < 40; ++y) {
      for (int32_t x = 0; x < 40; ++x) {
        const bool left = x < 20;
        const float a = left ? 1.0f : 0.5f;
        const float v = left ? 0.18f : 1.0f;
        const PixelCoord p{x, y};
        src.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {v * a, v * a, v * a, a});
      }
    }

    SelectionLuminanceRange highlights;
    highlights.low = 0.75f;
    highlights.high = 1.0f;
    const Selection hi = selectLuminanceRange(src, 80, 80, highlights);
    std::printf("  [selftest] refine: highlights 0.75..1.0 -- opaque 18%% grey %.5f, "
                "half-opaque white %.5f, never-written %.5f\n",
                static_cast<double>(coverageAt(hi, 5, 5)),
                static_cast<double>(coverageAt(hi, 30, 5)),
                static_cast<double>(coverageAt(hi, 70, 70)));
    check(coverageAt(hi, 5, 5) == 0.0f && coverageAt(hi, 30, 5) == quantised(0.5f),
          "refine: a half-opaque white is HALF selected by a highlight range -- coverage is "
          "weighted by alpha, the same 'half present, not half bright' rule as everywhere else");
    check(coverageAt(hi, 70, 70) == 0.0f,
          "refine: and a never-written texel is not selected at all");

    SelectionLuminanceRange shadows;
    shadows.low = 0.0f;
    shadows.high = 0.5f;
    const Selection lo = selectLuminanceRange(src, 80, 80, shadows);
    check(coverageAt(lo, 5, 5) == 1.0f && coverageAt(lo, 30, 5) == 0.0f,
          "refine: a shadow range picks the dark opaque half and leaves the bright half");
    check(coverageAt(lo, 70, 70) == 0.0f && lo.tiles.occupiedTileCount() == 1,
          "refine: and the empty canvas is NOT a shadow -- an unwritten texel un-premultiplies "
          "to black, so ignoring alpha here would select the whole document");

    SelectionLuminanceRange inverted;
    inverted.low = 0.9f;
    inverted.high = 0.1f;
    check(selectionSelectsNothing(selectLuminanceRange(src, 80, 80, inverted)),
          "refine: an inverted band selects NOTHING rather than everything outside itself -- "
          "an empty range is empty, not a complement");

    SelectionLuminanceRange hard;
    hard.low = 0.0f;
    hard.high = 0.5f;
    hard.edgeBand = 0.0f;
    const Selection hardSel = selectLuminanceRange(src, 80, 80, hard);
    check(coverageAt(hardSel, 5, 5) == 1.0f && coverageAt(hardSel, 30, 5) == 0.0f,
          "refine: a zero edge band gives a hard in/out answer instead of selecting nothing -- "
          "a band of zero width is not a tolerance of zero, and they mean opposite things");
  }

  std::printf("[selftest] selection refine %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
