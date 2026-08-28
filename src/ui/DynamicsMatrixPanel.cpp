#include "ui/DynamicsMatrixPanel.hpp"

#include <cstdint>
#include <cstdio>

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Dynamics.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/LabelledControl.hpp"

namespace np {

// `drawCurveWidget()` is `ui/MacPaintUI.cpp`'s own -- defined there and still
// used by everything else in that file that was already using it (the GRADE
// section's curve op editor), given external linkage there specifically so
// this second translation unit can reach it. Declared again here rather than
// through a shared header because nothing else needs it outside those two
// files, and a header with one caller is a header this codebase's own
// convention (`app/CurveEdit.hpp`'s split from its ImGui glue) would not add.
// `ctlSlider()` itself now comes from `ui/LabelledControl.hpp` above --
// a later, unrelated move (D0) relocated it out of `MacPaintUI.cpp` into its
// own shared header, which is exactly the second caller this comment used to
// say didn't exist yet.
bool drawCurveWidget(Curve& curve, float plotSize = 200.0f);

namespace {

// One labelled row of the matrix's own geometry: a 54 px row label, twelve
// equal cells, a 34 px live-value gutter. Shared by the header row and the
// ten source rows so the columns cannot drift between them.
constexpr float kMatrixLabelW = 54.0f;
constexpr float kMatrixGutterW = 34.0f;
constexpr float kMatrixRowH = 19.0f;
constexpr float kMatrixHeadH = 16.0f;

// The two shades the matrix needs that the token table does not carry: the
// alternating column wash, and the dot marking a cell with no link. Local to
// the matrix because that is the only thing with columns to alternate.
constexpr uint32_t kMatrixColumnAlt = 0x333030;
constexpr uint32_t kMatrixEmptyDot = 0x4f4c4c;

}  // namespace

// The DYNAMICS matrix: every source against every target it could drive.
//
// **Drawing the whole space is the design's central claim, not a stylistic
// choice** -- "an empty cell is as informative as a filled one -- you can see
// that nothing drives spacing". A list of existing links would be smaller and
// would answer a different question. The cost the design names honestly is
// that twelve targets in 322 px means two-letter heads, which have to be
// learned; the full names are on the cells' tooltips.
void drawDynamicsMatrix(AppState& st) {
  const DynamicInputs live = dynamicInputsFor(st);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float cellsW = width - kMatrixLabelW - kMatrixGutterW;
  if (!(cellsW > 0.0f)) return;
  const float cellW = cellsW / static_cast<float>(kDynamicTargetCount);

  const ImU32 ruleCol = atelierToken(kDivider);
  const ImU32 textCol = atelierToken(kTextPrimary);
  const ImU32 mutedCol = atelierToken(kTextSecondary);
  const ImU32 accentCol = atelierToken(kAccent);
  const ImU32 altCol = atelierToken(kMatrixColumnAlt);

  pushAtelierMono();

  // --- header row: the two-letter target heads --------------------------
  {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    for (size_t t = 0; t < kDynamicTargetCount; ++t) {
      const float x0 = o.x + kMatrixLabelW + cellW * static_cast<float>(t);
      if (t % 2 == 1)
        dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(x0 + cellW, o.y + kMatrixHeadH), altCol);
      const DynamicTarget headTarget = static_cast<DynamicTarget>(t);
      const char* ab = targetAbbrev(headTarget);
      const ImVec2 sz = ImGui::CalcTextSize(ab);
      // A refused column's head reads in the muted colour too -- the same
      // "disabled rather than absent" treatment `newLayerKindMenuItem()`
      // gives a layer kind with nowhere to land, so a reader learns the
      // matrix has twelve columns even though one of them does nothing.
      const bool headBuildable = targetUnbuildableReason(headTarget) == nullptr;
      dl->AddText(ImVec2(x0 + (cellW - sz.x) * 0.5f, o.y + (kMatrixHeadH - sz.y) * 0.5f),
                  headBuildable ? textCol : mutedCol, ab);
    }
    dl->AddLine(ImVec2(o.x, o.y + kMatrixHeadH), ImVec2(o.x + width, o.y + kMatrixHeadH),
                ruleCol, 1.0f);
    ImGui::Dummy(ImVec2(width, kMatrixHeadH));
  }

