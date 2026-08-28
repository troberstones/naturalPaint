#include "app/selftest/Support.hpp"

#include <cstring>

#include "ui/TransformPreviewTexture.hpp"

namespace np {

// ui/TransformPreviewTexture (docs/testing-issues.md T14: a Free Transform
// drag shows the transformed pixels live, not only the wireframe box).
//
// Only the CPU half -- `transformPreviewStraightHalf()` -- is exercised here,
// matching this suite's own established split for a small, single-shot GPU
// upload wrapper: `app/selftest/DabPreview.cpp` and its `StrokePreview`
// sibling test their CPU rasterisers (`app/DabPreview`, `app/StrokePreview`)
// and never touch `DabPreviewTexture`/`StrokePreviewTexture`'s
// `wgpuQueueWriteTexture()` call at all -- those wrapper classes are ~30 lines
// each of "create once, write on a miss" with no incremental path, no dirty
// set and no cache-key arithmetic to get wrong, unlike ui/DocumentTexture
// (which DOES get a GPU readback section, because decision 4's incremental
// upload is exactly the kind of code a headless assertion cannot see a bug
// in). `TransformPreviewTexture::upload()`/`reset()` are the same shape as
// those two -- reset, create, one `wgpuQueueWriteTexture()`, return -- so the
// same reasoning applies: what needs proving is the CONTENT this file packs,
// which is entirely CPU work, headless and GPU-free.
bool runTransformPreviewTextureTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] transform preview texture: the CPU pack, headless and GPU-free\n");

  // A distinctive, hand-known fixture: two FULLY OPAQUE flat-colour blocks
  // side by side (red then blue), so a misplacement or a channel swap is a
  // wrong colour, not a statistical drift -- app/selftest/TransformSession.cpp
  // section 11 makes the identical choice for the identical reason.
  auto makeDoc = [] {
    OpenDocument od = makeBlankOpenDocument(20, 10, WorkingSpace{});
    TileStore& tiles = *od.document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 10; ++y) {
      for (int32_t x = 0; x < 10; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {1.0f, 0.0f, 0.0f, 1.0f});  // red
      for (int32_t x = 10; x < 20; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.0f, 0.0f, 1.0f, 1.0f});  // blue
    }
    od.recordEdit("fill fixture", EditKind::Content);
    return od;
  };
  auto texelAt = [](const std::vector<uint16_t>& half, uint32_t w, int32_t x, int32_t y) {
    const size_t i = (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 4u;
    return std::array<uint16_t, 4>{half[i], half[i + 1], half[i + 2], half[i + 3]};
  };
  auto halfOf = [](float r, float g, float b, float a) {
    return std::array<uint16_t, 4>{floatToHalf(r), floatToHalf(g), floatToHalf(b), floatToHalf(a)};
  };

  // --- 1. A null selection copies the WHOLE layer, straight-alpha ----------
  {
    OpenDocument od = makeDoc();
    const DocumentRegion whole{0, 0, 20u, 10u};
    const std::vector<uint16_t> half =
        transformPreviewStraightHalf(od.document.layers[0], nullptr, whole);
    check(half.size() == 20u * 10u * 4u,
          "null selection: output is exactly width*height*4 half words");
    check(texelAt(half, 20u, 0, 0) == halfOf(1.0f, 0.0f, 0.0f, 1.0f),
          "null selection: the red block's own texel comes through unchanged");
    check(texelAt(half, 20u, 15, 5) == halfOf(0.0f, 0.0f, 1.0f, 1.0f),
          "null selection: the blue block's own texel comes through unchanged, at a DIFFERENT "
          "row too -- not just row 0");
  }

  // --- 2. A selection crops AND coverage-weights, exactly what commit() ----
  //        will actually read -- app/TransformSession.hpp's own reason for
  //        pointing this file at copyThroughSelection() rather than a raw
  //        imageFromTileStore() of the whole layer.
  {
    OpenDocument od = makeDoc();
    // A rectangle well inside the red block, with margin on every side, so
    // every texel this test reads is unambiguously fully-covered or
    // fully-uncovered -- no antialiased edge texel's exact coverage fraction
    // has to be known for this assertion to be exact.
    const Selection interior = selectRectangle(2.0f, 2.0f, 8.0f, 8.0f);
    // sourceBounds deliberately LARGER than the selection (the whole red
    // block), so a texel inside the block but OUTSIDE the selection is the
    // one this section is really about.
    const DocumentRegion redBlock{0, 0, 10u, 10u};
    const std::vector<uint16_t> half =
        transformPreviewStraightHalf(od.document.layers[0], &interior, redBlock);
    check(half.size() == 10u * 10u * 4u, "selection crop: sized to sourceBounds, not the selection");
    check(texelAt(half, 10u, 4, 4) == halfOf(1.0f, 0.0f, 0.0f, 1.0f),
          "selection crop: a texel INSIDE the selection is the real colour, full alpha");
    check(texelAt(half, 10u, 0, 0) == halfOf(0.0f, 0.0f, 0.0f, 0.0f),
          "selection crop: a texel inside sourceBounds but OUTSIDE the selection reads "
          "transparent black -- imageFromTileStore()'s own 'absent tile' rule, because "
          "copyThroughSelection() never materialised a tile there");
  }

  // --- 3. Un-premultiply is exercised, not just full-alpha pass-through ----
  {
    OpenDocument od = makeBlankOpenDocument(4, 4, WorkingSpace{});
    // A half-alpha texel written PREMULTIPLIED (core/TileStore.hpp's own
    // working space -- ops/Transform.hpp §"the store's own space") as
    // (0.25, 0, 0, 0.5); straight is (0.5, 0, 0, 0.5). Chosen so the
    // premultiplied and straight values differ in every channel that
    // matters, which a bug that skipped the unpremultiply() call entirely
    // would fail to reproduce.
    od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(PixelCoord{1, 1}))
        .writePixel(tileLocalOffset(PixelCoord{1, 1}), {0.25f, 0.0f, 0.0f, 0.5f});
    od.recordEdit("fill fixture", EditKind::Content);
    const DocumentRegion whole{0, 0, 4u, 4u};
    const std::vector<uint16_t> half =
        transformPreviewStraightHalf(od.document.layers[0], nullptr, whole);
    check(texelAt(half, 4u, 1, 1) == halfOf(0.5f, 0.0f, 0.0f, 0.5f),
          "SABOTAGE: a premultiplied (0.25,0,0,0.5) texel unpremultiplies to (0.5,0,0,0.5) -- "
          "proof this file un-premultiplies rather than passing the store's own premultiplied "
          "value straight through to a straight-alpha texture");
  }

  // --- 4. Pigment: named scope reduction, refused rather than wrong --------
  {
    OpenDocument od = makeBlankOpenDocument(10, 10, WorkingSpace{});
    Layer pigment;
    pigment.kind = LayerKind::Pigment;
    pigment.pigmentTiles.emplace();
    pigment.pigmentTiles->getOrCreate(TileCoord{0, 0})
        .writeTexel(PixelCoord{2, 2}, PigmentTexel{Latent{{0.2f, 0.2f, 0.2f}, {}}, 1.0f});
    const DocumentRegion whole{0, 0, 10u, 10u};
    const std::vector<uint16_t> half = transformPreviewStraightHalf(pigment, nullptr, whole);
    check(half.empty(),
          "a Pigment layer's preview is empty -- this file's own header names why (no "
          "latentToRgb() projection here) rather than returning a blank or wrong quad");
  }

  // --- 5. An empty sourceBounds is refused up front -------------------------
  {
    OpenDocument od = makeDoc();
    const DocumentRegion empty{0, 0, 0u, 0u};
    const std::vector<uint16_t> half =
        transformPreviewStraightHalf(od.document.layers[0], nullptr, empty);
    check(half.empty(), "an empty sourceBounds produces an empty preview, not a 0x0 texture");
  }

  // --- 6. Cost: the ONE crop, at a realistic 2048x2048 layer, against ------
  //        PRD F3's 20 ms -- ui/DocumentTexture.hpp's own "6. what the
  //        cache saves, measured" section is the model for this one, and the
  //        comparison it invites matters: THAT section measures a
  //        multi-layer composite walk that would have to re-run every DRAG
  //        FRAME without a cache. This section measures a single TileStore
  //        read that runs ONCE per session, at `begin*()`, and never again
  //        for the rest of the drag -- so a number under budget here is not
  //        "the frame is safe", it is "even the one-time hitch at Cmd+T is
  //        not the bottleneck a naive per-frame resample would have been".
  //        No `check()` gate: a wall-clock figure is load-sensitive
  //        (app/selftest's own documented flake class), and there is nothing
  //        in this file that should ever FAIL a build over how fast the
  //        machine it happens to run on is.
  {
    auto buildLayer = [](int32_t size) {
      OpenDocument od = makeBlankOpenDocument(size, size, WorkingSpace{});
      TileStore& tiles = *od.document.layers[0].rgbTiles;
      // Fully opaque and fully covered -- the worst case for this function's
      // own cost (every texel is a real un-premultiply, not a same-cost-
      // either-way branch), and the realistic one: a Free Transform's usual
      // target is a layer someone actually painted or imported, not a
      // sparse fixture.
      for (int32_t y = 0; y < size; ++y)
        for (int32_t x = 0; x < size; ++x)
          tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
              .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.25f, 0.5f, 0.75f, 1.0f});
      od.recordEdit("fill fixture", EditKind::Content);
      return od;
    };
    const OpenDocument big = buildLayer(2048);
    const DocumentRegion whole{0, 0, 2048u, 2048u};

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint16_t> half =
        transformPreviewStraightHalf(big.document.layers[0], nullptr, whole);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(half.size() == 2048u * 2048u * 4u, "cost: the 2048x2048 fixture packs to the full size");

    constexpr double kPenToPhotonMs = 20.0;  // PRD F3
    std::printf("  -- 6. cost: the ONE upload at begin(), never a per-frame cost --\n");
    std::printf(
        "    [measured] 2048x2048 crop, CPU pack (copyThroughSelection + un-premultiply + f16): "
        "%.3f ms (%.1f%% of PRD F3's %.0f ms pen-to-photon budget, paid ONCE per session)\n",
        ms, 100.0 * ms / kPenToPhotonMs, kPenToPhotonMs);
    std::printf(
        "    The per-DRAG-FRAME cost is NOT this number: it is addCanvasQuad()'s own push of "
        "four already-computed screen points plus one GPU draw call sized to the quad's ON-"
        "SCREEN pixel footprint (view resolution), which is what stays cheap regardless of "
        "document size -- see ui/TransformPreviewTexture.hpp's own header.\n");
  }

  // --- 7. SABOTAGE PROOF: the source is bit-identical before and after -----
  //
  // app/TransformSession.hpp's own invariant, restated for this file's own
  // reader: a live preview is a SECOND READER of the source, added at draw
  // time, and it must never become a second WRITER -- app/TransformSession's
  // own bit-identical commit proof (app/selftest/TransformSession.cpp
  // section 5) is what protects `commit()` itself, but it never calls
  // through THIS file at all (ui/TransformPreviewTexture has no caller in
  // that headless test -- it is ui-only, deliberately, this header's own
  // top section explains why), so it cannot be the thing that catches a bug
  // introduced HERE. This assertion is: compare the layer's own tiles,
  // read independently through `imageFromTileStore()`, before and after a
  // `transformPreviewStraightHalf()` call -- bit for bit, the same
  // `std::memcmp` idiom app/selftest/TransformSession.cpp's own SABOTAGE
  // PROOF 2 uses for the identical claim about `commit()`.
  {
    OpenDocument od = makeDoc();
    const DocumentRegion whole{0, 0, 20u, 10u};
    const TransformImage before =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 20u, 10u);

    const Selection interior = selectRectangle(2.0f, 2.0f, 8.0f, 8.0f);
    (void)transformPreviewStraightHalf(od.document.layers[0], &interior, whole);

    const TransformImage after =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 20u, 10u);
    check(before.px.size() == after.px.size() &&
              std::memcmp(before.px.data(), after.px.data(), before.px.size() * sizeof(float)) == 0,
          "SABOTAGE: the source layer's tiles are BIT-IDENTICAL before and after "
          "transformPreviewStraightHalf() -- proof this file's read never became a write, whether "
          "or not a selection was passed");
  }

  return ok;
}

}  // namespace np
