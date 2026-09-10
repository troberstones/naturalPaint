# What is specified but not built

**Written 2026-09-02 against `3b067ac`. Refreshed 2026-09-08 against `36f5509`**
— the merge of the adjustment-layer, Linux-build (carrying autoFlats) and SVG
branches. A survey of the specifications in this repository — `PRD.md`,
`PLAN.md`, `docs/ui.md`, `docs/operations.md`, `docs/autoflats-migration.md`,
`docs/psd-import-gaps.md`, `docs/blend-mode-gaps.md`, `docs/vector-editing.md`
— asking one question of each: *is there code behind this?*

**Re-verified 2026-09-09 against `3002d73`**, 34 commits later. Every
absence-claim below was re-run and every one still held. Both edits that pass
made are places where this file *understated* a gap rather than overstating
one — a phase missing from a table, and an absence pinned by an assertion
whose reason had rotted — which is the failure mode the rot warning below does
not cover, because nothing here was wrong to read (§6).

## Why this file exists, and what it is not

This is a **dispatch inventory**, not a ledger of defects. `docs/testing-issues.md`
holds things that are wrong; this holds things that were designed and never
built. The two are different kinds of work and mixing them makes both harder to
read.

**Every claim below was checked against the source, not against another
document.** That distinction earned its place: the entry that prompted this
survey — `docs/testing-issues.md`'s **T3** — reads as though the gradient tool
were nearly done, and a first pass over it concluded "closed". It was not. The
call site in `ui/MacPaintUI.cpp` (line 13174 at `3b067ac`) hard-coded
`geom.kind = GradientKind::Linear`, and the stop editor that looked like the
missing half was `drawGradientMapDialog()` — the Gradient **Map** adjustment, a
different feature that happens to share the `ops/Gradient` vocabulary. Two
features with overlapping names is exactly the shape that defeats a
documentation-only reading, which is why this file records file and line for
each absence rather than a citation to a plan.

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
thing**, and this file has now demonstrated it on itself: the 2026-09-02
edition opened with "autoFlats: nothing of it exists", and six days later
`src/flats/` is 4,816 lines that pass a bit-exact port suite (§1). **Re-verify
before dispatching from this list**; do not brief an agent off a line here
without first re-running the check it cites.

## 1. autoFlats — was the largest unbuilt item; now built, with a named tail

**Refreshed 2026-09-08.** The 2026-09-02 edition said `ls src/ops/ | grep -i
flat` was empty and `LayerKind::Flats` was an enum value nothing consumed.
Both were true then and are false now. Checked at `36f5509`:

* `src/flats/` holds Expand, Field, FlatsLayer, Gaps, Ink, Membrane, Model,
  Morphology, Sag, Segment, Tool and a FlatsSelfTest — 4,816 lines — and is
  included from `core/Merge.cpp`, `core/VectorRaster.cpp`, `core/Layer.hpp`,
  `app/AppState.hpp`, `io/FlatsSerial.hpp`, `ui/MacPaintUI.cpp` and the
  headless `tools/FlatsTestMain.cpp`.
* `LayerKind::Flats` has 29 non-selftest consumers. `LayerCommand::NewFlatsLayer`
  makes one; the paint bucket's `FILL: Flats` mode (`BucketFill::Flats`,
  `app/AppState.hpp:657`) is the entry point, per ADR-0009.

PLAN Phases 16 and 17 are built. What `docs/autoflats-migration.md` §0
(dated 2026-09-04) still lists as unbuilt, re-checked here:

