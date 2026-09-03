#include "io/TextSerial.hpp"

#include <cstring>
#include <utility>

namespace np {
namespace {

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void putU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFFu));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// The bit pattern, not a decimal rendering -- io/PathSerial's and
// io/OpSerial's reason, and it applies here too: `style.sizePx`, `tracking`,
// `leading` and `origin` all feed text/Shaper.hpp positions directly, so an
// ulp of drift per save/load cycle would eventually re-wrap a paragraph for
// no authored reason.
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}

// `n` is `size_t` so the two call sites below can each cap it to their own
// length-prefix width before calling.
void putBytes(std::vector<uint8_t>& b, const std::string& s, size_t n) {
  b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void putString16(std::vector<uint8_t>& b, const std::string& s) {
  const size_t n = s.size() > 0xFFFFu ? 0xFFFFu : s.size();
  putU16(b, static_cast<uint16_t>(n));
  putBytes(b, s, n);
}

// `utf8` gets a wider, u32 length prefix than `fontFamily`'s u16 -- see
// io/TextSerial.hpp's format note: a font family name is a handful of words,
// but a text block's own content is exactly what a user might paste a page
// of, and 65 535 bytes is a paragraph and a half of plain ASCII.
void putString32(std::vector<uint8_t>& b, const std::string& s) {
  const size_t n = s.size() > 0xFFFFFFFFu ? 0xFFFFFFFFu : s.size();
  putU32(b, static_cast<uint32_t>(n));
  putBytes(b, s, n);
}

void putPoint(std::vector<uint8_t>& b, const PathPoint& p) {
  putF32(b, p.x);
  putF32(b, p.y);
}

void putPaint(std::vector<uint8_t>& b, const Paint& p) {
  putU8(b, p.on ? 1u : 0u);
  for (float c : p.rgba) putF32(b, c);
}

// io/PathSerial.cpp's cursor, verbatim: every read is bounds-checked and sets
// `bad` rather than throwing, so one `if (r.bad)` covers every truncation.
struct Reader {
  const uint8_t* p = nullptr;
  size_t left = 0;
  bool bad = false;

  uint8_t u8() {
    if (left < 1) {
      bad = true;
      return 0;
    }
    --left;
    return *p++;
  }
  uint16_t u16() {
    const uint8_t a = u8(), b = u8();
    return static_cast<uint16_t>(a | (static_cast<uint16_t>(b) << 8));
  }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
    return v;
  }
  float f32() {
    const uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  // Bounded by the bytes that actually remain, checked BEFORE the string is
  // constructed -- the allocation-bomb guard io/TextSerial.hpp's header
  // promises. A declared length past what remains fails here rather than
  // reserving it.
  std::string str16() {
    const uint16_t n = u16();
    if (bad) return {};
    if (static_cast<size_t>(n) > left) {
      bad = true;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    left -= n;
    return s;
  }
  std::string str32() {
    const uint32_t n = u32();
    if (bad) return {};
    if (static_cast<size_t>(n) > left) {
      bad = true;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    left -= n;
    return s;
  }
  PathPoint point() {
    PathPoint q;
    q.x = f32();
    q.y = f32();
    return q;
  }
  Paint paint() {
    Paint pt;
    pt.on = u8() != 0;
    for (float& c : pt.rgba) c = f32();
    return pt;
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // tolerated on read, never written
  return -1;
}

}  // namespace

std::string serializeTextContent(const TextContent& text) {
  std::vector<uint8_t> payload;
  putString32(payload, text.utf8);
  putString16(payload, text.style.fontFamily);
  putF32(payload, text.style.sizePx);
  putF32(payload, text.style.tracking);
  putF32(payload, text.style.leading);
  putU8(payload, text.style.bold ? 1u : 0u);
  putU8(payload, text.style.italic ? 1u : 0u);

  putF32(payload, text.frame.width);
  putF32(payload, text.frame.height);

  // Explicit literals, not `static_cast<uint8_t>(text.align)` left implicit
  // in a comment somewhere -- see io/TextSerial.hpp's header: the numbers on
  // disk must not move if `text/Shaper.hpp`'s enum is ever reordered.
  uint8_t alignCode = 0;
  switch (text.align) {
    case TextAlign::Left: alignCode = 0; break;
    case TextAlign::Center: alignCode = 1; break;
    case TextAlign::Right: alignCode = 2; break;
    case TextAlign::Justified: alignCode = 3; break;
  }
  putU8(payload, alignCode);

  putPoint(payload, text.origin);
  putPaint(payload, text.fill);
  putPaint(payload, text.stroke);

  putF32(payload, text.strokeStyle.width);
  putU8(payload, static_cast<uint8_t>(text.strokeStyle.cap));
  putU8(payload, static_cast<uint8_t>(text.strokeStyle.join));
  putF32(payload, text.strokeStyle.miterLimit);
  const size_t dashCount = text.strokeStyle.dashes.size();
  putU16(payload, static_cast<uint16_t>(dashCount > 0xFFFFu ? 0xFFFFu : dashCount));
  for (size_t i = 0; i < dashCount && i <= 0xFFFFu; ++i) putF32(payload, text.strokeStyle.dashes[i]);
  putF32(payload, text.strokeStyle.dashOffset);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kTextContentSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeTextContent(std::string_view value, TextContent* textOut, std::string* errorOut) {
  auto fail = [&](std::string why) {
    if (errorOut) *errorOut = std::move(why);
    return false;
  };
  if (textOut == nullptr) return fail("no destination for the decoded text content.");

  const std::string_view prefix(kTextContentSerialPrefix);
  // The version is read before a byte is decoded. See the header: this is
  // what makes a newer document survive an older build unaltered.
  if (value.size() < prefix.size() || value.substr(0, prefix.size()) != prefix)
    return fail("np:text does not begin with '" + std::string(prefix) +
                "' -- this build cannot read that version, and the attribute is carried "
                "through unchanged rather than being reinterpreted.");

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0)
    return fail("np:text payload has an odd number of hex digits (" +
                std::to_string(hex.size()) + "), so it is truncated.");

  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0)
      return fail("np:text payload has a non-hex character at offset " + std::to_string(i) + ".");
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{bytes.data(), bytes.size(), false};
  TextContent t;
  t.utf8 = r.str32();
  if (r.bad)
    return fail("np:text declares a utf8 length past its remaining " +
                std::to_string(r.left) + " bytes.");
  t.style.fontFamily = r.str16();
  if (r.bad)
    return fail("np:text declares a font-family length past its remaining " +
                std::to_string(r.left) + " bytes.");
  t.style.sizePx = r.f32();
  t.style.tracking = r.f32();
  t.style.leading = r.f32();
  t.style.bold = r.u8() != 0;
  t.style.italic = r.u8() != 0;
  if (r.bad) return fail("np:text payload is truncated in its style block.");

  t.frame.width = r.f32();
  t.frame.height = r.f32();
  if (r.bad) return fail("np:text payload is truncated in its frame.");

  const uint8_t alignCode = r.u8();
  if (r.bad) return fail("np:text payload is truncated before its align byte.");
  // An align value this build does not know is refused OUTRIGHT, matching
  // io/PathSerial.cpp's `readPath()` treatment of an out-of-range `FillRule`/
  // `LineCap`/`LineJoin`: a payload is homogeneous, so there is no per-field
  // carry-forward to fall back to (io/TextSerial.hpp's header explains why
  // building one would not be worth it for a single enum), and clamping to
  // `Left` would silently re-align a paragraph the user set some other way.
  switch (alignCode) {
    case 0: t.align = TextAlign::Left; break;
    case 1: t.align = TextAlign::Center; break;
    case 2: t.align = TextAlign::Right; break;
    case 3: t.align = TextAlign::Justified; break;
    default:
      return fail("np:text declares align code " + std::to_string(alignCode) +
                  ", which this build does not recognise.");
  }

  t.origin = r.point();
  t.fill = r.paint();
  t.stroke = r.paint();
  if (r.bad) return fail("np:text payload is truncated in its origin/fill/stroke block.");

  t.strokeStyle.width = r.f32();
  const uint8_t cap = r.u8();
  const uint8_t join = r.u8();
  if (r.bad) return fail("np:text payload is truncated in its stroke style.");
  if (cap > static_cast<uint8_t>(LineCap::Square))
    return fail("np:text declares stroke cap code " + std::to_string(cap) +
                ", which this build does not recognise.");
  if (join > static_cast<uint8_t>(LineJoin::Bevel))
    return fail("np:text declares stroke join code " + std::to_string(join) +
                ", which this build does not recognise.");
  t.strokeStyle.cap = static_cast<LineCap>(cap);
  t.strokeStyle.join = static_cast<LineJoin>(join);
  t.strokeStyle.miterLimit = r.f32();

  const uint16_t dashCount = r.u16();
  if (r.bad) return fail("np:text payload is truncated before its dash count.");
  // Bounded by the bytes that remain before anything is reserved -- the same
  // rule io/PathSerial.cpp applies to a subpath's anchor count.
  if (static_cast<size_t>(dashCount) * 4u > r.left)
    return fail("np:text declares " + std::to_string(dashCount) +
                " dashes, which cannot fit in its remaining " + std::to_string(r.left) +
                " bytes.");
  t.strokeStyle.dashes.reserve(dashCount);
  for (uint16_t i = 0; i < dashCount; ++i) t.strokeStyle.dashes.push_back(r.f32());
  t.strokeStyle.dashOffset = r.f32();
  if (r.bad) return fail("np:text payload is truncated in its dash list.");

  // The exact-length rule: the parse must have consumed the payload and
  // nothing less. Trailing bytes mean the writer knew something this reader
  // does not, and guessing is what PRD I10 forbids.
  if (r.left != 0)
    return fail("np:text has " + std::to_string(r.left) +
                " trailing bytes after its declared fields.");

  *textOut = std::move(t);
  return true;
}

}  // namespace np
