# A Media layer's solver runs in a transient window around the wet region

The watercolour field set costs **184 bytes per pixel**, so a full-document Media
layer is 1.53 GB at 4K and 6.1 GB at 8K — untenable, and a hard conflict with
ADR-0001. Instead each Media layer allocates a *solver window*: a transient
bounding box in **document space** around its currently-wet cells, capped at 1024².
Drying is the handoff — wet pigment lives in the window, dry pigment is baked into
tiles and the window is freed.

Anchoring the window to the document rather than the viewport is the load-bearing
part. ADR-0001 classifies live solver state as *not reconstructible*, so a
viewport-following window would let an ordinary pan silently destroy a wash that
was still bleeding.

## Considered options

- **Stroke-scoped window** — allocate at pen-down, bake at pen-up. Simplest and
  bounded by construction, but it kills every effect that continues after the pen
  lifts (bleeding, blooms, backruns) and contradicts the 15-second `workingTime`
  the solver already models.
- **Full-document Media layers with a document size cap** — no transient anything,
  but it caps simulated painting near 1200² for a 250 MB budget, far below where
  visdev works, and makes "add a watercolour layer" fail on most real documents.

## Consequences

- **Exceeding the cap refuses further wetting** rather than silently baking. The
  Water tool can hit this, so the limit has to be visible in the UI — a silent
  bake would look like the simulation had broken.
- Two washes far apart would explode a single bounding box, so a layer holds a
  small number of windows (2–4) and bakes the oldest when it needs another.
- A stroke longer than the window forces a rolling bake behind the brush. Acceptable
  for painting, but it means a single very long stroke is not perfectly reversible
  in the solver.
- Media layers therefore have a *wet* extent and a *dry* extent, and only the dry
  extent is what gets saved. Wet state does not survive a document close.
