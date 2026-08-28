#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "core/SelectionShapes.hpp"
#include "paint/Palette.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Painting on a plain RGB layer (brush/RgbDeposit), and the routing fix that
// made it reachable (app/StrokeSession section 1).
//
// See app/SelfTest.hpp for the section's own contents list, and the two headers
// for every decision this file only checks. Two things are worth saying here
// because they are what the section is *for*:
//
//   * Before this, selecting an RGB layer and dragging the brush deposited on
//     the **solver canvas**. Paint appeared -- in roughly the right place, in
//     roughly the right colour -- on something that was not the layer the user
//     had selected, and that nothing saves, composites or undoes. It was a
//     wrong-target bug wearing a missing-feature costume, and section 9 is the
//     assertion that would have caught it.
//   * The flow/opacity model (section 4) is the one piece of arithmetic here
//     that can be *plausibly* wrong: a per-dab opacity paints, and looks like
//     paint, and is only detectable by comparing two strokes that differ in how
//     much they overlap themselves. Section 4 runs both models side by side and
//     prints the number the wrong one produces.
// ---------------------------------------------------------------------------
bool runRgbDepositTest() {
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
  //    absolute floor of half a subnormal ulp, 2^-25 = 2.980e-08. The identical
  //    derivation runPigmentDepositTest() and runPigmentLayerTest() each state
  //    for the identical storage, restated rather than shared because a
  //    tolerance borrowed without its derivation is the one that later gets
  //    applied where it does not hold.
  //
  //  * **kAccumTol for a stroke of N writes** -- the layer rounds to binary16
  //    once per dab that writes, while brush/RgbDeposit's accumulator does not
  //    (its §3). So the stored alpha after N *writing* dabs can drift from the
  //    accumulator by at most `N * kHalfRel * A`: each write contributes one
  //    rounding of at most `kHalfRel` relative, and every earlier error is
  //    damped by the factor `(1 - a)` of every later dab, which is at most 1.
  //    Computed per test from the N that test actually spends, and the measured
  //    value printed beside it -- it comes out far under the bound, because the
  //    damping is real and because the cap turns most dabs into no-ops.
  //
  //  * Several assertions below are at **exactly zero** tolerance, and each
  //    says why in place. They are claims about which operations happen -- that
  //    a capped dab writes nothing at all, that two strokes emit the identical
  //    dabs -- rather than claims about accuracy.
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

  // A hard disc, so `dabCoverage()` is exactly 1.0f over the whole core and
  // every number below is about the deposit rather than about the falloff.
  // Section 3 is where the falloff itself is checked.
  auto discTip = [](float radius, float flow, const std::array<float, 3>& rgb) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.linearRgb = rgb;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h, size_t extraRgbLayers) {
    // `Document::createBlank()` already makes layer 0 an RGB layer with tiles
    // -- which is the whole reason this module matters: an ordinary File > New
    // followed by a brush stroke lands here, not on a Pigment layer.
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "rgb deposit");
    for (size_t i = 0; i < extraRgbLayers; ++i)
      recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                   makeRgbLayer("RGB " + std::to_string(i + 1))));
    return od;
  };

  // ======================================================================
  // 1. Premultiplied, established rather than assumed
  // ======================================================================
  //
  // The compositing arithmetic differs between premultiplied and straight
  // alpha, and getting it backwards produces a picture that is *almost* right
  // -- correct where alpha is 1, too bright at every soft edge. So this is
  // checked in the one way that distinguishes them, and checked against the
  // reader as well as against the writer: `core/Composite` must agree.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const BrushTip t = discTip(20.0f, 0.5f, {1.0f, 1.0f, 1.0f});
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 0.5f);
    stroke.depositDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> texel = readAt(store, 128, 128);
    std::printf("  [measured] white ink at alpha %.4f stores rgb %.6f "
                "(premultiplied) -- straight would store %.6f\n",
                static_cast<double>(texel[3]), static_cast<double>(texel[0]), 1.0);
    check(texel[3] == 0.5f && texel[0] == 0.5f && texel[1] == 0.5f && texel[2] == 0.5f,
          "premultiplied: WHITE ink at half alpha stores 0.5, not 1.0 -- core::Tile is "
          "associated alpha, and storing the straight colour would composite every soft "
          "edge at twice its brightness");

    // The other half of the claim: the *reader* takes it the same way. A single
    // layer at opacity 1 over nothing must composite to the stored bytes with
    // no conversion at all, which is only true if both ends agree.
    const std::vector<float> comp = compositeDocumentPremultiplied(od.document);
    const size_t i = (static_cast<size_t>(128) * 256 + 128) * 4;
    check(comp[i] == texel[0] && comp[i + 1] == texel[1] && comp[i + 2] == texel[2] &&
              comp[i + 3] == texel[3],
          "premultiplied: core/Composite reads the deposited texel through UNCHANGED, at "
          "zero tolerance -- writer and reader hold the same convention, not two that "
          "happen to look alike at alpha 1");
  }

  // ======================================================================
  // 2. Linear, and the same colour the bucket and the gradient fill with
  // ======================================================================
  //
  // The palette is display-referred sRGB; a document part is scene-referred
  // linear. Skip the decode and every stroke lands at roughly half the
  // brightness of the swatch that was clicked, which reads as a
  // colour-management bug somewhere else entirely.
  //
  // `brushTipFor()` cannot call `ui/MacPaintUI`'s `foregroundLinearRgba()` --
  // `app/` must not include `ui/` -- so the decode is written twice, and this
  // is the guard that spelling it twice needs.
  {
    MixboxLut noLut;  // no file is read anywhere in this section
    const std::vector<Pigment>& palette = defaultPalette();
    bool matchesUi = true;
    bool anyDiffered = false;
    for (size_t i = 0; i < palette.size(); ++i) {
      BrushState brush;
      brush.pigment = static_cast<int>(i);
      const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
      const std::array<float, 4> ui = foregroundLinearRgba(static_cast<int>(i));
      for (int c = 0; c < 3; ++c) {
        if (tip.linearRgb[c] != ui[c]) matchesUi = false;
        // sRGB and linear agree only at 0 and 1, so a palette of real colours
        // must differ somewhere -- otherwise this would pass just as happily
        // against a version that skipped the decode entirely.
        if (std::fabs(palette[i].rgb[c] - tip.linearRgb[c]) > 1e-3f) anyDiffered = true;
      }
    }
    check(matchesUi && anyDiffered,
          "colour: the brush tip's linear RGB is bit-identical to foregroundLinearRgba() "
          "for every palette entry, and the palette is checked to really differ from its "
          "own decode so this cannot pass on a missing conversion");

    BrushState brush;
    brush.pigment = 6;  // Ultramarine Blue, BrushState's own default
    const BrushTip tip = brushTipFor(brush, noLut, 1.0f);
    std::printf("  [measured] pigment 6 swatch sRGB (%.4f %.4f %.4f) -> linear "
                "(%.4f %.4f %.4f)\n",
                static_cast<double>(palette[6].rgb[0]), static_cast<double>(palette[6].rgb[1]),
                static_cast<double>(palette[6].rgb[2]), static_cast<double>(tip.linearRgb[0]),
                static_cast<double>(tip.linearRgb[1]), static_cast<double>(tip.linearRgb[2]));

    // And that colour reaches the layer: an opaque stroke's un-premultiplied
    // colour is the ink, not something the compositing stages perturbed.
    OpenDocument od = makeRgbDoc(128, 128, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    BrushTip solid = tip;
    solid.hardness = 1.0f;
    solid.flow = 1.0f;
    solid.opacity = 1.0f;
    RgbStroke stroke;
    stroke.begin(solid.linearRgb, solid.opacity);
    stroke.depositDab(store, solid, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    stroke.end();
    const std::array<float, 4> texel = readAt(store, 64, 64);
    check(texel[3] == 1.0f && nearHalf(texel[0], solid.linearRgb[0]) &&
              nearHalf(texel[1], solid.linearRgb[1]) && nearHalf(texel[2], solid.linearRgb[2]),
          "colour: a fully opaque dab stores the tip's LINEAR colour to within f16 -- not "
          "the sRGB triple, which would land the stroke at roughly half the swatch");
  }

  // ======================================================================
  // 3. One dab: the stored alpha profile IS the coverage profile
  // ======================================================================
  //
  // With flow 1, opacity 1 and nothing underneath, `A' = cov` exactly, so this
  // is the deposit and the falloff checked against each other. It is also the
  // check that would catch the dab being written through the wrong channel or
  // at the wrong offset, which no aggregate assertion below would notice.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    BrushTip t = discTip(10.0f, 1.0f, {1.0f, 0.0f, 0.0f});
    t.hardness = 0.0f;  // a pure smoothstep, so the profile is not a plateau
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f);
    stroke.depositDab(store, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);

    float worst = 0.0f;
    for (int32_t x = 50; x < 80; ++x) {
      const float cov = dabCoverage(t, static_cast<float>(x) + 0.5f - 64.5f, 0.0f);
      worst = std::max(worst, std::fabs(readAt(store, x, 64)[3] - cov));
    }
    std::printf("  [measured] worst |stored alpha - dabCoverage| across a dab row: %.6e "
                "(one f16 rounding bounds it at %.6e)\n",
                static_cast<double>(worst), static_cast<double>(kHalfRel));
    check(worst <= kHalfRel + kHalfFloor,
          "one dab: the stored alpha across a row IS dabCoverage(), to a single f16 "
          "rounding -- the same falloff the pigment route uses, not a second one");

    // Exactly zero at and beyond the radius, which is what makes the footprint
    // a bounded set at all (brush/Deposit.hpp §3, fact 1).
    check(readAt(store, 64 - 11, 64)[3] == 0.0f && readAt(store, 64 + 11, 64)[3] == 0.0f,
          "one dab: nothing at all is written at or beyond the radius, at zero tolerance -- "
          "the footprint is bounded, so the reported tile set can be complete");
    stroke.end();
  }

  // ======================================================================
  // 4. Opacity is a PER-STROKE ceiling, not a per-dab multiplier
  // ======================================================================
  //
  // The headline. brush/RgbDeposit.hpp §2 is the argument; this is the
  // measurement, with the rejected model computed beside it in the same loop so
  // the two numbers are directly comparable.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const BrushTip t = discTip(20.0f, 0.35f, {0.2f, 0.4f, 0.8f});
    constexpr float kCeiling = 0.5f;
    constexpr int kDabs = 50;

    RgbStroke stroke;
    stroke.begin(t.linearRgb, kCeiling);
    size_t writingDabs = 0;
    // The model this build rejected, run on the same numbers: each dab
    // composites at `flow * cov * opacity` with no memory of the ones before.
    float perDab = 0.0f;
    for (int i = 0; i < kDabs; ++i) {
      const DepositCount c =
          stroke.depositDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
      if (c.texels > 0) ++writingDabs;
      const float a = t.flow * kCeiling;
      perDab = a + perDab * (1.0f - a);
    }

    const float accumulated = stroke.strokeAlphaAt(PixelCoord{128, 128});
    const float stored = readAt(store, 128, 128)[3];
    const float bound = static_cast<float>(writingDabs) * kHalfRel * kCeiling;
    std::printf("  [measured] %d overlapping dabs at opacity %.2f: accumulator %.9f, stored "
                "%.9f (|err| %.3e, bound %.3e); only %zu of them wrote anything\n",
                kDabs, static_cast<double>(kCeiling), static_cast<double>(accumulated),
                static_cast<double>(stored), static_cast<double>(std::fabs(stored - accumulated)),
                static_cast<double>(bound), writingDabs);
    std::printf("  [measured] the REJECTED per-dab model reaches %.6f on the identical "
                "numbers -- a \"50%%\" stroke that is 100%% opaque\n",
                static_cast<double>(perDab));

    check(accumulated == kCeiling,
          "opacity cap: 50 overlapping dabs reach EXACTLY the stroke's opacity in the "
          "accumulator -- not 0.75, not 1.0; the accumulator never rounds, so this is at "
          "zero tolerance");
    check(std::fabs(stored - kCeiling) <= bound + kHalfFloor,
          "opacity cap: and the layer's stored alpha agrees, within the f16 bound derived "
          "from the number of writes -- if this said 1.0 the brush would have no opacity "
          "setting at all, only a slower flow");
    check(perDab > 0.99f,
          "opacity cap: the rejected per-dab model is *checked to be wrong* on these "
          "numbers -- otherwise the assertion above would pass against it too and prove "
          "nothing");
    check(writingDabs < 5 && writingDabs > 0,
          "opacity cap: once the ceiling is reached the remaining dabs write NOTHING -- not "
          "a value equal to what is there, nothing at all, so a scrubbed stroke stops "
          "dirtying tiles and live feedback stops re-uploading them");

    // The colour, at the cap, is still the ink -- a capped stroke must not be
    // a differently-coloured stroke.
    const std::array<float, 4> texel = readAt(store, 128, 128);
    check(nearHalf(texel[0], t.linearRgb[0] * kCeiling) &&
              nearHalf(texel[1], t.linearRgb[1] * kCeiling) &&
              nearHalf(texel[2], t.linearRgb[2] * kCeiling),
          "opacity cap: the capped texel holds the ink premultiplied by the ceiling -- the "
          "cap changes how much paint is there, never which paint");
    stroke.end();
  }

  // ======================================================================
  // 5. The ceiling holds at EVERY coverage, not only under the dab's core
  // ======================================================================
  //
  // `A' = A + w(1-A)` converges to the ceiling for any positive `w`, so a rim
  // texel at 3 % coverage reaches the same alpha as the centre -- it just takes
  // longer. That is what makes the ceiling a property of the stroke rather than
  // of the tip, and it is the reason a scrubbed stroke has a flat interior
  // instead of a bright spine.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    BrushTip t = discTip(10.0f, 0.3f, {1.0f, 0.0f, 0.0f});
    t.hardness = 0.0f;
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 0.5f);
    for (int i = 0; i < 200; ++i)
      stroke.depositDab(store, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);

    bool flat = true;
    for (const int32_t x : {64, 68, 71, 73}) {
      const float cov = dabCoverage(t, static_cast<float>(x) + 0.5f - 64.5f, 0.0f);
      const float got = stroke.strokeAlphaAt(PixelCoord{x, 64});
      std::printf("  [measured] rim texel x=%d at coverage %.4f reaches alpha %.9f\n", x,
                  static_cast<double>(cov), static_cast<double>(got));
      if (got != 0.5f) flat = false;
    }
    check(flat,
          "ceiling: a 3%-coverage rim texel reaches the SAME ceiling as the 100% centre, at "
          "zero tolerance -- the ceiling belongs to the stroke, so a scrubbed stroke has a "
          "flat interior rather than a bright spine");
    stroke.end();
  }

  // ======================================================================
  // 6. A fast stroke and a slow stroke land in the same place
  // ======================================================================
  //
  // ADR-0003: deposition depends on distance travelled, never on how many input
  // events that distance was divided into. `brush/StrokePath` already
  // guarantees the *dabs* are the same; what this checks is that nothing in the
  // deposit reintroduced a dependence on the sample count -- which a per-dab
  // opacity, a per-sample composite, or an accumulator keyed on anything but
  // position all would.
  //
  // Driven through `StrokeSession`, not through `RgbStroke` directly, because
  // the sample-to-dab path is exactly what is under test.
  {
    auto paintLine = [&](int samples) {
      OpenDocument od = makeRgbDoc(256, 256, 0);
      BrushTip t;
      t.radius = 12.0f;
      t.hardness = 0.35f;
      t.flow = 0.4f;
      t.spacing = 0.25f;
      t.linearRgb = {0.9f, 0.1f, 0.05f};
      t.opacity = 1.0f;
      StrokeSession s;
      std::string e;
      s.begin(od, 0, t, Tool::Brush, &e);
      for (int i = 0; i <= samples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(samples);
        s.addPoint(40.0f + u * 180.0f, 100.0f);
      }
      s.end();
      return std::make_pair(std::move(od), s.dabCount());
    };

    auto slow = paintLine(60);  // one sample per frame, a leisurely drag
    auto fast = paintLine(3);   // the same 180 px in three frames

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
                "worst |dAlpha| %.6e, worst |dRGB| %.6e; mid-stroke alpha %.6f\n",
                slow.second, fast.second, static_cast<double>(worstA),
                static_cast<double>(worstRgb), static_cast<double>(readAt(a, 130, 100)[3]));

    // The predicted count is the arc length over the spacing: 180 px at
    // `spacing * radius` = 0.25 * 12 = 3 px is 60 dabs, and both sample rates
    // must produce it. Asserted as a floor rather than as the literal 60 so a
    // future change to how `flush()` treats the final sub-spacing remainder
    // does not turn an equality between the two runs into a failure about one.
    check(slow.second == fast.second && slow.second >= 50,
          "speed: 61 samples and 4 samples over the same straight 180 px emit the IDENTICAL "
          "number of dabs -- ADR-0003, and the premise the next assertion rests on");
    check(worstA == 0.0f && worstRgb == 0.0f,
          "speed: and the two layers are BIT-IDENTICAL over the whole stroke, at zero "
          "tolerance -- a per-dab opacity, or any accumulation keyed on samples rather than "
          "position, would make the slow stroke the darker one");
    check(readAt(a, 130, 100)[3] > 0.5f,
          "speed: checked against a stroke that actually deposited something -- two empty "
          "layers are also bit-identical");
  }

  // ======================================================================
  // 7. The ceiling is per STROKE -- a second stroke builds on the first
  // ======================================================================
  //
  // The boundary of section 4's claim, asserted rather than left implied. If
  // the accumulator survived pen-up, a painter could never darken a pass, which
  // is as wrong in the other direction.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const BrushTip t = discTip(20.0f, 1.0f, {1.0f, 1.0f, 1.0f});
    for (int pass = 0; pass < 2; ++pass) {
      RgbStroke stroke;
      stroke.begin(t.linearRgb, 0.5f);
      for (int i = 0; i < 10; ++i)
        stroke.depositDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
      stroke.end();
    }
    const float after = readAt(store, 128, 128)[3];
    std::printf("  [measured] two separate strokes at opacity 0.50 reach %.6f (one stroke "
                "reaches 0.50; 1 - 0.5*0.5 = 0.75)\n",
                static_cast<double>(after));
    check(nearHalf(after, 0.75f),
          "per-stroke: a SECOND stroke at opacity 0.5 builds to 0.75 -- the accumulator is "
          "thrown away at pen-up, so a painter can still darken a pass; a ceiling that "
          "survived the stroke would be a layer lock, not an opacity");
  }

  // ======================================================================
  // 8. The active selection bounds the deposit (PRD E1, P0)
  // ======================================================================
  //
  // Both halves of brush/RgbDeposit.hpp §4: the selection scales what one dab
  // lays down, AND it caps what any number of dabs can reach. The second is the
  // one that makes it a bound rather than a speed limit, and it is the one that
  // was found by measurement rather than designed in -- with the first alone, a
  // half-selected texel still climbs to full opacity, just more slowly.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // A fractional left edge, so one column of texels is PARTIALLY selected --
    // an in-or-out boundary would pass against a version that treated the
    // selection as a bitmask.
    Selection sel = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    const float partial = selectionCoverageAt(&sel, PixelCoord{64, 64});

    const BrushTip t = discTip(30.0f, 1.0f, {1.0f, 1.0f, 1.0f});
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f);
    stroke.depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);

    const float onePass = readAt(store, 64, 64)[3];
    check(readAt(store, 50, 64)[3] == 0.0f && readAt(store, 60, 64)[3] == 0.0f,
          "selection: a texel outside the ants is not written AT ALL, at zero tolerance -- "
          "no tile allocated for it, nothing to distinguish it from unpainted");
    check(nearHalf(onePass, partial),
          "selection: one full-flow dab through a PARTIALLY selected texel lands exactly "
          "its coverage -- the same `coverage * opacity` weight the paint bucket uses, so "
          "an antialiased edge is antialiased rather than stepped");

    for (int i = 0; i < 40; ++i)
      stroke.depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    const float scrubbed = stroke.strokeAlphaAt(PixelCoord{64, 64});
    std::printf("  [measured] selection coverage %.6f: one dab reaches %.6f, 41 dabs reach "
                "%.6f (an unbounded model would reach 1.0)\n",
                static_cast<double>(partial), static_cast<double>(onePass),
                static_cast<double>(scrubbed));
    check(scrubbed == partial,
          "selection: 41 dabs through a partially selected texel still reach exactly its "
          "coverage and no further -- gating only the flow makes a bound a scrubbing brush "
          "walks straight through, which is the same speed dependence in another costume");
    check(readAt(store, 80, 64)[3] == 1.0f,
          "selection: and a fully selected texel is unaffected by any of it -- the gate "
          "costs nothing where the selection covers everything");
    stroke.end();

    // The null case, through this loop and not only through
    // selectionCoverageAt(): core/SelectionMask.hpp requires every hoisted
    // per-texel loop to repeat the "null Selection means 1.0" branch itself,
    // and warns that a perturbation inverting one copy leaves the others right.
    OpenDocument od2 = makeRgbDoc(256, 256, 0);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    RgbStroke stroke2;
    stroke2.begin(t.linearRgb, 1.0f);
    stroke2.depositDab(store2, t, Vec2{64.0f, 64.0f}, 256, 256, nullptr, nullptr);
    stroke2.end();
    check(readAt(store2, 64, 64)[3] == 1.0f && readAt(store2, 50, 64)[3] == 1.0f,
          "selection: a NULL selection means no restriction, asserted through this loop's "
          "own copy of that branch -- reading it as \"selects nothing\" inverts the editor");

    // An ENGAGED selection that names no tile at all is the other end of the
    // same rule, and it is the one an optimisation can silently break by
    // treating "no tile here" as "no restriction here".
    OpenDocument od3 = makeRgbDoc(256, 256, 0);
    TileStore& store3 = *od3.document.layers[0].rgbTiles;
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    RgbStroke stroke3;
    stroke3.begin(t.linearRgb, 1.0f);
    stroke3.depositDab(store3, t, Vec2{64.0f, 64.0f}, 256, 256, &elsewhere, nullptr);
    stroke3.end();
    check(store3.occupiedTileCount() == 0,
          "selection: a dab entirely outside an engaged selection allocates NO tiles -- an "
          "absent selection tile is \"selects nothing\", the inverse of a mask's absent tile");
  }

  // ======================================================================
  // 9. The routing table, in full (app/StrokeSession section 1)
  // ======================================================================
  //
  // Including the four rows whose answer this step CHANGED. Each of them used
  // to be `PaintSim`, which meant: the user picked a layer, dragged, saw paint,
  // and the paint was on the solver canvas -- not saved, not composited into
  // the document, not undoable, and gone at the next mode switch.
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

    check(strokeRouteFor(Tool::Brush, &rgbLayer) == StrokeRoute::RgbDeposit &&
              strokeRouteFor(Tool::DryBrush, &rgbLayer) == StrokeRoute::RgbDeposit,
          "routing: Brush and DryBrush on a writable RGB layer -> the RGB deposit; this row "
          "used to say PaintSim, which is the whole defect");
    check(strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer is untouched by this step -- still the CPU pigment "
          "deposit, still brush/Deposit's arithmetic");
    check(strokeRouteFor(Tool::Brush, &lockedRgb) == StrokeRoute::None,
          "routing: a locked RGB layer refuses, exactly as a locked Pigment one does -- the "
          "lock is checked before the kind, so the message a user gets names the one thing "
          "they can fix");
    check(strokeRouteFor(Tool::Brush, &hiddenRgb) == StrokeRoute::RgbDeposit,
          "routing: a HIDDEN RGB layer still deposits -- visibility is a view decision, and "
          "layerCoverage() already makes it contribute nothing (section 11)");
    check(strokeRouteFor(Tool::Brush, &storelessRgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &media) == StrokeRoute::None,
          "routing: an RGB layer with no store, an Adjustment layer and a Media layer each "
          "refuse -- a target that cannot take the stroke routes NOWHERE, and no longer "
          "silently to the solver canvas");
    check(strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::DryBrush, nullptr) == StrokeRoute::PaintSim,
          "routing: no layer at all is still PaintSim, and is the ONLY row that is -- with "
          "no document open there is nothing to have aimed at, and watercolour and oil "
          "paint the canvas texture legitimately");
    check(strokeRouteFor(Tool::Water, &rgbLayer) == StrokeRoute::PaintSim,
          "routing: Water on an RGB layer is still PaintSim -- an RGB texel has nowhere to "
          "put wetness, and the readback bridge is what would carry it into a document");
    check(strokeRouteFor(Tool::Eyedropper, &rgbLayer) == StrokeRoute::None &&
              strokeRouteFor(Tool::PaintBucket, &rgbLayer) == StrokeRoute::None,
          "routing: a non-stroke tool deposits nowhere whatever the target -- the bucket "
          "writes RGB layers, but not through a stroke");
    check(std::string(strokeRouteName(StrokeRoute::RgbDeposit)) == "rgb-deposit" &&
              strokeRouteWritesLayer(StrokeRoute::RgbDeposit) &&
              strokeRouteWritesLayer(StrokeRoute::CpuDeposit) &&
              !strokeRouteWritesLayer(StrokeRoute::PaintSim) &&
              !strokeRouteWritesLayer(StrokeRoute::None),
          "routing: the new route has a name of its own and answers the one predicate three "
          "call sites ask -- so a fourth route reaches all three of them or none");
  }

  // ======================================================================
  // 10. Paint lands on the ACTIVE layer, and on no other
  // ======================================================================
  //
  // The assertion that would have caught the original defect, and it is
  // deliberately driven end to end -- `setActiveLayer()`, `activeLayerOf()` and
  // `od.activeLayer` fed to `StrokeSession::begin()`, which is the exact chain
  // `ui/MacPaintUI`'s canvas branch runs. A version that painted the right
  // pixels into the wrong store would pass every section above this one.
  {
    OpenDocument od = makeRgbDoc(256, 256, 1);  // layers 0 and 1, both RGB
    const std::array<std::array<float, 3>, 2> inks{
        std::array<float, 3>{1.0f, 0.0f, 0.0f}, std::array<float, 3>{0.0f, 0.0f, 1.0f}};

    for (size_t i = 0; i < 2; ++i) {
      setActiveLayer(od, i);
      const Layer* target = activeLayerOf(od);
      if (target == nullptr || target->kind != LayerKind::RGB) ok = false;
      BrushTip t = discTip(24.0f, 1.0f, inks[i]);
      t.opacity = 1.0f;
      StrokeSession s;
      std::string e;
      // `od.activeLayer`, not the loop variable -- the point is that the index
      // the UI hands over is the one that gets painted.
      check(s.begin(od, od.activeLayer, t, Tool::Brush, &e) && e.empty(),
            i == 0 ? "active layer: a stroke begins on RGB layer 0"
                   : "active layer: and on RGB layer 1, with the same call");
      // The SAME position on both layers, which is what makes cross-
      // contamination detectable rather than merely unlikely.
      for (int k = 0; k < 6; ++k) s.addPoint(100.0f + 6.0f * static_cast<float>(k), 128.0f);
      s.end();
    }

    const TileStore& low = *od.document.layers[0].rgbTiles;
    const TileStore& up = *od.document.layers[1].rgbTiles;
    const std::array<float, 4> lowTexel = readAt(low, 110, 128);
    const std::array<float, 4> upTexel = readAt(up, 110, 128);
    std::printf("  [measured] at (110,128): layer 0 holds (%.4f %.4f %.4f a=%.4f), layer 1 "
                "holds (%.4f %.4f %.4f a=%.4f)\n",
                static_cast<double>(lowTexel[0]), static_cast<double>(lowTexel[1]),
                static_cast<double>(lowTexel[2]), static_cast<double>(lowTexel[3]),
                static_cast<double>(upTexel[0]), static_cast<double>(upTexel[1]),
                static_cast<double>(upTexel[2]), static_cast<double>(upTexel[3]));

    check(lowTexel[3] > 0.9f && lowTexel[0] > 0.9f && lowTexel[2] == 0.0f,
          "active layer: layer 0 holds ITS OWN red and not a trace of layer 1's blue -- the "
          "blue channel is exactly zero, at zero tolerance");
    check(upTexel[3] > 0.9f && upTexel[2] > 0.9f && upTexel[0] == 0.0f,
          "active layer: layer 1 holds its own blue and not a trace of layer 0's red -- two "
          "strokes over the identical pixels stayed in the stores they were aimed at");
    check(od.history.entries().size() >= 2,
          "active layer: and each stroke is its own history entry, so each can be undone "
          "without touching the other layer");
  }

  // ======================================================================
  // 11. Hiding a layer takes its paint with it
  // ======================================================================
  //
  // `core/Composite`'s `layerCoverage()` already returns 0 for an invisible
  // layer, so no code was needed for this -- which is exactly why it is worth
  // asserting rather than assuming. It is checked through a REAL composite of a
  // REAL painted layer and not by reading the flag back: the claim is about
  // what reaches the picture, and a `visible` flag that nothing consulted would
  // read back perfectly.
  {
    OpenDocument od = makeRgbDoc(128, 128, 0);
    BrushTip t = discTip(20.0f, 1.0f, {0.5f, 0.25f, 0.125f});
    t.opacity = 1.0f;
    StrokeSession s;
    std::string e;
    s.begin(od, 0, t, Tool::Brush, &e);
    for (int k = 0; k < 6; ++k) s.addPoint(40.0f + 8.0f * static_cast<float>(k), 64.0f);
    s.end();

    const size_t i = (static_cast<size_t>(64) * 128 + 64) * 4;
    const std::vector<float> shown = compositeDocumentPremultiplied(od.document);
    check(shown[i + 3] > 0.9f && shown[i] > 0.0f,
          "hidden layer: the painted stroke reaches the composite while the layer is "
          "visible -- the premise the next assertion needs, or it would pass on a document "
          "with nothing in it");

    od.document.layers[0].visible = false;
    const std::vector<float> hidden = compositeDocumentPremultiplied(od.document);
    bool allZero = true;
    for (const float v : hidden)
      if (v != 0.0f) allZero = false;
    std::printf("  [measured] composite at (64,64): visible a=%.6f -> hidden a=%.6f; the "
                "whole %dx%d frame is exactly zero: %s\n",
                static_cast<double>(shown[i + 3]), static_cast<double>(hidden[i + 3]), 128, 128,
                allZero ? "yes" : "no");
    check(allZero,
          "hidden layer: hiding it removes its paint from the composite ENTIRELY -- every "
          "float of the frame is exactly 0, not merely dark, because layerCoverage() makes "
          "an invisible layer a skip rather than a multiply by zero");

    // And the paint is still in the document -- hiding is a view decision, not
    // an erase. Un-hiding must give the identical bytes back.
    od.document.layers[0].visible = true;
    const std::vector<float> again = compositeDocumentPremultiplied(od.document);
    check(again == shown,
          "hidden layer: un-hiding restores the BIT-IDENTICAL composite -- the layer kept "
          "its texels throughout, which is why visibility is not a refusal at the route "
          "table either");
  }

  // ======================================================================
  // 12. Every refusal, by name
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("locked")));
    setLayerLocked(od.document, 1, true);
    recordLayerEdit(od,
                    addLayer(od.document, od.document.layers.size(), makeAdjustmentLayer("adj")));

    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;
    BrushTip t = discTip(8.0f, 0.5f, {1.0f, 1.0f, 1.0f});
    t.opacity = 1.0f;
    StrokeSession s;
    std::string error;

    check(!s.begin(od, 1, t, Tool::Brush, &error) && contains(error, "locked") &&
              contains(error, "none") && contains(error, "RGB"),
          "refusal: a LOCKED RGB layer refuses and names the layer, its kind and its route "
          "-- falling through to the solver would put paint where the user cannot see it "
          "came from and cannot undo it");
    check(!s.begin(od, 2, t, Tool::Brush, &error) && contains(error, "Adjustment"),
          "refusal: an Adjustment layer refuses by name -- it holds no tiles at all");
    check(!s.begin(od, 99, t, Tool::Brush, &error) && contains(error, "out of range"),
          "refusal: an out-of-range index refuses without touching the document");
    check(!s.begin(od, 0, t, Tool::Water, &error) && contains(error, "paint-sim"),
          "refusal: Water on an RGB layer refuses and names where that stroke DOES go");
    check(od.history.entries().size() == entries && od.revision == rev && !s.active(),
          "refusal: not one of the four refusals recorded an entry or moved the revision");

    // A stroke entirely off the canvas: legal, deposits nothing, records
    // nothing. The same rule the pigment route has, through the new branch.
    check(s.begin(od, 0, t, Tool::Brush, &error) && s.route() == StrokeRoute::RgbDeposit,
          "empty stroke: begins normally on the RGB layer, and reports the RGB route");
    for (int i = 0; i < 10; ++i) s.addPoint(-500.0f - static_cast<float>(i), -500.0f);
    s.end();
    check(s.texelsWritten() == 0 && od.history.entries().size() == entries && od.revision == rev,
          "empty stroke: a stroke that deposited nothing records NO history entry and moves "
          "no revision -- an undo step that undoes nothing is worse than a missing one");

    // Opacity 0 is a legitimate setting, not an error, and must behave as one.
    BrushTip invisible = t;
    invisible.opacity = 0.0f;
    check(s.begin(od, 0, invisible, Tool::Brush, &error), "opacity 0: begins normally");
    for (int i = 0; i < 8; ++i) s.addPoint(60.0f + 10.0f * static_cast<float>(i), 60.0f);
    s.end();
    check(s.texelsWritten() == 0 && od.history.entries().size() == entries,
          "opacity 0: a stroke at zero opacity writes nothing and records nothing -- a "
          "setting, refused by arithmetic rather than by a special case");
  }

  // ======================================================================
  // 13. One stroke is one undo step, and the accumulator does not outlive it
  // ======================================================================
  //
  // The history half is `app/StrokeSession`'s and is already asserted for the
  // pigment route; what is new is that the RGB route reaches the same code, and
  // that brush/RgbDeposit.hpp §3's memory claim ("allocated at stroke start,
  // freed at stroke end") is a claim rather than a comment.
  {
    OpenDocument od = makeRgbDoc(512, 512, 0);
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    const uint64_t structBefore = od.structuralRevision;

    BrushTip t;
    t.radius = 18.0f;
    t.hardness = 0.4f;
    t.flow = 0.2f;
    t.linearRgb = {0.1f, 0.6f, 0.3f};
    t.opacity = 0.8f;

    StrokeSession s;
    std::string e;
    s.begin(od, 0, t, Tool::Brush, &e);
    for (int i = 0; i < 40; ++i) {
      const float u = static_cast<float>(i) / 39.0f;
      s.addPoint(60.0f + 380.0f * u, 120.0f + 220.0f * std::sin(u * 3.1415926f));
    }
    check(od.history.entries().size() == entries,
          "undo: not one history entry has appeared mid-stroke, however many dabs it is");
    s.end();

    std::printf("  [measured] a %zu-dab stroke wrote %zu texels across %zu tiles; the "
                "document holds %zu tiles of %zu KiB\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                od.document.layers[0].rgbTiles->occupiedTileCount(), sizeof(Tile) / 1024);
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "brush stroke",
          "undo: a stroke of hundreds of dabs is EXACTLY ONE history entry, labelled for "
          "the tool -- ADR-0005's stroke-granular undo, reached by the new route too");
    check(od.revision > revBefore && od.structuralRevision == structBefore,
          "undo: it moved the content revision and not the structural one, so a stroke "
          "costs at most one journal write per interval rather than one per frame");

    // The accumulator, measured on both sides of pen-up. A live one is one
    // 64 KiB float tile per tile the stroke touched.
    RgbStroke probe;
    probe.begin({1.0f, 1.0f, 1.0f}, 1.0f);
    BrushTip wide = discTip(60.0f, 0.5f, {1.0f, 1.0f, 1.0f});
    TileStore scratch;
    for (int i = 0; i < 4; ++i)
      probe.depositDab(scratch, wide, Vec2{80.0f + 100.0f * static_cast<float>(i), 200.0f}, 512,
                       512, nullptr, nullptr);
    const size_t liveTiles = probe.accumulatorTiles();
    const size_t liveBytes = probe.accumulatorBytes();
    probe.end();
    std::printf("  [measured] accumulator while painting: %zu tiles, %zu KiB (%zu KiB per "
                "tile); after pen-up: %zu tiles, %zu KiB\n",
                liveTiles, liveBytes / 1024, sizeof(StrokeAlphaTile) / 1024,
                probe.accumulatorTiles(), probe.accumulatorBytes() / 1024);
    check(liveTiles > 0 && liveBytes == liveTiles * sizeof(StrokeAlphaTile),
          "accumulator: a stroke in flight holds exactly one 64 KiB float tile per tile it "
          "has touched -- sparse, so an untouched canvas costs nothing");
    check(probe.accumulatorTiles() == 0 && probe.accumulatorBytes() == 0 && !probe.active(),
          "accumulator: pen-up frees ALL of it -- an application sitting idle after a long "
          "stroke must not still be holding the stroke's scratch alpha");

    // The suite's own configuration claim: nothing in this section opens a
    // file, so every number above is the same in both NP_USE_OIIO builds.
    check(oiioBackendCompiledIn(),
          "config: this whole section reads no file, so its answers are configuration-"
          "independent -- and this build really does have the OIIO backend compiled in");
  }

  // ======================================================================
  // 14. Alpha lock: a FREEZE, not another bound (core/Layer.hpp's
  //     `alphaLocked`; brush/RgbDeposit.hpp §4.5)
  // ======================================================================
  //
  // The distinction this section exists to prove: a bound (`sel * dst.a` on
  // the weight, composited through the ordinary §2 rule) still lets a SECOND
  // separate stroke push the alpha further, because the bound is computed
  // fresh from whatever `dst.a` the first stroke left -- section 8 measures
  // exactly that creep for the selection, "0.5, then 0.75". A freeze must
  // not: `dst.a` is copied through unchanged by §4.5's rule, so there is
  // nothing for a later pass to read that the first pass could have moved.
  // This is the assertion sabotage (a) in this task's brief reddens.
  {
    OpenDocument od = makeRgbDoc(256, 256, 0);
    TileStore& store = *od.document.layers[0].rgbTiles;
    od.document.layers[0].alphaLocked = true;

    // A texel starting at alpha 0.5 -- not 0 and not 1, so a freeze (stays
    // 0.5), a bound computed from `dst.a` (climbs, section 8's shape) and an
    // unlocked composite (climbs to `opacity`) would each read differently.
    // Straight (0.3, 0.3, 0.3) at a=0.5: premultiplied (0.15, 0.15, 0.15,
    // 0.5), all exactly representable in binary16.
    {
      const PixelCoord p{64, 64};
      Tile& t0 = store.getOrCreate(tileCoordAt(p));
      t0.writePixel(tileLocalOffset(p), {0.15f, 0.15f, 0.15f, 0.5f});
    }
    const BrushTip t = discTip(20.0f, 1.0f, {1.0f, 1.0f, 1.0f});  // white ink, full flow

    for (int pass = 0; pass < 2; ++pass) {
      RgbStroke stroke;
      stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/true);
      stroke.depositDab(store, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);
      stroke.end();
    }
    const std::array<float, 4> after = readAt(store, 64, 64);
    std::printf("  [measured] alpha lock, two SEPARATE full-strength strokes: alpha stays "
                "%.9f (started at 0.5; a bound would have reached ~0.75 after the first pass "
                "alone, section 8's own shape)\n",
                static_cast<double>(after[3]));
    check(after[3] == 0.5f,
          "alpha lock: alpha is EXACTLY 0.5 after two separate full-strength strokes, at zero "
          "tolerance -- a freeze copies dst.a through unchanged, so there is nothing left for "
          "a second stroke to move; this is the assertion that tells a freeze from a bound");
    check(nearHalf(after[0], 0.5f) && nearHalf(after[1], 0.5f) && nearHalf(after[2], 0.5f),
          "alpha lock: and the COLOUR did change -- two full-strength white dabs over grey at "
          "a=0.5 land the straight colour at white, premultiplied by the frozen 0.5 -- so this "
          "is a lock that still paints, not a second `locked`");

    // The un-frozen control, same fixture, same dabs, `alphaLocked=false`: the
    // alpha DOES move, which is what makes the assertion above a real claim
    // about the flag rather than a coincidence of the numbers chosen.
    OpenDocument odCtrl = makeRgbDoc(256, 256, 0);
    TileStore& storeCtrl = *odCtrl.document.layers[0].rgbTiles;
    {
      const PixelCoord p{64, 64};
      storeCtrl.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p),
                                                        {0.15f, 0.15f, 0.15f, 0.5f});
    }
    RgbStroke unlocked;
    unlocked.begin(t.linearRgb, 1.0f, /*alphaLocked=*/false);
    unlocked.depositDab(storeCtrl, t, Vec2{64.5f, 64.5f}, 256, 256, nullptr, nullptr);
    unlocked.end();
    check(readAt(storeCtrl, 64, 64)[3] == 1.0f,
          "alpha lock: the IDENTICAL dab with the flag off reaches alpha 1.0 in one pass -- "
          "the control that shows section 14's premise (a starting alpha the flag actually "
          "has to hold back) is real");
  }

  // ======================================================================
  // 15. depositRgbTexel() directly: the composite rule, at zero tolerance
  // ======================================================================
  //
  // Section 14 exercises the flag through `RgbStroke`, which is what a real
  // stroke does; this is brush/RgbDeposit.hpp §4.5's formula checked against
  // the pure function it is written on, the same relationship section 1
  // through 5 already have with `dabCoverage()`.
  {
    // `a == 1` (weight and cap both saturate the accumulator from 0 in one
    // dab) makes the arithmetic checkable by hand: `out.rgb = ink * dst.a`,
    // `out.a = dst.a`.
    const std::array<float, 4> dst{0.1f, 0.2f, 0.3f, 0.5f};
    const std::array<float, 3> ink{1.0f, 1.0f, 1.0f};
    const RgbDepositStep locked = depositRgbTexel(dst, ink, 0.0f, 1.0f, 1.0f, /*alphaLocked=*/true);
    check(locked.dabAlpha == 1.0f && locked.strokeAlpha == 1.0f,
          "§4.5: the accumulator arithmetic (`a`, `A'`) is UNCHANGED by the flag -- it is not "
          "an input to either, only to which composite reads it");
    check(locked.premultiplied[0] == 0.5f && locked.premultiplied[1] == 0.5f &&
              locked.premultiplied[2] == 0.5f && locked.premultiplied[3] == 0.5f,
          "§4.5: white ink at a=1 over dst.a=0.5 gives premultiplied (0.5,0.5,0.5,0.5) exactly "
          "-- straight white at the FROZEN alpha, not the unlocked route's (1,1,1,1)");

    const RgbDepositStep unlocked =
        depositRgbTexel(dst, ink, 0.0f, 1.0f, 1.0f, /*alphaLocked=*/false);
    check(unlocked.premultiplied[3] == 1.0f,
          "§4.5: the SAME inputs with the flag off reach alpha 1.0 -- the control that shows "
          "the branch above is not vacuous");

    // `dst.a == 0`: "no paint on transparent texels", produced by the
    // arithmetic itself rather than by a special-cased branch (§4.5's own
    // claim). `dabAlpha` is still 1 -- the caller's cue that a dab happened
    // -- even though the texel it wrote is bit-identical to what was there.
    const std::array<float, 4> transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const RgbDepositStep onTransparent =
        depositRgbTexel(transparent, ink, 0.0f, 1.0f, 1.0f, /*alphaLocked=*/true);
    check(onTransparent.premultiplied == transparent,
          "§4.5: an alpha-locked dab on a fully TRANSPARENT texel comes back bit-identical to "
          "the input -- dst.rgb is 0 (premultiplied) and dst.a is 0, so both terms of the "
          "rule are 0 with no branch written to special-case it");
    check(onTransparent.dabAlpha == 1.0f,
          "§4.5: and `dabAlpha` still reports the dab as having HAPPENED -- the no-op is a "
          "property of the arithmetic's result, not of a refusal that skipped the texel");
  }

  std::printf("[selftest] rgb deposit %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
