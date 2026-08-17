# naturalPaint — Product Requirements

**Status:** draft · **Owner:** Chris Harvey · **Last updated:** 2026-08-17

Engineering design lives in [DESIGN-imaging.md](DESIGN-imaging.md); terminology in
[CONTEXT.md](CONTEXT.md); settled decisions in [docs/adr/](docs/adr/). This document
states *what* must be true and how it will be verified. Where it and the design doc
disagree about mechanism, the design doc wins; where they disagree about
requirements, this one does.

---

## 1. Summary

A painting and image-processing application for visual development and texture work,
distinguished by two things the incumbent does not do: **colour that mixes as real
pigment does**, and **staying resource-light** — fast cold start, near-zero memory at
rest, and many documents open without consuming the machine.

Built on the existing naturalPaint codebase: WebGPU compute via wgpu-native, SDL3 for
tablet input, Dear ImGui, C++20.

---

## 2. Problem

Photoshop is the incumbent for both target jobs, and fails at three specific things.

**It is heavy.** Eager loading means gigabytes at rest and slow cold start,
irrespective of what the user is doing.

**Its colour mixing is wrong.** Blending is RGB interpolation, so blue over yellow
goes grey rather than green. The Mixer Brush is a partial patch over a model-level
problem. *"Digital colour mixing goes muddy"* is a standing complaint in visual
development.

**It composites in gamma space by default.** Blurs, gradients, resamples and
composites are therefore subtly incorrect — the reason its default blurs look muddy.

### Why this codebase

The hard half already exists and is verified working: Mixbox latent pigment transport
with Kubelka-Munk mixing, three physically-based media solvers (Curtis '97, IMPaSTo,
MoXi), conservative advection with measured mass conservation, and a WebGPU compute
pipeline with shader hot-reload. What is missing is everything *around* it —
documents, layers, save/load, undo, a brush engine.

The `README.md` "Not done yet" list ends with *save / load, layers, undo*. This
document specifies that foundation and what it makes possible.

---

## 3. Users and jobs

| | who | job |
|---|---|---|
| **Primary** | the author | Prepare photos and textures for 3D work: inspect, grade, clone out defects, remove lighting gradients, make tileable, export at depth |
| **Secondary** | visdev / concept painters | Mark-making — block in, build up colour, paint for hours with predictable, responsive brushes |

Both are first-class. Painting is not a follow-on to image work, nor the reverse.

---

## 4. Product principles

Five invariants. Requirements that conflict with these are wrong by default.

1. **Lightweight is a resource property, not a feature limit.** Fast start and
   near-zero idle memory. A feature-rich application is lightweight if nothing
   allocates until used. Features are never cut *for* this reason — they are made
   lazy instead.
2. **Linear working space, always.** Every operation that averages pixels is defined
   on linear light. Files decode in, encode out.
3. **Pigment by default.** A new layer mixes as paint. The differentiator is on
   without anyone having to discover it.
4. **Non-destructive where it is free; honest where it is not.** Parametric and
   replayable operations stay live. Operations that genuinely cannot replay say so
   rather than pretending.
5. **Measured, not asserted.** `--diag` and `--selftest` are the acceptance
   mechanism, not a supplement to it.

---

## 5. Non-goals

Explicitly out of scope. Photoshop's moat is a long tail plus PSD interchange plus a
plugin ecosystem; attacking it head-on is how every competitor has lost.

| not doing | why |
|---|---|
| **Full-fidelity layered PSD** | Native format is EXR ([docs/document-format.md](docs/document-format.md)); PSD is a deliberate export, flattened or simply-layered. Native `curv`/`levl`/`TySh` blocks are never written — our curves live in the shaper log domain (ADR-0004), so one would be *silently wrong* rather than approximately right. |
| **ML matting — select subject, select sky, refine edge** | A model, its weights, its licence and its inference runtime, for a result that the wand plus quick mask (E12) reaches manually. Explicitly declined. |
| **Artboards** | Multiple canvases in one document. Serves UI design, not painting or texture work. |
| **Measure / ruler tool** | Declined. The pixel probe and the rulers cover what it was for. Removed from the wireframe scope it arrived in. |
| **Print, soft proofing and prepress** | Follows the CMYK non-goal below — output here is files, not paper. The useful half of soft proofing for this audience became the grayscale value check, Q3. |
| **Erase-to-history, background eraser, magic eraser** | Erase-to-history needs the non-linear history ADR-0005 declines. The other two are selection tools in an eraser costume — the wand plus delete does the job with fewer surprises. |
| **Liquify, artistic / sketch / texture filter sets** | Large subsystems serving neither target job. The incumbent's own users mostly do not use them. See [docs/operations.md §7](docs/operations.md). |
| **Lighting effects, 3D, video and timeline** | Out of scope. |
| **Content-aware scale** | PatchMatch inpaint (D7) covers the case that actually comes up. |
| **A camera-raw pipeline** | OIIO reads raw files; demosaic, lens profiles and raw-specific tone mapping are their own product. |
| **A second watercolour implementation for flats** | The wash runs the existing solver on a Media layer. autoFlats' 409-line reduced version is deleted, not ported — see [docs/autoflats-migration.md §4.2](docs/autoflats-migration.md). |
| **Text on a path, vertical text, rich-text runs** | The multi-year part of a text engine, and none of it serves annotation. Basic text *is* in scope as of the UI direction — see §6 area K. |
| **Own text shaping** | CoreText does shaping, bidi, cluster breaking and font fallback correctly. Hand-rolling these is where text engines die. |
| ~~Selection → path tracing~~ | Moved to §12 Future work — deferred, not refused. |
| **Slice / web export** | The one tool in the source wireframe with no constituency in visdev or texture work. |
| **CMYK / prepress** | Separations, spot channels, Pantone. Real capability, no overlap with texture or visdev work. |
| **Layer styles, smart objects, arbitrary smart-filter stacks** | The unwinnable part of PSD fidelity, and the live op stack covers the useful 95% at a fraction of the machinery. Neither read nor written. |
| **Wet mix outside Media layers** | Requires film depth and a contact mask a flat layer does not have. Smudge covers the cheap case. |
| **Dual Brush, Brush Pose, Procreate's six Rendering modes** | Import reports them as dropped rather than approximating them. |
| **Re-export of imported commercial brush presets** | Import is a convenience for brushes the user owns, not a redistribution path. |

