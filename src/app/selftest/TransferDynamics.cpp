#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/StrokeSession.hpp"
#include "brush/RgbDeposit.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Part 2 of the Phase C brief: Transfer Opacity/Flow (`PsTransfer::opacity`/
// `.flow`, `opVr`/`prVr`, `useAheadDynamics` on for 69 of 101 presets).
// Latched ONCE at pen-down -- Opacity into `rgb_`/`erase_`/`pigErase_`'s own
// accumulator members directly, Flow into `StrokeSession::transferFlowMul_`,
// applied fresh every dab -- see `StrokeSession::begin()`'s own comment for
// why the two are latched differently and `depositPending()`'s for why Flow
// specifically has to be.
//
// Two claims:
//
//   1. **Inert Transfer Variance paints bit-identically to no model at
//      all.** `varianceIsInert()` (brush/Variance.hpp) already exists for
//      exactly this question; this section checks the CONSEQUENCE of it
//      being true for a default-constructed `Variance` (Off, 0 jitter, 0
//      minimum) rather than merely trusting the predicate, by comparing a
//      real stroke's dab counts, texel counts, tile set AND stored pixels.
//   2. **A real `opVr`/`prVr` measurably changes painted opacity/flow.**
//      Constructed directly in C++ -- no real `.abr` file is needed, since
//      `Variance` is a plain, publicly-constructible struct -- with a
//      `PenPressure` control and no jitter, so the resolved multiplier is
//      exactly `hardwareInputs.pressure`, hand-derivable rather than merely
//      measured. A single, non-overlapping, hard-disc dab at its own exact
//      centre (`dabCoverage() == 1.0` there) makes both the flow weight and
//      the resulting stored alpha exact literals: 1.0 for the unscaled
//      case, 0.5 for the halved one, both exactly representable in the f16
//      tile `readPixel()` decodes -- so this compares stored bytes at ZERO
//      tolerance rather than to a derived bound.
bool runTransferDynamicsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto makeDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "transfer-dynamics");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("r")));
    return od;
  };

  // The exact pixel a dab lands on is not predicted here -- `brush/
  // StrokePath.cpp`'s own emitter walks `spacingPx` in from the first raw
  // sample along a subdivided Catmull-Rom curve, so the first dab's centre
  // is somewhere near `firstPoint.x + spacingPx`, not AT `firstPoint`
  // itself. Rather than guess that offset, this scans a window wide enough
  // to contain it and reports the maximum alpha found -- which, at
  // `hardness == 1.0`, is read off the FLAT CORE of whichever dab is under
  // it (`dabCoverage() == 1.0` there, exactly, not merely near the rim), so
  // the number this returns is exact regardless of the scan window's exact
  // bounds.
  auto maxAlphaInRegion = [](const TileStore& store, int32_t x0, int32_t y0, int32_t x1,
                             int32_t y1) {
    float best = 0.0f;
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
        if (tile == nullptr) continue;
        const float a = tile->readPixel(tileLocalOffset(PixelCoord{x, y}))[3];
        if (a > best) best = a;
      }
    }
    return best;
  };

  // Section 1's stronger form: every texel in the same window is bit-
  // identical between the two stores, not merely one probed coordinate --
  // so a difference anywhere near where these strokes actually paint cannot
  // hide from it.
  auto regionsEqual = [](const TileStore& a, const TileStore& b, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const Tile* ta = a.find(tileCoordAt(PixelCoord{x, y}));
        const Tile* tb = b.find(tileCoordAt(PixelCoord{x, y}));
        const std::array<float, 4> pa =
            ta != nullptr ? ta->readPixel(tileLocalOffset(PixelCoord{x, y}))
                         : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        const std::array<float, 4> pb =
            tb != nullptr ? tb->readPixel(tileLocalOffset(PixelCoord{x, y}))
                         : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
        if (pa != pb) return false;
      }
    }
    return true;
  };

  constexpr float kRadius = 24.0f;

  // A model whose base Size/Angle/Roundness/Scatter/Count match `baseTip()`
  // and are all inert, exactly `app/selftest/ScatterCount.cpp`'s own
  // `makeModel()` -- so any difference this section measures is provably
  // Transfer's alone, never a side effect of the OTHER four resolutions this
  // same `if (haveModel_)` block also runs.
  auto makeModel = [&]() {
    BrushModel model;
    model.tip.diameterPx = kRadius * 2.0f;
    model.tip.angleDeg = 0.0f;
    model.tip.roundness = 1.0f;
    model.scatter.count = 1;
    return model;
  };

  auto baseTip = [&](float opacity, float flow) {
    BrushTip tip;
    tip.radius = kRadius;
    tip.hardness = 1.0f;  // a hard disc: dabCoverage() == 1.0 exactly at
                          // and inside the centre, which is what makes
                          // section 2's literals exact rather than
                          // approximate
    tip.flow = flow;
    tip.opacity = opacity;
    tip.linearRgb = {0.6f, 0.3f, 0.1f};
    tip.spacing = 2.0f;  // in radii -- 48 px at this radius, comfortably
                         // past the 30 px point spacing below, so dabs never
                         // overlap and the FIRST dab always lands with
                         // strokeAlpha == 0 (fresh canvas) under it
    return tip;
  };

  // Six points 30 px apart -- `app/selftest/MultiplyFloor.cpp`'s own reason
  // for six rather than two (`brush/StrokePath.hpp`'s 0-/1-sample contract).
  // Collinear and horizontal, so the Catmull-Rom curve `emitAlongSegment()`
  // walks degenerates to an exact straight line (`mirror()`'s own comment:
  // "for collinear input this makes the extrapolated point collinear too")
  // -- every dab this stroke deposits lands on the SAME row, `kScanY`,
  // somewhere in `[kScanX0, kScanX1]`.
  constexpr float kStartX = 200.0f, kScanY = 260.0f;
  constexpr int32_t kScanX0 = 150, kScanX1 = 400;
  constexpr int32_t kScanY0 = 245, kScanY1 = 275;  // +/- 15 px of margin
                                                   // around kScanY; the path
                                                   // never actually leaves
                                                   // that row, but a probe
                                                   // that only worked by
                                                   // landing exactly on one
                                                   // integer texel row would
                                                   // be the fragile kind
  auto paint = [&](const BrushTip& tip, const BrushModel* model, const DynamicInputs& inputs) {
    OpenDocument od = makeDoc(512, 512);
    StrokeSession s;
    std::string err;
    const bool began = s.begin(od, 1, tip, Tool::Brush, &err, model, inputs);
    if (!began) return std::pair<OpenDocument, StrokeSession>{std::move(od), std::move(s)};
    for (int i = 0; i < 6; ++i) s.addPoint(kStartX + 30.0f * static_cast<float>(i), kScanY);
    s.end();
    return std::pair<OpenDocument, StrokeSession>{std::move(od), std::move(s)};
  };

  // ==========================================================================
  // 1. Inert Transfer Variance -- bit-identical to no model at all
  // ==========================================================================
  {
    // flow == opacity == 1.0, exactly section 2/3's own fixture: with a
    // hard disc and a non-overlapping first dab, `weight == 1.0` actually
    // REACHES the ceiling (`a1 = min(weight, cap) = cap`), which is what
    // makes a wrong resolved Opacity visible in the stored pixel at all --
    // at a lower flow the ceiling never binds for one pass and a resolution
    // bug in Opacity specifically would be invisible here. Found by
    // sabotage: an earlier `flow == 0.5` fixture here passed even with the
    // Opacity resolution deliberately broken, because 0.5 never approached
    // either the correct or the sabotaged ceiling.
    const BrushTip tip = baseTip(1.0f, 1.0f);
    const BrushModel model = makeModel();  // transfer.opacity/.flow are both
                                           // default-constructed -- inert

    auto [odNoModel, noModel] = paint(tip, nullptr, DynamicInputs{});
    auto [odModel, withModel] = paint(tip, &model, DynamicInputs{});

    check(noModel.dabCount() > 0, "transfer: (setup) the no-model stroke deposited at least one "
                                  "real dab -- the premise every comparison below rests on");
    check(noModel.dabCount() == withModel.dabCount() &&
              noModel.texelsWritten() == withModel.texelsWritten(),
          "transfer: an inert model's dabCount()/texelsWritten() match the no-model stroke "
          "exactly");
    check(noModel.strokeTiles() == withModel.strokeTiles(),
          "transfer: an inert model dirties the identical set of tiles");
    check(regionsEqual(*odNoModel.document.layers[1].rgbTiles,
                       *odModel.document.layers[1].rgbTiles, kScanX0, kScanY0, kScanX1, kScanY1),
          "transfer: EVERY stored pixel across the whole painted region is bit-identical between "
          "the inert-model stroke and the no-model one -- Opacity's resolved ceiling and Flow's "
          "resolved multiplier both round-trip to the untouched value (1.0) for an inert "
          "Variance, so neither route's arithmetic moves at all, anywhere the strokes actually "
          "painted");
  }

  // ==========================================================================
  // 2. A real opVr measurably changes painted opacity -- exactly 1.0 vs 0.5
  // ==========================================================================
  {
    // flow == 1.0 so the dab's own weight at its hard-disc centre is exactly
    // 1.0 (`dabCoverage() == 1.0` across the whole flat core at hardness ==
    // 1.0, `sel == 1.0`, no selection) -- large enough that Opacity's
    // ceiling BINDS in the halved case (0.5 < 1.0) and does not in the
    // unscaled one (`a1 = min(weight, cap) = min(1.0, 1.0) = 1.0`).
    const BrushTip tip = baseTip(/*opacity=*/1.0f, /*flow=*/1.0f);

    BrushModel modelFull = makeModel();
    modelFull.transfer.opacity.control = VarianceControl::PenPressure;
    // jitter/minimum stay 0 -- the resolved multiplier is EXACTLY
    // `hardwareInputs.pressure` (brush/Variance.cpp's own formula: m=0,
    // rj=1 since jitter<=0, c=clamp(pressure,0,1), result = 0 + 1*c = c).
    DynamicInputs fullPressure;
    fullPressure.hasPressure = true;
    fullPressure.pressure = 1.0f;  // resolved opacity = 1.0 * tip.opacity = 1.0

    BrushModel modelHalf = modelFull;
    DynamicInputs halfPressure;
    halfPressure.hasPressure = true;
    halfPressure.pressure = 0.5f;  // resolved opacity = 0.5 * tip.opacity = 0.5

    auto [odFull, sFull] = paint(tip, &modelFull, fullPressure);
    auto [odHalf, sHalf] = paint(tip, &modelHalf, halfPressure);

    const float aFull = maxAlphaInRegion(*odFull.document.layers[1].rgbTiles, kScanX0, kScanY0,
                                         kScanX1, kScanY1);
    const float aHalf = maxAlphaInRegion(*odHalf.document.layers[1].rgbTiles, kScanX0, kScanY0,
                                         kScanX1, kScanY1);
    std::printf("  [measured] opacity Jitter -- PenPressure 1.0 max stored alpha = %.6f, "
                "PenPressure 0.5 max stored alpha = %.6f\n",
                static_cast<double>(aFull), static_cast<double>(aHalf));

    check(aFull == 1.0f,
          "transfer: opacity -- PenPressure 1.0 resolves the ceiling to 1.0*tip.opacity == 1.0, "
          "and every fresh dab's own flat core reaches it exactly");
    check(aHalf == 0.5f,
          "transfer: opacity -- PenPressure 0.5 resolves the ceiling to 0.5*tip.opacity == 0.5, "
          "and every dab is capped there exactly, at zero tolerance (both are exact binary16 "
          "values)");
    check(aFull != aHalf,
          "transfer: opacity -- restated as the measured claim this section exists to make: a "
          "real opVr changes painted opacity");
  }

  // ==========================================================================
  // 3. A real prVr measurably changes painted flow -- exactly 1.0 vs 0.5
  // ==========================================================================
  {
    // opacity == 1.0 so the ceiling never binds in either case (weight <=
    // 1.0 always here) -- isolating Flow's own effect from Opacity's.
    const BrushTip tip = baseTip(/*opacity=*/1.0f, /*flow=*/1.0f);

    BrushModel modelFull = makeModel();
    modelFull.transfer.flow.control = VarianceControl::PenPressure;
    DynamicInputs fullPressure;
    fullPressure.hasPressure = true;
    fullPressure.pressure = 1.0f;  // transferFlowMul_ = 1.0 -> dabTip.flow = 1.0

    BrushModel modelHalf = modelFull;
    DynamicInputs halfPressure;
    halfPressure.hasPressure = true;
    halfPressure.pressure = 0.5f;  // transferFlowMul_ = 0.5 -> dabTip.flow = 0.5

    auto [odFull, sFull] = paint(tip, &modelFull, fullPressure);
    auto [odHalf, sHalf] = paint(tip, &modelHalf, halfPressure);

    const float aFull = maxAlphaInRegion(*odFull.document.layers[1].rgbTiles, kScanX0, kScanY0,
                                         kScanX1, kScanY1);
    const float aHalf = maxAlphaInRegion(*odHalf.document.layers[1].rgbTiles, kScanX0, kScanY0,
                                         kScanX1, kScanY1);
    std::printf("  [measured] flow Jitter -- PenPressure 1.0 max stored alpha = %.6f, "
                "PenPressure 0.5 max stored alpha = %.6f\n",
                static_cast<double>(aFull), static_cast<double>(aHalf));

    check(aFull == 1.0f,
          "transfer: flow -- PenPressure 1.0 resolves transferFlowMul_ to 1.0, so every fresh "
          "dab's weight (flow*coverage) is exactly 1.0 across its own flat core");
    check(aHalf == 0.5f,
          "transfer: flow -- PenPressure 0.5 resolves transferFlowMul_ to 0.5, halving every "
          "dab's weight to exactly 0.5, at zero tolerance");
    check(aFull != aHalf,
          "transfer: flow -- restated as the measured claim: a real prVr changes painted flow");
  }

  std::printf("[selftest] transfer dynamics %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
