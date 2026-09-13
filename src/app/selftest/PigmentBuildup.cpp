#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "paint/Palette.hpp"

namespace np {

// ---------------------------------------------------------------------------
// brush/Deposit.hpp §1a -- how a pigment stroke builds up where it overlaps
// itself: Build-up (every dab straight into the layer, the route's historical
// rule) or Wash (a per-stroke buffer that eases toward the stroke's opacity,
// applied to the layer as it was at pen-down).
//
// Reported from a tablet against Photoshop: "the overall buildup feels odd on
// overlaps", and then, of a hard per-stroke clamp, "it feels unnatural". Wash
// is Krita's indirect Wash mode on this route's quantities.
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
    // Approaches, never clamps: after k dabs of rate r the amount is exactly
    // the closed form, so a soft tip's lighter texels stay lighter rather than
    // being flattened into a plateau with a cliff at its edge.
    float s = 0.0f;
    for (int k = 0; k < 6; ++k) s = washAmount(s, 0.18f, 0.4f);
    const float closed = 0.4f * (1.0f - std::pow(0.82f, 6.0f));
    std::printf("  [measured] six rate-0.18 dabs toward 0.4: %.6f, closed form %.6f\n", s,
                closed);
    check(std::fabs(s - closed) < 1.0e-5f && s < 0.4f - 0.1f,
          "wash: an amount eases toward the ceiling by the closed form, and six dabs are "
          "still well short of it -- no corner where a clamp would bite");

    float many = 0.0f;
    bool neverOver = true;
    for (int k = 0; k < 200; ++k) {
      many = washAmount(many, 0.9f, 0.4f);
      if (many > 0.4f) neverOver = false;
    }
    check(neverOver && washAmount(0.0f, 5.0f, 0.4f) == 0.4f,
          "wash: no number of dabs and no rate above one takes an amount past its ceiling");

    // Order-independent, because what is left below the ceiling is a product
    // of (1 - rate) terms. That is what lets a stroke's crossings and its
    // passes arrive in any order and come out the same.
    const float abc = washAmount(washAmount(washAmount(0.0f, 0.1f, 0.7f), 0.5f, 0.7f), 0.3f, 0.7f);
    const float cba = washAmount(washAmount(washAmount(0.0f, 0.3f, 0.7f), 0.5f, 0.7f), 0.1f, 0.7f);
    check(std::fabs(abc - cba) < 1.0e-6f,
          "wash: the same dabs in a different order reach the same amount");
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
  // 4. Through the session: a self-crossing stroke, a second pass, the taper
  // ======================================================================
  {
    struct Stroke {
      PigmentBuildup buildup;
      float opacity = 1.0f;
      bool exitTaper = false;
    };
    const auto brushFor = [](float opacity, float hardness, float diameter) {
      BrushState brush;
      brush.model.tip.diameterPx = diameter;
      brush.model.tip.hardness = hardness;
      brush.native.load = 0.18f;
      brush.opacity = opacity;
      return brush;
    };
    // The user's case: one X-shaped stroke of a soft brush, read at the
    // crossing and at a point one pass covers.
    const auto xStroke = [&](PigmentBuildup buildup, float opacity) {
      OpenDocument od = makePigmentDoc(256, 256);
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
      const PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
      return std::make_pair(texelAt(store, 216, 128).mass, texelAt(store, 128, 128).mass);
    };
    const auto [builtSingle, builtCross] = xStroke(PigmentBuildup{}, 1.0f);
    const auto [washSingle, washCross] = xStroke(wash, 0.5f);
    std::printf("  [measured] X stroke, single pass / crossing: build-up %.4f / %.4f, wash at "
                "opacity 0.5 %.4f / %.4f\n",
                builtSingle, builtCross, washSingle, washCross);
    check(builtCross > 0.9f && washCross <= 0.5f + kF16Slack,
          "a self-crossing stroke: build-up fills the paper at the crossing, wash stays under "
          "the stroke's opacity");
    check(washSingle > 0.0f && washSingle < washCross,
          "...and wash still eases up where the stroke crosses itself rather than flattening "
          "one pass and the crossing into the same plateau");

    // ONE session for both passes, as the application holds it. Cleared per
    // stroke, the second pass lays the same amount again on top of the first
    // (mass doubles); a buffer carried over would start pass two part way to
    // its ceiling and land noticeably more.
    const auto passes = [&](int count) {
      OpenDocument od = makePigmentDoc(256, 128);
      BrushState brush = brushFor(0.4f, 1.0f, 24.0f);
      MixboxLut noLut;
      std::string error;
      StrokeSession session;
      for (int p = 0; p < count; ++p) {
        session.begin(od, 1, brushTipFor(brush, noLut, 1.0f), Tool::Brush, &error,
                      /*model=*/nullptr, DynamicInputs{}, /*clone=*/nullptr, StabiliserParams{},
                      1.0f, &brush.native, wash);
        for (int i = 0; i <= 12; ++i)
          session.addPoint(40.0f + static_cast<float>(i) * 8.0f, 64.0f);
        session.end();
      }
      return texelAt(*od.document.layers[1].pigmentTiles, 100, 64).mass;
    };
    const float one = passes(1);
    const float two = passes(2);
    std::printf("  [measured] wash at opacity 0.4, one pass vs two: %.5f -> %.5f\n", one, two);
    check(one > 0.05f && std::fabs(two - 2.0f * one) < 0.01f,
          "wash: a second stroke glazes the same amount again on top of the first -- the "
          "session starts each stroke with an empty buffer");

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
    check(taperedWorst > 0.1f && taperedWorst <= 0.4f + kF16Slack,
          "wash: the exit taper's repaint lays the stroke again under the same ceiling");

    // What the Opacity slider greys itself out by.
    const StrokeRoute pigment = strokeRouteFor(Tool::Brush, &od.document.layers[1]);
    check(pigment == StrokeRoute::CpuDeposit &&
              !opacityReachesRoute(pigment, PigmentBuildup{}) && opacityReachesRoute(pigment, wash),
          "the Opacity slider is live on a Pigment layer exactly in Wash");
    check(opacityReachesRoute(StrokeRoute::RgbDeposit, PigmentBuildup{}) &&
              !opacityReachesRoute(StrokeRoute::Smudge, wash),
          "...and the other routes answer as the slider always did, whatever BUILDUP says");
  }

  std::printf("[selftest] pigment buildup %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
