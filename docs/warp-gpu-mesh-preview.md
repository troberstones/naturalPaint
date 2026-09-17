# Warp preview as a GPU mesh

**Status: proposed, nothing built.** Wanted follow-on to D23's lattice warp.
This file is the sketch, not a plan with steps — it records why the idea is
worth doing, what already exists that it would reuse, and the three things
that are genuinely unsolved, so whoever picks it up does not rediscover them.

## The idea

Draw the warp drag preview as a **textured triangle mesh on the GPU** — one
upload of the source crop per session, then per frame a draw of the
tessellated net with per-vertex `(u, v)` — instead of re-warping the whole
document on the CPU every throttled tick.

## Why it is worth doing

`warpPreviewViewFor()` (`src/ui/MacPaintUI.cpp`) recomputes a whole
hypothetical `Document` through `TransformSession::previewWarpDocument()`,
which runs `warpRgbTiles()` over the entire layer. Its throttle interval **is**
that recompute's own measured duration, deliberately — so the drag preview's
frame rate is exactly the reciprocal of this cost:

| canvas | per tick (Release+LTO, M-series) | preview rate |
|---|---|---|
| 2048x1536 | 20 ms | ~50 Hz |
| 4096x3072 | 91 ms | ~11 Hz |

Those are the numbers *after* dcb48e8 made the rasteriser 1.5x faster, and
that commit's own notes say where the rest of the time sits: the remaining
cost is resampling 12.6M destination pixels and shuttling two 201 MB float
images, none of which a further CPU optimisation removes by an order of
magnitude. The iPad is the case that motivates this — the same function's
header records a ~5.3 s recompute there before the gather went parallel.

A GPU mesh draw costs **screen** pixels, not document pixels, and does not
scale with canvas size at all.

## What already exists — this is an extension, not a new mechanism

The affine half of Free Transform already works exactly this way, and the
draw site says so in as many words:

> Warp's own live pixels come from `warpPreviewViewFor()`'s whole-canvas
> composite (`documentView`, above), not this four-corner quad — a non-affine
> net has no four corners to draw one at.
> — `src/ui/MacPaintUI.cpp`, in the T14 preview block

That is the whole gap, and the answer is that a warp does not have four
corners but it *does* have a mesh of them:

- **`ui/TransformPreviewTexture.hpp`** uploads the untransformed source crop
  **once** per session (the source cannot change while a session is live —
  that is `app/TransformSession`'s invariant) and never re-reads a tile
  during the drag.
- **`ui/CanvasQuad`** draws that texture as an arbitrary quad through our own
  pipeline, with the GPU's own bilinear-minify/nearest-magnify sampler
  standing in for a resampler. There is no CPU resample in the affine drag
  path at all.
- **`tessellateWarpMesh()`** (`src/app/WarpMesh.cpp`) already produces
  precisely the vertex data a mesh draw needs: each `WarpQuad` carries its
  four document-space corners *and* the `(u0,v0)-(u1,v1)` rectangle they were
  evaluated at. Nothing new has to be computed — at a typical bend that is
  225 quads, 450 triangles, which is nothing for a GPU.
- **The below / moving-pixels / above composite split** the affine preview
  draws into already exists in the same canvas block, so a warped mesh slots
  into the same three-part ordering rather than needing its own.

So the missing piece is narrow: a `ui/CanvasQuad` variant that takes a vertex
and index buffer instead of four corners.

## Scope: preview only

`commit()` keeps its CPU Catmull-Rom pass. This changes what is on screen
*during* a drag, not what lands in the document — the same split
`docs/testing-issues.md`'s T14 entry already asks for ("a cheap kernel
(nearest or bilinear)" at draw time against `commit()`'s one Catmull-Rom
pass), and the same split the affine preview already ships.

## The three things that are actually unsolved

1. **A folded warp renders differently from what it commits.** The CPU gather
   picks, per destination pixel, the candidate quad whose parametric interior
   contains it *with the largest margin* — an explicit tie-break for the
   overlap a self-intersecting net produces. A GPU rasteriser has no such
   rule: overlapping triangles resolve by draw order (or by a depth test we
   would have to invent). A fold would therefore preview as something
   `commit()` does not reproduce. Either give the mesh a depth value derived
   from the same margin, or accept and document the divergence — but decide
   it deliberately, because it is silent otherwise.

2. **The one-time upload becomes this feature's new floor.**
   `app/selftest/TransformPreviewTexture.cpp` measures the affine upload at a
   2048x2048 fully-opaque layer — **11-14 ms** across runs as of dcb48e8,
   down from 18.2 ms before it parallelised `imageFromTileStore()`, which is
   the pass that dominates it along with the per-texel unpremultiply +
   `floatToHalf()`. Read the number the suite prints rather than this one: it
   swings by 25% run to run, and that header carried a stale "49.7 ms" for a
   while. It fits PRD F3's 20 ms today and it is paid
   once per session rather than every tick, so trading it against ~91 ms *per
   tick* is unambiguous. But it is O(document pixels) and a 4096x3072 layer is
   4x the fixture, so it will not fit there — and the fix that header already
   names, packing at **view** resolution, prefiltered through
   `ops/Resample.hpp` and re-triggered on a zoom change mid-drag, is unbuilt
   for the affine case too. Doing it once would serve both.

3. **Bilinear is not Catmull-Rom.** The preview would soften relative to the
   committed result. Already true of the affine preview, so this is a
   consistency argument rather than a new defect — but a warp magnifies more
   aggressively than a typical affine drag, so the difference will be more
   visible and should be looked at on a real image before calling it fine.

## What would tell you it worked

`--profile-vector-warp` measures the CPU rasteriser and would be unchanged by
this — it is the wrong instrument. The thing to measure is the drag frame
itself, and the honest check is that `warpPreviewViewFor()`'s adaptive
throttle (`cache.throttleSeconds`) collapses to its 1/60 s floor and stays
there at 4096x3072, on the iPad as well as the Mac.
