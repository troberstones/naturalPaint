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

## T3 — Gradient tool has no hooked-up functionality · open

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

## T13 — The ellipse marquee draws a rectangle while you drag it · open

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

---

## T14 — A transform shows a box, not the pixels · open

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

---

## T15 — Filters have no preview · open · (Cancel already exists)

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

## T17 — Every selection tool shares one cursor, and it is a resize arrow · open

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
  **This one stands.** It is a real accessibility regression and should be
  stated in the work, not discovered by a user who enlarges their pointer.
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
