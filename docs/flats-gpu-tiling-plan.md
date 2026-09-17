# Flats GPU membrane solver — tiling plan for higher-resolution canvases

**Status: not started.** This records a design for a follow-up, not work in
progress. The GPU port itself (below) is done and merged; tiling is the next
step only if canvas sizes push past what a whole-canvas GPU solve can hold.

## What already landed (2026-09-16/17)

`flats/Membrane.cpp`'s CPU geometric-multigrid Poisson solve (`flatMembraneSag`)
has a GPU port, `flats/MembraneGpu.hpp`/`.cpp`'s `membraneSagGpu`, verified
numerically against the CPU solver and the same analytic oracles
`flats/FlatsSelfTest.cpp` holds the CPU version to (`--flats-gpu-check`), and
wired into the interactive path: `ui/MacPaintUI.cpp`'s `drawUI` passes its
live `GpuContext&` into `flatsEvaluateLayer`/`flatsEvaluateSource`, which
thread an optional `GpuContext*` (default `nullptr`) down through
`flatEvaluate` → `flatSagSegment` (`flats/Sag.cpp`) to pick `membraneSagGpu`
over `flatMembraneSag`. Every headless caller (`--batch`, `--psd-export`, the
golden harness, `flatstest`) keeps the CPU path unchanged — the parameter
defaults to null, and `flats/Sag.cpp` calls the GPU solver through a
function-pointer slot (`flatsSetMembraneSagGpu`, registered once from
`main.cpp` after `gpu.init()`) rather than naming `membraneSagGpu` directly,
so `flatstest` (deliberately GPU-free) has zero link dependency on
`flats/MembraneGpu.cpp`.

Measured speedup (`--profile-flats-solver`, production defaults
cycles=30/tol=1e-2):

| canvas | Mac (M4 Max) CPU → GPU | iPad Pro (M5) CPU → GPU |
|---|---|---|
| 1024² | 77 → 43 ms (1.8x) | 653 → 150 ms (4.4x) |
| 2048² | 266 → 83 ms (3.2x) | 2518 → 454 ms (5.5x) |
| 4096×3072 | 720 → 187 ms (3.9x) | — |

The whole-canvas GPU solver allocates four `f32` storage buffers per
multigrid level (`u`, `b`, `e`, `free`), one level per halving down to ≤8×8
(`Membrane.cpp:36,272`). Total bytes across all levels is the finest level's
`4 × 4B × w × h` times a geometric series that converges to ~4/3 (each level
is ¼ the pixel count of the one above), i.e. **≈21.3 bytes/pixel** of the
finest level, ignoring the small per-workgroup reduction buffers
(`shaders/membrane_correct_reduce.wgsl`, `membrane_residual_reduce.wgsl`).

## Why plain overlapping-tile-and-stitch does not work

The sag field is a global elliptic solve, not a local one: `Membrane.hpp`'s
own header notes sag ranges from ~15px inside a tight sleeve to ~600px in an
open background, and sag magnitude in a region scales with **that region's
full extent**, not distance to the nearest ink. A tile solved independently
necessarily imposes a fake Dirichlet boundary (effectively "there is ink
here") at its own edge. For a room smaller than the tile+overlap this is
fine — the real ink is inside the overlap and the tile sees it. For an open
region **larger** than the tile+overlap (the 600px background case), the tile
cannot see how far the room actually extends past its overlap, and will
systematically under-compute sag near every tile edge. This is not a seam
artifact fixable by edge-blending; the tile is solving a smaller, different
problem than the true one. Making the overlap large enough to fix it means
overlap ≈ the size of the largest open region on the canvas, which defeats
the purpose of tiling. (This is also why the CPU solver needs multigrid
rather than plain relaxation: plain local iteration converges slowly on
exactly this large-scale/smooth error component.)

## The actual design: split by multigrid level, not by canvas region

The multigrid V-cycle already separates "cheap, global, long-range" from
"expensive, local, high-frequency" — that split just needs to become a tiling
boundary too:

1. **Run the coarse levels globally, unchanged.** They are already cheap
   (¼ the pixels each level up) and are what carries the long-range
   information — a coarse solve over the whole canvas is what makes "there is
   a 600px room here" visible in the first place. No tiling needed; this is
   already affordable at any canvas size that fits an *ink mask* in memory.
2. **Only tile the finest 1–2 levels** — the ones whose full-resolution
   buffers are what threatens GPU memory at large canvas sizes. Each tile's
   `membrane_smooth.wgsl` sweeps only need a **small halo** (enough for the
   5-point stencil over `NU=6` sweeps, tens of pixels at most, not
   room-sized), because the long-range correction already arrived via
   `membrane_prolong.wgsl` from the coarse solve. This is the standard
   multigrid + domain-decomposition combination, not a new algorithm — it
   applies the V-cycle's own coarse/fine split at the tile-memory boundary
   instead of only at the resolution boundary.
3. **The line-search reduction (`correct()`'s `<r,e>`/`<e,Ae>` in
   `Membrane.cpp`, ported to `membrane_correct_reduce.wgsl`) and the residual
   norm (`membrane_residual_reduce.wgsl`) already use a per-workgroup partial
   buffer read back and finished on the CPU** (to avoid float atomics, which
   are not reliably available across wgpu backends including iOS/Metal — see
   `MembraneGpu.cpp`'s design notes). Extending that reduction to sum across
   tiles as well as workgroups is mechanical: same idiom, one more level of
   summation, no new technique.
4. **Restriction/prolongation across a tile boundary at the fine level**
   needs each tile to read a few pixels from its neighbour's edge (or share a
   halo region) — this is the one genuinely new piece of bookkeeping tiling
   adds, since `membrane_restrict.wgsl`/`membrane_prolong.wgsl` currently
   assume one whole-canvas buffer per level.

## When this is worth building

Not now. Rough memory math at ~21.3 bytes/pixel of the finest level:

| canvas | finest-level solver memory |
|---|---|
| 4096×3072 (current largest profiled) | ~268 MB |
| 8192×8192 | ~1.4 GB |
| 16384×16384 (the adapter's own `maxTextureDimension2D`) | ~5.6 GB |

8192² is the rough point where this starts to matter, especially on iPad's
shared memory alongside everything else the app holds (PaintSim's solver
already budgets against a much smaller ceiling per
`app/selftest/SolverFootprint.cpp`). Current measured canvas sizes (2048²,
4096×3072) are nowhere near this. Revisit when either (a) real documents
start approaching 8K+ on a side, or (b) an iPad memory-pressure jetsam is
observed with a Flats layer active on a large document — whichever comes
first is the actual trigger, not a calendar date.

## Explicitly out of scope until then

- Any change to the CPU solver (`flats/Membrane.cpp`) — it stays the
  reference implementation and the thing `--flats-gpu-check` verifies against.
- Persisting/reusing GPU buffers across multiple `membraneSagGpu` calls
  (each call currently allocates fresh, matching the CPU function's own
  pure-function shape) — worth revisiting only if buffer allocation itself
  shows up as a measured cost, not assumed.
- Tiling the *coarse* levels — they are cheap at any canvas size that is
  buildable at all; only the finest level(s) are the memory-pressure risk.

## Record

*(empty — fill in once this is actually built and measured)*
