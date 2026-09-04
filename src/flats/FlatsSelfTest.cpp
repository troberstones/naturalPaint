#include "flats/FlatsSelfTest.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "flats/Expand.hpp"
#include "flats/Field.hpp"
#include "flats/Gaps.hpp"
#include "flats/Ink.hpp"
#include "flats/Membrane.hpp"
#include "flats/Model.hpp"
#include "flats/Morphology.hpp"
#include "flats/Sag.hpp"
#include "flats/Segment.hpp"
#include "flats/Tool.hpp"

namespace np {

// flats/: the invariant suite autoFlats shipped (its test/run.ts), ported
// FIRST -- docs/autoflats-migration.md §8 step 1: "these are the specification
// for everything that follows; porting them last would mean porting blind."
//
// The point of the suite is INVARIANTS, not numbers. The bug that motivated
// it in autoFlats (a GPU growth path silently returning a zero-filled label
// map) made every flat over 2MP wrong while still reporting a plausible fill
// count. So `checkSegmentation` asserts the SHAPE of the result: every pixel
// belongs to a region, core is empty on ink and non-empty elsewhere,
// something is background, ids are compact 1..K, areas tile the free space.
//
// Two additions the port needs that the original did not:
//
//   * **Bit-exactness against the reference.** The label-field hashes below
//     were produced by running autoFlats' own TypeScript on the same
//     fixtures (three boxes, hatched box, leaky box at 6 and 40 px). They
//     pin the port to the reference implementation; a change that moves them
//     is a change in the algorithm, which is allowed but must be deliberate.
//   * **Determinism across a parameter sweep** (PRD N4, flats/Model.hpp §2):
//     a fill's colour comes from its anchor, so nudging a parameter and
//     putting it back must reproduce every colour exactly, and a recorded
//     recolour must survive the sweep.
//
// This translation unit deliberately includes NOTHING from app/ or gfx/: it
// is the one section that also runs on a machine with no Metal and no SDL,
// through the `flatstest` executable (src/tools/FlatsTestMain.cpp), which is
// how it was developed. The macOS build reaches it through the usual
// `--selftest` chain in main.cpp.

namespace {

struct Check {
  bool ok = true;
  void operator()(bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  }
};

uint32_t hash32(const FlatLabels& v) {
  uint32_t h = 2166136261u;
  for (const int32_t x : v) {
    h ^= static_cast<uint32_t>(x);
    h *= 16777619u;
  }
  return h;
}

struct Flat {
  FlatLabels core, labels;
  std::vector<FlatRegionInfo> regions;
};

Flat flatBall(const FlatArt& a, int maxGap = 8, int minArea = 50) {
  Flat f;
  f.core = flatTrappedBall(a.line, a.w, a.h, maxGap, &a.ink).core;
  f.labels = flatExpandLabels(f.core, a.w, a.h, &a.ink);
  f.regions = flatFinalizeRegions(f.core, f.labels, a.w, a.h, minArea).regions;
  return f;
}

Flat flatSag(const FlatArt& a, float tau = 6, int maxGap = 8, int minArea = 50) {
  Flat f;
  f.core = flatSagSegment(a.line, a.w, a.h, tau, maxGap).core;
  f.labels = flatExpandLabels(f.core, a.w, a.h, &a.ink);
  f.regions = flatFinalizeRegions(f.core, f.labels, a.w, a.h, minArea).regions;
  return f;
}

int32_t regionAt(const FlatLabels& labels, int w, int x, int y) { return labels[static_cast<size_t>(y) * w + x]; }

void checkSegmentation(Check& check, const std::string& what, const Flat& f, const FlatMask& line, int w, int h) {
  const size_t n = static_cast<size_t>(w) * h;
  size_t zeroLabels = 0, inkWithCore = 0, freeWithoutCore = 0, freePx = 0;
  for (size_t i = 0; i < n; i++) {
    if (f.labels[i] == 0) zeroLabels++;
    if (line[i]) {
      if (f.core[i] != 0) inkWithCore++;
    } else {
      freePx++;
      if (f.core[i] == 0) freeWithoutCore++;
    }
  }
  check(zeroLabels == 0, (what + ": every pixel belongs to a region (expansion covers the ink)").c_str());
  check(inkWithCore == 0, (what + ": core is empty on ink").c_str());
  check(freeWithoutCore == 0, (what + ": every free pixel has a core label").c_str());
  check(!f.regions.empty(), (what + ": produced regions").c_str());
  bool anyBg = false;
  int maxId = 0, minId = 1 << 30;
  long areaSum = 0;
  bool allPositive = true;
  for (const FlatRegionInfo& r : f.regions) {
    anyBg = anyBg || r.isBg;
    maxId = std::max(maxId, r.id);
    minId = std::min(minId, r.id);
    areaSum += r.area;
    allPositive = allPositive && r.area > 0;
  }
  check(anyBg, (what + ": something was identified as background").c_str());
  check(minId == 1 && maxId == static_cast<int>(f.regions.size()), (what + ": region ids are compact 1..K").c_str());
  check(allPositive, (what + ": every region covers at least one pixel").c_str());
  check(areaSum == static_cast<long>(freePx), (what + ": region areas tile the free space").c_str());
}

}  // namespace

bool runFlatsTest() {
  Check check;
  std::printf("[selftest] flats: the ported autoFlats invariant suite\n");

  // --- 1. membrane: the analytic solutions -------------------------------
  {
    const int W = 200, H = 32;
    FlatMask line(static_cast<size_t>(W) * H, 0);
    for (int x = 0; x < W; x++) { line[x] = 1; line[static_cast<size_t>(H - 1) * W + x] = 1; }
    const std::vector<float> sag = flatMembraneSag(line, W, H, 60, 1e-4f);
    // discrete u_i = i(n+1-i)/2 with n=30 -> u_max = 120, sag = sqrt(8*120)
    check(std::fabs(sag[15 * W + 100] - std::sqrt(8 * 120.0)) <= 0.1, "membrane: matches the analytic strip solution");
  }
  {
    const int S = 201, R = 80;
    FlatMask line(static_cast<size_t>(S) * S, 1);
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++)
        if (std::hypot(x - 100, y - 100) < R) line[static_cast<size_t>(y) * S + x] = 0;
    const std::vector<float> sag = flatMembraneSag(line, S, S, 60, 1e-4f);
    const double expect = R * std::sqrt(2.0);
    check(std::fabs(sag[100 * S + 100] - expect) <= expect * 0.01, "membrane: matches the analytic disc solution");
  }
  {
    // The line search on the coarse correction is what keeps the multigrid
    // from diverging on real line art. autoFlats pins that with a 40 KB
    // real-image fixture; here the same property is held on the hatched box
    // (solving harder must not move the answer), and the real fixture is
    // left in autoFlats where it is runnable.
    const FlatArt a = flatHatchedBox();
    const std::vector<float> loose = flatMembraneSag(a.line, a.w, a.h, 80, 1e-2f);
    const std::vector<float> tight = flatMembraneSag(a.line, a.w, a.h, 200, 1e-5f);
    float mLoose = 0, mTight = 0;
    for (size_t i = 0; i < loose.size(); i++) { mLoose = std::max(mLoose, loose[i]); mTight = std::max(mTight, tight[i]); }
    check(mTight > 0 && std::fabs(mLoose / mTight - 1) <= 0.05, "membrane: max sag does not drift as the solve tightens");
  }

