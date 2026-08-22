#include "app/selftest/Support.hpp"

namespace np {

// ---------------------------------------------------------------------------
// PLAN.md Phase 5 step 1 -- multiple layers, with reorder, visibility, lock
// and opacity, and the `over` compositing that makes the last two mean
// anything. See app/SelfTest.hpp for the section's own contents list.
// ---------------------------------------------------------------------------
bool runLayerStackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // Most of this section asserts at **exactly zero tolerance**, and that is
  // the interesting part rather than a shortcut. Every fixture value below is
  // chosen to be exactly representable in binary16 (halves and quarters), so
  // the whole premultiplied chain -- store as half, read back as float, scale
  // by an exact opacity, `src + dst * (1 - src.a)` -- is exact: each operand
  // is a small dyadic rational and every product and sum lands back on the
  // float grid with no rounding at all. The hidden-layer, non-overlap and
  // opacity-composition claims are therefore bit-exact comparisons, which is
  // the only strength at which "contributes exactly nothing" means anything.
  //
  // The one lossy stage is `flattenDocumentToLinear()`'s final un-premultiply,
  // a single float division by the composited alpha. IEEE-754 requires it to
  // be correctly rounded, so its error is at most half an ulp: for a result in
  // [0.25, 1) that is 2^-25 = 2.98e-8 absolute. Landed 1.0e-7 -- 3.4x the
  // derived bound, the same "a small multiple of the bound, never of the
  // measurement" discipline runExportTest()'s kRoundTripTol16 and
  // runNpaintFormatTest()'s kCompositeTol both follow.
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  // Same fixture helper every other section uses: a document holds
  // *premultiplied* halves, so a fixture writes straight values through the
  // same `rgb *= a` io/ImageIO.cpp performs on import.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto addRgbLayer = [](Document& doc, std::string name) {
    Layer l;
    l.kind = LayerKind::RGB;
    l.rgbTiles.emplace();
    l.name = std::move(name);
    doc.layers.push_back(std::move(l));
  };

  // --- `over` against a hand-computed reference --------------------------
  //
  // Two translucent layers over the same pixel, both at alpha 0.5, both fully
  // saturated in one channel. The arithmetic, in full, in premultiplied
  // linear light:
  //
  //   bottom straight (1, 0, 0, 0.5) -> premultiplied (0.5, 0, 0,   0.5)
  //   top    straight (0, 0, 1, 0.5) -> premultiplied (0,   0, 0.5, 0.5)
  //
  //   acc  = bottom over transparent black
  //        = (0.5, 0, 0, 0.5) + (0,0,0,0) * (1 - 0.5)
  //        = (0.5, 0, 0, 0.5)
  //   out  = top over acc
  //        = (0, 0, 0.5, 0.5) + (0.5, 0, 0, 0.5) * (1 - 0.5)
  //        = (0 + 0.25, 0, 0.5 + 0, 0.5 + 0.25)
  //        = (0.25, 0, 0.5, 0.75)
  //
  //   straight = out.rgb / out.a = (0.25/0.75, 0, 0.5/0.75, 0.75)
  //            = (1/3, 0, 2/3, 0.75)
  //
  // Every intermediate is a dyadic rational and exact in both half and float;
  // only the two divisions at the end round. Note what the answer is *not*: a
  // plain sum would give premultiplied (0.5, 0, 0.5, 1.0), i.e. straight
  // (0.5, 0, 0.5, 1.0) -- a fully opaque even purple. The two answers differ
  // in every channel, so this fixture cannot pass against the code it
  // replaced.
  {
    Document doc = Document::createBlank(1, 1, WorkingSpace{});
    doc.layers[0].name = "bottom";
    addRgbLayer(doc, "top");
    writeStraight(doc, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(doc, 1, 0, 0, 0.0f, 0.0f, 1.0f, 0.5f);

    const DecodedImage flat = flattenDocumentToLinear(doc);
    const auto got = pixelOf(flat, 0, 0);
    check(near(got[0], 1.0f / 3.0f, kUnpremultiplyTol) && near(got[1], 0.0f, 0.0f) &&
              near(got[2], 2.0f / 3.0f, kUnpremultiplyTol) &&
              near(got[3], 0.75f, 0.0f),
          "over: two 50%-alpha layers composite to the hand-computed (1/3, 0, 2/3, 0.75)");
    check(!near(got[3], 1.0f, 1e-3f),
          "over: and NOT to the plain sum's fully opaque alpha 1.0 -- the fixture would pass "
          "against the summing flattener if it did");

    // The primitive on its own, at the same numbers, so a failure says whether
    // the arithmetic or the document walk is wrong.
    const std::array<float, 4> prim =
        compositeOver({0.0f, 0.0f, 0.5f, 0.5f}, {0.5f, 0.0f, 0.0f, 0.5f});
    check(prim[0] == 0.25f && prim[1] == 0.0f && prim[2] == 0.5f && prim[3] == 0.75f,
          "over: compositeOver() alone gives premultiplied (0.25, 0, 0.5, 0.75), exactly");

    // Reorder, and the result must flip in the direction the ordering
    // convention predicts: `layers` is bottom-to-top, so moving index 0 to
    // index 1 puts red on top, and red must now dominate.
    const LayerOpResult moved = moveLayer(doc, 0, 1);
    check(moved.ok, "reorder: moveLayer(0 -> 1) succeeds");
    const DecodedImage swapped = flattenDocumentToLinear(doc);
    const auto after = pixelOf(swapped, 0, 0);
    check(near(after[0], 2.0f / 3.0f, kUnpremultiplyTol) &&
              near(after[2], 1.0f / 3.0f, kUnpremultiplyTol),
          "reorder: swapping the two layers swaps which colour dominates -- red 2/3 blue 1/3, "
          "the mirror of before, because layers[] is bottom-to-top");
    check(near(after[3], 0.75f, 0.0f),
          "reorder: and alpha is unchanged at 0.75 -- `over` is order-dependent in colour, "
          "not in coverage");
  }

  // --- Opacity is a multiplier, and provably not the same thing as alpha --
  {
    // Both fixtures use only exactly-representable values, so these are
    // zero-tolerance comparisons.
    Document byOpacity = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(byOpacity, 0, 0, 0, 0.5f, 0.25f, 0.75f, 1.0f);
    byOpacity.layers[0].opacity = 0.5f;

    Document byAlpha = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(byAlpha, 0, 0, 0, 0.5f, 0.25f, 0.75f, 0.5f);

    const auto o = pixelOf(flattenDocumentToLinear(byOpacity), 0, 0);
    const auto a = pixelOf(flattenDocumentToLinear(byAlpha), 0, 0);
    check(o[0] == a[0] && o[1] == a[1] && o[2] == a[2] && o[3] == a[3],
          "opacity: a fully opaque layer at 50% opacity composites bit-identically to the "
          "same colour stored at alpha 0.5");
    check(o[3] == 0.5f, "opacity: and the composited alpha is exactly 0.5");

    // ...and yet it is not alpha, because the two compose rather than one
    // overriding the other.
    Document both = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(both, 0, 0, 0, 0.5f, 0.25f, 0.75f, 0.5f);
    both.layers[0].opacity = 0.5f;
    const auto b = pixelOf(flattenDocumentToLinear(both), 0, 0);
    check(b[3] == 0.25f,
          "opacity: alpha 0.5 at 50% opacity composites to alpha exactly 0.25 -- the two "
          "multiply, so opacity is a coverage scale and not an alpha replacement");
    check(b[0] == 0.5f && b[1] == 0.25f && b[2] == 0.75f,
          "opacity: and the straight colour is untouched by it -- scaling a premultiplied "
          "texel scales coverage, not hue");

    // Nothing was mutated to achieve any of that.
    const Tile* t = both.layers[0].rgbTiles->find(TileCoord{0, 0});
    const std::array<float, 4> stored =
        t ? t->readPixel(PixelCoord{0, 0}) : std::array<float, 4>{0, 0, 0, 0};
    check(stored[3] == 0.5f && both.layers[0].opacity == 0.5f,
          "opacity: the stored texel alpha and Layer::opacity are both unchanged after "
          "compositing -- the multiplier is applied to a copy, never baked into the tile");

    // The scalar itself, including the clamp and the NaN guard, since
    // Layer::opacity is a public member of a plain aggregate.
    Layer probe;
    probe.opacity = 0.25f;
    check(layerCoverage(probe) == 0.25f, "opacity: layerCoverage() passes an in-range value "
                                          "through unchanged");
    probe.opacity = 1.5f;
    check(layerCoverage(probe) == 1.0f, "opacity: layerCoverage() clamps above 1 rather than "
                                         "letting `1 - a` go negative");
    probe.opacity = -0.5f;
    check(layerCoverage(probe) == 0.0f, "opacity: layerCoverage() clamps below 0");
    probe.opacity = std::nanf("");
    check(layerCoverage(probe) == 0.0f,
          "opacity: a NaN opacity yields 0 rather than propagating across the canvas");
  }

  // --- A hidden layer contributes EXACTLY nothing (zero tolerance) -------
  {
    // The comparison is against the same document with the layer *deleted*,
    // not against a hand-written expectation: "hidden" has to mean "as if it
    // were not there", and only removing it actually proves that.
    auto build = [&]() {
      Document doc = Document::createBlank(200, 200, WorkingSpace{});
      doc.layers[0].name = "keep";
      writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.75f, 1.0f);
      writeStraight(doc, 0, 150, 150, 0.25f, 0.5f, 0.5f, 0.5f);
      addRgbLayer(doc, "hide me");
      writeStraight(doc, 1, 3, 3, 0.75f, 0.5f, 0.25f, 1.0f);   // exactly overlapping
      writeStraight(doc, 1, 4, 3, 1.0f, 1.0f, 1.0f, 1.0f);     // and one of its own
      writeStraight(doc, 1, 150, 150, 0.5f, 0.5f, 0.5f, 0.5f);
      return doc;
    };

    Document hidden = build();
    hidden.layers[1].visible = false;
    Document deleted = build();
    deleted.layers.pop_back();

    const DecodedImage h = flattenDocumentToLinear(hidden);
    const DecodedImage d = flattenDocumentToLinear(deleted);
    check(h.pixels.size() == d.pixels.size() && !h.pixels.empty() &&
              std::memcmp(h.pixels.data(), d.pixels.data(),
                          h.pixels.size() * sizeof(float)) == 0,
          "hidden: a hidden layer's composite is BYTE-identical to the same document with "
          "that layer deleted -- zero tolerance, over 200x200x4 floats");

    // Zero opacity is the same claim by the other route.
    Document zeroOpacity = build();
    zeroOpacity.layers[1].opacity = 0.0f;
    const DecodedImage z = flattenDocumentToLinear(zeroOpacity);
    check(z.pixels.size() == d.pixels.size() &&
              std::memcmp(z.pixels.data(), d.pixels.data(),
                          z.pixels.size() * sizeof(float)) == 0,
          "hidden: a layer at 0.0 opacity is byte-identical to the same deletion too");

    // The negative control: the same fixture *visible* must differ, or the two
    // assertions above would pass against a flattener that ignored layer 1
    // entirely.
    const DecodedImage shown = flattenDocumentToLinear(build());
    check(shown.pixels.size() == d.pixels.size() &&
              std::memcmp(shown.pixels.data(), d.pixels.data(),
                          shown.pixels.size() * sizeof(float)) != 0,
          "hidden: and the same layer *visible* genuinely changes the composite -- the two "
          "checks above cannot pass vacuously");
  }

  // --- The regression property: non-overlapping documents are unchanged --
  //
  // PLAN.md's standard verification is "the selftest output changes by
  // additions only", and this step legitimately breaks that for documents
  // whose layers overlap. What must NOT change is everything else, and this
  // block asserts the boundary rather than leaving it to a diff: against a
  // second, independent implementation of the plain sum this step replaced --
  // written here rather than called, exactly like the recovery journal
  // section's own second FNV-1a -- a single-layer document and a multi-layer
  // document with no two layers covering the same pixel must produce
  // bit-identical output.
  {
    auto plainSumFlatten = [](const Document& doc) {
      const size_t w = static_cast<size_t>(doc.width);
      const size_t h = static_cast<size_t>(doc.height);
      std::vector<float> premul(w * h * 4, 0.0f);
      for (const Layer& layer : doc.layers) {
        if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;
        for (const auto& [coord, tile] : *layer.rgbTiles) {
          const PixelCoord origin = tileOrigin(coord);
          for (int32_t ty = 0; ty < kTileSize; ++ty) {
            const int32_t dy = origin.y + ty;
            if (dy < 0 || dy >= doc.height) continue;
            for (int32_t tx = 0; tx < kTileSize; ++tx) {
              const int32_t dx = origin.x + tx;
              if (dx < 0 || dx >= doc.width) continue;
              const std::array<float, 4> px = tile.readPixel(PixelCoord{tx, ty});
              float* dst = &premul[(static_cast<size_t>(dy) * w + static_cast<size_t>(dx)) * 4];
              dst[0] += px[0];
              dst[1] += px[1];
              dst[2] += px[2];
              dst[3] += px[3];
            }
          }
        }
      }
      std::vector<float> straight(premul.size(), 0.0f);
      for (size_t i = 0; i < premul.size(); i += 4) {
        const float a = premul[i + 3];
        if (a <= 0.0f) continue;
        straight[i + 0] = premul[i + 0] / a;
        straight[i + 1] = premul[i + 1] / a;
        straight[i + 2] = premul[i + 2] / a;
        straight[i + 3] = a;
      }
      return straight;
    };
    auto identical = [](const std::vector<float>& a, const DecodedImage& b) {
      return a.size() == b.pixels.size() && !a.empty() &&
             std::memcmp(a.data(), b.pixels.data(), a.size() * sizeof(float)) == 0;
    };

    // Deliberately awkward: translucent texels (so the un-premultiply really
    // runs), several tiles, content off the canvas edge, and values that are
    // NOT exactly representable in half, so a difference of one ulp anywhere
    // would show up.
    Document single = Document::createBlank(300, 200, WorkingSpace{});
    writeStraight(single, 0, 0, 0, 0.1f, 0.2f, 0.3f, 1.0f);
    writeStraight(single, 0, 7, 9, 0.8f, 0.4f, 0.2f, 0.3f);
    writeStraight(single, 0, 199, 150, 0.37f, 0.61f, 0.94f, 0.77f);
    writeStraight(single, 0, 290, 195, 1.0f, 1.0f, 1.0f, 1.0f);
    writeStraight(single, 0, 400, 400, 0.5f, 0.5f, 0.5f, 1.0f);  // off canvas
    check(identical(plainSumFlatten(single), flattenDocumentToLinear(single)),
          "regression: a SINGLE-layer document composites bit-identically to the plain sum "
          "this step replaced -- every float, including the translucent ones");

    Document disjoint = single;
    addRgbLayer(disjoint, "second");
    addRgbLayer(disjoint, "third");
    writeStraight(disjoint, 1, 1, 0, 0.55f, 0.05f, 0.95f, 0.42f);
    writeStraight(disjoint, 1, 8, 9, 0.13f, 0.79f, 0.31f, 1.0f);
    writeStraight(disjoint, 1, 250, 180, 0.9f, 0.1f, 0.6f, 0.66f);
    writeStraight(disjoint, 2, 2, 0, 0.22f, 0.44f, 0.88f, 0.9f);
    writeStraight(disjoint, 2, 128, 128, 0.71f, 0.29f, 0.07f, 1.0f);
    check(identical(plainSumFlatten(disjoint), flattenDocumentToLinear(disjoint)),
          "regression: and so does a THREE-layer document whose layers never cover the same "
          "pixel -- `over` reduces to the sum exactly when nothing overlaps");

    // The negative control again: one overlapping texel and the two must part
    // company, or the two checks above would be asserting that the compositor
    // is the summer.
    Document overlapping = disjoint;
    writeStraight(overlapping, 1, 0, 0, 0.6f, 0.6f, 0.6f, 0.5f);  // onto layer 0's (0,0)
    check(!identical(plainSumFlatten(overlapping), flattenDocumentToLinear(overlapping)),
          "regression: one overlapping texel is enough to make the two disagree -- the "
          "identity above is a property of non-overlap, not of the implementation");
  }

  // --- Layer operations, and what each does to the dirty state -----------
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "ops");
    check(!od.isDirty() && od.revision == 0 && od.structuralRevision == 0,
          "ops: a blank document starts clean, at revision 0");

    const DocumentOpResult added =
        recordLayerEdit(od, addLayer(od.document, 1, makeRgbLayer("Layer 2")));
    check(added.ok && od.document.layers.size() == 2 && od.document.layers[1].name == "Layer 2",
          "ops: addLayer inserts at the requested index");
    check(od.isDirty() && od.revision == 1 && od.structuralRevision == 1,
          "ops: and bumps BOTH revision and structuralRevision by exactly one -- a layer "
          "change is structural, so PRD O5's journal writes at once rather than on the timer");
    check(od.unsavedEdits.size() == 1 && contains(od.unsavedEdits[0], "Layer 2"),
          "ops: the recorded edit label names the layer, for PRD I11's refusal and the "
          "future History panel");

    const DocumentOpResult dup = recordLayerEdit(od, duplicateLayer(od.document, 0));
    check(dup.ok && od.document.layers.size() == 3,
          "ops: duplicateLayer inserts the copy directly above its source");
    check(od.revision == 2 && od.structuralRevision == 2, "ops: and is structural too");

    const size_t before = od.document.layers.size();
    const DocumentOpResult moved = recordLayerEdit(od, moveLayer(od.document, 0, 2));
    check(moved.ok && od.document.layers.size() == before,
          "ops: moveLayer keeps the layer count");
    check(od.revision == 3 && od.structuralRevision == 3, "ops: and is structural too");

    const DocumentOpResult removed = recordLayerEdit(od, removeLayer(od.document, 2));
    check(removed.ok && od.document.layers.size() == before - 1, "ops: removeLayer removes one");
    check(od.revision == 4 && od.structuralRevision == 4, "ops: and is structural too");

    const DocumentOpResult vis = recordLayerEdit(od, setLayerVisible(od.document, 0, false));
    check(vis.ok && !od.document.layers[0].visible && od.revision == 5 &&
              od.structuralRevision == 5,
          "ops: a property change (visibility) is structural as well -- it changes what the "
          "composite is, and the journal must not learn about it a minute later");

    // A refused operation must leave the record completely alone.
    const uint64_t rev = od.revision;
    const size_t edits = od.unsavedEdits.size();
    const DocumentOpResult bad = recordLayerEdit(od, removeLayer(od.document, 99));
    check(!bad.ok && contains(bad.error, "index 99"),
          "ops: an out-of-range index is refused by name, naming the index");
    check(od.revision == rev && od.structuralRevision == rev &&
              od.unsavedEdits.size() == edits,
          "ops: and a refused operation records NOTHING -- no revision bump, no label. A "
          "document is not dirty because someone tried something that did not happen");

    const DocumentOpResult badOpacity =
        recordLayerEdit(od, setLayerOpacity(od.document, 0, 1.5f));
    check(!badOpacity.ok && contains(badOpacity.error, "1.5") &&
              contains(badOpacity.error, "[0, 1]"),
          "ops: an out-of-range opacity is refused by name rather than clamped -- clamping "
          "would only move io/NpaintFile's own refusal to the next save");

    // Removing the last layer is allowed (PRD C16: no privileged background).
    Document lastOne = Document::createBlank(8, 8, WorkingSpace{});
    check(removeLayer(lastOne, 0).ok && lastOne.layers.empty(),
          "ops: removing the only layer is allowed -- PRD C16 rules out a special locked "
          "Background, and a zero-layer document is representable");

    // Duplication is a real deep copy, the same claim duplicateDocument makes.
    Document deep = Document::createBlank(64, 64, WorkingSpace{});
    writeStraight(deep, 0, 1, 1, 0.5f, 0.5f, 0.5f, 1.0f);
    check(duplicateLayer(deep, 0).ok && deep.layers.size() == 2,
          "ops: duplicateLayer succeeds on a layer with tiles");
    writeStraight(deep, 1, 1, 1, 0.25f, 0.25f, 0.25f, 1.0f);
    const Tile* src = deep.layers[0].rgbTiles->find(TileCoord{0, 0});
    check(src != nullptr && src->readPixel(PixelCoord{1, 1})[0] == 0.5f,
          "ops: painting into the copy does not reach the source -- the TileStore really was "
          "deep-copied, at 128 KiB per tile (COW is Phase 5 step 6)");

    check(defaultNewLayerName(Document::createBlank(8, 8, WorkingSpace{})) == "Layer 1",
          "ops: defaultNewLayerName() starts at \"Layer 1\"");
    Document named = Document::createBlank(8, 8, WorkingSpace{});
    named.layers[0].name = "Layer 7";
    addRgbLayer(named, "Layer 3");
    check(defaultNewLayerName(named) == "Layer 8",
          "ops: and goes one above the highest existing \"Layer N\", not one above the count "
          "-- so deleting from the middle cannot hand out a name already on screen");
  }

  // --- `locked`, refusing exactly what it should, by name ----------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    doc.layers[0].name = "Line pass";
    addRgbLayer(doc, "Free");
    doc.layers[0].locked = true;

    const LayerOpResult rm = removeLayer(doc, 0);
    check(!rm.ok && contains(rm.error, "locked") && contains(rm.error, "Line pass") &&
              doc.layers.size() == 2,
          "locked: removeLayer is refused, naming the layer and the lock");
    const LayerOpResult mv = moveLayer(doc, 0, 1);
    check(!mv.ok && contains(mv.error, "locked") && doc.layers[0].name == "Line pass",
          "locked: moving the locked layer itself is refused");
    const LayerOpResult op = setLayerOpacity(doc, 0, 0.5f);
    check(!op.ok && contains(op.error, "locked") && doc.layers[0].opacity == 1.0f,
          "locked: setLayerOpacity is refused, and the value really is unchanged");
    const LayerOpResult rn = setLayerName(doc, 0, "nope");
    check(!rn.ok && contains(rn.error, "locked") && doc.layers[0].name == "Line pass",
          "locked: setLayerName is refused");

    const LayerOpResult hide = setLayerVisible(doc, 0, false);
    check(hide.ok && !doc.layers[0].visible,
          "locked: hiding a locked layer is ALLOWED -- it changes nothing about the layer, "
          "and every editor with a lock agrees the eye icon stays live");
    check(setLayerVisible(doc, 0, true).ok, "locked: and showing it again is allowed");

    const LayerOpResult dupLocked = duplicateLayer(doc, 0);
    check(dupLocked.ok && doc.layers.size() == 3 && doc.layers[1].locked,
          "locked: duplicating a locked layer is allowed (it reads the source) and the copy "
          "inherits the lock -- duplication is not a one-step way to launder it off");

    // Moving a *different* layer past a locked one is allowed: locking one
    // layer must not freeze the whole stack.
    const size_t topIndex = doc.layers.size() - 1;
    check(!doc.layers[topIndex].locked && moveLayer(doc, topIndex, 0).ok &&
              doc.layers[0].name == "Free",
          "locked: an unlocked layer may still be moved past a locked one -- a lock freezes "
          "one layer, not the document");

    // And the lock itself always comes off.
    const size_t lockedNow = 1;
    check(doc.layers[lockedNow].locked && setLayerLocked(doc, lockedNow, false).ok &&
              !doc.layers[lockedNow].locked,
          "locked: setLayerLocked(false) is allowed on a locked layer -- a lock that cannot "
          "be removed is a bug, not a lock");
    check(removeLayer(doc, lockedNow).ok,
          "locked: and once unlocked, the operation that was refused succeeds");
  }

  // --- An unimplemented blend: composited as `over`, never silently ------
  {
    // **Changed by PLAN.md Phase 5 step 2**, which is the step that made the
    // old wording false: these two used to read "\"normal\" is the one blend
    // this build implements" and "everything else ... is reported
    // unimplemented", and core/Blend has since landed five more. What this
    // section is actually for survives unchanged -- a blend this build cannot
    // composite is composited as `over` and reported, never silently -- so the
    // fixture below moved to a name that is still outside the set. The full
    // enumeration is asserted in runBlendTest(); these two only pin the
    // boundary this section's fixtures sit on.
    check(blendIsImplemented(kDefaultBlendName) && blendIsImplemented("multiply"),
          "blend: \"normal\" and the rest of core/Blend's linear set are implemented");
    // `mix` left this list at PLAN.md Phase 5 step 3, which implemented it at
    // the layer level, so what remains outside the set is what was always the
    // real case: a name from a newer build, and a blank one.
    check(blendIsImplemented("mix") && !blendIsImplemented("linear-burn") &&
              !blendIsImplemented(""),
          "blend: a newer build's name and an empty string are reported unimplemented "
          "rather than assumed to be `over`");

    Document odd = Document::createBlank(1, 1, WorkingSpace{});
    odd.layers[0].name = "Line pass";
    addRgbLayer(odd, "");
    odd.layers[1].blend = "linear-burn";
    writeStraight(odd, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(odd, 1, 0, 0, 0.0f, 0.0f, 1.0f, 0.5f);

    std::vector<std::string> warnings;
    const DecodedImage flat = flattenDocumentToLinear(odd, &warnings);
    check(warnings.size() == 1 && contains(warnings[0], "linear-burn") &&
              contains(warnings[0], "layer 1"),
          "blend: an unimplemented blend produces exactly one warning, naming the layer and "
          "the blend it asked for");
    check(contains(warnings[0], "core/Blend"),
          "blend: and points at where the real implementation is coming from");

    // The pixels really are `over`, not something else: identical to the same
    // document with the blend set to normal.
    Document asNormal = odd;
    asNormal.layers[1].blend = kDefaultBlendName;
    const DecodedImage normalFlat = flattenDocumentToLinear(asNormal);
    check(flat.pixels.size() == normalFlat.pixels.size() &&
              std::memcmp(flat.pixels.data(), normalFlat.pixels.data(),
                          flat.pixels.size() * sizeof(float)) == 0,
          "blend: and the composite is byte-identical to `over` -- the warning describes what "
          "actually happened rather than hinting at something else");
    check(odd.layers[1].blend == "linear-burn",
          "blend: while the value itself is untouched, so PRD I10 still carries it to disk");

    // A hidden layer with an unimplemented blend is still warned about: the
    // document is approximate whether or not this particular layer mattered
    // today.
    Document hiddenOdd = odd;
    hiddenOdd.layers[1].visible = false;
    std::vector<std::string> hiddenWarnings;
    flattenDocumentToLinear(hiddenOdd, &hiddenWarnings);
    check(hiddenWarnings.size() == 1,
          "blend: a HIDDEN layer's unimplemented blend is still reported -- unhiding it must "
          "not be where the user first learns the composite is approximate");

    // The export boundary carries it too, on success and on refusal alike.
    const ExportResult png = exportDocument(odd, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(png.ok && png.warnings.size() == 1 && contains(png.warnings[0], "linear-burn"),
          "blend: exportDocument() carries the warning out with the bytes");
    const ExportResult refused = exportDocument(odd, ImageFormat::Jpeg,
                                                ExportTargetSpace::Rec709Srgb,
                                                ExportBitDepth::UInt8);
    check(!refused.ok && refused.warnings.size() == 1,
          "blend: and carries it out with a REFUSAL too -- fixing the refusal must not be "
          "how the approximation gets discovered");
  }

  // --- The probe agrees with the flattener -------------------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    addRgbLayer(doc, "top");
    writeStraight(doc, 0, 1, 1, 1.0f, 0.0f, 0.0f, 0.5f);
    writeStraight(doc, 1, 1, 1, 0.0f, 0.0f, 1.0f, 0.5f);

    ProbeParams all;
    all.sampleAllLayers = true;
    const ProbeSample sample = probePixel(doc, PixelCoord{1, 1}, all);
    const auto flat = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(near(sample.linear[0], flat[0], kUnpremultiplyTol) &&
              near(sample.linear[2], flat[2], kUnpremultiplyTol) &&
              near(sample.linear[3], flat[3], kUnpremultiplyTol),
          "probe: sampleAllLayers composites through the SAME `over` the flattener uses -- an "
          "eyedropper and an export that disagreed would be a bug nobody could explain");

    doc.layers[1].visible = false;
    const ProbeSample hiddenSample = probePixel(doc, PixelCoord{1, 1}, all);
    check(near(hiddenSample.linear[0], 1.0f, kUnpremultiplyTol) &&
              near(hiddenSample.linear[3], 0.5f, 0.0f),
          "probe: and honours `visible` -- with the blue layer hidden it reads the red one "
          "underneath");

    ProbeParams justTop;
    justTop.sampleAllLayers = false;
    justTop.activeLayerIndex = 1;
    const ProbeSample own = probePixel(doc, PixelCoord{1, 1}, justTop);
    check(near(own.linear[2], 1.0f, kUnpremultiplyTol) && near(own.linear[3], 0.5f, 0.0f),
          "probe: single-layer mode reads a HIDDEN layer's own colour, deliberately ignoring "
          "visible/opacity -- it asks what is on the layer, not what the document shows");
  }

  // --- The panel's one reversal, and what a row says ---------------------
  {
    check(layerIndexForPanelRow(0, 3) == 2 && layerIndexForPanelRow(1, 3) == 1 &&
              layerIndexForPanelRow(2, 3) == 0,
          "panel: row 0 is the TOP of the stack (the last element of layers[]), which is what "
          "every layers panel shows first");
    check(panelRowForLayerIndex(2, 3) == 0 && panelRowForLayerIndex(0, 3) == 2,
          "panel: panelRowForLayerIndex() is its exact inverse");
    check(layerIndexForPanelRow(5, 3) == 0 && layerIndexForPanelRow(0, 0) == 0 &&
              panelRowForLayerIndex(9, 3) == 0,
          "panel: an out-of-range row or index returns 0 rather than wrapping through "
          "unsigned subtraction -- the one arithmetic accident this mapping exists to avoid");

    // Drag-to-reorder's target math. Dropped in the lower half of a row, the
    // dragged layer lands AT that row's own model index (`to == hoveredIndex`
    // is the "insert below" case core::moveLayer()'s rotate already lands
    // correctly with no +1). Dropped in the upper half, it lands one past it
    // -- "above" in panel terms is `+1` in model terms, the same reversal
    // layerIndexForPanelRow() owns, restated here for the drop math instead
    // of the row math.
    check(layerDropTargetIndex(1, false, 5) == 1,
          "drop: lower half of row 1 lands the dragged layer AT index 1");
    check(layerDropTargetIndex(1, true, 5) == 2,
          "drop: upper half of row 1 lands the dragged layer one past it, at index 2");
    check(layerDropTargetIndex(4, true, 5) == 4,
          "drop: the upper half of the topmost row (index 4 of 5) saturates at 4, never 5 -- "
          "there is no slot past the top of the stack");
    check(layerDropTargetIndex(0, false, 5) == 0,
          "drop: the lower half of the bottom row lands at index 0, the bottom itself");
    check(layerDropTargetIndex(0, true, 1) == 0,
          "drop: a single-layer document has nowhere else for a drop to land");

    Layer row;
    row.kind = LayerKind::RGB;
    check(std::string(layerRowSubLine(row)) == "RGB \xC2\xB7 NORMAL \xC2\xB7 100%",
          "panel: the sub-line reads `RGB - NORMAL - 100%`, docs/ui.md's own row format");
    row.opacity = 0.72f;
    row.visible = false;
    row.locked = true;
    check(std::string(layerRowSubLine(row)) ==
              "RGB \xC2\xB7 NORMAL \xC2\xB7 72% \xC2\xB7 HIDDEN \xC2\xB7 LOCKED",
          "panel: hidden and locked are spelled out on the sub-line, so --selftest can read "
          "state the eye and lock glyphs otherwise only show");
    row.blend = "linear-burn";
    check(contains(layerRowSubLine(row), "LINEAR-BURN (!)"),
          "panel: an unrecognised blend shows as itself, marked (!) -- the panel's half of "
          "\"never silently composited as over\"");

    Layer unnamed;
    check(layerRowTitle(unnamed, 0) == "Layer 1" && layerRowTitle(unnamed, 4) == "Layer 5",
          "panel: an unnamed layer gets a positional placeholder title, never a blank row");
    unnamed.name = "Line pass";
    check(layerRowTitle(unnamed, 4) == "Line pass",
          "panel: a named layer shows its own name");
    check(std::string(layerKindGlyph(LayerKind::RGB)) != std::string(layerKindGlyph(
              LayerKind::Pigment)),
          "panel: every kind's glyph is distinct, per docs/ui.md 3.2 (the wireframe's rows "
          "could not tell a Pigment layer from an RGB one, which hid the differentiator)");
  }

  // --- The round trip: order and all six metadata fields -----------------
  {
    const char* kPath = "selftest_layerstack.npaint";
    std::remove(kPath);

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0].name = "bottom";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.25f;
    doc.layers[0].visible = false;
    doc.layers[0].locked = true;
    doc.layers[0].parent = "G0001";
    addRgbLayer(doc, "middle");
    doc.layers[1].opacity = 0.5f;
    // The second locked layer is the middle one, not the top one, and that is
    // forced rather than arbitrary: the top layer is about to be moved, and
    // core/LayerOps refuses to move a locked layer. Two layers still carry
    // `np:locked = 1` and one carries 0, which is all the round trip needs --
    // and the reorder additionally exercises the rule that an unlocked layer
    // may still travel *past* a locked one.
    doc.layers[1].locked = true;
    addRgbLayer(doc, "top");
    doc.layers[2].blend = "screen";
    // One identifiable texel per layer, in a different tile each, so the order
    // is checkable from the pixels and not only from the names.
    writeStraight(doc, 0, 1, 1, 1.0f, 0.0f, 0.0f, 1.0f);
    writeStraight(doc, 1, 130, 1, 0.0f, 1.0f, 0.0f, 1.0f);
    writeStraight(doc, 2, 1, 130, 0.0f, 0.0f, 1.0f, 1.0f);

    // Reorder before saving, so the file records an order that is NOT the one
    // the layers were created in -- a round trip that only ever saw creation
    // order could not catch a reversal.
    check(moveLayer(doc, 2, 0).ok, "round trip: the top layer is moved to the bottom first");
    check(doc.layers[0].name == "top" && doc.layers[1].name == "bottom" &&
              doc.layers[2].name == "middle",
          "round trip: and the in-memory order really is top/bottom/middle before saving");

    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok, "round trip: the reordered three-layer document saves");
    if (saved.ok) {
      const NpaintLoadResult back = loadNpaint(kPath);
      check(back.ok && back.document.layers.size() == 3,
            "round trip: and reads back with three layers");
      if (back.ok && back.document.layers.size() == 3) {
        const Layer& b0 = back.document.layers[0];
        const Layer& b1 = back.document.layers[1];
        const Layer& b2 = back.document.layers[2];
        check(b0.name == "top" && b1.name == "bottom" && b2.name == "middle",
              "round trip: in exactly the saved order, bottom-first -- the part order IS the "
              "layer order (docs/document-format.md)");
        check(b0.blend == "screen" && b1.blend == "multiply" && b2.blend == kDefaultBlendName,
              "round trip: np:blend travels with its own layer, not with its old index");
        check(b0.opacity == 1.0f && b1.opacity == 0.25f && b2.opacity == 0.5f,
              "round trip: np:opacity likewise, at exact float equality");
        check(b0.visible == true && b1.visible == false && b2.visible == true,
              "round trip: np:visible likewise");
        check(b0.locked == false && b1.locked == true && b2.locked == true,
              "round trip: np:locked likewise");
        check(b1.parent == "G0001" && b0.parent.empty() && b2.parent.empty(),
              "round trip: np:parent likewise -- still carried, still never acted on");
        check(b0.kind == LayerKind::RGB && b1.kind == LayerKind::RGB &&
                  b2.kind == LayerKind::RGB,
              "round trip: np:kind likewise");

        // And the pixels moved with the metadata: blue was the top layer and
        // is now the bottom one, so its texel must be in layers[0].
        const Tile* blue = b0.rgbTiles->find(TileCoord{0, 1});
        check(blue != nullptr && blue->readPixel(PixelCoord{1, 2})[2] == 1.0f,
              "round trip: and each layer's TILES came back with it -- the reordered bottom "
              "layer holds the blue texel it had before the save");

        // The composite the file carries must agree with compositing the
        // loaded document, which is only true if the order survived.
        const DecodedImage expect = flattenDocumentToLinear(back.document);
        const DecodedImage direct = flattenDocumentToLinear(doc);
        check(expect.valid() && direct.valid() &&
                  std::memcmp(expect.pixels.data(), direct.pixels.data(),
                              expect.pixels.size() * sizeof(float)) == 0,
              "round trip: the loaded document composites BYTE-identically to the one that "
              "was saved -- order, visibility and opacity all survived together");
      }
    }
    std::remove(kPath);
    std::FILE* left = std::fopen(kPath, "rb");
    check(left == nullptr, "round trip: the scratch file is removed");
    if (left) std::fclose(left);
  }

  std::printf("[selftest] layer stack %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
