# UI direction

Supersedes the MacPaint chrome described in `README.md`. Source design:
[2D Drawing App Design](https://claude.ai/design/p/7e4bb4dc-cf1b-40da-905e-62b296cca588),
`Drawing App Wireframe.dc.html`.

Platform is **desktop**; 1366×1024 is a window size, not a tablet target. Input stays
`SDL_PenEvent` (pressure, tilt, barrel) with mouse fallback and keyboard modifiers. No
multi-touch — the wireframe's "hold second finger to add" becomes a modifier key.

---

## 1. Design language

Brutalist / modernist: flat fills, hard rules, no radius, no gradients outside the
colour picker, one shadow (on the canvas). This is the aesthetic Dear ImGui renders
*well* — a rounded, soft-shadowed style would have been the fight.

### Tokens

Dark chrome, light paper. The canvas is the only bright surface.

| role | value |
|---|---|
| chrome base — panels, palette, status bar | `#2d2b2b` |
| chrome deep — tool options, active tab, navigator | `#201e1d` |
| chrome mid — tab strip, internal fills | `#444141` |
| rule — 2px between major regions | `#f3f2f2` |
| divider — 1px internal | `#444141` |
| hairline, rulers | `#9b9797` |
| text primary | `#f3f2f2` |
| text secondary | `#9b9797` |
| accent — active tool, dirty marker, selection | `#ff563c` |
| row selected | `#7c1405` |
| canvas paper | `#f8f4f4` |
| on-accent foreground | `#201e1d` |

Rules: **2px `#f3f2f2`** between major regions, **1px `#444141`** internally. Type is
Archivo (400 / 600 / 800) with `ui-monospace` for all numerics and caps labels;
800-weight caps carry `.10–.14em` tracking.

The accent brightened from `#ec3013` in the light revision to `#ff563c` here — correct
practice, since the same hue needs more luminance to hold up against dark chrome.

> ⚠️ **The canvas surround is the one thing to push back on.** It is `#2d2b2b`, near
> black. Simultaneous contrast makes paint read lighter and more saturated against a
> near-black surround than it truly is, which is precisely the judgement a painting
> application must not distort. A mid-grey (`#bab6b6`, as the light revision used) is the
> neutral you actually want to judge colour against.
>
> Keep the dark chrome — but **make the surround a separate, user-adjustable value**
> defaulting to mid-grey, not to the chrome colour. Photoshop separates these two for the
> same reason. This makes PRD **L6** more important, not less.

---

## 2. Layout

```
┌────────────────────────────────────────────────────────────┐
│ naturalPaint │ File Edit … Help        undo redo ⟲ panels  │ 36
├────────────────────────────────────────────────────────────┤
│ ▨ study-plate-04.npaint ●│ retouch-ref.tif 64% │ … │ +  ⫿⫿ ⊞ │ 34
├────────────────────────────────────────────────────────────┤
│ ■BRUSH│ PRESET ▣ Round Bristle 03 │ SIZE ── 48px │ HARD …  │ 46
├──────┬──────────────────────────────────────┬──────────────┤
│ tool │                                      │ COLOR        │
│ 2×n  │            canvas + rulers           │ BRUSH SET.   │
│ grid │                        ┌───────────┐ │ LAYERS       │
│      │                        │ NAVIGATOR │ │ CHANNELS     │
│ FG/BG│                        └───────────┘ │              │
├──────┴──────────────────────────────────────┴──────────────┤
│ 64% │ 2048×1536 · LIN16 │ 214 MB / 512 MB │ Clone source…  │ 26
└────────────────────────────────────────────────────────────┘
  104                                              322
```

Tool cells are 50px in a 2-wide grid — generous desktop targets, and the palette
scrolls, so the tool count is not layout-constrained.

Every cell shows its **shortcut letter** on hover and in its tooltip, and the palette
**switches to the flatting set when a Flats layer is active** — those tools are scoped to
that layer kind rather than holding global keys. Default keymap and its reasoning:
[shortcuts.md](shortcuts.md).

---

## 3. Reconciliations

The wireframe made four commitments that conflicted with settled architecture. Each is
resolved in the architecture's favour, because each conflict was the design assuming
Photoshop's model.

### 3.1 Bit depth and working space

The wireframe reads `RGB · 8 BIT` and `2048 × 1536 · RGB/8`. PRD B1/B6 make linear
`rgba16float` working space and end-to-end bit depth **P0**.

**Resolution.** The status bar reports the *working space*, not a legacy mode:
`2048 × 1536 · LIN16` (or `LIN32`). The Channels panel header shows the same. A
document's *source* encoding belongs in document info, not in the persistent chrome —
an 8-bit source file becomes a 16-bit linear document on import, and the chrome should
say what the document *is*.

### 3.2 Layer kind is invisible

The wireframe's rows cannot distinguish a Pigment layer from an RGB layer, and nothing
creates a Media layer. Pigment is the *default* kind, so this hid the product's entire
differentiator.

**Resolution.** A kind glyph left of the thumbnail, and the kind leads the existing
monospace sub-line — which already reads `MULTIPLY · 72%`, so it absorbs this with no
new vocabulary.

```
◉ [▨] Line pass            PIGMENT · MULTIPLY · 72%
□ [▨] photo plate          RGB · NORMAL · 100%
◈ [▨] Wash                 MEDIA:WATERCOLOUR · WET 4.2s
✂ [▨] Retouch (clone)      STROKES · NORMAL · MASK
▤ [ ] Adjust · Curves      ADJUSTMENT · CLIPPED
T  [▨] Plate caption        TEXT · NORMAL · 100%
```

| glyph | kind |
|---|---|
| `◉` | Pigment — the default |
| `□` | RGB |
| `◈` | Media (suffixed with the medium) |
| `✂` | Strokes |
| `▤` | Adjustment |
| `T` | Text |
| `▩` | Flats — suffixed with the fill count |

> ✅ **`CLIPPED` became real at PLAN.md Phase 5 step 9 (PRD C9).** The `ADJUSTMENT ·
> CLIPPED` row above predated the feature by four steps; `app::layerRowSubLine()` now
> emits the marker, after `MASK` and before `HIDDEN`, so a full row reads
> `ADJUSTMENT · NORMAL · 100% · 2 OPS · CLIPPED`. It reports what the layer *asks for*
> — that function takes a `Layer` and no `Document`, exactly as the `(!)` blend marker
> does, and "is there anything below to clip to" is a question only a stack can answer.
> A clipped layer with no base is reported by the compositor instead, by name, at every
> boundary that writes a file.

A **Flats** row reads `FLATS · 153 FILLS · NORMAL`, and its fills are listed in a
**Fills panel** beside Layers rather than as layer rows — hundreds of them would make the
layer panel useless. See
[docs/autoflats-migration.md §4](autoflats-migration.md).

The Media sub-line is also where **wet state** lives — remaining working time while
wet, and the refuse-to-wet warning PRD H5 requires be *visible*.

### 3.3 The colour picker cannot express pigment

The wireframe's COLOR panel is HSV + hex + RGB. But pigment selection drives *physical*
constants — density, staining, granulation — so Ultramarine and Phthalo Blue behave
differently at the same RGB (`README.md`, Controls).

**Resolution.** COLOR gains a mode toggle in its header:

- **RGB** — the wireframe's picker as drawn. What you get on an RGB layer.
- **PIGMENT** — a pigment well (the `Palette` set) plus a mixing tray. Selecting a
  pigment sets colour *and* its physical constants. The RGB readout stays visible as
  the resulting colour, read-only.

On a Pigment or Media layer the panel defaults to PIGMENT mode. Choosing a raw RGB
colour there is still allowed — it maps through RGB→latent, with the caveat from §3 of
the design doc that the decomposition is plausible rather than true.

### 3.4 Missing blend mode and medium controls

**Resolution.** `Mix` joins the layer blend dropdown, shown only when both the layer
and the one beneath it are Pigment layers — it is meaningless otherwise. Medium
(watercolour / oil / ink) is chosen at Media-layer creation and editable in layer
properties; it is not a global mode, because it is a per-layer property now.

---

## 4. Scope the wireframe added

The palette carries 26 tools against the ~12 the plan covered. **Accepted as real
scope**, which changes the PRD's non-goals.

| added | disposition |
|---|---|
| CROP | PRD **D17**, phase 6. |
| DODGE, BURN | PRD **D13**, phase 10 — as a brush painting an adjustment mask, not a pixel op. |
| GRAD | PRD **D24**, phase 6 — linear/radial/angular with an editor and presets. |
| FILL | PRD **D25, D26**, phase 6 — the paint bucket with tolerance is distinct from Fill-with-colour. |
| **PEN, CURVE, + PATHS tab** | New subsystem. Phase 13. |
| **TEXT** | Was a documented non-goal. Now phase 14. |
| MEASURE | **Dropped.** The pixel probe and the rulers cover what it was for. |
| SLICE | Web-export slicing. Dropped — no plausible use in visdev or texture work, and it is the one tool here with no constituency. |

> **This table used to say "Fold into existing phases" for six tools, and four of them then
> never became requirements.** That is how GRAD, FILL and MEASURE went missing for a
> revision. Accepting scope in a UI document is not the same as specifying it — every row
> here now names its requirement or says it was dropped.

The palette also needs two tools the wireframe did not draw: the **eraser** (PRD F9,
[ADR-0007](adr/0007-erase-is-mass-reduction-not-a-colour.md)) and the **eyedropper**
(PRD Q10).

### Paths compose better than expected

A Bézier path is another curve feeding the dab emitter built in phase 1. **Stroke path
with brush** is therefore nearly free once both exist — same arc-length walk, different
curve source. And path → selection is how professionals make precise selections, so it
strengthens phase 7 rather than sitting beside it.

The hard, optional half is the reverse direction — selection → path needs contour
extraction plus curve fitting. Defer it.

### Text is a parametric layer, not a rasteriser

Text fits the existing op-class taxonomy: a **Text layer** stores the string, font
reference and layout parameters, and rasterises at evaluation time. That makes it
parametric and non-destructive by construction, the same shape as an Adjustment layer —
so a typo is fixable and re-layout is free.

> ⚠️ **Use CoreText for tier 1, not a hand-rolled engine.** Shaping, bidirectional
> text, cluster breaking and font fallback are where hand-written text engines die, and
> the OS does all four correctly for free. macOS-only is acceptable — `Context.cpp`
> already stubs Windows and Linux. HarfBuzz + FreeType behind the same interface is the
> portability swap if it is ever needed.

Explicitly still out: text on a path, vertical text, rich-text runs. Those are the
multi-year part of a text engine, and none of them serve annotation.

---

## 5. Implementation notes

- **`src/ui/MacPaintUI.*` and `Theme.*` are replaced**, not extended. The new module is
  `src/ui/Atelier*`. Retain the pan/zoom viewport logic — that part is reusable.
- Dear ImGui's docking branch is already a dependency and already fits: the right stack
  is a docked column of collapsing headers, the tool palette a fixed child window.
- Every numeric in the chrome is monospace and right-aligned in a fixed-width cell.
  With live values this is what stops the layout juddering as numbers change.
- The `columns-2` and `layout-grid` icons in the tab strip are the two-tab split from
  ADR-0001's amendment. Wire them to that, not to a floating-window manager.
- The status bar's `Doc 18.4 MB / 42.1 MB` becomes **resident / budget**. Surfacing the
  real numbers there makes the lightweight claim continuously visible instead of a
  thing only `--selftest` knows. The clipboard's cost is part of that figure — PRD **M5**.
- **Mirror view and grayscale preview need visible state.** All three toggles — mirror
  L/R, mirror U/D, grayscale — change what the canvas shows without changing the document,
  so each active one must be indicated in the status bar. A user who forgets grayscale is on
  will mix colour blind; a user who forgets a mirror is on will sign their work backwards.
  With two mirror axes this matters more, not less: both on looks like a deliberate
  composition, not like two toggles nobody cleared.
- **The History panel** (PRD O2) joins the right-hand docked column. It lists entries by
  originating tool, and clicking one moves the history cursor — which for Media layers is a
  single replay from the nearest keyframe, not N replays.

## 6. Naming

**Decided: the project keeps the name naturalPaint.** The wireframe's "ATELIER 2D"
wordmark and `.atl` extension are not adopted — substitute the naturalPaint wordmark in
the menu bar, and documents are **`.npaint`**
(see [document-format.md](document-format.md)),
not a private extension. The tab strip's mixed `.atl` / `.tif` filenames become `.npaint`
and whatever else was imported.
