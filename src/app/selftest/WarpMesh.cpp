#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/TransformSession.hpp"
#include "app/WarpMesh.hpp"

namespace np {

// app/WarpMesh + app/TransformSession's Warp mode (PRD D23).
// Headless and GPU-free. See app/WarpMesh.hpp for the model this proves and
// app/TransformSession.hpp section 9 for how a session's Warp mode uses it.
//
// Replaces the prior bicubic-Bezier net's own selftest (see git history):
// two cases below (the old "anchor rides its handles" drag case and the old
// "warped shape stays inside the box" SelectionPixels case) asserted
// properties that were true ONLY of a Bezier control net -- a handle/anchor
// distinction that no longer exists, and the convex-hull containment a
// Bezier patch has and a Catmull-Rom spline does not. Both are replaced with
// tests of what IS true of this net, not carried over unexamined.
bool runWarpMeshTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] warp mesh: Catmull-Rom lattice model, rasteriser and session wiring, no GPU\n");

  const DocumentRegion bounds{0, 0, 40u, 20u};

  // --- 1. flat() reproduces the rectangle exactly ---------------------------
  {
    const WarpMesh m = WarpMesh::flat(bounds, 4);
    check(m.n() == 4, "flat() keeps the requested grid size");
    check(m.pointsPerSide() == 4, "n() IS the point count per side now (was 3n+1 for Bezier)");
    const float c = static_cast<float>(m.cells());  // 3
    const Point2 tl = m.evaluate(0.0f, 0.0f);
    const Point2 br = m.evaluate(c, c);
    const Point2 mid = m.evaluate(c * 0.5f, c * 0.5f);
    check(tl.x == 0.0f && tl.y == 0.0f, "flat net's corner evaluates to the rectangle's own corner");
    check(br.x == 40.0f && br.y == 20.0f, "flat net's opposite corner matches too");
    // Tolerance, not bit equality: unlike the Bernstein basis at t=0/1, the
    // Catmull-Rom weights at a fractional t (here 0.5) are irrational-looking
    // binary fractions (-1/16, 9/16, 9/16, -1/16) whose float rounding does
    // not happen to land back on an exact 20.0/10.0 for every bounds/grid
    // combination, even though the math is exact.
    check(std::fabs(mid.x - 20.0f) < 1e-3f && std::fabs(mid.y - 10.0f) < 1e-3f,
          "flat net's centre matches the rectangle's centre");
    check(m.isAffine(), "a flat net is affine");
    check(m.isIdentity(), "a freshly-built flat net is its own identity");
  }

  // --- 2. Hit-test: every control point is a plain, equal point -------------
  // Unlike the Bezier net, there is no anchor/handle distinction to test --
  // every one of the n*n points is the same kind of thing.
  {
    const WarpMesh m = WarpMesh::flat(bounds, 3);
    check(m.pointsPerSide() == 3, "a 3x3 grid choice really is 3 points per side now");
    const Point2 corner = m.at(0, 0);
    const WarpControlRef hitCorner = hitTestWarpControl(m, corner, 2.0f);
    check(hitCorner.valid && hitCorner.row == 0 && hitCorner.col == 0,
          "hit test on a control point's own position finds it");
    const Point2 interior = m.at(1, 1);
    const WarpControlRef hitInterior = hitTestWarpControl(m, interior, 2.0f);
    check(hitInterior.valid && hitInterior.row == 1 && hitInterior.col == 1,
          "hit test on the interior point's own position finds it too, same as any other point");
    const WarpControlRef hitNothing = hitTestWarpControl(m, Point2{1000.0f, 1000.0f}, 2.0f);
    check(!hitNothing.valid, "hit test far from every control point finds nothing");
  }

  // --- 3. Dragging one control point moves only that point -------------------
  // The direct replacement for the old "anchor carries its handles" case:
  // under Catmull-Rom there is nothing else FOR a drag to carry along, since
  // every other point's own stored position is untouched by construction
  // (only the TANGENTS derived through it change, at evaluate() time, not
  // its stored coordinate) -- see case 4 for that continuity claim proven
  // directly rather than just asserted here.
  {
    WarpMesh m = WarpMesh::flat(bounds, 4);
    const int side = m.pointsPerSide();
    std::vector<Point2> before(static_cast<size_t>(side) * side);
    for (int row = 0; row < side; ++row)
      for (int col = 0; col < side; ++col) before[static_cast<size_t>(row) * side + col] = m.at(row, col);

    m.dragControl(WarpControlRef{true, 1, 2}, Point2{5.0f, -2.0f});

    bool onlyOneMoved = true;
    for (int row = 0; row < side && onlyOneMoved; ++row) {
      for (int col = 0; col < side; ++col) {
        const Point2 b = before[static_cast<size_t>(row) * side + col];
        const Point2 a = m.at(row, col);
        const bool isDragged = (row == 1 && col == 2);
        const bool moved = (a.x != b.x || a.y != b.y);
        if (moved != isDragged) {
          onlyOneMoved = false;
          break;
        }
      }
    }
    check(onlyOneMoved, "dragging one point moves exactly that point's stored coordinate, no other");
    const Point2 dragged = m.at(1, 2);
    const Point2 orig = before[1u * static_cast<size_t>(side) + 2u];
    check(dragged.x == orig.x + 5.0f && dragged.y == orig.y - 2.0f,
          "the dragged point moved by exactly delta");
  }

  // --- 4. C1 continuity across a cell boundary, proven directly --------------
  // The whole point of switching to Catmull-Rom (per app/WarpMesh.hpp's own
  // header): two adjacent cells derive their SHARED boundary's tangent from
  // the SAME pair of neighbouring grid points, so they can never disagree --
  // structurally, not by a synchronisation step that could be forgotten.
  //
  // Checked with an EXACT analytic partial derivative, independently
  // re-derived and re-typed here rather than reused from WarpMesh.cpp (this
  // project's own "test asks the broken oracle" lesson: sharing the
  // implementation's own weight formula would let a shared bug cancel out of
  // both sides equally and still "match"). A finite-difference probe was
  // tried first and produced float-precision noise an order of magnitude
  // larger than the effect being measured -- see git history on this file --
  // which is itself the reason to prefer an exact derivative here over a
  // numerical one.
  {
    // Independent re-derivation of the weight DERIVATIVE, from the same
    // P(t) = 0.5*[2P1 + (-P0+P2)t + (2P0-5P1+4P2-P3)t^2 + (-P0+3P1-3P2+P3)t^3]
    // this header's own comment states, differentiated term by term -- typed
    // fresh, not copy-pasted from WarpMesh.cpp's `catmullRomWeights()`.
    auto crWeightDeriv = [](float t) -> std::array<float, 4> {
      const float t2 = t * t;
      return {-0.5f + 2.0f * t - 1.5f * t2, -5.0f * t + 4.5f * t2, 0.5f + 4.0f * t - 4.5f * t2,
              -1.0f * t + 1.5f * t2};
    };
    auto crWeight = [](float t) -> std::array<float, 4> {
      const float t2 = t * t, t3 = t2 * t;
      return {-0.5f * t + t2 - 0.5f * t3, 1.0f - 2.5f * t2 + 1.5f * t3,
             0.5f * t + 2.0f * t2 - 1.5f * t3, -0.5f * t2 + 0.5f * t3};
    };
    // dP/du at (u0, v0) using the 4x4 stencil anchored at cell (cellI, cellJ)
    // with local U-parameter `tLocal` -- `mesh.extendedAt()` supplies the
    // boundary phantom the same way the production surface does, since this
    // probe needs to work at an interior boundary either way (it does not
    // here, `u0` sits well inside the net).
    auto analyticDu = [&](const WarpMesh& mesh, int cellI, int cellJ, float tLocal,
                          float vLocal) -> Point2 {
      const std::array<float, 4> dwa = crWeightDeriv(tLocal);
      const std::array<float, 4> wb = crWeight(vLocal);
      float dx = 0.0f, dy = 0.0f;
      for (int b = 0; b < 4; ++b) {
        for (int a = 0; a < 4; ++a) {
          const Point2 p = mesh.extendedAt(cellJ - 1 + b, cellI - 1 + a);
          const float w = dwa[static_cast<size_t>(a)] * wb[static_cast<size_t>(b)];
          dx += w * p.x;
          dy += w * p.y;
        }
      }
      return Point2{dx, dy};
    };

    WarpMesh m = WarpMesh::flat(DocumentRegion{0, 0, 200u, 100u}, 5);  // cells 0..3, boundary at u=2
    // Bend it off-plane first so the two sides are really curved, not flat
    // (a flat net's C1 match would be true for a trivial reason).
    m.dragControl(WarpControlRef{true, 1, 1}, Point2{-6.0f, 9.0f});
    m.dragControl(WarpControlRef{true, 2, 3}, Point2{8.0f, -5.0f});

    const int boundaryCol = 2;  // shared by cell I=1 (ends here) and cell I=2 (starts here)
    const float v0 = 1.3f;      // off-integer in V, so a real bicubic blend is exercised
    const int cellJ = 1;        // floor(1.3) == 1
    const float vLocal = v0 - static_cast<float>(cellJ);

    auto boundaryMismatch = [&](const WarpMesh& mesh) {
      const Point2 leftTangent = analyticDu(mesh, boundaryCol - 1, cellJ, 1.0f, vLocal);
      const Point2 rightTangent = analyticDu(mesh, boundaryCol, cellJ, 0.0f, vLocal);
      return std::hypot(leftTangent.x - rightTangent.x, leftTangent.y - rightTangent.y);
    };
    const float mismatchBefore = boundaryMismatch(m);
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "boundary tangent matches to %.6f px/unit (float roundoff only) before any "
                  "further drag",
                  mismatchBefore);
    check(mismatchBefore < 1e-3f, buf);

    // Now move ONE of the two points both sides' tangents are derived from
    // (column 1, a neighbour of the boundary column 2, shared by cell I=1's
    // right side and cell I=2's left side) -- the exact case the header
    // argues can never desync.
    m.dragControl(WarpControlRef{true, 3, 1}, Point2{15.0f, -11.0f});
    const float mismatchAfter = boundaryMismatch(m);
    std::snprintf(buf, sizeof(buf),
                  "boundary tangent STILL matches to %.6f px/unit after moving a point both "
                  "sides derive their tangent from",
                  mismatchAfter);
    check(mismatchAfter < 1e-3f, buf);
  }

  // --- 5. Chord-error bound on a strongly bent net ---------------------------
  {
    WarpMesh m = WarpMesh::flat(DocumentRegion{0, 0, 100u, 100u}, 3);
    // Bend the interior point far out of plane -- a strong bulge.
    m.dragControl(WarpControlRef{true, 1, 1}, Point2{0.0f, -60.0f});
    const int sub = warpChordSubdivisions(m, 0.5f);
    check(sub >= 1 && sub <= kMaxSubdivisionsPerCell, "subdivision count stays in its bounded range");

    // Sample the true surface between tessellation nodes and measure how far
    // it strays from each chord. Parameters run 0..cells(), and the bent
    // point sits at (u,v) = (1,1).
    const float tol = 0.5f;
    const int cells = m.cells();  // 2
    auto worstChordDeviation = [&](int segmentsPerCell) {
      const int probesPerSeg = 9;
      float worst = 0.0f;
      for (int cell = 0; cell < cells; ++cell) {
        for (int a = 0; a < segmentsPerCell; ++a) {
          const float u0 = cell + static_cast<float>(a) / segmentsPerCell;
          const float u1 = cell + static_cast<float>(a + 1) / segmentsPerCell;
          const Point2 segA = m.evaluate(u0, 1.0f);
          const Point2 segB = m.evaluate(u1, 1.0f);
          const float ex = segB.x - segA.x, ey = segB.y - segA.y;
          const float len2 = ex * ex + ey * ey;
          for (int p = 1; p < probesPerSeg; ++p) {
            const Point2 truePoint = m.evaluate(u0 + (u1 - u0) * p / probesPerSeg, 1.0f);
            float proj = len2 > 1e-9f
                             ? ((truePoint.x - segA.x) * ex + (truePoint.y - segA.y) * ey) / len2
                             : 0.0f;
            proj = std::clamp(proj, 0.0f, 1.0f);
            worst = std::max(worst, std::hypot(truePoint.x - (segA.x + proj * ex),
                                               truePoint.y - (segA.y + proj * ey)));
          }
        }
      }
      return worst;
    };
    check(worstChordDeviation(1) > tol,
          "the probed iso-line really bends: one chord per cell misses it by more than the bound");
    const float worst = worstChordDeviation(sub);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "chord deviation %.4f px stays under the %.2f px bound", worst,
                 tol);
    check(worst < tol, buf);
  }

  // --- 6. Corner control-point drag lands that texel where evaluation says --
  {
    OpenDocument od = makeBlankOpenDocument(40, 40, WorkingSpace{});
    // A distinctive marker in the corner cell so a mis-mapped corner is
    // obvious rather than statistical.
    for (int32_t y = 0; y < 40; ++y) {
      for (int32_t x = 0; x < 40; ++x) {
        const float v = (x < 20 && y < 20) ? 1.0f : 0.25f;
        od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, v, v, 1.0f});
      }
    }
    od.recordEdit("fixture", EditKind::Content);
    TransformSession session;
    const TransformBeginResult began = session.beginLayer(od, 0);
    check(began.ok, "corner-drag fixture: beginLayer() succeeds");
    session.setWarpMode(true, 3);
    check(session.mode() == TransformMode::Warp, "setWarpMode(true) switches the session to Warp");
    // Drag the mesh's own top-left point (document corner 0,0) inward and
    // down by (8, 8) -- the corner texel should now warp to land near (8, 8).
    const WarpControlRef corner{true, 0, 0};
    session.warpBeginDrag(corner, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{8.0f, 8.0f});
    session.warpEndDrag();
    const Point2 predicted = session.warpMesh().evaluate(0.0f, 0.0f);
    check(std::fabs(predicted.x - 8.0f) < 1e-3f && std::fabs(predicted.y - 8.0f) < 1e-3f,
          "the surface's own evaluate() at the dragged corner matches the drop point");

    const TransformCommitResult done = session.commit(od);
    check(done.ok, "corner-drag warp commits");
    if (done.ok) {
      // The rendered corner texel (a pixel centre very close to (8,8)) should
      // read the marker colour that used to sit at the untouched corner.
      const TileCoord tc = tileCoordAt(PixelCoord{8, 8});
      const Tile* tile = od.document.layers[0].rgbTiles->find(tc);
      bool cornerLanded = false;
      if (tile != nullptr) {
        const std::array<float, 4> px = tile->readPixel(tileLocalOffset(PixelCoord{8, 8}));
        cornerLanded = px[0] > 0.5f;  // the corner cell's marker was 1.0, background 0.25
      }
      check(cornerLanded, "the corner cell's own colour is what landed at the predicted texel");
    }
  }

  // --- 7. Interior texel at a known (u,v) matches evaluation ----------------
  {
    OpenDocument od = makeBlankOpenDocument(60, 60, WorkingSpace{});
    for (int32_t y = 0; y < 60; ++y) {
      for (int32_t x = 0; x < 60; ++x) {
        const float fx = static_cast<float>(x) / 60.0f, fy = static_cast<float>(y) / 60.0f;
        const float v = 0.5f + 0.3f * std::sin(fx * 12.0f) * std::cos(fy * 9.0f);
        od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, v, v, 1.0f});
      }
    }
    od.recordEdit("fixture", EditKind::Content);
    TransformSession session;
    session.beginLayer(od, 0);
    session.setWarpMode(true, 3);
    // A 3x3 grid's ONE interior point, at parameter (1,1) -- the direct
    // Catmull-Rom equivalent of the old Bezier test's interior anchor.
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{10.0f, -6.0f});
    session.warpEndDrag();
    const WarpMesh mesh = session.warpMesh();
    const Point2 dstPoint = mesh.evaluate(1.0f, 1.0f);
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "interior-texel fixture: warp commits");
    if (done.ok) {
      const int32_t ix = static_cast<int32_t>(std::floor(dstPoint.x));
      const int32_t iy = static_cast<int32_t>(std::floor(dstPoint.y));
      // Source point for (u,v)=(1,1) over a 2-cell (3-point), 60x60 mesh:
      // exactly the source centre, (30, 30) -- the fixture's own known value
      // there (a Catmull-Rom net's grid points always land exactly on the
      // surface, same interpolation property the old Bezier anchors had).
      const float srcFx = 30.0f / 60.0f, srcFy = 30.0f / 60.0f;
      const float expected = 0.5f + 0.3f * std::sin(srcFx * 12.0f) * std::cos(srcFy * 9.0f);
      const TileCoord tc = tileCoordAt(PixelCoord{ix, iy});
      const Tile* tile = od.document.layers[0].rgbTiles->find(tc);
      float got = -1.0f;
      if (tile != nullptr) got = tile->readPixel(tileLocalOffset(PixelCoord{ix, iy}))[0];
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "interior texel at evaluated (u,v) reads %.4f, expected %.4f (bilinear "
                    "kernel tolerance 0.05)",
                    got, expected);
      check(tile != nullptr && std::fabs(got - expected) < 0.05f, buf);
    }
  }

  // --- 8. SelectionPixels warp leaves everything outside its computed
  //        region bit-identical -----------------------------------------
  // The direct replacement for the old convex-hull argument. A Bezier patch
  // stays inside the hull of its own control points, so the old test could
  // check a small, FIXED box. A Catmull-Rom surface can overshoot past its
  // own neighbouring points (this file's header, and the task that landed
  // it, both name this tradeoff explicitly) -- so instead this proves the
  // one guarantee that holds regardless of which interpolation basis
  // computes the surface: `warpRgbTiles()`/`warpImage()` only ever write
  // texels inside `warpedRegion()`'s own computed footprint, by
  // construction (the fast identity path aside, not exercised here since
  // this net is deliberately bent). Everything strictly outside that
  // region -- wherever it ends up -- is untouched.
  {
    auto fillRgb = [](TileStore& store, int32_t w, int32_t h) {
      for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
          const float fx = static_cast<float>(x) / static_cast<float>(w);
          const float fy = static_cast<float>(y) / static_cast<float>(h);
          const float v = 0.5f + 0.25f * std::sin(fx * 18.0f) * std::cos(fy * 14.0f);
          store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
              .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, 0.5f * v, 1.0f - v, 1.0f});
        }
      }
    };
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 64, 64);
    od.recordEdit("fixture", EditKind::Content);
    const TileStore before = *od.document.layers[0].rgbTiles;

    const Selection sel = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);
    TransformSession session;
    const TransformBeginResult began = session.beginSelectionPixels(od, sel, 0);
    check(began.ok, "selection-pixels fixture: beginSelectionPixels() succeeds");
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{-2.0f, 1.5f});
    session.warpEndDrag();
    // The same region-computation `commit()` itself uses internally
    // (mirrored here the way `app/ProfileVectorWarp.cpp` already does it),
    // so this test's notion of "the warp's own footprint" is not a
    // hand-picked box but the actual thing `warpRgbTiles()` bounds itself
    // to.
    const WarpMesh meshAfterDrag = session.warpMesh();
    const DocumentRegion footprint =
        warpedRegion(meshAfterDrag, warpChordSubdivisions(meshAfterDrag));
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "selection-pixels warp commits");

    if (done.ok) {
      const TransformImage after =
          imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 64, 64);
      const TransformImage beforeImg = imageFromTileStore(before, 0, 0, 64, 64);
      bool outsideUntouched = true;
      int32_t outsideProbed = 0;
      for (int32_t y = 0; y < 64 && outsideUntouched; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
          const bool insideFootprint = x >= footprint.x && y >= footprint.y &&
                                       x < footprint.x + static_cast<int32_t>(footprint.width) &&
                                       y < footprint.y + static_cast<int32_t>(footprint.height);
          if (insideFootprint) continue;
          ++outsideProbed;
          const size_t i = (static_cast<size_t>(y) * 64 + x) * 4u;
          if (std::memcmp(&beforeImg.px[i], &after.px[i], 4 * sizeof(float)) != 0) {
            outsideUntouched = false;
            break;
          }
        }
      }
      check(outsideProbed > 0, "the footprint check fixture: at least one pixel really is outside");
      check(outsideUntouched, "every texel outside the warp's own computed footprint is bit-identical");
    }
  }

  // --- 9. Cancel leaves the document untouched -------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{});
    od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{4, 4}))
        .writePixel(tileLocalOffset(PixelCoord{4, 4}), {0.7f, 0.2f, 0.1f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);
    const TileStore before = *od.document.layers[0].rgbTiles;
    const uint64_t revBefore = od.revision;

    TransformSession session;
    session.beginLayer(od, 0);
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{9.0f, 9.0f});
    session.warpEndDrag();
    session.cancel();

    check(!session.active(), "cancel() ends the session");
    const TransformImage before2 =
        imageFromTileStore(before, 0, 0, 32, 32);
    const TransformImage afterCancel =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 32, 32);
    check(before2.px.size() == afterCancel.px.size() &&
              std::memcmp(before2.px.data(), afterCancel.px.data(),
                         before2.px.size() * sizeof(float)) == 0,
          "cancel() leaves every pixel bit-identical to before the warp began");
    check(od.revision == revBefore, "cancel() does not bump the document's revision");
  }

  // --- 10. One undo restores the pre-warp document ----------------------------
  {
    OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{});
    od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{4, 4}))
        .writePixel(tileLocalOffset(PixelCoord{4, 4}), {0.7f, 0.2f, 0.1f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);
    const TileStore before = *od.document.layers[0].rgbTiles;

    TransformSession session;
    session.beginLayer(od, 0);
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{9.0f, 9.0f});
    session.warpEndDrag();
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "undo fixture: warp commits");

    const TransformImage warped = imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 32, 32);
    const TransformImage beforeImg = imageFromTileStore(before, 0, 0, 32, 32);
    const bool actuallyChangedSomething =
        warped.px.size() != beforeImg.px.size() ||
        std::memcmp(warped.px.data(), beforeImg.px.data(), warped.px.size() * sizeof(float)) != 0;
    check(actuallyChangedSomething, "the commit actually changed the document (a real edit to undo)");

    const Document* prior = od.history.undo();
    check(prior != nullptr, "one undo() call succeeds after a warp commit");
    if (prior != nullptr) od.document = *prior;
    const TransformImage restored = imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 32, 32);
    check(restored.px.size() == beforeImg.px.size() &&
              std::memcmp(restored.px.data(), beforeImg.px.data(),
                         restored.px.size() * sizeof(float)) == 0,
          "one undo() restores every pixel to its pre-warp value");
  }

  // --- 11. N-change re-fit is exact for an affine net -----------------------
  {
    WarpMesh m = WarpMesh::flat(bounds, 3);
    // An affine bend: translate + scale the whole net, still one plane.
    const int side = m.pointsPerSide();
    for (int row = 0; row < side; ++row) {
      for (int col = 0; col < side; ++col) {
        const Point2 p = m.at(row, col);
        m.setAt(row, col, Point2{p.x * 1.4f + 5.0f, p.y * 0.8f - 3.0f});
      }
    }
    check(m.isAffine(), "an affinely-scaled-and-translated flat net still reads as affine");
    const WarpMesh refitted = m.refit(5);
    check(refitted.n() == 5, "refit() honours the requested grid size");
    // Exactness: sample several (u,v) points converted between the two
    // meshes' own [0, cells()] domains at the SAME fractional position and
    // compare.
    bool exact = true;
    const float mCells = static_cast<float>(m.cells());
    const float rCells = static_cast<float>(refitted.cells());
    for (float fv = 0.0f; fv <= 1.0001f && exact; fv += 0.25f) {
      for (float fu = 0.0f; fu <= 1.0001f && exact; fu += 0.25f) {
        const Point2 a = m.evaluate(fu * mCells, fv * mCells);
        const Point2 b = refitted.evaluate(fu * rCells, fv * rCells);
        if (std::fabs(a.x - b.x) > 1e-2f || std::fabs(a.y - b.y) > 1e-2f) exact = false;
      }
    }
    check(exact, "refit() of an affine net reproduces the same shape at every sampled fraction");
  }

  // --- 12. Selection follows the warp, with a matching undo record ----------
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{10, 10}))
        .writePixel(tileLocalOffset(PixelCoord{10, 10}), {0.4f, 0.4f, 0.4f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);
    // As `ui/MacPaintUI.cpp`'s own call sites do: `od.selection` engaged
    // BEFORE the session begins, and that same object passed in.
    od.selection = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);

    TransformSession session;
    const TransformBeginResult began = session.beginSelectionPixels(od, *od.selection, 0);
    check(began.ok, "selection-follows-warp fixture: beginSelectionPixels() succeeds");
    session.setWarpMode(true, 3);
    // A small nudge of the grid's one interior point. Unlike the old
    // Bezier fixture, this is NOT provably confined to the original box --
    // Catmull-Rom has no convex-hull guarantee -- but a small interior
    // nudge stays close to its own neighbours in practice, which is all
    // this fixture's own two point-probes below need to be true.
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{-2.0f, 1.5f});
    session.warpEndDrag();
    const WarpMesh meshAfterDrag = session.warpMesh();
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "selection-follows-warp: commit succeeds");

    if (done.ok) {
      check(od.selection.has_value(), "warping SelectionPixels leaves od.selection engaged");
      if (od.selection.has_value()) {
        // The mesh's own interior point, u=v=1: dragged only slightly, and
        // still well inside the pre-warp full-box selection -- so the
        // warped coverage there should still read "selected".
        const Point2 inside = meshAfterDrag.evaluate(1.0f, 1.0f);
        const float insideCoverage = selectionCoverageAt(
            &*od.selection, PixelCoord{static_cast<int32_t>(std::lround(inside.x)),
                                       static_cast<int32_t>(std::lround(inside.y))});
        check(insideCoverage > 0.9f,
              "warped coverage at evaluate()'s own interior point matches the mesh: selected");
      }
      const float outsideCoverage = selectionCoverageAt(&*od.selection, PixelCoord{2, 2});
      check(outsideCoverage < 0.05f, "warped coverage well outside the box is unselected");

    }
    // A pixel Undo must not revert a selection (app/DocumentLifecycle.hpp on
    // `selection`), so the warped coverage survives it, exactly as a Move's does.
    const std::optional<Selection> warpedSelection = od.selection;
    const Document* prior = od.history.undo();
    check(prior != nullptr, "selection-follows-warp: one undo() call succeeds");
    if (prior != nullptr) od.document = *prior;
    bool selectionUntouched = od.selection.has_value() == warpedSelection.has_value();
    for (int32_t y = 0; selectionUntouched && warpedSelection && y < 32; ++y)
      for (int32_t x = 0; selectionUntouched && x < 32; ++x)
        selectionUntouched = selectionCoverageAt(&*od.selection, PixelCoord{x, y}) ==
                             selectionCoverageAt(&*warpedSelection, PixelCoord{x, y});
    check(selectionUntouched, "undo restores the pixels and leaves the warped selection alone");
  }

  // --- 13. previewWarpDocument() matches what commit() actually writes ------
  // The pure function ui/MacPaintUI.cpp's live preview composites -- this is
  // "the preview at full resolution equals what commit() writes", asserted.
  {
    OpenDocument od = makeBlankOpenDocument(48, 48, WorkingSpace{});
    TileStore& rgb = *od.document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 48; ++y) {
      for (int32_t x = 0; x < 48; ++x) {
        rgb.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}),
                       {static_cast<float>(x) / 48.0f, static_cast<float>(y) / 48.0f, 0.5f, 1.0f});
      }
    }
    od.recordEdit("fixture", EditKind::Content);

    TransformSession session;
    session.beginLayer(od, 0);
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{6.0f, -4.0f});
    session.warpEndDrag();

    Document preview;
    const bool previewOk = session.previewWarpDocument(od, ResampleKernel::CatmullRom, &preview);
    check(previewOk, "previewWarpDocument() succeeds mid-drag for a Layer target");

    const TransformCommitResult done = session.commit(od);
    check(done.ok, "preview-vs-commit fixture: the same warp commits");
    if (previewOk && done.ok) {
      const TransformImage previewImg = imageFromTileStore(*preview.layers[0].rgbTiles, 0, 0, 48, 48);
      const TransformImage committedImg =
          imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 48, 48);
      check(previewImg.px.size() == committedImg.px.size() &&
                std::memcmp(previewImg.px.data(), committedImg.px.data(),
                           previewImg.px.size() * sizeof(float)) == 0,
            "previewWarpDocument()'s pixels are bit-identical to what commit() wrote");
    }
  }

  // --- 14. The same, for a SelectionPixels warp -----------------------------
  {
    OpenDocument od = makeBlankOpenDocument(48, 48, WorkingSpace{});
    TileStore& rgb = *od.document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 48; ++y) {
      for (int32_t x = 0; x < 48; ++x) {
        rgb.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}),
                       {static_cast<float>(x) / 48.0f, static_cast<float>(y) / 48.0f, 0.5f, 1.0f});
      }
    }
    od.recordEdit("fixture", EditKind::Content);
    od.selection = selectRectangle(8.0f, 8.0f, 40.0f, 40.0f);

    TransformSession session;
    const TransformBeginResult began = session.beginSelectionPixels(od, *od.selection, 0);
    check(began.ok, "selection preview-vs-commit fixture: beginSelectionPixels() succeeds");
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 1, 1}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{6.0f, -4.0f});
    session.warpEndDrag();

    Document preview;
    const bool previewOk = session.previewWarpDocument(od, ResampleKernel::CatmullRom, &preview);
    check(previewOk, "previewWarpDocument() succeeds mid-drag for a SelectionPixels target");
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "selection preview-vs-commit fixture: the same warp commits");
    if (previewOk && done.ok) {
      const TransformImage previewImg = imageFromTileStore(*preview.layers[0].rgbTiles, 0, 0, 48, 48);
      const TransformImage committedImg =
          imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 48, 48);
      check(previewImg.px.size() == committedImg.px.size() &&
                std::memcmp(previewImg.px.data(), committedImg.px.data(),
                           previewImg.px.size() * sizeof(float)) == 0,
            "a SelectionPixels warp's preview is bit-identical to what commit() wrote");
    }
  }

  // --- 15. An untouched net returns its source bit-for-bit ------------------
  {
    const DocumentRegion bounds2{0, 0, 16u, 16u};
    const WarpMesh identity = WarpMesh::flat(bounds2, 3);
    TransformImage src;
    src.width = 16;
    src.height = 16;
    src.px.resize(src.sampleCount());
    for (size_t i = 0; i < src.px.size(); ++i)
      src.px[i] = static_cast<float>((i * 37) % 101) / 101.0f;  // noise, so any resample shows
    TransformImage out;
    std::string err;
    const bool warped = warpImage(src, identity, bounds2, ResampleKernel::CatmullRom, &out, &err);
    check(warped && out.px.size() == src.px.size() &&
              std::memcmp(out.px.data(), src.px.data(), src.px.size() * sizeof(float)) == 0,
          "an identity net returns its source bit-identical");
  }

  // --- 16. With several controls in range, the nearest one wins --------------
  {
    // Points 10 px apart (a 3x3 grid over a 20x20 box: spacing = 20/(3-1) =
    // 10); a 15 px radius around a cursor 1 px from (0, 0) also reaches
    // (0, 1), (1, 0) and (1, 1), which a row-major scan meets later.
    const WarpMesh m = WarpMesh::flat(DocumentRegion{0, 0, 20u, 20u}, 3);
    const Point2 nearCorner{m.at(0, 0).x + 1.0f, m.at(0, 0).y + 1.0f};
    check(hitTestWarpControl(m, nearCorner, 15.0f) == (WarpControlRef{true, 0, 0}),
          "several controls in range: the hit test picks the nearest, not the last scanned");
  }

  // --- 17. The committed selection is the warped shape, not the old box ------
  {
    // Case 12's interior drag keeps the warped shape close to the original
    // box; pulling a CORNER out (a point with no neighbour pulling back)
    // moves the selection unambiguously.
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    for (int32_t y = 16; y < 48; ++y)
      for (int32_t x = 16; x < 48; ++x)
        od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.6f, 0.3f, 0.2f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);
    od.selection = selectRectangle(16.0f, 16.0f, 32.0f, 32.0f);
    check(selectionCoverageAt(&*od.selection, PixelCoord{11, 11}) < 0.05f,
          "corner-pull fixture: (11, 11) starts outside the selection");

    TransformSession session;
    const TransformBeginResult began = session.beginSelectionPixels(od, *od.selection, 0);
    check(began.ok, "corner-pull fixture: beginSelectionPixels() succeeds");
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 0, 0}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{-8.0f, -8.0f});
    session.warpEndDrag();
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "corner-pull: commit succeeds");
    check(done.ok && od.selection.has_value() &&
              selectionCoverageAt(&*od.selection, PixelCoord{11, 11}) > 0.5f,
          "corner-pull: the committed selection follows the corner out to (11, 11)");
  }

  std::printf("[selftest] warp mesh %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
