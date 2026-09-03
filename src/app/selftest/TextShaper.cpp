#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "core/Path.hpp"
#include "core/PathFlatten.hpp"
#include "text/Shaper.hpp"

#if defined(__APPLE__)
// Section 6 (quadratic elevation) is the one place this file reaches past
// text/Shaper.hpp's own interface, and only to fetch a REFERENCE quadratic
// control point CoreText itself emitted for a real glyph -- there is no way
// to ask the public `glyphPath()` "was this segment originally a quadratic",
// by design (text/Shaper.hpp's whole point is that no caller needs to know
// or care). Guarded to Apple only: on a build with no CoreText, `shaperAvailable()`
// is false before section 6 is ever reached, but the file still has to
// COMPILE on that platform, which these headers would not.
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif

namespace np {

// text/Shaper -- PRD K2's platform-independent shaping interface, and its
// CoreText implementation. See app/SelfTest.hpp for the full list of what
// this section proves and why it is guarded on `shaperAvailable()`.
bool runTextShaperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  if (!shaperAvailable()) {
    std::printf("  [skip] text/Shaper: %s\n", shaperUnavailableReason());
    return true;
  }

  // A byte offset is a valid UTF-8 character boundary when it is the end of
  // the string or does not point at a continuation byte (0x80-0xBF) -- the
  // check `cluster` values must pass so a caller slicing the source string
  // at a cluster never lands mid-character.
  auto isUtf8Boundary = [](std::string_view s, size_t off) {
    if (off >= s.size()) return off == s.size();
    return (static_cast<unsigned char>(s[off]) & 0xC0u) != 0x80u;
  };

  auto clustersWellFormed = [&](const ShapedText& t, std::string_view src) {
    uint32_t prev = 0;
    for (size_t i = 0; i < t.glyphs.size(); ++i) {
      const uint32_t c = t.glyphs[i].cluster;
      if (c > src.size()) return false;
      if (i > 0 && c < prev) return false;
      if (!isUtf8Boundary(src, c)) return false;
      prev = c;
    }
    return true;
  };

  TextStyle style;
  style.fontFamily = "Helvetica";
  style.sizePx = 24.0f;
  const TextFrame pointFrame;  // width == 0 -> point text

  // ==========================================================================
  // 1. "Hello": glyph count, strictly increasing x, non-empty fontsUsed.
  // ==========================================================================
  std::printf("  -- 1. \"Hello\" in point mode --\n");
  {
    const ShapedText hello = shapeText("Hello", style, pointFrame, TextAlign::Left);
    check(hello.ok, "shapeText(\"Hello\") succeeds");
    check(hello.glyphs.size() == 5, "\"Hello\" shapes to exactly 5 glyphs");
    check(!hello.fontsUsed.empty(), "fontsUsed is non-empty");
    bool increasing = true;
    for (size_t i = 1; i < hello.glyphs.size(); ++i)
      if (!(hello.glyphs[i].x > hello.glyphs[i - 1].x)) increasing = false;
    check(increasing, "glyph x strictly increases left to right");
    check(clustersWellFormed(hello, "Hello"),
          "clusters are non-decreasing, in bounds, and on UTF-8 boundaries (ASCII)");
  }

  // ==========================================================================
  // 2. Multi-byte UTF-8: an accented Latin string and a CJK string.
  // ==========================================================================
  std::printf("  -- 2. multi-byte UTF-8 --\n");
  {
    // "h\xC3\xA9llo" == "héllo": 'é' is one 2-byte sequence, so the cluster
    // after it must be 3, never 2 (mid-sequence).
    const std::string accented = "h\xC3\xA9llo";
    const ShapedText hAccented = shapeText(accented, style, pointFrame, TextAlign::Left);
    check(hAccented.ok, "shapeText(\"h\\xC3\\xA9llo\") succeeds");
    check(!hAccented.glyphs.empty(), "\"héllo\" shapes to at least one glyph");
    check(clustersWellFormed(hAccented, accented),
          "clusters land on UTF-8 boundaries for a 2-byte character, not mid-sequence");

    // "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" == "日本語", three 3-byte
    // characters -- the cluster after the first must be 3, not 1 or 2.
    const std::string cjk = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    const ShapedText jp = shapeText(cjk, style, pointFrame, TextAlign::Left);
    check(jp.ok, "shapeText() on a CJK string succeeds");
    check(!jp.glyphs.empty(), "a CJK string shapes to at least one glyph");
    check(clustersWellFormed(jp, cjk),
          "clusters land on UTF-8 boundaries for 3-byte characters, not mid-sequence");
  }

