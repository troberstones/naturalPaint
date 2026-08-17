# naturalPaint — Implementation Plan

Ordered, executable steps. Requirements are in [PRD.md](PRD.md), mechanism in
[DESIGN-imaging.md](DESIGN-imaging.md), settled decisions in [docs/adr/](docs/adr/),
terminology in [CONTEXT.md](CONTEXT.md).

Every step states **what**, **files**, and **verify**. A step is not done until its
verification passes.

> **Detail tapers on purpose.** Phase 1 is at file-and-function level because the code
> it touches exists and has been read. Phases 2–5 and 19 are step-level. Phases 6–18 are
> outlines — writing them at step level now would be inventing specifics that the
> earlier phases will contradict. Re-expand each phase when it becomes next.

The operation catalogue — every colour control, filter, transform, selection and clipboard
op with its evaluation class — is [docs/operations.md](docs/operations.md).

---

# Phase 1 — Make the simulation obey the new rules

No new user-facing features except one bug fix. Establishes the invariants and the
measurement baseline everything later depends on.

**Order within the phase is load-bearing:** latency first because it is the top
product risk and cheapest to check now; fixed timestep second because it makes
`--diag` reproducible, and the other changes are verified *with* `--diag`.

## 1.1 — Latency baseline

**What.** Measure pen-to-photon end to end, before any brush work exists. PRD risk #1
is that this target is unreachable on wgpu; if it reads 60 ms, the premise of the
whole painting chain is broken and you need to know now, not at phase 10.

- Capture `SDL_PenEvent` arrival time (`SDL_GetTicksNS`) and carry it with the point.
- Record time immediately after `wgpuSurfacePresent` for the frame that first drew
  that point.
- Accumulate p50 / p99 over a stroke; print on stroke end and via a `--latency` flag.

**Files.** `src/main.cpp`, new `src/app/Latency.{hpp,cpp}`

**Verify.** Paint a few strokes; read p50/p99. Record the number in this file's
Findings section below whatever it is. Target < 20 ms (PRD F3).

> If p50 exceeds ~30 ms, stop and investigate present mode and frame pacing before
> continuing. Everything downstream assumes this is fixable.

## 1.2 — Fixed timestep

**What.** Replace frame-derived `dt` with a fixed step plus an accumulator.

- `kFixedDt = 1.0f / 240.0f` — matches the current effective substep rate.
- Per frame: `acc += min(realElapsed, kMaxCatchUp)`, run `floor(acc / kFixedDt)`
  substeps, keep the remainder.
- Cap substeps per frame (8) so a slow frame cannot trigger a death spiral.
- `P.dt` becomes the constant, not a per-frame value.

**Files.** `src/sim/PaintSim.cpp` (step loop), `src/main.cpp` (timing), possibly
`src/app/AppState.hpp`

**Verify.**
```bash
./build/src/naturalPaint --diag 20 > a.txt && ./build/src/naturalPaint --diag 20 > b.txt && diff a.txt b.txt
```
Must be identical. This is PRD H7, and it is what makes 1.3 verifiable at all.

> ⚠️ **`--diag` numbers will shift** relative to the tables recorded in `README.md`,
> because the stepping changes. Re-record them as the new baseline in the same tables
> and note that they are post-fixed-timestep. Do not treat the shift as a regression
> without comparing physics, not numbers.

## 1.3 — Arc-length dab emission

**What.** Deposition becomes per-dab and `dt`-independent (ADR-0003). Closes
`README.md` Known Bug #2.

1. **Build the emitter.** Centripetal Catmull-Rom through the last four sampled
   points; accumulate arc length along it; emit a dab every `spacing × radius`
   pixels; carry the leftover distance across frames so spacing stays continuous.
2. **Feed the solver per dab.** Each emitted dab dispatches the splat pass as a
   *point* (`brushA == brushB`) rather than a per-frame segment. Dab counts per frame
   are small — a fast stroke at 20 px spacing across 200 px is ten dabs — so a
   dispatch per dab is acceptable and avoids a dab buffer for now.
3. **Strip the `dt` scaling** from every deposition term.
4. **Delete `velocityCutoff`** from `oil_transfer.wgsl` — a stationary brush now
   accumulates no arc length, so it emits no dabs, and the hack is redundant.

