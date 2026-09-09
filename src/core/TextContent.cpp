#include "core/TextContent.hpp"

#include <algorithm>
#include <cstring>

#include "core/VectorShape.hpp"

// core/TextContent -- see core/TextContent.hpp for the argument this file is
// an implementation of. Nothing here decides anything the header did not
// already decide; this is the four functions and nothing else.
namespace np {
namespace {

// --------------------------------------------------------------------------
// Hashing: the same FNV-1a mixing and float-bit-pattern rule as
// core/VectorShape.cpp's vectorContentHash(), on purpose (TextContent.hpp's
// own comment on textContentHash() says so). Duplicated rather than shared,
// because vectorContentHash()'s helpers are file-local to that translation
// unit (anonymous namespace) and this hash walks fields
// (`utf8`, `TextStyle`, `TextFrame`, `TextAlign`) that file never touches --
// a shared header for four three-line functions would cost more indirection
// than the duplication it removes.
// --------------------------------------------------------------------------
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline void hashBytes(uint64_t& h, const void* p, size_t n) noexcept {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= kFnvPrime;
  }
}

inline void hashF32(uint64_t& h, float v) noexcept {
  // By bit pattern, so the hash is exact -- see vectorContentHash()'s own
  // comment on why that is the safe direction (a denormal or signed-zero
  // difference re-rasterises rather than being hashed away).
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  hashBytes(h, &bits, sizeof(bits));
}

inline void hashU64(uint64_t& h, uint64_t v) noexcept { hashBytes(h, &v, sizeof(v)); }

inline void hashStr(uint64_t& h, const std::string& s) noexcept {
  hashU64(h, s.size());
  hashBytes(h, s.data(), s.size());
}

void hashPaint(uint64_t& h, const Paint& p) noexcept {
  hashU64(h, p.on ? 1u : 0u);
  for (float c : p.rgba) hashF32(h, c);
}

// Shift every anchor and handle of `path` by (dx, dy).
//
// This is the one translation textContentToShapes() ever applies -- origin
// plus a glyph's pen position -- and core/Path.hpp's own comment names
// core/PathTransform as the place a general affine map would live, but that
// file does not exist yet (nothing before Stage 4's manipulator needs
// scale/rotate on a `Path`). Writing a two-float shift here rather than
// standing up that file for one caller is the smaller change; when
// core/PathTransform lands for the manipulator, this becomes a one-line call
// into it instead of a private helper.
void translateInPlace(Path& path, float dx, float dy) noexcept {
  auto shift = [&](PathPoint& p) {
    p.x += dx;
    p.y += dy;
  };
  for (SubPath& sub : path.subpaths) {
    for (Anchor& a : sub.anchors) {
      shift(a.pt);
      shift(a.in);
      shift(a.out);
    }
  }
}

// Map every anchor and BOTH handles of `path` through `m`.
//
// The handles are not optional. `in`/`out` are absolute positions, not
// offsets from the anchor (core/Path.hpp), so a transform that moved only
// `pt` would leave every curve's control points behind and turn a rotated
// "S" inside out. That failure is silent on straight-edged glyphs and
// obvious on round ones, which is the worst way for it to be wrong -- hence
// one helper both callers share rather than a loop at each site.
void mapInPlace(Path& path, const Mat3& m) noexcept {
  auto map = [&](PathPoint& p) {
    const Point2 q = mat3MapPoint(m, Point2{p.x, p.y});
    p.x = q.x;
    p.y = q.y;
  };
  for (SubPath& sub : path.subpaths) {
    for (Anchor& a : sub.anchors) {
      map(a.pt);
      map(a.in);
      map(a.out);
    }
  }
}

// `transform` is identity for every block nobody has scaled or rotated, which
// is nearly all of them -- so the mapping pass is skipped rather than run as
// nine multiplies per control point. Exact equality, not a tolerance: this
// only has to catch the default-constructed case, and a matrix that is
// almost identity must still be applied or the block would drift.
bool isIdentity(const Mat3& m) noexcept {
  static const Mat3 kId = mat3Identity();
  return m.m == kId.m;
}

// `transform` mapped the other way, for turning a document-space click back
// into text space. False (leaving `*out` untouched) when the matrix is
// degenerate -- see textOffsetAtPoint()'s header.
bool inverseOf(const TextContent& text, Mat3* out) noexcept {
  if (isIdentity(text.transform)) {
    *out = mat3Identity();
    return true;
  }
  return mat3Invert(text.transform, out);
}

// Where the SHAPED block's own (0,0) sits in document space -- i.e. what to
// add to a `ShapedGlyph`'s pen position to put it on the page.
//
// This is the whole of core/TextContent.hpp section 2b in code. For point
// text `origin` is the first BASELINE's left end, so the shaped block's top
// sits an ascent ABOVE it and this subtracts that ascent. For paragraph text
// `origin` is the frame's top-left, which is already the shaped block's own
// top-left, so there is nothing to subtract.
//
// **One helper, four callers** -- the shapes, the caret, the selection quads
// and the hit test. Spelling the subtraction at each site instead is the
// silent partial fan-out this codebase keeps getting bitten by: three of the
// four agreeing and the fourth not is a caret that sits an ascent away from
// its own text, which reads as a rendering bug rather than a missed edit.
PathPoint shapedOrigin(const TextContent& text, const ShapedText& shaped) noexcept {
  if (text.frame.width > 0.0f) return text.origin;  // paragraph: origin IS the block top-left
  return PathPoint{text.origin.x, text.origin.y - shaped.firstBaselineY};
}

// Where a line with NO glyphs on it begins, in text space -- which is its
// alignment point, since there is nothing on it to align.
//
// Two callers, and they are the two states a text block spends its first
// moments in: a block just created and not yet typed into, and a block whose
// text ends in the newline you just pressed. Justified starts at the left
// edge like Left does; there is nothing to stretch on an empty line.
float emptyLineStartX(const TextContent& text) noexcept {
  if (text.frame.width <= 0.0f) return 0.0f;  // point text: no frame to align within
  if (text.align == TextAlign::Center) return text.frame.width * 0.5f;
  if (text.align == TextAlign::Right) return text.frame.width;
  return 0.0f;
}

// The conventional ascent/descent split of a line box around the baseline,
// shared by the caret and the selection highlight so the two cannot disagree
// -- core/TextContent.hpp's `TextCaretSegment` says why that matters.
constexpr float kAscentFraction = 0.8f;
constexpr float kDescentFraction = 0.2f;

}  // namespace

