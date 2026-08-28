#pragma once

#include <cstdint>

#include "core/TileStore.hpp"
#include "ops/Blur.hpp"
#include "ops/Roi.hpp"

// ops/Filters (PLAN.md "Phase 6 -- Filter and transform it": "highpass as
// `src - blur(src)` · unsharp · offset with wrap · then the rest of the filter
// set: sharpen ... add noise ... local contrast"; DESIGN-imaging.md "Class B
// -- parametric spatial ops -> live passes with ROI").
//
// The filter set that hangs off the blur spine. Everything here is built on
// ops/Blur and ops/Roi rather than beside them, and that is a correctness
// decision before it is a tidiness one: ops/Blur.hpp's apron property -- **a
// blur split across a tile boundary is bit-identical to one computed in a
// single call** -- is the invariant this whole phase rests on, and an op that
// grew its own convolution would have to re-earn it. Six filters, one kernel,
// one gather -- plus three more (sections 7-9) added later, each of which
// earns the same property by a different route: section 7's emboss is a
// second, smaller linear combination of premultiplied samples rather than a
// reuse of `ops/Blur`; section 8's median is a rank statistic, not a
// combination at all, and keeps the property only by refusing the standard
// fast algorithm for one (see that section for why); section 9's motion blur
// is the first apron in the file whose two axes are not interchangeable.
//
// ==========================================================================
// The invariant every op in this file inherits, and what it costs to keep
// ==========================================================================
//
// ops/Blur.hpp's seam section is the thing to read first. Its one-line form:
// an output texel's value must not depend on which rectangle the caller asked
// for it in. A blur that reads only its own tile is exactly right in the
// middle of every tile and wrong by 0.467 -- nearly half of full scale -- on a
// grid of lines every 128 texels, which is a bug that survives review.
//
// Each op below states how it keeps the property, because for two of them it
// is *not* free:
//
//   highpass, unsharp, sharpen, local contrast   inherited. They gather
//       `roiBackward(blurRoiOp(p), outRect)`, blur that plane once, and then
//       combine it with the source texel at the same document coordinate. The
//       combine is a pure function of two texel values, so it cannot introduce
//       a dependence on the request rectangle that the blur did not already
//       have -- and the blur does not have one.
//
//   offset      trivially inherited: a pure gather, one source texel per
//       output texel, no accumulation of any kind.
//
//   **add noise -- NOT free, and this is the design decision the op is built
//       around.** A noise generator that walks a stream (`rng.next()` per
//       texel, however it is seeded) produces a value that depends on how many
//       texels came before it, which depends on the shape of `outRect`. Ask
//       for the same region as one call and as two and the grain differs
//       across the split -- the seam bug again, in a form no blur apron can
//       fix because there is no apron involved. So the noise here is
//       **counter-based**: `filterRandomUniform(seed, x, y, stream)` is a pure
//       hash of the seed and the document coordinate, holding no state at all.
//       See section 5.
//
//   emboss (section 7)   inherited, the same way as the first group: two
//       fixed-offset reads of premultiplied samples, combined pointwise. Not
//       an `ops/Blur` call, but the same shape of argument.
//
//   **median (section 8) -- NOT free, but for a different reason than add
//       noise.** Its ROI is exactly a blur's apron, `roiDilateOp(radius)`.
//       What is not free is the STANDARD fast algorithm for a windowed rank
//       statistic (an incrementally-updated running histogram), which
//       bootstraps at the edge of whatever region it is asked to process and
//       would reproduce the seam bug in rank-statistic form. Section 8 keeps
//       the property by not using that algorithm: it re-derives the true,
//       source-gathered window at every output texel instead.
//
//   **motion blur (section 9) -- NOT free.** A blur-style gather-then-combine
//       inherits the property automatically only if its apron is computed
//       correctly, and this is the first op in the file whose apron is a
//       function of a continuous parameter (the angle) rather than a
//       constant read off `blurApron()`. Getting the per-axis margin formula
//       wrong is the failure mode; section 9 works the formula out in full
//       rather than approximating it.
//
// ==========================================================================
// The domain, twice, in opposite directions
// ==========================================================================
//
// PLAN.md names two traps for this phase and this file contains one of each,
// two functions apart, so both are said out loud:
//
//   **Averaging happens in LINEAR LIGHT.** PRD principle 2. Every blur these
//   ops call runs on core/TileStore's storage exactly as it is -- linear,
//   premultiplied, rgba16float -- with no transfer function in either
//   direction. ops/Blur.hpp carries the argument and the factor-of-2.3
//   measurement.
//
//   **Fixed-amplitude changes happen in the SHAPER DOMAIN.** PLAN.md: "add
//   noise runs in the shaper domain -- fixed-amplitude noise in linear light
//   is invisible in shadow and enormous in highlight". Measured here through
//   color/Shaper (ACEScct), amplitude 0.02 of linear light, as the shaper-
//   domain (i.e. roughly visible) amplitude it turns into:
//
//       linear level    0.001     0.01      0.1      0.5      1.0      4.0
//       visible ampl.  8.19e-2   9.66e-2  1.67e-2  3.30e-3  1.65e-3  4.12e-4
//
//   A 234.6x spread. The same noise is a sandstorm in the shadows and
//   invisible in the highlights, from one number the user typed once. In the
//   shaper domain the same amplitude is a **constant ratio** in linear light
//   everywhere above the ACEScct breakpoint (2^-7): +0.02 shaper is x1.2749,
//   i.e. +27.49%, at every level. That is the number `NoiseParams::amount` and
//   `UnsharpParams::threshold` are both denominated in, and it is why they
//   share a domain rather than each picking one.
//
//   Below the breakpoint ACEScct is deliberately a straight line (it is what
//   avoids log2(0)), so shaper-domain noise degenerates back into fixed-
//   amplitude noise down there -- at linear 0.001 an amplitude of 0.05 is
//   474% of the level rather than the 64.5% it is everywhere above the knee.
//   Stated rather than hidden: the toe is a property of the published curve,
//   not of this file, and it affects only light below 1/128 of mid-grey.
//
// **The shaper round trip is free, exhaustively.** Two ops here decode a value
// out of linear light, change it, and encode it back, so the natural worry is
// that a filter at its neutral setting no longer returns what it was given.
// Measured over **all 31 744 finite positive halves**, not sampled: zero of
// them change under `floatToHalf(shaperDecode(shaperEncode(halfToFloat(h))))`.
// The worst float-domain round-trip error is 6.56e-07 over [0,1], which is
// 372x below the f16 store's own 2.441e-04 rounding, so the store cannot
// resolve it. --selftest re-runs the exhaustive check.
//
// ==========================================================================
// Premultiplied alpha: what `src - blur(src)` actually means here
// ==========================================================================
//
// This is the part of the file most likely to be got wrong quietly, so it gets
// the longest section. The working space is **linear premultiplied**
// (core/TileStore, DESIGN-imaging.md §2): a texel holds `C * A` and `A`, not
// `C` and `A`.
//
// Subtracting a blurred premultiplied image from a premultiplied image is not
// obviously the same operation as doing it to straight colour, and the two
// disagree exactly where it matters -- at a soft edge, which is the only place
// alpha varies and therefore the only place a sharpening filter can fringe.
//
// **The rule, and it is one rule: every one of the four channels gets the
// same treatment and the same coefficient.** Highpass takes the difference in
// all four; unsharp adds `amount * gain * (src - blur)` to all four, with a
// single `gain` per texel shared by the four. Nothing here sharpens RGB while
// leaving A alone.
//
// The reason is an identity worth writing out. Take a soft edge of constant
// straight colour `C` -- an antialiased cut-out, the commonest thing in a
// layered document. Then `srcRGB = C * srcA` and, because a blur is linear and
// `C` is constant, `blurRGB = C * blurA`. Unsharp with a shared coefficient
// `k` gives
//
//     outRGB = C*srcA + k*(C*srcA - C*blurA) = C * (srcA + k*(srcA - blurA))
//     outA   =   srcA + k*(  srcA -   blurA)
//
// so `outRGB / outA = C` **exactly**. The alpha edge gets sharper and the
// colour does not move at all. That is the behaviour a user wants and it comes
// out of the premultiplied formulation for free.
//
// **Rejected: sharpen RGB, copy alpha through.** It is the obvious reading of
// "sharpen the picture", it is what a straight-colour mental model produces,
// and on the same edge it gives `outRGB / srcA = C * (1 + k*(1 - blurA/srcA))`
// -- a colour that brightens on the inner side of every soft edge and darkens
// on the outer side, in proportion to how fast alpha is falling. That is the
// classic light-and-dark rim around a cut-out. --selftest computes it on
// purpose and measures how far the un-premultiplied colour drifts, so that the
// assertion the real path passes is proved *sensitive* rather than merely
// satisfied.
//
// **Rejected: un-premultiply, filter straight colour, re-premultiply.** It
// gets the constant-colour case right too, but it has to divide by alpha at
// every texel of the gather -- including the ones where alpha is near zero and
// the quotient is numerically meaningless -- and, worse, it blurs *straight*
// colour, which is the halo bug ops/Blur.hpp rejects at length (a fully
// transparent texel's arbitrary RGB gets the same vote as an opaque one's).
// The premultiplied form needs no division anywhere in the spatial pass.
//
// **What highpass's output is, said plainly: not an image.** `src - blur(src)`
// has signed RGB and signed alpha. A negative alpha is not a coverage and the
// tuple is not a valid premultiplied texel; it is a difference field, the
// operand `unsharpMaskTiles()` consumes, and it is written to a `TileStore`
// only because that is the container this codebase has. Do not composite it,
// do not feed it to `localContrastTiles()` (which assumes a valid image and
// says so), and do not expect `+0.5 grey` -- Photoshop's High Pass adds a mid-
// grey bias so the result is viewable, PLAN.md's formulation does not, and
// adding one here would make `src + amount * highpass` wrong by that bias.
//
// **Where a clamp is applied, and where one is refused.**
//
//   Unsharp clamps **alpha to [0,1] and rescales RGB with it** -- `if (outA >
//   1) { outRGB /= outA; outA = 1; }`, and `outA <= 0` zeroes the texel
//   outright. Alpha is a coverage fraction; there is no such thing as 1.3
//   covered, and core/Blend's `src + (1-a)*dst` turns an alpha above 1 into a
//   *subtraction* of the backdrop, which reads as a dark halo nobody can trace
//   back to a sharpen. Dividing RGB by the same factor is what keeps the
//   identity above intact through the clamp: the clamp reduces coverage and
//   leaves the colour alone. Clamping alpha on its own -- the obvious version
//   -- would brighten the overshoot rim by exactly the factor it clipped.
//
//   Unsharp does **not** clamp RGB. Overshoot in light is the entire visible
//   signature of unsharp masking, ops/PointOps.hpp's no-clamp policy is the
//   house rule for linear working-space values, and clipping the halo would
//   make `amount` mean something different at every brightness.
//
//   Local contrast and add noise **do** clamp their linear result to
//   `[0, kFilterMaxLinear]`, and the asymmetry is deliberate. Their excursions
//   do not come from a neighbourhood difference in the image's own units; they
//   come from the log domain's unbounded gain, where a strong amount on an HDR
//   highlight overflows the half's range. An `inf` written into a tile is not
//   a local artifact -- every later blur whose apron reaches it returns `inf`
//   for a whole neighbourhood -- so the clamp is at the storage format's own
//   limit, which leaves all the HDR headroom the format has and only refuses
//   the values it cannot hold.
//
// ==========================================================================
// Downscaling, which this file does not do
// ==========================================================================
//
// PLAN.md's other trap for this phase: "**downscale must prefilter** (area
// average or descend the mip pyramid); no reconstruction filter fixes aliasing
// after the fact". **No operation in this file changes scale.** Every one maps
// document texel to document texel at 1:1 -- the blur-based four dilate their
// ROI, offset translates it, add noise is a point op -- so none of them can
// alias, and none of them needs a prefilter.
//
// The downscale case is ops/Transform's, it is already handled there (its
// `TransformReport` names the area-average prefilter it ran), and it is
// deliberately not duplicated here. If a filter in this file ever wants the
// mip shortcut for a wide blur, note that ops/Roi.hpp says why that cannot be
// folded through `RoiOp` -- it crosses a scale boundary the type cannot
// express -- and that ops/Blur is the exact kernel such an approximation would
// have to be checked against.
//
// ==========================================================================
// Cost
// ==========================================================================
//
// The four blur-based ops hold exactly what ops/Blur.hpp's apron table says a
// blur holds -- **two planes**, the gather and `blurPlane`'s intermediate --
// and not three. The obvious implementation keeps a third: gather the source,
// blur a copy, then combine the two. This one blurs the gathered plane in
// place and re-reads the source texel from the `TileStore` during the write
// walk, which is legal precisely because every one of these ops combines the
// blurred value with the source at **the same document coordinate**, so the
// source tile is the one the write loop has already looked up. The cost is one
// extra `readPixel` per output texel -- a half-to-float decode, no hash lookup
// -- against a plane that at sigma = 32 over a 1024x1024 request would have
// been 20.5 MiB.
//
// The blurred plane stays in **float** until the combine, and that is a
// precision decision rather than a convenience. Routing it through
// `blurTiles()` instead -- the obvious reuse -- would round the blurred term
// to half before the subtraction, and `src - blur(src)` is a difference of two
// nearly equal numbers, so that rounding lands undivided in a result that is
// often smaller than itself. Measured on a sigma = 6 highpass over 131 072
// channel values: **58 753 of them change**, worst gap 4.50e-04, which is
// 1.84x the f16 store's own 2.441e-04. Keeping the plane in float costs one
// duplicated gather loop (ops/Filters.cpp says so at the top, and says where
// it should live when a third caller wants it) and keeps the storage format
// the only source of error, which is ops/Blur.hpp's standard for this phase.
//
// Add noise and offset gather nothing at all.
namespace np {

// The largest finite half. `local contrast` and `add noise` clamp here rather
// than at 1.0, so HDR headroom survives and only unrepresentable values are
// refused -- see the clamp discussion above. Spelled as a literal because
// core/Half exposes the conversion, not the bound.
inline constexpr float kFilterMaxLinear = 65504.0f;

// ==========================================================================
// 1. Highpass -- PLAN.md's own formulation, `src - blur(src)`
// ==========================================================================

// The same dilation the blur declares. Highpass reads exactly what its blur
// reads and not one texel more: the source term is the output texel itself,
// which is already inside the blur's apron.
RoiOp highpassRoiOp(const BlurParams& blur) noexcept;

// `dst = src - blur(src)` over `outRect`, on all four channels.
//
// Read the premultiplied-alpha section above before using the result: this is
// a **signed difference field**, not an image, and its alpha can be negative.
//
// Refusals match `blurTiles()` exactly -- null `dst`, `dst == &src`, an
// invalid blur, an empty rectangle -- and for the same reasons. An identity
// blur (zero radius) makes this the zero field, which is correct and is what
// "a filter at its neutral setting does nothing" means for a *difference*.
bool highpassTiles(const TileStore& src, const PixelRect& outRect, const BlurParams& blur,
                   TileStore* dst);

// ==========================================================================
// 2. Unsharp mask -- amount, radius, threshold
// ==========================================================================
//
// `dst = src + amount * gain * (src - blur(src))`, all four channels, one
// `gain` per texel.
//
// **Radius is `blur.sigma`** (or a box radius), so the radius control is
// ops/Blur's and there is no second radius-to-sigma convention in the codebase
// -- ops/Feather already had to invent one for selections and says why. A
// caller wanting the traditional "radius" dial converts once, at the dialog.
//
// **Amount is a plain multiplier on the highpass.** 0 is the identity, 1 adds
// the whole difference back, and there is no upper limit because there is no
// non-arbitrary one; the visible ceiling is where the overshoot clips against
// something, and this file does not clip RGB.
//
// **Threshold is in the SHAPER domain, and it is SOFT.** Two decisions:
//
//   *Shaper domain*, for the same reason add noise is: a threshold in linear
//   light means "0.01 of light", which is a huge tonal step in the shadows and
//   nothing at all in the highlights -- the 234.6x spread measured above. In
//   the shaper domain a threshold is a constant ratio: 0.02 means "ignore
//   differences smaller than 27.5% of the local level", at every level. The
//   gate compares the **straight** (un-premultiplied) colours' shaper-domain
//   difference, because that is where the tonal question lives; alpha's own
//   difference joins the same comparison undivided, on the argument that
//   coverage is already a bounded, roughly uniformly-visible [0,1] quantity
//   and so shares the threshold's units without needing a curve. That is an
//   approximation -- how visible a coverage change is depends on the backdrop
//   -- and it is taken so that a threshold on a flat-coloured cut-out still
//   sharpens the cut-out's edge rather than doing nothing at all.
//
//   *Soft*, i.e. `gain = max(|d| - t, 0) / |d|` rather than `gain = |d| > t`.
//   A hard gate is discontinuous exactly where the image crosses the
//   threshold, so a smooth ramp through it acquires a step. Measured, as the
//   jump in linear light at the moment the gate flips, at amount 1.0:
//
//       threshold    at linear 0.05    at linear 0.5     at linear 2.0
//         0.005      3.13e-03  (13)    3.13e-02 (128)    1.25e-01  (513)
//         0.020      1.37e-02  (56)    1.37e-01 (563)    5.50e-01 (2252)
//
//   with the f16 store's own step in brackets. 563 storage steps of
//   discontinuity, tracing a contour through the picture wherever the local
//   contrast happens to equal the number in the dialog. The soft form is the
//   same formula with the step removed and costs nothing.
//
//   What the gate is *for*, measured on the fixture it exists for -- Gaussian
//   grain of 0.004 shaper-domain standard deviation on two plateaus separated
//   by a 0.1142 shaper-domain step, sharpened at amount 2.0, sigma 2:
//
//       threshold    grain amplified by    step overshoot
//         0.000          0.059937             0.452148
//         0.010          0.019165             0.338013
//         0.020          0.000000             0.284424
//
//   At five standard deviations of the grain the gate removes it entirely --
//   not "mostly", exactly, because the soft ramp's numerator is clamped at
//   zero -- and the step keeps 63% of its overshoot. That is the trade the
//   control exists to offer.
//
//   `threshold = 0` (the default) skips the gate entirely -- no
//   un-premultiply, no `shaperEncode` -- so the two transcendentals per colour
//   channel are paid only by callers who asked for them.
//
// **One `gain` for all four channels**, taken as the largest of the three
// colour differences and the alpha difference. Per-channel gains would break
// the constant-colour identity in the section above -- a texel whose red gate
// opened and whose green gate did not would have its hue changed by the
// sharpen -- which is exactly the failure the shared coefficient exists to
// prevent.
struct UnsharpParams {
  // The radius control. Gaussian sigma, or a box radius; anything ops/Blur
  // accepts, including the identity.
  BlurParams blur;

