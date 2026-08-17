# Operation catalogue

Every image operation the application offers, each tagged with the **op class** that
decides how it is evaluated and what it costs. Classes are defined in
[DESIGN-imaging.md §4](../DESIGN-imaging.md):

| class | kind | evaluation | cost of adding one more |
|---|---|---|---|
| **A** | parametric point | folded into the shaper + 3-D LUT | ~zero |
| **B** | parametric spatial | its own pass, ROI-bounded | one pass over the ROI |
| **C** | recorded stroke | replayed from stored geometry | one replay |
| **D** | baked | copy-on-write into tiles | one tile write |

The class is not cosmetic. **Class A is free and class B is not**, so which column an
operation lands in decides whether a stack of thirty is viable. Anything that needs a
neighbourhood — even a 3×3 — is class B, and a surprising number of controls that look
like point operations are not.

Priorities are the PRD's: **P0** ships in the milestone that names it, **P1** is
committed, **P2** is wanted.

---

## 1. Colour correction

All class A unless marked. They collapse together, so a document carrying twelve of
these costs exactly what one costs — see [ADR-0004](adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md).

### 1.1 Committed (PRD D1)

| op | class | P | notes |
|---|---|---|---|
| Levels | A | **P0** | black/white/gamma per channel and composite |
| Curves | A | **P0** | authored in the **shaper domain**, not linear — ADR-0004 |
| Exposure | A | **P0** | stops; a pure multiply in linear |
| Saturation | A | **P0** | against a stated luma weight, not a naive average |
| RGB → grayscale | A | **P0** | Rec.709 luma default, weights exposed |
| Channel mixer | A | **P0** | 3×4 matrix including offsets |

### 1.2 Committed additions

Each is a new set of parameters feeding the same LUT bake. That is why this list can be
long without being expensive.

| op | class | P | notes |
|---|---|---|---|
| Gain / offset / gamma | A | **P1** | the Nuke primitive; the honest form of "brightness/contrast" |
| White balance | A | **P1** | temperature/tint via chromatic adaptation, not a channel scale |
| Colour balance | A | **P1** | lift / gamma / gain by tonal range — the grading control |
| Hue / saturation by range | A | **P1** | HSL qualifier with soft range edges |
| Black & white | A | **P1** | six-channel weights, as the mono-conversion control |
| Gradient map | A | **P1** | luma → gradient; class A because the input is one scalar |
| Invert | A | **P1** | in linear or display domain — the choice is visible, so expose it |
| Clamp | A | **P1** | needed once `HALF` `inf` is possible on save |
| Posterize | A | **P2** | quantise in the shaper domain or it bands unevenly |
| Threshold | A | **P2** | |
| Selective colour | A | **P2** | per-primary CMYK offsets |
| Vibrance | A | **P2** | saturation weighted by existing saturation |
| Apply LUT (`.cube`) | A | **P2** | a 3-D LUT composed into ours — the cheapest op in the list |
| Auto-tone | A | **P1** | per-channel black/white points from the histogram |
| Auto-contrast | A | **P1** | composite black/white points, so hue is preserved |
| Auto-white-balance | A | **P1** | grey-world or brightest-neutral estimate |

Auto-anything is a **parameter solver, not an op**: it inspects the histogram, computes
levels or balance parameters, and writes them into an ordinary editable op. The user can
then adjust what it chose, which is the whole reason to build it that way.

### 1.3 Not class A, despite appearances

Calling these out because filing them under colour correction and discovering the cost
later is the mistake this table exists to prevent.

| op | class | P | why it is spatial |
|---|---|---|---|
| Shadows / highlights | **B** | **P1** | needs a blurred luminance guide; it is local tone mapping |
| Clarity / local contrast | **B** | **P1** | unsharp on luminance only |
| Vignette | **B** | **P2** | a function of position, so it cannot be a per-pixel LUT |
| Dodge / burn | **C** | **P1** | see below |