---

## 6. Requirements

**P0** — no coherent product without it. **P1** — required to be competitive.
**P2** — desirable, deferrable.

### A. Resource behaviour

| ID | Requirement | P |
|---|---|---|
| A1 | With no document open, resident memory is under 40 MB and zero GPU field textures are allocated | **P0** |
| A2 | Cold start to first frame under 100 ms, excluding the WebGPU device request | **P0** |
| A3 | No heavy subsystem is constructed until first use; each releases on document deactivate | **P0** |
| A4 | The simulation allocates only the *active* medium's fields, never all three | **P0** |
| A5 | Documents present as tabs, with an optional split showing two | **P1** |
| A6 | Only *visible* documents hold GPU textures, at most two | **P0** |
| A7 | A hidden document costs metadata only; irreplaceable tiles spill to an `mmap` scratch file | **P1** |
| A8 | A visible-but-unfocused document continues stepping its solver | **P1** |
| A9 | Undo history is bounded in *bytes*, not steps, and the tail spills to scratch | **P1** |

Baseline: the current application allocates **~294 MB** at launch before any document
exists, 100.7 MB of it the ink-only lattice, dead in watercolour mode.

### B. Colour

| ID | Requirement | P |
|---|---|---|
| B1 | Working space is linear light, `rgba16float`, sRGB/Rec.709 primaries by default, with primaries a document property | **P0** |
| B2 | Files decode to working space on import and encode to the target space on export | **P0** |
| B3 | Round trip decode → encode returns the original within tolerance, asserted in `--selftest` | **P0** |
| B4 | Alpha is premultiplied; per-channel colour ops un-premultiply and re-premultiply | **P0** |
| B5 | Curves are authored in a log (shaper) domain so control points land where a user expects | **P0** |
| B6 | Bit depth is preserved end to end; 16- and 32-bit files never silently truncate to 8 | **P0** |
| B7 | Blend modes are chosen for linear space; display-referred modes are labelled as such | **P1** |
| B8 | OCIO configuration support | **P2** |

### C. Documents and layers

| ID | Requirement | P |
|---|---|---|
| C1 | Layer kinds: **Pigment** (default), **RGB**, **Media**, **Strokes**, **Adjustment**, **Text**, **Flats** | **P0** |
| C2 | Memory tracks content, not canvas dimensions — tiles allocate only where content exists | **P0** |
| C3 | Layer opacity means transparency on every kind; KM mixing between layers is the opt-in `Mix` blend mode | **P0** |
| C4 | Layers behave as in Photoshop: reorder, group, opacity, blend mode, per-layer mask, visibility, lock | **P0** |
| C5 | An Adjustment layer applies its op stack to the composite below it | **P1** |
| C6 | Undo is stroke-granular and uniform across all layer kinds | **P1** |
| C7 | A document can be created blank, not only opened from a file | **P0** |
| C8 | Pigment latents survive save/load; the file records which pigment basis produced them | **P1** |
| C9 | **Clipping masks**: a layer or group is clipped by the alpha of the layer below it | **P0** |
| C10 | Merge down, merge visible, **stamp visible** (merge visible to a new layer), flatten image | **P0** |
| C11 | Rasterise a parametric layer — Text, Adjustment, Strokes, Flats — into pixels | **P1** |
| C12 | Multi-select layers; move, transform, group, delete and set properties as a set | **P0** |
| C13 | Align and distribute selected layers, to each other or to the canvas | **P1** |
| C14 | **Layer comps**: named, saved sets of layer visibility, position and properties, restorable in one click, persisted in the document | **P1** |
| C15 | Layer colour labels, linking, and filtering the panel by kind or name | **P2** |
| C16 | A new document's base layer is an ordinary layer with alpha — there is no special locked Background | **P0** |

> **C16 is a deliberate divergence.** Photoshop's Background layer is a compatibility
> artefact that surprises everyone who tries to erase on it: erase paints white instead of
> removing pixels. ADR-0007 makes erase mean removal on every kind, which requires that no
> layer be exempt from having alpha.

### D. Image operations

