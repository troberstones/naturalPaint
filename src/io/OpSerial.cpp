#include "io/OpSerial.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace np {
namespace {

// The wire codes. Explicit numbers, never the C++ enumerators' ordinals --
// see OpSerial.hpp on why appending to either enum must not be able to move
// the meaning of a file that already exists.
constexpr uint16_t kClassPointA = 0;
constexpr uint16_t kClassSpatialB = 1;
constexpr uint16_t kClassStrokeC = 2;
constexpr uint16_t kClassBakedD = 3;

constexpr uint16_t kKindLevels = 0;
constexpr uint16_t kKindCurves = 1;
constexpr uint16_t kKindExposure = 2;
constexpr uint16_t kKindSaturation = 3;
constexpr uint16_t kKindGrayscale = 4;
constexpr uint16_t kKindChannelMixer = 5;

// Every record body starts with class, kind, enabled and the reserved byte.
constexpr size_t kRecordHeaderBytes = 6;

void putU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFFu));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

// The bit pattern, not a decimal rendering. This is the whole of why a grade
// survives a save/load exactly rather than nearly.
void putF32(std::vector<uint8_t>& b, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(b, bits);
}

// A cursor over a decoded payload. Every read is bounds-checked and sets
// `bad` rather than throwing, so one `if (r.bad)` at the end of a parse
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
  float f32() {
    const uint32_t bits = u32();
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // tolerated on read, never written
  return -1;
}

// How many bytes this build's parse of one PointA kind consumes, *not*
// counting the six-byte record header. `Curves` is variable and therefore
// absent here -- it is length-checked by consuming it and comparing (see
// parseRecord).
bool fixedParamBytes(uint16_t kind, size_t* out) {
  switch (kind) {
    case kKindLevels: *out = 3 * 5 * 4; return true;
    case kKindExposure: *out = 4; return true;
    case kKindSaturation: *out = 4 * 4; return true;
    case kKindGrayscale: *out = 3 * 4; return true;
    case kKindChannelMixer: *out = 12 * 4; return true;
    default: return false;
  }
}

// One record body, appended to `body`. Never called for OpClass::Unknown --
// that entry's bytes are emitted verbatim by the caller.
void writeBody(std::vector<uint8_t>& body, const Op& op) {
  uint16_t classCode = kClassPointA;
  switch (op.opClass) {
    case OpClass::PointA: classCode = kClassPointA; break;
    case OpClass::SpatialB: classCode = kClassSpatialB; break;
    case OpClass::StrokeC: classCode = kClassStrokeC; break;
    case OpClass::BakedD: classCode = kClassBakedD; break;
    case OpClass::Unknown: classCode = kClassPointA; break;  // unreachable, see above
  }
  uint16_t kindCode = kKindLevels;
  switch (op.pointKind) {
    case PointOpKind::Levels: kindCode = kKindLevels; break;
    case PointOpKind::Curves: kindCode = kKindCurves; break;
    case PointOpKind::Exposure: kindCode = kKindExposure; break;
    case PointOpKind::Saturation: kindCode = kKindSaturation; break;
    case PointOpKind::Grayscale: kindCode = kKindGrayscale; break;
    case PointOpKind::ChannelMixer: kindCode = kKindChannelMixer; break;
  }

  putU16(body, classCode);
  putU16(body, kindCode);
  body.push_back(op.enabled ? 1u : 0u);
  body.push_back(0u);  // reserved

  // Params only for PointA. A SpatialB/StrokeC/BakedD entry has no params
  // anywhere in this codebase to write (core/OpStack.hpp: nothing constructs
  // one with meaningful data), so its body is the six-byte header and nothing
  // else -- which is exactly what the reader's "length must match" rule then
  // requires of it.
  if (op.opClass != OpClass::PointA) return;

  switch (op.pointKind) {
    case PointOpKind::Levels:
      for (const LevelsParams& p : op.levels) {
        putF32(body, p.blackIn);
        putF32(body, p.whiteIn);
        putF32(body, p.gamma);
        putF32(body, p.blackOut);
        putF32(body, p.whiteOut);
      }
      break;
    case PointOpKind::Curves:
      for (const Curve& c : op.curves) {
        // Capped at what the count field can hold. A curve longer than 65535
        // points cannot arise from anything in this codebase (app/CurveEdit
        // caps insertion at kMaxCurvePointsPerChannel = 16), and truncating
        // silently would be the one thing this module exists to prevent -- so
        // the cap is stated here and asserted by the reader's exact-length
        // rule rather than being a possibility nobody thought about.
        const uint16_t n =
            static_cast<uint16_t>(c.size() > 0xFFFFu ? 0xFFFFu : c.size());
        putU16(body, n);
        for (uint16_t i = 0; i < n; ++i) {
          putF32(body, c[i].x);
          putF32(body, c[i].y);
        }
      }
      break;
    case PointOpKind::Exposure: putF32(body, op.exposure.stops); break;
    case PointOpKind::Saturation:
      putF32(body, op.saturation.scale);
      for (const float w : op.saturation.lumaWeights) putF32(body, w);
      break;
    case PointOpKind::Grayscale:
      for (const float w : op.grayscale.lumaWeights) putF32(body, w);
      break;
    case PointOpKind::ChannelMixer:
      for (const std::array<float, 4>& row : op.channelMixer.matrix)
        for (const float v : row) putF32(body, v);
      break;
  }
}

