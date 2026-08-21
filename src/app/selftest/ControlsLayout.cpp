#include "app/selftest/Support.hpp"

namespace np {

// UI detour step 3, problems 1 and 1b: the right-hand controls column's order,
// its default-open set, and the label column that stops a slider's name being
// clipped by the panel edge. app/ControlsLayout.hpp carries the argument for
// all three; this is the check that the code still obeys it.
//
// Nothing here draws anything, and it deliberately does not pretend to: what
// a panel *looks* like is a screenshot's job. What it can check is every
// decision the panel makes before it draws -- which is where all three of this
// step's layout bugs lived.
bool runControlsLayoutTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif
  // PLAN.md §1.5: an unexercised build option is not a seam. There is no
  // `#ifdef` around a single assertion in this section and nothing for one to
  // guard: app/ControlsLayout is a list of sections and one arithmetic
  // function, so every answer below is the same in both configurations. Said
  // in the output rather than assumed, so the claim is checkable from a log.
  std::printf("[selftest] controls layout: NP_USE_OIIO=%s -- every assertion in this section has "
              "the same correct answer in both configurations; nothing here reaches a file, an "
              "encoder, the GPU or ImGui\n",
              kOiioBuild ? "ON" : "OFF");

  // --- Part A: the order and the default-open set --------------------------
  //
  // The bug this fixes, stated as numbers rather than as a complaint: before
  // this step the column ran BRUSH, PIGMENT, WATER, BOARD TILT, GRID, SOLVER,
  // LAYERS, HISTORY, GRADE, with no headers and nothing collapsible, so the two
  // panels that edit the open document were the seventh and eighth things in a
  // single scroll and were off the bottom of the window at the default size.
  const std::vector<ControlsSectionSpec>& sections = controlsSections();
  const std::vector<ControlsSection> kOldOrder = {
      ControlsSection::Brush,  ControlsSection::Pigment, ControlsSection::Medium,
      ControlsSection::BoardTilt, ControlsSection::Grid, ControlsSection::Solver,
      ControlsSection::Layers, ControlsSection::History, ControlsSection::Grade};

  auto positionIn = [](const std::vector<ControlsSection>& order, ControlsSection s) {
    for (size_t i = 0; i < order.size(); ++i)
      if (order[i] == s) return i;
    return order.size();
  };
  std::vector<ControlsSection> newOrder;
  for (const ControlsSectionSpec& spec : sections) newOrder.push_back(spec.section);

  std::printf("  LAYERS was section %zu of %zu, is now section %zu of %zu\n",
              positionIn(kOldOrder, ControlsSection::Layers) + 1, kOldOrder.size(),
              positionIn(newOrder, ControlsSection::Layers) + 1, newOrder.size());
  std::printf("  HISTORY was section %zu of %zu, is now section %zu of %zu\n",
              positionIn(kOldOrder, ControlsSection::History) + 1, kOldOrder.size(),
              positionIn(newOrder, ControlsSection::History) + 1, newOrder.size());

  check(sections.size() == 10, "every section has exactly one spec (10)");
  check(newOrder.size() == kOldOrder.size() + 1,
        "the same sections plus COMPS, reordered -- none was dropped");
  {
    // Every enumerator appears exactly once. Written against the list of
    // enumerators rather than against a count, so a section added to the enum
    // and forgotten in the list fails here rather than being invisible.
    const ControlsSection kAll[] = {
        ControlsSection::Layers, ControlsSection::History,   ControlsSection::Comps,
        ControlsSection::Grade,
        ControlsSection::Brush,  ControlsSection::Pigment,   ControlsSection::Medium,
        ControlsSection::BoardTilt, ControlsSection::Grid,   ControlsSection::Solver};
    bool eachOnce = true;
    for (const ControlsSection s : kAll) {
      size_t seen = 0;
      for (const ControlsSectionSpec& spec : sections)
        if (spec.section == s) ++seen;
      if (seen != 1) eachOnce = false;
      // And the lookup agrees with the list, which is what the draw loop and
      // the header text depend on being the same thing.
      if (controlsSectionSpec(s).section != s) eachOnce = false;
    }
    check(eachOnce, "each enumerator appears once, and the lookup agrees");
  }

