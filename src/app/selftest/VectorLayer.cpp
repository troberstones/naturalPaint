#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/DirtyTiles.hpp"
#include "core/LayerCompOps.hpp"
#include "core/LayerSetOps.hpp"
#include "core/OpStack.hpp"
#include "core/Path.hpp"
#include "core/VectorRaster.hpp"
#include "core/VectorShape.hpp"
#include "io/FlatsSerial.hpp"
#include "io/NpaintFile.hpp"
#include "io/PathSerial.hpp"
#include "io/TextSerial.hpp"

namespace np {

// `LayerKind::Vector` -- the layer that holds Bezier geometry instead of
// pixels (PLAN.md phase 13; PRD J1-J5). Headless, GPU-free, writes no files.
//
// ==========================================================================
// The two claims that matter, and why they are not the obvious ones
// ==========================================================================
//
// "A vector layer composites" is easy to assert and easy to make true. The
// two claims worth building a section around are the ones whose failure is
// SILENT:
//
//  1. **A geometry edit is visible.** core/DirtyTiles' pass 1 is a whitelist
//     -- kind, ops, mask presence, tile-store presence -- and a Vector layer
//     has neither tiles nor ops, so before this phase a pure geometry edit
//     compared equal on every field it looks at and produced an EMPTY dirty
//     set. The symptom is not a wrong pixel; it is an edit that does not
//     appear until something unrelated dirties the canvas. Section 4 asserts
//     the dirty set is non-empty AND that the composited pixels actually
//     moved, because either alone can pass while the feature is broken.
//
//  2. **The layer holds no tiles, and history stays cheap.** The whole reason
//     the raster lives outside the document (core/VectorRaster.hpp section 1)
//     is that a `HistoryEntry` holds a `Document` by value. Section 6 asserts
//     the absence directly rather than trusting the constructor.
bool runVectorLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // A closed axis-aligned rectangle as a path, with straight-line encoding.
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

  auto premulAt = [](const std::vector<float>& img, int32_t w, int32_t x, int32_t y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return std::array<float, 4>{img[i], img[i + 1], img[i + 2], img[i + 3]};
  };

  constexpr int32_t kW = 64, kH = 64;

  // --- 1. A Vector layer reaches the composite -----------------------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(8.0f, 8.0f, 24.0f, 24.0f);
    s.fill.on = true;
    s.fill.rgba = {1.0f, 0.0f, 0.0f, 1.0f};  // linear straight red
    doc.layers[0].shapes.push_back(s);

    const std::vector<float> img = compositeDocumentPremultiplied(doc);
    check(img.size() == static_cast<size_t>(kW) * kH * 4,
          "vector: the composite is the full canvas");

    const auto inside = premulAt(img, kW, 16, 16);
    check(inside[0] > 0.999f && inside[3] > 0.999f,
          "vector: a filled shape reaches the composite as opaque red");
    const auto outside = premulAt(img, kW, 40, 40);
    check(outside[3] < 1.0e-4f, "vector: outside the shape stays transparent");

