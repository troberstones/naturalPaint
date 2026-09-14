#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/CropTool.hpp"
#include "app/MoveTool.hpp"
#include "core/Merge.hpp"
#include "app/TransformSession.hpp"
#include "core/Composite.hpp"
#include "core/LayerOps.hpp"
#include "core/TextContent.hpp"
#include "core/VectorShape.hpp"
#include "ops/DocumentTransform.hpp"

// Vector and Text layers following the Move tool, Free Transform, nudge and the
// whole-document transforms (Image Size, Canvas Size, crop). Headless.
namespace np {
namespace {

VectorShape rectShape(float x0, float y0, float x1, float y1) {
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
  s.fill.on = true;
  s.fill.rgba = {1.0f, 0.0f, 0.0f, 1.0f};
  return s;
}

Document vectorDoc(int32_t w, int32_t h, const VectorShape& shape) {
  Document doc = Document::createBlank(w, h, WorkingSpace{});
  doc.layers.clear();
  addLayer(doc, 0, makeVectorLayer("shapes"));
  doc.layers[0].shapes.push_back(shape);
  return doc;
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool nearPt(PathPoint p, float x, float y, float eps = 1e-4f) {
  return near(p.x, x, eps) && near(p.y, y, eps);
}

bool sameShapes(const std::vector<VectorShape>& a, const std::vector<VectorShape>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const Path& pa = a[i].path;
    const Path& pb = b[i].path;
    if (pa.subpaths.size() != pb.subpaths.size()) return false;
    for (size_t s = 0; s < pa.subpaths.size(); ++s) {
      const auto& aa = pa.subpaths[s].anchors;
      const auto& ab = pb.subpaths[s].anchors;
      if (aa.size() != ab.size()) return false;
      for (size_t k = 0; k < aa.size(); ++k)
        if (aa[k].pt.x != ab[k].pt.x || aa[k].pt.y != ab[k].pt.y || aa[k].in.x != ab[k].in.x ||
            aa[k].in.y != ab[k].in.y || aa[k].out.x != ab[k].out.x || aa[k].out.y != ab[k].out.y)
          return false;
    }
    if (a[i].strokeStyle.width != b[i].strokeStyle.width) return false;
  }
  return true;
}

}  // namespace

bool runVectorTransformTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  std::printf("[selftest] vector transform: shapes and text follow move and document transforms\n");

  const DocumentTransformParams params;

