#include "io/AbrBrushes.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>

#include "io/Descriptor.hpp"
#include "brush/BrushModel.hpp"
#include "io/PackBits.hpp"
#include "io/PsPatterns.hpp"

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
// `minimumRoundness` for roundness. It used to be Photoshop's name for
// exactly what `BrushLink::rangeLo` is; for Size (docs/reachability-audit.md
// B6) it is now Photoshop's name for `BrushLinkSet::multiplyFloor[Size]`
// instead -- a floor under the TARGET's whole product, not under any one
// link's own range, which is what keeps a control link and a jitter link on
// the same target from each contributing their own copy of it. Roundness has
// not made that move yet; see this function's own `sizeFloorsAtEngine`.
void addDynamicsLinks(BrushLinkSet& links, const AbrDynamics& d, DynamicTarget target,
                      double floorPercent, const std::string& brushName,
                      AbrImportResult& result) {
  if (!d.present) return;

  const bool angular = targetCombine(target) == TargetCombine::Add;
  // **Size decomposes the floor out of the range entirely (docs/
  // reachability-audit.md B6); Roundness does not, yet.** Both are
  // non-angular (Multiply) targets and both can carry a Photoshop minimum
  // (`minimumDiameter`, `minimumRoundness`) into this same function, so both
  // USED to bake that minimum straight into `lo` below and hand it to every
  // link this call adds -- which is exactly the squaring B6 describes, the
  // moment a control AND a jitter both land on the one target. Size is fixed
  // here because it is the instance the audit measured (eleven of twelve
  // Runny Inkers carry both `PRESSURE->Size` and `RANDOM->Size`) and the
  // instance `BrushLinkSet::multiplyFloor` is wired all the way to a pixel
  // radius (`app/StrokeSession.cpp`'s `BrushTip::sizeFloorPx`). Roundness
  // carries the identical shape and is left on the old behaviour rather than
  // silently changed alongside Size -- see `BrushLinkSet::multiplyFloor`'s
  // own comment on why that gap is named rather than just left.
  const bool sizeFloorsAtEngine = target == DynamicTarget::Size;
  float lo = 0.0f, hi = 1.0f;
  if (angular) {
    targetDefaultRange(target, lo, hi);
  } else if (sizeFloorsAtEngine) {
    // The honest range: a control link spans Size's own whole [0,1], with
    // NOTHING of Photoshop's Minimum Diameter folded in. That floor now
    // lives once, on `links.multiplyFloor[Size]`, applied downstream of
    // every link this function adds rather than baked into each one's own
    // `rangeLo` -- see `BrushLinkSet::multiplyFloor`'s own comment for the
    // whole argument.
    lo = 0.0f;
    hi = 1.0f;
    links.multiplyFloor[static_cast<size_t>(target)] =
        clampf(static_cast<float>(floorPercent) / 100.0f, 0.0f, 1.0f);
  } else {
    lo = clampf(static_cast<float>(floorPercent) / 100.0f, 0.0f, 1.0f);
  }

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
  // value ranges over the bottom 26%..100%. For every non-angular target
  // EXCEPT Size, still bounded below by that same target's own minimum
  // (Roundness's old, unfixed behaviour -- see this function's own comment
  // on `sizeFloorsAtEngine`). For Size, deliberately NOT bounded here any
  // more: `lo` above is `sizeFloorsAtEngine`'s honest 0.0f, and the
  // Minimum Diameter this jitter link used to have blended into its own
  // `rangeLo` now lives once, on `links.multiplyFloor[Size]`, set above --
  // folding it in again here would be the second half of the squaring B6
  // describes, just with the jitter link supplying its own copy instead of
  // the control link's.
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
    } else if (sizeFloorsAtEngine) {
      // The honest depth: how far this jitter dips, `1 - jitter/100`, with no
      // `max()` against the floor -- `links.multiplyFloor[Size]` above
      // already carries the floor once, and this link no longer needs to
      // know it exists.
      l.rangeLo = clampf(1.0f - static_cast<float>(d.jitter) / 100.0f, 0.0f, 1.0f);
      l.rangeHi = 1.0f;
    } else {
      l.rangeLo = std::max(lo, clampf(1.0f - static_cast<float>(d.jitter) / 100.0f, 0.0f, 1.0f));
      l.rangeHi = 1.0f;
    }
    addLink(links, l);
  }
}

// One `Brsh`-shaped tip object -- DIAMETER, ANGLE, ROUNDNESS, SPACING and,
// optionally, a sampled bitmap resolved by UUID against the `samp` block's
// decoded samples. Shared between a preset's PRIMARY tip (`node.field("Brsh")`)
// and a Dual Brush's SECOND tip (`node.field("dualBrush").field("Brsh")`) --
// the two are the identical shape on the wire, direct inspection of a real
// Kyle Webster pack having found `Dmtr`/`Angl`/`Rndn`/`Spcn`/`sampledData` on
// both -- so this is one function rather than two, exactly as this file's own
// header says to prefer.
//
// **Never reads a `dualBrush` key off `brsh`.** Photoshop's own UI cannot put
// a Dual Brush inside a Dual Brush's own tip -- there is no such panel -- but
// this reads untrusted bytes, and `brush/Deposit.hpp` §2d's no-recursion
// guarantee is stated twice on purpose: once here, at import time (this
// function simply never looks), and once more in `dabCoverage()` at composite
// time (which would ignore a second level even if one somehow arrived). A
// hand-crafted `.abr` nesting `dualBrush` inside `dualBrush` cannot make a
// three-tip chain either way.
//
// `readHardness` is false for a Dual Brush's second tip: Photoshop's own Dual
// Brush panel has no Hardness slider at all, so there is no `Hrdn` key on
// that shape to read (confirmed by the nine-key inspection of `dualBrush`
// this step was scoped against: `Brsh` carries `Dmtr`, `Angl`, `Rndn`, `Spcn`
// and, optionally, `sampledData` -- no `Hrdn` among them). `hardness` in the
// result is then simply `defaultHardness`, unread and unclamped further.
//
// `noteLabel` distinguishes the sampled-tip-not-imported / percentage-diameter
// notes' wording between the brush's own tip (empty string, identical text to
// before this function existed) and its Dual Brush tip ("Dual Brush's "), so
// a report reader does not have to guess which of the two shapes lost a
// bitmap.
struct AbrTipShape {
  float radius;
  float hardness;
  float roundness = 1.0f;
  float angle = 0.0f;
  float spacing = 0.25f;
  std::shared_ptr<const BrushTipBitmap> bitmap;
  // The `sampledData` uuid, carried out so the preset can store `abr:<uuid>`
  // and re-find the tip next launch without the pack (brush/Library.hpp's
  // `dabId`). Set whenever the descriptor named one, INCLUDING when the
  // lookup then failed -- a preset that names a tip this build could not
  // decode should still say which tip it wanted.
  std::string sampleId;
};

