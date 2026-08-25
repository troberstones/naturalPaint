#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/PenAxes.hpp"
#include "brush/Dynamics.hpp"

namespace np {

// The brush dynamics link model (design "naturalPaint Panels" turn 4a) --
// what a link resolves to, and how several links to one target combine.
//
// Almost nothing here is visible in a screenshot. The DYNAMICS matrix draws a
// filled cell whether the link behind it is inverted, clamped, disabled or
// wrong by a factor of two, so a golden image cannot tell a working link
// system from a decorative one. These are the assertions that can.
bool runBrushDynamicsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- 1. An empty set is exactly a no-op -------------------------------
  //
  // A tablet is not required to paint. If an unlinked brush did not resolve
  // to its authored tip, every mouse user would be painting a brush nobody
  // designed -- and the error would be a plausible-looking one, not a crash.
  {
    const DynamicResult r = evaluateLinks(BrushLinkSet{}, DynamicInputs{});
    bool allIdentity = true;
    bool sawBoth = false;
    float firstMul = -1.0f, firstAdd = -1.0f;
    for (size_t i = 0; i < kDynamicTargetCount; ++i) {
      const DynamicTarget t = static_cast<DynamicTarget>(i);
      const float want = targetCombine(t) == TargetCombine::Add ? 0.0f : 1.0f;
      if (r.at(t) != want) allIdentity = false;
      if (targetCombine(t) == TargetCombine::Add) firstAdd = r.at(t);
      else firstMul = r.at(t);
    }
    sawBoth = (firstAdd == 0.0f && firstMul == 1.0f);
    check(allIdentity && sawBoth,
          "dynamics: an empty link set resolves EVERY target to its identity -- 1.0 for a "
          "scale, 0.0 for an offset, so a mouse paints the tip as authored");
  }

  // --- 2. RANGE is the OUTPUT range -------------------------------------
  //
  // The reading that decides what the whole system means. As an output range,
  // RANGE 0.10-1.00 on PRESSURE -> SIZE says "never below 10% of the radius,
  // however light the touch" -- a minimum-diameter control. Read as an input
  // gate instead, the same numbers would mean "ignore the lightest 10% of the
  // pressure range", which is a different feature that happens to produce
  // plausible strokes. Both are defensible in isolation; only one reproduces
  // the design's own worked example (see section 7).
  {
    BrushLink l;
    l.rangeLo = 0.10f;
    l.rangeHi = 1.00f;
    check(nearf(linkContribution(l, 0.0f), 0.10f, 1e-6f) &&
              nearf(linkContribution(l, 1.0f), 1.00f, 1e-6f),
          "dynamics: a link at source 0 resolves to rangeLo and at source 1 to rangeHi -- "
          "RANGE bounds the OUTPUT, so 0.10 is a floor on size, not a deadzone on pressure");

    // And the source is clamped, not extrapolated: a velocity estimate that
    // overshoots 1.0 for one dab must not push size past its own ceiling.
    check(nearf(linkContribution(l, 2.0f), 1.00f, 1e-6f) &&
              nearf(linkContribution(l, -1.0f), 0.10f, 1e-6f),
          "dynamics: a source outside [0,1] clamps rather than extrapolating past the "
          "authored range");
  }

  // --- 3. INVERT mirrors, and survives the curve -------------------------
  {
    BrushLink l;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    l.invert = true;
    check(nearf(linkContribution(l, 0.0f), 1.0f, 1e-6f) &&
              nearf(linkContribution(l, 1.0f), 0.0f, 1e-6f),
          "dynamics: INVERT mirrors the source about the middle of its domain");

    // Kept as a flag rather than baked into the curve, so that flipping a
    // hand-drawn response does not destroy it. Assert the curve is still
    // there and still shaping the result.
    l.curve = easingCurve(EasingPreset::SCurve);
    const size_t pointsBefore = l.curve.size();
    const float inverted = linkContribution(l, 0.25f);
    l.invert = false;
    const float upright = linkContribution(l, 0.75f);
    check(l.curve.size() == pointsBefore && nearf(inverted, upright, 1e-5f),
          "dynamics: INVERT composes with an authored curve instead of replacing it -- "
          "inverted at 0.25 equals upright at 0.75 on a symmetric S");
  }

