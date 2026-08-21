#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace np {

// ui/Fonts -- the UI's glyph coverage, and the one place that knows a
// designed glyph is not the same thing as a drawable one.
//
// --- What this completes, and what was already here ------------------------
//
// docs/ui.md §3.2 assigns each layer kind a glyph, for a stated reason:
// "Pigment is the *default* kind, so this hid the product's entire
// differentiator." `app/LayerPanel`'s `layerKindGlyph()` returns them, and
// six of the seven are multi-byte UTF-8 (U+25C9, U+25A1, U+25C8, U+2702,
// U+25A4, U+25A9).
//
// **Nothing in this project ever loaded a font**, and there is still no
// `AddFontFrom*` call outside this file -- so Dear ImGui uses its embedded
// ProggyClean, which holds no glyph above U+00FF. That was found by
// photographing the panel, and `ui/MacPaintUI`'s `layerKindGlyphForFont()`
// already answers it: it asks `ImGui::GetFont()->IsGlyphInFont()` and
// substitutes the kind's initial in brackets -- `[R]`, `[P]`, `[A]` -- when
// the answer is no.
//
// **That stand-in was written to be temporary, and this module is the half it
// was waiting for.** Its comment says so in as many words: "The real glyph is
// used the moment a font that has it is loaded, because the test is against
// the font rather than against a build flag." So nothing in the panel changes
// here. A glyph source is merged, `IsGlyphInFont()` starts answering yes, and
// the existing predicate returns the designed glyph on its own.
//
// The reason the stand-in was needed for nine steps is worth keeping: **nine
// `--selftest` sections assert the correct glyph and all nine pass**, because
// they check what `layerKindGlyph()` *returns*, never what reaches a pixel.
// `requiredUiCodepoints()` below is the part that can be tested, and the
// assertion that matters is that every glyph the UI asks for is in that list.
//
// --- Why a merge, and not simply a better UI font -------------------------
//
// The obvious fix is to load Menlo (measured below) as *the* UI font. That
// would also re-metric every string in the application, and the right panel
// already clips its labels horizontally ("Granulatio", "Edge darke") at
// ProggyClean's widths. Changing every width to fix six glyphs would trade a
// visible bug for a wider one.
//
// So ProggyClean stays the UI font and a second source is **merged** over it
// for exactly the codepoints it cannot draw. Every existing string keeps its
// existing metrics, byte for byte, and only the previously-undrawable
// codepoints come from elsewhere.
//
// --- Which font, measured not assumed -------------------------------------
//
// CoreText was asked directly (`CTFontGetGlyphsForCharacters`) which system
// fonts can draw all seven codepoints the UI needs:
//
//     Menlo.ttc            7/7
//     Apple Symbols.ttf    6/7   (no U+2702 scissors)
//     SFNSMono.ttf         2/7
//     Geneva.ttf           1/7
//
// Menlo is the only complete one on this machine, and it is monospace --
// which is what §3.2 specifies for the sub-line it sits beside anyway. Apple
// Symbols is kept as a second candidate because six of seven is better than
// nothing, and the one it misses is reported by name rather than silently
// drawn as a box.
//
// A system font path is not a guarantee. When no candidate loads, this
// module **says which files it tried and what it wanted from them** rather
// than leaving boxes on screen with no explanation.

// The codepoints the UI needs beyond ImGui's default `0x0020-0x00FF`.
// `app/LayerPanel`'s kind glyphs are the only source of these today; a new
// kind whose glyph is not in this list fails `--selftest` rather than
// shipping as a box.
const std::vector<uint32_t>& requiredUiCodepoints();

// UTF-8 -> codepoints. Pure, and the piece the glyph test needs: it turns
// `layerKindGlyph()`'s bytes back into the numbers this header reasons about.
// Malformed input yields U+FFFD for the offending byte rather than throwing,
// because the caller is a display path.
std::vector<uint32_t> decodeUtf8(std::string_view utf8);

struct FontLoadResult {
  // True when a merge source loaded AND it covered every required codepoint.
  bool ok = false;
  // The file that was merged, or empty when none loaded.
  std::string path;
  // Non-empty exactly when !ok. Names every candidate tried and what was
  // wrong with it, in the tone core/LayerOps refuses in.
  std::string error;
  // Required codepoints the merged font still cannot draw. Empty when ok.
  std::vector<uint32_t> missing;
};

// Merges a glyph source over the current default font for
// `requiredUiCodepoints()`. Call once, after `ImGui::CreateContext()` and
// before the first frame. `sizePx` should match the default font's size so
// the merged glyphs sit on the same baseline.
FontLoadResult installUiGlyphFont(float sizePx);

}  // namespace np