  // --- one row per source -------------------------------------------------
  for (size_t s = 0; s < kDynamicSourceCount; ++s) {
    const DynamicSource source = static_cast<DynamicSource>(s);
    const ImVec2 o = ImGui::GetCursorScreenPos();

    dl->AddText(ImVec2(o.x + 5.0f, o.y + (kMatrixRowH - ImGui::GetFontSize()) * 0.5f), textCol,
                sourceName(source));
    dl->AddLine(ImVec2(o.x + kMatrixLabelW, o.y),
                ImVec2(o.x + kMatrixLabelW, o.y + kMatrixRowH), ruleCol, 1.0f);

    for (size_t t = 0; t < kDynamicTargetCount; ++t) {
      const DynamicTarget target = static_cast<DynamicTarget>(t);
      const float x0 = o.x + kMatrixLabelW + cellW * static_cast<float>(t);
      const ImVec2 cellMin(x0, o.y);
      const ImVec2 cellMax(x0 + cellW, o.y + kMatrixRowH);
      if (t % 2 == 1) dl->AddRectFilled(cellMin, cellMax, altCol);

      const size_t at = findLink(st.brush.links, source, target);
      const bool selected = st.brush.editSource == source && st.brush.editTarget == target;
      const ImVec2 mid((cellMin.x + cellMax.x) * 0.5f, (cellMin.y + cellMax.y) * 0.5f);
      // Per-CELL, not merely per-column (`cellUnbuildableReason()`,
      // brush/Dynamics.hpp): Wetness refuses its whole column, but Hue/
      // Saturation/Value refuse only the four cells a stroke-local source
      // would otherwise resolve to a silent constant. A refused cell draws
      // as neither filled nor an empty dot: it is not a decision the user
      // can make yet, so it should not look like one that simply has not
      // been made.
      const char* unbuildable = cellUnbuildableReason(source, target);

      if (unbuildable != nullptr) {
        dl->AddLine(ImVec2(mid.x - 3.0f, mid.y), ImVec2(mid.x + 3.0f, mid.y), mutedCol, 1.0f);
      } else if (at != kNoLink) {
        // A link that has been switched off keeps its curve but drives
        // nothing, so it draws as neither filled nor empty -- an outline.
        const float half = selected ? 5.5f : 4.5f;
        const ImVec2 a(mid.x - half, mid.y - half), b(mid.x + half, mid.y + half);
        if (st.brush.links.links[at].enabled) {
          dl->AddRectFilled(a, b, accentCol);
        } else {
          dl->AddRect(a, b, accentCol, 0.0f, 0, 1.0f);
        }
        if (selected) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), textCol,
                                  0.0f, 0, 1.0f);
      } else {
        dl->AddRectFilled(ImVec2(mid.x - 1.0f, mid.y - 1.0f), ImVec2(mid.x + 1.0f, mid.y + 1.0f),
                          atelierToken(kMatrixEmptyDot));
        if (selected)
          dl->AddRect(ImVec2(mid.x - 5.5f, mid.y - 5.5f), ImVec2(mid.x + 5.5f, mid.y + 5.5f),
                      mutedCol, 0.0f, 0, 1.0f);
      }

