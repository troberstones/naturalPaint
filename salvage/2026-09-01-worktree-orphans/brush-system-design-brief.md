# Design brief — the brush system

For a design tool. Companion to [layers-panel-design-brief.md](layers-panel-design-brief.md);
the two share tokens and vocabulary, and this one repeats what it needs so it can be fed on
its own. Everything is a constraint or a fact except items marked **Design question** —
those are the parts to solve.

---

## 1. The product, in the sentences that matter to the brush

**naturalPaint** is a desktop painting and image-processing application on WebGPU whose
distinguishing capability is that **colour mixes as real pigment does** (Kubelka–Munk), not
as RGB. Blue over yellow goes green, not grey.

Three consequences the brush design must absorb:

1. **The brush is loaded with a pigment, not a colour.** Selecting Ultramarine rather than
   Phthalo Blue changes *density, staining and granulation* — the same RGB behaves
   differently on paper. The brush's paint has physics.
2. **One brush works on every layer kind; only the deposit step differs.** There is not a
   pigment brush and an RGB brush and a watercolour brush. There is one brush, one dynamics
   matrix, one preset, and the last step of a dab changes with what it lands on.
3. **The eraser is that same brush with a negative deposit** — it inherits the full dynamics
   matrix. It is not a tool that paints white; on a pigment layer white is *opaque paint*.

Renderer is Dear ImGui: **flat fills, hard rules, no corner radius, no gradients (the colour
picker is the one exception), one shadow in the whole application** (on the canvas). Do not
produce a rounded, glassy, soft-shadowed design — it is not implementable here.

---

## 2. The surfaces this brief covers

The brush system is not one panel. It is five surfaces, and their division of labour is the
first thing to design.

| # | surface | space available |
|---|---|---|
| 1 | **Tool options bar** — full-width band under the tab strip, contextual to the active tool | **46 px tall**, ~1360 px wide |
| 2 | **BRUSH SETTINGS panel** — right-hand docked column, a collapsing header among COLOR / LAYERS / CHANNELS / HISTORY | **322 px wide**, ~400–600 px tall |
| 3 | **On-canvas control** — the brush cursor, and the size/hardness drag HUD | over the canvas, transient |
| 4 | **Preset browser** — presets with thumbnails, saved, loaded, imported | free (modal or a panel mode) |
| 5 | **Import report** — what an imported `.abr` could not reproduce | modal dialog |

```
┌────────────────────────────────────────────────────────────┐
│ naturalPaint │ File Edit … Help        undo redo ⟲ panels  │ 36 px
├────────────────────────────────────────────────────────────┤
│ ▨ study-plate-04.npaint ●│ retouch-ref.tif 64% │ … │ +      │ 34 px
├────────────────────────────────────────────────────────────┤
│ ■BRUSH│ PRESET ▣ Round Bristle 03 │ SIZE ── 48px │ HARD …  │ 46 px  ← surface 1
├──────┬──────────────────────────────────────┬──────────────┤
│ tool │                                      │ COLOR        │
│ 2×n  │            canvas + rulers           │ ►BRUSH SET.  │  ← surface 2
│ grid │              (surface 3)             │ LAYERS       │
│      │                                      │ CHANNELS     │
│ FG/BG│                                      │ HISTORY      │
├──────┴──────────────────────────────────────┴──────────────┤
│ 64% │ 2048×1536 · LIN16 │ 214 MB / 512 MB │ WET 1 of 4     │ 26 px
└────────────────────────────────────────────────────────────┘
  104 px                                          322 px
```

Desktop only, minimum 1366×1024. Input is a pen with **pressure, tilt magnitude, tilt
azimuth and barrel rotation**, plus mouse fallback and keyboard modifiers. **No
multi-touch** — anything the design would express as a second finger becomes a modifier key.

---

## 3. Visual tokens — use these exact values

| role | value |
|---|---|
| chrome base — panels, palette, status bar | `#2d2b2b` |
| chrome deep — tool options bar, active tab, navigator | `#201e1d` |
| chrome mid — tab strip, internal fills | `#444141` |
| rule — 2 px, between major regions | `#f3f2f2` |
| divider — 1 px, internal | `#444141` |
| hairline, rulers | `#9b9797` |
| text primary | `#f3f2f2` |
| text secondary | `#9b9797` |
| accent — active tool, dirty marker, selection | `#ff563c` |
| row selected | `#7c1405` |
| canvas paper | `#f8f4f4` |
| on-accent foreground | `#201e1d` |

