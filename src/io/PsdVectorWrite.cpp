#include "io/PsdVectorWrite.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/Path.hpp"

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

// Photoshop's stop position on the wire: a `long` in 0..4096, the exact
// inverse of io/PsdVectorStyle.cpp's `locationToPosition()`.
int32_t positionToLocation(float position) {
  return static_cast<int32_t>(
      std::lround(std::clamp(position, 0.0f, 1.0f) * 4096.0f));
}

// `Mdpn`, a `long` percentage. The inverse of `midpointFromPercent()`.
int32_t midpointToPercent(float midpoint) {
  return static_cast<int32_t>(std::lround(std::clamp(midpoint, 0.0f, 1.0f) * 100.0f));
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

void writePsdDescriptorBool(PsdWriter& w, std::string_view key, bool value) {
  writePsdDescriptorKey(w, key);
  w.fourcc("bool");
  w.u8(value ? 1u : 0u);
}

void writePsdDescriptorInteger(PsdWriter& w, std::string_view key, int32_t value) {
  writePsdDescriptorKey(w, key);
  w.fourcc("long");
  w.u32(static_cast<uint32_t>(value));
}

void writePsdDescriptorUnitFloat(PsdWriter& w, std::string_view key, std::string_view unit,
                                 double value) {
  writePsdDescriptorKey(w, key);
  w.fourcc("UntF");
  // The unit is a BARE four-character code, not a Key: no length precedes it.
  for (const char c : unit) w.u8(static_cast<uint8_t>(c));
  writeDoubleBE(w, value);
}

void writePsdDescriptorEnum(PsdWriter& w, std::string_view key, std::string_view typeId,
                            std::string_view valueId) {
  writePsdDescriptorKey(w, key);
  w.fourcc("enum");
  // Both halves ARE Keys, so both take the zero-means-four rule.
  writePsdDescriptorKey(w, typeId);
  writePsdDescriptorKey(w, valueId);
}

void writePsdDescriptorText(PsdWriter& w, std::string_view key, std::string_view utf8) {
  writePsdDescriptorKey(w, key);
  w.fourcc("TEXT");
  writePsdDescriptorUnicodeString(w, utf8);
}

void writePsdDescriptorListItem(PsdWriter& w, std::string_view key, uint32_t count) {
  writePsdDescriptorKey(w, key);
  w.fourcc("VlLs");
  w.u32(count);
}

void writePsdDescriptorListObjectElement(PsdWriter& w, std::string_view classId,
                                         uint32_t itemCount) {
  // NO key: a list element is positional. See the header.
  w.fourcc("Objc");
  writePsdDescriptorHead(w, classId, itemCount);
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

// ==========================================================================
// `GdFl`
// ==========================================================================

std::vector<uint8_t> encodePsdGradientFillBlock(const GradientDef& gradient,
                                                const PathBounds& bounds) {
  if (gradient.stops.colorStops.empty()) return {};

  // --- The inverse of psdGradientGeometryFor() ----------------------------
  //
  // Two points in document texels go back out as an angle, a scale percentage
  // and an offset percentage pair. Everything here mirrors a line of that
  // function, including the y negation that turns a y-down direction back into
  // Photoshop's y-up angle.
  const GradientGeometry& g = gradient.geometry;
  const bool reflected = g.spread == GradientSpread::Reflect;
  const double dx = static_cast<double>(g.x1) - static_cast<double>(g.x0);
  const double dy = static_cast<double>(g.y1) - static_cast<double>(g.y0);
  const double span = std::sqrt(dx * dx + dy * dy);

  // A Linear-Pad ramp's centre is the midpoint of its two points; every other
  // kind runs from its centre outward, so p0 IS the centre.
  const bool centreIsP0 = reflected || g.kind != GradientKind::Linear;
  const double cx = centreIsP0 ? g.x0 : (static_cast<double>(g.x0) + static_cast<double>(g.x1)) * 0.5;
  const double cy = centreIsP0 ? g.y0 : (static_cast<double>(g.y0) + static_cast<double>(g.y1)) * 0.5;

  double w = 0.0, h = 0.0, boundsCx = cx, boundsCy = cy;
  if (bounds.valid) {
    w = static_cast<double>(bounds.maxX) - static_cast<double>(bounds.minX);
    h = static_cast<double>(bounds.maxY) - static_cast<double>(bounds.minY);
    boundsCx = (static_cast<double>(bounds.minX) + static_cast<double>(bounds.maxX)) * 0.5;
    boundsCy = (static_cast<double>(bounds.minY) + static_cast<double>(bounds.maxY)) * 0.5;
  }

  constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
  // atan2 of the NEGATED dy, undoing the y-down flip the decoder applied.
  double angleDegrees = span > 0.0 ? std::atan2(-dy, dx) * kRadToDeg : 0.0;
  // atan2(-0.0, positive) is -0.0, which prints as "-0" and round-trips
  // perfectly but reads as a bug in every dump. Normalised, not clamped.
  if (angleDegrees == 0.0) angleDegrees = 0.0;
  double scalePercent = 100.0;

  if (g.kind == GradientKind::Radial) {
    // The radial's angle is meaningless (the decoder never reads it back
    // either), so it goes out as zero rather than as whatever atan2 made of an
    // axis-aligned radius vector.
    angleDegrees = 0.0;
    const double diagonalHalf = std::sqrt(w * w + h * h) * 0.5;
    scalePercent = diagonalHalf > 0.0 ? span / diagonalHalf * 100.0 : 0.0;
  } else if (g.kind == GradientKind::Angular) {
    scalePercent = 100.0;  // an angular ramp has a direction and no length
  } else {
    const double a = angleDegrees / kRadToDeg;
    const double projected = std::fabs(w * std::cos(a)) + std::fabs(h * std::sin(a));
    const double length = reflected ? span * 2.0 : span;
    scalePercent = projected > 0.0 ? length / projected * 100.0 : 0.0;
  }

  const double offsetXPercent = w > 0.0 ? (cx - boundsCx) / w * 100.0 : 0.0;
  const double offsetYPercent = h > 0.0 ? (cy - boundsCy) / h * 100.0 : 0.0;

  const char* typeId = "Lnr ";
  if (reflected && g.kind == GradientKind::Linear) typeId = "Rflc";
  else if (g.kind == GradientKind::Radial) typeId = "Rdl ";
  else if (g.kind == GradientKind::Angular) typeId = "Angl";

  // --- The descriptor -----------------------------------------------------
  PsdWriter w2;
  w2.u32(kActionDescriptorVersion);
  // EIGHT items: Grad, Type, Angl, Scl, Ofst, Dthr, Rvrs, Algn. A count that
  // disagrees with what follows leaves the parser mid-item at the end of the
  // block -- app/selftest/PsdVectorGradient.cpp asserts the parse consumes the
  // block EXACTLY, which is what caught this being 7.
  writePsdDescriptorHead(w2, "null", 8);

  const uint32_t colorCount = static_cast<uint32_t>(
      std::min<size_t>(gradient.stops.colorStops.size(), 0xFFFFFFFFull));
  const uint32_t opacityCount = static_cast<uint32_t>(
      std::min<size_t>(gradient.stops.opacityStops.size(), 0xFFFFFFFFull));

  writePsdDescriptorObjectItem(w2, "Grad", "Grdn", 5);
  writePsdDescriptorText(w2, "Nm  ", gradient.name);
  // `CstS` -- a custom-stop ramp, the only form with a stop list, which is the
  // only form this build can produce.
  writePsdDescriptorEnum(w2, "GrdF", "GrdF", "CstS");
  // `Intr` is the smoothness, 4096 being 100 %. This build's ramp is always
  // fully smooth between stops, so the maximum is the honest value.
  writePsdDescriptorDouble(w2, "Intr", 4096.0);
  writePsdDescriptorListItem(w2, "Clrs", colorCount);
  for (const ColorStop& c : gradient.stops.colorStops) {
    writePsdDescriptorListObjectElement(w2, "Clrt", 4);
    writePsdDescriptorObjectItem(w2, "Clr ", "RGBC", 3);
    writePsdDescriptorDouble(w2, "Rd  ", channelToRgbcDouble(c.color[0]));
    writePsdDescriptorDouble(w2, "Grn ", channelToRgbcDouble(c.color[1]));
    writePsdDescriptorDouble(w2, "Bl  ", channelToRgbcDouble(c.color[2]));
    // `UsrS`: a fixed colour rather than one tracking a swatch. This build has
    // no foreground/background stop, so it can only ever write this one.
    writePsdDescriptorEnum(w2, "Type", "Clry", "UsrS");
    writePsdDescriptorInteger(w2, "Lctn", positionToLocation(c.position));
    writePsdDescriptorInteger(w2, "Mdpn", midpointToPercent(c.midpoint));
  }
  writePsdDescriptorListItem(w2, "Trns", opacityCount);
  for (const OpacityStop& o : gradient.stops.opacityStops) {
    writePsdDescriptorListObjectElement(w2, "TrnS", 3);
    writePsdDescriptorUnitFloat(w2, "Opct", "#Prc",
                                static_cast<double>(std::clamp(o.opacity, 0.0f, 1.0f)) * 100.0);
    writePsdDescriptorInteger(w2, "Lctn", positionToLocation(o.position));
    writePsdDescriptorInteger(w2, "Mdpn", midpointToPercent(o.midpoint));
  }

  writePsdDescriptorEnum(w2, "Type", "GrdT", typeId);
  writePsdDescriptorUnitFloat(w2, "Angl", "#Ang", angleDegrees);
  writePsdDescriptorUnitFloat(w2, "Scl ", "#Prc", scalePercent);
  writePsdDescriptorObjectItem(w2, "Ofst", "Pnt ", 2);
  writePsdDescriptorUnitFloat(w2, "Hrzn", "#Prc", offsetXPercent);
  writePsdDescriptorUnitFloat(w2, "Vrtc", "#Prc", offsetYPercent);
  // Photoshop's own defaults -- see the header on why neither is a claim about
  // this document.
  writePsdDescriptorBool(w2, "Dthr", true);
  writePsdDescriptorBool(w2, "Rvrs", false);
  writePsdDescriptorBool(w2, "Algn", true);

  if (!w2.ok()) return {};
  return w2.take();
}

}  // namespace np
