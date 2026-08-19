# naturalPaint

Real-time natural-media painting on WebGPU: watercolour, oil and ink.

## Stack

| Piece | Choice | Why |
|---|---|---|
| Language | C++20 | Dear ImGui is a C++ library, and every paper worth porting ships C++ reference code |
| GPU | WebGPU via **wgpu-native** | One WGSL compute path across Metal / Vulkan / D3D12 |
| Window / input | **SDL3** | `SDL_PenEvent` gives real tablet pressure and tilt with no per-platform code |
| UI | **Dear ImGui** | MacPaint chrome, dark |
| Build | CMake + FetchContent | No submodules, no package manager |

**Why not Dawn.** Dawn is the reference WebGPU implementation and has better WGSL
diagnostics, plus a first-party C++ wrapper. But it is packaged for Chromium, not
for outside consumers: a shallow clone plus dependencies came to **4.9 GB** —
4.4 GB of git objects and a 431 MB Tint conformance corpus — to produce a library
that compiles to ~40 MB. wgpu-native is a 13 MB prebuilt zip. The cost is the C
API instead of `webgpu_cpp.h`, which is why the handles here are raw `WGPU*` types
with no RAII: every GPU object lives for the life of the app, so there is nothing
for RAII to manage.

⚠️ **wgpu-native is pinned to v25.0.2.2 on purpose.** v27 renamed
`WGPUProgrammableStageDescriptor` and merged two `WGPUSurfaceGetCurrentTextureStatus`
enums, and Dear ImGui's WebGPU backend has not caught up. v25 is the newest release
whose header satisfies both. Revisit when the backend updates.

## Three media

| Mode | Model | Passes |
|---|---|---|
| **Watercolour** | Curtis et al. 1997 | shallow water + pigment + capillary layer |
| **Oil** | Baxter et al. 2004 (IMPaSTo) | height field + brush-canvas transfer + impasto |
| **Ink** | Chu & Tai 2005 (MoXi) | D2Q9 lattice Boltzmann percolation |

Switching medium clears the canvas: the shared field textures are read
differently by each model, so a wash is not a valid initial state for a slab of
oil. `water` is `(u, v, p, M)` for watercolour and `(u, v, volume, contact)` for
oil — both are a velocity field plus a scalar height and a coverage mask.

**Oil** keeps paint on the brush as well as the canvas (a 64x64 brush grid), so
it runs dry as you paint and picks colour up off wet paint. Transfer follows
IMPaSTo Algorithm 1 verbatim, including the equal-paint and velocity cutoffs
that stop the pair oscillating and stop the brush oozing when held still.

