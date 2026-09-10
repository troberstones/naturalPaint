# PSD export: writing the format we already read

PLAN.md phase 15 asks for "flattened PSD first (small), then simply-layered:
one PSD layer per naturalPaint layer, blend modes mapped where they exist,
latents dropped with a warning naming what was lost."

This document is the dispatchable form of that. It is deliberately the mirror
of [docs/psd-import-gaps.md](psd-import-gaps.md): every wire layout below is
stated as `io/PsdImport.cpp` **already parses it**, because that parse was
verified layer-for-layer against three real Photoshop files with psd-tools
1.18.0 as an oracle (io/PsdImport.hpp's header records the comparison). A
writer that emits what our own reader parses is not guessing at a format —
it is inverting a function whose forward direction has been checked against
Photoshop's own output.

## OpenImageIO is not going to do this, and two documents say it will

`io/Export.cpp:252` refuses PSD export today with a reason that names the
cause: **the linked OpenImageIO has no PSD writer at all** — its
`output_format_list` contains no `psd` entry, which `io/Capabilities.cpp`
probes at runtime rather than assuming. OIIO's `psd` plugin is input-only
upstream; there is a `psdinput` and there is no `psdoutput`.

Two lines in [docs/autoflats-migration.md](autoflats-migration.md) are written
against the opposite assumption and are **wrong**:

* `:112` — "`core/psd.ts` | 217 | **delete** — OIIO plus the existing PSD
  export tiers". There are no existing PSD export tiers, and OIIO is not one.
* `:322` — "`ag-psd` (MIT) is replaced by OIIO".

`docs/spec-vs-implementation.md:83` has the honest version of the same row
("PSD group export | no symbol | not built"). Both autoFlats lines are
corrected by track **E** (gather-time) below; they are called out here because the flats
chain was planned against them, and a plan resting on a writer that does not
exist is the shape of stale-magnitude-claim this project has been bitten by
before.

## What already exists, and what genuinely has to be written

| needed | status |
|---|---|
| big-endian byte writer, backpatched section lengths | **landed in the wave base** — `io/PsdWrite`, `5881d5b` |
| PackBits **encoder** | **landed in the wave base** — `encodePackBits()`, asserted as `decodePackBits()`'s inverse |
| PSD wire layout | **known and verified** — `io/PsdImport.cpp` parses every field below |
| blend-key ↔ `core::BlendMode` table | **exists**, 30 keys, `PsdImport.cpp:238` — but file-local; needs promoting (track D) |
| flattened composite as linear RGBA | **exists** — `flattenDocumentToLinear(doc, warningsOut)`, `io/Export.hpp:345` |
| per-layer pixels | **exists** — `TileStore` iteration; `app/PsdReport.cpp:55` is a worked example |
| sRGB encode on the way out | **exists** — `color::srgbEncode()`, the exact inverse of the `srgbDecode()` the importer uses |
| straight (unassociated) alpha | **exists** — `io/Export.cpp:55` already un-premultiplies for every format except EXR |
| capability gate, dialog, refusal plumbing | **exists and is parameterised** — `offerableExportFormats()` is built from `canWrite`, so the UI lights up when the flag flips |
| an external oracle | **exists** — psd-tools 1.18.0 in a venv, plus three real Photoshop files and `--psd-report` |

## The finding that scopes the first landing to 8-bit

**A 16-bit layered PSD does not put its layers in the layer info section.**
Photoshop writes them into an `Lr16` additional-layer-info block inside the
Layer and Mask Information section and leaves the ordinary layer info length
at zero. `grep -n "Lr16" src/io/PsdImport.cpp` finds exactly one hit — a
comment at `:398` listing PSB's widened-length key whitelist. **There is no
`Lr16` case in the reader.**

So a real 16-bit layered PSD reaching `importPsd()` today reads
`layerInfoLen == 0`, returns `noLayerData`, and falls through
`app/OpenAnyFile.cpp` to the flattened OpenImageIO path — opening correct
pixels with one layer and no error. That is consistent with io/PsdImport.hpp
already listing 16-bit as **unverified** (none of the three real files is
16-bit), and it is a latent *import* gap this document found by looking at
export.

**Consequence for this work, and it is not negotiable:** the first landing
writes **8-bit only**. Writing 16-bit correctly means writing `Lr16`, and a
round-trip test would then fail against our own reader — which is the right
failure, but it is a second piece of work on the import side. `ExportBitDepth`
for PSD therefore offers 8-bit alone, and 16-bit is refused **by name**,
naming `Lr16`, in the same style every other refusal in `io/PsdImport` is
worded. Do not silently write 16-bit samples into an ordinary layer info
section: that produces a file Photoshop reads as corrupt and our own reader
reads as flat.

## Tier 1 — the container and the flattened composite

Five sections, in this order. Every field is big-endian.

### File Header — 26 bytes, no length prefix

| bytes | field | value we write |
|---|---|---|
| 4 | signature | `8BPS` |
| 2 | version | `1` (never 2 — PSB is refused on read and never written) |
| 6 | reserved | zero |
| 2 | channel count | `4` (RGB + composite alpha) |
| 4 | height | `doc.height` |
| 4 | width | `doc.width` |
| 2 | depth | `8` |
| 2 | colour mode | `3` (RGB) |

`PsdImport.cpp:1014-1069` parses exactly this and bounds width/height at
**30,000** — PSD's own ceiling, and the reason PSB exists. A document past
that is refused by name at the top of the writer, before a byte is emitted,
rather than truncated.

### Colour Mode Data — `u32` length

Zero for RGB. Only Indexed and Duotone carry any (`PsdImport.cpp:1073`).

### Image Resources — `u32` length

Legal to write as zero, and the first landing does. Two resources are worth
a follow-up, not this landing: `1005` ResolutionInfo (Photoshop shows 72dpi
without it) and `1039` ICC profile.

### Layer and Mask Information — `u32` length

Zero-length in tier 1. That is a legal flat PSD and it is exactly the
`noLayerData` case `app/OpenAnyFile.cpp` already routes to the flattened
path — so **tier 1's own output round-trips through our importer's fallback,
not through `importPsd()`**. Say so in the test, or the round-trip assertion
will look like it passed the wrong function.

### Image Data — to end of file

`u16` compression (`0` raw, `1` RLE), then **planar** channel data: all of R,
then all of G, then all of B, then all of A. For RLE the row-count table for
**every channel** comes first, then the compressed rows — `decodePackBits()`
in `io/PackBits.hpp` documents that framing from the reading side.

**This section is not optional even in tier 2.** A PSD whose Image Data
Section is absent or blank opens blank in every application that does not
parse layers — which is most of them, and is the whole reason Photoshop's
"Maximize Compatibility" exists. Tier 2 writes the same flattened composite
here that tier 1 writes, and gets it from the same
`flattenDocumentToLinear()` call.

### The PackBits encoder

`encodePackBits(std::span<const uint8_t> row) -> std::vector<uint8_t>`, one
row at a time, because the row-count table needs each row's compressed length
separately. Rules: a literal run is `n-1` (0..127) followed by `n` bytes; a
repeat run is `257-n` (i.e. `-(n-1)` as a signed byte, 128..255... encoded as
`257-n` for n in 2..128) followed by one byte; `128` is a no-op and must never
be emitted. Runs never straddle a row boundary — `io/PackBits.hpp` states that
Adobe's own encoder does not emit one that does, and our decoder is
single-pass specifically because well-formed files never need it.

**A correct encoder can make a row larger than the raw row** (worst case
`n + ceil(n/128)`). Photoshop accepts that; do not "fall back to raw" for a
single row inside an RLE-compressed channel, because the compression word is
per-channel-section, not per-row.

## Tier 2 — one PSD layer per naturalPaint layer

Everything below lives inside the Layer and Mask Information section:

```
u32  layer-and-mask-info length      (backpatched)
  u32  layer info length             (backpatched, padded to even)
    i16  layer count                 (negative = composite's first alpha is transparency)
    N x  layer record
    N x  channel image data          (same order as the records)
  u32  global layer mask info length (0)
  (no section-level additional blocks in this landing)
```

### Stacking order: append in `Document::layers` order, no reversal

`Document::layers` index 0 is the bottom of the stack, and the first layer
record in a PSD is also the bottom. io/PsdImport.hpp settles that against
psd-tools' compositor (`bottom = psd[0]`) and it was confirmed on all three
real files (`Background` at index 0). **The writer reverses nothing.** A
reversal is the natural first guess in both directions and is wrong in both.

### Layer record

`PsdImport.cpp:411-560` is the field-by-field parse. In write order:

| field | note |
|---|---|
| `i32` top, left, bottom, right | the layer's own rect, not the canvas |
| `u16` channel count | 4 with a mask, 4 without (`-1` alpha always written), 5 with a mask |
| per channel: `i16` id, `u32` length | id `0/1/2` = R/G/B, `-1` = alpha, `-2` = mask. Length includes the channel's own 2-byte compression word |
| `4` `8BIM` | blend mode signature |
| `4` blend key | from the reversed table, track D |
| `u8` opacity | 0..255 |
| `u8` clipping | `Layer::clipped` → 1, else 0 |
| `u8` flags | **bit 1 SET means HIDDEN**, see below |
| `u8` filler | zero |
| `u32` extra data length | backpatched |
| extra: mask block | `u32` length, `0` or `20` |
| extra: blending ranges | `u32` length, `0` |
| extra: Pascal name | `u8` length + bytes, whole field padded to a multiple of 4 |
| extra: additional blocks | `luni`, and `lsct` for group records |

**The flags bit is inverted from its own name in the spec, and the writer
inverts with it.** Adobe's table says "bit 1 = visible"; psd-tools computes
`visible = not bool(flags & 2)` with the comment "# why 'not'?" at the exact
line. Our reader follows that inversion (`kFlagHidden = 0x02`,
`PsdImport.cpp:496`) and it was confirmed against real files by hidden-layer
counts of exactly 1 and 12 — a wrong-way reader would have said 86 and 41. A
wrong-way **writer** produces a file that round-trips through our own reader
perfectly and shows every layer backwards in Photoshop. The round-trip test
cannot catch this. **Only an external check can** — see Verification.

### Layer rects: tight, not full-canvas

Real Photoshop layers are routinely stored at full canvas size with a
hand-sized patch of content, which is what
[[naturalpaint-psd-empty-tile-cost]] is about on the reading side. The writer
has the opposite freedom and should use it: emit the **tight bounding box of
occupied tiles**, clipped to the canvas. A fully transparent layer gets an
empty rect (`top==bottom`, `left==right`) and zero-length channels, which is
legal and is what Photoshop itself writes for an empty layer.

### Channel image data

One block per channel per layer, in the record's channel order, all after
every record. Each block is `u16` compression then the data — the same shape
as the Image Data Section but per channel. Alpha is channel `-1` and is
**straight, not premultiplied**: `io/Export.cpp:55` already establishes that
every format except EXR wants unassociated alpha, and the un-premultiply is
already written and guarded against `a == 0`.

RGB samples are `color::srgbEncode()` of the linear working value, the exact
inverse of the `srgbDecode()` the importer applies (`io/PsdImport.hpp`'s
"Colour space" section). Out-of-range linear values **clip** at 0 and 1 on
the way to 8-bit — and that clip is a warning with a number in it, per PRD
I11 and the precedent `io/ExportAs.hpp` sets ("which highlight value an
integer depth will clip").

### Masks — channel `-2` and the 20-byte record

`Layer::mask` writes as channel `-2` plus a 20-byte mask block in extra data:

```
i32 top, left, bottom, right     (absolute document coords)
u8  default colour               (0 = hide outside the rect, 255 = reveal)
u8  flags                        (0: bit 0 = rect is relative, bit 1 = mask disabled)
u16 padding                      (zero — this is what makes the record exactly 20)
```

Write **absolute** coordinates and leave flags at zero. The reader handles
the relative form (bit 0) but all ten real masks it was checked against sit
at the origin, where relative and absolute coincide — so writing relative
would be exercising a path nothing has ever confirmed. Choose the rect as the
tight bounds of coverage that is **not** 1.0, and set default colour to 255
so everything outside it reveals; that is the inverse of the empty-tile rule
`MaskTile` needs on the reading side, where the default is 1.0.

A mask block **larger** than 20 bytes means a second vector-derived mask; our
reader refuses it by name. The writer never emits one.

### Groups (`lsct`) — and the order that inverts

A naturalPaint `LayerKind::Group` becomes **two** PSD records, and they are
not in the order intuition suggests. Records run bottom-first, so:

1. the **divider** record (`lsct` type `3`) is written **first** — it opens
   the group from below;
2. the group's members, in `Document::layers` order;
3. the **header** record (`lsct` type `1` open / `2` closed) is written
   **last** — it closes and *names* the group.

`docs/psd-import-gaps.md` §3 records this as one of its three
"opens-without-error-and-is-confidently-wrong" traps, verified on two real
files. Getting it backwards inverts every group's membership silently. Both
synthetic records are empty layers: zero-size rect, zero-length channels, and
the header carries the group's `name` and `luni`.

The `lsct` block is `u32` type, then optionally `8BIM` + a 4-byte blend key.
Write `pass` (pass-through) for the header's key — `core/Composite.hpp:688`
makes a Group pass-through, so `pass` is the truthful key, not a default.

**Nesting is depth 0 in every file this project has ever checked.** Nested
groups are legal and the writer should emit them by recursion, but the test
for depth > 1 has to be a hand-written fixture — say so rather than claiming
real-file coverage.

### Names — write both

`luni` (a `u32` code-unit count then UTF-16BE) is what Photoshop has read
since 5.0 and what our importer prefers. The legacy Pascal name is still
mandatory in the record. Write both: the Pascal one as the name's bytes
truncated to 255 and padded to a multiple of 4 *including its length byte*,
the `luni` block as the full name. Non-ASCII names therefore round-trip
through `luni` while the Pascal field carries a lossy fallback — which is
exactly what Photoshop does.

## What cannot be carried, and must be named

PRD I11: a save that loses data names exactly what. Every one of these
rasterises into an ordinary PSD layer **and emits a warning naming the layer
and what it lost** — never a silent degradation:

| kind | what is lost |
|---|---|
| `Pigment` | pigment latents; the layer exports as its resolved RGB |
| `Adjustment` | the op stack — **and never as `curv`/`levl`** (see below) |
| `Strokes` | dab records; exports as its rasterised result |
| `Text` | editability; exports as pixels, not a `TySh` type layer |
| `Flats` | fill table and adjacency; exports as pixels |
| `Media` | whatever the kind holds beyond its raster |

**Never emit native `curv` or `levl` blocks.** PLAN.md:640 states the reason
and it is not a style preference: our curves live in the shaper log domain,
and a `curv` block would be read by Photoshop as a plain tone curve and be
silently, confidently wrong. Rasterise the adjustment into the layers below
it, or export the adjusted result — never translate the parameters.

**Photoshop's 16-bit range is 0–32768, not 65535**, and its 32-bit mode is
IEEE float. This landing writes 8-bit only (see above), so this trap is
deferred, not solved — leave the constant out of the code entirely rather
than writing a `65535` that a later 16-bit landing would have to find.

## Verification

Three layers of check, and the middle one is the one that cannot be skipped.

1. **Round-trip, in `--selftest`.** Build a `Document`, export, `importPsd()`
   it back, compare layer-for-layer: count, order, name, opacity, visibility,
   clipping, blend mode, rect, and the **mean straight linear RGBA** over
   covered pixels — the same comparison `app/PsdReport.cpp` already makes,
   for the same reason: geometry and alpha alone cannot catch a channel swap
   or a missing sRGB encode, and a mean can. Tolerance: 8-bit quantisation
   (~2e-3 in linear at the low end), not `rgba16float`'s 2.3e-4.

2. **psd-tools as an external oracle, and it is not optional.** A round-trip
   through our own reader is symmetric in exactly the places most likely to
   be wrong — the inverted visible bit, the stacking order, the group record
   order. Invert any of those in *both* directions and the round-trip stays
   green while the file is backwards everywhere else. `python3 -m venv
   psdvenv && ./psdvenv/bin/pip install psd-tools numpy` takes seconds
   (the recipe is already in this project's notes), and
   `PSDImage.open(p)._record.layer_and_mask_information.layer_info.layer_records`
   is the flat file record order to compare against. **Every track that
   writes a field listed as "verified against real files" above must show a
   psd-tools reading of its own output.**

3. **Open one in Photoshop.** The user has it and has supplied real files
   before. This is the only check that covers the Image Data Section
   actually being what other applications render.

**Sabotage, per this project's standing rule:** each headline assertion gets
its production line broken deliberately at gather time, by me, not by the
track's own report. The ones that matter most: flip `kFlagHidden` in the
writer (round-trip stays green — the psd-tools check must redden), reverse
the layer append order, and swap the `lsct` divider/header order.

## Tracks

Base for the wave is **`5881d5b`** — `main` plus one commit landing
`io/PsdWrite` (the big-endian writer, backpatched section lengths,
Photoshop's two string shapes) and `encodePackBits()`. Every track therefore
compiles against real byte primitives from its first build, and none of them
writes its own backpatch arithmetic.

**Four tracks scatter. Two do not**, and the reason is dependency, not size:

| track | branch | owns | files |
|---|---|---|---|
| **A** | `psd/container` | tier 1: file header, colour mode, image resources, empty layer section, Image Data Section — `writeFlattenedPsd()` | `io/PsdExport.{hpp,cpp}` |
| **B** | `psd/layers` | tier 2: layer records, channel data, `luni` + Pascal names, opacity/flags/clipping. **Assumes no masks and no groups** | `io/PsdLayerSection.{hpp,cpp}` |
| **C** | `psd/extras` | the mask block + channel `-2`, the `lsct` blocks, and the group divider/header expansion — as **standalone functions**, not edits to B's record writer | `io/PsdLayerExtras.{hpp,cpp}` |
| **D** | `psd/blend` | promote `kBlendKeyMap` to a shared header with a reverse lookup; correct io/PsdImport.hpp's stale "no equivalent for Overlay/Soft Light/Color" prose | `io/PsdBlendKeys.hpp`, `io/PsdImport.{hpp,cpp}` |

**E (capability + dialog + doc corrections) and F (the round-trip selftest)
are gather-time work, not scatter work.** E cannot flip `canWrite` until A's
symbol exists, and F's round trip needs A and B both. Dispatching them in
parallel would mean two agents writing against a function signature neither
can compile — which is how a wave produces three plausible variants of the
same call site. They land after the gather, against the merged tree.

**B and C are separated by FILE, not by function.** An earlier draft of this
plan had C editing B's `writeLayerRecord()`; that is how a `break;` gets
eaten by a union resolution. Instead C writes free functions taking a
`PsdWriter&`, B writes the record with no mask and no group, and **wiring
them together is done by hand at gather** — a dozen lines, written once, by
someone looking at both.

**Known collision points, to be checked by grep after every merge**, not
assumed from a clean merge: `src/CMakeLists.txt` (all four tracks add source
files to the same two lists), `main.cpp`'s aggregate `&&` chain, and every
tripwire constant a track may have bumped independently. A clean merge is not
a correct one — the last wave shipped a value-count guard that both branches
moved to 29 while the enum had become thirty.
