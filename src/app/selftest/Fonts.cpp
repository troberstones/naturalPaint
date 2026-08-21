#include "app/selftest/Support.hpp"

#include "core/Layer.hpp"
#include "imgui.h"
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
        LayerKind::Adjustment, LayerKind::Text, LayerKind::Flats}) {
    for (const uint32_t cp : decodeUtf8(layerKindGlyph(kind))) {
      if (cp < 0x0100u) continue;  // ImGui's default range draws these already
      if (std::find(required.begin(), required.end(), cp) == required.end()) allListed = false;
    }
  }
  check(allListed, "every above-U+00FF kind glyph is in requiredUiCodepoints()");
  check(required.size() == 6,
        "six of the seven kinds need a merge source (Text's 'T' is ASCII)");

  // The tripwire. `layerKindGlyph()` returns "?" for a kind it has no case
  // for, so casting one past the last real kind must still be "?" -- if a
  // kind is added, this stops being true and the list above stops being a
  // complete walk. Without this the walk silently covers seven of eight.
  check(std::string(layerKindGlyph(static_cast<LayerKind>(7))) == "?",
        "LayerKind still has exactly 7 values, so the walk above is complete");

  std::printf("  -- C. and the loaded font can actually draw them --\n");

  // The assertion the other nine sections are missing. A real atlas, the real
  // merge, and a per-codepoint question to the baked font. No GPU: baking is
  // CPU-side, and nothing here is uploaded.
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  const FontLoadResult loaded = installUiGlyphFont(13.0f);
  std::printf("    [measured] %s\n",
              loaded.ok ? ("merged " + loaded.path).c_str() : loaded.error.c_str());
  check(loaded.ok, "a glyph source loaded and covers every required codepoint");
  check(loaded.missing.empty(), "no required codepoint is left undrawable");
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);

  std::printf("[selftest] fonts %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
