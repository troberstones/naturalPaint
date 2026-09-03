#include "text/Shaper.hpp"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

// text/CoreTextShaper -- the only translation unit in this tree allowed to
// name a CoreText type (text/Shaper.hpp's header comment explains why the
// boundary is drawn there).
//
// ==========================================================================
// ARC is on; Core Foundation is not
// ==========================================================================
//
// `-fobjc-arc` (set by src/CMakeLists.txt on this file, same as ui/
// MacNativeMenu.mm) manages Objective-C object lifetimes automatically, but
// every `CTFontRef`, `CFStringRef`, `CFAttributedStringRef`, `CGPathRef` and
// friends here is a Core Foundation type, and ARC does not touch those --
// `CFRelease`/`CGPathRelease`/`CFRelease` is this file's own job, once per
// `Create`/`Copy` function it calls (the "Create Rule"). A leak here would
// not show up in --selftest, which shapes a handful of short strings once;
// it would show up as unbounded growth in a document somebody keeps typing
// into, which is exactly the failure mode the header comment on
// `shaperAvailable()` warns future maintainers to instrument for.
//
// ==========================================================================
// UTF-16 is CoreText's alphabet; UTF-8 byte offsets are this file's contract
// ==========================================================================
//
// A `CFString` is UTF-16 internally, so `CTRunGetStringIndices()` returns
// indices into that UTF-16 buffer -- NOT byte offsets into the `utf8` this
// file was handed. Returning those indices unchanged would satisfy every
// assertion written against ASCII (where the two coincide) and silently
// break on the first accented or CJK character, which is why
// app/selftest/TextShaper.cpp shapes "h\xC3\xA9llo" and a CJK string rather
// than only ASCII. `Utf16ToUtf8Map` below is the one piece of code in this
// file that re-decodes the source UTF-8 rather than delegating to CoreText,
// built once per `shapeText()` call and used to translate every cluster
// index CoreText hands back.
namespace np {
namespace {

// Maps a UTF-16 code-unit index (as `CTRunGetStringIndices()` returns) to
// the UTF-8 byte offset of the character that unit belongs to. Index
// `utf16Length` (one past the end) maps to `utf8.size()`, so a caller never
// needs a separate bounds check for "the cluster after the last glyph".
//
// Only reached after `CFStringCreateWithBytes()` has already accepted
// `utf8` as valid UTF-8 (see `shapeText()`), so the decode below assumes
// well-formed input and does not need its own malformed-sequence recovery --
// io/SvgImport-style leniency belongs to a file that parses untrusted files,
// and this one only ever seconds CoreText's own validation.
struct Utf16ToUtf8Map {
  std::vector<size_t> offsetForUnit;

  explicit Utf16ToUtf8Map(std::string_view utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
      const unsigned char lead = static_cast<unsigned char>(utf8[i]);
      size_t seqLen;
      uint32_t cp;
      if ((lead & 0x80) == 0x00) { seqLen = 1; cp = lead; }
      else if ((lead & 0xE0) == 0xC0) { seqLen = 2; cp = lead & 0x1Fu; }
      else if ((lead & 0xF0) == 0xE0) { seqLen = 3; cp = lead & 0x0Fu; }
      else if ((lead & 0xF8) == 0xF0) { seqLen = 4; cp = lead & 0x07u; }
      else { seqLen = 1; cp = 0xFFFDu; }
      seqLen = std::min(seqLen, utf8.size() - i);
      for (size_t k = 1; k < seqLen; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
      const size_t units = (cp > 0xFFFFu) ? 2 : 1;  // a surrogate pair for astral code points
      for (size_t u = 0; u < units; ++u) offsetForUnit.push_back(i);
      i += seqLen;
    }
    offsetForUnit.push_back(utf8.size());
  }

  size_t byteOffset(CFIndex utf16Index) const {
    if (utf16Index < 0) return 0;
    const size_t idx = static_cast<size_t>(utf16Index);
    return idx < offsetForUnit.size() ? offsetForUnit[idx] : utf8_size();
  }

