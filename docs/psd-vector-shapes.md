# Importing Photoshop shape layers as vector layers

A plan, in the shape `docs/psd-import-gaps.md` is in: every wire layout below
was dumped from a real file and cross-checked against psd-tools' parse, and
the claims that could **not** be verified that way are named as such rather
than left to look verified.

The file is Apple's `App Icon Template.psd` (1024x1024, 16-bit, RGB, PSD
version 1). It is the motivating case as well as the fixture: it opens today
and **twelve of its thirteen layers are empty**, because every one of them is
a shape layer whose raster is deliberately 0x0. The one layer with pixels is
a smart object.

This is the "opens without error and is confidently wrong" failure mode
`io/SvgImport.hpp` argues against, reached from the other direction.

## What already exists, and what that buys

`LayerKind::Vector` layers hold `std::vector<VectorShape>` (`core/Layer.hpp`),
rasterise through `rasterizeVectorLayer()` (`core/VectorRaster.cpp:94`) and
composite by being rewritten into RGB layers by `MaterializedDocument`. The
receiving model is finished and shipping:

| PSD shape layer carries | receives it | already works? |
|---|---|---|
| closed cubic subpaths | `Path` / `SubPath` / `Anchor`, `core/Path.hpp:107` | yes, and handles are absolute there too |
| a fill colour | `VectorShape::fill`, a linear straight-alpha `Paint` | yes |
| fill **disabled** (a guide shape) | `Paint::on == false` — "none" is distinct from alpha 0 | yes, exactly |
| a stroke with width/cap/join/miter/dash | `StrokeStyle`, `core/PathStroke.hpp:59` | yes, all of it |
| even-odd or nonzero coverage | `Path::rule`, both first class in `PathRaster.cpp:146` | yes |
| layer name, opacity, blend, visibility, clipping | generic `Layer` fields | unchanged from the raster path |

**No new layer kind, so no new dirty-tile whitelist entry.** The trap in
`core/DirtyTiles.cpp:254` — a parametric layer kind whose content is never
compared is invisible until something else forces a recomposite — is already
paid for `Vector` by `vectorContentHash()`. A PSD importer that produces
`LayerKind::Vector` inherits that. (See [[dirtytiles-parametric-whitelist]].)

What is **missing** and cannot be papered over: `Paint` has no gradient and no
pattern (argued at `core/VectorShape.hpp:33-49`), and there are no boolean
path operations anywhere in the tree — `app/PathOps.hpp:149`'s eleven verbs
are `Close … MakeCompound`, and none of them is union or subtract.

## The wire format, as dumped from the file

### Where the geometry is

Per-layer tagged blocks, `vsms` ("vector shape mask", 8 layers here) and
`vmsk` ("vector mask", 1 layer). Identical payloads:

```
uint32 version   (3 in every block in this file)
uint32 flags     (0 in every block in this file)
N x 26-byte path records
```

The block length is padded to a multiple of 4, so `192 = 8 + 7*26 + 2` — **the
record count must be derived from the payload length by division, and the
remainder discarded**, not assumed to divide evenly.

Record types seen: `6` path fill rule, `8` initial fill rule, `0` closed
subpath length, `1` closed knot linked, `2` closed knot unlinked. The
fill-rule records come first, before any subpath. Types 3/4/5 (open subpath)
and 7 (clipboard) do not occur in this file — an importer still has to decide
what to do with them, because any file with an open path has them.

### Coordinates

A knot record is `uint16 selector` then six `int32`, in the order
**(preceding.y, preceding.x, anchor.y, anchor.x, leaving.y, leaving.x)** —
vertical first, which is the field order most likely to be transposed by
someone working from memory.

Each `int32` is **signed 8.24 fixed point**: value / 2^24 is a fraction of the
document dimension. Worked example, verified byte for byte — `PNG/1 - Layer.png`
knot 0 reads `0x00194000 0x005CA75E 0x00194000 0x00800000 …`, and
`0x00800000 / 2^24 = 0.5 -> x = 512.0`, `0x00194000 / 2^24 = 0.0986 -> y = 101.0`.
The whole shape is a circle, centre (512, 357), r = 256.

