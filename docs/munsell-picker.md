# A third COLOR picker: the Munsell page, at constant luminance

The COLOR panel has two pickers today (`ui/MacPaintUI.cpp`'s
`drawColorSection`, docs/ui.md §3.3): **PIGMENT**, a well of `defaultPalette()`
rows that sets a colour *and* three physical constants, and **RGB**, an
`ImGui::ColorPicker3` saturation/value square. This document specifies a third,
**MUNSELL**, and everything below exists to serve one property the other two do
not have:

> **Changing hue must not change how light the colour looks.** The row you are
> on is a Munsell *value*; every cell in it has the same luminance, whatever
> hue the page is showing.

That is not a nicety of the layout — it is the whole reason the picker is
worth building, and it is why the geometry below is what it is. It is also
**exactly checkable**, which the RGB square's own behaviour is not. Measured
in the built suite, over 30 156 live cells spanning every `n` from 3 to 16,
36 hues and every row: worst `|ΔY|` = **2.9e-08**, against a 1e-6 tolerance.
That figure is float, not double — it is measured off the `std::array<float,3>`
a swatch would actually receive, which is where the last two orders of
magnitude go (the double-only prototype holds `2.2e-16`).

## Why neither existing picker can do it

`ImGui::ColorPicker3`'s square is HSV. HSV's `V` is `max(r,g,b)` — a channel
maximum, not a luminance. Sliding hue along the top edge of that square at
`S=V=1` walks from a yellow of relative luminance **0.928** to a blue of
**0.072**, a factor of thirteen, with nothing on screen saying so. That
behaviour is documented in `color/Space.hpp` and `app/AppState.hpp` and is not
a defect of the widget; it is what HSV is. The PIGMENT well is a fixed set of
~20 measured paints and has no tint/shade axis at all.

## The colour model

Three decisions, in the order they constrain each other.

### 1. The row is a Munsell value, and Munsell value is a function of luminance alone

ASTM D1535 defines the value scale as a quintic in the luminance factor `Y`
(percent, `Y=100` for the perfect diffuser):

```
Y(V) = 1.1914 V − 0.22533 V² + 0.23352 V³ − 0.020484 V⁴ + 0.00081939 V⁵
```

It is exact at both ends (`Y(0)=0`, `Y(10)=100.000` — the coefficients are
chosen for it) and strictly increasing, so its inverse is a bisection with no
special cases. Measured over the integers:

| V | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|----|
| Y % | 1.180 | 3.048 | 6.391 | 11.701 | 19.272 | 29.301 | 41.985 | 57.620 | 76.696 | 100.000 |
| L\* | 10.41 | 20.24 | 30.38 | 40.74 | 51.00 | 61.05 | 70.86 | 80.53 | 90.18 | 100.00 |

**This is the load-bearing fact of the whole design.** Because `V` depends on
`Y` and nothing else, "same Munsell value" and "same relative luminance" are
the same statement, and the invariant the user asked for stops being a matter
of taste and becomes an equality that `--selftest` can hold to float epsilon.

The honest caveat, said once here and once in the panel's `?` text: Munsell
value — like CIELAB, like CIELUV — ignores the Helmholtz–Kohlrausch effect, so
a strongly chromatic cell will still *look* slightly lighter than the grey at
the far left of its own row. No colour space in this build models that, and
inventing a correction would be a lie the user could not check.

### 2. The plane is CIELUV at fixed L\*, because that is the space where "same L\* ⇒ same Y" is true by construction

Within a row we need a hue angle and a chroma radius. The candidate spaces:

| space | constant lightness ⇒ constant Y? | hue uniformity |
|---|---|---|
| **CIELUV** `L*C*ₕ` | **yes, identically** — `L*` is a pure function of `Y` and `u*,v*` do not touch it | fair; poor in blue |
| CIELAB `L*C*ₕ` | yes, same reason | fair; the notorious blue→purple drift |
| OKLCh | **no** — OK `L` is a function of all three cone responses | best of the three |

Pick **CIELUV**. The picker's one promise is the luminance one; a space where
that promise is approximate rather than exact would make the central assertion
a tolerance rather than an equality, and a tolerance is what rots. OKLCh buys
better hue spacing and would cost the invariant — the trade is not close.

