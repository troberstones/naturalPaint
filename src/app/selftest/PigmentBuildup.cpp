#include "app/selftest/Support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "core/LayerOps.hpp"
#include "paint/Palette.hpp"

namespace np {

// ---------------------------------------------------------------------------
// brush/Deposit.hpp §1a -- how a pigment stroke builds up where it overlaps
// itself, on the Pigment and RGB routes: Build-up (each route's historical
// rule) or Wash (a per-stroke buffer holding the strongest dab laid at each
// texel, applied over the layer as it was at pen-down).
//
// Reported from a tablet against Photoshop: "the overall buildup feels odd on
// overlaps"; of a hard per-stroke clamp, "it feels unnatural"; and of Wash
// easing toward opacity, that it "still builds up on overlapping strokes".
// ---------------------------------------------------------------------------
bool runPigmentBuildupTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Hand-built rather than palette-derived: an unloaded `MixboxLut` maps two
  // different palette entries to the same latent, which would silently turn
  // every hue assertion below into a comparison of one colour with itself.
  const Latent kBlue{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
  const Latent kRed{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};

  const auto tip = [](float radius, float hardness, float flow, const Latent& z) {
    BrushTip t;
    t.radius = radius;
    t.hardness = hardness;
    t.flow = flow;
    t.pigment = z;
    return t;
  };
  const auto makePigmentDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "buildup");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                 makePigmentLayer("Pigment")));
    return od;
  };
  const auto texelAt = [](const PigmentTileStore& store, int32_t x, int32_t y) {
    const PigmentTile* t = store.find(tileCoordAt(PixelCoord{x, y}));
    return t != nullptr ? t->readTexel(tileLocalOffset(PixelCoord{x, y})) : PigmentTexel{};
  };
  const auto worstMass = [](const PigmentTileStore& store) {
    float worst = 0.0f;
    for (const auto& [coord, tile] : store) {
      (void)coord;
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          worst = std::max(worst, tile.readTexel(PixelCoord{x, y}).mass);
    }
    return worst;
  };
  // Mass is stored as binary16, whose ulp at 0.4 is 2^-12 = 2.44e-4, so a
  // ceiling read back from the layer holds to a few ulps rather than exactly.
  constexpr float kF16Slack = 1.0e-3f;
  PigmentBuildup wash;
  wash.mode = PigmentBuildupMode::Wash;

  // ======================================================================
  // 1. The Wash rule on its own
  // ======================================================================
  {
    // The strongest dab, not a total: a spot crossed again by a dab no
    // stronger than one already there does not change at all.
    const float once = washAmount(0.0f, 0.18f, 0.4f);
    float again = once;
    for (int k = 0; k < 20; ++k) again = washAmount(again, 0.18f, 0.4f);
    std::printf("  [measured] a rate-0.18 dab toward 0.4: %.6f once, %.6f after twenty more\n",
                once, again);
    check(once == std::min(0.18f, 1.0f) * 0.4f && again == once,
          "wash: a spot crossed twenty more times by the same dab is exactly as dark as after "
          "one -- nothing builds");
    const float strong = washAmount(once, 0.5f, 0.4f);
    check(strong == 0.5f * 0.4f && washAmount(strong, 0.18f, 0.4f) == strong,
          "wash: a stronger dab raises a spot to its own strength, and a weaker one never "
          "lowers it");
    check(washAmount(0.0f, 5.0f, 0.4f) == 0.4f,
          "wash: no rate above one takes a spot past the stroke's opacity");
    const float abc = washAmount(washAmount(washAmount(0.0f, 0.1f, 0.7f), 0.5f, 0.7f), 0.3f, 0.7f);
    const float cba = washAmount(washAmount(washAmount(0.0f, 0.3f, 0.7f), 0.5f, 0.7f), 0.1f, 0.7f);
    check(abc == cba, "wash: the same dabs in any order reach the same amount, bit for bit");
  }

  // ======================================================================
  // 2. Build-up is the route's historical rule, whatever else is supplied
  // ======================================================================
  {
    const auto paint = [&](PigmentBuildup buildup, bool withWash, bool withBefore) {
      OpenDocument od = makePigmentDoc(128, 128);
      PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
      depositDab(store, tip(30.0f, 0.5f, 0.6f, kBlue), Vec2{64.0f, 64.0f}, 128, 128, nullptr,
                 nullptr);
      const PigmentTileStore before = store;
      WashStroke w;
      if (withBefore) w.before = &before;
      for (int i = 0; i < 10; ++i)
        depositDab(store, tip(12.0f, 0.4f, 0.18f, kRed),
                   Vec2{40.0f + 5.0f * static_cast<float>(i), 60.0f + static_cast<float>(i)},
                   128, 128, nullptr, nullptr, buildup, withWash ? &w : nullptr);
      std::vector<PigmentTexel> texels;
      for (int32_t y = 0; y < 128; ++y)
        for (int32_t x = 0; x < 128; ++x) texels.push_back(texelAt(store, x, y));
      return texels;
    };
    const auto same = [](const std::vector<PigmentTexel>& a, const std::vector<PigmentTexel>& b) {
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].mass != b[i].mass || a[i].latent.c != b[i].latent.c ||
            a[i].latent.res != b[i].latent.res)
          return false;
      return true;
    };
    const auto plain = paint(PigmentBuildup{}, false, false);
    check(same(plain, paint(PigmentBuildup{}, true, true)),
          "build-up: handing the deposit a wash buffer and a snapshot changes nothing, bit "
          "for bit -- the mode, not the arguments, decides");
    check(same(plain, paint(wash, true, false)),
          "wash with no snapshot falls back to build-up bit for bit, rather than washing "
          "against the live layer");
    check(!same(plain, paint(wash, true, true)),
          "...and wash with one really is a different rule -- the fixture discriminates");
  }

  // ======================================================================
  // 3. Wash meets the paint already there ONCE
  // ======================================================================
  //
  // Forty red dabs in one place over a blue texel. Build-up mixes every dab
  // into what the last one left, so the blue is driven out however low the
  // opacity; Wash lays one glaze of its amount on the blue as it was.
  {
    const auto stack = [&](PigmentBuildup buildup, float opacity) {
      OpenDocument od = makePigmentDoc(128, 128);
      PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
      depositDab(store, tip(30.0f, 1.0f, 0.5f, kBlue), Vec2{64.0f, 64.0f}, 128, 128, nullptr,
                 nullptr);
      const PigmentTileStore before = store;
      WashStroke w;
      w.before = &before;
      buildup.opacity = opacity;
      for (int i = 0; i < 40; ++i)
        depositDab(store, tip(12.0f, 1.0f, 0.18f, kRed), Vec2{64.0f, 64.0f}, 128, 128, nullptr,
                   nullptr, buildup, &w);
      return std::make_pair(texelAt(before, 64, 64), texelAt(store, 64, 64));
    };
    const auto [blue, washed] = stack(wash, 0.3f);
    const auto [blue2, builtUp] = stack(PigmentBuildup{}, 0.3f);
    (void)blue2;
    float amount = 0.0f;
    for (int i = 0; i < 40; ++i) amount = washAmount(amount, 0.18f, 0.3f);
    const PigmentTexel once = depositTexel(blue, kRed, amount, 1.0f);
    float offOnce = std::fabs(washed.mass - once.mass);
    for (size_t i = 0; i < 3; ++i) offOnce = std::max(offOnce, std::fabs(washed.latent.c[i] - once.latent.c[i]));
    std::printf("  [measured] 40 red dabs over blue (mass %.3f): blue channel %.4f washed at "
                "opacity 0.3 vs %.4f built up; mass %.4f vs %.4f\n",
                blue.mass, washed.latent.c[2], builtUp.latent.c[2], washed.mass, builtUp.mass);
    check(offOnce < kF16Slack,
          "wash: the layer texel is one deposit of the stroke's amount onto the texel as it "
          "was at pen-down -- not forty");
    check(washed.latent.c[2] > builtUp.latent.c[2] + 0.2f,
          "wash: a low-opacity glaze leaves the colour underneath showing, where build-up "
          "drives it out");
    check(washed.mass >= blue.mass && washed.mass <= blue.mass + 0.3f + kF16Slack,
          "wash: it glazes on top of the paint there -- never removing any, never adding more "
          "than its opacity");
  }

  // ======================================================================
  // 4. Through the session, on both routes
  // ======================================================================
  {
    const auto brushFor = [](float opacity, float hardness, float diameter) {
      BrushState brush;
      brush.model.tip.diameterPx = diameter;
      brush.model.tip.hardness = hardness;
      brush.native.load = 0.18f;
      brush.opacity = opacity;
      return brush;
    };
    const auto makeRgbDoc = [](int32_t w, int32_t h) {
      OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "buildup-rgb");
      recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("RGB")));
      return od;
    };
    // What a painter sees at one texel: mass on a Pigment layer, alpha on RGB.
    const auto coverAt = [&](const Layer& layer, int32_t x, int32_t y) {
      if (layer.pigmentTiles.has_value()) return texelAt(*layer.pigmentTiles, x, y).mass;
      const Tile* t = layer.rgbTiles.has_value() ? layer.rgbTiles->find(tileCoordAt(PixelCoord{x, y}))
                                                 : nullptr;
      return t != nullptr ? t->readPixel(tileLocalOffset(PixelCoord{x, y}))[3] : 0.0f;
    };
    // The user's case: one zigzag stroke of a soft brush that crosses itself.
    // Returns the darkest texel along a stretch one pass covers, and the
    // crossing -- the crossing may not be darker than a single pass anywhere.
    const auto xStroke = [&](bool rgb, PigmentBuildup buildup, float opacity) {
      OpenDocument od = rgb ? makeRgbDoc(256, 256) : makePigmentDoc(256, 256);
      BrushState brush = brushFor(opacity, 0.3f, 40.0f);
      MixboxLut noLut;
      std::string error;
      StrokeSession session;
      session.begin(od, 1, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &error,
                    /*model=*/nullptr, DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{},
                    1.0f, &brush.native, buildup);
      const float pts[4][2] = {{40, 40}, {216, 216}, {216, 40}, {40, 216}};
      for (int s = 0; s < 3; ++s)
        for (int i = 0; i < 44; ++i) {
          const float f = static_cast<float>(i) / 44.0f;
          session.addPoint(pts[s][0] + (pts[s + 1][0] - pts[s][0]) * f,
                           pts[s][1] + (pts[s + 1][1] - pts[s][1]) * f);
        }
      session.addPoint(40.0f, 216.0f);
      session.end();
      const Layer& layer = od.document.layers[1];
      float single = 0.0f;
      for (int32_t y = 80; y <= 176; ++y) single = std::max(single, coverAt(layer, 216, y));
      return std::make_pair(single, coverAt(layer, 128, 128));
    };
    for (bool rgb : {false, true}) {
      const auto [builtSingle, builtCross] = xStroke(rgb, PigmentBuildup{}, 1.0f);
      const auto [washSingle, washCross] = xStroke(rgb, wash, 1.0f);
      std::printf("  [measured] %s zigzag, darkest single pass / crossing: build-up %.4f / "
                  "%.4f, wash %.4f / %.4f\n",
                  rgb ? "RGB" : "Pigment", builtSingle, builtCross, washSingle, washCross);
      check(builtCross > builtSingle + 0.1f,
            rgb ? "RGB zigzag: build-up is darker where the stroke crosses itself"
                : "Pigment zigzag: build-up is darker where the stroke crosses itself");
      check(washSingle > 0.05f && washCross <= washSingle + kF16Slack,
            rgb ? "RGB zigzag: wash is no darker at the crossing than a single pass"
                : "Pigment zigzag: wash is no darker at the crossing than a single pass");
    }
    const auto [halfSingle, halfCross] = xStroke(false, wash, 0.5f);
    std::printf("  [measured] Pigment zigzag, wash at opacity 0.5: %.4f / %.4f\n", halfSingle,
                halfCross);
    check(halfCross <= 0.5f * 0.18f + kF16Slack,
          "wash: a stroke is never darker than its load times its opacity");

    // The edge of a straight Wash stroke at the default spacing. A row of
    // discs kept by `max` scallops once per spacing; swept, the edge is flat.
    for (bool rgb : {false, true}) {
      OpenDocument od = rgb ? makeRgbDoc(256, 128) : makePigmentDoc(256, 128);
      BrushState brush = brushFor(1.0f, 0.3f, 40.0f);
      brush.native.load = 0.5f;
      MixboxLut noLut;
      std::string error;
      StrokeSession session;
      session.begin(od, 1, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &error, nullptr,
                    DynamicInputs{}, nullptr, StabiliserParams{}, 1.0f, &brush.native, wash);
      for (int i = 0; i <= 40; ++i) session.addPoint(30.0f + 5.0f * static_cast<float>(i), 64.3f);
      session.end();
      float worst = 0.0f;
      for (int dy : {10, 14, 17}) {
        float lo = 1.0f;
        float hi = 0.0f;
        for (int32_t x = 80; x < 180; ++x) {
          const float v = coverAt(od.document.layers[1], x, 64 + dy);
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
        if (hi > 0.0f) worst = std::max(worst, (hi - lo) / hi);
      }
      std::printf("  [measured] %s wash, straight stroke at 25%% spacing: worst edge ripple "
                  "%.2f%%\n",
                  rgb ? "RGB" : "Pigment", 100.0f * worst);
      check(worst < 0.01f, rgb ? "RGB wash: the stroke's edge is smooth, not scalloped per dab"
                               : "Pigment wash: the stroke's edge is smooth, not scalloped per dab");
    }

    // Separate strokes still layer, each by its OWN strength: the second pass
    // is lighter than the first, so a buffer carried over from the first
    // would lay the first stroke's amount again rather than its own.
    const auto passes = [&](bool rgb) {
      OpenDocument od = rgb ? makeRgbDoc(256, 128) : makePigmentDoc(256, 128);
      MixboxLut noLut;
      std::string error;
      StrokeSession session;
      std::array<float, 3> after{};
      const float opacities[2] = {0.8f, 0.4f};
      for (int p = 0; p < 2; ++p) {
        BrushState brush = brushFor(opacities[p], 1.0f, 24.0f);
        session.begin(od, 1, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &error,
                      /*model=*/nullptr, DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{},
                      1.0f, &brush.native, wash);
        for (int i = 0; i <= 12; ++i)
          session.addPoint(40.0f + static_cast<float>(i) * 8.0f, 64.0f);
        session.end();
        after[p + 1] = coverAt(od.document.layers[1], 100, 64);
      }
      return after;
    };
    const auto pig = passes(false);
    std::printf("  [measured] Pigment wash, stroke at opacity 0.8 then 0.4: %.5f -> %.5f\n",
                pig[1], pig[2]);
    check(pig[1] > 0.1f && std::fabs((pig[2] - pig[1]) - 0.5f * pig[1]) < 0.005f,
          "wash: a second, lighter stroke glazes its own amount on top -- each stroke starts "
          "with an empty buffer");
    const auto rgbPasses = passes(true);
    const float expectRgb = rgbPasses[1] + (1.0f - rgbPasses[1]) * 0.5f * rgbPasses[1];
    std::printf("  [measured] RGB wash, stroke at opacity 0.8 then 0.4: alpha %.5f -> %.5f "
                "(own amount over the first: %.5f)\n",
                rgbPasses[1], rgbPasses[2], expectRgb);
    check(rgbPasses[1] > 0.1f && std::fabs(rgbPasses[2] - expectRgb) < 0.005f,
          "RGB wash: a second, lighter stroke composites its own amount over the first");

    // The exit taper's repaint puts the stroke's tiles back and replays it,
    // which for Wash means washing against the same pen-down picture again.
    OpenDocument od = makePigmentDoc(256, 128);
    BrushState brush = brushFor(0.4f, 1.0f, 24.0f);
    brush.native.taperOut.on = true;
    brush.native.taperOut.lengthPx = 40.0f;
    MixboxLut noLut;
    std::string error;
    StrokeSession session;
    session.begin(od, 1, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &error, nullptr,
                  DynamicInputs{}, nullptr, StabiliserParams{}, 1.0f, &brush.native, wash);
    for (int i = 0; i <= 12; ++i) session.addPoint(40.0f + static_cast<float>(i) * 8.0f, 64.0f);
    session.end();
    const float taperedWorst = worstMass(*od.document.layers[1].pigmentTiles);
    std::printf("  [measured] wash with an exit taper, after the repaint: worst mass %.5f\n",
                taperedWorst);
    check(taperedWorst > 0.05f && taperedWorst <= 0.4f * 0.18f + kF16Slack,
          "wash: the exit taper's repaint lays the stroke again at the same strength");

    // What the Opacity slider greys itself out by.
    const StrokeRoute pigment = strokeRouteFor(Tool::Brush, &od.document.layers[1]);
    check(pigment == StrokeRoute::CpuDeposit &&
              !opacityReachesRoute(pigment, PigmentBuildup{}) && opacityReachesRoute(pigment, wash),
          "the Opacity slider is live on a Pigment layer exactly in Wash");
    check(opacityReachesRoute(StrokeRoute::RgbDeposit, PigmentBuildup{}) &&
              !opacityReachesRoute(StrokeRoute::Smudge, wash),
          "...and the other routes answer as the slider always did, whatever BUILDUP says");
  }

  // ======================================================================
  // 5. The RGB texel rule
  // ======================================================================
  {
    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 3> ink{0.2f, 0.3f, 0.8f};
    const RgbDepositStep first = depositRgbTexel(clear, ink, 0.0f, 0.3f, 1.0f, false, true);
    const RgbDepositStep washAgain =
        depositRgbTexel(first.premultiplied, ink, first.strokeAlpha, 0.3f, 1.0f, false, true);
    const RgbDepositStep builtAgain =
        depositRgbTexel(first.premultiplied, ink, first.strokeAlpha, 0.3f, 1.0f, false, false);
    std::printf("  [measured] RGB texel, a weight-0.3 dab twice: stroke alpha %.4f wash, %.4f "
                "build-up\n",
                washAgain.dabAlpha > 0.0f ? washAgain.strokeAlpha : first.strokeAlpha,
                builtAgain.strokeAlpha);
    check(first.strokeAlpha == 0.3f && !(washAgain.dabAlpha > 0.0f) &&
              builtAgain.strokeAlpha > first.strokeAlpha + 0.1f,
          "RGB wash: the same dab again changes nothing, where build-up adds to it");
    const RgbDepositStep stronger =
        depositRgbTexel(first.premultiplied, ink, first.strokeAlpha, 0.6f, 1.0f, false, true);
    check(stronger.strokeAlpha == 0.6f && std::fabs(stronger.premultiplied[3] - 0.6f) < 1.0e-5f,
          "RGB wash: a stronger dab lands the texel exactly at its own strength over what was "
          "there at pen-down");
  }

  // ======================================================================
  // 6. A Wash dab is the tip swept back to the previous dab
  // ======================================================================
  {
    BrushTip round = tip(20.0f, 0.3f, 1.0f, kBlue);
    BrushTip ellipse = round;
    ellipse.roundness = 0.4f;
    ellipse.angle = 30.0f;
    bool identical = true;
    float below = 0.0f;
    float above = 0.0f;
    const Vec2 sweep{-10.0f, 3.0f};
    for (const BrushTip* t : {&round, &ellipse})
      for (float dy = -26.0f; dy <= 26.0f; dy += 1.3f)
        for (float dx = -30.0f; dx <= 22.0f; dx += 1.3f) {
          if (sweptDabCoverage(*t, dx, dy, Vec2{}) != dabCoverage(*t, dx, dy)) identical = false;
          float sampled = 0.0f;
          for (int k = 0; k <= 400; ++k) {
            const float f = static_cast<float>(k) / 400.0f;
            sampled = std::max(sampled, dabCoverage(*t, dx - f * sweep.x, dy - f * sweep.y));
          }
          const float swept = sweptDabCoverage(*t, dx, dy, sweep);
          below = std::max(below, sampled - swept);
          above = std::max(above, swept - sampled);
        }
    std::printf("  [measured] swept vs densely sampled tip (round and a 30-degree ellipse): "
                "%.6f below, %.6f above\n",
                below, above);
    check(identical, "sweep: no sweep is the plain dab bit for bit, round and elliptical");
    check(below < 1.0e-5f && above < 2.0e-3f,
          "sweep: a swept tip covers each texel as its nearest position along the path does, "
          "for a round tip and a rotated ellipse");
  }

  std::printf("[selftest] pigment buildup %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
