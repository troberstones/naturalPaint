#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/ToolSwitch.hpp"

namespace np {

// ---------------------------------------------------------------------------
// app/ToolSwitch -- the spring-loaded Eyedropper (Alt/Option).
//
// **Modelled on the Hand, byte-identically, on the Hand's own half.**
// `beginSpringHand()`/`endSpringHand()` are untouched by this track, and
// section 2 of `app/selftest/ToolSwitch.cpp` is still the proof of that half.
// What is new here is a second borrow of the same shape -- `brush.tool`
// really becomes `Tool::Eyedropper` for the hold, which is what lets
// `ui/MacPaintUI.cpp`'s existing `toolSamplesCanvas(st.brush.tool)` sample
// gate and `ui/MacPaintUI.cpp`'s existing `toolCursorOnTarget(st.brush.tool,
// ...)` cursor call both pick it up with **no change to either read**: both
// already key off `st.brush.tool`, the same way `toolPansView(st.brush.tool)`
// already picks up the Hand's own borrow. `effectiveTool()` keeps its
// existing meaning for both springs -- "the tool the user believes is
// selected" -- so it reports the tool the eyedropper was borrowed FROM while
// the borrow is live, not `Eyedropper` itself; section 2 below asserts this
// explicitly because it is the one place a reader modelling this on the Hand
// could reasonably guess the other way.
//
// **Eligibility is a business rule, not a UI guard**, so it is checked once,
// inside `beginSpringEyedropper()` itself, rather than at the
// `ui/MacPaintUI.cpp` call site -- which is what lets section 4 below drive
// "a begin while ineligible is refused" through the public function alone,
// headless, with no key press and no frame.
//
// **The eligibility table is hand-written**, not derived from
// `springEyedropperEligible()` itself -- asserting a function against its
// own output proves nothing. It is walked over the whole `Tool` enum rather
// than sampled, the same "the enum walked, not counted" argument
// `app/selftest/ToolSwitch.cpp`'s own file comment makes about `kToolMeta`.
//
// Headless and GPU-free: app/ToolSwitch only.
// ---------------------------------------------------------------------------

namespace {

// The hand-written oracle. Every tool this track's brief names as eligible
// is listed `true`; everything else, including the five call-outs (Clone
// Stamp's source pick, the four selection tools' Alt-subtract, Zoom's
// Alt-out, Pen's gnomon suppression, the Flats bucket's carve), is `false`
// by falling through to the `default`. `Tool::PaintBucket` is the only tool
// whose answer depends on `fill`, handled before the switch so the switch
// itself stays a flat membership table.
bool expectedEligible(Tool t, BucketFill fill) noexcept {
  if (t == Tool::PaintBucket) return fill == BucketFill::Colour;
  switch (t) {
    case Tool::Brush:
    case Tool::Water:
    case Tool::DryBrush:
    case Tool::Pencil:
    case Tool::Eraser:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Smudge:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool runSpringEyedropperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  std::printf(
      "[selftest] spring eyedropper: Alt/Option borrows Tool::Eyedropper, and hands it back\n");

  // =========================================================================
  // 1. Eligibility, walked over every (Tool, BucketFill) pair
  // =========================================================================
  {
    bool matchesOracleEverywhere = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      for (BucketFill fill : {BucketFill::Colour, BucketFill::Flats}) {
        if (springEyedropperEligible(t, fill) != expectedEligible(t, fill))
          matchesOracleEverywhere = false;
      }
    }
    check(matchesOracleEverywhere,
          "springeyedrop: springEyedropperEligible() matches a hand-written table "
          "for EVERY (Tool, BucketFill) pair -- Brush/Water/DryBrush/Pencil/"
          "Eraser/Dodge/Burn/Smudge always, PaintBucket only in Colour mode, "
          "everything else (including Flats-mode PaintBucket) never");

    // The one tool whose answer is not a constant, spelled out rather than
    // trusted to the walk above: a Colour<->Flats bug that only shows up on
    // PaintBucket would still make the walk fail, but this line says WHICH
    // tool and WHICH mode without having to re-derive it from a FAIL count.
    check(springEyedropperEligible(Tool::PaintBucket, BucketFill::Colour) &&
              !springEyedropperEligible(Tool::PaintBucket, BucketFill::Flats),
          "springeyedrop: the paint bucket has Alt to lend only in Colour mode -- "
          "Flats keeps it for carving a fill out of a leaked area (ADR-0009)");
  }

  // =========================================================================
  // 2. Begin/end restores the exact prior tool, and effectiveTool() keeps its
  //    existing meaning through the borrow
  // =========================================================================
  {
    AppState st;
    setActiveTool(st, Tool::Water);
    const Tool ledgerBefore = previousTool(st);

    check(!springEyedropperHeld(st) && effectiveTool(st) == Tool::Water,
          "springeyedrop: with Alt up there is no borrow, and the effective "
          "tool is simply the selected one");

    check(beginSpringEyedropper(st) && st.brush.tool == Tool::Eyedropper,
          "springeyedrop: Alt down over an eligible tool borrows the "
          "Eyedropper -- brush.tool really becomes it, the same shape as the "
          "Hand's borrow, so toolSamplesCanvas(st.brush.tool) and the cursor's "
          "toolCursorOnTarget(st.brush.tool, ...) both pick it up unmodified");
    check(effectiveTool(st) == Tool::Water,
          "springeyedrop: ...while the tool the USER is in stays what they "
          "picked, exactly like the Hand's own borrow -- NOT Tool::Eyedropper");
    check(previousTool(st) == ledgerBefore && hasPreviousTool(st),
          "springeyedrop: and the borrow writes NO ledger entry, the Hand's "
          "own reasoning: recording 'previous = Eyedropper' here would make "
          "the feature erase itself one sample at a time");

    // Auto-repeat / a second press with no intervening release.
    check(!beginSpringEyedropper(st) && st.brush.tool == Tool::Eyedropper &&
              effectiveTool(st) == Tool::Water,
          "springeyedrop: a repeat press is refused rather than re-borrowing");

    check(endSpringEyedropper(st) && st.brush.tool == Tool::Water,
          "springeyedrop: Alt up gives back exactly the tool that was held");
    check(previousTool(st) == ledgerBefore && effectiveTool(st) == Tool::Water,
          "springeyedrop: and the whole borrow is invisible to the ledger "
          "afterwards");

    // "with no previous tool ever set": a release with no press must be a
    // no-op, not an install of whatever `springEyedropperReturn` holds.
    AppState fresh;
    setActiveTool(fresh, Tool::Dodge);
    check(!endSpringEyedropper(fresh) && fresh.brush.tool == Tool::Dodge,
          "springeyedrop: an Alt release with no press behind it changes "
          "nothing -- it must not install a stale return tool");

    // Walked over every eligible tool, not sampled at one: the shape above
    // proven once for Water is not proof it holds for Smudge.
    bool restoresEveryEligibleTool = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      // Colour mode: the more permissive of PaintBucket's two, and the
      // default a fresh AppState starts in, so a plain `setActiveTool()`
      // fixture below is really exercising it.
      if (!expectedEligible(t, BucketFill::Colour)) continue;
      AppState w;
      setActiveTool(w, t);
      const bool began = beginSpringEyedropper(w);
      const bool sampledDuring = w.brush.tool == Tool::Eyedropper && effectiveTool(w) == t;
      const bool ended = endSpringEyedropper(w);
      if (!began || !sampledDuring || !ended || w.brush.tool != t)
        restoresEveryEligibleTool = false;
    }
    check(restoresEveryEligibleTool,
          "springeyedrop: EVERY eligible tool borrows and gives itself back "
          "exactly (the enum walked, not sampled)");
  }

