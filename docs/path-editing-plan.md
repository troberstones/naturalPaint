# The PATHS panel, the Pen's paint, and Path Select

Written 2026-09-09 against `3002d73`, before the code, in the manner of
`docs/vector-editing.md` — which this file extends rather than replaces. Read
that one first: its §3 (hit-test priority), §6 (one struct, one writer) and §8
(what is on screen) are load-bearing here and are not restated.

Five tracks, in dependency order. A–C can run in parallel; D depends on A and
B; E is the gather.

---

## 0. Three findings that shaped the plan

**0.1 — A pen-drawn shape is invisible.** `pathEditBeginPen()` creates its
shape as `VectorShape s;` — default-constructed, so `Paint::on` is false on
both the fill and the stroke. `core/VectorRaster` is correctly wired into
`core/Composite`, so the shape *is* rasterised; it rasterises to nothing. The
editing overlay draws it, which is why this has never looked broken: the
moment you switch tools, the path vanishes.

The Text tool one screen over does the opposite, and its comment states the
rule this violates:

> The new layer takes the TOOL's current style and the FOREGROUND colour —
> `app/AppState.hpp`'s rule, and the same colour every other tool in this
> build paints with.
> — `ui/MacPaintUI.cpp:15992`

So track B is not decoration. It is the difference between the Pen drawing
something and the Pen drawing nothing.

**0.2 — There is no path-operations module.** `src/ops/` has no `PathOps`;
`core/Path.cpp` holds five queries and `moveAnchorTo()`. Close, open, join,
reverse, delete-anchor, insert-anchor, compound/release do not exist anywhere
in the tree. Every button on the panel needs new headless code — with one
happy exception, see 0.3.

**0.3 — SMOOTH is already written.** `fitAnchorTangent(SubPath*, size_t,
bool closed)` (`app/PenTool.cpp:554`) is Curve mode's per-anchor tangent fit:
a uniform Catmull-Rom tangent converted to a Bezier handle pair, with the
`in`/`out` pair opposite through `pt` *by construction*. That is exactly the
"smooth the tangents of a selected knot" button. It is file-static today and
gets promoted to the new module rather than reimplemented — which also means
the panel button and Curve mode can never drift apart on what "smooth" means.

**0.4 — no tool in this build has a working letter hotkey.** `kToolMeta`'s
shortcut column (`"B"`, `"V"`, `"P"`, …) is display-only text used in
tooltips. `keymaps/default.json` carries 46 actions and not one of them
switches a tool; `main.cpp`'s dispatch is a string else-if chain with no tool
arm. Twenty tooltips currently promise a key that nothing reads. Track C
fixes the mechanism, not just the one new binding.

---

## 1. Track A — `app/PathOps`: the headless verbs

A new module beside `app/PenTool`, in the same relationship to it that
`app/CurveEdit` has to the GRADE widget: pure math and list mutation over
`std::vector<VectorShape>` + `PathSelection`, no ImGui, no `AppState`, no
drawing, and therefore fully exercisable by `--selftest`.

### 1.1 Every verb returns a refusal, not a bool

`PathOpResult { bool changed; PathOpRefusal why; }`, with `why` naming the
reason in the enum and a `pathOpRefusalText()` giving the sentence. This
codebase's established shape (`g_strokeRefusal`, the bucket's refusal ladder,
the flats panel's duplicated sentence), and it is what lets the panel grey a
button *and* say why when it is pressed anyway.

The refusals are the specification. Writing them out is most of the design:

| Verb | Refuses when |
|---|---|
| `closeSubPaths` | selection touches no subpath; every touched subpath already closed; a touched subpath has < 2 anchors |
| `openSubPaths` | the selected anchor's subpath is already open; selection is not exactly one anchor per subpath |
| `joinAtSelectedAnchors` | selection is not exactly 2 anchors; either is not an *endpoint* of an *open* subpath |
| `reverseSubPaths` | selection touches no subpath |
| `smoothAnchors` / `cornerAnchors` | selection is empty, or is not in Component mode |
| `deleteSelectedAnchors` | selection is empty; (never refuses for emptying a subpath — see 1.4) |
| `insertAnchorOnSegment` | the two selected anchors are not adjacent on one subpath |
| `makeCompoundPath` | fewer than 2 shapes selected |
| `releaseCompoundPath` | selected shapes all have exactly 1 subpath |

