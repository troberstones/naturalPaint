#include "app/BrushStrokeDemo.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/StrokeSession.hpp"
#include "brush/BrushModel.hpp"
#include "brush/Deposit.hpp"
#include "brush/Grain.hpp"
#include "core/LayerOps.hpp"

namespace np {

namespace {

constexpr float kPi = 3.14159265358979f;

// The window shows the 1024 px document at 100% from its top-left corner, so
// only about (0, 0)-(736, 672) is on screen, and the navigator covers the
// lower right beyond that. Two columns of five rows fit inside it.
constexpr float kLeftX0 = 30.0f;
constexpr float kLeftX1 = 350.0f;
constexpr float kRightX0 = 400.0f;
constexpr float kRightX1 = 690.0f;
constexpr float kRowY[5] = {70.0f, 200.0f, 330.0f, 460.0f, 590.0f};

struct Pt {
  float x, y;
};

BrushState baseBrush(float r, float g, float b) {
  BrushState brush;
  brush.tool = Tool::Brush;
  brush.colorMode = ColorMode::Rgb;
  brush.rgb = {r, g, b};
  brush.model.tip.diameterPx = 34.0f;
  brush.model.tip.hardness = 0.8f;
  brush.model.tip.spacingPercent = 10.0f;
  return brush;
}

std::function<Pt(float)> line(float x0, float x1, float y) {
  return [=](float t) { return Pt{x0 + (x1 - x0) * t, y}; };
}

// Loops that run back over themselves: the horizontal swing outpaces the
// travel, so every loop overlaps the one before it.
std::function<Pt(float)> scribble(float x0, float x1, float y) {
  return [=](float t) {
    const float a = t * 2.0f * kPi * 4.0f;
    return Pt{x0 + 40.0f + (x1 - x0 - 80.0f) * t + 40.0f * std::sin(a),
              y + 22.0f * std::cos(a)};
  };
}

float fullPressure(float) { return 1.0f; }

}  // namespace

void buildBrushStrokeDemo(OpenDocument& od, const MixboxLut& lut) {
  recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                               makePigmentLayer("Wash / Build Up")));
  constexpr size_t kRgb = 0;
  const size_t pigment = od.document.layers.size() - 1;

  auto paint = [&](const char* name, size_t layer, BrushState brush, PigmentBuildup buildup,
                   const std::function<Pt(float)>& path,
                   const std::function<float(float)>& pressure, float holdSeconds) {
    StrokeSession stroke;
    std::string refusal;
    DynamicInputs beginInputs;
    beginInputs.pressure = pressure(0.0f);
    if (!stroke.begin(od, layer, brushTipFor(brush, lut, beginInputs), brush.tool, &refusal,
                      &brush.model, beginInputs, /*clone=*/nullptr, StabiliserParams{},
                      /*viewZoom=*/1.0f, &brush.native, buildup)) {
      std::fprintf(stderr, "[brush-stroke-demo] %s refused: %s\n", name, refusal.c_str());
      return;
    }
    constexpr int kSamples = 120;
    for (int s = 0; s <= kSamples; ++s) {
      const float t = static_cast<float>(s) / static_cast<float>(kSamples);
      DynamicInputs in;
      in.pressure = stroke.smoothPressure(pressure(t));
      stroke.setTip(brushTipFor(brush, lut, in), in);
      const Pt p = path(t);
      stroke.addPoint(p.x, p.y);
    }
    if (holdSeconds > 0.0f) {
      // A synthetic 60 Hz clock, so the pile is the same every launch.
      constexpr uint64_t kFrameNs = 16'666'667;
      const auto frames = static_cast<uint64_t>(holdSeconds * 60.0f);
      for (uint64_t f = 0; f <= frames; ++f) stroke.airbrushTick(1'000'000'000ull + f * kFrameNs);
    }
    stroke.end();
    std::printf("[brush-stroke-demo] %-32s layer %zu: %zu dabs, %zu texels\n", name, layer,
                stroke.dabCount(), stroke.texelsWritten());
  };

  // Left column, row 1-2: the tapers. Rows 1 and 2 differ only in the flow
  // switch and the minimum size.
  {
    BrushState b = baseBrush(0.10f, 0.14f, 0.40f);
    b.native.taperIn = BrushTaper{true, 120.0f, 0.0f, false};
    b.native.taperOut = BrushTaper{true, 120.0f, 0.0f, false};
    paint("taper size", kRgb, b, {}, line(kLeftX0, kLeftX1, kRowY[0]), fullPressure, 0.0f);
  }
  {
    BrushState b = baseBrush(0.10f, 0.14f, 0.40f);
    b.native.taperIn = BrushTaper{true, 120.0f, 25.0f, true};
    b.native.taperOut = BrushTaper{true, 120.0f, 25.0f, true};
    paint("taper size+flow, 25% min", kRgb, b, {}, line(kLeftX0, kLeftX1, kRowY[1]), fullPressure,
          0.0f);
  }

  // Left column, rows 3-4: the same scribble in the two Pigment buildup modes.
  // Wash keeps the strongest dab where the loops cross; Build Up piles up. A
  // low load, so one pass is far from the ceiling and the overlaps show.
  for (const auto& [name, mode, y] :
       {std::tuple{"pigment Wash", PigmentBuildupMode::Wash, kRowY[2]},
        std::tuple{"pigment Build Up", PigmentBuildupMode::BuildUp, kRowY[3]}}) {
    BrushState b = baseBrush(0.55f, 0.12f, 0.08f);
    b.model.tip.diameterPx = 26.0f;
    b.native.load = 0.12f;
    PigmentBuildup buildup;
    buildup.mode = mode;
    paint(name, pigment, b, buildup, scribble(kLeftX0, kLeftX1, y), fullPressure, 0.0f);
  }

  // Left column, row 5: size and flow both on pressure, with a size floor.
  {
    BrushState b = baseBrush(0.05f, 0.30f, 0.20f);
    b.model.tip.diameterPx = 48.0f;
    b.model.shape.enabled = true;
    b.model.shape.size.control = VarianceControl::PenPressure;
    b.model.shape.size.minimum = 0.15f;
    b.model.transfer.enabled = true;
    b.model.transfer.flow.control = VarianceControl::PenPressure;
    paint("pressure size+flow", kRgb, b, {}, line(kLeftX0, kLeftX1, kRowY[4]),
          [](float t) { return std::sin(t * kPi); }, 0.0f);
  }

  // Right column, row 1: a flat tip turned by the stroke's direction along a wave.
  {
    BrushState b = baseBrush(0.35f, 0.10f, 0.35f);
    b.model.tip.diameterPx = 40.0f;
    b.model.tip.roundness = 0.25f;
    b.model.shape.enabled = true;
    b.model.shape.angle.control = VarianceControl::Direction;
    paint("direction angle, roundness 25%", kRgb, b, {},
          [](float t) {
            return Pt{kRightX0 + (kRightX1 - kRightX0) * t,
                      kRowY[0] - 22.0f * std::sin(t * 4.0f * kPi)};
          },
          fullPressure, 0.0f);
  }

  // Right column, row 2: scatter, both axes, three dabs per position.
  {
    BrushState b = baseBrush(0.60f, 0.35f, 0.02f);
    b.model.tip.diameterPx = 14.0f;
    b.model.tip.spacingPercent = 90.0f;
    b.model.scatter.enabled = true;
    b.model.scatter.scatter.jitter = 2.0f;
    b.model.scatter.bothAxes = true;
    b.model.scatter.count = 3;
    paint("scatter x3, both axes", kRgb, b, {}, line(kRightX0, kRightX1, kRowY[1]), fullPressure,
          0.0f);
  }

  // Right column, row 3: a stripe paper, so its grain and its orientation are
  // both visible.
  {
    auto paper = std::make_shared<PaperField>();
    paper->width = 16;
    paper->height = 16;
    paper->height8.resize(16 * 16);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x)
        paper->height8[static_cast<size_t>(y * 16 + x)] = ((x + y) % 16) < 8 ? 255 : 40;
    BrushState b = baseBrush(0.10f, 0.10f, 0.10f);
    b.model.tip.diameterPx = 56.0f;
    b.model.texture.enabled = true;
    b.model.texture.pattern.id = "demo:stripes";
    b.model.texture.pattern.name = "Demo Stripes";
    b.model.texture.pattern.field = paper;
    b.model.texture.blend = CoverageBlend::Multiply;
    b.model.texture.depth = 1.0f;
    paint("texture stripes, Multiply", kRgb, b, {}, line(kRightX0, kRightX1, kRowY[2]),
          fullPressure, 0.0f);
  }

  // Right column, row 4: the Dual Brush, its second tip scattered.
  {
    BrushState b = baseBrush(0.02f, 0.25f, 0.55f);
    b.model.tip.diameterPx = 44.0f;
    b.model.dual.enabled = true;
    b.model.dual.tip.diameterPx = 18.0f;
    b.model.dual.tip.spacingPercent = 50.0f;
    b.model.dual.blend = CoverageBlend::Multiply;
    b.model.dual.scatter.enabled = true;
    b.model.dual.scatter.scatter.jitter = 0.8f;
    b.model.dual.scatter.bothAxes = true;
    b.model.dual.scatter.count = 2;
    b.dualTip = dualTipFromModel(b.model.dual);
    b.dualBlend = b.model.dual.blend;
    paint("dual brush, scattered", kRgb, b, {}, line(kRightX0, kRightX1, kRowY[3]), fullPressure,
          0.0f);
  }

  // Right column, row 5: Build-up, a short stroke and then the pen held still
  // for a second, so the end piles up.
  {
    BrushState b = baseBrush(0.45f, 0.05f, 0.10f);
    b.model.tip.diameterPx = 40.0f;
    b.model.tip.hardness = 0.3f;
    b.model.airbrush = true;
    b.native.load = 0.15f;
    paint("build-up, held 1 s", kRgb, b, {}, line(kRightX0, kRightX0 + 180.0f, kRowY[4]),
          fullPressure, 1.0f);
  }
}

}  // namespace np
