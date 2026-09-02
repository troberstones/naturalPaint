#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/GradientTool.hpp"
#include "app/StrokeSession.hpp"
#include "ops/Gradient.hpp"

namespace np {

// app/GradientTool -- `Tool::Gradient`, the palette's `G` cell.
//
// What this section proves is NOT the gradient renderer: `ops/Gradient` has
// its own section, and this file adds no second ramp evaluator for it to
// disagree with. What is new is the tool's own three answers -- which ramp,
// aimed how, and is this drag a gradient at all -- plus the one property
// those three functions exist to protect and that no unit test of
// `ops/Gradient` could ever state:
//
//   **What the options bar swatch draws is what the canvas receives.**
//
// The swatch is a preview, and a preview's only failure mode that matters is
// lying. Three readers show this ramp (`app/GradientTool.hpp` § 1) and the
// two that a user compares -- the swatch and the committed pixels -- run in
// different files, in different coordinate conventions, one straight and one
// premultiplied. Asserting that they call the same function is worth
// something; asserting that the PIXELS match the SWATCH end to end, through
// the real renderer, is the claim the user actually makes when they trust
// the toolbar. That is § 6 below.
//
// Headless and GPU-free. The tool's UI half (the options-bar block in
// ui/AtelierChrome.cpp and the drag block in ui/MacPaintUI.cpp) is out of
// reach here, which is exactly why the three decisions were lifted into
// app/GradientTool in the first place.
bool runGradientToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] gradient tool: spread table, the ramp, the aim, swatch-equals-canvas\n");

  // A foreground that is asymmetric in all three channels and in none of them
  // near 0 or 1, so "the colour came through" is a real claim: a bug that
  // dropped a channel, swapped two, or scaled by alpha would all move at
  // least one of these numbers.
  const std::array<float, 4> kFg{0.21f, 0.64f, 0.37f, 1.0f};

  // -----------------------------------------------------------------------
  // § 1. The spread table covers the enum
  // -----------------------------------------------------------------------
  //
  // `kGradientSpreads` is the one place `ops/Gradient`'s vocabulary and the
  // options bar's meet (`app/GradientTool.hpp` § 4), and a table indexed
  // beside an enum is precisely the shape `kToolMeta` had when it shipped
  // rows in the wrong order -- because its `static_assert` checked the COUNT,
  // which passes on any permutation and on any duplicate. So this walks the
  // enum rather than counting it.
  {
    const GradientSpread kAll[] = {GradientSpread::Pad, GradientSpread::Repeat,
                                   GradientSpread::Reflect};
    static_assert(sizeof(kAll) / sizeof(kAll[0]) == kGradientSpreadCount,
                  "this list must name every GradientSpread");
    bool everyValueOnce = true;
    for (GradientSpread want : kAll) {
      size_t seen = 0;
      for (size_t i = 0; i < kGradientSpreadCount; ++i)
        if (kGradientSpreads[i].spread == want) ++seen;
      if (seen != 1) everyValueOnce = false;
    }
    check(everyValueOnce, "gradient/spread: every GradientSpread appears exactly once");

    // The label the combo shows must be the row's own, not a parallel switch
    // that happens to agree today.
    bool labelsAgree = true;
    for (size_t i = 0; i < kGradientSpreadCount; ++i)
      if (std::strcmp(gradientSpreadLabel(kGradientSpreads[i].spread),
                      kGradientSpreads[i].label) != 0)
        labelsAgree = false;
    check(labelsAgree, "gradient/spread: gradientSpreadLabel() returns the table's label");

    // Distinct and non-empty, both ways. Two rows sharing a label is a combo
    // in which one of three modes is unreachable and the user cannot tell
    // which one they picked -- green under every assertion above.
    bool distinct = true;
    for (size_t i = 0; i < kGradientSpreadCount; ++i) {
      if (kGradientSpreads[i].label == nullptr || kGradientSpreads[i].label[0] == '\0')
        distinct = false;
      if (kGradientSpreads[i].tip == nullptr || kGradientSpreads[i].tip[0] == '\0')
        distinct = false;
      for (size_t j = i + 1; j < kGradientSpreadCount; ++j)
        if (std::strcmp(kGradientSpreads[i].label, kGradientSpreads[j].label) == 0)
          distinct = false;
    }
    check(distinct, "gradient/spread: labels and tips are non-empty and labels distinct");

    // The default is Clamp/Pad. Stated here rather than left to the struct's
    // initialiser, because a default that quietly became Repeat would make
    // every short drag tile the ramp across the whole canvas -- a change
    // nothing else in this suite would notice.
    const GradientToolState fresh;
    check(fresh.spread == GradientSpread::Pad,
          "gradient/spread: a fresh GradientToolState clamps");
  }

  // -----------------------------------------------------------------------
  // § 2. The ramp is foreground-to-transparent, and does not darken
  // -----------------------------------------------------------------------
  //
  // The trap this guards is named in `app/GradientTool.hpp` § 5 and is
  // invisible on a white canvas: fading toward a transparent BLACK instead of
  // a transparent foreground. Both spellings have alpha 1 at t=0 and alpha 0
  // at t=1; they differ only in the colour carried through the middle, which
  // is why the midpoint is where this is asserted.
  {
    const GradientStops stops = gradientToolStops(kFg);
    check(stops.colorStops.size() == 2 && stops.opacityStops.size() == 2,
          "gradient/ramp: two colour stops and two opacity stops");

    bool endsCarryFg = stops.colorStops.size() == 2;
    if (endsCarryFg) {
      for (const ColorStop& cs : stops.colorStops) {
        if (cs.color[0] != kFg[0] || cs.color[1] != kFg[1] || cs.color[2] != kFg[2])
          endsCarryFg = false;
        if (cs.midpoint != 0.5f) endsCarryFg = false;
      }
      if (stops.colorStops[0].position != 0.0f || stops.colorStops[1].position != 1.0f)
        endsCarryFg = false;
    }
    check(endsCarryFg,
          "gradient/ramp: both colour stops are the foreground at 0 and 1");

    const bool fade = stops.opacityStops.size() == 2 &&
                      stops.opacityStops[0].position == 0.0f &&
                      stops.opacityStops[0].opacity == 1.0f &&
                      stops.opacityStops[1].position == 1.0f &&
                      stops.opacityStops[1].opacity == 0.0f;
    check(fade, "gradient/ramp: the opacity stops carry the fade, 1 -> 0");

    // The load-bearing one. STRAIGHT colour at the midpoint is the full
    // foreground -- the alpha is what has halved, not the colour. A ramp
    // interpolating toward transparent black would read ~0.105/0.32/0.185
    // here, every channel exactly half, and would look like a shadow.
    const std::array<float, 4> mid = gradientSampleStraight(stops, 0.5f);
    const bool noDarkening = std::fabs(mid[0] - kFg[0]) < 1e-6f &&
                             std::fabs(mid[1] - kFg[1]) < 1e-6f &&
                             std::fabs(mid[2] - kFg[2]) < 1e-6f &&
                             std::fabs(mid[3] - 0.5f) < 1e-6f;
    check(noDarkening,
          "gradient/ramp: the midpoint fades alpha only, not toward black");

    // A foreground alpha must not multiply into the ramp: the opacity stops
    // own alpha entirely (`app/GradientTool.hpp` § 5), and the day the colour
    // panel grows an alpha slider is the day a swatch built one way and a
    // commit built the other stop matching.
    const std::array<float, 4> translucentFg{kFg[0], kFg[1], kFg[2], 0.25f};
    const GradientStops other = gradientToolStops(translucentFg);
    const std::array<float, 4> a = gradientSampleStraight(stops, 0.31f);
    const std::array<float, 4> b = gradientSampleStraight(other, 0.31f);
    check(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3],
          "gradient/ramp: the foreground's own alpha is ignored");
  }

  // -----------------------------------------------------------------------
  // § 3. The aim carries every field, including the one that is easy to drop
  // -----------------------------------------------------------------------
  //
  // `spread` is the field a hand-written second copy forgets, because the
  // four handle coordinates are obviously required and it is not. A geometry
  // built without it silently clamps, so the preview and the commit agree
  // whenever the user leaves the default alone and diverge the moment they
  // do not -- which is the worst possible distribution of a bug.
  {
    bool carried = true;
    for (size_t i = 0; i < kGradientSpreadCount; ++i) {
      GradientToolState tool;
      tool.spread = kGradientSpreads[i].spread;
      const GradientGeometry g = gradientToolGeometry(tool, 3.5f, -7.25f, 190.0f, 42.0f);
      if (g.spread != kGradientSpreads[i].spread) carried = false;
      if (g.x0 != 3.5f || g.y0 != -7.25f || g.x1 != 190.0f || g.y1 != 42.0f) carried = false;
      if (g.kind != GradientKind::Linear) carried = false;
    }
    check(carried, "gradient/aim: handles, Linear kind and spread all survive");
  }

  // -----------------------------------------------------------------------
  // § 4. The one degeneracy test both readers use
  // -----------------------------------------------------------------------
  //
  // Its exact threshold matters less than there being ONE of it
  // (`app/GradientTool.hpp` § 7). What is asserted is the boundary, so that a
  // future edit which loosens or tightens it has to come through here rather
  // than through only one of the two call sites.
  {
    check(!gradientDragIsUsable(10.0f, 10.0f, 10.0f, 10.0f),
          "gradient/degenerate: a click with no drag is not a gradient");
    check(!gradientDragIsUsable(10.0f, 10.0f, 10.5f, 10.0f),
          "gradient/degenerate: half a texel is not a gradient");
    check(gradientDragIsUsable(10.0f, 10.0f, 11.0f, 10.0f),
          "gradient/degenerate: exactly one texel is");
    check(gradientDragIsUsable(10.0f, 10.0f, 9.0f, 11.0f),
          "gradient/degenerate: direction and sign do not matter");
  }

  // -----------------------------------------------------------------------
  // § 4a. Why the gesture may not borrow `marqueeDragging`
  // -----------------------------------------------------------------------
  //
  // The defect that made this tool do nothing at all
  // (`app/GradientTool.hpp` § 3a) was the conjunction of exactly two facts
  // about the tool tables, and this pins both of them so that a change to
  // either arrives here with the explanation attached rather than as a tool
  // that silently stops drawing again:
  //
  //   * the gradient WRITES RGB pixels, so its canvas block lives under
  //     `toolWritesRgbPixels()`;
  //   * the gradient does NOT draw selections, so on every frame it is
  //     active the selection switch above that block takes its `else` arm --
  //     the one whose job is to cancel a selection drag abandoned by a tool
  //     change, and which does that by clearing `marqueeDragging`.
  //
  // Together those mean any gradient state stored in `marqueeDragging` is
  // erased at the top of the frame after it is written. The fix was a
  // separate `GradientDrag`, and what this section asserts is that the two
  // facts which made sharing fatal are both still true -- because if either
  // ever flips, the note in that header stops describing this build.
  {
    check(toolWritesRgbPixels(Tool::Gradient),
          "gradient/gesture: the gradient writes RGB pixels");
    check(!toolDrawsSelection(Tool::Gradient),
          "gradient/gesture: the gradient is NOT a selection tool");

    // And the state really is separate: setting one leaves the other alone.
    // Cheap, and it is the property the whole fix consists of -- a later
    // refactor that made `GradientDrag::active` a view onto the shared flag
    // would reintroduce the bug with every other assertion here still green.
    AppState st;
    st.gradientDrag.active = true;
    st.gradientDrag.x0 = 12.0f;
    const bool independent = !st.marqueeDragging && st.marqueeX0 != 12.0f;
    st.marqueeDragging = true;
    check(independent && st.gradientDrag.active,
          "gradient/gesture: the drag does not alias the marquee's flag");
  }

  // -----------------------------------------------------------------------
  // § 5. Scratch geometry shared by § 6 and § 7
  // -----------------------------------------------------------------------
  //
  // 64x4, a full drag across the width. Small enough that the loops below are
  // free, wide enough that the ramp is genuinely sampled rather than hitting
  // only its two ends.
  constexpr int32_t kW = 64;
  constexpr int32_t kH = 4;
  const GradientRegion region{0, 0, kW, kH};
  const GradientStops stops = gradientToolStops(kFg);

  // -----------------------------------------------------------------------
  // § 6. **Swatch equals canvas**
  // -----------------------------------------------------------------------
  //
  // The options bar draws column i of its swatch as
  // `gradientSampleStraight(currentGradientStops(brush), t)`, encoded for
  // display. The canvas receives `renderGradient(..., currentGradientStops(
  // brush), ...)`. This asserts those two are the same ramp by running the
  // real renderer onto an EMPTY layer and reading the texels back.
  //
  // Empty is what makes the comparison exact: `compositeOver` onto a fully
  // transparent destination is the identity, so the stored texel is the
  // source and nothing else. The one transform in between is the
  // premultiply, which `ops/Gradient` applies deliberately and documents --
  // so it is applied here too rather than tolerated as slop. A test that
  // allowed for it with a loose epsilon would pass on a build that had
  // stopped premultiplying at all.
  {
    GradientToolState tool;  // Pad
    TileStore tiles;
    const GradientGeometry geom =
        gradientToolGeometry(tool, 0.0f, 0.0f, static_cast<float>(kW), 0.0f);
    const size_t written = renderGradient(tiles, region, geom, stops, nullptr);
    check(written > 0, "gradient/wysiwyg: the drag wrote texels");

    bool matches = true;
    float worst = 0.0f;
    for (int32_t x = 0; x < kW; ++x) {
      // The swatch's own sampling: texel centres, the same convention
      // `ops/Gradient` states at its inner loop and the same one the
      // options-bar column loop uses.
      const float t = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW);
      const std::array<float, 4> straight = gradientSampleStraight(stops, t);
      const std::array<float, 4> want{straight[0] * straight[3], straight[1] * straight[3],
                                      straight[2] * straight[3], straight[3]};
      const PixelCoord p{x, 1};
      const std::array<float, 4> got =
          tiles.getOrCreate(tileCoordAt(p)).readPixel(tileLocalOffset(p));
      for (int c = 0; c < 4; ++c) {
        const float d = std::fabs(got[c] - want[c]);
        if (d > worst) worst = d;
        // Half-float storage, so this is a storage tolerance and not a
        // formula tolerance: 2^-11 is the format's own relative step near 1.
        if (d > 1.0f / 2048.0f) matches = false;
      }
    }
    check(matches, "gradient/wysiwyg: every canvas texel is the swatch's own sample");
    if (!matches) std::printf("      worst channel difference: %g\n", static_cast<double>(worst));

    // The ends specifically, because they are what a user checks by eye: the
    // start is the foreground at full strength and the far end is nothing at
    // all. A ramp rendered backwards passes the loop above only if the ramp
    // is symmetric -- this one is not, and this states it.
    const PixelCoord first{0, 1};
    const PixelCoord last{kW - 1, 1};
    const std::array<float, 4> f =
        tiles.getOrCreate(tileCoordAt(first)).readPixel(tileLocalOffset(first));
    const std::array<float, 4> l =
        tiles.getOrCreate(tileCoordAt(last)).readPixel(tileLocalOffset(last));
    check(f[3] > 0.98f && l[3] < 0.02f,
          "gradient/wysiwyg: opaque at the pen-down end, clear at the pen-up end");
  }

  // -----------------------------------------------------------------------
  // § 7. The spread control actually reaches the pixels
  // -----------------------------------------------------------------------
  //
  // A combo wired to a field nothing reads is the defect this build has shipped
  // before (`docs/reachability-audit.md`): the control moves, the state
  // changes, every assertion about the state passes, and the canvas is
  // identical. So this renders the SAME drag three times, changing only
  // `GradientToolState::spread`, and asserts the pixels differ.
  //
  // The drag covers the left half only, so the right half is entirely
  // "outside the ramp" -- which is the only region the three modes are
  // allowed to differ in, and the only region where the difference is the
  // whole content of the setting.
  {
    auto renderWith = [&](GradientSpread spread) {
      GradientToolState tool;
      tool.spread = spread;
      TileStore tiles;
      renderGradient(tiles, region,
                     gradientToolGeometry(tool, 0.0f, 0.0f, static_cast<float>(kW) * 0.5f,
                                          0.0f),
                     stops, nullptr);
      // One texel deep into the region past the end handle.
      const PixelCoord p{kW - 4, 1};
      return tiles.getOrCreate(tileCoordAt(p)).readPixel(tileLocalOffset(p));
    };
    const std::array<float, 4> pad = renderWith(GradientSpread::Pad);
    const std::array<float, 4> rep = renderWith(GradientSpread::Repeat);
    const std::array<float, 4> ref = renderWith(GradientSpread::Reflect);

    // Pad holds the last stop, which this ramp makes fully transparent -- so
    // past the end handle Pad writes nothing at all.
    check(pad[3] < 0.02f, "gradient/spread: Clamp holds the ramp's transparent end");
    // Repeat has restarted the ramp, so it is opaque again somewhere out
    // there; Reflect has mirrored it, so it is not the same value as Repeat.
    check(rep[3] > 0.02f, "gradient/spread: Repeat re-enters the opaque end of the ramp");
    check(std::fabs(rep[3] - ref[3]) > 0.01f,
          "gradient/spread: Reflect and Repeat are not the same picture");
    check(std::fabs(pad[3] - rep[3]) > 0.01f,
          "gradient/spread: the setting reaches the pixels at all");
  }

  return ok;
}

}  // namespace np
