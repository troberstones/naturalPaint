#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/TonalBrush.hpp"
#include "core/SelectionShapes.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Dodge and Burn on a plain RGB layer (brush/TonalBrush), and the routing that
// made them exist at all (app/StrokeSession §1's Dodge/Burn rows).
//
// See app/SelfTest.hpp for the section's own contents list, and the two headers
// for every decision this file only checks. Four things are worth saying here
// because they are what the section is *for*:
//
//   * **The two tools did nothing.** Not "did the wrong thing" and not "were
//     approximate": `Tool::Dodge` and `Tool::Burn` sat in `strokeRouteFor()`'s
//     not-built list, so a drag with either reached no layer, produced no
//     message, moved no revision and recorded nothing. They drew a cursor.
//   * **The alpha invariant is the claim a picture does not show.** A tonal op
//     adjusts colour and must not create or destroy coverage, and a version
//     that leaked into alpha would read as a slightly soft edge rather than as
//     a bug. Section 1 asserts alpha at ZERO tolerance across a whole ramp of
//     starting alphas, and asserts the colour moved in the same breath so the
//     first claim cannot be satisfied by a tool that did nothing at all.
//   * **The working space is a decision and not a default.** This build's
//     texels are linear light and the classic dodge/burn is defined on
//     display-referred tone; section 2 asserts the chosen domain by its
//     consequences -- the endpoints are fixed, the same stroke is a different
//     exposure change at different tones, and a value past display white is
//     left bit-identical rather than silently darkened by a dodge.
//   * **The per-stroke ceiling (section 3) is the one piece of arithmetic here
//     that can be *plausibly* wrong**, exactly as the deposit's ceiling and the
//     eraser's floor are. A per-dab shift dodges, and looks like dodging, and is
//     only detectable by comparing two strokes that differ in how much they
//     overlap themselves. Section 3 runs both models side by side and prints the
//     number the wrong one produces.
// ---------------------------------------------------------------------------
bool runTonalBrushTest() {
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
  //    from the storage rather than borrowed from runRgbEraseTest(), which
  //    states the identical derivation for the identical storage -- a tolerance
  //    copied without its derivation is the one that later gets applied where it
  //    does not hold.
  //
  //  * **N * kHalfRel for a stroke of N WRITING dabs** -- the layer rounds to
  //    binary16 once per dab that writes, while brush/TonalBrush's accumulator
  //    does not. Unlike the erase, whose later dabs damp earlier error by a
  //    factor in [0,1], a gamma AMPLIFIES a relative error by the exponent: a
  //    relative error `r` on `d` becomes `g*r` on `d^g`, and `g` is at most
  //    `kTonalFullGamma == 2`. So the bound used below is `2 * N * kHalfRel`,
  //    and the derivation is why the 2 is there rather than because a smaller
  //    number failed.
  //
  //  * Several assertions are at **exactly zero** tolerance, and each says why
  //    in place. Three kinds: claims about which operations happen (a dab past
  //    the ceiling writes nothing at all; a texel outside the ants is
  //    untouched), claims that a channel was COPIED rather than computed (the
  //    alpha), and claims that two computations produce the same binary16 word
  //    (the one-dab and two-dab strokes of section 3).
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearRel = [&](float got, float want, float rel) {
    return std::fabs(got - want) <= std::fabs(want) * rel + kHalfFloor;
  };

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // The un-premultiplied, display-referred value of one channel -- the quantity
  // brush/TonalBrush §2's curve is actually defined on, and therefore the one
  // every number below is stated in. Reading the stored channel directly would
  // fold the texel's own alpha into every assertion and make section 1's
  // un-premultiply claim untestable.
  auto displayOf = [](const std::array<float, 4>& texel, int ch) {
    return srgbEncode(texel[ch] / texel[3]);
  };

  // The fixture is written straight into the store rather than painted with
  // brush/RgbDeposit first, deliberately, for runRgbEraseTest()'s stated
  // reason: this section's subject is what a tonal shift does to a texel that
  // is already there, and building the "already there" out of another module
  // would make every number below depend on two arithmetics instead of one.
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
  // every number below is about the tonal shift rather than about the falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h, size_t extraRgbLayers) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "tonal");
    for (size_t i = 0; i < extraRgbLayers; ++i)
      recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                   makeRgbLayer("RGB " + std::to_string(i + 1))));
    return od;
  };

  // A premultiplied texel whose STRAIGHT colour is the same grey at every
  // alpha -- the fixture section 1 and section 2 both need, because "the same
  // colour under different coverage" is the pair a premultiplied-storage bug
  // separates and nothing else does.
  auto greyAt = [](float straight, float alpha) {
    return std::array<float, 4>{straight * alpha, straight * alpha, straight * alpha, alpha};
  };

  std::printf("  -- 1. Alpha is COPIED, not computed (brush/TonalBrush section 1) --\n");
  // ======================================================================
  // 1. The alpha invariant, and the un-premultiply that DESIGN-imaging §2
  //    requires of every per-channel colour op
  // ======================================================================
  //
  // A tonal op adjusts colour; it does not create or destroy coverage. Dodging
  // a fully transparent texel must leave it fully transparent, and dodging a
  // half-covered one must leave it exactly half-covered -- so the assertion is
  // at zero tolerance on every one of a ramp of starting alphas, and the colour
  // is asserted to have MOVED in the same breath, or a tool that did nothing at
  // all would satisfy the first claim perfectly.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;

    // A ramp of alphas across the dab, all holding the SAME straight colour.
    // 0.0 is included on purpose: it is the texel §1 says has no colour to
    // shift, and it is also the one an "apply f to all four channels"
    // implementation would give coverage to.
    constexpr int kX0 = 100;
    constexpr int kN = 17;
    for (int i = 0; i < kN; ++i) {
      const float a = static_cast<float>(i) / static_cast<float>(kN - 1);
      fillRect(store, kX0 + i, 120, kX0 + i, 136, greyAt(0.35f, a));
    }
    std::array<std::array<float, 4>, kN> before{};
    for (int i = 0; i < kN; ++i) before[i] = readAt(store, kX0 + i, 128);

    TonalStroke stroke;
    stroke.begin(1.0f, TonalDirection::Dodge);
    const DepositCount c =
        stroke.toneDab(store, discTip(20.0f, 1.0f), Vec2{static_cast<float>(kX0) + 8.0f, 128.0f},
                       256, 256, nullptr, nullptr);
    stroke.end();

    bool alphaExact = true;
    bool colourMoved = true;
    for (int i = 0; i < kN; ++i) {
      const std::array<float, 4> after = readAt(store, kX0 + i, 128);
      if (after[3] != before[i][3]) alphaExact = false;
      // The alpha-0 column is excluded from "the colour moved": §1 says that
      // texel has no colour to shift and is left completely alone, which the
      // next assertion checks separately rather than smuggling in here.
      if (before[i][3] > 0.0f && after[0] == before[i][0]) colourMoved = false;
    }
    std::printf("  [measured] a full-strength dodge across %d alphas from 0.000 to 1.000: "
                "%zu texels written; alpha at a=0.5 %.9f -> %.9f\n",
                kN, c.texels, static_cast<double>(before[8][3]),
                static_cast<double>(readAt(store, kX0 + 8, 128)[3]));

    check(alphaExact,
          "alpha: every one of 17 starting alphas is BIT-IDENTICAL after a full-strength "
          "dodge, at zero tolerance -- the channel is copied out of the read, not recovered "
          "by rounding, so a tonal op cannot create or destroy coverage");
    check(colourMoved,
          "alpha: and the colour of every covered texel DID move -- without this the line "
          "above is satisfied perfectly by a tool that does nothing at all");
    check(readAt(store, kX0, 128) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "alpha: the fully TRANSPARENT texel is untouched -- no coverage means no colour to "
          "shift, and a curve applied to all four channels would give it alpha here");

    // The un-premultiply itself, which is DESIGN-imaging.md §2's boxed warning
    // and the mistake that produces a rim on exactly the soft edges this tool is
    // used on. Two texels of the SAME straight colour at two different alphas
    // must come out of the same dab holding the same straight colour again.
    //
    // **Zero tolerance is earned rather than lucky**: the second alpha is
    // exactly 0.5, and halving a normal binary16 decrements its exponent and
    // touches no significand bit, so `half(f(c) * 0.5) == half(f(c)) * 0.5`
    // exactly and the two straight readbacks are the same number.
    OpenDocument od2 = makeRgbDoc(128, 128, 0);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    const PixelCoord opaque{60, 64};
    const PixelCoord halfCovered{68, 64};
    fillRect(store2, opaque.x, opaque.y, opaque.x, opaque.y, greyAt(0.35f, 1.0f));
    fillRect(store2, halfCovered.x, halfCovered.y, halfCovered.x, halfCovered.y,
             greyAt(0.35f, 0.5f));
    TonalStroke s2;
    s2.begin(1.0f, TonalDirection::Burn);
    s2.toneDab(store2, discTip(20.0f, 1.0f), Vec2{64.0f, 64.5f}, 128, 128, nullptr, nullptr);
    s2.end();
    const std::array<float, 4> gotOpaque = readAt(store2, opaque.x, opaque.y);
    const std::array<float, 4> gotHalf = readAt(store2, halfCovered.x, halfCovered.y);
    const float straightOpaque = gotOpaque[0] / gotOpaque[3];
    const float straightHalf = gotHalf[0] / gotHalf[3];
    std::printf("  [measured] one straight colour 0.35 at alpha 1.0 and at alpha 0.5, burned: "
                "straight %.9f and %.9f\n",
                static_cast<double>(straightOpaque), static_cast<double>(straightHalf));
    check(straightOpaque == straightHalf && straightOpaque != 0.35f,
          "un-premultiply: one colour under two coverages burns to ONE colour, at zero "
          "tolerance -- running the curve on the stored (premultiplied) channels would shift "
          "the half-covered texel as though it were four stops darker, which is a rim on the "
          "soft edge this tool is used to remove");
    check(gotHalf[3] == 0.5f,
          "un-premultiply: and the half-covered texel is still exactly half covered -- the "
          "re-premultiply put the alpha back, it did not fold the shift into it");
  }

  std::printf("  -- 2. The working space: a DISPLAY-referred curve, and its consequences --\n");
  // ======================================================================
  // 2. Which space the curve lives in (brush/TonalBrush section 2)
  // ======================================================================
  //
  // The texels are linear light (CONTEXT.md's "Working space"); the classic
  // dodge/burn is defined on display-referred tone. The header decides for the
  // second and this section asserts the decision by its consequences, not by
  // reading the source: the endpoints are fixed, the same stroke is a different
  // exposure change at different tones, and a value past display white comes out
  // bit-identical instead of being silently darkened by a dodge.
  {
    // The curve on its own first, because the domain rule is the decision and
    // it is a property of `tonalCurve()` rather than of any texel that samples
    // it.
    check(tonalCurve(0.0f, 0.5f) == 0.0f && tonalCurve(1.0f, 0.5f) == 1.0f &&
              tonalCurve(0.0f, 2.0f) == 0.0f && tonalCurve(1.0f, 2.0f) == 1.0f,
          "curve: black and white are fixed points of BOTH directions, exactly -- that is "
          "what \"lightens without blowing the whites\" means, and it is why a power law and "
          "not a multiply");
    check(tonalCurve(1.75f, 0.5f) == 1.75f && tonalCurve(1.75f, 2.0f) == 1.75f &&
              tonalCurve(-0.2f, 0.5f) == -0.2f,
          "curve: outside [0,1] it is the IDENTITY, both directions -- d^g there has the "
          "wrong sign (a dodge exponent darkens a superwhite value), invisibly, because both "
          "still clip to white");
    check(tonalCurve(0.5f, 1.0f) == 0.5f && tonalCurve(0.25f, 2.0f) == 0.0625f,
          "curve: gamma 1 is the exact identity and gamma 2 squares -- the two anchors the "
          "exponent arithmetic of section 3 is built on");

    // A superwhite texel through the whole module, because "the curve leaves it
    // alone" and "the module writes nothing" are two claims and only the second
    // costs the caller anything.
    OpenDocument odHi = makeRgbDoc(128, 128, 0);
    TileStore& hi = *odHi.document.layers[0].rgbTiles;
    fillRect(hi, 60, 60, 70, 70, {4.0f, 4.0f, 4.0f, 1.0f});
    const std::array<float, 4> hiBefore = readAt(hi, 64, 64);
    TonalStroke shi;
    shi.begin(1.0f, TonalDirection::Dodge);
    const DepositCount hiCount =
        shi.toneDab(hi, discTip(6.0f, 1.0f), Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);
    shi.end();
    check(hiCount.texels == 0 && readAt(hi, 64, 64) == hiBefore,
          "space: a texel at linear 4.0 (past display white) is BIT-IDENTICAL after a "
          "full-strength dodge and is not even written -- the highlight headroom of every "
          "stroke that crosses a specular survives");

    // The consequence that separates this choice from a linear-light multiply:
    // the same stroke is a different exposure change at different tones. A
    // multiply would give one ratio everywhere.
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const float midLinear = srgbDecode(0.5f);
    const float quarterLinear = srgbDecode(0.25f);
    fillRect(store, 120, 120, 127, 136, {midLinear, midLinear, midLinear, 1.0f});
    fillRect(store, 128, 120, 136, 136, {quarterLinear, quarterLinear, quarterLinear, 1.0f});
    const float midBefore = readAt(store, 124, 128)[0];
    const float quarterBefore = readAt(store, 132, 128)[0];

    TonalStroke stroke;
    stroke.begin(1.0f, TonalDirection::Dodge);
    stroke.toneDab(store, discTip(20.0f, 1.0f), Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    stroke.end();
    const float midAfter = readAt(store, 124, 128)[0];
    const float quarterAfter = readAt(store, 132, 128)[0];
    const float midRatio = midAfter / midBefore;
    const float quarterRatio = quarterAfter / quarterBefore;
    // The same two numbers computed from the header's own formula, so the
    // assertion names the operator rather than a measurement of itself.
    const float wantMid = srgbDecode(std::pow(0.5f, 0.5f)) / srgbDecode(0.5f);
    const float wantQuarter = srgbDecode(std::pow(0.25f, 0.5f)) / srgbDecode(0.25f);
    std::printf("  [measured] one full-strength dodge is a %.4fx linear multiply at display "
                "0.50 and a %.4fx one at display 0.25 (formula: %.4f and %.4f)\n",
                static_cast<double>(midRatio), static_cast<double>(quarterRatio),
                static_cast<double>(wantMid), static_cast<double>(wantQuarter));
    check(nearRel(midRatio, wantMid, 4.0f * kHalfRel) &&
              nearRel(quarterRatio, wantQuarter, 4.0f * kHalfRel),
          "space: the linear-light effect of one stroke is exactly srgbDecode(d^g)/"
          "srgbDecode(d) at both tones -- the curve is applied to the DISPLAY-referred value "
          "and the texel is decoded and re-encoded around it");
    check(quarterRatio > midRatio * 1.5f,
          "space: and the quarter tone is opened nearly twice as far as the midtone -- a "
          "linear-light MULTIPLY would give one ratio everywhere and would push the "
          "highlights past 1.0, which is the alternative the header rejects");

    // The direction, on the same fixture, because a sign error is the one bug
    // in this module a user would report as "the tools are swapped".
    OpenDocument odB = makeRgbDoc(256, 256, 0);
    TileStore& storeB = *odB.document.layers[0].rgbTiles;
    fillRect(storeB, 120, 120, 136, 136, {midLinear, midLinear, midLinear, 1.0f});
    TonalStroke burn;
    burn.begin(1.0f, TonalDirection::Burn);
    burn.toneDab(storeB, discTip(20.0f, 1.0f), Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    burn.end();
    const std::array<float, 4> burned = readAt(storeB, 128, 128);
    std::printf("  [measured] display 0.5 -> dodge %.6f, burn %.6f (formula: 0.707107 and "
                "0.250000)\n",
                static_cast<double>(srgbEncode(midAfter)),
                static_cast<double>(displayOf(burned, 0)));
    check(nearRel(srgbEncode(midAfter), 0.70710678f, 4.0f * kHalfRel) &&
              nearRel(displayOf(burned, 0), 0.25f, 4.0f * kHalfRel),
          "space: at full strength a display midtone goes to 0.7071 under Dodge and to "
          "0.2500 under Burn -- one stop of gamma each way, and the two tools are not "
          "swapped");
    check(std::string(tonalDirectionName(TonalDirection::Dodge)) == "dodge" &&
              std::string(tonalDirectionName(TonalDirection::Burn)) == "burn",
          "space: and the direction has a name of its own, so a route that prints one "
          "cannot print the other's");
  }

  std::printf("  -- 3. A stroke that crosses itself applies its shift ONCE --\n");
  // ======================================================================
  // 3. The per-stroke ceiling (brush/TonalBrush section 3)
  // ======================================================================
  //
  // The headline, and the brief's own requirement: a texel covered by two
  // overlapping dabs of one stroke receives the stroke's shift once, not twice.
  // The comparison is against a one-dab stroke over the same texel, at ZERO
  // tolerance -- with flow 1 the first dab reaches the cap, so the second writes
  // nothing at all and the two strokes produce the same binary16 words rather
  // than merely similar ones.
  {
    const float midLinear = srgbDecode(0.5f);
    const std::array<float, 4> paint{midLinear, midLinear * 0.5f, midLinear * 0.25f, 1.0f};

    OpenDocument odOne = makeRgbDoc(256, 256, 0);
    OpenDocument odTwo = makeRgbDoc(256, 256, 0);
    TileStore& one = *odOne.document.layers[0].rgbTiles;
    TileStore& two = *odTwo.document.layers[0].rgbTiles;
    fillRect(one, 100, 100, 160, 160, paint);
    fillRect(two, 100, 100, 160, 160, paint);
    const std::array<float, 4> before = readAt(one, 128, 128);

    const BrushTip t = discTip(20.0f, 1.0f);
    TonalStroke s1;
    s1.begin(0.6f, TonalDirection::Dodge);
    s1.toneDab(one, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    s1.end();

    TonalStroke s2;
    s2.begin(0.6f, TonalDirection::Dodge);
    s2.toneDab(two, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    const DepositCount second =
        s2.toneDab(two, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    s2.end();

    bool identical = true;
    for (int32_t x = 110; x <= 146; ++x)
      if (readAt(one, x, 128) != readAt(two, x, 128)) identical = false;
    std::printf("  [measured] one dab vs two overlapping dabs of one stroke: identical over "
                "the row = %s; the second dab wrote %zu texels and %zu tiles\n",
                identical ? "yes" : "no", second.texels, second.tiles);
    check(identical,
          "once: a two-dab overlapping stroke and a ONE-dab stroke leave BIT-IDENTICAL "
          "texels across the whole dab, at zero tolerance -- a stroke's shift belongs to the "
          "stroke, not to how many times it crossed itself");
    check(second.texels == 0 && second.tiles == 0,
          "once: and the second dab wrote nothing and reported no tile -- at the ceiling the "
          "module stops dirtying the tile it is scrubbing over, so live feedback stops "
          "re-uploading it");
    check(readAt(one, 128, 128)[0] != before[0],
          "once: checked against a stroke that actually shifted something -- two untouched "
          "layers are also bit-identical");

    // The closed form, and the rejected per-dab model computed on the identical
    // inputs beside it. This is the assertion that cannot pass against the model
    // this build rejected: `d^(kFullGamma^(-N*flow*cov*strength))` is unbounded
    // in N and goes to white.
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    constexpr float kStrength = 0.5f;
    constexpr int kDabs = 50;
    const BrushTip slow = discTip(20.0f, 0.35f);
    fillRect(store, 110, 120, 145, 136, {midLinear, midLinear, midLinear, 1.0f});
    const float d0 = displayOf(readAt(store, 128, 128), 0);

    TonalStroke scrub;
    scrub.begin(kStrength, TonalDirection::Dodge);
    size_t writingDabs = 0;
    float perDabExponent = 1.0f;  // the rejected model's running exponent
    for (int i = 0; i < kDabs; ++i) {
      const DepositCount c =
          scrub.toneDab(store, slow, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
      if (c.texels > 0) ++writingDabs;
      perDabExponent *= std::exp2(-slow.flow * kStrength);
    }
    const float appliedTone = scrub.strokeToneAt(PixelCoord{128, 128});
    const float wantDisplay = std::pow(d0, scrub.ceilingGamma());
    const float gotDisplay = displayOf(readAt(store, 128, 128), 0);
    const float badDisplay = std::pow(d0, perDabExponent);
    const float bound = 2.0f * static_cast<float>(writingDabs) * kHalfRel;
    std::printf("  [measured] %d overlapping dabs at strength %.2f: tone accumulated %.9f, "
                "display %.6f -> %.6f (closed form %.6f); only %zu dabs wrote\n",
                kDabs, static_cast<double>(kStrength), static_cast<double>(appliedTone),
                static_cast<double>(d0), static_cast<double>(gotDisplay),
                static_cast<double>(wantDisplay), writingDabs);
    std::printf("  [measured] the REJECTED per-dab model on the identical inputs reaches "
                "display %.6f -- white\n",
                static_cast<double>(badDisplay));
    check(appliedTone == kStrength,
          "ceiling: 50 overlapping dabs accumulate EXACTLY the strength and no further, at "
          "zero tolerance -- the accumulator is the memory, so the ceiling is exact rather "
          "than approached");
    check(writingDabs > 0 && writingDabs < 5,
          "ceiling: and only a handful of those 50 dabs wrote anything -- once the ceiling is "
          "reached the rest are free, which is what stops a scrubbed stroke re-uploading a "
          "tile it is not changing");
    check(nearRel(gotDisplay, wantDisplay, bound),
          "ceiling: the stored texel matches the CLOSED FORM d0^(kFullGamma^-strength) "
          "within one binary16 rounding per writing dab, amplified by the gamma -- the "
          "exponent composes exactly, so the stroke's total does not depend on its dab count");
    check(badDisplay > 0.99f && !nearRel(badDisplay, wantDisplay, bound),
          "ceiling: and the rejected per-dab model is computed on the identical inputs and "
          "asserted WRONG -- it drives the midtone to 0.998, so the assertion above cannot "
          "pass against it");

    // The other side of the same claim: the ceiling is per STROKE. An
    // accumulator that survived pen-up would be a tool that stopped working
    // after one drag.
    TonalStroke again;
    again.begin(kStrength, TonalDirection::Dodge);
    for (int i = 0; i < 10; ++i)
      again.toneDab(store, slow, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    again.end();
    const float twoPasses = displayOf(readAt(store, 128, 128), 0);
    const float wantTwo = std::pow(d0, again.ceilingGamma() * again.ceilingGamma());
    std::printf("  [measured] a SECOND strength-0.5 dodge over the same texel: display "
                "%.6f (two ceilings composed: %.6f)\n",
                static_cast<double>(twoPasses), static_cast<double>(wantTwo));
    check(nearRel(twoPasses, wantTwo, 8.0f * kHalfRel),
          "per-stroke: a second pass composes a second ceiling onto the first -- the "
          "accumulator is thrown away at pen-up, so a painter can still go further; one that "
          "survived would be a layer lock, not a strength");
  }

  std::printf("  -- 4. Strength 0 is the identity, and the two directions are inverses --\n");
  // ======================================================================
  // 4. The two boundary cases the exponent formulation buys
  // ======================================================================
  //
  // Both are properties of `kFullGamma^(±T)` rather than of `1 ± T`, and both
  // are gestures a painter actually makes: setting the slider to zero, and
  // burning back a dodge that went too far.
  {
    const float midLinear = srgbDecode(0.5f);
    OpenDocument od = makeRgbDoc(128, 128, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 50, 50, 80, 80, {midLinear, midLinear * 0.4f, midLinear * 0.9f, 1.0f});
    const std::array<float, 4> before = readAt(store, 64, 64);

    TonalStroke zero;
    zero.begin(0.0f, TonalDirection::Dodge);
    const DepositCount zc =
        zero.toneDab(store, discTip(12.0f, 1.0f), Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);
    zero.end();
    check(zc.texels == 0 && zc.tiles == 0 && readAt(store, 64, 64) == before,
          "strength 0: an exact identity -- nothing written, no tile reported, texel "
          "bit-identical; the exponent is kFullGamma^0 and that is exactly 1");

    TonalStroke up;
    up.begin(0.4f, TonalDirection::Dodge);
    up.toneDab(store, discTip(12.0f, 1.0f), Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);
    up.end();
    const std::array<float, 4> dodged = readAt(store, 64, 64);
    TonalStroke down;
    down.begin(0.4f, TonalDirection::Burn);
    down.toneDab(store, discTip(12.0f, 1.0f), Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);
    down.end();
    const std::array<float, 4> back = readAt(store, 64, 64);

    std::printf("  [measured] display %.6f -> dodge 0.4 -> %.6f -> burn 0.4 -> %.6f; the two "
                "ceiling gammas multiply to %.9f\n",
                static_cast<double>(displayOf(before, 0)), static_cast<double>(displayOf(dodged, 0)),
                static_cast<double>(displayOf(back, 0)),
                static_cast<double>(up.ceilingGamma() * down.ceilingGamma()));
    check(nearRel(up.ceilingGamma() * down.ceilingGamma(), 1.0f, 1.0e-6f),
          "inverse: the ceiling gammas of a Dodge and a Burn at equal strength multiply to 1 "
          "-- the exponents are +T and -T of one constant, which is what makes 'burn it back' "
          "a real inverse rather than an approximation");
    bool restored = true;
    for (int ch = 0; ch < 3; ++ch)
      if (!nearRel(back[ch], before[ch], 8.0f * kHalfRel)) restored = false;
    check(restored && displayOf(dodged, 0) > displayOf(before, 0) + 0.02f,
          "inverse: and burning a dodged texel back at the same strength restores it to "
          "within the two binary16 roundings it took to get there -- checked against a dodge "
          "that visibly moved it first");
  }

  std::printf("  -- 5. The active selection bounds the shift, twice (PRD E1) --\n");
  // ======================================================================
  // 5. The selection (brush/TonalBrush section 4)
  // ======================================================================
  //
  // Both halves: the selection scales what one dab shifts, AND it caps what any
  // number of dabs can shift. The second is what makes it a bound rather than a
  // speed limit -- with the first alone a half-selected texel would dodge all
  // the way and merely take longer, so a feathered edge would come out hard for
  // a slow stroke and soft for a fast one.
  {
    const float midLinear = srgbDecode(0.5f);
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 255, 200, {midLinear, midLinear, midLinear, 1.0f});
    // A fractional left edge, so one column of texels is PARTIALLY selected --
    // an in-or-out boundary would pass against a version that treated the
    // selection as a bitmask.
    Selection sel = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    const float partial = selectionCoverageAt(&sel, PixelCoord{64, 64});
    const std::array<float, 4> outsideBefore = readAt(store, 50, 64);
    const float d0 = displayOf(readAt(store, 64, 64), 0);

    const BrushTip t = discTip(30.0f, 1.0f);
    TonalStroke stroke;
    stroke.begin(1.0f, TonalDirection::Dodge);
    stroke.toneDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    const float onePass = displayOf(readAt(store, 64, 64), 0);

    check(partial > 0.0f && partial < 1.0f,
          "selection: the boundary column really is PARTIALLY covered -- the premise the "
          "assertions below rest on, or they would pass against a bitmask selection");
    check(readAt(store, 50, 64) == outsideBefore && readAt(store, 60, 64) == outsideBefore,
          "selection: a texel outside the ants is BIT-IDENTICAL afterwards, at zero tolerance "
          "-- what a runaway tonal brush changes outside a selection is invisible until the "
          "layer under it is, and one undo step covers the whole stroke");
    check(nearRel(onePass, std::pow(d0, std::exp2(-partial)), 4.0f * kHalfRel),
          "selection: one full-flow dab through a PARTIALLY selected texel applies exactly "
          "its coverage of the shift -- so a feathered selection edge dodges feathered "
          "rather than stepped");

    for (int i = 0; i < 40; ++i)
      stroke.toneDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    const float scrubbed = stroke.strokeToneAt(PixelCoord{64, 64});
    std::printf("  [measured] selection coverage %.6f: one dab applies %.6f, 41 dabs apply "
                "%.6f (an unbounded model would apply 1.0)\n",
                static_cast<double>(partial), static_cast<double>(partial),
                static_cast<double>(scrubbed));
    check(scrubbed == partial,
          "selection: 41 dabs through a partially selected texel still apply exactly its "
          "coverage and no more -- gating only the flow makes a bound a scrubbing stroke "
          "walks straight through, which is the same speed dependence in another costume");
    check(nearRel(displayOf(readAt(store, 90, 64), 0), std::pow(d0, 0.5f), 8.0f * kHalfRel),
          "selection: and a fully selected texel receives the whole shift from the same "
          "stroke -- the gate costs nothing where the selection covers everything");
    stroke.end();

    // The null case, through this loop and not only through
    // selectionCoverageAt(): core/SelectionMask.hpp requires every hoisted
    // per-texel loop to repeat the "null Selection means 1.0" branch itself, and
    // warns that a perturbation inverting one copy leaves the others right.
    OpenDocument od2 = makeRgbDoc(256, 256, 0);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 0, 0, 255, 200, {midLinear, midLinear, midLinear, 1.0f});
    TonalStroke s2;
    s2.begin(1.0f, TonalDirection::Dodge);
    s2.toneDab(store2, t, Vec2{64.0f, 64.0f}, 256, 256, nullptr, nullptr);
    s2.end();
    check(nearRel(displayOf(readAt(store2, 64, 64), 0), std::pow(d0, 0.5f), 4.0f * kHalfRel) &&
              nearRel(displayOf(readAt(store2, 50, 64), 0), std::pow(d0, 0.5f), 4.0f * kHalfRel),
          "selection: a NULL selection means no restriction, asserted through this loop's own "
          "copy of that branch -- reading it as \"selects nothing\" inverts the editor");

    // An ENGAGED selection that names no tile at all is the other end of the
    // same rule, and it is the one an optimisation can silently break by
    // treating "no tile here" as "no restriction here".
    OpenDocument od3 = makeRgbDoc(256, 256, 0);
    TileStore& store3 = *od3.document.layers[0].rgbTiles;
    fillRect(store3, 0, 0, 255, 200, {midLinear, midLinear, midLinear, 1.0f});
    const std::array<float, 4> untouched = readAt(store3, 64, 64);
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    TonalStroke s3;
    s3.begin(1.0f, TonalDirection::Dodge);
    s3.toneDab(store3, t, Vec2{64.0f, 64.0f}, 256, 256, &elsewhere, nullptr);
    s3.end();
    check(readAt(store3, 64, 64) == untouched,
          "selection: a dab entirely outside an engaged selection changes NOTHING -- an "
          "absent selection tile is \"selects nothing\", the inverse of a mask's absent tile");
  }

  std::printf("  -- 6. Shifting nothing COSTS nothing (brush/TonalBrush section 5) --\n");
  // ======================================================================
  // 6. The empty and fixed-point cases
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(512, 512, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;

    BrushTip t = discTip(30.0f, 1.0f);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string e;
    check(s.begin(od, 0, t, Tool::Dodge, &e) && s.route() == StrokeRoute::TonalBrush,
          "empty: a Dodge begins normally on the blank RGB layer, and reports the tonal route");
    for (int k = 0; k < 20; ++k) s.addPoint(60.0f + 20.0f * static_cast<float>(k), 200.0f);
    s.end();

    std::printf("  [measured] a %zu-dab dodge across blank canvas: %zu texels, %zu tiles "
                "reported, %zu tiles in the layer\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                store.occupiedTileCount());
    check(s.dabCount() > 0 && store.occupiedTileCount() == 0 && s.strokeTiles().empty(),
          "empty: a stroke of dozens of dabs across unpainted canvas allocates NOT ONE tile "
          "and reports none -- 224 KiB per tile crossed, plus a dirty tile per frame, is what "
          "the absent-tile skip is worth");
    check(s.texelsWritten() == 0 && od.history.entries().size() == entries &&
              od.revision == rev,
          "empty: and it records NO history entry and moves no revision -- an undo step that "
          "undoes nothing is worse than a missing one, and here it arrives from the "
          "arithmetic rather than from a special case");

    // The fixed points, and the malformed texel. All three reach the module as
    // "computed a value equal to the one already there", which is one skip rule
    // and not three special cases.
    TileStore fixed;
    const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> white{1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<float, 4> malformed{0.4f, 0.0f, 0.0f, 0.0f};
    for (int32_t x = 8; x <= 10; ++x) {
      const PixelCoord p{x, 10};
      fixed.getOrCreate(tileCoordAt(p))
          .writePixel(tileLocalOffset(p), x == 8 ? black : (x == 9 ? white : malformed));
    }
    // Read back BEFORE the dab rather than comparing against the literals: 0.4f
    // is not representable in binary16, so the malformed texel's stored red is
    // 0.40015 and an assertion written against the literal would fail for a
    // reason that has nothing to do with this module.
    const std::array<float, 4> malformedStored = readAt(fixed, 10, 10);
    TonalStroke probe;
    probe.begin(1.0f, TonalDirection::Dodge);
    const DepositCount fc =
        probe.toneDab(fixed, discTip(6.0f, 1.0f), Vec2{9.5f, 10.5f}, 64, 64, nullptr, nullptr);
    probe.end();
    check(malformedStored[0] > 0.0f && malformedStored[3] == 0.0f,
          "malformed: the fixture really does hold colour at alpha 0 -- the premise the "
          "assertion below rests on, and the texel core/Composite reads as an additive glow");
    check(fc.texels == 0 && readAt(fixed, 8, 10) == black && readAt(fixed, 9, 10) == white,
          "fixed points: pure black and pure white are unmoved by any power law, so the dab "
          "writes NOTHING over them -- the skip is a comparison of the computed texel against "
          "the stored one, so a future curve inherits it");
    check(readAt(fixed, 10, 10) == malformedStored,
          "malformed: a texel holding colour at alpha 0 is LEFT ALONE, which is the deliberate "
          "inverse of brush/RgbErase section 4 -- the eraser removes what is there, a tonal op "
          "shifts a colour, and rgb/0 is not one");
  }

  std::printf("  -- 7. The routing table's Dodge and Burn rows --\n");
  // ======================================================================
  // 7. Routing (app/StrokeSession section 1)
  // ======================================================================
  //
  // Every one of these rows used to be `None` for the trivial reason that both
  // tools sat in the not-built list -- so they did nothing, said nothing, and
  // the options bar's route indicator read a grey "-> none" that was accurate
  // and useless.
  {
    Layer rgbLayer = makeRgbLayer("r");
    Layer pigment = makePigmentLayer("p");
    Layer lockedRgb = makeRgbLayer("lr");
    lockedRgb.locked = true;
    Layer hiddenRgb = makeRgbLayer("hr");
    hiddenRgb.visible = false;
    Layer alphaLockedRgb = makeRgbLayer("alr");
    alphaLockedRgb.alphaLocked = true;
    Layer storelessRgb = makeRgbLayer("sr");
    storelessRgb.rgbTiles.reset();
    Layer adjustment = makeAdjustmentLayer("adj");
    Layer media = makeRgbLayer("m");
    media.kind = LayerKind::Media;
    media.rgbTiles.reset();

    check(strokeRouteFor(Tool::Dodge, &rgbLayer) == StrokeRoute::TonalBrush &&
              strokeRouteFor(Tool::Burn, &rgbLayer) == StrokeRoute::TonalBrush,
          "routing: BOTH tonal tools on a writable RGB layer -> the one tonal route; these "
          "rows used to say None, which is why neither tool did anything at all");
    check(strokeRouteFor(Tool::Dodge, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Burn, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer REFUSES both, and this row is a decision -- its texels "
          "hold a Latent premultiplied by mass, not a display-referred colour, and the brush "
          "on the same layer still deposits, so the refusal is about the tool");
    check(strokeRouteFor(Tool::Dodge, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Burn, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: no layer at all is None for both tonal tools and PaintSim for the brush "
          "-- the solver has no tonal step, so a Dodge sent there would deposit the loaded "
          "pigment instead");
    check(strokeRouteFor(Tool::Dodge, &lockedRgb) == StrokeRoute::None,
          "routing: a locked RGB layer refuses, exactly as it refuses the brush -- the lock "
          "is checked before the kind, so the message names the one thing a user can fix");
    check(strokeRouteFor(Tool::Dodge, &hiddenRgb) == StrokeRoute::TonalBrush,
          "routing: a HIDDEN RGB layer still takes the stroke -- visibility is a view "
          "decision, the same answer the deposit and erase rows give");
    check(strokeRouteFor(Tool::Dodge, &alphaLockedRgb) == StrokeRoute::TonalBrush &&
              strokeRouteFor(Tool::Eraser, &alphaLockedRgb) == StrokeRoute::None,
          "routing: an ALPHA-LOCKED layer takes the tonal stroke and still refuses the "
          "eraser -- the tonal route copies alpha rather than moving it, so the lock is "
          "satisfied by construction; the eraser exists to move exactly that quantity");
    check(strokeRouteFor(Tool::Dodge, &storelessRgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Dodge, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Burn, &media) == StrokeRoute::None,
          "routing: an RGB layer with no store, an Adjustment layer and a Media layer each "
          "refuse -- none of them has a writable RGB store, and each says so instead of "
          "silently doing nothing");
    check(std::string(strokeRouteName(StrokeRoute::TonalBrush)) == "tonal-brush" &&
              strokeRouteWritesLayer(StrokeRoute::TonalBrush) &&
              !strokeRouteWritesLayer(StrokeRoute::PaintSim) &&
              !strokeRouteWritesLayer(StrokeRoute::None),
          "routing: the new route has a name of its own and answers the one predicate four "
          "call sites ask -- a route that neither adds nor removes paint still writes a "
          "layer, so the options bar accents it rather than greying it as the solver's");
    check(std::string(strokeEditLabel(Tool::Dodge)) == "dodge" &&
              std::string(strokeEditLabel(Tool::Burn)) == "burn" &&
              std::string(strokeEditLabel(Tool::Eraser)) == "erase",
          "routing: ONE route but TWO history labels -- the panel is scanned to find an edit "
          "to undo, and a lightening pass and the darkening pass made to correct it are "
          "opposite edits");
    check(grainReachesRoute(StrokeRoute::TonalBrush) &&
              !wetnessReachesSolver(StrokeRoute::TonalBrush),
          "routing: PAPER GRAIN reaches the tonal route (it computes a CPU coverage and calls "
          "grainCoverageAt()) and WET does not -- the predicate pair the BRUSH panel greys "
          "its two groups on, answered for the new route rather than inherited");
    check(toolImplemented(Tool::Dodge) && toolImplemented(Tool::Burn) &&
              toolHasCanvasHandler(Tool::Dodge) && toolHasCanvasHandler(Tool::Burn),
          "routing: both palette cells are now implemented AND have a canvas handler -- the "
          "flag and the handler land together, which is what --selftest's tool-table "
          "assertion forces");
  }

  std::printf("  -- 8. End to end through app/StrokeSession --\n");
  // ======================================================================
  // 8. The stroke lifecycle
  // ======================================================================
  {
    const float midLinear = srgbDecode(0.5f);
    OpenDocument od = makeRgbDoc(256, 256, 1);
    TileStore& target = *od.document.layers[1].rgbTiles;
    TileStore& other = *od.document.layers[0].rgbTiles;
    for (TileStore* s : {&target, &other})
      fillRect(*s, 40, 40, 200, 200, {midLinear, midLinear, midLinear, 1.0f});
    const std::array<float, 4> otherBefore = readAt(other, 100, 100);
    const float d0 = displayOf(readAt(target, 100, 100), 0);
    const size_t entries = od.history.entries().size();
    const uint64_t structural = od.structuralRevision;

    BrushTip t = discTip(14.0f, 1.0f);
    t.opacity = 0.5f;
    StrokeSession s;
    std::string err;
    check(s.begin(od, 1, t, Tool::Burn, &err) && s.route() == StrokeRoute::TonalBrush &&
              err.empty(),
          "session: a Burn on layer 1 begins, reports the tonal route and refuses nothing");
    for (int k = 0; k < 12; ++k) s.addPoint(60.0f + 8.0f * static_cast<float>(k), 100.0f);
    s.end();

    const float after = displayOf(readAt(target, 100, 100), 0);
    std::printf("  [measured] a 12-point Burn at opacity 0.5: display %.6f -> %.6f "
                "(closed form %.6f); %zu texels, %zu tiles, %zu history entries added\n",
                static_cast<double>(d0), static_cast<double>(after),
                static_cast<double>(std::pow(d0, std::exp2(0.5f))), s.texelsWritten(),
                s.strokeTiles().size(), od.history.entries().size() - entries);
    check(nearRel(after, std::pow(d0, std::exp2(0.5f)), 8.0f * kHalfRel),
          "session: the stroke lands the closed form of its OPACITY -- the same slider the "
          "deposit reads as a ceiling and the eraser as a floor, latched once at pen-down");
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "burn" &&
              od.structuralRevision == structural,
          "session: exactly ONE history entry for the whole stroke, labelled \"burn\", and "
          "the structural revision is untouched -- a content edit, ADR-0008");
    bool otherUntouched = true;
    for (int32_t x = 50; x <= 150; ++x)
      if (readAt(other, x, 100) != otherBefore) otherUntouched = false;
    check(otherUntouched,
          "session: the layer that was NOT the target is bit-identical across the whole "
          "stroke path, at zero tolerance -- the tonal shift reaches the active layer and no "
          "other");

    // The refusals, by name. Two of them, and they must read differently: a
    // locked layer is a switch in LAYERS, and a Pigment layer is not something
    // clearing a lock will help with.
    OpenDocument odR = makeRgbDoc(128, 128, 0);
    odR.document.layers[0].locked = true;
    StrokeSession sr;
    std::string lockedWhy;
    const bool lockedBegan = sr.begin(odR, 0, t, Tool::Dodge, &lockedWhy);
    recordLayerEdit(odR, addLayer(odR.document, odR.document.layers.size(),
                                  makePigmentLayer("pig")));
    StrokeSession sp;
    std::string pigmentWhy;
    const bool pigmentBegan = sp.begin(odR, 1, t, Tool::Dodge, &pigmentWhy);
    std::printf("  [measured] locked: \"%s\"\n  [measured] pigment: \"%s\"\n",
                lockedWhy.c_str(), pigmentWhy.c_str());
    check(!lockedBegan && !pigmentBegan && contains(lockedWhy, "locked") &&
              contains(lockedWhy, "dodge") && contains(pigmentWhy, "Pigment") &&
              contains(pigmentWhy, "dodge"),
          "session: both refusals name the TOOL and the reason -- a stroke that reaches no "
          "layer and says nothing is the defect these two tools shipped with for nineteen "
          "revisions");
    check(lockedWhy != pigmentWhy,
          "session: and the two sentences DIFFER -- both present to a user as \"dodge did "
          "nothing\", and only one of them has a switch in LAYERS that fixes it");
  }

  std::printf("[selftest] tonal brush %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