### 1.2 JOIN — the "merge 2 paths by their knots" verb

The decided rule: **allow across shapes, first shape's paint wins.** Precisely:

- Both selected anchors must be an endpoint (index `0` or `n-1`) of an **open**
  subpath. Anything else refuses.
- **Same subpath, both ends** → that is a close. Set `closed = true`, and
  nothing else; do not duplicate an anchor (`core/Path.hpp` §3: the closing
  segment is implied, never a repeated vertex).
- **Different subpaths** → concatenate. Whichever of the four endpoint
  combinations was selected, reverse whichever subpath is needed so the two
  selected anchors become adjacent, then append. **The two selected anchors
  stay as two anchors** — Illustrator welds them into one when they are
  coincident; we do not, because a weld is lossy and undoable-with-surprise,
  and two anchors a hair apart is a state the user can see and fix. Note this
  in the button's tooltip.
- **Different shapes** → the subpath moves into the shape that appears
  **earlier in the layer's `shapes` vector**; the later shape loses that
  subpath, and is erased entirely if it had no others. Its `fill`, `stroke`,
  `strokeStyle`, `pivot`, `name` and `id` are discarded. The refusal enum
  carries no case for this, but `PathOpResult` gains a
  `bool discardedShapeStyle` so the panel can say *"the second path's fill and
  stroke were dropped"* in its status line after the fact. **A silent discard
  is the failure mode here**, and this is the cheapest honest guard.

Reversing a subpath is not just `std::reverse(anchors)`: each anchor's `in`
and `out` swap, because the segment arriving at an anchor becomes the segment
leaving it. That is one line and exactly the kind of one line that is wrong
in half of the implementations of it. It gets its own assertion.

### 1.3 SMOOTH / CORNER — three verbs, not two

- **SMOOTH** — `fitAnchorTangent()`, promoted from `app/PenTool.cpp`, plus
  `smooth = true`. Handles are *recomputed from the neighbours*, discarding
  where they were.
- **CORNER** — `smooth = false`, and `in = out = pt`. The handles collapse, so
  both adjoining segments become straight (`core/Path.hpp`: a line is a cubic
  whose handles coincide with their anchors). Illustrator's "convert to
  corner point".
- **BREAK** — `smooth = false`, handles left exactly where they are. This is
  the one users actually reach for when they want to kink a curve without
  losing it, and it is *not* CORNER. Two buttons that both say "corner" and do
  different things is worse than three buttons with distinct verbs.

`core/Path.hpp` is explicit that `smooth` "is a hint about intent, not a
geometric invariant" and that nothing enforces collinearity. SMOOTH is the one
place that *makes* it true; nothing downstream may assume it stays true.

### 1.4 DELETE anchor

Removes the anchors, keeps the subpath, does not join anything. A subpath
reduced to 0 anchors is erased; a shape reduced to 0 subpaths is erased. Both
of those cascade silently and that is correct — but a *shape id* disappearing
means `PathEditState::selection` and the open-placement fields can dangle, so
every verb in this module ends by handing back the set of surviving shape ids
and the panel calls a new `pathEditPruneSelection()` (in `app/PenTool.cpp`,
where the single-writer rule lives). Without that, deleting the shape you are
mid-placement on leaves `openPathShapeId` pointing at nothing —
`pathEditBeginPen()` already has a defensive arm for exactly that case, which
is evidence the hazard is real, not hypothetical.

### 1.5 Selftest

`src/app/selftest/PathOps.cpp`, in `--selftest`. Topology first, geometry
second — the join cases are where the bugs are:

- join four endpoint combinations (head-head, head-tail, tail-head, tail-tail)
  and assert the resulting anchor *order*, not just the count
- join across shapes: the survivor's paint, and the loser's erasure
- reverse: `in`/`out` swapped, not merely order-flipped. Sabotage by
  deleting the swap and confirm this reddens and nothing else does.
- close a 1-anchor subpath refuses; `subPathSegmentCount()` must not be asked
  for a segment from a point to itself
- smooth: `in` and `out` exactly opposite through `pt`
- corner vs break: both clear `smooth`, only one moves the handles
- delete cascading to an empty shape, and the pruned selection

Per [[naturalpaint-sabotage-catches-the-repair]], every new assertion gets a
sabotage at gather time, against the **production** line, not a copy.