  // --- 1. Anchor mapping against a hand-computed affine ---------------------
  {
    // x' = 2x + 0.5y + 10, y' = -0.25x + 1.5y - 4; det = 3.125.
    Mat3 m;
    m.m = {2.0f, 0.5f, 10.0f, -0.25f, 1.5f, -4.0f, 0.0f, 0.0f, 1.0f};

    VectorShape s;
    SubPath sub;
    Anchor a;
    a.pt = PathPoint{3.0f, 7.0f};
    a.in = PathPoint{1.0f, 2.0f};
    a.out = PathPoint{-4.0f, 0.5f};
    sub.anchors.push_back(a);
    sub.anchors.push_back(a);
    s.path.subpaths.push_back(sub);
    s.clip = s.path;
    s.pivot = PathPoint{1.0f, 2.0f};
    s.stroke.on = true;
    s.strokeStyle.width = 2.0f;
    s.strokeStyle.dashes = {4.0f, 2.0f};
    s.strokeStyle.dashOffset = 1.0f;
    Document doc = vectorDoc(64, 64, s);

    const LayerTransformResult r = transformVectorLayer(doc, 0, m, params);
    const VectorShape& out = doc.layers[0].shapes[0];
    const Anchor& o = out.path.subpaths[0].anchors[0];
    check(r.ok, "anchors: transformVectorLayer accepts an affine skew/scale/translate");
    check(nearPt(o.pt, 19.5f, 5.75f) && nearPt(o.in, 13.0f, -1.25f) && nearPt(o.out, 2.25f, -2.25f),
          "anchors: pt, in and out each land on the hand-computed image");
    check(nearPt(out.clip->subpaths[0].anchors[0].pt, 19.5f, 5.75f) &&
              nearPt(*out.pivot, 13.0f, -1.25f),
          "anchors: the clip path and the pivot are mapped by the same matrix");
    const float k = std::sqrt(3.125f);
    check(near(out.strokeStyle.width, 2.0f * k) && near(out.strokeStyle.dashes[0], 4.0f * k) &&
              near(out.strokeStyle.dashOffset, k),
          "anchors: stroke width, dashes and dash offset scale by sqrt(|det|)");

    // A linear gradient under a shear x' = x + y: the isoline x = c becomes
    // x - y = c, so (0,0)->(10,0) must become (0,0)->(5,-5).
    Mat3 shear;
    shear.m = {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Document gdoc = vectorDoc(64, 64, rectShape(0, 0, 10, 10));
    GradientDef g;
    g.geometry.kind = GradientKind::Linear;
    g.geometry.x1 = 10.0f;
    gdoc.gradients.push_back(g);
    gdoc.layers[0].shapes[0].fill.kind = PaintKind::Gradient;
    gdoc.layers[0].shapes[0].fill.gradient = 0;
    addLayer(gdoc, 1, makeVectorLayer("shares the gradient"));
    gdoc.layers[1].shapes.push_back(gdoc.layers[0].shapes[0]);
    const LayerTransformResult gr = transformVectorLayer(gdoc, 0, shear, params);
    const uint32_t gi = gdoc.layers[0].shapes[0].fill.gradient;
    check(gr.ok && gi < gdoc.gradients.size() &&
              near(gdoc.gradients[gi].geometry.x1, 5.0f) && near(gdoc.gradients[gi].geometry.y1, -5.0f),
          "gradient: a linear gradient's end maps exactly under a shear");
    check(gi != 0 && gdoc.layers[1].shapes[0].fill.gradient == 0 &&
              gdoc.gradients[0].geometry.x1 == 10.0f && gdoc.gradients[0].geometry.y1 == 0.0f,
          "gradient: a gradient another layer references is copied, and theirs is untouched");
  }

  // --- 2. Image Size x2: composites at the new place and size ----------------
  {
    VectorShape s = rectShape(8.0f, 8.0f, 24.0f, 24.0f);
    s.stroke.on = false;
    Document doc = vectorDoc(64, 64, s);
    doc.layers[0].locked = true;
    const DocumentTransformResult r = resizeDocumentImage(doc, 128, 128, params, nullptr);
    check(r.ok && doc.width == 128 && doc.layers[0].locked,
          "image size: a document with a locked Vector layer resizes, and stays locked");
    const Anchor& a2 = doc.layers[0].shapes[0].path.subpaths[0].anchors[2];
    check(nearPt(a2.pt, 48.0f, 48.0f), "image size: the far corner (24,24) is now at (48,48)");

    const std::vector<float> img = compositeDocumentPremultiplied(doc);
    const std::vector<float> ref = compositeDocumentPremultiplied(
        vectorDoc(128, 128, rectShape(16.0f, 16.0f, 48.0f, 48.0f)));
    bool same = img.size() == ref.size() && !img.empty();
    double alpha = 0.0;
    for (size_t i = 0; same && i < img.size(); ++i) {
      if (std::fabs(img[i] - ref[i]) > 1e-5f) same = false;
      if (i % 4 == 3) alpha += img[i];
    }
    check(same, "image size: the composite equals a shape drawn at 16..48 on a 128px canvas");
    const auto at = [&](int x, int y) { return img[(static_cast<size_t>(y) * 128u + x) * 4u + 3u]; };
    check(at(20, 20) > 0.999f && at(46, 46) > 0.999f && at(12, 12) < 1e-4f && at(50, 50) < 1e-4f,
          "image size: opaque at (20,20) and (46,46), clear at (12,12) and (50,50)");
    check(std::fabs(alpha - 32.0 * 32.0) < 0.5, "image size: covered area is 32x32, four times 16x16");
  }

  // --- 3. Text: Image Size scales the type; Canvas Size keeps origin form ----
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    addLayer(doc, 1, makeTextLayer("caption"));
    doc.layers[1].text = makeTextContent("Hi", PathPoint{10.0f, 20.0f});
    Document canvasDoc = doc;
    Document cropDoc = doc;

    doc.layers[1].text.style.tracking = 1.5f;
    doc.layers[1].text.style.leading = 30.0f;
    const DocumentTransformResult r = resizeDocumentImage(doc, 128, 128, params, nullptr);
    const TextContent& t = doc.layers[1].text;
    check(r.ok && t.style.sizePx == 48.0f && t.style.tracking == 3.0f && t.style.leading == 60.0f,
          "text: image size x2 doubles font size, tracking and leading");
    check(t.transform.m == mat3Identity().m && t.origin.x == 20.0f && t.origin.y == 40.0f,
          "text: a uniform x2 keeps the identity matrix and doubles the origin");

    const DocumentTransformResult c =
        resizeDocumentCanvas(canvasDoc, 80, 80, CanvasAnchor::Center, nullptr);
    const TextContent& ct = canvasDoc.layers[1].text;
    check(c.ok && ct.transform.m == mat3Identity().m && ct.origin.x == 18.0f && ct.origin.y == 28.0f,
          "text: canvas size (centre, +16) shifts origin by 8 and keeps the identity matrix");

    cropDoc.layers[1].locked = true;
    const DocumentTransformResult k = cropDocument(cropDoc, 4, 6, 32, 32, nullptr);
    const TextContent& kt = cropDoc.layers[1].text;
    check(k.ok && kt.transform.m == mat3Identity().m && kt.origin.x == 6.0f && kt.origin.y == 14.0f &&
              cropDoc.layers[1].locked,
          "text: a crop moves a locked caption's origin, still in origin form");

    Document vcrop = vectorDoc(64, 64, rectShape(8.0f, 8.0f, 24.0f, 24.0f));
    const DocumentTransformResult vk = resizeDocumentCanvas(vcrop, 80, 80, CanvasAnchor::Center, nullptr);
    check(vk.ok && nearPt(vcrop.layers[0].shapes[0].path.subpaths[0].anchors[0].pt, 16.0f, 16.0f),
          "vector: canvas size (centre, +16) moves the shapes by 8");
  }

