#include "io/RegionSerial.hpp"

#include <algorithm>
#include <cstring>

namespace np {
namespace {

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

void putI32(std::vector<uint8_t>& b, int32_t v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}

void putString(std::vector<uint8_t>& b, const std::string& s) {
  const uint16_t n = static_cast<uint16_t>(s.size() > 0xFFFFu ? 0xFFFFu : s.size());
  putU16(b, n);
  b.insert(b.end(), s.begin(), s.begin() + n);
}

// io/CompSerial.cpp's cursor, verbatim in shape: every read is bounds-checked
// and sets `bad` rather than throwing, so one check at the end of a parse
// covers every truncation in it. Duplicated here rather than shared, matching
// io/OpSerial.cpp and io/CompSerial.cpp each holding their own copy -- not
// made a shared header in the same step that would introduce a second
// caller.
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
  int32_t i32() {
    const uint32_t bits = u32();
    int32_t v = 0;
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
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // tolerated on read, never written
  return -1;
}

}  // namespace

std::string serializeRegions(const RegionCarrier& in) {
  std::vector<uint8_t> payload;
  putU64(payload, in.nextRegionId);

  const size_t count = in.regions.size();
  putU16(payload, static_cast<uint16_t>(count > 0xFFFFu ? 0xFFFFu : count));
  for (size_t i = 0; i < count && i <= 0xFFFFu; ++i) {
    const Region& r = in.regions[i];
    putU64(payload, r.id);
    payload.push_back(r.kind == RegionKind::Slice ? 1u : 0u);
    putI32(payload, r.x);
    putI32(payload, r.y);
    putU32(payload, r.width);
    putU32(payload, r.height);
    putString(payload, r.name);
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kRegionSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeRegions(std::string_view value, RegionCarrier* out, std::string* errorOut) {
  auto bail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    return false;
  };
  if (out == nullptr) return bail("internal: deserializeRegions() called with no destination.");

  const std::string_view prefix(kRegionSerialPrefix);
  if (value.size() < prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
    const std::string_view seen = value.substr(0, std::min<size_t>(value.size(), 20));
    return bail("regions refused: the value begins \"" + std::string(seen) + "\", not \"" +
               std::string(prefix) +
               "\". The version tag is the prefix precisely so a build decides whether it "
               "understands the encoding before decoding a byte of it; this build reads "
               "version 1 only. The attribute is preserved verbatim and written back "
               "unchanged (PRD I10).");
  }

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0) {
    return bail("regions refused: the payload is " + std::to_string(hex.size()) +
               " hex characters, an odd number, so it does not describe whole bytes.");
  }
  std::vector<uint8_t> payload;
  payload.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return bail("regions refused: '" + std::string(1, hex[hi < 0 ? i : i + 1]) +
                 "' at payload offset " + std::to_string(hi < 0 ? i : i + 1) +
                 " is not a hex digit.");
    }
    payload.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{payload.data(), payload.size(), false};
  RegionCarrier carrier;
  carrier.nextRegionId = r.u64();
  const uint16_t count = r.u16();
  if (r.bad) {
    return bail("regions refused: the payload is too short to hold even its header (the id "
               "counter and the region count).");
  }
  carrier.regions.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Region region;
    region.id = r.u64();
    const uint8_t kindByte = r.u8();
    if (r.bad) {
      return bail("regions refused: region " + std::to_string(i) + " of " +
                 std::to_string(count) + " is truncated before its kind byte.");
    }
    if (kindByte != 0u && kindByte != 1u) {
      return bail("regions refused: region " + std::to_string(i) + " of " +
                 std::to_string(count) + " declares kind byte " + std::to_string(kindByte) +
                 ", which this build does not recognise (0 = Frame, 1 = Slice). The whole "
                 "attribute is preserved verbatim and written back unchanged (PRD I10) rather "
                 "than decoding the regions before this one and dropping the rest.");
    }
    region.kind = kindByte == 1u ? RegionKind::Slice : RegionKind::Frame;
    region.x = r.i32();
    region.y = r.i32();
    region.width = r.u32();
    region.height = r.u32();
    region.name = r.str();
    if (r.bad) {
      return bail("regions refused: region " + std::to_string(i) + " of " +
                 std::to_string(count) + " is truncated. Nothing was decoded.");
    }
    carrier.regions.push_back(std::move(region));
  }
  if (r.left != 0) {
    return bail("regions refused: " + std::to_string(r.left) + " byte(s) follow the " +
               std::to_string(count) +
               " region(s) the header declares. A payload with something after the last record "
               "is one this build does not understand the framing of, and reading the records "
               "anyway would be claiming it does.");
  }

  *out = std::move(carrier);
  return true;
}

}  // namespace np