      // One hit target per cell. Clicking an empty cell selects it rather
      // than creating a link there -- creation is the editor's ADD button,
      // so that a stray click on a 17 px cell cannot silently change how the
      // brush paints. A refused cell is disabled outright -- the same
      // `BeginDisabled()` / `IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)`
      // pair `newLayerKindMenuItem()` uses, so the reason is still reachable
      // by hovering a cell that cannot be clicked.
      ImGui::SetCursorScreenPos(cellMin);
      char id[48];
      std::snprintf(id, sizeof id, "##cell%zu_%zu", s, t);
      ImGui::BeginDisabled(unbuildable != nullptr);
      ImGui::InvisibleButton(id, ImVec2(cellW, kMatrixRowH));
      ImGui::EndDisabled();
      if (unbuildable != nullptr) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          popAtelierMono();
          ImGui::SetTooltip("%s \xE2\x86\x92 %s\n%s", sourceName(source), targetName(target),
                            unbuildable);
          pushAtelierMono();
        }
      } else {
        if (ImGui::IsItemHovered()) {
          popAtelierMono();
          ImGui::SetTooltip("%s \xE2\x86\x92 %s%s", sourceName(source), targetName(target),
                            at == kNoLink ? "  (no link)" : "");
          pushAtelierMono();
        }
        if (ImGui::IsItemClicked()) {
          st.brush.editSource = source;
          st.brush.editTarget = target;
        }
      }
    }

    // The live gutter. Its whole job is that the matrix teaches what a source
    // IS while you paint, so it shows the value in the source's own unit --
    // degrees for the angular three -- not the normalised number the model
    // carries.
    char val[32];
    sourceDisplay(source, sourceValue(live, source), val, sizeof val);
    const ImVec2 vsz = ImGui::CalcTextSize(val);
    const float gx = o.x + width - kMatrixGutterW;
    dl->AddLine(ImVec2(gx, o.y), ImVec2(gx, o.y + kMatrixRowH), ruleCol, 1.0f);
    // RANDOM genuinely is redrawn per dab now (`dynamicRandomDraw()`,
    // `app/StrokeSession`'s deposit loop), so it has no value between dabs;
    // its em dash is drawn muted for the same reason. VELOCITY, FADE, NOISE,
    // DIRECTION and INITIAL DIRECTION are also stroke-local -- `live` here is
    // `dynamicInputsFor()`'s per-FRAME hardware sample, which cannot see a
    // dab in progress, so this gutter shows their truthful idle reading (0.0,
    // "not moving" / "just started" / "at rest" / "no heading yet" / "no
    // heading LATCHED yet") rather than what the stroke is doing right now.
    dl->AddText(ImVec2(o.x + width - 5.0f - vsz.x, o.y + (kMatrixRowH - vsz.y) * 0.5f),
                source == DynamicSource::Random ? mutedCol : textCol, val);

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(width, kMatrixRowH));
    dl->AddLine(ImVec2(o.x, o.y + kMatrixRowH), ImVec2(o.x + width, o.y + kMatrixRowH), ruleCol,
                1.0f);
  }
  popAtelierMono();

  if (!st.penSeen)
    ImGui::TextDisabled("No tablet: tilt, azimuth and barrel read as rest.");
}

