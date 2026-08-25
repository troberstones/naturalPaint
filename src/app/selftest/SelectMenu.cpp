#include "app/selftest/Support.hpp"

#include <array>
#include <cstdint>
#include <optional>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "color/Space.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "ops/Feather.hpp"
#include "ops/FloodFill.hpp"
#include "ui/MacPaintUI.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

float coverageAt(const Selection& s, int32_t x, int32_t y) {
  return selectionCoverageAt(&s, PixelCoord{x, y});
}

// Exact texel-for-texel comparison over one rectangle. Used to prove two
// Selections are the SAME selection, not merely similar in size -- the
// difference between "the wiring passed the right radius" and "the wiring
// passed A radius".
bool identicalOver(const Selection& a, const Selection& b, int32_t x0, int32_t y0, int32_t x1,
                   int32_t y1) {
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      if (coverageAt(a, x, y) != coverageAt(b, x, y)) return false;
    }
  }
  return true;
}

}  // namespace

// app/selftest/SelectMenu -- docs/reachability-audit.md C5, PRD E4/E8/E9.
//
// Five engines -- grow and shrink (a distance transform), feather (a blur of
// coverage), colour range and luminance range (whole-layer predicate passes)
// -- were implemented and proven correct with **no caller outside
// app/selftest/SelectionRefine.cpp and app/selftest/Blur.cpp**, because there
// was no Select menu to reach them from. `track7/selectmenu` built that menu,
// its five dialogs, and a dedicated one-entry-per-op undo
// (`OpenDocument::refineUndoStack`); this section is what proves the wiring
// rather than the arithmetic.
//
// **This is not core/SelectionRefine's suite again, and it does not restate
// what that section already proves.** That section is the authority on
// whether growSelection() is Euclidean, whether feather preserves
// antialiasing, and whether colour range agrees with a Global flood fill --
// none of that is repeated here. What THAT section cannot see, because it
// never calls anything above core/, is whether the menu that was built on
// top of it actually reaches it: whether `MenuAction::SelectGrow` calls
// growSelection() and not shrinkSelection(), whether the radius a dialog's
// slider holds is the radius the engine receives or a hardcoded default that
// happened to compile, and whether an operation that only changes a
// Selection -- never a pixel -- leaves the user anywhere to undo it. Every
// assertion below is chosen to fail on exactly one of those wiring mistakes
// and to stay green no matter how the underlying engine's numbers move.
//
// **Why this can run with no window at all.** The ImGui popups
// (ui/MacPaintUI.cpp's drawSelectMenuDialogs()) cannot run headless -- there
// is nothing for `ImGui::BeginPopupModal()` to draw into. But every popup's
// confirm button does exactly one thing: it hands the dialog's own held
// values to one of six small functions declared in ui/MacPaintUI.hpp
// (applySelectRefineAction(), applySelectColourRangeAction(),
// applySelectLuminanceRangeAction(), installRefinedSelection(),
// undoLastRefine(), and the three enable predicates). Those six ARE the
// dialog-to-engine boundary, factored out for exactly this reason, and they
// are plain functions of their arguments -- no ImGui, no window, no
// AppState beyond what a test can build by hand.
bool runSelectMenuTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] select menu\n");

  // ==========================================================================
  // A. The dialog -> engine boundary for grow/shrink/feather: which action
  //    reaches which engine function, with the dialog's OWN radius.
  // ==========================================================================
  {
    const Selection rect = selectRectangle(10.0f, 10.0f, 30.0f, 30.0f);

    // Deliberately not a round number and not RefineRadiusDialog's own
    // default (4.0f, ui/MacPaintUI.cpp) -- so a wiring mistake that silently
    // substituted the struct's default for the slider's live value cannot
    // pass by coincidence.
    const float dialogRadius = 6.25f;

    const Selection grown = applySelectRefineAction(MenuAction::SelectGrow, rect, dialogRadius);
    const Selection shrunk = applySelectRefineAction(MenuAction::SelectShrink, rect, dialogRadius);
    const Selection feathered =
        applySelectRefineAction(MenuAction::SelectFeather, rect, dialogRadius);

    check(identicalOver(grown, growSelection(rect, dialogRadius), 0, 0, 40, 40),
          "wiring: SelectGrow reaches growSelection() with the dialog's own radius -- bit for "
          "bit against calling the engine directly");
    check(identicalOver(shrunk, shrinkSelection(rect, dialogRadius), 0, 0, 40, 40),
          "wiring: SelectShrink reaches shrinkSelection(), not growSelection(), with the same "
          "radius");
    check(identicalOver(feathered, featherSelection(rect, dialogRadius), 0, 0, 40, 40),
          "wiring: SelectFeather reaches featherSelection() with the dialog's own radius");

    // The assertion that fails if a later edit ever wired two menu items to
    // one operation: grow and shrink move the boundary in OPPOSITE
    // directions, so for the same nonzero radius they cannot agree anywhere
    // near the edge.
    check(!identicalOver(grown, shrunk, 5, 5, 35, 35),
          "wiring: SelectGrow and SelectShrink do NOT produce the same selection for the same "
          "radius -- the assertion that reddens if two menu items were wired to one op");

    // The assertion that fails if a later edit ever dropped the dialog's
    // radius on the way to the engine, substituting a fixed value: growing by
    // the dialog's 6.25 must differ from growing by the dialog struct's OWN
    // default of 4.0.
    const Selection grownAtStructDefault = growSelection(rect, 4.0f);
    check(!identicalOver(grown, grownAtStructDefault, 0, 0, 40, 40),
          "wiring: the radius reaching growSelection() is the dialog's 6.25, not "
          "RefineRadiusDialog's own struct default of 4.0 -- catches a radius dropped on the "
          "way to the engine");
  }

  // ==========================================================================
  // B. The coverage convention at the engaged region's edge (core/
  //    SelectionMask.hpp): a null TILE inside an engaged Selection means
  //    coverage 0.0 -- the opposite of a null Selection*, which means 1.0,
  //    "no restriction". Grow has to read the sparse store's absence as the
  //    FIRST meaning, never the second, when it crosses into unallocated
  //    tiles.
  // ==========================================================================
  {
    // Exactly tile (0,0): selectRectangle only allocates tiles that hold
    // nonzero coverage (core/SelectionMask.hpp's drop rule), so tile (1,0) --
    // the one this grow has to cross into -- is never touched by the
    // constructor at all. It does not exist in the sparse store; it is not a
    // zeroed tile sitting there waiting to be read.
    const Selection edge = selectRectangle(0.0f, 0.0f, 128.0f, 128.0f);
    check(edge.tiles.occupiedTileCount() == 1,
          "edge: a 128x128 rectangle at the origin occupies exactly ONE tile (kTileSize == "
          "128) -- the neighbour this block grows into is genuinely absent, not present-and-empty");
    check(coverageAt(edge, 200, 5) == 0.0f,
          "edge: before growing, the unallocated neighbouring tile reads as UNSELECTED -- the "
          "null-tile-means-zero rule, not the opposite rule a null Selection* uses");

    const Selection grownAcrossTileEdge =
        applySelectRefineAction(MenuAction::SelectGrow, edge, 10.0f);
    // x=132 is 4 texels past the tile boundary at x=128, well inside a
    // radius-10 grow and far from where coverage would still be fractional
    // (that transition sits within ~1 texel of x=138); x=200 is 62 texels
    // past the boundary, nowhere near a radius-10 reach.
    check(coverageAt(grownAcrossTileEdge, 132, 5) > 0.9f,
          "edge: grow correctly treats the absent neighbouring tile as coverage 0.0 (an "
          "'outside' to grow INTO) and not as 1.0 (which the OTHER null convention -- a null "
          "Selection* meaning no restriction -- would give, making the whole plane already "
          "selected and grow() a no-op at this point)");
    check(coverageAt(grownAcrossTileEdge, 200, 5) == 0.0f,
          "edge: and the grow does not overshoot -- a point 62 texels past the boundary is "
          "still outside a radius-10 reach, so the wiring did not substitute 'no restriction' "
          "for the input either");
  }

  // ==========================================================================
  // C. Grow by N then shrink by N is NOT an identity in general -- core/
  //    SelectionRefine.hpp §2 says so directly: "a Minkowski open/close
  //    rounds off features narrower than the radius". Grow-then-shrink is
  //    erode(dilate(x)), i.e. a morphological CLOSING, and closing is
  //    EXTENSIVE (closing(X) superset-or-equal X, always) -- it can only ADD
  //    area relative to the original, by permanently bridging a gap narrower
  //    than the radius. So the loss this assertion demonstrates is a GAIN:
  //    a texel with zero coverage in the original, sitting in a gap the grow
  //    bridges, does not return to zero after the matching shrink.
  // ==========================================================================
  {
    // Two tall, narrow strips with a 1-texel gap between them, both far
    // taller than the radius so the round-trip's behaviour at the gap is
    // decided by the horizontal geometry alone -- the vertical ends of the
    // strips (+/-40) sit outside a radius-8 reach from the sampled point at
    // y = 0 and cannot interfere with it.
    const Selection stripA = selectRectangle(0.0f, -40.0f, 6.0f, 40.0f);
    const Selection stripB = selectRectangle(7.0f, -40.0f, 13.0f, 40.0f);
    const Selection twoStrips = combineSelections(stripA, stripB, SelectionCombine::Add);

    const float bridgeRadius = 8.0f;   // >> the 1-texel gap's half-width
    check(coverageAt(twoStrips, 6, 0) == 0.0f,
          "closing: the gap texel starts UNSELECTED -- neither strip covers x=6");

    const Selection grownStrips =
        applySelectRefineAction(MenuAction::SelectGrow, twoStrips, bridgeRadius);
    const Selection roundTrip =
        applySelectRefineAction(MenuAction::SelectShrink, grownStrips, bridgeRadius);

    check(coverageAt(grownStrips, 6, 0) > 0.99f,
          "closing: growing by 8 comfortably bridges a 1-texel gap -- the midpoint is fully "
          "selected before the shrink even runs");
    check(coverageAt(roundTrip, 6, 0) > 0.99f,
          "closing: grow-by-8 THEN shrink-by-8 does NOT restore the original gap -- "
          "core/SelectionRefine.hpp's own words, 'a Minkowski open/close rounds off features "
          "narrower than the radius': once bridged, a gap this narrow stays bridged, which is "
          "the loss the header names and this assertion is the one that would catch a "
          "regression toward assuming grow-then-shrink is an identity");
  }

  // ==========================================================================
  // D. Colour range / luminance range: the same dialog -> engine boundary,
  //    over a raw TileStore fixture (no Document needed for THIS half --
  //    core/SelectionRefine.hpp PRD E9: both take a TileStore, not a
  //    Selection).
  // ==========================================================================
  {
    // Three bands by column, matching app/selftest/SelectionRefine.cpp's own
    // fixture shape (a new instance, not shared code -- this section proves
    // wiring, that one proves the metric, and they should not depend on one
    // another's data to stay independently readable).
    // **The three band values are sRGB, and the store holds their linear
    // decode.** A `TileStore` texel is premultiplied LINEAR, but every
    // tolerance in this section is compared in sRGB-encoded space --
    // `floodFillDistance()` encodes each channel before subtracting, and
    // `selectionLuminanceOf()` encodes the luma scalar -- so the numbers that
    // have to be spaced correctly are the ENCODED ones. Writing the band
    // values straight in as linear is how this fixture used to read, and it
    // silently put the swatch nowhere near the band it claimed to name: sRGB
    // 0.45 decodes to linear 0.171, while the linear 0.45 band encodes back to
    // sRGB 0.70. Every band then sat 0.21 or more from the reference, past
    // BOTH tolerances below, so both selections came out empty and the two
    // assertions comparing them compared nothing.
    //
    // The 0.08 spacing is the point of the fixture and is chosen to sit
    // between the two tolerances it has to tell apart: wider than the dialog's
    // 0.05, so a correctly-forwarded tolerance selects the middle band ALONE,
    // and narrower than SelectionRangeParams{}'s kFloodDefaultTolerance
    // (~0.1255), so a dropped tolerance reaches both neighbours and the
    // difference is three whole bands rather than a fringe of texels. Margins
    // either side (0.03 and 0.045) are ~300x the half-float quantisation the
    // store round trip costs at these magnitudes (~1e-4 in sRGB), so nothing
    // here is riding a rounding edge.
    constexpr float kBandLoSrgb = 0.54f;
    constexpr float kBandMidSrgb = 0.62f;
    constexpr float kBandHiSrgb = 0.70f;
    TileStore src;
    for (int32_t y = 0; y < 60; ++y) {
      for (int32_t x = 0; x < 60; ++x) {
        const float s = x < 20 ? kBandLoSrgb : (x < 40 ? kBandMidSrgb : kBandHiSrgb);
        const float v = srgbDecode(s);
        const PixelCoord p{x, y};
        src.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {v, v, v, 1.0f});
      }
    }

    // The middle band, in sRGB -- the same number the fixture wrote, which is
    // the whole reason it names the band it says it does.
    const std::array<float, 3> swatchSrgb = {kBandMidSrgb, kBandMidSrgb, kBandMidSrgb};
    const float tolerance = 0.05f;   // tighter than kFloodDefaultTolerance (~0.1255)
    const float edgeBand = 0.01f;

    const Selection viaWiring =
        applySelectColourRangeAction(swatchSrgb, tolerance, edgeBand, src, 60, 60);

    const std::array<float, 4> linear = {srgbDecode(swatchSrgb[0]), srgbDecode(swatchSrgb[1]),
                                         srgbDecode(swatchSrgb[2]), 1.0f};
    SelectionRangeParams paramsDirect;
    paramsDirect.tolerance = tolerance;
    paramsDirect.edgeBand = edgeBand;
    const Selection viaDirect = selectColourRange(src, linear, 60, 60, paramsDirect);

    check(identicalOver(viaWiring, viaDirect, 0, 0, 60, 60),
          "wiring: Colour Range decodes the swatch sRGB->linear exactly once and forwards the "
          "dialog's own tolerance/edge band -- bit for bit against calling the engine "
          "directly");

    // Catches tolerance/edge band dropped on the way to the engine: a
    // tighter-than-default tolerance must select FEWER texels than
    // SelectionRangeParams{}'s own default (~0.1255), which at this fixture's
    // 0.08 band spacing reaches BOTH of the middle band's neighbours while
    // the dialog's 0.05 reaches neither. The difference is therefore two
    // whole 20-column bands, not a fringe.
    const Selection viaEngineDefault = selectColourRange(src, linear, 60, 60, {});
    check(!identicalOver(viaWiring, viaEngineDefault, 0, 0, 60, 60),
          "wiring: the tolerance/edge band reaching selectColourRange() are the dialog's own "
          "values, not SelectionRangeParams{}'s defaults -- catches those two being dropped "
          "on the way to the engine");

    // `selectionLuminanceOf()` is srgbEncode(luma), and for the neutral greys
    // this fixture holds luma IS the linear value, so each band's luminance is
    // exactly the sRGB number it was written as: 0.54 / 0.62 / 0.70. The high
    // endpoint is 0.58 rather than the 0.6 this read before because 0.6 would
    // land 0.02 from the middle band -- exactly `lumBand` -- and
    // `floodFillCoverage()` refuses at `!(distance < tolerance)`, so that band
    // would sit precisely on the knife edge between selected and not. At 0.58
    // the low band is inside with 0.04 to spare and both others are outside by
    // 2x and 6x the edge band.
    const float low = 0.3f, high = 0.58f, lumBand = 0.02f;
    const Selection viaWiringLum = applySelectLuminanceRangeAction(low, high, lumBand, src, 60, 60);
    SelectionLuminanceRange rangeDirect;
    rangeDirect.low = low;
    rangeDirect.high = high;
    rangeDirect.edgeBand = lumBand;
    const Selection viaDirectLum = selectLuminanceRange(src, 60, 60, rangeDirect);
    check(identicalOver(viaWiringLum, viaDirectLum, 0, 0, 60, 60),
          "wiring: Luminance Range forwards the dialog's own low/high/edge band -- bit for bit "
          "against calling the engine directly");

    // SelectionLuminanceRange{}'s default band is 0..1 -- everything opaque
    // and grey-ish in this fixture qualifies -- so a genuine 0.3..0.6 band
    // must select strictly less.
    const Selection viaEngineDefaultLum = selectLuminanceRange(src, 60, 60, {});
    check(!identicalOver(viaWiringLum, viaEngineDefaultLum, 0, 0, 60, 60),
          "wiring: the low/high/edge band reaching selectLuminanceRange() are the dialog's own "
          "values, not SelectionLuminanceRange{}'s 0..1 default -- catches those being dropped "
          "on the way to the engine");
  }

  // ==========================================================================
  // E. The enable predicates, as pure functions of constructed state -- no
  //    window, no dialog, no AppState beyond a hand-built OpenDocument.
  // ==========================================================================
  {
    OpenDocument fresh = makeBlankOpenDocument(64, 64, WorkingSpace{});
    check(!selectRefineEnabled(fresh),
          "enable: grow/shrink/feather start disabled -- a fresh document's selection is "
          "std::nullopt, and all three take a Selection&, never a Selection*, so there is no "
          "way to hand them 'no restriction'");
    check(selectRangeEnabled(fresh),
          "enable: colour/luminance range need only the RGB layer makeBlankOpenDocument() "
          "already allocates -- NO selection required at all, unlike the three above (PRD E9: "
          "they take a TileStore, not a Selection)");
    check(!selectUndoRefineEnabled(fresh),
          "enable: Undo Refine starts disabled -- refineUndoStack is empty until an operation "
          "pushes to it");

    fresh.selection = selectAll(64, 64);
    check(selectRefineEnabled(fresh),
          "enable: engaged once ANY selection exists, Select All included -- selectAll() IS "
          "engaged (core/SelectionMask.hpp distinguishes it from Deselect on exactly this "
          "point), unlike std::nullopt");

    // A document with no layer at all -- not even the RGB layer
    // makeBlankOpenDocument() gives one, because this is a default-
    // constructed OpenDocument with an empty layer list.
    const OpenDocument noLayerAtAll;
    check(!selectRangeEnabled(noLayerAtAll),
          "enable: colour/luminance range refuse a document with no active layer to sample");
  }

  // ==========================================================================
  // F. Undo: exactly one entry per operation, and an exact restore --
  //    including the case where what a refine replaced was 'no selection' at
  //    all, which is a real, restorable state and not the same as an
  //    empty-but-engaged one (core/SelectionMask.hpp).
  // ==========================================================================
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    od.selection = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);
    const Selection before = *od.selection;
    check(od.refineUndoStack.empty(), "undo: starts empty");

    installRefinedSelection(od, growSelection(before, 3.0f));
    check(od.refineUndoStack.size() == 1,
          "undo: one refine pushes EXACTLY one entry -- neither zero (unrecoverable) nor two "
          "(a double-push that would need two Undo Refines to reverse one Grow)");
    check(od.selection.has_value() && coverageAt(*od.selection, 8, 15) > coverageAt(before, 8, 15),
          "undo: the grown selection really did install (sanity check, not this block's point)");

    const bool undone = undoLastRefine(od);
    check(undone, "undo: Undo Refine reports success while the stack holds an entry");
    check(od.refineUndoStack.empty(), "undo: and consumes exactly the one entry it pushed");
    check(od.selection.has_value(), "undo: restores an ENGAGED selection, because the state "
          "before the grow was engaged");
    check(identicalOver(*od.selection, before, 5, 5, 25, 25),
          "undo: restores the previous mask EXACTLY, texel for texel -- not merely 'a smaller "
          "selection'");

    check(!undoLastRefine(od),
          "undo: a second Undo Refine on an empty stack reports failure rather than restoring "
          "something invented");

    // The nullopt case: colour/luminance range may run with NOTHING
    // selected (section E above), so the state a refine replaces can
    // legitimately be "no selection" -- and undo must restore exactly that
    // absence, not an empty-but-engaged selection, which core/SelectionMask.hpp
    // is explicit is a different state ("A selection that selects nothing" is
    // its own named case, distinct from "no selection at all").
    OpenDocument noSelectionYet = makeBlankOpenDocument(64, 64, WorkingSpace{});
    check(!noSelectionYet.selection.has_value(),
          "undo: sanity -- a fresh document has no selection, the OTHER absence "
          "core/SelectionMask.hpp names (opposite of an engaged-but-empty one)");
    installRefinedSelection(noSelectionYet, selectRectangle(0.0f, 0.0f, 10.0f, 10.0f));
    check(noSelectionYet.refineUndoStack.size() == 1 && noSelectionYet.selection.has_value(),
          "undo: a range op run with nothing selected still pushes one entry (holding "
          "std::nullopt) and installs its result");
    check(undoLastRefine(noSelectionYet) && !noSelectionYet.selection.has_value(),
          "undo: and undoing it restores 'no selection' EXACTLY -- not an empty-but-engaged "
          "selection, a different state this codebase is explicit about");
  }

  std::printf("[selftest] select menu %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
