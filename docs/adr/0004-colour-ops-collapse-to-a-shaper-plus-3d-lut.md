# Colour ops collapse into a shaper 1-D LUT plus a 3-D LUT

Every per-pixel colour op — levels, curves, exposure, saturation, grayscale,
channel mixer — is a pure `f(rgb) → rgb`, so any maximal run of adjacent ones
composes into a single function. That function is baked onto a 32³ grid and applied
in one texture fetch, preceded by a 1-D log *shaper* that maps scene-linear values
into the LUT's [0,1] domain.

The deciding factor is that op stacks multiply: there is one per layer *plus* one
per document. Eight layers carrying three grade ops each is 24 evaluation passes
per frame ≈ 1.4 GB of traffic, around 85 GB/s at 60 fps — past a base M1's total
memory bandwidth. Collapsed, it is two passes regardless of depth.

## The part that is hard to reverse

> ⚠️ **Curves are authored in the shaper's log domain, not in linear.** This is a
> *format-level* commitment, not an implementation detail: saved curve control
> points are coordinates in that domain, so changing the shaper later silently
> shifts every grade in every saved document.

It is also what makes the curve UI usable at all — on raw linear values everything
interesting crowds into the bottom 5% of the graph — and it is what gives the 3-D
LUT a bounded domain when linear values exceed 1.0. One mechanism, three problems.

## Consequences

- It is each **maximal run** of adjacent point ops that collapses, not the stack as
  a whole. `levels → blur → curves` bakes two LUTs around one blur pass.
- Ops must un-premultiply before evaluating and re-premultiply after, or partially
  transparent pixels grade wrongly.
- A 32³ LUT cannot represent a hard posterize or a near-vertical curve segment; 64³
  (2 MiB, still cache-resident) is the escape hatch.
- Hardware trilinear filtering on the LUT can drift hue near the neutral axis.
  Tetrahedral interpolation is the fix Resolve and OCIO use, at the cost of giving
  up hardware filtering. Not needed initially.
- Rejected: direct per-op ping-ponged evaluation, deferring the LUT until measured.
  Cheaper for a single shallow stack, but it does not survive per-layer stacks.
