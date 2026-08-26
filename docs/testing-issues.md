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

Leads for whoever picks this up, none of them proven:

* The allocation appears **inside the render loop**, not in `GpuContext::init()`
  or `ImGui_ImplWGPU_Init()`.
* `ui/CanvasQuad.cpp` rebuilds bind groups and a vertex buffer per frame
  (`:413`, `:449`, `:461`) — but it also **releases** them (`:421`, `:488`,
  `:491`), so this is churn rather than a leak, and a leak is not what the
  numbers show: RSS is flat at 155.5 MB across repeated samples.
* wgpu-native's own allocator pools and the 2560×1580 HIGH_PIXEL_DENSITY
  surface are unexamined. Testing whether the figure scales with window size
  would separate render targets from pools in one run.

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

## T9 — New Document has no size dialog · open

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

## T10 — The three selection-drag gestures are missing · open

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

## T11 — LAYERS and COMPS have no scroll region either · open

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
