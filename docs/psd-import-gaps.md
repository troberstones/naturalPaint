# PSD import: what the real files showed is missing, and how to close it

`io/PsdImport` was verified layer-for-layer against three Photoshop-authored
documents on 2026-08-28 (io/PsdImport.hpp's header records that comparison).
Everything it *claims* to read, it reads correctly. This document is the other
list: **features those three files contain that the importer discards**, in
dispatch order, with the wire layout and the receiving field for each.

Four of the five gaps below need **no engine work at all**. The receiving
feature already exists, is already composited, and is already persisted — only
the decode is missing. That is why the ordering below is what it is.

## The evidence these are worth doing

`Peter_confronts_a_small_monster_with_fire.psd` opened in the application today
shows orange halos around both figures and a scatter of sparks that are not in
Photoshop's own composite. Two causes, and one layer carries most of it:

> **`Layer 30`** is a full-canvas Linear Dodge (`lddg`) glow at 20% opacity
> whose mask has a **mean of 0.017** — Photoshop shows 1.7% of it. We import
> neither the mask nor the blend mode, so it composites as a full-canvas
> Normal layer at 20%. One layer, both gaps, most of the visible error.

## The sample files, and what they can and cannot test

All three are **8-bit, RGB (mode 3), version 1 (PSD not PSB), RLE**. 16-bit,
32-bit, ZIP, PSB and non-RGB modes stay fixture-only and are refused by name;
nothing below changes that.

| feature | `Lineart4_crop` | `Testforautoflats 2` | `Peter_…fire` |
|---|---|---|---|
| layer records | 27 | 87 | 53 |
| `luni` names | 27 | 87 | 53 |
| raster masks (channel −2) | 0 | 0 | **10** |
| groups (`lsct`) | 0 | **6 records / 3 groups** | **6 records / 3 groups** |
| group nesting depth | — | 0 | 0 |
| non-`norm` blend keys | 0 | 0 | **`colr`×3, `lddg`×1** |
| `lspf` non-zero | 0 | 0 | **2** |
| `lclr` set | 0 | 0 | 0 (present, all NONE) |
| `iOpa` (fill opacity) | 0 | 0 | 0 |
| doc-level blocks | none | none | `Patt CAI OCIO GenI FMsk cinf` |

**Two things no fixture here can validate**, and both need a hand-written
fixture rather than a real file:

1. **The mask flag "position relative to layer" (bit 0) is set on all ten
   masks, and every masked layer sits at origin (0,0) with a full-canvas
   rect** — so relative and absolute mask coordinates coincide in all ten
   cases. A wrong implementation of that flag is indistinguishable from a
   right one on these files.
2. **Group nesting.** Every group in both files is depth 0.

---

## 1. Raster layer masks — the visible one

**Receiving feature is complete.** `Layer::mask` is a `MaskTileStore`
(core/Mask.hpp); `core/Composite.cpp:30` multiplies coverage by it on every
path; `np:mask` already persists it; `app/selftest/LayerMask.cpp` already
tests it. T16's open work is the mask *chip UI* — unrelated, and not a
blocker.

### The one thing that makes this harder than it looks

**A mask channel's pixel data is sized by the MASK rectangle, not the layer
rectangle.** Verified on the real file — every masked layer there is
5000×2559 while its mask is not:

```
Layer 36     layer 5000x2559   mask 1114x1506   mean 0.852
glass glare  layer 5000x2559   mask  160x105    mean 0.920
Tint         layer 5000x2559   mask  567x919    mean 0.777
Layer 30     layer 5000x2559   mask 5000x2559   mean 0.017
Layer 10     layer 5000x2559   mask  141x138    mean 0.292
```

So the mask rect must reach the channel loop. Today it cannot: the block that
carries it is skipped wholesale by its own length at
[PsdImport.cpp:422](../src/io/PsdImport.cpp) —

```cpp
  uint32_t maskLen = 0;
  if (!ec.u32(maskLen) || !ec.skip(maskLen)) { ... }
```

— and the channel loop at [PsdImport.cpp:877](../src/io/PsdImport.cpp)
`continue`s past ids −2/−3 before decoding. **Those two edits are one change,
not two.** Decoding a −2 channel at `layerWidth × layerHeight` would read the
wrong number of samples and desynchronise nothing (each channel's length is
declared) but produce garbage.

### Wire layout, verified against `Peter_…fire.psd` rather than recalled

The 20-byte shape, byte for byte from `Layer 36`:

```
00 00 00 14    size = 20        (u32; 0 = no mask, 20 = this shape, >20 = with "real" mask)
00 00 02 17    top    =  535    (i32)
00 00 02 25    left   =  549    (i32)
00 00 07 f9    bottom = 2041    (i32)
00 00 06 7f    right  = 1663    (i32)
ff             default colour = 255   (u8)
01             flags = 0x01           (u8)
00 00          padding — present ONLY when size == 20
```

`flags` bit 0 = position relative to layer, bit 1 = **mask disabled**, bit 2 =
invert (obsolete), bit 3 = user mask from render, bit 4 = mask parameters
applied. When size > 20 a second "real" mask follows (real flags, real
background, real rect); derive that half from the published spec and **refuse
by name if you do not implement it** rather than guessing — none of the three
sample files exercises it (all ten are exactly 20).

### `MaskTile`'s default is 1.0, and that inverts the empty-tile rule

`MaskTile()` fills with `kRevealWord` (binary16 1.0) — core/Mask.hpp is
explicit that this is the one tile type in the codebase whose default is not
zero, because an absent mask tile must mean *reveal*. Consequences:

- The empty-tile skip that `cacbd12` added for RGB **inverts** here: skip a
  mask tile when every sample in it is **1.0**, never when it is 0.
- **The mask's `default colour` byte governs everything outside the mask
  rect.** 255 (all ten samples) means "reveal outside", which is exactly what
  absent tiles already mean — free. **0 means everything outside the rect is
  HIDDEN**, and that must be written explicitly across the whole layer extent,
  which is expensive and is the case no sample file covers. Handle it or
  refuse it by name; do not silently treat 0 as 255.
- Related and worth reading first: `naturalpaint-selection-semantics` — this
  project has been bitten once already by a coverage default that is the
  inverse of a layer mask's.

### Assertions to write

- mask rect **smaller** than the layer rect, offset from it, decoded at the
  mask's own dimensions — the property that fails if you reuse `layerWidth`.
- mask rect **larger** than the layer rect.
- `default colour = 0` with a small mask rect: everything outside is hidden.
- flags bit 1 (**mask disabled**) — imported as no mask at all, not as an
  applied one.
- a mask whose samples are all 255 allocates **zero** tiles (the inverted skip).
- **and the anti-vacuity partner**: a mask with real content still multiplies
  coverage where it should. Section E of `app/selftest/PsdImport.cpp` is the
  model — E2 pins the allocation, E3 pins that the content survived, because
  "allocate nothing" is what a broken reader does best.
- **position-relative-to-layer**, both ways, on a layer whose origin is NOT
  (0,0). The real files cannot discriminate this; a fixture must.

### Sabotages that must redden exactly one assertion each

- decode the mask at `layerWidth × layerHeight` → the offset-rect assertion.
- ignore the `default colour` byte → the `default = 0` assertion.
- ignore flags bit 1 → the disabled-mask assertion.
- skip mask tiles that are all **0** instead of all **1** → the reveal-skip
  assertion, and it must NOT redden the content one.

---

## 2. `lddg` → `BlendMode::Plus` — one line

`core/Blend.cpp:177` implements `Cs + Cb`, additive light. Photoshop's
**Linear Dodge (Add)** is that operator. It is simply absent from
`kBlendKeyMap` at [PsdImport.cpp:231](../src/io/PsdImport.cpp), so it takes
the "no equivalent" path and imports as Normal with a warning.

```cpp
constexpr BlendKeyMap kBlendKeyMap[] = {
    {"norm", BlendMode::Normal},
    {"mul ", BlendMode::Multiply},
    {"scrn", BlendMode::Screen},
    {"dark", BlendMode::Min},
    {"lite", BlendMode::Max},
};
```

**State the caveat in the comment rather than claiming exactness.** `dark` and
`lite` are genuinely exact — min and max are order-preserving, so they commute
with any monotone transfer function. Addition does not: Photoshop adds in
gamma space by default ("Blend RGB Colors Using Gamma 1.0" off) and this
codebase adds in linear light. That is the **identical** compromise `mul ` and
`scrn` already ship with, so the mapping is consistent with what is already
there — but the header currently argues `dark`/`lite` are exact, and adding
`lddg` to that same sentence would make the sentence false.

Assertion: a `lddg` layer imports as `plus` **and emits no warning**. Sabotage:
remove the row; the warning-count assertion must redden.

`colr` (Color) has **no** equivalent — the palette is Normal, Plus, Multiply,
Screen, Min, Max, Mix. The warning is the honest answer and stays.

---

## 3. Layer groups (`lsct`) — a mapping, not new machinery

Today the six `lsct` records per file import as six junk empty layers, one of
them literally named `</Layer group>` in the layer panel.

**The receiving model exists**: `LayerKind::Group`, `Layer::groupTag`
(`"G" + Document::nextGroupId`, assigned eagerly), `Layer::parent` (holds a
group's `groupTag` verbatim), and `core/LayerSetOps.cpp`'s `GroupLayers` /
`UngroupLayers`. Read `core/Layer.hpp`'s comments on `parent` and `groupTag`
before writing anything — the identity scheme is argued there and must not be
reinvented.

**And the semantics line up exactly**: `core/Composite.hpp:688` argues that a
group here is **pass-through, not isolated**, and all six groups in the two
sample files are `pass`. No mismatch to paper over.

### Wire layout, verified

```
divider:   38 42 49 4d 6c 73 63 74  00 00 00 04  00 00 00 03
           '8BIM'      'lsct'        len = 4      type = 3

header:    38 42 49 4d 6c 73 63 74  00 00 00 10  00 00 00 01
           '8BIM'      'lsct'        len = 16     type = 1
           38 42 49 4d 70 61 73 73  00 00 00 00
           '8BIM'      'pass'        sub-type = 0
```

`type`: 0 = other, 1 = open folder, 2 = closed folder, **3 = bounding section
divider**. If `len >= 12`, `8BIM` + a 4-byte blend key follows. If
`len >= 16`, a u32 sub-type follows.

### The shape of the pass, and the trap in it

PSD's flat record list runs **bottom-first**, and a group's members are
enclosed **between** its divider (type 3, which comes *first* in file order)
and its header (type 1/2, which comes *last*). So the divider **opens** the
group as you read forward and the header **closes** it and names it. Reading
them the other way round produces a document that opens with every group's
membership inverted and no error anywhere.

Suggested pass: walk records in file order maintaining a stack; type 3 pushes;
type 1/2 pops, emits the Group layer with a fresh `groupTag`, and stamps
`parent` on everything popped. Drop the divider record entirely — it is not a
layer.

- Non-`pass` group blend keys: import the group and **warn by name**, same
  discipline as an unmapped layer blend key. Do not silently treat isolated
  as pass-through.
- An unbalanced stack at end of file (a divider with no header, or the
  reverse) is a **refusal**, not a repair — `io/Descriptor.hpp`: "a refusal is
  total".
- Nesting: implement it (the stack gives it for free) but say plainly in the
  test that no sample file exercises depth > 0.

Assertions: 3 groups and 84 layers from 87 records; membership correct;
`</Layer group>` appears nowhere in the result; a nested fixture; an
unbalanced fixture refuses. Sabotage: swap the push/pop roles — the membership
assertion must redden while the count assertion stays green (that is what
makes them two assertions).

---

## 4. `lspf` — layer protection flags

```
38 42 49 4d 6c 73 70 66  00 00 00 04  00 00 00 01
'8BIM'      'lspf'        len = 4      flags = 0x01
```

Bit 0 = transparency locked → **`Layer::alphaLocked`**, the feature that
landed in `4931d6d`. Bit 1 = composite locked, bit 2 = position locked; the
nearest thing to those here is `Layer::locked` — decide deliberately whether
either maps, and if not, say so in the header rather than dropping them
silently.

Two layers in `Peter_…fire.psd` carry `0x00000001` (`Layer 19`, `Layer 26`).
Cheap, and there is a working feature on the other end.

---

## 5. `lyid` and `lclr`

`lyid` (u32 layer id) → `Layer::id`. **Read `core/Layer.hpp` on `id` first**:
it is assigned *lazily*, only by `normalizeLayerIds()` from
`captureLayerComp()`, and its comment states the property that protects — "a
document that never uses a comp carries zeros here for its whole life".
Stamping ids at import time from a foreign document breaks that invariant.
Probably: do **not** import `lyid` unless a concrete need appears. Recorded
here so the decision is visible rather than an oversight.

`lclr` → `Layer::colorLabel`. Present on all 53 layers of `Peter_…fire.psd`
and set to NONE on every one, so nothing in these files is lost. Low value,
trivial, do it alongside `lspf` if at all.

---

## Present in the samples and correctly ignored

`knko`, `infx`, `clbl` — all at their default values in all three files.
`fxrp`, `shmd`, `lnsr` — reference point, metadata, name-source; no pixel
meaning here. No vector masks, no text layers, no adjustment layers, no smart
objects, no layer effects (`lrFX`/`lfx2`), no `iOpa`.

Document-level in `Peter_…fire.psd`: `Patt` (patterns — note `io/PsPatterns`
exists on the ABR branch), `CAI ` (content provenance), `OCIO`, `GenI`
(generative), `FMsk`, `cinf`. None affects pixels. If `Patt` is ever wanted,
it is a separate job with an existing reader to reuse.

---

## Dispatch notes

**Do 1 and 2 together.** They are the whole visible defect, they share the
same four layers (all four unmapped-blend layers are also masked), and the
before/after is one screenshot.

3 and 4 are independent of each other and of 1–2; they can go in parallel or
be skipped without blocking anything.

**How to verify any of it** — the oracle is reusable and takes seconds to set
up. psd-tools 1.18.0 (MIT) in a venv:

```bash
python3 -m venv psdvenv && ./psdvenv/bin/pip install psd-tools numpy
```

`PSDImage.open(p)._record.layer_and_mask_information.layer_info.layer_records`
is the **flat file record order** — the list `io/PsdImport` walks.
`p.descendants()` is **not**: it nests and hides the divider records, so a
pairwise index diff reports a difference that is only traversal. Compare as a
multiset keyed on (name, covered-pixel count).

`--psd-report <file.psd>` (app/PsdReport.hpp) is this side of the comparison.
Its mean-linear-RGBA column is the part that catches a channel swap or a lost
linearisation; agreement to ~2.3e-4 is `rgba16float`'s quantisation floor and
anything past ~2e-3 is a real disagreement.

**The three sample files are the user's artwork.** They are named individually
in `.gitignore`, are not this project's to redistribute, and must never be
committed — the same rule the Kyle Webster `.abr` packs are under. Nothing in
`--selftest` may depend on them; every assertion above is a hand-written
fixture, and `--psd-report` is the by-hand check against a file someone has.
