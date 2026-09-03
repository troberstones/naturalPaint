#include "app/selftest/Support.hpp"

#include <cmath>
#include <random>

#include "core/Path.hpp"
#include "core/PathFlatten.hpp"
#include "core/PathRaster.hpp"
#include "core/PathStroke.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// core/Path, core/PathFlatten and core/PathRaster: the vector geometry model,
// the curve flattener and the antialiased coverage rasteriser (PLAN.md
// "13 -- Paths"; PRD J1, J2, J4).
//
// Headless and GPU-free. Writes no files.
//
// ==========================================================================
// Why there is a differential oracle here, and what it is worth
// ==========================================================================
//
// The strongest claim available for a new rasteriser is not "it looks right"
// and not "it matches a tolerance I chose". It is that a SECOND, independently
// written, independently justified exact-area implementation already in this
// tree agrees with it.
//
// `core/SelectionShapes.cpp`'s `selectPolygon()` is that implementation. It
// computes exact coverage by clipping the polygon against each boundary
// texel's unit square and taking the true area (Sutherland-Hodgman plus the
// shoelace formula) -- an algorithm with nothing whatsoever in common with
// this file's cell accumulation. It is limited to a single contour and costs
// O(vertices * boundary texels), which is exactly why it is not the
// implementation; but for a single contour it is an oracle, and section 2
// below holds the new rasteriser to it over randomised polygons at a
// tolerance derived from the store it quantises into, not chosen to pass.
//
// Sections 3 and 4 then cover what the oracle cannot reach: multiple
// contours, and the even-odd fill rule. Section 4 is the assertion that
// actually pins the choice of algorithm -- see core/PathRaster.hpp section 2
// on why the simpler signed-area rasteriser cannot express even-odd at all.
bool runPathRasterTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- helpers -------------------------------------------------------------

  // A closed subpath through `pts` with every handle coincident with its own
  // anchor: core/Path.hpp section 1's straight-line encoding, so this is a
  // genuine polygon and not a curve that happens to look like one.
  auto polygonPath = [](const std::vector<PathPoint>& pts, FillRule rule) {
    Path p;
    p.rule = rule;
    SubPath sub;
    sub.closed = true;
    for (const PathPoint& q : pts) {
      Anchor a;
      a.pt = a.in = a.out = q;
      sub.anchors.push_back(a);
    }
    p.subpaths.push_back(std::move(sub));
    return p;
  };

  // Rasterise into a dense float image so tests can index it directly.
  auto rasterize = [](const Path& path, int32_t w, int32_t h) {
    std::vector<float> img(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
    PathRasterScratch scratch;
    RasterClip clip{0, 0, w, h};
    rasterizePath(path, 0.01f, clip, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x)
                      img[static_cast<size_t>(y) * static_cast<size_t>(w) +
                          static_cast<size_t>(x)] = cov[x - x0];
                  });
    return img;
  };

  auto sumOf = [](const std::vector<float>& img) {
    double s = 0.0;
    for (float v : img) s += v;
    return s;
  };

  // --- 1. Analytic oracles: the total coverage IS the area -----------------
  //
  // Exact-area coverage means the sum over all texels equals the shape's true
  // area, with no dependence on where the shape sits relative to the grid.
  // A supersampler would miss this by its own sampling error; a rasteriser
  // that double-counted a shared edge would exceed it.
  {
    // A rectangle placed deliberately off the grid in both axes, so that
    // every one of its four edges cuts texels rather than landing on
    // boundaries. On-grid would pass under almost any implementation.
    const float x0 = 3.25f, y0 = 5.75f, x1 = 20.5f, y1 = 14.125f;
    const Path rect = polygonPath(
        {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, FillRule::NonZero);
    const double expect =
        static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
    const double got = sumOf(rasterize(rect, 32, 24));
    // Tolerance derived, not chosen: the coverage of each of the ~60 boundary
    // texels is a sum of a handful of float products, so relative error is a
    // few ulps of float (2^-24) per texel. 1e-3 absolute over an area of ~144
    // is roughly 7e-6 relative -- three orders of magnitude of headroom over
    // the arithmetic, and tight enough that a half-texel misplacement (which
    // would cost ~8 units of area here) fails outright.
    std::printf("  [measured] off-grid rectangle area: expected %.6f, got %.6f\n",
                expect, got);
    check(std::fabs(got - expect) < 1.0e-3, "rasteriser: off-grid rectangle covers exactly its area");

    // Translating by a non-integer amount must not change the total. This is
    // the property that separates exact area from any sampling scheme.
    const Path moved = polygonPath({{x0 + 0.37f, y0 + 0.61f},
                                    {x1 + 0.37f, y0 + 0.61f},
                                    {x1 + 0.37f, y1 + 0.61f},
                                    {x0 + 0.37f, y1 + 0.61f}},
                                   FillRule::NonZero);
    const double gotMoved = sumOf(rasterize(moved, 32, 24));
    check(std::fabs(gotMoved - expect) < 1.0e-3,
          "rasteriser: area is invariant under sub-texel translation");
  }

  // A circle as four cubic quarter-arcs with the classic
  // `k = 4/3 (sqrt(2) - 1) r` handle length. Deliberately built by hand
  // rather than through `arcToCubics()`, so that this measures the FLATTENER
  // and the RASTERISER against a known area, and the arc converter is tested
  // separately below against the true ellipse. Folding both into one area
  // check would leave either one free to be wrong by however much the other
  // was wrong in the opposite direction.
  {
    const float cx = 24.0f, cy = 24.0f, r = 15.3f;
    const float k = r * 0.5522847498307936f;
    Path circle;
    circle.rule = FillRule::NonZero;
    SubPath sub;
    sub.closed = true;
    auto anchor = [](PathPoint pt, PathPoint in, PathPoint out) {
      Anchor a;
      a.pt = pt;
      a.in = in;
      a.out = out;
      a.smooth = true;
      return a;
    };
    sub.anchors.push_back(anchor({cx + r, cy}, {cx + r, cy - k}, {cx + r, cy + k}));
    sub.anchors.push_back(anchor({cx, cy + r}, {cx + k, cy + r}, {cx - k, cy + r}));
    sub.anchors.push_back(anchor({cx - r, cy}, {cx - r, cy + k}, {cx - r, cy - k}));
    sub.anchors.push_back(anchor({cx, cy - r}, {cx - k, cy - r}, {cx + k, cy - r}));
    circle.subpaths.push_back(std::move(sub));

    const double expect = 3.14159265358979 * static_cast<double>(r) * r;
    const double got = sumOf(rasterize(circle, 48, 48));
    const double relErr = std::fabs(got - expect) / expect;
    std::printf("  [measured] circle area: expected %.4f, got %.4f (rel %.2e)\n",
                expect, got, relErr);
    // Derived, not chosen. Two approximations perturb the radius and area
    // goes as r^2, so the relative area error is about twice the relative
    // radius error:
    //   * the cubic quarter-arc fit is at most 2.73e-4 r too small;
    //   * flattening at 0.01 px cuts chords inside the curve, costing on
    //     average about (2/3)(0.01) px of radius, i.e. 4.4e-4 r.
    // Together ~1.4e-3 relative. 5e-3 leaves headroom for the rounding
    // without being vacuous -- a rasteriser off by half a texel around this
    // circumference would land near 6e-2 and fail, as an earlier revision of
    // this very test did.
    check(relErr < 5.0e-3, "rasteriser: circle covers pi r^2 within the derived bound");
  }

  // `arcToCubics()` on its own, against the true ellipse rather than against
  // an area -- a far sharper test, because an area check lets a bulge on one
  // side cancel a dent on the other.
  //
  // NOTE the aliasing hazard this test had to be rewritten to avoid, because
  // the same shape will appear in io/SvgImport: `arcToCubics()` appends to
  // the caller's vector AND writes through a pointer to the previous
  // anchor's handle. Passing `&v.back().out` while it appends to `v` is a
  // dangling write the moment the vector reallocates. The converter takes a
  // separate `fromOut` pointer precisely so the caller can keep it outside
  // the vector; do that.
  {
    const float rx = 20.0f, ry = 12.0f, rotDeg = 30.0f;
    const float ccx = 24.0f, ccy = 24.0f;
    const float ph = rotDeg * 3.14159265358979f / 180.0f;
    const float cp = std::cos(ph), sp = std::sin(ph);

    // Endpoints are DERIVED from the ellipse, at parameter t, rather than
    // picked by eye. An earlier revision of this test chose two points that
    // merely looked opposite and assumed the centre was their midpoint; on a
    // ROTATED ellipse that is false, so it measured the residual against the
    // wrong centre and reported a 27% error against correct output. Deriving
    // the endpoints is what makes the centre known independently of the code
    // under test.
    auto onEllipse = [&](float t) {
      const float ex = rx * std::cos(t), ey = ry * std::sin(t);
      return PathPoint{cp * ex - sp * ey + ccx, sp * ex + cp * ey + ccy};
    };

    // Every sampled point of the produced cubics, as a residual on the unit
    // circle in the ellipse's own frame. Sharper than an area check, which
    // lets a bulge on one side cancel a dent on the other.
    auto worstResidual = [&](const std::vector<Anchor>& arc, PathPoint from,
                             PathPoint fromOut) {
      double worst = 0.0;
      PathPoint prev = from, prevOut = fromOut;
      for (const Anchor& a : arc) {
        PathPoint seg[4] = {prev, prevOut, a.in, a.pt};
        for (int i = 0; i <= 16; ++i) {
          const PathPoint q = cubicAt(seg, static_cast<float>(i) / 16.0f);
          const float ux = (q.x - ccx) * cp + (q.y - ccy) * sp;
          const float uy = -(q.x - ccx) * sp + (q.y - ccy) * cp;
          const float e = std::sqrt((ux / rx) * (ux / rx) + (uy / ry) * (uy / ry));
          worst = std::max(worst, static_cast<double>(std::fabs(e - 1.0f)));
        }
        prev = a.pt;
        prevOut = a.out;
      }
      return worst;
    };

    // A quarter turn. This is the case that genuinely exercises the centre
    // computation (SVG F.6.5 step 2): for a half turn the centre falls out as
    // the midpoint and the interesting arithmetic is skipped.
    {
      const PathPoint from = onEllipse(0.0f);
      const PathPoint to = onEllipse(1.57079632679f);
      std::vector<Anchor> arc;
      PathPoint fromOut{};
      // The converter appends to `arc` AND writes through `fromOut`. Keeping
      // `fromOut` OUTSIDE the vector is required, not stylistic: passing
      // `&arc.back().out` is a dangling write the moment the vector
      // reallocates. io/SvgImport must follow the same rule.
      const bool built =
          arcToCubics(from, rx, ry, rotDeg, false, true, to, &fromOut, &arc);
      check(built && !arc.empty(), "arcToCubics: a rotated quarter turn builds cubics");
      check(std::fabs(arc.back().pt.x - to.x) < 1.0e-3f &&
                std::fabs(arc.back().pt.y - to.y) < 1.0e-3f,
            "arcToCubics: the last anchor lands exactly on the stated endpoint");
      const double worst = worstResidual(arc, from, fromOut);
      std::printf("  [measured] arcToCubics quarter turn: worst radial deviation %.3e\n",
                  worst);
      // The published worst case for the `k = 4/3 tan(theta/4)` fit over a
      // quarter turn is 2.73e-4 of the radius. 5e-4 is that plus rounding --
      // tight enough that a wrong k, or a failure to split at 90 degrees,
      // fails immediately.
      check(worst < 5.0e-4, "arcToCubics: quarter turn stays on the true ellipse (2.7e-4 bound)");
    }

    // A three-quarter turn, which must be split into more than one piece --
    // the assertion that the 90-degree splitting happens at all. Without it a
    // single cubic spanning 270 degrees is wrong by tens of percent.
    {
      const PathPoint from = onEllipse(0.0f);
      const PathPoint to = onEllipse(4.71238898038f);
      std::vector<Anchor> arc;
      PathPoint fromOut{};
      const bool built =
          arcToCubics(from, rx, ry, rotDeg, true, true, to, &fromOut, &arc);
      check(built && arc.size() >= 3,
            "arcToCubics: a 270-degree sweep is split into at least three pieces");
      const double worst = worstResidual(arc, from, fromOut);
      std::printf("  [measured] arcToCubics 270-degree turn: worst radial deviation %.3e\n",
                  worst);
      check(worst < 5.0e-4, "arcToCubics: the split pieces all stay on the ellipse");
    }

    // SVG 1.1 F.6.2's two named degeneracies: both are "draw a line", not an
    // error, and the caller relies on `false` to mean exactly that.
    {
      std::vector<Anchor> arc;
      PathPoint fromOut{};
      check(!arcToCubics({0.0f, 0.0f}, 0.0f, 12.0f, 0.0f, false, true, {10.0f, 0.0f},
                         &fromOut, &arc),
            "arcToCubics: a zero radius degenerates to a line (F.6.2), not an error");
      check(arc.empty(), "arcToCubics: and appends nothing when it refuses");
      check(!arcToCubics({5.0f, 5.0f}, 10.0f, 10.0f, 0.0f, false, true, {5.0f, 5.0f},
                         &fromOut, &arc),
            "arcToCubics: coincident endpoints degenerate to a line (F.6.2)");
    }

    // F.6.6: radii too small to span the chord are scaled UP until they fit,
    // which the spec requires rather than treating as an error. The arc must
    // then still reach its stated endpoint exactly.
    {
      const PathPoint from{0.0f, 0.0f}, to{40.0f, 0.0f};
      std::vector<Anchor> arc;
      PathPoint fromOut{};
      const bool built =
          arcToCubics(from, 5.0f, 5.0f, 0.0f, false, true, to, &fromOut, &arc);
      check(built, "arcToCubics: radii too small for the chord are scaled up, not refused");
      check(!arc.empty() && std::fabs(arc.back().pt.x - to.x) < 1.0e-3f &&
                std::fabs(arc.back().pt.y - to.y) < 1.0e-3f,
            "arcToCubics: and the scaled arc still lands on its endpoint");
    }
  }

  // --- 2. The differential oracle: agreement with selectPolygon() ----------
  {
    std::mt19937 rng(20260902u);
    std::uniform_real_distribution<float> coord(2.0f, 30.0f);
    const int32_t w = 32, h = 32;

    double worstDiff = 0.0;
    int polygons = 0;
    bool agree = true;

    for (int trial = 0; trial < 24; ++trial) {
      // Convex polygons for the first half, arbitrary (possibly non-convex,
      // possibly self-intersecting) for the second. `selectPolygon()` is
      // documented as NONZERO, so the comparison uses NonZero throughout and
      // self-intersection is a case both are expected to handle alike.
      const int n = 3 + (trial % 5);
      std::vector<PathPoint> pts;
      if (trial < 12) {
        // Convex by construction: sorted angles around a centre.
        const float cx = 16.0f, cy = 16.0f;
        std::vector<float> angles;
        for (int i = 0; i < n; ++i)
          angles.push_back(static_cast<float>(i) * 6.2831853f / static_cast<float>(n));
        for (int i = 0; i < n; ++i) {
          const float rr = 5.0f + static_cast<float>((trial * 7 + i * 3) % 9);
          pts.push_back(PathPoint{cx + rr * std::cos(angles[i]),
                                  cy + rr * std::sin(angles[i])});
        }
      } else {
        for (int i = 0; i < n; ++i) pts.push_back(PathPoint{coord(rng), coord(rng)});
      }

      std::vector<SelectionPoint> oraclePts;
      oraclePts.reserve(pts.size());
      for (const PathPoint& p : pts) oraclePts.push_back(SelectionPoint{p.x, p.y});
      const Selection oracle = selectPolygon(oraclePts);

      const std::vector<float> mine = rasterize(polygonPath(pts, FillRule::NonZero), w, h);
      ++polygons;

      for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
          const float theirs = selectionCoverageAt(&oracle, PixelCoord{x, y});
          const float ours = mine[static_cast<size_t>(y) * static_cast<size_t>(w) +
                                  static_cast<size_t>(x)];
          worstDiff = std::max(worstDiff, static_cast<double>(std::fabs(theirs - ours)));
        }
    }

    std::printf("  [measured] %d polygons vs selectPolygon(): worst texel diff %.5f\n",
                polygons, worstDiff);
    // 1/255 is not a taste tolerance: `SelectionTile` stores coverage as a
    // uint8 (core/SelectionMask.hpp), so the oracle's own answer is quantised
    // to that step before we can read it back. Two exact implementations
    // cannot be shown to agree more closely than the oracle can report, and a
    // tighter bound here would be measuring the quantiser. Anything larger
    // than one step is a real disagreement.
    agree = worstDiff <= (1.0 / 255.0) + 1.0e-6;
    check(agree, "rasteriser: agrees with selectPolygon() to the oracle's own 1/255 step");
  }

  // --- 3. Multiple contours: the hole the oracle cannot express ------------
  {
    // A square with a square hole, wound in OPPOSITE directions so nonzero
    // cancels in the middle. This is the case `selectPolygon()` structurally
    // cannot represent (one contour), and the reason a second rasteriser had
    // to be written at all.
    Path p;
    p.rule = FillRule::NonZero;
    auto addSquare = [&p](float x0, float y0, float x1, float y1, bool reverse) {
      SubPath sub;
      sub.closed = true;
      std::vector<PathPoint> pts{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
      if (reverse) std::reverse(pts.begin(), pts.end());
      for (const PathPoint& q : pts) {
        Anchor a;
        a.pt = a.in = a.out = q;
        sub.anchors.push_back(a);
      }
      p.subpaths.push_back(std::move(sub));
    };
    addSquare(4.0f, 4.0f, 28.0f, 28.0f, false);
    addSquare(12.0f, 12.0f, 20.0f, 20.0f, true);

    const std::vector<float> img = rasterize(p, 32, 32);
    auto at = [&](int32_t x, int32_t y) {
      return img[static_cast<size_t>(y) * 32u + static_cast<size_t>(x)];
    };
    check(at(6, 6) > 0.999f, "nonzero: outside the hole is solid");
    check(at(16, 16) < 1.0e-4f, "nonzero: an opposite-wound inner contour is a hole");
    const double expect = 24.0 * 24.0 - 8.0 * 8.0;
    const double got = sumOf(img);
    std::printf("  [measured] square-with-hole area: expected %.1f, got %.4f\n", expect, got);
    check(std::fabs(got - expect) < 1.0e-3, "nonzero: hole area is exactly subtracted");
  }

  // --- 4. The fill rules genuinely differ ----------------------------------
  //
  // THE assertion that pins the algorithm. Two SAME-wound concentric squares:
  // nonzero sees winding 2 in the middle and fills it solid; even-odd sees 2
  // and leaves a hole. A saturating signed-area rasteriser -- the simpler
  // design core/PathRaster.hpp section 2 rejects -- produces the nonzero
  // answer for BOTH rules and would fail this and only this.
  {
    auto twoSquares = [](FillRule rule) {
      Path p;
      p.rule = rule;
      auto add = [&p](float x0, float y0, float x1, float y1) {
        SubPath sub;
        sub.closed = true;
        for (const PathPoint& q :
             std::vector<PathPoint>{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}) {
          Anchor a;
          a.pt = a.in = a.out = q;
          sub.anchors.push_back(a);
        }
        p.subpaths.push_back(std::move(sub));
      };
      add(4.0f, 4.0f, 28.0f, 28.0f);
      add(12.0f, 12.0f, 20.0f, 20.0f);  // SAME winding as the outer
      return p;
    };

    const std::vector<float> nz = rasterize(twoSquares(FillRule::NonZero), 32, 32);
    const std::vector<float> eo = rasterize(twoSquares(FillRule::EvenOdd), 32, 32);
    auto at = [](const std::vector<float>& img, int32_t x, int32_t y) {
      return img[static_cast<size_t>(y) * 32u + static_cast<size_t>(x)];
    };
    check(at(nz, 16, 16) > 0.999f, "nonzero: winding 2 fills solid");
    check(at(eo, 16, 16) < 1.0e-4f, "even-odd: winding 2 is a hole");
    check(at(nz, 6, 6) > 0.999f && at(eo, 6, 6) > 0.999f,
          "both rules: winding 1 fills solid");
    const double eoExpect = 24.0 * 24.0 - 8.0 * 8.0;
    check(std::fabs(sumOf(eo) - eoExpect) < 1.0e-3,
          "even-odd: total area matches the annulus exactly");
    check(std::fabs(sumOf(nz) - 24.0 * 24.0) < 1.0e-3,
          "nonzero: total area is the outer square, hole included");
  }

  // --- 5. Clipping: a shape larger than the clip still fills it ------------
  //
  // core/PathRaster.hpp section 3's named bug, in both directions. An edge
  // left of the clip must still open the fill; an edge right of it must not
  // truncate the fill at the last cell an edge happened to touch.
  {
    const Path big = polygonPath(
        {{-50.0f, -50.0f}, {100.0f, -50.0f}, {100.0f, 100.0f}, {-50.0f, 100.0f}},
        FillRule::NonZero);
    std::vector<float> img(16u * 16u, 0.0f);
    PathRasterScratch scratch;
    rasterizePath(big, 0.01f, RasterClip{0, 0, 16, 16}, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x)
                      img[static_cast<size_t>(y) * 16u + static_cast<size_t>(x)] =
                          cov[x - x0];
                  });
    bool allSolid = true;
    for (float v : img)
      if (v < 0.999f) allSolid = false;
    check(allSolid, "clip: a shape enclosing the whole clip fills every texel of it");

    // And the same shape offset so only its right portion overlaps the clip,
    // which exercises the left-clamp on its own.
    const Path straddle = polygonPath(
        {{-50.0f, -50.0f}, {8.5f, -50.0f}, {8.5f, 100.0f}, {-50.0f, 100.0f}},
        FillRule::NonZero);
    std::fill(img.begin(), img.end(), 0.0f);
    rasterizePath(straddle, 0.01f, RasterClip{0, 0, 16, 16}, scratch,
                  [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                    for (int32_t x = x0; x < x1; ++x)
                      img[static_cast<size_t>(y) * 16u + static_cast<size_t>(x)] =
                          cov[x - x0];
                  });
    check(img[8u * 16u + 0u] > 0.999f, "clip: left-clamped edge still fills column 0");
    check(std::fabs(img[8u * 16u + 8u] - 0.5f) < 1.0e-3,
          "clip: the partially covered column keeps its exact half coverage");
    check(img[8u * 16u + 9u] < 1.0e-4f, "clip: nothing is filled past the edge");
  }

  // --- 6. Refusals and degenerate input ------------------------------------
  //
  // Untrusted-input guards. io/SvgImport will hand this path model
  // coordinates parsed out of a file this build did not write, so "refuses
  // cleanly" is a contract, not a nicety. A NaN in a scanline rasteriser does
  // not make a wrong pixel; it makes a loop bound compare false both ways.
  {
    Path nan;
    nan.subpaths.push_back(SubPath{});
    Anchor a;
    a.pt = a.in = a.out = PathPoint{std::nanf(""), 4.0f};
    nan.subpaths[0].anchors.push_back(a);
    a.pt = a.in = a.out = PathPoint{8.0f, 8.0f};
    nan.subpaths[0].anchors.push_back(a);
    nan.subpaths[0].closed = true;
    check(!pathIsFinite(nan), "pathIsFinite: rejects a NaN coordinate");
    check(flattenPath(nan, 0.1f).empty(), "flatten: refuses a non-finite path, emitting nothing");

    Path inf = nan;
    inf.subpaths[0].anchors[0].pt = PathPoint{4.0f, 4.0f};
    inf.subpaths[0].anchors[0].in = PathPoint{4.0f, 4.0f};
    inf.subpaths[0].anchors[0].out =
        PathPoint{std::numeric_limits<float>::infinity(), 4.0f};
    check(!pathIsFinite(inf), "pathIsFinite: rejects an infinite HANDLE, not just an anchor");

    Path empty;
    check(pathIsEmpty(empty), "pathIsEmpty: no subpaths");
    empty.subpaths.push_back(SubPath{});
    empty.subpaths[0].anchors.push_back(a);
    check(pathIsEmpty(empty), "pathIsEmpty: a single anchor encloses nothing");
    check(rasterize(empty, 8, 8).size() == 64u, "rasteriser: a degenerate path emits no spans");
    check(sumOf(rasterize(empty, 8, 8)) == 0.0, "rasteriser: and therefore no coverage");

    // A zero-area closed triangle: not "empty" by the model's own definition
    // (it has three anchors), and correctly produces no coverage.
    Path degenerate;
    SubPath tri;
    tri.closed = true;
    for (int i = 0; i < 3; ++i) {
      Anchor t;
      t.pt = t.in = t.out = PathPoint{5.0f, 5.0f};
      tri.anchors.push_back(t);
    }
    degenerate.subpaths.push_back(std::move(tri));
    check(!pathIsEmpty(degenerate), "pathIsEmpty: three coincident anchors are not 'empty'");
    check(sumOf(rasterize(degenerate, 16, 16)) < 1.0e-6,
          "rasteriser: but they cover nothing");
  }

  // --- 7. The flattener's segment count is bounded and tolerance-driven ----
  {
    PathPoint straight[4] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}, {30.0f, 0.0f}};
    check(cubicSegmentCount(straight, 0.1f) == 1,
          "flatten: an exactly straight cubic needs one segment");

    PathPoint curved[4] = {{0.0f, 0.0f}, {0.0f, 100.0f}, {100.0f, 100.0f}, {100.0f, 0.0f}};
    const size_t coarse = cubicSegmentCount(curved, 1.0f);
    const size_t fine = cubicSegmentCount(curved, 0.01f);
    std::printf("  [measured] cubic segments at tol 1.0 / 0.01 px: %zu / %zu\n", coarse, fine);
    check(fine > coarse, "flatten: a tighter tolerance asks for more segments");

    // Translation invariance -- the property a chord-distance flatness test
    // does not give you, and the reason core/PathFlatten uses a second
    // difference. A path exported at a large offset must not cost more.
    PathPoint far[4];
    for (int i = 0; i < 4; ++i)
      far[i] = PathPoint{curved[i].x + 1.0e5f, curved[i].y + 1.0e5f};
    check(cubicSegmentCount(far, 0.01f) == fine,
          "flatten: segment count is invariant under a large translation");

    // A caller passing zero, or coordinates far apart enough to ask for
    // millions of segments, must be clamped rather than allowed to allocate.
    PathPoint huge[4] = {{0.0f, 0.0f}, {0.0f, 1.0e9f}, {1.0e9f, 1.0e9f}, {1.0e9f, 0.0f}};
    check(cubicSegmentCount(huge, 0.0f) == kMaxSegmentsPerCurve,
          "flatten: a zero tolerance clamps rather than allocating unboundedly");
    check(cubicSegmentCount(curved, -1.0f) >= 1,
          "flatten: a negative tolerance is treated as the floor, not as a sign");
  }

  // --- 8. Tight bounds are tighter than hull bounds, and still contain ------
  {
    // A cubic whose handles swing far outside the curve: the hull bound is
    // visibly loose and the tight bound must not be.
    Path p;
    SubPath sub;
    Anchor a0, a1;
    a0.pt = PathPoint{10.0f, 10.0f};
    a0.in = a0.pt;
    a0.out = PathPoint{10.0f, 90.0f};
    a1.pt = PathPoint{50.0f, 10.0f};
    a1.in = PathPoint{50.0f, 90.0f};
    a1.out = a1.pt;
    sub.anchors = {a0, a1};
    sub.closed = false;
    p.subpaths.push_back(std::move(sub));

    const PathBounds hull = pathControlBounds(p);
    const PathBounds tight = pathTightBounds(p);
    check(hull.valid && tight.valid, "bounds: both are valid for a real curve");
    // The curve's own maximum y is at t=0.5: 0.125*10 + 0.375*90 + 0.375*90 +
    // 0.125*10 = 70. The hull says 90.
    std::printf("  [measured] hull maxY %.3f vs tight maxY %.3f (curve peaks at 70)\n",
                hull.maxY, tight.maxY);
    check(std::fabs(tight.maxY - 70.0f) < 1.0e-3, "bounds: tight bound finds the curve's true peak");
    check(hull.maxY > tight.maxY + 1.0f, "bounds: the hull bound is genuinely looser");
    check(tight.minX >= hull.minX - 1.0e-3f && tight.maxX <= hull.maxX + 1.0e-3f,
          "bounds: the tight bound is contained in the hull bound");
  }

  // --- 9. moveAnchorTo carries the handles ---------------------------------
  //
  // core/Path.hpp section 2's stated cost, paid in one place. If this ever
  // stops being true the symptom is handles drifting away from their anchor
  // under a drag, which reads as a rounding bug rather than a missing update.
  {
    Anchor a;
    a.pt = PathPoint{10.0f, 10.0f};
    a.in = PathPoint{5.0f, 10.0f};
    a.out = PathPoint{15.0f, 12.0f};
    moveAnchorTo(a, PathPoint{20.0f, 30.0f});
    check(a.pt.x == 20.0f && a.pt.y == 30.0f, "moveAnchorTo: the anchor lands exactly");
    check(a.in.x == 15.0f && a.in.y == 30.0f, "moveAnchorTo: the in handle keeps its offset");
    check(a.out.x == 25.0f && a.out.y == 32.0f, "moveAnchorTo: the out handle keeps its offset");
  }


  // --- 10. The stroker ------------------------------------------------------
  //
  // Every expected area below is derived on paper first and written into the
  // check, never read off a run and pasted back. That distinction is the
  // whole value of the section: an area "tolerance" fitted to whatever the
  // code currently prints asserts only that the code is deterministic.
  //
  // The tolerance throughout is 2e-2 absolute on areas of order 200-700. It
  // is bounded by the flattener and the round-join chord count (both set to
  // 0.01 px here), not chosen -- a join or cap that was wrong by even a
  // fraction of the stroke width moves these by whole units.
  {
    auto strokeAreaOf = [&](const Path& p, const StrokeStyle& st, int32_t w,
                            int32_t h) {
      return sumOf(rasterize(strokePath(p, st, 0.01f), w, h));
    };

    // An open horizontal line: the simplest possible stroke, and the one that
    // isolates the caps because it has no joins at all.
    Path line;
    {
      SubPath sub;
      sub.closed = false;
      for (const PathPoint& q : {PathPoint{10.0f, 20.0f}, PathPoint{40.0f, 20.0f}}) {
        Anchor a;
        a.pt = a.in = a.out = q;
        sub.anchors.push_back(a);
      }
      line.subpaths.push_back(std::move(sub));
    }
    const float L = 30.0f, W = 6.0f, H = W * 0.5f;

    StrokeStyle butt;
    butt.width = W;
    butt.cap = LineCap::Butt;
    const double buttArea = strokeAreaOf(line, butt, 64, 40);
    std::printf("  [measured] butt-cap line area: expected %.3f, got %.3f\n",
                static_cast<double>(L * W), buttArea);
    check(std::fabs(buttArea - L * W) < 2.0e-2,
          "stroke: a butt-capped line covers exactly length * width");

    StrokeStyle square = butt;
    square.cap = LineCap::Square;
    const double squareExpect = (L + W) * W;  // half a width added at each end
    const double squareArea = strokeAreaOf(line, square, 64, 40);
    check(std::fabs(squareArea - squareExpect) < 2.0e-2,
          "stroke: a square cap adds exactly half a width at each end");

    StrokeStyle round = butt;
    round.cap = LineCap::Round;
    const double roundExpect = L * W + 3.14159265358979 * H * H;  // two halves = one disc
    const double roundArea = strokeAreaOf(line, round, 64, 40);
    // A round cap is drawn as chords at the same 0.01 px tolerance the
    // flattener uses, so the arc is INSCRIBED and the area is always a little
    // SHORT of the true disc -- never over. The deficit is bounded rather
    // than guessed: every chord lies between the circle of radius r and that
    // of radius r - tol, so the whole disc loses at most
    // pi*r^2 - pi*(r-tol)^2 = pi*tol*(2r - tol).
    //
    // Asserted one-sided on purpose. An earlier revision used a flat 2e-2
    // here and failed against correct output, because it had quietly assumed
    // a chord approximation was area-preserving.
    const double capBound = 3.14159265358979 * 0.01 * (2.0 * H - 0.01);
    const double capDeficit = roundExpect - roundArea;
    std::printf("  [measured] round-cap line area: expected %.3f, got %.3f "
                "(deficit %.4f, bound %.4f)\n",
                roundExpect, roundArea, capDeficit, capBound);
    check(capDeficit >= -2.0e-2 && capDeficit < capBound,
          "stroke: two round caps add one disc, short only by the inscribed-chord bound");

    // A closed square, which isolates the JOINS because it has no caps.
    // Stroking a side-s square with width w gives an outer square of side
    // s + w and an inner hole of side s - w, so the band is
    // (s+w)^2 - (s-w)^2 = 4 s w -- independent of the join style only for
    // miter, which is why the three styles below differ by known amounts.
    Path square20;
    {
      SubPath sub;
      sub.closed = true;
      for (const PathPoint& q : {PathPoint{10.0f, 10.0f}, PathPoint{30.0f, 10.0f},
                                 PathPoint{30.0f, 30.0f}, PathPoint{10.0f, 30.0f}}) {
        Anchor a;
        a.pt = a.in = a.out = q;
        sub.anchors.push_back(a);
      }
      square20.subpaths.push_back(std::move(sub));
    }
    const float s = 20.0f, w = 4.0f, hh = w * 0.5f;

    StrokeStyle miter;
    miter.width = w;
    miter.join = LineJoin::Miter;
    const double miterExpect = 4.0 * s * w;
    const double miterArea = strokeAreaOf(square20, miter, 48, 48);
    std::printf("  [measured] mitred square band: expected %.3f, got %.3f\n",
                miterExpect, miterArea);
    check(std::fabs(miterArea - miterExpect) < 2.0e-2,
          "stroke: a mitred square band is exactly 4*s*w");

    // A bevel replaces each corner's full h-by-h square with the triangle
    // under its diagonal, losing h^2/2 per corner at a right angle.
    StrokeStyle bevel = miter;
    bevel.join = LineJoin::Bevel;
    const double bevelExpect = miterExpect - 4.0 * (hh * hh * 0.5);
    const double bevelArea = strokeAreaOf(square20, bevel, 48, 48);
    std::printf("  [measured] bevelled square band: expected %.3f, got %.3f\n",
                bevelExpect, bevelArea);
    check(std::fabs(bevelArea - bevelExpect) < 2.0e-2,
          "stroke: a bevel join loses exactly h^2/2 per right-angle corner");

    // A round join replaces the same square with a quarter disc: four of them
    // make one disc of radius h.
    StrokeStyle roundJoin = miter;
    roundJoin.join = LineJoin::Round;
    const double roundJoinExpect =
        miterExpect - 4.0 * hh * hh + 3.14159265358979 * hh * hh;
    const double roundJoinArea = strokeAreaOf(square20, roundJoin, 48, 48);
    // Same inscribed-chord bound as the round cap above, for the one disc the
    // four quarter-joins add up to.
    const double joinBound = 3.14159265358979 * 0.01 * (2.0 * hh - 0.01);
    const double joinDeficit = roundJoinExpect - roundJoinArea;
    std::printf("  [measured] round-joined square band: expected %.3f, got %.3f "
                "(deficit %.4f, bound %.4f)\n",
                roundJoinExpect, roundJoinArea, joinDeficit, joinBound);
    check(joinDeficit >= -2.0e-2 && joinDeficit < joinBound,
          "stroke: four round joins make one disc, short only by the inscribed-chord bound");
    check(bevelArea < roundJoinArea && roundJoinArea < miterArea,
          "stroke: the three join styles are strictly ordered bevel < round < miter");

    // The miter limit. A very sharp spike would give a miter length of
    // h/cos(theta/2) -- unbounded as the turn approaches 180 degrees. SVG
    // requires a bevel past `stroke-miterlimit`, and without that fallback
    // the stroke grows a long spike whose area is far larger.
    Path spike;
    {
      SubPath sub;
      sub.closed = false;
      for (const PathPoint& q : {PathPoint{10.0f, 40.0f}, PathPoint{60.0f, 41.0f},
                                 PathPoint{10.0f, 42.0f}}) {
        Anchor a;
        a.pt = a.in = a.out = q;
        sub.anchors.push_back(a);
      }
      spike.subpaths.push_back(std::move(sub));
    }
    StrokeStyle tight = miter;
    tight.width = 4.0f;
    tight.miterLimit = 1.0f;  // effectively "always bevel"
    StrokeStyle loose = tight;
    loose.miterLimit = 1000.0f;
    const double tightArea = strokeAreaOf(spike, tight, 128, 96);
    const double looseArea = strokeAreaOf(spike, loose, 128, 96);
    std::printf("  [measured] spike area at miterlimit 1 vs 1000: %.2f vs %.2f\n",
                tightArea, looseArea);
    check(looseArea > tightArea + 10.0,
          "stroke: a permissive miter limit really does grow a spike");
    check(tightArea < looseArea,
          "stroke: miterlimit 1 falls back to bevel and bounds the join");

    // Dashing. A 40-long line under a 4-on/4-off pattern is exactly five
    // cycles, so exactly half of it is drawn -- an integer number of dashes,
    // chosen so the expected area needs no partial-dash reasoning.
    Path line40;
    {
      SubPath sub;
      sub.closed = false;
      for (const PathPoint& q : {PathPoint{5.0f, 20.0f}, PathPoint{45.0f, 20.0f}}) {
        Anchor a;
        a.pt = a.in = a.out = q;
        sub.anchors.push_back(a);
      }
      line40.subpaths.push_back(std::move(sub));
    }
    StrokeStyle dashed;
    dashed.width = 6.0f;
    dashed.cap = LineCap::Butt;
    dashed.dashes = {4.0f, 4.0f};
    const double dashExpect = 20.0 * 6.0;  // five 4-long dashes
    const double dashArea = strokeAreaOf(line40, dashed, 64, 40);
    std::printf("  [measured] dashed line area: expected %.3f, got %.3f\n",
                dashExpect, dashArea);
    check(std::fabs(dashArea - dashExpect) < 2.0e-2,
          "stroke: a 4-on/4-off dash over 40 units draws exactly half of it");

    // The dash walk on its own, where the state is.
    {
      FlatContour c;
      c.closed = false;
      c.points = {{0.0f, 0.0f}, {40.0f, 0.0f}};
      check(dashContour(c, {4.0f, 4.0f}, 0.0f).size() == 5,
            "dash: five on-runs across forty units of 4-on/4-off");
      // An offset of one full dash starts in the OFF phase, so the first run
      // is clipped away and only four full dashes remain in the same length.
      check(dashContour(c, {4.0f, 4.0f}, 4.0f).size() == 5,
            "dash: a 4-unit offset shifts the phase without losing a run");
      // SVG repeats an odd-length array so on/off alternation is defined.
      check(dashContour(c, {5.0f}, 0.0f).size() == 4,
            "dash: an odd-length array is repeated, giving 5-on/5-off");
      // A closed contour dashes around its closing edge too -- forgetting
      // that leaves a permanent gap at the seam of every dashed circle.
      FlatContour box;
      box.closed = true;
      box.points = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}};
      const size_t closedRuns = dashContour(box, {2.0f, 2.0f}, 0.0f).size();
      const size_t openRuns = dashContour(
          FlatContour{{{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}}, false},
          {2.0f, 2.0f}, 0.0f).size();
      check(closedRuns > openRuns,
            "dash: a closed contour dashes its closing edge, an open one does not");
    }

    // Degenerate and hostile input.
    StrokeStyle zero = miter;
    zero.width = 0.0f;
    check(strokePath(square20, zero, 0.01f).subpaths.empty(),
          "stroke: zero width draws nothing (SVG's rule), rather than refusing");
    StrokeStyle negative = miter;
    negative.width = -3.0f;
    check(strokePath(square20, negative, 0.01f).subpaths.empty(),
          "stroke: a negative width draws nothing rather than a mirrored band");
    StrokeStyle zeroDash = miter;
    zeroDash.dashes = {0.0f, 0.0f};
    // The hang, not the wrong picture: an all-zero pattern advances the dash
    // walk by nothing, so it must be treated as solid rather than looped over.
    check(!strokePath(square20, zeroDash, 0.01f).subpaths.empty(),
          "stroke: an all-zero dash array is solid, and terminates");
    check(strokePath(square20, miter, 0.01f).rule == FillRule::NonZero,
          "stroke: the result is always NonZero, whatever the source rule was");
  }

  return ok;
}

}  // namespace np
