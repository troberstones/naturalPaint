#include "app/selftest/Support.hpp"

namespace np {

// ---------------------------------------------------------------------------
// PLAN.md Phase 5 step 3 -- Pigment layers: latent x mass tile storage at f16,
// the latent -> RGB projection, the per-layer op stack that applies *after* it,
// and `Mix`. See app/SelfTest.hpp for the section's own contents list.
// ---------------------------------------------------------------------------
bool runPigmentLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };


  // --- Tolerances, each derived here rather than borrowed ----------------
  //
  //  * **kHalfRel / kHalfFloor -- the f16 latent storage bound.** This is the
  //    one the task of reusing an existing number would have got wrong.
  //    binary16 has an 11-bit significand (10 stored plus the implicit one),
  //    so for a normal value the ulp is between 2^-11 and 2^-10 of the value
  //    and round-to-nearest gives at most half of it: a **relative** error of
  //    2^-11 = 4.883e-04. That is nearly four orders of magnitude looser than
  //    the 1.0e-7 the RGB paths use for their un-premultiply, which is a
  //    single float division -- applying the float number here would fail on
  //    correct data, and applying this one there would hide a real regression.
  //    Below binary16's smallest normal (2^-14) the spacing stops shrinking,
  //    so an **absolute** floor of half a subnormal ulp, 2^-25 = 2.980e-08,
  //    is added; a latent residual is routinely that small.
  //  * **kProjectionTol -- the latent -> RGB round trip.** Unchanged from
  //    runBlendTest()'s derivation, restated because it is not obvious:
  //    `rgbToLatent()` *defines* the residual as `rgb - pigmentPolynomialRgb(c)`
  //    and `latentToRgb()` returns `pigmentPolynomialRgb(c) + res` from the
  //    same polynomial, so the whole trip is `p + (r - p)` -- two correctly-
  //    rounded operations on magnitudes <= 1, at most 2 ulps of 1.0 =
  //    1.19e-07. Bounded at 5.0e-7, 4.2x that. **NP_USE_MIXBOX=OFF only**:
  //    this derivation is specific to Mixbox's fit-plus-residual construction
  //    and does not hold for the KM2 fallback, which clamps a channel at or
  //    near 0 or 1 to `MixboxLut::kKm2ReflectanceFloor` before taking
  //    Kubelka's K/S ratio (paint/Palette.cpp) -- a real, bounded loss for a
  //    pigment with an exact-0 or exact-1 channel (both palette pigments this
  //    section uses have one), not float noise. See runBlendTest()'s own
  //    `#else` for the identical derivation stated once more.
  //
  // Everything else in this section is asserted at **exactly zero tolerance**,
  // and where it is, it is because the reference is computed from the values
  // actually read back out of the f16 tiles through the same functions the
  // walk calls -- not because the arithmetic happens to be tidy.
  constexpr float kHalfRel = 4.8828125e-04f;   // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f; // 2^-25
#if defined(NP_USE_MIXBOX)
  constexpr float kProjectionTol = 5.0e-7f;
#else
  constexpr float kProjectionTol = MixboxLut::kKm2ReflectanceFloor + 1.0e-6f;