AbrTipShape readAbrTipShape(
    const DescriptorRef& brsh, float defaultRadius, float defaultHardness, bool readHardness,
    const std::string& brushName, const char* noteLabel, AbrImportResult& result,
    const std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>>& sampledTips) {
  AbrTipShape t;
  t.radius = defaultRadius;
  t.hardness = defaultHardness;

  // `sampledData` is a UUID naming a bitmap in the `samp` block -- resolved
  // here against the samples `importAbrBrushes()` already decoded, so the
  // lookup (and its one failure note) happens once, in the one place that has
  // both the descriptor and the samples in hand.
  const DescriptorRef sampledDataRef = brsh.field("sampledData");
  if (sampledDataRef.valid()) {
    bool found = false;
    if (const auto idText = sampledDataRef.asText()) {
      t.sampleId = std::string(*idText);
      const auto it = sampledTips.find(t.sampleId);
      if (it != sampledTips.end()) {
        t.bitmap = it->second;
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
          {brushName, std::string(noteLabel) +
                          "sampled bitmap tip not imported -- it will paint with the round "
                          "procedural tip"});
    }
  }

  double v = 0.0;
  // Diameter is in pixels; radius is half of it. A `#Prc` diameter is a
  // percentage of the sampled tip's own size -- resolvable now that `t.bitmap`
  // above carries that size, against whichever of its width/height is larger
  // (brush/Deposit.hpp §2c point 2, the same convention `dabCoverage()` uses
  // to map the tip circle onto the bitmap's rectangle, so `Dmtr` and the
  // painted size agree about what "100%" means). Still refused, exactly as
  // before, when there is no bitmap to measure it against.
  const DescriptorRef dmtr = brsh.field("Dmtr");
  if (const auto uf = dmtr.asUnitFloat()) {
    if (uf->unit == "#Prc") {
      if (t.bitmap) {
        const float nativeMax = static_cast<float>(std::max(t.bitmap->width, t.bitmap->height));
        const float pct = static_cast<float>(uf->value);
        t.radius = clampf(nativeMax * (pct / 100.0f) * 0.5f, 0.5f, 4096.0f);
      } else {
        result.notes.push_back(
            {brushName,
             std::string(noteLabel) + "diameter is a percentage of a sampled tip; size not imported"});
      }
    } else if (uf->value > 0.0) {
      t.radius = clampf(static_cast<float>(uf->value) * 0.5f, 0.5f, 4096.0f);
    }
  }

  if (readHardness && unitValue(brsh.field("Hrdn"), v))
    t.hardness = clampf(static_cast<float>(v) / 100.0f, 0.0f, 1.0f);
  if (unitValue(brsh.field("Rndn"), v)) t.roundness = clampf(static_cast<float>(v) / 100.0f, 0.01f, 1.0f);
  // **`Angl` is copied, NOT negated, and that is an open question rather than
  // a decision.** `BrushTip::angle` is clockwise-positive as seen on screen --
  // `brush/Deposit.hpp` sect2b's rotation puts the major axis at world
  // direction `(cos a, sin a)` and `dy` increases downward, the same fact
  // `ops/Gradient.hpp` and `ops/Transform.hpp` derive independently for their
  // own rotations. That half is settled. Whether Photoshop's `Angl` dial is
  // the OPPOSITE sense is NOT: it was asserted from memory rather than read
  // off the application, and this file has already carried one control
  // ordinal backwards from exactly that kind of confident recollection (see
  // `AbrControl` in the header). If Photoshop is CCW-positive this line needs
  // a `-`, and every brush pairing a non-zero `Angl` with a non-round tip is
  // currently mirrored. Unobservable in both sample packs today, since none
  // pairs a non-zero static `Angl` with an elliptical or bitmap tip.
  // docs/reachability-audit.md **B9** names the one observation that closes
  // it. `selftest/AbrBrushes` pins the MAGNITUDE with `fabs` and asserts no
  // sign, deliberately, so that a guess is not canonized here.
  if (unitValue(brsh.field("Angl"), v)) t.angle = static_cast<float>(v);
  if (unitValue(brsh.field("Spcn"), v)) t.spacing = abrSpacingToRadii(v);

  return t;
}

