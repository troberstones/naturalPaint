#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/PencilDeposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "core/SelectionShapes.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The pencil on a plain RGB layer (brush/PencilDeposit), and the routing that
// made it exist at all (app/StrokeSession §1's Pencil rows).
//
// See app/SelfTest.hpp for the section's own contents list and
// brush/PencilDeposit.hpp for every decision this file only checks. Two things
// are worth saying here because they are what the section is *for*:
//
//   * **The design question this tool had to answer.** "A pencil is aliased"
//     is not, on its own, a difference from `brush/RgbDeposit` in THIS
//     codebase: `singleTipCoverage()` at `hardness == 1` already returns only
//     1 or 0. Section 3 below is the assertion that finds the real difference
//     -- a hard *dab* is not a hard *mark*, because at `flow < 1` a texel near
//     a stroke's rim is covered by fewer dabs than one on its spine and
//     therefore ends the stroke at a lower alpha. It runs a hardness-1
//     `RgbDeposit` stroke and a pencil stroke on identical inputs and counts
//     the distinct alphas each leaves. **An implementation of "the pencil is
//     RgbDeposit with hardness pinned to 1" passes sections 1, 2 and 4 and
//     fails section 3**, which is the whole reason section 3 is written the
//     way it is.
//   * **The threshold's one deliberate exception is the selection** (section
//     6). A feathered selection edge still feathers a pencil mark, because the
//     threshold is about the mark's own shape and a selection is a mask on
//     where it may land. That is asserted as a value, not left as prose, so a
//     later "tidy-up" that thresholded `sel` too would redden here rather than
//     silently hardening every feathered edge in the build.
// ---------------------------------------------------------------------------
bool runPencilDepositTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances -------------------------------------------------------
  //
  // **Most of this section is at EXACTLY zero tolerance, and that is a
  // property of the subject rather than optimism.** A pencil writes one value,
  // `opacity`, at every covered texel; the interesting claims are about which
  // texels are covered and how many distinct values come back, both of which
  // are counts. Where a stored value is compared against a computed one the
  // binary16 bound below is used and derived from the storage, the same
  // derivation `runRgbEraseTest()` and `runRgbDepositTest()` each state for
  // the same `core::Tile`: an 11-bit significand gives a round-to-nearest
  // relative error of at most 2^-11, plus an absolute floor of half a
  // subnormal ulp, 2^-25.
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

  auto makeRgbDoc = [](int32_t w, int32_t h) {
    return makeBlankOpenDocument(w, h, WorkingSpace{}, "pencil");
  };

  // The ink, chosen uneven on purpose: three equal channels would pass just as
  // happily against an implementation that wrote the wrong one.
  const std::array<float, 3> kInk{0.8f, 0.4f, 0.1f};

  // A texel is "partially covered" when its stored alpha is neither exactly 0
  // (untouched) nor exactly the stroke's own ceiling. Exact comparison is
  // right rather than approximate here: `1.0f` and `0.5f` are exact in
  // binary16, and the question being asked is "did this arithmetic produce a
  // fraction at all", not "is the fraction close to something".
  auto countAlphas = [&](const TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         float ceiling) {
    struct Counts {
      int zero = 0;
      int atCeiling = 0;
      int partial = 0;
    } c;
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const float a = readAt(store, x, y)[3];
        if (a == 0.0f) ++c.zero;
        else if (a == ceiling) ++c.atCeiling;
        else ++c.partial;
      }
    }
    return c;
  };

  // ======================================================================
  // 1. `pencilCoverage()` -- two values and nothing between
  // ======================================================================
  //
  // The threshold as a pure function, driven directly rather than through a
  // tile of deposited texels, for the reason brush/Deposit.cpp gives about
  // `combineDualCoverage()`: the rule IS the design, and an assertion that can
  // only see it through the loop is an assertion about the loop.
  {
    bool onlyTwoValues = true;
    bool monotone = true;
    float prev = 0.0f;
    for (int i = -20; i <= 220; ++i) {
      const float c = static_cast<float>(i) / 200.0f;  // -0.1 .. 1.1
      const float v = pencilCoverage(c);
      if (v != 0.0f && v != 1.0f) onlyTwoValues = false;
      if (v < prev) monotone = false;
      prev = v;
    }
    check(onlyTwoValues,
          "threshold: pencilCoverage() returns exactly 0 or exactly 1 over the whole range, "
          "including outside [0,1] -- never a fraction, which is the module's whole claim");
    check(monotone,
          "threshold: and it is monotone, so it is a threshold and not a mask -- a rule that "
          "dropped a MIDDLE band of coverages would pass the two-values test above");
    // The boundary read off the named constant rather than off a literal 0.5:
    // a test carrying its own copy of a threshold keeps passing after the
    // threshold moves.
    check(pencilCoverage(kPencilCoverageThreshold) == 1.0f &&
              pencilCoverage(std::nextafter(kPencilCoverageThreshold, 0.0f)) == 0.0f,
          "threshold: the boundary is INCLUSIVE at exactly kPencilCoverageThreshold, and the "
          "float one ulp below it is out -- the half-height contour, so a pencil and a brush "
          "at one radius draw marks of the same size");
    check(pencilCoverage(std::numeric_limits<float>::quiet_NaN()) == 0.0f &&
              pencilCoverage(1.0f) == 1.0f && pencilCoverage(0.0f) == 0.0f,
          "threshold: a NaN coverage refuses rather than drawing at full opacity -- the "
          "!(c >= t) guard shape depositRgbTexel() uses, where the c < t spelling would admit "
          "it");
  }

  // ======================================================================
  // 2. The aliased DAB: no partially covered texels, and the right set
  // ======================================================================
  //
  // One dab of each engine, identical tip, identical ink, identical ceiling,
  // on a blank layer. The pencil's mark must be two-valued; the soft brush's
  // must not be. The radius is small (4 px) because that is where the
  // difference is worst and where a pencil is actually used -- at radius 1 the
  // two would agree by accident, since a single texel centre falls under the
  // disc and gets full coverage from both.
  {
    OpenDocument pd = makeRgbDoc(128, 128);
    OpenDocument bd = makeRgbDoc(128, 128);
    TileStore& pencilStore = *pd.document.layers[0].rgbTiles;
    TileStore& brushStore = *bd.document.layers[0].rgbTiles;

    BrushTip t;
    t.radius = 4.0f;
    t.hardness = 0.35f;  // the shipped default -- the brush this is compared to
    t.flow = 1.0f;
    const Vec2 centre{64.5f, 64.5f};

    PencilStroke pencil;
    pencil.begin(kInk, 1.0f);
    const DepositCount pc = pencil.drawDab(pencilStore, t, centre, 128, 128, nullptr, nullptr);

    RgbStroke brush;
    brush.begin(kInk, 1.0f);
    const DepositCount bc = brush.depositDab(brushStore, t, centre, 128, 128, nullptr, nullptr);

    const auto pcount = countAlphas(pencilStore, 58, 58, 71, 71, 1.0f);
    const auto bcount = countAlphas(brushStore, 58, 58, 71, 71, 1.0f);
    std::printf("  [measured] r=4 hardness=0.35 dab: pencil %d opaque / %d partial, "
                "soft brush %d opaque / %d partial (%zu vs %zu texels written)\n",
                pcount.atCeiling, pcount.partial, bcount.atCeiling, bcount.partial, pc.texels,
                bc.texels);
    check(pcount.partial == 0 && pcount.atCeiling > 0,
          "aliased dab: the pencil's dab has NO partially covered texel at all -- every texel "
          "it wrote is at exactly the ceiling, which no falloff can produce");
    check(bcount.partial > 0,
          "aliased dab: and the soft brush's dab on the identical tip DOES have them, so the "
          "assertion above is a difference rather than a property of a 4 px disc");

    // The precise claim, not just the two-valued one: the set of texels the
    // pencil wrote is exactly the set whose coverage reached the threshold.
    // Computed from `dabCoverage()` here rather than restated as a radius, so
    // an ellipse, a bitmap tip or a dual brush would be covered by the same
    // line.
    bool setMatches = true;
    for (int32_t y = 56; y <= 73; ++y) {
      for (int32_t x = 56; x <= 73; ++x) {
        const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        const bool want = dabCoverage(t, dx, dy) >= kPencilCoverageThreshold;
        const bool got = readAt(pencilStore, x, y)[3] != 0.0f;
        if (want != got) setMatches = false;
      }
    }
    check(setMatches,
          "aliased dab: and the texels it wrote are EXACTLY those whose dabCoverage() reached "
          "the threshold -- the mark's edge is the falloff's half-height contour, derived "
          "from the shared shape function rather than from a second radius here");

    // Premultiplied, and the ink unchanged: this route reuses
    // `depositRgbTexel()` (brush/PencilDeposit §4) precisely so that it cannot
    // disagree with the brush about colour, and that is worth one value.
    const std::array<float, 4> centreTexel = readAt(pencilStore, 64, 64);
    check(nearHalf(centreTexel[0], kInk[0]) && nearHalf(centreTexel[1], kInk[1]) &&
              nearHalf(centreTexel[2], kInk[2]) && centreTexel[3] == 1.0f,
          "aliased dab: at alpha 1 the premultiplied texel IS the straight ink -- the shared "
          "composite, so the pencil and the brush cannot drift on colour");
  }

  // ======================================================================
  // 3. The aliased MARK -- the assertion the hardness slider cannot pass
  // ======================================================================
  //
  // **This is the section that answers the design question.** Both strokes run
  // at `hardness = 1`, so both engines' *dabs* are already two-valued and
  // section 2's difference is switched off entirely. What is left is flow:
  // `RgbDeposit` accumulates `flow * coverage` per dab toward the ceiling, and
  // a texel on the stroke's rim is covered by fewer dabs than one on its spine
  // -- so the brush's mark has graded shoulders however hard its tip is. The
  // pencil reads no flow at all (brush/PencilDeposit §2), so one dab takes a
  // covered texel to the ceiling and the count of overlapping dabs cannot
  // change the answer.
  //
  // Measured as "how many distinct non-zero alphas does the mark contain",
  // which is exactly the question and needs no tolerance.
  {
    auto line = [&](bool asPencil) {
      OpenDocument od = makeRgbDoc(256, 128);
      TileStore& store = *od.document.layers[0].rgbTiles;
      BrushTip t;
      t.radius = 6.0f;
      t.hardness = 1.0f;  // a HARD tip for both -- section 2's difference, disabled
      t.flow = 0.25f;     // the whole of section 3: a rate the pencil must not have
      t.spacing = 0.25f;
      // 40 dabs along a straight horizontal line, generated here rather than
      // through StrokePath so both engines see the identical positions.
      std::vector<Vec2> dabs;
      for (int i = 0; i < 40; ++i)
        dabs.push_back(Vec2{40.0f + 1.5f * static_cast<float>(i), 64.5f});
      if (asPencil) {
        PencilStroke p;
        p.begin(kInk, 0.6f);
        p.drawDabs(store, t, dabs, 256, 128, nullptr);
      } else {
        RgbStroke b;
        b.begin(kInk, 0.6f);
        b.depositDabs(store, t, dabs, 256, 128, nullptr);
      }
      return od;
    };

    auto distinctAlphas = [&](const TileStore& store) {
      std::vector<float> vals;
      for (int32_t y = 55; y <= 74; ++y)
        for (int32_t x = 30; x <= 110; ++x) {
          const float a = readAt(store, x, y)[3];
          if (a != 0.0f) vals.push_back(a);
        }
      std::sort(vals.begin(), vals.end());
      vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
      return vals;
    };

    OpenDocument pencilDoc = line(true);
    OpenDocument brushDoc = line(false);
    const std::vector<float> pv = distinctAlphas(*pencilDoc.document.layers[0].rgbTiles);
    const std::vector<float> bv = distinctAlphas(*brushDoc.document.layers[0].rgbTiles);
    std::printf("  [measured] hard tip, flow 0.25, opacity 0.6, 40 dabs: pencil leaves %zu "
                "distinct non-zero alpha(s) (%.4f), hard BRUSH leaves %zu (%.4f .. %.4f)\n",
                pv.size(), pv.empty() ? 0.0f : pv.front(), bv.size(),
                bv.empty() ? 0.0f : bv.front(), bv.empty() ? 0.0f : bv.back());
    check(pv.size() == 1 && nearHalf(pv.front(), 0.6f),
          "aliased mark: a pencil STROKE holds exactly ONE non-zero alpha, the opacity -- one "
          "dab is the whole mark, so how many dabs overlap a texel cannot change it");
    check(bv.size() > 1,
          "aliased mark: the same stroke with a HARDNESS-1 brush holds many, because flow "
          "grades the rim -- which is why 'RgbDeposit with hardness 1' is not a pencil, and "
          "the reason this module exists rather than a slider preset");
    // The brush's spine DOES reach the ceiling in 40 dabs; what it leaves
    // behind is the rim, and the spread between the two is the artefact this
    // module exists to remove. Asserted as a spread rather than as "it never
    // arrives", so it stays true at whatever dab count and flow a later reader
    // retunes these numbers to.
    check(!bv.empty() && bv.back() - bv.front() > 0.2f,
          "aliased mark: and the hard brush's alphas SPREAD by more than a third of its own "
          "ceiling between rim and spine, so the difference above is the flow rate grading "
          "the mark and not a rounding");
  }

  // ======================================================================
  // 4. One dab is the whole mark: the ceiling reached instantly, and held
  // ======================================================================
  //
  // The accumulator is what stops a scrubbed pencil climbing toward opaque
  // (brush/PencilDeposit §2's second bullet: without it, dab two would
  // source-over `opacity` onto a texel already holding `opacity` and OPACITY
  // would become a flow slider under another name). Reaching the ceiling in
  // one dab is *why* that matters, not a reason to drop it.
  {
    OpenDocument od = makeRgbDoc(128, 128);
    TileStore& store = *od.document.layers[0].rgbTiles;
    BrushTip t;
    t.radius = 5.0f;
    t.hardness = 1.0f;
    t.flow = 0.1f;  // deliberately tiny: the pencil must not read it at all
    const Vec2 centre{64.5f, 64.5f};

    PencilStroke p;
    p.begin(kInk, 0.5f);
    const DepositCount first = p.drawDab(store, t, centre, 128, 128, nullptr, nullptr);
    const float afterOne = readAt(store, 64, 64)[3];
    const float accumOne = p.strokeAlphaAt(PixelCoord{64, 64});

    check(afterOne == 0.5f && accumOne == 0.5f,
          "one dab: a single dab at flow 0.1 lands the texel on the full 0.5 ceiling, layer "
          "and accumulator alike -- flow is not read, so a low one cannot slow the mark");

    // Now scrub. Fifty more dabs at the identical position must write nothing:
    // no texel, no tile, no dirty report.
    size_t extraTexels = 0;
    size_t extraTiles = 0;
    std::vector<TileCoord> touched;
    for (int i = 0; i < 50; ++i) {
      const DepositCount c = p.drawDab(store, t, centre, 128, 128, nullptr, &touched);
      extraTexels += c.texels;
      extraTiles += c.tiles;
    }
    check(extraTexels == 0 && extraTiles == 0 && touched.empty(),
          "one dab: fifty more dabs over the same spot write not one texel and report not one "
          "tile -- so a scrubbed pencil stops dirtying tiles and live feedback stops "
          "re-uploading a mark that is not changing");
    check(readAt(store, 64, 64)[3] == 0.5f && p.strokeAlphaAt(PixelCoord{64, 64}) == 0.5f,
          "one dab: and the value is unmoved after 51 dabs -- without the accumulator it "
          "would have climbed toward opaque and OPACITY would be a flow slider by another "
          "name");
    std::printf("  [measured] first dab wrote %zu texels across %zu tiles; the next 50 wrote "
                "%zu / %zu; accumulator holds %zu tile(s), %zu bytes\n",
                first.texels, first.tiles, extraTexels, extraTiles, p.accumulatorTiles(),
                p.accumulatorBytes());
    check(p.accumulatorTiles() >= 1 && p.accumulatorBytes() >= p.accumulatorTiles() * 64u * 1024u,
          "one dab: the accumulator is really allocated while the stroke is live -- the "
          "assertions above would also pass against a stroke that never wrote anywhere");
    p.end();
    check(p.accumulatorTiles() == 0 && p.accumulatorBytes() == 0 && !p.active(),
          "one dab: and pen-up frees it, so an idle application is not holding 64 KiB per "
          "tile the pencil crossed");
  }

  // ======================================================================
  // 5. A STROKE is the union of its dabs, at one value, in any order
  // ======================================================================
  //
  // Section 4 is why: each covered texel is decided by the first dab that
  // reaches it and refused by every later one.
  //
  // **The order half is exact-arithmetic trivial and binary16-nontrivial, and
  // the second is where it earns its keep.** `1 - prod(1 - w_i)` is symmetric
  // in the dabs, so on paper `brush/RgbDeposit` is order-independent too -- but
  // the LAYER rounds to binary16 once per *writing* dab, so a route with a
  // per-dab rate accumulates a different rounding path forwards and backwards
  // and loses bit-identity. Sabotage-proven exactly there: dropping the
  // threshold and restoring a `cov * sel` weight (the "pencil is RgbDeposit at
  // flow 1" implementation) reddens this line. The union assertion beside it is
  // the stronger of the two and is what a shifted contour breaks.
  {
    BrushTip t;
    t.radius = 7.0f;
    t.hardness = 0.35f;
    t.flow = 0.3f;
    std::vector<Vec2> forward;
    for (int i = 0; i < 24; ++i)
      forward.push_back(Vec2{30.0f + 2.0f * static_cast<float>(i), 60.5f + (i % 3)});
    std::vector<Vec2> backward(forward.rbegin(), forward.rend());

    auto paint = [&](const std::vector<Vec2>& dabs) {
      OpenDocument od = makeRgbDoc(128, 128);
      PencilStroke p;
      p.begin(kInk, 0.75f);
      p.drawDabs(*od.document.layers[0].rgbTiles, t, dabs, 128, 128, nullptr);
      return od;
    };
    OpenDocument a = paint(forward);
    OpenDocument b = paint(backward);
    bool identical = true;
    // The union, computed here from `dabCoverage()` and the threshold alone --
    // no accumulator, no composite, no knowledge of what the loop did.
    bool unionMatches = true;
    int covered = 0;
    for (int32_t y = 45; y <= 80; ++y) {
      for (int32_t x = 15; x <= 100; ++x) {
        const std::array<float, 4> pa = readAt(*a.document.layers[0].rgbTiles, x, y);
        if (pa != readAt(*b.document.layers[0].rgbTiles, x, y)) identical = false;
        bool want = false;
        for (const Vec2& d : forward) {
          const float dx = (static_cast<float>(x) + 0.5f) - d.x;
          const float dy = (static_cast<float>(y) + 0.5f) - d.y;
          if (dabCoverage(t, dx, dy) >= kPencilCoverageThreshold) want = true;
        }
        if (want) ++covered;
        if (want != (pa[3] == 0.75f)) unionMatches = false;
      }
    }
    std::printf("  [measured] 24 dabs, flow 0.3: %d texels in the computed union, all at the "
                "0.75 ceiling; forwards == backwards: %s\n", covered, identical ? "yes" : "no");
    check(unionMatches && covered > 0,
          "union: the mark is EXACTLY the union of the dabs' thresholded discs, every texel of "
          "it at the ceiling -- computed from dabCoverage() and the threshold alone, so a flow "
          "leak, a shifted contour or a lost accumulator all break it");
    check(identical,
          "union: and the same dabs stamped backwards leave BIT-IDENTICAL tiles -- exact only "
          "because no dab of this route rounds twice; a per-dab rate takes a different "
          "binary16 path forwards and backwards and loses this");
  }

  // ======================================================================
  // 6. The selection (PRD E1, P0) -- and the one place a fraction survives
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // The fractional left edge is the point: texel column 64 is 0.75 covered.
    Selection sel = selectRectangle(64.25f, 0.0f, 200.0f, 256.0f);
    BrushTip t;
    t.radius = 24.0f;
    t.hardness = 1.0f;
    t.flow = 1.0f;

    PencilStroke p;
    p.begin(kInk, 1.0f);
    // Scrubbed: twelve dabs over the boundary, so a selection that were only a
    // rate limit rather than a bound would have walked straight through it.
    for (int i = 0; i < 12; ++i)
      p.drawDab(store, t, Vec2{64.5f, 60.5f + 4.0f * static_cast<float>(i)}, 256, 256, &sel,
                nullptr);

    // (50, 80) is INSIDE the dab's disc -- 14 px from its centre column, well
    // under the 24 px radius -- and outside the ants. Picking a texel the dab
    // could not have reached anyway would make this assertion vacuous, which
    // is exactly what the first draft of it did.
    check(readAt(store, 50, 80) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "selection: not one texel inside the disc but outside the ants was touched, after "
          "twelve overlapping dabs -- the bound is a cap and not a rate, which is the mistake "
          "that passes every single-dab test");
    check(readAt(store, 80, 80)[3] == 1.0f,
          "selection: and well inside the ants the mark is at the full ceiling");
    // The stated exception (brush/PencilDeposit §3): the selection is NOT
    // thresholded, so a feathered column comes out at `opacity * sel`.
    //
    // **The expected value is read back out of the Selection rather than
    // written as 0.75**, because a `SelectionTile` stores uint8 coverage --
    // 0.75 quantises to 191/255 = 0.7490, and a test that asserted the
    // requested fraction would be asserting the selection is not quantised
    // rather than that the pencil passed it through.
    const float wantSel = selectionCoverageAt(&sel, PixelCoord{64, 80});
    const float feathered = readAt(store, 64, 80)[3];
    std::printf("  [measured] feathered selection column x=64: coverage %.4f, pencil alpha "
                "%.4f\n", wantSel, feathered);
    check(wantSel > 0.0f && wantSel < 1.0f && nearHalf(feathered, wantSel),
          "selection: the FEATHERED column comes out at opacity * coverage, NOT thresholded "
          "-- the one fraction this module lets through, because the threshold is about the "
          "mark's shape and a selection is a mask on where it may land (PRD E2)");

    // The engaged-but-absent-tile case, through this module's own hoisted loop
    // -- core/SelectionMask.hpp requires every such loop to own its own copy
    // of the two nulls and warns that a perturbation inverting one leaves the
    // others right.
    OpenDocument od2 = makeRgbDoc(256, 256);
    Selection elsewhere = selectRectangle(200.0f, 200.0f, 240.0f, 240.0f);
    PencilStroke p2;
    p2.begin(kInk, 1.0f);
    const DepositCount none =
        p2.drawDab(*od2.document.layers[0].rgbTiles, t, Vec2{60.0f, 60.0f}, 256, 256, &elsewhere,
                   nullptr);
    check(none.texels == 0 && none.tiles == 0 &&
              od2.document.layers[0].rgbTiles->occupiedTileCount() == 0,
          "selection: an engaged selection naming no tile here selects NOTHING here -- the "
          "tile is skipped whole, before anything is allocated, which is the inverse of a "
          "mask's absent tile");

    // And the null branch, which must mean "no restriction" and not "nothing".
    OpenDocument od3 = makeRgbDoc(256, 256);
    PencilStroke p3;
    p3.begin(kInk, 1.0f);
    const DepositCount all = p3.drawDab(*od3.document.layers[0].rgbTiles, t, Vec2{60.0f, 60.0f},
                                        256, 256, nullptr, nullptr);
    check(all.texels > 0 && readAt(*od3.document.layers[0].rgbTiles, 60, 60)[3] == 1.0f,
          "selection: a NULL Selection is no restriction and 1.0 everywhere -- this loop's own "
          "copy of the branch, driven separately from the engaged one above");
  }

  // ======================================================================
  // 7. The routing table's Pencil rows, including the two refusals
  // ======================================================================
  {
    Layer rgbLayer = makeRgbLayer("r");
    Layer pigment = makePigmentLayer("p");
    Layer lockedRgb = makeRgbLayer("lr");
    lockedRgb.locked = true;
    Layer alphaLockedRgb = makeRgbLayer("ar");
    alphaLockedRgb.alphaLocked = true;
    Layer hiddenRgb = makeRgbLayer("hr");
    hiddenRgb.visible = false;
    Layer adjustment = makeRgbLayer("adj");
    adjustment.kind = LayerKind::Adjustment;
    adjustment.rgbTiles.reset();
    Layer storeless = makeRgbLayer("sl");
    storeless.rgbTiles.reset();

    check(strokeRouteFor(Tool::Pencil, &rgbLayer) == StrokeRoute::PencilDeposit,
          "routing: the pencil on a writable RGB layer reaches brush/PencilDeposit -- it used "
          "to sit in the not-built list, so a drag with it reached no layer at all");
    check(strokeRouteFor(Tool::Pencil, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer REFUSES the pencil while accepting the brush -- the "
          "tempting CpuDeposit row would draw soft-edged marks and be a pencil in name only, "
          "because that route has no per-stroke ceiling to be 'one dab is the mark' about");
    check(strokeRouteFor(Tool::Pencil, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: NO layer is None for the pencil and PaintSim for the brush -- the solver's "
          "whole output is diffusion and wet edges, so a pencil sent there would draw the "
          "softest mark in the build");
    check(strokeRouteFor(Tool::Pencil, &lockedRgb) == StrokeRoute::None,
          "routing: a locked layer refuses the pencil too, from the shared body and before "
          "the kind is looked at");
    check(strokeRouteFor(Tool::Pencil, &alphaLockedRgb) == StrokeRoute::PencilDeposit &&
              strokeRouteFor(Tool::Eraser, &alphaLockedRgb) == StrokeRoute::None,
          "routing: an ALPHA-locked layer still takes the pencil though it refuses the eraser "
          "-- a pencil is a deposit and reuses the colour-only composite, which is the "
          "feature rather than the refusal");
    check(strokeRouteFor(Tool::Pencil, &hiddenRgb) == StrokeRoute::PencilDeposit,
          "routing: a HIDDEN layer still draws -- visibility is a view decision, matching "
          "every other route");
    check(strokeRouteFor(Tool::Pencil, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Pencil, &storeless) == StrokeRoute::None,
          "routing: an Adjustment layer and an RGB layer whose store was never allocated both "
          "refuse rather than falling through to the solver");
    check(std::string(strokeRouteName(StrokeRoute::PencilDeposit)) == "pencil-deposit" &&
              strokeRouteWritesLayer(StrokeRoute::PencilDeposit) &&
              grainReachesRoute(StrokeRoute::PencilDeposit),
          "routing: the new route names itself, counts as writing a layer (so the options "
          "bar accents it), and is one PAPER GRAIN reaches -- the question that predicate's "
          "own comment says a fifth route would have to answer");
    check(std::string(strokeEditLabel(Tool::Pencil)) == "pencil stroke",
          "routing: the History panel says 'pencil stroke', not 'brush stroke' -- a column "
          "that called both of them the same thing is a column that cannot be read");
    check(toolBeginsStroke(Tool::Pencil) && toolImplemented(Tool::Pencil) &&
              toolHasCanvasHandler(Tool::Pencil),
          "routing: the palette cell is live, the canvas has a handler, and the two agree -- "
          "the completeness check the eyedropper's two silent phases needed");
  }

  // ======================================================================
  // 8. End to end through StrokeSession: one undo step, and the refusal
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    const size_t entries = od.history.entries().size();
    const uint64_t revBefore = od.revision;
    const uint64_t structBefore = od.structuralRevision;

    BrushTip t;
    t.radius = 8.0f;
    t.hardness = 0.35f;
    t.flow = 0.2f;
    t.spacing = 0.2f;
    // 0.75 rather than 0.8 deliberately: the assertion below classifies texels
    // by EXACT equality with the ceiling, and 0.8 has no binary16
    // representation -- every texel would read back as 0.7998 and count as
    // "partial" for a reason that has nothing to do with the pencil.
    t.opacity = 0.75f;
    t.linearRgb = kInk;

    StrokeSession s;
    std::string err;
    check(s.begin(od, 0, t, Tool::Pencil, &err) && err.empty() &&
              s.route() == StrokeRoute::PencilDeposit,
          "session: a pencil stroke begins on the RGB layer and latches the pencil route");
    for (int i = 0; i <= 40; ++i)
      s.addPoint(40.0f + 4.0f * static_cast<float>(i), 128.0f);
    check(od.history.entries().size() == entries,
          "session: not one history entry has appeared mid-stroke, however many dabs it is");
    s.end();
    std::printf("  [measured] a %zu-dab pencil stroke wrote %zu texels; history back() = "
                "\"%s\"\n", s.dabCount(), s.texelsWritten(),
                od.history.entries().back().label.c_str());
    check(s.texelsWritten() > 0 && od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "pencil stroke",
          "session: the whole stroke is EXACTLY ONE history entry, labelled for the tool that "
          "made it");
    check(od.revision > revBefore && od.structuralRevision == structBefore,
          "session: it moved the content revision and not the structural one, so a pencil "
          "stroke costs a journal write on the interval rather than immediately");
    // And the mark the session left is still two-valued, which is the claim
    // sections 2 and 3 make about the module surviving the real dab stream --
    // StrokePath's spacing, the per-dab tip rebuild and all.
    const auto counts = countAlphas(*od.document.layers[0].rgbTiles, 30, 118, 210, 138, 0.75f);
    std::printf("  [measured] through the session: %d at the 0.75 ceiling, %d partial\n",
                counts.atCeiling, counts.partial);
    check(counts.atCeiling > 0 && counts.partial == 0,
          "session: the mark a REAL stroke leaves is still two-valued end to end -- the "
          "module's claim surviving StrokePath's spacing and the per-frame tip rebuild");

    // The refusal, by name, with nothing written.
    OpenDocument pig = makeBlankOpenDocument(128, 128, WorkingSpace{}, "pencil refusal");
    recordLayerEdit(pig, addLayer(pig.document, pig.document.layers.size(),
                                  makePigmentLayer("Pigment")));
    const size_t pigEntries = pig.history.entries().size();
    StrokeSession s2;
    std::string why;
    const bool began = s2.begin(pig, pig.document.layers.size() - 1, t, Tool::Pencil, &why);
    std::printf("  [measured] pencil on a Pigment layer: \"%s\"\n", why.c_str());
    check(!began && !why.empty() && contains(why, "pencil stroke") && contains(why, "none"),
          "session: a Pigment layer refuses the pencil with a sentence that names the edit "
          "and the route it went to -- refused out loud rather than silently doing nothing");
    check(pig.history.entries().size() == pigEntries,
          "session: and the refusal recorded no history entry -- an undo step that undoes "
          "nothing is worse than a missing one");
  }

  // ======================================================================
  // 9. Alpha lock: the colour moves, the alpha does not
  // ======================================================================
  //
  // §4's reuse of `depositRgbTexel()` in its one visible form. Nothing here is
  // the pencil's own arithmetic -- which is the point: a copied composite
  // would be where this quietly stopped being true.
  {
    OpenDocument od = makeRgbDoc(128, 128);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // A half-transparent grey rectangle, premultiplied. It stops at y == 65
    // deliberately, so that the dab below straddles its edge: the bottom half
    // of the disc lands on texels with no alpha at all, which is what makes
    // the third assertion of this section a real one rather than a reading of
    // a texel the dab could never have reached.
    for (int32_t y = 50; y <= 65; ++y)
      for (int32_t x = 50; x <= 80; ++x)
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.25f, 0.25f, 0.25f, 0.5f});

    BrushTip t;
    t.radius = 6.0f;
    t.hardness = 1.0f;
    t.flow = 1.0f;
    PencilStroke p;
    p.begin(kInk, 1.0f, /*alphaLocked=*/true);
    p.drawDab(store, t, Vec2{65.5f, 65.5f}, 128, 128, nullptr, nullptr);

    const std::array<float, 4> inside = readAt(store, 65, 62);
    check(inside[3] == 0.5f,
          "alpha lock: the alpha is BIT-IDENTICAL after a pencil dab -- the lock freezes "
          "exactly the quantity a deposit would otherwise add to");
    check(nearHalf(inside[0], kInk[0] * 0.5f) && nearHalf(inside[1], kInk[1] * 0.5f) &&
              nearHalf(inside[2], kInk[2] * 0.5f),
          "alpha lock: and the colour is the ink at the texel's own existing alpha, which is "
          "brush/RgbDeposit §4.5's composite reached through no code of this module's own");

    // (65, 69) is INSIDE the dab's disc and below the rectangle's edge, so it
    // has no alpha to paint inside -- and an alpha-locked deposit must leave it
    // at exactly transparent black rather than at the ink with zero alpha,
    // which is the malformed texel core/Composite reads as an additive glow.
    check(readAt(store, 65, 69) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "alpha lock: and a texel the dab covers with no alpha to paint inside stays exactly "
          "transparent black -- not the ink at zero alpha, which is malformed");
  }

  std::printf("[selftest] pencil deposit %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
