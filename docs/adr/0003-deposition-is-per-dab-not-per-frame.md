# Brush deposition is per-dab and dt-independent, in every medium

`splat.wgsl` originally swept one capsule between `brushA` and `brushB` per frame
and scaled deposition by `P.dt`, making the amount of paint laid down proportional
to *time* rather than to *distance*. Moving fast deposited thinly over a long
segment, moving slowly piled up, and holding still kept depositing — which is why
`oil_transfer.wgsl` carries a `velocityCutoff` to stop the brush oozing. Deposition
is now driven by the brush engine's arc-length dab emitter: a dab every
`spacing × radius` pixels, each depositing a fixed amount **not** scaled by `dt`.

> ⚠️ **Do not "fix" this by reintroducing a `dt` term.** Every other term in the
> solver is correctly `dt`-scaled — advection, diffusion, evaporation, absorption —
> because they are rates. Deposition is not a rate; it is a quantity per unit of
> brush travel. Making it `dt`-proportional is the bug, not the convention.

## Consequences

- Closes Known Bug #2 (periodic ridges in oil from per-frame stamping), which the
  README already identified as needing rate-independent stamping.
- `velocityCutoff` in `oil_transfer.wgsl` becomes unnecessary — a stationary brush
  emits no dabs because it accumulates no arc length.
- Stroke density stops depending on hand speed. Where that dependence is *wanted*
  it becomes an explicit velocity→flow link in the dynamics matrix rather than a
  property of the integrator.
- Media layers inherit the whole dynamics matrix — pressure, tilt, velocity and
  jitter curves driving a fluid solver, which was the main argument for one brush
  engine across all three layer kinds.
- Regression gate: `--diag`'s pigment-mass table must stay flat once the brush
  lifts. This change touches the one part of the codebase with hard-won measured
  conservation properties, so the existing instrumentation is the acceptance test.