  {
    bool titlesOk = true;
    for (size_t i = 0; i < sections.size(); ++i) {
      const std::string title = sections[i].title;
      if (title.empty()) titlesOk = false;
      for (const char c : title)
        if (c >= 'a' && c <= 'z') titlesOk = false;  // the column's caps idiom
      for (size_t j = i + 1; j < sections.size(); ++j)
        if (title == sections[j].title) titlesOk = false;
    }
    check(titlesOk, "titles are non-empty, unique and upper case");
  }

  {
    // The rule: Document before View before Simulation. Asserted as an
    // ordering over roles rather than as a fixed list of titles, so inserting
    // a new simulation section cannot bury LAYERS and still pass.
    auto rank = [](ControlsSectionRole role) {
      switch (role) {
        case ControlsSectionRole::Document:   return 0;
        case ControlsSectionRole::View:       return 1;
        case ControlsSectionRole::Simulation: return 2;
      }
      return 3;
    };
    bool nonDecreasing = true;
    for (size_t i = 1; i < sections.size(); ++i)
      if (rank(sections[i].role) < rank(sections[i - 1].role)) nonDecreasing = false;
    check(nonDecreasing, "document sections precede view, view precedes simulation");
    check(sections.front().section == ControlsSection::Layers, "LAYERS is the first section");
    check(sections[1].section == ControlsSection::History, "HISTORY is the second section");
  }

  {
    // The default-open set is exactly the document sections. This is the
    // decision the step was asked to make deliberately, so it is asserted in
    // both directions: no document section closed, no other section open.
    bool exact = true;
    size_t open = 0;
    for (const ControlsSectionSpec& spec : sections) {
      const bool shouldBeOpen = spec.role == ControlsSectionRole::Document;
      if (spec.defaultOpen != shouldBeOpen) exact = false;
      if (spec.defaultOpen) ++open;
    }
    check(exact && open == 3,
          "exactly the document sections start open (LAYERS, HISTORY, COMPS)");
  }

  // --- Part B: the label column -------------------------------------------
  //
  // The measured input, and the only one this section needs from the running
  // application: the app prints its label column every time it changes, and on
  // this machine it prints
  //
  //   [controls] label column 129 px -- widest label "Capillary diffuse" at
  //   119 px, panel 300 px, so a slider gets 159 px
  //
  // 119 px for 17 characters is 7.0 px per character exactly, which is what a
  // fixed-width bitmap font at this scale should be -- ImGui's built-in
  // ProggyClean, which is the only font this project loads. Every width below
  // is derived from that one measurement, so this test is pinned to a number
  // the application prints rather than to a guess about a font.
  constexpr float kCharPx = 7.0f;
  constexpr float kAvailPx = 288.0f;  // 300 px panel, less padding and scrollbar
  auto labelPx = [](const char* s) { return kCharPx * static_cast<float>(std::strlen(s)); };
  check(std::fabs(labelPx("Capillary diffuse") - 119.0f) < 0.001f,
        "7.0 px/char reproduces the app's measured 119 px");

  // Every label the controls column draws through ctlSlider()/ctlSliderInt().
  // Listed here because a *test* may hold a copy of what a UI draws; the panel
  // itself must not, which is why the running measurement is self-tuning.
  const std::vector<const char*> kLabels = {
      "Load", "Water", "Hardness", "Density", "Staining", "Granulation", "Diffusion",
      "Viscosity", "Drag", "Edge darkening", "Paper slope", "Working time", "Max film",
      "Capillary diffuse", "Spacing", "Subdivisions", "Jacobi iters", "Substeps",
      "Brush load", "Pressure", "Squish", "Transfer", "Max transfer", "Levelling",
      "Impasto light", "Adhesion", "Relaxation", "Blocking", "Grain block", "Glue",
      "Receptivity", "Settle rate", "Lattice steps", "Evaporation",
      "Opacity", "Blend", "Name", "Stops", "Scale", "Black in", "White in", "Gamma",
      "Black out", "White out"};

  {
    // The invariant, over the real label set in the order the column draws it:
    // the widget never starts before the label ends. Checked per label, not
    // once at the end, because the column grows *during* the frame and the
    // whole point is that the growing frame is safe too.
    float column = 0.0f;
    bool neverOverlaps = true;
    bool everOnOwnLine = false;
    for (const char* label : kLabels) {
      const float px = labelPx(label);
      const LabelledControlLayout lay = layoutLabelledControl(column, px, kAvailPx);
      if (lay.labelOnOwnLine) {
        everOnOwnLine = true;
      } else if (lay.labelColumn < px) {
        neverOverlaps = false;
      }
      if (lay.widgetWidth <= 0.0f) neverOverlaps = false;
    }
    check(neverOverlaps, "no label is overlapped by its widget, at any point in the frame");
    check(!everOnOwnLine, "at 300 px every label fits beside its widget");
    check(std::fabs(column - (labelPx("Capillary diffuse") + kControlsLabelGapPx)) < 0.001f,
          "the settled column is the widest label plus the gap (129 px)");
    std::printf("  settled label column %.0f px, slider %.0f px, over %zu labels\n", column,
                kAvailPx - column, kLabels.size());
  }

