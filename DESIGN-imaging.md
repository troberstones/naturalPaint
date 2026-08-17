# naturalPaint imaging core — design

Turning naturalPaint from a fluid-paint demo into a lightweight painting and
image-editing application. The README's "Not done yet" list ends with *save /
load, layers, undo* — this document is that foundation, and both the brush engine
and the image-editing features fall out of it.

## Decision record

Where this document and the records below disagree, **the records win** — they were
settled later, in a design review, and several correct earlier claims here.

- [`CONTEXT.md`](CONTEXT.md) — canonical terminology. Layer kinds are **Pigment**
  (the default), **RGB**, **Media**, **Strokes**, **Adjustment**, **Text**, **Flats**.
  **Flow** means brush flow only; the fluid field is **velocity**.
- [`docs/ui.md`](docs/ui.md) — UI direction. Supersedes the MacPaint chrome, and
  resolves four conflicts between the wireframe and this document.
- [`docs/operations.md`](docs/operations.md) — the full operation catalogue: every
  colour control, filter, transform, selection op and clipboard op, each tagged with its
  op class. Supersedes §4's illustrative lists, which name examples rather than the set.
  Two rules from it that this document does not state: **add noise runs in the shaper
  domain**, and **dodge/burn is a brush painting into an adjustment mask**, not a pixel op.
- [`docs/shortcuts.md`](docs/shortcuts.md) — the default keymap. Photoshop-identical
  wherever Photoshop assigns a key; three documented deviations; flatting tools are
  **layer-kind scoped**. The keymap is data with load-time conflict detection, which is
  why `app/Keymap` lands in phase 2 rather than alongside the tools that use it.
- [`docs/autoflats-migration.md`](docs/autoflats-migration.md) — absorbing autoFlats as
  the **Flats layer** and phases 16–18. Its rubber-sheet solve re-parameterises the
  Poisson solver already in `shaders/jacobi.wgsl`.
- [`docs/document-format.md`](docs/document-format.md) — the native format is
  **multi-part tiled EXR** with a `.npaint` extension. Latents are named channels; OIIO
  writes it, so native save needs no bespoke writer. Supersedes an earlier PSD-container
  decision.
- [ADR-0001](docs/adr/0001-lazy-allocation-gated-by-idle-budget.md) — heavy
  subsystems allocate lazily, per mode, gated by an idle-RSS assertion
- [ADR-0002](docs/adr/0002-solver-window-tracks-the-wet-region.md) — a Media layer's
  solver runs in a transient window around the wet region
- [ADR-0003](docs/adr/0003-deposition-is-per-dab-not-per-frame.md) — deposition is
  per-dab and `dt`-independent, in every medium
- [ADR-0004](docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md) — colour
  ops collapse to a shaper + 3-D LUT, from phase 2
- [ADR-0005](docs/adr/0005-fixed-timestep-and-dab-replay-for-undo.md) — fixed
  timestep; undo replays dabs from a solver keyframe
- [ADR-0006](docs/adr/0006-mixbox-now-reimplement-from-the-paper-later.md) — ship on
  Mixbox now; reimplement from the paper before distribution
- [ADR-0007](docs/adr/0007-erase-is-mass-reduction-not-a-colour.md) — erase reduces
  **Mass** and leaves the **Latent** alone; it is the brush with a negative deposit, never
  a brush that paints white
- [ADR-0008](docs/adr/0008-recovery-needs-a-model-journal-not-tile-spill.md) — recovery
  needs a journal of the document model. **Corrects this document's claim** that crash
  recovery falls out of the `mmap` tile spill for free — see §3

Superseded by the above: "opacity as latent lerp" in §3 (opacity is alpha; mixing is
the `Mix` blend mode), the 2048² sim canvas in §7, and "one stroke, one undo step"
in §7 as it applies to Media layers.

## Scope

Two co-equal pillars, not one: **painting** and **image work**. Visdev uses
Photoshop for mark-making at least as much as for manipulation, so a brush engine
is a requirement, not a follow-on.

| | |
|---|---|
| **Paint** | brush engine, pressure / tilt, opacity vs flow, layers, KM colour mixing |
| Inspect | open, pan/zoom, pixel probe, histogram, channel isolation |
| Grade | levels, curves, exposure, saturation, RGB→grayscale, channel mixer |
| Repair | clone, heal, inpaint |
| Texture prep | offset, make-tileable, lighting-gradient removal, seam heal |
| Ship | export to any format OIIO writes, at any bit depth |

### What this deliberately does not become

Photoshop's moat is a long tail plus PSD interchange plus a plugin ecosystem.
Attacking that head-on is how every competitor has lost. Explicitly out:

- **Full-fidelity layered PSD.** The native format is EXR; PSD is a deliberate export,
  flattened or simply-layered. Layer styles and smart objects stay out, and native
  `curv`/`levl` blocks are deliberately *not* written because our curves live in the
  shaper log domain and would be silently wrong.
- **Own text shaping, and text on a path.** Basic text became scope with the UI
  direction (phase 14) as a parametric `Text` layer, but shaping / bidi / fallback come
  from CoreText, and text-on-path, vertical text and rich-text runs stay out.
- **Selection → path tracing.** Path → selection is in scope; contour extraction plus
  curve fitting is not.
- **CMYK / prepress.** Separations, spot channels, Pantone. Real capability, no
  overlap with texture and photo work.
- **Layer styles, smart filters, adjustment-layer stacks of arbitrary depth.**
  The live op stack below covers the useful 95% at 2% of the machinery.

The bet is the same one Procreate made: beat Photoshop decisively at one job
rather than partially at all of them.

---

## 1. Architecture

```
src/
├── core/
│   ├── Tile.hpp          tile geometry, kTileSize
│   ├── TileStore.{hpp,cpp}   sparse COW tile map, residency, LRU
│   ├── Document.{hpp,cpp}    canvas dims, colorspace, layer list
│   ├── Layer.{hpp,cpp}       content + mask + ops + blend
│   ├── Blend.{hpp,cpp}       linear-safe merge modes
│   ├── OpStack.{hpp,cpp}     ordered live ops + ROI + dirty tracking
│   └── History.{hpp,cpp}     undo as tile refs + op-stack diffs
├── color/
│   ├── Space.{hpp,cpp}    primaries, transfer fns, working-space policy
│   └── LutBake.{hpp,cpp}  shaper 1D + 3D LUT bake from the op stack
├── io/
│   ├── ImageIO.hpp        backend interface
│   ├── StbBackend.cpp     always built
│   ├── OiioBackend.cpp    NP_USE_OIIO
│   ├── BrushIR.hpp        neutral brush representation + import report
│   ├── AbrImport.cpp      8BIM container + Action Descriptor tree
│   ├── BrushImport.cpp    Procreate zip + NSKeyedArchiver bplist
│   └── Descriptor.{hpp,cpp}  shared Objc/VlLs/UntF reader (reusable for PSD)
├── ops/
│   ├── PointOps.{hpp,cpp}   levels, curves, sat, grayscale   class A
│   ├── Blur.cpp             gaussian/box via mip pyramid     class B
│   ├── Highpass.cpp         src − blur(src), unsharp         class B
│   ├── Offset.cpp           wrap-shift                       class B
│   ├── Tileable.cpp         gradient removal + seam heal     class B/D
│   ├── Clone.cpp            dab replay, spatial index        class C
│   └── Inpaint.cpp          diffusion (C), PatchMatch (D)    class C/D
├── brush/
│   ├── Brush.{hpp,cpp}      tip, spacing, response curves
│   ├── Dynamics.{hpp,cpp}   pressure/tilt/velocity → parameters
│   ├── StrokePath.{hpp,cpp} Catmull-Rom + arc-length dab emission
│   └── StrokeBuffer.{hpp,cpp}  per-stroke coverage scratch (opacity ceiling)
├── gfx/    Context, ShaderLoader                (unchanged, shared)
├── sim/    PaintSim → becomes a brush mode      (bounded, see §7)
└── ui/     Atelier*  — replaces MacPaintUI/Theme; see docs/ui.md
              (viewport pan/zoom carried forward, chrome does not)

shaders/ops/*.wgsl   hot-reloadable via existing Cmd+R
```