The handles are absolute positions, which is what `core/Path.hpp`'s `Anchor`
already stores — no conversion, only a scale.

**Two things Apple's file could not prove — both now settled by a second
file.** It is square (1024x1024), so the divisor split was psd-tools'
convention rather than a measurement, and all 480 of its coordinates lie in
`[0, 2^24]`, so the field's signedness was never exercised.

`testNonSquareWithShapesOffPage.psd` (768x512, 8-bit, RGB) settles both at
once, and is the fixture to keep for them:

- **Vertical divides by height, horizontal by width.** `Ellipse 1` decodes
  under that split to x 493..666, y -189..-16, against psd-tools' bbox of
  (491, -191, 668, -14) — the 2 px inflation is the render's stroke and
  antialiasing. Swap the divisors and the same knots give x 328..444,
  y -283..-24: not near the bbox, and wrong in a way no render could hide.
- **The field is signed.** `Ellipse 1` sits entirely ABOVE the canvas; its
  knot y values are genuinely negative (`-6193152`, `-4924133`, `-524288`,
  `-1793307`). Read as unsigned, each is about 4.29e9, which is ~130,900 px
  down the page. `Star 1` crosses the top edge at y = -15.54, and its own
  `vogk` descriptor independently says `Vrtc = -15.5352` — a second witness
  in doubles rather than in fixed point.

That file adds three cases Apple's does not have: `strokeEnabled` is **true**
on all three shapes (1 px black, butt cap, miter join, centre aligned), so the
stroke path is exercised rather than assumed; `Star 1`'s ten knots are all
selector 2 with `in == pt == out`, a corner-only polygon; and its subpath
record's "unknown" field at offset 6 is **2**, not the 1 seen everywhere in
Apple's file, so nothing may key off that field's value.

One more colour warning from it: `Star 1`'s descriptor fill is
(14.45, 0.59, 104.53) and psd-tools renders it (25, 0, 108) — a drift of 10
units, not the 4 seen on Apple's file. The file carries an Apple *monitor*
profile rather than sRGB. Compare colour against the descriptor doubles.

### Which way the picture is made: the path operation

**It is not in a descriptor.** Grepping the whole 6.5 MB file for
`pathOperation` finds nothing. The operation is an `int16` inside the
26-byte *subpath length* record:

```
off 0   uint16 selector      (0 = closed, 3 = open)
off 2   uint16 knot count
off 4   int16  PATH OPERATION
off 6   uint16 unknown (1 here)
off 8   uint32 unknown (0 here)
off 12  uint32 origination index  (ties to vogk)
off 16  10 bytes zero
```

`App Icon Shape`'s two subpath records are `knots=4, op=1, index=0` and
`knots=44, op=2, index=1` — a full-canvas rectangle **minus** a squircle. Its
render is 57,136 opaque pixels, the four corners only.

Operation numbering (0 Exclude, 1 Union, 2 Subtract, 3 Intersect, -1 merge
with previous) is psd-tools' reading. What was actually verified here is that
op 2 renders as a subtraction; the rest is taken on psd-tools' authority and
should be treated as such.

### Where the colour is, and the trap in it

Two carriers, and **the common one is not the obvious one**:

- `SoCo` — a solid-colour fill block. One layer in this file has it.
- `vscg` — "vector stroke content". Eight layers have it and **no `SoCo` at
  all**. Its payload is a 4-character fill-type tag (`SoCo` here) followed by
  a byte-identical version word + descriptor.

So an importer that reads only `SoCo` imports one shape out of nine.

`vstk` (vector stroke, 832 bytes on those same eight layers) holds the stroke
style, and two booleans that decide whether any of this is drawn at all:

```
strokeEnabled  false   (on every layer in this file)
fillEnabled    true    -- and FALSE on `PNG/4 - Layer.png`
strokeStyleLineWidth 1.0 #Pxl, miterLimit 100.0,
strokeStyleLineCapType strokeStyleButtCap, ...LineJoinType strokeStyleMiterJoin,
strokeStyleLineAlignment strokeStyleAlignCenter, LineDashSet []
```