**Files.** new `src/brush/StrokePath.{hpp,cpp}`; `shaders/splat.wgsl`,
`shaders/oil_splat.wgsl`, `shaders/ink_splat.wgsl`, `shaders/oil_transfer.wgsl`;
`src/sim/PaintSim.cpp`

**Verify.**
- `--diag 20` — pigment mass flat once the brush lifts (PRD F1).
- Hold the brush stationary for 5 s — no mass accumulates. This is the `velocityCutoff`
  behaviour arriving for free.
- **New `--selftest` case:** stroke the same path twice at different speeds; deposited
  mass must match within tolerance. This is the assertion that pins ADR-0003.
- Visual: oil ridges gone under impasto lighting.

## 1.4 — Lazy, per-mode allocation

**What.** ADR-0001. Idle drops from ~294 MB to zero; watercolour from 294 to ~193 MB.

1. **Make the sim optional.** `main.cpp` holds `std::unique_ptr<PaintSim>`, null until
   a paint tool is first used. Every access site becomes construct-on-demand.
2. **Split field allocation by mode.** Currently `PaintSim::init` allocates all of
   `water_ pigC_ pigR_ depC_ depR_ sat_ aux_ paper_ lbmA_ lbmB_ lbmC_ brushVol_
   brushC_ brushR_` unconditionally.
   - watercolour → `water sat aux paper pigC pigR depC depR`
   - ink → `lbmA lbmB lbmC` + the shared water/deposit set
   - oil → `brushVol brushC brushR` + the shared set
3. **Mode switch frees the outgoing set** and allocates the incoming one. The canvas
   already clears on mode switch, so nothing of value is discarded.
4. **Add resident-memory measurement.** On macOS use `task_info` with
   `MACH_TASK_BASIC_INFO` for *current* resident size — `getrusage`'s `ru_maxrss` is
   peak, not current, and will not do.
5. **Add the gate** to `--selftest`: idle RSS < 40 MB, and zero sim field textures
   allocated when no Media content exists.

**Files.** `src/main.cpp`, `src/sim/PaintSim.{hpp,cpp}`, `src/app/SelfTest.cpp`, new
`src/app/Memory.{hpp,cpp}`

**Verify.** `--selftest` passes the two new assertions. Launch and confirm RSS with no
painting. Switch modes and confirm the other modes' fields are not resident.

> ⚠️ **`bindCache_` must be cleared whenever textures are recreated.**
> `PaintSim.hpp:248` caches bind groups keyed by `uint64_t`. Freeing and reallocating
> fields on a mode switch invalidates every cached group, and a stale handle here will
> present as corrupt output rather than as a clean crash.

## 1.5 — Verify the Mixbox seam actually exists

**What.** ADR-0006 relies on `NP_USE_MIXBOX=OFF` as the escape hatch from a CC BY-NC
dependency. That option may not have been exercised in a long time. **An unexercised
build option is not a seam** — and discovering it has rotted at the point you need it
is the worst possible timing.

```bash
cmake -S . -B build-nomix -DNP_USE_MIXBOX=OFF && cmake --build build-nomix -j
./build-nomix/src/naturalPaint --selftest out-nomix.png
```

The selftest asserts blue crossing yellow gives green, so it is a real test of the
`km2` fallback rather than a compile check. Fix whatever has rotted; keep both
configurations building from here on.

**Files.** whatever the build reveals — likely `shaders/include/mixbox.wgsl` guards and
`src/paint/Palette.cpp`

**Verify.** Both configurations build; both pass `--selftest`. Record the visual
difference between the two mixing models — that difference *is* what the Mixbox licence
is being accepted for, and it is worth knowing its size.

## Phase 1 exit criteria

| | target |
|---|---|
| `--selftest` idle RSS | < 40 MB |
| sim field textures when idle | 0 |
| watercolour resident | ~193 MB |
| `--diag` run-to-run | identical |
| pigment mass after brush lift | flat |
| equal-mass-at-different-speeds test | passes |
| Known Bug #2 | closed |
| latency p50 | recorded |
| `NP_USE_MIXBOX=OFF` builds and passes `--selftest` | yes |