`gfx/ShaderLoader` already resolves `//#include` and hot-reloads on Cmd+R. Every
op shader inherits that for free, which makes op development a sub-second loop.

---

## 2. Colour pipeline

Adopt Nuke's model: **decode to a linear working space on import, do all work
there, encode to the target space on export.** This is not stylistic. Every
operation that averages pixels — resample, blur, downscale, alpha composite,
the paint solver's advection — is only correct on linearly-encoded light.
Photoshop composites in the document's gamma space by default, which is the
actual reason its default blurs and gradients look muddy.

```
file ─► OIIO/stb ─► decode transfer fn ─► [ adapt primaries ] ─► LINEAR RGBA16F
                                                                      │
                          all ops, all compositing, the paint sim ────┤
                                                                      │
        display ◄── view transform ◄── shaper+3D LUT ◄─────────────────┤
                                                                      │
        file ◄── encode transfer fn ◄── [ adapt primaries ] ◄──────────┘
```

### Decisions

**Working space: linear, sRGB/Rec.709 primaries, by default.** Keep primaries a
`Document` field rather than a constant so ACEScg is a config change, not a
rewrite. Do *not* hard-code "sRGB linear" into op shaders.

**Storage: `rgba16float`.** Half gives ~10 bits of mantissa with a huge
exponent range — for linear light that beats 16-bit integer everywhere, at half
the memory of f32. Compute in f32 in the shader, store f16. The paint sim's
pigment fields stay `rgba32float` for the conservation reasons the README
documents; that constraint is local to the sim and should not propagate.

**Alpha: premultiplied (associated).** Correct for compositing and matches the
EXR/Nuke convention.

> ⚠️ **Un-premultiply before any per-channel colour op.** Running levels or
> curves on premultiplied data darkens partially-transparent pixels wrongly.
> `unpremult → op → premult` must be baked into the point-op path, not left to
> the caller. This is the single most common bug in linear pipelines.

**Curves are authored in a log domain, not in linear.** A curve UI on raw linear
values is unusable — everything interesting crowds into the bottom 5% of the
graph. The shaper LUT in §4 solves this and the LUT-domain problem at once.

---

## 3. Document model, memory, and the resource budget

**"Lightweight" means three specific things**, and they are requirements, not
aspirations:

1. **Fast to start.** Window on screen in well under a second, cold.
2. **Small until loaded.** Memory tracks document data, not the binary.
3. **Graceful across several open documents.** An inactive document must not hold
   the machine's resources hostage.

The third is the one with real architectural consequences, and it is handled
under *Multiple documents* below.

### Layers

```
Document
├── colourspace, dimensions
├── Layer[]                    bottom → top
│   ├── kind      Pigment | RGB | Media | Strokes | Adjustment | Text
│   ├── mask      single-channel TileStore, optional
│   ├── ops       OpStack — per-layer, non-destructive
│   ├── opacity, blendMode
│   └── visible, locked
└── ops                        document-level OpStack (global grade)
```

An `Adjustment` layer holds no pixels — it is an op stack applied to the
composite accumulated below it, which is Photoshop's adjustment layer. A
per-layer op stack is the same thing attached rather than floating. Supporting
both costs one enum case and removes the "clip to layer below" confusion for the
common case.

Layers are nearly free under sparse COW: an adjustment layer is zero pixel bytes,
a mask is 16 KiB per touched 128² tile at `r8unorm`, and a layer holding one
200 px stroke owns four tiles.

> ⚠️ **Blend modes have to be chosen for linear space.** Over, plus, multiply,
> screen, min and max are mathematically meaningful on linear light. Overlay,
> Soft Light and Hard Light are defined against a display-referred [0,1] domain
> and misbehave above 1.0 — which is why Nuke's merge set is mostly the first
> group. Ship the linear-safe set; if Overlay is wanted, define it explicitly on
> the shaped domain from §4 and document that it is display-referred.

### Selections

A selection is a single-channel **coverage** mask over document space — the same
sparse tile machinery as everything else, at `r8unorm`, 16 KiB per touched 128² tile.
Coverage rather than a bitmask: a hard 0/1 selection puts jagged edges on every
operation it gates.

Earlier in this document I called selections *"a scheduling question, not an
architectural one."* That was wrong in three specific ways:

**Gating is cross-cutting.** Every deposit and every op must respect the mask — dabs,
LUT application, blur, clone, bake. That is not a feature bolted on later; it is a
parameter threaded through interfaces that already exist. **Reserve the seam in
phase 2**, even though no selection tool ships for several phases, or it becomes
precisely the pervasive retrofit ADR-0001 warns about.

**Inpaint depends on it.** PatchMatch operates on a masked region by definition, so
inpaint cannot ship before selections exist. That makes selections a prerequisite
rather than polish, and it earns them their own phase.

**Feather wants the blur from phase 6.** Grow, shrink and feather are a distance
transform or a blur over the mask, so selections land naturally *after* the filter
phase rather than before it.

The five tools, in ascending order of difficulty:

| tool | mechanism |
|---|---|
| Rectangle, Ellipse | analytic — evaluate coverage per pixel, antialiased for free |
| Lasso, Polygon lasso | polygon rasterisation with coverage |
| **Magic wand** | flood fill with tolerance — the awkward one |

> ⚠️ **Magic wand does not tile.** Flood fill is a connected-component operation, so
> a region may cross any number of tile boundaries and the result is inherently
> sequential. Do it on the **CPU**, paging through the tile store. It is one click,
> not a per-frame operation, and a GPU label-propagation pass would be iterative,
> approximate, and far harder to debug for no visible gain.

Selections and layer masks are the same data, so *load mask as selection* and *save
selection as mask* are free once both exist. Marching-ants display is genuine UI work
though — a boundary edge-detect plus an animated dash.

### Pigment layers — Kubelka-Munk in a layered document

Yes, it survives, under one rule.