 private:
  size_t utf8_size() const { return offsetForUnit.empty() ? 0 : offsetForUnit.back(); }
};

// Builds the font for a style. Bold/italic go through CoreText's symbolic-
// trait API rather than a name like "Helvetica-Bold", because not every
// family has that exact PostScript name -- going through traits degrades to
// the family's regular face when no bold/italic face exists, instead of
// silently returning a non-bold font under a name CoreText could not find.
//
// A `fontFamily` CoreText does not recognise is not an error here:
// `CTFontCreateWithName()` substitutes the platform default rather than
// returning NULL, which is exactly `shapeText()`'s documented "does not
// fail, `fontsUsed` says what happened instead" contract.
CTFontRef createFont(const TextStyle& style) {
  CFStringRef familyName = CFStringCreateWithCString(
      kCFAllocatorDefault, style.fontFamily.c_str(), kCFStringEncodingUTF8);
  CTFontRef base = CTFontCreateWithName(familyName, style.sizePx, nullptr);
  if (familyName) CFRelease(familyName);
  if (!base) return nullptr;

  CTFontSymbolicTraits traits = 0;
  if (style.bold) traits |= kCTFontTraitBold;
  if (style.italic) traits |= kCTFontTraitItalic;
  if (traits == 0) return base;

  CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
      base, style.sizePx, nullptr, traits, traits);
  if (!styled) return base;  // no matching bold/italic face -- keep the regular one
  CFRelease(base);
  return styled;
}

CTTextAlignment mapAlign(TextAlign align) {
  // No `default:` -- see src/CMakeLists.txt's `-Werror=switch` comment for
  // why that is deliberate here: a fifth `TextAlign` must fail to compile
  // this function rather than silently falling back to `Left`.
  switch (align) {
    case TextAlign::Left: return kCTTextAlignmentLeft;
    case TextAlign::Center: return kCTTextAlignmentCenter;
    case TextAlign::Right: return kCTTextAlignmentRight;
    case TextAlign::Justified: return kCTTextAlignmentJustified;
  }
  return kCTTextAlignmentLeft;
}

// One attributed string, one font attribute (over the whole range -- PRD
// K2's "rich-text runs" is explicitly out of scope, see text/Shaper.hpp),
// optional tracking, and -- only for paragraph text -- an alignment and
// line-height paragraph style.
//
// **`tracking == 0.0f` deliberately does not set `kCTKernAttributeName` at
// all.** CoreText treats an explicit `0.0` there as "disable kerning
// entirely", not "zero extra tracking on top of normal kerning" -- setting
// it unconditionally would make every default-tracked string lose its
// font's own kerning pairs. Leaving the attribute absent is how "the font's
// default kerning, no extra tracking" is actually expressed.
CFMutableAttributedStringRef makeAttributedString(CFStringRef str, CTFontRef font,
                                                  const TextStyle& style,
                                                  bool paragraph, TextAlign align) {
  CFMutableAttributedStringRef attr = CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);
  CFAttributedStringReplaceString(attr, CFRangeMake(0, 0), str);
  const CFRange whole = CFRangeMake(0, CFStringGetLength(str));
  CFAttributedStringSetAttribute(attr, whole, kCTFontAttributeName, font);

  if (style.tracking != 0.0f) {
    CFNumberRef tracking = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &style.tracking);
    CFAttributedStringSetAttribute(attr, whole, kCTKernAttributeName, tracking);
    CFRelease(tracking);
  }

  if (paragraph) {
    CTTextAlignment ctAlign = mapAlign(align);
    CTLineBreakMode breakMode = kCTLineBreakByWordWrapping;
    std::vector<CTParagraphStyleSetting> settings = {
        {kCTParagraphStyleSpecifierAlignment, sizeof(ctAlign), &ctAlign},
        {kCTParagraphStyleSpecifierLineBreakMode, sizeof(breakMode), &breakMode},
    };
    // `leading == 0` means "the font's own" (text/Shaper.hpp), so the
    // fixed-line-height settings are only added when the caller asked to
    // override it -- otherwise CTParagraphStyle's own defaults (the font's
    // ascent + descent + leading metrics) apply, unmodified.
    //
    // `leadingValue` is a `CGFloat`, not `style.leading`'s own `float`,
    // and that is not a style nit: CoreText reads exactly `sizeof(CGFloat)`
    // bytes (8, on every Mac this targets) out of the pointer this struct
    // carries, so handing it a 4-byte `float` and `sizeof(float)` would have
    // it read four bytes of whatever happens to sit past `style.leading` on
    // the stack as the top half of its line height.
    const CGFloat leadingValue = style.leading;
    if (style.leading > 0.0f) {
      settings.push_back({kCTParagraphStyleSpecifierMinimumLineHeight,
                          sizeof(leadingValue), &leadingValue});
      settings.push_back({kCTParagraphStyleSpecifierMaximumLineHeight,
                          sizeof(leadingValue), &leadingValue});
    }
    CTParagraphStyleRef paraStyle =
        CTParagraphStyleCreate(settings.data(), settings.size());
    CFAttributedStringSetAttribute(attr, whole, kCTParagraphStyleAttributeName, paraStyle);
    CFRelease(paraStyle);
  }

  return attr;
}

