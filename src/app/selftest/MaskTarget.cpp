#include "app/selftest/Support.hpp"

#include "app/LayerThumbnail.hpp"
#include "app/StrokeSession.hpp"
#include "brush/MaskPaint.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The mask as a paint target, the route that writes it, and the two thumbnails
// that make it visible -- `docs/testing-issues.md` T16.
//
// See app/SelfTest.hpp for the section's contents list, and the three headers
// (app/StrokeSession §1, brush/MaskPaint, app/LayerThumbnail) for every
// decision this file only checks. Four things are worth saying here because
// they are what the section is *for*:
//
//   * **Nothing could paint a mask.** `core/Mask.hpp` and `core/Layer.hpp` both
//     said so in the same words -- "the content of a mask can only come from a
//     `.npaint` or from a test writing texels" -- and `strokeRouteFor()` had no
//     row for one. A store, a compositor that reads it and a panel that says
//     `MASK` all existed; the route did not.
//   * **A target selector with no write route is a live control over nothing.**
//     That is the failure this whole section is arranged around, and this
//     codebase has shipped it (`app/StrokeSession.hpp` §6 records the paint
//     bucket discarding a click on a Pigment layer with "no message, no history
//     entry and no mark on the canvas"). So the assertions come in pairs: the
//     target resolves *to* something, and a stroke aimed at that something
//     *changes texels*.
//   * **The two thumbnails need two transfer functions, and the difference is
//     invisible at the endpoints.** `app/selftest/PresentTransfer.cpp` measured
//     what happens when a linear texture goes through Dear ImGui's pipeline:
//     zero error at black and at white, and byte 61 where 137 belonged in
//     between. Every assertion here is therefore about the MIDDLE of the range,
//     in both directions -- an encode that must be there and an encode that
//     must not.
//   * **A stale thumbnail is worse than no thumbnail.** A blank square says "I
//     do not know"; a stale one says something false with confidence. The cache
//     assertions check both halves of `core::SelectionBoundaryCache`'s own
//     rule: that the build count moves when the key moves, and that what comes
//     back afterwards is the new picture rather than a re-blessed old one.
//
// Headless and GPU-free: everything here is arithmetic over tile stores. The
// *pixels on screen* are the golden harness's job, because `--selftest` has no
// window and can never reach a draw call.
// ---------------------------------------------------------------------------
bool runMaskTargetTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("[selftest] mask target: T16's paint target, the mask stroke route, the layer "
              "and mask thumbnails, and the thumbnail cache's invalidation rule\n");

  // The mask channel's own f16 bound, derived rather than borrowed --
  // `app/selftest/LayerMask.cpp` carries the full derivation and it is the same
  // channel: a coverage lives in [0,1], binary16's spacing in [0.5,1) is 2^-11,
  // so round-to-nearest costs at most 2^-12 and every lower binade is finer.
  constexpr float kMaskAbs = 2.4414063e-04f;  // 2^-12

  // A hard disc, so `dabCoverage()` is exactly 1.0 over the whole core and every
  // number below is about the mask arithmetic rather than about the falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.opacity = 1.0f;
    return t;
  };
  auto maskAt = [](const MaskTileStore& store, int32_t x, int32_t y) {
    const PixelCoord at{x, y};
    const MaskTile* t = store.find(tileCoordAt(at));
    // Absent means REVEAL, 1.0 -- core/Mask.hpp, and the opposite reading from
    // an absent content tile. Written the right way round here on purpose: a
    // helper that returned 0 for an absent tile would make every assertion
    // below agree just as happily with an implementation that had the rule
    // inverted.
    return t == nullptr ? 1.0f : t->readCoverage(tileLocalOffset(at));
  };
  auto rgbAt = [](const TileStore& store, int32_t x, int32_t y) {
    const PixelCoord at{x, y};
    const Tile* t = store.find(tileCoordAt(at));
    return t == nullptr ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
                        : t->readPixel(tileLocalOffset(at));
  };
  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord at{x, y};
        store.getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at), premultiplied);
      }
  };

  // ======================================================================
  // 1. The target concept, and its unambiguous answer with no mask
  // ======================================================================
  //
  // `resolveLayerEditTarget()`. The row that matters is the third: a flag
  // saying "the mask" over a layer that has none must answer `Content`, because
  // the alternative is a `Mask` target with nothing behind it -- a lit chip, a
  // brush that leaves no mark, no message. That state is one gesture away
  // (select the mask on one row, click a different row), so it is not a corner
  // case.
  {
    Layer plain = makeRgbLayer("no mask");
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    addLayerMask(doc, 0);

    check(resolveLayerEditTarget(false, &plain) == LayerEditTarget::Content &&
              resolveLayerEditTarget(false, &doc.layers[0]) == LayerEditTarget::Content,
          "target: not requested -> content, mask present or not");
    check(resolveLayerEditTarget(true, &doc.layers[0]) == LayerEditTarget::Mask,
          "target: requested on a layer WITH a mask -> mask");
    check(resolveLayerEditTarget(true, &plain) == LayerEditTarget::Content,
          "target: requested on a layer with NO mask -> content, not a dead mask");
    check(resolveLayerEditTarget(true, nullptr) == LayerEditTarget::Content,
          "target: requested with no layer at all -> content");
    check(std::strcmp(layerEditTargetName(LayerEditTarget::Mask), "mask") == 0 &&
              std::strcmp(layerEditTargetName(LayerEditTarget::Content), "content") == 0,
          "target: both names are words rather than '?'");

    // **Across a layer switch**, which is the gesture the resolution exists
    // for. The FLAG survives -- deliberately, so clicking back restores the
    // user's choice rather than silently forgetting it -- and the resolved
    // answer follows the layer, which is what keeps the surviving flag honest.
    addLayer(doc, doc.layers.size(), makeRgbLayer("second, no mask"));
    const bool flag = true;  // set once, never cleared, exactly as the panel leaves it
    check(resolveLayerEditTarget(flag, &doc.layers[0]) == LayerEditTarget::Mask &&
              resolveLayerEditTarget(flag, &doc.layers[1]) == LayerEditTarget::Content &&
              resolveLayerEditTarget(flag, &doc.layers[0]) == LayerEditTarget::Mask,
          "target: one unchanged flag, two layers -- the answer follows the layer");
  }

  // ======================================================================
  // 2. The three-argument route table DELEGATES rather than duplicating
  // ======================================================================
  //
  // §1's whole purpose is that one question is answered in one place. A
  // three-argument copy of the rows would be the drift this file's own comments
  // describe -- "the options bar's route indicator read 'goes to the solver'
  // grey for a live RGB stroke for exactly as long as it had its own copy of
  // the test". Asserted over EVERY tool and four targets, so a row added to one
  // form and not the other fails here rather than diverging quietly.
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    Layer rgb = makeRgbLayer("rgb");
    Layer pigment = makePigmentLayer("pigment");
    bool allAgree = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool tool = static_cast<Tool>(i);
      const Layer* targets[4] = {nullptr, &rgb, &pigment, &doc.layers[0]};
      for (const Layer* t : targets)
        if (strokeRouteFor(tool, t, LayerEditTarget::Content) != strokeRouteFor(tool, t))
          allAgree = false;
    }
    check(allAgree, "route: the Content arm delegates -- identical over every tool x 4 targets");

    // The null-target rows, by name. **This is the assertion that makes the
    // dead-`if` cleanup stick.** `strokeRouteFor()` carried two consecutive
    // `if (target == nullptr)` blocks from a union merge; the second was
    // unreachable and its condition was a strict SUBSET of the first's, so
    // deleting the wrong one of the pair would have sent four tools to the
    // solver silently. Re-narrowing the surviving condition fails the second of
    // these two lines.
    check(strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim &&
              strokeRouteFor(Tool::DryBrush, nullptr) == StrokeRoute::PaintSim,
          "route: no target -- brush and dry brush reach the solver canvas");
    check(strokeRouteFor(Tool::Eraser, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Pencil, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Dodge, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Burn, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::CloneStamp, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Smudge, nullptr) == StrokeRoute::None,
          "route: no target -- all six non-paint families refuse, none reaches the solver");
  }

  // ======================================================================
  // 3. The mask rows, and the enum-exhaustive predicates after adding one
  // ======================================================================
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    addLayerMask(doc, 0);
    Layer& masked = doc.layers[0];

    check(strokeRouteFor(Tool::Brush, &masked, LayerEditTarget::Mask) == StrokeRoute::MaskPaint &&
              strokeRouteFor(Tool::DryBrush, &masked, LayerEditTarget::Mask) ==
                  StrokeRoute::MaskPaint,
          "route: brush and dry brush on a mask target -> mask-paint");
    bool othersRefuse = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool tool = static_cast<Tool>(i);
      if (tool == Tool::Brush || tool == Tool::DryBrush) continue;
      if (strokeRouteFor(tool, &masked, LayerEditTarget::Mask) != StrokeRoute::None)
        othersRefuse = false;
    }
    check(othersRefuse,
          "route: every other tool refuses a mask target, by name rather than by default");

    Document plainDoc = Document::createBlank(64, 64, WorkingSpace{});
    check(strokeRouteFor(Tool::Brush, &plainDoc.layers[0], LayerEditTarget::Mask) ==
              StrokeRoute::None,
          "route: a Mask target on a layer with no mask answers None, not a write");

    masked.locked = true;
    check(strokeRouteFor(Tool::Brush, &masked, LayerEditTarget::Mask) == StrokeRoute::None,
          "route: a locked layer refuses the mask too -- the lock covers the layer");
    masked.locked = false;
    // The disagreement with the line above is the decision: `alphaLocked`
    // freezes the layer's own alpha channel, and a mask is a separate store that
    // multiplies coverage at composite time. Refusing here would block the one
    // gesture the flag exists to permit.
    masked.alphaLocked = true;
    check(strokeRouteFor(Tool::Brush, &masked, LayerEditTarget::Mask) == StrokeRoute::MaskPaint,
          "route: alpha lock does NOT refuse a mask -- it freezes a different channel");
    masked.alphaLocked = false;

    // An **Adjustment** layer holds no pixels at all, so its content route is
    // `None` -- and `core/Layer.hpp` names painting into one's mask as PRD D13's
    // dodge and burn, the case that motivated the mask store being a third
    // member rather than living inside the other two. If this pair ever reads
    // the same, the feature that argument was made for is gone.
    Document adjDoc = Document::createBlank(64, 64, WorkingSpace{});
    addLayer(adjDoc, adjDoc.layers.size(), makeAdjustmentLayer("grade"));
    addLayerMask(adjDoc, 1);
    check(strokeRouteFor(Tool::Brush, &adjDoc.layers[1]) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &adjDoc.layers[1], LayerEditTarget::Mask) ==
                  StrokeRoute::MaskPaint,
          "route: an Adjustment layer takes no content stroke and DOES take a mask one");

    // The predicates that are enum-exhaustive by contract, re-asked rather than
    // inherited -- each of their headers warns that a route added without
    // answering their question leaves them correct and their call sites wrong.
    check(strokeRouteWritesLayer(StrokeRoute::MaskPaint),
          "predicates: mask-paint writes a layer (all four call sites want yes)");
    check(grainReachesRoute(StrokeRoute::MaskPaint),
          "predicates: PAPER GRAIN reaches mask-paint -- and the route makes the call");
    check(!wetnessReachesSolver(StrokeRoute::MaskPaint),
          "predicates: WET does not reach mask-paint -- it names the solver alone");
    check(std::strcmp(strokeRouteName(StrokeRoute::MaskPaint), "mask-paint") == 0,
          "predicates: the route has a name the options bar can print");
  }

  // ======================================================================
  // 4. The ink -> coverage conversion, asserted in the MIDDLE of the range
  // ======================================================================
  //
  // brush/MaskPaint §2. Black is 0 and white is 1 under every candidate
  // implementation, which is exactly why those two prove nothing; the mid-grey
  // number is the whole assertion. A missing encode would make a 50 % grey
  // swatch paint coverage 0.214 -- about twice as opaque as the swatch looks --
  // and every black-and-white test would still pass.
  {
    // Black is exact. **White is not, and the 1 ulp is not this module's to
    // fix.** `srgbEncode()`'s upper segment is the affine `1.055*x^(1/2.4) -
    // 0.055`, and in float `1.055f - 0.055f` is 0.99999994 rather than 1.0 --
    // a property of those two constants that every caller of that function
    // shares. Special-casing 1.0 here would put a second transfer function in
    // the codebase to disagree with the first over exactly one input, which is
    // the drift this section is otherwise entirely about. It costs nothing
    // where it lands: 0.99999994 rounds to binary16 1.0, the one encoding of
    // reveal-all that `MaskTile::kRevealWord` names, so a mask painted white is
    // bit-identical to one that was never touched -- asserted below rather than
    // argued.
    check(maskTargetForInk({0.0f, 0.0f, 0.0f}) == 0.0f,
          "ink: black hides completely, exactly 0");
    check(near(maskTargetForInk({1.0f, 1.0f, 1.0f}), 1.0f, 1.0e-6f),
          "ink: white reveals completely, to within srgbEncode()'s own 1 ulp");
    check(floatToHalf(maskTargetForInk({1.0f, 1.0f, 1.0f})) == MaskTile::kRevealWord,
          "ink: and STORES as the one word that means reveal-all");

    // The linear value a 50 % display grey really is, taken through the
    // codebase's own decode rather than typed in.
    const float halfGreyLinear = srgbDecode(0.5f);
    const float target = maskTargetForInk({halfGreyLinear, halfGreyLinear, halfGreyLinear});
    check(near(target, 0.5f, 1.0e-5f),
          "ink: a 50% display grey paints coverage 0.5 -- the encode is applied");
    check(!near(target, halfGreyLinear, 0.01f),
          "ink: and NOT its linear value 0.214 -- the failure no B/W test would show");
    std::printf("  [mask ink] 50%% display grey: linear %.4f -> coverage %.4f "
                "(un-encoded would have been %.4f)\n",
                static_cast<double>(halfGreyLinear), static_cast<double>(target),
                static_cast<double>(halfGreyLinear));

    // Rec.709 luma in linear light, then encode -- core/SelectionRefine.hpp's
    // order, argued there. Pure green is far brighter than pure blue, and a
    // conversion that weighted the three channels equally would say otherwise.
    const float green = maskTargetForInk({0.0f, 1.0f, 0.0f});
    const float blue = maskTargetForInk({0.0f, 0.0f, 1.0f});
    check(green > blue && green > 0.8f && blue < 0.4f,
          "ink: Rec.709 weights -- pure green is much lighter than pure blue");
    check(maskTargetForInk({std::nanf(""), 0.0f, 0.0f}) == 0.0f,
          "ink: a NaN channel clamps to 0 rather than poisoning the mask");
  }

  // ======================================================================
  // 5. The per-stroke ceiling, and the closed form
  // ======================================================================
  //
  // brush/MaskPaint §3. The wrong version -- a per-dab lerp factor with no
  // accumulator -- converges to the target for any positive weight, so it
  // passes any test that paints once and fails only for a user who scrubs. Both
  // halves are asserted: the value does not move after the first dab, and it
  // sits at the closed form rather than at the target.
  {
    MaskTileStore store;
    MaskPaintStroke st;
    st.begin(0.0f, 0.5f);  // paint black, at half opacity
    const BrushTip t = discTip(20.0f, 1.0f);
    st.paintDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    const float afterOne = maskAt(store, 128, 128);
    for (int k = 0; k < 12; ++k)
      st.paintDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    const float afterMany = maskAt(store, 128, 128);
    const float applied = st.strokeAppliedAt(PixelCoord{128, 128});

    check(afterOne == afterMany,
          "ceiling: twelve more dabs over the same texel change it BIT-identically");
    check(near(afterMany, 0.5f, kMaskAbs),
          "ceiling: a half-opacity black stroke reaches 0.5, not 0.0, however scrubbed");
    // The closed form: `v_end = v_start + A(target - v_start)`, with `v_start`
    // 1.0 because an absent mask tile reveals. The accumulator has not been
    // through binary16 and the stored value has, once per dab, which is what the
    // f16 bound is for.
    check(near(afterMany, 1.0f + applied * (0.0f - 1.0f), kMaskAbs),
          "ceiling: the stored coverage IS one lerp by the accumulator (closed form)");
    check(near(applied, 0.5f, 1.0e-6f),
          "ceiling: and the accumulator itself stopped exactly at the ceiling");

    // A second, separate stroke goes deeper -- each starts its accumulator at
    // zero and lerps into what the last one left, which is what every editor
    // does and what makes "50 %" a per-stroke quantity rather than a per-texel
    // one.
    MaskPaintStroke st2;
    st2.begin(0.0f, 0.5f);
    st2.paintDab(store, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    check(near(maskAt(store, 128, 128), 0.25f, kMaskAbs),
          "ceiling: a SECOND stroke halves again -- 0.5 then 0.25, not stuck at 0.5");

    std::printf("  [mask stroke] accumulator while live: %zu tiles, %zu bytes\n",
                st.accumulatorTiles(), st.accumulatorBytes());
    st.end();
    check(st.accumulatorTiles() == 0,
          "ceiling: the accumulator is freed at pen-up, not at the next pen-down");
  }

  // ======================================================================
  // 6. An absent mask tile is 1.0 -- the one place copying the eraser fails
  // ======================================================================
  //
  // brush/MaskPaint §5. `brush/RgbErase` skips an absent tile whole, correctly,
  // because a content tile that does not exist holds nothing to remove. A mask
  // tile that does not exist holds **1.0**, the most a mask texel can hold, and
  // `core::addLayerMask()` creates a store with **zero** tiles -- so a route
  // that inherited that skip would be a mask brush that did nothing at all on
  // every mask this application can create. It would look exactly like a
  // missing route, which is the defect it was built to close.
  {
    const BrushTip t = discTip(20.0f, 1.0f);

    MaskTileStore white;
    MaskPaintStroke reveal;
    reveal.begin(1.0f, 1.0f);  // paint white onto an untouched mask
    const DepositCount noop =
        reveal.paintDab(white, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    check(white.occupiedTileCount() == 0 && noop.texels == 0 && noop.tiles == 0,
          "absent: painting WHITE on a blank mask allocates nothing (already there)");

    MaskTileStore black;
    MaskPaintStroke hide;
    hide.begin(0.0f, 1.0f);
    const DepositCount real =
        hide.paintDab(black, t, Vec2{128.0f, 128.0f}, 256, 256, nullptr, nullptr);
    check(black.occupiedTileCount() > 0 && real.texels > 0 && real.tiles > 0,
          "absent: painting BLACK on a blank mask allocates and writes -- not skipped");
    check(maskAt(black, 128, 128) == 0.0f && maskAt(black, 10, 10) == 1.0f,
          "absent: the dab lands at 0 and the rest of the tile still reveals at 1");
  }

  // ======================================================================
  // 7. PRD E1: the selection bounds a mask stroke, twice
  // ======================================================================
  {
    MaskTileStore store;
    MaskPaintStroke st;
    st.begin(0.0f, 1.0f);
    const BrushTip t = discTip(24.0f, 1.0f);
    Selection sel = selectRectangle(64.0f, 0.0f, 200.0f, 256.0f);
    // Scrubbed, because one pass through the rate gate alone would look right:
    // `A' = A + w(1-A)` converges to the ceiling for any positive weight, so a
    // half-selected texel would reach 0 given enough dabs. Only a scrub tells a
    // bound from a speed limit.
    for (int k = 0; k < 20; ++k)
      st.paintDab(store, t, Vec2{64.0f, 64.0f}, 256, 256, &sel, nullptr);
    check(maskAt(store, 40, 64) == 1.0f,
          "selection: a scrubbed mask stroke stops dead outside the ants");
    check(maskAt(store, 70, 64) == 0.0f,
          "selection: and reaches the target well inside them");
  }

  // ======================================================================
  // 8. Through a real StrokeSession: coverage moves, content does not
  // ======================================================================
  //
  // The pairing this section exists for. A target concept and a route are two
  // claims, and only a session ties them together: `begin()` resolves the target
  // off the document it is handed, so nothing between the panel's click and the
  // tile write can disagree about which store is meant.
  {
    OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{}, "mask target");
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 255, 255, {0.8f, 0.4f, 0.2f, 1.0f});
    addLayerMask(od.document, 0);
    od.recordEdit("fixture", EditKind::Content);
    const uint64_t revisionBefore = od.revision;
    const size_t historyBefore = od.history.entries().size();
    const std::array<float, 4> contentBefore = rgbAt(*od.document.layers[0].rgbTiles, 128, 128);

    od.maskIsEditTarget = true;  // the click on the mask thumbnail

    BrushTip tip = discTip(24.0f, 1.0f);
    tip.linearRgb = {0.0f, 0.0f, 0.0f};  // black: hide
    StrokeSession s;
    std::string err;
    check(s.begin(od, 0, tip, Tool::Brush, &err) && err.empty(),
          "session: a brush session begins on the mask target");
    check(s.route() == StrokeRoute::MaskPaint,
          "session: and it latched the mask route, not the rgb deposit");
    for (int k = 0; k < 10; ++k) s.addPoint(128.0f, 96.0f + 6.0f * static_cast<float>(k));
    s.end();

    check(s.texelsWritten() > 0 && !s.strokeTiles().empty(),
          "session: the stroke wrote texels and reported dirty tiles");
    check(maskAt(*od.document.layers[0].mask, 128, 128) == 0.0f,
          "session: the MASK coverage is hidden where the stroke went");
    check(maskAt(*od.document.layers[0].mask, 20, 20) == 1.0f,
          "session: and still reveals where it did not");
    // The other half, and the one a picture would not show: the layer's own
    // pixels are untouched. Compared bit for bit rather than approximately,
    // because "the stroke went into the wrong store" and "the stroke went into
    // both" are the two failures this pairing exists to separate.
    check(rgbAt(*od.document.layers[0].rgbTiles, 128, 128) == contentBefore,
          "session: the layer's own texels are BIT-identical -- nothing bled across");
    // **Exactly one history entry, and MORE than one revision bump** -- and the
    // asymmetry is a fact about this class that a thumbnail cache has to know,
    // so it is asserted rather than assumed. `addPoint()` bumps `revision` on
    // every frame that landed tiles ("the revision is what invalidates
    // ui/DocumentTexture's cache"), and `end()` bumps it once more with the
    // history entry. So a cache keyed on the revision follows a stroke LIVE
    // rather than catching up at pen-up -- which is what
    // `app/LayerThumbnail.hpp` §4 claims and what §12's cost measurement is
    // therefore a per-frame number rather than a per-edit one.
    check(od.revision > revisionBefore + 1,
          "session: the revision moved per frame -- a revision cache follows a live stroke");
    check(od.history.entries().size() == historyBefore + 1,
          "session: and exactly one history entry, at pen-up, for the whole drag");
    check(contains(od.history.entries().back().label, "mask"),
          "session: labelled 'mask stroke', not 'brush stroke' -- PRD O2 is scannable");

    // And the negative: with the flag cleared, the identical gesture writes the
    // identical layer's CONTENT and leaves the mask alone. Without this the
    // section would pass just as happily against a build that always painted
    // the mask.
    OpenDocument od2 = makeBlankOpenDocument(256, 256, WorkingSpace{}, "content target");
    addLayerMask(od2.document, 0);
    od2.maskIsEditTarget = false;
    StrokeSession s2;
    check(s2.begin(od2, 0, tip, Tool::Brush, &err) && s2.route() == StrokeRoute::RgbDeposit,
          "session: with the flag cleared the same gesture takes the rgb route");
    for (int k = 0; k < 10; ++k) s2.addPoint(128.0f, 96.0f + 6.0f * static_cast<float>(k));
    s2.end();
    check(od2.document.layers[0].mask->occupiedTileCount() == 0 &&
              od2.document.layers[0].rgbTiles->occupiedTileCount() > 0,
          "session: and it wrote content tiles while the mask store stayed empty");

    // The flag over a maskless layer paints the CONTENT -- the resolution, seen
    // from the far end of the plumbing rather than at the pure function.
    OpenDocument od3 = makeBlankOpenDocument(256, 256, WorkingSpace{}, "no mask");
    od3.maskIsEditTarget = true;
    StrokeSession s3;
    std::string why;
    check(s3.begin(od3, 0, tip, Tool::Brush, &why) && why.empty() &&
              s3.route() == StrokeRoute::RgbDeposit,
          "session: the flag over a maskless layer paints content rather than nothing");
    s3.end();

    // And a locked layer refuses in prose, naming both facts, so the message is
    // about the one problem the user can fix.
    OpenDocument od4 = makeBlankOpenDocument(256, 256, WorkingSpace{}, "locked mask");
    addLayerMask(od4.document, 0);
    od4.document.layers[0].locked = true;
    od4.maskIsEditTarget = true;
    StrokeSession s4;
    std::string lockWhy;
    check(!s4.begin(od4, 0, tip, Tool::Brush, &lockWhy) && contains(lockWhy, "locked") &&
              contains(lockWhy, "mask"),
          "session: a locked layer refuses the mask stroke and names both facts");
  }

  // ======================================================================
  // 9. The two thumbnails, and their two DIFFERENT transfer functions
  // ======================================================================
  //
  // app/LayerThumbnail §1. `ui/CanvasQuad.hpp` records what this application
  // learned the expensive way -- the swapchain is non-sRGB, ImGui's gamma is
  // 1.0, so a byte handed to `AddImage()` reaches the screen unchanged. A layer
  // thumbnail therefore has to be sRGB-encoded on the CPU; a mask thumbnail must
  // NOT be, because coverage is an opacity. Both are checked in the middle of
  // the range, which is the only place the difference exists.
  {
    OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{}, "thumbs");
    // Linear 0.5 at alpha 1.0, stored premultiplied (which at alpha 1 is the
    // same three numbers).
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 255, 255, {0.5f, 0.5f, 0.5f, 1.0f});
    const LayerThumbnail content = layerContentThumbnail(od.document, 0);
    const size_t centre =
        (static_cast<size_t>(kLayerThumbPx / 2) * kLayerThumbPx + kLayerThumbPx / 2) * 4;
    const int encodedByte = static_cast<int>(std::lround(srgbEncode(0.5f) * 255.0f));
    check(content.rgba[centre] == static_cast<uint8_t>(encodedByte) && encodedByte != 128,
          "thumb: a linear 0.5 layer texel becomes the sRGB byte, not the linear one");
    check(content.rgba[centre + 3] == 255,
          "thumb: alpha is a coverage and is NOT encoded -- opaque stays 255");
    std::printf("  [thumb transfer] layer linear 0.500 -> byte %d (un-encoded would be 128)\n",
                static_cast<int>(content.rgba[centre]));

    addLayerMask(od.document, 0);
    MaskTileStore& mask = *od.document.layers[0].mask;
    for (int32_t y = 0; y < 256; ++y)
      for (int32_t x = 0; x < 256; ++x) {
        const PixelCoord at{x, y};
        mask.getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), 0.5f);
      }
    const LayerThumbnail maskThumb = layerMaskThumbnail(od.document, 0);
    check(maskThumb.rgba[centre] == 128,
          "thumb: a 0.5 mask texel stays byte 128 -- coverage is never gamma-encoded");
    check(maskThumb.rgba[centre] != static_cast<uint8_t>(encodedByte),
          "thumb: and is NOT the layer thumbnail's byte -- the two paths differ");
    check(maskThumb.rgba[centre + 1] == 128 && maskThumb.rgba[centre + 2] == 128 &&
              maskThumb.rgba[centre + 3] == 255,
          "thumb: a mask draws as opaque grey, so 'hidden' reads as black not as gap");

    // core/Mask.hpp's three states, kept apart. "No mask" and "a mask that
    // reveals everything" composite identically and must NOT look identical in
    // the panel, or nothing in the row says a mask is there at all.
    OpenDocument bare = makeBlankOpenDocument(64, 64, WorkingSpace{}, "no mask");
    const LayerThumbnail none = layerMaskThumbnail(bare.document, 0);
    check(none.w == 0 && none.h == 0 && none.rgba[centre + 3] == 0,
          "thumb: a layer with NO mask gets an empty thumbnail, not a white one");
    addLayerMask(bare.document, 0);
    const LayerThumbnail revealAll = layerMaskThumbnail(bare.document, 0);
    check(revealAll.w > 0 && revealAll.rgba[centre] == 255 && revealAll.rgba[centre + 3] == 255,
          "thumb: a reveal-all mask (zero tiles) is solid WHITE, not solid black");
    // The inverted-absence bug this guards against is the one core/Mask.hpp
    // designed out of the tile type: with the branch the wrong way round every
    // freshly added mask would draw black -- "discovered by the user as a black
    // layer".
    check(revealAll.rgba[centre] != 0, "thumb: absent mask tiles read as reveal, not as hide");
  }

  // ======================================================================
  // 10. Orientation: a thumbnail is not flipped, mirrored or transposed
  // ======================================================================
  //
  // A 24 px square is exactly the size at which a flip is invisible to a person
  // scanning a panel, and symmetric content would hide one anyway -- so the
  // fixture is asymmetric in BOTH axes and the assertion names all four
  // quadrants. The golden views cover the other half (that the upload does not
  // flip it either); this covers the CPU build, where a row-order mistake lives.
  {
    OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{}, "orientation");
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 127, 127, {1.0f, 0.0f, 0.0f, 1.0f});
    const LayerThumbnail t = layerContentThumbnail(od.document, 0);
    auto px = [&](int x, int y, int c) {
      return t.rgba[(static_cast<size_t>(y) * kLayerThumbPx + static_cast<size_t>(x)) * 4 +
                    static_cast<size_t>(c)];
    };
    check(px(4, 4, 3) == 255 && px(4, 4, 0) == 255 && px(4, 4, 1) == 0,
          "orientation: the document's top-left quadrant is the thumbnail's, and is red");
    check(px(19, 4, 3) == 0 && px(4, 19, 3) == 0 && px(19, 19, 3) == 0,
          "orientation: the other three quadrants are empty -- no flip, no mirror");

    // Aspect: a wide document letterboxes rather than stretching, so a thumbnail
    // is never a picture of a document that does not exist.
    OpenDocument wide = makeBlankOpenDocument(400, 100, WorkingSpace{}, "wide");
    fillRect(*wide.document.layers[0].rgbTiles, 0, 0, 399, 99, {1.0f, 1.0f, 1.0f, 1.0f});
    const LayerThumbnail w = layerContentThumbnail(wide.document, 0);
    check(w.w == kLayerThumbPx && w.h < w.w && w.y > 0,
          "orientation: a 4:1 document letterboxes and is centred, never stretched");
  }

  // ======================================================================
  // 11. The cache invalidates on the revision it claims to key on
  // ======================================================================
  //
  // app/LayerThumbnail §4, and `core::SelectionBoundaryCache`'s own rule: a
  // cache that never refreshes returns a plausible-looking picture forever and
  // passes every test that draws once. Both halves are checked -- the build
  // count moves, AND the picture that comes back is the new one.
  {
    OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{}, "cache");
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 255, 255, {1.0f, 0.0f, 0.0f, 1.0f});
    od.recordEdit("fixture", EditKind::Content);

    LayerThumbnailCache cache;
    const size_t centre =
        (static_cast<size_t>(kLayerThumbPx / 2) * kLayerThumbPx + kLayerThumbPx / 2) * 4;
    const uint8_t red = cache.rowFor(od.document, 0, od.id, od.revision).content.rgba[centre];
    check(cache.buildCount() == 1 && red == 255,
          "cache: the first ask builds once and returns the red layer");
    for (int k = 0; k < 20; ++k) cache.rowFor(od.document, 0, od.id, od.revision);
    check(cache.buildCount() == 1,
          "cache: twenty more asks at the same key build nothing (the panel redraws)");

    // Repaint the layer BLUE without moving the revision. The cache is entitled
    // to return the old picture here, and asserting that it does is what pins the
    // key down: a cache that rebuilt anyway would be doing this work every frame
    // and this assertion is the only thing that would notice.
    fillRect(*od.document.layers[0].rgbTiles, 0, 0, 255, 255, {0.0f, 0.0f, 1.0f, 1.0f});
    const uint8_t stale = cache.rowFor(od.document, 0, od.id, od.revision).content.rgba[centre];
    check(cache.buildCount() == 1 && stale == red,
          "cache: tiles changed with no revision bump -> the key says nothing happened");

    // Now move the revision, which is what every real edit does
    // (`recordEdit()`), and the picture must follow.
    od.recordEdit("repaint", EditKind::Content);
    const LayerThumbnailCache::Row& fresh = cache.rowFor(od.document, 0, od.id, od.revision);
    check(cache.buildCount() == 2 && fresh.content.rgba[centre] == 0 &&
              fresh.content.rgba[centre + 2] == 255,
          "cache: the revision moved -> rebuilt, and the picture is the NEW one");

    // Two documents at the same revision. Revisions start at 0 per document, so
    // this is the common case rather than the rare one -- keying on the revision
    // alone would show one tab's layer in the other tab's panel.
    OpenDocument other = makeBlankOpenDocument(256, 256, WorkingSpace{}, "other tab");
    fillRect(*other.document.layers[0].rgbTiles, 0, 0, 255, 255, {0.0f, 1.0f, 0.0f, 1.0f});
    other.recordEdit("green", EditKind::Content);
    other.revision = od.revision;  // the collision, made explicit
    const LayerThumbnailCache::Row& green =
        cache.rowFor(other.document, 0, other.id, other.revision);
    check(green.content.rgba[centre + 1] == 255 && green.content.rgba[centre + 2] == 0,
          "cache: a second document at the SAME revision gets its own picture");

    // The explicit escape hatch, for a caller that has written tiles behind the
    // revision's back -- `main.cpp`'s `buildDemoDocument()` is the one path in
    // this repository that does, and it calls `recordEdit()` by hand for exactly
    // this reason.
    const size_t before = cache.buildCount();
    cache.invalidate();
    cache.rowFor(od.document, 0, od.id, od.revision);
    check(cache.buildCount() == before + 1 && cache.residentRows() == 1,
          "cache: invalidate() forces a rebuild whatever the key says");
  }

  // ======================================================================
  // 12. The cost, measured rather than argued
  // ======================================================================
  //
  // app/LayerThumbnail §3 claims sampling is O(thumbnail) and not O(document).
  // The claim is checkable in one line -- build the same thumbnail from a small
  // document and from one 256 times its area and compare the sample counts --
  // and it is worth checking, because the plausible implementation (walk the
  // tiles and downsample) has exactly the same signature and would be 256 times
  // the work for twenty rows on every edit.
  {
    OpenDocument small = makeBlankOpenDocument(256, 256, WorkingSpace{}, "small");
    fillRect(*small.document.layers[0].rgbTiles, 0, 0, 255, 255, {1.0f, 1.0f, 1.0f, 1.0f});
    OpenDocument big = makeBlankOpenDocument(4096, 4096, WorkingSpace{}, "big");
    fillRect(*big.document.layers[0].rgbTiles, 0, 0, 255, 255, {1.0f, 1.0f, 1.0f, 1.0f});

    const auto started = std::chrono::steady_clock::now();
    const LayerThumbnail a = layerContentThumbnail(small.document, 0);
    const LayerThumbnail b = layerContentThumbnail(big.document, 0);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    check(a.samples == b.samples,
          "cost: a 4096px document costs the same samples as a 256px one");
    check(a.samples <= static_cast<size_t>(kLayerThumbPx) * kLayerThumbPx * kThumbSupersample *
                           kThumbSupersample,
          "cost: and that is the declared bound, not something larger");
    std::printf("  [thumb cost] %zu samples per thumbnail on both documents; two built in "
                "%.3f ms -> ~%.2f ms for 10 visible rows x 2 thumbnails\n",
                a.samples, ms, ms * 10.0);
  }

  std::printf("[selftest] mask target %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
