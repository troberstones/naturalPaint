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

**Refreshed 2026-09-10 against `16ead20`**, 154 commits after `3002d73` —
PSD export, the phase 8 + 9 wave, Phase 19's automation and the pigment
smudge. **Five things this file listed as absent had been built**, and one
was a whole phase: §3's Phase 19 row said "no code" over the recorder, the
action-file format and the batch runner. The other four were the palette's
keyboard layer, Text selection ranges, the PATHS panel with its three
consumers, and a Mixbox-off fallback `PLAN.md` still marks ❌. §3 also had
**no rows at all for Phases 1–7, 10 and 12** — the omission the 2026-09-09
pass found for Phase 8, repeated for nine more phases — and those phases hold
the largest gap still open: no filter in this build is live (§3, Phase 6).
§6 lists every correction.

**Refreshed 2026-09-11 on `integrate/gap`**, after the gap-closing wave —
six tracks off `ca35341`, each re-checked against the source before this
edit rather than taken from its branch's report. **Six rows below closed**:
the last three unbuilt palette tools (Shape, Frame, Slice — §2 now has none),
the Gradient tool's stop editor and presets (PRD D24), the gnomon's scale and
rotate, C12's multi-layer transform, and all five `NotYetRegistered` menu
actions (Phase 19's tail, half 1). Slice was a PRD non-goal; the owner asked
for it, and `PRD.md` now carries it as **I19**.

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
| Bridge pen / eraser, draw-merge, lasso → group / shape as **canvas gestures** | the same grep now finds callers in `ui/MacPaintUI.cpp`: the flats-tool canvas route, and `flatsLassoCommit()` at both `selectPolygon()` commit sites | **built.** Reached as sticky tools from the FLATS TOOLS palette (`ControlsSection::FlatsTools`), which is what `docs/shortcuts.md` §1.1 meant by "the scoped set must be visible". **Keys: half bound.** `keymaps/default.json:78–83` binds the Flats-scoped `K` (delete fill), `U` (merge pair), `,` / `.` / `Return` (gap review) and `⌘⇧K`. Still unbound from `docs/shortcuts.md` §1.1's set: `Y` shape fill, `⇧K` group lasso, `⇧U` draw-merge, `⇧B` draw bridge, `⇧V` select edits, `⇧Enter` accept all. The palette is still the entry point for those. |
| `FLATS · N FILLS` sub-line, Fills panel | `layerRowSubLine()` still returns no fill count for Flats; no `FillsPanel` symbol | **partly answered elsewhere.** There is still no per-fill list and no sub-line count, but the SEGMENTATION panel now reads the count (`N FILLS · C COLOURS · G GROUPS`) through `flatsPeekEvaluation()`. A per-fill list remains unbuilt. **What the sub-line is actually blocked on** is named at `ui/MacPaintUI.cpp:2347`: `layerRowSubLine()` reads kind, blend and opacity off the `Layer` alone, and the count lives in an evaluation keyed on content hash × beneath signature that the panel would have to fetch. The absence is *pinned by an assertion* (`app/selftest/LayerPanel2a.cpp:294`). Its stated reason had rotted by 2026-09-09 and **has since been corrected**: it now says the fills exist and the row does not show them (§6). |
| Expand to layers (N9) | `expandFlatsLayer()` in `core/Merge.cpp`, `applyFlatsExpand()` in `app/LayerEditor`, the "Expand Flats to Layers" submenu on the LAYERS row | **built** — per fill / per colour / per group / merged, capped at `kFlatsExpandMaxLayers`, one undo step, asserted in `app/selftest/FlatsExpand.cpp`. |
| Reference layer for flatting | `Layer::flatsReference`, `flatsSourceLayers()` / `flatsSourceSignature()`, `LayerCommand::ToggleFlatsReference`, `np:flatsRef` | **built, and not in the original spec.** The default (every layer beneath) re-flatted whenever ANY layer below changed, because the cache key covered them all: measured at 228 ms of re-segmentation for five dabs on an unrelated colour rough, versus 0 ms once the line art was marked. Asserted in `app/selftest/FlatsSource.cpp` and `app/selftest/NpaintFormat.cpp`. |
| Bake source for `FILL: Flats` on an RGB layer | `FlatsBakeSource`, the SOURCE combo, golden view `bucket_options_flats` | **built, and it closed a defect.** The bake segmented the whole composite *including the layer it was writing to*, so a second fill read the first fill's own pixels as line art. `Below fill` is now the default. |
| PSD group export | `planPsdRecords()`, `writePsdLsctBlock()` in `io/PsdLayerExtras` | **writer built** (phase 15); no flats-side caller yet, so the *feature* is still not reachable from the Fills panel |
| GPU membrane on `jacobi.wgsl` (N8), GPU growth kernel, capped-resolution preview | no symbol | not built; the CPU port is the fixed answer they are held to |
| Phase 18 — wash via a Media layer (N12) | blocked on §3's Phase 11 | not built |

## 2. The tool palette

`enum class Tool` in `app/AppState.hpp` has **30** values (plus `Count`) since
Path Select landed on 2026-09-09 (`711ddf5`, the manipulator split off the
Pen). **As of 2026-09-11 all 30 carry real behaviour** — pinned by
`kImplementedTools[]` in `app/selftest/AtelierChrome.cpp` (30 entries) and by
the structural `toolImplemented(t) == toolHasCanvasHandler(t)` for every
`Tool`. With no unbuilt tool left, the ToolCursor and MenuBasics selftests
that used to demand "at least one unbuilt tool" now pin "every tool is
built" instead, so a gate predicate lost in a future merge fails by count
rather than greying a cell.

**Seven shipped on 2026-09-02** — Move, Measure, Pencil, Dodge, Burn, Clone
Stamp and Smudge. **Crop shipped after them, Pen and Curve on 2026-09-03**
with PLAN Phase 13's path model behind them (§3), and **Text on 2026-09-03**
(`1d6db71`) with Phase 14 behind it. **The last three shipped in the
gap-closing wave (2026-09-11)**, each on its own terms:

| Tool | Was blocked on | Built as |
|---|---|---|
| Shape | nothing structural | `app/ShapeTool` — a two-point gesture emitting a `VectorShape` (rect, ellipse, rounded, polygon, line) into a Vector layer, gated by its own `toolCreatesShapes()` |
| Frame, Slice | **no receiving model** | the model first: `core/Region` in `Document::regions` (so Undo covers it), saved as `np:regions` (`io/RegionSerial`), kept correct by crop / canvas size / image size / rotate in `ops/DocumentTransform`. Both tools share `app/RegionTool`; every commit is a recordable command (`app/CommandsRegions`). File > Export Frames and Slices (`io/ExportRegions`, PRD I19) and View > Show Frames and Slices. **Not built:** typing a region's x/y/w/h (the options row shows them read-only), and a golden view of either palette cell — both live in flyouts no view opens |

### Built tools with specced halves still missing

A tool being marked built means it has a canvas handler that does its job, not
that every option its engine can already reach has a control. Those gaps
belong here rather than in `docs/testing-issues.md`, because nothing about
them is *wrong* — they are designed capability with no UI, which is exactly
this file's subject.

| Tool | Missing | Blocked on | Evidence |
|---|---|---|---|
| Dodge / Burn | **The non-destructive form.** PRD D13 (P1, `PRD.md:191`) says "a brush painting into an adjustment layer's mask, never as a destructive pixel op", and `PLAN.md`'s Phase 10 repeats it. What shipped is a brush that rewrites RGB texels. | **a decision, not code** — either build the mask form or amend D13; nothing records the choice either way | `brush/TonalBrush.hpp:459` — `toneDab(TileStore& store, …)` writes the layer's own tiles. Not in `PLAN.md`'s Deviations table, and `TonalBrush.hpp` does not cite D13. |

**The Text row closed on 2026-09-09** (`9a6fff4`): `app/TextTool.hpp` §3b
holds a caret and an anchor, and the selection is derived from the two, so
shift-extend and every edit that respects a range work. Styled runs stay a
non-goal (PRD.md:102). **The Dodge / Burn row is new and is a different kind
of entry** — a built tool that contradicts its requirement rather than one
missing a half.

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

### The palette's keyboard layer — built the same day it was found

**Found 2026-09-09 and built 2026-09-09** (`1c092f3`, "Twenty-one tooltips
promised a letter; now the letters work"). The finding was that
`keymaps/default.json` held no tool entry at all and nothing in `main.cpp`
could switch a tool from a key, so every letter `kToolMeta` printed in a
tooltip was a **label** that looked exactly like a binding — which is how a
brief came to cite `docs/shortcuts.md:34` ("`J` | Heal") as one to honour.

Now, checked at `16ead20`:

* `keymaps/default.json` carries **23** `tool_*` bindings, one per reserved
  letter and its `⇧` sibling. They resolve through **one** dispatch arm,
  `toolFromSelectAction()` (`ui/AtelierChrome.cpp:355`), which strips the
  `tool_` prefix and looks the slug up in `kToolMeta`, ending at
  `app/ToolSwitch`'s single writer of `st.brush.tool` — the same writer the
  Tools menu and the palette cell use.
* `app/selftest/ToolHotkeys.cpp` walks **both** directions: every binding
  names a real tool, and every tooltip letter has a binding behind it.
* **The text-caret question is answered at the keymap gate**:
  `keyChordReachesKeymap()` refuses a bare key while a canvas Text session is
  live **or** `io.WantTextInput` is set (`main.cpp:5193`). The second half
  closed a latent defect the change found — typing an `f` into the layer
  rename box flipped the mirror.

**What is still unbound is the Flats-scoped set** — six of `docs/shortcuts.md`
§1.1's keys (§1's first row names them). The load-time conflict detector
already scopes by layer kind, so this is data rather than a new dispatch
point.

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
* **A route added for an EXISTING tool touches three of those five and two
  others**, measured on `StrokeRoute::StrokesRecord` (2026-09-10), which gave
  Clone Stamp and Heal a second destination without adding a `Tool`. The three
  are the enum, `strokeRouteFor()` and `strokeRouteWritesLayer()`;
  `kToolMeta`/`kImplementedTools[]` are untouched because no tool changed. The
  two others are `strokeRouteName()` (`-Wswitch`-enforced, so free to find) and
  `grainReachesRoute()` — which is **not** enforced by anything, delegates to
  `strokeRouteWritesLayer()` by default, and is therefore the one that silently
  lights a whole control group over a route that ignores every control in it.
  Its own comment says so; a route added without answering its question leaves
  it correct and its call site wrong.
* **Adding a `Tool` VALUE costs four more on top of those five**, measured on
  `Tool::Heal` (PRD D6, Phase 8) — the only value added since the palette was
  drawn. They are: `enum class Tool` itself, where declaration order is
  load-bearing because `kToolMeta` is positional; `kToolGroups`, or the tool is
  unreachable from the palette; `cursorForTool()` and
  `cursorHotspotAnchorFor()` in `ui/ToolCursor.cpp`; and
  `springEyedropperEligible()` in `app/ToolSwitch.cpp`. The last three are
  `-Wswitch`-enforced and therefore free to find; the first two are **tables**,
  and a table takes a row without complaining. The pinned set in
  `app/selftest/Smudge.cpp` — `toolBeginsStroke()` accepts exactly these tools —
  is the tripwire that makes a new stroke tool a decision rather than an
  accident, and it fired as designed.

## 3. PLAN phases, and what each still lacks

Checked by looking for the implementation files each phase would have to
create, not by reading the phase text. Phase headings are `PLAN.md`'s
`## N — Name` lines; 16–18 live in `docs/autoflats-migration.md` §8.

**Until 2026-09-10 this table began at Phase 8**, and its title was "PLAN
phases with no code". Rows 1–7, 10 and 12 are new. Each lists only what is
left of its phase: all of them are mostly built, which is presumably why no
earlier pass looked, and Phase 6 turned out to hold the largest single gap
in this file.

| Phase | Status |
|---|---|
| 1 — Make the simulation obey the new rules (`PLAN.md:20`) | **built, with one stale ❌.** `PLAN.md:176`'s exit table still says `NP_USE_MIXBOX=OFF` is "deferred — no `km2` fallback exists". One does, since `6892102` (2026-08-27, architecture review P2-3): `paint/Palette.cpp:53`'s closed-form KM2 branch, with `core/Pigment` and `gfx/ShaderLoader` guarded alongside it. **Not re-verified by this pass** — nothing builds the OFF tree routinely, and an unbuilt `#else` is how that ❌ was earned in the first place. |
| 2 — See a file (`PLAN.md:186`) | **built but for one P0 verb.** Tiles, colour policy, place-as-layer, mip pyramid, the probe and eyedropper with sample size and source (`ui/AtelierChrome.cpp:843`), mirror, grayscale preview, view rotation, rulers, guides, grid, snap, scrubby zoom and the data-driven keymap are all in. **Zoom to selection** (PRD Q1, P0) is not: no symbol and no `MenuAction` — the View menu's zoom rows are Fit to Window, 100%, Zoom In and Zoom Out (`ui/MenuModel.hpp:224–227`). |
| 3 — Grade it (`PLAN.md:252`) | **built.** `color/Shaper`, `ops/PointOps`, `color/LutBake`, `core/OpStack`, `core/Histogram` and the op-stack UI. Nothing specified is outstanding; the live-*spatial*-op gap belongs to Phase 6. |
| 4 — Write it out (`PLAN.md:277`) | **built, with three named residues.** Camera raw (PRD I2) is refused by the capability query because this project's OpenImageIO is built without LibRaw on purpose (`io/Capabilities.cpp:77`) — a build decision, not a gap. Step 6 ("lazy OIIO init") was re-scoped by its own measurement to "`dlopen` OIIO on first use" and never done: `grep -rlw dlopen src` finds only `app/Memory.cpp` and a selftest. **The journal recovers no document-level op stack** — `app/Journal.hpp:95` says PRD O5's verify line is "half met", unblocked by `np:docOps`, which `io/NpaintFile.hpp:197` still defers. The other half of that old entry, "the default build has no crash recovery", is gone: `NP_USE_OIIO=OFF` is now a configure-time `FATAL_ERROR`. |
| 5 — Stack it (`PLAN.md:389`) | **built.** PRD C12's last verb, **transform as a set**, landed in the gap-closing wave: `TransformTarget::LayerSet` in `app/TransformSession` — one gizmo around the union of the members' bounds, one shared matrix, one atomic commit and one history entry. `core/LayerSetOps.hpp:34` records the refusal it replaced. |
| 6 — Filter and transform it (`PLAN.md:448`) | **transform built; filters built destructively; the phase's own architecture absent.** Built: the resample kernels the phase names, exact flips and 90° paths, crop / canvas size / image size, free and numeric transform, **straighten** and **perspective correction** (both in `app/CropTool` — four free corners through `transformFromQuad()`, `:416`), the gradient tool, the paint bucket, and the ten entries of the Filter menu (`ui/MenuModel.cpp:1132`). **Not built, heaviest first:** (1) **No filter is live.** The phase opens with ROI propagation and a tile cache keyed on the ops below; neither exists. `core/OpStack.hpp:41` says only `OpClass::PointA` has real ops — `SpatialB` is a tag nothing constructs outside a fixture — so every spatial op writes texels through `app/PixelOpBridge` and cannot be re-edited. That fails PRD D4 (P1, "live and re-editable") and D12's "spatial, not point ops" framing. (2) **Lattice warp of a selection** (D23, P1): no symbol. (3) **Dust & scratches** (D11) and **shadows/highlights** (D12): no symbol. (4) **Highpass and local contrast** have engines (`ops/Filters.hpp:294`, `:726`) and no caller outside `ops/` and the selftest; `ui/MenuModel.hpp` has no entry for either. (5) **Fill and stroke a selection or layer with colour, pattern or gradient** (D26, P1): no symbol — `define_pattern` / `fill_with_pattern` exist, but only in the recorder (Phase 19 row). (6) Lens blur and radial / spin / zoom blur (P2): §4. Puppet warp is deferred by the phase itself. |
| 7 — Select and paste (`PLAN.md:480`) | **selection and clipboard built; channels half-reachable; quick mask headless.** `core/Channels` implements all three of PRD E11–E13 in one file — named alpha channels persisted in `.npaint`, selection ↔ channel both ways, and quick mask. **Quick mask has no caller**: `paintQuickMask()` (`core/Channels.hpp:338`), `quickMaskFromSelection()` and `selectionFromQuickMask()` are referenced only from `core/Channels.cpp` and the selftest. **Save / load selection as channel run only from an action file**: they are registered as `save_selection_as_channel` and `load_channel_as_selection` (`app/CommandsOpStack.cpp:1011`, `:1013`), with no `MenuAction`, no builder in a header and no panel. There is no Channels panel (E13's single-channel view and edit). **PRD M9 (P1)**: paste into selection, paste as new document and ⌥-drag duplicate have no symbol; fill with colour is D26 in the Phase 6 row. |
| 8 — Repair it (`PLAN.md:505`) | **mostly built as of 2026-09-09**, and this table omitted the phase entirely until that morning. `LayerKind::Strokes` now holds dab records with a spatial index over their bounds, checkpoint tiles and the samples-only-from-below rule (`core/StrokesContent`, `brush/StrokesLayer`, `io/StrokesSerial`); it rasterises per PRD C11, round-trips as `np:dabs`, and erases by deleting the records a stroke covers (PRD F11). **Heal** is `Tool::Heal` over a gradient-domain solve (`ops/Poisson`, `brush/Heal`); **diffusion inpaint** is `ops/Inpaint` (Telea) through `app/PixelOpBridge`. Clone remains *aligned only* — `brush/CloneStamp.hpp:218` records that toggle as a deliberate omission, so it is not an absence. The **class-C form of clone and heal** landed on 2026-09-10: `StrokeRoute::StrokesRecord` (one route for both tools, `app/StrokeSession` §1d argues the call) appends a `DabRecord` to a Strokes layer instead of writing texels, and `brush/StrokesLayer` re-evaluates it against the composite beneath — so a repair tracks a regrade under it, which is PRD D6 and `docs/operations.md:341`'s class C. A recorded heal is `DabColorSource::BelowHealed`, a solve whose source is the below-composite at the offset and whose Dirichlet rim is the below-composite straight down (`brush/StrokesLayer` §1b). That closed the reachability hole this row used to describe from the other end: `Layer > New Strokes Layer` made a real layer that nothing in the build could put a mark in, and PRD F11's erase worked correctly on records nothing could make. **Not built:** PatchMatch (D7's textured half — a cached class-D op with a Recompute button and a deterministic seed, shared with Phase 9), and a recorded PAINT stroke (`DabColorSource::Ink`), which is deferred by name — `app/StrokeSession` §1d states the three questions it is blocked on. |
| 9 — Tile it (`PLAN.md:510`) | **partly built as of 2026-09-09.** PRD D8's first, second and fourth pieces are in: **lighting-gradient removal** (divide by a heavily blurred copy and re-centre the mean, `ops/Filters`), **offset with wrap** (an addressing change that does no filtering), and the **3×3 repeat preview** (`app/TilePreview`, a View-menu toggle drawing nine quads through `ui/CanvasQuad`, golden view `tile_preview`). **Not built:** **seam heal**, and **PatchMatch** as the cached class-D op with a Recompute button and a deterministic seed that both this phase and §3's Phase 8 row want. Lighting-gradient removal was the piece PRD.md:208 calls the precondition for the rest, so the workflow now has a front and a way to judge its result, and no repair for what it shows you. `app/FilterOps.hpp:334` names seam heal as absent rather than stubbed. All three built ops are recordable since the gap-closing wave (Phase 19 row). |
| 10 — Paint on it (`PLAN.md:517`) | **built, with one contradiction.** Brush and dynamics, deposit on RGB and Pigment, the eraser per layer kind (ADR-0007), pencil, dodge and burn, and smudge on RGB and — since `16ead20` — on Pigment (`brush/PigmentSmudge`, a mass-weighted mean of latents). **Dodge and burn contradict PRD D13**, which forbids a destructive pixel op; §2 has the row. The Media deposit waits on Phase 11. This pass checked the tools and routes only — not the latency path or the `⌃⌥`-drag size gesture. |
| 11 — Media layers (`PLAN.md:545`) | `LayerKind::Media` enum value only — 7 non-selftest mentions, all display name / glyph / colour / font-set. `app/LayerPanel.cpp:139` tells the user "Not built yet." Blocks autoFlats Phase 18 (§1) and Phase 10's Media deposit. Unchanged since 2026-09-02, and now the only whole phase in `PLAN.md` with no code. |
| 12 — Import brushes (`PLAN.md:552`) | **ABR half built; Procreate half not.** `io/Descriptor`, `io/AbrBrushes` (tips, dynamics, the `bVTy` mapping) and `--abr-report` cover PRD G7 and G9, and `io/GimpBrush` adds `.gbr`/`.gih`, which the plan never asked for. **Not built:** `.brush` / `.brushset` import (PRD G8, P2) — no `bplist00` reader, no NSKeyedArchiver resolution; `grep -rliE 'bplist|procreate' src` is empty. **Dual Brush** is read and counted (`io/AbrBrushes.cpp:835`) and never rendered, so 66 of 101 measured presets lose their second tip (§4). |
| 13 — Paths (`PLAN.md:559`) | **built.** Everything the 2026-09-09 row listed as unbuilt has landed: the **PATHS dock tab** (`app/ControlsLayout.cpp:63`, `app/PathsPanel`, `b2c048c` — eleven verbs that grey themselves, planned in `docs/path-editing-plan.md`), and the three PRD J consumers now have UI callers — `pathToSelection()`, `fillPathIntoLayer()` and `strokePathWithBrush()` at `ui/MacPaintUI.cpp:13700`, `:13731` and `:13769`. Path Select split off the Pen as its own tool (`711ddf5`). Earlier landings still stand: the MODE segment, the drawn gnomon read from `gnomonHandlePositions()`, placement (`pathEditBeginPen()`), and vector thumbnails in LAYERS. The gnomon's scale corners and rotate ring now scale and rotate (`pathEditUpdate()`'s `GnomonPart` switch in `app/PenTool.cpp`, gap-closing wave), so **nothing specified is outstanding** here. `docs/vector-editing.md` §5 defers soft selection, symmetry, object-space gnomon and lasso component selection by design, and SVG gradients and patterns are still refused by name (`io/SvgImport.cpp:535`). |
| 14 — Text | **built** (`1d6db71`). `text/Shaper` + `text/CoreTextShaper.mm` (+ a stub for the Linux build), `core/TextContent`, a live `LayerKind::Text`, `np:text` in `io/NpaintFile`, `app/TextTool`, the canvas gesture and overlay, and an options row with FONT / SIZE / B / I / ALIGN / COLOR. SVG `<text>` imports as a Text layer when the element reduces to one styled run under a translate-plus-uniform-scale, and as glyph outlines otherwise, the fallback named per element in the report. Selection ranges landed on 2026-09-09 (`9a6fff4`), so **nothing specified is outstanding**. Text on a path, vertical text and rich-text runs are non-goals (PRD.md:102). |
| 15 — PSD export (`PLAN.md:635`) | **built.** `io/PsdWrite` (byte primitives), `io/PsdExport` (`writeFlattenedPsd()` / `writeLayeredPsd()`), `io/PsdLayerSection` (records, channels, names), `io/PsdLayerExtras` (masks, `lsct` groups), `io/PsdBlendKeys` (one 26-row table read in both directions). PSD is writable and offerable from the export dialog at **8 bits only**, and now for one reason rather than two. The original blocker — a 16-bit layered PSD keeps its records in an `Lr16` block *the reader* had no case for — was closed on 2026-09-10 (`49d5328`, `findLayerInfoBlock16()` in `io/PsdImport.cpp`, checked against psd-tools on a hand-built file). **What still blocks 16-bit is the full-scale value**: `PLAN.md`'s 0–32768 is disputed by psd-tools, which divides by 65535, and no real 16-bit Photoshop file has been available to settle it (`io/PsdImport.hpp`, "Depth"). `io/Capabilities.cpp:111`'s refusal comment gave the old reason until this refresh (§6). Verified against psd-tools 1.19.0: stacking order, the inverted hidden flag, opacity, clipping, blend and both name forms all correct, and every observable pixel exact (the only differences are fully transparent texels, where the merged composite is deliberately matted on white). |
| 16, 17 — Flat it, Fix it | **built** (§1). |
| 18 — Sheet it, and wash it | sheet/gap/declutter controls are in the bucket's options row; the wash half is blocked on Phase 11. |
| 19 — Automate it (`PLAN.md:719`) | **built** — the 2026-09-09 row said "no code", and its probe (`grep -rniE automat src`) was the wrong one: the phase's own vocabulary is *command*, *action* and *record*. `app/Command` is one `applyCommand()` funnel over a table of `CommandSpec`s split across `app/Commands{Image,Layers,OpStack,Patterns}.cpp`; `app/Recorder` writes steps; `io/ActionFile` is the file format; `app/Action` replays on a copy and commits one edit; `app/ActionsPanel`; `app/Batch`, `--batch` and `app/BatchDialog`, with PRD P4 asserted on bytes; and step 5's lens correction (`ops/Lens`) and pattern define/fill (`ops/Pattern`). `docs/automation.md` is the contract for features added after it. **The tail, which fails silently:** (1) ~~Five menu actions were `NotYetRegistered`~~ — Numeric Transform, Delete Selection, Inpaint, Remove Lighting Gradient and Offset are all registered since the gap-closing wave, and `app/CommandCoverage.cpp` has no `NotYetRegistered` row left. (2) **Five commands run only from an action file** — the inverse gap: `lens_correct`, `define_pattern` and `fill_with_pattern` (`app/CommandsPatterns.cpp:172–190`), plus Phase 7's two channel commands, have no `MenuAction` and no builder, so no menu, panel or key reaches them. `app/CommandsPatterns` has no header at all. |

## 4. Small and ready

* **`docs/psd-import-gaps.md` §5 — `lyid` and `lclr`.** The only one of that
  document's five gaps still open; `grep -c '"lyid"\|"lclr"' src/io/PsdImport.cpp`
  is 0. That section's own recommendation is "do **not** import `lyid` unless a
  concrete need appears", so this is ready in the sense of small, not in the
  sense of wanted. §§1–4 are implemented and verified in place.
* **`docs/operations.md`**: polar↔rectangular remap is marked "future work"
  (`:191`); radial, spin, zoom and lens blur are P2 (`:132–134`). `src/ops/`
  has the isotropic `Blur` and a **linear** motion blur (`ops/Filters.hpp:987`,
  `32ab577`, 2026-08-28) — which the two previous editions missed by
  searching `ops/Blur` only. It is directional along a line, not around a
  centre, so it is not the radial/spin family's machinery. All of them are
  ops-shaped and would fit the existing filter bridge, which means they would
  land destructive like everything else there (§3, Phase 6). That document's P0/P1/P2 tables do not consistently
  carry a ✅ for what is built, so a row without one is not an absence-claim;
  check `src/ops/` per row.
* **`docs/blend-mode-gaps.md` is effectively closed.** Written 2026-08-31
  against 7 modes; `core/Blend.hpp` now has 27 values — the KM `Mix` plus 26 Photoshop modes, every one except
  **Dissolve**, which that document scopes out on purpose (§"Out of scope:
  Dissolve", and item 5 of its own list: "only if asked for specifically").
  Nothing there is ready-to-build; it is done.
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
* **Menu entries for the headless engines**: Highpass and Local Contrast
  (`ops/Filters.hpp:294`, `:726`), and Lens Correction, Define Pattern and
  Fill With Pattern (`app/CommandsPatterns.cpp`). The first two need a
  command and a dialog; the last three already have their commands and need
  only the `MenuAction` and a dialog on `ui/Dialog`. Nothing here is new
  behaviour, only reachability.

## 5. Specified and deliberately blocked

The **pattern picker** is blocked by design, not by effort:
`app/DabLibrary.hpp:416` ("Deliberately not a `PatternLibrary`") and
`brush/BrushModel.hpp:78` both record why — the picker that would need the
index does not exist, and building the index ahead of its only consumer is
the reachability defect this file is otherwise about. It is listed here so it
is not repeatedly rediscovered as "missing". (The 2026-09-02 edition cited
`brush/DabLibrary.hpp`; the file is under `app/`.)

**Define Pattern and Fill With Pattern landed on 2026-09-10 and do not
unblock it** — the same shared-name trap as the Gradient tool and Gradient
Map above. `ops/Pattern` (PRD D27) keeps a defined pattern as *session*
state, argued in its §1, and has nothing to do with a brush's texture
`PatternRef`, which is what the picker would choose.

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
  and HISTORY only; `ls src/io | grep -i psd` is no longer import-only (phase 15 landed); there is
  still no `--version`; `lyid`/`lclr` is still 0; `src/ops/` still has one
  non-directional `Blur`; and `app/PenTool.cpp`'s Manipulator arm (now `:810`)
  still applies `transformTranslate(dx, dy)` to every non-tangent drag.

**2026-09-10** (refreshed against `16ead20`, 154 commits after `3002d73`):

* **Five absence-claims were wrong, one of them a whole phase.** §3 said
  Phase 19 had "no code" over the recorder, the action format and the batch
  runner; the probe was `grep -rniE automat src`, and the phase never uses
  that word. §2 said the keyboard layer did not exist and that Text had no
  selection ranges; both landed on 2026-09-09, the keyboard layer on the day
  §2 reported it missing. §3's Phase 13 row said the PATHS tab did not exist
  and the three path consumers had no UI caller; both landed the same day
  (`b2c048c`). And `PLAN.md:176` still marks the Mixbox-off fallback ❌,
  two weeks after `6892102` built one. **The probe, not the reading, is
  what rots**: a grep chosen for the old code's vocabulary goes on returning
  nothing after the feature lands under different names. Re-run the check,
  and when it comes back empty, look for the feature under the words its own
  plan uses.
* **§3 skipped nine phases, the same way it skipped Phase 8.** Phases 1–7,
  10 and 12 had no rows. They are mostly built, which is why nobody looked — and
  Phase 6 holds this file's largest open item: no filter is live, and PRD D4
  says "live and re-editable". The 2026-09-09 lesson ("a phase missing from
  this table is not a claim") was recorded and not acted on. The table now
  has a row for every phase.
* **A built tool contradicts its requirement, and no document says so.**
  Dodge and Burn rewrite pixels; PRD D13 says "never as a destructive pixel
  op". The palette check (`toolImplemented() == toolHasCanvasHandler()`) can
  prove a tool does *something* and cannot prove it does the specified
  thing. New row in §2.
* **Reachability now fails in both directions.** The five `NotYetRegistered`
  menu actions are features with no recording; the five recorder-only
  commands are recordings with no feature a user can reach. A green
  `--selftest` sees neither (§3, Phase 19).
* **Four code comments gave false reasons, and are fixed in the same
  commit as this refresh** — two for refusals (`io/Capabilities.cpp:111`,
  PSD 16-bit blamed on a reader gap that is closed; `core/LayerSetOps.hpp:35`,
  a set transform blamed on there being no layer transform at all), and two
  for a cleanup that had already happened (`CMakeLists.txt:68`,
  `src/CMakeLists.txt:714`, "~230 `#if defined(NP_USE_OIIO)` sites remain";
  `5b28695` and `8f4aa1e` removed them, and the define is now inert). The
  first was a miss by the Lr16 fix itself, which corrected
  `io/PsdImport.hpp` and not the capability comment citing the same gap. A
  refusal whose stated reason is false is a small defect that survives
  every green suite, because the refusal itself is still correct.
* **Resolved since 2026-09-09:** `app/selftest/LayerPanel2a.cpp`'s FILLS
  assertion now gives the true reason (§1).
* **Also corrected:** §1's Flats keys are half bound, not unbound; the
  motion blur has existed since `32ab577` (2026-08-28), which the last two
  editions missed by searching `ops/Blur` alone; and the palette is 27 built
  of 30, not 26 of 29. **Two P0 gaps surfaced only because the new rows
  forced a look**: zoom to selection (Q1) and C12's multi-layer transform.

**2026-09-11** (on `integrate/gap`, after the gap-closing wave):

* **Six rows closed, each re-checked in the source rather than taken from a
  branch report**: Shape, Frame and Slice (§2 has no unbuilt tool left);
  the Gradient tool's stop editor and presets; the gnomon's scale and
  rotate; C12's multi-layer transform; and the five `NotYetRegistered`
  menu actions.
* **Two assertions demanded the gap this wave closed.** ToolCursor's and
  MenuBasics' guards required "at least one unbuilt tool" so that their
  disabled-tool checks could not pass vacuously. Building the last three
  made the guards fail for the best reason there is. Both now pin the
  opposite — every tool built — and state that the disabled-tool checks are
  vacuous until an enumerator is added ahead of its behaviour.
* **The region branch as its agents left it recorded nothing from the
  canvas.** It had registered five region commands, and then the drag, the
  options row's rename and its Delete button all called `core::RegionOps`
  directly — the failure `docs/automation.md` §7 says no assertion can see.
  Every one now goes through `applyCommand()`, and a selftest arms the
  recorder around the same functions the UI calls; a sabotage that bypassed
  the command left every history-count assertion green and turned only
  that one red. The first `--region-demo` capture also found the options
  row printing its ImGui id as a label and hiding its own Delete button.
