#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Path.hpp"

// text/Shaper -- the platform-independent text-shaping interface (PRD K2:
// "shaping, bidirectional text and font fallback come from the platform
// (CoreText), behind an interface that HarfBuzz + FreeType could replace").
//
// ==========================================================================
// Why this header may not name a single CoreText type
// ==========================================================================
//
// K2's whole point is that the *platform* does shaping -- bidi reordering,
// cluster breaking, and font fallback are each their own multi-year problem,
// and CoreText already solves all three correctly (see PRD's "Own text
// shaping" non-goal). But "call the platform" and "let the platform's types
// leak into every caller" are different decisions. A `Path`-producing text
// tool, a layer panel, and (eventually) a Windows/Linux build all need to
// call `shapeText()`; none of them need to know it was CoreText that ran.
//
// So every type below is either a plain value (`std::string_view`, `float`,
// `uint32_t`) or defined in this file. `text/CoreTextShaper.mm` is the only
// translation unit in the tree that may `#include <CoreText/CoreText.h>`,
// and `text/StubShaper.cpp` implements the identical interface with no
// platform framework at all, so a HarfBuzz + FreeType backend is a third
// `.cpp` behind this same header, not a header change.
//
// ==========================================================================
// The space every point and path in this file is measured in
// ==========================================================================
//
// **Y is DOWN.** CoreText and CoreGraphics are y-up (text grows upward off
// the baseline, a Quartz path's origin is its lower-left corner); this
// application's document space is y-down, matching every other geometry
// producer a `Path` reaches (io/SvgPath, core/PathFlatten). The flip happens
// exactly once, inside text/CoreTextShaper.mm, so that nothing past this
// header ever has to remember which convention a given number came in. A
// caller that gets this wrong by *not* flipping renders symmetric glyphs
// ("o", "x") correctly and everything with an ascender or descender upside
// down -- which is why app/selftest/TextShaper.cpp's load-bearing assertion
// is on a "p" and a "b", not on a glyph that would hide the bug.
//
// X and Y for `ShapedGlyph` are pen positions in the same units as
// `TextStyle::sizePx` (pixels at 1:1 zoom), with the origin at the top-left
// of the shaped block -- of the single line for point text, of the frame's
// content box for paragraph text.
//
// `glyphPath()` uses a *different* origin -- the glyph's own design origin
// on its baseline -- because a glyph outline is reusable across every place
// that glyph ID is drawn, and a caller composites it by translating to
// `ShapedGlyph::{x, y}` itself rather than this file re-deriving that
// translation on every call.
namespace np {

// --------------------------------------------------------------------------
// Style and layout inputs
// --------------------------------------------------------------------------

enum class TextAlign { Left, Center, Right, Justified };

// One style applies to a whole `shapeText()` call. PRD's "Text on a path,
// vertical text, rich-text runs" is explicitly out of scope (PRD:102) --
// there is deliberately no way to change style mid-string here. A caller
// that wants two styles in one text block shapes twice and concatenates the
// two `ShapedText` results itself, at the cost of losing CoreText's own
// cross-run line breaking; that trade is PRD's, not this file's.
struct TextStyle {
  std::string fontFamily = "Helvetica";
  float sizePx = 24.0f;
  float tracking = 0.0f;       // extra advance per glyph, in px
  float leading = 0.0f;        // 0 means "the font's own"
  bool bold = false;
  bool italic = false;
};

// A paragraph's box. Point text (`width == 0`) has no wrapping and no
// alignment -- there is only one line, so `TextAlign` has nothing to align
// against, matching what every vector tool calls "point text" vs "area
// text". `height == 0` under paragraph text means "as tall as the shaped
// lines need", not "zero lines": the frame grows to fit rather than
// clipping, because a caller that wanted clipping would have to re-shape at
// a different height anyway to know how much text was lost.
struct TextFrame {
  float width = 0.0f;
  float height = 0.0f;
};

// --------------------------------------------------------------------------
// Shaped output
// --------------------------------------------------------------------------

// One shaped glyph, already positioned in text-space (see the header
// comment above for the origin and the y-down convention).
//
// `glyphId` is meaningful only together with the `TextStyle` that produced
// it -- it is the font's own glyph index, not a Unicode code point, and two
// different fonts number their glyphs differently. Pass the SAME style to
// `glyphPath()` that was passed to the `shapeText()` call this glyph came
// from; there is no way to recover a style from a `glyphId` alone, by
// design -- carrying a font handle through this struct is exactly the
// platform leak section 1 above rules out.
struct ShapedGlyph {
  uint32_t glyphId = 0;
  float x = 0.0f;              // pen position, y DOWN (see the header)
  float y = 0.0f;
  uint32_t cluster = 0;        // byte offset into the source UTF-8
};

// `cluster` on every glyph above is a byte offset into the *original*
// `utf8` argument, not a code-point or UTF-16 index -- so a caller doing
// "click here, find the glyph" can slice the original `std::string_view`
// directly. Bidi reordering and font-fallback substitution can both make
// clusters land out of glyph order (a right-to-left run's glyphs walk
// backwards through the source bytes) but never off a UTF-8 character
// boundary, which app/selftest/TextShaper.cpp checks directly against a
// multi-byte string rather than trusting the platform.
struct ShapedText {
  bool ok = false;
  std::string error;
  std::vector<ShapedGlyph> glyphs;
  std::vector<std::string> fontsUsed;   // after fallback
  float widthPx = 0.0f;
  float heightPx = 0.0f;
  int lineCount = 0;
};

// Shape UTF-8 text. Bidi, cluster breaking and font fallback all come from
// the platform.
//
// **Defined answers for the two inputs that are not malformed but are
// awkward**, so callers do not need a special case of their own:
//   * An empty `utf8` returns `ok == true` with no glyphs, no fonts, zero
//     size, and `lineCount == 0` -- there is nothing shaped and nothing to
//     report a font for, so claiming one line or one font would be
//     asserting a fact about a font this call never touched.
//   * A `fontFamily` the platform does not have does NOT fail: the platform
//     substitutes its own default and `ok` stays true. `fontsUsed` names
//     whatever font actually got used, which is how a caller notices the
//     substitution happened without this call refusing to shape a document
//     that only has a typo in its font name.
// `ok == false` is reserved for input this file cannot make sense of at
// all -- `utf8` that is not valid UTF-8 -- and `error` says why.
ShapedText shapeText(std::string_view utf8, const TextStyle& style,
                     const TextFrame& frame, TextAlign align);

// One glyph's outline, in text-space units at `style.sizePx`, with the
// origin at the glyph's own design origin on its baseline and Y DOWN --
// a descender (a "p"'s tail) has POSITIVE y, an ascender or capital
// (a "b"'s stem) has NEGATIVE y. See the header comment on why this is a
// different origin from `ShapedGlyph`'s.
//
// Every curve CoreText can hand back -- line, quadratic, cubic, close --
// lands in `out` as core/Path.hpp's one segment type: a quadratic is
// elevated to a cubic by `c1 = p0 + (2/3)(q - p0)`, `c2 = p2 + (2/3)(q -
// p2)` (the same formula io/SvgPath.hpp uses for the same reason), and a
// line is an anchor pair whose handles coincide with their own anchors.
//
// Returns false, leaving `*out` untouched, for a `glyphId` the current font
// does not have (including `glyphId == 0`, the platform's own "notdef"/
// missing-glyph sentinel) -- a missing glyph is not a geometry error, it is
// the font simply not answering, and a caller papering over that with an
// empty `Path` would draw nothing and never learn why.
bool glyphPath(uint32_t glyphId, const TextStyle& style, Path* out);

// Whether this build has a real shaper. False on the non-Apple stub
// (text/StubShaper.cpp) -- true on every Apple build, because CoreText
// ships with the OS and this file has no "installed but broken" state to
// report; a font family that cannot be found is not a shaper failure (see
// `shapeText()`'s own comment on that case).
bool shaperAvailable() noexcept;

// What the stub/absent case should say, for a refusal message. Empty when
// `shaperAvailable()` is true -- there is nothing to explain when shaping
// works, matching the convention app/Journal's `journalUnavailableReason()`
// already uses for the same shape of question.
const char* shaperUnavailableReason() noexcept;

}  // namespace np
