# ADR-0009 — Flatting is a bucket mode on a Flats layer, not a modal tool

**Status:** accepted · **Date:** 2026-09-04

## Context

[docs/autoflats-migration.md](../autoflats-migration.md) settles *what* is absorbed from
autoFlats and *where it lands* (a seventh layer kind, evaluated like a Strokes layer). It
leaves the interaction open: autoFlats was a whole application with its own toolbar of
nine tools, and naturalPaint is a painting application whose palette already holds 27
tools nested into 17 flyout groups. Three shapes were on the table:

1. **A modal tool.** A "Flat" tool that, while active, replaces the canvas interaction
   with autoFlats' toolbar — bucket, barrier pen, eraser, merge, draw-merge, delete,
   shape, group, pick — and hands back when the user leaves it.
2. **A paint-bucket option.** The existing bucket grows a mode in which the region it
   fills is decided by the rubber-sheet segmentation (the sag field's basin under the
   click) instead of by colour tolerance. Nothing else changes.
3. **A layer kind that scopes the tools.** A Flats layer, with the existing tools taking
   on flatting meanings only while a Flats layer is active — the direction
   [docs/ui.md](../ui.md) §2 already records ("the palette switches to the flatting set
   when a Flats layer is active") and [docs/shortcuts.md](../shortcuts.md) §5.1 already
   plans keys for.

The question the user actually asked is which of 2 and 3 to build, and whether 1 is
needed to carry "the supporting tools of autoFlats".

## Decision

**Build 2 on top of 3, and not 1.** Concretely:

**The Flats layer is the model, and it is the only place a flat lives.** A Flats layer
holds parameters plus recorded repairs and evaluates against the line art beneath it
(`flats/Model.hpp` §1, PRD N1–N3). Every flatting gesture is an edit *to that layer*,
recorded as geometry and replayed, so the whole feature has one undo model (the
document snapshot the layer edit records), one save path (`np:flatParams` /
`np:flatEdits` / `np:fills` on the layer's part) and one re-evaluation rule.

**The paint bucket gains a `FILL` mode: `Colour` or `Flats`.** This is the user's
"paint bucket option", and it is the entry point:

| target layer | `Flats` mode bucket click does |
|---|---|
| a **Flats** layer | recolours the fill under the click with the foreground colour, recorded as a `FlatRecolor` at the fill's anchor; Option-click carves a new fill out of a leaked area (`FlatCarve`); Shift-click recolours every fill sharing that colour |
| an **RGB** layer | evaluates the sag segmentation of the composite beneath the click once, turns the basin under the click into a `Selection`, and fills it through `fillThroughSelection()` — the gap-tolerant bucket, baked, with no Flats layer involved |
| any other kind | refuses through `PixelOpRefusal`, exactly as the colour bucket does |

The RGB row is the cheap, no-commitment version of the feature — a bucket that does not
leak through a broken stroke — and it needs no new layer, no panel and no new tool. The
Flats row is the same gesture on the layer that remembers it. **One control, one gesture,
two depths of commitment**, and a user discovers the layer by wanting the fill to survive
a re-flat.

**"Flat all" is a command, not a tool.** The autoFlats behaviour of colouring every
region on load becomes a button on the Flats layer's options row (and a `Layer` menu
item): it records nothing, because the evaluation already colours every fill from its
anchor or the palette (PRD N4, N5). Region-by-region work is the bucket; all-at-once is
the command; both produce the same fills.

**The supporting tools are the existing tools, scoped by layer kind.** On a Flats layer:

| existing tool | flatting meaning | recorded as |
|---|---|---|
| Paint Bucket (`G`) | recolour / carve, above | `FlatRecolor`, `FlatCarve` |
| Pencil (`B` group) | **bridge pen** — an invisible barrier the fills stop at | `FlatBridgeStroke` |
| Eraser (`E`) | bridge eraser | `FlatBridgeStroke{erase}` |
| Lasso (`L`) | **group** the fills it covers — with `⇧K` from the shortcuts table | `FlatGroup` |
| Lasso + `Y` | **shape fill** — a fill drawn by hand | `FlatShapeFill` |
| Marquee + `⇧V` | select recorded edits to remove | — |
| `⇧U` | draw-merge: stroke from a fill, everything crossed merges into it | `FlatMergeStroke` |
| `M` (Flats-scoped) | two-click merge | `FlatMergePair` |
| `K` | delete the fill under the cursor | `FlatDeleteMark` |
| `,` / `.` / `Return` | cycle and accept gap suggestions | `FlatBridgeStroke` |
| `Layer ▸ Cluster small fills` | small open-bordered fills into their neighbours | `FlatMergePair` ×N |

The keymap machinery for this exists and is tested (`app/Keymap`'s `scope` field;
`resolve()` prefers a binding scoped to the active layer's kind). What was missing is one
line: `main.cpp` resolves every chord with `std::nullopt` as the scope. Feeding it the
active layer's kind is what turns the table above on, and it is the only global change
the flatting tools make to key handling — every autoFlats letter that collided with a
Photoshop meaning (§5.1 of the shortcuts doc) is resolved in Photoshop's favour.

## Why not a modal tool

A modal tool would have carried autoFlats' UI across intact, and that is the problem
with it. It puts a second toolbar inside the first, with its own bucket and its own
eraser that behave differently from the ones a pixel above them; it needs its own
undo (autoFlats' closure stack) or a bridge to the document's; and it makes the lasso
and marquee — which are selection tools here, built and tested — into something else
while the mode is on. The migration doc's §8 observation stands: *the lasso and marquee
edit tools are selection tools.* Scoping by layer kind gives every one of those gestures
its normal meaning on every other layer and its flatting meaning on the one layer where
that meaning exists, which is also how Photoshop's own type and shape tools behave.

## What the merge improves over autoFlats

Folded into the port rather than deferred, because each is a consequence of the
integration rather than new work:

- **The line art is editable.** A Flats layer re-evaluates when the layer beneath it
  changes (its raster cache is keyed on the content hash of both), so redrawing a stroke
  re-flats the drawing. autoFlats' line art was an immovable input.
- **Evaluation is a pure function of the document.** autoFlats carried colours, names
  and visibility forward from the *previous* flat by overlap (`matchColors`), so what a
  drawing showed depended on the order things were done. Here every one of those is a
  recorded edit at an anchor (`FlatRecolor`, `FlatFillNote`), so the same saved document
  always evaluates to the same fills, and `flatsContentHash()` can key a cache.
- **Carve is replayable.** autoFlats' bucket-carve mutated the label field and was a
  heavy snapshot undo; here it is a `FlatCarve` edit replayed before merges, so it
  survives a re-flat like everything else.
- **The bucket on an RGB layer** gives the gap-tolerant fill to a user who never opens
  the Layers panel.
- **The wash is the real simulation** (migration doc §4.2): a flat is stamped into a
  Media layer and the Curtis solver runs on it; autoFlats' 409-line reduced solver is
  not ported.
- **Bit-exact port, proven.** `src/flats/` reproduces autoFlats' label fields hash for
  hash on the shared fixtures (`app/selftest` flats section), which is what lets the
  GPU replacements the migration doc plans (§1.2 the membrane on `jacobi.wgsl`, §1.3 the
  growth kernel) be swapped in later against a fixed answer.

## Consequences

- `core/Layer` gains a `FlatsContent flats` member by value, on `TextContent`'s pattern;
  `core/VectorRaster`'s `layerRastersToTiles()` names `Flats`, and the materialise loop
  evaluates a Flats layer against the composite of the visible layers beneath it,
  encoded to display sRGB first (PRD N10, migration doc §5.1).
- The evaluation is CPU-only today and full-frame dense (migration doc §5.3): at 4K the
  transient buffers are ~160 MB and the rubber sheet is a multigrid solve of a few
  seconds. The Flats layer therefore evaluates on a content-hash cache, never per frame,
  and the sheet solve is the first thing to move to the GPU (PRD N8).
- Fills are rows in a Fills panel beside LAYERS; `FLATS · 153 FILLS` replaces the
  `FLATS · NORMAL · 100%` sub-line the layer row shows today.
- `.npaint` gains three attributes on a Flats part and the save refusal for the kind is
  lifted; `io/FlatsSerial` is the carrier, on `io/TextSerial`'s shape.

## Alternatives rejected

**A modal Flat tool.** Above.

**A bucket mode only, with no Flats layer.** Gives the gap-tolerant bucket and nothing
else: no re-flat, no repairs that survive one, no fills panel, no export by colour. It is
kept — as the RGB-layer row of the table — but as the shallow end of the same feature
rather than the whole of it.

**Region ids as the edit currency.** The obvious model and the wrong one; autoFlats
arrived at geometry-recorded edits after trying ids, and the reason (a re-flat renumbers
everything) is stated in its `state.ts`. Not re-litigated.