  // --- 2. segmentation invariants ------------------------------------------
  const struct { const char* name; FlatArt art; } fixtures[] = {
      {"three boxes", flatThreeBoxes()}, {"hatched box", flatHatchedBox()}, {"leaky box (4px)", flatLeakyBox(4)}};
  for (const auto& fx : fixtures) {
    checkSegmentation(check, std::string("ball/") + fx.name, flatBall(fx.art), fx.art.line, fx.art.w, fx.art.h);
    checkSegmentation(check, std::string("sag/") + fx.name, flatSag(fx.art), fx.art.line, fx.art.w, fx.art.h);
  }

  {
    const FlatArt a = flatThreeBoxes();
    const Flat f = flatBall(a);
    const std::set<int32_t> inside{regionAt(f.labels, a.w, 90, 90), regionAt(f.labels, a.w, 240, 90),
                                   regionAt(f.labels, a.w, 165, 220)};
    check(inside.size() == 3, "three closed boxes: the three interiors are three distinct fills");
    check(!inside.count(regionAt(f.labels, a.w, 5, 5)), "three closed boxes: no interior shares the background");
    check(f.regions.size() >= 4 && f.regions.size() <= 8, "three closed boxes: region count in 4..8");
  }
  {
    // The trapped-ball guarantee: a ball of radius r cannot pass a gap under 2r.
    const FlatArt narrow = flatLeakyBox(6);
    const Flat nf = flatBall(narrow, 8);
    check(regionAt(nf.labels, narrow.w, 200, 150) != regionAt(nf.labels, narrow.w, 5, 5),
          "trapped ball: a 6px gap does not leak with gap size 8");
    const FlatArt wide = flatLeakyBox(40);
    const Flat wf = flatBall(wide, 8);
    check(regionAt(wf.labels, wide.w, 200, 150) == regionAt(wf.labels, wide.w, 5, 5),
          "trapped ball: a 40px gap is wider than the ball and leaks");
  }
  {
    const FlatArt a = flatLeakyBox(6);
    const Flat f = flatSag(a);
    check(regionAt(f.labels, a.w, 200, 150) != regionAt(f.labels, a.w, 5, 5),
          "rubber sheet: holds a 6px break closed");
  }
  {
    const FlatArt a = flatHatchedBox();
    const size_t ball = flatBall(a).regions.size(), sag = flatSag(a).regions.size();
    check(sag < ball, "rubber sheet: absorbs hatching instead of seeding a fill per pocket");
    std::printf("    hatched box: ball %zu fills, sheet %zu fills\n", ball, sag);
  }

