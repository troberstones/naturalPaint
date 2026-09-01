#include "app/selftest/Support.hpp"

namespace np {

bool runDocumentTextureTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerances, and why most of this section is at exactly zero --------
  //
  // Every fixture colour, coverage and opacity below is **dyadic** (0, 0.125,
  // 0.25, 0.5, 0.75, 1) so that the compositor's multiplies and this module's
  // one divide are all exact in binary floating point, and so that every value
  // is exactly representable in f16 as well as in f32. Under those conditions
  // the upload path is a *bijection* on the fixtures and the assertions are
  // `==` and `memcmp`, not comparisons against a tolerance.
  //
  // Where a tolerance is unavoidable it is **derived and then measured beside
  // the derivation**, never borrowed:
  //
  //  * `kUnpremultiplyTol` = 1.0e-7 -- one correctly-rounded f32 division, so
  //    half an ulp at results in [0.25, 1) is 2^-25 = 2.98e-8, a 3.4x margin.
  //    The identical derivation appears in runLayerStackTest(),
  //    runBlendTest(), runLayerMaskTest() and runClippingMaskTest(); this
  //    section reuses the number rather than inventing a second one.
  //  * `kHalfTol` = 2^-11 = 4.88e-4 -- IEEE binary16 carries an 11-bit
  //    significand, so the worst relative round-trip error for a normal value
  //    is 2^-11. That bound is *measured* against real composite output in
  //    §3 and the observed maximum is printed beside it, together with the
  //    8-bit path's own measured maximum on the same data.
  constexpr float kUnpremultiplyTol = 1.0e-7f;
  constexpr float kHalfTol = 4.8828125e-4f;  // 2^-11

  // The blend every widget in the ImGui window is drawn with. This is the
  // whole reason the upload is straight-alpha, so it is written once, here,
  // as arithmetic -- `src` is a STRAIGHT-alpha texel, `dst` an opaque
  // backdrop.
  auto imguiBlend = [](const std::array<float, 4>& src, const std::array<float, 3>& dst) {
    const float a = src[3];
    return std::array<float, 3>{src[0] * a + dst[0] * (1.0f - a),
                                src[1] * a + dst[1] * (1.0f - a),
                                src[2] * a + dst[2] * (1.0f - a)};
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
  auto texelAt = [](const std::vector<uint16_t>& halves, int32_t w, int32_t x, int32_t y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return std::array<float, 4>{halfToFloat(halves[i + 0]), halfToFloat(halves[i + 1]),
                                halfToFloat(halves[i + 2]), halfToFloat(halves[i + 3])};
  };
  auto exposureOp = [](float stops) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = stops;
    return op;
  };

  std::printf("  -- 1. core/Premultiply: one guard, five call sites --\n");

