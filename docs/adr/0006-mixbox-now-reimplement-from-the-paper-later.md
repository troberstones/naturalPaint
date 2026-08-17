# Use Mixbox now; reimplement from the paper before any distribution

Mixbox is the load-bearing piece of the product's differentiator — it is what makes
blue over yellow give green — and it is **CC BY-NC**. We ship on it now and accept
non-commercial-only terms, and reimplement the method from Sochorová & Jamriška 2021
(`papers/Sochorova2021_PracticalPigmentMixing_Mixbox.pdf`) before any commercial
distribution. The method is published; only the implementation is licensed.

`NP_USE_MIXBOX=OFF` already exists as the seam, falling back to a plain two-constant
Kubelka-Munk mix with no encumbrance.

## Considered options

- **Reimplement first.** Removes the encumbrance before any code depends on it, but it
  is pure research risk with no user-visible benefit, and it blocks every phase behind
  a spectral-fitting problem.
- **Ship on the `km2` fallback.** Unencumbered today, but it is the reason Mixbox is
  the default — the fallback's mixing quality is the whole thing being traded away.
- **Licence commercially from Secret Weapons.** Fastest and lowest-risk if distribution
  is ever actually imminent. Not ruled out; it stays the cheaper alternative to
  reimplementation if the fit turns out to be hard.

## The encumbrance travels with documents, not just the binary

> ⚠️ A saved file containing **Latent**s carries Mixbox's terms. This is not only a
> build-configuration question — it is a question about every document produced before
> the swap.

And a reimplementation will be a **third basis**, not a drop-in: its latents will not
be numerically identical to Mixbox's, because the primary pigments and the fit differ.
So `mixbox-v1`, `km2-v1`, and a future `np-km-v1` are three mutually unreadable
encodings of the same idea.

**Therefore, two rules, both already partly in the design:**

1. Every file stamps `pigmentModel`.
2. **Every file embeds a baked RGB composite** (PRD I4). This is what makes the swap
   survivable — a Mixbox-era document stays openable forever as RGB, losing only its
   re-mixability as pigment.

This upgrades the earlier "refuse a basis mismatch" rule to something more useful:
opening a foreign-basis document **offers to open the embedded RGB composite** rather
than failing.

## What the reimplementation involves

Recorded so the scope is not rediscovered later:

- **Spectral K/S data for the primary pigments.** The crux. The method is published;
  whether the measured spectra are is the first thing to check in the paper. If not,
  either public spectral databases or own measurements are needed.
- Kubelka-Munk two-constant mixing evaluated over spectral bands.
- An offline constrained fit per LUT cell — find pigment weights minimising perceptual
  error between the KM mix and the target colour, with a residual absorbing what is
  left. Runs offline, so conditioning matters and speed does not.
- Spectral → XYZ → sRGB for the forward path, and the LUT bake.

The risk is that a reimplementation does not match Mixbox's perceptual quality. It is
gated behind *distribution* rather than behind features, so there is time to iterate —
and the commercial licence remains the fallback.

## Consequences

- Non-commercial use only until this is done. Personal use and research are unaffected.
- `NP_USE_MIXBOX=OFF` must be **verified working**, not assumed. An unexercised build
  option is not a seam. Added to the phase 1 checklist.
- The `Latent` abstraction in `CONTEXT.md` must stay basis-agnostic. Nothing outside the
  pigment module should assume three weights plus a residual.