> **Dodge and burn are not operations — they are a brush painting into a mask.**
> Photoshop's versions destructively modify pixels. Here, the tool paints coverage into
> the mask of an exposure adjustment layer, which makes it non-destructive, re-editable
> and adjustable in strength *after* the fact. It costs nothing new once phase 5
> (adjustment layers, masks) and phase 10 (brush) exist, and the result is strictly
> better than the incumbent's. The palette entries in
> [docs/ui.md §4](ui.md) wire to this, not to a pixel op.

---

## 2. Spatial filters

Class B throughout. Each is a real pass whose cost scales with area and radius, so this
list is ordered by what earns its cost.

### 2.1 Committed (PRD D4, D5)

| op | class | P | implementation |
|---|---|---|---|
| Gaussian blur | B | **P1** | mip pyramid above σ ≈ 8; separable below |
| Box blur | B | **P1** | summed-area, radius-independent |
| Highpass | B | **P1** | `src − blur(src)`; the texture-prep workhorse |
| Unsharp mask | B | **P1** | amount / radius / threshold |
| Offset with wrap | B | **P1** | free — an addressing change, no filtering |

### 2.2 Committed additions

| op | class | P | notes |
|---|---|---|---|
| Sharpen | B | **P1** | a named unsharp preset, not a separate kernel |
| Motion blur | B | **P1** | directional line integral; **cannot use the mip pyramid** |
| Add noise | B | **P1** | Gaussian / uniform, monochromatic toggle — see the domain warning below |
| Median | B | **P1** | the dust-and-scratches primitive |
| Dust & scratches | B | **P1** | median gated by a threshold, so clean areas are untouched |
| Surface blur (bilateral) | B | **P2** | edge-preserving; expensive but the skin/texture smoother |
| Reduce noise | B | **P2** | guided or non-local means |
| Find edges | B | **P2** | Scharr, not Sobel — better rotational symmetry for the same cost |
| Emboss | B | **P2** | |
| Radial / spin blur | B | **P2** | polar-space line integral |
| Zoom blur | B | **P2** | same machinery as radial, different axis |
| Lens blur | B | **P2** | aperture-shaped bokeh — the most expensive filter here |
| Displace | B | **P2** | driven by a map layer; the ROI is unbounded, so it evaluates whole-tile |
| Chromatic aberration fix | B | **P2** | per-channel radial scale |

### 2.3 Three cost warnings

**The mip pyramid only helps isotropic blurs.** A σ = 200 px Gaussian is cheap because
it is a small blur of a small image ([DESIGN-imaging.md:527](../DESIGN-imaging.md)).
Motion blur, radial blur and lens blur are directional or shaped, so that trick does not
apply and their cost is the honest one. A 128 px motion blur is 128 taps per pixel.

**Lens blur is the one to budget for.** A correct aperture-shaped bokeh at radius 64 is
~12,000 taps per pixel by brute force. Implement it as a scatter pass or a
summed-area/separable-hexagon approximation, and expect it to be the only filter with a
visible progress indicator.

**Add noise must not be applied in linear light.** Gaussian noise of fixed amplitude in
linear light is invisible in the shadows and enormous in the highlights — backwards from
how sensor grain and film grain actually behave. Add noise in the **shaper domain**, the
same domain curves are authored in, for the same reason. This is a correctness
requirement, not a preference.

### 2.4 Why these are better here than in Photoshop

Worth stating because it is the case for building them at all rather than a footnote.
Photoshop composites and filters in gamma space by default. In linear light:

- A Gaussian blur across a high-contrast edge does not produce the dark halo that makes
  the incumbent's default blurs look muddy.
- Motion blur and lens blur put **energy** in the right place, so a blurred highlight
  blooms and stays bright instead of going grey. This is the single most visible
  difference in the whole filter set.
- Downscaling averages light rather than encoded values, so a resampled image keeps its
  brightness instead of darkening.

---

## 3. Geometry and transform