// The PostScript name of the font actually used to draw one run -- after
// fallback, which is why this is read back off the run's own attributes
// rather than off the `TextStyle` the caller passed in.
std::string runFontName(CTRunRef run) {
  CFDictionaryRef attrs = CTRunGetAttributes(run);
  CTFontRef runFont = attrs ? static_cast<CTFontRef>(
      const_cast<void*>(CFDictionaryGetValue(attrs, kCTFontAttributeName))) : nullptr;
  if (!runFont) return {};
  CFStringRef name = CTFontCopyPostScriptName(runFont);
  if (!name) return {};
  char buf[256];
  std::string result;
  if (CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8)) result = buf;
  CFRelease(name);
  return result;
}

void addFontUsed(std::vector<std::string>* fontsUsed, const std::string& name) {
  if (name.empty()) return;
  if (std::find(fontsUsed->begin(), fontsUsed->end(), name) == fontsUsed->end())
    fontsUsed->push_back(name);
}

// Appends every glyph of one run, translating CoreText's y-up run-local
// offsets into this file's y-down, block-relative convention in one place
// (see text/Shaper.hpp's header comment): `lineBaselineYDown` already
// carries the flip for the line's own origin, so only the run's per-glyph
// offset (usually 0 -- non-zero only for super/subscript-style baseline
// shifts) needs negating here.
void appendRunGlyphs(CTRunRef run, double lineOriginX, double lineBaselineYDown,
                     const Utf16ToUtf8Map& clusterMap, ShapedText* out) {
  const CFIndex count = CTRunGetGlyphCount(run);
  if (count <= 0) return;
  std::vector<CGGlyph> glyphs(static_cast<size_t>(count));
  std::vector<CGPoint> positions(static_cast<size_t>(count));
  std::vector<CFIndex> indices(static_cast<size_t>(count));
  CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs.data());
  CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
  CTRunGetStringIndices(run, CFRangeMake(0, 0), indices.data());

  addFontUsed(&out->fontsUsed, runFontName(run));

  for (CFIndex i = 0; i < count; ++i) {
    ShapedGlyph g;
    g.glyphId = glyphs[static_cast<size_t>(i)];
    g.x = static_cast<float>(lineOriginX + positions[static_cast<size_t>(i)].x);
    g.y = static_cast<float>(lineBaselineYDown - positions[static_cast<size_t>(i)].y);
    g.cluster = static_cast<uint32_t>(clusterMap.byteOffset(indices[static_cast<size_t>(i)]));
    out->glyphs.push_back(g);
  }
}

}  // namespace

bool shaperAvailable() noexcept { return true; }

const char* shaperUnavailableReason() noexcept { return ""; }