  // -----------------------------------------------------------------------
  // 1. The promoted guard
  // -----------------------------------------------------------------------
  {
    const std::array<float, 4> half{0.5f, 0.25f, 0.125f, 0.5f};
    const std::array<float, 4> straight = unpremultiply(half);
    check(straight[0] == 1.0f && straight[1] == 0.5f && straight[2] == 0.25f &&
              straight[3] == 0.5f,
          "guard: premultiplied / a on dyadic input, exactly");

    const std::array<float, 4> opaque{0.25f, 0.5f, 0.75f, 1.0f};
    check(std::memcmp(unpremultiply(opaque).data(), opaque.data(), sizeof(float) * 4) == 0,
          "guard: a == 1 is a BIT-EXACT identity, not a divide by one");

    const std::array<float, 4> zero{0.0f, 0.0f, 0.0f, 0.0f};
    check(std::memcmp(unpremultiply(std::array<float, 4>{0.9f, 0.8f, 0.7f, 0.0f}).data(),
                      zero.data(), sizeof(float) * 4) == 0,
          "guard: a == 0 -> {0,0,0,0}, the value an untouched Tile texel reads");
    check(std::memcmp(unpremultiply(std::array<float, 4>{0.9f, 0.8f, 0.7f, -0.5f}).data(),
                      zero.data(), sizeof(float) * 4) == 0,
          "guard: a < 0 caught too -- `<=`, so a file's bad alpha cannot negate colour");

    // The reason it is a template rather than a float function: ops/Resample
    // accumulates in double and must divide there.
    const double exactThird = unpremultiply(std::array<double, 4>{1.0, 0.0, 0.0, 3.0})[0];
    const double viaFloat =
        static_cast<double>(unpremultiply(std::array<float, 4>{1.0f, 0.0f, 0.0f, 3.0f})[0]);
    check(exactThird == 1.0 / 3.0 && viaFloat != 1.0 / 3.0,
          "guard: the double instantiation gives an answer float cannot");
    std::printf("    1/3 in double %.17g, through float %.17g, difference %.3g\n", exactThird,
                viaFloat, viaFloat - exactThird);

    // All five call sites, on the one input where four independently retyped
    // copies could have disagreed.
    Document blank = Document::createBlank(64, 64, WorkingSpace{});
    const ProbeSample probed = probePixel(blank, PixelCoord{7, 9}, ProbeParams{});
    check(std::memcmp(probed.linear.data(), zero.data(), sizeof(float) * 4) == 0,
          "call site 1/5 core/Probe: a transparent probe is {0,0,0,0}");

    const DecodedImage flat = flattenDocumentToLinear(blank);
    bool flatAllZero = flat.valid();
    for (float v : flat.pixels) flatAllZero = flatAllZero && v == 0.0f;
    check(flatAllZero, "call site 2/5 io/Export: a transparent flatten is all zero");

    OpStack stack;
    stack.add(exposureOp(3.0f));
    // layerPointOps() hands back raw core::Op copies, not ops::PointOp
    // closures, since docs/architecture-review.md P0-5 (core/Composite.hpp);
    // core::applyOpsPremultiplied() (core/OpStack.hpp) is that
    // representation's un-premultiply/switch/re-premultiply bracket, the
    // same contract ops::applyPointOpsPremultiplied() keeps for its own
    // std::function-closure callers elsewhere in this suite.
    const std::vector<Op> ops = layerPointOps(stack);
    const std::array<float, 4> graded =
        applyOpsPremultiplied({0.9f, 0.8f, 0.7f, 0.0f}, ops);
    check(!ops.empty() && std::memcmp(graded.data(), zero.data(), sizeof(float) * 4) == 0,
          "call site 3/5 ops/PointOps: +3 stops on transparent is still {0,0,0,0}");

    std::vector<float> transparentSrc(4 * 4 * 4, 0.0f);
    std::vector<float> resampled;
    std::string resampleError;
    const bool resampleOk = resampleAreaAverage(transparentSrc.data(), 4, 4, 2, 2, &resampled,
                                                &resampleError);
    bool resampleAllZero = resampleOk && resampled.size() == 2u * 2u * 4u;
    for (float v : resampled) resampleAllZero = resampleAllZero && v == 0.0f;
    check(resampleAllZero, "call site 4/5 ops/Resample: a transparent reduce is all zero");

    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(blank);
    bool halvesAllZero = halves.size() == 64u * 64u * 4u;
    for (uint16_t h : halves) halvesAllZero = halvesAllZero && h == 0;
    check(halvesAllZero, "call site 5/5 ui/DocumentTexture: a blank upload is all zero");
  }

  // ops/Resample's own regression boundary, re-made after the rewrite: its
  // weights are double precisely so that a fully opaque image survives a
  // reduction at alpha exactly 1.0f -- io/Export refuses a JPEG below that.
  {
    constexpr uint32_t kSrc = 64, kDst = 8;  // an 8x reduction, 64 weights/axis
    std::vector<float> opaque(static_cast<size_t>(kSrc) * kSrc * 4, 0.0f);
    for (size_t i = 0; i < opaque.size(); i += 4) {
      opaque[i + 0] = 0.25f;
      opaque[i + 1] = 0.5f;
      opaque[i + 2] = 0.75f;
      opaque[i + 3] = 1.0f;
    }
    std::vector<float> out;
    std::string err;
    const bool resized = resampleAreaAverage(opaque.data(), kSrc, kSrc, kDst, kDst, &out, &err);
    float worstAlpha = 0.0f, worstColour = 0.0f;
    for (size_t i = 0; i < out.size(); i += 4) {
      worstAlpha = std::max(worstAlpha, std::fabs(out[i + 3] - 1.0f));
      worstColour = std::max(worstColour, std::fabs(out[i + 0] - 0.25f));
      worstColour = std::max(worstColour, std::fabs(out[i + 1] - 0.5f));
      worstColour = std::max(worstColour, std::fabs(out[i + 2] - 0.75f));
    }
    check(resized && worstAlpha == 0.0f,
          "resample: an 8x reduce of an opaque image is alpha EXACTLY 1.0f");
    check(resized && worstColour == 0.0f,
          "resample: and its colour is unchanged, at zero tolerance");
    std::printf("    64x64 -> 8x8: worst |alpha - 1| %.3g, worst |colour| %.3g "
                "(io/Export refuses a JPEG below alpha 1.0)\n",
                static_cast<double>(worstAlpha), static_cast<double>(worstColour));
  }

