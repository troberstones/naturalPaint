#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/DirtyTiles.hpp"
#include "core/Path.hpp"
#include "core/VectorRaster.hpp"
#include "core/VectorShape.hpp"
#include "io/NpaintFile.hpp"
#include "io/PathSerial.hpp"

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

  // --- 5. The content hash discriminates -----------------------------------
  {
    std::vector<VectorShape> base;
    VectorShape s = rectShape(0.0f, 0.0f, 10.0f, 10.0f);
    s.fill.on = true;
    base.push_back(s);
    const uint64_t h0 = vectorContentHash(base);
    check(vectorContentHash(base) == h0, "hash: is stable for unchanged content");

    std::vector<VectorShape> moved = base;
    moved[0].path.subpaths[0].anchors[0].pt.x += 0.001f;
    check(vectorContentHash(moved) != h0, "hash: a sub-texel anchor move changes it");

    std::vector<VectorShape> handled = base;
    handled[0].path.subpaths[0].anchors[0].out.y += 1.0f;
    check(vectorContentHash(handled) != h0, "hash: a HANDLE move changes it, not just an anchor");

    std::vector<VectorShape> painted = base;
    painted[0].fill.rgba[2] = 0.5f;
    check(vectorContentHash(painted) != h0, "hash: a paint change changes it");

    std::vector<VectorShape> stroked = base;
    stroked[0].strokeStyle.dashOffset = 1.0f;
    check(vectorContentHash(stroked) != h0, "hash: a dash-offset change changes it");

    std::vector<VectorShape> pivoted = base;
    pivoted[0].pivot = PathPoint{1.0f, 2.0f};
    check(vectorContentHash(pivoted) != h0,
          "hash: a pivot move changes it, so a cache rebuild cannot drop the pivot");

    std::vector<VectorShape> ruled = base;
    ruled[0].path.rule = FillRule::EvenOdd;
    check(vectorContentHash(ruled) != h0, "hash: a fill-rule change changes it");
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
    const uint64_t h = vectorContentHash(doc.layers[0].shapes);
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
    check(cache.lookup(77, vectorContentHash(moved.layers[0].shapes)) != nullptr,
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
    check(vectorContentHash(back) == vectorContentHash(shapes),
          "serial: the content hash round-trips, so NO field was silently dropped");

    // Refusals. Each is a shape a `.npaint` could genuinely arrive carrying.
    std::vector<VectorShape> dummy;
    // A future tag on a payload that is otherwise PERFECTLY VALID. An earlier
    // revision of this check used "npvec2:00", which is refused whether or not
    // the version is inspected -- its two hex digits are truncated anyway --
    // so it passed with the version gate deleted. Sabotage found that; the
    // payload below can only be refused BY the version gate.
    const std::string futureTagged = "npvec2:" + encoded.substr(std::strlen(kVectorShapeSerialPrefix));
    check(!deserializeVectorShapes(futureTagged, &dummy, nullptr, &why),
          "serial: a FUTURE version is refused even when its payload is otherwise valid");
    check(why.find("npvec1:") != std::string::npos,
          "serial: and the refusal names the version this build speaks");
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
        check(vectorContentHash(v.shapes) == vectorContentHash(doc.layers[1].shapes),
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
                    vectorContentHash(twice.document.layers[1].shapes) ==
                        vectorContentHash(doc.layers[1].shapes),
                "npaint: and a SECOND generation is still identical");
        }
        std::remove(path2);
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

  return ok;
}

}  // namespace np
