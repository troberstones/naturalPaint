#include "app/selftest/Support.hpp"

namespace np {

bool runTiledViewportTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol = 0.02f;

  // --- a Document with no RGB layer at all, and a createBlank()'d one (RGB
  // layer present, zero occupied tiles) both upload nothing, and draw()
  // no-ops safely -- even given a null ImDrawList, the one call in this
  // module that needs a live ImGui context, which this headless test
  // deliberately never constructs (see SelfTest.hpp's comment on this
  // function) -------------------------------------------------------------
  {
    Document noLayers;
    noLayers.width = 64;
    noLayers.height = 64;
    TiledDocumentView tv;
    tv.setDocument(gpu, noLayers);
    check(tv.tileCount() == 0,
          "TiledDocumentView: a Document with no RGB layer uploads zero tiles");
    tv.draw(nullptr, CanvasView{}, ImVec2(0, 0));
    check(true, "TiledDocumentView: draw() doesn't crash with zero tiles / a null ImDrawList");
    tv.release();
  }
  {
    const Document blank = Document::createBlank(256, 256, WorkingSpace{});
    TiledDocumentView tv;
    tv.setDocument(gpu, blank);
    check(tv.tileCount() == 0,
          "TiledDocumentView: a freshly createBlank()'d Document uploads zero tiles "
          "(RGB layer present, but nothing painted into it yet)");
    tv.release();
  }

  // --- tileScreenRect(): pure geometry, checked against a hand-computed
  // expectation independent of anything GPU-side --------------------------
  CanvasView view;
  view.zoom = 2.0f;
  view.panX = 5.0f;
  view.panY = -3.0f;
  const ImVec2 canvasOrigin(10.0f, 20.0f);
  const TileScreenRect rect = tileScreenRect(TileCoord{0, 0}, view, canvasOrigin);
  // screenPos = canvasOrigin + tileOrigin({0,0})*zoom + (panX,panY)
  //           = (10,20) + (0,0)*2 + (5,-3) = (15,17); size = 128*2 = 256.
  check(near(rect.min.x, 15.0f, 1e-3f) && near(rect.min.y, 17.0f, 1e-3f) &&
            near(rect.max.x, 271.0f, 1e-3f) && near(rect.max.y, 273.0f, 1e-3f),
        "tileScreenRect: matches canvasOrigin + tileOrigin*zoom + pan, tile size kTileSize*zoom");