  // --- 3. bit-exact against the TypeScript reference ------------------------
  // Hashes measured by running autoFlats' own core on identical fixtures
  // (see this file's header). The ball path and the sheet path both.
  {
    struct Ref { const char* name; FlatArt art; uint32_t ball, sag; size_t ballCount, sagCount; };
    const Ref refs[] = {
        {"three boxes", flatThreeBoxes(), 2872038773u, 3689103717u, 4, 4},
        {"hatched box", flatHatchedBox(), 2482140844u, 756426508u, 28, 2},
        {"leaky box (6px)", flatLeakyBox(6), 760825091u, 2699863589u, 2, 2},
        {"leaky box (40px)", flatLeakyBox(40), 981464453u, 981464453u, 1, 1},
    };
    for (const Ref& r : refs) {
      const Flat b = flatBall(r.art), s = flatSag(r.art);
      check(hash32(b.labels) == r.ball && b.regions.size() == r.ballCount,
            (std::string("reference: trapped ball matches autoFlats on ") + r.name).c_str());
      check(hash32(s.labels) == r.sag && s.regions.size() == r.sagCount,
            (std::string("reference: rubber sheet matches autoFlats on ") + r.name).c_str());
    }
    // slivers + declutter on the hatched box: 28 -> 26 fills, hash 2203830520.
    const FlatArt a = flatHatchedBox();
    Flat b = flatBall(a);
    const bool sl = flatMergeSlivers(b.core, b.labels, a.line, a.w, a.h, 3);
    const FlatFinalizeResult r2 = flatFinalizeRegions(b.core, b.labels, a.w, a.h, 50);
    check(sl && r2.regions.size() == 26 && hash32(b.labels) == 2203830520u,
          "reference: sliver merge matches autoFlats on the hatched box");
  }

