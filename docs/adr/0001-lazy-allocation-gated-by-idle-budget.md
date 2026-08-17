# Heavy subsystems allocate lazily, per mode, gated by an idle-memory assertion

*Lightweight* for this project means fast start and near-zero memory when nothing
is loaded — a resource property, not a limit on features. `main.cpp` currently
violates it by ~294 MB: `PaintSim` is constructed unconditionally at startup and
allocates every field for all three media, including the 100.7 MB ink-only D2Q9
lattice, before any document exists. So: no heavy subsystem is constructed until
first use, the simulation allocates only the *active* medium's fields, and
`--selftest` asserts an idle RSS ceiling so the invariant fails the build rather
than drifting.

## Considered options

- **Per-subsystem only** — build `PaintSim` on first Sim-layer use, but keep
  allocating all three media's fields. Gets idle to zero with far less code, but
  any paint layer costs the full ~294 MB.
- **Texture pool keyed by (dimensions, format)** — recycle field textures across
  media and documents. The only option that *bounds* total GPU use rather than
  deferring it, but it is a real allocator with lifetime tracking and aliasing
  bugs that are unpleasant to debug.
- **Per-subsystem and per-mode** — chosen. Switching medium already clears the
  canvas, so freeing the outgoing medium's fields discards nothing that was not
  already being discarded.

## Consequences

Measured targets at a 1024² simulation canvas:

| state | GPU fields |
|---|---|
| idle, no document | 0 MB |
| image document, no paint layer | 0 MB |
| watercolour layer | ~193 MB |
| ink layer | ~210 MB |
| oil layer | ~160 MB |

The pool option stays available later if several documents with paint layers turn
out to be a common case — per-mode laziness does not preclude it.

### Amendment: *visible*, not *active*

Documents are presented as tabs with an optional two-tab split, so up to two can be
on screen at once. The rule is therefore **only visible documents hold GPU
textures, at most two** — not "only the active document". View tiles are bounded by
viewport rather than image size, so two visible documents roughly double a small
number (~30 → ~60 MiB at 2560×1440) and the per-document LUT is negligible. The
ceiling stays bounded; only the predicate changed.

A visible-but-unfocused document **keeps stepping its solver**, because a frozen
wet wash sitting on screen looks like a bug. Hidden tabs bake their wet extent and
release everything reconstructible.

Non-obvious costs: every subsystem accessor becomes fallible or
construct-on-demand, which is a pervasive shape change rather than a local one;
and the `--selftest` RSS ceiling will occasionally fail for reasons unrelated to
the change under test (allocator behaviour, driver versions), so it needs
headroom rather than a tight bound.