  {
    // Order independence: the widest label last, and first, must settle the
    // same column. A column that depended on draw order would clip whichever
    // section a user happened to open first.
    float forward = 0.0f;
    for (const char* label : kLabels) layoutLabelledControl(forward, labelPx(label), kAvailPx);
    float backward = 0.0f;
    for (size_t i = kLabels.size(); i > 0; --i)
      layoutLabelledControl(backward, labelPx(kLabels[i - 1]), kAvailPx);
    check(std::fabs(forward - backward) < 0.001f, "the settled column does not depend on order");
  }

  {
    // The column never shrinks, which is what makes the invariant hold for a
    // section opened later in the session.
    float column = 0.0f;
    layoutLabelledControl(column, labelPx("Capillary diffuse"), kAvailPx);
    const float wide = column;
    layoutLabelledControl(column, labelPx("Drag"), kAvailPx);
    check(std::fabs(column - wide) < 0.001f, "a narrow label after a wide one does not shrink it");
  }

  {
    // The floor, and the one case where a label is not in the column at all.
    // A 140 px panel cannot hold "Capillary diffuse" and a usable slider side
    // by side, so the label goes above the widget -- still never clipped,
    // which is the property, rather than "always beside", which is not.
    float column = 0.0f;
    const LabelledControlLayout lay =
        layoutLabelledControl(column, labelPx("Capillary diffuse"), 140.0f);
    check(lay.labelOnOwnLine && std::fabs(lay.widgetWidth - 140.0f) < 0.001f,
          "too narrow to hold both: the label takes its own line");
    float column2 = 0.0f;
    const LabelledControlLayout wide =
        layoutLabelledControl(column2, labelPx("Drag"), kControlsMinWidgetPx + 40.0f);
    check(!wide.labelOnOwnLine && wide.widgetWidth >= kControlsMinWidgetPx,
          "a widget beside its label is never narrower than the floor");
  }

  {
    // --- The rejected alternative, run beside the built one ---------------
    //
    // What the column did before this step: leave the label where Dear ImGui
    // puts it, to the *right* of a widget of the default width, which is
    // `trunc(window width * 0.65)`. The label then gets whatever is left, and
    // anything longer than that is clipped by the window edge mid-word.
    //
    // **The geometry below is a reconstruction, not a measurement**: the old
    // panel is gone, and ImGui's default item width is read from its
    // documented rule rather than from this build. What it is used for is the
    // comparison, and the comparison's answer -- that the old scheme clips
    // several of these labels at either panel width and the new one clips none
    // -- is not sensitive to a few pixels either way.
    auto oldSchemeLabelSpace = [](float panelPx) {
      const float widget = std::trunc(panelPx * 0.65f);   // ImGui's ItemWidthDefault
      const float padding = 8.0f;                         // WindowPadding, one side
      const float innerSpacing = 4.0f;                    // ItemInnerSpacing
      return panelPx - padding - widget - innerSpacing;
    };
    for (const float panelPx : {268.0f, 300.0f}) {
      const float space = oldSchemeLabelSpace(panelPx);
      size_t clippedOld = 0;
      for (const char* label : kLabels)
        if (labelPx(label) > space) ++clippedOld;
      float column = 0.0f;
      size_t clippedNew = 0;
      for (const char* label : kLabels) {
        const LabelledControlLayout lay =
            layoutLabelledControl(column, labelPx(label), panelPx - 12.0f);
        if (!lay.labelOnOwnLine && lay.labelColumn < labelPx(label)) ++clippedNew;
      }
      std::printf("  panel %.0f px: label-on-the-right leaves %.0f px and clips %zu of %zu "
                  "labels; label column clips %zu\n",
                  panelPx, space, clippedOld, kLabels.size(), clippedNew);
      check(clippedNew == 0, "the built scheme clips nothing at this panel width");
      check(clippedOld > 0, "the rejected scheme clips at least one label at this panel width");
    }
  }

  std::printf("[selftest] controls layout %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
