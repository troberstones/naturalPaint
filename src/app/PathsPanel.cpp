#include "app/PathsPanel.hpp"

namespace np {

PathOpRefusal PathOpAvailability::refusalFor(PathOp op) const noexcept {
  for (size_t i = 0; i < kPathPanelOps.size(); ++i)
    if (kPathPanelOps[i] == op) return refusal[i];
  // Unreachable while `kPathPanelOps` lists every enumerator, which
  // app/selftest/PathsPanel.cpp asserts against a hand-written second list.
  // `StaleSelection` rather than `None` so a missing row greys the button
  // instead of lighting one whose verb nobody swept.
  return PathOpRefusal::StaleSelection;
}

bool PathOpAvailability::enabled(PathOp op) const noexcept {
  return refusalFor(op) == PathOpRefusal::None;
}

PathOpAvailability pathOpAvailability(const std::vector<VectorShape>& shapes,
                                      const PathSelection& selection) {
  PathOpAvailability a;
  for (size_t i = 0; i < kPathPanelOps.size(); ++i)
    a.refusal[i] = pathOpCanRun(kPathPanelOps[i], shapes, selection);
  return a;
}

std::optional<size_t> pathPaintTargetBelow(const std::vector<Layer>& layers, size_t vectorIndex,
                                           PathPaintKind kind) {
  if (vectorIndex >= layers.size()) return std::nullopt;
  for (size_t i = vectorIndex; i-- > 0;) {
    const Layer& l = layers[i];
    // The tile store, not the kind alone. A Group and an Adjustment are
    // neither, but a malformed RGB layer with no store engaged would pass a
    // kind test and then be refused by the consumer -- and the consumer's
    // sentence for that one ("no tile store") tells the user nothing they can
    // act on, unlike the lock refusals this function deliberately lets
    // through.
    if (l.kind == LayerKind::RGB && l.rgbTiles.has_value()) return i;
    if (kind == PathPaintKind::Stroke && l.kind == LayerKind::Pigment &&
        l.pigmentTiles.has_value())
      return i;
  }
  return std::nullopt;
}

}  // namespace np