  // Multiplier on the highpass. 0 is the identity.
  float amount = 1.0f;

  // Shaper-domain magnitude below which a difference is ignored, ramped in
  // softly. 0 disables the gate.
  float threshold = 0.0f;
};

// False for a request no filter can be built from: an invalid blur, a
// non-finite or negative amount, a non-finite or negative threshold. Refused
// by name rather than clamped, the same discipline `blurParamsValid()` takes
// -- a negative amount arriving here is a caller bug and treating it as an
// unsharpen hides it.
bool unsharpParamsValid(const UnsharpParams& p) noexcept;

// The blur's own dilation. The threshold gate reads only the output texel and
// the blurred value already gathered, so it widens nothing.
RoiOp unsharpRoiOp(const UnsharpParams& p) noexcept;

bool unsharpMaskTiles(const TileStore& src, const PixelRect& outRect, const UnsharpParams& p,
                      TileStore* dst);

// ==========================================================================
// 3. Sharpen -- the one-click filter, which is a preset and not an operator
// ==========================================================================
//
// A separate entry point because it is a separate *menu item* -- no radius,
// no threshold, one strength -- and not because it is separate arithmetic. It
// is `unsharpMaskTiles()` with the radius fixed.
//
// **Rejected: the classic 3x3 Laplacian**, `[[0,-1,0],[-1,5,-1],[0,-1,0]]`,
// which is what most implementations mean by "Sharpen". It is not separable,
// so it would be a second convolution path in this phase -- and a second path
// is a second apron, a second gather, and a second chance to reintroduce the
// tile seam that ops/Blur.hpp's whole design prevents. Nothing about the
// result justifies that: the 3x3 kernel is itself `I + 4*(I - mean4)`, an
// unsharp against a 4-neighbour mean, so the difference is a choice of blur.
//
// **Why sigma 1.0.** The number that decides it is the 2-D kernel's centre
// tap, which is how much of an isolated single-texel spike the highpass keeps:
//
//     sigma    1-D centre    2-D centre    highpass keeps
//      0.5      0.682690      0.466066         53.4 %
//      1.0      0.382928      0.146633         85.3 %
//      2.0      0.197417      0.038973         96.1 %
//
// At sigma 0.5 nearly half the kernel's mass is the texel's own value, so
// `src - blur(src)` is dominated by the texel rather than by its
// neighbourhood: the filter amplifies single-texel sensor noise almost as
// hard as it amplifies a real edge, and `amount` means something different
// than it does at any radius a user would type. Sigma 1.0 is the first row
// where the centre tap is a minority of the kernel, and it costs an apron of
// `ceil(4*sigma) = 4` texels, which a one-click filter can pay. Sigma 2.0
// would be a further 4 texels of apron to move the number from 85% to 96%,
// and would start visibly softening the thing it was asked to sharpen.
inline constexpr float kSharpenSigma = 1.0f;

// The unsharp request `sharpenTiles()` runs. Exposed so a UI preview, an ROI
// query and --selftest all use the same parameters the operator does rather
// than a retyped copy of them.
UnsharpParams sharpenParams(float strength) noexcept;

// `strength` is the unsharp amount: 0 is the identity, 1 is the usual full
// strength. Non-finite or negative is refused, like every other amount here.
bool sharpenTiles(const TileStore& src, const PixelRect& outRect, float strength,
                  TileStore* dst);

// ==========================================================================
// 4. Offset with wrap
// ==========================================================================
//
// PLAN.md phase 6, "offset with wrap". Moves the picture by whole texels; the
// wrap mode is what makes it the tile-a-texture tool rather than a slow way to
// translate a layer.
//
// **Wrapping needs a modulus, so it needs a rectangle.** "Wrap around" is
// modular arithmetic and modular arithmetic is undefined without a period, so
// `OffsetParams` carries the rectangle the wrap is taken over -- the canvas,
// normally. There is no way to spell this that does not require the caller to
// say what the document is; `TileStore` is sparse and unbounded and genuinely
// does not know, and inferring the modulus from the occupied tiles would make
// the filter's result change when a stroke happens to reach a new tile.
//
// **The modulus is Euclidean, not C's `%`.** `-1 % 128` is `-1` in C and the
// wrap needs `127`. Content dragged past the origin lives at negative document
// coordinates (ops/Roi.hpp makes the same point about `roiTileRange` flooring
// rather than truncating), so the truncating remainder is not a corner case
// here, it is the ordinary case for the left and top edges.
//
// **What this does to ROI, stated rather than discovered.** Offset in
// `Transparent` mode is a pure translation and `offsetRoiOp()` returns exactly
// that -- and it is, incidentally, the first asymmetric op in the build, the
// one ops/Roi.hpp predicted would finally make `roiBackward` and `roiForward`
// return different rectangles.
//
// In `Wrap` mode the backward map is **not expressible as a `RoiOp` at all**:
// a translated rectangle that crosses the wrap boundary comes from up to four
// disjoint source rectangles, and no dilate-and-translate describes that. So
// `offsetRoiOp()` is documented as the `Transparent` answer, and
// `offsetSourceRect()` is the function to ask instead -- it returns the
// translated rectangle when the wrap does not bite, and the whole wrap
// rectangle when it does. Conservative in the one direction ops/Roi.hpp says
// is safe: "an ROI that is too large is merely slow, one that is too small
// silently produces wrong pixels".
//
// **Rejected: a `RepeatEdge` mode.** The third option every offset dialog
// offers. Under premultiplied alpha the edge row of a document is usually
// transparent, so repeating it repeats nothing and the mode is indistinguish-
// able from `Transparent`; where the edge row is *not* transparent it smears
// one row of texels across the whole vacated band, which reads as a rendering
// fault rather than as a choice. Neither outcome is worth a mode.
enum class OffsetEdge {
  // Source coordinates are reduced modulo `wrapRect`. The picture is a torus.
  Wrap,
  // Source coordinates are taken as they fall. Texels the source does not
  // hold read as transparent black, which needs no special case because an
  // absent tile already is one.
  Transparent,
};

struct OffsetParams {
  // The picture moves by `(+dx, +dy)`: a source texel at `s` appears at
  // `s + (dx, dy)`. Same sign convention as `RoiOp::dx`, which is the one an
  // Offset dialog's own numbers use.
  int32_t dx = 0;
  int32_t dy = 0;

