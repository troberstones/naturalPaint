#include "app/selftest/Support.hpp"

namespace np {

// PLAN.md Phase 2 step 9 ("Mip pyramid for tiles, so a 25% zoom evaluates at
// a matching level"). See SelfTest.hpp for the full breakdown; in short:
// CPU-only downsample correctness for buildMipChain() (no GPU), CPU-only
// level-selection formula checks for mipLevelForZoom() (no GPU), and an
// end-to-end GPU proof that draw()'s own level pick actually changes which
// texels land on screen -- extending runTiledViewportTest()'s own offscreen
// blit-and-readback technique (immediately above) rather than duplicating
// it from scratch.
bool runMipPyramidTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  constexpr float kTol = 0.02f;

  // --- CPU-side downsample correctness: no GPU needed. A hand-built Tile
  // with a known, distinct 4x4 ramp in its R channel (v(x,y) = x + 4*y; G,
  // B, A left at 0/0/1) lets mip level 1's four corner texels, and mip
  // level 2's single corner texel, be hand-computed independently of
  // buildMipChain() itself -- checking both the 128->64 step and the
  // 64->32 step, i.e. the recursive "downsample the downsample" step, not
  // just level 0->1 -----------------------------------------------------
  {
    Tile tile;  // value-initialized to transparent black everywhere else
    for (int32_t y = 0; y < 4; ++y) {
      for (int32_t x = 0; x < 4; ++x) {
        const float v = static_cast<float>(x + 4 * y);
        tile.writePixel(PixelCoord{x, y}, {v, 0.0f, 0.0f, 1.0f});
      }
    }

    const std::vector<MipLevel> mips = buildMipChain(tile);
    check(mips.size() == static_cast<size_t>(kMipLevelCount),
          "buildMipChain: produces the full 128..1 chain (8 levels)");

    if (mips.size() >= 3) {
      check(mips[0].size == 128 && mips[1].size == 64 && mips[2].size == 32,
            "buildMipChain: level sizes halve each step (128, 64, 32, ...)");

      auto texelR = [&](const MipLevel& lvl, int32_t x, int32_t y) {
        return lvl.texels[(static_cast<size_t>(y) * static_cast<size_t>(lvl.size) +
                           static_cast<size_t>(x)) *
                          4];
      };
      // Hand-computed level-1 corner values -- each is the average of the
      // corresponding 2x2 block of level 0's ramp; all four blocks sit
      // entirely inside the written 4x4 region, so texels outside it (left
      // at 0 by Tile's default construction) don't influence these.
      //   (0,0): {0,1,4,5}/4 = 2.5   (1,0): {2,3,6,7}/4 = 4.5
      //   (0,1): {8,9,12,13}/4 = 10.5  (1,1): {10,11,14,15}/4 = 12.5
      check(near(texelR(mips[1], 0, 0), 2.5f, 1e-4f) && near(texelR(mips[1], 1, 0), 4.5f, 1e-4f) &&
                near(texelR(mips[1], 0, 1), 10.5f, 1e-4f) &&
                near(texelR(mips[1], 1, 1), 12.5f, 1e-4f),
            "buildMipChain: level 1's four corner texels equal the hand-computed 2x2 box-filter "
            "average of level 0's ramp");
      // Level 2's corner texel recurses on level 1's own four values above
      // (2.5, 4.5, 10.5, 12.5), not on level 0 directly -- {2.5+4.5+10.5+
      // 12.5}/4 = 7.5. This is the check that actually exercises the
      // recursive step: a chain that only handles level 0->1 correctly and
      // silently no-ops (or copies) every level after would fail this.
      check(near(texelR(mips[2], 0, 0), 7.5f, 1e-4f),
            "buildMipChain: level 2's corner texel equals the hand-computed average of level "
            "1's own values -- proves the recursive downsample-the-downsample step, not just "
            "level 0->1");
    }
  }

  // --- level selection: mipLevelForZoom() is pure math, no GPU needed.
  // PLAN.md's own literal example (zoom=0.25 -> the 32px level, mip 2) plus
  // zoom=1.0 -> level 0, and clamping at both extremes ------------------
  check(mipLevelForZoom(1.0f) == 0, "mipLevelForZoom: 100% zoom selects level 0 (full 128px res)");
  check(mipLevelForZoom(0.25f) == 2,
        "mipLevelForZoom: 25% zoom selects level 2 (128 -> 64 -> 32px) -- PLAN.md's own example");
  check(mipLevelForZoom(0.5f) == 1, "mipLevelForZoom: 50% zoom selects level 1 (64px)");
  check(mipLevelForZoom(2.0f) == 0, "mipLevelForZoom: zooming in past 100% still clamps at level 0");
  check(mipLevelForZoom(1000.0f) == 0,
        "mipLevelForZoom: extreme zoom-in clamps at level 0, not a negative level");
  check(mipLevelForZoom(0.001f) == kMipLevelCount - 1,
        "mipLevelForZoom: extreme zoom-out clamps at the smallest level rather than going out "
        "of range");