  // --- end to end: a known-pixel Document -> uploaded tile -> a dedicated
  // offscreen WebGPU render pass places it at tileScreenRect()'s own rect ->
  // read back and check known corners land at the expected screen pixel and
  // colour (PLAN.md step 8's actual verify criterion) ---------------------
  {
    // 2x2 fixture, opaque corners (alpha=255) so premultiplied == straight --
    // io/ImageIO's premultiply behaviour is already covered by
    // runImageIOTest(); this fixture is about position and upload, not
    // premultiply, so keeping alpha out of the arithmetic keeps the expected
    // colours exact (0.0/1.0, not a fraction).
    const uint8_t px[2 * 2 * 4] = {
        255, 0,   0,   255,  0,   255, 0,   255,
        0,   0,   255, 255,  255, 255, 255, 255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "runTiledViewportTest: 2x2 fixture PNG decodes");
    if (!docOpt) {
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    TiledDocumentView tv;
    tv.setDocument(gpu, *docOpt);
    check(tv.tileCount() == 1, "TiledDocumentView: the 2x2 fixture uploads exactly one tile");
    const auto it = tv.tiles().find(TileCoord{0, 0});
    check(it != tv.tiles().end(), "TiledDocumentView: the uploaded tile is at TileCoord{0,0}");
    if (it == tv.tiles().end()) {
      tv.release();
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    WGPUShaderModule shaderMod = compileBlitShader(gpu);
    check(shaderMod != nullptr, "runTiledViewportTest: blit shader compiles");
    if (!shaderMod) {
      tv.release();
      std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
      return ok;
    }

    WGPUColorTargetState target = {};
    target.format = WGPUTextureFormat_RGBA16Float;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fs = {};
    fs.module = shaderMod;
    fs.entryPoint = sv("fs");
    fs.targetCount = 1;
    fs.targets = &target;

    WGPURenderPipelineDescriptor rd = {};
    rd.label = sv("tiled-viewport-selftest-blit");
    rd.vertex.module = shaderMod;
    rd.vertex.entryPoint = sv("vs");
    rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rd.primitive.frontFace = WGPUFrontFace_CCW;
    rd.primitive.cullMode = WGPUCullMode_None;
    rd.multisample.count = 1;
    rd.multisample.mask = 0xFFFFFFFF;
    rd.fragment = &fs;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
    wgpuShaderModuleRelease(shaderMod);
    check(pipeline != nullptr, "runTiledViewportTest: blit render pipeline builds");

    if (pipeline) {
      constexpr uint32_t kTargetSize = 64;  // comfortably covers the corner this test samples
      std::vector<float> pixels;
      // it->second.view is the tile's all-levels view; the blit shader
      // reads its level 0 explicitly (kBlitShaderSrc's `textureLoad(...,
      // 0)`), so this exercises the exact same level-0 pixels step 8's
      // test always has, just via the shared helper both this test and
      // runMipPyramidTest() below now use.
      const bool readOk =
          blitPipelineRenderAndReadback(gpu, pipeline, it->second.view, rect, kTargetSize, pixels);
      check(readOk, "runTiledViewportTest: offscreen render target reads back");

      if (readOk) {
        auto sample = [&](uint32_t x, uint32_t y) {
          const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
          return std::array<float, 4>{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        };
        auto pixNear = [&](std::array<float, 4> a, std::array<float, 4> b, const char* what) {
          check(near(a[0], b[0], kTol) && near(a[1], b[1], kTol) && near(a[2], b[2], kTol) &&
                    near(a[3], b[3], kTol),
                what);
        };

        // rect.min = (15,17), zoom = 2 -> tile texel (tx,ty) covers screen
        // [15+tx*2, 15+tx*2+2) x [17+ty*2, 17+ty*2+2); sampling the first
        // pixel of each block is unambiguous since 15/17 are exact integers.
        pixNear(sample(15, 17), {1.0f, 0.0f, 0.0f, 1.0f},
               "runTiledViewportTest: fixture's top-left red pixel lands at the tile's screen "
               "origin (rect.min)");
        pixNear(sample(17, 17), {0.0f, 1.0f, 0.0f, 1.0f},
               "runTiledViewportTest: fixture's top-right green pixel lands one zoomed texel "
               "to the right");
        pixNear(sample(15, 19), {0.0f, 0.0f, 1.0f, 1.0f},
               "runTiledViewportTest: fixture's bottom-left blue pixel lands one zoomed texel "
               "down");
        pixNear(sample(17, 19), {1.0f, 1.0f, 1.0f, 1.0f},
               "runTiledViewportTest: fixture's bottom-right white pixel lands diagonally "
               "opposite the origin");
        // Untouched tile interior (only the 2x2 fixture corner was ever
        // written; core::Tile value-initializes the rest to zero) stays
        // transparent black, not garbage.
        pixNear(sample(25, 27), {0.0f, 0.0f, 0.0f, 0.0f},
               "runTiledViewportTest: unpainted tile interior reads back transparent black");
        // Outside the tile's own screen quad entirely (rect.min is (15,17);
        // this is well above/left of it) -- proves the draw is actually
        // bounded to the computed rect, not filling the whole target.
        pixNear(sample(2, 2), {0.0f, 0.0f, 0.0f, 0.0f},
               "runTiledViewportTest: area outside the tile's screen quad stays untouched "
               "(clear colour)");
      }

      wgpuRenderPipelineRelease(pipeline);
    }

    tv.release();
  }

  std::printf("[selftest] tiled viewport %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