| Piece | Checked | Status |
|---|---|---|
| Bridge pen / eraser, draw-merge, lasso → group / shape as **canvas gestures** | the same grep now finds callers in `ui/MacPaintUI.cpp`: the flats-tool canvas route, and `flatsLassoCommit()` at both `selectPolygon()` commit sites | **built.** Reached as sticky tools from the FLATS TOOLS palette (`ControlsSection::FlatsTools`), which is what `docs/shortcuts.md` §1.1 meant by "the scoped set must be visible". The keys for them are still unbound; the palette is the entry point. |
| `FLATS · N FILLS` sub-line, Fills panel | `layerRowSubLine()` still returns no fill count for Flats; no `FillsPanel` symbol | **partly answered elsewhere.** There is still no per-fill list and no sub-line count, but the SEGMENTATION panel now reads the count (`N FILLS · C COLOURS · G GROUPS`) through `flatsPeekEvaluation()`. A per-fill list remains unbuilt. **What the sub-line is actually blocked on** is named at `ui/MacPaintUI.cpp:2158`: `layerRowSubLine()` reads kind, blend and opacity off the `Layer` alone, and the count lives in an evaluation keyed on content hash × beneath signature that the panel would have to fetch. The absence is *pinned by an assertion* (`app/selftest/LayerPanel2a.cpp:286`) whose stated reason has since rotted — §6, 2026-09-09. |
| Expand to layers (N9) | `expandFlatsLayer()` in `core/Merge.cpp`, `applyFlatsExpand()` in `app/LayerEditor`, the "Expand Flats to Layers" submenu on the LAYERS row | **built** — per fill / per colour / per group / merged, capped at `kFlatsExpandMaxLayers`, one undo step, asserted in `app/selftest/FlatsExpand.cpp`. |
| Reference layer for flatting | `Layer::flatsReference`, `flatsSourceLayers()` / `flatsSourceSignature()`, `LayerCommand::ToggleFlatsReference`, `np:flatsRef` | **built, and not in the original spec.** The default (every layer beneath) re-flatted whenever ANY layer below changed, because the cache key covered them all: measured at 228 ms of re-segmentation for five dabs on an unrelated colour rough, versus 0 ms once the line art was marked. Asserted in `app/selftest/FlatsSource.cpp` and `app/selftest/NpaintFormat.cpp`. |
| Bake source for `FILL: Flats` on an RGB layer | `FlatsBakeSource`, the SOURCE combo, golden view `bucket_options_flats` | **built, and it closed a defect.** The bake segmented the whole composite *including the layer it was writing to*, so a second fill read the first fill's own pixels as line art. `Below fill` is now the default. |
| PSD group export | no symbol | not built |
| GPU membrane on `jacobi.wgsl` (N8), GPU growth kernel, capped-resolution preview | no symbol | not built; the CPU port is the fixed answer they are held to |
| Phase 18 — wash via a Media layer (N12) | blocked on §3's Phase 11 | not built |

## 2. The tool palette

`enum class Tool` in `app/AppState.hpp` has 28 values (plus `Count`).
**25 carry real behaviour and 3 are name/icon/slot only** — honestly greyed
out, and pinned there by the `--selftest` assertion in
`app/selftest/AtelierChrome.cpp:437` (`kImplementedTools[]`, 25 entries) plus
the stronger structural one, `toolImplemented(t) == toolHasCanvasHandler(t)`
for every `Tool`. That pair is why this half of the palette cannot quietly
claim to work: a tool cannot be marked built without a handler, and cannot
acquire a handler while still marked unbuilt.

**Seven shipped on 2026-09-02** — Move, Measure, Pencil, Dodge, Burn, Clone
Stamp and Smudge. **Crop shipped after them, Pen and Curve on 2026-09-03**
with PLAN Phase 13's path model behind them (§3), and **Text on 2026-09-03**
(`1d6db71`) with Phase 14 behind it. Pen and Curve could only select and drag
geometry that already existed until placement landed (`pathEditBeginPen()`,
`docs/vector-editing.md` §8) — an empty-canvas press now creates and extends
a shape rather than starting a marquee.

The remaining three are not three instances of one gap:

| Tool | Blocked on | Notes |
|---|---|---|
| Shape | nothing structural | `core/Path`, `core/PathRaster` and `core/VectorShape` are built, so this is a gesture that emits a `VectorShape` into the layer the Pen already edits. The cheapest tool left. |
| Frame, Slice | **no receiving model** | both name a document-level *region* concept that does not exist; the gesture without it draws a rectangle and forgets it |

### Built tools with specced halves still missing

A tool being marked built means it has a canvas handler that does its job, not
that every option its engine can already reach has a control. Those gaps
belong here rather than in `docs/testing-issues.md`, because nothing about
them is *wrong* — they are designed capability with no UI, which is exactly
this file's subject.

