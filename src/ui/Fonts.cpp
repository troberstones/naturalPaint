#include "ui/Fonts.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <system_error>

#include "app/LayerEditor.hpp"
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

// The type ramp's two faces (docs/ui.md section 1). Same short-list discipline
// as the glyph source above: every entry is one somebody has a reason for.
//
// Archivo leads both lists it could plausibly satisfy, at the paths a manual
// or Homebrew install puts it, so that installing the design's actual face is
// the whole of the work. It is a Google font; nothing ships it.
constexpr GlyphFontCandidate kTextCandidates[] = {
    {"/Library/Fonts/Archivo-Regular.ttf", "the design's own face, if it was ever installed"},
    {"/System/Library/Fonts/HelveticaNeue.ttc",
     "the substitution this build makes -- a neo-grotesque, the same species as Archivo"},
    {"/System/Library/Fonts/Helvetica.ttc", "the older grotesque, if Neue is absent"},
};

// `ui-monospace` on macOS is SF Mono. Menlo is second because ui/Fonts already
// proves it loads and covers -- it is the glyph source above -- so a machine
// where SFNSMono refuses still gets the distinction rather than losing it.
constexpr GlyphFontCandidate kMonoCandidates[] = {
    {"/System/Library/Fonts/SFNSMono.ttf", "macOS's own ui-monospace, which is what the doc names"},
    {"/System/Library/Fonts/Menlo.ttc", "proven here already: it is the glyph source"},
};

UiFonts g_fonts;

// Loads the first candidate that exists and that ImGui accepts, appending to
// `tried` for the ones that did not. Returns nullptr when none did.
ImFont* loadFirst(const GlyphFontCandidate* first, size_t count, float sizePx, std::string* pathOut,
                  std::string* tried) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  for (size_t i = 0; i < count; ++i) {
    const GlyphFontCandidate& candidate = first[i];
    std::error_code ec;
    if (!std::filesystem::exists(candidate.path, ec)) {
      if (!tried->empty()) *tried += "; ";
      *tried += std::string(candidate.path) + " (not present)";
      continue;
    }
    ImFontConfig config;
    // Deliberately NOT PixelSnapH: that exists for ImGui's built-in bitmap
    // font, and snapping a vector face's advances to whole pixels at 13 px is
    // what makes proportional text look unevenly spaced.
    ImFont* font = atlas->AddFontFromFileTTF(candidate.path, sizePx, &config);
    if (font == nullptr) {
      if (!tried->empty()) *tried += "; ";
      *tried += std::string(candidate.path) + " (present, but ImGui refused it)";
      continue;
    }
    *pathOut = candidate.path;
    return font;
  }
  return nullptr;
}

}  // namespace

const UiFonts& uiFonts() { return g_fonts; }

const std::vector<uint32_t>& requiredUiCodepoints() {
  // Built from `layerKindGlyph()` and `layerCommandGlyph()` themselves rather
  // than retyped, so the list can never drift from what the panel actually
  // asks for. Every kind and every command is walked, including any added
  // later, which is what makes the drift impossible rather than merely
  // unlikely.
  static const std::vector<uint32_t> kPoints = [] {
    std::vector<uint32_t> points;
    auto addGlyph = [&points](const char* glyph) {
      for (const uint32_t cp : decodeUtf8(glyph)) {
        // Below 0x0100 is ImGui's own default range and needs no merge
        // source; including it would ask a second font for glyphs the first
        // already draws, which is exactly what GlyphExcludeRanges exists to
        // prevent.
        if (cp >= 0x0100 && std::find(points.begin(), points.end(), cp) == points.end())
          points.push_back(cp);
      }
    };
    for (const LayerKind kind :
         {LayerKind::Pigment, LayerKind::RGB, LayerKind::Media, LayerKind::Strokes,
          LayerKind::Adjustment, LayerKind::Text, LayerKind::Flats})
      addGlyph(layerKindGlyph(kind));
    // Most of `layerCommandGlyph()`'s results are "" (a command with no
    // icon), and `addGlyph()` decodes that to nothing -- exactly the no-op
    // this walk wants for them.
    for (const LayerCommand command : allLayerCommands()) addGlyph(layerCommandGlyph(command));
    // Panel chrome that opens a dialog rather than issuing a `LayerCommand` --
    // there is only one of these today, so a table would be one row. The
    // "Layer Properties" gear button in ui/MacPaintUI.cpp is the one caller;
    // if a second one of these ever exists, the two belong in a shared list
    // the way the kind and command glyphs already are.
    addGlyph("\xE2\x9A\x99");  // U+2699 gear
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

FontLoadResult installUiFonts(float sizePx) {
  FontLoadResult result;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;

  // --- the ramp -----------------------------------------------------------
  //
  // The text face goes in FIRST, and that ordering is the whole of the
  // installation: ImGui draws with `Fonts[0]` unless something pushes another,
  // and `MergeMode` merges into the *previous* font in the atlas (imgui.h). So
  // loading the grotesque here makes it both the default face and the merge
  // target for the layer-kind glyphs below, in one step.
  //
  // If none loads, ProggyClean is added explicitly rather than left to ImGui's
  // lazy first-use path -- lazy is too late to merge into, and this call is the
  // reason the merge has a target at all.
  std::string textTried;
  g_fonts.text =
      loadFirst(kTextCandidates, std::size(kTextCandidates), sizePx, &result.textPath, &textTried);
  if (g_fonts.text == nullptr) g_fonts.text = atlas->AddFontDefault();

  const std::vector<uint32_t>& required = requiredUiCodepoints();
  if (required.empty()) {
    result.ok = true;
    return result;
  }

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
    config.MergeMode = true;  // add to the text face above, do not replace it
    // PixelSnapH only when the target is ImGui's bitmap ProggyClean -- see
    // loadFirst() above on why a vector face must not be snapped.
    config.PixelSnapH = result.textPath.empty();
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

  // --- the monospace half of the ramp -------------------------------------
  //
  // Last, and separately: it is a font callers *push*, not the default one, so
  // it must not be the atlas's first entry and must not be a merge target.
  // A missing mono face costs the prose/numerics distinction and nothing else,
  // so it does not touch `result.ok` -- which is still about the glyphs.
  std::string monoTried;
  g_fonts.mono =
      loadFirst(kMonoCandidates, std::size(kMonoCandidates), sizePx, &result.monoPath, &monoTried);

  result.ok = true;
  return result;
}

}  // namespace np