---

## 2. Track B — the Pen's paint

### 2.1 `VectorStyle` on `AppState`

```
struct VectorStyle {
  Paint fill;          // .on defaults FALSE
  Paint stroke;        // .on defaults TRUE
  StrokeStyle strokeStyle;   // width 1.0
};
```

Sits beside `st.textStyle`, which is the exact precedent: the style the *next*
authored object gets. Defaults are stroke-on / fill-off because that is what a
pen is — a line — and because a filled-by-default open path being auto-closed
for the fill is a surprise.

`pathEditBeginPen()` takes it and stamps it onto the shape it creates. Colours
come from `foregroundLinearRgba(st.brush)` at creation the way the Text tool's
do, so the Pen honours the one foreground colour the whole app shares; the
swatches on the options bar then override per-style.

### 2.2 The options bar row

Added to `drawAtelierOptionsBarContent()`'s `toolEditsPath()` block in
`ui/AtelierChrome.cpp`, above the MODE segment:

```
STROKE  [══ 2.0 ══]  [swatch]  [none]     FILL  [swatch]  [none]
```

**Selection-first, else default** (your call). The rule, stated once so both
controls follow it:

> If the current `PathSelection` names one or more shapes (in either mode —
> Component mode's anchors resolve to their shapes), the control edits those
> shapes and records an edit. Otherwise it writes `st.vectorStyle`.

Two consequences worth designing for rather than discovering:

- **The control has to *show* the selection's value, not the default's**, or it
  lies about what it is about to change. With a mixed selection (two shapes,
  two widths) it shows the first and a "mixed" affordance. A control that
  silently shows the default while editing the selection is the class of defect
  [[naturalpaint-gradient-map-vs-gradient-tool]] is about.
- **A width drag is one undo entry, not sixty.** `recordEdit()` on the first
  frame the drag changes anything, `amendEdit()` after — the same
  `PathEditChange::EditBegan`/`EditContinued` split `pathEditUpdate()` already
  returns, and the reason it returns it.

Cap and join are deliberately *not* on the options bar: they are per-shape
finishing choices, they belong in the PATHS panel, and the band is already
carrying MODE and SELECTED.

---

## 3. Track C — `Tool::PathSelect`, and the hotkey table

### 3.1 The tool

- New `Tool::PathSelect` value. **Appended at the end of the enum's run**, not
  inserted beside `Pen` — `kToolMeta` is one row per value in declaration order
  with a `static_assert` on the count, and `ui/AtelierChrome.cpp:214` states the
  rule out loud: *"a tool shipping moves its comment, never its slot."*
- `kToolGroups`' Pen slot goes from 2 members to 3:
  `{{Tool::Pen, Tool::Curve, Tool::PathSelect}, 3, false}`. The group holds 4,
  so this adds **no palette cell** — `docs/ui.md` §2's 28-cell count is
  untouched, which was the reason the MODE segment exists instead of a
  black/white arrow pair. That reason survives: we are adding one flyout
  sibling, not two cells.
- `kToolMeta` row: `{"Path Select", "mouse-pointer-2", <codepoint>, "A", true}`.
  The Lucide codepoint must be added to `tools/…/codepoints.json`'s source and
  will be caught by `app/selftest/AtelierChrome.cpp`'s icon cross-check if it
  is not.

### 3.2 The tripwire this trips, and how it is satisfied

`app/selftest/Eyedropper.cpp` asserts `toolImplemented(t) ==
toolHasCanvasHandler(t)` for every `Tool`, with a second assertion that
`toolNoHandlerException()` holds **zero** rows. So `implemented = true`,
`toolEditsPath(Tool::PathSelect) == true`, and a canvas block that does
something must land **in one commit**. Adding an exception-table row is
precisely what that second assertion exists to forbid.

`toolEditsPath()` widens to three tools. That is the correct move here and not
an instance of [[absence-claims-and-generic-dispatch]]'s hazard: all three
genuinely edit the same anchor model and want the same gate, which is the
argument `app/PenTool.hpp` §2 already makes for Pen and Curve.

### 3.3 What each tool's press now means

| | Pen / Curve | Path Select |
|---|---|---|
| empty canvas | **place** an anchor (or start a shape) | marquee |
| open path's first anchor | **close** | ordinary anchor gesture |
| an endpoint of another open subpath | **resume** placement from it *(new)* | ordinary anchor gesture |
| any other anchor | *inert* | AnchorDrag |
| a tangent handle | *inert* | TangentDrag |
| a segment | *inert* | select the shape |
| a gnomon handle | *(no gnomon — see below)* | Manipulator |

So: **the Pen no longer calls `pathEditBegin()` at all.** `pathEditBeginPen()`
loses its forwarding arm and its `PenPressResult::Editing`/`Selecting` returns
collapse into a single `Inert`, plus a new `Resumed`. The gesture block routes
on the tool rather than on the hit.

**The gnomon must stop being drawn under Pen.** It is drawn and hit-tested from
one function (`gnomonHandlePositions()`) specifically so drawn and hit geometry
cannot disagree; leaving it drawn under a tool that no longer manipulates
recreates that disagreement at the tool level. `ui/MacPaintUI.cpp`'s overlay
gates the gnomon on `Tool::PathSelect`. Selected-anchor squares and tangent
sticks stay under all three — the Pen must show you the tangent it just laid
down.

Pen's press still ends with the placed anchor selected in Component mode, which
means the Pen writes `selection.mode` behind the MODE segment's back. That is
correct and pre-existing (it is what makes the new anchor's handles visible),
but it means the segment is a *readout* as well as a control, which it already
was.

### 3.4 The hotkey table

- `main.cpp`'s string dispatch gains one arm, not twenty-one: `action` with a
  `tool_` prefix is looked up against a new `slug` column on `ToolMeta`
  (`"path_select"`, `"dry_brush"`, …) and calls `setActiveTool()`.
- `keymaps/default.json` gains one row per tool that `kToolMeta` declares a
  shortcut for — bare letters, and `Shift+` for the flyout siblings that
  declare one (`Shift+M`, `Shift+L`).
- **A new selftest asserts the two agree in both directions**: every
  `kToolMeta` row with a non-empty `shortcut` has a matching binding in the
  default keymap, and every `tool_*` binding names a real slug. Without that
  assertion the shortcut column goes straight back to being decorative, which
  is the state we are fixing.
- `keyChordReachesKeymap()` already gates bare letters out while a text session
  is live, so `A` does not switch tools mid-word. Free.
- The `scope` field on `KeyBinding` stays `nullopt` — tool switches are global.

This is the widest-blast-radius track: twenty bindings that did nothing start
doing something. `docs/shortcuts.md` §1 is the spec being implemented, not a
new claim, but the golden suite and `--selftest` should both be re-run against
the *unchanged* views to catch a binding stealing a key some panel was reading
directly.

---

## 4. Track D — the PATHS panel

`ControlsSection::Paths`, title `PATHS`, role `Tool`, registered in
`ControlsLayout.cpp`, `PanelLayout.cpp` (key `"paths"`) and `drawPanelBody()`'s
switch. `ControlsLayout.hpp` already names this: *"Vector layers are the named
next consumer"* of the layer-scoped-palette pattern `FlatsTools` established.

### 4.1 Placement and reveal — copy FlatsTools exactly

`defaultPlacementFor()` returns `PanelPlacement::Flyout`, and the panel is
revealed on the **transition** into a Vector layer, edge-triggered via
`st.lastActiveLayerKind` — the same block at `ui/MacPaintUI.cpp:13886` that
reveals FLATS TOOLS, extended, not copied. Per
[[naturalpaint-flats-panels]]: level-triggering makes a panel that cannot be
dismissed. Docked, it stays put and greys out with `BeginDisabled`,
deliberately not `panelHasSubject()`.

### 4.2 The one trap that does *not* apply, and why it is worth saying

The flats panel's hard-won lesson was that **a panel button can never see
`onCanvas`** — the docks draw before the canvas hit-test, so the pointer is
over the panel and any command needing a canvas point has to be raised as an
action and consumed in the canvas route.

**Every command here is point-free.** They all act on the existing
`PathSelection`, which outlives the frame. So they are direct calls in the
panel body — `runPathOp(st, PathOp::Join)` — with no `st.pathAction` request
flag and no round trip. That is a real simplification over the flats panel, and
it is worth stating so that nobody adds the request-flag machinery by analogy.

The one exception is **INSERT ANCHOR**, if we ever want it to mean "at the
point I click" rather than "at the midpoint of the selected segment". It means
the midpoint. No canvas point needed.

### 4.3 Contents

```
PATHS
┌ shapes ─────────────────────────┐
│ ▪ Path 1        [██] [▬ 2.0]    │   ← name (dbl-click to rename), fill,
│ ▪ leaf          [  ] [▬ 8.0]    │      stroke swatch + width; row selects
│ ▪ Path 3        [██] [  ]       │
└─────────────────────────────────┘

PATH      [CLOSE] [OPEN] [JOIN] [REVERSE]
          [COMPOUND] [RELEASE]
          RULE  [NONZERO] [EVEN-ODD]
          CAP   [BUTT] [ROUND] [SQUARE]
          JOIN  [MITER] [ROUND] [BEVEL]

ANCHOR    [SMOOTH] [CORNER] [BREAK]
          [INSERT] [DELETE]

MAKE      [SELECTION] [FILL] [STROKE]
```

**MAKE is the point of the panel as much as the verbs are.** Those three
buttons are the first UI callers `app/PathConsumers` has ever had —
`pathToSelection()`, `fillPathIntoLayer()`, `strokePathWithBrush()` are built,
selftested, and listed in `docs/vector-editing.md` §8 under *"Not on screen
yet"* along with the sentence naming this panel as their home. PRD J's three
consumers become reachable in this track.

Every button greys on its own `PathOpResult` refusal, computed once per frame
against the current selection — so the panel is a live readout of what the
selection permits, and pressing a lit button cannot refuse. A status line at
the bottom carries the sentence for the ones that can still fail after the
fact (JOIN's discarded style, per 1.2).

The shape list is the first thing to cut if the branch runs long; the verb
groups are the ask.

---

## 5. Track E — proof

**`--selftest`**: new sections `PathOps` (1.5) and `ToolHotkeys` (3.4);
extensions to the existing `PenTool` section for the changed press semantics
and to `PanelLayout`/`ControlsLayout` for the new section (both already walk
tables that a new enumerator must appear in — `app/selftest/PanelLayout.cpp:24`
and `ControlsLayout.cpp:79` list sections by name and will need the row).

**Golden views.** Existing views that *will* move and must be re-blessed
deliberately, not swept:

- `tools`, `tools_lower`, `flyout` — the Pen group gains a third member
- `pen_options`, `pen_options_component` — the STROKE/FILL row lands above MODE
- `pen_drawing` — `--vector-demo pendraw`'s three anchors now carry a stroke,
  so this becomes the first view where a Pen path is *visible in the composite*
  rather than only in the overlay. That is the screenshot that proves 0.1.

New views: `paths_panel` (the panel over a Vector layer, verbs lit and greyed),
`paths_panel_component` (Component mode: the ANCHOR group lit), and
`pen_options_stroke` if the paint row needs its own crop.

Per [[naturalpaint-golden-update-touches-drift]], `update <view>` also rewrites
other within-threshold PNGs — revert the unasked ones. Per
[[golden-crop-from-harness-capture]], aim new crops from a **harness** capture,
not a hand-run `--screenshot`, which uses my saved panel layout.

**Sabotage** at gather, on the production line, for every new assertion.
[[naturalpaint-test-that-tests-a-copy]] is the failure this prevents.

---

## 6. Open questions I am not deciding alone

1. ~~**Does the Pen resume from a foreign endpoint (3.3, row 3)?**~~ **Decided
   2026-09-09: yes.** Resuming *is* drawing points, and it is how a path is
   picked back up after a tool switch. `PenPressResult::Resumed` is a real
   return value, and the press adopts that subpath as the open placement
   session — reversing it first when the endpoint pressed was index `0`, so
   that placement always appends at the back (`pathEditBeginPen()`'s existing
   `anchors.push_back()` arm stays the only writer of new anchors). The
   reversal is `reverseSubPath()` from track A, which is why A lands first.

2. **Where does `Tool::Curve` end up?** It is Pen-with-auto-tangents. With
   Path Select in the group and STROKE/FILL on the band, the flyout reads
   Pen / Curve / Path Select, which is fine. Flagging only because Curve has
   no options of its own and a user may not find the distinction.

3. **Cap/join chips in the panel or on the band?** Placed in the panel above
   (4.3) to keep the band short. Easy to move.
