# The solver runs on a fixed timestep, and undo replays dabs from a keyframe

Undo needs one model across every layer kind. Four of the five are trivially
snapshottable, but a Media layer's state is ~193 MB with no inverse — paint has
been moving for seconds, so there is nothing to undo *to*. Rather than special-case
it, the solver is pinned to a **fixed `dt`** driven by an accumulator, dabs are
recorded, and undo replays: restore the nearest solver keyframe, then replay the dab
stream forward with the undone stroke omitted. Determinism makes this exact.

## This requires keyframes, not just a dab log

> ⚠️ Replay cost is the binding constraint. Watercolour runs ~240 steps/second at up
> to ~50 dispatches per step, so replaying twenty seconds of solve is ~240,000
> dispatches — unusable as an undo press. The solver window is therefore snapshotted
> every K seconds and replay starts from the nearest keyframe.
>
> **K directly sets undo latency**, and each keyframe costs 193 MB, so one or two
> are kept. K ≈ 2 s puts replay near 24,000 dispatches, which is sub-second.

## Consequences

- **Fixed `dt` pays for itself independently of undo.** Physics decouples from frame
  rate, so a slow frame produces more substeps rather than different behaviour;
  `--diag` becomes reproducible rather than approximate; and the solver becomes
  regression-testable, which the conservation work recorded in `README.md` would
  plainly have benefited from.
- Undo is stroke-granular and uniform across all layer kinds — no per-kind rules to
  explain in the UI.
- Undoing a stroke from the *middle* of a wet session is now meaningful, not just
  the most recent one, because replay reconstructs everything after it.
- Replay must be pure: any dependence on wall-clock time, frame timing, or
  unseeded randomness breaks it. Dab jitter is already required to seed from
  `(strokeId, dabIndex)` for a separate reason, and this makes that mandatory rather
  than merely advisable.
- Rejected: **per-kind undo units** (Media rewinding only to its last bake) — simpler
  and bounded, but it makes undo mean two different things and gives up
  stroke-granular undo exactly where painting is most exploratory. Also rejected:
  uniform per-stroke snapshots of the solver window, at 193 MB per history step.

## Amendment — redo, and history as a list

*Added 2026-08-17. The original record specified undo and never mentioned redo, which
turned out not to be an omission that could be fixed later.*

**History is a linear list with a cursor**, not a stack. Undo moves the cursor back, redo
moves it forward, and the entries are never discarded until a *new* edit is made at a
cursor that is not at the end — at which point everything after it is dropped. Standard
behaviour, and worth writing down because the replay model makes one part of it
non-obvious:

> **Redo is not the inverse of undo — it is the same replay with a longer dab stream.**
> Because undo already works by replaying from a keyframe with the undone stroke omitted,
> redo is that identical operation with the stroke included again. There is nothing to
> invert and no second mechanism to build. It falls out for free *provided* history is a
> list with a cursor from the start; retrofitting it onto a stack would mean rebuilding
> the traversal.

**A History panel lists the entries** with the tool or op that produced each, and clicking
one moves the cursor there directly. For Media layers, jumping backward N entries costs
one replay from the nearest keyframe rather than N replays.

**Snapshots are explicit, user-created history entries** that hold a full document state
and are exempt from A9's byte-bounded eviction until dismissed. This is what makes "try
something risky" safe when the automatic tail may have been spilled or dropped.

**Not adopted: non-linear history** (Photoshop's history brush, painting from an earlier
state). It requires keeping divergent branches materialised, which fights A9's byte bound
directly, and the snapshot mechanism covers the case people actually reach for.
