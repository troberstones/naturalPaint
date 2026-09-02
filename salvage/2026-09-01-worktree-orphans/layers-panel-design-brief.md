# Design brief — the LAYERS panel

For a design tool. Everything below is a constraint or a fact about the product, not a
suggestion, unless it is marked **Design question** — those are the parts to solve.

---

## 1. The product, in the three sentences that matter to this panel

**naturalPaint** is a desktop painting and image-processing application on WebGPU whose
distinguishing capability is that **colour mixes as real pigment does** (Kubelka–Munk),
not as RGB. Painting and image processing are equally first-class; neither is a follow-on
to the other.

A layer here is therefore not "a bitmap with a blend mode". **A layer has a kind, and the
kind says how colour combines inside it.** That is the product's whole differentiator, and
the current panel hides it — a Pigment layer and an RGB layer look identical in a row. The
single most important job of this design is to make kind legible at a glance without
turning the panel into a zoo of icons.

Renderer is Dear ImGui: **flat fills, hard rules, no corner radius, no gradients, one
shadow in the whole application** (on the canvas). A rounded, soft, glassy design is not
implementable here — do not produce one.

---

## 2. Placement and hard dimensions

```
┌────────────────────────────────────────────────────────────┐
│ naturalPaint │ File Edit … Help        undo redo ⟲ panels  │ 36 px
├────────────────────────────────────────────────────────────┤
│ ▨ study-plate-04.npaint ●│ retouch-ref.tif 64% │ … │ +      │ 34 px
├────────────────────────────────────────────────────────────┤
│ ■BRUSH│ PRESET ▣ Round Bristle 03 │ SIZE ── 48px │ HARD …  │ 46 px
├──────┬──────────────────────────────────────┬──────────────┤
│ tool │                                      │ COLOR        │
│ 2×n  │            canvas + rulers           │ BRUSH SET.   │
│ grid │                                      │ ►LAYERS      │  ← this panel
│      │                                      │ CHANNELS     │
│ FG/BG│                                      │ HISTORY      │
├──────┴──────────────────────────────────────┴──────────────┤
│ 64% │ 2048×1536 · LIN16 │ 214 MB / 512 MB │ Clone source…  │ 26 px
└────────────────────────────────────────────────────────────┘
  104 px                                          322 px
```

- **Panel width: 322 px.** Fixed. Everything must work at that width — this is the single
  tightest constraint in the brief and most Photoshop-derived row layouts do not survive it.
- Right-hand column is a stack of collapsing headers; LAYERS is one of them and shares
  vertical space with COLOR, BRUSH SETTINGS, CHANNELS, HISTORY and COMPS. Assume LAYERS
  gets **roughly 380–520 px of height** in the common case, and must degrade gracefully to
  200 px.
- Minimum window is 1366×1024. Desktop only. Pen input (pressure/tilt/barrel) with mouse
  fallback and keyboard modifiers — **no multi-touch**; anything the design would express
  as a second finger becomes a modifier key.

---

## 3. Visual tokens — use these exact values

Dark chrome, light paper. The canvas is the only bright surface in the application.

| role | value |
|---|---|
| chrome base — panels, palette, status bar | `#2d2b2b` |
| chrome deep — tool options, active tab, navigator | `#201e1d` |
| chrome mid — tab strip, internal fills | `#444141` |
| rule — 2 px, between major regions | `#f3f2f2` |
| divider — 1 px, internal | `#444141` |
| hairline, rulers | `#9b9797` |
| text primary | `#f3f2f2` |
| text secondary | `#9b9797` |
| accent — active tool, dirty marker, selection | `#ff563c` |
| row selected | `#7c1405` |
| canvas paper | `#f8f4f4` |
| on-accent foreground | `#201e1d` |

- **2 px `#f3f2f2`** rules between major regions; **1 px `#444141`** internally.
- Type is **Archivo** (400 / 600 / 800) with `ui-monospace` for **all numerics and caps
  labels**; 800-weight caps carry `.10–.14em` tracking.
- **Every numeric in the chrome is monospace and right-aligned in a fixed-width cell.**
  These are live values; a proportional font makes the layout judder as the numbers change.