**Ink** runs a real D2Q9 lattice with half-way bounce-back, where the blocking
factor is the average of the two linked sites — that symmetry is what conserves
density and momentum. Paper grain, glue and already-deposited ink all feed the
blocking factor, and deposited ink feeding back into permeability is what pins a
mark's boundary. Surface-to-fibre supply is tracked explicitly (`phi = clamp(s,
0, pi - rho)`), which MoXi singles out as the thing Curtis's model lacks.

All three transport pigment as Mixbox latent-times-mass, so Kubelka-Munk mixing
holds in every medium — blue over yellow gives green in oil and ink too.

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

### Pigment lives in Mixbox latent space, not RGB

This is the one non-obvious design decision.

Mixbox (Sochorová & Jamriška, SIGGRAPH Asia 2021) decomposes an RGB colour into
a latent of three pigment weights plus an additive residual, chosen so that
**linear combinations of latents are Kubelka-Munk mixes**.

Advection, diffusion, and deposition in a fluid solver are all linear
interpolations. So if the pigment field stores latents rather than RGB, the
solver gets physically-correct pigment mixing for free — a wet blue stroke
dragged through a wet yellow one gives green, not the grey an RGB solver
produces. Conversion to RGB happens once, in `composite.wgsl`.

Pigment is stored premultiplied by mass across two `rgba32float` textures:

```
pigC = (c0·m, c1·m, c2·m, m)      pigR = (res.r·m, res.g·m, res.b·m, —)
```

so varying concentration mixes correctly; `latentFromMass()` normalises on read.

### Field layout

| Texture | Contents |
|---|---|
| `water` | `(u, v, p, M)` — the whole shallow-water state in one texture, so the Jacobi loop ping-pongs a single target |
| `pigC` / `pigR` | suspended pigment latent × mass |
| `depC` / `depR` | deposited pigment latent × mass |
| `sat` | capillary-layer saturation `s` |
| `aux` | `(∇·u, δp)` — solver scratch |
| `paper` | `(height, capacity)`, generated once by `paper.wgsl` |

## Status

Verified working:

- **Latent-space pigment mixing.** Blue crossing yellow gives green, not grey.
  This is the load-bearing claim of the design and `--selftest` asserts it.
- **Mass conservation.** Exact while the board is level; 0.13% rounding loss
  during transport on a tilted board, flat once the canvas dries.
- **Edge darkening.** Washes strand pigment at the rim.
- **Capillary flow.** Stroke edges feather along the paper grain.
- **Granulation.** Pigment visibly pools in the paper's valleys.
- **Oil**: paint runs out, picks colour up off the canvas, and lights as impasto.
- **Ink**: the lattice percolates and marks bleed into the fibres.
- Paper substrate, brush transport, deposition, all three pass pipelines, and
  shader hot-reload.

### Conservative advection

Pigment advection is the donor-cell flux scheme from IMPaSTo §4.1.1, shared by
all three media. Each *donor* cell splits its contents over at most four
destinations with weights summing to exactly 1, so mass is conserved by
construction whatever the velocity field does. It replaced a semi-Lagrangian
resample, which is a gather with no such guarantee and measured +132% pigment
mass over 20 s. Measured after the change: **112969.4 at every sample across
20 s, constant to the decimal** on a level board. Adding `advect_water` for
board tilt costs 0.13% to f32 rounding while paint is moving; see Board tilt.

### Fixed

Run `--diag` to reproduce any of this. It lays one wet blob and reports pigment
mass, wet area, and flow speed over 20 s.

**The spreading wash used to run clear at its leading edge.** `pigCells /
wetCells` decayed monotonically — 1.07 → 0.81 — because water spreads via a
dedicated capillary diffusion pass while pigment moved *only* by advection, so
the wet front always outran the pigment. `advect_pigment` now carries a diffusion
term (`pigmentDiffuse`), using the symmetric pair weight `min(M_here,
M_neighbour)` so the exchange is conservative and falls to zero at the edge of
the wet region. The ratio now holds at **0.98**.

**Pigment mass was being destroyed.** Two independent bugs that partially
cancelled, which is why the net number looked merely "wrong" rather than
obviously two things. Bisected by neutering passes:

| config | 20 s mass change |
|---|---|
| f16, full pipeline | −35 % |
| f16, advection disabled | −45 % |
| f16, advection **and** transfer disabled | 0.0 % (exact) |
| f32, advection disabled | 0.0 % (exact) |
| f32, full pipeline | +132 % |

- `transfer_pigment` is algebraically conservative — `pigC + depC` is invariant —
  but each side was rounded to `rgba16float` independently 240×/second. **Fixed:**
  the four pigment fields are now `rgba32float`, where the same exchange conserves
  to the decimal. Water, saturation and solver scratch stay f16.
- `update_velocities` and `project` ended with `vel *= water.w`. Scaling a freshly
  divergence-free field by a fractional mask makes it divergent again exactly at
  the stroke boundary, and semi-Lagrangian advection duplicates pigment there.
  **Fixed:** removed; the mask is already a Dirichlet condition on `dp` in
  `jacobi.wgsl`, which is where it belongs. Cut mass creation from +132 % to +49 %.

`advect_pigment` no longer uses a sampler — it does its own bilinear fetch, so
the `rgba32float` pigment textures never need the optional `float32-filterable`
feature.

### Edge darkening — found, after four attempts

`project.wgsl` folded the pressure correction into `water.z`:

```wgsl
textureStore(waterDst, p, vec4<f32>(vel, water.z + dp, water.w));  // wrong
```

That channel is the physical **water depth** — `splat` adds to it,
`capillary_flow` absorbs from it, `flow_outward` lowers it at the rim. `dp` is a
pressure *correction* for the velocity solve only. Folding it in drove the depth
to zero within a single Jacobi iteration.

The measurement that found it, with the paper-slope force disabled so only
pressure-driven flow remains:

| `jacobiIterations` | mean speed (before) | mean speed (after) |
|---|---|---|
| 0 | 0.299 | 0.299 |
| 1 | **0.000** | 0.217 |
| 40 | **0.000** | 0.076 |

One iteration annihilating the flow is not what a projection does. With it fixed
the pressure gradient drives flow again, `FlowOutward` has something to act on,
and strokes get a proper dark rim.

This also explains the earlier symptom that `paperSlope` was the *only* thing
moving water: slope-driven flow has structure that survives projection, while
radial pressure-driven flow was exactly what was being destroyed.

Three earlier hypotheses were wrong and are recorded in `shaders/project.wgsl`
so they are not re-tested: the magnitude of the `FlowOutward` nudge (raised 50x,
no change), over-projection (relaxed to 0.55, no change), and vanishing velocity
(speed measured a healthy ~0.07 px/step).

### Diffusion: not the bug

Worth stating plainly, since it was the reported symptom. Comparing at t = 20 s:

| config | `pigCells/wetCells` | look |
|---|---|---|
| diffusion on | 1.00 | smooth, mottled by paper grain |
| diffusion off | 0.85 | sharp radial streaks and voids |
| granulation off | 1.00 | *identical* to granulation on |

Diffusion was **masking** a flow artifact, not causing one. Turning it off makes
the blotching worse, and granulation is not involved at all. The blotching traced
back to the `project.wgsl` bug above.

### Hollow washes — a missing ceiling, not a rate

`splat` added `brushWater * dt` to the water depth every frame with no cap, so
dwell time alone built a pressure head **~20x the paper's fibre capacity** (depth
~20 against a capacity ~0.9). The outward flow then never stopped and emptied the
wash into its own rim.

Surface tension limits how deep a film sits on paper before it runs, so depth is
now clamped to `maxFilm`. Swept first and ruled out: `edgeDarkening` (0.05 - 1.5,
barely moved it) and the deposition rates (`RATE_SCALE`). Both were the wrong
knob — the problem was an unbounded accumulation, not a rate.

`paperSlope` also came down 2.2 -> 0.9. It had been raised while the pressure
term was broken and was the only thing moving water; with pressure working it
over-channelled flow into the paper's valleys and left mottled voids.

### Ink that would not bleed

Two compounding causes, the first dominant:

- **Settle saturated.** `density * dt * (1 + 3exp(-8v)) * 2.5` reached **2.75 and
  clamped to 1.0** — every suspended particle fixed to the fibres on the first
  step. Ink that deposits instantly cannot travel, so no amount of loosening the
  lattice would have helped. Now scaled by `settleScale`.
- **Lattice pinned.** `kappa` summed to ~0.75 of a 0.98 maximum, bouncing three
  quarters of every streaming step straight back. Blocking 0.42 -> 0.10, grain
  0.45 -> 0.22, glue 0.10 -> 0.04.

Reach grows only as `sqrt(steps)` — the LBE spreads diffusively at
`nu = (1/omega - 1/2)/3`, so 2 substeps moved a mark ~5 px in a couple of seconds
against the ~25 px millimetre-scale bleed needs. Ink now has its own
`inkSubsteps` (8), affordable because its passes are cheap (4 dispatches, no
Poisson solve), and `omega` dropped 1.30 -> 0.70, raising `nu` from 0.09 to 0.31.

One consequence worth knowing: total ink accepted *drops* as percolation speeds
up. That is MoXi's receptivity mask `max(1 - rho/lambda, m)` working as designed —
wet paper takes less ink — not mass loss. `receptivity` was raised 0.65 -> 1.20 so
tone stays rich.

### Working time

How long a wash keeps bleeding before it sets is exposed as one slider in
seconds, because on its own the wet lifetime is an emergent product of three
parameters and none of them means much alone. `setWorkingTime()` derives both
drain paths from it:

```cpp
p.evaporation = 0.06f / t;
p.absorbRate  = 3.75f / t;
```

Scaling evaporation alone was tried first and left the mapping compressing at
the long end (25 s asked, 19.5 s measured) because absorption kept draining at a
fixed rate. Scaling the whole drying process keeps it linear, and matches the
physical reading — paper that stays workable longer is absorbing more slowly too.

Measured against `--diag`, which reports when the canvas actually goes dry:

> ⚠️ Measured pre-1.3. Water depth deposited per stroke also lost its `* P.dt`
> scaling (ADR-0003, same change as pigment) and roughly doubled along with
> it, which plausibly shifts how long a wash takes to fully evaporate at a
> fixed rate. Not yet re-measured — flagging rather than leaving it silently
> stale.

| set | measured |
|---|---|
| 2 s | 2.75 s |
| 5 s | 6.0 s |
| 15 s | 13.7 s |
| 20 s | 17.2 s |

Good to ~15% across the slider's range. It is capped at 20 s on purpose: past
that the wash spreads thin enough that capillary diffusion dilutes saturation
below `wetThreshold` before evaporation can, so the wet lifetime saturates near
19 s whatever the drying rate. `absorbRate` and `evaporation` are no longer
exposed separately — letting them drift out of step only makes timing
unpredictable. `Capillary diffuse` stays, since it sets how *far* a wash reaches
rather than how long it lasts.

### Board tilt

Watercolourists tilt the board to make a wash run, so `update_velocities` carries
a gravity term. It scales with film depth, which is what makes it behave like the
technique rather than a global scroll: a standing puddle streaks downhill while
merely damp paper stays put, so you tilt while it is wet and lay the board flat
once it has soaked in.

The UI is a pad you drag rather than two numbers — the dot is the low corner of
the board, distance from centre is steepness, double-click levels it.

**Tilt alone was not enough.** With only the gravity term the centre of mass
moved downhill but the mark kept its outline: pigment sloshed around inside a wet
region that never went anywhere. Water depth was never advected — `water.z` was
only ever changed by the splat, flow-outward and capillary passes. `advect_water`
now transports the shallow-water layer with the same conservative donor-cell
scheme, so the wet region itself runs and a mark can grow a tail.

Two things that fell out of that, both worth knowing:

- Adding water transport *reduced* the run distance at fixed tilt (19.8 px ->
  10.0 px). Spreading thins the film, and gravity scales with depth, so a run
  damps itself. Physically right; it just means tilt needs more headroom than it
  first appeared. `kMaxTilt` ended at 0.50 — the same value that produced a
  degenerate hollow sweep *before* water advection existed, and a proper
  teardrop after.
- Re-asserting the wet mask with a hard `depth > 0.02` step crenellated the
  leading edge into blocky teeth. It is a `smoothstep` now.

Measured deflection of deposited pigment, 20 s working time over a 40 s run:

> ⚠️ Also measured pre-1.3, same caveat as the working-time table above —
> deposited water/pigment magnitude shifted, position/timing dynamics likely
> did not, but this hasn't been re-verified.

| tilt | y-offset |
|---|---|
| 0 | −0.9 px |
| 0.10 | 29.4 px |
| 0.25 | 56.3 px |
| 0.50 | 77.1 px |

Sideways drift stays under 1 px throughout, so the force is on the axis it
should be. Oil and ink ignore tilt: oil is far too viscous to run, and gravity in
the lattice model would need a proper LBE body force rather than this term.

**One regression from the extra pass:** pigment conservation is no longer exact.
It loses 0.13% during the transport phase (112969.3 -> 112821.7) and then goes
completely flat once the canvas dries — the loss tracks motion, so it is f32
rounding across an extra donor gather rather than a leak.

### Known bugs

**1. `--diag` labels are watercolour-centric.** In oil, "deposited" is always
zero because paint drying is not implemented, and "activeCells" counts brush
contact rather than coverage. The numbers are right; the column headings lie.

**2. ~~Oil strokes still show faint periodic ridges.~~ Closed by 1.3 / ADR-0003.**
Deposition across all three media is now driven by an arc-length dab emitter
(`src/brush/StrokePath.*`) instead of one swept capsule per rendered frame, and
every deposition term had its `* P.dt` scaling removed — a dab now deposits a
fixed quantity per unit of brush travel, never per unit of time. For oil
specifically this replaces per-frame stamping (the actual cause of the ridges)
with stamping at a fixed spatial cadence. Confirmed: pigment mass is now
provably speed-independent (`--selftest`'s stroke-speed case matches fast vs.
slow strokes over an identical path to within 0.0%, well inside its 5%
tolerance). Not independently re-confirmed here: the *visual* ridge reduction
under impasto lighting specifically — worth an eyeball check next time an oil
stroke is painted interactively.

**3. Ink bleed is present but tight.** Recognisably sumi, but short of the
dramatic feathering of the paper's figures. More lattice steps widen it at a
linear cost.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
```

