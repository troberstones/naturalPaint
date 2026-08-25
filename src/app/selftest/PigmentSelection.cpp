#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/PigmentErase.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The active selection on a Pigment layer -- the gate the pigment deposit did
// not have (brush/Deposit §4; PRD E1, **P0**) -- and the eraser that gate
// unblocked (brush/PigmentErase; PRD F9/F10, both **P0**; ADR-0007).
//
// See app/SelfTest.hpp for the section's own contents list, and the two headers
// for every decision this file only checks. Three things are worth saying here
// because they are what the section is *for*:
//
//   * **The brush painted straight through the marching ants.**
//     `depositDab(PigmentTileStore&, ...)` took no `Selection` at all -- the
//     word did not appear in `brush/Deposit.hpp` -- so `app/StrokeSession`'s
//     pigment branch could not pass one while the RGB branch on the line
//     directly above it passed `doc_->selection` explicitly. Section A is the
//     assertion that fails against that code, on a texel the dab covers and the
//     selection does not, at exactly zero.
//   * **`kMaxMass` was not already a bound, and that had to be derived rather
//     than assumed.** The RGB route's argument for gating the ceiling as well as
//     the rate rests on a per-stroke accumulator this route does not have. Mass
//     accumulates *linearly*, so gating only the rate still reaches `kMaxMass`
//     exactly -- at the shipped defaults a half-selected texel saturates in six
//     dabs, one and a half radii of travel. Section E runs that model beside the
//     built one and prints both numbers.
//   * **A Pigment texel emptied by the eraser keeps its hue, and that is the
//     opposite of the RGB rule.** `brush/RgbErase` insists a fully erased texel
//     hold RGB 0 as well as alpha 0, because `core::Tile` is premultiplied. A
//     Pigment texel is not: `projectPigmentTexel()` multiplies the latent by the
//     mass, so mass 0 composites to four exact zeros whatever the hue, and
//     `depositTexel()`'s §1(ii) is written for precisely that texel. Section K
//     asserts the composited consequence rather than the storage convention.
// ---------------------------------------------------------------------------
bool runPigmentSelectionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerances -------------------------------------------------------
  //
  //  * **kHalfRel / kHalfFloor** -- binary16 storage, which is what a
  //    `core::PigmentTile` channel is. An 11-bit significand gives a
  //    round-to-nearest relative error of at most 2^-11 = 4.883e-04 for a normal
  //    value, plus an absolute floor of half a subnormal ulp, 2^-25 = 2.980e-08.
  //    Derived here from the storage rather than borrowed from
  //    runPigmentDepositTest() or runRgbEraseTest(), which state the identical
  //    derivation for the identical storage -- a tolerance copied without its
  //    derivation is the one that later gets applied where it does not hold.
  //
  //  * **kAccumTol for a stroke of N writes** -- the layer rounds to binary16
  //    once per dab that writes, while brush/PigmentErase's accumulator does not
  //    (brush/RgbDeposit.hpp §3's float-not-half derivation, borrowed with the
  //    accumulator). So the stored mass after N *writing* dabs can drift from
  //    `mass_0 * (1 - E)` by at most `N * kHalfRel * value`: each write
  //    contributes one rounding of at most `kHalfRel` relative, and every earlier
  //    error is then damped by the factor `(1 - e)` of every later dab, which is
  //    at most 1. Computed per test from the N that test actually spends, and the
  //    measured value printed beside it.
  //
  //  * Several assertions below are at **exactly zero** tolerance, and each says
  //    why in place. Three kinds: claims about which operations happen (a texel
  //    the selection excludes is not written at all; a deposit past its cap adds
  //    no mass); claims that a value is *the stored form of a specific float*
  //    (the cap writes `kMaxMass * sel` itself, so the stored half is
  //    `floatToHalf()` of it and nothing else); and claims that a particular
  //    multiply is exact in binary16 (scaling a normal half by 0.5 only
  //    decrements its exponent, so a strength-0.5 erase of 0.75 really is 0.375
  //    and not approximately).
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };
  // The stored form of a float, for the assertions that are about the value the
  // code computed rather than about how accurately it computed it.
  auto stored = [](float v) { return halfToFloat(floatToHalf(v)); };

  // Mixbox's own primaries, read straight off core/Pigment.cpp's polynomial
  // rather than through paint/Palette's 512x512 LUT -- runPigmentDepositTest()'s
  // choice, for its reason: building latents from the weights keeps this whole
  // section free of file I/O, which is what makes its answers the same in both
  // NP_USE_OIIO configurations by construction rather than by luck. 0.625 rather
  // than 0.6 because 5/8 is exact in binary16, so a latent stored here
  // round-trips through the f16 tile with no error at all and every hue
  // assertion below can be at zero tolerance.
  Latent kBlue;
  kBlue.c = {0.625f, 0.0f, 0.0f};
  Latent kRed;
  kRed.c = {0.0f, 0.0f, 1.0f};

  // A hard disc, so `dabCoverage()` is exactly 1.0f over the whole footprint and
  // every number below is about the selection rather than about the falloff.
  // runPigmentDepositTest() is where the falloff itself is checked.
  auto discTip = [](float radius, float flow, const Latent& z) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.pigment = z;
    return t;
  };

  auto readAt = [](const PigmentTileStore& store, int32_t x, int32_t y) -> PigmentTexel {
    const PixelCoord at{x, y};
    const PigmentTile* t = store.find(tileCoordAt(at));
    return t ? t->readTexel(tileLocalOffset(at)) : PigmentTexel{};
  };

  // Written straight into the store rather than painted with brush/Deposit
  // first, deliberately: the erase sections' subject is what the eraser does to
  // a texel that is already there, and building the "already there" out of the
  // *other* module would make every number below depend on two arithmetics
  // instead of one. A regression in the deposit would then show up here as an
  // erase failure, in a section that has nothing to say about it.
  auto fillRect = [](PigmentTileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const PigmentTexel& texel) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writeTexel(tileLocalOffset(p), texel);
      }
    }
  };

  // Every occupied tile's raw half words, keyed by coordinate. The ground truth
  // the "outside the ants is untouched" assertions compare against -- a memcmp
  // of the storage, not a re-read of the numbers this file already believes.
  using TileBytes = std::vector<std::pair<TileCoord, std::vector<uint16_t>>>;
  auto snapshotBytes = [](const PigmentTileStore& store) {
    TileBytes out;
    for (const auto& [coord, tile] : store)
      out.emplace_back(coord,
                       std::vector<uint16_t>(tile.data(), tile.data() + PigmentTile::kTexelCount));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      return a.first.y != b.first.y ? a.first.y < b.first.y : a.first.x < b.first.x;
    });
    return out;
  };

  auto makePigmentDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "pigment selection");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                 makePigmentLayer("Pigment 0")));
    return od;
  };

  // ======================================================================
  // A. A dab through an engaged selection deposits NOTHING outside it
  // ======================================================================
  //
  // **This is the assertion that fails against the code this section replaced**,
  // and it is the whole point of the section. A fractional left edge, so one
  // column of texels is PARTIALLY selected -- an in-or-out boundary would pass
  // against a version that treated the selection as a bitmask (PRD E2).
  //
  // The excluded texels are inside the dab's footprint by construction: the tip
  // is a radius-30 hard disc centred at (64,64), so `dabCoverage()` is exactly
  // 1.0 at (50,64) and (60,64), fourteen and four texels from the centre. What
  // stops the paint there is the selection and nothing else.
  Selection sel = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
  const float partial = selectionCoverageAt(&sel, PixelCoord{64, 64});
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    const BrushTip t = discTip(30.0f, 0.5f, kBlue);

    check(dabCoverage(t, 14.0f, 0.0f) == 1.0f && dabCoverage(t, 4.0f, 0.0f) == 1.0f,
          "A. premise: the dab's own falloff covers both excluded texels fully, so what "
          "stops the paint there can only be the selection");

    depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);

    check(readAt(store, 50, 64) == PigmentTexel{} && readAt(store, 60, 64) == PigmentTexel{},
          "A. a texel the dab covers and the selection does not is UNTOUCHED -- mass and "
          "latent both exactly zero, the value an unwritten texel holds, so there is nothing "
          "to tell it from paper");
    check(readAt(store, 80, 64).mass > 0.0f,
          "A. and the same dab did land inside the ants -- or a deposit that wrote nothing "
          "anywhere would pass the assertion above");
    std::printf("  [measured] selection coverage at the partial column: %.6f\n",
                static_cast<double>(partial));
    check(partial > 0.0f && partial < 1.0f,
          "A. the boundary column is PARTIALLY selected, not in-or-out -- PRD E2 stores "
          "antialiased coverage, and a bitmask would make every assertion below vacuous");
  }

  // ======================================================================
  // B. A null Selection means NO RESTRICTION, through this loop's own branch
  // ======================================================================
  //
  // core/SelectionMask.hpp requires every hoisted per-texel loop to repeat the
  // "null Selection means 1.0" branch itself, and warns that a perturbation
  // inverting one copy leaves the others right. `brush/Deposit`'s loop is one
  // such copy; this drives it. A fix that read null as "selects nothing" -- the
  // inverse, and the one that inverts the editor -- fails here.
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    const BrushTip t = discTip(30.0f, 0.5f, kBlue);
    depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, nullptr, nullptr);
    check(readAt(store, 64, 64).mass > 0.0f && readAt(store, 50, 64).mass > 0.0f &&
              readAt(store, 60, 64).mass > 0.0f,
          "B. a NULL selection deposits everywhere in the footprint, including the two "
          "texels section A's engaged selection excluded -- absence is no restriction, not "
          "\"select nothing\"");
  }

  // ======================================================================
  // C. An ENGAGED selection with no tile at that coordinate selects NOTHING
  // ======================================================================
  //
  // The other end of the same rule, and the one an optimisation can silently
  // break by treating "no tile here" as "no restriction here". Asserted on tile
  // count rather than on texel values, because the requirement is stronger than
  // "wrote zero": nothing may be allocated, or an eraser-shaped hole in the
  // selection would cost 224 KiB a tile.
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    const BrushTip t = discTip(30.0f, 0.5f, kBlue);
    const DepositCount c = depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &elsewhere,
                                      nullptr);
    check(store.occupiedTileCount() == 0 && c.tiles == 0 && c.texels == 0,
          "C. a dab entirely outside an ENGAGED selection allocates NO tiles and reports "
          "none -- an absent selection tile is \"selects nothing\", the exact inverse of a "
          "layer mask's absent tile");
  }

  // ======================================================================
  // D. A partially covered texel receives mass PROPORTIONAL to coverage
  // ======================================================================
  //
  // The rate half of brush/Deposit §4. One dab, flow 0.5, `dabCoverage()`
  // exactly 1, so the expected mass is `flow * sel` and the only inexactness
  // between that and the stored number is the one binary16 rounding at the
  // write -- hence kHalfRel and not a looser bound.
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    const BrushTip t = discTip(30.0f, 0.5f, kBlue);
    depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);

    const float onePass = readAt(store, 64, 64).mass;
    const float inside = readAt(store, 80, 64).mass;
    std::printf("  [measured] one flow-0.5 dab: coverage %.6f leaves mass %.6f; fully "
                "selected leaves %.6f (f16 bound %.3e)\n",
                static_cast<double>(partial), static_cast<double>(onePass),
                static_cast<double>(inside), static_cast<double>(kHalfRel * 0.5f));
    check(nearHalf(onePass, t.flow * partial),
          "D. one dab through a partially selected texel lays exactly `flow * coverage` of "
          "mass -- an antialiased selection edge comes out antialiased rather than stepped");
    check(nearHalf(inside, t.flow),
          "D. and a fully selected texel takes the whole `flow` -- the gate costs nothing "
          "where the selection covers everything");
  }

  // ======================================================================
  // E. The scrubbing walk-through, which `kMaxMass` alone does NOT stop
  // ======================================================================
  //
  // The derivation this section exists to check, run against both models.
  //
  // Gate only the rate and mass accumulates **linearly**: after N dabs a texel
  // holds `min(N * flow * cov * sel, kMaxMass)`, which reaches 1 exactly for any
  // `sel > 0` in `ceil(1 / (flow*cov*sel))` dabs. That is strictly worse than
  // the RGB route's version of the same defect, where `A' = A + w(1-A)`
  // approaches the ceiling asymptotically and never arrives. Both numbers are
  // computed here on the identical inputs and printed.
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    const BrushTip t = discTip(30.0f, 0.35f, kBlue);  // 0.35 is BrushTip::flow's own default
    for (int i = 0; i < 41; ++i)
      depositDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);

    const float scrubbed = readAt(store, 64, 64).mass;
    const float cap = stored(kMaxMass * partial);
    // The rejected model, on the identical numbers: mass per dab with no cap of
    // its own, run to the same 41 dabs.
    float rateOnly = 0.0f;
    int dabsToSaturate = 0;
    for (int i = 0; i < 41; ++i) {
      rateOnly = std::min(rateOnly + t.flow * partial, kMaxMass);
      if (rateOnly >= kMaxMass && dabsToSaturate == 0) dabsToSaturate = i + 1;
    }
    std::printf("  [measured] coverage %.6f, 41 dabs: bounded model reaches %.6f (cap "
                "%.6f); rate-only model reaches %.6f and saturated at dab %d\n",
                static_cast<double>(partial), static_cast<double>(scrubbed),
                static_cast<double>(cap), static_cast<double>(rateOnly), dabsToSaturate);
    check(scrubbed == cap,
          "E. 41 dabs through a partially selected texel reach exactly `kMaxMass * coverage` "
          "and no further, at ZERO tolerance -- the cap writes that float itself, so the "
          "stored half is its binary16 image and nothing else");
    check(rateOnly == kMaxMass && dabsToSaturate > 0 && dabsToSaturate <= 6,
          "E. and the rate-only model saturates OUTRIGHT within six dabs on the same inputs "
          "-- at 0.25-radius spacing that is 1.5 radii of travel, so one ordinary pass would "
          "delete a feathered edge rather than soften it");
    check(readAt(store, 80, 64).mass == kMaxMass,
          "E. a FULLY selected texel still reaches kMaxMass under the same scrubbing -- the "
          "cap is the coverage, not a blanket reduction");
  }

  // ======================================================================
  // F. A deposit never REMOVES mass, and the gate is bit-identical at sel == 1
  // ======================================================================
  //
  // Two clauses of `depositTexel()`'s cap, each of which is a real input rather
  // than a defensive line.
  {
    // (i) A texel already thicker than the selection allows keeps what it has.
    // Without this clause the brush would take paint OFF wherever a selection is
    // thinner than the paint under it, which is the most alarming way a gate can
    // be wrong -- a brush that erases.
    PigmentTexel thick;
    thick.latent = kRed;
    thick.mass = 0.9f;
    const PigmentTexel dabbed = depositTexel(thick, kBlue, 0.25f, 0.5f);
    check(dabbed.mass == thick.mass,
          "F. a deposit through a HALF-engaged selection onto a texel already at mass 0.9 "
          "leaves the mass exactly where it was -- a deposit must never remove paint, and "
          "the cap is a ceiling rather than an assignment");
    check(!(dabbed.latent == thick.latent),
          "F. and the hue still moves there -- the selection bounds how much paint is "
          "present, not which paint it is; bounding the hue too would need the pre-stroke "
          "latent stored per texel");

    // (ii) sel == 1 is the identity, asserted on the raw half words rather than
    // on any reading of them: a document with no selection, --pigment-stroke-demo
    // and the `canvas` golden reference must deposit the identical floats they
    // did before the gate existed.
    OpenDocument a = makePigmentDoc(256, 256);
    OpenDocument b = makePigmentDoc(256, 256);
    Selection all = selectAll(256, 256);
    const BrushTip t = discTip(24.0f, 0.4f, kBlue);
    const std::vector<Vec2> path = {{40.0f, 40.0f}, {60.0f, 55.0f}, {90.0f, 70.0f},
                                    {120.0f, 100.0f}};
    depositDabs(*a.document.layers[1].pigmentTiles, t, path, 256, 256, nullptr);
    depositDabs(*b.document.layers[1].pigmentTiles, t, path, 256, 256, &all);
    check(snapshotBytes(*a.document.layers[1].pigmentTiles) ==
              snapshotBytes(*b.document.layers[1].pigmentTiles),
          "F. a null selection and a Select All deposit BIT-IDENTICAL tiles -- the two "
          "spellings of \"no restriction\" agree, and the gate costs the unselected document "
          "not one rounding");
  }

  // ======================================================================
  // G. The selection reaches the deposit through the SESSION, not only the
  //    free function
  // ======================================================================
  //
  // The defect was never in the arithmetic: it was that
  // `StrokeSession::depositPending()` had nothing to pass. So the end-to-end
  // path is asserted as well as the module, with the texels outside the ants
  // compared as **raw half words** -- what a brush destroys outside a selection
  // is exactly as invisible as what an eraser does, and one undo step covers the
  // whole stroke either way.
  {
    OpenDocument od = makePigmentDoc(256, 256);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    // Something already on the layer OUTSIDE the selection, so "untouched" is a
    // comparison against real bytes rather than against emptiness.
    PigmentTexel existing;
    existing.latent = kRed;
    existing.mass = 0.5f;
    fillRect(store, 20, 20, 60, 110, existing);

    od.selection = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    setActiveLayer(od, 1);
    const size_t entries = od.history.entries().size();

    BrushTip t = discTip(20.0f, 0.5f, kBlue);
    StrokeSession s;
    std::string error;
    check(s.begin(od, od.activeLayer, t, Tool::Brush, &error) && error.empty() &&
              s.route() == StrokeRoute::CpuDeposit,
          "G. a brush stroke begins on the active Pigment layer and reports the pigment "
          "route");
    for (int i = 0; i < 24; ++i) s.addPoint(30.0f + 4.0f * static_cast<float>(i), 64.0f);
    s.end();

    check(readAt(store, 40, 64).latent == kRed && readAt(store, 40, 64).mass == stored(0.5f),
          "G. through the SESSION, a texel the stroke crossed but the selection excludes is "
          "bit-identical to what it held -- same latent, same mass, not one dab of blue");
    check(readAt(store, 100, 64).mass > 0.0f,
          "G. and the same stroke did paint inside the ants -- the premise the assertion "
          "above needs, or a stroke that deposited nothing would pass it");
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "brush stroke",
          "G. and it is still exactly ONE history entry -- the gate changed where paint "
          "lands, not how a stroke is recorded");

    // The whole rectangle the pre-fill covered, as bytes. Tile (0,0) is written
    // by this stroke (the selection's right half lives in it too), so the
    // comparison is per texel over the excluded rectangle rather than per tile.
    bool outsideIntact = true;
    for (int32_t y = 20; y <= 110; ++y)
      for (int32_t x = 20; x <= 60; ++x)
        if (!(readAt(store, x, y) == PigmentTexel{kRed, stored(0.5f)})) outsideIntact = false;
    check(outsideIntact,
          "G. every one of the 3731 pre-painted texels outside the marching ants is exactly "
          "as it was -- asserted over the rectangle, not sampled, because a gate that leaked "
          "at one edge would pass a spot check");
  }

  // ======================================================================
  // H. The routing table's Pigment rows, both of them
  // ======================================================================
  //
  // The Eraser row here used to be `None`, refused by name on one stated ground:
  // the pigment deposit had no selection parameter, so ADR-0007's mass reduction
  // could only have been built ungated. Sections A-G are that ground removed.
  {
    Layer pigment = makePigmentLayer("p");
    Layer lockedPigment = makePigmentLayer("lp");
    lockedPigment.locked = true;
    Layer hiddenPigment = makePigmentLayer("hp");
    hiddenPigment.visible = false;
    Layer storeless = makePigmentLayer("sp");
    storeless.pigmentTiles.reset();

    check(strokeRouteFor(Tool::Eraser, &pigment) == StrokeRoute::PigmentErase &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit &&
              strokeRouteFor(Tool::DryBrush, &pigment) == StrokeRoute::CpuDeposit,
          "H. a writable Pigment layer takes the brush on the deposit route and the eraser "
          "on a route of its own -- two tools, two answers, one layer");
    check(strokeRouteFor(Tool::Eraser, &lockedPigment) == StrokeRoute::None,
          "H. a LOCKED Pigment layer refuses the erase, exactly as it refuses the brush -- "
          "the lock is tested before the kind, so the message names the one thing a user can "
          "fix");
    check(strokeRouteFor(Tool::Eraser, &hiddenPigment) == StrokeRoute::PigmentErase,
          "H. a HIDDEN Pigment layer still erases -- visibility is a view decision, the same "
          "answer every other row gives");
    check(strokeRouteFor(Tool::Eraser, &storeless) == StrokeRoute::None,
          "H. a Pigment layer whose store was never allocated refuses -- a store to write is "
          "part of the question, not a precondition");
    check(std::string(strokeRouteName(StrokeRoute::PigmentErase)) == "pigment-erase" &&
              strokeRouteWritesLayer(StrokeRoute::PigmentErase),
          "H. the new route has a name of its own and answers the one predicate four call "
          "sites ask -- a route that removes paint still writes a layer, so the options bar "
          "accents it rather than greying it as though it went to the solver");
  }

  // ======================================================================
  // I. The eraser's per-stroke FLOOR, with the rejected model beside it
  // ======================================================================
  //
  // brush/PigmentErase §2. The accumulator holds `E`, the fraction removed, so
  // the closed form is `mass_final = mass_0 * (1 - strength)` at any number of
  // dabs. The rejected per-dab model -- `e = flow*cov*strength` with no memory --
  // erases, and looks like erasing, and is detectable only by scrubbing.
  {
    PigmentTileStore store;
    PigmentTexel paint;
    paint.latent = kRed;
    // 0.75 is exact in binary16 (1.1b x 2^-1) and so is half of it, so the
    // strength-0.5 floor below is an exact assertion rather than a lucky one:
    // scaling a normal half by 0.5 only decrements its exponent.
    paint.mass = 0.75f;
    fillRect(store, 32, 32, 96, 96, paint);

    const BrushTip t = discTip(16.0f, 1.0f, kBlue);
    PigmentEraseStroke stroke;
    stroke.begin(0.5f);
    for (int i = 0; i < 50; ++i)
      stroke.eraseDab(store, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);

    // The rejected model on the identical inputs.
    float perDab = 0.75f;
    for (int i = 0; i < 50; ++i) perDab *= (1.0f - t.flow * 1.0f * 0.5f);

    const float got = readAt(store, 64, 64).mass;
    std::printf("  [measured] 50 dabs at strength 0.5 on mass 0.7500: floor model leaves "
                "%.6f, per-dab model leaves %.3e, accumulator holds %.6f\n",
                static_cast<double>(got), static_cast<double>(perDab),
                static_cast<double>(stroke.strokeEraseAt(PixelCoord{64, 64})));
    check(got == 0.375f,
          "I. a strength-0.5 erase takes mass 0.75 to exactly 0.375 and STOPS, at zero "
          "tolerance -- the floor is mass_0 * (1 - strength), and halving a normal binary16 "
          "is exact");
    check(stroke.strokeEraseAt(PixelCoord{64, 64}) == 0.5f,
          "I. and the accumulator holds exactly the strength -- E is a fraction, so the "
          "floor needs no memory of what the texel held at pen-down");
    check(perDab < 0.01f * got,
          "I. while the rejected per-dab model grinds the same texel to under 1 % of the "
          "floor over the same 50 dabs -- run here so the good assertion cannot pass against "
          "the bad implementation");
    check(readAt(store, 64, 64).latent == kRed,
          "I. and the latent is BIT-IDENTICAL through all of it -- ADR-0007 and PRD F10 say "
          "mass down, Latent untouched, and a half-erased red must be thin red rather than "
          "pink");
    stroke.end();
    check(stroke.accumulatorTiles() == 0 && stroke.accumulatorBytes() == 0,
          "I. pen-up frees the accumulator -- 64 KiB a tile held for one drag and nothing "
          "afterwards");
  }

  // ======================================================================
  // J. A half-selected texel cannot be scrubbed past its coverage
  // ======================================================================
  //
  // brush/PigmentErase §2's second entry of the selection. With the rate gated
  // alone, `E' = E + w(1-E)` still converges to `strength` for any positive `w`
  // -- it just takes longer, so a feathered selection edge would come out hard
  // for a slow stroke and soft for a fast one. On an eraser that is paint
  // destroyed outside a selection the user drew to protect it, under one undo
  // step.
  {
    PigmentTileStore store;
    PigmentTexel paint;
    paint.latent = kRed;
    paint.mass = 1.0f;
    fillRect(store, 0, 0, 127, 127, paint);
    const PigmentTexel outsideBefore = readAt(store, 50, 64);

    const BrushTip t = discTip(30.0f, 1.0f, kBlue);
    PigmentEraseStroke stroke;
    stroke.begin(1.0f);
    for (int i = 0; i < 40; ++i)
      stroke.eraseDab(store, t, Vec2{64.0f, 64.0f}, 128, 128, &sel, nullptr);

    const float scrubbed = readAt(store, 64, 64).mass;
    const float removed = stroke.strokeEraseAt(PixelCoord{64, 64});
    // The rate-only model on the identical inputs: the cap is `strength` rather
    // than `strength * sel`, so it converges to 1 and the mass to 0.
    float rateOnly = 0.0f;
    for (int i = 0; i < 40; ++i) rateOnly += t.flow * partial * (1.0f - rateOnly);
    std::printf("  [measured] coverage %.6f, 40 dabs at strength 1: E reaches %.6f leaving "
                "mass %.6f; the rate-only model reaches E %.6f leaving %.3e\n",
                static_cast<double>(partial), static_cast<double>(removed),
                static_cast<double>(scrubbed), static_cast<double>(rateOnly),
                static_cast<double>(1.0f - rateOnly));
    check(removed == partial,
          "J. 40 dabs of a strength-1 eraser through a partially selected texel remove "
          "exactly its coverage and no more, at zero tolerance -- gating only the rate makes "
          "a bound a scrubbing eraser walks straight through");
    check(nearHalf(scrubbed, 1.0f - partial),
          "J. so the mass left standing is mass_0 * (1 - strength * coverage), within one "
          "binary16 rounding of the single write that produced it");
    check(rateOnly > 0.99f,
          "J. while the rate-only model has removed over 99 % of the same texel -- the "
          "feathered edge is gone, and what it took is under one undo step");
    check(readAt(store, 50, 64) == outsideBefore,
          "J. and a texel the selection excludes is BIT-IDENTICAL -- not merely undamaged "
          "but untouched, so its tile is not re-uploaded either");
  }

  // ======================================================================
  // K. Erasing to nothing leaves a texel that is NOT malformed -- by the rule
  //    this storage actually has
  // ======================================================================
  //
  // brush/PigmentErase §3, and the one place this module is deliberately the
  // opposite of brush/RgbErase. There, a fully erased texel must hold RGB 0 as
  // well as alpha 0, because `core::Tile` is premultiplied and `(colour, 0)` is
  // read by core/Composite as an additive glow with no coverage. A Pigment texel
  // is STRAIGHT: `projectPigmentTexel()` multiplies, so the composited answer is
  // four exact zeros whatever the hue.
  //
  // Asserted on the composited consequence rather than on the storage
  // convention, because that is the claim -- and then on the *next* deposit,
  // because the only way a stale hue could bite is by biasing what is painted
  // over it, and `depositTexel()`'s §1(ii) is written for exactly that.
  {
    PigmentTileStore store;
    PigmentTexel paint;
    paint.latent = kRed;
    paint.mass = 0.6f;
    fillRect(store, 32, 32, 96, 96, paint);

    const BrushTip t = discTip(16.0f, 1.0f, kBlue);
    PigmentEraseStroke stroke;
    stroke.begin(1.0f);
    stroke.eraseDab(store, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    stroke.end();

    const PigmentTexel emptied = readAt(store, 64, 64);
    const std::array<float, 4> projected = projectPigmentTexel(emptied);
    std::printf("  [measured] erased to nothing: mass %.6f, latent c0..c2 %.4f %.4f %.4f, "
                "composites to (%.6f %.6f %.6f a=%.6f)\n",
                static_cast<double>(emptied.mass), static_cast<double>(emptied.latent.c[0]),
                static_cast<double>(emptied.latent.c[1]), static_cast<double>(emptied.latent.c[2]),
                static_cast<double>(projected[0]), static_cast<double>(projected[1]),
                static_cast<double>(projected[2]), static_cast<double>(projected[3]));
    check(emptied.mass == 0.0f,
          "K. a strength-1 erase takes the mass to EXACTLY zero -- `finite * 0.0f` is zero, "
          "so a fully erased texel is not a very thin one");
    check(emptied.latent == kRed,
          "K. and it keeps its hue, deliberately: PRD F10 and ADR-0007 both say \"leaving "
          "the Latent untouched\", and there is no latent meaning \"no pigment\" -- "
          "Latent{}'s implied fourth Mixbox weight is 1, which is WHITE");
    check(projected == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "K. so it composites to four exact zeros, which is the pigment form of \"not "
          "malformed\" -- the straight-vs-premultiplied difference is why this rule is the "
          "inverse of brush/RgbErase's, not a relaxation of it");

    // The only way the stale hue could bite: biasing the next deposit.
    const PigmentTexel repainted = depositTexel(emptied, kBlue, 0.4f);
    check(repainted.latent == kBlue,
          "K. and the next deposit onto it takes the brush's latent OUTRIGHT, bit-for-bit -- "
          "brush/Deposit §1(ii)'s `m + dm == 0 -> w = 1` was written for this texel, so the "
          "stale hue is already unable to bias anything");
  }

  // ======================================================================
  // L. Erasing nothing costs nothing, and records nothing
  // ======================================================================
  //
  // brush/PigmentErase §4's asymmetry, on a 224 KiB tile -- an eraser dragged
  // across blank canvas that allocated per tile crossed would grow the document
  // faster than the RGB one does, and every such tile would then be reported
  // dirty and re-uploaded every frame of the drag.
  {
    OpenDocument od = makePigmentDoc(512, 512);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    setActiveLayer(od, 1);
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;

    BrushTip t = discTip(30.0f, 1.0f, kBlue);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string error;
    check(s.begin(od, od.activeLayer, t, Tool::Eraser, &error) && error.empty() &&
              s.route() == StrokeRoute::PigmentErase,
          "L. an erase begins on the active Pigment layer and reports the pigment erase "
          "route -- the row app/StrokeSession §1 used to refuse by name");
    for (int i = 0; i < 40; ++i) s.addPoint(40.0f + 10.0f * static_cast<float>(i), 200.0f);
    s.end();
    check(store.occupiedTileCount() == 0 && s.texelsWritten() == 0 && s.strokeTiles().empty(),
          "L. a 40-dab erase across blank canvas allocates NOT ONE 224 KiB tile and reports "
          "none -- an absent tile has nothing to lose, so it is skipped before getOrCreate()");
    check(od.history.entries().size() == entries && od.revision == rev,
          "L. and it records no history entry and moves no revision -- an undo step that "
          "undoes nothing is a worse defect than a missing one");
  }

  // ======================================================================
  // M. One Pigment erase stroke is ONE history entry, labelled "erase"
  // ======================================================================
  {
    OpenDocument od = makePigmentDoc(512, 512);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    PigmentTexel paint;
    paint.latent = kRed;
    paint.mass = 1.0f;
    fillRect(store, 40, 150, 470, 260, paint);
    setActiveLayer(od, 1);
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    const uint64_t structBefore = od.structuralRevision;

    BrushTip t = discTip(24.0f, 0.5f, kBlue);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string error;
    s.begin(od, od.activeLayer, t, Tool::Eraser, &error);
    for (int i = 0; i < 40; ++i) s.addPoint(60.0f + 10.0f * static_cast<float>(i), 200.0f);
    check(od.history.entries().size() == entries,
          "M. not one history entry has appeared mid-stroke, however many dabs it is");
    s.end();

    std::printf("  [measured] a %zu-dab pigment erase wrote %zu texels across %zu tiles of "
                "%zu KiB\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                sizeof(PigmentTile) / 1024);
    check(s.texelsWritten() > 0 && od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "erase",
          "M. a stroke of dozens of dabs is EXACTLY ONE entry, labelled \"erase\" and not "
          "\"brush stroke\" -- ADR-0005's stroke-granular undo, and PRD O2's panel is scanned "
          "by name");
    check(od.revision > revBefore && od.structuralRevision == structBefore,
          "M. it moved the content revision and not the structural one, so a stroke costs at "
          "most one journal write per interval rather than one per frame");
    check(readAt(store, 200, 200).mass < 1.0f && readAt(store, 200, 200).latent == kRed,
          "M. and the paint it passed over is thinner in mass with its latent untouched -- "
          "PRD section 7's own acceptance row for this gesture");
  }

  // ======================================================================
  // N. The erase loop's own copy of the two selection nulls
  // ======================================================================
  //
  // core/SelectionMask.hpp requires each hoisted per-texel loop to repeat both
  // branches and to be named where it does. brush/PigmentErase's loop is a
  // second such copy, and a perturbation inverting one copy leaves the others
  // right -- so it is driven here rather than inferred from section J.
  {
    PigmentTileStore store;
    PigmentTexel paint;
    paint.latent = kRed;
    paint.mass = 1.0f;
    fillRect(store, 0, 0, 127, 127, paint);

    const BrushTip t = discTip(20.0f, 1.0f, kBlue);
    PigmentEraseStroke nullSel;
    nullSel.begin(1.0f);
    nullSel.eraseDab(store, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    nullSel.end();
    check(readAt(store, 64, 64).mass == 0.0f && readAt(store, 50, 64).mass == 0.0f,
          "N. a NULL selection erases everywhere in the footprint, through this loop's own "
          "copy of that branch -- reading it as \"selects nothing\" would make the eraser "
          "stop working entirely");

    PigmentTileStore store2;
    fillRect(store2, 0, 0, 127, 127, paint);
    const TileBytes untouched = snapshotBytes(store2);
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    PigmentEraseStroke absent;
    absent.begin(1.0f);
    const DepositCount c =
        absent.eraseDab(store2, t, Vec2{64.0f, 64.0f}, 128, 128, &elsewhere, nullptr);
    absent.end();
    check(c.texels == 0 && c.tiles == 0 && snapshotBytes(store2) == untouched,
          "N. and an ENGAGED selection naming no tile here erases NOTHING, leaving the "
          "layer's bytes identical -- the tile is skipped before its coverage is read");
  }

  // The suite's own configuration claim: nothing in this section opens a file,
  // so every number above is the same in both NP_USE_OIIO builds.
  check(oiioBackendCompiledIn(),
        "config: this whole section reads no file, so its answers are configuration-"
        "independent -- and this build really does have the OIIO backend compiled in");

  std::printf("[selftest] pigment selection %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