// Turns one record body into an Op. Returns false when this build cannot
// interpret it, in which case the caller keeps the bytes verbatim -- that is
// not an error, it is PRD I10 (see OpSerial.hpp).
bool parseRecord(const uint8_t* body, size_t length, Op* out) {
  if (length < kRecordHeaderBytes) return false;
  Reader r{body, length, false};
  const uint16_t classCode = r.u16();
  const uint16_t kindCode = r.u16();
  const uint8_t enabled = r.u8();
  const uint8_t reserved = r.u8();
  // A byte this build writes as 0 that came back non-zero means a newer build
  // gave it a meaning. Interpreting the rest of the record would be assuming
  // that meaning does not change what the rest says.
  if (reserved != 0) return false;

  Op op;
  op.enabled = enabled != 0;

  switch (classCode) {
    case kClassSpatialB: op.opClass = OpClass::SpatialB; break;
    case kClassStrokeC: op.opClass = OpClass::StrokeC; break;
    case kClassBakedD: op.opClass = OpClass::BakedD; break;
    case kClassPointA: op.opClass = OpClass::PointA; break;
    default: return false;  // a class this build has no name for
  }

  if (op.opClass != OpClass::PointA) {
    // No params exist for these classes anywhere in this codebase, so the
    // body must be exactly the header. Anything longer is a newer build's
    // parameterised class-B/C/D op and is carried whole.
    if (length != kRecordHeaderBytes) return false;
    *out = std::move(op);
    return true;
  }

  switch (kindCode) {
    case kKindLevels: op.pointKind = PointOpKind::Levels; break;
    case kKindCurves: op.pointKind = PointOpKind::Curves; break;
    case kKindExposure: op.pointKind = PointOpKind::Exposure; break;
    case kKindSaturation: op.pointKind = PointOpKind::Saturation; break;
    case kKindGrayscale: op.pointKind = PointOpKind::Grayscale; break;
    case kKindChannelMixer: op.pointKind = PointOpKind::ChannelMixer; break;
    default: return false;  // a point op this build has no implementation for
  }

  if (size_t fixed = 0; fixedParamBytes(kindCode, &fixed)) {
    // Exactly, never "at least": see OpSerial.hpp on why a longer body is
    // carried rather than half-read.
    if (length != kRecordHeaderBytes + fixed) return false;
  }

  switch (op.pointKind) {
    case PointOpKind::Levels:
      for (LevelsParams& p : op.levels) {
        p.blackIn = r.f32();
        p.whiteIn = r.f32();
        p.gamma = r.f32();
        p.blackOut = r.f32();
        p.whiteOut = r.f32();
      }
      break;
    case PointOpKind::Curves:
      for (Curve& c : op.curves) {
        const uint16_t n = r.u16();
        if (r.bad) return false;
        // Bounded by what is actually left, so a corrupt count cannot make
        // this reserve gigabytes before failing.
        if (static_cast<size_t>(n) * 8u > r.left) return false;
        c.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
          c[i].x = r.f32();
          c[i].y = r.f32();
        }
      }
      // The variable-length kind's own exact-length check: the parse must
      // have consumed the body and nothing less.
      if (r.left != 0) return false;
      break;
    case PointOpKind::Exposure: op.exposure.stops = r.f32(); break;
    case PointOpKind::Saturation:
      op.saturation.scale = r.f32();
      for (float& w : op.saturation.lumaWeights) w = r.f32();
      break;
    case PointOpKind::Grayscale:
      for (float& w : op.grayscale.lumaWeights) w = r.f32();
      break;
    case PointOpKind::ChannelMixer:
      for (std::array<float, 4>& row : op.channelMixer.matrix)
        for (float& v : row) v = r.f32();
      break;
  }
  if (r.bad) return false;
  *out = std::move(op);
  return true;
}

}  // namespace

