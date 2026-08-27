#include "app/selftest/Support.hpp"

#include "app/LayerEditor.hpp"
#include "core/Layer.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/Fonts.hpp"

namespace np {

// ui/Fonts -- the glyph coverage the layers panel depends on.
//
// **This section exists because nine other sections could not have caught the
// bug it guards.** Nine `--selftest` sections assert `layerKindGlyph()`
// returns the right glyph, and all nine pass whether or not that glyph can be
// drawn -- they check a return value, not a pixel. The gap was found by
// photographing the panel.
//
// So this section asserts the part that is actually load-bearing: that every
// codepoint the UI asks for is one the *loaded font can draw*. Part C does
// that against a real ImGui font atlas, which is why it is worth the context.
bool runFontsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. UTF-8 decode, the piece the glyph check is built on --\n");

  // Two of app/LayerPanel's own glyphs, plus the ASCII one, plus the middle
  // dot the sub-line uses -- decoded here rather than trusted, because every
  // assertion below is a claim about the numbers this function returns.
  const auto fisheye = decodeUtf8("\xE2\x97\x89");
  check(fisheye.size() == 1 && fisheye[0] == 0x25C9u, "3-byte U+25C9 fisheye decodes to one codepoint");
  const auto scissors = decodeUtf8("\xE2\x9C\x82");
  check(scissors.size() == 1 && scissors[0] == 0x2702u, "3-byte U+2702 scissors decodes to one codepoint");
  const auto ascii = decodeUtf8("T");
  check(ascii.size() == 1 && ascii[0] == 0x54u, "1-byte ASCII 'T' decodes to U+0054");
  const auto dot = decodeUtf8("\xC2\xB7");
  check(dot.size() == 1 && dot[0] == 0x00B7u, "2-byte U+00B7 middle dot decodes to one codepoint");

  // A display path must not hang or read past the end on bad input. A lone
  // continuation byte and a truncated 3-byte lead are the two shapes that
  // would; both must terminate and report the replacement character.
  const auto lone = decodeUtf8("\x80");
  check(lone.size() == 1 && lone[0] == 0xFFFDu, "a lone continuation byte yields U+FFFD, not a hang");
  const auto truncated = decodeUtf8("\xE2\x97");
  check(truncated.size() == 1 && truncated[0] == 0xFFFDu, "a truncated 3-byte sequence yields U+FFFD");

  std::printf("  -- B. every glyph the panel asks for is in the required set --\n");

  const std::vector<uint32_t>& required = requiredUiCodepoints();
  bool allListed = true;
  for (const LayerKind kind :
       {LayerKind::Pigment, LayerKind::RGB, LayerKind::Media, LayerKind::Strokes,
        LayerKind::Adjustment, LayerKind::Text, LayerKind::Flats, LayerKind::Group}) {
    for (const uint32_t cp : decodeUtf8(layerKindGlyph(kind))) {
      if (cp < 0x0100u) continue;  // ImGui's default range draws these already
      if (std::find(required.begin(), required.end(), cp) == required.end()) allListed = false;
    }
  }
  for (const LayerCommand command : allLayerCommands()) {
    for (const uint32_t cp : decodeUtf8(layerCommandGlyph(command))) {
      if (cp < 0x0100u) continue;
      if (std::find(required.begin(), required.end(), cp) == required.end()) allListed = false;
    }
  }
  check(allListed,
        "every above-U+00FF kind glyph and command icon is in requiredUiCodepoints()");
  // Six of the eight kinds need a merge source (Text's 'T' and Group's 'G'
  // are both ASCII -- `LayerKind::Group` is new since this comment first
  // said "seven kinds"; it deliberately reused Text's own trick rather than
  // spending a codepoint, see app/LayerPanel.cpp's own note). Nine
  // command icons are new codepoints -- the toolbar's twelve buttons draw
  // fifteen distinct glyphs, not twelve, because the three creation commands
  // deliberately reuse their kind's own glyph (`layerCommandGlyph()`) rather
  // than adding three more. The Layer Properties gear is the sixteenth --
  // ui/Fonts.cpp's one hand-added glyph, since it belongs to no table.
  check(required.size() == 16,
        "6 kind glyphs + 9 command icons + the Properties gear need a merge source");
  const auto gear = decodeUtf8("\xE2\x9A\x99");
  check(gear.size() == 1 && gear[0] == 0x2699u &&
            std::find(required.begin(), required.end(), 0x2699u) != required.end(),
        "the Layer Properties gear (U+2699) is in the required set");

