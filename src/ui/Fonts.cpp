#include "ui/Fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "app/LayerPanel.hpp"
#include "core/Layer.hpp"
#include "imgui.h"

namespace np {
namespace {

// Candidates in preference order, with the coverage measured for each (see
// the header). The list is short on purpose: a long fallback chain whose
// later entries have never been looked at is a list of guesses.
struct GlyphFontCandidate {
  const char* path;
  const char* why;
};

constexpr GlyphFontCandidate kCandidates[] = {
    {"/System/Library/Fonts/Menlo.ttc", "covers all 7 required codepoints (measured)"},
    {"/System/Library/Fonts/Apple Symbols.ttf",
     "covers 6 of 7 -- no U+2702 scissors, which is reported rather than drawn as a box"},
};

}  // namespace

const std::vector<uint32_t>& requiredUiCodepoints() {
  // Built from `layerKindGlyph()` itself rather than retyped, so the two can
  // never drift: the list IS what the panel asks for. Every kind is walked,
  // including any added later, which is what makes the drift impossible
  // rather than merely unlikely.
  static const std::vector<uint32_t> kPoints = [] {
    std::vector<uint32_t> points;
    for (const LayerKind kind :
         {LayerKind::Pigment, LayerKind::RGB, LayerKind::Media, LayerKind::Strokes,
          LayerKind::Adjustment, LayerKind::Text, LayerKind::Flats}) {
      for (const uint32_t cp : decodeUtf8(layerKindGlyph(kind))) {
        // Below 0x0100 is ImGui's own default range and needs no merge
        // source; including it would ask a second font for glyphs the first
        // already draws, which is exactly what GlyphExcludeRanges exists to
        // prevent.
        if (cp >= 0x0100 && std::find(points.begin(), points.end(), cp) == points.end())
          points.push_back(cp);
      }
    }
    std::sort(points.begin(), points.end());
    return points;
  }();
  return kPoints;
}

std::vector<uint32_t> decodeUtf8(std::string_view utf8) {
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < utf8.size()) {
    const auto byte = static_cast<unsigned char>(utf8[i]);
    size_t length = 0;
    uint32_t cp = 0;
    if (byte < 0x80) {
      length = 1;
      cp = byte;
    } else if ((byte & 0xE0) == 0xC0) {
      length = 2;
      cp = byte & 0x1Fu;
    } else if ((byte & 0xF0) == 0xE0) {
      length = 3;
      cp = byte & 0x0Fu;
    } else if ((byte & 0xF8) == 0xF0) {
      length = 4;
      cp = byte & 0x07u;
    } else {
      // A continuation byte or an invalid lead. Consume one byte so a
      // malformed string cannot spin here, and report it as the replacement
      // character -- this is a display path, not a validator.
      out.push_back(0xFFFDu);
      ++i;
      continue;
    }
    if (i + length > utf8.size()) {
      out.push_back(0xFFFDu);
      break;
    }
    bool wellFormed = true;
    for (size_t k = 1; k < length; ++k) {
      const auto cont = static_cast<unsigned char>(utf8[i + k]);
      if ((cont & 0xC0) != 0x80) {
        wellFormed = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3Fu);
    }
    out.push_back(wellFormed ? cp : 0xFFFDu);
    i += wellFormed ? length : 1;
  }
  return out;
}

FontLoadResult installUiGlyphFont(float sizePx) {
  FontLoadResult result;
  const std::vector<uint32_t>& required = requiredUiCodepoints();
  if (required.empty()) {
    result.ok = true;
    return result;
  }

  ImFontAtlas* atlas = ImGui::GetIO().Fonts;

  // `MergeMode` merges into the *previous* font in the atlas (imgui.h), so
  // there has to be one. ImGui adds ProggyClean lazily at first use if the
  // atlas is empty, which is too late to merge into -- so it is added here,
  // explicitly, and this call is the reason the merge has a target at all.
  if (atlas->Fonts.Size == 0) atlas->AddFontDefault();

  // ImGui's *LEGACY* GlyphRanges array: pairs of inclusive bounds, zero
  // terminated, and "THE ARRAY DATA NEEDS TO PERSIST AS LONG AS THE FONT IS
  // ALIVE" (imgui.h). Hence static. One pair per codepoint rather than one
  // spanning pair, so the merge asks for exactly the six glyphs it needs and
  // not the ~1400 codepoints between U+25A1 and U+2702.
  static std::vector<ImWchar> ranges;
  if (ranges.empty()) {
    for (const uint32_t cp : required) {
      ranges.push_back(static_cast<ImWchar>(cp));
      ranges.push_back(static_cast<ImWchar>(cp));
    }
    ranges.push_back(0);
  }

  std::string tried;
  ImFont* merged = nullptr;
  for (const GlyphFontCandidate& candidate : kCandidates) {
    std::error_code ec;
    if (!std::filesystem::exists(candidate.path, ec)) {
      if (!tried.empty()) tried += "; ";
      tried += std::string(candidate.path) + " (not present)";
      continue;
    }
    ImFontConfig config;
    config.MergeMode = true;   // add to the default font, do not replace it
    config.PixelSnapH = true;  // the default font is a bitmap font
    config.GlyphRanges = ranges.data();
    ImFont* font = atlas->AddFontFromFileTTF(candidate.path, sizePx, &config);
    if (font == nullptr) {
      if (!tried.empty()) tried += "; ";
      tried += std::string(candidate.path) + " (present, but ImGui refused it)";
      continue;
    }
    result.path = candidate.path;
    merged = font;
    break;
  }

  if (result.path.empty()) {
    result.missing = required;
    result.error = "ui/Fonts: no glyph source loaded, so " + std::to_string(required.size()) +
                   " layer-kind glyphs will draw as boxes. Tried: " + tried +
                   ". docs/ui.md 3.2 assigns these glyphs; ImGui's built-in ProggyClean "
                   "holds nothing above U+00FF.";
    return result;
  }

  // Loaded is not the same as covers. Ask the baked font, per codepoint,
  // rather than trusting the candidate table -- the table is a measurement
  // from one machine and this runs on whichever machine it runs on.
  if (merged != nullptr) {
    ImFontBaked* baked = merged->GetFontBaked(sizePx);
    if (baked != nullptr) {
      for (const uint32_t cp : required)
        if (baked->FindGlyphNoFallback(static_cast<ImWchar>(cp)) == nullptr)
          result.missing.push_back(cp);
    }
  }

  if (!result.missing.empty()) {
    std::string list;
    for (const uint32_t cp : result.missing) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "U+%04X", cp);
      if (!list.empty()) list += ", ";
      list += buffer;
    }
    result.error = "ui/Fonts: merged " + result.path + ", but it cannot draw " +
                   std::to_string(result.missing.size()) + " of " +
                   std::to_string(required.size()) + " required codepoints (" + list +
                   "); those will draw as boxes.";
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace np