| Tool | Missing | Blocked on | Evidence |
|---|---|---|---|
| Gradient | **A stop editor.** The ramp is foreground-to-transparent, built by `gradientToolStops()`. | **PRD D25/D26** — `docs/ui.md` deliberately has no background half to the swatch, so "foreground to background" would name a colour that does not exist | `app/GradientTool.cpp:71`; the options-bar tooltip at `ui/AtelierChrome.cpp:990` says so to the user |
| Pen / Curve | **Scale and rotate.** The gnomon's four scale corners and rotate ring are drawn and hit-tested, and a press on any of them starts a `Manipulator` drag. | nothing structural | `app/PenTool.cpp:736` — every `Manipulator` drag that is not a `TangentDrag` applies `transformTranslate(dx, dy)`. Nothing reads which handle was pressed once the drag kind is set. |
| Text | **Selection ranges.** The caret is a single byte offset. | nothing structural | `app/TextTool.hpp:101`; no anchor/range member exists, so no shift-click, no double-click-a-word, no styled run |

The gradient's swatch, live preview and commit all read **one**
`gradientToolStops()` and **one** `gradientToolGeometry()`, so an editor
changes one function body rather than three call sites. Spread and the three
kinds are live combos and `--selftest` proves each setting reaches the pixels.

**The kinds row was in this table for about four hours** on 2026-09-02, which
is the shortest life any entry here has had and is worth recording as the good
case: the row named the blocker as "nothing", named the exact change, and
pointed at the function whose own comment described it. A row that specific is
a brief, and it got built off this table rather than off a re-survey. The Pen
row above is written to the same standard.

### The palette's keyboard layer does not exist, for any tool

Found 2026-09-09 while adding a tool whose brief cited `docs/shortcuts.md:34`
("`J` | Heal") as though it were a binding to honour. It is not a binding, and
neither is any other row of that table:

* `keymaps/default.json` contains **no tool entries at all** — `grep -icE
  '"tool|brush|eraser|marquee'` over it is 0.
* There is no letter-key tool switch under `src/ui/` or in `main.cpp`.

So `docs/shortcuts.md` §1 — twenty-odd unmodified single keys, each stated to
"match Photoshop", plus a whole `⇧`+key column — is **specified and entirely
unbuilt**, and every tool in the palette is equally unreachable from the
keyboard. `kToolMeta`'s per-tool letter is a **label**, shown in the tooltip;
it looks exactly like a binding at a glance, which is how a brief came to cite
one, and is the reason this section exists rather than a line in a tool's row.