// `BlnM` (Dual Brush) and `textureBlendMode` (Texture) share this table,
// because they are the same question asked in two panels: how a second
// coverage value combines with the first (brush/CoverageBlend.hpp). An id not
// listed is left at the caller's default rather than approximated.
//
// **Provenance, per id, kept in full because the confidence genuinely
// differs between them** -- this began as the Dual Brush's own if/else chain
// and the evidence is the reason each line is here:
//
// **`Mltp`/`Ovrl` cross-checked against `psd_tools.terminology`
// (Adobe's own Action Descriptor `BlnM` enumeration, independently
// reverse-engineered), not against a real `.abr`** -- this build's
// PLAN.md forbids shipping one, and no `dualBrush.BlnM` field has been
// read from a real Kyle Webster file the way `AbrControl`'s own header
// reads a real `bVTy` off "Blot Bot Perfecto". Treat this pair with the
// same "inferred, not observed" caution that header gives control 7,
// and re-check against a real file's bytes before trusting it further.
// **`CBrn` -- Color Burn. HIGH confidence, TWO independent sources,
// both giving the identical terse id in the identical enum family
// `Mltp`/`Ovrl` already came from:**
//   1. `psd_tools/terminology.py`'s `Enum` class -- the SAME class,
//      not a sibling one -- carries `ColorBurn = b"CBrn"` alongside
//      `Multiply = b"Mltp"` and `Overlay = b"Ovrl"`.
//   2. `ag-psd` (Agamnentzar/ag-psd, `src/descriptor.ts`)'s `BlnM`
//      enum -- built specifically to decode/encode THIS field
//      (`db.BlnM` for a `dualBrush`, `fx['Md  ']` for a layer effect,
//      the same enum both places) -- has `'color burn': 'CBrn'`.
// No caution needed the way `hMix` below needs it: this id is short,
// matches the naming convention of every other original-era mode in
// both tables (`Drkn`, `Lghn`, `Scrn`, `Dfrn`, `Xclu`...), and both
// sources that use it are purpose-built for exactly this field.
// **`hMix` -- Hard Mix. MEDIUM confidence, and the caveat is worth
// keeping rather than smoothing over.** `psd_tools/constants.py`'s
// `BlendMode` enum (a DIFFERENT table from `terminology.py`'s `Enum`
// above -- it serialises a PSD layer record's fixed 4-byte
// blend-mode-key field, not an Action Descriptor value) has
// `HARD_MIX = b"hMix"`. That table is NOT purpose-built for `BlnM`
// and its vocabulary provably differs from `terminology.py`'s for the
// SAME concept -- it spells Color Burn `b"idiv"`, not `CBrn` -- so
// `hMix` being real in ONE Adobe wire format does not by itself prove
// it is what `dualBrush.BlnM` emits. Independently, `ag-psd`'s `BlnM`
// enum (the one built for this exact field, cited above) spells Hard
// Mix the LONG way, `'hard mix': 'hardMix'`, alongside every other
// "second-generation" mode added after the original terse set
// (`linearBurn`, `darkerColor`, `linearDodge`, `lighterColor`,
// `vividLight`, `linearLight`, `pinLight`, `blendSubtraction`,
// `blendDivide`) -- a pattern `hMix` breaks and `hardMix` fits.
//
// **The tie is broken by the bytes, first-hand.** Scanned both sample
// packs directly: `threeOtherBrushes.abr` contains the literal
// `hMix` twice and `hardMix` zero times, and both occurrences sit at
// the end of the same key run every Dual Brush descriptor in these
// files has -- `Dmtr Hrdn Angl Rndn Spcn Intr flipX flipY
// sampledData`, then `BlnM enum`, then `useScatter` -- so this is
// `dualBrush.BlnM` and not some other descriptor's blend field. `hMix`
// is therefore the spelling that actually occurs; `hardMix` is kept as
// an accepted alias only because `ag-psd` documents it and accepting an
// id that never arrives costs nothing, while refusing one that does
// costs a brush. The MEANING was never in question either way -- both
// spellings mean Hard Mix in every source checked, and none suggests a
// third reading.
// **`linearHeight` -- deliberately NOT mapped.** `ag-psd`'s `BlnM`
// enum (again, the table built for this exact field) has
// `'linear height': 'linearHeight'`, with its own "// used in ABR"
// comment -- so this id is confirmed real and confirmed to appear in
// exactly this context, not a typo for "Linear Light". Confirmed
// first-hand too: `runny_inkers.abr` carries `linearHeight` twice, both
// times at the end of the same Dual Brush key run described for `hMix`
// above, so it genuinely is a `dualBrush.BlnM` value and not a
// Texture-panel field that merely looks like one. But "Linear
// Height" is not one of Photoshop's paint/layer blend modes at all:
// the Krita `abr_brush_importer` plugin's own texture-mode table
// (`kpp_writer.py`, `_map_ps_texture_mode()`) lists "Height" and
// "Linear Height" as TEXTURE-panel compositing modes -- how a
// pattern's grayscale HEIGHT MAP blends into a stroke, not how two
// coverage discs blend into each other. No source found gives that a
// per-pixel formula, so this id is left unmapped and falls through to
// `dualBrushUnsupportedBlend` below, honestly, rather than reusing one
// of the four color-blend formulas as a guess.
//
// **What changed when the two panels merged onto one table.** `linearHeight`
// is now MAPPED rather than dropped. The reasoning above still stands
// unaltered -- no source gives it a per-pixel formula and this build still
// refuses to invent one -- but there is a difference between "cannot read
// this field" and "read it, and cannot render what it says". Mapping it lets
// the importer NAME what it found and count it; `coverageBlendIsRenderable()`
// is the question a caller asks before using one, and it is false for
// `linearHeight` alone. `Hght` ("Height", 31 of the 84 textured presets and
// the most common texture blend) IS mapped, to Zimmer's subtractive
// height-vs-pressure comparison, which is stated as an approximation where it
// is implemented rather than as a decoded formula.
// `BlnM` and `textureBlendMode` share this table, because they are the same
// question asked in two panels (brush/BrushModel.hpp's `CoverageBlend`). Every
// id below was observed in a real pack; an id not here is left at the caller's
// default rather than approximated.
bool coverageBlendFromId(const std::string& id, CoverageBlend& out) noexcept {
  if (id == "Mltp") { out = CoverageBlend::Multiply; return true; }
  if (id == "Ovrl") { out = CoverageBlend::Overlay; return true; }
  if (id == "CBrn") { out = CoverageBlend::ColorBurn; return true; }
  if (id == "hMix" || id == "hardMix") { out = CoverageBlend::HardMix; return true; }
  if (id == "linearBurn") { out = CoverageBlend::LinearBurn; return true; }
  if (id == "CDdg") { out = CoverageBlend::ColorDodge; return true; }
  if (id == "Drkn") { out = CoverageBlend::Darken; return true; }
  if (id == "Sbtr") { out = CoverageBlend::Subtract; return true; }
  if (id == "Hght") { out = CoverageBlend::Height; return true; }
  if (id == "linearHeight") { out = CoverageBlend::LinearHeight; return true; }
  return false;
}

