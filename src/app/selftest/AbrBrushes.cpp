#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/selftest/DescFixture.hpp"
#include "io/AbrBrushes.hpp"

namespace np {
namespace {

// Wrap a descriptor in the `.abr` container: version 6.2, then the 8BIM
// sections. `extraSections` puts a `samp` block in front of the `desc` one,
// which every real file has and which the section walk therefore has to step
// over rather than assume it is first.
std::vector<uint8_t> wrapAbr(const std::vector<uint8_t>& descBody, uint16_t version = 6,
                             bool leadingSamp = true, bool includeDesc = true) {
  DescFixture f;
  f.u16v(version).u16v(2);
  if (leadingSamp) {
    // A short, odd-length `samp` so the word-alignment step is exercised: get
    // that wrong and the reader lands one byte into `8BIM` and finds nothing.
    const std::vector<uint8_t> samp{1, 2, 3};
    f.code("8BIM").code("samp").u32v(static_cast<uint32_t>(samp.size()));
    for (const uint8_t b : samp) f.u8v(b);
    f.u8v(0);  // pad to even
  }
  if (includeDesc) {
    f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(descBody.size()));
    for (const uint8_t b : descBody) f.u8v(b);
  }
  return f.bytes;
}

// One brush preset descriptor, with as much or as little as a test needs.
struct BrushSpec {
  const char* name = "Test Brush";
  double diameterPx = 40.0;
  double spacingPercent = 25.0;
  double roundnessPercent = 100.0;
  double angleDeg = 0.0;
  bool sampled = false;
  bool useTipDynamics = false;
  int sizeControl = 0;
  double sizeJitter = 0.0;
  double minimumDiameter = 0.0;
  // Angle's own control/jitter pair -- `angleDynamics`, read by
  // `readDynamics(node, "angleDynamics")` the identical way `szVr` is read
  // for Size, and gated by the same `useTipDynamics` flag.
  int angleControl = 0;
  double angleJitter = 0.0;
  // Scatter (docs/reachability-audit.md B5). `useScatter` gates whether
  // `scatterDynamics` is APPLIED, exactly as `useTipDynamics` gates `szVr`
  // above; `scatterJitter` is Photoshop's own percentage, OF THE DIAMETER;
  // `bothAxes` is the Scatter panel's own checkbox, a sibling of
  // `useScatter` rather than something inside `scatterDynamics`.
  bool useScatter = false;
  int scatterControl = 0;
  double scatterJitter = 0.0;
  bool bothAxes = false;
  // `dualBrush` present but SWITCHED OFF vs present and ON. Two flags rather
  // than one because those are genuinely different files and the importer is
  // required to tell them apart: every real preset carries a `dualBrush`
  // object whether or not the feature is enabled, so an importer that keyed
  // off the object's presence would report a loss on every brush in the
  // library and the note would mean nothing.
  bool dualBrushPresent = false;
  bool dualBrushOn = false;
};

void appendBrush(DescFixture& f, const BrushSpec& s) {
  // brushPreset: Nm, Brsh, useTipDynamics, minimumDiameter, szVr,
  // angleDynamics, useScatter, bothAxes, scatterDynamics [, dualBrush].
  // **The count is the SUM of what two parallel changes each added.**
  // Taking either side's number alone would under-declare the descriptor,
  // and an Action Descriptor that promises fewer fields than it writes is
  // not a parse error -- the reader simply stops early and the fixture
  // silently tests less than it appears to.
  f.objc("brushPreset", "brushPreset", s.dualBrushPresent ? 10u : 9u);

  f.key4("Nm  ").textv(s.name);

  f.key4("Brsh").objc("sampledBrush", "sampledBrush", s.sampled ? 5u : 4u);
  f.key4("Dmtr").untf("#Pxl", s.diameterPx);
  f.key4("Angl").untf("#Ang", s.angleDeg);
  f.key4("Rndn").untf("#Prc", s.roundnessPercent);
  f.key4("Spcn").untf("#Prc", s.spacingPercent);
  if (s.sampled) f.keyN("sampledData").textv("some-uuid");

  f.keyN("useTipDynamics").boolv(s.useTipDynamics);
  f.keyN("minimumDiameter").untf("#Prc", s.minimumDiameter);

  f.key4("szVr").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(s.sizeControl);
  f.keyN("jitter").untf("#Prc", s.sizeJitter);
  f.key4("Mnm ").untf("#Prc", 0.0);

  f.keyN("angleDynamics").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(s.angleControl);
  f.keyN("jitter").untf("#Prc", s.angleJitter);
  // Third of the three fields `angleDynamics` promises. A `brVr` that declares
  // 3 and writes 2 is not a parse error: the reader takes the NEXT top-level
  // key as this object's missing field and every key after it lands one slot
  // early, so a whole fixture quietly describes a different brush.
  f.key4("Mnm ").untf("#Prc", 0.0);

  // Scatter -- `useScatter` and `bothAxes` are consecutive top-level keys,
  // siblings of `scatterDynamics` rather than fields inside it, matching what
  // `strings` against a real Kyle Webster pack shows (io/AbrBrushes.cpp's own
  // comment at the import site). Written unconditionally, like `szVr` above,
  // because every real preset carries all three whether or not Scatter is
  // switched on.
  f.keyN("useScatter").boolv(s.useScatter);
  f.keyN("bothAxes").boolv(s.bothAxes);
  f.keyN("scatterDynamics").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(s.scatterControl);
  f.keyN("jitter").untf("#Prc", s.scatterJitter);
  f.key4("Mnm ").untf("#Prc", 0.0);

  // The real key carries a whole second tip (`Brsh`, `BlnM`, `Cnt `,
  // `bothAxes`, ...). Only `useDualBrush` is written here, because that is the
  // only field the importer reads -- it has no second tip to put the rest in,
  // and a fixture that carried fields nobody reads would imply otherwise.
  if (s.dualBrushPresent) {
    f.keyN("dualBrush").objc("dualBrush", "dualBrush", 1);
    f.keyN("useDualBrush").boolv(s.dualBrushOn);
  }
}

std::vector<uint8_t> oneBrushLibrary(const BrushSpec& s) {
  DescFixture f;
  f.version();
  f.descriptor("null", "null", 1);
  f.key4("Brsh").vlls(1);
  appendBrush(f, s);
  return f.bytes;
}

// A brushPreset descriptor with NO `bothAxes` key at all -- built by hand
// rather than through `appendBrush()`, which (realistically) always writes
// one. Some older or hand-edited `.abr` might genuinely lack it, and
// `presetFromDescriptor()` must not mis-read a key that is not there; it must
// fall back to Photoshop's own default, PERPENDICULAR scatter -- B5's
// documented choice for the absent case.
std::vector<uint8_t> oneBrushLibraryScatterNoBothAxes(int control, double jitterPercent) {
  DescFixture f;
  f.version();
  f.descriptor("null", "null", 1);
  f.key4("Brsh").vlls(1);

  // brushPreset: Nm, Brsh, useTipDynamics, minimumDiameter, szVr, useScatter,
  // scatterDynamics -- seven keys, no `bothAxes`.
  f.objc("brushPreset", "brushPreset", 7u);
  f.key4("Nm  ").textv("No Both Axes Key");
  f.key4("Brsh").objc("sampledBrush", "sampledBrush", 4u);
  f.key4("Dmtr").untf("#Pxl", 40.0);
  f.key4("Angl").untf("#Ang", 0.0);
  f.key4("Rndn").untf("#Prc", 100.0);
  f.key4("Spcn").untf("#Prc", 25.0);
  f.keyN("useTipDynamics").boolv(false);
  f.keyN("minimumDiameter").untf("#Prc", 0.0);
  f.key4("szVr").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(0);
  f.keyN("jitter").untf("#Prc", 0.0);
  f.key4("Mnm ").untf("#Prc", 0.0);
  f.keyN("useScatter").boolv(true);
  f.keyN("scatterDynamics").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(control);
  f.keyN("jitter").untf("#Prc", jitterPercent);
  f.key4("Mnm ").untf("#Prc", 0.0);
  return f.bytes;
}

}  // namespace

