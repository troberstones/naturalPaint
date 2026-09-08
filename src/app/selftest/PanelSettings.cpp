#include "app/selftest/Support.hpp"

#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Fonts.hpp"

namespace np {

// Track panelgear: a settings (gear) button on the panel grip, beside the
// "?" -- `ControlsSectionSpec::hasSettings` (app/ControlsLayout.hpp), the
// Lucide "settings" codepoint (`ui/AtelierChrome.hpp`'s
// `kSettingsIconCodepoint`), and ui/MacPaintUI.cpp's `panelGripFor()` /
// `drawPanelGrip()` / `drawSectionSettings()`. COLOR is the first section to
// declare settings: its Munsell page's steps slider and per-row/per-page
// chroma toggle moved out of the panel body -- where they used to share one
// cramped control row under the grid -- into the gear's popover, and the
// row's reserved height went back to the grid.
//
// `panelGripFor()` and `drawSectionSettings()` are file-local to
// ui/MacPaintUI.cpp -- no header declares either, the same situation
// app/selftest/ChromeConsistency.cpp describes for `atelierLayout()`'s
// arithmetic ("written out again independently here rather than obtained by
// calling the function and comparing it to itself"). Part C below follows
// that same discipline for `panelGripFor()`'s title-fit predicate: it
// re-derives the formula from the two named constants that formula and
// `drawPanelGrip()`'s button placement both read (`kPanelHelpBtnSize` = 16,
// `kPanelHelpBtnMargin` = 6, re-typed from ui/MacPaintUI.cpp -- if either
// ever moves, this section starts failing rather than silently checking a
// stale number), against a REAL measured title width: the same font and the
// same `pushAtelierMono()` call `panelGripFor()` itself uses, run through a
// real headless `ImGuiContext` the way app/selftest/Fonts.cpp's Part C/D and
// app/selftest/AtelierChrome.cpp's palette probe already establish is safe
// without a window or a renderer.
//
// **What this cannot prove headlessly**: that `panelGripFor()`'s actual body
// still reads these same two constants for both buttons rather than having
// drifted from them, or that a click on the drawn gear really does open
// `drawSectionSettings()` for the right section -- both are that one
// function's internals and its input handling, provable only by screenshot
// (tools/golden's COLOR/Munsell views, re-blessed for this track).
bool runPanelSettingsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Part A: COLOR declares settings, and no other section does ---------
  {
    size_t settingsCount = 0;
    bool onlyColor = true;
    for (const ControlsSectionSpec& spec : controlsSections()) {
      if (!spec.hasSettings) continue;
      ++settingsCount;
      if (spec.section != ControlsSection::Color) onlyColor = false;
    }
    check(settingsCount == 1 && onlyColor,
          "exactly one section (COLOR) declares hasSettings, by a scan of every section");
    check(controlsSectionSpec(ControlsSection::Color).hasSettings,
          "and the lookup agrees: controlsSectionSpec(Color).hasSettings is true");
  }

  // --- Part B: the gear's codepoint reaches the actual font the grip draws
  //             with -------------------------------------------------------
  {
    const std::vector<uint32_t>& merged = toolIconCodepoints();
    const bool listed =
        std::find(merged.begin(), merged.end(), kSettingsIconCodepoint) != merged.end();
    check(listed,
          "kSettingsIconCodepoint is in toolIconCodepoints() -- the list "
          "ui/Fonts.cpp's installToolIconFont() merges onto uiFonts().text");

    // The same "ask the REAL call site's question" discipline
    // app/selftest/Fonts.cpp's Part D uses for the tool palette: merging the
    // codepoint onto SOME font proves nothing if drawToolGlyph() -- which
    // reads uiFonts().text specifically -- cannot find it there. That gap
    // is exactly how the tool palette's own merge once silently landed on
    // the wrong face (ui/Fonts.cpp's installToolIconFont() comment on
    // `config.DstFont`).
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    const FontLoadResult textLoaded = installUiFonts(13.0f);
    const ToolIconLoadResult iconLoaded = installToolIconFont(merged);
    bool textFaceHasGear = false;
    if (textLoaded.ok && iconLoaded.ok && uiFonts().text != nullptr) {
      ImFontBaked* baked = uiFonts().text->GetFontBaked(kToolIconSizePx);
      textFaceHasGear =
          baked != nullptr &&
          baked->FindGlyphNoFallback(static_cast<ImWchar>(kSettingsIconCodepoint)) != nullptr;
    }
    check(textFaceHasGear,
          "uiFonts().text itself -- what drawToolGlyph() actually reads -- draws the gear "
          "codepoint, not just whatever installToolIconFont() merged onto");
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
  }