Configure takes ~2 minutes (cloning SDL3 and ImGui); the build is a couple of
minutes more. wgpu-native is vendored in `third_party/wgpu`.

```bash
./build/src/naturalPaint
```

Check the solver without touching the UI:

```bash
./build/src/naturalPaint --selftest out.png
```

It paints Hansa Yellow, drags Phthalo Blue through it, samples the overlap, and
fails loudly if the result is not green. Exit code 0 on pass.

Track where the pigment actually goes:

```bash
./build/src/naturalPaint --diag 20
```

Lays one wet blob, then reports suspended/deposited mass, wet-cell and
pigment-cell counts, and flow speed every 2 s. Mass should be flat once the brush
lifts; it is not, and the table under Known bugs is what this prints.

Shaders and the pigment LUT are read from the source tree at runtime — edit any
`.wgsl` and hit **Cmd+R** to recompile the solver without restarting.

## Controls

| | |
|---|---|
| Left drag | paint |
| Middle drag, or Hand tool | pan |
| Wheel | zoom |
| Space | pause solver |
| Cmd+K / Cmd+N | clear canvas |
| Cmd+R | reload shaders |

Tools are limited to the ones that mean something for a fluid canvas: Brush,
Water (pre-wet the sheet), Dry Brush, Eyedropper, Hand, Zoom. Lasso, marquee and
text belong to MacPaint's palette but have no implementation here, so they are
absent rather than present and dead.