  // --- 4. the model: evaluation, determinism, replay -------------------------
  {
    const FlatArt a = flatThreeBoxes();
    FlatsContent content;
    content.params.sheet = 0;  // trapped ball, so the fixture numbers above apply
    content.params.minRegion = 50;
    content.params.sliverWidth = 0;
    content.params.declutter = 0;
    content.params.closeTightGaps = false;
    const FlatEvaluation e1 = flatEvaluateInk(a.ink, a.w, a.h, content);
    check(e1.fills.size() == 5 && e1.roots().size() == 4, "model: three boxes evaluate to four fills");
    const int box = e1.fillAt(90, 90), bg = e1.fillAt(5, 5);
    check(box && bg && box != bg && e1.fills[bg].isBg && !e1.fills[bg].visible,
          "model: the background is flagged and hidden by default");

    // Determinism (PRD N4): nudge a parameter, put it back, every colour returns.
    FlatsContent nudged = content;
    nudged.params.gapSize = 6;
    const FlatEvaluation e2 = flatEvaluateInk(a.ink, a.w, a.h, nudged);
    const FlatEvaluation e3 = flatEvaluateInk(a.ink, a.w, a.h, content);
    bool same = e1.fills.size() == e3.fills.size();
    for (size_t i = 1; same && i < e1.fills.size(); i++) same = e1.fills[i].color == e3.fills[i].color;
    check(same, "determinism: a parameter sweep and back reproduces every colour");
    // And the colours are anchor-derived: the box at (90,90) keeps its colour
    // across the sweep even though ids may have moved.
    check(e2.fills[e2.fillAt(90, 90)].color == e1.fills[box].color,
          "determinism: a fill the sweep did not change keeps its colour");
    const std::vector<std::array<int, 2>> at = e1.anchors();
    check(at[box][0] > 30 && at[box][0] < 150 && at[box][1] > 30 && at[box][1] < 150 &&
              e1.fills[box].color == flatAnchorColor(at[box][0], at[box][1]),
          "determinism: a fill's colour is its anchor's colour");

    // A recorded recolour survives the sweep; so does a delete mark.
    FlatsContent edited = content;
    edited.edits.recolors.push_back({edited.edits.nextId++, 90, 90, -1, FlatRgb{200, 40, 40}});
    edited.edits.deleteMarks.push_back({edited.edits.nextId++, 240, 90});
    const FlatEvaluation e4 = flatEvaluateInk(a.ink, a.w, a.h, edited);
    check(e4.fills[e4.fillAt(90, 90)].color == FlatRgb{200, 40, 40}, "replay: a recolour lands on the fill under its point");
    check(e4.fills[e4.fillAt(240, 90)].deleted, "replay: a delete mark deletes the fill under it");
    edited.params.gapSize = 6;
    const FlatEvaluation e5 = flatEvaluateInk(a.ink, a.w, a.h, edited);
    check(e5.fills[e5.fillAt(90, 90)].color == FlatRgb{200, 40, 40} && e5.fills[e5.fillAt(240, 90)].deleted,
          "replay: recolour and delete survive a parameter change");

    // Palette link (PRD N6): a recolour from swatch 2 follows the swatch.
    FlatsContent linked = content;
    linked.palette = {FlatRgb{1, 2, 3}, std::nullopt, FlatRgb{10, 20, 30}};
    linked.edits.recolors.push_back({linked.edits.nextId++, 90, 90, 2, FlatRgb{10, 20, 30}});
    const FlatEvaluation e6 = flatEvaluateInk(a.ink, a.w, a.h, linked);
    check(e6.fills[e6.fillAt(90, 90)].color == FlatRgb{10, 20, 30} && e6.fills[e6.fillAt(90, 90)].swatch == 2,
          "palette: a fill painted from a swatch carries the swatch");
    linked.palette[2] = FlatRgb{99, 98, 97};
    const FlatEvaluation e7 = flatEvaluateInk(a.ink, a.w, a.h, linked);
    check(e7.fills[e7.fillAt(90, 90)].color == FlatRgb{99, 98, 97}, "palette: adjusting the swatch recolours the fill");

    // Merge pair and draw-merge: two boxes become one root.
    FlatsContent merged = content;
    merged.edits.mergePairs.push_back({merged.edits.nextId++, 90, 90, 240, 90});
    const FlatEvaluation e8 = flatEvaluateInk(a.ink, a.w, a.h, merged);
    check(e8.fillAt(90, 90) == e8.fillAt(240, 90) && e8.roots().size() == 3, "replay: a merge pair joins the two fills under its points");
    FlatsContent drawn = content;
    drawn.edits.mergeStrokes.push_back({drawn.edits.nextId++, FlatPolyline{90, 90, 240, 90, 165, 220}});
    const FlatEvaluation e9 = flatEvaluateInk(a.ink, a.w, a.h, drawn);
    check(e9.fillAt(90, 90) == e9.fillAt(240, 90) && e9.fillAt(90, 90) == e9.fillAt(165, 220) && e9.roots().size() == 2,
          "replay: a draw-merge merges every fill it crosses into the first");
    // Ink between the boxes is also crossed by that stroke -- but the
    // background is never merged into a fill.
    check(e9.fillAt(5, 5) != e9.fillAt(90, 90), "replay: a draw-merge never swallows the background");

    // Shape fill: a hand-drawn region wins over the segmenter.
    FlatsContent shaped = content;
    shaped.edits.shapeFills.push_back({shaped.edits.nextId++, FlatPolyline{10, 10, 20, 10, 20, 20, 10, 20}, FlatRgb{7, 7, 7}, "Shape 1"});
    const FlatEvaluation e10 = flatEvaluateInk(a.ink, a.w, a.h, shaped);
    const int sh = e10.fillAt(15, 15);
    check(sh && sh != e10.fillAt(5, 5) && e10.fills[sh].color == FlatRgb{7, 7, 7} && e10.fills[sh].area == 100,
          "replay: a shape fill stamps a new fill over what was there");

    // Group lasso: membership from geometry, background never joins.
    FlatsContent grouped = content;
    grouped.edits.groups.push_back({grouped.edits.nextGroup++, "Boxes", FlatPolyline{20, 20, 320, 20, 320, 160, 20, 160}});
    const FlatEvaluation e11 = flatEvaluateInk(a.ink, a.w, a.h, grouped);
    check(e11.fills[e11.fillAt(90, 90)].group == 1 && e11.fills[e11.fillAt(240, 90)].group == 1 &&
              e11.fills[e11.fillAt(165, 220)].group == 0 && e11.fills[e11.fillAt(5, 5)].group == 0,
          "replay: a lasso groups the fills it covers and never the background");

    // Carve: a new fill out of the background at a click, replayable. The
    // carve takes the whole ball-fitting component the click can reach -- on
    // open paper that is all of it -- and leaves the old background fill the
    // band hugging the strokes. (Its real use is inside a figure the
    // background leaked into, where the ball stops at the break.)
    FlatsContent carved = content;
    carved.edits.carves.push_back({carved.edits.nextId++, 350, 50});
    const FlatEvaluation e12 = flatEvaluateInk(a.ink, a.w, a.h, carved);
    const int cv = e12.fillAt(350, 50);
    check(cv && cv == e12.fillAt(5, 5) && !e12.fills[cv].isBg && cv != bg && e12.fills[bg].area > 0 &&
              e12.roots().size() == 5,
          "replay: a carve cuts a new fill out of the background");

    // Rendering: a hidden background renders transparent, a fill opaque.
    std::vector<uint8_t> rgba(static_cast<size_t>(a.w) * a.h * 4);
    flatRenderRgba8(e1, rgba.data());
    const size_t pBg = (static_cast<size_t>(5) * a.w + 5) * 4, pBox = (static_cast<size_t>(90) * a.w + 90) * 4;
    check(rgba[pBg + 3] == 0 && rgba[pBox + 3] == 255 && rgba[pBox] == e1.fills[box].color[0],
          "render: hidden background is transparent, a fill is its colour");
    // The line art overlays with no fringe: the fill reaches the middle of
    // the 2 px wall at x = 30..31 -- the inner half belongs to the box, the
    // outer half to the (hidden) background, so it is (31, 90) that must be
    // opaque with the box's colour.
    const size_t pInk = (static_cast<size_t>(90) * a.w + 31) * 4;
    check(a.line[static_cast<size_t>(90) * a.w + 31] && rgba[pInk + 3] == 255 && rgba[pInk] == e1.fills[box].color[0],
          "render: fills reach under the ink to the stroke's middle");

    // Content hash: every edit and parameter moves it; the same content does not.
    check(flatsContentHash(content) == flatsContentHash(content) && flatsContentHash(content) != flatsContentHash(edited) &&
              flatsContentHash(content) != flatsContentHash(nudged),
          "hash: stable on equal content, moved by an edit or a parameter");
  }

