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
| ~~R7~~ | **A1** — Eyedropper did nothing (PRD **Q10**, P0) | `track7/eyedropper` — three sample sources, five sample sizes; also fixed `probePixel()` diluting alpha with off-canvas texels |
| ~~R8~~ | **A6** — half the DYNAMICS matrix was inert | `track7/dynamics` — and the RANDOM arm of `sourceValue()` is now asserted directly, which is what a constant would have hidden from every behavioural test |
| ~~R9~~ | **C1/C5** — Filter, Image and Select menus were unreachable engines | `track7/filtermenu`, `track7/selectmenu` |
| ~~R10~~ | **A3 / D3** — Zoom tool did nothing (PRD **Q1**/**R5**, P0) | `track8/zoom` — scrubby zoom, `[`/`]` size keys; the `toolNoHandlerException()` row recording Zoom is now deleted, which is what that mechanism existed to force |
| ~~R11~~ | **A7** — `BRUSH EDITOR > Save` did not persist a built-in preset | `track8/brushlib` — a separate, atomically-written user preset file; Save forks rather than shadows a built-in |
| ~~R12~~ | **D5** — PRD **I13** (P1) save-readback was unimplemented | `track8/savereadback` — and it found the larger bug underneath: `saveNpaint()` wrote in place and its failure path *deleted the target*, so a failed save destroyed the previously-good file |
| ~~R13~~ | **B1** — the solver canvas could not be saved, exported or undone | Already resolved by the "stroke bridge" series **before the audit was written**; see B1's own note. `track8/solverio` proved the missing half (a baked layer survives `saveNpaint()`/`loadNpaint()` bit-for-bit) and surfaced **B1a** |

**A note on R13.** An audit is a snapshot, and this one outlived the tree by
seven commits: B1 was already false when it was written. That is worth more than
the item itself — every entry here has a shelf life, and the ones that describe
*absence* ("nothing calls X") spoil fastest, because a single merge elsewhere
falsifies them silently. Re-verify before building against any of them.

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

### B1 — The solver canvas cannot be saved, exported or undone — ~~OPEN~~ **STALE, see B1a**

> **This entry's premise no longer holds, and it was already false when the
> audit was written.** The audit was verified at `304bd21`; the seven "stroke
> bridge" commits (`ab77003`…`9490517`) are ancestors of that same history and
> had already built the machinery this entry says is missing. Verified directly
> against the tree, not taken from a report: `StrokeBakeCycle::step()` runs
> unconditionally every frame (`main.cpp:2638`), `bakeReadyTiles()` writes dried
> solver texels into an ordinary `Layer::pigmentTiles` and calls
> `recordEdit("dried paint")` / `amendEdit(...)` (`StrokeBake.cpp:227-230`), and
> `forceBake()` settles wet paint before every history move
> (`MacPaintUI.cpp:4377`). Because the bake target is a plain
> `LayerKind::Pigment` layer, `io/` needed no changes at all — persisted dried
> paint is indistinguishable from hand-painted content by the time
> `saveNpaint()` sees it. `readbackCanvas()`, cited below, is an unrelated RGBA8
> diagnostic blit; the production path is the deferred tile readback.
>
> Kept rather than deleted because the reasoning below is what the stroke
> bridge was built to answer, and because "the audit item was already stale"
> is itself the finding: **an audit is a snapshot, and this one outlived the
> tree by seven commits.**

### B1a — A refused bake is silent, and the refusal names itself to nobody

`StrokeBakeCycle::step()` and `forceBake()` both return a `BakeCycleReport`
saying *why* a bake did not happen, and **both call sites discard it** —
`main.cpp:2638` and `MacPaintUI.cpp:4377`.

So when solver paint exists but the active layer is not a writable Pigment
layer (an RGB layer selected, no document, the layer locked, or Oil mode), the
paint renders on screen every frame and never persists, and nothing anywhere
tells the user. No data is lost — the solver keeps it — but the user has no way
to learn that the thing they are looking at is not going into their file.

This is category A's defect shape exactly: the refusal exists, is computed
correctly, and is thrown away one line before it could be shown. The fix is a
readout, not a mechanism.

---

*Original entry follows.*

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

### B2 — The WET / Water slider reaches no layer deposit — **half closed**

**The silence is fixed; the capability is not.** Both writers of
`st.brush.wetness` — the options bar (`AtelierChrome.cpp:721`) and the BRUSH
panel (`MacPaintUI.cpp:3909`) — now draw the control `BeginDisabled` when
`wetnessReachesSolver(route)` (`app/StrokeSession.hpp:347`) says this route
ignores it, which is the same honest treatment Opacity already had. Re-verified
2026-08-25 against both call sites and the golden `toolbar` view, where the
control renders greyed with a document layer selected.

**Still true, and it is the substantive half:** `brushTipFor()` never reads
`brush.wetness`, `BrushTip` has no water field, and the only consumer is
`applyToolToBrush()` inside the solver branch. On any layer route WET still does
nothing — it now *says* so instead of pretending, which is a different and
lesser fix than making wetness reach a layer deposit.

### B3 — Two brush-size sliders with different ranges — ~~OPEN~~ **CLOSED**

Was: options bar `2.0f–90.0f`, BRUSH panel `1.0f–200.0f`, so setting 150 px in
the panel and then touching the options bar clamped it to 90.

Both now read the single `kBrushRadiusMin`/`kBrushRadiusMax` pair from
`app/AppState.hpp` (`AtelierChrome.cpp:675`, `MacPaintUI.cpp:3875`), and
`selftest/ChromeConsistency.cpp` asserts that they share it rather than merely
agreeing today. This is the rule `AtelierChrome.cpp` already stated for the LOAD
slider — *"one field behind two widgets with two ranges is two clamps, and the
narrower one silently truncates what the other set"* — now obeyed by SIZE too.

**The visible trace of this fix is why the `toolbar` golden reference moved.**
At radius 20 the grab's travel fraction went from `(20-2)/88 = 0.2045` to
`(20-1)/199 = 0.0955`; over the 256 px of grab travel inside a 276 px track that
predicts a 28.0 px leftward shift, and the measured shift is 28 px (grab left
edge 252 → 224). Recorded here because a golden reference re-blessed without that
arithmetic is a defect being canonised, and this project has done that once
already.

### B4 — `DynamicTarget::Spacing` applies on layer strokes but not solver strokes

`StrokeSession.cpp:249` scales `tip.spacing` by the matrix; `MacPaintUI.cpp:6744`
and `:6757` use raw `st.brush.spacing * st.sim.brushRadius`. `Deposit.hpp:359`
claims the two routes "cannot emit dabs at different spacings from one tip" — the
`0.1f` floor matches, the multiplier does not.

### B5 — Scatter is imported in the wrong unit, and applied on the wrong axis — ~~OPEN~~ **CLOSED, and every sentence below it was already false**

**Both halves landed in `ee796a6`** ("Scatter in the right unit, on the right
axis, and a descriptor field I lost in the merge"), which is an ancestor of
this file's own tree. Verified 2026-08-27 by reading the tree, not the entry:

* **Unit.** `abrScatterFractionToRadii()` (`io/AbrBrushes.hpp:247`,
  `.cpp:688`) returns `fractionOfDiameter * 2.0f`, applied at `.cpp:440-441`
  to every `Scatter` link — the same place `abrSpacingToRadii()` does the
  analogous spacing conversion, which is exactly where the entry below
  *predicted* it should go.
* **Axis.** `applyPerDabScatter()` (`app/StrokeSession.cpp:103`) is
  perpendicular by default and isotropic only under `scatterBothAxes`, which
  `io/AbrBrushes.cpp:451` imports as
  `field("bothAxes").asBoolean().value_or(false)` — Photoshop's own default.
* **"Neither is covered by an assertion today."** Also false, and the most
  wrong of the four: `app/selftest/Scatter.cpp` asserts the
  perpendicular/isotropic split *geometrically*, by projecting displacement
  onto the tangent, rather than by trusting the flag; `selftest/AbrBrushes.cpp`
  asserts the unit end-to-end from a fixture descriptor. Both were sabotage-
  verified again on 2026-08-27: dropping the `* 2.0f` reddens four assertions,
  forcing the isotropic branch reddens three.

**This entry cost a task to disprove, and that is the point of recording it.**
The work was dispatched as an investigation rather than a fix, precisely
because the axis half looked already-done on a five-minute read — and the
investigation came back with *no code change at all*. Had it been briefed as
"apply the factor of two", the agent would have doubled a conversion that was
already correct and shipped a real defect while reporting success. That is not
hypothetical: it is B6's exact failure, one entry below.

Two independent defects that both make an imported brush scatter *less* and
*differently* than the original, and that compound:

* **Unit.** Photoshop's Scatter is a percentage of the tip's **diameter**;
  `applyPerDabScatter()` treats its parameter as a fraction of the **radius**.
  Every imported brush therefore scatters at half the distance the artist set.
  This is the same class of bug as the spacing conversion in
  `abrSpacingToRadii()` — which *is* handled, and whose selftest comment
  explains why it would be invisible — so the machinery to get it right is
  already present and simply was not applied here.
* **Axis.** Photoshop scatters **perpendicular** to the stroke unless "Both
  Axes" is ticked, and it is off by default. Ours is isotropic: the angle is
  drawn off a salted seed, deliberately bypassing the link system. An isotropic
  scatter smears along the stroke as well as across it, which reads as a
  *blurrier* line rather than a *rougher* one.

Both were found while building the Initial Direction / sampled-tip work and are
recorded rather than fixed, because fixing the unit alone would double the
scatter distance along an axis that is still wrong, and the two want to land
together. Neither is covered by an assertion today.

### B6 — Two links onto one Multiply target each contribute their own floor — ~~OPEN~~ **CLOSED, with the magnitude claim corrected**

`addDynamicsLinks()` emits up to two links per target — a control link and a
jitter link — and Size is a `TargetCombine::Multiply` target, so the two
compose as a product. Each link contributes `rangeLo` at source 0, so
Photoshop's **Minimum Diameter**, which is ONE floor beneath the final size,
arrives as a floor per contributing row and multiplies into its own square.

`--abr-report` on Runny Inkers shows the shape directly: eleven of twelve
presets carry both `PRESSURE->Size` and `RANDOM->Size`, so eleven of twelve
brushes paint attenuated. The user has chosen the fix — **keep the jitter,
floor the product once** — which is an engine change to how a Multiply target
composes, not a change to the importer.

**FIXED, and the paragraph above is wrong about the size of it.** The mechanism
landed: the importer emits honest ranges (control `[0,1]`, jitter
`[1-jitter, 1]` as a depth), `BrushLinkSet::multiplyFloor[Size]` carries
Photoshop's Minimum Diameter once, `brushTipFor()` computes it into
`BrushTip::sizeFloorPx` without applying it, and each consumer applies one
`std::max()` at its own last multiply. The two-halves split — the hardware half
in `brushTipFor()`, the stroke-local half in `StrokeSession` — is what made
this larger than it looks, and it is resolved by *carrying* the floor rather
than applying it early: applying `max()` in both halves is only idempotent
while every contribution is ≤ 1, and `rangeHi` goes to 2.0 on the LINK editor's
own slider.

**But the "eleven of twelve paint attenuated" claim was an inference from the
arithmetic at source 0, never a measurement, and it is wrong twice over.**
Rendered `--brush-sheet` on the real pack from both sides of the change and
counted lit pixels per cell:

- **Only FIVE of twelve presets carry a non-zero Minimum Diameter at all** —
  one at 3%, two at 5%, two at 10%. The other seven have `minDiameter == 0`, so
  the "squaring" was `0 × jitterLo = 0`: there was nothing to square. Those
  seven render **bit-identical** across the fix, and the five that changed are
  *exactly* the five with a real minimum. Clean correspondence, nothing else
  moved.
- **"Attenuated" is only half of what the old shape did.** Baking the floor into
  the control link made it `lerp(minDia, 1, p)`, which *inflates* every
  mid-pressure size as well as lifting the bottom. For Blot Bot 8/9
  (minDia 10%, jitter `[0.31, 1]`), new÷old radius runs 0.99 at p=0.9, 0.91 at
  0.5, **0.81 at 0.3** — then crosses over to 1.39 at 0.1 and 3.23 at 0. So the
  fix makes these brushes *thinner* through the body of a stroke and *fatter* in
  the tails, which is what Photoshop's own model says: a plain pressure factor
  with one floor under the product.

Measured net across all twelve cells: **+0.9%**. Individual brushes moved both
ways — ×1.72 and ×1.03 up, ×0.99, ×0.98 and ×0.78 down. The two brushes with
*identical* Size configuration moved in opposite directions, because one paints
a sparse stroke dominated by its thin tails (where the floor lifts) and the
other a dense one dominated by its body (where the old inflation is removed).

The fix is still right — it is Photoshop's model rather than an approximation
of it, and it is what lets **B7**'s tilt-0 case thin to its minimum instead of
vanishing. It is simply not the large visual correction this entry promised.
Recorded at length because the entry was written from arithmetic and believed
for weeks; see the standing note that absence- and magnitude-claims rot.

**Roundness carries the identical defect and is deliberately NOT fixed.**
`addDynamicsLinks()` is one function serving both Size (`minimumDiameter`) and
Roundness (`minimumRoundness`), both Multiply targets that can take a control
link and a jitter link at once. Generalising would need a parallel
`BrushTip::roundnessFloor` and its own consumer wiring. Named in
`BrushLinkSet::multiplyFloor`'s comment and in `addDynamicsLinks()` itself
rather than left to be rediscovered.

### B7 — A hardware source that idles at zero can multiply a brush out of existence

`Kyle's Spatter Brushes - Supreme Spatter & Texture` imports cleanly — right
tip, right radius (142), seven links, no notes — and paints **exactly zero
pixels**, measured, not estimated.

The mechanism is one link: `TILT->Size [0.00..1.00]`. Tilt is a *hardware*
source, it reads `0.0` at rest, Size is a `TargetCombine::Multiply` target, and
a link at source 0 contributes exactly its `rangeLo`. So the size multiplier is
0.00, the radius is 0, and `dabCoverage()` returns on `!(r > 0.0f)` for every
dab of the stroke.

**This is not a preview artifact.** Anyone painting with a mouse, or with a
stylus held upright, supplies tilt 0 and gets the same nothing — with no
refusal, no message, and an import report that says everything arrived.

It is the same shape as the `RANDOM->Size` floor of 0.00 that made Blot Bot 3
paint nothing (fixed at `b704411`), reached through a different source, which
is the argument that the earlier fix addressed an instance rather than the
class. Any `rangeLo == 0` on a Multiply target is a brush that can vanish; the
sources that idle at zero (tilt, azimuth, barrel, velocity, fade) just make it
certain rather than occasional.

Worth deciding alongside **B6** — both are questions about what a Multiply
target's floor means, and a rule that fixes one should be checked against the
other rather than chosen for it alone.

### B8 — Medium is a global mode, and the design says it is a per-layer property

`docs/ui.md` §3.4 resolves this explicitly: medium (watercolour / oil / ink) "is
chosen at Media-layer creation and editable in layer properties; it is not a
global mode, because it is a per-layer property now."

`drawMediumSection()` (`src/ui/MacPaintUI.cpp:4024`) is gated on `st.mode`, a
single `PaintMode` on `AppState`, and its own comment concedes the consequence:
switching medium switches the whole solver. So a document cannot hold an oil
layer and a watercolour layer at once, which is the thing the design decided it
should.

This is a data-model gap wearing a UI symptom — closing it needs `core::Layer`
to carry a medium and the solver to become per-layer aware. It is the same
missing-model problem `docs/ui.md` §3.2 already names for wet state and fill
count, and it is recorded here rather than patched in the panel, because a
UI-only fix would let the control claim a per-layer property the engine cannot
actually hold. Found while auditing the right column against its own design.

### B9 — OPEN QUESTION: which way does Photoshop's `Angl` dial turn?

Half of this is settled and half is not, and the unsettled half is why nothing
has been changed.

**Settled:** `BrushTip::angle` is **clockwise-positive as seen on screen**.
`brush/Deposit.hpp`'s rotation puts the major axis at world direction
`(cos a, sin a)`, and `dy` increases downward in every raster this build owns.
`ops/Gradient.hpp` and `ops/Transform.hpp` derive the same fact independently
for their own rotations.

**Not settled:** whether Photoshop's `Angl` is the opposite (counter-clockwise
positive). If it is, `io/AbrBrushes.cpp` must negate on import, and every
imported brush pairing a non-zero `Angl` with a non-round tip is currently
mirrored about the horizontal.

**Why it is recorded instead of fixed.** The claim that Photoshop is CCW-positive
was asserted from memory, in a task brief, and then came back as a conclusion in
the work that brief produced — with no independent source anywhere in the loop.
This project has already shipped one control ordinal backwards from exactly that
kind of confident recollection (see `AbrControl` in `io/AbrBrushes.hpp`), and it
survived a fully green suite for the same reason this would: everything still
imports, still varies, still paints.

**It is currently unobservable either way**, which is the only reason it is not
urgent. No preset in either sample pack pairs a non-zero static `Angl` with an
elliptical or bitmap tip — Blot Bot 5's `angle 90.0` sits on a `roundness 1.00`
tip, where angle is skipped outright, and a 90° error on an ellipse is hidden by
its own 180° symmetry anyway.

**How to close it:** open any brush in Photoshop's Brush Tip Shape panel, set
Roundness below 100% so the tip is visibly elliptical, set Angle to something
without symmetry (30°, say), and look at which way it leans. That single
observation decides it. `selftest/AbrBrushes` currently pins the angle's
MAGNITUDE with `fabs` and deliberately asserts no sign, so closing this means
tightening one assertion rather than discovering which one was wrong.

---

### B10 — Two easing presets with no chip — ~~OPEN~~ **CLOSED, and the class is now guarded**

`EasingPreset::LogTaper` and `PowerIn` (PaintCopilot §3.2's `log(1+9p)/log(10)`
radius law and `p^2.5` opacity law) were added to `brush/Dynamics.hpp`, built
correctly by `easingCurve()`, matched correctly by `matchesPreset()`, and
covered by nine selftest assertions — and **no chip in the LINK editor could
select either one**. `ui/MacPaintUI.cpp`'s chip row was a hand-written
`presets[3]`, which does not grow when the enum does. Every test passed; the
feature was reachable only from a debugger.

**Caught before the merge, not after**, which is the only reason this entry is
short. It is nonetheless the exact shape of A1–A7 above: the code exists, the
application cannot reach it, and nothing says so.

**The fix is structural, not a fourth and fifth array entry.** The preset list
now lives once, in `brush/Dynamics.cpp`'s `kEasingPresetOrder`, behind
`easingPresetCount()` / `easingPresetAt()` / `easingPresetName()`. The chip row
walks that; it owns only the tooltip text, and does so through a `switch` with
no `default:`, so a sixth preset is a **compile error in the panel** rather
than a chip nobody notices is missing. `selftest/PressureFeel` section 6 then
asserts the enumeration is complete against an independently written list of
every enum value, that no preset is listed twice, that the labels are non-empty
and distinct, and that clicking chip *i* produces a curve `matchesPreset` lights
for *i* and for no other — so "add a preset, forget the row" now reddens the
suite. Verified by sabotage: dropping `LogTaper` from the order array reddens
two assertions; collapsing both response laws to the identity reddens six.

**Related, and still true generally:** `F1` records the same hand-maintained-
list hazard for `toolImplemented()`. This is that hazard in the brush panel.

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

**Scouted 2026-08-25, and there is one obstacle worth knowing before anyone
starts.** Captured `--demo-document --screenshot` at 2560x1580 and cropped
`(0, 0, 2560, 77)` — the band holds the "naturalPaint" wordmark on the left and,
on the right, **Undo**, **Redo**, and a **live FPS readout**. Ink-column
measurement across rows 15–62, against the band's own background luminance of 43:

| element | x range |
|---|---|
| Undo | 2216–2303 |
| Redo | 2316–2401 |
| `"%.1f fps"` (`MacPaintUI.cpp:7332`) | 2446–2550, right-aligned to the window |

The fps number changes every run, so **a full-width view of this band can never
hold a threshold** — it is not glyph-edge noise like `toolbar`'s, it is a
different string. Two ways out, and they are not equivalent:

1. **Crop short of it** — `(0, 0, 2418, 77)` clears Redo's right edge by 17 px
   and the fps text's leftmost ink by 28 px (about 21 px in the worst case, since
   the readout is right-aligned and a longer number grows leftward at roughly
   10.4 px per character). Cheap, touches only `run_golden.sh`, and covers the
   wordmark plus **Undo/Redo enablement** — which is the part actually worth
   covering, given **D1**.
2. **Suppress the readout on screenshot frames**, exactly as `main.cpp` already
   feeds `(-FLT_MAX, -FLT_MAX)` for the pointer on those frames. Strictly better —
   it removes a real nondeterminism source instead of cropping around one, and
   makes the whole band coverable — but it touches `ui/MacPaintUI.cpp`.

Either way the threshold must be **measured**, not inherited: this view contains
text, so it cannot be exact, and `flyout` is the recorded case of a view taking a
threshold from a rule rather than a measurement and passing on luck for two days.

### F3 — `--selftest` asserts behaviour no user can trigger

`selftest/Probe.cpp` verifies NxN sample-size averaging and linear-vs-display
readout — PRD Q10 and D2 verbatim — for a tool with no click handler.
`selftest/AbrBrushes.cpp` verifies `Rndn`/`Angl`/`scatterDynamics` import, and its
scatter import feeds a **dead** `DynamicTarget` (see **A6**).

A green suite is not evidence of a reachable feature. That is the whole point of
this document.

### F4 — `--selftest` cannot reach a single ImGui or SDL dispatch site

Established by sabotage during `track8/zoom`, and confirmed here: corrupting the
*pure* functions in `app/ZoomAndSize.cpp` reddens 7 assertions immediately, but
corrupting the ImGui canvas block or `main.cpp`'s SDL action dispatch that
*calls* them reddens **nothing**. No test anywhere drives ImGui mouse/keyboard
dispatch headlessly; `runKeymapTest()` covers `Keymap::resolve()` and stops
short of the action-string dispatch that consumes its output.

So for every gesture and shortcut in the build, the shape of the coverage is
the same: the arithmetic is proven, and the wire from the event to the
arithmetic is not. A regression that makes a correct, well-tested function stop
being *called* ships silently. This is the same gap that let the original
wheel-zoom anchor bug — a formula that never read the mouse position — live in
the tree with a green suite.

Not new, not introduced by any track, and not cheap to close: it needs a
headless input harness, which is its own piece of work. Recorded because it
bounds what every other green line in this file is worth.

### F5 — a refused save crashes a later section rather than failing it

Found by sabotage, and narrower than it first looks. Forcing
`verifyNpaintRoundTrip()` to refuse every save makes `--selftest` exit **139**
(SIGSEGV) in an unrelated later section, rather than reporting red assertions —
a fixture there saves a file and then reads it back without checking that the
save succeeded, so it hands a truncated file to the PNG reader.

This is a test-harness robustness gap, not a production defect: no production
path makes every save refuse. It is recorded because a suite that *crashes*
under a fault instead of reporting it costs the next person the diagnosis, and
because a crash is the one failure mode that stops later sections from running
at all — which is how a single fault hides an unknown number of others.

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