  // --- 4. The curve's output is clamped ----------------------------------
  //
  // ops/PointOps.hpp does not confine a Curve's y, and app/CurveEdit lets a
  // control point be dragged anywhere. Without a clamp here, one dragged
  // handle hands the deposit path a negative radius scale.
  {
    BrushLink l;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    l.curve = Curve{{0.0f, -3.0f}, {1.0f, 4.0f}};
    bool inUnit = true;
    for (int i = 0; i <= 20; ++i) {
      const float f = linkContribution(l, static_cast<float>(i) / 20.0f);
      if (!(f >= 0.0f && f <= 1.0f)) inUnit = false;
    }
    check(inUnit,
          "dynamics: a control point dragged outside the plot cannot drive a target past "
          "its range -- no negative size scale reaches the deposit path");
  }

  // --- 5. Multi-source targets, and order independence --------------------
  //
  // The design's own matrix drives ANGLE from tilt, azimuth AND barrel, so
  // this is the normal case. If the fold order mattered, the resolved angle
  // would depend on the order the user happened to create the links in --
  // invisible in the matrix, which draws three identical cells either way.
  {
    DynamicInputs in;
    in.tilt = 0.3f;
    in.azimuth = 0.7f;
    in.barrel = 0.5f;

    auto makeAngle = [](DynamicSource s) {
      BrushLink l;
      l.source = s;
      l.target = DynamicTarget::Angle;
      targetDefaultRange(DynamicTarget::Angle, l.rangeLo, l.rangeHi);
      return l;
    };
    BrushLinkSet a, b;
    addLink(a, makeAngle(DynamicSource::Tilt));
    addLink(a, makeAngle(DynamicSource::Azimuth));
    addLink(a, makeAngle(DynamicSource::Barrel));
    // The same three links installed in the opposite order.
    addLink(b, makeAngle(DynamicSource::Barrel));
    addLink(b, makeAngle(DynamicSource::Azimuth));
    addLink(b, makeAngle(DynamicSource::Tilt));

    const float angleA = evaluateLinks(a, in).at(DynamicTarget::Angle);
    const float angleB = evaluateLinks(b, in).at(DynamicTarget::Angle);
    check(nearf(angleA, angleB, 1e-4f),
          "dynamics: three sources driving one target resolve the same whichever order the "
          "links were added -- the fold is commutative by construction");

    // And it actually summed rather than picking one: 0.3+0.7+0.5 turns.
    check(nearf(angleA, (0.3f + 0.7f + 0.5f) * 360.0f, 0.05f),
          "dynamics: an Add target ACCUMULATES its sources -- three rotations compose "
          "instead of the last one winning");

    // A Multiply target composes rather than accumulating, which is the
    // other half of the same rule and the one that makes two size links
    // behave like two scale factors.
    BrushLinkSet m;
    auto sizeLink = [](DynamicSource s, float hi) {
      BrushLink l;
      l.source = s;
      l.target = DynamicTarget::Size;
      l.rangeLo = 0.0f;
      l.rangeHi = hi;
      return l;
    };
    addLink(m, sizeLink(DynamicSource::Pressure, 1.0f));
    addLink(m, sizeLink(DynamicSource::Velocity, 1.0f));
    DynamicInputs mi;
    mi.pressure = 0.5f;
    mi.velocity = 0.5f;
    check(nearf(evaluateLinks(m, mi).at(DynamicTarget::Size), 0.25f, 1e-5f),
          "dynamics: a Multiply target COMPOSES its sources -- two half-strength size links "
          "give a quarter, not a half and not one");
  }