This is one job for the whole palette, not a per-tool tail: a key table, a
dispatch point, and a decision about what a letter does while a text caret is
live (`docs/shortcuts.md` §6 is the place that argument belongs). It is
**not** small — the flats scoped set has the same problem from the other end
(§1's first row: "the keys for them are still unbound"), so the two want one
answer, not two.

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
create, not by reading the phase text. Phase headings are `PLAN.md`'s
`## N — Name` lines; 16–18 live in `docs/autoflats-migration.md` §8.

| Phase | Status |
|---|---|
| 8 — Repair it (`PLAN.md:505`) | **partly built — and this table omitted the phase entirely until 2026-09-09.** Clone is built (`brush/CloneStamp`, `app/StrokeSession`), *aligned only*; `brush/CloneStamp.hpp:218` records the aligned/non-aligned toggle as a deliberate omission, so that half is not an absence. The rest of the phase has no code: `LayerKind::Strokes` is a greyed row whose own reason is "the kind has no parameter member to hold them" (`app/LayerPanel.cpp:130`) — no dab records, no spatial index over dab bounds, no checkpoint tiles, no samples-only-from-below rule — and `grep -rniE 'inpaint\|patchmatch' src` finds exactly one comment (`app/TransformSession.hpp:93`), so neither the gradient-domain heal nor the Telea inpaint exists. PRD C1 lists Strokes as **P0**; `io/NpaintFile.hpp:165` already reserves the `strokes` part and its `np:dabs` blob for it. |
| 9 — Tile it (`PLAN.md:510`) | no code. `core/Tile*` is the tile *storage* model, not this; nothing offsets-by-half, seam-heals or previews a 3×3 repeat. |
| 11 — Media layers (`PLAN.md:545`) | `LayerKind::Media` enum value only — 7 non-selftest mentions, all display name / glyph / colour / font-set. `app/LayerPanel.cpp:125` tells the user "Not built yet." Blocks autoFlats Phase 18 (§1). |
| 13 — Paths (`PLAN.md:559`) | **mostly built.** `core/Path`, `core/PathFlatten`, `core/PathRaster`, `core/PathStroke`, `core/VectorShape`, `LayerKind::Vector`, `io/PathSerial`, `io/SvgImport` and `app/PenTool` all exist; Pen/Curve have a canvas gesture, an on-canvas overlay, a **MODE segment** (Shape / Component, `ui/AtelierChrome.cpp:1243`, so `docs/vector-editing.md` §8's "no caller under `ui/`" is stale), a **drawn gnomon** read from `gnomonHandlePositions()` (`ui/MacPaintUI.cpp:16478`) so the drawn geometry and the hit geometry cannot disagree, and now **placement**: an empty-canvas press creates or extends a shape, Curve auto-fits its tangents (`pathEditBeginPen()`, `docs/vector-editing.md` §8), which had been the biggest gap this row understated -- `PathDragKind::PenExtend` was a switch arm with no writer until it landed. Vector and Text layers draw their geometry into the LAYERS thumbnail (`app/LayerThumbnail`), rasterised at 24×24 with no cache. `app/PathConsumers` supplies the three PRD J consumers headless — path-to-selection, fill path, stroke path — **and they still have no UI caller**: `grep -rn PathConsumers src` finds only `CMakeLists.txt`, the selftest registration and the header. **Not built, by name:** the PATHS dock tab (`app/ControlsLayout.cpp` names COLOR, LAYERS, HISTORY and nothing else), the gestures that would invoke the three consumers, and the gnomon's scale/rotate (§2). `docs/vector-editing.md` §5 defers soft selection, symmetry, object-space gnomon and lasso component selection by design. |
| 14 — Text | **built** (`1d6db71`). `text/Shaper` + `text/CoreTextShaper.mm` (+ a stub for the Linux build), `core/TextContent`, a live `LayerKind::Text`, `np:text` in `io/NpaintFile`, `app/TextTool`, the canvas gesture and overlay, and an options row with FONT / SIZE / B / I / ALIGN / COLOR. SVG `<text>` imports as a Text layer when the element reduces to one styled run under a translate-plus-uniform-scale, and as glyph outlines otherwise, the fallback named per element in the report. **Not built:** selection ranges (§2). Text on a path, vertical text and rich-text runs are non-goals (PRD.md:102). |
| 15 — PSD export (`PLAN.md:635`) | no code — `ls src/io | grep -i psd` is `PsdImport.{cpp,hpp}` and nothing else; no `PsdExport` or `writePsd` symbol. **Import is done and oracle-verified** against psd-tools 1.18.0 across three real Photoshop files. autoFlats' "PSD group export" (§1) lands here when this does. |
| 16, 17 — Flat it, Fix it | **built** (§1). |
| 18 — Sheet it, and wash it | sheet/gap/declutter controls are in the bucket's options row; the wash half is blocked on Phase 11. |
| 19 — Automate it (`PLAN.md:706`) | no code. `grep -rniE automat src` hits only comments about GPU LOD selection. |

## 4. Small and ready

* **`docs/psd-import-gaps.md` §5 — `lyid` and `lclr`.** The only one of that
  document's five gaps still open; `grep -c '"lyid"\|"lclr"' src/io/PsdImport.cpp`
  is 0. That section's own recommendation is "do **not** import `lyid` unless a
  concrete need appears", so this is ready in the sense of small, not in the
  sense of wanted. §§1–4 are implemented and verified in place.
* **`docs/operations.md`**: polar↔rectangular remap is marked "future work"
  (`:191`); radial, spin and zoom blur are P2 (`:132–133`). `src/ops/` has one
  `Blur` and it is not directional. Both are ops-shaped and would fit the
  existing filter bridge. That document's P0/P1/P2 tables do not consistently
  carry a ✅ for what is built, so a row without one is not an absence-claim;
  check `src/ops/` per row.
* **`docs/blend-mode-gaps.md` is effectively closed.** Written 2026-08-31
  against 7 modes; `core/Blend.hpp` now has 27 values — the KM `Mix` plus 26 Photoshop modes, every one except
  **Dissolve**, which that document scopes out on purpose (§"Out of scope:
  Dissolve", and item 5 of its own list: "only if asked for specifically").
  Nothing there is ready-to-build; it is done.
* **The Shape tool** (§2) — one gesture over a model that already exists.
* **ABR brush-import "Phase D2, Phase E, Phase 8."** Carried over from the
  2026-09-02 edition. **The plan those names come from is not in this
  repository** — `grep -rnE 'Phase (D2|E)\b' docs PLAN.md PRD.md src` finds only
  this file, and the one `src/` mention (`main.cpp:1587`, a golden hook for
  the brush-settings window "Phase 8 of the ABR plan") is a comment. Treat
  these as un-dispatchable until someone finds or rewrites the plan; a brief
  cannot be written off a phase letter with no text behind it. Known engine
  gaps in that area that *are* verifiable: Dual Brush is detected and counted
  by `--abr-report` but not rendered (the engine has one tip per brush).
* There is still no `--version` flag (`grep '"--version"' src/main.cpp` is
  empty).

## 5. Specified and deliberately blocked

The **pattern picker** is blocked by design, not by effort:
`app/DabLibrary.hpp:416` ("Deliberately not a `PatternLibrary`") and
`brush/BrushModel.hpp:78` both record why — the picker that would need the
index does not exist, and building the index ahead of its only consumer is
the reachability defect this file is otherwise about. It is listed here so it
is not repeatedly rediscovered as "missing". (The 2026-09-02 edition cited
`brush/DabLibrary.hpp`; the file is under `app/`.)

## 6. What the surveys corrected

Recorded so the corrections do not have to be rediscovered:

**2026-09-02:**
* **T3 was open, not closed** — see the argument at the top of this file.
* **T18's two named causes are both fixed in the decode**, but the visual
  symptom was never re-checked against the user's file; see that entry.
* **T14 lost one of its three open bullets** to `f597459` — see that entry.

**2026-09-08:**
* **§1 was wholly stale.** "Nothing of autoFlats exists" was written six days
  before 4,816 lines of it merged. The file's own warning about rot applied to
  its headline entry.
* **§2 counted Text among the unbuilt** while §3 said it was built. Both were
  edited on 2026-09-03 and only one was updated. 25 built / 3 unbuilt is the
  number the selftest pins.
* **`docs/vector-editing.md` §8 is stale in two places** — the MODE segment
  and the drawn gnomon both landed in `58239cb`. Its third bullet, that scale
  and rotate reach nothing, is still true and is now a row in §2 here.
* **The ABR phase letters have no spec behind them in the tree.** They were
  carried forward from 2026-09-02 without a citation, and this refresh could
  not find one. Flagged in §4 rather than silently dropped.
* **`ui/MacPaintUI.cpp:2117`'s comment** ("a Flats layer has no fills") is a
  pre-port statement that survived the port. Cosmetic, but it is exactly the
  kind of line a future survey would read and believe. **Fixed since:** the
  comment (now at `:2158`) reads "A Flats layer HAS fills since the autoFlats
  port" and goes on to name why the count is off the row. Its copy in the
  selftest did not move with it — see 2026-09-09 below.

**2026-09-09** (re-verified against `3002d73`; nothing above was found stale):

* **§3's phase table skipped Phase 8 entirely**, 9 straight to 11, from the
  2026-09-02 edition onward — so a P0 layer kind (`Strokes`, PRD C1) and two
  whole repair algorithms were absent from the inventory for a week. The likely
  cause is that the phase reads as answered from the palette: Clone Stamp
  shipped on 2026-09-02 and is in §2's built list, and Phase 8's headline is
  "clone". Everything *else* in the phase went unlooked-at. The row exists now,
  and the reading to carry forward is that **a phase missing from this table is
  not a claim** — the table only ever held the phases someone thought to check.
* **The FILLS absence is pinned by an assertion whose reason has rotted.**
  `app/selftest/LayerPanel2a.cpp:286` asserts the row carries no fill count
  because "a Flats layer holds no regions in this build" — true when written,
  false since `src/flats/` merged. **The assertion is still correct about the
  UI**; only its justification is wrong, which is the more dangerous half: a
  green suite reads as a survey, and the next reader takes the reason for a
  finding rather than re-checking it. Its own source comment has since been
  corrected and the two have drifted apart. Fixing the assertion's wording is
  not the same work as building the sub-line, and only the second closes §1's
  row.
* **Re-run and unchanged:** `kImplementedTools[]` is still 25 entries with
  Shape / Frame / Slice outside it; `grep -rn PathConsumers src` still finds
  only CMake, the header, the selftest and `main.cpp`'s registration, so PRD
  J1/J2/J3/J4 remain UI-less; `ControlsLayout.cpp` still names COLOR, LAYERS
  and HISTORY only; `ls src/io | grep -i psd` is still import-only; there is
  still no `--version`; `lyid`/`lclr` is still 0; `src/ops/` still has one
  non-directional `Blur`; and `app/PenTool.cpp`'s Manipulator arm (now `:810`)
  still applies `transformTranslate(dx, dy)` to every non-tangent drag.
