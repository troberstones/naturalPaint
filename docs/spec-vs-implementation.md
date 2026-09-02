# What is specified but not built

**Written 2026-09-02, against `3b067ac`.** A survey of the specifications in
this repository — `PRD.md`, `PLAN.md`, `docs/ui.md`, `docs/operations.md`,
`docs/autoflats-migration.md`, `docs/psd-import-gaps.md` — asking one question
of each: *is there code behind this?*

## Why this file exists, and what it is not

This is a **dispatch inventory**, not a ledger of defects. `docs/testing-issues.md`
holds things that are wrong; this holds things that were designed and never
built. The two are different kinds of work and mixing them makes both harder to
read.

**Every claim below was checked against the source, not against another
document.** That distinction earned its place: the entry that prompted this
survey — `docs/testing-issues.md`'s **T3** — reads as though the gradient tool
were nearly done, and a first pass over it concluded "closed". It is not. The
call site at `ui/MacPaintUI.cpp:13174` still hard-codes
`geom.kind = GradientKind::Linear`, and the stop editor that looks like the
missing half is `drawGradientMapDialog()` at `ui/MacPaintUI.cpp:8596` — the
Gradient **Map** adjustment, a different feature that happens to share the
`ops/Gradient` vocabulary. Two features with overlapping names is exactly the
shape that defeats a documentation-only reading, which is why this file records
file and line for each absence rather than a citation to a plan.

The dating matters too. **An absence-claim rots the moment someone builds the
thing**, and this project has been bitten by that before — `docs/reachability-audit.md`
carries entries whose "there is no code for this" was true when written and is
not now. **Re-verify before dispatching from this list**; do not brief an
agent off a line here without first re-running the check in its `checked`
column.

## 1. The largest ready-to-build body of work: autoFlats

`docs/autoflats-migration.md` is 379 lines of complete plan covering PLAN
Phases 16–18. **Nothing of it exists.**

* `ls src/ops/ | grep -i flat` → empty. There is no `ops/Flats*` of any kind.
* `LayerKind::Flats` exists as an enum value with a display name, an icon and a
  mauve panel label, and is listed in `core/Layer.hpp`'s "inert" group
  alongside Media/Strokes/Text — it holds no pixels and nothing consumes it.

The plan is unusually complete for something with no code behind it, which
makes this the single largest piece of work in the repository that could be
dispatched today without a design pass first.

## 2. The tool palette

`docs/ui.md` §2 specifies 28 palette cells. **21 carry real behaviour and 7 are
name/icon/slot only** — honestly greyed out, and pinned there by the
`--selftest` assertion *"`toolImplemented()` is true for exactly the twenty-one
tools with real behaviour"* plus the stronger structural one,
`toolImplemented(t) == toolHasCanvasHandler(t)` for every `Tool`
(`ui/AtelierChrome.cpp`). That pair is why this half of the palette cannot
quietly claim to work: a tool cannot be marked built without a handler, and
cannot acquire a handler while still marked unbuilt.

**Seven shipped on 2026-09-02** — Move, Measure, Pencil, Dodge, Burn, Clone
Stamp and Smudge — built as six parallel tracks off `3b067ac` and merged
together.

The remaining seven are not seven instances of one gap:

| Tool | Blocked on | Notes |
|---|---|---|
| Crop, Shape | nothing structural | document geometry and raster primitives; the natural next wave |
| Pen, Curve | **PLAN Phase 13 (Paths)** | no path model exists: no bezier storage, no stroke-or-fill-from-path |
| Text | **PLAN Phase 14** | no font rasteriser, and `LayerKind::Text` is inert |
| Frame, Slice | **no receiving model** | both name a document-level *region* concept that does not exist; the gesture without it draws a rectangle and forgets it |

### What the tool wave established about the palette's own machinery

Worth recording, because it changes what the next wave costs:

* **`strokeRouteFor()` is the extension point for a brush-family tool**, and
  `toolBeginsStroke()` derives its answer by *probing* it rather than restating
  it — so a new route flips its own gate with no second list to keep in step.
* **`ui/MacPaintUI.cpp`'s canvas gate was the exception, and is no longer.** It
  read a hand-written `strokeTool = paintTool || eraseTool`, so a tool could
  have a route, a flipped flag, a passing `toolHasCanvasHandler()` and a fully
  green suite **and still be unusable** — the eyedropper's original reachability
  defect surviving in the last predicate spelled as literal `Tool` values. It
  now reads `toolBeginsStroke()` itself, and an assertion pins the set it
  accepts so a tenth stroke tool is a decision rather than an accident.
* **Adding a stroke route touches five shared registration points** — the
  `StrokeRoute` enum, `strokeRouteFor()`, `strokeRouteWritesLayer()`,
  `kToolMeta`, and `kImplementedTools[]` in the selftest. That is fine for one
  tool and expensive for six at once; a future wave should either pick tracks
  whose seams do not collide or widen those seams first.

## 3. PLAN phases with no code

Checked by looking for the implementation files each phase would have to
create, not by reading the phase text.

| Phase | Status |
|---|---|
| 9 — Tile it | no code |
| 11 — Media layers | `LayerKind::Media` enum value only |
| 13 — Paths | no code (`ls src/ops/ src/core/` finds no path/bezier file) |
| 14 — Text | no code |
| 15 — PSD export | no code — `src/io/` holds `PsdImport.{cpp,hpp}` and nothing else PSD-shaped. **Import is done and oracle-verified** against psd-tools 1.18.0 across three real Photoshop files; export is untouched. |
| 19 — Automate it | no code |

## 4. Small and ready

* **`docs/psd-import-gaps.md` §5 — `lyid` and `lclr`.** The only one of that
  document's five gaps still open; `grep '"lyid"\|"lclr"' src/io/PsdImport.cpp`
  finds neither. §§1–4 are all implemented and verified in place: masks decode,
  `lddg` → `BlendMode::Plus` (`PsdImport.cpp:258`), `colr` → `BlendMode::Color`
  (`:312`), `lsct` groups (`:774`), `lspf` (`:737`).
* **ABR Phase D2, Phase E, Phase 8** — brush-import work specified and not
  built.
* **`docs/operations.md`**: polar↔rectangular remap is marked "future work";
  radial and spin blur are P2. Both are ops-shaped and would fit the existing
  filter bridge.

## 5. Specified and deliberately blocked

The **pattern picker** is blocked by design, not by effort:
`brush/DabLibrary.hpp:416` and `brush/BrushModel.hpp:78` both record why. It is
listed here so it is not repeatedly rediscovered as "missing".

## 6. What this survey corrected

Recorded so the corrections do not have to be rediscovered:

* **T3 is open, not closed** — see the argument at the top of this file.
* **T18's two named causes are both fixed in the decode**, but the visual
  symptom was never re-checked against the user's file; see that entry.
* **T14 lost one of its three open bullets** to `f597459` — see that entry.