**The rule follows from the property that makes the sim work.** Mixbox
guarantees that *linear combinations of latents are KM mixes*. So any op that is
a linear combination of pixels stays valid in latent space, and any op that is
not, is not:

| valid on latents | must bake to RGB first |
|---|---|
| blur, offset, resample, advection, diffusion | levels, curves, any LUT |
| `over` / opacity as a latent lerp | saturation, grayscale, channel mixer |

That is the same linearity that lets `advect_pigment` transport latents in the
first place. Not a coincidence, and worth stating as the document invariant.

**Compositing — opacity is transparency, mixing is a blend mode.** Latent lerp
between two pigment layers,

```
latent = latent_over · α  +  latent_under · (1 − α)
```

gives green for blue-at-50%-over-yellow. That is correct for *mixing* and wrong
for *fading*: a user dragging a layer's opacity down is asking to see through it,
not to mix it, and layer opacity has thirty years of meaning as transparency.

So **opacity is alpha on every layer kind**, and the latent lerp above is exposed
as a distinct blend mode, `Mix`, alongside Normal / Multiply / Screen. Default
behaviour is what everyone expects; pigment behaviour is one dropdown away and
labels itself. Mixing *within* a layer — between brush dabs — is unconditional and
needs no mode.

`composite.wgsl` already does the latent → RGB conversion at the end of either
path.

Note this is *mixing* semantics, not *glazing*. True glazing — a translucent dry
layer over another — is the KM layering equation
`R = R_up + T_up²·R_down / (1 − R_up·R_down)`, which needs explicit K and S per
pigment. Mixbox's latent is a mixing model and does not expose them; the
`NP_USE_MIXBOX=OFF` two-constant path does. So glazing is reachable on that path
and not on this one. Worth knowing before promising it in the UI.

**The stack is one-way.** Going down, the first RGB layer collapses everything
below it to RGB. Mixbox does have an RGB→latent direction, so lifting back is
possible — but that map picks *a* plausible pigment decomposition, not the true
one, and repeated round-trips drift. Make it an explicit user action, never
automatic. Worth having, though: "interpret as pigment" would let a scanned
watercolour be mixed into with simulated paint, which no other tool can do.

**Memory.** A pigment tile carries 7 channels (c0 c1 c2 m, res rgb) against 4:

| 128² tile | bytes |
|---|---|
| RGBA16F | 128 KiB |
| pigment, 8ch f16 | 256 KiB |
| pigment, 8ch f32 | 512 KiB |

**Store f16, simulate f32.** The README's conservation argument for
`rgba32float` is a *simulation* constraint — the error came from rounding the
same exchange 240× per second. A committed, dry layer is not being exchanged, it
is being read. Only pigment layers pay the 2×, and only on tiles they occupy.

**On disk.** No standard format carries pigment latents, but EXR carries
arbitrary named channels, which is precisely what it exists for:

```
R, G, B, A                          ← baked composite; any tool opens this
layer0.pig.c0 … c2, layer0.pig.m
layer0.res.r … b
```

The file is simultaneously a normal image and a pigment document — other tools
see the picture and ignore the rest. OIIO reads and writes this today.

> ⚠️ **Stamp the pigment basis as a file attribute.** `NP_USE_MIXBOX=ON` and `OFF`
> produce latents in *different bases*, and a future reimplementation (ADR-0006) will
> be a third. A file written in one basis is meaningless in another, and silently so.
> Record `pigmentModel = mixbox-v1 | km2-v1 | np-km-v1`.
>
> On a basis mismatch, **offer to open the embedded RGB composite** rather than
> failing — which is exactly why that composite is mandatory. A document written today
> stays openable forever; it loses only its re-mixability as pigment. Note also that
> latents in a saved file inherit Mixbox's CC BY-NC terms: the encumbrance travels with
> the document, not just the binary.

### Sparse copy-on-write tiles

The document is a hash map from tile coordinate to tile, not a buffer.
`kTileSize = 128` default:

| | RGBA16F |
|---|---|
| one 128² tile | 128 KiB |
| one 256² tile | 512 KiB |

128² is the recommended default. Larger tiles cut per-tile bookkeeping and
dispatch overhead; smaller tiles cut *write amplification* — a 20 px clone dab
should not dirty and snapshot a 512 KiB tile. Since the primary interaction is
stroke-based, favour the small tile. Keep it `constexpr` so it stays tunable.

### Where the pixels actually live

Three residency classes, and this distinction is the whole design:

1. **Unmodified source tiles** — not owned. Paged on demand from OIIO's
   `ImageCache`, which is itself a tiled, mip-mapped, LRU-bounded reader with a
   configurable budget. An untouched 8K photo costs the cache budget you set,
   not its full footprint.
2. **Modified tiles** — owned, resident, dirty. Only tiles a stroke actually
   touched.
3. **View tiles** — the GPU-side evaluated result for the visible viewport at
   the current zoom level. Bounded by window size, not image size.

Resulting footprint:

| case | resident |
|---|---|
| 8K photo, opened, untouched | cache budget (e.g. 256 MiB), evictable |
| same, 1:1 view on a 2560×1440 window | + ~30 MiB of view tiles |
| same, after 200 clone dabs | + ~10 MiB of owned tiles |
| 8K flattened in one buffer (the naive design) | 255 MiB, mandatory |

Undo is a stack of *previous tile references*, not full-image snapshots — a COW
tile that no history entry references is freed. Op-stack edits undo as small
parameter diffs.

**Mip pyramid for display.** At 25% zoom, evaluating full-res tiles to draw a
quarter of them is four times the work for no visible gain. Keep a display
pyramid; evaluate at the mip level the zoom actually needs. OIIO's ImageCache
supplies mips for source tiles; generate them for dirty tiles on commit.

### Multiple documents

The budget is **process-wide**, not per-document, and the active document has
priority. One rule makes this tractable: *classify every resource by whether it
can be reconstructed.*

| resource | reconstructible from | on deactivate |
|---|---|---|
| source tiles | the file on disk | evict |
| evaluated tiles | replay the op stack | evict |
| GPU textures | re-upload | **release — always** |
| baked LUT | rebake, microseconds | release |
| op stacks, stroke records, layer metadata | nothing — but they are kilobytes | keep |
| dirty pixel tiles | nothing — sole copy | spill (below) |
| sim fields | nothing — live solver state | bake to tiles, then free |

An inactive document therefore costs its metadata and nothing else: **kilobytes,
not megabytes.**

**Presentation is tabs, with an optional split showing two tabs.** So the GPU rule
is **only visible documents hold GPU textures, at most two** — a simple, enforceable
predicate, and VRAM is the tighter of the two budgets. Two visible documents roughly
double a small number (~30 → ~60 MiB of view tiles at 2560×1440), because view tiles
are bounded by viewport rather than by image size.

A visible-but-unfocused document in a split **keeps stepping its solver** — a frozen
wet wash on screen reads as a bug. Hidden tabs bake their wet extent and release
everything reconstructible, which is what makes "twenty tabs open" cost kilobytes
each.

