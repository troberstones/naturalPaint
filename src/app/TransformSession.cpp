#include "app/TransformSession.hpp"

#include <algorithm>
#include <cmath>

#include "core/LayerGeometry.hpp"
#include "core/LayerOps.hpp"

namespace np {
namespace {

// `core/TextContent`'s `PathBounds` (float, min/max, `valid`) as the
// `LayerBounds` (integer texels, `empty`) the rest of this file speaks. The
// block is shaped to get them, which is why this is not a member of anything
// hot: it runs once, at pen-down.
LayerBounds boundsFromTextContent(const TextContent& text) {
  LayerBounds b;
  PathBounds pb = textContentBounds(text);
  if (!pb.valid) {
    // No ink -- but that is not the same as nothing to transform. A paragraph
    // frame is dragged out BEFORE a word of it is typed, and from that moment
    // it is a real object on screen: an outline and eight resize handles, at a
    // size and place the user chose. Refusing Cmd+T on it said "nothing to
    // transform" about a box they were looking at.
    //
    // So the fallback is the block's own drawn box -- `textFrameQuad()`, the
    // same function the overlay outlines and the handles are built from, so
    // the gizmo cannot appear anywhere but around the frame the user sees.
    // Its corners come back already mapped through `TextContent::transform`,
    // which is why the extent is taken over all four rather than from two:
    // an empty frame that has ALREADY been rotated has no axis-aligned pair.
    //
    // Still refused, and rightly, for an empty POINT block: `textFrameQuad()`
    // returns false there because point text has no frame at all and no ink
    // to stand in for one, so there is no box on screen either -- only a
    // caret. Handles around a zero-width nothing would be a gizmo the user
    // could not aim, and the region maths behind it is degenerate.
    TextQuad q;
    if (!textFrameQuad(text, &q)) return b;
    pb.valid = true;
    pb.minX = pb.maxX = q.corner[0].x;
    pb.minY = pb.maxY = q.corner[0].y;
    for (const PathPoint& c : q.corner) {
      pb.minX = std::min(pb.minX, c.x);
      pb.minY = std::min(pb.minY, c.y);
      pb.maxX = std::max(pb.maxX, c.x);
      pb.maxY = std::max(pb.maxY, c.y);
    }
  }
  b.empty = false;
  b.minX = static_cast<int32_t>(std::floor(pb.minX));
  b.minY = static_cast<int32_t>(std::floor(pb.minY));
  b.maxX = static_cast<int32_t>(std::ceil(pb.maxX));
  b.maxY = static_cast<int32_t>(std::ceil(pb.maxY));
  return b;
}

// A Vector layer's box comes from its shapes, strokes included.
LayerBounds boundsFromVectorShapes(const std::vector<VectorShape>& shapes) {
  LayerBounds b;
  const PathBounds pb = vectorShapesBounds(shapes);
  if (!pb.valid) return b;
  b.empty = false;
  b.minX = static_cast<int32_t>(std::floor(pb.minX));
  b.minY = static_cast<int32_t>(std::floor(pb.minY));
  b.maxX = static_cast<int32_t>(std::ceil(pb.maxX));
  b.maxY = static_cast<int32_t>(std::ceil(pb.maxY));
  return b;
}

// Text and Vector hold geometry rather than pixels, so they have the only kinds
// admitted without tiles.
bool isGeometryOnlyKind(LayerKind kind) noexcept {
  return kind == LayerKind::Text || kind == LayerKind::Vector;
}

LayerBounds sessionBoundsFor(const Layer& layer) {
  if (layer.kind == LayerKind::Text) return boundsFromTextContent(layer.text);
  if (layer.kind == LayerKind::Vector) return boundsFromVectorShapes(layer.shapes);
  return layerContentBounds(layer);
}

LayerTransformResult transformForSession(Document& doc, size_t index, const Mat3& m,
                                         const DocumentTransformParams& params) {
  const LayerKind kind = index < doc.layers.size() ? doc.layers[index].kind : LayerKind::RGB;
  if (kind == LayerKind::Text) return transformTextLayer(doc, index, m);
  if (kind == LayerKind::Vector) return transformVectorLayer(doc, index, m, params);
  return transformLayer(doc, index, m, params);
}

}  // namespace

namespace {

// A point at fraction (u, v) across `b` -- (0,0) is the top-left corner, (1,1)
// the bottom-right, in the half-open-region convention DocumentRegion already
// uses (ops/DocumentTransform.hpp).
Point2 fractionalCorner(const DocumentRegion& b, float u, float v) noexcept {
  return Point2{static_cast<float>(b.x) + u * static_cast<float>(b.width),
               static_cast<float>(b.y) + v * static_cast<float>(b.height)};
}

Point2 boxCenter(const DocumentRegion& b) noexcept { return fractionalCorner(b, 0.5f, 0.5f); }

constexpr float kPi = 3.14159265358979323846f;

// Composites `top` (already positioned at `region` in ABSOLUTE document
// coordinates -- the shape `transformRgbTiles()`'s output store is in)
// premultiplied `over` `dst`'s own content at that same region, and writes
// the result back into `dst`.
//
// This is NOT a second resampler: every texel `top` holds already passed
// through `transformImage()` exactly once (ops/Transform.hpp section 1's
// PRD D16 guarantee is about resampling, i.e. reconstruction-filter passes,
// not about ordinary alpha compositing of two already-final images). It is
// the same premultiplied-`over` algebra `core/Premultiply.hpp` and every
// blend in this codebase already use, applied here because there is no
// existing "splice one TileStore over another at an offset" entry point in
// core/ or ops/ -- only whole-document compositing
// (core/Composite.hpp) and this file's own narrower need. `imageFromTileStore`
// / `tileStoreFromImage` are ops/Transform.hpp's own "tile-store bridge",
// used exactly as documented there.
void compositeStoreOverRegion(const TileStore& top, const DocumentRegion& region,
                              TileStore* dst) {
  TransformImage below = imageFromTileStore(*dst, region.x, region.y, region.width, region.height);
  const TransformImage above = imageFromTileStore(top, region.x, region.y, region.width, region.height);
  TransformImage out;
  out.width = region.width;
  out.height = region.height;
  out.px.resize(below.px.size());
  for (size_t i = 0; i + 3 < out.px.size(); i += 4) {
    const float srcA = above.px[i + 3];
    for (int c = 0; c < 4; ++c) out.px[i + c] = above.px[i + c] + below.px[i + c] * (1.0f - srcA);
  }
  tileStoreFromImage(out, region.x, region.y, dst);
}

std::string layerLabel(const Document& doc, size_t index) {
  return "layer " + std::to_string(index) +
        (index < doc.layers.size() ? " ('" + doc.layers[index].name + "')" : "");
}

// One member's admission to a `TransformTarget::LayerSet` session -- header
// section 8: exactly `beginLayer()`'s own predicate (locked; Text is
// geometry-only and always admitted; every other kind needs RGB or Pigment
// tiles; the bounds must be non-empty), duplicated here rather than factored
// out of `beginLayer()` so that function's code is untouched by this feature
// and its bit-identical guarantee costs nothing to believe. `bounds` is
// undefined when `ok` is false.
struct MemberAdmission {
  bool ok = false;
  std::string error;
  LayerBounds bounds;
};

MemberAdmission admitLayerSetMember(const Document& doc, size_t index) {
  MemberAdmission a;
  if (index >= doc.layers.size()) {
    a.error = "transform refused: index " + std::to_string(index) +
             " is out of range; this document has " + std::to_string(doc.layers.size()) +
             " layer(s).";
    return a;
  }
  const Layer& layer = doc.layers[index];
  if (layer.locked) {
    a.error = "transform refused: " + layerLabel(doc, index) + " is locked. Unlock it first.";
    return a;
  }
  if (!isGeometryOnlyKind(layer.kind) && !layer.rgbTiles.has_value() &&
      !layer.pigmentTiles.has_value()) {
    // Covers Group and Adjustment with no special case for either -- section
    // 8's stated decision: a Group is refused exactly as an Adjustment layer
    // is, because neither holds a pixel for a matrix to resample.
    a.error = "transform refused: " + layerLabel(doc, index) + " is a " +
             layerKindName(layer.kind) + " layer, which holds no pixels to transform.";
    return a;
  }
  a.bounds = sessionBoundsFor(layer);
  if (a.bounds.empty) {
    a.error = "transform refused: " + layerLabel(doc, index) + " has no content -- nothing to "
             "transform.";
    return a;
  }
  a.ok = true;
  return a;
}

}  // namespace

// --------------------------------------------------------------------------
// Handle geometry
// --------------------------------------------------------------------------

TransformHandlePositions transformHandlePositions(const DocumentRegion& sourceBounds,
                                                   const Mat3& pending, float rotateReach) noexcept {
  TransformHandlePositions h;
  const Point2 tc = fractionalCorner(sourceBounds, 0.5f, 0.0f);
  // Above top-center, in LOCAL space -- mapped through `pending` below like
  // every other handle, so it turns and scales with the box instead of
  // staying screen-axis-aligned.
  const Point2 aboveLocal{tc.x, tc.y - rotateReach};

  h.topLeft = mat3MapPoint(pending, fractionalCorner(sourceBounds, 0.0f, 0.0f));
  h.topCenter = mat3MapPoint(pending, tc);
  h.topRight = mat3MapPoint(pending, fractionalCorner(sourceBounds, 1.0f, 0.0f));
  h.middleLeft = mat3MapPoint(pending, fractionalCorner(sourceBounds, 0.0f, 0.5f));
  h.middleRight = mat3MapPoint(pending, fractionalCorner(sourceBounds, 1.0f, 0.5f));
  h.bottomLeft = mat3MapPoint(pending, fractionalCorner(sourceBounds, 0.0f, 1.0f));
  h.bottomCenter = mat3MapPoint(pending, fractionalCorner(sourceBounds, 0.5f, 1.0f));
  h.bottomRight = mat3MapPoint(pending, fractionalCorner(sourceBounds, 1.0f, 1.0f));
  h.center = mat3MapPoint(pending, boxCenter(sourceBounds));
  h.rotate = mat3MapPoint(pending, aboveLocal);
  return h;
}

TransformHandle hitTestTransformHandle(const TransformHandlePositions& h,
                                       const DocumentRegion& sourceBounds, const Mat3& pending,
                                       Point2 cursor, float handleRadius) noexcept {
  const float r2 = handleRadius * handleRadius;
  auto near = [&](Point2 p) noexcept {
    const float dx = p.x - cursor.x, dy = p.y - cursor.y;
    return dx * dx + dy * dy <= r2;
  };
  // Rotate first: it usually sits outside the box, where nothing else
  // competes for it. Corners before edges, so an overlapping radius near a
  // corner resolves to the corner.
  if (near(h.rotate)) return TransformHandle::Rotate;
  if (near(h.topLeft)) return TransformHandle::TopLeft;
  if (near(h.topRight)) return TransformHandle::TopRight;
  if (near(h.bottomLeft)) return TransformHandle::BottomLeft;
  if (near(h.bottomRight)) return TransformHandle::BottomRight;
  if (near(h.topCenter)) return TransformHandle::TopCenter;
  if (near(h.bottomCenter)) return TransformHandle::BottomCenter;
  if (near(h.middleLeft)) return TransformHandle::MiddleLeft;
  if (near(h.middleRight)) return TransformHandle::MiddleRight;

  // Inside the (possibly rotated) box body: map the cursor back into
  // source-local space through `pending`'s inverse and test the
  // axis-aligned `sourceBounds` there -- exact for any invertible `pending`,
  // including a rotation, without a rotated-polygon test in destination
  // space.
  Mat3 inv;
  if (mat3Invert(pending, &inv)) {
    const Point2 local = mat3MapPoint(inv, cursor);
    if (local.x >= static_cast<float>(sourceBounds.x) &&
        local.x <= static_cast<float>(sourceBounds.x) + static_cast<float>(sourceBounds.width) &&
        local.y >= static_cast<float>(sourceBounds.y) &&
        local.y <= static_cast<float>(sourceBounds.y) + static_cast<float>(sourceBounds.height))
      return TransformHandle::Move;
  }
  return TransformHandle::None;
}

// --------------------------------------------------------------------------
// Drag semantics
// --------------------------------------------------------------------------

Mat3 computeTransformDragMatrix(TransformHandle handle, const DocumentRegion& sourceBounds,
                                const Mat3& baseMatrix, Point2 startCursor, Point2 curCursor,
                                bool shiftHeld, bool optionHeld) noexcept {
  if (handle == TransformHandle::None) return baseMatrix;

  if (handle == TransformHandle::Move) {
    // Destination-space translate, applied AFTER the base matrix (left-
    // multiplied): sliding an already-rotated box along the canvas axes, not
    // its own.
    const Mat3 delta = transformTranslate(curCursor.x - startCursor.x, curCursor.y - startCursor.y);
    return mat3Multiply(delta, baseMatrix);
  }

  if (handle == TransformHandle::Rotate) {
    const Point2 centerDest = mat3MapPoint(baseMatrix, boxCenter(sourceBounds));
    const float startAngle = std::atan2(startCursor.y - centerDest.y, startCursor.x - centerDest.x);
    const float curAngle = std::atan2(curCursor.y - centerDest.y, curCursor.x - centerDest.x);
    const float deltaDeg = (curAngle - startAngle) * (180.0f / kPi);
    return mat3Multiply(transformRotateDegreesAbout(deltaDeg, centerDest), baseMatrix);
  }

  // The eight scale handles: work in SOURCE-LOCAL space (through the base
  // matrix's inverse) so the ratio is measured in the box's own, possibly
  // already-rotated frame, then right-multiply the scale onto the base
  // matrix (applied BEFORE it, i.e. in source space -- resizing about one of
  // the box's own corners/edges, not about the canvas origin).
  Mat3 inv;
  if (!mat3Invert(baseMatrix, &inv)) return baseMatrix;  // degenerate; refuse to make it worse
  const Point2 localStart = mat3MapPoint(inv, startCursor);
  const Point2 localCur = mat3MapPoint(inv, curCursor);

  const float x0 = static_cast<float>(sourceBounds.x);
  const float y0 = static_cast<float>(sourceBounds.y);
  const float x1 = x0 + static_cast<float>(sourceBounds.width);
  const float y1 = y0 + static_cast<float>(sourceBounds.height);
  const Point2 center = boxCenter(sourceBounds);

  bool activeX = false, activeY = false;
  float anchorX = x0, anchorY = y0;
  switch (handle) {
    case TransformHandle::TopLeft: activeX = activeY = true; anchorX = x1; anchorY = y1; break;
    case TransformHandle::TopRight: activeX = activeY = true; anchorX = x0; anchorY = y1; break;
    case TransformHandle::BottomLeft: activeX = activeY = true; anchorX = x1; anchorY = y0; break;
    case TransformHandle::BottomRight: activeX = activeY = true; anchorX = x0; anchorY = y0; break;
    case TransformHandle::TopCenter: activeY = true; anchorX = center.x; anchorY = y1; break;
    case TransformHandle::BottomCenter: activeY = true; anchorX = center.x; anchorY = y0; break;
    case TransformHandle::MiddleLeft: activeX = true; anchorX = x1; anchorY = center.y; break;
    case TransformHandle::MiddleRight: activeX = true; anchorX = x0; anchorY = center.y; break;
    default: return baseMatrix;  // Move/Rotate/None handled above
  }
  if (optionHeld) { anchorX = center.x; anchorY = center.y; }

  // Degenerate-input guard, not a correctness tolerance: this only fires
  // when the drag's own start cursor mapped to (within 1/1000 of a source
  // pixel of) the anchor's own axis -- a start point on the anchor line
  // itself, which a legitimately hit-tested handle several pixels from that
  // line cannot produce in practice. It exists so a pathological start point
  // divides by a small number rather than a literal zero; it does not bound
  // any resampling error (ops/Transform.hpp's own kernel-error measurements
  // are the tolerances that do that, and they are elsewhere).
  constexpr float kMinLocalSpan = 1e-3f;

  float sx = 1.0f, sy = 1.0f;
  if (activeX) {
    const float span = localStart.x - anchorX;
    if (std::fabs(span) > kMinLocalSpan) sx = (localCur.x - anchorX) / span;
  }
  if (activeY) {
    const float span = localStart.y - anchorY;
    if (std::fabs(span) > kMinLocalSpan) sy = (localCur.y - anchorY) / span;
  }

  if (shiftHeld) {
    // Aspect lock: both axes take whichever factor is furthest from 1 -- the
    // scale-ratio analogue of app/SelectionDrag.cpp's own "larger of the two
    // deltas, keeping each axis's sign" idiom. On an edge handle only one
    // axis was active above (the other's factor is still exactly 1, i.e.
    // |factor-1| == 0), so this is also what gives Shift a second, tied axis
    // on an edge handle rather than only ever moving one.
    const float common = (std::fabs(sx - 1.0f) >= std::fabs(sy - 1.0f)) ? sx : sy;
    sx = sy = common;
  }

  return mat3Multiply(baseMatrix, transformScaleAbout(sx, sy, Point2{anchorX, anchorY}));
}

Mat3 computeDropFitTransform(const DocumentRegion& sourceBounds,
                             const DocumentRegion& canvas) noexcept {
  // Either side missing content, or a canvas with no area at all (shouldn't
  // happen for an open document, but `beginLayer()` itself already refused
  // an empty `sourceBounds` before a caller could reach this): nothing to
  // fit, and identity is the only sane answer.
  if (sourceBounds.empty() || canvas.empty()) return mat3Identity();

  const float srcW = static_cast<float>(sourceBounds.width);
  const float srcH = static_cast<float>(sourceBounds.height);
  const float dstW = static_cast<float>(canvas.width);
  const float dstH = static_cast<float>(canvas.height);

  // Already fits both dimensions: identity, unchanged -- no forced upscale,
  // and no seeded transform at all for content that fit before this feature
  // existed (this header's own comment on `computeDropFitTransform()`).
  if (srcW <= dstW && srcH <= dstH) return mat3Identity();

  // Whichever axis overflows more governs both, so the image shrinks enough
  // to clear the tighter dimension without ever needing to crop the other.
  const float scale = std::min(dstW / srcW, dstH / srcH);

  // Centre the scaled box on the canvas: the source's own origin need not be
  // (0, 0) in principle, so this is computed as "translate the scaled
  // top-left to the centred target", not assumed away.
  const float srcX = static_cast<float>(sourceBounds.x);
  const float srcY = static_cast<float>(sourceBounds.y);
  const float targetX = (dstW - srcW * scale) * 0.5f;
  const float targetY = (dstH - srcH * scale) * 0.5f;

  return mat3Multiply(transformTranslate(targetX - srcX * scale, targetY - srcY * scale),
                      transformScale(scale, scale));
}

Mat3 composeNumericTransform(float rotateDegrees, float scaleXFraction, float scaleYFraction,
                             float translateX, float translateY, Point2 pivot) noexcept {
  const Mat3 scaled = transformScaleAbout(scaleXFraction, scaleYFraction, pivot);
  const Mat3 rotatedAndScaled = mat3Multiply(transformRotateDegreesAbout(rotateDegrees, pivot), scaled);
  return mat3Multiply(transformTranslate(translateX, translateY), rotatedAndScaled);
}

// --------------------------------------------------------------------------
// TransformSession
// --------------------------------------------------------------------------

TransformHandlePositions TransformSession::handlePositions(float rotateReach) const noexcept {
  return transformHandlePositions(sourceBounds_, pending_, rotateReach);
}

TransformHandle TransformSession::hitTest(Point2 cursor, float handleRadius,
                                          float rotateReach) const noexcept {
  const TransformHandlePositions h = handlePositions(rotateReach);
  return hitTestTransformHandle(h, sourceBounds_, pending_, cursor, handleRadius);
}

void TransformSession::beginDrag(TransformHandle handle, Point2 startCursor) noexcept {
  drag_.active = true;
  drag_.handle = handle;
  drag_.start = startCursor;
  drag_.base = pending_;
}

void TransformSession::updateDrag(Point2 curCursor, bool shiftHeld, bool optionHeld) noexcept {
  if (!drag_.active) return;
  pending_ = computeTransformDragMatrix(drag_.handle, sourceBounds_, drag_.base, drag_.start,
                                        curCursor, shiftHeld, optionHeld);
}

void TransformSession::endDrag() noexcept { drag_.active = false; }

// --------------------------------------------------------------------------
// Warp (header section 9)
// --------------------------------------------------------------------------

void TransformSession::setWarpMode(bool warpOn, int gridN) noexcept {
  if (!active_) return;
  if (warpOn) {
    if (mode_ == TransformMode::Warp) {
      // Already warping: a grid-size change re-fits the existing shape
      // rather than re-baking `pending_`, which would throw the bend away.
      if (gridN != warp_.n()) warp_ = warp_.refit(gridN);
      return;
    }
    // Bake the accumulated affine into the warp's starting net -- header
    // section 9. `WarpMesh::flat()` reproduces `sourceBounds_` exactly;
    // mapping every control point through `pending_` carries over whatever
    // the user had already dragged.
    WarpMesh flat = WarpMesh::flat(sourceBounds_, gridN);
    const int side = flat.pointsPerSide();
    for (int row = 0; row < side; ++row) {
      for (int col = 0; col < side; ++col) {
        flat.setAt(row, col, mat3MapPoint(pending_, flat.at(row, col)));
      }
    }
    warp_ = std::move(flat);
    mode_ = TransformMode::Warp;
  } else {
    // Section 9: the net is discarded, not collapsed into an approximating
    // matrix -- there is generally no single affine map that reproduces a
    // bent surface, and guessing one would silently lose the bend with no
    // way for the user to tell.
    mode_ = TransformMode::Affine;
    warp_ = WarpMesh{};
    warpDrag_ = WarpDragState{};
  }
}

WarpControlRef TransformSession::warpHitTest(Point2 cursor, float radius) const noexcept {
  if (!active_ || mode_ != TransformMode::Warp) return WarpControlRef{};
  return hitTestWarpControl(warp_, cursor, radius);
}

void TransformSession::warpBeginDrag(WarpControlRef ref, Point2 startCursor) noexcept {
  if (!active_ || mode_ != TransformMode::Warp || !ref.valid) return;
  warpDrag_.active = true;
  warpDrag_.ref = ref;
  warpDrag_.start = startCursor;
  warpDrag_.base = warp_;
}

void TransformSession::warpUpdateDrag(Point2 curCursor) noexcept {
  if (!warpDrag_.active) return;
  // Recomputed fresh from the drag's own baseline every frame -- section 6's
  // discipline, applied here too, so a multi-frame drag never accumulates a
  // delta onto a delta.
  const Point2 delta{curCursor.x - warpDrag_.start.x, curCursor.y - warpDrag_.start.y};
  warp_ = warpDrag_.base;
  warp_.dragControl(warpDrag_.ref, delta);
}

void TransformSession::warpEndDrag() noexcept { warpDrag_.active = false; }

// Mirrors commit()'s own Warp branch below, read-only: same refusals, same
// warpRgbTiles()/cutThroughSelection()/compositeStoreOverRegion() sequence,
// but writing into `*out` (a copy) instead of `od.document`.
bool TransformSession::previewWarpDocument(const OpenDocument& od, ResampleKernel kernel,
                                           Document* out) const {
  if (out == nullptr || !active_ || mode_ != TransformMode::Warp) return false;
  if (target_ == TransformTarget::LayerSet || duplicate_) return false;
  if (layerIndex_ >= od.document.layers.size() ||
      od.document.layers[layerIndex_].id != layerId_)
    return false;
  const Layer& layer = od.document.layers[layerIndex_];
  if (layer.locked || layer.kind == LayerKind::Pigment || !layer.rgbTiles.has_value())
    return false;
  if (target_ == TransformTarget::Layer && layer.mask.has_value()) return false;

  const DocumentRegion dstRegion = warpedRegion(warp_, warpChordSubdivisions(warp_));
  if (dstRegion.empty()) return false;

  std::string err;
  if (target_ == TransformTarget::Layer) {
    TileStore newRgb;
    if (!warpRgbTiles(*layer.rgbTiles, warp_, dstRegion, kernel, &newRgb, &err)) return false;
    *out = od.document;
    *out->layers[layerIndex_].rgbTiles = std::move(newRgb);
    return true;
  }

  // SelectionPixels: cut a copy of the layer, never the real one.
  Layer cutCopy = layer;
  Clipboard clip = cutThroughSelection(cutCopy, &selectionSnapshot_);
  if (clip.empty() || !clip.rgbTiles.has_value()) return false;
  TileStore moved;
  if (!warpRgbTiles(*clip.rgbTiles, warp_, dstRegion, kernel, &moved, &err)) return false;
  compositeStoreOverRegion(moved, dstRegion, &*cutCopy.rgbTiles);
  *out = od.document;
  out->layers[layerIndex_].rgbTiles = std::move(cutCopy.rgbTiles);
  return true;
}

void TransformSession::cancel() noexcept { *this = TransformSession{}; }

TransformBeginResult TransformSession::beginLayer(OpenDocument& od, size_t layerIndex,
                                                  const Mat3& initialPending, bool duplicate) {
  const Document& doc = od.document;
  TransformBeginResult r;
  if (layerIndex >= doc.layers.size()) {
    r.error = "transform refused: index " + std::to_string(layerIndex) +
             " is out of range; this document has " + std::to_string(doc.layers.size()) +
             " layer(s).";
    return r;
  }
  const Layer& layer = doc.layers[layerIndex];
  if (layer.locked) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) +
             " is locked. Unlock it first.";
    return r;
  }
  // Text and Vector layers hold geometry, not pixels: `transformTextLayer()`
  // composes the block's matrix and `transformVectorLayer()` maps every anchor.
  // Without this exemption the Move tool, Cmd+T and `nudgeMove()` all refused them.
  if (!isGeometryOnlyKind(layer.kind) && !layer.rgbTiles.has_value() &&
      !layer.pigmentTiles.has_value()) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) + " is a " +
             layerKindName(layer.kind) + " layer, which holds no pixels to transform.";
    return r;
  }
  // `layerContentBounds()` scans tile stores and finds nothing on these kinds,
  // so their bounds come from the geometry. Not widened inside
  // `layerContentBounds()` itself: thumbnails, fitting and several layer ops
  // read it, and that is a much larger blast radius.
  const LayerBounds bounds = sessionBoundsFor(layer);
  if (bounds.empty) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) +
             " has no content -- nothing to transform.";
    return r;
  }

  // **Stamped before the reset below wipes the session**, and read back out of
  // the document rather than remembered from above: `ensureLayerId()` is what
  // assigns one when the layer has none, and the number it returns is the one
  // `commit()` will compare against. See the header's `layerId()` for the
  // defect this closes.
  const uint64_t layerId = ensureLayerId(od.document, layerIndex);

  *this = TransformSession{};
  sourceBounds_ = regionFromBounds(bounds);
  // Set together with `layerIndex_`, and never apart from it: the three are
  // what identify the pixels this session owns. See the header's beginLayer().
  documentId_ = od.id;
  layerIndex_ = layerIndex;
  layerId_ = layerId;
  target_ = TransformTarget::Layer;
  pending_ = initialPending;
  duplicate_ = duplicate;
  active_ = true;
  r.ok = true;
  return r;
}

