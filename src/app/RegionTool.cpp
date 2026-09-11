#include "app/RegionTool.hpp"

#include <algorithm>
#include <cmath>

#include "app/AppState.hpp"  // enum class Tool, for toolCreatesRegions() alone
#include "app/SelectionDrag.hpp"

namespace np {

bool toolCreatesRegions(Tool tool) noexcept {
  return tool == Tool::Frame || tool == Tool::Slice;
}

RegionKind regionKindForTool(Tool tool) noexcept {
  return tool == Tool::Slice ? RegionKind::Slice : RegionKind::Frame;
}

Tool toolForRegionKind(RegionKind kind) noexcept {
  return kind == RegionKind::Slice ? Tool::Slice : Tool::Frame;
}

const Region* findRegionById(const Document& doc, uint64_t id) noexcept {
  if (id == 0) return nullptr;
  for (const Region& r : doc.regions)
    if (r.id == id) return &r;
  return nullptr;
}

size_t indexOfRegionId(const Document& doc, uint64_t id) noexcept {
  for (size_t i = 0; i < doc.regions.size(); ++i)
    if (doc.regions[i].id == id) return i;
  return doc.regions.size();
}

namespace {

bool pointInRegion(const Region& r, float x, float y) noexcept {
  return x >= static_cast<float>(r.x) && x < static_cast<float>(r.x) + static_cast<float>(r.width) &&
         y >= static_cast<float>(r.y) && y < static_cast<float>(r.y) + static_cast<float>(r.height);
}

}  // namespace

const Region* regionAt(const Document& doc, RegionKind kind, float x, float y) noexcept {
  // Most-recently-added first: the last entry in the list is what a click
  // "sees" first, the same z-order convention this codebase already applies
  // to handle preference (`app/CropTool::cropHandleAt()`) and to which
  // layer a click paints.
  for (size_t i = doc.regions.size(); i-- > 0;) {
    const Region& r = doc.regions[i];
    if (r.kind == kind && pointInRegion(r, x, y)) return &r;
  }
  return nullptr;
}

std::array<Point2, 4> regionHandlePoints(const Region& region) noexcept {
  const float x0 = static_cast<float>(region.x);
  const float y0 = static_cast<float>(region.y);
  const float x1 = x0 + static_cast<float>(region.width);
  const float y1 = y0 + static_cast<float>(region.height);
  // CropQuad's own fixed order: top-left, top-right, bottom-right,
  // bottom-left.
  return {Point2{x0, y0}, Point2{x1, y0}, Point2{x1, y1}, Point2{x0, y1}};
}

int regionHandleAt(const Region& region, float x, float y, float radius) noexcept {
  const std::array<Point2, 4> pts = regionHandlePoints(region);
  int best = -1;
  float bestDist = radius;
  for (int i = 0; i < 4; ++i) {
    const float d = std::hypot(pts[static_cast<size_t>(i)].x - x, pts[static_cast<size_t>(i)].y - y);
    if (d <= bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

void regionBeginDefine(RegionSession& session, DocumentId doc, RegionKind kind, float x,
                       float y) noexcept {
  (void)kind;  // the kind is supplied again at commit; nothing here depends on it
  session = RegionSession{};
  session.doc = doc;
  session.gesture = RegionGesture::Defining;
  session.anchorX = x;
  session.anchorY = y;
}

DocumentRegion regionDefineRect(const RegionSession& session, float curX, float curY, bool square,
                                bool fromCentre) noexcept {
  const SelectionDragBox box = computeSelectionDragBox(session.anchorX, session.anchorY, curX,
                                                       curY, 0.0f, 0.0f, square, fromCentre);
  return cropRegionFromDrag(box.x0, box.y0, box.x1, box.y1);
}

LayerOpResult regionCommitDefine(RegionSession& session, Document& doc, RegionKind kind,
                                 float curX, float curY, bool square, bool fromCentre) {
  const DocumentRegion rect = regionDefineRect(session, curX, curY, square, fromCentre);
  session.gesture = RegionGesture::Idle;
  // `cropRegionFromDrag()` still returns a (possibly 1-texel) rectangle for
  // a zero-extent drag rounded outward on one axis only; `addRegion()`'s own
  // empty check is what actually implements "a click without a drag creates
  // nothing" for the fully degenerate case (both axes) -- see that
  // function's refusal.
  LayerOpResult result = addRegion(doc, kind, rect.x, rect.y, rect.width, rect.height);
  if (result.ok) session.selectedId = doc.regions[result.index].id;
  return result;
}

void regionBeginMove(RegionSession& session, const Document& doc, float x, float y) noexcept {
  const Region* r = findRegionById(doc, session.selectedId);
  if (r == nullptr) return;
  session.gesture = RegionGesture::Moving;
  session.grabOffsetX = x - static_cast<float>(r->x);
  session.grabOffsetY = y - static_cast<float>(r->y);
}

void regionMoveOrigin(const RegionSession& session, float curX, float curY, int32_t* outX,
                      int32_t* outY) noexcept {
  *outX = static_cast<int32_t>(std::lround(curX - session.grabOffsetX));
  *outY = static_cast<int32_t>(std::lround(curY - session.grabOffsetY));
}

LayerOpResult regionCommitMove(RegionSession& session, Document& doc, float curX, float curY) {
  int32_t x = 0, y = 0;
  regionMoveOrigin(session, curX, curY, &x, &y);
  session.gesture = RegionGesture::Idle;
  const size_t index = indexOfRegionId(doc, session.selectedId);
  if (index >= doc.regions.size()) {
    LayerOpResult r;
    r.ok = false;
    r.error = "move region refused: the selected region no longer exists.";
    return r;
  }
  return moveRegion(doc, index, x, y);
}

void regionBeginResize(RegionSession& session, const Document& doc, int handle) noexcept {
  const Region* r = findRegionById(doc, session.selectedId);
  if (r == nullptr || handle < 0 || handle > 3) return;
  const std::array<Point2, 4> pts = regionHandlePoints(*r);
  // The FIXED corner is the one diagonally opposite the one being dragged:
  // 0<->2 (TL/BR), 1<->3 (TR/BL).
  const Point2 fixed = pts[static_cast<size_t>((handle + 2) % 4)];
  session.gesture = RegionGesture::Resizing;
  session.resizeCorner = handle;
  session.fixedX = fixed.x;
  session.fixedY = fixed.y;
}

DocumentRegion regionResizeRect(const RegionSession& session, float curX, float curY) noexcept {
  return cropRegionFromDrag(session.fixedX, session.fixedY, curX, curY);
}

LayerOpResult regionCommitResize(RegionSession& session, Document& doc, float curX, float curY) {
  const DocumentRegion rect = regionResizeRect(session, curX, curY);
  session.gesture = RegionGesture::Idle;
  const size_t index = indexOfRegionId(doc, session.selectedId);
  if (index >= doc.regions.size()) {
    LayerOpResult r;
    r.ok = false;
    r.error = "resize region refused: the selected region no longer exists.";
    return r;
  }
  return resizeRegion(doc, index, rect.x, rect.y, rect.width, rect.height);
}

LayerOpResult regionDeleteSelected(RegionSession& session, Document& doc) {
  const size_t index = indexOfRegionId(doc, session.selectedId);
  if (index >= doc.regions.size()) {
    LayerOpResult r;
    r.ok = false;
    return r;
  }
  LayerOpResult result = deleteRegion(doc, index);
  if (result.ok) session.selectedId = 0;
  return result;
}

void regionCancelGesture(RegionSession& session) noexcept {
  session.gesture = RegionGesture::Idle;
  session.resizeCorner = -1;
}

}  // namespace np