**Dirty tiles spill through `mmap`.** They are the only irreplaceable pixel data,
so on deactivate they are written to a per-document scratch file and mapped back.
The kernel's page cache then handles eviction, under real system memory pressure,
without anyone having to write an eviction policy.

> ⚠️ **This paragraph used to claim "crash recovery falls out of it nearly free". That was
> wrong**, and it was the most reassuring sentence in the document about data safety. The
> spill fires *on deactivate*, so the document being actively painted has written nothing;
> it stores tiles with no record of which layer they belong to; and a document whose value
> is an op stack and a Strokes layer has almost nothing in it — meaning the more
> non-destructively a user worked, the less recovery they got. Real recovery needs a
> periodic journal of the document **model**, with the tile spill as its pixel store and the
> journal as the index it was always missing. See
> [ADR-0008](docs/adr/0008-recovery-needs-a-model-journal-not-tile-spill.md).

> ⚠️ **The sim must be lazily built and torn down on document switch.** The field
> set is large enough that this dominates everything else:
>
> | sim resolution | ~14 field textures |
> |---|---|
> | 2048² | ≈ 570 MiB |
> | 1024² | ≈ 142 MiB |
> | 512² | ≈ 36 MiB |
>
> **1024² is the better default** (§7 originally said 2048²). More importantly,
> an editing session that never touches a paint layer must allocate *none* of it.
> naturalPaint currently builds the sim at startup because the sim *is* the app;
> in the merged app it has to be constructed on first Sim-layer use and released
> when the document deactivates.

### Startup

Target: **window on screen in under 100 ms, ~30 MiB resident with no document
open.** What stands in the way today, in rough order of cost:

- **Shader compilation** — ~20 WGSL files compiled up front. Compile lazily: op
  shaders on first use of that op, sim shaders on first Sim layer. Cache the
  pipelines in a map; `ShaderLoader` already has the seam.
- **The Mixbox LUT** is a PNG decode. Skip it until a pigment or sim layer exists.
- **Sim field allocation** — see above.
- **OIIO plugin discovery** scans for format plugins on first API use. Initialise
  on first file open, not at app start; `NP_USE_OIIO` already isolates the call
  sites.
- **WebGPU adapter/device request** is async and largely irreducible (~50–150 ms).
  It is the floor, so nothing else should be racing it.

Worth asserting rather than hoping for — `--selftest` already exists as the place:

| assertion | budget |
|---|---|
| no document open | < 40 MiB RSS |
| one 4K RGB document, 1:1 view | < 150 MiB RSS |
| same, deactivated behind a second document | < 20 MiB attributable |
| GPU textures held by inactive documents | 0 |
| cold start to first frame | < 100 ms |

---

## 4. The op stack

Four op classes. **The axis that matters is whether the op is a deterministic,
order-stable, affordable function of its inputs** — not point vs spatial, and not
parametric vs stroke. A stroke is perfectly re-evaluable if you record it; a
time-stepped fluid solve is not, however few parameters it has.

| class | examples | live? | mechanism |
|---|---|---|---|
| **A** parametric point | levels, curves, saturation, grayscale, mixer | yes | LUT collapse |
| **B** parametric spatial | blur, highpass, unsharp, offset, resample | yes | pass + ROI |
| **C** recorded stroke | clone, heal | yes | replay + spatial index |
| **D** baked | paint sim, PatchMatch inpaint | no | COW write, one-way |

### Class A — point ops → collapse into one LUT

Levels, curves, exposure, gain/offset/gamma, saturation, RGB→grayscale, channel
mixer, hue shift. All are `f(rgb) → rgb` with no spatial extent, so **the entire
stack bakes into a shaper 1D LUT + a 3D LUT and applies in a single pass.**

```
linear ─► shaper (log encode, 1D×3) ─► 3D LUT (32³ or 64³) ─► linear
```

Three problems, one mechanism:

- **Stack depth becomes free at draw time.** Twelve grade ops cost the same as
  one. Rebake on parameter change (sub-millisecond for 32³).
- **The LUT domain problem is solved.** A 3D LUT is indexed on [0,1]; linear HDR
  data exceeds that. The log shaper maps a wide scene-linear range into [0,1]
  first. This is exactly what OCIO's GPU path does.
- **Curve UX is fixed for free.** Author curves in the shaped domain and the
  control points land where a Photoshop user expects them.

Note the WebGPU storage-texture limit — `GpuContext::maxStorageTextures` is 4.
A naive "one bind per op" design hits that wall at four ops. The LUT collapse
sidesteps it entirely rather than working around it.

One refinement: it is each **maximal run of adjacent point ops** that collapses,
not the stack as a whole. `levels → blur → curves` bakes two LUTs around one blur
pass. The common cases — all grading, or grading after a filter — still collapse
to one.

### Class B — parametric spatial ops → live passes with ROI

Real passes, but still parametric, so they stay live and re-editable. Only two
pieces of machinery are new:

**Region of interest.** To evaluate one tile through a blur of radius *r* you
need input covering that tile expanded by *r*, and the expansion propagates back
up the stack. Each op declares `roi(rect) → rect`; the evaluator walks it
backwards to decide which source tiles to fetch.

**Hash-keyed tile cache**, keyed on `(layer, tileCoord, mipLevel, hashOfOpsBelow)`.
Because the stack is a bounded list rather than a graph, that hash is a running
fold down the list — no traversal, no fan-out, no invalidation graph. This is the
cheap 80% of what Nuke's cache buys.

**Large blurs go through the mip pyramid.** A σ = 200 px Gaussian at full res is
absurd; downsample, blur small, upsample. The pyramid from §3 already exists for
display. It also bounds the ROI blowup in practice — at 25% zoom you evaluate at
a mip where *r* is a quarter as large.

Highpass is `src − blur(src)`, so it arrives free with blur, and it is *already
required* by make-tileable (§6) for lighting-gradient removal. Building blur
non-destructively costs nothing extra and pays for itself twice.

> Photoshop has Smart Filters, but a Smart Object re-renders in full on every
> parameter change rather than per-tile, which is why people flatten instead.
> Tiles plus ROI make a live blur *cheaper* here than it is there — one of the
> few places where arriving late is an advantage.

### Class C — recorded strokes → a layer that replays

A `Strokes` layer holds no pixels. It holds an ordered list of stroke records —
position, radius, pressure, hardness, flow, source offset, aligned flag — and at
evaluation time it replays them, sampling from the composite below. If anything
underneath changes, the clone re-derives and stays consistent.

```cpp
struct Dab {                        // ~48 bytes
  float x, y, radius, pressure, hardness, flow;
  float srcDx, srcDy;               // clone vector
  uint32_t strokeId, flags;         // aligned / heal / erase
};
```

Three reasons this beats baking, and the first is the one that matters most:

**Correctness under later grading.** Bake a clone, then change the base layer's
exposure, and the patch carries the old tone — visibly wrong. Photoshop has this
problem; the workaround is "grade before you retouch", an ordering rule the user
has to remember. Replay removes the rule.

**Memory.** 500 dabs is ~24 KiB of records against ~12.8 MiB of dirtied tiles.
Roughly 500× smaller, and it is one of the better wins available against the
lightweight goal.