Pigment selection drives the *physical* constants too — density, staining, and
granulation come from the selected paint, so Phthalo Blue (staining, no
granulation) behaves differently from Ultramarine (granulating, lifts easily).

## Licensing

⚠️ **Mixbox is CC BY-NC** — non-commercial use only. `third_party/mixbox` and the
WGSL port in `shaders/include/mixbox.wgsl` carry that licence. Commercial use
requires a licence from Secret Weapons (mixbox@scrtwpns.com), or replacing the
latent model with a plain two-constant Kubelka-Munk mix. The `NP_USE_MIXBOX`
CMake option exists as the seam for that swap.

Everything else: SDL3 (Zlib), Dear ImGui (MIT), Dawn (BSD-3), stb_image (public
domain).

## Papers

See `papers/`. The load-bearing ones are Curtis '97 (the whole model), Stam '99
(advection and projection), and Sochorová & Jamriška '21 (pigment mixing).
Bridson's course notes are the best practical reference for the fluid half.

## Not done yet

- Paint drying / layering for oil (IMPaSTo supports unlimited dry layers)
- Blooms and backruns
- Windows and Linux surface creation (`src/gfx/Context.cpp` has the entry point
  stubbed with the right struct names; everything above it is portable)
- Lift / sponge tool — needs `splat.wgsl` to bind the deposited layer
- Save / load, layers, undo
