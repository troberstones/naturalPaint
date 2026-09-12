#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "paint/Palette.hpp"

namespace np {

// ---------------------------------------------------------------------------
// brush/Deposit.hpp §1a -- how a pigment deposit builds up where a stroke
// overlaps ITSELF. Two rules, independently switchable, both off by default:
// `saturating` (each overlap adds a share of what is still empty) and
// `strokeCeiling` (`opacity` is the most mass one stroke may lay at a texel).
//
// Reported from a tablet against a Photoshop reference: "the overall buildup
// feels odd on overlaps." Linear accumulation gives a single pass a dark core
// and light rims -- a texel under the middle of a stroke is covered by more
// dabs than one under its edge -- and makes a self-crossing disproportionately
// darker than either pass.
// ---------------------------------------------------------------------------
bool runPigmentBuildupTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Hand-built rather than palette-derived: every claim in this file is about
  // the MASS rule, and an unloaded `MixboxLut` maps two different palette
  // entries to the same latent -- which silently turns the hue assertion below
  // into a comparison of one colour with itself. Two latents that simply
  // differ are exactly what these assertions need.
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

  // ======================================================================
  // 1. Both rules off is §1's rule, bit for bit
  // ======================================================================
  //
  // This is what lets the modes be runtime toggles rather than a fork: with a
  // default `MassRule` the arithmetic must be the same floats the route stored
  // before §1a existed, for every mass, every dab and every selection
  // coverage -- including the two clauses §4 argues (never below the mass
  // already stored, never above `kMaxMass`).
  {
    const float masses[] = {0.0f, 0.1f, 0.5f, 0.9f, 1.0f};
    const float deltas[] = {0.01f, 0.3f, 2.0f};
    const float sels[] = {1.0f, 0.5f, 0.25f};
    bool identical = true;
    for (float m : masses)
      for (float dm : deltas)
        for (float sel : sels) {
          PigmentTexel dst;
          dst.latent = kRed;
          dst.mass = m;
          float cap = kMaxMass * sel;
          if (cap < m) cap = m;
          if (cap > kMaxMass) cap = kMaxMass;
          const float want = (m + dm) < cap ? (m + dm) : cap;
          if (depositTexel(dst, kBlue, dm, sel).mass != want) identical = false;
        }
    check(identical,
          "buildup off: the mass rule is min(m + dm, cap) bit for bit, over every mass, dab "
          "and selection coverage -- the modes are free when off");
  }

  // ======================================================================
  // 2. Saturating: the first touch is unchanged, the overlaps diminish
  // ======================================================================
  {
    MassRule sat;
    sat.saturating = true;

    PigmentTexel bare;
    bare.mass = 0.0f;
    check(depositTexel(bare, kBlue, 0.18f, 1.0f, sat).mass ==
              depositTexel(bare, kBlue, 0.18f, 1.0f).mass,
          "saturating: on bare paper it lays exactly what the linear rule lays -- a stroke's "
          "first touch is untouched and only its overlaps differ");

    // Ten dabs of the user's own load at one texel, which is roughly what the
    // middle of one pass of a stroke receives at the default spacing.
    PigmentTexel lin;
    PigmentTexel sam;
    float firstLin = 0.0f;
    float firstSat = 0.0f;
    for (int i = 0; i < 10; ++i) {
      const float beforeLin = lin.mass;
      const float beforeSat = sam.mass;
      lin = depositTexel(lin, kBlue, 0.18f, 1.0f);
      sam = depositTexel(sam, kBlue, 0.18f, 1.0f, sat);
      if (i == 1) {
        firstLin = lin.mass - beforeLin;
        firstSat = sam.mass - beforeSat;
      }
    }
    std::printf("  [measured] ten load-0.18 dabs at one texel: mass %.4f linear vs %.4f "
                "saturating; the SECOND dab adds %.4f vs %.4f\n",
                lin.mass, sam.mass, firstLin, firstSat);
    check(sam.mass < lin.mass && firstSat < firstLin,
          "saturating: ten overlapping dabs land less mass than ten linear ones, and the "
          "second dab already adds less than the first did");
    check(sam.mass > 0.18f,
          "saturating: it still builds -- diminishing returns, not a per-dab clamp");

    // §4's two clauses, which the new rate must not be able to violate: a
    // texel already thicker than a partial selection allows keeps what it has.
    PigmentTexel thick;
    thick.mass = 0.9f;
    check(depositTexel(thick, kBlue, 0.5f, 0.25f, sat).mass == 0.9f,
          "saturating: a deposit still never REMOVES paint -- a texel over a partial "
          "selection's cap keeps its mass rather than being pulled down to it");
  }

