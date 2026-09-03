#include "app/CropTool.hpp"

#include <algorithm>
#include <cmath>

#include "app/AppState.hpp"  // enum class Tool, for toolCropsCanvas() alone
#include "core/LayerGeometry.hpp"
#include "core/SelectionMask.hpp"

namespace np {
namespace {

float edgeLength(const Point2& a, const Point2& b) noexcept {
  return std::hypot(b.x - a.x, b.y - a.y);
}

// The z of the 2D cross product of the two edges meeting at corner `i`:
// positive when the ring turns the same way an unmirrored top-left ->
// top-right -> bottom-right -> bottom-left ring turns in image coordinates
// (x right, y DOWN), which is clockwise on screen and therefore a positive
// cross. Sign is the whole content of the convexity test below.
float cornerTurn(const CropQuad& q, int i) noexcept {
  const Point2& a = q.c[static_cast<size_t>(i)];
  const Point2& b = q.c[static_cast<size_t>((i + 1) & 3)];
  const Point2& c = q.c[static_cast<size_t>((i + 2) & 3)];
  const float ux = b.x - a.x, uy = b.y - a.y;
  const float vx = c.x - b.x, vy = c.y - b.y;
  return ux * vy - uy * vx;
}

bool allFinite(const CropQuad& q) noexcept {
  for (const Point2& p : q.c) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
  }
  return true;
}

// The two horizontal edge lengths (top, bottom) and the two vertical ones
// (left, right), in that order. One place that decides which corner pair is
// which edge, because getting that wrong is invisible on a square test quad and
// obvious on nothing else.
struct QuadEdges {
  float top = 0.0f, bottom = 0.0f, left = 0.0f, right = 0.0f;
};

QuadEdges quadEdges(const CropQuad& q) noexcept {
  QuadEdges e;
  e.top = edgeLength(q.c[0], q.c[1]);     // TL -> TR
  e.bottom = edgeLength(q.c[3], q.c[2]);  // BL -> BR
  e.left = edgeLength(q.c[0], q.c[3]);    // TL -> BL
  e.right = edgeLength(q.c[1], q.c[2]);   // TR -> BR
  return e;
}

DocumentRegion extentFrom(float w, float h) noexcept {
  DocumentRegion r;
  r.x = 0;
  r.y = 0;
  // Floored at one texel rather than at zero: `cropDocument()` and
  // `transformDocument()` both refuse a zero extent, and a caller asking this
  // function for an extent has already decided to warp something. Whether the
  // quad is *meaningful* is `cropQuadRefusal()`'s question, not this one's.
  const float rw = std::round(w);
  const float rh = std::round(h);
  r.width = static_cast<uint32_t>(std::max(1.0f, std::isfinite(rw) ? rw : 1.0f));
  r.height = static_cast<uint32_t>(std::max(1.0f, std::isfinite(rh) ? rh : 1.0f));
  return r;
}

// The intersection of `r` with `[0, w) x [0, h)`. Empty (width or height 0)
// when they do not overlap. Section 7's "neither menu item can grow the
// document".
DocumentRegion intersectCanvas(const DocumentRegion& r, uint32_t w, uint32_t h) noexcept {
  const int64_t x0 = std::max<int64_t>(r.x, 0);
  const int64_t y0 = std::max<int64_t>(r.y, 0);
  const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(r.x) + r.width, w);
  const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(r.y) + r.height, h);
  DocumentRegion out;
  if (x1 <= x0 || y1 <= y0) return out;
  out.x = static_cast<int32_t>(x0);
  out.y = static_cast<int32_t>(y0);
  out.width = static_cast<uint32_t>(x1 - x0);
  out.height = static_cast<uint32_t>(y1 - y0);
  return out;
}

DocumentTransformResult refusal(const Document& doc, std::string why) {
  DocumentTransformResult r;
  r.ok = false;
  r.error = std::move(why);
  r.previousWidth = static_cast<int32_t>(doc.width);
  r.previousHeight = static_cast<int32_t>(doc.height);
  return r;
}

}  // namespace