TransformBeginResult TransformSession::beginSelectionPixels(OpenDocument& od,
                                                             const Selection& selection,
                                                             size_t layerIndex, bool duplicate) {
  const Document& doc = od.document;
  TransformBeginResult r;
  if (layerIndex >= doc.layers.size()) {
    r.error = "transform refused: index " + std::to_string(layerIndex) +
             " is out of range; this document has " + std::to_string(doc.layers.size()) +
             " layer(s).";
    return r;
  }
  const Layer& layer = doc.layers[layerIndex];
  if (layer.locked) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) +
             " is locked. Unlock it first.";
    return r;
  }
  // See this header's section 4: a whole-layer transform of a Pigment layer
  // is supported (ops/DocumentTransform.hpp already decided that); a
  // selection-bounded one is refused because splicing the moved paint back
  // needs this build's Kubelka-Munk mixing rule, not the straight-alpha
  // `over` this file uses for RGB.
  if (layer.kind == LayerKind::Pigment) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) +
             " is a Pigment layer. A selection-pixels transform would have to re-mix the moved "
             "paint with whatever is left on the layer where it used to be, using this build's "
             "Kubelka-Munk pigment mixing rule (core/Pigment.hpp) rather than a straight alpha "
             "blend -- that mixer has no entry point outside the brush-deposit path, so this is "
             "refused rather than spliced back wrong. Transform the whole layer instead.";
    return r;
  }
  if (!layer.rgbTiles.has_value()) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) + " is a " +
             layerKindName(layer.kind) + " layer, which holds no RGB pixels to transform.";
    return r;
  }
  if (selectionSelectsNothing(selection)) {
    r.error = "transform refused: the selection covers no pixels. Nothing to transform.";
    return r;
  }
  const DocumentRegion region = selectionContentRegion(selection);
  if (region.empty()) {
    r.error = "transform refused: the selection covers no pixels. Nothing to transform.";
    return r;
  }

  const uint64_t layerId = ensureLayerId(od.document, layerIndex);

  *this = TransformSession{};
  sourceBounds_ = region;
  selectionSnapshot_ = selection;
  documentId_ = od.id;
  layerIndex_ = layerIndex;
  // A selection-pixels transform is bounded by the selection but still lands
  // on ONE layer, at `layerIndex_`, so it goes stale in exactly the same way
  // and gets the identical stamp.
  layerId_ = layerId;
  target_ = TransformTarget::SelectionPixels;
  duplicate_ = duplicate;
  active_ = true;
  r.ok = true;
  return r;
}