**Editability.** Change a source offset after the fact, adjust one stroke's
radius, delete stroke 17 out of the middle. None of that survives a bake.

RotoPaint is the cautionary example — it is slow because it replays every stroke
over the whole frame. Two things fix that here:

- **Spatial index over stroke bounding boxes.** A tile only replays the dabs
  whose footprint intersects it. On a typical retouch that is 5–20 dabs, not 500.
- **Checkpoint tiles.** Cache the evaluated state after dab *K*. Appending a dab
  replays only from the last checkpoint. Editing dab 3 of 500 is the expensive
  case, and it is rare.

**A Strokes layer samples only from layers below it, never from itself.** This is
the rule that keeps the whole thing tractable: with no self-reference, ROI is
exactly one clone-vector offset, there is no cascade, and tiles stay independent.

Clone-from-clone is not lost — stack a second Strokes layer above the first, whose
"below" now includes the first layer's evaluated result. Cascade depth becomes the
number of Strokes layers, which the user can see and control, rather than an
invisible chain-depth cap that changes behaviour silently at its boundary.

Healing is the same layer type with a gradient-domain solve instead of a straight
copy — deterministic, so it replays too.

### Class D — baked ops → destructive COW writes

Two things genuinely cannot replay:

**The paint sim.** A time-stepped fluid solve. "Re-evaluating" means re-running
the simulation, and the result depends on step count and timing rather than on a
parameter set. It composites into tiles on stroke commit and that is final.

**PatchMatch inpaint.** Iterative *and* stochastic — a re-run gives a different
answer, so a live one would shimmer whenever anything below it changed. Seeding
the RNG deterministically per region fixes reproducibility, but not the cost of
re-running an iterative optimiser on every underlying edit. Ship it as a cached
result with an explicit **Recompute** button: honest about what it is, and the
mask stays editable even though the output is baked.

Diffusion inpaint (tier 1) is cheap and deterministic enough to sit in class C if
it proves useful there.

---

## 5. I/O

```
ImageIO (interface: read tile, read whole, write, query caps)
├── StbBackend   always built — png, jpg, tga, bmp, 8-bit
└── OiioBackend  NP_USE_OIIO=ON
                 exr, tif, dpx, hdr, psd (flat), cin, raw via libraw
                 + ImageCache tiled/mip/LRU reads
                 + OCIO config for input/output transforms
```

Format support becomes a runtime capability query. The core builds and runs with
zero new dependencies, so §1–§4 can be built and tested before touching OIIO's
build system.

When OIIO does land, `ImageCache` is not merely the file reader — it *is* the
residency layer for class-1 tiles in §3. That is the main reason it earns the
dependency cost.

---

## 6. The named workflows

### Make tileable

Ordering matters, and the first step is the one most tools skip:

1. **Remove the lighting gradient.** A photo has light falloff. Tile it and the
   falloff reads as a visible brightness grid *no matter how good the seam is*.
   Divide by a heavily-blurred copy of itself (σ ≈ ⅛ of the short edge) and
   re-centre the mean. This is the step that separates a usable texture from an
   obviously-tiled one.
2. **Offset by (w/2, h/2) with wrap.** Seams move to a cross through the centre.
3. **Heal the cross.** Inpaint or clone over the band. Graphcut seam placement
   (Kwatra 2003) is the better long-term answer than straight-line blending.
4. **Offset back** (optional — the texture is tileable either way).

**A 3×3 repeat preview is not optional.** Tileability cannot be judged on a
single tile; this is a required part of the feature, not a nicety.

### Inpaint — a ladder, not a single choice

| tier | method | good for | cost |
|---|---|---|---|
| 1 | Diffusion (Telea / Navier–Stokes) | scratches, dust, small holes | ~150 lines, ships in a day |
| 2 | **PatchMatch** (Barnes 2009) | textured regions, real content-aware fill | ~400 lines, GPU-able |
| 3 | Diffusion models | novel content | out of scope for "lightweight" |

Ship tier 1 first because it is cheap and covers dust and scratches. **Tier 2 is
the single highest-leverage feature in this document** — PatchMatch is literally
the algorithm behind Content-Aware Fill, it is a readable paper, and porting
papers is how this project has been built so far. It belongs in `papers/`.

### Clone

An offset-source brush. The stroke plumbing, pen pressure and brush falloff
already exist in `AppState`/`MacPaintUI`; clone is a sampling change, not new
infrastructure. Needs aligned and non-aligned modes and a source-point pick.

---

## 7. Painting

Visdev work is *painting*, and that is the job Photoshop is most used for in that
world. It needs a real brush engine. The fluid sim is not one.

### One brush engine, three substrates

There is **one** brush engine. Tip, spacing, dab emission, the dynamics matrix and
the stroke buffer are all shared — **only the final deposit differs**, and which
deposit you get is a property of the layer, not of the brush.

| layer kind | deposit | feel | cost |
|---|---|---|---|
| **Pigment** — *the default* | composite latent × mass | full speed, and colour mixes Kubelka-Munk | ~2× memory |
| **RGB** | composite RGBA | traditional digital painting — Photoshop / Procreate | cheapest |
| **Media** | `splat.wgsl` — inject water and pigment into the solver | real watercolour / oil / ink | solver-rate |

All three bake into the same document tiles, so they interleave freely: block in
on a `Pixels` layer, drop a watercolour wash on a `Sim` layer above it, keep going.
**The sim becomes one substrate rather than the application.**

Three consequences worth stating outright:

**This is a ladder, not a mode switch.** Traditional painting, physically-mixing
paint at full interactive speed, and true simulated media are three points on one
axis of fidelity against responsiveness. The user picks per layer. The middle rung
is the one no other tool offers.

**One preset works on all three.** Because only the deposit differs, a brush's tip
and dynamics carry across substrates unchanged — which means, and this exists
nowhere today, **pressure, tilt, velocity and jitter curves driving a watercolour
solver.** `splat.wgsl` is fed straight from `BrushState` today; feeding it from the
dab stream instead gets this for free.

**Targets are substrate-scoped.** `wetness`, `load` and `bleed` mean something only
on `Sim`; hue jitter means something everywhere. Tag each `Target` with the
substrates it applies to, so the generated UI greys out the rest rather than
offering links that silently do nothing.

Conversions, all explicit user actions:

```
Sim ──bake──► Pigment ──bake──► Pixels     natural direction, near-lossless
Pixels ──interpret──► Pigment              lossy — RGB→latent picks a plausible
                                            decomposition, see §3
Pixels│Pigment ──re-wet──► Sim             seeds the solver's deposited field
```

That last one is worth noticing: it re-wets a traditionally-painted layer and lets
watercolour physics move paint that was laid down by a stamp brush.

Trying to make the sim serve *all* painting would still fail on responsiveness,
predictability and cost. A concept painter needs a mark that lands exactly where
and how they expect, a couple of hundred times a minute, for hours — that is what
the `Pixels` and `Pigment` substrates are for.

### Dabs are placed by arc length, not per event

