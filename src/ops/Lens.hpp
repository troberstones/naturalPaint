#pragma once

#include <cstdint>

#include "core/TileStore.hpp"
#include "ops/Roi.hpp"
#include "ops/Transform.hpp"

// ops/Lens (PRD D22, PLAN.md Phase 19 step 5 / docs/automation-plan.md §6
// step 8) -- barrel and pincushion correction plus lateral chromatic
// aberration, as a class-B spatial op over one layer.
//
// ==========================================================================
// (1) WHY THIS IS NOT `transformImage()` -- and what it still borrows from it
// ==========================================================================
//
// Everything else geometric in this tree goes through `ops/Transform`'s one
// resampler, and the whole point of that file is that pixels are touched once
// per commit by one gather loop. This op cannot use it, and the reason is not
// a preference: `transformImage()` takes a `Mat3`, and a lens distortion is
// **not affine and not even projective**. A radial polynomial moves a texel by
// an amount that depends on its own distance from the optical centre; no 3x3
// homogeneous matrix can express that, so there is no matrix to hand it.
//
// What this file therefore does NOT do is invent a second sampler. It reuses:
//
//   * `resampleKernelRadius()` / `resampleKernelWeight()` -- the same five
//     kernels, the same coefficients. There is one Catmull-Rom in this build.
//   * `imageFromTileStore()` / `tileStoreFromImage()` -- the same flat,
//     linear-light, **premultiplied** rgba float bridge, so nothing here has
//     to re-derive the half round trip or the premultiplied-averaging
//     argument (ops/Transform.hpp section 2).
//   * `transformImage()`'s edge policy and its weight normalisation, verbatim
//     in behaviour and cited at the loop: a destination whose source position
//     lands outside the source rectangle stays transparent black, taps inside
//     it clamp to the border texel, and the gathered weights are normalised
//     **over the whole footprint**.
//
// That last one is the trap ops/Transform.hpp names and this file inherits
// unchanged. Nearest, bilinear, Catmull-Rom and Mitchell are partitions of
// unity, so dividing by the weight sum is a no-op for them and it is tempting
// to skip. **Lanczos3 is not**: its weights sum to 1 only to about 1e-3, and
// unnormalised that residual is a periodic brightness ripple across the whole
// picture -- not an edge artefact, a whole-image one. And because every tap is
// clamped into range rather than dropped, the sum is over the *full* footprint
// and every term of it contributes, which is what makes the same division
// correct at the border as in the middle. Normalising over the *clipped* sum
// instead is the difference between a border that fades and a border that
// smears, and both look plausible in a thumbnail.
//
// ==========================================================================
// (2) THE MODEL, WRITTEN OUT, BECAUSE THE SIGN IS THE THING PEOPLE GET WRONG
// ==========================================================================
//
// Brown-Conrady's radial terms, in the **destination-to-source** direction --
// which is the direction a gather needs and the direction the coefficients are
// therefore defined in here:
//
//     c    = the centre of `frame`
//     R    = half the diagonal of `frame`         (so r == 1 at the corners)
//     d    = dstTexelCentre - c
//     r2   = |d|^2 / R^2
//     s(r) = 1 + k1*r2 + k2*r2^2
//     srcPos = c + d * s(r)
//
// So **k1 > 0 samples from further out than the destination texel sits**,
// which drags the outer picture inward -- the correction for *barrel*
// distortion, the bulge a wide-angle lens produces. k1 < 0 samples from
// further in and pushes the picture outward, correcting *pincushion*. A
// caller who wants to simulate a distortion rather than correct one passes the
// opposite sign; there is no separate mode, because there is no separate
// arithmetic.
//
// `r` is normalised by the half-diagonal and not by the half-width. That is
// the choice that makes a coefficient **resolution- and aspect-independent**:
// the same k1 authored on a 2000x1500 plate does the same thing to a 4000x3000
// one, which is exactly docs/automation-plan.md §5's "resolution-dependent
// parameters carry their unit" rule satisfied by having no unit at all.
//
// **Lateral chromatic aberration** is one more radial scale, per channel:
//
//     srcPos_R = c + d * s(r) * (1 + caRed)
//     srcPos_G = c + d * s(r)
//     srcPos_B = c + d * s(r) * (1 + caBlue)
//
// Green is the reference channel and takes no scale of its own, because CA is
// only ever measurable as a *difference* between channels; giving all three a
// scale would make one of the three redundant with `k1` and let a user cancel
// a distortion with a colour control.
//
// **Alpha follows green, and that is a decision with a cost.** Alpha is
// coverage, not a colour: a texel cannot be three different amounts present at
// once, and this working space is premultiplied, so there is no representation
// in which R could carry its own alpha. The consequence, named rather than
// discovered: on a layer whose content is partly transparent, the R and B
// channels are gathered from a position whose own coverage differs from the
// alpha stored beside them, so a strong CA correction across a soft edge can
// push a channel above the `rgb <= a` display-referred inequality. This
// working space does not claim that inequality (ops/Transform.hpp section 2
// makes the same point about kernel overshoot), and CA correction is a
// photographic operation on opaque plates. It is not clamped, for the same
// reason nothing else here clamps: crushing a scene-referred highlight to fix
// an inequality the space never promised is the worse of the two errors.
//
// ==========================================================================
// (3) THE SEAM PROPERTY, AND WHY `frame` IS A PARAMETER AND NOT `outRect`
// ==========================================================================
//
// ops/Blur.hpp's invariant -- an output texel's value must not depend on which
// rectangle the caller asked for it in -- is the one every op in this tree
// inherits, and it is the reason `LensParams::frame` exists. The optical
// centre and the normalising radius are properties of the *picture*, not of
// the request: derive them from `outRect` and asking for the left half and the
// right half separately would centre two different lenses and produce a visible
// discontinuity down the middle. `frame` is the document canvas, is supplied
// by the caller, and `outRect` only ever chooses which of the answers get
// written.
//
// The gather itself is global -- a destination texel can read from anywhere in
// `frame` -- so there is no apron that would make this an `ops/Roi` op. That
// is stated rather than left as an omission: `roiBackward()` for a lens is the
// whole frame, which is a correct ROI and a useless one.
//
// ==========================================================================
// (4) WHAT IS NOT HERE
// ==========================================================================
//
//   - **Tangential (decentring) distortion**, Brown-Conrady's p1/p2 terms. A
//     real effect on a badly centred lens and a small one; adding it is two
//     more coefficients in `lensSourcePosition()` and nothing else, and it is
//     absent because no calibration source in this build supplies them.
//   - **Vignetting.** A radial *intensity* correction, not a geometric one. It
//     belongs with the tone ops, not here.
//   - **Lens profiles.** Reading a camera/lens database and filling these
//     coefficients in is the useful product feature; this is the op it would
//     drive.
//   - **Boundary antialiasing**, inherited absent from ops/Transform: the
//     frame's own edge comes out hard rather than coverage-weighted.
namespace np {

struct LensParams {
  // Radial coefficients, destination-to-source. See section 2 for the sign.
  float k1 = 0.0f;
  float k2 = 0.0f;