// io/AbrBrushes: reading Photoshop `.abr` libraries into brush/Library presets.
//
// Two halves. The container framing, which parses a format from the internet
// and must refuse rather than guess; and the MAPPING, which is where an import
// silently produces a brush that is not the brush it claims to be -- every
// value it gets wrong is still in range, still plausible, and still paints.
bool runAbrBrushesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- 1. The unit conversion that would be invisible ---------------------
  //
  // **Photoshop's spacing is a percentage of DIAMETER; BrushTip::spacing is in
  // RADII.** Drop the factor of two and every imported brush lays down half or
  // twice the dabs it should -- which reads as "the import made everything
  // grainy" or "the import made everything slow", not as a unit bug, so it
  // could sit unexplained for a long time.
  {
    check(nearf(abrSpacingToRadii(25.0), 0.5f, 1e-6f) &&
              nearf(abrSpacingToRadii(50.0), 1.0f, 1e-6f),
          "abr: spacing converts percent-of-DIAMETER to RADII -- 25% of a diameter is half a "
          "radius, and dropping the 2x halves every imported brush's dab count");
    // A zero spacing would mean an unbounded dab count for any radius.
    check(abrSpacingToRadii(0.0) > 0.0f,
          "abr: a zero spacing is floored where it enters rather than where it is used");

    // **The same class of bug, for Scatter (docs/reachability-audit.md B5).**
    // Photoshop's Scatter jitter is a percentage of DIAMETER; `BrushTip::
    // scatter` is in RADII (brush/Dynamics.hpp: "in radii"). Drop the factor
    // of two and every imported brush scatters at HALF the distance the
    // artist set -- still in range, still plausible, still paints, so it
    // reads as "a slightly tighter brush" rather than as a unit bug.
    // `abrScatterFractionToRadii()` takes the ALREADY-CLAMPED [0,1] fraction
    // `addDynamicsLinks()` resolves a Scatter link's range to (see that
    // function's own header comment for why), so 0.25 is 25% of a diameter,
    // not a raw percent.
    check(nearf(abrScatterFractionToRadii(0.25f), 0.5f, 1e-6f) &&
              nearf(abrScatterFractionToRadii(0.5f), 1.0f, 1e-6f) &&
              nearf(abrScatterFractionToRadii(1.0f), 2.0f, 1e-6f),
          "abr: scatter converts fraction-of-DIAMETER to RADII -- 25% of a diameter is half a "
          "radius, and 100% of a diameter (the jitter dynamics' own ceiling) is a FULL diameter, "
          "two radii, not one");
  }

  // --- 2. Controls this build has no input for ----------------------------
  //
  // Photoshop can drive a parameter from the stroke's direction and from a
  // stylus wheel. Neither exists here. The import must SAY so rather than drop
  // them, because "the imported brush does less than the original" is exactly
  // what an import must not do quietly.
  {
    DynamicSource s{};
    check(abrControlToSource(2, s) && s == DynamicSource::Pressure,
          "abr: Pen Pressure maps to the PRESSURE row");
    check(abrControlToSource(3, s) && s == DynamicSource::Tilt, "abr: Pen Tilt maps to TILT");
    check(abrControlToSource(1, s) && s == DynamicSource::Fade, "abr: Fade maps to FADE");
    check(abrControlToSource(5, s) && s == DynamicSource::Barrel,
          "abr: Rotation maps to BARREL -- a real SDL_PEN_AXIS_ROTATION axis");

    // Stylus Wheel deliberately does NOT also take Barrel. Two different
    // physical inputs in one matrix cell would make one of them a lie.
    check(!abrControlToSource(4, s),
          "abr: Stylus Wheel is UNMAPPED rather than folded onto barrel -- they are different "
          "physical inputs and sharing a cell would silently misreport one");
    // **This assertion used to claim the exact opposite, and it was right at
    // the time.** Until `DynamicSource::Direction` and
    // `DynamicSource::InitialDirection` existed, nothing here computed a
    // stroke heading and both controls were honestly unmapped. Now each maps
    // onto its OWN source, and asserting that they land on DIFFERENT sources
    // is the point: folding both onto the live tangent would import every
    // brush that asked to be oriented ONCE as one that re-orients every dab,
    // which paints a visibly different mark while looking like the import
    // worked. Every one of Kyle Webster's Runny Inkers uses Initial
    // Direction and none uses the live control, so that collapse would have
    // been wrong for every shipped brush in the library rather than for a
    // rare edge case.
    DynamicSource live{}, initial{};
    check(abrControlToSource(6, live) && live == DynamicSource::Direction &&
              abrControlToSource(7, initial) && initial == DynamicSource::InitialDirection &&
              live != initial,
          "abr: the two Direction controls map to two DIFFERENT sources -- the live tangent and "
          "the once-latched heading are not interchangeable");
    // **6 is the LIVE tangent, and this line is here because it was written
    // the other way round first.** The ordinals were taken off an enum that
    // read plausibly rather than off Photoshop, and every brush still
    // imported, still varied and still painted -- the only thing that caught
    // it was opening Blot Bot Perfecto in Photoshop and reading its Angle
    // Control off the panel. Pinning 6 by name here means the next person to
    // touch this pair has to disagree with a specific brush rather than with
    // a plausible-looking ordering. See `AbrControl` in io/AbrBrushes.hpp for
    // the reading and its cross-checks.
    check(std::strcmp(abrControlName(6), "Direction") == 0 &&
              std::strcmp(abrControlName(7), "Initial Direction") == 0,
          "abr: control 6 is named Direction and 7 Initial Direction -- the order Photoshop's "
          "own panel shows for a real brush, not the order the enum looked like it should have");
    check(!abrControlToSource(0, s), "abr: Off maps to no source, and the caller checks it first");
    check(std::strcmp(abrControlName(5), "Rotation") == 0 &&
              std::strcmp(abrControlName(99), "unknown control") == 0,
          "abr: every control has a name for the report, including one the enum does not cover");
  }

  // --- 3. The container refuses rather than guesses ------------------------
  {
    const std::vector<uint8_t> body = oneBrushLibrary({});

    // Versions 1 and 2 are a wholly different layout. Parsing one hopefully
    // would walk arbitrary offsets through a real file.
    const std::vector<uint8_t> v1 = wrapAbr(body, 1);
    const AbrImportResult old = importAbrBrushes(v1);
    check(!old.ok && old.presets.empty() && old.error.find("version") != std::string::npos,
          "abr: an unsupported version is refused BY NAME with no presets, not parsed hopefully");

    const std::vector<uint8_t> tiny{0x00, 0x06};
    check(!importAbrBrushes(tiny).ok, "abr: a file too short for its own header is refused");

    const std::vector<uint8_t> noDesc = wrapAbr(body, 6, true, false);
    const AbrImportResult none = importAbrBrushes(noDesc);
    check(!none.ok && none.error.find("desc") != std::string::npos,
          "abr: a file with no `desc` block is refused -- it carries no brush parameters");

    // A `desc` length running past the end of the file. Clamping it would
    // parse a shorter descriptor and import part of a library while reporting
    // success, which is worse than refusing.
    //
    // **This fixture is built so that clamping SUCCEEDS.** The descriptor body
    // is complete and would parse perfectly; only the declared section length
    // overruns. A simpler test that just chopped bytes off the end passed
    // whether the reader refused or clamped -- the descriptor was broken
    // either way, so the assertion could not tell the two apart and was
    // passing for the wrong reason. This one can: refuse gives ok=false,
    // clamp gives ok=true with a brush in it.
    {
      DescFixture f;
      f.u16v(6).u16v(2);
      f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(body.size() + 100));
      for (const uint8_t b : body) f.u8v(b);
      const AbrImportResult overlong = importAbrBrushes(f.bytes);
      check(!overlong.ok && overlong.presets.empty(),
            "abr: a section whose declared length overruns the file is REFUSED even though the "
            "bytes present would parse -- clamping would import a partial library and call it "
            "success");
    }

    // Every prefix of a good file must refuse or succeed, never crash or hang.
    const std::vector<uint8_t> good = wrapAbr(body);
    bool allPrefixesSurvive = true;
    for (size_t n = 0; n < good.size(); ++n) {
      const std::vector<uint8_t> prefix(good.begin(), good.begin() + static_cast<long>(n));
      const AbrImportResult r = importAbrBrushes(prefix);
      if (r.ok && r.presets.empty()) allPrefixesSurvive = false;
    }
    check(allPrefixesSurvive,
          "abr: every truncation of a good file is handled -- no prefix reports success with "
          "nothing in it");
  }

  // --- 4. The mapping, end to end ------------------------------------------
  {
    BrushSpec spec;
    spec.name = "Inker 7";
    spec.diameterPx = 40.0;
    spec.spacingPercent = 25.0;
    spec.roundnessPercent = 50.0;
    spec.angleDeg = 30.0;
    const AbrImportResult r = importAbrBrushes(wrapAbr(oneBrushLibrary(spec)));
    check(r.ok && r.presets.size() == 1, "abr: a one-brush library imports one brush");
    if (r.presets.size() == 1) {
      const BrushPreset& p = r.presets[0];
      check(p.name == "Inker 7", "abr: the brush keeps its own name");
      // Diameter is a DIAMETER; radius is half of it. Import it whole and
      // every brush arrives twice the size the artist made it.
      check(nearf(p.model.tip.diameterPx / 2.0f, 20.0f, 1e-4f),
            "abr: `Dmtr` is a DIAMETER, so radius is half of it -- importing it whole doubles "
            "every brush");
      // **The angle's MAGNITUDE is pinned; its SIGN is deliberately not.**
      // `brush/Deposit.hpp`'s rotation is clockwise-positive on screen once
      // `dy` is read increasing-downward -- derived here and independently
      // in `ops/Gradient.hpp` and `ops/Transform.hpp`, so that half is
      // settled. Whether Photoshop's `Angl` dial is the OPPOSITE sense is
      // not: it was asserted from memory rather than read off the
      // application, and this project has already shipped one control
      // ordinal backwards from exactly that kind of confident recollection
      // (`AbrControl`'s own header comment). Asserting either sign here
      // would canonize a guess -- and a green suite around a guess is how
      // the last one survived. `fabs` pins everything that IS known (the
      // number survives import unscaled) and leaves the open question to
      // docs/reachability-audit.md **B9**, where it can be closed by looking
      // at Photoshop rather than by reasoning.
      // `p.model.tip.spacingPercent / 100.0f * 2.0f` is RADII -- the old,
      // now-deleted `BrushPreset::spacing` scalar's own unit, which this
      // check compared against directly. `spec.spacingPercent = 25.0`
      // survives import unchanged as a raw percent (checked separately, the
      // importer's own `readRawField()`), and 25% of the diameter is 0.5 of
      // the radius -- `io/AbrBrushes.cpp`'s `abrSpacingToRadii()` names the
      // identical `/ 100 * 2` conversion.
      check(nearf(p.model.tip.spacingPercent / 100.0f * 2.0f, 0.5f, 1e-4f) &&
                nearf(p.model.tip.roundness, 0.5f, 1e-4f) &&
                nearf(std::fabs(p.model.tip.angleDeg), 30.0f, 1e-4f),
            "abr: spacing and roundness arrive unchanged, and `Angl` arrives with its MAGNITUDE "
            "intact (30 degrees, unscaled) -- the SIGN convention is an open question, audit B9, "
            "and is deliberately not asserted either way rather than canonizing a guess");
    }

    // A second, less "special" angle -- 30 above is also what section 9's
    // spacing/roundness checks reuse, so this is one value away from a wrong
    // negation and a coincidence both looking right. 125 has no symmetry
    // (not a multiple of 90) for a sign error to hide behind.
    {
      BrushSpec skew;
      skew.name = "Inker Skew";
      skew.angleDeg = 125.0;
      const AbrImportResult rSkew = importAbrBrushes(wrapAbr(oneBrushLibrary(skew)));
      check(rSkew.ok && rSkew.presets.size() == 1 &&
                nearf(std::fabs(rSkew.presets[0].model.tip.angleDeg), 125.0f, 1e-4f),
            "abr: a second, asymmetric `Angl` (125.0) also survives import at full magnitude -- "
            "125 has no 90-degree symmetry for an error to hide behind, unlike the round number "
            "the section above shares with the spacing/roundness checks");
    }

    // A `desc` block reached only after stepping over an odd-length `samp`
    // section -- which is every real file. Miss the word-alignment pad and the
    // walk lands mid-signature and reports no brushes at all.
    check(r.ok, "abr: the section walk steps over an odd-length `samp` to reach `desc`");
  }

  // --- 5. useTipDynamics gates the dynamics, as the Brush panel does -------
  //
  // Photoshop keeps the authored jitter values with Shape Dynamics switched
  // off. An import that applied them anyway would paint differently from the
  // brush it claims to be -- and the difference would look like the import
  // working, since the marks would vary.
  {
    BrushSpec off;
    off.useTipDynamics = false;
    off.sizeControl = 2;
    off.sizeJitter = 80.0;
    const AbrImportResult a = importAbrBrushes(wrapAbr(oneBrushLibrary(off)));
    check(a.ok && a.presets.size() == 1 && a.presets[0].links.links.empty(),
          "abr: with useTipDynamics OFF, authored jitter is kept by Photoshop but NOT applied "
          "-- so it imports as no links at all");

    BrushSpec on = off;
    on.useTipDynamics = true;
    on.minimumDiameter = 20.0;
    const AbrImportResult b = importAbrBrushes(wrapAbr(oneBrushLibrary(on)));
    check(b.ok && b.presets.size() == 1 && b.presets[0].links.links.size() == 2,
          "abr: with it ON, a control AND a jitter make TWO links -- Photoshop runs both, and "
          "they are different rows of the same matrix column");

    if (b.presets.size() == 1) {
      const BrushLinkSet& links = b.presets[0].links;
      const size_t ctrl = findLink(links, DynamicSource::Pressure, DynamicTarget::Size);
      const size_t rnd = findLink(links, DynamicSource::Random, DynamicTarget::Size);
      check(ctrl != kNoLink && rnd != kNoLink,
            "abr: the control lands on PRESSURE and the jitter on RANDOM");
      // **`minimumDiameter` is no longer rangeLo -- docs/reachability-audit.md
      // B6.** It used to be, and both links carried it: a control link and a
      // jitter link on the same Multiply target each contributing their own
      // copy of the floor makes the floor its own SQUARE at source 0 (0.20 *
      // 0.20 = 0.04, a brush promising "never below 20%" actually bottoming
      // out at 4%). The control link's range is now the honest [0,1] -- Size's
      // whole span, nothing folded in -- and the floor lives once, on
      // `links.multiplyFloor[Size]`, checked below.
      check(ctrl != kNoLink && nearf(links.links[ctrl].rangeLo, 0.0f, 1e-4f) &&
                nearf(links.links[ctrl].rangeHi, 1.0f, 1e-4f),
            "abr: the control link's range is Size's honest [0,1] -- the minimum diameter is no "
            "longer blended into it");
      // 80% jitter varies down to 20% of full -- the DEPTH of the dip, not a
      // floor. No `max()` against `minimumDiameter` any more: with it still
      // folded in here too, this line and the control link's `rangeLo` would
      // both carry 20% independently, and Size being a Multiply target would
      // square them into 4% for exactly the reason the control link's own
      // check just above does not.
      check(rnd != kNoLink && nearf(links.links[rnd].rangeLo, 0.20f, 1e-4f),
            "abr: an 80% jitter varies down to 20% of full -- its own depth, not a floor");
      // The floor itself: recorded once, on the SET, not on either link.
      check(nearf(links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)], 0.20f, 1e-4f),
            "abr: `minimumDiameter` 20% lands on `links.multiplyFloor[Size]` -- Photoshop's "
            "minimum-diameter control and this model's output floor are still the same idea "
            "under two names, just no longer folded into a link's own range");
      // And nowhere else: every other target's slot stays at the "no floor"
      // default, `addDynamicsLinks()` having been asked for Size alone here.
      size_t nonzero = 0;
      for (size_t t = 0; t < kDynamicTargetCount; ++t)
        if (links.multiplyFloor[t] != 0.0f) ++nonzero;
      check(nonzero == 1,
            "abr: exactly one target's floor is set -- a Size-only fixture touches no other "
            "slot of `multiplyFloor`, including the Add-target ones the header says stay unused");
    }
  }

  // --- 6. What the import could not bring across, per brush ----------------
  //
  // The one that decides whether this feature is honest. A sampled brush's
  // shape is a bitmap and this build has no bitmap tip, so an imported Kyle
  // brush behaves like the original and does not look like it. Said per brush,
  // in the result, rather than left to be discovered.
  {
    BrushSpec sampled;
    sampled.sampled = true;
    sampled.useTipDynamics = true;
    // **Stylus Wheel, and deliberately not Initial Direction.** This fixture
    // used to reach for control 6 as its example of "no source here", which
    // was true until `DynamicSource::InitialDirection` existed and silently
    // stopped being true the moment it did -- at which point this whole
    // section asserted nothing, because a brush that lost NOTHING produces no
    // notes and no unmapped count. Stylus Wheel is a device axis SDL does not
    // report at all, so it is absent for a reason no new source can quietly
    // resolve, which is what makes it the durable choice here.
    sampled.sizeControl = 4;  // Stylus Wheel -- genuinely no input to drive it
    const AbrImportResult r = importAbrBrushes(wrapAbr(oneBrushLibrary(sampled)));
    check(r.ok && r.sampledTips == 1,
          "abr: a sampled bitmap tip is COUNTED as not imported -- the brush will paint with "
          "the round procedural tip and the import says so");
    check(r.unmappedControls == 1,
          "abr: a control with no source here is counted rather than dropped");
    bool saidTip = false, saidControl = false;
    for (const AbrImportNote& n : r.notes) {
      if (n.what.find("sampled bitmap") != std::string::npos) saidTip = true;
      if (n.what.find("Stylus Wheel") != std::string::npos) saidControl = true;
    }
    check(saidTip && saidControl,
          "abr: and both are named in the notes, against the brush that lost them");

    // A brush that lost nothing produces no notes -- otherwise the report is
    // noise and nobody reads the one line that matters.
    BrushSpec clean;
    const AbrImportResult q = importAbrBrushes(wrapAbr(oneBrushLibrary(clean)));
    check(q.ok && q.notes.empty() && q.sampledTips == 0,
          "abr: a brush that lost nothing produces NO notes, so the report is signal");
  }

  // --- 7. Dual Brush, the loss that was silent for longest ------------------
  //
  // Photoshop stamps a SECOND tip through the first, with its own blend mode,
  // spacing, scatter and count, and it is most of why an ink brush reads
  // granular rather than smooth. Eight of the twelve Runny Inkers switch it
  // on. This build has one tip per brush, so it cannot be honoured -- and for
  // as long as `dualBrush` was simply never read, an imported brush came out
  // smooth and NOTHING anywhere said why. Not a note, not a counter, not a
  // line in `--abr-report`. The sampled-tip note two sections up exists to
  // prevent exactly that, one descriptor key away.
  //
  // The discriminating pair is present-and-off vs present-and-on. Keying off
  // the object's mere presence would fire on every preset in a real library,
  // including ones that lose nothing, and a note that fires always carries no
  // information at all.
  {
    BrushSpec off;
    off.dualBrushPresent = true;
    off.dualBrushOn = false;
    const AbrImportResult a = importAbrBrushes(wrapAbr(oneBrushLibrary(off)));
    check(a.ok && a.dualBrushes == 0 && a.notes.empty(),
          "abr: a `dualBrush` object that is present but switched OFF loses nothing, so it is "
          "not counted and produces no note -- every real preset carries this object");

    BrushSpec on;
    on.dualBrushPresent = true;
    on.dualBrushOn = true;
    const AbrImportResult b = importAbrBrushes(wrapAbr(oneBrushLibrary(on)));
    bool saidDual = false;
    for (const AbrImportNote& n : b.notes)
      if (n.what.find("Dual Brush") != std::string::npos) saidDual = true;
    check(b.ok && b.dualBrushes == 1 && saidDual,
          "abr: Dual Brush switched ON is COUNTED and NAMED against its brush -- a second tip "
          "this build cannot stamp is a loss, and an unreported loss is the whole failure mode");
  }

  // --- 8. Angle Jitter 0% plus a live control adds NO spurious RANDOM link --
  //
  // Kyle Webster's "Blot Bot Perfecto" shows "Angle Jitter 0%, Control:
  // Direction" on Photoshop's own Shape Dynamics panel -- the tip should
  // follow the stroke's heading exactly, with no random contribution mixed
  // in. `addDynamicsLinks()` (this file) gates its RANDOM branch on
  // `d.jitter > 0.0`, so a control with zero jitter must import as ONE link,
  // not two -- Angle is an `Add` target, and a spurious RANDOM->Angle link
  // at even a narrow range would still wobble a "0% jitter" brush every dab.
  {
    BrushSpec direction0;
    direction0.useTipDynamics = true;
    direction0.angleControl = static_cast<int>(AbrControl::Direction);
    direction0.angleJitter = 0.0;
    const AbrImportResult r = importAbrBrushes(wrapAbr(oneBrushLibrary(direction0)));
    check(r.ok && r.presets.size() == 1, "abr: the zero-jitter Direction fixture imports");
    if (r.presets.size() == 1) {
      const BrushLinkSet& links = r.presets[0].links;
      check(findLink(links, DynamicSource::Direction, DynamicTarget::Angle) != kNoLink,
            "abr: Control: Direction lands on DIRECTION -> ANGLE");
      check(findLink(links, DynamicSource::Random, DynamicTarget::Angle) == kNoLink,
            "abr: Angle Jitter 0% adds NO RANDOM -> ANGLE link -- '0%' means exactly that, "
            "not a link at a zero-width range that would still look live to a reader of the "
            "matrix");
      check(links.links.size() == 1, "abr: exactly one link total -- the control, nothing else");
    }

    // The contrasting case, so the check above is a real gate and not an
    // importer that dropped RANDOM->Angle altogether: nonzero jitter DOES
    // add the second link, at the correctly SCALED span (half the jitter
    // percentage is half the target's own default [0,360] span, not a
    // fully-random [0,360] -- io/AbrBrushes.cpp's own comment on why the
    // span is scaled rather than selected).
    BrushSpec direction50;
    direction50.useTipDynamics = true;
    direction50.angleControl = static_cast<int>(AbrControl::Direction);
    direction50.angleJitter = 50.0;
    const AbrImportResult r2 = importAbrBrushes(wrapAbr(oneBrushLibrary(direction50)));
    check(r2.ok && r2.presets.size() == 1, "abr: the 50%-jitter Direction fixture imports");
    if (r2.presets.size() == 1) {
      const size_t rnd =
          findLink(r2.presets[0].links, DynamicSource::Random, DynamicTarget::Angle);
      check(rnd != kNoLink, "abr: Angle Jitter 50% DOES add a RANDOM -> ANGLE link");
      if (rnd != kNoLink) {
        const BrushLink& l = r2.presets[0].links.links[rnd];
        check(nearf(l.rangeLo, 0.0f, 1e-4f) && nearf(l.rangeHi, 180.0f, 1e-4f),
              "abr: 50% of Angle's own default [0,360] span is [0,180] -- half a turn, not a "
              "whole one");
      }
    }
  }

  // --- 9. Scatter: the unit end to end, and the axis flag -------------------
  //
  // docs/reachability-audit.md B5, both halves. The pure conversion is
  // section 1's job; this is the mapping proving `presetFromDescriptor()`
  // actually calls it, on the actual links `addDynamicsLinks()` adds, and
  // that "Both Axes" -- a top-level sibling of `useScatter`, not a field
  // inside `scatterDynamics` -- imports (or, absent, defaults to
  // Photoshop's own perpendicular).
  {
    // useScatter OFF keeps whatever jitter/control/bothAxes the descriptor
    // carries but does not apply them -- section 5's gating, restated for
    // Scatter.
    BrushSpec off;
    off.useScatter = false;
    off.scatterControl = 2;  // Pen Pressure
    off.scatterJitter = 80.0;
    off.bothAxes = true;
    const AbrImportResult offR = importAbrBrushes(wrapAbr(oneBrushLibrary(off)));
    check(offR.ok && offR.presets.size() == 1 &&
              findLink(offR.presets[0].links, DynamicSource::Random, DynamicTarget::Scatter) ==
                  kNoLink &&
              findLink(offR.presets[0].links, DynamicSource::Pressure, DynamicTarget::Scatter) ==
                  kNoLink,
          "abr: useScatter OFF imports no Scatter links at all, even with jitter and a control "
          "authored -- Photoshop keeps the values and does not apply them");

    // useScatter ON, jitter only (Control: Off): one RANDOM link, and its
    // range is the unit conversion this section exists to prove end to end.
    BrushSpec jitterOnly;
    jitterOnly.useScatter = true;
    jitterOnly.scatterControl = 0;  // Off
    jitterOnly.scatterJitter = 50.0;  // 50% of the DIAMETER
    jitterOnly.bothAxes = false;
    const AbrImportResult jR = importAbrBrushes(wrapAbr(oneBrushLibrary(jitterOnly)));
    check(jR.ok && jR.presets.size() == 1, "abr: a scattering brush imports");
    if (jR.ok && jR.presets.size() == 1) {
      const BrushPreset& p = jR.presets[0];
      const size_t rnd = findLink(p.links, DynamicSource::Random, DynamicTarget::Scatter);
      check(rnd != kNoLink && p.links.links.size() == 1,
            "abr: jitter alone (Control: Off) makes exactly ONE Scatter link, on RANDOM");
      // 50% of a DIAMETER is 1.0 RADII, not 0.5 -- the exact half-vs-whole
      // gap B5 describes, now checked past the whole import, not just the
      // isolated conversion function.
      check(rnd != kNoLink && nearf(p.links.links[rnd].rangeLo, 0.0f, 1e-4f) &&
                nearf(p.links.links[rnd].rangeHi, 1.0f, 1e-4f),
            "abr: a 50% Scatter jitter imports as up to 1.0 RADII of offset, not 0.5 -- half the "
            "distance is exactly the bug this section exists to catch");
      check(!p.scatterBothAxes,
            "abr: `bothAxes` FALSE imports as false -- perpendicular scatter, Photoshop's own "
            "default");
    }

    // useScatter ON, control only (Control: Pen Pressure, no jitter): one
    // PRESSURE link, doubled the same way -- the control branch and the
    // jitter branch must agree about which unit a Scatter link's range is in,
    // or a brush with both composes two contributions on two different
    // scales onto the same Add target.
    BrushSpec controlOnly;
    controlOnly.useScatter = true;
    controlOnly.scatterControl = 2;  // Pen Pressure
    controlOnly.scatterJitter = 0.0;
    controlOnly.bothAxes = true;
    const AbrImportResult cR = importAbrBrushes(wrapAbr(oneBrushLibrary(controlOnly)));
    if (cR.ok && cR.presets.size() == 1) {
      const BrushPreset& p = cR.presets[0];
      const size_t ctrl = findLink(p.links, DynamicSource::Pressure, DynamicTarget::Scatter);
      check(ctrl != kNoLink && p.links.links.size() == 1,
            "abr: a control alone (no jitter) makes exactly ONE Scatter link, on PRESSURE");
      // targetDefaultRange(Scatter) is (0,1) -- a full DIAMETER of travel at
      // full pressure -- so doubled it is (0,2) RADII, matching the jitter
      // branch's own unit rather than a different one.
      check(ctrl != kNoLink && nearf(p.links.links[ctrl].rangeLo, 0.0f, 1e-4f) &&
                nearf(p.links.links[ctrl].rangeHi, 2.0f, 1e-4f),
            "abr: a control-driven Scatter link is ALSO in RADII -- (0,1) DIAMETER doubles to "
            "(0,2) RADII, the same unit the jitter branch converts to");
      check(p.scatterBothAxes,
            "abr: `bothAxes` TRUE imports as true -- isotropic scatter, matching the checkbox");
    }

    // Both a control AND jitter: TWO links, both converted -- section 5's
    // "two links" shape, restated for Scatter with its own unit.
    BrushSpec both;
    both.useScatter = true;
    both.scatterControl = 2;  // Pen Pressure
    both.scatterJitter = 80.0;
    const AbrImportResult bR = importAbrBrushes(wrapAbr(oneBrushLibrary(both)));
    if (bR.ok && bR.presets.size() == 1) {
      const BrushPreset& p = bR.presets[0];
      const size_t ctrl = findLink(p.links, DynamicSource::Pressure, DynamicTarget::Scatter);
      const size_t rnd = findLink(p.links, DynamicSource::Random, DynamicTarget::Scatter);
      check(ctrl != kNoLink && rnd != kNoLink && p.links.links.size() == 2,
            "abr: a control AND jitter together make TWO Scatter links, both converted");
      // 80% of a diameter is 1.6 radii.
      check(rnd != kNoLink && nearf(p.links.links[rnd].rangeHi, 1.6f, 1e-4f),
            "abr: an 80% jitter converts to 1.6 RADII, not 0.8");
    }

    // No `bothAxes` key in the descriptor at all -- the absent case B5 says
    // must default to PERPENDICULAR, exercised through the real import path
    // rather than assumed from `DescriptorRef::asBoolean()`'s own contract.
    const AbrImportResult nB =
        importAbrBrushes(wrapAbr(oneBrushLibraryScatterNoBothAxes(0, 50.0)));
    check(nB.ok && nB.presets.size() == 1 && !nB.presets[0].scatterBothAxes,
          "abr: a descriptor with NO `bothAxes` key imports scatterBothAxes as false -- absent "
          "means perpendicular, Photoshop's own default, not isotropic");
  }

  std::printf("[selftest] abr brushes %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