| ID | Requirement | P |
|---|---|---|
| D1 | Levels, curves, exposure, saturation, RGB→grayscale, channel mixer — live and re-editable | **P0** |
| D2 | Histogram, pixel probe reporting both linear and display values, channel isolation | **P0** |
| D3 | Grade stack depth does not degrade viewport performance | **P0** |
| D4 | Blur, highpass, unsharp — live and re-editable | **P1** |
| D5 | Offset with wrap | **P1** |
| D6 | Clone and heal, which stay correct when layers beneath them are regraded | **P1** |
| D7 | Inpaint: diffusion for scratches and dust; PatchMatch for textured regions | **P1** |
| D8 | Make-tileable: lighting-gradient removal, offset, seam heal, and a 3×3 repeat preview | **P1** |
| D9 | Resample and transform with correct linear-space filtering | **P2** |
| D10 | Colour set beyond D1: gain/offset/gamma, white balance, colour balance, hue/saturation by range, black & white, gradient map, invert, clamp | **P1** |
| D11 | Filter set beyond D4: sharpen, motion blur, add noise, median, dust & scratches | **P1** |
| D12 | Shadows/highlights and local contrast — spatial, not point ops | **P1** |
| D13 | Dodge and burn as a brush painting into an adjustment layer's mask, never as a destructive pixel op | **P1** |
| D14 | Free transform: translate, scale, rotate, skew, 4-corner perspective, with handles | **P1** |
| D15 | Flips and 90° rotations are **exact** — no resample | **P1** |
| D16 | Stacked transforms compose their matrices and resample **once**, from the original pixels | **P1** |
| D17 | Crop, canvas size, image size; downscale prefilters before resampling | **P1** |
| D18 | Every op in §D is re-editable in place unless it changes the pixel grid or document structure; `Bake` is explicit and never automatic | **P0** |
| D19 | Auto-tone, auto-contrast and auto-white-balance as one-click starting points | **P1** |
| D20 | **Straighten**: draw a line along something that should be level, and the image rotates to match | **P1** |
| D21 | **Perspective correction**: rectify keystoning by marking what should be a rectangle | **P1** |
| D22 | Lens correction — barrel/pincushion distortion and chromatic aberration, by parameter | **P2** |
| D23 | **Lattice warp of a selection**: a subdividable mesh with draggable control points | **P1** |
| D24 | Gradient tool — linear, radial, angular; with a gradient editor and saveable presets | **P1** |
| D25 | Paint bucket: contiguous fill with tolerance, and fill-all-similar | **P1** |
| D26 | Fill and stroke a selection or layer with colour, pattern or gradient | **P1** |
| D27 | Define a pattern from a selection; patterns persist and are usable by fill | **P2** |

> **D20–D22 are the photo-prep gap.** A photographed wall needs deskewing and distortion
> removal *before* it can be made tileable, and D14's manual four-corner transform is not
> the same workflow as marking a horizon or a rectangle and letting the app solve for it.

> The 3×3 repeat preview is a **requirement**, not a nicety — tileability cannot be
> judged from a single tile.

The full catalogue, with each entry's op class and cost, is
[docs/operations.md](docs/operations.md). D10–D13 are cheap *because* of their class: the
colour set collapses into the existing LUT, so a document carrying twelve of them costs
what one costs. D11's filters do not — each is a real pass, and motion blur, radial blur
and lens blur cannot use the mip-pyramid shortcut.

> ⚠️ **Add noise must be applied in the shaper domain, not linear.** Fixed-amplitude
> Gaussian noise in linear light is invisible in shadow and enormous in highlight —
> backwards from real grain. Same domain rule as curves (ADR-0004).

### E. Selections

| ID | Requirement | P |
|---|---|---|
| E1 | Every deposit and every op respects the active selection | **P0** |
| E2 | Selections store antialiased *coverage*, not a bitmask | **P0** |
| E3 | Tools: rectangle, ellipse, lasso, polygon lasso, magic wand | **P1** |
| E4 | Feather, grow, shrink, invert | **P1** |
| E5 | Selection ↔ layer mask conversion in both directions | **P1** |
| E6 | Marching-ants display of the selection boundary | **P1** |
| E7 | Boolean combination: add, subtract, intersect | **P1** |
| E8 | Grow and shrink go through a distance transform, so the radius is a real number and antialiasing survives | **P1** |
| E9 | Selection from colour range and from luminance range | **P1** |
| E10 | Selections can be transformed, moving coverage without touching pixels | **P1** |
| E11 | A selection can be saved into the document and restored | **P1** |
| E12 | **Quick mask**: edit the active selection as a paintable overlay with any brush, and convert back | **P1** |
| E13 | Alpha channels stored in the document; load a channel as a selection and save a selection as a channel | **P1** |

> E1 must be honoured by interfaces from the first phase that has ops, even though no
> tool ships until phase 7. Retrofitting a mask gate into every dab and op path later
> is a pervasive change, not a local one.

### F. Painting