TextContent makeTextContent(std::string utf8, PathPoint origin) {
  TextContent t;
  t.utf8 = std::move(utf8);
  t.origin = origin;
  // Everything else the header wants ("a ready-to-use black 24 px block") is
  // already `TextStyle`/`TextFrame`/`TextAlign`'s own default (Helvetica,
  // 24px, point text, left align) -- the one field none of those defaults
  // cover is `Paint::on`, which defaults false on `Paint` itself so that a
  // `TextContent` built by aggregate initialisation paints nothing. Clicking
  // with `Tool::Text` on a blank canvas has to produce visible text, not a
  // layer that silently draws nothing until someone finds the colour picker.
  t.fill.on = true;
  t.fill.rgba = {0.0f, 0.0f, 0.0f, 1.0f};
  return t;
}

uint64_t textContentHash(const TextContent& text) noexcept {
  uint64_t h = kFnvOffset;
  hashStr(h, text.utf8);
  hashStr(h, text.style.fontFamily);
  hashF32(h, text.style.sizePx);
  hashF32(h, text.style.tracking);
  hashF32(h, text.style.leading);
  hashU64(h, text.style.bold ? 1u : 0u);
  hashU64(h, text.style.italic ? 1u : 0u);
  hashF32(h, text.frame.width);
  hashF32(h, text.frame.height);
  hashU64(h, static_cast<uint64_t>(text.align));
  hashF32(h, text.origin.x);
  hashF32(h, text.origin.y);
  // Every one of the nine, by bit pattern like every other float here. The
  // header's "every field above is in it" is not a style rule: this hash is
  // core/VectorRaster's cache key, so a matrix left out of it means rotating
  // a block redraws the block exactly as it was, which reads as the rotate
  // handle being dead rather than as a stale cache.
  for (float v : text.transform.m) hashF32(h, v);
  hashPaint(h, text.fill);
  hashPaint(h, text.stroke);
  hashF32(h, text.strokeStyle.width);
  hashU64(h, static_cast<uint64_t>(text.strokeStyle.cap));
  hashU64(h, static_cast<uint64_t>(text.strokeStyle.join));
  hashF32(h, text.strokeStyle.miterLimit);
  hashU64(h, text.strokeStyle.dashes.size());
  for (float d : text.strokeStyle.dashes) hashF32(h, d);
  hashF32(h, text.strokeStyle.dashOffset);
  return h;
}