ShapedText shapeText(std::string_view utf8, const TextStyle& style,
                     const TextFrame& frame, TextAlign align) {
  ShapedText out;

  // Defined, not an error: text/Shaper.hpp's own contract for the empty
  // string -- nothing shaped, nothing to report a font or a line for.
  if (utf8.empty()) {
    out.ok = true;
    return out;
  }

  CFStringRef str = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
      static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
  if (!str) {
    out.ok = false;
    out.error = "input is not valid UTF-8";
    return out;
  }

  CTFontRef font = createFont(style);
  if (!font) {
    CFRelease(str);
    out.ok = false;
    out.error = "CoreText could not create a font";
    return out;
  }

  const bool paragraph = frame.width > 0.0f;
  CFMutableAttributedStringRef attr = makeAttributedString(str, font, style, paragraph, align);
  CFRelease(font);
  CFRelease(str);

  const Utf16ToUtf8Map clusterMap(utf8);
  CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attr);

  if (!paragraph) {
    // Point text: one CTLine, no wrapping box, no alignment (there is
    // nothing to align a single line against -- text/Shaper.hpp's own
    // comment on `TextFrame`).
    CTLineRef line = CTLineCreateWithAttributedString(attr);
    CGFloat ascent = 0, descent = 0, leadingOut = 0;
    const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leadingOut);

    // A single line has no second line to space against, so `style.leading`
    // (a line-to-line override) does not apply here -- the font's own
    // metrics are the only defined answer, matching the header comment on
    // `TextFrame`.
    const double baselineYDown = ascent;
    out.heightPx = static_cast<float>(ascent + descent + leadingOut);
    out.widthPx = static_cast<float>(width);
    out.lineCount = 1;

    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
      CTRunRef run = static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs, r)));
      appendRunGlyphs(run, 0.0, baselineYDown, clusterMap, &out);
    }
    CFRelease(line);
  } else {
    // Paragraph text: let the framesetter say how tall the text needs to be
    // when the caller did not pin a height, so `H` below is the real
    // content height rather than an arbitrarily oversized box whose slack
    // would otherwise leak into every line's y-down origin (see
    // text/Shaper.hpp's header comment on the coordinate space).
    CFRange fitRange;
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter, CFRangeMake(0, 0), nullptr,
        CGSizeMake(frame.width, CGFLOAT_MAX), &fitRange);
    const double H = frame.height > 0.0f
                         ? static_cast<double>(frame.height)
                         : std::max(1.0, std::ceil(suggested.height));

    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, nullptr, CGRectMake(0, 0, frame.width, H));
    CTFrameRef ctFrame = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
    CGPathRelease(path);

    CFArrayRef lines = CTFrameGetLines(ctFrame);
    const CFIndex lineCount = CFArrayGetCount(lines);
    std::vector<CGPoint> origins(static_cast<size_t>(std::max<CFIndex>(lineCount, 0)));
    if (lineCount > 0) CTFrameGetLineOrigins(ctFrame, CFRangeMake(0, 0), origins.data());

    out.lineCount = static_cast<int>(lineCount);
    out.widthPx = static_cast<float>(frame.width);
    out.heightPx = static_cast<float>(H);

    for (CFIndex li = 0; li < lineCount; ++li) {
      CTLineRef line = static_cast<CTLineRef>(const_cast<void*>(CFArrayGetValueAtIndex(lines, li)));
      const CGPoint origin = origins[static_cast<size_t>(li)];
      // The one flip in this whole file (see text/Shaper.hpp's header
      // comment): CTFrame's path is y-up with (0,0) at its own lower-left
      // corner, and this expresses every line's baseline as a distance DOWN
      // from the top of that same box.
      const double baselineYDown = H - origin.y;
      CFArrayRef runs = CTLineGetGlyphRuns(line);
      for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
        CTRunRef run = static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs, r)));
        appendRunGlyphs(run, origin.x, baselineYDown, clusterMap, &out);
      }
    }
    CFRelease(ctFrame);
  }

  CFRelease(framesetter);
  CFRelease(attr);
  out.ok = true;
  return out;
}

