#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/TransformSession.hpp"
#include "app/WarpMesh.hpp"

namespace np {

// app/WarpMesh + app/TransformSession's Warp mode (PRD D23, track `warp`).
// Headless and GPU-free. See app/WarpMesh.hpp for the model this proves and
// app/TransformSession.hpp section 9 for how a session's Warp mode uses it.
bool runWarpMeshTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] warp mesh: bicubic lattice model, rasteriser and session wiring, no GPU\n");

  const DocumentRegion bounds{0, 0, 40u, 20u};

  // --- 1. flat() reproduces the rectangle exactly ---------------------------
  {
    const WarpMesh m = WarpMesh::flat(bounds, 4);
    check(m.n() == 4, "flat() keeps the requested grid size");
    const Point2 tl = m.evaluate(0.0f, 0.0f);
    const Point2 br = m.evaluate(4.0f, 4.0f);
    const Point2 mid = m.evaluate(2.0f, 2.0f);
    check(tl.x == 0.0f && tl.y == 0.0f, "flat net's corner evaluates to the rectangle's own corner");
    check(br.x == 40.0f && br.y == 20.0f, "flat net's opposite corner matches too");
    check(mid.x == 20.0f && mid.y == 10.0f, "flat net's centre matches the rectangle's centre");
    check(m.isAffine(), "a flat net is affine");
    check(m.isIdentity(), "a freshly-built flat net is its own identity");
  }

  // --- 2. Hit-test: anchor vs handle -----------------------------------------
  {
    const WarpMesh m = WarpMesh::flat(bounds, 3);
    const int side = m.pointsPerSide();  // 10
    const Point2 anchor = m.at(0, 0);
    const Point2 handle = m.at(0, 1);
    check(WarpControlRef{true, 0, 0}.isAnchor(), "(0,0) is classified an anchor");
    check(!WarpControlRef{true, 0, 1}.isAnchor(), "(0,1) is classified a handle");
    const WarpControlRef hitAnchor = hitTestWarpControl(m, anchor, 2.0f);
    check(hitAnchor.valid && hitAnchor.isAnchor() && hitAnchor.row == 0 && hitAnchor.col == 0,
          "hit test on an anchor's own position picks the anchor");
    const WarpControlRef hitHandle = hitTestWarpControl(m, handle, 2.0f);
    check(hitHandle.valid && !hitHandle.isAnchor() && hitHandle.col == 1,
          "hit test on a handle's own position picks the handle, not the nearby anchor");
    const WarpControlRef hitNothing = hitTestWarpControl(m, Point2{1000.0f, 1000.0f}, 2.0f);
    check(!hitNothing.valid, "hit test far from every control point finds nothing");
    (void)side;
  }

  // --- 3. Dragging an anchor carries its handles; a handle moves alone ------
  {
    WarpMesh m = WarpMesh::flat(bounds, 3);
    const Point2 beforeHandleRight = m.at(0, 4);   // handle right of anchor (0,3)
    const Point2 beforeHandleBelow = m.at(1, 3);   // handle below anchor (0,3)
    const Point2 beforeFarAnchor = m.at(0, 0);
    m.dragControl(WarpControlRef{true, 0, 3}, Point2{5.0f, -2.0f});
    const Point2 afterAnchor = m.at(0, 3);
    check(afterAnchor.x == bounds.x + (3.0f / 9.0f) * bounds.width + 5.0f &&
              afterAnchor.y == bounds.y + -2.0f,
          "the dragged anchor moved by exactly delta");
    check(m.at(0, 4).x == beforeHandleRight.x + 5.0f && m.at(0, 4).y == beforeHandleRight.y - 2.0f,
          "the anchor's right-hand tangent handle rode along by the SAME delta");
    check(m.at(1, 3).x == beforeHandleBelow.x + 5.0f && m.at(1, 3).y == beforeHandleBelow.y - 2.0f,
          "the anchor's tangent handle below it rode along too");
    check(m.at(0, 0).x == beforeFarAnchor.x && m.at(0, 0).y == beforeFarAnchor.y,
          "a distant anchor is untouched");

    WarpMesh m2 = WarpMesh::flat(bounds, 3);
    const Point2 beforeAdjacentAnchor = m2.at(0, 3);
    m2.dragControl(WarpControlRef{true, 0, 4}, Point2{1.0f, 1.0f});  // a handle, not an anchor
    check(m2.at(0, 3).x == beforeAdjacentAnchor.x && m2.at(0, 3).y == beforeAdjacentAnchor.y,
          "dragging a handle alone does not move its neighbouring anchor");
  }

  // --- 4. Chord-error bound on a strongly bent net ---------------------------
  {
    WarpMesh m = WarpMesh::flat(DocumentRegion{0, 0, 100u, 100u}, 3);
    // Bend the centre anchor far out of plane -- a strong bulge.
    m.dragControl(WarpControlRef{true, 3, 3}, Point2{0.0f, -60.0f});
    const int sub = warpChordSubdivisions(m, 0.5f);
    check(sub >= 1 && sub <= kMaxSubdivisionsPerCell, "subdivision count stays in its bounded range");

    // Directly test the bound the derivation claims: sample the TRUE surface
    // far more finely than the chosen subdivision and check no sample point
    // strays more than the tolerance from the nearest tessellated segment of
    // its own cell edge (the V=3 row, which the bent anchor sits on, is the
    // most strongly curved iso-line in this net).
    const float tol = 0.5f;
    const int probesPerSeg = 9;  // finer than `sub`, to actually probe BETWEEN nodes
    float worst = 0.0f;
    for (int a = 0; a < sub; ++a) {
      const float u0 = 3.0f + static_cast<float>(a) / sub;
      const float u1 = 3.0f + static_cast<float>(a + 1) / sub;
      const Point2 segA = m.evaluate(u0, 3.0f);
      const Point2 segB = m.evaluate(u1, 3.0f);
      for (int p = 1; p < probesPerSeg; ++p) {
        const float t = static_cast<float>(p) / probesPerSeg;
        const float u = u0 + (u1 - u0) * t;
        const Point2 truePoint = m.evaluate(u, 3.0f);
        // Distance from truePoint to the chord segA-segB.
        const float ex = segB.x - segA.x, ey = segB.y - segA.y;
        const float len2 = ex * ex + ey * ey;
        float proj = len2 > 1e-9f ? ((truePoint.x - segA.x) * ex + (truePoint.y - segA.y) * ey) / len2
                                  : 0.0f;
        proj = std::clamp(proj, 0.0f, 1.0f);
        const float cx = segA.x + proj * ex, cy = segA.y + proj * ey;
        const float d = std::hypot(truePoint.x - cx, truePoint.y - cy);
        worst = std::max(worst, d);
      }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "chord deviation %.4f px stays under the %.2f px bound", worst,
                 tol);
    check(worst < tol, buf);
  }

  // --- 5. Corner anchor drag lands that texel where evaluation says --------
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
    // Drag the mesh's own top-left anchor (document corner 0,0) inward and
    // down by (8, 8) -- the corner texel should now warp to land near (8, 8).
    WarpMesh mesh = session.warpMesh();
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

  // --- 6. Interior texel at a known (u,v) matches evaluation ----------------
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
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{10.0f, -6.0f});
    session.warpEndDrag();
    const WarpMesh mesh = session.warpMesh();
    // A known (u, v) in the bent mesh's own parameter space -- its centre
    // anchor, which we just dragged.
    const Point2 dstPoint = mesh.evaluate(1.5f, 1.5f);
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "interior-texel fixture: warp commits");
    if (done.ok) {
      const int32_t ix = static_cast<int32_t>(std::floor(dstPoint.x));
      const int32_t iy = static_cast<int32_t>(std::floor(dstPoint.y));
      // Source point for (u,v)=(1.5,1.5) over a 3-cell, 60x60 mesh: exactly
      // the source centre, (30, 30) -- the fixture's own known value there.
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

  // --- 7. SelectionPixels warp leaves outside texels bit-identical ---------
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
    // An INTERIOR anchor (u=1,v=1 of a 3-cell grid, i.e. away from every box
    // edge by 16/3 ~= 5.3 px), nudged by a small delta. A Bezier surface lies
    // within the convex hull of its own control points (a standard property
    // of the Bernstein basis), and every OTHER control point here is still
    // exactly on the box's own boundary -- so the warped shape provably
    // cannot leave the selection's 16x16 box, and "outside the box" really
    // does mean "untouched" for this fixture.
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{-2.0f, 1.5f});
    session.warpEndDrag();
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "selection-pixels warp commits");

    if (done.ok) {
      const TransformImage after =
          imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 64, 64);
      const TransformImage beforeImg = imageFromTileStore(before, 0, 0, 64, 64);
      // A few pixels of margin around the box for `warpedRegion()`'s own
      // one-pixel rounding outset (`app/WarpMesh.hpp`) and the resample
      // kernel's support, so this checks "untouched" well clear of either.
      bool outsideUntouched = true;
      for (int32_t y = 0; y < 64 && outsideUntouched; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
          if (x >= 5 && x < 27 && y >= 5 && y < 27) continue;  // the box, plus margin
          const size_t i = (static_cast<size_t>(y) * 64 + x) * 4u;
          if (std::memcmp(&beforeImg.px[i], &after.px[i], 4 * sizeof(float)) != 0) {
            outsideUntouched = false;
            break;
          }
        }
      }
      check(outsideUntouched, "every texel outside the selection's own box (plus margin) is bit-identical");
    }
  }

  // --- 8. Cancel leaves the document untouched -------------------------------
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
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
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

  // --- 9. One undo restores the pre-warp document ----------------------------
  {
    OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{});
    od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{4, 4}))
        .writePixel(tileLocalOffset(PixelCoord{4, 4}), {0.7f, 0.2f, 0.1f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);
    const TileStore before = *od.document.layers[0].rgbTiles;

    TransformSession session;
    session.beginLayer(od, 0);
    session.setWarpMode(true, 3);
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
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

  // --- 10. N-change re-fit is exact for an affine net -----------------------
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
    // meshes' own [0,n] domains at the SAME fractional position and compare.
    bool exact = true;
    for (float fv = 0.0f; fv <= 1.0001f && exact; fv += 0.25f) {
      for (float fu = 0.0f; fu <= 1.0001f && exact; fu += 0.25f) {
        const Point2 a = m.evaluate(fu * 3.0f, fv * 3.0f);
        const Point2 b = refitted.evaluate(fu * 5.0f, fv * 5.0f);
        if (std::fabs(a.x - b.x) > 1e-2f || std::fabs(a.y - b.y) > 1e-2f) exact = false;
      }
    }
    check(exact, "refit() of an affine net reproduces the same shape at every sampled fraction");
  }

  // --- 11. Selection follows the warp, with a matching undo record ----------
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
    // The interior anchor from case 7 -- convex hull keeps the warped shape
    // inside the original box, so the sampled points below stay meaningful.
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
    session.warpUpdateDrag(Point2{-2.0f, 1.5f});
    session.warpEndDrag();
    const WarpMesh meshAfterDrag = session.warpMesh();
    const TransformCommitResult done = session.commit(od);
    check(done.ok, "selection-follows-warp: commit succeeds");

    if (done.ok) {
      check(od.selection.has_value(), "warping SelectionPixels leaves od.selection engaged");
      if (od.selection.has_value()) {
        // The mesh's own centre, u=v=1.5 of the 3x3 grid: inside its convex
        // hull by construction, and inside the pre-warp full-box selection
        // too -- so the warped coverage there should still read "selected".
        const Point2 inside = meshAfterDrag.evaluate(1.5f, 1.5f);
        const float insideCoverage = selectionCoverageAt(
            &*od.selection, PixelCoord{static_cast<int32_t>(std::lround(inside.x)),
                                       static_cast<int32_t>(std::lround(inside.y))});
        check(insideCoverage > 0.9f,
              "warped coverage at evaluate()'s own interior point matches the mesh: selected");
      }
      const float outsideCoverage = selectionCoverageAt(&*od.selection, PixelCoord{2, 2});
      check(outsideCoverage < 0.05f, "warped coverage well outside the box is unselected");

      // `OpenDocument::warpSelectionUndo` is what lets ordinary Undo restore
      // the selection too (ui/MacPaintUI.cpp's moveHistoryCursor()) -- keyed
      // by the serial of the entry `commit()` just pushed.
      const uint64_t committedSerial = od.history.entries()[od.history.cursor()].serial;
      const OpenDocument::WarpSelectionUndo* rec = nullptr;
      for (const auto& u : od.warpSelectionUndo)
        if (u.serial == committedSerial) rec = &u;
      check(rec != nullptr, "commit() recorded a warpSelectionUndo entry for its own history entry");
      if (rec != nullptr) {
        check(rec->before.has_value() && selectionCoverageAt(&*rec->before, PixelCoord{2, 2}) < 0.05f &&
                  selectionCoverageAt(&*rec->before, PixelCoord{16, 16}) > 0.9f,
              "the recorded 'before' selection is the pre-warp rectangle");
        // The exact restore `moveHistoryCursor()` performs on Undo.
        od.selection = rec->before;
      }
    }
    const Document* prior = od.history.undo();
    check(prior != nullptr, "selection-follows-warp: one undo() call succeeds");
    if (prior != nullptr) od.document = *prior;
    check(od.selection.has_value() &&
              selectionCoverageAt(&*od.selection, PixelCoord{16, 16}) > 0.9f &&
              selectionCoverageAt(&*od.selection, PixelCoord{2, 2}) < 0.05f,
          "after undo (pixels via History, selection via warpSelectionUndo) the original box reads back");
  }

  // --- 12. previewWarpDocument() matches what commit() actually writes ------
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
    session.warpBeginDrag(WarpControlRef{true, 3, 3}, Point2{0.0f, 0.0f});
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

  return ok;
}

}  // namespace np
