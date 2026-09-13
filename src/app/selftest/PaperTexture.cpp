#include "app/selftest/Support.hpp"

#include <string>

#include "app/StrokeSession.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "core/LayerOps.hpp"
#include "paint/Palette.hpp"
#include "brush/Deposit.hpp"
#include "brush/Grain.hpp"
#include "brush/PigmentErase.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/RgbErase.hpp"

namespace np {

// ---------------------------------------------------------------------------
// **A real scanned paper under the brush** -- `GrainParams::field`, and the
// three deposit routes that never asked for grain at all.
//
// brush/Grain has generated its height field procedurally since it shipped
// (its own §0 says so: the patent names a stored lookup table, this codebase
// ships no binary fixtures, so the table's entries are hashed rather than
// read). A `.abr` pack carries the real thing -- "Extra Heavy Canvas", "ktw
// watercolor paper 2k17 b" -- in its `patt` block, 98-99% of the file's bytes,
// named by 84 of the 101 presets measured. io/PsPatterns decodes them;
// `PaperField` is the shape they arrive in; this file is what checks that the
// deposit actually samples one.
//
// **Sections D-F are a gap being closed, not a feature being regression-
// tested.** `grainCoverageAt()` was called from exactly one deposit route --
// the CPU Pigment one -- and from the preview. `RgbDeposit`, `RgbErase` and
// `PigmentErase` had no grain call in them at all, so paper texture worked on
// a Pigment layer and silently did nothing on an RGB layer, which is what an
// ordinary File > New gives you (brush/RgbDeposit's own section 9 makes the
// same argument about a different bug). Every assertion in D-F fails on the
// code as it stood before this file was written.
//
// The field used throughout is a **checkerboard**, not a scan and not a hash:
// alternate texels at full height, the rest at zero, at depth 1.0. Under
// Subtract that makes the arithmetic exact and the outcome binary -- a peak
// texel gets `clamp(P - 1, 0, 1) == 0` for any coverage at all, a valley texel
// gets `P` untouched -- so "did the deposit sample the paper" becomes a
// question about which texels are EMPTY rather than a question about
// tolerances. A procedural field could not do that: its heights are hashed
// draws in [0, depth) and never reach the depth itself.
// ---------------------------------------------------------------------------
bool runPaperTextureTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // A checkerboard of period 2: `(x + y) % 2 == 0` is a peak at full height.
  // Peaks are stored BLACK: with Invert off the darkest texels take the least
  // paint (brush/Grain.hpp, `GrainParams::invert`).
  auto makeChecker = [](int32_t w, int32_t h) {
    auto f = std::make_shared<PaperField>();
    f->width = w;
    f->height = h;
    f->height8.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x)
        f->height8[static_cast<size_t>(y) * w + x] = ((x + y) % 2 == 0) ? 0 : 255;
    return f;
  };
  // A left-to-right ramp, for the shaping checks where a two-value field
  // cannot distinguish brightness from contrast.
  // Stored black to white, so its HEIGHT runs 1 down to 0.
  auto makeRamp = [](int32_t w) {
    auto f = std::make_shared<PaperField>();
    f->width = w;
    f->height = 1;
    f->height8.resize(static_cast<size_t>(w));
    for (int32_t x = 0; x < w; ++x)
      f->height8[static_cast<size_t>(x)] =
          static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
    return f;
  };

  auto readRgb = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // A hard disc: `dabCoverage()` is exactly 1.0 across the whole core, so
  // every number below is about the paper and not about the falloff.
  auto discTip = [](float radius) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = 1.0f;
    t.linearRgb = {1.0f, 1.0f, 1.0f};
    return t;
  };

  // ======================================================================
  std::printf("  -- A. a null field is the procedural path, bit for bit --\n");
  // ======================================================================
  {
    // The default, and the reason it is the default: every brush authored
    // before sampled papers existed, every golden reference and every
    // assertion in selftest/Grain.cpp must take the identical code path and
    // produce the identical float. Checked at exactly zero tolerance over a
    // spread of coordinates, because this is a claim about which branch runs.
    GrainParams p;
    p.enabled = true;
    p.periodX = 24;
    p.periodY = 17;
    p.depth = 0.35f;
    check(p.field == nullptr, "paper/default: GrainParams carries no field until one is attached");

    // The shaping controls exist only for a sampled field. Setting them with
    // no field attached must change nothing at all -- otherwise a preset that
    // merely REMEMBERS a texture it is no longer using would paint
    // differently from one that never had one.
    GrainParams shaped = p;
    shaped.scale = 3.5f;
    shaped.invert = true;
    shaped.brightness = 0.4f;
    shaped.contrast = -0.9f;

    bool identical = true;
    for (int32_t y = -40; y <= 40; y += 7)
      for (int32_t x = -40; x <= 40; x += 3)
        if (grainHeightAt(shaped, x, y) != grainHeightAt(p, x, y)) identical = false;
    check(identical,
          "paper/null: scale, invert, brightness and contrast are inert with no field");

    // And the whole pipeline, not only the field: same coverage in, same
    // coverage out.
    bool coverageIdentical = true;
    for (int32_t y = -40; y <= 40; y += 7)
      for (int32_t x = -40; x <= 40; x += 3)
        if (grainCoverageAt(shaped, 0.625f, x, y) != grainCoverageAt(p, 0.625f, x, y))
          coverageIdentical = false;
    check(coverageIdentical,
          "paper/null: and grainCoverageAt() is unchanged through the procedural branch");
  }

  // ======================================================================
  std::printf("  -- B. the sampled field: nearest, wrapped, scaled, shaped --\n");
  // ======================================================================
  {
    GrainParams p;
    p.enabled = true;
    p.depth = 1.0f;
    p.field = makeChecker(4, 4);

    check(grainHeightAt(p, 0, 0) == 1.0f && grainHeightAt(p, 1, 0) == 0.0f,
          "paper/sample: a black texel holds paint off by depth, a white one not at all");

    // Adobe's direction, through the whole formula: with Invert off the lightest
    // texels receive the most paint.
    {
      GrainParams subtract = p;
      subtract.depth = 0.5f;
      const float onWhite = grainCoverageAt(subtract, 0.75f, 1, 0);
      const float onBlack = grainCoverageAt(subtract, 0.75f, 0, 0);
      std::printf("    [measured] coverage 0.75, depth 0.5: white texel %.3f, black texel %.3f\n",
                  static_cast<double>(onWhite), static_cast<double>(onBlack));
      check(onWhite == 0.75f && onBlack == 0.25f,
            "paper/direction: with Invert off a white texel takes the paint, a black one resists");
    }

    // **Tiling is the property the whole feature stands on** (brush/Grain.hpp
    // §3): keyed by absolute document position, so the same brush picks up
    // different paper depending on where on the page it lands. A field that
    // did not wrap would be a 4x4 patch at the origin and nothing anywhere
    // else.
    bool wraps = true;
    for (int32_t y = -16; y <= 16; ++y)
      for (int32_t x = -16; x <= 16; ++x)
        if (grainHeightAt(p, x, y) != grainHeightAt(p, x + 4, y) ||
            grainHeightAt(p, x, y) != grainHeightAt(p, x, y + 4))
          wraps = false;
    check(wraps, "paper/wrap: tiles exactly in both axes, including at negative coordinates");

    // Negative coordinates specifically: a `%` that keeps the sign would read
    // out of bounds or mirror the pattern about the origin, and a canvas is
    // scrolled to negative document coordinates all the time.
    check(grainHeightAt(p, -1, 0) == grainHeightAt(p, 3, 0) &&
              grainHeightAt(p, -4, -4) == grainHeightAt(p, 0, 0),
          "paper/wrap: -1 reads the LAST column, not a mirrored or clamped first one");

    // Scale: how many document texels one paper texel covers. Photoshop's
    // Texture panel spells it as a percentage; this is that over 100.
    GrainParams s2 = p;
    s2.scale = 2.0f;
    check(grainHeightAt(s2, 0, 0) == grainHeightAt(s2, 1, 0) &&
              grainHeightAt(s2, 0, 0) != grainHeightAt(s2, 2, 0),
          "paper/scale: at 2.0 each paper texel covers two document texels");
    // Floor, not truncate: `-1 / 2` truncates toward zero and would make the
    // texel at -1 share the one at 0, putting a two-wide seam at the origin.
    check(grainHeightAt(s2, -1, 0) == grainHeightAt(s2, -2, 0),
          "paper/scale: divides with floor, so there is no double-width cell at the origin");
    GrainParams s0 = p;
    s0.scale = 0.0f;
    check(grainHeightAt(s0, 3, 1) == grainHeightAt(p, 3, 1),
          "paper/scale: a zero scale falls back to 1.0 rather than dividing by zero");

    // Depth scales the sampled height exactly as it bounds the procedural one.
    GrainParams half = p;
    half.depth = 0.5f;
    check(grainHeightAt(half, 0, 0) == 0.5f,
          "paper/depth: the sampled height is scaled by depth, same as the generated one");

    // Invert swaps which parts of the paper resist paint, and does NOT change
    // how deep the tooth is -- so it is applied before `depth`, not after.
    GrainParams inv = p;
    inv.invert = true;
    check(grainHeightAt(inv, 0, 0) == 0.0f && grainHeightAt(inv, 1, 0) == 1.0f,
          "paper/invert: peaks and valleys trade places");
    GrainParams invHalf = inv;
    invHalf.depth = 0.5f;
    check(grainHeightAt(invHalf, 1, 0) == 0.5f,
          "paper/invert: applied BEFORE depth, so inverting does not change the amplitude");

    // Brightness and contrast need a field with more than two values.
    GrainParams r;
    r.enabled = true;
    r.depth = 1.0f;
    r.field = makeRamp(5);  // 0, 63, 127, 191, 255 -> heights 1, .753, .502, .251, 0
    const float mid = grainHeightAt(r, 2, 0);

    GrainParams rb = r;
    rb.brightness = 0.25f;
    check(grainHeightAt(rb, 2, 0) < mid && grainHeightAt(rb, 4, 0) == 0.0f,
          "paper/brightness: a brighter paper holds less paint off, and clamps at white");

    GrainParams rc = r;
    rc.contrast = 1.0f;  // (h - 0.5) * 2 + 0.5
    check(grainHeightAt(rc, 0, 0) == 1.0f && grainHeightAt(rc, 4, 0) == 0.0f &&
              grainHeightAt(rc, 3, 0) < grainHeightAt(r, 3, 0),
          "paper/contrast: pivots about mid-grey, pushing lights lighter and darks darker");
    GrainParams rflat = r;
    rflat.contrast = -1.0f;  // (h - 0.5) * 0 + 0.5 -- every texel identical
    check(grainHeightAt(rflat, 0, 0) == 0.5f && grainHeightAt(rflat, 4, 0) == 0.5f,
          "paper/contrast: at -1 the paper goes perfectly flat, every texel mid-grey");
  }

  // ======================================================================
  std::printf("  -- C. Height and Subtract really are one formula --\n");
  // ======================================================================
  {
    // Height is Subtract with the paper `heightDepthGain()` deeper, so Height at
    // depth d paints what Subtract paints at depth d * gain. The grid includes
    // strength 1.75: routed through `applyCoverageBlend()`, which clamps its
    // base first, the two would diverge.
    GrainParams sub;
    sub.enabled = true;
    sub.depth = 1.0f;
    sub.field = makeChecker(4, 4);
    sub.blend = CoverageBlend::Subtract;

    bool same = true;
    bool strengthAbove1Reached = false;
    for (const float strength : {0.5f, 1.0f, 1.75f}) {
      GrainParams a = sub;
      a.strength = strength;
      a.depth = 0.1f * heightDepthGain(0.1f);
      GrainParams b = a;
      b.blend = CoverageBlend::Height;
      b.depth = 0.1f;
      for (int ci = 0; ci <= 8; ++ci) {
        const float cov = static_cast<float>(ci) / 8.0f;
        for (int32_t y = 0; y < 4; ++y)
          for (int32_t x = 0; x < 4; ++x) {
            if (std::fabs(grainCoverageAt(a, cov, x, y) - grainCoverageAt(b, cov, x, y)) > 1e-5f)
              same = false;
            if (strength > 1.0f && cov > 0.75f && grainCoverageAt(a, cov, x, y) > 0.0f)
              strengthAbove1Reached = true;
          }
      }
    }
    check(same, "paper/Height: at depth d it paints what Subtract paints at depth d * "
                "heightDepthGain(d), at every strength including above 1");
    check(strengthAbove1Reached,
          "paper/Height: and the strength>1 case is actually reached, not vacuously equal");

    // The gain's anchors: tenfold for a shallow paper, none at depth 1 (Adobe:
    // Depth 100% leaves only the lowest points unpainted), and never so deep
    // that a full-coverage tip loses more than the deepest hollow.
    bool reachCapped = true;
    bool decreasing = true;
    float previous = heightDepthGain(0.0f);
    for (int di = 1; di <= 100; ++di) {
      const float d = static_cast<float>(di) / 100.0f;
      if (d * heightDepthGain(d) > 1.0f + 1e-6f) reachCapped = false;
      if (!(heightDepthGain(d) < previous)) decreasing = false;
      previous = heightDepthGain(d);
    }
    check(heightDepthGain(0.0f) == kHeightDepthGain && heightDepthGain(1.0f) == 1.0f && decreasing,
          "paper/Height: the depth gain is ten for a shallow paper, falling to one at depth 1");
    check(reachCapped, "paper/Height: depth times gain never exceeds 1, so a full-coverage dab at "
                       "any depth still paints all but the deepest hollows");
    std::printf("    [measured] Height depth gain: %.2f at 0.05, %.2f at 0.17, %.2f at 0.36\n",
                heightDepthGain(0.05f), heightDepthGain(0.17f), heightDepthGain(0.36f));
    GrainParams shallowHeight = sub;
    shallowHeight.blend = CoverageBlend::Height;
    shallowHeight.depth = 0.1f;
    GrainParams shallowSub = shallowHeight;
    shallowSub.blend = CoverageBlend::Subtract;
    bool deeperThanSubtract = false;
    bool neverShallower = true;
    for (int32_t y = 0; y < 4; ++y)
      for (int32_t x = 0; x < 4; ++x) {
        const float hgt = grainCoverageAt(shallowHeight, 0.6f, x, y);
        const float sbt = grainCoverageAt(shallowSub, 0.6f, x, y);
        if (hgt < sbt) deeperThanSubtract = true;
        if (hgt > sbt) neverShallower = false;
      }
    check(deeperThanSubtract && neverShallower,
          "paper/Height: at a shallow depth it takes more paint out of the hollows than Subtract");

    // Flow: for Height it presses the tip less hard, so more paper shows; for
    // every other blend it only scales the result.
    GrainParams h = sub;
    h.blend = CoverageBlend::Height;
    h.depth = 0.3f;
    h.field = makeChecker(8, 8);
    int emptyAtFull = 0;
    int emptyAtLow = 0;
    bool otherBlendsScale = true;
    for (int32_t y = 0; y < 8; ++y)
      for (int32_t x = 0; x < 8; ++x) {
        emptyAtFull += grainWeightAt(h, 1.0f, 1.0f, x, y) == 0.0f;
        emptyAtLow += grainWeightAt(h, 1.0f, 0.5f, x, y) == 0.0f;
        for (const CoverageBlend other : {CoverageBlend::Subtract, CoverageBlend::Multiply,
                                          CoverageBlend::ColorBurn}) {
          GrainParams o = h;
          o.blend = other;
          if (grainWeightAt(o, 0.7f, 0.35f, x, y) != 0.35f * grainCoverageAt(o, 0.7f, x, y))
            otherBlendsScale = false;
        }
      }
    std::printf("    [measured] Height depth 0.3: %d/64 texels empty at flow 1, %d/64 at flow 0.5\n",
                emptyAtFull, emptyAtLow);
    check(emptyAtLow > emptyAtFull,
          "paper/Height: lowering flow opens more of the paper, not merely a paler stroke");
    check(otherBlendsScale,
          "paper/weight: every other blend is exactly flow times its grain coverage");

    // A faint tip at full flow: the paper is measured against the flow, so a
    // peak the flow clears still takes some of the tip -- where subtracting
    // from flow times coverage left nothing.
    const float faintOnPeak = grainWeightAt(h, 0.25f, 1.0f, 0, 0);
    const float fullOnPeak = grainWeightAt(h, 1.0f, 1.0f, 0, 0);
    std::printf("    [measured] Height depth 0.3, flow 1, on a peak: tip 0.25 -> %.4f, tip 1 -> %.4f\n",
                static_cast<double>(faintOnPeak), static_cast<double>(fullOnPeak));
    check(faintOnPeak > 0.0f && std::abs(faintOnPeak - 0.25f * fullOnPeak) < 1e-6f,
          "paper/Height: a faint tip keeps its share where the flow clears the paper");
  }

  {
    // Wash keeps the strongest dab, so a flow-scaled Height weight left
    // low-flow grain as a faint glaze. Low flow must open more paper while the
    // specks keep the tip's strength -- on both routes.
    GrainParams paper;
    paper.enabled = true;
    paper.blend = CoverageBlend::Height;
    paper.depth = 0.3f;
    paper.field = makeChecker(8, 8);
    const auto washDisc = [&](bool rgbRoute, float flow) {
      BrushTip t;
      t.radius = 20.0f;
      t.hardness = 1.0f;
      t.flow = flow;
      t.grain = paper;
      TileStore rgbStore;
      RgbStroke rgb;
      rgb.begin({1.0f, 1.0f, 1.0f}, 1.0f, false, BlendMode::Normal, /*wash=*/true);
      PigmentTileStore pigStore;
      PigmentTileStore nothingBefore;
      WashStroke wash;
      wash.before = &nothingBefore;
      for (int k = 0; k < 4; ++k) {
        if (rgbRoute)
          rgb.depositDab(rgbStore, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
        else
          depositDab(pigStore, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr,
                     PigmentBuildup{PigmentBuildupMode::Wash, 1.0f}, &wash);
      }
      int empty = 0;
      float strongest = 0.0f;
      for (int32_t y = 52; y < 76; ++y)
        for (int32_t x = 52; x < 76; ++x) {
          const PixelCoord pc{x, y};
          float v = 0.0f;
          if (rgbRoute) {
            if (const Tile* tl = rgbStore.find(tileCoordAt(pc))) v = tl->readPixel(tileLocalOffset(pc))[3];
          } else if (const PigmentTile* tl = pigStore.find(tileCoordAt(pc))) {
            v = tl->readTexel(tileLocalOffset(pc)).mass;
          }
          empty += v == 0.0f;
          strongest = std::max(strongest, v);
        }
      return std::make_pair(empty, strongest);
    };
    for (const bool rgbRoute : {false, true}) {
      const auto [emptyFull, strongFull] = washDisc(rgbRoute, 1.0f);
      const auto [emptyLow, strongLow] = washDisc(rgbRoute, 0.25f);
      std::printf("    [measured] %s wash, Height depth 0.3: empty %d -> %d of 576, strongest %.3f -> "
                  "%.3f (flow 1 -> 0.25)\n",
                  rgbRoute ? "RGB" : "Pigment", emptyFull, emptyLow, strongFull, strongLow);
      check(emptyLow > emptyFull && strongLow >= 0.9f * strongFull && strongFull > 0.0f,
            rgbRoute ? "paper/wash: RGB, low flow opens the paper and the specks keep their strength"
                     : "paper/wash: Pigment, low flow opens the paper and the specks keep their strength");
    }
    GrainParams other = paper;
    other.blend = CoverageBlend::Subtract;
    check(grainWashWeightAt(other, 0.6f, 0.3f, 3, 5) == grainWeightAt(other, 0.6f, 0.3f, 3, 5) &&
              grainWashWeightAt(paper, 0.6f, 0.0f, 3, 5) == grainWeightAt(paper, 0.6f, 0.0f, 3, 5),
          "paper/wash: every other blend, and zero flow, keep the Build-up weight");
  }

  {
    // Texture Each Tip OFF: the paper is cut from the stroke's accumulated
    // weight once, so overlap fills a hollow only as far as the stroke's weight
    // reaches -- where per tip, a hollow deeper than one dab's weight stays
    // empty however many dabs cross it.
    GrainParams paper;
    paper.enabled = true;
    paper.blend = CoverageBlend::Height;
    paper.depth = 0.3f;
    paper.field = makeChecker(8, 8);
    int32_t hx = -1, hy = -1, px = -1, py = -1;
    float deepest = -1.0f, shallowest = 2.0f;
    for (int32_t y = 56; y < 72; ++y)
      for (int32_t x = 56; x < 72; ++x) {
        const float g = grainHeightAt(paper, x, y);
        if (g > deepest) { deepest = g; hx = x; hy = y; }
        if (g < shallowest) { shallowest = g; px = x; py = y; }
      }
    const auto stroke = [&](bool rgbRoute, bool eachTip, bool wash, float flow) {
      BrushTip t;
      t.radius = 20.0f;
      t.hardness = 1.0f;
      t.flow = flow;
      t.grain = paper;
      t.grain.eachTip = eachTip;
      TileStore rgbStore;
      RgbStroke rgb;
      rgb.begin({1.0f, 1.0f, 1.0f}, 1.0f, false, BlendMode::Normal, wash);
      PigmentTileStore pigStore;
      StrokeTexture texture;
      for (int k = 0; k < 40; ++k) {
        if (rgbRoute)
          rgb.depositDab(rgbStore, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr, Vec2{},
                         nullptr, &texture);
        else
          depositDab(pigStore, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr, PigmentBuildup{},
                     nullptr, Vec2{}, nullptr, &texture);
      }
      const auto read = [&](int32_t x, int32_t y) {
        const PixelCoord pc{x, y};
        if (rgbRoute) {
          const Tile* tl = rgbStore.find(tileCoordAt(pc));
          return tl != nullptr ? tl->readPixel(tileLocalOffset(pc))[3] : 0.0f;
        }
        const PigmentTile* tl = pigStore.find(tileCoordAt(pc));
        return tl != nullptr ? tl->readTexel(tileLocalOffset(pc)).mass : 0.0f;
      };
      return std::make_pair(read(hx, hy), read(px, py));
    };
    float union40 = 0.0f;
    for (int k = 0; k < 40; ++k) union40 = union40 + 0.3f * (1.0f - union40);
    const float wantHollow = grainCoverageAt(paper, union40, hx, hy);
    const float wantPeak = grainCoverageAt(paper, union40, px, py);
    for (const bool rgbRoute : {false, true}) {
      const auto [tipHollow, tipPeak] = stroke(rgbRoute, true, false, 0.3f);
      const auto [strokeHollow, strokePeak] = stroke(rgbRoute, false, false, 0.3f);
      std::printf("    [measured] %s build-up, 40 dabs at flow 0.3, hollow / peak: each tip %.4f / "
                  "%.4f, stroke %.4f / %.4f (paper cut from the stroke: %.4f / %.4f)\n",
                  rgbRoute ? "RGB" : "Pigment", tipHollow, tipPeak, strokeHollow, strokePeak,
                  wantHollow, wantPeak);
      // Both layers store binary16: near full coverage its step is ~5e-4, and
      // the stroke's last increments are smaller than that.
      const float tol = 2e-3f;
      check(tipHollow == 0.0f && std::fabs(strokeHollow - wantHollow) <= tol &&
                std::fabs(strokePeak - wantPeak) <= tol && wantHollow > 0.1f,
            rgbRoute ? "paper/each tip off: RGB lays the paper cut once from the stroke's weight"
                     : "paper/each tip off: Pigment lays the paper cut once from the stroke's weight");
    }
    const auto [washTipHollow, washTipPeak] = stroke(true, true, true, 0.25f);
    const auto [washStrokeHollow, washStrokePeak] = stroke(true, false, true, 0.25f);
    std::printf("    [measured] RGB wash at flow 0.25, peak: each tip %.4f, stroke %.4f\n",
                washTipPeak, washStrokePeak);
    check(washTipPeak > 0.9f && washStrokePeak <= 0.25f + 2e-3f && washStrokePeak > 0.2f &&
              washTipHollow == 0.0f && washStrokeHollow == 0.0f,
          "paper/each tip off: a Wash stroke stays as light as its flow, grain cut from that");
    GrainParams off = paper;
    off.eachTip = false;
    check(!grainParamsEqual(paper, off), "paper/each tip: the flag is part of a tip's identity");

    // The Brush Settings checkbox reaches the tip, and a session stroke on
    // either layer honours it -- through ONE reused session, so a stroke that
    // inherited the last one's texture weight would show here.
    StrokeSession shared;
    for (const bool rgbLayer : {false, true}) {
      float firstOff = -1.0f;
      for (const bool eachTip : {true, false, false}) {
        OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "each tip");
        recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                     rgbLayer ? makeRgbLayer("RGB") : makePigmentLayer("Pigment")));
        BrushState brush;
        brush.model.tip.diameterPx = 40.0f;
        brush.model.tip.hardness = 1.0f;
        brush.model.tip.spacingPercent = 5.0f;
        brush.native.load = 0.3f;
        brush.native.grain = paper;
        brush.model.texture.enabled = true;
        brush.model.texture.eachTip = eachTip;
        MixboxLut noLut;
        const BrushTip base = brushTipFor(brush, noLut, 1.0f);
        std::string error;
        shared.begin(od, 1, base, Tool::Brush, &error, &brush.model, DynamicInputs{}, nullptr,
                     StabiliserParams{}, 1.0f, &brush.native);
        for (int32_t i = 54; i <= 74; i += 2) shared.addPoint(static_cast<float>(i), 64.0f);
        shared.end();
        const Layer& layer = od.document.layers[1];
        const PixelCoord pc{hx, hy};
        float hollow = 0.0f;
        if (layer.pigmentTiles.has_value()) {
          if (const PigmentTile* tl = layer.pigmentTiles->find(tileCoordAt(pc)))
            hollow = tl->readTexel(tileLocalOffset(pc)).mass;
        } else if (const Tile* tl = layer.rgbTiles->find(tileCoordAt(pc))) {
          hollow = tl->readPixel(tileLocalOffset(pc))[3];
        }
        if (eachTip) {
          check(base.grain.eachTip && hollow == 0.0f,
                rgbLayer ? "paper/each tip: an RGB session stroke textured per tip keeps the hollows empty"
                         : "paper/each tip: a Pigment session stroke textured per tip keeps the hollows empty");
        } else if (firstOff < 0.0f) {
          firstOff = hollow;
          check(!base.grain.eachTip && hollow > 0.1f,
                rgbLayer ? "paper/each tip: an RGB session stroke with Each Tip off fills them as far as the stroke reaches"
                         : "paper/each tip: a Pigment session stroke with Each Tip off fills them as far as the stroke reaches");
        } else {
          check(hollow == firstOff,
                rgbLayer ? "paper/each tip: RGB, each stroke starts with an empty texture weight"
                         : "paper/each tip: Pigment, each stroke starts with an empty texture weight");
        }
      }
    }
    BrushState native;
    native.native.grain = paper;
    MixboxLut noLut;
    check(brushTipFor(native, noLut, 1.0f).grain.eachTip,
          "paper/each tip: a brush with no imported texture is textured per dab, as before");
  }

  // ======================================================================
  std::printf("  -- D. the RGB deposit route, which had NO grain call at all --\n");
  // ======================================================================
  {
    // Paper texture worked on a Pigment layer and silently did nothing on an
    // RGB layer -- which is the layer an ordinary File > New selects. This is
    // the assertion that fails on the code as it stood.
    //
    // The checkerboard at depth 1.0 makes the outcome binary: a peak texel
    // gets `clamp(1 - 1, 0, 1) == 0` and must be EMPTY; a valley texel gets
    // full coverage. So the painted region is a checkerboard, and "the grain
    // call is missing" is "the whole disc is solid".
    OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "paper rgb");
    TileStore& store = *od.document.layers[0].rgbTiles;

    BrushTip t = discTip(12.0f);
    t.grain.enabled = true;
    t.grain.depth = 1.0f;
    t.grain.field = makeChecker(4, 4);

    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f);
    stroke.depositDab(store, t, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    stroke.end();

    int peaksPainted = 0;
    int valleysPainted = 0;
    int valleysTotal = 0;
    for (int32_t y = 58; y <= 70; ++y) {
      for (int32_t x = 58; x <= 70; ++x) {
        const bool peak = ((x % 4 + 4) % 4 + (y % 4 + 4) % 4) % 2 == 0;
        const float alpha = readRgb(store, x, y)[3];
        if (peak) {
          if (alpha > 0.0f) ++peaksPainted;
        } else {
          ++valleysTotal;
          if (alpha > 0.0f) ++valleysPainted;
        }
      }
    }
    std::printf("    [measured] inside the disc: %d peak texels painted (want 0), "
                "%d of %d valley texels painted\n",
                peaksPainted, valleysPainted, valleysTotal);
    check(peaksPainted == 0,
          "paper/rgb: a full-height paper texel takes NO paint on an RGB layer");
    check(valleysTotal > 0 && valleysPainted == valleysTotal,
          "paper/rgb: and every valley texel takes it in full -- a checkerboard, not a hole");

    // The other direction, so the assertion above cannot be satisfied by a
    // deposit that simply failed: with grain off, the same dab fills solid.
    OpenDocument plain = makeBlankOpenDocument(128, 128, WorkingSpace{}, "paper rgb off");
    TileStore& plainStore = *plain.document.layers[0].rgbTiles;
    BrushTip noGrain = discTip(12.0f);
    RgbStroke s2;
    s2.begin(noGrain.linearRgb, 1.0f);
    s2.depositDab(plainStore, noGrain, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    s2.end();
    bool solid = true;
    for (int32_t y = 58; y <= 70; ++y)
      for (int32_t x = 58; x <= 70; ++x)
        if (!(readRgb(plainStore, x, y)[3] > 0.0f)) solid = false;
    check(solid, "paper/rgb: the SAME dab with grain off fills the disc solid");
  }

  // ======================================================================
  std::printf("  -- E. the two erase routes, which had no grain call either --\n");
  // ======================================================================
  {
    // An eraser is a brush, and a brush on paper skips the peaks. Without the
    // call, erasing over a textured stroke lifts a clean disc out of it --
    // which reads as the texture being painted ON rather than being the paper.
    OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "paper erase");
    TileStore& store = *od.document.layers[0].rgbTiles;

    BrushTip fill = discTip(14.0f);
    RgbStroke lay;
    lay.begin(fill.linearRgb, 1.0f);
    lay.depositDab(store, fill, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    lay.end();

    BrushTip eraser = discTip(12.0f);
    eraser.grain.enabled = true;
    eraser.grain.depth = 1.0f;
    eraser.grain.field = makeChecker(4, 4);
    RgbEraseStroke erase;
    erase.begin(1.0f);
    erase.eraseDab(store, eraser, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    erase.end();

    int peaksSurvived = 0;
    int peaksTotal = 0;
    int valleysCleared = 0;
    int valleysTotal = 0;
    for (int32_t y = 58; y <= 70; ++y) {
      for (int32_t x = 58; x <= 70; ++x) {
        const bool peak = ((x % 4 + 4) % 4 + (y % 4 + 4) % 4) % 2 == 0;
        const float alpha = readRgb(store, x, y)[3];
        if (peak) {
          ++peaksTotal;
          if (alpha > 0.0f) ++peaksSurvived;
        } else {
          ++valleysTotal;
          if (!(alpha > 0.0f)) ++valleysCleared;
        }
      }
    }
    std::printf("    [measured] after erasing: %d of %d peak texels still hold paint, "
                "%d of %d valleys cleared\n",
                peaksSurvived, peaksTotal, valleysCleared, valleysTotal);
    check(peaksTotal > 0 && peaksSurvived == peaksTotal,
          "paper/erase: the eraser skips the paper's peaks, leaving their paint behind");
    check(valleysTotal > 0 && valleysCleared == valleysTotal,
          "paper/erase: and clears the valleys completely -- an eraser on paper, not a hole");
  }

  // ======================================================================
  std::printf("  -- E2. and the pigment eraser, the third route with no call --\n");
  // ======================================================================
  {
    // The same claim on the Pigment route, because it is a genuinely different
    // eraser (it removes stored MASS rather than compositing alpha) and
    // "I added the call to the RGB ones" is not evidence about this one. Two
    // routes proven and a third assumed is exactly the shape of the gap this
    // section exists to close.
    PigmentTileStore store;
    Latent yellow;
    yellow.c = {0.0f, 1.0f, 0.0f};

    BrushTip fill = discTip(14.0f);
    fill.pigment = yellow;
    depositDab(store, fill, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);

    BrushTip eraser = discTip(12.0f);
    eraser.grain.enabled = true;
    eraser.grain.depth = 1.0f;
    eraser.grain.field = makeChecker(4, 4);
    PigmentEraseStroke erase;
    erase.begin(1.0f);
    erase.eraseDab(store, eraser, Vec2{64.5f, 64.5f}, 128, 128, nullptr, nullptr);
    erase.end();

    const auto massAt = [&](int32_t x, int32_t y) {
      const PixelCoord at{x, y};
      const PigmentTile* t = store.find(tileCoordAt(at));
      return t != nullptr ? t->readTexel(tileLocalOffset(at)).mass : 0.0f;
    };
    int peaksKept = 0;
    int peaksTotal = 0;
    int valleysCut = 0;
    int valleysTotal = 0;
    for (int32_t y = 58; y <= 70; ++y) {
      for (int32_t x = 58; x <= 70; ++x) {
        const bool peak = ((x % 4 + 4) % 4 + (y % 4 + 4) % 4) % 2 == 0;
        const float mass = massAt(x, y);
        if (peak) {
          ++peaksTotal;
          if (mass > 0.0f) ++peaksKept;
        } else {
          ++valleysTotal;
          if (!(mass > 0.0f)) ++valleysCut;
        }
      }
    }
    std::printf("    [measured] pigment erase: %d of %d peaks kept their mass, "
                "%d of %d valleys cut to zero\n",
                peaksKept, peaksTotal, valleysCut, valleysTotal);
    check(peaksTotal > 0 && peaksKept == peaksTotal,
          "paper/pigment-erase: the paper's peaks keep their pigment");
    check(valleysTotal > 0 && valleysCut == valleysTotal,
          "paper/pigment-erase: and the valleys are cut clean");
  }

  // ======================================================================
  std::printf("  -- F. grain off is still a no-op on every route --\n");
  // ======================================================================
  {
    // Three new call sites is three new chances to make a disabled paper cost
    // something. `grainCoverageAt()` returns its argument before any floating-
    // point operation when `!enabled`, and the routes must not defeat that by
    // doing the work anyway -- so the two deposits are compared texel for
    // texel at EXACTLY zero tolerance.
    auto paint = [&](bool attachDisabledPaper) {
      OpenDocument od = makeBlankOpenDocument(96, 96, WorkingSpace{}, "paper noop");
      BrushTip t = discTip(10.0f);
      if (attachDisabledPaper) {
        t.grain.enabled = false;  // the point: a paper is attached but switched off
        t.grain.depth = 1.0f;
        t.grain.field = makeChecker(4, 4);
      }
      RgbStroke s;
      s.begin(t.linearRgb, 0.75f);
      s.depositDab(*od.document.layers[0].rgbTiles, t, Vec2{48.0f, 48.0f}, 96, 96, nullptr,
                   nullptr);
      s.end();
      std::vector<float> out;
      for (int32_t y = 36; y <= 60; ++y)
        for (int32_t x = 36; x <= 60; ++x)
          for (const float c : readRgb(*od.document.layers[0].rgbTiles, x, y)) out.push_back(c);
      return out;
    };
    const std::vector<float> without = paint(false);
    const std::vector<float> withDisabled = paint(true);
    check(without.size() == withDisabled.size() && without == withDisabled,
          "paper/off: a paper attached but disabled changes not one texel, at zero tolerance");
  }

  return ok;
}

}  // namespace np