| ID | Requirement | P |
|---|---|---|
| F1 | Dabs are emitted by **arc length**, at `spacing × radius` — never per input event, never scaled by frame time | **P0** |
| F2 | Flow controls per-dab deposition; opacity is a ceiling on the whole stroke | **P0** |
| F3 | Pen-to-photon latency under 20 ms; the in-progress stroke does not wait on a full document re-composite | **P0** |
| F4 | Pressure, tilt magnitude, tilt azimuth, barrel rotation and velocity all drive brush parameters | **P0** |
| F5 | One brush works on every layer kind; only the deposit step differs | **P1** |
| F6 | Painting on a Pigment layer mixes colour under Kubelka-Munk at full interactive speed | **P0** |
| F7 | Smudge on RGB and Pigment layers; wet mix on Media layers | **P1** |
| F8 | One stroke is one undo step | **P0** |
| F9 | **An eraser exists**, and it is the brush with a negative deposit — it inherits the full dynamics matrix | **P0** |
| F10 | Erase reduces alpha on RGB layers, **Mass** on Pigment layers leaving the Latent untouched, and deposit on Media layers; on parametric layers it paints the mask | **P0** |
| F11 | Erasing a Strokes layer deletes the dab records it covers, not pixels | **P1** |
| F12 | **Blot**: a Media-layer brush mode removing film and saturation while leaving deposit | **P2** |

> F9–F12 are specified in
> [ADR-0007](docs/adr/0007-erase-is-mass-reduction-not-a-colour.md). Erase is not a brush
> that paints white — on a Pigment layer that *adds* opaque pigment, which is the opposite
> of erasing.

### G. Brush system

| ID | Requirement | P |
|---|---|---|
| G1 | *Every* dynamic is a uniform source→target link with an editable response curve; none are special-cased | **P0** |
| G2 | Jitter is deterministic, seeded per `(stroke, dab)`, so preview matches composite and undo is stable | **P0** |
| G3 | Tips: procedural round with hardness, plus greyscale stamp textures | **P0** |
| G4 | Grain, in both canvas-locked (rolling) and dab-locked (stamp) modes | **P1** |
| G5 | Taper, stabilisation, scatter, count, colour dynamics | **P1** |
| G6 | Presets save, load, and carry a thumbnail | **P1** |
| G7 | `.abr` import including dynamics, not tips alone | **P1** |
| G8 | `.brush` / `.brushset` import | **P2** |
| G9 | Import emits a report naming everything dropped rather than silently approximating | **P1** |

### H. Simulated media

| ID | Requirement | P |
|---|---|---|
| H1 | Watercolour, oil and ink available as Media layers | **P1** |
| H2 | The solver runs in a transient window around the wet region, anchored in document space | **P0** |
| H3 | Panning or switching tabs never destroys wet state | **P0** |
| H4 | Drying bakes into tiles and releases the window | **P0** |
| H5 | Exceeding the window cap refuses further wetting *visibly*, rather than silently baking | **P1** |
| H6 | Media layers inherit the full brush dynamics matrix | **P1** |
| H7 | Solver behaviour is reproducible run to run for identical input | **P1** |

Watercolour fields cost **184 bytes/pixel**, so a full-document Media layer would be
1.53 GB at 4K. H2 is what makes Media layers viable at all.

### I. Interoperability

| ID | Requirement | P |
|---|---|---|
| I1 | Read and write PNG, JPEG, TGA, BMP with no optional dependency | **P0** |
| I2 | Read and write EXR, TIFF, HDR, DPX; read PSD flattened; read camera raw | **P1** |
| I3 | Format support is a runtime capability query, not a build-time hard requirement | **P0** |
| I4 | **The native format is multi-part tiled EXR with a `.npaint` extension** — one part per layer, `HALF` channels, latents as named channels, everything else as typed attributes. Written by OIIO, so native save requires no bespoke writer | **P0** |
| I5 | Export offers target colour space and bit depth explicitly | **P0** |
| I5b | Part 0 is a correct flattened composite, so any EXR reader shows the right image | **P0** |
| I6 | Primaries are declared by the standard `chromaticities` attribute | **P1** |
| I7 | Native files use lossless compression only — never DWAA/DWAB/B44 | **P0** |
| I8 | `.npaint` and `.exr` are the same container, so pipeline handoff is a rename | **P1** |
| I9 | PSD export exists as a deliberate feature: flattened, and simply-layered | **P1** |
| I10 | **Attributes and parts the reader does not understand are preserved verbatim on save**, so an older build cannot destroy a newer document's data | **P0** |
| I11 | A save that would lose data names exactly what, rather than degrading silently | **P0** |
| I12 | The composite part is regenerated on every save, never stale | **P0** |
| I13 | Saves are read back and structurally verified before the original leaves memory | **P1** |
| I14 | An image can be placed into the **open** document as a new layer, by menu or drag-drop | **P0** |
| I15 | Export As: target format, colour space, bit depth **and resize**, with saveable presets | **P1** |
| I16 | Export layers to individual files | **P1** |
| I17 | **Export layer comps to files** — one file per comp, with a name template, a choice of which comps, and I15's format/space/depth/resize options | **P1** |
| I18 | Revert, duplicate document, save a copy, save incremental, open recent | **P1** |

> **I16 and I17 are one feature, not two.** Both are the same loop: set a document state,
> composite, write with a naming rule. Splitting them across phases would mean building the
> mechanism twice. They land together with layer comps in phase 5, not in the automation
> phase — C14/I17 is an established part of the primary user's Photoshop workflow, which is
> the evidence that decides its priority.

### J. Vector paths

