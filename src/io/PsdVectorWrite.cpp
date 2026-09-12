#include "io/PsdVectorWrite.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "color/Space.hpp"
#include "io/Descriptor.hpp"
#include "io/PsdVectorPath.hpp"

// io/PsdVectorWrite -- see the header for the design. This file is the
// mechanics: the descriptor primitives, then the two block encoders.
namespace np {
namespace {

// PSD's own record-selector numbering, spelled the way io/PsdVectorPath.cpp's
// own `PsdPathSelector` spells it so the encoder and the decoder are
// greppable together.
enum PsdPathSelector : uint16_t {
  kSelClosedLength = 0,
  kSelClosedLinked = 1,
  kSelClosedUnlinked = 2,
  kSelOpenLength = 3,
  kSelOpenLinked = 4,
  kSelOpenUnlinked = 5,
  kSelPathFillRule = 6,
  kSelInitialFillRule = 8,
};

// The two all-zero records every real block opens with, in this order, before
// any subpath. io/PsdVectorPath.hpp records that neither carries the
// even-odd/nonzero choice and that both are all-zero in every file examined;
// they are written because every real block has them, not because anything
// reads them.
void writeEmptyRecord(PsdWriter& w, uint16_t selector) {
  w.u16(selector);
  w.zeros(kPsdPathRecordBytes - 2);
}

// One coordinate as signed 8.24 fixed point: `raw / 2^24` is a fraction of
// `dimension`. The exact inverse of io/PsdVectorPath.cpp's `fixedToCoord()`.
//
// Saturated rather than wrapped -- see `PsdVectorShapeMask::saturatedCoords`.
// The double arithmetic matters: 2^24 * a coordinate near the int32 ceiling
// overflows a float's 24-bit significand long before it overflows int32, so
// computing this in float would round the saturation test's own input.
int32_t coordToFixed(float coord, int32_t dimension, size_t& saturated) {
  if (dimension <= 0 || !std::isfinite(coord)) return 0;
  const double raw = static_cast<double>(coord) / static_cast<double>(dimension) * 16777216.0;
  constexpr double kMin = -2147483648.0;
  constexpr double kMax = 2147483647.0;
  if (raw <= kMin) {
    ++saturated;
    return -2147483647 - 1;
  }
  if (raw >= kMax) {
    ++saturated;
    return 2147483647;
  }
  return static_cast<int32_t>(std::lround(raw));
}

void writeKnot(PsdWriter& w, uint16_t selector, const Anchor& a, int32_t docWidth,
               int32_t docHeight, size_t& saturated) {
  w.u16(selector);
  // Y BEFORE X in each pair, and the pairs in the order in / pt / out.
  w.i32(coordToFixed(a.in.y, docHeight, saturated));
  w.i32(coordToFixed(a.in.x, docWidth, saturated));
  w.i32(coordToFixed(a.pt.y, docHeight, saturated));
  w.i32(coordToFixed(a.pt.x, docWidth, saturated));
  w.i32(coordToFixed(a.out.y, docHeight, saturated));
  w.i32(coordToFixed(a.out.x, docWidth, saturated));
}

// An IEEE-754 binary64, most significant byte first. `PsdWriter` has no
// 64-bit field because nothing else in PSD export needs one, and a descriptor
// is the only place a double occurs -- so this is two `u32`s here rather than
// a fifth primitive in a module every other writer shares.
void writeDoubleBE(PsdWriter& w, double value) {
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "binary64 is the wire type");
  std::memcpy(&bits, &value, sizeof(bits));
  w.u32(static_cast<uint32_t>(bits >> 32));
  w.u32(static_cast<uint32_t>(bits & 0xFFFFFFFFull));
}

// Linear straight alpha -> the 0..255 sRGB double an `RGBC` descriptor
// carries. The exact inverse of io/PsdVectorStyle.cpp's
// `srgbDecode(value / 255)`, including the clamp an out-of-gamut scene-
// referred colour needs before it reaches a field bounded at 255.
double channelToRgbcDouble(float linear) {
  const float encoded = srgbEncode(std::clamp(linear, 0.0f, 1.0f));
  return static_cast<double>(std::clamp(encoded, 0.0f, 1.0f)) * 255.0;
}

}  // namespace

// ==========================================================================
// Action Descriptor primitives
// ==========================================================================

void writePsdDescriptorKey(PsdWriter& w, std::string_view key) {
  // Zero means four. See io/PsdVectorWrite.hpp and io/Descriptor.hpp's own
  // "The Key quirk, stated first because it is where readers break".
  w.u32(key.size() == 4 ? 0u : static_cast<uint32_t>(key.size()));
  for (const char c : key) w.u8(static_cast<uint8_t>(c));
}

void writePsdDescriptorUnicodeString(PsdWriter& w, std::string_view utf8) {
  const std::vector<uint16_t> units = utf8ToUtf16(std::string(utf8));
  // The trailing NUL is inside the count, which is what Photoshop writes and
  // what io/Descriptor's `readUnicodeString()` strips back off.
  w.u32(static_cast<uint32_t>(units.size() + 1));
  for (const uint16_t u : units) w.u16(u);
  w.u16(0);
}