const CropModeRow kCropModes[kCropModeCount] = {
    {CropMode::Rectangle, "Rectangle",
     "Drag a rectangle, adjust it by its eight handles, then Enter to crop or Escape to "
     "cancel. No resample: the pixels that survive are bit-for-bit the pixels that were "
     "there."},
    {CropMode::Perspective, "Perspective",
     "Put the four corners on something that ought to be a rectangle -- a receding wall, a "
     "photographed page -- and the crop straightens it. The output is as wide as the LONGER "
     "of the two horizontal edges and as tall as the longer of the two vertical ones, so the "
     "nearest edge is never resampled down. It does NOT recover the scene's true aspect "
     "ratio: no rule can, from four points alone, without knowing the camera. Switching back "
     "to Rectangle snaps the corners to their bounding box."},
};

const char* cropModeLabel(CropMode mode) noexcept {
  for (const CropModeRow& row : kCropModes)
    if (row.mode == mode) return row.label;
  return "Rectangle";
}

// --------------------------------------------------------------------------
// Section 2 -- the rectangle rule.
// --------------------------------------------------------------------------

DocumentRegion cropRegionFromDrag(float x0, float y0, float x1, float y1) noexcept {
  DocumentRegion r;
  if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
    return r;
  const float lox = std::min(x0, x1), hix = std::max(x0, x1);
  const float loy = std::min(y0, y1), hiy = std::max(y0, y1);
  // Outward: floor the near edge, ceil the far one. See the header -- a
  // destructive op that has to round guesses in favour of keeping pixels.
  const float fx = std::floor(lox), fy = std::floor(loy);
  const float cx = std::ceil(hix), cy = std::ceil(hiy);
  r.x = static_cast<int32_t>(fx);
  r.y = static_cast<int32_t>(fy);
  // **A DEGENERATE interval stays degenerate; a merely sub-texel one does not.**
  // Outward rounding of `[10.2, 10.4]` is one whole texel, which is right --
  // the user pointed at that texel. Outward rounding of `[10.2, 10.2]` would be
  // one whole texel too, which is wrong: that is a click, not a drag, and a
  // click that armed a 1x1 crop would put the next Enter one keystroke away
  // from destroying the document. So an axis with no extent at all reports
  // zero, and `applyCropRegion()` refuses the region by name -- the same "an
  // empty gesture is not a gesture" rule `commitDrawnSelection()` applies to a
  // zero-area marquee.
  r.width = hix > lox ? static_cast<uint32_t>(cx - fx) : 0u;
  r.height = hiy > loy ? static_cast<uint32_t>(cy - fy) : 0u;
  return r;
}

CropQuad cropQuadFromRegion(const DocumentRegion& region) noexcept {
  const float x0 = static_cast<float>(region.x);
  const float y0 = static_cast<float>(region.y);
  const float x1 = x0 + static_cast<float>(region.width);
  const float y1 = y0 + static_cast<float>(region.height);
  return CropQuad{{Point2{x0, y0}, Point2{x1, y0}, Point2{x1, y1}, Point2{x0, y1}}};
}

// --------------------------------------------------------------------------
// Section 3 -- the output rectangle.
// --------------------------------------------------------------------------

DocumentRegion perspectiveCropExtent(const CropQuad& quad) noexcept {
  if (!allFinite(quad)) return extentFrom(1.0f, 1.0f);
  const QuadEdges e = quadEdges(quad);
  return extentFrom(std::max(e.top, e.bottom), std::max(e.left, e.right));
}

DocumentRegion perspectiveCropExtentByMean(const CropQuad& quad) noexcept {
  if (!allFinite(quad)) return extentFrom(1.0f, 1.0f);
  const QuadEdges e = quadEdges(quad);
  return extentFrom((e.top + e.bottom) * 0.5f, (e.left + e.right) * 0.5f);
}