  // ==========================================================================
  // 3. Refusals that must be defined, not a crash: invalid UTF-8, an empty
  //    string, and a font family the platform does not have.
  // ==========================================================================
  std::printf("  -- 3. defined answers for empty/invalid/unknown-font input --\n");
  {
    // A lone continuation byte is not valid UTF-8 on its own.
    const std::string invalid = "\x80\x80";
    const ShapedText bad = shapeText(invalid, style, pointFrame, TextAlign::Left);
    check(!bad.ok && !bad.error.empty(),
          "invalid UTF-8 returns ok=false with a non-empty error, not a crash");

    const ShapedText empty = shapeText("", style, pointFrame, TextAlign::Left);
    check(empty.ok && empty.glyphs.empty() && empty.fontsUsed.empty() &&
              empty.lineCount == 0 && empty.widthPx == 0.0f && empty.heightPx == 0.0f,
          "an empty string is ok=true with nothing shaped, per text/Shaper.hpp's own contract");

    TextStyle unknownFont = style;
    unknownFont.fontFamily = "ThisFontDoesNotExistAnywhere12345";
    const ShapedText fallback = shapeText("A", unknownFont, pointFrame, TextAlign::Left);
    check(fallback.ok && !fallback.glyphs.empty() && !fallback.fontsUsed.empty(),
          "an unknown font family substitutes a real one rather than failing");
  }

  // ==========================================================================
  // 4. The y-flip: a "p" (descender) and a "b" (ascender) against the
  //    baseline. This is the load-bearing assertion -- see app/SelfTest.hpp.
  // ==========================================================================
  std::printf("  -- 4. the y-flip, on an asymmetric glyph pair --\n");
  {
    const ShapedText pb = shapeText("pb", style, pointFrame, TextAlign::Left);
    check(pb.ok && pb.glyphs.size() == 2, "\"pb\" shapes to exactly 2 glyphs");
    if (pb.ok && pb.glyphs.size() == 2) {
      const ShapedGlyph& glyphP = pb.glyphs[0];
      const ShapedGlyph& glyphB = pb.glyphs[1];

      Path pathP, pathB;
      const bool gotP = glyphPath(glyphP.glyphId, style, &pathP);
      const bool gotB = glyphPath(glyphB.glyphId, style, &pathB);
      check(gotP && gotB, "glyphPath() succeeds for both 'p' and 'b'");

      if (gotP && gotB) {
        check(pathIsFinite(pathP) && pathIsFinite(pathB),
              "glyphPath() output passes pathIsFinite()");
        const PathBounds boundsP = pathTightBounds(pathP);
        const PathBounds boundsB = pathTightBounds(pathB);
        check(boundsP.valid && boundsP.maxX > boundsP.minX && boundsP.maxY > boundsP.minY,
              "'p' has non-degenerate bounds");
        check(boundsB.valid && boundsB.maxX > boundsB.minX && boundsB.maxY > boundsB.minY,
              "'b' has non-degenerate bounds");
        std::printf("  [measured] 'p' y-range %.2f..%.2f, 'b' y-range %.2f..%.2f (baseline at 0)\n",
                    boundsP.minY, boundsP.maxY, boundsB.minY, boundsB.maxY);
        // 'p' has a descender: in this y-DOWN space its outline must reach
        // measurably PAST the baseline (y > 0) -- getting the flip backwards
        // would instead show 'p' entirely at y <= 0.
        check(boundsP.maxY > 2.0f, "'p': the descender extends to LARGER y than the baseline");
        // The line above is TRUE IN BOTH CONVENTIONS and so proves nothing on
        // its own -- 'p' straddles the baseline, so its y-range clears +2
        // whichever way up it is (measured: -12.86..5.00 flipped,
        // -5.00..12.86 not). Found by sabotage: dropping the flip left it
        // green. What actually distinguishes them is the ASYMMETRY -- a 'p'
        // reaches much further above the baseline (bowl and stem) than below
        // it (descender), so in this y-down space the descender depth must be
        // the SMALLER of the two magnitudes. Upside down, it is the larger.
        check(boundsP.maxY < -boundsP.minY,
             "'p': the descender is shallower than the bowl is tall -- only "
             "true y-DOWN (the assertion above passes either way up)");
        // 'b' has an ascender and no descender: its outline must reach
        // measurably ABOVE the baseline (y < 0) and not noticeably below it.
        check(boundsB.minY < -2.0f, "'b': the ascender extends to SMALLER y than the baseline");
        check(boundsB.maxY < 2.0f, "'b': (contrast) has no descender, unlike 'p'");
      }
    }
  }

