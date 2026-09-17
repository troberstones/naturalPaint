#include "app/ProfileVectorWarp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "app/PenTool.hpp"
#include "app/WarpMesh.hpp"
#include "core/Document.hpp"
#include "core/VectorRaster.hpp"
#include "core/VectorShape.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// See ProfileVectorWarp.hpp for what this measures and why. Physical-device
// note: `xcrun devicectl` can install and launch an app on the iPad and
// stream its stdout/stderr, but nothing in this toolchain can synthesize a
// finger drag on the glass -- `xctrace record` needs a live gesture to
// sample against, and there is no scriptable substitute for one on a
// CoreDevice-managed iPad the way `mcp__Claude_Code_iOS_Simulator__control`
// can tap a *simulator*. This harness sidesteps that: every function it
// times is pure and headless already (both headers say so), so the same
// binary that would otherwise sit idle waiting for a touch can instead time
// its own hot path directly, on the real CPU, with no GPU device and no
// window.
namespace np {

namespace {

void printStats(const char* label, std::vector<double>& ms) {
  std::sort(ms.begin(), ms.end());
  double sum = 0.0;
  for (double v : ms) sum += v;
  std::printf("profile-vector-warp: %s: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms (n=%zu)\n",
              label, ms.front(), ms[ms.size() / 2], sum / static_cast<double>(ms.size()), ms.back(),
              ms.size());
}

// `shapeCount` shapes, each an open subpath of `anchorsPerShape` anchors
// zigzagging across [0,width) x [0,height) -- enough on-curve segments to
// give `rasterizeVectorLayer()` real scanline work across most of the
// canvas, and enough total anchors to give `hitTestPath()` a realistic
// component count to walk.
std::vector<VectorShape> buildShapes(int32_t width, int32_t height, int shapeCount,
                                     int anchorsPerShape) {
  std::vector<VectorShape> shapes;
  shapes.reserve(static_cast<size_t>(shapeCount));
  for (int s = 0; s < shapeCount; ++s) {
    VectorShape shape;
    shape.id = static_cast<uint64_t>(s + 1);
    SubPath sub;
    sub.closed = true;
    const float bandTop = static_cast<float>(height) * static_cast<float>(s) /
                          static_cast<float>(shapeCount);
    const float bandHeight = static_cast<float>(height) / static_cast<float>(shapeCount);
    for (int a = 0; a < anchorsPerShape; ++a) {
      const float t = static_cast<float>(a) / static_cast<float>(anchorsPerShape - 1);
      const float x = t * static_cast<float>(width);
      const float y = bandTop + bandHeight * (0.5f + 0.4f * ((a % 2 == 0) ? -1.0f : 1.0f));
      Anchor anchor;
      anchor.pt = PathPoint{x, y};
      // Handles offset from the anchor so the flattener walks real cubic
      // segments rather than degenerating to straight lines -- the case
      // `kDocumentTolerancePx`'s comment (core/VectorRaster.cpp) says the
      // flattening cost actually scales with.
      anchor.in = PathPoint{x - 8.0f, y};
      anchor.out = PathPoint{x + 8.0f, y};
      sub.anchors.push_back(anchor);
    }
    shape.path.subpaths.push_back(sub);
    shape.fill.on = true;
    shape.fill.rgba = {0.2f + 0.15f * static_cast<float>(s % 4), 0.4f, 0.6f, 1.0f};
    shape.stroke.on = true;
    shape.stroke.rgba = {0.05f, 0.05f, 0.05f, 1.0f};
    shape.strokeStyle.width = 2.0f;
    shapes.push_back(std::move(shape));
  }
  return shapes;
}

}  // namespace

int runProfileVectorWarp(int width, int height, int anchors, int iterations) {
  if (width <= 0) width = 4096;
  if (height <= 0) height = 3072;
  if (anchors <= 0) anchors = 200;
  if (iterations <= 0) iterations = 30;

  constexpr int kShapeCount = 4;
  const int anchorsPerShape = std::max(2, anchors / kShapeCount);

  std::printf("profile-vector-warp: canvas %dx%d, %d shapes x %d anchors, %d iterations\n", width,
              height, kShapeCount, anchorsPerShape, iterations);

  std::vector<VectorShape> shapes = buildShapes(width, height, kShapeCount, anchorsPerShape);
  const GradientTable gradients;

  // --- 1. hitTestPath(): what a press (select / grab-a-knot) pays --------
  {
    PathSelection selection;
    selection.mode = PathSelectMode::Component;
    for (uint64_t shapeId = 1; shapeId <= static_cast<uint64_t>(kShapeCount); ++shapeId) {
      for (uint32_t a = 0; a < static_cast<uint32_t>(anchorsPerShape); ++a) {
        ComponentRef c;
        c.shapeId = shapeId;
        c.anchor = a;
        selection.components.push_back(c);
      }
    }
    std::vector<double> ms(static_cast<size_t>(iterations));
    // One untimed call first (same reasoning as ProfileToggle.cpp: don't
    // credit the timed loop with first-call allocator warmup).
    (void)hitTestPath(shapes, selection, PathPoint{static_cast<float>(width) * 0.5f,
                                                    static_cast<float>(height) * 0.5f},
                      6.0f, false);
    for (int i = 0; i < iterations; ++i) {
      // A cursor that sweeps the canvas -- the miss case (empty canvas) a
      // marquee-drag frame pays, which walks every component with nothing to
      // early-out on.
      const float x = static_cast<float>(width) * (static_cast<float>(i % 7) / 7.0f);
      const float y = static_cast<float>(height) * (static_cast<float>((i * 3) % 5) / 5.0f);
      const auto t0 = std::chrono::steady_clock::now();
      const PathHit hit = hitTestPath(shapes, selection, PathPoint{x, y}, 6.0f, false);
      const auto t1 = std::chrono::steady_clock::now();
      ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
      (void)hit;
    }
    printStats("hitTestPath (press / marquee frame)", ms);
  }

  // --- 2. rasterizeVectorLayer(): what a drag frame pays on a hash miss --
  {
    std::vector<double> ms(static_cast<size_t>(iterations));
    (void)rasterizeVectorLayer(shapes, gradients, width, height);  // untimed warmup
    for (int i = 0; i < iterations; ++i) {
      // Nudge one anchor by a pixel each iteration -- exactly what dragging a
      // knot does to `PathEditState::shapesAtDragStart`-derived geometry one
      // frame at a time, and exactly what changes `vectorContentHash()`'s
      // answer so `MaterializedDocument`'s cache misses on every frame of the
      // drag (core/VectorRaster.hpp section on `VectorRasterCache`).
      shapes[0].path.subpaths[0].anchors[0].pt.x += 1.0f;
      const auto t0 = std::chrono::steady_clock::now();
      const uint64_t hash = vectorContentHash(shapes, gradients);
      const TileStore tiles = rasterizeVectorLayer(shapes, gradients, width, height);
      const auto t1 = std::chrono::steady_clock::now();
      ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
      (void)hash;
      (void)tiles;
    }
    printStats("rasterizeVectorLayer (per-frame hash-miss during a knot drag)", ms);
  }

  // --- 3. warpRgbTiles(): one warpPreviewViewFor() cache-due tick ---------
  {
    const DocumentRegion bounds{0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    WarpMesh mesh = WarpMesh::flat(bounds, 4);

    // A solid-filled tile store standing in for a real RGB layer -- the warp
    // sampler's cost does not depend on the pixel VALUES, only their count,
    // so a flat fill measures the same per-texel cost a real photo layer
    // would.
    TileStore layerTiles;
    {
      TransformImage img;
      img.width = static_cast<uint32_t>(width);
      img.height = static_cast<uint32_t>(height);
      img.px.assign(img.sampleCount(), 0.0f);
      for (size_t i = 0; i < img.sampleCount(); i += 4) {
        img.px[i + 0] = 0.5f;
        img.px[i + 1] = 0.3f;
        img.px[i + 2] = 0.2f;
        img.px[i + 3] = 1.0f;
      }
      tileStoreFromImage(img, 0, 0, &layerTiles);
    }

    std::vector<double> ms(static_cast<size_t>(iterations));
    const WarpControlRef center{true, mesh.n() / 2, mesh.n() / 2};
    std::string err;
    {
      // Untimed warmup tick, bent slightly, mirroring warpBeginDrag().
      WarpMesh warm = mesh;
      warm.dragControl(center, Point2{5.0f, 5.0f});
      TileStore out;
      const DocumentRegion dst = warpedRegion(warm, warpChordSubdivisions(warm));
      (void)warpRgbTiles(layerTiles, warm, dst, ResampleKernel::CatmullRom, &out, &err);
    }
    for (int i = 0; i < iterations; ++i) {
      // A slightly different bend each tick -- what `warpUpdateDrag()`
      // recomputes fresh from the drag's own baseline every call
      // (TransformSession.cpp's own comment: "never accumulates a delta onto
      // a delta").
      WarpMesh dragged = mesh;
      const float bend = 10.0f + static_cast<float>(i);
      dragged.dragControl(center, Point2{bend, -bend});
      const auto t0 = std::chrono::steady_clock::now();
      const DocumentRegion dst = warpedRegion(dragged, warpChordSubdivisions(dragged));
      TileStore out;
      const bool ok =
          warpRgbTiles(layerTiles, dragged, dst, ResampleKernel::CatmullRom, &out, &err);
      const auto t1 = std::chrono::steady_clock::now();
      ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (!ok) {
        std::fprintf(stderr, "profile-vector-warp: warpRgbTiles failed: %s\n", err.c_str());
        return 1;
      }
    }
    printStats("warpRgbTiles (one warpPreviewViewFor() cache-due tick)", ms);
  }

  return 0;
}

}  // namespace np
