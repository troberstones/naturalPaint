# Reachability audit

**What a user can actually reach from the UI, and what they cannot.**

Audited at `dbaf315` (2026-08-25), re-verified at `304bd21` after the four-track
gather. Every item carries a `file:line` and a stable id (`A1`, `M3`, …) so it
can be referenced from a plan, a commit message or a track brief without
restating it.

This is a **planning document**, not a design. Items say what is broken and what
a fix would touch; they deliberately do not say how to build it.

## Why this file exists

`--selftest` passing tells you a thing **works**. It does not tell you a user can
**reach** it. This project has repeatedly built, tested and proven engine code
that nothing in the UI ever calls — and, more recently, has shipped chrome that
declares a tool live while no handler consumes its click. Those are different
failures with the same symptom: **nothing happens, and nothing says why.**

### The categories, and why two of them are not defects

| Status | Meaning |
|---|---|
| **Silent no-op** | Enabled, looks live, does nothing, says nothing. The worst state. |
| **Partially wired** | Reaches an implementation but drops a value on the way. |
| **Unreachable** | Implemented and tested; no UI path leads to it. |
| **Honest refusal** | Disabled, greyed, or says "Not built yet." **This is a good state.** Not a defect and not tracked here. |
| **Not built** | The feature does not exist. Also not a defect — just work. |

Conflating an honest refusal with a silent no-op is how a tidy-up removes the
one thing protecting a user from confusion. The refusals this codebase already
makes — the 13 disabled palette cells, the layer-kind refusals in
`app/LayerPanel.cpp:92-101`, the brush/eraser/bucket/gradient refusals that name
the layer *and* the fix — are correct and should be left alone.

---

## Resolved since the audit

Kept here because "was this ever fixed?" is a question that recurs, and because
each was verified by sabotage rather than by reading.

| Id | Item | Resolved by |
|---|---|---|
| ~~R1~~ | Pigment deposit ignored the selection (PRD **E1**, P0) — natural media painted straight through the marching ants | `track6/pigsel`, verified: reverting the gate reddens 6 assertions |
| ~~R2~~ | No Pigment eraser, so PRD **F10** (P0) was half-met | `track6/pigsel` |
| ~~R3~~ | `File > Open` opened `.npaint` only; `openImageAsDocument()` had no caller outside `--selftest` | `track6/openany`, dispatch is on content not extension |
| ~~R4~~ | No `SDL_EVENT_DROP_FILE` handler anywhere (PRD **I14**, P0) | `track6/openany` |
| ~~R5~~ | No brush-library persistence of any kind | `track6/abrlib` — preferences file, lazy load, unload |
| ~~R6~~ | No native macOS menu bar (PRD **R10**) | `track6/nativemenu` — menu is now a 49-action data model with ImGui and AppKit backends |

**Partially resolved:** `BRUSH EDITOR > Save` still does not persist an edit to a
*built-in* preset. `track6/abrlib` disabled Save for library-owned presets
deliberately (this build writes no `.abr`, so the edit would be silently replaced
next launch) and added the preferences file, but a user-authored preset library
does not exist yet. Tracked below as **A7**.

---

## A. Silent no-ops

The defining trait: enabled, looks live, produces no visible change, no history
entry and no message.

### A1 — Eyedropper does nothing. PRD **Q10**, P0. *(in flight: `track7/eyedropper`)*

`kToolMeta` (`src/ui/AtelierChrome.cpp:152`) marks it `implemented = true`, so the
palette cell is clickable and its tooltip does not say "Not built yet."
`src/ui/ToolCursor.cpp:38` gives it a dedicated `ToolCursor::Sample` cursor —
`toolCursorOnTarget()` withholds `Refuse` *because* the tool claims to be
implemented. **`Tool::Eyedropper` outside `--selftest` exists only at
`AtelierChrome.hpp:184` (group table), `ToolCursor.cpp:38`, `MacPaintUI.cpp:377`
(icon art) and `StrokeSession.cpp:44,131` (routes to `None`).** No canvas handler.

The engine is complete and tested: `probePixel()` (`src/core/Probe.hpp`) implements
Q10's sample-size averaging and sample-all-layers, with 19 assertions in
`app/selftest/Probe.cpp`. `core/Probe.hpp` is included by two files — its own
`.cpp` and `app/selftest/Support.hpp`.

**Blocker named by the engine's own header:** there is nowhere to put a picked
colour. `BrushState::pigment` (`app/AppState.hpp:99`) is an `int` palette index,
not an RGB triple. Overlaps **A2**.

### A2 — COLOR panel's RGB mode is a live picker nothing reads. PRD **L4**, P0

