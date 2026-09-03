#include "app/ControlsLayout.hpp"

#include <algorithm>

namespace np {

const std::vector<ControlsSectionSpec>& controlsSections() {
  using R = ControlsSectionRole;
  // The order and the default-open set, in one place. See the header for why
  // COLOR leads and why the three Document sections follow it.
  static const std::vector<ControlsSectionSpec> kSections = {
      // The two former chrome bands, ahead of everything -- see the header's
      // `Tools`/`Options` note. Open by default because a collapsed tool
      // palette is an empty left edge, which is not a state a first run
      // should ever start in.
      {ControlsSection::Tools, R::Tool, "TOOLS", true},
      {ControlsSection::Options, R::Tool, "OPTIONS", true},
      {ControlsSection::Color, R::Tool, "COLOR", true,
       "PIGMENT mode picks a real pigment: its colour and its physical "
       "constants (density, staining, granulation) together. RGB mode picks "
       "a colour directly -- Pigment layers still take it through an "
       "RGB->latent decomposition, but the three constants shown below are "
       "NOT derived from an RGB triple, which has none; they stay whatever "
       "pigment was last selected in PIGMENT mode. Switch to PIGMENT to "
       "change them.\n\n"
       "The colour is scene-referred (T25a): an eyedropper pick off a "
       "highlight brighter than white keeps its real value, and the panel "
       "shows OVER RANGE when it holds one. Two routes cannot carry such a "
       "value and clamp it to 1.000 -- the swatch, because it is 8-bit, and "
       "PIGMENT mode, because paint cannot reflect more light than falls on "
       "it. RGB strokes, the bucket and the gradient all keep it. Dragging "
       "in the saturation/value square brings the colour back into range; "
       "the numeric row under it does not."},
      {ControlsSection::BrushLibrary, R::Tool, "BRUSH LIBRARY", false},
      {ControlsSection::Brush, R::Tool, "BRUSH EDITOR", false},
      {ControlsSection::Layers, R::Document, "LAYERS", true},
      {ControlsSection::History, R::Document, "HISTORY", true},
      {ControlsSection::Comps, R::Document, "COMPS", true},
      {ControlsSection::Grade, R::View, "GRADE", false},
      // View role, right beside GRADE for the same reason (see the header):
      // closed by default too, so a document that is merely open does not
      // pay this section's per-open recompute cost until someone asks for
      // it.
      {ControlsSection::Histogram, R::View, "HISTOGRAM", false},
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

std::string controlsSectionShortLabel(ControlsSection section,
                                      const std::vector<ControlsSection>& among) {
  const std::string title = controlsSectionSpec(section).title;
  // Two characters is the floor rather than one: a single letter reads as an
  // abbreviation of nothing, and every title in this build is at least four
  // characters long, so two is always available.
  const size_t lo = std::min<size_t>(2, title.size());
  const size_t hi = std::min(kSectionShortLabelMax, title.size());

  for (size_t n = lo; n <= hi; ++n) {
    const std::string candidate = title.substr(0, n);
    bool unique = true;
    for (const ControlsSection other : among) {
      if (other == section) continue;
      const std::string otherTitle = controlsSectionSpec(other).title;
      if (otherTitle.compare(0, n, candidate) == 0) unique = false;
    }
    if (unique) return candidate;
  }
  // No prefix that fits separates it -- see the header's honest limit. The
  // longest one is still the most informative thing to draw, and the caller's
  // tooltip carries the title.
  return title.substr(0, hi);
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

float controlsWheelScrollStep(float innerHeightPx, float fontSizePx) noexcept {
  // Degenerate inputs get ImGui's own answer rather than a zero step: a
  // column measured at zero height during a layout pass must not silently
  // become unscrollable.
  const float imguiStep = 5.0f * (fontSizePx > 0.0f ? fontSizePx : 13.0f);
  if (!(innerHeightPx > 0.0f)) return imguiStep;

  const float quarterPage = innerHeightPx * 0.25f;
  const float ceiling = innerHeightPx * 0.67f;  // imgui.cpp's own max_step
  // Order matters at very short heights, where the ceiling can fall BELOW
  // ImGui's step: the floor is applied last so "never slower than the default"
  // wins over "never more than two thirds". A window that short cannot show a
  // section anyway, and the default is the behaviour a user already expects.
  return std::max(std::min(std::max(quarterPage, imguiStep), ceiling), imguiStep);
}

}  // namespace np