std::vector<VectorShape> textContentToShapes(const TextContent& text, std::string* errorOut) {
  std::vector<VectorShape> shapes;

  // Empty text is a legitimate state -- the header's own "a Text layer a
  // user has just created and not yet typed into" -- and it has to read
  // that way on EVERY build, including text/StubShaper.cpp's, whose
  // `shapeText()` answers every call, empty string included, with the
  // "no shaper" refusal (it has no special case for empty input; nothing
  // downstream of `ok == false` would need one). So this is checked here,
  // before `shapeText()` is ever called, rather than trusted to it -- or an
  // empty Text layer on a non-Apple build would report an error for doing
  // nothing wrong.
  if (text.utf8.empty()) return shapes;

  const ShapedText shaped = shapeText(text.utf8, text.style, text.frame, text.align);
  if (!shaped.ok) {
    // The two real failures this function can report -- invalid UTF-8, and
    // a build with no shaper at all -- both arrive here as `shaped.ok ==
    // false`, and `shaped.error` is already worded for a user by
    // text/Shaper.hpp's own contract (it names the specific problem in
    // either case), so this is a straight pass-through rather than this
    // file inventing a second message for the same fact.
    if (errorOut) *errorOut = shaped.error;
    return shapes;
  }

  // One shape per glyph (header section on why), so `id` is assigned in
  // shaping order starting at 1 -- 0 stays reserved for "not yet assigned",
  // matching `VectorShape::id`'s own convention.
  uint64_t nextId = 1;
  for (const ShapedGlyph& g : shaped.glyphs) {
    Path path;
    if (!glyphPath(g.glyphId, text.style, &path)) {
      // glyphPath()'s own contract (text/Shaper.hpp): a missing glyph --
      // including glyphId 0, "notdef" -- is the font simply not answering,
      // not a geometry error, and a caller papering over it with an empty
      // `Path` would draw nothing and never learn why. The same reasoning
      // holds one level up: a whole STRING this font cannot provide a
      // single outline for (an emoji-only run against a font with no emoji
      // table, say) looks, from here, identical to legitimate empty text --
      // an empty vector, `errorOut` untouched -- rather than this function
      // inventing a "some glyphs were skipped" failure text/Shaper.hpp never
      // asked for and that would make an ordinary missing accent mark in an
      // otherwise-fine string report an error. A caller that needs to tell
      // the two apart already can, by comparing `shaped.glyphs.size()`
      // against the size of the vector this function returns.
      continue;
    }
    // The two origins meet here: `glyphPath()` returns the outline about the
    // glyph's own design origin on its baseline (text/Shaper.hpp), and
    // `ShapedGlyph::{x, y}` is that glyph's pen position within the shaped
    // block. `text.origin` is the block's own place in document space
    // (TextContent.hpp). All three add.
    const PathPoint blockAt = shapedOrigin(text, shaped);
    translateInPlace(path, blockAt.x + g.x, blockAt.y + g.y);
    // Section 4: `document = transform * (origin + penPosition)`. Applied
    // per glyph, after the two translations above, so the block scales and
    // rotates as one piece -- the shaping that produced `g` has already
    // happened and is not disturbed by it.
    if (!isIdentity(text.transform)) mapInPlace(path, text.transform);

    VectorShape shape;
    shape.path = std::move(path);
    shape.fill = text.fill;
    shape.stroke = text.stroke;
    shape.strokeStyle = text.strokeStyle;
    shape.id = nextId++;
    shapes.push_back(std::move(shape));
  }
  return shapes;
}