`ImGui::ColorPicker3("##rgb", g_colorRgb, ...)` at `src/ui/MacPaintUI.cpp:2772`.
**`g_colorRgb` (declared `:2703`) is read nowhere in `src/`.**

Borderline: the panel prints "Not yet connected: no tool reads this colour," which
is honest about the outcome — but the widget is fully interactive rather than
disabled, and the stated *reason* ("the pen is not yet wired to a layer at all")
is stale, since the pen reaches layers via `StrokeSession` now.

Fixing **A1** properly requires solving this. They are one problem.

### A3 — Zoom tool does nothing. PRD **Q1**, P0 (scrubby zoom)

Identical shape to A1. `AtelierChrome.cpp:154` marks it implemented;
`ToolCursor.cpp:64` gives it `ToolCursor::Zoom`. Zoom works **only** by scroll
wheel (`MacPaintUI.cpp:6061`) and menu/keyboard — both tool-independent. Select
the tool, click the canvas, nothing.

### A4 — `Goodies` menu bypasses the disabled-tool guard entirely

`MacPaintUI.cpp:5320` loops all 27 `Tool` values and assigns `st.brush.tool = t`
**unconditionally** — no `toolImplemented()` check — and draws a checkmark on the
current one. The palette's `toolButton()` gates correctly
(`clicked = clickedRaw && implemented`, `AtelierChrome.cpp:456`).

Result is an incoherent state: the options bar prints the tool name, the palette
shows *no* cell selected, and the cursor becomes a slashed circle. Every one of
the 13 carefully-disabled tools is freely selectable from this menu.

### A5 — PIGMENT panel: Density, Staining, Granulation are overwritten every frame

`drawPigmentSection()` writes `st.sim.density/staining/granulation`
(`MacPaintUI.cpp:3388,3392,3396`), each with an explanatory tooltip.
`main.cpp:2381-2383` then overwrites all three **unconditionally**, after
`ImGui::Render()` and before the sim upload. The slider visibly snaps back next
frame.

The comment at `MacPaintUI.cpp:3351` claiming these are "a different set of numbers
with the same three names" is **false**; `main.cpp:2379`'s comment is the truthful
one. `Diffusion` (`:3397`) is not clobbered and is live.

### A6 — DYNAMICS matrix: half the grid is inert

All 96 cells are clickable (`MacPaintUI.cpp:3013-3061`), any of them creates a link
(`:3102`), and each gets a curve, range, invert, easing and a live `OUT` readout.

- **Dead targets** (6 of 12): Scatter, Concentration, Hue, Saturation, Value, Wetness. Only six targets are consumed (`StrokeSession.cpp:225-249`).
- **Dead sources** (4 of 8): Velocity, Fade, Noise, Random. `dynamicInputsFor()` (`StrokeSession.cpp:290-297`) populates only Pressure/Tilt/Azimuth/Barrel; the rest stay hard `0.0` (`Dynamics.hpp:210-213`).

Aggravating: `brush/Library.cpp:124` ships the `Dry Bristle` preset with
`Random → Scatter` — a dead target fed by a dead source — and `:125`'s
`Random → Flow` over `[0.35, 1.0]` resolves to a constant `0.35` forever.
`Dynamics.cpp:48` and `MacPaintUI.cpp:3073` both claim RANDOM "is redrawn per dab";
nothing redraws it.

### A7 — BRUSH EDITOR "Save" does not persist a built-in preset

`MacPaintUI.cpp:3289` overwrites `lib.presets[lib.active]` **in memory**. There is
no serialisation for user-authored presets — `track6/abrlib` added a preferences
file for *imported `.abr` libraries*, and correctly disables Save for those, but a
built-in edited and saved is still lost at quit with no warning. PRD **G6** (P1).

---

## B. Partially wired — a value dropped at exactly one tier

### B1 — The solver canvas cannot be saved, exported or undone

`strokeRouteFor()` sends `Tool::Water` to `PaintSim` for every target
(`StrokeSession.cpp:41`), and Brush/DryBrush there whenever no writable layer
exists (`:86`). That canvas is drawn *under* the document quad
(`MacPaintUI.cpp:5857` vs `:5897`), so the marks are visible — but **nothing in
`src/io/` reads `PaintSim`**, `readbackCanvas()` has no production caller, and the
PaintSim branch never calls `recordEdit()`.

**Watercolour, oil and ink — the product's differentiator, PRD H1/F6 — paint onto
a surface with no history, no save and no export.** Every MEDIUM, SOLVER and
BOARD TILT control governs those pixels. This is the largest single gap in the
audit and probably the most important item in this file.

### B2 — The WET / Water slider reaches no layer deposit