  // Lateral chromatic aberration, as a fractional radial scale on R and B
  // relative to G. 0.001 is a strong correction; a per-mille scale over a
  // 2000-texel half-diagonal is a two-texel shift at the corner.
  float caRed = 0.0f;
  float caBlue = 0.0f;

  ResampleKernel kernel = ResampleKernel::CatmullRom;

  // The picture the lens is defined over -- the document canvas. Section 3.
  // Required, and must be non-empty.
  PixelRect frame{};

  // Whether a request with every coefficient at zero short-circuits to a
  // verbatim copy instead of running the gather.
  //
  // Defaulted true because it is the correct behaviour and not merely the fast
  // one -- `resizeImage()`'s 1:1 case makes the identical argument: "a resize
  // that changes nothing must not perturb a value". A reconstruction kernel is
  // an identity at exact integer offsets only to within weight rounding, and
  // for Lanczos3 `sin(pi*t)` at integer `t` is not exactly zero, so without
  // this a zero-strength correction would quietly rewrite every texel.
  //
  // The `false` setting exists for exactly one caller, --selftest, in the
  // shape `TransformParams::allowExactPaths` already established: a claim that
  // the short-circuit is not *hiding* a wrong gather is only worth anything if
  // the gather can be made to run on the identity case and be measured. It is
  // not a user-facing setting and nothing in the app sets it.
  bool allowExactIdentity = true;
};

// Refuses, by returning false, a request that is not finite, one whose frame
// is empty, and one whose radial map folds the picture through itself.
//
// That last check is worth its cost. `r -> r * s(r)` must be strictly
// increasing on [0, 1] for the map to be a bijection of the picture onto
// itself; a large enough negative k1 turns it around, and past the turn two
// different destination radii read the *same* source ring, so the output is a
// mirrored halo that looks like a lens effect rather than like a bug. The test
// is a **sampled** one -- 257 steps over [0, 1], which is one step per texel
// on a 512-texel half-diagonal -- so a pathological k1/k2 pair that dips and
// recovers between two samples passes. That is a bounded, named approximation
// and not a claim of exactness; an exact test means solving a cubic in r2 and
// buys nothing at the coefficient magnitudes a lens has.
bool lensParamsValid(const LensParams& p) noexcept;

// The source position one destination texel centre reads, for one channel.
//
// `channel` is 0=R, 1=G (and alpha), 2=B; anything else is treated as green.
// Exposed because it is the entire semantic content of the op -- the sign
// convention, the half-diagonal normalisation and the CA reference channel all
// live in these four lines -- and a test that retyped it would be checking its
// own copy rather than the shipped one.
//
// `dst` is a texel *centre*, i.e. (x + 0.5, y + 0.5): ops/Transform.hpp's
// half-integer convention, and getting it wrong here is the classic half-pixel
// shift, which on a radial map presents as a picture that creeps as the
// strength slider moves rather than as an obvious offset.
Point2 lensSourcePosition(const LensParams& p, Point2 dst, int channel) noexcept;

// The engine, in the shape `app/PixelOpBridge.hpp` dispatches:
// `(const TileStore&, const PixelRect&, const Params&, TileStore*) -> bool`.
//
// Returns false without writing anything for a null or aliased `dst`, an empty
// `outRect`, or params `lensParamsValid()` rejects. `app/PixelOpBridge`'s
// `computePixelFilter()` reads a false here as "the op is a no-op", which is
// the correct reading: none of these is a `PixelOpRefusal` and none of them
// may corrupt the layer.
bool lensCorrectTiles(const TileStore& src, const PixelRect& outRect, const LensParams& p,
                      TileStore* dst);

}  // namespace np