---

# Phase 2 — See a file

**Goal.** Open a PNG, pan and zoom it, probe pixel values in linear and display
encodings. Proves the tile path and the colour policy before anything depends on them.

1. **`core/Tile.hpp`** — `kTileSize = 128` as `constexpr`, tile coordinate type,
   document↔tile coordinate conversion.
2. **`core/TileStore`** — sparse hash map from tile coordinate to tile. Allocate on
   write, query without allocating, iterate occupied tiles. No COW yet.
3. **`color/Space`** — sRGB and Rec.709 transfer functions both directions, primaries
   as data, `Document`-level working-space policy. Not hard-coded into shaders.
4. **`core/Document` + `core/Layer`** — layer list holding exactly one entry, with the
   `kind` enum from `CONTEXT.md` present but only `RGB` implemented. **Design for N,
   ship 1.**
5. **`Document::createBlank(w, h, space)`** — PRD C7. Nothing else in the plan creates
   a document that did not come from a file.
6. **`io/ImageIO` + `io/StbBackend`** — read PNG/JPEG/TGA/BMP, decode to linear
   `rgba16float`, write into tiles.
7. **Reserve the selection seam.** Deposit and op interfaces take an optional
   selection mask parameter *now*, even though nothing populates it until phase 7.
   PRD E1 — retrofitting this later touches every path.
8. **Tiled viewport draw**, retaining the pan/zoom logic from the outgoing
   `MacPaintUI` — that part is reusable even though the chrome is replaced. New UI
   direction and layout in [docs/ui.md](docs/ui.md).
9. **Mip pyramid** for tiles, so a 25 % zoom evaluates at a matching level.
10. **Pixel probe** reporting both linear and display values, and the **eyedropper**,
    which is the same sampling code writing to the foreground colour instead of a readout
    (PRD Q10). Sample size and sample-all-layers are parameters of the sample, not
    separate tools.
11. **View controls** — fit, 100 %, zoom to selection, cursor-anchored zoom, **mirror view
    L/R and U/D** as independent toggles, **grayscale preview**, rotate view (PRD Q1–Q4).
    All of the last four are *view* state: they never touch the document, and `--selftest`
    should assert that saving with either mirror axis on produces an unmirrored file.
    Build these as one **view matrix** with mirror as sign flips — then both axes, rotation
    and zoom compose in one place, and pen input maps back through its inverse. Doing
    mirror as a special case in the draw path is what makes painting-under-mirror land in
    the wrong spot.
12. **Rulers, guides, grid and snapping** (PRD Q5–Q7). Guides land here rather than later
    because the offset-by-half tiling workflow in phase 9 depends on them and retrofitting
    snapping into an existing viewport is fiddlier than building it in.
13. **Place an image as a layer** into the open document, by menu and by drag-drop
    (PRD I14) — distinct from opening a file, which creates a document.
14. **The base layer is an ordinary layer with alpha** (PRD C16). No locked Background.
15. **`app/Keymap`** — bindings loaded from a data file, not `if (key == ...)` scattered
    through the UI. Actions are named; the file maps keys to names; **conflicts are detected
    at load and reported**, including conflicts that only exist within one layer-kind scope
    (PRD R7, R8). Default keymap and the reasoning behind it:
    [docs/shortcuts.md](docs/shortcuts.md).

> **Build the keymap as data in phase 2, before there are tools to bind.** Every tool added
> from here registers a named action, so the keymap grows by a table row rather than a code
> change — and the conflict detector is what stops the collisions in
> [docs/shortcuts.md §5](docs/shortcuts.md) from reappearing as tools arrive across fifteen
> phases. Retrofitting this after phase 10 means unpicking hardcoded keys from every tool.

> ⚠️ **`⌃⌥`-drag for brush size and hardness is a phase 10 requirement, not a nicety**
> (PRD R5). `[` and `]` are unreachable while holding a pen in the right hand, so the
> on-canvas gesture is the primary path. Reserve the chord here so nothing else claims it.

