# ADR-0007 — Erase is mass reduction, not a colour

**Status:** accepted · **Date:** 2026-08-17

## Context

The plan had no eraser. Not a forgotten menu item — the word appeared nowhere in the
design, and the reason it was easy to miss is that on a **Pigment layer** it has no
obvious meaning.

On an RGB layer erasing is unambiguous: reduce alpha. On a Pigment layer the pixel is a
**Latent** plus a **Mass**, and mixing is *not invertible* — once cadmium red and
ultramarine have been mixed into one latent, there is no operation that recovers either.
So "erase" cannot mean "remove the last thing painted".

On a **Media layer** it is worse, because there are three physical strata: the **film**
(`water.z`), the **saturation** (capillary reservoir) and the **deposit** (dry pigment).
Erasing could plausibly mean any of them.

The temptation is to make the eraser a brush that paints the background colour, which is
what MacPaint did and what Photoshop still does on a locked Background layer. On a layer
with real alpha that is wrong, and on a Pigment layer it is wrong twice — it *adds* white
pigment, which under Kubelka-Munk is opaque paint, not absence of paint.

## Decision

**The eraser is the brush with a negative deposit step.** F5 already says one brush works
on every layer kind and only the deposit differs; the eraser is that statement taken
seriously. It inherits the whole modulation matrix — pressure, tilt, jitter, spacing,
grain — because an eraser with no dynamics is useless for the drawing work it is for.

Per layer kind:

| kind | erase means |
|---|---|
| **RGB** | reduce alpha toward zero, premultiplied-correct |
| **Pigment** | reduce **Mass** toward zero; **leave the Latent untouched** |
| **Media** | reduce the **deposit**, and *optionally* lift film and saturation — see below |
| **Strokes** | remove dabs whose footprint the eraser covers, not pixels |
| **Adjustment / Text / Flats** | erase paints into the layer's **mask**, since there are no pixels to remove |

**Pigment: mass, not latent.** Reducing mass while holding the latent means a half-erased
pixel is *less paint of the same colour*, which is what an eraser does to a drawing. It
also keeps the invariant the whole pigment design rests on: mass is a linear quantity, so
scaling it is a valid operation on latents, whereas pushing the latent toward "nothing"
is not defined.

**Media: two tools, named honestly.** Erasing a Media layer's *dry* deposit is the direct
analogue and is what the eraser does. Removing *wet* paint is a different physical act —
blotting with a dry brush or tissue, a real watercolour technique — and it removes film
and saturation while leaving most deposit behind. These are different enough that
conflating them would confuse the result. The eraser removes deposit; **Blot** is a
separate brush mode that removes film and saturation. Blot is P2.

**Strokes: remove dabs, not pixels.** A Strokes layer has no pixels of its own — it is
replayed. Erasing pixels would be discarded on the next re-evaluation. So the eraser
deletes the dab records it covers, and the layer re-derives. This is the only kind where
erasing is a structural edit rather than a value change.

## Consequences

- The brush engine needs a signed deposit, decided per layer kind, not a separate eraser
  tool with its own code path. Phase 10 builds both directions at once.
- Erasing on a Pigment layer that has been mixed is **lossy and correct** — you get less
  of the mixed colour, never the original components back. That is what paint does, and
  the UI should not imply otherwise.
- `Blot` is a new brush mode on Media layers, and belongs with the wet-mix work rather
  than with the eraser.
- Erase-to-history and the background/magic erasers are **not** adopted. Erase-to-history
  needs the non-linear history model this design does not have, and the two smart erasers
  are selection tools wearing an eraser costume — the magic wand plus delete does the same
  job with fewer surprises.

## Alternatives rejected

**Erase paints the background colour.** MacPaint's model. Wrong on any layer with alpha,
and on a Pigment layer it deposits white paint — opaque under KM, so it lightens *and*
becomes something you then have to erase.

**Erase interpolates the latent toward white.** Plausible, and wrong for the same reason:
white is a pigment, not the absence of one. It would make a half-erased red into pink
rather than into thin red.

**One eraser that removes everything on a Media layer** (film, saturation and deposit
together). Simpler to implement and physically meaningless — nothing in watercolour
removes dry deposit and standing water in the same gesture. Split into eraser and Blot.
