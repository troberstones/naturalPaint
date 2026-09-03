#include "io/PathSerial.hpp"

#include <cstring>
#include <utility>

namespace np {
namespace {

// The smallest a shape record can be: reserved + id + empty name + two paints
// + stroke style + no dashes + no pivot + empty path + no clip.
constexpr size_t kMinShapeRecordBytes = 1 + 8 + 2;

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void putU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFFu));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

void putU64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// The bit pattern, not a decimal rendering -- io/OpSerial's and
// io/CompSerial's reason, and it bites harder here: an anchor that shifted by
// one ulp per save/load cycle would visibly drift a shape over a session.
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}

void putString(std::vector<uint8_t>& b, const std::string& s) {
  const uint16_t n = static_cast<uint16_t>(s.size() > 0xFFFFu ? 0xFFFFu : s.size());
  putU16(b, n);
  b.insert(b.end(), s.begin(), s.begin() + n);
}

void putPoint(std::vector<uint8_t>& b, const PathPoint& p) {
  putF32(b, p.x);
  putF32(b, p.y);
}

void putPaint(std::vector<uint8_t>& b, const Paint& p) {
  putU8(b, p.on ? 1u : 0u);
  for (float c : p.rgba) putF32(b, c);
}

void putPath(std::vector<uint8_t>& b, const Path& path) {
  putU8(b, static_cast<uint8_t>(path.rule));
  const size_t n = path.subpaths.size();
  putU16(b, static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n));
  for (size_t i = 0; i < n && i <= 0xFFFFu; ++i) {
    const SubPath& sub = path.subpaths[i];
    putU8(b, sub.closed ? 1u : 0u);
    const size_t m = sub.anchors.size();
    putU32(b, static_cast<uint32_t>(m));
    for (const Anchor& a : sub.anchors) {
      putPoint(b, a.pt);
      putPoint(b, a.in);
      putPoint(b, a.out);
      putU8(b, a.smooth ? 1u : 0u);
    }
  }
}

// io/CompSerial's cursor, verbatim in shape: every read is bounds-checked and
// sets `bad` rather than throwing, so one `if (r.bad)` covers every truncation.
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
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
    return v;
  }
  float f32() {
    const uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  std::string str() {
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
  PathPoint point() {
    PathPoint q;
    q.x = f32();
    q.y = f32();
    return q;
  }
  Paint paint() {
    Paint p;
    p.on = u8() != 0;
    for (float& c : p.rgba) c = f32();
    return p;
  }
};

// The per-anchor cost on the wire: 6 floats plus the smooth flag.
constexpr size_t kAnchorBytes = 6 * 4 + 1;

bool readPath(Reader& r, Path* out) {
  const uint8_t rule = r.u8();
  if (r.bad) return false;
  // An unknown rule is refused rather than clamped to NonZero: a shape filled
  // by a rule this build does not have is not "approximately" this shape.
  if (rule > static_cast<uint8_t>(FillRule::EvenOdd)) return false;
  out->rule = static_cast<FillRule>(rule);

  const uint16_t subCount = r.u16();
  if (r.bad) return false;
  // Bounded by what actually remains: the smallest subpath is 1 + 4 = 5 bytes.
  if (static_cast<size_t>(subCount) * 5u > r.left) return false;
  out->subpaths.reserve(subCount);
  for (uint16_t i = 0; i < subCount; ++i) {
    SubPath sub;
    sub.closed = r.u8() != 0;
    const uint32_t anchorCount = r.u32();
    if (r.bad) return false;
    // The count that matters for a hostile file: a u32 anchor count could
    // claim four billion anchors, so it is bounded by the bytes left before
    // a single one is reserved.
    if (static_cast<size_t>(anchorCount) * kAnchorBytes > r.left) return false;
    sub.anchors.reserve(anchorCount);
    for (uint32_t k = 0; k < anchorCount; ++k) {
      Anchor a;
      a.pt = r.point();
      a.in = r.point();
      a.out = r.point();
      a.smooth = r.u8() != 0;
      if (r.bad) return false;
      sub.anchors.push_back(a);
    }
    out->subpaths.push_back(std::move(sub));
  }
  return !r.bad;
}

bool parseShapeRecord(const uint8_t* body, size_t length, VectorShape* out) {
  if (length < kMinShapeRecordBytes) return false;
  Reader r{body, length, false};
  // A byte this build writes as 0 that came back non-zero means a newer build
  // gave it a meaning; interpreting the rest would assume that meaning does
  // not change what the rest says. io/CompSerial's rule, verbatim.
  if (r.u8() != 0) return false;

  VectorShape s;
  s.id = r.u64();
  s.name = r.str();
  s.fill = r.paint();
  s.stroke = r.paint();

  s.strokeStyle.width = r.f32();
  const uint8_t cap = r.u8();
  const uint8_t join = r.u8();
  if (r.bad) return false;
  if (cap > static_cast<uint8_t>(LineCap::Square)) return false;
  if (join > static_cast<uint8_t>(LineJoin::Bevel)) return false;
  s.strokeStyle.cap = static_cast<LineCap>(cap);
  s.strokeStyle.join = static_cast<LineJoin>(join);
  s.strokeStyle.miterLimit = r.f32();
  const uint16_t dashCount = r.u16();
  if (r.bad) return false;
  if (static_cast<size_t>(dashCount) * 4u > r.left) return false;
  s.strokeStyle.dashes.reserve(dashCount);
  for (uint16_t i = 0; i < dashCount; ++i) s.strokeStyle.dashes.push_back(r.f32());
  s.strokeStyle.dashOffset = r.f32();

  const uint8_t hasPivot = r.u8();
  if (r.bad) return false;
  if (hasPivot) s.pivot = r.point();

  if (!readPath(r, &s.path)) return false;

  const uint8_t hasClip = r.u8();
  if (r.bad) return false;
  if (hasClip) {
    Path clip;
    if (!readPath(r, &clip)) return false;
    s.clip = std::move(clip);
  }

  // The exact-length rule: the parse must have consumed the record and nothing
  // less. Trailing bytes mean the writer knew something this reader does not,
  // and guessing is what PRD I10 forbids.
  if (r.bad || r.left != 0) return false;
  *out = std::move(s);
  return true;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // tolerated on read, never written
  return -1;
}

}  // namespace

