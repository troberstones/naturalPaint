#include "app/selftest/Support.hpp"

#include <memory>

#include "app/DabLibrary.hpp"
#include "app/StrokeSession.hpp"
#include "brush/BrushModel.hpp"
#include "brush/Library.hpp"
#include "paint/Palette.hpp"

namespace np {

// Brush Settings edits `BrushModel`, so every panel has to be read where the
// stroke reads it. A panel written by an `.abr` still carries its values when it
// is switched off, which is why the gates are asserted with those values set.
bool runBrushPanelsLiveTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-78s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto makeDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "brush-panels");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));
    return od;
  };

  constexpr float kRadius = 24.0f;
  auto baseTip = [&]() {
    BrushTip tip;
    tip.radius = kRadius;
    tip.hardness = 0.5f;
    tip.flow = 0.6f;
    tip.spacing = 0.5f;
    return tip;
  };
  auto makeModel = [&]() {
    BrushModel model;
    model.tip.diameterPx = kRadius * 2.0f;
    model.tip.roundness = 1.0f;
    return model;
  };
  auto texels = [&](const BrushModel& model, const DynamicInputs& in) {
    OpenDocument od = makeDoc(512, 512);
    StrokeSession s;
    std::string err;
    if (!s.begin(od, 1, baseTip(), Tool::Brush, &err, &model, in)) return size_t{0};
    for (int i = 0; i < 6; ++i) s.addPoint(200.0f + 30.0f * static_cast<float>(i), 260.0f);
    s.end();
    return s.texelsWritten();
  };

  std::printf("  -- 1. a switched-off panel contributes nothing --\n");
  {
    const DynamicInputs none;
    const BrushModel inert = makeModel();
    BrushModel countOn = makeModel();
    countOn.scatter.enabled = true;
    countOn.scatter.count = 3;
    BrushModel countOff = countOn;
    countOff.scatter.enabled = false;
    const size_t tInert = texels(inert, none);
    const size_t tOn = texels(countOn, none);
    const size_t tOff = texels(countOff, none);
    std::printf("  [measured] texels: inert %zu, Count 3 on %zu, Count 3 off %zu\n", tInert, tOn,
                tOff);
    check(tInert > 0 && tOn == 3 * tInert,
          "panels: Scattering on with Count 3 lays three dabs a position (the control)");
    check(tOff == tInert, "panels: Scattering off, the same Count 3 lays one");

    DynamicInputs light;
    light.hasPressure = true;
    light.pressure = 0.25f;
    BrushModel sizeOn = makeModel();
    sizeOn.shape.enabled = true;
    sizeOn.shape.size.control = VarianceControl::PenPressure;
    BrushModel sizeOff = sizeOn;
    sizeOff.shape.enabled = false;
    const size_t tFull = texels(inert, light);
    const size_t tThin = texels(sizeOn, light);
    const size_t tSizeOff = texels(sizeOff, light);
    std::printf("  [measured] texels at pressure 0.25: inert %zu, Size on %zu, Size off %zu\n",
                tFull, tThin, tSizeOff);
    check(tThin < tFull,
          "panels: Shape Dynamics on, light pressure narrows the stroke (the control)");
    check(tSizeOff == tFull,
          "panels: Shape Dynamics off, the same pressure leaves the stroke full size");
  }

  std::printf("  -- 2. Texture is read off the panel for every tip --\n");
  {
    const MixboxLut noLut;
    auto paper = std::make_shared<PaperField>();
    paper->width = 2;
    paper->height = 2;
    paper->height8 = {0, 255, 255, 0};

    BrushState brush;
    brush.model.texture.enabled = true;
    brush.model.texture.pattern.id = "11111111-2222-3333-4444-555555555555";
    brush.model.texture.pattern.name = "test paper";
    brush.model.texture.pattern.field = paper;
    brush.model.texture.blend = CoverageBlend::Height;
    brush.model.texture.depth = 0.36f;

    BrushTip tip = brushTipFor(brush, noLut, 1.0f);
    check(tip.grain.enabled && tip.grain.field == paper && tip.grain.depth == 0.36f,
          "texture: the panel's paper and depth reach the tip");

    brush.model.texture.depth = 0.8f;
    brush.model.texture.scalePercent = 60.0f;
    tip = brushTipFor(brush, noLut, 1.0f);
    check(tip.grain.depth == 0.8f && tip.grain.scale == 60.0f / 100.0f,
          "texture: an edit to Depth and Scale reaches the next tip, not only a fresh import");

    brush.model.texture.enabled = false;
    tip = brushTipFor(brush, noLut, 1.0f);
    check(!tip.grain.enabled && tip.grain.field == nullptr,
          "texture: panel off, the tip paints naturalPaint's own Paper Grain (off here)");

    brush.model.texture.enabled = true;
    brush.model.texture.blend = CoverageBlend::LinearHeight;
    tip = brushTipFor(brush, noLut, 1.0f);
    check(tip.grain.field == nullptr,
          "texture: Linear Height has no formula, so the tip paints without the paper");
  }

  std::printf("  -- 3. the Dual Brush tip comes from its panel --\n");
  {
    PsDualBrush dual;
    dual.tip.diameterPx = 28.0f;
    dual.tip.spacingPercent = 25.0f;
    dual.tip.roundness = 0.5f;
    dual.tip.angleDeg = 30.0f;
    dual.tip.hardness = 0.0f;
    auto bitmap = std::make_shared<BrushTipBitmap>();
    bitmap->width = 1;
    bitmap->height = 1;
    dual.tip.dab.id = "abr:test";
    dual.tip.dab.bitmap = bitmap;

    check(dualTipFromModel(dual) == nullptr, "dual: panel off builds no second tip");
    dual.enabled = true;
    const auto second = dualTipFromModel(dual);
    check(second != nullptr && second->radius == 14.0f && second->spacing == 0.5f &&
              second->roundness == 0.5f && second->angle == 30.0f && second->bitmap == bitmap,
          "dual: 28 px at 25% spacing is radius 14 and 0.5 radii, carrying the picked bitmap");
    check(second != nullptr && second->hardness == 1.0f,
          "dual: the second tip is stamped hard, as the import always has");
    dual.blend = CoverageBlend::LinearHeight;
    check(dualTipFromModel(dual) == nullptr, "dual: Linear Height builds no second tip");

    BrushLibrary lib;
    BrushPreset saved;
    saved.name = "saved dual";
    saved.model.dual.enabled = true;
    saved.model.dual.tip.diameterPx = 28.0f;
    saved.model.dual.blend = CoverageBlend::ColorBurn;
    lib.presets.push_back(saved);
    DabLibrary dabs;
    resolveDabIds(lib, dabs, nullptr);
    check(lib.presets[0].dualTip != nullptr && lib.presets[0].dualTip->radius == 14.0f &&
              lib.presets[0].dualBlend == CoverageBlend::ColorBurn,
          "dual: a saved preset reloads with its second tip, rebuilt from the panel");
  }

  return ok;
}

}  // namespace np