  // ======================================================================
  // 3. The stroke ceiling, and what it is a ceiling ON
  // ======================================================================
  //
  // Dabs stamped at one place, which is the worst case a self-crossing stroke
  // makes: with no ceiling the paper fills, with one the stroke stops at its
  // own opacity however many dabs it spends.
  {
    const auto stampRepeatedly = [&](PigmentBuildup buildup, StrokeMassStore* laid, int dabs) {
      OpenDocument od = makePigmentDoc(128, 128);
      PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
      const BrushTip t = tip(12.0f, 0.5f, 0.18f, kBlue);
      for (int i = 0; i < dabs; ++i)
        depositDab(store, t, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr, buildup, laid);
      return worstMass(store);
    };

    PigmentBuildup ceil04;
    ceil04.strokeCeiling = true;
    ceil04.opacity = 0.4f;
    StrokeMassStore laid;
    const float capped = stampRepeatedly(ceil04, &laid, 20);
    const float uncapped = stampRepeatedly(PigmentBuildup{}, nullptr, 20);
    std::printf("  [measured] 20 load-0.18 dabs in one place: worst mass %.4f at opacity 0.4 "
                "with the ceiling on vs %.4f with it off\n",
                capped, uncapped);
    // Mass is stored as binary16, whose ulp at 0.4 is 2^-12 = 2.44e-4, and the
    // memory accumulates the deltas that storage actually kept -- so the
    // ceiling holds to within a couple of ulps of the format rather than
    // exactly. Rounding toward zero at the point of storage would make it
    // exact and would break §1's bit-for-bit identity when the modes are off,
    // which is the more valuable of the two guarantees.
    check(capped <= 0.4f + 1.0e-3f,
          "stroke ceiling: no number of dabs takes one stroke past its own opacity, which is "
          "what makes a self-crossing flat instead of darker");
    check(uncapped > 0.9f,
          "...and without it the same dabs fill the paper -- the fixture discriminates");

    // The ceiling must be a ceiling on the STROKE, not on the document: a
    // second stroke starts with an empty memory and layers over the first,
    // exactly as a second pass in Photoshop does.
    PigmentBuildup ceil04b = ceil04;
    StrokeMassStore first;
    OpenDocument od = makePigmentDoc(128, 128);
    PigmentTileStore& store = *od.document.layers[1].pigmentTiles;
    const BrushTip t = tip(12.0f, 0.5f, 0.18f, kBlue);
    for (int i = 0; i < 20; ++i)
      depositDab(store, t, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr, ceil04b, &first);
    const float afterOne = worstMass(store);
    StrokeMassStore second;  // a new stroke: a new memory
    for (int i = 0; i < 20; ++i)
      depositDab(store, t, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr, ceil04b, &second);
    const float afterTwo = worstMass(store);
    std::printf("  [measured] a second stroke over the first, both at opacity 0.4: %.4f -> "
                "%.4f\n",
                afterOne, afterTwo);
    check(afterTwo > afterOne + 0.05f,
          "stroke ceiling: a SECOND stroke layers past it -- the memory belongs to the stroke, "
          "not to the document, so repeated passes still darken");

    // §1a's documented divergence from the RGB route, which skips a dab
    // outright once its ceiling is reached: here mass and hue are separate
    // quantities and only one of them is full, so a texel that can hold no
    // more paint still takes on the colour of what is laid on it -- exactly
    // what §1(iii) and §4 already settle for the paper cap and the
    // selection's.
    const PigmentTile* beforeTile = store.find(TileCoord{0, 0});
    const PigmentTexel wasBlue = beforeTile->readTexel(tileLocalOffset(PixelCoord{64, 64}));
    const BrushTip red = tip(12.0f, 0.5f, 0.18f, kRed);
    StrokeMassStore third;
    for (int i = 0; i < 20; ++i)
      depositDab(store, red, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr, ceil04b, &third);
    const PigmentTexel nowRed =
        store.find(TileCoord{0, 0})->readTexel(tileLocalOffset(PixelCoord{64, 64}));
    float hueMoved = 0.0f;
    for (size_t i = 0; i < 3; ++i) {
      hueMoved = std::max(hueMoved, std::fabs(nowRed.latent.c[i] - wasBlue.latent.c[i]));
      hueMoved = std::max(hueMoved, std::fabs(nowRed.latent.res[i] - wasBlue.latent.res[i]));
    }
    std::printf("  [measured] red laid over a texel already at its ceiling: mass %.4f -> %.4f, "
                "worst latent channel moved %.4f\n",
                wasBlue.mass, nowRed.mass, hueMoved);
    check(hueMoved > 0.01f,
          "stroke ceiling: hue keeps moving once the ceiling is reached, as it does at the "
          "paper cap -- a texel that can hold no more paint still takes the colour");
  }

