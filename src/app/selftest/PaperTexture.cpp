#include "app/selftest/Support.hpp"

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
  auto makeChecker = [](int32_t w, int32_t h) {
    auto f = std::make_shared<PaperField>();
    f->width = w;
    f->height = h;
    f->height8.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x)
        f->height8[static_cast<size_t>(y) * w + x] = ((x + y) % 2 == 0) ? 255 : 0;
    return f;
  };
  // A left-to-right ramp, for the shaping checks where a two-value field
  // cannot distinguish brightness from contrast.
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
          "paper/sample: a texel reads its own height, 255 -> depth and 0 -> 0");

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
    r.field = makeRamp(5);  // 0, 63, 127, 191, 255 -> 0, .247, .498, .749, 1
    const float mid = grainHeightAt(r, 2, 0);

    GrainParams rb = r;
    rb.brightness = 0.25f;
    check(grainHeightAt(rb, 2, 0) > mid && grainHeightAt(rb, 4, 0) == 1.0f,
          "paper/brightness: lifts the field, and clamps rather than overflowing");

    GrainParams rc = r;
    rc.contrast = 1.0f;  // (h - 0.5) * 2 + 0.5
    check(grainHeightAt(rc, 0, 0) == 0.0f && grainHeightAt(rc, 4, 0) == 1.0f &&
              grainHeightAt(rc, 3, 0) > grainHeightAt(r, 3, 0),
          "paper/contrast: pivots about mid-grey, pushing highs up and lows down");
    GrainParams rflat = r;
    rflat.contrast = -1.0f;  // (h - 0.5) * 0 + 0.5 -- every texel identical
    check(grainHeightAt(rflat, 0, 0) == 0.5f && grainHeightAt(rflat, 4, 0) == 0.5f,
          "paper/contrast: at -1 the paper goes perfectly flat, every texel mid-grey");
  }

  // ======================================================================
  std::printf("  -- C. Height and Subtract really are one formula --\n");
  // ======================================================================
  {
    // brush/CoverageBlend.hpp claims the two ids "resolve to the same
    // formula". They only do because `grainCoverageAt()` routes BOTH to
    // `grainOverlayFraction()`; routed through `applyCoverageBlend()` instead,
    // Height would clamp its base to [0,1] before subtracting and Subtract
    // would not, so any `strength` above 1 -- which GrainParams::strength
    // explicitly permits -- would make them diverge while the header said
    // they could not. The grid below deliberately includes strength 1.75.
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
      GrainParams b = a;
      b.blend = CoverageBlend::Height;
      for (int ci = 0; ci <= 8; ++ci) {
        const float cov = static_cast<float>(ci) / 8.0f;
        for (int32_t y = 0; y < 4; ++y)
          for (int32_t x = 0; x < 4; ++x) {
            if (grainCoverageAt(a, cov, x, y) != grainCoverageAt(b, cov, x, y)) same = false;
            if (strength > 1.0f && cov > 0.75f && grainCoverageAt(a, cov, x, y) > 0.0f)
              strengthAbove1Reached = true;
          }
      }
    }
    check(same, "paper/Height: identical to Subtract at every strength, including above 1");
    check(strengthAbove1Reached,
          "paper/Height: and the strength>1 case is actually reached, not vacuously equal");
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
