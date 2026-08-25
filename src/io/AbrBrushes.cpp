#include "io/AbrBrushes.hpp"

#include <algorithm>
#include <cstring>

#include "io/Descriptor.hpp"

namespace np {
namespace {

// Big-endian reads that refuse rather than wrap. `at` is checked against the
// span's size on every call; this file parses a format from the internet and
// the whole point of io/Descriptor.hpp's contract is not to read past a
// buffer, so the block framing around it holds itself to the same rule.
bool readU16(std::span<const uint8_t> b, size_t at, uint16_t& out) noexcept {
  if (at + 2 > b.size()) return false;
  out = static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
  return true;
}

bool readU32(std::span<const uint8_t> b, size_t at, uint32_t& out) noexcept {
  if (at + 4 > b.size()) return false;
  out = (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
        (static_cast<uint32_t>(b[at + 2]) << 8) | static_cast<uint32_t>(b[at + 3]);
  return true;
}

float clampf(float v, float lo, float hi) noexcept {
  return v < lo ? lo : (v > hi ? hi : v);
}

// A `UntF` read that does not care which unit tag it carries.
//
// Photoshop is not consistent about them -- `Dmtr` is `#Pxl` on one brush and
// `#Prc` on another depending on how the brush was authored -- and this
// importer wants the number either way. **The unit IS checked where it changes
// the meaning** (see the diameter read, which refuses a percentage because a
// percentage of what is not knowable from the descriptor alone).
bool unitValue(const DescriptorRef& ref, double& out) {
  if (!ref.valid()) return false;
  if (const auto uf = ref.asUnitFloat()) {
    out = uf->value;
    return true;
  }
  // Some keys are written as a plain double rather than a unit float.
  if (const auto d = ref.asDouble()) {
    out = *d;
    return true;
  }
  return false;
}

// One of Photoshop's `brVr` dynamics objects: a control, a jitter percentage
// and a minimum.
struct AbrDynamics {
  bool present = false;
  int control = 0;   // bVTy
  double jitter = 0.0;  // percent
  double minimum = 0.0;  // percent
};

AbrDynamics readDynamics(const DescriptorRef& owner, const char* key) {
  AbrDynamics d;
  const DescriptorRef ref = owner.field(key);
  if (!ref.valid()) return d;
  d.present = true;
  if (const auto v = ref.field("bVTy").asInteger()) d.control = *v;
  unitValue(ref.field("jitter"), d.jitter);
  unitValue(ref.field("Mnm "), d.minimum);
  return d;
}

// Turn one Photoshop dynamics object into up to two links.
//
// **Up to two, because Photoshop's jitter and its control are independent and
// both live.** "Size Jitter 74%, Control: Pen Pressure" means the size varies
// randomly AND follows the pen; they are two rows of this application's matrix
// (RANDOM and PRESSURE, same column), which compose by multiplication because
// Size is a Multiply target. That is close to what Photoshop does and not
// exactly it, and the header says so.
//
// `floorPercent` is the target's own minimum -- `minimumDiameter` for size,
// `minimumRoundness` for roundness -- which is Photoshop's name for exactly
// what `BrushLink::rangeLo` is.
void addDynamicsLinks(BrushLinkSet& links, const AbrDynamics& d, DynamicTarget target,
                      double floorPercent, const std::string& brushName,
                      AbrImportResult& result) {
  if (!d.present) return;

  const bool angular = targetCombine(target) == TargetCombine::Add;
  float lo = 0.0f, hi = 1.0f;
  if (angular) targetDefaultRange(target, lo, hi);
  else lo = clampf(static_cast<float>(floorPercent) / 100.0f, 0.0f, 1.0f);

  DynamicSource source{};
  if (d.control != static_cast<int>(AbrControl::Off)) {
    if (abrControlToSource(d.control, source)) {
      BrushLink l;
      l.source = source;
      l.target = target;
      l.rangeLo = lo;
      l.rangeHi = hi;
      addLink(links, l);
    } else {
      // A control this build has no input for. Recorded per brush rather than
      // dropped, because "the imported brush does less than the original" is
      // exactly the thing an import must not do quietly.
      ++result.unmappedControls;
      result.notes.push_back(
          {brushName, std::string("dynamics control '") + abrControlName(d.control) +
                          "' on " + targetName(target) + " has no source here"});
    }
  }

  // Jitter is a random variation DOWNWARD from full: 74% jitter means the
  // value ranges over the bottom 26%..100%. Bounded below by the target's own
  // minimum, which is what Photoshop's Minimum Diameter does to Size Jitter.
  if (d.jitter > 0.0) {
    BrushLink l;
    l.source = DynamicSource::Random;
    l.target = target;
    if (angular) {
      // **The jitter percentage scales the SPAN, it does not select a preset
      // span.** An angle jitter of 50% is half a turn, not a whole one --
      // taking the target's default range here regardless would import every
      // partially-jittered brush as fully random, which looks like the import
      // works (marks do vary) while being wrong for every brush that asked for
      // restraint.
      float lo0 = 0.0f, hi0 = 0.0f;
      targetDefaultRange(target, lo0, hi0);
      const float scale = clampf(static_cast<float>(d.jitter) / 100.0f, 0.0f, 1.0f);
      l.rangeLo = lo0 * scale;
      l.rangeHi = hi0 * scale;
    } else {
      l.rangeLo = std::max(lo, clampf(1.0f - static_cast<float>(d.jitter) / 100.0f, 0.0f, 1.0f));
      l.rangeHi = 1.0f;
    }
    addLink(links, l);
  }
}

BrushPreset presetFromDescriptor(const DescriptorRef& node, AbrImportResult& result) {
  BrushPreset p;
  if (const auto nm = node.field("Nm  ").asText()) p.name = std::string(*nm);
  if (p.name.empty()) p.name = "Untitled brush";

  const DescriptorRef brsh = node.field("Brsh");

  // The tip. `sampledData` is a UUID pointing into the `samp` block's bitmap;
  // its presence is exactly "this brush's shape is a picture we cannot use".
  if (brsh.field("sampledData").valid()) {
    ++result.sampledTips;
    result.notes.push_back(
        {p.name, "sampled bitmap tip not imported -- it will paint with the round procedural tip"});
  }

  double v = 0.0;
  // Diameter is in pixels; radius is half of it. A `#Prc` diameter is a
  // percentage of the sampled tip's own size, which is a number that only
  // exists inside the `samp` block -- so it is refused rather than guessed.
  const DescriptorRef dmtr = brsh.field("Dmtr");
  if (const auto uf = dmtr.asUnitFloat()) {
    if (uf->unit == "#Prc") {
      result.notes.push_back({p.name, "diameter is a percentage of a sampled tip; size not imported"});
    } else if (uf->value > 0.0) {
      p.radius = clampf(static_cast<float>(uf->value) * 0.5f, 0.5f, 4096.0f);
    }
  }

  if (unitValue(brsh.field("Hrdn"), v)) p.hardness = clampf(static_cast<float>(v) / 100.0f, 0.0f, 1.0f);
  if (unitValue(brsh.field("Rndn"), v)) p.roundness = clampf(static_cast<float>(v) / 100.0f, 0.01f, 1.0f);
  if (unitValue(brsh.field("Angl"), v)) p.angle = static_cast<float>(v);
  if (unitValue(brsh.field("Spcn"), v)) p.spacing = abrSpacingToRadii(v);

  // Dynamics. `useTipDynamics` gates the first three the way the Brush panel's
  // own Shape Dynamics checkbox does: with it off, Photoshop keeps the authored
  // jitter values but does not apply them, and an import that applied them
  // anyway would paint differently from the brush it claims to be.
  const bool tipDynamics = node.field("useTipDynamics").asBoolean().value_or(false);
  if (tipDynamics) {
    double minDiameter = 0.0, minRoundness = 0.0;
    unitValue(node.field("minimumDiameter"), minDiameter);
    unitValue(node.field("minimumRoundness"), minRoundness);
    addDynamicsLinks(p.links, readDynamics(node, "szVr"), DynamicTarget::Size, minDiameter,
                     p.name, result);
    addDynamicsLinks(p.links, readDynamics(node, "angleDynamics"), DynamicTarget::Angle, 0.0,
                     p.name, result);
    addDynamicsLinks(p.links, readDynamics(node, "roundnessDynamics"), DynamicTarget::Roundness,
                     minRoundness, p.name, result);
  }

  if (node.field("useScatter").asBoolean().value_or(false)) {
    addDynamicsLinks(p.links, readDynamics(node, "scatterDynamics"), DynamicTarget::Scatter, 0.0,
                     p.name, result);
  }

  return p;
}

}  // namespace

const char* abrControlName(int bVTy) noexcept {
  switch (static_cast<AbrControl>(bVTy)) {
    case AbrControl::Off: return "Off";
    case AbrControl::Fade: return "Fade";
    case AbrControl::PenPressure: return "Pen Pressure";
    case AbrControl::PenTilt: return "Pen Tilt";
    case AbrControl::StylusWheel: return "Stylus Wheel";
    case AbrControl::Rotation: return "Rotation";
    case AbrControl::InitialDirection: return "Initial Direction";
    case AbrControl::Direction: return "Direction";
  }
  return "unknown control";
}

bool abrControlToSource(int bVTy, DynamicSource& out) noexcept {
  switch (static_cast<AbrControl>(bVTy)) {
    case AbrControl::Fade: out = DynamicSource::Fade; return true;
    case AbrControl::PenPressure: out = DynamicSource::Pressure; return true;
    case AbrControl::PenTilt: out = DynamicSource::Tilt; return true;
    case AbrControl::Rotation: out = DynamicSource::Barrel; return true;
    // Off is not "unmapped", it is "no link", and the caller checks it first.
    case AbrControl::Off:
    // The three with no input here. Stylus Wheel is a device axis SDL does not
    // report; both Direction controls need the stroke's own heading, which
    // nothing in this build computes yet -- brush/StrokePath has the geometry
    // to derive it, so this is a gap rather than an impossibility.
    case AbrControl::StylusWheel:
    case AbrControl::InitialDirection:
    case AbrControl::Direction:
      return false;
  }
  return false;
}

float abrSpacingToRadii(double percentOfDiameter) noexcept {
  // percent-of-diameter -> fraction-of-diameter -> fraction-of-radius.
  const float radii = static_cast<float>(percentOfDiameter) / 100.0f * 2.0f;
  // brush/Deposit.hpp's spacingPx() floors at 0.1 px anyway, but a spacing of
  // zero here would mean an unbounded dab count for any radius, so it is
  // clamped where it enters rather than where it is used.
  return clampf(radii, 0.02f, 8.0f);
}

AbrImportResult importAbrBrushes(std::span<const uint8_t> bytes) {
  AbrImportResult result;

  uint16_t version = 0, subversion = 0;
  if (!readU16(bytes, 0, version) || !readU16(bytes, 2, subversion)) {
    result.error = "not an .abr file: fewer than four bytes.";
    return result;
  }
  if (version != 6) {
    // Versions 1 and 2 are a wholly different layout with no descriptor block
    // at all. Refused by name rather than parsed hopefully, because the framing
    // is the only thing standing between this and reading arbitrary memory.
    result.error = "unsupported .abr version " + std::to_string(version) +
                   " (only version 6 is read; 1 and 2 are a different, much older layout).";
    return result;
  }

  // Walk the 8BIM sections for `desc`.
  size_t off = 4;
  size_t descAt = 0, descLen = 0;
  while (off + 12 <= bytes.size()) {
    if (std::memcmp(bytes.data() + off, "8BIM", 4) != 0) break;
    char key[5] = {0};
    std::memcpy(key, bytes.data() + off + 4, 4);
    uint32_t len = 0;
    if (!readU32(bytes, off + 8, len)) break;
    const size_t body = off + 12;
    // A length that runs past the end is a truncated or hostile file; stop
    // rather than clamping, since a clamped block would parse as a shorter
    // descriptor and silently import half a library.
    if (len > bytes.size() - body) break;
    if (std::strcmp(key, "desc") == 0) {
      descAt = body;
      descLen = len;
      break;
    }
    off = body + len;
    if (len % 2 != 0) ++off;  // sections are word-aligned
  }

  if (descLen == 0) {
    result.error = "no `desc` block: this .abr carries no brush parameters.";
    return result;
  }

  const DescriptorParseResult parsed =
      parseVersionedActionDescriptor(bytes.subspan(descAt, descLen));
  if (!parsed.ok) {
    result.error = "the `desc` block did not parse: " + parsed.error;
    return result;
  }

  const DescriptorRef root(&parsed.tree, 0);
  const DescriptorRef list = root.field("Brsh");
  if (!list.valid() || list.childCount() == 0) {
    result.error = "the `desc` block has no `Brsh` list.";
    return result;
  }

  for (size_t i = 0; i < list.childCount(); ++i)
    result.presets.push_back(presetFromDescriptor(list.child(i), result));

  result.ok = true;
  return result;
}

}  // namespace np