// The LINK editor: one cell's response curve, its range, and what it is
// resolving to right now.
void drawLinkEditor(AppState& st) {
  const DynamicSource source = st.brush.editSource;
  const DynamicTarget target = st.brush.editTarget;
  const size_t at = findLink(st.brush.links, source, target);

  pushAtelierMono();
  ImGui::Text("%s \xE2\x86\x92 %s", sourceName(source), targetName(target));
  popAtelierMono();

  // The same per-CELL refusal the matrix's own cells already enforce by
  // disabling the click that would get here -- restated rather than
  // trusted, because a link loaded from an older preset file (or a future
  // importer) could name a refused cell directly, without ever going
  // through a matrix click.
  if (const char* unbuildable = cellUnbuildableReason(source, target)) {
    ImGui::TextDisabled("%s", unbuildable);
    return;
  }

  if (at == kNoLink) {
    ImGui::TextDisabled("No link in this cell.");
    if (ImGui::Button("Add link")) {
      BrushLink made;
      made.source = source;
      made.target = target;
      targetDefaultRange(target, made.rangeLo, made.rangeHi);
      addLink(st.brush.links, made);
    }
    return;
  }

  // Hue/Saturation/Value shift the deposited colour only -- brush/
  // Dynamics.hpp's own section comment on `applyHsvDynamics()` is the full
  // argument; this is that same caveat surfaced where the one person who
  // needs it, someone wiring up exactly this link, can actually see it.
  if (target == DynamicTarget::Hue || target == DynamicTarget::Saturation ||
      target == DynamicTarget::Value) {
    ImGui::TextDisabled(
        "Shifts the deposited colour only. Density, staining and granulation stay the "
        "swatch's own.");
  }

  BrushLink& link = st.brush.links.links[at];
  const DynamicInputs live = dynamicInputsFor(st);
  const float in = sourceValue(live, source);

  ImGui::Checkbox("On", &link.enabled);
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    removeLink(st.brush.links, source, target);
    return;  // `link` is dangling from here on.
  }

  // The design's 104 px plot, and the grading stack's own widget rather than
  // a second one -- see drawCurveWidget()'s comment.
  drawCurveWidget(link.curve, 104.0f);

  // IN is the source's live value; OUT is what the link resolves to. Showing
  // both is what makes the curve legible while painting: the design rides the
  // live value along the curve as the pen moves.
  const float out = link.enabled ? linkContribution(link, in) : targetIdentity(target);
  pushAtelierMono();
  ImGui::Text("IN  %.2f", in);
  ImGui::Text("OUT %.3f", out);
  popAtelierMono();

  // RANGE bounds the OUTPUT (brush/Dynamics.hpp): at curve 0 the link resolves
  // to lo, at 1 to hi. On PRESSURE -> SIZE at 0.10-1.00 that is a floor on
  // size, not a deadzone on pressure.
  float lo = link.rangeLo, hi = link.rangeHi;
  const bool angular = targetCombine(target) == TargetCombine::Add;
  const float sliderLo = angular ? -360.0f : 0.0f;
  const float sliderHi = angular ? 360.0f : 2.0f;
  if (ctlSlider("Range lo", &lo, sliderLo, sliderHi)) link.rangeLo = lo;
  if (ctlSlider("Range hi", &hi, sliderLo, sliderHi)) link.rangeHi = hi;
  ImGui::Checkbox("Invert", &link.invert);

  // The easing chips. They set the SAME Curve the widget edits, so a chip and
  // a hand-drawn curve cannot disagree -- and a curve dragged away from all of
  // them lights none of them, which is the honest answer.
  //
  // **All five presets are here, including LogTaper and PowerIn.** A preset
  // `easingCurve()` can build but no chip can select is reachable only from
  // code, which is this codebase's own silent-no-op class (docs/reachability-
  // audit.md): the feature exists, nothing in the application can turn it on,
  // and nothing says so. The two paper-derived laws are the ones most worth
  // reaching -- they are the shapes a real medium has, not shapes that were
  // eyeballed -- so they get chips on the same row-break rule as the rest.
  //
  // The list itself is `brush/Dynamics.hpp`'s `easingPresetCount()`/`At()`,
  // not a literal here: a hand-written array in this file is what let two
  // presets ship with no chip. Only the TOOLTIP text is the panel's -- and it
  // is a switch with no `default:`, so a sixth preset cannot reach the row
  // without someone writing its explanation.
  auto chipTooltip = [](EasingPreset preset) -> const char* {
    switch (preset) {
      case EasingPreset::Linear:
        return "Straight through: OUT tracks IN with no shaping.";
      case EasingPreset::EaseOut:
        return "Quick at first, flattening toward the top.";
      case EasingPreset::SCurve:
        return "Slow at both ends, fast through the middle.";
      case EasingPreset::LogTaper:
        return "log(1+9p)/log(10) -- PaintCopilot's radius law.\n"
               "Most of the growth in the first light third of\n"
               "the range. On PRESSURE -> SIZE this is how a real\n"
               "brush head spreads: it fattens fast off the paper,\n"
               "then resists.";
      case EasingPreset::PowerIn:
        return "p^2.5 -- PaintCopilot's opacity law, the mirror\n"
               "shape. Barely builds at low IN, most of the gain\n"
               "in the top third. On PRESSURE -> FLOW that is a\n"
               "long, honest light range before the mark goes solid.";
    }
    return "";
  };
  // Three per row: five chips do not fit the panel's width, and letting them
  // run off the edge would hide the last ones behind a clip rather than
  // showing them -- which would be the same unreachability wearing a chip.
  for (size_t i = 0; i < easingPresetCount(); ++i) {
    if (i > 0 && (i % 3) != 0) ImGui::SameLine();
    const EasingPreset preset = easingPresetAt(i);
    const bool active = matchesPreset(link.curve, preset);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, atelierToken(kAccent));
    if (ImGui::Button(easingPresetName(preset))) link.curve = easingCurve(preset);
    if (active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", chipTooltip(preset));
  }
}

}  // namespace np