std::string serializeOpStack(const OpStack& ops) {
  std::vector<uint8_t> payload;
  const size_t n = ops.size();
  putU16(payload, static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n));

  for (size_t i = 0; i < n && i <= 0xFFFFu; ++i) {
    const Op& op = ops.at(i);
    std::vector<uint8_t> body;
    if (op.opClass == OpClass::Unknown) {
      // PRD I10, the write half: the bytes go back out exactly as they came
      // in. The length prefix is recomputed rather than stored, so a record
      // cannot come back with a length that disagrees with its own contents.
      body = op.unrecognised;
    } else {
      writeBody(body, op);
    }
    putU32(payload, static_cast<uint32_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = kOpStackSerialPrefix;
  out.reserve(out.size() + payload.size() * 2);
  for (const uint8_t b : payload) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool deserializeOpStack(std::string_view value, OpStack* out, std::string* errorOut) {
  auto fail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    return false;
  };
  if (out == nullptr) return fail("internal: deserializeOpStack() called with no destination.");

  const std::string_view prefix(kOpStackSerialPrefix);
  if (value.size() < prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
    // Named rather than "malformed": the overwhelmingly likely cause is a
    // newer format version, and a reader that says which one it saw is what
    // makes that diagnosable from the file alone.
    const std::string_view seen = value.substr(0, std::min<size_t>(value.size(), 16));
    return fail("op stack refused: the value begins \"" + std::string(seen) +
                "\", not \"" + std::string(prefix) +
                "\". The version tag is the prefix precisely so a build decides whether it "
                "understands the encoding before decoding a byte of it; this build reads "
                "version 1 only. The attribute is preserved verbatim and written back "
                "unchanged (PRD I10).");
  }

  const std::string_view hex = value.substr(prefix.size());
  if (hex.size() % 2 != 0) {
    return fail("op stack refused: the payload is " + std::to_string(hex.size()) +
                " hex characters, an odd number, so it does not describe whole bytes.");
  }
  std::vector<uint8_t> payload;
  payload.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return fail("op stack refused: '" + std::string(1, hex[hi < 0 ? i : i + 1]) +
                  "' at payload offset " + std::to_string(hi < 0 ? i : i + 1) +
                  " is not a hex digit.");
    }
    payload.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  Reader r{payload.data(), payload.size(), false};
  const uint16_t count = r.u16();
  if (r.bad) return fail("op stack refused: the payload is too short to hold even its op count.");

  OpStack stack;
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t bodyLength = r.u32();
    if (r.bad || bodyLength > r.left) {
      return fail("op stack refused: entry " + std::to_string(i) + " of " +
                  std::to_string(count) + " declares a " + std::to_string(bodyLength) +
                  "-byte body but only " + std::to_string(r.left) +
                  " bytes remain. The stack is truncated; nothing was decoded.");
    }
    Op op;
    if (parseRecord(r.p, bodyLength, &op)) {
      stack.add(std::move(op));
    } else {
      // PRD I10 at the entry level. Kept whole, in position, inert.
      Op unknown;
      unknown.opClass = OpClass::Unknown;
      unknown.unrecognised.assign(r.p, r.p + bodyLength);
      stack.add(std::move(unknown));
    }
    r.p += bodyLength;
    r.left -= bodyLength;
  }
  if (r.left != 0) {
    return fail("op stack refused: " + std::to_string(r.left) +
                " byte(s) follow the " + std::to_string(count) +
                " entries the header declares. A payload with something after the last "
                "record is one this build does not understand the framing of, and reading "
                "the entries anyway would be claiming it does.");
  }

  *out = std::move(stack);
  return true;
}

}  // namespace np
