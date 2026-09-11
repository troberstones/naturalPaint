#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/PathConsumers.hpp"
#include "brush/Deposit.hpp"
#include "core/Path.hpp"
#include "core/PathFlatten.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionShapes.hpp"
#include "core/TextContent.hpp"
#include "core/VectorShape.hpp"
#include "text/Shaper.hpp"

namespace np {
namespace {

// A closed polygon as a `Path`: one anchor per vertex, each anchor's two
// handles coincident with its own point.
//
// core/Path.hpp: "A straight line is an anchor pair whose handles coincide
// with their own anchors, which makes it a cubic whose control points are
// collinear and evenly spaced -- geometrically exact, not an approximation."
// So this is the SAME polygon `selectPolygon()` is handed, not an
// approximation of it, which is what makes section 1's comparison an oracle
// rather than a similarity check.
Path polygonPath(const std::vector<SelectionPoint>& verts) {
  Path p;
  SubPath sub;
  sub.closed = true;
  for (const SelectionPoint& v : verts) {
    Anchor a;
    a.pt = PathPoint{v.x, v.y};
    a.in = a.pt;
    a.out = a.pt;
    sub.anchors.push_back(a);
  }
  p.subpaths.push_back(std::move(sub));
  p.rule = FillRule::NonZero;  // selectPolygon()'s own rule, stated in its header.
  return p;
}

// A circle as four cubic quarter-arcs, the standard k = 4/3 tan(theta/4)
// handle length. core/PathFlatten.hpp puts the worst-case radial error of this
// approximation at about 2.7e-4 of the radius, which is what section 4's area
// tolerance is derived from rather than guessed at.
Path circlePath(float cx, float cy, float r) {
  const float k = 0.5522847498307936f * r;
  const PathPoint pts[4] = {{cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy}};
  const PathPoint outs[4] = {{cx + k, cy - r}, {cx + r, cy + k}, {cx - k, cy + r}, {cx - r, cy - k}};
  const PathPoint ins[4] = {{cx - k, cy - r}, {cx + r, cy - k}, {cx + k, cy + r}, {cx - r, cy + k}};
  Path p;
  SubPath sub;
  sub.closed = true;
  for (int i = 0; i < 4; ++i) {
    Anchor a;
    a.pt = pts[i];
    a.in = ins[i];
    a.out = outs[i];
    sub.anchors.push_back(a);
  }
  p.subpaths.push_back(std::move(sub));
  return p;
}

VectorShape filledShape(Path path, std::array<float, 4> rgba) {
  VectorShape s;
  s.path = std::move(path);
  s.fill.on = true;
  s.fill.rgba = rgba;
  return s;
}

Layer makeRgb(const char* name, int32_t w, int32_t h) {
  (void)w;
  (void)h;
  Layer l;
  l.kind = LayerKind::RGB;
  l.name = name;
  l.rgbTiles = TileStore{};
  return l;
}

float selCoverage(const Selection& s, int32_t x, int32_t y) {
  const PixelCoord at{x, y};
  return selectionTileCoverage(s.tiles.find(tileCoordAt(at)), tileLocalOffset(at));
}

// The sum of the stored ALPHA channel over a whole store. For an opaque fill
// (`rgba[3] == 1`) into an empty layer, source-over leaves `alpha == coverage`
// at every texel, so this sum IS the rasterised area -- which is the quantity
// section 4 checks against pi*r^2 and w*h.
double sumAlpha(const TileStore& store) {
  double total = 0.0;
  for (const auto& [coord, tile] : store) {
    (void)coord;
    for (int32_t ty = 0; ty < kTileSize; ++ty)
      for (int32_t tx = 0; tx < kTileSize; ++tx)
        total += static_cast<double>(tile.readPixel(PixelCoord{tx, ty})[3]);
  }
  return total;
}

std::array<float, 4> pixelAt(const TileStore& store, int32_t x, int32_t y) {
  const PixelCoord at{x, y};
  const Tile* t = store.find(tileCoordAt(at));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(at));
}

// A refusal has to be a sentence a user can act on -- core/Merge.cpp's
// Adjustment refusal is the template. This is the mechanical half of that
// claim: it is present, it names something specific, it is punctuated, and it
// does not fall back on the vocabulary of a stub.
bool refusalIsUsable(const std::string& msg, const char* mustName) {
  if (msg.size() < 60) return false;
  if (msg.back() != '.') return false;
  if (msg.find(mustName) == std::string::npos) return false;
  if (msg.find("unsupported") != std::string::npos) return false;
  if (msg.find("not implemented") != std::string::npos) return false;
  return true;
}

BrushTip testTip() {
  BrushTip t;
  t.radius = 6.0f;
  // Hardness 1: section 7's off-line assertion needs a rim it can trust. Since
  // BrushTip::edgePx the last pixel is antialiased rather than hard, but the
  // footprint -- exactly 0 at and beyond the radius -- did not move, and that
  // is the part of the rim section 7 trusts.
  t.hardness = 1.0f;
  t.spacing = 0.25f;
  t.flow = 1.0f;
  t.opacity = 1.0f;
  t.linearRgb = {1.0f, 0.25f, 0.0f};
  return t;
}

}  // namespace

