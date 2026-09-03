# Vector editing: the two selection modes, the gnomon, and pivots

Stage 4's design pass, written before any of its code. The plan
(`zesty-puzzling-badger`) deferred five questions to "the start of Stage 4"
precisely so they would be decided deliberately rather than discovered; this
file answers all five, and says what each answer costs later.

Two editing models coexist rather than compete. Photoshop's Pen is how a path
gets **authored**. A Maya-style component/object mode with a gnomon is how it
gets **edited**. They share one selection state, one undo path, and one
modifier grammar.

---

## 1. The pivot's coordinate frame

**Decision: anchors stay in document coordinates, the pivot is stored in the
same space, and any transform applied to a *whole shape* is applied to its
pivot as well. A transform applied to a *subset of anchors* is not.**

That last sentence is the whole of "make it stick". Move a shape and its pivot
travels with it; edit some of its points and the pivot stays where the user put
it. Both are what Maya does, and the second is the one a naive implementation
gets wrong by recomputing the centroid.

`core/VectorShape.hpp` already stores it this way:

```cpp
std::optional<PathPoint> pivot;   // nullopt means "use the centroid"
```

`nullopt` is deliberately distinct from a pivot a user has deliberately placed
*at* the centroid. The first tracks the shape as it is edited; the second does
not. Collapsing them would look like a rounding bug rather than a lost field.

### The alternative, and why not

The textbook design is a per-shape `Mat3 transform` with the path kept in an
untransformed local space. The pivot is then genuinely local and invariant
under any object move, which is strictly cleaner.

It was rejected on blast radius. Every consumer of a `VectorShape` — the
rasteriser, `vectorShapesBounds()`, hit-testing, `io/PathSerial`, the SVG
importer, `vectorContentHash()` — would have to compose that matrix, and each
one becomes a site that can be wrong in the same silent way: geometry drawn in
the wrong space, or a cache keyed on a hash that omits the transform. It also
introduces a "is this path local or document space?" question at roughly ten
call sites where today there is no question at all.

The importer already flattens SVG's transform stack at parse time, so flat
document-space anchors is the convention the data arrives in. Staying flat
keeps one space in the whole subsystem.

**The cost, stated:** §2's answer follows from this one, and a future
object-space gnomon would require exactly the rejected change. That is a real
door being narrowed, so it is named here rather than left to be found.

### The second pivot, which is not the same concept

A component-mode selection is a set of anchors that may span several shapes. It
has no object to hang a pivot on, and it stops existing the moment the selection
changes. So there are **two** pivot concepts, named differently so they cannot
be confused:

| | Lives on | Persisted | Default |
|---|---|---|---|
| `VectorShape::pivot` | the shape | yes — serialised, undoable | shape centroid |
| `PathEditState::componentPivot` | `AppState` | no | centroid of the selected anchors |

The transient one is recomputed whenever the component selection changes —
**unless** the user has explicitly placed it, tracked by a
`componentPivotIsUserPlaced` flag that clears on the next selection change.
Without that flag, placing a pivot and then adding one anchor to the selection
silently throws the placement away.

---

## 2. Gnomon orientation

**Decision: world-axis-aligned, in document space.**

This is not a preference, it falls out of §1: with flat document-space anchors
there *is* no per-shape frame to align to. Offering "object" as an orientation
would mean inventing a frame, and the only honest one is the matrix §1
rejected.

The gnomon still inherits canvas zoom, pan, rotation and mirror for display,
because it is drawn through `ViewTransform::toScreen()` like every other
overlay. What is world-aligned is the **axes it constrains motion to**:
document X and document Y, not screen X and Y. Those differ the moment the
canvas is rotated, and constraining to screen axes under a rotated canvas is
the bug this sentence exists to prevent.

Snapping routes through `app/Snapping.hpp`'s `resolveSnap()` rather than growing
a second snap rule.

---

## 3. Handle collision: what wins the hit-test

In component mode the gnomon is drawn on top of Bézier tangent handles, so
without a rule the manipulator makes tangent editing unreachable.

**Pick priority, highest first:**

1. gnomon handles (axis arrows, scale boxes, rotate ring, free-move centre)
2. the pivot marker, when pivot-move mode is active
3. anchor points
4. tangent handles
5. path segments
6. empty canvas → marquee

Three things keep that from being a trap:

- **Tangent handles are drawn only for selected anchors.** The overlapping set
  is therefore small, not the whole path.
- **The gnomon is a fixed screen-space size.** Zooming in does not grow it over
  more of the drawing, which is what makes an overlap permanent rather than
  incidental.
- **Holding Alt/Option suppresses the gnomon entirely for the duration of the
  press**, so anything underneath is reachable without moving the pivot or
  changing the zoom. This is the actual escape hatch; the first two only make
  it rarely needed.

The priority order gets its own assertion. A hit-test whose order is only
implicit in the sequence of `if`s in the handler is one refactor away from
silently changing.

---

## 4. Marquee component selection and its modifiers

Marquee only in the first cut. **Lasso is deferred** — it needs a freehand path
accumulator that the marquee does not, and nothing else in Stage 4 depends on
it.

The modifier grammar is **not** invented here. `core/SelectionOps.hpp:94`'s
`selectionCombineFromModifiers()` already maps modifier state to
`Replace` / `Add` / `Subtract` / `Intersect`, and every raster selection tool in
the app uses it. This reuses that mapping and applies **set** semantics to the
anchor selection instead of coverage semantics:

| Combine | On a set of anchors |
|---|---|
| Replace | the marquee's contents |
| Add | union |
| Subtract | difference |
| Intersect | anchors in both |

One modifier grammar, two implementations of what it means. That is the point:
a user who has learned the modifiers on the Lasso does not have to learn them
again here.

---

## 5. Explicitly deferred

- Soft selection / falloff
- Symmetry
- An object-space gnomon orientation (see §1's stated cost)
- Lasso component selection (§4)

---

## 6. Gesture state: one struct, one writer, an explicit kind

The manipulator has **six** distinct drag meanings, not the three the plan
guessed. Conflating any of them into shared mutable flags is exactly the defect
that made the Gradient tool inert for its entire history — `marqueeDragging` has
three writers and is cleared unconditionally in the selection switch's `else`
arm every frame (`ui/MacPaintUI.cpp:13105`), so a tool that borrowed it never
once committed a stroke.

```cpp
enum class PathDragKind {
  None,
  Manipulator,   // the gnomon: translate / scale / rotate the selection
  PivotMove,     // move the pivot alone, editing no geometry
  Marquee,       // rubber-band component selection
  AnchorDrag,    // drag a single anchor point directly
  TangentDrag,   // drag one tangent handle
  PenExtend,     // the Pen tool laying down a new anchor
};
```

**`PathEditState` lives on `AppState` beside `gradientDrag`
(`app/AppState.hpp:547`), and `app/PenTool.cpp` is its only writer.**

The precedent is `st.brush.tool`, and **which state that field is in depends on
where you are standing**, which is worth knowing before writing the rule down:

- On `main` (since `21f3374`, "Add app/ToolSwitch") it has exactly one writer,
  `src/app/ToolSwitch.cpp`, and that was done because four sites each
  overwrote the field without reading it, so nothing could answer "what was the
  previous tool".
- On **this branch** that was not true when this section was written — the
  vector work forked at `af9368a`, twelve commits before `21f3374` — so the
  field then had five writers across three files. **The merge has since
  happened**, and `grep -rn 'brush\.tool = ' src/` now finds exactly the three
  writes in `src/app/ToolSwitch.cpp`.

The merge was the exact moment the rule was most likely to be broken: a track
branched before `21f3374` writes the field directly and merges clean. The
enforcement is therefore a grep re-run after every merge touching `main.cpp` or
`ui/MacPaintUI.cpp`, plus a selftest that fails if a second writer of
`PathEditState` appears.

Regression-test it the way `app/selftest/GradientTool.cpp:207-245` does — that
test exists because the defect it guards against shipped.

---

## 7. The one thing the data model gives us for free

The plan requires an assertion that a component-mode affine moves **anchors and
their tangent handles together**, so that scaling a subset of points scales the
curvature with it rather than shearing the handles off their anchors.

Stage 1 stored `Anchor::in` and `Anchor::out` as **absolute points, not offsets**
(`core/Path.hpp`). So applying the affine to all three points of a selected
anchor *is* the correct operation, with no special case. The requirement is
satisfied by the representation rather than by code that has to remember.

The assertion is still written, against hand-computed expected geometry, because
"satisfied by the representation" is a claim about the representation and the
representation can change.

One case the free answer does **not** cover: an anchor that is selected while its
neighbour is not. The neighbour's handle points *at* the selected anchor and does
not move — correct, and what Photoshop and Maya both do, but it is a case a test
should pin rather than leave to look like a bug.

---

## 8. What of this design is on screen, as of 2026-09-03

This file was written before the code, so the honest closing section is which of
its decisions a user can actually reach. **Built and photographed** (three
golden views — `vector_shape`, `vector_components`, `vector_marquee`):

- §1's pivot frame, drawn as a crosshair at a selected shape's pivot.
- §3's hit-test priority, driving the canvas gesture in `ui/MacPaintUI.cpp`.
- §4's modifier grammar, via `selectionCombineFromModifiers()`.
- §6's six drag kinds and the single-writer rule — `PathEditState` is mutated
  only in `app/PenTool.cpp`, and `grep -rnP 'pathEdit\.[a-zA-Z]+ *=[^=]' src/ui/ src/main.cpp` finds
  nothing.
- The overlay: outlines, anchors as squares, tangent handles as discs **for
  selected anchors only**, and the marquee — deliberately not marching ants.

**Not on screen yet, by name:**

- **The mode segment.** `pathEditSetSelectMode()` is built and tested, and has
  no caller under `ui/`. So §3's Component mode — half of this document — is
  unreachable from the UI, and the shipped Pen only ever selects whole shapes.
  It is one options-bar row (`docs/ui.md` §4b names it).
- **The gnomon's own handles.** `gnomonHandlePositions()` is *hit-tested*
  (a press inside the ring starts a `Manipulator` drag) but never *drawn*, so
  §2's world-axis-aligned gnomon is currently an invisible target. Scale and
  rotate reach nothing: `pathEditUpdate()` applies a translate for every drag
  kind that is not a `TangentDrag`.
- **The PATHS panel**, and the three PRD J consumers.

§6's own warning came true in a small way and is worth recording: the canvas
block originally ended a drag on `IsMouseReleased` alone, which is what every
sibling gesture does. That is fine for a preview and wrong for a gesture that
*writes the document* — a release ImGui never saw (one that happened outside the
window) leaves the drag live, and every later frame then rewrites geometry and
amends the undo entry from a pointer with no button held. It ends on
`!IsMouseDown` instead.
