# Testing issues

A running list of defects and gaps found by **using** the application, kept
separate from `docs/reachability-audit.md` because that document is a
one-time audit of unreachable code and this one is open-ended.

Every entry records three things, in this order, and the order is the point:

1. **Reported** — what was observed, in the reporter's words.
2. **Verified** — what is actually true of the tree, checked before any work
   was scheduled against it.
3. **Work** — what therefore has to be done.

Steps 1 and 2 are separated because they disagreed for **four of the first
nine entries**. `docs/reachability-audit.md`'s R13 note argues why an audit
entry spoils; this is the same hazard arriving from the other direction — a
symptom is real, and the cause named alongside it is a guess. A task briefed
on the guess builds the wrong thing and reports success.

Nothing here is scheduled or assigned. Status is one of **open**,
**in progress**, **closed**.

---

## T1 — No way to deselect · CLOSED (was never broken)

**Reported.** There is no way to deselect. Photoshop uses ⌘D.

**Verified — the binding exists and is correct.** `ui/MenuModel.cpp:93` binds
`MenuAction::Deselect` to **⌘D** already, with a native key equivalent
(`MenuKeyEquivalent{'d', kMenuModCmd, "deselect"}`). The chain is whole:
`main.cpp:2599` sets `st.requestDeselect`, and `ui/MacPaintUI.cpp:8464`
consumes it with `installSelection(*od, std::nullopt)`.

So the reported symptom is real but the named cause is not. Two candidates,
and they need a repro to tell apart:

* **The menu item is greyed.** `ctx.hasSelection = doc != nullptr &&
  doc->selection.has_value()` (`MacPaintUI.cpp:6863`). With **no document
  open** it is false, so ⌘D is disabled — and the marquee tools need a
  document too, so this state should be unreachable. Should be.
* **The native key equivalent is not firing**, while the menu item itself
  works when clicked.

**Resolution, 2026-08-26.** Confirmed working by the reporter. Nothing to
fix. Kept because the entry is the evidence that stopped a second ⌘D binding
from being added on top of the working one.

---

## T2 — No way to subtract from a selection · CLOSED (was never broken)

**Reported.** Shift adds to selections, but there is no way to remove from
one. Use the Photoshop hotkey — possibly Ctrl.

**Verified — subtract is implemented, and the hotkey is Option, not Ctrl.**
Adobe's own documentation gives the three modifiers: **Shift** adds,
**Option/Alt** subtracts, **Shift+Option** intersects.
`core/SelectionOps.hpp:80-81` already spells that out in its comments
(`Subtract, // Option/Alt`, `Intersect, // Shift+Option`), and
`MacPaintUI.cpp:8591` resolves them at mouse-down:

```cpp
st.marqueeCombine = selectionCombineFromModifiers(mods.KeyShift, mods.KeyAlt);
```

**Resolution, 2026-08-26.** Confirmed working by the reporter once the key
was named. Nothing to fix. Kept here so the next person hunting for a Ctrl
binding finds this entry instead of adding one.

**One thing it leaves behind.** Photoshop also puts four combine buttons in
the options bar. Every modifier here is invisible until someone is told about
it — which is how this entry came to be filed at all. Discoverability is
tracked with **T10**, not here.

---

## T3 — Gradient tool has no hooked-up functionality · closed 2026-09-02

**Reported.** The gradient tool does nothing.

**Verified — partly wired, and the gap is narrower than "nothing".**
`MacPaintUI.cpp:8929-8954` builds a `GradientGeometry`, builds `GradientStops`,
and calls `renderGradient(*target->rgbTiles, region, geom, stops, sel)`. So
there is a live call site, and it is not a silent no-op in the audit's sense.

What that call site is **not**:

* `geom.kind = GradientKind::Linear` is hard-coded — no radial, angle,
  reflected or diamond, and no UI to pick one.
* The stops come from the current foreground/background, with no gradient
  editor.
* It writes `target->rgbTiles`, so it needs an **RGB layer of an open
  document**. On the painting canvas, or on a Pigment layer, there is nothing
  for it to write — see **T5**.

**Work.** Establish which of those three the reporter hit, then decide scope:
a stop editor and the other four kinds are a feature, while "does nothing on
the surface I was looking at" is a routing bug.

**Reconciled 2026-09-02 — still open, and one false close recorded.** Re-read
against the source rather than against this entry:

* `geom.kind = GradientKind::Linear` is **still hard-coded**, now at
  `ui/MacPaintUI.cpp:13174`. `GradientKind` itself carries `Linear`, `Radial`
  and an angle sweep (`ops/Gradient.hpp:213`), so the four missing kinds are a
  UI gap, not an engine gap — the engine can already draw them.
* The stops are still built at the call site from the foreground alone, and
  the comment there is worth keeping in view: the ramp is
  **foreground-to-transparent**, not foreground-to-background, because
  `docs/ui.md` deliberately has no background half to the swatch. So "add a
  background stop" is blocked on PRD D25/D26, not on this entry.
* **What did change**: the call site now issues
  `od->recordEdit("gradient", EditKind::Content)`, so a gradient is undoable
  and appears in the history panel. That rules out the reporter's literal
  reading ("does nothing") for good.

**A trap that cost a false close, recorded so it does not cost a second one.**
`ui/MacPaintUI.cpp:8596` contains `drawGradientMapDialog()`, which has a full
stop editor — add, delete, reposition, colour-pick, with a live preview. It is
**not** this tool. It is the Gradient **Map** adjustment, a different feature
sharing `ops/Gradient`'s vocabulary. Grepping for `GradientStops` finds it and
makes the gradient tool look finished. It is not.

**Closed 2026-09-02. The reporter was right and both reconciliations above
were wrong: the tool really did nothing, and the reason was not any of the
three gaps either of them named.**

The gradient stored its drag in `AppState::marqueeDragging` and
`marqueeX0..Y1`. The selection-tool switch at `ui/MacPaintUI.cpp:13104` ends
in an `else` arm that clears `marqueeDragging` on every frame a **non**-
selection tool is active — a correct and necessary arm, whose job is to cancel
a selection drag abandoned by a tool change. The gradient is not a selection
tool, so it took that arm every frame: the drag was set on the frame of
pen-down and wiped at the top of the next frame, and `IsMouseReleased` was
therefore never seen inside the drag block. **The pen-up commit could not run
at any point in this build's history.** The `renderGradient()` call both
reconciliations found by reading really was there, really was correct, and was
unreachable.

Why two careful reads missed it: nothing in the gradient's own block is wrong.
The defect is a write performed by *another tool's* code, to a flag the
gradient happened to borrow, seventy lines earlier in the same function. It is
invisible to a reader who starts at the gradient and reads outward, and
invisible to `--selftest`, which cannot reach a canvas block at all. It was
found by **instrumenting the running app** — one `fprintf` of the gate's
inputs, which reported `dragging=0` on every frame of a drag that had been
forced open — after the code had been read three times without it.

Fixed by giving the gesture its own state (`GradientDrag`,
`app/GradientTool.hpp` § 3a) rather than by guarding the `else`: sharing a
mutable flag between two unrelated gestures had also produced a **second**,
independent defect at the other end of the same frame, where the rubber-band
draw's `else if (marqueeDragging || polygonLassoActive)` arm treated any
non-marquee drag as a lasso and drew the stale outline of whatever lasso had
been drawn last. One cause, two bugs, one fix.