  // --- 6. One link per matrix cell ---------------------------------------
  //
  // The matrix is only a faithful picture of the configuration if a cell is a
  // link. Two links in one cell would draw as one, and the panel's header
  // would count a number the user cannot find on screen.
  {
    BrushLinkSet s;
    BrushLink first;
    first.source = DynamicSource::Pressure;
    first.target = DynamicTarget::Flow;
    first.rangeHi = 0.5f;
    addLink(s, first);
    BrushLink second = first;
    second.rangeHi = 0.9f;
    addLink(s, second);
    check(s.links.size() == 1 && nearf(s.links[0].rangeHi, 0.9f, 1e-6f),
          "dynamics: adding a second link to an occupied cell REPLACES it -- the matrix "
          "shows one square per cell, so it must hold one link per cell");

    check(findLink(s, DynamicSource::Pressure, DynamicTarget::Flow) == 0 &&
              findLink(s, DynamicSource::Tilt, DynamicTarget::Flow) == kNoLink,
          "dynamics: findLink locates an occupied cell and reports kNoLink for an empty one");

    check(removeLink(s, DynamicSource::Pressure, DynamicTarget::Flow) &&
              s.links.empty() &&
              !removeLink(s, DynamicSource::Pressure, DynamicTarget::Flow),
          "dynamics: removing a link empties the cell, and removing an empty cell is a "
          "no-op that reports it did nothing");
  }

  // --- 7. The design's own worked example ---------------------------------
  //
  // 4a's LINK editor shows PRESSURE -> SIZE, RANGE 0.10-1.00, IN 0.62, and
  // OUT 17.1 px against the TIP section's 24 px radius. That figure is the
  // one external check on section 2's reading of RANGE, so it is worth
  // reproducing -- with a caveat recorded rather than hidden: the mock's
  // curve marker does not sit on the mock's own drawn bezier (the path gives
  // y ~= 0.54 at the marker's x, the marker is drawn at ~0.68), so 17.1 is a
  // hand-placed number and the exact preset behind it is ambiguous.
  //
  // What is NOT ambiguous is that it falls inside the span the three presets
  // produce under the output-range reading, and that the S curve reproduces
  // it to 0.13 px. Under an input-gate reading the same inputs give 15.0 px
  // and no preset reaches 17.1 at all.
  {
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.10f;
    l.rangeHi = 1.00f;
    l.curve = easingCurve(EasingPreset::SCurve);
    const float px = 24.0f * linkContribution(l, 0.62f);
    check(nearf(px, 17.1f, 0.25f),
          "dynamics: the design's worked example reproduces -- pressure 0.62 through the S "
          "curve at RANGE 0.10-1.00 puts a 24 px tip at ~17.1 px");

    // The presets must actually differ, or the check above would pass just as
    // happily against a build where the chips did nothing.
    BrushLink lin = l;
    lin.curve = easingCurve(EasingPreset::Linear);
    BrushLink eo = l;
    eo.curve = easingCurve(EasingPreset::EaseOut);
    const float pxLin = 24.0f * linkContribution(lin, 0.62f);
    const float pxEo = 24.0f * linkContribution(eo, 0.62f);
    check(std::fabs(pxLin - px) > 1.0f && std::fabs(pxEo - px) > 1.0f &&
              std::fabs(pxLin - pxEo) > 1.0f,
          "dynamics: the three easing presets are genuinely three curves -- each moves the "
          "worked example by more than a pixel, so the chips are not decorative");
  }

  // --- 8. Disabled links keep their work ----------------------------------
  {
    BrushLinkSet s;
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    l.curve = easingCurve(EasingPreset::SCurve);
    l.enabled = false;
    addLink(s, l);
    DynamicInputs in;
    in.pressure = 0.5f;
    check(evaluateLinks(s, in).at(DynamicTarget::Size) == 1.0f &&
              s.links[0].curve.size() == easingCurve(EasingPreset::SCurve).size(),
          "dynamics: a disabled link contributes nothing but KEEPS its authored curve -- "
          "the toggle is not destructive");
  }