  // =========================================================================
  // 3. A begin while ineligible is refused
  // =========================================================================
  {
    // A representative spread of the call-outs the brief names, plus the
    // Flats-mode bucket, plus the enum walked in full below.
    AppState clone;
    setActiveTool(clone, Tool::CloneStamp);
    check(!beginSpringEyedropper(clone) && clone.brush.tool == Tool::CloneStamp &&
              !springEyedropperHeld(clone),
          "springeyedrop: Clone Stamp keeps Alt for its own source pick -- a "
          "begin over it is refused and changes nothing");

    AppState zoom;
    setActiveTool(zoom, Tool::Zoom);
    check(!beginSpringEyedropper(zoom) && zoom.brush.tool == Tool::Zoom,
          "springeyedrop: Zoom keeps Alt-out -- refused");

    AppState pen;
    setActiveTool(pen, Tool::Pen);
    check(!beginSpringEyedropper(pen) && pen.brush.tool == Tool::Pen,
          "springeyedrop: Pen keeps Alt to suppress its gnomon -- refused");

    AppState marquee;
    setActiveTool(marquee, Tool::Marquee);
    check(!beginSpringEyedropper(marquee) && marquee.brush.tool == Tool::Marquee,
          "springeyedrop: Marquee keeps Alt-subtract -- refused");

    AppState flatsBucket;
    setActiveTool(flatsBucket, Tool::PaintBucket);
    flatsBucket.bucketFill = BucketFill::Flats;
    check(!beginSpringEyedropper(flatsBucket) && flatsBucket.brush.tool == Tool::PaintBucket,
          "springeyedrop: the Flats-mode bucket keeps Alt to carve a fill -- "
          "refused, even though the SAME tool in Colour mode is eligible");

    // The enum walked, not sampled: every tool the oracle marks ineligible,
    // in Colour mode (the more permissive of the two for PaintBucket).
    bool refusedEverywhereIneligible = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (expectedEligible(t, BucketFill::Colour)) continue;
      AppState w;
      setActiveTool(w, t);
      const bool began = beginSpringEyedropper(w);
      if (began || w.brush.tool != t || springEyedropperHeld(w))
        refusedEverywhereIneligible = false;
    }
    check(refusedEverywhereIneligible,
          "springeyedrop: EVERY ineligible tool refuses the begin and changes "
          "nothing (the enum walked, not sampled)");
  }