  // --- 5. gaps: a leaky box gets a suggestion that closes it ------------------
  {
    // A radius-8 ball cannot pass a gap under 16 px, and the chamfer corridor
    // through an 18 px one pinches shut, so the break must be 20 px to leak
    // (measured: 18 holds, 20 leaks) -- exactly the 20 px the gap finder
    // reaches across at this gap size.
    const FlatArt a = flatLeakyBox(20);
    FlatsContent content;
    content.params.sheet = 0;
    content.params.minRegion = 50;
    content.params.declutter = 0;
    content.params.closeTightGaps = false;
    content.params.autoMergeLeaks = false;
    const FlatEvaluation e = flatEvaluateInk(a.ink, a.w, a.h, content);
    check(e.fillAt(200, 150) == e.fillAt(5, 5), "gaps: a 20px break leaks with gap size 8");
    const FlatSegs segs = flatSuggestGaps(a.line, a.w, a.h, 8, &e.labels);
    bool spansBreak = false;
    for (size_t i = 0; i + 3 < segs.size(); i += 4) {
      const float my = (segs[i + 1] + segs[i + 3]) / 2;
      if (std::fabs(segs[i] - 100) <= 4 && std::fabs(segs[i + 2] - 100) <= 4 && std::fabs(my - 150) <= 10) spansBreak = true;
    }
    check(spansBreak, "gaps: the endpoint pairing proposes a bridge across the break");
    // Tight closures: the deliberately mean pass. An 18 px break (two tips
    // facing each other head-on over blank paper) is sealed before
    // segmenting; the 20 px one above is past its reach and left for review.
    const FlatArt tight = flatLeakyBox(18);
    FlatsContent sealed = content;
    sealed.params.closeTightGaps = true;
    const FlatEvaluation es = flatEvaluateInk(tight.ink, tight.w, tight.h, sealed);
    bool sealsBreak = false;
    for (size_t i = 0; i + 3 < es.closures.size(); i += 4)
      if (std::fabs(es.closures[i] - 100) <= 4 && std::fabs(es.closures[i + 2] - 100) <= 4) sealsBreak = true;
    check(sealsBreak && es.fillAt(200, 150) != es.fillAt(5, 5), "gaps: a tight closure seals an 18px break before segmenting");
    const FlatEvaluation e20 = flatEvaluateInk(a.ink, a.w, a.h, sealed);
    check(e20.closures.empty(), "gaps: the 20px break is past the tight-closure pass and is left for review");
    // Accepting a bridge as a recorded stroke closes the box on replay.
    FlatsContent bridged = content;
    bridged.edits.bridges.push_back({bridged.edits.nextId++, FlatPolyline{100, 136, 100, 164}, false});
    const FlatEvaluation eb = flatEvaluateInk(a.ink, a.w, a.h, bridged);
    check(eb.fillAt(200, 150) != eb.fillAt(5, 5), "gaps: an accepted bridge stroke closes the box on replay");
    // And erasing it re-opens the leak. The eraser is a 6 px disc, so one dab
    // leaves a 12 px break the ball still cannot pass; a short stroke clears
    // enough of the bridge to let it through.
    bridged.edits.bridges.push_back({bridged.edits.nextId++, FlatPolyline{100, 142, 100, 158}, true});
    const FlatEvaluation ee = flatEvaluateInk(a.ink, a.w, a.h, bridged);
    check(ee.fillAt(200, 150) == ee.fillAt(5, 5), "gaps: an erase stroke over the bridge re-opens it");
  }

