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
       "constants (density, staining, granulation) together. RGB and MUNSELL "
       "both pick a colour directly -- Pigment layers still take it through "
       "an RGB->latent decomposition, but the physical constants are NOT "
       "derived from a colour, which has none; they stay whatever pigment was "
       "last selected in PIGMENT mode. Switch to PIGMENT to change them.\n\n"
       "MUNSELL is a hue page of the Munsell solid: rows are values, columns "
       "are chroma, quantized to an n-by-n grid. Munsell value is defined by "
       "the luminance factor and nothing else, so a ROW IS A CONSTANT-"
       "LUMINANCE SET -- moving the hue bar changes which hue you are on and "
       "not how light it looks, which the RGB square cannot do (its V is "
       "max(r,g,b), so its top edge runs from a yellow of luminance 0.928 to "
       "a blue of 0.072). Outlined cells are colours sRGB does not contain; "
       "they are left empty rather than clamped, because clamping a channel "
       "would change the luminance the row exists to hold. The ragged edge "
       "is the shape of the gamut at that hue, and it leans -- there is no "
       "light saturated blue and no dark saturated yellow. If a hue change "
       "makes your cell disappear, the selection walks LEFT along its row and "
       "never up or down: chroma is negotiable, luminance is not.\n\n"
       "The colour is scene-referred (T25a): an eyedropper pick off a "
       "highlight brighter than white keeps its real value, and the panel "
       "shows OVER RANGE when it holds one. Two routes cannot carry such a "
       "value and clamp it to 1.000 -- the swatch, because it is 8-bit, and "
       "PIGMENT mode, because paint cannot reflect more light than falls on "
       "it. RGB strokes, the bucket and the gradient all keep it. Dragging "
       "in the saturation/value square brings the colour back into range; "
       "the numeric row under it does not.",
       // hasSettings: the Munsell page's steps slider and per-row/per-page
       // chroma toggle live in the grip's gear popover, not under the grid
       // -- see ui/MacPaintUI.cpp's drawMunsellPage() and
       // drawSectionSettings().
       true},
      {ControlsSection::BrushLibrary, R::Tool, "BRUSH LIBRARY", false},
      {ControlsSection::Brush, R::Tool, "BRUSH EDITOR", false},
      {ControlsSection::FlatsTools, R::Tool, "FLATS TOOLS", false,
       "The flatting gestures of a Flats layer, as tools rather than as keys. Each one is\n"
       "STICKY: pick DELETE and every click deletes a fill until you pick something else,\n"
       "the way a brush stays picked. What each one records is a POINT or a PATH, never a\n"
       "region id -- so editing the line art re-flats the drawing and every repair you made\n"
       "replays against the fresh regions (ADR-0009).\n\n"
       "The buttons grey out when the active layer is not a Flats layer. The panel stays\n"
       "where you docked it rather than vanishing, so its place on screen is yours to keep."},
      // Last of the `Tool` roles, which is what keeps the role sequence
      // non-decreasing (app/selftest/ControlsLayout.cpp asserts it).
      {ControlsSection::Paths, R::Tool, "PATHS", false,
       "What you can do to a path once it exists: close it, open it, join two of them at\n"
       "their end points, reverse the direction it runs in, gather several into one\n"
       "compound path or release one back into its parts -- and, with anchors selected,\n"
       "smooth or corner or break a knot, insert one in the middle of a segment, or delete\n"
       "the ones you picked.\n\n"
       "EVERY BUTTON IS A LIVE READOUT. A verb is lit exactly when it would run against\n"
       "what is selected right now, so a lit button cannot refuse and a greyed one says\n"
       "why underneath. Anchor verbs need COMPONENT mode on the OPTIONS band; the shape\n"
       "verbs need SHAPE mode.\n\n"
       "MAKE turns the path into something else: a selection, a filled area, or a stroke\n"
       "laid down with the current brush. Those three paint into the nearest layer BELOW\n"
       "the path, because the path's own layer is the source.\n\n"
       "The buttons grey out when the active layer is not a Vector layer. The panel stays\n"
       "where you docked it rather than vanishing, so its place on screen is yours to keep."},
      {ControlsSection::Layers, R::Document, "LAYERS", true},
      {ControlsSection::History, R::Document, "HISTORY", true},
      {ControlsSection::Comps, R::Document, "COMPS", true},
      // docs/automation-plan.md step 7 / PRD P1, P5. A `Document` role beside
      // the three above it, and starting CLOSED for the same budget reason
      // they mostly do -- but note that its default PLACEMENT is the flyout
      // rail, not the right dock (app/PanelLayout.cpp's
      // `defaultPlacementFor()` carries that exception and its argument).
      {ControlsSection::Actions, R::Document, "ACTIONS", false,
       "Record what you do to a document as a list of COMMANDS, not of clicks -- so the\n"
       "same list replays on a different document whose layers differ in order and count.\n"
       "RECORD, do the work by hand, STOP; PLAY runs the list back as ONE history entry,\n"
       "so undo takes the whole action out in one stroke.\n\n"
       "A step is keyed by a stable command id and targets layers BY NAME, which is what\n"
       "makes an action outlive a menu rewording and a reordered layer stack.\n\n"
       "SAVE greys out while a take carries a REFUSAL. A recording with a hole in it is\n"
       "not a shorter recording: it replays confidently and does the wrong thing partway\n"
       "through. Every refusal names the command, the reason and the fix, and they are\n"
       "listed under the steps for as long as SAVE is grey.\n\n"
       "The recording stops itself if you close the document, switch to another one, or\n"
       "put this panel away -- a take that spans two documents is a sequence that never\n"
       "happened on either, and a recording nobody is looking at goes on collecting every\n"
       "command in the session."},
      // Last of the `Document` roles, for the same ordering reason.
      {ControlsSection::FlatsSegmentation, R::Document, "SEGMENTATION", false,
       "How the line art beneath a Flats layer is cut into fills. A Flats layer stores no\n"
       "pixels: it stores these parameters plus the repairs you made, and re-derives every\n"
       "fill from the drawing underneath. So moving a slider here re-flats the drawing.\n\n"
       "Fills are anchored to a PLACE, not to a number, which is why nudging a slider from\n"
       "417 fills to 381 leaves most of them wearing the colour they already had, and why\n"
       "putting the slider back reproduces all 417 exactly.\n\n"
       "SHEET, GAP and DECLUTTER also appear on the Paint Bucket's options row. They are the\n"
       "same three fields, not a copy -- both bind the active layer directly."},
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