  // --- 9. Preset identification -------------------------------------------
  {
    check(matchesPreset(Curve{}, EasingPreset::Linear) &&
              !matchesPreset(Curve{}, EasingPreset::EaseOut),
          "dynamics: an empty curve is linear, so a fresh link lights the LINEAR chip and "
          "no other");
    check(matchesPreset(easingCurve(EasingPreset::SCurve), EasingPreset::SCurve) &&
              !matchesPreset(easingCurve(EasingPreset::SCurve), EasingPreset::EaseOut),
          "dynamics: each preset curve identifies as itself and not as its neighbours");
    // A curve dragged away from every preset lights none of them, which is
    // the honest answer -- the chips describe curves.
    const Curve custom{{0.0f, 0.0f}, {0.5f, 0.2f}, {1.0f, 1.0f}};
    check(!matchesPreset(custom, EasingPreset::Linear) &&
              !matchesPreset(custom, EasingPreset::EaseOut) &&
              !matchesPreset(custom, EasingPreset::SCurve),
          "dynamics: an edited curve matches no chip rather than the nearest one");
  }

  // --- 10. Tripwires on the enum shapes ------------------------------------
  //
  // The matrix's whole premise is that it draws the WHOLE space. A source or
  // target added to the enum without a name, an abbreviation, a combine rule
  // and a column would be invisible in the one panel built to make the space
  // visible -- so the counts and the tables are pinned here.
  {
    bool namesOk = true;
    for (size_t i = 0; i < kDynamicSourceCount; ++i) {
      const char* n = sourceName(static_cast<DynamicSource>(i));
      if (n == nullptr || n[0] == '?' || n[0] == '\0') namesOk = false;
    }
    for (size_t i = 0; i < kDynamicTargetCount; ++i) {
      const DynamicTarget t = static_cast<DynamicTarget>(i);
      const char* a = targetAbbrev(t);
      const char* n = targetName(t);
      if (a == nullptr || a[0] == '?' || std::strlen(a) != 2) namesOk = false;
      if (n == nullptr || n[0] == '?' || n[0] == '\0') namesOk = false;
    }
    check(namesOk,
          "dynamics: every source has a row label and every target a two-letter head and a "
          "full name -- a nameless one would be an invisible column");

    // The counts the panel lays out against. 8 rows x 12 columns is not a
    // number the matrix can discover at run time; it is baked into the column
    // widths at 322 px.
    check(kDynamicSourceCount == 8 && kDynamicTargetCount == 12,
          "dynamics: the matrix is 8 sources x 12 targets, the shape the 322 px column was "
          "laid out for");

    check(sourceName(static_cast<DynamicSource>(kDynamicSourceCount))[0] == '?' &&
              targetAbbrev(static_cast<DynamicTarget>(kDynamicTargetCount))[0] == '?',
          "dynamics: a source or target past the end of the enum reports '?' rather than "
          "reading past the table");
  }

  // --- 11. The live gutter's own conversions -------------------------------
  {
    char buf[32];
    check(std::strcmp(sourceDisplay(DynamicSource::Random, 0.5f, buf, sizeof buf),
                      "\xE2\x80\x94") == 0,
          "gutter: RANDOM shows an em dash -- it is redrawn per dab, so any number would be "
          "one the next dab already discarded");

    // Barrel is signed: a pen twirls either way from rest, which is why the
    // design's own gutter reads a negative angle.
    sourceDisplay(DynamicSource::Barrel, 0.5f, buf, sizeof buf);
    const bool barrelRestIsZero = (std::strcmp(buf, "0\xC2\xB0") == 0);
    sourceDisplay(DynamicSource::Barrel, 0.0f, buf, sizeof buf);
    const bool barrelGoesNegative = (buf[0] == '-');
    check(barrelRestIsZero && barrelGoesNegative,
          "gutter: BARREL is signed about its rest orientation, so mid-range reads 0 and "
          "the low end reads negative");

    sourceDisplay(DynamicSource::Tilt, 1.0f, buf, sizeof buf);
    const bool tiltFlat = (std::strcmp(buf, "90\xC2\xB0") == 0);
    sourceDisplay(DynamicSource::Pressure, 0.62f, buf, sizeof buf);
    const bool pressureTwoDp = (std::strcmp(buf, "0.62") == 0);
    check(tiltFlat && pressureTwoDp,
          "gutter: TILT reads as an altitude in degrees and the plain sources to two "
          "decimals -- the panel converts, the model stays normalised");
  }