// app/PathConsumers -- PRD J1/J2/J3/J4, the three things a user does WITH a
// path. Headless, GPU-free, writes no files.
//
// The sections that carry the weight, and why each is shaped the way it is:
//
//  1. **Two independent oracles for path -> selection.** core/SelectionShapes'
//     `selectPolygon()` is a separate exact-area antialiased polygon
//     rasteriser with no code in common with core/PathRaster's cell method, so
//     the two agreeing to within the 1/255 the store quantises to is a far
//     stronger claim than either passing a hand-typed table. `selectRectangle()`
//     is the second, and it is the one that pins the COORDINATE CONVENTION --
//     a half-texel offset between the two files would pass any "is roughly the
//     right shape" test and fail this one.
//  4. **Area, not eyeballs, for the fill.** A circle's coverage sums to
//     pi*r^2 and a rectangle's to w*h, both to a tolerance derived from the
//     Bezier-circle error and binary16 rather than tuned until green.
//  5. **A Text layer goes through all three, and produces the SAME answer a
//     Vector layer built from its own shapes does.** That is the assertion a
//     fixture written from the Vector side forgets, and core/TextContent.hpp
//     section 1's whole claim.
//  8. **Alpha lock is honoured by the stroke, not refused.** See
//     app/PathConsumers.hpp section 5 -- and this is the section that makes
//     that a tested decision rather than an omission.
bool runPathConsumersTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr int32_t W = 256;
  constexpr int32_t H = 256;

  // ==========================================================================
  // 1. Path -> selection agrees with two independent rasterisers
  // ==========================================================================
  std::printf("  -- 1. path -> selection, against selectPolygon() and selectRectangle() --\n");
  {
    // A convex hexagon on deliberately non-integer coordinates: every edge
    // crosses texels fractionally, so the comparison is against antialiased
    // coverage rather than against a shape whose edges happen to land on texel
    // boundaries and agree trivially.
    const std::vector<SelectionPoint> verts = {{40.3f, 20.7f},  {150.9f, 35.2f},
                                               {190.1f, 110.6f}, {140.4f, 200.8f},
                                               {55.5f, 185.3f},  {18.2f, 95.9f}};
    const Selection oracle = selectPolygon(verts);
    const PathSelectionResult got =
        pathToSelection({filledShape(polygonPath(verts), {1, 1, 1, 1})}, nullptr,
                        SelectionCombine::Replace, W, H);
    check(got.ok, "a plain closed polygon converts to a selection");

    // The premise, without which every comparison below would pass against a
    // BITMASK: the oracle really does hold fractional coverage on its edges.
    int fractional = 0;
    for (int32_t y = 0; y < H; ++y)
      for (int32_t x = 0; x < W; ++x) {
        const float c = selCoverage(oracle, x, y);
        if (c > 0.02f && c < 0.98f) ++fractional;
      }
    check(fractional > 200,
          "premise: the oracle's edge really is fractional, not a bitmask");

    float maxDiff = 0.0f;
    int worstX = -1, worstY = -1;
    double sumOracle = 0.0, sumGot = 0.0;
    if (got.ok) {
      for (int32_t y = 0; y < H; ++y)
        for (int32_t x = 0; x < W; ++x) {
          const float a = selCoverage(oracle, x, y);
          const float b = selCoverage(got.selection, x, y);
          sumOracle += a;
          sumGot += b;
          const float d = std::fabs(a - b);
          if (d > maxDiff) {
            maxDiff = d;
            worstX = x;
            worstY = y;
          }
        }
    }
    std::printf("  [measured] max |cell - exact-area| = %.5f (%.2f/255) at (%d,%d); "
                "areas %.2f vs %.2f\n",
                static_cast<double>(maxDiff), static_cast<double>(maxDiff) * 255.0, worstX,
                worstY, sumOracle, sumGot);
    // Both stores quantise to 1/255 and round to nearest, so two exact answers
    // can legitimately land one step apart; 2/255 is that step plus one for
    // the cell rasteriser's own float accumulation. Anything larger is a real
    // disagreement about where the edge is.
    check(maxDiff <= 2.5f / 255.0f,
          "every texel agrees with selectPolygon() to within the store's own step");
    check(std::fabs(sumOracle - sumGot) < 1.0,
          "and the two rasterisers' total selected AREA agrees to under one texel");
  }
  {
    // The coordinate convention. A texel spans [x, x+1), so a rectangle from
    // (30, 40) to (100.5, 90.25) covers exactly 70.5 * 50.25 = 3542.625 texels
    // -- a number neither rasteriser can produce by accident, and one a
    // half-texel offset between core/PathRaster and core/SelectionShapes would
    // still hit while placing the shape one texel over.
    const std::vector<SelectionPoint> rect = {
        {30.0f, 40.0f}, {100.5f, 40.0f}, {100.5f, 90.25f}, {30.0f, 90.25f}};
    const Selection oracle = selectRectangle(30.0f, 40.0f, 100.5f, 90.25f);
    const PathSelectionResult got =
        pathToSelection({filledShape(polygonPath(rect), {1, 1, 1, 1})}, nullptr,
                        SelectionCombine::Replace, W, H);
    check(got.ok, "an axis-aligned rectangle path converts");

    double area = 0.0, oracleArea = 0.0;
    float maxDiff = 0.0f;
    for (int32_t y = 0; y < H; ++y)
      for (int32_t x = 0; x < W; ++x) {
        const float a = selCoverage(oracle, x, y);
        const float b = got.ok ? selCoverage(got.selection, x, y) : 0.0f;
        oracleArea += a;
        area += b;
        maxDiff = std::fmax(maxDiff, std::fabs(a - b));
      }
    std::printf("  [measured] rectangle area: exact 3542.625, selectRectangle %.3f, path %.3f; "
                "max diff %.5f\n",
                oracleArea, area, static_cast<double>(maxDiff));
    check(std::fabs(area - 3542.625) < 4.0,
          "the path's area is the rectangle's true area (fixes the texel convention)");
    check(maxDiff <= 2.5f / 255.0f, "and it matches selectRectangle() texel for texel");
  }

  // ==========================================================================
  // 2. The combine parameter is the app's own modifier grammar
  // ==========================================================================
  std::printf("  -- 2. SelectionCombine is threaded, and it is the shared grammar --\n");
  {
    const Selection base = selectRectangle(20.0f, 20.0f, 120.0f, 120.0f);
    const std::vector<SelectionPoint> verts = {
        {80.0f, 80.0f}, {200.0f, 80.0f}, {200.0f, 200.0f}, {80.0f, 200.0f}};
    const std::vector<VectorShape> shapes = {filledShape(polygonPath(verts), {1, 1, 1, 1})};

    const PathSelectionResult alone =
        pathToSelection(shapes, nullptr, SelectionCombine::Replace, W, H);
    check(alone.ok, "the path alone converts (the operand every op below uses)");

    // The four rows, each against core/SelectionOps' own combine of the same
    // two operands. A version that ignored `op` would pass exactly one row.
    const SelectionCombine ops[4] = {SelectionCombine::Replace, SelectionCombine::Add,
                                     SelectionCombine::Subtract, SelectionCombine::Intersect};
    const char* names[4] = {"Replace", "Add", "Subtract", "Intersect"};
    for (int i = 0; i < 4; ++i) {
      const PathSelectionResult got = pathToSelection(shapes, &base, ops[i], W, H);
      const Selection want = combineSelections(base, alone.selection, ops[i]);
      bool same = got.ok;
      if (same)
        for (int32_t y = 0; y < H && same; ++y)
          for (int32_t x = 0; x < W && same; ++x)
            if (selCoverage(got.selection, x, y) != selCoverage(want, x, y)) same = false;
      std::string label = std::string("  ") + names[i] +
                          ": matches combineSelections() on the same two operands";
      check(same, label.c_str());
    }

    // Semantic, not just structural: Subtract really removes, and the four
    // rows above cannot all be Replace in disguise.
    const PathSelectionResult sub = pathToSelection(shapes, &base, SelectionCombine::Subtract, W, H);
    check(sub.ok && selCoverage(sub.selection, 100, 100) < 0.01f &&
              selCoverage(sub.selection, 40, 40) > 0.99f,
          "  Subtract clears the overlap and leaves the rest of the base alone");

    // The modifier table is core/SelectionOps' and is not re-typed here.
    const PathSelectionResult shifted =
        pathToSelection(shapes, &base, selectionCombineFromModifiers(true, false), W, H);
    check(shifted.ok && selCoverage(shifted.selection, 40, 40) > 0.99f &&
              selCoverage(shifted.selection, 180, 180) > 0.99f,
          "  Shift (via selectionCombineFromModifiers) adds, keeping both regions");

    // The empty operand. `base == nullptr` is NOT selectAll(): Shift-dragging
    // a first path must give that path, not the whole canvas.
    const PathSelectionResult firstShift =
        pathToSelection(shapes, nullptr, SelectionCombine::Add, W, H);
    check(firstShift.ok && selCoverage(firstShift.selection, 10, 10) < 0.01f,
          "  a null base is the EMPTY operand, not selectAll()");
  }

  // ==========================================================================
  // 3. A selection change is not a document edit; a fill and a stroke are
  // ==========================================================================
  std::printf("  -- 3. only the two edits carry an editLabel --\n");
  {
    // `PathSelectionResult` has no `editLabel` MEMBER and `pathToSelection()`
    // is handed no Document, no OpenDocument and no Layer, so it cannot reach
    // `recordEdit()` -- app/DocumentLifecycle.hpp lines 257-265. That half is
    // enforced by the compiler and cannot be asserted at run time; what CAN be
    // asserted is the other half of the asymmetry, that the two operations
    // which ARE edits say so.
    Layer target = makeRgb("fill target", W, H);
    const std::vector<VectorShape> shapes = {
        filledShape(polygonPath({{10, 10}, {60, 10}, {60, 60}, {10, 60}}), {1, 1, 1, 1})};
    const PathFillResult f = fillPathIntoLayer(target, shapes, nullptr, W, H);
    check(f.ok && !f.editLabel.empty(), "a fill reports an editLabel for recordEdit()");

    Layer strokeTarget = makeRgb("stroke target", W, H);
    const PathStrokeResult s =
        strokePathWithBrush(strokeTarget, shapes, testTip(), nullptr, W, H);
    check(s.ok && !s.editLabel.empty(), "a brush stroke reports an editLabel too");
  }

  // ==========================================================================
  // 4. Fill: the coverage it writes IS the shape's area
  // ==========================================================================
  std::printf("  -- 4. fill area against closed forms --\n");
  {
    Layer target = makeRgb("circle", W, H);
    const float r = 60.0f;
    const PathFillResult f = fillPathIntoLayer(
        target, {filledShape(circlePath(128.0f, 128.0f, r), {1, 1, 1, 1})}, nullptr, W, H);
    check(f.ok && f.texelsChanged > 0, "an opaque circle fills");
    const double area = sumAlpha(*target.rgbTiles);
    const double want = 3.14159265358979 * static_cast<double>(r) * static_cast<double>(r);
    std::printf("  [measured] circle r=%.0f: stored alpha sums to %.2f, pi*r^2 = %.2f "
                "(%.3f%% off)\n",
                static_cast<double>(r), area, want, 100.0 * (area - want) / want);
    // A four-arc Bezier circle is radially within ~2.7e-4 of the true circle
    // (core/PathFlatten.hpp), so its area is within ~5.4e-4 of pi*r^2; the
    // stored alpha is binary16, adding ~5e-4 relative per texel with no
    // systematic sign. 0.3% is well above both and well below any real bug.
    check(std::fabs(area - want) < 0.003 * want,
          "  its stored alpha sums to pi*r^2 within 0.3%");
  }
  {
    Layer target = makeRgb("rect", W, H);
    // 70.5 x 50.25 = 3542.625 again -- the same non-integer area section 1
    // used, now measured through the fill's own destination rather than
    // through a selection tile.
    const PathFillResult f = fillPathIntoLayer(
        target,
        {filledShape(polygonPath({{30, 40}, {100.5f, 40}, {100.5f, 90.25f}, {30, 90.25f}}),
                     {1, 1, 1, 1})},
        nullptr, W, H);
    check(f.ok, "an opaque rectangle fills");
    const double area = sumAlpha(*target.rgbTiles);
    std::printf("  [measured] rectangle: stored alpha sums to %.3f, exact 3542.625\n", area);
    check(std::fabs(area - 3542.625) < 4.0, "  and its stored alpha sums to w*h");

    // Premultiplied, not straight: a half-covered edge texel is half PRESENT,
    // so rgb scales with alpha. Reading the interior at full alpha pins the
    // colour, and reading an edge texel pins the convention.
    const std::array<float, 4> inside = pixelAt(*target.rgbTiles, 60, 60);
    check(inside[3] > 0.99f, "  an interior texel is fully opaque");
  }
  {
    // A 50% alpha fill lays 50% alpha, and does so ONCE. A fill that composited
    // each shape's coverage twice, or that treated `rgba[3]` as opacity on top
    // of an already-opaque source, would show up here and nowhere in the
    // opaque cases above.
    Layer target = makeRgb("half", W, H);
    const PathFillResult f = fillPathIntoLayer(
        target,
        {filledShape(polygonPath({{20, 20}, {80, 20}, {80, 80}, {20, 80}}), {1, 0, 0, 0.5f})},
        nullptr, W, H);
    check(f.ok, "a 50%-alpha rectangle fills");
    const std::array<float, 4> px = pixelAt(*target.rgbTiles, 50, 50);
    std::printf("  [measured] 50%% fill stores rgba = (%.4f, %.4f, %.4f, %.4f)\n",
                static_cast<double>(px[0]), static_cast<double>(px[1]),
                static_cast<double>(px[2]), static_cast<double>(px[3]));
    check(std::fabs(px[3] - 0.5f) < 0.002f && std::fabs(px[0] - 0.5f) < 0.002f,
          "  and stores premultiplied (0.5, 0, 0, 0.5), not straight (1, 0, 0, 0.5)");
  }

  {
    // SVG's order, and the only one under which a stroke reads as an outline:
    // within one shape the FILL paints first and the STROKE over it. Painted
    // the other way round the stroke becomes a band UNDER the fill and the
    // boundary comes out fill-coloured, which is invisible in any test that
    // uses a shape with only one of the two turned on -- as every other case
    // in this file does.
    Layer target = makeRgb("fill then stroke", W, H);
    VectorShape sh =
        filledShape(polygonPath({{60, 60}, {180, 60}, {180, 180}, {60, 180}}), {0, 0, 1, 1});
    sh.stroke.on = true;
    sh.stroke.rgba = {1.0f, 0.0f, 0.0f, 1.0f};
    sh.strokeStyle.width = 10.0f;
    const PathFillResult f = fillPathIntoLayer(target, {sh}, nullptr, W, H);
    check(f.ok, "a shape with BOTH a fill and a stroke paints");
    const std::array<float, 4> onEdge = pixelAt(*target.rgbTiles, 120, 60);
    const std::array<float, 4> inside = pixelAt(*target.rgbTiles, 120, 120);
    std::printf("  [measured] on the boundary rgb = (%.2f, %.2f, %.2f); inside = "
                "(%.2f, %.2f, %.2f)\n",
                static_cast<double>(onEdge[0]), static_cast<double>(onEdge[1]),
                static_cast<double>(onEdge[2]), static_cast<double>(inside[0]),
                static_cast<double>(inside[1]), static_cast<double>(inside[2]));
    check(f.ok && onEdge[0] > 0.9f && onEdge[2] < 0.1f,
          "  the boundary is the STROKE's colour -- fill first, stroke over it");
    check(f.ok && inside[2] > 0.9f && inside[0] < 0.1f,
          "  and well inside it is still the fill's colour");
  }

  // ==========================================================================
  // 5. A Text layer goes through all three, and answers identically
  // ==========================================================================
  std::printf("  -- 5. Text is not a second path: all three consumers, same answer --\n");
  if (!shaperAvailable()) {
    std::printf("  [skip] Text sections: %s\n", shaperUnavailableReason());
  } else {
    Layer textLayer;
    textLayer.kind = LayerKind::Text;
    textLayer.name = "Ag";
    textLayer.text = makeTextContent("Ag", PathPoint{30.0f, 120.0f});

    const PathShapesResult fromText = pathConsumerShapes(textLayer);
    check(fromText.ok && !fromText.shapes.empty(),
          "premise: a Text layer yields glyph outlines through pathConsumerShapes()");

    // The SAME shapes, on a Vector layer. If Text were a second rendering path
    // anywhere below pathConsumerShapes(), these two would diverge.
    Layer vecLayer;
    vecLayer.kind = LayerKind::Vector;
    vecLayer.name = "Ag as vector";
    vecLayer.shapes = fromText.shapes;
    const PathShapesResult fromVec = pathConsumerShapes(vecLayer);
    check(fromVec.ok && fromVec.shapes.size() == fromText.shapes.size(),
          "  and a Vector layer holding those same shapes yields the same list");

    if (fromText.ok && fromVec.ok) {
      // J2
      const PathSelectionResult selText =
          pathToSelection(fromText.shapes, nullptr, SelectionCombine::Replace, W, H);
      const PathSelectionResult selVec =
          pathToSelection(fromVec.shapes, nullptr, SelectionCombine::Replace, W, H);
      check(selText.ok && selText.selectedTexels > 100,
            "  J2: the Text layer's glyphs convert to a real selection");
      bool sameSel = selText.ok && selVec.ok;
      if (sameSel)
        for (int32_t y = 0; y < H && sameSel; ++y)
          for (int32_t x = 0; x < W && sameSel; ++x)
            if (selCoverage(selText.selection, x, y) != selCoverage(selVec.selection, x, y))
              sameSel = false;
      check(sameSel, "  J2: texel-identical to the Vector layer's selection");

      // J1/J4
      Layer fillA = makeRgb("from text", W, H);
      Layer fillB = makeRgb("from vector", W, H);
      const PathFillResult fT = fillPathIntoLayer(fillA, fromText.shapes, nullptr, W, H);
      const PathFillResult fV = fillPathIntoLayer(fillB, fromVec.shapes, nullptr, W, H);
      check(fT.ok && fT.texelsChanged > 100, "  J1: the Text layer's glyphs fill a layer");
      std::printf("  [measured] text fill touched %zu texels; vector fill %zu\n",
                  fT.texelsChanged, fV.texelsChanged);
      check(fT.ok && fV.ok && fT.texelsChanged == fV.texelsChanged &&
                std::fabs(sumAlpha(*fillA.rgbTiles) - sumAlpha(*fillB.rgbTiles)) < 1e-6,
            "  J1: identical to the Vector layer's fill, texel count and area");

      // J3
      Layer strokeA = makeRgb("stroke from text", W, H);
      Layer strokeB = makeRgb("stroke from vector", W, H);
      const PathStrokeResult sT =
          strokePathWithBrush(strokeA, fromText.shapes, testTip(), nullptr, W, H);
      const PathStrokeResult sV =
          strokePathWithBrush(strokeB, fromVec.shapes, testTip(), nullptr, W, H);
      check(sT.ok && sT.dabs > 10, "  J3: the brush runs along the Text layer's glyph outlines");
      std::printf("  [measured] text stroke laid %zu dabs over %zu texels\n", sT.dabs,
                  sT.texelsChanged);
      check(sT.ok && sV.ok && sT.dabs == sV.dabs && sT.texelsChanged == sV.texelsChanged,
            "  J3: identical to the Vector layer's stroke, dab count and texel count");
    }

    // The empty-Text refusal is a DIFFERENT sentence from "wrong kind", and
    // must be: a user who has just made a Text layer and not typed needs to be
    // told to type, not that Text layers hold no paths.
    Layer emptyText;
    emptyText.kind = LayerKind::Text;
    emptyText.name = "untitled";
    emptyText.text = makeTextContent("", PathPoint{0.0f, 0.0f});
    const PathShapesResult e = pathConsumerShapes(emptyText);
    check(!e.ok && refusalIsUsable(e.error, "typed"),
          "  an empty Text layer refuses by naming that nothing is typed in it");
  }

  // ==========================================================================
  // 5b. A shape's clip is honoured, and by BOTH the selection and the fill
  // ==========================================================================
  std::printf("  -- 5b. VectorShape::clip cuts the selection and the fill alike --\n");
  {
    // A big square clipped to its own left half. The two consumers must agree
    // about which texels survive -- if only one honoured the clip, a user
    // would fill one region and select a different one from the same shape.
    VectorShape sh =
        filledShape(polygonPath({{40, 40}, {200, 40}, {200, 200}, {40, 200}}), {1, 1, 1, 1});
    sh.clip = polygonPath({{40, 40}, {120, 40}, {120, 200}, {40, 200}});

    const PathSelectionResult sel =
        pathToSelection({sh}, nullptr, SelectionCombine::Replace, W, H);
    check(sel.ok && selCoverage(sel.selection, 80, 120) > 0.99f,
          "inside the clip the shape is selected");
    check(sel.ok && selCoverage(sel.selection, 160, 120) < 0.01f,
          "  outside it, nothing is -- the clip cut the selection");

    Layer target = makeRgb("clipped", W, H);
    const PathFillResult f = fillPathIntoLayer(target, {sh}, nullptr, W, H);
    check(f.ok && pixelAt(*target.rgbTiles, 80, 120)[3] > 0.99f,
          "  and the fill lands inside the clip");
    check(f.ok && pixelAt(*target.rgbTiles, 160, 120)[3] == 0.0f,
          "  and not one texel outside it");

    // An engaged clip that covers nothing hides the shape entirely -- distinct
    // from having no clip at all, and getting it backwards would make a
    // clipped-to-nothing shape paint over everything.
    VectorShape blind = sh;
    blind.clip = polygonPath({{900, 900}, {960, 900}, {960, 960}});
    Layer t2 = makeRgb("blind", W, H);
    const PathFillResult bf = fillPathIntoLayer(t2, {blind}, nullptr, W, H);
    check(!bf.ok && t2.rgbTiles->occupiedTileCount() == 0,
          "  a clip covering nothing hides the shape, rather than un-clipping it");
  }

  // ==========================================================================
  // 5c. "It ran and changed nothing" is a refusal too, not an empty edit
  // ==========================================================================
  std::printf("  -- 5c. an operation that touched no texel refuses rather than no-ops --\n");
  {
    // Three anchors, all at one point. core/Path.hpp is explicit that this is
    // NOT `pathIsEmpty()` -- it has anchors -- so it survives every bounds and
    // emptiness test above and reaches the rasteriser, which correctly
    // produces no coverage at all. Without the "covered no texel" refusal the
    // caller would install a selection of nothing, or record an edit that
    // changed nothing, and be told the operation succeeded.
    const std::vector<VectorShape> degenerate = {
        filledShape(polygonPath({{100, 100}, {100, 100}, {100, 100}}), {1, 1, 1, 1})};
    const PathSelectionResult sel =
        pathToSelection(degenerate, nullptr, SelectionCombine::Replace, W, H);
    // **Matched on "enclose no area", not on "no texel".** The off-canvas
    // refusal used to contain that phrase too, and a sabotage that deleted
    // THIS refusal outright still left the assertion green -- because the
    // degenerate path was reaching the off-canvas branch instead, and being
    // told a path at (100, 100) of a 256x256 canvas was outside it. Both the
    // wrong branch and the matcher that could not tell were fixed; the
    // matcher now names the sentence only this refusal can produce.
    check(!sel.ok && refusalIsUsable(sel.error, "enclose no area") &&
              sel.error.find("outside") == std::string::npos,
          "a path with anchors but no area refuses for its AREA, not as off-canvas");

    // `fill="none"` with no stroke: a real SVG state, a shape that exists and
    // draws nothing.
    VectorShape unpainted;
    unpainted.path = polygonPath({{40, 40}, {120, 40}, {120, 120}, {40, 120}});
    Layer target = makeRgb("nothing", W, H);
    const PathFillResult f = fillPathIntoLayer(target, {unpainted}, nullptr, W, H);
    check(!f.ok && f.editLabel.empty() && refusalIsUsable(f.error, "changed no texel") &&
              f.error.find("outside") == std::string::npos,
          "a shape with no fill and no stroke refuses, and records no edit");
    check(target.rgbTiles->occupiedTileCount() == 0, "  and allocated not one tile");
  }

  // ==========================================================================
  // 6. Every refusal names what is wrong
  // ==========================================================================
  std::printf("  -- 6. refusals, each naming its own cause --\n");
  {
    Layer adj;
    adj.kind = LayerKind::Adjustment;
    adj.name = "Curves 1";
    const PathShapesResult r = pathConsumerShapes(adj);
    check(!r.ok && refusalIsUsable(r.error, "Adjustment") &&
              r.error.find("Curves 1") != std::string::npos,
          "a source layer of the wrong kind refuses, naming kind AND layer");
  }
  {
    const PathSelectionResult r =
        pathToSelection({}, nullptr, SelectionCombine::Replace, W, H);
    check(!r.ok && refusalIsUsable(r.error, "empty"),
          "an empty path refuses to become a selection");
  }
  {
    // Entirely off-canvas, and the sentence has to carry the NUMBERS -- "it is
    // outside" is not actionable, "x 900..960 and the canvas is 256x256" is.
    const std::vector<VectorShape> far = {
        filledShape(polygonPath({{900, 900}, {960, 900}, {960, 960}}), {1, 1, 1, 1})};
    const PathSelectionResult r = pathToSelection(far, nullptr, SelectionCombine::Replace, W, H);
    check(!r.ok && refusalIsUsable(r.error, "outside") &&
              r.error.find("900.00") != std::string::npos &&
              r.error.find("256x256") != std::string::npos,
          "a path entirely off-canvas refuses, with its bounds and the canvas size");

    Layer t = makeRgb("t", W, H);
    const PathFillResult f = fillPathIntoLayer(t, far, nullptr, W, H);
    check(!f.ok && refusalIsUsable(f.error, "outside") && f.editLabel.empty(),
          "  the fill refuses it too, and records no edit");
    const PathStrokeResult s = strokePathWithBrush(t, far, testTip(), nullptr, W, H);
    check(!s.ok && refusalIsUsable(s.error, "outside") && s.editLabel.empty(),
          "  and so does the stroke, allowing for the brush radius");
  }
  {
    const std::vector<VectorShape> shapes = {
        filledShape(polygonPath({{10, 10}, {60, 10}, {60, 60}}), {1, 1, 1, 1})};

    Layer locked = makeRgb("finished", W, H);
    locked.locked = true;
    const PathFillResult f = fillPathIntoLayer(locked, shapes, nullptr, W, H);
    check(!f.ok && refusalIsUsable(f.error, "locked") &&
              f.error.find("finished") != std::string::npos,
          "a locked target refuses the fill, by name");
    check(locked.rgbTiles->occupiedTileCount() == 0,
          "  and the refusal is total -- not one tile was allocated");
    const PathStrokeResult s = strokePathWithBrush(locked, shapes, testTip(), nullptr, W, H);
    check(!s.ok && refusalIsUsable(s.error, "locked"), "a locked target refuses the stroke too");

    Layer alphaLocked = makeRgb("sky", W, H);
    alphaLocked.alphaLocked = true;
    const PathFillResult af = fillPathIntoLayer(alphaLocked, shapes, nullptr, W, H);
    check(!af.ok && refusalIsUsable(af.error, "transparent"),
          "an alpha-locked target refuses the FILL, naming transparency");

    Layer group;
    group.kind = LayerKind::Group;
    group.name = "Folder";
    // `layerKindName(LayerKind::Group)` is "group", lower case, alone among
    // the nine -- so the expectation is read off that function rather than
    // typed, which is how this assertion stopped being a test of my own
    // spelling.
    const char* groupName = layerKindName(LayerKind::Group);
    const PathFillResult gf = fillPathIntoLayer(group, shapes, nullptr, W, H);
    check(!gf.ok && refusalIsUsable(gf.error, groupName),
          "a layer with no tile store refuses the fill, naming its kind");
    const PathStrokeResult gs = strokePathWithBrush(group, shapes, testTip(), nullptr, W, H);
    check(!gs.ok && refusalIsUsable(gs.error, groupName),
          "  and refuses the stroke, naming its kind");

    // A Pigment layer is refused BY THE FILL and accepted BY THE STROKE, and
    // that asymmetry is the module's own decision rather than an accident:
    // brush/Deposit is a deposit path a Pigment layer has, and a premultiplied
    // source-over is not.
    Layer pig;
    pig.kind = LayerKind::Pigment;
    pig.name = "wash";
    pig.pigmentTiles = PigmentTileStore{};
    const PathFillResult pf = fillPathIntoLayer(pig, shapes, nullptr, W, H);
    check(!pf.ok && refusalIsUsable(pf.error, "Pigment"),
          "a Pigment target refuses the fill, saying why the arithmetic differs");
    BrushTip pigTip = testTip();
    pigTip.pigment.c = {0.9f, 0.2f, 0.1f};
    pigTip.pigment.res = {0.05f, 0.05f, 0.05f};
    const PathStrokeResult ps = strokePathWithBrush(pig, shapes, pigTip, nullptr, W, H);
    check(ps.ok && ps.texelsChanged > 0,
          "  but ACCEPTS the stroke -- brush/Deposit is its own deposit route");

    Layer t = makeRgb("t", W, H);
    BrushTip noRadius = testTip();
    noRadius.radius = 0.0f;
    const PathStrokeResult ns = strokePathWithBrush(t, shapes, noRadius, nullptr, W, H);
    check(!ns.ok && refusalIsUsable(ns.error, "radius"),
          "a zero-radius tip refuses the stroke, naming the radius");
  }

  // ==========================================================================
  // 7. The stroke follows the path, at arc-length spacing
  // ==========================================================================
  std::printf("  -- 7. the dabs are on the path, and spaced by arc length --\n");
  {
    // A straight open run of 160 px. brush/StrokePath lays a dab every
    // `spacing * radius` px of ARC LENGTH, so the count is bounded by the
    // length over the spacing -- an emitter that stamped once per input SAMPLE
    // would emit one per flattened vertex instead, which at a 0.1 px tolerance
    // is hundreds.
    Path line;
    SubPath sub;
    for (float x : {40.0f, 200.0f}) {
      Anchor a;
      a.pt = PathPoint{x, 128.0f};
      a.in = a.pt;
      a.out = a.pt;
      sub.anchors.push_back(a);
    }
    sub.closed = false;
    line.subpaths.push_back(sub);

    Layer target = makeRgb("line", W, H);
    const BrushTip tip = testTip();
    const PathStrokeResult s =
        strokePathWithBrush(target, {filledShape(line, {1, 1, 1, 1})}, tip, nullptr, W, H);
    check(s.ok, "a straight open subpath strokes");
    const double expected = 160.0 / static_cast<double>(tip.spacingPx());
    std::printf("  [measured] 160 px at %.2f px spacing: %zu dabs (arc-length bound is %.0f)\n",
                static_cast<double>(tip.spacingPx()), s.dabs, expected);
    // +/-2, not a percentage band -- see the "reaches the end" block above for
    // why the loose version passed every sabotage of the walk.
    check(s.ok && std::fabs(static_cast<double>(s.dabs) - expected) <= 2.0,
          "  the dab count is length/spacing, not one per flattened vertex");

    // On the line, not merely somewhere. A hard tip of radius 6 centred on
    // y=128 must paint y=128 and must not reach y=128+20.
    check(s.ok && pixelAt(*target.rgbTiles, 120, 128)[3] > 0.9f,
          "  a texel on the line is painted");
    check(s.ok && pixelAt(*target.rgbTiles, 120, 148)[3] == 0.0f,
          "  a texel 20 px off the line, past the tip radius, is untouched");
    check(s.ok && pixelAt(*target.rgbTiles, 20, 128)[3] == 0.0f,
          "  and a texel before the subpath's first anchor is untouched");
  }
  {
    // A closed square: the closing edge IS walked. A version that fed the
    // contour's points and stopped -- core/PathFlatten does not repeat the
    // first point -- would leave one whole side of the square unpainted, which
    // is the single easiest thing to get wrong here.
    Layer target = makeRgb("square", W, H);
    const std::vector<VectorShape> square = {
        filledShape(polygonPath({{60, 60}, {180, 60}, {180, 180}, {60, 180}}), {1, 1, 1, 1})};
    const PathStrokeResult s = strokePathWithBrush(target, square, testTip(), nullptr, W, H);
    check(s.ok, "a closed subpath strokes");
    // The closing edge of this square is the LEFT one, from (60,180) back to
    // (60,60) -- the segment core/PathFlatten leaves implicit.
    check(s.ok && pixelAt(*target.rgbTiles, 60, 120)[3] > 0.9f,
          "  the implied closing edge is painted, not left as a gap");
    check(s.ok && pixelAt(*target.rgbTiles, 120, 60)[3] > 0.9f, "  as is the first edge");
    check(s.ok && pixelAt(*target.rgbTiles, 120, 120)[3] == 0.0f,
          "  and the interior is NOT filled -- a stroke is not a fill");
  }
  {
    // TWO DISJOINT SUBPATHS. `brush/StrokePath` fits a Catmull-Rom through the
    // last four samples it was fed, so feeding the second contour's points
    // without `reset()` first walks a curve spanning the GAP between them and
    // lays dabs along it -- a stroke joining two shapes the user drew apart.
    // Nothing else in this file has more than one contour in flight, so this
    // is the only assertion that can see it.
    Path two;
    for (float y : {60.0f, 200.0f}) {
      SubPath sub;
      for (float x : {40.0f, 100.0f}) {
        Anchor a;
        a.pt = PathPoint{x, y};
        a.in = a.pt;
        a.out = a.pt;
        sub.anchors.push_back(a);
      }
      sub.closed = false;
      two.subpaths.push_back(sub);
    }
    Layer target = makeRgb("two runs", W, H);
    const PathStrokeResult s2 =
        strokePathWithBrush(target, {filledShape(two, {1, 1, 1, 1})}, testTip(), nullptr, W, H);
    check(s2.ok, "a path with two disjoint subpaths strokes");
    check(s2.ok && pixelAt(*target.rgbTiles, 70, 60)[3] > 0.9f &&
              pixelAt(*target.rgbTiles, 70, 200)[3] > 0.9f,
          "  both runs are painted");
    check(s2.ok && pixelAt(*target.rgbTiles, 70, 130)[3] == 0.0f,
          "  and the gap between them is NOT -- the emitter is reset per contour");
  }
  {
    // **THE STROKE MUST REACH THE END OF THE PATH.** Two mechanisms protect
    // that, and MEASUREMENT -- not reasoning -- decided how each is asserted,
    // because the obvious geometric probe is blind to one of them:
    //
    //   * `StrokePath::flush()` walks a contour's final segment; `addPoint()`
    //     lags one real sample behind by design and never gets there. Dropping
    //     it leaves a real GAP at the far end of an OPEN subpath -- measured
    //     at exactly one texel with a radius-2 tip on this 160 px line, the
    //     paint stopping at x=200 instead of x=201. So it gets a probe, and
    //     the probe has to sit on the LAST covered texel; three texels earlier
    //     it is inside the covered region either way, which is why an earlier
    //     version of this block was green under the sabotage.
    //
    //   * feeding the first point back on a CLOSED subpath. **This one leaves
    //     no geometric gap at all**, and that is a measured fact rather than a
    //     concession: a closed contour ends where it began, so the last stretch
    //     of the closing edge sits under dabs the FIRST edge already laid at
    //     the shared anchor. The alpha profile down the closing edge is
    //     byte-for-byte identical with and without it. What does change is the
    //     ARC LENGTH walked, and therefore the dab count -- 293 to 288 on the
    //     circle below -- so the assertion that covers it is a count, not a
    //     probe.
    //
    // Hence the dab-count bounds in this section are +/-2 rather than a loose
    // percentage band. That is not a tuned number: a walk lays a dab every
    // `spacingPx` starting one spacing in, so the count is `length/spacing`
    // to within one dab, plus one for where the walk's own rounding falls. A
    // 10%% band -- what this file had first -- is wider than every failure
    // either mechanism produces, and passed all of them.
    BrushTip fine = testTip();
    fine.radius = 2.0f;

    Path line;
    SubPath sub;
    for (float x : {40.0f, 200.0f}) {
      Anchor a;
      a.pt = PathPoint{x, 128.0f};
      a.in = a.pt;
      a.out = a.pt;
      sub.anchors.push_back(a);
    }
    sub.closed = false;
    line.subpaths.push_back(sub);
    Layer openTarget = makeRgb("reaches the end", W, H);
    const PathStrokeResult so =
        strokePathWithBrush(openTarget, {filledShape(line, {1, 1, 1, 1})}, fine, nullptr, W, H);
    check(so.ok && pixelAt(*openTarget.rgbTiles, 120, 128)[3] > 0.5f,
          "premise: the fine tip paints the middle of an open subpath");
    // **Painted at all (> 0), where this used to demand > 0.5.** The probe
    // has to be the LAST covered texel (above), and since BrushTip::edgePx the
    // last covered texel of a hardness-1 tip lies in its antialiased last
    // pixel: at r = 2 the flat core ends at d = 1, and x=201's centre is ~1.58
    // px from the final anchor, where one dab's coverage is well under a half.
    // Lowering the threshold keeps the probe on the edge texel rather than
    // moving it inward -- which is what made an earlier version of this block
    // blind to the sabotage -- and it still discriminates: without `flush()`
    // the last dab lags behind 200, x=201's centre is then at or past the
    // radius, and its coverage is exactly 0 (the squared comparison, unmoved
    // by edgePx), not merely small.
    const float lastAlpha = so.ok ? pixelAt(*openTarget.rgbTiles, 201, 128)[3] : 0.0f;
    std::printf("  [measured] the last texel's alpha from the final anchor: %.6f\n",
                static_cast<double>(lastAlpha));
    check(so.ok && lastAlpha > 0.0f,
          "  and the LAST texel the tip can reach from the final anchor is painted -- at the "
          "antialiased rim's fractional alpha since edgePx, so 'painted' means nonzero now, "
          "not over half");
    check(so.ok && pixelAt(*openTarget.rgbTiles, 202, 128)[3] == 0.0f,
          "  premise: one texel further is past the tip, so the probe above is the edge");

    // A closed contour walks its WHOLE perimeter. 2*(40+220) = 520 px at this
    // tip's 0.5 px spacing.
    Layer closedTarget = makeRgb("closes the loop", W, H);
    const std::vector<VectorShape> tall = {
        filledShape(polygonPath({{20, 20}, {60, 20}, {60, 240}, {20, 240}}), {1, 1, 1, 1})};
    const PathStrokeResult sc = strokePathWithBrush(closedTarget, tall, fine, nullptr, W, H);
    const double perimeter = 520.0;
    const double wantDabs = perimeter / static_cast<double>(fine.spacingPx());
    std::printf("  [measured] closed 40x220 rectangle: %zu dabs, perimeter/spacing = %.1f\n",
                sc.dabs, wantDabs);
    check(sc.ok && std::fabs(static_cast<double>(sc.dabs) - wantDabs) <= 2.0,
          "a closed subpath walks its WHOLE perimeter, closing edge included");
  }

  {
    // Curvature is walked, not chorded. A semicircle bulges well away from the
    // straight line between its endpoints, so an emitter fed only the anchors
    // would lay far fewer dabs and would paint the chord instead of the arc.
    Layer target = makeRgb("arc", W, H);
    const PathStrokeResult s = strokePathWithBrush(
        target, {filledShape(circlePath(128.0f, 128.0f, 70.0f), {1, 1, 1, 1})}, testTip(),
        nullptr, W, H);
    check(s.ok, "a circle's four cubics stroke");
    const double circumference = 2.0 * 3.14159265358979 * 70.0;
    const double expected = circumference / static_cast<double>(testTip().spacingPx());
    std::printf("  [measured] circle r=70: %zu dabs, circumference/spacing = %.0f\n", s.dabs,
                expected);
    check(s.ok && std::fabs(static_cast<double>(s.dabs) - expected) <= 2.0,
          "  the dab count is the whole CIRCUMFERENCE over the spacing, closing edge included");
    check(s.ok && pixelAt(*target.rgbTiles, 128, 58)[3] > 0.9f,
          "  the top of the arc is painted");
    check(s.ok && pixelAt(*target.rgbTiles, 128, 128)[3] == 0.0f, "  and the centre is not");
  }

  // ==========================================================================
  // 8. Alpha lock: honoured by the stroke (app/PathConsumers.hpp section 5)
  // ==========================================================================
  std::printf("  -- 8. an alpha-locked stroke moves colour and not alpha --\n");
  {
    Layer target = makeRgb("locked alpha", W, H);
    // A half-present green block under the whole stroke, plus fully
    // transparent texels beside it. Premultiplied: (0, 0.5, 0, 0.5) is a
    // straight green at 50% presence.
    for (int32_t y = 100; y < 160; ++y)
      for (int32_t x = 20; x < 220; ++x) {
        const PixelCoord at{x, y};
        target.rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at), {0.0f, 0.5f, 0.0f, 0.5f});
      }
    target.alphaLocked = true;

    Path line;
    SubPath sub;
    for (float x : {40.0f, 200.0f}) {
      Anchor a;
      a.pt = PathPoint{x, 128.0f};
      a.in = a.pt;
      a.out = a.pt;
      sub.anchors.push_back(a);
    }
    line.subpaths.push_back(sub);

    const std::array<float, 4> before = pixelAt(*target.rgbTiles, 120, 128);
    const PathStrokeResult s =
        strokePathWithBrush(target, {filledShape(line, {1, 1, 1, 1})}, testTip(), nullptr, W, H);
    check(s.ok, "an alpha-locked RGB layer ACCEPTS the stroke rather than refusing it");
    const std::array<float, 4> after = pixelAt(*target.rgbTiles, 120, 128);
    std::printf("  [measured] under the stroke: rgba %.3f/%.3f/%.3f/%.3f -> "
                "%.3f/%.3f/%.3f/%.3f\n",
                static_cast<double>(before[0]), static_cast<double>(before[1]),
                static_cast<double>(before[2]), static_cast<double>(before[3]),
                static_cast<double>(after[0]), static_cast<double>(after[1]),
                static_cast<double>(after[2]), static_cast<double>(after[3]));
    check(s.ok && after[3] == before[3], "  the stored alpha is bit-identical -- it is a FREEZE");
    check(s.ok && after[0] > before[0] + 0.1f,
          "  and the colour moved toward the ink, so the lock is not a no-op");
    // The other half of a freeze: nothing appears where there was no alpha.
    check(pixelAt(*target.rgbTiles, 120, 180)[3] == 0.0f,
          "  and no alpha appears on a transparent texel under the same stroke");
  }

  // ==========================================================================
  // 9. The active selection bounds both edits (PRD E1)
  // ==========================================================================
  std::printf("  -- 9. the active selection bounds the fill and the stroke --\n");
  {
    // Selects the left half only. Both edits must stop at x = 128.
    const Selection sel = selectRectangle(0.0f, 0.0f, 128.0f, 256.0f);

    Layer fillTarget = makeRgb("bounded fill", W, H);
    const std::vector<VectorShape> band = {
        filledShape(polygonPath({{40, 100}, {220, 100}, {220, 160}, {40, 160}}), {1, 1, 1, 1})};
    const PathFillResult f = fillPathIntoLayer(fillTarget, band, &sel, W, H);
    check(f.ok, "a band crossing the selection edge fills");
    check(f.ok && pixelAt(*fillTarget.rgbTiles, 60, 130)[3] > 0.99f,
          "  inside the selection the fill is full");
    check(f.ok && pixelAt(*fillTarget.rgbTiles, 200, 130)[3] == 0.0f,
          "  outside it not one texel was written");
    // The area is the intersection, exactly: 128-40 = 88 wide by 60 tall.
    const double area = sumAlpha(*fillTarget.rgbTiles);
    std::printf("  [measured] bounded fill area %.2f, intersection is 88 x 60 = 5280\n", area);
    check(std::fabs(area - 5280.0) < 6.0, "  and the filled area is exactly the intersection");

    Layer strokeTarget = makeRgb("bounded stroke", W, H);
    Path line;
    SubPath sub;
    for (float x : {40.0f, 220.0f}) {
      Anchor a;
      a.pt = PathPoint{x, 128.0f};
      a.in = a.pt;
      a.out = a.pt;
      sub.anchors.push_back(a);
    }
    line.subpaths.push_back(sub);
    const PathStrokeResult s = strokePathWithBrush(
        strokeTarget, {filledShape(line, {1, 1, 1, 1})}, testTip(), &sel, W, H);
    check(s.ok && pixelAt(*strokeTarget.rgbTiles, 60, 128)[3] > 0.9f,
          "a stroke crossing the edge paints inside the selection");
    check(s.ok && pixelAt(*strokeTarget.rgbTiles, 200, 128)[3] == 0.0f,
          "  and paints nothing outside it");
  }

  // ==========================================================================
  // 10. The brush's own blend mode reaches a path stroke (review finding 4)
  // ==========================================================================
  //
  // `strokePathWithBrush()` used to call `RgbStroke::begin()` without
  // `tip.blend`, so the defaulted Normal painted a Multiply brush as Normal
  // along a path -- while the Tool Options banner said Blend Mode was applied
  // on an RGB layer. White ink is the discriminating fixture: under Multiply
  // it is the identity (`dst * 1 == dst`), so the grey must come back
  // unchanged, and under Normal it is the one ink that moves a 0.2 grey the
  // furthest (to 1.0). A second ink, 0.5, proves Multiply is actually being
  // computed rather than the stroke being dropped.
  //
  // Tolerance: the layer is binary16, so a stored value can differ from the
  // exact product by one round-to-nearest, 2^-11 relative plus a 2^-25
  // subnormal floor -- the derivation runRgbDepositTest() states for the same
  // `core::Tile`. The probe texel (120, 128) is on the path's spine, in the
  // hardness-1 tip's flat core (coverage exactly 1 at flow 1), so the stroke's
  // `A'` there is exactly 1 and brush/RgbDeposit.hpp §2a's composite reduces to
  // `blend(dst0, ink)` with no partial-coverage term to budget for.
  std::printf("  -- 10. stroke path honours the brush's blend mode --\n");
  {
    constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
    constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
    auto nearHalf = [&](float got, float want) {
      return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
    };
    Path line;
    SubPath sub;
    for (float x : {40.0f, 200.0f}) {
      Anchor a;
      a.pt = PathPoint{x, 128.0f};
      a.in = a.pt;
      a.out = a.pt;
      sub.anchors.push_back(a);
    }
    line.subpaths.push_back(sub);

    // One stroke over a fresh opaque 0.2-grey layer, returning the probe
    // texel before and after.
    auto strokeOverGrey = [&](BlendMode mode, float ink, std::array<float, 4>* before,
                              bool* strokeOk) {
      Layer target = makeRgb("blend over grey", W, H);
      for (int32_t y = 96; y < 160; ++y)
        for (int32_t x = 0; x < W; ++x) {
          const PixelCoord at{x, y};
          target.rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {0.2f, 0.2f, 0.2f, 1.0f});
        }
      *before = pixelAt(*target.rgbTiles, 120, 128);
      BrushTip tip = testTip();
      tip.linearRgb = {ink, ink, ink};
      tip.blend = mode;
      const PathStrokeResult s =
          strokePathWithBrush(target, {filledShape(line, {1, 1, 1, 1})}, tip, nullptr, W, H);
      *strokeOk = s.ok;
      return pixelAt(*target.rgbTiles, 120, 128);
    };

    std::array<float, 4> greyN{}, greyM{}, greyM5{};
    bool okN = false, okM = false, okM5 = false;
    const std::array<float, 4> normalWhite = strokeOverGrey(BlendMode::Normal, 1.0f, &greyN, &okN);
    const std::array<float, 4> multWhite =
        strokeOverGrey(BlendMode::Multiply, 1.0f, &greyM, &okM);
    const std::array<float, 4> multHalf =
        strokeOverGrey(BlendMode::Multiply, 0.5f, &greyM5, &okM5);
    std::printf("  [measured] white ink over %.6f grey: Normal -> %.6f, Multiply -> %.6f; "
                "0.5 ink under Multiply -> %.6f (want %.6f)\n",
                static_cast<double>(greyM[0]), static_cast<double>(normalWhite[0]),
                static_cast<double>(multWhite[0]), static_cast<double>(multHalf[0]),
                static_cast<double>(greyM5[0] * 0.5f));
    check(okN && nearHalf(normalWhite[0], 1.0f) && nearHalf(normalWhite[3], 1.0f),
          "blend: premise -- a NORMAL white stroke over the grey whitens it to 1.0, so the "
          "fixture can tell the two modes apart");
    check(okM && nearHalf(multWhite[0], greyM[0]) && nearHalf(multWhite[1], greyM[1]) &&
              nearHalf(multWhite[2], greyM[2]) && multWhite[3] == greyM[3],
          "blend: a MULTIPLY brush's white ink stroked along a path leaves the 0.2 grey at 0.2 "
          "-- the path stroke reads the brush's own blend mode, as a live RGB stroke does");
    check(okM5 && nearHalf(multHalf[0], greyM5[0] * 0.5f) &&
              nearHalf(multHalf[2], greyM5[2] * 0.5f),
          "blend: and a 0.5 ink under the same Multiply lands on grey * 0.5 -- the mode is "
          "computed, not the stroke dropped");
  }

  std::printf("[selftest] path consumers %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