// --------------------------------------------------------------------------
// Section 4 -- the refusal ladder.
// --------------------------------------------------------------------------

std::string cropQuadRefusal(const CropQuad& quad) {
  if (!allFinite(quad))
    return "Perspective crop refused: a corner is not a finite point. Drag it back onto the "
           "canvas.";

  // Collapsed, tested pairwise rather than only on adjacent corners: two
  // opposite corners dropped on top of each other leave a "quad" that is a
  // triangle with a doubled vertex, which is not caught by any turn test
  // because two of the four turns are exactly zero either way.
  for (int i = 0; i < kCropCornerCount; ++i) {
    for (int j = i + 1; j < kCropCornerCount; ++j) {
      if (edgeLength(quad.c[static_cast<size_t>(i)], quad.c[static_cast<size_t>(j)]) < 1.0f) {
        return "Perspective crop refused: two corners are on the same texel, so the four "
               "points name a triangle rather than a quadrilateral. Pull them apart.";
      }
    }
  }

  int positive = 0, negative = 0;
  for (int i = 0; i < kCropCornerCount; ++i) {
    const float turn = cornerTurn(quad, i);
    // Normalised by the two edge lengths, so the test is on the SINE of the
    // corner's angle rather than on a raw cross product -- an area threshold
    // would refuse a small valid quad and accept a large near-degenerate one,
    // which is exactly backwards. The corner index in the message is the one
    // the handle carries on screen.
    const float lenA = edgeLength(quad.c[static_cast<size_t>(i)],
                                  quad.c[static_cast<size_t>((i + 1) & 3)]);
    const float lenB = edgeLength(quad.c[static_cast<size_t>((i + 1) & 3)],
                                  quad.c[static_cast<size_t>((i + 2) & 3)]);
    const float denom = lenA * lenB;
    const float sine = denom > 0.0f ? std::fabs(turn) / denom : 0.0f;
    if (sine < kMinCornerSin) {
      return "Perspective crop refused: three corners are almost on one line, so there is no "
             "quadrilateral to straighten. The solve loses the far corner well before it "
             "becomes singular, so this is refused rather than warped.";
    }
    if (turn > 0.0f) ++positive;
    else ++negative;
  }

  if (positive != 0 && negative != 0) {
    return "Perspective crop refused: the outline crosses itself. One corner has been dragged "
           "past its neighbour, so the crop would fold the picture through itself. Drag it "
           "back.";
  }
  if (positive == 0) {
    return "Perspective crop refused: the corners run the wrong way round, so the result would "
           "come out mirrored. Take the corner handles round in the order the outline shows: "
           "top-left, top-right, bottom-right, bottom-left.";
  }

  const DocumentRegion extent = perspectiveCropExtent(quad);
  if (extent.width < 1u || extent.height < 1u) {
    return "Perspective crop refused: the corners enclose less than one texel.";
  }
  return {};
}

bool cropQuadIsUsable(const CropQuad& quad) { return cropQuadRefusal(quad).empty(); }

bool cropQuadIsSteep(const CropQuad& quad) {
  if (!cropQuadIsUsable(quad)) return false;
  const QuadEdges e = quadEdges(quad);
  const float h = std::max(e.top, e.bottom) / std::max(1e-6f, std::min(e.top, e.bottom));
  const float v = std::max(e.left, e.right) / std::max(1e-6f, std::min(e.left, e.right));
  return h > kSteepQuadEdgeRatio || v > kSteepQuadEdgeRatio;
}

// --------------------------------------------------------------------------
// Section 6 -- the eighth canvas gate.
// --------------------------------------------------------------------------

bool toolCropsCanvas(Tool tool) noexcept { return tool == Tool::Crop; }

// --------------------------------------------------------------------------
// Section 5 -- the gesture.
// --------------------------------------------------------------------------