The most common brush-engine bug. Pen events arrive at 120–240 Hz while frames
present at 60–120, so there are several events per frame and the count varies with
stroke speed. One dab per event makes fast strokes gappy and slow strokes
over-dark.

Correct: fit a centripetal Catmull-Rom spline through the last four sampled
points, accumulate arc length along it, and emit a dab every `spacing × radius`
pixels — carrying the remainder across frames so spacing stays continuous.

### Opacity and flow are different things

The distinction most implementations get wrong, and it dictates the architecture:

- **Flow** — how much each *dab* deposits.
- **Opacity** — the ceiling for the *whole stroke*.

At 50% opacity you can scrub back and forth within one stroke and never exceed
50%; lift, stroke again, and it builds to 75%. That is impossible if dabs
composite straight onto the layer. So:

**every stroke needs a stroke accumulation buffer** — a tile-sparse scratch
coverage layer that lives for the duration of one stroke.

```
per dab:  cover = cover + flow · dab · (1 − cover)      // over-composite to scratch
on end:   layer = composite(layer, colour, min(cover, opacity))
```

That one buffer also solves latency and undo granularity: an in-progress stroke
touches only the scratch, so nothing COW-snapshots until the pen lifts. One
stroke, one undo step, no per-frame tile churn.

### The pigment brush is the differentiator

A dab composite is a lerp, and by §3's rule a lerp is valid on latents. So **the
fast brush can paint pigment latents and get Kubelka-Munk mixing with no fluid
solver at all** — blue over yellow gives green at full interactive speed.

**Smudge and wet mix are different things, and only one of them needs the solver.**
Conflating them overstated what a Pigment layer can do:

| | reads | state | available on |
|---|---|---|---|
| **Smudge** | the destination under the tip, re-deposits it nearby | none — stateless per dab | RGB and **Pigment** layers |
| **Wet mix** | destination *into a persistent brush reservoir* that loads and unloads | the 64² reservoir, plus film volume and contact | **Media** layers only |

Wet mix needs `oil_transfer.wgsl`'s bidirectional exchange, which reads `water.z`
and `water.w` — a film depth and a contact mask that a flat Pigment layer does not
have. So it stays Media-only.

What survives on a Pigment layer is still the thing that matters: colour **mixes
correctly when you paint over it**, at full interactive speed, and smudge mixes in
KM rather than RGB. What you give up is the brush *loading* colour off the canvas
and running dry. *"Digital colour mixing goes muddy"* is a standing complaint in
visdev and this still answers it — just one rung lower than Photoshop's Mixer Brush
sits, with the full-fidelity version one layer kind away.

Of everything in this document, this is the most plausible answer to *why would a
concept artist switch.*

### Brush dynamics — the modulation matrix

Procreate has 12 Brush Studio panels; Photoshop has 9 and well over a hundred
controls. Reaching that is mostly **volume, not difficulty** — but only if the
dynamics are data-driven from the first line. Hard-coding them individually
produces a combinatorial mess and makes the UI impossible to generate.

One abstraction carries the entire system:

```cpp
struct Link {
  Source  src;      // pressure, tilt, velocity, fade, noise, random…
  Target  dst;      // size, angle, roundness, flow, scatter, hue, grain…
  Curve   curve;    // editable response, 4–8 control points
  float   amount;
  float   minimum;  // Photoshop's "Minimum Diameter"
  bool    invert;
  Scope   scope;    // per-dab, or frozen at stroke start
};

struct Brush {
  Tip tip;  Grain grain;  Accumulation mode;
  float baseSize, baseFlow, baseOpacity, spacing;
  std::vector<Link> links;      // ← every dynamic, uniformly
};
```

Each dab evaluates `value = base · lerp(min, 1, curve(src)) ± jitter` for every
link targeting it. That is roughly 400 lines plus a curve widget.

> ⚠️ **`BrushState::pressureSize` / `pressureFlow` are exactly the wrong shape.**
> They are the hard-coded special case of a `Link`. Delete them and ship
> pressure→size and pressure→flow as two default links. If *any* dynamic bypasses
> the matrix, the UI can no longer be generated from it and presets stop
> round-tripping.

| sources — per dab | sources — frozen per stroke |
|---|---|
| pressure, tilt magnitude, tilt azimuth, barrel rotation | initial direction |
| velocity, acceleration, stroke direction | stroke-level random |
| fade (arc length), stroke progress | |
| dab random, 1-D noise along arc length | |

Tilt azimuth → angle is a single link and it is what makes a chisel or
calligraphic brush work. The noise source matters more than it looks, too:
coherent 1-D noise along arc length reads as organic variation, where per-dab
random reads as sizzle.

> ⚠️ **Jitter must be deterministic.** Seed from `(strokeId, dabIndex)`, never a
> global PRNG. Otherwise the scratch-buffer preview will not match the final
> composite and undo/redo silently changes the image — a real bug class, and an
> unpleasant one to find late.

### What it actually costs — and what you already have

| item | notes | scale |
|---|---|---|
| Modulation matrix + curve editor | the table above | ~600 LOC |
| Procedural + stamp tips | round with hardness; greyscale stamp | ~400 |
| **Rolling grain** | grain locked to *canvas* UV, not dab UV — this is what makes a mark read as real media | ~500 |
| Glaze vs blending accumulation | two different models; see below | ~600 |
| Taper, streamline, stabilisation | pull-string or exponential | ~400 |
| Colour dynamics | hue/sat/val jitter, secondary colour | ~250 |
| Scatter / count / dual tip | cheap once the matrix exists | ~300 |
| **Wet mix / colour pickup** | the long pole — but see below | ~1200 |
| Preset format + library UI | JSON, thumbnails, import/export | ~800 |
| The panels | ~100 controls, generated from parameter metadata | ~1000 |

**You already own the long pole.** Wet mix — brush carries paint, runs dry, picks
colour up off the canvas — is the hardest item on that list, and IMPaSTo's 64×64
brush grid in `shaders/oil_brush.wgsl` and `shaders/oil_transfer.wgsl` already
implements it, Algorithm 1 verbatim. The work is *exposing* it as brush parameters
(dilution, charge, attack, pull) rather than building it. And because the mixing
is Kubelka-Munk, it is better than Procreate's, which mixes in RGB.

**Rolling grain interacts with the tile system.** Grain UV has to be
document-space so a stroke reveals fixed paper texture instead of dragging one
along with the dab. That means sampling by canvas coordinate and mip-selecting by
zoom, or the grain shimmers as the view scales.

### The dispatch crux

Accumulation mode decides whether dabs can batch:

- **Glaze** — dabs only write the stroke buffer (`max` / over). Independent, so
  upload a dab instance buffer and issue *one* dispatch per frame.
- **Blending** — each dab reads what is beneath it. Naively serial: hundreds of
  tiny dispatches per frame, dispatch-overhead-bound.

Invert the loop to fix it. Build a per-tile list of the dab indices touching that
tile, then run one dispatch in which **each pixel walks its tile's dab list in
order.** Ordering is preserved, because a pixel's result depends only on its own
history — and N sequential dispatches collapse into one.

