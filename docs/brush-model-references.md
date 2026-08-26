# Brush model: external references

Prior art for the dab/stamp brush model, with provenance. **Every claim here is
labelled with what the source actually says versus what is inferred**, because
this project has twice shipped a defect that came from a confident recollection
rather than a reading (see `AbrControl` in `io/AbrBrushes.hpp`, and audit B9).

## Licensing status — read before using any of this

| Source | Status | May we implement from it? |
|---|---|---|
| US 5,347,620 (Zimmer) | **Expired** (Google Patents: "Expired - Lifetime"; pre-GATT, lapsed ~2011) | **Yes.** Public domain. |
| US 6,373,490 (Macromedia) | **Expired** (anticipated expiration 2018-03-09, 20 yrs from filing) | **Yes.** Public domain. |
| US 6,459,439 (Macromedia) | Filed 1998, same family | Yes, on the same basis |
| Krita **documentation** | CC-licensed docs | Yes — concepts |
| Krita **source code** | **GPL** | **NO.** This project is not GPL; a derived work would force it to be. Do not read `kis_paintop`/`kis_brush` to reimplement. |
| Adobe US 10,902,645 / 8,654,143 | **In force** | **No.** Not needed; deliberately not read. |

Expired patents are an unusually good source: disclosure of the mechanism is the
price of the monopoly, so they describe *how*, not just *what*.

## Two corrections to how this material was first described

1. **US 6,373,490 is a Macromedia patent, not Fractal Design / MetaCreations.**
   Inventors Bendiksen & Griffin, filed 1998-03-09. It is nonetheless in the same
   conceptual lineage: it cites Zimmer's US 5,347,620 as prior art.
2. **US 5,347,620 never says "Beer" or "Lambert."** Searched, absent. That framing
   is Zimmer's own later blog gloss. The patent describes an **additive
   density-vector overlay**, which is mathematically what Beer–Lambert predicts for
   stacked absorbing media — but cite it as an equivalent additive-density
   mechanism, not as invoking the named law.

## US 5,347,620 — Zimmer, "System and method for digital rendering of images and printed articulation"

Filed 1991, granted 1994. The Painter 3 splash-screen patent.

**Additive density overlay**, quoted:

> "the present invention overlays dyes by adding their density vectors. This
> approach is far superior to additive color interpolation of the prior art
> because it preserves the saturation of the color."

Per subtractive-primary channel: `Ic' = Ic + Of*Bc` (likewise magenta, yellow),
where `Of` is an overlay fraction that "can take on any value from 0.0 to at
least 8.0, thickening the dye concentration of color in the brush far beyond the
usual 100 percent."

**Why this matters to us:** density is additive and *unbounded upward*, clamped
only at final render. Repeated dab overlap keeps darkening rather than
asymptoting the way alpha-over-alpha compositing does. That is a genuinely
different model from what `brush/Deposit` does today, and it is the mechanism
behind "buildup". Compare our `kMaxMass` saturation, which asymptotes.

**Grain interaction** (the "tiled textures" of the blog post): grain is a tiled
wraparound lookup, `grain_pattern[Y % NR][X % NC]`, and the overlay fraction is
`F = clamp(P*S*O1 - G, 0, 1)` where `P` is tip pressure/height, `G` the grain
surface height at that pixel. Deep grain valleys fill, peaks get skipped — this
is paper tooth, and it is how colored pencil and charcoal behave.

**Bristles** are N replica strokes (typically 2–20) each displaced by a random
polar vector, optionally scaled to a fraction of local stroke width — not N
named tips.

## US 6,373,490 — Bendiksen & Griffin, "Using remembered properties…"

**The stroke is stored as parameters, not pixels.** Per point: position,
pressure, velocity, and a random seed. Velocity is stored rather than derived
because sample timing is not regular; the seed is stored so re-rendering is
deterministic — "every copy of the same rendered brushed image will have the
same randomness to it." We already do the equivalent via
`strokeSeedFromStart()` and per-dab draws keyed by dab index.

**The three-way distinction — the most valuable content here**, and the likely
missing vocabulary for our `Flow` vs `Concentration` ambiguity:

| Parameter | Controls | Scope | Example |
|---|---|---|---|
| **Ink concentration** | alpha at dab centre for one pass | within a stroke | highlighter ≈25%, oil ≈opaque |
| **Buildup flag** | whether repeated passes ratchet alpha past that ceiling toward 100% | across passes | on for highlighter; off where scrubbing never fully obscures |
| **Blackness** | whether overlap additionally shifts *darker*, not merely more opaque | across passes, independent of buildup | on for felt/highlighter ("dirty marker"); explicitly **off** for charcoal |

The charcoal counter-example is quoted directly: "charcoal brushes do not use
blackness because when a piece of charcoal is used, the brush stroke does not
get blacker. As it overdraws on top of itself, it might become more opaque, but
the actual color does not become blacker."

**Scatter is perpendicular jitter off the centreline** — independent
confirmation of audit B5's axis half: "If scatter is zero, then the stamp images
line up with their centers directly on the line between the two points."

**Direction is derived, not stored**, and used to rotate an asymmetric stamp —
which is what our `DynamicSource::Direction` does.

**Multi-tip**: up to N tips, each following its own trajectory with configurable
(possibly random) spacing, for a rake/bristle appearance.

## Known gaps — deliberately not filled with guesses

- **"Flow rate" is named but never defined** in 6,373,490's parameter list. Our
  `Flow` vs `Concentration` split therefore has *no* patent grounding. A
  defensible reading — concentration is the ceiling, flow is the per-dab
  deposition rate toward it, buildup is whether the ceiling can be exceeded — is
  **synthesis, not source**, and must not be cited as the patent's.
- **Application 09/037,721** ("Use of filters attached to objects…") could not be
  resolved to a granted number. Not guessed. Needs USPTO Public Search by serial.
- **The suggested `inventor:"Mark Zimmer" assignee:"Fractal Design"` sweep was
  run on 2026-08-25 and found nothing beyond US 5,347,620** — but the search was
  *not* conclusive, and the distinction matters. `patents.google.com` returned
  **HTTP 503** on every direct fetch (both the patent page and the query URL) and
  `patents.justia.com` returned **HTTP 403**, so the sweep fell back to indexed
  search summaries, which are second-hand by construction. What it did establish,
  as a useful *negative*: the "Digital painting" patents that surface first for
  these terms are **not** in the Fractal Design lineage —
  **US 6,870,550** (Schuster & Wilensky, **Adobe**) and **US 5,835,086**
  (Champernowne, a lazy-region-update rendering scheme). Do not cite either as
  Painter prior art; the Adobe one in particular is off-limits under the table
  above unless its status is checked and found expired.
  **Next step if this is worth reopening:** USPTO Public Search
  (`ppubs.uspto.gov`) by inventor, or the `image-ppubs.uspto.gov/dirsearch-public/
  print/downloadPdf/<number>` full-text endpoint, which responds where the two
  commercial mirrors do not. Note that 5,347,620's own PDF there is an un-OCR'd
  fax scan, so expect to read images rather than text.
- **US 5,347,620's claim text** could not be retrieved (Google Patents omits it;
  the USPTO PDF is an un-OCR'd fax scan). Everything above is from the
  description, which is where the mechanism lives — but claim *scope* is unsourced.
- **PaintCopilot (arXiv:2605.20941)** formulas — logarithmic pressure→radius,
  `p^2.5` opacity, EMA smoothing — are relayed second-hand and are being verified
  against the paper separately. Do not implement from this file's mention of them.

## What this suggests for naturalPaint

Ordered by how much it would change the feel, not by effort:

1. **Buildup as a distinct concept.** We have no equivalent. `kMaxMass`
   saturation asymptotes; additive density does not. This is likely the single
   biggest difference between our marks and Painter-lineage marks.
2. **Grain / paper tooth.** `F = clamp(P*S*O1 - G, 0, 1)` against a tiled height
   field. We have no grain interaction at all.
3. **Ground the `Flow`/`Concentration` split** — or collapse it — using the
   concentration/buildup/blackness vocabulary rather than leaving it ambiguous.
4. **Blackness** as an opt-in per-brush overlap darkening, separate from opacity.