The forward map is closed-form and needs no table:

```
Y  = Y(V)/100                       ASTM D1535, above
L* = 116·Y^⅓ − 16   (Y > 216/24389; the linear branch below it)
u' = u*/(13 L*) + u'ₙ,   v' = v*/(13 L*) + v'ₙ        u*,v* = C·cos h, C·sin h
X  = Y·9u'/(4v'),  Z = Y·(12 − 3u' − 20v')/(4v')
rgb_linear = M⁻¹ · [X Y Z]          Rec.709 primaries, D65 — color/Space.hpp
```

`u'ₙ, v'ₙ` are the D65 white derived from the same matrix `color/Space.hpp`
already carries, not a separately-typed constant — two copies of a white point
is how they drift apart.

### 3. Out of gamut is a **void**, never a clamp

A cell whose linear RGB has any channel outside `[0,1]` is not drawn as a
colour at all. Clamping is not an option here and the reason is structural
rather than aesthetic: **clamping a channel changes `Y`**, and `Y` is the one
thing this picker promises. A row of clamped cells would silently stop being a
constant-luminance row, which is worse than a row with holes in it — the holes
are visible and the drift is not.

So the page has the shape the sRGB gamut actually has, and that shape is
recognisably a Munsell hue page. Measured, `n=9`, `#` = in gamut:

```
  blue, h=260°                     yellow, h=90°
  V=9  ##.......                   V=9  #########
  V=8  ####.....                   V=8  ########.
  V=7  #####....                   V=7  #######..
  V=6  #######..                   V=6  ######...
  V=5  #########   ← widest        V=5  #####....
  V=4  ########.                   V=4  ####.....
  V=3  ######...                   V=3  ###......
  V=2  ####.....                   V=2  ##.......
  V=1  ##.......                   V=1  #........   ← widest is V=9
```

The leaf leans. That is the picker teaching the user something true and
otherwise invisible — there is no light saturated blue and no dark saturated
yellow — and it is the strongest argument for keeping the voids rather than
squaring the grid up.

## The grid

### `n` rows of value, `n` columns of chroma

- **Rows.** `V_i = 10·(i+1)/(n+1)`, `i = 0…n−1`, drawn bottom-to-top so light
  is up. The `+1` offsets matter: they exclude `V=0` and `V=10`, which are the
  black and white points and have zero chroma, so an inclusive sampling would
  spend two of `n` rows on a single colour each. It also makes the default
  `n=9` land on exactly `V = 1,2,…,9` — the classic printed Munsell page.
- **Columns.** `C_j = j·C_page/(n−1)`, `j = 0…n−1`, so column 0 is `C=0`: the
  neutral grey of that value. **The left column of the grid is the full grey
  ramp, for free**, which is the control a tint/shade grid most needs and the
  one the RGB square hides at the far-left edge.
- **`C_page`** is the largest in-gamut chroma over the whole page —
  `max over rows of maxChroma(L*_i, h)` — found by bisection on the in-gamut
  predicate. Normalising per *page* rather than per *row* is what keeps chroma
  comparable between rows, which is what makes the leaf shape above legible.
- **`n` range 3…16, default 9.** Measured live-cell fraction, averaged over 36
  hues: `n=5` 62%, `n=9` 58%, `n=12` 56%, `n=16` 54%. At `n=9` that is 47
  usable colours per page. In the default 322 px dock column a cell is
  `322/n` px, so `n=16` gives 20 px cells — the floor, and the reason for the
  cap.

### Two policies, one switch — and why the default is the ragged one

An alternative normalisation is per **row** (`C_j = j·maxChroma(L*_i,h)/(n−1)`),
which fills 100% of cells. It preserves the luminance invariant exactly — the
prototype confirms `2.2e-16` under both — and it destroys the leaf: chroma
column 4 then means something different in every row, and the page no longer
shows the gamut's shape. Ship **Page** as the default because the user asked
for the Munsell system and the raggedness *is* the Munsell system; expose
**Row** as a checkbox for whoever wants a full grid, and say in one line of
`?` text what it costs.

### Selecting, and what survives a hue change