  OffsetEdge edge = OffsetEdge::Wrap;

  // The wrap period. Required (and must be non-empty) for `Wrap`; ignored
  // entirely for `Transparent`.
  PixelRect wrapRect{};
};

bool offsetParamsValid(const OffsetParams& p) noexcept;

// The `Transparent` mode's ROI, `roiOffsetOp(dx, dy)`. **Not valid for
// `Wrap`** -- see above and use `offsetSourceRect()`.
RoiOp offsetRoiOp(const OffsetParams& p) noexcept;

// The source rectangle an evaluator must fetch to produce `outRect`, correct
// for both modes. Exact for `Transparent` and for a `Wrap` whose translated
// rectangle stays inside the wrap period; the whole `wrapRect` otherwise.
PixelRect offsetSourceRect(const OffsetParams& p, const PixelRect& outRect) noexcept;

// The single source texel an output texel reads. Exposed because it is the
// entire semantic content of the op -- the Euclidean modulus in particular --
// and a test that retyped it would be checking its own copy.
PixelCoord offsetSourceTexel(const OffsetParams& p, PixelCoord out) noexcept;

bool offsetTiles(const TileStore& src, const PixelRect& outRect, const OffsetParams& p,
                 TileStore* dst);

// ==========================================================================
// 5. Add noise -- in the shaper domain, from a counter-based PRNG
// ==========================================================================
//
// **The PRNG holds no state, and that is not a style preference.** See the
// invariant section at the top: a stream PRNG makes a texel's value depend on
// how many texels were drawn before it, so the same document region rendered
// as one request and as two gets different grain across the split. That is
// unfixable by any apron, it is invisible in a single-tile test, and it is
// exactly the class of bug ops/Blur.hpp's seam property exists to catch.
//
// So the generator is a **hash of (seed, x, y, stream)**, with `x` and `y` in
// document texels. Same seed, same coordinate, same value, in any order, from
// any thread, split any way. The mixer is splitmix64's finalizer applied once
// per component in sequence, rather than to a linear combination of the
// components -- a combination lets `x` and `y` trade off against each other
// and produce visible diagonal structure in the grain.
//
// The cost of that decision, named: three finalizers per draw and two draws
// per Gaussian sample, against one multiply-and-shift for a stream generator.
// It buys the seam property and reproducibility, and it is still far cheaper
// than the `shaperEncode`/`shaperDecode` pair in the same loop.
//
// **The noise is anchored to the DOCUMENT grid**, because that is what the
// coordinates are. Re-running the op over any rectangle reproduces the same
// grain -- the point of the exercise -- but a layer translated *after* the
// noise was applied carries its grain with it, while a *live* noise op in an
// op stack would have the grain stay put as the layer moves beneath it. That
// is the honest consequence of a coordinate-hashed generator and it is the
// same one every procedural texture has.
//
// **Amount is a shaper-domain amplitude**: the standard deviation for
// `Gaussian`, the half-width for `Uniform`. From the table at the top of this
// header, an amount of 0.02 is a x1.2749 (+27.5%) swing in linear light at
// every level above the ACEScct knee. Measured through this op itself, an
// amount of 0.05 uniform applied to a ten-stop ramp, as the worst perturbation
// relative to the level:
//
//     linear level   1.69e-03   8.67e-03   4.46e-02   2.29e-01   6.83e-01
//     |delta|/level    2.8144     0.8151     0.8342     0.8316     0.8212
//
// 0.83 at every level for six stops -- and 0.8353 is what `2^(0.05*17.52) - 1`
// says it should be, so the operator is doing exactly the arithmetic the
// domain promises. The first column is the ACEScct toe, below 2^-7, where the
// curve is a straight line and the grain reverts to fixed amplitude.
//
// Alpha is **never** given noise --
// color/Shaper.hpp's standing rule that alpha is opacity, not light, and no
// transfer function is ever applied to it. Noise is added to the *straight*
// colour and re-premultiplied, so the perturbation scales with coverage on its
// own and a fully transparent texel is left exactly as it was; the op cannot
// grow a layer's support.
enum class NoiseDistribution {
  // Values uniform on `[-amount, +amount]`, drawn at the centres of 2^24
  // bins so the distribution is symmetric about zero rather than biased half
  // a bin low. (The topmost bin's centre rounds to exactly 1.0 in float, so
  // `+amount` is attainable and `-amount` is not; the asymmetry is one part
  // in 2^24 and is named here only so nobody has to rediscover it.)
  Uniform,
  // Box-Muller from two uniforms, standard deviation `amount`. The tail is
  // bounded by the generator's 24-bit mantissa: `|z| <= sqrt(2*ln(2^25)) =
  // 5.887` standard deviations, so a single texel cannot draw an arbitrarily
  // large value even though a true normal could.
  Gaussian,
};

struct NoiseParams {
  // Shaper-domain amplitude. 0 is the identity.
  float amount = 0.0f;