  // ======================================================================
  // 4. At opacity 1 the ceiling is a no-op, and the session clears its memory
  // ======================================================================
  {
    const auto strokeThrough = [&](PigmentBuildup buildup, int passes, float opacity) {
      OpenDocument od = makePigmentDoc(256, 128);
      BrushState brush;
      brush.model.tip.diameterPx = 24.0f;
      brush.native.load = 0.18f;
      brush.opacity = opacity;
      MixboxLut noLut;
      const BrushTip t = brushTipFor(brush, noLut, 1.0f);
      std::string error;
      // ONE session for every pass, which is how the application holds it --
      // a session per pass would get a fresh accumulator from the member's own
      // constructor and could not tell whether `beginRoutes()` resets it.
      StrokeSession session;
      for (int p = 0; p < passes; ++p) {
        session.begin(od, 1, t, Tool::Brush, &error, /*model=*/nullptr, DynamicInputs{},
                      /*clone=*/nullptr, StabiliserParams{}, 1.0f, &brush.native, buildup);
        for (int i = 0; i <= 12; ++i) session.addPoint(40.0f + static_cast<float>(i) * 8.0f, 64.0f);
        session.end();
      }
      return worstMass(*od.document.layers[1].pigmentTiles);
    };

    PigmentBuildup ceilingOn;
    ceilingOn.strokeCeiling = true;
    const float onAtFull = strokeThrough(ceilingOn, 1, 1.0f);
    const float offAtFull = strokeThrough(PigmentBuildup{}, 1, 1.0f);
    std::printf("  [measured] one stroke at opacity 1: worst mass %.6f with the ceiling on vs "
                "%.6f with it off\n",
                onAtFull, offAtFull);
    check(onAtFull == offAtFull,
          "the ceiling is exactly a no-op at opacity 1 -- the paper cap already enforces it, "
          "so turning the mode on cannot change a stroke at the default");

    // The session drops the memory per stroke (`beginRoutes()`); without that,
    // pass two would find pass one's ceiling already spent. Measured at
    // opacity 0.4 and NOT at 1, because at 1 the ceiling is the no-op just
    // asserted above -- a reset test there cannot fail whether the reset
    // happens or not.
    const float onePass = strokeThrough(ceilingOn, 1, 0.4f);
    const float twoPasses = strokeThrough(ceilingOn, 2, 0.4f);
    std::printf("  [measured] one pass vs two, ceiling on at opacity 0.4: %.6f -> %.6f\n",
                onePass, twoPasses);
    check(onePass <= 0.4f + 1.0e-3f && twoPasses > onePass + 0.1f,
          "the session clears the stroke's mass memory at pen-down -- one pass stops at the "
          "opacity, and a second pass is not capped by what the first one spent");
  }