  // --- 12. SDL's raw pen axes -> the matrix's polar sources ---------------
  //
  // SDL reports tilt as two independent angles; the TILT and AZIMUTH rows
  // want a lean and a direction. The conversion is the one piece of the pen
  // path with an answer that can be quietly wrong -- every value it produces
  // is plausible, in range, and moves the right way when the pen moves.
  {
    // The whole reason for the tangents. A pen at 45 degrees in x and 45 in y
    // leans 54.74 from vertical, not sqrt(45^2+45^2) = 63.64: the tilts
    // compose as direction cosines. The naive form overstates every diagonal
    // lean, worst at exactly this angle -- which is the ordinary wrist
    // position for a right-handed painter shading.
    const float diagonal = penTiltNormalised(45.0f, 45.0f);
    const float naive = std::sqrt(45.0f * 45.0f + 45.0f * 45.0f) / 90.0f;
    check(nearf(diagonal, 54.7356f / 90.0f, 1e-3f) && std::fabs(diagonal - naive) > 0.09f,
          "pen: a 45/45 lean is 54.7 degrees, not 63.6 -- tilts compose as tangents, and "
          "the naive hypotenuse of the two angles is a full 0.098 of the range away");

    // On-axis, the tangent form and the angle agree exactly -- which is what
    // makes the diagonal case the only one that could hide an error.
    check(nearf(penTiltNormalised(45.0f, 0.0f), 0.5f, 1e-4f) &&
              nearf(penTiltNormalised(0.0f, -45.0f), 0.5f, 1e-4f),
          "pen: a purely single-axis lean normalises to the angle itself, so the two forms "
          "differ only off-axis");

    check(penTiltNormalised(0.0f, 0.0f) == 0.0f &&
              nearf(penTiltNormalised(90.0f, 0.0f), 1.0f, 2e-3f),
          "pen: upright is 0 and flat is 1, with tan(90) clamped rather than infinite");

    // Azimuth measured anticlockwise from +x, over a full turn.
    check(nearf(penAzimuthNormalised(1.0f, 0.0f), 0.0f, 1e-4f) &&
              nearf(penAzimuthNormalised(0.0f, 1.0f), 0.25f, 1e-4f) &&
              nearf(penAzimuthNormalised(-1.0f, 0.0f), 0.5f, 1e-4f) &&
              nearf(penAzimuthNormalised(0.0f, -1.0f), 0.75f, 1e-4f),
          "pen: azimuth runs a full turn anticlockwise from +x, so the four cardinal leans "
          "land on the quarters rather than wrapping through a negative");

    check(penAzimuthNormalised(0.0f, 0.0f) == 0.0f,
          "pen: an UPRIGHT pen reports azimuth 0 -- it has no direction of lean, and a "
          "stale or NaN angle would drive an Angle link from noise");

    // Barrel rests in the MIDDLE, and round-trips through the gutter's own
    // inverse -- the pair is only meaningful if the two agree.
    char buf[32];
    sourceDisplay(DynamicSource::Barrel, penBarrelNormalised(0.0f), buf, sizeof buf);
    check(nearf(penBarrelNormalised(0.0f), 0.5f, 1e-6f) &&
              penBarrelNormalised(-180.0f) == 0.0f && penBarrelNormalised(180.0f) == 1.0f &&
              std::strcmp(buf, "0\xC2\xB0") == 0,
          "pen: barrel rest is 0.5 and round-trips through the gutter's inverse -- a rest "
          "at 0 would put half the range beyond a curve whose domain starts there");
  }

  std::printf("[selftest] brush dynamics %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
