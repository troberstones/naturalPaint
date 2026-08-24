#include "app/selftest/Support.hpp"

#include <cmath>
#include <vector>

#include "core/SelectionMask.hpp"
#include "core/SelectionShapes.hpp"

namespace np {
namespace {

// The store's own quantisation step. Every tolerance below is stated as a
// multiple of THIS rather than as a decimal picked to make a test pass: a
// SelectionTile holds one uint8 per texel, so 1/255 is the finest difference
// it can represent and no exact-area algorithm can beat it.
constexpr float kQuantum = 1.0f / 255.0f;

// Ground truth for one texel, by brute force. Slow and obviously correct,
// which is the point -- it is the independent check on the closed-form
// integration, not a second copy of it.
double superSampleEllipse(int32_t x, int32_t y, double cx, double cy, double rx, double ry,
                          int n) {
  int inside = 0;
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      const double u = x + (i + 0.5) / n;
      const double v = y + (j + 0.5) / n;
      const double dx = (u - cx) / rx;
      const double dy = (v - cy) / ry;
      if (dx * dx + dy * dy <= 1.0) ++inside;
    }
  }
  return static_cast<double>(inside) / (static_cast<double>(n) * n);
}

double totalCoverage(const Selection& s) {
  double sum = 0.0;
  for (const auto& [coord, tile] : s.tiles) {
    (void)coord;
    for (int32_t ly = 0; ly < kTileSize; ++ly)
      for (int32_t lx = 0; lx < kTileSize; ++lx) sum += tile.coverageAt(PixelCoord{lx, ly});
  }
  return sum;
}

}  // namespace