Currently one P2 line in the PRD (D9). It needs to be a section, because free transform
of a pasted selection is how retouching actually works.

| op | class | P | notes |
|---|---|---|---|
| Free transform | B | **P1** | translate, scale, rotate, skew, 4-corner perspective, with handles |
| Flip H / V | — | **P1** | **exact** — no resample; own code path |
| Rotate 90° / 180° | — | **P1** | **exact** — no resample; own code path |
| Crop | D | **P1** | with a non-destructive option that only moves the display window |
| Canvas size | D | **P1** | grows the display window; layers keep their data windows |
| Image size / resample | B | **P1** | with prefiltering — see below |
| Transform selection | B | **P1** | transforms coverage, leaving pixels alone |
| Rotate canvas (arbitrary) | B | **P2** | one resample of everything |
| **Straighten** | B | **P1** | draw a line that should be level; solves the rotation |
| **Perspective correction** | B | **P1** | mark what should be a rectangle; solves the homography |
| **Lattice warp** | B | **P1** | subdividable mesh with draggable control points, over a selection |
| Lens correction | B | **P2** | barrel/pincushion and chromatic aberration, by parameter |
| Puppet warp | B | — | **future work** — shares the lattice mesh, different control scheme |
| Vanishing point | B | — | **future work** — perspective-aware clone and paste |
| Content-aware scale | — | — | **not planned** |

> **Straighten and perspective correction are the photo-prep pair.** A photographed wall
> needs deskewing and keystone removal *before* it can be made tileable, and marking a
> horizon or a rectangle and letting the app solve is a different workflow from dragging
> four corners by eye. §3's free transform covers the manual case; these cover the real one.

### 3.1 Resampling filters

| filter | use |
|---|---|
| Lanczos3 | default for downscale |
| Mitchell | default for upscale — less ringing than Lanczos on synthetic edges |
| Catmull-Rom | sharper upscale when ringing is acceptable |
| Bilinear | previews |
| **Nearest** | masks, selections and **flat label fields** — never interpolate an id |

> **Downscaling without prefiltering aliases, and no reconstruction filter fixes it.**
> A 4:1 downscale must area-average or descend the mip pyramid first. This is the most
> commonly botched operation in image editors and the texture-prep workflow hits it
> constantly.

### 3.2 Transforms compose before they resample

A transform on a layer is a class-B op holding a 3×3 matrix. Stacking a rotate on a
scale on a rotate **multiplies the matrices and resamples once**, from the original
pixels. Photoshop resamples on each commit unless the content is a Smart Object, so
three transforms cost three generations of softening.

This falls out of the op stack for free and should be stated as a feature.

---

## 4. Selection operations

PRD **E** covers the *tools*. This covers the *operations*, which is what a selection is
actually used through. All operate on antialiased coverage (E2), never a bitmask.

| group | ops | P |
|---|---|---|
| Boolean | new, add, subtract, intersect, intersect-inverse | **P1** |
| Modify | feather, grow, shrink, border, smooth, invert | **P1** |
| Whole-document | select all, deselect, reselect, select inverse | **P1** |
| From data | from mask, from channel, from layer transparency, from path | **P1** |
| From colour | colour range, luminance range | **P1** |
| To data | to mask, save as a document part, to path | **P1** / future (to path) |
| Transform | move, scale, rotate the coverage | **P1** |
| Channels | load channel as selection, save selection as channel | **P1** |
| **Quick mask** | edit the active selection as a paintable overlay, with any brush | **P1** |

**Grow and shrink go through a distance transform**, not iterated dilation. One pass,
subpixel-accurate, and the radius is a real number rather than an iteration count.
Iterated dilation on coverage also quantises the antialiasing away, which defeats E2.

**Feather is a Gaussian on coverage** — so it needs phase 6's blur, which is why
selections sit at phase 7 and not earlier.

---

## 5. Clipboard, and duplication

