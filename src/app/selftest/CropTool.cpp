#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/CropTool.hpp"
#include "app/MoveTool.hpp"       // toolMovesPixels(), one of the gates Crop must NOT answer
#include "app/StrokeSession.hpp"  // the four Crop must not answer either
#include "app/ToolSurface.hpp"    // the sixth surface-refusal row
#include "app/ToolSwitch.hpp"     // a tool change discards a pending crop
#include "app/ZoomAndSize.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/MenuModel.hpp"

namespace np {

// app/CropTool -- `Tool::Crop`, the palette's `C` cell, in both modes, and the
// two Image-menu items that fall out of the same machinery.
//
// **What this section deliberately does NOT re-prove** is the geometry:
// `ops/Transform`'s eight-unknown homography, `ops/DocumentTransform`'s crop
// and its document-level warp each already have a section of their own, and
// app/CropTool adds no second copy of any of them. What is new -- and therefore
// what is asserted here -- is the three judgement calls that header records:
//
//   the rectangle a drag names (sorted, rounded OUTWARD, deliberately NOT
//   clamped); the rectangle a four-corner quad warps into (the longer of each
//   pair of opposite edges, and why that beats the mean); and which quads are
//   refused, by name, in the tool rather than in the engine.
//
// That last one carries the finding this section exists to pin: **the engine
// does not refuse a bow-tie.** `transformFromQuad()`'s singularity test is
// `|pivot| < 1e-12` on the 8x8 solve, which catches collinear points and
// collapsed quads and has nothing to say about a quad with one corner dragged
// past its neighbour -- that system is well-conditioned and solves happily,
// into a homography that folds the picture through itself. Section D asserts
// the engine accepting it and the tool refusing it, in adjacent lines, because
// "the UI refusal is redundant with the engine's" is exactly the belief that
// would delete it.
//
// Headless and GPU-free. The tool's UI half -- ui/MacPaintUI's two delimited
// Crop blocks and ui/AtelierChrome's options row -- is out of reach here for
// the reason every canvas block is, which is why the decisions were lifted into
// app/CropTool in the first place, and why this feature also ships five golden
// views: `crop_options` and `crop_options_perspective` for the band in each of
// its two modes, `crop_drag` and `crop_perspective` for the two canvas gestures
// with their darkened surround, and `crop_refused` for the bow-tie -- the
// dashed outline, the greyed CROP button and the whole sentence. That last one
// is the negative half and it is the one this feature most needs: a build whose
// refusal predicate was perfectly correct and whose UI said nothing about it
// would pass every assertion below.
bool runCropToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] crop tool: the rectangle rule, the perspective output extent, the refusals, "
      "and the two menu items\n");

  // A deterministic, high-frequency fill: every texel differs from its
  // neighbours, so "the pixels came back identical" is a real claim rather than
  // one a flat fill would satisfy by accident. Same fixture shape as
  // app/selftest/MoveTool.cpp's, for the same reason.
  auto fillRgb = [](TileStore& store, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    for (int32_t y = y0; y < y0 + h; ++y) {
      for (int32_t x = x0; x < x0 + w; ++x) {
        const float fx = static_cast<float>(x - x0) / static_cast<float>(w);
        const float fy = static_cast<float>(y - y0) / static_cast<float>(h);
        const float v = 0.5f + 0.25f * std::sin(fx * 23.0f) * std::cos(fy * 17.0f) +
                        0.15f * std::sin((fx + fy) * 37.0f);
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, 0.5f * v, 1.0f - v, 1.0f});
      }
    }
  };
  auto region = [](const OpenDocument& od, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return imageFromTileStore(*od.document.layers[0].rgbTiles, x, y, w, h);
  };
  auto samePixels = [](const TransformImage& a, const TransformImage& b) {
    return a.px.size() == b.px.size() &&
           std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(float)) == 0;
  };
  auto quadOf = [](float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                   float y3) {
    return CropQuad{{Point2{x0, y0}, Point2{x1, y1}, Point2{x2, y2}, Point2{x3, y3}}};
  };

  // -----------------------------------------------------------------------
  // A. The eighth gate (app/CropTool.hpp section 6)
  // -----------------------------------------------------------------------
  //
  // `runEyedropperTest()` already asserts `toolImplemented() ==
  // toolHasCanvasHandler()` for every tool. What is asserted HERE is the half
  // that assertion cannot see: that Crop is handled through its OWN predicate
  // and not by widening somebody else's -- the same claim the Move and Zoom
  // rows make, and here with the sharpest consequence of the three, because
  // `toolDrawsSelection()` is the gate on the selection tools' canvas block and
  // a Crop inside it would have had every crop drag handed to
  // `commitDrawnSelection()`.
  {
    check(toolCropsCanvas(Tool::Crop), "gate: toolCropsCanvas() is true for Tool::Crop");
    bool othersFalse = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (t != Tool::Crop && toolCropsCanvas(t)) othersFalse = false;
    }
    check(othersFalse,
          "gate: and false for every other tool -- Tool::Slice shares the palette group and "
          "the cursor and is still not built");
    check(!toolImplemented(Tool::Slice) && !toolHasCanvasHandler(Tool::Slice),
          "gate: Slice stays unbuilt -- a group pairing is a layout fact, not a capability");
    check(toolImplemented(Tool::Crop) && toolHasCanvasHandler(Tool::Crop) &&
              toolNoHandlerException(Tool::Crop) == nullptr,
          "gate: Crop is implemented, has a canvas handler and needs no recorded exception");
    check(!toolDrawsSelection(Tool::Crop) && !toolWritesRgbPixels(Tool::Crop) &&
              !toolSamplesCanvas(Tool::Crop) && !toolMeasuresCanvas(Tool::Crop) &&
              !toolPansView(Tool::Crop) && !toolMovesPixels(Tool::Crop) &&
              !toolZoomsView(Tool::Crop) && !toolBeginsStroke(Tool::Crop),
          "gate: and through NO other gate -- the assertion that catches Crop being wired by "
          "widening toolDrawsSelection() instead of adding the eighth");
    const char* surface = toolSurfaceRefusal(Tool::Crop, false);
    check(surface != nullptr && std::strstr(surface, "Nothing to crop") != nullptr &&
              std::strstr(surface, "File > New Document makes one.") != nullptr,
          "gate: with no document the surface names CROP and ends in the build's own clause");
    check(toolSurfaceRefusal(Tool::Slice, false) == nullptr,
          "gate: and Slice gets no surface sentence -- one reason per cell, and its reason is "
          "\"Not built yet.\"");
  }

  // -----------------------------------------------------------------------
  // A2. The MODE table is complete (app/CropTool.hpp, the kGradientKinds rule)
  // -----------------------------------------------------------------------
  {
    bool everyModeListed = true;
    for (const CropMode m : {CropMode::Rectangle, CropMode::Perspective}) {
      bool found = false;
      for (const CropModeRow& row : kCropModes)
        if (row.mode == m) found = true;
      if (!found) everyModeListed = false;
    }
    check(everyModeListed && kCropModeCount == 2,
          "modes: the options-bar table has a row for every CropMode -- a mode added to the "
          "enum and not to the table would be a combo silently offering one of two");
    check(std::strcmp(cropModeLabel(CropMode::Rectangle),
                      cropModeLabel(CropMode::Perspective)) != 0,
          "modes: and the two labels differ, so the combo cannot show one name for both");
  }

  // -----------------------------------------------------------------------
  // B. The rectangle rule (section 2)
  // -----------------------------------------------------------------------
  {
    const DocumentRegion forward = cropRegionFromDrag(10.4f, 20.6f, 30.2f, 40.1f);
    const DocumentRegion backward = cropRegionFromDrag(30.2f, 40.1f, 10.4f, 20.6f);
    check(forward == backward,
          "rect rule: a drag names the same rectangle in either direction (sorted)");
    // floor(10.4)=10, ceil(30.2)=31 -> 21 wide. floor(20.6)=20, ceil(40.1)=41 -> 21 tall.
    // Every one of the four numbers differs from the round-to-nearest answer,
    // so an implementation that rounded to nearest fails all of them rather
    // than passing three by luck.
    check(forward.x == 10 && forward.y == 20 && forward.width == 21u && forward.height == 21u,
          "rect rule: rounded OUTWARD -- (10.4,20.6)-(30.2,40.1) is (10,20)+21x21, not the "
          "round-to-nearest (10,21)+20x19");
    const DocumentRegion offCanvas = cropRegionFromDrag(-12.0f, -8.0f, 40.0f, 30.0f);
    check(offCanvas.x == -12 && offCanvas.y == -8 && offCanvas.width == 52u &&
              offCanvas.height == 38u,
          "rect rule: NOT clamped -- a negative origin survives, because that is how "
          "cropDocument() spells \"extend the canvas\"");
    const DocumentRegion subTexel = cropRegionFromDrag(10.2f, 10.2f, 10.4f, 10.4f);
    check(subTexel.x == 10 && subTexel.width == 1u && subTexel.height == 1u,
          "rect rule: a sub-texel drag rounds outward to the ONE texel it is inside -- the "
          "rule keeping pixels, applied at the smallest scale it has");
    check(cropRegionFromDrag(10.2f, 10.2f, 10.2f, 10.2f).empty() &&
              cropRegionFromDrag(10.0f, 10.0f, 40.0f, 10.0f).empty(),
          "rect rule: but a drag with NO extent on either axis is empty, not one texel -- a "
          "click that armed a 1x1 crop would put Enter one keystroke from destroying the "
          "document");
  }

  // -----------------------------------------------------------------------
  // C. A rectangle crop: exact, undoable, and undo gives back what it HID
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(96, 64, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 0, 0, 96, 64);
    od.recordEdit("fill fixture", EditKind::Content);
    const TransformImage whole = region(od, 0, 0, 96, 64);
    const TransformImage kept = region(od, 20, 12, 40, 30);
    const size_t entriesBefore = od.history.entries().size();

    const DocumentTransformResult r = applyCropRegion(od, DocumentRegion{20, 12, 40u, 30u});
    check(r.ok && od.document.width == 40u && od.document.height == 30u,
          "rect crop: the extent becomes the rectangle");
    check(r.reconstructionPasses == 0,
          "rect crop: reconstructionPasses == 0 -- the EXACT path, stated as a number rather "
          "than inferred from which function was called");
    check(r.previousWidth == 96 && r.previousHeight == 64,
          "rect crop: and the result reports the extent it replaced");
    check(samePixels(region(od, 0, 0, 40, 30), kept),
          "rect crop: the kept region is bit-identical to what was under it -- memcmp, not a "
          "tolerance");
    check(od.history.entries().size() == entriesBefore + 1 && r.editLabel == "crop",
          "rect crop: exactly one history entry, labelled \"crop\"");

    // The non-clipping contract, which is the whole reason a crop can be
    // undone at all: `cropDocument()` KEEPS content outside the new extent, so
    // undo restores the picture rather than a rectangle of it.
    if (const Document* prior = od.history.undo()) od.document = *prior;
    check(od.document.width == 96u && od.document.height == 64u,
          "rect crop: undo restores the extent");
    check(samePixels(region(od, 0, 0, 96, 64), whole),
          "rect crop: and every texel the crop HID comes back bit-identical -- the "
          "non-clipping contract, asserted rather than assumed");
  }

  // -----------------------------------------------------------------------
  // C2. A crop that is the whole canvas is not an edit; a negative origin grows
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(64, 48, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 0, 0, 64, 48);
    od.recordEdit("fill fixture", EditKind::Content);
    const size_t entries = od.history.entries().size();
    const DocumentTransformResult noop = applyCropRegion(od, DocumentRegion{0, 0, 64u, 48u});
    check(noop.ok && od.history.entries().size() == entries,
          "rect crop: cropping to the whole canvas is ok and records NO history entry -- "
          "applyImageSize()'s own \"a no-op the user asked for is not an edit\" rule");

    const DocumentTransformResult grow = applyCropRegion(od, DocumentRegion{-16, -8, 96u, 64u});
    check(grow.ok && od.document.width == 96u && od.document.height == 64u,
          "rect crop: a negative origin GROWS the canvas, which is how the tool exposes "
          "cropDocument()'s signed-origin rule instead of hiding it");
    check(samePixels(region(od, 16, 8, 64, 48),
                     [&] {
                       OpenDocument fresh = makeBlankOpenDocument(64, 48, WorkingSpace{});
                       fillRgb(*fresh.document.layers[0].rgbTiles, 0, 0, 64, 48);
                       return region(fresh, 0, 0, 64, 48);
                     }()),
          "rect crop: and the old picture is exactly where the new origin puts it");

    const DocumentTransformResult empty = applyCropRegion(od, DocumentRegion{0, 0, 0u, 10u});
    check(!empty.ok && empty.error.find("less than one texel") != std::string::npos,
          "rect crop: an empty rectangle is refused IN WORDS, not discarded in silence");
  }

  // -----------------------------------------------------------------------
  // D. The perspective output extent (section 3) -- the decision itself
  // -----------------------------------------------------------------------
  {
    // A plain rectangle. The property the mode toggle rests on: for a
    // rectangular quad the two rules agree with each other AND with the
    // Rectangle mode's own region, so switching modes on an unmoved shape
    // cannot change what commits.
    const CropQuad rect = cropQuadFromRegion(DocumentRegion{10, 20, 300u, 200u});
    const DocumentRegion rectMax = perspectiveCropExtent(rect);
    const DocumentRegion rectMean = perspectiveCropExtentByMean(rect);
    check(rectMax.width == 300u && rectMax.height == 200u,
          "extent: a rectangular quad gives back its own width and height");
    check(rectMax.width == rectMean.width && rectMax.height == rectMean.height,
          "extent: max and mean agree exactly on a rectangle -- the property that keeps the "
          "two modes from being two tools sharing a name");

    // A keystone: top edge 400 long, bottom edge 200 long, both sides 300 tall
    // in y. Chosen so max and mean give DIFFERENT integers (400 against 300)
    // and so no two of the four edge lengths coincide by accident.
    //   TL(100,100) TR(500,100) BR(400,400) BL(200,400)
    // top    = 400
    // bottom = 200
    // left   = |(200,400)-(100,100)| = hypot(100,300) = 316.227...
    // right  = |(400,400)-(500,100)| = hypot(100,300) = 316.227...
    const CropQuad keystone = quadOf(100.f, 100.f, 500.f, 100.f, 400.f, 400.f, 200.f, 400.f);
    const DocumentRegion kMax = perspectiveCropExtent(keystone);
    const DocumentRegion kMean = perspectiveCropExtentByMean(keystone);
    check(kMax.width == 400u,
          "extent: the keystone's width is its LONGER horizontal edge (400), not the mean "
          "(300) -- the near edge is never resampled down");
    check(kMean.width == 300u,
          "extent: and the mean rule really would have said 300, so the two are being "
          "compared rather than one being computed twice");
    check(kMax.height == 316u && kMean.height == 316u,
          "extent: the two vertical edges are equal here, so both rules give 316 -- the "
          "difference between the rules is on the axis that is keystoned, and only there");

    // The relationship, as a property over a family rather than as two hand
    // computed numbers that a later change to either rule would leave agreeing
    // by luck.
    bool maxNeverBelowMean = true;
    bool differsSomewhere = false;
    for (int i = 0; i < 24; ++i) {
      const float shrink = 20.0f + static_cast<float>(i) * 11.0f;
      const CropQuad q = quadOf(0.f, 0.f, 500.f, 0.f, 500.f - shrink, 300.f, shrink, 300.f);
      const DocumentRegion a = perspectiveCropExtent(q);
      const DocumentRegion b = perspectiveCropExtentByMean(q);
      if (a.width < b.width || a.height < b.height) maxNeverBelowMean = false;
      if (a.width != b.width) differsSomewhere = true;
    }
    check(maxNeverBelowMean && differsSomewhere,
          "extent: over 24 keystones of increasing severity the chosen rule is never SMALLER "
          "than the mean, and is genuinely larger on some -- so it can only ever interpolate "
          "where the mean would have discarded");

    // Monotone under the gesture: pulling a corner outward never shrinks the
    // result. The failure this rules out is a rule that reads as the picture
    // flinching away from the handle being dragged.
    bool monotone = true;
    DocumentRegion previous = perspectiveCropExtent(
        quadOf(0.f, 0.f, 200.f, 0.f, 200.f, 200.f, 0.f, 200.f));
    for (int i = 1; i <= 12; ++i) {
      const float out = static_cast<float>(i) * 9.0f;
      const DocumentRegion now = perspectiveCropExtent(
          quadOf(0.f, 0.f, 200.f + out, 0.f, 200.f, 200.f, 0.f, 200.f));
      if (now.width < previous.width || now.height < previous.height) monotone = false;
      previous = now;
    }
    check(monotone,
          "extent: dragging one corner outward never shrinks the output -- monotone under the "
          "gesture, which the mean rule is not");

    check(perspectiveCropExtent(quadOf(0.f, 0.f, 0.2f, 0.f, 0.2f, 0.2f, 0.f, 0.2f)).width >= 1u,
          "extent: floored at one texel, so it is always an extent the engines accept even "
          "for a quad the refusal ladder is about to reject");
  }

  // -----------------------------------------------------------------------
  // E. The refusals (section 4), INCLUDING the one the engine does not make
  // -----------------------------------------------------------------------
  {
    const CropQuad good = quadOf(100.f, 100.f, 500.f, 100.f, 400.f, 400.f, 200.f, 400.f);
    check(cropQuadIsUsable(good) && cropQuadRefusal(good).empty(),
          "refusal: an ordinary keystone is accepted, and the two spellings of that agree");

    const CropQuad collapsed = quadOf(100.f, 100.f, 100.4f, 100.2f, 400.f, 400.f, 100.f, 400.f);
    check(cropQuadRefusal(collapsed).find("same texel") != std::string::npos,
          "refusal: two corners on one texel are refused BY NAME (collapsed)");

    // Corner 1 within ~0.6 degrees of straight: sin ~= 0.010, below
    // kMinCornerSin (0.02). And a companion quad just the other side of it, so
    // the threshold is being tested rather than a direction.
    auto nearlyStraight = [&](float sine) {
      const float dy = 400.0f * sine;
      return quadOf(0.f, 0.f, 200.f, 0.f, 400.f, dy, 200.f, 300.f);
    };
    check(!cropQuadRefusal(nearlyStraight(kMinCornerSin * 0.4f)).empty(),
          "refusal: a corner well inside kMinCornerSin is refused -- the solve loses the far "
          "corner long before |pivot| < 1e-12 makes it singular");
    check(cropQuadRefusal(nearlyStraight(kMinCornerSin * 8.0f)).empty(),
          "refusal: and a corner comfortably outside it is accepted, so the threshold is a "
          "threshold and not a blanket");

    // The bow-tie: corner 0 and corner 1 swapped from the good quad, which is
    // exactly what dragging one corner past its neighbour produces.
    const CropQuad bowTie = quadOf(500.f, 100.f, 100.f, 100.f, 400.f, 400.f, 200.f, 400.f);
    const std::string bowWhy = cropQuadRefusal(bowTie);
    check(bowWhy.find("crosses itself") != std::string::npos,
          "refusal: a bow-tie is refused BY NAME (self-intersecting)");

    // **The finding.** The engine solves the bow-tie without complaint: its
    // singularity test is on the 8x8 pivot, and a crossed quad is not singular.
    // So the UI-level convexity refusal is not a restatement of the engine's --
    // delete it and the tool folds the picture through itself in silence.
    {
      const DocumentRegion extent = perspectiveCropExtent(bowTie);
      const std::array<Point2, 4> dst{
          Point2{0.0f, 0.0f}, Point2{static_cast<float>(extent.width), 0.0f},
          Point2{static_cast<float>(extent.width), static_cast<float>(extent.height)},
          Point2{0.0f, static_cast<float>(extent.height)}};
      Mat3 m;
      std::string engineWhy;
      const bool engineAccepts = transformFromQuad(bowTie.c, dst, &m, &engineWhy);
      check(engineAccepts && engineWhy.empty(),
            "refusal: transformFromQuad() ACCEPTS the same bow-tie -- its test is |pivot| < "
            "1e-12 and a crossed quad is not singular, so the tool's convexity refusal is a "
            "new guarantee and not a duplicate of the engine's");
    }

    // The reversed winding: the good quad walked the other way round. Not a
    // bow-tie -- every turn is consistent -- but consistently the wrong way,
    // which would silently mirror the result.
    const CropQuad reversed = quadOf(200.f, 400.f, 400.f, 400.f, 500.f, 100.f, 100.f, 100.f);
    check(cropQuadRefusal(reversed).find("mirrored") != std::string::npos,
          "refusal: a quad wound the wrong way is refused BY NAME (would come out mirrored), "
          "which no turn-consistency test alone would catch");

    check(!cropQuadIsSteep(good),
          "steep: a 2:1 keystone is under the warning threshold and draws no warning");
    check(cropQuadIsSteep(quadOf(0.f, 0.f, 800.f, 0.f, 500.f, 300.f, 300.f, 300.f)),
          "steep: a 4:1 keystone is past kSteepQuadEdgeRatio and warns -- ops/Transform.hpp "
          "section 3's centre-Jacobian limitation, named rather than silently exceeded");
    check(!cropQuadIsSteep(bowTie),
          "steep: a REFUSED quad never also warns -- one message at a time");
  }

  // -----------------------------------------------------------------------
  // F. A perspective crop actually commits, through the engine
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(600, 400, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 0, 0, 600, 400);
    od.recordEdit("fill fixture", EditKind::Content);
    const size_t entries = od.history.entries().size();

    const CropQuad keystone = quadOf(100.f, 100.f, 500.f, 100.f, 400.f, 380.f, 200.f, 380.f);
    const DocumentRegion expect = perspectiveCropExtent(keystone);
    const DocumentTransformResult r = applyCropPerspective(od, keystone);
    check(r.ok && od.document.width == expect.width && od.document.height == expect.height,
          "perspective: the document comes out at exactly the extent the rule named");
    check(r.reconstructionPasses >= 1,
          "perspective: reconstructionPasses >= 1 -- the RESAMPLE path, against the rectangle "
          "crop's 0, which is the difference between moving pixels and remaking them");
    check(r.editLabel == "perspective crop" && od.history.entries().size() == entries + 1,
          "perspective: one history entry, and labelled for the tool rather than "
          "transformDocument()'s generic \"transform document\"");

    // A refused commit changes nothing at all -- not the extent, not history.
    OpenDocument od2 = makeBlankOpenDocument(200, 150, WorkingSpace{});
    const size_t entries2 = od2.history.entries().size();
    const CropQuad bowTie = quadOf(150.f, 20.f, 20.f, 20.f, 180.f, 130.f, 40.f, 130.f);
    const DocumentTransformResult refused = applyCropPerspective(od2, bowTie);
    check(!refused.ok && refused.error.find("crosses itself") != std::string::npos,
          "perspective: a bow-tie commit is refused with the SAME sentence the canvas has "
          "been showing -- one predicate, two readers");
    check(od2.document.width == 200u && od2.document.height == 150u &&
              od2.history.entries().size() == entries2,
          "perspective: and a refused commit leaves the extent and the history untouched");
  }

  // -----------------------------------------------------------------------
  // G. The two menu items (section 7)
  // -----------------------------------------------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(120, 90, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 0, 0, 120, 90);
    od.recordEdit("fill fixture", EditKind::Content);

    check(!cropToSelectionRegion(od).has_value(),
          "menu: with no selection there is no Crop to Selection region");
    const DocumentTransformResult noSel = applyCropToSelection(od);
    check(!noSel.ok && noSel.error.find("no selection") != std::string::npos,
          "menu: and the command refuses in words naming the missing selection");

    od.selection = selectRectangle(30.0f, 20.0f, 70.0f, 60.0f);
    const std::optional<DocumentRegion> selRegion = cropToSelectionRegion(od);
    check(selRegion.has_value() && selRegion->x == 30 && selRegion->y == 20 &&
              selRegion->width == 40u && selRegion->height == 40u,
          "menu: Crop to Selection's region is the selection's own bounds (30,20)+40x40");
    const DocumentTransformResult cropped = applyCropToSelection(od);
    check(cropped.ok && od.document.width == 40u && od.document.height == 40u,
          "menu: and applying it really leaves a 40x40 document");
  }
  {
    // Trim: content in one corner only, plus content deliberately placed
    // OUTSIDE the canvas -- which `core/LayerGeometry.hpp` section 4 says
    // genuinely exists. The assertion is that trim clips it away rather than
    // growing the document to include it.
    OpenDocument od = makeBlankOpenDocument(200, 150, WorkingSpace{});
    check(!trimToContentRegion(od.document).has_value(),
          "menu: a document whose layers are all empty has no Trim region");
    const DocumentTransformResult nothing = applyTrimToContent(od);
    check(!nothing.ok && nothing.error.find("nothing to trim") != std::string::npos,
          "menu: and Trim refuses it by name rather than cropping the document to nothing");

    fillRgb(*od.document.layers[0].rgbTiles, 40, 30, 60, 50);
    od.recordEdit("fill fixture", EditKind::Content);
    const std::optional<DocumentRegion> inside = trimToContentRegion(od.document);
    check(inside.has_value() && inside->x == 40 && inside->y == 30 && inside->width == 60u &&
              inside->height == 50u,
          "menu: Trim's region is the union of layer content bounds (40,30)+60x50");

    fillRgb(*od.document.layers[0].rgbTiles, 240, 180, 20, 20);
    const std::optional<DocumentRegion> outside = trimToContentRegion(od.document);
    check(outside.has_value() && outside->x == 40 && outside->y == 30 &&
              outside->x + static_cast<int32_t>(outside->width) <= 200 &&
              outside->y + static_cast<int32_t>(outside->height) <= 150,
          "menu: content beyond the canvas does NOT make Trim grow the document -- a command "
          "called \"trim\" that made the picture bigger would be an astonishing answer");
  }

  // -----------------------------------------------------------------------
  // G2. The two items are really IN the Image menu, and gated as claimed
  // -----------------------------------------------------------------------
  //
  // An absence claim needs a positive test: `MenuAction::CropToSelection`
  // existing as an enumerator and being handled in a switch says nothing about
  // whether `buildMenuModel()` ever puts it in the tree, and a menu item nobody
  // can click is the reachability gap docs/reachability-audit.md C1 is about.
  {
    // A local walker rather than a shared one: `findMenuAction()` lives in
    // app/selftest/FilterMenu.cpp's own file scope, and the split-per-section
    // rule (app/selftest/Support.hpp) is that internal linkage does not cross a
    // TU. Four lines is cheaper than promoting it.
    struct Find {
      static const MenuNode* in(const std::vector<MenuNode>& nodes, MenuAction a) {
        for (const MenuNode& n : nodes) {
          if ((n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check) &&
              n.action == a)
            return &n;
          if (const MenuNode* f = in(n.children, a)) return f;
        }
        return nullptr;
      }
    };

    MenuContext noDoc;
    noDoc.hasDocument = false;
    noDoc.hasSelection = false;
    const std::vector<MenuNode> closed = buildMenuModel(noDoc);
    const MenuNode* cropClosed = Find::in(closed, MenuAction::CropToSelection);
    const MenuNode* trimClosed = Find::in(closed, MenuAction::TrimToContent);
    check(cropClosed != nullptr && trimClosed != nullptr,
          "menu: both items are actually IN the tree -- an enumerator and a switch arm say "
          "nothing about whether buildMenuModel() ever offers them");
    check(cropClosed != nullptr && !cropClosed->enabled && trimClosed != nullptr &&
              !trimClosed->enabled,
          "menu: and both are disabled with no document open");

    MenuContext docNoSel;
    docNoSel.hasDocument = true;
    docNoSel.hasSelection = false;
    const std::vector<MenuNode> open = buildMenuModel(docNoSel);
    const MenuNode* cropNoSel = Find::in(open, MenuAction::CropToSelection);
    const MenuNode* trimNoSel = Find::in(open, MenuAction::TrimToContent);
    check(cropNoSel != nullptr && !cropNoSel->enabled,
          "menu: Crop to Selection takes the STRONGER gate -- a document is not enough, "
          "because its precondition is a fact this snapshot already carries and a dead item "
          "with nothing to explain should grey");
    check(trimNoSel != nullptr && trimNoSel->enabled,
          "menu: Trim to Content takes the WEAKER one -- whether any layer holds content is "
          "not in the snapshot and cannot cheaply be, so applyTrimToContent() answers it in a "
          "sentence instead, the Adjustments submenu's own trade");

    MenuContext docSel = docNoSel;
    docSel.hasSelection = true;
    const MenuNode* cropSel = Find::in(buildMenuModel(docSel), MenuAction::CropToSelection);
    check(cropSel != nullptr && cropSel->enabled,
          "menu: and Crop to Selection goes live the moment there is a selection");
  }

  // -----------------------------------------------------------------------
  // H. The gesture (section 5)
  // -----------------------------------------------------------------------
  {
    CropSession s;
    check(!s.active && !s.defining && s.dragHandle == -1,
          "gesture: a fresh session is inactive and holds no handle");

    cropBeginDefine(s, 7u, CropMode::Rectangle, 10.0f, 20.0f);
    check(s.defining && s.doc == 7u && cropRegionOf(s).empty(),
          "gesture: the first frame of a drag is degenerate -- the moving corner starts ON "
          "the anchor rather than on last drag's endpoint");

    s.quad = cropQuadFromRegion(cropRegionFromDrag(10.0f, 20.0f, 110.0f, 90.0f));
    s.defining = false;
    s.active = true;
    check(cropRegionOf(s) == DocumentRegion{10, 20, 100u, 70u},
          "gesture: the laid-down rectangle is the drag's own region");

    // A corner drag in Rectangle mode moves its two neighbours, so the four
    // corners never stop naming a rectangle.
    cropDragHandle(s, 0, 30.0f, 40.0f);
    check(s.quad.c[0].x == 30.0f && s.quad.c[3].x == 30.0f && s.quad.c[1].y == 40.0f &&
              s.quad.c[1].x == 110.0f && s.quad.c[2].y == 90.0f,
          "gesture: dragging a corner in Rectangle mode carries its two neighbours -- the "
          "shape stays a rectangle by construction, not by a second field kept in step");
    cropDragHandle(s, 5, 200.0f, 999.0f);
    check(s.quad.c[1].x == 200.0f && s.quad.c[2].x == 200.0f && s.quad.c[1].y == 40.0f,
          "gesture: an edge handle moves one side only, and ignores the axis it does not own");

    // The same handle index in Perspective mode moves one corner alone.
    cropSetMode(s, CropMode::Perspective);
    const CropQuad before = s.quad;
    cropDragHandle(s, 2, 400.0f, 500.0f);
    auto samePoint = [](const Point2& a, const Point2& b) {
      return a.x == b.x && a.y == b.y;
    };
    check(s.quad.c[2].x == 400.0f && s.quad.c[2].y == 500.0f &&
              samePoint(s.quad.c[0], before.c[0]) && samePoint(s.quad.c[1], before.c[1]) &&
              samePoint(s.quad.c[3], before.c[3]),
          "gesture: in Perspective mode a corner moves alone -- which is the whole feature");

    check(cropHandleAt(s.quad, CropMode::Perspective, 400.0f, 500.0f, 6.0f) == 2,
          "gesture: the hit-test finds the handle the draw code drew, because both walk "
          "cropHandlePoints()");
    check(cropHandleAt(s.quad, CropMode::Perspective, 400.0f, 500.0f, 6.0f) ==
              cropHandleAt(s.quad, CropMode::Rectangle, 400.0f, 500.0f, 6.0f),
          "gesture: a corner is found in both modes; only the four edge handles are "
          "mode-dependent");
    check(cropHandleAt(s.quad, CropMode::Perspective, -900.0f, -900.0f, 6.0f) == -1,
          "gesture: and a pointer nowhere near a handle grabs nothing");

    // Perspective -> Rectangle is the tool's one lossy step, and it is the
    // bounding box rather than a silent keep of four corners the shape no
    // longer has.
    cropSetMode(s, CropMode::Rectangle);
    check(s.quad.c[0].x == 30.0f && s.quad.c[1].x == 400.0f && s.quad.c[0].y == 40.0f &&
              s.quad.c[2].y == 500.0f,
          "gesture: switching back to Rectangle snaps to the quad's bounding box -- lossy, "
          "and the combo's tooltip says so rather than the shape changing silently");

    cropCancel(s);
    check(!s.active && !s.defining && s.doc == 0u,
          "gesture: cancel clears the whole session, including the document it was drawn on");
  }
  {
    // The session's own commit, and the DocumentId earning its place.
    OpenDocument od = makeBlankOpenDocument(80, 60, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 0, 0, 80, 60);
    od.recordEdit("fill fixture", EditKind::Content);

    CropSession s;
    check(!applyCropSession(s, od).ok,
          "commit: an inactive session refuses rather than cropping to a rectangle nobody "
          "drew");

    s.active = true;
    s.mode = CropMode::Rectangle;
    s.doc = od.id + 1;  // a rectangle drawn on another tab
    s.quad = cropQuadFromRegion(DocumentRegion{10, 10, 40u, 30u});
    const DocumentTransformResult wrongDoc = applyCropSession(s, od);
    check(!wrongDoc.ok && wrongDoc.error.find("different document") != std::string::npos,
          "commit: a session drawn on another document is refused BY NAME -- a rectangle in "
          "one document's texels is meaningless in another's");
    check(od.document.width == 80u,
          "commit: and that refusal really left the document alone");

    s.doc = od.id;
    check(applyCropSession(s, od).ok && od.document.width == 40u && od.document.height == 30u,
          "commit: the same session on its own document crops");
    check(!s.active && s.doc == 0u,
          "commit: and a successful commit clears the session, so Enter twice cannot crop "
          "twice");
  }
  {
    // A pending crop does not survive a tool change (app/ToolSwitch).
    AppState st;
    setActiveTool(st, Tool::Crop);
    st.crop.active = true;
    st.crop.doc = 3u;
    st.crop.quad = cropQuadFromRegion(DocumentRegion{5, 5, 20u, 20u});
    setActiveTool(st, Tool::Brush);
    check(!st.crop.active && st.crop.doc == 0u,
          "tool switch: leaving the crop tool discards the pending rectangle -- the polygon "
          "lasso's rule, with a destructive commit behind it");

    // Re-picking the tool that is already selected is not a switch and must not
    // throw away a rectangle the user is in the middle of adjusting.
    setActiveTool(st, Tool::Crop);
    st.crop.active = true;
    st.crop.doc = 3u;
    setActiveTool(st, Tool::Crop);
    check(st.crop.active,
          "tool switch: but re-picking Crop while Crop is selected keeps it -- that is not a "
          "switch, and the palette, the flyout and the menu can all deliver one");
  }

  std::printf("[selftest] crop tool %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
