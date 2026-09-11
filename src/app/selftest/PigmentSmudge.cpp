#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/PigmentSmudge.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The smudge on a Pigment layer (brush/PigmentSmudge; PRD F7's Pigment half),
// and the routing row that used to refuse it by name.
//
// See app/SelfTest.hpp for the section's own contents list and
// brush/PigmentSmudge.hpp for every decision this file only checks. The refusal
// this section replaces named one condition -- "the row opens when someone
// decides what the mass-weighted mean of a footprint of latents is and asserts
// it" -- so the section is built around asserting exactly that, three ways:
//
//   * **One pigment in is that pigment out, at ZERO tolerance**, over a whole
//     stroke: every texel a smudge leaves paint on holds the latent it started
//     with, bit for bit. That is brush/Deposit §1's idempotence-in-hue
//     invariant, held by a tool that moves paint rather than adds it.
//   * **Emptiness thins the paint and does not bleach it.** The rejected
//     arithmetic mean averages a zero-mass texel's latent in -- `Latent{}`,
//     which is white in the Mixbox basis, or the stale hue an eraser left --
//     and it is computed on the identical footprint and asserted DIFFERENT.
//   * **It agrees with the brush.** A mixed footprint picks up the latent
//     `depositTexel()` itself makes from the same two paints in the same
//     proportion: one mixing rule in this build, not two.
// ---------------------------------------------------------------------------
bool runPigmentSmudgeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerances -------------------------------------------------------
  //
  //  * **kHalfRel / kHalfFloor** -- binary16 storage, which is what a
  //    `core::PigmentTile` channel is: round-to-nearest relative error at most
  //    2^-11 for a normal value, plus half a subnormal ulp, 2^-25. Derived from
  //    the storage here, as runSmudgeTest() and runPigmentSelectionTest() each
  //    derive it for theirs. Section 5 is the one place a stored latent is
  //    compared with something it was not copied from, and it states its own
  //    per-write bound in place.
  //
  //  * **kRunningMeanUlp** -- the pick-up's latent is a RUNNING mean, one
  //    `mixLatents()` per contributing texel (brush/PigmentSmudge §1(ii)), so
  //    against a single closed-form lerp it can drift by one rounding per step:
  //    at most 2^-24 relative per `std::lerp` of values of magnitude <= 1, plus
  //    one more for the float-rounded weight. The bound used is `N * 2^-23` for
  //    a footprint of N paint texels, counted by the test from the footprint it
  //    actually walks, and the measured drift is printed beside it.
  //
  //  * Every assertion about HUE on a single-pigment fixture is at **exactly
  //    zero** tolerance, and that is the point of them: `std::lerp(z, z, t)` is
  //    specified to return `z`, and the fixture latents are f16-exact, so any
  //    drift at all means a second mixing rule is in the loop.
  constexpr float kHalfRel = 4.8828125e-04f;      // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;    // 2^-25
  constexpr double kRunningMeanUlp = 1.1920929e-07;  // 2^-23

  // **The two paints**, as latents with every one of the six stored floats
  // non-zero somewhere between them -- so a rule that mixed the pigment weights
  // and forgot the residual, or the reverse, cannot pass -- and every value
  // f16-exact (a short sum of powers of two), which is what lets the hue claims
  // be made at zero tolerance through the tile's binary16 storage. In the
  // Mixbox basis the weights are runPigmentDepositTest()'s cerulean and yellow;
  // every claim here is about lerps of six floats and holds in either basis.
  Latent kBlue;
  kBlue.c = {0.625f, 0.0f, 0.0f};
  kBlue.res = {0.0625f, 0.0f, -0.03125f};
  Latent kYellow;
  kYellow.c = {0.0f, 1.0f, 0.0f};
  kYellow.res = {0.0f, -0.0625f, 0.015625f};
  // What the pigment eraser leaves behind: no mass, a stale hue
  // (brush/PigmentErase §3). Deliberately a DIFFERENT pigment from the paint
  // beside it, so a rule that let a zero-mass latent into the mix shows up.
  const PigmentTexel kErasedYellow{kYellow, 0.0f};
  const PigmentTexel kBluePaint{kBlue, 1.0f};
  const PigmentTexel kYellowPaint{kYellow, 1.0f};
  const PigmentTexel kNothing{};

  auto sameLatent = [](const Latent& a, const Latent& b) { return a == b; };
  auto latentDiff = [](const Latent& a, const Latent& b) {
    double worst = 0.0;
    for (int i = 0; i < 3; ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(a.c[i]) - b.c[i]));
      worst = std::max(worst, std::fabs(static_cast<double>(a.res[i]) - b.res[i]));
    }
    return worst;
  };

  auto readAt = [](const PigmentTileStore& store, int32_t x, int32_t y) -> PigmentTexel {
    const PigmentTile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return PigmentTexel{};
    return tile->readTexel(tileLocalOffset(PixelCoord{x, y}));
  };

  // Written straight into the store, for runSmudgeTest()'s reason: this
  // section's subject is what the smudge does to paint already there, and
  // building that paint with brush/Deposit would put two arithmetics under
  // every number below.
  auto fillRect = [](PigmentTileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const PigmentTexel& t) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writeTexel(tileLocalOffset(p), t);
      }
  };

  // A hardness-1 disc: `dabCoverage()` is exactly 1 over its flat CORE, out to
  // `radius - edgePx`, and antialiased in the last pixel (BrushTip::edgePx) --
  // not "exactly 1 over the whole footprint", as this said before `edgePx`.
  // The checks below never needed the whole footprint to be 1, which is why
  // none of them moved: the pick-up fixtures rest on SYMMETRY (a footprint
  // split down its centre line has the same rim on both halves, so "half the
  // mass" is still exactly half), on UNIFORM mass or hue across the footprint
  // (any coverage weighting of one value is that value), or on probes on the
  // drag's spine, inside the core -- so the numbers are about the smudge and
  // not about the falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    return t;
  };

  // Layer 0 is the blank document's RGB base; the Pigment layer is layer 1.
  auto makePigmentDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "pigment smudge");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                 makePigmentLayer("Pigment 0")));
    return od;
  };

  auto dragDabs = [](float fromX, float toX) {
    std::vector<Vec2> dabs;
    const float step = fromX < toX ? 2.0f : -2.0f;
    for (float x = fromX; step > 0.0f ? x <= toX : x >= toX; x += step)
      dabs.push_back(Vec2{x, 64.0f});
    return dabs;
  };

  // ======================================================================
  // 1. smudgePigmentTexel(): the write
  // ======================================================================
  {
    // Strength 0 is a bit-exact no-op -- and the rejected strength-free weight
    // (a blur) writes on the identical inputs, so the claim is not vacuous.
    const PigmentSmudgeStep zero = smudgePigmentTexel(kBluePaint, kYellowPaint, 1.0f, 0.0f);
    const PigmentSmudgeStep blur = smudgePigmentTexel(kBluePaint, kYellowPaint, 1.0f, 1.0f);
    check(zero.texel == kBluePaint && zero.dabAlpha == 0.0f && blur.dabAlpha > 0.0f &&
              !(blur.texel == kBluePaint),
          "write: strength 0 returns the texel BIT-IDENTICAL with dabAlpha 0, and the "
          "strength-free weight does write on these inputs -- the no-op is real");

    // **The empty pair is refused on MASS** (header §3). The two texels differ
    // -- their stale latents are different pigments -- so the RGB route's
    // all-channels equality would have written here, rewriting an erased texel
    // with an unchanged mass 0 and dirtying its 224 KiB tile.
    const PigmentSmudgeStep emptyPair = smudgePigmentTexel(kErasedYellow, kNothing, 1.0f, 1.0f);
    check(!(kErasedYellow == kNothing) && emptyPair.dabAlpha == 0.0f &&
              emptyPair.texel == kErasedYellow,
          "write: an erased texel under an empty finger is LEFT ALONE -- nothing to move is "
          "decided on mass, whatever stale hue either side carries");

    // a == 1 writes the finger exactly: `(1-1)*d.m == 0`, weight `x/x == 1`.
    const PigmentSmudgeStep full = smudgePigmentTexel(kBluePaint, kYellowPaint, 4.0f, 1.0f);
    check(full.dabAlpha == 1.0f && full.texel == kYellowPaint,
          "write: a weight above 1 is clamped, and a == 1 lays the finger down EXACTLY -- "
          "both mass and all six latent floats");

    // An empty finger over paint: less paint, same hue, exactly.
    const PigmentSmudgeStep thin = smudgePigmentTexel(kBluePaint, kNothing, 1.0f, 0.5f);
    check(thin.dabAlpha == 0.5f && sameLatent(thin.texel.latent, kBlue) &&
              thin.texel.mass == 0.5f,
          "write: an EMPTY finger over paint halves the mass and leaves the hue EXACTLY -- "
          "less of the same paint, PRD F10's meaning of taking paint away");

    // A loaded finger over an erased texel: the finger's hue outright, not
    // mixed with the stale one -- depositTexel()'s §1(ii) rule.
    const PigmentTexel halfBlue{kBlue, 0.5f};
    const PigmentSmudgeStep over = smudgePigmentTexel(kErasedYellow, halfBlue, 1.0f, 0.5f);
    check(sameLatent(over.texel.latent, kBlue) && over.texel.mass == 0.25f,
          "write: a loaded finger over an ERASED texel lays its own hue down outright -- the "
          "stale yellow the eraser left is not paint and is not mixed in");

    // Mass-weighted, and NOT the plain lerp. Blue at mass 1 under a finger of
    // yellow at mass 0.25, a = 0.5: 0.5 of blue survives against 0.125 of
    // yellow, so the yellow weight is 0.125 / 0.625 = 0.2 -- not 0.5.
    const PigmentTexel quarterYellow{kYellow, 0.25f};
    const PigmentSmudgeStep mixed = smudgePigmentTexel(kBluePaint, quarterYellow, 1.0f, 0.5f);
    const Latent want = mixLatents(kBlue, kYellow, 0.2f);
    const Latent plain = mixLatents(kBlue, kYellow, 0.5f);
    std::printf("  [measured] write mix: off the mass-weighted latent by %.3g, off the "
                "plain a-lerp by %.3g; mass %.4f\n",
                latentDiff(mixed.texel.latent, want), latentDiff(mixed.texel.latent, plain),
                static_cast<double>(mixed.texel.mass));
    check(latentDiff(mixed.texel.latent, want) <= 2.0 * kRunningMeanUlp &&
              latentDiff(mixed.texel.latent, plain) > 0.1 && mixed.texel.mass == 0.625f,
          "write: the latent mixes by the paint each side contributes -- (1-a)*dst.mass "
          "against a*finger.mass -- and not by a alone; mass lerps like alpha");

    // Hue idempotence on the write: one pigment on both sides, any masses.
    const PigmentSmudgeStep same =
        smudgePigmentTexel(PigmentTexel{kBlue, 0.75f}, PigmentTexel{kBlue, 0.25f}, 0.6f, 0.5f);
    check(same.dabAlpha > 0.0f && sameLatent(same.texel.latent, kBlue),
          "write: blue smudged into blue at different masses is blue at ZERO tolerance -- "
          "the mass moves, the hue cannot");
  }

  // ======================================================================
  // 2. smudgePigmentFinger(): the latch, both endpoints, and the middle
  // ======================================================================
  {
    const PigmentTexel pick{kBlue, 0.25f};
    const PigmentTexel carried{kYellow, 1.0f};
    check(smudgePigmentFinger(kNothing, false, pick, 1.0f) == pick &&
              smudgePigmentFinger(carried, false, pick, 0.5f) == pick,
          "finger: the first dab LOADS the pick-up outright whatever the strength -- a "
          "strength-1 smudge starting from nothing would carry nothing forever");
    check(smudgePigmentFinger(carried, true, pick, 1.0f) == carried &&
              smudgePigmentFinger(carried, true, pick, 0.0f) == pick,
          "finger: strength 1 keeps the carried paint EXACTLY and strength 0 replaces it "
          "EXACTLY -- both ends are specified-exact lerps, latent and mass alike");

    // s = 0.5: 0.5 of the carried yellow at mass 1 against 0.5 of the picked
    // blue at mass 0.25, so the yellow weight is 0.5 / 0.625 = 0.8.
    const PigmentTexel mid = smudgePigmentFinger(carried, true, pick, 0.5f);
    check(mid.mass == 0.625f &&
              latentDiff(mid.latent, mixLatents(kBlue, kYellow, 0.8f)) <= 2.0 * kRunningMeanUlp,
          "finger: between the ends the MASS decays at the strength and the latent mixes by "
          "the paint each side holds -- the carried colour is a Kubelka-Munk mix");
  }

  // ======================================================================
  // 3. The pick-up: THE mass-weighted mean of a footprint of latents
  // ======================================================================
  //
  // The condition the old refusal named, asserted three ways. Every probe is a
  // flow-0 dab, so it measures the pick-up alone and writes nothing.
  {
    // (a) One pigment, any masses -> that pigment, bit for bit.
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    // The radius-8 disc at x = 32 spans x in [24, 40]; the mass-1 block ends
    // at x = 31, so the footprint is split between the two masses. (A block
    // reaching x = 40 covered the whole disc and made this a one-mass fixture.)
    fillRect(store, 0, 0, 127, 255, PigmentTexel{kBlue, 0.5f});
    fillRect(store, 0, 100, 31, 156, PigmentTexel{kBlue, 1.0f});
    PigmentSmudgeStroke s;
    s.begin(1.0f);
    s.smudgeDab(store, discTip(8.0f, 0.0f), Vec2{32.0f, 128.0f}, 256, 256, nullptr, nullptr);
    std::printf("  [measured] one pigment at masses 1.0 and 0.5: picked mass %.6f, latent off "
                "blue by %.3g\n",
                static_cast<double>(s.finger().mass), latentDiff(s.finger().latent, kBlue));
    check(s.loaded() && sameLatent(s.finger().latent, kBlue) && s.finger().mass > 0.5f &&
              s.finger().mass < 1.0f,
          "pick-up: a footprint of ONE pigment at two different masses picks up that pigment "
          "at ZERO tolerance -- the running lerp cannot walk a hue that is not moving");
    s.end();

    // (b) Half paint, half ABSENT tile: half the mass, and the hue untouched.
    // Centred on the tile boundary x = 128, which a radius-8 hard disc straddles
    // exactly symmetrically (texel centres at x + 0.5).
    OpenDocument od2 = makePigmentDoc(256, 256);
    PigmentTileStore& store2 = *od2.document.layers[1].pigmentTiles;
    fillRect(store2, 0, 0, 127, 255, kBluePaint);
    const size_t seeded = store2.occupiedTileCount();
    PigmentSmudgeStroke half;
    half.begin(1.0f);
    half.smudgeDab(store2, discTip(8.0f, 0.0f), Vec2{128.0f, 128.0f}, 256, 256, nullptr,
                   nullptr);
    std::printf("  [measured] pick-up over half paint, half absent tile: mass %.6f\n",
                static_cast<double>(half.finger().mass));
    check(half.finger().mass == 0.5f && sameLatent(half.finger().latent, kBlue) &&
              store2.occupiedTileCount() == seeded,
          "pick-up: half a footprint of paint picks up HALF THE MASS and the hue EXACTLY -- "
          "emptiness thins the finger and never bleaches it; and reading allocated nothing");
    half.end();

    // (c) Half paint, half ERASED paint of another hue. The built rule ignores
    // the stale yellow; the rejected arithmetic mean averages it in.
    OpenDocument od3 = makePigmentDoc(256, 256);
    PigmentTileStore& store3 = *od3.document.layers[1].pigmentTiles;
    fillRect(store3, 0, 0, 127, 255, kBluePaint);
    fillRect(store3, 128, 0, 255, 255, kErasedYellow);
    PigmentSmudgeStroke erased;
    erased.begin(1.0f);
    const BrushTip probe = discTip(8.0f, 0.0f);
    const Vec2 at{128.0f, 128.0f};
    erased.smudgeDab(store3, probe, at, 256, 256, nullptr, nullptr);
    // The rejected model, walked over the identical footprint: a coverage-
    // weighted mean of the latents with mass left out of the weight -- which is
    // brush/Smudge §2's arithmetic applied to the latent as if it were colour.
    Latent rejected{};
    double w = 0.0;
    const PixelBounds b = dabPixelBounds(probe, at, 256, 256);
    for (int32_t y = b.y0; y <= b.y1; ++y)
      for (int32_t x = b.x0; x <= b.x1; ++x) {
        const float cov = dabCoverage(probe, (static_cast<float>(x) + 0.5f) - at.x,
                                      (static_cast<float>(y) + 0.5f) - at.y);
        if (!(cov > 0.0f)) continue;
        w += cov;
        rejected = mixLatents(rejected, readAt(store3, x, y).latent,
                              static_cast<float>(cov / w));
      }
    std::printf("  [measured] over blue beside ERASED yellow: built latent is off blue by "
                "%.3g; the arithmetic mean is off by %.3g\n",
                latentDiff(erased.finger().latent, kBlue), latentDiff(rejected, kBlue));
    check(sameLatent(erased.finger().latent, kBlue) && erased.finger().mass == 0.5f &&
              latentDiff(rejected, kBlue) > 0.1,
          "pick-up: an ERASED yellow beside blue paint contributes no hue -- blue EXACTLY -- "
          "while the arithmetic mean on the same footprint resurrects the yellow");
    erased.end();

    // (d) Two real paints: blue at mass 1 beside yellow at mass 0.25. Equal
    // coverage each side, so yellow's share of the paint is 0.25/1.25 = 0.2 --
    // exactly the weight depositTexel() gives 0.25 of yellow laid on 1.0 of
    // blue. The same mixing rule, not a second one.
    OpenDocument od4 = makePigmentDoc(256, 256);
    PigmentTileStore& store4 = *od4.document.layers[1].pigmentTiles;
    fillRect(store4, 0, 0, 127, 255, kBluePaint);
    fillRect(store4, 128, 0, 255, 255, PigmentTexel{kYellow, 0.25f});
    PigmentSmudgeStroke two;
    two.begin(1.0f);
    two.smudgeDab(store4, probe, at, 256, 256, nullptr, nullptr);
    const PigmentTexel brush = depositTexel(kBluePaint, kYellow, 0.25f);
    size_t paintTexels = 0;
    for (int32_t y = b.y0; y <= b.y1; ++y)
      for (int32_t x = b.x0; x <= b.x1; ++x)
        if (dabCoverage(probe, (static_cast<float>(x) + 0.5f) - at.x,
                        (static_cast<float>(y) + 0.5f) - at.y) > 0.0f)
          ++paintTexels;
    const double bound = static_cast<double>(paintTexels) * kRunningMeanUlp;
    const double drift = latentDiff(two.finger().latent, brush.latent);
    std::printf("  [measured] blue(1.0) beside yellow(0.25): off depositTexel()'s own mix by "
                "%.3g (bound %.3g over %zu texels); off the unweighted 50/50 by %.3g\n",
                drift, bound, paintTexels,
                latentDiff(two.finger().latent, mixLatents(kBlue, kYellow, 0.5f)));
    check(drift <= bound && two.finger().mass == 0.625f &&
              latentDiff(two.finger().latent, mixLatents(kBlue, kYellow, 0.5f)) > 0.1,
          "pick-up: two paints in a footprint mix to EXACTLY what depositTexel() makes of "
          "them in the same proportion -- one Kubelka-Munk rule in this build, not two");
    two.end();
  }

  // ======================================================================
  // 4. Direction, falloff, and the hue of every texel the stroke touched
  // ======================================================================
  {
    OpenDocument od = makePigmentDoc(256, 128);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    fillRect(store, 0, 0, 63, 127, kBluePaint);
    PigmentSmudgeStroke s;
    s.begin(0.75f);
    const StrokeDeposit out =
        s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    s.end();
    std::printf("  [measured] mass along the drag past the boundary at x=63:");
    for (int32_t x = 76; x <= 196; x += 20)
      std::printf(" %d:%.5f", x, static_cast<double>(readAt(store, x, 64).mass));
    std::printf("  (%zu dabs, %zu texels)\n", out.dabs, out.texels);

    const float nearMass = readAt(store, 76, 64).mass;
    const float farMass = readAt(store, 196, 64).mass;
    bool monotone = true;
    float prev = nearMass;
    for (int32_t x = 80; x <= 196; x += 4) {
      const float m = readAt(store, x, 64).mass;
      if (m > prev) monotone = false;
      prev = m;
    }
    check(nearMass > 0.05f && monotone && nearMass > farMass * 4.0f,
          "direction: paint is carried BEYOND the boundary and thins monotonically along the "
          "drag, at least 4x weaker at the far end -- the finger decaying per dab");

    // **The zero-tolerance hue claim, over the whole stroke.** Every texel of
    // the layer that holds paint holds BLUE, bit for bit: the stroke moved
    // mass over thousands of texel writes and could not move the hue at all.
    size_t painted = 0;
    bool allBlue = true;
    for (int32_t y = 0; y < 128; ++y)
      for (int32_t x = 0; x < 256; ++x) {
        const PigmentTexel t = readAt(store, x, y);
        if (!(t.mass > 0.0f)) continue;
        ++painted;
        if (!sameLatent(t.latent, kBlue)) allBlue = false;
      }
    std::printf("  [measured] %zu texels hold paint after the drag\n", painted);
    check(allBlue && painted > 64 * 128,
          "hue: EVERY painted texel on the layer is blue at ZERO tolerance after a whole "
          "smudge -- it can move the paint in the picture and cannot invent one");

    // The same drag backwards: starts on blank canvas with an empty finger,
    // so the region past the boundary is never touched.
    OpenDocument rev = makePigmentDoc(256, 128);
    PigmentTileStore& revStore = *rev.document.layers[1].pigmentTiles;
    fillRect(revStore, 0, 0, 63, 127, kBluePaint);
    PigmentSmudgeStroke back;
    back.begin(0.75f);
    back.smudgeDabs(revStore, discTip(6.0f, 1.0f), dragDabs(200.0f, 32.0f), 256, 128, nullptr);
    back.end();
    check(readAt(revStore, 76, 64) == kNothing && readAt(revStore, 196, 64) == kNothing,
          "direction: the SAME drag run BACKWARDS leaves that region bit-identically empty -- "
          "paint travels the way the stroke does");
  }

  // ======================================================================
  // 5. Two paints meet: the smear is a Kubelka-Munk mix and nothing else
  // ======================================================================
  //
  // Blue dragged into yellow. Every texel the stroke writes must hold a latent
  // ON THE SEGMENT between the two -- a lerp of them and nothing else -- and,
  // because both paints are at mass 1, mass 1 exactly: a smudge through solid
  // paint moves colour and neither adds nor removes any.
  {
    OpenDocument od = makePigmentDoc(256, 128);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    fillRect(store, 0, 0, 63, 127, kBluePaint);
    fillRect(store, 64, 0, 255, 127, kYellowPaint);
    PigmentSmudgeStroke s;
    s.begin(0.75f);
    s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 160.0f), 256, 128, nullptr);
    s.end();

    // t from the one component where the two paints differ most (c1: 0 vs 1),
    // then all six components checked against lerp(blue, yellow, t).
    //
    // **The bound is binary16 rounding, counted per WRITE.** In exact arithmetic
    // every write is a convex combination of points on the segment, so it lands
    // on the segment. The tile rounds each channel to binary16 at every write --
    // at most half an ulp, 2^-12, for a value below 1 -- and a later convex
    // combination of points each within e of the line is itself within e, so
    // the deviation can grow by at most one rounding per write. A radius-6 disc
    // stepping 2 texels along the row writes any one texel in at most 7 dabs;
    // add one rounding for the read of `share` itself and one for the lerp that
    // builds `line`. Measured beside it.
    constexpr int kWritesPerTexel = 7;
    constexpr double kHalfUlpBelowOne = 2.44140625e-04;  // 2^-12
    const double segmentBound = (kWritesPerTexel + 2) * kHalfUlpBelowOne;
    double worstDev = 0.0;
    bool inRange = true;
    bool massKept = true;
    float mostMixed = 1.0f;  // the smallest yellow share seen past the boundary
    for (int32_t x = 0; x < 256; ++x) {
      const PigmentTexel t = readAt(store, x, 64);
      if (t.mass != 1.0f) massKept = false;
      const float share = t.latent.c[1];  // kYellow.c1 == 1, kBlue.c1 == 0
      if (share < -kHalfFloor || share > 1.0f + kHalfRel) inRange = false;
      worstDev = std::max(worstDev, latentDiff(t.latent, mixLatents(kBlue, kYellow, share)));
      if (x >= 70) mostMixed = std::min(mostMixed, share);
    }
    std::printf("  [measured] worst distance from the blue-yellow line along the drag: %.3g "
                "(bound %.3g)\n",
                worstDev, segmentBound);
    const bool onSegment = inRange && worstDev <= segmentBound;
    const PigmentTexel probe = readAt(store, 80, 64);
    const std::array<float, 3> rgb = latentToRgb(probe.latent);
    std::printf("  [measured] blue dragged into yellow: at x=80 yellow share %.4f, projects "
                "to linear RGB (%.3f %.3f %.3f); least-yellow texel past x=70 is %.4f\n",
                static_cast<double>(probe.latent.c[1]), static_cast<double>(rgb[0]),
                static_cast<double>(rgb[1]), static_cast<double>(rgb[2]),
                static_cast<double>(mostMixed));
    check(onSegment && mostMixed < 0.9f && mostMixed > 0.0f,
          "mix: blue dragged into yellow leaves latents strictly BETWEEN the two and ON the "
          "line joining them -- a Kubelka-Munk mix of the paints present, nothing invented");
    check(massKept,
          "mix: and every texel along the drag still holds mass 1 EXACTLY -- through solid "
          "paint a smudge moves colour and neither adds nor removes any");
  }

  // ======================================================================
  // 6. Both endpoints of STRENGTH, end to end
  // ======================================================================
  {
    OpenDocument od = makePigmentDoc(256, 128);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    fillRect(store, 0, 0, 63, 127, kBluePaint);
    PigmentSmudgeStroke s;
    s.begin(1.0f);
    s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    check(s.finger() == kBluePaint,
          "strength 1: the finger still holds the paint it picked up at pen-down after the "
          "whole drag, exactly");
    s.end();
    check(readAt(store, 196, 64) == kBluePaint && readAt(store, 100, 64) == kBluePaint,
          "strength 1: and that paint is on the layer 133 texels past its boundary at ZERO "
          "tolerance, mass and latent");

    OpenDocument quiet = makePigmentDoc(256, 128);
    PigmentTileStore& quietStore = *quiet.document.layers[1].pigmentTiles;
    fillRect(quietStore, 0, 0, 63, 127, kBluePaint);
    const size_t tilesBefore = quietStore.occupiedTileCount();
    PigmentSmudgeStroke none;
    none.begin(0.0f);
    const StrokeDeposit nothing = none.smudgeDabs(
        quietStore, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    none.end();
    bool unchanged = quietStore.occupiedTileCount() == tilesBefore;
    for (int32_t x = 0; x < 256; ++x)
      if (!(readAt(quietStore, x, 64) == (x <= 63 ? kBluePaint : kNothing))) unchanged = false;
    check(unchanged && nothing.texels == 0 && nothing.tiles.empty() && nothing.dabs > 50,
          "strength 0: a whole drag across real paint writes NOT ONE texel and reports not "
          "one tile -- nothing to re-upload and nothing to undo");
  }

  // ======================================================================
  // 7. The selection bounds the write (PRD E1, P0)
  // ======================================================================
  {
    OpenDocument od = makePigmentDoc(256, 128);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    fillRect(store, 0, 0, 63, 127, kBluePaint);
    const Selection sel = selectRectangle(0.0f, 0.0f, 120.0f, 128.0f);
    PigmentSmudgeStroke s;
    s.begin(0.9f);
    s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, &sel);
    s.end();
    bool outsideClean = true;
    for (int32_t x = 121; x < 256; ++x)
      if (!(readAt(store, x, 64) == kNothing)) outsideClean = false;
    check(readAt(store, 100, 64).mass > 0.02f && outsideClean,
          "selection: the smear reaches x=100 inside the ants and stops DEAD at the boundary "
          "-- every texel beyond it bit-identically untouched");

    OpenDocument od2 = makePigmentDoc(256, 128);
    PigmentTileStore& store2 = *od2.document.layers[1].pigmentTiles;
    fillRect(store2, 0, 0, 63, 127, kBluePaint);
    const Selection elsewhere = selectRectangle(200.0f, 100.0f, 250.0f, 127.0f);
    PigmentSmudgeStroke s2;
    s2.begin(0.9f);
    s2.smudgeDabs(store2, discTip(6.0f, 1.0f), dragDabs(32.0f, 100.0f), 256, 128, &elsewhere);
    s2.end();
    check(readAt(store2, 32, 64) == kBluePaint && readAt(store2, 76, 64) == kNothing,
          "selection: a drag entirely outside an engaged selection changes NOTHING -- an absent "
          "selection tile is \"selects nothing\"");
  }

  // ======================================================================
  // 8. Smudging nothing costs nothing -- including over erased paint
  // ======================================================================
  {
    OpenDocument od = makePigmentDoc(512, 512);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    // An ERASED patch: tiles that exist, holding a stale hue at mass 0 --
    // exactly what brush/PigmentErase leaves. The drag starts on blank canvas,
    // so the finger is empty when it arrives.
    fillRect(store, 200, 220, 400, 280, kErasedYellow);
    const size_t seeded = store.occupiedTileCount();
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    BrushTip t = discTip(24.0f, 1.0f);
    t.smudgeStrength = 1.0f;  // the hardest setting: the finger never decays
    StrokeSession sess;
    std::string err;
    sess.begin(od, 1, t, Tool::Smudge, &err);
    for (int k = 0; k < 30; ++k) sess.addPoint(40.0f + 14.0f * static_cast<float>(k), 250.0f);
    sess.end();
    std::printf("  [measured] a %zu-dab drag across blank canvas and erased paint: %zu texels, "
                "%zu tiles reported, %zu tiles in the store (%zu seeded)\n",
                sess.dabCount(), sess.texelsWritten(), sess.strokeTiles().size(),
                store.occupiedTileCount(), seeded);
    check(sess.dabCount() > 20 && sess.texelsWritten() == 0 && sess.strokeTiles().empty() &&
              store.occupiedTileCount() == seeded,
          "empty: dozens of dabs across blank canvas AND an erased patch write nothing and "
          "allocate no 224 KiB tile -- an empty finger over empty paint is decided on mass");
    check(od.history.entries().size() == entries && od.revision == revBefore,
          "empty: and a stroke that smudged nothing records nothing -- no undo step, no "
          "revision bump");

    OpenDocument od2 = makePigmentDoc(512, 512);
    PigmentTileStore& store2 = *od2.document.layers[1].pigmentTiles;
    fillRect(store2, 0, 32, 60, 96, kBluePaint);
    const size_t seeded2 = store2.occupiedTileCount();
    PigmentSmudgeStroke grow;
    grow.begin(1.0f);
    grow.smudgeDabs(store2, discTip(10.0f, 1.0f), dragDabs(30.0f, 300.0f), 512, 512, nullptr);
    grow.end();
    check(store2.occupiedTileCount() > seeded2 && readAt(store2, 290, 64).mass > 0.0f,
          "empty: but a LOADED finger over blank canvas DOES allocate and write -- a smudge "
          "grows the painted region");
  }

  // ======================================================================
  // 9. The routing row, and the session end to end
  // ======================================================================
  {
    Layer pigment = makePigmentLayer("p");
    Layer storeless;
    storeless.kind = LayerKind::Pigment;
    check(pigment.pigmentTiles.has_value() &&
              strokeRouteFor(Tool::Smudge, &pigment) == StrokeRoute::PigmentSmudge &&
              strokeRouteWritesLayer(StrokeRoute::PigmentSmudge) &&
              grainReachesRoute(StrokeRoute::PigmentSmudge) &&
              !wetnessReachesSolver(StrokeRoute::PigmentSmudge),
          "routing: Smudge on a writable Pigment layer takes pigment-smudge, which writes a "
          "layer, is reached by paper grain and never touches the solver");
    check(std::string(strokeRouteName(StrokeRoute::PigmentSmudge)) == "pigment-smudge" &&
              std::string(strokeEditLabel(Tool::Smudge)) == "smudge",
          "routing: the route is named for its storage, the history row for the edit -- "
          "\"smudge\" on either layer kind");
    check(strokeRouteFor(Tool::Smudge, &storeless) == StrokeRoute::None,
          "routing: a Pigment layer whose store was never allocated still refuses -- None, "
          "not a fallthrough");
    pigment.locked = true;
    check(strokeRouteFor(Tool::Smudge, &pigment) == StrokeRoute::None,
          "routing: a LOCKED Pigment layer refuses, locked before kind");
    pigment.locked = false;
    // A hand-edited document can carry the flag on this kind; it freezes an
    // alpha channel a Pigment layer does not have, so it must not block the
    // row -- the pigment eraser's position on the same flag.
    pigment.alphaLocked = true;
    check(strokeRouteFor(Tool::Smudge, &pigment) == StrokeRoute::PigmentSmudge &&
              strokeRouteFor(Tool::Eraser, &pigment) == StrokeRoute::PigmentErase,
          "routing: alphaLocked on a Pigment layer does NOT refuse the smudge -- it freezes a "
          "channel this kind does not have, as the pigment eraser already reads it");

    // Through StrokeSession: the route, PRD E1 read live, the strength read
    // from STRENGTH and not from OPACITY, and ONE undo step labelled "smudge".
    OpenDocument od = makePigmentDoc(512, 256);
    fillRect(*od.document.layers[1].pigmentTiles, 0, 0, 63, 255, kBluePaint);
    od.selection = selectRectangle(0.0f, 0.0f, 120.0f, 256.0f);
    const size_t entries = od.history.entries().size();
    const uint64_t structBefore = od.structuralRevision;
    BrushTip tip = discTip(6.0f, 1.0f);
    tip.smudgeStrength = 0.9f;
    tip.opacity = 0.0f;  // hostile: the field the smudge must NOT read
    StrokeSession sess;
    std::string err;
    check(sess.begin(od, 1, tip, Tool::Smudge, &err) && err.empty() &&
              sess.route() == StrokeRoute::PigmentSmudge,
          "session: a smudge begins on the Pigment layer and takes the pigment-smudge route");
    for (int k = 0; k <= 21; ++k) sess.addPoint(32.0f + 8.0f * static_cast<float>(k), 64.0f);
    check(od.history.entries().size() == entries,
          "session: not one history entry mid-stroke, however many dabs");
    sess.end();
    const PigmentTileStore& gated = *od.document.layers[1].pigmentTiles;
    bool outsideClean = true;
    for (int32_t x = 121; x < 512; ++x)
      if (!(readAt(gated, x, 64) == kNothing)) outsideClean = false;
    check(readAt(gated, 100, 64).mass > 0.02f && sameLatent(readAt(gated, 100, 64).latent, kBlue) &&
              outsideClean,
          "session: the smear lands inside the ants in blue, reads STRENGTH with OPACITY at 0, "
          "and stops at the selection read live off the document");
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "smudge" &&
              od.structuralRevision == structBefore,
          "session: the whole stroke is EXACTLY ONE history entry, labelled \"smudge\", and a "
          "content edit rather than a structural one");
  }

  std::printf("[selftest] pigment smudge %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