The seven layer colour-label swatches are fixed and are deliberately *not* from the theme
palette — they must stay distinguishable from each other as chips a few pixels wide, and
muted enough that a column of them does not out-shout the rows they annotate:

| label | hex |
|---|---|
| red | `#d44a45` |
| orange | `#de8733` |
| yellow | `#d9c23d` |
| green | `#5cad59` |
| blue | `#4782d1` |
| violet | `#8f61c7` |
| grey | `#8c8c8c` |

---

## 4. The structural facts a designer must not get wrong

1. **The panel is upside down relative to the model.** The panel lists the top layer first;
   the document stores bottom-first. Nothing in the design should imply a bottom-first list.
2. **There is no Background layer.** A new document's base layer is an ordinary layer with
   alpha — not locked, not special, renameable, deletable, movable. Do not design the
   Photoshop italic-"Background"-with-a-padlock row; it does not exist here.
3. **The bottom layer cannot be clipped** (nothing below it to clip to), and a move that
   would put a clipped layer at the bottom is refused.
4. **Groups are not built.** The file format reserves them (a member layer names its
   parent), but this build creates none. Reserve visual room for one level of nesting;
   do not require it to function.
5. **A layer's name is not unique and is not an identity.** Two layers may both read
   "Layer 1". An unnamed layer's row shows a synthesised positional label ("Layer 3") that
   *changes when the layer moves*.
6. **Locked does not mean frozen.** A locked layer still accepts: visibility toggle,
   unlocking, duplication, and colour-labelling — labelling is precisely how a user marks a
   layer they have finished and locked, so a lock that froze the label would fight its own
   most common use.

---

## 5. The capabilities that make this panel unlike Photoshop's

This is the section the design should be built around. Each item is a thing the panel must
express; several have no Photoshop equivalent to borrow from.

### 5.1 Layer kind (the differentiator)

Seven kinds. The three raster kinds are **paintable** and are named for how colour combines;
the other four hold no pixels and are evaluated from parameters.

| glyph | kind | what it is |
|---|---|---|
| `◉` U+25C9 | **Pigment** | Pixels are pigment latents. Colour combines under Kubelka–Munk. **The default kind for a new layer.** |
| `□` U+25A1 | **RGB** | Pixels are linear working-space RGBA. Ordinary interpolation. For imports, image processing, and deliberate RGB painting — a first-class choice, not an import artefact. |
| `◈` U+25C8 | **Media** | A Pigment layer advanced by the fluid solver — watercolour, oil or ink. The simulated one. Carries **wet state**. |
| `✂` U+2702 | **Strokes** | No pixels: an ordered list of clone/heal dabs replayed against the composite *below*, so the result re-derives when anything underneath changes. |
| `▤` U+25A4 | **Adjustment** | No pixels: an op stack applied to the composite accumulated beneath it. **Its op stack is its entire content.** |
| `T` | **Text** | No pixels: a string, a font reference and layout parameters, rasterised at evaluation time. Parametric, so a typo is fixable. |
| `▩` U+25A9 | **Flats** | No pixels: segmentation parameters plus recorded repairs, producing one *Fill* per enclosed region of the line art beneath it. |

**Design question.** The glyph is currently a single character in the row's leading edge and
it is doing the most important job in the panel with the least ink. Is a mono glyph enough
at 322 px, or does kind deserve colour, a tinted rail down the row's leading edge, or a
grouped/sectioned list? Show at least two answers.

### 5.2 `Mix` — a blend mode that only sometimes exists

Blend modes: Normal, Plus, Multiply, Screen, Min, Max, **Mix**.

`Mix` interpolates the two layers' pigment latents, producing a **Kubelka–Munk mix rather
than a composite**. It is meaningless unless *both this layer and the one directly beneath
it are Pigment layers*, so the blend dropdown's contents **change per layer**. This is not
a greyed-out item — the mode is absent from the menu, and the model refuses it too.

Three labelling rules the menu must carry, because a blend mode here can be honest in three
different ways:

- A **display-referred** mode reads `Screen  (display-referred)`. Linear-light modes read
  their bare label — the working space *is* linear, so labelling the majority case would be
  noise that hides the minority case.