Written by the options bar (`AtelierChrome.cpp:555`) and the BRUSH panel
(`MacPaintUI.cpp:3316`). Its only consumer is `applyToolToBrush()`, whose sole call
site (`MacPaintUI.cpp:6733`) is **inside the solver branch**. `brushTipFor()` never
reads `brush.wetness`, and `BrushTip` has no water field.

With any document open and a paintable layer selected, WET does nothing. Contrast
**Opacity**, three lines away at `:3337`, which is drawn `BeginDisabled` with a
caption naming the route that ignores it. The panel applies the honest treatment
to Opacity and not to Water — which is ignored on *more* routes.

### B3 — Two brush-size sliders with different ranges

Options bar: `2.0f–90.0f` (`AtelierChrome.cpp:528`). BRUSH panel: `1.0f–200.0f`
(`MacPaintUI.cpp:3308`). Set 150 px in the panel, touch the options bar, and it
clamps to 90.

`AtelierChrome.cpp:540` states the rule being broken, for the LOAD slider directly
below it: *"one field behind two widgets with two ranges is two clamps, and the
narrower one silently truncates what the other set."* LOAD, WET and HARD all obey
it. SIZE does not.

### B4 — `DynamicTarget::Spacing` applies on layer strokes but not solver strokes

`StrokeSession.cpp:249` scales `tip.spacing` by the matrix; `MacPaintUI.cpp:6744`
and `:6757` use raw `st.brush.spacing * st.sim.brushRadius`. `Deposit.hpp:359`
claims the two routes "cannot emit dabs at different spacings from one tip" — the
`0.1f` floor matches, the multiplier does not.

---

## C. Whole subsystems built, tested, and unreachable

**The common cause is menus that do not exist.** The bar is File, Edit, Layer,
Medium, Goodies, View, Window — verified against the menu model at `304bd21`.
There is **no Filter menu, no Image menu and no Select menu.**

| Id | Subsystem | PRD | Evidence |
|---|---|---|---|
| **C1** | Blur / Filters / Feather / Transform / DocumentTransform / Roi — ~93 entry points | D4, D5, D9, D11, D12, D14–D18, D20, D21, D23 | No file under `src/ui/` or non-selftest `src/app/` includes any of those headers |
| **C2** | Histogram | **D2, P0** | `core/Histogram.hpp` included only by its own `.cpp` and `selftest/Support.hpp:71`; its own header admits it at `:80` |
| **C3** | Pixel probe readout | **D2, P0** | `core/Probe.hpp` included only by its `.cpp` and `selftest/Support.hpp:80`. Shares an engine with **A1** |
| **C4** | Channels panel + quick mask | Q11, E12, E13 | `core/Channels.hpp` reaches `Document.hpp` and `NpaintFile.cpp` — channels are **saved and loaded** — but no UI file. `ControlsSection` has no Channels slot, though its own comment cites the design's "… / LAYERS / CHANNELS" column |
| **C5** | Selection refine — grow, shrink, feather, colour range, luminance range | E4, E9 | Five proven engines; callers only in `selftest/SelectionRefine.cpp` and `selftest/Blur.cpp`. **No Select menu to put them in** |
| **C6** | Cached / out-of-core tile residency | A7 | Every production site assigns `TileResidencyMode::Eager` (`DocumentLifecycle.cpp:291,346,385`; `Journal.cpp:803`); `Cached` only at `selftest/TileResidency.cpp:212` |
| **C7** | Layer grouping | **C12, P0** | Not unwired — **unwritten**. No Group/Ungroup in `LayerCommand` (`app/LayerEditor.hpp:68-116`) or `LayerSetCommand` (`core/LayerSetOps.hpp:239-284`) |

---

## D. Menu and keyboard gaps

### D1 — ⌘Z does nothing, and cannot be bound. PRD **O1** and **R2**, both P0

There is no `undo` or `redo` action name in `main.cpp`'s dispatch, none in
`keymaps/default.json`, and **no `Undo`/`Redo` enumerator among the menu model's
49 `MenuAction`s** (verified at `304bd21`). Undo/redo is **mouse-only** — the
title-bar buttons and the History panel. R2 (P0) requires matching Photoshop's
keys.

### D2 — The Edit menu has exactly one item: "Clear Canvas"

`src/ui/MenuModel.cpp:405-407`. All nine clipboard/selection commands — Cut, Copy,
Copy Merged, Paste, Select All, Deselect, Reselect, Invert, Delete — are correctly
written *and* consumed (`main.cpp:2184-2192` → `MacPaintUI.cpp:6084-6149`) but have
**no menu items**. If `keymap.loadFromFile` fails (`main.cpp:1934`), PRD M1–M9
(mostly P0) become wholly unreachable behind one stderr line.

### D3 — PRD **R5** (P0) has no implementation at all