**`PNG/4 - Layer.png` carries an orange `vscg` colour and is invisible**: both
`fillEnabled` and `strokeEnabled` are false. An importer that reads the colour
without reading those flags paints a solid orange circle Photoshop does not
show — a wrong document that looks deliberate. `Paint::on = false` is the
exact receiving field, so this costs nothing to get right and is silent to get
wrong.

`strokeStyleLineAlignment` has no receiving field: `PathStroke` centres every
stroke. Inside/outside alignment is a refusal line, not a guess.

### What must NOT be applied: `vogk`

`vogk` carries a `Trnf` matrix, and applying it is wrong. `App Icon Shape`'s
`vogk` says `xx=1.032258, tx=-6.193548 …`, while its `keyOriginShapeBBox` and
its decoded path both say exactly 0..1024. Applying the matrix moves the shape
off the canvas. `vogk` is **origination history** — how the live parametric
rectangle was scaled since it was created — and six of the nine path-bearing
layers have no `vogk` at all yet decode correctly. The path stream is
self-sufficient; the paths are already in document space.

## The steps

Each step is independently verifiable, and the order puts the two things that
can be quietly wrong — geometry and fill selection — first.

### 1. Decode the path record stream into `core::Path`

New `io/PsdVectorPath.{hpp,cpp}`: bytes + document size in, `std::vector<Path>`
(one per subpath group) + the per-subpath operation out. No dependency on the
rest of the importer, so it can be tested on its own.

- Derive the record count by division; discard the pad remainder.
- Knots: `(in.y, in.x, pt.y, pt.x, out.y, out.x)`, each `int32 / 2^24`, scaled
  by document height/width respectively.
- A closed `SubPath` is `closed = true` with **no repeated final anchor**
  (`core/Path.hpp:124` — the closing segment is implied). PSD's knot list has
  the same convention, so this is a straight copy, not a fix-up.
- Selector 2 (unlinked) versus 1 (linked) maps to `Anchor::smooth`, which is
  an editor hint and affects nothing geometric.
- Guard with `pathIsFinite()` (`core/Path.hpp:193`) before anything downstream
  touches it — the rasteriser's documented precondition, and this is untrusted
  input.

Fixtures: hand-built record streams in `app/selftest/`, plus the circle above
as a known-answer test (centre 512,357, r 256, four knots).

### 2. Turn the operations into one `Path` with a fill rule

This is the step with a real limit in it, and it should be sized before it is
started rather than discovered halfway.

naturalPaint has **one fill rule per `Path` and no boolean ops**. That is
enough for three of PSD's four operations and not for the fourth:

| PSD ops on a layer | expressible as | sound? |
|---|---|---|
| all Union | one compound `Path`, `NonZero` | yes — same-winding overlap unions under nonzero |
| Union + Subtract | compound `Path`, `NonZero`, subtracted subpaths **reversed** | yes for nested/disjoint holes (the font and SVG convention); **not** where the subtracted region is covered twice |
| all Exclude | compound `Path`, `EvenOdd` | yes — exclude *is* XOR |
| any Intersect | nothing here expresses it | **no** |

So: implement the first three, and refuse Intersect (and a layer mixing
Exclude with the others) by name in `PsdImportResult::warnings`, naming the
layer. The alternative — a real path booleans pass — is a much larger piece of
work that belongs to `app/PathOps` and the PATHS panel, not to an importer.

`App Icon Shape` is the Union+Subtract case and is the acceptance test: a
1024x1024 rectangle minus a squircle must rasterise to ~57,136 opaque pixels
in the four corners, with the centre transparent.

### 3. Read the fill and stroke descriptors

`io/Descriptor.hpp`'s `parseVersionedActionDescriptor()` takes exactly the
framing these blocks use, at a fixed offset per key:

| block | skip before the descriptor |
|---|---|
| `SoCo` | 0 |
| `vscg` | 4 (the fill-type tag) |
| `vstk` | 0 |
| `vogk` | 4 (a block version word) |

