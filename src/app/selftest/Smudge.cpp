#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/Smudge.hpp"
#include "core/SelectionShapes.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The smudge tool (brush/Smudge), and the routing that made it exist at all
// (app/StrokeSession section 1).
//
// See app/SelfTest.hpp for the section's own contents list and brush/Smudge.hpp
// for every decision this file only checks. Three things are worth saying here
// because they are what the section is *for*:
//
//   * **The tool did nothing.** `Tool::Smudge` sat in `strokeRouteFor()`'s
//     not-built list beside the rest of the name/icon/slot-only cells, so a drag
//     with it reached no layer, wrote no texel, produced no message and recorded
//     nothing. It drew a cursor ring and took a keystroke.
//   * **The load-bearing assertion is DIRECTIONAL** (section 4). Every other
//     assertion here would pass just as happily against a tool that blurred, or
//     one that picked a colour up once and stamped it. What only a real smudge
//     does is move colour *along the drag*: a stroke from paint into empty
//     canvas leaves colour beyond the boundary, the amount falls off with
//     distance, and the SAME stroke run backwards leaves that region untouched.
//     All three are asserted, and the third is what makes the first two
//     non-vacuous.
//   * **Both endpoints of STRENGTH are provable, not approximate.** 0 is a
//     byte-identical no-op with no history entry -- and the near-miss version
//     (leaving strength out of the write weight) is a *blur*, which writes every
//     texel on the path, so it is computed on the identical inputs and asserted
//     to be different. 1 carries the colour picked up at pen-down to the far end
//     of the stroke at **zero tolerance**.
// ---------------------------------------------------------------------------
bool runSmudgeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerances -------------------------------------------------------
  //
  //  * **kHalfRel / kHalfFloor** -- binary16 storage, which is what a
  //    `core::Tile` texel is. An 11-bit significand gives a round-to-nearest
  //    relative error of at most 2^-11 = 4.883e-04 for a normal value, plus an
  //    absolute floor of half a subnormal ulp, 2^-25 = 2.980e-08. Derived from
  //    the storage here rather than borrowed from runRgbEraseTest(), which
  //    states the identical derivation for the identical storage -- a tolerance
  //    copied without its derivation is the one that later gets applied where it
  //    does not hold.
  //
  //  * Several assertions below are at **exactly zero** tolerance and each says
  //    why in place. The strength-1 ones are exact because `std::lerp(a, b, 1)`
  //    is specified to return `b`, and the fixture colour is built from
  //    f16-exact values (0.75, 0.25, 0.125 and 1.0 are all short sums of powers
  //    of two), so the whole chain -- pick-up, finger, write, round trip through
  //    half -- is exact rather than lucky. The strength-0 ones are exact because
  //    they are claims about which operations *happen*: a no-op that wrote an
  //    equal value would still unshare and dirty the tile.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // The fixture is written straight into the store rather than painted with
  // brush/RgbDeposit first, for runRgbEraseTest()'s stated reason: this
  // section's subject is what the smudge does to colour that is already there,
  // and building the "already there" out of another module would make every
  // number below depend on two arithmetics instead of one.
  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), premultiplied);
      }
    }
  };

  // A hard disc, so `dabCoverage()` is exactly 1.0f over the whole footprint and
  // every number below is about the smudge rather than about the falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h) {
    return makeBlankOpenDocument(w, h, WorkingSpace{}, "smudge");
  };

  // **Deliberately uneven, and deliberately f16-exact.** Three equal channels
  // would pass just as happily against an implementation that carried the wrong
  // one; and every component is a short sum of powers of two, which is what lets
  // section 5's strength-1 claim be made at zero tolerance rather than at an f16
  // bound.
  const std::array<float, 4> kPaint{0.75f, 0.25f, 0.125f, 1.0f};
  const std::array<float, 4> kNothing{0.0f, 0.0f, 0.0f, 0.0f};

  // The straight drag every directional assertion below uses: dabs every 2 px
  // along y = 64, starting deep inside the filled block (which ends at x = 63)
  // and running well past it. Explicit positions rather than
  // `StrokeSession::addPoint()`, because the claim is about the ENGINE's carried
  // state and a Catmull-Rom fit between samples would put the dab positions
  // themselves into the argument.
  auto dragDabs = [](float fromX, float toX) {
    std::vector<Vec2> dabs;
    const float step = fromX < toX ? 2.0f : -2.0f;
    for (float x = fromX; step > 0.0f ? x <= toX : x >= toX; x += step)
      dabs.push_back(Vec2{x, 64.0f});
    return dabs;
  };

  // ======================================================================
  // 1. smudgeTexel(): the write, and strength 0 as a BIT-EXACT no-op
  // ======================================================================
  //
  // brush/Smudge section 3. Asserted on the pure function rather than on a tile
  // of it, because the claims are about this arithmetic and nothing else.
  {
    const std::array<float, 4> dst{0.8f, 0.4f, 0.2f, 1.0f};
    const std::array<float, 4> finger{0.0f, 0.5f, 0.5f, 0.5f};

    // **Strength 0 is the requirement the whole design turns on.** Not "small",
    // not "within a tolerance": the returned texel must BE the argument, so the
    // caller's `dabAlpha > 0` test fails and no tile is fetched, unshared,
    // reported or re-uploaded.
    const SmudgeStep zero = smudgeTexel(dst, finger, 1.0f, 0.0f);
    check(zero.premultiplied == dst && zero.dabAlpha == 0.0f,
          "strength 0: the texel comes back BIT-IDENTICAL with dabAlpha 0 -- the caller's "
          "signal not to fetch a write handle at all, at any flow and any coverage");

    // The near-miss, computed on the identical numbers and asserted to be
    // *different*, so the assertion above cannot pass against it. Leaving
    // `strength` out of the write weight gives `a = flow * cov`, which mixes the
    // texel toward the local mean -- a blur, which writes every texel it
    // touches. Printed because "it would have blurred" is worth seeing rather
    // than believing.
    const SmudgeStep blurred = smudgeTexel(dst, finger, 1.0f, 1.0f);
    std::printf("  [measured] strength 0 leaves a=%.4f (alpha %.4f); the rejected "
                "strength-free weight writes a=%.4f (alpha %.4f)\n",
                static_cast<double>(zero.dabAlpha), static_cast<double>(zero.premultiplied[3]),
                static_cast<double>(blurred.dabAlpha),
                static_cast<double>(blurred.premultiplied[3]));
    check(blurred.dabAlpha > 0.0f && blurred.premultiplied != dst,
          "strength 0: and the rejected model (strength left out of the write weight) DOES "
          "write on these same inputs -- so the no-op above is a real assertion");

    // The lerp itself, all four channels at one factor (section 5). A
    // half-weight dab lands each channel exactly half way, which is exact in
    // binary32 for these values.
    const SmudgeStep half = smudgeTexel(dst, finger, 1.0f, 0.5f);
    // **The claim is that ONE factor moved all four**, so what is measured is
    // the factor each channel actually moved by -- `(out - dst) / (finger -
    // dst)`, which is `a` if and only if the same `a` was applied to each. Every
    // one of the four gaps here is non-zero and they differ in sign and
    // magnitude, so a rule that scaled the alpha alone, or that used a different
    // factor per channel, cannot produce four equal quotients. At a binary32
    // epsilon rather than exactly, because `a + t*(b - a)` and `(a + b) * 0.5`
    // are the same number in exact arithmetic and not always in floats.
    bool halfway = true;
    for (int c = 0; c < 4; ++c) {
      const float moved = (half.premultiplied[c] - dst[c]) / (finger[c] - dst[c]);
      if (std::fabs(moved - half.dabAlpha) > 1e-6f) halfway = false;
    }
    check(half.dabAlpha == 0.5f && halfway,
          "the write: one lerp factor across ALL FOUR premultiplied channels -- alpha moves "
          "with colour, which is what lets a smudge smear an edge into blank canvas");

    // `a` is clamped rather than left to a `min` that does not exist on this
    // route: a flow above 1 is legitimate upstream, and an unclamped mix
    // fraction would extrapolate PAST the finger into negative alpha on a soft
    // rim.
    const SmudgeStep over = smudgeTexel(dst, finger, 4.0f, 1.0f);
    check(over.dabAlpha == 1.0f && over.premultiplied == finger,
          "the write: a weight above 1 is CLAMPED, so one dab reaches the finger and does "
          "not overshoot past it -- there is no accumulator min here to have capped it");

    // The refusals, each returning the argument bit-identically.
    check(smudgeTexel(dst, finger, 0.0f, 1.0f).dabAlpha == 0.0f &&
              smudgeTexel(dst, finger, -1.0f, 1.0f).premultiplied == dst &&
              smudgeTexel(kNothing, kNothing, 1.0f, 1.0f).dabAlpha == 0.0f,
          "the write: no coverage, a negative weight, and a finger that already equals the "
          "texel all refuse -- the last is what makes empty-on-empty cost nothing");
  }

  // ======================================================================
  // 2. smudgeFinger(): the first dab LOADS, and both endpoints are exact
  // ======================================================================
  //
  // brush/Smudge section 3. The first-dab latch is the one that would be easy to
  // skip, and skipping it breaks the tool at exactly the setting a user reaches
  // for when they want the strongest effect.
  {
    const std::array<float, 4> pick{0.6f, 0.3f, 0.1f, 0.75f};
    const std::array<float, 4> carried{0.2f, 0.8f, 0.4f, 1.0f};

    check(smudgeFinger(kNothing, false, pick, 1.0f) == pick &&
              smudgeFinger(carried, false, pick, 0.5f) == pick,
          "first dab: an unloaded finger takes the pick-up OUTRIGHT whatever strength says "
          "-- blending from a zero start would make a strength-1 smudge do nothing at all");

    check(smudgeFinger(carried, true, pick, 1.0f) == carried,
          "strength 1: a loaded finger is retained EXACTLY -- lerp(pick, finger, 1) is "
          "specified to return finger, which is what carries one colour indefinitely");
    check(smudgeFinger(carried, true, pick, 0.0f) == pick,
          "strength 0: the finger is replaced by the pick-up outright -- it holds no memory "
          "at all, which is the other half of why the tool is a no-op there");

    bool mid = true;
    const std::array<float, 4> blended = smudgeFinger(carried, true, pick, 0.5f);
    for (int c = 0; c < 4; ++c)
      if (blended[c] != (pick[c] + carried[c]) * 0.5f) mid = false;
    check(mid,
          "between the ends: the finger decays toward the pick-up at exactly the strength -- "
          "one geometric factor per dab, which is section 4's falloff");
  }

  // ======================================================================
  // 3. The pick-up is the COVERAGE-WEIGHTED mean, and emptiness counts
  // ======================================================================
  //
  // brush/Smudge section 2. Two claims, and the second is the one that separates
  // a smudge from a clone stamp: an absent tile contributes transparent black
  // rather than being skipped, so the carried colour thins as the tip leaves the
  // paint.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // **Filled to a TILE boundary, not to an arbitrary column.** 128 is
    // `kTileSize`, so the empty half of the footprint below is in a tile that
    // does not exist at all rather than in unwritten texels of one that does --
    // and those are two different branches of the pick-up loop. Filling to x=63
    // exercised only the second, and a sabotage that skipped absent tiles
    // outright then reddened nothing.
    fillRect(store, 0, 0, 127, 255, kPaint);
    const size_t seeded = store.occupiedTileCount();

    // Centred ON that boundary: a hard disc at x = 128 with radius 8 covers
    // x in [120, 135], of which [120, 127] is paint in an existing tile and
    // [128, 135] is an absent tile -- exactly half by count, since the disc is
    // symmetric about x = 128 (texels are sampled at their centres, so x=127
    // sits at dx = -0.5 and x=128 at dx = +0.5).
    //
    // **`flow` is 0 deliberately**: the dab writes nothing, so this measures the
    // pick-up alone rather than the pick-up composed with whatever the same dab
    // then laid down.
    SmudgeStroke s;
    s.begin(1.0f);
    s.smudgeDab(store, discTip(8.0f, 0.0f), Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    const std::array<float, 4> picked = s.finger();
    std::printf("  [measured] pick-up over a footprint half paint, half empty: "
                "(%.4f %.4f %.4f a=%.4f); half the paint is (%.4f %.4f %.4f a=%.4f)\n",
                static_cast<double>(picked[0]), static_cast<double>(picked[1]),
                static_cast<double>(picked[2]), static_cast<double>(picked[3]),
                static_cast<double>(kPaint[0] * 0.5), static_cast<double>(kPaint[1] * 0.5),
                static_cast<double>(kPaint[2] * 0.5), static_cast<double>(kPaint[3] * 0.5));
    check(s.loaded() && nearHalf(picked[3], 0.5f) && nearHalf(picked[0], kPaint[0] * 0.5f) &&
              nearHalf(picked[1], kPaint[1] * 0.5f) && nearHalf(picked[2], kPaint[2] * 0.5f),
          "pick-up: an absent tile contributes TRANSPARENT BLACK to the mean rather than "
          "being skipped -- half a footprint of paint picks up half the paint, on all four "
          "channels, which is why a smear thins instead of cloning");
    check(store.occupiedTileCount() == seeded,
          "pick-up: reading is not writing -- a flow-0 dab whose footprint straddles two "
          "ABSENT tiles loaded the finger and allocated neither");
    s.end();
    check(!s.loaded() && s.finger() == kNothing,
          "pen-up empties the finger -- a colour carried across strokes would lay the "
          "previous stroke's paint down before this one had picked anything up");

    // Wholly inside the paint: the mean of one repeated value is that value
    // exactly, because `sum / sum` is exactly 1.0 for any finite non-zero sum.
    SmudgeStroke inside;
    inside.begin(1.0f);
    inside.smudgeDab(store, discTip(8.0f, 0.0f), Vec2{32.0f, 128.0f}, 256, 256, nullptr,
                     nullptr);
    check(inside.finger() == kPaint,
          "pick-up: a footprint of one uniform colour picks up that colour at ZERO "
          "tolerance -- the weighted mean's divisor is its own numerator's weight sum");
    inside.end();
  }

  // ======================================================================
  // 4. THE DIRECTIONAL CLAIM: colour crosses the boundary, and falls off
  // ======================================================================
  //
  // This is the assertion the tool exists for and the one no weaker
  // implementation passes. A blur would not carry colour past the boundary at
  // all beyond one tip radius. A pick-once-and-stamp would carry it with no
  // falloff. And a tool that merely spread colour symmetrically would leave the
  // same trail whichever way the stroke was drawn -- which is why the reverse
  // drag is asserted here rather than in a section of its own.
  {
    OpenDocument od = makeRgbDoc(256, 128);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 63, 127, kPaint);

    SmudgeStroke s;
    s.begin(0.75f);
    const StrokeDeposit out =
        s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    s.end();

    std::printf("  [measured] alpha along the drag past the boundary at x=63:");
    for (int32_t x = 76; x <= 196; x += 20)
      std::printf(" %d:%.5f", x, static_cast<double>(readAt(store, x, 64)[3]));
    std::printf("  (%zu dabs, %zu texels)\n", out.dabs, out.texels);

    // x = 76 is thirteen texels past the last filled column and more than one
    // tip radius past it, so nothing that reaches it can be explained by a
    // single dab straddling the edge.
    const float nearAlpha = readAt(store, 76, 64)[3];
    const float farAlpha = readAt(store, 196, 64)[3];
    check(nearAlpha > 0.05f,
          "direction: colour is left BEYOND the original boundary -- thirteen texels past "
          "the last filled column, further than one tip radius, so no single dab explains it");

    // Monotone, sampled every 4 texels across the whole tail. `<=` rather than
    // `<` because two adjacent texels can round to the same binary16 value once
    // the trail is faint; the strict claim is the ratio beside it.
    bool monotone = true;
    float prev = nearAlpha;
    for (int32_t x = 80; x <= 196; x += 4) {
      const float a = readAt(store, x, 64)[3];
      if (a > prev) monotone = false;
      prev = a;
    }
    std::printf("  [measured] near alpha %.6f, far alpha %.6f, ratio %.1fx\n",
                static_cast<double>(nearAlpha), static_cast<double>(farAlpha),
                static_cast<double>(nearAlpha / (farAlpha > 0.0f ? farAlpha : 1e-9f)));
    check(monotone && nearAlpha > farAlpha * 4.0f,
          "direction: and the amount FALLS OFF along the drag -- monotone across the whole "
          "tail and at least 4x weaker at the far end, which is the finger decaying by one "
          "geometric factor per dab rather than a distance ramp anyone coded");

    // **The same stroke backwards.** It starts on empty canvas with an empty
    // finger, so section 6's equality test refuses every texel until the tip
    // reaches paint -- and by then it is past the whole region checked above. A
    // tool that spread colour symmetrically, or one that blurred, would leave a
    // trail here.
    OpenDocument rev = makeRgbDoc(256, 128);
    TileStore& revStore = *rev.document.layers[0].rgbTiles;
    fillRect(revStore, 0, 0, 63, 127, kPaint);
    SmudgeStroke back;
    back.begin(0.75f);
    back.smudgeDabs(revStore, discTip(6.0f, 1.0f), dragDabs(200.0f, 32.0f), 256, 128, nullptr);
    back.end();
    check(readAt(revStore, 76, 64) == kNothing && readAt(revStore, 196, 64) == kNothing,
          "direction: the SAME drag run BACKWARDS leaves that whole region bit-identically "
          "empty -- colour travels the way the stroke does, which is what makes the two "
          "assertions above non-vacuous");
  }

  // ======================================================================
  // 5. Both endpoints of STRENGTH, end to end
  // ======================================================================
  {
    // **Strength 1 carries indefinitely, at zero tolerance.** flow 1 and a hard
    // disc make the write weight exactly 1, so each dab assigns the finger
    // outright; strength 1 means the finger is never updated after the first dab
    // loads it; and the fixture colour is f16-exact. So the far end of the drag
    // holds the colour picked up at pen-down, bit for bit.
    OpenDocument od = makeRgbDoc(256, 128);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 63, 127, kPaint);
    SmudgeStroke s;
    s.begin(1.0f);
    s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    check(s.finger() == kPaint,
          "strength 1: the finger still holds the colour it picked up at pen-down after the "
          "whole drag -- no decay at all, which is the setting a user picks to drag one "
          "colour across a canvas");
    s.end();
    check(readAt(store, 196, 64) == kPaint && readAt(store, 100, 64) == kPaint,
          "strength 1: and that colour is on the layer at the far end of the stroke, at ZERO "
          "tolerance -- 133 texels past the boundary it came from");

    // **Strength 0 is a no-op for the whole stroke, byte for byte.** Not "small
    // enough to ignore": not one tile is allocated, none is reported, and
    // section 9's history rule then follows for free.
    OpenDocument quiet = makeRgbDoc(256, 128);
    TileStore& quietStore = *quiet.document.layers[0].rgbTiles;
    fillRect(quietStore, 0, 0, 63, 127, kPaint);
    const size_t tilesBefore = quietStore.occupiedTileCount();
    std::vector<std::array<float, 4>> snapshot;
    for (int32_t x = 0; x < 256; ++x) snapshot.push_back(readAt(quietStore, x, 64));

    SmudgeStroke none;
    none.begin(0.0f);
    const StrokeDeposit nothing = none.smudgeDabs(
        quietStore, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, nullptr);
    none.end();
    bool unchanged = quietStore.occupiedTileCount() == tilesBefore;
    for (int32_t x = 0; x < 256; ++x)
      if (readAt(quietStore, x, 64) != snapshot[static_cast<size_t>(x)]) unchanged = false;
    std::printf("  [measured] a strength-0 drag of %zu dabs wrote %zu texels and reported "
                "%zu tiles\n",
                nothing.dabs, nothing.texels, nothing.tiles.size());
    check(unchanged && nothing.texels == 0 && nothing.tiles.empty() && nothing.dabs > 50,
          "strength 0: a whole drag across real paint writes NOT ONE texel and reports not "
          "one tile -- the layer is byte-identical, so there is nothing to re-upload and "
          "nothing to undo");
  }

  // ======================================================================
  // 6. The selection bounds the write (PRD E1, P0)
  // ======================================================================
  //
  // brush/Smudge section 4. What a runaway smudge destroys outside a selection
  // drawn to protect it is invisible until the layer under it is, and one undo
  // step covers the whole stroke -- so the untouched region is asserted
  // bit-identical rather than approximately.
  {
    OpenDocument od = makeRgbDoc(256, 128);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 63, 127, kPaint);
    // Everything left of x = 120 may be edited; everything from there on may not.
    const Selection sel = selectRectangle(0.0f, 0.0f, 120.0f, 128.0f);

    SmudgeStroke s;
    s.begin(0.9f);
    s.smudgeDabs(store, discTip(6.0f, 1.0f), dragDabs(32.0f, 200.0f), 256, 128, &sel);
    s.end();

    const bool insideMoved = readAt(store, 100, 64)[3] > 0.02f;
    bool outsideClean = true;
    for (int32_t x = 121; x < 256; ++x)
      if (readAt(store, x, 64) != kNothing) outsideClean = false;
    check(insideMoved && outsideClean,
          "selection: the smear reaches x=100 inside the ants and stops DEAD at the "
          "boundary -- every texel beyond it is bit-identically untouched by a stroke whose "
          "dabs ran 80 texels past it");

    // The engaged-but-absent-tile case, which core/SelectionMask.hpp requires
    // each hoisted loop to drive: a null *Selection* is "no restriction", a null
    // *tile* inside an engaged one is "selects nothing", and getting the two the
    // same way round is how a tool starts smearing outside the ants.
    OpenDocument od2 = makeRgbDoc(256, 128);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 0, 0, 63, 127, kPaint);
    const Selection elsewhere = selectRectangle(200.0f, 100.0f, 250.0f, 127.0f);
    SmudgeStroke s2;
    s2.begin(0.9f);
    s2.smudgeDabs(store2, discTip(6.0f, 1.0f), dragDabs(32.0f, 100.0f), 256, 128, &elsewhere);
    s2.end();
    check(readAt(store2, 32, 64) == kPaint && readAt(store2, 76, 64) == kNothing,
          "selection: a drag entirely outside an engaged selection changes NOTHING -- an "
          "absent selection tile is \"selects nothing\", the inverse of a mask's absent tile");

    // And once more through `StrokeSession`, because PRD E1 is a claim about the
    // TOOL and not about the module: the session reads `OpenDocument::selection`
    // live, and a route that forgot to pass it would pass everything above.
    OpenDocument od3 = makeRgbDoc(256, 128);
    fillRect(*od3.document.layers[0].rgbTiles, 0, 0, 63, 127, kPaint);
    od3.selection = selectRectangle(0.0f, 0.0f, 120.0f, 128.0f);
    BrushTip sessionTip = discTip(6.0f, 1.0f);
    sessionTip.opacity = 0.9f;
    StrokeSession sess;
    std::string err;
    check(sess.begin(od3, 0, sessionTip, Tool::Smudge, &err) && err.empty() &&
              sess.route() == StrokeRoute::Smudge,
          "selection: a smudge session begins on the RGB layer and takes the Smudge route");
    for (int k = 0; k <= 21; ++k) sess.addPoint(32.0f + 8.0f * static_cast<float>(k), 64.0f);
    sess.end();
    const TileStore& gated = *od3.document.layers[0].rgbTiles;
    bool sessionOutsideClean = true;
    for (int32_t x = 121; x < 256; ++x)
      if (readAt(gated, x, 64) != kNothing) sessionOutsideClean = false;
    check(readAt(gated, 100, 64)[3] > 0.02f && sessionOutsideClean,
          "selection: PRD E1 through the SESSION -- the smear stops at the ants with the "
          "selection read live off the document rather than latched at pen-down");
  }

  // ======================================================================
  // 7. Smudging nothing with nothing costs nothing
  // ======================================================================
  //
  // brush/Smudge section 6, and it is the one place this route may NOT borrow
  // brush/RgbErase section 4's rule: a smudge grows the painted region, so it
  // has to be allowed to allocate. The condition is on the finger, not on the
  // tile, and both halves are asserted.
  {
    OpenDocument od = makeRgbDoc(512, 512);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;

    BrushTip t = discTip(24.0f, 1.0f);
    t.opacity = 1.0f;
    StrokeSession s;
    std::string err;
    s.begin(od, 0, t, Tool::Smudge, &err);
    for (int k = 0; k < 30; ++k) s.addPoint(40.0f + 14.0f * static_cast<float>(k), 250.0f);
    s.end();
    std::printf("  [measured] a %zu-dab drag across blank canvas: %zu texels, %zu tiles "
                "reported, %zu tiles in the store\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                store.occupiedTileCount());
    check(s.dabCount() > 20 && s.texelsWritten() == 0 && s.strokeTiles().empty() &&
              store.occupiedTileCount() == 0,
          "empty: dozens of dabs across blank canvas allocate NOT ONE 224 KiB tile -- the "
          "finger is the zero texel and every write is refused for equalling it");
    check(od.history.entries().size() == entries && od.revision == revBefore,
          "empty: and a stroke that smudged nothing records nothing -- no undo step that "
          "undoes nothing, and no revision bump to re-upload a frame for");

    // The other half: a LOADED finger over blank canvas must allocate, or the
    // tool could never smear an edge outwards at all.
    OpenDocument od2 = makeRgbDoc(512, 512);
    TileStore& store2 = *od2.document.layers[0].rgbTiles;
    fillRect(store2, 0, 32, 60, 96, kPaint);
    const size_t seeded = store2.occupiedTileCount();
    SmudgeStroke grow;
    grow.begin(1.0f);
    grow.smudgeDabs(store2, discTip(10.0f, 1.0f), dragDabs(30.0f, 300.0f), 512, 512, nullptr);
    grow.end();
    check(store2.occupiedTileCount() > seeded && readAt(store2, 290, 64)[3] > 0.0f,
          "empty: but a LOADED finger over blank canvas DOES allocate and write -- a smudge "
          "grows the painted region, which is why the eraser's unconditional skip could not "
          "be copied");
  }

  // ======================================================================
  // 8. The routing table's Smudge rows (app/StrokeSession section 1)
  // ======================================================================
  {
    Layer rgb;
    rgb.kind = LayerKind::RGB;
    rgb.rgbTiles.emplace();
    Layer pigment;
    pigment.kind = LayerKind::Pigment;
    pigment.pigmentTiles.emplace();
    Layer adjustment;
    adjustment.kind = LayerKind::Adjustment;
    Layer storeless;
    storeless.kind = LayerKind::RGB;

    check(strokeRouteFor(Tool::Smudge, &rgb) == StrokeRoute::Smudge &&
              strokeRouteWritesLayer(StrokeRoute::Smudge),
          "routing: Smudge on a writable RGB layer takes the Smudge route, and that route "
          "WRITES a layer -- it unshares tiles, moves the revision and owes one undo step");

    check(strokeRouteFor(Tool::Smudge, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: no target at all is None for the smudge and PaintSim for the brush -- "
          "the solver has no smudge step, so a smudge sent there would deposit the loaded "
          "foreground pigment instead of moving anything");

    check(strokeRouteFor(Tool::Smudge, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Eraser, &pigment) == StrokeRoute::PigmentErase &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer refuses the smudge BY NAME while it still takes the "
          "brush and the eraser -- the arithmetic mean of Kubelka-Munk latents is not what "
          "mixing those paints means here");

    rgb.locked = true;
    check(strokeRouteFor(Tool::Smudge, &rgb) == StrokeRoute::None,
          "routing: a locked layer refuses, through the shared body and with no case of its "
          "own -- locked before kind, so the UI names the one problem a user can fix");
    rgb.locked = false;

    rgb.alphaLocked = true;
    check(strokeRouteFor(Tool::Smudge, &rgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &rgb) == StrokeRoute::RgbDeposit,
          "routing: an ALPHA-locked layer refuses the smudge while it still takes the brush "
          "-- smudge moves alpha inseparably from colour, so letting it through would make "
          "the flag decorative");
    rgb.alphaLocked = false;

    check(strokeRouteFor(Tool::Smudge, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Smudge, &storeless) == StrokeRoute::None,
          "routing: an Adjustment layer and an RGB layer whose store was never allocated "
          "both refuse -- None rather than a silent fallthrough to the solver canvas");

    check(std::string(strokeRouteName(StrokeRoute::Smudge)) == "smudge" &&
              std::string(strokeEditLabel(Tool::Smudge)) == "smudge",
          "routing: the route and the history entry are both named \"smudge\" -- PRD O2's "
          "panel is scanned for a noun, and a smudge is the edit a user most wants to find");

    // **The canvas block's own gate, pinned to a set.** `ui/MacPaintUI.cpp`'s
    // `strokeTool` used to be a hand-written `tool == Brush || Water ||
    // DryBrush || Eraser`, and the smudge route was written, routed, flipped to
    // `toolImplemented()` and green in this suite *before* anyone noticed that
    // a drag still could not reach it -- the eyedropper's original defect
    // exactly, arriving through the one predicate in the chain that was still a
    // list of `Tool` values rather than the gate itself. That line reads
    // `toolBeginsStroke()` now, and this assertion is what keeps the set it
    // accepts a decision: a sixth stroke tool reddens it, and the author has to
    // answer for the canvas as well as for the route table.
    int accepting = 0;
    bool exactly = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const bool want = t == Tool::Brush || t == Tool::Water || t == Tool::DryBrush ||
                        t == Tool::Eraser || t == Tool::Pencil || t == Tool::Dodge ||
                        t == Tool::Burn || t == Tool::CloneStamp || t == Tool::Smudge;
      if (toolBeginsStroke(t) != want) exactly = false;
      if (toolBeginsStroke(t)) ++accepting;
    }
    check(exactly && accepting == 9,
          "routing: toolBeginsStroke() accepts EXACTLY Brush, Water, DryBrush, Eraser, Pencil, "
          "Dodge, Burn, Clone Stamp and Smudge -- the set ui/MacPaintUI's canvas gate now reads "
          "instead of its own list");

    check(toolBeginsStroke(Tool::Smudge) && toolImplemented(Tool::Smudge) &&
              toolHasCanvasHandler(Tool::Smudge) && !toolDrawsSelection(Tool::Smudge) &&
              !toolSamplesCanvas(Tool::Smudge),
          "routing: toolBeginsStroke() picks the new row up by PROBING strokeRouteFor(), so "
          "the palette cell, the cursor and the completeness check flip together -- and "
          "through that gate alone, not by widening another");
  }

  // ======================================================================
  // 9. One stroke is ONE undo step, labelled "smudge"
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(512, 512);
    fillRect(*od.document.layers[0].rgbTiles, 40, 200, 200, 300, kPaint);
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    const uint64_t structBefore = od.structuralRevision;

    BrushTip t = discTip(14.0f, 0.6f);
    t.hardness = 0.4f;
    t.opacity = 0.7f;
    StrokeSession s;
    std::string err;
    s.begin(od, 0, t, Tool::Smudge, &err);
    for (int i = 0; i < 40; ++i) {
      const float u = static_cast<float>(i) / 39.0f;
      s.addPoint(60.0f + 380.0f * u, 250.0f + 40.0f * std::sin(u * 3.1415926f));
    }
    check(od.history.entries().size() == entries,
          "undo: not one history entry has appeared mid-stroke, however many dabs it is");
    s.end();
    std::printf("  [measured] a %zu-dab smudge wrote %zu texels across %zu tiles; the entry "
                "is \"%s\"\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                od.history.entries().back().label.c_str());
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "smudge" && s.texelsWritten() > 0,
          "undo: a stroke of hundreds of dabs is EXACTLY ONE history entry, labelled "
          "\"smudge\" -- ADR-0005's stroke-granular undo, reached by the new route too");
    check(od.revision > revBefore && od.structuralRevision == structBefore,
          "undo: it moved the content revision and not the structural one, so a smudge costs "
          "at most one journal write per interval rather than one per frame");
  }

  std::printf("[selftest] smudge %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
