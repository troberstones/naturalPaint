#include "app/MeasureLine.hpp"

#include <cmath>
#include <type_traits>

#include "app/DocumentLifecycle.hpp"
#include "brush/Dynamics.hpp"

namespace np {

// The header's §1 promise that `MeasureLine::documentId` IS `DocumentId`,
// enforced rather than commented: the header spells the underlying type to
// stay free of `core/Document` and `io/NpaintFile`, and this is the tripwire
// that turns a widened alias into a build error here instead of a truncated
// id at a call site.
static_assert(std::is_same_v<DocumentId, decltype(MeasureLine::documentId)>,
              "MeasureLine::documentId must be spelled as app/DocumentLifecycle's DocumentId");

MeasureReadout measureReadout(const MeasureLine& line) noexcept {
  MeasureReadout r;
  r.dx = line.x1 - line.x0;
  r.dy = line.y1 - line.y0;
  r.lengthPx = std::sqrt(r.dx * r.dx + r.dy * r.dy);
  // §3: the build's one vector-to-heading function, not a second `atan2`.
  // `dynamicDirection()` returns the heading normalised to [0, 1); the
  // multiply is the same one `app/selftest/AngleConvention.cpp` uses to read
  // it back as degrees.
  r.angleDeg = dynamicDirection(r.dx, r.dy) * 360.0f;
  return r;
}

bool measureLineAppliesTo(const MeasureLine& line, uint64_t activeDocumentId) noexcept {
  return line.active && line.documentId == activeDocumentId;
}

void beginMeasureLine(MeasureLine& line, uint64_t documentId, float x, float y) noexcept {
  line.active = true;
  line.dragging = true;
  line.documentId = documentId;
  line.x0 = x;
  line.y0 = y;
  line.x1 = x;
  line.y1 = y;
}

void updateMeasureLine(MeasureLine& line, float x, float y) noexcept {
  if (!line.dragging) return;
  line.x1 = x;
  line.y1 = y;
}

void endMeasureLine(MeasureLine& line) noexcept { line.dragging = false; }

void clearMeasureLine(MeasureLine& line) noexcept {
  line.active = false;
  line.dragging = false;
}

}  // namespace np
