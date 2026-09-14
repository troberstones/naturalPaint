#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "app/MoveTool.hpp"
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

  // --- 3. Text: Image Size composes the matrix; Canvas Size keeps origin form -
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    addLayer(doc, 1, makeTextLayer("caption"));
    doc.layers[1].text = makeTextContent("Hi", PathPoint{10.0f, 20.0f});
    Document canvasDoc = doc;
    Document cropDoc = doc;

    const DocumentTransformResult r = resizeDocumentImage(doc, 128, 128, params, nullptr);
    const TextContent& t = doc.layers[1].text;
    check(r.ok && near(t.transform.m[0], 2.0f) && near(t.transform.m[4], 2.0f) &&
              near(t.transform.m[1], 0.0f) && near(t.transform.m[2], 0.0f) &&
              t.origin.x == 10.0f && t.origin.y == 20.0f,
          "text: image size x2 composes scale(2) onto the matrix, origin unchanged");

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

  std::printf("[selftest] vector transform: %s\n", ok ? "all pass" : "FAILED");
  return ok;
}

}  // namespace np