| ID | Requirement | P |
|---|---|---|
| J1 | Draw and edit Bézier paths — add, delete, move anchors; convert corner ↔ smooth | **P1** |
| J2 | Path → selection, with antialiased coverage | **P1** |
| J3 | Stroke a path with the current brush | **P1** |
| J4 | Fill a path, honouring the active selection | **P2** |
| J5 | Paths panel: list, rename, save, delete; paths persist in the document | **P1** |

### K. Text

| ID | Requirement | P |
|---|---|---|
| K1 | A Text layer stores string, font reference and layout parameters, and rasterises at evaluation — so text stays editable and re-layout is free | **P1** |
| K2 | Shaping, bidirectional text and font fallback come from the platform (CoreText), behind an interface that HarfBuzz + FreeType could replace | **P1** |
| K3 | Point text and paragraph text, with alignment and leading | **P1** |
| K4 | Text renders correctly at any zoom without re-rasterising visibly | **P2** |

### L. Interface

| ID | Requirement | P |
|---|---|---|
| L1 | The chrome reports the *working space* — `LIN16` / `LIN32` — never a legacy 8-bit mode | **P0** |
| L2 | Every layer row shows its kind, by glyph and in its sub-line | **P0** |
| L3 | Media layer rows show wet state, including the refuse-to-wet warning from H5 | **P1** |
| L4 | The colour panel has RGB and PIGMENT modes; PIGMENT selects physical constants, not just a colour | **P0** |
| L5 | `Mix` appears in the blend dropdown only between two Pigment layers | **P1** |
| L6 | Canvas surround value is user-adjustable | **P2** |
| L7 | The status bar reports real resident memory against the budget | **P1** |

### M. Clipboard and duplication

| ID | Requirement | P |
|---|---|---|
| M1 | Cut, copy, paste, clear — coverage-weighted, leaving premultiply-correct holes | **P0** |
| M2 | Copy merged: composites the visible stack through the selection | **P0** |
| M3 | Paste creates a layer; paste in place preserves document coordinates | **P0** |
| M4 | Selection → new layer, and duplicate layer | **P0** |
| M5 | The internal clipboard holds a copy-on-write tile reference, **not** a flattened buffer, and its cost appears in the status-bar figure | **P0** |
| M6 | Paste decodes to linear: tagged images from their profile, untagged as sRGB | **P0** |
| M7 | Copy writes display-encoded sRGB with a profile attached; a float representation is offered alongside | **P1** |
| M8 | An internal copy-paste takes the internal path and never round-trips the pasteboard, so pigment latents survive | **P0** |
| M9 | Paste into selection, paste as new document, ⌥-drag duplicate, fill with colour | **P1** |

> **M5 is a Lightweight requirement, not a convenience.** A 4K full-document copy is
> 68 MB at `rgba16float`. Photoshop ships a "Purge → Clipboard" command because it holds
> exactly this invisibly. A5 forbids it here.

### N. Flatting

Absorbed from autoFlats — see [docs/autoflats-migration.md](docs/autoflats-migration.md).
An independent chain that starts after phase 7.

| ID | Requirement | P |
|---|---|---|
| N1 | A Flats layer produces one flat colour per enclosed region of the line art beneath it | **P1** |
| N2 | Editing the line art re-flats the drawing | **P1** |
| N3 | Every repair — merge, delete, group, recolour, hand-drawn fill, bridge — is stored as **geometry** and replayed after re-segmentation | **P1** |
| N4 | A region's colour is derived from a point inside it, never from its id, so a parameter change does not repaint the drawing | **P1** |
| N5 | Neighbouring fills never receive the same palette colour (graph colouring) | **P1** |
| N6 | Fills painted from a palette swatch follow that swatch when it is adjusted | **P1** |
| N7 | Gap suggestions must be relatable, must close a region, and must survive simplicity pruning | **P1** |
| N8 | Rubber-sheet segmentation via the existing GPU Poisson solver | **P2** |
| N9 | A Flats layer expands to real layers on demand: per colour, per fill, or merged | **P1** |
| N10 | Ink extraction and all region-colour picking run in the **display domain**, not linear | **P1** |
| N11 | Label fields compress at rest; transient segmentation buffers are released after the flat | **P1** |
| N12 | A flat can be washed by stamping it into a Media layer and running the existing simulation — there is no second watercolour implementation | **P2** |

### O. History and recovery

| ID | Requirement | P |
|---|---|---|
| O1 | **Redo**, and history as a linear list with a cursor rather than a stack | **P0** |
| O2 | A History panel listing entries by the tool or op that produced them; clicking one moves the cursor there | **P1** |
| O3 | Jumping back N entries costs one replay from the nearest keyframe, not N replays | **P1** |
| O4 | **Snapshots**: explicit, named history entries exempt from A9's byte-bounded eviction | **P1** |
| O5 | **A journal of the document *model*** — layers, op stacks, masks, dabs, selections, paths — written on a timer and after every structural edit | **P0** |
| O6 | Dirty tiles flush on the same timer for the **active** document, not only on deactivate | **P0** |
| O7 | The journal uses the same serialiser as native save; there is no second writer to keep in step | **P0** |
| O8 | Unclean scratch directories are offered for recovery on launch, named and dated; never opened silently, never auto-deleted | **P0** |
| O9 | Autosave never writes over the user's own file | **P0** |
| O10 | A journal write never blocks the paint loop; one that would collide with an active stroke defers to the end of it | **P0** |

