# naturalPaint

A painting and image-processing application built on WebGPU, whose distinguishing
capability is that colour mixes as real pigment does (Kubelka-Munk) rather than as
RGB. Both painting and image processing are first-class; neither is a follow-on to
the other.

## Language

### Product properties

**Lightweight**:
A property of *resource behaviour*, not of feature count — fast cold start, and
near-zero memory when no document is loaded.
_Avoid_: "minimal", "simple", "small" — those imply a bounded feature set, which
is explicitly **not** what is meant. A feature-rich application is lightweight if
nothing allocates until used.

**Idle**: The state of the application with no document loaded. Memory in this
state is the headline number the term *Lightweight* is measured against.

### Colour

**Working space**:
Linear-light RGBA, sRGB/Rec.709 primaries by default, stored `rgba16float`. The
space every operation is defined in; files are decoded into it on import and
encoded out of it on export.
_Avoid_: "linear sRGB" (ambiguous — sRGB names a transfer function *and* a set of
primaries), "scene-referred" (true but imports ACES baggage not yet adopted).

**Latent**:
A Mixbox pigment decomposition — three pigment weights plus an additive residual —
chosen so that *linear combinations of latents are Kubelka-Munk mixes*.
_Avoid_: "pigment colour", "KM colour". The latent is a representation; the pigment
is what it represents.

**Mass**: The quantity of pigment at a point. Latents are always stored
premultiplied by mass, so varying concentration mixes correctly.

**Pigment basis**:
Which pigment model produced a **Latent** — `mixbox-v1` today, `km2-v1` for the
unencumbered fallback, and a future `np-km-v1` for the reimplementation. Bases are
mutually unreadable: latents from one are meaningless to another, and silently so.
Every saved document stamps its basis.
_Note_: nothing outside the pigment module may assume a basis has three weights plus a
residual — that is a Mixbox detail, not a property of **Latent**.

**Shaper**:
A 1-D log encoding applied before a 3-D LUT, so that linear values above 1.0 fit
the LUT's [0,1] domain and grading control points land where a user expects them.

### Layer kinds

Seven kinds. The three raster kinds — **Pigment**, **RGB**, **Media** — are all
paintable, and are named for the property that actually differs between them: **how
colour combines.** The other four — **Strokes**, **Adjustment**, **Text**, **Flats** —
hold no pixels of their own and are evaluated from parameters.

**Pigment layer**:
A layer whose pixels are **Latent**s, so colour combines under Kubelka-Munk. The
default kind for a new layer.
_Avoid_: "standard layer" — accurate as user-facing shorthand, but in code and
documentation it invites the Photoshop reading (that standard means RGB), which is
the opposite of what is meant here.

**RGB layer**:
A layer whose pixels are **Working space** RGBA, so colour combines by ordinary
interpolation. Used for imported images, image processing, and deliberate
RGB painting. A first-class choice, not only an import artefact.
_Avoid_: "image layer", "normal layer", "flat layer".

**Media layer**:
A **Pigment layer** whose contents are advanced by the fluid solver — watercolour,
oil or ink. The simulated one.
_Avoid_: "sim layer" (fine in conversation, but *Media* is the canonical term),
"wet layer" (that already names a physical stratum inside the solver).

**Adjustment layer**: A layer holding no pixels — only an op stack applied to the
composite accumulated beneath it.

**Text layer**:
A layer holding no pixels — a string, a font reference and layout parameters,
rasterised at evaluation time. Parametric like an **Adjustment layer**, so the text
stays editable and re-layout is free.

**Strokes layer**:
A layer holding no pixels — only an ordered list of clone and heal **Dab**s, replayed
at evaluation time against the composite *below* it, so the result re-derives when
anything underneath changes. It never samples itself; clone-from-clone is expressed
by stacking a second Strokes layer.

**Flats layer**:
A layer holding no pixels of its own — segmentation parameters plus a list of recorded
repairs, evaluated against the line art *beneath* it to produce one **Fill** per enclosed
region. Like a **Strokes layer** it re-derives when what is underneath changes, which is
what makes editing the line art re-**Flat** the drawing. Its colour is RGB, not
**Latent**: flat colour does not mix, so routing it through pigment would add a
plausible-but-untrue decomposition for no benefit.

**Layer comp**:
A named, saved set of layer visibility, position and properties, restorable in one click
and stored in the document. Comps **export**, one file per comp — that is what they are
mostly *for*, rather than for switching between in the app.
A view of *one* document's state, so it is not a **Snapshot** (a history entry) and not a
version of the file.