BrushPreset presetFromDescriptor(
    const DescriptorRef& node, AbrImportResult& result,
    const std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>>& sampledTips) {
  BrushPreset p;
  if (const auto nm = node.field("Nm  ").asText()) p.name = std::string(*nm);
  if (p.name.empty()) p.name = "Untitled brush";

  const AbrTipShape primary = readAbrTipShape(node.field("Brsh"), p.radius, p.hardness,
                                              /*readHardness=*/true, p.name, "", result,
                                              sampledTips);
  p.tipBitmap = primary.bitmap;
  // The durable half of the same thing (brush/Library.hpp's `dabId`): the
  // pointer above lives as long as the library stays loaded, this id survives
  // a relaunch once app/DabLibrary has written the tip out.
  if (!primary.sampleId.empty()) p.dabId = "abr:" + primary.sampleId;
  p.radius = primary.radius;
  p.hardness = primary.hardness;
  p.roundness = primary.roundness;
  p.angle = primary.angle;
  p.spacing = primary.spacing;

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
    // `addDynamicsLinks()` just above resolved the (up to two) links it added
    // in DIAMETER units -- `targetDefaultRange(Scatter)`'s generic [0,1]
    // span, the same math Angle and Hue share and are already correct under.
    // Scatter alone needs `abrScatterFractionToRadii()`'s factor of two,
    // applied here, once, to whichever Scatter links this call just added -- see
    // that function's own header comment for why the shared math upstream is
    // left alone rather than threading the conversion through it.
    for (BrushLink& l : p.links.links) {
      if (l.target != DynamicTarget::Scatter) continue;
      l.rangeLo = abrScatterFractionToRadii(l.rangeLo);
      l.rangeHi = abrScatterFractionToRadii(l.rangeHi);
    }
    // Photoshop's Scatter panel has its own "Both Axes" checkbox, a sibling
    // of `useScatter` and `scatterDynamics` rather than something inside the
    // latter -- confirmed against a real Kyle Webster pack (every scattering
    // preset in both sample libraries carries `useScatter`, `bothAxes` and
    // `scatterDynamics` as three consecutive top-level keys). Unticked
    // (false) is Photoshop's own default and this build's fallback alike
    // when the key is absent, so an older or hand-built descriptor with no
    // `bothAxes` at all imports as PERPENDICULAR scatter, not isotropic.
    p.scatterBothAxes = node.field("bothAxes").asBoolean().value_or(false);
  }

  // --- Dual Brush: a whole second tip, stamped through the first -----------
  //
  // `dualBrush` carries its own `Brsh` (a second tip: `Dmtr`/`Angl`/`Rndn`/
  // `Spcn`, optionally `sampledData`), a `BlnM` blend mode, and its own
  // `useScatter`/`Cnt `/`bothAxes`/`countDynamics`/`scatterDynamics` -- nine
  // keys total, verified by direct inspection. It is a large part of why
  // Photoshop's ink brushes look granular rather than smooth: the second tip
  // is what breaks up the first tip's edge.
  //
  // Gated on `useDualBrush` rather than on the object's presence: every one of
  // these presets carries a `dualBrush` object whether or not the feature is
  // switched on, so reporting on presence would fire on brushes that lose
  // nothing and make the note worthless.
  const DescriptorRef dual = node.field("dualBrush");
  if (dual.valid() && dual.field("useDualBrush").asBoolean().value_or(false)) {
    // `BlnM` first: whether a second tip is worth building at all depends on
    // whether this build can composite it, and the three outcomes below
    // (built / understood-but-unsupported / nothing usable) are told apart by
    // what this enumerated read produces.
    // One table, `coverageBlendFromId()` above, shared with the Texture
    // panel's `textureBlendMode` -- see its comment for where every id came
    // from and how confident each one is. `linearHeight` is now MAPPED rather
    // than dropped: naming a mode this build cannot render is strictly better
    // than reporting an unreadable field, and `coverageBlendIsRenderable()`
    // is the question asked here in its place.
    const auto blendEnum = dual.field("BlnM").asEnumerated();
    std::optional<DualBrushBlend> blend;
    if (blendEnum) {
      CoverageBlend parsed = CoverageBlend::Multiply;
      if (coverageBlendFromId(blendEnum->valueId, parsed) &&
          coverageBlendIsRenderable(parsed)) {
        blend = parsed;
      }
    }

    const DescriptorRef dualBrsh = dual.field("Brsh");
    if (blend.has_value() && dualBrsh.valid()) {
      // The second tip's own shape, through the SAME reader the primary tip
      // uses (`readAbrTipShape()`, this file's own comment on why it is one
      // function). `readHardness=false`: Photoshop's Dual Brush panel has no
      // Hardness slider, so there is no authored value to read, and `1.0f`
      // (a hard edge) is the default here -- a REASONED default, not a
      // measured one: it matches "Hard Round", Photoshop's own default
      // second-tip preset, but was not checked against a real file's second
      // tip, sampled or procedural.
      const AbrTipShape second = readAbrTipShape(dualBrsh, p.radius, /*defaultHardness=*/1.0f,
                                                 /*readHardness=*/false, p.name, "Dual Brush's ",
                                                 result, sampledTips);
      auto dualTip = std::make_shared<BrushTip>();
      dualTip->radius = second.radius;
      dualTip->hardness = second.hardness;
      dualTip->roundness = second.roundness;
      dualTip->angle = second.angle;
      dualTip->spacing = second.spacing;  // parsed, but see dualBrushCadenceNotHonoured below
      dualTip->bitmap = second.bitmap;
      // `dualTip->dualTip` stays null (its default): `readAbrTipShape()` never
      // looked for a nested `dualBrush` key to begin with (this function's own
      // comment), so there is nothing to have carried across even if it had.
      p.dualTip = dualTip;
      p.dualBlend = *blend;

      // The second tip's OWN spacing/scatter/count -- distinct from the
      // primary tip's (brush/Deposit.hpp §2d) and not honoured by this
      // build's compositing, which stamps `dualTip` once, centred on every
      // dab of the first. Read only far enough to say whether that loses
      // anything for THIS brush: Count 1 with scatter off is the one
      // configuration where "stamped once, centred" is not an approximation.
      const bool scatterOn = dual.field("useScatter").asBoolean().value_or(false);
      const int32_t count = dual.field("Cnt ").asInteger().value_or(1);
      if (scatterOn || count != 1) {
        ++result.dualBrushCadenceNotHonoured;
        result.notes.push_back(
            {p.name,
             "Dual Brush's own spacing, scatter and count are not honoured -- its second tip is "
             "stamped once, centred on every dab of the first, rather than scattered its own "
             "number of times"});
      }
    } else if (blendEnum.has_value()) {
      // A `BlnM` was read -- a real enumerated value, not absence -- and it is
      // not one of the two this build composites. Falls back to the primary
      // tip alone, exactly like `dualBrushes` below, but this is the more
      // informative diagnosis and gets its own counter and note so a reader
      // can tell the two apart (this file's own header on `AbrImportResult`).
      ++result.dualBrushUnsupportedBlend;
      result.notes.push_back(
          {p.name, "Dual Brush's blend mode '" + blendEnum->typeId + "." + blendEnum->valueId +
                       "' is not implemented -- painting with the primary tip alone"});
    } else {
      ++result.dualBrushes;
      result.notes.push_back(
          {p.name,
           "Dual Brush is ON and not imported -- a second tip is stamped through this brush in "
           "Photoshop, and its absence is why the mark reads smoother than the original"});
    }
  }

  return p;
}