> **O5–O10 correct a false claim**, not a missing feature. The design asserted that crash
> recovery fell out of the `mmap` tile spill for free. It does not: the spill only fires on
> *deactivate*, it stores pixels with no structure, and a document whose value is an op
> stack and a Strokes layer has almost nothing in it — so the more non-destructively a user
> works, the less recovery they got. See
> [ADR-0008](docs/adr/0008-recovery-needs-a-model-journal-not-tile-spill.md).

### P. Automation

| ID | Requirement | P |
|---|---|---|
| P1 | An op stack can be **saved as a named action** and applied to another document | **P1** |
| P2 | Batch: run an action over a folder or a selected set of files, with an output rule | **P1** |
| P3 | Batch output supports format, colour space, bit depth and resize, reusing I15's presets | **P1** |
| P4 | A batch run reports per-file success or failure and never partially overwrites an input | **P0** |
| P5 | Actions are files, human-readable, and diffable | **P2** |
| P6 | Recording an action is optional — any document's existing op stack is already one | **P1** |

> **P1/P6 are why this is cheap.** An op stack is *already* a serialisable, ordered list of
> parameterised operations, so an action is a file format rather than a subsystem. It is
> also **more robust than Photoshop's Actions**, which record UI events and break when a
> dialog changes. For texture preparation — the primary user's stated job — this is the
> highest-leverage feature in the document relative to its cost.

### Q. View and navigation

| ID | Requirement | P |
|---|---|---|
| Q1 | Fit to window, 100%, zoom to selection, scrubby zoom, and zoom that keeps the cursor anchored | **P0** |
| Q2 | **Mirror view left/right and up/down**, independently toggleable and combinable — view-only, never touching the document | **P1** |
| Q3 | **Grayscale preview toggle** on a hotkey, for judging values | **P1** |
| Q4 | Rotate view by arbitrary angle, view-only, with a reset | **P1** |
| Q5 | Rulers, guides, drag-to-create, guide at a numeric or percentage position | **P1** |
| Q6 | Snapping to guides, grid, canvas edges and layer bounds, with a global toggle | **P1** |
| Q7 | Grid with configurable spacing and subdivisions | **P1** |
| Q8 | Navigator panel with the viewport rectangle draggable | **P2** |
| Q9 | Pixel grid at high zoom | **P2** |
| Q10 | Eyedropper picks into the foreground colour, with sample size, sample-all-layers, and ⌥-click while painting | **P0** |
| Q11 | Channels panel: view and edit a single channel, with alpha channels listed alongside | **P1** |
| Q12 | Notes — positioned text annotations that persist in the document and never render | **P2** |

> **Q2 and Q3 are painter's tools, not view chrome.** Mirroring the canvas to catch drawing
> errors and dropping to grayscale to check values are both things visual-development
> painters do constantly. Q3 is what became of soft proofing: the useful half of "preview
> as something else" for this audience is a value check, not a print simulation.
>
> Both mirror axes are independent toggles, so enabling both gives a 180° view — a
> composition check in its own right, and free rather than a third feature. Each is a sign
> flip in the view matrix, so the cost of the pair is the same as the cost of one.

### R. Input and shortcuts

Full keymap in [docs/shortcuts.md](docs/shortcuts.md).

| ID | Requirement | P |
|---|---|---|
| R1 | Every tool has an **unmodified single-key** shortcut; commands take modifiers | **P0** |
| R2 | Where Photoshop assigns a key, the default keymap matches it exactly; deviations are documented and few | **P0** |
| R3 | `⇧`+letter cycles within a tool group | **P1** |
| R4 | Tools that only apply to one layer kind are **scoped** to it rather than claiming a global key, and the palette shows the scoped set when it is active | **P1** |
| R5 | Brush size and hardness are adjustable **by an on-canvas gesture**, not only by `[` and `]` | **P0** |
| R6 | Every frequently used painting shortcut is reachable by the off-hand alone, without leaving the keyboard's left half | **P0** |
| R7 | The keymap is a data file, editable in-app and by hand, with a named default preset to return to | **P1** |
| R8 | Binding conflicts are **detected and reported**, including conflicts that occur only within one layer-kind scope | **P1** |
| R9 | An in-app shortcut list, searchable by action and by key | **P1** |
| R10 | `⌘H` hides the application, as on every other macOS application | **P0** |

> **R5 and R6 are ergonomics requirements, not preferences.** `[` and `]` are the two
> most-used keys in painting and are **unreachable while holding a pen in the right hand**.
> Photoshop has this defect and never fixed it. A `⌃⌥`-drag gesture on the canvas is
> therefore the *primary* path for size and hardness, with the bracket keys as the
> alternate.

> **R2 has already paid for itself.** Writing the keymap down surfaced that all seven of
> autoFlats' letter bindings collide with a Photoshop meaning — `X`, `R`, `C`, `D`, `F`, `S`
> and `Tab`. Transliterating that keymap along with the algorithm would have imported every
> one of them. See [docs/shortcuts.md §5](docs/shortcuts.md).

---

## 7. Acceptance criteria