PathBounds textContentBounds(const TextContent& text) {
  // Exactly `vectorShapesBounds(textContentToShapes(text))`, per the header
  // -- which means every call SHAPES the text. That is milliseconds against
  // core/PathRaster's own cost (this header's section 1), which is fine for
  // an occasional hit-test or a layer's on-disk extent, but a caller on a
  // genuinely per-frame path (a live drag of a text block, a hover
  // highlight redrawn every frame) should cache the result keyed on
  // `textContentHash()`, the same way core/VectorRaster's cache exists so a
  // Vector layer is not re-rasterised on every composite. This function does
  // no caching of its own -- it is a pure function of `text` with nowhere to
  // keep a cache across calls without becoming stateful, and that decision
  // belongs to the caller who knows whether it is on such a path.
  return vectorShapesBounds(textContentToShapes(text));
}

bool textContentDraws(const TextContent& text) {
  // Cheap check first: most `TextContent`s that draw nothing are invisible
  // by paint (fill and stroke both off), not by having nothing to shape, and
  // this skips a shape() call -- the same cost `textContentBounds()`'s
  // comment above just described -- in that common case.
  if (!text.fill.on && !text.stroke.on) return false;
  return !textContentToShapes(text).empty();
}

// --------------------------------------------------------------------------
// The caret (header section 4)
// --------------------------------------------------------------------------

namespace {

// The line height a caret bar should be drawn at. `TextStyle::leading == 0`
// means "the font's own" (text/Shaper.hpp), which this file cannot ask the
// font for without a second platform call -- so it falls back to the size
// itself scaled by a conventional 1.2, the same ratio a typical font's
// ascent+descent+gap bears to its em size. Stated rather than silent because
// it IS an approximation: a caret one or two pixels short of the glyphs it
// sits beside is a cosmetic wrongness, and a caret derived from a number
// nobody wrote down is a mystery.
float caretHeightFor(const TextStyle& style) noexcept {
  return style.leading > 0.0f ? style.leading : style.sizePx * 1.2f;
}

}  // namespace

