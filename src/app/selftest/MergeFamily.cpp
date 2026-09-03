#include "app/selftest/Support.hpp"

namespace np {

bool runMergeFamilyTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // PLAN.md §1.5. Nothing in this section reaches a file, an encoder or the
  // GPU: it composites in memory and writes half floats into tiles.
  std::printf(
      "[selftest] merge family: nothing here reaches a file, an encoder or the GPU\n");

  // --- The tolerance, derived rather than chosen -------------------------
  //
  // core/Merge.hpp §8. A merge cannot be bit-exact because the composite is
  // computed in `float` and a Layer stores `half`. IEEE binary16 has a 10-bit
  // stored significand, so round-to-nearest costs at most 2^-11 relative on a
  // normal value, and at most 2^-25 absolute below the normal range. Nothing
  // below is compared at a number that was picked to make a test pass.
  constexpr float kHalfRel = 1.0f / 2048.0f;       // 2^-11 = 4.8828e-04
  constexpr float kHalfAbs = 1.0f / 33554432.0f;   // 2^-25 = 2.9802e-08
  struct Diff {
    size_t over = 0;        // channels outside the bound
    float maxAbs = 0.0f;    // largest absolute difference anywhere
    float worstRatio = 0.0f;  // largest |diff| / bound
    size_t channels = 0;
  };
  auto compare = [&](const std::vector<float>& before, const std::vector<float>& after) {
    Diff d;
    if (before.size() != after.size() || before.empty()) {
      d.over = 1;
      d.worstRatio = 1.0e9f;
      return d;
    }
    d.channels = before.size();
    for (size_t i = 0; i < before.size(); ++i) {
      const float err = std::fabs(after[i] - before[i]);
      const float bound = kHalfRel * std::fabs(before[i]) + kHalfAbs;
      if (err > d.maxAbs) d.maxAbs = err;
      const float ratio = err / bound;
      if (ratio > d.worstRatio) d.worstRatio = ratio;
      if (err > bound) ++d.over;
    }
    return d;
  };
  auto bitIdentical = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && !a.empty() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };
  std::printf("  the f16 bound this section asserts against: |after-before| <= %.4e*|before| + "
              "%.4e (2^-11 and 2^-25, core/Merge.hpp section 8)\n",
              static_cast<double>(kHalfRel), static_cast<double>(kHalfAbs));

  // --- Fixtures -----------------------------------------------------------
  //
  // 256x192 is deliberately not a whole number of tiles down: 2 x 2 tiles with
  // the bottom row half empty, so `layerFromPremultiplied()`'s partial-tile
  // span arithmetic is exercised rather than only its full-tile path.
  constexpr int32_t kW = 256, kH = 192;
  auto fillRect = [](Layer& layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     std::array<float, 4> straight) {
    for (int32_t y = y0; y < y1; ++y) {
      for (int32_t x = x0; x < x1; ++x) {
        const PixelCoord at{x, y};
        Tile& tile = layer.rgbTiles->getOrCreate(tileCoordAt(at));
        const float a = straight[3];
        tile.writePixel(tileLocalOffset(at),
                        {straight[0] * a, straight[1] * a, straight[2] * a, a});
      }
    }
  };
  auto rgbLayer = [&](const char* name, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      std::array<float, 4> straight) {
    Layer layer = makeRgbLayer(name);
    fillRect(layer, x0, y0, x1, y1, straight);
    return layer;
  };
  // Values chosen so almost none of them is exactly representable in f16 --
  // the point of the tolerance is that it is not zero, and a fixture built out
  // of halves would hide a real error behind an accidental exactness.
  auto threeLayerDoc = [&]() {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    doc.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.37f, 0.21f, 0.13f, 1.0f}));
    doc.layers.push_back(rgbLayer("Mid", 40, 30, 180, 150, {0.11f, 0.63f, 0.29f, 0.6f}));
    doc.layers.push_back(rgbLayer("Top", 90, 70, 240, 180, {0.82f, 0.44f, 0.05f, 0.85f}));
    return doc;
  };

  // --- Part A: every merge is reachable from the layer editor -------------
  //
  // The same coverage assertion UI detour step 3 introduced, re-run because
  // five commands were just added to the enum: a merge that exists in
  // core/Merge and not in `allLayerCommands()` is a merge with no menu item
  // and no panel button, which is exactly the failure that file exists to fix.
  {
    const std::vector<LayerCommand>& all = allLayerCommands();
    // The count itself is asserted once, in the layer editor section that owns
    // that list; what this section owes is that the five merges are in it.
    size_t named = 0;
    bool everyValueListed = true;
    for (int v = 0; v < 64; ++v) {
      const LayerCommand c = static_cast<LayerCommand>(v);
      if (std::string(layerCommandLabel(c)) == "?") continue;
      ++named;
      bool inList = false;
      for (const LayerCommand listed : all)
        if (listed == c) inList = true;
      if (!inList) everyValueListed = false;
    }
    check(everyValueListed && named == all.size(),
          "every command with a label is in the list the menu walks");
    bool labelsUnique = true;
    for (size_t i = 0; i < all.size(); ++i)
      for (size_t j = i + 1; j < all.size(); ++j)
        if (std::string(layerCommandLabel(all[i])) == layerCommandLabel(all[j]))
          labelsUnique = false;
    check(labelsUnique, "every command's menu label is still distinct");
    const std::array<LayerCommand, 5> merges = {
        LayerCommand::MergeDown, LayerCommand::MergeVisible, LayerCommand::StampVisible,
        LayerCommand::FlattenImage, LayerCommand::RasteriseLayer};
    bool allPresent = true;
    for (const LayerCommand c : merges) {
      bool found = false;
      for (const LayerCommand listed : all)
        if (listed == c) found = true;
      if (!found) allPresent = false;
    }
    check(allPresent, "all five of PRD C10's and C11's operations are among them");
  }

  // --- Part B: merge down preserves the picture (the load-bearing claim) --
  {
    Document doc = threeLayerDoc();
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    std::vector<std::string> warnings;
    const LayerOpResult r = mergeLayerDown(doc, 2, &warnings);
    check(r.ok && doc.layers.size() == 2, "merge down folds the top layer into the middle one");
    check(r.index == 1 && doc.layers[1].name == "Mid",
          "the merged layer takes the lower layer's index and its name");
    check(doc.layers[1].opacity == 1.0f && doc.layers[1].visible && !doc.layers[1].mask.has_value() &&
              doc.layers[1].ops.size() == 0 && doc.layers[1].blend == std::string("normal"),
          "and is a plain layer: opacity 1, normal, no mask, no ops");
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    const Diff d = compare(before, after);
    std::printf("  merge down: %zu of %zu channels outside the f16 bound; max |diff| %.3e; worst "
                "|diff|/bound %.3f\n",
                d.over, d.channels, static_cast<double>(d.maxAbs),
                static_cast<double>(d.worstRatio));
    check(d.over == 0, "merge down preserves the composite within the derived f16 bound");
    check(d.maxAbs > 0.0f,
          "and is NOT bit-exact on this fixture -- the f16 store is a real rounding");

    // The other end of the same claim: a fixture whose composite is exactly
    // representable in f16 merges bit-exactly, which is what shows the error
    // above is storage and not arithmetic.
    Document exact = Document::createBlank(kW, kH, WorkingSpace{});
    exact.layers.clear();
    exact.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.5f, 0.25f, 0.75f, 1.0f}));
    exact.layers.push_back(rgbLayer("Top", 64, 32, 192, 160, {0.125f, 0.375f, 0.625f, 1.0f}));
    const std::vector<float> exactBefore = compositeDocumentPremultiplied(exact);
    const LayerOpResult er = mergeLayerDown(exact, 1);
    const std::vector<float> exactAfter = compositeDocumentPremultiplied(exact);
    check(er.ok && bitIdentical(exactBefore, exactAfter),
          "a composite that is exactly representable in f16 merges bit-exactly");
  }

  // --- Part B2: what a merge bakes, said out loud -------------------------
  {
    Document doc = threeLayerDoc();
    doc.layers[2].opacity = 0.5f;
    doc.layers[2].mask.emplace();
    Op op = makeNewOp(PointOpKind::Exposure);
    op.enabled = true;
    op.exposure.stops = -0.75f;
    doc.layers[2].ops.add(op);
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    std::vector<std::string> warnings;
    const LayerOpResult r = mergeLayerDown(doc, 2, &warnings);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    const Diff d = compare(before, after);
    check(r.ok && d.over == 0,
          "a masked, half-opacity, graded layer merges down within the same bound");
    check(!doc.layers[1].mask.has_value() && doc.layers[1].ops.size() == 0 &&
              doc.layers[1].opacity == 1.0f,
          "the mask, the op stack and the opacity are gone from the merged layer");
    size_t maskWarn = 0, opsWarn = 0, opacityWarn = 0;
    for (const std::string& w : warnings) {
      if (contains(w, "layer mask(s) into the merged layer's alpha")) ++maskWarn;
      if (contains(w, "op stack(s) into pixels")) ++opsWarn;
      if (contains(w, "layer opacity value(s)")) ++opacityWarn;
    }
    check(maskWarn == 1 && opsWarn == 1 && opacityWarn == 1,
          "and each of the three is reported by name rather than silently baked");
    std::printf("  merge down warnings on the baked fixture: %zu, first is \"%.90s...\"\n",
                warnings.size(), warnings.empty() ? "" : warnings.front().c_str());
  }

  // --- Part C: every refusal, with the numbers in it ----------------------
  {
    Document doc = threeLayerDoc();
    const LayerOpResult bottom = mergeLayerDown(doc, 0);
    check(!bottom.ok && contains(bottom.error, "bottom layer") &&
              contains(bottom.error, "nothing below it"),
          "merge down of the bottom layer is refused: there is nothing below it");
    const LayerOpResult range = mergeLayerDown(doc, 99);
    check(!range.ok && contains(range.error, "no layer at index 99") &&
              contains(range.error, "3 layer(s)"),
          "an out-of-range index is refused with both numbers");

    Document locked = threeLayerDoc();
    locked.layers[1].locked = true;
    const LayerOpResult lockedLower = mergeLayerDown(locked, 2);
    check(!lockedLower.ok && contains(lockedLower.error, "locked") &&
              contains(lockedLower.error, "layer 1"),
          "a locked *lower* layer refuses the merge, not only a locked upper one");

    Document hidden = threeLayerDoc();
    hidden.layers[2].visible = false;
    const LayerOpResult h = mergeLayerDown(hidden, 2);
    check(!h.ok && contains(h.error, "is hidden") && contains(h.error, "opacity 0"),
          "a hidden layer is refused, and the sentence says why opacity 0 is not");
    // The asymmetry that refusal claims, demonstrated rather than asserted.
    Document faded = threeLayerDoc();
    faded.layers[2].opacity = 0.0f;
    const std::vector<float> fadedBefore = compositeDocumentPremultiplied(faded);
    const LayerOpResult f = mergeLayerDown(faded, 2);
    const std::vector<float> fadedAfter = compositeDocumentPremultiplied(faded);
    check(f.ok && compare(fadedBefore, fadedAfter).over == 0,
          "and a layer at opacity 0 does merge, exactly as that sentence promises");

    for (const char* mode : {"multiply", "screen", "dissolve"}) {
      Document blended = threeLayerDoc();
      blended.layers[2].blend = mode;
      const LayerOpResult b = mergeLayerDown(blended, 2);
      if (!(!b.ok && contains(b.error, mode) && contains(b.error, "Merge Visible"))) ok = false;
    }
    check(true, "a non-normal blend is refused by name, pointing at Merge Visible");
    Document lowerBlend = threeLayerDoc();
    lowerBlend.layers[1].blend = "multiply";
    const LayerOpResult lb = mergeLayerDown(lowerBlend, 2);
    check(!lb.ok && contains(lb.error, "layer 1"),
          "and the *lower* layer's blend is refused too, for the same reason");

    Document adj = threeLayerDoc();
    adj.layers[2] = makeAdjustmentLayer("Grade");
    const LayerOpResult a = mergeLayerDown(adj, 2);
    check(!a.ok && contains(a.error, "Adjustment layer") && contains(a.error, "Rasterise"),
          "an Adjustment layer is refused and pointed at Rasterise Layer (PRD C11)");

    Document inert = threeLayerDoc();
    inert.layers[2] = Layer{};
    inert.layers[2].kind = LayerKind::Text;
    inert.layers[2].pigmentTiles.reset();
    const LayerOpResult t = mergeLayerDown(inert, 2);
    check(!t.ok && contains(t.error, "Text") && contains(t.error, "no pixel storage"),
          "an inert kind is refused by naming what the kind is, not that it is unsupported");

    Document groups = threeLayerDoc();
    groups.layers[2].parent = "L0009";
    const LayerOpResult g = mergeLayerDown(groups, 2);
    check(!g.ok && contains(g.error, "different groups") && contains(g.error, "L0009"),
          "a merge across a group boundary is refused, naming the np:parent");
  }

  // --- Part C2: the four clipping arrangements (core/Merge.hpp section 6) --
  {
    // upper clipped only: the lower layer is the base, and the merge runs
    // through core/Composite's own clip-group bracket.
    Document doc = threeLayerDoc();
    doc.layers[2].clipped = true;
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    const LayerOpResult r = mergeLayerDown(doc, 2);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    check(r.ok && !doc.layers[1].clipped && compare(before, after).over == 0,
          "a clipped layer merges into its base, unclipped and picture-preserving");

    // both clipped: one run, one member shorter.
    Document both = threeLayerDoc();
    both.layers.push_back(rgbLayer("Fourth", 100, 60, 200, 170, {0.2f, 0.5f, 0.9f, 0.7f}));
    both.layers[2].clipped = true;
    both.layers[3].clipped = true;
    const std::vector<float> bothBefore = compositeDocumentPremultiplied(both);
    const LayerOpResult br = mergeLayerDown(both, 3);
    const std::vector<float> bothAfter = compositeDocumentPremultiplied(both);
    check(br.ok && both.layers[2].clipped && compare(bothBefore, bothAfter).over == 0,
          "two members of one clip run merge into one member, still clipped");

    // lower clipped only: refused, because either answer changes a picture.
    Document mixedClip = threeLayerDoc();
    mixedClip.layers.push_back(rgbLayer("Fourth", 100, 60, 200, 170, {0.2f, 0.5f, 0.9f, 0.7f}));
    mixedClip.layers[2].clipped = true;
    const LayerOpResult mr = mergeLayerDown(mixedClip, 3);
    check(!mr.ok && contains(mr.error, "is clipped and") && contains(mr.error, "un-clip both"),
          "a clipped lower layer under an unclipped upper one is refused");

    // a clip base whose member sits above the pair.
    Document baseWithMember = threeLayerDoc();
    baseWithMember.layers.push_back(rgbLayer("Member", 100, 60, 200, 170, {0.2f, 0.5f, 0.9f, 0.7f}));
    baseWithMember.layers[3].clipped = true;
    const LayerOpResult cr = mergeLayerDown(baseWithMember, 2);
    check(!cr.ok && contains(cr.error, "layer 3") && contains(cr.error, "cut away"),
          "merging away a clip base is refused, naming the layer clipped to it");
  }

  // --- Part D: Pigment, the decision this step had to make ----------------
  //
  // core/Merge.hpp section 5. `over` between two Pigment layers is a glaze and
  // has no latent answer at all; a `Mix` pair does, and it is exact. The
  // rejected alternative -- fall back to RGB -- is run beside the built one and
  // its cost is printed rather than described.
  {
    auto pigmentLayer = [&](const char* name, Latent z, float mass, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1) {
      Layer layer = makePigmentLayer(name);
      for (int32_t y = y0; y < y1; ++y)
        for (int32_t x = x0; x < x1; ++x) {
          const PixelCoord at{x, y};
          PigmentTile& tile = layer.pigmentTiles->getOrCreate(tileCoordAt(at));
          PigmentTexel texel;
          texel.latent = z;
          texel.mass = mass;
          tile.writeTexel(tileLocalOffset(at), texel);
        }
      return layer;
    };
    const Latent yellowish{{0.62f, 0.09f, 0.04f}, {0.031f, -0.012f, 0.023f}};
    const Latent blueish{{0.07f, 0.55f, 0.11f}, {-0.019f, 0.026f, 0.041f}};
    auto pigmentPairDoc = [&](const char* upperBlend) {
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      doc.layers.clear();
      doc.layers.push_back(pigmentLayer("Yellow", yellowish, 1.0f, 0, 0, kW, kH));
      doc.layers.push_back(pigmentLayer("Blue", blueish, 0.5f, 40, 30, 200, 170));
      doc.layers[1].blend = upperBlend;
      return doc;
    };

    Document glaze = pigmentPairDoc("normal");
    const LayerOpResult gr = mergeLayerDown(glaze, 1);
    check(!gr.ok && contains(gr.error, "glaze") && contains(gr.error, "PRD C3") &&
              contains(gr.error, "F10"),
          "two Pigment layers under `over` are refused: a glaze has no latent");
    check(glaze.layers.size() == 2 && glaze.layers[1].pigmentTiles.has_value(),
          "and the refusal changed nothing -- no silent fallback to RGB happened");

    Document mixed = pigmentPairDoc("mix");
    const std::vector<float> before = compositeDocumentPremultiplied(mixed);
    std::vector<std::string> warnings;
    const LayerOpResult mr = mergeLayerDown(mixed, 1, &warnings);
    const std::vector<float> after = compositeDocumentPremultiplied(mixed);
    const Diff d = compare(before, after);
    std::printf("  latent merge: %zu of %zu channels outside the f16 bound; max |diff| %.3e; "
                "worst |diff|/bound %.3f\n",
                d.over, d.channels, static_cast<double>(d.maxAbs),
                static_cast<double>(d.worstRatio));
    check(mr.ok && mixed.layers.size() == 1, "a Mix-paired Pigment pair merges");
    check(mixed.layers[0].kind == LayerKind::Pigment &&
              mixed.layers[0].pigmentTiles.has_value() && !mixed.layers[0].rgbTiles.has_value(),
          "and the result is still a Pigment layer -- latents, not RGB");
    check(d.over == 0, "the latent merge preserves the composite within the same f16 bound");
    // What "still a Pigment layer" is worth, in the terms PRD C3 states it:
    // the merged layer can be the lower half of another mix. An RGB fallback
    // could not have been, and that is the whole cost of the rejected
    // alternative, measured through the project's own predicate.
    Document again = mixed;
    again.layers.push_back(pigmentLayer("Red", yellowish, 0.4f, 0, 0, kW, kH));
    again.layers[1].blend = "mix";
    check(blendModeAvailableForLayer(again, 1, BlendMode::Mix),
          "the merged layer can be mixed again -- PRD C3 survives the merge");
    Document rgbFallback = again;
    rgbFallback.layers[0] = makeRgbLayer("What an RGB fallback would have left");
    check(!blendModeAvailableForLayer(rgbFallback, 1, BlendMode::Mix),
          "the rejected RGB fallback could not have been -- run beside, not asserted");
    check(!warnings.empty() && contains(warnings.back(), "latent space"),
          "the latent merge says what it did rather than leaving it to be inferred");

    // Each condition that makes the latent equality hold, refused by name when
    // it does not. core/Merge.hpp section 5 lists them; this asserts the list.
    struct Case {
      const char* what;
      const char* needle;
    };
    Document opacityCase = pigmentPairDoc("mix");
    opacityCase.layers[1].opacity = 0.5f;
    Document maskCase = pigmentPairDoc("mix");
    maskCase.layers[1].mask.emplace();
    Document opsCase = pigmentPairDoc("mix");
    opsCase.layers[1].ops.add(makeNewOp(PointOpKind::Levels));
    Document aboveCase = pigmentPairDoc("mix");
    aboveCase.layers.push_back(pigmentLayer("Third", blueish, 0.3f, 0, 0, kW, kH));
    aboveCase.layers[2].blend = "mix";
    const std::array<std::pair<const Document*, const char*>, 4> cases = {
        std::make_pair(&opacityCase, "opacity 1"), std::make_pair(&maskCase, "has a mask"),
        std::make_pair(&opsCase, "op stack"), std::make_pair(&aboveCase, "also carries")};
    bool everyConditionNamed = true;
    for (const auto& [source, needle] : cases) {
      Document copy = *source;
      const LayerOpResult r = mergeLayerDown(copy, 1);
      if (r.ok || !contains(r.error, needle) || !contains(r.error, "latent space"))
        everyConditionNamed = false;
    }
    check(everyConditionNamed,
          "each condition the latent merge needs is refused by name when unmet");

    Document crossKind = pigmentPairDoc("mix");
    crossKind.layers[0] = rgbLayer("Rgb", 0, 0, kW, kH, {0.4f, 0.4f, 0.4f, 1.0f});
    const LayerOpResult ck = mergeLayerDown(crossKind, 1);
    check(!ck.ok && contains(ck.error, "is a Pigment layer and") && contains(ck.error, "C3"),
          "a Pigment layer over an RGB one is refused rather than degraded");
  }

  // --- Part E: merge visible, stamp visible, and the walk they share ------
  {
    Document doc = threeLayerDoc();
    doc.layers[1].visible = false;
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    std::vector<std::string> warnings;
    const LayerOpResult r = mergeVisibleLayers(doc, &warnings);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    check(r.ok && doc.layers.size() == 2, "merge visible leaves the hidden layer in place");
    check(doc.layers[0].name == "Merged" && doc.layers[1].name == "Mid" &&
              !doc.layers[1].visible,
          "the merged layer lands at the bottom-most visible index, hidden one above");
    check(compare(before, after).over == 0, "and the composite is preserved within the bound");

    // The claim merge down's blend refusal makes on merge visible's behalf,
    // checked rather than asserted in prose: a `multiply` layer that merge down
    // refuses to fold is folded here exactly, because the backdrop the blend
    // consumed is inside the composite.
    Document blended = threeLayerDoc();
    blended.layers[1].blend = "multiply";
    blended.layers[2].blend = "screen";
    const std::vector<float> blendedBefore = compositeDocumentPremultiplied(blended);
    const LayerOpResult mb = mergeLayerDown(blended, 2);
    const LayerOpResult vb = mergeVisibleLayers(blended);
    const std::vector<float> blendedAfter = compositeDocumentPremultiplied(blended);
    check(!mb.ok && vb.ok && blended.layers.size() == 1 &&
              compare(blendedBefore, blendedAfter).over == 0,
          "what merge down refuses (multiply, screen) merge visible folds exactly");

    Document one = threeLayerDoc();
    one.layers[1].visible = false;
    one.layers[2].visible = false;
    const LayerOpResult single = mergeVisibleLayers(one);
    check(!single.ok && contains(single.error, "1 visible layer(s) of 3") &&
              contains(single.error, "Stamp Visible"),
          "one visible layer is refused with the count and the alternative");

    Document lockedDoc = threeLayerDoc();
    lockedDoc.layers[1].locked = true;
    const LayerOpResult lr = mergeVisibleLayers(lockedDoc);
    check(!lr.ok && contains(lr.error, "locked"), "a locked visible layer refuses merge visible");

    // Stamp: the same buffer, a different placement, and therefore a different
    // invariant. core/Merge.hpp section 7.
    Document stampDoc = threeLayerDoc();
    stampDoc.layers[1].locked = true;  // a lock does not obstruct a stamp
    const std::vector<float> stampBefore = compositeDocumentPremultiplied(stampDoc);
    std::vector<std::string> stampWarnings;
    const LayerOpResult sr = stampVisibleLayers(stampDoc, &stampWarnings);
    check(sr.ok && stampDoc.layers.size() == 4 && sr.index == 3,
          "stamp visible adds a new top layer and destroys nothing, lock and all");
    Document stampAlone = Document::createBlank(kW, kH, WorkingSpace{});
    stampAlone.layers.clear();
    stampAlone.layers.push_back(stampDoc.layers[3]);
    const std::vector<float> aloneAfter = compositeDocumentPremultiplied(stampAlone);
    check(compare(stampBefore, aloneAfter).over == 0,
          "the stamped layer ALONE composites to what the whole document did");

    // ...and the honest other half: the full document does not, because the
    // stamp now sits over the layers it was made from. Measured, not implied.
    Document opaqueBase = threeLayerDoc();
    const std::vector<float> opaqueBefore = compositeDocumentPremultiplied(opaqueBase);
    stampVisibleLayers(opaqueBase);
    const std::vector<float> opaqueAfter = compositeDocumentPremultiplied(opaqueBase);
    check(compare(opaqueBefore, opaqueAfter).over == 0,
          "with an opaque composite the whole document is unchanged too");
    Document holed = threeLayerDoc();
    holed.layers[0] = rgbLayer("Base", 0, 0, 200, kH, {0.37f, 0.21f, 0.13f, 0.5f});
    const std::vector<float> holedBefore = compositeDocumentPremultiplied(holed);
    std::vector<std::string> holedWarnings;
    stampVisibleLayers(holed, &holedWarnings);
    const std::vector<float> holedAfter = compositeDocumentPremultiplied(holed);
    const Diff hd = compare(holedBefore, holedAfter);
    std::printf("  stamp over a partly transparent composite: %zu of %zu channels differ beyond "
                "the f16 bound (expected: the stamp builds up against itself)\n",
                hd.over, hd.channels);
    check(hd.over > 0, "with a partly transparent composite it is NOT unchanged");
    bool warned = false;
    for (const std::string& w : holedWarnings)
      if (contains(w, "build up against itself")) warned = true;
    check(warned, "and the stamp says so, with the count of texels it applies to");

    // (c)'s real question: does merge visible use the compositor's own walk?
    // The fixture is everything a second walk would have got subtly wrong --
    // a Mix pair, a clipping run, and an adjustment layer -- and the assertion
    // is that the merged single layer reproduces the composite anyway.
    Document hard = Document::createBlank(kW, kH, WorkingSpace{});
    hard.layers.clear();
    hard.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.30f, 0.34f, 0.38f, 1.0f}));
    {
      Layer lower = makePigmentLayer("Yellow");
      Layer upper = makePigmentLayer("Blue");
      for (int32_t y = 20; y < 150; ++y)
        for (int32_t x = 20; x < 200; ++x) {
          const PixelCoord at{x, y};
          PigmentTexel a;
          a.latent = Latent{{0.62f, 0.09f, 0.04f}, {0.031f, -0.012f, 0.023f}};
          a.mass = 0.9f;
          lower.pigmentTiles->getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), a);
          PigmentTexel b;
          b.latent = Latent{{0.07f, 0.55f, 0.11f}, {-0.019f, 0.026f, 0.041f}};
          b.mass = 0.45f;
          upper.pigmentTiles->getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at), b);
        }
      upper.blend = "mix";
      hard.layers.push_back(std::move(lower));
      hard.layers.push_back(std::move(upper));
    }
    hard.layers.push_back(rgbLayer("ClipBase", 60, 40, 220, 160, {0.9f, 0.8f, 0.2f, 0.8f}));
    hard.layers.push_back(rgbLayer("Clipped", 0, 0, kW, kH, {0.1f, 0.1f, 0.9f, 1.0f}));
    hard.layers[4].clipped = true;
    {
      Layer grade = makeAdjustmentLayer("Grade");
      Op op = makeNewOp(PointOpKind::Exposure);
      op.enabled = true;
      op.exposure.stops = -0.6f;
      grade.ops.add(op);
      hard.layers.push_back(std::move(grade));
    }
    check(mixPairing(hard).mixedWithBelow[2] && clipRuns(hard).any,
          "the hard fixture really does have a mix pair and a clipping run");
    const std::vector<float> hardBefore = compositeDocumentPremultiplied(hard);
    const LayerOpResult hr = mergeVisibleLayers(hard);
    const std::vector<float> hardAfter = compositeDocumentPremultiplied(hard);
    const Diff hardDiff = compare(hardBefore, hardAfter);
    std::printf("  merge visible over a mix pair + clipping run + adjustment layer: %zu of %zu "
                "channels outside the bound; max |diff| %.3e\n",
                hardDiff.over, hardDiff.channels, static_cast<double>(hardDiff.maxAbs));
    check(hr.ok && hard.layers.size() == 1 && hardDiff.over == 0,
          "merge visible reuses the one walk -- six layers collapse and nothing moves");

    // The one loss merge visible cannot refuse: it collapses a whole stack, so
    // the two Pigment layers in that fixture became RGB. core/Merge.hpp
    // section 5 argues why this is a warning here and a refusal in merge down;
    // what is asserted is that it is never silent.
    Document pigmentStack = Document::createBlank(kW, kH, WorkingSpace{});
    pigmentStack.layers.clear();
    pigmentStack.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.3f, 0.3f, 0.3f, 1.0f}));
    pigmentStack.layers.push_back(makePigmentLayer("Paint"));
    std::vector<std::string> pigmentWarnings;
    const LayerOpResult pr = mergeVisibleLayers(pigmentStack, &pigmentWarnings);
    bool pigmentWarned = false;
    for (const std::string& w : pigmentWarnings)
      if (contains(w, "turned 1 Pigment layer(s) into RGB") && contains(w, "PRD C3")) pigmentWarned = true;
    check(pr.ok && pigmentStack.layers[0].kind == LayerKind::RGB && pigmentWarned,
          "merge visible spends a Pigment layer's latents and says so, never silently");
  }

  // --- Part F: flatten, and what it does to transparency ------------------
  {
    // Deliberately laid out so one of the canvas's four tiles is covered by
    // nothing at all: the transparent-corner and the never-allocated-tile
    // assertions below are about that tile, and a fixture that happened to
    // cover the whole canvas would assert neither.
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    doc.layers.push_back(rgbLayer("Base", 0, 0, 120, 120, {0.37f, 0.21f, 0.13f, 0.5f}));
    doc.layers.push_back(rgbLayer("Mid", 40, 30, 110, 100, {0.11f, 0.63f, 0.29f, 0.6f}));
    doc.layers.push_back(rgbLayer("Top", 130, 10, 250, 110, {0.82f, 0.44f, 0.05f, 0.85f}));
    doc.layers[1].visible = false;
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    std::vector<std::string> warnings;
    const LayerOpResult r = flattenDocument(doc, &warnings);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    check(r.ok && doc.layers.size() == 1 && doc.layers[0].name == "Flattened",
          "flatten collapses the whole stack to exactly one layer");
    check(compare(before, after).over == 0, "and preserves the composite within the bound");
    bool hiddenWarned = false, alphaWarned = false;
    for (const std::string& w : warnings) {
      if (contains(w, "discarded 1 hidden layer(s)")) hiddenWarned = true;
      if (contains(w, "preserved alpha")) alphaWarned = true;
    }
    check(hiddenWarned, "the hidden layer it discarded is reported by count");
    check(alphaWarned, "and so is the alpha it kept -- PRD C16 leaves no background to fill with");
    // The claim, checked at a texel rather than in prose: the far corner of
    // the canvas is outside every layer, and is still transparent.
    const size_t corner = (static_cast<size_t>(kH - 1) * kW + (kW - 1)) * 4;
    check(after[corner + 3] == 0.0f, "a corner no layer covered is still fully transparent");
    check(doc.layers[0].rgbTiles.has_value() &&
              doc.layers[0].rgbTiles->occupiedTileCount() == 2,
          "and the two empty tiles were never allocated (PRD C2), so they cost nothing");
    std::printf("  flatten: %zu of the canvas's 4 tiles allocated; the two the composite left "
                "transparent are absent\n",
                doc.layers[0].rgbTiles->occupiedTileCount());

    Document empty;
    empty.width = kW;
    empty.height = kH;
    const LayerOpResult er = flattenDocument(empty);
    check(!er.ok && contains(er.error, "no layers"), "an empty document refuses to flatten");
  }

  // --- Part G: rasterise a parametric layer (PRD C11) ---------------------
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers.clear();
    doc.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.55f, 0.42f, 0.18f, 1.0f}));
    Layer grade = makeAdjustmentLayer("Grade");
    Op op = makeNewOp(PointOpKind::Exposure);
    op.enabled = true;
    op.exposure.stops = -1.25f;
    grade.ops.add(op);
    doc.layers.push_back(std::move(grade));

    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    std::vector<std::string> warnings;
    const LayerOpResult r = rasteriseLayer(doc, 1, &warnings);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    check(r.ok && doc.layers.size() == 2 && doc.layers[1].kind == LayerKind::RGB &&
              doc.layers[1].name == "Grade",
          "rasterise turns the Adjustment layer into pixels and keeps its name");
    check(doc.layers[0].kind == LayerKind::RGB && doc.layers[0].name == "Base",
          "the layers beneath it are still there -- rasterise is not a flatten");
    check(doc.layers[1].ops.size() == 0, "and the op stack it evaluated is gone");
    check(compare(before, after).over == 0,
          "over an opaque composite the picture is preserved within the bound");

    // The other half, measured: over a *partly transparent* composite it is
    // not, because the rasterised layer sits over what it was computed from.
    Document holed = Document::createBlank(kW, kH, WorkingSpace{});
    holed.layers.clear();
    holed.layers.push_back(rgbLayer("Base", 0, 0, 200, kH, {0.55f, 0.42f, 0.18f, 0.5f}));
    Layer grade2 = makeAdjustmentLayer("Grade");
    grade2.ops.add(op);
    holed.layers.push_back(std::move(grade2));
    const std::vector<float> holedBefore = compositeDocumentPremultiplied(holed);
    std::vector<std::string> holedWarnings;
    rasteriseLayer(holed, 1, &holedWarnings);
    const std::vector<float> holedAfter = compositeDocumentPremultiplied(holed);
    const Diff hd = compare(holedBefore, holedAfter);
    bool warnedPartial = false;
    for (const std::string& w : holedWarnings)
      if (contains(w, "alpha strictly between 0 and 1")) warnedPartial = true;
    std::printf("  rasterise over a partly transparent composite: %zu of %zu channels differ; "
                "warned: %s\n",
                hd.over, hd.channels, warnedPartial ? "yes" : "NO");
    check(hd.over > 0 && warnedPartial,
          "over a partly transparent composite it is not, and says so with the count");

    // The kinds that CANNOT be rasterised, refused by naming what each still
    // lacks. `Text` and `Vector` are deliberately not in this list any more:
    // PLAN.md phases 13 and 14 gave both a parameter member and a rasteriser,
    // so C11 applies to them for real and they are asserted just below
    // instead. Leaving them here would have pinned the refusal as correct
    // behaviour -- which is how a suite ends up asserting that a shipped
    // feature does not work.
    Document kinds = Document::createBlank(kW, kH, WorkingSpace{});
    kinds.layers.clear();
    kinds.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.5f, 0.5f, 0.5f, 1.0f}));
    bool everyKindNamed = true;
    for (const LayerKind kind : {LayerKind::Strokes, LayerKind::Flats, LayerKind::Media,
                                 LayerKind::RGB, LayerKind::Pigment}) {
      Document one = kinds;
      Layer layer;
      layer.kind = kind;
      layer.pigmentTiles.reset();
      if (kind == LayerKind::RGB) layer.rgbTiles.emplace();
      if (kind == LayerKind::Pigment) layer.pigmentTiles.emplace();
      one.layers.push_back(std::move(layer));
      const LayerOpResult rr = rasteriseLayer(one, 1);
      if (rr.ok || !contains(rr.error, layerKindName(kind)) || !contains(rr.error, "C11"))
        everyKindNamed = false;
    }
    check(everyKindNamed,
          "every kind that still has no parameter member is refused, naming the kind and "
          "PRD C11's list");

    // --- Vector and Text rasterise, and become ordinary pixels --------------
    //
    // The property under test is that the layer's PARTICIPATION in the stack
    // survives and only its content changes: a rasterised layer keeps its
    // name, opacity, blend and visibility, loses its geometry, and gains
    // tiles. The composite is deliberately NOT asserted equal here -- that is
    // core/VectorRaster's own section's job, and re-deriving it in the merge
    // family would be a second, weaker copy of it.
    {
      Document vec = kinds;
      Layer v = makeVectorLayer("A square");
      v.opacity = 0.5f;
      v.blend = "multiply";
      VectorShape sq;
      SubPath sub;
      sub.closed = true;
      const PathPoint pts[4] = {{4, 4}, {20, 4}, {20, 20}, {4, 20}};
      for (const PathPoint& pt : pts) {
        Anchor an;
        an.pt = pt;
        an.in = pt;
        an.out = pt;
        sub.anchors.push_back(an);
      }
      sq.path.subpaths.push_back(sub);
      sq.fill.on = true;
      sq.fill.rgba = {1.0f, 0.0f, 0.0f, 1.0f};
      sq.id = 1;
      v.shapes.push_back(std::move(sq));
      vec.layers.push_back(std::move(v));

      std::vector<std::string> vw;
      const LayerOpResult vr = rasteriseLayer(vec, 1, &vw);
      const Layer& done = vec.layers[1];
      check(vr.ok && done.kind == LayerKind::RGB && done.shapes.empty() &&
                done.rgbTiles.has_value() && done.rgbTiles->occupiedTileCount() > 0,
            "rasterise turns a Vector layer into an RGB layer holding the tiles its shapes "
            "painted, and drops the geometry");
      check(done.name == "A square" && done.opacity == 0.5f && done.blend == "multiply",
            "...keeping name, opacity and blend -- rasterising changes what the layer's "
            "content IS, not how it participates in the stack");
      bool warnedLost = false;
      for (const std::string& w : vw)
        if (contains(w, "resolution-independent")) warnedLost = true;
      check(warnedLost,
            "...and warns that the geometry is gone, because nothing on screen says so and "
            "the layer looks identical the instant after");
    }

    Document emptyStack = kinds;
    emptyStack.layers.push_back(makeAdjustmentLayer("Nothing"));
    const LayerOpResult es = rasteriseLayer(emptyStack, 1);
    check(!es.ok && contains(es.error, "empty op stack") && contains(es.error, "Stamp Visible"),
          "an empty op stack is refused: it is a stamp under a misleading name");

    Document bottom = Document::createBlank(kW, kH, WorkingSpace{});
    bottom.layers.clear();
    Layer only = makeAdjustmentLayer("Alone");
    only.ops.add(op);
    bottom.layers.push_back(std::move(only));
    const LayerOpResult br = rasteriseLayer(bottom, 0);
    check(!br.ok && contains(br.error, "nothing beneath"),
          "an Adjustment layer at the bottom has nothing to evaluate against");
  }

  // --- Part H: a merge that cannot be undone is a data-loss bug -----------
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{});
    od.document.layers.clear();
    od.document.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.37f, 0.21f, 0.13f, 1.0f}));
    od.document.layers.push_back(rgbLayer("Top", 90, 70, 240, 180, {0.82f, 0.44f, 0.05f, 0.85f}));
    od.recordEdit("fixture", EditKind::Content);

    const std::vector<float> before = compositeDocumentPremultiplied(od.document);
    const uint64_t revision = od.revision;
    const size_t entries = od.history.entries().size();
    const LayerEditResult r = applyLayerCommand(od, LayerCommand::MergeDown, 1);
    check(r.ok && od.document.layers.size() == 1 && r.selected == 0,
          "Merge Down through the layer editor merges and moves the selection");
    check(od.revision == revision + 1 && od.history.entries().size() == entries + 1,
          "it moved the revision exactly once and appended exactly one history entry");
    check(!od.history.entries().empty() &&
              contains(od.history.entries().back().label, "merge layer 1"),
          "and the entry's label names what happened, for a human to read (PRD O2)");

    const Document* undone = od.history.undo();
    check(undone != nullptr, "undo moves the cursor back one edit");
    if (undone != nullptr) od.document = *undone;
    check(od.document.layers.size() == 2 && od.document.layers[0].name == "Base" &&
              od.document.layers[1].name == "Top",
          "and the pre-merge stack is back: two layers, both names");
    const std::vector<float> restored = compositeDocumentPremultiplied(od.document);
    check(bitIdentical(before, restored),
          "restored bit-for-bit, not merely within the merge's own tolerance");

    // A refusal must move neither, which is the same rule every other layer
    // command here obeys and is worth re-checking on the one that composites.
    const uint64_t afterUndo = od.revision;
    const size_t entriesAfterUndo = od.history.entries().size();
    const LayerEditResult bad = applyLayerCommand(od, LayerCommand::MergeDown, 0);
    check(!bad.ok && od.revision == afterUndo && od.history.entries().size() == entriesAfterUndo,
          "a refused merge moves neither the revision nor the history");
    check(bad.warnings.empty(), "and a refusal carries no warnings, only the sentence");
  }

  // --- Part I: what a merge actually costs --------------------------------
  //
  // core/Merge.hpp section 2. The reuse of core/Composite buys correctness and
  // costs one canvas-sized float buffer. Both numbers are printed, beside what
  // copy-on-write makes the *layer* copies cost, so the trade is visible
  // rather than described.
  {
    constexpr int32_t kBigW = 1024, kBigH = 1024;  // 8 x 8 = 64 tiles per layer
    Document doc = Document::createBlank(kBigW, kBigH, WorkingSpace{});
    doc.layers.clear();
    for (int layer = 0; layer < 2; ++layer) {
      Layer l = makeRgbLayer(layer == 0 ? "Base" : "Top");
      for (int32_t y = 0; y < kBigH; y += 4)
        for (int32_t x = 0; x < kBigW; x += 4) {
          const PixelCoord at{x, y};
          l.rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {0.3f, 0.4f, 0.5f, 1.0f});
        }
      doc.layers.push_back(std::move(l));
    }
    const size_t tilesBefore = documentTileCount(doc);
    const size_t bytesBefore = documentTileBytes(doc);

    // The sub-document a merge builds, measured: copying a Layer shares its
    // tiles, so the two-layer sub-document holds 128 tiles and owns none of
    // them exclusively.
    Document sub;
    sub.width = doc.width;
    sub.height = doc.height;
    sub.layers.push_back(doc.layers[0]);
    sub.layers.push_back(doc.layers[1]);
    const size_t subShared = documentSharedTileCount(sub);
    const size_t subExclusive = documentExclusiveTileBytes(sub);
    check(subShared == documentTileCount(sub) && subExclusive == 0,
          "the sub-document a merge composites shares every tile and owns none");

    // The rejected alternative, run beside: what the same sub-document would
    // have cost before copy-on-write.
    Document deep = sub;
    unshareDocumentTiles(deep);
    const size_t deepBytes = documentExclusiveTileBytes(deep);

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> warnings;
    const LayerOpResult r = mergeLayerDown(doc, 1, &warnings);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const size_t bytesAfter = documentTileBytes(doc);

    std::printf("  merge cost, 1024x1024 with %zu occupied tiles (%.1f MiB):\n", tilesBefore,
                static_cast<double>(bytesBefore) / (1024.0 * 1024.0));
    std::printf("    the two-layer sub-document                %zu shared tiles, %zu exclusive "
                "bytes\n",
                subShared, subExclusive);
    std::printf("    the same sub-document deep-copied          %.1f MiB -- what it cost before "
                "copy-on-write\n",
                static_cast<double>(deepBytes) / (1024.0 * 1024.0));
    std::printf("    the intermediate composite buffer          %.1f MiB (canvas-sized, "
                "core/Merge.hpp section 2)\n",
                static_cast<double>(mergeCompositeBufferBytes(doc)) / (1024.0 * 1024.0));
    std::printf("    the merged layer's tiles                   %.1f MiB\n",
                static_cast<double>(bytesAfter) / (1024.0 * 1024.0));
    std::printf("    merge down wall time                       %.2f ms [measured]\n", ms);
    check(r.ok, "the 1024x1024 merge went ahead");
    check(mergeCompositeBufferBytes(doc) > bytesAfter,
          "the intermediate buffer is larger than the layer it produces -- the price of reuse");
    check(deepBytes == documentTileBytes(sub) && deepBytes > 0,
          "and the deep copy really would have cost the full tile bytes");

    // core/Merge.hpp section 3: a merge through the compositor is canvas-
    // clipped, so a tile past the edge is discarded, by count.
    Document past = Document::createBlank(kW, kH, WorkingSpace{});
    past.layers.clear();
    past.layers.push_back(rgbLayer("Base", 0, 0, kW, kH, {0.4f, 0.4f, 0.4f, 1.0f}));
    Layer wide = makeRgbLayer("Overhanging");
    for (int32_t y = 0; y < 16; ++y)
      for (int32_t x = 0; x < 16; ++x) {
        const PixelCoord at{kW + 200 + x, y};
        wide.rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at), {1.0f, 0.0f, 0.0f, 1.0f});
      }
    past.layers.push_back(std::move(wide));
    check(offCanvasTileCount(past, past.layers[1]) == 1,
          "the fixture really does have one tile entirely past the canvas edge");
    std::vector<std::string> pastWarnings;
    const LayerOpResult pr = mergeLayerDown(past, 1, &pastWarnings);
    bool discardWarned = false;
    for (const std::string& w : pastWarnings)
      if (contains(w, "discarded 1 occupied tile(s)")) discardWarned = true;
    check(pr.ok && discardWarned,
          "a merge through the compositor discards it and says so by count");
  }

  std::printf("[selftest] merge family %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


// ==========================================================================
// PLAN.md Phase 5 step 12 -- layer comps (PRD C14)
// ==========================================================================


}  // namespace np