std::array<Point2, kCropHandleCount> cropHandlePoints(const CropQuad& quad) noexcept {
  std::array<Point2, kCropHandleCount> h{};
  for (int i = 0; i < kCropCornerCount; ++i) h[static_cast<size_t>(i)] = quad.c[static_cast<size_t>(i)];
  auto mid = [](const Point2& a, const Point2& b) {
    return Point2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  };
  h[4] = mid(quad.c[0], quad.c[1]);  // top
  h[5] = mid(quad.c[1], quad.c[2]);  // right
  h[6] = mid(quad.c[3], quad.c[2]);  // bottom
  h[7] = mid(quad.c[0], quad.c[3]);  // left
  return h;
}

int cropHandleAt(const CropQuad& quad, CropMode mode, float x, float y, float radius) noexcept {
  const std::array<Point2, kCropHandleCount> h = cropHandlePoints(quad);
  // Corners first and returned on the first hit, so a corner always wins a tie
  // with the edge handle overlapping it on a small rectangle. Two passes rather
  // than one pass plus a distance comparison: "the more specific handle wins"
  // is the rule, not "the nearer one", and on a 3-texel-wide crop the nearer
  // one is a coin toss.
  const int last = mode == CropMode::Rectangle ? kCropHandleCount : kCropCornerCount;
  for (int i = 0; i < kCropCornerCount; ++i) {
    if (edgeLength(h[static_cast<size_t>(i)], Point2{x, y}) <= radius) return i;
  }
  for (int i = kCropCornerCount; i < last; ++i) {
    if (edgeLength(h[static_cast<size_t>(i)], Point2{x, y}) <= radius) return i;
  }
  return -1;
}

void cropDragHandle(CropSession& session, int handle, float x, float y) noexcept {
  if (handle < 0 || handle >= kCropHandleCount) return;
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  CropQuad& q = session.quad;

  if (session.mode == CropMode::Perspective) {
    // A corner moves alone; there is no meaning yet agreed for dragging the
    // midpoint of a perspective edge, so an edge handle is ignored rather than
    // guessed at (and `cropHandleAt()` does not offer one in this mode, so this
    // arm is reached only by a caller passing an index of its own).
    if (handle < kCropCornerCount) q.c[static_cast<size_t>(handle)] = Point2{x, y};
    return;
  }

  // Rectangle mode: the four corners must go on naming a rectangle, so a corner
  // drags its two neighbours and an edge handle moves one side. Written as
  // "which of the two x's and which of the two y's does this handle own",
  // because the alternative -- rebuilding the quad from a sorted box -- would
  // renumber the corners the moment a drag crossed the opposite edge, and the
  // handle under the pointer would jump to a different index mid-drag.
  const bool ownsLeft = handle == 0 || handle == 3 || handle == 7;
  const bool ownsRight = handle == 1 || handle == 2 || handle == 5;
  const bool ownsTop = handle == 0 || handle == 1 || handle == 4;
  const bool ownsBottom = handle == 2 || handle == 3 || handle == 6;

  float left = q.c[0].x, right = q.c[1].x, top = q.c[0].y, bottom = q.c[2].y;
  if (ownsLeft) left = x;
  if (ownsRight) right = x;
  if (ownsTop) top = y;
  if (ownsBottom) bottom = y;

  q.c[0] = Point2{left, top};
  q.c[1] = Point2{right, top};
  q.c[2] = Point2{right, bottom};
  q.c[3] = Point2{left, bottom};
}

DocumentRegion cropRegionOf(const CropSession& session) noexcept {
  const CropQuad& q = session.quad;
  return cropRegionFromDrag(q.c[0].x, q.c[0].y, q.c[2].x, q.c[2].y);
}