  // ==========================================================================
  // 5. Paragraph text: a narrow frame wraps, a wide one does not.
  // ==========================================================================
  std::printf("  -- 5. paragraph text: frame width controls line count --\n");
  {
    const char* sentence = "The quick brown fox jumps over the lazy dog";
    const ShapedText narrow =
        shapeText(sentence, style, TextFrame{80.0f, 0.0f}, TextAlign::Left);
    const ShapedText wide =
        shapeText(sentence, style, TextFrame{4000.0f, 0.0f}, TextAlign::Left);
    check(narrow.ok && wide.ok, "shapeText() succeeds for both frame widths");
    std::printf("  [measured] lineCount narrow=%d wide=%d\n", narrow.lineCount, wide.lineCount);
    check(narrow.lineCount > 1, "an 80px-wide frame wraps the sentence onto more than one line");
    check(wide.lineCount == 1, "a 4000px-wide frame keeps the sentence on one line");
    check(clustersWellFormed(narrow, sentence) && clustersWellFormed(wide, sentence),
          "paragraph-text clusters are still well-formed");
  }

  // ==========================================================================
  // 6. tracking increases total width monotonically.
  // ==========================================================================
  std::printf("  -- 6. tracking increases width --\n");
  {
    TextStyle noTrack = style, someTrack = style, moreTrack = style;
    someTrack.tracking = 5.0f;
    moreTrack.tracking = 10.0f;
    const ShapedText w0 = shapeText("Hello", noTrack, pointFrame, TextAlign::Left);
    const ShapedText w1 = shapeText("Hello", someTrack, pointFrame, TextAlign::Left);
    const ShapedText w2 = shapeText("Hello", moreTrack, pointFrame, TextAlign::Left);
    check(w0.ok && w1.ok && w2.ok, "shapeText() succeeds at all three tracking values");
    std::printf("  [measured] widthPx at tracking 0/5/10: %.2f / %.2f / %.2f\n",
                w0.widthPx, w1.widthPx, w2.widthPx);
    check(w1.widthPx > w0.widthPx && w2.widthPx > w1.widthPx,
          "widthPx strictly increases as tracking increases");
  }

