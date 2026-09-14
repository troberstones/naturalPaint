#include "io/GradientSerial.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace np {
namespace {

void putU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFFu));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// The bit pattern, not a decimal rendering -- io/PathSerial's reason.
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

// io/RegionSerial.cpp's cursor, verbatim in shape and duplicated for its
// stated reason: every read is bounds-checked and sets `bad` rather than
// throwing, so one check covers every truncation.
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

// The smallest a gradient record can be, used to bound the declared count
// against what actually remains before a single entry is reserved: an empty
// name (2), kind + spread (2), four geometry floats (16), and two zero stop
// counts (4).
constexpr size_t kMinGradientRecordBytes = 2 + 2 + 16 + 4;

// Per-stop wire costs, for the same bounding rule one level down.
constexpr size_t kColorStopBytes = 5 * 4;
constexpr size_t kOpacityStopBytes = 3 * 4;

}  // namespace

std::string serializeGradients(const GradientTable& gradients) {
  std::vector<uint8_t> payload;
  const size_t count = gradients.size();
  putU32(payload, static_cast<uint32_t>(count));
  for (const GradientDef& g : gradients) {
    putString(payload, g.name);
    payload.push_back(static_cast<uint8_t>(g.geometry.kind));
    payload.push_back(static_cast<uint8_t>(g.geometry.spread));
    putF32(payload, g.geometry.x0);
    putF32(payload, g.geometry.y0);
    putF32(payload, g.geometry.x1);
    putF32(payload, g.geometry.y1);

    const size_t cs = std::min<size_t>(g.stops.colorStops.size(), 0xFFFFu);
    putU16(payload, static_cast<uint16_t>(cs));
    for (size_t i = 0; i < cs; ++i) {
      const ColorStop& c = g.stops.colorStops[i];
      putF32(payload, c.position);
      putF32(payload, c.color[0]);
      putF32(payload, c.color[1]);
      putF32(payload, c.color[2]);
      putF32(payload, c.midpoint);
    }

    const size_t os = std::min<size_t>(g.stops.opacityStops.size(), 0xFFFFu);
    putU16(payload, static_cast<uint16_t>(os));
    for (size_t i = 0; i < os; ++i) {
      const OpacityStop& o = g.stops.opacityStops[i];
      putF32(payload, o.position);
      putF32(payload, o.opacity);
      putF32(payload, o.midpoint);
    }
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kGradientSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeGradients(std::string_view value, GradientTable* out, std::string* errorOut) {
  auto bail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    return false;
  };
  if (out == nullptr) return bail("internal: deserializeGradients() called with no destination.");

  const std::string_view prefix(kGradientSerialPrefix);
  if (value.size() < prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
    const std::string_view seen = value.substr(0, std::min<size_t>(value.size(), 20));
    return bail("gradients refused: the value begins \"" + std::string(seen) + "\", not \"" +
                std::string(prefix) +
                "\". The version tag is the prefix precisely so a build decides whether it "
                "understands the encoding before decoding a byte of it; this build reads "
                "version 1 only. The attribute is preserved verbatim and written back "
                "unchanged (PRD I10).");
  }

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0) {
    return bail("gradients refused: the payload is " + std::to_string(hex.size()) +
                " hex characters, an odd number, so it does not describe whole bytes.");
  }
  std::vector<uint8_t> payload;
  payload.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return bail("gradients refused: '" + std::string(1, hex[hi < 0 ? i : i + 1]) +
                  "' at payload offset " + std::to_string(hi < 0 ? i : i + 1) +
                  " is not a hex digit.");
    }
    payload.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{payload.data(), payload.size(), false};
  const uint32_t count = r.u32();
  if (r.bad)
    return bail("gradients refused: the payload is too short to hold even its entry count.");
  // Bounded by the bytes that actually remain, before anything is reserved:
  // a u32 count could otherwise claim four billion entries.
  if (static_cast<size_t>(count) * kMinGradientRecordBytes > r.left) {
    return bail("gradients refused: the header declares " + std::to_string(count) +
                " gradient(s), which cannot fit in the remaining " + std::to_string(r.left) +
                " byte(s).");
  }

  GradientTable table;
  table.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    GradientDef g;
    g.name = r.str();
    const uint8_t kindByte = r.u8();
    const uint8_t spreadByte = r.u8();
    if (r.bad) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " is truncated before its kind and spread bytes.");
    }
    if (kindByte > static_cast<uint8_t>(GradientKind::Angular)) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " declares kind byte " + std::to_string(kindByte) +
                  ", which this build does not recognise (0 = Linear, 1 = Radial, 2 = "
                  "Angular). The whole attribute is preserved verbatim and written back "
                  "unchanged (PRD I10) rather than decoding the gradients before this one "
                  "and dropping the rest -- which would silently renumber every "
                  "Paint::gradient index after it.");
    }
    if (spreadByte > static_cast<uint8_t>(GradientSpread::Reflect)) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " declares spread byte " + std::to_string(spreadByte) +
                  ", which this build does not recognise (0 = Pad, 1 = Repeat, 2 = Reflect).");
    }
    g.geometry.kind = static_cast<GradientKind>(kindByte);
    g.geometry.spread = static_cast<GradientSpread>(spreadByte);
    g.geometry.x0 = r.f32();
    g.geometry.y0 = r.f32();
    g.geometry.x1 = r.f32();
    g.geometry.y1 = r.f32();

    const uint16_t colorCount = r.u16();
    if (r.bad || static_cast<size_t>(colorCount) * kColorStopBytes > r.left) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " declares " + std::to_string(colorCount) +
                  " colour stop(s), which do not fit in what remains.");
    }
    g.stops.colorStops.reserve(colorCount);
    for (uint16_t k = 0; k < colorCount; ++k) {
      ColorStop c;
      c.position = r.f32();
      c.color[0] = r.f32();
      c.color[1] = r.f32();
      c.color[2] = r.f32();
      c.midpoint = r.f32();
      g.stops.colorStops.push_back(c);
    }

    const uint16_t opacityCount = r.u16();
    if (r.bad || static_cast<size_t>(opacityCount) * kOpacityStopBytes > r.left) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " declares " + std::to_string(opacityCount) +
                  " opacity stop(s), which do not fit in what remains.");
    }
    g.stops.opacityStops.reserve(opacityCount);
    for (uint16_t k = 0; k < opacityCount; ++k) {
      OpacityStop o;
      o.position = r.f32();
      o.opacity = r.f32();
      o.midpoint = r.f32();
      g.stops.opacityStops.push_back(o);
    }

    if (r.bad) {
      return bail("gradients refused: gradient " + std::to_string(i) + " of " +
                  std::to_string(count) + " is truncated. Nothing was decoded.");
    }
    table.push_back(std::move(g));
  }

  if (r.left != 0) {
    return bail("gradients refused: " + std::to_string(r.left) + " byte(s) follow the " +
                std::to_string(count) +
                " gradient(s) the header declares. A payload with something after the last "
                "record is one this build does not understand the framing of, and reading the "
                "records anyway would be claiming it does.");
  }

  *out = std::move(table);
  return true;
}

}  // namespace np
