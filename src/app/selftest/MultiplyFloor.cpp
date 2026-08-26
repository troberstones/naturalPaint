#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"

namespace np {

// ---------------------------------------------------------------------------
// docs/reachability-audit.md B6 and B7 -- a Multiply target's floor.
//
// B6: `io/AbrBrushes.cpp`'s importer used to fold Photoshop's Minimum
// Diameter into EVERY link it emitted onto Size. Size is a `TargetCombine::
// Multiply` target, so two links (a control and a jitter -- eleven of twelve
// Runny Inkers carry both) composed as a PRODUCT, and each contributing its
// own copy of the floor at source 0 made the floor its own SQUARE rather
// than the single bound Photoshop's dialog promises.
//
// The fix (`BrushLinkSet::multiplyFloor`, brush/Dynamics.hpp) moves the floor
// off every individual link and onto the TARGET, applied once. The hard part
// -- and B6's own header says so -- is not the rule but WHERE: since commit
// `b704411` the Size product is resolved in two halves at two different
// times (`app/StrokeSession::brushTipFor()`'s once-per-frame hardware half,
// `applyStrokeLocalCorrection()`'s once-per-dab stroke-local half), and
// nothing sees "the whole product" at either time. This file's centrepiece
// (§2) is the proof that flooring EITHER half on its own, rather than the
// point where both have already multiplied together, is wrong whenever a
// Multiply contribution exceeds 1.0 -- legal today, since the LINK editor's
// own range slider goes to 2.0.
//
// B7: a hardware source that idles at zero (tilt, at rest or absent) can
// multiply a brush to a radius of zero. The device-UNAVAILABLE half of that
// (a mouse fabricating tilt 0) is already fixed, elsewhere, by
// `sourceUnavailable()` -- not retested here. What THIS file checks (§4) is
// the half B7's own text asks B6's fix to be checked against: a real pen
// GENUINELY reporting tilt 0 still resolves the link (the device is
// available), so Size's product is still 0 -- and the question is whether
// that 0 is now floored to a minimum, or still reaches the canvas as a
// vanished brush.
//
// Every section drives the REAL engine -- `brushTipFor()`, a real
// `StrokeSession` stroke, `evaluateLinks()` -- rather than reimplementing the
// `max()` this file exists to check the placement of. `StrokeSession::
// lastDabRadius()` is the one addition to production code this required: the
// per-dab radius `depositPending()` actually deposits with has no other
// external observer (its own correction step, `applyStrokeLocalCorrection()`,
// has internal linkage, and reading it back out of painted tile data would
// be a test of `dabCoverage()`'s falloff shape as much as of the floor).
// ---------------------------------------------------------------------------
bool runMultiplyFloorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  auto makeDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "multiply-floor");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));
    return od;
  };

  const MixboxLut noLut;  // invalid on purpose -- brushTipFor()'s documented
                          // fallback; radius, not colour, is this file's
                          // whole subject.

  // A generous absolute tolerance for every radius comparison below, derived
  // once here rather than re-guessed per assertion. The literals this file
  // feeds `BrushLink::rangeLo/rangeHi` and `BrushLinkSet::multiplyFloor`
  // (0.1f, 0.15f, 0.2f, 0.25f) are none of them exact binary fractions except
  // 0.25f, so each carries single-precision's own ~1.19e-7 relative rounding;
  // multiplied through a base radius of 100 and folded twice (once per half
  // of a two-phase product) that error is still under 1e-4 absolute -- see
  // the arithmetic worked through in each section's own comment. `kTol` is a
  // full order of magnitude above that (absorbs it with room to spare) and
  // at least two orders below the smallest gap any section here actually
  // distinguishes (4 px, between section 1's "before" and "after"; 25 px,
  // between section 2's correct answer and the double-floored one) -- wide
  // enough to never flake on rounding, narrow enough that it cannot pass a
  // wrong formula off as a right one.
  constexpr float kTol = 1e-3f;
  constexpr float kBaseRadius = 100.0f;

  // A short straight-line stroke, read back through `lastDabRadius()` after
  // it ends. **Not a single `addPoint()` call** -- `brush/StrokePath.hpp`'s
  // own contract says a 0- or 1-sample stroke emits nothing at all, even
  // through `flush()` at `end()` (it needs two real samples on either side of
  // a segment before it will walk it), so a "one-dab" helper built on exactly
  // one `addPoint()` would silently deposit zero dabs and leave
  // `lastDabRadius()` at its untouched 0.0f default -- indistinguishable from
  // a genuinely-floored-to-zero brush, which is exactly the wrong thing for
  // this file's own B7 section to be unable to tell apart. Six points 40 px
  // apart (240 px of path) comfortably clears even this brush's widest
  // possible per-dab spacing (`spacing * radius`, 0.25 * 100 px = 25 px at
  // the largest radius any fixture in this file uses), so at least one real
  // dab is always emitted regardless of which section calls this.
  //
  // Every fixture this helper is called with drives Size from HARDWARE
  // sources only (Pressure, Tilt) with no stroke-local link -- so every dab
  // along the path shares the identical resolved radius, and which one
  // happens to be "last" does not matter.
  //
  // `inputs` defaults to a plain `DynamicInputs{}` (a mouse: full pressure,
  // no tilt device) -- explicit at every call site that needs anything else,
  // since a hardcoded default here once silently fed section 4's "tilt 0"
  // fixtures a `hasTilt = false` sample instead of the reporting device the
  // section was actually testing, which skipped the link
  // (`sourceUnavailable()`) instead of resolving it at 0 and made every
  // assertion in that section fail against the WRONG mechanism.
  auto oneDabRadius = [&](const BrushState& brush, const DynamicInputs& inputs = {}) {
    OpenDocument od = makeDoc(1024, 1024);
    StrokeSession s;
    std::string e;
    const BrushTip tip = brushTipFor(brush, noLut, inputs);
    if (!s.begin(od, 1, tip, Tool::Brush, &e, &brush.links)) return -1.0f;  // unreachable if
                                                                            // the fixture is
                                                                            // well-formed; -1
                                                                            // fails every check
                                                                            // below loudly
    for (int i = 0; i < 6; ++i) s.addPoint(400.0f + 40.0f * static_cast<float>(i), 500.0f);
    s.end();
    return s.lastDabRadius();
  };

  // ==========================================================================
  // 1. The squaring itself, in real numbers -- before this fix and after it
  // ==========================================================================
  //
  // Two HARDWARE links on Size (Pressure and Tilt, both resolved in the one
  // `brushTipFor()` call -- no stroke needed) so the product is exact and
  // deterministic, isolating the arithmetic from B6's own real shape
  // (control + RANDOM jitter, checked end to end through the importer in
  // `AbrBrushes.cpp`'s own section 5, not repeated here).
  {
    // BEFORE: the old importer's shape, built by hand -- a floor of 20%
    // baked into EVERY link's own `rangeLo`, `multiplyFloor` never touched.
    // At Pressure 0 and Tilt 0 each link resolves to exactly its `rangeLo`
    // (linear curve, u = 0), so the product is `0.2 * 0.2`.
    BrushState before;
    before.radius = kBaseRadius;
    BrushLink beforeCtrl;
    beforeCtrl.source = DynamicSource::Pressure;
    beforeCtrl.target = DynamicTarget::Size;
    beforeCtrl.rangeLo = 0.2f;
    beforeCtrl.rangeHi = 1.0f;
    addLink(before.links, beforeCtrl);
    BrushLink beforeJit;
    beforeJit.source = DynamicSource::Tilt;
    beforeJit.target = DynamicTarget::Size;
    beforeJit.rangeLo = 0.2f;
    beforeJit.rangeHi = 1.0f;
    addLink(before.links, beforeJit);

    DynamicInputs zeroTilt;
    zeroTilt.pressure = 0.0f;
    zeroTilt.hasTilt = true;  // a REPORTING device at rest, not an absent one
                              // -- see section 4's own comment on why that
                              // distinction matters
    zeroTilt.tilt = 0.0f;
    const BrushTip beforeTip = brushTipFor(before, noLut, zeroTilt);
    // 0.2 is not exact in binary32; 100 * 0.2f * 0.2f lands a few
    // millionths off 4.0, absorbed by `kTol`.
    check(nearf(beforeTip.radius, 4.0f, kTol),
          "floor/before: two links each carrying their own 20% floor square to 4.0 px on a "
          "100 px brush -- the defect B6 measured on eleven of twelve Runny Inkers, "
          "reconstructed by hand from the old shape");
    check(nearf(beforeTip.sizeFloorPx, 0.0f, kTol),
          "floor/before: the old shape never wrote `multiplyFloor` at all -- nothing here is "
          "the NEW mechanism doing this, it is the OLD one, reproduced to have a real 'before' "
          "number to compare against");

    // AFTER: the honest shape -- both links span Size's own whole [0,1],
    // and the SAME 20% lives once, on `multiplyFloor[Size]`.
    BrushState after;
    after.radius = kBaseRadius;
    BrushLink afterCtrl;
    afterCtrl.source = DynamicSource::Pressure;
    afterCtrl.target = DynamicTarget::Size;
    afterCtrl.rangeLo = 0.0f;
    afterCtrl.rangeHi = 1.0f;
    addLink(after.links, afterCtrl);
    BrushLink afterJit;
    afterJit.source = DynamicSource::Tilt;
    afterJit.target = DynamicTarget::Size;
    afterJit.rangeLo = 0.0f;
    afterJit.rangeHi = 1.0f;
    addLink(after.links, afterJit);
    after.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.2f;

    const float afterRadius = oneDabRadius(after, zeroTilt);
    // Product is 0 * 0 = 0 (both links at their honest rangeLo); floored to
    // 0.2 * 100 = 20 px exactly the promised minimum, not its square.
    check(nearf(afterRadius, 20.0f, kTol),
          "floor/after: the identical 20% minimum now reaches the canvas as 20.0 px, not "
          "4.0 px -- the floor once, not the floor squared");
    check(afterRadius > beforeTip.radius * 4.0f,
          "floor/after: a >4x jump from the squared number to the correct one, so this is not "
          "a rounding difference between two nearly-equal answers");
  }

  // ==========================================================================
  // 2. `rangeHi` above 1.0 -- the floor must not be applied in both halves
  // ==========================================================================
  //
  // The worked counter-example from `BrushTip::sizeFloorPx`'s own comment
  // (brush/Deposit.hpp), run for real: base radius 100, a HARDWARE product of
  // 0.1 (Pressure, flat -- `rangeLo == rangeHi` makes the contribution
  // independent of the actual pressure value, so this is exact regardless of
  // what `brushTipFor()` is called with), a floor of 0.25 (25 px), and a
  // STROKE-LOCAL product of 2.0 (Fade, whose own ramp -- `dynamicFade()`,
  // clamp01(distance / 480 px) -- reaches EXACTLY 1.0 once the path has
  // travelled 480 px and holds there, so `rangeHi` alone decides the
  // contribution with no dependence on exactly how many dabs the emitter
  // produced getting there).
  //
  //   Floored in BOTH halves: max(max(100*0.1, 25) * 2.0, 25)
  //                         = max(max(10, 25) * 2.0, 25) = max(50, 25) = 50
  //   Floored ONCE, at the end: max(100 * 0.1 * 2.0, 25) = max(20, 25) = 25
  //
  // 25 is correct; 50 is what flooring the hardware half before the
  // stroke-local multiply runs would produce. This is the case B6's own
  // header calls "the hard part": both numbers are plausible, both come from
  // real per-half floors, and only one of them is what Photoshop's Minimum
  // Diameter actually means.
  {
    BrushState brush;
    brush.radius = kBaseRadius;
    BrushLink pressureLink;
    pressureLink.source = DynamicSource::Pressure;
    pressureLink.target = DynamicTarget::Size;
    pressureLink.rangeLo = 0.1f;
    pressureLink.rangeHi = 0.1f;  // flat: 0.1 regardless of the pressure sample
    addLink(brush.links, pressureLink);
    BrushLink fadeLink;
    fadeLink.source = DynamicSource::Fade;
    fadeLink.target = DynamicTarget::Size;
    fadeLink.rangeLo = 0.0f;  // irrelevant once fade reaches 1.0 -- see above
    fadeLink.rangeHi = 2.0f;
    addLink(brush.links, fadeLink);
    brush.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.25f;

    const BrushTip startTip = brushTipFor(brush, noLut, DynamicInputs{});
    // The hardware half alone, UNFLOORED -- proves `brushTipFor()` really
    // does leave this unfloored (brush/Deposit.hpp's own contract for
    // `BrushTip::sizeFloorPx`), which is the precondition the rest of this
    // section depends on.
    check(nearf(startTip.radius, 10.0f, kTol) && nearf(startTip.sizeFloorPx, 25.0f, kTol),
          "floor/two-halves: brushTipFor() reports the hardware half (10 px) and the floor "
          "(25 px) separately -- neither has touched the other yet");

    OpenDocument od = makeDoc(4096, 4096);
    StrokeSession s;
    std::string e;
    check(s.begin(od, 1, startTip, Tool::Brush, &e, &brush.links),
          "floor/two-halves: the stroke begins");
    // A straight line well past `kFadeLengthPx` (480 px) so the LAST dab's
    // own `distanceTravelled_` clears it and Fade reads exactly 1.0 for that
    // dab -- see this section's header comment.
    for (int i = 0; i <= 16; ++i) s.addPoint(100.0f + 50.0f * static_cast<float>(i), 2000.0f);
    s.end();
    const float finalRadius = s.lastDabRadius();
    check(nearf(finalRadius, 25.0f, kTol),
          "floor/two-halves: the LAST dab's radius is 25 px -- floored once, downstream of "
          "BOTH halves of the product, matching the hand-worked counter-example exactly");
    check(!nearf(finalRadius, 50.0f, 5.0f),
          "floor/two-halves: and specifically NOT 50 px, which is what flooring the hardware "
          "half before the stroke-local multiply runs would have produced -- the mistake this "
          "whole section exists to catch");
  }

  // ==========================================================================
  // 3. No-op when only one link drives Size -- existing brushes do not shift
  // ==========================================================================
  //
  // `multiplyFloor` is written only by `io/AbrBrushes.cpp`'s importer
  // (Size only) and read back by `app/UserBrushLibrary.cpp`'s file parser;
  // no built-in preset (`brush/Library.cpp`'s `defaultBrushLibrary()`) and no
  // hand-authored `BrushLinkSet` ever sets it, so it defaults to 0.0f for
  // every one of them. `std::max(x, 0.0f)` cannot lower an `x` that
  // `linkContribution()` already never lets go negative -- so this is not
  // merely "close to a no-op", it is bit-for-bit exact, asserted with `==`
  // rather than a tolerance.
  {
    BrushState brush;
    brush.radius = kBaseRadius;
    // `defaultBrushLinks()`'s own literal range (brush/Dynamics.cpp), so this
    // is not a contrived fixture -- it is what every new brush already ships
    // with.
    BrushLink pressureSize;
    pressureSize.source = DynamicSource::Pressure;
    pressureSize.target = DynamicTarget::Size;
    pressureSize.rangeLo = 0.25f;
    pressureSize.rangeHi = 1.0f;
    addLink(brush.links, pressureSize);
    // multiplyFloor left at its default {} -- every entry 0.0f.

    bool allExact = true;
    for (float p : {0.0f, 0.35f, 0.5f, 1.0f}) {
      DynamicInputs in;
      in.pressure = p;
      const BrushTip tip = brushTipFor(brush, noLut, in);
      OpenDocument od = makeDoc(1024, 1024);
      StrokeSession s;
      std::string e;
      s.begin(od, 1, tip, Tool::Brush, &e, &brush.links);
      // Six points, not one -- see `oneDabRadius`'s own comment above on why
      // a single `addPoint()` call deposits nothing at all.
      for (int i = 0; i < 6; ++i) s.addPoint(400.0f + 40.0f * static_cast<float>(i), 500.0f);
      s.end();
      // `==`, not `nearf` -- see this section's own header comment for why
      // this is exact rather than approximate.
      if (s.lastDabRadius() != tip.radius) allExact = false;
    }
    check(allExact,
          "floor/no-op: with no Minimum Diameter authored, the deposited radius is BIT-EXACTLY "
          "brushTipFor()'s own unfloored radius at every pressure sampled -- the engine change "
          "changes nothing for a brush that never sets `multiplyFloor`");

    // The check above alone would not catch `BrushLinkSet::multiplyFloor`'s
    // own DEFAULT quietly becoming nonzero (a plausible sabotage: `= {}`
    // becoming, say, `= {0.1f}`) -- `pressureSize`'s 0.25 floor never dips
    // the radius below 25 px, comfortably above a small stray default. A
    // brush whose radius genuinely goes near zero closes that gap: any
    // nonzero default would lift it, which the bit-exact comparison below
    // would catch immediately.
    BrushState tiny;
    tiny.radius = kBaseRadius;
    BrushLink tinyLink;
    tinyLink.source = DynamicSource::Pressure;
    tinyLink.target = DynamicTarget::Size;
    tinyLink.rangeLo = 0.0f;  // radius genuinely reaches 0 at pressure 0
    tinyLink.rangeHi = 1.0f;
    addLink(tiny.links, tinyLink);
    DynamicInputs zeroPressure;
    zeroPressure.pressure = 0.0f;
    const BrushTip tinyTip = brushTipFor(tiny, noLut, zeroPressure);
    check(tinyTip.radius == 0.0f && tinyTip.sizeFloorPx == 0.0f,
          "floor/no-op: a brush with no Minimum Diameter and a Size link that genuinely "
          "reaches 0 at pressure 0 has BOTH its unfloored radius and its floor sitting at "
          "exactly 0.0f -- `multiplyFloor`'s default really is zero, not merely small enough "
          "not to matter for the fixture above");
  }

  // ==========================================================================
  // 4. B7, checked against this fix: a genuinely-reporting device at rest
  // ==========================================================================
  //
  // B7's own text: "a rule that fixes B6 must be checked against B7 rather
  // than chosen for it alone." The ORIGINAL B7 instance -- a mouse (or an
  // absent pen) fabricating tilt 0 -- is fixed elsewhere, by
  // `sourceUnavailable()` skipping the link entirely rather than resolving
  // it at a manufactured 0 (§4c below re-confirms that fix still holds, as a
  // contrast). What is new here is the case B7's own text raises: a REAL pen
  // that DOES report tilt, genuinely held upright (tilt == 0.0, not a
  // fabricated default), still resolves the link -- the device is available
  // -- and Size's product is still exactly 0. The question is what happens
  // to that 0.
  {
    // 4a. A Minimum Diameter IS authored (15%): the brush THINS to its
    // minimum rather than vanishing.
    BrushState withFloor;
    withFloor.radius = kBaseRadius;
    BrushLink tiltLink;
    tiltLink.source = DynamicSource::Tilt;
    tiltLink.target = DynamicTarget::Size;
    tiltLink.rangeLo = 0.0f;  // honest range -- see section 1
    tiltLink.rangeHi = 1.0f;
    addLink(withFloor.links, tiltLink);
    withFloor.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.15f;

    DynamicInputs reporting;
    reporting.hasTilt = true;  // a REAL device, reporting -- not absent
    reporting.tilt = 0.0f;     // ...genuinely upright
    const BrushTip tipWithFloor = brushTipFor(withFloor, noLut, reporting);
    check(nearf(tipWithFloor.radius, 0.0f, kTol),
          "floor/B7: tilt 0 from a REPORTING device still resolves the link at its own honest "
          "rangeLo -- the hardware product genuinely is 0, exactly as B7's text describes");
    const float floored = oneDabRadius(withFloor, reporting);
    check(nearf(floored, 15.0f, kTol),
          "floor/B7: ...and WITH a Minimum Diameter authored, the deposited dab thins to "
          "15 px rather than vanishing -- a real pen reporting tilt 0 gets Photoshop's own "
          "floor behaviour, not a blank canvas");

    // 4b. No Minimum Diameter authored (0%, the default): this brush
    // genuinely vanishes at tilt 0 -- and that is Photoshop's OWN authored
    // intent for a brush with no Minimum Diameter set, not a residual
    // defect this task leaves open. `multiplyFloor` stays at 0.0f (nothing
    // sets it), so `std::max(0.0f, 0.0f) == 0.0f` exactly.
    BrushState noFloor;
    noFloor.radius = kBaseRadius;
    addLink(noFloor.links, tiltLink);
    // multiplyFloor left at its default {}.
    check(oneDabRadius(noFloor, reporting) == 0.0f,
          "floor/B7: ...and WITHOUT one, it still reaches exactly 0 px -- Photoshop's own "
          "Minimum Diameter is opt-in, and a brush that never opted in vanishing at its own "
          "authored 0% floor is the brush working as designed, not the defect this task fixes");

    // 4c. Contrast: the ORIGINAL B7 instance -- no pen present at all --
    // stays fixed. `sourceUnavailable()` skips the link outright, so Size
    // holds its Multiply identity (1.0) regardless of any floor.
    BrushState absent = withFloor;  // same 15% floor, to prove it is moot here
    DynamicInputs noPen;             // hasTilt defaults false: no device
    const BrushTip tipAbsent = brushTipFor(absent, noLut, noPen);
    check(tipAbsent.radius == kBaseRadius,
          "floor/B7: ...and a MOUSE (no tilt device at all) still paints at full, unattenuated "
          "size -- the fix that closed the original B7 (device availability, not this file's "
          "own floor) is untouched by adding a floor mechanism beside it");
  }

  // ==========================================================================
  // 5. The floor lives outside link RESOLUTION entirely -- Add targets stay
  //    unused, not silently ignored
  // ==========================================================================
  //
  // `multiplyFloor` is not read by `evaluateLinksWhere()` (brush/Dynamics.cpp)
  // at all -- every consumer is a specific call site (`app/StrokeSession.cpp`,
  // `app/DabPreview.cpp`, `ui/MacPaintUI.cpp`) reading `multiplyFloor[Size]`
  // directly, never the link-evaluation engine itself. This proves that
  // generally (any target, not only the three `TargetCombine::Add` ones),
  // and then specifically for Angle, Scatter and Hue -- the three targets a
  // floor is not even a coherent concept for (`BrushLinkSet::multiplyFloor`'s
  // own comment) -- so "unused" is demonstrated, not merely asserted in a
  // header comment nobody runs.
  {
    BrushLinkSet links;
    BrushLink angleLink;
    angleLink.source = DynamicSource::Tilt;
    angleLink.target = DynamicTarget::Angle;
    angleLink.rangeLo = 0.0f;
    angleLink.rangeHi = 45.0f;
    addLink(links, angleLink);
    BrushLink sizeLink;
    sizeLink.source = DynamicSource::Pressure;
    sizeLink.target = DynamicTarget::Size;
    sizeLink.rangeLo = 0.3f;
    sizeLink.rangeHi = 1.0f;
    addLink(links, sizeLink);

    DynamicInputs in;
    in.pressure = 0.6f;
    in.hasTilt = true;
    in.tilt = 0.4f;
    const DynamicResult before = evaluateLinks(links, in);

    // Every slot, including Size's OWN, set to a large nonzero garbage value
    // -- if `evaluateLinks()` read `multiplyFloor` ANYWHERE, at least one of
    // these twelve would move the result.
    for (size_t t = 0; t < kDynamicTargetCount; ++t) links.multiplyFloor[t] = 0.9f;
    const DynamicResult after = evaluateLinks(links, in);

    bool anyMoved = false;
    for (size_t t = 0; t < kDynamicTargetCount; ++t)
      if (before.value[t] != after.value[t]) anyMoved = true;
    check(!anyMoved,
          "floor/unused: setting every `multiplyFloor` slot to 0.9 -- Size's own included -- "
          "changes NONE of `evaluateLinks()`'s twelve resolved values, bit for bit. The floor "
          "is applied only at the specific radius-consuming call sites this file's other "
          "sections exercise, never inside link resolution itself");

    check(after.at(DynamicTarget::Angle) == before.at(DynamicTarget::Angle) &&
              after.at(DynamicTarget::Scatter) == before.at(DynamicTarget::Scatter) &&
              after.at(DynamicTarget::Hue) == before.at(DynamicTarget::Hue),
          "floor/unused: named directly for the three Add targets -- Angle, Scatter, Hue -- "
          "matching `BrushLinkSet::multiplyFloor`'s own comment on why a floor is not even a "
          "coherent concept for an offset the way it is for a scale");
  }

  // ==========================================================================
  // 6. Persistence: `multiplyFloor[Size]` round-trips through
  //    app/UserBrushLibrary's `user-presets.txt`
  // ==========================================================================
  //
  // `BrushLinkSet::multiplyFloor` did not exist when that file's `link`/
  // `point` keys were designed, and its own §1 rule is explicit: new scalar
  // data arrives as a new key, never folded into an existing one. Skipping
  // this would mean an imported brush with a Minimum Diameter loses it the
  // moment a user hits Save -- the floor would still be right in memory
  // (`applyPresetToBrush()` copies the whole `BrushLinkSet` by value) but
  // gone after the next load, silently, with no note anywhere.
  {
    UserBrushLibraryStore store;
    BrushLibrary lib;
    BrushPreset preset;
    preset.name = "Floored Preset";
    preset.radius = 42.0f;
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    addLink(preset.links, l);
    preset.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.33f;
    lib.presets.push_back(preset);

    const std::string text = store.serialize(lib);
    check(text.find("floor 0 0.33") != std::string::npos,
          "floor/persist: the serialised file names the target by ITS ORDINAL (0 == Size), "
          "the same convention `link` already uses -- a display name would break the moment a "
          "tooltip's wording changed");

    BrushLibrary reloaded;
    UserBrushLibraryStore reader;
    reader.parse(text, reloaded);
    check(reloaded.presets.size() == 1 &&
              nearf(reloaded.presets[0].links.multiplyFloor[static_cast<size_t>(
                        DynamicTarget::Size)],
                    0.33f, 1e-6f),
          "floor/persist: round-trips back to 0.33 exactly (f9()'s own nine-significant-digit "
          "width is binary32's documented round-trip precision, so 1e-6 is generous rather "
          "than load-bearing here)");

    // A preset with NO floor writes no `floor` line at all -- the "costs
    // nothing" half of `multiplyFloor`'s own default, restated for the file
    // format: a preset saved before this key existed reserialises
    // byte-for-byte the same as it always did.
    BrushLibrary plainLib;
    BrushPreset plain;
    plain.name = "Plain Preset";
    addLink(plain.links, l);
    plainLib.presets.push_back(plain);
    const std::string plainText = store.serialize(plainLib);
    check(plainText.find("floor") == std::string::npos,
          "floor/persist: a preset with the default (no) floor emits no `floor` line at all");

    // Forward compatibility: an out-of-range target ordinal is preserved
    // verbatim rather than dropped -- §2's rule for `link`, restated for
    // `floor`. `kDynamicTargetCount` itself is always out of range for the
    // CURRENT build, by definition, and stays that way as new targets are
    // appended (they are appended, never inserted -- brush/Dynamics.hpp's
    // own ordinal-freezing rule).
    const std::string futureFloor =
        std::string(kUserPresetsFileHeader) + " 1\npreset Future\nscalars 1 1 1 1 1 1 1\nfloor " +
        std::to_string(static_cast<int>(kDynamicTargetCount)) + " 0.5\n";
    BrushLibrary futureLib;
    UserBrushLibraryStore futureReader;
    futureReader.parse(futureFloor, futureLib);
    check(futureLib.presets.size() == 1 &&
              futureLib.presets[0].links.multiplyFloor[0] == 0.0f,
          "floor/persist: an out-of-range target ordinal is not applied to any in-range slot "
          "this build has");
    const auto it = futureReader.presetUnknownLines().find("Future");
    check(it != futureReader.presetUnknownLines().end() && it->second.size() == 1 &&
              it->second[0].rfind("floor ", 0) == 0,
          "floor/persist: ...and is preserved verbatim, scoped to its own preset, exactly like "
          "an out-of-range `link` -- a future build's floor on a target this one cannot "
          "evaluate is correct data, not corruption");
  }

  // ==========================================================================
  // 7. `linkSetsEqual()`/`presetMatches()` see a floor difference too
  // ==========================================================================
  //
  // `brush/Library.cpp`'s `linkSetsEqual()` is the BRUSH EDITOR's "has the
  // live brush drifted from the preset it was loaded from" check
  // (`presetMatches()`, `brushIsEdited()`). It walks `BrushLinkSet::links`
  // cell by cell, which cannot see `multiplyFloor` at all -- a per-target
  // field, not a per-link one -- so it was extended to compare that array
  // directly. Nothing in this build's UI can currently DRIVE that
  // difference (no control edits `multiplyFloor` live; only an import or a
  // file load ever sets it, and both go through a whole-`BrushLinkSet` copy
  // that keeps the two in step) -- which is exactly why this needed its own
  // assertion rather than being caught by an existing one: the gap was
  // dormant, not exercised by any UI-driven test already in the suite.
  {
    BrushLinkSet a;
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    addLink(a, l);
    BrushLinkSet b = a;  // identical links, identical (default, zero) floor
    check(linkSetsEqual(a, b), "floor/equal: two sets with identical links and no floor compare equal");

    b.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.4f;
    check(!linkSetsEqual(a, b),
          "floor/equal: ...and stop comparing equal the moment ONLY their Size floor differs -- "
          "every `BrushLink` in both sets is still identical, so a walk that only compared "
          "`links` would miss this");

    BrushLinkSet c = a;
    c.multiplyFloor[static_cast<size_t>(DynamicTarget::Angle)] = 0.4f;  // an Add-target slot --
                                                                        // unused by anything
                                                                        // downstream, but still
                                                                        // part of the SET's own
                                                                        // identity
    check(!linkSetsEqual(a, c),
          "floor/equal: ...and the same for a slot nothing ever reads (Angle) -- 'unused' means "
          "no consumer applies it, not that two otherwise-identical sets are the same brush");
  }

  return ok;
}

}  // namespace np