The selection is the **cell index `(row, col)` plus the hue**, not the RGB
triple. Changing hue therefore keeps the row — which is the promise — and
keeps the chroma *column*, whose absolute chroma moves with `C_page`.

If the new page's cell at `(row, col)` is a void, **clamp along the row to the
rightmost live column, never to another row.** Falling to a neighbouring row
would change the luminance to preserve the chroma, which is the trade this
picker exists to refuse.

## The hue control, and one thing it must not claim yet

A horizontal strip under the grid, `40` steps (Munsell's own 2.5-hue
convention), each swatch drawn **at the currently selected row's `V` and its
own `C_page`-relative chroma** — so the strip is itself a constant-luminance
band, and demonstrates the invariant rather than asserting it.

**Do not print Munsell hue notation (`5R`, `7.5PB`) in phase 1.** The strip's
angle is a CIELUV hue angle, and CIELUV hue angle is *not* Munsell hue —
the mapping is a 40-entry calibration table that has to be derived from the
Munsell renotation data and checked in with its provenance. Until that table
exists, a `5PB` label would be a confident wrong answer, which is the failure
mode this repo has been bitten by before. Phase 1 labels the strip by angle or
not at all; phase 2 adds the table, the notation readout, and
`foregroundName()` returning `"Munsell 5R 5/8"`.

## Wiring: three predicates that will silently paint the wrong colour

`ColorMode` (`app/AppState.hpp:136`) is a two-member enum and **every consumer
tests it by equality against `Rgb`, with pigment as the `else`.** Adding
`ColorMode::Munsell` compiles clean and routes the new mode into the pigment
branch everywhere — the panel would look completely live and every stroke
would lay down `defaultPalette()[st.brush.pigment]`. The three sites, all of
which must become `!= ColorMode::Pigment`:

| site | today | why it matters |
|---|---|---|
| `app/StrokeSession.cpp:958` | `if (colorMode == Rgb) return brush.rgb;` | **the bug.** Every stroke, bucket and gradient |
| `app/StrokeSession.cpp:985` | `if (colorMode == Rgb) return "Custom RGB";` | status readout names the wrong paint |
| `ui/MacPaintUI.cpp:5844` | `switchedToRgbMode = colorMode == Pigment;` | eyedropper reports "no mode change" when it just left MUNSELL |

Alternatively `ColorMode` stays two-valued and MUNSELL becomes a
picker-geometry flag inside `Rgb`. That is safer against exactly this class of
bug — but it makes the header row a two-button toggle plus a hidden third
state, and `foregroundName()` can then never say `"Munsell 5R 5/8"` because
nothing records that the triple came from a page. Take the enum, fix the three
predicates, and pin them with the assertion below.

### State

On `BrushState`, beside `rgb` and `pigment`:

```cpp
float munsellHueDeg = 30.0f;   // CIELUV hue angle; Munsell notation is phase 2
int   munsellRow = 4;          // 0-based, bottom-up
int   munsellCol = 4;
int   munsellSteps = 9;        // n, 3..16
bool  munsellPerRowChroma = false;
```

`BrushState::rgb` stays the single source of truth for *painting* and is
rewritten on every pick; `H/V/C` is the editing state that RGB alone cannot
round-trip. The desync risk is real and has one rule: **any route that writes
`rgb` without going through the grid must leave MUNSELL mode.** The eyedropper
already switches mode; the table above is what makes it switch correctly.

## Panel layout — measured, and it changed the design

The section's content region is **306 x 126 px** in the default dock. That is
measured, from an `NP_MUNSELL_LAYOUT_DUMP=1` build of the branch itself, and
the first implementation — a square grid with a hue strip beneath it and five
lines of readout, sized the way the RGB branch sizes its picker — asked for
**221 px of the 126 it had**. The dock does not scroll (docs/ui.md §2c), so the
chroma toggle and every readout line were not merely below the fold, they were
unreachable. Three readings of the code produced three wrong guesses about
why; one line of instrumented output gave the answer.

Three things changed as a result, and all three are the measurement's doing
rather than taste:

- **The chips are not square.** Width and height are sized independently.
  A square grid capped by the height throws away 170 px of a 306 px column and
  still only yields 9 px cells.