TransformBeginResult TransformSession::beginLayerSet(OpenDocument& od, const LayerSelection& sel,
                                                     const Mat3& initialPending) {
  TransformBeginResult r;
  // Section 8: a one-layer selection is `beginLayer()`'s own path, and this
  // is not a second door into it.
  if (sel.size() < 2) {
    r.error = "transform refused: a set transform needs at least two selected layers (" +
             std::to_string(sel.size()) + " selected). Use Free Transform on the single layer "
             "directly.";
    return r;
  }
  const Document& doc = od.document;
  LayerBounds unioned;  // empty; unionLayerBounds() treats empty as identity.
  for (const size_t index : sel.indices) {
    const MemberAdmission a = admitLayerSetMember(doc, index);
    if (!a.ok) {
      r.error = a.error;
      return r;
    }
    unioned = unionLayerBounds(unioned, a.bounds);
  }

  // Every member admitted. Stamp ids for all of them before the reset below
  // wipes the session -- the identical ordering `beginLayer()` uses and for
  // the identical reason: `ensureLayerId()` mutates `od.document`, and doing
  // that after `*this = TransformSession{}` would be fine too since they are
  // independent objects, but matching the single-layer function's own order
  // keeps the two readable side by side.
  std::vector<uint64_t> ids;
  ids.reserve(sel.indices.size());
  for (const size_t index : sel.indices) ids.push_back(ensureLayerId(od.document, index));

  *this = TransformSession{};
  sourceBounds_ = regionFromBounds(unioned);
  documentId_ = od.id;
  layerIndices_ = sel.indices;  // already sorted, duplicate-free (LayerSelection's invariant)
  layerIds_ = std::move(ids);
  target_ = TransformTarget::LayerSet;
  pending_ = initialPending;
  active_ = true;
  r.ok = true;
  return r;
}