**This area has no requirements at all today.** It is the largest genuine gap in the
specification: it is the payoff for having built selections, and it is load-bearing for
the make-tileable workflow — offset, copy a clean patch, paste over the seam.

| op | class | P | notes |
|---|---|---|---|
| Cut | D | **P0** | coverage-weighted; leaves a premultiply-correct hole |
| Copy | — | **P0** | active layer, within the selection |
| Copy merged | — | **P0** | composites the visible stack through the selection |
| Paste | D | **P0** | as a new layer by default |
| Paste in place | D | **P0** | same document coordinates, not centred in the view |
| Paste into selection | D | **P1** | the selection becomes the new layer's mask |
| Paste as new document | — | **P1** | |
| Duplicate layer | — | **P0** | |
| Selection → new layer | D | **P0** | Photoshop's ⌘J; the most-used retouching move |
| Duplicate by ⌥-drag | D | **P1** | |
| Clear | D | **P0** | |
| Fill with colour / pattern | D | **P1** | |

### 5.1 The clipboard is a Lightweight hazard

A full-document copy at 4K `rgba16float` is **68 MB**; at 32-bit float, 136 MB. Held
invisibly, it is exactly the kind of resting allocation that
[ADR-0001](adr/0001-lazy-allocation-gated-by-idle-budget.md) exists to prevent —
and Photoshop's "Purge → Clipboard" command exists precisely because it got this wrong.

> **Rule.** The internal clipboard holds a **copy-on-write reference to a tile set plus a
> coverage mask**, not a flattened buffer. It materialises pixels only when written to
> the system pasteboard. Its resident cost appears in the status-bar figure like
> everything else, and it is discarded on document close unless the pasteboard owns it.

### 5.2 Colour management across the system pasteboard

The pasteboard is a boundary between our linear working space and everything else, so it
gets the same treatment as file import/export and not a shortcut.

**Paste in.** Tagged image → decode from its profile. **Untagged → assume sRGB**, which
is what every other application means. Decode to linear on arrival, exactly as import
does. Never paste display values into a linear document unconverted.

**Copy out.** Write display-encoded 8- or 16-bit sRGB with the profile attached, because
that is what receivers expect. Optionally also offer a float TIFF representation for the
applications that can use it — the pasteboard carries multiple representations, and the
receiver picks.

**What is lost.** Pigment latents, layer structure and Media wet state do not cross the
pasteboard. An internal copy-paste inside the app must therefore take the *internal*
path, not round-trip through the pasteboard, or painting on a Pigment layer would
silently lose its latents on every copy.

---

## 6. Tool operations

Ops driven by direct manipulation rather than by a parameter panel. Listed here because
three of them were accepted as scope in [docs/ui.md §4](ui.md) and then never became
requirements — the tool palette drew them and the specification lost them.

| op | class | P | notes |
|---|---|---|---|
| **Gradient** | D | **P1** | linear, radial, angular; gradient editor with saveable presets |
| **Paint bucket** | D | **P1** | contiguous fill with tolerance, plus fill-all-similar |
| **Fill** | D | **P1** | a selection or layer, with colour, pattern or gradient |
| **Stroke** | D | **P1** | outline a selection or layer at N px, inside/centre/outside |
| Define pattern | — | **P2** | from a selection; patterns persist and feed Fill |
| **Eraser** | — | **P0** | see below |
| Dodge / burn | C | **P1** | a brush painting an adjustment mask — §1.3 |
| Eyedropper | — | **P0** | picks into the foreground colour; sample size, sample-all-layers |
| Clone / heal | C | **P1** | the Strokes layer, phase 8 |

### 6.1 The eraser is not an op

It is the brush with a **negative deposit step**, which is why it appears here with no op
class. It inherits the entire modulation matrix — pressure, tilt, jitter, spacing, grain —
because an eraser without dynamics is useless for drawing. What it removes depends on the
layer kind: alpha on RGB, **Mass** on Pigment with the **Latent** left alone, deposit on
Media, dab records on Strokes, and the mask on the parametric kinds.

