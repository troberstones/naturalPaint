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
| rule — 2px between major regions | `#201e1d` |
| divider — 1px internal | `#444141` |
| hairline, rulers | `#9b9797` |
| text primary | `#f3f2f2` |
| text secondary | `#9b9797` |
| accent — active tool, dirty marker, selection | `#ff563c` |
| row selected | `#7c1405` |
| canvas paper | `#f8f4f4` |
| on-accent foreground | `#201e1d` |

Rules: **2px `#201e1d`** between major regions, **1px `#444141`** internally. Type is
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

> ⚠️ **The palette went from a wireframe to a supplied design mid-project, and this
> section now describes that design, not the wireframe.** The wireframe's own palette was
> "50px [cells] in a 2-wide grid"; the supplied design (sidequest/lucide-toolbox) redraws
> it as a **single column** of ~26/27 tools, grouped by thin rules, with the selected tool
> drawn on the accent colour and Lucide glyphs at 15px, one per tool. The diagram and the
> prose below are updated to match, and have now been updated a second time for a direct
> user correction: *"make the toolbar fit without scrolling so we don't have a scroll bar.
> the buttons are too large."* ui/AtelierLayout.hpp's `kToolPaletteW`/`kToolCellMax`/
> `kToolCellMin`/`atelierToolCellSize()` carry the current arithmetic -- a **44px palette**
> holding a **cell size computed fresh every frame** from the window's actual height,
> shrinking from 28px down to a 18px floor before the column ever reaches for a scrollbar,
> and reaching for one (silently, wheel-only, never drawn) only in the one case shrinking
> cannot rescue: a window shorter than roughly 670px. `--selftest`'s atelier-chrome section
> asserts all of this against a *live* `ImGui::GetContentRegionAvail()`, not only against a
> hand-derived formula for what one should be, and against a table of representative window
> heights for the shrink-to-fit arithmetic itself.
>
> This is the **third** revision of this number. The first,
> `kToolPaletteW = kToolCellSize + 8` (a fixed 36px cell), was a cell plus *half* of one
> side's `WindowPadding` and none of a scrollbar's width -- every icon rendered clipped in
> half, found by a screenshot rather than by that revision's own (vacuous) `static_assert`.
> The fix for *that* bug was a fixed 36px cell in a **64px** palette (room for the cell,
> both sides' padding, and a then-permanent scrollbar) -- correct, but it satisfied "every
> cell fits inside the palette" while failing the actual design brief, which is a single
> column that shows all of itself without scrolling. The second revision replaced the fixed
> cell with the shrink-to-fit one described above, which let the palette narrow to 44px and
> drop the scrollbar entirely -- but with 28 cells still to fit, most windows still landed
> on a 20-26px cell, which is what prompted the user's *next* instruction: **"nest similar
> tools into a flyout to conserve space like photoshop."** ui/AtelierChrome.hpp's
> `kToolGroups` collapses the 27 `Tool` values plus the "More" cell from 28 palette slots to
> 18 -- one cell per Photoshop-style group, each showing whichever member was last used,
> with a small corner triangle marking a group of more than one and a flyout listing the
> rest -- and the room that frees up is spent raising `kToolCellMax` back to 36, this file's
> very first revision's number, rather than shrinking further than it has to. The palette is
> **52px** now. See ui/AtelierLayout.hpp's `kToolPaletteW`/`kToolCellMax`/`kToolCellMin` for
> the full arithmetic, ui/AtelierChrome.hpp's `kToolGroups` for the grouping table, and
> ui/AtelierTheme.hpp for the shared constants that make the width arithmetic hold.

> ⚠️ **Every band in the diagram below that holds controls is a DOCK now, not a welded
> band**, and what the diagram draws is the *default arrangement* of a system rather than a
> fixed layout. Three things a first run now shows that the diagram does not: each panel
> carries a **grip** -- a 26px bar, or a 14px rail on a short slot -- that collapses it,
> moves it, and tears it off; the right dock starts with COLOR and LAYERS open and the rest
> as titled grips; and a **flyout rail** sits at the canvas's right edge holding the seven
> View/Simulation panels. All three are under golden coverage
> (`toolbar`/`tools`/`flyout`/`layers`/`rail`) and asserted headlessly in `--selftest`'s
> panel-layout section. §2c has the measurement behind each, and covers the tab stacks and
> the splitter rule that arrived with them. The user's instruction:
> *"revamp the right panel to be dockable, not scrollable, I want to be able to put the
> parts I want in and have them stay put, and put others in flyout mode or dock around the
> app"*, and *"All four edges but move the brush setting and the tool pallet to dockable
> panels as well, this makes the UI modular and customizable."* See §2c below.

> ⚠️ **The title bar and the tab strip are ONE band now, not two**, per the user's own
> words: *"The tab bar and the space where the menus used to live take up too much space,
> collapse them into one band."* Two things changed from the diagram below, and neither is
> new information the diagram used to carry correctly: first, `File Edit … Help` has not
> lived in this row for a while -- `ui/MacNativeMenu.mm` moved the menus into the real
> macOS menu bar outside this window, and the diagram kept drawing them anyway, so the row
> the user was actually looking at was already down to the wordmark, PANELS, Undo, Redo and
> an fps readout, with an open gap where the menu labels used to sit. Second, the document
> tabs -- `▨ study-plate ●│ ref.tif …` below -- now fill exactly that gap instead of owning
> a 34px band of their own underneath it. The merged row is still **36px**, the taller of
> the two originals, because that is what the wordmark's controls and a tab's close box both
> already needed; what disappears is the *second* 34px band and its 2px rule, not any
> content. `ui/AtelierLayout.hpp`'s `kTitleWordmarkW` (100px) and `kTitleControlsW` (230px)
> reserve this row's two ends for the wordmark and for Undo/Redo/PANELS/fps; the document
> tabs, the `+`, and the two split icons fill whatever the row leaves between them --
> `AtelierBands::tabStrip`, nested inside `AtelierBands::titleBar` rather than a band beside
> it. Opening a document now costs the canvas nothing at all, where it used to cost 34px
> plus a rule; `--selftest`'s atelier-chrome section asserts exactly that. The `titlebar`
> and `toolbar` golden views moved with it -- see `tools/golden/run_golden.sh`'s own account
> of the crop-coordinate shift.

```
┌────────────────────────────────────────────────────────────┐
│ naturalPaint │▨ study-plate ● │ ref.tif … │ +⫿⫿⊞ panels undo redo │ 36
├────────────────────────────────────────────────────────────┤
│ ■BRUSH│ PRESET ▣ Round Bristle 03 │ SIZE ── 48px │ HARD …  │ 46
├───┬──────────────────────────────────────────────┬──────────┤
│ ▤ │                                              │ COLOR    │
│ 1 │                                              │ BRUSH SET│
│ col│           canvas + rulers                   │ LAYERS   │
│   │                        ┌───────────┐         │ CHANNELS │
│   │                        │ NAVIGATOR │         │          │
│FG │                        └───────────┘         │          │
├───┴──────────────────────────────────────────────┴──────────┤
│ 64% │ 2048×1536 · LIN16 │ 214 MB / 512 MB │ Clone source…  │ 26
└────────────────────────────────────────────────────────────┘
  52                                                    322
```

Tool cells are **15px Lucide glyphs in a single column of nested flyout groups** —
Photoshop's own "one cell per tool family" convention, and close to Photoshop's own
single-column tool rail. **One cell per group, not per tool**: `ui/AtelierChrome.hpp`'s
`kToolGroups` maps this build's 27 `Tool` values onto 17 groups (derived from Photoshop's
own real tool groupings, not arbitrary — Move+Artboard, Lasso+Polygonal Lasso,
Crop+Slice, Eyedropper+Ruler, Gradient+Paint Bucket, Brush+Pencil (+this build's own Water
and Dry Brush variants), Dodge+Burn, Pen+Curvature Pen, and nine groups of one member —
see that table for the exact membership and display order), plus the "…" overflow cell:
18 palette cells in all. A cell shows whichever member of its group was last used, marked
with a small corner triangle when the group has more than one member; right-click or a
~350ms press-and-hold opens a flyout listing the rest, icon and name, the same disabled
treatment (dimmed, "Not built yet.") as the palette itself for a member that is not
implemented. Picking a member from the flyout makes it the group's displayed tool and, if
it is implemented, the active one.