- **2 px `#f3f2f2`** rules between major regions; **1 px `#444141`** internally.
- Type is **Archivo** (400 / 600 / 800); `ui-monospace` for **all numerics and caps labels**;
  800-weight caps carry `.10–.14em` tracking.
- **Every numeric is monospace, right-aligned, in a fixed-width cell.** Brush values change
  live under the pen; a proportional font makes the whole bar judder mid-stroke.
- The tool options bar sits on **chrome deep** `#201e1d`, not on chrome base.

---

## 4. Vocabulary — use these words, and only these

The project has a settled glossary and the UI is expected to speak it. Getting these wrong
is the most likely way a design misdescribes the product.

| term | means | do not say |
|---|---|---|
| **Stroke** | one continuous gesture, pen-down to pen-up. **The unit of undo.** | — |
| **Dab** | one stamp of the tip, emitted every `spacing × radius` **pixels of arc length** — never per input event, never per frame | "splat", "stamp per sample" |
| **Link** | one entry in the dynamics matrix: a source driving a target through an editable response curve | "dynamic", "modifier" |
| **Deposit** | the final per-dab write — the only part of the brush that differs by layer kind | — |
| **Flow** | per-dab deposition | — |
| **Opacity** | a ceiling on the **whole stroke**, not a per-dab value. Distinct from Flow, and the distinction is real | — |
| **Load** | pigment concentration the brush carries | — |
| **Smudge** | reading the destination under the tip and re-depositing it nearby. **Stateless per dab.** Works on RGB and Pigment layers | — |
| **Wet mix** | a *persistent* reservoir that loads colour off the canvas and unloads it, running dry. **Media layers only** | conflating with Smudge |
| **Erase** | a deposit with a negative sign | "erase to white", "paint the background colour" |
| **Blot** | removing **film** and **saturation** while leaving **deposit** — lifting wet paint with a dry brush or tissue. A distinct mode from Erase, because no physical act does both | — |
| **Film / saturation / deposit** | the three physical strata inside a Media layer: standing water, capillary reservoir, dry pigment | "the wet layer" |
| **Working time** | how long paint stays wet and workable | — |
| **Solver window** | the transient allocation the fluid solve actually runs in — a document-space box around the wet region, capped at 1024² | "sim canvas", "wet window" |
| **Bake** | the one-way conversion of live state into stored tiles — drying | "flatten" (that is the layer stack), "commit" |
| **Latent / Mass** | what a pigment pixel is: a pigment decomposition, premultiplied by how much paint is there | "pigment colour" |

---

## 5. The structural facts a designer must not get wrong

1. **Dabs are spaced by distance, not time.** A slow stroke and a fast stroke over the same
   path lay down identical paint. Nothing in the UI should imply a rate-per-second.
2. **One stroke is one undo step**, on every layer kind, including simulated media (where
   undo replays the dab stream from a keyframe rather than storing a before-image).
3. **Flow and Opacity are different controls** and both belong on the tool options bar.
   Flow is per dab; Opacity caps the accumulated stroke. `1`–`0` set opacity, `⇧1`–`⇧0` set
   flow.
4. **Every dynamic is a Link.** None are special-cased, none are hardcoded, there is no
   "pressure controls size" checkbox that is different in kind from "velocity controls hue".
   A design that gives pressure its own privileged widget contradicts the architecture.
5. **Jitter is deterministic**, seeded per `(stroke, dab)` — so the live preview matches the
   final composite exactly, and undo/redo reproduce the same marks. "Random" here is
   reproducible, and that is worth saying somewhere in the UI.
6. **`[` and `]` are unreachable with a pen in the right hand.** Photoshop has this problem
   and never fixed it. The **`⌃⌥`-drag on-canvas gesture is the primary path** for size and
   hardness — not a convenience. Every frequently used painting shortcut must be reachable
   by the off-hand alone, in the keyboard's left half.
7. **Pen-to-photon under 20 ms**, and the in-progress stroke does not wait on a full
   document re-composite. Nothing in the design may sit between the pen and the mark —
   no modal that appears mid-stroke, no control that must be read to know what will happen.

---

## 6. Capability inventory

Status is honest: **Built** exists in the running application today; **P0/P1/P2** are
specified requirements at that priority, not yet built.

### 6.1 The tip and the stroke

