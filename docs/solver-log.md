# Solver log

How the fluid simulation was debugged, and what was measured. Moved out of
`README.md` so the landing page can describe what the project *is*; nothing here
is superseded, and the parameter values in the code are the ones these
measurements produced.

Run `--diag` to reproduce any of it. It lays one wet blob and reports pigment
mass, wet area, and flow speed over 20 s.

---

## The spreading wash ran clear at its leading edge

`pigCells / wetCells` decayed monotonically — 1.07 → 0.81 — because water spreads
via a dedicated capillary diffusion pass while pigment moved *only* by advection,
so the wet front always outran the pigment.

`advect_pigment` now carries a diffusion term (`pigmentDiffuse`), using the
symmetric pair weight `min(M_here, M_neighbour)` so the exchange is conservative
and falls to zero at the edge of the wet region. The ratio now holds at **0.98**.

## Pigment mass was being destroyed

Two independent bugs that partially cancelled, which is why the net number looked
merely "wrong" rather than obviously two things. Bisected by neutering passes:

| config | 20 s mass change |
|---|---|
| f16, full pipeline | −35 % |
| f16, advection disabled | −45 % |
| f16, advection **and** transfer disabled | 0.0 % (exact) |
| f32, advection disabled | 0.0 % (exact) |
| f32, full pipeline | +132 % |

- `transfer_pigment` is algebraically conservative — `pigC + depC` is invariant —
  but each side was rounded to `rgba16float` independently 240×/second.
  **Fixed:** the four pigment fields are now `rgba32float`, where the same
  exchange conserves to the decimal. Water, saturation and solver scratch stay
  f16.
- `update_velocities` and `project` ended with `vel *= water.w`. Scaling a freshly
  divergence-free field by a fractional mask makes it divergent again exactly at
  the stroke boundary, and semi-Lagrangian advection duplicates pigment there.
  **Fixed:** removed; the mask is already a Dirichlet condition on `dp` in
  `jacobi.wgsl`, which is where it belongs. Cut mass creation from +132 % to
  +49 %.

The remaining +49 % was the semi-Lagrangian resample itself, replaced by the
donor-cell scheme described in the README's *Conservative advection*.

## Edge darkening — found, after four attempts

`project.wgsl` folded the pressure correction into `water.z`:

```wgsl
textureStore(waterDst, p, vec4<f32>(vel, water.z + dp, water.w));  // wrong
```

That channel is the physical **water depth** — `splat` adds to it,
`capillary_flow` absorbs from it, `flow_outward` lowers it at the rim. `dp` is a
pressure *correction* for the velocity solve only. Folding it in drove the depth
to zero within a single Jacobi iteration.

The measurement that found it, with the paper-slope force disabled so only
pressure-driven flow remains:

| `jacobiIterations` | mean speed (before) | mean speed (after) |
|---|---|---|
| 0 | 0.299 | 0.299 |
| 1 | **0.000** | 0.217 |
| 40 | **0.000** | 0.076 |

One iteration annihilating the flow is not what a projection does. With it fixed
the pressure gradient drives flow again, `FlowOutward` has something to act on,
and strokes get a proper dark rim.

This also explains the earlier symptom that `paperSlope` was the *only* thing
moving water: slope-driven flow has structure that survives projection, while
radial pressure-driven flow was exactly what was being destroyed.

**Three earlier hypotheses were wrong** and are recorded in `shaders/project.wgsl`
so they are not re-tested: the magnitude of the `FlowOutward` nudge (raised 50×,
no change), over-projection (relaxed to 0.55, no change), and vanishing velocity
(speed measured a healthy ~0.07 px/step).

## Diffusion: not the bug

Worth stating plainly, since it was the reported symptom. Comparing at t = 20 s:

| config | `pigCells/wetCells` | look |
|---|---|---|
| diffusion on | 1.00 | smooth, mottled by paper grain |
| diffusion off | 0.85 | sharp radial streaks and voids |
| granulation off | 1.00 | *identical* to granulation on |

Diffusion was **masking** a flow artifact, not causing one. Turning it off makes
the blotching worse, and granulation is not involved at all. The blotching traced
back to the `project.wgsl` bug above.

## Hollow washes — a missing ceiling, not a rate