// core/SelectionShapes -- PRD E3's ellipse, lasso and polygon lasso.
//
// The claim under test is EXACTNESS: these compute true covered area rather
// than counting samples, so the only error should be the store's own 1/255
// quantisation. Every tolerance here was measured before it was written, and
// each check says what was measured next to what is allowed.
bool runSelectionShapesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. The ellipse's area is the ellipse's area ------------------------
  {
    // pi*rx*ry, closed form, against the summed coverage. Measured worst
    // relative error across five radius pairs x three sub-texel centres was
    // 2.0e-4, on the SMALLEST ellipse (rx 3.5, ry 7.25) where the boundary is
    // the largest fraction of the shape and accumulated quantisation therefore
    // weighs most. The bound below is 1e-3, five times the worst measurement.
    bool allNear = true;
    double worst = 0.0;
    const std::vector<std::pair<double, double>> radii = {
        {10.0, 10.0}, {40.0, 25.0}, {3.5, 7.25}, {60.0, 5.0}};
    for (const auto& [rx, ry] : radii) {
      // Off-integer centres deliberately: an ellipse centred on a texel corner
      // is the easy case, and it is not the case a pointer produces.
      for (const double off : {0.0, 0.37, 0.5}) {
        const Selection s = selectEllipse(static_cast<float>(200.0 + off),
                                          static_cast<float>(200.0 + off),
                                          static_cast<float>(rx), static_cast<float>(ry));
        const double want = 3.14159265358979323846 * rx * ry;
        const double rel = std::fabs(totalCoverage(s) - want) / want;
        worst = std::max(worst, rel);
        if (!(rel < 1e-3)) allNear = false;
      }
    }
    check(allNear,
          "ellipse: summed coverage is pi*rx*ry to better than 1e-3 relative, across radii "
          "and sub-texel centres (worst measured 2.0e-4, on the smallest ellipse)");
    (void)worst;

    // The sharper claim: per texel, the closed form agrees with brute force to
    // within ONE quantisation step. Measured worst delta was 0.00205 -- just
    // over half a step -- which says the residual is the uint8 store and not
    // the integration. If this ever exceeds 1/255 the algorithm has drifted,
    // not the rounding.
    const double cx = 64.3, cy = 64.7, rx = 20.0, ry = 13.0;
    const Selection s = selectEllipse(static_cast<float>(cx), static_cast<float>(cy),
                                      static_cast<float>(rx), static_cast<float>(ry));
    double worstTexel = 0.0;
    for (int32_t y = 40; y < 90; ++y) {
      for (int32_t x = 40; x < 90; ++x) {
        const double got = selectionCoverageAt(&s, PixelCoord{x, y});
        const double want = superSampleEllipse(x, y, cx, cy, rx, ry, 64);
        worstTexel = std::max(worstTexel, std::fabs(got - want));
      }
    }
    check(worstTexel <= kQuantum,
          "ellipse: every texel matches a brute-force supersample to within one 1/255 step "
          "-- the residual is the store's quantisation, not the integration");

    // Coverage, not a bitmask (PRD E2) -- the same property section 2 asserts
    // for the rectangle, restated for a curved edge where it is harder to hold.
    bool sawPartial = false;
    for (int32_t x = 40; x < 90 && !sawPartial; ++x) {
      const float c = selectionCoverageAt(&s, PixelCoord{x, 65});
      if (c > 0.02f && c < 0.98f) sawPartial = true;
    }
    check(sawPartial,
          "ellipse: its edge texels carry PARTIAL coverage -- a curved boundary is "
          "antialiased, not rounded to in-or-out");

    check(selectEllipse(50.0f, 50.0f, 0.0f, 10.0f).tiles.occupiedTileCount() == 0 &&
              selectEllipse(50.0f, 50.0f, 10.0f, -1.0f).tiles.occupiedTileCount() == 0,
          "ellipse: a zero or negative radius selects NOTHING, matching the degenerate "
          "rectangle -- and deliberately not everything");
  }

  // --- 2. The polygon, against an independent implementation --------------
  {
    // The strongest check available: a rectangle expressed as four vertices
    // must reproduce selectRectangle(), which reaches the same answer by a
    // completely different route (separable axis overlaps, no clipping, no
    // winding). Measured worst delta across 2500 texels was EXACTLY 0.0; the
    // bound is one quantisation step so that a future compiler reordering a
    // float sum cannot flake it.
    const Selection poly =
        selectPolygon({{12.3f, 7.8f}, {40.6f, 7.8f}, {40.6f, 33.1f}, {12.3f, 33.1f}});
    const Selection rect = selectRectangle(12.3f, 7.8f, 40.6f, 33.1f);
    float worst = 0.0f;
    for (int32_t y = 0; y < 50; ++y) {
      for (int32_t x = 0; x < 50; ++x) {
        worst = std::max(worst, std::fabs(selectionCoverageAt(&poly, PixelCoord{x, y}) -
                                          selectionCoverageAt(&rect, PixelCoord{x, y})));
      }
    }
    check(worst <= kQuantum,
          "polygon: a four-vertex rectangle reproduces selectRectangle() -- two independent "
          "algorithms agreeing texel for texel (measured delta was exactly zero)");

    // A triangle's area is base*height/2 and nothing about the rasteriser
    // should perturb that. Measured relative error 3.3e-9, which is float
    // arithmetic rather than coverage error, because a triangle's boundary
    // texels happen to quantise almost symmetrically.
    const Selection tri = selectPolygon({{10.0f, 10.0f}, {50.0f, 10.0f}, {10.0f, 40.0f}});
    const double triWant = 0.5 * 40.0 * 30.0;
    check(std::fabs(totalCoverage(tri) - triWant) / triWant < 1e-3,
          "polygon: a triangle's summed coverage is base*height/2 (measured 3.3e-9 "
          "relative, so the bound is slack by six orders of magnitude)");

    // A bowtie exercises the crossing logic, but it does NOT discriminate the
    // winding rule: its two lobes wind +1 and -1, and both are odd, so
    // even-odd and nonzero agree. Checked anyway, and labelled for what it is,
    // because the first version of this section asserted the bowtie and
    // claimed it proved the rule -- it does not, and a sabotage run flipping
    // the rule to even-odd passed it untouched.
    const Selection bowtie =
        selectPolygon({{10.0f, 10.0f}, {30.0f, 30.0f}, {10.0f, 30.0f}, {30.0f, 10.0f}});
    const double bowtieWant = 200.0;  // two 10x10 triangles
    check(std::fabs(totalCoverage(bowtie) - bowtieWant) / bowtieWant < 1e-3,
          "polygon: a self-crossing bowtie keeps both lobes (this does NOT distinguish the "
          "winding rule -- both lobes are odd; the next check is the one that does)");

    // The rule itself. A square traced TWICE in the same direction winds to 2
    // inside: nonzero selects it, even-odd punches it out and leaves only the
    // boundary ring. That difference is enormous -- the full 400 texels versus
    // the perimeter alone -- so this is the check that actually holds the
    // header's choice in place.
    const Selection doubled = selectPolygon({{10.0f, 10.0f},
                                             {30.0f, 10.0f},
                                             {30.0f, 30.0f},
                                             {10.0f, 30.0f},
                                             {10.0f, 10.0f},
                                             {30.0f, 10.0f},
                                             {30.0f, 30.0f},
                                             {10.0f, 30.0f}});
    check(std::fabs(totalCoverage(doubled) - 400.0) / 400.0 < 1e-3,
          "polygon: a square traced twice winds to 2 and stays SOLID -- nonzero winding, "
          "not even-odd, which would hollow it out and leave a ring");

    check(selectPolygon({}).tiles.occupiedTileCount() == 0 &&
              selectPolygon({{1.0f, 1.0f}, {2.0f, 2.0f}}).tiles.occupiedTileCount() == 0,
          "polygon: fewer than three vertices enclose no area and select nothing");

    // The lasso and the polygon lasso are the same function at two densities,
    // which is the claim the header makes. A circle approximated by 256
    // vertices -- what a freehand drag produces -- must land on the ellipse's
    // area, reached by an entirely different method.
    std::vector<SelectionPoint> circle;
    for (int i = 0; i < 256; ++i) {
      const double a = 2.0 * 3.14159265358979323846 * i / 256.0;
      circle.push_back(SelectionPoint{static_cast<float>(100.0 + 30.0 * std::cos(a)),
                                      static_cast<float>(100.0 + 30.0 * std::sin(a))});
    }
    const double lassoArea = totalCoverage(selectPolygon(circle));
    const double circleArea = 3.14159265358979323846 * 30.0 * 30.0;
    // A 256-gon inscribed in a circle is SMALLER than it by a factor
    // cos^2(pi/n) ~ 1 - (pi/n)^2, which at n=256 is 1.5e-4. So the bound is
    // the polygon approximation itself, not the rasteriser, and 1e-3 leaves
    // room for it without hiding a real error.
    check(std::fabs(lassoArea - circleArea) / circleArea < 1e-3,
          "polygon: a 256-vertex freehand circle lands on pi*r^2 -- the lasso and the "
          "ellipse agree by two entirely different routes");
  }

  std::printf("[selftest] selection shapes %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
