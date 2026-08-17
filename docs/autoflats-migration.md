# Migrating autoFlats into naturalPaint

[autoFlats](file:///Users/chrisharvey/Documents/Dev/autoFlats) is a working
TypeScript/WebGPU application that turns line art into one flat colour fill per region,
lets the artist repair the segmentation interactively, and exports a layered PSD. This
document plans its absorption into naturalPaint.

**Headline.** Of its 6,817 lines, about **2,850 are the technology**. The remainder is
either a browser UI that naturalPaint already has a better substrate for, or duplicates
of subsystems naturalPaint already owns — including one 409-line subsystem that should be
**deleted and replaced by the real simulation**, which improves the feature while removing
the code.

---

## 1. Why this merges well

Three structural facts, each verified against both codebases rather than assumed.

### 1.1 The edit-replay model is already naturalPaint's architecture

autoFlats stores every user repair as **where the artist drew**, not as which region ids
it hit — merges as the two clicked points or the stroke drawn, deletions as a marker
position, groups as the lasso path, recolours as a point inside the fill. After
re-segmentation it replays them all against the renumbered regions
([`src/state.ts`](file:///Users/chrisharvey/Documents/Dev/autoFlats/src/state.ts),
`replayEdits` in `main.ts`).

That is **exactly** naturalPaint's class-C recorded-op model: store intent, replay when
what is beneath changes. The Strokes layer already re-evaluates a clone when the layer
below is regraded ([DESIGN-imaging.md §4](../DESIGN-imaging.md)); a Flats layer
re-evaluates its repairs when the line art below is redrawn. Same idea, arrived at
independently, which is the strongest possible signal that the abstraction is right.

### 1.2 The rubber sheet's solver already exists on the GPU

The rubber sheet pins a membrane wherever there is ink and lets gravity pull — "how far
it sags is a Poisson problem"
([`membrane.ts`](file:///Users/chrisharvey/Documents/Dev/autoFlats/src/core/membrane.ts),
246 lines of CPU relaxation).

naturalPaint solves Poisson equations on the GPU already: `shaders/jacobi.wgsl`,
`divergence.wgsl`, `project.wgsl` — the pressure projection in the watercolour solver. The
membrane is the same solve with Dirichlet boundaries at ink instead of at the canvas edge.

> This is the single largest saving in the migration. The most algorithmically serious
> part of autoFlats becomes a re-parameterisation of a shader that is already written,
> tested and running — and it runs on the GPU rather than in a CPU relaxation loop, which
> is what makes the rubber sheet affordable at plate resolution.

### 1.3 The GPU growth path is already WGSL

[`gpuGrow.ts`](file:///Users/chrisharvey/Documents/Dev/autoFlats/src/core/gpuGrow.ts)
is 160 lines wrapping a WGSL chamfer-relaxation kernel — parallel Bellman-Ford over
ping-pong buffers. naturalPaint has 22 WGSL shaders with an established
`//#include` mechanism and hot reload. The kernel transfers nearly verbatim; only the
host-side binding code is rewritten. The CPU Dijkstra in `expand.ts` becomes the fallback
it was always meant to be.

---

## 2. Inventory and disposition

| file | lines | disposition |
|---|---|---|
| `core/ink.ts` | 29 | **port** — but see the colour hazard, §5.1 |
| `core/noise.ts` | 38 | **port** |
| `core/slivers.ts` | 58 | **port** |
| `core/flow.ts` | 66 | **port** — and rename; see §6 |
| `core/regions.ts` | 67 | **port** |
| `core/curves.ts` | 72 | **port** |
| `core/expand.ts` | 72 | **port** as the CPU fallback |
| `core/trappedBall.ts` | 83 | **port** |
| `core/relatability.ts` | 97 | **port** |
| `core/morphology.ts` | 122 | **port** |
| `core/closure.ts` | 131 | **port** |
| `core/fronts.ts` | 162 | **port** |
| `core/declutter.ts` | 174 | **port** |
| `core/completionField.ts` | 218 | **port** |
| `core/gaps.ts` | 326 | **port** |
| `core/sag.ts` | 338 | **port** — watershed on the sag field |
| | **2,053** | **to C++** |
| `core/gpuGrow.ts` | 160 | **to WGSL** — near-verbatim kernel |
| `core/membrane.ts` | 246 | **to WGSL** — re-parameterise `jacobi.wgsl` (§1.2) |
| | **406** | **to GPU** |
| `core/watercolor.ts` | 409 | **delete** — superseded by the real sim (§4) |
| `core/psd.ts` | 217 | **delete** — OIIO plus the existing PSD export tiers |
| | **626** | **deleted** |
| `main.ts` | 2,294 | **split**: ~800 lines of algorithm extracted (§3), the rest replaced by ImGui |
| `ui/canvasView.ts` | 423 | **replaced** — naturalPaint's viewport, pan/zoom and rulers |
| `ui/icons.ts` | 68 | **replaced** — and the icon licences (§7) drop with it |
| `worker.ts` | 316 | **replaced** — a job on naturalPaint's evaluator, not a Web Worker |
| `state.ts` | 153 | **becomes** the Flats layer model (§4) |
| `test/*` | 478 | **port first** (§8) |

---

## 3. The trap: the algorithm is not all in `core/`

`src/core/` is clean, dependency-free typed-array code that ports mechanically. But a
substantial part of the *system* lives in the 2,294-line UI file, interleaved with DOM
handling:

| function | what it is |
|---|---|
| `replayEdits` | the whole replay loop — the mechanism §1.1 praises |
| `assignGroups`, `groupFromStroke` | recomputing group membership from lasso geometry |
| `regionAnchor` | picking a stable point inside a region — the determinism keystone (§5.2) |
| `applyMergePair`, `applyMergeStroke`, `mergeAlongStroke` | the two merge tools |
| `applyShapeFill`, `shapeFillFromStroke` | hand-drawn fills |
| `regionAdjacency`, `applyAutoColors`, `applyPalette` | the region adjacency graph and its graph colouring |
| `clusterSmall`, `carveAt`, `rasterizeBarriers` | small-region clustering, bucket carve, bridge rasterisation |
| `pointInPoly`, `distToSeg` | geometry helpers |

Roughly **800 lines of algorithm sitting in a UI file.** Plan the first step as
*extracting a headless flatting library* — from `core/` **and** from `main.ts` — rather
than "translate `core/`, then write a UI". Estimating from `core/` alone understates the
port by about 40%.

### 3.1 Do not transliterate the keymap

Every one of autoFlats' letter bindings collides with a Photoshop meaning: `X` (delete
fill vs swap colours), `R` (group lasso vs rotate view), `C` (recolour vs crop), `D`
(draw-merge vs default colours), `F` (shape fill vs screen mode), `S` (select edits vs
clone stamp), and `Tab` (cycle gaps vs hide panels). `Space` for pan is the only one that
already agreed.

Photoshop's meaning wins in all seven cases, and the flatting tools become a **layer-kind
scoped** set instead of claiming global letters — two of them (`B` recolour brush, `G`
bucket) then need no new key at all, because on a Flats layer the brush *is* the recolour
brush and the bucket *is* the bucket. Resolutions are tabulated in
[docs/shortcuts.md §5.1](shortcuts.md); the porting task is to apply them rather than
carry the old keys across with the algorithm.

---

## 4. Where it lands: the Flats layer

A flat is not a stack of layers. It is one parametric thing that *produces* many coloured
regions, and it must stay re-evaluable — the re-flat loop is autoFlats' entire value.
Materialising 400 real layers would destroy that, make the layer panel unusable and put
400 parts in a `.npaint` file.

So: a **Flats layer**, the seventh layer kind.

```
Flats layer
  reads      the line-art layer(s) beneath it
  parameters lineThreshold, colourReject, smoothing, skeletonize, gapSize,
             sagThreshold, minRegion, sliverWidth, declutter, paletteSize
  recorded   bridges · merge pairs · merge strokes · delete marks ·
             shape fills · group lassos · recolours     (all as geometry)
  derived    ink mask · label field · fill table
  outputs    RGBA — one flat colour per region
  expands    to real layers on demand: per colour, per fill, or merged
```

Class **B + C**: parametric spatial with recorded edits. It re-evaluates when a parameter
changes, when an edit is added, or when the line art beneath it changes — which means
**editing the line art re-flats the drawing**, something autoFlats cannot do because the
line art is an immovable input there. That is a real capability gain from the merge, not
just a port.

Fills are rows in a **Fills panel** beside the Layers panel, carrying the existing
sort-by-sweep, rename-with-Enter and visibility behaviour.

### 4.1 The format already accommodates it

No change to [docs/document-format.md](document-format.md) is needed:

```
part N   "F0001"    R G B A            ← the baked flat projection
                    flat.id            ← UINT channel: the label field
         attrs:  np:kind        "flats"
                 np:flatParams  <blob>
                 np:flatEdits   <blob>
                 np:fills       <blob>   id, colour, name, group, swatch, visible
```

EXR's `UINT` pixel type stores a label field natively and losslessly, and `ZIP`
compression handles flat integer regions extremely well. A recoverer opening the file
sees a channel literally named `flat.id`. This is the fifth time an EXR native feature has
matched a decision made for other reasons.

### 4.2 The wash: delete 409 lines, get a better result

autoFlats' `watercolor.ts` runs a reduced shallow-water coffee-ring simulation at 256 px
on the long side to give exported fills a painted look. It exists because a browser page
had nothing better available.

naturalPaint already has the real thing: the Curtis '97 solver with capillary flow,
granulation, pigment transport and paper tooth, on the GPU, at full resolution.

> **Do not port `watercolor.ts`.** Wire "wash this flat" to *stamp the Flats layer's
> regions into a Media layer and run the existing simulation*. The 409 lines are deleted,
> the feature gets materially better, and the two washes stop being two
> implementations of one idea that will drift apart.

The Beer-Lambert 256-entry colour table at the end of autoFlats' pipeline is worth keeping
as a concept, though — it is why recolouring under a wash is free there, and the same
trick applies to a baked Media result.

---

## 5. Three hazards

### 5.1 Every threshold was authored in 8-bit sRGB

Ink extraction is "dark **and** desaturated", with a line threshold, a colour-reject
saturation and morphological smoothing — all tuned against 8-bit display values. Run them
against linear `rgba16float` and every one silently changes meaning: mid-grey sits at
0.216 in linear, not 0.5, so a threshold that caught faint sketch lines now catches
almost everything.

> **Rule.** Ink extraction, colour reject and all region-colour picking run in the
> **display/shaper domain**, not linear. Identical to the curves decision in
> [ADR-0004](adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md), for identical
> reasons: a threshold is a perceptual judgement and must live where the artist made it.

A Flats layer is also **RGB-kind colour**, not Pigment. Flat colour does not mix, and
routing it through pigment latents would add a plausible-but-untrue decomposition for no
benefit. Painting *over* the flats on a Pigment layer is the normal case and is
unaffected.

### 5.2 Determinism is a UX invariant, not an implementation detail

autoFlats derives each fill's colour from **a point inside the region** rather than from
its id, because ids are reshuffled by any parameter change; an id-derived palette
repainted the whole drawing whenever a slider moved, making it impossible to see what had
changed. Anchored to a place, nudging a slider from 417 fills to 381 leaves 91% of fills
wearing the colour they had, and putting the slider back reproduces all 417 exactly.

The anchor is used rather than the centroid because a ring and its hole share a centroid,
and because anchors are pixels in disjoint regions, so no two fills can collide.

This is hard-won and easy to lose in a port. It belongs in a **regression test**, not a
comment.

### 5.3 Global algorithms do not tile

Trapped ball, watershed, the Poisson sag and the distance transforms are all global over
a dense label field. They cannot be evaluated per 128² tile, so a flat needs full-frame
dense buffers, transiently:

| buffer | bytes/px |
|---|---|
| ink, line mask | 2 |
| core, labels | 8 |
| distance | 4 |
| sag (float) | 4 |
| **total** | **18** |

At 4096×2160 that is **159 MB transient** — an ADR-0001-scale allocation, with the same
resolution as [ADR-0002](adr/0002-solver-window-tracks-the-wet-region.md)'s solver
window: it is scoped to the operation and released after it.

At rest, the label field **RLE-compresses per scanline**. Flat regions are long runs, so a
400-region 4K flat stores in a few hundred KB rather than 35 MB. Expand on edit, compress
on idle.

**Open question:** cap the segmentation working resolution (long edge 2048 by default) and
refine labels upward, or segment at full resolution? autoFlats' inputs are 1–2K line art;
visdev plates are 4–8K, where the Poisson solve is materially more expensive. Recommend
capped-and-refined, with a full-resolution option — but this needs measuring, not deciding.

---

## 6. Terminology consequences

The merge forces a resolution that [CONTEXT.md](../CONTEXT.md) has been deferring, and
adds new terms.

**`flow` now has three meanings** — brush flow (per-dab deposition), fluid flow
(`flow_outward.wgsl`), and autoFlats' stroke-direction field
([`flow.ts`](file:///Users/chrisharvey/Documents/Dev/autoFlats/src/core/flow.ts), which
gap suggestions are direction-checked against). Three is past the point of tolerance.
**Resolution: `Flow` means brush flow and nothing else. The fluid field is `velocity`.
The stroke-direction field is the `stroke orientation field`.**

New terms: **Flat**, **Fill**, **Ink mask**, **Bridge**, **Sag field**, **Ridge**,
**Relatable**. Definitions land in CONTEXT.md with the phase.

Note that **Fill** collides with the fill *operation* in
[docs/operations.md §5](operations.md) (fill with colour). Distinguish: a **Fill** is a
region of a Flat; filling is an operation. Watch this one.

---

## 7. Licensing

Cleaner than the Mixbox situation. Every algorithm is an independent implementation from
published literature — FlatMagic (CHI 2022), *Fast Leak-Resistant Segmentation for Anime
Line Art*, Williams & Jacobs stochastic completion fields, Kellman–Shipley relatability,
Euler elastica, Zhang–Suen thinning, Deegan et al. on the coffee-ring effect. No
encumbrance and no ADR-0006 equivalent needed; credit them in the design doc.

The two dependencies both drop out: `ag-psd` (MIT) is replaced by OIIO, and the
pixel-art icon sets (MIT, CC BY 4.0) go with the browser UI.

---

## 8. Phasing

Dependencies are layers and masks (phase 5), the ROI evaluator and tile cache (phase 6)
and selections (phase 7 — the lasso and marquee edit tools *are* selection tools). So
flatting is a **third independent chain**, alongside image (6–9) and painting (10–12), and
it cannot start before 7.

> **Scope note, stated plainly.** Flatting is an animation and comics production task. It
> is adjacent to the visdev painting pillar but is not among the jobs the PRD names, and
> nothing in the daily workflow that phases 6–9 deliver depends on it. It is therefore
> placed **after** the milestone that matters most (phase 9) so it cannot delay it. If
> flatting is part of your own work, it belongs where it now sits; if it is speculative,
> it belongs behind text and PSD export instead. Worth an explicit call rather than my
> assumption.

### Phase 16 — Flat it

Port the tests first, then the headless library, then the layer kind.

1. **Port `test/run.ts` and `test/harness.ts`** (478 lines) to the existing `--selftest`
   harness, with the fixtures. These are the specification for everything that follows —
   porting them last would mean porting blind.
2. **`flats/Ink`** — extraction in the display domain (§5.1), morphology, Zhang–Suen
   skeletonisation.
3. **`flats/Segment`** — trapped ball, line-centre expansion, region finalisation,
   declutter, slivers. GPU growth via the `gpuGrow` kernel, CPU Dijkstra fallback.
4. **`flats/Fills`** — the fill table, anchors, adjacency graph, graph colouring, palette
   assignment, with §5.2's determinism as a test.
5. **Flats layer kind** — parameters, evaluation, RLE label storage, `.npaint` part.
6. **Fills panel** — the layers-panel sibling; sort by sweep, rename, visibility.

**Deliverable:** open line art → flat → recolour from a palette → save and reload.
**Gate:** the ported invariant tests pass; determinism test holds across a parameter
sweep; idle RSS after closing the document returns to baseline.

### Phase 17 — Fix it

The interactive repair loop, which is what makes it usable on real drawings.

1. **`flats/Gaps`** — fronts, stroke-width matching, orientation checks, relatability,
   Euler-elastica ranking, closure and simplicity pruning, Hermite bridge curves.
2. **Completion field** — the optional high-precision scorer.
3. **Edit recording and replay** — the machinery extracted in §3, all six edit types.
4. **Edit tools** — merge pair, draw-merge, delete, shape fill, group lasso, marquee
   select-edits, plus bridge draw/erase.
5. **Auto-bridge** and the ridge/edit overlays.

**Deliverable:** a real drawing flats correctly and repairs survive re-flatting.
**Gate:** replay is idempotent; a full parameter sweep preserves every recorded edit.

### Phase 18 — Sheet it, and wash it

1. **Membrane sag** — `jacobi.wgsl` re-parameterised with Dirichlet boundaries at ink
   (§1.2), plus the watershed in `sag.ts`.
2. **Sag, zebra and ridge visualisations.**
3. **Expand to layers** — per colour, per fill inside colour groups, merged; and the PSD
   group export those modes feed.
4. **Wash** — flats into a Media layer, running the existing solver (§4.2).

**Deliverable:** the rubber-sheet segmenter, which subsumes gap size, min region, sliver
width and declutter in one threshold, and a wash that is a real simulation.
**Gate:** fill counts on the sample set match autoFlats' published behaviour (615 → 153 on
`Lineart4`); the Poisson solve at 4K stays inside the frame budget or is explicitly
backgrounded.

---

## 9. What this buys

**Photoshop cannot flat.** There is no equivalent feature — flatting there is manual
bucket work, and the dedicated tools that exist are separate applications. This is not a
parity feature, it is one the incumbent does not have, aimed at an audience adjacent to
the one already targeted.

And the merged version is better than either half alone: line art becomes *editable*
because it is a layer rather than a fixed input (§4), the wash becomes a real simulation
rather than a 256 px approximation (§4.2), and the sag solve moves to a GPU shader that
already exists (§1.2).
