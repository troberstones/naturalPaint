#include "app/selftest/Support.hpp"

#include <functional>

#include <set>

#include "brush/BrushModelIo.hpp"

namespace np {
namespace {

// A running counter so every numeric leaf below gets its OWN value, never
// shared with another leaf of the same type. That is the whole defence
// against the failure mode the task brief calls out by name: a fixture where
// every float is 1.0f would pass a round trip even if the implementation
// swapped `angleDeg` and `roundness`, because both would still read back as
// 1.0f. Monotonically increasing and never touching an actual default (every
// default in `BrushModel.hpp` is either 0, an integer under 26, or one of a
// handful of named constants below 100; the counter starts above 1 and its
// step is irrational-looking enough in decimal that it never lands on one of
// them across the ~60 numeric leaves this fixture uses).
struct Counter {
  float f = 0.0f;
  int32_t i = 0;
  float nextF() {
    f += 1.0100101f;
    return f;
  }
  int32_t nextI() {
    i += 3;  // never 1 (scatter/count's default) or 25 (fadeSteps' default)
    return i;
  }
};

// The seven non-`Off` ordinals -- `Off` is `VarianceControl`'s own default,
// so a `control` field left at `Off` would not be the "non-default value"
// the fixture is supposed to give every leaf, even though the field's
// *sibling* leaves (jitter, minimum, ...) are non-default. Cycled rather than
// picked once so the 17 `Variance` instances in `BrushModel` do not all name
// the same control, which would leave a control-for-control swap between two
// of them undetectable.
const VarianceControl kControls[] = {
    VarianceControl::Fade,       VarianceControl::PenPressure, VarianceControl::PenTilt,
    VarianceControl::StylusWheel, VarianceControl::Rotation,    VarianceControl::Direction,
    VarianceControl::InitialDirection,
};

// Every field of `BrushModel` set to a value that differs from BOTH that
// field's own default AND every other field's fixture value where the type
// allows it (bool is the one type that cannot -- see the module comment at
// `runBrushModelIoTest()`). Built by hand, one member at a time, rather than
// through `visitBrushModelFields()`: the round trip below feeds this into
// the code under test, so building it with the SAME visitor would make a
// wrong `fn`-to-member binding invisible to itself.
BrushModel makeFixture() {
  BrushModel m;
  Counter c;
  size_t controlIdx = 0;
  auto fillVariance = [&](Variance& v) {
    v.control = kControls[controlIdx % 7];
    ++controlIdx;
    v.jitter = c.nextF();
    v.minimum = c.nextF();
    v.fadeSteps = c.nextI();
    v.present = true;  // default false
  };

  m.tip.dab.id = "abr:fixture-primary-tip";
  m.tip.diameterPx = c.nextF();
  m.tip.angleDeg = c.nextF();
  m.tip.roundness = c.nextF();
  m.tip.spacingPercent = c.nextF();
  m.tip.hardness = c.nextF();
  m.tip.spacingEnabled = false;  // default true
  m.tip.flipX = true;
  m.tip.flipY = true;
  m.tip.computed = true;

  m.shape.enabled = true;
  fillVariance(m.shape.size);
  fillVariance(m.shape.angle);
  fillVariance(m.shape.roundness);
  m.shape.flipXJitter = true;
  m.shape.flipYJitter = true;
  m.shape.brushProjection = true;
  m.shape.tiltScale = c.nextF();

  m.scatter.enabled = true;
  fillVariance(m.scatter.scatter);
  m.scatter.bothAxes = true;
  m.scatter.count = c.nextI();
  fillVariance(m.scatter.countJitter);

  m.texture.enabled = true;
  m.texture.pattern.id = "11111111-2222-3333-4444-555555555555";
  // A pattern name with real spaces in it -- `ktw watercolor paper 2k17 b`
  // is a name that has actually shipped in a pack on this machine
  // (BrushModelIo.hpp's own note on why a path's value is "everything after
  // the first space", not a tokenised rest of the line).
  m.texture.pattern.name = "ktw watercolor paper 2k17 b";
  m.texture.invert = true;
  m.texture.scalePercent = c.nextF();
  m.texture.depth = c.nextF();
  m.texture.minimumDepth = c.nextF();
  fillVariance(m.texture.depthJitter);
  m.texture.blend = CoverageBlend::ColorBurn;  // default Height
  m.texture.brightness = c.nextF();
  m.texture.contrast = c.nextF();
  m.texture.eachTip = true;
  m.texture.protectTexture = true;

  m.dual.enabled = true;
  m.dual.tip.dab.id = "abr:fixture-dual-tip";
  m.dual.tip.diameterPx = c.nextF();
  m.dual.tip.angleDeg = c.nextF();
  m.dual.tip.roundness = c.nextF();
  m.dual.tip.spacingPercent = c.nextF();
  m.dual.tip.hardness = c.nextF();
  m.dual.tip.spacingEnabled = false;
  m.dual.tip.flipX = true;
  m.dual.tip.flipY = true;
  m.dual.tip.computed = true;
  m.dual.blend = CoverageBlend::LinearBurn;  // default Multiply
  m.dual.scatter.enabled = true;
  fillVariance(m.dual.scatter.scatter);
  m.dual.scatter.bothAxes = true;
  m.dual.scatter.count = c.nextI();
  fillVariance(m.dual.scatter.countJitter);
  m.dual.flip = true;

  m.color.enabled = true;
  m.color.perTip = true;
  fillVariance(m.color.foregroundBackground);
  m.color.hueJitter = c.nextF();
  m.color.saturationJitter = c.nextF();
  m.color.brightnessJitter = c.nextF();
  m.color.purity = c.nextF();

  m.transfer.enabled = true;
  fillVariance(m.transfer.opacity);
  fillVariance(m.transfer.flow);
  fillVariance(m.transfer.wetness);
  fillVariance(m.transfer.mix);

  m.options.blendMode = "Multiply";  // default ""
  m.options.opacity = c.nextF();
  m.options.flow = c.nextF();
  m.options.smoothing = false;  // default true
  m.options.pressureOverridesSize = true;
  m.options.pressureOverridesOpacity = true;
  m.options.useLegacy = true;
  fillVariance(m.options.sizeOverride);
  fillVariance(m.options.opacityOverride);
  fillVariance(m.options.flowOverride);
  fillVariance(m.options.colorOverride);

  m.noise = true;
  m.wetEdges = true;
  m.airbrush = true;
  m.brushPose = true;
  m.load = c.nextF();
  m.wetness = c.nextF();

  return m;
}

}  // namespace

// brush/BrushModelIo: the text format for a `BrushModel`, and the one
// templated visitor (brush/BrushModelIo.hpp's `visitBrushModelFields()`)
// `brushModelToLines()`, `brushModelApplyLine()` and `brushModelFieldPaths()`
// each walk once instead of ~117 hand-written branches apiece.
//
// **Why the fixture below is not "every field set to 1.0f".** A round trip
// built on identical values passes even when an implementation swaps two
// same-typed fields -- `tip.angleDeg` written where `tip.roundness` should
// be, say -- because both still read back as whatever the shared placeholder
// was. `makeFixture()` gives every leaf a distinct value where the type has
// room for one (every float and int in the model, from a single running
// counter; every string; most enum ordinals), so a swap moves a
// recognisably-wrong number into a field this test is watching, not a
// coincidentally-right one. `bool` is the one type that cannot supply 117
// distinct values -- it has two -- so those leaves are simply flipped from
// their own default and no more can honestly be claimed for them.
bool runBrushModelIoTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto checkVariance = [&](const char* prefix, const Variance& want, const Variance& got) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s.control round-trips", prefix);
    check(want.control == got.control, buf);
    std::snprintf(buf, sizeof(buf), "%s.jitter round-trips exactly", prefix);
    check(want.jitter == got.jitter, buf);
    std::snprintf(buf, sizeof(buf), "%s.minimum round-trips exactly", prefix);
    check(want.minimum == got.minimum, buf);
    std::snprintf(buf, sizeof(buf), "%s.fadeSteps round-trips", prefix);
    check(want.fadeSteps == got.fadeSteps, buf);
    std::snprintf(buf, sizeof(buf), "%s.present round-trips", prefix);
    check(want.present == got.present, buf);
  };
  auto checkTipShape = [&](const char* prefix, const PsTipShape& want, const PsTipShape& got) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s.dab.id round-trips", prefix);
    check(want.dab.id == got.dab.id, buf);
    std::snprintf(buf, sizeof(buf), "%s.diameterPx round-trips exactly", prefix);
    check(want.diameterPx == got.diameterPx, buf);
    std::snprintf(buf, sizeof(buf), "%s.angleDeg round-trips exactly", prefix);
    check(want.angleDeg == got.angleDeg, buf);
    std::snprintf(buf, sizeof(buf), "%s.roundness round-trips exactly", prefix);
    check(want.roundness == got.roundness, buf);
    std::snprintf(buf, sizeof(buf), "%s.spacingPercent round-trips exactly", prefix);
    check(want.spacingPercent == got.spacingPercent, buf);
    std::snprintf(buf, sizeof(buf), "%s.hardness round-trips exactly", prefix);
    check(want.hardness == got.hardness, buf);
    std::snprintf(buf, sizeof(buf), "%s.spacingEnabled round-trips", prefix);
    check(want.spacingEnabled == got.spacingEnabled, buf);
    std::snprintf(buf, sizeof(buf), "%s.flipX round-trips", prefix);
    check(want.flipX == got.flipX, buf);
    std::snprintf(buf, sizeof(buf), "%s.flipY round-trips", prefix);
    check(want.flipY == got.flipY, buf);
    std::snprintf(buf, sizeof(buf), "%s.computed round-trips", prefix);
    check(want.computed == got.computed, buf);
  };
  auto checkScatter = [&](const char* prefix, const PsScatter& want, const PsScatter& got) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s.enabled round-trips", prefix);
    check(want.enabled == got.enabled, buf);
    std::snprintf(buf, sizeof(buf), "%s.scatter", prefix);
    checkVariance(buf, want.scatter, got.scatter);
    std::snprintf(buf, sizeof(buf), "%s.bothAxes round-trips", prefix);
    check(want.bothAxes == got.bothAxes, buf);
    std::snprintf(buf, sizeof(buf), "%s.count round-trips", prefix);
    check(want.count == got.count, buf);
    std::snprintf(buf, sizeof(buf), "%s.countJitter", prefix);
    checkVariance(buf, want.countJitter, got.countJitter);
  };

  // ==========================================================================
  std::printf("  -- A. a default model writes zero lines --\n");
  // ==========================================================================
  {
    BrushModel def;
    const std::vector<std::string> lines = brushModelToLines(def);
    check(lines.empty(), "brushModelToLines: default BrushModel produces zero lines");
  }

  // ==========================================================================
  std::printf("  -- B. every path is unique, and the count is pinned --\n");
  // ==========================================================================
  {
    const std::vector<std::string> paths = brushModelFieldPaths();
    // Pinned to a literal, not to `paths.size()` computed some other way: a
    // field added to `BrushModel` with no matching call in
    // `visitBrushModelFields()` must fail a comparison against a number
    // written down here, or this assertion would silently track whatever the
    // visitor happens to produce and prove nothing. 151 = 66 scalar/enum/
    // string leaves (`PsTipShape`'s 10 x 2 instances -- `tip`, `dual.tip` --
    // plus `PsScatter`'s 3 x 2 instances -- `scatter`, `dual.scatter` --
    // plus 40 more that appear once each) + 17 `Variance` instances x 5
    // leaves apiece (control, jitter, minimum, fadeSteps, present) = 85.
    constexpr size_t kExpectedPathCount = 151;
    check(paths.size() == kExpectedPathCount,
          "brushModelFieldPaths: path count matches the count pinned in this test");

    std::set<std::string> unique(paths.begin(), paths.end());
    check(unique.size() == paths.size(),
          "brushModelFieldPaths: no path is duplicated (a duplicate would be unreadable)");
  }

  // ==========================================================================
  std::printf("  -- C. round trip: every field, a distinct value, zero tolerance --\n");
  // ==========================================================================
  BrushModel fixture = makeFixture();
  std::vector<std::string> fixtureLines;
  {
    fixtureLines = brushModelToLines(fixture);
    const std::vector<std::string> allPaths = brushModelFieldPaths();
    check(fixtureLines.size() == allPaths.size(),
          "brushModelToLines: a fixture with every field non-default writes one line per path");

    BrushModel restored;  // starts default; every leaf below must move off it
    size_t appliedOk = 0;
    for (const std::string& line : fixtureLines) {
      if (brushModelApplyLine(restored, line)) ++appliedOk;
    }
    check(appliedOk == fixtureLines.size(),
          "brushModelApplyLine: every line the fixture produced re-applies successfully");

    checkTipShape("tip", fixture.tip, restored.tip);

    check(fixture.shape.enabled == restored.shape.enabled, "shape.enabled round-trips");
    checkVariance("shape.size", fixture.shape.size, restored.shape.size);
    checkVariance("shape.angle", fixture.shape.angle, restored.shape.angle);
    checkVariance("shape.roundness", fixture.shape.roundness, restored.shape.roundness);
    check(fixture.shape.flipXJitter == restored.shape.flipXJitter, "shape.flipXJitter round-trips");
    check(fixture.shape.flipYJitter == restored.shape.flipYJitter, "shape.flipYJitter round-trips");
    check(fixture.shape.brushProjection == restored.shape.brushProjection,
          "shape.brushProjection round-trips");
    check(fixture.shape.tiltScale == restored.shape.tiltScale,
          "shape.tiltScale round-trips exactly");

    checkScatter("scatter", fixture.scatter, restored.scatter);

    check(fixture.texture.enabled == restored.texture.enabled, "texture.enabled round-trips");
    check(fixture.texture.pattern.id == restored.texture.pattern.id,
          "texture.pattern.id round-trips");
    check(fixture.texture.pattern.name == restored.texture.pattern.name,
          "texture.pattern.name round-trips WITH ITS SPACES");
    check(fixture.texture.invert == restored.texture.invert, "texture.invert round-trips");
    check(fixture.texture.scalePercent == restored.texture.scalePercent,
          "texture.scalePercent round-trips exactly");
    check(fixture.texture.depth == restored.texture.depth, "texture.depth round-trips exactly");
    check(fixture.texture.minimumDepth == restored.texture.minimumDepth,
          "texture.minimumDepth round-trips exactly");
    checkVariance("texture.depthJitter", fixture.texture.depthJitter, restored.texture.depthJitter);
    check(fixture.texture.blend == restored.texture.blend, "texture.blend round-trips its ordinal");
    check(fixture.texture.brightness == restored.texture.brightness,
          "texture.brightness round-trips exactly");
    check(fixture.texture.contrast == restored.texture.contrast,
          "texture.contrast round-trips exactly");
    check(fixture.texture.eachTip == restored.texture.eachTip, "texture.eachTip round-trips");
    check(fixture.texture.protectTexture == restored.texture.protectTexture,
          "texture.protectTexture round-trips");

    check(fixture.dual.enabled == restored.dual.enabled, "dual.enabled round-trips");
    checkTipShape("dual.tip", fixture.dual.tip, restored.dual.tip);
    check(fixture.dual.blend == restored.dual.blend, "dual.blend round-trips its ordinal");
    checkScatter("dual.scatter", fixture.dual.scatter, restored.dual.scatter);
    check(fixture.dual.flip == restored.dual.flip, "dual.flip round-trips");

    check(fixture.color.enabled == restored.color.enabled, "color.enabled round-trips");
    check(fixture.color.perTip == restored.color.perTip, "color.perTip round-trips");
    checkVariance("color.foregroundBackground", fixture.color.foregroundBackground,
                  restored.color.foregroundBackground);
    check(fixture.color.hueJitter == restored.color.hueJitter,
          "color.hueJitter round-trips exactly");
    check(fixture.color.saturationJitter == restored.color.saturationJitter,
          "color.saturationJitter round-trips exactly");
    check(fixture.color.brightnessJitter == restored.color.brightnessJitter,
          "color.brightnessJitter round-trips exactly");
    check(fixture.color.purity == restored.color.purity, "color.purity round-trips exactly");

    check(fixture.transfer.enabled == restored.transfer.enabled, "transfer.enabled round-trips");
    checkVariance("transfer.opacity", fixture.transfer.opacity, restored.transfer.opacity);
    checkVariance("transfer.flow", fixture.transfer.flow, restored.transfer.flow);
    checkVariance("transfer.wetness", fixture.transfer.wetness, restored.transfer.wetness);
    checkVariance("transfer.mix", fixture.transfer.mix, restored.transfer.mix);

    check(fixture.options.blendMode == restored.options.blendMode,
          "options.blendMode round-trips");
    check(fixture.options.opacity == restored.options.opacity,
          "options.opacity round-trips exactly");
    check(fixture.options.flow == restored.options.flow, "options.flow round-trips exactly");
    check(fixture.options.smoothing == restored.options.smoothing, "options.smoothing round-trips");
    check(fixture.options.pressureOverridesSize == restored.options.pressureOverridesSize,
          "options.pressureOverridesSize round-trips");
    check(fixture.options.pressureOverridesOpacity == restored.options.pressureOverridesOpacity,
          "options.pressureOverridesOpacity round-trips");
    check(fixture.options.useLegacy == restored.options.useLegacy, "options.useLegacy round-trips");
    checkVariance("options.sizeOverride", fixture.options.sizeOverride,
                  restored.options.sizeOverride);
    checkVariance("options.opacityOverride", fixture.options.opacityOverride,
                  restored.options.opacityOverride);
    checkVariance("options.flowOverride", fixture.options.flowOverride,
                  restored.options.flowOverride);
    checkVariance("options.colorOverride", fixture.options.colorOverride,
                  restored.options.colorOverride);

    check(fixture.noise == restored.noise, "noise round-trips");
    check(fixture.wetEdges == restored.wetEdges, "wetEdges round-trips");
    check(fixture.airbrush == restored.airbrush, "airbrush round-trips");
    check(fixture.brushPose == restored.brushPose, "brushPose round-trips");
    check(fixture.load == restored.load, "load round-trips exactly");
    check(fixture.wetness == restored.wetness, "wetness round-trips exactly");
  }

  // ==========================================================================
  std::printf("  -- D. rejected input mutates nothing --\n");
  // ==========================================================================
  {
    // Start from the fixture, not a default model: every field already
    // carries a non-default value, so a bug that clobbered a field the
    // rejection is not supposed to touch has something to clobber.
    BrushModel guarded = fixture;
    const std::vector<std::string> before = brushModelToLines(guarded);

    check(!brushModelApplyLine(guarded, "tip.shoeSize 12"),
          "brushModelApplyLine: an unknown path returns false");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: an unknown path leaves every field untouched");

    check(!brushModelApplyLine(guarded, "tip.diameterPx not_a_number"),
          "brushModelApplyLine: an unparseable float returns false");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: an unparseable value leaves every field untouched");

    check(!brushModelApplyLine(guarded, "tip.diameterPx 12.5 trailing"),
          "brushModelApplyLine: a value with unparsed trailing text is rejected");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: trailing-garbage rejection leaves every field untouched");

    check(!brushModelApplyLine(guarded, "options.pressureOverridesSize 2"),
          "brushModelApplyLine: a bool field rejects a non-0/1 value");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: a rejected bool leaves every field untouched");

    // Ordinal 8 does not name a VarianceControl (Variance.hpp's own enum
    // tops out at 7, InitialDirection) -- accepting it would let a future
    // build's own new ordinal, or a hand-corrupted file, silently become
    // whatever `static_cast` happens to do with it.
    check(!brushModelApplyLine(guarded, "shape.size.control 8"),
          "brushModelApplyLine: an out-of-range VarianceControl ordinal is rejected");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: the rejected ordinal leaves every field untouched");
    check(!brushModelApplyLine(guarded, "shape.size.control -1"),
          "brushModelApplyLine: a negative VarianceControl ordinal is rejected");

    // CoverageBlend tops out at 9 (LinearHeight); 10 names nothing.
    check(!brushModelApplyLine(guarded, "texture.blend 10"),
          "brushModelApplyLine: an out-of-range CoverageBlend ordinal is rejected");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: the rejected blend leaves every field untouched");

    check(!brushModelApplyLine(guarded, "texture.pattern.name"),
          "brushModelApplyLine: a line with no value token at all is rejected");
    check(brushModelToLines(guarded) == before,
          "brushModelApplyLine: a valueless line leaves every field untouched");
  }

  // ==========================================================================
  std::printf("  -- E. a string value survives its own spaces --\n");
  // ==========================================================================
  {
    // Isolated from the round trip above (which already exercises this
    // through the fixture's pattern name) so a failure here names the exact
    // path/value pair rather than being buried in section C's field list.
    BrushModel m;
    check(brushModelApplyLine(m, "texture.pattern.name ktw watercolor paper 2k17 b"),
          "brushModelApplyLine: a pattern name with spaces is accepted");
    check(m.texture.pattern.name == "ktw watercolor paper 2k17 b",
          "brushModelApplyLine: the spaces inside the value survive verbatim");
  }

  // =====================================================================
  std::printf("  -- F. a path names the member it claims to --\n");
  // =====================================================================
  //
  // **Every other section in this file is self-consistent, and a sabotage
  // proved it.** Swapping the path strings of `texture.brightness` and
  // `texture.contrast` in the field list left the suite entirely green: the
  // count is still 151, no path is duplicated, and the round trip still
  // passes because it writes and reads through the SAME mislabelled walk.
  // Section C's per-field check is generated from that walk too, so it
  // inherits the lie.
  //
  // That matters because these paths are a FILE FORMAT. A value stored under
  // the wrong field name round-trips perfectly inside this build and is wrong
  // to everything else: a hand-edited `user-presets.txt`, a future migration,
  // a panel drawing a control per path, and any build whose list is correct.
  //
  // So this section is written by HAND, member by member, and is the one
  // place in this file that does not go through the visitor. The pairs below
  // are chosen for the risk they carry rather than for coverage: each is two
  // adjacent leaves of the SAME TYPE inside one struct, which is exactly the
  // swap no generated check can see. `dual.tip.roundness` is here for a
  // second reason -- `PsTipShape` is visited twice, and a prefix passed
  // wrongly would make the Dual Brush's tip write over the primary one's.
  {
    struct Pinned {
      const char* path;
      std::function<void(BrushModel&)> set;
    };
    const std::vector<Pinned> pinned = {
        {"texture.brightness", [](BrushModel& m) { m.texture.brightness = 7.0f; }},
        {"texture.contrast",   [](BrushModel& m) { m.texture.contrast = 7.0f; }},
        {"tip.flipX",          [](BrushModel& m) { m.tip.flipX = true; }},
        {"tip.flipY",          [](BrushModel& m) { m.tip.flipY = true; }},
        {"shape.flipXJitter",  [](BrushModel& m) { m.shape.flipXJitter = true; }},
        {"shape.flipYJitter",  [](BrushModel& m) { m.shape.flipYJitter = true; }},
        {"color.hueJitter",        [](BrushModel& m) { m.color.hueJitter = 7.0f; }},
        {"color.saturationJitter", [](BrushModel& m) { m.color.saturationJitter = 7.0f; }},
        {"color.brightnessJitter", [](BrushModel& m) { m.color.brightnessJitter = 7.0f; }},
        {"transfer.opacity.jitter", [](BrushModel& m) { m.transfer.opacity.jitter = 7.0f; }},
        {"transfer.flow.jitter",    [](BrushModel& m) { m.transfer.flow.jitter = 7.0f; }},
        {"tip.roundness",           [](BrushModel& m) { m.tip.roundness = 7.0f; }},
        {"dual.tip.roundness",      [](BrushModel& m) { m.dual.tip.roundness = 7.0f; }},
        {"scatter.count",           [](BrushModel& m) { m.scatter.count = 7; }},
        {"dual.scatter.count",      [](BrushModel& m) { m.dual.scatter.count = 7; }},
    };

    bool everyPinHolds = true;
    std::string firstWrong;
    for (const Pinned& pin : pinned) {
      BrushModel m;
      pin.set(m);
      // Exactly one leaf was touched, so exactly one line must come out, and
      // its path must be the one this test named. Asserting the COUNT as well
      // as the path is what catches a prefix that leaks into a sibling.
      const std::vector<std::string> lines = brushModelToLines(m);
      const std::string want = std::string(pin.path) + " ";
      if (lines.size() != 1 || lines[0].compare(0, want.size(), want) != 0) {
        everyPinHolds = false;
        if (firstWrong.empty()) {
          firstWrong = std::string(pin.path) + " -> " +
                       (lines.empty() ? std::string("(nothing written)")
                                      : lines[0] + (lines.size() > 1 ? " (+more)" : ""));
        }
      }
    }
    if (!everyPinHolds)
      std::printf("  [measured] first mismatch: %s\n", firstWrong.c_str());
    check(everyPinHolds,
          "paths: setting one member by NAME writes that member's own path -- "
          "hand-written, so a swapped label cannot hide behind a consistent walk");
  }

  std::printf("[selftest] brush model io %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