  // ==========================================================================
  // 7. glyphPath() refusals: glyph 0 ("notdef") and an out-of-range id.
  // ==========================================================================
  std::printf("  -- 7. glyphPath() refusals --\n");
  {
    Path scratch;
    check(!glyphPath(0, style, &scratch), "glyphPath(0, ...) refuses rather than drawing tofu");
    check(!glyphPath(0xFFFFFFFFu, style, &scratch),
          "glyphPath() refuses a glyph id outside CGGlyph's range rather than crashing");
  }

#if defined(__APPLE__)
  // ==========================================================================
  // 8. Quadratic elevation: a REAL quadratic element from a real glyph,
  //    elevated by CoreTextShaper.mm's formula, sampled with cubicAt()
  //    against the quadratic evaluated directly.
  //
  // This reaches past text/Shaper.hpp on purpose (see the header comment at
  // the top of this file): the only way to get a reference quadratic to
  // check the elevation against is to ask CoreText for the same glyph's raw
  // path a second time, independently of glyphPath(). Whether any given
  // glyph's outline contains a quadratic segment at all is a property of
  // the installed font (TrueType glyf outlines are quadratic; CFF/PostScript
  // outlines are already cubic), so this scans several glyphs from a
  // curve-rich string and reports a measured skip -- not a FAIL -- if none
  // of them happens to contain one, rather than asserting a fact about a
  // font this build does not control.
  // ==========================================================================
  std::printf("  -- 8. quadratic-to-cubic elevation, against a real glyph --\n");
  {
    const ShapedText probe =
        shapeText("Sphinx of black quartz judge my vow", style, pointFrame, TextAlign::Left);
    check(probe.ok, "shapeText() succeeds for the quadratic-probe sentence");

    CFStringRef familyName =
        CFStringCreateWithCString(kCFAllocatorDefault, style.fontFamily.c_str(), kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateWithName(familyName, style.sizePx, nullptr);
    if (familyName) CFRelease(familyName);

    bool foundQuad = false;
    bool elevationMatches = true;
    float worstDeviation = 0.0f;

    if (font && probe.ok) {
      for (const ShapedGlyph& g : probe.glyphs) {
        if (foundQuad) break;
        CGPathRef cgPath = CTFontCreatePathForGlyph(font, static_cast<CGGlyph>(g.glyphId), nullptr);
        if (!cgPath) continue;

        __block bool blockFound = false;
        __block PathPoint blockP0{0.0f, 0.0f};
        __block PathPoint blockCurrent{0.0f, 0.0f};
        __block PathPoint blockQ{0.0f, 0.0f};
        __block PathPoint blockEnd{0.0f, 0.0f};
        // Same y-flip text/CoreTextShaper.mm applies -- the reference must
        // be taken in the same space `glyphPath()`'s output already is, or
        // this would compare a y-up quadratic against a y-down cubic and
        // fail by construction rather than by a real defect.
        CGPathApplyWithBlock(cgPath, ^(const CGPathElement* element) {
          if (blockFound) return;
          if (element->type == kCGPathElementMoveToPoint) {
            blockCurrent = {static_cast<float>(element->points[0].x),
                            -static_cast<float>(element->points[0].y)};
          } else if (element->type == kCGPathElementAddLineToPoint) {
            blockCurrent = {static_cast<float>(element->points[0].x),
                            -static_cast<float>(element->points[0].y)};
          } else if (element->type == kCGPathElementAddCurveToPoint) {
            blockCurrent = {static_cast<float>(element->points[2].x),
                            -static_cast<float>(element->points[2].y)};
          } else if (element->type == kCGPathElementAddQuadCurveToPoint) {
            blockP0 = blockCurrent;
            blockQ = {static_cast<float>(element->points[0].x), -static_cast<float>(element->points[0].y)};
            blockEnd = {static_cast<float>(element->points[1].x), -static_cast<float>(element->points[1].y)};
            blockCurrent = blockEnd;
            blockFound = true;
          }
        });
        CGPathRelease(cgPath);

        if (blockFound) {
          foundQuad = true;
          const PathPoint p0 = blockP0, q = blockQ, end = blockEnd;
          // text/Shaper.hpp's stated elevation formula, applied here
          // independently of text/CoreTextShaper.mm's own copy of it.
          const PathPoint c1{p0.x + (2.0f / 3.0f) * (q.x - p0.x), p0.y + (2.0f / 3.0f) * (q.y - p0.y)};
          const PathPoint c2{end.x + (2.0f / 3.0f) * (q.x - end.x), end.y + (2.0f / 3.0f) * (q.y - end.y)};
          PathPoint cubic[4] = {p0, c1, c2, end};
          for (float t : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
            const float u = 1.0f - t;
            const PathPoint quadAt{u * u * p0.x + 2.0f * u * t * q.x + t * t * end.x,
                                    u * u * p0.y + 2.0f * u * t * q.y + t * t * end.y};
            const PathPoint cubicAtT = cubicAt(cubic, t);
            const float dx = quadAt.x - cubicAtT.x, dy = quadAt.y - cubicAtT.y;
            const float dev = std::sqrt(dx * dx + dy * dy);
            worstDeviation = std::max(worstDeviation, dev);
            if (dev > 1.0e-2f) elevationMatches = false;
          }
        }
      }
    }
    if (font) CFRelease(font);

    if (!foundQuad) {
      std::printf("  [skip] no quadratic glyph-outline segment found in %s at %.0fpx -- this "
                  "font's outlines may be CFF/PostScript (already cubic), not a defect\n",
                  style.fontFamily.c_str(), style.sizePx);
    } else {
      std::printf("  [measured] quadratic->cubic elevation: worst deviation %.4e px\n",
                  worstDeviation);
      check(elevationMatches,
            "a real CoreText quadratic, elevated by the stated formula, samples "
            "against the quadratic itself to 1e-2px");
    }
  }
#endif

  // ==========================================================================
  // 9. availableFontFamilies() / fontFamilyAvailable(): the font-picker's
  //    query surface. Proves the list is well-formed (non-empty, filtered,
  //    sorted, de-duplicated), that fontFamilyAvailable() agrees with it in
  //    both directions, and that the list is not just names CoreText knows
  //    about but names shapeText() will actually honour.
  // ==========================================================================
  std::printf("  -- 9. availableFontFamilies() / fontFamilyAvailable() --\n");
  {
    const std::vector<std::string> families = availableFontFamilies();
    check(!families.empty(),
          "availableFontFamilies() is non-empty -- an empty result on a real Mac would mean "
          "the CTFontManager call was misused, not that the machine truly has zero fonts");

    bool ascending = true;
    for (size_t i = 1; i < families.size(); ++i)
      if (!(families[i] > families[i - 1])) ascending = false;
    check(ascending,
          "the list is strictly ascending, which proves both sorted (non-decreasing) and "
          "de-duplicated (no two equal neighbours) in one assertion");

    bool wellFormed = true;
    for (const std::string& f : families)
      if (f.empty() || f[0] == '.') wellFormed = false;
    check(wellFormed,
          "no entry is empty and none begins with '.' (CoreText's own hidden system families, "
          "e.g. \".SF NS\", \".LastResort\")");

    bool allSelfAvailable = true;
    for (const std::string& f : families)
      if (!fontFamilyAvailable(f)) allSelfAvailable = false;
    check(allSelfAvailable,
          "fontFamilyAvailable() is true for every entry the list itself returned -- catches a "
          "case-folding bug that would reject a family given back in its OWN capitalisation");

    // Taken FROM the list at runtime, not a hardcoded "Helvetica" -- a
    // hardcoded name is an assumption about this one machine's installed
    // fonts, and this property must hold on every Mac this build runs on.
    const std::string& sample = families.front();
    auto flipCase = [](const std::string& s, bool toUpper) {
      std::string out = s;
      std::transform(out.begin(), out.end(), out.begin(), [toUpper](unsigned char c) {
        return static_cast<char>(toUpper ? std::toupper(c) : std::tolower(c));
      });
      return out;
    };
    check(fontFamilyAvailable(flipCase(sample, false)),
          "fontFamilyAvailable() accepts a lower-cased form of a real family name");
    check(fontFamilyAvailable(flipCase(sample, true)),
          "fontFamilyAvailable() accepts an upper-cased form of a real family name -- catches "
          "the case-folding bug in the OTHER direction from the check above");

    // No installed font family is 34 random alphanumeric characters with no
    // spaces or punctuation -- real family names are short, human-chosen
    // words. This is the same string section 3 above already relies on to
    // prove shapeText() substitutes rather than failing on an unknown name.
    check(!fontFamilyAvailable("ThisFontDoesNotExistAnywhere12345"),
          "fontFamilyAvailable() is false for a name that cannot be an installed font family");

#if defined(__APPLE__)
    // The connection to shapeText(): a family taken from the list must be
    // the family the shaper ACTUALLY uses, not just a name CoreText happens
    // to recognise as installed. `fontsUsed` (text/Shaper.hpp's own comment)
    // records the PostScript name of the run's font, which in general is
    // NOT the family string itself -- e.g. family "Times New Roman" has
    // PostScript name "TimesNewRomanPSMT" -- so the oracle here is
    // CoreText's own CTFontCreateWithName()+CTFontCopyPostScriptName() on
    // the exact same family, independent of text/CoreTextShaper.mm's own
    // createFont()/runFontName(); the same "ask the platform a second time"
    // pattern section 8 above uses for the same reason.
    //
    // The family is chosen to have its OWN glyph for 'A' (checked directly,
    // not assumed) rather than just taking families.front(): this build's
    // font catalog can contain families with no Latin coverage at all (a
    // symbol font, a CJK-only family), and shaping "A" against one of those
    // would trigger CoreText's per-run fallback -- a real and correct
    // behaviour, but one that would fail this assertion for a reason that
    // has nothing to do with whether the requested family was honoured.
    // Scanning for Latin coverage keeps "was the family honoured" and "does
    // this font have this glyph" as the two separate questions they are.
    std::string latinSample;
    std::string expectedPostScriptName;
    for (const std::string& candidate : families) {
      CFStringRef cfName =
          CFStringCreateWithCString(kCFAllocatorDefault, candidate.c_str(), kCFStringEncodingUTF8);
      CTFontRef font = cfName ? CTFontCreateWithName(cfName, 24.0, nullptr) : nullptr;
      if (cfName) CFRelease(cfName);
      if (!font) continue;

      UniChar ch = 'A';
      CGGlyph glyph = 0;
      const bool hasGlyph = CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1) && glyph != 0;
      if (hasGlyph) {
        CFStringRef psName = CTFontCopyPostScriptName(font);
        if (psName) {
          char buf[256];
          if (CFStringGetCString(psName, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            expectedPostScriptName = buf;
            latinSample = candidate;
          }
          CFRelease(psName);
        }
      }
      CFRelease(font);
      if (!latinSample.empty()) break;
    }

    if (latinSample.empty()) {
      std::printf("  [skip] no family in availableFontFamilies() has its own 'A' glyph -- "
                  "cannot test the shapeText() connection without a font this build actually "
                  "has Latin coverage in\n");
    } else {
      TextStyle sampleStyle = style;
      sampleStyle.fontFamily = latinSample;
      const ShapedText shaped = shapeText("A", sampleStyle, pointFrame, TextAlign::Left);
      check(shaped.ok, "shapeText() succeeds with a family taken from availableFontFamilies()");
      const bool honoured = std::find(shaped.fontsUsed.begin(), shaped.fontsUsed.end(),
                                      expectedPostScriptName) != shaped.fontsUsed.end();
      check(honoured,
            "shaping \"A\" with a family from availableFontFamilies() (chosen to have its own "
            "'A' glyph, so fallback cannot confound the result) reports that exact font's "
            "PostScript name in fontsUsed -- proves the list names fonts the shaper actually "
            "uses, not just names CoreText happens to know about");
    }
#endif
  }

  return ok;
}

}  // namespace np