  // The tripwire. `layerKindGlyph()` returns "?" for a kind it has no case
  // for, so casting one past the last real kind must still be "?" -- if a
  // kind is added, this stops being true and the list above stops being a
  // complete walk. Without this the walk silently covers eight of nine.
  //
  // **This literal moved once already** -- it used to read
  // `static_cast<LayerKind>(7)` and assert "exactly 7 values", back when
  // `LayerKind::Group` (ordinal 7) did not exist. A raw ordinal cast is
  // exactly the pattern this codebase warns against elsewhere ("never key
  // anything by enum ordinal") for data; it survives here, once, as the one
  // deliberate use of an ordinal *as* an ordinal -- probing "one past every
  // real value" -- and it had to move the moment a real value took the slot
  // it used to probe. There is no way to spell "one past the end" without a
  // number, so the tripwire is this comment plus this assertion, not the
  // literal alone.
  check(std::string(layerKindGlyph(static_cast<LayerKind>(8))) == "?",
        "LayerKind still has exactly 8 values, so the walk above is complete");

  // The three creation icons are their kind's own glyph, verbatim -- not a
  // second copy of it, so the toolbar button and the row it produces can
  // never drift to two different marks.
  check(std::string(layerCommandGlyph(LayerCommand::NewRgbLayer)) ==
            std::string(layerKindGlyph(LayerKind::RGB)),
        "the New RGB Layer icon is the RGB kind glyph, not a copy of it");
  check(std::string(layerCommandGlyph(LayerCommand::NewPigmentLayer)) ==
            std::string(layerKindGlyph(LayerKind::Pigment)),
        "the New Pigment Layer icon is the Pigment kind glyph");
  check(std::string(layerCommandGlyph(LayerCommand::NewAdjustmentLayer)) ==
            std::string(layerKindGlyph(LayerKind::Adjustment)),
        "the New Adjustment Layer icon is the Adjustment kind glyph");
  // CaptureComp is deliberately iconless (it belongs to COMPS, not the
  // per-row toolbar) -- asserted so a future icon added for it is a decision
  // rather than an accident that silently changes the required-codepoint
  // count above.
  check(std::string(layerCommandGlyph(LayerCommand::CaptureComp)).empty(),
        "CaptureComp has no toolbar icon");

  std::printf("  -- C. and the loaded font can actually draw them --\n");

  // The assertion the other nine sections are missing. A real atlas, the real
  // merge, and a per-codepoint question to the baked font. No GPU: baking is
  // CPU-side, and nothing here is uploaded.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  const FontLoadResult loaded = installUiFonts(13.0f);
  std::printf("    [measured] %s\n",
              loaded.ok ? ("merged " + loaded.path).c_str() : loaded.error.c_str());
  check(loaded.ok, "a glyph source loaded and covers every required codepoint");
  check(loaded.missing.empty(), "no required codepoint is left undrawable");

  // -- D. and the tool palette's Lucide merge draws too --------------------
  //
  // This section's own top comment says why: nine sections once asserted a
  // return value and never a pixel, and the bug that taught this suite the
  // difference would have sailed straight through a test that only checked
  // ui/AtelierChrome.cpp's `toolIconCodepoint()` table. installToolIconFont()
  // must run after installUiFonts() (it merges onto the Fonts[0] that call
  // creates), which is exactly the order this section already has them in.
  std::printf("  -- D. the tool palette's Lucide merge, checked the same way --\n");
  const ToolIconLoadResult iconLoaded = installToolIconFont(toolIconCodepoints());
  std::printf("    [measured] %s\n",
              iconLoaded.ok ? ("merged " + iconLoaded.path).c_str() : iconLoaded.error.c_str());
  check(iconLoaded.ok, "the vendored Lucide font loaded and covers every tool-icon codepoint");
  check(iconLoaded.missing.empty(), "no tool-icon codepoint is left undrawable");

  // The check above asks the pointer installToolIconFont() itself merged
  // onto -- which is exactly the check that let a real bug through once
  // (see ui/Fonts.cpp's installToolIconFont() comment on `config.DstFont`):
  // that merge briefly landed on the *mono* face rather than on
  // `uiFonts().text`, and asking the wrong-but-self-consistent pointer
  // whether it covered its own merge said "yes" while ui/MacPaintUI.cpp's
  // `drawToolGlyph()` -- which asks `uiFonts().text` specifically, because
  // that is the font every cell is actually drawn with -- got nothing and
  // silently fell back to hand-drawn vectors. So this asks the *real* call
  // site's question a second time, independently.
  bool textFaceHasIcons = true;
  if (uiFonts().text != nullptr) {
    ImFontBaked* textBaked = uiFonts().text->GetFontBaked(kToolIconSizePx);
    if (textBaked == nullptr) {
      textFaceHasIcons = false;
    } else {
      for (const uint32_t cp : toolIconCodepoints())
        if (textBaked->FindGlyphNoFallback(static_cast<ImWchar>(cp)) == nullptr)
          textFaceHasIcons = false;
    }
  } else {
    textFaceHasIcons = false;
  }
  check(textFaceHasIcons,
        "uiFonts().text itself -- what ui/MacPaintUI.cpp's drawToolGlyph() actually reads -- "
        "draws every tool-icon codepoint, not just whatever installToolIconFont() merged onto");

  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);

  std::printf("[selftest] fonts %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