**Mix**:
A blend mode that combines two **Pigment layer**s by interpolating their
**Latent**s, so the result is a Kubelka-Munk mix rather than a composite.
_Distinct from_ **Opacity**, which always means transparency on every layer kind.
Mixing between layers is opt-in; mixing *within* a layer, between brush dabs, is
unconditional.
_Avoid_: calling this "glaze" — a glaze is a translucent layer over a dry one,
which is the KM *layering* equation and needs explicit K and S values the Mixbox
latent does not carry.

### Painting

**Stroke**: One continuous gesture, pen-down to pen-up. The unit of undo.

**Dab**:
One stamp of the brush tip, emitted every `spacing × radius` pixels of arc length
along a **Stroke** — never once per input event, and never scaled by frame time.

**Link**:
One entry in the dynamics matrix: a source (pressure, tilt, velocity, fade, noise,
random) driving a target (size, angle, flow, hue…) through an editable response
curve. *Every* brush dynamic is a Link; none are special-cased.

**Deposit**:
The final per-**Dab** write, and the only part of the brush engine that differs
between layer kinds — RGBA for an **RGB layer**, latent × mass for a **Pigment
layer**, solver injection for a **Media layer**.

**Smudge**:
Reading the destination under the tip and re-depositing it nearby. Stateless per
**Dab**, so it works on **RGB** and **Pigment** layers — and on a Pigment layer it
mixes under Kubelka-Munk.

**Wet mix**:
A brush carrying a *persistent* reservoir that loads colour off the canvas and
unloads it, running dry as it goes. Requires the reservoir plus a film depth and
contact mask, so it is **Media layer only**.
_Distinct from_ **Smudge** — conflating the two overstates what a Pigment layer can
do. Smudge displaces colour; Wet mix carries it.

**Erase**:
A **Deposit** with a negative sign — the same brush, the same **Link** matrix, removing
instead of adding. On a **Pigment layer** it reduces **Mass** and leaves the **Latent**
untouched, so a half-erased mark is *less paint of the same colour*.
_Avoid_: "erase to white", "paint the background colour" — white is a pigment, so on a
Pigment layer that adds paint rather than removing it.

**Blot**:
Removing the **film** and **saturation** from a **Media layer** while leaving the
**deposit** — lifting wet paint with a dry brush or tissue. A distinct brush mode from
**Erase**, which removes deposit, because no physical act does both at once.

**Snapshot**:
An explicit, user-created history entry holding a full document state, exempt from the
byte-bounded eviction that trims the automatic history tail.
_Distinct from_ a **Layer comp**, which records layer *properties* rather than history
position, and from a saved file.

### Selections

**Selection**:
A single-channel *coverage* mask over document space, stored in the same sparse tile
machinery as everything else. Coverage rather than a bitmask, so every operation it
gates has antialiased edges.
_Note_: a Selection and a layer **Mask** are the same data in different roles, so
converting either way is free.

### Simulation

**Solver window**:
The transient allocation in which a **Media layer**'s fluid solve actually runs — a
bounding box in *document* space around the layer's currently-wet cells, capped at
1024².
_Avoid_: "sim canvas", "wet window" ("wet layer" already names a physical stratum
inside the solver).

**Wet extent** / **Dry extent**:
A **Media layer**'s wet extent is the region still being simulated; its dry extent
is what has been **Bake**d into tiles. Only the dry extent is saved — wet state does
not survive closing a document.

**Bake**:
The one-way conversion of derived or live state into stored tiles: drying paint into
a **Media layer**'s dry extent, or a **Latent** into **Working space** RGBA.
_Avoid_: "flatten" (that is the layer-stack operation), "commit".

### Flatting

Absorbed from autoFlats — see [docs/autoflats-migration.md](docs/autoflats-migration.md).

**Flat**:
The result of segmenting line art into enclosed regions and giving each one a single
colour. Also the verb: *to flat* a drawing.
_Avoid_: "flatten" — a **Flat** is produced by segmentation, and flattening is what the
layer stack does. The two words must not be interchangeable.

**Fill**:
One region of a **Flat** — an id in the label field, a colour, a name, and optionally a
group and a palette swatch. Hundreds per drawing, and **not** a **Layer**: fills live in
a Flats layer and are rows in the Fills panel, becoming layers only on export.

**Ink mask**:
The binary judgement "this pixel is line art", derived from the line-art layer by
darkness and desaturation. Computed in the **display domain**, never in
**Working space**.

