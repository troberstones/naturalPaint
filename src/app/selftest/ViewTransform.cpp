#include "app/selftest/Support.hpp"

namespace np {

bool runViewTransformTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearVec = [&](Vec2 a, Vec2 b, float tol) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };

  // --- (a) round-trip identity: toCanvas(toScreen(p)) == p, across a spread
  // of zoom/pan/mirrorX/mirrorY/rotation combinations, including mirrors
  // and a non-zero rotation together -- the concrete proof that "pen input
  // maps back through its inverse" (docs/shortcuts.md section 3) actually
  // holds, for the general case, not just identity. ---------------------
  {
    struct Case {
      float zoom, panX, panY, rotation;
      bool mirrorX, mirrorY;
      const char* what;
    };
    const Case cases[] = {
        {1.0f, 0.0f, 0.0f, 0.0f, false, false, "identity"},
        {2.5f, 30.0f, -12.0f, 0.0f, false, false, "zoom+pan only"},
        {1.0f, 0.0f, 0.0f, 0.0f, true, false, "mirror X only"},
        {1.0f, 0.0f, 0.0f, 0.0f, false, true, "mirror Y only"},
        {1.0f, 0.0f, 0.0f, 0.0f, true, true, "both mirrors (180 degrees)"},
        {1.0f, 0.0f, 0.0f, 0.7f, false, false, "rotation only"},
        {3.0f, -40.0f, 18.0f, 1.9f, true, true,
         "zoom+pan+both mirrors+rotation together"},
        {0.35f, 100.0f, -75.0f, -2.4f, true, false,
         "small zoom, big pan, mirror X, negative rotation"},
    };
    const Vec2 canvasCenter{512.0f, 384.0f};
    const Vec2 pivotScreen{640.0f, 400.0f};
    const Vec2 samplePts[] = {{0.0f, 0.0f}, {1024.0f, 768.0f}, {512.0f, 384.0f},
                              {200.0f, 600.0f}, {900.0f, 50.0f}};

    for (const auto& c : cases) {
      CanvasView v;
      v.zoom = c.zoom;
      v.panX = c.panX;
      v.panY = c.panY;
      v.mirrorX = c.mirrorX;
      v.mirrorY = c.mirrorY;
      v.rotation = c.rotation;
      const ViewTransform xform(v, canvasCenter, pivotScreen);
      for (Vec2 p : samplePts) {
        const Vec2 s = xform.toScreen(p);
        const Vec2 back = xform.toCanvas(s);
        char label[192];
        std::snprintf(label, sizeof(label),
                      "runViewTransformTest: round-trip identity (%s) at canvas (%.0f,%.0f)",
                      c.what, p.x, p.y);
        check(nearVec(back, p, 1e-2f), label);
      }
    }
  }

  // --- (b) hand-computed known point: zoom=2, mirrorX=true, rotation=90
  // degrees, canvasCenter=(100,50), pivotScreen=(300,200), canvas point
  // p=(150,50).
  //
  //   rel = p - canvasCenter = (50, 0)
  //   mirror X (mx=-1, my=1): (-50, 0)
  //   rotate 90 degrees (cosT=0, sinT=1):
  //     x' = x*cosT - y*sinT = -50*0 - 0*1 = 0
  //     y' = x*sinT + y*cosT = -50*1 + 0*0 = -50
  //   scale by zoom=2: (0, -100)
  //   + pivotScreen (300, 200) => (300, 100)
  //
  // -- checked against the transform's actual output, not just re-asserted
  // as "whatever toScreen returns", and then round-tripped back through
  // toCanvas() to confirm it lands on the same (300, 100) -> (150, 50). ---
  {
    CanvasView v;
    v.zoom = 2.0f;
    v.mirrorX = true;
    v.mirrorY = false;
    v.rotation = 1.5707963267948966f;  // 90 degrees, avoiding an M_PI dependency
    const Vec2 canvasCenter{100.0f, 50.0f};
    const Vec2 pivotScreen{300.0f, 200.0f};
    const ViewTransform xform(v, canvasCenter, pivotScreen);

    const Vec2 p{150.0f, 50.0f};
    const Vec2 s = xform.toScreen(p);
    check(nearVec(s, Vec2{300.0f, 100.0f}, 1e-2f),
          "runViewTransformTest: hand-computed toScreen (zoom=2, mirrorX, 90-degree rotation) "
          "lands exactly where hand-worked algebra predicts");
    const Vec2 back = xform.toCanvas(s);
    check(nearVec(back, p, 1e-2f),
          "runViewTransformTest: that same hand-computed screen point round-trips back to the "
          "original canvas point through toCanvas()");
  }

  // --- (c) view-only: toggling mirrorX/mirrorY/rotation/grayscale/grade
  // never mutates PaintSim's own canvas texture -- the available headless
  // proxy for PLAN.md's "mirror both axes, save, reopen -- the file is
  // unmirrored" (no save path exists yet in this codebase to assert that
  // literally, so this checks the thing that could actually regress it: does
  // flipping view state write into the document/canvas at all).
  //
  // mirrorX/mirrorY/rotation have no code path into PaintSim whatsoever --
  // ui/MacPaintUI.cpp only ever reads them to place screen-space quad
  // corners (ViewTransform::toScreen()) and to invert a mouse position
  // (ViewTransform::toCanvas()), never to call anything on a PaintSim. The
  // two view flags that *do* reach into PaintSim are grayscale (via
  // updateGrayscalePreview()) and grade (PLAN.md Phase 3 step 6, via
  // updateGradePreview()) -- so those are what this actually exercises
  // against a live sim, each twice (to catch a pass that only corrupts
  // canvas_ on a repeat), while readbackCanvas() -- the same technique
  // runFieldAllocationTest() and runSelfTest() already hold PaintSim to --
  // confirms canvas_ itself never moved. updateGradePreview() is exercised
  // with an empty (identity-bake) OpStack here -- this block's job is only
  // "does running the grade preview ever touch canvas_", not grading
  // correctness, which runApplyPassTest() (Phase 3 step 6's own dedicated
  // case, immediately below in the --selftest chain) already covers in
  // depth. ----------------------------------
  {
    std::vector<uint8_t> before, after;
    const bool readBefore = sim.readbackCanvas(gpu, before);
    check(readBefore, "runViewTransformTest: canvas readback before toggling view state");

    CanvasView view;  // the real AppState type -- not a stand-in struct
    view.mirrorX = true;
    view.mirrorY = true;
    view.rotation = 1.234f;
    view.grayscale = true;
    view.grade = true;
    (void)view;  // exercised for its shape only; nothing reads it further --
                 // mirror/rotation have no PaintSim call to make in the
                 // first place, per the comment above.
    sim.updateGrayscalePreview(gpu);
    sim.updateGrayscalePreview(gpu);
    const OpStack identityOps;  // empty -- detectRuns() has nothing to bake, a valid seed-only LUT
    sim.updateGradePreview(gpu, identityOps);
    sim.updateGradePreview(gpu, identityOps);

    const bool readAfter = sim.readbackCanvas(gpu, after);
    check(readAfter, "runViewTransformTest: canvas readback after toggling view state");
    check(readBefore && readAfter && before == after,
          "runViewTransformTest: mirror/rotation/grayscale/grade view state leaves PaintSim's "
          "own canvas byte-identical (proxy for PLAN.md's \"save with a mirror on -> file is "
          "unmirrored\" -- no save path exists yet to test that literally)");
  }

  std::printf("[selftest] view transform %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
