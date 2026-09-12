#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/DirtyTiles.hpp"
#include "core/Gradient.hpp"
#include "core/LayerOps.hpp"
#include "core/VectorRaster.hpp"
#include "core/VectorShape.hpp"
#include "io/GradientSerial.hpp"
#include "io/NpaintFile.hpp"
#include "io/PathSerial.hpp"
#include "ops/Gradient.hpp"

namespace np {

// docs/psd-vector-shapes.md S2 -- a gradient fill on a vector shape: the
// document-level table, the raster that reads it, the hash that guards the
// raster, and both halves of the on-disk form.
//
// ==========================================================================
// The three claims whose failure would be SILENT
// ==========================================================================
//
//  1. **A stale raster.** `core/VectorRaster`'s cache is keyed on
//     `vectorContentHash()`, and a gradient's appearance is NOT in the shape
//     -- it is in `Document::gradients`, which the shape only points at. A
//     hash over the shapes alone therefore cannot see a ramp edit, and the
//     symptom is not a wrong pixel but an edit that does not appear. Section C
//     asserts it at the hash AND end to end through `MaterializedDocument`,
//     because either alone can pass while the feature is broken.
//
//  2. **An out-of-range index painting something.** `Paint::gradient` is a
//     position in a table that travels with the document; a shape whose index
//     is past the end must paint NOTHING. The tempting fallbacks -- `rgba`,
//     or the last entry -- both produce a document that opens without error
//     and renders a colour nobody authored. Section B asserts the shape is
//     fully transparent while its `rgba` is opaque red, so a fallback to
//     either would be visible rather than plausible.
//
//  3. **A version bump rewriting every existing file.** `npvec2:` exists only
//     to carry the two new paint fields, and a writer that emitted it
//     unconditionally would rewrite every vector layer's `np:vector`
//     attribute on its next save for no change of meaning. Section E asserts
//     the v1 payload is byte for byte the shorter one, by arithmetic rather
//     than by eye.
//
// Headless and GPU-free. Section F writes real files and removes every one.
bool runVectorGradientTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr int32_t kW = 64, kH = 64;

  // A closed axis-aligned rectangle as a path, with straight-line encoding --
  // app/selftest/VectorLayer.cpp's own helper, copied rather than shared for
  // the reason app/selftest/Support.hpp gives about section-local scaffolding.
  auto rectShape = [](float x0, float y0, float x1, float y1) {
    VectorShape s;
    SubPath sub;
    sub.closed = true;
    for (const PathPoint& q :
         {PathPoint{x0, y0}, PathPoint{x1, y0}, PathPoint{x1, y1}, PathPoint{x0, y1}}) {
      Anchor a;
      a.pt = a.in = a.out = q;
      sub.anchors.push_back(a);
    }
    s.path.subpaths.push_back(std::move(sub));
    s.path.rule = FillRule::NonZero;
    return s;
  };