// ===========================================================================
// The BrushModel path -- Photoshop's own panels, read whole.
// ===========================================================================
//
// **This runs ALONGSIDE `presetFromDescriptor()` above, not instead of it.**
// The engine still paints from `BrushPreset` and its link matrix; this fills
// the model the engine will move to, so the switchover is one commit that
// changes what CONSUMES the data rather than one that changes both producer
// and consumer in the same breath. Until then the model's whole job is to let
// `--abr-report` say what is in the file -- which for Texture, Transfer and
// Scatter Count is the first time anything has said so.

Variance readVariance(const DescriptorRef& owner, const char* key) {
  Variance v;
  const DescriptorRef ref = owner.field(key);
  if (!ref.valid()) return v;
  v.present = true;
  if (const auto c = ref.field("bVTy").asInteger()) {
    // Ordinals outside 0..7 are NOT clamped into range. An unknown control is
    // a control this reader does not understand, and silently calling it
    // `InitialDirection` because 7 is the nearest legal value is the shape of
    // guess that produced the 6/7 defect in the first place.
    if (*c >= 0 && *c <= 7) v.control = static_cast<VarianceControl>(*c);
  }
  double d = 0.0;
  if (unitValue(ref.field("jitter"), d))
    v.jitter = clampf(static_cast<float>(d) / 100.0f, 0.0f, 1.0f);
  if (unitValue(ref.field("Mnm "), d))
    v.minimum = clampf(static_cast<float>(d) / 100.0f, 0.0f, 1.0f);
  if (const auto f = ref.field("fStp").asInteger()) v.fadeSteps = *f;
  return v;
}

bool readBoolField(const DescriptorRef& owner, const char* key, bool fallback = false) {
  if (const auto b = owner.field(key).asBoolean()) return *b;
  return fallback;
}

// A percentage read as a 0..1 fraction. Leaves `out` alone when the key is
// absent, so a struct default survives rather than being overwritten with 0.
void readPercentField(const DescriptorRef& owner, const char* key, float& out) {
  double d = 0.0;
  if (unitValue(owner.field(key), d)) out = static_cast<float>(d) / 100.0f;
}

void readRawField(const DescriptorRef& owner, const char* key, float& out) {
  double d = 0.0;
  if (unitValue(owner.field(key), d)) out = static_cast<float>(d);
}