The three gaps the reconciliations named are all still true, and are now
narrower. **Two of them survive this closure and have moved to
`docs/spec-vs-implementation.md` § 2** ("Built tools with specced halves still
missing"), which is where designed-but-unbuilt capability belongs — closing an
entry that still carries open work is how work gets lost:

* the stops are still foreground-to-transparent with no stop editor — still
  blocked on PRD D25/D26 for the reasons above, and now built by one shared
  `gradientToolStops()` that the options-bar swatch and the commit both read,
  so a future editor changes one function rather than three call sites;
* ~~`GradientKind` is still hard-coded to `Linear`~~ — **no longer true as of
  later the same day; see the note below**, left struck through rather than
  deleted because a closed entry whose text quietly changes is how a reader
  loses track of what was true when;
* **spread is no longer missing**: Clamp / Repeat / Reflect is a combo in the
  options bar, and `--selftest` proves the setting reaches the pixels rather
  than moving a field nothing reads.

**Amended later on 2026-09-02 — the kinds landed too.** Radial and Angular are
now a KIND combo beside SPREAD; `gradientToolGeometry()` carries
`GradientToolState::kind` instead of hard-coding `Linear`. So of the three
gaps above, only the stop editor survives, and it is recorded in
`docs/spec-vs-implementation.md` § 2 rather than here.

Two things came with the kinds that are worth naming, because neither is
"wire an enum through":

* **SPREAD is drawn disabled on Angular**, with the reason in a tooltip. A
  sweep wraps into [0, 1) and every spread mode is the identity on that
  range, so a live control there would sit over something that provably moves
  no texel — `docs/ui.md` §4a's "no dead button looks live", applied to a
  control rather than to a palette cell. The predicate is
  `gradientKindUsesSpread()`, and `--selftest` proves it agrees with the op by
  rendering the same sweep under all three modes rather than by restating the
  rule.
* **The rubber band is no longer one shape for three geometries.** A Radial
  drag is a radius, so it draws its rim circle; an Angular drag is a
  zero-angle ray, so it draws a clockwise arc and arrowhead. A bare line means
  "from here to there", which is true only of Linear — and one preview
  standing in for gestures that differ is the exact mistake this entry's own
  fix was about.

Coverage: 39 assertions in `app/selftest/GradientTool.cpp`, sabotage-proven,
including that the canvas texels are bit-for-bit the options-bar swatch's own
samples, that a radial is rotationally symmetric about its centre, and that
the angular sweep runs clockwise on screen. Five golden views — `gradient` and
`gradient_spread_off` for the options bar, `gradient_drag` / `gradient_radial`
/ `gradient_angular` for the three geometries under one identical held drag.
**Those canvas views are the artifacts that fail when this regresses**;
nothing headless can cover them.

One assertion in that file was found to be inert while its own sabotage was
being run: the radial's "clear at the rim" probe sat outside the rendered
region and was passing on transparent black for free. It stayed green under a
sabotage that rendered the radial inside out, which is how it was caught. An
assertion whose probe is outside the data is not a weak assertion; it is no
assertion — and only the sabotage step distinguishes the two.

---

## T4 — GRADE nodes have no place in the layer stack · open

**Reported.** The GRADE UI is confusing because the nodes have no place in
the layer stack. Where are they supposed to operate? Also they do not work.

**Verified — there are two grading systems, and that is the confusion.**

* `drawGradeSection()` (`MacPaintUI.cpp:1385`) drives **`AppState::opStack`**,
  described in its own comment as "the session-level grading preview". It
  belongs to the session, not to any layer, which is exactly why it has no
  row in the stack.
* `LayerKind::Adjustment` **already exists and is live** — `core/Layer.hpp:24`:
  "**`Adjustment` stopped being inert at PLAN.md Phase 5 step 5.** It owns no
  pixels" and "transforms the composite accumulated *beneath* it".

So the honest answer to "where are they supposed to operate" is that the
panel and the layer kind are two separate implementations of one idea, and
the panel is the one with no home.

**Work.** Decide which is the real one. The likely answer is that GRADE
becomes the **editor for the selected Adjustment layer's op stack**, and the
session-level preview either goes away or is renamed so it cannot be mistaken
for a document edit. That is a design decision, not a bug fix, and it should
be made before either half is touched. "Also they don't work" needs its own
repro — it may be entirely downstream of editing a stack that composites
nowhere.

---

## T5 — Closing every document leaves a canvas belonging to nothing · open

**Reported.** When you close all documents, there is still a document/canvas
that does not belong to anything.

**Verified — true, and it is the oldest known structural gap in the build,
not a regression.** PLAN.md's Phase 4 step 8 entry states it plainly: "the
live painting canvas is still not one of these documents. `sim::PaintSim`
owns a single dense GPU texture with no layer awareness, so a stroke writes
that texture and touches no `Layer::rgbTiles` anywhere". The File menu's
"New" was renamed **"New Canvas"** specifically so it could not be mistaken
for "New Document".

So the canvas is working as designed. What is *not* designed is what the
reporter actually ran into: with no document open, the canvas is still there
and still paintable, while every document-scoped feature silently has nothing
to act on. **T1, T2 and T3 are all plausibly the same defect wearing three
different hats.**

*(Amended 2026-09-02: T3 was not, in the end, this defect at all — it was a
shared mutable flag erased by another tool's code, and it is now closed. The
grouping above was a reasonable guess from three similar symptoms and it was
wrong about one of the three. T1 and T2 stand.)*

**Work.** Two separable pieces:

* **Short term, and cheap:** make the state legible. A canvas with no
  document behind it should say so, and the tools that cannot act on it
  should be disabled with the reason rather than appearing live. This is the
  `toolImplemented`/`toolHasCanvasHandler` discipline applied to a second
  axis — the surface, not the tool.
* **Long term:** the canvas-to-document bridge PLAN.md describes, which needs
  a decision about which layer the solver deposits into and a
  texture-to-tile path with its own dirty tracking (PRD O6). Genuinely large;
  not a bug fix.

---

## T6 — 500+ MB of memory at startup, against a documented <100 MB · open

**Reported.** naturalPaint shows 500+ MB of RAM on open; it was supposed to
be under 100 MB.

**Verified — both figures are right, and they are measuring different
processes.** Measured on this machine, 2026-08-26, idle, **no document
opened**:

| Measurement | Value |
|---|---|
| `--selftest`'s own `idle RSS` assertion | **92.6 MB** (ceiling 80 MB core + 32 MB OpenImageIO allowance) |
| GUI process, `ps` RSS | **155.5 MB** |
| GUI process, `footprint` (what Activity Monitor shows) | **577 MB** |

`footprint -p` breaks the 577 MB down, and it is not the heap:

| Category | Dirty |
|---|---|
| IOAccelerator (graphics) | **404 MB** |
| IOSurface | 46 MB |
| IOAccelerator | 32 MB |
| MALLOC_SMALL + MEDIUM + LARGE + TINY | 74 MB combined |

**482 MB of the 577 MB is GPU allocation.** So the "<100 MB" figure was never
violated — it is asserted by a **headless** selftest section that has no
window, no GPU adapter and no ImGui, and it therefore **has never covered the
windowed application at all**. This is the pattern
`docs/reachability-audit.md` already names: a green suite describing a
property nobody checked.

**Work.** Give the windowed process a measured budget of its own, the way
`--selftest` has one for the headless path, so this cannot drift again
unnoticed.

**Reconciled 2026-09-02 — the work above is still not done, and one figure in
this entry needs restating so it is not misread.** `--selftest` prints
`[measured] resident 357.0 MB of a 512 MB budget`
(`app/selftest/AtelierChrome.cpp:321`, PRD L7). That number is easy to mistake
for an answer to this entry and is not one:

* It is the **headless selftest process**, the same process this entry already
  showed has no window, no GPU adapter and no ImGui. The 482 MB of GPU
  allocation that *is* the reported symptom cannot appear in it.
* The 512 MB is the **status bar's** design budget — what PRD L7 puts in front
  of the user — not a ceiling anything fails against. `mem.bytes > 0` is the
  only assertion on the numerator; the printed megabytes are asserted by
  nothing.
* It drifts between runs of the same binary (357.0 MB here, 349.5 MB in an
  earlier run) — this suite's documented RSS noise class.

So the gap this entry names is unchanged: **there is still no measured budget
covering the windowed process**, and the 577 MB `footprint` figure has not been
re-measured since 2026-08-26, which now predates
`docs/architecture-review.md`'s P0-1/P0-2. A re-measure is cheap and should
come before any work, since it may well have moved.

**Where the 482 MB comes from is OPEN, and the obvious answer is wrong.** This
entry originally ended "see T7, which is where it comes from". It is not.
`sim::PaintSim` is never constructed at idle — proved by a temporary
`fprintf` at the top of `PaintSim::init()`, which fires once under
`--diag 1` (`[PROBE] PaintSim::init 1024x1024`) and **zero** times across ten
seconds of idle GUI, while `footprint` on that same process still reads
404 MB IOAccelerator. See T7 for why the solver was never the suspect it
looked like.

### Narrowed 2026-08-27 — three measurements, and two of the old leads are dead

**1. It does not scale with the window.** A temporary `NP_PROBE_WINDOW` hook
on `SDL_CreateWindow` (reverted; the tree is clean) ran the GUI at three
sizes, each sampled after 9 s idle:

| Window | Device pixels @2× | IOAccelerator (graphics) |
|---|---|---|
| 640×480 | 1280×960 | **399 MB** |
| 1480×940 (shipping) | 2960×1880 | **401 MB** |
| 2400×1500 | 4800×3000 | **404 MB** |

An 11.7× change in area moves the figure by **1.25%**. A triple-buffered
swapchain across that range would differ by ~158 MB on its own, so **the
surface is not in this number** — the HIGH_PIXEL_DENSITY-surface lead is
refuted, not merely unproven.

**2. It is one-shot at startup, not accumulation.** Sampled at ~1, 2, 4, 8
and 16 s in a single run: 407, 404, 404, 406, 404 MB. Flat from the first
second. That also disposes of the `ui/CanvasQuad` per-frame-churn lead —
there is nothing left for churn to explain.

**3. It is 48 identical 8 MiB allocations.** `vmmap`, resident-size histogram
of the 154 `IOAccelerator (graphics)` regions:

| Count | Size | Subtotal |
|---|---|---|
| **48** | **8192 K** | **384 MiB** |
| 1 | 8224 K | 8 MiB |
| 48 | 32 K | 1.5 MiB |
| 23 | 16 K | 0.4 MiB |
| rest | ≤2 MiB each | ~10 MiB |

**95% of the total is 48 allocations of exactly 8 MiB**, all
`SM=SHM PURGE=N`, all non-volatile.

**What this rules out on our side.** 8 MiB is exactly a 1024×1024
RGBA16Float texture, which is suggestive — but the interactive path has only
four `wgpuDeviceCreateTexture` call sites outside `sim/PaintSim` (proven
unconstructed, above), and none can produce 48 of them:

| Site | Size | Ceiling |
|---|---|---|
| `ui/DocumentTexture.cpp:98` "document composite" | RGBA16F, doc-sized | **`kVisibleDocumentCap` = 2** slots (`DocumentTexture.hpp:452`) |
| `ui/NaturalPaintUI.cpp:114` tile mip chain | `kTileSize` = **128** → 128 KiB | wrong order of magnitude |
| `ui/MacPaintUI.cpp:2890`, `:2954` brush previews | RGBA8, `app/DabPreview` constants | kilobytes |
| `color/LutBake.cpp:59` 3-D LUT | size³ RGBA16F | ~2 MiB at 64³ |

So 384 MiB in 8 MiB units is **not accounted for by naturalPaint's own
textures**, and the remaining suspect is wgpu-native's / Metal's allocator
behaviour.

**Deliberately NOT concluded.** "8 MiB is a common suballocator block size"
is a plausible story and it is *exactly* the kind of header-derived
arithmetic that made T7 wrong. The next person should prove it — instrument
allocation at the wgpu boundary, or vary our own texture demand and see
whether the count of 8 MiB regions moves — rather than inherit this
paragraph as a finding.

**Note the third number.** The status bar's "402 MB / 512 MB" is the
**History** byte budget (`ui/AtelierChrome.cpp:828`), not memory in use. It
is easy to read as a RAM meter and it is not one. Worth relabelling.

---

## T7 — "The solver is allocated at startup" · CLOSED (it never was)

**Reported.** Is a simulation canvas being allocated when the app runs?

**Answered, 2026-08-26: no. It has been lazily allocated all along, and this
entry asserted the opposite.**

`main.cpp:1304` leaves `std::unique_ptr<PaintSim> sim` **null** on the
interactive path, with a comment that already said why — "the interactive path
leaves it null and defers construction to MacPaintUI's canvas … so idle RSS
with nothing painted stays near zero rather than paying for the sim on every
launch". Construction happens only in `ensurePaintSim()`
(`sim/PaintSim.cpp:1621`), and outside the test flags its one caller
(`MacPaintUI.cpp:9238`) is gated on a paint tool actually depositing. The
decision is written up in `docs/adr/0001-lazy-allocation-gated-by-idle-budget.md`,
which was checked in long before this entry was filed.

Per-mode laziness — item 4 of the original work list — is also already built
and asserted: `inkFieldsAllocated()` / `oilFieldsAllocated()` are checked
absent by default, allocated on `setMode()`, and freed on switching away
(`app/selftest/FieldAllocation.cpp`).

**How this entry came to be wrong, because the mechanism matters more than the
mistake.** T6 measured 482 MB of GPU allocation at idle. That measurement was
real. This entry then *counted textures in the header* — 13 `PingPong` fields
× 2, plus 5 singles, at 1024×1024 rgba16float ≈ 248 MiB — and presented the
arithmetic as the explanation. Nobody checked whether the object existed.

That is the failure `docs/reachability-audit.md`'s R13 note already describes,
arriving from the other direction: **a magnitude derived from a header reads
exactly like a magnitude measured from a process.** The B6 entry in the audit
was wrong the same way, and this document's own preamble was written to stop
it. It did not, because the preamble guards the *Reported* line and this was
an error in the *Verified* line.

**What is genuinely true and was worth finding.** With a document open — the
state at launch — `strokeRouteFor()` sends `Tool::Brush` to `RgbDeposit`,
the CPU path, because a fresh document's default layer is RGB
(`core/Document.hpp:211`). The solver is still reachable: `Tool::Water`
returns `StrokeRoute::PaintSim` unconditionally (`StrokeSession.cpp:153`),
and Brush/DryBrush route there whenever there is no target layer. So the
fluid engine is not stranded — but the *default* brush on the *default*
document does not touch it, which is the same canvas-versus-document split
**T5** is about, seen from the routing table.

**No work.** The 482 MB question moves to **T6**, where it started.

---

## T8 — HISTORY panel grows without a scroll region · CLOSED

**Reported.** The HISTORY section of the right panel grows as more is done;
it should have a scroll bar.

**Verified — true.** `drawHistorySection()` (`MacPaintUI.cpp:4642`) draws its
rows straight into the collapsing header with no `ImGui::BeginChild()`
anywhere in it. Other panels in the same file do use one (`##pick`,
`##plan`, `##report`, `##toolgrid`), so the idiom exists and this section
simply does not use it. `MacPaintUI.cpp:7815` already records a related
symptom: HISTORY was "off the bottom of the window at the default size".

**Resolution, 2026-08-26.** Bounded to `kHistoryVisibleRows` (8) row-heights
in a `##historyrows` child, with the buttons and the redo-tail/error lines
left outside it so they never scroll away. Height is row-count times
`GetTextLineHeightWithSpacing()`, not a pixel constant.

**The auto-scroll needed two frames, not one, and the reason is worth
keeping.** `SetScrollHereY()` only sets `ScrollTarget`; the clamp that turns
it into a scroll position ends with
`scroll = ImMin(scroll, window->ScrollMax)`, and `ScrollMax` comes from the
content size measured on the *previous* frame — which is **0** on the frame a
child first exists. Since the panel is normally first drawn on a document
that already has history, "the child's first frame" and "the cursor serial
changed" are usually the *same* frame, so a one-frame trigger clamps to the
top every time. Proved by sabotage: with the holdover cut to 1, the list sits
on "new document · PAST" and the CURRENT row is off the bottom; with 2, it
lands on CURRENT.

**LAYERS and COMPS share the defect.** `drawLayersSection()` and
`drawCompsSection()` have no `BeginChild` either. Deliberately left alone —
tracked as **T11**.

---

## T9 — New Document has no size dialog · CLOSED

**Reported.** Making a new document should let you set the resolution — a
simple dialog with standard presets, a way to make new presets, and a preset
that creates a document at the **system clipboard's** resolution and pastes
its contents in.

**Verified — there is no dialog at all, and the clipboard half is bigger than
it looks.**

`MacPaintUI.cpp:7113` is the whole of New Document:

```cpp
case MenuAction::NewDocument:
  st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(canvasW),
                                         static_cast<int32_t>(canvasH), WorkingSpace{}));
```

It silently inherits the **solver canvas's** dimensions, which is both
undiscoverable and, per **T5**, the wrong thing to inherit from.

The clipboard preset needs something this build does not have.
`core/Clipboard.hpp` describes itself as "**the internal** clipboard" and is
built around sharing copy-on-write tiles with the source document (PRD M5).
There is **no `NSPasteboard` and no `SDL_GetClipboard*` call anywhere in
`src/`** — so nothing here can see an image copied from another application.

**Work — three pieces, and they should not be one task:**

1. **The dialog.** Width, height, resolution, working space, background;
   built-in presets; the last-used size remembered. Follows
   `io/ExportAs`'s existing preset-store precedent for user presets
   (`~/Library/Application Support/naturalPaint/`, `$NP_*` override, a
   refusal-by-name for anything unrepresentable) rather than inventing a
   fourth settings format.
2. **A system-pasteboard bridge.** Read an image off `NSPasteboard` into a
   `Document`. New platform code, and the same question ui/FileDialog just
   answered should be asked first: check what SDL already provides before
   writing Objective-C++ — `SDL_GetClipboardData` may cover it, and the
   ui/FileDialog precedent is that a "platform-integration job" in a comment
   is an estimate someone once made, not a fact.
3. **The preset that combines them**, which is trivial once 1 and 2 exist and
   impossible before.

---

## T10 — The three selection-drag gestures are missing · CLOSED

**Reported.** While drawing a selection: **Shift** should constrain it to a
square/circle; **Space** should move the in-progress region (start a circle,
hold Space to reposition it, release, carry on drawing from the new origin);
and **Option pressed *after* the drag has started** should draw the shape
from its centre rather than from the corner.

**Verified — none of the three exists.** The rectangular and elliptical
marquee case (`ui/MacPaintUI.cpp:8596`) is a plain two-corner drag: on click
it stores `marqueeX0/Y0`, every frame it overwrites `marqueeX1/Y1` with the
cursor, and on release it takes the min/max as the bounding box. No modifier
is read between those two events, and there is no anchor-offset state for
Space to move.

**The design is already half-anticipated, and this is the part to get
right.** `MacPaintUI.cpp:8584` explains why the combine mode is latched at
mouse-down:

> Latched at mouse-down for every tool … Shift is also the constrain
> modifier, so "which boolean" is a question asked once, at the start, and
> not re-read from a hand that moved during the drag.

That is exactly Photoshop's rule, and it generalises to Option too. So:

| Modifier | Held **before** mouse-down | Held **during** the drag |
|---|---|---|
| Shift | add to selection (latched, works) | constrain to square/circle |
| Option | subtract (latched, works) | draw from centre |
| Shift+Option | intersect (latched, works) | both of the above |
| Space | — | move the in-progress region |

The latch that already exists is what makes this safe: the combine mode is
answered once and cannot be changed by a hand that moves, so the live reads
added for constrain and from-centre cannot corrupt it.

**Work.**

* **Constrain.** Square/circle off the larger of the two deltas, keeping the
  sign, so the shape follows the direction of travel rather than jumping
  quadrant.
* **From centre.** Treat `marqueeX0/Y0` as the centre instead of a corner.
  It must be switchable **mid-drag in both directions** — press and release
  Option repeatedly and the shape should track — which means the anchor stays
  stored as-is and the interpretation changes, rather than the anchor being
  rewritten.
* **Space-move.** Needs new state: the offset applied to the anchor. On Space
  down, record the cursor; while held, add the delta to *both* anchor and
  current point so the shape's size is unchanged; on release, keep the offset
  and carry on. The classic bug is applying the delta to only one of the two,
  which silently resizes the shape while it moves.
* All three apply to the **ellipse** as well as the rectangle, and the
  ellipse's centre/radii derivation already sits on the bounding box, so it
  should need nothing extra.
* Lasso and polygon lasso are out of scope: Photoshop does not constrain
  them, though Space-move does apply to the polygon lasso and can follow
  later.

Worth asserting headlessly: constrain, from-centre and space-offset are
arithmetic on four floats, so the geometry can be a pure function that
`--selftest` drives directly, with the widget layer doing nothing but
sampling the modifiers. That is the split `app/ControlsLayout` and
`app/CurveEdit` already use.

---

## T11 — LAYERS and COMPS have no scroll region either · CLOSED

**Verified** while closing **T8**: across the whole of `ui/MacPaintUI.cpp`
only `##pick`, `##plan`, `##report`, `##toolgrid` and the new
`##historyrows` use `ImGui::BeginChild`. `drawLayersSection()` and
`drawCompsSection()` draw straight into their headers, so both grow without
bound exactly as HISTORY did.

LAYERS is the more pressing of the two: a document can carry far more layers
than a session carries comps, and unlike history its row order is
most-recent-*first*, so growth pushes the oldest rows down rather than the
newest ones out of view.

**Work.** The same treatment T8 got. **Reuse T8's two-frame auto-scroll
finding** rather than rediscovering it — the ImGui clamp behaviour is a
property of `BeginChild`, not of the history panel, so any of these that
wants to follow a selection will hit it. LAYERS should follow the *selected*
layer; COMPS probably needs no follow at all, and should not grow one just
for symmetry.

---

## T12 — The right column is a fixed list; it should be the user's · CLOSED

**Reported.** Break up the side panel: let the user choose what shows in the
right panel, allow re-ordering, and save it in the user's preferences.

**Verified — the seam already exists and was built for this.**
`app/ControlsLayout.hpp:149` exposes `controlsSections()`, a
`std::vector<ControlsSectionSpec>` of the twelve sections, and
`ui/MacPaintUI.cpp:7945` draws the column by iterating it. The header says
why it is data:

> The list is data rather than a sequence of calls in the draw function
> precisely so the ordering rule above can be asserted headlessly.

So this feature is not a refactor of the draw loop. It is a second list —
the *user's* — that the draw loop iterates instead, with the built-in list
demoted to "the default and the repair target".

The draw loop also already has a precedent for a section being absent:
`BoardTilt` is skipped outside Watercolor, with the comment "A section can be
absent when its subject is; it is still in the list, because the list is the
column's order and not its contents." User-hidden sections are the same
mechanism with a different predicate.

**Persistence has three precedents, all agreeing.**
`app/UserBrushLibrary.cpp:137`, `app/BrushLibraryFile.cpp:210` and
`app/DocumentLifecycle.cpp:728` all resolve
`~/Library/Application Support/naturalPaint/<name>.txt` with a
`~/.config/naturalPaint/` fallback, and the first and `app/Journal.cpp` both
write atomically via temp-then-rename. The panel layout is a fourth file in
that family, not a new idea.

**Work — split in two, because only one half can be tested.**

* **Headless half (in progress).** An ordered `{section, visible}` sequence
  whose invariant is *every enumerator exactly once* — order and visibility
  vary, membership does not. Reorder, toggle, reset-to-default, and
  save/load. The interesting surface is not the mutations, it is the
  **round-trip repair**: a file written by an older or newer build must load
  into a valid layout. Unknown name dropped, **missing section appended**,
  duplicate collapsed, garbage falling back to the default.
* **UI half (not started).** The draw loop iterates the user's sequence; a
  configuration affordance offers reorder and visibility.

**Two traps, both already paid for once in this repo.**

1. **Serialize by stable text name, never by enum ordinal.** An enumerator
   inserted mid-list silently re-points every previously written file, with
   no parse error — the panel simply comes back scrambled. This is the ABR
   control-ordinal lesson arriving in our own file format.
2. **The missing-section repair is the whole feature's silent-no-op risk.** A
   section added to `ControlsSection` after a user's file was written must
   *appear*. Drop it instead and the new section is unreachable for everyone
   who ever launched an older build — a feature that exists that nothing can
   reach, which is the defect class `docs/reachability-audit.md` is named
   after.

**Deliberately allowed: hiding every section.** The configuration affordance
lives outside the column, so an empty column is recoverable and does not need
a forced-visible section propping it up.

---

## Closing notes for T10, T11 and T12's first half — 2026-08-27

**T10 shipped a Space-move that resized the shape it was moving, and the
unit test passed over it.** The geometry went into `app/SelectionDrag` as a
pure function, which is the right shape. But the first draft translated
*both* corners by the offset, and the call site derives that offset from the
very cursor it also passes as the current point — so the hand's movement was
counted twice on the moving corner and the box grew on every frame of the
move, then jumped when Space came up.

The test did not catch it because it varied the offset with the cursor held
still. **That pairing cannot occur at the call site.** A pure function
extracted from a caller can be driven with argument combinations the caller
never generates, and an invariant that holds across those combinations is
not the same claim as one that holds in use. The fix is one line; the test
that pins it drives the coupled loop, and reverting the formula fails three
of its assertions.

The correct rule reads asymmetric and is not: the offset moves the anchor
only, and because the moving corner *is* the cursor and the cursor has
already moved, both corners still shift by exactly the distance moved.

**T11 found a defect in T8, which was already merged.** `BeginChild()` sizes
the outer box, but a bordered child lays its rows inside `WindowPadding`, so
a box of exactly N row-heights holds fewer than N rows and grows a scrollbar
for content that fits. HISTORY shipped that way — two rows against an
eight-row cap, and a sliver. Verified by removing the padding term again and
watching the sliver return.

Worth noting how it surfaced: T11's brief handed over T8's two findings so
they would not be rediscovered, and the third one turned up *anyway*,
because the agent screenshotted a state T8 never photographed. **Handing
over findings is not the same as handing over coverage.**

**T12's malformed-line rule was mine and it was wrong.** The brief said a
malformed line should invalidate the whole file. It should be skipped: on a
foreign file, discarding buys nothing (nothing parses, every section is
missing, the append rule rebuilds the default regardless), and on a
mostly-valid file it throws away an arrangement the user built over one
damaged byte.

The instructive part is the test: **five of the six malformed-input
assertions passed under either rule**, because their good lines happened to
be in default order, so both rules rebuilt the default. Six assertions, and
the disagreement was invisible to all but one that had to be written on
purpose — with a surviving order the default does not have.

---

## T12 is closed, and it changes what the next feature costs

Landed 2026-08-27. The column now draws `AppState::controlsColumn`; PANELS
chooses membership and order; the arrangement persists.

**The part worth carrying forward is what it did to the cost of adding a
section.** Before, a new panel meant an enumerator, a spec row, a drawer, and
an argument about where in a fixed list it belonged — that last one being the
expensive part, because the order was a shared global decision. Now the order
is the user's, the spec row only supplies a *default* position, and the
missing-section repair rule means every existing user's saved file picks the
new section up automatically instead of hiding it.

So `docs/reachability-audit.md`'s **C2** (Histogram, PRD D2, P0) and **C4**
(Channels, PRD Q11/E12/E13) are now mostly "write the drawer". C4 is the
pointed one: `ControlsLayout.hpp`'s own comment already cites the design's
"COLOR / BRUSH SET. / LAYERS / CHANNELS" column, and `ControlsSection` has no
Channels slot — measured, still zero today.

**And a correction to that audit, made while checking it.** **C3** (pixel
probe unreachable) is **stale**: `core/Probe.hpp` is included by
`app/AppState.hpp` now, wired when the eyedropper landed as R7. **C2 is not
stale** — `core/Histogram.hpp` is still included by nothing but its own `.cpp`
and `selftest/Support.hpp`, verified the same way in the same minute. Two
entries in one section, one rotted and one did not, which is the argument
that document already makes about itself.

---

## T13 — The ellipse marquee draws a rectangle while you drag it · CLOSED

**Reported.** Draw the ellipse interactively instead of showing a rectangle
until you finish the operation.

**Verified — true, and it is one `else` away from the lasso already doing it
right.** `ui/MacPaintUI.cpp:10076` gates the live rubber band on
`st.brush.tool == Tool::Marquee || st.brush.tool == Tool::EllipseMarquee` and
then draws, for both, the four-corner overload at `:10081`. There are exactly
two `drawMarchingAnts()` overloads (`:1022` takes a traced boundary, `:1045`
takes `x0,y0,x1,y1`); no ellipse overload exists. The commit path is not
confused — `case Tool::EllipseMarquee:` at `:9273` builds a real ellipse on
release — so this is purely the preview lying about what mouse-up will do.

Note what is NOT wrong, because it constrains the fix: `marqueeBoxX0..Y1` is
already this frame's `computeSelectionDragBox()` result, carrying
Shift-constrain, Option-from-centre and Space-move (T10). The ellipse preview
must be inscribed in **that** box, not in the drag's raw corners, or the
preview and the commit part company again in a new way.

**Work.** A third `drawMarchingAnts()` overload taking the same four floats
and walking an ellipse inscribed in them, and a branch on the tool at
`:10076`. The ants machinery (phase from `ImGui::GetTime()`, dash crawl along
the contour) is parameterised by a point run, so this is a point generator,
not new ant code.

**Built, and it was a point generator.** `ellipseMarqueePreviewPoints()`
(`app/SelectionDrag.hpp:162`) walks an ellipse inscribed in the four floats;
`ui/MacPaintUI.cpp:1094` feeds that run to the existing ants machinery, and the
branch at the live rubber band picks it for `Tool::EllipseMarquee`. The
constraint this entry flagged is honoured structurally: **both** branches read
the same `st.marqueeBoxX0..Y1`, so Shift-constrain, Option-from-centre and
Space-move (T10) reach the ellipse preview by construction rather than by a
second copy of that arithmetic — only the drawing overload differs.
`kEllipseMarqueePreviewSegments = 32`.

---

## T14 — A transform shows a box, not the pixels · PARTLY CLOSED (RGB yes, Pigment no; stack order closed 2026-09-02; the begin cost is still over budget)

**Reported.** Show the transform results live instead of deferred until done.

**Verified — true, and it is a deliberate design that has now met its limit.**
`app/TransformSession` accumulates a matrix in `pending_` and writes nothing
until `commit()`. That is what makes `cancel()` free and what guarantees one
resample instead of one per drag frame (its header's §1, and the assertion
"a multi-frame-driven commit is BIT-IDENTICAL to one direct `transformLayer()`
call"). `ui/MacPaintUI.cpp` draws the box, eight handles and the rotate disc
from `handlePositions()` and nothing else — so the user is scaling a wireframe
and finding out what it did to the picture afterwards.

**The tension is real and should not be resolved by deleting the invariant.**
Resampling the true result every frame is exactly the "once per drag frame"
cost the model exists to avoid, and on the 2048×2048 measurement in
`app/selftest/DocumentTransform` a full-quality pass is far past a 20 ms frame
(PRD F3).

**Work.** A *preview* that is explicitly not the commit: map the source
tiles through `pending()` at draw time with a cheap kernel (nearest or
bilinear) into the canvas quad, and keep `commit()` exactly as it is —
Catmull-Rom, one pass, from the untouched source. That preserves the
bit-identical assertion, because the preview never feeds the commit. The
honest cost note is that preview and result will differ slightly at edges
while dragging, which is what every implementation of this does and is
strictly better than showing no pixels at all.

**Built (`ui/TransformPreviewTexture`).** The "cheap kernel" turned out not to
need writing: `ui/CanvasQuad` already draws an arbitrary quad through the GPU
rasteriser's own sampler, so the preview is one texture uploaded ONCE at
`begin*()` and drawn every frame at the four corners `handlePositions()`
already computes — the same corners the wireframe segments use, so the pixels
cannot drift from the handles being dragged. `commit()` is untouched and its
bit-identical assertion still holds. The preview reads through
`copyThroughSelection()`, the non-destructive twin of the `cutThroughSelection()`
`commit()` uses, so a `SelectionPixels` preview shows exactly what commit will
move, coverage-weighted edges included.

**What is still open, and this entry stays open for it:**

* **Pigment layers still show the box only.** Previewing one means projecting
  each `Latent` through `latentToRgb()`; that is not built. The preview
  returns empty and the caller falls back to the wireframe, so a Pigment
  transform behaves exactly as it did before — a bounded gap, not a wrong quad.
* **The one upload is 49.0 ms at 2048×2048 fully opaque — 245% of PRD F3.**
  Measured in `--selftest`, printed rather than gated (a wall-clock number is
  this suite's documented flake class). This is a single hitch at ⌘T's
  mouse-down, not a per-drag-frame cost, but it is a real stall on a large
  document. The fix is to pack at *view* resolution — prefilter the crop toward
  the quad's on-screen size before upload, bounding cost by screen pixels
  rather than document pixels — which needs a resample step sized to the
  current zoom and re-triggered when the zoom changes mid-drag. Not one line,
  and not done.
* ~~**The original stays visible underneath.**~~ **CLOSED 2026-09-02 by
  `f597459`** — and closed better than this bullet asked for. The bullet framed
  the choice as "accept the ghost, or pay a full recomposite every frame". Both
  horns turned out to be avoidable:

  * The moving layer *is* now hidden from the composite, via
    `ui/TransformCompositeSplit`'s `documentWithLayerHidden()`.
  * It is not a per-frame cost. The hidden-layer composite is a second
    `DocumentTexture` view distinguished by a new `DocumentTextureKey::variant`
    field, so it is cached across frames like any other; and
    `documentDirtyTiles()` already handles a `visible` flip by narrowing to that
    layer's own tiles, so switching to the variant is **incremental, not a full
    recomposite**. The 7.0 / 28.3 ms figures above are the cost this bullet
    feared and are simply not paid.
  * The user's actual complaint went further than the bullet did — they asked
    for the moving pixels to appear **in the right stack order**, with layers
    above still drawing over them. That needed a three-way split, not a
    two-way: below-half, moving preview, above-half, drawn in that order.
    `transformSplitIsExact()` is the predicate for when the split is
    arithmetically exact — it holds only while every layer above composites as
    a plain Porter-Duff `over`, and it refuses `LayerKind::Adjustment`,
    `LayerKind::Group`, a non-default blend, a clipped layer and a group
    member. When it refuses, the code falls back to the two-way arrangement,
    which is still strictly better than the ghost this bullet described.

  `--selftest` pins the arithmetic with a **bit-identity** assertion —
  `over(above, over(moving, below))` is bit-identical to compositing the whole
  document — plus a permanent assertion that stacking on the *fallback* half
  instead double-counts, which is the exact error the first implementation of
  this made. The golden `transform` reference was re-blessed, and a second
  golden view `transform_stack` was added specifically to hold the stack order.

---

## T15 — Filters have no preview · PARTLY CLOSED (4 of 9 dialogs; the cost does not fit)

**Reported.** Add a preview mode to the filters, and a cancel.

**Verified — half of this is already there, and the half that is there is why
the other half is not free.** All nine modal dialogs already carry a Cancel:
`drawGaussianBlurDialog`, `drawSharpenDialog`, `drawUnsharpMaskDialog`,
`drawAddNoiseDialog`, `drawImageSizeDialog`, `drawCanvasSizeDialog`,
`drawRefineRadiusDialog`, `drawSelectColourRangeDialog` and
`drawSelectLuminanceRangeDialog` — checked one by one, not sampled. None has a
preview.

**Why the existing Cancel does not survive adding one.** Today Cancel is
`ImGui::CloseCurrentPopup()` and nothing else, and that is *correct* precisely
because nothing has been applied yet — the filter runs when the user presses
**Blur**, not before. A live preview inverts that: something is on screen, and
Cancel acquires a job it does not currently have. So this is one piece of work,
not two, and the entry is worded that way to stop a future task being briefed
as "add a cancel button" against dialogs that already have one.

**Work.** Two candidate shapes, and the choice is real:

* **Preview as a display-only overlay** — compute into a scratch buffer at
  view resolution, draw that instead of the layer, discard on Cancel. Cancel
  stays a close. Costs a second evaluation path.
* **Apply-and-undo** — run the real op, let Cancel issue an undo. Reuses the
  op exactly, but puts a rejected filter in the history the user has to see,
  and re-runs the full-resolution op on every slider tick.

The first is the one worth building; it is written down here so the second
is a rejected option rather than an unconsidered one.

**Built — the first shape, and the second stayed rejected.** `previewX()`
computes into a scratch `TileStore` from a `const OpenDocument&`, so it
*cannot* write to the document; the canvas composites a throwaway `Document`
sharing every tile with the real one except the active layer's (a refcount
bump, not a copy). `applyPixelFilter()` was split so preview and commit both
route through one `computePixelFilter()` — "what you saw is what you got" is a
property of there being one implementation, not of two agreeing.

**Four of nine converted**: Gaussian Blur, Sharpen, Unsharp Mask, Add Noise —
the per-layer pixel ops sharing one shape. The other five are deliberately
untouched and behave exactly as before: Image Size and Canvas Size are
document geometry through `ops/DocumentTransform`; Refine Radius, Colour Range
and Luminance Range produce a *selection*, where a live preview means animating
marching ants, not a pixel overlay.

**Still open, and this entry stays open for it — the cost does not fit.**
Measured: 1024² blur 266.5 ms + recomposite 23.6 ms = 290.0 ms (1450% of F3);
2048² = 1136.7 ms (5684%). **Re-measured after
`docs/architecture-review.md` P0-1/P0-2** (the hardware half convert and LTO):
1024² is now 233.9 + 7.0 = **240.9 ms** and 2048² **940.9 ms** — the
recomposite half fell 3.45×, the blur itself only 1.15× because it is bound on
its own float arithmetic rather than on conversion. Still far past budget; the
fix is still view-resolution preview. That is at the app's own default document size. All
four throttle recompute to `IsItemDeactivatedAfterEdit()` — one stall per
completed slider adjustment instead of one per pixel of travel — which bounds
the *frequency* and not the cost. The real fix is the same one T14 names:
preview at view resolution, or on a downsampled proxy. Not built.

---

## T16 — The mask chip is not a control, and no mask can be painted · open

**Reported.** When a layer has a layer mask, paint into it when the layer mask
icon is active; disable it if you shift-click it; and show its result when you
use the Photoshop command that shows the mask (⌥-click).

**Verified — all three are absent, and they are absent for one shared reason
plus one deeper one.**

*The shared reason.* The mask chip is **drawn, not clickable**.
`ui/MacPaintUI.cpp:2512-2514` computes its rectangle and fills it; there is no
`InvisibleButton`, so no click of any modifier reaches it. Every one of the
three gestures needs that control to exist first.

*The deeper one, and it is the expensive part.* There is **no concept of an
active mask anywhere in the tree** — `maskActive`, `editingMask`,
`maskSelected`, `MaskTarget` all return nothing — and, more to the point,
**nothing in this build can paint a mask at all**. `core/Layer.hpp:281` says
so outright: "the content of a mask can only come from a `.npaint` or from a
test writing texels", with `addLayerMask()`/`removeLayerMask()` being "the
whole of the lifecycle a user can reach". `StrokeRoute`
(`app/StrokeSession.hpp:300`) confirms it from the other side: its six values
are `None`, `CpuDeposit`, `RgbDeposit`, `RgbErase`, `PigmentErase`, `PaintSim`
— there is no mask route, and the parametric kinds "already refuse for having
no writable store".

So "paint into it when the mask icon is active" is not a wiring job. It is a
seventh `StrokeRoute` writing a `MaskTileStore`, plus the target concept to
select it.

*And a third thing the report implies but does not name.* Shift-click
**disables** a mask, which means a mask can be off without being removed —
there is no such flag on `Layer` (no `maskEnabled`, checked). That is a new
model field, and therefore a `docs/document-format.md` decision about whether
it round-trips through `.npaint` or is session-only. It should round-trip: a
disabled mask that silently re-enables on reload is a data-shaped surprise,
unlike the group-collapse state (PRD C7) which is genuinely view-only.

**Work**, smallest first, and the first is independently useful:

1. Make the chip a control (`InvisibleButton` on the rect already computed at
   `:2512`), with plain click selecting the mask as the paint target.
2. `Layer::maskEnabled`, its `.npaint` attribute, its reader/writer, and its
   place in the format table. Shift-click toggles it; the chip draws the
   disabled state so it is visible without hovering.
3. ⌥-click shows the mask alone in the canvas — a view mode, not a document
   change, so it belongs beside the grayscale check in
   `docs/operations.md §7` rather than in the layer model.
4. `StrokeRoute::MaskPaint` and the deposit that backs it. This is the real
   cost of the entry and should not be scheduled as if it were part of 1.

---

## T17 — Every selection tool shares one cursor, and it is a resize arrow · CLOSED

**Reported.** Show a lasso cursor for the lasso, a polygon-lasso cursor for
that tool, a circle or square with a crosshair at the bottom-left for the two
marquees, a magic-wand icon for the wand, and appropriate icons for the rest.

**Verified — true, and the module already says so about itself.**
`ui/ToolCursor.cpp`'s `cursorForTool()` maps `Tool::Marquee`,
`EllipseMarquee`, `Lasso`, `PolygonLasso` and `MagicWand` — five tools, all
five listed as consecutive fall-through cases — to the single value
`ToolCursor::Select`, and `sdlCursorFor()` turns that into
`SDL_SYSTEM_CURSOR_NWSE_RESIZE`, a diagonal double-headed arrow. So the lasso
and the wand currently show the same "drag a rectangle" shape, and it is a
shape neither of them means. `ui/ToolCursor.hpp` §3 already names this as one
of its own two weakest entries.

**This one cannot be delivered without reopening a documented decision, which
is why the decision is quoted rather than worked around.** `ToolCursor.hpp:25`
chose SDL system cursors *and rejected custom bitmaps on the record*. There is
no system cursor that is a lasso, a polygon lasso, a wand, or a shape with an
offset crosshair, so every part of this request needs bitmaps. The four
standing objections, and which of them survive:

* **"A missing font would give a blank cursor."** `NP_LUCIDE_TTF` is a
  compile-time absolute path that degrades silently (`ui/Fonts.cpp:333`), and
  a cursor cut from a missing glyph is an invisible pointer — "an arrow that
  conveys too little is a poor cursor; a pointer that is not there is a broken
  application." **Answerable, and the answer is the design:** rasterise, check
  the coverage is non-zero, and fall back to today's system cursor per tool if
  it is not. That check belongs in `--selftest`, which makes the silent
  degradation loud for the first time.
* **"A hotspot per tool that nothing in `--selftest` could check."**
  **Answerable:** a hotspot is data. A test can assert it is inside the glyph
  bounds and, for the marquee pair, that it sits at the crosshair's centre
  rather than at a corner — which is exactly the bug the report is describing
  when it asks for the crosshair specifically.
* **"Bitmaps do not scale with the OS cursor-size accessibility setting."**
  **This objection is simply false on macOS** — see the "Built" section below.
  It was believed on the strength of published claims, acted on, and cost a
  shipped bug.
* **"Bitmaps ignore the user's cursor theme."** **Stands**, and is the milder
  of the two — a drawing tool overriding the pointer over its own canvas is
  conventional.

**Work.** Extend `ToolCursor` from a 10-value category enum to one value per
distinguishable tool, keep `sdlCursorFor()` as the fallback path, and add a
bitmap path (`SDL_CreateColorCursor`) with a per-cursor hotspot and a
non-blank assertion. The two marquees want the reported composite — the shape
plus an offset crosshair — which is one generator taking the shape as a
parameter, not two cursors. **The accessibility cost above should be settled
before this is scheduled**, because "the pointer no longer grows with the
system setting" is a decision for the person who owns the product, not for
whoever picks up the task.

**Built, and switched on.** `setBitmapCursorsEnabled()` defaults to **true**
and `--selftest` pins that default, so it cannot drift back to dark unnoticed.
The flag survives as the fallback switch, not as a gate waiting for permission.

**The bitmaps are keyed by `Tool`, not by cursor intent.** A first revision
answered the report by splitting `ToolCursor` — five new enumerators for the
five tools named. That put the distinction at the wrong layer: what a user
tells apart is the tool, not the family it belongs to, and a five-value split
leaves the other twenty-three tools sharing shapes. `ToolCursor` is back to
being the *intent* enum §2 argues for, and §7's bitmap table is indexed by
`Tool`, so **every tool has its own cursor** — hand, paint bucket, magnifier,
gradient, eyedropper, eraser, pencil, pen, crop, text, move and the rest, each
drawn from the same Lucide glyph its palette cell uses. `toolIconCodepoint()`
is the single source, so changing the palette's icon changes the cursor with
no edit in `ui/ToolCursor.cpp`.

**The hotspot is per tool, and that was a real defect in the first revision.**
Centre-of-glyph was used for every icon, on the reasoning that a tip is not
recoverable from rasterised coverage. True — and the wrong conclusion, because
it is perfectly well known to whoever picked the icon. It put the lasso's
draw point in the middle of its loop instead of at the end of its tail: the
same class of defect the report started from, a cursor that does not say where
the click lands. `cursorHotspotAnchorFor()` now gives each tool a fraction of
its own **inked** bounding box — tail for the lasso and every tipped tool, apex
for the polygon lasso, tip for the wand, the drip for the bucket, the lens
centre for the magnifier, and the centre for the genuinely symmetric ones
(hand, move, gradient) because there the centre *is* the working point.

Resolving against the inked box rather than stb_truetype's metric box matters:
eight of the tools put their hotspot one row past the last visible pixel when
it was resolved against the glyph metrics, because an edge row whose coverage
rounds to zero is inside the metric box and outside the ink.

### The size bug, and why the tests could not see it

**macOS scales an application's own `NSCursor` along with its own cursors.**
Published claims say otherwise, and a revision built on them read
`com.apple.universalaccess`/`mouseDriverCursorSize` and rasterised bigger. On a
machine with the setting at 2.07× the OS's scaling multiplied by ours and
produced a cursor roughly three times the size it should have been. Every
scale assertion in `app/selftest/ToolCursor.cpp` section H passed the whole
time, because each one exercised the rasteriser at a scale the *test* chose —
nothing read the number the application actually uses.

Two things came out of that, and the second is the more useful:

* Cursors now ship at one size, **24 points** —
  `[NSCursor crosshairCursor].image.size` measured, not picked — and the OS
  applies the pointer-size setting on top. Nothing reads an accessibility
  preference. The one scale axis left is the display backing scale, and that
  is SDL's: `SDL_AddSurfaceAlternateImage()` attaches a 48-pixel alternate and
  SDL's Cocoa backend folds base and alternate into one multi-representation
  `NSImage` whose *point* size stays the base surface's.
* `cursorBasePoints()` and `cursorBaseScale()` are **public**, and the suite
  asserts through them rather than through its own constants. Changing the
  shipping size now reddens exactly one line. That is the repair for the blind
  spot, not the size fix itself — a suite full of internal assertions can be
  entirely green while the thing on screen is wrong, and the fix is to make it
  read the number the application reads.

**Still true, and not fixed**: bitmaps ignore the user's cursor theme. That was
the milder of the two objections and the reasoning has not changed — a drawing
tool overriding the pointer over its own canvas is conventional.

---

## T18 — A layered PSD opens with orange halos and stray sparks · open (every named cause fixed; awaiting one visual re-check)

**Reported.** Three Photoshop-authored `.psd` files were supplied to test
`io/PsdImport` against something other than its own writer. Two open
correctly. The third —
`Peter_confronts_a_small_monster_with_fire.psd`, 5000x2559, 53 records —
opens with orange halos around both figures and a scatter of sparks that are
not in Photoshop's own composite.

**Verified — two causes, both decode gaps, neither a compositing bug.** The
importer was compared layer-for-layer against psd-tools 1.18.0 (MIT, used as
an oracle) across all three files: name, count, stacking order, opacity,
visibility, clipping, rectangle, covered-pixel count and mean straight linear
RGBA all match, worst deviation 2.3e-4, which is `rgba16float`'s quantisation
floor. **Everything the reader claims to read, it reads correctly.** The
symptom is entirely what it discards:

* **Ten layers carry a raster mask** (channel id −2) that is walked past and
  dropped. `core/Composite.cpp:30` already multiplies coverage by
  `Layer::mask`, so the receiving feature is complete — only the decode is
  missing.
* **Four layers carry a blend key that is not imported** — `colr` x3 and
  `lddg` x1, all four of them *also* masked, so those four are wrong twice.
  `lddg` (Linear Dodge/Add) is `BlendMode::Plus`, which `core/Blend.cpp:177`
  already implements; it is merely absent from the mapping table. `colr` has
  no equivalent in this codebase's seven modes and the warning is honest.

One layer carries most of it: `Layer 30` is a full-canvas Linear Dodge glow at
20% opacity whose mask has a **mean of 0.017**. Photoshop shows 1.7% of it; we
show all of it, as Normal.

Also dropped, with no visual symptom: **six `lsct` group records per file**
(imported as junk empty layers, one of them named `</Layer group>` in the
panel — `LayerKind::Group` and `Layer::groupTag`/`parent` already exist, and
all six groups are pass-through, which is exactly what `core/Composite.hpp:688`
implements), and **`lspf` transparency-lock on two layers**, whose receiving
field `Layer::alphaLocked` landed in `4931d6d`.

**Work.** `docs/psd-import-gaps.md` — sequence, verified wire layouts,
receiving fields, the assertions to write and the sabotage for each. Masks and
`lddg` together first; they are the whole visible defect. The three files are
the user's artwork, are gitignored by name, and nothing in `--selftest` may
depend on them.

**Reconciled 2026-09-02 — every named cause is fixed in the decoder; the
symptom itself has not been re-checked, and that is why this stays open.**
Verified in `src/io/PsdImport.cpp`:

| Cause named above | Now |
|---|---|
| raster masks (channel −2) dropped | decoded |
| `lddg` missing from the blend table | `{"lddg", BlendMode::Plus}` at `:258` |
| `colr` unmapped | `{"colr", BlendMode::Color}` at `:312` — this entry said the codebase had no equivalent among "seven modes"; it now has 27, and `Color` is one of them |
| `lsct` groups imported as junk empty layers | handled at `:774` |
| `lspf` transparency-lock dropped | handled at `:737` |

`docs/psd-import-gaps.md`'s §§1–4 are therefore all closed. **§5 (`lyid`,
`lclr`) is the one gap left**, and `grep '"lyid"\|"lclr"' src/io/PsdImport.cpp`
finds neither — it is layer ids and panel colour labels, with no bearing on
what the canvas looks like.

**Why this entry is not marked CLOSED.** The report was a *visual* one — orange
halos and stray sparks on
`Peter_confronts_a_small_monster_with_fire.psd`. Nobody has reopened that file
since the fixes landed and confirmed the halos are gone. The causal chain is
strong (`Layer 30` is a full-canvas Linear Dodge glow at 20% opacity whose mask
means 1.7% of it should show, and both the mask and the Linear Dodge mapping
were exactly what was missing), but a chain of reasoning is not the observation,
and this project has been caught before treating the two as interchangeable.
**Closing this entry costs one file open.** The file is the user's artwork,
gitignored by name, and nothing in `--selftest` may depend on it — so this is a
check a human does, not one the suite can inherit.

---

# Batch reported 2026-09-02 — T19–T27

**None of these has had step 2.** They are recorded in the reporter's own
words, with the "Verified" half deliberately empty, because this document's own
header says a symptom and the cause guessed beside it disagreed for four of the
first nine entries. Anything below that reads like a cause is the *reporter's*
reading, not a checked one. **Do not brief work off these until each has been
verified against the tree.**

Two items in the batch are **re-reports of open entries** rather than new ones,
and are noted under T3 and T5 rather than renumbered here.

---

## T19 — The smudge needs more work · open, unspecified

**Reported.** "that smudge needs more work."

No detail was given, and none is invented here. `brush/Smudge` shipped in
`540adf8`; its header records what it deliberately does not do (no Pigment
smudge, alpha-locked RGB refuses), and any of those may be what this means, or
it may be the feel of the result. **This entry needs one round of detail from
the reporter before it is worth anything.**

---

## T20 — Space should be a spring-loaded Hand · closed 2026-09-02

**Reported.** "space bar should switch to the hand tool while held down and go
back to the previous tool when released."

**Closed by `21f3374`,** which had to build the thing the request presumes:
nothing in the build recorded which tool the user was in. Four sites assigned
`st.brush.tool` directly, each overwriting without reading. `app/ToolSwitch` is
now the only writer.

The borrow is deliberately **not** `setActiveTool(Hand)` followed by
`setActiveTool(previous)` — that pair records "previous = Hand" and erases the
fact the feature exists to keep. `beginSpringHand()`/`endSpringHand()` install
the Hand without touching the ledger.

Three guards, each naming a gesture Space already belongs to: text input (Space
is a space, and the layer-rename box is one panel over), any mouse button down
(a marquee drag reads Space as its *move* modifier, and swapping the tool
mid-gesture would abandon the rectangle), and the polygon lasso (the one canvas
gesture that spans frames with the button up). The release asks `!IsKeyDown`
rather than `IsKeyReleased`, so Cmd-Tabbing away with Space held cannot strand
the user in the Hand.

One non-obvious consequence: the Measure handler's clear arm needed
`!springHandHeld()`. Its gate reads `toolMeasuresCanvas(st.brush.tool)`, which
IS false during the borrow, so without it panning to look at the far end of a
measurement would have deleted the measurement.

---

## T21 — The tool settings do not follow the active tool · closed 2026-09-02 for the wand and the bucket

**Reported.** "the tool settings need to be updated to reflect the setting for
the active tool, such as magic wand should have options for tolerance,
contiguous/noncontiguous toggle."

Note that this is a general complaint with one example, not a request for two
magic-wand controls: the options bar is expected to be *per tool*, and the wand
is the case that made it obvious.

**Closed by `a1294ff` for those two tools; the general complaint stays open**
as the `docs/ui.md` §4b inventory of which tools bring their own options bar.

Each tool holds its OWN `FloodFillParams`. One shared block drawn under two
tool names would be a hidden coupling — nudge TOLERANCE with the wand selected
and the bucket's next click changes too, with nothing on screen saying so.
`floodToolParamsFor()` is the single mapping from tool to block, so the row the
user edits and the click that consumes it cannot look at different structs.

TOLERANCE is shown in Photoshop's 0..255 and stored in the engine's 0..1, as a
conversion rather than a second field. REACH's engine word is `Global`; the
band says "All Similar", PRD D25's own phrase.

**Option no longer forces Global on either tool.** With REACH visible, the
modifier was a second source of truth that made the band wrong whenever the key
was down — and on the wand it was double-booked, since Option is Subtract for
every selection tool, which meant "subtract a contiguous region" was a gesture
this build could not express. The shortcut became a visible control and the
modifier went back to meaning one thing.

The first capture taken for the `wand_options` golden view found a defect of
exactly the class the view exists for: the REACH combo's width was measured
with the band's proportional face and drawn in the mono one, so "Contiguous"
was clipped under its own arrow. Headless, that combo was perfect.

---

## T22 — A single click lays no dab · closed 2026-09-02

**Reported.** "the brush engine right now won't start stamping until the brush
moves after being clicked, but we need to support single click dabs, so adjust
that, single click draws dab, moving stroke will do what it currently does."

The desired behaviour is stated exactly: **click deposits one dab; a drag
behaves as it does today.** This plausibly applies to every route that begins a
stroke, not only the brush — there are nine as of `540adf8`.

**Closed by `87e1eaf`,** and the nine routes were the reason it was one fix
rather than nine: `brush/StrokePath` is the sole dab emitter for all of them.
`leftover_` starts at 0, so a moving stroke's first dab lands one full spacing
along the path and the origin is never stamped; a click, having no path at all,
emitted nothing.

`flush()` now emits the pending point when the accumulated chord distance never
exceeded 1e-3 px. Distance, not "was there one point" — a click that jitters by
a texel under the pen is still a click, and a two-point path that has genuinely
moved must still go down the spacing walk.

---

## T23 — The clone stamp shows no source · closed 2026-09-02

**Reported.** "clone needs to show an indicator of where it is cloning from,
Opt click should set that anchor with an indicator of where it was put and
drawing with the clone tool should move that indicator to show what is
currently being cloned."

Three separate pieces of feedback are being asked for: the anchor at the moment
it is set, a persistent marker for where the source is, and a *live* marker
that tracks the sampled point during a stroke.

**Closed by `71c14f4`.** All three pieces: the anchor crosshair where Option
put it, the persistent marker, and the live ring at `pointer + offset` with a
leader line between them.

The state underneath was already asserted headlessly and was already right;
what was missing was every pixel of it. So the coverage is two golden views
sharing one crop, and the negative is not decoration: `clone_anchor` runs the
same fixture with the offset **not** latched, because before the first pen-down
the source IS the anchor. A build that dropped that gate would draw the ring
concentric with the brush cursor — where it is least likely to be noticed — and
`clone_source` would still pass.

---

## T24 — The measure angle should reach the transform panel · closed 2026-09-02

**Reported.** "The measure tool's angle should be remembered so that when the
transform panel is open, and the measure was the last tool the angle from the
measure is put into the transform angle field, if it wasn't the last tool the
angle should be zero."

The conditional is the whole feature and is easy to drop: the handoff happens
**only when Measure was the last tool**, and the field is zero otherwise.

**Closed by `21f3374`,** alongside T20 — both needed the same missing record of
which tool the user was in.

Two corrections to the framing above, both found while building it. The angle
goes in the **Numeric Transform modal** (`Image > Transform…`), which is where
this build's rotate field actually lives; there is no transform *panel*. And
the predicate is the tool the user is **in**, not the previous one: the ruler
is destroyed the frame the tool changes, so keying the handoff off `previous`
would be a branch no running state can reach.

The zero is the half that matters and the half that is easy to drop — a ruler
measured on another document, a dismissed ruler, and every non-Measure tool all
seed exactly 0.0. The seeded angle is also pushed through `setPending()`:
otherwise Apply-without-touching-anything would commit the identity while the
field read 37.4.

---

## T25 — The colour picker clamps a canvas that does not · open, a question

**Reported.** "The canvas supports fp16 data, but the color picker only shows
values clamped to 1, what can we do about it?"

Asked as a question rather than a defect report, and it should stay one until
someone has established what the options are. It touches
`app/AppState.hpp`'s `BrushState::rgb` encoding contract (sRGB, display-
referred) and everything downstream that decodes it, so "unclamp the widget" is
unlikely to be the whole answer.

