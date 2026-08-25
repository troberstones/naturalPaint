#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/RgbErase.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The eraser on a plain RGB layer (brush/RgbErase), and the routing that made
// it exist at all (app/StrokeSession section 1; PRD F9/F10, both P0; ADR-0007).
//
// See app/SelfTest.hpp for the section's own contents list, and the two headers
// for every decision this file only checks. Three things are worth saying here
// because they are what the section is *for*:
//
//   * **The eraser did nothing.** Not "did the wrong thing" and not "was
//     approximate": `Tool::Eraser` sat in `strokeRouteFor()`'s not-built list, so
//     a drag with it reached no layer, produced no message, moved no revision
//     and recorded nothing. It drew a cursor. PRD F9 and F10 are both P0.
//   * **The floor (section 4) is the one piece of arithmetic here that can be
//     *plausibly* wrong**, exactly as the deposit's ceiling is. A per-dab
//     strength erases, and looks like erasing, and is only detectable by
//     comparing two strokes that differ in how much they overlap themselves.
//     Section 4 runs both models side by side and prints the number the wrong
//     one produces.
//   * **The premultiplied claim (section 1) is checked on all four channels.**
//     Scaling the alpha alone leaves a texel that un-premultiplies *brighter*
//     than the paint it came from -- a fringe on precisely the soft edges an
//     eraser is used for -- and it is invisible in the layer's own numbers until
//     something composites them.
// ---------------------------------------------------------------------------
bool runRgbEraseTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances -------------------------------------------------------
  //
  //  * **kHalfRel / kHalfFloor** -- binary16 storage, which is what a
  //    `core::Tile` texel is. An 11-bit significand gives a round-to-nearest
  //    relative error of at most 2^-11 = 4.883e-04 for a normal value, plus an
  //    absolute floor of half a subnormal ulp, 2^-25 = 2.980e-08. Derived here
  //    from the storage rather than borrowed from runRgbDepositTest(), which
  //    states the identical derivation for the identical storage -- a tolerance
  //    copied without its derivation is the one that later gets applied where it
  //    does not hold.
  //
  //  * **kAccumTol for a stroke of N writes** -- the layer rounds to binary16
  //    once per dab that writes, while brush/RgbErase's accumulator does not.
  //    So the stored alpha after N *writing* dabs can drift from
  //    `alpha_0 * (1 - E)` by at most `N * kHalfRel * value`: each write
  //    contributes one rounding of at most `kHalfRel` relative, and every
  //    earlier error is then damped by the factor `(1 - e)` of every later dab,
  //    which is at most 1. That damping argument is the deposit's, and it
  //    transfers exactly, because a destination-out is *also* a product of
  //    factors in [0,1] -- which is the whole of why the two accumulators are
  //    the same algebra. Computed per test from the N that test actually spends,
  //    and the measured value printed beside it.
  //
  //  * Several assertions below are at **exactly zero** tolerance, and each says
  //    why in place. Two kinds: claims about which operations happen (a dab past
  //    the floor writes nothing at all; two strokes emit identical dabs), and
  //    claims that a particular multiply is *exact in binary16* -- scaling a
  //    normal half by 0.5 only decrements its exponent, so a strength-0.5 erase
  //    of a normal texel rounds nowhere and "exactly half" is a real assertion
  //    rather than a lucky one.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // The fixture is written straight into the store rather than painted with
  // brush/RgbDeposit first, deliberately: this section's subject is what the
  // eraser does to a texel that is already there, and building the "already
  // there" out of the *other* module would make every number below depend on
  // two arithmetics instead of one. A regression in the deposit would then show
  // up here as an erase failure, in a file that has nothing to say about it.
  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), premultiplied);
      }
    }
  };

  // A hard disc, so `dabCoverage()` is exactly 1.0f over the whole core and
  // every number below is about the erase rather than about the falloff.
  // Section 2 is where the falloff itself is checked.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h, size_t extraRgbLayers) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "rgb erase");
    for (size_t i = 0; i < extraRgbLayers; ++i)
      recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                   makeRgbLayer("RGB " + std::to_string(i + 1))));
    return od;
  };

  // ======================================================================
  // 1. Destination-out, on ALL FOUR channels
  // ======================================================================
  //
  // `dst' = dst * (1 - e)`, one factor for rgb and alpha alike, because
  // core::Tile is premultiplied. Scaling the alpha alone is the plausible wrong
  // version and it is checked against here rather than merely argued about: it
  // would leave the colour un-premultiplying to `colour / (1 - e)`, so a
  // half-erased red would read back *brighter* than the paint it came from.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // A deliberately uneven colour: three equal channels would pass just as
    // happily against an implementation that scaled the wrong one.
    const std::array<float, 4> paint{0.8f, 0.4f, 0.2f, 1.0f};
    fillRect(store, 100, 100, 160, 160, paint);
    const std::array<float, 4> before = readAt(store, 128, 128);

    const BrushTip t = discTip(20.0f, 1.0f);
    RgbEraseStroke stroke;
    stroke.begin(0.5f);
    stroke.eraseDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);

    const std::array<float, 4> after = readAt(store, 128, 128);
    std::printf("  [measured] (%.4f %.4f %.4f a=%.4f) erased at strength 0.50 -> "
                "(%.4f %.4f %.4f a=%.4f)\n",
                static_cast<double>(before[0]), static_cast<double>(before[1]),
                static_cast<double>(before[2]), static_cast<double>(before[3]),
                static_cast<double>(after[0]), static_cast<double>(after[1]),
                static_cast<double>(after[2]), static_cast<double>(after[3]));

    // **Zero tolerance, and it is earned rather than lucky**: `e` is exactly
    // 0.5 here (one full-flow dab from E = 0 to the cap), and multiplying a
    // normal binary16 by 0.5 decrements its exponent and touches no significand
    // bit. So "exactly half, in all four channels" is a real claim about the
    // arithmetic, not a tolerance that happened to be met.
    check(after[0] == before[0] * 0.5f && after[1] == before[1] * 0.5f &&
              after[2] == before[2] * 0.5f && after[3] == before[3] * 0.5f,
          "destination-out: a strength-0.5 dab halves ALL FOUR channels, at zero tolerance "
          "-- premultiplied storage, so rgb and alpha take the same factor");

    // The consequence, stated as its own assertion because it is the one a
    // picture shows: the colour did not move, only how much of it is there.
    check(after[0] / after[3] == before[0] / before[3] &&
              after[2] / after[3] == before[2] / before[3],
          "destination-out: the un-premultiplied colour is UNCHANGED -- scaling alpha alone "
          "would leave it brighter by 1/(1-e), which is the fringe on exactly the soft edges "
          "an eraser is used for");
    stroke.end();

    // And the general case, where the factor is not a power of two and the f16
    // bound is what applies. One dab at flow 0.35, strength 1: `E' = 0.35`, so
    // `e = 0.35` and the texel keeps 0.65 of itself.
    OpenDocument od2 = makeRgbDoc(256, 256, 0);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 100, 100, 160, 160, paint);
    RgbEraseStroke stroke2;
    stroke2.begin(1.0f);
    stroke2.eraseDab(store2, discTip(20.0f, 0.35f), Vec2{128.0f, 128.0f}, 256, 256, nullptr,
                     nullptr);
    const std::array<float, 4> partial = readAt(store2, 128, 128);
    std::printf("  [measured] one flow-0.35 dab leaves alpha %.9f (0.65 exactly; one f16 "
                "rounding bounds the error at %.3e)\n",
                static_cast<double>(partial[3]), static_cast<double>(kHalfRel * 0.65));
    check(nearHalf(partial[3], 0.65f) && nearHalf(partial[0], 0.8f * 0.65f),
          "one dab: flow 0.35 at strength 1 removes exactly 0.35 of the texel, to a single "
          "f16 rounding -- `e = flow * coverage` on the first dab, with nothing accumulated "
          "to damp it yet");
    stroke2.end();
  }

  // ======================================================================
  // 2. One dab: the removed fraction profile IS the coverage profile
  // ======================================================================
  //
  // With flow 1, strength 1 and a full texel underneath, `E' = cov` exactly, so
  // the stored alpha is `1 - cov` and this is the erase and the falloff checked
  // against each other. It is also the check that would catch the dab being
  // applied at the wrong offset or through the wrong channel, which no aggregate
  // assertion below would notice.
  //
  // **The falloff is `dabCoverage()`, the brush's own**, and that is a
  // requirement rather than reuse: a painter alternates brush and eraser over one
  // edge, and a rim that erased one texel wider than it painted would eat the
  // stroke it was tidying.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 40, 40, 90, 90, {1.0f, 1.0f, 1.0f, 1.0f});

    BrushTip t = discTip(10.0f, 1.0f);
    t.hardness = 0.0f;  // a pure smoothstep, so the profile is not a plateau
    RgbEraseStroke stroke;
    stroke.begin(1.0f);
    stroke.eraseDab(store, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);

    float worst = 0.0f;
    for (int32_t x = 55; x < 75; ++x) {
      const float cov = dabCoverage(t, static_cast<float>(x) + 0.5f - 64.5f, 0.0f);
      worst = std::max(worst, std::fabs((1.0f - readAt(store, x, 64)[3]) - cov));
    }
    std::printf("  [measured] worst |removed fraction - dabCoverage| across a dab row: %.6e "
                "(one f16 rounding bounds it at %.6e)\n",
                static_cast<double>(worst), static_cast<double>(kHalfRel));
    check(worst <= kHalfRel + kHalfFloor,
          "one dab: the fraction removed across a row IS dabCoverage(), to a single f16 "
          "rounding -- the brush's own falloff, so the eraser cannot eat a rim the brush "
          "never painted");

    check(readAt(store, 64 - 11, 64) == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} &&
              readAt(store, 64 + 11, 64) == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f},
          "one dab: the texel at and beyond the radius is BIT-IDENTICAL to what it was, at "
          "zero tolerance -- the footprint is bounded, so the reported tile set can be "
          "complete");
    stroke.end();
  }

  // ======================================================================
  // 3. Erasing to nothing leaves EXACTLY nothing, in all four channels
  // ======================================================================
  //
  // A texel at alpha 0 holding non-zero RGB is a malformed premultiplied texel:
  // `core/Composite` reads it straight, so it contributes colour with no
  // coverage -- it glows. Nothing in the pipeline flags it, and it survives a
  // save. This is the one assertion that has to be at exactly zero and on every
  // channel, and it holds because `e == 1` makes `keep` exactly 0 and `0.0f *
  // finite` is exactly `0.0f`.
  {
    OpenDocument od = makeRgbDoc(128, 128, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 40, 40, 90, 90, {0.9f, 0.5f, 0.25f, 1.0f});

    RgbEraseStroke stroke;
    stroke.begin(1.0f);
    // Flow 1 from `E == 0` reaches the cap in one dab, exactly: `E' = 0 + 1 * 1`
    // is 1 with no rounding anywhere, so `e` is exactly 1. A flow below 1 at
    // strength 1 converges to zero geometrically instead and only reaches exact
    // zero when the retained value underflows binary16 -- true, and a much
    // weaker thing to assert, which is why the tip here has flow 1.
    stroke.eraseDab(store, discTip(20.0f, 1.0f), Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> gone = readAt(store, 64, 64);
    std::printf("  [measured] fully erased texel: (%.9f %.9f %.9f a=%.9f)\n",
                static_cast<double>(gone[0]), static_cast<double>(gone[1]),
                static_cast<double>(gone[2]), static_cast<double>(gone[3]));
    check(gone == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "full erase: alpha AND all three colour channels are exactly 0 -- a texel with "
          "alpha 0 and colour left in it is malformed, and core/Composite reads it as an "
          "additive glow with no coverage");

    // Through the compositor as well as through the store, because the claim is
    // about what reaches the picture.
    const std::vector<float> comp = compositeDocumentPremultiplied(od.document);
    const size_t i = (static_cast<size_t>(64) * 128 + 64) * 4;
    check(comp[i] == 0.0f && comp[i + 1] == 0.0f && comp[i + 2] == 0.0f && comp[i + 3] == 0.0f,
          "full erase: and the composite of that texel is exactly transparent black -- the "
          "erased area is indistinguishable from one that was never painted");
  }

  // ======================================================================
  // 4. Strength is a PER-STROKE FLOOR, not a per-dab multiplier
  // ======================================================================
  //
  // The headline. brush/RgbErase.hpp §2 is the argument; this is the
  // measurement, with the rejected per-dab model computed beside it in the same
  // loop so the two numbers are directly comparable.
  //
  // The floor is `alpha_0 * (1 - strength)`, and the accumulator that produces it
  // holds the **fraction removed**, not the alpha -- which is why it works
  // identically on a texel that starts at 1.0 and one that starts at 0.3. Both
  // are checked, in the same stroke, because "eraser does nothing to faint
  // paint" is exactly what an absolute floor would produce and it would pass
  // every assertion made only on an opaque texel.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    constexpr float kStrength = 0.5f;
    constexpr int kDabs = 50;
    const BrushTip t = discTip(20.0f, 0.35f);

    // Two starting alphas under one dab, both in its flat core.
    fillRect(store, 110, 120, 130, 136, {0.2f, 0.4f, 0.8f, 1.0f});
    fillRect(store, 131, 120, 145, 136, {0.06f, 0.12f, 0.24f, 0.3f});
    const float opaque0 = readAt(store, 120, 128)[3];
    const float faint0 = readAt(store, 138, 128)[3];

    RgbEraseStroke stroke;
    stroke.begin(kStrength);
    size_t writingDabs = 0;
    // The model this build rejected, run on the same numbers: each dab removes
    // `flow * cov * strength` with no memory of the ones before, so the retained
    // fraction is a plain geometric decay with no floor at all.
    float perDabRetained = 1.0f;
    for (int i = 0; i < kDabs; ++i) {
      const DepositCount c =
          stroke.eraseDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
      if (c.texels > 0) ++writingDabs;
      perDabRetained *= (1.0f - t.flow * kStrength);
    }

    const float removedOpaque = stroke.strokeEraseAt(PixelCoord{120, 128});
    const float removedFaint = stroke.strokeEraseAt(PixelCoord{138, 128});
    const float storedOpaque = readAt(store, 120, 128)[3];
    const float storedFaint = readAt(store, 138, 128)[3];
    const float bound = static_cast<float>(writingDabs) * kHalfRel;
    std::printf("  [measured] %d overlapping dabs at strength %.2f: removed fraction %.9f "
                "(opaque texel) and %.9f (alpha-0.3 texel); only %zu of them wrote anything\n",
                kDabs, static_cast<double>(kStrength), static_cast<double>(removedOpaque),
                static_cast<double>(removedFaint), writingDabs);
    std::printf("  [measured] alpha %.6f -> %.9f (floor %.9f, |err| %.3e, bound %.3e); "
                "alpha %.6f -> %.9f (floor %.9f)\n",
                static_cast<double>(opaque0), static_cast<double>(storedOpaque),
                static_cast<double>(opaque0 * 0.5f),
                static_cast<double>(std::fabs(storedOpaque - opaque0 * 0.5f)),
                static_cast<double>(bound * opaque0), static_cast<double>(faint0),
                static_cast<double>(storedFaint), static_cast<double>(faint0 * 0.5f));
    std::printf("  [measured] the REJECTED per-dab model retains %.9f of the texel on the "
                "identical numbers -- a \"50%%\" eraser that removes %.4f%% of the paint\n",
                static_cast<double>(perDabRetained),
                static_cast<double>(100.0f * (1.0f - perDabRetained)));

    check(removedOpaque == kStrength && removedFaint == kStrength,
          "floor: 50 overlapping dabs remove EXACTLY the stroke's strength and no more -- and "
          "the same fraction from a texel that started at alpha 0.3 as from an opaque one, "
          "because the accumulator holds the fraction removed rather than the alpha");
    check(std::fabs(storedOpaque - opaque0 * 0.5f) <= bound * opaque0 + kHalfFloor &&
              std::fabs(storedFaint - faint0 * 0.5f) <= bound * faint0 + kHalfFloor,
          "floor: and both layers' stored alphas land on alpha_0 * (1 - strength), within the "
          "f16 bound derived from the number of writes -- the floor is a PROPORTION of what "
          "was there, which is what \"50% eraser\" means to a painter");
    check(perDabRetained < 0.01f,
          "floor: the rejected per-dab model is *checked to be wrong* on these numbers -- it "
          "grinds the texel to under 1% of itself, so the assertion above cannot pass against "
          "it and prove nothing");
    check(writingDabs < 5 && writingDabs > 0,
          "floor: once the floor is reached the remaining dabs write NOTHING -- not a value "
          "equal to what is there, nothing at all, so a scrubbed erase stops dirtying tiles "
          "and live feedback stops re-uploading them");

    // The colour at the floor is still the colour -- an erase must not be a
    // recolour. Checked as the un-premultiplied ratio, which is the quantity a
    // viewer sees.
    const std::array<float, 4> texel = readAt(store, 120, 128);
    check(nearHalf(texel[0] / texel[3], 0.2f) && nearHalf(texel[2] / texel[3], 0.8f),
          "floor: the texel at the floor holds its ORIGINAL colour at the new alpha -- the "
          "eraser changes how much paint is there, never which paint");
    stroke.end();
  }

  // ======================================================================
  // 5. The floor holds at EVERY coverage, not only under the dab's core
  // ======================================================================
  //
  // `E' = E + w(1 - E)` converges to the cap for any positive `w`, so a rim texel
  // at 3 % coverage reaches the same floor as the centre -- it just takes longer.
  // That is what makes the floor a property of the stroke rather than of the tip,
  // and it is the reason a scrubbed erase has a flat interior instead of a hole
  // with a bright ring around it.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 40, 40, 90, 90, {1.0f, 1.0f, 1.0f, 1.0f});
    BrushTip t = discTip(10.0f, 0.3f);
    t.hardness = 0.0f;
    RgbEraseStroke stroke;
    stroke.begin(0.5f);
    for (int i = 0; i < 200; ++i)
      stroke.eraseDab(store, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);

    bool flat = true;
    for (const int32_t x : {64, 68, 71, 73}) {
      const float cov = dabCoverage(t, static_cast<float>(x) + 0.5f - 64.5f, 0.0f);
      const float got = stroke.strokeEraseAt(PixelCoord{x, 64});
      std::printf("  [measured] rim texel x=%d at coverage %.4f reaches removed fraction "
                  "%.9f\n",
                  x, static_cast<double>(cov), static_cast<double>(got));
      if (got != 0.5f) flat = false;
    }
    check(flat,
          "floor: a 3%-coverage rim texel reaches the SAME floor as the 100% centre, at zero "
          "tolerance -- the floor belongs to the stroke, so a scrubbed erase has a flat "
          "interior rather than a bright ring");
    stroke.end();
  }

  // ======================================================================
  // 6. The floor is per STROKE -- a second pass cuts deeper
  // ======================================================================
  //
  // The boundary of section 4's claim, asserted rather than left implied. If the
  // accumulator survived pen-up, a painter could never take a second bite, which
  // is as wrong in the other direction: the tool would appear to stop working
  // after the first drag.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 100, 100, 160, 160, {1.0f, 1.0f, 1.0f, 1.0f});
    const BrushTip t = discTip(20.0f, 1.0f);
    for (int pass = 0; pass < 2; ++pass) {
      RgbEraseStroke stroke;
      stroke.begin(0.5f);
      for (int i = 0; i < 10; ++i)
        stroke.eraseDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
      stroke.end();
    }
    const float after = readAt(store, 128, 128)[3];
    std::printf("  [measured] two separate strokes at strength 0.50 leave %.9f (one stroke "
                "leaves 0.50; 0.5 * 0.5 = 0.25)\n",
                static_cast<double>(after));
    check(after == 0.25f,
          "per-stroke: a SECOND strength-0.5 stroke takes the texel to 0.25 -- the "
          "accumulator is thrown away at pen-up, so a painter can still cut deeper; a floor "
          "that survived the stroke would be a layer lock, not a strength");
  }

  // ======================================================================
  // 7. The active selection bounds the erase (PRD E1, P0)
  // ======================================================================
  //
  // Both halves of brush/RgbErase.hpp §3: the selection scales what one dab
  // removes, AND it caps what any number of dabs can remove. The second is what
  // makes it a bound rather than a speed limit. It matters more here than in the
  // deposit: what a runaway eraser destroys outside the ants is not visible until
  // the layer under it is, and one undo step covers the whole stroke.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 255, 200, {1.0f, 1.0f, 1.0f, 1.0f});
    // A fractional left edge, so one column of texels is PARTIALLY selected --
    // an in-or-out boundary would pass against a version that treated the
    // selection as a bitmask.
    Selection sel = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    const float partial = selectionCoverageAt(&sel, PixelCoord{64, 64});
    const std::array<float, 4> outsideBefore = readAt(store, 50, 64);

    const BrushTip t = discTip(30.0f, 1.0f);
    RgbEraseStroke stroke;
    stroke.begin(1.0f);
    stroke.eraseDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    const float onePass = readAt(store, 64, 64)[3];

    check(partial > 0.0f && partial < 1.0f,
          "selection: the boundary column really is PARTIALLY covered -- the premise the two "
          "assertions below rest on, or they would pass against a bitmask selection");
    check(readAt(store, 50, 64) == outsideBefore && readAt(store, 60, 64) == outsideBefore,
          "selection: a texel outside the ants is BIT-IDENTICAL afterwards, at zero tolerance "
          "-- an eraser that leaked past a selection destroys work whose only trace is one "
          "undo step covering the whole stroke");
    check(nearHalf(onePass, 1.0f - partial),
          "selection: one full-flow dab through a PARTIALLY selected texel removes exactly "
          "its coverage -- so an antialiased selection edge erases antialiased rather than "
          "stepped");

    for (int i = 0; i < 40; ++i)
      stroke.eraseDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    const float scrubbed = stroke.strokeEraseAt(PixelCoord{64, 64});
    std::printf("  [measured] selection coverage %.6f: one dab removes %.6f, 41 dabs remove "
                "%.6f (an unbounded model would remove 1.0)\n",
                static_cast<double>(partial), static_cast<double>(1.0f - onePass),
                static_cast<double>(scrubbed));
    check(scrubbed == partial,
          "selection: 41 dabs through a partially selected texel still remove exactly its "
          "coverage and no further -- gating only the flow makes a bound a scrubbing eraser "
          "walks straight through, which is the same speed dependence in another costume");
    check(readAt(store, 80, 64)[3] == 0.0f,
          "selection: and a fully selected texel is erased completely by the same stroke -- "
          "the gate costs nothing where the selection covers everything");
    stroke.end();

    // The null case, through this loop and not only through
    // selectionCoverageAt(): core/SelectionMask.hpp requires every hoisted
    // per-texel loop to repeat the "null Selection means 1.0" branch itself, and
    // warns that a perturbation inverting one copy leaves the others right.
    OpenDocument od2 = makeRgbDoc(256, 256, 0);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 0, 0, 255, 200, {1.0f, 1.0f, 1.0f, 1.0f});
    RgbEraseStroke stroke2;
    stroke2.begin(1.0f);
    stroke2.eraseDab(store2, t, Vec2{64.0f, 64.0f}, 256, 256, nullptr, nullptr);
    stroke2.end();
    check(readAt(store2, 64, 64)[3] == 0.0f && readAt(store2, 50, 64)[3] == 0.0f,
          "selection: a NULL selection means no restriction, asserted through this loop's own "
          "copy of that branch -- reading it as \"selects nothing\" inverts the editor");

    // An ENGAGED selection that names no tile at all is the other end of the
    // same rule, and it is the one an optimisation can silently break by
    // treating "no tile here" as "no restriction here".
    OpenDocument od3 = makeRgbDoc(256, 256, 0);
    TileStore& store3 = *od3.document.layers[0].rgbTiles;
    fillRect(store3, 0, 0, 255, 200, {1.0f, 1.0f, 1.0f, 1.0f});
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    RgbEraseStroke stroke3;
    stroke3.begin(1.0f);
    stroke3.eraseDab(store3, t, Vec2{64.0f, 64.0f}, 256, 256, &elsewhere, nullptr);
    stroke3.end();
    check(readAt(store3, 64, 64) == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f},
          "selection: a dab entirely outside an engaged selection changes NOTHING -- an "
          "absent selection tile is \"selects nothing\", the inverse of a mask's absent tile");

    // And the whole of it once more through `StrokeSession`, because PRD E1 is a
    // claim about the tool and not about the module: the session reads
    // `OpenDocument::selection` live, and a route that forgot to pass it would
    // pass every assertion above.
    OpenDocument od4 = makeRgbDoc(256, 256, 0);
    fillRect(*od4.document.layers[0].rgbTiles, 0, 0, 255, 200, {1.0f, 1.0f, 1.0f, 1.0f});
    od4.selection = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    BrushTip sessionTip = discTip(24.0f, 1.0f);
    sessionTip.opacity = 1.0f;
    StrokeSession s;
    std::string e;
    check(s.begin(od4, 0, sessionTip, Tool::Eraser, &e) && e.empty(),
          "selection: an erase session begins on the RGB layer with a selection engaged");
    for (int k = 0; k < 12; ++k) s.addPoint(64.0f, 40.0f + 6.0f * static_cast<float>(k));
    s.end();
    const TileStore& gated = *od4.document.layers[0].rgbTiles;
    check(readAt(gated, 40, 64) == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} &&
              nearHalf(readAt(gated, 64, 64)[3], 1.0f - partial),
          "selection: PRD E1 through the SESSION -- a scrubbed erase stops dead outside the "
          "ants and stops at the coverage on the feathered column, with the selection read "
          "live off the document rather than latched");
  }

  // ======================================================================
  // 8. A fast stroke and a slow stroke erase the same paint
  // ======================================================================
  //
  // ADR-0003: deposition depends on distance travelled, never on how many input
  // events that distance was divided into. `brush/StrokePath` already guarantees
  // the *dabs* are the same; what this checks is that nothing in the erase
  // reintroduced a dependence on the sample count -- which a per-dab strength, a
  // per-sample composite, or an accumulator keyed on anything but position all
  // would.
  //
  // **The path is straight, and that is a condition of the claim rather than a
  // convenience.** The dab stream comes off a centripetal Catmull-Rom fit whose
  // control points *are* the samples, so on a curve two sample rates describe two
  // slightly different curves and the dab positions genuinely differ. Bit
  // identity is therefore the right assertion here and would be the wrong one
  // there; on a curve the claim that survives is the weaker one that both are
  // arc-length parameterisations of the path the user drew. Same caveat, same
  // reason, as the deposit's own speed section.
  {
    auto eraseLine = [&](int samples) {
      OpenDocument od = makeRgbDoc(256, 256, 0);
      fillRect(*od.document.layers[0].rgbTiles, 20, 75, 240, 125, {0.9f, 0.1f, 0.05f, 1.0f});
      BrushTip t;
      t.radius = 12.0f;
      t.hardness = 0.35f;
      t.flow = 0.4f;
      t.spacing = 0.25f;
      t.opacity = 1.0f;
      StrokeSession s;
      std::string e;
      s.begin(od, 0, t, Tool::Eraser, &e);
      for (int i = 0; i <= samples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(samples);
        s.addPoint(40.0f + u * 180.0f, 100.0f);
      }
      s.end();
      return std::make_pair(std::move(od), s.dabCount());
    };

    auto slow = eraseLine(60);  // one sample per frame, a leisurely drag
    auto fast = eraseLine(3);   // the same 180 px in three frames

    const TileStore& a = *slow.first.document.layers[0].rgbTiles;
    const TileStore& b = *fast.first.document.layers[0].rgbTiles;
    float worstA = 0.0f;
    float worstRgb = 0.0f;
    for (int32_t y = 80; y < 120; ++y) {
      for (int32_t x = 25; x < 235; ++x) {
        const std::array<float, 4> pa = readAt(a, x, y);
        const std::array<float, 4> pb = readAt(b, x, y);
        worstA = std::max(worstA, std::fabs(pa[3] - pb[3]));
        for (int c = 0; c < 3; ++c) worstRgb = std::max(worstRgb, std::fabs(pa[c] - pb[c]));
      }
    }
    std::printf("  [measured] 61 samples vs 4 samples over the same 180 px: %zu dabs vs %zu; "
                "worst |dAlpha| %.6e, worst |dRGB| %.6e; mid-stroke alpha %.6f (from 1.0)\n",
                slow.second, fast.second, static_cast<double>(worstA),
                static_cast<double>(worstRgb), static_cast<double>(readAt(a, 130, 100)[3]));

    check(slow.second == fast.second && slow.second >= 50,
          "speed: 61 samples and 4 samples over the same straight 180 px emit the IDENTICAL "
          "number of dabs -- ADR-0003, and the premise the next assertion rests on");
    check(worstA == 0.0f && worstRgb == 0.0f,
          "speed: and the two layers are BIT-IDENTICAL over the whole stroke, at zero "
          "tolerance -- a per-dab strength, or any accumulation keyed on samples rather than "
          "position, would make the slow stroke the deeper one");
    check(readAt(a, 130, 100)[3] < 0.5f,
          "speed: checked against a stroke that actually erased something -- two untouched "
          "layers are also bit-identical");
  }

  // ======================================================================
  // 9. Erasing nothing COSTS nothing (brush/RgbErase.hpp §4)
  // ======================================================================
  //
  // The asymmetry with the deposit, and the reason `ops/FloodFill` states the
  // same one for its own pair: a clear can only remove, so a tile that does not
  // exist has nothing to lose. An eraser that allocated on blank canvas would be
  // a tool that grows the document by being used on nothing, and would then
  // report every one of those tiles dirty for re-composite and re-upload on every
  // frame of the drag.
  {
    OpenDocument od = makeRgbDoc(512, 512, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;

    BrushTip t = discTip(30.0f, 1.0f);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string e;
    check(s.begin(od, 0, t, Tool::Eraser, &e) && s.route() == StrokeRoute::RgbErase,
          "empty erase: begins normally on the blank RGB layer, and reports the erase route");
    for (int k = 0; k < 20; ++k) s.addPoint(60.0f + 20.0f * static_cast<float>(k), 200.0f);
    s.end();

    std::printf("  [measured] a %zu-dab erase across blank canvas: %zu texels, %zu tiles "
                "reported, %zu tiles in the layer\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                store.occupiedTileCount());
    check(s.dabCount() > 0 && store.occupiedTileCount() == 0 && s.strokeTiles().empty(),
          "empty erase: a stroke of dozens of dabs across unpainted canvas allocates NOT ONE "
          "tile and reports none -- 224 KiB per tile crossed, plus a dirty tile per frame, is "
          "what the absent-tile skip is worth");
    check(s.texelsWritten() == 0 && od.history.entries().size() == entries &&
              od.revision == rev,
          "empty erase: and it records NO history entry and moves no revision -- an undo step "
          "that undoes nothing is worse than a missing one, and here it arrives from the "
          "arithmetic rather than from a special case");

    // The other half of §4: a texel holding colour at alpha 0 is malformed, not
    // empty, and the eraser must scale it rather than declaring it absent.
    TileStore odd;
    const PixelCoord p{10, 10};
    odd.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {0.4f, 0.0f, 0.0f, 0.0f});
    RgbEraseStroke probe;
    probe.begin(1.0f);
    const DepositCount c =
        probe.eraseDab(odd, discTip(4.0f, 1.0f), Vec2{10.5f, 10.5f}, 64, 64, nullptr, nullptr);
    probe.end();
    check(c.texels > 0 && readAt(odd, 10, 10) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "empty erase: a MALFORMED texel (colour at alpha 0) is erased rather than skipped -- "
          "the skip tests all four channels, so \"nothing here\" means nothing, not \"no "
          "alpha here\"");
  }

  // ======================================================================
  // 10. The routing table's Eraser rows (app/StrokeSession section 1)
  // ======================================================================
  //
  // Every one of these rows used to be `None` for the trivial reason that
  // `Tool::Eraser` sat in the not-built list -- so the tool did nothing, said
  // nothing, and the options bar's route indicator was the only place in the
  // chrome that could have told anyone, reading a grey "-> none" that was
  // accurate and useless.
  {
    Layer rgbLayer = makeRgbLayer("r");
    Layer pigment = makePigmentLayer("p");
    Layer lockedRgb = makeRgbLayer("lr");
    lockedRgb.locked = true;
    Layer hiddenRgb = makeRgbLayer("hr");
    hiddenRgb.visible = false;
    Layer storelessRgb = makeRgbLayer("sr");
    storelessRgb.rgbTiles.reset();
    Layer adjustment = makeAdjustmentLayer("adj");
    Layer media = makeRgbLayer("m");
    media.kind = LayerKind::Media;
    media.rgbTiles.reset();

    check(strokeRouteFor(Tool::Eraser, &rgbLayer) == StrokeRoute::RgbErase,
          "routing: the Eraser on a writable RGB layer -> the erase route; this row used to "
          "say None, which is why the tool did nothing at all");
    check(strokeRouteFor(Tool::Brush, &rgbLayer) == StrokeRoute::RgbDeposit &&
              strokeRouteFor(Tool::DryBrush, &rgbLayer) == StrokeRoute::RgbDeposit,
          "routing: and the brush on the SAME layer still deposits -- the two tools give two "
          "different answers about one layer, which is why the session latches its tool");
    check(strokeRouteFor(Tool::Eraser, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer takes a deposit and REFUSES an erase -- ADR-0007's mass "
          "reduction needs the pigment route's missing selection gate (PRD E1, P0), so it "
          "refuses by name rather than half-implementing a P0");
    check(strokeRouteFor(Tool::Eraser, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: no layer at all is None for the eraser and PaintSim for the brush -- the "
          "one row where the two families part company, because the solver has no alpha and "
          "an eraser sent there would ADD pigment");
    check(strokeRouteFor(Tool::Eraser, &lockedRgb) == StrokeRoute::None,
          "routing: a locked RGB layer refuses the eraser, exactly as it refuses the brush -- "
          "the lock is checked before the kind, so the message names the one thing a user can "
          "fix");
    check(strokeRouteFor(Tool::Eraser, &hiddenRgb) == StrokeRoute::RgbErase,
          "routing: a HIDDEN RGB layer still erases -- visibility is a view decision, the same "
          "answer the deposit row gives");
    check(strokeRouteFor(Tool::Eraser, &storelessRgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Eraser, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Eraser, &media) == StrokeRoute::None,
          "routing: an RGB layer with no store, an Adjustment layer and a Media layer each "
          "refuse -- ADR-0007 defines Media and the parametric kinds, and none of them is "
          "built, so each says so instead of silently doing nothing");
    check(std::string(strokeRouteName(StrokeRoute::RgbErase)) == "rgb-erase" &&
              strokeRouteWritesLayer(StrokeRoute::RgbErase) &&
              strokeRouteWritesLayer(StrokeRoute::RgbDeposit) &&
              strokeRouteWritesLayer(StrokeRoute::CpuDeposit) &&
              !strokeRouteWritesLayer(StrokeRoute::PaintSim) &&
              !strokeRouteWritesLayer(StrokeRoute::None),
          "routing: the new route has a name of its own and answers the one predicate four "
          "call sites ask -- a route that removes paint still writes a layer, so the options "
          "bar accents it rather than greying it as though it went to the solver");
    check(std::string(strokeEditLabel(Tool::Eraser)) == "erase" &&
              std::string(strokeEditLabel(Tool::Brush)) == "brush stroke",
          "routing: an erase is labelled \"erase\" and not \"brush stroke\" -- PRD O2's panel "
          "is scanned by name, and a row that misnames what it did is a row nobody can find");
  }

  // ======================================================================
  // 11. The erase lands on the ACTIVE layer, and on no other
  // ======================================================================
  //
  // Driven end to end through `setActiveLayer()`, `activeLayerOf()` and
  // `od.activeLayer` fed to `StrokeSession::begin()` -- the exact chain
  // `ui/MacPaintUI`'s canvas branch runs. A version that erased the right pixels
  // from the wrong store would pass every section above this one, and would be
  // the worst possible form of that bug: it destroys work on a layer the user was
  // not even looking at.
  {
    OpenDocument od = makeRgbDoc(256, 256, 1);  // layers 0 and 1, both RGB
    fillRect(*od.document.layers[0].rgbTiles, 60, 100, 200, 160, {1.0f, 0.0f, 0.0f, 1.0f});
    fillRect(*od.document.layers[1].rgbTiles, 60, 100, 200, 160, {0.0f, 0.0f, 1.0f, 1.0f});
    const std::array<float, 4> lowBefore = readAt(*od.document.layers[0].rgbTiles, 110, 128);

    setActiveLayer(od, 1);
    const Layer* target = activeLayerOf(od);
    if (target == nullptr || target->kind != LayerKind::RGB) ok = false;

    BrushTip t = discTip(24.0f, 1.0f);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string e;
    // `od.activeLayer`, not a literal -- the point is that the index the UI hands
    // over is the one that gets erased.
    check(s.begin(od, od.activeLayer, t, Tool::Eraser, &e) && e.empty(),
          "active layer: an erase begins on the active RGB layer, layer 1");
    for (int k = 0; k < 6; ++k) s.addPoint(100.0f + 6.0f * static_cast<float>(k), 128.0f);
    s.end();

    const std::array<float, 4> lowTexel = readAt(*od.document.layers[0].rgbTiles, 110, 128);
    const std::array<float, 4> upTexel = readAt(*od.document.layers[1].rgbTiles, 110, 128);
    std::printf("  [measured] at (110,128): layer 0 holds (%.4f %.4f %.4f a=%.4f), layer 1 "
                "holds (%.4f %.4f %.4f a=%.4f)\n",
                static_cast<double>(lowTexel[0]), static_cast<double>(lowTexel[1]),
                static_cast<double>(lowTexel[2]), static_cast<double>(lowTexel[3]),
                static_cast<double>(upTexel[0]), static_cast<double>(upTexel[1]),
                static_cast<double>(upTexel[2]), static_cast<double>(upTexel[3]));
    check(upTexel == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "active layer: layer 1 is erased where the stroke went -- the premise the next "
          "assertion needs, or an erase that did nothing would pass it");
    check(lowTexel == lowBefore,
          "active layer: layer 0 is BIT-IDENTICAL to what it was, at zero tolerance -- an "
          "erase aimed at one layer must not take a single bit off another");
  }

  // ======================================================================
  // 12. One stroke is one undo step, labelled "erase"
  // ======================================================================
  //
  // The history half is `app/StrokeSession` §2's and is already asserted for the
  // two deposit routes; what is new is that the erase route reaches the same
  // code, with a label of its own, and that the accumulator's lifetime claim is a
  // claim rather than a comment.
  {
    OpenDocument od = makeRgbDoc(512, 512, 0);
    fillRect(*od.document.layers[0].rgbTiles, 40, 60, 470, 380, {0.3f, 0.6f, 0.9f, 1.0f});
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    const uint64_t structBefore = od.structuralRevision;

    BrushTip t;
    t.radius = 18.0f;
    t.hardness = 0.4f;
    t.flow = 0.2f;
    t.opacity = 0.8f;

    StrokeSession s;
    std::string e;
    s.begin(od, 0, t, Tool::Eraser, &e);
    for (int i = 0; i < 40; ++i) {
      const float u = static_cast<float>(i) / 39.0f;
      s.addPoint(60.0f + 380.0f * u, 120.0f + 220.0f * std::sin(u * 3.1415926f));
    }
    check(od.history.entries().size() == entries,
          "undo: not one history entry has appeared mid-stroke, however many dabs it is");
    s.end();

    std::printf("  [measured] a %zu-dab erase wrote %zu texels across %zu tiles; the history "
                "entry is \"%s\"\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                od.history.entries().back().label.c_str());
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "erase",
          "undo: a stroke of hundreds of dabs is EXACTLY ONE history entry, labelled "
          "\"erase\" -- ADR-0005's stroke-granular undo, reached by the new route too");
    check(od.revision > revBefore && od.structuralRevision == structBefore,
          "undo: it moved the content revision and not the structural one, so an erase costs "
          "at most one journal write per interval rather than one per frame");

    // The accumulator, measured on both sides of pen-up. A live one is one 64 KiB
    // float tile per tile the stroke touched -- the same StrokeAlphaTile the
    // deposit uses, holding the fraction removed instead of the alpha added.
    OpenDocument probeDoc = makeRgbDoc(512, 512, 0);
    TileStore& scratch = *probeDoc.document.layers[0].rgbTiles;
    fillRect(scratch, 0, 100, 460, 300, {1.0f, 1.0f, 1.0f, 1.0f});
    RgbEraseStroke probe;
    probe.begin(1.0f);
    BrushTip wide = discTip(60.0f, 0.5f);
    for (int i = 0; i < 4; ++i)
      probe.eraseDab(scratch, wide, Vec2{80.0f + 100.0f * static_cast<float>(i), 200.0f}, 512,
                     512, nullptr, nullptr);
    const size_t liveTiles = probe.accumulatorTiles();
    const size_t liveBytes = probe.accumulatorBytes();
    probe.end();
    std::printf("  [measured] accumulator while erasing: %zu tiles, %zu KiB (%zu KiB per "
                "tile); after pen-up: %zu tiles, %zu KiB\n",
                liveTiles, liveBytes / 1024, sizeof(StrokeAlphaTile) / 1024,
                probe.accumulatorTiles(), probe.accumulatorBytes() / 1024);
    check(liveTiles > 0 && liveBytes == liveTiles * sizeof(StrokeAlphaTile),
          "accumulator: a stroke in flight holds exactly one 64 KiB float tile per tile it has "
          "touched -- sparse, so an erase that reaches nothing costs nothing");
    check(probe.accumulatorTiles() == 0 && probe.accumulatorBytes() == 0 && !probe.active(),
          "accumulator: pen-up frees ALL of it -- an application sitting idle after a long "
          "erase must not still be holding the stroke's scratch");
  }

  // ======================================================================
  // 13. Every refusal, by name
  // ======================================================================
  //
  // Refusing by name rather than doing nothing is the whole difference between
  // this step and the state it replaced: the eraser *already* did nothing on
  // every layer kind, silently, and no assertion about silence can tell the two
  // apart. Each of these checks that the sentence names the layer, so a user is
  // told which of their layers refused and why.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("locked")));
    setLayerLocked(od.document, 1, true);
    recordLayerEdit(od,
                    addLayer(od.document, od.document.layers.size(), makePigmentLayer("pig")));
    recordLayerEdit(od,
                    addLayer(od.document, od.document.layers.size(), makeAdjustmentLayer("adj")));

    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;
    BrushTip t = discTip(8.0f, 0.5f);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string error;

    check(!s.begin(od, 1, t, Tool::Eraser, &error) && contains(error, "locked") &&
              contains(error, "erase") && contains(error, "none"),
          "refusal: a LOCKED RGB layer refuses the erase and names the lock, the tool and the "
          "route -- the one refusal of the four with a fix the user can carry out");
    check(!s.begin(od, 2, t, Tool::Eraser, &error) && contains(error, "Pigment") &&
              contains(error, "erase"),
          "refusal: a PIGMENT layer refuses BY NAME and names its kind -- ADR-0007's mass "
          "reduction is owed, and a silent no-op is exactly what this step replaced");
    check(!s.begin(od, 3, t, Tool::Eraser, &error) && contains(error, "Adjustment"),
          "refusal: an Adjustment layer refuses by name -- it holds no tiles at all");
    check(!s.begin(od, 99, t, Tool::Eraser, &error) && contains(error, "out of range"),
          "refusal: an out-of-range index refuses without touching the document");
    check(od.history.entries().size() == entries && od.revision == rev && !s.active(),
          "refusal: not one of the four refusals recorded an entry or moved the revision");

    // The Pigment refusal is told apart from the locked one in the half of the
    // sentence that names the fix, on two layers whose names differ -- so the
    // difference cannot be mistaken for a name.
    std::string lockedWhy;
    std::string pigmentWhy;
    s.begin(od, 1, t, Tool::Eraser, &lockedWhy);
    s.begin(od, 2, t, Tool::Eraser, &pigmentWhy);
    std::printf("  [measured] locked: %s\n  [measured] pigment: %s\n", lockedWhy.c_str(),
                pigmentWhy.c_str());
    check(lockedWhy != pigmentWhy && !lockedWhy.empty() && !pigmentWhy.empty(),
          "refusal: the locked and Pigment sentences are DIFFERENT -- both present to a user "
          "as \"the eraser did nothing\", and only one of them has a switch in LAYERS that "
          "fixes it");

    // Strength 0 is a legitimate setting, not an error, and must behave as one.
    fillRect(*od.document.layers[0].rgbTiles, 40, 40, 120, 120, {1.0f, 1.0f, 1.0f, 1.0f});
    const std::array<float, 4> untouched = readAt(*od.document.layers[0].rgbTiles, 80, 80);
    BrushTip inert = t;
    inert.opacity = 0.0f;
    check(s.begin(od, 0, inert, Tool::Eraser, &error), "strength 0: begins normally");
    for (int i = 0; i < 8; ++i) s.addPoint(60.0f + 5.0f * static_cast<float>(i), 80.0f);
    s.end();
    check(s.texelsWritten() == 0 && od.history.entries().size() == entries &&
              readAt(*od.document.layers[0].rgbTiles, 80, 80) == untouched,
          "strength 0: an erase at zero strength removes nothing and records nothing, and the "
          "texels it passed over are bit-identical -- a setting, refused by arithmetic rather "
          "than by a special case");

    // The suite's own configuration claim: nothing in this section opens a file,
    // so every number above is the same in both NP_USE_OIIO builds.
    check(oiioBackendCompiledIn(),
          "config: this whole section reads no file, so its answers are configuration-"
          "independent -- and this build really does have the OIIO backend compiled in");
  }

  std::printf("[selftest] rgb erase %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
