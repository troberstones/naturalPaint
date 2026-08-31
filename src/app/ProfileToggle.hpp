#pragma once

namespace np {

// --profile-toggle <file.psd> <layer-index> <iterations> : headless
// benchmarking scaffold for one layer's visibility toggle, added to chase a
// user-reported hitch when toggling a mid-stack layer in a large PSD.
//
// Imports the file once, then repeats exactly what a click on the layer
// panel's eye icon does downstream of ui/MacPaintUI's `run(setLayerVisible
// (...))`: flip `Layer::visible` and bring the document texture up to date.
//
// **This mirrors `ui/DocumentTexture::viewFor()`'s CPU-only work, not just
// `compositeDocumentStraightHalf()`.** Before core/DirtyTiles.cpp's per-layer
// tile-footprint narrowing, `documentDirtyTiles()` classified any visibility
// change as `FullRecompositeReason::LayerVisibilityChanged` unconditionally,
// so the real update path always took the full-canvas branch and calling
// `compositeDocumentStraightHalf()` directly WAS the whole story. Now a
// visibility toggle on a layer that holds pixels and is not an Adjustment
// layer narrows to that layer's own tiles (when `preferFullRecomposite()`'s
// crossover doesn't override it back to full because the dirty set already
// covers the canvas), so this harness has to run `documentDirtyTiles()` and
// the tile-band composite-and-pack loop itself to measure the real cost --
// see `applyDocumentUpdate()` in ProfileToggle.cpp, which is that logic with
// the `wgpuQueueWriteTexture` calls removed (a DMA of already-computed bytes,
// not a source of per-frame variance the way the composite walk is) and
// nothing else changed, so it needs no GPU device.
//
// Temporary: exists to be profiled under Instruments/xctrace, not to become
// a permanent CLI surface. Prints min/median/mean/max milliseconds per
// composite across `iterations` alternating toggles once done.
int runProfileToggle(const char* psdPath, int layerIndex, int iterations);

}  // namespace np
