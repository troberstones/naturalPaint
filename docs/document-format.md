# Document format — `.npaint`

A **multi-part, tiled, mip-mapped OpenEXR** file with a `.npaint` extension. One part per
layer, `HALF` channels, pigment latents as first-class named channels, everything else as
typed header attributes.

Supersedes an earlier decision to use a PSD container. That reversal is explained in §7.

---

## 1. Why EXR

Four of EXR's native features are exact matches for decisions already made elsewhere in
this design, which is unusual enough to be the whole argument.

| our decision | EXR feature |
|---|---|
| Working space is `rgba16float` | `HALF` channels — byte-identical, no conversion |
| Linear light | EXR is linear by convention; `chromaticities` declares primaries |
| 128² tiles + a display mip pyramid | native tiled, mip-mapped storage, per part |
| Layers allocate only where content exists | per-part **data window** |
| Latents are 7 channels alongside RGB | arbitrary **named channels** in the same part |

And the decisive practical point:

> **OIIO writes multi-part tiled EXR.** Native save needs *zero* bespoke writer code, so
> it lands in phase 4 with the rest of I/O. Under the PSD plan the writer was ~2–3k
> hand-written lines scheduled for phase 15 — meaning eleven phases of an application that
> could not save its own documents. That was a defect, not a trade-off.

A further consequence: because the on-disk layout is tiled and mip-mapped, **OIIO's
`ImageCache` becomes the residency layer for our own documents**, not only for imported
files. ADR-0001's lazy-residency model is then served by machinery already in the plan.

> ✅ **Measured, 2026-08-19, while implementing PLAN.md phase 4 step 5: this claim holds,
> and it holds *better* for our own documents than for imported files.** `io/TileResidency`
> opens a `.npaint` layer part as an OpenImageIO `ImageCache` subimage and serves its
> 128² tiles on demand; all 256 tiles of a 2048×2048 document come back **bit-identical**
> to the eager path (`memcmp` of all 128 KiB, zero tolerance — `HALF` in, `HALF` out, no
> conversion stage), at **73 µs cold and 5.6 µs warm** per tile, with our own resident cost
> falling from **32.00 MiB to 0.12 MiB** (one staging tile). Part 0, the composite, is
> cache-addressable on the same terms. `--selftest`'s `tile residency` section is the proof.
>
> Two corrections to the wording, though, both measured:
>
> - **"and mip-mapped" is not doing any work here, because nothing writes a pyramid.**
>   The residency runs entirely at miplevel 0. Tiling is the whole of what makes this
>   possible; the pyramid would only matter for a zoomed-out fetch, and §1's table pairs
>   it with a display pyramid that lives on the GPU side.
> - **"not only for imported files" reads as if imports were the easy case. They are the
>   case that does not work.** PNG/JPEG/TGA/BMP — every format `io/ImageDecode` handles —
>   are scanline-stored, and OpenImageIO's cache gives them no partial residency: with
>   `autotile` off, one 128×128 request against a 2048×2048 PNG pulls the **whole 16.00 MiB
>   image** into the cache (15.9 ms); with `autotile=128` the memory is bounded but
>   scattered cold tiles cost **1549 µs each** against 49 µs for a tiled EXR, because the
>   decoder restarts to reach a scanline it has passed. `io/TileResidency` therefore
>   refuses untiled sources by name and leaves them on the eager path. The tiled on-disk
>   layout this section argues for is not merely *compatible* with the cache — it is the
>   only reason the cache is usable at all.

---

## 2. File layout