#endif
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  auto nearHalf = [&](float got, float want) {
    return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]};
  };
  auto writePigment = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                         const Latent& z, float mass) {
    PigmentTileStore& tiles = *doc.layers[layerIndex].pigmentTiles;
    const PixelCoord at{x, y};
    PigmentTexel t;
    t.latent = z;
    t.mass = mass;
    tiles.getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), t);
  };
  auto readPigment = [](const Document& doc, size_t layerIndex, int32_t x,
                        int32_t y) -> PigmentTexel {
    const PixelCoord at{x, y};
    const PigmentTile* tile = doc.layers[layerIndex].pigmentTiles->find(tileCoordAt(at));
    return tile ? tile->readTexel(tileLocalOffset(at)) : PigmentTexel{};
  };
  auto tileBytes = [](const Document& doc, size_t layerIndex,
                      TileCoord coord) -> std::vector<uint16_t> {
    const PigmentTile* tile = doc.layers[layerIndex].pigmentTiles->find(coord);
    if (tile == nullptr) return {};
    return std::vector<uint16_t>(tile->data(), tile->data() + PigmentTile::kTexelCount);
  };
  auto addRgbLayer = [](Document& doc, std::string name) {
    const LayerOpResult r = addLayer(doc, doc.layers.size(), makeRgbLayer(std::move(name)));
    return r.ok;
  };
  auto exposureOp = [](float stops) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = stops;
    return op;
  };

  // --- 1. The tile: shape, cost, sparsity, and the f16 round trip --------
  {
    check(PigmentTile::kChannels == 7,
          "tile: a pigment tile stores SEVEN channels -- pig.c0/c1/c2/m and res.R/G/B, "
          "docs/document-format.md's own list, with the fourth pigment weight derived");
    check(sizeof(PigmentTile) == 224 * 1024 && sizeof(Tile) == 128 * 1024,
          "tile: 224 KiB against an RGBA tile's 128 KiB -- 1.75x, under "
          "DESIGN-imaging.md 3's 256 KiB budget row for a pigment tile");
    std::printf("  [measured] pigment tile %zu KiB vs. rgba16float tile %zu KiB (%.2fx)\n",
                sizeof(PigmentTile) / 1024, sizeof(Tile) / 1024,
                static_cast<double>(sizeof(PigmentTile)) / static_cast<double>(sizeof(Tile)));

    PigmentTileStore store;
    check(store.occupiedTileCount() == 0 && store.find(TileCoord{0, 0}) == nullptr,
          "tile: a fresh store allocates nothing and find() does not allocate -- PRD C2, and "
          "the same contract TileStoreOf gives core::Tile because it is the same template");
    store.getOrCreate(TileCoord{3, -2});
    check(store.occupiedTileCount() == 1 && store.find(TileCoord{3, -2}) != nullptr &&
              store.find(TileCoord{0, 0}) == nullptr,
          "tile: allocate-on-write puts one tile at a negative-coordinate key and no others");
    const PigmentTexel blank = store.find(TileCoord{3, -2})->readTexel(PixelCoord{7, 9});
    check(blank.mass == 0.0f && blank.latent.c == std::array<float, 3>{0.0f, 0.0f, 0.0f} &&
              blank.latent.res == std::array<float, 3>{0.0f, 0.0f, 0.0f},
          "tile: an untouched texel is mass 0 with a zero latent -- 'no pigment here', the "
          "exact analogue of core::Tile's transparent black");

    // Exact case first: every component a dyadic rational inside binary16's
    // range, so the round trip is not merely within a bound, it is equality.
    // A tolerance-only assertion here would pass against a store that
    // quietly rounded to 8 bits.
    PigmentTexel dyadic;
    dyadic.latent.c = {0.25f, 0.5f, 0.125f};
    dyadic.latent.res = {0.0625f, -0.125f, 0.375f};
    dyadic.mass = 0.75f;
    store.getOrCreate(TileCoord{0, 0}).writeTexel(PixelCoord{1, 2}, dyadic);
    check(store.find(TileCoord{0, 0})->readTexel(PixelCoord{1, 2}) == dyadic,
          "tile: a latent of exactly-representable values round-trips through f16 storage "
          "EXACTLY -- not within a tolerance");
    check(store.find(TileCoord{0, 0})->readTexel(PixelCoord{1, 3}) == PigmentTexel{},
          "tile: and writing one texel leaves its neighbour untouched, so the seven channels "
          "are indexed per texel rather than per tile");
  }

  // --- 2. The projection, against the real Mixbox LUT --------------------
  MixboxLut lut;
  const bool lutLoaded = pigmentSourceReady(lut, mixboxLutPath().c_str());
  check(lutLoaded,
        "projection: this build's pigment source is live and answering with real data -- "
        "everything below asserts against measured "
        "pigment data, not a stand-in");
  const Pigment& yellowPigment = defaultPalette()[0];
  const Pigment& bluePigment = defaultPalette()[7];
  const Latent zYellow = lut.rgbToLatent(yellowPigment.rgb[0], yellowPigment.rgb[1],
                                          yellowPigment.rgb[2]);
  const Latent zBlue = lut.rgbToLatent(bluePigment.rgb[0], bluePigment.rgb[1],
                                        bluePigment.rgb[2]);
  {
    // The projection moved out of MixboxLut into core/Pigment at this step and
    // must still be the same function to the bit -- the exactness below is
    // what would break first if the polynomial had been retyped.
    float worst = 0.0f;
    const std::array<float, 3> backY = latentToRgb(zYellow);
    const std::array<float, 3> backB = latentToRgb(zBlue);
    for (int i = 0; i < 3; ++i) {
      worst = std::fmax(worst, std::fabs(backY[i] - yellowPigment.rgb[i]));
      worst = std::fmax(worst, std::fabs(backB[i] - bluePigment.rgb[i]));
    }
    std::printf("  [measured] latentToRgb(rgbToLatent(p)) on two palette pigments: max "
                "residual = %.3e (bound %.3e)\n",
                static_cast<double>(worst), static_cast<double>(kProjectionTol));
    check(lutLoaded && worst <= kProjectionTol,
          "projection: core/Pigment's latentToRgb() reproduces the pigments rgbToLatent() "
          "was given -- the polynomial moved out of paint/Palette unchanged");
#if defined(NP_USE_MIXBOX)
    check(latentToRgb(Latent{}) == pigmentPolynomialRgb(std::array<float, 3>{0.0f, 0.0f, 0.0f}),
          "projection: it needs no LUT and no state -- a zero latent projects with nothing "
          "loaded, which is why core/ can own it and paint/ keeps only the inverse");
#else
    // KM2 basis: there is no separate polynomial to compare `latentToRgb()`
    // against (its whole point is that it is closed-form, see
    // core/Pigment.cpp), so the property under test -- "needs no LUT and no
    // state" -- is checked directly: the all-zero latent (K=0, S=0 in every
    // channel, core/Pigment.cpp's guarded degenerate case) still projects to
    // a finite, in-range colour with nothing loaded, rather than a NaN or a
    // crash from dividing by S=0.
    const auto z0 = latentToRgb(Latent{});
    bool finiteAndInRange = true;
    for (float v : z0)
      if (!std::isfinite(v) || v < 0.0f || v > 1.0f) finiteAndInRange = false;
    check(finiteAndInRange,
          "projection: it needs no LUT and no state -- a zero latent projects with nothing "
          "loaded, which is why core/ can own it and paint/ keeps only the inverse");
#endif

    // Latent -> f16 -> latent -> RGB, which is what a composite actually does.
    // Bounded by the derived f16 figure, and the measurement is printed so the
    // bound can be seen to be a bound and not a fitted number.
    PigmentTile tile;
    PigmentTexel t;
    t.latent = zYellow;
    t.mass = 1.0f;
    tile.writeTexel(PixelCoord{0, 0}, t);
    const PigmentTexel back = tile.readTexel(PixelCoord{0, 0});
    float worstStore = 0.0f;
    bool storedInBound = true;
    for (int i = 0; i < 3; ++i) {
      worstStore = std::fmax(worstStore, std::fabs(back.latent.c[i] - zYellow.c[i]));
      worstStore = std::fmax(worstStore, std::fabs(back.latent.res[i] - zYellow.res[i]));
      if (!nearHalf(back.latent.c[i], zYellow.c[i])) storedInBound = false;
      if (!nearHalf(back.latent.res[i], zYellow.res[i])) storedInBound = false;
    }
    std::printf("  [measured] a real Mixbox latent through f16 tile storage: max absolute "
                "error = %.3e (bound |v|*2^-11 + 2^-25)\n",
                static_cast<double>(worstStore));
    check(lutLoaded && storedInBound && back.mass == 1.0f,
          "tile: a real Mixbox latent survives f16 storage within the derived 2^-11 relative "
          "bound, and a mass of 1.0 survives exactly");
  }

  // --- 3. The layer plumbing --------------------------------------------
  {
    const Layer p = makePigmentLayer("wash");
    check(p.kind == LayerKind::Pigment && p.pigmentTiles.has_value() &&
              !p.rgbTiles.has_value() && p.pigmentTiles->occupiedTileCount() == 0,
          "layer: makePigmentLayer() engages pigmentTiles, leaves rgbTiles absent, and "
          "allocates no tiles -- PRD C2's 'only where content exists'");
    const Layer r = makeRgbLayer("plate");
    check(r.kind == LayerKind::RGB && r.rgbTiles.has_value() && !r.pigmentTiles.has_value(),
          "layer: and an RGB layer is still the exact mirror of that, so at most one of the "
          "two stores is ever engaged");
    check(p.ops.size() == 0,
          "layer: a new layer's per-layer op stack is empty -- which the compositor treats "
          "as 'skip entirely', not as 'apply the identity'");
  }

  // --- 4. PLAN.md's Phase 5 verify sentence, as a first-class assertion --
  //
  // "Blue on a Pigment layer over yellow gives green under `Mix` and
  // translucent blue under `Normal`."
  //
  // Mass 0.5 on the blue layer is the whole experiment and is not a tuning
  // choice: DESIGN-imaging.md 3's worked example is "blue-at-50%-over-yellow",
  // and at mass 1.0 the correct answer under `Mix` really is blue, because
  // opaque paint covers. The two blends then differ in exactly the way PRD 2
  // says Photoshop gets wrong -- and under `Normal` the composite here is
  // *precisely* the naive RGB lerp (0.5*blue + 1.0*yellow*(1-0.5)), which is
  // the muddy answer the whole pigment model exists to avoid.
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("yellow");
    check(addLayer(doc, 1, makePigmentLayer("blue")).ok, "verify: a two-Pigment-layer stack");
    writePigment(doc, 0, 1, 1, zYellow, 1.0f);
    writePigment(doc, 1, 1, 1, zBlue, 0.5f);

    // Read the f16 values back and build every reference from *those*, so the
    // comparisons below are about the compositor and not about quantisation.
    const PigmentTexel low = readPigment(doc, 0, 1, 1);
    const PigmentTexel up = readPigment(doc, 1, 1, 1);
    const std::array<float, 3> rgbYellow = latentToRgb(low.latent);
    const std::array<float, 3> rgbBlue = latentToRgb(up.latent);

    const LayerOpResult toMix = setLayerBlend(doc, 1, BlendMode::Mix);
    check(toMix.ok && doc.layers[1].blend == "mix",
          "verify: PRD L5 lets `Mix` be set here, because both layers are Pigment layers");

    std::vector<std::string> mixWarnings;
    const DecodedImage mixFlat = flattenDocumentToLinear(doc, &mixWarnings);
    const std::array<float, 4> mixed = pixelOf(mixFlat, 1, 1);
    check(mixWarnings.empty(),
          "verify: and compositing it produces NO warning -- `Mix` is implemented here, not "
          "approximated as `over` the way step 2 had to");

    doc.layers[1].blend = kDefaultBlendName;
    const std::array<float, 4> normal = pixelOf(flattenDocumentToLinear(doc), 1, 1);

    const std::array<float, 3> naive = {0.5f * (rgbBlue[0] + rgbYellow[0]),
                                        0.5f * (rgbBlue[1] + rgbYellow[1]),
                                        0.5f * (rgbBlue[2] + rgbYellow[2])};
    std::printf("  [measured] blue(mass 0.5) over yellow(mass 1.0):\n");
    std::printf("  [measured]   Mix    -> (%.3f, %.3f, %.3f) alpha %.3f\n",
                static_cast<double>(mixed[0]), static_cast<double>(mixed[1]),
                static_cast<double>(mixed[2]), static_cast<double>(mixed[3]));
    std::printf("  [measured]   Normal -> (%.3f, %.3f, %.3f) alpha %.3f\n",
                static_cast<double>(normal[0]), static_cast<double>(normal[1]),
                static_cast<double>(normal[2]), static_cast<double>(normal[3]));
    std::printf("  [measured]   the naive RGB lerp Normal must match: (%.3f, %.3f, %.3f)\n",
                static_cast<double>(naive[0]), static_cast<double>(naive[1]),
                static_cast<double>(naive[2]));

    check(lutLoaded && mixed[1] > mixed[0] && mixed[1] > mixed[2],
          "VERIFY (PLAN.md Phase 5): blue on a Pigment layer over yellow gives GREEN under "
          "`Mix` -- green is the largest channel of the composited document");
    check(lutLoaded && near(normal[0], naive[0], kUnpremultiplyTol) &&
              near(normal[1], naive[1], kUnpremultiplyTol) &&
              near(normal[2], naive[2], kUnpremultiplyTol),
          "VERIFY (PLAN.md Phase 5): and translucent blue under `Normal` -- which is exactly "
          "the naive RGB lerp, the muddy answer PRD 2 says Photoshop gives");
    check(lutLoaded && mixed[0] < normal[0] * 0.5f,
          "verify: the two are not the same picture -- `Mix` keeps under half the red the "
          "translucent-blue composite has, which is the desaturation the model exists to "
          "avoid");
    check(near(mixed[3], 1.0f, 0.0f) && near(normal[3], 1.0f, 0.0f),
          "verify: both composite to alpha EXACTLY 1.0 -- mass unions as `over` does, so a "
          "blend mode changes colour and never coverage");

    // The reference for the mixed value, computed here from the stored
    // latents through the same three functions the walk calls. This is the
    // assertion that would catch a wrong mixing weight; the green test above
    // would not.
    const Latent expectMix = mixLatents(low.latent, up.latent, up.mass);
    const std::array<float, 3> expectRgb = latentToRgb(expectMix);
    check(lutLoaded && near(mixed[0], expectRgb[0], kUnpremultiplyTol) &&
              near(mixed[1], expectRgb[1], kUnpremultiplyTol) &&
              near(mixed[2], expectRgb[2], kUnpremultiplyTol),
          "verify: and the mixed colour is exactly latentToRgb(mixLatents(low, up, up.mass)) "
          "-- the mixing weight is the upper layer's MASS");
  }

  // --- 5. Opacity is transparency on a Pigment layer, and never mass -----
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("yellow");
    addLayer(doc, 1, makePigmentLayer("blue"));
    writePigment(doc, 0, 1, 1, zYellow, 1.0f);
    writePigment(doc, 1, 1, 1, zBlue, 0.5f);
    setLayerBlend(doc, 1, BlendMode::Mix);

    const std::vector<uint16_t> upperBefore = tileBytes(doc, 1, TileCoord{0, 0});
    const std::vector<uint16_t> lowerBefore = tileBytes(doc, 0, TileCoord{0, 0});

    const DecodedImage full = flattenDocumentToLinear(doc);
    doc.layers[1].opacity = 0.5f;
    const DecodedImage half = flattenDocumentToLinear(doc);

    check(!upperBefore.empty() && tileBytes(doc, 1, TileCoord{0, 0}) == upperBefore &&
              tileBytes(doc, 0, TileCoord{0, 0}) == lowerBefore,
          "opacity: compositing at any opacity leaves both pigment tiles' raw half words "
          "BIT-IDENTICAL -- opacity never becomes stored mass (PRD F10 reserves mass for the "
          "eraser)");

    // Corner 1: opacity 0 is byte-identically the document without the layer.
    doc.layers[1].opacity = 0.0f;
    const DecodedImage faded = flattenDocumentToLinear(doc);
    Document withoutUpper = Document::createBlank(4, 4, WorkingSpace{});
    withoutUpper.layers[0] = makePigmentLayer("yellow");
    writePigment(withoutUpper, 0, 1, 1, zYellow, 1.0f);
    const DecodedImage alone = flattenDocumentToLinear(withoutUpper);
    check(faded.pixels.size() == alone.pixels.size() &&
              std::memcmp(faded.pixels.data(), alone.pixels.data(),
                          faded.pixels.size() * sizeof(float)) == 0,
          "opacity: a `Mix` layer at opacity 0 composites BYTE-IDENTICALLY to the document "
          "with that layer deleted -- transparency means absent, even for the blend whose "
          "whole job is to combine");

    // Corner 2: hiding the LOWER layer must not blank the pair.
    doc.layers[1].opacity = 1.0f;
    doc.layers[0].visible = false;
    const DecodedImage lowerHidden = flattenDocumentToLinear(doc);
    Document upperOnly = Document::createBlank(4, 4, WorkingSpace{});
    upperOnly.layers[0] = makePigmentLayer("blue");
    writePigment(upperOnly, 0, 1, 1, zBlue, 0.5f);
    const DecodedImage upperAlone = flattenDocumentToLinear(upperOnly);
    check(lowerHidden.pixels.size() == upperAlone.pixels.size() &&
              std::memcmp(lowerHidden.pixels.data(), upperAlone.pixels.data(),
                          lowerHidden.pixels.size() * sizeof(float)) == 0,
          "opacity: hiding the layer a `Mix` mixes with leaves the mixing layer visible and "
          "unmixed, rather than blanking the pair -- the third corner of the coverage form");
    doc.layers[0].visible = true;

    // The distinction that matters: half the opacity is NOT half the mass.
    doc.layers[1].opacity = 0.5f;
    const std::array<float, 4> atHalfOpacity = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    Document halfMass = Document::createBlank(4, 4, WorkingSpace{});
    halfMass.layers[0] = makePigmentLayer("yellow");
    addLayer(halfMass, 1, makePigmentLayer("blue"));
    writePigment(halfMass, 0, 1, 1, zYellow, 1.0f);
    writePigment(halfMass, 1, 1, 1, zBlue, 0.25f);
    setLayerBlend(halfMass, 1, BlendMode::Mix);
    const std::array<float, 4> atHalfMass = pixelOf(flattenDocumentToLinear(halfMass), 1, 1);
    std::printf("  [measured]   Mix at opacity 0.5 -> (%.3f, %.3f, %.3f); the same layer at "
                "HALF MASS -> (%.3f, %.3f, %.3f)\n",
                static_cast<double>(atHalfOpacity[0]), static_cast<double>(atHalfOpacity[1]),
                static_cast<double>(atHalfOpacity[2]), static_cast<double>(atHalfMass[0]),
                static_cast<double>(atHalfMass[1]), static_cast<double>(atHalfMass[2]));
    check(lutLoaded && !near(atHalfOpacity[0], atHalfMass[0], 1.0e-3f),
          "opacity: and halving the OPACITY gives a measurably different colour from halving "
          "the MASS -- the two are different operations, which is what 'opacity must not "
          "become pigment mass' means numerically");

    // The argument for the coverage form used on a mixed pair: for `over` it
    // is provably the same thing as scaling the source's coverage, so it is
    // not an invention for `Mix`. Dyadic fixtures, so both routes are exact.
    float worstIdentity = 0.0f;
    for (float o : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const std::array<float, 4> src{0.5f, 0.25f, 0.125f, 0.5f};
      const std::array<float, 4> dst{0.25f, 0.75f, 0.5f, 1.0f};
      const std::array<float, 4> scaled{src[0] * o, src[1] * o, src[2] * o, src[3] * o};
      const std::array<float, 4> byScaling = compositeOver(scaled, dst);
      const std::array<float, 4> plain = compositeOver(src, dst);
      for (int i = 0; i < 4; ++i)
        worstIdentity = std::fmax(worstIdentity,
                                  std::fabs(byScaling[i] - ((1.0f - o) * dst[i] + o * plain[i])));
    }
    check(worstIdentity == 0.0f,
          "opacity: `lerp(backdrop, blend(src,backdrop), o)` and `blend(o*src, backdrop)` are "
          "the SAME value for `over` -- so fading a mixed pair's whole effect is what opacity "
          "already does, not a new rule invented for `Mix`");
  }

  // --- 6. The op stack applies AFTER the projection ----------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("yellow");
    writePigment(doc, 0, 1, 1, zYellow, 1.0f);
    const std::array<float, 3> projected = latentToRgb(readPigment(doc, 0, 1, 1).latent);
    const std::vector<uint16_t> before = tileBytes(doc, 0, TileCoord{0, 0});

    doc.layers[0].ops.add(exposureOp(1.0f));  // +1 stop: a pure doubling in linear
    const std::array<float, 4> graded = pixelOf(flattenDocumentToLinear(doc), 1, 1);

    check(tileBytes(doc, 0, TileCoord{0, 0}) == before && !before.empty(),
          "ops: a grade on a Pigment layer leaves the stored latents BIT-IDENTICAL -- this is "
          "the whole of 'grading never bakes the latents', asserted by memcmp of the tile's "
          "own half words");
    check(lutLoaded && near(graded[0], projected[0] * 2.0f, kUnpremultiplyTol) &&
              near(graded[1], projected[1] * 2.0f, kUnpremultiplyTol) &&
              near(graded[2], projected[2] * 2.0f, kUnpremultiplyTol),
          "ops: and the composited colour is the grade of the PROJECTION -- +1 stop doubles "
          "latentToRgb(latent), which is only true if the op ran after the projection");
    check(near(graded[3], 1.0f, 0.0f),
          "ops: a point op never touches alpha, so the mass-derived coverage is unchanged");

    // The negative control that makes the ordering claim mean something: the
    // other order is a genuinely different picture, so 'after' is a choice
    // this test can see, not a distinction without a difference.
    Latent doubledLatent = readPigment(doc, 0, 1, 1).latent;
    for (int i = 0; i < 3; ++i) doubledLatent.c[i] *= 2.0f;
    const std::array<float, 3> gradedFirst = latentToRgb(doubledLatent);
    check(lutLoaded && !near(gradedFirst[0], projected[0] * 2.0f, 1.0e-3f),
          "ops: applying the same doubling to the latents FIRST gives a different colour -- "
          "the polynomial is cubic, so a grade in latent space is a different pigment rather "
          "than a brighter one");

    // A disabled entry occupies a slot and contributes nothing, and the
    // composite is bit-identical to the ungraded one -- which also proves the
    // 'empty stack is skipped, not applied as identity' rule, since an
    // un-premultiply/re-premultiply round trip would not be bit-exact.
    Document plain = Document::createBlank(4, 4, WorkingSpace{});
    plain.layers[0] = makePigmentLayer("yellow");
    writePigment(plain, 0, 1, 1, zYellow, 1.0f);
    const DecodedImage ungraded = flattenDocumentToLinear(plain);
    plain.layers[0].ops.add(exposureOp(3.0f));
    plain.layers[0].ops.setEnabled(0, false);
    const DecodedImage disabled = flattenDocumentToLinear(plain);
    check(std::memcmp(ungraded.pixels.data(), disabled.pixels.data(),
                      ungraded.pixels.size() * sizeof(float)) == 0,
          "ops: a DISABLED op leaves the composite byte-identical -- the walk skips an empty "
          "op list outright rather than running it as an identity, which would not be "
          "bit-exact");

    // The same member on an RGB layer, because it is one member on Layer and
    // one code path, not a Pigment feature.
    Document rgb = Document::createBlank(4, 4, WorkingSpace{});
    {
      Tile& tile = rgb.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      tile.writePixel(PixelCoord{1, 1}, {0.25f, 0.125f, 0.0625f, 1.0f});
    }
    const DecodedImage rgbUngraded = flattenDocumentToLinear(rgb);
    rgb.layers[0].ops.add(exposureOp(1.0f));
    const std::array<float, 4> rgbGraded = pixelOf(flattenDocumentToLinear(rgb), 1, 1);
    check(near(pixelOf(rgbUngraded, 1, 1)[0], 0.25f, 0.0f) &&
              near(rgbGraded[0], 0.5f, 0.0f) && near(rgbGraded[1], 0.25f, 0.0f),
          "ops: the per-layer stack applies to an RGB layer identically -- it is one member "
          "on Layer and one branch in the walk, as DESIGN-imaging.md 3's Layer diagram has "
          "it");
  }

  // --- 7. Every other `Mix` combination, and what each does --------------
  {
    // A Pigment layer over an RGB layer. setLayerBlend() refuses it (PRD L5,
    // already asserted in runBlendTest), so it can only arrive from a file --
    // which is exactly why the compositor must answer for one.
    Document overRgb = Document::createBlank(4, 4, WorkingSpace{});
    addLayer(overRgb, 1, makePigmentLayer("wash"));
    writePigment(overRgb, 1, 1, 1, zBlue, 1.0f);
    overRgb.layers[1].blend = "mix";  // as a file would carry it
    std::vector<std::string> w;
    flattenDocumentToLinear(overRgb, &w);
    check(w.size() == 1 && contains(w[0], "L5") && contains(w[0], "RGB layer") &&
              contains(w[0], "composited as"),
          "mix: over an RGB layer it is composited as `over` and warned by name, naming PRD "
          "L5 and the kind of the layer beneath -- never silently, never refused");
    check(overRgb.layers[1].blend == "mix",
          "mix: and the value is untouched, so PRD I10 still writes it back verbatim");

    // The bottom layer.
    Document bottom = Document::createBlank(4, 4, WorkingSpace{});
    bottom.layers[0] = makePigmentLayer("wash");
    writePigment(bottom, 0, 1, 1, zBlue, 1.0f);
    bottom.layers[0].blend = "mix";
    std::vector<std::string> bw;
    flattenDocumentToLinear(bottom, &bw);
    check(bw.size() == 1 && contains(bw[0], "bottom layer"),
          "mix: on the bottom layer it names the reason -- there is nothing beneath it to "
          "mix with");

    // A chain of three, which is the limit this step states rather than hides.
    Document chain = Document::createBlank(4, 4, WorkingSpace{});
    chain.layers[0] = makePigmentLayer("a");
    addLayer(chain, 1, makePigmentLayer("b"));
    addLayer(chain, 2, makePigmentLayer("c"));
    writePigment(chain, 0, 1, 1, zYellow, 1.0f);
    writePigment(chain, 1, 1, 1, zBlue, 0.5f);
    writePigment(chain, 2, 1, 1, zBlue, 0.5f);
    check(setLayerBlend(chain, 1, BlendMode::Mix).ok && setLayerBlend(chain, 2, BlendMode::Mix).ok,
          "mix: PRD L5 permits `Mix` on both upper layers of a three-Pigment stack");
    const MixPairing pairing = mixPairing(chain);
    check(pairing.mixedWithBelow[1] && pairing.consumedByAbove[0] && !pairing.mixedWithBelow[2] &&
              !pairing.consumedByAbove[1],
          "mix: pairing is greedy from the bottom, so (0,1) forms and layer 2 is left "
          "unpaired -- deterministic, and the same answer the composite and the probe both "
          "use");
    std::vector<std::string> cw;
    flattenDocumentToLinear(chain, &cw);
    check(cw.size() == 1 && contains(cw[0], "chained"),
          "mix: a chained `Mix` is composited as `over` and says so by name -- a stated limit "
          "with a warning, not an approximation that keeps quiet");
    check(blendIsImplementedForLayer(chain, 1) && !blendIsImplementedForLayer(chain, 2) &&
              blendIsImplemented("mix"),
          "mix: `blendIsImplementedForLayer()` answers per position while "
          "`blendIsImplemented()` answers per name -- the two questions differ for exactly "
          "this mode");
  }

  // --- 8. The eyedropper and the flattener still agree -------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("yellow");
    addLayer(doc, 1, makePigmentLayer("blue"));
    writePigment(doc, 0, 1, 1, zYellow, 1.0f);
    writePigment(doc, 1, 1, 1, zBlue, 0.5f);
    setLayerBlend(doc, 1, BlendMode::Mix);

    ProbeParams all;
    all.source = ProbeSource::AllLayers;
    const ProbeSample sample = probePixel(doc, PixelCoord{1, 1}, all);
    const std::array<float, 4> flat = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(lutLoaded && near(sample.linear[0], flat[0], kUnpremultiplyTol) &&
              near(sample.linear[1], flat[1], kUnpremultiplyTol) &&
              near(sample.linear[2], flat[2], kUnpremultiplyTol) &&
              near(sample.linear[3], flat[3], kUnpremultiplyTol),
          "probe: ProbeSource::AllLayers reads a mixed Pigment pair through the same functions the "
          "flattener uses -- an eyedropper and an export that disagreed would be a bug "
          "nobody could explain");

    ProbeParams own;
    own.source = ProbeSource::CurrentLayer;
    own.activeLayerIndex = 1;
    const ProbeSample layerOwn = probePixel(doc, PixelCoord{1, 1}, own);
    const std::array<float, 3> blueProjected = latentToRgb(readPigment(doc, 1, 1, 1).latent);
    check(lutLoaded && near(layerOwn.linear[0], blueProjected[0], kUnpremultiplyTol) &&
              near(layerOwn.linear[3], 0.5f, 0.0f),
          "probe: single-layer mode on a Pigment layer reads its own projected colour at its "
          "own mass -- 'what is on this layer', unmixed and ungraded");
  }

  // --- 9. The regression boundary: pigment changes nothing for RGB -------
  {
    // A hidden Pigment layer must be byte-identically absent from a document
    // that is otherwise pure RGB -- the same "hidden means as if deleted"
    // check step 1 made, re-made across the new code path.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    {
      Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      tile.writePixel(PixelCoord{2, 3}, {0.3f, 0.6f, 0.9f, 1.0f});
      tile.writePixel(PixelCoord{4, 5}, {0.125f, 0.25f, 0.375f, 0.5f});
    }
    const DecodedImage rgbOnly = flattenDocumentToLinear(doc);
    addLayer(doc, 1, makePigmentLayer("hidden wash"));
    writePigment(doc, 1, 2, 3, zBlue, 1.0f);
    doc.layers[1].visible = false;
    const DecodedImage withHidden = flattenDocumentToLinear(doc);
    check(rgbOnly.pixels.size() == withHidden.pixels.size() &&
              std::memcmp(rgbOnly.pixels.data(), withHidden.pixels.data(),
                          rgbOnly.pixels.size() * sizeof(float)) == 0,
          "regression: a hidden Pigment layer contributes byte-identically nothing to an "
          "otherwise RGB document");
    doc.layers[1].visible = true;
    const DecodedImage visible = flattenDocumentToLinear(doc);
    check(std::memcmp(rgbOnly.pixels.data(), visible.pixels.data(),
                      rgbOnly.pixels.size() * sizeof(float)) != 0,
          "regression: and the negative control -- unhidden, it genuinely does change the "
          "picture, so the check above is not vacuous");
  }

  // --- 10. The `.npaint` round trip, Pigment and RGB in one document -----
  {
    const char* kPath = "selftest_pigment.npaint";
    std::remove(kPath);

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("wash");
    doc.layers[0].opacity = 0.75f;
    doc.layers[0].locked = true;
    addRgbLayer(doc, "plate");
    doc.layers[1].blend = "multiply";
    // A latent with a negative residual and a partial mass, in two different
    // tiles, so the data window is more than one tile and the residual's sign
    // has to survive. The residual's red and blue are deliberately different
    // and of opposite sign: `res.R`/`res.B` are the pair a channel-order
    // mistake would swap, and a swap is the failure mode that would otherwise
    // show up as wrong colour rather than as a crash.
    Latent quirky;
    quirky.c = {0.75f, 0.125f, 0.03125f};
    quirky.res = {-0.25f, 0.0625f, 0.5f};
    writePigment(doc, 0, 1, 1, zBlue, 0.5f);
    writePigment(doc, 0, 200, 130, quirky, 0.875f);
    {
      Tile& tile = doc.layers[1].rgbTiles->getOrCreate(TileCoord{1, 0});
      tile.writePixel(PixelCoord{3, 4}, {0.5f, 0.25f, 0.125f, 1.0f});
    }

    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok, "npaint: a document with a Pigment layer and an RGB layer saves");
    if (saved.ok) {
      check(saved.partsWritten == 3 && saved.warnings.empty(),
            "npaint: three parts -- the composite plus one per layer -- and nothing about "
            "the save is approximate");
      const NpaintLoadResult back = loadNpaint(kPath);
      check(back.ok && back.document.layers.size() == 2 &&
                back.document.layers[0].kind == LayerKind::Pigment &&
                back.document.layers[1].kind == LayerKind::RGB,
            "npaint: it loads back as a Pigment layer under an RGB layer -- the reader "
            "recognises `np:kind = \"Pigment\"` with the eleven named channels");
      if (back.ok && back.document.layers.size() == 2 &&
          back.document.layers[0].pigmentTiles.has_value()) {
        check(back.warnings.empty(),
              "npaint: and reads with no warnings -- nothing in the file was carried because "
              "it could not be understood");
        const PigmentTileStore& got = *back.document.layers[0].pigmentTiles;
        const PigmentTileStore& want = *doc.layers[0].pigmentTiles;
        check(got.occupiedTileCount() == want.occupiedTileCount() &&
                  got.occupiedTileCount() == 2,
              "npaint: both occupied pigment tiles come back and no empty one is invented -- "
              "the all-zero tiles inside the data window are dropped, as for RGB");
        bool bitIdentical = got.occupiedTileCount() == want.occupiedTileCount();
        for (const auto& [coord, tile] : want) {
          const PigmentTile* g = got.find(coord);
          if (g == nullptr ||
              std::memcmp(g->data(), tile.data(),
                          PigmentTile::kTexelCount * sizeof(uint16_t)) != 0)
            bitIdentical = false;
        }
        check(bitIdentical,
              "npaint: every pigment tile is BIT-IDENTICAL after the round trip -- HALF in, "
              "HALF out, no float stage, zero tolerance, exactly the claim the RGB parts "
              "already make");
        const PigmentTexel gotQuirky = readPigment(back.document, 0, 200, 130);
        check(gotQuirky.latent.res[0] < 0.0f && gotQuirky.latent.res[2] > 0.0f &&
                  gotQuirky.latent.res == readPigment(doc, 0, 200, 130).latent.res,
              "npaint: res.R comes back as res.R and res.B as res.B, sign included -- the "
              "reader matches channels by NAME, so nothing here depends on OpenImageIO "
              "handing the eleven back in the order they were written");
        check(back.document.layers[0].opacity == 0.75f && back.document.layers[0].locked &&
                  back.document.layers[0].name == "wash" &&
                  back.document.layers[1].blend == "multiply",
              "npaint: and a Pigment layer's np:* metadata round-trips exactly as an RGB "
              "layer's does");
        const DecodedImage a = flattenDocumentToLinear(doc);
        const DecodedImage b = flattenDocumentToLinear(back.document);
        check(a.pixels.size() == b.pixels.size() &&
                  std::memcmp(a.pixels.data(), b.pixels.data(),
                              a.pixels.size() * sizeof(float)) == 0,
              "npaint: the reloaded document composites BYTE-IDENTICALLY to the saved one");
      }

      // The basis refusal docs/document-format.md 3.3 asks for, which could
      // not exist before this step because no latent could be written.
      NpaintCarry foreign;
      // The basis this build is NOT -- which of core/Document.hpp's two names
      // that is depends on NP_USE_MIXBOX, and spelling "km2-v1" outright would
      // turn this refusal test into a same-basis save that succeeds under
      // NP_USE_MIXBOX=OFF.
#if defined(NP_USE_MIXBOX)
      foreign.basis = kPigmentBasisKm2;
#else
      foreign.basis = kPigmentBasisMixbox;
#endif
      const NpaintSaveResult mismatched = saveNpaint(doc, kPath, {}, &foreign);
      check(!mismatched.ok && contains(mismatched.error, foreign.basis.c_str()) &&
                contains(mismatched.error, "basis"),
            "npaint: a document with Pigment layers whose carried np:basis is not this "
            "build's is REFUSED by name -- a latent is meaningless in another basis, and "
            "silently so");
      Document rgbOnly = Document::createBlank(64, 64, WorkingSpace{});
      const NpaintSaveResult rgbForeign = saveNpaint(rgbOnly, kPath, {}, &foreign);
      check(rgbForeign.ok,
            "npaint: but an RGB-only document still carries a foreign basis through untouched "
            "-- nothing in such a file depends on it (PRD I10)");

      // **A Pigment layer's op stack now round-trips**, and this assertion is
      // the replacement for the one this section carried until PLAN.md Phase 5
      // step 5: "a layer's op stack is not written -- there is no working blob
      // carrier -- and the save says so by name rather than dropping it
      // quietly (PRD I11)". That claim was true and is now false: step 5 built
      // io/OpSerial and the hex `string` carrier docs/document-format.md names,
      // because an Adjustment layer's whole content is its stack. The claim
      // *this* section is responsible for -- that a Pigment layer's grade
      // survives a save -- is asserted here rather than deleted, and the
      // carrier itself is covered at length by runAdjustmentLayerTest().
      Document graded = Document::createBlank(64, 64, WorkingSpace{});
      graded.layers[0] = makePigmentLayer("wash");
      writePigment(graded, 0, 1, 1, zBlue, 1.0f);
      graded.layers[0].ops.add(exposureOp(1.0f));
      const NpaintSaveResult gradedSave = saveNpaint(graded, kPath);
      const NpaintLoadResult gradedBack = loadNpaint(kPath);
      check(gradedSave.ok && gradedSave.warnings.empty() && gradedBack.ok &&
                gradedBack.document.layers.size() == 1 &&
                gradedBack.document.layers[0].ops.size() == 1 &&
                gradedBack.document.layers[0].ops.at(0).pointKind == PointOpKind::Exposure &&
                gradedBack.document.layers[0].ops.at(0).exposure.stops == 1.0f,
            "npaint: a Pigment layer's op stack is WRITTEN and comes back exactly -- the "
            "`np:ops` deferral this section used to assert closed at Phase 5 step 5");
    }
    std::remove(kPath);
    check(std::fopen(kPath, "rb") == nullptr,
          "npaint: every scratch file this section wrote is removed");
  }

  std::printf("[selftest] pigment layers %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
