#include "app/selftest/Support.hpp"

#include <string>

#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "core/SelectionShapes.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The brush's own blend mode (Photoshop's `Md `) reaching an RGB layer --
// brush/RgbDeposit.hpp §2a, and the routing edge that feeds it
// (`app/StrokeSession.cpp`'s `RgbStroke::begin(..., tip.blend)`, RGB route
// only).
//
// Driven through `RgbStroke` directly on a bare `TileStore`, the way
// app/selftest/RgbDeposit.cpp does -- this section is deliberately NOT a
// second copy of that file's §§1-15, which already cover the unblended
// composite (premultiplied, linear, the opacity ceiling, the selection
// bound, alpha lock's freeze) at length. Everything here is about the ONE
// thing §2a adds: the pre-stroke `dst0` a blended composite reads, and the
// stroke-level (never per-dab) arithmetic built on it.
// ---------------------------------------------------------------------------
bool runBrushBlendModeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // **kHalfRel** -- binary16 storage's round-to-nearest relative error bound
  // (2^-11 for a normal value), restated from app/selftest/RgbDeposit.cpp's
  // own derivation. Printed, not applied: every fixture below is chosen from
  // exactly-representable-in-half values (0.5, 0.25, 0.125, ...) precisely
  // so the assertions can be at ZERO tolerance instead of against this
  // bound -- it appears only as the number those zero-tolerance claims beat.
  constexpr float kHalfRel = 4.8828125e-04f;  // 2^-11

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // A hard disc (hardness 1), so `dabCoverage()` is exactly 1.0f over the
  // whole core -- every number below is about §2a's composite, not about the
  // falloff, which app/selftest/RgbDeposit.cpp §3 already checks.
  auto discTip = [](float radius, float flow, const std::array<float, 3>& rgb) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.linearRgb = rgb;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h) {
    return makeBlankOpenDocument(w, h, WorkingSpace{}, "brush blend mode");
  };

  // Paints an opaque flat `straight` colour into every texel of
  // `[x0,x1] x [y0,y1]`, direct through `Tile::writePixel()` -- the same
  // "pre-paint the canvas by hand" idiom app/selftest/RgbDeposit.cpp §14
  // uses to give alpha lock a non-trivial starting texel. Used here so
  // `dst0` (the texel latched at a blended stroke's first touch) is a real,
  // non-transparent, non-trivial colour rather than the blank canvas's
  // (0,0,0,0) -- Multiply and Min both degenerate to "the ink, unchanged" at
  // a transparent destination (§2a, and this file's own transparent-
  // destination section), so a test that never leaves that case could not
  // tell a correct blend from a broken one.
  auto fillOpaque = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       const std::array<float, 3>& straight) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p))
            .writePixel(tileLocalOffset(p), {straight[0], straight[1], straight[2], 1.0f});
      }
  };

  // ======================================================================
  // 1. Normal: bit-identical to the old (unblended) path, and NO colour
  //    plane -- the two claims RgbDeposit.hpp §3's "half, not float" note
  //    and this task's D6 sabotage table both turn on.
  // ======================================================================
  {
    OpenDocument odOld = makeRgbDoc(128, 128);
    OpenDocument odNew = makeRgbDoc(128, 128);
    TileStore& storeOld = *odOld.document.layers[0].rgbTiles;
    TileStore& storeNew = *odNew.document.layers[0].rgbTiles;
    // A non-trivial pre-existing background on BOTH, so "bit-identical"
    // is a claim about real compositing and not merely about two blank
    // canvases agreeing on zero.
    fillOpaque(storeOld, 40, 40, 88, 88, {0.3f, 0.5f, 0.7f});
    fillOpaque(storeNew, 40, 40, 88, 88, {0.3f, 0.5f, 0.7f});

    const BrushTip t = discTip(20.0f, 0.4f, {0.9f, 0.2f, 0.6f});

    // The OLD path: the pre-existing two-argument `begin()`, which defaults
    // `blend` to Normal exactly as every caller written before this task
    // still does.
    RgbStroke strokeOld;
    strokeOld.begin(t.linearRgb, 0.7f);
    for (int i = 0; i < 8; ++i)
      strokeOld.depositDab(storeOld, t, Vec2{64.0f + 3.0f * static_cast<float>(i), 64.0f}, 128,
                           128, nullptr, nullptr);
    const size_t oldDst0Tiles = strokeOld.dst0Tiles();
    const size_t oldDst0Bytes = strokeOld.dst0Bytes();
    strokeOld.end();

    // The SAME stroke through the NEW, four-argument `begin()`, with
    // `blend` named explicitly as `Normal` rather than left to the default.
    RgbStroke strokeNew;
    strokeNew.begin(t.linearRgb, 0.7f, /*alphaLocked=*/false, BlendMode::Normal);
    for (int i = 0; i < 8; ++i)
      strokeNew.depositDab(storeNew, t, Vec2{64.0f + 3.0f * static_cast<float>(i), 64.0f}, 128,
                           128, nullptr, nullptr);
    const size_t newDst0Tiles = strokeNew.dst0Tiles();
    const size_t newDst0Bytes = strokeNew.dst0Bytes();
    strokeNew.end();

    bool identical = true;
    for (int32_t y = 30; y <= 98 && identical; ++y)
      for (int32_t x = 30; x <= 98; ++x)
        if (readAt(storeOld, x, y) != readAt(storeNew, x, y)) {
          identical = false;
          break;
        }
    std::printf("  [measured] Normal, old 2-arg begin() vs new 4-arg begin(Normal): identical "
                "over the sampled region = %s; dst0 store while active: old %zu tiles / %zu "
                "bytes, new %zu tiles / %zu bytes\n",
                identical ? "yes" : "no", oldDst0Tiles, oldDst0Bytes, newDst0Tiles, newDst0Bytes);
    check(identical,
          "Normal: the default (legacy) begin() and the explicit begin(..., BlendMode::Normal) "
          "produce a BIT-IDENTICAL store, at zero tolerance -- the Normal path is the exact "
          "code and floating-point sequence it always was, reached by construction (dispatch "
          "on blend_ != Normal), not merely by coincidence of the arithmetic");
    check(oldDst0Tiles == 0 && oldDst0Bytes == 0 && newDst0Tiles == 0 && newDst0Bytes == 0,
          "Normal: the accumulator's latched-dst0 colour plane holds ZERO tiles and ZERO bytes "
          "for a Normal stroke, WHILE THE STROKE IS STILL ACTIVE -- not merely after end() frees "
          "it, so this is a claim about the Normal path never allocating it, not about cleanup");
  }

  // ======================================================================
  // 2. Multiply, white ink, over an OPAQUE mid grey, opacity 1: the grey is
  //    unchanged -- Multiply by white is the identity.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(64, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillOpaque(store, 22, 22, 42, 42, {0.5f, 0.5f, 0.5f});

    const BrushTip t = discTip(10.0f, 1.0f, {1.0f, 1.0f, 1.0f});  // white, full flow
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/false, BlendMode::Multiply);
    stroke.depositDab(store, t, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> texel = readAt(store, 32, 32);
    std::printf("  [measured] Multiply, white ink over opaque mid grey: (%.6f %.6f %.6f a=%.6f) "
                "-- grey was (0.5 0.5 0.5 a=1)\n",
                static_cast<double>(texel[0]), static_cast<double>(texel[1]),
                static_cast<double>(texel[2]), static_cast<double>(texel[3]));
    check(texel[0] == 0.5f && texel[1] == 0.5f && texel[2] == 0.5f && texel[3] == 1.0f,
          "Multiply: white ink over opaque mid grey at opacity 1 leaves the grey EXACTLY "
          "unchanged, at zero tolerance -- multiplying by white is the identity, and both "
          "0.5 and 1.0 are exact in binary16 so there is no rounding for a bug to hide behind");
  }

  // ======================================================================
  // 3. Multiply, black ink, over mid grey, flow 1: black.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(64, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillOpaque(store, 22, 22, 42, 42, {0.5f, 0.5f, 0.5f});

    const BrushTip t = discTip(10.0f, 1.0f, {0.0f, 0.0f, 0.0f});  // black, full flow
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/false, BlendMode::Multiply);
    stroke.depositDab(store, t, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> texel = readAt(store, 32, 32);
    std::printf("  [measured] Multiply, black ink over mid grey: (%.6f %.6f %.6f a=%.6f)\n",
                static_cast<double>(texel[0]), static_cast<double>(texel[1]),
                static_cast<double>(texel[2]), static_cast<double>(texel[3]));
    check(texel[0] == 0.0f && texel[1] == 0.0f && texel[2] == 0.0f && texel[3] == 1.0f,
          "Multiply: black ink over mid grey at flow 1 gives EXACT black, opaque -- multiplying "
          "by black is the zero, at zero tolerance");
  }

  // ======================================================================
  // 4. No compounding: two dabs at flow 0.4 == one dab at the SAME final
  //    accumulator value, over a non-trivial opaque background -- Multiply
  //    and Darken.
  // ======================================================================
  //
  // The headline claim (task brief's "get this right, it is the whole
  // track"). The combined accumulator value is read back from the
  // two-dab stroke itself rather than hand-derived, so this cannot pass on
  // an arithmetic slip in the test's own expectation -- it is comparing two
  // RUNS of the real code against each other, not against a hand computation.
  for (const BlendMode mode : {BlendMode::Multiply, BlendMode::Min}) {
    const char* modeName = mode == BlendMode::Multiply ? "Multiply" : "Darken (Min)";

    OpenDocument odTwo = makeRgbDoc(64, 64);
    TileStore& storeTwo = *odTwo.document.layers[0].rgbTiles;
    fillOpaque(storeTwo, 20, 20, 44, 44, {0.65f, 0.35f, 0.55f});
    const BrushTip tTwo = discTip(10.0f, 0.4f, {0.9f, 0.2f, 0.1f});
    RgbStroke strokeTwo;
    strokeTwo.begin(tTwo.linearRgb, 1.0f, /*alphaLocked=*/false, mode);
    strokeTwo.depositDab(storeTwo, tTwo, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    strokeTwo.depositDab(storeTwo, tTwo, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    const float combined = strokeTwo.strokeAlphaAt(PixelCoord{32, 32});
    strokeTwo.end();

    OpenDocument odOne = makeRgbDoc(64, 64);
    TileStore& storeOne = *odOne.document.layers[0].rgbTiles;
    fillOpaque(storeOne, 20, 20, 44, 44, {0.65f, 0.35f, 0.55f});
    BrushTip tOne = tTwo;
    tOne.flow = combined;  // one dab, reaching the identical A' directly
    RgbStroke strokeOne;
    strokeOne.begin(tOne.linearRgb, 1.0f, /*alphaLocked=*/false, mode);
    strokeOne.depositDab(storeOne, tOne, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    const float single = strokeOne.strokeAlphaAt(PixelCoord{32, 32});
    strokeOne.end();

    const std::array<float, 4> twoTexel = readAt(storeTwo, 32, 32);
    const std::array<float, 4> oneTexel = readAt(storeOne, 32, 32);
    float worst = 0.0f;
    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(twoTexel[c] - oneTexel[c]));
    std::printf("  [measured] %s no-compounding: two dabs at flow 0.4 reach A'=%.9f, one dab "
                "at flow %.9f reaches A'=%.9f; two-dab texel (%.6f %.6f %.6f a=%.6f), one-dab "
                "texel (%.6f %.6f %.6f a=%.6f), worst |diff| %.3e (bound: one round trip "
                "through half per store, %.3e)\n",
                modeName, static_cast<double>(combined), static_cast<double>(combined),
                static_cast<double>(single), static_cast<double>(twoTexel[0]),
                static_cast<double>(twoTexel[1]), static_cast<double>(twoTexel[2]),
                static_cast<double>(twoTexel[3]), static_cast<double>(oneTexel[0]),
                static_cast<double>(oneTexel[1]), static_cast<double>(oneTexel[2]),
                static_cast<double>(oneTexel[3]), static_cast<double>(worst),
                static_cast<double>(kHalfRel));
    check(single == combined,
          "no compounding: the one-dab stroke, built to the SAME flow as the two-dab "
          "accumulator's own reading, reaches the identical A', at zero tolerance -- the "
          "premise the texel comparison below needs");
    // Both scenarios compute `out = dst0*(1-A') + target*A'` from the SAME
    // dst0 (the identical pre-painted background, latched once) and the
    // SAME A' (just asserted equal), through the SAME single half-rounding
    // at the point of storage -- so this is asserted at zero tolerance, not
    // to a derived f16 bound: a real compounding bug (blending dab 2 against
    // dab 1's ALREADY-blended write) would make the two-dab texel visibly
    // darker (Multiply) or lower (Min), not merely off by a rounding ulp.
    const std::string msg4 = std::string(modeName) +
                             ": no compounding -- two dabs at flow 0.4 and one dab at the "
                             "SAME final A' write the BIT-IDENTICAL texel, at zero tolerance "
                             "-- blending against the live tile instead of the latched dst0 "
                             "would make the two-dab run visibly darker/lower than the one-dab "
                             "run, on this non-trivial opaque background";
    check(worst == 0.0f, msg4.c_str());
  }

  // ======================================================================
  // 5. Transparent destination: Multiply/Darken over dst0.a == 0 deposits
  //    the ink colour EXACTLY -- the `lerp(ink, blend, dst0.a)` rule,
  //    produced by blendPixel()'s own arithmetic with no extra branch.
  // ======================================================================
  for (const BlendMode mode : {BlendMode::Multiply, BlendMode::Min}) {
    const char* modeName = mode == BlendMode::Multiply ? "Multiply" : "Darken (Min)";
    OpenDocument od = makeRgbDoc(64, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;  // blank: transparent everywhere

    // Ink chosen exactly representable in binary16 (powers of two) -- 0.5,
    // 0.25 and 0.125 all round-trip through half with NO rounding, so
    // "exactly" in the assertion below is a claim the storage format can
    // actually keep, not one `float` vs. half rounding would falsify on its
    // own.
    const BrushTip t = discTip(10.0f, 1.0f, {0.5f, 0.25f, 0.125f});
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/false, mode);
    stroke.depositDab(store, t, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> texel = readAt(store, 32, 32);
    std::printf("  [measured] %s over transparent dst0: (%.6f %.6f %.6f a=%.6f), ink was "
                "(0.5 0.25 0.125)\n",
                modeName, static_cast<double>(texel[0]), static_cast<double>(texel[1]),
                static_cast<double>(texel[2]), static_cast<double>(texel[3]));
    const std::string msg5 = std::string(modeName) +
                             ": over a transparent dst0, the stored texel is the INK, "
                             "EXACTLY (premultiplied at alpha 1) -- blendPixel()'s own "
                             "three-term split collapses to the source at ab == 0 with no "
                             "extra branch, and this checks that fallout rather than a "
                             "separate special case";
    check(texel[0] == t.linearRgb[0] && texel[1] == t.linearRgb[1] &&
              texel[2] == t.linearRgb[2] && texel[3] == 1.0f,
          msg5.c_str());
  }

  // ======================================================================
  // 6. Darken (Min): a texel LIGHTER than the ink -> ink; DARKER -> the
  //    texel is unchanged.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(128, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // Two opaque flats, and the ink between them -- all three exactly
    // representable in binary16 (powers of two), so "unchanged"/"becomes
    // the ink" below is a zero-tolerance claim the storage format can
    // actually keep.
    fillOpaque(store, 12, 22, 32, 42, {0.75f, 0.75f, 0.75f});  // lighter
    fillOpaque(store, 76, 22, 96, 42, {0.25f, 0.25f, 0.25f});  // darker
    const std::array<float, 3> ink{0.5f, 0.5f, 0.5f};

    const BrushTip tLight = discTip(10.0f, 1.0f, ink);
    RgbStroke sLight;
    sLight.begin(ink, 1.0f, /*alphaLocked=*/false, BlendMode::Min);
    sLight.depositDab(store, tLight, Vec2{22.5f, 32.5f}, 128, 64, nullptr, nullptr);
    sLight.end();

    const BrushTip tDark = discTip(10.0f, 1.0f, ink);
    RgbStroke sDark;
    sDark.begin(ink, 1.0f, /*alphaLocked=*/false, BlendMode::Min);
    sDark.depositDab(store, tDark, Vec2{86.5f, 32.5f}, 128, 64, nullptr, nullptr);
    sDark.end();

    const std::array<float, 4> lighter = readAt(store, 22, 32);
    const std::array<float, 4> darker = readAt(store, 86, 32);
    std::printf("  [measured] Darken: over lighter (0.75) -> (%.4f); over darker (0.25) -> "
                "(%.4f); ink was 0.5\n",
                static_cast<double>(lighter[0]), static_cast<double>(darker[0]));
    check(lighter[0] == 0.5f && lighter[1] == 0.5f && lighter[2] == 0.5f,
          "Darken: a texel LIGHTER than the ink (0.75 vs ink 0.5) becomes the ink -- Darken "
          "keeps whichever is darker, at zero tolerance");
    check(darker[0] == 0.25f && darker[1] == 0.25f && darker[2] == 0.25f,
          "Darken: a texel DARKER than the ink (0.25 vs ink 0.5) is UNCHANGED -- the texel was "
          "already the darker of the two, at zero tolerance");
  }

  // ======================================================================
  // 7. Alpha lock + Multiply: alpha UNCHANGED, colour moves per the
  //    re-derived §4.5 rule (RgbDeposit.hpp §2a): out.rgb = dst0*(1-A') +
  //    target*A'*dst0.a, out.a = dst0.a.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(64, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;
    od.document.layers[0].alphaLocked = true;
    // A PARTIALLY transparent, non-uniform starting texel -- straight
    // (0.5,0.5,0.5) at alpha 0.5, so a bound (dst.a creeping) and a freeze
    // (dst.a exactly preserved) would read differently, exactly
    // app/selftest/RgbDeposit.cpp §14's own reasoning for picking neither 0
    // nor 1 -- and, again, every value here (0.5, and the 0.25 premultiplied
    // product) is exact in binary16, so the "EXACTLY"/"zero tolerance"
    // claims below are claims the storage format can actually keep.
    {
      const PixelCoord p{32, 32};
      store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {0.25f, 0.25f, 0.25f, 0.5f});
    }
    const BrushTip t = discTip(10.0f, 1.0f, {1.0f, 1.0f, 1.0f});  // white, full flow
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/true, BlendMode::Multiply);
    stroke.depositDab(store, t, Vec2{32.5f, 32.5f}, 64, 64, nullptr, nullptr);
    stroke.end();

    const std::array<float, 4> texel = readAt(store, 32, 32);
    // By hand, from §2a's re-derivation: dst0 = (0.25,0.25,0.25,0.5),
    // straight(dst0) = 0.5; Multiply(0.5, white) = 0.5 (white is the
    // identity); target = lerp(1.0, 0.5, 0.5) = 0.75 -- verified against
    // blendPixel()'s own arithmetic, not merely asserted. At flow 1 /
    // opacity 1, A' = 1, so out.rgb = dst0*(1-1) + target*1*dst0.a =
    // 0.75*0.5 = 0.375, out.a = dst0.a = 0.5 (frozen).
    std::printf("  [measured] alpha lock + Multiply: (%.6f %.6f %.6f a=%.6f) -- started at "
                "(0.25 0.25 0.25 a=0.5), by-hand expectation (0.375 ... a=0.5)\n",
                static_cast<double>(texel[0]), static_cast<double>(texel[1]),
                static_cast<double>(texel[2]), static_cast<double>(texel[3]));
    check(texel[3] == 0.5f,
          "alpha lock + Multiply: alpha is EXACTLY 0.5 (frozen) after the dab, at zero "
          "tolerance -- dst0[3] is copied through unchanged by §2a's re-derived rule, "
          "identically to §4.5's unblended one");
    check(texel[0] == 0.375f && texel[1] == 0.375f && texel[2] == 0.375f,
          "alpha lock + Multiply: the COLOUR moved to the EXACT by-hand value from §2a's "
          "re-derived formula (dst0*(1-A') + target*A'*dst0.a) -- a lock that still paints, "
          "not a second `locked`, and not §4.5's unblended answer (which would use `ink` "
          "directly in place of `target` and land at 0.5, not 0.375)");
  }

  // ======================================================================
  // 8. Half-selected texel: A' caps at opacity * sel EXACTLY as the
  //    unblended path does (RgbDeposit.hpp §4) -- the selection's two
  //    entries into the rule are unaware of, and unaffected by, the blend
  //    mode, because depositRgbTexelBlended() duplicates the identical
  //    accumulator arithmetic depositRgbTexel() has.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(64, 64);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillOpaque(store, 10, 10, 54, 54, {0.5f, 0.5f, 0.5f});
    // A fractional edge, so the selection at the probed texel is neither 0
    // nor 1 -- the same discriminating shape app/selftest/RgbDeposit.cpp §8
    // uses.
    Selection sel = selectRectangle(32.25f, 0.0f, 64.0f, 64.0f);
    const float partial = selectionCoverageAt(&sel, PixelCoord{32, 32});

    const BrushTip t = discTip(20.0f, 1.0f, {0.1f, 0.9f, 0.1f});
    RgbStroke stroke;
    stroke.begin(t.linearRgb, 1.0f, /*alphaLocked=*/false, BlendMode::Multiply);
    for (int i = 0; i < 40; ++i)  // scrub well past what an unbounded model would need
      stroke.depositDab(store, t, Vec2{32.0f, 32.0f}, 64, 64, &sel, nullptr);
    const float scrubbed = stroke.strokeAlphaAt(PixelCoord{32, 32});
    stroke.end();

    std::printf("  [measured] Multiply, 40 dabs through a partial selection (coverage %.6f): "
                "accumulator reaches %.9f (opacity * sel = %.9f)\n",
                static_cast<double>(partial), static_cast<double>(scrubbed),
                static_cast<double>(1.0f * partial));
    check(scrubbed == partial,
          "selection under Multiply: 40 dabs through a partially selected texel reach EXACTLY "
          "opacity * sel and no further, at zero tolerance -- the blend mode does not change "
          "the selection's role as a BOUND, only what the write at that bound looks like");
  }

  // ======================================================================
  // 9. The latched-dst0 colour plane is freed at end() -- the memory claim
  //    RgbDeposit.hpp §3's "§2a's second store" note makes, checked rather
  //    than trusted, mirroring app/selftest/RgbDeposit.cpp §13's own check
  //    for the alpha accumulator.
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillOpaque(store, 20, 20, 236, 236, {0.4f, 0.4f, 0.4f});

    RgbStroke probe;
    probe.begin({0.9f, 0.1f, 0.1f}, 1.0f, /*alphaLocked=*/false, BlendMode::Multiply);
    const BrushTip wide = discTip(40.0f, 0.5f, {0.9f, 0.1f, 0.1f});
    for (int i = 0; i < 4; ++i)
      probe.depositDab(store, wide, Vec2{60.0f + 60.0f * static_cast<float>(i), 128.0f}, 256, 256,
                       nullptr, nullptr);
    const size_t liveTiles = probe.dst0Tiles();
    const size_t liveBytes = probe.dst0Bytes();
    probe.end();
    std::printf("  [measured] latched-dst0 store while painting: %zu tiles, %zu KiB; after "
                "pen-up: %zu tiles, %zu KiB\n",
                liveTiles, liveBytes / 1024, probe.dst0Tiles(), probe.dst0Bytes() / 1024);
    check(liveTiles > 0 && liveBytes == liveTiles * sizeof(Tile),
          "dst0 store: a BLENDED stroke in flight holds exactly one 128 KiB core::Tile per "
          "tile it has touched -- sparse, allocated only because this stroke is not Normal");
    check(probe.dst0Tiles() == 0 && probe.dst0Bytes() == 0,
          "dst0 store: pen-up frees ALL of it -- an application sitting idle after a long "
          "blended stroke must not still be holding the stroke's latched colour plane");
  }

  std::printf("[selftest] brush blend mode %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