void cropSetMode(CropSession& session, CropMode mode) noexcept {
  if (session.mode == mode) return;
  if (mode == CropMode::Rectangle && session.active) {
    // Snap to the bounding box. Lossy, and named as such in the header and in
    // the combo's tooltip: four dragged corners cannot survive a round trip
    // through two points, and silently keeping them while drawing a rectangle
    // would be a shape on screen that is not the shape that commits.
    float lox = session.quad.c[0].x, hix = lox, loy = session.quad.c[0].y, hiy = loy;
    for (const Point2& p : session.quad.c) {
      lox = std::min(lox, p.x);
      hix = std::max(hix, p.x);
      loy = std::min(loy, p.y);
      hiy = std::max(hiy, p.y);
    }
    session.quad = CropQuad{{Point2{lox, loy}, Point2{hix, loy}, Point2{hix, hiy},
                             Point2{lox, hiy}}};
  }
  session.mode = mode;
}

void cropBeginDefine(CropSession& session, DocumentId doc, CropMode mode, float x,
                     float y) noexcept {
  session.active = false;
  session.defining = true;
  session.anchorX = x;
  session.anchorY = y;
  session.doc = doc;
  session.mode = mode;
  session.dragHandle = -1;
  // An offset from whatever drag last used this field has no meaning for a
  // brand-new one (`app/SelectionDrag.hpp`'s `SelectionMoveState` comment).
  session.move = SelectionMoveState{};
  // The moving corner starts ON the anchor, so the first frame is degenerate
  // and refuses rather than showing last crop's rectangle pinned to a point --
  // `--gradient-demo drag`'s own argument for starting `x1` on `x0`.
  session.quad = CropQuad{{Point2{x, y}, Point2{x, y}, Point2{x, y}, Point2{x, y}}};
}

void cropCancel(CropSession& session) noexcept { session = CropSession{}; }

// --------------------------------------------------------------------------
// Section 7 -- commit, and the two menu items.
// --------------------------------------------------------------------------

DocumentTransformResult applyCropRegion(OpenDocument& doc, const DocumentRegion& region) {
  if (region.empty()) {
    return refusal(doc.document,
                   "Crop refused: the rectangle is less than one texel wide or tall.");
  }
  // A crop that asks for exactly the canvas is a no-op the user asked for, and
  // `applyImageSize()`'s rule applies: it is not an edit and records no history
  // entry. Reported as `ok`, with the extent unchanged, so a caller can tell
  // "nothing to do" from "refused" by reading `ok` rather than by comparing
  // strings.
  if (region.x == 0 && region.y == 0 && region.width == doc.document.width &&
      region.height == doc.document.height) {
    DocumentTransformResult r;
    r.ok = true;
    r.editLabel = "crop";
    r.previousWidth = static_cast<int32_t>(doc.document.width);
    r.previousHeight = static_cast<int32_t>(doc.document.height);
    return r;
  }
  Selection* selection = doc.selection.has_value() ? &*doc.selection : nullptr;
  const DocumentTransformResult r =
      cropDocument(doc.document, region.x, region.y, region.width, region.height, selection);
  if (r.ok) doc.recordEdit(r.editLabel, EditKind::Structural);
  return r;
}

DocumentTransformResult applyCropPerspective(OpenDocument& doc, const CropQuad& quad) {
  // The tool's own refusal first, so the sentence the user reads at commit is
  // the identical sentence the canvas has been showing them during the drag --
  // one predicate, two readers, `app/GradientTool.hpp` section 7's rule.
  const std::string why = cropQuadRefusal(quad);
  if (!why.empty()) return refusal(doc.document, why);

  const DocumentRegion extent = perspectiveCropExtent(quad);
  const std::array<Point2, 4> dst{
      Point2{0.0f, 0.0f}, Point2{static_cast<float>(extent.width), 0.0f},
      Point2{static_cast<float>(extent.width), static_cast<float>(extent.height)},
      Point2{0.0f, static_cast<float>(extent.height)}};

  Mat3 dstFromSrc;
  std::string solveError;
  // **The engine's refusal is kept, not swallowed.** `cropQuadRefusal()` above
  // is a judgement about which quads are meaningful; this one is a fact about
  // which systems are solvable, and they are genuinely different failures --
  // conflating them would make a numerical surprise read as a user error.
  if (!transformFromQuad(quad.c, dst, &dstFromSrc, &solveError)) {
    return refusal(doc.document, solveError);
  }

  DocumentTransformParams params;
  Selection* selection = doc.selection.has_value() ? &*doc.selection : nullptr;
  DocumentTransformResult r = transformDocument(doc.document, dstFromSrc, extent.width,
                                                extent.height, params, selection);
  if (r.ok) {
    // The one place the engine's `editLabel` is overridden rather than passed
    // through. `transformDocument()` answers "transform document", which is
    // correct for the engine and wrong in an Edit menu that already offers
    // "Undo crop": the user did not transform a document, they cropped one,
    // and an undo entry that does not use the word on the tool they just
    // clicked is an undo entry they will not trust.
    r.editLabel = "perspective crop";
    doc.recordEdit(r.editLabel, EditKind::Structural);
  }
  return r;
}

