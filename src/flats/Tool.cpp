#include "flats/Tool.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace np {

namespace {

// One recolour record per fill: the record whose point still lands in the
// fill is replaced rather than stacked -- autoFlats' `recolorRecordFor`.
void noteRecolor(FlatEdits& edits, const FlatEvaluation& e, int fill, const std::array<int, 2>& at, FlatRgb color,
                 int slot) {
  for (FlatRecolor& rc : edits.recolors) {
    if (e.fillAt(rc.x, rc.y) == fill) {
      rc.color = color;
      rc.slot = slot;
      rc.x = static_cast<float>(at[0]);
      rc.y = static_cast<float>(at[1]);
      return;
    }
  }
  edits.recolors.push_back({edits.nextId++, static_cast<float>(at[0]), static_cast<float>(at[1]), slot, color});
}

}  // namespace

FlatRgb flatRgbFromSrgb(const std::array<float, 3>& srgb) {
  FlatRgb out{};
  for (int i = 0; i < 3; i++)
    out[i] = static_cast<uint8_t>(std::lround(std::min(1.f, std::max(0.f, srgb[i])) * 255.f));
  return out;
}

int flatsBucketRecolor(Layer& layer, const FlatEvaluation& e, float x, float y, FlatRgb color, int slot,
                       bool allSameColour) {
  const int hit = e.fillAt(x, y);
  if (!hit || e.fills[hit].deleted) return 0;
  const std::vector<std::array<int, 2>> at = e.anchors();
  int n = 0;
  for (const int r : e.roots()) {
    if (r != hit && !(allSameColour && e.fills[r].color == e.fills[hit].color)) continue;
    if (e.fills[r].deleted || at[r][0] < 0) continue;
    noteRecolor(layer.flats.edits, e, r, at[r], color, slot);
    n++;
  }
  return n;
}

bool flatsBucketCarve(Layer& layer, const FlatEvaluation& e, float x, float y) {
  // Dry-run the carve on a copy so a click with no room records nothing.
  FlatEvaluation probe = e;
  if (!flatCarveAt(probe, static_cast<int>(x), static_cast<int>(y), layer.flats.params.gapSize)) return false;
  layer.flats.edits.carves.push_back({layer.flats.edits.nextId++, x, y});
  return true;
}

bool flatsDeleteFill(Layer& layer, const FlatEvaluation& e, float x, float y) {
  const int hit = e.fillAt(x, y);
  if (!hit || e.fills[hit].deleted) return false;
  layer.flats.edits.deleteMarks.push_back({layer.flats.edits.nextId++, x, y});
  return true;
}

bool flatsMergePair(Layer& layer, const FlatEvaluation& e, float ax, float ay, float bx, float by) {
  const int a = e.fillAt(ax, ay), b = e.fillAt(bx, by);
  if (!a || !b || a == b || e.fills[a].isBg || e.fills[b].isBg) return false;
  layer.flats.edits.mergePairs.push_back({layer.flats.edits.nextId++, ax, ay, bx, by});
  return true;
}

bool flatsDrawMerge(Layer& layer, const FlatEvaluation& e, const FlatPolyline& pts) {
  FlatEvaluation probe = e;
  const FlatMergeOutcome out = flatApplyMergeStroke(probe, pts);
  if (out.merged.empty()) return false;
  layer.flats.edits.mergeStrokes.push_back({layer.flats.edits.nextId++, pts});
  return true;
}

bool flatsGroupFromPath(Layer& layer, const FlatPolyline& path) {
  if (path.size() < 6) return false;
  FlatEdits& ed = layer.flats.edits;
  ed.groups.push_back({ed.nextGroup, "Group " + std::to_string(ed.nextGroup), path});
  ed.nextGroup++;
  return true;
}

bool flatsShapeFromPath(Layer& layer, const FlatPolyline& path, FlatRgb color) {
  if (path.size() < 6) return false;
  FlatEdits& ed = layer.flats.edits;
  ed.shapeFills.push_back({ed.nextId++, path, color, "Shape " + std::to_string(ed.shapeFills.size() + 1)});
  return true;
}

bool flatsBridgeStroke(Layer& layer, const FlatPolyline& pts, bool erase) {
  if (pts.size() < 2) return false;
  layer.flats.edits.bridges.push_back({layer.flats.edits.nextId++, pts, erase});
  return true;
}

bool flatsAcceptSuggestion(Layer& layer, const FlatEvaluation& e, int index) {
  if (index < 0 || static_cast<size_t>(index) >= e.suggestions.size()) return false;
  return flatsBridgeStroke(layer, e.suggestions[index], false);
}

int flatsClusterSmall(Layer& layer, const FlatEvaluation& e, int maxArea) {
  const std::vector<FlatMergePair> pairs = flatClusterSmall(e, maxArea);
  for (FlatMergePair p : pairs) {
    p.id = layer.flats.edits.nextId++;
    layer.flats.edits.mergePairs.push_back(p);
  }
  return static_cast<int>(pairs.size());
}

bool flatsRemoveEditAt(Layer& layer, float x, float y, float reach) {
  const FlatEditRef ref = flatEditAt(layer.flats.edits, x, y, reach);
  return ref.kind != 0 && flatRemoveEdit(layer.flats.edits, ref);
}

int flatsClusterMaxArea(const FlatParams& p) noexcept { return std::max(500, p.minRegion * 10); }

}  // namespace np