- **The hue control is a vertical bar to the right of the page, not a strip
  under it.** Under the grid it cost 14 px of height plus spacing and left
  7 px rows; beside it, it costs 15 px of width — which this panel has to
  spare — and the rows come out at 10 px. A vertical hue bar next to a
  value/chroma page is also the conventional arrangement, so the layout the
  measurement forced is the one a picker would have wanted anyway.
- **The pigment-constant readout is gone from this branch**, contradicting the
  "Out of scope" note at the end of this document, which said the panel must
  print density/staining/granulation the way the RGB branch does. There is no
  room. They moved into the panel's `?` text instead of being drawn into the
  clipped region where nobody would ever see them. Per-cell value, chroma and
  sRGB are in the grid's own tooltip, which costs no height at all.

Final geometry at `n = 9` in the default dock: 31 x 10 px chips, 90 px of
grid, 29 px of controls, 1 px to spare. The arithmetic is linear in the
available space, so a taller dock gives a taller grid with no further work.

`NP_MUNSELL_LAYOUT_DUMP` is kept rather than deleted: if the dock geometry
changes, the numbers above get re-measured rather than re-derived.

## New files

- **`src/color/Munsell.{hpp,cpp}`** *(built)* — `munsellValueToLuminanceFactor`,
  `luminanceFactorToMunsellValue`, `lStarFromRelativeLuminance`,
  `munsellPageRowValue`, `lchUvToLinearRgb`, `munsellCellLinearRgb`,
  `maxInGamutChroma`, `pageChroma`, and the RGB↔XYZ matrix it derives from
  `kRec709Primaries`. `double` internally, `float` at the boundary — the
  gamut bisection is why. No ImGui, no `AppState`.
- **`src/app/selftest/Munsell.cpp`** *(built)* — registered in
  `src/CMakeLists.txt`, declared in `app/SelfTest.hpp`, invoked from
  `main.cpp` beside `runColorSpaceTest()` and folded into the suite's `ok`.
- `drawColorSection`'s third branch in `ui/MacPaintUI.cpp`, and the `?` text
  in `ui/ControlsLayout.cpp`'s `Color` entry.

## Assertions

1. **`Y(V)` round-trips.** `yToMunsellValue(munsellValueToY(V)) == V` to `1e-5`
   over `V ∈ [0,10]` in 0.01 steps; `Y(10) == 100` exactly to `1e-9`.
2. **The row is constant-luminance.** For `n = 3…16`, all 36 hues, every row,
   every live column: `Y` equals the row's target to `1e-6`. *(Built: `2.9e-08`
   over 30 156 cells.)* The coefficients come from the matrix `color/Munsell`
   derives from `color/Space.hpp`'s primaries — checked against the published
   sRGB constants first, so this is not a derivation grading itself. Paired
   with a guard that the sweep visited a plausible number of live cells, so it
   cannot pass by measuring nothing.
3. **No cell is clamped.** Every colour the grid returns is inside `[0,1]` on
   all three channels; a request known to be outside (`L*=90`, `h=260°`,
   `C=110`) returns "void" rather than a triple.
4. **Hue change preserves the row.** Set `(row, col)`, sweep hue over 36
   angles, assert `munsellRow` is unchanged and the resulting `Y` is unchanged
   to `1e-6` — including across at least one void-clamp.
5. **Void clamping moves along the row only.** Force a page where `(row,col)`
   is void; assert the landing cell has the same `row`.
6. **`foregroundSrgb()` obeys MUNSELL.** With `colorMode = Munsell` and
   `pigment = 3`, assert `foregroundSrgb() == brush.rgb` and *not*
   `defaultPalette()[3].rgb`. **This is the one that catches the
   `== ColorMode::Rgb` trap; without it the whole feature is decorative.**
7. **Rows are strictly ordered.** For every `n`, row `i+1`'s target `Y` is
   greater than row `i`'s, and the grid is exactly `n×n` cells.
8. **`maxInGamutChroma` is positive and finite** for all 40 hue steps at every
   default row, and is monotone under bisection (the returned chroma is in
   gamut, `+1e-3` is not).

## Sabotages — measured, not predicted

Eight sabotages, applied one at a time, each with a full rebuild and a full
`--selftest` (or golden) run, source restored byte-for-byte afterwards and
verified by `diff`; `grep -rn SABOTAGE src/` shows only the pre-existing
comment text in four unrelated selftest files.