  // =========================================================================
  // 4. The two springs are mutually exclusive, in both directions
  // =========================================================================
  //
  // Reachable: Alt held with the mouse still up borrows the Eyedropper (no
  // drag has claimed Space's own "no mouse down" guard yet), and Space
  // pressed before Alt lifts would otherwise install the Hand over it -- or
  // the same sequence with the two keys the other way round.
  {
    AppState st;
    setActiveTool(st, Tool::Pencil);
    check(beginSpringEyedropper(st) && st.brush.tool == Tool::Eyedropper,
          "springeyedrop: the Eyedropper borrow begins normally");
    check(!beginSpringHand(st) && st.brush.tool == Tool::Eyedropper &&
              !springHandHeld(st),
          "springeyedrop: a Hand-borrow attempted WHILE the Eyedropper is "
          "held is refused -- brush.tool stays Eyedropper, not Hand, and "
          "nothing overwrites the one field either borrow has to restore");
    check(endSpringEyedropper(st) && st.brush.tool == Tool::Pencil,
          "springeyedrop: ending the Eyedropper borrow still hands back "
          "Pencil, undisturbed by the refused Hand attempt");

    AppState reverse;
    setActiveTool(reverse, Tool::Burn);
    check(beginSpringHand(reverse) && reverse.brush.tool == Tool::Hand,
          "springeyedrop: the Hand borrow begins normally");
    check(!beginSpringEyedropper(reverse) && reverse.brush.tool == Tool::Hand &&
              !springEyedropperHeld(reverse),
          "springeyedrop: ...and the SAME refusal the other way round -- an "
          "Eyedropper-borrow attempted while the Hand is held changes nothing");
    check(endSpringHand(reverse) && reverse.brush.tool == Tool::Burn,
          "springeyedrop: ending the Hand borrow hands back Burn, undisturbed "
          "by the refused Eyedropper attempt");

    // Never simultaneously true, for any sequence a two-key chord can
    // actually produce -- the pair of booleans this whole section exists to
    // keep from both being set.
    check(!(springHandHeld(reverse) && springEyedropperHeld(reverse)) &&
              !(springHandHeld(st) && springEyedropperHeld(st)),
          "springeyedrop: a Hand spring and an Eyedropper spring are never "
          "both live at once, in either fixture above");
  }

  std::printf("[selftest] spring eyedropper %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
