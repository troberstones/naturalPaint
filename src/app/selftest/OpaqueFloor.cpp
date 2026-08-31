#include "app/selftest/Support.hpp"

namespace np {

// core/Composite.cpp's opaque-floor early exit: walking a tile's layer stack
// bottom to top, everything strictly below the topmost Normal-blended layer
// (or clip base, or Mix pair) whose own effective alpha is exactly 1.0
// everywhere in that tile can be skipped, because `blendPixel(Normal, src,
// dst) == src` bit-for-bit whenever `src[3] == 1.0f` -- so the accumulator's
// zero-initialized start already IS the correct seed for a walk truncated at
// that layer. See core/Composite.cpp's own section on it for the derivation.
//
// **Why every comparison below is `sameFloats`/`memcmp`, never a tolerance.**
// This is the identical discipline app/selftest/IncrementalComposite.cpp
// already established for the incremental composite: the claim is bit-
// identity, not agreement, because the optimization is only worth having if
// it is invisible to every caller. A "close enough" comparison here would
// hide precisely the class of bug this section exists to catch.
bool runOpaqueFloorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto writeRgbFull = [](Document& doc, size_t layerIndex, int32_t w, int32_t h,
                         const std::array<float, 4>& premultiplied) {
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex]
            .rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at), premultiplied);
      }
  };
  auto writePigmentFull = [](Document& doc, size_t layerIndex, int32_t w, int32_t h,
                             const PigmentTexel& texel) {
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex]
            .pigmentTiles->getOrCreate(tileCoordAt(at))
            .writeTexel(tileLocalOffset(at), texel);
      }
  };
  auto writeMaskHalf = [](Document& doc, size_t layerIndex, int32_t w, int32_t h, float v) {
    // Masks only the RIGHT half of the canvas to `v`; the left half stays at
    // the mask store's own default (1.0, "reveal" -- core/Mask.hpp), so a
    // layer masked this way is NOT fully-revealing everywhere, on purpose.
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = w / 2; x < w; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex]
            .mask->getOrCreate(tileCoordAt(at))
            .writeCoverage(tileLocalOffset(at), v);
      }
  };
  auto sameFloats = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };

  constexpr int32_t kW = 256;  // 2x2 tiles -- enough to prove "every texel of
  constexpr int32_t kH = 256;  // the tile", not just one, without the O(N)
                                // per-layer scan costing anything real here.

  // A "kitchen sink" stack: several semi-transparent RGB layers below index
  // `kCandidate`, so a layer wrongly skipped (or wrongly NOT skipped) below
  // the real floor changes the picture; several layers of MIXED kind/blend
  // above it (Multiply, an Adjustment layer, another semi-transparent
  // Normal layer), so the test also proves layers above the floor are
  // completely unaffected by the optimization regardless of their own kind
  // or blend mode. `configureCandidate` sets up layer `kCandidate` -- kind,
  // blend, opacity, mask, tile content -- to control whether it should or
  // should not qualify as a floor; some configurations (the clip-base and
  // Mix-pair cases) insert an EXTRA layer of their own, so every "above"
  // layer's index below is computed relative to `doc.layers.size()` AFTER
  // `configureCandidate` runs, never hardcoded -- a fixed index here would
  // silently configure the wrong layer the moment a configurator's own
  // layer count differs from the plain single-candidate case.
  constexpr size_t kCandidate = 3;
  auto buildStack = [&](auto&& configureCandidate) {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    writeRgbFull(doc, 0, kW, kH, {0.6f, 0.05f, 0.05f, 0.5f});      // 0: below, semi-transparent
    addLayer(doc, doc.layers.size(), makeRgbLayer("below-1"));
    writeRgbFull(doc, 1, kW, kH, {0.05f, 0.6f, 0.05f, 0.4f});      // 1: below, semi-transparent
    addLayer(doc, doc.layers.size(), makeRgbLayer("below-2"));
    writeRgbFull(doc, 2, kW, kH, {0.05f, 0.05f, 0.6f, 0.3f});      // 2: below, semi-transparent
    addLayer(doc, doc.layers.size(), makeRgbLayer("candidate"));  // 3: kCandidate
    configureCandidate(doc);
    const size_t aboveMultiply = doc.layers.size();
    addLayer(doc, doc.layers.size(), makeRgbLayer("above-multiply"));
    setLayerBlend(doc, aboveMultiply, BlendMode::Multiply);
    writeRgbFull(doc, aboveMultiply, kW, kH, {0.8f, 0.8f, 0.5f, 0.5f});  // above, Multiply
    const size_t aboveAdjust = doc.layers.size();
    addLayer(doc, doc.layers.size(), makeAdjustmentLayer("above-adjust"));
    {
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = PointOpKind::Exposure;
      op.exposure.stops = 0.7f;
      doc.layers[aboveAdjust].ops.add(op);  // above, Adjustment
    }
    const size_t aboveNormal = doc.layers.size();
    addLayer(doc, doc.layers.size(), makeRgbLayer("above-normal"));
    writeRgbFull(doc, aboveNormal, kW, kH, {0.1f, 0.7f, 0.9f, 0.3f});  // above, Normal, semi-transp.
    return doc;
  };

  // The single comparison every case below reduces to: the SAME document,
  // composited with the optimization forced off and then with it at its
  // default (on), must produce bit-identical premultiplied buffers.
  auto proveBitIdenticalOnOff = [&](const Document& doc, const char* what) {
    setOpaqueFloorEnabledForTesting(false);
    const std::vector<float> withoutFloor = compositeDocumentPremultiplied(doc);
    setOpaqueFloorEnabledForTesting(true);
    const std::vector<float> withFloor = compositeDocumentPremultiplied(doc);
    check(sameFloats(withoutFloor, withFloor), what);
  };

  std::printf("  -- positive cases: a genuine floor is used, and changes nothing --\n");

  // 1. A fully-opaque RGB Normal layer partway up the stack.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
    });
    proveBitIdenticalOnOff(
        doc, "RGB Normal layer, alpha 1.0 everywhere: floor on/off is bit-identical");
  }

  // 2. A fully-opaque Pigment layer partway up the stack.
  {
    Document doc = buildStack([&](Document& d) {
      removeLayer(d, kCandidate);
      addLayer(d, kCandidate, makePigmentLayer("candidate-pigment"));
      PigmentTexel t;
      t.latent.c = {0.3f, 0.2f, 0.1f};
      t.mass = 1.0f;
      writePigmentFull(d, kCandidate, kW, kH, t);
    });
    proveBitIdenticalOnOff(
        doc, "Pigment layer, mass 1.0 everywhere: floor on/off is bit-identical");
  }

  // 3. A clip base whose folded group is fully opaque -- the base itself is
  // opaque (alpha 1 everywhere), and a Multiply-blended clip MEMBER sits on
  // top of it, on purpose: `clipGroupClose()` assigns the closed group's
  // alpha to exactly the base's own effective alpha regardless of the
  // member's blend, so this proves qualification does not depend on the
  // member at all -- only on the base.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.4f, 0.4f, 0.9f, 1.0f});  // the base, opaque
      addLayer(d, kCandidate + 1, makeRgbLayer("clip-member"));
      writeRgbFull(d, kCandidate + 1, kW, kH, {0.9f, 0.9f, 0.1f, 0.6f});
      setLayerBlend(d, kCandidate + 1, BlendMode::Multiply);
      setLayerClipped(d, kCandidate + 1, true);
    });
    proveBitIdenticalOnOff(
        doc, "clip base with a Multiply member, base alpha 1.0 everywhere: floor on/off matches");
  }

  // 4. A Mix pair whose combined result is fully opaque.
  {
    Document doc = buildStack([&](Document& d) {
      removeLayer(d, kCandidate);
      addLayer(d, kCandidate, makePigmentLayer("mix-lower"));
      PigmentTexel lo;
      lo.latent.c = {0.5f, 0.1f, 0.1f};
      lo.mass = 1.0f;
      writePigmentFull(d, kCandidate, kW, kH, lo);
      addLayer(d, kCandidate + 1, makePigmentLayer("mix-upper"));
      PigmentTexel hi;
      hi.latent.c = {0.1f, 0.5f, 0.1f};
      hi.mass = 1.0f;
      writePigmentFull(d, kCandidate + 1, kW, kH, hi);
      setLayerBlend(d, kCandidate + 1, BlendMode::Mix);
    });
    check(mixPairing(doc).mixedWithBelow[kCandidate + 1],
          "setup: the mix layer above actually paired (blendModeAvailableForLayer held)");
    proveBitIdenticalOnOff(
        doc, "Mix pair, combined mass 1.0 everywhere: floor on/off is bit-identical");
  }

  std::printf("  -- negative cases: each disqualifier is honoured, not just accepted --\n");

  // A fully-opaque Multiply layer must NOT be treated as a floor: it still
  // reads and darkens the backdrop even at alpha 1.0 (core/Blend.hpp's
  // general separable-blend formula -- only `over`'s B(Cs,Cb)=Cs collapses
  // the backdrop term to zero). If the search wrongly picked it, this
  // document's lower, painted layers would vanish from the result.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
      setLayerBlend(d, kCandidate, BlendMode::Multiply);
    });
    proveBitIdenticalOnOff(
        doc, "fully-opaque Multiply layer is never a floor: floor on/off is bit-identical");
  }

  // An Adjustment layer has no alpha of its own and must never be picked.
  {
    Document doc = buildStack([&](Document& d) {
      removeLayer(d, kCandidate);
      addLayer(d, kCandidate, makeAdjustmentLayer("candidate-adjustment"));
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = PointOpKind::Exposure;
      op.exposure.stops = 1.3f;
      d.layers[kCandidate].ops.add(op);
    });
    proveBitIdenticalOnOff(
        doc, "an Adjustment layer is never a floor: floor on/off is bit-identical");
  }

  // Alpha 1.0 everywhere in the tile, but opacity < 1.0: the layer's own
  // EFFECTIVE alpha is not 1.0, so it must not qualify.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
      setLayerOpacity(d, kCandidate, 0.999f);
    });
    proveBitIdenticalOnOff(
        doc, "alpha 1.0 but opacity < 1.0: never a floor -- floor on/off is bit-identical");
  }

  // Alpha 1.0 everywhere, opacity 1.0, but a mask that does NOT reveal
  // everywhere (the right half of the canvas at 0.5): the layer's effective
  // alpha is 1.0 on the left half of the tile and 0.5 on the right, so
  // "everywhere" fails and it must not qualify.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
      addLayerMask(d, kCandidate);
      writeMaskHalf(d, kCandidate, kW, kH, 0.5f);
    });
    proveBitIdenticalOnOff(
        doc, "alpha 1.0 but a partial mask: never a floor -- floor on/off is bit-identical");
  }

  // A clip MEMBER that happens to be locally opaque must never be picked
  // either -- only its base can be. The base here is deliberately
  // semi-transparent, so if the member were wrongly treated as a floor
  // (skipping the base and everything below it) the result would visibly
  // differ from compositing the group as PRD C9 defines it.
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.4f, 0.4f, 0.9f, 0.5f});  // the base, semi-transparent
      addLayer(d, kCandidate + 1, makeRgbLayer("locally-opaque-member"));
      writeRgbFull(d, kCandidate + 1, kW, kH, {0.9f, 0.9f, 0.1f, 1.0f});  // opaque, but a MEMBER
      setLayerClipped(d, kCandidate + 1, true);
    });
    proveBitIdenticalOnOff(
        doc, "a locally-opaque clip MEMBER (not the base) is never a floor: on/off matches");
  }

  std::printf("  -- sabotage: a WRONG floor decision really would be caught --\n");

  // The above cases exercise the real, presumably-correct implementation
  // both ways and show no difference -- which is only informative if a
  // *wrong* floor decision would actually produce a difference the same
  // comparison catches. Rather than patching production code (which would
  // have to be un-patched before this binary's own `--selftest` run could
  // pass), this reproduces exactly what an incorrectly-implemented floor
  // search would do to the ACCUMULATOR: erase every layer below a chosen
  // index from a COPY of the document, and composite that truncated copy
  // through the real, unmodified compositor. For a genuine floor this must
  // still equal the full composite (an independent cross-check of the cases
  // above, by document surgery instead of the on/off toggle); for a layer
  // this section's own negative cases just proved is NOT a safe floor, the
  // truncated composite must DISAGREE with the full one -- proving that if
  // `floorFor()` ever mistakenly picked such a layer, this section's own
  // bit-identity style of assertion would turn red, not stay silently green.
  auto compositeAsIfFloorWereAt = [&](const Document& doc, size_t forcedFloorIndex) {
    Document truncated = doc;
    for (size_t i = forcedFloorIndex; i-- > 0;) removeLayer(truncated, 0);
    return compositeDocumentPremultiplied(truncated);
  };

  {
    // A genuine floor (case 1's fixture): document surgery agrees with the
    // real optimization, which is the same claim proveBitIdenticalOnOff
    // already made a different way.
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
    });
    check(sameFloats(compositeAsIfFloorWereAt(doc, kCandidate),
                     compositeDocumentPremultiplied(doc)),
          "sabotage control: truncating at a GENUINE floor still matches the full composite");
  }
  {
    // The Multiply negative case: forcing a truncation at that layer (i.e.
    // simulating the exact mistake "treat a fully-opaque Multiply layer as
    // a floor") must NOT match the full composite -- proving that mistake
    // is visible, not silently absorbed by the comparison style this
    // section relies on throughout.
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
      setLayerBlend(d, kCandidate, BlendMode::Multiply);
    });
    check(!sameFloats(compositeAsIfFloorWereAt(doc, kCandidate),
                      compositeDocumentPremultiplied(doc)),
          "sabotage: truncating at a fully-opaque MULTIPLY layer is caught -- disagrees");
  }
  {
    // The partial-mask negative case, the same way.
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
      addLayerMask(d, kCandidate);
      writeMaskHalf(d, kCandidate, kW, kH, 0.5f);
    });
    check(!sameFloats(compositeAsIfFloorWereAt(doc, kCandidate),
                      compositeDocumentPremultiplied(doc)),
          "sabotage: truncating at a partially-masked opaque layer is caught -- disagrees");
  }

  std::printf("  -- stale-cache proof: painting over an opaque tile invalidates it --\n");

  // The cache lives inside TileStoreOf and is invalidated at every write-
  // capable call (core/TileStore.hpp's own barrier enumeration); this proves
  // that story end to end rather than trusting it from the header comment
  // alone. A DISTINCT layer sits directly below the candidate so that a
  // wrongly-stale "still opaque" answer would show through as the WRONG
  // colour at the punched-out texel, not merely "no visible change".
  {
    Document doc = buildStack([&](Document& d) {
      writeRgbFull(d, kCandidate, kW, kH, {0.9f, 0.1f, 0.1f, 1.0f});
    });
    // Warm the cache: composite once with the real document, which is
    // exactly the call that populates `coverageCache_` for the candidate's
    // tile (0,0) via the real `tileAlphaIsOneEverywhere` predicate --
    // deliberately not probed here with a second, different predicate of
    // this test's own: `tileSatisfiesEverywhere()`'s cache is keyed by
    // coordinate alone, not by which predicate produced the cached answer,
    // so querying it with an unrelated predicate would contaminate the
    // entry the real walk depends on. Every production call site uses
    // exactly one fixed predicate per store type, which is what keeps that
    // sharp edge out of reach in the running application; see
    // core/TileStore.hpp's own comment on `coverageCache_`.
    (void)compositeDocumentPremultiplied(doc);
    check(doc.layers[kCandidate].rgbTiles->find(TileCoord{0, 0}) != nullptr,
          "setup: the candidate's tile (0,0) exists, so the cache had something to warm");

    // Punch a single fully-transparent texel into the candidate's tile --
    // still within tile (0,0), still not touching any other tile -- so the
    // ONLY thing that can make the next composite correct is the cache
    // noticing this specific write.
    const PixelCoord punch{5, 5};
    doc.layers[kCandidate]
        .rgbTiles->getOrCreate(tileCoordAt(punch))
        .writePixel(tileLocalOffset(punch), {0.0f, 0.0f, 0.0f, 0.0f});

    const std::vector<float> afterPunchWithFloor = compositeDocumentPremultiplied(doc);
    setOpaqueFloorEnabledForTesting(false);
    const std::vector<float> afterPunchGroundTruth = compositeDocumentPremultiplied(doc);
    setOpaqueFloorEnabledForTesting(true);
    check(sameFloats(afterPunchWithFloor, afterPunchGroundTruth),
          "stale-cache: a composite AFTER the punch matches ground truth -- invalidation worked");

    // And the punched texel must actually show the layer below through --
    // not the candidate's own opaque marker colour -- which is the visible
    // symptom a stale cache would have produced (the below-3 layers would
    // stay hidden, so the pixel would still read the candidate's own red).
    const size_t punchOffset =
        (static_cast<size_t>(punch.y) * static_cast<size_t>(kW) + static_cast<size_t>(punch.x)) *
        4u;
    check(afterPunchWithFloor[punchOffset] < 0.85f,
          "stale-cache: the punched texel no longer reads the candidate's own opaque colour");
  }

  std::printf("  -- performance sanity (printed, not asserted) --\n");

  // A larger, deeper stack approximating the motivating profile: a near-
  // full-canvas opaque RGB layer close to the top of a many-layer stack.
  // Smaller than the live 5000x2559/50-layer document this optimization was
  // profiled against -- `--selftest` has to stay fast -- so this is a sanity
  // check that the mechanism actually saves work, not a reproduction of the
  // live number. `--profile-toggle` against the real document is the
  // faithful measurement and is not driven from here; see this function's
  // own report for what could and could not be measured headlessly.
  {
    constexpr int32_t kBigW = 2048;
    constexpr int32_t kBigH = 2048;
    constexpr size_t kLayerCount = 40;
    constexpr size_t kFloorAt = 32;  // near the top, matching the profiled shape
    Document big = Document::createBlank(kBigW, kBigH, WorkingSpace{});
    for (size_t i = 0; i < kLayerCount; ++i) {
      if (i > 0) addLayer(big, big.layers.size(), makeRgbLayer("layer"));
      writeRgbFull(big, i, kBigW, kBigH,
                   {0.1f * static_cast<float>(i % 7), 0.2f, 0.3f, i == kFloorAt ? 1.0f : 0.4f});
    }

    const int kReps = 5;
    setOpaqueFloorEnabledForTesting(false);
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) (void)compositeDocumentPremultiplied(big);
    auto t1 = std::chrono::steady_clock::now();
    const double withoutMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;

    setOpaqueFloorEnabledForTesting(true);
    auto t2 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) (void)compositeDocumentPremultiplied(big);
    auto t3 = std::chrono::steady_clock::now();
    const double withMs = std::chrono::duration<double, std::milli>(t3 - t2).count() / kReps;

    std::printf(
        "    %zu-layer %dx%d, opaque floor at index %zu of %zu: %.2f ms without -> %.2f ms with "
        "(%.1fx)\n",
        kLayerCount, kBigW, kBigH, kFloorAt, kLayerCount, withoutMs, withMs,
        withMs > 0.0 ? withoutMs / withMs : 0.0);
  }

  std::printf("  -- constants: the half-float encoding this section's raw-word checks rely on --\n");
  check(floatToHalf(1.0f) == 0x3C00,
        "binary16's canonical 1.0 word is 0x3C00 -- what core/Composite.cpp's "
        "tileAlphaIsOneEverywhere()/pigmentTileMassIsOneEverywhere() compare against");

  std::printf("[selftest] opaque floor %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