**Bridge**:
An invisible barrier the artist or the app places across a break in a stroke, so fills
stop at it. Constrains segmentation; never rendered and never exported.
_Avoid_: "gap" for the bridge itself — a gap is the *break*, a bridge is what closes it.

**Sag field**:
The height of a membrane pinned at every ink pixel and pulled down by gravity — a
Poisson solve. Roominess grows with the square of available space, so a drawn area is a
deep valley and a stroke break is only a shallow col between two of them. That single
fact is why fills do not leak through gaps.

**Ridge**:
A watershed boundary between two **Fill**s. On ink, the artist drew that boundary; over
blank paper, it is a gap the segmenter closed by itself.

**Stroke orientation field**:
The local direction of the line work, used to check that a candidate **Bridge** runs
*with* the strokes rather than across them. Not **Flow**, and not velocity — see the
resolved collision below.

**Relatable**:
Kellman-Shipley: two stroke tips can be joined by a smooth, monotonic curve bending no
more than ~90° with no inflection. A candidate **Bridge** that is not relatable is
rejected before it is ever scored.

## Relationships

- A **Document** holds an ordered list of **Layer**s and a **Working space**
- A **Pigment layer** and a **Media layer** are both *paint layers*; a **Media
  layer** is the simulated one
- A **Media layer** is a **Pigment layer** with a solver attached — not a separate
  storage format
- Any **Layer** kind may be painted on; the brush's deposit step differs per kind,
  nothing else does
- A **Layer** holds **Tile**s; tiles are allocated only where content exists
- A **Latent** is only meaningful alongside its **Mass**
- Linear operations are valid on **Latent**s; non-linear ones require a bake to
  **Working space** first — this is the invariant the whole pigment design rests on

## Example dialogue

> **Dev:** "If we add the brush engine and two format importers, is it still
> **Lightweight**?"
>
> **Domain expert:** "Yes — **Lightweight** is about **Idle** cost and start time,
> not feature count. Add whatever you like, as long as none of it allocates until
> someone uses it."
>
> **Dev:** "So a blur on a pigment layer — that works directly on the **Latent**s?"
>
> **Domain expert:** "Blur is a linear combination, so yes. Curves aren't, so those
> need a bake to **Working space** first."

## Flagged ambiguities

Unresolved term collisions, mostly because the existing simulation code already
uses these words for other things. Each needs a canonical meaning:

- **composite** — layer compositing, vs. `composite.wgsl`, which converts latent →
  RGB
- **tile** — a document storage tile, vs. a tileable texture
- **fill** — a region of a **Flat**, vs. the fill *operation* (fill with colour).
  Leaning toward keeping both: "a Fill" is a noun in flatting and "fill it" is a verb
  everywhere else, and no sentence has yet been ambiguous. Watch it.

### Resolved

- **flow** — **Flow** means **brush flow** — per-dab deposition — and nothing else.
  Forced by the autoFlats merge, which brought a third meaning: a stroke-direction
  field that gap suggestions are checked against. Three was past tolerance. The fluid
  field in `flow_outward.wgsl` is the **velocity** field; the line-art direction field
  is the **stroke orientation field**.

- **brush** — *not* a collision. A **Brush** is the thing used to draw a **Stroke**.
  A preset is a Brush serialised; a tip is a component of one (`Brush::tip`); and
  the reservoir textures read naturally as *the brush's* paint (`brushVol`,
  `brushC`, `brushR`). Tip footprint and reservoir resolution already have distinct
  names (`brushRadius` vs `BRUSH_GRID`). Previously flagged in error.
- **layer** — **Layer** means a document layer and nothing else. The simulation's
  physical strata get their own names, matching the fields they already live in:
  the **film** (shallow-water depth, `water.z`), the **saturation** (capillary
  reservoir, `sat`), and the **deposit** (dry pigment, `dep*`). Do not write "the
  wet layer" or "the deposited layer" — a **Media layer** *contains* film,
  saturation and deposit, so using "layer" for both makes sentences like "bake the
  wet layer into the layer" possible.
- **stroke / dab / splat** — a **Stroke** is the user's gesture, pen-down to
  pen-up. A **Dab** is one stamp of the tip at one point along the stroke path,
  emitted every `spacing × radius` pixels of arc length. "Splat" is no longer a
  distinct concept — it survives only as the filename of the shader that deposits a
  **Dab** onto a **Media layer**.

- **paint layer types** — three kinds, named for how colour combines: **Pigment
  layer** (the default), **RGB layer**, **Media layer**. "Standard" is user-facing
  shorthand for *Pigment layer*, and is avoided in code because it reads as RGB to
  anyone arriving from Photoshop.
