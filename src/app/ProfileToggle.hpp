#pragma once

namespace np {

// --profile-toggle <file.psd> <layer-index> <iterations> : headless
// benchmarking scaffold for one layer's visibility toggle, added to chase a
// user-reported hitch when toggling a mid-stack layer in a large PSD.
//
// Imports the file once, then repeats exactly what a click on the layer
// panel's eye icon does downstream of ui/MacPaintUI's `run(setLayerVisible
// (...))`: flip `Layer::visible` and recomposite. `documentDirtyTiles()`
// classifies any visibility change as `FullRecompositeReason::
// LayerVisibilityChanged` (core/DirtyTiles.cpp), so ui/DocumentTexture's own
// update path calls `compositeDocumentStraightHalf()` -- this harness calls
// the identical function, which is why it needs no GPU device: that call is
// the whole CPU cost of the real toggle, missing only the `wgpuQueueWrite
// Texture` upload after it (a DMA of already-computed bytes, not a source of
// per-frame variance the way the composite walk is).
//
// Temporary: exists to be profiled under Instruments/xctrace, not to become
// a permanent CLI surface. Prints min/median/mean/max milliseconds per
// composite across `iterations` alternating toggles once done, then repeats
// the identical toggle sequence a second time through the buffer-reuse path
// `ui::DocumentTexture`'s own full recomposite now takes (one accumulator
// held and reused across every toggle, rather than a fresh one per call) and
// prints a second min/median/mean/max line for it.
int runProfileToggle(const char* psdPath, int layerIndex, int iterations);

}  // namespace np