  NoiseDistribution distribution = NoiseDistribution::Gaussian;

  // One draw shared by R, G and B rather than three independent ones --
  // luminance grain instead of colour speckle.
  bool monochrome = false;

  // **The whole reproducibility contract.** Same seed, same result, exactly,
  // for the same document coordinates. There is no default-random seed and no
  // clock: a filter whose output cannot be reproduced cannot be tested, and a
  // caller who wants a different grain passes a different number.
  uint64_t seed = 0;
};

bool noiseParamsValid(const NoiseParams& p) noexcept;

// Add noise is a point op: it reads exactly the texel it writes.
constexpr RoiOp noiseRoiOp() noexcept { return RoiOp{}; }

// The primitive: a uniform draw on `(0, 1)`, at the centre of one of 2^24
// bins, as a pure function of the seed, the document coordinate and the
// stream index. Streams 0..3 are the four channels' first draw and 4..7 their
// second (Box-Muller's `u2`).
//
// Exposed so --selftest can build the reference grain from the same source the
// operator uses instead of from a retyped copy of the mixer.
float filterRandomUniform(uint64_t seed, int32_t x, int32_t y, int32_t stream) noexcept;

// The shaper-domain offset added to colour channel `channel` (0..2) at a
// document coordinate. Exposed for the same reason, and because it is the one
// place the distribution and the `monochrome` flag are interpreted.
float filterNoiseOffset(const NoiseParams& p, int32_t x, int32_t y, int32_t channel) noexcept;

bool addNoiseTiles(const TileStore& src, const PixelRect& outRect, const NoiseParams& p,
                   TileStore* dst);

// ==========================================================================
// 6. Local contrast
// ==========================================================================
//
// PLAN.md lists it beside shadows/highlights, and docs/operations.md §1.3
// already ruled it out of ops/PointOps: "clarity/local contrast" is class B
// because it needs a neighbourhood. This is that neighbourhood pass.
//
// The shape is an unsharp mask and the domain is not, which is the entire
// content of the op:
//
//     blur in LINEAR light          (an average of light is an average of
//                                    light; ops/Blur.hpp's argument, unchanged)
//     take the difference and add it back in the SHAPER domain
//
//        s = shaperEncode(straight src colour)
//        b = shaperEncode(straight blurred colour)
//        out = shaperDecode(s + amount * (s - b)) * srcAlpha
//
// **Why the difference moves domains halfway through.** Contrast is a ratio,
// not a difference: "make this region 20% brighter than its surroundings" is
// multiplication in linear light, and adding a fixed linear difference instead
// is the same 234.6x spread measured at the top of this header, in a different
// costume. Adding in the log domain *is* multiplying in linear, so the boost
// is an exact, level-independent identity:
//
//     the local ratio is raised to the power (1 + amount), everywhere.
//
// Measured on detail of the same *absolute* size sitting on two plateaus 100x
// apart (linear 0.02 and 2.0), at amount 1.0: the shadow's local ratio goes
// 1.49952 -> 2.24777 and the highlight's 1.00391 -> 1.00784, which are
// exponents of 1.9991 and 2.0000 -- the same boost, from the same number, at
// both ends. The same radius as a linear-light unsharp gives 2.0895 and
// 2.0000, i.e. it does 4.5% more work in the shadows than in the highlights on
// this fixture and progressively more as the detail grows.
//
// **What it buys concretely, and what it does not.** It does not stop a strong
// setting from producing very bright values -- the log domain's gain is
// unbounded upward, and at amount 6.0 on a 0.05/0.60 step this op reaches
// linear 14.6 where a linear unsharp reaches 2.09. What it does is make the
// operator **incapable of producing negative light**, which a linear unsharp
// does immediately: on that same step, at amount 1.0, a linear unsharp
// undershoots to **-0.198** -- light that is not light, a premultiplied texel
// that cannot be un-premultiplied, and a visible inversion at the dark side of
// every edge -- while this op's worst undershoot is +0.0084, and at amount 3.0
// it is exactly 0. A multiplicative operator has 0 as an asymptote; an
// additive one walks straight past it.
//
// **Alpha is untouched, and this is the difference from unsharp.** Unsharp
// sharpens the shape -- it shares its coefficient with alpha on purpose, so a
// soft edge gets crisper. Local contrast is a *tonal* op: it changes how light
// is distributed within the shape and must not move the shape's boundary by
// so much as a texel. So `outA = srcA`, and the un-premultiply/re-premultiply
// is by that same unchanged alpha, which is what keeps the result a valid
// premultiplied texel with no clamp on the coverage needed at all.
//
// **What it requires of its input**, since it divides by alpha and takes a
// logarithm: a *valid* premultiplied image -- `0 <= A <= 1`, RGB the light
// actually present. Texels with `A <= 0` are copied through untouched (there
// is no colour there to grade). `highpassTiles()`'s signed output is not a
// valid image and must not be fed here; the header section above says so from
// the other side.
//
// **Radius.** The caller's, via `blur`, and it is the parameter that decides
// what "local" means: a sigma of a few texels sharpens, a sigma of a few
// percent of the image's short side is the clarity slider. There is no default
// because there is no default without a document size, and inventing one here
// would put a layout decision inside an operator.
struct LocalContrastParams {
  BlurParams blur;