void writePsdDescriptorHead(PsdWriter& w, std::string_view classId, uint32_t itemCount) {
  writePsdDescriptorUnicodeString(w, std::string_view());
  writePsdDescriptorKey(w, classId);
  w.u32(itemCount);
}

void writePsdDescriptorObjectItem(PsdWriter& w, std::string_view key, std::string_view classId,
                                  uint32_t itemCount) {
  writePsdDescriptorKey(w, key);
  w.fourcc("Objc");
  writePsdDescriptorHead(w, classId, itemCount);
}

void writePsdDescriptorDouble(PsdWriter& w, std::string_view key, double value) {
  writePsdDescriptorKey(w, key);
  w.fourcc("doub");
  writeDoubleBE(w, value);
}

// ==========================================================================
// `vsms`
// ==========================================================================

PsdVectorShapeMask encodePsdVectorShapeMask(const Path& path, int32_t docWidth,
                                            int32_t docHeight) {
  PsdVectorShapeMask out;
  if (docWidth <= 0 || docHeight <= 0) return out;

  // A subpath of fewer than two anchors encloses nothing -- `pathIsEmpty()`'s
  // own test, applied per subpath here so one degenerate subpath does not
  // cost the rest of a compound path.
  size_t writable = 0;
  for (const SubPath& sub : path.subpaths)
    if (sub.anchors.size() >= 2) ++writable;
  if (writable == 0) return out;

  // The fill rule, inverted through the boolean operation it was folded out
  // of. See the header: NonZero -> Union everywhere, EvenOdd -> Exclude
  // everywhere, and `composePsdSubPaths()` folds either back to the rule it
  // came from.
  const int16_t op = path.rule == FillRule::EvenOdd
                         ? static_cast<int16_t>(PsdPathOp::Exclude)
                         : static_cast<int16_t>(PsdPathOp::Union);

  PsdWriter w;
  w.u32(kPsdPathBlockVersion);
  w.u32(kPsdPathBlockFlags);
  writeEmptyRecord(w, kSelPathFillRule);
  writeEmptyRecord(w, kSelInitialFillRule);

  for (const SubPath& sub : path.subpaths) {
    if (sub.anchors.size() < 2) continue;

    w.u16(sub.closed ? kSelClosedLength : kSelOpenLength);
    w.u16(static_cast<uint16_t>(std::min<size_t>(sub.anchors.size(), 0xFFFFu)));
    w.i16(op);
    // The `uint16` at offset 6 that nothing may key off: Apple's file writes
    // 1 everywhere and `testNonSquareWithShapesOffPage.psd`'s `Star 1` writes
    // 2, so the value carries no meaning this project has found. 1 is written
    // because it is the commoner of the two observed.
    w.u16(1);
    w.u32(0);
    // The origination index, which ties a subpath to an entry in `vogk`.
    // Zero for every subpath because this writer emits no `vogk` at all --
    // `vogk` is origination history (how a live parametric rectangle was
    // scaled since it was created) and io/PsdVectorPath.hpp records that
    // applying its `Trnf` is wrong. An index pointing into a block that does
    // not exist would be worse than none.
    w.u32(0);
    w.zeros(10);

    for (const Anchor& a : sub.anchors) {
      const uint16_t selector =
          sub.closed ? (a.smooth ? kSelClosedLinked : kSelClosedUnlinked)
                     : (a.smooth ? kSelOpenLinked : kSelOpenUnlinked);
      writeKnot(w, selector, a, docWidth, docHeight, out.saturatedCoords);
    }
    ++out.subpathsWritten;
  }

  // Photoshop pads the block to a multiple of 4 (`192 = 8 + 7*26 + 2`) and
  // io/PsdVectorPath's decoder discards exactly that remainder. 8 + 26n is 2
  // mod 4 for odd n, so this is 0 or 2 bytes and never breaks the "each extra
  // block's length must be even" rule io/PsdLayerSection.hpp states.
  w.padTo(4, 0);

  if (!w.ok()) return PsdVectorShapeMask{};
  out.bytes = w.take();
  return out;
}

// ==========================================================================
// `SoCo`
// ==========================================================================

std::vector<uint8_t> encodePsdSolidColorBlock(const std::array<float, 4>& linearRgba) {
  PsdWriter w;
  w.u32(kActionDescriptorVersion);
  writePsdDescriptorHead(w, "null", 1);
  writePsdDescriptorObjectItem(w, "Clr ", "RGBC", 3);
  // The space padding in all four keys is real data, not formatting:
  // io/PsdVectorStyle.cpp reads exactly `"Clr "`, `"Rd  "`, `"Grn "`, `"Bl  "`.
  writePsdDescriptorDouble(w, "Rd  ", channelToRgbcDouble(linearRgba[0]));
  writePsdDescriptorDouble(w, "Grn ", channelToRgbcDouble(linearRgba[1]));
  writePsdDescriptorDouble(w, "Bl  ", channelToRgbcDouble(linearRgba[2]));
  if (!w.ok()) return {};
  return w.take();
}

}  // namespace np
