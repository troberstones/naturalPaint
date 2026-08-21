#include "io/CompSerial.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace np {
namespace {

// Every comp record body starts with the reserved byte.
constexpr size_t kMinRecordBytes = 1 + 2 + 2;  // reserved, nameLength, entryCount

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

// The bit pattern, not a decimal rendering -- io/OpSerial's reason applies
// unchanged: reopening a document must give back the opacity that was
// captured, exactly rather than nearly.
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}

// Capped at what the length field can hold. Nothing in this codebase produces a
// 64 KiB layer name or comp name, and truncating silently is the one thing this
// module exists to prevent -- so the cap is stated here and the reader's
// exact-length rule is what asserts it.
void putString(std::vector<uint8_t>& b, const std::string& s) {
  const uint16_t n = static_cast<uint16_t>(s.size() > 0xFFFFu ? 0xFFFFu : s.size());
  putU16(b, n);
  b.insert(b.end(), s.begin(), s.begin() + n);
}

// io/OpSerial's cursor, verbatim in shape: every read is bounds-checked and
// sets `bad` rather than throwing, so one `if (r.bad)` at the end of a parse
// covers every truncation in it.
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
    // Bounded by what is actually left, so a corrupt length cannot make this
    // reserve 64 KiB before failing.
    if (static_cast<size_t>(n) > left) {
      bad = true;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    left -= n;
    return s;
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // tolerated on read, never written
  return -1;
}

// One comp record body, appended to `body`. Never called for an unrecognised
// comp -- those bytes are emitted verbatim by the caller.
void writeRecord(std::vector<uint8_t>& body, const LayerComp& comp) {
  body.push_back(0u);  // reserved
  putString(body, comp.name);
  const size_t n = comp.layers.size();
  putU16(body, static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n));
  for (size_t i = 0; i < n && i <= 0xFFFFu; ++i) {
    const LayerCompEntry& e = comp.layers[i];
    putU64(body, e.layerId);
    body.push_back(e.visible ? 1u : 0u);
    body.push_back(e.clipped ? 1u : 0u);
    putF32(body, e.opacity);
    putString(body, e.blend);
    putString(body, e.nameAtCapture);
  }
}

// Turns one record body into a comp. Returns false when this build cannot
// interpret it, in which case the caller keeps the bytes verbatim -- that is
// not an error, it is PRD I10 (see CompSerial.hpp).
bool parseRecord(const uint8_t* body, size_t length, LayerComp* out) {
  if (length < kMinRecordBytes) return false;
  Reader r{body, length, false};
  // A byte this build writes as 0 that came back non-zero means a newer build
  // gave it a meaning. Interpreting the rest would be assuming that meaning
  // does not change what the rest says.
  if (r.u8() != 0) return false;

  LayerComp comp;
  comp.name = r.str();
  const uint16_t entryCount = r.u16();
  if (r.bad) return false;
  // Bounded by what is left: the smallest possible entry is 8 + 1 + 1 + 4 + 2
  // + 2 = 18 bytes, so a corrupt count cannot make this reserve gigabytes.
  if (static_cast<size_t>(entryCount) * 18u > r.left) return false;
  comp.layers.reserve(entryCount);
  for (uint16_t i = 0; i < entryCount; ++i) {
    LayerCompEntry e;
    e.layerId = r.u64();
    e.visible = r.u8() != 0;
    e.clipped = r.u8() != 0;
    e.opacity = r.f32();
    e.blend = r.str();
    e.nameAtCapture = r.str();
    if (r.bad) return false;
    comp.layers.push_back(std::move(e));
  }
  // The exact-length rule: the parse must have consumed the record and nothing
  // less. See CompSerial.hpp on why "exactly" and never "at least".
  if (r.bad || r.left != 0) return false;
  *out = std::move(comp);
  return true;
}

}  // namespace