**Verify.** Open an 8-bit PNG and a 16-bit PNG; probe known values; confirm memory
tracks occupied tiles rather than canvas dimensions (PRD C2). Mirror both axes, save,
reopen — the file is unmirrored. Paint a stroke with a mirror on and confirm it lands under
the cursor.

---

# Phase 3 — Grade it

**Goal.** Live levels, curves, saturation, grayscale with correct linear math and a
curve widget that behaves.

1. **`color/Shaper`** — 1-D log encode/decode. ADR-0004 notes this is a *format-level*
   commitment: saved curve control points are coordinates in this domain.
2. **`ops/PointOps`** — levels, curves, exposure, saturation, RGB→grayscale, channel
   mixer, as plain functions. These are what the LUT baker consumes, so they are
   written once and reused.
3. **Un-premultiply / re-premultiply** wrapped around the point-op path, not left to
   callers (PRD B4).
4. **`color/LutBake`** — bake a maximal run of adjacent point ops onto a 32³ grid via
   one compute dispatch. Rebake on parameter change.
5. **`core/OpStack`** — ordered ops, dirty tracking, run detection for the collapse.
6. **Apply pass** — shaper → 3-D LUT fetch → un-shape.
7. **Histogram** over the visible region.
8. **Op-stack UI** — reorder, toggle, delete, and a curve widget operating in the
   shaper domain.

**Verify.** `--selftest` round trip: decode → encode returns the original within
tolerance (PRD B3). Twelve stacked grade ops cost the same as one at draw time.

---

# Phase 4 — Write it out

**Goal.** A complete open → grade → save loop.

1. **Export path** — encode from working space to a chosen target space and bit depth,
   explicitly, never silently (PRD B6, I5).
2. **`io/OiioBackend` behind `NP_USE_OIIO`** — EXR, TIFF, HDR, DPX, flattened PSD,
   camera raw.
3. **Capability query** — format support is discovered at runtime; the core builds and
   runs without OIIO (PRD I3).
4. **Native `.npaint` save and load** — multi-part tiled EXR via OIIO. No bespoke writer:
   one part per layer, `HALF` channels, latents as named channels, `np:*` typed
   attributes. Spec: [docs/document-format.md](docs/document-format.md). PSD *read*
   arrives free here too; PSD *export* is phase 15.
5. **Wire OIIO's `ImageCache`** as the residency layer for unmodified source tiles.
   This is the main reason the dependency earns its cost.
6. **Lazy OIIO init** — on first file open, not at startup, so PRD A2 holds.
7. **Export As** — format, space, depth **and resize**, with saveable presets (PRD I15).
   Downscale prefilters; see the phase 6 warning.
8. **Document lifecycle** — revert, duplicate document, save a copy, save incremental,
   open recent (PRD I17).
9. **`core/Journal`** — the recovery journal from
   [ADR-0008](docs/adr/0008-recovery-needs-a-model-journal-not-tile-spill.md). Serialises
   the document *model* on a timer and after every structural edit, using **the same
   writer as native save** aimed at a scratch file. Dirty tiles flush on the same timer
   **for the active document**, not only on deactivate. Unclean scratch directories are
   offered on launch, named and dated (PRD O5–O10).

> ⚠️ **The design's claim that crash recovery falls out of the `mmap` tile spill is
> wrong**, which is why step 9 exists. The spill fires on *deactivate*, so the document
> being painted has written nothing; it stores tiles with no layer structure; and a
> document whose value is an op stack and a Strokes layer has almost nothing in it. The
> more non-destructively a user works, the less recovery the old scheme gave them.

**Verify.** Round-trip a 32-bit EXR without precision loss. Confirm cold start is
unchanged with `NP_USE_OIIO=ON` but no file opened. `kill -9` mid-session and confirm the
recovered document matches the journal, including layer structure and op stacks — not just
pixels.

---

# Phase 5 — Stack it

**Goal.** The document model, complete — and the save/load/layers/undo the README has
been missing.

1. **Multiple layers** in `Document`, with reorder, visibility, lock, opacity.
2. **`core/Blend`** — the linear-safe set (over, plus, multiply, screen, min, max) and
   `Mix`, the KM latent lerp. Display-referred modes labelled as such (PRD B7).
