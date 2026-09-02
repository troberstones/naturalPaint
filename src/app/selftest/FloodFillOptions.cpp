#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>
#include <set>

#include "app/AppState.hpp"
#include "color/Space.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "ops/FloodFill.hpp"

namespace np {
namespace {

// A straight linear colour written the way a real writer writes it --
// premultiplied on the way in, the same helper `app/selftest/FloodFill.cpp`
// opens with and for the same reason: a fixture that stored straight values
// would make every claim below about a store nobody produces.
void putStraight(TileStore& tiles, int32_t x, int32_t y, const std::array<float, 4>& straight) {
  const float a = straight[3];
  tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
      .writePixel(tileLocalOffset(PixelCoord{x, y}),
                  {straight[0] * a, straight[1] * a, straight[2] * a, a});
}

// The set of texels a selection actually takes, as coordinates rather than as
// a count.
//
// A count is the tempting summary and it is the one that hides the failure this
// section is about: a tolerance change that selected the same NUMBER of texels
// somewhere else would pass a count assertion and be a completely different
// region. The subset claims below are made on the sets.
std::set<std::pair<int32_t, int32_t>> selectedSet(const Selection& sel, int32_t w, int32_t h) {
  std::set<std::pair<int32_t, int32_t>> out;
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x)
      if (selectionCoverageAt(&sel, PixelCoord{x, y}) > 0.0f) out.insert({x, y});
  return out;
}

// One row of greys whose DISPLAY-ENCODED distance from x = 0 rises in equal
// steps of `kFloodDefaultTolerance / 16.5`. Lifted in shape (not shared as
// code -- it is a file-static there) from `app/selftest/FloodFill.cpp`, whose
// own comment records why the divisor is 16.5 and not 16: at /16 the sixteenth
// sample lands exactly on the tolerance boundary, where half-float rounding of
// the stored value decides inclusion, and an assertion about which texels are
// taken then fails for a reason that has nothing to do with the claim.
TileStore encodedRampRow(int32_t width, int32_t height) {
  TileStore src;
  const float step = kFloodDefaultTolerance / 16.5f;
  for (int32_t x = 0; x < width; ++x) {
    const float v = srgbDecode(static_cast<float>(x) * step);
    for (int32_t y = 0; y < height; ++y) putStraight(src, x, y, {v, v, v, 1.0f});
  }
  return src;
}

}  // namespace

