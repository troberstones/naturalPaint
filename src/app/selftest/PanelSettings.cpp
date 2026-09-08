#include "app/selftest/Support.hpp"

#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Fonts.hpp"
#include "ui/PanelGrip.hpp"

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
// `drawSectionSettings()` is still file-local to ui/MacPaintUI.cpp -- no
// header declares it, the same situation app/selftest/ChromeConsistency.cpp
// describes for `atelierLayout()`'s arithmetic. `panelGripFor()` is not: it
// is declared in ui/PanelGrip.hpp precisely so Part C below can call the REAL
// function rather than re-deriving its formula. An earlier revision of this
// section re-typed `kPanelHelpBtnSize`/`kPanelHelpBtnMargin` and re-wrote the
// `showTitle` predicate by hand -- which meant a change to
// `panelGripFor()`'s own arithmetic (e.g. `settingsReserve` silently dropped
// to zero) left the re-derivation, and this section, unable to see it: the
// sabotage that finds that exact defect left `--selftest` green. Part C now
// builds three `ControlsSectionSpec`s (no help/no settings, settings only,
// settings+help -- the middle one has no real section, since COLOR is the
// only section with `hasSettings` and it also carries `helpText`) and calls
// `panelGripFor()` itself at slot widths bracketing an independently
// predicted threshold, checking `showTitle` flips exactly there. The
// predicted threshold still needs a REAL measured title width -- the same
// font and the same `pushAtelierMono()` call `panelGripFor()` itself uses,
// run through a headless `ImGuiContext` the way app/selftest/Fonts.cpp's
// Part C/D and app/selftest/AtelierChrome.cpp's palette probe already
// establish is safe without a window or a renderer -- but the reserve math
// and the fit predicate are now the real function's, not a copy of them.
//
// **What this cannot prove headlessly**: that a click on the drawn gear
// really does open `drawSectionSettings()` for the right section, or that
// `drawPanelGrip()`'s own button placement still matches the reserve
// `panelGripFor()` computes -- both are that function's input handling and
// drawing, provable only by screenshot (tools/golden's COLOR/Munsell views,
// re-blessed for this track).
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

  // --- Part C: panelGripFor() ITSELF -- not a re-derivation -- flips
  //             showTitle at the predicted width, for three specs bracketing
  //             every helpText/hasSettings combination that matters --------
  {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    const FontLoadResult loaded = installUiFonts(13.0f);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 400.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;  // this probe has no business writing imgui.ini

    // One title, shared by all three specs below, so the same measured width
    // predicts all three thresholds -- panelGripFor() itself reads
    // spec.title, so what it measures is this exact string too.
    static constexpr const char* kProbeTitle = "PROBE SECTION";

    float titleW = -1.0f;
    // Every check below that touches ImGui text measurement -- the probe
    // title AND the three panelGripFor() calls, since panelGripFor() itself
    // calls CalcTextSize() -- runs inside this one NewFrame()/EndFrame()
    // bracket. NewFrame(), not a bare PushFont(): PushFont() alone leaves
    // g.FontSizeBase at whatever an unstarted frame defaults it to (0),
    // which CalcTextSize() would read as a zero-size font -- the same
    // "a headless ImGuiContext is fine for layout questions" precedent
    // app/selftest/AtelierChrome.cpp's palette-width probe already
    // established, minus the child-window scrollbar machinery this question
    // does not need.
    if (loaded.ok) {
      ImGui::NewFrame();
      pushAtelierMono();
      titleW = ImGui::CalcTextSize(kProbeTitle).x;
      popAtelierMono();

      std::printf("    [measured] probe title width in the panel's mono face = %.1f px\n",
                  static_cast<double>(titleW));
      check(titleW > 0.0f, "the real mono face loaded and measured a nonzero probe title width");

      if (titleW > 0.0f) {
        // ui/PanelGrip.hpp's real constants -- not re-typed -- so a change to
        // either one moves this section's predicted threshold along with
        // panelGripFor()'s own.
        constexpr float kOneBtnReserve = kPanelHelpBtnSize + kPanelHelpBtnMargin;  // 22

        // Three specs, one shared title, differing only in helpText/
        // hasSettings -- section/role/defaultOpen play no part in
        // panelGripFor()'s title-fit arithmetic, so an arbitrary real
        // section (Color) fills that slot.
        ControlsSectionSpec specNeither{ControlsSection::Color, ControlsSectionRole::Tool,
                                         kProbeTitle, true};
        ControlsSectionSpec specSettingsOnly = specNeither;
        specSettingsOnly.hasSettings = true;  // helpText stays null -- no real section is this
        ControlsSectionSpec specBoth = specSettingsOnly;
        specBoth.helpText = "probe help text";

        // Calls the REAL panelGripFor() at slot widths bracketing the
        // predicted threshold and checks showTitle flips exactly there --
        // this is what makes the check see a change to panelGripFor()'s own
        // reserve arithmetic, where the old re-derivation could not.
        auto assertFlip = [&](const ControlsSectionSpec& spec, float reserve, const char* label) {
          const float threshold = 22.0f + titleW + 4.0f + reserve;
          const AtelierRect below{0.0f, 0.0f, threshold - 1.0f, 200.0f};
          const AtelierRect at{0.0f, 0.0f, threshold, 200.0f};
          const bool showsBelow = panelGripFor(spec, below, /*collapsed=*/false).showTitle;
          const bool showsAt = panelGripFor(spec, at, /*collapsed=*/false).showTitle;
          check(!showsBelow, (std::string("title-fit (") + label +
                               "): 1 px under the predicted threshold, the REAL panelGripFor() "
                               "does not show the title")
                                  .c_str());
          check(showsAt, (std::string("title-fit (") + label +
                          "): at the predicted threshold, the REAL panelGripFor() shows the "
                          "title -- it flips exactly there, not before or after")
                             .c_str());
        };

        assertFlip(specNeither, 0.0f, "no help, no settings");
        assertFlip(specSettingsOnly, kOneBtnReserve, "settings only, no help");
        assertFlip(specBoth, 2.0f * kOneBtnReserve, "settings + help");
      }

      ImGui::EndFrame();
    } else {
      check(false, "the real mono face loaded and measured a nonzero probe title width");
    }

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
  }

  std::printf("[selftest] panel settings %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