| capability | status | notes |
|---|---|---|
| Arc-length dab emission at `spacing × radius` | **Built** | Centripetal Catmull-Rom through the last four samples; leftover distance carried across frames |
| Procedural round tip with hardness | **Built** | Hardness is the fraction of the radius that is a flat, fully-covered core; 0 = pure smoothstep, 1 = hard disc |
| Greyscale stamp-texture tips | **P0** | |
| Flow (per-dab deposition) | **Built** | |
| Opacity (whole-stroke ceiling) | **P0** | |
| Spacing control | **Built, no UI** | Default 0.25 radii; most painting apps sit in 0.1–0.3 |
| Pressure → size, pressure → flow | **Built, as two booleans** | To be replaced by the Link matrix — see 6.2 |
| Taper, stabilisation, scatter, count | **P1** | |
| Straight-line constraint (`⇧` drag) | **P0** | |
| Pen-to-photon < 20 ms | **Built / measured** | p50 12.1–12.4 ms, p99 15.7–16.4 ms |
| One stroke = one undo step | **Built** | |

Current defaults, for realistic mock data: **radius 24 px · hardness 0.35 · flow 0.35 ·
spacing 0.25 radii · load 0.9 · water 1.3 · working time 15 s · pigment: Ultramarine Blue.**

### 6.2 The dynamics matrix — the centrepiece

**Every** brush dynamic is a uniform **Link**: one source → one target, through an editable
response curve. None are special-cased. This is the single most distinctive piece of UI in
the brush system and the thing most worth designing well.

**Sources** (named in the requirements): pressure · tilt magnitude · tilt azimuth · barrel
rotation · velocity · fade (position along the stroke) · noise · random (per dab).

**Targets** (named): size · angle · flow · hue. The matrix is uniform, so any parameter the
deposit step reads is a legal target — expect the full set to include at least: size, angle,
roundness, hardness, flow, scatter, spacing, count, hue, saturation, value, and on Media
layers wetness and load.

That is on the order of **8 sources × 12 targets**, each cell either empty or a link
carrying its own response curve. The application already has a curve editor (used for tone
curves in the grading stack) and the response curve editor should read as the same widget.

> **Design question — the biggest one in this brief.** A 96-cell matrix does not fit a 322 px
> panel, and the three obvious prior arts each fail differently: Photoshop's checkbox-list
> panes hide which links are live; Krita's per-target list buries the matrix idea; a big
> modal breaks the "nothing between the pen and the mark" rule. Produce **two** answers —
> a compact always-visible representation that shows *at a glance which links are active*,
> and an expanded editor for authoring one link's curve.

> **Design question.** A link is live *right now* while the user paints. Is there a state
> where the matrix shows live source values (this pressure, this tilt, this velocity) as the
> pen moves? It would make the matrix teachable, and it is the kind of thing the fixed-width
> monospace numeric cells exist for.

### 6.3 What a dab does on each of the seven layer kinds

One brush; the deposit step differs. The UI must make the *current* target legible, because
the same gesture does genuinely different things.

| layer kind | what a dab does | what erase does |
|---|---|---|
| **Pigment** (default) | Deposits latent × mass; colour mixes under Kubelka–Munk at full interactive speed | Reduces **mass**, leaves the latent untouched — a half-erased mark is *less paint of the same colour* |
| **RGB** | Ordinary premultiplied RGBA deposit | Reduces alpha |
| **Media** | Injects into the fluid solver — film, saturation and deposit | Removes deposit (and **Blot** removes film and saturation, leaving deposit) |
| **Strokes** | Records a clone/heal dab; replayed against the composite below | **Deletes the dab records it covers, not pixels** |
| **Adjustment** | Paints the layer's mask — this is how dodge and burn work, never as a destructive pixel op | Paints the mask back |
| **Text** | Paints the mask | Paints the mask |
| **Flats** | Recolours fills — the brush is a recolour brush, and the tool palette switches to the flatting set | — |

**Refusals must be visible, not silent.** A locked layer refuses the stroke; so does a kind
with no deposit path. The application's rule everywhere is that a refusal is *a sentence
with the numbers in it*, never a dead click.

> **Design question.** Where does "this brush, on this layer, will do X" live? The layers
> panel shows kind; the tool options bar shows the brush. Neither currently says what the
> combination means, and the eraser on a Pigment layer versus on a Strokes layer is a
> genuinely different act.

### 6.4 The brush is loaded with a pigment

Pigment selection sets **physical constants**, not just a colour:

- **density** — settles out of suspension faster when high
- **staining** — resists lifting; a stain will not wash back out
- **granulation** — pools into the paper's valleys