    // Exact-area coverage all the way through the layer, not just in the
    // rasteriser: the alpha channel of the composite must sum to the shape's
    // true area. This is the assertion that would catch a shape painted
    // through a thresholded coverage rather than a weighted one.
    double alphaSum = 0.0;
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) alphaSum += premulAt(img, kW, x, y)[3];
    std::printf("  [measured] composited alpha sums to %.4f (shape area is 256)\n", alphaSum);
    check(std::fabs(alphaSum - 256.0) < 1.0e-2,
          "vector: composited alpha sums to the shape's exact area");
  }

  // --- 2. Fill then stroke, in that order ----------------------------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(16.0f, 16.0f, 48.0f, 48.0f);
    s.fill.on = true;
    s.fill.rgba = {0.0f, 1.0f, 0.0f, 1.0f};  // green fill
    s.stroke.on = true;
    s.stroke.rgba = {0.0f, 0.0f, 1.0f, 1.0f};  // blue stroke
    s.strokeStyle.width = 4.0f;
    s.strokeStyle.join = LineJoin::Miter;
    doc.layers[0].shapes.push_back(s);

    const std::vector<float> img = compositeDocumentPremultiplied(doc);
    // Well inside: fill only.
    const auto core = premulAt(img, kW, 32, 32);
    check(core[1] > 0.999f && core[2] < 1.0e-3f, "vector: the interior is the fill colour");
    // On the boundary: the stroke straddles it, so the stroke wins there --
    // which is only true because the stroke paints AFTER the fill.
    const auto edge = premulAt(img, kW, 32, 16);
    check(edge[2] > 0.999f && edge[1] < 1.0e-3f,
          "vector: the stroke paints over the fill, not under it");
    // Just outside the fill but inside the stroke's outer half.
    const auto skirt = premulAt(img, kW, 32, 15);
    check(skirt[2] > 0.999f, "vector: the stroke extends outside the path, as a stroke does");
  }

  // --- 3. A clip multiplies coverage ---------------------------------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(8.0f, 8.0f, 40.0f, 40.0f);
    s.fill.on = true;
    s.fill.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    // Clip to the left half of the shape.
    s.clip = rectShape(8.0f, 8.0f, 24.0f, 40.0f).path;
    doc.layers[0].shapes.push_back(s);

    const std::vector<float> img = compositeDocumentPremultiplied(doc);
    check(premulAt(img, kW, 16, 20)[3] > 0.999f, "clip: inside the clip the shape paints");
    check(premulAt(img, kW, 32, 20)[3] < 1.0e-3f, "clip: outside the clip it does not");

    double alphaSum = 0.0;
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) alphaSum += premulAt(img, kW, x, y)[3];
    // 16 wide by 32 tall. The clip carries 8-bit coverage (it reuses
    // `Selection`, whose store is uint8 by core/SelectionMask.hpp's own
    // argument), so the tolerance is the quantisation over the clip's
    // perimeter rather than the exactness the unclipped case gets.
    std::printf("  [measured] clipped alpha sums to %.4f (expected 512)\n", alphaSum);
    check(std::fabs(alphaSum - 512.0) < 1.0,
          "clip: the clipped area is exactly the intersection");

    // An engaged clip covering nothing hides the shape entirely -- distinct
    // from no clip at all, which is core/SelectionMask.hpp's inverse-default
    // trap applied here. Getting it backwards makes a clipped-to-nothing
    // shape paint over everything.
    Document doc2 = doc;
    doc2.layers[0].shapes[0].clip = rectShape(200.0f, 200.0f, 210.0f, 210.0f).path;
    const std::vector<float> img2 = compositeDocumentPremultiplied(doc2);
    double sum2 = 0.0;
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) sum2 += premulAt(img2, kW, x, y)[3];
    check(sum2 < 1.0e-3, "clip: a clip that covers nothing hides the shape, not reveals it");
  }

  // --- 4. THE HAZARD: a geometry edit is visible ---------------------------
  //
  // Both halves are required. A non-empty dirty set with an unchanged
  // composite would mean the invalidation fires and the raster is stale; a
  // changed composite with an empty dirty set would mean the pixels are right
  // only because this test recomposited everything from scratch.
  {
    Document before = Document::createBlank(kW, kH, WorkingSpace{});
    before.layers.clear();
    addLayer(before, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(8.0f, 8.0f, 24.0f, 24.0f);
    s.fill.on = true;
    s.fill.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    before.layers[0].shapes.push_back(s);

    Document after = before;
    // Move one anchor: the smallest possible geometry edit, and precisely the
    // one Stage 4's manipulator makes on every mouse-move.
    moveAnchorTo(after.layers[0].shapes[0].path.subpaths[0].anchors[2],
                 PathPoint{40.0f, 40.0f});

    const DocumentDirtyTiles dirty = documentDirtyTiles(before, after);
    check(dirty.everything || !dirty.tiles.empty(),
          "hazard: a pure geometry edit produces a NON-EMPTY dirty set");
    check(dirty.reason == FullRecompositeReason::VectorGeometryChanged,
          "hazard: and it is attributed to the geometry, not to something incidental");

    const std::vector<float> a = compositeDocumentPremultiplied(before);
    const std::vector<float> b = compositeDocumentPremultiplied(after);
    check(a.size() == b.size(), "hazard: both composites are the same size");
    bool moved = false;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i] != b[i]) {
        moved = true;
        break;
      }
    check(moved, "hazard: and the composited pixels genuinely moved");

    // The mirror image: an edit that changes nothing must NOT force a full
    // recomposite, or the "detect a change" fix has become "always redraw"
    // and the assertion above would pass for the wrong reason.
    Document same = before;
    const DocumentDirtyTiles quiet = documentDirtyTiles(before, same);
    check(quiet.reason != FullRecompositeReason::VectorGeometryChanged,
          "hazard: an untouched vector layer does NOT force a recomposite");

    // A paint-only change (no geometry moved) must also be caught: the hash
    // covers paint, and a cache keyed on geometry alone would go stale here.
    Document recoloured = before;
    recoloured.layers[0].shapes[0].fill.rgba = {0.0f, 0.0f, 1.0f, 1.0f};
    const DocumentDirtyTiles paintDirty = documentDirtyTiles(before, recoloured);
    check(paintDirty.reason == FullRecompositeReason::VectorGeometryChanged,
          "hazard: a paint-only change is caught too, not just a moved anchor");
  }

  // --- 4b. THE SAME HAZARD FOR TEXT, and it is worse -----------------------
  //
  // Every word of section 4 applies to a Text layer with more force: a Vector
  // layer at least has a `shapes` member for a future comparison to reach,
  // and a Text layer holds neither tiles nor ops nor shapes -- so before
  // `TextContentChanged` existed, pass 1's whitelist compared EQUAL on every
  // field for a layer whose entire string had just changed.
  //
  // This section is NOT guarded on `shaperAvailable()`. `documentDirtyTiles()`
  // hashes the content and never shapes, so the invalidation is testable on
  // every build; only the "and the pixels genuinely moved" half needs a
  // shaper, and it is guarded on its own below.
  {
    Document before = Document::createBlank(kW, kH, WorkingSpace{});
    before.layers.clear();
    addLayer(before, 0, makeTextLayer("caption"));
    before.layers[0].text.utf8 = "Hi";
    before.layers[0].text.style.sizePx = 24.0f;
    before.layers[0].text.origin = PathPoint{4.0f, 24.0f};

    // The smallest possible text edit, and precisely the one a user makes on
    // every keystroke.
    Document typed = before;
    typed.layers[0].text.utf8 = "Hip";

    const DocumentDirtyTiles dirty = documentDirtyTiles(before, typed);
    check(dirty.everything || !dirty.tiles.empty(),
          "hazard: one keystroke on a Text layer produces a NON-EMPTY dirty set");
    check(dirty.reason == FullRecompositeReason::TextContentChanged,
          "hazard: attributed to the TEXT, not to VectorGeometryChanged -- a slow frame has "
          "to name what the user actually did");

    // Every OTHER field of the content, one at a time, because a hash that
    // covers the string and nothing else would pass the assertion above and
    // fail the moment a user changed the font. Named per field so a miss says
    // which one.
    struct Mutation {
      const char* what;
      void (*apply)(TextContent&);
    };
    static const Mutation kMutations[] = {
        {"the font family", [](TextContent& c) { c.style.fontFamily = "Courier"; }},
        {"the size", [](TextContent& c) { c.style.sizePx = 48.0f; }},
        {"the tracking", [](TextContent& c) { c.style.tracking = 3.0f; }},
        {"the leading", [](TextContent& c) { c.style.leading = 60.0f; }},
        {"bold", [](TextContent& c) { c.style.bold = true; }},
        {"italic", [](TextContent& c) { c.style.italic = true; }},
        {"the frame width", [](TextContent& c) { c.frame.width = 120.0f; }},
        {"the frame height", [](TextContent& c) { c.frame.height = 60.0f; }},
        {"the alignment", [](TextContent& c) { c.align = TextAlign::Center; }},
        {"the origin", [](TextContent& c) { c.origin = PathPoint{9.0f, 30.0f}; }},
        {"the fill colour", [](TextContent& c) { c.fill.rgba = {0.0f, 1.0f, 0.0f, 1.0f}; }},
        {"the stroke", [](TextContent& c) { c.stroke.on = true; }},
    };
    for (const Mutation& m : kMutations) {
      Document changed = before;
      m.apply(changed.layers[0].text);
      const DocumentDirtyTiles d = documentDirtyTiles(before, changed);
      std::string label = std::string("hazard: changing ") + m.what +
                          " also invalidates -- a control that changed nothing on screen "
                          "would look like a broken control";
      check(d.reason == FullRecompositeReason::TextContentChanged, label.c_str());
    }

    // The mirror image, section 4's own: an edit that changes nothing must NOT
    // force a full recomposite, or "detect a change" has become "always
    // redraw" and every assertion above passes for the wrong reason.
    Document same = before;
    const DocumentDirtyTiles quiet = documentDirtyTiles(before, same);
    check(quiet.reason != FullRecompositeReason::TextContentChanged,
          "hazard: an untouched Text layer does NOT force a recomposite");

    if (shaperAvailable()) {
      const std::vector<float> a2 = compositeDocumentPremultiplied(before);
      const std::vector<float> b2 = compositeDocumentPremultiplied(typed);
      bool moved = a2.size() == b2.size();
      if (moved) {
        moved = false;
        for (size_t i = 0; i < a2.size(); ++i)
          if (a2[i] != b2[i]) {
            moved = true;
            break;
          }
      }
      check(moved,
            "hazard: and the composited pixels genuinely moved -- the invalidation firing "
            "over a raster that never changed would be the other half of this bug");
    }
  }

  // --- 5. The content hash discriminates -----------------------------------
  {
    std::vector<VectorShape> base;
    VectorShape s = rectShape(0.0f, 0.0f, 10.0f, 10.0f);
    s.fill.on = true;
    base.push_back(s);
    const uint64_t h0 = vectorContentHash(base, {});
    check(vectorContentHash(base, {}) == h0, "hash: is stable for unchanged content");

    std::vector<VectorShape> moved = base;
    moved[0].path.subpaths[0].anchors[0].pt.x += 0.001f;
    check(vectorContentHash(moved, {}) != h0, "hash: a sub-texel anchor move changes it");

    std::vector<VectorShape> handled = base;
    handled[0].path.subpaths[0].anchors[0].out.y += 1.0f;
    check(vectorContentHash(handled, {}) != h0, "hash: a HANDLE move changes it, not just an anchor");

    std::vector<VectorShape> painted = base;
    painted[0].fill.rgba[2] = 0.5f;
    check(vectorContentHash(painted, {}) != h0, "hash: a paint change changes it");

    std::vector<VectorShape> stroked = base;
    stroked[0].strokeStyle.dashOffset = 1.0f;
    check(vectorContentHash(stroked, {}) != h0, "hash: a dash-offset change changes it");

    std::vector<VectorShape> pivoted = base;
    pivoted[0].pivot = PathPoint{1.0f, 2.0f};
    check(vectorContentHash(pivoted, {}) != h0,
          "hash: a pivot move changes it, so a cache rebuild cannot drop the pivot");

    std::vector<VectorShape> ruled = base;
    ruled[0].path.rule = FillRule::EvenOdd;
    check(vectorContentHash(ruled, {}) != h0, "hash: a fill-rule change changes it");
  }

  // --- 6. The layer holds no tiles, and neither does history ---------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(0.0f, 0.0f, float(kW), float(kH));
    s.fill.on = true;
    doc.layers[0].shapes.push_back(s);

    const Layer& l = doc.layers[0];
    check(!l.rgbTiles.has_value() && !l.pigmentTiles.has_value(),
          "storage: a Vector layer owns neither tile store");
    check(!layerHoldsPixels(l),
          "storage: and layerHoldsPixels() says so -- the guard core/Composite re-derives");

    // Compositing must not have installed a raster on the real document. The
    // materialised view is a copy; if this ever fails, every HistoryEntry
    // starts carrying a full-canvas raster (core/VectorRaster.hpp section 1).
    (void)compositeDocumentPremultiplied(doc);
    check(!doc.layers[0].rgbTiles.has_value(),
          "storage: compositing does NOT write a raster back onto the document");
    check(doc.layers[0].kind == LayerKind::Vector,
          "storage: and does not leave the layer claiming to be RGB");
  }

  // --- 7. The cache: hit, miss, staleness, eviction ------------------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    doc.layers[0].id = 77;
    VectorShape s = rectShape(4.0f, 4.0f, 30.0f, 30.0f);
    s.fill.on = true;
    doc.layers[0].shapes.push_back(s);

    VectorRasterCache cache;
    const uint64_t h = vectorContentHash(doc.layers[0].shapes, {});
    check(cache.lookup(77, h) == nullptr, "cache: starts empty");

    {
      MaterializedDocument m(doc, &cache);
      check(m.rewrote(), "cache: a document with a Vector layer IS rewritten");
      check(cache.entryCount() == 1, "cache: materialising populated one entry");
    }
    const auto first = cache.lookup(77, h);
    check(first != nullptr, "cache: and the entry is retrievable at the same hash");
    {
      MaterializedDocument m(doc, &cache);
      (void)m;
      check(cache.lookup(77, h) == first, "cache: a second pass reuses the same raster");
    }

    // Staleness is the property that matters: an entry stored under a
    // different hash must never be handed back.
    check(cache.lookup(77, h ^ 1ull) == nullptr,
          "cache: a stale entry is a MISS, never returned");

    Document moved = doc;
    moveAnchorTo(moved.layers[0].shapes[0].path.subpaths[0].anchors[2], PathPoint{50.0f, 50.0f});
    {
      MaterializedDocument m(moved, &cache);
      (void)m;
    }
    check(cache.entryCount() == 1, "cache: an edit replaces the layer's entry rather than adding");
    check(cache.lookup(77, vectorContentHash(moved.layers[0].shapes, {})) != nullptr,
          "cache: and the new entry is under the new hash");
    std::printf("  [measured] one cached raster of a 26x26 shape: %zu bytes resident\n",
                cache.residentBytes());

    // Deleting the layer must not leave its raster resident for the session.
    Document empty = Document::createBlank(kW, kH, WorkingSpace{});
    empty.layers.clear();
    cache.forgetLayersNotIn(empty);
    check(cache.entryCount() == 0 && cache.residentBytes() == 0,
          "cache: forgetLayersNotIn() drops a deleted layer's raster");
  }

  // --- 8. A document with no Vector layer is not copied at all -------------
  //
  // The fast path every pre-existing caller takes. If this regresses, every
  // composite in the application starts copying the layer vector.
  {
    Document plain = Document::createBlank(kW, kH, WorkingSpace{});
    check(!documentHasVectorLayers(plain), "materialise: a blank document has no vector layers");
    MaterializedDocument m(plain, nullptr);
    check(!m.rewrote(), "materialise: so nothing is rewritten");
    check(&m.get() == &plain, "materialise: and get() is the ORIGINAL document, not a copy");
  }

  // --- 9. A null cache still works ----------------------------------------
  //
  // io/Export and any one-shot caller pass null rather than growing a cache
  // they immediately discard.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeVectorLayer("shapes"));
    VectorShape s = rectShape(8.0f, 8.0f, 24.0f, 24.0f);
    s.fill.on = true;
    doc.layers[0].shapes.push_back(s);

    MaterializedDocument m(doc, nullptr);
    check(m.rewrote(), "materialise: a null cache still rasterises");
    check(m.get().layers[0].kind == LayerKind::RGB &&
              m.get().layers[0].rgbTiles.has_value(),
          "materialise: the view's layer is an ordinary RGB layer with tiles");
    check(m.get().layers[0].name == "shapes" && m.get().layers[0].shapes.empty(),
          "materialise: carrying the layer's name, with the geometry left behind");
  }

  // --- 10. Bounds include the stroke, and a miter's reach ------------------
  {
    std::vector<VectorShape> shapes;
    VectorShape s = rectShape(10.0f, 10.0f, 20.0f, 20.0f);
    s.fill.on = true;
    shapes.push_back(s);
    const PathBounds fillOnly = vectorShapesBounds(shapes);
    check(fillOnly.valid && std::fabs(fillOnly.minX - 10.0f) < 1.0e-4f,
          "bounds: a fill-only shape bounds its path exactly");

    shapes[0].stroke.on = true;
    shapes[0].strokeStyle.width = 4.0f;
    shapes[0].strokeStyle.join = LineJoin::Bevel;
    const PathBounds bevelled = vectorShapesBounds(shapes);
    check(std::fabs(bevelled.minX - 8.0f) < 1.0e-4f,
          "bounds: a bevelled stroke outsets by exactly half its width");

    // A miter join reaches up to miterLimit * half-width, which the bounds
    // must allow for or a mitred spike is clipped at the layer's own edge.
    shapes[0].strokeStyle.join = LineJoin::Miter;
    shapes[0].strokeStyle.miterLimit = 4.0f;
    const PathBounds mitred = vectorShapesBounds(shapes);
    check(mitred.minX < bevelled.minX - 1.0e-3f,
          "bounds: a miter join outsets FURTHER than half the width");
    check(std::fabs(mitred.minX - 2.0f) < 1.0e-4f,
          "bounds: by exactly miterLimit times the half width");
  }


  // --- 11. io/PathSerial: the encoding on its own -------------------------
  {
    std::vector<VectorShape> shapes;
    VectorShape s = rectShape(1.5f, 2.25f, 30.125f, 40.0f);
    s.id = 7;
    s.name = "outline";
    s.fill.on = true;
    s.fill.rgba = {0.25f, 0.5f, 0.75f, 0.875f};
    s.stroke.on = true;
    s.stroke.rgba = {1.0f, 0.0f, 0.5f, 1.0f};
    s.strokeStyle.width = 3.5f;
    s.strokeStyle.cap = LineCap::Round;
    s.strokeStyle.join = LineJoin::Bevel;
    s.strokeStyle.miterLimit = 7.25f;
    s.strokeStyle.dashes = {2.0f, 3.5f, 1.0f};
    s.strokeStyle.dashOffset = 0.75f;
    s.pivot = PathPoint{11.0f, 12.0f};
    s.path.rule = FillRule::EvenOdd;
    s.path.subpaths[0].anchors[1].in = PathPoint{5.5f, 6.5f};
    s.path.subpaths[0].anchors[1].smooth = true;
    s.clip = rectShape(0.0f, 0.0f, 10.0f, 10.0f).path;
    shapes.push_back(s);

    const std::string encoded = serializeVectorShapes(shapes, 99);
    check(encoded.rfind(kVectorShapeSerialPrefix, 0) == 0,
          "serial: the value begins with the version prefix, so it is read before the payload");

    std::vector<VectorShape> back;
    uint64_t nextId = 0;
    std::string why;
    check(deserializeVectorShapes(encoded, &back, &nextId, &why),
          "serial: and decodes cleanly");
    check(nextId == 99, "serial: nextShapeId survives");
    check(back.size() == 1, "serial: one shape in, one out");

    // **Bit-exact, not near.** Floats are written as IEEE-754 bit patterns
    // precisely so that an anchor does not drift by an ulp per save/load
    // cycle, which over a session would visibly move a shape.
    const bool exact = back[0].id == 7 && back[0].name == "outline" &&
                       back[0].fill.on && back[0].fill.rgba == s.fill.rgba &&
                       back[0].stroke.rgba == s.stroke.rgba &&
                       back[0].strokeStyle.width == s.strokeStyle.width &&
                       back[0].strokeStyle.cap == LineCap::Round &&
                       back[0].strokeStyle.join == LineJoin::Bevel &&
                       back[0].strokeStyle.miterLimit == 7.25f &&
                       back[0].strokeStyle.dashes == s.strokeStyle.dashes &&
                       back[0].strokeStyle.dashOffset == 0.75f &&
                       back[0].path.rule == FillRule::EvenOdd;
    check(exact, "serial: every scalar comes back bit-identical, not approximately");
    check(back[0].pivot.has_value() && back[0].pivot->x == 11.0f && back[0].pivot->y == 12.0f,
          "serial: the pivot survives -- 'move a pivot and have it stick' depends on this");
    check(back[0].clip.has_value() && back[0].clip->subpaths.size() == 1,
          "serial: the clip path survives");
    check(back[0].path.subpaths[0].anchors[1].in.x == 5.5f &&
              back[0].path.subpaths[0].anchors[1].smooth,
          "serial: handles and the smooth flag survive, not just anchors");
    // The hash is the strongest single statement of round-trip fidelity: it
    // covers every field that affects the raster plus the pivot, so agreement
    // means nothing was silently dropped.
    check(vectorContentHash(back, {}) == vectorContentHash(shapes, {}),
          "serial: the content hash round-trips, so NO field was silently dropped");

    // Refusals. Each is a shape a `.npaint` could genuinely arrive carrying.
    std::vector<VectorShape> dummy;
    // A future tag on a payload that is otherwise PERFECTLY VALID. An earlier
    // revision of this check used "npvec2:00", which is refused whether or not
    // the version is inspected -- its two hex digits are truncated anyway --
    // so it passed with the version gate deleted. Sabotage found that; the
    // payload below can only be refused BY the version gate.
    //
    // `npvec3:` and not `npvec2:` since S2: v2 is a version this build READS,
    // so the old tag would now test nothing at all. The shape of the check is
    // unchanged, and so is the property it defends.
    const std::string futureTagged = "npvec3:" + encoded.substr(std::strlen(kVectorShapeSerialPrefix));
    check(!deserializeVectorShapes(futureTagged, &dummy, nullptr, &why),
          "serial: a FUTURE version is refused even when its payload is otherwise valid");
    check(why.find("npvec1:") != std::string::npos && why.find("npvec2:") != std::string::npos,
          "serial: and the refusal names BOTH versions this build speaks");
    check(!deserializeVectorShapes("npvec1:abc", &dummy, nullptr, &why),
          "serial: an odd hex length is refused as truncated");
    check(!deserializeVectorShapes("npvec1:zz", &dummy, nullptr, &why),
          "serial: a non-hex character is refused");
    check(!deserializeVectorShapes("", &dummy, nullptr, &why),
          "serial: an empty value is refused, not read as zero shapes");
    // A hostile count: 0xFFFFFFFF shapes declared in a payload with nothing
    // after it. Must be refused before a single reserve().
    check(!deserializeVectorShapes("npvec1:0000000000000000ffffffff", &dummy, nullptr, &why),
          "serial: a count larger than the remaining bytes is refused before allocating");
    check(deserializeVectorShapes(serializeVectorShapes({}, 1), &dummy, nullptr, &why),
          "serial: an empty shape list is well-formed, not an error");
  }

  // --- 12. The .npaint round trip -----------------------------------------
  //
  // A document format is a file format, so this section writes real files --
  // io/NpaintFile's own selftest rule -- and removes every one of them.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("under"));
    addLayer(doc, 1, makeVectorLayer("art"));
    doc.layers[1].opacity = 0.625f;
    doc.layers[1].blend = "multiply";
    VectorShape s = rectShape(4.0f, 4.0f, 28.0f, 28.0f);
    s.id = 3;
    s.name = "box";
    s.fill.on = true;
    s.fill.rgba = {0.125f, 0.25f, 0.5f, 1.0f};
    s.stroke.on = true;
    s.stroke.rgba = {1.0f, 1.0f, 0.0f, 1.0f};
    s.strokeStyle.width = 2.0f;
    s.pivot = PathPoint{9.0f, 9.0f};
    doc.layers[1].shapes.push_back(s);
    doc.layers[1].nextShapeId = 4;

    const char* path = "selftest_vector_roundtrip.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, path, NpaintSaveOptions{});
    check(saved.ok, "npaint: a document containing a Vector layer SAVES (it used to be refused)");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(path);
      check(loaded.ok, "npaint: and loads back");
      if (loaded.ok) {
        check(loaded.document.layers.size() == 2, "npaint: both layers come back");
        const Layer& v = loaded.document.layers[1];
        check(v.kind == LayerKind::Vector, "npaint: the Vector layer is still a Vector layer");
        check(!v.rgbTiles.has_value(),
              "npaint: and did NOT come back as pixels -- the geometry is what was stored");
        check(v.name == "art" && v.blend == "multiply" && v.opacity == 0.625f,
              "npaint: its ordinary layer metadata survives");
        check(v.shapes.size() == 1 && v.nextShapeId == 4,
              "npaint: one shape, and the id counter, come back");
        check(vectorContentHash(v.shapes, {}) == vectorContentHash(doc.layers[1].shapes, {}),
              "npaint: the shape's content hash is identical across the round trip");
        check(v.shapes[0].pivot.has_value() && v.shapes[0].pivot->x == 9.0f,
              "npaint: including the pivot");

        // The composite must agree, which is the end-to-end claim: geometry
        // stored, geometry reloaded, same pixels.
        const std::vector<float> before = compositeDocumentPremultiplied(doc);
        const std::vector<float> after = compositeDocumentPremultiplied(loaded.document);
        bool same = before.size() == after.size();
        if (same)
          for (size_t i = 0; i < before.size(); ++i)
            if (before[i] != after[i]) {
              same = false;
              break;
            }
        check(same, "npaint: and the reloaded document composites BIT-IDENTICALLY");

        // A second generation, which is the claim that actually matters: one
        // round trip only shows the reader kept what the writer had in hand.
        const char* path2 = "selftest_vector_roundtrip2.npaint";
        const NpaintSaveResult again = saveNpaint(loaded.document, path2, NpaintSaveOptions{});
        check(again.ok, "npaint: a second save of the reloaded document succeeds");
        if (again.ok) {
          const NpaintLoadResult twice = loadNpaint(path2);
          check(twice.ok && twice.document.layers.size() == 2 &&
                    vectorContentHash(twice.document.layers[1].shapes, {}) ==
                        vectorContentHash(doc.layers[1].shapes, {}),
                "npaint: and a SECOND generation is still identical");
        }
        std::remove(path2);

        // **Edit the geometry, save again WITH THE CARRY, and the EDIT must
        // win.** This is the assertion that catches a missing
        // `isLayerAttributeRecognised()` entry for `np:vector`, and none of
        // the round trips above can: without that entry the reader files
        // `np:vector` in the carry as an unknown attribute, and the writer
        // then emits BOTH its own fresh one and the carried stale one on the
        // same part, leaving OpenImageIO's last-write-wins to choose. On a
        // document that has not been edited the two are byte-identical and
        // every assertion above passes -- which is exactly how the `np:text`
        // side of this file stayed green under the same sabotage until an
        // edit-after-load case was added to section 13.
        //
        // **`&loaded.carry` is the whole point of this block.** Without the
        // carry, `saveNpaint()` has no unknown attributes to replay and the
        // double-`np:vector` hazard is unreachable. PRD I10's carry-forward
        // is a real save path -- every save of an opened document takes it --
        // so testing the write path without it tests the wrong path.
        //
        // The user-visible failure is: open a drawing, drag a shape, save,
        // reopen, and the shape is back where it started. So the test drags.
        Document edited = loaded.document;
        VectorShape moved = rectShape(40.0f, 40.0f, 60.0f, 52.0f);
        moved.id = edited.layers[1].shapes[0].id;
        moved.name = "box moved after the round trip";
        moved.fill.on = true;
        moved.fill.rgba = {1.0f, 0.0f, 0.25f, 1.0f};
        moved.pivot = PathPoint{50.0f, 46.0f};
        edited.layers[1].shapes[0] = moved;
        // A SECOND shape as well: a stale `np:vector` winning would restore
        // the one-shape list, so the shape COUNT separates the two payloads
        // on its own, without relying on the hash.
        VectorShape drawn = rectShape(2.0f, 2.0f, 10.0f, 10.0f);
        drawn.id = 4;
        drawn.name = "drawn after the round trip";
        edited.layers[1].shapes.push_back(drawn);
        edited.layers[1].nextShapeId = 5;
        const uint64_t editedHash = vectorContentHash(edited.layers[1].shapes, {});
        // Guard the guard: if the edit did not actually change the payload the
        // assertions below would pass under the sabotage for the wrong reason,
        // because a stale attribute winning would be indistinguishable from a
        // fresh one.
        check(editedHash != vectorContentHash(doc.layers[1].shapes, {}),
              "npaint: the post-load edit really does change the vector payload (without "
              "this, the stale-attribute assertions below could not tell the two apart)");

        const char* path3 = "selftest_vector_edited.npaint";
        const NpaintSaveResult afterEdit =
            saveNpaint(edited, path3, NpaintSaveOptions{}, &loaded.carry);
        check(afterEdit.ok, "npaint: saving a Vector layer that was edited after loading works");
        if (afterEdit.ok) {
          const NpaintLoadResult reread = loadNpaint(path3);
          const bool shaped = reread.ok && reread.document.layers.size() == 2;
          check(shaped && reread.document.layers[1].shapes.size() == 2,
                "npaint: and BOTH shapes come back -- one shape here would be the stale "
                "np:vector the file was opened with, replayed out of the carry");
          check(shaped && vectorContentHash(reread.document.layers[1].shapes, {}) == editedHash,
                "npaint: the EDIT comes back, not the geometry the file was opened with -- a "
                "stale np:vector left in the carry alongside a fresh one would silently win "
                "here and nowhere else");
          check(shaped && reread.document.layers[1].nextShapeId == 5,
                "npaint: including the id counter the edit advanced");
        }
        std::remove(path3);
      }
    }
    std::remove(path);

    // An EMPTY Vector layer must not write the attribute at all, so a document
    // without vector content keeps producing the bytes it produced before
    // `np:vector` existed. Asserted through the reader rather than by reading
    // the header, because that is the property callers depend on.
    Document emptyVec = Document::createBlank(kW, kH, WorkingSpace{});
    emptyVec.layers.clear();
    addLayer(emptyVec, 0, makeVectorLayer("no shapes"));
    const char* p3 = "selftest_vector_empty.npaint";
    const NpaintSaveResult e = saveNpaint(emptyVec, p3, NpaintSaveOptions{});
    check(e.ok, "npaint: an empty Vector layer saves");
    if (e.ok) {
      const NpaintLoadResult back = loadNpaint(p3);
      check(back.ok && back.document.layers.size() == 1 &&
                back.document.layers[0].kind == LayerKind::Vector &&
                back.document.layers[0].shapes.empty(),
            "npaint: and comes back as an empty Vector layer, not as an RGB one");
    }
    std::remove(p3);
  }

  // Sections 13 and 13b both need a payload this build CANNOT write -- a
  // future carrier version -- inside a real file, and there is one way to get
  // it: save a document, then patch the version tag of its only `oldTag` in
  // the EXR header (same length, and an EXR header is not compressed). The
  // Strokes layer's section F2 makes its `npdabs3:` file the same way.
  auto retagFile = [](const char* from, const char* to, const std::string& oldTag,
                      const std::string& newTag) {
    if (oldTag.size() != newTag.size()) return false;
    std::ifstream in(from, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t at = bytes.find(oldTag);
    if (at == std::string::npos || bytes.find(oldTag, at + 1) != std::string::npos) return false;
    bytes.replace(at, oldTag.size(), newTag);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
  };
  // The value of the attribute `name` held in layer `layer`'s carry, or "" --
  // the loader files an `np:text` / `np:flats` there only when it could not
  // decode it, so "" after a reload also means "this build read it".
  auto carriedAttr = [](const NpaintLoadResult& r, size_t layer, const char* name) {
    if (r.carry.layerAttributes.size() <= layer) return std::string();
    for (const NpaintAttribute& a : r.carry.layerAttributes[layer])
      if (a.name == name) return a.stringValue;
    return std::string();
  };

  // --- 13. A Text layer's .npaint round trip (PLAN.md phase 14) ------------
  //
  // Here rather than in app/selftest/TextContent.cpp for one concrete reason:
  // that section is guarded on `shaperAvailable()`, because every assertion in
  // it shapes real text. **A round trip does not shape** -- it stores and
  // reloads a string, a style and a paint -- so guarding it would leave the
  // file format untested on exactly the build (text/StubShaper.cpp) where a
  // document must still open and save correctly.
  //
  // The claim is the one `np:vector`'s section above makes, plus the two
  // places Text deliberately behaves DIFFERENTLY: the attribute is written
  // even for an empty string, and the reader warns when a Text part carries no
  // `np:text` at all.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("under"));
    addLayer(doc, 1, makeTextLayer("caption"));
    Layer& t = doc.layers[1];
    t.opacity = 0.375f;
    t.blend = "screen";
    // Every field non-default, and a string with a 2-, a 3- and a 4-byte UTF-8
    // sequence: a serialiser that mishandled a continuation byte would still
    // round-trip pure ASCII.
    t.text.utf8 = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xA8";
    t.text.style.fontFamily = "Georgia";
    t.text.style.sizePx = 37.5f;
    t.text.style.tracking = 1.25f;
    t.text.style.leading = 44.0f;
    t.text.style.bold = true;
    t.text.style.italic = true;
    t.text.frame.width = 300.0f;
    t.text.frame.height = 120.0f;
    t.text.align = TextAlign::Justified;
    t.text.origin = PathPoint{11.5f, 23.25f};
    t.text.fill.on = true;
    t.text.fill.rgba = {0.5f, 0.25f, 0.125f, 0.75f};
    t.text.stroke.on = true;
    t.text.stroke.rgba = {0.0f, 1.0f, 0.5f, 1.0f};
    t.text.strokeStyle.width = 1.5f;

    const char* tp = "selftest_text_roundtrip.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, tp, NpaintSaveOptions{});
    check(saved.ok, "npaint: a document containing a Text layer SAVES (it used to be refused "
                    "by name -- the kind had no on-disk representation)");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(tp);
      check(loaded.ok, "npaint: and the Text layer loads back");
      if (loaded.ok && loaded.document.layers.size() == 2) {
        const Layer& r = loaded.document.layers[1];
        check(r.kind == LayerKind::Text,
              "npaint: it is still a Text layer, not an RGB one -- the text is what was "
              "stored, not a picture of it");
        check(!r.rgbTiles.has_value() && !r.pigmentTiles.has_value(),
              "npaint: and it came back with no tile store, which is the kind's definition");
        check(r.name == "caption" && r.blend == "screen" && r.opacity == 0.375f,
              "npaint: its ordinary layer metadata survives");
        // The hash over every field is the strongest single statement of
        // fidelity here, exactly as it is for `np:vector` above: a field the
        // writer skipped or the reader never read back changes it.
        check(textContentHash(r.text) == textContentHash(t.text),
              "npaint: the text's content hash is identical across the round trip, so NO "
              "field was silently dropped");
        check(r.text.utf8 == t.text.utf8,
              "npaint: including the multi-byte string, byte for byte");
        check(r.text.style.sizePx == 37.5f && r.text.style.tracking == 1.25f,
              "npaint: and the float fields exactly, not nearly -- io/TextSerial carries "
              "IEEE-754 bit patterns, not decimal renderings");

        const char* tp2 = "selftest_text_roundtrip2.npaint";
        const NpaintSaveResult again = saveNpaint(loaded.document, tp2, NpaintSaveOptions{});
        check(again.ok, "npaint: a second save of the reloaded document succeeds");
        if (again.ok) {
          const NpaintLoadResult twice = loadNpaint(tp2);
          check(twice.ok && twice.document.layers.size() == 2 &&
                    textContentHash(twice.document.layers[1].text) == textContentHash(t.text),
                "npaint: and a SECOND generation is still identical -- one round trip only "
                "shows the reader kept what the writer had in hand");
        }
        std::remove(tp2);

        // **Edit the text, save again, and the EDIT must win.** This is the
        // assertion that catches a missing `isLayerAttributeRecognised()`
        // entry, and the round trips above provably cannot: without that
        // entry the reader files `np:text` in the carry as an unknown
        // attribute, and the writer then emits BOTH its own and the carried
        // one, leaving OpenImageIO's last-write-wins to choose. On a document
        // that has not been edited the two are byte-identical and every
        // assertion above passes -- the version this test had before this
        // block stayed green under exactly that sabotage.
        //
        // The user-visible failure is: type, save, reopen, and your typing is
        // gone. So the test types.
        Document edited = loaded.document;
        edited.layers[1].text.utf8 = "edited after the round trip";
        edited.layers[1].text.style.sizePx = 8.5f;
        const char* tp4 = "selftest_text_edited.npaint";
        // **`&loaded.carry` is the whole point of this block.** Without the
        // carry, `saveNpaint()` has no unknown attributes to replay and the
        // double-`np:text` hazard is unreachable -- which is exactly why the
        // first version of this test stayed green under the sabotage. PRD
        // I10's carry-forward is a real save path (every save of an opened
        // document takes it), so testing the write path without it tests the
        // wrong path.
        const NpaintSaveResult afterEdit =
            saveNpaint(edited, tp4, NpaintSaveOptions{}, &loaded.carry);
        check(afterEdit.ok, "npaint: saving a Text layer that was edited after loading works");
        if (afterEdit.ok) {
          const NpaintLoadResult reread = loadNpaint(tp4);
          check(reread.ok && reread.document.layers.size() == 2 &&
                    reread.document.layers[1].text.utf8 == "edited after the round trip" &&
                    reread.document.layers[1].text.style.sizePx == 8.5f,
                "npaint: and the EDIT comes back, not the string the file was opened with -- "
                "a stale np:text left in the carry alongside a fresh one would silently win "
                "here and nowhere else");
        }
        std::remove(tp4);
      }
    }
    std::remove(tp);

    // **An EMPTY Text layer still writes `np:text`, unlike an empty Vector
    // layer.** An empty string with a chosen font and size is a real state --
    // the state a layer is in between being created and being typed into --
    // and the guard `np:vector` uses would silently discard the font choice on
    // save. Asserted through the reader, which is the property callers depend
    // on: the style has to survive even though the string is empty.
    Document emptyText = Document::createBlank(kW, kH, WorkingSpace{});
    emptyText.layers.clear();
    addLayer(emptyText, 0, makeTextLayer("nothing typed yet"));
    emptyText.layers[0].text.style.fontFamily = "Courier";
    emptyText.layers[0].text.style.sizePx = 96.0f;
    const char* tp3 = "selftest_text_empty.npaint";
    const NpaintSaveResult e2 = saveNpaint(emptyText, tp3, NpaintSaveOptions{});
    check(e2.ok, "npaint: a Text layer with an empty string saves");
    if (e2.ok) {
      const NpaintLoadResult back = loadNpaint(tp3);
      check(back.ok && back.document.layers.size() == 1 &&
                back.document.layers[0].kind == LayerKind::Text &&
                back.document.layers[0].text.utf8.empty(),
            "npaint: and comes back as an empty Text layer, not as an RGB one");
      check(back.ok && !back.document.layers.empty() &&
                back.document.layers[0].text.style.fontFamily == "Courier" &&
                back.document.layers[0].text.style.sizePx == 96.0f,
            "npaint: with the FONT AND SIZE intact -- the reason an empty Text layer writes "
            "its attribute where an empty Vector layer does not");
    }
    std::remove(tp3);

    // **A future `np:text` survives a save by this build (PRD I10).** The
    // loader opens an `nptext3:` Text layer with a default `TextContent` and
    // keeps the attribute in the carry, promising to write it back verbatim.
    // The writer used to break that promise: it wrote `np:text` for every
    // Text layer unconditionally, and its carry replay skips a carried
    // `np:text` whenever it wrote its own, so the next save replaced a newer
    // build's text with an EMPTY one -- and reopened cleanly, because that
    // empty payload is one this build reads. The round trips above cannot see
    // it: they never hold a payload this build cannot decode.
    {
      const char* fa = "selftest_text_future_a.npaint";
      const char* fb = "selftest_text_future_b.npaint";
      const char* fc = "selftest_text_future_c.npaint";
      const char* fd = "selftest_text_future_d.npaint";
      const char* fe = "selftest_text_future_typed.npaint";
      const std::string v1(kTextContentSerialPrefix);
      const std::string written = serializeTextContent(t.text);
      const std::string futureValue = "nptext3:" + written.substr(v1.size());
      const bool patched = written.compare(0, v1.size(), v1) == 0 &&
                           saveNpaint(doc, fa, NpaintSaveOptions{}).ok &&
                           retagFile(fa, fb, v1, "nptext3:");
      check(patched, "npaint future: premise -- a Text file was written and its one nptext1: "
                     "tag patched to nptext3:");
      const NpaintLoadResult future = patched ? loadNpaint(fb) : NpaintLoadResult{};
      bool named = false;
      for (const std::string& w : future.warnings)
        if (w.find("nptext1:") != std::string::npos && w.find("nptext2:") != std::string::npos)
          named = true;
      const bool opened = future.ok && future.document.layers.size() == 2 &&
                          future.document.layers[1].kind == LayerKind::Text;
      check(opened &&
                serializeTextContent(future.document.layers[1].text) ==
                    serializeTextContent(TextContent{}) &&
                named && carriedAttr(future, 1, "np:text") == futureValue,
            "npaint future: an nptext3: Text layer opens with DEFAULT content, a warning naming "
            "the versions this build reads, and its payload held in the carry byte for byte");
      std::string gen1, gen2;
      if (opened && saveNpaint(future.document, fc, NpaintSaveOptions{}, &future.carry).ok) {
        const NpaintLoadResult again = loadNpaint(fc);
        gen1 = carriedAttr(again, 1, "np:text");
        if (again.ok && saveNpaint(again.document, fd, NpaintSaveOptions{}, &again.carry).ok)
          gen2 = carriedAttr(loadNpaint(fd), 1, "np:text");
      }
      check(gen1 == futureValue,
            "npaint future: SAVED by this build, the nptext3: payload is written back "
            "verbatim -- not replaced by the empty default the layer opened with (PRD I10)");
      check(gen2 == futureValue,
            "npaint future: and it is still verbatim after a SECOND save of that reopened file");
      // The other half of the rule, so the fix cannot pass by always
      // preferring the carry: once the user types into the layer, THEIR text
      // is what saves, alone. Both copies on one part would leave
      // OpenImageIO's last-write-wins to pick, and it picks the carried one.
      bool typedWins = false;
      if (opened) {
        Document typed = future.document;
        typed.layers[1].text.utf8 = "typed over a newer build's text";
        if (saveNpaint(typed, fe, NpaintSaveOptions{}, &future.carry).ok) {
          const NpaintLoadResult r = loadNpaint(fe);
          typedWins = r.ok && r.document.layers.size() == 2 &&
                      r.document.layers[1].text.utf8 == "typed over a newer build's text" &&
                      carriedAttr(r, 1, "np:text").empty();
        }
      }
      check(typedWins,
            "npaint future: but once the user TYPES into that layer, their text is what saves "
            "-- this build cannot merge an edit into a payload it cannot read");
      for (const char* p : {fa, fb, fc, fd, fe}) std::remove(p);
    }
  }

  // --- 13b. A Flats layer's .npaint round trip -----------------------------
  //
  // Section 13's claims for `np:flats`, which the loader handles on
  // `np:text`'s rules exactly: the content is parameters, repairs and a
  // palette rather than pixels, it is written even at its defaults (a fresh
  // Flats layer still flats the drawing), and a payload this build cannot
  // decode is carried to the next save. Nothing else in `--selftest` writes a
  // Flats layer to a file.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("inks"));
    addLayer(doc, 1, makeFlatsLayer("flats"));
    Layer& f = doc.layers[1];
    f.opacity = 0.5f;
    f.blend = "multiply";
    // Something in each of the three parts: parameters, a recorded repair and
    // the id allocator it advanced, and a palette with a HOLE -- position is
    // meaning there (flats/Model.hpp), so a reader that compacted it would
    // move every later swatch.
    f.flats.params.gapSize = 5;
    f.flats.params.sheet = 0.0f;
    f.flats.params.lineThreshold = 0.125f;
    FlatBridgeStroke bridge;
    bridge.id = f.flats.edits.nextId++;
    bridge.pts = {4.0f, 4.0f, 20.0f, 9.5f};
    f.flats.edits.bridges.push_back(bridge);
    f.flats.palette = {FlatRgb{200, 150, 120}, std::nullopt, FlatRgb{10, 20, 30}};
    const std::string written = serializeFlatsContent(f.flats);

    const char* fa = "selftest_flats_roundtrip.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, fa, NpaintSaveOptions{});
    check(saved.ok, "npaint: a document containing a Flats layer saves");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());
    const NpaintLoadResult loaded = saved.ok ? loadNpaint(fa) : NpaintLoadResult{};
    const bool shaped = loaded.ok && loaded.document.layers.size() == 2 &&
                        loaded.document.layers[1].kind == LayerKind::Flats;
    check(shaped && !loaded.document.layers[1].rgbTiles.has_value() &&
              loaded.document.layers[1].blend == "multiply" &&
              loaded.document.layers[1].opacity == 0.5f,
          "npaint: and loads back as a Flats layer with no tiles and its layer metadata");
    check(shaped && serializeFlatsContent(loaded.document.layers[1].flats) == written &&
              loaded.document.layers[1].flats.params.gapSize == 5 &&
              loaded.document.layers[1].flats.edits.bridges.size() == 1 &&
              loaded.document.layers[1].flats.palette.size() == 3 &&
              !loaded.document.layers[1].flats.palette[1].has_value(),
          "npaint: its parameters, repair and palette -- hole included -- come back exactly");

    // **A future `np:flats` survives a save by this build (PRD I10)** --
    // section 13's closing block, for the same writer defect: `np:flats` was
    // written unconditionally for the kind, so an `npflats2:` payload the
    // loader carried was replaced on the next save by the default content
    // the layer opened with.
    const char* fb = "selftest_flats_future_b.npaint";
    const char* fc = "selftest_flats_future_c.npaint";
    const char* fd = "selftest_flats_future_d.npaint";
    const char* fe = "selftest_flats_future_edited.npaint";
    const std::string v1 = "npflats1:";
    const std::string futureValue = "npflats2:" + written.substr(v1.size());
    const bool patched =
        saved.ok && written.compare(0, v1.size(), v1) == 0 && retagFile(fa, fb, v1, "npflats2:");
    check(patched, "npaint future: premise -- the Flats file's one npflats1: tag patched to "
                   "npflats2:");
    const NpaintLoadResult future = patched ? loadNpaint(fb) : NpaintLoadResult{};
    bool named = false;
    for (const std::string& w : future.warnings)
      if (w.find("npflats1:") != std::string::npos) named = true;
    const bool opened = future.ok && future.document.layers.size() == 2 &&
                        future.document.layers[1].kind == LayerKind::Flats;
    check(opened &&
              serializeFlatsContent(future.document.layers[1].flats) ==
                  serializeFlatsContent(FlatsContent{}) &&
              named && carriedAttr(future, 1, "np:flats") == futureValue,
          "npaint future: an npflats2: Flats layer opens with DEFAULT content, a warning "
          "naming the version this build reads, and its payload held in the carry byte for byte");
    std::string gen1, gen2;
    if (opened && saveNpaint(future.document, fc, NpaintSaveOptions{}, &future.carry).ok) {
      const NpaintLoadResult again = loadNpaint(fc);
      gen1 = carriedAttr(again, 1, "np:flats");
      if (again.ok && saveNpaint(again.document, fd, NpaintSaveOptions{}, &again.carry).ok)
        gen2 = carriedAttr(loadNpaint(fd), 1, "np:flats");
    }
    check(gen1 == futureValue,
          "npaint future: SAVED by this build, the npflats2: payload is written back verbatim "
          "-- not replaced by the default parameters the layer opened with (PRD I10)");
    check(gen2 == futureValue,
          "npaint future: and it is still verbatim after a SECOND save of that reopened file");
    // Section 13's other half: a user edit to the carried layer wins, alone.
    bool editWins = false;
    if (opened) {
      Document edited = future.document;
      edited.layers[1].flats.params.gapSize = 11;
      if (saveNpaint(edited, fe, NpaintSaveOptions{}, &future.carry).ok) {
        const NpaintLoadResult r = loadNpaint(fe);
        editWins = r.ok && r.document.layers.size() == 2 &&
                   r.document.layers[1].flats.params.gapSize == 11 &&
                   carriedAttr(r, 1, "np:flats").empty();
      }
    }
    check(editWins,
          "npaint future: but once the user changes that layer's parameters, theirs are what "
          "save -- this build cannot merge an edit into a payload it cannot read");
    for (const char* p : {fa, fb, fc, fd, fe}) std::remove(p);
  }

  // --- 14. The rest of the layer attribute table, edited after load --------
  //
  // Sections 12 and 13 each close ONE name in
  // `io/NpaintFile.cpp`'s `isLayerAttributeRecognised()` list. This section
  // closes four more, by exactly the same argument and in one fixture.
  //
  // The hazard, once, in full: an attribute missing from that list is filed by
  // the reader as an *unknown* attribute in `NpaintCarry::layerAttributes`.
  // The writer replays the carry AFTER writing its own attributes, so the part
  // ends up with two attributes of the same name and OpenImageIO's
  // last-write-wins picks the CARRIED, stale one. Nothing errors. The document
  // simply loads as something other than what was saved.
  //
  // Why an ordinary round trip cannot see this: on an unedited document the
  // two copies hold the same value, so every round-trip and second-generation
  // assertion passes. **The document has to be edited after loading, and the
  // carry has to be handed back to the save** -- which is the real save path
  // for every opened document, so this is the ordinary case and not a corner.
  //
  // The four names below are the ones whose value a caller can change with a
  // plain assignment and whose payload this build can construct. The audit's
  // remaining uncovered names -- `np:parent`, `np:groupId`, `np:mask`,
  // `np:ops`, and the document-level `np:comps`, `np:version`, `np:basis`,
  // `np:tileSize` -- need a Group layer, a decodable op stack, or a
  // hand-built carry, and are deliberately NOT faked here: a test that cannot
  // fail is worse than an admitted gap.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("base"));
    addLayer(doc, 1, makeRgbLayer("decorated"));
    Layer& d = doc.layers[1];
    // Every one of the four is written by the writer ONLY when set (`np:label`
    // and `np:link` when non-empty/non-zero, `np:clipped` and `np:alphaLocked`
    // only when true), so the fixture has to set them or the file holds
    // nothing for the carry to go stale with.
    d.clipped = true;
    d.alphaLocked = true;
    d.colorLabel = "red";
    d.linkGroup = 7;

    const char* fp = "selftest_npattrs_fixture.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, fp, NpaintSaveOptions{});
    check(saved.ok, "npaint: a layer carrying clipped/alphaLocked/label/link saves");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(fp);
      const bool shaped = loaded.ok && loaded.document.layers.size() == 2;
      check(shaped && loaded.document.layers[1].clipped &&
                loaded.document.layers[1].alphaLocked &&
                loaded.document.layers[1].colorLabel == "red" &&
                loaded.document.layers[1].linkGroup == 7,
            "npaint: and all four come back on the ordinary round trip");

      if (shaped) {
        // Every field moved to a DIFFERENT value. The two booleans go true ->
        // false on purpose: that is the direction in which the writer emits
        // nothing of its own, so a carried copy is the only `np:clipped` /
        // `np:alphaLocked` in the file and wins outright rather than by a
        // tie-break. The two non-booleans move to another non-default value
        // rather than to the default, so BOTH copies are present and the
        // assertion is about which one OpenImageIO kept.
        Document edited = loaded.document;
        edited.layers[1].clipped = false;
        edited.layers[1].alphaLocked = false;
        edited.layers[1].colorLabel = "blue";
        edited.layers[1].linkGroup = 3;

        const char* ep = "selftest_npattrs_edited.npaint";
        const NpaintSaveResult afterEdit =
            saveNpaint(edited, ep, NpaintSaveOptions{}, &loaded.carry);
        check(afterEdit.ok,
              "npaint: saving those four fields after an edit, WITH the carry, works");
        if (afterEdit.ok) {
          const NpaintLoadResult r = loadNpaint(ep);
          const bool rs = r.ok && r.document.layers.size() == 2;
          check(rs && !r.document.layers[1].clipped,
                "npaint: an UNCLIPPED layer stays unclipped -- a carried np:clipped replayed "
                "beside no fresh one would silently re-clip it (the user unclips, saves, "
                "reopens, and the layer is clipped again)");
          check(rs && !r.document.layers[1].alphaLocked,
                "npaint: an UNLOCKED alpha stays unlocked, for the same reason and by the "
                "same mechanism");
          check(rs && r.document.layers[1].colorLabel == "blue",
                "npaint: the NEW colour label wins, not the one the file was opened with -- "
                "two np:label attributes on one part and last-write-wins picks the carried");
          check(rs && r.document.layers[1].linkGroup == 3,
                "npaint: and the NEW link group wins, not the stale carried one");
        }
        std::remove(ep);
      }
    }
    std::remove(fp);
  }

  // --- 15. The three attributes only a GROUP part carries -----------------
  //
  // `np:parent`, `np:groupId` and `np:mask` are section 14's hazard again, but
  // no plain RGB fixture can reach them: `np:groupId` and `np:mask` are
  // written only on a Group part, and `np:parent` -- though written
  // unconditionally -- is an EMPTY string on an ungrouped layer, which this
  // OpenImageIO drops before it reaches the file (see io/NpaintFile.cpp's
  // NpaintAttribute comment). An attribute that never lands is an attribute
  // the carry never picks up, so the double-write is unreachable until a real
  // group exists. Hence the group.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("member A"));
    addLayer(doc, 1, makeRgbLayer("member B"));
    const LayerSetOpResult grouped = applyLayerSetOp(doc, LayerSetCommand::GroupLayers,
                                                     makeLayerSelection({0, 1}));
    check(grouped.ok && doc.layers.size() == 3 && doc.layers[2].kind == LayerKind::Group,
          "npaint: a real Group layer is built for the group-only attributes");

    if (grouped.ok && doc.layers.size() == 3) {
      const size_t gi = 2;
      const std::string tag0 = doc.layers[gi].groupTag;

      const char* gp = "selftest_npattrs_group.npaint";
      const NpaintSaveResult saved = saveNpaint(doc, gp, NpaintSaveOptions{});
      check(saved.ok, "npaint: a grouped document saves");
      if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

      if (saved.ok) {
        const NpaintLoadResult loaded = loadNpaint(gp);
        const bool shaped = loaded.ok && loaded.document.layers.size() == 3 &&
                            loaded.document.layers[gi].kind == LayerKind::Group;
        check(shaped && loaded.document.layers[gi].groupTag == tag0 &&
                  loaded.document.layers[0].parent == tag0 &&
                  loaded.document.layers[1].parent == tag0,
              "npaint: the group's tag and both members' np:parent come back");

        if (shaped) {
          Document edited = loaded.document;
          // (a) Ungroup member A: `np:parent` goes to the empty string, which
          //     the writer emits and OpenImageIO then DROPS -- so a carried
          //     stale tag would be the only np:parent left on the part and
          //     would win outright. The user gesture is: ungroup one layer,
          //     save, reopen, and it is back inside the group.
          edited.layers[0].parent.clear();
          // (b) Rename the group's identity. Member B is moved with it, so
          //     the document stays internally consistent and the assertion is
          //     purely about which np:groupId survived.
          const std::string tag1 = tag0 + "-renamed";
          edited.layers[gi].groupTag = tag1;
          edited.layers[1].parent = tag1;
          // (c) Give the group a mask it did not have. The writer emits
          //     np:mask=1 and a real mask channel; the carried np:mask=0 would
          //     win and the reader -- which trusts the attribute, not the
          //     channel's presence -- would drop the mask on the floor.
          const LayerOpResult masked = addLayerMask(edited, gi);
          check(masked.ok && edited.layers[gi].mask.has_value(),
                "npaint: a mask can be added to the group after loading");

          const char* ep = "selftest_npattrs_group_edited.npaint";
          const NpaintSaveResult afterEdit =
              saveNpaint(edited, ep, NpaintSaveOptions{}, &loaded.carry);
          check(afterEdit.ok, "npaint: saving the edited group WITH the carry works");
          if (afterEdit.ok) {
            const NpaintLoadResult r = loadNpaint(ep);
            const bool rs = r.ok && r.document.layers.size() == 3;
            check(rs && r.document.layers[0].parent.empty(),
                  "npaint: the UNGROUPED member stays ungrouped -- a carried np:parent "
                  "replayed beside a dropped empty one would silently put it back in the "
                  "group (ungroup, save, reopen, still grouped)");
            check(rs && r.document.layers[gi].groupTag == tag1 &&
                      r.document.layers[1].parent == tag1,
                  "npaint: the group's NEW np:groupId wins, not the one the file was opened "
                  "with");
            check(rs && r.document.layers[gi].mask.has_value(),
                  "npaint: and the newly added group mask survives -- np:mask, not the "
                  "channel's presence, is what the reader trusts, so a stale carried 0 "
                  "would throw the mask away without a word");
          }
          std::remove(ep);
        }
      }
      std::remove(gp);
    }
  }

  // --- 16. The document-level twin, isDocumentAttributeRecognised() -------
  //
  // Same hazard, other table. `np:version`, `np:basis` and `np:tileSize` have
  // one property the layer attributes do not: this build writes a CONSTANT
  // for each, so a file this build wrote holds exactly the value it would
  // write again, and a stale carried copy is byte-identical to the fresh one.
  // No edit-after-load can separate them, because there is nothing on the
  // Document to edit.
  //
  // So the carry is built BY HAND with values this build would never write.
  // That is not an artificial state: it is precisely the carry the loader
  // itself produces the moment a name goes missing from the table, and it is
  // also what a foreign or newer writer's file would leave behind. The claim
  // under test is the writer's replay rule -- "a name this build owns is
  // never replayed out of the carry beside this build's own" -- asserted
  // where it is observable, on the file that comes back.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});

    NpaintCarry carry;
    NpaintAttribute ver;
    ver.name = "np:version";
    ver.type = NpaintAttribute::Type::Int;
    ver.intValue = 9999;  // no build will ever stamp this
    NpaintAttribute tile;
    tile.name = "np:tileSize";
    tile.type = NpaintAttribute::Type::Int;
    tile.intValue = 7;  // not a power of two, and not kTileSize
    NpaintAttribute basis;
    basis.name = "np:basis";
    basis.type = NpaintAttribute::Type::String;
    basis.stringValue = "stale-carried-basis";
    carry.documentAttributes = {ver, tile, basis};
    // Left EMPTY on purpose: `NpaintCarry::basis` is the *supported* route by
    // which a foreign basis reaches the writer, and it is not the route under
    // test. With it empty the writer stamps the document's own basis, so the
    // only way "stale-carried-basis" can reach the file is the replay bug.
    carry.basis.clear();

    const char* dp = "selftest_npattrs_doc.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, dp, NpaintSaveOptions{}, &carry);
    check(saved.ok, "npaint: a save carrying hand-built document attributes succeeds");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

    if (saved.ok) {
      const NpaintLoadResult back = loadNpaint(dp);
      check(back.ok, "npaint: and the file it wrote loads");
      if (back.ok) {
        auto warnedAbout = [&](const char* needle) {
          for (const std::string& w : back.warnings)
            if (w.find(needle) != std::string::npos) return true;
          return false;
        };
        check(back.carry.sourceVersion != 9999 && !warnedAbout("np:version 9999"),
              "npaint: the carried np:version was DROPPED, not written beside this build's -- "
              "two np:version attributes and last-write-wins would make every file this build "
              "saves claim a format it does not speak");
        check(!warnedAbout("np:tileSize 7"),
              "npaint: the carried np:tileSize was dropped too, so the file does not describe "
              "a tile layout it was not written in");
        check(back.carry.basis != "stale-carried-basis" &&
                  back.document.pigmentBasis != "stale-carried-basis",
              "npaint: and the carried np:basis was dropped -- NpaintCarry::basis is the "
              "supported route for a foreign basis and it was deliberately empty here, so a "
              "basis arriving by replay is the bug, and it would mislabel every pigment "
              "latent in the file");
        // A general tripwire for the NEXT document attribute somebody adds,
        // and deliberately not a restatement of the three checks above: the
        // reader consumes a correctly-typed np:version / np:basis /
        // np:tileSize with its own unconditional `continue`, BEFORE
        // `isDocumentAttributeRecognised()` is consulted at all, so none of
        // those three can reach this vector however the table is edited. What
        // this does catch is a name the writer emits that the reader has no
        // case for: it comes straight back as unknown, and the save after
        // that doubles it. Empty is the only correct answer for a file this
        // build wrote.
        check(back.carry.documentAttributes.empty(),
              "npaint: a document this build wrote leaves NOTHING in the document-attribute "
              "carry -- an entry here is a name the writer emits and the reader has no case "
              "for, which is where a doubled attribute begins");
      }
    }
    std::remove(dp);
  }

  // --- 17. The last two: np:ops and np:comps ------------------------------
  //
  // Both hold a payload rather than a scalar, which is why they are last, and
  // both have a documented carry EXCEPTION in the writer -- an `np:ops` is
  // replayed when the layer's own stack is empty, and an `np:comps` when the
  // document has no comps, because in each of those cases the carried copy is
  // an undecodable payload this build has nothing of its own to put in place
  // of (PRD I10). The exception is what makes the test's direction matter:
  // **the edit has to leave a non-empty stack and a non-empty comp list**, or
  // the carried copy is replayed legitimately and the assertion would be
  // asserting the exception rather than the bug.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    addLayer(doc, 0, makeRgbLayer("bottom"));
    addLayer(doc, 1, makeRgbLayer("adjusted"));
    Op exposure;
    exposure.pointKind = PointOpKind::Exposure;
    exposure.exposure.stops = 1.0f;
    doc.layers[1].ops.add(exposure);
    const LayerOpResult captured = captureLayerComp(doc, "as opened");
    check(captured.ok && doc.comps.size() == 1,
          "npaint: a fixture with one op and one layer comp is built");

    const char* fp = "selftest_npattrs_payload.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, fp, NpaintSaveOptions{});
    check(saved.ok, "npaint: and it saves");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());

    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(fp);
      const bool shaped = loaded.ok && loaded.document.layers.size() == 2;
      check(shaped && loaded.document.layers[1].ops.size() == 1 &&
                loaded.document.comps.size() == 1,
            "npaint: the op stack and the comp both come back on the ordinary round trip");

      if (shaped) {
        Document edited = loaded.document;
        // A SECOND op and a SECOND comp: both lists stay non-empty, so the
        // writer emits its own copy of each and the carry exceptions above do
        // not apply. A stale carried payload winning shows up as a list that
        // is one entry short -- the user adds an adjustment, saves, reopens,
        // and it is gone.
        Op saturation;
        saturation.pointKind = PointOpKind::Saturation;
        saturation.saturation.scale = 0.5f;
        edited.layers[1].ops.add(saturation);
        const LayerOpResult second = captureLayerComp(edited, "after the edit");
        check(second.ok && edited.comps.size() == 2,
              "npaint: a second op and a second comp are added after loading");

        // ==================================================================
        // `np:comps` IS NOT ASSERTED HERE, AND THAT IS THE FINDING
        // ==================================================================
        //
        // The comps assertion below is an ordinary round-trip claim. It does
        // NOT close `np:comps` in `isDocumentAttributeRecognised()`, and no
        // assertion can, because removing that entry has no observable
        // effect. Measured, not reasoned: with the entry removed the whole
        // suite stays green.
        //
        // Two independent reasons, both worth writing down because each on
        // its own would make a test here decorative:
        //
        //  1. The READER never files a decodable `np:comps` in
        //     `documentAttributes` -- its own block `continue`s first -- and
        //     when the payload is UNdecodable the writer's documented
        //     exception replays it deliberately. So on every carry the loader
        //     can actually produce, the table entry changes nothing.
        //
        //  2. Even from a hand-built carry holding a stale comps payload, the
        //     doubled attribute is harmless, because io/NpaintFile.cpp pushes
        //     its own `np:comps` onto part 0 AFTER the carry replay, not
        //     before. Last-write-wins therefore picks the FRESH copy. Every
        //     other document attribute is pushed before the replay and so
        //     loses -- which is exactly why section 16's three assertions
        //     bite and this one cannot.
        //
        // So `np:comps` is protected by a push ORDER that nothing states or
        // enforces, rather than by the recognition table. Moving that push
        // above the carry replay would make the hazard live and silent, and
        // no assertion in this suite would notice. Writing a green "test" for
        // it here would only record that the accident currently holds.
        const char* ep = "selftest_npattrs_payload_edited.npaint";
        const NpaintSaveResult afterEdit =
            saveNpaint(edited, ep, NpaintSaveOptions{}, &loaded.carry);
        check(afterEdit.ok, "npaint: saving the edited payloads WITH the carry works");
        if (afterEdit.ok) {
          const NpaintLoadResult r = loadNpaint(ep);
          const bool rs = r.ok && r.document.layers.size() == 2;
          check(rs && r.document.layers[1].ops.size() == 2,
                "npaint: BOTH ops come back -- one op here would be the stale np:ops the file "
                "was opened with, replayed out of the carry beside the fresh one");
          check(rs && r.document.comps.size() == 2 &&
                    r.document.comps[1].name == "after the edit",
                "npaint: and BOTH comps, the new one included -- np:comps is the document's "
                "only record of them, so a stale copy winning loses the capture outright");
        }
        std::remove(ep);
      }
    }
    std::remove(fp);
  }

  return ok;
}

}  // namespace np