  // A black-to-white horizontal ramp across x in [8, 56), the same span the
  // rectangles below cover.
  auto rampDef = [](float x0, float x1) {
    GradientDef g;
    g.name = "ramp";
    g.geometry.kind = GradientKind::Linear;
    g.geometry.spread = GradientSpread::Pad;
    g.geometry.x0 = x0;
    g.geometry.y0 = 0.0f;
    g.geometry.x1 = x1;
    g.geometry.y1 = 0.0f;
    g.stops.colorStops.push_back(ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f});
    g.stops.colorStops.push_back(ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f});
    return g;
  };

  auto texel = [](const TileStore& tiles, int32_t x, int32_t y) {
    const PixelCoord p{x, y};
    const Tile* t = tiles.find(tileCoordAt(p));
    return t == nullptr ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
                        : t->readPixel(tileLocalOffset(p));
  };

  // ==========================================================================
  std::printf("  -- A. A gradient fill actually paints a ramp --\n");
  // ==========================================================================
  {
    GradientTable table;
    table.push_back(rampDef(8.0f, 56.0f));

    VectorShape s = rectShape(8.0f, 8.0f, 56.0f, 56.0f);
    s.fill.on = true;
    s.fill.kind = PaintKind::Gradient;
    s.fill.gradient = 0;
    // Deliberately a colour nothing may fall back to: if anything reads `rgba`
    // for a gradient paint, every probe below comes back red.
    s.fill.rgba = {1.0f, 0.0f, 0.0f, 1.0f};

    const TileStore tiles = rasterizeVectorLayer({s}, table, kW, kH);

    // Three probes across the ramp. The expected value is what
    // `gradientSampleStraight()` gives at the SAME texel centre -- not a
    // number typed in here -- so this asserts that the vector path uses the
    // shared evaluator at the shared sampling convention, and would fail if
    // it sampled at texel corners instead.
    bool matchesEvaluator = true;
    bool monotonic = true;
    float previous = -1.0f;
    for (const int32_t x : {12, 32, 52}) {
      const std::array<float, 4> got = texel(tiles, x, 32);
      const float t = gradientParameterAt(table[0].geometry, static_cast<float>(x) + 0.5f, 32.5f);
      const std::array<float, 4> want = gradientSampleStraight(table[0].stops, t);
      // Interior texels are fully covered, so premultiplied == straight and
      // alpha is 1: any mismatch is the colour, not the coverage.
      for (int c = 0; c < 3; ++c)
        if (std::fabs(got[c] - want[c]) > 1e-3f) matchesEvaluator = false;
      if (std::fabs(got[3] - 1.0f) > 1e-3f) matchesEvaluator = false;
      if (!(got[0] > previous)) monotonic = false;
      previous = got[0];
    }
    check(matchesEvaluator,
          "gradient fill: each probe equals gradientSampleStraight() at the same texel centre");
    check(monotonic, "gradient fill: and the ramp increases left to right, so it is a ramp");
    check(std::fabs(texel(tiles, 12, 32)[0] - texel(tiles, 52, 32)[0]) > 0.5f,
          "gradient fill: the two ends differ by more than half -- not a flat fill");

    // The same ramp through the OTHER renderer. ops/Gradient's
    // `renderGradient()` and core/VectorRaster's `paintCoverage()` are two
    // loops over one evaluator, and this is the assertion that keeps them one:
    // inside a fully covered region they must agree to the last bit.
    TileStore viaOp;
    renderGradient(viaOp, GradientRegion{8, 8, 56, 56}, table[0].geometry, table[0].stops,
                   nullptr);
    bool sameAsOp = true;
    for (const int32_t x : {12, 32, 52})
      for (int c = 0; c < 4; ++c)
        if (texel(viaOp, x, 32)[c] != texel(tiles, x, 32)[c]) sameAsOp = false;
    check(sameAsOp,
          "gradient fill: BIT-IDENTICAL to ops/Gradient's renderGradient() over the same span");

    // A gradient STROKE, not only a fill. The stroke path resolves its paint
    // through the same call, and asserting only the fill would leave a whole
    // branch unmeasured.
    VectorShape line = rectShape(16.0f, 16.0f, 48.0f, 48.0f);
    line.fill.on = false;
    line.stroke.on = true;
    line.stroke.kind = PaintKind::Gradient;
    line.stroke.gradient = 0;
    line.stroke.rgba = {1.0f, 0.0f, 0.0f, 1.0f};
    line.strokeStyle.width = 4.0f;
    const TileStore stroked = rasterizeVectorLayer({line}, table, kW, kH);
    const std::array<float, 4> leftEdge = texel(stroked, 16, 32);
    const std::array<float, 4> rightEdge = texel(stroked, 48, 32);
    check(leftEdge[3] > 0.5f && rightEdge[3] > 0.5f,
          "gradient stroke: both edges of the stroke are painted");
    check(rightEdge[0] - leftEdge[0] > 0.3f,
          "gradient stroke: and the right edge is lighter than the left -- the ramp reached it");
  }

  // ==========================================================================
  std::printf("  -- B. The three ways a gradient paint draws NOTHING --\n");
  // ==========================================================================
  {
    GradientTable one;
    one.push_back(rampDef(8.0f, 56.0f));

    // (1) An index past the end. `rgba` is opaque red and the table's single
    // entry is a visible ramp, so a fallback to EITHER would show here.
    VectorShape past = rectShape(8.0f, 8.0f, 56.0f, 56.0f);
    past.fill.on = true;
    past.fill.kind = PaintKind::Gradient;
    past.fill.gradient = 7;
    past.fill.rgba = {1.0f, 0.0f, 0.0f, 1.0f};
    const TileStore pastTiles = rasterizeVectorLayer({past}, one, kW, kH);
    check(pastTiles.occupiedTileCount() == 0,
          "out of range: an index past the table paints NOTHING -- not rgba, not the last entry");

    // (2) An empty table is the same case, and is what a document whose
    // np:gradients could not be decoded opens with (io/GradientSerial.hpp).
    VectorShape sameShape = past;
    sameShape.fill.gradient = 0;
    const TileStore emptyTable = rasterizeVectorLayer({sameShape}, GradientTable{}, kW, kH);
    check(emptyTable.occupiedTileCount() == 0,
          "out of range: index 0 against an EMPTY table paints nothing either");

    // (3) No colour stops: ops/Gradient's `renderGradient()` returns 0 for
    // this input, and the vector path must agree rather than painting black.
    GradientTable colourless;
    colourless.push_back(GradientDef{});
    colourless[0].geometry = one[0].geometry;
    VectorShape noStops = sameShape;
    const TileStore noStopTiles = rasterizeVectorLayer({noStops}, colourless, kW, kH);
    check(noStopTiles.occupiedTileCount() == 0,
          "no colour stops: paints nothing, matching renderGradient()'s own return of 0");

    // The OTHER empty case is asymmetric and must NOT behave like the one
    // above: no OPACITY stops means fully opaque. Reading the absence as
    // transparency would make every two-colour gradient invisible.
    GradientTable noOpacity;
    noOpacity.push_back(rampDef(8.0f, 56.0f));
    noOpacity[0].stops.opacityStops.clear();
    const TileStore opaque = rasterizeVectorLayer({sameShape}, noOpacity, kW, kH);
    check(std::fabs(texel(opaque, 32, 32)[3] - 1.0f) < 1e-3f,
          "no opacity stops: FULLY OPAQUE, the asymmetry core/Gradient.hpp states");

    // A shape whose fill is off paints nothing even with a perfectly good
    // index -- `on` still gates everything, exactly as for a solid fill.
    VectorShape off = sameShape;
    off.fill.on = false;
    check(rasterizeVectorLayer({off}, one, kW, kH).occupiedTileCount() == 0,
          "fill off: a gradient paint still obeys Paint::on");
  }

  // ==========================================================================
  std::printf("  -- C. The stale-raster trap: the hash must see the TABLE --\n");
  // ==========================================================================
  {
    GradientTable table;
    table.push_back(rampDef(8.0f, 56.0f));
    VectorShape s = rectShape(8.0f, 8.0f, 56.0f, 56.0f);
    s.fill.on = true;
    s.fill.kind = PaintKind::Gradient;
    s.fill.gradient = 0;
    const std::vector<VectorShape> shapes{s};

    const uint64_t h0 = vectorContentHash(shapes, table);
    check(vectorContentHash(shapes, table) == h0, "hash: stable for unchanged content");

    GradientTable edited = table;
    edited[0].stops.colorStops[1].color = {0.0f, 1.0f, 0.0f};
    check(vectorContentHash(shapes, edited) != h0,
          "hash: editing a REFERENCED gradient's stops changes it -- the trap this closes");

    GradientTable moved = table;
    moved[0].geometry.x1 = 40.0f;
    check(vectorContentHash(shapes, moved) != h0, "hash: moving the gradient's endpoint changes it");

    GradientTable spread = table;
    spread[0].geometry.spread = GradientSpread::Reflect;
    check(vectorContentHash(shapes, spread) != h0, "hash: changing the spread changes it");

    GradientTable unreferenced = table;
    unreferenced.push_back(rampDef(0.0f, 10.0f));
    check(vectorContentHash(shapes, unreferenced) == h0,
          "hash: adding an UNREFERENCED gradient does NOT -- only referenced entries are hashed");

    // Two shapes that differ only in which entry they reference must hash
    // differently, which is what makes "one edit, one shape" work.
    GradientTable two = table;
    two.push_back(rampDef(0.0f, 10.0f));
    std::vector<VectorShape> other = shapes;
    other[0].fill.gradient = 1;
    check(vectorContentHash(other, two) != vectorContentHash(shapes, two),
          "hash: two shapes pointing at DIFFERENT entries hash differently");

    // And the same claim end to end, which is the one a user would notice:
    // edit the ramp, and the cached raster must not come back.
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    doc.layers.push_back(makeVectorLayer("art"));
    doc.layers[0].id = 42;
    doc.layers[0].shapes = shapes;
    doc.gradients = table;

    VectorRasterCache cache;
    { MaterializedDocument warm(doc, &cache); }
    check(cache.entryCount() == 1, "cache: the gradient-filled layer rasterised once");
    check(cache.lookup(42, vectorContentHash(doc.layers[0].shapes, doc.gradients)) != nullptr,
          "cache: and the entry is found at its own hash");

    Document after = doc;
    after.gradients[0].stops.colorStops[1].color = {0.0f, 1.0f, 0.0f};
    check(cache.lookup(42, vectorContentHash(after.layers[0].shapes, after.gradients)) == nullptr,
          "cache: after a RAMP edit the cached raster is a MISS, so the edit is not lost");

    const MaterializedDocument warm2(after, &cache);
    const TileStore& painted = *warm2.get().layers[0].rgbTiles;
    const PixelCoord probe{52, 32};
    const Tile* t = painted.find(tileCoordAt(probe));
    check(t != nullptr && t->readPixel(tileLocalOffset(probe))[1] >
                              t->readPixel(tileLocalOffset(probe))[0],
          "cache: and the re-rasterised layer shows the NEW colour (green above red)");

    // core/DirtyTiles is the other half: the canvas has to be told.
    const DocumentDirtyTiles dirty = documentDirtyTiles(doc, after);
    check(dirty.everything && dirty.reason == FullRecompositeReason::VectorGeometryChanged,
          "dirty: a ramp edit alone marks the canvas dirty -- otherwise it never reaches screen");
  }

  // ==========================================================================
  std::printf("  -- D. io/GradientSerial: the document table on disk --\n");
  // ==========================================================================
  {
    GradientTable table;
    table.push_back(rampDef(8.0f, 56.0f));
    table[0].name = "sky";
    table[0].stops.colorStops[0].midpoint = 0.7322f;
    table[0].stops.opacityStops.push_back(OpacityStop{0.25f, 0.5f, 0.4f});
    table[0].stops.opacityStops.push_back(OpacityStop{1.0f, 1.0f, 0.5f});
    table.push_back(rampDef(0.0f, 64.0f));
    table[1].geometry.kind = GradientKind::Angular;
    table[1].geometry.spread = GradientSpread::Reflect;
    table[1].name.clear();

    const std::string encoded = serializeGradients(table);
    check(encoded.rfind(kGradientSerialPrefix, 0) == 0,
          "gradser: the value begins with the version prefix, read before the payload");

    GradientTable back;
    std::string why;
    check(deserializeGradients(encoded, &back, &why), "gradser: and decodes cleanly");
    bool exact = back.size() == 2;
    if (exact) {
      for (size_t i = 0; i < 2 && exact; ++i) {
        const GradientDef& a = table[i];
        const GradientDef& b = back[i];
        exact = a.name == b.name && a.geometry.kind == b.geometry.kind &&
                a.geometry.spread == b.geometry.spread && a.geometry.x0 == b.geometry.x0 &&
                a.geometry.y0 == b.geometry.y0 && a.geometry.x1 == b.geometry.x1 &&
                a.geometry.y1 == b.geometry.y1 &&
                a.stops.colorStops.size() == b.stops.colorStops.size() &&
                a.stops.opacityStops.size() == b.stops.opacityStops.size();
        for (size_t k = 0; k < a.stops.colorStops.size() && exact; ++k)
          exact = a.stops.colorStops[k].position == b.stops.colorStops[k].position &&
                  a.stops.colorStops[k].color == b.stops.colorStops[k].color &&
                  a.stops.colorStops[k].midpoint == b.stops.colorStops[k].midpoint;
        for (size_t k = 0; k < a.stops.opacityStops.size() && exact; ++k)
          exact = a.stops.opacityStops[k].position == b.stops.opacityStops[k].position &&
                  a.stops.opacityStops[k].opacity == b.stops.opacityStops[k].opacity &&
                  a.stops.opacityStops[k].midpoint == b.stops.opacityStops[k].midpoint;
      }
    }
    check(exact,
          "gradser: every field comes back BIT-identical, midpoints and opacity stops included");
    // The midpoint is the field a reader is most likely to drop silently,
    // because a 0.5 default renders almost the same. Asserted by value.
    check(back.size() == 2 && back[0].stops.colorStops[0].midpoint == 0.7322f,
          "gradser: the authored midpoint survives rather than reverting to 0.5");

    GradientTable dummy;
    check(!deserializeGradients("npgrads2:00", &dummy, &why),
          "gradser: a future version is refused by name");
    check(why.find("npgrads1:") != std::string::npos,
          "gradser: and the refusal names the version this build speaks");
    check(!deserializeGradients(encoded.substr(0, encoded.size() - 1), &dummy, &why),
          "gradser: an odd hex length is refused as truncated");
    check(!deserializeGradients(std::string(kGradientSerialPrefix) + "zz", &dummy, &why),
          "gradser: a non-hex character is refused");
    // A hostile count: 0xFFFFFFFF entries declared with nothing after them.
    // Must be refused before a single reserve().
    check(!deserializeGradients(std::string(kGradientSerialPrefix) + "ffffffff", &dummy, &why),
          "gradser: a count larger than the remaining bytes is refused before allocating");
    check(!deserializeGradients(encoded + "00", &dummy, &why),
          "gradser: trailing bytes after the last record are refused");
    // A kind byte this build does not have. Byte offset 4 + 2 (count, then the
    // empty-name length of entry 0 is 2 bytes) -- built here rather than
    // patched, so the offset cannot drift.
    {
      GradientTable oneEntry;
      oneEntry.push_back(rampDef(0.0f, 1.0f));
      oneEntry[0].name.clear();
      std::string bad = serializeGradients(oneEntry);
      const size_t kindNibble = std::strlen(kGradientSerialPrefix) + (4 + 2) * 2;
      bad[kindNibble] = '0';
      bad[kindNibble + 1] = '9';
      check(!deserializeGradients(bad, &dummy, &why),
            "gradser: an unrecognised KIND byte refuses the whole table, not just that entry");
      check(why.find("renumber") != std::string::npos,
            "gradser: and the refusal says why partial decoding would be worse");
    }
    check(deserializeGradients(serializeGradients({}), &dummy, &why),
          "gradser: an empty table is well-formed, not an error");
  }

  // ==========================================================================
  std::printf("  -- E. np:vector stays at v1 unless a gradient needs v2 --\n");
  // ==========================================================================
  {
    VectorShape solid = rectShape(1.0f, 1.0f, 5.0f, 5.0f);
    solid.fill.on = true;
    solid.fill.rgba = {0.25f, 0.5f, 0.75f, 1.0f};
    solid.stroke.on = true;

    const std::string v1 = serializeVectorShapes({solid}, 3);
    check(v1.rfind(kVectorShapeSerialPrefix, 0) == 0,
          "npvec: an all-solid shape list still writes npvec1: -- no existing file is rewritten");

    VectorShape grad = solid;
    grad.fill.kind = PaintKind::Gradient;
    grad.fill.gradient = 2;
    const std::string v2 = serializeVectorShapes({grad}, 3);
    check(v2.rfind(kVectorShapeSerialPrefixV2, 0) == 0,
          "npvec: one gradient paint in the list moves it to npvec2:");

    // The arithmetic, not the eye: v2 adds a kind byte and a u32 index to each
    // of the two paints, so ten bytes -- twenty hex characters -- per shape.
    // If v1 had quietly grown those fields, this difference would be zero.
    check(v2.size() == v1.size() + 20,
          "npvec: v2 is exactly 20 hex characters longer, so v1 carries none of the new fields");

    std::vector<VectorShape> back;
    uint64_t nextId = 0;
    std::string why;
    check(deserializeVectorShapes(v1, &back, &nextId, &why) && back.size() == 1 &&
              back[0].fill.kind == PaintKind::Solid && back[0].fill.gradient == 0,
          "npvec: a v1 payload decodes with Solid paints and index 0");
    check(deserializeVectorShapes(v2, &back, &nextId, &why) && back.size() == 1 &&
              back[0].fill.kind == PaintKind::Gradient && back[0].fill.gradient == 2 &&
              back[0].stroke.kind == PaintKind::Solid,
          "npvec: a v2 payload brings back the kind and the index, per paint");

    // Flipping the paint back to solid must reproduce the ORIGINAL v1 bytes,
    // so the version choice is a function of the content and not of history.
    VectorShape unflipped = grad;
    unflipped.fill.kind = PaintKind::Solid;
    unflipped.fill.gradient = 0;
    check(serializeVectorShapes({unflipped}, 3) == v1,
          "npvec: removing the gradient returns the payload to byte-identical v1");

    // A paint kind a newer build invented is refused, not clamped to Solid --
    // a shape painted with something this build does not have is not
    // approximately that shape.
    {
      std::string mutated = v2;
      // The fill paint's kind byte: reserved(1) + id(8) + nameLen(2) + on(1)
      // + rgba(16), inside a record that itself follows nextShapeId(8) +
      // count(4) + bodyLength(4).
      const size_t kindNibble =
          std::strlen(kVectorShapeSerialPrefixV2) + (8 + 4 + 4 + 1 + 8 + 2 + 1 + 16) * 2;
      mutated[kindNibble] = '0';
      mutated[kindNibble + 1] = '7';
      std::vector<VectorShape> dummy;
      check(!deserializeVectorShapes(mutated, &dummy, nullptr, &why),
            "npvec: an unknown PaintKind byte is refused rather than clamped to Solid");
    }
  }

  // ==========================================================================
  std::printf("  -- F. The .npaint round trip, and the zero-cost promise --\n");
  // ==========================================================================
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    doc.layers.push_back(makeVectorLayer("art"));
    doc.layers[0].id = 9;
    VectorShape s = rectShape(8.0f, 8.0f, 56.0f, 56.0f);
    s.id = 1;
    s.fill.on = true;
    s.fill.kind = PaintKind::Gradient;
    s.fill.gradient = 0;
    doc.layers[0].shapes.push_back(s);
    doc.layers[0].nextShapeId = 2;
    doc.gradients.push_back(rampDef(8.0f, 56.0f));
    doc.gradients[0].name = "sky";

    const char* path = "selftest_gradient_roundtrip.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, path, NpaintSaveOptions{});
    check(saved.ok, "npaint: a document with a gradient-filled shape saves");
    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(path);
      check(loaded.ok, "npaint: and loads back");
      if (loaded.ok) {
        check(loaded.document.gradients.size() == 1 &&
                  loaded.document.gradients[0].name == "sky",
              "npaint: the document's gradient table comes back");
        check(loaded.document.layers.size() == 1 &&
                  loaded.document.layers[0].shapes.size() == 1 &&
                  loaded.document.layers[0].shapes[0].fill.kind == PaintKind::Gradient,
              "npaint: and the shape still points at it");
        // The strongest single statement: the hash covers the shape AND the
        // table entry it references, so agreement means neither was dropped.
        check(vectorContentHash(loaded.document.layers[0].shapes, loaded.document.gradients) ==
                  vectorContentHash(doc.layers[0].shapes, doc.gradients),
              "npaint: the content hash round-trips across shape and table together");
        // And the pixels, which is what a user would see.
        const TileStore beforeTiles =
            rasterizeVectorLayer(doc.layers[0].shapes, doc.gradients, kW, kH);
        const TileStore afterTiles = rasterizeVectorLayer(
            loaded.document.layers[0].shapes, loaded.document.gradients, kW, kH);
        bool samePixels = true;
        for (const int32_t x : {12, 32, 52})
          for (int c = 0; c < 4; ++c)
            if (texel(beforeTiles, x, 32)[c] != texel(afterTiles, x, 32)[c]) samePixels = false;
        check(samePixels, "npaint: and the reloaded gradient rasterises BIT-IDENTICALLY");
      }
    }
    std::remove(path);

    // **The zero-cost promise, asserted against the FILE.** A document with no
    // gradients must produce exactly the bytes it produced before this
    // attribute existed, which means the attribute name must not appear in the
    // header at all. EXR stores attribute names as plain NUL-terminated ASCII,
    // so searching the file for the literal is a direct test rather than a
    // proxy for one.
    auto fileContains = [](const char* path, const char* needle) {
      std::ifstream in(path, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
      return bytes.find(needle) != std::string::npos;
    };

    Document plain = Document::createBlank(kW, kH, WorkingSpace{});
    const char* plainPath = "selftest_gradient_absent.npaint";
    const NpaintSaveResult plainSaved = saveNpaint(plain, plainPath, NpaintSaveOptions{});
    check(plainSaved.ok, "npaint: a document with no gradients saves");
    if (plainSaved.ok)
      check(!fileContains(plainPath, "np:gradients"),
            "npaint: and its file contains no np:gradients attribute AT ALL");
    std::remove(plainPath);

    const char* withPath = "selftest_gradient_present.npaint";
    const NpaintSaveResult withSaved = saveNpaint(doc, withPath, NpaintSaveOptions{});
    if (withSaved.ok)
      check(fileContains(withPath, "np:gradients"),
            "npaint: while a document WITH one does -- so the check above discriminates");
    std::remove(withPath);
  }

  std::printf("[selftest] vector gradient %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