```
part 0   "composite"      R G B A                    ← any EXR reader shows this
                          tiled, mip-mapped
         attrs:  chromaticities
                 np:version      1
                 np:basis        "mixbox-v1"
                 np:tileSize     128
                 np:docOps       <blob>
                 np:paths        <blob>
                 np:comps        <blob>   layer comps: name + per-layer state

part 1   "L0001"          R G B A                    ← baked projection
                          pig.c0 pig.c1 pig.c2 pig.m
                          res.R res.G res.B
                          mask
         attrs:  np:kind        "pigment"
                 np:name        "Line pass"
                 np:blend       "multiply"
                 np:opacity     0.72
                 np:visible     1
                 np:locked      1
                 np:parent      ""
                 np:ops         <blob>

part 2   "L0002"          R G B A + pig.* + res.*
         attrs:  np:kind        "media"
                 np:medium      "watercolour"
                 np:simParams   <blob>

part 3   "L0003"          (no image channels)
         attrs:  np:kind        "strokes"
                 np:dabs        <blob>

part 4   "S0001"          coverage                   ← a saved selection
         attrs:  np:kind        "selection"
```

**Part order is layer order**, bottom to top, after part 0.

### Details that bite

- **Part names must be unique**, and EXR requires a `name` on every part in a multi-part
  file. Layer names are not unique — two layers may both be "Layer 1" — so the part name
  is a stable synthetic id (`L0001`) and the user-facing name lives in `np:name`.
- **Groups have no native concept.** A group is a part with no image channels and
  `np:kind="group"`; members carry `np:parent` naming it.
- **Some attributes must match across all parts** — `displayWindow`,
  `pixelAspectRatio`, `chromaticities`. Document-level `np:*` attributes live in part 0.
- **Use only OIIO-representable attribute types**: `string`, `int`, `float`, and
  `UINT8[n]` for blobs. This avoids registering custom EXR attribute types, which OIIO
  would otherwise skip on read.

  > ⚠️ **Measured, 2026-08-19, while implementing this: `UINT8[n]` does not work.**
  > Written through this project's OpenImageIO, a `TypeDesc(UINT8, 5)` header attribute
  > is simply **absent** when the file is read back — no error, no warning. The same is
  > true of `INT32[n]` and of every other array type tried; `string`, `int` and `float`
  > all survive. (Re-measured independently: the one apparent exception is an *exactly
  > three*-element int array, which survives only because OpenEXR coerces it to a `v3i`
  > and it reads back as `vectori` -- a different type from the one written, not working
  > array support. `INT32[5]` is absent like the rest.) So **there is no working blob
  > carrier today**, and every blob this
  > document names — `np:ops`, `np:dabs`, `np:comps`, `np:paths`, `np:docOps`,
  > `np:simParams` — needs one before it can be written. The cheap fix is a base64 or
  > hex **`string`** attribute; the expensive one is writing the header through OpenEXR
  > directly instead of OpenImageIO. `io/NpaintFile` refuses a blob attribute by name
  > rather than writing a file that quietly lacks it.
  >
  > A smaller one from the same measurement: an **empty** `string` attribute is dropped
  > too. Harmless where the reader's default for that attribute is the empty string
  > (`np:name`, `np:parent`), but it is luck, not design.

> ✅ **Implemented, 2026-08-19, at PLAN.md Phase 5 step 3: a Pigment layer's part is
> written and read with all eleven channels above.** The seven stored ones (`pig.c0
> pig.c1 pig.c2 pig.m`, `res.R res.G res.B`) are `core::PigmentTile`'s own `HALF` words
> moved with no float stage, so `--selftest` asserts them bit-identical at zero tolerance;
> `R G B A` is the baked projection, written for other tools and **ignored on read**, for
> the same reason part 0 is. Two notes from doing it:
>
> - **The seven channels are six latent floats plus mass, and the fourth pigment weight is
>   *not* among them.** `np::Latent` holds `c0..c2` and `res.R/G/B`; Mixbox's fourth weight
>   is `c3 = 1 - (c0+c1+c2)`, derived on every use. `pig.m` is the seventh, and it is not
>   part of the latent at all — it is the Pigment analogue of alpha, the quantity PRD F10's
>   eraser reduces. Nothing invents a seventh stored float.
> - **Channel order on read is OpenImageIO's, not OpenEXR's, and neither is a contract.**
>   Measured: a part written in the order above reads back in exactly that order — which is
>   *not* what `Imf::ChannelList` stores (it is a name-sorted map, in which `res.B` precedes
>   `res.R`), so it is OIIO normalising per EXR layer name. A positional read would work
>   today by luck and would silently swap the residual's red and blue if that ever changed.
>   `io/NpaintFile` matches by name.
>
> `np:basis` stops being inert here: a document holding Pigment layers whose carried
> `np:basis` is not this build's is **refused** on save, which is §3.3's own listed case.