std::string serializeLayerComps(const LayerCompCarrier& in) {
  std::vector<uint8_t> payload;
  putU64(payload, in.nextLayerId);

  const size_t layerCount = in.layerIds.size();
  putU16(payload, static_cast<uint16_t>(layerCount > 0xFFFFu ? 0xFFFFu : layerCount));
  for (size_t i = 0; i < layerCount && i <= 0xFFFFu; ++i) {
    putString(payload, in.layerIds[i].first);
    putU64(payload, in.layerIds[i].second);
  }

  const size_t compCount = in.comps.size();
  putU16(payload, static_cast<uint16_t>(compCount > 0xFFFFu ? 0xFFFFu : compCount));
  for (size_t i = 0; i < compCount && i <= 0xFFFFu; ++i) {
    const LayerComp& comp = in.comps[i];
    std::vector<uint8_t> body;
    if (!comp.known) {
      // PRD I10, the write half: the bytes go back out exactly as they came in,
      // in the position they came in at. The length prefix is recomputed rather
      // than stored, so a record cannot come back with a length that disagrees
      // with its own contents.
      body = comp.unrecognised;
    } else {
      writeRecord(body, comp);
    }
    putU32(payload, static_cast<uint32_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kLayerCompSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeLayerComps(std::string_view value, LayerCompCarrier* out, std::string* errorOut) {
  auto bail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    return false;
  };
  if (out == nullptr) return bail("internal: deserializeLayerComps() called with no destination.");

  const std::string_view prefix(kLayerCompSerialPrefix);
  if (value.size() < prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
    // Named rather than "malformed": the overwhelmingly likely cause is a newer
    // format version, and a reader that says which one it saw is what makes
    // that diagnosable from the file alone.
    const std::string_view seen = value.substr(0, std::min<size_t>(value.size(), 18));
    return bail("layer comps refused: the value begins \"" + std::string(seen) + "\", not \"" +
                std::string(prefix) +
                "\". The version tag is the prefix precisely so a build decides whether it "
                "understands the encoding before decoding a byte of it; this build reads "
                "version 1 only. The attribute is preserved verbatim and written back "
                "unchanged (PRD I10).");
  }

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0) {
    return bail("layer comps refused: the payload is " + std::to_string(hex.size()) +
                " hex characters, an odd number, so it does not describe whole bytes.");
  }
  std::vector<uint8_t> payload;
  payload.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return bail("layer comps refused: '" + std::string(1, hex[hi < 0 ? i : i + 1]) +
                  "' at payload offset " + std::to_string(hi < 0 ? i : i + 1) +
                  " is not a hex digit.");
    }
    payload.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{payload.data(), payload.size(), false};
  LayerCompCarrier carrier;
  carrier.nextLayerId = r.u64();
  const uint16_t layerCount = r.u16();
  if (r.bad) {
    return bail("layer comps refused: the payload is too short to hold even its header (the "
                "id counter and the layer count).");
  }
  for (uint16_t i = 0; i < layerCount; ++i) {
    std::string name = r.str();
    const uint64_t id = r.u64();
    if (r.bad) {
      return bail("layer comps refused: the layer id table declares " +
                  std::to_string(layerCount) + " entries but runs out of payload at entry " +
                  std::to_string(i) + ". Nothing was decoded.");
    }
    carrier.layerIds.emplace_back(std::move(name), id);
  }

  const uint16_t compCount = r.u16();
  if (r.bad) return bail("layer comps refused: the payload ends before its comp count.");
  for (uint16_t i = 0; i < compCount; ++i) {
    const uint32_t bodyLength = r.u32();
    if (r.bad || bodyLength > r.left) {
      return bail("layer comps refused: comp " + std::to_string(i) + " of " +
                  std::to_string(compCount) + " declares a " + std::to_string(bodyLength) +
                  "-byte record but only " + std::to_string(r.left) +
                  " bytes remain. The list is truncated; nothing was decoded.");
    }
    LayerComp comp;
    if (parseRecord(r.p, bodyLength, &comp)) {
      carrier.comps.push_back(std::move(comp));
    } else {
      // PRD I10 at the record level. Kept whole, in position, inert.
      LayerComp unknown;
      unknown.known = false;
      unknown.unrecognised.assign(r.p, r.p + bodyLength);
      carrier.comps.push_back(std::move(unknown));
    }
    r.p += bodyLength;
    r.left -= bodyLength;
  }
  if (r.left != 0) {
    return bail("layer comps refused: " + std::to_string(r.left) + " byte(s) follow the " +
                std::to_string(compCount) +
                " comps the header declares. A payload with something after the last record is "
                "one this build does not understand the framing of, and reading the records "
                "anyway would be claiming it does.");
  }

  *out = std::move(carrier);
  return true;
}

}  // namespace np