  std::printf("  -- 2. straight alpha, with the premultiplied reading beside it --\n");

  // -----------------------------------------------------------------------
  // 2. Straight vs premultiplied, against ImGui's actual blend
  // -----------------------------------------------------------------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    // Straight (0.5, 0.25, 1.0) at coverage 0.5 -> premultiplied storage.
    const std::array<float, 4> straightSource{0.5f, 0.25f, 1.0f, 0.5f};
    writeRgb(doc, 0, 3, 4,
             {straightSource[0] * 0.5f, straightSource[1] * 0.5f, straightSource[2] * 0.5f, 0.5f});

    const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc);
    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
    const size_t i = (4u * 8u + 3u) * 4u;
    const std::array<float, 4> uploaded = texelAt(halves, 8, 3, 4);
    check(uploaded[0] == straightSource[0] && uploaded[1] == straightSource[1] &&
              uploaded[2] == straightSource[2] && uploaded[3] == straightSource[3],
          "upload: the texel is STRAIGHT alpha, exactly the authored colour");
    check(premultiplied[i + 0] == 0.25f && premultiplied[i + 3] == 0.5f,
          "upload: and the compositor's own output is still premultiplied");

    // Paper, as the canvas block draws it: IM_COL32(250, 250, 247).
    const std::array<float, 3> paper{250.0f / 255.0f, 250.0f / 255.0f, 247.0f / 255.0f};
    const std::array<float, 3> withStraight = imguiBlend(uploaded, paper);
    const std::array<float, 4> asPremultiplied{premultiplied[i + 0], premultiplied[i + 1],
                                               premultiplied[i + 2], premultiplied[i + 3]};
    const std::array<float, 3> withPremultiplied = imguiBlend(asPremultiplied, paper);

    // The true answer, from core/Blend, over an opaque backdrop.
    const std::array<float, 4> paperTexel{paper[0], paper[1], paper[2], 1.0f};
    const std::array<float, 4> trueOver =
        blendPixel(BlendMode::Normal, asPremultiplied, paperTexel);
    float worst = 0.0f;
    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(withStraight[c] - trueOver[c]));
    check(worst <= kUnpremultiplyTol,
          "blend: straight-alpha upload through ImGui == core/Blend's `over`");

    float rejectedWorst = 0.0f;
    for (int c = 0; c < 3; ++c)
      rejectedWorst = std::max(rejectedWorst, std::fabs(withPremultiplied[c] - trueOver[c]));
    check(rejectedWorst > 0.05f,
          "blend: the premultiplied upload is WRONG through the same blend");
    std::printf("    straight  %.4f %.4f %.4f  (error %.3g)\n",
                static_cast<double>(withStraight[0]), static_cast<double>(withStraight[1]),
                static_cast<double>(withStraight[2]), static_cast<double>(worst));
    std::printf("    premult.  %.4f %.4f %.4f  (error %.3g -- darker at every partial "
                "coverage, i.e. every soft brush edge)\n",
                static_cast<double>(withPremultiplied[0]),
                static_cast<double>(withPremultiplied[1]),
                static_cast<double>(withPremultiplied[2]),
                static_cast<double>(rejectedWorst));

    // At the two alphas where the two readings coincide -- which is exactly
    // why a wrong choice here survives a casual look.
    Document opaqueDoc = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(opaqueDoc, 0, 1, 1, {0.5f, 0.25f, 1.0f, 1.0f});
    const std::vector<uint16_t> opaqueHalves = compositeDocumentStraightHalf(opaqueDoc);
    const std::array<float, 4> opaqueTexel = texelAt(opaqueHalves, 4, 1, 1);
    const std::array<float, 3> a = imguiBlend(opaqueTexel, paper);
    const std::array<float, 3> b = imguiBlend({0.5f, 0.25f, 1.0f, 1.0f}, paper);
    check(a[0] == b[0] && a[1] == b[1] && a[2] == b[2],
          "blend: at alpha 1 the two readings agree -- why the error hides");
  }

  std::printf("  -- 3. RGBA16Float, with the 8-bit path beside it (PRD B6) --\n");

  // -----------------------------------------------------------------------
  // 3. f16 vs 8-bit
  // -----------------------------------------------------------------------
  {
    auto quantise8 = [](float v) {
      const float c = std::clamp(v, 0.0f, 1.0f);
      return std::round(c * 255.0f) / 255.0f;
    };
    // One 8-bit code apart is 1/255 = 0.00392; these two are 1/1024 apart.
    const float v1 = 0.5f;
    const float v2 = 0.5f + 1.0f / 1024.0f;
    check(quantise8(v1) == quantise8(v2),
          "8-bit: two linear samples 1/1024 apart round to the SAME byte");
    check(floatToHalf(v1) != floatToHalf(v2),
          "f16: the same two samples stay distinct -- PRD B6, in one line");

    // The same claim through the real pipeline, on a real document.
    Document doc = Document::createBlank(64, 4, WorkingSpace{});
    for (int32_t x = 0; x < 64; ++x) {
      const float g = 0.5f + static_cast<float>(x) / 1000.0f;
      writeRgb(doc, 0, x, 1, {g, g, g, 1.0f});
    }
    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
    size_t distinctHalf = 0, distinct8 = 0;
    float worstHalf = 0.0f, worst8 = 0.0f;
    for (int32_t x = 0; x < 64; ++x) {
      const float g = 0.5f + static_cast<float>(x) / 1000.0f;
      const std::array<float, 4> up = texelAt(halves, 64, x, 1);
      worstHalf = std::max(worstHalf, std::fabs(up[0] - g));
      worst8 = std::max(worst8, std::fabs(quantise8(g) - g));
      if (x > 0) {
        const float prev = 0.5f + static_cast<float>(x - 1) / 1000.0f;
        if (up[0] != halfToFloat(floatToHalf(prev))) ++distinctHalf;
        if (quantise8(g) != quantise8(prev)) ++distinct8;
      }
    }
    check(distinctHalf == 63,
          "f16: all 64 steps of a 1/1000 ramp survive the upload distinctly");
    check(distinct8 < 63, "8-bit: the same ramp collapses -- measured, not argued");
    check(worstHalf <= kHalfTol, "f16: worst round-trip error inside the derived 2^-11 bound");
    check(worstHalf < worst8, "f16 beats 8-bit on the same data");
    std::printf("    ramp of 64 steps 1/1000 apart: f16 keeps %zu distinct, 8-bit keeps %zu\n",
                distinctHalf + 1, distinct8 + 1);
    std::printf("    worst round-trip error: f16 %.3g (bound 2^-11 = %.3g), 8-bit %.3g "
                "-- a factor of %.1f\n",
                static_cast<double>(worstHalf), static_cast<double>(kHalfTol),
                static_cast<double>(worst8),
                worstHalf > 0.0f ? static_cast<double>(worst8 / worstHalf) : 0.0);
  }

  std::printf("  -- 4. the upload buffer's layout, and the blank-document boundary --\n");

  // -----------------------------------------------------------------------
  // 4. Layout
  // -----------------------------------------------------------------------
  {
    Document doc = Document::createBlank(19, 7, WorkingSpace{});
    writeRgb(doc, 0, 13, 2, {0.25f, 0.5f, 0.75f, 1.0f});
    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
    check(halves.size() == 19u * 7u * 4u, "layout: exactly w * h * 4 halves, no padding");
    const std::array<float, 4> at = texelAt(halves, 19, 13, 2);
    check(at[0] == 0.25f && at[1] == 0.5f && at[2] == 0.75f && at[3] == 1.0f,
          "layout: row-major, top to bottom -- (13,2) is at (2*19+13)*4");
    size_t nonZero = 0;
    for (uint16_t h : halves) nonZero += (h != 0) ? 1 : 0;
    check(nonZero == 4, "layout: and every other texel is untouched zero");

    // The regression boundary, in numbers: this is the screenshot's twin.
    Document blank = Document::createBlank(19, 7, WorkingSpace{});
    const std::vector<uint16_t> blankHalves = compositeDocumentStraightHalf(blank);
    const std::vector<uint16_t> zeros(blankHalves.size(), 0);
    check(!blankHalves.empty() && std::memcmp(blankHalves.data(), zeros.data(),
                                              blankHalves.size() * sizeof(uint16_t)) == 0,
          "boundary: a new document is BIT-EXACTLY transparent everywhere");
    const std::array<float, 3> paper{250.0f / 255.0f, 250.0f / 255.0f, 247.0f / 255.0f};
    const std::array<float, 3> over = imguiBlend(texelAt(blankHalves, 19, 5, 3), paper);
    check(over[0] == paper[0] && over[1] == paper[1] && over[2] == paper[2],
          "boundary: so drawing it over the paper leaves the paper EXACTLY as it was");

    check(compositeDocumentStraightHalf(Document::createBlank(0, 0, WorkingSpace{})).empty(),
          "layout: a non-positive canvas uploads nothing, matching the compositor");
  }

  std::printf("  -- 5. the cache key, and the collision the obvious key would have --\n");

  // -----------------------------------------------------------------------
  // 5. The key
  // -----------------------------------------------------------------------
  {
    OpenDocument a = makeBlankOpenDocument(32, 32, WorkingSpace{}, "a");
    OpenDocument b = makeBlankOpenDocument(32, 32, WorkingSpace{}, "b");
    check(a.revision == 0 && b.revision == 0,
          "key: two fresh documents are BOTH at revision 0 -- the collision");
    check(a.id != b.id && documentTextureKey(a) != documentTextureKey(b),
          "key: `id` separates them, so revision alone would have been wrong");
    check(documentTextureKey(a) == documentTextureKey(a),
          "key: the same document at the same revision is the same key");

    const DocumentTextureKey before = documentTextureKey(a);
    a.recordEdit("an edit", EditKind::Content);
    check(documentTextureKey(a) != before, "key: recordEdit() moves it, so an edit re-uploads");

    OpenDocument sized = makeBlankOpenDocument(32, 32, WorkingSpace{}, "s");
    const DocumentTextureKey sameSize = documentTextureKey(sized);
    sized.document.width = 64;
    check(documentTextureKey(sized) != sameSize,
          "key: width and height are in it -- they decide the texture, not just its content");
  }

  std::printf("  -- 6. what the cache saves, measured --\n");

  // -----------------------------------------------------------------------
  // 6. Cost
  // -----------------------------------------------------------------------
  double composite1024Ms = 0.0;
  {
    auto buildContent = [&](int32_t size) {
      Document doc = Document::createBlank(size, size, WorkingSpace{});
      addLayer(doc, doc.layers.size(), makeRgbLayer("upper"));
      for (int32_t y = 0; y < size; y += 3) {
        for (int32_t x = 0; x < size; x += 3) {
          writeRgb(doc, 0, x, y, {0.25f, 0.5f, 0.75f, 1.0f});
          if (((x + y) & 1) == 0) writeRgb(doc, 1, x, y, {0.5f, 0.25f, 0.125f, 0.5f});
        }
      }
      return doc;
    };
    auto timeComposite = [&](const Document& doc) {
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      // Touched so the optimiser cannot discard the work being timed.
      if (halves.empty()) return -1.0;
      return ms;
    };

    {
      const Document doc = buildContent(1024);
      composite1024Ms = timeComposite(doc);
    }
    double composite2048Ms = 0.0;
    {
      const Document doc = buildContent(2048);
      composite2048Ms = timeComposite(doc);
    }

    // A cache hit: build the key and compare it. Timed over enough iterations
    // that the clock's own resolution is not the measurement.
    OpenDocument od = makeBlankOpenDocument(1024, 1024, WorkingSpace{}, "cached");
    const DocumentTextureKey held = documentTextureKey(od);
    constexpr int kHits = 1000000;
    size_t agreed = 0;
    const auto h0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kHits; ++i)
      if (documentTextureKey(od) == held) ++agreed;
    const double hitsMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - h0).count();
    const double perHitNs = hitsMs * 1.0e6 / static_cast<double>(kHits);

    // PRD F3 (P0): "Pen-to-photon latency under 20 ms; the in-progress stroke
    // does not wait on a full document re-composite." That budget is
    // end-to-end -- input event to displayed frame -- and NOT a compute
    // budget, so a composite that merely fits inside it has already spent the
    // whole thing and left nothing for the upload or the present. Read the
    // percentages below that way.
    constexpr double kPenToPhotonMs = 20.0;  // PRD F3
    std::printf("    CPU composite + un-premultiply + f16 pack:\n");
    std::printf("      [measured] 1024x1024  %8.3f ms  (%5.1f%% of PRD F3's %.0f ms pen-to-photon "
                "budget, which is end-to-end and not compute)\n",
                composite1024Ms, 100.0 * composite1024Ms / kPenToPhotonMs, kPenToPhotonMs);
    std::printf("      [measured] 2048x2048  %8.3f ms  (%5.1f%% of the same budget)\n", composite2048Ms,
                100.0 * composite2048Ms / kPenToPhotonMs);
    std::printf("    [measured] cache hit (build key + compare): %.1f ns, %d hits in %.3f ms\n", perHitNs,
                kHits, hitsMs);
    std::printf("    [measured] so one 1024x1024 upload costs about %.0f cache hits; an unchanged frame "
                "pays the %.1f ns\n",
                composite1024Ms * 1.0e6 / std::max(perHitNs, 1.0e-9), perHitNs);

    check(agreed == kHits, "cache: an unchanged document agrees with its key every time");
    check(perHitNs < composite1024Ms * 1.0e6,
          "cache: a hit is orders of magnitude cheaper than a recomposite");
    // **This used to be a hard assertion** ("a 2048x2048 recomposite alone
    // overruns F3's whole pen-to-photon budget") -- a fixed claim about how
    // slow the uncached path is, which is why the cache exists at all. It
    // stopped being reliably true once core/Composite.cpp's opaque-floor
    // early exit and tile-parallel walk landed: this fixture's own two-layer
    // content is a poor case for the opaque floor (every-3rd-pixel writes
    // never make a tile's alpha channel exactly 1.0 everywhere), so the
    // improvement seen here is squarely the parallel walk's -- but it means
    // a hardcoded ">kPenToPhotonMs" is now a claim about the machine's core
    // count as much as about the document, and asserting it risks becoming
    // exactly the stale-premise trap this comment is now warning the next
    // reader about. The claim this section actually needs -- a cache hit is
    // dramatically cheaper than recompositing, at ANY document size -- is
    // still proven above, size-independently, by `perHitNs <
    // composite1024Ms * 1.0e6`. This prints the now-measured number rather
    // than asserting a direction for it.
    std::printf("    [measured] 2048x2048 recomposite is now %.1f%% of PRD F3's %.0f ms budget "
                "(used to reliably exceed 100%% before the compositor's opaque-floor + "
                "tile-parallel work; no longer assumed, since a future speedup should not have "
                "to re-litigate this comment to stay green)\n",
                100.0 * composite2048Ms / kPenToPhotonMs, kPenToPhotonMs);
  }

  std::printf("  -- 7. the GPU round trip, at a stride the readback direction refuses --\n");

  // -----------------------------------------------------------------------
  // 7. Upload and read back
  // -----------------------------------------------------------------------
  {
    // 61 * 4 * 2 = 976 bytes/row, which is NOT a multiple of 256.
    constexpr int32_t kW = 61, kH = 37;
    check((static_cast<uint32_t>(kW) * 8u) % 256u != 0u,
          "gpu: the fixture's row stride is deliberately not 256-aligned");

    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "odd");
    writeRgb(od.document, 0, 43, 29, {0.25f, 0.5f, 0.75f, 1.0f});
    writeRgb(od.document, 0, 0, 0, {0.125f, 0.125f, 0.125f, 0.25f});
    od.recordEdit("content", EditKind::Content);

    DocumentTexture dt;
    const WGPUTextureView view = dt.viewFor(gpu, od);
    check(view != nullptr, "gpu: the upload produced a view for ImGui to sample");
    check(dt.uploads() == 1 && dt.cacheHits() == 0, "gpu: one upload, no hit yet");

    const std::vector<uint16_t> expected = compositeDocumentStraightHalf(od.document);
    std::vector<float> readback;
    const bool read = readbackRGBA16FPadded(gpu, dt.texture(), kW, kH, readback);
    check(read && readback.size() == expected.size(),
          "gpu: read back through a padded staging buffer");
    bool identical = read && readback.size() == expected.size();
    for (size_t i = 0; read && i < expected.size(); ++i)
      identical = identical && readback[i] == halfToFloat(expected[i]);
    check(identical, "gpu: every texel identical to the CPU halves -- any stride uploads");

    const size_t at = (29u * static_cast<size_t>(kW) + 43u) * 4u;
    check(read && readback[at + 0] == 0.25f && readback[at + 1] == 0.5f &&
              readback[at + 2] == 0.75f && readback[at + 3] == 1.0f,
          "gpu: and it landed at (43,29), so the row stride was honoured");

    // The cache, end to end.
    const WGPUTextureView again = dt.viewFor(gpu, od);
    check(again == view && dt.uploads() == 1 && dt.cacheHits() == 1,
          "gpu: a second ask at the same revision re-uploads nothing");
    check(dt.retiredTextures() == 0, "gpu: and retires nothing -- the same texture is reused");

    // The documented way to defeat it: a tile written without recordEdit().
    writeRgb(od.document, 0, 5, 5, {1.0f, 1.0f, 1.0f, 1.0f});
    dt.viewFor(gpu, od);
    check(dt.uploads() == 1 && dt.cacheHits() == 2,
          "gpu: a tile written WITHOUT recordEdit() is not seen -- the documented trap");
    od.recordEdit("the write", EditKind::Content);
    dt.viewFor(gpu, od);
    check(dt.uploads() == 2, "gpu: and recordEdit() is what makes it visible");

    std::vector<float> after;
    readbackRGBA16FPadded(gpu, dt.texture(), kW, kH, after);
    const size_t five = (5u * static_cast<size_t>(kW) + 5u) * 4u;
    check(after.size() == expected.size() && after[five + 0] == 1.0f && after[five + 3] == 1.0f,
          "gpu: the new texel is on the GPU, in the right place");

    // A size change is the one thing that retires a texture.
    OpenDocument bigger = makeBlankOpenDocument(kW * 2, kH, WorkingSpace{}, "bigger");
    const WGPUTextureView third = dt.viewFor(gpu, bigger);
    check(third != nullptr && dt.uploads() == 3 && dt.retiredTextures() == 1,
          "gpu: a size change makes a new texture and RETIRES the old one");
    check(third != view,
          "gpu: a new view, so ImGui's view-pointer-keyed bind-group cache stays honest");
    dt.release();
    check(dt.retiredTextures() == 0, "gpu: release() frees the retired list too");
  }

  std::printf("  -- 8. the visible consequences: what changes what is drawn --\n");

  // -----------------------------------------------------------------------
  // 8. Every document feature, proven to reach the upload
  // -----------------------------------------------------------------------
  {
    auto baseDocument = [&]() {
      Document doc = Document::createBlank(16, 16, WorkingSpace{});
      for (int32_t y = 4; y < 12; ++y)
        for (int32_t x = 4; x < 12; ++x) writeRgb(doc, 0, x, y, {0.5f, 0.25f, 0.125f, 1.0f});
      return doc;
    };
    const Document plain = baseDocument();
    const std::vector<uint16_t> plainHalves = compositeDocumentStraightHalf(plain);
    const std::vector<uint16_t> blankHalves =
        compositeDocumentStraightHalf(Document::createBlank(16, 16, WorkingSpace{}));
    auto differs = [&](const std::vector<uint16_t>& v) {
      return v.size() == plainHalves.size() &&
             std::memcmp(v.data(), plainHalves.data(), v.size() * sizeof(uint16_t)) != 0;
    };
    check(differs(blankHalves), "visible: content in a layer changes the uploaded bytes");

    {
      Document doc = baseDocument();
      setLayerVisible(doc, 0, false);
      const std::vector<uint16_t> hidden = compositeDocumentStraightHalf(doc);
      check(hidden.size() == blankHalves.size() &&
                std::memcmp(hidden.data(), blankHalves.data(),
                            hidden.size() * sizeof(uint16_t)) == 0,
            "visible: hiding it gives back the blank buffer BIT-EXACTLY");
    }
    {
      Document doc = baseDocument();
      setLayerOpacity(doc, 0, 0.5f);
      const std::vector<uint16_t> half = compositeDocumentStraightHalf(doc);
      check(differs(half), "visible: opacity changes it");
      check(texelAt(half, 16, 6, 6)[3] == 0.5f && texelAt(half, 16, 6, 6)[0] == 0.5f,
            "visible: and opacity is coverage, not colour -- alpha 0.5, colour unchanged");
    }
    {
      Document doc = baseDocument();
      addLayer(doc, doc.layers.size(), makeRgbLayer("upper"));
      for (int32_t y = 4; y < 12; ++y)
        for (int32_t x = 4; x < 12; ++x) writeRgb(doc, 1, x, y, {0.5f, 0.5f, 0.5f, 1.0f});
      const std::vector<uint16_t> normal = compositeDocumentStraightHalf(doc);
      setLayerBlend(doc, 1, BlendMode::Multiply);
      const std::vector<uint16_t> multiplied = compositeDocumentStraightHalf(doc);
      check(normal.size() == multiplied.size() &&
                std::memcmp(normal.data(), multiplied.data(),
                            normal.size() * sizeof(uint16_t)) != 0,
            "visible: a blend mode changes it");
      check(texelAt(multiplied, 16, 6, 6)[0] == 0.25f,
            "visible: and Multiply really multiplied -- 0.5 * 0.5");
    }
    {
      Document doc = baseDocument();
      addLayerMask(doc, 0);
      for (int32_t y = 4; y < 12; ++y)
        for (int32_t x = 4; x < 12; ++x) writeMask(doc, 0, x, y, x < 8 ? 0.0f : 1.0f);
      const std::vector<uint16_t> masked = compositeDocumentStraightHalf(doc);
      check(differs(masked), "visible: a mask changes it");
      check(texelAt(masked, 16, 5, 6)[3] == 0.0f && texelAt(masked, 16, 9, 6)[3] == 1.0f,
            "visible: and per texel -- covered on one side, revealed on the other");
    }
    {
      Document doc = baseDocument();
      addLayer(doc, doc.layers.size(), makeAdjustmentLayer("Exposure"));
      doc.layers[1].ops.add(exposureOp(1.0f));
      const std::vector<uint16_t> graded = compositeDocumentStraightHalf(doc);
      check(differs(graded), "visible: an adjustment layer changes it");
      check(texelAt(graded, 16, 6, 6)[0] == 1.0f && texelAt(graded, 16, 6, 6)[3] == 1.0f,
            "visible: +1 stop doubled the colour and left alpha alone");
    }
    {
      Document doc = baseDocument();
      addLayer(doc, doc.layers.size(), makeRgbLayer("clipped"));
      for (int32_t y = 0; y < 16; ++y)
        for (int32_t x = 0; x < 16; ++x) writeRgb(doc, 1, x, y, {0.0f, 1.0f, 0.0f, 1.0f});
      const std::vector<uint16_t> unclipped = compositeDocumentStraightHalf(doc);
      setLayerClipped(doc, 1, true);
      const std::vector<uint16_t> clipped = compositeDocumentStraightHalf(doc);
      check(unclipped.size() == clipped.size() &&
                std::memcmp(unclipped.data(), clipped.data(),
                            clipped.size() * sizeof(uint16_t)) != 0,
            "visible: a clipping mask changes it");
      check(texelAt(unclipped, 16, 1, 1)[3] == 1.0f && texelAt(clipped, 16, 1, 1)[3] == 0.0f,
            "visible: and outside the base's alpha the clipped layer is gone");
    }
  }

  std::printf("  -- 9. the export and the screen, agreeing --\n");

  // -----------------------------------------------------------------------
  // 9. The export and the screen, agreeing -- the risk four copies posed
  // -----------------------------------------------------------------------
  //
  // io/Export's flattener and this module both un-premultiply the same
  // composite, and they used to do it through two separately typed guards. If
  // those ever drifted, a file and the screen would disagree about a colour and
  // nothing would report it. They now share core/Premultiply, and that is
  // asserted here rather than assumed from the fact that both call it.
  //
  // The flattener is format-free -- only io/Export's *encoder* touches a
  // format -- so nothing here depends on it.
  {
    Document doc = Document::createBlank(24, 11, WorkingSpace{});
    for (int32_t y = 1; y < 10; ++y)
      for (int32_t x = 1; x < 23; ++x)
        writeRgb(doc, 0, x, y,
                 {0.5f * 0.75f, 0.25f * 0.75f, 0.125f * 0.75f, 0.75f});
    const DecodedImage flat = flattenDocumentToLinear(doc);
    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
    float worst = 0.0f;
    bool sameSize = flat.valid() && flat.pixels.size() == halves.size();
    for (size_t i = 0; sameSize && i < halves.size(); ++i)
      worst = std::max(worst, std::fabs(flat.pixels[i] - halfToFloat(halves[i])));
    check(sameSize && worst <= kUnpremultiplyTol,
          "agreement: io/Export's flatten and the screen upload give the same colour");
    std::printf("    largest disagreement %.3g (bound %.3g)\n", static_cast<double>(worst),
                static_cast<double>(kUnpremultiplyTol));
  }

  std::printf("[selftest] document texture %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
