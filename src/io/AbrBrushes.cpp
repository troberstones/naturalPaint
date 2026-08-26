#include "io/AbrBrushes.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

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

// A sampled tip's own pixel count is bounded here, before a single byte is
// decoded, for the reason io/Descriptor.hpp's `DescriptorParseOptions::
// maxNodes` gives of itself: a hostile `top`/`left`/`bottom`/`right` is four
// numbers this reader has not verified yet, and multiplying two of them
// straight into an allocation size is how a 60-byte file claims a multi-
// gigabyte bitmap. 4096 px is far past anything a real sampled tip needs --
// the largest sample found by direct inspection of a real Kyle Webster pack
// (this file's header) is 120x93 -- and 4096x4096 at one byte per texel is a
// bounded 16 MiB, not the unbounded allocation a raw `width * height` would
// be for an adversarial rectangle.
inline constexpr uint32_t kMaxSampledTipDimension = 4096;

// Photoshop's per-scanline RLE for `samp` image data: `height` big-endian
// u16 compressed-byte-counts, then that many PackBits-compressed bytes,
// decoded as ONE continuous stream (not re-synchronised at each scanline
// boundary) to exactly `expected` bytes.
//
// **Decoding as one stream rather than one call per row is deliberate, not a
// shortcut.** A PackBits run or literal never straddles Photoshop's own row
// boundaries in a well-formed file -- Adobe's own encoder does not emit one
// that does -- so decoding scanline-by-scanline and decoding the whole
// concatenated stream in one pass produce identical bytes for every well-
// formed file, and the single-pass form is what the openly-published
// `abrupng` reader this framing was cross-checked against does too (this
// file's header). Where the two approaches WOULD diverge -- a malformed
// stream where a run crosses a row boundary -- this form still cannot read
// past `end`, because every byte access below is checked against it first;
// it can only decode fewer than `expected` bytes and report the shortfall,
// never more.
bool decodePackBits(std::span<const uint8_t> body, size_t off, size_t end, uint32_t height,
                    size_t expected, std::vector<uint8_t>& out) noexcept {
  if (off > end || end > body.size()) return false;

  // The row-length table: `height` u16s, big-endian, summed for the total
  // compressed byte count -- `abrupng`'s own `read_rle_data()` does the same
  // ("We just need the total length"), which is what makes decoding as one
  // stream rather than `height` separate calls correct rather than merely
  // convenient (see this function's own comment above).
  if (static_cast<uint64_t>(height) * 2u > end - off) return false;
  uint64_t total = 0;
  size_t p = off;
  for (uint32_t i = 0; i < height; ++i) {
    uint16_t rowLen = 0;
    if (!readU16(body, p, rowLen)) return false;
    total += rowLen;
    p += 2;
  }
  if (total > end - p) return false;
  const size_t dataEnd = p + static_cast<size_t>(total);

  out.clear();
  out.reserve(expected);
  while (p < dataEnd && out.size() < expected) {
    const int8_t n = static_cast<int8_t>(body[p]);
    ++p;
    if (n == -128) {
      continue;  // NOP: PackBits' own no-op control byte
    } else if (n < 0) {
      // Run: repeat the next byte (-n + 1) times.
      if (p >= dataEnd) return false;
      const size_t count = static_cast<size_t>(-static_cast<int>(n) + 1);
      const uint8_t b = body[p];
      ++p;
      for (size_t k = 0; k < count && out.size() < expected; ++k) out.push_back(b);
    } else {
      // Literal: the next (n + 1) bytes, verbatim.
      const size_t count = static_cast<size_t>(n) + 1;
      if (p + count > dataEnd) return false;
      out.insert(out.end(), body.data() + p, body.data() + p + count);
      p += count;
    }
  }
  return out.size() == expected;
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

BrushPreset presetFromDescriptor(
    const DescriptorRef& node, AbrImportResult& result,
    const std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>>& sampledTips) {
  BrushPreset p;
  if (const auto nm = node.field("Nm  ").asText()) p.name = std::string(*nm);
  if (p.name.empty()) p.name = "Untitled brush";

  const DescriptorRef brsh = node.field("Brsh");

  // The tip. `sampledData` is a UUID naming a bitmap in the `samp` block --
  // resolved here against the samples `importAbrBrushes()` already decoded,
  // so the lookup (and its one failure note) happens once, in the one place
  // that has both the descriptor and the samples in hand.
  const DescriptorRef sampledDataRef = brsh.field("sampledData");
  std::shared_ptr<const BrushTipBitmap> bitmap;
  if (sampledDataRef.valid()) {
    bool found = false;
    if (const auto idText = sampledDataRef.asText()) {
      const auto it = sampledTips.find(std::string(*idText));
      if (it != sampledTips.end()) {
        bitmap = it->second;
        found = true;
      }
    }
    if (!found) {
      // Covers all three of this file's header's failure modes alike: an id
      // this `samp` block does not contain, a record that did not decode to
      // something trusted, or (here) a `sampledData` whose value was not even
      // `TEXT`. One bucket, because to the brush it claims to be they are the
      // same outcome -- the shape did not arrive.
      ++result.sampledTips;
      result.notes.push_back(
          {p.name, "sampled bitmap tip not imported -- it will paint with the round procedural tip"});
    }
  }
  p.tipBitmap = bitmap;

  double v = 0.0;
  // Diameter is in pixels; radius is half of it. A `#Prc` diameter is a
  // percentage of the sampled tip's own size -- resolvable now that `bitmap`
  // above carries that size, against whichever of its width/height is larger
  // (brush/Deposit.hpp §2c point 2, the same convention `dabCoverage()` uses
  // to map the tip circle onto the bitmap's rectangle, so `Dmtr` and the
  // painted size agree about what "100%" means). Still refused, exactly as
  // before, when there is no bitmap to measure it against.
  const DescriptorRef dmtr = brsh.field("Dmtr");
  if (const auto uf = dmtr.asUnitFloat()) {
    if (uf->unit == "#Prc") {
      if (bitmap) {
        const float nativeMax = static_cast<float>(std::max(bitmap->width, bitmap->height));
        const float pct = static_cast<float>(uf->value);
        p.radius = clampf(nativeMax * (pct / 100.0f) * 0.5f, 0.5f, 4096.0f);
      } else {
        result.notes.push_back(
            {p.name, "diameter is a percentage of a sampled tip; size not imported"});
      }
    } else if (uf->value > 0.0) {
      p.radius = clampf(static_cast<float>(uf->value) * 0.5f, 0.5f, 4096.0f);
    }
  }

  if (unitValue(brsh.field("Hrdn"), v)) p.hardness = clampf(static_cast<float>(v) / 100.0f, 0.0f, 1.0f);
  if (unitValue(brsh.field("Rndn"), v)) p.roundness = clampf(static_cast<float>(v) / 100.0f, 0.01f, 1.0f);
  // **Negated, not copied.** `brush/Deposit.hpp` sect2b's own derivation --
  // `u = dx*cos(angle) + dy*sin(angle)`, so the major axis sits at world
  // direction `(cos(angle), sin(angle))` -- makes positive `BrushTip::angle`
  // a CLOCKWISE turn as viewed on screen, once `dy` is read the way every
  // raster in this build reads it: increasing downward (the same fact
  // `ops/Gradient.hpp`'s `Angular` gradient and `ops/Transform.hpp`'s
  // `transformRotateDegrees()` both derive independently for their own
  // rotations). Photoshop's `Angl` dial is COUNTER-clockwise-positive, the
  // opposite sense -- so a raw copy imports every brush mirrored about the
  // horizontal, invisible on a round tip (`roundness == 1`, angle skipped
  // outright) or at a static 90/180/270 (an ellipse's own 180-degree
  // symmetry hides the mirror at exactly those four headings, which is why
  // Blot Bot 5's `angle 90.0` -- imported on a `roundness 1.00` tip besides
  // -- never surfaced this) and visible on any OTHER angle paired with an
  // elliptical or sampled-bitmap tip. Negating crosses Photoshop's frame
  // into this engine's the same way `abrSpacingToRadii()` crosses a percent-
  // of-diameter into a fraction-of-radius: once, here, rather than leaving
  // every reader of `BrushTip::angle` to remember which file it came from.
  if (unitValue(brsh.field("Angl"), v)) p.angle = static_cast<float>(v);  // HELD: see above
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

  // --- Dual Brush: read far enough to say what was lost --------------------
  //
  // **A Dual Brush is a whole second tip**, stamped through the first with its
  // own blend mode, spacing, scatter and count -- `dualBrush` carries its own
  // `Brsh`, `BlnM`, `Cnt `, `bothAxes`, `countDynamics` and `scatterDynamics`.
  // It is a large part of why Photoshop's ink brushes look granular rather
  // than smooth: the second tip is what breaks up the first tip's edge.
  //
  // This build has no second tip, so it cannot be imported. What it must not
  // do is stay quiet about that, which is exactly what happened until now --
  // `dualBrush` was not read, not counted, and not mentioned, so a brush
  // arrived looking smooth and nothing anywhere said why. That is the same
  // silent-loss failure the sampled-tip note was written to prevent, sitting
  // one key further down the same descriptor.
  //
  // Gated on `useDualBrush` rather than on the object's presence: every one of
  // these presets carries a `dualBrush` object whether or not the feature is
  // switched on, so reporting on presence would fire on brushes that lose
  // nothing and make the note worthless.
  const DescriptorRef dual = node.field("dualBrush");
  if (dual.valid() && dual.field("useDualBrush").asBoolean().value_or(false)) {
    ++result.dualBrushes;
    result.notes.push_back(
        {p.name,
         "Dual Brush is ON and not imported -- a second tip is stamped through this brush in "
         "Photoshop, and its absence is why the mark reads smoother than the original"});
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
    // **Both Direction controls are exact matches, each onto its own
    // source.** Photoshop's own "Direction" is the live stroke tangent,
    // updating dab to dab -- precisely `DynamicSource::Direction`
    // (`brush/Dynamics.hpp`'s `dynamicDirection()`, resolved fresh every
    // dab). Its "Initial Direction" samples the heading ONCE, at the
    // stroke's opening step, and holds that single value fixed for the rest
    // of the stroke -- precisely `DynamicSource::InitialDirection`, which
    // shares `dynamicDirection()`'s own arithmetic but is latched exactly
    // once by `app/StrokeSession` and never re-read (see that source's own
    // section in brush/Dynamics.hpp).
    //
    // **Which ordinal is which was got wrong once, in the direction the
    // evidence did not support.** The claim was that every Runny Inker used
    // Initial Direction and none used the live control -- stated confidently,
    // and backwards. All twelve carry `bVTy = 6`, and Photoshop shows that
    // brush's Angle Control as "Direction" (see `AbrControl`'s own header
    // comment for the reading and the three cross-checks that place it). So
    // every one of them wants the LIVE tangent, turning through the whole
    // curve, and importing them as latched froze each stroke at its opening
    // heading -- a visibly straighter, more uniform mark than the original.
    //
    // The lesson worth keeping is not the swap itself: it is that this was
    // read off an enum that looked orderly rather than off the application
    // that writes the files, and the resulting brushes still painted, still
    // varied, and still looked plausible. Nothing failed. It took someone
    // opening the brush in Photoshop and comparing panels to see it.
    case AbrControl::Direction: out = DynamicSource::Direction; return true;
    case AbrControl::InitialDirection: out = DynamicSource::InitialDirection; return true;
    // Off is not "unmapped", it is "no link", and the caller checks it first.
    case AbrControl::Off:
    // Stylus Wheel is a device axis SDL does not report, so there is no
    // input this build could read to drive it from -- unlike the two
    // Direction controls above, this one is genuinely absent, not merely
    // resolved by a different mechanism.
    case AbrControl::StylusWheel:
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

std::vector<AbrSampledTip> parseAbrSampledTips(std::span<const uint8_t> samp, uint16_t subversion) {
  std::vector<AbrSampledTip> out;

  // Subversion-dependent, per `abrupng`'s own `abr6.rs` (this file's header):
  // the bytes between a record's UUID key and its bounds rectangle changed
  // shape once between subversion 1 and subversion 2, and this reader has no
  // use for what is in them either way, so they are skipped rather than
  // parsed.
  const size_t skipAmt = (subversion == 1) ? 47 : 301;

  size_t off = 0;
  while (off + 4 <= samp.size()) {
    uint32_t recLen = 0;
    if (!readU32(samp, off, recLen)) break;
    const size_t bodyStart = off + 4;
    if (recLen > samp.size() - bodyStart) break;  // truncated record: stop, keep what decoded
    const size_t bodyEnd = bodyStart + recLen;

    AbrSampledTip tip;
    // The key: a literal '$' then a 36-character UUID, 37 bytes, at the very
    // start of the record body -- confirmed by direct inspection of a real
    // pack (this file's header) and not otherwise documented. Missing or
    // short is not fatal to the record; it just means this sample cannot be
    // named, so it can be decoded but never matched by a preset's
    // `sampledData`.
    if (recLen >= 37 && samp[bodyStart] == '$') {
      tip.id.assign(reinterpret_cast<const char*>(samp.data() + bodyStart + 1), 36);
    }

    size_t hoff = bodyStart + skipAmt;
    // 16 (rect) + 2 (depth) + 1 (compression byte) must fit before bodyEnd.
    if (hoff + 19 <= bodyEnd) {
      uint32_t top = 0, left = 0, bottom = 0, right = 0;
      readU32(samp, hoff, top);
      readU32(samp, hoff + 4, left);
      readU32(samp, hoff + 8, bottom);
      readU32(samp, hoff + 12, right);
      hoff += 16;
      uint16_t depth = 0;
      readU16(samp, hoff, depth);
      hoff += 2;
      const uint8_t compressed = samp[hoff];
      hoff += 1;

      // `right > left && bottom > top` rather than trusting the subtraction:
      // both are u32 reads from an untrusted file, and an inverted rect would
      // underflow into a near-4-billion width rather than a negative one.
      // depth == 8: this file's header states what that limit rests on.
      if (right > left && bottom > top && depth == 8 && right - left <= kMaxSampledTipDimension &&
          bottom - top <= kMaxSampledTipDimension) {
        const uint32_t w = right - left;
        const uint32_t h = bottom - top;
        const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h);

        std::vector<uint8_t> alpha;
        bool decoded = false;
        if (compressed != 0) {
          decoded = decodePackBits(samp, hoff, bodyEnd, h, expected, alpha);
        } else if (hoff + expected <= bodyEnd) {
          alpha.assign(samp.data() + hoff, samp.data() + hoff + expected);
          decoded = true;
        }

        if (decoded && alpha.size() == expected) {
          auto bmp = std::make_shared<BrushTipBitmap>();
          bmp->width = static_cast<int32_t>(w);
          bmp->height = static_cast<int32_t>(h);
          bmp->alpha = std::move(alpha);
          tip.bitmap = std::move(bmp);
        }
      }
    }

    if (!tip.id.empty() && tip.bitmap != nullptr) out.push_back(std::move(tip));

    // 4-byte-aligned between records (`abrupng`'s own `(end_pos + 3) & !3`,
    // this file's header) -- NOT the 2-byte word-alignment the top-level 8BIM
    // walk below uses. Getting this wrong desynchronises onto the middle of
    // the next record's own bytes and every sample after the first bad one is
    // garbage that happens to still decode.
    const size_t nextOff = (bodyEnd + 3) & ~static_cast<size_t>(3);
    if (nextOff <= off) break;  // no forward progress: malformed, stop rather than loop
    off = nextOff;
  }

  return out;
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

  // Walk the 8BIM sections for `samp` and `desc`. Both are located here
  // rather than each reading past the other, because a real file (and this
  // module's own `wrapAbr` fixture) puts `samp` BEFORE `desc` -- so a walk
  // that stopped at the first section it specifically wanted would need to
  // run twice, once per section, and disagree with itself about where `off`
  // resumes. One pass, one `off`, every section keeps its own start/length.
  size_t off = 4;
  size_t descAt = 0, descLen = 0;
  size_t sampAt = 0, sampLen = 0;
  bool haveDesc = false;
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
    if (std::strcmp(key, "desc") == 0 && !haveDesc) {
      descAt = body;
      descLen = len;
      haveDesc = true;
    } else if (std::strcmp(key, "samp") == 0) {
      sampAt = body;
      sampLen = len;
    }
    off = body + len;
    if (len % 2 != 0) ++off;  // 8BIM sections are word-aligned (2 bytes) --
                              // NOT `samp`'s own internal 4-byte record
                              // alignment; see `parseAbrSampledTips()`.
  }

  if (descLen == 0) {
    result.error = "no `desc` block: this .abr carries no brush parameters.";
    return result;
  }

  // Decoded before the descriptor is walked, so `presetFromDescriptor()` can
  // resolve a `sampledData` id against real pixels rather than deferring it.
  // Absent or empty `samp` decodes to an empty map, same as a `.abr` with
  // only procedural brushes -- every `sampledData` lookup then misses, which
  // is the existing "not imported" path and not a new failure mode.
  std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>> tipsById;
  if (sampLen > 0) {
    for (AbrSampledTip& tip : parseAbrSampledTips(bytes.subspan(sampAt, sampLen), subversion))
      tipsById.emplace(std::move(tip.id), std::move(tip.bitmap));
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
    result.presets.push_back(presetFromDescriptor(list.child(i), result, tipsById));

  result.ok = true;
  return result;
}

}  // namespace np