  // Log-domain boost on the local difference. 0 is the identity; negative
  // values are legal and *reduce* local contrast, which is the flatten-a-
  // harsh-photograph direction and is why `amount` is not validated as
  // non-negative here the way unsharp's is.
  float amount = 0.0f;
};

bool localContrastParamsValid(const LocalContrastParams& p) noexcept;

RoiOp localContrastRoiOp(const LocalContrastParams& p) noexcept;

bool localContrastTiles(const TileStore& src, const PixelRect& outRect,
                        const LocalContrastParams& p, TileStore* dst);

// ==========================================================================
// 7. Emboss -- a two-tap directional relief, blended against the source
// ==========================================================================
//
// A stylize filter, not a corrective one -- Photoshop's own menu puts it
// under "Stylize", not "Sharpen", and that distinction is the reason its
// arithmetic looks nothing like sections 1-6: there is no blur, no apron
// dilated by a sigma, and no `ops/Blur` call anywhere in this section.
//
// **The op, in one line.** `dst = lerp(src, relief * srcA, amount)` where
// `relief = 0.5 + depth * (lum(ahead) - lum(behind))`, `ahead = src(x+dx,
// y+dy)`, `behind = src(x-dx, y-dy)`, and `lum()` is a Rec. 709 luma weight
// (0.2126, 0.7152, 0.0722) applied to the PREMULTIPLIED RGB the store
// already holds -- no un-premultiply anywhere in this filter, which is the
// decision that makes it simple rather than merely short.
//
// **Why luminance of the PREMULTIPLIED value, not the straight one.** Every
// other filter in this file that leaves linear light does so because it
// needs a *ratio* (local contrast's log-domain multiply) or a *bounded
// visible unit* (noise's, unsharp's shaper-domain amplitude) -- neither
// applies here. Emboss wants "how much light this texel contributes", and a
// premultiplied texel's own three channels already answer that: a texel at
// alpha 0 contributes luminance exactly 0 without any special case, which is
// the same "an absent tile needs no special case" property `ops/Blur.hpp`
// relies on for its own gather. Un-premultiplying first would need a
// zero-alpha guard for no benefit -- the relief this filter draws at a
// layer's own soft edge (the alpha boundary reads as a ridge) is correct
// output, not a bug to route around, because embossing a shape's silhouette
// along with its paint is what every emboss filter with an alpha channel
// does.
//
// **This is a linear combination of premultiplied samples, so it inherits
// the seam property exactly the way `blurPlane` does, and for the same
// reason.** `lum()` is a fixed-weight sum of three premultiplied channels
// (no different, arithmetically, from one output channel of a blur's
// weighted average), and `relief` and the final `lerp` are pure functions of
// two such sums taken at two DOCUMENT-relative offsets from the output
// texel. Nothing here depends on where the request rectangle's own edges
// fall, which is the whole content of the property. See section 8 below for
// the filter in this trio where that is not true for free.
//
// **The apron is symmetric per axis and, in general, not square** --
// `left = right = |dx|`, `up = down = |dy|` -- the same "per-axis, not
// uniform" shape section 9's motion blur needs for a continuous angle, just
// reached here by two fixed integer taps instead of a swept line. `dx = dy =
// 0` is a legal, deliberately not-refused request: both taps read the
// output texel itself, the gradient is identically zero at every texel, and
// `relief` is the flat card `0.5` everywhere -- a real, testable answer
// ("no direction was given, so there is no ridge"), not a special case to
// avoid.
//
// **Where the clamp is and is not.** `depth * gradient` can push `relief`
// outside `[0, 1]` (a strong depth on a sharp edge is the point: an
// exaggerated ridge is what "depth" means), so `relief * srcA` can exceed
// `srcA` or go negative -- the same HDR-overshoot-or-negative-light shape
// section 2's unsharp already accepts in RGB and section 6's local contrast
// already refuses via its own clamp. Emboss follows local contrast's choice,
// not unsharp's: the output channel is clamped to `[0, kFilterMaxLinear]`
// with `clampStorable()`, because an unbounded ridge height is not a
// property a user dialing "depth" is asking to preserve the way unsharp's
// overshoot is the visible signature of sharpening -- it is just a large
// number that would otherwise poison every later blur's apron the way
// section 6's own clamp section argues. Alpha is untouched -- `outA = srcA`
// always, exactly section 6's "a tonal/stylize op must not move the shape's
// boundary by a texel" argument, restated for a different op.
//
// **`amount` is the strength control, and `amount = 0` is the identity** --
// the one degenerate-parameter case this filter shares with every other op
// in the file: 0 skips the two extra reads and the luminance arithmetic
// entirely and returns `src` bit for bit, the same short-circuit
// `addNoiseTiles()`'s `amount` and `localContrastTiles()`'s take. `depth = 0`
// or `dx = dy = 0` (at `amount > 0`) are NOT the identity -- they are the
// flat mid-grey card described above, a well-defined and deterministic
// answer, not a no-op, and --selftest checks it as exactly that rather than
// mistaking it for a second identity case.
struct EmbossParams {
  // The compare offset, in document texels. `ahead` and `behind` are read at
  // `+(dx,dy)` and `-(dx,dy)` from the output texel, so the apron this op
  // declares is `|dx|` left/right and `|dy|` up/down regardless of sign.
  int32_t dx = 1;
  int32_t dy = -1;