3. **Pigment layers** — latent × mass tile storage at f16. Per-layer op stack applies
   *after* the latent→RGB projection, so grading never bakes the latents.
4. **Layer masks** — single-channel tile store, the same machinery.
5. **Adjustment layers** — op stack against the composite below.
6. **COW tiles** — copy-on-write with reference-counted history.
7. **`core/History`** — a **linear list with a cursor**, not a stack: undo moves it back,
   **redo** moves it forward, and a new edit at a non-end cursor truncates the tail. Undo
   bounded in *bytes*, compressed, tail spilled to `mmap` scratch (PRD A9, O1).
   **Snapshots** are explicit entries exempt from eviction (O4). See ADR-0005's amendment
   — redo is not an inverse, it is the same keyframe replay with a longer dab stream, which
   only works if history is a cursor from the start.
8. **History panel** listing entries by originating tool or op; clicking one moves the
   cursor there in a single replay, not N (PRD O2, O3).
9. **Clipping masks** — a layer or group clipped by the alpha of the layer below
   (PRD C9). The UI already assumed this: `ADJUSTMENT · CLIPPED` appears in
   [docs/ui.md](docs/ui.md)'s layer rows.
10. **The merge family** — merge down, merge visible, stamp visible, flatten, and
    rasterise a parametric layer (PRD C10, C11). Distinct from `Bake`, which flattens one
    layer's *op stack* and leaves the layer.
11. **Multi-select, align and distribute, colour labels, linking, panel filtering**
    (PRD C12, C13, C15).
12. **Layer comps** — named sets of visibility, position and properties, restorable in one
    click and **persisted in the document** as an `np:comps` blob on part 0 (PRD C14).
    Cheap here because the layer model is fresh in hand; expensive later.
13. **Export comps to files, and layers to files** — one shared loop: set a document state,
    composite, write through phase 4's Export As presets with a name template (PRD I16,
    I18). Both are the same mechanism, so building them apart would mean building it twice.

> **Comps and comp export are not speculative scope.** Export Layer Comps to Files is part
> of the primary user's existing Photoshop workflow, which is the only evidence that settles
> a priority. It is the reason this pair sits at phase 5 rather than in the automation phase
> — and the reason `np:comps` has to be in the format from the first save that carries
> layers, not added later.
14. **Tabs + optional two-tab split**, with the visible-documents GPU rule from
    ADR-0001's amendment.
15. **Native save/load** carrying layers and latents, with the pigment basis stamped and
    a baked RGB composite embedded (PRD C8, I4).
**Verify.** Twenty open tabs cost kilobytes each. Two visible documents hold GPU
textures; hidden ones hold none. Blue on a Pigment layer over yellow gives green under
`Mix` and translucent blue under `Normal`. Undo ten strokes, redo ten, and the result is
pixel-identical to before the undos. Export four comps and confirm four correct files with
the right names.

---

# Phases 6–19 — Outlines

Re-expand to step level when each becomes next. The operation catalogue these phases
build from — every op, its class and its cost — is
[docs/operations.md](docs/operations.md).

## 6 — Filter and transform it
ROI propagation (`roi(rect) → rect` per op, walked backwards) · hash-keyed tile cache
on `(layer, tile, mip, hashOfOpsBelow)` · Gaussian/box blur through the mip pyramid ·
highpass as `src − blur(src)` · unsharp · offset with wrap · then the rest of the filter
set: sharpen, motion blur, median, dust & scratches, add noise, shadows/highlights,
local contrast.

Transform lands here too, because it is the same class-B machinery: a 3×3 matrix per
layer, **composed before resampling** so a stack of three transforms costs one
generation · Lanczos3 / Mitchell / Catmull-Rom / bilinear / nearest · exact paths for
flips and 90° rotations · crop, canvas size, image size.

Then the solved-for-you variants, which are the photo-prep half: **straighten** (draw a
line that should be level) · **perspective correction** (mark what should be a rectangle
and solve the homography) · **lattice warp** of a selection over a subdividable mesh.
Puppet warp is deferred to future work and shares this mesh, so building the lattice first
is not wasted.