| sabotage | assertions reddened | elsewhere in the suite |
|---|---|---|
| drop the `V⁵` term from the D1535 quintic | `Y(10)==100`, `Y(5)==19.28`, the V→Y→V round trip, monotonicity | none |
| make luminance depend on chroma (what a constant-OK-`L` surface would do) | **the row-luminance invariant, and only it** — worst `1.5e-01` | none |
| clamp out-of-gamut cells instead of voiding them | the void check, the invariant (worst `7.8e-01`), and the live-cell guard | none |
| `maxInGamutChroma` returns `0.9·lo` | the boundary check, and only it | none |

Steps 2 and 3:

| sabotage | assertions reddened | elsewhere |
|---|---|---|
| revert `foregroundSrgb()` to `== ColorMode::Rgb` | **4** — the cell is not returned, the pigment is, and both luminance assertions collapse | none |
| clamp a void by moving the row instead of the column | the row-held and luminance-held assertions, and only those | none |
| store the linear triple in `BrushState::rgb` instead of the sRGB one | the encode-boundary assertion and the foreground-luminance one | none |
| draw voids as nothing instead of as outlines | the `munsell_page` golden view | none |

**The first of those is the important one.** The `== ColorMode::Rgb` trap was
argued from reading in the first version of this document; reverting that one
line and rebuilding confirms it is real — the panel stays entirely live and
every stroke lays down `defaultPalette()[pigment]`. The last one confirms the
new golden view is photographing the picker rather than empty chrome, which is
this harness's known failure mode.

**Two of my own predictions were wrong, and both matter.**

*The round trip was supposed to survive sabotage 1.* I argued a round trip
only asks a function to agree with itself, so dropping a term would leave it
green and only the published-value checks would fire. It went red too —
dropping `V⁵` makes `Y(V)` non-monotone above V≈8, and the bisection inverse
then lands somewhere else entirely (worst error 4.78 value units). The
published-value checks were still worth adding; the reasoning for adding them
was just incomplete.

*"Every non-void cell is inside [0,1]" is weaker than it reads.* It did **not**
fire under the clamping sabotage, because the returned triple is clipped
against the 1e-9 gamut slack on the way out, which makes the assertion
tautological once voiding is removed. The assertion that actually carries the
no-clamping claim is the single known-out-of-gamut request, plus the
invariant's own drift. Keep the legality check — it is a cheap guard on the
clip — but do not count it as the one that proves the void policy.

The sabotages for assertions 4, 5 and 6 (selection clamping, and
`foregroundSrgb()` obeying the mode) belong to step 2 and have not been run.
A sabotage that reddens nothing is a finding about the suite, not about the
sabotage.

## Dispatch order

**Steps 1-3 are built.** Full suite **7746 pass / 0 FAIL**; golden **39/39**
(one new view, `munsell_page`, reachable through a new `--munsell-demo` flag).

1. **Done.** `color/Munsell.{hpp,cpp}` + `app/selftest/Munsell.cpp`. Assertions
   1, 2, 3, 7, 8 — wider than originally scoped, because the luminance
   invariant needs no UI and it is the point.
2. **Done.** `ColorMode::Munsell`, the three predicate fixes, the
   `app/MunsellSelection` layer, assertions 4, 5, 6 and the sRGB encode
   boundary.
3. **Done.** The panel branch, the `n` control, the hue bar, the golden view.
4. *(phase 2, not started)* The 40-entry renotation hue table, Munsell
   notation readout, `foregroundName()` returning `5R 5/8`. Until that table
   exists, `foregroundName()` says "Munsell page" and nothing prints a hue
   family — a `5PB` label derived from a CIELUV angle would be a confident
   wrong answer.

## Out of scope

Munsell **pigment** constants. A page cell is three floats and cannot say how a
paint settles or lifts, exactly as docs/ui.md §3.3 says of the RGB picker.
MUNSELL is a colour mode, not a paint mode: `foregroundPhysicalConstants()`
keeps answering from the loaded pigment in this mode exactly as it does in RGB
mode, and `--selftest` asserts it. The panel says so in its `?` text rather
than in its body — see *Panel layout* for why that moved.