So Phthalo Blue (staining, no granulation) and Ultramarine (granulating, lifts easily)
behave differently at the same RGB. The COLOR panel therefore has two modes — **PIGMENT**
(a pigment well plus a mixing tray, showing those three constants) and **RGB** — and defaults
to PIGMENT on a Pigment or Media layer. Choosing a raw RGB colour there is still allowed;
it maps through an RGB→latent decomposition that is plausible rather than true.

The brush settings surface must show **what the tip is loaded with**, and it must be the
same visual object as the COLOR panel's pigment well.

### 6.5 Erase, smudge and blot are modes of the same brush

- **Eraser** — the brush with a negative deposit, inheriting the whole Link matrix. `E`.
- **Smudge** — reads the destination under the tip and re-deposits nearby; stateless per
  dab, so it works on RGB and Pigment layers, and on a Pigment layer it mixes under
  Kubelka–Munk. `N`.
- **Wet mix** — a persistent reservoir that carries colour and runs dry. **Media only.**
- **Blot** — removes film and saturation, leaves deposit. **Media only.** `⇧E`.

> **Design question.** Four related behaviours, two of which only exist on one layer kind.
> Are they separate tools in the palette, or modes on one brush in the tool options bar?
> The keymap assigns separate keys, which argues for tools; the architecture says it is one
> brush, which argues for modes. Show both and make the trade-off visible.

### 6.6 Simulated media — wet paint has a budget and a deadline

Watercolour fields cost **184 bytes per pixel**. A full-document Media layer would be
1.53 GB at 4K, so the solver runs in a **transient window** — a document-space box around
the currently-wet region, **capped at 1024²**, with a layer holding a small number of them
(2–4) and baking the oldest when it needs another.

Three consequences the UI is *required* to surface:

1. **Working time.** Paint stays wet for a set period (default 15 s, adjustable 1–20 s) and
   then dries. A Media layer row shows the remaining time; the brush surface owns the
   setting.
2. **Exceeding the cap refuses further wetting, visibly.** A silent bake would look like the
   simulation had broken. The Water tool can hit this in ordinary use.
3. **Wet state does not survive closing the document.** Only the dried extent is saved.

Undo on a wet Media layer replays the dab stream from a solver keyframe (kept roughly every
2 s) rather than restoring a before-image — so undo there has a real, non-zero cost.

> **Design question.** Design the wet budget as a first-class, always-visible indicator —
> windows in use, how full, what happens at the cap — without making it read as an error
> condition during normal painting. The status bar has room for a compact form; the brush
> panel has room for the full one.

### 6.7 The physics controls already exist, and there are a lot of them

Today's application exposes the three solvers' real parameters as sliders, grouped by
medium. These are real, working controls with real defaults:

- **Watercolour** — Density 0.12 · Staining 0.60 · Granulation 0.45 · Diffusion 0.25 ·
  Edge darkening 0.30 · Paper slope 0.9 · Viscosity 0.05 · Drag 0.12 · Evaporation 0.004 ·
  Max film 2.2 · Capillary diffuse 0.85
- **Oil** — Brush load 1.0 · Pressure 0.55 · Squish 1.4 · Transfer 0.10 · Max transfer 0.02 ·
  Levelling 0.05 · Impasto light 0.65 · Adhesion 0.06
- **Ink** — Relaxation 0.70 · Blocking 0.10 · Grain block 0.22 · Glue 0.04 ·
  Receptivity 1.20 · Settle rate 0.008

> **Design question.** Thirty-odd physical parameters is a physics laboratory. It is correct
> for the primary user and hostile as a default. Design the disclosure: which handful are
> *artist* controls that belong on the tool options bar, which are *paint* controls in the
> panel, and which live behind an "advanced / physical parameters" tier. Note that a medium
> is a per-layer property chosen at Media-layer creation — it is not a global mode, so these
> groups are contextual to the active layer.

### 6.8 Presets, and imports that tell the truth

- Presets **save, load and carry a thumbnail** (P1).
- **`.abr` import including dynamics, not tips alone** (P1). Most importers take the tip
  bitmap and drop everything else.
- **An import emits a report naming everything it dropped**, rather than silently
  approximating (P1). This is a designable dialog and a genuine differentiator: the honest
  answer to "your 300-brush pack imported, here are the 41 things that did not survive".
- `.brush` / `.brushset` import (P2).
- `,` and `.` step to the previous and next preset.

