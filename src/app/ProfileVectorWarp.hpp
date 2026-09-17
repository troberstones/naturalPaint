#pragma once

namespace np {

// --profile-vector-warp [width height anchors iterations] : headless
// benchmarking scaffold for the two user-reported iPad hitches --
// "drawing/selecting/dragging vector anchors is slow" and "warp is slow, and
// looks bad" -- added because app/ProfileToggle.hpp's own precedent is
// exactly this project's answer to "don't guess, measure": a synthetic
// document plus the real production functions, timed with no GPU device and
// no touchscreen required (see this file's own .cpp for why a physical iPad
// gives no scriptable way to synthesize a touch-drag, which is what forced
// this shape of harness instead of a live `--frame-trace` capture).
//
// Three things are measured, each mirroring one real per-frame call site
// exactly (see the .cpp for the call site each block reproduces):
//
//   1. `hitTestPath()` over a many-anchor, many-shape document -- what a
//      knot-select or knot-drag PRESS pays before anything else happens.
//   2. `rasterizeVectorLayer()` on a per-frame content-hash miss -- what a
//      knot DRAG pays every frame it moves, since `MaterializedDocument`
//      hashes shape content and a moved anchor changes that hash every frame
//      (core/VectorRaster.hpp's cache has no per-frame throttle, unlike #3).
//   3. `warpRgbTiles()` -- what `ui/MacPaintUI.cpp`'s `warpPreviewViewFor()`
//      pays on a cache-due tick while dragging a warp handle (throttled to
//      once per 50ms there; this harness measures the unthrottled cost of
//      one tick so the 50ms budget can be judged against it).
//
// `width`/`height` default to a realistic iPad Pro canvas (4096x3072);
// `anchors` to a moderately complex path (200, spread across 4 shapes);
// `iterations` to 30. Temporary: exists to be profiled under Instruments/
// xctrace on the physical device, not to become a permanent CLI surface.
int runProfileVectorWarp(int width, int height, int anchors, int iterations);

}  // namespace np
