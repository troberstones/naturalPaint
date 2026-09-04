#include "io/FlatsSerial.hpp"

#include <cstring>
#include <vector>

namespace np {
namespace {

constexpr const char* kPrefix = "npflats1:";
// The largest count any list may declare. A well-formed file from this build
// never approaches it (a drawing has hundreds of edits, not millions); a
// corrupt or hostile one that declares more is refused before any
// allocation, which is the whole point of the cap.
constexpr uint32_t kMaxCount = 1u << 20;
constexpr uint32_t kMaxPolyFloats = 1u << 22;

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
void putI32(std::vector<uint8_t>& b, int32_t v) { putU32(b, static_cast<uint32_t>(v)); }
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}
void putString(std::vector<uint8_t>& b, const std::string& s) {
  const size_t n = s.size() > 0xFFFFFFFFu ? 0xFFFFFFFFu : s.size();
  putU32(b, static_cast<uint32_t>(n));
  b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
void putPoly(std::vector<uint8_t>& b, const FlatPolyline& p) {
  putU32(b, static_cast<uint32_t>(p.size()));
  for (const float v : p) putF32(b, v);
}
void putRgb(std::vector<uint8_t>& b, FlatRgb c) {
  putU8(b, c[0]);
  putU8(b, c[1]);
  putU8(b, c[2]);
}

struct Reader {
  const std::vector<uint8_t>& b;
  size_t at = 0;
  std::string* err;
  bool ok = true;
  bool need(size_t n, const char* what) {
    if (!ok) return false;
    if (at + n > b.size()) {
      ok = false;
      if (err) *err = std::string("np:flats payload is truncated while reading ") + what + ".";
      return false;
    }
    return true;
  }
  uint8_t u8(const char* what) {
    if (!need(1, what)) return 0;
    return b[at++];
  }
  uint32_t u32(const char* what) {
    if (!need(4, what)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[at + i]) << (8 * i);
    at += 4;
    return v;
  }
  int32_t i32(const char* what) { return static_cast<int32_t>(u32(what)); }
  float f32(const char* what) {
    const uint32_t bits = u32(what);
    float v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  uint32_t count(const char* what) {
    const uint32_t n = u32(what);
    if (ok && n > kMaxCount) {
      ok = false;
      if (err) *err = std::string("np:flats declares ") + std::to_string(n) + " " + what + ", which is not plausible.";
      return 0;
    }
    return n;
  }
  std::string str(const char* what) {
    const uint32_t n = u32(what);
    if (!ok) return {};
    if (!need(n, what)) return {};
    std::string s(reinterpret_cast<const char*>(b.data() + at), n);
    at += n;
    return s;
  }
  FlatPolyline poly(const char* what) {
    const uint32_t n = u32(what);
    if (ok && n > kMaxPolyFloats) {
      ok = false;
      if (err) *err = std::string("np:flats declares a ") + std::to_string(n) + "-float polyline for " + what + ", which is not plausible.";
      return {};
    }
    if (!need(static_cast<size_t>(n) * 4, what)) return {};
    FlatPolyline p(n);
    for (uint32_t i = 0; i < n; i++) p[i] = f32(what);
    return p;
  }
  FlatRgb rgb(const char* what) {
    FlatRgb c{};
    c[0] = u8(what);
    c[1] = u8(what);
    c[2] = u8(what);
    return c;
  }
};

}  // namespace

std::string serializeFlatsContent(const FlatsContent& c) {
  std::vector<uint8_t> b;
  const FlatParams& p = c.params;
  putF32(b, p.lineThreshold);
  putF32(b, p.colourReject);
  putI32(b, p.smoothing);
  putU8(b, p.skeletonize);
  putI32(b, p.gapSize);
  putF32(b, p.sheet);
  putU8(b, p.closeTightGaps);
  putI32(b, p.minRegion);
  putI32(b, p.sliverWidth);
  putI32(b, p.declutter);
  putU8(b, p.autoMergeLeaks);
  putI32(b, p.paletteSize);
  putU8(b, p.completionField);

  const FlatEdits& e = c.edits;
  putU32(b, e.nextId);
  putU32(b, e.nextGroup);
  putU32(b, static_cast<uint32_t>(e.bridges.size()));
  for (const auto& s : e.bridges) { putU32(b, s.id); putPoly(b, s.pts); putU8(b, s.erase); }
  putU32(b, static_cast<uint32_t>(e.carves.size()));
  for (const auto& v : e.carves) { putU32(b, v.id); putF32(b, v.x); putF32(b, v.y); }
  putU32(b, static_cast<uint32_t>(e.mergeStrokes.size()));
  for (const auto& m : e.mergeStrokes) { putU32(b, m.id); putPoly(b, m.pts); }
  putU32(b, static_cast<uint32_t>(e.mergePairs.size()));
  for (const auto& m : e.mergePairs) { putU32(b, m.id); putF32(b, m.ax); putF32(b, m.ay); putF32(b, m.bx); putF32(b, m.by); }
  putU32(b, static_cast<uint32_t>(e.deleteMarks.size()));
  for (const auto& d : e.deleteMarks) { putU32(b, d.id); putF32(b, d.x); putF32(b, d.y); }
  putU32(b, static_cast<uint32_t>(e.shapeFills.size()));
  for (const auto& s : e.shapeFills) { putU32(b, s.id); putPoly(b, s.pts); putRgb(b, s.color); putString(b, s.name); }
  putU32(b, static_cast<uint32_t>(e.groups.size()));
  for (const auto& g : e.groups) { putU32(b, g.id); putString(b, g.name); putPoly(b, g.path); }
  putU32(b, static_cast<uint32_t>(e.recolors.size()));
  for (const auto& r : e.recolors) { putU32(b, r.id); putF32(b, r.x); putF32(b, r.y); putI32(b, r.slot); putRgb(b, r.color); }
  putU32(b, static_cast<uint32_t>(e.notes.size()));
  for (const auto& n : e.notes) { putU32(b, n.id); putF32(b, n.x); putF32(b, n.y); putString(b, n.name); putU8(b, n.visible); }

  putU32(b, static_cast<uint32_t>(c.palette.size()));
  for (const auto& sw : c.palette) {
    putU8(b, sw.has_value());
    putRgb(b, sw.value_or(FlatRgb{}));
  }

  static const char* hex = "0123456789abcdef";
  std::string out = kPrefix;
  out.reserve(out.size() + b.size() * 2);
  for (const uint8_t v : b) {
    out.push_back(hex[v >> 4]);
    out.push_back(hex[v & 15]);
  }
  return out;
}

bool deserializeFlatsContent(std::string_view value, FlatsContent* contentOut, std::string* errorOut) {
  const std::string_view prefix(kPrefix);
  if (value.substr(0, prefix.size()) != prefix) {
    if (errorOut)
      *errorOut = "np:flats does not start with '" + std::string(kPrefix) +
                  "'; a newer carrier version, or not a flats payload at all. The attribute is carried "
                  "unchanged and the layer opened with default content (PRD I10).";
    return false;
  }
  const std::string_view hexBody = value.substr(prefix.size());
  if (hexBody.size() % 2 != 0) {
    if (errorOut) *errorOut = "np:flats has an odd-length hex payload.";
    return false;
  }
  std::vector<uint8_t> bytes;
  bytes.reserve(hexBody.size() / 2);
  auto nib = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < hexBody.size(); i += 2) {
    const int hi = nib(hexBody[i]), lo = nib(hexBody[i + 1]);
    if (hi < 0 || lo < 0) {
      if (errorOut) *errorOut = "np:flats contains a character that is not hex.";
      return false;
    }
    bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
  }

  Reader r{bytes, 0, errorOut};
  FlatsContent c;
  FlatParams& p = c.params;
  p.lineThreshold = r.f32("lineThreshold");
  p.colourReject = r.f32("colourReject");
  p.smoothing = r.i32("smoothing");
  p.skeletonize = r.u8("skeletonize") != 0;
  p.gapSize = r.i32("gapSize");
  p.sheet = r.f32("sheet");
  p.closeTightGaps = r.u8("closeTightGaps") != 0;
  p.minRegion = r.i32("minRegion");
  p.sliverWidth = r.i32("sliverWidth");
  p.declutter = r.i32("declutter");
  p.autoMergeLeaks = r.u8("autoMergeLeaks") != 0;
  p.paletteSize = r.i32("paletteSize");
  p.completionField = r.u8("completionField") != 0;

  FlatEdits& e = c.edits;
  e.nextId = r.u32("nextId");
  e.nextGroup = r.u32("nextGroup");
  for (uint32_t n = r.count("bridges"), i = 0; r.ok && i < n; i++) {
    FlatBridgeStroke s;
    s.id = r.u32("bridge id");
    s.pts = r.poly("bridge");
    s.erase = r.u8("bridge erase") != 0;
    e.bridges.push_back(std::move(s));
  }
  for (uint32_t n = r.count("carves"), i = 0; r.ok && i < n; i++) {
    FlatCarve v;
    v.id = r.u32("carve id");
    v.x = r.f32("carve x");
    v.y = r.f32("carve y");
    e.carves.push_back(v);
  }
  for (uint32_t n = r.count("merge strokes"), i = 0; r.ok && i < n; i++) {
    FlatMergeStroke m;
    m.id = r.u32("merge stroke id");
    m.pts = r.poly("merge stroke");
    e.mergeStrokes.push_back(std::move(m));
  }
  for (uint32_t n = r.count("merge pairs"), i = 0; r.ok && i < n; i++) {
    FlatMergePair m;
    m.id = r.u32("merge pair id");
    m.ax = r.f32("merge pair"); m.ay = r.f32("merge pair"); m.bx = r.f32("merge pair"); m.by = r.f32("merge pair");
    e.mergePairs.push_back(m);
  }
  for (uint32_t n = r.count("delete marks"), i = 0; r.ok && i < n; i++) {
    FlatDeleteMark d;
    d.id = r.u32("delete id");
    d.x = r.f32("delete x");
    d.y = r.f32("delete y");
    e.deleteMarks.push_back(d);
  }
  for (uint32_t n = r.count("shape fills"), i = 0; r.ok && i < n; i++) {
    FlatShapeFill s;
    s.id = r.u32("shape id");
    s.pts = r.poly("shape");
    s.color = r.rgb("shape colour");
    s.name = r.str("shape name");
    e.shapeFills.push_back(std::move(s));
  }
  for (uint32_t n = r.count("groups"), i = 0; r.ok && i < n; i++) {
    FlatGroup g;
    g.id = r.u32("group id");
    g.name = r.str("group name");
    g.path = r.poly("group path");
    e.groups.push_back(std::move(g));
  }
  for (uint32_t n = r.count("recolours"), i = 0; r.ok && i < n; i++) {
    FlatRecolor rc;
    rc.id = r.u32("recolour id");
    rc.x = r.f32("recolour x");
    rc.y = r.f32("recolour y");
    rc.slot = r.i32("recolour slot");
    rc.color = r.rgb("recolour colour");
    e.recolors.push_back(rc);
  }
  for (uint32_t n = r.count("notes"), i = 0; r.ok && i < n; i++) {
    FlatFillNote nt;
    nt.id = r.u32("note id");
    nt.x = r.f32("note x");
    nt.y = r.f32("note y");
    nt.name = r.str("note name");
    nt.visible = r.u8("note visible") != 0;
    e.notes.push_back(std::move(nt));
  }
  for (uint32_t n = r.count("palette swatches"), i = 0; r.ok && i < n; i++) {
    const bool has = r.u8("swatch presence") != 0;
    const FlatRgb col = r.rgb("swatch");
    c.palette.push_back(has ? std::optional<FlatRgb>(col) : std::nullopt);
  }
  if (!r.ok) return false;
  if (r.at != bytes.size()) {
    if (errorOut) *errorOut = "np:flats has trailing bytes after its last field.";
    return false;
  }
  *contentOut = std::move(c);
  return true;
}

}  // namespace np