  // --- 3b. Text resized in type units renders as the true scaled image --------
  {
    // Reference: the same block with the whole resize carried by its matrix,
    // which maps the outlines exactly.
    const auto textOnly = [](int32_t w, int32_t h, const TextContent& text) {
      Document d = Document::createBlank(w, h, WorkingSpace{});
      d.layers.clear();
      addLayer(d, 0, makeTextLayer("caption"));
      d.layers[0].text = text;
      return d;
    };
    // Largest alpha difference, and differing alpha as a fraction of the ink.
    const auto compare = [](const Document& got, const Document& want, float* maxDiff, double* inkFrac) {
      const std::vector<float> a = compositeDocumentPremultiplied(got);
      const std::vector<float> b = compositeDocumentPremultiplied(want);
      *maxDiff = 1.0f;
      *inkFrac = 1.0;
      if (a.size() != b.size() || a.empty()) return;
      double diff = 0.0, ink = 0.0;
      float mx = 0.0f;
      for (size_t i = 3; i < a.size(); i += 4) {
        mx = std::max(mx, std::fabs(a[i] - b[i]));
        diff += std::fabs(a[i] - b[i]);
        ink += b[i];
      }
      *maxDiff = mx;
      *inkFrac = ink > 0.0 ? diff / ink : 1.0;
    };

    TextContent base = makeTextContent("Hamburgefonstiv", PathPoint{6.0f, 40.0f});
    base.style.sizePx = 13.0f;

    struct Case {
      const char* name;
      uint32_t w, h;
      Mat3 pre;  // the block's matrix before the resize
    };
    const Case cases[] = {
        {"uniform x2", 256, 128, mat3Identity()},
        {"non-uniform 2 x 1.5", 256, 96, mat3Identity()},
        {"rotated 30, x2", 256, 128, transformRotateDegreesAbout(30.0f, Point2{60.0f, 40.0f})},
    };
    for (const Case& c : cases) {
      TextContent pre = base;
      pre.transform = c.pre;
      Document doc = textOnly(128, 64, pre);
      const DocumentTransformResult r = resizeDocumentImage(doc, c.w, c.h, params, nullptr);
      const float sx = static_cast<float>(c.w) / 128.0f, sy = static_cast<float>(c.h) / 64.0f;
      TextContent ref = pre;
      ref.transform = mat3Multiply(transformScale(sx, sy), pre.transform);
      float maxDiff = 0.0f;
      double inkFrac = 0.0;
      compare(doc, textOnly(static_cast<int32_t>(c.w), static_cast<int32_t>(c.h), ref), &maxDiff,
              &inkFrac);
      std::printf("  [measure] text %s: font %.2f, max alpha diff %.4f, diff/ink %.5f\n", c.name,
                  doc.layers[0].text.style.sizePx, maxDiff, inkFrac);
      const std::string what = std::string("text render: ") + c.name +
                               " matches the matrix-scaled reference (diff/ink < 1%)";
      check(r.ok && inkFrac < 0.01, what.c_str());
    }

    TextContent pre = base;
    Document nu = textOnly(128, 64, pre);
    (void)resizeDocumentImage(nu, 256, 96, params, nullptr);
    const TextContent& nt = nu.layers[0].text;
    check(nt.style.sizePx == 19.5f && near(nt.transform.m[0], 4.0f / 3.0f) && nt.transform.m[4] == 1.0f &&
              nt.transform.m[1] == 0.0f && nt.transform.m[3] == 0.0f && near(nt.origin.x, 9.0f) &&
              near(nt.origin.y, 60.0f),
          "text: non-uniform 2 x 1.5 scales the font by 1.5 and keeps x 4/3 in the matrix");

    TextContent rot = base;
    rot.transform = transformRotateDegrees(30.0f);
    Document rd = textOnly(128, 64, rot);
    (void)resizeDocumentImage(rd, 256, 128, params, nullptr);
    const TextContent& rt = rd.layers[0].text;
    check(rt.style.sizePx == 26.0f && near(rt.transform.m[0], rot.transform.m[0]) &&
              near(rt.transform.m[1], rot.transform.m[1]) && near(rt.transform.m[3], rot.transform.m[3]) &&
              near(rt.transform.m[4], rot.transform.m[4]),
          "text: a rotated block doubles its font and keeps its rotation");
  }

