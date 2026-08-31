# Blend modes: what Photoshop has that this build doesn't, and what closing that costs

`core/Blend` implements **7** blend modes. Photoshop has **27** (26 real blend
modes plus `pass`, which is a group-compositing flag, not a blend). This
document scopes closing that gap — which modes are cheap, which are hard, and
why, so the work can be dispatched the way `docs/psd-import-gaps.md` was.

**This is a compositor gap, not an import gap.** `io/PsdImport`'s
`kBlendKeyMap` already does everything it can: every PSD blend key is read
correctly, mapped where `core::BlendMode` has an equivalent, and reported by
name where it doesn't (`io/PsdImport.hpp`'s "Blend mode mapping" section).
Adding a PSD key to that table is a one-line, last-step change *once the
engine has the mode* — the entire cost below is building the mode itself,
which the importer will pick up for free.

## The evidence

The user's own file, `Peter_confronts_a_small_monster_with_fire.psd`, uses
`colr` (Color) three times (`Tint`, `Layer 26`, `Layer 58`) and `lddg` (Linear
Dodge/Add) once. `lddg` now imports correctly (this session's PSD-gap-2 fix).
`colr` correctly imports as Normal with a named warning, because there is
nothing in `core::BlendMode` to map it to:

```
1 x  layer 'Layer 26': PSD blend mode 'colr' has no equivalent in this build and was imported as Normal.
1 x  layer 'Layer 58': PSD blend mode 'colr' has no equivalent in this build and was imported as Normal.
1 x  layer 'Tint':     PSD blend mode 'colr' has no equivalent in this build and was imported as Normal.
```

That is the importer behaving exactly as designed. Fixing the look means
teaching the compositor Color, not teaching the importer anything new.

## What's already implemented

| mode | PSD key | `core::BlendMode` | `BlendSpace` |
|---|---|---|---|
| Normal | `norm` | `Normal` | LinearLight |
| Multiply | `mul ` | `Multiply` | LinearLight |
| Screen | `scrn` | `Screen` | **DisplayReferred** |
| Darken | `dark` | `Min` | LinearLight |
| Lighten | `lite` | `Max` | LinearLight |
| Linear Dodge (Add) | `lddg` | `Plus` | LinearLight |
| — (no PSD key; this build's own KM latent lerp) | — | `Mix` | LinearLight |

`Screen` is the load-bearing precedent for everything below: it is already a
`DisplayReferred` mode living inside a scene-linear compositor, labelled as
such (`core/Blend.hpp`'s PRD B7 mechanism), with its misbehaviour above 1.0
proven numerically in `--selftest` rather than asserted in a comment. Adding
more `DisplayReferred` modes is not new territory — it is doing seven more
times what this codebase already does once.

## The two real technical problems

**1. Every mode below is display-referred, and most of Photoshop's modes are
_all_ of Photoshop's modes.** `core/Blend.hpp`'s own B7 criterion — monotone
non-decreasing over the whole non-negative range, with no built-in reference
white — is what makes a mode safe in a scene-linear working space where a
value can legitimately exceed 1.0 (an additive brush stroke, an HDR
highlight). Apply that criterion below and **every mode this document scopes
fails it**: each one has a `1` or a `0.5` baked into its formula that only
means "white" or "the midpoint" in a display-referred [0,1] encoding. This
isn't a defect in the plan — B7's whole mechanism exists so a mode can ship
with this property disclosed rather than pretended away — but it means the
`DisplayReferred` count goes from 1 mode to potentially 20+, and every one of
them will look subtly wrong on out-of-gamut / above-1.0 linear-light content,
the same way `Screen` already does. Worth deciding, up front, whether that's
an acceptable trade project-wide or whether some modes should wait for a
"blend in display-referred space" mode toggle (a bigger feature, not scoped
here).

**2. The four non-separable modes need real division by alpha, which nothing
in this codebase's blend arithmetic has ever needed before.** Every mode
implemented so far is *separable* — each output channel depends only on the
matching input channel — which is exactly what let `core/Blend.cpp` derive a
division-free premultiplied form for each one (the worked derivations in its
comments). Hue, Saturation, Color and Luminosity are **not separable**: they
need the actual `Lum()`/`Sat()` of the whole RGB triple, which means
un-premultiplying (`Cs = cs / as`) somewhere. Two ways to keep the existing
"transparent source is a bit-exact identity" invariant (asserted in
`--selftest` across every mode today) despite that:

  - Derive a fully division-free premultiplied form, the same way `Multiply`
    and `Screen` were. Possible in principle — `Lum()` and `Sat()` are both
    positively homogeneous (`Lum(k·C) = k·Lum(C)` for `k ≥ 0`, likewise
    `Sat`), so they scale cleanly with the premultiply factor without
    literal division — but `ClipColor()`'s two conditional branches
    (`n < 0`, `x > 1`) each contain their own division (`/(L−n)`,
    `/(x−L)`), and those have not been derived here. This is real,
    unstarted math, not a formality.
  - Or special-case `as == 0` / `ab == 0` as an explicit early return
    (`blendPixel()` already does exactly this for `Normal` and `Mix`), then
    un-premultiply for real in the general case. Simpler to get right,
    slightly less uniform with the rest of the file's style, and the
    pragmatic recommendation below.

**A smaller third finding, worth one line:** Photoshop's own `Lum()` for
these four modes uses weights `0.3 / 0.59 / 0.11`, defined by the same PDF
1.7 spec `core/Blend.hpp` already cites for its separable formula. This
codebase already has a luma-weights constant —
`kRec709LumaWeights` (`0.2126 / 0.7152 / 0.0722`, `ops/ColorOps.hpp`) — used
by Saturation/Grayscale ops. **Do not reuse it here.** It's Rec.709 luma for
a different purpose; using it for blend-mode `Lum()` would silently diverge
from Photoshop's own output while looking like a reasonable code-reuse move.

## Per-mode scope

Every formula below is the PDF 1.7 / CSS Compositing Level 1 spec's public,
standard definition — the same spec `core/Blend.hpp` already cites for its
existing separable-blend formula — or a widely-published Photoshop-parity
extension to it (Linear Burn/Light, Vivid/Pin Light, Hard Mix, Darker/Lighter
Color, Subtract, Divide are Photoshop additions beyond the W3C/PDF set, not
in it). None of this came from Krita or from either in-force patent this
project stays away from; blend-mode arithmetic has been public, standard
graphics knowledge for decades, independently reimplemented in GIMP,
ImageMagick and every PDF/SVG renderer. PSD keys are cross-checked against
psd-tools 1.18.0's `constants.py` (the same MIT-licensed oracle
`io/PsdImport.hpp` already uses), not guessed.

`Cb`/`Cs` are straight (un-premultiplied), [0,1]-nominal backdrop/source.

### Stage 1 — separable, no new machinery, closest to what's already there

| mode | key | formula | `BlendSpace` |
|---|---|---|---|
| Difference | `diff` | `\|Cb − Cs\|` | DisplayReferred |
| Exclusion | `smud` | `Cb + Cs − 2·Cb·Cs` | DisplayReferred |
| Subtract | `fsub` | `Cb − Cs` | DisplayReferred |
| Linear Burn | `lbrn` | `Cb + Cs − 1` | DisplayReferred |
| Color Dodge | `div ` | `Cb==0 ? 0 : Cs==1 ? 1 : min(1, Cb/(1−Cs))` | DisplayReferred |
| Color Burn | `idiv` | `Cb==1 ? 1 : Cs==0 ? 0 : 1 − min(1, (1−Cb)/Cs)` | DisplayReferred |
| Divide | `fdiv` | `Cs==0 ? 1 : min(1, Cb/Cs)` | DisplayReferred |

Difference and Exclusion are the two modes in this whole document closest to
`Screen`'s own shape (`Exclusion` is literally `Screen` minus `2·Cb·Cs`
instead of `Cb·Cs`), which makes them worth checking against B7's
monotonicity clause on its own terms rather than by pattern-matching the rest
of the table — done here rather than deferred: `∂/∂Cs |Cb−Cs| = ∓1`
(V-shaped, decreasing then increasing — fails monotonicity outright, no
reference-white argument needed), and `∂/∂Cs Exclusion = 1 − 2Cb`, negative
for any `Cb > 0.5` — so both fail on monotonicity grounds alone, independent
of and in addition to the "has a baked-in 1.0" argument every other row in
this table relies on.

Color Dodge/Burn/Divide each have one conditional guarding a division by a
source or backdrop value — the same shape `ClipColor()` has, at much smaller
scale, and a good place to validate the "explicit early-return before
dividing" approach from problem 2 above before using it on the harder modes.

### Stage 2 — separable, but each is defined in terms of a Stage 1 or existing mode

| mode | key | formula | depends on |
|---|---|---|---|
| Hard Light | `hLit` | `Cs≤0.5 ? Multiply(Cb,2Cs) : Screen(Cb,2Cs−1)` | Multiply, Screen (have) |
| Overlay | `over` | `HardLight(Cs, Cb)` — same function, swapped args | Hard Light |
| Vivid Light | `vLit` | `Cs≤0.5 ? ColorBurn(Cb,2Cs) : ColorDodge(Cb,2Cs−1)` | Stage 1 |
| Linear Light | `lLit` | `Cs≤0.5 ? LinearBurn(Cb,2Cs) : LinearDodge(Cb,2Cs−1)` = `Cb+2Cs−1` | Stage 1, `Plus` (have) |
| Pin Light | `pLit` | `Cs<0.5 ? min(Cb,2Cs) : max(Cb,2Cs−1)` | Darken/Lighten shape (have) |
| Soft Light | `sLit` | see below | none (self-contained) |
| Hard Mix | `hMix` | **two published formulas — needs a decision, not an assumption** | Vivid Light (usually) |

Soft Light, spelled out because it's the one Stage-2 mode not reducible to
something else:

```
D(x) = x ≤ 0.25 ? ((16x−12)x+4)x : sqrt(x)
SoftLight(Cb,Cs) = Cs ≤ 0.5
    ? Cb − (1−2Cs)·Cb·(1−Cb)
    : Cb + (2Cs−1)·(D(Cb)−Cb)
```

Hard Mix is commonly published two ways — `VividLight(Cb,Cs) < 0.5 ? 0 : 1`
(threshold the Stage-2 result) and the simpler `Cb+Cs ≥ 1 ? 1 : 0` — and they
do not always agree. Pick one deliberately and say which, rather than
transcribing the first source found.

### Stage 3 — non-separable, needs problem 2's un-premultiply resolved first

| mode | key | formula |
|---|---|---|
| Hue | `hue ` | `SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb))` |
| Saturation | `sat ` | `SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb))` |
| **Color** | `colr` | `SetLum(Cs, Lum(Cb))` |
| **Luminosity** | `lum ` | `SetLum(Cb, Lum(Cs))` |
| Darker Color | `dkCl` | `Lum(Cb) ≤ Lum(Cs) ? Cb : Cs` (whole triple, not per-channel) |
| Lighter Color | `lgCl` | `Lum(Cb) ≥ Lum(Cs) ? Cb : Cs` |

Color and Luminosity — the two the current PSD file actually uses — are the
**cheapest pair in this stage**: each is one `SetLum()` call, no `SetSat()`.
Hue and Saturation need the full `SetSat()` algorithm (sorts the three
channels, rescales the middle one to preserve `Sat()` while keeping relative
order) on top. Darker/Lighter Color are non-separable but far simpler than
the other four — one `Lum()` comparison and a whole-triple select, no
`ClipColor()` at all — and could reasonably move to Stage 1 once problem 2's
early-return approach is in hand, since they need no un-premultiply beyond
computing `Lum()` (which, per problem 2, scales cleanly through premultiply
without literal division).

`Lum`, `Sat`, `SetLum`, `SetSat`, `ClipColor` are the PDF 1.7 spec's own
named helper functions for exactly these four modes — same spec, same
section, publicly documented, not reimplemented from anywhere restricted.

### Out of scope: Dissolve

`diss` is not a deterministic two-pixel function. Photoshop dithers: each
covered pixel is shown at full source opacity or not at all, chosen by a
per-pixel random threshold weighted by the layer's alpha. `blendPixel(mode,
src, dst)`'s signature — pure, no side channel, called identically by every
site in `core/Composite.cpp` (see "What touching the engine actually costs"
below) — has nowhere to put a random seed,
and giving it one would change the signature for every other mode to serve
exactly one. Recommend leaving this out entirely rather than scoping it in;
flag to the user only if they specifically want it.

## What touching the engine actually costs

Checked directly, not assumed:

- **`core/Blend.hpp`/`.cpp`**: grow the `BlendMode` enum, add a `kModes` row
  per mode (name, label, `BlendSpace`, the two `composites*` flags — the
  aggregate-init trick already makes a missing classification a compile
  error), add the `blendPixel()` switch case.
- **`core/Composite.cpp`**: **nothing.** Every call site
  (`clipGroupFold()`, the main walk, the `Mix`-pair path) already dispatches
  through the single `blendPixel(mode, src, dst)` function with no
  mode-specific branching outside it — confirmed by reading every call site,
  not inferred from the header comment.
- **UI dropdown**: **nothing.** `app/LayerPanel.cpp`'s `blendMenuEntryText()`
  and the panel's own list both walk `allBlendModes()` and already append
  `" (display-referred)"` automatically from `BlendModeInfo::space`. A mode
  added to the table appears in the menu, correctly labelled, for free.
- **`io/PsdImport.cpp`**: one `kBlendKeyMap` row per mode, using the PSD keys
  in the tables above.
- **`--selftest`**: two hardcoded `allBlendModes().size() == 7` /
  `blendMenuForLayer(...).size() == 7` counts (`app/selftest/Blend.cpp`,
  `app/selftest/LayerPanel2a.cpp`) need bumping per mode added. The existing
  per-mode loops (alpha-is-always-`over`, transparent-source/backdrop
  identity) already iterate `allBlendModes()`, so a new mode is checked
  against both invariants automatically — but a mode that can't actually
  satisfy the transparent-identity invariant (a naive, non-early-returning
  non-separable implementation, see problem 2) will **fail that loop**, not
  silently skip it. That failure is the safety net for problem 2, not a
  soon-to-be-deleted test to route around.
- **Oracle coverage**: only `colr` and `lddg` are exercised by a real
  Photoshop file on this machine (`lddg` already verified this session).
  Every other mode in this document ships on formula-plus-selftest evidence
  alone, same as `docs/psd-import-gaps.md`'s own fixture-only gaps — worth
  naming per mode when it lands, not left implicit.

## Recommended order

1. **Color, Luminosity** (Stage 3's cheap pair) — closes the actual gap in
   the user's file, and forces problem 2's early-return resolution while the
   blast radius is one `SetLum()` call rather than four modes' worth.
2. **Stage 1** (7 modes, all separable, all follow the existing derivation
   pattern almost mechanically) — builds out the "Darken"/"Lighten" families
   Photoshop users expect to find complete, and Color Dodge/Burn's
   div-by-zero guard is good practice for the harder modes.
3. **Stage 2** (6 modes, mostly composed from Stage 1 + existing modes) —
   Overlay is probably the single most-requested mode not yet covered.
4. **Hue, Saturation, Darker Color, Lighter Color** (Stage 3's remainder).
5. **Dissolve** — only if asked for specifically; different mechanism
   entirely, scoped out above.

Each stage is independently shippable and independently PSD-import-testable
the moment its `kBlendKeyMap` row lands, the same dispatch shape
`docs/psd-import-gaps.md`'s four gaps used.
