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
// the `wgpuQueueWriteTexture` calls removed, so it needs no GPU device. Its
// full-recomposite branch also matches `DocumentTexture::viewFor()`'s own:
// composited into a persistent buffer reused across calls
// (`premultScratch_`'s analogue here), not a fresh allocation per call -- the
// same buffer-reuse optimisation, so a key miss that does take the full path
// is not measured against a stale, pre-optimisation cost.
//
// **The omitted upload calls are not free, and this harness cannot see
// that.** A user-reported ~2s stall toggling a layer covering 780 of 800
// tiles on a real 5000x2559, 50-layer document measured ~15ms here (small
// layer, few tiles) and ~450ms live for the large one -- almost entirely
// `wgpuQueueWriteTexture` driver-call overhead, once tiles are packed one at
// a time regardless of how many sit adjacent in a row. `ui/DocumentTexture.cpp`
// now batches each contiguous run of adjacent dirty tiles into one upload
// call instead of one per tile (this file's own loop was updated to match
// its packing shape, run by run, for the same reason it always mirrors that
// class: so the two do not silently drift apart on what "the real cost"
// means). This harness still cannot measure the fix, or a regression in it --
// only a live run can, with `--frame-trace` (main.cpp) printing per-phase
// timing plus the pool's `lastDirtyTiles()`/`lastFullRecompositeReason()` for
// exactly this reason.
//
// Temporary: exists to be profiled under Instruments/xctrace, not to become
// a permanent CLI surface. Prints min/median/mean/max milliseconds per
// composite across `iterations` alternating toggles, plus how many of those
// iterations actually took the full-canvas path vs. the narrowed incremental
// one vs. an empty (nothing-to-composite) key miss -- the number this fix
// exists to move, made visible rather than left implicit in the timing alone.
int runProfileToggle(const char* psdPath, int layerIndex, int iterations);

}  // namespace np