  // --- 4. Move session and nudge on a Vector layer, and undo -----------------
  {
    OpenDocument od;
    od.id = 7301;
    od.document = vectorDoc(64, 64, rectShape(8.0f, 8.0f, 24.0f, 24.0f));
    od.recordEdit("vector fixture", EditKind::Structural);
    const std::vector<VectorShape> original = od.document.layers[0].shapes;

    TransformSession ts;
    const TransformBeginResult began = beginMove(ts, od);
    check(began.ok && ts.active() && ts.sourceBounds().x == 8 && ts.sourceBounds().y == 8 &&
              ts.sourceBounds().width == 17u,
          "move: beginMove accepts a Vector layer, with bounds from its shapes");
    setMoveTranslation(ts, 5.0f, -3.0f);
    const TransformCommitResult done = ts.commit(od);
    check(done.ok && nearPt(od.document.layers[0].shapes[0].path.subpaths[0].anchors[0].pt, 13.0f, 5.0f),
          "move: a committed move shifts every anchor by (5,-3)");

    if (const Document* prior = od.history.undo()) od.document = *prior;
    check(sameShapes(od.document.layers[0].shapes, original),
          "move: undo restores the shapes bit for bit");

    const TransformCommitResult nudge = nudgeMove(od, -1.0f, 0.0f);
    check(nudge.ok && od.document.layers[0].shapes[0].path.subpaths[0].anchors[0].pt.x == 7.0f,
          "move: an arrow-key nudge moves a Vector layer");

    od.document.layers[0].locked = true;
    TransformSession locked;
    const TransformBeginResult lb = beginMove(locked, od);
    check(!lb.ok && lb.error.find("locked") != std::string::npos,
          "move: a locked Vector layer is refused by name");
  }

