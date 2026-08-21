#include "app/ControlsLayout.hpp"

#include <algorithm>

namespace np {

const std::vector<ControlsSectionSpec>& controlsSections() {
  using R = ControlsSectionRole;
  // The order and the default-open set, in one place. See the header for why
  // the two Document sections lead and are the only two that start open.
  static const std::vector<ControlsSectionSpec> kSections = {
      {ControlsSection::Layers, R::Document, "LAYERS", true},
      {ControlsSection::History, R::Document, "HISTORY", true},
      {ControlsSection::Comps, R::Document, "COMPS", true},
      {ControlsSection::Grade, R::View, "GRADE", false},
      {ControlsSection::Brush, R::Simulation, "BRUSH", false},
      {ControlsSection::Pigment, R::Simulation, "PIGMENT", false},
      {ControlsSection::Medium, R::Simulation, "MEDIUM", false},
      {ControlsSection::BoardTilt, R::Simulation, "BOARD TILT", false},
      {ControlsSection::Grid, R::Simulation, "GRID", false},
      {ControlsSection::Solver, R::Simulation, "SOLVER", false},
  };
  return kSections;
}

const ControlsSectionSpec& controlsSectionSpec(ControlsSection section) {
  const std::vector<ControlsSectionSpec>& all = controlsSections();
  for (const ControlsSectionSpec& spec : all)
    if (spec.section == section) return spec;
  // Unreachable while every enumerator has an entry, which `--selftest`
  // asserts. Returning the first entry rather than dereferencing nothing keeps
  // a missing entry a wrong header rather than a crash.
  return all.front();
}

LabelledControlLayout layoutLabelledControl(float& column, float labelPx, float availPx) {
  // Grow first, and grow *now* rather than next frame: the invariant this
  // whole file exists for is that the widget never starts before the label
  // ends, and a column updated after the fact would violate it for exactly one
  // frame per new label -- which is one frame of a clipped word.
  column = std::max(column, labelPx + kControlsLabelGapPx);

  LabelledControlLayout out;
  const float remaining = availPx - column;
  if (remaining < kControlsMinWidgetPx) {
    // Too narrow for both. The label keeps its full width on its own line and
    // the widget takes everything -- still no clipping, which is the point.
    out.labelOnOwnLine = true;
    out.labelColumn = 0.0f;
    out.widgetWidth = std::max(availPx, 1.0f);
    return out;
  }
  out.labelColumn = column;
  out.widgetWidth = remaining;
  return out;
}

}  // namespace np