Assertions in `--selftest` unless noted. A phase does not ship until its criteria pass.

| criterion | target | source |
|---|---|---|
| idle resident memory | < 40 MB | A1 |
| GPU field textures when idle | 0 | A1 |
| GPU textures held by hidden documents | 0 | A6 |
| cold start to first frame | < 100 ms | A2 |
| watercolour Media layer, 1024² window | ~193 MB, not 294 | A4 |
| 4K RGB document, 1:1 view | < 150 MB | A7 |
| same document backgrounded behind another | < 20 MB attributable | A7 |
| colour round trip decode → encode | within tolerance | B3 |
| pigment mass after brush lift (`--diag`) | flat | F1, H7 |
| `--diag` across two identical runs | identical | H7 |
| latent mixing: blue crossing yellow | green, not grey | F6 |
| pen-to-photon latency | < 20 ms, measured | F3 |
| stroke preview vs final composite | pixel-identical | G2 |
| undo N strokes then redo N | pixel-identical to before | O1 |
| `kill -9` mid-session, then recover | layer structure and op stacks intact, not just pixels | O5–O8 |
| save with either mirror axis or rotate-view active | file is unmirrored and unrotated | Q2, Q4 |
| erase on a Pigment layer | Mass falls, Latent unchanged | F10 |
| batch run failing on file 12 of 40 | files 13–40 untouched, failure reported | P4 |
| clipboard holding a 4K copy | no flattened buffer resident; cost visible in the status bar | M5 |

---

## 8. Milestones

Nineteen phases. Each leaves the application working. Detail in
[PLAN.md](PLAN.md); mechanism in [DESIGN-imaging.md §8](DESIGN-imaging.md).

| # | milestone | ships | gate |
|---|---|---|---|
| 1 | Simulation obeys the new rules | lazy per-mode fields, fixed timestep, arc-length dabs | A1–A4, F1, H7; Known Bug #2 closed |
| 2 | See a file | document, layer, tile store, linear decode, viewport, blank documents, view and navigation, the keymap | B1, B2, C2, C7, C16, I14, Q1–Q10, R1–R4, R7–R10 |
| 3 | Grade it | shaper + LUT, the whole colour set, histogram, auto-tone, op-stack UI | B3, B5, D1–D3, D10, D19 |
| 4 | Write it out | **native `.npaint` save/load via OIIO**, export with space/depth/resize, **the recovery journal** | B6, I1–I8, I10–I15, I18, O5–O10 |
| 5 | Stack it | layers UI, blend modes, `Mix`, masks, clipping, adjustment layers, merges, multi-select, **comps and comp export**, history and redo | C1, C3–C6, C9–C16, I16, I17, O1–O4 |
| 6 | Filter and transform it | ROI evaluator, tile cache, the filter set, free transform, resample, straighten, perspective and lattice warp, gradient and bucket | D4, D5, D9, D11, D12, D14–D18, D20, D21, D23–D26 |
| 7 | Select and paste | mask store, five tools, feather, booleans, quick mask, channels, marching ants, **the clipboard** | E1–E13, M1–M9 |
| 8 | Repair it | Strokes layer, clone, heal, diffusion inpaint | D6, D7 |
| 9 | Tile it | gradient removal, seam heal, 3×3 preview, PatchMatch | D8 |
| 10 | Paint on it | brush engine, modulation matrix, stroke buffer, pigment deposit, **the eraser**, dodge/burn, on-canvas size gesture | F1–F11, G1–G4, D13, R5, R6 |
| 11 | Media layers | solver window, bake-on-dry, keyframe replay undo | H1–H7 |
| 12 | Import brushes | `.abr`, then `.brush` | G6–G9 |
| 13 | Paths | Bézier editing, path → selection, stroke path with brush | J1–J5 |
| 14 | Text | Text layer, CoreText shaping, point and paragraph text | K1–K4 |
| 15 | PSD export | Flattened, then simply-layered. Native save already shipped in phase 4 | I9 |
| 16 | Flat it | ported invariant tests, headless flatting library, Flats layer, Fills panel | N1, N2, N4, N5, N6, N10, N11 |
| 17 | Fix it | gap suggestion, completion field, edit recording and replay, the six repair tools | N3, N7 |
| 18 | Sheet it | rubber-sheet segmentation on the existing Poisson solver, sag/zebra/ridge views, expand to layers, wash | N8, N9, N12 |
| 19 | Automate it | actions from op stacks, batch runner over folders | P1–P6, D22, D27 |

> **Phase 19's dependencies are satisfied at phase 6**, so it can move earlier at any
> point. For texture preparation it is arguably the highest-value phase in the second half
> of this list, and it is placed last only because nothing else depends on it.

The interface requirements (L) are not a phase — they land inside phases 2–11 as the
chrome they belong to is built. See [docs/ui.md](docs/ui.md).

**Phase 9 is the milestone that matters most.** At its completion the primary user's
daily workflow is complete and this replaces Photoshop for that work.

There are three independent chains after phase 7: **image** (6–9), **painting** (10–12)
and **flatting** (16–18). Their order is a renumbering, nothing more — worth considering
if motivation on a solo project favours building a differentiator sooner. Flatting is
placed last because it is the one chain the primary user's stated jobs do not require;
see the scope note in
[docs/autoflats-migration.md §8](docs/autoflats-migration.md).