  // --- 5. Refusals: non-affine, perspective crop, warp ----------------------
  {
    Document doc = vectorDoc(64, 64, rectShape(8.0f, 8.0f, 24.0f, 24.0f));
    const std::vector<VectorShape> before = doc.layers[0].shapes;
    Mat3 persp = mat3Identity();
    persp.m[6] = 0.001f;
    const LayerTransformResult r = transformVectorLayer(doc, 0, persp, params);
    check(!r.ok && r.error.find("affine") != std::string::npos && sameShapes(doc.layers[0].shapes, before),
          "refusal: a perspective matrix is refused by name, shapes untouched");

    const DocumentTransformResult dr = transformDocument(doc, persp, 64, 64, params, nullptr);
    check(!dr.ok && dr.error.find("Vector") != std::string::npos &&
              dr.error.find("Nothing was changed") != std::string::npos &&
              sameShapes(doc.layers[0].shapes, before),
          "refusal: a perspective document transform is refused before any layer moves");

    OpenDocument od;
    od.id = 7302;
    od.document = doc;
    od.recordEdit("vector fixture", EditKind::Structural);
    TransformSession ws;
    const TransformBeginResult wb = ws.beginLayer(od, 0);
    ws.setWarpMode(true, 2);
    const TransformCommitResult wc = ws.commit(od);
    check(wb.ok && !wc.ok && wc.error.find("Vector") != std::string::npos &&
              sameShapes(od.document.layers[0].shapes, before),
          "refusal: warp on a Vector layer is refused by name");
  }