> ✅ **Implemented, 2026-08-20, at PLAN.md Phase 5 step 4: the `mask` channel above carries a
> real layer mask.** One `HALF` channel of per-texel coverage, so an RGB layer part is
> `R G B A mask` and a Pigment one is the eleven above plus `mask`. Four notes from doing it:
>
> - **It is written only when the layer actually has a mask**, and that is a compatibility
>   property rather than a saving. A document whose layers carry no mask produces the part
>   this build produced before masks existed — measured, not assumed: every `.npaint`
>   `--selftest` writes is byte-identical between HEAD's binary and this one once
>   OpenImageIO's `capDate` header string is masked out, which is the only place HEAD's own
>   two consecutive runs differ as well.
> - **1.0 means reveal, and an absent mask tile means 1.0.** So the drop-on-read rule for
>   this channel is "every sample is exactly 1.0", the mirror of the "every word is zero"
>   rule the RGB and Pigment channels use — same rule, the identity element the channel
>   actually has. Had it been 0, a mask painted on one tile of a four-tile layer would blank
>   the other three on reload.
> - **A mask is per-texel *opacity*, never pigment mass.** On a Pigment part, `mask` and
>   `pig.m` are different quantities in different channels: `pig.m` is what PRD F10's eraser
>   reduces, `mask` is what PRD C3's transparency scales. A reader that multiplied one into
>   the other would change the pigment mixture rather than the coverage.
> - **A mask sample outside [0,1], or NaN, is clamped on load and named in a warning with a
>   count** (PRD I11). The clamped values are what the document then holds and what the next
>   save writes; a mask is the one channel where a bad sample makes a whole layer disappear,
>   so it must not be absorbed silently.
>
> The layer part's baked `R G B A` is deliberately **unmasked** as well as ungraded — it is a
> projection of what the layer stores, and the mask sits beside it in its own named channel.
> Part 0 is where another tool gets the masked composite, because part 0 comes from the
> flattener.

- **Every part must agree about being tiled.** Also measured: OpenImageIO cannot write a
  multi-part EXR mixing tiled and scanline parts — it fails partway through with
  `Can't build a TiledOutputFile from a type-mismatched part`. Since part 0 is tiled,
  every part is.
- **Compression must be lossless.** `ZIP` for general use, `PIZ` for grainy content.
  **Never `DWAA`/`DWAB`/`B44`** in a working file — they are lossy, and a working file is
  the one place that is unacceptable.

---

## 3. Guaranteeing no data loss

Five mechanisms. The extension is the weakest of them.

### 3.1 The extension is a contract

| extension | contract |
|---|---|
| **`.npaint`** | Native. Full fidelity. |
| `.exr` | The same bytes, renamed — pipeline-ready for 3D work. |
| `.psd` | Deliberate export for Photoshop users. Simply-layered, lossy by design. |

`.npaint` and `.exr` are the *same container*, so "give this to the 3D pipeline" is a
rename. That is the property PSD was originally chosen for, and EXR has it for the
audience that actually matters here.

### 3.2 Preserve unknown attributes verbatim

> **The important rule.** Any `np:*` attribute or part the reader does not recognise is
> retained and written back unchanged.

This is forward compatibility by preservation: an older build can open a newer document,
edit what it understands, and save without destroying what it does not. Without it,
"upgrade, then open in an older build" is a data-loss event.

EXR makes this *easier* than PSD would have. OIIO surfaces every attribute in
`ImageSpec::extra_attribs`, so retaining unknown ones is iteration over a list rather
than a parser that has to know block layouts in advance.