- A mode that exists but this build cannot yet composite reads `Mix  (not composited yet)`.
- A blend name from a **newer build of the app** that this build does not know displays as
  itself (`LINEAR-BURN`) with a trailing **`(!)`** marker on the row, never normalised away
  and never silently mapped to Normal.

**Design question.** Three orthogonal annotations on menu entries, plus a `(!)` state that
must also read in a 322 px-wide row. Design the dropdown and the row marker together.

### 5.3 Media layers are wet, and wetness is time-limited

A Media layer's sub-line is where **wet state** lives: the remaining working time while the
paint is wet (`WET 4.2s`), and the *refuse-to-wet* warning, which the requirements insist
must be **visible** rather than silent. Wet state does not survive closing a document — only
the dried extent is saved.

**Design question.** A live countdown in a layer row is unusual and slightly alarming, which
is correct — it is a real deadline. Design it so it reads as information, not as an error,
and so a stack with three wet Media layers does not become a slot machine. This is also the
one number in the panel that changes every frame; the monospace fixed-cell rule is
load-bearing here.

### 5.4 Memory tracks content, not canvas

Tiles allocate only where content exists, so **a layer's real footprint is a meaningful,
highly variable number** — an empty 8K layer costs nothing. The status bar already carries
`resident / budget` for the document. Per-layer footprint in the row or the properties popup
is an option the architecture makes cheap and that most competitors cannot offer.
**Proposal, not built** — show it as an option, not a requirement.

### 5.5 Every layer has a non-destructive op stack

Each layer carries its own ordered stack of grading operations, applied *after* the layer is
projected to RGB, so **grading never bakes the pigment latents**. The row reads `2 OPS`
(`1 OP` for one); an empty stack prints nothing at all. On an Adjustment layer the stack is
the layer's entire content, so a row that said nothing about it would be describing an
empty layer.

The stack is edited under the selected layer, in the panel — not in a separate window.

### 5.6 Masks, and the fact that a mask is a selection

A per-layer mask stores **antialiased coverage**, not a bitmask, and a selection is *the same
data in a different role*, so converting either way is free. Three states are distinct and
must not be collapsed: **no mask at all**, a mask that reveals everything, and a mask that
reveals nothing. A reveal-all mask composites identically to no mask — so the `MASK` marker
in the row is the only thing that tells a user the two apart.

What the mask multiplies is **coverage**, after projection and after the op stack — never
pigment mass. (Scaling mass would be an eraser, not a mask: it would change the mixture
rather than let the backdrop through.)

### 5.7 Clipping runs clip to one base

A clipped layer is clipped by the alpha of the layer below. **A run of consecutive clipped
layers all clip to ONE base** — the nearest non-clipped layer below the run. They do not
progressively erode each other. This is the single most common clipping-mask
misunderstanding, and the panel is where it either gets communicated or does not.

The row reports what the layer **asks for**, not whether the ask can be honoured where it
sits; a clipped layer that has drifted to the bottom of the stack still shows `CLIPPED`, and
the compositor warns separately. A flag must never be the thing that makes a layer's pixels
vanish.

**Design question.** Draw the run — a bracket, an indent, a rail joining a run to its base —
in a way that survives a drag-reorder animation and a 322 px width.

### 5.8 Nothing fails silently: refusals are sentences

The whole application distinguishes **availability** from **refusal**:

- *Unavailable* means the gesture is meaningless for this selection (Move Up with the top
  layer selected; Distribute with two layers). Greyed out.
- *Refused* means the gesture was offered, attempted, and declined — a lock, a clip with no
  base, a `Mix` pair that is no longer a pair. **The refusal comes back as a sentence with
  the numbers in it, shown verbatim.**

Warnings are a second, separate channel from errors — an operation can succeed *and* warn.

The panel therefore needs a **persistent, dismissible message area with two levels**, sized
for one to three lines of real prose, e.g.:

> `Delete Layers refused: all 3 selected layer(s) are hidden by the panel filter, so this
> would have acted on layers you cannot see.`

**Design question.** This is the least glamorous and most distinctive element in the panel.
Where does it live so it is unmissable but does not shove the rows around every time it
appears?

### 5.9 Filtering that cannot act invisibly