  // --- 6. A perspective crop rasterises Vector and Text, with a warning -------
  {
    const auto build = [&](OpenDocument& od, DocumentId id) {
      od.id = id;
      od.document = Document::createBlank(64, 64, WorkingSpace{});
      addLayer(od.document, 1, makeVectorLayer("shapes"));
      od.document.layers[1].shapes.push_back(rectShape(10.0f, 10.0f, 40.0f, 30.0f));
      od.document.layers[1].opacity = 0.5f;
      (void)addLayerMask(od.document, 1);
      addLayer(od.document, 2, makeTextLayer("caption"));
      od.document.layers[2].text = makeTextContent("Hi", PathPoint{12.0f, 50.0f});
      od.document.layers[2].locked = true;
      od.recordEdit("crop fixture", EditKind::Structural);
    };
    CropQuad keystone;
    // Not a parallelogram: the top edge is longer than the bottom one.
    keystone.c = {Point2{2.0f, 2.0f}, Point2{62.0f, 6.0f}, Point2{54.0f, 60.0f}, Point2{10.0f, 58.0f}};

    OpenDocument od;
    build(od, 7310);
    const Document before = od.document;
    const DocumentTransformResult r = applyCropPerspective(od, keystone);
    check(r.ok, "perspective crop: succeeds on a document holding Vector and Text layers");
    const Layer& v = od.document.layers[1];
    const Layer& t = od.document.layers[2];
    check(r.ok && v.kind == LayerKind::RGB && t.kind == LayerKind::RGB && v.shapes.empty() &&
              v.name == "shapes" && v.opacity == 0.5f && v.mask.has_value() && t.name == "caption" &&
              t.locked,
          "perspective crop: both become RGB, keeping name, opacity, mask and lock");

    // Reference: rasterise, then the same perspective transform, by hand.
    Document ref = before;
    for (size_t i : {size_t{1}, size_t{2}}) {
      const bool wasLocked = ref.layers[i].locked;
      ref.layers[i].locked = false;
      (void)rasteriseLayer(ref, i);
      ref.layers[i].locked = wasLocked;
    }
    const DocumentRegion extent = perspectiveCropExtent(keystone);
    const std::array<Point2, 4> dst{
        Point2{0.0f, 0.0f}, Point2{static_cast<float>(extent.width), 0.0f},
        Point2{static_cast<float>(extent.width), static_cast<float>(extent.height)},
        Point2{0.0f, static_cast<float>(extent.height)}};
    Mat3 persp;
    std::string solveErr;
    const bool solved = transformFromQuad(keystone.c, dst, &persp, &solveErr);
    const DocumentTransformResult rr =
        transformDocument(ref, persp, extent.width, extent.height, params, nullptr);
    Document gotView = od.document, wantView = ref;
    gotView.layers[0].visible = wantView.layers[0].visible = false;  // the shapes and text alone
    const std::vector<float> got = compositeDocumentPremultiplied(gotView);
    const std::vector<float> want = compositeDocumentPremultiplied(wantView);
    double ink = 0.0;
    for (size_t i = 3; i < got.size(); i += 4) ink += got[i];
    check(solved && rr.ok && !mat3IsAffine(persp) && got == want && ink > 50.0,
          "perspective crop: pixels equal rasterise-then-perspective-crop, bit for bit");

    check(r.warnings.size() == 1 && r.warnings[0].find("'shapes' (Vector)") != std::string::npos &&
              r.warnings[0].find("'caption' (Text)") != std::string::npos,
          "perspective crop: the warning names each rasterised layer");

    if (const Document* prior = od.history.undo()) od.document = *prior;
    const TextContent& ut = od.document.layers[2].text;
    const TextContent& bt = before.layers[2].text;
    check(od.document.width == 64 && od.document.layers[1].kind == LayerKind::Vector &&
              od.document.layers[2].kind == LayerKind::Text &&
              sameShapes(od.document.layers[1].shapes, before.layers[1].shapes) && ut.utf8 == bt.utf8 &&
              ut.origin.x == bt.origin.x && ut.origin.y == bt.origin.y &&
              ut.style.sizePx == bt.style.sizePx && ut.transform.m == bt.transform.m,
          "perspective crop: one undo restores the editable shapes and text exactly");

    // A rectangle in Perspective mode solves to an affine matrix: nothing rasterises.
    OpenDocument flat;
    build(flat, 7311);
    CropQuad rect;
    rect.c = {Point2{4.0f, 4.0f}, Point2{60.0f, 4.0f}, Point2{60.0f, 60.0f}, Point2{4.0f, 60.0f}};
    const DocumentTransformResult fr = applyCropPerspective(flat, rect);
    check(fr.ok && fr.warnings.empty() && flat.document.layers[1].kind == LayerKind::Vector &&
              flat.document.layers[2].kind == LayerKind::Text,
          "perspective crop: an affine quad keeps Vector and Text editable, no warning");

    OpenDocument boxed;
    build(boxed, 7312);
    const DocumentTransformResult br = applyCropRegion(boxed, DocumentRegion{4, 4, 56u, 56u});
    check(br.ok && boxed.document.layers[1].kind == LayerKind::Vector &&
              boxed.document.layers[2].kind == LayerKind::Text &&
              nearPt(boxed.document.layers[1].shapes[0].path.subpaths[0].anchors[0].pt, 6.0f, 6.0f),
          "crop: a rectangle crop keeps the geometry editable and moves it");
  }

  std::printf("[selftest] vector transform: %s\n", ok ? "all pass" : "FAILED");
  return ok;
}

}  // namespace np