Every osType in these blocks (`Objc doub VlLs long bool UntF enum TEXT`) is in
the parser's set, and **none of the four it refuses by name** (`obj `, `ObAr`,
`UnFl`, `Pth `) occurs. That is a check of the grammar, not a run: nobody has
fed these bytes to that parser yet, and `io/Descriptor.hpp:210-220` is still
honest that the module is unproven against a real PSD. **Step 3 is where that
claim gets tested, so do it early and loudly.**

Precedence, matching psd-tools' compositor: `SoCo`, then `PtFl`, then `GdFl`,
then `vscg` — gated on `vstk.fillEnabled`. Colour arrives as `RGBC` doubles in
0..255, sRGB-encoded; `Paint::rgba` is linear straight alpha, so it goes
through `color::srgbDecode()` exactly as `io/SvgImport` does.

`GdFl` and `PtFl` do not occur in this file. They have no receiving field, so
they are a named refusal (the layer imports with `fill.on = false` and a
warning naming the layer and the fill kind) rather than a flat colour guessed
from a gradient stop.

### 4. Emit a Vector layer

In `io/PsdImport.cpp`, where a layer is built today (`layer.kind =
LayerKind::RGB; layer.rgbTiles.emplace();`), a record carrying vector geometry
**and** a fill becomes `makeVectorLayer()` with `shapes` instead. Everything
else — name, opacity, visibility, `clipped`, `alphaLocked`, blend — is
unchanged, because none of it is per-kind.

Two shapes of input, and they are different features:

- fill block + `vsms`/`vmsk`, raster empty → **a shape layer**. Vector layer.
- real pixels + `vmsk` → **a raster layer with a vector mask**. That is
  `Layer::mask` (rasterise the path into it), not a Vector layer, and it is a
  separate piece of work. `channel -3` is already walked past deliberately at
  `PsdImport.cpp:1322`.

Shape ids are assigned at the `app/OpenAnyFile.cpp` call site, the way SVG's
are (`:262`) — the importer leaves `id = 0`.

### 5. The things that will look wrong if step 4 lands alone

- `app/PsdReport.cpp` measures tiles, and a Vector layer has none. Its "EMPTY"
  column would then be reporting the opposite of the truth. It needs a vector
  branch — shape count, and the bounds from `vectorShapesBounds()`.
- `io/PsdExport` round-trips documents back to PSD. What it does with
  `LayerKind::Vector` today needs checking before this lands, not after: a
  silent rasterise-on-export is defensible, a silent drop is not.
- `.npaint` round-trip is already handled — `io/PathSerial`'s `npvec1:` writes
  these structures today.

### 6. Verification

The oracle is the same one `docs/psd-import-gaps.md` used, plus one addition:
psd-tools renders vector shapes only with **aggdraw** installed (`pip install
aggdraw` — it is not a psd-tools dependency and the import error is the only
hint). With it, every shape layer in this file rasterises, and those renders
are the per-layer comparison target.

Known-answer renders from this file, all measured:

| layer | expected |
|---|---|
| `Background (Do not export)` | 1,048,576 opaque px, (245,245,245) |
| `PNG/1`, `SVG/1` | circle centre (512,357) r 256, (192,204,216) |
| `PNG/4` | **nothing drawn** — fill disabled (psd-tools renders it anyway; see below) |
| `App Icon Shape` | 57,136 opaque px, black, four corners only |
| `SVG/4 - Layer.svg` | **nothing** — it is a plain empty pixel layer with no vector data at all, unlike its three siblings |

That last row is worth keeping: an importer that produced four shapes for the
SVG group would be wrong in a way that looks right.

Note the fill colours above are the descriptor's own doubles. psd-tools'
*render* of `PNG/1` comes back (188,203,216) rather than (192,204,216), and
its render of `testNonSquareWithShapesOffPage.psd`'s `Star 1` drifts 10 units
on red. **Compare colour against the descriptor, never against the render.**

