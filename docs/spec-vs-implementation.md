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

**And checking against the source was still not enough. Amended 2026-09-02.**
T3 is now closed, and the reason it was open had nothing to do with either the
hard-coded kind or the missing stop editor — both of which were correctly
found by reading, and both of which were real. The tool drew nothing because
its drag state lived in a flag that *another tool's code* cleared every frame,
seventy lines earlier in the same function. Three separate readings of the
gradient's own block could not see it, because nothing in the gradient's own
block is wrong. It took instrumenting the running app.

The lesson this file should carry forward: **reading the source proves what
the code says, not what it does.** For anything whose failure mode is "the
gesture never happens", an absence-claim derived from reading is a hypothesis,
and the cheapest way to test it is one `fprintf` in the running build — not a
fourth read.

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

`docs/ui.md` §2 specifies 28 palette cells. **25 carry real behaviour and 3 are
name/icon/slot only** — honestly greyed out, and pinned there by the
`--selftest` assertion *"`toolImplemented()` is true for exactly the twenty-five
tools with real behaviour"* plus the stronger structural one,
`toolImplemented(t) == toolHasCanvasHandler(t)` for every `Tool`
(`ui/AtelierChrome.cpp`). That pair is why this half of the palette cannot
quietly claim to work: a tool cannot be marked built without a handler, and
cannot acquire a handler while still marked unbuilt.

**Seven shipped on 2026-09-02** — Move, Measure, Pencil, Dodge, Burn, Clone
Stamp and Smudge — built as six parallel tracks off `3b067ac` and merged
together. **Crop shipped after them, and Pen and Curve on 2026-09-03** with
PLAN Phase 13's path model behind them (§3).

The remaining four are not four instances of one gap:

| Tool | Blocked on | Notes |
|---|---|---|
| Shape | nothing structural | and no longer on geometry either: `core/Path`, `core/PathRaster` and `core/VectorShape` are built, so this is a gesture that emits a `VectorShape` into the layer the Pen already edits |
| Text | **PLAN Phase 14's back half** | the path model, rasteriser and `LayerKind::Vector` a glyph outline would land in all exist; the shaper (`src/text/`) and `LayerKind::Text`'s content do not |
| Frame, Slice | **no receiving model** | both name a document-level *region* concept that does not exist; the gesture without it draws a rectangle and forgets it |

### Built tools with specced halves still missing

A tool being marked built means it has a canvas handler that does its job, not
that every option its engine can already reach has a control. Those gaps
belong here rather than in `docs/testing-issues.md`, because nothing about
them is *wrong* — they are designed capability with no UI, which is exactly
this file's subject. Recorded 2026-09-02, when closing T3 would otherwise have
lost them.

| Tool | Missing | Blocked on | Evidence |
|---|---|---|---|
| Gradient | **A stop editor.** The ramp is foreground-to-transparent, built by `gradientToolStops()`. | **PRD D25/D26** — `docs/ui.md` deliberately has no background half to the swatch, so "foreground to background" would name a colour that does not exist | `app/GradientTool.hpp` § 5 |

Both are cheaper than they were before 2026-09-02: the swatch, the live
preview and the commit now read **one** `gradientToolStops()` and **one**
`gradientToolGeometry()` (`app/GradientTool.hpp` § 1), so an editor or a kind
picker changes one function body rather than three call sites that have to be
kept agreeing.

**What is no longer missing:** spread, and — since later the same day — the
kinds. Clamp / Repeat / Reflect and Linear / Radial / Angular are both live
combos in the options bar, and `--selftest` proves each setting reaches the
pixels rather than moving a field nothing reads, which is the reachability
defect this file's § 2 is otherwise about.

**The kinds row was in this table for about four hours**, which is the
shortest life any entry here has had and is worth recording as the good case:
the row named the blocker as "nothing", named the exact change, and pointed at
the function whose own comment described it. A row that specific is a brief,
and it got built off this table rather than off a re-survey.

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
| 13 — Paths | **partly built.** `core/Path`, `core/PathFlatten`, `core/PathRaster`, `core/PathStroke`, `core/VectorShape`, `LayerKind::Vector`, `io/PathSerial`, `io/SvgImport` and `app/PenTool` all exist, and Pen/Curve have a canvas gesture and an on-canvas overlay. **Not built, by name:** the options bar's Shape/Component mode segment (so Component mode is unreachable from the UI), the gnomon's drawn scale/rotate handles, the PATHS dock tab, and the three PRD J consumers — path-to-selection, fill path, stroke path with the brush. |
| 14 — Text | **built.** `text/Shaper` + `text/CoreTextShaper.mm` (+ a stub elsewhere), `core/TextContent`, a live `LayerKind::Text` layer, `np:text` in `io/NpaintFile`, `app/TextTool`, the canvas gesture and overlay, and an options row with FONT / SIZE / B / I / ALIGN / COLOR. `Tool::Text` is marked built and `LayerCommand::NewTextLayer` makes an empty one from the LAYERS panel. **Not built, by name:** selection ranges — the caret is a single byte offset, so there is no shift-click, no double-click-a-word and no styled run. The LAYERS thumbnail for a Text layer is blank, which is Vector's gap it inherits (`layerContentThumbnail()` gates on `layerHoldsPixels()`). Text on a path, vertical text and rich-text runs are non-goals (PRD.md:102). |
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