That trick does *not* extend to wet mix, where the reservoir is state shared
across pixels: dab N+1's colour depends on what dab N picked up. That path stays
per-dab with a reservoir reduction, which is exactly what the oil model does now.

### Scope tiers

| tier | contents | buys |
|---|---|---|
| **1 — credible** | matrix + curves, procedural/stamp tips, pressure/tilt/velocity/fade/jitter sources, size/angle/roundness/flow/opacity/scatter targets, rolling grain, glaze + blending | ~80% of perceived quality |
| **2 — competitive** | wet mix exposed, colour dynamics, taper, streamline, dual tip, preset library, generated panels, **`.abr` import** | brushes chosen over Procreate's defaults |
| **3 — parity** | **`.brush` import**, per-brush blend modes, wet edges, build-up, brush-pose overrides, noise | the long tail |

Tier 1 is the one that matters. Tiers 2–3 are volume against a stable
abstraction — but **do not start tier 1 without the matrix**, or all of tier 2
becomes a rewrite.

### Importing `.abr` and `.brush`

Both formats carry dynamics, not just tips, and both map onto `Link` far more
directly than expected — because both engines model dynamics the same way this one
does. The importer is closer to deserialisation than to translation.

#### `.abr` (Photoshop) — first

The container is **documented** in Adobe's Photoshop File Formats specification,
because `.abr` reuses the same *Action Descriptor* structure `.psd` uses for layer
effects and adjustment layers:

```
8BIM  version, subversion          (v1/v2 are tip bitmaps only; v6+ carry settings)
├── samp   tip bitmaps, RLE-compressed
├── desc   descriptor tree — the brush settings
└── patt   patterns, for the Texture panel
```

The descriptor is a recursive typed key/value tree (`Objc`, `VlLs`, `doub`, `UntF`,
`bool`, `long`, `TEXT`, `enum`). Container and value encodings are specified; the
*brush* key semantics are not, but the keys are legible — `Dmtr` diameter, `Angl`
angle, `Rndn` roundness, `Hrdn` hardness, `Spcn` spacing, `szVr` size variance,
`opVr` opacity variance, `minimumDiameter`, `scatterDynamics`, `Cnt ` count,
`Txtr` texture, `hueJitter`, `purity`, `WtEd` wet edges.

**The load-bearing key is `bVTy`** — Photoshop's "Control" dropdown. It is
`Link::src`, one for one:

| `bVTy` | Photoshop control | `Source` |
|---|---|---|
| 0 | Off | — drop the link |
| 1 | Fade | fade / arc length |
| 2 | Pen Pressure | pressure |
| 3 | Pen Tilt | tilt magnitude |
| 4 | Stylus Wheel | barrel rotation |
| 5 | Rotation | tilt azimuth |
| 6 | Initial Direction | initial direction |
| 7 | Direction | stroke direction |

So every Photoshop dynamic is a `(bVTy, jitter, minimum)` triple that deserialises
into exactly one `Link`: `src` from `bVTy`, `amount` from the jitter value,
`minimum` from the paired minimum key, and the target implied by which key carried
it. Photoshop has no response curves, so `Link::curve` imports as linear — meaning
an imported brush is immediately *more* tweakable than it was at home. Krita and
GIMP both ship `.abr` readers worth referencing.

#### `.brush` / `.brushset` (Procreate) — second, for the wet-mix parameters

A ZIP holding `Shape.png`, `Grain.png`, a thumbnail, and `Brush.archive` — an
**NSKeyedArchiver binary plist**. That needs a `bplist00` reader plus
keyed-archiver object-graph resolution (`$objects` / `$top` / `CF$UID`): roughly
600 lines, no new dependency.

Its parameter set is richer, and several entries map onto machinery already in the
repo:

| Procreate keys | maps to |
|---|---|
| `pencilPressureSize`, `pencilTiltSize`, `dynamicsSpeedSize`, `dynamicsJitterSize` | `Link`s, directly |
| `plotSpacing`, `plotJitter`, `plotStreamline`, `plotFallOff` | stroke path + stabilisation |
| `grainMovement` (rolling / stamp), `grainScale`, `grainDepth` | grain |
| `taperStartLength`, `taperEndLength`, pressure taper | taper |
| `wetMix`, `wetDilution`, `wetCharge`, `wetAttack`, `wetPull` | **the oil model** |
| `blendMode`, rendering mode | accumulation |

The wet-mix keys are the prize: they are a UI over very nearly what
`oil_transfer.wgsl` already computes.

#### Both

> ⚠️ **Import into a neutral IR, never straight into `Brush`.** Two formats, both
> version-fragmented, both lossy in different directions:
> `abr | brush → BrushIR → Brush`. And **emit an import report** — a brush that
> silently imports *wrong* is worse than one that refuses to import. List what was
> dropped and why.

What will not map, and should be reported rather than approximated:

| feature | why |
|---|---|
| Photoshop Dual Brush | needs a second tip with its own spacing and scatter — tier 3 |
| Photoshop Brush Pose overrides | no equivalent concept here |
| Procreate's six Rendering modes | the accumulation enum has two; the glaze/blend family needs six |
| Texture / brush blend modes | the PS blend set is not the linear-safe set from §3 |

Neither format is encrypted and reading both is straightforward, but Procreate
brush packs are frequently commercial products: import is a convenience for
brushes the user already owns, not a redistribution path. Worth deciding
deliberately whether imported presets can be re-exported.

### Latency is the acceptance test

Painters judge a tool on this inside thirty seconds; Procreate's whole reputation
rests on it. Pen-to-photon under 20 ms is the bar, under 10 ms is good. Which
means the in-progress stroke must reach the screen without waiting on a full
document re-composite: draw the scratch buffer over the last composited frame and
reconcile when the pen lifts.

> ⚠️ **Undo history is the memory risk here, not the brush.** One long stroke can
> dirty 50 tiles at 128 KiB ≈ 6.4 MiB, so a 50-step history is ~320 MiB. Bound
> history in *bytes* rather than steps, compress snapshots (LZ4 or zstd-1 gets
> 2–4× on image tiles for almost no CPU), and spill the tail to the `mmap` scratch
> from §3. This is exactly what Photoshop's scratch disk is.

### The sim must not scale with the document

⚠️ Flagged as a live risk. Per the README's field layout the sim holds ~8
distinct fields, four of them `rgba32float`, plus ping-pong copies. At 4K:

```
pigC + pigR + depC + depR  =  4 × (3840 × 2160 × 16 B)  ≈  531 MiB
```

before counting water, sat, aux, paper and ping-pong targets. Binding the sim
canvas to the document size destroys the lightweight goal on the first 4K file.

**The sim runs at a bounded working resolution** (1024² default, config-capped —
see the field-size table in §3), independent of document size, and composites
into document tiles when a stroke commits. This is also the right model
interactively — you paint into a region, not across a 60-megapixel canvas — and
it means the sim's memory is a fixed, known constant rather than a function of
the file you happened to open. It is allocated on first Sim-layer use and freed
on document deactivate, so an editing-only session never pays for it.