namespace {

// The caret's text-space pen position and the line height there, before any
// mapping. Split out because `textCaretSegment()` needs both, and keeping the
// glyph scan in one place stops the bidi rules below being re-derived.
struct CaretPen {
  PathPoint pen{0.0f, 0.0f};
  float height = 0.0f;
};

CaretPen caretPenFor(const TextContent& text, size_t caretByte) {
  CaretPen c;
  c.height = caretHeightFor(text.style);

  // An empty block has nothing to shape, and shaping an empty string would go
  // through `shapeText()`'s stub-refusal path (see `textContentToShapes()`'s
  // own comment on why that matters), so it is answered here instead.
  if (text.utf8.empty()) {
    // Point text: `origin` IS the baseline (section 2b), so the caret is
    // already in the right place and nothing needs measuring.
    c.pen = text.origin;

    // Paragraph text: `origin` is the frame's TOP-LEFT, and a caret whose
    // baseline sits on the top edge draws almost entirely ABOVE the box --
    // which is what a freshly dragged text frame looked like, until the first
    // character was typed and the type appeared a whole ascent lower down.
    //
    // The first baseline is an ascent below the frame top, and that ascent is
    // a property of the font at this size, so it is MEASURED rather than
    // guessed: one character is shaped in this block's own style and frame,
    // and its first baseline is where this block's would be. A fraction of
    // `sizePx` would be wrong by a few pixels in a way that reads as the
    // caret being misaligned with its own text.
    if (text.frame.width > 0.0f) {
      const ShapedText probe = shapeText("x", text.style, text.frame, text.align);
      if (probe.ok)
        c.pen = PathPoint{text.origin.x + emptyLineStartX(text),
                          text.origin.y + probe.firstBaselineY};
    }
    return c;
  }

  const ShapedText shaped = shapeText(text.utf8, text.style, text.frame, text.align);
  if (!shaped.ok || shaped.glyphs.empty()) {
    c.pen = text.origin;
    return c;
  }

  // The first glyph at or after the caret. Scanned rather than indexed
  // because `cluster` is NOT monotonic in glyph order -- a right-to-left run
  // walks backwards through the source bytes (text/Shaper.hpp) -- so
  // `glyphs[k]` says nothing about byte k and a binary search would be
  // searching an unsorted array.
  const ShapedGlyph* best = nullptr;
  for (const ShapedGlyph& g : shaped.glyphs) {
    if (g.cluster < caretByte) continue;
    if (best == nullptr || g.cluster < best->cluster) best = &g;
  }

  const PathPoint blockAt = shapedOrigin(text, shaped);
  if (best != nullptr) {
    c.pen = PathPoint{blockAt.x + best->x, blockAt.y + best->y};
    return c;
  }

  // Past every cluster: the caret is at the end of the text. Its x is the
  // trailing edge of the LAST GLYPH IN PEN ORDER, which is the largest x --
  // not `glyphs.back()`, which under bidi is the last glyph in logical order
  // and can sit at the left end of the line.
  //
  // **`x + advance`, not `x`.** This used to return the pen position itself,
  // because `ShapedGlyph` carried no advance -- which drew the caret in front
  // of the last character rather than after it. Since the caret is at the end
  // of the block for the whole of ordinary typing, that was not a "known
  // half-a-character offset": it was the caret being wrong on essentially
  // every keystroke. `advance` now exists on `ShapedGlyph` (text/Shaper.hpp
  // says why it lives there), and this is the reader it exists for.
  const ShapedGlyph* last = &shaped.glyphs.front();
  for (const ShapedGlyph& g : shaped.glyphs)
    if (g.y > last->y || (g.y == last->y && g.x > last->x)) last = &g;

  // --- the caret after a TRAILING newline -----------------------------------
  //
  // CoreText's framesetter does not lay out a line for a newline that ends the
  // text: "Hi\n" is ONE line, and "Hi\n\n" is two. So the glyphs stop a line
  // short of where the caret belongs, and the code above -- which can only
  // point at a glyph -- left the caret at the end of the previous line.
  //
  // What that looked like: you pressed Return and the caret did not move.
  // Pressing it again and then typing put you two lines down, because both
  // newlines were in the string all along and only the caret was lying about
  // it. The insertion point has to be the one thing that never does that.
  //
  // So the caret is placed on the line the shaper declined to produce: one
  // `lineHeightPx` below the last glyph's baseline, at the line's own start.
  // Only ONE line is added however many newlines trail, because CoreText lays
  // out every one of them except the last.
  const bool endsWithNewline = caretByte > 0 && caretByte <= text.utf8.size() &&
                               text.utf8[caretByte - 1] == '\n';
  // Point text is excluded deliberately, and not because it has no trailing
  // newline to handle: `CTLineCreateWithAttributedString` never breaks a line
  // at all, so a point block draws "Hi\nYo" as one line and a caret dropped to
  // a second one would sit under text that is not there. A newline in point
  // text is a real gap, named in core/TextContent.hpp section 2, and moving
  // the caret without moving the type would disguise it rather than fix it.
  if (endsWithNewline && text.frame.width > 0.0f) {
    c.pen = PathPoint{blockAt.x + emptyLineStartX(text),
                      blockAt.y + last->y + shaped.lineHeightPx};
    return c;
  }

  c.pen = PathPoint{blockAt.x + last->x + last->advance, blockAt.y + last->y};
  return c;
}

// One text-space point through `transform`.
PathPoint mapped(const TextContent& text, PathPoint p) noexcept {
  if (isIdentity(text.transform)) return p;
  const Point2 q = mat3MapPoint(text.transform, Point2{p.x, p.y});
  return PathPoint{q.x, q.y};
}

}  // namespace