The panel filters by **name substring** (matched against the row's *displayed* title, so
typing `3` finds the row that reads "Layer 3") and by **kind**.

The rule, which the design must not undermine:

- A filter changes **which rows are drawn** and nothing else.
- **A hidden row stays selected** — clearing the filter brings it back exactly as it was.
  Typing three characters into a search box must never be a destructive edit to a selection.
- **A command acts only on rows the user can see.** If restriction empties the set, the
  command refuses and says how many selected layers the filter is hiding.

So the filter needs a permanently visible "filter is on, N rows hidden" state — a filter you
can forget is a filter that makes commands look broken.

### 5.10 Colour labels are open, not an enum

Seven labels ship (red, orange, yellow, green, blue, violet, grey — Photoshop's seven,
because the user's existing muscle memory is the only evidence about which seven). But the
label is stored as a **name**, so a label invented by a newer build round-trips through this
one untouched. **A label with no known swatch is shown as its own text, never as a default
colour** — painting an unknown label in some fallback colour is the one behaviour that makes
two different labels look like the same one.

### 5.11 Links

Two layers are linked when they share a link-group number; membership is symmetric by
construction and cannot become one-sided. **A group with fewer than two live members is not
a link** — links are resolved, never repaired, so deleting one of a pair silently leaves the
survivor unlinked, and undo restores both the partner and the link. The row shows a partner
count, currently `LINKED+2`.

### 5.12 Layer comps live next door

A **layer comp** is a named, saved set of layer visibility, clipping, opacity and blend,
restorable in one click and stored in the document. Comps are keyed by a **stable per-layer
id**, not by index, so a comp survives the stack changing underneath it — and when it cannot
find a layer it **reports or refuses rather than guessing**. Comps mostly exist to *export*,
one file per comp.

They have their own panel; **capture** is the one comp gesture that belongs in LAYERS. The
two panels must read as related.

### 5.13 Flats do not become rows

A Flats layer's row reads its fill count (`FLATS · 153 FILLS · NORMAL`). **Its hundreds of
fills are listed in a separate Fills panel beside Layers, never as layer rows** — that is
the difference between a usable panel and an unusable one.

---

## 6. The row

### 6.1 What a row must be able to show

Currently one line tall, with everything below the title relegated to a hover tooltip. That
is a compromise, not a decision — **treat the row as an open design problem.**

Leading edge → trailing edge, current order:

`[visibility] [lock] [colour chip] [kind glyph] [name] … [link badge]`

Plus a **sub-line** in a fixed grammar, in this exact marker order — omitted parts print
nothing at all:

```
KIND · BLEND · NN% · N OPS · MASK · CLIPPED · HIDDEN · LOCKED · LABEL · LINKED+n
```

Real examples, all valid today or specified:

```
◉  Line pass              PIGMENT · MULTIPLY · 72%
□  photo plate            RGB · NORMAL · 100%
◈  Wash                   MEDIA:WATERCOLOUR · WET 4.2s
✂  Retouch (clone)        STROKES · NORMAL · 100% · MASK
▤  Adjust · Curves        ADJUSTMENT · NORMAL · 100% · 2 OPS · CLIPPED
T  Plate caption          TEXT · NORMAL · 100%
▩  Flats                  FLATS · 153 FILLS · NORMAL
□  Layer 2                RGB · LINEAR-BURN (!) · 100% · HIDDEN · LOCKED · RED · LINKED+2
```

### 6.2 Thumbnails

**There are none, and it is a real question rather than an omission.** Four of the seven
kinds hold no pixels of their own — what does an Adjustment layer's thumbnail show? A
Strokes layer re-derives from what is beneath it, so its thumbnail is a function of its
neighbours. A Pigment layer's thumbnail requires projecting latents to RGB.

**Design question.** Either design a thumbnail that tells the truth for all seven kinds, or
design a row that is confidently thumbnail-free and better for it. Do not design a row that
assumes a thumbnail and leaves the parametric kinds blank.

### 6.3 Row interactions that exist

- **Click** replaces the selection; **⌘-click** toggles one row; **⇧-click** extends from the
  primary row. The selection is never empty.
- **Double-click the name** starts an inline rename in place — the field *replaces* the
  title rather than sitting beside it. Enter commits; losing focus, Tab or Escape cancels.
- **Drag to reorder**, with a midpoint rule: dropping in a row's upper half lands the dragged
  layer above it. The drop indicator needs to be unambiguous about which side of the target
  row the layer will land on.
- Hovering the row reveals the sub-line (today's tooltip).

---

## 7. Panel chrome

### 7.1 Single-layer commands (a toolbar today)

`New RGB Layer` · `New Pigment Layer` · `New Adjustment Layer` · `Duplicate` · `Delete` ·
`Add Mask` · `Remove Mask` · `Merge Down` · `Merge Visible` · `Stamp Visible` ·
`Flatten Image` · `Rasterise Layer` · `Capture Comp`

Every one also appears in the `Layer` menu; the two are views of one command list, so
whatever the panel offers, the menu offers, with the same label and the same greying.

**Note the shape of the problem:** three *different* "new layer" commands, because kind is
chosen at creation and Pigment is the default. A single `+` button is wrong here.

### 7.2 Multi-selection commands (26 of them)

Delete · Duplicate · Move Up · Move Down · Show · Hide · Lock · Unlock · Clip · Unclip ·
Link · Unlink · seven colour labels · six **align to selection** · six **align to canvas** ·
Distribute Horizontally · Distribute Vertically.

Show/Hide and Lock/Unlock are deliberately *pairs*, not toggles: a toggle over a mixed
selection has no defensible meaning — half the rows would invert and the user would have to
look to find out what happened.

**Design question.** Twenty-six commands cannot all be buttons in a 322 px panel, and they
are currently hidden behind a collapsing "Multi-selection" header, which is where features
go to be undiscovered. Solve this — a contextual bar that appears when ≥2 rows are selected,
a segmented align widget, an overflow menu, something else.

### 7.3 Active-layer controls

Blend dropdown and opacity for the selected layer, plus a properties popup carrying name,
opacity, blend, colour label, visible, locked, clip-to-below, and the layer's op stack.

**Design question.** Blend and opacity are the two most-used controls in any layers panel
and they are currently one in the panel header and one behind a popup. Decide where they
live. Note that a per-row opacity slider does not fit 322 px alongside everything else.

---

## 8. Artboards to produce

1. **Default** — a seven-layer stack using the sample rows in §6.1, one row selected.
2. **Empty** — no document open. (The application idles at near-zero memory with nothing
   loaded; this state is common, and it is the product's "Lightweight" claim on screen.)
3. **Multi-selection** — five rows selected, with whatever §7.2 becomes.
4. **A clipping run** — three consecutive clipped layers over one base, plus one clipped
   layer with nothing below it to clip to.
5. **Filter active** — filter set to Pigment, 4 of 9 rows drawn, the "N hidden" state, and a
   selected row that the filter is hiding.
6. **Refusal** — the error banner carrying the sentence from §5.8, plus a warning banner
   below it, both dismissible.
7. **Inline rename** in progress.
8. **Drag-reorder** in progress, with the drop indicator between two rows.
9. **Adjustment layer selected**, showing its op stack in the panel.
10. **A Media layer, wet** — countdown running, plus the refuse-to-wet warning state.
11. **Row state matrix** — one row drawn in every state: hidden, locked, clipped, masked,
    labelled, linked, unknown blend `(!)`, unknown colour label, unnamed.
12. **Compressed** — the whole panel at 200 px of height, showing what collapses first.

---

## 9. Do not

- Do not design a locked **Background** row. There isn't one.
- Do not use corner radius, gradients, drop shadows, or blur. Flat fills and hard rules only.
- Do not invent a fallback swatch colour for an unrecognised label, or map an unrecognised
  blend name onto Normal. Unknown values display as themselves.
- Do not put Flats fills, or history entries, in the layer list.
- Do not hide any state that changes what is on the canvas behind a hover. Hover may
  *elaborate*; it may not be the only place a state appears.
- Do not treat kind as decoration. It is the product.
- Do not assume Photoshop's model where it conflicts with §4 and §5 — every one of those
  conflicts was resolved in the architecture's favour, deliberately.