TransformCommitResult TransformSession::commit(OpenDocument& od,
                                               const DocumentTransformParams& params) {
  TransformCommitResult out;
  if (!active_) {
    out.error = "transform commit refused: no transform is active.";
    return out;
  }

  // The document this session began on, and no other. `layerIndex_` is an
  // index into THAT document's layer list and means nothing in another one --
  // applying it anyway resampled a layer the user was not transforming, in a
  // document they had merely switched to. Refused rather than clamped or
  // silently dropped: `od` is untouched and `active_` stays true, so switching
  // back and pressing Return again still does what the user meant.
  if (od.id != documentId_) {
    out.error = "transform commit refused: this transform belongs to a different document. "
                "Switch back to it to commit or cancel.";
    return out;
  }

  // **The same guard, one level down: the LAYER this session began on.**
  // `documentId_` above stops a commit landing in the wrong document;
  // `layerIndex_` is an index into THIS document's layer list and does not
  // survive that list moving. `Layer > Delete Layer` is reachable from the menu
  // bar while a gizmo is up (docs/testing-issues.md T29), and deleting a layer
  // BELOW the transformed one shifts every index above it down by one --
  // measured on a four-layer document, a session begun on the layer named `L0`
  // at index 1 committed onto `L1` and reported success.
  //
  // Refused rather than clamped or re-found by searching for the id: the
  // matrix was dragged against a layer at a position that no longer holds it,
  // and silently applying it somewhere else is the defect, not the fix.
  // `active_` stays true, matching the document guard just above -- undo the
  // reorder and the id lines up again, and Return does what the user meant.
  //
  // Out-of-range is folded in here rather than left to `transformLayer()`'s
  // own bounds check, so that the two halves of one question ("is the layer
  // still there, and is it still the same layer") answer in one sentence
  // instead of two written in different files.
  //
  // `TransformTarget::LayerSet` re-checks EVERY member against its own
  // stamped id (header section 8) rather than just one -- the identical
  // hazard `layerIndex_`/`layerId_` close for a single layer, closed once
  // per member here, because `Layer > Delete Layer` reachable mid-gizmo
  // (docs/testing-issues.md T29) can shift any one of them.
  if (target_ == TransformTarget::LayerSet) {
    for (size_t i = 0; i < layerIndices_.size(); ++i) {
      const size_t index = layerIndices_[i];
      if (index >= od.document.layers.size() || od.document.layers[index].id != layerIds_[i]) {
        out.error = "transform commit refused: " + layerLabel(od.document, index) +
                    " -- one of the transformed set -- is no longer at that position in the "
                    "stack -- it was deleted, reordered or merged, or the document was undone "
                    "past it. Press Escape to discard the transform.";
        return out;
      }
    }
  } else if (layerIndex_ >= od.document.layers.size() ||
             od.document.layers[layerIndex_].id != layerId_) {
    out.error = "transform commit refused: the layer this transform began on is no longer at "
                "that position in the stack -- it was deleted, reordered or merged, or the "
                "document was undone past it. Press Escape to discard the transform.";
    return out;
  }

  // Header section 9: Warp branches off here, before the affine identity
  // check below (a warp net has no single `pending_` to compare against
  // identity -- `WarpMesh::isIdentity()` is its own, separate no-op test,
  // applied inside `warpRgbTiles()`'s fast path rather than here).
  if (mode_ == TransformMode::Warp) {
    if (target_ == TransformTarget::LayerSet) {
      out.error = "warp commit refused: a set of layers has no per-member live-preview story "
                  "(app/TransformSession.hpp section 9, and section 8's identical gap for the "
                  "affine case) -- warp one layer or one selection at a time.";
      return out;
    }
    if (duplicate_) {
      out.error = "warp commit refused: Option-drag duplicate is not built for Warp mode -- "
                  "commit the duplicate as an ordinary Free Transform, or duplicate the layer or "
                  "selection first and warp the copy.";
      return out;
    }
    if (layerIndex_ >= od.document.layers.size()) {
      out.error = "warp commit refused: the target layer no longer exists.";
      return out;
    }
    Layer& layer = od.document.layers[layerIndex_];
    if (layer.locked) {
      out.error = "warp commit refused: " + layerLabel(od.document, layerIndex_) +
                  " is locked. Unlock it first.";
      return out;
    }
    if (layer.kind == LayerKind::Vector) {
      out.error = "warp commit refused: " + layerLabel(od.document, layerIndex_) +
                  " is a Vector layer, and its shapes can only follow an affine transform. Use "
                  "Free Transform, or rasterize the layer first to warp it.";
      return out;
    }
    if (layer.kind == LayerKind::Pigment || !layer.rgbTiles.has_value()) {
      out.error = "warp commit refused: " + layerLabel(od.document, layerIndex_) +
                  " -- warping a Pigment layer needs the identical mass-weighted, lobe-free-"
                  "kernel bridge ops/DocumentTransform.hpp already built for the AFFINE path, run "
                  "through a non-linear map instead of a Mat3 (app/WarpMesh.hpp); that "
                  "is real, separate machinery this track's time did not extend to. A layer with "
                  "no RGB pixels (Text, Group, Adjustment, Strokes, Flats, Media) has nothing for "
                  "a warp to resample either.";
      return out;
    }
    if (target_ == TransformTarget::Layer && layer.mask.has_value()) {
      out.error = "warp commit refused: " + layerLabel(od.document, layerIndex_) +
                  " carries a mask. Warping it needs the identical hide-space bridge "
                  "ops/DocumentTransform.hpp's transformMaskTiles() already built for the affine "
                  "path, run through this session's non-linear map instead of a Mat3 -- unbuilt "
                  "for the same reason as the Pigment refusal above. Remove the mask, or use "
                  "Free Transform instead.";
      return out;
    }
    const int subdivisions = warpChordSubdivisions(warp_);
    const DocumentRegion dstRegion = warpedRegion(warp_, subdivisions);
    if (dstRegion.empty()) {
      out.error = "warp commit refused: this net collapses the target to zero pixels. Nothing "
                  "was changed.";
      return out;
    }
    const bool identity = warp_.isIdentity();

    if (target_ == TransformTarget::Layer) {
      TileStore newRgb;
      if (!warpRgbTiles(*layer.rgbTiles, warp_, dstRegion, params.pixels.kernel, &newRgb,
                        &out.error))
        return out;
      *layer.rgbTiles = std::move(newRgb);
      const std::string label = "warp layer";
      od.recordEdit(label, EditKind::Structural);
      active_ = false;
      out.ok = true;
      out.editLabel = label;
      out.exact = identity ? ExactRemap::Identity : ExactRemap::None;
      out.reconstructionPasses = identity ? 0 : 1;
      return out;
    }

    // --- TransformTarget::SelectionPixels, warped -------------------------
    Clipboard clip = cutThroughSelection(layer, &selectionSnapshot_);
    if (clip.empty() || !clip.rgbTiles.has_value()) {
      out.error = "warp commit refused: the selection covers no pixels on this layer. Nothing "
                  "was changed.";
      return out;
    }
    TileStore moved;
    std::string err;
    if (!warpRgbTiles(*clip.rgbTiles, warp_, dstRegion, params.pixels.kernel, &moved, &err)) {
      // Unreachable in practice (the same regions and net that already
      // produced a non-empty dstRegion above), but restore what
      // cutThroughSelection() removed rather than leave a hole, matching the
      // affine path's own recovery just below.
      compositeStoreOverRegion(*clip.rgbTiles, sourceBounds_, &*layer.rgbTiles);
      out.error = "warp commit refused: " + err + " The cut content was restored in place.";
      return out;
    }
    compositeStoreOverRegion(moved, dstRegion, &*layer.rgbTiles);

    // The selection moves with the pixels, same net and regions -- the warp
    // sibling of the affine path's `transformSelectionCoverage()` call above.
    Selection movedSelection;
    std::string selErr;
    const bool selectionMoved = warpSelectionCoverage(
        selectionSnapshot_, sourceBounds_, warp_, dstRegion, params.pixels.kernel,
        &movedSelection, &selErr);
    if (selectionMoved) od.selection = movedSelection;

    const std::string label = "warp selection";
    od.recordEdit(label, EditKind::Structural);
    active_ = false;
    out.ok = true;
    out.editLabel = label;
    out.exact = identity ? ExactRemap::Identity : ExactRemap::None;
    out.reconstructionPasses = identity ? 0 : 1;
    return out;
  }

  // An identity transform is a no-op: nothing is written, nothing is
  // recorded. See this header's section 7 for why that is this file's own
  // decision rather than inherited from TransformStack's "no-op" rule.
  // Duplicate mode is the one exception: a zero-distance Option-drag still
  // stamps a copy, exactly as Photoshop's own Option-click does, so it falls
  // through to the branches below instead of short-circuiting here.
  if (!duplicate_ && pending_.m == mat3Identity().m) {
    active_ = false;
    out.ok = true;
    out.exact = ExactRemap::Identity;
    return out;
  }

  if (target_ == TransformTarget::LayerSet) {
    // Header section 8 / `core/LayerSetOps.hpp` section 3's atomicity
    // discipline, one level up: run against a COPY of the document, one
    // member at a time, through the identical per-layer entry point the
    // single-layer path uses; the first refusal discards the copy and `od`
    // is never touched. `transformLayer()`'s own refusals (locked, no
    // pixels) cannot fire here -- `beginLayerSet()` already excluded every
    // layer that would trip them -- but a non-invertible `pending_` still
    // can, uniformly, since it is the one matrix every member shares; this
    // loop is what makes that refusal atomic across the whole set rather
    // than something a partial loop could half-apply before discovering it.
    Document scratch = od.document;
    for (const size_t index : layerIndices_) {
      const LayerTransformResult r = transformForSession(scratch, index, pending_, params);
      if (!r.ok) {
        out.error = "transform commit refused: " + layerLabel(od.document, index) +
                   " -- " + r.error;
        return out;
      }
      out.reconstructionPasses = std::max(out.reconstructionPasses, r.reconstructionPasses);
    }
    od.document = std::move(scratch);
    out.exact = exactRemapKind(pending_);
    out.editLabel = "transform " + std::to_string(layerIndices_.size()) + " layers";
    od.recordEdit(out.editLabel, EditKind::Structural);
    active_ = false;
    out.ok = true;
    return out;
  }

  if (target_ == TransformTarget::Layer) {
    // PRD M9's Option-drag duplicate: insert the copy first and transform
    // THAT index, leaving `layerIndex_` (the source) untouched. A failed
    // transform rolls the insert back via `removeLayer()` so a refused
    // duplicate leaves the document exactly as `cutThroughSelection()`'s own
    // refusal path does elsewhere in this function -- unchanged.
    if (duplicate_) {
      const LayerOpResult dup = duplicateLayer(od.document, layerIndex_);
      if (!dup.ok) {
        out.error = dup.error;
        return out;
      }
      const size_t newIndex = dup.index;
      const LayerTransformResult r = transformForSession(od.document, newIndex, pending_, params);
      if (!r.ok) {
        removeLayer(od.document, newIndex);
        out.error = r.error;
        return out;
      }
      od.activeLayer = newIndex;
      const std::string label = "duplicate and move " + layerLabel(od.document, newIndex);
      od.recordEdit(label, EditKind::Structural);
      active_ = false;
      out.ok = true;
      out.editLabel = label;
      out.exact = r.exact;
      out.reconstructionPasses = r.reconstructionPasses;
      return out;
    }

    // Text and Vector take their geometry paths: `transformLayer()` would find
    // no stores on them and report success having moved nothing.
    const LayerTransformResult r = transformForSession(od.document, layerIndex_, pending_, params);
    if (!r.ok) {
      out.error = r.error;
      return out;
    }
    od.recordEdit(r.editLabel, EditKind::Structural);
    active_ = false;
    out.ok = true;
    out.editLabel = r.editLabel;
    out.exact = r.exact;
    out.reconstructionPasses = r.reconstructionPasses;
    return out;
  }

  // --- TransformTarget::SelectionPixels -----------------------------------
  if (layerIndex_ >= od.document.layers.size()) {
    out.error = "transform commit refused: the target layer no longer exists.";
    return out;
  }
  Layer& layer = od.document.layers[layerIndex_];
  if (layer.locked) {
    out.error = "transform commit refused: " + layerLabel(od.document, layerIndex_) +
               " is locked. Unlock it first.";
    return out;
  }
  if (layer.kind == LayerKind::Pigment || !layer.rgbTiles.has_value()) {
    out.error = "transform commit refused: " + layerLabel(od.document, layerIndex_) +
               " no longer holds transformable RGB pixels.";
    return out;
  }

  // Refuse everything refusable BEFORE cutThroughSelection(), which is
  // destructive (it erases the selected pixels from the layer as part of the
  // same call) -- a refusal here must leave the document untouched, matching
  // every refusal elsewhere in this codebase.
  Mat3 inv;
  if (!mat3Invert(pending_, &inv)) {
    out.error = "transform commit refused: the pending matrix is not invertible -- a collapsed "
               "or zero-scale transform has no source position for a destination pixel to read "
               "from. Nothing was changed.";
    return out;
  }
  const DocumentRegion dstRegion = transformedRegion(pending_, sourceBounds_);
  if (dstRegion.empty()) {
    out.error = "transform commit refused: this transform collapses the selection to zero "
               "pixels. Nothing was changed.";
    return out;
  }

  // PRD M9's Option-drag duplicate: copy rather than cut, so the source
  // pixels are never removed -- `duplicate_` is the only difference between
  // this path and a plain Move.
  Clipboard clip = duplicate_ ? copyThroughSelection(layer, &selectionSnapshot_)
                              : cutThroughSelection(layer, &selectionSnapshot_);
  if (clip.empty() || !clip.rgbTiles.has_value()) {
    out.error = "transform commit refused: the selection covers no pixels on this layer. "
               "Nothing was changed.";
    return out;
  }

  TileStore moved;
  TransformReport report;
  std::string err;
  if (!transformRgbTiles(*clip.rgbTiles, sourceBounds_, pending_, dstRegion, params.pixels, &moved,
                         &report, &err)) {
    // Unreachable given the checks above (same matrix, same regions the
    // invertibility/extent checks already passed) -- but if it ever does
    // happen and this was a cut, restore what cutThroughSelection() removed
    // rather than leave a hole with nothing to show for it. Nothing to
    // restore when duplicating: the source was only ever read.
    if (!duplicate_) compositeStoreOverRegion(*clip.rgbTiles, sourceBounds_, &*layer.rgbTiles);
    out.error = "transform commit refused: " + err +
               (duplicate_ ? " Nothing was changed." : " The cut content was restored in place.");
    return out;
  }

  compositeStoreOverRegion(moved, dstRegion, &*layer.rgbTiles);

  // Section 3: the selection moves with the pixels, through the identical
  // matrix and regions. If this secondary step somehow fails, the pixel move
  // above already succeeded and must not be reverted for it -- the selection
  // is left as it was rather than the edit being thrown away.
  Selection movedSelection;
  TransformReport selReport;
  std::string selErr;
  if (transformSelectionCoverage(selectionSnapshot_, sourceBounds_, pending_, dstRegion,
                                 params.pixels, &movedSelection, &selReport, &selErr)) {
    od.selection = movedSelection;
  }

  const std::string label = duplicate_ ? "duplicate and move selection" : "transform selection";
  od.recordEdit(label, EditKind::Structural);
  active_ = false;
  out.ok = true;
  out.editLabel = label;
  out.exact = report.exact;
  out.reconstructionPasses = report.reconstructionPasses;
  return out;
}

}  // namespace np