  // Gain on the luminance gradient before it is folded into `relief`.
  // Negative is legal (it flips which side of the ridge is lit, exactly
  // equivalent to negating both `dx` and `dy`) for the same reason local
  // contrast's `amount` may be negative: there is no non-arbitrary sign
  // restriction on a gain.
  float depth = 1.0f;

  // Blend against the source: 0 is the identity, 1 is the full grey relief.
  // No upper limit, matching unsharp's "there is no non-arbitrary one" --
  // amount above 1 extrapolates past the relief card rather than clamping
  // to it.
  float amount = 1.0f;
};

// False for a request no filter can be built from: a non-finite or negative
// `amount`, a non-finite `depth`. `dx`/`dy` have no invalid values -- every
// integer pair, including `(0, 0)`, is a legal (if degenerate) direction.
bool embossParamsValid(const EmbossParams& p) noexcept;

// The two-tap window: `RoiOp{|dx|, |dx|, |dy|, |dy|, 0, 0}`. No translation
// -- the output texel stays exactly where the input was, this op only reads
// a window around it.
RoiOp embossRoiOp(const EmbossParams& p) noexcept;

bool embossTiles(const TileStore& src, const PixelRect& outRect, const EmbossParams& p,
                 TileStore* dst);

// ==========================================================================
// 8. Median / despeckle -- a rank filter, not a convolution, and what it
//    costs to keep the seam property without one
// ==========================================================================
//
// `ops/Roi.hpp`'s own header names this filter before it existed ("median
// and dust-and-scratches are symmetric dilations of their window"), because
// ROI-wise a rank filter's reach is exactly a blur's: a `(2r+1)x(2r+1)`
// window centred on the output texel, declared the identical way blur
// declares its apron -- `roiDilateOp(radius)`, uniform on all four sides.
//
// **So why does the task brief call this filter out as "does NOT inherit the
// invariant the same way"?** Not because the ROI shape is any different --
// it is `roiDilateOp`, unchanged -- but because the STANDARD FAST algorithm
// for a windowed median is not safe to reach for here, and reaching for it
// out of habit is exactly how this filter would silently reintroduce
// `blurTiles()`'s original bug in rank-statistic form. The efficient way to
// slide a median window across a row is a running histogram (or a running
// balanced-partition structure) that is incrementally updated one column at
// a time and is bootstrapped once at the row's own left edge. Applied naively
// to "the requested rectangle" as the row's whole world, that bootstrap runs
// at `outRect`'s own left edge rather than at the true window's, which is
// precisely `blurTiles()`'s tile-clamped-at-its-own-edge failure mode,
// wearing a different algorithm's clothes. **This file does not implement
// that algorithm.** `medianTiles()` gathers the true, un-clamped
// `roiBackward(medianRoiOp(p), outRect)` window from SOURCE tiles -- the
// identical discipline `gatherBlurredPlane()` uses -- and recomputes the full
// window's rank from scratch at every output texel via `std::nth_element`.
// That is genuinely more expensive than a rolling algorithm (the Performance
// section below has the number), and it is the price section 8 pays that
// sections 1-7 do not: the invariant here is EARNED by refusing an
// optimisation, not inherited for free by construction.
//
// **Colour and coverage are two independent rank problems, not one.** A
// literal per-channel median of the four PREMULTIPLIED channels would let
// each channel's answer come from a different source texel, and there is no
// guarantee those four texels' `RGB/A` ratios agree -- the median could
// synthesise a straight colour no sample in the window ever held. So this
// filter un-premultiplies first: it takes the median of `A` over every
// texel in the window (coverage is meaningful even where it is 0, so every
// sample counts), and, independently, the median of each straight colour
// channel over only the texels whose `A > 0` in the window (a fully
// transparent sample has no colour to contribute a vote to -- "nothing there
// is nothing to grade", section 5's rule for add noise, restated for a rank
// filter). The straight median is then re-premultiplied by the ALPHA
// median, so the result is `(straightMedian * alphaMedian, alphaMedian)` --
// always a valid premultiplied texel by construction, never an
// arithmetically-impossible one. A window whose every sample is fully
// transparent has no straight-colour vote at all and returns `(0,0,0,0)`,
// matching local contrast's identical convention for a texel with no light
// to grade.
//
// **`radius = 0` is the identity, exactly**: a `1x1` window's "median" is
// its own single sample, so `medianTiles()` short-circuits it to a bit-exact
// copy rather than paying the un-premultiply/rank/re-premultiply round trip
// for an answer that is provably its own input.
struct MedianParams {
  // Window half-width in document texels; the window is `(2*radius+1)^2`.
  // 0 is the identity.
  int32_t radius = 0;
};

// False only for a negative radius -- the one request no window can be
// built from.
bool medianParamsValid(const MedianParams& p) noexcept;

// `roiDilateOp(radius)` -- the blur-shaped declaration ops/Roi.hpp's own
// header anticipated for this filter.
RoiOp medianRoiOp(const MedianParams& p) noexcept;

bool medianTiles(const TileStore& src, const PixelRect& outRect, const MedianParams& p,
                 TileStore* dst);

// ==========================================================================
// 9. Motion blur -- a rotated-line convolution, and the first apron in this
//    file whose two axes genuinely differ
// ==========================================================================
//
// Every apron declared by sections 1-8 is either a uniform `roiDilateOp` (a
// blur-shaped op, or median's blur-shaped window) or a small symmetric
// window whose margins happen not to be square (emboss's `|dx|`/`|dy|`, but
// those are two fixed integers a caller chose, not a formula this file
// derives). Motion blur is the first one where the LEFT/RIGHT margin and the
// UP/DOWN margin are related by a continuous parameter that is not under the
// caller's direct control -- the angle -- and getting that relation wrong is
// exactly the "anisotropic apron" hazard the task brief names: an apron
// computed as if the kernel reached equally in every direction would clip
// the long axis of a near-horizontal or near-vertical smear and reproduce
// the tile-seam bug along whichever edge the clip fell inside.
//
// **The kernel.** A uniform (box) average of `2*radius+1` taps spaced one
// texel apart along the unit direction vector `(cos(angle), sin(angle))`,
// centred on the output texel: tap `t` (for `t` in `[-radius, radius]`)
// samples the PREMULTIPLIED plane at `(x + t*cos, y + t*sin)`. At `angle =
// 0` every tap lands exactly on an integer texel and this degenerates to a
// 1-D horizontal box average -- ops/Blur.hpp's own box kernel, minus its
// vertical pass -- which is what --selftest cross-checks the general path
// against rather than trusting the bilinear machinery on its own telling.
//
// **Off-grid taps are bilinear, and that is the one place this filter adds
// error the rest of the file does not have.** For a general angle, `t*sin`
// and `t*cos` are not integers, so each tap reads the four surrounding
// texels and blends by fractional position -- ordinary bilinear
// interpolation of the PREMULTIPLIED plane, which is safe for the identical
// reason averaging premultiplied RGBA is always safe in this file: a
// fully-transparent neighbour contributes `(0,0,0,0)`, the correct implicit
// content of a tile that is not there, with no division anywhere in the
// sampling.
//
// **The apron, worked out rather than guessed.** A tap at signed offset `t`
// reaches `t*cos` texels in x and `t*sin` texels in y; the farthest tap is
// `radius`, so the farthest REACH is `radius*|cos|` in x and `radius*|sin|`
// in y -- but a bilinear sample at a non-integer position also touches the
// texel one further out (the ceiling of its own fractional position), so
// the declared margin adds one more texel of safety on each axis:
//
//     left = right = ceil(radius * |cos(angle)|) + 1
//     up   = down  = ceil(radius * |sin(angle)|) + 1
//
// Both margins are needed on BOTH sides of each axis, not just the side the
// angle points toward, because the kernel is symmetric about the output
// texel (`t` ranges over BOTH signs) -- unlike `OffsetParams`, this is not a
// translation and `roiOffsetOp`'s asymmetric dx/dy has no part in it. At
// `angle = 0` the y-margin collapses to `ceil(0) + 1 = 1` even though no tap
// ever leaves row `y` -- one texel of apron paid for a bilinear safety net
// the axis-aligned case does not actually need, which --selftest's
// exact-agreement-with-a-1-D-box check absorbs for free (the extra row
// contributes nothing because it is multiplied by a zero vertical weight,
// same argument ops/Blur.hpp's seam section makes for its own oversized taps
// past the true kernel edge). **This is the "not free" case named at the top
// of this file**: unlike a fixed-radius blur, the margin here is a function
// the filter has to derive correctly for every angle, not a constant it can
// read off `blurApron()`.
//
// **`radius = 0` is the exact identity** -- `motionBlurRoiOp()` returns
// `RoiOp{}` (no apron at all, at any angle) and `motionBlurTiles()`
// short-circuits to a bit-exact copy, matching every other filter's zero-
// strength convention in this file and avoiding a pointless bilinear
// self-sample (`t = 0` samples exactly the output texel's own position, so
// the general path would already be exact here too -- the short circuit is
// a cost saving, not a correctness fix).
//
// Clamped to `[0, kFilterMaxLinear]` with `clampStorable()`, for the
// identical reason section 7's emboss is: a box average of finite
// premultiplied samples cannot itself produce a value outside the range its
// inputs already occupy (averaging cannot overshoot, unlike unsharp's
// difference-based `amount`), so the clamp here only ever fires on a NaN or
// an already-out-of-range input, and exists so this filter cannot propagate
// one rather than because its own arithmetic manufactures one.
struct MotionBlurParams {
  // Direction of the smear, radians, measured from +x toward +y (this
  // codebase's document-space convention: y increases downward, matching
  // `core/Tile.hpp`'s own top-left origin). Any finite value is legal --
  // the kernel is symmetric about the output texel, so `angle` and `angle +
  // pi` declare the identical filter.
  float angleRadians = 0.0f;

  // Half-length of the smear in document texels; the kernel is
  // `2*radius+1` taps. 0 is the identity.
  int32_t radius = 0;
};

// False for a negative radius or a non-finite angle.
bool motionBlurParamsValid(const MotionBlurParams& p) noexcept;

// `RoiOp{}` (the exact identity) at `radius == 0`; otherwise the per-axis
// margins worked out above, symmetric on each axis, no translation.
RoiOp motionBlurRoiOp(const MotionBlurParams& p) noexcept;

bool motionBlurTiles(const TileStore& src, const PixelRect& outRect, const MotionBlurParams& p,
                     TileStore* dst);

}  // namespace np
