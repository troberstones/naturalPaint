#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "ops/FloodFill.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The paint bucket's refusals (app/StrokeSession section 6), and the one
// question the Layer Properties dialog's un-dimming depends on.
//
// **This section exists because of a silence, not a miscalculation.**
// ops/FloodFill was correct; every arithmetic assertion about it in
// runFloodFillTest() passed before this step and passes after it. What was
// wrong was one line of wiring in `ui/MacPaintUI.cpp`: the "can this layer take
// a fill" test was a local bool spelled inline, and it sat *inside* the click
// condition --
//
//     if (hovered && IsMouseClicked(Left) && usable && inBounds) { ...fill... }
//
// -- so a bucket click on any layer that was not an unlocked RGB layer
// evaluated to false and disappeared. No fill, no history entry, no mark on the
// canvas, and **no message anywhere in the chrome**. `CONTEXT.md` makes Pigment
// "the default kind for a new layer" and it is the first entry in the LAYERS
// panel's own NEW popup, so the ordinary way to reach this was to add a layer
// and click. The user's report was "paint bucket doesn't do anything", and that
// was exactly and literally true.
//
// The brush had had a refusal for this case since the RGB deposit route landed
// -- app/StrokeSession §1's whole last paragraph is about the invisible
// wrong-target failure, and `strokeRouteFor()` plus the options bar's route
// indicator are the fix. The bucket and the gradient had never been given the
// equivalent, and they sit behind the identical guard in the identical block.
// Section D is the assertion that would have caught it, and it is worth reading
// before the others: it fails on a build where the refusal has no words.
//
// **Section G is the Layer Properties dialog's half**, and it is here rather
// than in its own file because it is one question with one answer: does the
// canvas re-composite while the op stack is being edited? Suppressing that
// dialog's modal dim so the user can watch their adjustment take effect is
// worth nothing if what they are watching is a stale picture, so the claim is
// asserted rather than assumed. Everything else about that change --
// `ImGuiCol_ModalWindowDimBg` pushed around one `BeginPopupModal()` -- is ImGui
// state inside a window this suite has no window for, and is stated as
// untestable rather than given a test that asserts something else.
// ---------------------------------------------------------------------------
bool runBucketRefusalTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // --- Tolerances ---------------------------------------------------------
  //
  // **kHalfRel / kHalfFloor** -- binary16 storage, which is what a `core::Tile`
  // texel is. An 11-bit significand gives a round-to-nearest relative error of
  // at most 2^-11 = 4.883e-04 for a normal value, plus an absolute floor of
  // half a subnormal ulp, 2^-25 = 2.980e-08. Identical to the derivation
  // runRgbDepositTest() and runPigmentDepositTest() each state for the
  // identical storage, restated here rather than shared because a tolerance
  // borrowed without its derivation is the one that later gets applied where it
  // does not hold.
  //
  // Everything else below is at **exactly zero** tolerance, and each says why
  // in place. They are claims about which texels were written *at all* -- a
  // refused fill must move nothing, a fill outside the selection must not
  // happen -- rather than claims about accuracy, and a tolerance on such a
  // claim would let a small wrong write pass as rounding.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };

  // A saturated straight linear colour with alpha 1, which is the shape
  // `ui/MacPaintUI`'s `foregroundLinearRgba()` returns (that function's own
  // decode is asserted against the palette in runRgbDepositTest(); repeating it
  // here would test the decode twice and the bucket once). Distinct in all
  // three channels so a channel swap in `fillThroughSelection()` could not pass
  // as a grey.
  const std::array<float, 4> kInk{0.25f, 0.50f, 0.75f, 1.0f};

  // The whole document is one tile at 128x128 (kTileSize is 128), which is
  // deliberate for section E: an "outside the selection" texel then lives in
  // the *same allocated tile* as the filled ones, so the assertion is about the
  // coverage gate rather than about a tile that happens not to exist.
  constexpr int32_t kW = 128;
  constexpr int32_t kH = 128;

  // The bucket's own sequence, exactly as `ui/MacPaintUI.cpp`'s canvas block
  // runs it: flood from the seed, intersect the active selection when there is
  // one, fill, and record an edit only when texels actually moved. Written once
  // here so every section below exercises the same order the UI does rather
  // than each inventing its own.
  auto bucketClick = [&](OpenDocument& od, int32_t sx, int32_t sy,
                         const std::array<float, 4>& ink) -> size_t {
    Layer* target = activeLayerOf(od);
    if (!pixelOpWritesLayer(target)) return 0;
    FloodFillParams params;
    Selection region = floodFillSelection(*target->rgbTiles, PixelCoord{sx, sy}, od.document.width,
                                          od.document.height, params);
    if (od.selection.has_value())
      region = combineSelections(region, *od.selection, SelectionCombine::Intersect);
    const size_t changed = fillThroughSelection(*target->rgbTiles, region, ink);
    if (changed > 0) od.recordEdit("paint bucket", EditKind::Content);
    return changed;
  };

  std::printf("  -- A. a bucket click on a writable RGB layer CHANGES texels --\n");

  // `Document::createBlank()` makes layer 0 an RGB layer with an allocated but
  // empty store -- the layer an ordinary File > New selects, and therefore the
  // one case that always worked. Asserted anyway, and first: every refusal
  // below is only interesting against a demonstrated success, and a fix that
  // silenced the bucket everywhere would otherwise pass sections B through D.
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "bucket");
    const uint64_t revisionBefore = od.revision;
    const size_t changed = bucketClick(od, 64, 64, kInk);

    // A blank layer's texels are all {0,0,0,0}, so the seed matches every one
    // of them at distance 0 and the flood covers the whole document. That is
    // ops/FloodFill's stated ordinary case ("a fill seeded on blank canvas
    // floods across space no tile exists for"), and it is what makes the count
    // the document's whole area rather than something smaller.
    check(changed == static_cast<size_t>(kW) * static_cast<size_t>(kH),
          "fill: every texel of a blank RGB layer moves -- a bucket on a fresh document "
          "fills the document, not a region of it");

    // The **stored** value, not the count. Alpha is 1 and storage is
    // premultiplied, so the stored RGB is the ink's RGB unchanged; a straight
    // store, or a double premultiply, would show here and nowhere else.
    const std::array<float, 4> got = readAt(*od.document.layers[0].rgbTiles, 64, 64);
    check(nearHalf(got[0], kInk[0]) && nearHalf(got[1], kInk[1]) && nearHalf(got[2], kInk[2]) &&
              nearHalf(got[3], 1.0f),
          "fill: the stored texel is the ink, premultiplied -- a fill that wrote straight "
          "alpha or the wrong channel order stores a different colour, not a count");

    // A corner, because the flood is a traversal and a fill that only reached
    // the seed's neighbourhood would still pass a centre-texel assertion.
    const std::array<float, 4> corner = readAt(*od.document.layers[0].rgbTiles, kW - 1, kH - 1);
    check(nearHalf(corner[3], 1.0f),
          "fill: the far corner is filled too -- the flood reaches the whole blank layer "
          "rather than stopping near the seed");

    check(od.revision > revisionBefore && od.isDirty() && !od.unsavedEdits.empty() &&
              od.unsavedEdits.back() == "paint bucket",
          "edit: the fill records one edit named \"paint bucket\" and moves the revision -- "
          "which is what ui/DocumentTexture caches on, so an unrecorded fill is invisible");

    // The other half of `fillThroughSelection()`'s return contract, and the
    // reason `recordEdit()` is gated on it: filling with the colour that is
    // already there is not an edit, and an undo step that undoes nothing is a
    // worse defect than a missing one.
    const uint64_t revisionAfterFirst = od.revision;
    const size_t again = bucketClick(od, 64, 64, kInk);
    check(again == 0 && od.revision == revisionAfterFirst,
          "edit: re-filling with the colour already there moves no texel and records no "
          "second entry -- the history must not gain a step that undoes nothing");
  }

  std::printf("  -- B. a refused click changes NOTHING, and says which layer --\n");

  // The three refusals, each on a document whose *other* layer is a perfectly
  // fillable RGB layer -- so "nothing was written" is a claim about the refusal
  // and not about a document with nowhere to write at all.
  {
    struct Case {
      const char* what;
      Layer (*make)(std::string);
      bool lock;
      PixelOpRefusal want;
    };
    const Case cases[] = {
        // Pigment first, because it is the one a user reaches by accident: it
        // is `CONTEXT.md`'s default kind for a new layer and the first entry in
        // the NEW popup, so "add a layer, pick the bucket, click" lands here.
        {"pigment", &makePigmentLayer, false, PixelOpRefusal::NoRgbStore},
        {"adjustment", &makeAdjustmentLayer, false, PixelOpRefusal::NoRgbStore},
        {"locked RGB", &makeRgbLayer, true, PixelOpRefusal::Locked},
    };

    for (const Case& c : cases) {
      OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "bucket refusal");
      const size_t at = od.document.layers.size();
      recordLayerEdit(od, addLayer(od.document, at, c.make(std::string("Sky ") + c.what)));
      if (c.lock) recordLayerEdit(od, setLayerLocked(od.document, at, true));
      od.activeLayer = at;

      const Layer* target = activeLayerOf(od);
      const PixelOpRefusal reason = pixelOpRefusalFor(target);
      const std::string message = pixelOpRefusalMessage(reason, target, "paint bucket");

      std::printf("    %-12s -> %s\n", c.what, message.c_str());

      check(reason == c.want,
            "refusal: the reason is the specific one, not merely 'cannot' -- a single "
            "catch-all reason cannot produce two different sentences");

      // **Names the layer.** A refusal that says "this layer" is no use in a
      // stack of twelve: the LAYERS panel and the options bar are different
      // bands, and the user is looking at the canvas when the click fails.
      check(!message.empty() && contains(message, "Sky "),
            "refusal: the sentence NAMES the layer -- this is the assertion that fails "
            "against the shipped behaviour, which produced no sentence at all");

      // The whole point. The refusal must be a refusal, not a warning printed
      // beside a write that happened anyway.
      const uint64_t revisionBefore = od.revision;
      const size_t changed = bucketClick(od, 64, 64, kInk);
      const bool baseUntouched =
          readAt(*od.document.layers[0].rgbTiles, 64, 64) == std::array<float, 4>{0, 0, 0, 0};
      check(changed == 0 && od.revision == revisionBefore && baseUntouched,
            "refusal: not one texel moves, on the refused layer OR on the RGB layer under "
            "it -- exact zero, because 'a refused fill wrote a little' is not rounding");
    }
  }

  std::printf("  -- C. locked and no-RGB-store are told apart --\n");

  // Both present to a user as "the bucket did nothing", and **only one of them
  // has a switch in LAYERS that fixes it**. Telling someone to clear a lock
  // they never set is worse than telling them nothing, so the two sentences
  // have to differ -- and differ in the part that names the fix, not only in
  // the layer name they happen to carry.
  {
    Layer locked = makeRgbLayer("Sky");
    locked.locked = true;
    const Layer pigment = makePigmentLayer("Sky");

    const std::string lockedMsg =
        pixelOpRefusalMessage(pixelOpRefusalFor(&locked), &locked, "paint bucket");
    const std::string kindMsg =
        pixelOpRefusalMessage(pixelOpRefusalFor(&pigment), &pigment, "paint bucket");

    check(lockedMsg != kindMsg,
          "refusal: the two sentences differ even for two layers of the SAME name -- so the "
          "difference is the reason and not the name");
    check(contains(lockedMsg, "Lock") && !contains(kindMsg, "Lock"),
          "refusal: only the locked one mentions the Lock -- a user who can fix the lock is "
          "told about it, and a user who cannot is not sent to look for it");
    check(contains(kindMsg, "Pigment") && contains(kindMsg, "RGB"),
          "refusal: the kind refusal names the kind that failed AND the kind that works -- "
          "'no RGB pixels' alone leaves the user with no next move");

    // No layer at all is its own third answer, and `nullptr` is a legal
    // argument rather than a precondition violation: the canvas block reaches
    // this with a null target whenever every document has been closed.
    const std::string noneMsg =
        pixelOpRefusalMessage(pixelOpRefusalFor(nullptr), nullptr, "paint bucket");
    check(pixelOpRefusalFor(nullptr) == PixelOpRefusal::NoLayer && !noneMsg.empty() &&
              !contains(noneMsg, "\"\""),
          "refusal: no layer at all answers NoLayer and says so without quoting an empty "
          "name -- a sentence reading '\"\" is ...' reads as a bug, not as an explanation");

    // The op's own noun travels into the sentence, so a gradient's refusal is
    // not a bucket's with a different tool selected. Same wording, same voice,
    // different op -- which is what stops the two tools needing two message
    // tables that can drift apart.
    const std::string gradientMsg =
        pixelOpRefusalMessage(pixelOpRefusalFor(&pigment), &pigment, "gradient");
    check(contains(gradientMsg, "gradient") && !contains(gradientMsg, "paint bucket"),
          "refusal: the sentence names the op that was refused -- one message table, two "
          "tools, and neither wearing the other's name");
  }

  std::printf("  -- D. the shape of the defect: the gate, and the indicator --\n");

  {
    const Layer rgb = makeRgbLayer("Sky");
    const Layer pigment = makePigmentLayer("Sky");

    // The gate itself. A build where `pixelOpRefusalFor()` answers `None` for a
    // Pigment layer is the shipped behaviour restored -- the click is admitted,
    // `*target->rgbTiles` is dereferenced on an empty optional, and there is
    // nothing to say because nothing was refused.
    check(pixelOpWritesLayer(&rgb) && !pixelOpWritesLayer(&pigment),
          "gate: an RGB layer is writable and a Pigment layer is not -- the predicate the "
          "canvas block, the refusal sentence and the options bar all read");

    // **The lying indicator**, which is the second half of the same defect and
    // the one nothing on the canvas could have revealed. `strokeRouteFor()`
    // answers `None` for the bucket on *every* layer, because the bucket begins
    // no stroke and that table must go on refusing it -- so the options bar,
    // which is the one place in the chrome that answers "what will this tool do
    // to this layer", read a grey "-> none" while sitting over a layer the
    // bucket was about to fill. Two tables, and the band has to read the right
    // one.
    check(strokeRouteFor(Tool::PaintBucket, &rgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::Gradient, &rgb) == StrokeRoute::None,
          "indicator: the STROKE table still answers None for both fill tools -- they begin "
          "no stroke, and StrokeSession::begin() must go on refusing them");
    check(pixelOpWritesLayer(&rgb),
          "indicator: the FILL predicate answers writable for the same layer at the same "
          "moment -- so a band that reads only the stroke table calls a live bucket dead");

    // Which tools the second table covers, exhaustively enough to catch a third
    // fill tool added to one call site and not the other.
    check(toolWritesRgbPixels(Tool::PaintBucket) && toolWritesRgbPixels(Tool::Gradient),
          "indicator: both fill tools are covered -- fixing the bucket and not the gradient "
          "is half a fix, and they sit in the same palette group behind the same guard");
    check(!toolWritesRgbPixels(Tool::Brush) && !toolWritesRgbPixels(Tool::Water) &&
              !toolWritesRgbPixels(Tool::MagicWand) && !toolWritesRgbPixels(Tool::Eyedropper) &&
              !toolWritesRgbPixels(Tool::Hand),
          "indicator: no stroke or selection tool is claimed by the fill predicate -- it "
          "must not divert the brush away from the route table that owns it");
  }

  std::printf("  -- E. the active selection still bounds the fill --\n");

  // PRD **E1** (P0): "every deposit and every op respects the active
  // selection". The bucket is the op most able to break it, because its own
  // region is a flood that ignores the selection by design (ops/FloodFill §4
  // refuses to restrict itself, and says why) -- so the bound is entirely the
  // caller's, and this is the assertion that it was applied.
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "bucket selection");
    // Integer edges, so the rectangle's antialiased ramp lands exactly on texel
    // boundaries and every texel is fully in or fully out. A fractional edge
    // would make the outside-texel assertion a claim about the ramp instead of
    // a claim about the bound.
    od.selection = selectRectangle(32.0f, 32.0f, 96.0f, 96.0f);

    const size_t changed = bucketClick(od, 64, 64, kInk);
    const TileStore& store = *od.document.layers[0].rgbTiles;

    check(changed == 64u * 64u,
          "selection: exactly the selected texels move -- the flood found the whole blank "
          "layer, and the intersection is what cut it down to the rectangle");
    check(nearHalf(readAt(store, 64, 64)[3], 1.0f) && nearHalf(readAt(store, 32, 32)[3], 1.0f) &&
              nearHalf(readAt(store, 95, 95)[3], 1.0f),
          "selection: the centre and both far corners INSIDE the rectangle are filled -- so "
          "the intersection narrowed the region rather than emptying it");

    // **Exactly zero, all four channels.** The whole document is one tile, so
    // (10,10) sits in the same allocated tile as the filled texels -- this
    // cannot pass by the tile merely not existing, which is what would make a
    // sparse-store assertion vacuous. Zero rather than a tolerance because the
    // claim is that the texel was never written at all: a coverage of exactly 0
    // makes `fillThroughSelection()` `continue` before it reads or writes.
    const std::array<float, 4> zero{0.0f, 0.0f, 0.0f, 0.0f};
    check(store.find(tileCoordAt(PixelCoord{10, 10})) != nullptr &&
              readAt(store, 10, 10) == zero && readAt(store, 120, 120) == zero &&
              readAt(store, 31, 64) == zero && readAt(store, 96, 64) == zero,
          "selection: every probed texel OUTSIDE the rectangle is exactly {0,0,0,0} in an "
          "ALLOCATED tile -- including the two one texel past each edge");
  }

  std::printf("  -- F. the gradient is refused by the same predicate --\n");

  // The gradient shares the block, the guard and now the refusal. It is checked
  // through the predicate rather than by rendering a ramp because the defect
  // was never in ops/Gradient -- runGradientTest() already covers that -- it
  // was in the gate, and the gate is what has to be shown to cover both.
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "gradient refusal");
    const size_t at = od.document.layers.size();
    recordLayerEdit(od, addLayer(od.document, at, makePigmentLayer("Sky")));
    od.activeLayer = at;

    const Layer* target = activeLayerOf(od);
    check(!pixelOpWritesLayer(target),
          "gradient: the same predicate refuses the same layer -- one gate for both fill "
          "tools, so neither can be fixed without the other");
    check(contains(pixelOpRefusalMessage(pixelOpRefusalFor(target), target, "gradient"), "Sky"),
          "gradient: and produces a sentence naming the layer, so a refused DRAG is not a "
          "silently wasted gesture either");
  }

  std::printf("  -- G. the canvas re-composites while the op stack is edited --\n");

  // **The Layer Properties question.** That dialog stays modal and stops
  // dimming the canvas, so the user can watch an adjustment take effect while
  // they edit it. That is worth nothing unless the picture behind the dialog is
  // actually being rebuilt on every change, so the claim is asserted here on
  // the two mechanisms it rests on: the revision the texture caches on, and the
  // dirty-tile classification that decides what gets recomposited.
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "live composite");
    // Real content underneath, because an Exposure op scales -- over a
    // transparent black canvas every stops value composites to the same all-zero
    // picture and the assertion below would pass against a dialog that changed
    // nothing at all.
    bucketClick(od, 64, 64, kInk);

    const size_t adj = od.document.layers.size();
    recordLayerEdit(od, addLayer(od.document, adj, makeAdjustmentLayer("Exposure")));
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = 0.0f;
    recordLayerEdit(od, addLayerOp(od.document, adj, op));

    const Document before = od.document;
    const std::vector<uint16_t> pictureBefore = compositeDocumentStraightHalf(before);
    const uint64_t revisionBefore = od.revision;

    // One drag of the dialog's Exposure slider, through the same
    // `recordLayerEdit()` funnel the dialog's `run()` lambda uses.
    op.exposure.stops = 1.0f;
    const DocumentOpResult edit = recordLayerEdit(od, setLayerOp(od.document, adj, 0, op));
    check(edit.ok, "live: editing an op through the dialog's own funnel is accepted");

    check(od.revision > revisionBefore,
          "live: the op edit moves OpenDocument::revision -- that counter is the whole of "
          "ui/DocumentTexture's cache key, so an edit that did not move it shows nothing");

    const DocumentDirtyTiles dirty = documentDirtyTiles(before, od.document);
    check(dirty.everything && dirty.reason == FullRecompositeReason::LayerOpsChanged,
          "live: the change classifies as NOT tile-local, for the op-stack reason -- an op "
          "moves no tile, so a tile-only diff would find an empty dirty set and upload none");

    const std::vector<uint16_t> pictureAfter = compositeDocumentStraightHalf(od.document);
    check(pictureBefore.size() == pictureAfter.size() && !pictureBefore.empty() &&
              pictureBefore != pictureAfter,
          "live: and the recomposited picture really differs -- the two halves are compared "
          "bit for bit, so 'it recomposited' cannot pass on an unchanged image");

    // The op stack is the *whole content* of an Adjustment layer, so disabling
    // an entry has to be as visible as retyping one. This is the checkbox
    // beside each row in the dialog's Ops tree, and it is the control most
    // likely to be special-cased into invisibility.
    const Document beforeDisable = od.document;
    op.enabled = false;
    recordLayerEdit(od, setLayerOp(od.document, adj, 0, op));
    const DocumentDirtyTiles dirty2 = documentDirtyTiles(beforeDisable, od.document);
    check(dirty2.everything && dirty2.reason == FullRecompositeReason::LayerOpsChanged &&
              compositeDocumentStraightHalf(od.document) == pictureBefore,
          "live: disabling the op recomposites too, and lands back on the bit-identical "
          "picture it started from -- the Ops checkbox is as live as the slider");
  }

  // The section verdict every other section prints. Without it this file's
  // assertions are still counted -- `ok` reaches main.cpp's chain either way --
  // but a reader scanning the run for section names cannot see that it ran at
  // all, which is how a section that silently stopped being called gets missed.
  std::printf("[selftest] bucket refusal %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