WebGPU's `maxTextureDimension2D` is 8192 by default, so documents beyond 8K
cannot live in a single texture regardless. Tiling is load-bearing on the GPU
side too, not only for RAM.

---

## 8. Phases

Each phase is a vertical slice that leaves the app usable.

Re-derived against the five ADRs.

**Phase 1 — make the simulation obey the new rules.** Three changes, all inside
`PaintSim`, all verifiable with instrumentation that already exists:

| change | ADR | gate |
|---|---|---|
| lazy construction + per-mode field allocation | 0001 | `--selftest`: idle RSS < 40 MB, zero GPU textures |
| fixed `dt` via an accumulator | 0005 | `--diag` reproducible run to run |
| arc-length dab emitter replacing per-frame capsules | 0003 | `--diag` mass table flat once the brush lifts |

Deliverable: the application it is today, at 0 MB idle and ~193 MB in watercolour
rather than 294 MB, reproducible, with Known Bug #2 closed.

**Why this is first rather than housekeeping deferred to the end** — two reasons,
and the second is the load-bearing one:

- ADR-0001 is an invariant, not a feature. The `--selftest` gate has to exist
  *before* there are features to violate it. Retrofitting laziness across eight
  subsystems is a different and much worse job than starting with it.
- **The risky solver changes are only verifiable while the current baseline is
  valid.** `--diag` measures pigment mass against a known-good number today. Once
  the solver lives inside a Media layer behind a transient window (ADR-0002) that
  baseline is gone, and fixed-`dt` plus per-dab deposition would be debugged
  simultaneously with a new architecture. Do them against working code, with the
  conservation tables in `README.md` as the reference.

A useful side effect: the dab emitter built here is the same one the brush engine
needs, so phase 6 arrives smaller than it looks.

**Phase 2 — see a file.** `Document` + `Layer` + `TileStore` + `StbBackend` +
linear decode + `createBlank()` + tiled viewport draw reusing the existing pan/zoom.
The layer list exists and holds exactly one entry; there is no layers UI yet.
**Design for N, ship 1** — retrofitting a layer model later means touching every op
that was written assuming a single buffer. Deliverable: open a PNG, pan and zoom it,
read pixel values in both linear and display encodings.

**Phase 3 — grade it.** Shaper + 3D LUT bake (ADR-0004), the class A op set,
histogram, the op-stack UI. Deliverable: levels, curves, saturation, grayscale,
live, with correct linear-space math and a curve widget that behaves. Extend
`--selftest` with a round-trip assertion: decode → encode returns the original
within tolerance.

**Phase 4 — write it out.** Export with target-space encode and bit-depth choice.
`NP_USE_OIIO` lands here, because export is where format breadth first actually
bites. Deliverable: a working open → grade → save loop.

**Phase 5 — stack it.** Layers UI, the linear-safe blend set plus `Mix`, masks,
adjustment layers, COW tiles, and history/undo for every kind except Media.
Deliverable: the document model, complete — and the save / load / layers / undo the
README has been missing since the beginning.

---

**The remaining phases split into two independent chains.** Nothing in the painting
chain depends on the image chain or vice versa, so the order below is a choice, not
a constraint:

```
image chain      filter → select → repair → tileable
painting chain   brush  → media  → import
```

The image chain is internally coupled — the ROI evaluator and hash-keyed tile cache
built for blur are prerequisites for the Strokes layer's clone replay; selections
need blur for feather; and inpaint needs selections, while make-tileable needs both
blur and inpaint. The painting chain is likewise sequential. Interleaving them costs
context-switching for no benefit.

The numbering below runs the **image chain first** (phases 6–9), because phases 2–4
plus 6–9 are exactly the workflow described as the daily 90%, and finishing that
makes the tool something used rather than something built. Swapping the chains is a
renumbering, nothing more.

### Image chain

**Phase 6 — filter it.** ROI propagation, the hash-keyed tile cache, and
blur / highpass / unsharp. Deliverable: live, re-editable spatial filters.
**Deliberately ahead of retouching:** the Strokes layer needs exactly this ROI and
tile-cache machinery, so building it here means clone is never written against a
destructive path that then gets thrown away.

**Phase 7 — select it.** The selection mask tile store, all five tools (rectangle,
ellipse, lasso, polygon lasso, magic wand on the CPU), feather / grow / shrink using
phase 6's blur, marching ants, and mask ↔ selection conversion. Deliverable:
every op and every brush confined to a region. Ahead of repair because **inpaint
operates on a masked region by definition** and cannot ship without this.

**Phase 8 — repair it.** The `Strokes` layer — dab recording, spatial index,
checkpoint tiles, and the rule that it samples only from layers *below* it — then
clone, heal, and diffusion inpaint. Deliverable: retouching that survives a regrade
of the layers beneath it.

**Phase 9 — tile it.** Offset, gradient removal (wants phase 6's blur), seam heal,
3×3 repeat preview, then PatchMatch as a cached class D op. Deliverable: photo →
tileable texture as a first-class workflow.

**At the end of phase 9 the daily 90% is complete** — inspect, grade, filter, select,
clone, inpaint, offset, make tileable, export at any bit depth. Everything after this
is new capability rather than replacement of an existing workflow.

### Painting chain

**Phase 10 — paint on it.** Brush engine tier 1: the modulation matrix, procedural
and stamp tips, the stroke buffer with a real opacity/flow split, pressure and tilt
response curves, rolling grain, and the latency path that draws the scratch buffer
over the last composite. Plus the deposit seam for **Pigment** and **RGB** layers.
The arc-length dab emitter already exists from phase 1, so this phase is smaller
than the tier-1 line count suggests. Deliverable: a tool a concept artist would
paint with, and Kubelka-Munk colour mixing at full interactive speed.

**Phase 11 — Media layers.** The solver stops being the application and becomes a
layer kind: solver window tracking the wet region (ADR-0002), bake-on-dry into the
dry extent, solver keyframes plus dab replay for undo (ADR-0005), and the Media
deposit path. It inherits the entire dynamics matrix for free, because ADR-0003
already put deposition on the dab stream in phase 1.

**Phase 12 — import brushes.** `.abr` first — the Action Descriptor reader is
reusable for PSD later — then `.brush`. Necessarily last in this chain: the
importer's target *is* `Link`, so it cannot be written until the matrix is settled.
Once it is, `.abr` import is deserialisation rather than translation.

---

## 9. Open questions

- **Does `Tool` stay one enum?** The paint tools and the imaging tools have
  little in common. Likely a tool *registry* rather than extending the enum.
- **OCIO config, or hard-coded transforms?** OCIO is correct and is what makes
  ACES viable, but it is another dependency. Deferrable: hard-code sRGB/Rec.709
  and Rec.2020 transfer functions behind the `color::Space` interface, and swap
  the implementation later without touching call sites.
- **Selections.** Nothing above assumes a selection mask, but clone, inpaint and
  graded regions all eventually want one. A selection is a single-channel tile
  store — the same machinery — so this is a scheduling question, not an
  architectural one.
- **Does the app get a new name?** "naturalPaint" describes the sim, not this.
```
