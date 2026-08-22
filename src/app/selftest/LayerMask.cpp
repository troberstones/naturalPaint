#include "app/selftest/Support.hpp"

namespace np {

bool runLayerMaskTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- Tolerances, derived for THIS channel rather than borrowed ----------
  //
  //  * **kMaskAbs -- the mask channel's own f16 bound, and it is tighter than
  //    the one step 3 derived for a latent.** A mask sample is a coverage, so
  //    it lives in [0,1] and only the *absolute* error matters. binary16's
  //    spacing in [0.5, 1) is 2^-11, so round-to-nearest costs at most half of
  //    that, **2^-12 = 2.441e-04**, and every lower binade is finer. That is
  //    the bound for the whole channel, with no relative term and no subnormal
  //    floor -- borrowing step 3's `|v|*2^-11 + 2^-25` would have been valid
  //    but twice as loose, because a latent is unbounded and this is not.
  //    Measured against a 1025-point ramp below, and printed.
  //  * **kUnpremultiplyTol -- anything read back through the flattener**,
  //    whose final un-premultiply is one correctly-rounded division. Half an
  //    ulp at results in [0.25,1) is 2^-25 = 2.98e-8; bounded at 1.0e-7, 3.4x.
  //    Identical derivation to runLayerStackTest()/runBlendTest()/
  //    runPigmentLayerTest(), restated rather than cross-referenced because a
  //    reader checking this section should not have to go and find it.
  //
  // Everything else here is asserted at **exactly zero tolerance**, and that
  // is not luck: a mask multiplies a premultiplied texel by one scalar, and
  // every mask value this section uses (0, 0.25, 0.5, 0.75, 1) is a dyadic
  // rational exactly representable in binary16 and in float, so every
  // reference below is an exact float expression rather than a rounded one.
  constexpr float kMaskAbs = 2.4414063e-04f;  // 2^-12
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]};
  };
  auto sameImage = [](const DecodedImage& a, const DecodedImage& b) {
    return a.pixels.size() == b.pixels.size() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0;
  };
  auto writeMask = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float v) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].mask->getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), v);
  };
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
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
  // Every stored `pig.m` half word of one tile, so "a mask never writes mass"
  // can be a memcmp rather than an argument. Channel 3 of core::PigmentTile's
  // seven is `pig.m` (core/Pigment.hpp's channel order).
  auto massWords = [](const Document& doc, size_t layerIndex,
                      TileCoord coord) -> std::vector<uint16_t> {
    const PigmentTile* tile = doc.layers[layerIndex].pigmentTiles->find(coord);
    if (tile == nullptr) return {};
    std::vector<uint16_t> out;
    out.reserve(MaskTile::kTexelCount);
    for (size_t i = 0; i < PigmentTile::kTexelCount; i += PigmentTile::kChannels)
      out.push_back(tile->data()[i + 3]);
    return out;
  };

  // --- 1. The tile: one channel, 32 KiB, and a default of REVEAL ---------
  {
    check(MaskTile::kChannels == 1 && sizeof(MaskTile) == 32 * 1024,
          "tile: a mask tile is ONE half-float channel and exactly 32 KiB -- a quarter of an "
          "RGBA tile, a seventh of a pigment tile");
    std::printf("  [measured] mask tile %zu KiB vs. rgba16float %zu KiB (%.3fx) vs. pigment "
                "%zu KiB (%.3fx)\n",
                sizeof(MaskTile) / 1024, sizeof(Tile) / 1024,
                static_cast<double>(sizeof(MaskTile)) / static_cast<double>(sizeof(Tile)),
                sizeof(PigmentTile) / 1024,
                static_cast<double>(sizeof(MaskTile)) / static_cast<double>(sizeof(PigmentTile)));
    check(MaskTile::kRevealWord == floatToHalf(1.0f),
          "tile: kRevealWord really is binary16 1.0 -- io/NpaintFile's drop rule is a word "
          "comparison, so the literal is checked against core/Half rather than trusted");

    MaskTileStore store;
    check(store.occupiedTileCount() == 0 && store.find(TileCoord{0, 0}) == nullptr,
          "tile: a fresh mask store allocates nothing and find() does not allocate -- PRD C2, "
          "from the same TileStoreOf template core::Tile and PigmentTile use");
    check(maskCoverage(nullptr, PixelCoord{5, 7}) == 1.0f,
          "tile: a MISSING mask tile reads 1.0 -- reveal. This is the decision that stops a "
          "mask on one tile of a four-tile layer from blanking the other three");
    MaskTile& fresh = store.getOrCreate(TileCoord{2, -1});
    bool allReveal = fresh.isFullyRevealed();
    for (int32_t y = 0; y < kTileSize; y += 17)
      for (int32_t x = 0; x < kTileSize; x += 13)
        if (fresh.readCoverage(PixelCoord{x, y}) != 1.0f) allReveal = false;
    check(allReveal && store.occupiedTileCount() == 1,
          "tile: and a FRESHLY ALLOCATED one is all 1.0 too, so getOrCreate() cannot hand back "
          "a tile that hides 16384 texels -- the one tile type here whose default is not zero");

    fresh.writeCoverage(PixelCoord{3, 4}, 0.5f);
    check(fresh.readCoverage(PixelCoord{3, 4}) == 0.5f &&
              fresh.readCoverage(PixelCoord{4, 4}) == 1.0f &&
              fresh.readCoverage(PixelCoord{3, 5}) == 1.0f && !fresh.isFullyRevealed(),
          "tile: writing one texel changes exactly that texel and makes the tile no longer "
          "'fully revealed' -- the single channel is indexed per texel");

    // Exact case first: a tolerance-only assertion would pass against a store
    // that quietly rounded to 8 bits, which is the alternative core/Mask.hpp
    // rejects.
    bool exact = true;
    for (const float v : {0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      fresh.writeCoverage(PixelCoord{9, 9}, v);
      if (fresh.readCoverage(PixelCoord{9, 9}) != v) exact = false;
    }
    check(exact,
          "tile: 0, 1/8, 1/4, 1/2, 3/4 and 1 round-trip through f16 mask storage EXACTLY -- "
          "not within a tolerance");

    // The derived bound, measured over the whole range the channel can hold.
    // The ramp step is 1/1000 and deliberately **not** a power of two: at
    // i/1024 every one of the 1025 samples is exactly representable in
    // binary16 and the measured error is 0.000e+00, which would make this a
    // test of nothing. Measured once at that step while writing this, which is
    // why the denominator is stated rather than chosen.
    float worst = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
      const float v = static_cast<float>(i) / 1000.0f;
      fresh.writeCoverage(PixelCoord{1, 1}, v);
      worst = std::fmax(worst, std::fabs(fresh.readCoverage(PixelCoord{1, 1}) - v));
    }
    std::printf("  [measured] f16 mask storage over a 1001-point ramp of [0,1]: max absolute "
                "error = %.3e (derived bound 2^-12 = %.3e; uint8 would be 1/510 = %.3e)\n",
                static_cast<double>(worst), static_cast<double>(kMaskAbs),
                static_cast<double>(1.0f / 510.0f));
    check(worst <= kMaskAbs && worst > 0.0f,
          "tile: every mask sample is within the derived 2^-12 bound -- 8x better than the "
          "uint8 store PLAN.md's phase 7 line specifies for a *selection*, which is why a "
          "document-persisted mask is not that");
  }

  // --- 2. Out of range and NaN: clamped at every boundary ----------------
  {
    MaskTile tile;
    tile.writeCoverage(PixelCoord{0, 0}, 1.5f);
    tile.writeCoverage(PixelCoord{1, 0}, -0.25f);
    tile.writeCoverage(PixelCoord{2, 0}, std::numeric_limits<float>::quiet_NaN());
    tile.writeCoverage(PixelCoord{3, 0}, std::numeric_limits<float>::infinity());
    check(tile.readCoverage(PixelCoord{0, 0}) == 1.0f &&
              tile.readCoverage(PixelCoord{1, 0}) == 0.0f &&
              tile.readCoverage(PixelCoord{2, 0}) == 0.0f &&
              tile.readCoverage(PixelCoord{3, 0}) == 1.0f,
          "range: writeCoverage() clamps 1.5 to 1, -0.25 to 0, inf to 1 and NaN to 0 -- the "
          "identical `!(v > 0)` rule core::layerCoverage() uses, because a mask and an opacity "
          "are the same quantity");

    // A file can put anything in those half words, so the READ clamps too --
    // this is the path a `.npaint` from another tool takes.
    tile.data()[0] = 0x7E00;  // binary16 quiet NaN
    tile.data()[1] = 0x4000;  // binary16 2.0
    tile.data()[2] = 0xBC00;  // binary16 -1.0
    check(tile.readCoverage(PixelCoord{0, 0}) == 0.0f &&
              tile.readCoverage(PixelCoord{1, 0}) == 1.0f &&
              tile.readCoverage(PixelCoord{2, 0}) == 0.0f,
          "range: raw half words a FILE could carry -- NaN, 2.0, -1.0 -- are clamped on read, "
          "so nothing out of range reaches the compositor whatever is on disk");

    // And the reason that matters: one NaN sample must not poison a canvas.
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(doc, 0, 0, 0, {0.5f, 0.25f, 0.125f, 1.0f});
    writeRgb(doc, 0, 1, 1, {0.5f, 0.25f, 0.125f, 1.0f});
    doc.layers[0].mask.emplace();
    MaskTile& poisoned = doc.layers[0].mask->getOrCreate(TileCoord{0, 0});
    poisoned.data()[0] = 0x7E00;                       // (0,0): NaN -> hidden
    poisoned.data()[kTileSize + 1] = 0x4000;           // (1,1): 2.0 -> revealed
    const DecodedImage flat = flattenDocumentToLinear(doc);
    bool anyNonFinite = false;
    for (const float v : flat.pixels)
      if (!std::isfinite(v)) anyNonFinite = true;
    check(!anyNonFinite && pixelOf(flat, 1, 1)[3] == 1.0f && pixelOf(flat, 0, 0)[3] == 0.0f,
          "range: a document whose mask holds a NaN composites to finite values everywhere -- "
          "the NaN texel is hidden, the 2.0 one is fully revealed, and nothing propagates");
  }

  // --- 3. A mask multiplies COVERAGE, against hand-computed references ----
  {
    // One opaque texel under a 1/4 mask, on nothing. The reference is exact:
    // src becomes (0.5m, 0.25m, 0.125m, m) premultiplied, and un-premultiplying
    // by m gives the colour back unchanged with alpha m. **A mask changes
    // coverage and not colour**, which is the whole claim in one line.
    Document lone = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(lone, 0, 1, 1, {0.5f, 0.25f, 0.125f, 1.0f});
    lone.layers[0].mask.emplace();
    writeMask(lone, 0, 1, 1, 0.25f);
    const std::array<float, 4> alone = pixelOf(flattenDocumentToLinear(lone), 1, 1);
    std::printf("  [measured] opaque (0.5, 0.25, 0.125) under a 0.25 mask, on nothing: "
                "(%.6f, %.6f, %.6f, %.6f)\n",
                static_cast<double>(alone[0]), static_cast<double>(alone[1]),
                static_cast<double>(alone[2]), static_cast<double>(alone[3]));
    check(alone[0] == 0.5f && alone[1] == 0.25f && alone[2] == 0.125f && alone[3] == 0.25f,
          "coverage: a mask changes ALPHA and leaves the colour bit-identical -- exactly what "
          "opacity does, and the reason PRD C3's sentence is the one that governs a mask");

    // The same texel over an opaque white backdrop, where the mask's arithmetic
    // is visible in the colour. Every term is dyadic, so this is exact:
    //   R = 0.5*0.25 + 1*(1-0.25) = 0.125 + 0.75  = 0.875
    //   G = 0.25*0.25 + 0.75      = 0.0625 + 0.75 = 0.8125
    //   B = 0.125*0.25 + 0.75     = 0.03125 + 0.75= 0.78125
    //   A = 0.25 + 1*(1-0.25)                     = 1.0
    Document over = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(over, 0, 1, 1, {1.0f, 1.0f, 1.0f, 1.0f});
    addLayer(over, 1, makeRgbLayer("top"));
    writeRgb(over, 1, 1, 1, {0.5f, 0.25f, 0.125f, 1.0f});
    over.layers[1].mask.emplace();
    writeMask(over, 1, 1, 1, 0.25f);
    const std::array<float, 4> onWhite = pixelOf(flattenDocumentToLinear(over), 1, 1);
    std::printf("  [measured] the same texel over opaque white: (%.6f, %.6f, %.6f, %.6f) "
                "against the hand-computed (0.875000, 0.812500, 0.781250, 1.000000)\n",
                static_cast<double>(onWhite[0]), static_cast<double>(onWhite[1]),
                static_cast<double>(onWhite[2]), static_cast<double>(onWhite[3]));
    check(onWhite[0] == 0.875f && onWhite[1] == 0.8125f && onWhite[2] == 0.78125f &&
              onWhite[3] == 1.0f,
          "coverage: over a backdrop it matches the hand-computed `over(m*src, dst)` at ZERO "
          "tolerance -- three quarters of the white shows through a quarter-strength mask");

    // Mask x opacity is a plain product, and the strongest form of that claim
    // is byte-identity rather than a tolerance: all three documents reach
    // contribute() carrying the single scalar 0.25.
    Document a = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(a, 0, 1, 1, {0.5f, 0.25f, 0.125f, 1.0f});
    Document b = a, c = a, d = a;
    a.layers[0].opacity = 0.25f;  // opacity alone
    b.layers[0].mask.emplace();
    writeMask(b, 0, 1, 1, 0.25f);  // mask alone
    c.layers[0].opacity = 0.5f;
    c.layers[0].mask.emplace();
    writeMask(c, 0, 1, 1, 0.5f);  // half of each
    d.layers[0].opacity = 0.5f;
    d.layers[0].mask.emplace();
    writeMask(d, 0, 1, 1, 0.25f);  // the negative control: product 0.125
    const DecodedImage fa = flattenDocumentToLinear(a);
    check(sameImage(fa, flattenDocumentToLinear(b)) && sameImage(fa, flattenDocumentToLinear(c)),
          "coverage: opacity 0.25, mask 0.25, and opacity 0.5 x mask 0.5 all composite "
          "BYTE-IDENTICALLY -- a mask and an opacity compose as a plain product, asserted "
          "rather than assumed");
    check(!sameImage(fa, flattenDocumentToLinear(d)),
          "coverage: and the negative control -- opacity 0.5 x mask 0.25 is a different "
          "picture, so the identity above is not vacuous");

    // The identity that licenses the coverage form everywhere it is used,
    // including on a mixed pair where only the fade is available. Re-measured
    // over the mask x opacity grid rather than cross-referenced, and measured
    // twice: on dyadic operands, where both routes are exact and the residual
    // must be exactly 0 (runPigmentLayerTest()'s form, extended to a product
    // of two coverages), and on **non-dyadic** ones, where they are not and
    // the residual is bounded instead.
    //
    // The bound for that second measurement is derived rather than guessed:
    // each route is at most five correctly-rounded float operations on
    // magnitudes <= 2, so at most five half-ulps each, ten in total, i.e.
    // 10 * 2^-24 = 5.96e-07 -- bounded at 1.0e-6, 1.7x.
    constexpr float kIdentityTol = 1.0e-6f;
    float worstDyadic = 0.0f;
    float worstMessy = 0.0f;
    for (const float m : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      for (const float o : {0.0f, 0.25f, 0.5f, 1.0f}) {
        const float e = m * o;
        auto residual = [&](const std::array<float, 4>& src, const std::array<float, 4>& dst) {
          const std::array<float, 4> scaled{src[0] * e, src[1] * e, src[2] * e, src[3] * e};
          const std::array<float, 4> byScaling = compositeOver(scaled, dst);
          const std::array<float, 4> plain = compositeOver(src, dst);
          float w = 0.0f;
          for (int i = 0; i < 4; ++i)
            w = std::fmax(w, std::fabs(byScaling[i] - ((1.0f - e) * dst[i] + e * plain[i])));
          return w;
        };
        worstDyadic = std::fmax(worstDyadic, residual({0.5f, 0.25f, 0.125f, 0.5f},
                                                      {0.25f, 0.75f, 0.5f, 1.0f}));
        worstMessy = std::fmax(worstMessy, residual({0.30f, 0.60f, 0.90f, 0.75f},
                                                    {0.10f, 0.20f, 0.40f, 0.50f}));
      }
    }
    std::printf("  [measured] `lerp(dst, over(src,dst), m*o)` vs `over(m*o*src, dst)` over the "
                "mask x opacity grid: dyadic residual = %.3e, non-dyadic = %.3e (bound %.3e)\n",
                static_cast<double>(worstDyadic), static_cast<double>(worstMessy),
                static_cast<double>(kIdentityTol));
    check(worstDyadic == 0.0f && worstMessy <= kIdentityTol,
          "coverage: `lerp(backdrop, over(src,backdrop), m*o)` and `over(m*o*src, backdrop)` "
          "are the SAME value across the whole mask x opacity grid -- exactly 0 on dyadic "
          "operands, within the derived 10-half-ulp bound otherwise");
  }

  // --- 4. Absent, all-1.0 and all-0.0 are three different things ---------
  {
    Document base = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(base, 0, 2, 3, {0.3f, 0.6f, 0.9f, 1.0f});
    writeRgb(base, 0, 4, 5, {0.125f, 0.25f, 0.375f, 0.5f});
    const DecodedImage absent = flattenDocumentToLinear(base);

    // (a) Engaged with zero tiles -- what core::addLayerMask() creates.
    Document revealEmpty = base;
    check(addLayerMask(revealEmpty, 0).ok && revealEmpty.layers[0].mask.has_value() &&
              revealEmpty.layers[0].mask->occupiedTileCount() == 0,
          "states: addLayerMask() engages the store and allocates NOTHING -- 'reveal all' is "
          "free, which is PRD C2 applied to a mask");
    check(sameImage(absent, flattenDocumentToLinear(revealEmpty)),
          "states: an empty (reveal-all) mask composites BYTE-IDENTICALLY to no mask at all -- "
          "the walk's mask multiply is by a literal 1.0f, exact for every finite float");

    // (b) Engaged with a tile explicitly written to 1.0 everywhere.
    Document revealWritten = revealEmpty;
    MaskTile& t = revealWritten.layers[0].mask->getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x) t.writeCoverage(PixelCoord{x, y}, 1.0f);
    check(t.isFullyRevealed() && sameImage(absent, flattenDocumentToLinear(revealWritten)),
          "states: an ALLOCATED all-1.0 tile composites byte-identically too -- so an absent "
          "tile and a written-to-1.0 tile are the same picture and differ only in 32 KiB");

    // (c) All 0.0 -- real content, and equal to deleting the layer.
    Document hidden = base;
    hidden.layers[0].mask.emplace();
    MaskTile& z = hidden.layers[0].mask->getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x) z.writeCoverage(PixelCoord{x, y}, 0.0f);
    Document deleted = base;
    removeLayer(deleted, 0);
    check(sameImage(flattenDocumentToLinear(hidden), flattenDocumentToLinear(deleted)),
          "states: an all-0.0 mask composites BYTE-IDENTICALLY to the layer being deleted -- "
          "the walk skips a zero-coverage texel rather than multiplying by zero");
    check(!sameImage(absent, flattenDocumentToLinear(hidden)),
          "states: and all-0.0 is emphatically not the same as absent -- three states, three "
          "meanings, one of which costs 32 KiB per tile because it is real content");

    // The one thing that distinguishes absent from reveal-all where a user can
    // see it. docs/ui.md 3.2's own vocabulary.
    check(!contains(layerRowSubLine(base.layers[0]), "MASK") &&
              contains(layerRowSubLine(revealEmpty.layers[0]), "MASK") &&
              contains(layerRowSubLine(revealEmpty.layers[0]), "RGB \xC2\xB7 NORMAL"),
          "states: the layers panel says `MASK` for the reveal-all one and not for the "
          "maskless one -- otherwise nothing a user can see would tell them apart");

    // core/LayerOps' whole mask lifecycle, including what it refuses.
    Document ops = base;
    const LayerOpResult added = addLayerMask(ops, 0);
    check(added.ok && added.editLabel == "add mask to layer 0" && added.index == 0,
          "lifecycle: addLayerMask() reports the edit label app::recordLayerEdit() journals");
    check(!addLayerMask(ops, 0).ok && contains(addLayerMask(ops, 0).error, "already has a mask"),
          "lifecycle: adding a second mask is REFUSED by name rather than replacing the first "
          "-- one slot per layer, and a silent replacement would discard every texel in it");
    const LayerOpResult removed = removeLayerMask(ops, 0);
    check(removed.ok && !ops.layers[0].mask.has_value() &&
              removed.editLabel == "remove mask from layer 0",
          "lifecycle: removeLayerMask() disengages the store and reports its own label");
    check(!removeLayerMask(ops, 0).ok && contains(removeLayerMask(ops, 0).error, "has no mask"),
          "lifecycle: removing a mask that is not there is refused -- a no-op reporting "
          "success would journal an edit that did not happen");
    ops.layers[0].locked = true;
    check(!addLayerMask(ops, 0).ok && contains(addLayerMask(ops, 0).error, "is locked") &&
              !removeLayerMask(ops, 0).ok,
          "lifecycle: both are refused on a LOCKED layer -- which parts of a layer show is "
          "part of how it looks, the same reason setLayerBlend() is refused");
    check(!addLayerMask(ops, 99).ok && contains(addLayerMask(ops, 99).error, "no layer at index"),
          "lifecycle: and an out-of-range index is refused through core/LayerOps' one bounds "
          "check, not a second copy of it");
  }

  // --- 5. The C3 trap: a mask on a Pigment layer is NOT pigment mass ------
  MixboxLut lut;
  const bool lutLoaded = lut.load(NP_MIXBOX_LUT);
  check(lutLoaded,
        "pigment: the real Mixbox LUT loads -- every colour claim below is against measured "
        "pigment data rather than a stand-in");
  const Pigment& yellowPigment = defaultPalette()[0];
  const Pigment& bluePigment = defaultPalette()[7];
  const Latent zYellow =
      lut.rgbToLatent(yellowPigment.rgb[0], yellowPigment.rgb[1], yellowPigment.rgb[2]);
  const Latent zBlue = lut.rgbToLatent(bluePigment.rgb[0], bluePigment.rgb[1], bluePigment.rgb[2]);
  {
    // A lone Pigment layer first, where the two ARE numerically the same and
    // the distinction has to be made structurally: the stored mass is never
    // written, whatever the mask says.
    Document lone = Document::createBlank(4, 4, WorkingSpace{});
    lone.layers[0] = makePigmentLayer("wash");
    writePigment(lone, 0, 1, 1, zBlue, 0.75f);
    const std::vector<uint16_t> massBefore = massWords(lone, 0, TileCoord{0, 0});
    lone.layers[0].mask.emplace();
    for (const float m : {0.0f, 0.25f, 0.5f, 1.0f}) {
      writeMask(lone, 0, 1, 1, m);
      flattenDocumentToLinear(lone);
    }
    check(!massBefore.empty() && massWords(lone, 0, TileCoord{0, 0}) == massBefore,
          "pigment: the stored `pig.m` half words are BIT-IDENTICAL across composites at four "
          "mask values -- a mask never writes mass, asserted by memcmp rather than argued");

    // Now the mixed pair, where they are wildly different, because `Mix`'s
    // weight IS the upper layer's mass. This is the printed pair of triples
    // that separates a mask from an eraser.
    auto mixedDoc = [&](float upperMass, bool masked, float maskValue) {
      Document doc = Document::createBlank(4, 4, WorkingSpace{});
      doc.layers[0] = makePigmentLayer("yellow");
      addLayer(doc, 1, makePigmentLayer("blue"));
      writePigment(doc, 0, 1, 1, zYellow, 1.0f);
      writePigment(doc, 1, 1, 1, zBlue, upperMass);
      setLayerBlend(doc, 1, BlendMode::Mix);
      if (masked) {
        doc.layers[1].mask.emplace();
        writeMask(doc, 1, 1, 1, maskValue);
      }
      return doc;
    };
    Document byMask = mixedDoc(1.0f, true, 0.5f);
    Document byMass = mixedDoc(0.5f, false, 0.0f);
    const std::array<float, 4> pMask = pixelOf(flattenDocumentToLinear(byMask), 1, 1);
    const std::array<float, 4> pMass = pixelOf(flattenDocumentToLinear(byMass), 1, 1);
    std::printf("  [measured] blue over yellow, HALF THE MASK at full mass: (%.3f, %.3f, %.3f) "
                "-- vs HALF THE MASS unmasked: (%.3f, %.3f, %.3f)\n",
                static_cast<double>(pMask[0]), static_cast<double>(pMask[1]),
                static_cast<double>(pMask[2]), static_cast<double>(pMass[0]),
                static_cast<double>(pMass[1]), static_cast<double>(pMass[2]));
    check(lutLoaded && pMass[1] > pMass[0] && pMass[1] > pMass[2],
          "pigment: half the MASS is the Kubelka-Munk mix -- green is the largest channel, "
          "PLAN.md's own Phase 5 verify sentence, unchanged by this step");
    check(lutLoaded && !near(pMask[0], pMass[0], 1.0e-2f) && !near(pMask[1], pMass[1], 1.0e-2f),
          "pigment: half the MASK is a measurably different colour -- the mask fades the pair "
          "toward the backdrop, the mass changes which pigment mixture is being computed");

    // And the mask's answer is exactly the right one: at full mass the mix IS
    // blue (t = 1), so a half mask must give the naive 50/50 of blue and
    // yellow. Computed from the same projections rather than eyeballed.
    const std::array<float, 3> rgbBlue = latentToRgb(zBlue);
    const std::array<float, 3> rgbYellow = latentToRgb(zYellow);
    check(lutLoaded && near(pMask[0], 0.5f * (rgbBlue[0] + rgbYellow[0]), 1.0e-3f) &&
              near(pMask[1], 0.5f * (rgbBlue[1] + rgbYellow[1]), 1.0e-3f) &&
              near(pMask[2], 0.5f * (rgbBlue[2] + rgbYellow[2]), 1.0e-3f) &&
              near(pMask[3], 1.0f, kUnpremultiplyTol),
          "pigment: a 0.5 mask on an opaque mixing layer gives exactly the 50/50 RGB blend of "
          "the two projections -- the mixing weight `t` is untouched, only coverage moved");

    // The two corners of core/Composite.hpp 3, now per texel. Both are
    // byte-identity claims, which is what makes them worth making.
    Document maskZeroUpper = mixedDoc(1.0f, true, 0.0f);
    Document upperDeleted = mixedDoc(1.0f, false, 0.0f);
    removeLayer(upperDeleted, 1);
    check(sameImage(flattenDocumentToLinear(maskZeroUpper),
                    flattenDocumentToLinear(upperDeleted)),
          "pigment: a 0.0 mask on the MIXING layer is byte-identically the layer being deleted "
          "-- the pair still lets the layer beneath it through, per texel");
    Document maskZeroLower = mixedDoc(1.0f, false, 0.0f);
    maskZeroLower.layers[0].mask.emplace();
    writeMask(maskZeroLower, 0, 1, 1, 0.0f);
    Document lowerHidden = mixedDoc(1.0f, false, 0.0f);
    lowerHidden.layers[0].visible = false;
    check(sameImage(flattenDocumentToLinear(maskZeroLower),
                    flattenDocumentToLinear(lowerHidden)),
          "pigment: a 0.0 mask on the LOWER half leaves the mixing layer visible and unmixed, "
          "byte-identically to hiding that layer -- not a blanked pair");
    // The mixing layer's stored mass is identical in the two masked documents
    // and in the unmasked one they were built from -- so none of the masking
    // above reached `pig.m`, which is the whole distinction between a mask and
    // PRD F10's eraser.
    Document unmaskedPair = mixedDoc(1.0f, false, 0.0f);
    const std::vector<uint16_t> pairMass = massWords(unmaskedPair, 1, TileCoord{0, 0});
    check(!pairMass.empty() && massWords(maskZeroLower, 1, TileCoord{0, 0}) == pairMass &&
              massWords(maskZeroUpper, 1, TileCoord{0, 0}) == pairMass &&
              massWords(byMask, 1, TileCoord{0, 0}) == pairMass,
          "pigment: and the mixing layer's stored mass is bit-identical across every masked "
          "variant and the unmasked one -- masking is not erasing (PRD F10 owns mass)");
  }

  // --- 6. Where the mask sits relative to the op stack -------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.125f, 0.0625f, 1.0f});
    doc.layers[0].mask.emplace();
    writeMask(doc, 0, 1, 1, 0.5f);
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = 1.0f;  // +1 stop: a pure doubling in linear
    doc.layers[0].ops.add(op);
    const std::array<float, 4> graded = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(near(graded[0], 0.5f, kUnpremultiplyTol) && near(graded[1], 0.25f, kUnpremultiplyTol) &&
              graded[3] == 0.5f,
          "ops: the grade doubles the colour and the mask halves the coverage -- the mask "
          "applies AFTER the op stack, with opacity, because it is coverage and the stack "
          "grades colour");
  }

  // --- 7. The eyedropper and the flattener still agree -------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {1.0f, 1.0f, 1.0f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("top"));
    writeRgb(doc, 1, 1, 1, {0.5f, 0.25f, 0.125f, 1.0f});
    doc.layers[1].mask.emplace();
    writeMask(doc, 1, 1, 1, 0.25f);

    ProbeParams all;
    all.sampleAllLayers = true;
    const ProbeSample sample = probePixel(doc, PixelCoord{1, 1}, all);
    const std::array<float, 4> flat = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(near(sample.linear[0], flat[0], kUnpremultiplyTol) &&
              near(sample.linear[1], flat[1], kUnpremultiplyTol) &&
              near(sample.linear[2], flat[2], kUnpremultiplyTol) &&
              near(sample.linear[3], flat[3], kUnpremultiplyTol),
          "probe: sampleAllLayers reads a masked layer through the same "
          "`layerMaskCoverageAt()` the flattener's per-tile fast path calls -- an eyedropper "
          "and an export that disagreed would be a bug nobody could explain");

    // A mixed Pigment pair with masks on both halves, which is the case where
    // the two loops differ most.
    Document mixed = Document::createBlank(4, 4, WorkingSpace{});
    mixed.layers[0] = makePigmentLayer("yellow");
    addLayer(mixed, 1, makePigmentLayer("blue"));
    writePigment(mixed, 0, 1, 1, zYellow, 1.0f);
    writePigment(mixed, 1, 1, 1, zBlue, 0.5f);
    setLayerBlend(mixed, 1, BlendMode::Mix);
    mixed.layers[0].mask.emplace();
    mixed.layers[1].mask.emplace();
    writeMask(mixed, 0, 1, 1, 0.75f);
    writeMask(mixed, 1, 1, 1, 0.5f);
    const ProbeSample mixedSample = probePixel(mixed, PixelCoord{1, 1}, all);
    const std::array<float, 4> mixedFlat = pixelOf(flattenDocumentToLinear(mixed), 1, 1);
    check(lutLoaded && near(mixedSample.linear[0], mixedFlat[0], kUnpremultiplyTol) &&
              near(mixedSample.linear[1], mixedFlat[1], kUnpremultiplyTol) &&
              near(mixedSample.linear[2], mixedFlat[2], kUnpremultiplyTol) &&
              near(mixedSample.linear[3], mixedFlat[3], kUnpremultiplyTol),
          "probe: and they agree on a mixed pair with a mask on BOTH halves, where the probe's "
          "per-texel lookup and the walk's per-tile hoist could most easily diverge");

    ProbeParams own;
    own.sampleAllLayers = false;
    own.activeLayerIndex = 1;
    const ProbeSample layerOwn = probePixel(doc, PixelCoord{1, 1}, own);
    check(layerOwn.linear[0] == 0.5f && layerOwn.linear[3] == 1.0f,
          "probe: single-layer mode IGNORES the mask, as it already ignores visible and "
          "opacity -- 'what is on this layer', which is what makes an eyedropper usable for "
          "checking what a mask hides");
  }

  // --- 8. The regression boundary, re-made across the new code path ------
  {
    // Step 1's boundary: a document whose layers do not overlap composites
    // byte-identically to the plain sum `over` replaced. The walk gained a
    // mask multiply this step, so the claim is re-made rather than inherited.
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.3f, 0.6f, 0.9f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("second"));
    writeRgb(doc, 1, 9, 9, {0.125f, 0.25f, 0.375f, 0.5f});
    std::vector<float> sum(16 * 16 * 4, 0.0f);
    for (const Layer& layer : doc.layers) {
      for (const auto& [coord, tile] : *layer.rgbTiles) {
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t dx = origin.x + tx, dy = origin.y + ty;
            if (dx < 0 || dx >= 16 || dy < 0 || dy >= 16) continue;
            const std::array<float, 4> px = tile.readPixel(PixelCoord{tx, ty});
            for (int i = 0; i < 4; ++i)
              sum[(static_cast<size_t>(dy) * 16 + static_cast<size_t>(dx)) * 4 + i] += px[i];
          }
        }
      }
    }
    const std::vector<float> walked = compositeDocumentPremultiplied(doc);
    check(walked.size() == sum.size() &&
              std::memcmp(walked.data(), sum.data(), sum.size() * sizeof(float)) == 0,
          "regression: a non-overlapping multi-layer document with NO masks still composites "
          "byte-identically to the plain sum -- the mask multiply costs an unmasked layer "
          "nothing at all, not even an ulp");
  }

  // --- 9. The `.npaint` round trip ---------------------------------------
  {
    const char* kPath = "selftest_mask.npaint";
    const char* kBare = "selftest_mask_bare.npaint";
    const char* kAgain = "selftest_mask_again.npaint";
    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);

    // Reads a file with OpenImageIO's `capDate` header attribute blanked, so
    // two files written in different seconds compare equal on everything else.
    // Same masking the external HEAD-vs-this-build comparison uses; done here
    // too so the property is a test rather than only a measurement.
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

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0] = makePigmentLayer("wash");
    writePigment(doc, 0, 1, 1, zBlue, 0.5f);
    addLayer(doc, 1, makeRgbLayer("plate"));
    writeRgb(doc, 1, 3, 4, {0.5f, 0.25f, 0.125f, 1.0f});
    addLayer(doc, 2, makeRgbLayer("reveal all"));
    writeRgb(doc, 2, 5, 6, {0.25f, 0.5f, 0.75f, 1.0f});

    // The mask-free file first, and it is the reference for the property this
    // step's format change had to keep: adding and removing a mask must leave
    // the bytes exactly where they were.
    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok, "npaint: a mask-free three-layer document saves");

    // Masks: a partial one on the Pigment layer, one on the RGB layer with a
    // tile OUTSIDE the layer's own content bounds, and a reveal-all one.
    addLayerMask(doc, 0);
    writeMask(doc, 0, 1, 1, 0.25f);
    addLayerMask(doc, 1);
    writeMask(doc, 1, 3, 4, 0.75f);
    writeMask(doc, 1, 200, 130, 0.5f);  // tile (1,1); the layer's content is in tile (0,0)
    addLayerMask(doc, 2);               // engaged, zero tiles

    {
      const NpaintSaveResult saved = saveNpaint(doc, kPath);
      check(saved.ok && saved.partsWritten == 4 && saved.warnings.empty(),
            "npaint: a document with three masked layers saves as four parts with nothing "
            "approximate about it");
      const NpaintLoadResult back = loadNpaint(kPath);
      check(back.ok && back.warnings.empty() && back.document.layers.size() == 3 &&
                back.document.layers[0].mask.has_value() &&
                back.document.layers[1].mask.has_value() &&
                back.document.layers[2].mask.has_value(),
            "npaint: all three come back WITH their masks -- a Pigment part's twelfth channel "
            "and an RGB part's fifth, both matched by name");
      if (back.ok && back.document.layers.size() == 3 &&
          back.document.layers[1].mask.has_value()) {
        check(back.document.layers[2].mask->occupiedTileCount() == 0,
              "npaint: the reveal-all mask round-trips as ENGAGED with zero tiles -- the "
              "channel's presence is what engages it, and an all-1.0 tile is dropped exactly "
              "as an all-zero content tile is");
        bool bitIdentical = true;
        for (size_t li : {size_t{0}, size_t{1}}) {
          const MaskTileStore& want = *doc.layers[li].mask;
          const MaskTileStore& got = *back.document.layers[li].mask;
          if (got.occupiedTileCount() != want.occupiedTileCount()) bitIdentical = false;
          for (const auto& [coord, tile] : want) {
            const MaskTile* g = got.find(coord);
            if (g == nullptr || std::memcmp(g->data(), tile.data(),
                                            MaskTile::kTexelCount * sizeof(uint16_t)) != 0)
              bitIdentical = false;
          }
        }
        check(bitIdentical,
              "npaint: every mask tile is BIT-IDENTICAL after the round trip -- HALF in, HALF "
              "out, no float stage, zero tolerance, the claim the other channels already make");
        check(back.document.layers[1].mask->occupiedTileCount() == 2 &&
                  back.document.layers[1].rgbTiles->occupiedTileCount() == 1,
              "npaint: a mask tile OUTSIDE the layer's content bounds survives -- the data "
              "window is the union of both stores, and the all-zero content tile it forces is "
              "dropped again on read");
        check(sameImage(flattenDocumentToLinear(doc),
                        flattenDocumentToLinear(back.document)),
              "npaint: and the reloaded document composites BYTE-IDENTICALLY to the saved one");
      }

      // The property that makes this format change safe: a mask-free document
      // is byte-for-byte what it was. Add three masks, remove them, save again.
      Document unmasked = doc;
      for (size_t i = 0; i < 3; ++i) removeLayerMask(unmasked, i);
      const NpaintSaveResult again = saveNpaint(unmasked, kAgain);
      check(again.ok && bytesWithoutCapDate(kBare) == bytesWithoutCapDate(kAgain) &&
                !bytesWithoutCapDate(kBare).empty(),
            "npaint: removing every mask gives back a file BYTE-IDENTICAL to the mask-free "
            "one (OpenImageIO's capDate timestamp masked, which HEAD's own two runs differ "
            "in too) -- the `mask` channel is written only when a mask exists");
      check(bytesWithoutCapDate(kPath).size() > bytesWithoutCapDate(kBare).size(),
            "npaint: and the masked file really is bigger, so the check above is not passing "
            "because nothing was ever written");
      const NpaintLoadResult bareBack = loadNpaint(kBare);
      check(bareBack.ok && bareBack.document.layers.size() == 3 &&
                !bareBack.document.layers[0].mask.has_value() &&
                !bareBack.document.layers[1].mask.has_value(),
            "npaint: a mask-free file loads back with `Layer::mask` DISENGAGED -- absent stays "
            "absent across a round trip, rather than becoming an engaged empty store");

      // A file whose mask channel holds values this build has to change. The
      // raw words are poked past writeCoverage()'s clamp, which is exactly
      // what another tool's writer could produce.
      Document poisoned = Document::createBlank(128, 128, WorkingSpace{});
      writeRgb(poisoned, 0, 1, 1, {0.5f, 0.5f, 0.5f, 1.0f});
      poisoned.layers[0].mask.emplace();
      MaskTile& mt = poisoned.layers[0].mask->getOrCreate(TileCoord{0, 0});
      mt.data()[0] = 0x7E00;  // NaN
      mt.data()[1] = 0x4000;  // 2.0
      mt.data()[2] = 0xBC00;  // -1.0
      const NpaintSaveResult poisonedSave = saveNpaint(poisoned, kPath);
      const NpaintLoadResult poisonedBack = loadNpaint(kPath);
      bool warned = false;
      for (const std::string& w : poisonedBack.warnings)
        if (contains(w, "3 mask sample(s)")) warned = true;
      check(poisonedSave.ok && poisonedBack.ok && warned,
            "npaint: a file carrying NaN / 2.0 / -1.0 mask samples loads with a warning naming "
            "the COUNT -- a silent clamp of data the user did not author is what PRD I11 "
            "forbids, and a mask is where a bad sample makes a layer vanish");
      if (poisonedBack.ok && !poisonedBack.document.layers.empty() &&
          poisonedBack.document.layers[0].mask.has_value()) {
        const MaskTile* got = poisonedBack.document.layers[0].mask->find(TileCoord{0, 0});
        check(got != nullptr && got->readCoverage(PixelCoord{0, 0}) == 0.0f &&
                  got->readCoverage(PixelCoord{1, 0}) == 1.0f &&
                  got->readCoverage(PixelCoord{2, 0}) == 0.0f &&
                  got->data()[0] == 0 && got->data()[1] == MaskTile::kRevealWord,
              "npaint: and the loaded words are the CLAMPED ones, not the originals -- what is "
              "stored, what is rendered and what the next save writes are one number");
      }
    }

    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);
    check(std::fopen(kPath, "rb") == nullptr && std::fopen(kBare, "rb") == nullptr &&
              std::fopen(kAgain, "rb") == nullptr,
          "npaint: every scratch file this section wrote is removed");
  }

  std::printf("[selftest] layer masks %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
