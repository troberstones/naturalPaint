#include "app/VectorStyle.hpp"

#include <algorithm>

namespace np {

VectorStyle vectorStyleOf(const VectorShape& shape) {
  VectorStyle s;
  s.fill = shape.fill;
  s.stroke = shape.stroke;
  s.strokeStyle = shape.strokeStyle;
  return s;
}

void setVectorStyle(VectorShape* shape, const VectorStyle& style) {
  if (shape == nullptr) return;
  shape->fill = style.fill;
  shape->stroke = style.stroke;
  shape->strokeStyle = style.strokeStyle;
}

void setPaintSolidColor(Paint& paint, const std::array<float, 4>& linearRgba) {
  paint.rgba = linearRgba;
  paint.on = true;
  // The gradient index is left where it is rather than zeroed: it is dead
  // while `kind` is Solid, and keeping it means an undo of this edit has
  // nothing to reconstruct.
  paint.kind = PaintKind::Solid;
}

std::vector<uint64_t> vectorStyleTargets(const std::vector<VectorShape>& shapes,
                                         const PathSelection& selection) {
  // The selection is a set of ids (Shape mode) or of anchor references
  // (Component mode). Both answer the same question here -- WHICH SHAPES --
  // and the second answers it by throwing away the anchor: a style is a
  // property of a shape and there is no such thing as one anchor's stroke
  // width.
  std::vector<uint64_t> want;
  if (selection.mode == PathSelectMode::Shape) {
    want = selection.shapes;
  } else {
    want.reserve(selection.components.size());
    for (const ComponentRef& c : selection.components) want.push_back(c.shapeId);
  }
  if (want.empty()) return {};
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());

  // Walking `shapes` rather than `want` is what puts the result in LAYER
  // order, and it is also what drops an id the selection still names but the
  // layer no longer holds -- a shape deleted out from under a stale
  // selection. Reporting that id as a target would make the readout show a
  // style nothing has and the edit change nothing while claiming to.
  std::vector<uint64_t> out;
  out.reserve(want.size());
  for (const VectorShape& s : shapes) {
    if (std::binary_search(want.begin(), want.end(), s.id)) out.push_back(s.id);
  }
  return out;
}

namespace {

const VectorShape* findShape(const std::vector<VectorShape>& shapes, uint64_t id) {
  for (const VectorShape& s : shapes)
    if (s.id == id) return &s;
  return nullptr;
}

bool sameRgba(const std::array<float, 4>& a, const std::array<float, 4>& b) {
  // Exact equality, deliberately. These values are copied between shapes and
  // a swatch, never computed, so a tolerance would only be able to hide a
  // real difference the user deliberately made.
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

}  // namespace

VectorStyleReadout vectorStyleReadout(const std::vector<VectorShape>& shapes,
                                      const PathSelection& selection,
                                      const VectorStyle& fallback) {
  VectorStyleReadout out;
  const std::vector<uint64_t> targets = vectorStyleTargets(shapes, selection);
  out.targetCount = targets.size();
  if (targets.empty()) {
    out.style = fallback;
    out.fromSelection = false;
    return out;
  }
  const VectorShape* first = findShape(shapes, targets.front());
  out.style = first != nullptr ? vectorStyleOf(*first) : fallback;
  out.fromSelection = true;
  for (size_t i = 1; i < targets.size(); ++i) {
    const VectorShape* s = findShape(shapes, targets[i]);
    if (s == nullptr) continue;
    if (s->strokeStyle.width != out.style.strokeStyle.width) out.mixedStrokeWidth = true;
    if (s->stroke.on != out.style.stroke.on) out.mixedStrokeOn = true;
    if (!sameRgba(s->stroke.rgba, out.style.stroke.rgba)) out.mixedStrokeColor = true;
    if (s->fill.on != out.style.fill.on) out.mixedFillOn = true;
    if (!sameRgba(s->fill.rgba, out.style.fill.rgba)) out.mixedFillColor = true;
  }
  return out;
}

size_t applyVectorStyleEdit(std::vector<VectorShape>* shapes, const PathSelection& selection,
                            VectorStyle* fallback, const VectorStyleMutator& mutate) {
  if (!mutate) return 0;
  const std::vector<uint64_t> targets =
      shapes != nullptr ? vectorStyleTargets(*shapes, selection) : std::vector<uint64_t>{};
  if (targets.empty()) {
    // No selection (or none of it survives in this layer): the control edits
    // the tool default, and the document is untouched.
    if (fallback != nullptr) mutate(*fallback);
    return 0;
  }
  // `targets` is in LAYER order, which is NOT id order -- a shape moved up
  // the list keeps its id -- so it cannot be binary-searched as it stands.
  std::vector<uint64_t> byId = targets;
  std::sort(byId.begin(), byId.end());
  size_t changed = 0;
  for (VectorShape& s : *shapes) {
    if (!std::binary_search(byId.begin(), byId.end(), s.id)) continue;
    VectorStyle style = vectorStyleOf(s);
    mutate(style);
    setVectorStyle(&s, style);
    ++changed;
  }
  return changed;
}

}  // namespace np