DocumentTransformResult applyCropSession(CropSession& session, OpenDocument& doc) {
  if (!session.active) {
    return refusal(doc.document, "Crop refused: no crop rectangle has been drawn.");
  }
  if (session.doc != doc.id) {
    // The `DocumentId` on the session earning its place: a rectangle drawn on
    // one tab means nothing on another, and committing it would crop the wrong
    // picture to plausible-looking numbers.
    return refusal(doc.document,
                   "Crop refused: the crop rectangle was drawn on a different document.");
  }
  DocumentTransformResult r = session.mode == CropMode::Rectangle
                                  ? applyCropRegion(doc, cropRegionOf(session))
                                  : applyCropPerspective(doc, session.quad);
  if (r.ok) cropCancel(session);
  return r;
}

std::optional<DocumentRegion> cropToSelectionRegion(const OpenDocument& doc) {
  if (!doc.selection.has_value()) return std::nullopt;
  const std::optional<SelectionBounds> b = selectionBounds(*doc.selection);
  if (!b.has_value()) return std::nullopt;
  // `SelectionBounds` is already half-open (`width() == x1 - x0`), so this is a
  // relabelling and not the inclusive/half-open conversion `regionFromBounds()`
  // exists for -- the two rectangle conventions this codebase keeps apart
  // (`ops/DocumentTransform.hpp`'s `DocumentRegion` comment).
  if (b->x1 <= b->x0 || b->y1 <= b->y0) return std::nullopt;
  DocumentRegion r;
  r.x = b->x0;
  r.y = b->y0;
  r.width = static_cast<uint32_t>(b->x1 - b->x0);
  r.height = static_cast<uint32_t>(b->y1 - b->y0);
  const DocumentRegion clipped = intersectCanvas(r, doc.document.width, doc.document.height);
  if (clipped.empty()) return std::nullopt;
  return clipped;
}

std::optional<DocumentRegion> trimToContentRegion(const Document& doc) {
  LayerBounds all;
  for (const Layer& layer : doc.layers) all = unionLayerBounds(all, layerContentBounds(layer));
  if (all.empty) return std::nullopt;
  const DocumentRegion clipped =
      intersectCanvas(regionFromBounds(all), doc.width, doc.height);
  if (clipped.empty()) return std::nullopt;
  return clipped;
}

DocumentTransformResult applyCropToSelection(OpenDocument& doc) {
  const std::optional<DocumentRegion> region = cropToSelectionRegion(doc);
  if (!region.has_value()) {
    return refusal(doc.document,
                   "Crop to Selection refused: there is no selection, or it covers nothing "
                   "inside the canvas.");
  }
  return applyCropRegion(doc, *region);
}

DocumentTransformResult applyTrimToContent(OpenDocument& doc) {
  const std::optional<DocumentRegion> region = trimToContentRegion(doc.document);
  if (!region.has_value()) {
    return refusal(doc.document,
                   "Trim to Content refused: every layer is empty, so there is nothing to trim "
                   "to. Trimming to nothing would delete the document rather than crop it.");
  }
  return applyCropRegion(doc, *region);
}

}  // namespace np
