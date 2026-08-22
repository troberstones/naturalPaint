# naturalPaint

A painting and image-processing application for visual development and texture
work, built on WebGPU — distinguished by one thing the incumbents do not do:
**colour mixes as real pigment does.** Blue over yellow gives green, not grey,
because the pixels are Kubelka-Munk pigment quantities rather than RGB triples.

Painting and image processing are both first-class here; neither is a follow-on
to the other.

> **Status: early, and honest about it.** The fluid-simulation half is built and
> verified. The document/layer half is being built now. **They are not yet joined**
> — see [Where the project actually is](#where-the-project-actually-is) before
> forming expectations. macOS on Apple Silicon only.

---

## Contents

- [What it is](#what-it-is) · [How it works](#how-it-works) · [Build it](#build-it)
- [Run it](#run-it) · [Verify it](#verify-it) · [Where the project actually is](#where-the-project-actually-is)
- [The watercolour simulation](#the-watercolour-simulation) · [Repository layout](#repository-layout)
- [Documentation](#documentation) · [Licensing](#licensing)

---

## What it is

Photoshop is the incumbent for both target jobs, and fails at three specific
things this project sets out to fix.

**Its colour mixing is wrong.** Blending is RGB interpolation, so blue over
yellow goes grey rather than green. The Mixer Brush is a partial patch over a
model-level problem. *"Digital colour mixing goes muddy"* is a standing complaint
in visual development.

**It is heavy.** Eager loading means gigabytes at rest and slow cold start,
irrespective of what the user is doing. Here, *lightweight* is a property of
resource behaviour rather than feature count: nothing allocates until it is used,
so a feature-rich application can still start fast and idle near zero. The
measured idle figure is **63.4 MB RSS** against an 80 MB ceiling that
`--selftest` enforces on every run.

**It composites in gamma space by default.** Blurs, gradients, resamples and
composites are therefore subtly incorrect — the reason its default blurs look
muddy. Every operation here is defined in a linear-light working space.

Two users, both first-class: preparing photos and textures for 3D work (inspect,
grade, clone out defects, remove lighting gradients, make tileable, export at
depth), and mark-making for visual development (block in, build up colour, paint
for hours with predictable, responsive brushes).

---

## How it works

### The one idea: pigment lives in a latent space, not RGB

This is the design decision everything else rests on.

Mixbox (Sochorová & Jamriška, SIGGRAPH Asia 2021) decomposes an RGB colour into a
**latent** — three pigment weights plus an additive residual — chosen so that
*linear combinations of latents are Kubelka-Munk mixes*.

Advection, diffusion, deposition, blurs, resamples and layer blends are all
linear combinations. So if pixels store latents rather than RGB, all of those
operations get physically-correct pigment mixing **for free**. A wet blue stroke
dragged through a wet yellow one gives green, not the grey an RGB solver
produces. Conversion to RGB happens once, at the end, in `composite.wgsl`.

Latents are always stored premultiplied by **mass** — the quantity of pigment at
a point — so varying concentration mixes correctly:

```
pigC = (c0·m, c1·m, c2·m, m)      pigR = (res.r·m, res.g·m, res.b·m, —)
```

`latentFromMass()` normalises on read. The invariant that follows, and that the
whole design depends on: **linear operations are valid on latents; non-linear
ones require a bake to working space first.** Blur is a linear combination, so it
works directly on latents. Curves are not, so they need a bake.

A **pigment basis** records which pigment model produced a latent — `mixbox-v1`
today, `km2-v1` for the unencumbered fallback. Bases are mutually unreadable and
silently so, which is why every saved document stamps its own.

### The layer model

Seven kinds. The three raster kinds are all paintable and are named for the
property that actually differs between them — **how colour combines.** The other
four hold no pixels of their own and are evaluated from parameters.

| Kind | Pixels are | Built? |
|---|---|---|
| **Pigment** | Latents — colour combines under Kubelka-Munk. The default for a new layer. | ✅ tile storage, f16 |
| **RGB** | Working-space RGBA — ordinary interpolation. Imports, image processing, deliberate RGB painting. | ✅ tile storage |
| **Media** | A Pigment layer advanced by the fluid solver. The simulated one. | ⬜ placeholder — phase 11 |
| **Adjustment** | none — an op stack applied to the composite beneath it | ⬜ placeholder |
| **Strokes** | none — an ordered list of clone/heal dabs, replayed against the composite below | ⬜ placeholder |
| **Text** | none — a string, a font reference, layout parameters | ⬜ placeholder |
| **Flats** | none — segmentation parameters plus recorded repairs, evaluated against line art | ⬜ placeholder |

"Design for N, ship 1": all seven are constructible today, but only Pigment and
RGB own pixel storage. The parametric four structurally never will — they will
gain parameter members, not tiles.

**Blend modes** are the linear-safe set — `Normal` (which is `over`), `Plus`,
`Multiply`, `Min`, `Max` — plus **`Mix`**, the Kubelka-Munk latent lerp that makes
two pigment layers combine as paint rather than as composited images. Each mode
carries its blend space as *data*, so `Screen` is present but labelled
**display-referred**: its arithmetic presupposes an encoding where 1.0 is white,
which is not true of the working space. Labelling that is the point — a mode whose
space is wrong should say so rather than be silently applied in linear light.

### Documents are sparse tiles, allocated lazily

A layer holds tiles; tiles exist only where content does. Nothing about an empty
document costs memory, which is the mechanism behind the *lightweight* claim
rather than an aspiration attached to it. Tiles can spill to `mmap` scratch, and
a recovery journal records the model rather than the pixels — see
[ADR-0008](docs/adr/0008-recovery-needs-a-model-journal-not-tile-spill.md).

### The colour pipeline

Everything is defined in **working space**: linear-light RGBA, sRGB/Rec.709
primaries by default, `rgba16float`. Files decode into it on import and encode
out of it on export.

Colour operations collapse to a **shaper plus a 3-D LUT**
([ADR-0004](docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)): the
shaper is a 1-D log encoding applied before the LUT so linear values above 1.0
fit its [0,1] domain and grading control points land where a user expects them.

### Media layers: three fluid solvers

| Mode | Model | Passes |
|---|---|---|
| **Watercolour** | Curtis et al. 1997 | shallow water + pigment + capillary layer |
| **Oil** | Baxter et al. 2004 (IMPaSTo) | height field + brush-canvas transfer + impasto |
| **Ink** | Chu & Tai 2005 (MoXi) | D2Q9 lattice Boltzmann percolation |

All three transport pigment as latent × mass, so Kubelka-Munk mixing holds in
every medium — blue over yellow gives green in oil and ink too.

Switching medium clears the canvas: the shared field textures are read
differently by each model, so a wash is not a valid initial state for a slab of
oil. `water` is `(u, v, p, M)` for watercolour and `(u, v, volume, contact)` for
oil — both a velocity field plus a scalar height and a coverage mask.

**Oil** keeps paint on the brush as well as the canvas (a 64×64 brush grid), so it
runs dry as you paint and picks colour up off wet paint. Transfer follows IMPaSTo
Algorithm 1 verbatim, including the equal-paint and velocity cutoffs that stop the
pair oscillating and stop the brush oozing when held still.

**Ink** runs a real D2Q9 lattice with half-way bounce-back, where the blocking
factor is the average of the two linked sites — that symmetry is what conserves
density and momentum. Paper grain, glue and already-deposited ink all feed the
blocking factor, and deposited ink feeding back into permeability is what pins a
mark's boundary. Surface-to-fibre supply is tracked explicitly
(`phi = clamp(s, 0, pi - rho)`), which MoXi singles out as the thing Curtis's
model lacks.

### The stack it runs on

| Piece | Choice | Why |
|---|---|---|
| Language | C++20 | Dear ImGui is a C++ library, and every paper worth porting ships C++ reference code |
| GPU | WebGPU via **wgpu-native** | One WGSL compute path across Metal / Vulkan / D3D12 |
| Window / input | **SDL3** | `SDL_PenEvent` gives real tablet pressure and tilt with no per-platform code |
| UI | **Dear ImGui** | MacPaint chrome, dark |
| Build | CMake — FetchContent for SDL3/ImGui, one submodule, one vendored binary | No package manager |

**Why not Dawn.** Dawn is the reference WebGPU implementation and has better WGSL
diagnostics, plus a first-party C++ wrapper. But it is packaged for Chromium, not
for outside consumers: a shallow clone plus dependencies came to **4.9 GB** —
4.4 GB of git objects and a 431 MB Tint conformance corpus — to produce a library
that compiles to ~40 MB. wgpu-native is a 13 MB prebuilt zip. The cost is the C
API instead of `webgpu_cpp.h`, which is why the handles here are raw `WGPU*` types
with no RAII: every GPU object lives for the life of the app, so there is nothing
for RAII to manage.

⚠️ **wgpu-native is pinned to v25.0.2.2 on purpose.** v27 renamed
`WGPUProgrammableStageDescriptor` and merged two
`WGPUSurfaceGetCurrentTextureStatus` enums, and Dear ImGui's WebGPU backend has
not caught up. v25 is the newest release whose header satisfies both. Revisit when
the backend updates.

---

## Build it

### Requirements

| | |
|---|---|
| **OS** | macOS on **Apple Silicon**, and only that — see below |
| **Toolchain** | Xcode Command Line Tools (Clang with C++20), CMake **≥ 3.24**, Git |
| **OpenImageIO** | **required** — backs the native `.npaint` format, so the build refuses without it |
| **Network** | needed at configure time — CMake fetches SDL3 and Dear ImGui |
| **Disk** | ~350 MB for the clone, fetched dependencies and build tree |

**OpenImageIO** is not available as a plain `brew install` here: the bottle
hard-requires ffmpeg and Python, which this project deliberately avoids. Build it
from source with the heavy plugins disabled and install it somewhere local — this
project's own copy lives at `~/.local/openimageio` and is built with
`USE_PYTHON=0`, `ENABLE_FFmpeg=0`, `ENABLE_LibRaw=0` and the rest of the optional
readers off. Its light dependencies (`imath`, `openexr`, `opencolorio`,
`robin-map`) do come from Homebrew. Point `CMAKE_PREFIX_PATH` at the install
prefix when configuring, below.

⚠️ **macOS/arm64 only, in two independent ways.** `src/gfx/Context.cpp` wires up
surface creation for Metal alone and hits a hard `#error` on any other platform
(the Windows `HWND` and Linux Xlib/Wayland entry points are named in comments but
not written). Separately, the vendored `third_party/wgpu/lib/libwgpu_native.a` is
an `arm64` binary. Everything above the surface layer is portable; nothing else
about the port is done.

### Clone

⚠️ **Use `--recurse-submodules`.** The Mixbox pigment LUT lives in the
`third_party/mixbox` submodule and is loaded at startup from the source tree. A
plain `git clone` compiles and links perfectly, then fails at launch with
`Could not load the Mixbox LUT.`

```bash
git clone --recurse-submodules https://github.com/troberstones/naturalPaint.git
```

Already cloned without it:

```bash
git submodule update --init --recursive
```

wgpu-native does **not** need fetching — it is vendored in `third_party/wgpu` and
tracked in the repository.

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio" && cmake --build build -j
```

The first configure takes about a minute (it shallow-clones SDL3 and Dear ImGui);
the build is a couple of minutes more. `RelWithDebInfo` is the default if you omit
`CMAKE_BUILD_TYPE` — a Debug build will not keep up with the solver.

`CMAKE_PREFIX_PATH` is **not** optional: OpenImageIO is a required dependency and
`find_package` will not locate a non-standard install on its own. Point it at
wherever your OpenImageIO lives; see [Build options](#build-options) for why the
dependency is required, and [Requirements](#requirements) above for building it.

### Build options

| Option | Default | What it does |
|---|---|---|
| `NP_USE_MIXBOX` | `ON` | Use the Mixbox pigment LUT. **CC BY-NC — non-commercial only.** ⚠️ The `OFF` path is **not implemented** — see below. |

**OpenImageIO is a required dependency**, not a build option. It used to be
`NP_USE_OIIO`, defaulting `OFF`, on the reasoning that format support is a
*runtime capability query* rather than a build-time requirement — which is still
true of EXR, TIFF, HDR and DPX, and PNG/JPEG/TGA/BMP are stb-backed regardless.

What that argument missed is that `io/NpaintFile` builds the **native** `.npaint`
format on multi-part EXR. The same guard that made foreign formats optional also
made `saveNpaint()` and `loadNpaint()` refuse, and gated the crash-recovery
journal with them, so the default build could not save a document, open one, or
recover from a crash. A foreign format you cannot read is a capability gap; a
document you cannot save is not an application.

`find_package` will not locate a non-standard install, so configuring needs a
`CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio"
```

Passing `-DNP_USE_OIIO=OFF` is refused at configure time with a message naming
the fix, rather than being silently ignored now that the option is gone.

> ⚠️ **`NP_USE_MIXBOX=OFF` does not do what this table used to say.** No source
> file reads `NP_USE_MIXBOX` — the only occurrences are CMake setting the define
> and a comment noting the option "had rotted from never being built" — and
> `NP_MIXBOX_LUT` is defined unconditionally. Building with the option `OFF`
> still produces a binary that requires the CC BY-NC Mixbox LUT and dies at
> startup without it. The two-constant Kubelka-Munk fallback was planned and
> never written. Treat this project as CC BY-NC-encumbered until it is.
> An unexercised build option is not a seam, and this is what one rots into.

---

## Run it

```bash
./build/src/naturalPaint
```

⚠️ **The binary is not relocatable.** Shaders, the pigment LUT and the keymap are
read from the source tree at runtime via absolute paths baked in at compile time
(`NP_SHADER_DIR`, `NP_MIXBOX_LUT`, `NP_KEYMAP_DIR`). That is deliberate — it is
what lets you edit any `.wgsl` and hit **⌘R** to recompile the solver without
restarting, and hand-edit `keymaps/default.json`. It also means moving the
executable away from the checkout breaks it.

### Command-line modes

Each runs headless — a window is created but the UI never comes up — and each
verifies a different thing.

```bash
./build/src/naturalPaint --selftest [out.png]   # correctness gate; exit 0 on pass
./build/src/naturalPaint --diag [seconds]       # where the pigment goes over time
./build/src/naturalPaint --modes                # one stroke in each of the three media
./build/src/naturalPaint --latency              # per-sample input latency, not just the summary
```

`--diag` lays one wet blob and reports suspended/deposited mass, wet-cell and
pigment-cell counts, and flow speed every 2 s. Mass should be flat once the brush
lifts.

`--modes` paints one stroke per medium and writes `mode_Watercolour.png`,
`mode_Oil.png` and `mode_Ink.png`.

### Controls

| | |
|---|---|
| Left drag | paint |
| Middle drag, or Hand tool | pan |
| Wheel | zoom |
| Space | pause solver |
| ⌘K / ⌘N | clear canvas |
| ⌘R | reload shaders |

The full keymap — which matches Photoshop wherever Photoshop has an assignment,
because muscle memory is the largest switching cost — is
[docs/shortcuts.md](docs/shortcuts.md), and is remappable in
`keymaps/default.json`.

Painting tools are limited to the ones that mean something for a fluid canvas:
Brush, Water (pre-wet the sheet), Dry Brush, Eyedropper, Hand, Zoom. Lasso,
marquee and text belong to MacPaint's palette but have no implementation yet, so
they are absent rather than present and dead.

Pigment selection drives the *physical* constants too — density, staining and
granulation come from the selected paint, so Phthalo Blue (staining, no
granulation) behaves differently from Ultramarine (granulating, lifts easily).

---

## Verify it

`--selftest` is the project's correctness gate, and it is substantial: **1117
assertions across 34 sections in about 5 seconds**, exit code 0 on pass.

```bash
./build/src/naturalPaint --selftest out.png
```

It covers the pigment claim (paint Hansa Yellow, drag Phthalo Blue through it,
sample the overlap, fail loudly if it is not green) and then well beyond it —
colour-space round trips, the shaper's branch continuity, fixed-timestep
accumulator behaviour under stall, tile store and residency, image decode and
export, the `.npaint` format, document lifecycle, the recovery journal, blend
modes, the layer stack, pigment layers, layer masks, op stacks, LUT baking, curve
editing, histograms, point ops, view transform, guides and snapping, idle RSS
against its ceiling, and format-support answers for the compiled-in backend set.

Two properties worth calling out, because they are the ones that would silently
rot:

- **Stroke mass is speed-independent.** A fast and a slow stroke over an identical
  path deposit the same pigment to within 0.0%, well inside the 5% tolerance —
  the arc-length dab emitter working as specified
  ([ADR-0003](docs/adr/0003-deposition-is-per-dab-not-per-frame.md)).
- **Idle RSS stays under its ceiling.** Currently 63.4 MB against 80 MB, asserted
  before any heavy subsystem is constructed.

---

## Where the project actually is

**The two halves of the application are not yet joined.** This is the single most
important thing to understand about the current state, and it is easy to miss
because both halves work.

- `sim::PaintSim` — the verified fluid canvas — owns **one dense texture with no
  layer awareness**.
- `core::Document` — the layer model, tiles, blending, save/load — is populated by
  File ▸ Open and New Document, and every document operation acts on it correctly.

But a brush stroke reaches no `Layer`, and saving a document writes what was
*opened* rather than what is *on screen*. Building that bridge is the top blocker,
and it is what turns the solver into a **Media layer** (phase 11).

### Built and verified

- **Latent-space pigment mixing** — the load-bearing claim; `--selftest` asserts it
- **Mass conservation** — exact on a level board; 0.13% rounding loss during
  transport on a tilted one, flat once the canvas dries
- **Edge darkening, capillary flow, granulation** — washes strand pigment at the
  rim, stroke edges feather along the grain, pigment pools in the paper's valleys
- **Oil** — paint runs out, picks colour up off the canvas, and lights as impasto
- **Ink** — the lattice percolates and marks bleed into the fibres
- **The imaging half** — linear working space, shaper + 3-D LUT grading, point ops,
  curves, histograms, image decode/encode, Export As presets, the `.npaint`
  format, document lifecycle, recovery journal
- **The layer half so far** — multiple layers with reorder/visibility/lock/opacity,
  the blend set plus `Mix`, Pigment and RGB layers with tile storage, layer masks
- Shader hot-reload, fixed timestep, lazy per-mode allocation

### Next

Phases 1–4 are complete and phase 5 ("Stack it") is in progress — layer masks,
adjustment layers, copy-on-write tiles, history with a cursor (undo *and* redo),
clipping masks, the merge family, layer comps, and native save/load carrying
layers and latents. After that: filters and transforms, selections, and then the
brush engine and Media layers. [PLAN.md](PLAN.md) has the ordered steps, each with
its own verification.

### Known limitations

1. **`--diag` labels are watercolour-centric.** In oil, "deposited" is always zero
   because paint drying is not implemented, and "activeCells" counts brush contact
   rather than coverage. The numbers are right; the column headings lie.
2. **Ink bleed is present but tight.** Recognisably sumi, but short of the dramatic
   feathering of the paper's figures. More lattice steps widen it at a linear cost.
3. **Not implemented:** paint drying/layering for oil, blooms and backruns,
   Windows and Linux surface creation, the lift/sponge tool.

---

## The watercolour simulation

Layered exactly as Curtis et al. 1997, §4:

```
splat              brush wets the paper and injects pigment
├─ MoveWater       §4.1
│  update_velocities   semi-Lagrangian self-advection (Stam '99) + paper slope
│                      + pressure gradient + viscosity + drag
│  divergence      ∇·u
│  jacobi ×N       pressure-correction Poisson solve
│  project         u -= ∇p   (velocity only — never touch water.z)
│  flow_outward    p -= η(1 - blur(M))   ← this term is the dark stroke edge
│  advect_water    the wet layer itself flows, so a tilted board runs
├─ MovePigment     §4.2
│  advect_pigment  conservative donor-cell transport + diffusion
│  transfer_pigment deposition / lifting, per-pigment ρ ω γ
└─ CapillaryFlow   §4.3
   capillary_flow  wicking through the fibres; saturated cells re-wet the
                   surface, which is what produces blooms and backruns
composite          latent → RGB, Beer-Lambert build-up over the paper
```

### Field layout

| Texture | Contents |
|---|---|
| `water` | `(u, v, p, M)` — the whole shallow-water state in one texture, so the Jacobi loop ping-pongs a single target |
| `pigC` / `pigR` | suspended pigment latent × mass |
| `depC` / `depR` | deposited pigment latent × mass |
| `sat` | capillary-layer saturation `s` |
| `aux` | `(∇·u, δp)` — solver scratch |
| `paper` | `(height, capacity)`, generated once by `paper.wgsl` |

The four pigment fields are `rgba32float`; water, saturation and solver scratch
stay f16. That is not a precision preference — see the
[solver log](docs/solver-log.md) for the measurement that forced it.

### Conservative advection

Pigment advection is the donor-cell flux scheme from IMPaSTo §4.1.1, shared by all
three media. Each *donor* cell splits its contents over at most four destinations
with weights summing to exactly 1, so mass is conserved by construction whatever
the velocity field does. It replaced a semi-Lagrangian resample — a gather with no
such guarantee, measured at +132% pigment mass over 20 s. After the change:
**112969.4 at every sample across 20 s, constant to the decimal** on a level
board. Adding `advect_water` for board tilt costs 0.13% to f32 rounding while
paint is moving.

`advect_pigment` does its own bilinear fetch rather than using a sampler, so the
`rgba32float` pigment textures never need the optional `float32-filterable`
feature.

### Working time

How long a wash keeps bleeding before it sets is exposed as **one slider in
seconds**, because on its own the wet lifetime is an emergent product of three
parameters and none of them means much alone. `setWorkingTime()` derives both
drain paths from it:

```cpp
p.evaporation = 0.06f / t;
p.absorbRate  = 3.75f / t;
```

Scaling evaporation alone was tried first and left the mapping compressing at the
long end because absorption kept draining at a fixed rate. Scaling the whole
drying process keeps it linear, and matches the physical reading — paper that
stays workable longer is absorbing more slowly too. `absorbRate` and `evaporation`
are no longer exposed separately; letting them drift out of step only makes timing
unpredictable.

It is capped at 20 s on purpose: past that the wash spreads thin enough that
capillary diffusion dilutes saturation below `wetThreshold` before evaporation
can, so the wet lifetime saturates near 19 s whatever the drying rate.

> ⚠️ The measured calibration table (good to ~15% across the slider's range) was
> taken pre-1.3 and has not been re-measured since deposition lost its `* P.dt`
> scaling. See the [solver log](docs/solver-log.md).

### Board tilt

Watercolourists tilt the board to make a wash run, so `update_velocities` carries
a gravity term. It scales with film depth, which is what makes it behave like the
technique rather than a global scroll: a standing puddle streaks downhill while
merely damp paper stays put, so you tilt while it is wet and lay the board flat
once it has soaked in. The UI is a pad you drag rather than two numbers — the dot
is the low corner of the board, distance from centre is steepness, double-click
levels it.

Tilt alone was not enough: with only the gravity term the centre of mass moved
downhill but the mark kept its outline, because water depth was never advected.
`advect_water` now transports the shallow-water layer with the same conservative
donor-cell scheme, so the wet region itself runs and a mark can grow a tail. Two
consequences worth knowing:

- Adding water transport *reduced* run distance at fixed tilt (19.8 px → 10.0 px).
  Spreading thins the film, and gravity scales with depth, so a run damps itself.
  Physically right; tilt just needs more headroom than it first appeared.
- Re-asserting the wet mask with a hard `depth > 0.02` step crenellated the leading
  edge into blocky teeth. It is a `smoothstep` now.

Oil and ink ignore tilt: oil is far too viscous to run, and gravity in the lattice
model would need a proper LBE body force rather than this term.

### How it was debugged

The measurements behind the current parameter values — the edge-darkening bug that
took four attempts to find, the two cancelling mass-conservation bugs, the hollow
washes, the ink that would not bleed, and the three hypotheses that were wrong and
should not be re-tested — are recorded in
**[docs/solver-log.md](docs/solver-log.md)**, so the landing page does not have to
carry them.

---

## Repository layout

```
src/
  core/      Document, Layer, Tile, TileStore, Pigment, Blend, Composite,
             LayerOps, OpStack, Histogram, Probe — the domain model
  sim/       PaintSim — the three fluid solvers
  gfx/       WebGPU context and shader loading (Metal surface only)
  io/        image decode/encode, Export As, the .npaint format,
             tile residency, the OpenImageIO backend
  color/     working space, shaper, 3-D LUT baking
  ops/       point ops, resampling
  app/       document lifecycle, recovery journal, keymap, latency,
             memory, view transform, snapping, SelfTest
  ui/        Dear ImGui chrome
  brush/     arc-length stroke path and dab emission
  paint/     the pigment palette and its physical constants
shaders/     WGSL — read from the source tree at runtime, ⌘R to reload
keymaps/     default.json — hand-editable
docs/        format, operations, UI, shortcuts, ADRs, solver log
third_party/ mixbox (submodule), wgpu (vendored binary), stb
```

## Documentation

| | |
|---|---|
| [PRD.md](PRD.md) | What must be true, and how it will be verified |
| [PLAN.md](PLAN.md) | Ordered, executable steps — each with its own verification |
| [CONTEXT.md](CONTEXT.md) | The domain language. Read this before naming anything |
| [DESIGN-imaging.md](DESIGN-imaging.md) | Mechanism |
| [docs/adr/](docs/adr/) | Settled decisions, with the reasoning that settled them |
| [docs/document-format.md](docs/document-format.md) | The `.npaint` format |
| [docs/operations.md](docs/operations.md) | Every op, its evaluation class and its cost |
| [docs/ui.md](docs/ui.md) · [docs/shortcuts.md](docs/shortcuts.md) | Interface and keymap |
| [docs/solver-log.md](docs/solver-log.md) | How the simulation was debugged, and what was measured |

Where the PRD and the design doc disagree about *mechanism*, the design doc wins;
where they disagree about *requirements*, the PRD does.

## Licensing

⚠️ **Mixbox is CC BY-NC — non-commercial use only.** `third_party/mixbox` and the
WGSL port in `shaders/include/mixbox.wgsl` carry that licence. Commercial use
requires a licence from Secret Weapons (mixbox@scrtwpns.com), or replacing the
latent model with a plain two-constant Kubelka-Munk mix. The `NP_USE_MIXBOX` CMake
option exists as the seam for that swap, and building with `-DNP_USE_MIXBOX=OFF`
takes the unencumbered path.

Everything else: SDL3 (Zlib), Dear ImGui (MIT), wgpu-native (MIT/Apache-2.0),
stb_image (public domain), OpenImageIO (Apache-2.0, required).

## Papers

Kept on disk in `papers/` and deliberately untracked — the design docs cite them
by author and year, but redistributing publisher copies is not ours to do. The
load-bearing ones are Curtis '97 (the whole watercolour model), Stam '99
(advection and projection), Baxter '04 (IMPaSTo — oil, and the conservative
advection scheme all three media share), Chu & Tai '05 (MoXi — ink), and
Sochorová & Jamriška '21 (Mixbox — pigment mixing). Bridson's course notes are the
best practical reference for the fluid half.