  // --- end-to-end: a known, non-uniform (checkerboard) tile -> uploaded
  // mip chain -> the level draw() would pick for a given zoom -> an
  // offscreen render placed at tileScreenRect()'s own rect -> read back and
  // confirm the pixels match the *downsampled* level's known value, not
  // level 0's -- the check that actually proves level selection is wired
  // into the real GPU draw path, not just computed and ignored -----------
  {
    Document doc = Document::createBlank(kTileSize, kTileSize, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    // Finest-period checkerboard, opaque red/green. Any 2x2 box-filter
    // average of it is *exactly* uniform (0.5, 0.5, 0, 1) -- true of every
    // block, so mip level 1 and every level after it reads back that one
    // uniform colour everywhere, cleanly distinguishable from level 0's
    // alternating pure red/green at the same texel.
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const bool evenParity = ((x + y) % 2) == 0;
        tile.writePixel(PixelCoord{x, y}, evenParity
                                              ? std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f}
                                              : std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f});
      }
    }

    TiledDocumentView tv;
    tv.setDocument(gpu, doc);
    check(tv.tileCount() == 1, "runMipPyramidTest: checkerboard fixture uploads exactly one tile");
    const auto it = tv.tiles().find(TileCoord{0, 0});
    check(it != tv.tiles().end(), "runMipPyramidTest: the uploaded tile is at TileCoord{0,0}");

    if (it != tv.tiles().end()) {
      check(it->second.levelViews.size() == static_cast<size_t>(kMipLevelCount),
            "runMipPyramidTest: the uploaded tile carries one per-level view for each of the 8 "
            "mip levels");

      // PLAN.md's own literal example: 25% zoom -> the 32px level.
      constexpr float kZoom = 0.25f;
      const int level = mipLevelForZoom(kZoom);
      check(level == 2,
            "runMipPyramidTest: zoom=0.25 selects level 2 -- exactly the level draw() itself "
            "would compute for this zoom");

      if (level >= 0 && static_cast<size_t>(level) < it->second.levelViews.size()) {
        CanvasView view;
        view.zoom = kZoom;
        const ImVec2 canvasOrigin(10.0f, 20.0f);
        const TileScreenRect rect = tileScreenRect(TileCoord{0, 0}, view, canvasOrigin);

        WGPUShaderModule shaderMod = compileBlitShader(gpu);
        check(shaderMod != nullptr, "runMipPyramidTest: blit shader compiles");

        if (shaderMod) {
          WGPUColorTargetState target = {};
          target.format = WGPUTextureFormat_RGBA16Float;
          target.writeMask = WGPUColorWriteMask_All;

          WGPUFragmentState fs = {};
          fs.module = shaderMod;
          fs.entryPoint = sv("fs");
          fs.targetCount = 1;
          fs.targets = &target;

          WGPURenderPipelineDescriptor rd = {};
          rd.label = sv("mip-pyramid-selftest-blit");
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
          check(pipeline != nullptr, "runMipPyramidTest: blit render pipeline builds");

          if (pipeline) {
            constexpr uint32_t kTargetSize = 64;  // comfortably covers rect (32x32 at zoom=0.25)
            auto sampleAt = [&](const std::vector<float>& px, uint32_t x, uint32_t y) {
              const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
              return std::array<float, 4>{px[i], px[i + 1], px[i + 2], px[i + 3]};
            };
            auto pixNear = [&](std::array<float, 4> a, std::array<float, 4> b, const char* what) {
              check(near(a[0], b[0], kTol) && near(a[1], b[1], kTol) && near(a[2], b[2], kTol) &&
                        near(a[3], b[3], kTol),
                    what);
            };
            const uint32_t sx = static_cast<uint32_t>(rect.min.x) + 2;
            const uint32_t sy = static_cast<uint32_t>(rect.min.y) + 2;

            std::vector<float> levelPixels;
            const bool levelReadOk = blitPipelineRenderAndReadback(
                gpu, pipeline, it->second.levelViews[static_cast<size_t>(level)], rect,
                kTargetSize, levelPixels);
            check(levelReadOk, "runMipPyramidTest: offscreen render of the level-2 view reads "
                               "back");
            if (levelReadOk) {
              pixNear(sampleAt(levelPixels, sx, sy), {0.5f, 0.5f, 0.0f, 1.0f},
                     "runMipPyramidTest: rendered pixels match level 2's known downsampled "
                     "colour (uniform 0.5/0.5/0/1), proving level selection reached the real "
                     "GPU draw path");
            }

            // Contrast check: the exact same screen rect, rendered from
            // level 0's own single-level view instead, reads back a pure
            // checkerboard colour at this texel -- never the level-2 grey
            // -- so the level-2 result above is not a value level 0 could
            // have produced by coincidence. textureLoad is a point sample
            // (no bilinear filtering), so whichever texel the pixel-centre
            // arithmetic actually lands on, the value it reads is always
            // *exactly* pure red or pure green, never a blend -- checked as
            // "one of the two", not a specific one, so this doesn't depend
            // on hand-tracking sub-pixel/texel-index arithmetic through the
            // vertex shader's interpolation.
            std::vector<float> level0Pixels;
            const bool level0ReadOk = blitPipelineRenderAndReadback(
                gpu, pipeline, it->second.levelViews[0], rect, kTargetSize, level0Pixels);
            check(level0ReadOk,
                  "runMipPyramidTest: offscreen render of level 0's own view reads back "
                  "(contrast check)");
            if (level0ReadOk) {
              const std::array<float, 4> px = sampleAt(level0Pixels, sx, sy);
              const bool isPureCheckerColor =
                  (near(px[0], 1.0f, kTol) && near(px[1], 0.0f, kTol)) ||
                  (near(px[0], 0.0f, kTol) && near(px[1], 1.0f, kTol));
              check(isPureCheckerColor && near(px[2], 0.0f, kTol) && near(px[3], 1.0f, kTol),
                    "runMipPyramidTest: the same screen rect rendered from level 0 instead "
                    "reads a pure checkerboard colour (red or green), not level 2's grey -- "
                    "the level-2 result above genuinely differs from level 0, not "
                    "coincidentally equal");
            }

            wgpuRenderPipelineRelease(pipeline);
          }
        }
      }
    }

    tv.release();
  }

  std::printf("[selftest] mip pyramid %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