PsTipShape tipShapeFromDescriptor(
    const DescriptorRef& brsh,
    const std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>>& tipsById) {
  PsTipShape tip;
  if (!brsh.valid()) return tip;

  tip.computed = brsh.classId() == "computedBrush";
  if (const auto id = brsh.field("sampledData").asText()) {
    tip.dab.id = "abr:" + std::string(*id);
    const auto found = tipsById.find(std::string(*id));
    if (found != tipsById.end()) tip.dab.bitmap = found->second;
  }

  double d = 0.0;
  if (const auto dmtr = brsh.field("Dmtr").asUnitFloat()) {
    // A `#Prc` diameter is a percentage of the SAMPLE's own pixel size; with
    // no sample there is nothing to take a percentage of, which is why the
    // unit is checked here and nowhere else. Never observed in any of the 101
    // presets measured -- every `Dmtr` is `#Pxl` -- so this branch is carried
    // for files that have not turned up yet, not for any seen so far.
    if (dmtr->unit == "#Prc") {
      if (tip.dab.bitmap != nullptr) {
        const int32_t larger = std::max(tip.dab.bitmap->width, tip.dab.bitmap->height);
        tip.diameterPx = static_cast<float>(dmtr->value / 100.0 * larger);
      }
    } else {
      tip.diameterPx = static_cast<float>(dmtr->value);
    }
  } else if (unitValue(brsh.field("Dmtr"), d)) {
    tip.diameterPx = static_cast<float>(d);
  }

  readRawField(brsh, "Angl", tip.angleDeg);
  readPercentField(brsh, "Rndn", tip.roundness);
  readRawField(brsh, "Spcn", tip.spacingPercent);
  readPercentField(brsh, "Hrdn", tip.hardness);
  tip.spacingEnabled = readBoolField(brsh, "Intr", true);
  tip.flipX = readBoolField(brsh, "flipX");
  tip.flipY = readBoolField(brsh, "flipY");
  return tip;
}

PsScatter scatterFromDescriptor(const DescriptorRef& owner, const char* enableKey) {
  PsScatter sc;
  sc.enabled = readBoolField(owner, enableKey);
  sc.scatter = readVariance(owner, "scatterDynamics");
  sc.bothAxes = readBoolField(owner, "bothAxes");
  double d = 0.0;
  if (unitValue(owner.field("Cnt "), d)) sc.count = static_cast<int32_t>(d);
  sc.countJitter = readVariance(owner, "countDynamics");
  return sc;
}

BrushModel brushModelFromDescriptor(
    const DescriptorRef& brush,
    const std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>>& tipsById) {
  BrushModel m;

  m.tip = tipShapeFromDescriptor(brush.field("Brsh"), tipsById);

  // --- Shape Dynamics ---
  m.shape.enabled = readBoolField(brush, "useTipDynamics");
  m.shape.size = readVariance(brush, "szVr");
  m.shape.angle = readVariance(brush, "angleDynamics");
  m.shape.roundness = readVariance(brush, "roundnessDynamics");
  // Photoshop's Minimum Diameter and Minimum Roundness ARE the floors of their
  // variances, and folding them in here is what leaves exactly one minimum per
  // site -- the structural half of audit B6 (brush/Variance.hpp).
  readPercentField(brush, "minimumDiameter", m.shape.size.minimum);
  readPercentField(brush, "minimumRoundness", m.shape.roundness.minimum);
  m.shape.flipXJitter = readBoolField(brush, "flipX");
  m.shape.flipYJitter = readBoolField(brush, "flipY");
  m.shape.brushProjection = readBoolField(brush, "brushProjection");
  readPercentField(brush, "tiltScale", m.shape.tiltScale);

  // --- Scattering ---
  m.scatter = scatterFromDescriptor(brush, "useScatter");

  // --- Texture ---
  m.texture.enabled = readBoolField(brush, "useTexture");
  {
    const DescriptorRef txtr = brush.field("Txtr");
    if (txtr.valid()) {
      if (const auto id = txtr.field("Idnt").asText())
        m.texture.pattern.id = std::string(*id);
      if (const auto nm = txtr.field("Nm  ").asText())
        m.texture.pattern.name = std::string(*nm);
    }
  }
  m.texture.invert = readBoolField(brush, "InvT");
  readRawField(brush, "textureScale", m.texture.scalePercent);
  readPercentField(brush, "textureDepth", m.texture.depth);
  readPercentField(brush, "minimumDepth", m.texture.minimumDepth);
  m.texture.depthJitter = readVariance(brush, "textureDepthDynamics");
  readRawField(brush, "textureBrightness", m.texture.brightness);
  readRawField(brush, "textureContrast", m.texture.contrast);
  m.texture.eachTip = readBoolField(brush, "TxtC");
  m.texture.protectTexture = readBoolField(brush, "protectTexture");
  if (const auto blend = brush.field("textureBlendMode").asEnumerated())
    coverageBlendFromId(blend->valueId, m.texture.blend);

  // --- Dual Brush ---
  {
    const DescriptorRef dual = brush.field("dualBrush");
    if (dual.valid()) {
      // Gated on `useDualBrush`, never on the object's presence: every real
      // preset carries the object, so presence says nothing at all.
      m.dual.enabled = readBoolField(dual, "useDualBrush");
      m.dual.tip = tipShapeFromDescriptor(dual.field("Brsh"), tipsById);
      m.dual.scatter = scatterFromDescriptor(dual, "useScatter");
      m.dual.flip = readBoolField(dual, "Flip");
      if (const auto blend = dual.field("BlnM").asEnumerated())
        coverageBlendFromId(blend->valueId, m.dual.blend);
    }
  }

  // --- Color Dynamics ---
  m.color.enabled = readBoolField(brush, "useColorDynamics");
  m.color.perTip = readBoolField(brush, "colorDynamicsPerTip");
  m.color.foregroundBackground = readVariance(brush, "clVr");
  readPercentField(brush, "H   ", m.color.hueJitter);
  readPercentField(brush, "Strt", m.color.saturationJitter);
  readPercentField(brush, "Brgh", m.color.brightnessJitter);
  readPercentField(brush, "purity", m.color.purity);

  // --- Transfer ---
  m.transfer.enabled = readBoolField(brush, "usePaintDynamics");
  m.transfer.opacity = readVariance(brush, "opVr");
  m.transfer.flow = readVariance(brush, "prVr");
  m.transfer.wetness = readVariance(brush, "wtVr");
  m.transfer.mix = readVariance(brush, "mxVr");

  // --- The options bar state Photoshop saves WITH the preset ---
  {
    const DescriptorRef opts = brush.field("toolOptions");
    if (opts.valid()) {
      if (const auto md = opts.field("Md  ").asEnumerated()) m.options.blendMode = md->valueId;
      readPercentField(opts, "Opct", m.options.opacity);
      readPercentField(opts, "flow", m.options.flow);
      m.options.smoothing = readBoolField(opts, "smoothing", true);
      m.options.pressureOverridesSize = readBoolField(opts, "usePressureOverridesSize");
      m.options.pressureOverridesOpacity = readBoolField(opts, "usePressureOverridesOpacity");
      m.options.useLegacy = readBoolField(opts, "useLegacy");
      m.options.sizeOverride = readVariance(opts, "szVr");
      m.options.opacityOverride = readVariance(opts, "opVr");
      m.options.flowOverride = readVariance(opts, "prVr");
      m.options.colorOverride = readVariance(opts, "clVr");
    }
  }

  // --- The checkbox tail ---
  m.noise = readBoolField(brush, "Nose");
  m.wetEdges = readBoolField(brush, "Wtdg");
  m.airbrush = readBoolField(brush, "Rpt ");
  m.brushPose = readBoolField(brush, "useBrushPose");

  return m;
}