  // --- Part C: panelGripFor()'s title-fit predicate flips at the predicted
  //             width, for a section with BOTH a "?" and a gear ------------
  {
    // Re-typed from ui/MacPaintUI.cpp's kPanelHelpBtnSize/kPanelHelpBtnMargin
    // -- both the help button and the gear share this one size and margin,
    // so a section with BOTH reserves TWO button-widths, not one.
    constexpr float kBtnSize = 16.0f;
    constexpr float kBtnMargin = 6.0f;
    constexpr float kOneBtnReserve = kBtnSize + kBtnMargin;   // 22
    constexpr float kBothBtnReserve = 2.0f * kOneBtnReserve;  // 44

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    const FontLoadResult loaded = installUiFonts(13.0f);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 400.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;  // this probe has no business writing imgui.ini

    float titleW = -1.0f;
    if (loaded.ok) {
      // NewFrame(), not a bare PushFont(): PushFont() alone leaves
      // g.FontSizeBase at whatever an unstarted frame defaults it to (0),
      // which CalcTextSize() would read as a zero-size font -- the same
      // "a headless ImGuiContext is fine for layout questions" precedent
      // app/selftest/AtelierChrome.cpp's palette-width probe already
      // established, minus the child-window scrollbar machinery this
      // question does not need.
      ImGui::NewFrame();
      pushAtelierMono();
      titleW = ImGui::CalcTextSize(controlsSectionSpec(ControlsSection::Color).title).x;
      popAtelierMono();
      ImGui::EndFrame();
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);

    std::printf("    [measured] COLOR title width in the panel's mono face = %.1f px\n",
                static_cast<double>(titleW));
    check(loaded.ok && titleW > 0.0f,
          "the real mono face loaded and measured a nonzero COLOR title width");

    if (loaded.ok && titleW > 0.0f) {
      // panelGripFor()'s own formula: showTitle = slot.w >= 22 + titleW + 4
      // + helpReserve + settingsReserve. COLOR has both spec.helpText and
      // spec.hasSettings, so the reserve is the two-button sum.
      const float threshold = 22.0f + titleW + 4.0f + kBothBtnReserve;
      auto showTitleAt = [&](float slotW) { return slotW >= threshold; };
      check(!showTitleAt(threshold - 1.0f),
            "title-fit: 1 px under the predicted (two-button) threshold, the title does not "
            "fit");
      check(showTitleAt(threshold),
            "title-fit: at the predicted (two-button) threshold, the title fits -- it flips "
            "exactly there, not before or after");

      // And the reserve really is BOTH buttons, not one: a regression that
      // dropped the gear's own reserve from the sum (leaving only the help
      // button's) would predict a narrower threshold than COLOR actually
      // needs. Checked as an inequality between the two re-derived
      // thresholds rather than by calling panelGripFor() with only one flag
      // set, which this file cannot do without COLOR itself changing.
      const float oneButtonThreshold = 22.0f + titleW + 4.0f + kOneBtnReserve;
      check(oneButtonThreshold < threshold,
            "title-fit: the one-button threshold is strictly narrower than the two-button one "
            "a section with both a help button and a gear actually needs");
    }
  }

  std::printf("[selftest] panel settings %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