bool glyphPath(uint32_t glyphId, const TextStyle& style, Path* out) {
  // Glyph 0 is CoreText's own "notdef"/missing-glyph sentinel -- refused
  // rather than drawn, per text/Shaper.hpp's comment on why tofu is not a
  // geometry answer. A `glyphId` past `CGGlyph`'s 16-bit range cannot have
  // come from this file's own `shapeText()`, so it is refused the same way.
  if (glyphId == 0 || glyphId > 0xFFFFu) return false;

  CTFontRef font = createFont(style);
  if (!font) return false;
  CGPathRef cgPath = CTFontCreatePathForGlyph(font, static_cast<CGGlyph>(glyphId), nullptr);
  CFRelease(font);
  if (!cgPath) return false;

  // `__block` on `result` itself, not just the scalars below: an Objective-C
  // block captures an outer automatic variable BY VALUE, as a const copy,
  // unless it is marked `__block` -- so without this, every mutation inside
  // `CGPathApplyWithBlock`'s block below would land on a throwaway copy of
  // `result` and `*out` would come back empty. The three scalars need the
  // same annotation for the same reason.
  __block Path result;
  result.rule = FillRule::NonZero;  // core/Path.hpp: TrueType/CFF outlines rely on nonzero

  // Mirrors io/SvgPath.cpp's `appendLine()`/`appendCubic()` pair exactly --
  // a fresh anchor always starts with `in == out == pt` (a straight line to
  // whatever comes next, until a curve overrides the PREVIOUS anchor's
  // `out`), so a run of `AddLineToPoint` elements needs no per-call fixup.
  __block bool hasOpenSubpath = false;
  __block PathPoint currentPoint{0.0f, 0.0f};
  __block PathPoint subpathStart{0.0f, 0.0f};

  CGPathApplyWithBlock(cgPath, ^(const CGPathElement* element) {
    // Flips CoreText's y-up glyph space into this file's y-down convention
    // (text/Shaper.hpp's comment on `glyphPath()`) -- the only sign change
    // in this whole function, applied once per point rather than once per
    // caller.
    auto flip = [](CGPoint p) { return PathPoint{static_cast<float>(p.x), static_cast<float>(-p.y)}; };
    switch (element->type) {
      case kCGPathElementMoveToPoint: {
        const PathPoint p = flip(element->points[0]);
        SubPath sub;
        Anchor a;
        a.pt = a.in = a.out = p;
        sub.anchors.push_back(a);
        result.subpaths.push_back(std::move(sub));
        currentPoint = p;
        subpathStart = p;
        hasOpenSubpath = true;
        break;
      }
      case kCGPathElementAddLineToPoint: {
        if (!hasOpenSubpath) break;  // a malformed path from the platform; refuse to guess
        const PathPoint p = flip(element->points[0]);
        Anchor a;
        a.pt = a.in = a.out = p;
        result.subpaths.back().anchors.push_back(a);
        currentPoint = p;
        break;
      }
      case kCGPathElementAddQuadCurveToPoint: {
        if (!hasOpenSubpath) break;
        const PathPoint q = flip(element->points[0]);
        const PathPoint end = flip(element->points[1]);
        const PathPoint p0 = currentPoint;
        // text/Shaper.hpp's stated elevation: c1 = p0 + (2/3)(q - p0),
        // c2 = p2 + (2/3)(q - p2).
        const PathPoint c1{p0.x + (2.0f / 3.0f) * (q.x - p0.x), p0.y + (2.0f / 3.0f) * (q.y - p0.y)};
        const PathPoint c2{end.x + (2.0f / 3.0f) * (q.x - end.x), end.y + (2.0f / 3.0f) * (q.y - end.y)};
        result.subpaths.back().anchors.back().out = c1;
        Anchor a;
        a.pt = end; a.in = c2; a.out = end;
        result.subpaths.back().anchors.push_back(a);
        currentPoint = end;
        break;
      }
      case kCGPathElementAddCurveToPoint: {
        if (!hasOpenSubpath) break;
        const PathPoint c1 = flip(element->points[0]);
        const PathPoint c2 = flip(element->points[1]);
        const PathPoint end = flip(element->points[2]);
        result.subpaths.back().anchors.back().out = c1;
        Anchor a;
        a.pt = end; a.in = c2; a.out = end;
        result.subpaths.back().anchors.push_back(a);
        currentPoint = end;
        break;
      }
      case kCGPathElementCloseSubpath: {
        if (!hasOpenSubpath) break;
        result.subpaths.back().closed = true;
        currentPoint = subpathStart;
        hasOpenSubpath = false;
        break;
      }
    }
  });

  CGPathRelease(cgPath);
  *out = std::move(result);
  return true;
}

}  // namespace np