---

## T26 — The layers panel opens with a document-name row · closed 2026-09-02

**Reported.** "with the layers panel, what is the first UI item, it seems to
show the document name. remove it."

Half question, half instruction. The question deserves an answer before the
removal: if that row is load-bearing for anything, that should be said out loud
rather than discovered by deleting it.

**Answered, then done.** The row is the LAYERS *header band*
(`ui/MacPaintUI.cpp`, the `--- The header band ---` block): the document name
on the left, and on the right the layer count in monospace. It is the design's
tab strip with the tabs removed — that section's own doc comment argues at
length why there are no tabs — so what survived was the strip's right-hand
slot plus a name nobody asked for.

**The name is gone. The count stays, and they are not the same question.** The
name was redundant three ways over: the tab strip above the canvas names every
open document and marks the active one, the title band names it again, and this
panel is unambiguously about whatever document is active. The count is the only
place the name filter's effect is visible — with five rows hidden it reads
`3/8`, which is what separates "this document has three layers" from "this box
is hiding five of them". Removing it would delete feedback rather than a
duplicate.

**A stale claim found next to it, and fixed.** That band's tooltip ended with
"a stroke reaches no layer and nothing painted appears here." That was true
when written and has been false since the RGB stroke routes landed:
`strokeRouteWritesLayer()` now answers true for **eight of the nine** routes,
and only `PaintSim` — the solver route a Pigment layer takes — still paints
somewhere this panel cannot show. Rewritten as the narrower true statement
rather than deleted, because the surprise it exists to prevent is real; it is
just no longer the general case.