Brush size is written only by the two sliders in **B3**. There is no ⌃⌥-drag canvas
gesture and no `[` / `]` binding. R5 calls the gesture the *primary* path and the
brackets the alternate; neither exists.

### D4 — No positional command-line argument

`main.cpp:1010-1144` parses flags only. `naturalPaint foo.npaint` does not open
anything. Cheap to fix now that **R3** landed a content-dispatching entry point.

### D5 — PRD **I13** (P1) save-readback is unimplemented

`saveNpaint()` contains no reader call; `saveDocumentAs()`
(`DocumentLifecycle.cpp:427-446`) goes straight to bookkeeping. `Journal.cpp:448`
calls itself "a small down payment on PRD I13, which does not exist yet" and does
a hash, not a structural verify. See also the note in the project memory about a
green suite once asserting this gap was correct.

### D6 — PRD **F3** (P0) latency has no surface but `--latency`

`app/Latency.cpp:21,29` reports via `std::printf`; the status bar shows fps only.

---

## E. Dead state and dead code

**`AppState` fields:**

- `strokeStarting` (`AppState.hpp:381`) — **zero references anywhere**, including selftest.
- `lastX` / `lastY` (`:379`) — written at `MacPaintUI.cpp:6644,6645,6652,6653,6712,6713,6748,6749`; read nowhere.
- `penDown` (`:410`) — written at `main.cpp:516,519`; read nowhere.

**16 dead functions**, including `roiOpIsValid` (`ops/Roi.hpp:269` — not one
reference of any kind), `documentCanvasRegion` (`ops/DocumentTransform.hpp:364`),
`transformScaleAbout` (`ops/Transform.hpp:286`), `exactRemapName` (`:462`),
`oiioFormatPresent` (`io/OiioBackend.hpp:115`), `combineCoverage`
(`core/SelectionOps.hpp:101`), `History::evictOldest` (`core/History.hpp:538`),
`findChannel`/`findChannelForWrite` (`core/Channels.hpp:202-203`), four in
`core/TileShare.hpp` (`:88,110,118,127`), and `MixboxLut::pixels()`/`kSize`
(`paint/Palette.hpp:37-39`, whose comment "for upload to the GPU" is contradicted
by `PaintSim.cpp:157-159`).

---

## F. Gaps in the verification itself

These are the reason several items above survived so long, and they are worth
fixing before the things they failed to catch.

### F1 — `toolImplemented()` is a hand-maintained boolean tied to nothing

Nothing checks that a tool marked `implemented = true` has a canvas handler. This
is the direct cause of **A1** and **A3**, and it is worse than a missing check:
the machinery built to guarantee "no dead button looks live" (`docs/ui.md:405`) is
what makes those two look *most* alive, because `toolCursorOnTarget()` withholds
the refuse-cursor from anything claiming to be implemented.

`docs/ui.md:398` names Eyedropper among the tools with "real behaviour." It has
none. The doc is stale in both directions — it says seven, the table says
fourteen, the true count is twelve.

*(A check for this is in flight with `track7/eyedropper`.)*

### F2 — The golden harness has never covered the menu bar

`tools/golden/run_golden.sh`'s `view_crop_y=(77 1075 1037 220 700)`. The lowest
crop starts at **y=77**; the title band is y=0–71. When the native menu bar landed
and removed seven labels from that band, **all five views passed.** The menus
really had gone.

Anything in the title band — wordmark, undo/redo buttons, fps, document status
line, the menu bar itself — is currently unverifiable by the harness. A sixth view
over y=0–71 closes it.

### F3 — `--selftest` asserts behaviour no user can trigger

`selftest/Probe.cpp` verifies NxN sample-size averaging and linear-vs-display
readout — PRD Q10 and D2 verbatim — for a tool with no click handler.
`selftest/AbrBrushes.cpp` verifies `Rndn`/`Angl`/`scatterDynamics` import, and its
scatter import feeds a **dead** `DynamicTarget` (see **A6**).

A green suite is not evidence of a reachable feature. That is the whole point of
this document.

---

## Suggested order

Not a commitment — a starting argument.

1. **F1, F2** — the verification gaps, first, so the rest cannot silently regress.
2. **A1 + A2** together — one problem, in flight, and it forces the foreground-colour design that several other items want.
3. **D1, D2** — undo on ⌘Z and a real Edit menu. Small, and both are P0s that make the application feel broken in the first thirty seconds.
4. **A4, A5, B2, B3** — cheap silent-no-op removals, each an afternoon.
5. **C1 + C5** — a Filter menu and a Select menu unlock ~98 already-tested entry points between them. Highest ratio of reach to effort in the file.
6. **B1** — the solver canvas. Large, architectural, and the one that most affects what naturalPaint is *for*.
7. **A6, C4, C7** — dynamics, channels, layer groups.