**And the render is not a safe coverage oracle either, on precisely the row
that matters most.** `psd_tools`' per-layer `ShapeLayer.composite()` does not
consult `vstk.fillEnabled`: it renders `PNG/4 - Layer.png` as 205,452 opaque
pixels — the same count as its enabled sibling `PNG/2`, though painted white
rather than the layer's own orange. Photoshop draws nothing at all for that
layer, and the file's own saved flattened composite is the arbiter: it
contains **zero** pixels of `PNG/4`'s orange.

So on the one layer whose whole purpose is to catch the `fillEnabled` trap,
the oracle says "fully covered" where the truth is "nothing drawn", in a
direction that pushes an implementer straight into the trap. Use the render
for geometry on layers whose fill is enabled; use the **saved composite** and
the descriptors for anything else.

## Still open after steps 1-5 landed

Steps 1 through 4 imported a Photoshop shape layer. A second wave closed S1,
S4 and S5 and left S2 and S3, which are the two that need engine work rather
than importer work. What follows is the current state, not the plan.

### S1. Shape layers now export as shape layers. CLOSED

`writePsd()` used to hand the RAW document to `writePsdLayerAndMaskInfo()`
while taking its composite from `flattenDocumentToLinear()`, so a PSD written
from a document with shape layers carried an empty layer record and a merged
image that showed the shape. It now writes both halves of what Photoshop
writes: a real `vsms` path block plus a `SoCo` fill descriptor, **and** a
rasterised cache in the layer's own channel data. A reader that understands
shapes gets editable geometry; one that does not gets correct pixels.

io/PsdVectorWrite is that encoder, and its `SoCo` output is **byte-identical
to the block Photoshop wrote for `App Icon Shape`**, which settles both of
io/Descriptor.hpp's documented quirks from the writing side: a four-character
Key writes a length of **zero**, and a UnicodeString's trailing NUL is counted
**inside** its length. Note what that assertion does and does not prove --
the fixture colour is black, and `srgbEncode(0) == 0`, so byte-identity
confirms the descriptor framing and not the transfer function. The colour path
is covered by the separate round-trip assertion instead, which is the one that
fails when the encode is wrong.

**What a shape layer written by this code still loses**, all of it warned
about by name rather than silent:

- **Shapes past the first.** A PSD shape layer is exactly one path; the rest
  survive only in the raster.
- **Stroke and stroke style**, and `Paint::on == false` ("no fill"), and
  anything else carried in `vstk` -- not written at all. A fill-less shape
  gets `vsms` and no `SoCo`, so a reader that decides "shape layer" by looking
  for a fill block takes the raster instead.
- **Fill alpha.** PSD's shape colour has no alpha field; only the layer's own
  opacity carries transparency.
- **Clip path, and per-shape `name`/`id`/`pivot`** -- no PSD field exists.
- **Labelling, not appearance**: a NonZero compound's hole comes back marked
  Union rather than Subtract (the winding does the work, and it draws
  identically), and a **single-subpath** EvenOdd path comes back NonZero,
  because `composePsdSubPaths()` votes on the rule using subpaths 1..n-1 and a
  lone subpath casts no vote. Identical rendering for anything that does not
  self-intersect.
- **No `vmsk` beside `vsms`**, deliberately: `vmsk` is also how a vector mask
  on a raster layer is stored (S4), and a reader taking it that way would clip
  the outer half of a centred stroke out of the raster written next to it.

### S2. Gradient and pattern fills have nowhere to land. STILL OPEN

`GdFl` and `PtFl` are now recognised wherever they occur -- standalone as well
as embedded in `vscg`'s fill-type tag -- and are a named warning with
`fill.on = false`. Recognising them is all that can be done from here: where
Photoshop cached a raster the importer falls back to it, so the picture
survives and the editability does not.

`core/VectorShape.hpp:33-49` argues the receiving field: a paint kind plus an
index into a **document-level gradient table**, reusing `ops/Gradient.hpp`
rather than duplicating it, so the heavy type sits at the table and not in
every layer. That is a `core/` change, not an importer one, and `io/SvgImport`
wants exactly the same field.

### S3. Intersect is refused by name. STILL OPEN