TextCaretSegment textCaretSegment(const TextContent& text, size_t caretByte) {
  const CaretPen c = caretPenFor(text, caretByte);
  // The caret hangs from the pen position UP by the ascent and DOWN by the
  // descent, because the pen position is on the BASELINE and a bar drawn
  // downward from it would sit entirely under the text.
  //
  // Both endpoints are mapped INDIVIDUALLY rather than one being mapped and
  // the other derived by stepping down in y: under a rotation those are
  // different answers, and the derived one is the bug this segment API
  // exists to remove -- a vertical caret standing in turned text.
  TextCaretSegment seg;
  seg.top = mapped(text, PathPoint{c.pen.x, c.pen.y - c.height * kAscentFraction});
  seg.bottom = mapped(text, PathPoint{c.pen.x, c.pen.y + c.height * kDescentFraction});
  return seg;
}

std::vector<TextQuad> textSelectionQuads(const TextContent& text, size_t loByte, size_t hiByte) {
  std::vector<TextQuad> out;
  if (loByte >= hiByte || text.utf8.empty()) return out;

  const ShapedText shaped = shapeText(text.utf8, text.style, text.frame, text.align);
  if (!shaped.ok || shaped.glyphs.empty()) return out;

  const float h = caretHeightFor(text.style);

  // One accumulator per baseline. A linear scan keyed on `y` rather than a
  // map: a block has a handful of lines, and the glyphs of one line arrive
  // together, so this is a couple of comparisons per glyph.
  struct Line {
    float y = 0.0f;
    float minX = 0.0f;
    float maxX = 0.0f;
  };
  std::vector<Line> lines;
  for (const ShapedGlyph& g : shaped.glyphs) {
    if (g.cluster < loByte || g.cluster >= hiByte) continue;
    const float left = g.x;
    const float right = g.x + g.advance;  // the trailing edge -- see the header
    Line* line = nullptr;
    for (Line& l : lines)
      if (l.y == g.y) {
        line = &l;
        break;
      }
    if (line == nullptr) {
      lines.push_back(Line{g.y, left, right});
    } else {
      line->minX = std::min(line->minX, left);
      line->maxX = std::max(line->maxX, right);
    }
  }

  out.reserve(lines.size());
  for (const Line& l : lines) {
    const PathPoint blockAt = shapedOrigin(text, shaped);
    const float x0 = blockAt.x + l.minX;
    const float x1 = blockAt.x + l.maxX;
    // The same split the caret uses, from the same two constants.
    const float y0 = blockAt.y + l.y - h * kAscentFraction;
    const float y1 = blockAt.y + l.y + h * kDescentFraction;
    TextQuad q;
    q.corner[0] = mapped(text, PathPoint{x0, y0});
    q.corner[1] = mapped(text, PathPoint{x1, y0});
    q.corner[2] = mapped(text, PathPoint{x1, y1});
    q.corner[3] = mapped(text, PathPoint{x0, y1});
    out.push_back(q);
  }
  return out;
}