And the tool-shaped ops that need the same fill machinery: **gradient** (linear, radial,
angular, with an editor and presets) · **paint bucket** with tolerance and fill-all-similar
· fill and stroke a selection or layer. These were accepted as scope in
[docs/ui.md §4](docs/ui.md) and then never became requirements — D24–D26 closes that gap.

> ⚠️ Two domain traps. **Add noise runs in the shaper domain** — fixed-amplitude noise
> in linear light is invisible in shadow and enormous in highlight. And **downscale must
> prefilter** (area average or descend the mip pyramid); no reconstruction filter fixes
> aliasing after the fact. Both are the most commonly botched operations in this category.

> Motion blur, radial blur and lens blur **cannot use the mip-pyramid shortcut** — they
> are directional or aperture-shaped, so their cost is the honest one. Lens blur is P2
> and is the only filter here likely to need a progress indicator.

## 7 — Select and paste
Selection coverage mask tile store at `r8unorm` · rectangle and ellipse (analytic,
antialiased) · lasso and polygon lasso (polygon rasterisation) · magic wand as a **CPU**
flood fill paging through the tile store · booleans · feather via phase 6's blur ·
**grow/shrink through a distance transform**, not iterated dilation, so the radius is a
real number and the antialiasing survives · colour and luminance range · invert ·
selection ↔ mask conversion · transform selection · marching ants.

**Quick mask** and **channels** land here too: editing the active selection as a paintable
overlay is nearly free once brushes and coverage masks both exist, and it is how precise
selections actually get made · alpha channels stored in the document · load channel as
selection, save selection as channel · the Channels panel's single-channel view and edit
(PRD E12, E13, Q11).

The clipboard ships here, because coverage-weighted cut is what makes it correct and
selections are what make it useful: cut/copy/paste/clear · copy merged · paste in place ·
paste into selection · selection → new layer · duplicate.

> ⚠️ **The clipboard must not hold a flattened buffer.** A 4K full-document copy is 68 MB
> at `rgba16float`, held invisibly — precisely what ADR-0001 exists to prevent, and the
> reason Photoshop ships a "Purge → Clipboard" command. Hold a copy-on-write tile
> reference plus a coverage mask; materialise only when writing the system pasteboard.
> An *internal* copy-paste takes the internal path and never round-trips the pasteboard,
> or pigment latents are silently lost on every copy.

## 8 — Repair it
`Strokes` layer: dab records, spatial index over dab bounds, checkpoint tiles, and the
**samples-only-from-below** rule · clone with aligned/non-aligned · heal via a
gradient-domain solve · diffusion inpaint (Telea).

## 9 — Tile it
Lighting-gradient removal (divide by a heavily blurred copy, re-centre the mean) ·
offset by half · seam heal · **3×3 repeat preview** · PatchMatch as a cached class-D op
with a Recompute button and a deterministic seed.

**Primary user's workflow is complete here.**

## 10 — Paint on it
`brush/Brush`, `brush/Dynamics` (the `Link` matrix — every dynamic uniform, none
special-cased) · deterministic jitter seeded from `(strokeId, dabIndex)` ·
`brush/StrokeBuffer` (coverage scratch giving the opacity ceiling) · procedural round
and stamp tips · rolling and stamp grain · glaze vs blending accumulation, with the
per-pixel-walks-its-tile's-dab-list trick for blending · pressure/tilt/velocity response
curves · the latency path drawing scratch over last composite · Pigment and RGB deposit ·
smudge.

The arc-length emitter already exists from 1.3.

**The eraser is built here, in the same pass as the deposit** — not as a later tool. It is
the brush with a *signed* deposit step, so it inherits the whole `Link` matrix for free,
and the sign is decided per layer kind: alpha on RGB, **Mass** on Pigment with the
**Latent** untouched, deposit on Media, dab-record deletion on Strokes, mask on the
parametric kinds. Spec:
[ADR-0007](docs/adr/0007-erase-is-mass-reduction-not-a-colour.md) (PRD F9–F11).

**Dodge and burn** are also built here, and are not pixel ops: the brush paints coverage
into an exposure adjustment layer's mask, which makes them non-destructive and adjustable
after the fact (PRD D13).