The cell size is **not fixed**: `ui/AtelierLayout.hpp`'s `atelierToolCellSize()` computes
it fresh every frame from the palette band's live height, as large as 36px in a roomy
window and shrinking (never below an 18px floor, which still leaves a 15px glyph ~1.5px of
margin per side) as the window gets shorter, so that all 18 cells plus the FG swatch fit
**without a scrollbar** at essentially any window height a user is likely to run at.
`ui/MacPaintUI.cpp`'s palette draws the tool grid in a child window with
`ImGuiWindowFlags_NoScrollbar` and keeps the FG swatch pinned below it, outside that
child, at a size derived from the *maximum* cell size (36px) rather than the live one —
the swatch does not resize as the window resizes.

**The honest limit.** Below roughly a 540px window (18 cells at the 18px floor, plus the
separator rules and the swatch strip, worked back through the layout's band arithmetic —
well below the 28-cell design's ~670px, since nesting nearly halved the cell count), the
column genuinely cannot fit even at its smallest legible cell size. This build does not
clip the grid or hide any tool in that case: the child keeps `ImGuiWindowFlags_NoScrollbar`
(no bar drawn, no width reserved for one) but Dear ImGui's mouse wheel still scrolls inside
a `NoScrollbar` child, so every cell stays reachable — just not all visible at once — on a
window shorter than the design was built to fit.

The palette is drawn in **five groups, separated by thin rules**, top to bottom: selection
& sampling; retouch & fill; paint; vector & text; navigation — the same five the flat,
28-cell layout used, just carried over onto the 17 flyout-group slots instead of onto
individual tools (see `ui/AtelierChrome.hpp`'s `kToolGroups`, whose `ruleAfter` flag marks
the same four boundaries on different slots). Only **seven of the ~27 tools do
anything** — see §4a below for the full list and why the rest are drawn disabled, in the
palette or in a flyout, rather than omitted.

Every cell shows its **shortcut letter** on hover and in its tooltip when
[shortcuts.md](shortcuts.md) section 1 reserves one for it — this is the letter that
tooltip shows, not a claim the key is wired to a tool switch yet; `keymaps/default.json`
does not bind any tool-select key today, and wiring that is separate, later work. The
palette also **switches to the flatting set when a Flats layer is active** — those tools
are scoped to that layer kind rather than holding global keys.

### 2a. Icons: Lucide, one per tool, 15px

"Toolbox uses Lucide icons at 15px, one per tool; if a tool has no matching Lucide glyph,
use the closest and list the substitutions" was the design's own spec line. The icon font
is vendored at `third_party/lucide/` (ISC-licensed) and merged into the UI's font atlas by
`ui/Fonts.cpp`'s `installToolIconFont()` — a second, independent merge from the one that
already exists for the layer-kind glyphs (`installUiFonts()`), because the icon size is
fixed at 15px regardless of the UI's own text size, which is the spec's own line and not a
preference.

Every name below was checked against `third_party/lucide/codepoints.json` — not just at
authoring time but at `--selftest` time too (`app/selftest/AtelierChrome.cpp`'s "Part F"
reads the vendored JSON directly and compares every codepoint this table claims against
what the file actually says).

| tool | Lucide icon | match | substitution reason |
|---|---|---|---|
| Move | `move` | exact | — |
| Rectangle Marquee | `square-dashed` | close | Lucide has no dedicated marquee icon; a dashed square is the standard visual convention for a rectangular-selection tool. |
| Lasso | `lasso` | exact | — |
| Polygon Lasso | `pentagon` | substitution | no polygon-lasso icon exists; a pentagon is the clearest available stand-in for "select along straight polygon edges." |
| Magic Wand | `wand-sparkles` | substitution | no magic-wand icon exists; the sparkle wand is Lucide's closest "magic selection" glyph. |
| Crop | `crop` | exact | — |
| Eyedropper | `pipette` | exact | Lucide's own eyedropper/colour-sampler glyph is literally named `pipette`. |
| Measure | `ruler` | close | Photoshop itself calls this the Ruler tool; no dedicated "measure" icon exists or is needed. |
| Frame / Artboard | `frame` | exact | — |
| Clone Stamp | `stamp` | substitution | no clone-stamp icon exists; a rubber stamp is the closest available glyph for "copies a source elsewhere." |
| Eraser | `eraser` | exact | — |
| Paint Bucket | `paint-bucket` | exact | — |
| Gradient | `blend` | substitution | no gradient icon exists in Lucide; `blend` is its closest existing glyph for a gradual colour transition. |
| Brush | `brush` | exact | — |
| Water | `droplet` | substitution | not in the wireframe's ~26 at all (see §4a); a single water drop is the clearest stand-in for "pre-wet, no pigment." |
| Dry Brush | `paintbrush-2` | substitution | not in the wireframe's ~26 either; the alternate paintbrush glyph distinguishes it from Brush without inventing a "dryness" motif Lucide does not have. |
| Pencil | `pencil` | exact | — |
| Smudge | `droplets` | substitution | no smudge/smear/blur icon exists in Lucide; multiple droplets is the closest available metaphor for wet blending. |
| Dodge | `sun` | substitution | no dodge icon exists; sun is Photoshop's own metaphor for lightening exposure. |
| Burn | `moon` | substitution | paired with Dodge/sun; moon/night is the closest available darkening metaphor. |
| Pen | `pen-tool` | exact | — |
| Curve | `spline` | close | `spline` is the vector-curve-authoring concept Curve stands for. |
| Text | `type` | exact | — |
| Shape | `shapes` | exact | — |
| Slice | `slice` | exact | — |
| Hand | `hand` | exact | — |
| Zoom | `zoom-in` | close | Lucide has `zoom-in`/`zoom-out` but no neutral "zoom" glyph; `zoom-in` matches the tool's own default cursor. |
| More ("…") | `ellipsis` | exact | Not a `Tool` — the overflow cell at the foot of the palette; see §4a. |

15 of these 28 are exact Lucide matches; 4 are close-enough renamings that need no
substitution note (Rectangle Marquee, Measure, Curve, Zoom); 9 are genuine substitutions
with no matching Lucide concept at all (Polygon Lasso, Magic Wand, Clone Stamp, Gradient,
Water, Dry Brush, Smudge, Dodge, Burn). Sun/moon for Dodge/Burn is Photoshop's own
lighten/darken metaphor, not an invented one.

### 2b. Flyout groups: what each cell shows by default

Every icon above is still each *tool's* own — nesting did not change what glyph a tool
draws, only how many cells the palette needs to hold them all. What changed is which icon
a group's single palette cell shows before anyone touches its flyout: **the group's first
`toolImplemented()` member, or its first member if none of them are implemented yet**
(`ui/AtelierChrome.hpp`'s `toolGroupDefaultMember()`, checked by `--selftest` against an
independent re-derivation, not by asking that function to agree with itself). Photoshop's
own rule — a group opens on whichever tool actually does something — which is also why
every one of this build's seven working tools already happens to be group member 1 in the
table below, not a coincidence: Water and Dry Brush were placed beside Brush for exactly
this reason (§4a), and the rest of the table follows the same instinct.

| slot | group members (display order) | shown by default |
|---|---|---|
| 1 | Move, Frame | Move |
| 2 | Marquee | Marquee — implemented |
| 3 | Lasso, Polygon Lasso | Lasso |
| 4 | Magic Wand | Magic Wand |
| 5 | Crop, Slice | Crop |
| 6 | Eyedropper, Measure | Eyedropper — implemented |
| 7 | Clone Stamp | Clone Stamp |
| 8 | Eraser | Eraser |
| 9 | Gradient, Paint Bucket | Gradient |
| 10 | Brush, Pencil, Water, Dry Brush | Brush — implemented |
| 11 | Smudge | Smudge |
| 12 | Dodge, Burn | Dodge |
| 13 | Pen, Curve | Pen |
| 14 | Text | Text |
| 15 | Shape | Shape |
| 16 | Hand | Hand — implemented |
| 17 | Zoom | Zoom — implemented |

Then the "…" More cell, unchanged — 17 groups + 1 = 18 palette cells, down from 28.

Groups of one member today (Magic Wand, Clone Stamp, Eraser, Smudge, Text, Shape, Hand,
Zoom) still get their own slot rather than folding into a neighbour — per the user's own
instruction, "keep the pairings even where a group currently has one member," because
these are exactly where this build's not-yet-built variants will land as later phases add
them (a second selection-brush tool beside Magic Wand, for instance), without another
palette rebuild to make room.

---

### 2c. Docks: four edges, flyouts, and why the dock never scrolls

The chrome had two welded control bands (the 52px tool palette on the left edge, the 46px
options bar under the tab strip) and one scrolling right-hand column of thirteen
`CollapsingHeader`s. All three are gone. What replaced them is **fifteen panels, each of
which can be in any of four docks, on a flyout rail, or put away**, with the arrangement
persisted across relaunches.

**The parts.** `app/PanelLayout` is the model -- placement, order within that placement,
size weight, collapsed, plus the four dock extents -- and it is headless and
`--selftest`-asserted. `ui/DockLayout` is the slot arithmetic: given a dock rect and its
panels, where each one goes. Also headless, also asserted, for the reason
`ui/AtelierLayout.hpp` already gives about band geometry: *"a test that needs a window, a
device and a font atlas to check that four bands tile a rectangle is a test nobody runs."*
`ui/MacPaintUI.cpp` is the only file that knows what a splitter looks like.

**"Not scrollable" is a property of the dock, not of the panel.** The dock never scrolls;
its panels divide it, and the slots plus the splitters between them tile it exactly. A
panel whose content exceeds its own slot scrolls *inside that slot*. That is the whole
difference between "LAYERS is below the fold because HISTOGRAM above it is long" (the
complaint this revamp answers) and "LAYERS is exactly where I put it and has its own
scrollbar" (what it does now).

**Sizing.** Each panel carries a weight; expanded panels share what the collapsed ones
leave, floored at 72px of height (or 200px of width in a top/bottom dock). Drag the
splitter between two panels and only those two weights change, so every other panel keeps
the exact size it had -- which is what makes *"have them stay put"* true. Drag a dock's
outer edge to resize the whole dock. Both persist.

**A collapsed panel is ballast, not a wall.** A splitter drag reaches *past* the collapsed
panels beside it to the nearest expanded one on each side; the fixed-size panels in between
simply translate. Only a boundary with no expanded panel at all on one side is inert.

This started as the opposite rule -- refuse to drag a boundary next to a collapsed panel,
on the reasoning that a handle which does nothing is worse than no handle -- and that rule
shipped. **In the default arrangement it disabled every splitter in the dock**, because the
right dock alternates (COLOR expanded, two collapsed, LAYERS expanded, two collapsed) and
not one of its five boundaries had an expanded panel on both sides. The user's report was
the direct consequence: *"I found that the colorpanel was too big and I couldn't resize
it."* `--selftest` now asserts the old rule's count (zero live) beside the new one's
(three), so the regression cannot come back quietly.

**Tab stacks.** Several panels can share one slot, with a tab strip across the top
selecting between them -- *"tab support for putting multiple panels into a stack."* Drag a
panel onto the middle of another and they stack; drag onto a slot's leading or trailing
edge and it is inserted beside it instead. The drop preview says which: a wash over the
whole slot for a stack, a solid 4px seam for an insert. Right-click a tab to unstack it,
or drag it out.

The model is an opaque `stack` id on each panel plus an `active` flag, both persisted --
not a "tabbed with the previous entry" flag, because that would make the grouping a
property of the *order*, and the order is exactly what a person reshuffles. Two invariants
are repaired rather than trusted, on every mutation and every parse: **a stack has at least
two members in one placement** (a stack of one is a panel, and a lone tab strip is chrome
with nothing to select) and **exactly one member is active**. A hand-edited file cannot
produce a slot the UI cannot draw.

Three consequences worth stating because each is a decision:

- **The slot's geometry is its FIRST member's**, not its active tab's -- so switching tabs
  cannot resize the slot.
- **A stack collapses as a unit**, and a collapsed stack still shows its tabs. Hiding them
  would leave the other members with no way back, which is the same failure the grip rule
  below was written to end.
- **Tabs exist only where a slot does.** Stacking onto a flyout or a hidden panel is
  refused rather than half-done: a flyout draws one panel and a hidden one draws none.

**The honest limit**, stated the way the tool palette's is: when the minima plus grips
plus splitters exceed the dock, no distribution of weight helps. `DockTiling::overflowed`
says so, every panel sits at exactly its floor, and *that one case* lets the dock scroll --
the behaviour this feature exists to remove, reached only when the alternative is panels
too small to use.

**The default arrangement is a budget, not a preference.** This is the part the first
revision got wrong, and the correction is worth recording because the reasoning that
produced the error was superficially sound. That revision put all thirteen non-band
sections in the right dock and started open exactly the ones `app/ControlsLayout`'s
`defaultOpen` already marked open -- *"a section not worth being open in a scrolling column
is not worth a slot in a dock either."* But a scrolling column has an unbounded budget: an
open section there costs the ones below it nothing, because they move down. **A dock's
budget is its height**, and thirteen panels spent 286px of a ~650px dock on grips alone.
Four expanded panels then divided 293px, giving LAYERS a 57px body -- its document line,
its filter field, and then the bottom of the dock. *A first run's layer panel showed no
layers.* So the default now reads: the `View` and `Simulation` sections start on the flyout
rail rather than as grips in the dock (their own role descriptions call them occasional);
of the six left, COLOR and LAYERS start open; and LAYERS starts at twice the default weight
because it is the only panel in the dock whose content is a *list* -- every other one is a
fixed-height form, as useful at its floor as at twice it.

`--selftest` now asserts this as arithmetic -- LAYERS gets room for three layer rows at the
reference window size, and no expanded panel sits pinned at its floor. The assertion it
replaced was `!overflowed`, which is far too weak: a dock in which every panel has been
squeezed to exactly 72px does not overflow either, and that is precisely the state that
shipped.

**Every panel always has a grip.** What varies is its shape, never its existence: a 26px
**bar** across the top of the slot in the ordinary case, or a 14px **rail** down its left
edge when the slot is too short to spare 26px of height (the 46px top dock holding
OPTIONS). A slot too narrow for its title drops the word rather than clipping it
(`app/ControlsLayout.hpp` §2's rule) and shows grip dots instead, so the strip still reads
as grabbable. Click it to collapse, drag it to move the panel, right-click it for the
placement menu.

The rule this replaced was the opposite one -- *a header that cannot do its job is not
drawn* -- and it is worth naming what that cost, because it looked like the same
anti-clipping judgement `app/ControlsLayout.hpp` §2 makes about labels. The height floor it
tested against excluded the very grip it had to contain, so on an ordinary window **every
panel in the application lost its header at once**: nine anonymous grey bars that could not
be reopened and four panels that could not be moved. A panel a person cannot get hold of is
a panel they have lost, which is not a state this chrome is allowed to reach, whatever the
room.

**The tool palette flows.** `atelierToolGrid()` picks the cell size and the column count
together from both of the panel's dimensions, so TOOLS docked to the top edge is a row of
18 cells rather than a column that does not fit. A tall narrow panel resolves to one
column at exactly the cell size `atelierToolCellSize()` picks, which is asserted -- the
generalisation did not move the default.

**Flyouts, and the three ways back in.** A panel in flyout mode sits on a 34px rail at the
canvas's right edge and opens over the canvas when clicked -- and since the budget
correction above, seven panels start there, so the rail is populated on a first run rather
than being a mode nothing uses. Each cell shows the **shortest prefix of the panel's title
that no other panel on the rail shares** (`controlsSectionShortLabel`), so GRADE and GRID
read `GRA` and `GRI` while a panel with no near neighbour keeps two characters; the
tooltip carries the full title. A flat two-character label shipped first and collided on
exactly that pair.

The **PANELS** control in the title bar -- the one docs/ui.md has drawn since the first
revision, and which this file previously recorded as *"not built: there is no panel manager
to wire it to, and a button that does nothing is worse than a gap"* -- lists every panel
with its placement and moves it anywhere. It lives outside every dock on purpose: that is
what makes hiding every panel a legal state rather than a trap. Right-clicking any panel's
grip offers the same menu, and **dragging that grip tears the panel off**: `dockDropTargetAt`
resolves the pointer to a dock edge or, in the middle of the canvas, to the rail. Corners
resolve by which edge is nearer *in proportion*, so a 2000x800 window behaves like a square
one.

**Persistence.** `panel-layout.txt`, version 3, in the same file the single-column build
wrote. **Version 2 and version 1 files still read.** A version 2 `panel` line is recognised
by its field count -- five rather than seven -- and lands unstacked and active, which is
exactly what it meant when nothing could be stacked; six or eight fields is malformed and
skipped, because half of a `<stack> <active>` pair says nothing about the other half.
Version 1's `section <key> 1` means the right dock, `section <key> 0` means hidden, and the
two panels version 1 could not name arrive at their default placements. That compatibility is not incidental -- this revision inserted two enumerators
at the *front* of `ControlsSection`, which under ordinal-keyed persistence would have
silently turned every user's LAYERS panel into their SOLVER panel. `app/PanelLayout.hpp`
carries the full format and the round-trip repair rules.

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
| `◈` | Media (the medium name is not built; see the callout below) |
| `✂` | Strokes |
| `▤` | Adjustment |
| `T` | Text |
| `▩` | Flats — the fill count is not built; see the callout below |

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

> ⚠️ **Two things in this section have never been built, and the panel does not
> pretend otherwise.** The Media sub-line carries no wet state and no drying
> countdown, and a Flats row carries no fill count.
>
> Neither is a presentation gap — the model cannot supply either number.
> `core::Layer` has no medium name and no wet state; the wetness that exists is
> `sim::PaintSim`'s single canvas-wide field with no layer awareness at all, and
> nothing anywhere computes seconds-until-dry. `core/Merge.cpp` says the other
> half outright: a Flats layer has no regions, so there is no fill list to count.
>
> So a Media row reads `MEDIA · NORMAL · 100%` and a Flats row reads
> `FLATS · NORMAL · 100%`. Recorded here rather than quietly dropped, because
> this file is the design's own statement of intent and a reader comparing it
> against the running panel deserves to know which of the two is behind.

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
| MEASURE | **Un-dropped** (sidequest/lucide-toolbox). This row used to say "**Dropped.** The pixel probe and the rulers cover what it was for." The supplied palette design draws Measure as its own cell regardless of that judgement, and the user's own words on reversing it: **"the palette keeps them for now, and we'll prune the unneeded tools in the future as the capabilities settle in."** No PRD id assigned yet — drawn disabled (§4a) until one is. |
| SLICE | **Un-dropped** (sidequest/lucide-toolbox), same reversal and the same words as MEASURE above. This row used to say "Web-export slicing. Dropped — no plausible use in visdev or texture work, and it is the one tool here with no constituency." That judgement about its usefulness is not retracted, only the disposition is: the palette draws its cell either way, and a cell that exists gets a name rather than a silent gap. No PRD id assigned yet — drawn disabled (§4a) until one is. |

> **This table used to say "Fold into existing phases" for six tools, and four of them then
> never became requirements.** That is how GRAD, FILL and MEASURE went missing for a
> revision. Accepting scope in a UI document is not the same as specifying it — every row
> here now names its requirement or says it was dropped. **The MEASURE/SLICE reversal
> above is the same lesson applied to a "Dropped" row: it is edited in place, with the old
> text kept and the reason for the change recorded, rather than the disposition being
> silently flipped.**

The palette also needs two tools the wireframe did not draw: the **eraser** (PRD F9,
[ADR-0007](adr/0007-erase-is-mass-reduction-not-a-colour.md)) and the **eyedropper**
(PRD Q10).

### 4a. What the palette actually does today

Of the 27 `Tool` values (`app/AppState.hpp`) — reachable either directly, as the icon a
palette cell shows, or through a flyout for every group with more than one member (§2b) —
**seven have real behaviour**: Brush, Water, Dry Brush, Eyedropper, Rectangle Marquee,
Hand, Zoom. The other twenty — every tool named in the table above whose phase has not
arrived yet, plus Move, Lasso, Polygon Lasso, Magic Wand, Frame, Clone Stamp, Paint Bucket,
Pencil, Smudge — exist **for their name, icon and keyboard-shortcut slot only**.
`ui/MacPaintUI.cpp`'s `toolButton()` draws every one of them visibly disabled, whether it
is the member currently showing on a group's own cell or one listed inside that group's
flyout: dimmed icon, no hover highlight, not clickable, and a tooltip that says "Not built
yet." rather than merely doing nothing on a click. **No dead button looks live**, in the
palette or in a flyout.

Water and Dry Brush are this build's own watercolour brush variants and were never in the
wireframe's ~26 tools at all (§2a's icon table says which Lucide glyph each substitutes).
The palette places them in the paint group beside Brush, where a painter reaching for
"the wet one" or "the dry one" would look, rather than leaving them off the palette or
filing them somewhere unrelated to painting.

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