**A coverage gap, recorded not closed.** Golden passed 16/16 across this
change, which means the LAYERS header band sits outside every crop — the
`layers` view starts at y=927, below it. Same shape as the finding that every
crop starts at y=77 and so the title band has never been covered. Not worth a
17th view for a row that now holds one number, but worth knowing before someone
reads a green harness as coverage of this panel's top.

**Left undone, deliberately.** The row now holds a single right-aligned number
and reads sparse. Folding the count up into the `LAYERS` collapsing header
would remove the row outright — which may be what the reporter meant — but that
header is shared machinery for all fifteen panels, and changing it for one of
them is a different job from this one.

---

## T27 — Throttle the UI · open

**Reported.** "lets throttle the UI unless drawing to 60fps, and when nothing
is happening, throttle it further."

Two tiers are asked for: a 60 fps ceiling when idle-but-interactive, and a
lower one when nothing is happening at all — with drawing exempt from both.

---

## Re-reported 2026-09-02, against entries already open

* **T3 (gradient)** — reported again as "the gradient tool does nothing."
  **Resolved 2026-09-02; see T3.** The re-report was right and the
  reconciliation it contradicted was wrong. The note that closed this bullet
  is worth keeping as written, because it was the thing that led to the fix:
  *"the discrepancy is the useful part of this re-report — it says either the
  reporter is hitting the routing case with no feedback, or the tool is
  failing somewhere the reconciliation did not look."* It was the second one.
  A user report that contradicts a careful read of the source is evidence
  about the read, not only about the user; the read had covered every line of
  the gradient's own block and none of the line in another tool's block that
  erased its state.

* **T5 (a canvas belonging to nothing)** — reported again as "when all
  documents close, there should be no active canvas." That is T5 exactly, now
  stated as the desired behaviour rather than as a symptom, which is a useful
  sharpening: the ask is not "explain the canvas" but "close it with the last
  document."