`splat` added `brushWater * dt` to the water depth every frame with no cap, so
dwell time alone built a pressure head **~20× the paper's fibre capacity** (depth
~20 against a capacity ~0.9). The outward flow then never stopped and emptied the
wash into its own rim.

Surface tension limits how deep a film sits on paper before it runs, so depth is
now clamped to `maxFilm`. Swept first and ruled out: `edgeDarkening` (0.05 – 1.5,
barely moved it) and the deposition rates (`RATE_SCALE`). Both were the wrong
knob — the problem was an unbounded accumulation, not a rate.

`paperSlope` also came down 2.2 → 0.9. It had been raised while the pressure term
was broken and was the only thing moving water; with pressure working it
over-channelled flow into the paper's valleys and left mottled voids.

## Ink that would not bleed

Two compounding causes, the first dominant:

- **Settle saturated.** `density * dt * (1 + 3exp(-8v)) * 2.5` reached **2.75 and
  clamped to 1.0** — every suspended particle fixed to the fibres on the first
  step. Ink that deposits instantly cannot travel, so no amount of loosening the
  lattice would have helped. Now scaled by `settleScale`.
- **Lattice pinned.** `kappa` summed to ~0.75 of a 0.98 maximum, bouncing three
  quarters of every streaming step straight back. Blocking 0.42 → 0.10, grain
  0.45 → 0.22, glue 0.10 → 0.04.

Reach grows only as `sqrt(steps)` — the LBE spreads diffusively at
`nu = (1/omega - 1/2)/3`, so 2 substeps moved a mark ~5 px in a couple of seconds
against the ~25 px millimetre-scale bleed needs. Ink now has its own `inkSubsteps`
(8), affordable because its passes are cheap (4 dispatches, no Poisson solve), and
`omega` dropped 1.30 → 0.70, raising `nu` from 0.09 to 0.31.

One consequence worth knowing: total ink accepted *drops* as percolation speeds
up. That is MoXi's receptivity mask `max(1 - rho/lambda, m)` working as designed —
wet paper takes less ink — not mass loss. `receptivity` was raised 0.65 → 1.20 so
tone stays rich.

## Oil ridges — closed by 1.3 / ADR-0003

Oil strokes showed faint periodic ridges. Deposition across all three media is now
driven by an arc-length dab emitter (`src/brush/StrokePath.*`) instead of one
swept capsule per rendered frame, and every deposition term had its `* P.dt`
scaling removed — a dab deposits a fixed quantity per unit of brush travel, never
per unit of time. For oil specifically this replaces per-frame stamping (the
actual cause of the ridges) with stamping at a fixed spatial cadence.

**Confirmed:** pigment mass is provably speed-independent — `--selftest`'s
stroke-speed case matches fast vs. slow strokes over an identical path to within
0.0%, well inside its 5% tolerance.

**Not independently re-confirmed:** the *visual* ridge reduction under impasto
lighting specifically. Worth an eyeball check next time an oil stroke is painted
interactively.

---

## Measurements pending re-verification

Both tables below were taken **pre-1.3**. Water depth deposited per stroke also
lost its `* P.dt` scaling in that change (ADR-0003, the same change as pigment)
and roughly doubled along with it. Position and timing *dynamics* are unlikely to
have shifted, but the magnitudes have, and neither table has been re-measured.
Flagged rather than left silently stale.

### Working time calibration

Measured against `--diag`, which reports when the canvas actually goes dry:

| set | measured |
|---|---|
| 2 s | 2.75 s |
| 5 s | 6.0 s |
| 15 s | 13.7 s |
| 20 s | 17.2 s |

Good to ~15% across the slider's range.

### Board tilt deflection

Deflection of deposited pigment, 20 s working time over a 40 s run:

| tilt | y-offset |
|---|---|
| 0 | −0.9 px |
| 0.10 | 29.4 px |
| 0.25 | 56.3 px |
| 0.50 | 77.1 px |

Sideways drift stays under 1 px throughout, so the force is on the axis it should
be. `kMaxTilt` ended at 0.50 — the same value that produced a degenerate hollow
sweep *before* water advection existed, and a proper teardrop after.

**One regression from the extra pass:** pigment conservation is no longer exact.
It loses 0.13% during the transport phase (112969.3 → 112821.7) and then goes
completely flat once the canvas dries — the loss tracks motion, so it is f32
rounding across an extra donor gather rather than a leak.
