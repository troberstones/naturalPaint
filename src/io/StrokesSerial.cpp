#include "io/StrokesSerial.hpp"

#include <cstring>
#include <vector>

namespace np {
namespace {

// The two versions this build reads (io/StrokesSerial.hpp): `npdabs2:` adds
// one f32, `DabRecord::edgePx`, after `angle` in every record; `npdabs1:`
// has none and reads back as `edgePx == 0`.
constexpr const char* kPrefixV1 = "npdabs1:";
constexpr const char* kPrefixV2 = "npdabs2:";

// The largest dab count a payload may declare. io/FlatsSerial's cap and its
// reason: a well-formed file from this build never approaches it (a drawing
// is hundreds of thousands of dabs at the very most, and one dab is 73 bytes
// on disk -- 69 in `npdabs1` -- so a million is already a 73 MB attribute),
// and a corrupt or
// hostile one that declares more is refused BEFORE any allocation. That
// ordering is the whole point -- a four-byte count is all it takes to ask for
// a terabyte otherwise.
constexpr uint32_t kMaxDabs = 1u << 22;

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFull));
}
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
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
      if (err) *err = std::string("np:dabs payload is truncated while reading ") + what + ".";
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
  uint64_t u64(const char* what) {
    if (!need(8, what)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[at + i]) << (8 * i);
    at += 8;
    return v;
  }
  float f32(const char* what) {
    const uint32_t bits = u32(what);
    float v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
};

}  // namespace

std::string serializeStrokesContent(const StrokesContent& c) {
  // The header's version rule: `npdabs1` exactly when it is lossless -- every
  // record's `edgePx` is +0.0f, the value a v1 payload reads back as --
  // and `npdabs2` otherwise. Compared as a BIT PATTERN, not with `== 0.0f`:
  // `-0.0f` compares equal to `0.0f` but would come back as `+0.0f`, and
  // this carrier's whole promise is bit-exact floats.
  bool needsV2 = false;
  for (const DabRecord& d : c.dabs) {
    uint32_t bits = 0;
    std::memcpy(&bits, &d.edgePx, sizeof(bits));
    if (bits != 0u) {
      needsV2 = true;
      break;
    }
  }

  std::vector<uint8_t> b;
  putU64(b, c.nextDabId);
  putU32(b, static_cast<uint32_t>(c.dabs.size()));
  for (const DabRecord& d : c.dabs) {
    putU64(b, d.id);
    putU64(b, d.strokeId);
    putF32(b, d.x);
    putF32(b, d.y);
    putF32(b, d.radius);
    putF32(b, d.hardness);
    putF32(b, d.roundness);
    putF32(b, d.angle);
    if (needsV2) putF32(b, d.edgePx);
    putF32(b, d.flow);
    for (const float ch : d.rgba) putF32(b, ch);
    // One byte for the source enum rather than the enum's own width: the set
    // has two members and a file must not depend on an `enum class`'s
    // implementation-defined underlying type.
    putU8(b, static_cast<uint8_t>(d.source));
    putF32(b, d.sourceDx);
    putF32(b, d.sourceDy);
  }

  static const char* hex = "0123456789abcdef";
  std::string out = needsV2 ? kPrefixV2 : kPrefixV1;
  out.reserve(out.size() + b.size() * 2);
  for (const uint8_t v : b) {
    out.push_back(hex[v >> 4]);
    out.push_back(hex[v & 15]);
  }
  return out;
}

bool deserializeStrokesContent(std::string_view value, StrokesContent* contentOut,
                               std::string* errorOut) {
  // The version is read before a single byte is decoded (the header's rule),
  // and it selects the one framing difference: whether a record carries
  // `edgePx`. Both prefixes are the same length.
  const std::string_view v1(kPrefixV1);
  const std::string_view v2(kPrefixV2);
  const bool isV2 = value.substr(0, v2.size()) == v2;
  if (!isV2 && value.substr(0, v1.size()) != v1) {
    if (errorOut)
      *errorOut = "np:dabs does not start with '" + std::string(kPrefixV1) + "' or '" +
                  std::string(kPrefixV2) +
                  "', the two versions this build reads; a newer carrier version, or not a dab "
                  "payload at all. The attribute is carried unchanged and the layer opened "
                  "with no dabs (PRD I10).";
    return false;
  }
  const std::string_view hexBody = value.substr(isV2 ? v2.size() : v1.size());
  if (hexBody.size() % 2 != 0) {
    if (errorOut) *errorOut = "np:dabs has an odd-length hex payload.";
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
      if (errorOut) *errorOut = "np:dabs contains a character that is not hex.";
      return false;
    }
    bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
  }

  Reader r{bytes, 0, errorOut};
  StrokesContent c;
  c.nextDabId = r.u64("the dab id allocator");
  const uint32_t n = r.u32("the dab count");
  if (r.ok && n > kMaxDabs) {
    if (errorOut)
      *errorOut = "np:dabs declares " + std::to_string(n) +
                  " dab records, which is not plausible; nothing was allocated.";
    return false;
  }
  if (!r.ok) return false;
  c.dabs.reserve(n);
  for (uint32_t i = 0; r.ok && i < n; ++i) {
    DabRecord d;
    d.id = r.u64("a dab id");
    d.strokeId = r.u64("a dab's stroke id");
    d.x = r.f32("a dab x");
    d.y = r.f32("a dab y");
    d.radius = r.f32("a dab radius");
    d.hardness = r.f32("a dab hardness");
    d.roundness = r.f32("a dab roundness");
    d.angle = r.f32("a dab angle");
    // `npdabs1` predates the field: those records were painted, and are
    // replayed, with NO antialiasing floor -- the hard rim of the build that
    // wrote them -- not with `DabRecord`'s in-memory default of 1.
    d.edgePx = isV2 ? r.f32("a dab edge width") : 0.0f;
    d.flow = r.f32("a dab flow");
    for (float& ch : d.rgba) ch = r.f32("a dab colour channel");
    // An unrecognised source byte reads as `Ink`, which is the value that
    // makes the dab draw its OWN colour. The alternative -- reading it as one
    // of the below-sampling policies -- would make a dab from a newer build
    // reproduce whatever happens to lie under it at an offset it may not
    // carry, which is a picture nobody authored. Not a refusal, because one
    // unknown enumerator is exactly what PRD I10's "carry what you cannot
    // interpret" is for and the rest of the record is perfectly readable.
    //
    // **Every known value is listed, and a new one has to be added here.**
    // `DabColorSource::BelowHealed` (a recorded heal) is the second entry;
    // leaving it out would have made every recorded heal reopen as an
    // ordinary painted dab drawing its default black `rgba`, which is a
    // silent, total loss of the record's meaning on the round trip that
    // exists to preserve it.
    const uint8_t src = r.u8("a dab source");
    d.source = src == static_cast<uint8_t>(DabColorSource::Below) ? DabColorSource::Below
               : src == static_cast<uint8_t>(DabColorSource::BelowHealed)
                   ? DabColorSource::BelowHealed
                   : DabColorSource::Ink;
    d.sourceDx = r.f32("a dab source dx");
    d.sourceDy = r.f32("a dab source dy");
    if (r.ok) c.dabs.push_back(d);
  }
  if (!r.ok) return false;
  if (r.at != bytes.size()) {
    if (errorOut) *errorOut = "np:dabs has trailing bytes after its last dab record.";
    return false;
  }
  *contentOut = std::move(c);
  return true;
}

}  // namespace np
