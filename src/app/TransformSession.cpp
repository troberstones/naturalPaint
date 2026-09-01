#include "app/TransformSession.hpp"

#include <algorithm>
#include <cmath>

#include "core/LayerGeometry.hpp"

namespace np {

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

void TransformSession::cancel() noexcept { *this = TransformSession{}; }

TransformBeginResult TransformSession::beginLayer(const OpenDocument& od, size_t layerIndex,
                                                  const Mat3& initialPending) {
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
  if (!layer.rgbTiles.has_value() && !layer.pigmentTiles.has_value()) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) + " is a " +
             layerKindName(layer.kind) + " layer, which holds no pixels to transform.";
    return r;
  }
  const LayerBounds bounds = layerContentBounds(layer);
  if (bounds.empty) {
    r.error = "transform refused: " + layerLabel(doc, layerIndex) +
             " has no content -- nothing to transform.";
    return r;
  }

  *this = TransformSession{};
  sourceBounds_ = regionFromBounds(bounds);
  // Set together with `layerIndex_`, and never apart from it: the pair is what
  // identifies the pixels this session owns. See the header's beginLayer().
  documentId_ = od.id;
  layerIndex_ = layerIndex;
  target_ = TransformTarget::Layer;
  pending_ = initialPending;
  active_ = true;
  r.ok = true;
  return r;
}

TransformBeginResult TransformSession::beginSelectionPixels(const OpenDocument& od,
                                                             const Selection& selection,
                                                             size_t layerIndex) {
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

  *this = TransformSession{};
  sourceBounds_ = region;
  selectionSnapshot_ = selection;
  documentId_ = od.id;
  layerIndex_ = layerIndex;
  target_ = TransformTarget::SelectionPixels;
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

  // An identity transform is a no-op: nothing is written, nothing is
  // recorded. See this header's section 7 for why that is this file's own
  // decision rather than inherited from TransformStack's "no-op" rule.
  if (pending_.m == mat3Identity().m) {
    active_ = false;
    out.ok = true;
    out.exact = ExactRemap::Identity;
    return out;
  }

  if (target_ == TransformTarget::Layer) {
    const LayerTransformResult r = transformLayer(od.document, layerIndex_, pending_, params);
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

  Clipboard clip = cutThroughSelection(layer, &selectionSnapshot_);
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
    // happen, restore what cutThroughSelection() removed rather than leave a
    // hole with nothing to show for it.
    compositeStoreOverRegion(*clip.rgbTiles, sourceBounds_, &*layer.rgbTiles);
    out.error = "transform commit refused: " + err + " The cut content was restored in place.";
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

  od.recordEdit("transform selection", EditKind::Structural);
  active_ = false;
  out.ok = true;
  out.editLabel = "transform selection";
  out.exact = report.exact;
  out.reconstructionPasses = report.reconstructionPasses;
  return out;
}

}  // namespace np