> ⚠️ **Never implement erase as painting white.** On a Pigment layer white is an opaque
> pigment under Kubelka-Munk, so it would *add* paint — lightening the pixel and leaving
> something that itself needs erasing. Full reasoning:
> [ADR-0007](adr/0007-erase-is-mass-reduction-not-a-colour.md).

## 7. View operations

Not operations on the document at all — they change what you see and must never touch a
pixel. Grouped here so the distinction is explicit, because two of them are easy to
implement destructively by accident.

| op | P | notes |
|---|---|---|
| Fit, 100 %, zoom to selection, cursor-anchored zoom | **P0** | |
| **Mirror view L/R** | **P1** | a painter's staple for catching drawing errors |
| **Mirror view U/D** | **P1** | independent toggle; both on gives a 180° composition check |
| **Grayscale preview** | **P1** | hotkey value check — what became of soft proofing |
| Rotate view | **P1** | arbitrary angle, with reset |
| Rulers, guides, grid, snapping | **P1** | guides at numeric or percentage positions |
| Navigator, pixel grid | **P2** | |

Each mirror is a sign flip in the view matrix, so the pair costs what one costs. Both are
distinct from **Flip H / V** in §3, which transforms the *document* and is exact and
destructive-by-intent — the two must never share a code path or a menu.

> **Mirror and rotate view must be view-only.** `--selftest` should assert that saving with
> either mirror axis active produces an unmirrored file. Getting this wrong writes a
> mirrored document and nobody notices until much later. The input path needs the same
> care: pen coordinates must be mapped through the inverse view transform, or painting
> under a mirror lands in the wrong place.

## 8. Where operations live in the interface

Three surfaces, and which one an operation gets is decided by its class, not by
tradition.

| surface | holds | classes |
|---|---|---|
| **Adjustment layer** | anything that should affect the layers beneath | A, and the class-B tonal ops |
| **Layer op stack** | filters attached to one layer, re-editable in place | A, B |
| **Menu command** | one-shot geometry and structural edits | D |

**Default to non-destructive.** An operation is a menu command only when it changes the
pixel grid (crop, canvas size, image size) or the document structure. Everything in §1
and §2 is re-editable in place, which is the point of the op stack and is the concrete
form of "Photoshop largely does these destructively" being a thing worth fixing.

A **Bake** command flattens a layer's op stack into pixels, for when the stack has grown
past what is worth re-evaluating. It is explicit, it is undoable, and it is never
automatic.

---

## 9. Deferred, and deliberately absent

**Future work** — wanted, no phase yet. See [PRD §12](../PRD.md).

| deferred | shares machinery with |
|---|---|
| Puppet warp | §3's lattice warp |
| Vanishing point | perspective correction, lattice warp |
| Vector masks | paths (phase 13) and masks |
| Selection → path tracing | paths |

**Not planned.**

| not planned | why |
|---|---|
| Liquify | a large subsystem serving neither target job |
| Artistic / sketch / texture filter sets | the incumbent's own users do not use them |
| Lighting effects, 3D | out of scope |
| Web slicing | dropped in [docs/ui.md §4](ui.md) — no constituency |
| Measure / ruler tool | declined; the pixel probe and rulers cover it |
| Artboards | multiple canvases per document; serves UI design, not this |
| ML matting — select subject / sky / refine edge | a model, weights, licence and inference runtime, for what the wand plus quick mask reaches manually |
| Print, soft proofing, prepress | output here is files; the useful half of soft proofing became the grayscale value check in §7 |
| Erase-to-history, background eraser, magic eraser | the first needs non-linear history (declined in ADR-0005); the others are selection tools in an eraser costume |
| Video / timeline | out of scope |
| Camera raw demosaic | OIIO reads raw; a full raw pipeline is its own product |
| Content-aware fill/scale | PatchMatch inpaint (D7) covers the case that matters |