> ⚠️ **Do not implement erase as painting white.** On a Pigment layer white is an opaque
> pigment under Kubelka-Munk, so "erasing" would deposit paint — lightening the pixel *and*
> leaving something that itself needs erasing. Mass reduction is the only correct form, and
> it is lossy on mixed paint by nature: you get less of the mixed colour, never the
> components back. That is what paint does; the UI must not imply otherwise.

## 11 — Media layers
Solver window tracking the wet region in document space, capped at 1024² (ADR-0002) ·
bake-on-dry into the dry extent · 2–4 windows per layer, oldest baked when a new one is
needed · refuse-to-wet past the cap, *visibly* · Media deposit path · solver keyframes
every ~2 s plus dab replay for undo (ADR-0005) · wet mix exposed as dilution / charge /
attack / pull over the existing reservoir.

## 12 — Import brushes
`io/Descriptor` — Action Descriptor reader (`Objc`/`VlLs`/`UntF`/`doub`/`TEXT`/`enum`),
reusable for PSD later · `io/AbrImport` — 8BIM container, `samp` RLE tip bitmaps,
`desc` tree, `bVTy` → `Source` mapping · `io/BrushIR` neutral representation ·
**import report** naming everything dropped · then `io/BrushImport` — Procreate zip,
`bplist00` reader, NSKeyedArchiver graph resolution.

## 13 — Paths
Bézier path data model · anchor editing, corner ↔ smooth conversion · path →
selection via antialiased coverage rasterisation · **stroke path with brush** — a
Bézier feeding the phase-1 dab emitter, so it is nearly free · fill path · Paths panel
and document persistence.

Selection → path (contour extraction plus curve fitting) is a non-goal.

## 14 — Text
`Text` layer kind — string, font reference and layout parameters, rasterised at
evaluation, so it stays editable and parametric like an Adjustment layer · **CoreText**
for shaping, bidi, cluster breaking and font fallback, behind an interface HarfBuzz +
FreeType could replace · point and paragraph text, alignment, leading.

Text on a path, vertical text and rich-text runs are non-goals.

## 15 — PSD export
Not the save path — native save shipped in phase 4. Flattened PSD first (small), then
simply-layered: one PSD layer per naturalPaint layer, blend modes mapped where they
exist, latents dropped with a warning naming what was lost.

> ⚠️ Photoshop's 16-bit range is **0–32768**, not 0–65535, and its 32-bit mode is IEEE
> float. Never emit native `curv`/`levl` blocks — our curves are in the shaper log domain
> and would be silently wrong. Spec:
> [docs/document-format.md](docs/document-format.md).

---

# Phases 16–18 — Flatting

Absorbing autoFlats. Full accounting — inventory, dispositions, hazards and the
scope note — in [docs/autoflats-migration.md](docs/autoflats-migration.md). A third
independent chain; earliest start is after phase 7, because the lasso and marquee repair
tools *are* selection tools.

