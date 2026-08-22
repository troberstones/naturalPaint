#include "app/selftest/Support.hpp"

namespace np {

bool runClippingMaskTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, and why almost everything here is at exactly zero -----
  //
  // A clip stores nothing. The only arithmetic it adds is the open/close
  // bracket -- one divide and one multiply by the base's alpha -- so the
  // fixtures are chosen to make even that exact: **every base alpha used for
  // a numeric claim is 1.0 or 0.5**, every coverage and mask value is dyadic
  // (0, 0.25, 0.5, 0.75, 1), and every colour component is a dyadic rational.
  // Under those conditions the divide and the multiply are both exact, so the
  // references below are exact float expressions compared with `==`, and the
  // identity claims are `memcmp`.
  //
  // **One tolerance is used and it is derived rather than borrowed**: the
  // flattener's own final un-premultiply is one correctly-rounded division, so
  // half an ulp at results in [0.25, 1) is 2^-25 = 2.98e-8. Bounded at 1.0e-7,
  // a 3.4x margin -- the identical derivation runLayerStackTest(),
  // runBlendTest(), runPigmentLayerTest(), runLayerMaskTest() and
  // runAdjustmentLayerTest() each restate for themselves. The largest residual
  // actually observed is measured and printed beside it below, so the margin
  // is a fact rather than a hope.
  //
  // The **one** place a non-dyadic base alpha is used on purpose is §9's
  // proof that the bracket is not a round trip; there the claim is that
  // open-then-close is NOT bit-exact, and it is asserted as an inequality.
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]};
  };
  auto sameImage = [](const DecodedImage& a, const DecodedImage& b) {
    return a.pixels.size() == b.pixels.size() && !a.pixels.empty() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0;
  };
  auto samePixel = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return std::memcmp(a.data(), b.data(), sizeof(float) * 4) == 0;
  };
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto writeMask = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float v) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].mask->getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), v);
  };
  auto writePigment = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, const Latent& z,
                         float mass) {
    const PixelCoord at{x, y};
    PigmentTexel t;
    t.latent = z;
    t.mass = mass;
    doc.layers[layerIndex].pigmentTiles->getOrCreate(tileCoordAt(at))
        .writeTexel(tileLocalOffset(at), t);
  };
  // Exposure at +1 stop: an exact doubling of straight linear RGB, the same
  // op runAdjustmentLayerTest() uses and for the same reason.
  auto exposureOp = [](float stops) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = stops;
    return op;
  };
  // An RGB layer holding one premultiplied texel at (x,y), added on top.
  auto addRgbLayer = [&](Document& doc, const char* name, int32_t x, int32_t y,
                         const std::array<float, 4>& texel) {
    addLayer(doc, doc.layers.size(), makeRgbLayer(name));
    writeRgb(doc, doc.layers.size() - 1, x, y, texel);
  };

  // --- 1. The model: `Layer::clipped`, and a RUN clipping to ONE base -----
  {
    const Layer fresh = makeRgbLayer("plain");
    check(!fresh.clipped,
          "model: a new layer is not clipped -- the flag's default is the state every "
          "`.npaint` written before this step implies, so absence and false are one thing");
    Layer c = fresh;
    c.clipped = true;
    check(layerRowSubLine(c) == "RGB \xC2\xB7 NORMAL \xC2\xB7 100% \xC2\xB7 CLIPPED" &&
              layerRowSubLine(fresh) == "RGB \xC2\xB7 NORMAL \xC2\xB7 100%",
          "model: docs/ui.md §3.2's `CLIPPED` marker, which that document's own example row "
          "assumed four steps before the feature existed -- and an unclipped row is unchanged");
    Layer adj = makeAdjustmentLayer("Curves 1");
    adj.clipped = true;
    check(contains(layerRowSubLine(adj), "ADJUSTMENT") && contains(layerRowSubLine(adj), "CLIPPED"),
          "model: so docs/ui.md's literal `ADJUSTMENT \xC2\xB7 CLIPPED` row is now producible");

    // [base, c1, c2, c3]: ONE base for the whole run.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    for (const char* n : {"c1", "c2", "c3"}) addLayer(doc, doc.layers.size(), makeRgbLayer(n));
    check(setLayerClipped(doc, 1, true).ok && setLayerClipped(doc, 2, true).ok &&
              setLayerClipped(doc, 3, true).ok,
          "model: three consecutive layers are clipped, each accepted -- a clipped layer above "
          "another clipped layer is a legal run, not an error");
    ClipRuns runs = clipRuns(doc);
    check(runs.any && runs.members[0].size() == 3 && runs.members[0][0] == 1 &&
              runs.members[0][1] == 2 && runs.members[0][2] == 3 && runs.members[1].empty() &&
              runs.members[2].empty() && runs.members[3].empty(),
          "model: **all three clip to layer 0** and NONE of them is another's base -- the one "
          "line that separates this from the cumulative reading (core/Composite.hpp §12)");
    check(runs.clippedToBase[1] && runs.clippedToBase[2] && runs.clippedToBase[3] &&
              !runs.clippedToBase[0] && !runs.clippedWithoutBase[1],
          "model: and all three are composited BY that base rather than on their own, the same "
          "relationship `MixPairing::consumedByAbove` expresses for a mixed pair");

    // Two runs, so the loop is proven to reset rather than to accumulate.
    Document two = Document::createBlank(8, 8, WorkingSpace{});
    for (const char* n : {"c1", "b2", "c3"}) addLayer(two, two.layers.size(), makeRgbLayer(n));
    setLayerClipped(two, 1, true);
    setLayerClipped(two, 3, true);
    const ClipRuns twoRuns = clipRuns(two);
    check(twoRuns.members[0].size() == 1 && twoRuns.members[0][0] == 1 &&
              twoRuns.members[2].size() == 1 && twoRuns.members[2][0] == 3 &&
              twoRuns.members[1].empty(),
          "model: [b,c,b,c] resolves to two separate runs -- the running base advances at every "
          "UNCLIPPED layer, which is what makes the previous assertion true and this one too");

    Document none = Document::createBlank(8, 8, WorkingSpace{});
    addLayer(none, 1, makeRgbLayer("second"));
    const ClipRuns noRuns = clipRuns(none);
    check(!noRuns.any && noRuns.members[0].empty() && noRuns.members[1].empty() &&
              !noRuns.clippedToBase[0] && !noRuns.clippedToBase[1],
          "model: a document with no clipped layer reports `any == false` and empty runs, which "
          "is what lets the walk take byte-for-byte its pre-step-9 path");
  }

  // --- 2. A run clips to one base, in PIXELS, not just in bookkeeping ----
  //
  // The fixture is built so the two readings could not be confused: three
  // clipped layers each covering a DIFFERENT texel of an opaque base. Under
  // the correct reading all three show. Under the cumulative reading -- each
  // clipped layer masked by the alpha of the layer directly below it -- the
  // second is confined to the first's single texel and the third to the
  // second's, so two of the three vanish.
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    for (int32_t x = 0; x < 3; ++x) writeRgb(doc, 0, x, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    addRgbLayer(doc, "red", 0, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    addRgbLayer(doc, "green", 1, 0, {0.0f, 1.0f, 0.0f, 1.0f});
    addRgbLayer(doc, "blue", 2, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    for (size_t i = 1; i <= 3; ++i) setLayerClipped(doc, i, true);

    const DecodedImage flat = flattenDocumentToLinear(doc);
    const std::array<float, 4> a = pixelOf(flat, 0, 0);
    const std::array<float, 4> b = pixelOf(flat, 1, 0);
    const std::array<float, 4> c = pixelOf(flat, 2, 0);
    std::printf("  three clipped layers over one opaque base, at their own texels:\n"
                "    (0,0) %.3f %.3f %.3f   (1,0) %.3f %.3f %.3f   (2,0) %.3f %.3f %.3f\n"
                "    one-base reading: red, green, blue.  cumulative reading would give: "
                "blue, white, white\n",
                static_cast<double>(a[0]), static_cast<double>(a[1]), static_cast<double>(a[2]),
                static_cast<double>(b[0]), static_cast<double>(b[1]), static_cast<double>(b[2]),
                static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2]));
    check(a == std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f} &&
              b == std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f} &&
              c == std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f},
          "run: all three clipped layers show, each at its own texel -- under the cumulative "
          "reading the green and blue layers would be confined to the red one's single texel");

    // The alpha half of the same claim: three OPAQUE clipped layers stacked on
    // a half-covered base leave the coverage at exactly the base's, not eroded
    // and not grown.
    Document alpha = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(alpha, 0, 0, 0, {0.5f, 0.5f, 0.5f, 0.5f});  // straight white, alpha 0.5
    addRgbLayer(alpha, "c1", 0, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    addRgbLayer(alpha, "c2", 0, 0, {0.0f, 1.0f, 0.0f, 1.0f});
    addRgbLayer(alpha, "c3", 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    for (size_t i = 1; i <= 3; ++i) setLayerClipped(alpha, i, true);
    const std::vector<float> pre = compositeDocumentPremultiplied(alpha);
    std::printf("    stacked on a 0.5-alpha base, coverage stays %.6f "
                "(cumulative erosion would give 0.125; independent growth 0.875)\n",
                static_cast<double>(pre[3]));
    check(pre[3] == 0.5f && pre[0] == 0.0f && pre[1] == 0.0f && pre[2] == 0.5f,
          "run: and the coverage is EXACTLY the base's 0.5 -- a clipping group can neither add "
          "coverage nor erode it, whatever its members do (PRD C9 read literally)");
  }

  // --- 3. Which alpha, and where the group lands (the two-part answer) ----
  {
    // The fixture that separates the two readings: an opaque red backdrop, a
    // half-covered white base, an opaque blue clipped layer.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f});          // backdrop: opaque red
    addRgbLayer(doc, "base", 0, 0, {0.5f, 0.5f, 0.5f, 0.5f});  // white at alpha 0.5
    addRgbLayer(doc, "clip", 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});  // opaque blue
    setLayerClipped(doc, 2, true);
    const std::array<float, 4> got = pixelOf(flattenDocumentToLinear(doc), 0, 0);

    // The other reading, written out here rather than described: composite the
    // base into the backdrop, then land the clipped layer on THAT, masked by
    // the base's alpha.
    const std::array<float, 4> backdrop{1.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> afterBase =
        compositeOver({0.5f, 0.5f, 0.5f, 0.5f}, backdrop);
    const std::array<float, 4> independent =
        compositeOver({0.0f, 0.0f, 0.5f, 0.5f}, afterBase);  // blue scaled by the base's alpha
    std::printf("  base half-transparent, one opaque clipped layer over an opaque red backdrop:\n"
                "    group-then-land (built)   %.4f %.4f %.4f  a=%.4f\n"
                "    each-layer-independently  %.4f %.4f %.4f  a=%.4f\n",
                static_cast<double>(got[0]), static_cast<double>(got[1]),
                static_cast<double>(got[2]), static_cast<double>(got[3]),
                static_cast<double>(independent[0]), static_cast<double>(independent[1]),
                static_cast<double>(independent[2]), static_cast<double>(independent[3]));
    check(got == std::array<float, 4>{0.5f, 0.0f, 0.5f, 1.0f},
          "which: the group composites internally and lands through the base -- exact, and it "
          "is NOT what compositing each clipped layer onto the backdrop would give");
    check(!(got[1] == independent[1] && got[2] == independent[2]),
          "which: the two readings really are observably different on this fixture, so the "
          "line above is a choice being tested and not a distinction without a difference");

    // The clipped layer must not paint on the backdrop. Under the independent
    // reading the blue lands partly on the red showing through the base; under
    // this one it cannot, because the group's colour never sees the backdrop.
    check(got[1] == 0.0f && independent[1] > 0.0f,
          "which: the clipped layer does not bleed onto the backdrop -- green is exactly 0 "
          "here and non-zero under the independent reading, which is the red showing through");

    // The base's blend mode applies to the GROUP, not to the base alone.
    Document mul = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(mul, 0, 0, 0, {0.5f, 0.5f, 0.5f, 1.0f});          // opaque mid grey backdrop
    addRgbLayer(mul, "base", 0, 0, {0.0f, 0.0f, 0.0f, 1.0f});  // opaque black
    setLayerBlend(mul, 1, BlendMode::Multiply);
    addRgbLayer(mul, "clip", 0, 0, {1.0f, 1.0f, 1.0f, 1.0f});  // opaque white
    setLayerClipped(mul, 2, true);
    const std::array<float, 4> mulGot = pixelOf(flattenDocumentToLinear(mul), 0, 0);
    std::printf("    base blend=multiply (black) with an opaque white clipped layer over grey:\n"
                "      through the base's blend %.4f   each-layer-independently 1.0000\n",
                static_cast<double>(mulGot[0]));
    check(mulGot[0] == 0.5f && mulGot[1] == 0.5f && mulGot[2] == 0.5f,
          "which: **the base's blend mode acts on the whole group** -- white clipped to a "
          "`multiply` black base multiplies the backdrop, where landing the layers separately "
          "would have put opaque white on screen");

    // Which alpha: the base's EFFECTIVE alpha. Opacity and a mask on the base
    // each fade the whole group, and they are the same scalar.
    Document faded = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(faded, 0, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    addRgbLayer(faded, "clip", 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    setLayerClipped(faded, 1, true);
    Document byOpacity = faded;
    setLayerOpacity(byOpacity, 0, 0.5f);
    Document byMask = faded;
    addLayerMask(byMask, 0);
    writeMask(byMask, 0, 0, 0, 0.5f);
    const std::vector<float> fo = compositeDocumentPremultiplied(byOpacity);
    const std::vector<float> fm = compositeDocumentPremultiplied(byMask);
    check(fo[3] == 0.5f && fo[2] == 0.5f && fo[0] == 0.0f,
          "which: the base's OPACITY fades the whole clipping group -- an opaque base at 50% "
          "with an opaque clipped layer lands at coverage 0.5, not 1.0");
    check(std::memcmp(fo.data(), fm.data(), fo.size() * sizeof(float)) == 0,
          "which: and a 0.5 MASK sample on the base is BYTE-IDENTICAL to 0.5 opacity on it -- "
          "both reach the group as one scalar, which is §5's product rule reaching a clip");

    // Hiding the base hides the group -- bit-for-bit the layers not existing.
    Document hidden = faded;
    setLayerVisible(hidden, 0, false);
    Document without = Document::createBlank(8, 8, WorkingSpace{});
    const std::vector<float> hv = compositeDocumentPremultiplied(hidden);
    const std::vector<float> wv = compositeDocumentPremultiplied(without);
    check(hv.size() == wv.size() &&
              std::memcmp(hv.data(), wv.data(), hv.size() * sizeof(float)) == 0,
          "which: hiding the base hides the WHOLE group, byte-identically to a document with "
          "neither layer in it -- which is the sharpest consequence of the effective alpha");

    // The base's op stack cannot move the clip boundary: no committed op
    // touches alpha, so the group's coverage is bit-identical across a grade.
    Document graded = faded;
    graded.layers[0].ops.add(exposureOp(1.0f));
    const std::vector<float> gv = compositeDocumentPremultiplied(graded);
    const std::vector<float> fv = compositeDocumentPremultiplied(faded);
    bool alphaIdentical = gv.size() == fv.size();
    for (size_t i = 3; alphaIdentical && i < gv.size(); i += 4)
      if (std::memcmp(&gv[i], &fv[i], sizeof(float)) != 0) alphaIdentical = false;
    check(alphaIdentical,
          "which: a grade on the base leaves the clip boundary BIT-IDENTICAL -- ops/PointOps' "
          "committed set never touches alpha, so 'after the op stack' is safe to say");
  }

  // --- 4. A clipped Adjustment layer grades its BASE and nothing else -----
  {
    // Layer 0 covers two texels in red; layer 1 covers only the first, in
    // blue; layer 2 is an exposure of +1 stop (an exact doubling).
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    writeRgb(doc, 0, 1, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("blue"));
    writeRgb(doc, 1, 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    Document noAdjust = doc;
    addLayer(doc, 2, makeAdjustmentLayer("Exposure"));
    doc.layers[2].ops.add(exposureOp(1.0f));
    Document unclipped = doc;
    check(setLayerClipped(doc, 2, true).ok && doc.layers[2].clipped,
          "adjust: an Adjustment layer can be clipped -- it is the most common real use of "
          "clipping, and PRD D13's dodge and burn is exactly this shape");

    const DecodedImage clippedFlat = flattenDocumentToLinear(doc);
    const DecodedImage unclippedFlat = flattenDocumentToLinear(unclipped);
    const DecodedImage bareFlat = flattenDocumentToLinear(noAdjust);
    const std::array<float, 4> cIn = pixelOf(clippedFlat, 0, 0);
    const std::array<float, 4> cOut = pixelOf(clippedFlat, 1, 0);
    const std::array<float, 4> uIn = pixelOf(unclippedFlat, 0, 0);
    const std::array<float, 4> uOut = pixelOf(unclippedFlat, 1, 0);
    std::printf("  +1 stop over a blue layer covering only texel (0,0), on a red backdrop:\n"
                "    CLIPPED    (0,0) %.3f %.3f %.3f   (1,0) %.3f %.3f %.3f\n"
                "    unclipped  (0,0) %.3f %.3f %.3f   (1,0) %.3f %.3f %.3f\n",
                static_cast<double>(cIn[0]), static_cast<double>(cIn[1]),
                static_cast<double>(cIn[2]), static_cast<double>(cOut[0]),
                static_cast<double>(cOut[1]), static_cast<double>(cOut[2]),
                static_cast<double>(uIn[0]), static_cast<double>(uIn[1]),
                static_cast<double>(uIn[2]), static_cast<double>(uOut[0]),
                static_cast<double>(uOut[1]), static_cast<double>(uOut[2]));
    check(cIn == std::array<float, 4>{0.0f, 0.0f, 2.0f, 1.0f},
          "adjust: where the base HAS alpha the grade lands exactly -- blue doubled, the same "
          "value the unclipped layer produces there");
    check(samePixel(cOut, pixelOf(bareFlat, 1, 0)) &&
              uOut == std::array<float, 4>{2.0f, 0.0f, 0.0f, 1.0f},
          "adjust: **where the base has NO alpha the composite is byte-identical to the "
          "document without the adjustment layer** -- while the unclipped one doubles the red "
          "there, which is PRD C5's 'the composite below' and NOT PRD C9's clip");
    check(sameImage(clippedFlat, flattenDocumentToLinear(doc)) &&
              !sameImage(clippedFlat, unclippedFlat),
          "adjust: and the two documents differ as whole images, so the pixel above is not the "
          "only thing the clip changed");

    // The scope claim in its stronger form: outside the base's alpha, EVERY
    // texel is bit-identical to the document with no adjustment layer at all.
    bool outsideIdentical = clippedFlat.pixels.size() == bareFlat.pixels.size();
    for (uint32_t y = 0; outsideIdentical && y < clippedFlat.height; ++y)
      for (uint32_t x = 0; outsideIdentical && x < clippedFlat.width; ++x) {
        if (x == 0 && y == 0) continue;  // the one texel the base covers
        if (!samePixel(pixelOf(clippedFlat, x, y), pixelOf(bareFlat, x, y)))
          outsideIdentical = false;
      }
    check(outsideIdentical,
          "adjust: over the whole 8x8 canvas, every texel outside the base's single covered "
          "one is BYTE-IDENTICAL -- the clip is a scope restriction, asserted by memcmp");

    // A partly transparent base: the grade lands on the base's own STRAIGHT
    // colour, not on the composite that includes the backdrop.
    Document part = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(part, 0, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    addLayer(part, 1, makeRgbLayer("half white"));
    writeRgb(part, 1, 0, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    addLayer(part, 2, makeAdjustmentLayer("Exposure"));
    part.layers[2].ops.add(exposureOp(1.0f));
    Document partUnclipped = part;
    setLayerClipped(part, 2, true);
    const std::array<float, 4> pc = pixelOf(flattenDocumentToLinear(part), 0, 0);
    const std::array<float, 4> pu = pixelOf(flattenDocumentToLinear(partUnclipped), 0, 0);
    std::printf("    on a HALF-transparent base: clipped %.4f %.4f %.4f   "
                "unclipped %.4f %.4f %.4f\n",
                static_cast<double>(pc[0]), static_cast<double>(pc[1]),
                static_cast<double>(pc[2]), static_cast<double>(pu[0]),
                static_cast<double>(pu[1]), static_cast<double>(pu[2]));
    check(pc == std::array<float, 4>{1.5f, 1.0f, 1.0f, 1.0f} &&
              pu == std::array<float, 4>{2.0f, 1.0f, 1.0f, 1.0f},
          "adjust: on a half-transparent base the clipped grade doubles the base's own STRAIGHT "
          "white (1.0 -> 2.0, landing at 1.5 over the red), where the unclipped one doubles the "
          "already-composited 1.0 to 2.0 -- the group's alpha is exactly 1, so the bracket the "
          "op stack runs inside is a division by one");

    // Opacity and a mask on a clipped adjustment layer still mean §10's "how
    // much of the adjustment applies", unchanged by the clip.
    Document fade = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(fade, 0, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    addLayer(fade, 1, makeAdjustmentLayer("Exposure"));
    fade.layers[1].ops.add(exposureOp(1.0f));
    setLayerClipped(fade, 1, true);
    setLayerOpacity(fade, 1, 0.5f);
    const std::array<float, 4> half = pixelOf(flattenDocumentToLinear(fade), 0, 0);
    check(half == std::array<float, 4>{1.5f, 1.5f, 1.5f, 1.0f},
          "adjust: a clipped adjustment layer's own opacity still means HOW MUCH of the "
          "adjustment applies (§10) -- halfway from 1.0 to the graded 2.0, exactly");

    // An empty stack on a clipped adjustment layer must cost exactly nothing,
    // which here also means never opening the group's bracket.
    Document empty = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(empty, 0, 0, 0, {0.3f, 0.55f, 0.7f, 0.7f});
    Document withEmpty = empty;
    addLayer(withEmpty, 1, makeAdjustmentLayer("nothing"));
    setLayerClipped(withEmpty, 1, true);
    const std::vector<float> ev = compositeDocumentPremultiplied(empty);
    const std::vector<float> wv = compositeDocumentPremultiplied(withEmpty);
    check(ev.size() == wv.size() &&
              std::memcmp(ev.data(), wv.data(), ev.size() * sizeof(float)) == 0,
          "adjust: a clipped adjustment layer with an EMPTY stack is byte-identical to the "
          "layer not existing -- it never becomes a member, so it can never open the group's "
          "bracket, which §9 measures is not a bit-exact round trip");
  }

  // --- 5. A layer's own mask and its clip are different operators ---------
  {
    // Four texels: {base alpha 1, 0.5} x {mask 1, 0.5}, one opaque blue
    // clipped layer over an all-white base.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    writeRgb(doc, 0, 1, 0, {1.0f, 1.0f, 1.0f, 1.0f});
    writeRgb(doc, 0, 2, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    writeRgb(doc, 0, 3, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    addLayer(doc, 1, makeRgbLayer("clip"));
    for (int32_t x = 0; x < 4; ++x) writeRgb(doc, 1, x, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    setLayerClipped(doc, 1, true);
    addLayerMask(doc, 1);
    writeMask(doc, 1, 1, 0, 0.5f);
    writeMask(doc, 1, 3, 0, 0.5f);

    const DecodedImage flat = flattenDocumentToLinear(doc);
    std::array<std::array<float, 4>, 4> px{pixelOf(flat, 0, 0), pixelOf(flat, 1, 0),
                                           pixelOf(flat, 2, 0), pixelOf(flat, 3, 0)};
    std::printf("  one clipped layer, four combinations (straight colour, then coverage):\n");
    static const char* kNames[4] = {"base a=1.0  mask 1.0", "base a=1.0  mask 0.5",
                                    "base a=0.5  mask 1.0", "base a=0.5  mask 0.5"};
    for (int i = 0; i < 4; ++i)
      std::printf("    %-22s %.4f %.4f %.4f   a=%.4f\n", kNames[i],
                  static_cast<double>(px[i][0]), static_cast<double>(px[i][1]),
                  static_cast<double>(px[i][2]), static_cast<double>(px[i][3]));
    check(px[0] == std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f} &&
              px[1] == std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f} &&
              px[2] == std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.5f} &&
              px[3] == std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.5f},
          "both: all four combinations exact -- **the mask acts on COLOUR and the clip on "
          "COVERAGE**, so halving the mask gives the same straight colour at either base alpha");
    check(px[1][0] == px[3][0] && px[1][1] == px[3][1] && px[1][2] == px[3][2] &&
              px[1][3] != px[3][3],
          "both: stated as the one-line claim -- same colour, different coverage; a mask cannot "
          "change what a clipping group covers and a clip cannot change what it is coloured");

    // The negative control: if a clip were merely "mask the source by the
    // base's alpha", the answer would be different. It is.
    Document asMask = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(asMask, 0, 0, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    addLayer(asMask, 1, makeRgbLayer("masked"));
    writeRgb(asMask, 1, 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    addLayerMask(asMask, 1);
    writeMask(asMask, 1, 0, 0, 0.25f);  // the mask 0.5 times the base's alpha 0.5
    const std::vector<float> asMaskPre = compositeDocumentPremultiplied(asMask);
    const std::vector<float> clipPre = compositeDocumentPremultiplied(doc);
    std::printf("    the same layer as a plain 0.25 MASK instead of a clip: a=%.4f "
                "(the clipped answer is 0.5000)\n",
                static_cast<double>(asMaskPre[3]));
    check(asMaskPre[3] != 0.5f && clipPre[3 + 3 * 4] == 0.5f,
          "both: and a plain mask of mask x base-alpha is NOT the same operation -- its "
          "coverage is 0.625, because a mask lets the backdrop through where a clip does not");
  }

  // --- 6. `Mix` and a clip are mutually exclusive, in one predicate -------
  {
    MixboxLut lut;
    const bool lutLoaded = lut.load(NP_MIXBOX_LUT);
    check(lutLoaded,
          "mix: the real Mixbox LUT loads -- the pairing claims below are against real latents "
          "rather than a stand-in");
    const Pigment& yellowPigment = defaultPalette()[0];
    const Pigment& bluePigment = defaultPalette()[7];
    const Latent zYellow =
        lut.rgbToLatent(yellowPigment.rgb[0], yellowPigment.rgb[1], yellowPigment.rgb[2]);
    const Latent zBlue =
        lut.rgbToLatent(bluePigment.rgb[0], bluePigment.rgb[1], bluePigment.rgb[2]);

    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("yellow");
    writePigment(doc, 0, 0, 0, zYellow, 0.5f);
    addLayer(doc, 1, makePigmentLayer("blue"));
    writePigment(doc, 1, 0, 0, zBlue, 0.5f);
    check(blendModeAvailableForLayer(doc, 1, BlendMode::Mix),
          "mix: two stacked Pigment layers may take `Mix` -- PRD L5 as it stood before this "
          "step, restated so the change below is visible as a change");

    Document clipped = doc;
    check(setLayerClipped(clipped, 1, true).ok &&
              !blendModeAvailableForLayer(clipped, 1, BlendMode::Mix),
          "mix: **clipping the upper layer withdraws `Mix` from it** -- the layer beneath it "
          "cannot be both its mixing partner and the alpha it is masked by");
    const LayerOpResult blendRefused = setLayerBlend(clipped, 1, BlendMode::Mix);
    check(!blendRefused.ok && contains(blendRefused.error, "not available") &&
              contains(blendRefused.error, "mix"),
          "mix: so `core::setLayerBlend()` refuses it by name, through the same predicate the "
          "dropdown filters with -- one rule, not a UI convention");

    Document three = doc;
    addLayer(three, 2, makePigmentLayer("top"));
    writePigment(three, 2, 0, 0, zBlue, 0.5f);
    setLayerClipped(three, 1, true);
    check(!blendModeAvailableForLayer(three, 2, BlendMode::Mix),
          "mix: and clipping the LOWER of two candidates withdraws it too -- that layer belongs "
          "to a clipping run whose base is further down, and mixing it out of its own group is "
          "the case where the clip would silently stop applying");

    // The mirror refusal: clipping a layer that is already half of a pair.
    Document paired = doc;
    check(setLayerBlend(paired, 1, BlendMode::Mix).ok,
          "mix: a pair is formed the ordinary way");
    const LayerOpResult clipRefusedUpper = setLayerClipped(paired, 1, true);
    const LayerOpResult clipRefusedLower = setLayerClipped(paired, 0, true);
    check(!clipRefusedUpper.ok && contains(clipRefusedUpper.error, "half of a `Mix` pair") &&
              contains(clipRefusedUpper.error, "Change the blend mode"),
          "mix: `core::setLayerClipped()` refuses the upper half of a pair by name and says "
          "which change to make first -- the two refusals are each other's mirror");
    check(!clipRefusedLower.ok && contains(clipRefusedLower.error, "bottom layer"),
          "mix: and the lower half here is also layer 0, so it is refused for the stronger "
          "reason first -- the bottom layer can never be clipped at all");

    // A document that arrives from a file carrying both (PRD I10). The
    // compositor must answer, not refuse.
    Document fromFile = doc;
    fromFile.layers[1].blend = blendModeName(BlendMode::Mix);
    fromFile.layers[1].clipped = true;
    const MixPairing pairing = mixPairing(fromFile);
    check(!pairing.mixedWithBelow[1] && !pairing.consumedByAbove[0],
          "mix: a document carrying BOTH -- which no setter here would produce, but a file may "
          "(PRD I10) -- forms no pair at all");
    std::vector<std::string> warnings;
    const DecodedImage mixedFlat = flattenDocumentToLinear(fromFile, &warnings);
    bool namedTheClip = false;
    for (const std::string& wmsg : warnings)
      if (contains(wmsg, "it is clipped") && contains(wmsg, "one unit")) namedTheClip = true;
    check(namedTheClip,
          "mix: and it is warned about BY NAME with the clip as the specific reason, which is "
          "the contract §7 has applied to a misplaced `mix` since step 3 -- not a new one");
    Document asOver = fromFile;
    asOver.layers[1].blend = kDefaultBlendName;
    check(sameImage(mixedFlat, flattenDocumentToLinear(asOver)),
          "mix: and the pixels are BYTE-IDENTICAL to the same stack with the blend set to "
          "`normal` -- approximate, said so, and never silently");

    // A mixed pair IS a good clip base: it is one unit, and one unit is what
    // a base is. mass 0.5 over mass 0.5 unions to 0.75.
    Document overPair = doc;
    setLayerBlend(overPair, 1, BlendMode::Mix);
    addLayer(overPair, 2, makeRgbLayer("clip"));
    writeRgb(overPair, 2, 0, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    check(setLayerClipped(overPair, 2, true).ok, "mix: a layer above a mixed pair can be clipped");
    const ClipRuns pairRuns = clipRuns(overPair);
    const std::vector<float> pairPre = compositeDocumentPremultiplied(overPair);
    check(pairRuns.members[1].size() == 1 && pairRuns.members[1][0] == 2,
          "mix: it clips to the PAIR's upper index, which is the index the walk composites the "
          "pair at -- no special case, because the pair's output texel is the base's own");
    check(pairPre[3] == 0.75f && pairPre[0] == 0.0f && pairPre[1] == 0.0f && pairPre[2] == 0.75f,
          "mix: and it is clipped to the PAIR's coverage -- two masses of 0.5 union to 0.75, so "
          "an opaque clipped layer lands at exactly 0.75 rather than at 1.0");
  }

  // --- 7. The bottom layer cannot be clipped, and the two other orphans ---
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    addLayer(doc, 1, makeRgbLayer("middle"));
    addLayer(doc, 2, makeRgbLayer("top"));
    const LayerOpResult bottom = setLayerClipped(doc, 0, true);
    check(!bottom.ok && contains(bottom.error, "bottom layer") &&
              contains(bottom.error, "3-layer") && contains(bottom.error, "index 0") &&
              !doc.layers[0].clipped,
          "bottom: `setLayerClipped(0)` is refused BY NAME AND WITH THE NUMBERS -- the index, "
          "the layer count, and the sentence from PRD C9 that makes it impossible");

    // The reorder that would otherwise be the back door into the same state.
    check(setLayerClipped(doc, 2, true).ok, "bottom: the top layer clips fine");
    const LayerOpResult moved = moveLayer(doc, 2, 0);
    check(!moved.ok && contains(moved.error, "is clipped") &&
              contains(moved.error, "index 0 is the bottom") && contains(moved.error, "3-layer") &&
              doc.layers[2].clipped,
          "bottom: and `core::moveLayer()` refuses to drag a clipped layer to index 0, with the "
          "same numbers -- otherwise a reorder would be a back door into a state the setter "
          "refuses, and the setter's refusal would be decorative");
    check(moveLayer(doc, 2, 1).ok && doc.layers[1].clipped,
          "bottom: every OTHER move is untouched -- the base is derived from position, so "
          "dragging a clipped layer around the stack is exactly how it is re-based");
    Document unclippedMove = Document::createBlank(8, 8, WorkingSpace{});
    addLayer(unclippedMove, 1, makeRgbLayer("second"));
    check(moveLayer(unclippedMove, 1, 0).ok,
          "bottom: and an UNCLIPPED layer still moves to index 0, so the refusal is about the "
          "flag and not about the destination");

    // **The reorder that is NOT refused, and must not be.** Dragging a clipped
    // layer down to index 0 is a back door into a state the setter refuses.
    // Dragging its BASE out from under it reaches the SAME state, and is
    // allowed on purpose -- core/LayerOps.cpp says why: refusing it would let
    // one layer's flag veto a reorder of a different layer, and the baseless
    // state is one a document must tolerate anyway, because PRD I10 says a
    // file may carry a flag this build did not write. What makes "allowed"
    // defensible is not a second gate but the compositor, so the whole
    // orphaning path is asserted here rather than left as a claim in a comment.
    Document basePulled = Document::createBlank(8, 8, WorkingSpace{});
    addRgbLayer(basePulled, "base", 0, 0, {0.5f, 0.0f, 0.0f, 0.5f});
    addRgbLayer(basePulled, "clipme", 0, 0, {0.0f, 0.25f, 0.0f, 0.25f});
    addRgbLayer(basePulled, "above", 1, 0, {0.0f, 0.0f, 0.125f, 0.125f});
    check(setLayerClipped(basePulled, 1, true).ok,
          "basePulled: the middle layer clips to the base directly below it");
    const LayerOpResult baseMoved = moveLayer(basePulled, 0, 2);
    check(baseMoved.ok && basePulled.layers[0].clipped,
          "basePulled: **moving the BASE out from under its run is ALLOWED**, and leaves the clipped "
          "layer at index 0 -- the same baseless state `setLayerClipped()` refuses to create, "
          "reached by a reorder that is deliberately not guarded");
    std::vector<std::string> basePulledWarnings;
    const std::vector<float> basePulledPre = compositeDocumentPremultiplied(basePulled, &basePulledWarnings);
    Document basePulledClear = basePulled;
    basePulledClear.layers[0].clipped = false;
    const std::vector<float> basePulledClearPre = compositeDocumentPremultiplied(basePulledClear);
    bool basePulledNamed = false;
    for (const std::string& wmsg : basePulledWarnings)
      if (contains(wmsg, "layer 0") && contains(wmsg, "asks to be clipped") &&
          contains(wmsg, "composited **unclipped**"))
        basePulledNamed = true;
    check(basePulledNamed && basePulledWarnings.size() == 1,
          "basePulled: and the compositor is the safety net rather than a second gate -- it names the "
          "orphaned layer once, by the same sentence a flag that arrived in a file gets");
    check(basePulledPre.size() == basePulledClearPre.size() &&
              std::memcmp(basePulledPre.data(), basePulledClearPre.data(),
                          basePulledPre.size() * sizeof(float)) == 0,
          "basePulled: and its pixels are BYTE-IDENTICAL to the flag being clear, so a clip with no "
          "base costs pixels nothing -- the warning is the whole of its effect");

    // The two other ways to have nothing to clip to, refused by their own
    // reasons rather than by one catch-all sentence.
    Document stacked = Document::createBlank(8, 8, WorkingSpace{});
    addLayer(stacked, 1, makeRgbLayer("c1"));
    addLayer(stacked, 2, makeRgbLayer("c2"));
    stacked.layers[0].clipped = true;  // as a file may carry it; the setter refuses it
    const LayerOpResult buried = setLayerClipped(stacked, 1, true);
    check(!buried.ok && contains(buried.error, "down to layer 0") &&
              contains(buried.error, "never another clipped layer's base"),
          "bottom: a layer whose whole run below is clipped is refused with the run's own "
          "reason -- and the sentence restates why a clipped layer is never a base");

    Document overAdjust = Document::createBlank(8, 8, WorkingSpace{});
    addLayer(overAdjust, 1, makeAdjustmentLayer("grade"));
    addLayer(overAdjust, 2, makeRgbLayer("top"));
    const LayerOpResult onAdjust = setLayerClipped(overAdjust, 2, true);
    check(!onAdjust.ok && contains(onAdjust.error, "holds no pixels") &&
              contains(onAdjust.error, "Adjustment") &&
              contains(onAdjust.error, "not resolved by searching further down"),
          "bottom: and clipping onto a layer that holds no pixels is refused with ITS reason "
          "-- an Adjustment layer has no alpha, and the fix is not to clip to something else");

    // What a FILE can still carry, and what the compositor does with it.
    Document carried = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(carried, 0, 0, 0, {0.3f, 0.55f, 0.7f, 0.7f});
    addRgbLayer(carried, "second", 1, 0, {0.25f, 0.0f, 0.0f, 0.25f});
    Document clear = carried;
    carried.layers[0].clipped = true;
    std::vector<std::string> warnings;
    const std::vector<float> carriedPre = compositeDocumentPremultiplied(carried, &warnings);
    const std::vector<float> clearPre = compositeDocumentPremultiplied(clear);
    bool namedIt = false;
    for (const std::string& wmsg : warnings)
      if (contains(wmsg, "layer 0") && contains(wmsg, "bottom layer") &&
          contains(wmsg, "composited **unclipped**"))
        namedIt = true;
    check(namedIt && warnings.size() == 1,
          "bottom: a document that ARRIVED with a clipped bottom layer is not refused -- it is "
          "warned about by name, once, at the boundary that turns it into a durable artefact "
          "(PRD I10: the flag is carried, not coerced)");
    check(carriedPre.size() == clearPre.size() &&
              std::memcmp(carriedPre.data(), clearPre.data(),
                          carriedPre.size() * sizeof(float)) == 0,
          "bottom: and its pixels are BYTE-IDENTICAL to the flag being clear -- the layer is "
          "composited unclipped, never dropped, because one bit of metadata must not be what "
          "makes a layer's pixels vanish");

    Document orphanRun = clear;
    orphanRun.layers[0].clipped = true;
    orphanRun.layers[1].clipped = true;
    std::vector<std::string> runWarnings;
    const std::vector<float> orphanPre =
        compositeDocumentPremultiplied(orphanRun, &runWarnings);
    const ClipRuns orphanRuns = clipRuns(orphanRun);
    check(orphanRuns.clippedWithoutBase[0] && orphanRuns.clippedWithoutBase[1] &&
              orphanRuns.members[0].empty() && runWarnings.size() == 2,
          "bottom: a whole run clipped down to layer 0 leaves BOTH layers baseless, and both "
          "are named -- one warning each, not one for the run");
    check(std::memcmp(orphanPre.data(), clearPre.data(), orphanPre.size() * sizeof(float)) == 0,
          "bottom: and it too composites byte-identically to the flags being clear");
  }

  // --- 8. The probe and the flattener agree ------------------------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    for (int32_t x = 0; x < 4; ++x) writeRgb(doc, 0, x, 0, {1.0f, 0.0f, 0.0f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("base"));
    for (int32_t x = 0; x < 4; ++x) writeRgb(doc, 1, x, 0, {0.5f, 0.5f, 0.5f, 0.5f});
    addLayerMask(doc, 1);
    writeMask(doc, 1, 1, 0, 0.5f);
    addLayer(doc, 2, makeRgbLayer("clip"));
    for (int32_t x = 0; x < 3; ++x) writeRgb(doc, 2, x, 0, {0.0f, 0.0f, 1.0f, 1.0f});
    setLayerClipped(doc, 2, true);
    addLayerMask(doc, 2);
    writeMask(doc, 2, 2, 0, 0.25f);
    addLayer(doc, 3, makeAdjustmentLayer("Exposure"));
    doc.layers[3].ops.add(exposureOp(1.0f));
    setLayerClipped(doc, 3, true);
    setLayerOpacity(doc, 3, 0.75f);

    const DecodedImage flat = flattenDocumentToLinear(doc);
    ProbeParams params;
    params.sampleAllLayers = true;
    params.sampleSize = 1;
    float worst = 0.0f;
    for (int32_t x = 0; x < 4; ++x) {
      const ProbeSample s = probePixel(doc, PixelCoord{x, 0}, params);
      const std::array<float, 4> f = pixelOf(flat, static_cast<uint32_t>(x), 0);
      for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(s.linear[c] - f[c]));
    }
    std::printf("  [measured] probe vs. flattener over a masked, faded, clipped stack: worst "
                "residual %.3e against a derived bound of %.3e (2^-25 = %.3e, x3.4)\n",
                static_cast<double>(worst), static_cast<double>(kUnpremultiplyTol),
                static_cast<double>(2.9802322e-08f));
    check(worst <= kUnpremultiplyTol,
          "probe: the eyedropper and the export agree on a clipped stack -- both go through "
          "core/Composite's own clipGroupOpen/Fold/Close, so they cannot grow two answers");

    Document unclipped = doc;
    unclipped.layers[2].clipped = false;
    unclipped.layers[3].clipped = false;
    const ProbeSample clippedProbe = probePixel(doc, PixelCoord{3, 0}, params);
    const ProbeSample unclippedProbe = probePixel(unclipped, PixelCoord{3, 0}, params);
    check(!samePixel(clippedProbe.linear, unclippedProbe.linear),
          "probe: and it really is reading the clip -- clearing the two flags changes what the "
          "eyedropper reports at a texel the base does not fully cover");
  }

  // --- 9. The regression boundary, and why the bracket is opened lazily ---
  {
    // The three bracket functions on their own, first.
    check(clipGroupOpen({0.0f, 0.0f, 0.0f, 0.0f}) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          "bracket: `clipGroupOpen()` on a zero-alpha base returns zeros -- the division is "
          "unreachable rather than guarded twice, because a base with no coverage clips its "
          "members away entirely");
    const std::array<float, 4> opaque{0.25f, 0.5f, 0.75f, 1.0f};
    check(clipGroupOpen(opaque) == opaque,
          "bracket: on an OPAQUE base it is the identity, so the commonest clip of all costs "
          "nothing at all");
    const std::array<float, 4> folded =
        clipGroupFold({0.25f, 0.5f, 0.75f, 1.0f}, BlendMode::Normal, {0.0f, 0.0f, 0.5f, 0.5f}, {},
                      1.0f);
    check(folded[3] == 1.0f,
          "bracket: a fold ASSIGNS the group's alpha 1.0f rather than computing `as + 1*(1-as)` "
          "-- 'a clipping group's coverage is the base's' has to be an invariant, not a "
          "rounding that usually lands");
    check(clipGroupClose({1.0f, 1.0f, 1.0f, 1.0f}, 0.375f) ==
              std::array<float, 4>{0.375f, 0.375f, 0.375f, 0.375f},
          "bracket: and a close restores exactly the base's alpha, which is the only alpha a "
          "clipping group can have");

    // The claim the lazy open exists for: open-then-close is NOT a bit-exact
    // round trip, so a texel where no member contributes must never enter it.
    //
    // **The witness is swept for, not chosen, and the obvious way to look for
    // one finds nothing.** Fifty million pairs built the natural way -- pick an
    // alpha, then pick a premultiplied component as `k * a` -- produced no
    // exception at all while this section was being written, because that
    // construction has already rounded a product by `a` and the division
    // undoes it. Independent values are a different population entirely. The
    // loop below walks the whole binade [0.125, 0.25) against one fixed alpha,
    // counts every exception and prints the rate, so what follows is a
    // measurement of this machine's float unit and not a constant somebody
    // once found.
    const float sweepAlpha = 0x1.4c4f32p-1f;
    size_t mismatches = 0;
    size_t swept = 0;
    std::array<float, 4> witness{};
    for (uint32_t m = 0; m < (1u << 23); ++m) {
      const uint32_t word = (124u << 23) | m;
      float c;
      std::memcpy(&c, &word, sizeof(float));
      if (c > sweepAlpha) break;
      ++swept;
      if ((c / sweepAlpha) * sweepAlpha == c) continue;
      if (mismatches == 0) witness = {c, c, c, sweepAlpha};
      ++mismatches;
    }
    const std::array<float, 4> roundTripped =
        clipGroupClose(clipGroupOpen(witness), witness[3]);
    std::printf("  [measured] open-then-close over a whole binade against a=%.9g: %zu of %zu "
                "premultiplied values do not survive it (%.4f%%)\n"
                "    first witness c=%.9g -> %.9g, delta %.3e (one ulp)\n",
                static_cast<double>(sweepAlpha), mismatches, swept,
                swept > 0 ? 100.0 * static_cast<double>(mismatches) / static_cast<double>(swept)
                          : 0.0,
                static_cast<double>(witness[0]), static_cast<double>(roundTripped[0]),
                static_cast<double>(std::fabs(roundTripped[0] - witness[0])));
    check(mismatches > 0 && !samePixel(roundTripped, witness),
          "bracket: **open-then-close is measurably NOT the identity** -- which is why the "
          "group is opened by the first member that actually contributes, and why every "
          "empty-group claim above could be asserted with memcmp rather than a tolerance");

    // Step 1's boundary, re-made with this step's branch present: a
    // non-overlapping multi-layer document still composites byte-identically
    // to a plain sum, written here rather than borrowed.
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    addRgbLayer(doc, "b", 4, 2, {0.5f, 0.125f, 0.25f, 0.5f});
    addRgbLayer(doc, "c", 9, 7, {0.75f, 0.75f, 0.0f, 0.75f});
    setLayerBlend(doc, 1, BlendMode::Multiply);
    setLayerBlend(doc, 2, BlendMode::Screen);
    addLayerMask(doc, 2);
    writeMask(doc, 2, 9, 7, 0.5f);
    addLayer(doc, 3, makeAdjustmentLayer("nothing"));

    std::vector<float> plainSum(16 * 16 * 4, 0.0f);
    auto add = [&](int32_t x, int32_t y, const std::array<float, 4>& v, float k) {
      float* p = &plainSum[(static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4];
      for (int c = 0; c < 4; ++c) p[c] += v[c] * k;
    };
    add(1, 1, {0.25f, 0.5f, 0.75f, 1.0f}, 1.0f);
    add(4, 2, {0.5f, 0.125f, 0.25f, 0.5f}, 1.0f);
    add(9, 7, {0.75f, 0.75f, 0.0f, 0.75f}, 0.5f);
    const std::vector<float> composited = compositeDocumentPremultiplied(doc);
    check(composited.size() == plainSum.size() &&
              std::memcmp(composited.data(), plainSum.data(),
                          plainSum.size() * sizeof(float)) == 0,
          "regression: **a document with NO clipped layer composites BYTE-IDENTICALLY to the "
          "plain sum**, over raw floats at zero tolerance -- three blends, a mask and an "
          "adjustment layer, and step 9's branch costs it not one ulp");

    // And the same claim through the file-facing flattener, which is where a
    // regression would actually reach a user.
    Document withFlags = doc;
    setLayerClipped(withFlags, 2, true);
    setLayerClipped(withFlags, 2, false);
    check(sameImage(flattenDocumentToLinear(doc), flattenDocumentToLinear(withFlags)),
          "regression: setting a clip flag and clearing it again leaves the flattened image "
          "byte-identical, so the flag is genuinely one bit of state and not a latch");
  }

  // --- 10. The cost claim, measured rather than asserted ------------------
  {
    // §17: a clipping run is walked over the BASE's tiles only, so a clipped
    // layer's own tiles outside its base cost nothing. The measurement is of
    // the *marginal* cost of adding that layer, with and without the flag, on
    // an identical fixture -- which is the only form of the claim that is not
    // dominated by the accumulator's own zero-fill.
    constexpr int32_t kSide = 1024;  // 8 x 8 = 64 tiles
    Document base = Document::createBlank(kSide, kSide, WorkingSpace{});
    writeRgb(base, 0, 3, 3, {0.5f, 0.5f, 0.5f, 1.0f});  // one tile, tile (0,0)
    Document wide = base;
    addLayer(wide, 1, makeRgbLayer("wide"));
    for (int32_t ty = 0; ty < 8; ++ty)
      for (int32_t tx = 0; tx < 8; ++tx)
        writeRgb(wide, 1, tx * kTileSize + 5, ty * kTileSize + 5, {0.25f, 0.0f, 0.0f, 0.25f});
    Document wideClipped = wide;
    setLayerClipped(wideClipped, 1, true);
    check(wide.layers[1].rgbTiles->occupiedTileCount() == 64 &&
              base.layers[0].rgbTiles->occupiedTileCount() == 1,
          "cost: the fixture is a 1-tile base under a 64-tile layer, so the difference the "
          "measurement is looking for is a factor of 64 in tiles walked");

    auto timeComposite = [](const Document& d) {
      double best = 1e30;
      for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<float> v = compositeDocumentPremultiplied(d);
        const auto t1 = std::chrono::steady_clock::now();
        if (v.empty()) continue;
        best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
      }
      return best;
    };
    const double tBase = timeComposite(base);
    const double tBaseAgain = timeComposite(base);
    const double tWide = timeComposite(wide);
    const double tClipped = timeComposite(wideClipped);
    const double marginalWide = tWide - tBase;
    const double marginalClipped = tClipped - tBase;
    const double noise = std::fabs(tBaseAgain - tBase);
    char marginalPct[64];
    // The clipped marginal can come out at or below the re-timing noise -- it
    // is a single extra tile's work against a 16 MiB accumulator zero-fill --
    // so the line says that rather than printing a negative percentage as if
    // it meant something.
    std::printf("  [measured] 1024x1024 composite: base alone %.4f s, +64-tile layer %.4f s, "
                "+the same layer CLIPPED %.4f s\n"
                "    [measured] marginal cost of the layer: unclipped %.4f s, clipped %.4f s (%s); "
                "noise floor %.4f s from re-timing the base\n",
                tBase, tWide, tClipped, marginalWide, marginalClipped,
                marginalClipped <= noise
                    ? "at or below the noise floor"
                    : (std::snprintf(marginalPct, sizeof(marginalPct), "%.1f%% of it",
                                     100.0 * marginalClipped / marginalWide),
                       marginalPct),
                noise);
    check(marginalClipped < 0.5 * marginalWide,
          "cost: **a clipped layer's tiles outside its base are never visited** -- its marginal "
          "cost is under half the same layer's unclipped, on a fixture where it holds 64x the "
          "base's tiles. Clipping is the one feature in this walk that can only make it cheaper");
    check(marginalWide > noise * 2.0,
          "cost: and the unclipped marginal cost is itself well above the measured noise floor, "
          "so the comparison above is between two real numbers");
  }

  // --- 11. Persistence: `np:clipped` -------------------------------------
  {
    const char* kPath = "selftest_clip.npaint";
    const char* kBare = "selftest_clip_bare.npaint";
    const char* kAgain = "selftest_clip_again.npaint";
    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);

    auto bytesWithoutCapDate = [](const char* path) -> std::vector<unsigned char> {
      std::ifstream in(path, std::ios::binary);
      std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
      static const std::string kNeedle = "capDate";
      for (size_t i = 0; i + kNeedle.size() <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, kNeedle.data(), kNeedle.size()) != 0) continue;
        for (size_t j = i; j < std::min(i + 47, b.size()); ++j) b[j] = 0;
      }
      return b;
    };

    MixboxLut lut;
    lut.load(NP_MIXBOX_LUT);
    const Pigment& bluePigment = defaultPalette()[7];
    const Latent zBlue =
        lut.rgbToLatent(bluePigment.rgb[0], bluePigment.rgb[1], bluePigment.rgb[2]);

    // One base and three clipped layers, one of each kind that can hold or
    // carry content: RGB, Pigment and Adjustment.
    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    writeRgb(doc, 0, 3, 4, {0.5f, 0.25f, 0.125f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("clipped rgb"));
    writeRgb(doc, 1, 3, 4, {0.0f, 0.0f, 0.5f, 0.5f});
    addLayer(doc, 2, makePigmentLayer("clipped pigment"));
    writePigment(doc, 2, 3, 4, zBlue, 0.5f);
    addLayer(doc, 3, makeAdjustmentLayer("clipped grade"));
    doc.layers[3].ops.add(exposureOp(1.0f));

    // The flag-free file first: it is the reference for the property that
    // makes this format change safe.
    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok, "npaint: the four-layer fixture saves with no clip flags set");

    bool allClipped = true;
    for (size_t i = 1; i <= 3; ++i) allClipped = setLayerClipped(doc, i, true).ok && allClipped;
    check(allClipped, "npaint: all three layers above the base are clipped");

    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok && saved.partsWritten == 5 && saved.warnings.empty(),
          "npaint: it saves as five parts with nothing approximate about it -- a legal "
          "clipping run is not an approximation and must not warn");
    const NpaintLoadResult back = loadNpaint(kPath);
    check(back.ok && back.warnings.empty() && back.document.layers.size() == 4,
          "npaint: and it loads back clean, with all four layers");
    if (back.ok && back.document.layers.size() == 4) {
      check(!back.document.layers[0].clipped && back.document.layers[1].clipped &&
                back.document.layers[2].clipped && back.document.layers[3].clipped,
            "npaint: `np:clipped` round-trips on an RGB layer, a Pigment layer AND an "
            "Adjustment layer -- it is universal, unlike np:mask");
      const ClipRuns backRuns = clipRuns(back.document);
      check(backRuns.members[0].size() == 3 && backRuns.clippedToBase[3],
            "npaint: so the reloaded document resolves to the same single run -- the flag is "
            "what persists, and the structure is re-derived from it (core/Layer.hpp)");
      check(sameImage(flattenDocumentToLinear(doc),
                      flattenDocumentToLinear(back.document)),
            "npaint: and it composites BYTE-IDENTICALLY to the saved document, which is the "
            "only claim that covers the whole path at once");
    }

    // The property the format change rests on.
    Document cleared = doc;
    for (size_t i = 1; i <= 3; ++i) setLayerClipped(cleared, i, false);
    std::remove(kAgain);
    const NpaintSaveResult again = saveNpaint(cleared, kAgain);
    check(again.ok && !bytesWithoutCapDate(kBare).empty() &&
              bytesWithoutCapDate(kBare) == bytesWithoutCapDate(kAgain),
          "npaint: clearing every clip flag gives back a file BYTE-IDENTICAL to the one "
          "written before any was set (OpenImageIO's capDate masked, which HEAD's own two "
          "runs differ in too) -- np:clipped is written only when true");
    check(bytesWithoutCapDate(kPath).size() > bytesWithoutCapDate(kBare).size(),
          "npaint: and the file WITH the flags really is bigger, so the check above is not "
          "passing because nothing was ever written");

    // A clipped BOTTOM layer survives a round trip rather than being
    // refused, and the save says what it did about it.
    Document badBottom = Document::createBlank(128, 128, WorkingSpace{});
    writeRgb(badBottom, 0, 1, 1, {0.5f, 0.5f, 0.5f, 1.0f});
    badBottom.layers[0].clipped = true;
    std::remove(kPath);
    const NpaintSaveResult bottomSaved = saveNpaint(badBottom, kPath);
    bool bottomWarned = false;
    for (const std::string& wmsg : bottomSaved.warnings)
      if (contains(wmsg, "nothing beneath it to clip to")) bottomWarned = true;
    const NpaintLoadResult bottomBack = loadNpaint(kPath);
    check(bottomSaved.ok && bottomWarned,
          "npaint: a clipped bottom layer is SAVED, not refused, and the save names it -- "
          "refusing would let a preserved attribute be the thing that bricks the file it was "
          "preserved in");
    check(bottomBack.ok && bottomBack.document.layers.size() == 1 &&
              bottomBack.document.layers[0].clipped,
          "npaint: and the flag comes back exactly as written (PRD I10), rather than being "
          "coerced to something this build finds tidier");

    // The clipped-bottom-layer warning saveNpaint() surfaces above is
    // produced by the flattener itself, not by anything npaint-specific.
    Document clipBottomOnly = Document::createBlank(128, 128, WorkingSpace{});
    writeRgb(clipBottomOnly, 0, 1, 1, {0.5f, 0.5f, 0.5f, 1.0f});
    clipBottomOnly.layers[0].clipped = true;
    std::vector<std::string> warnings;
    flattenDocumentToLinear(clipBottomOnly, &warnings);
    check(warnings.size() == 1 && contains(warnings[0], "nothing beneath it to clip to"),
          "npaint: the clipped-bottom-layer warning is produced by the flattener directly, "
          "confirmed independently of saveNpaint()");

    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);
    check(std::fopen(kPath, "rb") == nullptr && std::fopen(kBare, "rb") == nullptr &&
              std::fopen(kAgain, "rb") == nullptr,
          "npaint: every scratch file this section wrote is removed");
  }

  std::printf("[selftest] clipping masks %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
