#include "core/TextContent.hpp"

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
    translateInPlace(path, text.origin.x + g.x, text.origin.y + g.y);

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

}  // namespace np