Union, Subtract and Exclude all fall out of one compound path plus a fill
rule. Intersect does not, and `composePsdSubPaths()` refuses the layer rather
than guessing. Closing it means real boolean path operations, which belong to
`app/PathOps` and the PATHS panel -- `app/PathOps.hpp:149`'s eleven verbs are
`Close … MakeCompound` and none of them is a boolean. A layer mixing Exclude
with the others is refused for the same reason and falls out of the same work.

The double-coverage soundness heuristic is the other thing exact boolean ops
would finish: it is now **tight** bounds per subpath rather than control-point
hulls, which strictly shrinks the false-positive set without ever hiding a
real overlap, but a bounding box is still not an intersection test.

### S4. A vector mask on a raster layer. CLOSED

`vmsk`/`vsms` + real pixels + **no** fill block is a raster layer wearing a
vector mask. The outline is now rasterised into `Layer::mask` rather than
dropped: the layer stays `LayerKind::RGB`, and the path becomes what reveals
its pixels instead of becoming content of its own.

Two things worth knowing:

- **Combining with a raster mask (channel −2) on the same layer MULTIPLIES.**
  Photoshop composites through the intersection of the two. No sample file
  carries both at once, so this is a stated design decision rather than a
  measurement, and `--selftest` asserts it rather than leaving it to be
  discovered.
- **The discriminator widened.** `hasFillBlock` counts `PtFl` and `GdFl` too,
  not just `SoCo`/`vscg`. Without that, the first real file with a
  gradient-filled shape would route here and import as a raster wearing its
  own outline as a mask -- a layer that looks almost right. That case only
  became reachable when this wave started slicing those two blocks.

Still verified only against hand-built fixtures: no sample file has a vector
mask on a raster layer.

### S5. The smaller ones. CLOSED, except one that is not ours

- **Standalone `PtFl`/`GdFl`** are recognised and named -- see S2.
- **Non-`#Pxl` stroke units.** `strokeStyleLineDashOffset` arrives as `#Pnt`
  in real files, and its number used to be used **unconverted**, which was a
  wrong value presented as a right one. It is now dropped to zero and warned
  about by name. Zero stays silent, in any unit, because zero is zero
  everywhere. A real conversion needs `strokeStyleResolution`, which has no
  receiving field.
- **Open subpaths.** `PsdPathStream::sawOpenSubPath` was set and nothing acted
  on it, and the honest answer turned out to be that nothing should:
  `core/PathFlatten` propagates `SubPath::closed` faithfully, `core/PathStroke`
  branches on it (an open contour gets end caps and no seam-joining edge), and
  `core/PathRaster` implicitly closes a contour for **filling**, which is what
  Photoshop and SVG both do. Both consumers already match Photoshop, so a
  warning would report nothing true. Asserted in both directions rather than
  only commented: an open three-sided square fills at its centre, and stroking
  the same anchors leaves the fourth side bare while the identical anchors
  marked closed do stroke it.
- **`strokeStyleLineAlignment`** other than centre is the one still open, and
  it is not importer work: `core/PathStroke` centres every stroke, so inside
  and outside alignment warn rather than guess.

### What is NOT left

Worth stating so nobody re-opens it: the coordinate encoding is settled
against a second file, `io/Descriptor` is proven against real Photoshop bytes
in **both** directions, the `fillEnabled` trap is asserted, `vogk`'s `Trnf` is
correctly ignored, and the three modules chained on one real layer match
psd-tools' render to 0.04%.

**psd-tools is no longer installed on the machine this was developed on.**
Every fact above that names it as an oracle was established while it was; a
future session must either reinstall it or reason from the bytes, and the
Python probes that read the layer records and image resources directly are the
cheaper of the two.

## What this does not cover

`lfx2` (layer effects) does not occur in this file at all — zero occurrences
in 6.5 MB — so nothing here is evidence about it, and `Layer` has no field for
it either way. Smart objects (`PlLd`/`SoLd`/`lnk2`, the `Grid` layer) import
as raster today and are out of scope; the embedded file is a 1024x1024 8-bit
**PSB**, which this build refuses by name, but its flattened raster is in the
layer data and reads correctly now that ZIP does.
