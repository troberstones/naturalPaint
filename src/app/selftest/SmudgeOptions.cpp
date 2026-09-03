#include "app/selftest/Support.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/StrokeSession.hpp"
#include "app/ToolSwitch.hpp"
#include "brush/Deposit.hpp"
#include "brush/Smudge.hpp"
#include "paint/Palette.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The smudge's own parameter block (brush/Smudge §3b): the strength that used
// to be the OPACITY slider, its default, and the tip override.
//
// See brush/Smudge.hpp §3b for every decision this file only checks. Three
// things are worth saying here because they are what the section is *for*:
//
//   * **The tool shipped at the one setting under which it provably cannot
//     work.** `strength` was `BrushTip::opacity`, whose default is 1, and
//     `lerp(pick, finger, 1)` returns `finger` exactly -- so the finger loaded
//     at pen-down was carried to the far edge of the canvas and the fade
//     `brush/Smudge` §5 derives never happened. Every assertion in
//     `runSmudgeTest()` was green throughout, because every one of them sets a
//     strength; not one of them asked what a user who had never found the
//     control would get. **That is the gap this section closes**, and section 4
//     below is the assertion that closes it: it runs the DEFAULT and requires a
//     fade, rather than running a chosen strength and requiring the arithmetic.
//   * **A default is a claim about a struct nobody wrote to.** So the checks
//     here are on default-constructed objects and on a freshly built
//     `AppState`, not on ones the test has configured -- the same shape
//     `app/selftest/BrushDynamics.cpp` section 13's "a freshly launched app is
//     not edited" uses, and for the same reason.
//   * **Decoupling is a NEGATIVE claim and needs a hostile fixture.** "Strength
//     is its own field" cannot be shown by setting the field and watching it
//     work; it is shown by setting the OLD field to a value that would be
//     obvious if anything still read it, and watching nothing happen. Sections
//     3 and 5 both do that, in opposite directions.
// ---------------------------------------------------------------------------
bool runSmudgeOptionsTest() {
  std::printf("[selftest] smudge options: strength decoupled from opacity, its default, the "
              "tool->block mapping, the tip override\n");
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), premultiplied);
      }
    }
  };
  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };
  // A hard disc, so `dabCoverage()` is 1 over the whole footprint and the
  // numbers below are about the carried colour rather than about the falloff --
  // `runSmudgeTest()`'s own fixture convention.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    return t;
  };
  const std::array<float, 4> kPaint{0.75f, 0.25f, 0.125f, 1.0f};

  // ======================================================================
  // 1. The default, and the two places it is spelled
  // ======================================================================
  //
  // brush/Smudge §3b. The number itself is the whole defect: any value
  // strictly inside (0,1) makes §5's decay real, and 1 is the one value that
  // makes it the identity.
  {
    const SmudgeParams fresh;
    check(fresh.strength == kSmudgeDefaultStrength && kSmudgeDefaultStrength == 0.5f,
          "default: SmudgeParams ships at kSmudgeDefaultStrength, and that is 0.5");

    check(fresh.strength > 0.0f && fresh.strength < 1.0f,
          "default: STRICTLY inside (0,1) -- 1 is the one strength at which the fade "
          "brush/Smudge section 5 derives is exactly the identity, and 0 is the no-op");

    check(fresh.dabId.empty() && fresh.tipBitmap == nullptr,
          "default: no tip override, so a user who never opens the TIP combo keeps the "
          "brush's own tip -- what this tool used for its whole history");

    // **The cross-header pin.** `BrushTip::smudgeStrength`'s default is spelled
    // as a literal because `brush/Deposit.hpp` cannot include
    // `brush/Smudge.hpp` (the include runs the other way). Two defaults for one
    // quantity is the drift `app/AppState.hpp` holds `FloodFillParams` itself in
    // order to avoid, so the agreement is asserted rather than inspected.
    const BrushTip bare;
    check(bare.smudgeStrength == kSmudgeDefaultStrength,
          "default: BrushTip's own literal agrees with kSmudgeDefaultStrength -- the one "
          "number brush/Deposit.hpp cannot name and must still match");

    // A freshly launched app, not a struct this test built: the field could
    // have a good default and still be overwritten during construction.
    AppState st;
    check(st.brush.smudge.strength == kSmudgeDefaultStrength && st.brush.smudge.dabId.empty(),
          "default: and a freshly built AppState is AT that default -- a default nothing "
          "reaches is not a default");
  }

  // ======================================================================
  // 2. smudgeToolParamsFor(): one mapping, walked over every Tool
  // ======================================================================
  //
  // app/AppState.hpp's own argument, and `floodToolParamsFor()`'s before it:
  // the options row edits a block and `brushTipFor()` consumes one, and two
  // spellings of "is this the smudge" agree until the day they do not.
  {
    AppState st;
    int accepting = 0;
    bool exactly = true;
    bool aliased = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      SmudgeParams* got = smudgeToolParamsFor(st.brush, t);
      if ((got != nullptr) != (t == Tool::Smudge)) exactly = false;
      if (got != nullptr) {
        ++accepting;
        // Not merely non-null: the block the row edits has to be the block the
        // stroke reads. A mapping that returned a static, or a copy, would be a
        // row of live controls over something no stroke ever sees -- which is
        // the failure this function exists to make unspellable.
        if (got != &st.brush.smudge) aliased = false;
      }
    }
    check(exactly && accepting == 1,
          "mapping: smudgeToolParamsFor() answers non-null for EXACTLY Tool::Smudge, walked "
          "over every Tool value -- a second smudging tool cannot be added silently");
    check(aliased,
          "mapping: and the pointer IS &brush.smudge -- the options row and the stroke read "
          "one struct, not two that agree today");
  }

  // ======================================================================
  // 3. brushTipFor() resolves the block, and OPACITY no longer reaches it
  // ======================================================================
  {
    MixboxLut lut;  // never loaded: the fallback path, and irrelevant to strength
    AppState st;
    setActiveTool(st, Tool::Smudge);
    st.brush.smudge.strength = 0.31f;
    // The hostile half. If anything still routed the smudge's strength through
    // the opacity slider, this is the value it would carry.
    st.brush.opacity = 1.0f;
    const BrushTip smudgeTip = brushTipFor(st.brush, lut, 1.0f);
    check(smudgeTip.smudgeStrength == 0.31f && smudgeTip.opacity == 1.0f,
          "resolve: brushTipFor() carries smudge.strength into BrushTip::smudgeStrength and "
          "leaves opacity alone -- two fields, two meanings, no aliasing");

    // The other direction: moving the OPACITY slider must not move the smudge's
    // strength by so much as a bit. This is the assertion that fails against a
    // repair that merely renamed the field it read.
    st.brush.opacity = 0.02f;
    const BrushTip afterOpacity = brushTipFor(st.brush, lut, 1.0f);
    check(afterOpacity.smudgeStrength == 0.31f,
          "resolve: dragging OPACITY from 1.00 to 0.02 does not move the smudge's strength "
          "-- brush/Smudge section 3b's whole point, stated as a negative");

    // And the block is read for the smudge and for no other tool: a brush
    // stroke's tip must not pick up a strength the brush route would never use
    // but `brushTipEqual()` would compare.
    setActiveTool(st, Tool::Brush);
    const BrushTip brushTip = brushTipFor(st.brush, lut, 1.0f);
    check(brushTip.smudgeStrength == kSmudgeDefaultStrength,
          "resolve: with the BRUSH selected the block is not read at all -- the tip keeps "
          "its own default however far the smudge's slider has been dragged");
  }

  // ======================================================================
  // 4. THE ASSERTION THE OLD SUITE DID NOT HAVE: at the DEFAULT, it fades
  // ======================================================================
  //
  // Every strength assertion in `runSmudgeTest()` sets a strength first. That
  // is why a shipped default of 1 -- the one value under which the tool does
  // not fade -- was green for the tool's whole history and was reported by a
  // user instead. This section sets NOTHING: it builds the tip the way the app
  // builds it, out of a freshly launched `AppState`, and requires the fade.
  {
    MixboxLut lut;
    AppState st;
    setActiveTool(st, Tool::Smudge);
    const BrushTip shipped = brushTipFor(st.brush, lut, 1.0f);

    // A block of paint ending at x = 63, and a straight drag out of it. Every
    // probe below is past that boundary, so what is read there arrived by
    // being carried.
    OpenDocument od = makeBlankOpenDocument(512, 128, WorkingSpace{}, "smudge-default");
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 63, 127, kPaint);
    SmudgeStroke s;
    s.begin(shipped.smudgeStrength);
    std::vector<Vec2> dabs;
    for (float x = 32.0f; x <= 400.0f; x += 2.0f) dabs.push_back(Vec2{x, 64.0f});
    s.smudgeDabs(store, shipped, dabs, 512, 128, nullptr);
    s.end();

    const float nearEdge = readAt(store, 80, 64)[3];
    const float farOut = readAt(store, 300, 64)[3];
    std::printf("  [measured] at the SHIPPED default (strength %.2f): alpha %.6f at x=80, "
                "%.6f at x=300, finger alpha %.6f\n",
                shipped.smudgeStrength, nearEdge, farOut, s.finger()[3]);
    check(nearEdge > 0.05f,
          "default fade: the smear does leave the paint -- colour is carried past the "
          "boundary at the shipped default, so this is a fade and not a dead tool");
    // 100x rather than "some fall": at strength 1 this ratio is exactly 1, and
    // an assertion that only asked for a decrease would pass against a tool
    // that faded by a rounding error.
    check(farOut * 100.0f < nearEdge,
          "default fade: and it is at least 100x weaker 220 texels further along -- the "
          "user report ('it never fades') asserted directly, at the default nobody sets");

    // The negative that makes it non-vacuous: the SAME fixture at strength 1,
    // which is what shipped. If this passed too, the assertion above would be
    // measuring the fixture rather than the default.
    OpenDocument od1 = makeBlankOpenDocument(512, 128, WorkingSpace{}, "smudge-strength-1");
    TileStore& store1 = *od1.document.layers[0].rgbTiles;
    fillRect(store1, 0, 0, 63, 127, kPaint);
    SmudgeStroke s1;
    s1.begin(1.0f);
    s1.smudgeDabs(store1, shipped, dabs, 512, 128, nullptr);
    s1.end();
    const float carried = readAt(store1, 300, 64)[3];
    std::printf("  [measured] the same drag at strength 1 (what shipped): alpha %.6f at "
                "x=300, %.0fx the default's\n",
                carried, farOut > 0.0f ? carried / farOut : 0.0f);
    check(carried > 0.5f && carried > farOut * 100.0f,
          "default fade: NEGATIVE -- the identical drag at strength 1 is still over half "
          "opaque out there, so the assertion above is about the default and not the drag");
  }

  // ======================================================================
  // 5. End to end through StrokeSession: opacity 0 smudges, strength 0 does not
  // ======================================================================
  //
  // Section 3 proves the plumbing; this proves the tool. Both fixtures set the
  // two fields to OPPOSITE extremes, so either one being read in the other's
  // place flips both assertions rather than neither.
  {
    OpenDocument od = makeBlankOpenDocument(256, 128, WorkingSpace{}, "smudge-opacity-0");
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 63, 127, kPaint);
    BrushTip t = discTip(6.0f, 1.0f);
    t.smudgeStrength = 0.9f;
    t.opacity = 0.0f;  // the field this route used to latch, at its deadest value
    StrokeSession s;
    std::string err;
    s.begin(od, 0, t, Tool::Smudge, &err);
    for (int k = 0; k <= 30; ++k) s.addPoint(32.0f + 6.0f * static_cast<float>(k), 64.0f);
    s.end();
    check(s.texelsWritten() > 0 && readAt(*od.document.layers[0].rgbTiles, 100, 64)[3] > 0.02f,
          "decoupled: a stroke at OPACITY 0 and strength 0.9 smudges normally -- the old "
          "wiring would have made this a whole-stroke no-op");

    OpenDocument od2 = makeBlankOpenDocument(256, 128, WorkingSpace{}, "smudge-strength-0");
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 0, 0, 63, 127, kPaint);
    const size_t entries = od2.history.entries().size();
    const uint64_t revBefore = od2.revision;
    const size_t tilesBefore = store2.occupiedTileCount();
    BrushTip t2 = discTip(6.0f, 1.0f);
    t2.smudgeStrength = 0.0f;
    t2.opacity = 1.0f;  // and here the opposite, for the same reason
    StrokeSession s2;
    s2.begin(od2, 0, t2, Tool::Smudge, &err);
    for (int k = 0; k <= 30; ++k) s2.addPoint(32.0f + 6.0f * static_cast<float>(k), 64.0f);
    s2.end();
    // Bit-exact, per brush/Smudge §3: the claim is about which operations
    // HAPPEN, and a no-op that wrote an equal value would still unshare the
    // tile, move the revision and cost an undo step.
    bool untouched = true;
    for (int32_t x = 0; x < 256; ++x) {
      const std::array<float, 4> want =
          x <= 63 ? kPaint : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
      if (readAt(store2, x, 64) != want) untouched = false;
    }
    check(s2.texelsWritten() == 0 && s2.strokeTiles().empty() && untouched &&
              store2.occupiedTileCount() == tilesBefore,
          "decoupled: strength 0 at OPACITY 1 is still a BIT-EXACT no-op -- not one texel, "
          "not one tile, the near-miss blur brush/Smudge section 3 names stays rejected");
    check(od2.history.entries().size() == entries && od2.revision == revBefore,
          "decoupled: and it records nothing -- no undo step that undoes nothing, reached "
          "through the new field rather than through the old one");
  }

  // ======================================================================
  // 6. The tip override (brush/Smudge section 3b, the report's second half)
  // ======================================================================
  {
    MixboxLut lut;
    AppState st;

    // A tiny hand-built coverage bitmap. Built here rather than resolved out of
    // `app/DabLibrary`, because the claim is about the SUBSTITUTION and a test
    // that needed a dab folder on disk would be a test of the folder.
    auto smear = std::make_shared<BrushTipBitmap>();
    smear->width = 2;
    smear->height = 2;
    smear->alpha = {255, 255, 255, 255};
    auto brushOwn = std::make_shared<BrushTipBitmap>();
    brushOwn->width = 2;
    brushOwn->height = 2;
    brushOwn->alpha = {255, 0, 0, 255};
    st.brush.tipBitmap = brushOwn;

    setActiveTool(st, Tool::Smudge);
    check(brushTipFor(st.brush, lut, 1.0f).bitmap == brushOwn,
          "tip: an empty dabId leaves the brush's own tip in place -- the default, and what "
          "this tool has always dragged");

    st.brush.smudge.dabId = "test-smear";
    st.brush.smudge.tipBitmap = smear;
    check(brushTipFor(st.brush, lut, 1.0f).bitmap == smear,
          "tip: a set override replaces BrushTip::bitmap for the smudge -- the dab-shape "
          "half of 'picking a dab shape as well ... for smudge'");

    // The containment claim, and it is the reason the override is not just
    // `BrushState::tipBitmap`: picking a smear shape must not silently repaint
    // the next BRUSH stroke with it.
    setActiveTool(st, Tool::Brush);
    check(brushTipFor(st.brush, lut, 1.0f).bitmap == brushOwn,
          "tip: and it is invisible to every other tool -- the brush still paints with the "
          "brush's tip, so choosing a smear shape is not an edit to the brush");

    // An id whose bitmap did not resolve (a dab folder moved out from under the
    // app). Falling back to the brush's tip is the only answer that still
    // smudges: an empty bitmap is a tip with no coverage anywhere, which on
    // this route is indistinguishable from the tool being broken.
    setActiveTool(st, Tool::Smudge);
    st.brush.smudge.tipBitmap.reset();
    check(!st.brush.smudge.dabId.empty() &&
              brushTipFor(st.brush, lut, 1.0f).bitmap == brushOwn,
          "tip: an id that no longer resolves falls back to the brush's tip rather than to "
          "an empty one -- a tip with no coverage reads as a broken tool");
  }

  std::printf("[selftest] smudge options %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