// The magic wand's and the paint bucket's OPTIONS -- `app/AppState.hpp`'s two
// `FloodFillParams` blocks, the tool -> block mapping, the REACH vocabulary
// table and the TOLERANCE unit conversion, plus what each control does to a
// real picture.
//
// **This section is not a second test of `ops/FloodFill`.** That engine has its
// own (`app/selftest/FloodFill.cpp`) and is deliberately untouched by this
// work; every parameter asserted here was already implemented and already
// correct. What was missing was any way for a user to reach three of them, and
// what is new is therefore the *binding*: two state blocks, one mapping, one
// row of widgets.
//
// The trap that shape of change carries is specific and this section is built
// around it: **a control bound to a field nothing reads.** A combo wired to
// `st.magicWand.reach` while the click rebuilds its own `FloodFillParams` is a
// toolbar that responds perfectly, prints nothing wrong, passes any assertion
// that only checks the field changed, and does not affect a single texel. So
// every claim below that could be made about a field is made about **pixels**
// instead: the region `floodFillSelection()` returns and the texels
// `fillThroughSelection()` writes, computed from the blocks through the same
// `floodToolParamsFor()` the two call sites use.
//
// What this section CANNOT reach, stated rather than left to be assumed: the
// widgets themselves and the two canvas call sites are ImGui and mouse code,
// out of reach of a headless suite the way the gradient tool's own UI half is.
// The row is photographed instead -- `tools/golden/run_golden.sh`'s
// `wand_options` and `bucket_options` views, one per tool, showing the opposite
// state of every control -- which is also the only artifact that can show the
// two tools reading two different blocks rather than one shared one.
//
// Headless and GPU-free: pure CPU flood fills over hand-built tile stores.
bool runFloodFillOptionsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] wand/bucket options: the two blocks, the mapping, and what each control does "
      "to real texels\n");

  // -----------------------------------------------------------------------
  // § 1. Two blocks, one mapping
  // -----------------------------------------------------------------------
  //
  // `floodToolParamsFor()` is the single answer to "which block does this
  // tool's click read", and both the options bar and both call sites go
  // through it. The failure it exists to prevent is the row editing one struct
  // while the canvas reads another, which is invisible from either side alone.
  {
    AppState st;
    check(floodToolParamsFor(st, Tool::MagicWand) == &st.magicWand &&
              floodToolParamsFor(st, Tool::PaintBucket) == &st.paintBucket,
          "options/map: the wand and the bucket map to their own blocks");

    // Every OTHER tool answers null, walked over the enum rather than spot-
    // checked. A third tool that started flooding without a block of its own
    // would otherwise reach the row through whichever pointer happened to be
    // returned -- `kToolMeta`'s own historical failure, one table over.
    bool othersNull = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (t == Tool::MagicWand || t == Tool::PaintBucket) continue;
      if (floodToolParamsFor(st, t) != nullptr) othersNull = false;
    }
    check(othersNull, "options/map: every other tool has no flood block");

    // The two blocks are genuinely two. A single shared block would satisfy
    // every assertion above (both tools would map to it and every other tool
    // would map to null) and differ only here.
    st.magicWand.tolerance = 0.75f;
    st.magicWand.reach = FloodFillReach::Global;
    st.magicWand.edgeBand = 0.0f;
    check(st.paintBucket.tolerance == kFloodDefaultTolerance &&
              st.paintBucket.edgeBand == kFloodDefaultEdgeBand &&
              st.paintBucket.reach == FloodFillReach::Contiguous,
          "options/map: editing the wand leaves the bucket untouched");
    st.paintBucket.tolerance = 0.1f;
    check(st.magicWand.tolerance == 0.75f,
          "options/map: and editing the bucket leaves the wand untouched");
  }

  // Both blocks start at the ENGINE's defaults, not at a second set of
  // numbers written out here. Holding `FloodFillParams` itself is what makes
  // that true by construction, and this is the assertion that would fail the
  // day someone replaces it with a mirror struct and retypes 32/255 slightly
  // wrong.
  {
    const AppState fresh;
    const FloodFillParams engine;
    check(fresh.magicWand.tolerance == engine.tolerance &&
              fresh.magicWand.edgeBand == engine.edgeBand &&
              fresh.magicWand.reach == engine.reach &&
              fresh.paintBucket.tolerance == engine.tolerance &&
              fresh.paintBucket.edgeBand == engine.edgeBand &&
              fresh.paintBucket.reach == engine.reach,
          "options/map: both blocks start at ops/FloodFill's own defaults");
  }

  // -----------------------------------------------------------------------
  // § 2. The REACH table covers the enum
  // -----------------------------------------------------------------------
  //
  // Walked, not counted, for `app/selftest/GradientTool.cpp` § 1's reason: a
  // `static_assert` on the row count passes on any permutation and on any
  // duplicate, which is exactly how `kToolMeta` once shipped its rows in the
  // wrong order.
  {
    const FloodFillReach kAll[] = {FloodFillReach::Contiguous, FloodFillReach::Global};
    static_assert(sizeof(kAll) / sizeof(kAll[0]) == kFloodReachCount,
                  "this list must name every FloodFillReach");
    bool everyValueOnce = true;
    for (FloodFillReach want : kAll) {
      size_t seen = 0;
      for (size_t i = 0; i < kFloodReachCount; ++i)
        if (kFloodReaches[i].reach == want) ++seen;
      if (seen != 1) everyValueOnce = false;
    }
    check(everyValueOnce, "options/reach: every FloodFillReach appears exactly once");

    bool labelsAgree = true;
    for (size_t i = 0; i < kFloodReachCount; ++i)
      if (std::strcmp(floodReachLabel(kFloodReaches[i].reach), kFloodReaches[i].label) != 0)
        labelsAgree = false;
    check(labelsAgree, "options/reach: floodReachLabel() returns the table's own label");

    // Distinct and non-empty. Two rows sharing a label is a combo in which one
    // of two modes is unreachable and the user cannot tell which one they
    // picked -- green under every assertion above.
    bool distinct = true;
    for (size_t i = 0; i < kFloodReachCount; ++i) {
      if (kFloodReaches[i].label == nullptr || kFloodReaches[i].label[0] == '\0') distinct = false;
      if (kFloodReaches[i].tip == nullptr || kFloodReaches[i].tip[0] == '\0') distinct = false;
      for (size_t j = i + 1; j < kFloodReachCount; ++j)
        if (std::strcmp(kFloodReaches[i].label, kFloodReaches[j].label) == 0) distinct = false;
    }
    check(distinct, "options/reach: labels and tips non-empty, labels distinct");
  }

  // -----------------------------------------------------------------------
  // § 3. TOLERANCE's units: 0..255 on screen, 0..1 in the engine
  // -----------------------------------------------------------------------
  //
  // The whole point of `ops/FloodFill.hpp` § 1's display-encoded metric is
  // that 32 means what a user arriving from Photoshop believes it means, and
  // the last step of delivering that is showing 32 rather than 0.125. A
  // conversion at the widget is the cheapest way to do it and the easiest to
  // get subtly wrong: a slider that cannot reproduce its own default, or that
  // walks a value one step every time the tool is selected.
  {
    bool roundTrips = true;
    for (int ui = 0; ui <= kFloodToleranceUiMax; ++ui)
      if (floodToleranceToUi(floodToleranceFromUi(ui)) != ui) roundTrips = false;
    check(roundTrips, "options/tolerance: all 256 slider steps round-trip exactly");

    check(floodToleranceToUi(kFloodDefaultTolerance) == 32,
          "options/tolerance: the shipped default reads as Photoshop's 32");
    check(floodToleranceFromUi(32) == kFloodDefaultTolerance,
          "options/tolerance: and 32 converts back to that exact float");

    // **An OFF-GRID tolerance rounds to nearest, and this assertion exists
    // because a sabotage found it missing.** Replacing the `+ 0.5f` with a
    // plain truncation changed nothing at all in the three claims above -- on
    // the 256-value grid `(int)(n / 255.0f * 255.0f)` is exactly `n` for every
    // n, so a round-trip test provably cannot tell rounding from truncation.
    // The rounding is load-bearing only for a value that did not come from this
    // slider, which is every value the demo flag, a future preferences file or
    // a script could set: at tolerance 0.5 the two spellings read 128 and 127,
    // and 127 would then be written back as a DIFFERENT tolerance the moment
    // the slider was touched. So the off-grid case is what pins it.
    check(floodToleranceToUi(0.5f) == 128 && floodToleranceToUi(1.0f / 255.0f * 0.4f) == 0,
          "options/tolerance: an off-grid tolerance rounds to nearest");

    // Out-of-range inputs clamp rather than wrap. The engine's own comment
    // says values above 1 accept most of the picture, so a field left at 4.0
    // by some future writer must show a pinned 255, not a wrapped 4.
    check(floodToleranceToUi(-1.0f) == 0 && floodToleranceToUi(4.0f) == kFloodToleranceUiMax &&
              floodToleranceFromUi(-5) == 0.0f && floodToleranceFromUi(9999) == 1.0f,
          "options/tolerance: conversions clamp at both ends");
  }

  // -----------------------------------------------------------------------
  // § 4. TOLERANCE changes WHICH texels are selected
  // -----------------------------------------------------------------------
  //
  // Rendered, not asserted on the field. The claim is a subset relation on the
  // texel sets, not a pair of counts: a control that moved the region sideways
  // would satisfy "more texels selected" and be a different tool.
  {
    constexpr int32_t W = 40, H = 4;
    const TileStore src = encodedRampRow(W, H);
    AppState st;

    st.magicWand.tolerance = floodToleranceFromUi(16);
    const Selection tight = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                               *floodToolParamsFor(st, Tool::MagicWand));
    st.magicWand.tolerance = floodToleranceFromUi(64);
    const Selection loose = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                               *floodToolParamsFor(st, Tool::MagicWand));

    const auto tightSet = selectedSet(tight, W, H);
    const auto looseSet = selectedSet(loose, W, H);
    check(!tightSet.empty() && tightSet.size() < looseSet.size(),
          "options/tolerance: a looser tolerance takes strictly more texels");
    bool subset = true;
    for (const auto& p : tightSet)
      if (looseSet.count(p) == 0) subset = false;
    check(subset, "options/tolerance: and the tight region is a subset of the loose one");

    // Zero tolerance takes only what matches the seed exactly. The ramp's
    // every column is a distinct grey, so that is the seed column alone --
    // the negative end of the control, which a slider whose minimum silently
    // clamped to the default would fail.
    st.magicWand.tolerance = floodToleranceFromUi(0);
    const Selection exact = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                               *floodToolParamsFor(st, Tool::MagicWand));
    check(selectedSet(exact, W, H).size() == static_cast<size_t>(H),
          "options/tolerance: 0 takes the seed's own column and nothing else");
  }

  // -----------------------------------------------------------------------
  // § 5. ANTI-ALIAS: the checkbox's two values, on real coverage
  // -----------------------------------------------------------------------
  //
  // The two values the checkbox writes are 0 and `kFloodDefaultEdgeBand`, and
  // the three things that must hold across them are exactly the three
  // `ops/FloodFill.hpp` § 2 promises and the tooltip repeats: the seed stays at
  // full coverage, the REACHED SET does not move, and the boundary's coverage
  // does. Asserted here rather than trusted from that header because this is
  // the first control that lets a user change the value at all -- before this
  // row, `edgeBand` was the default in every call in the build.
  {
    constexpr int32_t W = 40, H = 4;
    const TileStore src = encodedRampRow(W, H);
    AppState st;

    st.magicWand.edgeBand = kFloodDefaultEdgeBand;  // the box ticked
    const Selection soft = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                              *floodToolParamsFor(st, Tool::MagicWand));
    st.magicWand.edgeBand = 0.0f;  // the box unticked
    const Selection hard = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                              *floodToolParamsFor(st, Tool::MagicWand));

    check(selectionCoverageAt(&soft, PixelCoord{0, 0}) == 1.0f &&
              selectionCoverageAt(&hard, PixelCoord{0, 0}) == 1.0f,
          "options/anti-alias: the clicked texel is fully selected either way");

    check(selectedSet(soft, W, H) == selectedSet(hard, W, H),
          "options/anti-alias: it weights the boundary and never moves it");

    // The coverage itself differs, and in the direction that makes the setting
    // worth having: unticked, every texel is 0 or 1; ticked, at least one is
    // strictly between. Both halves are needed -- "some texel differs" alone
    // would pass on a hard edge that had simply moved.
    bool hardIsBinary = true, softHasPartial = false;
    for (int32_t y = 0; y < H; ++y) {
      for (int32_t x = 0; x < W; ++x) {
        const float h = selectionCoverageAt(&hard, PixelCoord{x, y});
        const float s = selectionCoverageAt(&soft, PixelCoord{x, y});
        if (h != 0.0f && h != 1.0f) hardIsBinary = false;
        if (s > 0.0f && s < 1.0f) softHasPartial = true;
      }
    }
    check(hardIsBinary && softHasPartial,
          "options/anti-alias: unticked is in-or-out, ticked ramps the edge");
  }

  // -----------------------------------------------------------------------
  // § 6. REACH, on a picture only the two modes can tell apart
  // -----------------------------------------------------------------------
  //
  // Two blocks of the identical colour with a different-coloured gap between
  // them. Contiguous must take one; All Similar must take both. Any other
  // fixture -- a single blob, a document of one colour -- renders identically
  // under both modes and would prove nothing while looking like a test.
  {
    constexpr int32_t W = 30, H = 6;
    const std::array<float, 4> ink{0.2f, 0.4f, 0.8f, 1.0f};
    const std::array<float, 4> paper{0.95f, 0.95f, 0.95f, 1.0f};
    TileStore src;
    for (int32_t y = 0; y < H; ++y)
      for (int32_t x = 0; x < W; ++x) putStraight(src, x, y, paper);
    // Left block x 0..4, right block x 20..24 -- the gap is wide enough that no
    // tolerance in play could bridge it, since paper and ink differ by far more
    // than the default band.
    for (int32_t y = 0; y < H; ++y) {
      for (int32_t x = 0; x < 5; ++x) putStraight(src, x, y, ink);
      for (int32_t x = 20; x < 25; ++x) putStraight(src, x, y, ink);
    }

    AppState st;
    st.magicWand.reach = FloodFillReach::Contiguous;
    const Selection near = floodFillSelection(src, PixelCoord{2, 2}, W, H,
                                              *floodToolParamsFor(st, Tool::MagicWand));
    st.magicWand.reach = FloodFillReach::Global;
    const Selection all = floodFillSelection(src, PixelCoord{2, 2}, W, H,
                                             *floodToolParamsFor(st, Tool::MagicWand));

    check(selectedSet(near, W, H).size() == 5u * H,
          "options/reach: Contiguous takes only the block clicked");
    check(selectedSet(all, W, H).size() == 10u * H,
          "options/reach: All Similar takes the disconnected block too");
    check(selectionCoverageAt(&near, PixelCoord{22, 2}) == 0.0f &&
              selectionCoverageAt(&all, PixelCoord{22, 2}) == 1.0f,
          "options/reach: and the far block is the texel they disagree on");
  }

  // -----------------------------------------------------------------------
  // § 7. The bucket fills exactly what the wand would have selected -- under
  //      ITS OWN parameters
  // -----------------------------------------------------------------------
  //
  // `ops/FloodFill.hpp`'s opening argument is that these two tools are one
  // algorithm, and `app/selftest/FloodFill.cpp` already asserts that filling
  // through the wand's selection changes exactly the texels the wand selected.
  // What is new here is that each tool brings its own parameters to that one
  // algorithm, so the property to pin is the pair: the bucket's write matches
  // the region ITS block produces, and that region is not the wand's.
  //
  // The two blocks are set the way `--wand-demo` sets them for the golden
  // views, so the photograph and this assertion describe the same two states.
  {
    constexpr int32_t W = 40, H = 4;
    TileStore src = encodedRampRow(W, H);
    AppState st;
    st.magicWand.tolerance = floodToleranceFromUi(32);
    st.magicWand.reach = FloodFillReach::Contiguous;
    st.paintBucket.tolerance = floodToleranceFromUi(96);
    st.paintBucket.edgeBand = 0.0f;
    st.paintBucket.reach = FloodFillReach::Global;

    const Selection wandRegion = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                                    *floodToolParamsFor(st, Tool::MagicWand));
    const Selection bucketRegion = floodFillSelection(src, PixelCoord{0, 0}, W, H,
                                                      *floodToolParamsFor(st, Tool::PaintBucket));
    const auto wandSet = selectedSet(wandRegion, W, H);
    const auto bucketSet = selectedSet(bucketRegion, W, H);
    check(wandSet != bucketSet && wandSet.size() < bucketSet.size(),
          "options/two blocks: the same click gives the two tools two regions");

    // The bucket's own write, through its own region. `fillThroughSelection()`
    // returns the number of texels whose STORED value changed, and with the
    // anti-alias box unticked every selected texel is fully covered, so the
    // two numbers are comparable exactly. A red fill over a grey ramp changes
    // every texel it touches.
    const size_t changed =
        fillThroughSelection(src, bucketRegion, {0.9f, 0.05f, 0.05f, 1.0f});
    check(changed == bucketSet.size(),
          "options/two blocks: the bucket writes exactly its own region");

    // And it wrote MORE than the wand's block would have -- which is the claim
    // that fails if the bucket's call site ever goes back to building its own
    // `FloodFillParams` instead of reading `floodToolParamsFor()`.
    check(changed > wandSet.size(),
          "options/two blocks: under the bucket's parameters, not the wand's");
  }

  // -----------------------------------------------------------------------
  // § 8. Option's remaining meaning
  // -----------------------------------------------------------------------
  //
  // The wand's Option-click used to force `FloodFillReach::Global` as well as
  // Subtract, and this revision removed the reach half so the visible REACH
  // combo is the only source of truth for it. That removal rests on one fact --
  // that Option still means Subtract, so the modifier is un-overloaded rather
  // than deleted -- and this is that fact, restated at the point that now
  // depends on it. `app/selftest/Selection.cpp` asserts the same four-row table
  // for its own reasons; this is a second reader, not a duplicate claim.
  //
  // **What this cannot assert**, said plainly rather than left to look
  // covered: the removal itself lives in a mouse handler in
  // `ui/MacPaintUI.cpp`, and nothing headless can prove a modifier is no longer
  // read there. The two golden views are the artifact for the visible half (the
  // combo shows the reach a click will use), and the diff is the artifact for
  // the removal.
  {
    check(selectionCombineFromModifiers(false, true) == SelectionCombine::Subtract &&
              selectionCombineFromModifiers(false, false) == SelectionCombine::Replace,
          "options/modifier: Option still means Subtract, and only that");

    // A fresh block is Contiguous, so the wand's first click after a launch is
    // the connected fill whatever keys are held. Stated because the old
    // behaviour made that untrue for anyone resting a thumb on Option.
    const AppState fresh;
    check(fresh.magicWand.reach == FloodFillReach::Contiguous &&
              fresh.paintBucket.reach == FloodFillReach::Contiguous,
          "options/modifier: reach comes from the block, and starts Contiguous");
  }

  std::printf("[selftest] wand/bucket options: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