---

## 9. Risks

| risk | severity | mitigation |
|---|---|---|
| **Latency target unreachable on this stack.** wgpu present modes and frame pacing may fight a sub-20 ms budget. F3 is a make-or-break requirement for painters, and no amount of feature work compensates. | **High** | Measure end-to-end latency in phase 1, before the brush engine exists and before it is expensive to change approach. |
| **Dab-replay undo too slow.** Replaying seconds of solve per undo press. | High | Keyframe interval is the tuning knob. Fallback is ADR-0005's rejected option — per-kind undo, Media rewinding to its last bake. |
| **Per-mode field splitting is harder than it looks.** Bind groups are layout-cached; conditional field sets may ripple further than expected. | Medium | Phase 1's whole purpose. Discovering this early is the point of sequencing it first. |
| **`.abr` key semantics are undocumented.** The container is specified; brush key meanings are not. | Medium | Krita and GIMP have working readers to reference. G9 makes partial import an acceptable outcome. |
| **OIIO build integration.** Breaks the project's no-package-manager convention. | Medium | Tiered behind `NP_USE_OIIO`; the core builds and runs without it. |
| **Scope.** Nineteen phases is a great deal for one person, and 76 requirements are marked P0. | **High** | Phases 4 and 9 are both genuinely usable stopping points. Four of the later chains are severable: paths (13), text (14), flatting (16–18) and automation (19) can each be dropped without touching what precedes them. **Reviewed for YAGNI on 2026-08-17 and the scope was affirmed deliberately** — the severability above is the mitigation, not trimming. Two things that review surfaced are worth re-reading before phase 10: the specification currently exceeds the codebase in size (5.1k lines vs 4.6k), and everything downstream of phase 1.1's latency measurement is provisional until that number exists. |
| **The autoFlats port is larger than its `core/` directory suggests.** ~800 lines of the actual algorithm — edit replay, graph colouring, group assignment, region anchors — live inside its 2,294-line UI file. Estimating from `core/` alone understates the port by ~40%. | Medium | Phase 16 step 1 is extracting a *headless* flatting library from both places, with autoFlats' 478 lines of invariant tests ported **first** as the specification. |
| **Flatting determinism is easy to lose in translation.** Region ids reshuffle on any parameter change; colours must derive from a point inside the region or the whole drawing repaints whenever a slider moves. | Medium | N4 is a regression test over a parameter sweep, not a comment. |
| **Mixbox is CC BY-NC**, and the encumbrance travels with saved documents containing latents, not just the binary. A reimplementation is a *third* pigment basis, not a drop-in — its latents will not match. | Medium | Decided in ADR-0006: ship on Mixbox now, non-commercial only; reimplement from the paper before distribution, with a commercial licence as the fallback if the spectral fit proves hard. Every file stamps its basis and embeds a baked RGB composite, so pre-swap documents stay openable forever. `NP_USE_MIXBOX=OFF` is verified working in phase 1 rather than assumed. |

---

## 10. Success criteria

1. **The author stops opening Photoshop for texture work.** The single clearest
   signal, and testable at phase 9.
2. **Idle memory stays under 40 MB** across every subsequent phase — the invariant
   holds as features accumulate, which is the whole claim of principle 1.
3. **A concept painter completes a full painting** without hitting a missing
   capability. Testable at phase 10.
4. **Blue over yellow gives green**, at interactive speed, on a layer a user created
   without configuring anything.

---

## 11. Open questions

Two terminology collisions remain unresolved in [CONTEXT.md](CONTEXT.md), where the
simulation code and the new code use one word for two things: **composite** (layer
compositing vs `composite.wgsl`'s latent→RGB conversion) and **tile** (a storage tile vs
a tileable texture). **flow** is now resolved — it means brush flow only; the fluid field
is *velocity*. Neither remaining collision blocks phase 1.

Unresolved product questions:

- Should imported brush presets be re-exportable? Currently a non-goal on licensing
  grounds, but it affects whether presets can be shared between machines.
- Is `Tool` one enum, or a registry? The painting and imaging tools have little in
  common.
- Should the recovery journal interval (ADR-0008, default 60 s) be user-visible? It trades
  worst-case data loss against disk churn, and the right answer probably depends on
  document size rather than preference.

---

## 12. Future work

Wanted, deferred — distinct from §5, which is *not doing*. These have no phase yet and
should get one when the chain they belong to is complete.

| deferred | depends on | note |
|---|---|---|
| **Puppet warp** | D23's lattice warp | The mesh machinery is shared; puppet warp is a different control scheme over it, so doing lattice first is not wasted work. |
| **Vector masks** | J (paths) | A path used as a resolution-independent mask. Cheap once paths and masks both exist; the reason to defer is that raster masks cover the common case. |
| **Vanishing point** | D21, D23 | Perspective-aware cloning and pasting. Genuinely useful for architectural texture work, and a large enough tool to deserve its own phase. |
| Selection → path tracing | J | Contour extraction plus curve fitting. Listed in §5 today; it belongs here instead — wanted, not refused. |
| Non-linear history / history brush | ADR-0005 | Declined there on byte-budget grounds. If snapshots prove insufficient in practice, this is the escalation. |