## 16 — Flat it
**Port `test/run.ts` and `test/harness.ts` first** (478 lines, with fixtures) — they are
the specification for everything after, and porting them last means porting blind ·
`flats/Ink` (extraction, morphology, Zhang-Suen skeletonisation) · `flats/Segment`
(trapped ball, line-centre expansion, region finalisation, declutter, slivers; GPU growth
from autoFlats' existing WGSL chamfer kernel, CPU Dijkstra fallback) · `flats/Fills`
(fill table, anchors, adjacency graph, graph colouring, palette) · the **Flats layer**
kind with RLE label storage and its `.npaint` part · the Fills panel.

Deliverable: open line art → flat → recolour from a palette → save and reload.

> ⚠️ **The port is not "translate `core/`".** About 800 lines of the actual algorithm —
> `replayEdits`, `assignGroups`, `regionAnchor`, the merge tools, `regionAdjacency`, the
> graph colouring — live inside autoFlats' 2,294-line UI file. Step one is extracting a
> *headless flatting library* from both places. Estimating from `core/` alone understates
> this by ~40%.

> ⚠️ **Ink extraction runs in the display domain.** Every threshold was authored against
> 8-bit sRGB; mid-grey is 0.216 in linear, not 0.5, so running them against the working
> space silently changes what "dark" means. Same rule as curves (ADR-0004).

> ⚠️ **Region colour comes from a point inside the region, never from its id.** Ids get
> reshuffled by any parameter change, and an id-derived palette repaints the whole drawing
> whenever a slider moves — which makes it impossible to see what changed. Anchored to a
> place, a 417→381 fill change keeps 91% of colours. This is a regression test, not a
> comment.

## 17 — Fix it
`flats/Gaps` — collision fronts, stroke-width matching, orientation checks against the
stroke orientation field, Kellman-Shipley relatability, Euler-elastica ranking, closure
and simplicity pruning, Hermite bridge curves · the optional stochastic completion field ·
**edit recording and replay** for all six edit types, extracted per §3 of the migration
doc · the repair tools: merge pair, draw-merge, delete, shape fill, group lasso, marquee
select-edits, bridge draw/erase · auto-bridge · ridge and edit overlays.

Gate: replay is idempotent, and a full parameter sweep preserves every recorded edit.

## 18 — Sheet it, and wash it
Membrane sag by **re-parameterising `shaders/jacobi.wgsl`** with Dirichlet boundaries at
ink — the Poisson solver already exists for pressure projection, which is the largest
single saving in the migration · watershed on the sag field · sag / zebra / ridge
visualisations · expand a Flats layer to real layers (per colour, per fill in colour
groups, merged) and the PSD group export those feed · **wash** = stamp the flats into a
Media layer and run the existing solver.

> **Do not port `watercolor.ts`.** Those 409 lines are a 256 px reduced coffee-ring
> simulation that existed because a browser page had nothing better. naturalPaint owns the
> real Curtis '97 solver. Deleting it removes code *and* improves the feature — and stops
> two implementations of one idea from drifting apart.

---

# Phase 19 — Automate it

**Goal.** Record once, run on forty files. For texture preparation this is the largest
productivity feature in the incumbent, and here it is mostly a file format.

1. **`ops/Action`** — serialise a layer or document op stack to a named, human-readable,
   diffable file. **This is not a new subsystem**: an op stack is already an ordered list of
   parameterised operations with a defined evaluation order, so an action is that list
   written down (PRD P1, P5, P6).
2. **Apply an action** to another document, resolving anything document-specific — layer
   references, resolution-dependent radii — explicitly rather than by index.
3. **`app/Batch`** — run an action over a folder or a chosen file set, with an output rule:
   format, space, depth and resize from I15's presets (PRD P2, P3).
4. **Per-file reporting**, and **never a partial overwrite of an input** (PRD P4).
   A batch that fails on file 12 of 40 leaves files 13–40 untouched and says so.
5. **Lens correction** and **pattern define/fill** land here as the remaining P2 image ops
   (PRD D22, D27).

Export-to-files already shipped in phase 5 (PRD I16, I18) — the batch runner reuses that
loop rather than growing a second one.

**Verify.** Grade one plate, save the stack as an action, run it over thirty photographs,
and confirm every output is correct and no input was modified.

> **This phase's dependencies are satisfied at phase 6.** It sits last because nothing
> depends on it, not because it is low value — for the primary user's stated job it is
> arguably the highest-value phase in the second half of the plan, and moving it earlier is
> a defensible reordering at any time.

> ⚠️ **Better than Photoshop's Actions, for a structural reason.** Photoshop records UI
> events, so an action breaks when a dialog changes or a panel moves. Recording a
> *parametric op stack* has no UI in it at all — which is why P6 can say recording is
> optional: any document you have already graded is a finished action.

---

# UI

The MacPaint chrome is superseded. Direction, layout, design tokens and the four
architecture reconciliations are in [docs/ui.md](docs/ui.md). `src/ui/MacPaintUI.*` and
`src/ui/Theme.*` are **replaced**, not extended; the viewport pan/zoom logic is the only
part carried forward.

Interface work is not a phase — it lands inside phases 2–11 alongside the features it
exposes.

---

# Findings

Recorded as phases complete. Empty until phase 1 runs.

| date | finding |
|---|---|
| | |

---

# Deviations

Where implementation diverged from the plan, and why. Keeping this honest is what
makes the plan worth re-reading.

| step | deviation | reason |
|---|---|---|
| | | |