**The PRESET dropdown in the tool options bar is currently drawn as empty on purpose** —
there are no presets yet, and the project's rule is that a control with nothing behind it is
absent rather than faked. The design should show it populated, but know that it lands last.

### 6.9 The tool family

Target keymap, unmodified single keys, matching Photoshop wherever Photoshop has an
assignment:

`B` Brush · `E` Eraser (`⇧E` Blot, Media only) · `N` Smudge · `S` Clone · `J` Heal ·
`O` Dodge (`⇧O` Burn) · `G` Gradient (`⇧G` paint bucket) · `I` Eyedropper · `Q` Quick mask

Plus, scoped to a Flats layer only: `B` becomes the recolour brush, `G` becomes the fill
carve, and `K`/`U`/`Y`/`⇧K`/`⇧U`/`⇧B`/`⇧V` bind to flatting operations. **A scoped set must
be visible** — the tool palette shows the flatting tools when a Flats layer is active. A
silent modal keymap is worse than an awkward global one.

Tool cells are **50 px in a 2-wide grid** in a 104 px column, and every cell shows its
shortcut letter on hover and in its tooltip.

> **Open question, flagged rather than answered.** Today's build has **Water** (pre-wet the
> sheet) and **Dry Brush** tools that the target keymap has no place for. They are real and
> they matter for watercolour. Do they become tools with keys, brush modes, or Media-scoped
> entries in the palette? This has not been decided and the design is a good place to argue
> it.

### 6.10 On-canvas control — the primary path, not a convenience

- **`⌃⌥` drag** adjusts size and hardness under the pen, on the canvas, without the off-hand
  leaving its resting position. This is the primary path.
- **The brush cursor** shows the tip's actual footprint — size, roundness and angle — and
  `Caps Lock` switches to a precise crosshair.
- `⌥` held is a temporary eyedropper, including mid-stroke.
- `[` / `]` size, `⇧[` / `⇧]` hardness, `1`–`0` opacity, `⇧1`–`⇧0` flow, `X` swap
  foreground/background, `D` default colours.

> **Design question.** Design the size/hardness drag HUD. It appears under the pen, must be
> readable against *any* canvas content including white paper and black ink, must show both
> values changing simultaneously, and must not obscure the area being sized. It is the most
> frequently seen piece of UI in the application after the cursor itself.

---

## 7. Artboards to produce

1. **Tool options bar, Brush active** — preset, size, hardness, flow, opacity, spacing,
   smoothing, mode. Full width at 46 px.
2. **Tool options bar, Eraser active**, and **Smudge active** — showing how much changes.
3. **BRUSH SETTINGS panel, default state** — tip, loaded pigment, the compact dynamics
   matrix, at 322 px.
4. **The dynamics matrix, expanded** — one link selected, its response curve being edited,
   live source values if you take that option.
5. **Media brush** — the panel with a Media layer active: medium-specific physics,
   working-time control, wet-window budget.
6. **Wet budget at the cap** — further wetting refused, visibly, without reading as a crash.
7. **On-canvas HUD** — `⌃⌥` drag over dark paint and over white paper, both.
8. **Brush cursor states** — round, tilted/elliptical, precise crosshair, and at a size
   larger than the viewport.
9. **Preset browser** — a populated grid with thumbnails, plus the empty state (no presets
   yet, which is today's truth).
10. **Import report** — 300 brushes imported, 41 things dropped, named.
11. **Refusal** — a stroke attempted on a locked layer, and on a layer kind with no deposit
    path, with the sentence the application would show.
12. **Tool palette, Flats layer active** — the scoped flatting set replacing the global one.

---

## 8. Do not

- Do not give pressure a privileged widget separate from the other Links. Every dynamic is
  the same kind of thing; that uniformity is the design.
- Do not design an eraser that picks a colour. Erase is a negative deposit.
- Do not merge Smudge and Wet mix into one control. One displaces colour, the other carries
  it, and only one of them exists on a Media layer.
- Do not imply time-based deposition anywhere — no "paint per second", no rate meters on
  flow.
- Do not put anything modal, blocking or read-required between the pen and the mark.
- Do not show a preset dropdown, a grain picker, or any other control whose feature does not
  exist as anything but an empty state. The project's rule is that a control with nothing
  behind it is absent rather than faked.
- Do not describe simulated media as a brush *type*. The medium is a property of the layer;
  the brush is the same brush.
- Do not use corner radius, gradients (outside the colour picker), drop shadows or blur.