  // ======================================================================
  // 5. A stroke that crosses itself, through the session and the chrome's gate
  // ======================================================================
  //
  // The user's own case end to end: one X-shaped stroke of a soft brush at load
  // 0.18, read at the crossing. Then the predicate the Opacity slider greys
  // itself out by -- it once left the slider disabled on the Pigment deposit
  // over a ceiling that worked, so turning the switch on changed nothing a
  // painter could reach.
  {
    const auto crossingOf = [&](PigmentBuildup buildup, float opacity) {
      OpenDocument od = makePigmentDoc(256, 256);
      BrushState brush;
      brush.model.tip.diameterPx = 40.0f;
      brush.model.tip.hardness = 0.3f;
      brush.native.load = 0.18f;
      brush.opacity = opacity;
      MixboxLut noLut;
      const BrushTip t = brushTipFor(brush, noLut, 1.0f);
      std::string error;
      StrokeSession session;
      session.begin(od, 1, t, Tool::Brush, &error, /*model=*/nullptr, DynamicInputs{},
                    /*clone=*/nullptr, StabiliserParams{}, 1.0f, &brush.native, buildup);
      const float pts[4][2] = {{40, 40}, {216, 216}, {216, 40}, {40, 216}};
      for (int s = 0; s < 3; ++s)
        for (int i = 0; i < 44; ++i) {
          const float f = static_cast<float>(i) / 44.0f;
          session.addPoint(pts[s][0] + (pts[s + 1][0] - pts[s][0]) * f,
                           pts[s][1] + (pts[s + 1][1] - pts[s][1]) * f);
        }
      session.addPoint(40.0f, 216.0f);
      session.end();
      const PigmentTile* tile = od.document.layers[1].pigmentTiles->find(TileCoord{1, 1});
      return tile != nullptr ? tile->readTexel(tileLocalOffset(PixelCoord{128, 128})).mass
                             : 0.0f;
    };
    PigmentBuildup sat;
    sat.saturating = true;
    PigmentBuildup cap;
    cap.strokeCeiling = true;
    const float linear = crossingOf(PigmentBuildup{}, 1.0f);
    const float saturating = crossingOf(sat, 1.0f);
    const float capped = crossingOf(cap, 0.5f);
    std::printf("  [measured] an X stroke's crossing: mass %.4f linear, %.4f saturating, "
                "%.4f with the ceiling at opacity 0.5\n",
                linear, saturating, capped);
    check(linear > 0.9f && saturating < linear - 0.2f,
          "a self-crossing stroke: diminishing overlaps lightens the crossing a painter "
          "actually draws, not only a stack of dabs at one point");
    check(capped <= 0.5f + 1.0e-3f,
          "a self-crossing stroke: with the ceiling on, the crossing stops at the opacity");

    OpenDocument od = makePigmentDoc(16, 16);
    const StrokeRoute pigment = strokeRouteFor(Tool::Brush, &od.document.layers[1]);
    check(pigment == StrokeRoute::CpuDeposit &&
              !opacityReachesRoute(pigment, PigmentBuildup{}) &&
              opacityReachesRoute(pigment, cap),
          "the Opacity slider is live on a Pigment layer exactly while the stroke ceiling is "
          "on -- the switch is not left pointing at a greyed-out control");
    check(opacityReachesRoute(StrokeRoute::RgbDeposit, PigmentBuildup{}) &&
              !opacityReachesRoute(StrokeRoute::Smudge, cap),
          "...and the other routes answer as the slider always did, whatever BUILDUP says");
  }

  std::printf("[selftest] pigment buildup %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
