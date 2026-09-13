#include "app/selftest/Support.hpp"

#include "core/PathBoolean.hpp"
#include "core/PathRaster.hpp"

namespace np {

// core/PathBoolean. Every expected value is computed by hand from the fixture
// geometry, and the areas are checked twice: once through
// `pathBooleanArea()`, and once by rasterising the result and summing
// coverage, so a wrong orientation that the area function happened to agree
// with still fails.
bool runPathBooleanTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto polygon = [](std::vector<PathPoint> pts, FillRule rule = FillRule::NonZero) {
    Path p;
    p.rule = rule;
    SubPath sub;
    sub.closed = true;
    for (const PathPoint& q : pts) {
      Anchor a;
      a.pt = a.in = a.out = q;
      sub.anchors.push_back(a);
    }
    p.subpaths.push_back(sub);
    return p;
  };
  auto square = [&](float x0, float y0, float x1, float y1) {
    return polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
  };
  auto circle = [](float cx, float cy, float r) {
    const float k = 0.5522847498f * r;
    Path p;
    SubPath sub;
    sub.closed = true;
    const PathPoint pts[4] = {{cx + r, cy}, {cx, cy + r}, {cx - r, cy}, {cx, cy - r}};
    const PathPoint tan[4] = {{0, k}, {-k, 0}, {0, -k}, {k, 0}};
    for (int i = 0; i < 4; ++i) {
      Anchor a;
      a.pt = pts[i];
      a.in = {pts[i].x - tan[i].x, pts[i].y - tan[i].y};
      a.out = {pts[i].x + tan[i].x, pts[i].y + tan[i].y};
      sub.anchors.push_back(a);
    }
    p.subpaths.push_back(sub);
    return p;
  };
  // Coverage summed over a raster whose origin is shifted by (ox, oy), so
  // fixtures with negative coordinates stay inside the clip.
  auto rasterArea = [](const Path& path, int32_t w, int32_t h, float ox = 0, float oy = 0) {
    Path shifted = path;
    for (SubPath& s : shifted.subpaths)
      for (Anchor& a : s.anchors)
        for (PathPoint* q : {&a.pt, &a.in, &a.out}) {
          q->x += ox;
          q->y += oy;
        }
    double sum = 0.0;
    PathRasterScratch scratch;
    rasterizePath(shifted, 0.01f, RasterClip{0, 0, w, h}, scratch,
                  [&](int32_t, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x) sum += cov[x - x0];
                  });
    return sum;
  };
  auto coverageAt = [](const Path& path, int32_t w, int32_t h, int32_t px, int32_t py) {
    float v = 0.0f;
    PathRasterScratch scratch;
    rasterizePath(path, 0.01f, RasterClip{0, 0, w, h}, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    if (y == py && px >= x0 && px < x1) v = cov[px - x0];
                  });
    return v;
  };
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
  auto anchorCount = [](const Path& p) {
    size_t n = 0;
    for (const SubPath& s : p.subpaths) n += s.anchors.size();
    return n;
  };

  // --- 1. Two overlapping squares: A=[0,10]^2, B=[5,15]^2, overlap 25 -----
  {
    const Path A = square(0, 0, 10, 10);
    const Path B = square(5, 5, 15, 15);
    const Path u = pathBoolean(PathBooleanOp::Union, A, B);
    const Path i = pathBoolean(PathBooleanOp::Intersect, A, B);
    const Path d = pathBoolean(PathBooleanOp::Difference, A, B);
    const Path x = pathBoolean(PathBooleanOp::Xor, A, B);
    std::printf("  [measured] areas: union %.4f  intersect %.4f  difference %.4f  xor %.4f\n",
                pathBooleanArea(u), pathBooleanArea(i), pathBooleanArea(d), pathBooleanArea(x));
    check(near(pathBooleanArea(u), 175.0, 1e-3), "squares: union area is 100 + 100 - 25 = 175");
    check(near(pathBooleanArea(i), 25.0, 1e-3), "squares: intersect area is the 5x5 overlap, 25");
    check(near(pathBooleanArea(d), 75.0, 1e-3), "squares: A minus B area is 100 - 25 = 75");
    check(near(pathBooleanArea(x), 150.0, 1e-3), "squares: xor area is 175 - 25 = 150");

    std::printf("  [measured] raster areas: union %.3f  difference %.3f  xor %.3f\n",
                rasterArea(u, 16, 16), rasterArea(d, 16, 16), rasterArea(x, 16, 16));
    check(near(rasterArea(u, 16, 16), 175.0, 0.05) && near(rasterArea(d, 16, 16), 75.0, 0.05) &&
              near(rasterArea(x, 16, 16), 150.0, 0.05),
          "squares: rasterised NonZero coverage agrees with every area above");

    // The intersection's corners are exactly two original corners and the
    // two crossings (10,5) and (5,10).
    bool corners = i.subpaths.size() == 1 && i.subpaths[0].anchors.size() == 4;
    const PathPoint want[4] = {{5, 5}, {10, 5}, {10, 10}, {5, 10}};
    for (const PathPoint& w : want) {
      bool found = false;
      if (corners)
        for (const Anchor& a : i.subpaths[0].anchors)
          if (near(a.pt.x, w.x, 1e-4) && near(a.pt.y, w.y, 1e-4)) found = true;
      corners = corners && found;
    }
    check(corners, "squares: intersect is ONE contour whose four corners are exactly "
                   "(5,5) (10,5) (10,10) (5,10)");
    check(u.subpaths.size() == 1 && anchorCount(u) == 8,
          "squares: union is one contour of 8 corners -- no split vertex survives on a "
          "straight edge");
    bool straight = u.rule == FillRule::NonZero;
    for (const SubPath& s : u.subpaths)
      for (const Anchor& a : s.anchors)
        if (a.in.x != a.pt.x || a.in.y != a.pt.y || a.out.x != a.pt.x || a.out.y != a.pt.y)
          straight = false;
    check(straight, "output is NonZero and every anchor's handles sit on the anchor");
  }

  // --- 2. Operands that coincide exactly ---------------------------------
  {
    const Path A = square(0, 0, 10, 10);
    const Path u = pathBoolean(PathBooleanOp::Union, A, A);
    check(u.subpaths.size() == 1 && anchorCount(u) == 4 && near(pathBooleanArea(u), 100.0, 1e-3),
          "coincident: A union A is A -- one contour, 4 corners, area 100");
    check(near(pathBooleanArea(pathBoolean(PathBooleanOp::Intersect, A, A)), 100.0, 1e-3),
          "coincident: A intersect A has area 100");
    check(pathBoolean(PathBooleanOp::Difference, A, A).subpaths.empty() &&
              pathBoolean(PathBooleanOp::Xor, A, A).subpaths.empty(),
          "coincident: A minus A and A xor A are both EMPTY paths");
  }

  // --- 3. A shared edge, and a shared corner -----------------------------
  {
    const Path L = square(0, 0, 10, 10);
    const Path R = square(10, 0, 20, 10);
    const Path u = pathBoolean(PathBooleanOp::Union, L, R);
    check(u.subpaths.size() == 1 && anchorCount(u) == 4 && near(pathBooleanArea(u), 200.0, 1e-3),
          "shared edge: side-by-side squares unite to ONE 20x10 rectangle of 4 corners");
    check(pathBoolean(PathBooleanOp::Intersect, L, R).subpaths.empty(),
          "shared edge: their intersection is empty -- a line has no area");

    const Path corner = pathBoolean(PathBooleanOp::Union, L, square(10, 10, 20, 20));
    std::printf("  [measured] pinch union: %zu contours, raster area %.3f\n",
                corner.subpaths.size(), rasterArea(corner, 21, 21));
    check(near(rasterArea(corner, 21, 21), 200.0, 0.05),
          "shared corner: squares touching at one point unite to raster area 200");
  }

  // --- 4. A nested hole ---------------------------------------------------
  {
    const Path d = pathBoolean(PathBooleanOp::Difference, square(0, 0, 20, 20), square(5, 5, 15, 15));
    check(d.subpaths.size() == 2 && near(pathBooleanArea(d), 300.0, 1e-3),
          "hole: [0,20]^2 minus [5,15]^2 is two contours of net area 400 - 100 = 300");
    check(coverageAt(d, 20, 20, 10, 10) < 0.01f && coverageAt(d, 20, 20, 2, 2) > 0.99f &&
              near(rasterArea(d, 20, 20), 300.0, 0.05),
          "hole: rasterised, the centre is EMPTY, the ring FILLED, and coverage sums to 300");
  }

  // --- 5. Each operand is read under its OWN fill rule --------------------
  {
    // Two same-direction nested squares: solid under NonZero, a 300-unit
    // ring under EvenOdd. Intersected with the half-plane-ish [0,20]x[0,10],
    // the ring gives 200 - (hole part [5,15]x[5,10] = 50) = 150; read as
    // NonZero it would give 200.
    Path ring = square(0, 0, 20, 20);
    ring.subpaths.push_back(square(5, 5, 15, 15).subpaths[0]);
    ring.rule = FillRule::EvenOdd;
    const double area = pathBooleanArea(pathBoolean(PathBooleanOp::Intersect, ring, square(0, 0, 20, 10)));
    std::printf("  [measured] EvenOdd ring intersect half: %.4f\n", area);
    check(near(area, 150.0, 1e-3), "fill rule: an EvenOdd ring intersected with its top half is "
                                   "150, not the NonZero reading's 200");
  }

  // --- 6. A self-intersecting operand ------------------------------------
  {
    // A bowtie: two triangles of area 25 each, crossing at (5,5).
    const Path bow = polygon({{0, 0}, {10, 10}, {10, 0}, {0, 10}});
    const Path u = pathBoolean(PathBooleanOp::Union, bow, Path{});
    std::printf("  [measured] bowtie: %zu contours, area %.4f, raster %.3f\n", u.subpaths.size(),
                pathBooleanArea(u), rasterArea(u, 10, 10));
    check(u.subpaths.size() == 2 && near(pathBooleanArea(u), 50.0, 1e-3) &&
              near(rasterArea(u, 10, 10), 50.0, 0.05),
          "self-crossing: a bowtie united with nothing is two triangles of total area 50");
  }

  // --- 7. Curves: two r=10 circles whose centres are 10 apart ------------
  {
    // Lens area 2 r^2 acos(d/2r) - (d/2) sqrt(4r^2 - d^2) = 200 pi/3 - 50 sqrt 3,
    // crossings at (5, +-5 sqrt 3).
    const double lens = 200.0 * 3.14159265358979 / 3.0 - 50.0 * std::sqrt(3.0);
    const double unionArea = 2.0 * 3.14159265358979 * 100.0 - lens;
    const Path A = circle(0, 0, 10);
    const Path B = circle(10, 0, 10);
    const Path i = pathBoolean(PathBooleanOp::Intersect, A, B, 0.005f);
    const Path u = pathBoolean(PathBooleanOp::Union, A, B, 0.005f);
    std::printf("  [measured] lens %.4f (exact %.4f), union %.4f (exact %.4f)\n",
                pathBooleanArea(i, 0.005f), lens, pathBooleanArea(u, 0.005f), unionArea);
    check(near(pathBooleanArea(i, 0.005f), lens, lens * 0.002),
          "circles: the lens has area 200pi/3 - 50 sqrt3 = 122.837, within 0.2%");
    check(near(pathBooleanArea(u, 0.005f), unionArea, unionArea * 0.002),
          "circles: the union has area 200pi - lens = 505.48, within 0.2%");
    const double h = 5.0 * std::sqrt(3.0);
    bool top = false, bottom = false;
    for (const SubPath& s : i.subpaths)
      for (const Anchor& a : s.anchors) {
        if (near(a.pt.x, 5.0, 0.01) && near(a.pt.y, h, 0.01)) top = true;
        if (near(a.pt.x, 5.0, 0.01) && near(a.pt.y, -h, 0.01)) bottom = true;
      }
    check(top && bottom, "circles: the lens has vertices at both crossings (5, +-8.660)");
  }

  // --- 8. Disjoint operands, and untrusted input -------------------------
  {
    const Path A = square(0, 0, 10, 10);
    const Path B = square(20, 0, 30, 10);
    check(pathBoolean(PathBooleanOp::Intersect, A, B).subpaths.empty(),
          "disjoint: intersection is empty");
    const Path u = pathBoolean(PathBooleanOp::Union, A, B);
    check(u.subpaths.size() == 2 && near(pathBooleanArea(u), 200.0, 1e-3),
          "disjoint: union keeps both, two contours of area 200");

    Path bad = square(0, 0, 10, 10);
    bad.subpaths[0].anchors[1].out.x = std::numeric_limits<float>::quiet_NaN();
    check(pathBoolean(PathBooleanOp::Union, bad, B).subpaths.empty(),
          "non-finite: an operand with a NaN handle yields an empty path, not NaN vertices");
  }

  std::printf("[selftest] path boolean %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