std::string serializeVectorShapes(const std::vector<VectorShape>& shapes,
                                  uint64_t nextShapeId) {
  std::vector<uint8_t> payload;
  putU64(payload, nextShapeId);

  const size_t count = shapes.size();
  putU32(payload, static_cast<uint32_t>(count));
  for (const VectorShape& s : shapes) {
    std::vector<uint8_t> body;
    putU8(body, 0u);  // reserved
    putU64(body, s.id);
    putString(body, s.name);
    putPaint(body, s.fill);
    putPaint(body, s.stroke);

    putF32(body, s.strokeStyle.width);
    putU8(body, static_cast<uint8_t>(s.strokeStyle.cap));
    putU8(body, static_cast<uint8_t>(s.strokeStyle.join));
    putF32(body, s.strokeStyle.miterLimit);
    const size_t dashCount = s.strokeStyle.dashes.size();
    putU16(body, static_cast<uint16_t>(dashCount > 0xFFFFu ? 0xFFFFu : dashCount));
    for (size_t i = 0; i < dashCount && i <= 0xFFFFu; ++i)
      putF32(body, s.strokeStyle.dashes[i]);
    putF32(body, s.strokeStyle.dashOffset);

    putU8(body, s.pivot.has_value() ? 1u : 0u);
    if (s.pivot) putPoint(body, *s.pivot);

    putPath(body, s.path);

    putU8(body, s.clip.has_value() ? 1u : 0u);
    if (s.clip) putPath(body, *s.clip);

    // Length prefix recomputed rather than stored, so a record can never come
    // back with a length that disagrees with its own contents.
    putU32(payload, static_cast<uint32_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kVectorShapeSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeVectorShapes(std::string_view value, std::vector<VectorShape>* shapesOut,
                             uint64_t* nextShapeIdOut, std::string* errorOut) {
  auto fail = [&](std::string why) {
    if (errorOut) *errorOut = std::move(why);
    return false;
  };
  if (shapesOut == nullptr) return fail("no destination for the decoded shapes.");

  const std::string_view prefix(kVectorShapeSerialPrefix);
  // The version is read before a byte is decoded. See the header: this is what
  // makes a newer document survive an older build unaltered.
  if (value.size() < prefix.size() || value.substr(0, prefix.size()) != prefix)
    return fail("np:vector does not begin with '" + std::string(prefix) +
                "' -- this build cannot read that version, and the attribute is carried "
                "through unchanged rather than being reinterpreted.");

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0)
    return fail("np:vector payload has an odd number of hex digits (" +
                std::to_string(hex.size()) + "), so it is truncated.");

  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0)
      return fail("np:vector payload has a non-hex character at offset " +
                  std::to_string(i) + ".");
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{bytes.data(), bytes.size(), false};
  const uint64_t nextShapeId = r.u64();
  const uint32_t count = r.u32();
  if (r.bad) return fail("np:vector payload is truncated before its shape count.");
  // Bounded by the bytes that remain before anything is reserved.
  if (static_cast<size_t>(count) * (4u + kMinShapeRecordBytes) > r.left)
    return fail("np:vector declares " + std::to_string(count) +
                " shapes, which cannot fit in its remaining " + std::to_string(r.left) +
                " bytes.");

  std::vector<VectorShape> shapes;
  shapes.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t bodyLen = r.u32();
    if (r.bad || bodyLen > r.left)
      return fail("np:vector shape " + std::to_string(i) + " declares " +
                  std::to_string(bodyLen) + " bytes, past the end of the payload.");
    VectorShape s;
    if (!parseShapeRecord(r.p, bodyLen, &s))
      return fail("np:vector shape " + std::to_string(i) +
                  " could not be decoded -- either it carries a reserved byte this build "
                  "does not understand, or its own declared lengths do not add up.");
    r.p += bodyLen;
    r.left -= bodyLen;
    shapes.push_back(std::move(s));
  }
  if (r.left != 0)
    return fail("np:vector has " + std::to_string(r.left) +
                " trailing bytes after its declared shape count.");

  *shapesOut = std::move(shapes);
  if (nextShapeIdOut) *nextShapeIdOut = nextShapeId;
  return true;
}

}  // namespace np
