#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "color/Space.hpp"
#include "paint/Palette.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The eyedropper (PRD Q10, P0), the foreground colour it picks into (PRD L4,
// P0), and the tripwire that would have caught the fact that it did not exist.
//
// **What this section is really about.** `core/Probe`'s `probePixel()` has been
// a complete, tested sampling engine since Phase 2 step 10, with nineteen
// assertions of its own in `runProbeTest()` -- and nothing outside `--selftest`
// ever called it. Meanwhile `ui/AtelierChrome`'s `kToolMeta` marked
// `Tool::Eyedropper` as `implemented = true`, which made the palette cell
// clickable and highlighted, and made `toolCursorOnTarget()` withhold the
// `Refuse` cursor and hand out a bespoke `ToolCursor::Sample` pointer. Every
// tier of the chrome said the tool was live except the one that consumes the
// click, where `Tool::Eyedropper` appeared **nowhere at all** outside
// `drawToolIcon()`'s fallback art.
//
// So this section asserts three separable things, and the third is the one that
// stops this recurring:
//
//   1. The sampling engine answers the three questions a user can ask of it --
//      current layer / current and below / all layers -- with three *different*
//      hand-computed colours, over a fixture built so they must differ; and it
//      averages the box it says it averages, including at a document edge.
//   2. A pick lands in a real foreground colour that a real stroke reads.
//   3. `toolImplemented()` and `toolHasCanvasHandler()` agree for every `Tool`,
//      with `Tool::Zoom`'s identical live defect recorded as a named exception
//      that the day it is fixed forces this file to be edited.
// ---------------------------------------------------------------------------
bool runEyedropperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // **kExact (0)** is used wherever every fixture value is a small dyadic
  // rational -- 0, 0.5, 1 and the products of those -- and the whole
  // premultiplied chain (store as binary16, read back as float, scale by an
  // exact coverage, `src + dst*(1-src.a)`, divide by an alpha of exactly 1)
  // lands back on the float grid with no rounding at all. This is
  // `runLayerStackTest()`'s own discipline and its own reasoning; the
  // three-source block below is built to qualify for it, because "these two
  // modes return different colours" is only worth asserting at the strength
  // where "identical" would have been bit-identical.
  constexpr float kExact = 0.0f;

  // **kBoxTol (1e-4)** is for the box averages, where the sum is genuinely
  // inexact. Naive summation of `n` non-negative floats has an error bound of
  // `(n-1) * u * sum(|x|)` with `u = 2^-24 = 5.96e-8`. The widest box asserted
  // here is 11x11 = 121 texels of values below 1.0, so `sum(|x|) < 121` and the
  // bound is `120 * 5.96e-8 * 121 = 8.7e-4`. That is the pessimal bound with
  // every rounding in the same direction; 1e-4 is a little under it and is what
  // the fixture's actual values (mostly exact halves and zeros) leave enormous
  // room under. Every *difference* this section asserts between two box sizes
  // is at least 0.03, i.e. 300x this tolerance, so no assertion here is
  // deciding anything on a knife edge.
  constexpr float kBoxTol = 1.0e-4f;

  // **kSrgbTol (2e-6)** is for the sRGB encode/decode round trip -- one
  // `srgbEncode()` followed by one `srgbDecode()`, both correctly-rounded
  // `powf`-based curves. Measured in this repo's own colour section at well
  // under 1e-6; doubled for headroom, exactly as runProbeTest() doubles its
  // own.
  constexpr float kSrgbTol = 2.0e-6f;

  // A document holds *premultiplied* halves, so a fixture writes straight
  // values through the same `rgb *= a` io/ImageIO.cpp performs on import.
  // Same helper, same wording, as runLayerStackTest()'s.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto addRgbLayer = [](Document& doc, std::string name) {
    Layer l;
    l.kind = LayerKind::RGB;
    l.rgbTiles.emplace();
    l.name = std::move(name);
    doc.layers.push_back(std::move(l));
  };

  // =======================================================================
  // 1. Three sample sources, three different colours
  // =======================================================================
  //
  // The fixture is built so the three modes CANNOT agree, which is the only
  // way this assertion means anything: a stack where "current" and "and below"
  // happen to coincide would pass just as happily against an implementation
  // where "and below" is a synonym for one of the other two.
  //
  //   layer 2  "top"     blue  (0,0,1) a=1  visible,  opacity 0.5
  //   layer 1  "middle"  green (0,1,0) a=1  HIDDEN,   opacity 1.0   <-- ACTIVE
  //   layer 0  "bottom"  red   (1,0,0) a=1  visible,  opacity 1.0
  //
  // Hand-computed, in premultiplied linear light, at the probed texel:
  //
  //   CurrentLayer   -> layer 1's own stored colour, `visible` and `opacity`
  //                     deliberately ignored                 = (0, 1, 0, 1)
  //   ActiveAndBelow -> layers 0..1 composited. Layer 1 is hidden, so its
  //                     coverage is 0 and it contributes exactly nothing;
  //                     layer 0 is opaque red                = (1, 0, 0, 1)
  //   AllLayers      -> the above, then layer 2 at coverage 0.5:
  //                       src = (0,0,1,1) * 0.5      = (0, 0, 0.5, 0.5)
  //                       acc = src + acc*(1 - 0.5)  = (0.5, 0, 0.5, 1.0)
  //                     un-premultiplied by alpha 1  = (0.5, 0, 0.5, 1)
  //
  // Red, green and purple: three values no two of which are within 0.5 of each
  // other in any channel, and all three exactly representable, so these are
  // asserted at kExact.
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});  // layer 0, RGB
    addRgbLayer(doc, "middle");
    addRgbLayer(doc, "top");
    writeStraight(doc, 0, 2, 2, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(doc, 1, 2, 2, 0.0f, 1.0f, 0.0f, 1.0f);
    writeStraight(doc, 2, 2, 2, 0.0f, 0.0f, 1.0f, 1.0f);
    doc.layers[1].visible = false;   // the hidden one, and the active one
    doc.layers[2].opacity = 0.5f;    // the non-100%-opacity one

    ProbeParams p;
    p.activeLayerIndex = 1;

    p.source = ProbeSource::CurrentLayer;
    const ProbeSample cur = probePixel(doc, PixelCoord{2, 2}, p);
    p.source = ProbeSource::ActiveAndBelow;
    const ProbeSample below = probePixel(doc, PixelCoord{2, 2}, p);
    p.source = ProbeSource::AllLayers;
    const ProbeSample all = probePixel(doc, PixelCoord{2, 2}, p);

    check(near(cur.linear[0], 0.0f, kExact) && near(cur.linear[1], 1.0f, kExact) &&
              near(cur.linear[2], 0.0f, kExact) && near(cur.linear[3], 1.0f, kExact),
          "eyedropper: CurrentLayer returns the active layer's own green, hand-computed");
    check(near(below.linear[0], 1.0f, kExact) && near(below.linear[1], 0.0f, kExact) &&
              near(below.linear[2], 0.0f, kExact) && near(below.linear[3], 1.0f, kExact),
          "eyedropper: ActiveAndBelow returns the red beneath it -- the layers above the "
          "active one are not in the composite at all");
    check(near(all.linear[0], 0.5f, kExact) && near(all.linear[1], 0.0f, kExact) &&
              near(all.linear[2], 0.5f, kExact) && near(all.linear[3], 1.0f, kExact),
          "eyedropper: AllLayers returns red under 50%-opacity blue = (0.5, 0, 0.5), the "
          "hand-computed `over` of the whole stack");
    // The discrimination claim, stated separately from the three values: if any
    // two of these coincided, one of the three assertions above would be
    // checking the wrong mode's arithmetic and still passing.
    check(cur.linear != below.linear && below.linear != all.linear && cur.linear != all.linear,
          "eyedropper: the three sources return three DIFFERENT colours on this fixture, so "
          "no two of them can be the same code path");

    // The asymmetry `ProbeSource` documents, asserted as a rule rather than as
    // a consequence of the numbers above. A hidden layer must stay probeable in
    // CurrentLayer mode -- that is the workflow (a layer hidden while being
    // worked on) the whole split exists for -- and must contribute nothing in
    // either compositing mode, because those ask what a stack SHOWS.
    check(doc.layers[1].visible == false && near(cur.linear[1], 1.0f, kExact) &&
              near(below.linear[1], 0.0f, kExact) && near(all.linear[1], 0.0f, kExact),
          "eyedropper: a HIDDEN layer is still fully probeable in CurrentLayer mode and "
          "contributes exactly nothing to either compositing mode");

    // ActiveAndBelow honours `opacity` too, which is the half of the rule the
    // hidden layer cannot demonstrate: pointing it at the top layer must
    // reproduce AllLayers exactly, 50% coverage and all.
    p.source = ProbeSource::ActiveAndBelow;
    p.activeLayerIndex = 2;
    const ProbeSample belowTop = probePixel(doc, PixelCoord{2, 2}, p);
    check(belowTop.linear == all.linear,
          "eyedropper: ActiveAndBelow at the TOP layer is bit-identical to AllLayers -- it "
          "honours visible and opacity exactly as AllLayers does, not as CurrentLayer does");

    // And pointed at the bottom layer it is the bottom layer alone.
    p.activeLayerIndex = 0;
    const ProbeSample belowBottom = probePixel(doc, PixelCoord{2, 2}, p);
    check(near(belowBottom.linear[0], 1.0f, kExact) && near(belowBottom.linear[2], 0.0f, kExact),
          "eyedropper: ActiveAndBelow at the BOTTOM layer sees only that layer, so the "
          "truncation is a real bound and not an off-by-one that always includes the top");

    // The default has not moved. `ProbeParams{}` meant "this layer, one texel"
    // before the field was widened and it must still mean that -- forty call
    // sites rely on it and none of them was edited.
    ProbeParams def;
    check(def.source == ProbeSource::CurrentLayer && def.sampleSize == 1 &&
              def.activeLayerIndex == 0,
          "eyedropper: a default ProbeParams still means point-sample the active layer, so "
          "the rename did not silently change what un-edited call sites ask for");
  }

  // =======================================================================
  // 2. Sample size: the box is the box, and the label says which
  // =======================================================================
  //
  // A 16x16 opaque single-layer fixture with three deliberately different
  // channels:
  //
  //   red   = x / 16          a LINEAR ramp. A symmetric box centred on x has
  //                           the same mean as its centre texel, so red is the
  //                           control: it must NOT move as the box grows.
  //   green = (x % 4 == 0)    a spike every fourth column. The mean over a box
  //                           is wildly different from the centre texel, so
  //                           green is what proves the box size is honoured.
  //   blue  = 0.5             constant. A second control.
  //
  // Every expected value below is computed by looping the fixture's own
  // generating expressions over the box -- NOT by calling probePixel() with a
  // different argument and comparing, which would be checking the function
  // against itself.
  {
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    auto red = [](int32_t x) { return static_cast<float>(x) / 16.0f; };
    auto green = [](int32_t x) { return (x % 4 == 0) ? 1.0f : 0.0f; };
    for (int32_t y = 0; y < 16; ++y)
      for (int32_t x = 0; x < 16; ++x) writeStraight(doc, 0, x, y, red(x), green(x), 0.5f, 1.0f);

    // The independent reference: the straight mean over the half-open box
    // [x0,x1) x [y0,y1), computed from the generating expressions.
    auto meanOver = [&](int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
      float r = 0.0f, g = 0.0f;
      int32_t n = 0;
      for (int32_t y = y0; y < y1; ++y)
        for (int32_t x = x0; x < x1; ++x) {
          r += red(x);
          g += green(x);
          ++n;
        }
      const float d = static_cast<float>(n > 0 ? n : 1);
      return std::array<float, 2>{r / d, g / d};
    };

    ProbeParams p;
    p.source = ProbeSource::CurrentLayer;

    p.sampleSize = 1;
    const ProbeSample point = probePixel(doc, PixelCoord{8, 8}, p);
    p.sampleSize = 3;
    const ProbeSample box3 = probePixel(doc, PixelCoord{8, 8}, p);
    p.sampleSize = 11;
    const ProbeSample box11 = probePixel(doc, PixelCoord{8, 8}, p);

    const std::array<float, 2> e1 = meanOver(8, 9, 8, 9);
    const std::array<float, 2> e3 = meanOver(7, 10, 7, 10);
    const std::array<float, 2> e11 = meanOver(3, 14, 3, 14);

    check(near(point.linear[0], e1[0], kBoxTol) && near(point.linear[1], e1[1], kBoxTol),
          "eyedropper: Point Sample returns exactly the one texel under the pointer");
    check(near(box3.linear[0], e3[0], kBoxTol) && near(box3.linear[1], e3[1], kBoxTol),
          "eyedropper: 3 by 3 Average matches the mean computed independently over the nine "
          "texels, not the centre texel and not a different box");
    check(near(box11.linear[0], e11[0], kBoxTol) && near(box11.linear[1], e11[1], kBoxTol),
          "eyedropper: 11 by 11 Average matches the mean computed independently over its 121 "
          "texels");

    // The controls, and the discriminator. Red is linear so it must be
    // untouched by box size (0.5 at all three); green must move a lot. A build
    // that ignored `sampleSize` entirely would pass the red check and fail this
    // one, which is exactly the split these two channels are for.
    check(near(point.linear[0], 0.5f, kBoxTol) && near(box3.linear[0], 0.5f, kBoxTol) &&
              near(box11.linear[0], 0.5f, kBoxTol) && near(point.linear[2], 0.5f, kBoxTol) &&
              near(box11.linear[2], 0.5f, kBoxTol),
          "eyedropper: a linear ramp and a constant are unmoved by box size, so the box is "
          "symmetric and centred on the probed texel rather than offset");
    check(std::fabs(point.linear[1] - box3.linear[1]) > 0.03f &&
              std::fabs(box3.linear[1] - box11.linear[1]) > 0.03f,
          "eyedropper: and the spike channel moves by far more than tolerance between all "
          "three sizes, so `sampleSize` is genuinely read rather than ignored");

    // ---- the document edge and the document corner ----------------------
    //
    // An 11x11 box at (0,0) asks for x,y in [-5,5]; only [0,6) x [0,6) is in a
    // 16x16 document. At (15,8) it asks for x in [10,21) and only [10,16) is.
    // Both must average the in-document texels ONLY: the colour is the mean of
    // those, and the alpha is 1.0 rather than 36/121 or 66/121.
    p.sampleSize = 11;
    const ProbeSample corner = probePixel(doc, PixelCoord{0, 0}, p);
    const ProbeSample edge = probePixel(doc, PixelCoord{15, 8}, p);
    const std::array<float, 2> eCorner = meanOver(0, 6, 0, 6);
    const std::array<float, 2> eEdge = meanOver(10, 16, 3, 14);

    check(near(corner.linear[0], eCorner[0], kBoxTol) &&
              near(corner.linear[1], eCorner[1], kBoxTol),
          "eyedropper: an 11 by 11 box at the document CORNER averages only the 6x6 that is "
          "actually in the document, not a box padded with off-canvas texels");
    check(near(edge.linear[0], eEdge[0], kBoxTol) && near(edge.linear[1], eEdge[1], kBoxTol),
          "eyedropper: and at the right EDGE it averages the 6x11 that is in the document");
    // The assertion the un-clipped implementation could not pass. Alpha is
    // where the old bug lived: premultiplied averaging cancels off-canvas zeros
    // out of the COLOUR exactly, so the colour checks above would have passed
    // either way, and only the coverage gave it away.
    check(near(corner.linear[3], 1.0f, kBoxTol) && near(edge.linear[3], 1.0f, kBoxTol),
          "eyedropper: a fully opaque document reads alpha 1.0 at its corner and its edge -- "
          "no off-canvas zeros dragged into the coverage (they would give 36/121 and 66/121)");

    // The box itself, stated independently of what was averaged in it, so a
    // failure says which half is wrong.
    const ProbeBox bc = probeSampleBox(doc, PixelCoord{0, 0}, 11);
    const ProbeBox be = probeSampleBox(doc, PixelCoord{15, 8}, 11);
    const ProbeBox bi = probeSampleBox(doc, PixelCoord{8, 8}, 11);
    check(bc.x0 == 0 && bc.x1 == 6 && bc.y0 == 0 && bc.y1 == 6 && bc.texels() == 36 &&
              be.x0 == 10 && be.x1 == 16 && be.y0 == 3 && be.y1 == 14 && be.texels() == 66 &&
              bi.x0 == 3 && bi.x1 == 14 && bi.y0 == 3 && bi.y1 == 14 && bi.texels() == 121,
          "eyedropper: probeSampleBox() clips to the document at the corner and the edge and "
          "leaves an interior box whole, with the texel counts to match");
    // Wholly outside is empty, not negative and not wrapped -- the case that
    // would divide by zero if the count were used unchecked.
    const ProbeBox bOut = probeSampleBox(doc, PixelCoord{-100, -100}, 3);
    check(bOut.texels() == 0 && probePixel(doc, PixelCoord{-100, -100}, p).linear[3] == 0.0f,
          "eyedropper: a box entirely off the document is empty, and probing there is "
          "transparent black rather than a divide by zero");

    // ---- unpainted-but-IN-document texels still dilute coverage ---------
    //
    // This is the property runProbeTest()'s old `4.0f / 9.0f` assertion was
    // really after, moved here and put on a fixture where it is actually true:
    // a 3x3 box wholly inside a 16x16 document, of which only four texels were
    // ever painted. Clipping to the document must NOT be confused with skipping
    // unpainted texels -- the first is "not part of the image", the second is
    // "transparent paper", and only the second belongs in the average.
    Document sparse = Document::createBlank(16, 16, WorkingSpace{});
    writeStraight(sparse, 0, 5, 5, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(sparse, 0, 6, 5, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(sparse, 0, 5, 6, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(sparse, 0, 6, 6, 1.0f, 0.0f, 0.0f, 1.0f);
    ProbeParams sp;
    sp.sampleSize = 3;
    const ProbeSample sq = probePixel(sparse, PixelCoord{5, 5}, sp);
    check(near(sq.linear[0], 1.0f, kBoxTol) && near(sq.linear[3], 4.0f / 9.0f, kBoxTol),
          "eyedropper: unpainted texels INSIDE the document still dilute coverage (4 of 9) "
          "without darkening the colour -- clipping to the canvas is not the same rule");
  }

  // =======================================================================
  // 3. The sample-size labels say which reading they mean
  // =======================================================================
  //
  // The user described these sizes as areas -- "1 px, 9px, etc." -- and the
  // field is an EDGE. "9" is 3x3. Every label spells the box out so that
  // ambiguity cannot survive contact with the menu.
  {
    bool everyLabelSaysTheBox = true;
    for (int i = 0; i < kProbeSampleSizeCount; ++i) {
      const char* label = probeSampleSizeLabel(kProbeSampleSizes[i]);
      if (label == nullptr || label != kProbeSampleSizeLabels[i]) everyLabelSaysTheBox = false;
      // Every size but 1 must name both edges; 1 is "Point Sample", which is
      // unambiguous by construction.
      const std::string s = label != nullptr ? label : "";
      if (kProbeSampleSizes[i] != 1 && s.find(" by ") == std::string::npos)
        everyLabelSaysTheBox = false;
    }
    check(everyLabelSaysTheBox,
          "eyedropper: every sample size has Photoshop's own label and every size above 1 "
          "spells the box out (\"3 by 3 Average\"), so no reader has to guess edge vs area");
    check(kProbeSampleSizeCount == 7 && kProbeSampleSizes[0] == 1 &&
              kProbeSampleSizes[kProbeSampleSizeCount - 1] == 101,
          "eyedropper: the set is Photoshop's seven, 1 through 101 by edge length");
    check(probeSampleSizeLabel(9) == nullptr && probeSampleSizeLabel(0) == nullptr,
          "eyedropper: 9 is NOT one of the sizes -- the area reading of \"9px\" has no label, "
          "so it cannot silently be offered as one");
  }

  // =======================================================================
  // 4. A pick lands in a real foreground colour, and that colour paints
  // =======================================================================
  //
  // The blocker this whole track had to clear first: `BrushState::pigment` is
  // an index into `defaultPalette()` and a sampled colour is an arbitrary
  // triple. `BrushState` now carries both, plus a `ColorMode` saying which one
  // is live, and `foregroundSrgb()` is the single answer every consumer reads.
  //
  // Nothing below re-reads `probePixel()` to check the result: the picked
  // colour is asserted through the foreground structure itself and then through
  // `brushTipFor()`, which is what actually paints.
  {
    AppState st;
    OpenDocument od;
    od.document = Document::createBlank(8, 8, WorkingSpace{});
    // A colour with all three channels distinct and none of them 0 or 1, so a
    // channel swap, a missing decode and a stuck channel are all visible.
    writeStraight(od.document, 0, 3, 3, 0.25f, 0.5f, 0.75f, 1.0f);
    od.activeLayer = 0;
    st.documents.add(std::move(od));

    const int pigmentBefore = st.brush.pigment;
    check(st.brush.colorMode == ColorMode::Pigment,
          "eyedropper: the COLOR panel starts in PIGMENT mode, which is the mode a pick has "
          "to have an answer for");

    const EyedropperPick pick = applyEyedropperPick(st, PixelCoord{3, 3});
    check(pick.applied, "eyedropper: a pick on a painted texel is applied");

    // The foreground is display-referred sRGB, so the stored triple is
    // srgbEncode() of what the document holds -- checked against an
    // independently computed encode of the fixture's own straight values, not
    // against ProbeSample::display.
    check(near(st.brush.rgb[0], srgbEncode(0.25f), kSrgbTol) &&
              near(st.brush.rgb[1], srgbEncode(0.5f), kSrgbTol) &&
              near(st.brush.rgb[2], srgbEncode(0.75f), kSrgbTol),
          "eyedropper: the picked colour lands in BrushState::rgb sRGB-ENCODED -- storing the "
          "linear value in an sRGB field would repaint every pick far too dark");
    check(foregroundSrgb(st.brush) == st.brush.rgb,
          "eyedropper: and foregroundSrgb() -- the one answer every consumer reads -- returns "
          "exactly it");

    // And back to linear, which is what any document write needs. Round trip
    // through the two curves must land on the fixture's own values.
    const std::array<float, 4> lin = foregroundLinearRgba(st.brush);
    check(near(lin[0], 0.25f, kSrgbTol) && near(lin[1], 0.5f, kSrgbTol) &&
              near(lin[2], 0.75f, kSrgbTol) && lin[3] == 1.0f,
          "eyedropper: foregroundLinearRgba() decodes it back to the document's own linear "
          "values, so the bucket and the gradient fill with the colour that was picked");

    // **The pigment-mode decision, asserted explicitly.** The mode moves; the
    // pigment selection does not; the physical constants keep coming from it;
    // and the report says all of that.
    check(pick.switchedToRgbMode && st.brush.colorMode == ColorMode::Rgb,
          "eyedropper: picking in PIGMENT mode switches the panel to RGB mode -- a sampled "
          "triple has no density, staining or granulation to select");
    check(st.brush.pigment == pigmentBefore &&
              &foregroundPhysicalConstants(st.brush) == &defaultPalette()[pigmentBefore],
          "eyedropper: the pigment SELECTION survives the pick, so the three physical "
          "constants still come from it and switching back to PIGMENT restores that paint");
    check(pick.report.find("RGB mode") != std::string::npos &&
              pick.report.find("granulation") != std::string::npos &&
              st.lastPickReport == pick.report,
          "eyedropper: and the mode change is SAID -- the options bar reports it by name "
          "rather than letting the panel change under the user in silence");
    check(std::string(foregroundName(st.brush)) == "Custom RGB",
          "eyedropper: the foreground well stops claiming a pigment name it is no longer "
          "showing");

    // A second pick, already in RGB mode, must not claim to have switched.
    const EyedropperPick again = applyEyedropperPick(st, PixelCoord{3, 3});
    check(again.applied && !again.switchedToRgbMode &&
              again.report.find("RGB mode") == std::string::npos,
          "eyedropper: a pick already in RGB mode reports a plain pick, so the mode-change "
          "sentence means a mode actually changed");

    // **Does the picked colour reach a stroke?** This is the assertion the
    // whole track turns on: a picker that appears to work and paints the old
    // colour would be one silent no-op traded for another.
    MixboxLut noLut;  // no file is read here; the latent half falls back, the linear half does not
    const BrushTip tip = brushTipFor(st.brush, noLut, 1.0f);
    check(near(tip.linearRgb[0], lin[0], kExact) && near(tip.linearRgb[1], lin[1], kExact) &&
              near(tip.linearRgb[2], lin[2], kExact),
          "eyedropper: brushTipFor()'s linear RGB is bit-identical to the picked foreground, "
          "so an RGB-layer stroke deposits the colour that was picked");
    // And it is genuinely NOT the pigment it would have deposited before.
    const std::array<float, 4> oldPigment = foregroundLinearRgba(pigmentBefore);
    check(std::fabs(tip.linearRgb[0] - oldPigment[0]) > 0.01f ||
              std::fabs(tip.linearRgb[1] - oldPigment[1]) > 0.01f ||
              std::fabs(tip.linearRgb[2] - oldPigment[2]) > 0.01f,
          "eyedropper: and it genuinely differs from the pigment that was selected before the "
          "pick, so this cannot pass on a tip that ignored the foreground");
    // The latent half, without a LUT, falls back to the sRGB triple -- which
    // must still be the PICKED triple rather than the palette's.
    check(near(tip.pigment.c[0], st.brush.rgb[0], kExact) &&
              near(tip.pigment.c[2], st.brush.rgb[2], kExact),
          "eyedropper: the Latent the Pigment-layer route deposits is derived from the picked "
          "colour too, so both layer kinds see one foreground rather than two");

    // ---- what a pick on nothing does ------------------------------------
    //
    // Transparent means "there is nothing here", not "paint in black". Writing
    // {0,0,0} into the foreground would destroy a colour the user chose in
    // exchange for a value they cannot have meant to pick.
    const std::array<float, 3> before = st.brush.rgb;
    const EyedropperPick empty = applyEyedropperPick(st, PixelCoord{7, 7});
    check(!empty.applied && st.brush.rgb == before &&
              empty.report.find("Nothing to sample") != std::string::npos,
          "eyedropper: a pick on a transparent texel is refused OUT LOUD and leaves the "
          "foreground exactly as it was, rather than setting it to black");

    // No document at all is the other refusal, and it is a sentence too.
    AppState bare;
    const EyedropperPick none = applyEyedropperPick(bare, PixelCoord{0, 0});
    check(!none.applied && none.report.find("no document is open") != std::string::npos,
          "eyedropper: with no document open the pick refuses by name rather than doing "
          "nothing");
  }

  // =======================================================================
  // 5. The pick honours the tool's own settings, end to end
  // =======================================================================
  //
  // Section 1 proved `probePixel()` distinguishes the three sources; this
  // proves `applyEyedropperPick()` actually passes `st.eyedropper` through to
  // it rather than sampling with defaults. Same fixture shape as section 1.
  //
  // **The active layer is HIDDEN, and it has to be for this to prove anything.**
  // As first written this fixture had an opaque green active layer over an
  // opaque red one, and asserted that `ActiveAndBelow` answered red -- but
  // "and below" composites the stack up to *and including* the active layer,
  // so an opaque green on top makes the honest answer green. The assertion was
  // wrong, not the code, and it could only ever have failed. Hiding the green
  // layer restores the property the section actually wants: `CurrentLayer`
  // still reads it (core/Probe.hpp's deliberate asymmetry -- a hidden layer
  // stays probeable for what it *holds*), while both compositing modes skip it
  // and see the red beneath. Now the two answers genuinely differ, and they
  // differ for the reason the SOURCE setting exists.
  {
    AppState st;
    OpenDocument od;
    od.document = Document::createBlank(8, 8, WorkingSpace{});
    Layer mid;
    mid.kind = LayerKind::RGB;
    mid.rgbTiles.emplace();
    mid.name = "middle";
    mid.visible = false;
    od.document.layers.push_back(std::move(mid));
    writeStraight(od.document, 0, 2, 2, 1.0f, 0.0f, 0.0f, 1.0f);  // red below
    writeStraight(od.document, 1, 2, 2, 0.0f, 1.0f, 0.0f, 1.0f);  // green above
    od.activeLayer = 1;
    st.documents.add(std::move(od));

    st.eyedropper.source = ProbeSource::CurrentLayer;
    applyEyedropperPick(st, PixelCoord{2, 2});
    const std::array<float, 4> gotCurrent = foregroundLinearRgba(st.brush);

    st.eyedropper.source = ProbeSource::ActiveAndBelow;
    st.brush.pigment = 0;
    applyEyedropperPick(st, PixelCoord{2, 2});
    const std::array<float, 4> gotBelow = foregroundLinearRgba(st.brush);

    check(near(gotCurrent[1], 1.0f, kSrgbTol) && near(gotCurrent[0], 0.0f, kSrgbTol) &&
              near(gotBelow[0], 1.0f, kSrgbTol) && near(gotBelow[1], 0.0f, kSrgbTol),
          "eyedropper: the SOURCE setting reaches the sample -- the same click picks green "
          "in Current Layer and red in Current & Below");

    // And the SIZE setting: a 3x3 over a half-red, half-green pair must land
    // between them rather than on either.
    st.eyedropper.source = ProbeSource::CurrentLayer;
    st.eyedropper.sampleSize = 3;
    OpenDocument* live = st.documents.active();
    writeStraight(live->document, 1, 1, 2, 0.0f, 0.0f, 0.0f, 1.0f);
    applyEyedropperPick(st, PixelCoord{2, 2});
    const std::array<float, 4> got3 = foregroundLinearRgba(st.brush);
    // Two painted texels of nine: green at (2,2) and black at (1,2). Colour is
    // their premultiplied mean un-premultiplied = (0, 0.5, 0), coverage 2/9.
    // Only the colour is asserted here -- the foreground has no alpha.
    check(near(got3[1], 0.5f, 0.01f) && got3[1] < 1.0f,
          "eyedropper: the SIZE setting reaches the sample too -- a 3 by 3 over one green and "
          "one black texel picks the average, not either one");
  }

  // =======================================================================
  // 6. The tripwire: implemented == has a canvas handler
  // =======================================================================
  //
  // `toolImplemented()` is a hand-written boolean in a table. It said `true`
  // about the eyedropper for two phases while no canvas block anywhere would
  // act on an eyedropper click, and nothing in the build could tell -- the
  // cursor layer went as far as *withholding* the refusal cursor because of the
  // flag. This is what makes that class of defect a red test instead of a ship.
  {
    bool agreeing = true;
    int exceptions = 0;
    bool everyExceptionIsImplemented = true;
    bool everyExceptionStillHasNoHandler = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const bool implemented = toolImplemented(t);
      const bool handled = toolHasCanvasHandler(t);
      const char* why = toolNoHandlerException(t);
      if (why != nullptr) {
        ++exceptions;
        if (!implemented) everyExceptionIsImplemented = false;
        // The forcing function: an exception that has quietly acquired a
        // handler must be DELETED, and this is what makes forgetting to
        // impossible.
        if (handled) everyExceptionStillHasNoHandler = false;
      }
      if (implemented != (handled || why != nullptr)) agreeing = false;
    }
    check(agreeing,
          "tool table: every Tool marked implemented has a canvas handler or a recorded "
          "exception, and no unimplemented tool has one -- the check the eyedropper's two "
          "silent phases needed");
    check(toolHasCanvasHandler(Tool::Eyedropper) && toolSamplesCanvas(Tool::Eyedropper) &&
              toolNoHandlerException(Tool::Eyedropper) == nullptr,
          "tool table: the eyedropper now HAS a handler, through the same predicate the canvas "
          "block is gated on, and needs no exception");
    check(exceptions == 1 && toolNoHandlerException(Tool::Zoom) != nullptr &&
              everyExceptionIsImplemented && everyExceptionStillHasNoHandler,
          "tool table: exactly one recorded exception -- Tool::Zoom, implemented=true with a "
          "bespoke cursor and no canvas handler, wheel-and-menu only. Recorded deliberately, "
          "not fixed; delete the row the day the Zoom tool lands and this goes red");
    check(!toolHasCanvasHandler(Tool::Zoom) && !toolPansView(Tool::Zoom) &&
              !toolBeginsStroke(Tool::Zoom) && !toolDrawsSelection(Tool::Zoom),
          "tool table: and Zoom's exception is checked against reality every run rather than "
          "taken on trust -- none of the five gates admits it");

    // The five predicates are the canvas's own gates, so a few spot answers
    // pin them to the blocks they gate rather than to this file's opinion.
    check(toolBeginsStroke(Tool::Brush) && toolBeginsStroke(Tool::Eraser) &&
              !toolBeginsStroke(Tool::Eyedropper) && !toolBeginsStroke(Tool::Pencil),
          "tool table: toolBeginsStroke() is strokeRouteFor()'s own answer, probed rather "
          "than restated -- the eraser counts, the eyedropper and the pencil do not");
    check(toolDrawsSelection(Tool::Marquee) && toolDrawsSelection(Tool::MagicWand) &&
              !toolDrawsSelection(Tool::Crop) && toolPansView(Tool::Hand) &&
              !toolSamplesCanvas(Tool::Measure),
          "tool table: the selection, pan and sample gates name exactly the tools their "
          "canvas blocks act on -- Measure shares the eyedropper's cursor but has no handler");
  }

  std::printf("[selftest] eyedropper %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