  // --- 6. the tools: each gesture is one recorded edit -----------------------
  {
    const FlatArt a = flatThreeBoxes();
    Layer layer;
    layer.kind = LayerKind::Flats;
    layer.flats.params.sheet = 0;
    layer.flats.params.minRegion = 50;
    layer.flats.params.declutter = 0;
    layer.flats.params.closeTightGaps = false;
    auto evaluate = [&]() { return flatEvaluateInk(a.ink, a.w, a.h, layer.flats); };
    FlatEvaluation e = evaluate();
    const FlatRgb red{200, 40, 40};
    check(flatsBucketRecolor(layer, e, 90, 90, red, -1, false) == 1 && layer.flats.edits.recolors.size() == 1,
          "tool: the bucket recolours the fill under the click as one recorded edit");
    e = evaluate();
    check(e.fills[e.fillAt(90, 90)].color == red, "tool: ...which the next evaluation shows");
    check(flatsBucketRecolor(layer, e, 90, 90, FlatRgb{1, 2, 3}, -1, false) == 1 && layer.flats.edits.recolors.size() == 1,
          "tool: recolouring the same fill again replaces its note rather than stacking one");
    e = evaluate();
    // Shift: every fill of the clicked colour. Paint two boxes red first.
    flatsBucketRecolor(layer, e, 90, 90, red, -1, false);
    flatsBucketRecolor(layer, e, 240, 90, red, -1, false);
    e = evaluate();
    check(flatsBucketRecolor(layer, e, 90, 90, FlatRgb{9, 9, 9}, -1, true) == 2,
          "tool: Shift-click recolours every fill wearing the clicked colour");
    // Fills reach under the ink (the line art overlays with no fringe), so a
    // click ON a stroke recolours the fill owning that half of it -- the
    // behaviour autoFlats has. Only a click off the image is nothing.
    check(flatsBucketRecolor(layer, e, -5, -5, red, -1, false) == 0 &&
              flatsBucketRecolor(layer, e, 31, 90, FlatRgb{9, 9, 9}, -1, false) == 1,
          "tool: a click off the image records nothing; a click on a stroke recolours its fill");
    check(flatsDeleteFill(layer, e, 165, 220) && layer.flats.edits.deleteMarks.size() == 1,
          "tool: K records a delete mark on the fill under the cursor");
    e = evaluate();
    check(e.fills[e.fillAt(165, 220)].deleted && !flatsDeleteFill(layer, e, 165, 220),
          "tool: ...and a second K on the deleted fill records nothing");
    check(!flatsMergePair(layer, e, 90, 90, 5, 5) && !flatsMergePair(layer, e, 90, 90, 95, 95),
          "tool: a merge into the background, or of a fill with itself, is refused");
    check(flatsMergePair(layer, e, 90, 90, 240, 90) && layer.flats.edits.mergePairs.size() == 1,
          "tool: M twice records a merge pair");
    e = evaluate();
    check(e.fillAt(90, 90) == e.fillAt(240, 90), "tool: ...which merges the two fills on evaluation");
    check(flatsBucketCarve(layer, e, 350, 50) && layer.flats.edits.carves.size() == 1,
          "tool: Option-click records a carve where a ball fits");
    check(!flatsBucketCarve(layer, e, 31, 90), "tool: ...and refuses on ink, recording nothing");
    check(flatsGroupFromPath(layer, FlatPolyline{20, 20, 320, 20, 320, 160, 20, 160}) &&
              layer.flats.edits.groups.size() == 1 && layer.flats.edits.groups[0].name == "Group 1",
          "tool: a lasso path becomes a named group");
    check(flatsShapeFromPath(layer, FlatPolyline{10, 10, 20, 10, 20, 20, 10, 20}, red) &&
              layer.flats.edits.shapeFills.size() == 1,
          "tool: a lasso path becomes a shape fill");
    check(!flatsGroupFromPath(layer, FlatPolyline{1, 1, 2, 2}), "tool: a two-point path is not a lasso");
    const int before = static_cast<int>(layer.flats.edits.mergePairs.size());
    const int clustered = flatsClusterSmall(layer, e, flatsClusterMaxArea(layer.flats.params));
    check(clustered == static_cast<int>(layer.flats.edits.mergePairs.size()) - before,
          "tool: cluster-small records one merge pair per cluster");
    check(flatsRemoveEditAt(layer, 165, 220, 4.f) && layer.flats.edits.deleteMarks.empty(),
          "tool: the edit picker removes the nearest recorded edit");

    // Gap acceptance, on the leaky box.
    const FlatArt lb = flatLeakyBox(20);
    Layer gl;
    gl.kind = LayerKind::Flats;
    gl.flats.params = layer.flats.params;
    gl.flats.params.autoMergeLeaks = false;
    FlatEvaluation ge = flatEvaluateInk(lb.ink, lb.w, lb.h, gl.flats);
    // The evaluation's own suggestions come from fronts; the endpoint pairing
    // is the tool's second source. Either way, accepting a bridge that spans
    // the break closes it.
    const FlatSegs segs = flatSuggestGaps(lb.line, lb.w, lb.h, gl.flats.params.gapSize, &ge.labels);
    FlatEvaluation withSeg = ge;
    withSeg.suggestions = flatBridgePaths(segs, nullptr, lb.line, lb.w, lb.h);
    check(!flatsAcceptSuggestion(gl, withSeg, -1) && !flatsAcceptSuggestion(gl, withSeg, 99),
          "tool: Return with no focused suggestion records nothing");
    check(!withSeg.suggestions.empty() && flatsAcceptSuggestion(gl, withSeg, 0) && gl.flats.edits.bridges.size() == 1,
          "tool: Return records the focused suggestion as a bridge stroke");
    ge = flatEvaluateInk(lb.ink, lb.w, lb.h, gl.flats);
    check(ge.fillAt(200, 150) != ge.fillAt(5, 5), "tool: ...and the box is closed on the next evaluation");
    check(flatRgbFromSrgb({1.f, 0.5f, 0.f}) == FlatRgb{255, 128, 0} && flatRgbFromSrgb({2.f, -1.f, 0.f}) == FlatRgb{255, 0, 0},
          "tool: the foreground's sRGB floats quantise and clamp to 8-bit");
  }

  // --- 7. ink extraction runs in the display domain --------------------------
  {
    const uint8_t px[4 * 4] = {0, 0, 0, 255,      255, 255, 255, 255,  // black, white
                               0, 0, 0, 0,        255, 0, 0, 255};     // transparent, saturated red
    const FlatInk ink = flatExtractInk(px, 4, 1, 0.30f);
    check(ink[0] == 255 && ink[1] == 0 && ink[2] == 0 && ink[3] == 0,
          "ink: black is line, white and transparent are not, a red construction line is rejected");
    const uint8_t grey[4] = {128, 128, 128, 255};
    check(flatExtractInk(grey, 1, 1, 0.30f)[0] == 127, "ink: mid-grey reads as half darkness (8-bit display values)");
  }

  std::printf("[selftest] flats %s\n", check.ok ? "PASS" : "FAIL");
  return check.ok;
}

}  // namespace np