// The Texture panel, into `GrainParams`.
//
// **This is the one place a `.abr`'s own paper reaches the deposit**, and it
// goes through `BrushPreset::grain` rather than waiting for the model to be
// consumed -- because `grain` already exists, is already persisted by
// app/UserBrushLibraryStore, and is already sampled by all four deposit
// routes. 84 of the 101 presets measured switch Texture on; before this every
// one of them painted on the procedural lattice or on nothing.
//
// Returns false, with `grain` untouched, when the brush names a pattern this
// file's `patt` block does not contain or whose blend mode has no formula --
// the caller counts those rather than substituting a different paper, for the
// same reason a missing sampled tip falls back to a round dab loudly.
bool grainFromTexture(const PsTexture& texture,
                      const std::unordered_map<std::string, std::shared_ptr<const PaperField>>&
                          patternsById,
                      GrainParams& grain, std::string& why) {
  if (!texture.enabled) return false;
  if (texture.pattern.id.empty()) {
    why = "Texture is on but names no pattern";
    return false;
  }
  const auto found = patternsById.find(texture.pattern.id);
  if (found == patternsById.end() || found->second == nullptr) {
    why = "Texture names pattern '" + texture.pattern.name +
          "' which this file's `patt` block does not contain";
    return false;
  }
  if (!coverageBlendIsRenderable(texture.blend)) {
    why = std::string("Texture's blend mode '") + coverageBlendName(texture.blend) +
          "' has no per-pixel formula in any source consulted";
    return false;
  }

  grain.enabled = true;
  grain.field = found->second;
  grain.depth = clampf(texture.depth, 0.0f, 1.0f);
  // Photoshop's Scale is a percentage of the pattern's own size. Clamped away
  // from zero because a zero scale is a division, and clamped at the top
  // because a pattern stretched a hundredfold is a flat colour, not paper.
  grain.scale = clampf(texture.scalePercent / 100.0f, 0.01f, 16.0f);
  grain.invert = texture.invert;
  grain.brightness = clampf(texture.brightness / 100.0f, -1.0f, 1.0f);
  grain.contrast = clampf(texture.contrast / 100.0f, -1.0f, 1.0f);
  grain.blend = texture.blend;
  // `strength` stays at its default 1.0: Photoshop's Texture panel has no
  // second multiplier on the tip's coverage, so inventing one from `depth`
  // would be this importer's opinion rather than the file's.
  return true;
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

float abrScatterFractionToRadii(float fractionOfDiameter) noexcept {
  // fraction-of-diameter -> fraction-of-radius. See this file's header for
  // why the input is already a clamped fraction rather than a raw percent.
  return fractionOfDiameter * 2.0f;
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
    // The key: a **Pascal string** -- one length byte, then that many
    // characters -- at the very start of the record body.
    //
    // **This was read as a literal '$' followed by 36 fixed bytes until
    // io/PsPatterns.cpp was written**, and it worked, because `0x24` is both
    // the character '$' and the length 36, and every id Photoshop has written
    // into either block so far is a 36-character UUID. The two readings agree
    // on every real file and disagree on the first one that carries an id of
    // any other length: the old form would refuse it outright (no '$'), and a
    // sample that cannot be named can never be matched by a preset's
    // `sampledData`, so the brush silently falls back to a round dab.
    //
    // The same encoding appears in `patt`, where the ids are the join key the
    // Texture panel resolves against -- so getting it right in one place and
    // not the other would have been two readers disagreeing about the same
    // four bytes. Missing or short is still not fatal to the record; it just
    // means this sample cannot be named.
    if (recLen >= 1) {
      const size_t idLength = samp[bodyStart];
      if (idLength > 0 && recLen - 1 >= idLength)
        tip.id.assign(reinterpret_cast<const char*>(samp.data() + bodyStart + 1), idLength);
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

AbrSectionTable readAbrSections(std::span<const uint8_t> bytes) {
  AbrSectionTable table;

  if (!readU16(bytes, 0, table.version) || !readU16(bytes, 2, table.subversion)) {
    table.error = "not an .abr file: fewer than four bytes.";
    return table;
  }
  // Version 10 is the same layout as version 6. `abrupng`'s own
  // `src/abr/mod.rs` routes `(version == 6 || version == 10) && (subversion
  // == 1 || subversion == 2)` through ONE decoder, and this reader's `samp`
  // framing was derived against that project (this file's header names it).
  // **No version 10 file was available to test against here**, so the layout
  // is inherited rather than observed -- `importAbrBrushes()` says so in a
  // note rather than presenting the assumption as a reading, and the first
  // person to open a real v10 pack sees the assumption instead of a silent
  // misparse. Versions 1 and 2 are a wholly different layout carrying no
  // descriptor block at all, and stay refused by name.
  if (table.version != 6 && table.version != 10) {
    table.error = "unsupported .abr version " + std::to_string(table.version) +
                  " (only versions 6 and 10 are read; 1 and 2 are a different, much older layout).";
    return table;
  }

  size_t off = 4;
  while (off + 12 <= bytes.size()) {
    if (std::memcmp(bytes.data() + off, "8BIM", 4) != 0) break;
    char key[5] = {0};
    std::memcpy(key, bytes.data() + off + 4, 4);
    uint32_t len = 0;
    if (!readU32(bytes, off + 8, len)) break;
    const size_t body = off + 12;
    if (len > bytes.size() - body) break;

    AbrSection section;
    section.key = key;
    section.at = body;
    section.length = len;
    table.sections.push_back(std::move(section));

    off = body + len;
    if (len % 2 != 0) ++off;  // 8BIM sections are word-aligned (2 bytes) --
                              // NOT `samp`'s own internal 4-byte record
                              // alignment; see `parseAbrSampledTips()`.
  }

  table.ok = true;
  return table;
}

AbrImportResult importAbrBrushes(std::span<const uint8_t> bytes) {
  AbrImportResult result;

  const AbrSectionTable table = readAbrSections(bytes);
  if (!table.ok) {
    result.error = table.error;
    return result;
  }

  // The two asymmetric tie-breaks are the previous walk's, preserved
  // bit-for-bit: the FIRST `desc` wins and the LAST `samp` wins. See
  // `AbrSectionTable`'s own comment on why neither is known to matter.
  size_t descAt = 0, descLen = 0;
  size_t sampAt = 0, sampLen = 0;
  bool haveDesc = false;
  for (const AbrSection& section : table.sections) {
    if (section.key == "desc" && !haveDesc) {
      descAt = section.at;
      descLen = section.length;
      haveDesc = true;
    } else if (section.key == "samp") {
      sampAt = section.at;
      sampLen = section.length;
    }
  }

  if (descLen == 0) {
    result.error = "no `desc` block: this .abr carries no brush parameters.";
    return result;
  }

  // The layout for version 10 is inherited from `abrupng`, never observed
  // here (`readAbrSections()`'s own comment). Say so per import rather than
  // letting the assumption pass as a reading -- a note costs nothing on the
  // six-and-only-six-version files every pack examined so far has been.
  if (table.version == 10) {
    result.notes.push_back(
        {"", "this is a version 10 .abr; its layout is assumed identical to version 6 on "
             "abrupng's authority and has never been checked against a real v10 file"});
  }

  // Decoded before the descriptor is walked, so `presetFromDescriptor()` can
  // resolve a `sampledData` id against real pixels rather than deferring it.
  // Absent or empty `samp` decodes to an empty map, same as a `.abr` with
  // only procedural brushes -- every `sampledData` lookup then misses, which
  // is the existing "not imported" path and not a new failure mode.
  // The `patt` block, decoded once per import and keyed by the same UUID a
  // brush's `Txtr` puts in `Idnt` -- verified against real packs, so the join
  // needs nothing invented in between. Patterns no brush references are freed
  // when this map goes out of scope; the referenced ones live on the presets
  // that took a `shared_ptr` to them.
  std::unordered_map<std::string, std::shared_ptr<const PaperField>> patternsById;
  size_t pattAt = 0, pattLen = 0;
  for (const AbrSection& section : table.sections)
    if (section.key == "patt" && pattLen == 0) {
      pattAt = section.at;
      pattLen = section.length;
    }
  if (pattLen > 0) {
    PsPatternResult pat = parseAbrPatterns(bytes.subspan(pattAt, pattLen));
    result.patternsDecoded = pat.patterns.size();
    result.patternsSkipped = pat.skipped;
    for (PsPattern& q : pat.patterns) {
      auto field = std::make_shared<PaperField>();
      field->width = q.width;
      field->height = q.height;
      field->height8 = std::move(q.height8);
      patternsById.emplace(std::move(q.id), std::move(field));
    }
  }

  std::unordered_map<std::string, std::shared_ptr<const BrushTipBitmap>> tipsById;
  if (sampLen > 0) {
    result.tipSamples = parseAbrSampledTips(bytes.subspan(sampAt, sampLen), table.subversion);
    for (const AbrSampledTip& tip : result.tipSamples) tipsById.emplace(tip.id, tip.bitmap);
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

  for (size_t i = 0; i < list.childCount(); ++i) {
    result.presets.push_back(presetFromDescriptor(list.child(i), result, tipsById));
    // The Photoshop-shaped model, filled alongside -- see the
    // `brushModelFromDescriptor()` block's own comment on why both, for now.
    result.models.push_back(brushModelFromDescriptor(list.child(i), tipsById));

    // The Texture panel, resolved against this file's own patterns and
    // attached to the preset that will paint with it.
    const BrushModel& model = result.models.back();
    BrushPreset& preset = result.presets.back();
    if (model.texture.enabled) {
      std::string why;
      if (grainFromTexture(model.texture, patternsById, preset.grain, why)) {
        ++result.texturesApplied;
      } else {
        ++result.texturesNotApplied;
        result.notes.push_back({preset.name, why + " -- painting without paper texture"});
      }
    }
  }

  result.ok = true;
  return result;
}

}  // namespace np