### 3.3 Never degrade silently on save

If a save would lose something, the dialog names exactly what: a `.psd` export dropping
latents and Media wet state, a 16-bit export clipping above-white, a basis mismatch.
Silence is the failure mode.

### 3.4 The composite part is never stale

Part 0 is what every other tool renders. If it disagrees with the layer parts, other
tools show something subtly wrong and nobody notices for months. Regenerate on every
save, unconditionally.

### 3.5 Degradation is graceful — and better than PSD's

This was the strongest argument *for* PSD, and EXR wins it outright.

- Any EXR reader shows part 0, the correct composite.
- Nuke, Blender and Clarisse read every part as a layer, with all channels **named**.
- If naturalPaint's own reader ever breaks, a recoverer sees `pig.c0`, `pig.m`, `res.R`
  as **self-describing named channels** — not an opaque private blob whose layout exists
  only in our source.

A PSD-container file hid the latents in `nPlt` blobs. EXR leaves them legible. For a
format expected to hold years of work, that is a materially better failure mode.

---

## 4. What EXR cannot do

Stated plainly, because these are real.

**Photoshop sees a flattened image.** Its EXR support is single-part and poor. Layer
parts are invisible to it.

**Layer semantics are ours alone.** `np:blend`, `np:opacity`, `np:parent` are custom
attributes no other tool honours, so another application sees N images with no
compositing recipe. PSD's blend modes *are* portable.

That second point is softer than it looks, though: our blend set is linear-space and
includes `Mix`, which **PSD cannot express anyway**. Layered-PSD fidelity was always going
to be partial, so the portability being given up is less than it appears.

---

## 5. PSD export

A separate, deliberate feature — not the save path.

| target | effort | fidelity |
|---|---|---|
| Flattened PSD | small | the composite, correct |
| Simply-layered PSD | ~800–1200 lines | one PSD layer per naturalPaint layer, blend modes mapped where they exist, latents dropped |
| Full-fidelity layered PSD | ~2–3k lines | not planned — see §4 |

Most PSD handoff wants the first two. That is why the sunk-cost argument for writing a
full PSD writer collapsed: the hard version serves a case nobody asked for.

> ⚠️ If layered PSD export is written, **do not emit native `curv` / `levl` adjustment
> blocks.** Our curves are authored in the shaper log domain (ADR-0004); Photoshop's are in
> the document's gamma space. A natively written block would be *silently wrong* rather
> than approximately right. Rasterise the effect.

---

## 6. EXR quirks worth knowing

- **`HALF` maxes out around 65504.** Fine for scene-linear values, but a saturating
  operation could produce `inf`, which EXR stores happily and which then poisons any
  downstream average. Clamp on write.
- **Data window vs display window.** The display window is the canvas; a layer's data
  window may be smaller *or larger*. Readers that ignore the distinction will crop or
  mis-place layers.
- **Tile size on disk need not equal `kTileSize`.** They should match, so a load is a
  direct read of the tiles needed, but nothing enforces it — assert it.
- **Multi-part is EXR 2.0+ (2013).** Any current reader handles it; some very old tools
  do not.

---

## 7. Why this reverses the PSD decision

PSD was chosen for distribution rather than technical merit: a PSD opens everywhere, and
a file nobody else can open is a social dead end. Three things undid that reasoning.

**The critical path.** PSD-native put a 2–3k line hand-written writer between the
application and its ability to save. EXR-native removes it entirely.

**The audience split the wrong way.** The primary user's job is texture and photo prep for
3D, and that world is **EXR-native** — Substance, Blender, Houdini, Nuke and Unreal all
prefer it. PSD is the visdev handoff format, which is an export concern either way.

**The interop win was partly illusory.** Layered PSD cannot express our blend set, and
Photoshop would drop our private blocks the moment it rewrote the file. What survived a
Photoshop round-trip was the baked composite — which EXR also provides, in part 0, to
every reader.