bool textFrameQuad(const TextContent& text, TextQuad* out) {
  if (out == nullptr) return false;

  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  if (text.frame.width > 0.0f) {
    // Paragraph text: the frame the user set, in text space, so it does not
    // breathe as lines wrap. `frame.height == 0` means "as tall as the lines
    // need" (header section 2), which has to be ASKED rather than drawn as a
    // zero-height line -- and asked in TEXT space, which is why this reads
    // the untransformed ink rather than `textContentBounds()`.
    TextContent flat = text;
    flat.transform = mat3Identity();
    const PathBounds ink = textContentBounds(flat);
    x0 = text.origin.x;
    y0 = text.origin.y;
    x1 = text.origin.x + text.frame.width;
    y1 = text.frame.height > 0.0f ? text.origin.y + text.frame.height
                                  : (ink.valid ? ink.maxY : text.origin.y);
  } else {
    TextContent flat = text;
    flat.transform = mat3Identity();
    const PathBounds ink = textContentBounds(flat);
    if (!ink.valid) return false;
    x0 = ink.minX;
    y0 = ink.minY;
    x1 = ink.maxX;
    y1 = ink.maxY;
  }

  out->corner[0] = mapped(text, PathPoint{x0, y0});
  out->corner[1] = mapped(text, PathPoint{x1, y0});
  out->corner[2] = mapped(text, PathPoint{x1, y1});
  out->corner[3] = mapped(text, PathPoint{x0, y1});
  return true;
}

size_t textOffsetAtPoint(const TextContent& text, PathPoint at) {
  if (text.utf8.empty()) return 0;
  const ShapedText shaped = shapeText(text.utf8, text.style, text.frame, text.align);
  if (!shaped.ok || shaped.glyphs.empty()) return 0;

  // Back into text space before anything is compared. Everything below is
  // written against the shaped pen positions, which are text-space, so this
  // one map is the whole of what `transform` costs the hit test -- and
  // without it a click on rotated type selects whatever character happens to
  // sit at the same place in the UNROTATED block, which is the kind of wrong
  // that feels like the tool ignoring the mouse.
  Mat3 inv;
  if (!inverseOf(text, &inv)) return 0;  // degenerate: header section 5
  const Point2 local = mat3MapPoint(inv, Point2{at.x, at.y});
  at = PathPoint{local.x, local.y};

  // Nearest pen position, with the LINE weighted far more heavily than the
  // column: a click below the last line of a paragraph must land at the end
  // of that line, not at whichever glyph happens to be horizontally closest
  // on the line above. The weight is a plain factor rather than a
  // line-height-relative one, because it only has to make vertical distance
  // dominate and the two axes are already in the same unit (text-space px).
  const PathPoint blockAt = shapedOrigin(text, shaped);
  const float lx = at.x - blockAt.x;
  const float ly = at.y - blockAt.y;

  const ShapedGlyph* best = nullptr;
  float bestScore = 0.0f;
  for (const ShapedGlyph& g : shaped.glyphs) {
    const float dx = g.x - lx;
    const float dy = (g.y - ly) * 4.0f;  // a line's worth of error beats a column's
    const float score = dx * dx + dy * dy;
    if (best == nullptr || score < bestScore) {
      best = &g;
      bestScore = score;
    }
  }
  if (best == nullptr) return 0;

  // A click past the MIDDLE of the nearest glyph belongs after it, not on it
  // -- without this, clicking anywhere past the last character puts the caret
  // before it and typing inserts in the wrong place, which is the single most
  // noticeable caret bug there is. `nextCluster` walks the glyph list rather
  // than adding a byte, so the result stays on a UTF-8 boundary.
  //
  // The midpoint rather than the leading edge (`lx <= best->x`, which is what
  // this tested before `ShapedGlyph::advance` existed): with the leading
  // edge, a click one pixel inside a character already counted as "after" it,
  // so the caret could only ever be placed before a character by clicking in
  // the character to its left. Half the glyph box each way is what every text
  // editor does and what a drag-select needs to feel right.
  if (lx <= best->x + best->advance * 0.5f) return best->cluster;
  size_t next = text.utf8.size();
  for (const ShapedGlyph& g : shaped.glyphs)
    if (g.cluster > best->cluster && g.cluster < next) next = g.cluster;
  return next;
}

}  // namespace np
